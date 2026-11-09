/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_sender.h"
#include "airplay_crypto.h"
#include "sdio.h"
#include "audio_stock_process.h"
#include "audio_stock_broker.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
typedef struct {
    pthread_t thread; _Atomic int done, cancel; int result, mode, pos_id; unsigned rate, bits;
    const char *root; char path[TRACK_PATH_MAX]; unsigned generation; uint64_t token;
} input_check;
static int input_cancel(void *ctx){ input_check *c = ctx; return atomic_load(&c->cancel); }
static void *input_run(void *ctx){
    input_check *c = ctx;
    c->result = aps_queue_check(c->root, c->mode, c->pos_id, c->path, input_cancel, c, &c->rate, &c->bits);
    sd_io_end(); atomic_store(&c->done, 1); return NULL;
}
static int due(uint32_t now, uint32_t end){ return (int32_t)(now - end) >= 0; }
/* Same descriptor-isolation mechanism as the reviewed proof supervisor. */
struct sender_dirent { uint64_t ino; int64_t offset; unsigned short length; unsigned char type; char name[]; };
static int isolate(void){
    int directory = open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC); if(directory < 0) return -1;
    char b[2048];
    for(;;){
        long n = syscall(SYS_getdents64, directory, b, sizeof b);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0){ close(directory); return n ? -1 : 0; }
        for(long at = 0; at < n;){
            struct sender_dirent *d = (struct sender_dirent *)(b + at);
            if(!d->length || d->length > n - at){ close(directory); return -1; }
            int fd = 0, valid = d->name[0] != 0;
            for(const char *p = d->name; *p; p++){
                if(*p < '0' || *p > '9' || fd > 100000000){ valid = 0; break; }
                fd = fd * 10 + *p - '0';
            }
            if(valid && fd >= 5 && fd != directory) close(fd);
            at += d->length;
        }
    }
}
static int spawn(aps_sender *a){
    int sockets[2], decoder[2]; char attempt[16]; struct sigaction child;
    if(sigaction(SIGCHLD,NULL,&child) || child.sa_handler!=SIG_DFL || (child.sa_flags&SA_NOCLDWAIT)) return -1;
    if(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets)) return -1;
    if(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, decoder)){ close(sockets[0]); close(sockets[1]); return -1; }
    snprintf(attempt, sizeof attempt, "%u", a->serial);
    char *const argv[] = { APS_WORKER_NAME, APS_WORKER_MARKER, "--ctl", "3", "--attempt", attempt, NULL };
    pid_t pid = fork();
    if(pid < 0){ close(decoder[0]); close(decoder[1]); close(sockets[0]); close(sockets[1]); return -1; }
    if(!pid){
        sigset_t none; sigemptyset(&none); sigprocmask(SIG_SETMASK, &none, NULL); setpgid(0, 0);
        int fd = fcntl(sockets[1], F_DUPFD, 10), broker = fcntl(decoder[1], F_DUPFD, 10);
        if(fd < 0 || broker < 0 || dup2(fd, 3) < 0 || dup2(broker, 4) < 0 || isolate()) _exit(126);
        int null = open("/dev/null", O_RDWR);
        if(null < 0 || dup2(null, 0) < 0 || dup2(null, 1) < 0) _exit(126);
        if(null > 3) close(null);
        execv("/proc/self/exe", argv); _exit(127);
    }
    close(decoder[1]); a->decoder_ctl=decoder[0];
    close(sockets[1]); setpgid(pid, pid); a->pid = pid; a->ctl = sockets[0]; return 0;
}
static int send_message(aps_sender *a, aps_message *m){ return aps_ipc_send(a->ctl, a->serial, m, -1); }
static void terminate(aps_sender *a){
    if(a->preflight) atomic_store(&((input_check *)a->preflight)->cancel, 1);
    if(a->pid <= 0 || a->terminated) return;
    aps_message m = { .type = APS_CANCEL }; (void)send_message(a, &m);
    a->terminated = 1; a->terminate_at = a->now + 1500; a->phase = APSS_ENDING; a->pin_wanted = 0;
}
static int pause_local(void *ctx){ aps_sender *a = ctx; return a->cfg.pause ? a->cfg.pause() : -1; }
static int load(void *ctx, uint32_t attempt, const char *path, long ms){
    aps_sender *a = ctx; (void)attempt;
    if(a->queue || ms < 0 || !sd_io_begin()) return -1;
    a->lease = 1;
    aps_message m = { .type = APS_GO, .position_ms = ms };
    snprintf(m.view.path, sizeof m.view.path, "%s", path);
    snprintf(m.view.title, sizeof m.view.title, "%s", a->handoff->title);
    snprintf(m.view.artist, sizeof m.view.artist, "%s", a->handoff->artist);
    snprintf(m.view.album, sizeof m.view.album, "%s", a->handoff->album);
    size_t name_len = strnlen(a->target.name, sizeof m.view.dest - 1);
    while(name_len && ((unsigned char)a->target.name[name_len] & 0xc0) == 0x80) --name_len;
    memcpy(m.view.dest, a->target.name, name_len); m.view.dest[name_len] = 0;
    m.view.duration_ms = a->handoff->dur; m.view.position_ms = ms;
    a->queue = aps_queue_start(a->ctl, a->serial, a->cfg.sd_root, a->mode, a->pos_id, &m);
    if(!a->queue){ a->lease = 0; sd_io_end(); return -1; }
    return 0;
}
static int play(void *ctx){
    aps_sender *a = ctx; aps_message m = { .type = APS_COMMAND, .command = PB_CMD_TOGGLE, .play = 1 };
    return send_message(a, &m);
}
static int stop(void *ctx){ terminate(ctx); return 0; }
static void sender_revoke(void *ctx){ (void)ctx; }
static void release(void *ctx){ terminate(ctx); }
static void publish(void *ctx, const pb_cast_state *v){
    aps_sender *a = ctx;
    if(v) a->view = *v;
    (void)playback_set_remote(PB_SRC_AIRPLAY, v);
}
static void say(void *ctx, ho_msg msg){ aps_sender *a = ctx; if(a->cfg.say) a->cfg.say(msg); }
static int teardown(void *ctx){ aps_sender *a = ctx; return a->cfg.teardown ? a->cfg.teardown(a->handoff->gen) : -1; }
static ho_sink sink(aps_sender *a){
    ho_sink k = { a, pause_local, load, play, stop, sender_revoke, release, publish, say, teardown }; return k;
}
static void event(aps_sender *a, ho_evtype type, const ho_local *l){
    ho_sink k = sink(a); ho_event e = { type, ho_attempt(a->handoff), l, a->target.name, 1 };
    ho_step(a->handoff, a->now, &e, &k);
}
static int input_start(aps_sender *a, const ho_local *l){
    if(a->preflight || !l || !l->sd_ok || !l->have_track || !l->path || strlen(l->path) >= 256) return -1;
    input_check *c = calloc(1, sizeof *c); if(!c) return -1;
    atomic_init(&c->done, 0); atomic_init(&c->cancel, 0);
    c->root = a->cfg.sd_root; c->mode = a->mode; c->pos_id = a->pos_id;
    c->generation = l->generation; c->token = l->token; strcpy(c->path, l->path);
    if(!sd_io_begin()){ free(c); return -1; }
    if(pthread_create(&c->thread, NULL, input_run, c)){ sd_io_end(); free(c); return -1; }
    a->input_generation = c->generation; a->input_token = c->token;
    a->input_mode = c->mode; a->input_pos_id = c->pos_id; strcpy(a->input_path, c->path);
    a->preflight = c; return 0;
}
static int input_same(const aps_sender *a, const ho_local *l){
    return l && l->have_track && l->path && a->input_generation == l->generation && a->input_token == l->token &&
        !strcmp(a->input_path, l->path) && a->input_mode == a->mode && a->input_pos_id == a->pos_id;
}
static void start_handoff(aps_sender *a, const ho_local *l){
    ho_sink k = sink(a); ho_event e = { HO_EV_SELECT, 0, l, a->target.name, 0 };
    a->phase = APSS_RUNNING; ho_step(a->handoff, a->now, &e, &k);
    if(ho_get(a->handoff) != HO_PREPARING){ terminate(a); return; }
    event(a, HO_EV_REMOTE_READY, l);
}
static void input_tick(aps_sender *a, const ho_local *l){
    input_check *c = a->preflight; if(!c || !atomic_load(&c->done)) return;
    pthread_join(c->thread, NULL); a->preflight = NULL;
    int result = c->result; unsigned rate = c->rate, bits = c->bits;
    int same = l && l->have_track && l->path && c->generation == l->generation && c->token == l->token &&
        !strcmp(c->path, l->path) && c->mode == a->mode && c->pos_id == a->pos_id;
    free(c);
    if(a->phase != APSS_PREPARING || a->terminated || a->pid <= 0) return;
    if(!same){ if(input_start(a, l)){ a->error = APS_ERR_FORMAT; terminate(a); } return; }
    if(result == APS_CHECK_DECODER){ a->error = APS_ERR_DECODER; terminate(a); return; }
    if(result != 0){ a->error = APS_ERR_FORMAT; terminate(a); return; }
    if(!a->stream_ready){
        aps_message m = { .type = APS_FORMAT, .rate = (int)rate, .bits = (int)bits };
        if(send_message(a, &m)){ terminate(a); return; }
        a->format_pending = 1; return;
    }
    start_handoff(a, l);
}
void aps_sender_init(aps_sender *a, ho *h, const aps_config *cfg){
    memset(a, 0, sizeof *a); a->ctl = a->decoder_ctl = -1; a->handoff = h; a->cfg = *cfg;
}
int aps_sender_active(const aps_sender *a){ return a->pid > 0 || a->phase != APSS_IDLE || a->preflight != NULL; }
int aps_sender_begin(aps_sender *a, const rd_receiver *target, const ho_local *l, uint32_t now){
    if(aps_sender_active(a) || ho_get(a->handoff) != HO_LOCAL || !l || !l->sd_ok || !l->have_track ||
       l->source_mode || !target || target->protocol == RD_CAST || a->serial == UINT32_MAX) return -1;
    memset(&a->view, 0, sizeof a->view); a->stop_confirmed = 0; a->stream_ready = a->format_pending = 0; a->have_volume = 0; a->volume = 0;
    a->target = *target; a->now = now; ++a->serial; a->error = a->terminated = a->faulted = 0;
    a->player_generation = l->generation; a->source = l->source_mode;
    if(!sd_io_owner_begin()) return -1;
    a->owner = 1;
    if(spawn(a)){ sd_io_owner_end(); a->owner = 0; return -1; }
    a->phase = APSS_PREPARING; a->deadline = now + 180000; a->pin_wanted = 0;
    aps_message m = { .type = APS_TARGET, .target = *target, .command = (uint32_t)!!a->forget_next };
    a->forget_next = 0;
    if(send_message(a, &m)){ terminate(a); return -1; }
    return 0;
}
void aps_sender_cancel(aps_sender *a, uint32_t now){
    a->now = now;
    if(a->phase == APSS_RUNNING) event(a, HO_EV_STOP, NULL);
    terminate(a);
}
int aps_sender_pin(aps_sender *a, const char *pin){
    size_t n = pin ? strlen(pin) : 0;
    if(!a->pin_wanted || a->phase != APSS_PREPARING || n < 4 || n > 8) return -1;
    for(size_t i = 0; i < n; i++) if(pin[i] < '0' || pin[i] > '9') return -1;
    aps_message m = { .type = APS_PIN }; memcpy(m.pin, pin, n + 1);
    int rc = send_message(a, &m); aps_wipe(&m, sizeof m); if(!rc) a->pin_wanted = 0; return rc;
}
int aps_sender_volume_query(aps_sender *a){
    if(a->phase != APSS_RUNNING || ho_get(a->handoff) != HO_CASTING) return -1;
    aps_message m = { .type = APS_COMMAND, .command = APS_VOLUME_QUERY }; return send_message(a, &m);
}
int aps_sender_volume_step(aps_sender *a, int direction){
    if(a->phase != APSS_RUNNING || ho_get(a->handoff) != HO_CASTING || (direction != -1 && direction != 1)) return -1;
    aps_message m = { .type = APS_COMMAND, .command = PB_CMD_VOLUME, .position_ms = direction > 0 ? -1 : -2 };
    return send_message(a, &m);
}
int aps_sender_command(aps_sender *a, pb_cmd command, long arg){
    if(a->phase != APSS_RUNNING || ho_get(a->handoff) != HO_CASTING || command > PB_CMD_VOLUME || command < PB_CMD_TOGGLE) return -1;
    aps_message m = { .type = APS_COMMAND, .command = (uint32_t)command, .position_ms = arg, .play = -1 };
    return send_message(a, &m);
}
static void receive(aps_sender *a, const ho_local *l){
    for(unsigned i = 0; i < 32; i++){
        aps_message m; int fd; int n = aps_ipc_receive(a->ctl, a->serial, &m, &fd);
        if(!n) break;
        if(n < 0){ terminate(a); break; }
        if(fd >= 0){ close(fd); terminate(a); break; }
        if(m.type == APS_NEED_PIN && a->phase == APSS_PREPARING) a->pin_wanted = 1;
        else if(m.type == APS_READY && a->phase == APSS_PREPARING){
            a->pin_wanted = 0;
            if(input_start(a, l)){ a->error = APS_ERR_FORMAT; terminate(a); }
        } else if(m.type == APS_STREAM_READY && a->phase == APSS_PREPARING && a->format_pending){
            a->format_pending = 0; a->stream_ready = 1;
            if(input_same(a, l)) start_handoff(a, l);
            else if(input_start(a, l)){ a->error = APS_ERR_FORMAT; terminate(a); }
        } else if(m.type == APS_STARTED && a->phase == APSS_RUNNING){
            event(a, HO_EV_LOAD_OK, l); event(a, HO_EV_MEDIA, l);
        } else if(m.type == APS_VIEW && a->phase == APSS_RUNNING && ho_get(a->handoff) == HO_CASTING){
            aps_queue_observe(a->queue, &m);
            cw_view queue_view;
            if(aps_queue_view(a->queue, &queue_view) && queue_view.qgen == m.qgen && queue_view.cnt &&
               (uint64_t)queue_view.occ[0]+1 == m.occurrence){ m.view.queue_pos = queue_view.pos+1; m.view.queue_len = queue_view.n_queue; }
            a->rate = m.rate; a->bits = m.bits; a->volume = m.volume; a->have_volume = m.have_volume; a->local_id = m.local_id; a->local_ord = m.local_ord; a->view.active = 1; a->view.v = m.view; (void)playback_set_remote(PB_SRC_AIRPLAY, &a->view);
        } else if(m.type == APS_NEED_NEXT && a->queue && a->phase == APSS_RUNNING){
            if(aps_queue_request(a->queue, &m)) terminate(a);
        } else if(m.type == APS_NOTICE){
            if(m.command == APS_QUEUE_CHANGED){ a->queue_changed++; continue; }
            if(m.command == APS_QUEUE_REFUSED){ a->unsupported_choice++; continue; }
            if(m.command == APS_QUEUE_SKIP){ if(m.position_ms > 0 && m.position_ms <= CQ_MAX_OCCURRENCES) a->skipped += (unsigned)m.position_ms; continue; }
            if(m.command == APS_REMOTE_STOP){ aps_sender_cancel(a, a->now); continue; }
            if(a->cfg.notice && m.command <= PB_CMD_VOLUME) a->cfg.notice((pb_cmd)m.command);
        } else if(m.type == APS_STOPPED){
            a->stop_confirmed = 1;
        } else if(m.type == APS_FAILED){
            if(a->error != APS_ERR_DECODER) a->error = m.error;
            terminate(a);
        } else if(m.type != APS_ENDED){ terminate(a); break; }
    }
}
/* Creation is synchronous in the supervisor, before any response. A worker
 * can die at any instruction without leaving an unregistered media owner. */
static void decoder_tick(aps_sender *a){
    if(a->decoder_unknown) return;
    if(a->decoder_ctl>=0 && !a->decoder_closing){
        unsigned op; int fd=-1;
        int n=asb_receive(a->decoder_ctl,a->serial,&op,&fd);
        if(n>0){
            if(op==ASB_OPEN && !a->decoder_pid && a->lease && !a->terminated){
                int stream=-1;
                a->decoder_pid=stock_process_spawn(fd,&stream);
                close(fd); fd=-1;
                if(a->decoder_pid<0){
                    if(a->decoder_pid==STOCK_SPAWN_UNPREPARED && !a->error) a->error=APS_ERR_DECODER;
                    a->decoder_pid=0; terminate(a);
                }
                else if(asb_send(a->decoder_ctl,a->serial,ASB_READY,stream)) terminate(a);
                if(stream>=0) close(stream);
            } else if(op==ASB_CLOSE && a->decoder_pid>0) a->decoder_closing=1;
            else terminate(a);
        } else if(n<0) terminate(a);
        if(fd>=0) close(fd);
    }
    if(a->decoder_pid>0 && (a->decoder_closing || a->terminated>=2 || a->worker_reaped)){
        kill(a->decoder_pid,SIGKILL);
        pid_t p=waitpid(a->decoder_pid,NULL,WNOHANG);
        if(p==a->decoder_pid){
            a->decoder_pid=0;
            if(a->decoder_closing && a->decoder_ctl>=0 &&
               asb_send(a->decoder_ctl,a->serial,ASB_REAPED,-1)) terminate(a);
            a->decoder_closing=0;
        } else if(p<0 && errno!=EINTR){
            a->decoder_unknown=1; a->faulted=1; sd_io_fault(); /* Unknown ownership never drains or signals again. */
        }
    }
}
void aps_sender_tick(aps_sender *a, const ho_local *l, uint32_t now){
    if(!aps_sender_active(a)) return;
    a->now = now;
    if(a->phase == APSS_PREPARING && (due(now, a->deadline) || (l &&
       (!l->sd_ok || l->generation != a->player_generation || l->source_mode != a->source)))){
        a->error = due(now, a->deadline) ? APS_ERR_TIMEOUT : APS_ERR_STREAM; terminate(a);
    }
    if(a->ctl >= 0) receive(a, l);
    decoder_tick(a);
    input_tick(a, l);
    if(a->phase == APSS_RUNNING){ event(a, HO_EV_LOCAL, l); if(ho_get(a->handoff) == HO_LOCAL) terminate(a); }
    int status; pid_t p = a->pid > 0 && !a->worker_reaped ? waitpid(a->pid, &status, WNOHANG) : 0;
    if(p == a->pid && p > 0){ a->worker_reaped=1; }
    if(a->worker_reaped){
        /* Exit can race the first receive pass. Collect the child's final
         * authenticated private stop acknowledgement before closing ctl. */
        if(a->ctl >= 0) receive(a, l);
        /* Close creation channel before releasing ownership. Queued OPENs
         * cannot create children after this point. Keep worker state/lease
         * until the last directly owned helper has actually been reaped. */
        if(a->decoder_ctl>=0){ close(a->decoder_ctl); a->decoder_ctl=-1; }
        decoder_tick(a);
        if(a->decoder_pid>0) return;
        if(a->preflight){
            atomic_store(&((input_check *)a->preflight)->cancel, 1);
            return; /* input_tick joins it before ownership can be cleared */
        }
        a->worker_reaped=0;
        if(a->ctl >= 0) close(a->ctl);
        a->ctl = -1; a->pid = 0;
        if(a->preflight) atomic_store(&((input_check *)a->preflight)->cancel, 1);
        aps_queue_stop(a->queue); a->queue = NULL;
        if(a->lease){ a->lease = 0; sd_io_end(); }
        if(a->owner){ sd_io_owner_end(); a->owner = 0; }
        if(ho_get(a->handoff) != HO_LOCAL) event(a, ho_get(a->handoff) == HO_STOPPING && a->stop_confirmed ? HO_EV_REMOTE_STOPPED : HO_EV_REMOTE_LOST, l);
        if(!a->stop_confirmed && a->view.active) a->error = APS_ERR_STOP;
        else if(!a->terminated && !a->error) a->error = APS_ERR_STREAM;
        (void)playback_set_remote(PB_SRC_AIRPLAY, NULL); a->phase = APSS_IDLE; a->pin_wanted = 0;
    } else if(a->pid > 0 && a->terminated == 1 && due(now, a->terminate_at)){
        kill(-a->pid, SIGTERM); kill(a->pid, SIGTERM); a->terminated = 2; a->kill_at = now + 1500;
    } else if(a->pid > 0 && a->terminated == 2 && due(now, a->kill_at)){
        kill(-a->pid, SIGKILL); kill(a->pid, SIGKILL); a->terminated = 3; a->fault_at = now + 2000;
    } else if(a->pid > 0 && a->terminated == 3 && !a->faulted && due(now, a->fault_at)){
        a->faulted = 1; sd_io_fault(); /* unknown child state: never report drained */
    }
}
