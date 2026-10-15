/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef IPC_H
#define IPC_H
#include <stdint.h>
#define IPC_TITLE_CAPACITY 256
unsigned ipc_transport_seq(void); /* main-thread transport-attempt epoch */
typedef struct {
    char title[IPC_TITLE_CAPACITY];
    char artist[160];
    char album[160];
    char playing_num[24];
    char work_mode[24];
    char path[256];
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
void ipc_seed_state(const track_state_t *s);  /* startup resume-state seed */
int  ipc_send_cmd(const char *frame);  /* e.g. "0201000C0000" play/pause; 0=queued, -1=failed */
int  ipc_send_probe(const char *frame);/* background send (state-sync/health): silent, no toast */
int  ipc_is_ready(void);               /* 1 once the /ui receive queue is open */
int  ipc_take_send_error(void);        /* 1 (and clears) if a recent send failed */
void ipc_health_check(void);           /* ~30s: detect player-restart queue recreation (fstat identity) */
int  ipc_take_reconnected(void);       /* 1 (and clears) after a recovery reattach -> caller resyncs */
unsigned ipc_rx_frames(void);          /* count of /ui frames received from the player (>0 => player up) */
int  ipc_player_mode(void);            /* last a607 player mode this gen: -1=none, 8=LOCALPLAYER (v2.40 oracle) */
unsigned ipc_generation(void);         /* bumps on each /ui reattach (player restart) - v2.40 one-shot re-arm */
/* the player's screen frames (aa23: *scr_seq bumps per frame, *on = the latest value) and power-off frames (aa03:
 * *poweroff_seq bumps per frame), with their generation under one lock. Reattach clears both counters
 * and sets *on to -1 (no screen frame yet). Counters also start at 0 at startup. */
void ipc_screen_snapshot(int *on, unsigned *scr_seq, unsigned *poweroff_seq, unsigned *generation);
int  ipc_tip_event(unsigned *seq);     /* last a60a TIP_INFO_EVENT (0x0212=connect, 0x0213=disconnect) */
#endif
