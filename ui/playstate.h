/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef PLAYSTATE_H
#define PLAYSTATE_H

/* Pure play/pause inference from the player's position samples. The raw A2 'state' field is
 * unreliable (reports 0 while playing), so play/pause is derived from whether the reported
 * position is advancing. Kept pure + dependency-free so it is unit-tested off-device and used as
 * ONE source of truth (the main-loop transport glyph + g_playing for routing/lastfm). */
typedef struct {
    long     last_pos_ms;   /* last observed position; -1 = no observation yet */
    unsigned change_tick;   /* monotonic tick at the last position ADVANCE */
} playstate_t;

/* reset to "no track / no observation" (e.g. on player reconnect). */
void playstate_reset(playstate_t *ps);

/* Return 1 if the track is playing. Call once per sample.
 *   have_track  : 0 -> not playing (and resets the observation).
 *   position_ms : the player's reported position.
 *   now_tick    : a monotonic millisecond tick (wrap-safe; e.g. lv_tick_get()).
 *   grace_ms    : still "playing" for this long after the last advance (absorbs a missed tick).
 *   max_adv_ms  : a forward jump larger than this is treated as a SEEK, not playback (0 = disabled,
 *                 i.e. any positive change counts - matches the pre-extraction behavior). A BACKWARD
 *                 jump never counts as advancing, regardless of max_adv_ms. */
int playstate_playing(playstate_t *ps, int have_track, long position_ms,
                      unsigned now_tick, unsigned grace_ms, unsigned max_adv_ms);

typedef struct {
    unsigned active, landed, sent_tick, landing_tick, pos_seq;
} playstate_seek_t;

/* Notify only after a successful local seek; never turn a paused seek into play. */
void playstate_seek_sent(playstate_seek_t *s, unsigned now, unsigned pos_seq,
                         int was_playing);
/* pos_seq changes only on a fresh position report, not on metadata updates. */
int playstate_seek_playing(playstate_t *ps, playstate_seek_t *s,
                           int have_track, long position_ms, unsigned pos_seq,
                           unsigned now, unsigned grace_ms, unsigned max_adv_ms);

#endif
