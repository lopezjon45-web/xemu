/*
 * xemu UWP port: install the Xbox dashboard from the user's own game disc.
 *
 * Many Xbox Live games carry Microsoft's dashboard installer (dashupdate.xbe)
 * in the root of the disc. The installer itself does not run on an emulated
 * console whose hard disk has no dashboard yet, but its payload can be
 * unpacked and written to the hard disk's C: partition directly. Nothing of
 * Microsoft's is shipped with the emulator: the files come from the disc
 * image the user starts.
 *
 * Copyright (c) 2026 Skitzelles
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef XEMU_UWP_DASH_H
#define XEMU_UWP_DASH_H

#include <stddef.h>
#include <stdint.h>

/* Byte access to a disc image or a hard disk. Both return 0 on success. */
typedef struct UwpDashIo {
    int (*read)(void *opaque, uint64_t offset, void *buf, size_t len);
    int (*write)(void *opaque, uint64_t offset, const void *buf, size_t len);
    void *opaque;
} UwpDashIo;

typedef void (*UwpDashLog)(const char *fmt, ...);

/*
 * 1 if the hard disk has a dashboard with its Xbox Live part
 * (C:\xodash\xonlinedash.xbe); *version is that file's version, e.g.
 * 0x185ead00. 0 if not, negative if the disk cannot be read as an Xbox disk.
 */
int uwp_dash_installed(const UwpDashIo *hdd, uint32_t *version);

/* 1 if the disc carries a dashboard installer; *version is what it installs. */
int uwp_dash_on_disc(const UwpDashIo *disc, uint32_t *version);

/*
 * Unpacks the installer's files, checks them, and writes them to C:.
 * Returns the number of files written, or a negative number; nothing is
 * written unless every file unpacked and checked out.
 */
int uwp_dash_install(const UwpDashIo *disc, const UwpDashIo *hdd, UwpDashLog log);

#endif
