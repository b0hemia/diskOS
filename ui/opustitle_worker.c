/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Per-identity state table, processed by decide() on the caller thread.
 * P = either PENDING_EVIDENCE or PENDING_READ. Rows are ordered by priority.
 * Input / state                         Next state / output
 * identity_end, changed generation / *  NONE / cancel, advance identity epoch
 * submit new IDs/path / *               NONE, then first-submit rows below
 * library epoch / NONE, COMMITTED       unchanged / remember epoch for next item
 * library epoch / P                     P / invalidate job, keep deadline;
 *                                      redispatch only in PENDING_READ
 * submit same / COMMITTED               COMMITTED / ignore title and evidence
 * poll / NONE, delivered COMMITTED      unchanged / no output
 * rejected publication / delivered COMMITTED
 *                                      PENDING_EVIDENCE / invalidate selection, keep original/deadline
 * poll / undelivered COMMITTED          COMMITTED / deliver exactly once
 * submit or poll at/after deadline / P  COMMITTED / original title, TIMEOUT
 * first submit / NONE                   initialize identity, original, deadline;
 *                                      then apply the submit rows below
 * submit genuine nonempty title / P     COMMITTED / player title immediately,
 *                                      regardless of known/unknown/changed flags
 * submit basename or empty / P:
 *   changed flags                       invalidate job, keep identity/deadline
 *   unknown kind                        PENDING_EVIDENCE / no read
 *   known virtual/unsupported path      COMMITTED / original player title
 *   known plain supported, empty title  PENDING_EVIDENCE / no read
 *   known plain supported, basename     PENDING_READ / dispatch or retry dispatch
 *   same evidence                       same rules, no duplicate dispatch
 *   eligible but worker unavailable     COMMITTED / original title, WORKER
 * poll before deadline / PENDING_EVIDENCE unchanged / discard mailbox, no read
 * poll before deadline / PENDING_READ:
 *   current result FOUND                COMMITTED / reader title
 *   current ABSENT, ERROR               COMMITTED / original, retain outcome
 *   current NOT_APPLICABLE              COMMITTED / original (stock title)
 *   stale result or no result           PENDING_READ / discard, retry dispatch
 * All commits cancel the token; late results cannot publish. A genuine title
 * cannot remain waiting at expiry: it already committed on its submit event.
 * Original means the first submitted player title, even if it was empty.
 * Titles are complete, successfully decoded strings supplied by the caller.
 */
#include "opustitle_worker.h"
#include "ui_wake.h"
#include "sdio.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
typedef enum { NONE, PENDING_EVIDENCE, PENDING_READ, COMMITTED } decision_state;
typedef struct { otw_key key; uint64_t deadline; unsigned token; } job;
struct opus_title_worker {
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t wake;
    atomic_uint token, stop;
    int started, pending, mailbox;
    job queued;
    otw_commit mail;
    uint64_t (*clock)(void *);
    void *ctx;
    /* Caller-owned state: worker NEVER reads these fields. */
    otw_commit current;
    uint64_t epoch, serial, media, generation, deadline;
    unsigned dispatched;
    decision_state state;
    int delivered;
    char original[256];
};
static uint64_t monotonic(void *ctx){
    (void)ctx; struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
typedef struct { opus_title_worker *w; job j; } read_context;
static int cancelled(void *ctx){
    read_context *r=ctx;
    return atomic_load(&r->w->stop) || atomic_load(&r->w->token)!=r->j.token ||
           r->w->clock(r->w->ctx)>=r->j.deadline || !sd_io_allowed();
}
static void *worker(void *ctx){
    opus_title_worker *w=ctx;
    for(;;){
        pthread_mutex_lock(&w->mu);
        while(!w->pending && !atomic_load(&w->stop)) pthread_cond_wait(&w->wake,&w->mu);
        if(atomic_load(&w->stop)){ pthread_mutex_unlock(&w->mu); break; }
        read_context r={w,w->queued}; w->pending=0;
        pthread_mutex_unlock(&w->mu);
        otw_commit c={.key=r.j.key,.result={.outcome=OPUS_TITLE_ERROR,.reason=OPUS_TITLE_CANCELLED}};
        /* No controller/mailbox lock spans lease calls, opening, parsing or closing. */
        if(!cancelled(&r) && sd_io_begin()){
            for(int attempt=0;attempt<2;attempt++){
                c.result=opus_title_read(r.j.key.path,cancelled,&r);
                if(c.result.outcome!=OPUS_TITLE_ERROR || c.result.reason!=OPUS_TITLE_IO || !c.result.retryable || cancelled(&r)) break;
            }
            sd_io_end();
        }
        if(atomic_load(&w->token)!=r.j.token || atomic_load(&w->stop)) continue;
        pthread_mutex_lock(&w->mu);
        w->mail=c; w->mailbox=1;
        pthread_mutex_unlock(&w->mu);
        ui_notify(UI_WAKE_IPC|UI_WAKE_ART);
    }
    return NULL;
}
opus_title_worker *otw_create(uint64_t (*clock)(void *),void *ctx){
    opus_title_worker *w=calloc(1,sizeof *w); if(!w) return NULL;
    if(pthread_mutex_init(&w->mu,NULL)){ free(w); return NULL; }
    if(pthread_cond_init(&w->wake,NULL)){ pthread_mutex_destroy(&w->mu); free(w); return NULL; }
    atomic_init(&w->token,0); atomic_init(&w->stop,0);
    w->clock=clock ? clock : monotonic; w->ctx=ctx;
    w->started=pthread_create(&w->thread,NULL,worker,w)==0;
    return w;
}
void otw_destroy(opus_title_worker *w){
    if(!w) return;
    atomic_store(&w->stop,1); atomic_fetch_add(&w->token,1);
    pthread_mutex_lock(&w->mu); pthread_cond_signal(&w->wake); pthread_mutex_unlock(&w->mu);
    if(w->started) pthread_join(w->thread,NULL);
    pthread_cond_destroy(&w->wake); pthread_mutex_destroy(&w->mu); free(w);
}
static int same_identity(otw_identity a,otw_identity b){
    return a.player_generation==b.player_generation && a.song_id_valid==b.song_id_valid &&
        a.pos_id_valid==b.pos_id_valid && a.song_id==b.song_id && a.pos_id==b.pos_id;
}
static int same_key(const otw_key *a,const otw_key *b){
    return same_identity(a->identity,b->identity) && a->identity_epoch==b->identity_epoch &&
        a->serial==b->serial && a->media_epoch==b->media_epoch && a->flags==b->flags && !strcmp(a->path,b->path);
}
static int supported(const char *path){
    const char *base=strrchr(path,'/'); base=base ? base+1 : path;
    const char *ext=strrchr(base,'.');
    return path[0]=='/' && ext &&
        (!strcasecmp(ext,".opus") || !strcasecmp(ext,".oga") || !strcasecmp(ext,".ogg"));
}
static void dispatch(opus_title_worker *w){
    unsigned token=atomic_load(&w->token);
    if(w->state!=PENDING_READ || w->dispatched==token || !w->started) return;
    if(pthread_mutex_trylock(&w->mu)) return;
    w->queued=(job){w->current.key,w->deadline,token}; w->pending=1; w->dispatched=token;
    pthread_cond_signal(&w->wake); pthread_mutex_unlock(&w->mu);
}
static void select_title(opus_title_worker *w,opus_title_result result,unsigned failure,const char *title){
    if(title) strcpy(result.title,title);
    w->current.result=result; w->current.failure=failure;
    w->state=COMMITTED; atomic_fetch_add(&w->token,1);
}
typedef enum { SUBMIT, POLL, IDENTITY_END, GENERATION, LIBRARY } decision_input;
/* One run-to-completion transition function owns every controller decision.
 * SUBMIT inputs have already passed the public API's size/NULL checks. */
static int decide(opus_title_worker *w,decision_input input,otw_identity id,
                  const char *path,const char *title,unsigned flags,uint64_t value,otw_commit *out){
    if(input==IDENTITY_END || (input==GENERATION && w->generation!=value)){
        atomic_fetch_add(&w->token,1); w->epoch++;
        w->state=NONE; w->delivered=0;
        if(input==GENERATION) w->generation=value;
        return 0;
    }
    if(input==GENERATION) return 0;
    if(input==LIBRARY){
        if(w->media==value) return 0;
        w->media=value;
        if(w->state==PENDING_EVIDENCE || w->state==PENDING_READ){
            atomic_fetch_add(&w->token,1);
            w->current.key.media_epoch=value; w->current.key.serial=++w->serial;
            dispatch(w);
        }
        return 0;
    }
    if(input==SUBMIT){
        decide(w,GENERATION,id,NULL,NULL,0,id.player_generation,NULL);
        if(w->state!=NONE && (!same_identity(w->current.key.identity,id) || strcmp(w->current.key.path,path)))
            decide(w,IDENTITY_END,id,NULL,NULL,0,0,NULL);
        if(w->state==COMMITTED) return 0;
        if(w->state==NONE){
            w->current=(otw_commit){.key={.identity=id,.identity_epoch=w->epoch,
                .serial=++w->serial,.media_epoch=w->media,.flags=flags}};
            strcpy(w->current.key.path,path); strcpy(w->original,title);
            /* Header reads run off-thread; allow cold SD latency without delaying transport. */
            w->deadline=value+5000; w->delivered=0; w->state=PENDING_EVIDENCE;
            atomic_fetch_add(&w->token,1);
        }
    }
    if(w->state==NONE || w->delivered) return 0;
    if(w->state!=COMMITTED && value>=w->deadline){
        select_title(w,(opus_title_result){.outcome=OPUS_TITLE_ERROR,.reason=OPUS_TITLE_CANCELLED},
                     OTW_FAILURE_TIMEOUT,w->original);
    }
    if(input==SUBMIT && w->state!=COMMITTED){
        const char *base=strrchr(path,'/'); base=base ? base+1 : path;
        if(title[0] && strcmp(base,title)){
            select_title(w,(opus_title_result){.outcome=OPUS_TITLE_NOT_APPLICABLE},OTW_FAILURE_NONE,title);
            return 0;
        }
        if(w->current.key.flags!=flags){
            atomic_fetch_add(&w->token,1);
            w->current.key.flags=flags; w->current.key.serial=++w->serial;
        }
        if(!(flags&OTW_KIND_KNOWN)) w->state=PENDING_EVIDENCE;
        else if(flags!=OTW_KIND_KNOWN || !supported(path))
            select_title(w,(opus_title_result){.outcome=OPUS_TITLE_NOT_APPLICABLE},OTW_FAILURE_NONE,w->original);
        else if(!title[0]){
            if(w->state==PENDING_READ){
                atomic_fetch_add(&w->token,1); w->current.key.serial=++w->serial;
            }
            w->state=PENDING_EVIDENCE;
        } else if(!w->started)
            select_title(w,(opus_title_result){.outcome=OPUS_TITLE_ERROR,.reason=OPUS_TITLE_CANCELLED},
                         OTW_FAILURE_WORKER,w->original);
        else { w->state=PENDING_READ; dispatch(w); }
        return 0;
    }
    if(input==POLL){
        if(w->state!=COMMITTED && !pthread_mutex_trylock(&w->mu)){
            if(w->mailbox){
                otw_commit c=w->mail; w->mailbox=0;
                if(w->state==PENDING_READ && same_key(&c.key,&w->current.key))
                    select_title(w,c.result,c.failure,c.result.outcome==OPUS_TITLE_FOUND ? NULL : w->original);
            }
            pthread_mutex_unlock(&w->mu);
        }
        dispatch(w);
        if(w->state!=COMMITTED) return 0;
        *out=w->current; w->delivered=1; return 1;
    }
    return 0;
}
void otw_identity_end(opus_title_worker *w){
    decide(w,IDENTITY_END,(otw_identity){0},NULL,NULL,0,0,NULL);
}
void otw_player_generation(opus_title_worker *w,uint64_t generation){
    decide(w,GENERATION,(otw_identity){0},NULL,NULL,0,generation,NULL);
}
void otw_library_commit(opus_title_worker *w,uint64_t epoch){
    decide(w,LIBRARY,(otw_identity){0},NULL,NULL,0,epoch,NULL);
}
int otw_submit(opus_title_worker *w,otw_identity id,const char *path,const char *title,unsigned flags){
    if(!w || !path || !title || strlen(path)>=sizeof w->current.key.path || strlen(title)>=sizeof w->original) return -1;
    return decide(w,SUBMIT,id,path,title,flags,w->clock(w->ctx),NULL);
}
int otw_poll(opus_title_worker *w,uint64_t now,otw_commit *out){
    return decide(w,POLL,(otw_identity){0},NULL,NULL,0,now,out);
}
int otw_title_ready(const opus_title_worker *w){ return w->state==COMMITTED; }

void otw_reject_commit(opus_title_worker *w){
    if(w->state!=COMMITTED || !w->delivered) return;
    atomic_fetch_add(&w->token,1);
    w->current.key.serial=++w->serial;
    w->state=PENDING_EVIDENCE; w->delivered=0;
}
