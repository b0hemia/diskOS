/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>
#include "cast_handoff.h"

int ho_quiet_step(uint32_t *since, uint32_t now, int playing, int pcm_quiet, uint32_t hold_ms){
    if(playing || !pcm_quiet) *since = 0;
    else if(!*since){ *since = now ? now : 1; return hold_ms == 0; }   /* the window opens now; tick 0 must not read as a wrapped 49-day hold */
    return *since && (int32_t)(now - *since) >= 0 &&
           (uint32_t)(now - *since) >= hold_ms;
}
/* V2.57 pause keeps PCM RUNNING; stock Local reselect closes it without START. */
int ho_drain_step(ho_drain *d, uint32_t now, int playing, int pcm_closed,
                  int (*send)(void *), void *ctx){
    if(d->sent < 0) return -1;
    if(!d->sent){
        if(playing) return 0;
        if(!send){ d->sent = -1; return -1; }
        int rc = send(ctx);
        if(rc == IPC_SEND_DEFERRED) return 0; /* no frame queued; retain this intention */
        if(rc != 0){ d->sent = -1; return -1; }
        d->sent = 1; d->quiet_since = 0;
        return 0;      /* the supplied PCM sample predates teardown */
    }
    return ho_quiet_step(&d->quiet_since, now, playing, pcm_closed, HO_QUIET_MS);
}
void ho_init(ho *h){ memset(h, 0, sizeof *h); }
ho_state ho_get(const ho *h){ return h->state; }
uint32_t ho_attempt(const ho *h){ return h->attempt; }

static void enter(ho *h, ho_state s, uint32_t now, uint32_t ms){ h->state = s; h->deadline = now + ms; h->quiet_since = h->loud_since = 0; }
static int due(const ho *h, uint32_t now){ return (int32_t)(now - h->deadline) >= 0; }
/* the one exit: serving revoked, session released, facade LOCAL (after any remote STOP the caller sent first). Nothing here
 * touches the local player. */
static void finish(ho *h, const ho_sink *k, ho_msg m){
    k->revoke(k->ctx); k->release(k->ctx);
    if(h->facade){ k->set_cast(k->ctx, NULL); h->facade = 0; }
    h->engaged = h->pause_sent = h->user_stop = 0;
    h->state = HO_LOCAL;
    if(m) k->message(k->ctx, m);
}
/* roll back: stop the receiver first when it may hold our media (then wait for the confirmation, bounded), else finish at once */
static void teardown(ho *h, uint32_t now, const ho_sink *k, ho_msg m, int stop, int revoke_now, const char *reason){
    fprintf(stderr, "AirPlay/Cast handoff end reason=%s state=%d stop=%d\n", reason, h->state, stop);
    if(revoke_now) k->revoke(k->ctx);
    if(stop && h->engaged){
        if(k->remote_stop(k->ctx) >= 0){ h->after = m; enter(h, HO_STOPPING, now, HO_STOP_MS); return; }
        fprintf(stderr, "AirPlay/Cast handoff stop not confirmed reason=stop request failed\n");
        m = HO_MSG_STOP_UNCONFIRMED;
    }
    finish(h, k, m);
}
static void on_local(ho *h, uint32_t now, const ho_local *l, const ho_sink *k){
    if(!l || h->state == HO_LOCAL || h->state == HO_STOPPING) return;
    if(!l->sd_ok){ teardown(h, now, k, HO_MSG_SD, 1, 1, "SD unavailable"); return; }
    if(h->state == HO_CASTING){   /* 16b: Cast owns playback; the Disc must stay silent, on the same player and source. Never toggles the player. */
        ho_msg m = HO_MSG_NONE;
        if(l->generation != h->gen) m = HO_MSG_RESTARTED;
        else if(l->source_mode != h->src) m = HO_MSG_SOURCE;
        else if(ho_quiet_step(&h->loud_since, now, 0, l->playing || !l->pcm_quiet, HO_QUIET_MS)) m = HO_MSG_LOCAL_PLAY;   /* audible for HO_QUIET_MS (the step reads "quiet" as "audible" here) */
        if(m) teardown(h, now, k, m, 1, 1, m == HO_MSG_RESTARTED ? "player generation changed" : m == HO_MSG_SOURCE ? "source changed" : "local audible");
        return;
    }
    if(l->generation != h->gen){ teardown(h, now, k, HO_MSG_RESTARTED, 1, 1, "player generation changed"); return; }
    /* clear_track adds one epoch; extra epochs mean another item was loaded. */
    if((!l->have_track && (h->drain.sent != 1 || l->token != h->token + UINT64_C(1))) ||
       (l->have_track && (l->token != h->token || strcmp(l->path ? l->path : "", h->path) != 0))){ teardown(h, now, k, HO_MSG_CHANGED, 1, 1, "track changed"); return; }
    if(h->state == HO_PAUSING){
        /* Keep the last media snapshot: teardown deliberately unloads the track. */
        if(!h->drain.sent && !l->playing) h->pos = l->position_ms;
        int drained = ho_drain_step(&h->drain, now, l->playing, l->pcm_quiet, k->local_teardown, k->ctx);
        if(drained < 0){ teardown(h, now, k, HO_MSG_BUSY, 0, 1, "local pause not confirmed"); return; }
        if(!drained) return;
        if(k->serve_load(k->ctx, h->attempt, h->path, h->pos) < 0){ teardown(h, now, k, HO_MSG_FAILED, 0, 1, "load failed"); return; }
        h->engaged = 1; enter(h, HO_LOCAL_PAUSED, now, HO_LOAD_MS);
    } else if((h->state == HO_LOCAL_PAUSED || h->state == HO_REMOTE_READY) && (l->playing || !l->pcm_quiet))
        teardown(h, now, k, HO_MSG_BUSY, 1, 1, "local audible");   /* the Disc is audible again: overlap risk, give up */
}
static void on_select(ho *h, uint32_t now, const ho_event *ev, const ho_sink *k){
    const ho_local *l = ev->local;
    if(h->state != HO_LOCAL) return;   /* double select / select while busy: ignored */
    if(!l || !l->have_track || !l->sd_ok || !l->path || !l->path[0]){ k->message(k->ctx, HO_MSG_UNAVAILABLE); return; }
    memset(&h->drain, 0, sizeof h->drain);
    h->attempt++; h->was_playing = l->playing != 0; h->gen = l->generation; h->src = l->source_mode; h->token = l->token; h->pos = l->position_ms; h->dur = l->duration_ms;
    snprintf(h->path, sizeof h->path, "%s", l->path);
    snprintf(h->title, sizeof h->title, "%s", l->title ? l->title : "");
    snprintf(h->artist, sizeof h->artist, "%s", l->artist ? l->artist : "");
    snprintf(h->album, sizeof h->album, "%s", l->album ? l->album : "");
    snprintf(h->dest, sizeof h->dest, "%s", ev->dest ? ev->dest : "");
    enter(h, HO_PREPARING, now, HO_PREP_MS);
}
static void on_media(ho *h, uint32_t now, const ho_sink *k){
    pb_cast_state c;
    if(h->was_playing && k->remote_play(k->ctx) < 0){ teardown(h, now, k, HO_MSG_FAILED, 1, 1, "play failed"); return; }
    memset(&c, 0, sizeof c); c.active = 1; c.v.have_track = 1; c.v.playing = h->was_playing; c.v.position_ms = h->pos; c.v.duration_ms = h->dur;
    snprintf(c.v.title, sizeof c.v.title, "%s", h->title); snprintf(c.v.artist, sizeof c.v.artist, "%s", h->artist);
    snprintf(c.v.album, sizeof c.v.album, "%s", h->album); snprintf(c.v.path, sizeof c.v.path, "%s", h->path);
    snprintf(c.v.dest, sizeof c.v.dest, "%s", h->dest);
    h->facade = 1; k->set_cast(k->ctx, &c);
    enter(h, HO_CASTING, now, 0);
}
static void on_timeout(ho *h, uint32_t now, const ho_sink *k){
    switch(h->state){
    case HO_PREPARING: teardown(h, now, k, HO_MSG_FAILED, 0, 1, "handoff timeout"); break;
    case HO_PAUSING: teardown(h, now, k, HO_MSG_BUSY, 0, 1, "handoff timeout"); break;   /* pause not proven: say so, send nothing more to the player */
    case HO_LOCAL_PAUSED: case HO_REMOTE_READY: teardown(h, now, k, HO_MSG_FAILED, 1, 1, "handoff timeout"); break;
    case HO_STOPPING: fprintf(stderr, "AirPlay/Cast handoff stop not confirmed reason=timeout\n"); finish(h, k, h->user_stop ? HO_MSG_STOP_UNCONFIRMED : h->after); break;
    default: break;
    }
}
void ho_step(ho *h, uint32_t now, const ho_event *ev, const ho_sink *k){
    if(h->state != HO_LOCAL && h->state != HO_CASTING && due(h, now)) on_timeout(h, now, k);   /* deadline first: a late event never advances */
    if(!ev) return;
    if(ev->type >= HO_EV_REMOTE_READY && ev->type <= HO_EV_TAKEOVER && (h->state == HO_LOCAL || ev->attempt != h->attempt)) return;   /* stale */
    switch(ev->type){
    case HO_EV_SELECT: on_select(h, now, ev, k); break;
    case HO_EV_LOCAL: on_local(h, now, ev->local, k); break;
    case HO_EV_REMOTE_READY:
        if(h->state != HO_PREPARING) break;
        if(!ev->local){ teardown(h, now, k, HO_MSG_BUSY, 0, 1, "local pause not confirmed"); break; }
        on_local(h, now, ev->local, k);
        if(h->state != HO_PREPARING) break;
        if((ev->local->playing != 0) != h->was_playing ||
           (h->was_playing && ev->local->pcm_quiet)){
            teardown(h, now, k, HO_MSG_BUSY, 0, 1, "local pause not confirmed");
            break;
        }
        if(h->was_playing){   /* one toggle frame, never resent; the PCM state confirms it */
            if(k->local_pause(k->ctx) < 0){ teardown(h, now, k, HO_MSG_BUSY, 0, 1, "local pause not confirmed"); break; }
            h->pause_sent = 1;
        }
        enter(h, HO_PAUSING, now, HO_PAUSE_MS);
        break;
    case HO_EV_LOAD_OK: if(h->state == HO_LOCAL_PAUSED) enter(h, HO_REMOTE_READY, now, HO_MEDIA_MS); break;
    case HO_EV_LOAD_FAILED: if(h->state == HO_LOCAL_PAUSED) teardown(h, now, k, HO_MSG_FAILED, 1, 1, "remote load failed"); break;
    case HO_EV_MEDIA: if(h->state == HO_REMOTE_READY && ev->owned) on_media(h, now, k); break;
    case HO_EV_REMOTE_STOPPED: if(h->state == HO_STOPPING) finish(h, k, h->after); break;
    case HO_EV_REMOTE_LOST:
        if(h->state == HO_STOPPING){
            fprintf(stderr, "AirPlay/Cast handoff stop not confirmed reason=receiver lost\n");
            finish(h, k, h->user_stop ? HO_MSG_STOP_UNCONFIRMED : h->after);
        }
        else teardown(h, now, k, h->state == HO_CASTING ? HO_MSG_LOST : HO_MSG_FAILED, 0, 1, "receiver lost");   /* the Disc is not restarted */
        break;
    case HO_EV_TAKEOVER: fprintf(stderr, "AirPlay/Cast handoff end reason=takeover\n"); finish(h, k, HO_MSG_TAKEN); break;   /* not ours any more: no STOP, only release */
    case HO_EV_STOP:
        if(h->state == HO_LOCAL || h->state == HO_STOPPING) break;
        h->user_stop = 1;
        teardown(h, now, k, HO_MSG_NONE, 1, h->state != HO_CASTING, "stop requested");   /* remote STOP first; the facade returns only in finish() */
        break;
    default: break;
    }
}
