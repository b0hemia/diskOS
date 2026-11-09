/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include "cast_ctlw.h"
#include "cast_format.h"
#include "sdio.h"

typedef struct { cc_ev e; int play_mode, pos_id; char path[TRACK_PATH_MAX]; uint32_t ep; } cw_ev;   /* ENTER fills play_mode / pos_id / path; the others only e. SELECT: e.ids is a malloc'd copy owned by the slot. ep: the epoch it was posted in */
struct cw {
    pthread_mutex_t mu; pthread_cond_t ev_cv, act_cv, idle_cv; pthread_t th;
    int closing, busy, force_local;
    uint32_t epoch, cur_ep;                      /* epoch: LOCALs posted so far; cur_ep: the epoch of the event the worker is running */
    cw_ev ev[CW_EV_N]; int en;
    cw_ev result; int have_result;   /* only the newest LOAD can still be pending */
    cw_act ac[CW_ACT_N]; int an;
    unsigned coalesced, dropped, lost;
    uint32_t (*now)(void);
    cw_view view;                                /* 14f: the published view, under mu */
    cast_ctl c;                                  /* never moves: allocated with the worker */
};

static cw *g_ui;
void cw_set_ui(cw *w){ g_ui = w; }
cw *cw_ui(void){ return g_ui; }

/* ---- selection start index (slice 14e2) ---- */
int cw_start_index(const int *ids, int n, int pos1, int tap_id){
    int i = pos1 < 1 ? 0 : pos1 - 1;
    if(!ids || n <= 0) return -1;
    if(tap_id <= 0) return i < n ? i : -1;                    /* no tapped id known: the position must at least be inside the list */
    if(i < n && ids[i] == tap_id) return i;
    for(i = 0; i < n; i++) if(ids[i] == tap_id) return i;     /* the list moved under the tap: the tapped song, wherever it is */
    return -1;                                                /* not in the list: refuse, never fall back to the first track */
}

/* ---- production deps ---- */
static int cw_classify(void *ctx, const cast_queue *q, int idx, int *dur_ms){
    const char *root = ctx; cc_row r; cast_format_info fi; size_t n = root ? strlen(root) : 0;
    *dur_ms = 0;
    if(cast_ctl_row_db(NULL, q, idx, &r)) return CF_V_UNKNOWN;
    *dur_ms = r.dur_ms;
    /* The LOAD gate (cb_serve_load_next) re-validates and re-classifies the path with the full component walk; this verdict only decides what
     * the cursor skips, so a plain O_NOFOLLOW open under the SD root is enough here. Any failure is UNKNOWN (inspected again before the LOAD). */
    if(!n || strncmp(r.path, root, n) || r.path[n] != '/' || !sd_io_begin()) return CF_V_UNKNOWN;
    int fd = open(r.path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if(fd < 0){ sd_io_end(); return CF_V_UNKNOWN; }
    cast_format_classify_fd(fd, q->flags[idx] & CQ_F_CUE, q->flags[idx] & CQ_F_ISO, &fi);
    close(fd); sd_io_end();
    return fi.verdict;
}
void cw_deps_sd(cast_ctl_deps *d, const char *root, int load_ms){
    uint64_t seed = 0; int f = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if(f < 0 || read(f, &seed, sizeof seed) != (ssize_t)sizeof seed){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); seed = (uint64_t)t.tv_nsec ^ ((uint64_t)t.tv_sec << 20) ^ (uint64_t)getpid(); }
    if(f >= 0) close(f);
    memset(d, 0, sizeof *d); d->classify = cw_classify; d->row = NULL; d->ctx = (void *)root; d->seed = seed; d->load_ms = load_ms;
}

/* ---- action mailbox: worker side ---- */
static void push(cw *w, const cc_act *a, const cc_row *row){
    pthread_mutex_lock(&w->mu);
    if(w->cur_ep != w->epoch){ pthread_mutex_unlock(&w->mu); return; }   /* 14e2 C2: a LOCAL was posted since this event was queued: its actions belong to a finished cast */
    if(a->kind == CC_A_REPUBLISH && w->an && w->ac[w->an - 1].a.kind == CC_A_REPUBLISH){   /* newest wins, but only into the TAIL (L2): never past a STAY_PAUSED / TOAST */
        w->ac[w->an - 1].a = *a; w->ac[w->an - 1].row = *row; w->ac[w->an - 1].a.row = &w->ac[w->an - 1].row; pthread_mutex_unlock(&w->mu); return;
    }
    if(a->kind == CC_A_TOAST && w->an && w->ac[w->an - 1].a.kind == CC_A_TOAST && w->ac[w->an - 1].a.toast == a->toast){ pthread_mutex_unlock(&w->mu); return; }
    while(w->an == CW_ACT_N && !w->closing) pthread_cond_wait(&w->act_cv, &w->mu);   /* only the worker ever waits */
    if(!w->closing && w->cur_ep == w->epoch){   /* (a LOCAL may have come while it waited) */
        cw_act *s = &w->ac[w->an++]; s->a = *a; if(row) s->row = *row; s->a.row = a->row ? &s->row : NULL;
    }
    pthread_mutex_unlock(&w->mu);
}
static void emit(cw *w, const cc_act *o, int n){
    for(int i = 0; i < n; i++){ cc_row r; if(o[i].row) r = *o[i].row; else memset(&r, 0, sizeof r); push(w, &o[i], &r); }
}
static void step(cw *w, cw_ev *x){
    cc_act o[CC_ACT_MAX]; int n; cast_queue q;
    if(x->e.type == CC_EV_ENTER){
        memset(&q, 0, sizeof q);
        if(cast_queue_snapshot(&q, x->play_mode, x->pos_id, x->path) != CQ_OK){   /* no queue: the handoff's one track keeps playing, nothing more */
            memset(o, 0, sizeof o); o[0].kind = CC_A_TOAST; o[0].toast = CC_T_LIST_FAILED; emit(w, o, 1); return;
        }
        x->e.q = &q;
        n = cast_ctl_step(&w->c, &x->e, o);
        if(q.n) cast_queue_free(&q);               /* refused (already active): the snapshot was not taken over */
    } else n = cast_ctl_step(&w->c, &x->e, o);
    emit(w, o, n);
}
/* 14f: the view, built from the worker-owned controller after each step (so only the worker reads the cursor), copied under mu */
static void publish(cw *w){
    cw_view v; const cast_ctl *c = &w->c;
    memset(&v, 0, sizeof v);
    if(c->active){
        const cc_slot *s = &c->s[c->a]; const cast_cursor *u = &s->cur;
        v.active = 1; v.qgen = c->qgen; v.pos = u->pos; v.mode = u->mode; v.n_queue = s->q.n; v.src = s->q.src;
        for(int i = 0; i < CW_VIEW_N; i++){
            int o = cast_cursor_at(u, u->pos + i);
            if(o < 0) break;
            v.occ[i] = o; v.id[i] = cast_queue_id_at(&s->q, o); v.fl[i] = s->q.flags[o]; v.cnt++;
        }
    }
    pthread_mutex_lock(&w->mu); w->view = v; pthread_mutex_unlock(&w->mu);
}
static void *run(void *p){
    cw *w = p; cw_ev x; int live;
    pthread_mutex_lock(&w->mu);
    for(;;){
        while(!w->closing && !w->en && !w->have_result && !w->force_local) pthread_cond_wait(&w->ev_cv, &w->mu);
        if(w->closing) break;
        if(w->force_local){ w->force_local = 0; memset(&x, 0, sizeof x); x.e.type = CC_EV_LOCAL; }
        else if(w->have_result){ x = w->result; w->have_result = 0; }
        else { x = w->ev[0]; memmove(w->ev, w->ev + 1, (size_t)(--w->en) * sizeof w->ev[0]); }
        if(x.e.type == CC_EV_LOCAL) x.ep = w->epoch;   /* a forced LOCAL has no slot; LOCAL emits no action, so its epoch is moot */
        w->cur_ep = x.ep; live = x.ep == w->epoch; w->busy = 1; pthread_mutex_unlock(&w->mu);
        if(x.e.type == CC_EV_LOCAL || live){ step(w, &x); publish(w); }   /* database, file reads and the shuffle walk: no lock held (an event of a finished cast is skipped) */
        free((void *)x.e.ids);                     /* SELECT: the slot's copy */
        pthread_mutex_lock(&w->mu); w->busy = 0; pthread_cond_broadcast(&w->idle_cv);
    }
    pthread_mutex_unlock(&w->mu);
    cc_ev l; memset(&l, 0, sizeof l); l.type = CC_EV_LOCAL; cc_act o[CC_ACT_MAX]; cast_ctl_step(&w->c, &l, o);   /* release the queues */
    return NULL;
}

/* ---- lifecycle ---- */
cw *cw_start(const cast_ctl_deps *d, uint32_t (*now)(void)){
    cw *w = calloc(1, sizeof *w);
    if(!w) return NULL;
    pthread_mutex_init(&w->mu, NULL); pthread_cond_init(&w->ev_cv, NULL); pthread_cond_init(&w->act_cv, NULL); pthread_cond_init(&w->idle_cv, NULL);
    w->now = now; cast_ctl_init(&w->c, d);
    if(pthread_create(&w->th, NULL, run, w)){ free(w); return NULL; }
    return w;
}
void cw_destroy(cw *w){
    if(!w) return;
    pthread_mutex_lock(&w->mu); w->closing = 1; pthread_cond_broadcast(&w->ev_cv); pthread_cond_broadcast(&w->act_cv); pthread_mutex_unlock(&w->mu);
    pthread_join(w->th, NULL);
    if(g_ui == w) g_ui = NULL;
    for(int i = 0; i < w->en; i++) free((void *)w->ev[i].e.ids);   /* queued SELECTs the worker never took */
    free(w);
}

/* ---- event mailbox: LVGL side, never blocks ---- */
static int droppable(int t){ return t == CC_EV_SNAPSHOT || t == CC_EV_NEXT || t == CC_EV_PREV || t == CC_EV_JUMP; }
/* >= 0: queued (a SELECT's ids copy now belongs to the mailbox); -1: refused, the caller still owns the copy */
static int post(cw *w, const cw_ev *in){
    cw_ev xv = *in; const cw_ev *x = &xv; int t = x->e.type, r = 0, nd = 0;
    pthread_mutex_lock(&w->mu);
    if(w->closing || t <= CC_EV_NONE || t > CC_EV_JUMP){ pthread_mutex_unlock(&w->mu); return -1; }
    if(t == CC_EV_JUMP && (w->busy || w->en || w->force_local)){
        w->dropped++; pthread_mutex_unlock(&w->mu); return -1;
    }
    xv.ep = w->epoch;
    if(t == CC_EV_LOCAL){ w->epoch++; w->an = 0; w->have_result = 0; pthread_cond_broadcast(&w->act_cv); }   /* 14e2 C2: LOCAL voids every queued and in-flight action of the cast it ends */
    if(t == CC_EV_LOAD_STARTED || t == CC_EV_LOAD_REFUSED){
        /* Results arrive in LOAD-action order from the single LVGL executor.
         * A newer result makes every older one stale, even across queue changes. */
        w->result = *x; w->have_result = 1; goto done;
    }
    if(t == CC_EV_SNAPSHOT && w->en && w->ev[w->en - 1].e.type == CC_EV_SNAPSHOT){ w->ev[w->en - 1] = *x; w->coalesced++; r = 1; goto done; }
    for(int i = 0; i < w->en; i++) nd += droppable(w->ev[i].e.type);
    if(droppable(t) ? (nd >= CW_EV_DROP || w->en == CW_EV_N) : w->en == CW_EV_N){
        int i = 0;
        if(!droppable(t)){
            while(i < w->en && !droppable(w->ev[i].e.type)) i++;
            if(i == w->en){ if(t == CC_EV_LOCAL) w->force_local = 1; else w->lost++; r = -1; goto done; }
            memmove(w->ev + i, w->ev + i + 1, (size_t)(w->en - i - 1) * sizeof w->ev[0]); w->en--; w->dropped++;
        } else { w->dropped++; r = -1; goto done; }
    }
    w->ev[w->en++] = *x;
done:
    pthread_cond_signal(&w->ev_cv); pthread_mutex_unlock(&w->mu);
    return r;
}
int cw_post(cw *w, const cc_ev *e){
    cw_ev x; memset(&x, 0, sizeof x); x.e = *e; x.e.q = NULL; x.e.ids = NULL; x.e.now_ms = w->now ? w->now() : 0;
    if(x.e.type == CC_EV_ENTER) return -1;
    if(x.e.type == CC_EV_SELECT){   /* the ids are copied: the mailbox owns the copy until the worker has built the queue (or the event is dropped) */
        if(!e->ids || e->n <= 0 || e->n > CQ_MAX_OCCURRENCES) return -1;   /* longer than a queue may be: refused, never truncated */
        int *cp = malloc((size_t)e->n * sizeof *cp);
        if(!cp) return -1;
        memcpy(cp, e->ids, (size_t)e->n * sizeof *cp); x.e.ids = cp;
        int r = post(w, &x);
        if(r < 0) free(cp);
        return r;
    }
    return post(w, &x);
}
int cw_post_enter(cw *w, const cc_ev *e, int play_mode, int pos_id, const char *path){
    cw_ev x; memset(&x, 0, sizeof x); x.e = *e; x.e.type = CC_EV_ENTER; x.e.q = NULL; x.e.ids = NULL; x.e.now_ms = w->now ? w->now() : 0;
    x.play_mode = play_mode; x.pos_id = pos_id; snprintf(x.path, sizeof x.path, "%s", path ? path : "");
    return post(w, &x);
}
int cw_view_get(cw *w, cw_view *out){
    pthread_mutex_lock(&w->mu); *out = w->view; pthread_mutex_unlock(&w->mu);
    return out->active;
}
int cw_view_same(const cw_view *a, const cw_view *b){ return a->active && b->active && a->qgen == b->qgen && a->pos == b->pos && a->mode == b->mode; }
int cw_jump(cw *w, uint32_t qgen, int occ){
    cc_ev e; memset(&e, 0, sizeof e); e.type = CC_EV_JUMP; e.qgen = qgen; e.occ = occ;
    return !w || cw_post(w, &e) < 0 ? -1 : 0;
}
int cw_take(cw *w, cw_act *out){
    int r = 0;
    pthread_mutex_lock(&w->mu);
    if(w->an){
        *out = w->ac[0]; out->a.row = out->a.row ? &out->row : NULL;
        memmove(w->ac, w->ac + 1, (size_t)(--w->an) * sizeof w->ac[0]);
        for(int i = 0; i < w->an; i++) if(w->ac[i].a.row) w->ac[i].a.row = &w->ac[i].row;
        pthread_cond_signal(&w->act_cv); r = 1;
    }
    pthread_mutex_unlock(&w->mu);
    return r;
}
int cw_wait_idle(cw *w, int ms){
    struct timespec t; int r;
    clock_gettime(CLOCK_REALTIME, &t); t.tv_sec += ms / 1000; t.tv_nsec += (long)(ms % 1000) * 1000000L; if(t.tv_nsec >= 1000000000L){ t.tv_sec++; t.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&w->mu);
    while((w->en || w->have_result || w->busy || w->force_local) && pthread_cond_timedwait(&w->idle_cv, &w->mu, &t) != ETIMEDOUT);
    r = !(w->en || w->have_result || w->busy || w->force_local);
    pthread_mutex_unlock(&w->mu);
    return r;
}
void cw_counts(cw *w, unsigned *coalesced, unsigned *dropped, unsigned *lost){
    pthread_mutex_lock(&w->mu);
    if(coalesced) *coalesced = w->coalesced;
    if(dropped) *dropped = w->dropped;
    if(lost) *lost = w->lost;
    pthread_mutex_unlock(&w->mu);
}
