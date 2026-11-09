/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef IPC_H
#define IPC_H
#include <stdint.h>

/* Main-thread runtime #28 experiment. eligible means confirmed local BT intent;
 * preference must be an explicit 0/1. Absent/invalid debug file sends nothing
 * unless restoring an experiment already active in this process. */
void ipc_bt_autonext_poll(uint32_t now, int eligible, int preference);
#include "trackpath.h"
#define IPC_TITLE_CAPACITY 256
/* Bounded receiver data, copied read-only; transport_state is the raw stock
 * value, not a proven playing indication. Progress stays invalid until qualified. */
typedef struct {
    unsigned generation;
    uint64_t session_revision, item_revision;
    char title[IPC_TITLE_CAPACITY], artist[160], album[160];
    int transport_state, transport_valid, progress_valid;
    long position_ms, duration_ms;
    /* Monotonic receive times (ms) and sequence counters feed receiver_progress.
     * pos_*: last a103 position; pos_item_revision is the item it arrived under.
     * item_rx_ms: when item_revision last changed. dur_*: last a202 duration. */
    uint64_t pos_rx_ms, pos_item_revision, item_rx_ms, dur_rx_ms;
    unsigned pos_seq, dur_seq;
} ipc_receiver_meta;
void ipc_receiver_begin(unsigned generation, uint64_t arm);
void ipc_receiver_end(void);
void ipc_receiver_return(unsigned generation, uint64_t arm);
void ipc_receiver_metadata(ipc_receiver_meta *out);
unsigned ipc_transport_seq(void); /* main-thread transport-attempt epoch */
typedef struct {
    char title[IPC_TITLE_CAPACITY];
    char artist[160];
    char album[160];
    char playing_num[24];
    char work_mode[24];
    char path[TRACK_PATH_MAX];
    long duration_ms;
    long position_ms;
    int  state;       /* RAW A2 metadata when returned by ipc_get_state() - UNRELIABLE (reports 0 while
                       * playing); never test it for play-state. The main loop overwrites ITS LOCAL copy
                       * with the normalized 2=playing / 1=paused inference (and publishes g_playing). */
    int  have_track;
    int  pos_id;      /* the current row of the player queue (LIST_SONG_0.ID, from a2 song.pos_id); 0 = unknown */
    int  sample_rate; /* Hz, from song_sample_rate */
    int  is_dsd;      /* DSD stream */
    int  is_favorite; /* outer "love" flag */
    int  volume;      /* 0..VOL_MAX, from a714 frames */
    unsigned volume_seq; /* bumps on each volume update (drives the on-screen bar) */
    /* Raw IDs include valid zero; playback_token records item/stop/generation
     * boundaries even between main-loop polls. Title/seek/pause never change it. */
    uint64_t song_id_raw, pos_id_raw, playback_token;
    unsigned song_id_valid, pos_id_valid, is_cue_valid, is_sacd_valid;
    int is_cue, is_sacd, player_confirmed, provisional, title_ready; /* seed is presentation only */
    unsigned player_generation;
    unsigned seq;
    unsigned path_seq;   /* value of seq when path last CHANGED (a track-identity epoch) */
    unsigned pos_seq;    /* value of seq when position was last reported (an a1 frame). pos_seq > path_seq
                          * means the current position was reported AFTER the current track loaded, i.e. it
                          * belongs to this track and not a stale one - used to gate audiobook checkpoints. */
} track_state_t;

typedef struct {
    track_state_t state;
    char player_title[IPC_TITLE_CAPACITY];
    uint64_t evidence_revision; /* title/kind changes, independent of transport epochs */
} ipc_title_event;
/* Main-thread handoff: copied evidence; no controller call under the IPC lock. */
void ipc_get_title_event(ipc_title_event *out);
/* Compare and publish under one lock. On rejection, replace event with latest evidence. */
int ipc_publish_title(ipc_title_event *event, const char *title, int freeze);

/* Scanner records successful commits; only the main owner advances the controller. */
void ipc_note_library_commit(void);
uint64_t ipc_library_epoch(void);
static inline const char *ipc_display_title(const track_state_t *st){
    return st->title;
}

#define VOL_MAX 120   /* device MAX_VOL (sysconfig); a714 VV is on this same 0..120 scale */

int  ipc_start(void);                  /* open queues + start thread; 0=ok */
void ipc_get_state(track_state_t *out);
/* Read-only complete A2 plus last accepted user toggle epoch and its A2 boundary.
 * Raw reply state 0=playing, 1=paused. Short replies carry have_track=0. */
int ipc_resume_snapshot(track_state_t *out, unsigned *serial, unsigned *toggle, unsigned *after_report);
void ipc_seed_state(const track_state_t *s);  /* startup resume-state seed */
#ifdef DISKOS_HOST
/* Renderer input is an explicit player snapshot, including readiness and identity. */
void ipc_host_set_state(const track_state_t *s);
#endif
int  ipc_send_cmd(const char *frame);  /* e.g. "0201000C0000" play/pause; 0=queued, -1=failed */
int  ipc_send_probe(const char *frame);/* background send (state-sync/health): silent, no toast */
#define IPC_SEND_DEFERRED 1 /* automatic intention remains pending, no send error */
int  ipc_confirmed_snapshot(unsigned *generation); /* atomic current-generation evidence */
int  ipc_send_automatic(const char *frame, unsigned generation); /* 0=queued, -1=failed, 1=deferred */
#ifdef DISKOS_TEST_BT_PREPARE
int ipc_prepare_snapshot(track_state_t *, unsigned *serial, unsigned *playing_frames);
/* Queue only while the preparation still owns this generation, track token
 * and main-thread transport epoch. Release additionally requires the exact
 * latest complete paused A2; RX cannot change it between the check and send. */
int ipc_prepare_send(const char *, unsigned generation, uint64_t token, unsigned transport,
                     unsigned report, unsigned playing_frames, int held, int release, unsigned poweroff);
#endif
int  ipc_is_ready(void);               /* 1 once the /ui receive queue is open */
int  ipc_take_send_error(void);        /* 1 (and clears) if a recent send failed */
void ipc_health_check(void);           /* ~30s: detect player-restart queue recreation (fstat identity) */
int  ipc_take_reconnected(void);       /* 1 (and clears) after a recovery reattach -> caller resyncs */
unsigned ipc_rx_frames(void);          /* count of /ui frames received from the player (>0 => player up) */
int  ipc_player_mode(void);            /* last a607 player mode this gen: -1=none, 8=LOCALPLAYER (v2.40 oracle) */
/* Source evidence is independent of local metadata/work_mode strings. Copy under one
 * recovery lock; valid is false while a player reattach is pending. Sequence counts
 * accepted a607 reports; mode_change_sequence counts changes, including departures. */
typedef struct {
    unsigned generation;
    uint64_t sequence, mode_change_sequence;
    uint64_t arm_id, arm_sequence;
    uint64_t adopt_id; /* observation arm that adopted an already selected AirPlay source, else 0 */
    int mode, valid;
} ipc_source_evidence;
void ipc_source_snapshot(ipc_source_evidence *out); /* any thread; no controller callbacks */
/* Main thread requests/cancels; only RX drains and publishes the arm boundary. */
uint64_t ipc_source_arm(unsigned generation); /* 0 = refused; otherwise async token */
void ipc_source_cancel_arm(uint64_t id);
/* Observation, not a transition: arms the same RX drain boundary with no 0657 behind it. Fresh AirPlay source
 * evidence after the boundary (a607 mode 10, or a length-framed a202 with work_mode AIRPLAY) adopts the source and
 * publishes that frame's receiver metadata. 0 = refused (cancelled by ipc_source_cancel_arm). */
uint64_t ipc_observe_arm(unsigned generation);
unsigned ipc_generation(void);         /* bumps on each /ui reattach (player restart) - v2.40 one-shot re-arm */
/* the player's screen frames (aa23: *scr_seq bumps per frame, *on = the latest value) and power-off frames (aa03:
 * *poweroff_seq bumps per frame), with their generation under one lock. Reattach clears both counters
 * and sets *on to -1 (no screen frame yet). Counters also start at 0 at startup. */
void ipc_screen_snapshot(int *on, unsigned *scr_seq, unsigned *poweroff_seq, unsigned *generation);
/* The stock player's own charging report, aa1f (V2.57 mq_player adc_control.c, see ipc.c): exactly "aa1f000C0000" (not
 * charging) or "aa1f000C0001" (charging). The player sends it only when its debounced flag changes and a 0 when the battery
 * reaches 100%, never on request, so a UI restart starts without one. state: -1 = no valid report in this player
 * generation (startup, or after a /ui reattach), 0 = not charging, 1 = charging. at_ms = CLOCK_MONOTONIC ms of the last
 * valid report (0 = none); seq counts valid reports and bad counts ignored malformed aa1f frames (both since startup).
 * Any thread; returns state. Consumers use power_charging() (power.h), never this directly. */
typedef struct { int state; uint64_t at_ms; unsigned seq, bad, generation; } ipc_charge_evidence;
int ipc_charge_snapshot(ipc_charge_evidence *out);
/* Stock still-image conversion: 0810 value 5 + source path, result in the a810 reply. One operation at a time;
 * the reply carries no id, so it completes the operation only while outstanding (PENDING, or UNRESOLVED after the
 * deadline) in the same player generation. A timeout or a /ui reattachment never yields a result by itself: the
 * state is UNRESOLVED (the converter may still write the output) and a new start is refused until one valid
 * same-generation a810 confirms completion; a reattachment alone does not prove the converter stopped. Source must
 * be /tmp/sdcard/CustomCover/<file>.{jpg,jpeg,png,bmp} (gif is not convertible).
 * Checks source components with lstat; no asset modification or DB access. */
#define IPC_CONV_TIMEOUT_MS 30000u
typedef enum { IPC_CONV_IDLE, IPC_CONV_PENDING, IPC_CONV_DONE, IPC_CONV_FAILED, IPC_CONV_UNRESOLVED,
               IPC_CONV_STALE /* poll of an op id that is no longer current */ } ipc_conv_state;
typedef enum { IPC_CONV_START_OK, IPC_CONV_START_BUSY, IPC_CONV_START_PATH, IPC_CONV_START_NOT_READY,
               IPC_CONV_START_SEND } ipc_conv_start_status;
ipc_conv_start_status ipc_conv_start(const char *src, unsigned *op);
/* FAILED: *code = the converter's nonzero result (exact for 4-hex-digit codes and 8-digit -1 forms). */
ipc_conv_state ipc_conv_poll(unsigned op, unsigned *code);
int ipc_conv_release(unsigned op);   /* 0 once DONE/FAILED (or UNRESOLVED from a dead player) is consumed, else -1 */
/* 1 while a conversion is pending, holds an unreleased result, or may still be writing (the start-BUSY rule). */
int ipc_conv_busy(void);
/* Stock's GIF selection: pathless 0810 value 5 ("08100005000C", no reply). Refused (BUSY) while a conversion is
 * busy, so it cannot interleave with one. OK only means the frame was queued. */
typedef enum { IPC_GIF_SELECT_OK, IPC_GIF_SELECT_BUSY, IPC_GIF_SELECT_NOT_READY, IPC_GIF_SELECT_SEND } ipc_gif_select_status;
ipc_gif_select_status ipc_gif_select(void);
/* Apply coordinator owns this reservation until commit/rollback/recovery is resolved.
 * Reselect requires that reservation; 0..4 = system, 5 = saved Custom, no conversion. */
int ipc_apply_reserve(void);
void ipc_apply_release(void);
int ipc_saver_reselect(unsigned style);
#endif
