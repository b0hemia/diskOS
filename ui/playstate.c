/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "playstate.h"

void playstate_reset(playstate_t *ps){ ps->last_pos_ms = -1; ps->change_tick = 0; }

int playstate_playing(playstate_t *ps, int have_track, long position_ms,
                      unsigned now_tick, unsigned grace_ms, unsigned max_adv_ms){
    if(!have_track){ ps->last_pos_ms = -1; return 0; }
    if(ps->last_pos_ms < 0){
        /* First observation: seed the baseline as FRESH (matches the original heuristic - a track
         * present at pos>0 reads playing within the grace window, self-correcting to paused if it
         * never advances). We deliberately do NOT report a startup "paused" here: g_playing feeds
         * autorouting, and a false-paused makes it skip the pause/resume and latch the route without
         * retry - worse than the transient boot-paused false-positive. Distinguishing a true UNKNOWN
         * belongs with the routing rework (a 3-state model), not here. */
        ps->last_pos_ms = position_ms;
        ps->change_tick = now_tick;
    } else if(position_ms != ps->last_pos_ms){
        long delta = position_ms - ps->last_pos_ms;
        ps->last_pos_ms = position_ms;
        /* Only a FORWARD advance consistent with playback refreshes the "advancing" timestamp. A
         * backward jump (rewind/restart) never does (an improvement over the original, which refreshed
         * on ANY change); and when max_adv_ms is set, a large forward jump (a paused seek/scrub) is a
         * SEEK, not playback. max_adv_ms==0 disables the upper bound (any positive change counts - the
         * original behavior; the bound is device-tunable). */
        if(delta > 0 && (max_adv_ms == 0 || (unsigned long)delta <= max_adv_ms))
            ps->change_tick = now_tick;
    }
    return (position_ms > 0 && (unsigned)(now_tick - ps->change_tick) < grace_ms);
}

void playstate_seek_sent(playstate_seek_t *s, unsigned now, unsigned pos_seq,
                         int was_playing){
    *s = (playstate_seek_t){ .active = !!was_playing,
                            .sent_tick = now, .pos_seq = pos_seq };
}

int playstate_seek_playing(playstate_t *ps, playstate_seek_t *s,
                           int have_track, long position_ms, unsigned pos_seq,
                           unsigned now, unsigned grace_ms, unsigned max_adv_ms){
    if(!have_track || (unsigned)(now - s->sent_tick) >= 4000 ||
       (s->landed && (unsigned)(now - s->landing_tick) >= 2500))
        s->active = 0;
    if(s->active && pos_seq != s->pos_seq){
        s->pos_seq = pos_seq;
        if(!s->landed){
            s->landed = 1;
            s->landing_tick = now;
            ps->last_pos_ms = position_ms;
        } else {
            s->active = 0;
        }
    }
    int playing = playstate_playing(ps, have_track, position_ms, now,
                                    grace_ms, max_adv_ms);
    return s->active ? 1 : playing;
}
