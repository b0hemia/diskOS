/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef ARTCACHE_H
#define ARTCACHE_H
#include <stdint.h>
#include "art_identity.h"
void artcache_accent_key(const art_picture_id *picture, char *out, int cap);
void artcache_protect(int group, const char *const *tracks, int n);
uint64_t artcache_pin_key(const char *track);
void artcache_protect_keys(int group, const uint64_t *keys, int n);
int artcache_accent_candidate(const char *track, const char *key, char *out, int cap);
int artcache_background_skip(const char *track, const char *key);
/* Worker-only bounded migration/reclamation, called with a stable library snapshot. */
int artcache_maintenance(void);
void artcache_maintenance_wake(void);
/* Bounded RAM lookup: no file reads, stats, hashing or decoding. */
int artcache_resolution(const char *track, art_resolution *out);
/* UI publication guard; eviction alone does not invalidate a completed request. */
int artcache_resolution_current(const char *track, const art_resolution *expected);
void artcache_resolution_invalidate(void);
/* Worker-only wake check. Returns 1 if a retained source changed or cannot be
 * verified. Include the displayed proof even if the bounded table evicted it. */
int artcache_revalidate(const char *track, const art_resolution *displayed);
/* Worker only. 8 retains the caller's known pixels; 0 wrote all derivatives. */
int artcache_resolve(const char *track, const char *cover, const char *thumb, const char *bg,
                     int cancellable, unsigned gen, const art_picture_id *known, art_resolution *out);

/* Warm small-cover requests omit thumb/backdrop BLOB reads. Cold decodes still
 * produce the unchanged complete recipe in the supplied private output paths. */
int artcache_resolve_cover(const char *track, const char *cover, const char *thumb, const char *bg,
                          int cancellable, unsigned gen, const art_picture_id *known, art_resolution *out);
/* Bounded SQLite derivative sets shared by encoded-picture SHA-256, length and
 * recipe; metadata fingerprints locate candidates, never establish equality. */

/* Worker-only variant consuming a just-captured artcache_lookup revision. */
int artcache_resolve_if(const char *revision, const char *track, const char *cover, const char *thumb, const char *bg,
                        int cancellable, unsigned gen, const art_picture_id *known, art_resolution *out);
/* Background preparation only: 1 if the entry under `key` (from artcache_lookup) is complete and its disk record
 * matches the source's current revision. Not a display proof: the RAM resolution table is left untouched. */
int artcache_prepared(const char *track, const char *key);
/* Satisfy a request from cache: copies the cached cover/thumb/backdrop BMPs to the
 * given output paths. Returns 0 on a full hit (all three written), -1 on miss. */
int  artcache_get(const char *track, const char *cover_out, const char *thumb_out, const char *bg_out);

/* Store freshly-decoded BMPs into the cache for this track (atomic, best-effort).
 * No-op if the SD is nearly full. */
void artcache_put(const char *track, const char *cover, const char *thumb, const char *bg);
/* The cache key for the track as it is NOW (track + sidecar covers). Capture it before decoding and insert with
 * artcache_put_if, which stores nothing if the key changed meanwhile. 0 on success. */
int  artcache_key(const char *track, char *out, int cap);
void artcache_put_if(const char *track, const char *fp_before, const char *cover, const char *thumb, const char *bg);

/* 1 if this track already has a complete cache entry (cover+thumb+backdrop). */
int  artcache_has(const char *track);
/* artcache_has + artcache_key with one fingerprint: 1 = complete entry, 0 = miss with the current key in key_out,
 * -1 = no key (track unreadable or SD not available). */
int  artcache_lookup(const char *track, char *key_out, int cap);

/* Copy JUST the cached 42px thumb to thumb_out (fast, no ffmpeg). Used by the Books
 * list, which only needs the small cover. Returns 0 on hit, -1 if the thumb isn't cached. */
int  artcache_get_thumb(const char *track, const char *thumb_out);
int artcache_get_cover_background(const char *track, const char *out);
int  artcache_get_cover(const char *track, const char *cover_out);   /* just the 148px cover (Album Wall) */
int  artcache_cover_path(const char *track, char *out, int cap);     /* worker-only per-thread RAM materialisation */
/* Worker-only candidate path. Does not open or validate the BMP; caller must do both. */
int  artcache_cover_candidate(const char *track, char *out, int cap);

/* Monotonic counter bumped each time a NEW cover is written to the cache (any decoder). The cover
 * flow polls this to re-bake placeholder albums the moment their art becomes available. */
unsigned artcache_gen(void);

/* SD-write ownership guard (implemented in main.c). artcache_put brackets its SD writes with
 * begin()/end() so they never race the player's Storage-mode export of /dev/mmcblk0. begin()
 * returns 0 (skip) when the card is host-owned or an export is in progress. */
int  sd_write_begin(void);
void sd_write_end(void);

#endif
