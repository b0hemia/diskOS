/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_AIRPLAY_RTSP_H
#define DISKOS_AIRPLAY_RTSP_H
#include <stddef.h>
#include <stdint.h>
#define APS_BODY_MAX 65536u
typedef struct {
    int fd, secure, dead;
    char diag_step[40]; const char *diag_next;
    const char *diag_cause; int diag_errno;
    int diag_io, diag_status, diag_rc; size_t diag_n;
    unsigned seq;
    uint64_t tx_counter, rx_counter;
    uint8_t write_key[32], read_key[32];
    uint8_t raw[4096], plain[1024]; size_t raw_at, raw_n, plain_at, plain_n;
    int (*cancel)(void *); void *ctx;
    char session[128], uri[160], identity[96], dacp_id[17], active_remote[11];
} aps_rtsp;
typedef struct { int status; char headers[8192]; uint8_t body[APS_BODY_MAX]; size_t n; } aps_reply;
int aps_rtsp_diag(aps_rtsp *, const char *step, int result, const aps_reply *);
int64_t aps_now_ms(void);
void aps_rtsp_init(aps_rtsp *, int fd, int (*cancel)(void *), void *);
int aps_rtsp_connect(aps_rtsp *, const char *ipv4, unsigned port, unsigned ms);
void aps_rtsp_close(aps_rtsp *);
/* Enable only AFTER authenticated pair-verify. Counters are never reset on
 * FLUSH, pause, seek, queue change or request failure. Error poisons the stream. */
int aps_rtsp_secure(aps_rtsp *, const uint8_t write_key[32], const uint8_t read_key[32]);
int aps_rtsp_request(aps_rtsp *, const char *method, const char *uri, const char *headers,
                     const void *body, size_t n, aps_reply *, unsigned timeout_ms);
int aps_rtsp_header(const aps_reply *, const char *key, char *out, size_t cap);
/* Pure HAP records, useful to protocol simulators. Counter increments only on
 * success; authentication failure clears output. Exhaustion always refuses. */
int aps_record_seal(const uint8_t key[32], uint64_t *, const void *, size_t, uint8_t out[1042]);
int aps_record_open(const uint8_t key[32], uint64_t *, const void *, size_t, uint8_t out[1024]);
#endif
