/*
 * Geforce NV2A PGRAPH OpenGL Renderer
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2024 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "hw/xbox/nv2a/nv2a_regs.h"
#include <hw/xbox/nv2a/nv2a_int.h>
#include "debug.h"
#include "renderer.h"
#include "qemu/xemu-uwp.h"

static void update_memory_buffer(NV2AState *d, hwaddr addr, hwaddr size,
                                 bool quick)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    glBindBuffer(GL_ARRAY_BUFFER, r->gl_memory_buffer);

    hwaddr end = TARGET_PAGE_ALIGN(addr + size);
    addr &= TARGET_PAGE_MASK;
    assert(end < memory_region_size(d->vram));

    static hwaddr last_addr, last_end;
    if (quick && (addr >= last_addr) && (end <= last_end)) {
        return;
    }
    last_addr = addr;
    last_end = end;

    size = end - addr;
    if (memory_region_test_and_clear_dirty(d->vram, addr, size,
                                           DIRTY_MEMORY_NV2A)) {
        glBufferSubData(GL_ARRAY_BUFFER, addr, size,
                        d->vram_ptr + addr);
        nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_1);
    }
}

#ifdef XEMU_UWP
/*
 * Vertex data on GL-on-D3D12. Upstream keeps one GL buffer that mirrors all
 * of VRAM and patches the changed pages with glBufferSubData before a draw.
 * That buffer is always referenced by draws that have not run yet, and Mesa's
 * D3D12 driver answers a write to a busy buffer by flushing and waiting for
 * the GPU: about 170 waits a frame in Halo 2, a third of the renderer
 * thread's time inside the kernel.
 *
 * Here the pages a draw needs are copied to the next free part of a stream
 * buffer instead, through an unsynchronized mapping: nothing that is queued
 * can refer to that part, so nothing has to be waited for. When the buffer
 * is full it is replaced (the old storage lives on until its draws are done).
 * A small table remembers which VRAM ranges already sit in the current buffer
 * so that unchanged vertex buffers are not copied again.
 */
#define UWP_STREAM_SIZE (64 * 1024 * 1024)
#define UWP_STREAM_RANGES 256

typedef struct UwpStreamRange {
    hwaddr addr, end;
    GLintptr off;
    unsigned int gen; /* 0 = unused or stale */
} UwpStreamRange;

static UwpStreamRange uwp_stream_ranges[UWP_STREAM_RANGES];
/* VRAM ranges whose dirty state has already been asked for during the current draw */
static hwaddr uwp_draw_checked[16][2];
static int uwp_draw_checked_count;
static unsigned int uwp_stream_next, uwp_stream_gen = 1;
static GLintptr uwp_stream_pos;
static GLuint uwp_stream_buffer;

/*
 * When the stream buffer is full, the next of a few buffers is taken instead
 * of asking GL for fresh storage (glBufferData). Fresh storage every time
 * looked free, but on Mesa's D3D12 driver the old 64 MB allocations were not
 * given back while a scene kept drawing: an online lobby went from 3.7 GB to
 * the app's 5.1 GB allowance in under a minute and the emulator closed. A
 * buffer is used again only after the GPU has finished the draws that read
 * from it (a fence is put behind them when the buffer is left).
 */
#define UWP_STREAM_POOL 3
static GLuint uwp_stream_pool[UWP_STREAM_POOL];
static GLsync uwp_stream_fence[UWP_STREAM_POOL];
static int uwp_stream_cur;

/* Leaves the buffer bound to GL_ARRAY_BUFFER. */
static void uwp_stream_next_buffer(void)
{
    if (uwp_stream_fence[uwp_stream_cur]) {
        glDeleteSync(uwp_stream_fence[uwp_stream_cur]);
    }
    uwp_stream_fence[uwp_stream_cur] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    uwp_stream_cur = (uwp_stream_cur + 1) % UWP_STREAM_POOL;
    if (uwp_stream_fence[uwp_stream_cur]) {
        glClientWaitSync(uwp_stream_fence[uwp_stream_cur], GL_SYNC_FLUSH_COMMANDS_BIT,
                         2000000000ull);
        glDeleteSync(uwp_stream_fence[uwp_stream_cur]);
        uwp_stream_fence[uwp_stream_cur] = NULL;
    }
    if (!uwp_stream_pool[uwp_stream_cur]) {
        glGenBuffers(1, &uwp_stream_pool[uwp_stream_cur]);
        glBindBuffer(GL_ARRAY_BUFFER, uwp_stream_pool[uwp_stream_cur]);
        glBufferData(GL_ARRAY_BUFFER, UWP_STREAM_SIZE, NULL, GL_STREAM_DRAW);
    }
    uwp_stream_buffer = uwp_stream_pool[uwp_stream_cur];
    glBindBuffer(GL_ARRAY_BUFFER, uwp_stream_buffer);
    uwp_stream_gen++;
    xemu_uwp_counters[UWP_CNT_STREAM_WRAP]++;
}

/*
 * Makes VRAM [addr, end) (page aligned) available in the stream buffer and
 * returns where; the offset is at least min_off, so that a pointer to an
 * attribute's element 0, which may lie before addr, does not go negative.
 */
static GLintptr uwp_stream_vertices(NV2AState *d, hwaddr addr, hwaddr end,
                                    hwaddr min_off)
{
    hwaddr size = end - addr;
    bool checked = false;

    for (int i = 0; i < uwp_draw_checked_count; i++) {
        if (uwp_draw_checked[i][0] <= addr && end <= uwp_draw_checked[i][1]) {
            checked = true;
            break;
        }
    }
    if (size == 0) {
        /* a draw of no vertices that starts on a page boundary: nothing to copy */
        return MAX(uwp_stream_pos, (GLintptr)TARGET_PAGE_ALIGN(min_off)) % UWP_STREAM_SIZE;
    }
    if (!checked && uwp_draw_checked_count < ARRAY_SIZE(uwp_draw_checked)) {
        uwp_draw_checked[uwp_draw_checked_count][0] = addr;
        uwp_draw_checked[uwp_draw_checked_count][1] = end;
        uwp_draw_checked_count++;
    }
    if (!checked &&
        memory_region_test_and_clear_dirty(d->vram, addr, size,
                                           DIRTY_MEMORY_NV2A)) {
        /* the guest wrote here: every copy that overlaps is out of date */
        for (int i = 0; i < UWP_STREAM_RANGES; i++) {
            UwpStreamRange *r = &uwp_stream_ranges[i];
            if (r->gen == uwp_stream_gen && r->addr < end && addr < r->end) {
                r->gen = 0;
            }
        }
    } else {
        for (int i = 0; i < UWP_STREAM_RANGES; i++) {
            UwpStreamRange *r = &uwp_stream_ranges[i];
            if (r->gen == uwp_stream_gen && r->addr <= addr && end <= r->end &&
                r->off + (GLintptr)(addr - r->addr) >= (GLintptr)min_off) {
                return r->off + (addr - r->addr);
            }
        }
    }

    GLintptr pos = MAX((GLintptr)TARGET_PAGE_ALIGN(uwp_stream_pos),
                       (GLintptr)TARGET_PAGE_ALIGN(min_off));
    if (pos + (GLintptr)size > UWP_STREAM_SIZE) {
        uwp_stream_next_buffer();
        pos = TARGET_PAGE_ALIGN(min_off);
    }
    void *map = glMapBufferRange(GL_ARRAY_BUFFER, pos, size,
                                 GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT |
                                 GL_MAP_UNSYNCHRONIZED_BIT);
    if (map) {
        memcpy(map, d->vram_ptr + addr, size);
        glUnmapBuffer(GL_ARRAY_BUFFER);
    } else {
        static int complaints;
        GLenum err = glGetError();
        if (complaints < 20) {
            complaints++;
            uwp_log("vertex stream: map failed, GL error 0x%x, pos %lld size %llu min_off %llu",
                    err, (long long)pos, (unsigned long long)size,
                    (unsigned long long)min_off);
        }
        while (glGetError() != GL_NO_ERROR) {
        }
        glBufferSubData(GL_ARRAY_BUFFER, pos, size, d->vram_ptr + addr);
    }
    nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_1);

    UwpStreamRange *r = &uwp_stream_ranges[uwp_stream_next];
    uwp_stream_next = (uwp_stream_next + 1) % UWP_STREAM_RANGES;
    r->addr = addr;
    r->end = end;
    r->off = pos;
    r->gen = uwp_stream_gen;
    uwp_stream_pos = pos + size;
    return pos;
}
#endif

#ifdef XEMU_UWP
/*
 * The other per-draw vertex data (inline arrays, inline buffers) goes through
 * the same stream. Upstream gives each its own GL buffer and respecifies it
 * with glBufferData for every draw; on D3D12 that is a fresh allocation in
 * the kernel each time (several hundred a frame in Halo 2). Leaves the stream
 * buffer bound to GL_ARRAY_BUFFER and returns the offset of the copy.
 */
static GLintptr uwp_inline_array_base;

GLintptr pgraph_gl_uwp_stream_put(const void *data, size_t size)
{
    glBindBuffer(GL_ARRAY_BUFFER, uwp_stream_buffer);
    GLintptr pos = ROUND_UP(uwp_stream_pos, 64);
    if (size == 0) {
        return pos % UWP_STREAM_SIZE;
    }
    if (pos + (GLintptr)size > UWP_STREAM_SIZE) {
        uwp_stream_next_buffer();
        pos = 0;
    }
    void *map = glMapBufferRange(GL_ARRAY_BUFFER, pos, size,
                                 GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT |
                                 GL_MAP_UNSYNCHRONIZED_BIT);
    if (map) {
        memcpy(map, data, size);
        glUnmapBuffer(GL_ARRAY_BUFFER);
    } else {
        while (glGetError() != GL_NO_ERROR) {
        }
        glBufferSubData(GL_ARRAY_BUFFER, pos, size, data);
    }
    uwp_stream_pos = pos + size;
    return pos;
}
#endif

void pgraph_gl_update_entire_memory_buffer(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

#ifdef XEMU_UWP
    /* all of VRAM may have changed (a state was loaded): forget what the stream buffer holds */
    uwp_stream_gen++;
    uwp_stream_pos = UWP_STREAM_SIZE;
#endif
    glBindBuffer(GL_ARRAY_BUFFER, r->gl_memory_buffer);
    glBufferSubData(GL_ARRAY_BUFFER, 0, memory_region_size(d->vram), d->vram_ptr);
}

void pgraph_gl_bind_vertex_attributes(NV2AState *d, unsigned int min_element,
                                   unsigned int max_element, bool inline_data,
                                   unsigned int inline_stride,
                                   unsigned int provoking_element)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    bool updated_memory_buffer = false;
#ifdef XEMU_UWP
    uwp_draw_checked_count = 0;
#endif
    unsigned int num_elements = max_element - min_element + 1;

    if (inline_data) {
        NV2A_GL_DGROUP_BEGIN("%s (num_elements: %d inline stride: %d)",
                             __func__, num_elements, inline_stride);
    } else {
        NV2A_GL_DGROUP_BEGIN("%s (num_elements: %d)", __func__, num_elements);
    }

    pg->compressed_attrs = 0;

    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];

        if (!attr->count) {
            glDisableVertexAttribArray(i);
            glVertexAttrib4fv(i, attr->inline_value);
            continue;
        }

        NV2A_DPRINTF("vertex data array format=%d, count=%d, stride=%d\n",
                     attr->format, attr->count, attr->stride);

        GLint gl_count = attr->count;
        GLenum gl_type;
        GLboolean gl_normalize;
        bool needs_conversion = false;

        switch (attr->format) {
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D:
            gl_type = GL_UNSIGNED_BYTE;
            gl_normalize = GL_TRUE;
            // http://www.opengl.org/registry/specs/ARB/vertex_array_bgra.txt
            gl_count = GL_BGRA;
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL:
            gl_type = GL_UNSIGNED_BYTE;
            gl_normalize = GL_TRUE;
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1:
            gl_type = GL_SHORT;
            gl_normalize = GL_TRUE;
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
            gl_type = GL_FLOAT;
            gl_normalize = GL_FALSE;
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K:
            gl_type = GL_SHORT;
            gl_normalize = GL_FALSE;
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP:
            /* 3 signed, normalized components packed in 32-bits. (11,11,10) */
            gl_type = GL_INT;
            assert(attr->count == 1);
            needs_conversion = true;
            break;
        default:
            fprintf(stderr, "Unknown vertex type: 0x%x\n", attr->format);
            assert(!"Unknown vertex type");
            break;
        }

        nv2a_profile_inc_counter(NV2A_PROF_ATTR_BIND);
        hwaddr attrib_data_addr;
        size_t stride;

        if (needs_conversion) {
            pg->compressed_attrs |= (1 << i);
        }

        hwaddr start = 0;
#ifdef XEMU_UWP
        GLintptr attrib_pointer = 0;
        bool uwp_streamed = false;
#endif
        if (inline_data) {
#ifdef XEMU_UWP
            glBindBuffer(GL_ARRAY_BUFFER, uwp_stream_buffer);
            attrib_pointer = uwp_inline_array_base + attr->inline_array_offset;
            uwp_streamed = true;
#else
            glBindBuffer(GL_ARRAY_BUFFER, r->gl_inline_array_buffer);
#endif
            attrib_data_addr = attr->inline_array_offset;
            stride = inline_stride;
        } else {
            hwaddr dma_len;
            uint8_t *attr_data = (uint8_t *)nv_dma_map(
                d, attr->dma_select ? pg->dma_vertex_b : pg->dma_vertex_a,
                &dma_len);
            assert(attr->offset < dma_len);
            attrib_data_addr = attr_data + attr->offset - d->vram_ptr;
            stride = attr->stride;
            start = attrib_data_addr + min_element * stride;
#ifdef XEMU_UWP
            {
                hwaddr lo = start & TARGET_PAGE_MASK;
                hwaddr hi = TARGET_PAGE_ALIGN(start + num_elements * stride);
                assert(hi < memory_region_size(d->vram));
                /* element 0 may lie before the first page that is needed */
                hwaddr before = attrib_data_addr < lo ? lo - attrib_data_addr : 0;
                if (before + (hi - lo) <= UWP_STREAM_SIZE / 4) {
                    glBindBuffer(GL_ARRAY_BUFFER, uwp_stream_buffer);
                    GLintptr off = uwp_stream_vertices(d, lo, hi, before);
                    attrib_pointer = off + (GLintptr)attrib_data_addr - (GLintptr)lo;
                    uwp_streamed = true;
                } else {
                    /* the mirror of all VRAM, patched in place (waits for the GPU) */
                    update_memory_buffer(d, start, num_elements * stride, false);
                }
            }
#else
            update_memory_buffer(d, start, num_elements * stride,
                                        updated_memory_buffer);
            updated_memory_buffer = true;
#endif
        }
#ifdef XEMU_UWP
        if (!uwp_streamed) {
            attrib_pointer = attrib_data_addr;
        }
#endif

        uint32_t provoking_element_index = provoking_element - min_element;
        size_t element_size = attr->size * attr->count;
        assert(element_size <= sizeof(attr->inline_value));
        const uint8_t *last_entry;

        if (inline_data) {
            last_entry = (uint8_t*)pg->inline_array + attr->inline_array_offset;
        } else {
            last_entry = d->vram_ptr + start;
        }
        if (!stride) {
            // Stride of 0 indicates that only the first element should be
            // used.
            pgraph_update_inline_value(attr, last_entry);
            glDisableVertexAttribArray(i);
            glVertexAttrib4fv(i, attr->inline_value);
            continue;
        }

#ifdef XEMU_UWP
#define UWP_ATTRIB_POINTER ((void *)(uintptr_t)attrib_pointer)
#else
#define UWP_ATTRIB_POINTER ((void *)attrib_data_addr)
#endif
        if (needs_conversion) {
            glVertexAttribIPointer(i, gl_count, gl_type, stride,
                                   UWP_ATTRIB_POINTER);
        } else {
            glVertexAttribPointer(i, gl_count, gl_type, gl_normalize, stride,
                                  UWP_ATTRIB_POINTER);
        }
#undef UWP_ATTRIB_POINTER

        glEnableVertexAttribArray(i);
        last_entry += stride * provoking_element_index;
        pgraph_update_inline_value(attr, last_entry);
    }

    NV2A_GL_DGROUP_END();
}

unsigned int pgraph_gl_bind_inline_array(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    unsigned int offset = 0;
    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];
        if (attr->count == 0) {
            continue;
        }

        /* FIXME: Double check */
        offset = ROUND_UP(offset, attr->size);
        attr->inline_array_offset = offset;
        NV2A_DPRINTF("bind inline attribute %d size=%d, count=%d\n",
            i, attr->size, attr->count);
        offset += attr->size * attr->count;
        offset = ROUND_UP(offset, attr->size);
    }

    unsigned int vertex_size = offset;
    unsigned int index_count = pg->inline_array_length*4 / vertex_size;

    NV2A_DPRINTF("draw inline array %d, %d\n", vertex_size, index_count);

    nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_2);
    GLsizeiptr buffer_size = index_count * vertex_size;
#ifdef XEMU_UWP
    uwp_inline_array_base = pgraph_gl_uwp_stream_put(pg->inline_array, buffer_size);
#else
    glBindBuffer(GL_ARRAY_BUFFER, r->gl_inline_array_buffer);
    glBufferData(GL_ARRAY_BUFFER, buffer_size, NULL, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, buffer_size, pg->inline_array);
#endif
    pgraph_gl_bind_vertex_attributes(d, 0, index_count-1, true, vertex_size,
                                  index_count-1);

    return index_count;
}

static void vertex_cache_entry_init(Lru *lru, LruNode *node, const void *key)
{
    VertexLruNode *vnode = container_of(node, VertexLruNode, node);
    memcpy(&vnode->key, key, sizeof(struct VertexKey));
    vnode->initialized = false;
}

static bool vertex_cache_entry_compare(Lru *lru, LruNode *node, const void *key)
{
    VertexLruNode *vnode = container_of(node, VertexLruNode, node);
    return memcmp(&vnode->key, key, sizeof(VertexKey));
}

static const size_t element_cache_size = 50*1024;

void pgraph_gl_init_buffers(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    lru_init(&r->element_cache);
    r->element_cache_entries = g_malloc_n(element_cache_size, sizeof(VertexLruNode));
    assert(r->element_cache_entries != NULL);
    GLuint element_cache_buffers[element_cache_size];
    glGenBuffers(element_cache_size, element_cache_buffers);
    for (int i = 0; i < element_cache_size; i++) {
        r->element_cache_entries[i].gl_buffer = element_cache_buffers[i];
        lru_add_free(&r->element_cache, &r->element_cache_entries[i].node);
    }

    r->element_cache.init_node = vertex_cache_entry_init;
    r->element_cache.compare_nodes = vertex_cache_entry_compare;

    GLint max_vertex_attributes;
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &max_vertex_attributes);
    assert(max_vertex_attributes >= NV2A_VERTEXSHADER_ATTRIBUTES);

    glGenBuffers(NV2A_VERTEXSHADER_ATTRIBUTES, r->gl_inline_buffer);
    glGenBuffers(1, &r->gl_inline_array_buffer);

#ifdef XEMU_UWP
    memset(uwp_stream_pool, 0, sizeof(uwp_stream_pool));
    memset(uwp_stream_fence, 0, sizeof(uwp_stream_fence));
    uwp_stream_cur = UWP_STREAM_POOL - 1;
    uwp_stream_next_buffer();
    uwp_stream_pos = 0;
#endif
    glGenBuffers(1, &r->gl_memory_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, r->gl_memory_buffer);
    glBufferData(GL_ARRAY_BUFFER, memory_region_size(d->vram),
                 NULL, GL_DYNAMIC_DRAW);

    glGenVertexArrays(1, &r->gl_vertex_array);
    glBindVertexArray(r->gl_vertex_array);

    assert(glGetError() == GL_NO_ERROR);
}

void pgraph_gl_finalize_buffers(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    GLuint element_cache_buffers[element_cache_size];
    for (int i = 0; i < element_cache_size; i++) {
        element_cache_buffers[i] = r->element_cache_entries[i].gl_buffer;
    }
    glDeleteBuffers(element_cache_size, element_cache_buffers);
    lru_flush(&r->element_cache);

    g_free(r->element_cache_entries);
    r->element_cache_entries = NULL;

    glDeleteBuffers(NV2A_VERTEXSHADER_ATTRIBUTES, r->gl_inline_buffer);
    memset(r->gl_inline_buffer, 0, sizeof(r->gl_inline_buffer));

    glDeleteBuffers(1, &r->gl_inline_array_buffer);
    r->gl_inline_array_buffer = 0;

    glDeleteBuffers(1, &r->gl_memory_buffer);
    r->gl_memory_buffer = 0;

    glDeleteVertexArrays(1, &r->gl_vertex_array);
    r->gl_vertex_array = 0;
}
