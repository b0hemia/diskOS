/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_CAST_SESSION_H
#define DISKOS_CAST_SESSION_H
#include "cast_messages.h"
#include "remote_tls.h"
/* One worker thread owns the socket, TLS, protocol state, request IDs and heartbeat. Callers (the UI thread)
 * only send bounded commands and copy a snapshot; nothing here calls LVGL or holds a lock across I/O.
 * Connect, receiver-0 status, launch of the Default Media Receiver, app CONNECT, heartbeat, deadlines, cancel with
 * rollback of an app this attempt launched, clean close and reattach; on the owned app: media PLAY/PAUSE/SEEK,
 * receiver SET_VOLUME, MEDIA_STATUS tracking of the owned mediaSessionId, takeover detection; LOAD (never autoplay) with a
 * confirmed owned media session, and STOP of that owned media session only. cast_session_track() alone only names the
 * contentId whose MEDIA_STATUS entry is adopted as the owned media session. */
typedef struct cast_session cast_session;
typedef enum { CS_CONNECTING = 1, CS_LAUNCHING, CS_READY, CS_BUSY, CS_LOST, CS_FAILED, CS_CLOSED, CS_TAKEN_OVER } cs_state; /* last five: terminal */
/* peer silent; a queued frame could not be written in time; LAUNCH refused; app not ready within launch_ms */
enum { CS_E_DEAD = -100, CS_E_STALL = -101, CS_E_LAUNCH = -102, CS_E_LAUNCH_TIMEOUT = -103,
    CS_E_TAKEOVER = -104, CS_E_REFUSED = -105 /* receiver/command not acceptable: unknown volume, INVALID_REQUEST */,
    CS_E_LOAD_FAILED = -106 /* LOAD_FAILED, LOAD_CANCELLED or INVALID_REQUEST for this LOAD */, CS_E_LOAD_TIMEOUT = -107, /* load_ms */
    CS_E_STOP_FAILED = -108 /* media STOP refused or superseded */, CS_E_STOP_TIMEOUT = -109, /* confirm_ms */
    CS_E_LINK = -110 /* session left READY (lost, failed, closed) while a LOAD or STOP was pending */ };
enum { CS_JOB_NONE, CS_JOB_PENDING, CS_JOB_OK, CS_JOB_FAILED }; /* LOAD / media STOP outcome; OK = media owned / media ended */
enum { CS_CLEAN_NONE, CS_CLEAN_STOPPED, CS_CLEAN_UNCERTAIN }; /* rollback of a launch that was never handed over */
typedef enum { CS_OP_STATUS = 1, CS_OP_PLAY, CS_OP_PAUSE, CS_OP_SEEK, CS_OP_VOLUME, CS_OP_VOLUME_STEP } cs_op; /* STATUS re-inspects the receiver */
enum { CS_PL_NONE, CS_PL_BUFFERING, CS_PL_PLAYING, CS_PL_PAUSED, CS_PL_IDLE, CS_PL_OTHER };                 /* playerState */
enum { CS_IDLE_NONE, CS_IDLE_FINISHED, CS_IDLE_ERROR, CS_IDLE_INTERRUPTED, CS_IDLE_CANCELLED, CS_IDLE_OTHER }; /* idleReason */
typedef struct {
    const char *host; int port;          /* numeric IPv4 and port from discovery */
    int replace;                         /* explicit intent to replace a running (non-idle) app */
    int reconnect;                       /* reattach attempts to our owned session after loss; never LAUNCH */
    int launch_ms;                       /* LAUNCH queued -> app transport CONNECT flushed */
    int connect_ms;                      /* TCP + TLS + CONNECT + first status */
    int load_ms;                         /* LOAD queued -> MEDIA_STATUS for it with the expected contentId */
    int confirm_ms;                      /* command taken from the queue -> matching reply */
    int close_ms;                        /* whole clean-close sequence */
    int ping_ms, dead_ms;                /* heartbeat interval; silence limit and per-frame write limit */
    void *ctx;                           /* optional connector (tests): returns a connected fd or -1 */
    int (*connect)(void *ctx, const char *host, int port, int64_t deadline);
} cast_session_cfg;
typedef struct {
    cs_state state; uint64_t generation; /* generation changes on every published change */
    int error, op_error;                 /* terminal RT or CS_E code; last command timeout */
    int cleanup;                         /* CS_CLEAN_*: STOP sent for an unfinished launch, or ownership uncertain */
    char busy_app[CM_ID], busy_name[CM_ID]; /* running app id and displayName when BUSY */
    cm_app app;                          /* owned app (ids from the matching LAUNCH reply) once known */
    cm_volume volume;                    /* receiver level and mute, from receiver-0 RECEIVER_STATUS only; absent fields keep the last value */
    int media_id, player, idle, confirmed; /* owned mediaSessionId (0 = none); CS_PL_*, CS_IDLE_* (reason valid while IDLE); id of the last confirmed command */
    int stopped;                        /* current occurrence stopped; media_id is a tombstone, only a new LOAD can play it */
    double media_time;                   /* last currentTime reported for the owned media session */
    char taken_by[CM_ID];                /* CS_TAKEN_OVER: displayName (else appId) of the replacing app, empty for a new media session */
    int load_state, load_error;          /* CS_JOB_*, and the CS_E_* code once FAILED; load_seq counts accepted cast_session_load() calls */
    int stop_state, stop_error;          /* media STOP likewise (OK also when nothing was owned); stop_seq counts accepted stop requests */
    unsigned load_seq, stop_seq;
    unsigned seek_failures;              /* refused/timed-out seeks; retained across background media polls */
} cs_snapshot;
typedef struct {
    const char *content_id, *content_type;   /* non-empty; the contentId is what the receiver is expected to report back */
    const char *title, *artist, *album;      /* NULL = empty */
    const char *image;                       /* NULL or empty = no cover URL */
    double start;                            /* seconds, finite, 0..INT32_MAX; the track is loaded paused (autoplay is always false) */
} cs_load;
/* Starts the worker; every other call is safe from any one controlling thread. */
int cast_session_start(cast_session **out, const cast_session_cfg *cfg);
uint64_t cast_session_snapshot(cast_session *, cs_snapshot *out);
/* RT_ARGUMENT unless READY (and, for PLAY/PAUSE/SEEK, an owned media session exists) or when the 4-entry queue is full.
 * Commands are sent one at a time, each with its own request id and the confirm_ms deadline; a reply for another request id,
 * or about another mediaSessionId, never changes the snapshot. After takeover no queued or unwritten command is sent: only
 * the receiver-0 CLOSE, and a frame already partly written may complete. */
int cast_session_submit(cast_session *, cs_op);  /* STATUS, PLAY, PAUSE */
int cast_session_seek(cast_session *, double seconds);            /* finite, 0..INT32_MAX */
int cast_session_volume(cast_session *, double level, int muted); /* receiver level 0..1 (<0 keeps) and mute 0/1 (-1 keeps) */
int cast_session_volume_step(cast_session *, int direction); /* +/-1: one percentage point, resolved by worker */
int cast_session_track(cast_session *, const char *content_id);   /* expect media with this contentId; forgets the owned media session */
/* LOAD of one track, always paused. RT_ARGUMENT unless READY and the request is valid (checked by encoding it, so the worker never
 * meets a request it cannot send). Hides the current media and sets load_state PENDING with a new load_seq. Failure restores
 * the last proven media identity so STOP can still address it; it never adopts the failed LOAD from a late broadcast.
 * The MEDIA_STATUS answering THIS request id names the candidate mediaSessionId (its media block may be omitted when unchanged; the
 * contentId is then taken from a status cached by mediaSessionId during this LOAD). OK only after a second, correlated media
 * GET_STATUS shows that candidate still exists with the expected contentId (that session becomes the owned media); a broadcast
 * alone never completes a LOAD. FAILED on a refusal for this request id, after load_ms (one total budget, not restarted by the
 * verification), or when the session leaves READY. A newer load supersedes a pending one: replies to the older request are
 * ignored. Statuses for other media are ignored while loading. */
int cast_session_load(cast_session *, const cs_load *);
/* Media-namespace STOP of the owned media session only (never another sender's). OK when a status shows our media IDLE with reason
 * CANCELLED or FINISHED, or the reply carries no media at all; FAILED on refusal, supersession by a new LOAD, after confirm_ms,
 * or when the session leaves READY.
 * With nothing owned and no LOAD pending it is OK at once and no frame is sent; during a pending LOAD it waits for that LOAD first.
 * RT_ARGUMENT when media is owned (or loading) but the session is not READY. */
int cast_session_stop_media(cast_session *);
void cast_session_cancel(cast_session *);       /* prompt: abort; STOPs an app launched but not yet READY; else clean close */
void cast_session_close(cast_session *);        /* clean close: CLOSE app transport, then receiver-0; a READY app keeps running */
void cast_session_free(cast_session *);         /* cancel + join; always bounded */
#endif
