/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "procrun.h"
#include "ui_wake.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* One permanent worker owns all children, pipes and deferred reaping. Never replace it.
 * Keep the leader unreaped until group cleanup, including when descendants hold stdout. */
static struct job {
    unsigned id; int done, consumed, cancel;
    char cmd[512], *out; int cap, n, fd, started, eof;
    pid_t pid; int64_t deadline;
} jobs[PROC_SLOTS];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static pthread_cond_t completed = PTHREAD_COND_INITIALIZER;
static unsigned serial;
static int running;
static int notify[2] = {-1, -1};
/* mu held: the condition covers idle waits; the pipe interrupts an active poll.
 * A full nonblocking pipe already holds a wake notification. */
static void signal_worker(void){
    pthread_cond_signal(&wake);
    char b = 0;
    while(write(notify[1], &b, 1) < 0 && errno == EINTR){}
}
static int64_t now_ms(void){
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void finish(struct job *j, int result){
    if(j->pid > 0){ kill(-j->pid, SIGKILL); kill(j->pid, SIGKILL); }
    if(j->fd >= 0){ close(j->fd); j->fd = -1; }
    j->out[j->n] = 0;
    pthread_mutex_lock(&mu);
    j->done = result;
    pthread_cond_broadcast(&completed);
    pthread_mutex_unlock(&mu);
    ui_notify(UI_WAKE_BT);
}
static void launch(struct job *j){
    int f[2]; j->started = 1;
    long fd_limit = sysconf(_SC_OPEN_MAX);
    if(fd_limit < 3 || fd_limit > INT_MAX){ finish(j, PROC_FAILED); return; }
    if(pipe2(f, O_CLOEXEC) != 0){ finish(j, PROC_FAILED); return; }
    j->pid = fork();
    if(j->pid == 0){
        if(setpgid(0, 0) != 0) _exit(126);
        if(dup2(f[1], STDOUT_FILENO) < 0) _exit(126);
        if(fcntl(STDOUT_FILENO, F_SETFD, 0) < 0) _exit(126);
        if(f[0] != STDOUT_FILENO) close(f[0]);
        if(f[1] != STDOUT_FILENO) close(f[1]);
        for(int fd = 3; fd < fd_limit; fd++) close(fd);
        execl("/bin/sh", "sh", "-c", j->cmd, (char *)0); _exit(127);
    }
    close(f[1]);
    if(j->pid < 0){ close(f[0]); finish(j, PROC_FAILED); return; }
    setpgid(j->pid, j->pid);
    j->fd = f[0];
    if(fcntl(j->fd, F_SETFL, O_NONBLOCK) < 0) finish(j, PROC_FAILED);
}
static void *worker(void *unused){
    (void)unused;
    for(;;){
        for(int i=0; i<PROC_SLOTS; i++){
            struct job *j = &jobs[i];
            pthread_mutex_lock(&mu);
            unsigned id = j->id; int cancel = j->cancel, done = j->done;
            pthread_mutex_unlock(&mu);
            if(!id) continue;
            if(!done){
                if(cancel) finish(j, PROC_CANCELLED);
                else if(now_ms() >= j->deadline) finish(j, PROC_TIMED_OUT);
                else {
                    if(!j->started) launch(j);
                    if(j->fd >= 0){
                        char b[1024]; ssize_t n = read(j->fd, b, sizeof b);
                        if(now_ms() >= j->deadline) finish(j, PROC_TIMED_OUT);
                        else if(n > 0){
                            int room = j->cap - 1 - j->n;
                            int take = n > room ? room : (int)n;
                            memcpy(j->out+j->n, b, take); j->n += take;
                            if(n > room) finish(j, PROC_TRUNCATED);
                        } else if(n == 0){
                            close(j->fd); j->fd = -1; j->eof = 1;
                        } else if(errno != EAGAIN && errno != EINTR) finish(j, PROC_FAILED);
                    }
                    if(j->eof && !j->done){
                        siginfo_t si = {0};
                        if(waitid(P_PID, j->pid, &si, WEXITED|WNOHANG|WNOWAIT) == 0 && si.si_pid)
                            finish(j, si.si_code == CLD_EXITED && !si.si_status ? PROC_COMPLETE : PROC_FAILED);
                    }
                }
            }
            if(j->done && j->pid > 0){
                pid_t p = waitpid(j->pid, NULL, WNOHANG);
                if(p == j->pid || (p < 0 && errno == ECHILD)) j->pid = 0;
            }
            pthread_mutex_lock(&mu);
            if(j->done && j->consumed && j->pid <= 0){ free(j->out); j->id = 0; }
            pthread_mutex_unlock(&mu);
        }
        pthread_mutex_lock(&mu);
        struct pollfd fds[PROC_SLOTS+1] = {{ .fd=notify[0], .events=POLLIN }};
        int nfds = 1, service = 0, timeout = -1;
        int64_t now = now_ms();
        for(int i=0;i<PROC_SLOTS;i++){
            struct job *j = &jobs[i];
            if(!j->id) continue;
            if(!j->done){
                service = 1;
                int64_t left = j->deadline - now;
                int ms = left <= 0 ? 0 : left > INT_MAX ? INT_MAX : (int)left;
                if(j->cancel || !j->started) ms = 0;
                /* EOF can precede leader exit. Avoid a permanent POLLHUP loop;
                 * only that short lifecycle gap needs a bounded waitid retry. */
                if(j->eof && ms > 10) ms = 10;
                if(timeout < 0 || ms < timeout) timeout = ms;
                if(j->fd >= 0) fds[nfds++] = (struct pollfd){ .fd=j->fd, .events=POLLIN };
            } else if(j->pid > 0){
                service = 1;
                if(timeout < 0 || timeout > 10) timeout = 10; /* deferred reap after SIGKILL */
            } else if(j->consumed){
                service = 1; timeout = 0; /* consumption raced the slot-cleanup pass */
            }
        }
        if(!service){
            pthread_cond_wait(&wake, &mu);
            pthread_mutex_unlock(&mu);
        } else {
            pthread_mutex_unlock(&mu);
            poll(fds, nfds, timeout);
            char b[64];
            while(read(notify[0], b, sizeof b) > 0){}
        }
    }
    return NULL;
}
unsigned proc_start(const char *cmd, int timeout_ms, int cap){
    if(!cmd || strlen(cmd) >= sizeof jobs[0].cmd || timeout_ms <= 0 || cap < 1 || cap > PROC_OUTPUT) return 0;
    pthread_mutex_lock(&mu);
    if(!running){
        pthread_t t;
        if(pipe2(notify, O_CLOEXEC|O_NONBLOCK) != 0){ pthread_mutex_unlock(&mu); return 0; }
        if(pthread_create(&t, NULL, worker, NULL)){
            close(notify[0]); close(notify[1]);
            pthread_mutex_unlock(&mu); return 0;
        }
        pthread_detach(t); running = 1;
    }
    unsigned id = 0;
    for(int i=0;i<PROC_SLOTS;i++) if(!jobs[i].id){
        struct job *j = &jobs[i];
        char *out = malloc(cap); if(!out) break;
        memset(j, 0, sizeof *j); j->out = out;
        if(++serial == 0) ++serial;
        id = j->id = serial; j->fd = -1; j->cap = cap;
        j->deadline = now_ms() + timeout_ms; strcpy(j->cmd, cmd); break;
    }
    signal_worker(); pthread_mutex_unlock(&mu); return id;
}
int proc_poll(unsigned id, char *out, int cap){
    int r = 0; if(cap < 1) return 0;
    out[0] = 0; pthread_mutex_lock(&mu);
    for(int i=0;i<PROC_SLOTS;i++) if(id && jobs[i].id == id && !jobs[i].consumed && jobs[i].done){
        struct job *j = &jobs[i]; int n = j->n < cap-1 ? j->n : cap-1;
        memcpy(out, j->out, n); out[n] = 0;
        r = n < j->n ? PROC_TRUNCATED : j->done; j->consumed = 1; signal_worker(); break;
    }
    pthread_mutex_unlock(&mu); return r;
}
void proc_cancel(unsigned id){
    pthread_mutex_lock(&mu);
    for(int i=0;i<PROC_SLOTS;i++) if(id && jobs[i].id == id && !jobs[i].done){
        jobs[i].cancel = 1; signal_worker(); break;
    }
    pthread_mutex_unlock(&mu);
}
int proc_wait(unsigned id, char *out, int cap){
    if(!id || cap<1) return PROC_FAILED;
    pthread_mutex_lock(&mu);
    for(;;){
        struct job *j=NULL;
        for(int i=0;i<PROC_SLOTS;i++) if(jobs[i].id==id && !jobs[i].consumed) j=&jobs[i];
        if(!j){pthread_mutex_unlock(&mu);out[0]=0;return PROC_FAILED;}
        if(j->done) break;
        pthread_cond_wait(&completed,&mu);
    }
    /* Only this caller owns the handle. Poll performs the existing consume. */
    pthread_mutex_unlock(&mu);
    return proc_poll(id,out,cap);
}
