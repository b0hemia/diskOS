/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cast_ctl.h"

static int cls(void *x, int i, int *d){ cc_slot *s = x; return s->c->d.classify(s->c->d.ctx, &s->q, i, d); }
static void drop(cc_slot *s){ cast_cursor_close(&s->cur); cast_queue_free(&s->q); memset(s, 0, sizeof *s); }
static cc_act *put(cc_act *o, int *n, int kind){
    static cc_act sink;                       /* a full list drops the extra action, never writes past it */
    if(*n >= CC_ACT_MAX) return &sink;
    cc_act *a = &o[(*n)++]; memset(a, 0, sizeof *a); a->kind = kind; return a;
}
static void toast(cc_act *o, int *n, int id){ put(o, n, CC_A_TOAST)->toast = id; }
static uint64_t next_seed(cast_ctl *c){ uint64_t s = c->d.seed; c->d.seed = s * 6364136223846793005ULL + 1442695040888963407ULL; return s; }   /* each cursor gets its own rng stream */
static int row_of(cast_ctl *c, int k){ return (c->d.row ? c->d.row : cast_ctl_row_db)(c->d.ctx, &c->s[c->a].q, k, &c->row); }

int cast_ctl_row_db(void *ctx, const cast_queue *q, int idx, cc_row *out){
    (void)ctx; memset(out, 0, sizeof *out);
    if(!q || idx < 0 || idx >= q->n) return -1;
    if(q->src == CQ_SRC_SELECTION){
        mdb_srow_t r; unsigned char f;
        int pl = q->flags[idx] & CQ_F_PLAYLIST;   /* 14g: a playlist occurrence is a CUSTOM_PLAYLIST.ID, never a SONG.ID */
        if((pl ? mdb_playlist_rows_get(&q->sid[idx], 1, &r, &f) : mdb_song_rows(&q->sid[idx], 1, &r, &f)) != 1) return -1;
        out->dur_ms = r.dur_ms; snprintf(out->title, sizeof out->title, "%s", r.title); snprintf(out->artist, sizeof out->artist, "%s", r.artist);
        snprintf(out->album, sizeof out->album, "%s", r.album); snprintf(out->path, sizeof out->path, "%s", r.path);
        return 0;
    }
    int k = idx; mdb_qrow_t r;
    if(q->shuf){ for(k = 0; k < q->n && q->shuf[k] != q->base[idx]; k++); if(k == q->n) return -1; }   /* page() is in snapshot play order */
    if(cast_queue_page(q, k, 1, &r, NULL) != 1) return -1;
    out->dur_ms = r.dur_ms; snprintf(out->title, sizeof out->title, "%s", r.title); snprintf(out->artist, sizeof out->artist, "%s", r.artist);
    snprintf(out->path, sizeof out->path, "%s", r.path);
    return 0;
}
void cast_ctl_init(cast_ctl *c, const cast_ctl_deps *d){ memset(c, 0, sizeof *c); c->d = *d; }

static void handle(cast_ctl *c, cast_action a, cc_act *o, int *n){
    cc_slot *s = &c->s[c->a]; cc_act *x;
    if(a.kind == CA_LOAD && row_of(c, a.k)){   /* no row, no path to serve: the LOAD cannot be made */
        handle(c, cast_cursor_failed(&s->cur, a.gen), o, n);
        return;
    }
    if(a.kind == CA_LOAD){
        c->last_ms = a.start_ms; c->last_known = a.start_ms >= 0;   /* a new LOAD: the occurrence's position is where it starts */
        c->stopped = 0; c->prior = c->pend || c->orph || c->out; c->pend = 1; c->known = 0; c->pgen = a.gen; c->wp = c->playing; c->orph = 0; c->pdue = c->now + (uint32_t)c->d.load_ms;
        x = put(o, n, CC_A_LOAD); x->k = a.k; x->start_ms = a.start_ms; x->skipped = a.skipped; x->qgen = c->qgen; x->gen = a.gen; x->row = &c->row;
        x->queue_pos = s->cur.pos + 1; x->queue_len = s->q.n;
    } else if(a.kind == CA_STAY_PAUSED){
        int stop = c->pend || c->orph || c->out;   /* an older LOAD is still out, or the adopted media is still playing (14e2 C1): it must not outlive this choice on the receiver */
        c->stopped = 0; c->pend = c->orph = 0;
        x = put(o, n, CC_A_STAY_PAUSED); x->reason = a.reason; x->cause = a.cause; x->k = a.k; x->skipped = a.skipped; x->qgen = c->qgen; x->gen = a.gen;
        x->row = row_of(c, a.k) ? NULL : &c->row; x->queue_pos = s->cur.pos + 1; x->queue_len = s->q.n;
        toast(o, n, a.reason == CA_QUEUE_FINISHED ? CC_T_QUEUE_FINISHED : a.cause == CA_C_EXCLUDED || a.cause == CA_C_UNKNOWN_FORMAT ? CC_T_UNSUPPORTED :
                    a.cause == CA_C_NONE_PLAYABLE ? CC_T_NONE_PLAYABLE : CC_T_COULD_NOT_PLAY);
        if(stop) put(o, n, CC_A_STOP_MEDIA);   /* orphan rule (14e1): stop the media, never leave an unowned paused track behind the facade */
    }
}
static void republish(cast_ctl *c, cc_act *o, int *n){
    cc_slot *s = &c->s[c->a]; cc_act *x = put(o, n, CC_A_REPUBLISH);
    x->row = &c->row; x->k = s->cur.order[s->cur.pos]; x->queue_pos = s->cur.pos + 1; x->queue_len = s->q.n; x->qgen = c->qgen; x->gen = s->cur.gen;
    x->playing = c->wp; x->start_ms = (int)(s->cur.base * 1000.0); x->media_id = s->cur.media_id;
}
static void release_all(cast_ctl *c){
    drop(&c->s[0]); drop(&c->s[1]); c->active = c->pend = c->known = c->playing = c->settle = c->wp = c->orph = c->out = c->prior = c->stopped = c->last_ms = c->last_known = 0; c->qgen++;
}
static int select_(cast_ctl *c, const cc_ev *e, cc_act *o, int *n){
    int t = 1 - c->a, r;
    cc_slot *ns = &c->s[t];
    memset(ns, 0, sizeof *ns); ns->c = c;
    r = cast_queue_from_rows(&ns->q, e->ids, e->n, e->start_index, e->mode, e->kind);
    if(r == CQ_OK && cast_cursor_open(&ns->cur, &ns->q, cls, ns, next_seed(c))){ cast_queue_free(&ns->q); r = CQ_DB_ERROR; }
    if(r != CQ_OK){   /* the old queue keeps playing: nothing of it was touched */
        memset(ns, 0, sizeof *ns);
        toast(o, n, r == CQ_REBUILDING || r == CQ_AMBIGUOUS ? CC_T_UPDATING : CC_T_LIST_FAILED);
        return 0;
    }
    cast_action a = e->mode == 1 ? cast_cursor_start_shuffled(&ns->cur, 0) : cast_cursor_play(&ns->cur, 0);
    c->out = c->s[c->a].cur.media_id > 0;   /* the replaced queue's adopted media keeps playing unless the new first track starts (14e2 C1) */
    drop(&c->s[c->a]);                      /* only now: the new queue is built, the old one is released */
    c->a = t; c->qgen++; c->orph = c->pend; c->pend = 0; c->known = 0;   /* the old queue's LOAD, if out, is now an orphan */
    c->playing = 1;                         /* a tap on a song or list always starts it, whether or not the receiver was playing */
    handle(c, a, o, n);
    c->out = 0;
    return 0;
}
/* The position a stopped occurrence resumes at: the last receiver position kept in c->last_ms, or 0 when none is usable (never set, negative, or
 * past the known duration). IDLE samples never feed it (their currentTime is not trusted), so a Stop reports the last live sample, up to one
 * SNAPSHOT period behind the real stop. */
static int stop_ms(const cast_ctl *c){
    int d = c->s[c->a].cur.dur_ms;
    return c->last_known && c->last_ms >= 0 && (d <= 0 || c->last_ms <= d) ? c->last_ms : 0;
}
static void stopped(cast_ctl *c, cc_act *o, int *n){
    cast_cursor *u = &c->s[c->a].cur;
    c->stopped = 1; c->playing = c->wp = c->settle = 0;
    u->media_id = 0; /* consume this media once, keep pos/order (including duplicate songs) */
    republish(c, o, n);
    o[*n - 1].kind = CC_A_STOPPED; o[*n - 1].start_ms = stop_ms(c);
}
int cast_ctl_position_usable(const cc_ev *e, int dur_ms){
    return (e->player == CS_PL_PLAYING ||
            e->player == CS_PL_PAUSED ||
            e->player == CS_PL_BUFFERING) &&
           e->media_time >= 0 && e->media_time < 1e6 &&
           (dur_ms <= 0 || e->media_time <= dur_ms / 1000.0);
}
/* Keep the receiver position only from a usable live sample: validated before it replaces the retained one, so a bad report never destroys a good one. */
static void retain_position(cast_ctl *c, const cc_ev *e){
    if(cast_ctl_position_usable(e, c->s[c->a].cur.dur_ms)){
        c->last_ms = (int)(e->media_time * 1000.0);
        c->last_known = 1;   /* as reported: a backward seek lowers it (prog only ever rises) */
    }
}
static void snapshot(cast_ctl *c, const cc_ev *e, cc_act *o, int *n){
    cast_cursor *u = &c->s[c->a].cur; cc_act *x;
    if(c->pend){
        if(c->known && e->load_seq == c->pseq){   /* the pending LOAD's result */
            if(e->load_state == CS_JOB_FAILED){ cast_action a = cast_cursor_failed(u, c->pgen); c->pend = 0; handle(c, a, o, n); return; }
            if(e->load_state == CS_JOB_OK && e->media_id > 0){
                cast_cursor_adopted(u, c->pgen, e->media_id); c->pend = 0;
                if(e->player == CS_PL_IDLE && e->idle == CS_IDLE_CANCELLED){ stopped(c, o, n); return; }
                retain_position(c, e);
                if(c->wp){ x = put(o, n, CC_A_PLAY_AFTER_ADOPT); x->qgen = c->qgen; x->gen = c->pgen; c->settle = 1; }
                republish(c, o, n);
                return;
            }
        }
        /* Deadline (carry-over 2): a LOAD that never adopts (LOAD_STARTED lost, OK without a media id, no report at all) must not pend forever.
         * Checked on the periodic SNAPSHOT only, after a matching result had its chance; the failure carries the orphan stop (it may still land). */
        if(c->d.load_ms > 0 && (int32_t)(c->now - c->pdue) >= 0){ cast_action a = cast_cursor_failed(u, c->pgen); handle(c, a, o, n); }
        return;
    }
    if(c->known && e->load_seq != c->pseq) return;
    c->known = 1; c->pseq = e->load_seq;
    if(e->media_id <= 0 || e->media_id != u->media_id) return;   /* not the owned media (or already consumed: a duplicate FINISHED) */
    if(e->player == CS_PL_IDLE && e->idle == CS_IDLE_CANCELLED){ stopped(c, o, n); return; }
    cast_cursor_progress(u, u->gen, e->media_id, e->media_time);
    retain_position(c, e);
    if(e->player == CS_PL_PLAYING){ c->playing = 1; c->settle = 0; }
    else if(e->player == CS_PL_PAUSED && !c->settle) c->playing = 0;
    else if(e->player == CS_PL_IDLE) handle(c, cast_cursor_finished(u, u->gen, e->media_id, e->idle, e->media_time), o, n);
}
int cast_ctl_step(cast_ctl *c, const cc_ev *e, cc_act out[CC_ACT_MAX]){
    int n = 0;
    if(e->now_ms) c->now = e->now_ms;
    if(e->type == CC_EV_ENTER){
        if(c->active || !e->q || !e->q->n) return 0;
        cc_slot *s = &c->s[0]; memset(s, 0, sizeof *s); s->c = c; s->q = *e->q;
        if(cast_cursor_open(&s->cur, &s->q, cls, s, next_seed(c))){ memset(s, 0, sizeof *s); toast(out, &n, CC_T_LIST_FAILED); return n; }
        memset(e->q, 0, sizeof *e->q);   /* moved: the caller must not free it */
        c->a = 0; c->active = 1; c->qgen++; c->pend = c->known = c->settle = 0; c->playing = c->wp = e->was_playing != 0;
        cast_cursor_attach_playing(&s->cur, e->start_ms, e->media_id, e->dur_ms);
        c->last_ms = e->start_ms; c->last_known = e->start_ms >= 0;
        if(!row_of(c, s->cur.order[s->cur.pos])) republish(c, out, &n);
        return n;
    }
    if(e->type == CC_EV_LOCAL){ if(c->active) release_all(c); return 0; }
    if(e->type == CC_EV_JUMP && c->active && e->qgen != c->qgen){ toast(out, &n, CC_T_QUEUE_CHANGED); return n; }   /* 14f: a tap on a row of a replaced queue never jumps */
    if(!c->active || (e->qgen && e->qgen != c->qgen)) return 0;   /* torn down, or an event about a replaced queue */
    cast_cursor *u = &c->s[c->a].cur;
    switch(e->type){
    case CC_EV_PLAY:
        if(c->stopped && !c->pend){ c->playing = 1; handle(c, cast_cursor_play(u, stop_ms(c)), out, &n); }   /* the same occurrence, at the stopped position */
        break;
    case CC_EV_SNAPSHOT: snapshot(c, e, out, &n); break;
    case CC_EV_LOAD_STARTED: case CC_EV_LOAD_REFUSED:
        if(!c->pend || c->known || e->gen != c->pgen) break;   /* an older LOAD's report: superseded */
        if(e->type == CC_EV_LOAD_STARTED){ c->known = 1; c->pseq = e->load_seq; }
        else { cast_action a = cast_cursor_failed(u, c->pgen); c->pend = 0; if(c->prior) c->orph = 1; handle(c, a, out, &n); }   /* C5: nothing was sent, so the older LOAD / media still sits on the receiver: stop it */
        break;
    case CC_EV_NEXT: case CC_EV_PREV:   /* a pending LOAD is superseded by the cursor's new gen; the adopted media is abandoned by a move that does not LOAD (C1) */
        c->out = u->media_id > 0;
        handle(c, e->type == CC_EV_NEXT ? cast_cursor_next(u) : cast_cursor_prev(u, e->position_ms), out, &n);
        c->out = 0; break;
    case CC_EV_JUMP: {   /* 14f: an explicit choice, like a tap on a song: it starts playing, and the media it abandons is stopped like Next's (C1) */
        int p = cast_cursor_pos_of(u, e->occ);
        if(p < 0){ toast(out, &n, CC_T_QUEUE_CHANGED); break; }
        c->playing = 1; c->out = u->media_id > 0;
        handle(c, cast_cursor_jump(u, p), out, &n);
        c->out = 0; break;
    }
    case CC_EV_SELECT: select_(c, e, out, &n); break;
    case CC_EV_MODE: cast_cursor_set_mode(u, e->mode); break;   /* the cursor only: no 0102 (owner decision 77) */
    default: break;
    }
    return n;
}
