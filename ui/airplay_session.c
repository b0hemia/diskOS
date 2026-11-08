/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_session.h"
#include "device_names.h"
#include "airplay_crypto.h"
#include "version.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#define STEP(name, call) aps_rtsp_diag(&s->rtsp, name, (call), NULL)
unsigned aps_quality_bits(uint64_t formats, unsigned source, unsigned source_bits, unsigned *bits){
    if(!formats){ *bits = 16; return 44100; }
    (void)source_bits;
    /* Keep precision for later queue items without restarting the stream. */
    unsigned preferred = source && source % 48000 == 0 ? 48000 : 44100;
    for(unsigned i = 0; i < 2; i++){
        unsigned rate = i ? (preferred == 48000 ? 44100 : 48000) : preferred;
        unsigned bit = rate == 48000 ? 20 : 18;
        if(formats & (UINT64_C(1) << (bit + 1))){ *bits = 24; return rate; }
        if(formats & (UINT64_C(1) << bit)){ *bits = 16; return rate; }
    }
    return 0;
}
unsigned aps_quality(uint64_t formats, unsigned source){ unsigned bits; return aps_quality_bits(formats, source, 16, &bits); }
static uint64_t wall_ns(void){
    struct timespec t; if(clock_gettime(CLOCK_REALTIME, &t)) return 0;
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
/* Use a monotonic projection after initial wall-clock sampling. An NTP/RTC
 * adjustment must not jump the media clock in the middle of a song. */
static uint64_t media_ntp(const aps_session *s){
    int64_t delta = aps_now_ms() - s->mono_ms;
    return aps_ntp((uint64_t)s->wall_ns + (uint64_t)(delta > 0 ? delta : 0) * UINT64_C(1000000));
}
void aps_session_init(aps_session *s, int (*cancel)(void *), void *ctx){
    memset(s, 0, sizeof *s); s->data = s->control = s->timing = -1;
    aps_remote_init(&s->remote);
    aps_mrp_init(&s->mrp);
    dn_sender_copy(s->sender_name);
    strcpy(s->mrp.display_name,s->sender_name);
    aps_rtsp_init(&s->rtsp, -1, cancel, ctx); aps_rtsp_init(&s->events, -1, cancel, ctx);
    snprintf(s->rtsp.identity, sizeof s->rtsp.identity, "%s / diskOS %s", s->sender_name, DISKOS_VERSION);
    s->wall_ns = (int64_t)wall_ns(); s->mono_ms = aps_now_ms();
}
static int okay(aps_session *s, int result){
    if(result || s->reply.status != 200){
        s->error = s->reply.status == 453 || s->reply.status == 409 ? APS_ERR_BUSY : APS_ERR_CONNECT;
        return -1;
    }
    return 0;
}
int aps_session_pair_post(void *ctx, const char *path, const aps_tlv *tx, aps_tlv *rx){
    aps_session *s = ctx; rx->n = 0;
    s->reply.status = 0; s->reply.n = 0;
    int result = aps_rtsp_request(&s->rtsp, "POST", path,
        "Content-Type: application/octet-stream\r\nX-Apple-HKP: 3\r\n", tx->b, tx->n, &s->reply, 8000);
    uint8_t state = 0, error = 0;
    int hs = result ? 0 : aps_tlv_get(s->reply.body, s->reply.n, 6, &state, 1);
    int he = result ? 0 : aps_tlv_get(s->reply.body, s->reply.n, 7, &error, 1);
    fprintf(stderr, "AirPlay %.32s: io=%d status=%d bytes=%zu state=%d error=%d\n",
        path, result, s->reply.status, s->reply.n, hs == 1 ? state : -1, he == 1 ? error : -1);
    if(okay(s, result)) return -1;
    if(s->reply.n > sizeof rx->b) return -1;
    memcpy(rx->b, s->reply.body, s->reply.n); rx->n = s->reply.n; return 0;
}
static int key_dir(void){
    int u = open("/usr", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC); if(u < 0) return -1;
    int d = openat(u, "data", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC); close(u); if(d < 0) return -1;
    if(mkdirat(d, ".diskos-airplay", 0700) && errno != EEXIST){ close(d); return -1; }
    int k = openat(d, ".diskos-airplay", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC); close(d);
    struct stat st;
    if(k >= 0 && (fstat(k, &st) || st.st_uid != geteuid() || (st.st_mode & 0777) != 0700)){ close(k); k = -1; }
    return k;
}
int aps_session_forget(const char *id){
    int d = key_dir(); if(d < 0) return -1;
    int rc = aps_store_forget(d, id); close(d); return rc;
}
static int udp(uint16_t *port){
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0); if(fd < 0) return -1;
    struct sockaddr_in a = { .sin_family = AF_INET }; socklen_t n = sizeof a;
    if(bind(fd, (struct sockaddr *)&a, sizeof a) || getsockname(fd, (struct sockaddr *)&a, &n)){ close(fd); return -1; }
    *port = ntohs(a.sin_port); return fd;
}
static void peer(aps_session *s, struct sockaddr_in *a, uint16_t port){
    memset(a, 0, sizeof *a); a->sin_family = AF_INET; a->sin_port = htons(port);
    inet_pton(AF_INET, s->target.addr, &a->sin_addr);
}
static void formats(plist_t info, aps_session *s){
    plist_t list = plist_dict_get_item(info, "audioFormats");
    if(!list || plist_get_node_type(list) != PLIST_ARRAY) return;
    uint32_t n = plist_array_get_size(list); if(n > 32) return;
    for(uint32_t i = 0; i < n; i++){
        plist_t entry = plist_array_get_item(list, i); uint64_t type, value;
        if(!aps_plist_uint(entry, "type", &type) && type == 96 && !aps_plist_uint(entry, "audioInputFormats", &value)) s->formats |= value;
    }
}
int aps_session_info(aps_session *s, const void *body, size_t n){
    plist_t info = aps_plist_read(body, n); if(!info) return -1;
    uint64_t flags = 0; (void)aps_plist_uint(info, "statusFlags", &flags);
    if((flags | (s->target.have_flags ? s->target.flags : 0)) & (UINT64_C(1) << 17)){
        plist_free(info); s->error = APS_ERR_BUSY; return -1;
    }
    s->formats = 0; formats(info, s); plist_free(info); return 0;
}
int aps_session_prepare(aps_session *s, const rd_receiver *target, const aps_pair_io *pair_io){
    s->target = *target; s->legacy = target->protocol == RD_RAOP; s->error = APS_ERR_CONNECT;
    if(s->legacy && target->password){ s->error = APS_ERR_PASSWORD; return -1; }
    if(aps_rtsp_connect(&s->rtsp, target->addr, target->port, 5000)) return -1;
    struct sockaddr_in local; socklen_t ln = sizeof local; char ip[16];
    if(getsockname(s->rtsp.fd, (struct sockaddr *)&local, &ln) || !inet_ntop(AF_INET, &local.sin_addr, ip, sizeof ip)) return -1;
    if(s->remote.listener < 0 && aps_remote_start(&s->remote, ip, target->addr)) return -1;
    strcpy(s->rtsp.dacp_id, s->remote.id); strcpy(s->rtsp.active_remote, s->remote.token);
    uint32_t random; if(aps_random(&random, sizeof random)) return -1;
    snprintf(s->rtsp.uri, sizeof s->rtsp.uri, "rtsp://%s/%u", ip, random);
    s->timing = udp(&s->timing_port); s->control = udp(&s->control_port); s->data = udp(&s->data_port);
    if(s->timing < 0 || s->control < 0 || s->data < 0) return -1;
    peer(s, &s->timing_peer, 0);
    if(s->legacy){
        s->legacy_rsa = !target->have_encryption || (target->encryption & 2);
        if(target->have_encryption && !(target->encryption & 3)) return -1;
        s->prepared = 1; return 0;
    }
    if(okay(s, aps_rtsp_request(&s->rtsp, "GET", "/info", NULL, NULL, 0, &s->reply, 5000))) return -1;
    if(aps_session_info(s, s->reply.body, s->reply.n)) return -1;
    aps_identity k = {0}; uint8_t wk[32], rk[32]; int rc = -1, dir = -1, fresh = 0;
    if(!target->stable_id || (dir = key_dir()) < 0){ s->error = APS_ERR_STORAGE; goto done; }
    int loaded = aps_store_load(dir, target->id, &k);
    if(loaded < 0){ s->error = APS_ERR_STORAGE; goto done; }
    if(loaded == 1){
        int paired = aps_pair_setup(pair_io, &k);
        if(paired){ if(s->error != APS_ERR_BUSY && s->error != APS_ERR_TIMEOUT) s->error = paired == APS_PAIR_AUTH ? APS_ERR_PIN : APS_ERR_VERIFY; goto done; }
        fresh = 1;
        /* Setup and verify use separate control connections. */
        aps_rtsp_close(&s->rtsp); int (*cancel)(void *) = s->rtsp.cancel; void *ctx = s->rtsp.ctx;
        char uri[160]; strcpy(uri, s->rtsp.uri); aps_rtsp_init(&s->rtsp, -1, cancel, ctx); strcpy(s->rtsp.uri, uri);
        strcpy(s->rtsp.dacp_id, s->remote.id); strcpy(s->rtsp.active_remote, s->remote.token);
        snprintf(s->rtsp.identity, sizeof s->rtsp.identity, "%s / diskOS %s", s->sender_name, DISKOS_VERSION);
        if(aps_rtsp_connect(&s->rtsp, target->addr, target->port, 5000)) goto done;
    }
    int verified = aps_pair_verify(pair_io, &k, wk, rk, s->audio_key);
    if(verified){ s->error = verified == APS_PAIR_AUTH ? APS_ERR_REMOVED : APS_ERR_VERIFY; goto done; }
    s->verified = 1;
    if(STEP("control-secure", aps_rtsp_secure(&s->rtsp, wk, rk))) goto done;
    if(fresh && STEP("pair-save", aps_store_save(dir, target->id, &k))){ s->error = APS_ERR_STORAGE; goto done; }
    if(STEP("rtp-init", aps_rtp_begin(&s->rtp, 1, s->audio_key))) goto done;
    plist_t p = plist_new_dict(); if(STEP("session-plist", p ? 0 : -1)) goto done;
    uint8_t hash[64]; aps_identity session = {0}; char device_id[18];
    if(STEP("sender-id", aps_hash(k.client_id, 36, hash)) || STEP("session-id", aps_identity_new(&session))){ plist_free(p); goto done; }
    hash[0] = (hash[0] & 0xfe) | 2; /* stable locally administered sender ID */
    snprintf(device_id, sizeof device_id, "%02X:%02X:%02X:%02X:%02X:%02X", hash[0],hash[1],hash[2],hash[3],hash[4],hash[5]);
    char session_id[37]; strcpy(session_id, session.client_id); aps_wipe(&session, sizeof session);
    aps_plist_text(p, "deviceID", device_id); aps_plist_text(p, "sessionUUID", session_id);
    aps_plist_text(p, "name", s->sender_name); aps_plist_text(p, "model", "Snowsky Disc");
    aps_plist_text(p, "osVersion", DISKOS_VERSION); aps_plist_text(p, "timingProtocol", "NTP");
    aps_plist_int(p, "timingPort", s->timing_port);
    s->rtsp.diag_next = "SETUP-session";
    int result = aps_plist_request(&s->rtsp, "SETUP", p, &s->reply, 8000); plist_free(p);
    if(okay(s, result)) goto done;
    p = aps_plist_read(s->reply.body, s->reply.n); if(STEP("session-reply", p ? 0 : -1)) goto done;
    uint16_t tp, ep;
    if(!aps_plist_port(p, "timingPort", &tp)) peer(s, &s->timing_peer, tp);
    if(!aps_plist_port(p, "eventPort", &ep)){
        /* Pair-verify shared secret, reversed for the receiver-originated channel. */
        if(STEP("event-write-key", aps_kdf(s->audio_key, 32, "Events-Salt", "Events-Read-Encryption-Key", wk)) ||
           STEP("event-read-key", aps_kdf(s->audio_key, 32, "Events-Salt", "Events-Write-Encryption-Key", rk))){ plist_free(p); goto done; }
        if(STEP("event-connect", aps_rtsp_connect(&s->events, target->addr, ep, 2000)) ||
           STEP("event-secure", aps_rtsp_secure(&s->events, wk, rk))){ plist_free(p); goto done; }
    }
    plist_free(p);
    if(STEP("mrp-open", aps_mrp_open(&s->mrp, &s->rtsp, &s->reply, target->addr, s->audio_key, k.client_id, session_id))) goto done;
    s->prepared = 1; rc = 0;
done:
    if(dir >= 0) close(dir);
    aps_wipe(&k, sizeof k); aps_wipe(wk, sizeof wk); aps_wipe(rk, sizeof rk);
    if(rc) aps_wipe(s->audio_key, sizeof s->audio_key);
    return rc;
}
static int legacy_start(aps_session *s){
    char body[1024], headers[256], ip[16]; struct sockaddr_in local; socklen_t ln = sizeof local;
    if(aps_rtp_begin(&s->rtp, 0, NULL) || getsockname(s->rtsp.fd, (struct sockaddr *)&local, &ln) ||
       !inet_ntop(AF_INET, &local.sin_addr, ip, sizeof ip)) return -1;
    int n = snprintf(body, sizeof body,
        "v=0\r\no=diskOS %u 0 IN IP4 %s\r\ns=%s\r\nc=IN IP4 %s\r\nt=0 0\r\n"
        "m=audio 0 RTP/AVP 96\r\na=rtpmap:96 AppleLossless\r\na=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100\r\n",
        s->rtp.ssrc, ip, s->sender_name, s->target.addr);
    char announce[128] = "Content-Type: application/sdp\r\n";
    if(s->legacy_rsa){
        char wrapped[345], iv[25], challenge[25];
        if(aps_raop_create(&s->legacy_key, wrapped, iv, challenge)) return -1;
        int extra = snprintf(body + n, sizeof body - (size_t)n, "a=rsaaeskey:%s\r\na=aesiv:%s\r\n", wrapped, iv);
        if(extra < 0 || (size_t)extra >= sizeof body - (size_t)n) return -1;
        n += extra;
        snprintf(announce, sizeof announce, "Content-Type: application/sdp\r\nApple-Challenge: %s\r\n", challenge);
        memcpy(s->rtp.key, s->legacy_key.key, 16); memcpy(s->rtp.iv, s->legacy_key.iv, 16); s->rtp.secure = 2;
    }
    if(okay(s, aps_rtsp_request(&s->rtsp, "ANNOUNCE", s->rtsp.uri, announce, body, (size_t)n, &s->reply, 5000))) return -1;
    if(s->legacy_rsa){
        char response[345]; struct in_addr receiver;
        if(aps_rtsp_header(&s->reply, "Apple-Response", response, sizeof response) != 1 || inet_pton(AF_INET, s->target.addr, &receiver) != 1 ||
           aps_raop_check(&s->legacy_key, response, (const uint8_t *)&receiver.s_addr)) return -1;
    }
    snprintf(headers, sizeof headers, "Transport: RTP/AVP/UDP;unicast;mode=record;control_port=%u;timing_port=%u\r\n", s->control_port, s->timing_port);
    if(okay(s, aps_rtsp_request(&s->rtsp, "SETUP", s->rtsp.uri, headers, NULL, 0, &s->reply, 5000))) return -1;
    char transport[512]; if(aps_rtsp_header(&s->reply, "Transport", transport, sizeof transport) != 1) return -1;
    unsigned dp = 0, cp = 0, tp = 0;
    char *p = strstr(transport, "server_port="); if(p) dp = (unsigned)strtoul(p + 12, NULL, 10);
    p = strstr(transport, "control_port="); if(p) cp = (unsigned)strtoul(p + 13, NULL, 10);
    p = strstr(transport, "timing_port="); if(p) tp = (unsigned)strtoul(p + 12, NULL, 10);
    if(!dp || dp > 65535 || !cp || cp > 65535 || !tp || tp > 65535) return -1;
    peer(s, &s->data_peer, (uint16_t)dp); peer(s, &s->control_peer, (uint16_t)cp); peer(s, &s->timing_peer, (uint16_t)tp);
    return 0;
}
int aps_session_start(aps_session *s, unsigned source, unsigned source_bits){
    if(STEP("start-state", !s->prepared || s->started ? -1 : 0)) return -1;
    s->bits = 16; s->rate = s->legacy ? 44100 : aps_quality_bits(s->formats, source, source_bits, &s->bits); if(STEP("audio-format", s->rate ? 0 : -1)) return -1;
    s->packet_frames = s->bits == 24 ? 220 : APS_PACKET_FRAMES;
    if(s->legacy){ if(legacy_start(s)) return -1; }
    if(okay(s, aps_rtsp_request(&s->rtsp, "RECORD", s->rtsp.uri, NULL, NULL, 0, &s->reply, 5000))) return -1;
    if(!s->legacy){
        plist_t p = plist_new_dict(), array = plist_new_array(), stream = plist_new_dict();
        if(STEP("audio-plist", !p || !array || !stream ? -1 : 0)){ plist_free(p); plist_free(array); plist_free(stream); return -1; }
        aps_plist_int(stream, "type", 96); aps_plist_int(stream, "ct", 2);
        aps_plist_int(stream, "audioFormat", UINT64_C(1) << ((s->rate == 48000 ? 20 : 18) + (s->bits == 24 ? 1 : 0)));
        aps_plist_int(stream, "sr", s->rate); aps_plist_int(stream, "spf", s->packet_frames);
        aps_plist_int(stream, "dataPort", s->data_port); aps_plist_int(stream, "controlPort", s->control_port);
        aps_plist_int(stream, "latencyMin", s->rate / 4); aps_plist_int(stream, "latencyMax", s->rate * 2);
        aps_plist_int(stream, "streamConnectionID", s->rtp.ssrc); aps_plist_text(stream, "audioMode", "default");
        plist_dict_set_item(stream, "isMedia", plist_new_bool(1));
        plist_dict_set_item(stream, "shk", plist_new_data((const char *)s->audio_key, 32));
        plist_array_append_item(array, stream); plist_dict_set_item(p, "streams", array);
        s->rtsp.diag_next = "SETUP-audio";
        int r = aps_plist_request(&s->rtsp, "SETUP", p, &s->reply, 8000); plist_free(p); if(okay(s, r)) return -1;
        p = aps_plist_read(s->reply.body, s->reply.n); if(STEP("audio-reply", p ? 0 : -1)) return -1;
        array = plist_dict_get_item(p, "streams"); int found = 0;
        if(array && plist_get_node_type(array) == PLIST_ARRAY && plist_array_get_size(array) <= 8){
            for(uint32_t i = 0; i < plist_array_get_size(array); i++){
                stream = plist_array_get_item(array, i); uint64_t type; uint16_t dp, cp;
                if(!aps_plist_uint(stream, "type", &type) && type == 96 &&
                   !aps_plist_port(stream, "dataPort", &dp) && !aps_plist_port(stream, "controlPort", &cp)){
                    if(found++){ plist_free(p); return STEP("audio-duplicate", -1); }
                    uint64_t negotiated;
                    if((!aps_plist_uint(stream, "sr", &negotiated) && negotiated != s->rate) ||
                       (!aps_plist_uint(stream, "audioFormat", &negotiated) && negotiated != (UINT64_C(1) << ((s->rate == 48000 ? 20 : 18) + (s->bits == 24))))){
                        plist_free(p); s->error = APS_ERR_FORMAT; return STEP("audio-negotiation", -1);
                    }
                    peer(s, &s->data_peer, dp); peer(s, &s->control_peer, cp);
                }
            }
        }
        plist_free(p); if(STEP("audio-ports", found == 1 ? 0 : -1)) return -1;
    }
    if(!s->legacy && STEP("commands-advertise", aps_events_advertise(&s->rtsp, &s->reply)) < 0) return -1;
    s->anchor_rtp = s->rtp.timestamp; s->started = 1; return 0;
}
int aps_session_packet(aps_session *s, const void *pcm, size_t frames){
    aps_packet p;
    if(!s->started || aps_rtp_encode_bits(&s->rtp, pcm, frames, s->bits, &p)) return -1;
    ssize_t n = sendto(s->data, p.bytes, p.n, MSG_DONTWAIT | MSG_NOSIGNAL, (struct sockaddr *)&s->data_peer, sizeof s->data_peer);
    if(n != p.n) return -1; /* never silently lose an accepted packet */
    s->quiet = 0; aps_rtp_retain(&s->rtp, &p); return 0;
}
int aps_session_service(aps_session *s){
    if(aps_remote_service(&s->remote)) return STEP("remote-service", -1);
    if(aps_events_service(&s->event_state, &s->events, &s->remote)) return STEP("event-service", -1);
    if(aps_mrp_service(&s->mrp, &s->remote)) return STEP("mrp-service", -1);
    if(aps_now_ms() - s->rtx_window >= 100){ s->rtx_window = aps_now_ms(); s->rtx_sent = 0; }
    uint8_t b[APS_PACKET_MAX + 4]; struct sockaddr_in from; socklen_t fl;
    for(int channel = 0; channel < 2; channel++) for(int i = 0; i < 8; i++){
        fl = sizeof from; int fd = channel ? s->control : s->timing;
        if(fd < 0) break;
        ssize_t n = recvfrom(fd, b, sizeof b, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
        if(n < 0){ if(errno == EAGAIN || errno == EWOULDBLOCK) break; if(errno == EINTR) continue; return -1; }
        struct sockaddr_in *expected = channel ? &s->control_peer : &s->timing_peer;
        if(from.sin_family != AF_INET || from.sin_addr.s_addr != expected->sin_addr.s_addr ||
           (expected->sin_port && from.sin_port != expected->sin_port)) continue;
        if(!channel){
            uint8_t response[32]; uint64_t now = media_ntp(s);
            if(!aps_timing_reply(b, (size_t)n, now, media_ntp(s), response))
                (void)sendto(fd, response, sizeof response, MSG_DONTWAIT | MSG_NOSIGNAL, (struct sockaddr *)&from, sizeof from);
        } else {
            uint16_t req, seq; unsigned count;
            if(aps_retransmit_request(b, (size_t)n, &req, &seq, &count)) continue;
            for(unsigned j = 0; j < count && s->rtx_sent < 32; j++){
                const aps_packet *p = aps_rtp_find(&s->rtp, (uint16_t)(seq + j)); if(!p) continue;
                s->rtx_sent++;
                b[0] = APS_RTP_V2; b[1] = APS_RTP_RESEND; b[2] = (uint8_t)(req >> 8); b[3] = (uint8_t)req;
                memcpy(b + 4, p->bytes, p->n);
                (void)sendto(fd, b, p->n + 4, MSG_DONTWAIT | MSG_NOSIGNAL, (struct sockaddr *)&from, sizeof from);
            }
        }
    }
    if(s->started && aps_now_ms() - s->last_sync >= 1000){
        uint8_t sync[20]; uint32_t submitted = s->rtp.timestamp;
        aps_sync(submitted - s->rate * 2, submitted, media_ntp(s), !s->last_sync, sync);
        if(sendto(s->control, sync, sizeof sync, MSG_DONTWAIT | MSG_NOSIGNAL, (struct sockaddr *)&s->control_peer, sizeof s->control_peer) != sizeof sync) return -1;
        s->last_sync = aps_now_ms();
    }
    return 0;
}
static int simple(aps_session *s, const char *method){
    char h[128]; snprintf(h, sizeof h, "RTP-Info: seq=%u;rtptime=%u\r\n", s->rtp.seq, s->rtp.timestamp);
    return okay(s, aps_rtsp_request(&s->rtsp, method, s->rtsp.uri, h, NULL, 0, &s->reply, 2000));
}
int aps_session_flush(aps_session *s){
    if(simple(s, "FLUSH")) return -1;
    s->quiet = 1; memset(s->rtp.history, 0, sizeof s->rtp.history); s->rtp.first = 1; s->last_sync = 0; return 0;
}
int aps_session_pause(aps_session *s){ return simple(s, "PAUSE"); }
int aps_session_resume(aps_session *s){ return simple(s, "RECORD"); }
static int parameter(aps_session *s, const char *type, const void *b, size_t n, uint32_t stamp){
    char h[192]; snprintf(h, sizeof h, "Content-Type: %s\r\nRTP-Info: seq=%u;rtptime=%u\r\n", type, (unsigned)s->rtp.seq, stamp);
    return okay(s, aps_rtsp_request(&s->rtsp, "SET_PARAMETER", s->rtsp.uri, h, b, n, &s->reply, 2000));
}
int aps_session_art(aps_session *s, const void *data, size_t n, uint32_t start){
    const unsigned char *p = data;
    const char *type;
    if (!p || n > 256u * 1024u) return -1;
    if (n >= 4 && p[0] == 0xff && p[1] == 0xd8 && p[n - 2] == 0xff && p[n - 1] == 0xd9)
        type = "image/jpeg";
    else if (n >= 8 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8))
        type = "image/png";
    else return -1;
    int rc = parameter(s, type, data, n, start);
    fprintf(stderr, "AirPlay artwork: type=%s bytes=%zu rtptime=%u rc=%d\n", type, n, start, rc);
    return rc;
}
static size_t tag(uint8_t *b, const char *name, const char *value){
    size_t n = strlen(value); memcpy(b, name, 4); uint32_t k = htonl((uint32_t)n); memcpy(b + 4, &k, 4); memcpy(b + 8, value, n); return n + 8;
}
int aps_session_metadata(aps_session *s, const pb_view *v, uint32_t start){
    uint8_t b[1024]; size_t n = 8;
    n += tag(b + n, "minm", v->title); n += tag(b + n, "asar", v->artist); n += tag(b + n, "asal", v->album);
    memcpy(b, "mlit", 4); uint32_t k = htonl((uint32_t)n - 8); memcpy(b + 4, &k, 4);
    return parameter(s, "application/x-dmap-tagged", b, n, start);
}
int aps_session_progress(aps_session *s, const pb_view *v, uint32_t start){
    if(v->position_ms < 0 || v->duration_ms < v->position_ms) return -1;
    char line[128]; uint32_t current = start + (uint32_t)((uint64_t)v->position_ms * s->rate / 1000);
    uint32_t end = start + (uint32_t)((uint64_t)v->duration_ms * s->rate / 1000);
    int n = snprintf(line, sizeof line, "progress: %u/%u/%u\r\n", start, current, end);
    return parameter(s, "text/parameters", line, (size_t)n, current);
}
int aps_session_volume_read(aps_session *s, int *percent){
    const char request[] = "volume\r\n";
    if(okay(s, aps_rtsp_request(&s->rtsp, "GET_PARAMETER", s->rtsp.uri, "Content-Type: text/parameters\r\n", request, sizeof request - 1, &s->reply, 1000)) || s->reply.n < 8 || s->reply.n >= 80) return -1;
    char body[80]; memcpy(body, s->reply.body, s->reply.n); body[s->reply.n] = 0;
    if(strncmp(body, "volume:", 7)) return -1;
    char *end; double db = strtod(body + 7, &end);
    if(end == body + 7 || !isfinite(db) || db < -144.0 || db > 0 || (*end && strcmp(end, "\r\n"))) return -1;
    *percent = db <= -30 ? 0 : (int)((db + 30.0) / 0.3 + 0.5); return 0;
}
int aps_session_volume(aps_session *s, int percent){
    if(percent < 0 || percent > 100) return -1;
    char line[64]; double db = percent ? -30.0 + (double)percent * 0.3 : -144.0;
    int n = snprintf(line, sizeof line, "volume: %.6f\r\n", db);
    int rc = parameter(s, "text/parameters", line, (size_t)n, s->rtp.timestamp);
    if(rc && !s->rtsp.dead && !aps_mrp_volume(&s->mrp, percent)) return 1; /* queued, await observed level */
    return rc;
}
int aps_session_close(aps_session *s){
    int confirmed = !s->prepared || !s->rtp.counter || s->quiet;
    int report = s->prepared || s->started || s->rtsp.fd >= 0;
    int flush = -2, flush_status = 0, teardown = -2, teardown_status = 0;
    const char *basis = !s->prepared ? "unprepared" : !s->rtp.counter ? "no packets" : s->quiet ? "quiet" : "none";
    const char *cause = s->rtsp.dead ? "already dead" : "unverified";
    if(s->prepared && !s->rtsp.dead){
        int (*cancel)(void *) = s->rtsp.cancel; s->rtsp.cancel = NULL;
        if(s->started && !confirmed){
            char info[128]; snprintf(info, sizeof info, "RTP-Info: seq=%u;rtptime=%u\r\n", s->rtp.seq, s->rtp.timestamp);
            flush = aps_rtsp_request(&s->rtsp, "FLUSH", s->rtsp.uri, info, NULL, 0, &s->reply, 250);
            flush_status = s->reply.status;
            if(!flush && flush_status == 200){ confirmed = 1; basis = "FLUSH"; }
            cause = s->rtsp.diag_cause ? s->rtsp.diag_cause : "unverified";
        }
        /* A successful FLUSH plus retiring every sending socket is a playback
         * stop acknowledgement even if the receiver closes on TEARDOWN. */
        if(!s->rtsp.dead){
            teardown = aps_rtsp_request(&s->rtsp, "TEARDOWN", s->rtsp.uri, NULL, NULL, 0, &s->reply, 250);
            teardown_status = s->reply.status;
            if(!teardown && teardown_status == 200){ confirmed = 1; basis = "TEARDOWN"; }
            cause = s->rtsp.diag_cause ? s->rtsp.diag_cause : "unverified";
        }
        s->rtsp.cancel = cancel;
    }
    aps_rtsp_close(&s->rtsp); aps_rtsp_close(&s->events); aps_mrp_close(&s->mrp); aps_remote_close(&s->remote);
    if(s->data >= 0) close(s->data);
    if(s->control >= 0) close(s->control);
    if(s->timing >= 0) close(s->timing);
    s->data = s->control = s->timing = -1; s->started = s->prepared = 0;
    aps_wipe(s->audio_key, 32); aps_wipe(s->rtp.key, 32); aps_wipe(s->rtp.iv, 16);
    aps_wipe(&s->legacy_key, sizeof s->legacy_key);
    if(report) fprintf(stderr, "AirPlay stop confirmed=%d basis=%s FLUSH=%s TEARDOWN=%s io=%d status=%d cause=%s flush_io=%d flush_status=%d\n",
        confirmed, basis, flush == -2 ? "skipped" : !flush && flush_status == 200 ? "ok" : "failed",
        teardown == -2 ? "skipped" : !teardown && teardown_status == 200 ? "ok" : "failed",
        teardown, teardown_status, cause, flush, flush_status);
    return confirmed;
}
