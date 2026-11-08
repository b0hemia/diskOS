/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_ARTSTORE_H
#define DISKOS_ARTSTORE_H
#include "art_identity.h"
/* All operations are worker-only. Each call owns a lease and closes all SQLite
 * handles before releasing it. No connection survives a media generation. */
int artstore_record(const char *key, const char *revision, art_resolution *out, int *complete);
int artstore_read(const char *key, const char *cover, const char *thumb, const char *bg, int demand);
int artstore_save(const char *track, const char *key, const art_resolution *r, const char *cover, const char *thumb, const char *bg, int background);
int artstore_link(const char *track, const char *key, const art_resolution *r, int background);
int artstore_suppressed(const char *key, const char *revision);
/* Pins are replaceable bounded groups: playback=0, playback neighbours=1,
 * wall viewport/neighbours=2. Reads/writes also pin their in-flight identity. */
void artstore_protect(int group, const char *const *tracks, int n);
/* Snapshot callbacks run under a successful read-only library transaction.
 * No retirement runs unless finish(complete=1) and the live snapshot still matches. */
void artstore_snapshot_begin(uint64_t library);
int artstore_snapshot_add(const char *key);
void artstore_snapshot_finish(int complete);
void artstore_idle(void);
int artstore_maintain(uint64_t library);
int artstore_maintain_checked(uint64_t library, int (*current)(void));
#endif
