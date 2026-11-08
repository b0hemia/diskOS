/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_SDIO_H
#define DISKOS_SDIO_H
#include <stdint.h>

/* A lease covers the lifetime of every SD handle, including decoder children.
 * Closing admission and counting existing leases use the same mutex. */
void sd_io_init(int (*media_local)(void));
int sd_io_begin(void);
void sd_io_end(void);
void sd_io_hold(void);
unsigned sd_io_active(void);
int sd_io_resume(void);
int sd_io_allowed(void);
/* Generation shares the admission lock. Invalidate when cached sources change.
 * Observe mount identity only off-thread; no card handles are opened here. */
uint64_t sd_io_generation(void);
uint64_t sd_io_media_generation(void);
void sd_io_invalidate(void);
void sd_io_check_media(void);
/* An unaccounted decoder keeps ownership uncertain across UI restarts. */
void sd_io_fault(void);
/* Boot-scoped sender ownership, recorded before any child can open media.
 * A stale marker holds admission until reboot; only its live owner clears it. */
int sd_io_owner_begin(void);
void sd_io_owner_end(void);
int sd_io_healthy(void);

#endif
