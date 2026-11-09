/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_CAST_CTL_H
#define DISKOS_CAST_CTL_H
#include <stdint.h>
#include "cast_queue.h"
#include "cast_session.h"
#include "trackpath.h"
/* Cast controller core (slice 14d): after the handoff reaches CASTING this owns the Cast queue and cursor and decides every later LOAD.
 * A pure step(event) -> action list: no threads, no LVGL, no sockets, no allocation per event (the queue and cursor allocate only when a
 * queue is built or replaced). The caller (14e: one worker plus the LVGL tick) feeds events and carries the actions out; the struct must
 * not move once an ENTER succeeded (the cursor points into it). Single thread.
 * Queue generation (qgen): bumped on every queue replace (SELECT), ENTER and teardown (LOCAL). Events that carry a nonzero qgen different
 * from the controller's are no-ops; LOAD actions hand back qgen + the cursor's gen, and LOAD_STARTED/LOAD_REFUSED must return both.
 * The machine owns the first LOAD, the controller every later one: its LOADs go through cb_serve_load_next (no handoff edge), so
 * cb_events emits nothing for them and the controller reads load_seq / load_state from the SNAPSHOT events itself.
 * Slice 14e1: deps.load_ms is the pending-LOAD deadline. A LOAD that has not adopted or failed that long after its action (measured by
 * the now_ms of the events) is failed on the next SNAPSHOT: STAY_PAUSED + a toast. Orphan rule: a STAY_PAUSED reached while an older LOAD
 * may still land (Next onto an excluded track, a SELECT that fails to start, the deadline) also emits STOP_MEDIA, so the receiver is not
 * left holding a track the controller no longer owns (the session's media STOP waits for the pending LOAD, then ends the media). */
#define CC_ACT_MAX 4
enum { CC_EV_NONE = 0, CC_EV_ENTER, CC_EV_SNAPSHOT, CC_EV_LOAD_STARTED, CC_EV_LOAD_REFUSED, CC_EV_NEXT, CC_EV_PREV, CC_EV_SELECT, CC_EV_MODE, CC_EV_LOCAL,
       CC_EV_JUMP /* slice 14f: Up Next tap: qgen + occ (a base index) */ };
enum { CC_A_NONE = 0, CC_A_LOAD, CC_A_PLAY_AFTER_ADOPT, CC_A_STAY_PAUSED, CC_A_TOAST, CC_A_REPUBLISH,
       CC_A_STOP_MEDIA /* slice 14e1: an older LOAD may still land on the receiver: stop it (cb_stop) */ };
enum { CC_T_NONE = 0, CC_T_LIST_FAILED /* "Couldn't play that list" */, CC_T_UPDATING /* "Queue is updating - try again" */,
       CC_T_COULD_NOT_PLAY /* "Receiver could not play this track" */, CC_T_QUEUE_FINISHED, CC_T_UNSUPPORTED, CC_T_NONE_PLAYABLE,
       CC_T_QUEUE_CHANGED /* "Queue changed" (14f) */ };   /* ids; the strings are slice 15/17 */
typedef struct { int dur_ms; char title[MDB_STR], artist[MDB_STR], album[MDB_STR], path[TRACK_PATH_MAX]; } cc_row;   /* what cb_serve_load_next and the facade need */
typedef struct {
    int type;
    uint32_t qgen, gen;             /* qgen: the queue generation the sender saw (0 = current). gen: the LOAD action's cursor gen (LOAD_STARTED / LOAD_REFUSED) */
    cast_queue *q;                  /* ENTER: the LIVE snapshot; moved into the controller on success (*q is zeroed), untouched on refusal */
    int start_ms, media_id, dur_ms, was_playing;   /* ENTER: the handed-off media (its mediaSessionId, duration) and whether the receiver was playing */
    int player, idle, load_state; unsigned load_seq; double media_time;   /* SNAPSHOT: cs_snapshot fields (media_id above); LOAD_STARTED: load_seq */
    int position_ms;                /* PREV */
    const int *ids; int n, start_index; /* SELECT (ids is copied); MODE and SELECT use mode */
    int mode;
    int kind;                       /* SELECT: 0 = ids are SONG.IDs, 1 = CUSTOM_PLAYLIST.IDs of one playlist (slice 14g); a zeroed event is a song selection */
    int occ;                        /* JUMP: the base index of the tapped occurrence; qgen must be the one the list was built from (0 or another = "Queue changed") */
    uint32_t now_ms;                /* any event: the caller's millisecond clock (wrap-safe); drives the pending-LOAD deadline, 0 = unchanged */
} cc_ev;
typedef struct {
    int kind, k, start_ms, skipped, reason, cause, toast, queue_pos, queue_len, playing, media_id;   /* playing: REPUBLISH, the play intent; media_id: REPUBLISH, the receiver media the published row is on (15c: Last.fm / history confirm) */
    uint32_t qgen, gen;
    const cc_row *row;              /* LOAD / REPUBLISH / STAY_PAUSED (NULL if unreadable): valid until the next step; the row of occurrence k */
} cc_act;                           /* LOAD: k = base index, row for cb_serve_load_next. STAY_PAUSED: reason/cause (CA_*) + k. queue_pos is 1-based, in play order */
typedef struct {
    int (*classify)(void *ctx, const cast_queue *q, int idx, int *dur_ms);   /* CF_V_* for base index idx of q (may read the file) */
    int (*row)(void *ctx, const cast_queue *q, int idx, cc_row *out);        /* base index -> display row; 0 ok. NULL = cast_ctl_row_db */
    void *ctx; uint64_t seed;                                                /* seed: the shuffle rng seed (production: /dev/urandom) */
    int load_ms;                                                             /* pending-LOAD deadline (slice 14e1), 0 = none: see cast_ctl_step */
} cast_ctl_deps;
typedef struct cast_ctl cast_ctl;
typedef struct { cast_ctl *c; cast_queue q; cast_cursor cur; } cc_slot;   /* two slots: a new queue is built beside the old one, then swapped */
struct cast_ctl {
    cast_ctl_deps d; cc_slot s[2]; int a;
    int active, pend, known, playing, settle, wp, orph, out, prior;   /* pend/known: a LOAD is out / its load_seq is reported; playing: receiver playing intent; wp: play after adopt;
                                                                   * out: set around one cursor move while the receiver holds media this move abandons; prior: the pending LOAD replaced something that may still sit on the receiver */
    uint32_t qgen, pgen, now, pdue; unsigned pseq;   /* orph: an older LOAD may still land (its queue was replaced) */
    cc_row row;
};
void cast_ctl_init(cast_ctl *c, const cast_ctl_deps *d);
/* Returns the number of actions written to out (0..CC_ACT_MAX). */
int  cast_ctl_step(cast_ctl *c, const cc_ev *e, cc_act out[CC_ACT_MAX]);
/* Default row lookup: SELECTION -> mdb_song_rows(sid) (a playlist occurrence: mdb_playlist_rows_get), LIVE -> cast_queue_page (album empty there). Worker thread only (reads the database). */
int  cast_ctl_row_db(void *ctx, const cast_queue *q, int idx, cc_row *out);
#endif
