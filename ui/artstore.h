/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_ARTSTORE_H
#define DISKOS_ARTSTORE_H
#include <stdint.h>
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
/* The same pins by key: artstore_pin_key() hashes a whole path (0 = none/empty), so a caller that keeps many pins
 * can hold 8 bytes per track instead of a copy of a path of up to TRACK_PATH_MAX bytes. A collision only protects
 * one extra picture, never unprotects one. */
uint64_t artstore_pin_key(const char *track);
void artstore_protect_keys(int group, const uint64_t *keys, int n);
/* Snapshot callbacks run under a successful read-only library transaction.
 * No retirement runs unless finish(complete=1) and the live snapshot still matches. */
void artstore_snapshot_begin(uint64_t library);
int artstore_snapshot_add(const char *key);
void artstore_snapshot_finish(int complete);
void artstore_idle(void);
int artstore_maintain(uint64_t library);
int artstore_maintain_checked(uint64_t library, int (*current)(void));
#endif
