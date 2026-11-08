/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cast_session.h"
#include "device_names.h"
#include "cast_wire.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#define NS_HB "urn:x-cast:com.google.cast.tp.heartbeat"
#define NS_CON "urn:x-cast:com.google.cast.tp.connection"
#define NS_RECV "urn:x-cast:com.google.cast.receiver"
#define NS_MEDIA "urn:x-cast:com.google.cast.media"
#define INF INT64_MAX
enum { PH_STATUS, PH_LAUNCH, PH_APP, PH_READY };
enum { T_CONN = 1, T_STATUS = 2, T_LAUNCH = 4, T_APP = 8 };
#ifdef CAST_SESSION_TEST /* host tests may act at a named point of the worker; compiled out of the product */
extern void cs_test_point(cast_session *, const char *);
#define TP(s, n) cs_test_point(s, n)
#else
#define TP(s, n) ((void)0)
#endif
typedef struct { int op, muted, track_ver; double v; } op_t;
typedef struct { char content[CM_TEXT], type[CM_ID], title[CM_TEXT], artist[CM_TEXT], album[CM_TEXT], image[CM_TEXT]; double start; } ldreq;
/* Locking: mu guards ops[] and snap only, never held across I/O. cancel and close_req are atomics.
 * Everything below "worker" is touched by the worker only. */
struct cast_session {
    cast_session_cfg cfg; char host[64], sender_name[33];
    pthread_t thread; pthread_mutex_t mu; int wake[2];
    int cancel, close_req, nops, track_ver; cs_snapshot snap;
    op_t ops[4];
    char want[CM_TEXT]; /* contentId named by cast_session_track() or cast_session_load(); with track_ver, guarded by mu */
    ldreq ld; int ld_ver, stop_req; /* newest LOAD request and its version; a media STOP asked for (both guarded by mu) */
    cs_snapshot retained; char retained_content[CM_TEXT]; /* last proven media; guarded by mu */
    /* worker */
    remote_tls *tls; int fd, hs, phase, todo, hb, live, conn_tx, req, pend;
    int fin, fin_err, fin_clean, pong_owed, hb_last;
    /* launch: lst 0 none, 1 LAUNCH queued with nothing written, 2 write begun (a session may exist), 3 resolved
     * (refused, or reattach found it gone), 4 handed over (READY): never rolled back. owned_set/owned/lst survive a reconnect; the rest is per connection. */
    int launch_id, lst, owned_set, app_tx, live_app, cleanup;
    /* owned media: mid is the adopted mediaSessionId (0 none); content/seen_ver follow cast_session_track(); pend_media: pend is a media command */
    int stopped, absence, probe_id, probe_ver; /* explicit absence needs a correlated media GET_STATUS in this track generation */
    int mid, seen_ver, pend_media; cs_op command_op; char content[CM_TEXT];
    /* LOAD / STOP in flight (request id, 0 none; independent of pend). ldw is the request being sent, ld_sent the ld_ver it came from:
     * a reply is only accepted while ld_ver == ld_sent (not superseded). */
    ldreq ldw; int ld_id, ld_sent, stop_id;
    int ld_mid, ld_check, ld_cache_at;
    cm_media ld_cache[CM_ENTRIES]; /* worker-only identity evidence for this LOAD */
    int want_load;                /* guarded by mu */
    int track_load;               /* worker copy, refreshed by sync_track */
    int64_t cdl, ldl, pend_dl, tx_dl, next_ping, last_rx, next_media_status;
    cm_app owned;
    cast_wire w;
};
static int64_t now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int flag(int *f) { return __atomic_load_n(f, __ATOMIC_ACQUIRE); }
static void wake(cast_session *s) { char c = 1; (void)!write(s->wake[1], &c, 1); }
/* Worker only, mu held. A failed replacement does not prove the previous media ended. */
static void restore_media(cast_session *s)
{
    if (!s->retained.media_id) return;
    s->mid = s->snap.media_id = s->retained.media_id;
    s->stopped = s->snap.stopped = s->retained.stopped;
    s->snap.player = s->retained.player; s->snap.idle = s->retained.idle;
    s->snap.media_time = s->retained.media_time;
    strcpy(s->content, s->retained_content);
}
static void remember_media(cast_session *s) /* mu held */
{
    s->retained = s->snap;
    strcpy(s->retained_content, s->content);
}
static void publish(cast_session *s, cs_state st, int err)
{
    if (s->lst == 2 && (st == CS_LOST || st == CS_FAILED || st == CS_CLOSED) && s->cleanup == CS_CLEAN_NONE)
        s->cleanup = CS_CLEAN_UNCERTAIN; /* a begun LAUNCH ended without rollback: the app may still run */
    pthread_mutex_lock(&s->mu);
    s->snap.state = st; s->snap.error = err; s->snap.generation++; s->snap.cleanup = s->cleanup;
    if (s->owned_set) s->snap.app = s->owned;
    if (st != CS_READY) { /* a pending LOAD or STOP cannot complete any more; an unsent LOAD is never sent after a reconnect */
        int e = st == CS_TAKEN_OVER ? CS_E_TAKEOVER : CS_E_LINK;
        if (s->snap.load_state == CS_JOB_PENDING) { restore_media(s); s->snap.load_state = CS_JOB_FAILED; s->snap.load_error = e; }
        if (s->snap.stop_state == CS_JOB_PENDING) { s->snap.stop_state = CS_JOB_FAILED; s->snap.stop_error = e; }
        s->ld_id = s->stop_id = s->stop_req = 0; s->ld_sent = s->ld_ver;
    }
    pthread_mutex_unlock(&s->mu);
}
static int hook(void *p) { cast_session *s = p; return s->hs && flag(&s->cancel); }
static void waitfd(cast_session *s, int ev, int64_t until)
{
    int64_t t = now_ms(); char b[16];
    struct pollfd p[2] = {{s->fd, (short)ev, 0}, {s->wake[0], POLLIN, 0}};
    int ms = until <= t ? 0 : until - t > 1000 ? 1000 : (int)(until - t);
    if (poll(p, 2, ms) > 0 && (p[1].revents & POLLIN)) while (read(s->wake[0], b, sizeof b) > 0);
}
static int dial(cast_session *s)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)s->cfg.port) };
    int e = 0, fd; socklen_t n = sizeof e;
    if (inet_pton(AF_INET, s->host, &a.sin_addr) != 1) return -1;
    if ((fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)) < 0) return -1;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0 && errno != EINPROGRESS) goto bad;
    for (;;) {
        struct pollfd p = { fd, POLLOUT, 0 };
        if (flag(&s->cancel) || now_ms() >= s->cdl) goto bad;
        if (poll(&p, 1, 50) > 0) {
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &n) || e) goto bad;
            return fd;
        }
    }
bad:
    close(fd); return -1;
}
static int queue(cast_session *s, const char *dest, cm_command c)
{
    char b[CM_JSON];
    if(c.kind==CM_CONNECT) c.sender_name=s->sender_name;
    s->tx_dl = now_ms() + s->cfg.dead_ms; /* absolute: an unfinished write is never abandoned for another frame */
    return cm_encode(b, sizeof b, &c) ? cast_wire_queue(&s->w, "sender-0", dest, cm_namespace(&c), b) : RT_ARGUMENT;
}
static int flush_until(cast_session *s, int64_t dl)
{
    int r;
    while ((r = cast_wire_flush(&s->w, dl)) == RT_READ || r == RT_WRITE) waitfd(s, r == RT_READ ? POLLIN : POLLOUT, dl);
    return r;
}
static int send_sync(cast_session *s, const char *dest, cm_command c, int64_t dl)
{
    int r = s->w.tx_len ? flush_until(s, dl) : RT_OK;
    return r ? r : (r = queue(s, dest, c)) ? r : flush_until(s, dl);
}
static void on_msg(cast_session *s);
static int drop(cast_session *s, int r)
{
    if (s->tls) remote_tls_free(s->tls);
    if (s->fd >= 0) close(s->fd);
    s->tls = NULL; s->fd = -1;
    return r;
}
static void fin(cast_session *s, cs_state st, int err, int clean, const char *reason)
{
    if (s->fin) return;
    fprintf(stderr, "Cast session end reason=%s state=%d error=%d clean=%d\n", reason, st, err, clean);
    s->fin = st; s->fin_err = err; s->fin_clean = clean;
}
/* An app this attempt launched that was never handed over (not READY) must not be left running. Finish the pending
 * write, take the LAUNCH reply if it is still coming (within close_ms), then STOP that exact session. A session we
 * cannot show we own is never stopped: the outcome is reported as uncertain. */
static int rollback(cast_session *s, int64_t dl)
{
    int r = s->w.tx_len ? flush_until(s, dl) : RT_OK;
    int64_t wdl = now_ms() + (dl - now_ms()) / 2; /* the reply gets half the budget; STOP and the CLOSEs keep the rest */
    char b[CM_JSON];
    s->phase = PH_LAUNCH; s->pend = s->launch_id;
    while (!r && !s->owned_set && s->lst == 2 && now_ms() < wdl) {
        int k = cast_wire_read(&s->w, wdl);
        if (k == CW_MESSAGE) on_msg(s);
        else if (k == RT_READ || k == RT_WRITE) waitfd(s, k == RT_READ ? POLLIN : POLLOUT, wdl);
        else r = k;
    }
    if (!r && s->owned_set) {
        if (!cm_encode_stop(b, sizeof b, ++s->req, s->owned.session_id)) r = RT_ARGUMENT;
        else if (!(r = cast_wire_queue(&s->w, "sender-0", "receiver-0", NS_RECV, b))) r = flush_until(s, dl);
        s->cleanup = r ? CS_CLEAN_UNCERTAIN : CS_CLEAN_STOPPED;
    } else if (r || s->lst == 2) s->cleanup = CS_CLEAN_UNCERTAIN;
    return r;
}
/* [rollback], CLOSE the app transport (once its CONNECT was queued), CLOSE receiver-0, TLS close_notify, all within
 * one close_ms deadline. Nothing here touches a session we do not own. */
static int finish(cast_session *s)
{
    int r = 0;
    if (s->fin_clean) {
        int64_t dl = now_ms() + s->cfg.close_ms; cm_command c = { .kind = CM_CLOSE };
        if (s->lst == 2) r = rollback(s, dl);
        if (!r && (s->app_tx || s->live_app)) r = send_sync(s, s->owned.transport_id, c, dl);
        if (!r && s->live) r = send_sync(s, "receiver-0", c, dl);
        while (!r) {
            r = remote_tls_close(s->tls, dl);
            if (r != RT_READ && r != RT_WRITE) break;
            waitfd(s, r == RT_READ ? POLLIN : POLLOUT, dl); r = 0;
        }
        /* Half-close, then discard input until the peer closes: closing with unread input would reset the
         * connection and could drop our CLOSE frames in flight. Bounded by close_ms. */
        shutdown(s->fd, SHUT_WR);
        while (!r && now_ms() < dl) {
            char b[512]; ssize_t n = recv(s->fd, b, sizeof b, MSG_DONTWAIT);
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) break;
            if (n < 0) waitfd(s, POLLIN, dl);
        }
    }
    drop(s, 0); publish(s, (cs_state)s->fin, s->fin_err);
    return 0;
}
static int pop_op(cast_session *s, op_t *o)
{
    int ok = 0;
    pthread_mutex_lock(&s->mu);
    if (s->nops) { *o = s->ops[0]; ok = 1; memmove(s->ops, s->ops + 1, (size_t)--s->nops * sizeof *s->ops); }
    pthread_mutex_unlock(&s->mu);
    return ok;
}
static void sync_track(cast_session *s) /* a new cast_session_track() forgets the owned media session */
{
    pthread_mutex_lock(&s->mu);
    if (s->track_ver != s->seen_ver) {
        s->seen_ver = s->track_ver;
        memcpy(s->content, s->want, sizeof s->content);
        s->track_load = s->want_load;
        s->mid = 0;
        s->stopped = s->absence = s->probe_id = 0;
    }
    pthread_mutex_unlock(&s->mu);
}
static void op_error(cast_session *s, int e)
{
    pthread_mutex_lock(&s->mu); s->snap.op_error = e;
    if(e && s->command_op == CS_OP_SEEK) s->snap.seek_failures++;
    s->snap.generation++; pthread_mutex_unlock(&s->mu);
}
/* End the LOAD in flight; ignored (and forgotten) when a newer cast_session_load() superseded it. 1 = recorded. */
static int ld_end(cast_session *s, int state, int err)
{
    int ok;
    pthread_mutex_lock(&s->mu);
    if ((ok = s->ld_ver == s->ld_sent)) {
        if (state == CS_JOB_FAILED) restore_media(s);
        s->snap.load_state = state; s->snap.load_error = err; s->snap.generation++;
    }
    pthread_mutex_unlock(&s->mu);
    s->ld_id = 0;
    return ok;
}
static void stop_end(cast_session *s, int state, int err)
{
    fprintf(stderr, "Cast stop job_state=%d error=%d\n", state, err);
    pthread_mutex_lock(&s->mu); s->snap.stop_state = state; s->snap.stop_error = err; s->snap.generation++; pthread_mutex_unlock(&s->mu);
    s->stop_id = 0;
}
/* Frame for the newest unsent LOAD (a LOAD in flight is superseded); the request stays PENDING until its reply, load_ms. */
static int load_next(cast_session *s, cm_command *c, const char **dest, int64_t t)
{
    int take;
    pthread_mutex_lock(&s->mu);
    if ((take = s->ld_ver != s->ld_sent)) {
        s->ldw = s->ld;
        s->ld_sent = s->ld_ver;
    }
    pthread_mutex_unlock(&s->mu);

    if (take) {
        sync_track(s);
        s->ld_mid = s->ld_check = s->ld_cache_at = 0;
        memset(s->ld_cache, 0, sizeof s->ld_cache);
        *c = (cm_command){
            .kind = CM_LOAD, .time = s->ldw.start,
            .content_id = s->ldw.content, .content_type = s->ldw.type,
            .stream_type = "BUFFERED", .title = s->ldw.title,
            .artist = s->ldw.artist, .album = s->ldw.album,
            .image = *s->ldw.image ? s->ldw.image : NULL
        };
        s->pend_dl = t + s->cfg.load_ms;
    } else if (s->ld_id && s->ld_check == 1) {
        /* Ask for all current media, not merely the historical candidate.
         * Keep the original LOAD deadline. */
        *c = (cm_command){ .kind = CM_GET_STATUS };
        s->ld_check = 2;
    } else return 0;

    s->ld_id = c->request_id = ++s->req;
    *dest = s->owned.transport_id;
    return 1;
}
static int media_ended(const cs_snapshot *n)
{
    return !n->media_id ||
        (n->player == CS_PL_IDLE &&
         (n->idle == CS_IDLE_CANCELLED || n->idle == CS_IDLE_FINISHED));
}
/* STOP of the owned media session; with nothing owned (a LOAD that failed) it is done without a frame. */
static int stop_next(cast_session *s, cm_command *c, const char **dest, int64_t t)
{
    int req, ended;
    pthread_mutex_lock(&s->mu); if ((req = s->stop_req)) s->stop_req = 0; pthread_mutex_unlock(&s->mu);
    if (!req) return 0;
    sync_track(s);
    pthread_mutex_lock(&s->mu);
    ended = media_ended(&s->snap);
    pthread_mutex_unlock(&s->mu);
    if (!s->mid || ended) { stop_end(s, CS_JOB_OK, 0); return 0; }
    *c = (cm_command){ .kind = CM_STOP, .media_session_id = s->mid };
    s->stop_id = c->request_id = ++s->req; s->pend_dl = t + s->cfg.confirm_ms; *dest = s->owned.transport_id;
    return 1;
}
/* Build the frame for one queued op; 0 = refused (nothing owned to command, or receiver level/mute unknown). */
static int start_op(cast_session *s, const op_t *o, cm_command *c, const char **dest, int64_t t)
{
    int media = o->op >= CS_OP_PLAY && o->op <= CS_OP_SEEK; cm_volume v;
    *c = (cm_command){ 0 }; s->command_op = o->op; sync_track(s);
    if (media && (!s->mid || s->stopped || o->track_ver != s->seen_ver)) return 0;
    switch (o->op) {
    case CS_OP_STATUS: c->kind = CM_GET_STATUS; c->receiver = 1; break;
    case CS_OP_PLAY: c->kind = CM_PLAY; break;
    case CS_OP_PAUSE: c->kind = CM_PAUSE; break;
    case CS_OP_SEEK: c->kind = CM_SEEK; c->time = o->v; break;
    case CS_OP_VOLUME_STEP:
    case CS_OP_VOLUME:
        pthread_mutex_lock(&s->mu); v = s->snap.volume; pthread_mutex_unlock(&s->mu);
        c->kind = CM_SET_VOLUME; c->receiver = 1;
        c->level = o->v >= 0 ? o->v : v.present & CM_LEVEL ? v.level : -1;
        c->muted = o->muted >= 0 ? o->muted : v.present & CM_MUTED ? v.muted : -1;
        if (o->op == CS_OP_VOLUME_STEP) {
            if (!(v.present & CM_LEVEL)) return 0;
            c->level = round((v.level + o->v * 0.01) * 100) / 100;
            if (c->level < 0) c->level = 0;
            if (c->level > 1) c->level = 1;
        }
        if (c->level < 0 || c->muted < 0) return 0;
        break;
    default: return 0;
    }
    if (media) { c->media_session_id = s->mid; *dest = s->owned.transport_id; }
    s->pend = c->request_id = ++s->req; s->pend_media = media; s->pend_dl = t + s->cfg.confirm_ms;
    op_error(s, 0); /* op_error describes the command in flight or the last one */
    return 1;
}
/* Alternate heartbeat and control frames when both are pending; keep one transmit slot. */
static int next_control(cast_session *s, cm_command *c, int64_t t, const char **dest)
{
    if (s->phase == PH_READY && s->pend && s->pend_media && !s->command_op) {
        int urgent;
        pthread_mutex_lock(&s->mu);
        urgent = s->nops || s->stop_req || s->ld_ver != s->ld_sent;
        pthread_mutex_unlock(&s->mu);
        if (urgent) s->pend = 0; /* abandon only a background poll; late replies cannot confirm a newer request */
    }
    if (s->todo & T_CONN) {
        c->kind = CM_CONNECT; s->todo &= ~T_CONN; s->conn_tx = 1;
    } else if (s->todo & T_STATUS) {
        c->kind = CM_GET_STATUS; c->receiver = 1; s->todo &= ~T_STATUS;
        s->pend = c->request_id = ++s->req;
    } else if (s->todo & T_LAUNCH) {
        c->kind = CM_LAUNCH; s->todo &= ~T_LAUNCH; s->launch_id = s->pend = c->request_id = ++s->req;
        s->ldl = t + s->cfg.launch_ms; s->phase = PH_LAUNCH; s->lst = 1; publish(s, CS_LAUNCHING, 0);
    } else if (s->todo & T_APP) {
        c->kind = CM_CONNECT; s->todo &= ~T_APP; s->app_tx = 1; *dest = s->owned.transport_id;
    } else if (s->phase == PH_READY && !s->pend) {
        op_t o = { 0 };
        if (!s->stop_id && load_next(s, c, dest, t)) return 1;
        if (!s->ld_id && !s->stop_id && stop_next(s, c, dest, t)) return 1;
        if (s->ld_id || s->stop_id) return 0; /* one command at a time */
        while (!c->request_id && pop_op(s, &o)) if (!start_op(s, &o, c, dest, t)) op_error(s, CS_E_REFUSED);
        sync_track(s);
        if (!c->request_id && s->mid && !s->stopped && (s->absence || t >= s->next_media_status)) {
            *c = (cm_command){ .kind = CM_GET_STATUS, .request_id = ++s->req };
            *dest = s->owned.transport_id;
            s->pend = c->request_id; s->pend_media = 1; s->command_op = 0;
            s->probe_id = c->request_id; s->probe_ver = s->seen_ver; s->absence = 0;
            s->pend_dl = t + s->cfg.confirm_ms;
            s->next_media_status = t + 1000;
        }
        if (!c->request_id) return 0;
    } else return 0;
    return 1;
}
static int next_msg(cast_session *s)
{
    int64_t t = now_ms(); cm_command c = { 0 }; const char *dest = "receiver-0"; int r;
    if (s->hb_last && next_control(s, &c, t, &dest)) s->hb_last = 0;
    else if (s->pong_owed) { c.kind = CM_PONG; s->pong_owed = 0; s->hb_last = 1; }
    else if (s->hb && t >= s->next_ping) {
        c.kind = CM_PING; s->next_ping = t + s->cfg.ping_ms; s->hb_last = 1;
    } else if (next_control(s, &c, t, &dest)) s->hb_last = 0;
    else return 0;
    if ((r = queue(s, dest, c))) fin(s, CS_FAILED, r, 0, "command queue failed");
    return 1;
}
/* Evidence that the receiver or the app session is no longer ours: report it, drop every queued and unsent command, and
 * send neither STOP nor a volume restoration (a pending half-written frame is only completed, never a fresh one). */
static void taken(cast_session *s, const cm_app *by)
{
    if (s->fin) return;
    pthread_mutex_lock(&s->mu);
    snprintf(s->snap.taken_by, CM_ID, "%s", by ? *by->display_name ? by->display_name : by->app_id : "");
    s->nops = 0; pthread_mutex_unlock(&s->mu);
    s->pend = 0; s->live_app = 0;
    if (!s->w.tx_used) s->w.tx_len = 0;
    fin(s, CS_TAKEN_OVER, CS_E_TAKEOVER, 1, "takeover");
}
static int word(const char *v, const char *const *t, int n)
{
    for (int i = 0; i < n; i++) if (!strcmp(v, t[i])) return i + 1;
    return *v ? n + 1 : 0;
}
/* MEDIA_STATUS of the owned app transport. The owned media session is adopted from the entry carrying the tracked
 * contentId; afterwards an entry is ours only if cm_identity accepts it against the stored identity (a missing media block
 * means no content to compare). A lower mediaSessionId is a stale leftover; a higher one, or another content, is a takeover. */
static void snap_media(cast_session *s, const cm_media *e) /* mu held */
{
    static const char *const pl[] = { "BUFFERING", "PLAYING", "PAUSED", "IDLE" }, *const ir[] = { "FINISHED", "ERROR", "INTERRUPTED", "CANCELLED" };
    if(s->snap.media_id != s->mid) s->next_media_status = now_ms() + 1000;
    s->snap.media_id = s->mid;
    if (*e->player_state) { s->snap.player = word(e->player_state, pl, 4); s->snap.idle = word(e->idle_reason, ir, 4); }
    if (e->present & CM_TIME) s->snap.media_time = e->current_time;
    if (s->snap.player == CS_PL_IDLE && s->snap.idle == CS_IDLE_CANCELLED) {
        s->stopped = s->snap.stopped = 1;
        s->snap.confirmed = 0;
    }
    s->snap.generation++;
    remember_media(s);
}
/* A LOAD is proven in two steps. ID-0 statuses only feed ld_cache (contentId by mediaSessionId). The status answering the LOAD id
 * nominates a candidate; the status answering a follow-up media GET_STATUS must list that mediaSessionId with the expected
 * contentId (own or cached) before it becomes the owned one. A different contentId, or a candidate that is gone while other media
 * is listed, is a takeover. */
static void load_msg(cast_session *s, const cm_status *st)
{
    size_t i, j;
    int current;

    pthread_mutex_lock(&s->mu);
    current = s->ld_ver == s->ld_sent &&
              s->track_ver == s->seen_ver;
    pthread_mutex_unlock(&s->mu);
    if (!current) return;

    /* Content can arrive in a broadcast before the correlated response.
     * Cache by mediaSessionId; a bounded eviction can only lose evidence,
     * never manufacture a match. */
    for (i = 0; i < st->media_count; i++) {
        const cm_media *e = &st->media[i];
        if (!(e->present & CM_MEDIA_ID) || !*e->content_id) continue;
        for (j = 0; j < CM_ENTRIES; j++)
            if (s->ld_cache[j].media_session_id == e->media_session_id)
                break;
        if (j == CM_ENTRIES) {
            j = (size_t)s->ld_cache_at;
            s->ld_cache_at = (s->ld_cache_at + 1) % CM_ENTRIES;
        }
        s->ld_cache[j] = *e;
    }

    if (!st->request_id || st->request_id != s->ld_id ||
        !st->has_status) return;

    if (!s->ld_check) {
        for (i = 0; i < st->media_count; i++) {
            const cm_media *e = &st->media[i];
            if (!(e->present & CM_MEDIA_ID)) continue;
            if (*e->content_id &&
                strcmp(e->content_id, s->ldw.content)) continue;
            s->ld_mid = e->media_session_id;
            s->ld_check = 1;
            return; /* correlated LOAD evidence, not success yet */
        }
        return;
    }

    if (s->ld_check != 2) return;
    for (i = 0; i < st->media_count; i++) {
        cm_media e = st->media[i];
        if (!(e.present & CM_MEDIA_ID) ||
            e.media_session_id != s->ld_mid) continue;
        if (!*e.content_id) {
            for (j = 0; j < CM_ENTRIES; j++)
                if (s->ld_cache[j].media_session_id == s->ld_mid) {
                    strcpy(e.content_id, s->ld_cache[j].content_id);
                    break;
                }
        }
        if (!*e.content_id) return; /* identity still unproved */
        if (strcmp(e.content_id, s->ldw.content)) {
            taken(s, NULL);
            return;
        }
        if (!strcmp(e.player_state, "IDLE")) {
            if (!strcmp(e.idle_reason, "INTERRUPTED")) taken(s, NULL);
            else ld_end(s, CS_JOB_FAILED, CS_E_LOAD_FAILED);
            return;
        }

        pthread_mutex_lock(&s->mu);
        if (s->ld_ver == s->ld_sent &&
            s->track_ver == s->seen_ver) {
            s->mid = e.media_session_id;
            snap_media(s, &e);
            s->snap.load_state = CS_JOB_OK;
            s->snap.load_error = 0;
            s->ld_id = 0;
        }
        pthread_mutex_unlock(&s->mu);
        return;
    }

    if (st->media_count) taken(s, NULL);
    else ld_end(s, CS_JOB_FAILED, CS_E_LOAD_FAILED);
}
static void media_msg(cast_session *s, const char *p)
{
    cm_status st; size_t i;
    if (!cm_parse(p, strlen(p), s->ld_id ? s->ld_id : s->stop_id ? s->stop_id : s->pend, &st)) return;
    if (strcmp(st.type, "MEDIA_STATUS")) { /* refusal of our own media command */
        if (s->ld_id && st.request_id == s->ld_id) ld_end(s, CS_JOB_FAILED, CS_E_LOAD_FAILED);
        else if (s->stop_id && st.request_id == s->stop_id) stop_end(s, CS_JOB_FAILED, CS_E_STOP_FAILED);
        else if (s->pend_media && st.request_id == s->pend) { s->pend = 0; op_error(s, CS_E_REFUSED); }
        return;
    }
    sync_track(s);
    if (s->ld_id) { load_msg(s, &st); return; }
    /* Nonzero replies must belong to a live request. In particular a delayed reply
     * to an older LOAD or poll cannot change a replacement, even if its ID is reused. */
    if (st.request_id && st.request_id != s->pend && st.request_id != s->stop_id) return;
    int probe = st.request_id && st.request_id == s->probe_id &&
                st.request_id == s->pend && s->probe_ver == s->seen_ver;
    if (st.has_status && !st.media_count) {
        if (!s->mid || s->stopped) return;
        if (!probe && !(s->stop_id && st.request_id == s->stop_id)) {
            s->absence = 1; return; /* broadcasts carry no track identity */
        }
        pthread_mutex_lock(&s->mu);
        if (s->track_ver == s->seen_ver) {
            cm_media ended = {0};
            strcpy(ended.player_state, "IDLE"); strcpy(ended.idle_reason, "CANCELLED");
            snap_media(s, &ended);
        }
        pthread_mutex_unlock(&s->mu);
        if (probe) s->pend = s->probe_id = 0;
        if (s->stop_id) stop_end(s, CS_JOB_OK, 0);
        return;
    }
    for (i = 0; i < st.media_count; i++) {
        const cm_media *e = &st.media[i];
        if (!(e->present & CM_MEDIA_ID)) continue;
        if (!s->mid && !s->track_load && *s->content && cm_identity(1, 1, &s->owned, e, &s->owned, 0, s->content)) s->mid = e->media_session_id;
        if (!s->mid || e->media_session_id < s->mid) continue;
        /* A delayed cancellation may carry an old ID/content pair. Confirm before
         * applying either stop or takeover to this generation. Active unknown media
         * still takes over below; a probe can also prove a different current identity. */
        if (!strcmp(e->player_state, "IDLE") &&
            !strcmp(e->idle_reason, "CANCELLED") && s->seen_ver > 1 && !probe && !s->stop_id) {
            if (!s->stopped) s->absence = 1;
            continue;
        }
        if (!cm_identity(1, 1, &s->owned, e, &s->owned, s->mid, *e->content_id ? s->content : NULL)) { taken(s, NULL); return; }
        if (s->stopped) continue; /* delayed playing/finished cannot revive a stopped occurrence */
        pthread_mutex_lock(&s->mu);
        if (s->track_ver == s->seen_ver) snap_media(s, e); /* not forgotten meanwhile */
        pthread_mutex_unlock(&s->mu);
        if (s->stop_id && e->media_session_id == s->mid && !strcmp(e->player_state, "IDLE") &&
            (!strcmp(e->idle_reason, "CANCELLED") || !strcmp(e->idle_reason, "FINISHED"))) stop_end(s, CS_JOB_OK, 0); /* our media ended */
        if (s->pend_media && st.request_id && st.request_id == s->pend) {
            s->pend = 0; pthread_mutex_lock(&s->mu); if(s->command_op && !s->snap.stopped) s->snap.confirmed = st.request_id; s->snap.generation++; pthread_mutex_unlock(&s->mu);
        }
    }
}
/* Every message is addressed to us. It comes from receiver-0 (the endpoint we CONNECTed to), or, for the media namespace
 * once READY, from the owned app transport. */
static void on_msg(cast_session *s)
{
    const openscreen_cast_proto_CastMessage *m = &s->w.message;
    const char *p = m->payload_utf8, *ns = m->namespace;
    cm_status st; char type[CM_ID]; size_t i;
    if (strcmp(m->destination_id, "sender-0") && strcmp(m->destination_id, "*")) return;
    if (!strcmp(ns, NS_MEDIA)) { if (s->phase == PH_READY && s->owned_set && !strcmp(m->source_id, s->owned.transport_id)) media_msg(s, p); return; }
    if (strcmp(m->source_id, "receiver-0")) {
        if (s->phase == PH_READY && s->owned_set &&
            !strcmp(m->source_id, s->owned.transport_id) &&
            !strcmp(ns, NS_CON) &&
            cm_type(p, strlen(p), type, sizeof type) &&
            !strcmp(type, "CLOSE"))
            fin(s, CS_LOST, 0, 0, "app transport closed");
        return;
    }
    if (!strcmp(ns, NS_HB) || !strcmp(ns, NS_CON)) {
        if (!cm_type(p, strlen(p), type, sizeof type)) return;
        if (!strcmp(ns, NS_HB)) { if (!strcmp(type, "PING")) s->pong_owed = 1; return; }
        if (!strcmp(type, "CLOSE")) fin(s, CS_LOST, 0, 1, "receiver closed");
        return;
    }
    if (strcmp(ns, NS_RECV) || !cm_parse(p, strlen(p), s->pend, &st)) return;
    if (s->phase == PH_READY && !s->pend_media && s->pend > 0 &&
        st.request_id == s->pend && !strcmp(st.type, "INVALID_REQUEST")) {
        s->pend = 0;
        op_error(s, CS_E_REFUSED);
        return;
    }
    if (!strcmp(st.type, "LAUNCH_ERROR")) { /* only the refusal of our own LAUNCH request */
        if (s->phase == PH_LAUNCH && s->launch_id > 0 && st.request_id == s->launch_id) { s->lst = 3; s->pend = 0; fin(s, CS_FAILED, CS_E_LAUNCH, 1, "launch rejected"); }
        return;
    }
    if (strcmp(st.type, "RECEIVER_STATUS")) return;
    if (s->phase < PH_READY && (s->pend <= 0 || st.request_id != s->pend)) return; /* only the reply to our request */
    if (s->phase == PH_READY && st.has_apps) { /* an explicit list must contain our exact identity */
        for (i = 0; i < st.app_count && !cm_identity(1, 1, &st.apps[i], NULL, &s->owned, 0, NULL); i++);
        if (i == st.app_count) {
            for (i = 0; i < st.app_count && st.apps[i].idle; i++);
            taken(s, st.app_count ? &st.apps[i == st.app_count ? 0 : i] : NULL);
            return;
        }
    }
    pthread_mutex_lock(&s->mu); /* absent fields keep the last known level/mute */
    if (st.volume.present & CM_LEVEL) { s->snap.volume.level = st.volume.level; s->snap.volume.present |= CM_LEVEL; }
    if (st.volume.present & CM_MUTED) { s->snap.volume.muted = st.volume.muted; s->snap.volume.present |= CM_MUTED; }
    if (s->phase == PH_READY && !s->pend_media && st.request_id && st.request_id == s->pend) s->snap.confirmed = st.request_id;
    s->snap.generation++; pthread_mutex_unlock(&s->mu);
    if (s->phase == PH_LAUNCH) { /* ownership comes only from the status answering our LAUNCH request id */
        for (i = 0; i < st.app_count; i++)
            if (!strcmp(st.apps[i].app_id, CM_APP) && *st.apps[i].session_id && *st.apps[i].transport_id) {
                s->owned = st.apps[i]; s->owned_set = 1; s->pend = 0; s->todo |= T_APP; s->phase = PH_APP; break;
            }
        return;
    }
    if (st.request_id && st.request_id == s->pend) s->pend = 0;
    if (s->phase != PH_STATUS) return;
    if (s->owned_set) { /* reattach: exactly the app/session/transport we own, else it is gone; never LAUNCH */
        for (i = 0; i < st.app_count && !cm_identity(1, 1, &st.apps[i], NULL, &s->owned, 0, NULL); i++);
        if (i == st.app_count) { s->lst = 3; fin(s, CS_LOST, 0, 1, "owned app missing"); }
        else { s->ldl = now_ms() + s->cfg.launch_ms; s->todo |= T_APP; s->phase = PH_APP; }
        return;
    }
    for (i = 0; i < st.app_count && st.apps[i].idle; i++); /* an idle screen (Backdrop) is not a running app */
    if (i < st.app_count && !s->cfg.replace) {
        const cm_app *a = &st.apps[i];
        pthread_mutex_lock(&s->mu);
        snprintf(s->snap.busy_app, CM_ID, "%s", a->app_id);
        snprintf(s->snap.busy_name, CM_ID, "%s", *a->display_name ? a->display_name : *a->app_id ? a->app_id : "Another app");
        pthread_mutex_unlock(&s->mu);
        fin(s, CS_BUSY, 0, 1, "receiver busy");
    } else s->todo |= T_LAUNCH;
}
static void soon(int64_t *u, int64_t v) { if (v < *u) *u = v; }
static int timers(cast_session *s)
{
    int64_t t = now_ms();
    if (!s->fin && s->w.tx_len && t >= s->tx_dl) return CS_E_STALL;
    if (!s->fin && s->phase == PH_STATUS && t >= s->cdl) fin(s, CS_FAILED, RT_TIMEOUT, 1, "status timeout");
    if (!s->fin && (s->phase == PH_LAUNCH || s->phase == PH_APP) && t >= s->ldl) fin(s, CS_FAILED, CS_E_LAUNCH_TIMEOUT, 1, "launch timeout");
    if (!s->fin && s->phase == PH_READY && s->pend && t >= s->pend_dl) {
        s->pend = 0; op_error(s, RT_TIMEOUT);
    }
    if (!s->fin && s->phase == PH_READY && (s->ld_id || s->stop_id) && t >= s->pend_dl) {
        if (s->ld_id) ld_end(s, CS_JOB_FAILED, CS_E_LOAD_TIMEOUT); else stop_end(s, CS_JOB_FAILED, CS_E_STOP_TIMEOUT);
    }
    return 0;
}
static int run(cast_session *s)
{
    int64_t t = now_ms(), until; int r;
    s->cdl = t + s->cfg.connect_ms; s->hs = 1; s->fin = s->hb = s->live = s->pend = s->ld_id = s->stop_id = s->pong_owed = s->conn_tx = s->hb_last = s->app_tx = s->live_app = 0;
    s->phase = PH_STATUS; s->todo = T_CONN | T_STATUS; s->next_media_status = 0;
    s->fd = s->cfg.connect ? s->cfg.connect(s->cfg.ctx, s->host, s->cfg.port, s->cdl) : dial(s);
    if (s->fd < 0) return flag(&s->cancel) ? RT_CANCEL : now_ms() >= s->cdl ? RT_TIMEOUT : RT_IO;
    r = remote_tls_open(&s->tls, s->fd, RT_DEVICE_UNVERIFIED, &(remote_tls_hooks){ .ctx = s, .cancelled = hook }, s->cdl);
    if (!r) {
        cast_wire_init(&s->w, s->tls);
        while ((r = remote_tls_handshake(s->tls, s->cdl)) == RT_READ || r == RT_WRITE)
            waitfd(s, r == RT_READ ? POLLIN : POLLOUT, s->cdl);
    }
    s->hs = 0;
    if (r) return drop(s, r);
    s->last_rx = now_ms(); /* silence is measured from the end of setup, not from before TCP/TLS */
    for (;;) {
        int ev = POLLIN, again = 0, n, rr = RT_READ;
        if (flag(&s->cancel) || flag(&s->close_req)) fin(s, CS_CLOSED, flag(&s->cancel) ? RT_CANCEL : 0, 1, "cancel or close requested");
        if ((r = timers(s))) return drop(s, r);
        if (s->fin) return finish(s);
        if (!s->fin && !s->w.tx_len) again = next_msg(s); /* no unconditional restart: receive and timers still run */
        if (!s->fin && s->lst == 1) { /* LAUNCH queued, nothing written: Cancel discards it; after this point it is committed */
            TP(s, "launch-queued");
            if (flag(&s->cancel) || flag(&s->close_req)) { s->w.tx_len = 0; s->lst = 0; s->pend = 0; }
            else s->lst = 2;
        }
        if (!s->fin && s->w.tx_len) {
            if (!(r = cast_wire_flush(&s->w, s->tx_dl))) {
                again = 1;
                if (s->conn_tx) { s->conn_tx = 0; s->live = s->hb = 1; s->next_ping = now_ms() + s->cfg.ping_ms; }
                if (s->app_tx) { /* READY only once flushed, and only if Cancel/launch expiry did not arrive meanwhile */
                    s->app_tx = 0;
                    s->live_app = 1;
                    if (flag(&s->cancel) || flag(&s->close_req))
                        fin(s, CS_CLOSED, flag(&s->cancel) ? RT_CANCEL : 0, 1, "cancel or close requested");
                    if ((r = timers(s))) return drop(s, r);
                    if (s->fin) return finish(s);
                    s->phase = PH_READY;
                    if (s->lst == 2) s->lst = 4;
                    publish(s, CS_READY, 0);
                }
            } else if (r == RT_WRITE) ev |= POLLOUT; else if (r != RT_READ) return drop(s, r == RT_TIMEOUT ? CS_E_STALL : r);
        }
        for (n = 0; n < 8 && !s->fin && !flag(&s->cancel) && !flag(&s->close_req); n++) { /* bounded batch */
            if ((rr = cast_wire_read(&s->w, INF)) != CW_MESSAGE) break;
            if ((r = timers(s))) return drop(s, r);
            if (s->fin) return finish(s);
            s->last_rx = now_ms(); on_msg(s);
        }
        if (n && !s->w.tx_len) again = 1; /* a message may have scheduled a frame (LAUNCH, app CONNECT, PONG, next command) */
        if (n == 8) again = 1;
        else if (rr == RT_WRITE) ev |= POLLOUT;
        else if (rr != RT_READ && rr != CW_MESSAGE) return drop(s, rr);
        t = now_ms();
        if ((r = timers(s))) return drop(s, r);
        if (s->fin) return finish(s);
        if (t - s->last_rx >= s->cfg.dead_ms) return drop(s, CS_E_DEAD);
        until = again ? t : s->last_rx + s->cfg.dead_ms;
        if (s->hb) soon(&until, s->next_ping);
        if (s->phase == PH_READY && s->mid && !s->stopped && !s->pend && !s->ld_id && !s->stop_id && !s->w.tx_len)
            soon(&until, s->next_media_status);
        if (s->w.tx_len) soon(&until, s->tx_dl);
        if (s->phase == PH_STATUS) soon(&until, s->cdl);
        if (s->phase == PH_LAUNCH || s->phase == PH_APP) soon(&until, s->ldl);
        if (s->phase == PH_READY && (s->pend || s->ld_id || s->stop_id)) soon(&until, s->pend_dl);
        waitfd(s, ev, until);
    }
}
/* After loss, only re-inspect and reattach the session we already own (exact ids); run() never LAUNCHes then. */
static void *worker(void *p)
{
    cast_session *s = p; int tries = s->cfg.reconnect, r;
    while ((r = run(s)) != 0) {
        if (r != RT_CANCEL && s->owned_set && tries-- > 0 && !flag(&s->cancel) && !flag(&s->close_req)) {
            publish(s, CS_CONNECTING, r); continue;
        }
        fprintf(stderr, "Cast session end reason=%s error=%d phase=%d\n", r == RT_CANCEL ? "cancel" : "transport failed", r, s->phase);
        publish(s, r == RT_CANCEL ? CS_CLOSED : CS_FAILED, r);
        break;
    }
    return NULL;
}
int cast_session_start(cast_session **out, const cast_session_cfg *c)
{
    cast_session *s; pthread_attr_t a;
    if (!out) return RT_ARGUMENT;
    *out = NULL;
    if (!c || !c->host || strlen(c->host) >= sizeof s->host || c->port < 1 || c->port > 65535 || c->reconnect < 0 || c->launch_ms <= 0 ||
        c->connect_ms <= 0 || c->load_ms <= 0 || c->confirm_ms <= 0 || c->close_ms <= 0 ||
        c->ping_ms <= 0 || c->dead_ms <= c->ping_ms) return RT_ARGUMENT;
    if (!(s = calloc(1, sizeof *s))) return RT_MEMORY;
    dn_sender_copy(s->sender_name);
    s->cfg = *c; strcpy(s->host, c->host); s->cfg.host = s->host; s->fd = -1;
    s->snap.state = CS_CONNECTING; s->snap.generation = 1;
    pthread_mutex_init(&s->mu, NULL);
    if (pipe2(s->wake, O_NONBLOCK | O_CLOEXEC)) { free(s); return RT_IO; }
    pthread_attr_init(&a); pthread_attr_setstacksize(&a, 256 * 1024);
    if (pthread_create(&s->thread, &a, worker, s)) {
        close(s->wake[0]); close(s->wake[1]); pthread_mutex_destroy(&s->mu); free(s); pthread_attr_destroy(&a);
        return RT_MEMORY;
    }
    pthread_attr_destroy(&a);
    *out = s;
    return RT_OK;
}
uint64_t cast_session_snapshot(cast_session *s, cs_snapshot *o)
{
    pthread_mutex_lock(&s->mu); *o = s->snap; pthread_mutex_unlock(&s->mu);
    return o->generation;
}
static int enqueue(cast_session *s, cs_op op, double v, int muted)
{
    int r = RT_ARGUMENT;
    if (!s) return r;
    pthread_mutex_lock(&s->mu); /* only an owned, READY session takes commands; media ops also need the owned media session */
    if (s->snap.state == CS_READY && s->nops < 4 && (op == CS_OP_STATUS || op == CS_OP_VOLUME || op == CS_OP_VOLUME_STEP || (s->snap.media_id && !s->snap.stopped))) {
        s->ops[s->nops].op = op;
        s->ops[s->nops].v = v;
        s->ops[s->nops].muted = muted;
        s->ops[s->nops++].track_ver = s->track_ver;
        r = RT_OK;
    }
    pthread_mutex_unlock(&s->mu);
    wake(s);
    return r;
}
int cast_session_submit(cast_session *s, cs_op op)
{
    return op == CS_OP_STATUS || op == CS_OP_PLAY || op == CS_OP_PAUSE ? enqueue(s, op, 0, -1) : RT_ARGUMENT;
}
int cast_session_seek(cast_session *s, double t) { return t >= 0 && t <= INT32_MAX ? enqueue(s, CS_OP_SEEK, t, -1) : RT_ARGUMENT; }
int cast_session_volume(cast_session *s, double level, int muted)
{
    return level <= 1 && muted >= -1 && muted <= 1 && (level >= 0 || muted >= 0) ? enqueue(s, CS_OP_VOLUME, level < 0 ? -1 : level, muted) : RT_ARGUMENT;
}
int cast_session_volume_step(cast_session *s, int direction)
{
    return direction == 1 || direction == -1 ? enqueue(s, CS_OP_VOLUME_STEP, direction, -1) : RT_ARGUMENT;
}
int cast_session_track(cast_session *s, const char *id)
{
    if (!s || !id || !*id || strlen(id) >= CM_TEXT) return RT_ARGUMENT;
    pthread_mutex_lock(&s->mu);
    strcpy(s->want, id); s->want_load = 0; s->track_ver++;
    s->retained.media_id = 0; /* explicit tracking deliberately forgets ownership */
    s->snap.media_id = s->snap.player = s->snap.idle = s->snap.stopped = s->snap.confirmed = 0; s->snap.media_time = 0; s->snap.generation++;
    pthread_mutex_unlock(&s->mu);
    wake(s);
    return RT_OK;
}
int cast_session_load(cast_session *s, const cs_load *r)
{
    cm_command c; char b[CM_JSON]; int rc = RT_ARGUMENT;
    if (!s || !r || !r->content_id || !r->content_type || !(r->start >= 0 && r->start <= INT32_MAX)) return rc;
    c = (cm_command){ .kind = CM_LOAD, .request_id = 1, .time = r->start, .content_id = r->content_id, .content_type = r->content_type, .stream_type = "BUFFERED",
        .title = r->title, .artist = r->artist, .album = r->album, .image = r->image && *r->image ? r->image : NULL };
    if (!cm_encode(b, sizeof b, &c)) return rc; /* sizes, UTF-8 and required fields: what the worker would send */
    pthread_mutex_lock(&s->mu);
    if (s->snap.state == CS_READY) {
        ldreq *q = &s->ld;
        snprintf(q->content, sizeof q->content, "%s", r->content_id); snprintf(q->type, sizeof q->type, "%s", r->content_type);
        snprintf(q->title, sizeof q->title, "%s", r->title ? r->title : ""); snprintf(q->artist, sizeof q->artist, "%s", r->artist ? r->artist : "");
        snprintf(q->album, sizeof q->album, "%s", r->album ? r->album : ""); snprintf(q->image, sizeof q->image, "%s", r->image ? r->image : "");
        if (s->stop_req) { s->stop_req = 0; s->snap.stop_state = CS_JOB_FAILED; s->snap.stop_error = CS_E_STOP_FAILED; } /* superseded, not confirmed stopped */
        q->start = r->start; s->ld_ver++;
        strcpy(s->want, q->content); s->want_load = 1; s->track_ver++; /* like cast_session_track(): the owned media session is forgotten */
        s->snap.media_id = s->snap.player = s->snap.idle = s->snap.stopped = s->snap.confirmed = 0; s->snap.media_time = 0;
        s->snap.load_state = CS_JOB_PENDING; s->snap.load_error = 0; s->snap.load_seq++; s->snap.generation++;
        rc = RT_OK;
    }
    pthread_mutex_unlock(&s->mu);
    wake(s);
    return rc;
}
int cast_session_stop_media(cast_session *s)
{
    int rc = RT_OK;
    if (!s) return RT_ARGUMENT;
    pthread_mutex_lock(&s->mu);
    if (s->snap.stop_state == CS_JOB_PENDING) { /* already asked: one STOP */ }
    else if (s->snap.load_state != CS_JOB_PENDING && media_ended(&s->snap)) {
        s->snap.stop_state = CS_JOB_OK; s->snap.stop_error = 0; s->snap.stop_seq++; s->snap.generation++;
    } else if (s->snap.state != CS_READY) rc = RT_ARGUMENT;
    else { s->stop_req = 1; s->snap.stop_state = CS_JOB_PENDING; s->snap.stop_error = 0; s->snap.stop_seq++; s->snap.generation++; }
    pthread_mutex_unlock(&s->mu);
    wake(s);
    return rc;
}
void cast_session_cancel(cast_session *s) { __atomic_store_n(&s->cancel, 1, __ATOMIC_RELEASE); wake(s); }
void cast_session_close(cast_session *s) { __atomic_store_n(&s->close_req, 1, __ATOMIC_RELEASE); wake(s); }
void cast_session_free(cast_session *s)
{
    if (!s) return;
    cast_session_cancel(s); pthread_join(s->thread, NULL);
    close(s->wake[0]); close(s->wake[1]); pthread_mutex_destroy(&s->mu); free(s);
}
