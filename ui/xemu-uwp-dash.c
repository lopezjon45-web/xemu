/*
 * Copyright (c) 2026 Skitzelles - part of xemu for Xbox Dev Mode (github.com/skitzellez/xemu-uwp). GPL-2.0-or-later.
 *
 * xemu UWP port: install the Xbox dashboard from the user's own game disc.
 * See xemu-uwp-dash.h. This file has no dependency on QEMU so that it can be
 * built and tested on its own (XemuUWP/tools/dash/test_dash.c).
 *
 * Layout of the installer, worked out from a retail disc:
 *
 * dashupdate.xbe has two sections, DASHUCTL (the file list) and DASHUDAT
 * (the data). DASHUCTL starts with a 40 byte header (u32 at 8: version, u32
 * at 28: offset of the second part) followed by entries:
 *   00/04/02  file for xboxdashdata.<version>\ / fonts\ / xboxdash.xbe:
 *             u32 size, name\0 (no name for 02), then for every 32 KB of the
 *             file a u16 compressed size and a 20 byte hash, then 00 00
 *   03        end of the first part
 *   80 name\0 enter a folder (the second part's root is xodash\)
 *   81        file, same layout as above
 *   82        leave the folder
 *   83 name\0 an alternative XBE header (for development consoles), skipped
 *   84        end
 * DASHUDAT holds one LZX stream per file (window 2^20, 32 KB frames, the
 * frames' compressed sizes are the u16 values), in list order. The streams
 * are decoded with libmspack by presenting each as a one-file cabinet.
 *
 * Copyright (c) 2026 Skitzelles
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifdef XEMU_UWP
/* libmspack is built with a 64 bit off_t; its structures do not match otherwise */
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <mspack.h>
#include "xemu-uwp-dash.h"

#define SECTOR 2048
#define FRAME 32768
#define MAX_FILES 2048
#define MAX_PATH_LEN 200

static uint16_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr16(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static void no_log(const char *fmt, ...) { (void)fmt; }

/* ---- SHA-1, for the section digests inside the XBE files ---- */

typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t fill; } Sha1;

static uint32_t rol(uint32_t v, int n) { return v << n | v >> (32 - n); }

static void sha1_block(Sha1 *s, const uint8_t *p)
{
    uint32_t w[80], a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[i * 4] << 24 | p[i * 4 + 1] << 16 | p[i * 4 + 2] << 8 | p[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void sha1_init(Sha1 *s)
{
    static const uint32_t iv[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    memcpy(s->h, iv, sizeof(iv));
    s->len = 0;
    s->fill = 0;
}

static void sha1_add(Sha1 *s, const uint8_t *p, size_t n)
{
    s->len += n;
    while (n) {
        size_t k = 64 - s->fill < n ? 64 - s->fill : n;
        memcpy(s->buf + s->fill, p, k);
        s->fill += k; p += k; n -= k;
        if (s->fill == 64) {
            sha1_block(s, s->buf);
            s->fill = 0;
        }
    }
}

static void sha1_end(Sha1 *s, uint8_t out[20])
{
    uint64_t bits = s->len * 8;
    uint8_t pad[72] = { 0x80 };
    size_t n = (s->fill < 56 ? 56 : 120) - s->fill;
    for (int i = 0; i < 8; i++) {
        pad[n + i] = bits >> (56 - 8 * i);
    }
    sha1_add(s, pad, n + 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = s->h[i] >> 24; out[i * 4 + 1] = s->h[i] >> 16;
        out[i * 4 + 2] = s->h[i] >> 8; out[i * 4 + 3] = s->h[i];
    }
}

/* Every section of an XBE carries the SHA-1 of (u32 size, data) in the header. */
static int xbe_sections_ok(const uint8_t *d, size_t len, uint32_t *version)
{
    if (len < 0x180 || memcmp(d, "XBEH", 4)) {
        return 0;
    }
    uint32_t base = rd32(d + 0x104), cert = rd32(d + 0x118) - base;
    uint32_t nsec = rd32(d + 0x11C), sec = rd32(d + 0x120) - base;
    if (cert + 0xB0 > len || nsec > 64 || sec + (uint64_t)nsec * 56 > len) {
        return 0;
    }
    if (version) {
        *version = rd32(d + cert + 0xAC);
    }
    for (uint32_t i = 0; i < nsec; i++) {
        const uint8_t *h = d + sec + i * 56;
        uint32_t raw = rd32(h + 12), size = rd32(h + 16);
        uint8_t szb[4], dg[20];
        Sha1 s;
        if ((uint64_t)raw + size > len) {
            return 0;
        }
        wr32(szb, size);
        sha1_init(&s);
        sha1_add(&s, szb, 4);
        sha1_add(&s, d + raw, size);
        sha1_end(&s, dg);
        if (memcmp(dg, h + 36, 20)) {
            return 0;
        }
    }
    return 1;
}

/* ---- the disc: find dashupdate.xbe and its two sections ---- */

typedef struct {
    uint64_t ctl_off, dat_off;
    uint32_t ctl_size, dat_size;
} Installer;

static int find_installer(const UwpDashIo *disc, Installer *out)
{
    static const uint64_t bases[] = { 0, 0x18300000, 0x0FD90000, 0x02080000 };
    uint8_t vd[32];
    uint64_t base = 0;
    int found = 0;
    for (int i = 0; i < 4 && !found; i++) {
        if (!disc->read(disc->opaque, bases[i] + 32 * SECTOR, vd, sizeof(vd)) &&
            !memcmp(vd, "MICROSOFT*XBOX*MEDIA", 20)) {
            base = bases[i];
            found = 1;
        }
    }
    if (!found) {
        return 0;
    }
    uint32_t root_sector = rd32(vd + 20), root_size = rd32(vd + 24);
    if (!root_size || root_size > 1 << 20) {
        return 0;
    }
    uint8_t *dir = malloc(root_size);
    uint64_t xbe = 0;
    if (dir && !disc->read(disc->opaque, base + (uint64_t)root_sector * SECTOR, dir, root_size)) {
        for (uint32_t o = 0; o + 14 <= root_size;) {
            if (rd16(dir + o) == 0xFFFF) {         /* padding up to the next sector */
                o = (o / SECTOR + 1) * SECTOR;
                continue;
            }
            uint8_t nl = dir[o + 13];
            if (nl == 14 && o + 14 + nl <= root_size) {
                char name[15];
                for (int i = 0; i < 14; i++) {
                    name[i] = tolower(dir[o + 14 + i]);
                }
                name[14] = 0;
                if (!strcmp(name, "dashupdate.xbe")) {
                    xbe = base + (uint64_t)rd32(dir + o + 4) * SECTOR;
                    break;
                }
            }
            o += (14 + nl + 3) & ~3u;
        }
    }
    free(dir);
    if (!xbe) {
        return 0;
    }

    uint8_t h[0x180];
    if (disc->read(disc->opaque, xbe, h, sizeof(h)) || memcmp(h, "XBEH", 4)) {
        return 0;
    }
    uint32_t xbase = rd32(h + 0x104), hsize = rd32(h + 0x108);
    uint32_t nsec = rd32(h + 0x11C), sec = rd32(h + 0x120) - xbase;
    if (hsize < 0x180 || hsize > 0x10000 || nsec > 64 || sec + nsec * 56 > hsize) {
        return 0;
    }
    uint8_t *hdr = malloc(hsize + 1);
    memset(out, 0, sizeof(*out));
    if (hdr && !disc->read(disc->opaque, xbe, hdr, hsize)) {
        hdr[hsize] = 0;
        for (uint32_t i = 0; i < nsec; i++) {
            const uint8_t *s = hdr + sec + i * 56;
            uint32_t name = rd32(s + 20) - xbase;
            if (name >= hsize) {
                continue;
            }
            if (!strcmp((char *)hdr + name, "DASHUCTL")) {
                out->ctl_off = xbe + rd32(s + 12);
                out->ctl_size = rd32(s + 16);
            } else if (!strcmp((char *)hdr + name, "DASHUDAT")) {
                out->dat_off = xbe + rd32(s + 12);
                out->dat_size = rd32(s + 16);
            }
        }
    }
    free(hdr);
    return out->ctl_size >= 48 && out->ctl_size <= 8 << 20 && out->dat_size;
}

/* ---- the file list ---- */

typedef struct {
    char path[MAX_PATH_LEN];
    uint32_t size, blocks;
    const uint8_t *recs;    /* 22 bytes per block, inside the list */
    uint64_t data_off;      /* inside DASHUDAT */
    uint32_t data_size;
    uint8_t *data;          /* unpacked */
} DashFile;

static int parse_list(const uint8_t *c, uint32_t len, DashFile *files, int *count,
                      uint32_t *version, uint32_t dat_size)
{
    char folders[8][64];
    int depth = 0, n = 0;
    uint32_t o = 40;
    uint64_t pos = 0;
    *version = rd32(c + 8);
    while (o < len) {
        uint8_t t = c[o++];
        if (t == 0 || t == 4 || t == 2 || t == 0x81) {
            if (o + 4 > len || n == MAX_FILES) {
                return 0;
            }
            DashFile *f = &files[n];
            f->size = rd32(c + o);
            o += 4;
            const char *name = "";
            if (t != 2) {
                name = (const char *)c + o;
                size_t nl = strnlen(name, len - o);
                if (o + nl >= len || nl == 0 || nl > 42 || strchr(name, '\\') || strchr(name, '/')) {
                    return 0;
                }
                o += nl + 1;
            }
            int k;
            if (t == 0) {
                k = snprintf(f->path, sizeof(f->path), "xboxdashdata.%08x\\%s", *version, name);
            } else if (t == 4) {
                k = snprintf(f->path, sizeof(f->path), "fonts\\%s", name);
            } else if (t == 2) {
                k = snprintf(f->path, sizeof(f->path), "xboxdash.xbe");
            } else {
                k = snprintf(f->path, sizeof(f->path), "xodash");
                for (int i = 0; i < depth && k < (int)sizeof(f->path); i++) {
                    k += snprintf(f->path + k, sizeof(f->path) - k, "\\%s", folders[i]);
                }
                if (k < (int)sizeof(f->path)) {
                    k += snprintf(f->path + k, sizeof(f->path) - k, "\\%s", name);
                }
            }
            if (k >= (int)sizeof(f->path)) {
                return 0;
            }
            f->blocks = (f->size + FRAME - 1) / FRAME;
            if (o + (uint64_t)f->blocks * 22 + 2 > len) {
                return 0;
            }
            f->recs = c + o;
            f->data_off = pos;
            f->data_size = 0;
            for (uint32_t i = 0; i < f->blocks; i++) {
                f->data_size += rd16(c + o + i * 22);
            }
            pos += f->data_size;
            o += f->blocks * 22;
            if (c[o] || c[o + 1]) {
                return 0;
            }
            o += 2;
            f->data = NULL;
            n++;
        } else if (t == 3) {
            /* end of the first part */
        } else if (t == 0x80) {
            const char *name = (const char *)c + o;
            size_t nl = strnlen(name, len - o);
            if (depth == 8 || o + nl >= len || nl == 0 || nl > 42 || strchr(name, '\\') ||
                strchr(name, '/') || !strcmp(name, "..") || !strcmp(name, ".")) {
                return 0;
            }
            strcpy(folders[depth++], name);
            o += nl + 1;
        } else if (t == 0x82) {
            if (depth) {
                depth--;
            }
        } else if (t == 0x83) {
            size_t nl = strnlen((const char *)c + o, len - o);
            o += nl + 1;
            if (o + 0x10C > len) {
                return 0;
            }
            o += rd32(c + o + 0x108);
        } else if (t == 0x84) {
            break;
        } else {
            return 0;
        }
    }
    *count = n;
    return n > 0 && pos == dat_size;
}

/* ---- LZX through libmspack: each stream is presented as a one-file cabinet ---- */

typedef struct { uint8_t *data; size_t size, pos; } MemFile;

static struct mspack_file *mem_open(struct mspack_system *self, const char *name, int mode)
{
    (void)self; (void)mode;
    MemFile *m = (MemFile *)name;
    m->pos = 0;
    return (struct mspack_file *)m;
}
static void mem_close(struct mspack_file *f) { (void)f; }
static int mem_read(struct mspack_file *f, void *buf, int bytes)
{
    MemFile *m = (MemFile *)f;
    size_t n = m->size - m->pos < (size_t)bytes ? m->size - m->pos : (size_t)bytes;
    memcpy(buf, m->data + m->pos, n);
    m->pos += n;
    return (int)n;
}
static int mem_write(struct mspack_file *f, void *buf, int bytes)
{
    MemFile *m = (MemFile *)f;
    if (m->size - m->pos < (size_t)bytes) {
        return -1;
    }
    memcpy(m->data + m->pos, buf, bytes);
    m->pos += bytes;
    return bytes;
}
static int mem_seek(struct mspack_file *f, off_t off, int mode)
{
    MemFile *m = (MemFile *)f;
    off_t p = mode == MSPACK_SYS_SEEK_START ? off :
              mode == MSPACK_SYS_SEEK_CUR ? (off_t)m->pos + off : (off_t)m->size + off;
    if (p < 0 || (size_t)p > m->size) {
        return -1;
    }
    m->pos = p;
    return 0;
}
static off_t mem_tell(struct mspack_file *f) { return ((MemFile *)f)->pos; }
static void mem_msg(struct mspack_file *f, const char *fmt, ...) { (void)f; (void)fmt; }
static void *mem_alloc(struct mspack_system *self, size_t bytes) { (void)self; return malloc(bytes); }
static void mem_free(void *p) { free(p); }
static void mem_copy(void *src, void *dst, size_t bytes) { memcpy(dst, src, bytes); }

static struct mspack_system mem_system = {
    mem_open, mem_close, mem_read, mem_write, mem_seek, mem_tell, mem_msg,
    mem_alloc, mem_free, mem_copy, NULL
};

/* comp: the file's compressed frames one after the other. Returns malloc'd data or NULL. */
static uint8_t *unpack(const DashFile *f, const uint8_t *comp)
{
    uint8_t *out = malloc(f->size ? f->size : 1);
    if (!out || !f->size) {
        return out;
    }
    const uint32_t off_files = 36 + 8, off_data = off_files + 16 + 2;
    size_t cab_size = off_data + f->data_size + (size_t)f->blocks * 8;
    uint8_t *cab = calloc(1, cab_size), *p = cab;
    if (!cab) {
        free(out);
        return NULL;
    }
    memcpy(p, "MSCF", 4); wr32(p + 8, cab_size); wr32(p + 16, off_files);
    p[24] = 3; p[25] = 1; wr16(p + 26, 1); wr16(p + 28, 1); wr16(p + 32, 0x1234);
    p += 36;
    wr32(p, off_data); wr16(p + 4, f->blocks); wr16(p + 6, 3 | 20 << 8);  /* LZX, window 2^20 */
    p += 8;
    wr32(p, f->size); wr16(p + 10, 0x3021); wr16(p + 14, 0x20); p[16] = 'f';
    p += 18;
    uint32_t left = f->size;
    for (uint32_t i = 0; i < f->blocks; i++) {
        uint32_t k = rd16(f->recs + i * 22), u = left < FRAME ? left : FRAME;
        wr16(p + 4, k); wr16(p + 6, u);
        memcpy(p + 8, comp, k);
        p += 8 + k; comp += k; left -= u;
    }

    MemFile in = { cab, cab_size, 0 }, dst = { out, f->size, 0 };
    if (getenv("UWP_DASH_DUMP")) {
        FILE *df = fopen(getenv("UWP_DASH_DUMP"), "wb");
        fwrite(cab, 1, cab_size, df);
        fclose(df);
    }
    int ok = 0;
    int selftest = MSPACK_ERR_SEEK;
    MSPACK_SYS_SELFTEST(selftest);
    struct mscab_decompressor *cabd =
        selftest == MSPACK_ERR_OK ? mspack_create_cab_decompressor(&mem_system) : NULL;
    if (cabd) {
        struct mscabd_cabinet *c = cabd->open(cabd, (const char *)&in);
        if (!c && getenv("UWP_DASH_DEBUG")) {
            fprintf(stderr, "open: error %d\n", cabd->last_error(cabd));
        }
        if (c) {
            int err = c->files ? cabd->extract(cabd, c->files, (const char *)&dst) : -1;
            ok = err == MSPACK_ERR_OK && dst.pos == f->size;
            if (!ok && getenv("UWP_DASH_DEBUG")) {
                fprintf(stderr, "extract: error %d, wrote %u of %u\n", err, (unsigned)dst.pos, f->size);
            }
            cabd->close(cabd, c);
        }
        mspack_destroy_cab_decompressor(cabd);
    }
    free(cab);
    if (!ok) {
        free(out);
        return NULL;
    }
    return out;
}

/* ---- the hard disk: FATX on partition 2 (C:) ---- */

#define C_BASE 0x8CA80000ull
#define C_SIZE 0x1F400000ull

typedef struct {
    const UwpDashIo *io;
    uint32_t cs, n, w, maxc, hint, end;
    uint64_t fat_off, data;
    uint32_t fat_size;
    uint8_t *fat, *cl;      /* cl: one cluster of scratch space */
    int failed;
} Vol;

static int vol_open(Vol *v, const UwpDashIo *io)
{
    uint8_t h[16];
    memset(v, 0, sizeof(*v));
    v->io = io;
    if (io->read(io->opaque, C_BASE, h, sizeof(h)) || memcmp(h, "FATX", 4)) {
        return 0;
    }
    v->cs = rd32(h + 8) * 512;
    if (v->cs < 4096 || v->cs > 1 << 20 || (v->cs & (v->cs - 1))) {
        return 0;
    }
    v->n = C_SIZE / v->cs + 1;
    v->w = v->n < 0xFFF0 ? 2 : 4;
    v->end = v->w == 2 ? 0xFFFF : 0xFFFFFFFF;
    v->fat_size = (v->n * v->w + 4095) & ~4095u;
    v->fat_off = C_BASE + 4096;
    v->data = v->fat_off + v->fat_size;
    v->maxc = (C_SIZE - 4096 - v->fat_size) / v->cs;
    v->hint = 2;
    v->fat = malloc(v->fat_size);
    v->cl = malloc(v->cs);
    if (!v->fat || !v->cl || io->read(io->opaque, v->fat_off, v->fat, v->fat_size)) {
        free(v->fat);
        free(v->cl);
        return 0;
    }
    return 1;
}

static void vol_close(Vol *v) { free(v->fat); free(v->cl); }
static uint32_t fat_get(Vol *v, uint32_t c) { return v->w == 2 ? rd16(v->fat + c * 2) : rd32(v->fat + c * 4); }
static void fat_set(Vol *v, uint32_t c, uint32_t x)
{
    if (v->w == 2) { wr16(v->fat + c * 2, x); } else { wr32(v->fat + c * 4, x); }
}
static int fat_valid(Vol *v, uint32_t c) { return c >= 1 && c <= v->maxc; }
static uint64_t cl_off(Vol *v, uint32_t c) { return v->data + (uint64_t)(c - 1) * v->cs; }
static void fat_flush(Vol *v)
{
    if (v->io->write(v->io->opaque, v->fat_off, v->fat, v->fat_size)) {
        v->failed = 1;
    }
}

static uint32_t fat_alloc(Vol *v, uint32_t count)
{
    uint32_t first = 0, prev = 0, c = v->hint;
    while (count) {
        if (c > v->maxc) {
            v->failed = 1;
            return first;
        }
        if (fat_get(v, c) == 0) {
            if (prev) { fat_set(v, prev, c); } else { first = c; }
            fat_set(v, c, v->end);
            prev = c;
            count--;
        }
        c++;
    }
    v->hint = c;
    return first;
}

static void fat_free(Vol *v, uint32_t c)
{
    for (uint32_t guard = 0; fat_valid(v, c) && guard < v->n; guard++) {
        uint32_t next = fat_get(v, c);
        fat_set(v, c, 0);
        c = next;
    }
    v->hint = 2;
}

/*
 * Looks for a name in a directory. Returns 1 and the entry if found; else 0,
 * with *slot_cl and *slot_off set to a free slot (or *slot_cl = 0 if the
 * directory is full; *last is then its last cluster).
 */
static int dir_find(Vol *v, uint32_t dir, const char *name, uint8_t entry[64],
                    uint32_t *slot_cl, uint32_t *slot_off, uint32_t *last)
{
    size_t nl = strlen(name);
    int have_slot = 0;
    *slot_cl = 0;
    for (uint32_t c = dir, guard = 0; fat_valid(v, c) && guard < v->n; c = fat_get(v, c), guard++) {
        *last = c;
        if (v->io->read(v->io->opaque, cl_off(v, c), v->cl, v->cs)) {
            v->failed = 1;
            return 0;
        }
        for (uint32_t o = 0; o < v->cs; o += 64) {
            uint8_t l = v->cl[o];
            if (l == 0 || l == 0xFF) {            /* end of the directory */
                if (!have_slot) { *slot_cl = c; *slot_off = o; }
                return 0;
            }
            if (l == 0xE5 || l > 42) {
                if (!have_slot) { *slot_cl = c; *slot_off = o; have_slot = 1; }
                continue;
            }
            if (l == nl && !strncasecmp((char *)v->cl + o + 2, name, nl)) {
                memcpy(entry, v->cl + o, 64);
                *slot_cl = c;
                *slot_off = o;
                return 1;
            }
        }
    }
    return 0;
}

static void dir_put(Vol *v, uint32_t dir, const char *name, uint8_t attr, uint32_t first, uint32_t size)
{
    uint8_t e[64], old[64];
    uint32_t cl, off = 0, last = dir;
    size_t nl = strlen(name);
    memset(e, 0xFF, sizeof(e));
    e[0] = nl; e[1] = attr;
    memcpy(e + 2, name, nl);
    wr32(e + 44, first); wr32(e + 48, size);
    for (int i = 0; i < 3; i++) {
        wr16(e + 52 + i * 4, 0x6000);       /* 12:00:00 */
        wr16(e + 54 + i * 4, 0x4A2D);       /* a fixed date */
    }
    dir_find(v, dir, name, old, &cl, &off, &last);
    if (!cl) {                               /* full: add a cluster of free entries */
        cl = fat_alloc(v, 1);
        if (!cl) {
            return;
        }
        fat_set(v, last, cl);
        memset(v->cl, 0xFF, v->cs);
        if (v->io->write(v->io->opaque, cl_off(v, cl), v->cl, v->cs)) {
            v->failed = 1;
        }
        fat_flush(v);
        off = 0;
    }
    if (v->io->write(v->io->opaque, cl_off(v, cl) + off, e, 64)) {
        v->failed = 1;
    }
}

static uint32_t dir_make(Vol *v, uint32_t dir, const char *name)
{
    uint8_t e[64];
    uint32_t cl, off, last;
    if (dir_find(v, dir, name, e, &cl, &off, &last)) {
        return (e[1] & 0x10) ? rd32(e + 44) : 0;
    }
    uint32_t c = fat_alloc(v, 1);
    if (!c) {
        return 0;
    }
    memset(v->cl, 0xFF, v->cs);
    if (v->io->write(v->io->opaque, cl_off(v, c), v->cl, v->cs)) {
        v->failed = 1;
    }
    fat_flush(v);
    dir_put(v, dir, name, 0x10, c, 0);
    return c;
}

/* path uses backslashes; folders are made as needed */
static void file_put(Vol *v, const char *path, const uint8_t *data, uint32_t size)
{
    char part[MAX_PATH_LEN];
    uint32_t dir = 1;
    const char *p = path, *sep;
    while ((sep = strchr(p, '\\'))) {
        size_t l = sep - p;
        if (l == 0 || l > 42) { v->failed = 1; return; }
        memcpy(part, p, l);
        part[l] = 0;
        dir = dir_make(v, dir, part);
        if (!dir) { v->failed = 1; return; }
        p = sep + 1;
    }
    if (!*p || strlen(p) > 42) { v->failed = 1; return; }

    uint8_t e[64];
    uint32_t cl, off, last, old_first = 0;
    int had = dir_find(v, dir, p, e, &cl, &off, &last);
    if (had) {
        if (e[1] & 0x10) { v->failed = 1; return; }
        old_first = rd32(e + 44);
    }
    uint32_t count = (size + v->cs - 1) / v->cs, first = count ? fat_alloc(v, count) : 0;
    if (v->failed) {
        return;
    }
    uint32_t c = first;
    for (uint32_t i = 0; i < count; i++, c = fat_get(v, c)) {
        uint32_t k = size - i * v->cs < v->cs ? size - i * v->cs : v->cs;
        const uint8_t *src = data + (uint64_t)i * v->cs;
        if (k < v->cs) {
            memset(v->cl, 0, v->cs);
            memcpy(v->cl, src, k);
            src = v->cl;
        }
        if (v->io->write(v->io->opaque, cl_off(v, c), src, v->cs)) {
            v->failed = 1;
            return;
        }
    }
    fat_flush(v);                    /* the new chain is on disk before anything points at it */
    dir_put(v, dir, p, 0x20, first, size);
    if (had && old_first) {
        fat_free(v, old_first);
        fat_flush(v);
    }
}

/* ---- public ---- */

int uwp_dash_installed(const UwpDashIo *hdd, uint32_t *version)
{
    Vol v;
    uint8_t e[64];
    uint32_t cl, off, last;
    int found = 0;
    if (!vol_open(&v, hdd)) {
        return -1;
    }
    if (dir_find(&v, 1, "xodash", e, &cl, &off, &last) && (e[1] & 0x10) &&
        dir_find(&v, rd32(e + 44), "xonlinedash.xbe", e, &cl, &off, &last) &&
        rd32(e + 48) > 0x400 && fat_valid(&v, rd32(e + 44))) {
        uint8_t h[0x400];
        if (!hdd->read(hdd->opaque, cl_off(&v, rd32(e + 44)), h, sizeof(h)) && !memcmp(h, "XBEH", 4)) {
            uint32_t cert = rd32(h + 0x118) - rd32(h + 0x104);
            found = 1;
            if (version) {
                *version = cert + 0xB0 <= sizeof(h) ? rd32(h + cert + 0xAC) : 0;
            }
        }
    }
    vol_close(&v);
    return found;
}

static int load_list(const UwpDashIo *disc, Installer *ins, uint8_t **ctl, DashFile **files,
                     int *count, uint32_t *version)
{
    if (!find_installer(disc, ins)) {
        return 0;
    }
    *ctl = malloc(ins->ctl_size + 2);
    *files = calloc(MAX_FILES, sizeof(DashFile));
    if (*ctl && *files && !disc->read(disc->opaque, ins->ctl_off, *ctl, ins->ctl_size)) {
        (*ctl)[ins->ctl_size] = (*ctl)[ins->ctl_size + 1] = 0;
        if (parse_list(*ctl, ins->ctl_size, *files, count, version, ins->dat_size)) {
            return 1;
        }
    }
    free(*ctl);
    free(*files);
    *ctl = NULL;
    *files = NULL;
    return 0;
}

int uwp_dash_on_disc(const UwpDashIo *disc, uint32_t *version)
{
    Installer ins;
    uint8_t *ctl;
    DashFile *files;
    int count;
    uint32_t ver = 0;
    if (!load_list(disc, &ins, &ctl, &files, &count, &ver)) {
        return 0;
    }
    free(ctl);
    free(files);
    if (version) {
        *version = ver;
    }
    return 1;
}

int uwp_dash_install(const UwpDashIo *disc, const UwpDashIo *hdd, UwpDashLog log)
{
    Installer ins;
    uint8_t *ctl = NULL;
    DashFile *files = NULL;
    int count = 0, result = -1, online = -1;
    uint32_t version = 0;
    if (!log) {
        log = no_log;
    }
    if (!load_list(disc, &ins, &ctl, &files, &count, &version)) {
        log("dashboard: this disc has no installer that I can read");
        return -1;
    }

    /* 1: unpack and check everything in memory */
    uint64_t total = 0;
    int have_dash = 0;
    for (int i = 0; i < count; i++) {
        DashFile *f = &files[i];
        uint8_t *comp = malloc(f->data_size ? f->data_size : 1);
        if (!comp || disc->read(disc->opaque, ins.dat_off + f->data_off, comp, f->data_size)) {
            free(comp);
            log("dashboard: cannot read the data of %s", f->path);
            goto out;
        }
        f->data = unpack(f, comp);
        free(comp);
        if (!f->data) {
            log("dashboard: %s does not unpack", f->path);
            goto out;
        }
        size_t pl = strlen(f->path);
        if (pl > 4 && !strcasecmp(f->path + pl - 4, ".xbe")) {
            uint32_t v = 0;
            if (!xbe_sections_ok(f->data, f->size, &v)) {
                log("dashboard: %s fails its own checksums", f->path);
                goto out;
            }
            if (!strcasecmp(f->path, "xboxdash.xbe")) {
                have_dash = 1;
            } else if (!strcasecmp(f->path, "xodash\\xonlinedash.xbe")) {
                online = i;
            }
        }
        total += f->size;
    }
    if (!have_dash || online < 0) {
        log("dashboard: the installer lacks the dashboard or its Xbox Live part");
        goto out;
    }

    /* 2: write. The Xbox Live part goes last: it is what marks the install as complete. */
    Vol v;
    if (!vol_open(&v, hdd)) {
        log("dashboard: the hard disk has no Xbox C: partition");
        goto out;
    }
    for (int pass = 0; pass < 2 && !v.failed; pass++) {
        for (int i = 0; i < count && !v.failed; i++) {
            if ((i == online) == (pass == 1)) {
                file_put(&v, files[i].path, files[i].data, files[i].size);
            }
        }
    }
    if (v.failed) {
        log("dashboard: writing to the hard disk failed (full or read-only?)");
    } else {
        log("dashboard: version %08x installed from the disc, %d files, %llu MB", version, count,
            (unsigned long long)(total >> 20));
        result = count;
    }
    vol_close(&v);

out:
    for (int i = 0; i < count; i++) {
        free(files[i].data);
    }
    free(files);
    free(ctl);
    return result;
}
#endif /* XEMU_UWP */
