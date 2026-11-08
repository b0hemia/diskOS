/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_rtsp.h"
#include "airplay_crypto.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
int64_t aps_now_ms(void){
    struct timespec t; if(clock_gettime(CLOCK_MONOTONIC, &t)) return -1;
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
int aps_record_seal(const uint8_t key[32], uint64_t *counter, const void *p, size_t n, uint8_t out[1042]){
    uint8_t nonce[12];
    if(!n || n > 1024 || *counter == UINT64_MAX) return -1;
    out[0] = (uint8_t)n; out[1] = (uint8_t)(n >> 8); aps_nonce(nonce, *counter);
    if(aps_seal(key, nonce, out, 2, p, n, out + 2, out + 2 + n)) return -1;
    ++*counter; return (int)(n + 18);
}
int aps_record_open(const uint8_t key[32], uint64_t *counter, const void *p, size_t n, uint8_t out[1024]){
    const uint8_t *b = p; uint8_t nonce[12];
    if(n < 19 || n > 1042 || *counter == UINT64_MAX) return -1;
    size_t k = b[0] | (size_t)b[1] << 8;
    if(!k || k > 1024 || n != k + 18) return -1;
    aps_nonce(nonce, *counter);
    if(aps_open(key, nonce, b, 2, b + 2, k, b + 2 + k, out)) return -1;
    ++*counter; return (int)k;
}
void aps_rtsp_init(aps_rtsp *r, int fd, int (*cancel)(void *), void *ctx){
    memset(r, 0, sizeof *r); r->fd = fd; r->cancel = cancel; r->ctx = ctx;
    strcpy(r->identity, "Snowsky Disc / diskOS"); strcpy(r->uri, "*");
    if(fd >= 0){ int f = fcntl(fd, F_GETFL); if(f >= 0) fcntl(fd, F_SETFL, f | O_NONBLOCK); }
}
static int wait_fd(aps_rtsp *r, int fd, short events, int64_t end){
    while(!r->dead){
        if(r->cancel && r->cancel(r->ctx)){ r->diag_cause = "cancelled"; return -1; }
        int64_t left = end - aps_now_ms(); if(left <= 0){ r->diag_cause = "timeout"; return -1; }
        struct pollfd p = { fd, events, 0 };
        int n = poll(&p, 1, left > 20 ? 20 : (int)left);
        if(n < 0 && errno == EINTR) continue;
        if(n < 0 || (p.revents & (POLLERR | POLLNVAL))){ r->diag_cause = "poll failed"; return -1; }
        if(n && (p.revents & (events | POLLHUP))) return 0;
    }
    return -1;
}
int aps_rtsp_connect(aps_rtsp *r, const char *ip, unsigned port, unsigned ms){
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    if(r->fd >= 0 || !port || port > 65535 || !ms || ms > 15000 || inet_pton(AF_INET, ip, &a.sin_addr) != 1) return -1;
    uint32_t host = ntohl(a.sin_addr.s_addr);
    if(host >> 24 == 0 || host >> 24 == 127 || host >> 24 >= 224) return -1;
    /* The cancellation callback services other channels. Do not publish this
     * socket until it is connected, or it can service an unfinished channel. */
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0); if(fd < 0) return -1;
    int rc = connect(fd, (struct sockaddr *)&a, sizeof a);
    if(rc && errno != EINPROGRESS) goto fail;
    if(rc){
        int error = 0; socklen_t n = sizeof error;
        if(wait_fd(r, fd, POLLOUT, aps_now_ms() + ms) || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &n) || error) goto fail;
    }
    r->fd = fd;
    return 0;
fail: close(fd); aps_rtsp_close(r); return -1;
}
void aps_rtsp_close(aps_rtsp *r){
    if(r->fd >= 0) close(r->fd);
    r->fd = -1; r->dead = 1;
    aps_wipe(r->write_key, 32); aps_wipe(r->read_key, 32); aps_wipe(r->plain, sizeof r->plain);
}
int aps_rtsp_secure(aps_rtsp *r, const uint8_t write_key[32], const uint8_t read_key[32]){
    if(r->secure || r->dead || r->raw_n != r->raw_at || r->plain_n != r->plain_at) return -1;
    memcpy(r->write_key, write_key, 32); memcpy(r->read_key, read_key, 32); r->secure = 1; return 0;
}
static int raw_read(aps_rtsp *r, uint8_t *out, size_t n, int64_t end){
    while(n){
        if(r->raw_at == r->raw_n){
            if(wait_fd(r, r->fd, POLLIN, end)) return -1;
            ssize_t k = recv(r->fd, r->raw, sizeof r->raw, 0);
            if(k < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            if(k <= 0){ r->diag_cause = k == 0 ? "peer closed" : "receive failed"; r->diag_errno = k < 0 ? errno : 0; return -1; }
            r->raw_at = 0; r->raw_n = (size_t)k;
        }
        size_t k = r->raw_n - r->raw_at; if(k > n) k = n;
        memcpy(out, r->raw + r->raw_at, k); out += k; n -= k; r->raw_at += k;
    }
    return 0;
}
static int plain_read(aps_rtsp *r, void *out, size_t n, int64_t end){
    if(!r->secure) return raw_read(r, out, n, end);
    uint8_t *p = out;
    while(n){
        if(r->plain_at == r->plain_n){
            uint8_t b[1042];
            if(raw_read(r, b, 2, end)) return -1;
            size_t k = b[0] | (size_t)b[1] << 8;
            if(!k || k > 1024 || raw_read(r, b + 2, k + 16, end)) return -1;
            int result = aps_record_open(r->read_key, &r->rx_counter, b, k + 18, r->plain);
            if(result < 0){ r->diag_cause = "record authentication failed"; return -1; }
            r->plain_at = 0; r->plain_n = (size_t)result;
        }
        size_t k = r->plain_n - r->plain_at; if(k > n) k = n;
        memcpy(p, r->plain + r->plain_at, k); p += k; n -= k; r->plain_at += k;
    }
    return 0;
}
static int raw_write(aps_rtsp *r, const void *data, size_t n, int64_t end){
    const uint8_t *p = data;
    while(n){
        if(wait_fd(r, r->fd, POLLOUT, end)) return -1;
        ssize_t k = send(r->fd, p, n, MSG_NOSIGNAL);
        if(k < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if(k <= 0){ r->diag_cause = "send failed"; r->diag_errno = k < 0 ? errno : 0; return -1; }
        p += k; n -= (size_t)k;
    }
    return 0;
}
static int plain_write(aps_rtsp *r, const void *data, size_t n, int64_t end){
    if(!r->secure) return raw_write(r, data, n, end);
    const uint8_t *p = data;
    while(n){
        size_t k = n > 1024 ? 1024 : n; uint8_t b[1042];
        int count = aps_record_seal(r->write_key, &r->tx_counter, p, k, b);
        if(count < 0 || raw_write(r, b, (size_t)count, end)) return -1;
        p += k; n -= k;
    }
    return 0;
}
int aps_rtsp_header(const aps_reply *r, const char *key, char *out, size_t cap){
    size_t kn = strlen(key); int found = 0;
    const char *p = strstr(r->headers, "\r\n"); if(!p) return -1;
    for(p += 2; *p && strncmp(p, "\r\n", 2);){
        const char *end = strstr(p, "\r\n"), *colon = strchr(p, ':');
        if(!end || !colon || colon >= end) return -1;
        if((size_t)(colon - p) == kn && !strncasecmp(p, key, kn)){
            if(found++) return -1;
            p = colon + 1; while(p < end && (*p == ' ' || *p == '\t')) p++;
            size_t n = (size_t)(end - p);
            if(n >= cap) return -1;
            memcpy(out, p, n); out[n] = 0;
        }
        p = end + 2;
    }
    return found;
}
static int decimal(const char *s, unsigned *out){
    unsigned v = 0; if(!*s) return -1;
    for(; *s; s++){
        if(*s < '0' || *s > '9' || v > (UINT_MAX - (unsigned)(*s - '0')) / 10) return -1;
        v = v * 10 + (unsigned)(*s - '0');
    }
    *out = v; return 0;
}
static int request(aps_rtsp *r, const char *method, const char *uri, const char *extra,
                     const void *body, size_t n, aps_reply *reply, unsigned ms){
    char head[2048], value[128], remote[96] = {0}; unsigned length = 0, seq; size_t hn = 0;
    if(r->fd < 0 || r->dead || !ms || ms > 15000 || n > 256u * 1024u || (!body && n) || r->seq == UINT_MAX) return -1;
    if(strpbrk(method, "\r\n ") || strpbrk(uri, "\r\n ") || strpbrk(r->identity, "\r\n")) return -1;
    seq = ++r->seq;
    if(r->dacp_id[0]) snprintf(remote, sizeof remote, "DACP-ID: %s\r\nActive-Remote: %s\r\n", r->dacp_id, r->active_remote);
    int count = snprintf(head, sizeof head, "%s %s RTSP/1.0\r\nCSeq: %u\r\nUser-Agent: %s\r\n%s%s%s%s%sContent-Length: %zu\r\n\r\n",
                         method, uri, seq, r->identity, r->session[0] ? "Session: " : "", r->session,
                         r->session[0] ? "\r\n" : "", remote, extra ? extra : "", n);
    if(count < 0 || (size_t)count >= sizeof head) return -1;
    int64_t end = aps_now_ms() + ms;
    if(plain_write(r, head, (size_t)count, end) || (n && plain_write(r, body, n, end))) goto fail;
    memset(reply, 0, sizeof *reply);
    while(hn + 1 < sizeof reply->headers){
        char c;
        if(plain_read(r, &c, 1, end) || !c || ((unsigned char)c < 32 && c != '\r' && c != '\n' && c != '\t')) goto fail;
        reply->headers[hn++] = c;
        if(hn >= 4 && !memcmp(reply->headers + hn - 4, "\r\n\r\n", 4)) break;
    }
    if(hn < 4 || memcmp(reply->headers + hn - 4, "\r\n\r\n", 4)) goto fail;
    if(strncmp(reply->headers, "RTSP/1.0 ", 9) && strncmp(reply->headers, "HTTP/1.1 ", 9)) goto fail;
    if(reply->headers[9] < '1' || reply->headers[9] > '5' || reply->headers[10] < '0' || reply->headers[10] > '9' ||
       reply->headers[11] < '0' || reply->headers[11] > '9' || reply->headers[12] != ' ') goto fail;
    reply->status = (reply->headers[9] - '0') * 100 + (reply->headers[10] - '0') * 10 + reply->headers[11] - '0';
    int h = aps_rtsp_header(reply, "Content-Length", value, sizeof value);
    if(h < 0 || (h && decimal(value, &length)) || length > APS_BODY_MAX) goto fail;
    h = aps_rtsp_header(reply, "Transfer-Encoding", value, sizeof value); if(h) goto fail;
    h = aps_rtsp_header(reply, "CSeq", value, sizeof value); unsigned got;
    if(h != 1 || decimal(value, &got) || got != seq) goto fail;
    if(plain_read(r, reply->body, length, end)) goto fail;
    reply->n = length;
    h = aps_rtsp_header(reply, "Session", value, sizeof value); if(h < 0) goto fail;
    if(h){
        char *semi = strchr(value, ';'); if(semi) *semi = 0;
        if(strpbrk(value, "\r\n ")) goto fail;
        strcpy(r->session, value);
    }
    return 0;
fail:
    r->dead = 1; aps_wipe(reply->body, sizeof reply->body); reply->n = 0; return -1;
}
int aps_rtsp_diag(aps_rtsp *r, const char *step, int result, const aps_reply *reply){
    snprintf(r->diag_step, sizeof r->diag_step, "%.39s", step);
    r->diag_rc = result;
    if(reply){ r->diag_io = result; r->diag_status = reply->status; r->diag_n = reply->n; }
    fprintf(stderr, "AirPlay step=%.39s io=%d status=%d bytes=%zu rc=%d cause=%s errno=%d\n",
        r->diag_step, r->diag_io, r->diag_status, r->diag_n, result,
        reply && r->diag_cause ? r->diag_cause : "unverified",
        reply ? r->diag_errno : 0);
    return result;
}
int aps_rtsp_request(aps_rtsp *r, const char *method, const char *uri, const char *extra,
                     const void *body, size_t n, aps_reply *reply, unsigned ms){
    const char *hint = r->diag_next; r->diag_next = NULL;
    r->diag_cause = r->dead ? "already dead" : "unverified"; r->diag_errno = 0;
    reply->status = 0; reply->n = 0;
    int result = request(r, method, uri, extra, body, n, reply, ms);
    if(!result) r->diag_cause = "ok";
    if(r->secure){
        char step[40];
        snprintf(step, sizeof step, "%.16s%.22s", method, uri[0] == '/' ? uri : "");
        aps_rtsp_diag(r, hint ? hint : step, result, reply);
    }
    return result;
}
