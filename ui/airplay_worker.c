/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Runs only in the self-exec child. No LVGL, player IPC or pathname media opens. */
#include "airplay_session.h"
#include "airplay_crypto.h"
#include "audio_convert.h"
#include "audio_feed.h"
#include "audio_stock_wire.h"
#include "cast_art.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define SLOT_COUNT 8
typedef struct { aps_message m; int fd, art_fd; uint64_t request; uint32_t start; } track;
typedef struct {
    int ctl, cancel, failure, ready, running, ended, completed, awaiting, cold, volume, have_volume, unmute_volume;
    int volume_pending, volume_target; int64_t volume_deadline;
    uint32_t attempt, jump_gen; uint64_t generation, request, jump_occ;
    aps_session session;
    aps_message pending[16]; int descriptors[16]; unsigned count;
    track tracks[SLOT_COUNT]; int ingest, audible, chosen, navigating;
    aps_message navigation[8]; unsigned navigation_count;
    audio_decode *decoder; audio_convert *converter; audio_feed feed;
    uint64_t input_offset, submitted, observed;
    int64_t anchor_ms, pause_since, last_view, last_progress;
    const char *service_reason;
    rd_receiver target;
} worker;
static int send_message(worker *w, aps_message *m){ return aps_ipc_send(w->ctl, w->attempt, m, -1); }
static int notify(worker *w, unsigned type){ aps_message m = { .type = type }; return send_message(w, &m); }
static int service(void *ctx){
    worker *w = ctx;
    if(w->cancel) return 1;
    if(aps_session_service(&w->session)){ w->service_reason = "session service failed"; w->failure = 1; return 1; }
    aps_message remote;
    while(aps_remote_take(&w->session.remote, &remote)){
        if(!w->ready){
            if(remote.command == APS_VOLUME_OBSERVED){
                if(remote.position_ms < 0 || remote.position_ms > 100){ w->service_reason = "invalid receiver volume"; w->failure = 1; return 1; }
                w->volume = (int)remote.position_ms; w->have_volume = 1;
                if(w->volume > 0) w->unmute_volume = w->volume;
            }
            continue;
        }
        if(w->count == 16){ w->service_reason = "remote command queue full"; w->failure = 1; return 1; }
        w->pending[w->count] = remote; w->descriptors[w->count++] = -1;
    }
    for(unsigned i = 0; i < 16; i++){
        aps_message m; int fd; const char *reason; int n = aps_ipc_receive_reason(w->ctl, w->attempt, &m, &fd, &reason);
        if(n < 0){ fprintf(stderr, "AirPlay worker end reason=%s\n", reason); w->cancel = 1; return 1; }
        if(!n) break;
        if(m.type == APS_CANCEL){ if(fd >= 0) close(fd); fprintf(stderr, "AirPlay worker end reason=APS_CANCEL\n"); w->cancel = 1; return 1; }
        if(w->count == 16){ if(fd >= 0) close(fd); w->service_reason = "IPC command queue full"; w->failure = 1; return 1; }
        w->pending[w->count] = m; w->descriptors[w->count++] = fd;
    }
    return 0;
}
static int pop(worker *w, aps_message *m, int *fd){
    if(!w->count) return 0;
    *m = w->pending[0]; *fd = w->descriptors[0]; --w->count;
    memmove(w->pending, w->pending + 1, w->count * sizeof w->pending[0]);
    memmove(w->descriptors, w->descriptors + 1, w->count * sizeof w->descriptors[0]);
    aps_wipe(&w->pending[w->count], sizeof w->pending[0]); w->descriptors[w->count] = -1; return 1;
}
static int pair_post(void *ctx, const char *path, const aps_tlv *tx, aps_tlv *rx){
    worker *w = ctx; return aps_session_pair_post(&w->session, path, tx, rx);
}
static int pin(void *ctx, char out[9]){
    worker *w = ctx; int64_t end = aps_now_ms() + 150000;
    if(notify(w, APS_NEED_PIN)) return -1;
    while(!service(w) && aps_now_ms() < end){
        aps_message m; int fd;
        if(pop(w, &m, &fd)){
            if(fd >= 0) close(fd);
            if(m.type != APS_PIN) return -1;
            size_t n = strlen(m.pin); if(n < 4 || n > 8) return -1;
            for(size_t i = 0; i < n; i++) if(m.pin[i] < '0' || m.pin[i] > '9') return -1;
            memcpy(out, m.pin, n + 1); aps_wipe(&m, sizeof m); return 0;
        }
        struct pollfd p = { w->ctl, POLLIN, 0 }; (void)poll(&p, 1, 20);
    }
    if(!w->cancel) w->session.error = APS_ERR_TIMEOUT;
    return -1;
}
static void close_decode(worker *w){
    audio_convert_close(w->converter); w->converter = NULL;
    audio_decode_close(w->decoder); w->decoder = NULL;
}
static int decode(worker *w, int index, int64_t ms){
    if(index < 0 || index >= SLOT_COUNT || w->tracks[index].fd < 0 || ms < 0) return -1;
    close_decode(w);
    if(audio_decode_open(&w->decoder, w->tracks[index].fd, 0, service, w) != AD_OK) return -1;
    const ad_info *info = audio_decode_info(w->decoder);
    if(!w->session.started && !w->cold){
        if(aps_session_start(&w->session, info->rate, info->bits)) return -1;
        if(!aps_session_volume_read(&w->session, &w->volume)) w->have_volume = 1;
        if(w->have_volume && w->volume > 0) w->unmute_volume = w->volume;
        if(w->session.rtsp.dead) return -1;
    }
    if(audio_convert_open_pcm(&w->converter, w->decoder, w->session.rate, w->session.bits, w->generation) != AD_OK) return -1;
    uint64_t frame = (uint64_t)ms / 1000 * info->rate + (uint64_t)ms % 1000 * info->rate / 1000;
    if(ms && audio_convert_seek(w->converter, frame) != AD_OK) return -1;
    w->ingest = index; w->input_offset = (uint64_t)ms * w->session.rate / 1000;
    w->tracks[index].start = w->session.rtp.timestamp + (uint32_t)w->feed.used - (uint32_t)w->input_offset;
    w->tracks[index].m.view.position_ms = (long)ms;
    if(info->frames != UINT64_MAX && info->frames <= UINT64_MAX / 1000)
        w->tracks[index].m.view.duration_ms = (long)(info->frames * 1000 / info->rate);
    return 0;
}
static void artwork(worker *w, int index);
static int reanchor(worker *w, int index, int64_t position){
    if(w->generation == UINT64_MAX || w->request == UINT64_MAX || (!w->cold && aps_session_flush(&w->session))) return -1;
    ++w->request; /* invalidate every in-flight prefetch response */
    for(int i = 0; i < SLOT_COUNT; i++) if(i != index && w->tracks[i].fd >= 0){
        close(w->tracks[i].fd); w->tracks[i].fd = -1;
        if(w->tracks[i].art_fd >= 0){ close(w->tracks[i].art_fd); w->tracks[i].art_fd = -1; }
    }
    if(audio_feed_begin_pcm(&w->feed, ++w->generation, w->session.rate, w->session.bits) != AF_OK) return -1;
    w->submitted = w->observed = 0; w->anchor_ms = aps_now_ms() + 2000; w->ended = w->completed = w->awaiting = w->navigating = 0; w->chosen = index;
    if(decode(w, index, position)) return -1;
    if(w->session.started){
        artwork(w, index);
    }
    return 0;
}
static void retire(worker *w, uint64_t item){
    for(int i = 0; i < SLOT_COUNT; i++) if(w->tracks[i].fd >= 0 && w->tracks[i].m.item < item && i != w->ingest){
        close(w->tracks[i].fd); w->tracks[i].fd = -1;
        if(w->tracks[i].art_fd >= 0){ close(w->tracks[i].art_fd); w->tracks[i].art_fd = -1; }
    }
}
static void artwork(worker *w, int index){
    if(w->cold || !w->session.started) return;
    track *t = &w->tracks[index];
    /* Establish the timeline and text before the image, including a cover
     * delivered after the track began. Keep each update in the same order. */
    (void)aps_session_progress(&w->session, &t->m.view, t->start);
    (void)aps_session_metadata(&w->session, &t->m.view, t->start);
    /* Receivers such as LG keep the first image sent for an item, so never send a
     * placeholder ahead of the real cover; a track without a cover sends none. */
    if(t->art_fd < 0) return;
    unsigned char *png = NULL; size_t n = 0;
    int result = cast_art_airplay_fd(t->art_fd, &png, &n);
    if(!result) (void)aps_session_art(&w->session, png, n, t->start);
    else fprintf(stderr, "AirPlay artwork encode failed: cover=%d\n", t->art_fd >= 0);
    free(png); /* Retain the approved cover descriptor for cold reconnect. */
}
/* The queue may have opened a cover while a skip was in flight. Only the
 * currently audible request may adopt it; a second delivery is harmless. */
static void accept_art(worker *w, const aps_message *m, int fd){
    int i = w->audible;
    if(i < 0 || w->tracks[i].fd < 0 ||
       w->tracks[i].m.occurrence != m->occurrence ||
       w->tracks[i].request != m->item || w->tracks[i].art_fd >= 0){
        close(fd); return;
    }
    w->tracks[i].art_fd = fd;
    artwork(w, i);
}
static int publish(worker *w, int force){
    int64_t now = aps_now_ms();
    if(w->audible < 0 || (!force && now - w->last_view < 100)) return 0;
    track *t = &w->tracks[w->audible]; aps_message m = t->m;
    m.art_request = t->request;
    m.type = APS_VIEW; m.view.playing = w->running; m.play = w->running; m.rate = (int)w->session.rate; m.bits = (int)w->session.bits; m.volume = w->volume; m.have_volume = w->have_volume;
    if(!w->cold && aps_mrp_state(&w->session.mrp, &m.view)) return -1;
    (void)send_message(w, &m); w->last_view = now; return 0;
}
static int observe(worker *w){
    if(!w->ready || !w->running) return 0;
    int64_t delta = aps_now_ms() - w->anchor_ms;
    if(delta < 0) return publish(w, 0);
    uint64_t frame = delta > 0 ? (uint64_t)delta * w->session.rate / 1000 : 0;
    if(frame > w->submitted) frame = w->submitted;
    atl_view v;
    if(audio_feed_observe(&w->feed, w->generation, frame, &v) != ATL_OK) return -1;
    w->observed = frame;
    if(v.have_item){
        for(int i = 0; i < SLOT_COUNT; i++) if(w->tracks[i].fd >= 0 && w->tracks[i].m.item == v.item){
            int changed = w->audible != i;
            if(w->chosen == w->audible) w->chosen = i;
            w->audible = i;
            w->tracks[i].m.view.position_ms = (long)v.position_ms;
            if(changed){
                retire(w, v.item);
                artwork(w, i); w->last_progress = 0;
            }
            break;
        }
    }
    if(w->ended && v.drained && !w->converter){
        w->running = 0; w->completed = 1; w->pause_since = aps_now_ms(); publish(w, 1); return notify(w, APS_ENDED);
    }
    return publish(w, 0);
}
static int request_next(worker *w, unsigned command){
    int index = command ? w->chosen : w->ingest;
    if(index < 0 || w->request == UINT64_MAX) return -1;
    if(!command){
        int space = 0;
        for(int i = 0; i < SLOT_COUNT; i++) space |= w->tracks[i].fd < 0;
        if(!space) return 0; /* wait for audible retirement before prefetch */
    }
    aps_message m = w->tracks[index].m; m.type = APS_NEED_NEXT; m.command = command; m.item = ++w->request;
    if(command == APS_QUEUE_JUMP){ m.occurrence = w->jump_occ; m.qgen = w->jump_gen; }
    m.position_ms = m.view.position_ms; w->awaiting = 1; w->navigating = command != 0;
    return send_message(w, &m);
}
static int install(worker *w, aps_message *m, int fd){
    if(m->type == APS_NEXT_FILE && (!w->awaiting || m->item != w->request)){ close(fd); return 0; }
    int i;
    for(i = 0; i < SLOT_COUNT; i++) if(w->tracks[i].fd < 0) break;
    if(i == SLOT_COUNT){ close(fd); return -1; }
    /* The request token is not the playback-pass identity; occurrence may repeat. */
    uint64_t request = m->item;
    m->item = w->tracks[w->ingest >= 0 ? w->ingest : i].m.item + 1;
    if(!m->item) m->item = 1;
    w->tracks[i].m = *m; w->tracks[i].fd = fd; w->tracks[i].request = request;
    if(decode(w, i, m->position_ms)) return -1;
    int navigation = w->navigating;
    w->awaiting = w->navigating = 0;
    if(navigation){
        w->anchor_ms = aps_now_ms() + 2000;
        w->chosen = i;
        if(!w->running){
            w->audible = i; retire(w, m->item);
            artwork(w, i); publish(w, 1);
        }
    }
    if(!w->ready){
        w->ready = 1; w->chosen = w->audible = i; w->generation = 1;
        if(audio_feed_begin_pcm(&w->feed, 1, w->session.rate, w->session.bits) != AF_OK) return -1;
        w->anchor_ms = aps_now_ms() + 2000; w->pause_since = aps_now_ms();
        if(notify(w, APS_STARTED)) return -1;
        artwork(w, i); publish(w, 1);
    }
    return 0;
}
static int refused(worker *w, unsigned command){
    aps_message m = { .type = APS_NOTICE, .command = command }; (void)send_message(w, &m);
    return w->session.rtsp.dead && !w->cold ? -1 : 0;
}
static int command(worker *w, const aps_message *m){
    if(!w->ready || w->audible < 0) return -1;
    if(w->volume_pending && aps_now_ms() >= w->volume_deadline){
        w->volume_pending = 0; w->have_volume = 0;
    }
    if((m->command == APS_QUEUE_MODE || m->command == PB_CMD_NEXT || m->command == PB_CMD_PREV) &&
       (w->navigating || w->navigation_count)){
        if(w->navigation_count == 8) return refused(w, m->command);
        w->navigation[w->navigation_count++] = *m; return 0;
    }
    track *t = &w->tracks[w->audible]; int play;
    switch(m->command){
    case APS_REMOTE_STOP: {
        fprintf(stderr, "AirPlay worker end reason=receiver stop\n");
        aps_message notice = { .type = APS_NOTICE, .command = APS_REMOTE_STOP };
        if(send_message(w, &notice)) return -1;
        w->cancel = 1; return 0;
    }
    case APS_VOLUME_OBSERVED:
        if(m->position_ms < 0 || m->position_ms > 100) return -1;
        if(w->volume_pending && m->position_ms == w->volume_target) w->volume_pending = 0;
        w->volume = (int)m->position_ms; w->have_volume = 1;
        if(w->volume > 0) w->unmute_volume = w->volume;
        return publish(w, 1); /* observation never writes volume back */
    case APS_MUTE: {
        int current;
        if(w->cold) return refused(w, PB_CMD_VOLUME);
        if(w->volume_pending) current = w->volume_target;
        else if(aps_session_volume_read(&w->session, &current)){
            if(!w->have_volume) { (void)aps_mrp_volume(&w->session.mrp, -1); return refused(w, PB_CMD_VOLUME); }
            current = w->volume;
        }
        if(current > 0) w->unmute_volume = current;
        if(current == 0 && !w->unmute_volume) return refused(w, PB_CMD_VOLUME);
        int level = current ? 0 : w->unmute_volume;
        int result = aps_session_volume(&w->session, level);
        if(result < 0) return refused(w, PB_CMD_VOLUME);
        if(result > 0){ w->volume_pending = 1; w->volume_target = level; w->volume_deadline = aps_now_ms()+2000; return 0; }
        w->volume_pending = 0; w->volume = level; w->have_volume = 1;
        if(w->volume > 0) w->unmute_volume = w->volume;
        return publish(w, 1);
    }
    case APS_VOLUME_QUERY:
        if(w->cold) return refused(w, PB_CMD_VOLUME);
        if(aps_session_volume_read(&w->session, &w->volume))
            return aps_mrp_volume(&w->session.mrp, -1) ? refused(w, PB_CMD_VOLUME) : 0;
        w->volume_pending = 0; w->have_volume = 1;
        if(w->volume > 0) w->unmute_volume = w->volume;
        return publish(w, 1);
    case PB_CMD_VOLUME: {
        if(w->cold) return refused(w, PB_CMD_VOLUME);
        int level = (int)m->position_ms;
        if(level == -1 || level == -2){
            int current;
            if(w->volume_pending) current = w->volume_target;
            else if(aps_session_volume_read(&w->session, &current)){
                if(!w->have_volume){ (void)aps_mrp_volume(&w->session.mrp, -1); return refused(w, PB_CMD_VOLUME); }
                current = w->volume;
            }
            level = current + (level == -1 ? 1 : -1);
            if(level < 0) level = 0;
            if(level > 100) level = 100;
        }
        int result = aps_session_volume(&w->session, level);
        if(result < 0) return refused(w, PB_CMD_VOLUME);
        if(result > 0){ w->volume_pending = 1; w->volume_target = level; w->volume_deadline = aps_now_ms()+2000; return 0; }
        w->volume_pending = 0; w->volume = level; w->have_volume = 1;
        if(w->volume > 0) w->unmute_volume = w->volume;
        return publish(w, 1);
    }
    case APS_QUEUE_REFUSED: case APS_QUEUE_CHANGED: {
        aps_message notice = { .type = APS_NOTICE, .command = m->command }; return send_message(w, &notice);
    }
    case APS_QUEUE_MODE:
        if(w->chosen < 0) return -1;
        /* The response may already include this edit's installed revision. */
        if(m->item != m->qgen && w->tracks[w->chosen].m.qgen == m->qgen) return 0;
        if(w->tracks[w->chosen].m.qgen != m->item)
            return refused(w, APS_QUEUE_CHANGED);
        w->tracks[w->chosen].m.qgen = m->qgen;
        if(reanchor(w, w->chosen, w->tracks[w->chosen].m.view.position_ms)) return -1;
        return publish(w, 1);
    case APS_QUEUE_JUMP:
        if(!m->occurrence || !m->qgen) return -1;
        w->jump_occ = m->occurrence; w->jump_gen = m->qgen;
        w->navigation_count = 0; w->navigating = 0;
        /* An explicit new selection invalidates prefetch through request_next. */
        /* fall through */
    case PB_CMD_NEXT: case PB_CMD_PREV:
        if(w->chosen < 0) return -1;
        t = &w->tracks[w->chosen];
        if(m->command == PB_CMD_PREV && t->m.view.position_ms >= 11000) return reanchor(w, w->chosen, 0);
        for(int i = 0; i < SLOT_COUNT; i++) if(i != w->audible && i != w->chosen && w->tracks[i].fd >= 0){
            close(w->tracks[i].fd); w->tracks[i].fd = -1;
            if(w->tracks[i].art_fd >= 0){ close(w->tracks[i].art_fd); w->tracks[i].art_fd = -1; }
        }
        if(!w->cold && aps_session_flush(&w->session)) return -1;
        close_decode(w); w->ended = w->completed = 0;
        if(w->generation == UINT64_MAX || audio_feed_begin_pcm(&w->feed, ++w->generation, w->session.rate, w->session.bits) != AF_OK) return -1;
        w->submitted = w->observed = 0; w->anchor_ms = aps_now_ms() + 2000;
        return request_next(w, m->command);
    case PB_CMD_SEEK:
        if(m->position_ms < 0 || (t->m.view.duration_ms > 0 && m->position_ms > t->m.view.duration_ms)) return refused(w, PB_CMD_SEEK);
        if(reanchor(w, w->audible, m->position_ms)) return -1;
        return publish(w, 1);
    case PB_CMD_TOGGLE:
        play = m->play < 0 ? !w->running : !!m->play;
        if(play == w->running) return 0;
        if(play){
            int64_t position = w->completed ? 0 : t->m.view.position_ms;
            if(w->cold){
                aps_pair_io io = { w, pair_post, pin };
                aps_remote remote = w->session.remote;
                aps_session_init(&w->session, service, w); w->session.remote = remote;
                if(aps_session_prepare(&w->session, &w->target, &io)) return -1;
                w->cold = 0;
            }
            if(!w->session.started){
                if(decode(w, w->audible, position)) return -1;
            } else if(aps_session_resume(&w->session)) return -1;
            if(reanchor(w, w->audible, position)) return -1;
            w->running = 1;
        } else {
            if(observe(w) || aps_session_flush(&w->session)) return -1;
            w->running = 0; w->pause_since = aps_now_ms();
            if(w->chosen != w->audible){
                w->audible = w->chosen;
                if(reanchor(w, w->chosen, w->tracks[w->chosen].m.view.position_ms)) return -1;
                retire(w, w->tracks[w->chosen].m.item);
            }
            close_decode(w);
        }
        return publish(w, 1);
    default: return -1;
    }
}
static int handle(worker *w){
    aps_message m; int fd;
    while(pop(w, &m, &fd)){
        if(m.type == APS_FORMAT){
            if(fd >= 0){ close(fd); return -1; }
            if(w->ready || w->session.started || !w->session.prepared || m.rate <= 0 || m.rate > AS_RATE_MAX || m.bits < 0 || m.bits > 32) return -1;
            if(aps_session_start(&w->session, (unsigned)m.rate, (unsigned)m.bits)) return -1;
            if(!aps_session_volume_read(&w->session, &w->volume)) w->have_volume = 1;
            if(w->have_volume && w->volume > 0) w->unmute_volume = w->volume;
            if(w->session.rtsp.dead || notify(w, APS_STREAM_READY)) return -1;
        } else if(m.type == APS_GO || m.type == APS_NEXT_FILE){
            if((m.type == APS_GO && (w->ready || !w->session.started)) || fd < 0){ if(fd >= 0) close(fd); return -1; }
            if(install(w, &m, fd)) return -1;
        } else if(m.type == APS_ART){
            accept_art(w, &m, fd);
        } else {
            if(fd >= 0){ close(fd); return -1; }
            if(m.type == APS_NOTICE && m.command == APS_QUEUE_SKIP){ if(send_message(w, &m)) return -1; }
            else if(m.type == APS_COMMAND){ if(command(w, &m)) return -1; if(w->cancel) return 0; }
            else if(m.type == APS_ENDED){ if(w->awaiting && m.item == w->request){ w->ended = 1; w->awaiting = 0; } }
            else return -1;
        }
        aps_wipe(&m, sizeof m);
    }
    return 0;
}
static int feed(worker *w){
    if(w->cancel || !w->ready || !w->running || w->cold) return 0;
    if(!w->converter && !w->awaiting && !w->ended && request_next(w, 0)) return -1;
    if(w->converter && audio_feed_space(&w->feed) >= w->session.packet_frames){
        union { int16_t s16[APS_PACKET_FRAMES * 2]; int32_t s24[APS_PACKET_FRAMES * 2]; } pcm; size_t n = 0;
        ad_result r = w->session.bits == 24 ? audio_convert_read24(w->converter, pcm.s24, w->session.packet_frames, &n) : audio_convert_read(w->converter, pcm.s16, w->session.packet_frames, &n);
        track *t = &w->tracks[w->ingest];
        if(r != AD_OK && r != AD_EOF) return -1;
        if(n){
            int a = w->session.bits == 24 ? audio_feed_push24(&w->feed, w->generation, t->m.item, t->m.occurrence, w->input_offset, pcm.s24, n) : audio_feed_push(&w->feed, w->generation, t->m.item, t->m.occurrence, w->input_offset, pcm.s16, n);
            if(a != AF_OK) return -1;
            w->input_offset += n;
        }
        if(r == AD_EOF){
            if(audio_feed_end(&w->feed, w->generation, t->m.item) != AF_OK) return -1;
            close_decode(w); if(request_next(w, 0)) return -1;
        }
    }
    int64_t due = w->anchor_ms - 2000 + (int64_t)(w->submitted * 1000 / w->session.rate);
    if(aps_now_ms() >= due){
        af_packet p; int r = audio_feed_peek(&w->feed, w->generation, w->session.packet_frames, &p);
        if(r != AF_OK && r != AF_EMPTY && r != AF_FULL) return -1;
        if(r == AF_OK){
            if(aps_session_packet(&w->session, w->session.bits == 24 ? (const void *)p.pcm24 : p.pcm, p.frames) || audio_feed_accept(&w->feed, &p, p.frames) != AF_OK) return -1;
            w->submitted += p.frames;
        } else if(w->awaiting && aps_now_ms() > w->anchor_ms + (int64_t)(w->submitted * 1000 / w->session.rate)) return -1;
    }
    if(observe(w)) return -1;
    if(w->audible >= 0 && aps_now_ms() - w->last_progress >= 1000){
        track *t = &w->tracks[w->audible];
        if(t->m.view.duration_ms > 0 && aps_session_progress(&w->session, &t->m.view, t->start)) return -1;
        w->last_progress = aps_now_ms();
    }
    return 0;
}
int aps_worker_main(int ctl, uint32_t attempt){
    if(ctl==3) audio_decode_stock_supervise(4,attempt);
    worker *w = calloc(1, sizeof *w); if(!w){ fprintf(stderr, "AirPlay worker end reason=allocation failed\n"); return 1; }
    w->ctl = ctl; w->attempt = attempt; w->ingest = w->audible = w->chosen = -1;
    for(int i = 0; i < SLOT_COUNT; i++) w->tracks[i].fd = w->tracks[i].art_fd = -1;
    for(int i = 0; i < 16; i++) w->descriptors[i] = -1;
    struct sigaction action = { .sa_handler = SIG_DFL }; sigemptyset(&action.sa_mask);
    sigaction(SIGALRM, &action, NULL); alarm(0); sigaction(SIGCHLD, &action, NULL);
    aps_session_init(&w->session, service, w);
    const char *end_reason = "startup failed";
    int64_t deadline = aps_now_ms() + 5000; aps_message first; int fd = -1, got = 0;
    while(!service(w) && aps_now_ms() < deadline){
        if(pop(w, &first, &fd)){ got = 1; break; }
        struct pollfd p = { ctl, POLLIN, 0 }; (void)poll(&p, 1, 20);
    }
    int rc = 1; if(fd >= 0) close(fd);
    if(!got || first.type != APS_TARGET){ end_reason = got ? "expected TARGET" : "TARGET timeout"; goto end; }
    w->target = first.target;
    if(first.command > 1 || (first.command && (!w->target.stable_id || aps_session_forget(w->target.id)))){
        end_reason = first.command > 1 ? "invalid TARGET command" : !w->target.stable_id ? "forget requires stable id" : "forget failed"; goto end;
    }
    aps_pair_io io = { w, pair_post, pin };
    if(aps_session_prepare(&w->session, &w->target, &io)){ end_reason = "prepare failed"; goto end; }
    if(notify(w, APS_READY)){ end_reason = "READY send failed"; goto end; }
    end_reason = "session service failed";
    while(!service(w)){
        if(handle(w)){ end_reason = "message handling failed"; w->failure = 1; break; }
        if(!w->navigating && w->navigation_count){
            aps_message next = w->navigation[0];
            --w->navigation_count;
            memmove(w->navigation, w->navigation + 1, w->navigation_count * sizeof *w->navigation);
            unsigned remaining = w->navigation_count;
            w->navigation_count = 0;
            if(command(w, &next)){ end_reason = "navigation failed"; w->failure = 1; break; }
            w->navigation_count = remaining;
        }
        if(feed(w)){ end_reason = "feed failed"; w->failure = 1; break; }
        if(w->ready && !w->running && !w->cold && aps_now_ms() - w->pause_since >= 30000){
            fprintf(stderr, "AirPlay transport retire reason=paused 30s\n");
            aps_remote remote = w->session.remote; aps_remote_init(&w->session.remote);
            (void)aps_session_close(&w->session); w->session.remote = remote; w->cold = 1;
        }
        if(w->session.prepared && !w->cold && !w->session.legacy && aps_now_ms() - w->session.last_feedback >= 2000){
            if(aps_rtsp_request(&w->session.rtsp, "POST", "/feedback", NULL, NULL, 0, &w->session.reply, 1000) || w->session.reply.status != 200){ end_reason = "feedback failed"; w->failure = 1; break; }
            w->session.last_feedback = aps_now_ms();
        }
        struct pollfd polls[4] = { { ctl, POLLIN, 0 } }; nfds_t count = 1;
        if(w->session.remote.listener >= 0) polls[count++] = (struct pollfd){ w->session.remote.listener, POLLIN, 0 };
        for(unsigned i = 0; i < 2; i++) if(w->session.remote.clients[i].fd >= 0) polls[count++] = (struct pollfd){ w->session.remote.clients[i].fd, w->session.remote.clients[i].answered ? POLLOUT : POLLIN, 0 };
        (void)poll(polls, count, w->cold ? 250 : w->running ? 2 : 20);
    }
    rc = w->failure ? 1 : 0;
end:
    if(rc && !w->cancel){
        aps_session *s = &w->session; aps_rtsp *r = &s->rtsp;
        int error = s->error ? s->error : APS_ERR_STREAM;
        fprintf(stderr, "AirPlay give-up reason=%s step=%.39s io=%d status=%d bytes=%zu rc=%d error=%d verified=%d ready=%d\n",
            w->service_reason ? w->service_reason : end_reason, r->diag_step, r->diag_io, r->diag_status, r->diag_n, r->diag_rc, error, s->verified, w->ready);
        if(s->verified && (error == APS_ERR_CONNECT || error == APS_ERR_STREAM))
            error = w->ready ? APS_ERR_STREAM : APS_ERR_START;
        aps_message m = { .type = APS_FAILED, .error = error }; (void)send_message(w, &m);
    }
    close_decode(w);
    if(aps_session_close(&w->session)) (void)notify(w, APS_STOPPED);
    for(int i = 0; i < SLOT_COUNT; i++){
        if(w->tracks[i].fd >= 0) close(w->tracks[i].fd);
        if(w->tracks[i].art_fd >= 0) close(w->tracks[i].art_fd);
    }
    for(unsigned i = 0; i < w->count; i++) if(w->descriptors[i] >= 0) close(w->descriptors[i]);
    close(ctl); aps_wipe(w, sizeof *w); free(w); return rc;
}
