/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_queue.h"
#include "airplay_crypto.h"
#include "audio_decode.h"
#include "cast_queue.h"
#include "cast_ctl.h"
#include "sdio.h"
#include "artcache.h"
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
typedef struct { aps_message m; int *ids, n, start, mode, kind; } queue_event;
struct aps_queue {
    pthread_t thread; pthread_mutex_t mutex; pthread_cond_t changed;
    _Atomic int stop; int ctl, mode, pos_id;
    queue_event events[16]; unsigned count; cw_view view; aps_message observed; int have_observed, audible; uint32_t revision, notified_generation, identity_generation; unsigned char *removed; uint32_t attempt;
    int (*check_cancel)(void *); void *check_ctx; unsigned check_rate, check_bits; ad_result check_error;
    aps_message art; int art_pending, art_tried; unsigned art_gen;
    int64_t art_deadline, art_next;
    char root[256]; aps_message first, request; cast_queue queue; cast_cursor cursor;
};
int aps_media_open(const char *root, const char *path){
    size_t n = root ? strlen(root) : 0;
    if(!n || !path || strncmp(root, path, n) || path[n] != '/' || strlen(path + n + 1) >= TRACK_PATH_MAX) return -1;
    char relative[TRACK_PATH_MAX]; strcpy(relative, path + n + 1);
    /* The configured mount root is trusted. Every library-relative component
     * and the final inode are checked through directory descriptors. */
    int d = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC); if(d < 0) return -1;
    for(char *p = relative;;){
        char *end = strchr(p, '/'); if(end) *end = 0;
        if(!*p || !strcmp(p, ".") || !strcmp(p, "..")){ close(d); return -1; }
        if(end){
            int next = openat(d, p, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            close(d); if(next < 0) return -1; d = next; p = end + 1; continue;
        }
        struct stat before, after;
        if(fstatat(d, p, &before, AT_SYMLINK_NOFOLLOW) || !S_ISREG(before.st_mode)){ close(d); return -1; }
        int fd = openat(d, p, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW); close(d);
        if(fd < 0) return -1;
        if(fstat(fd, &after) || !S_ISREG(after.st_mode) || before.st_dev != after.st_dev || before.st_ino != after.st_ino){ close(fd); return -1; }
        return fd;
    }
}
static int cancelled(void *ctx){ aps_queue *q = ctx; return atomic_load(&q->stop) || !sd_io_allowed() || (q->check_cancel && q->check_cancel(q->check_ctx)); }
static int classify_row(aps_queue *q, const cast_queue *queue, int idx, int *duration){
    cc_row row;
    *duration = 0; q->check_error = AD_OK;
    if(cancelled(q) || cast_ctl_row_db(NULL, queue, idx, &row) != 0) return CF_V_UNKNOWN;
    if(queue->flags[idx] & (CQ_F_CUE | CQ_F_ISO)) return CF_V_EXCLUDED;
    int fd = aps_media_open(q->root, row.path); if(fd < 0) return CF_V_UNKNOWN;
    audio_decode *d = NULL; ad_result r = audio_decode_open(&d, fd, 0, cancelled, q);
    if(r == AD_OK){ const ad_info *info = audio_decode_info(d); q->check_rate = info->rate; q->check_bits = info->bits; }
    close(fd); audio_decode_close(d); *duration = row.dur_ms; q->check_error = r;
    /* Only a format verdict excludes a track; AD_UNAVAILABLE stays retryable. */
    return r == AD_OK ? CF_V_OK : r == AD_FORMAT ? CF_V_EXCLUDED : CF_V_UNKNOWN;
}
static int classify(void *ctx, int idx, int *duration){ aps_queue *q = ctx; return classify_row(q, &q->queue, idx, duration); }
int aps_queue_check(const char *root, int mode, int pos_id, const char *path,
                    int (*cancel)(void *), void *ctx, unsigned *rate, unsigned *bits){
    if(!root || strlen(root) >= 256 || !path || !rate || !bits) return -1;
    *rate = *bits = 0;
    aps_queue *q = calloc(1, sizeof *q); if(!q) return -1;
    strcpy(q->root, root); q->check_cancel = cancel; q->check_ctx = ctx;
    int result = -1, duration;
    if(!cancelled(q)){
        int rc = cast_queue_snapshot(&q->queue, mode, pos_id, path);
        if(rc == CQ_OK) result = classify(q, q->queue.cur, &duration) == CF_V_OK ? 0 : -1;
        else if(rc == CQ_EMPTY){
            int fd = aps_media_open(root, path); audio_decode *d = NULL;
            if(fd >= 0){ q->check_error = audio_decode_open(&d, fd, 0, cancelled, q); result = q->check_error == AD_OK ? 0 : -1;
                if(!result){ const ad_info *info = audio_decode_info(d); q->check_rate = info->rate; q->check_bits = info->bits; }
                close(fd); }
            audio_decode_close(d);
        }
    }
    if(result && q->check_error == AD_UNAVAILABLE) result = APS_CHECK_DECODER;
    *rate = q->check_rate; *bits = q->check_bits;
    cast_queue_free(&q->queue); free(q); return result;
}
static int visible_position(aps_queue *q, int idx){
    int count = 0;
    for(int i = 0; i < q->queue.n; i++){
        int at = cast_cursor_at(&q->cursor, i);
        if(at < 0 || (q->removed && q->removed[at])) continue;
        if(at == idx) return count;
        count++;
    }
    return -1;
}
static int visible_count(aps_queue *q){
    int count = 0; for(int i = 0; i < q->queue.n; i++) if(!q->removed || !q->removed[i]) count++; return count;
}
static int submit(aps_queue *q, aps_message *m, int idx, int initial){
    int fd = -1;
    if(cancelled(q)) return -1;
    if(idx >= 0){
        cc_row row;
        if(cast_ctl_row_db(NULL, &q->queue, idx, &row) || (q->queue.flags[idx] & (CQ_F_CUE | CQ_F_ISO))) return -1;
        m->local_id = q->queue.src == CQ_SRC_LIVE ? q->queue.base[idx] : 0;
        m->local_ord = q->queue.src == CQ_SRC_LIVE ? idx + 1 : 0;
        snprintf(m->view.path, sizeof m->view.path, "%s", row.path);
        snprintf(m->view.title, sizeof m->view.title, "%s", row.title);
        snprintf(m->view.artist, sizeof m->view.artist, "%s", row.artist);
        if(!row.album[0]){
            char artist[160]; (void)mdb_song_meta_by_path(row.path, row.album, sizeof row.album, artist, sizeof artist);
        }
        snprintf(m->view.album, sizeof m->view.album, "%s", row.album); m->view.duration_ms = row.dur_ms;
        m->view.queue_pos = visible_position(q, idx)+1; m->view.queue_len = visible_count(q);
        m->occurrence = (uint64_t)idx + 1; m->qgen = q->queue.generation;
    } else { m->occurrence = 1; m->view.queue_pos = m->view.queue_len = 1; }
    fd = aps_media_open(q->root, m->view.path);
    if(fd < 0) return -1;
    m->type = initial ? APS_GO : APS_NEXT_FILE;
    m->view.have_track = 1;
    int rc = aps_ipc_send(q->ctl, q->attempt, m, fd); close(fd);
    return rc;
}
/* Retry only the audible playback pass. Its file request token is distinct
 * from item (the playback pass), and remains valid across queue edits/seeks.
 * The sender owns the SD lease until both this thread and the child exit. */
static int64_t art_now(void){
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void art_tick(aps_queue *q, const aps_message *view){
    int64_t now = art_now();
    if(cancelled(q)){ q->art_pending = 0; return; }
    if(view && view->view.have_track && view->item != q->art.item){
        q->art = *view; q->art_pending = 1; q->art_tried = 0;
        q->art_deadline = now + 15000; q->art_next = now;
    }
    if(!q->art_pending) return;
    if(now >= q->art_deadline){ q->art_pending = 0; return; }
    if(now < q->art_next) return;
    q->art_next = now + 250;
    /* Sample before looking up: a publication during the lookup is retried. */
    unsigned gen = artcache_gen();
    if(q->art_tried && gen == q->art_gen) return;
    q->art_gen = gen; q->art_tried = 1;
    char cover[600];
    if(artcache_cover_candidate(q->art.view.path, cover, sizeof cover)) return;
    int fd = open(cover, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    if(fd < 0) return;
    aps_message m = q->art; m.type = APS_ART; m.item = q->art.art_request;
    if(!cancelled(q) && !aps_ipc_send(q->ctl, q->attempt, &m, fd))
        q->art_pending = 0;
    else q->art_tried = 0; /* IPC backpressure: bounded by the same deadline. */
    close(fd);
}
static void view_publish(aps_queue *q){
    cw_view v = {0};
    if(q->queue.n && q->audible >= 0){
        v.active = 1; v.qgen = q->queue.generation; v.mode = q->cursor.mode;
        v.pos = visible_position(q, q->audible); v.n_queue = visible_count(q); v.src = q->queue.src;
        int raw = cast_cursor_pos_of(&q->cursor, q->audible);
        for(int i = raw; v.cnt < CW_VIEW_N && i < q->queue.n; i++){
            int idx = cast_cursor_at(&q->cursor, i);
            if(idx < 0) break;
            if(q->removed && q->removed[idx]) continue;
            int k = v.cnt++; v.occ[k] = idx; v.id[k] = cast_queue_id_at(&q->queue, idx); v.fl[k] = q->queue.flags[idx];
        }
    }
    pthread_mutex_lock(&q->mutex); q->view = v; pthread_mutex_unlock(&q->mutex);
}
static int worker_command(aps_queue *q, unsigned cmd, int idx){
    aps_message m = { .type = APS_COMMAND, .command = cmd, .occurrence = (uint64_t)idx+1, .qgen = q->queue.generation, .item = q->notified_generation };
    /* MODE carries its predecessor generation as well as the exact occurrence. */
    int rc = aps_ipc_send(q->ctl, q->attempt, &m, -1);
    if(!rc && (cmd == APS_QUEUE_JUMP || cmd == APS_QUEUE_MODE)) q->notified_generation = m.qgen;
    return rc;
}
static void *run(void *ctx){
    aps_queue *q = ctx; aps_message first = q->first;
    const char *end_reason = "queue stopped", *failure = "allocation failed";
    if(!cancelled(q) && cast_queue_snapshot(&q->queue, q->mode, q->pos_id, first.view.path) == CQ_OK){
        uint64_t seed; if(aps_random(&seed, sizeof seed) || cast_cursor_open(&q->cursor, &q->queue, classify, q, seed)) cast_queue_free(&q->queue);
    }
    q->identity_generation = q->notified_generation = q->revision = 1; q->queue.generation = q->revision;
    if(q->queue.n){ q->removed = calloc((size_t)q->queue.n, 1); if(!q->removed) goto failed; }
    q->audible = q->queue.n ? q->queue.cur : -1;
    failure = "initial submit failed";
    if(submit(q, &first, q->audible, 1)) goto failed;
    view_publish(q); failure = "queue operation failed";
    for(;;){
        pthread_mutex_lock(&q->mutex);
        while(!atomic_load(&q->stop) && !q->count && !q->have_observed){
            if(!q->art_pending) pthread_cond_wait(&q->changed, &q->mutex);
            else {
                struct timespec until = { .tv_sec = q->art_next / 1000,
                    .tv_nsec = (q->art_next % 1000) * 1000000 };
                if(pthread_cond_timedwait(&q->changed, &q->mutex, &until) == ETIMEDOUT) break;
            }
        }
        if(atomic_load(&q->stop)){ pthread_mutex_unlock(&q->mutex); break; }
        aps_message observed = q->observed; int have = q->have_observed; q->have_observed = 0;
        queue_event e = {0};
        if(q->count){ e = q->events[0]; --q->count; memmove(q->events, q->events+1, q->count*sizeof e); }
        pthread_mutex_unlock(&q->mutex);
        if(have && observed.qgen == q->queue.generation && observed.occurrence && observed.occurrence <= (uint64_t)q->queue.n){
            q->audible = (int)observed.occurrence-1; q->cursor.pos = cast_cursor_pos_of(&q->cursor, q->audible); view_publish(q);
        }
        if(cancelled(q)){ end_reason = atomic_load(&q->stop) ? "queue stopped" : "SD unavailable"; free(e.ids); break; }
        art_tick(q, have ? &observed : NULL);
        aps_message m = e.m;
        if(e.ids){
            cast_queue replacement = {0}; cast_cursor cursor = {0}; uint64_t seed;
            int rc = cast_queue_from_rows(&replacement, e.ids, e.n, e.start, e.mode, e.kind); free(e.ids);
            if(rc != CQ_OK || aps_random(&seed, sizeof seed) || cast_cursor_open(&cursor, &replacement, classify, q, seed)){
                cast_queue_free(&replacement); (void)worker_command(q, APS_QUEUE_CHANGED, 0); continue;
            }
            int duration;
            if(classify_row(q, &replacement, replacement.cur, &duration) != CF_V_OK){
                cast_cursor_close(&cursor); cast_queue_free(&replacement);
                (void)worker_command(q, APS_QUEUE_REFUSED, 0); continue;
            }
            unsigned char *removed = calloc((size_t)replacement.n, 1);
            if(!removed || q->revision == UINT32_MAX){ free(removed); cast_cursor_close(&cursor); cast_queue_free(&replacement); continue; }
            free(q->removed); q->removed = removed;
            cast_cursor_close(&q->cursor); cast_queue_free(&q->queue);
            replacement.generation = ++q->revision; q->identity_generation = replacement.generation;
            q->queue = replacement; q->cursor = cursor; q->cursor.q = &q->queue; q->audible = q->queue.cur;
            if(q->cursor.mode == 1) (void)cast_cursor_start_shuffled(&q->cursor, 0);
            view_publish(q);
            if(worker_command(q, APS_QUEUE_JUMP, q->audible)) goto failed;
            continue;
        }
        if(!m.type) continue;
        if(m.type == APS_COMMAND){
            if(m.command == APS_QUEUE_REMOVE || m.command == APS_QUEUE_MOVE_NEXT){
                if(m.qgen != q->queue.generation || !m.occurrence || m.occurrence > (uint64_t)q->queue.n || q->revision == UINT32_MAX){ (void)worker_command(q, APS_QUEUE_CHANGED, 0); continue; }
                int idx = (int)m.occurrence-1;
                if(idx == q->audible || q->removed[idx]) continue;
                if(m.command == APS_QUEUE_REMOVE) q->removed[idx] = 1;
                else {
                    int from = cast_cursor_pos_of(&q->cursor, idx), to = cast_cursor_pos_of(&q->cursor, q->audible)+1;
                    if(from < 0 || from < to) continue;
                    memmove(q->cursor.order+to+1, q->cursor.order+to, (size_t)(from-to)*sizeof(int)); q->cursor.order[to] = idx;
                }
                q->queue.generation = ++q->revision;
                view_publish(q); if(worker_command(q, APS_QUEUE_MODE, q->audible)) goto failed;
            } else if(m.command == APS_QUEUE_MODE){
                if(cast_cursor_set_mode(&q->cursor, e.mode)) continue;
                view_publish(q); if(worker_command(q, APS_QUEUE_MODE, q->audible)) goto failed;
            } else if(m.command == APS_QUEUE_JUMP && m.qgen == q->queue.generation && m.occurrence && m.occurrence <= (uint64_t)q->queue.n){
                int duration;
                if(q->removed[(int)m.occurrence-1] || classify(q, (int)m.occurrence-1, &duration) != CF_V_OK){ (void)worker_command(q, APS_QUEUE_REFUSED, 0); continue; }
                if(worker_command(q, APS_QUEUE_JUMP, (int)m.occurrence-1)) goto failed;
            } else if(m.command == APS_QUEUE_JUMP) (void)worker_command(q, APS_QUEUE_CHANGED, 0);
            continue;
        }
        int next = -1, skipped = 0;
        if(q->queue.n && m.qgen >= q->identity_generation && m.qgen <= q->queue.generation && m.occurrence && m.occurrence <= (uint64_t)q->queue.n){
            int occurrence = (int)m.occurrence-1;
            if(m.command == APS_QUEUE_JUMP && !q->removed[occurrence]){
                next = occurrence; int duration;
                if(classify(q, next, &duration) != CF_V_OK) next = -1;
            } else {
                int current = cast_cursor_pos_of(&q->cursor, occurrence);
                int direction = m.command == PB_CMD_PREV ? -1 : 1;
                int natural = m.command == 0, mode = q->cursor.mode;
                int wrap = !natural || mode == 1 || mode == 3;
                if(current >= 0 && !(natural && mode == 4)){
                    for(int i = 1; i <= q->queue.n && !cancelled(q); i++){
                        int pos = natural && mode == 2 ? current : current+direction*i;
                        if(pos < 0 || pos >= q->queue.n){ if(!wrap) break; pos = (pos%q->queue.n+q->queue.n)%q->queue.n; }
                        int idx = cast_cursor_at(&q->cursor, pos), duration;
                        if(q->removed[idx]) continue;
                        int result = classify(q, idx, &duration);
                        if(result == CF_V_OK){ next = idx; break; }
                        if(result != CF_V_EXCLUDED || (natural && mode == 2)){ failure = "next classify failed"; goto failed; }
                        skipped++;
                    }
                }
            }
        }
        if(skipped){ aps_message notice = { .type = APS_NOTICE, .command = APS_QUEUE_SKIP, .position_ms = skipped }; (void)aps_ipc_send(q->ctl, q->attempt, &notice, -1); }
        m.position_ms = 0;
        if(next < 0){
            m.type = APS_ENDED; int rc = aps_ipc_send(q->ctl, q->attempt, &m, -1);
            fprintf(stderr, "AirPlay queue end reason=no next item post=%d\n", rc);
            if(rc){ end_reason = "ENDED send failed"; break; }
        }
        else if(submit(q, &m, next, 0)){ failure = "next submit failed"; goto failed; }
    }
    fprintf(stderr, "AirPlay queue exit reason=%s\n", end_reason);
    return NULL;
failed:
    { aps_message m = { .type = APS_FAILED, .error = APS_ERR_FORMAT };
      int rc = aps_ipc_send(q->ctl, q->attempt, &m, -1);
      fprintf(stderr, "AirPlay queue failure reason=%s error=%d post=%d\n",
          atomic_load(&q->stop) ? "queue stopped" : !sd_io_allowed() ? "SD unavailable" : failure, m.error, rc); }
    return NULL;
}
aps_queue *aps_queue_start(int ctl, uint32_t attempt, const char *root, int mode, int pos_id, const aps_message *first){
    if(!root || strlen(root) >= 256) return NULL;
    aps_queue *q = calloc(1, sizeof *q); if(!q) return NULL;
    q->ctl = ctl; q->attempt = attempt; q->mode = mode; q->pos_id = pos_id; q->first = *first; strcpy(q->root, root);
    if(pthread_mutex_init(&q->mutex, NULL)){ free(q); return NULL; }
    pthread_condattr_t attr;
    if(pthread_condattr_init(&attr)){ pthread_mutex_destroy(&q->mutex); free(q); return NULL; }
    int err = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if(!err) err = pthread_cond_init(&q->changed, &attr);
    pthread_condattr_destroy(&attr);
    if(err){ pthread_mutex_destroy(&q->mutex); free(q); return NULL; }
    if(pthread_create(&q->thread, NULL, run, q)){ pthread_cond_destroy(&q->changed); pthread_mutex_destroy(&q->mutex); free(q); return NULL; }
    return q;
}
static int post(aps_queue *q, queue_event e){
    if(!q){ free(e.ids); return -1; }
    pthread_mutex_lock(&q->mutex);
    if(q->count == 16 || atomic_load(&q->stop)){ pthread_mutex_unlock(&q->mutex); free(e.ids); return -1; }
    q->events[q->count++] = e; pthread_cond_signal(&q->changed); pthread_mutex_unlock(&q->mutex); return 0;
}
int aps_queue_request(aps_queue *q, const aps_message *m){ return post(q, (queue_event){ .m = *m }); }
void aps_queue_observe(aps_queue *q, const aps_message *m){
    if(!q) return;
    pthread_mutex_lock(&q->mutex); q->observed = *m; q->have_observed = 1;
    pthread_cond_signal(&q->changed); pthread_mutex_unlock(&q->mutex);
}
int aps_queue_select(aps_queue *q, const int *ids, int n, int start, int mode, int kind){
    if(!q || !ids || n <= 0 || n > CQ_MAX_OCCURRENCES || start < 0 || start >= n || mode < 0 || mode > 4 || kind < 0 || kind > 1) return -1;
    int *copy = malloc((size_t)n*sizeof *copy); if(!copy) return -1;
    memcpy(copy, ids, (size_t)n*sizeof *copy);
    return post(q, (queue_event){ .ids = copy, .n = n, .start = start, .mode = mode, .kind = kind });
}
int aps_queue_mode(aps_queue *q, int mode){
    if(mode < 0 || mode > 4) return -1;
    return post(q, (queue_event){ .m = { .type = APS_COMMAND, .command = APS_QUEUE_MODE }, .mode = mode });
}
int aps_queue_view(aps_queue *q, cw_view *v){
    memset(v, 0, sizeof *v); if(!q) return 0;
    pthread_mutex_lock(&q->mutex); *v = q->view; pthread_mutex_unlock(&q->mutex); return v->active;
}
int aps_queue_jump(aps_queue *q, uint32_t gen, int occurrence){
    return post(q, (queue_event){ .m = { .type = APS_COMMAND, .command = APS_QUEUE_JUMP, .qgen = gen, .occurrence = (uint64_t)occurrence+1 } });
}
int aps_queue_edit(aps_queue *q, uint32_t gen, int occurrence, unsigned operation){
    if(operation != APS_QUEUE_REMOVE && operation != APS_QUEUE_MOVE_NEXT) return -1;
    return post(q, (queue_event){ .m = { .type = APS_COMMAND, .command = operation, .qgen = gen, .occurrence = (uint64_t)occurrence+1 } });
}
void aps_queue_stop(aps_queue *q){
    if(!q) return;
    atomic_store(&q->stop, 1); pthread_mutex_lock(&q->mutex); pthread_cond_signal(&q->changed); pthread_mutex_unlock(&q->mutex);
    pthread_join(q->thread, NULL);
    for(unsigned i = 0; i < q->count; i++) free(q->events[i].ids);
    cast_cursor_close(&q->cursor); cast_queue_free(&q->queue);
    pthread_cond_destroy(&q->changed); pthread_mutex_destroy(&q->mutex); free(q->removed); free(q);
}
