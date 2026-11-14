/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_FILE_OBSERVER_H
#define DISKOS_FILE_OBSERVER_H
#include <stddef.h>
/* One UI-thread owner. Parent watches cover atomic rename, unlink and recreate.
 * A dirty result only requests fresh verification; it never grants SD access. */
int file_observer_start(const char *directory, const char *const *names, size_t count);
int file_observer_fd(void);
int file_observer_take(void);
void file_observer_events(const void *buffer, size_t size);
#endif
