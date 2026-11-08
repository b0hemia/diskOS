/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_AIRPLAY_IPC_H
#define DISKOS_AIRPLAY_IPC_H
#include "remote_discovery.h"
#include "playback.h"
#include <stdint.h>
#define APS_WORKER_NAME "diskos-airplay-worker"
#define APS_WORKER_MARKER "--diskos-airplay-worker"
enum { APS_TARGET = 1, APS_PIN, APS_GO, APS_NEXT_FILE, APS_COMMAND, APS_CANCEL,
       APS_NEED_PIN, APS_READY, APS_STARTED, APS_VIEW, APS_NEED_NEXT, APS_ENDED,
       APS_FAILED, APS_STOPPED, APS_NOTICE, APS_ART, APS_FORMAT, APS_STREAM_READY };
enum { APS_ERR_CONNECT = 1, APS_ERR_PIN, APS_ERR_VERIFY, APS_ERR_BUSY,
       APS_ERR_TIMEOUT, APS_ERR_REMOVED, APS_ERR_FORMAT, APS_ERR_STREAM, APS_ERR_STORAGE, APS_ERR_STOP, APS_ERR_PASSWORD,
       APS_ERR_DECODER, /* the stock helper could not be prepared: retry, not a format verdict */
       APS_ERR_START };
#define APS_VOLUME_QUERY 100u
#define APS_MUTE 101u
#define APS_REMOTE_STOP 102u
#define APS_QUEUE_JUMP 103u
#define APS_QUEUE_MODE 104u
#define APS_QUEUE_SKIP 105u
#define APS_QUEUE_REFUSED 106u
#define APS_QUEUE_REMOVE 107u
#define APS_QUEUE_MOVE_NEXT 108u
#define APS_QUEUE_CHANGED 109u
#define APS_VOLUME_OBSERVED 110u
/* Private same-image ABI, never a network protocol. Every byte is initialized;
 * receiver requires exact length, magic, attempt, type and descriptor count.
 * Only GO/NEXT_FILE/ART carry one approved regular descriptor. */
typedef struct {
    uint32_t magic, attempt, type, command, qgen;
    uint64_t item, occurrence, art_request; /* APS_VIEW: original file request token */
    int64_t position_ms;
    rd_receiver target;
    pb_view view;
    char pin[9]; int play, rate, bits, error, local_id, local_ord, volume, have_volume;
} aps_message;
int aps_ipc_send(int socket, uint32_t attempt, aps_message *, int fd);
/* 1 message; 0 EAGAIN; -1 closed, malformed or IO. Unexpected rights are closed.
 * Stale messages are discarded with their descriptors and return 0. */
int aps_ipc_receive(int socket, uint32_t attempt, aps_message *, int *fd);
/* Same result and descriptor handling, with a diagnostic on -1. */
int aps_ipc_receive_reason(int socket, uint32_t attempt, aps_message *, int *fd, const char **reason);
int aps_worker_dispatch(int argc, char **argv, int *status);
int aps_worker_main(int ctl, uint32_t attempt);
#endif
