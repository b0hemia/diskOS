/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_CAST_HANDOFF_H
#define DISKOS_CAST_HANDOFF_H
#include <stdint.h>
#include "playback.h"
/* Cast handoff: the pure state machine that moves playback between the Disc and a receiver and back. No sockets, no LVGL, no
 * clock of its own: the caller (the main loop) feeds events with a millisecond clock and executes the actions the machine
 * asks for through an ho_sink. Single thread (the LVGL thread).
 *
 *   LOCAL -> PREPARING -> PAUSING -> LOCAL_PAUSED -> REMOTE_READY -> CASTING     (STOPPING = waiting for a remote STOP)
 *
 * PREPARING waits for the receiver (TLS, status); PAUSING sends ONE local pause (only if the Disc is playing) and waits until
 * Local is reselected once, then every playback PCM is proven closed; LOCAL_PAUSED has served the file and sent LOAD (autoplay off) at the captured position; REMOTE_READY
 * has the load accepted and waits for the owned media to be confirmed; only then is PLAY sent (if the Disc was playing).
 * Every state has a deadline and every failure rolls back the same way: serving revoked, the receiver stopped (when it may hold
 * our media) or released, the facade back to LOCAL, the Disc left as it is. The machine has no local-play action at all: it
 * never starts the Disc, and it never restarts the player or selects a different source. A queued pause is not an acknowledgement:
 * the pause frame is never resent (it is a toggle), the caller supplies the paused state before teardown. */
#define HO_QUIET_MS   500    /* PCM proven quiet this long: the same hold modes.c uses before it changes the output route */
#define HO_PREP_MS    15000  /* receiver connect + launch + status; device-tune */
#define HO_PAUSE_MS   3000   /* pause sent -> quiet proven (includes the hold); equals modes.c OUT_WAIT_MS */
#define HO_LOAD_MS    12000  /* LOAD sent -> accepted (the receiver fetches the first bytes over Wi-Fi); device-tune */
#define HO_MEDIA_MS   5000   /* LOAD accepted -> owned media confirmed */
#define HO_STOP_MS    4000   /* remote STOP sent -> confirmed; then ownership is released regardless */
typedef enum { HO_LOCAL = 0, HO_PREPARING, HO_PAUSING, HO_LOCAL_PAUSED, HO_REMOTE_READY, HO_CASTING, HO_STOPPING } ho_state;
typedef enum { HO_MSG_NONE = 0, HO_MSG_UNAVAILABLE, HO_MSG_FAILED, HO_MSG_SD, HO_MSG_RESTARTED, HO_MSG_CHANGED, HO_MSG_BUSY,
               HO_MSG_TAKEN, HO_MSG_LOST, HO_MSG_STOP_UNCONFIRMED, HO_MSG_LOCAL_PLAY, HO_MSG_SOURCE } ho_msg;
typedef enum { HO_EV_NONE = 0,
    HO_EV_SELECT,          /* user picked a receiver (local = the Disc right now, dest = its name) */
    HO_EV_LOCAL,           /* a local tick */
    HO_EV_REMOTE_READY,    /* session ready; local must contain the current Disc snapshot */
    HO_EV_LOAD_OK, HO_EV_LOAD_FAILED,
    HO_EV_MEDIA,           /* media status: owned = the owned media session is confirmed */
    HO_EV_REMOTE_STOPPED,  /* the remote STOP was confirmed */
    HO_EV_REMOTE_LOST,     /* session lost or failed */
    HO_EV_TAKEOVER,        /* another sender replaced our app or media */
    HO_EV_STOP             /* user: Stop casting */
} ho_evtype;
/* What the Disc is doing now. Pointers are only read during the call (a select copies what it keeps). */
typedef struct {
    int have_track, playing, pcm_quiet, sd_ok;   /* playing = inferred play state; pcm_quiet = every playback PCM readable and closed */
    unsigned generation;                          /* IPC generation: changes when the player restarts */
    uint64_t token;                               /* playback_token: track identity epoch */
    long position_ms, duration_ms;
    const char *path, *title, *artist, *album;
    int source_mode;                              /* 16b: the working mode (0 = Local); a change while CASTING ends the session */
} ho_local;
typedef struct { ho_evtype type; uint32_t attempt; const ho_local *local; const char *dest; int owned; } ho_event;
/* attempt: remote events carry the value ho_attempt() returned when the work they answer was started; others are ignored.
 * Every callback returns 0 or < 0 (refused) where it can; a refusal rolls back at once. serve_load starts serving the file and
 * sends LOAD with autoplay false at position_ms. */
typedef struct {
    void *ctx;
    int  (*local_pause)(void *ctx);
    int  (*serve_load)(void *ctx, uint32_t attempt, const char *path, long position_ms);
    int  (*remote_play)(void *ctx);
    int  (*remote_stop)(void *ctx);
    void (*revoke)(void *ctx);                    /* stop serving: the HTTP capability dies */
    void (*release)(void *ctx);                   /* give up the receiver session */
    void (*set_cast)(void *ctx, const pb_cast_state *c);   /* NULL = LOCAL */
    void (*message)(void *ctx, ho_msg m);
    int  (*local_teardown)(void *ctx);             /* guarded reselect: 0 queued, -1 failed, IPC_SEND_DEFERRED pending */
    /* Optional beam interlock (NULL = off: firmware where it is not verified). local_lock puts the player in NO_WORK_MODE
     * (NO_WORK_MODE) so the stock play/pause key cannot start the Disc; local_unlock restores Local (the Local reselect). Both
     * return like local_teardown. The machine sends the lock once PCM is proven closed, confirms it by a fresh closed-PCM hold,
     * keeps it through CASTING and STOPPING and sends the unlock on every exit (finish()), retrying while LOCAL. */
    int  (*local_lock)(void *ctx);
    int  (*local_unlock)(void *ctx);
} ho_sink;
typedef struct { int sent; uint32_t quiet_since; } ho_drain;
/* Zero per attempt. Caller owns pause, generation/source checks and deadline.
 * Returns -1 on send failure, 0 while waiting, 1 after strict closure holds.
 * pcm_closed must come from modes_pcm_quiet(). Deferred sends remain pending;
 * only a queued frame sets sent, so at most one teardown frame is enqueued. */
int ho_drain_step(ho_drain *d, uint32_t now, int playing, int pcm_closed,
                  int (*send)(void *), void *ctx);
#define HO_UNLOCK_RETRY_MS 250   /* a refused or deferred unlock is retried this often while LOCAL ... */
#define HO_UNLOCK_TRIES    20    /* ... after this many failed tries one diagnostic is logged; the unlock stays owed and keeps retrying */
typedef struct {
    ho_state state; uint32_t attempt, deadline, quiet_since, loud_since;
    int was_playing, pause_sent, engaged, facade, user_stop, src;   /* engaged: LOAD sent, the receiver may hold our media */
    ho_drain drain, lock;                          /* drain: Local reselect; lock: the interlock send + its closed-PCM hold */
    int lock_phase, unlock_due, unlock_tries; uint32_t unlock_next, clock;   /* clock = the last tick the machine saw */
    ho_msg after;                                  /* message once a STOPPING ends */
    unsigned gen; uint64_t token; long pos, dur;
    char path[TRACK_PATH_MAX], title[256], artist[160], album[160], dest[48];
} ho;
void     ho_init(ho *h);
ho_state ho_get(const ho *h);
uint32_t ho_attempt(const ho *h);
/* The physical play/pause key. A press is exactly EV_KEY (1), code 250, value 1; value 0 (release) and 2 (repeat: one press must be
 * one toggle) and every other code (268 long, 269 double press of the play key, volume codes) are not. */
#define HO_KEY_PLAYPAUSE 250
int      ho_key_is_press(unsigned type, unsigned code, int value);
typedef enum {
    HO_KEY_PASS = 0,   /* not ours: the stock player handles it (no beam, preparing, or the interlock is off) */
    HO_KEY_REMOTE,     /* exactly one receiver toggle; never a local frame */
    HO_KEY_HELD,       /* before the media is controllable: the pending PLAY intent was flipped, no frame at all */
    HO_KEY_DROP,       /* ending: swallowed, never replayed into Local or a later beam */
    HO_KEY_ABORT       /* pause/interlock not complete: the lock is not proven, the caller cancels the handoff */
} ho_key_act;
/* interlock_on: the caller's firmware switch (fwcaps). Only HO_KEY_HELD mutates the machine. */
ho_key_act ho_key_route(ho *h, int interlock_on);
int      ho_interlocked(const ho *h);                                   /* the lock frame was sent and has not been released */
void     ho_unlock_retry(ho *h, uint32_t now, const ho_sink *k);        /* LOCAL only: resend a refused unlock */
void     ho_step(ho *h, uint32_t now, const ho_event *ev, const ho_sink *k);   /* ev may be NULL: time only */
/* The quiet-confirmation technique shared with the output switch (modes.c): *since = 0 while playing or not proven quiet, else
 * the tick the quiet began. Returns 1 once quiet has held for hold_ms. Wrap-safe. */
int ho_quiet_step(uint32_t *since, uint32_t now, int playing, int pcm_quiet, uint32_t hold_ms);
#endif
