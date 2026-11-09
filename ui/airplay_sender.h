/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_AIRPLAY_SENDER_H
#define DISKOS_AIRPLAY_SENDER_H
#include "trackpath.h"
#include "airplay_queue.h"
#include "cast_handoff.h"
#include <sys/types.h>
enum { APSS_IDLE, APSS_PREPARING, APSS_RUNNING, APSS_ENDING };
typedef struct {
    const char *sd_root;
    int (*pause)(void);
    int (*teardown)(unsigned generation);
    void (*say)(ho_msg);
    void (*notice)(pb_cmd);
} aps_config;
typedef struct {
    ho *handoff; aps_config cfg; aps_queue *queue; void *preflight;
    pid_t pid, decoder_pid; int decoder_ctl, decoder_closing, decoder_unknown, worker_reaped; int ctl, phase, lease, owner, pin_wanted, error, terminated, faulted, stop_confirmed;
    uint32_t serial, now, deadline, terminate_at, kill_at, fault_at;
    unsigned skipped, unsupported_choice, queue_changed;
    int stream_ready, format_pending, input_mode, input_pos_id;
    unsigned input_generation; uint64_t input_token; char input_path[TRACK_PATH_MAX];
    unsigned player_generation; int source, mode, pos_id, local_id, local_ord, rate, bits, forget_next, volume, have_volume;
    rd_receiver target; pb_remote_state view;
} aps_sender;
void aps_sender_init(aps_sender *, ho *, const aps_config *);
int aps_sender_begin(aps_sender *, const rd_receiver *, const ho_local *, uint32_t now);
void aps_sender_tick(aps_sender *, const ho_local *, uint32_t now);
int aps_sender_active(const aps_sender *);
void aps_sender_cancel(aps_sender *, uint32_t now);
int aps_sender_pin(aps_sender *, const char *);
int aps_sender_volume_query(aps_sender *);
int aps_sender_command(aps_sender *, pb_cmd, long arg);
int aps_sender_volume_step(aps_sender *, int direction);
#endif
