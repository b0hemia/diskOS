/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_AUDIO_STOCK_PROCESS_H
#define DISKOS_AUDIO_STOCK_PROCESS_H
#include "audio_stock_wire.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
/* stock_process_spawn failures. No pathname is ever executed: the only helper
 * is the one embedded in this program (audio_stock_image.S), and only a
 * private, verified copy of it runs. */
#define STOCK_SPAWN_FAILED (-1)     /* descriptors, signal disposition or fork: retryable */
#define STOCK_SPAWN_UNPREPARED (-2) /* the private helper copy could not be made or verified: retryable */
#define STOCK_SPAWN_NONE (-3)       /* this build carries no helper: no stock backend */
#define AUDIO_STOCK_IMAGE_MAX (256u * 1024u)
#define AUDIO_STOCK_IMAGE_MAGIC "DADEC001"
#ifndef AUDIO_STOCK_PRIVATE_DIR
#define AUDIO_STOCK_PRIVATE_DIR "/tmp"
#endif
#ifdef AUDIO_STOCK_EMBEDDED
#include "embedded_helper.h"
/* Generated from the finished helper (tools/embed_audiodec.py): the image in its
 * own read-only section, and 52 descriptor bytes: magic[8], length[4], zero[4],
 * SHA-256[32], then the length the assembler actually included[4]. */
extern const unsigned char diskos_audiodec_image[], diskos_audiodec_image_end[], diskos_audiodec_desc[];
/* Report an execution failure separately from the helper's backend refusal. */
static inline void stock_process_unavailable(int fd){
    unsigned char h[AS_HEADER] = {0};
    as_put32(h, AS_MAGIC); as_put32(h + 8, AS_EXEC_FAILED);
    size_t done = 0;
    while(done < sizeof h){
        ssize_t n = send(fd, h + done, sizeof h - done, MSG_NOSIGNAL);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0) break;
        done += (size_t)n;
    }
    _exit(127);
}
/* A read-only, close-on-exec descriptor of a private copy of the embedded helper:
 * created on AUDIO_STOCK_PRIVATE_DIR and unlinked at once, written with checked
 * writes, writer closed, then verified. -1 on any failure; nothing is left named. */
static inline int stock_helper_prepare(void){
    char path[] = AUDIO_STOCK_PRIVATE_DIR "/.diskos-audiodec-XXXXXX";
    return eh_prepare(diskos_audiodec_image, diskos_audiodec_image_end, diskos_audiodec_desc,
                      AUDIO_STOCK_IMAGE_MAGIC, AUDIO_STOCK_IMAGE_MAX, path);
}
#endif
/* The caller owns the returned child until waitpid confirms its reap. Negative
 * results are the STOCK_SPAWN_* reasons; no child exists then. The child keeps
 * control on fd 3, media on fd 4 and the verified copy on close-on-exec fd 5. */
static inline pid_t stock_process_spawn(int input, int *output){
#ifndef AUDIO_STOCK_EMBEDDED
    (void)input; (void)output; return STOCK_SPAWN_NONE;
#else
    int sockets[2]; struct rlimit files; struct sigaction child;
    if(sigaction(SIGCHLD,NULL,&child) || child.sa_handler!=SIG_DFL || (child.sa_flags&SA_NOCLDWAIT)) return STOCK_SPAWN_FAILED;
    if(getrlimit(RLIMIT_NOFILE,&files) || files.rlim_cur>1048576) return STOCK_SPAWN_FAILED;
    int image=stock_helper_prepare();
    if(image<0) return STOCK_SPAWN_UNPREPARED;
    if(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,sockets)){ close(image); return STOCK_SPAWN_FAILED; }
    int ctl=fcntl(sockets[1],F_DUPFD_CLOEXEC,10), media=fcntl(input,F_DUPFD_CLOEXEC,10), exe=fcntl(image,F_DUPFD_CLOEXEC,10);
    close(image);
    if(ctl<0 || media<0 || exe<0){
        if(ctl>=0) close(ctl);
        if(media>=0) close(media);
        if(exe>=0) close(exe);
        close(sockets[0]); close(sockets[1]); return STOCK_SPAWN_FAILED;
    }
    pid_t parent=getpid(), pid=fork();
    if(!pid){
        /* Only async-signal-safe operations between fork and exec. */
        if(prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent || dup2(ctl,3)!=3 || dup2(media,4)!=4 ||
           dup2(exe,5)!=5 || fcntl(5,F_SETFD,FD_CLOEXEC)) stock_process_unavailable(ctl);
        close(0); close(1); close(2);
#ifdef SYS_close_range
        if(syscall(SYS_close_range,6,~0u,0))
#endif
            for(unsigned fd=6;fd<files.rlim_cur;fd++) close((int)fd);
        sigset_t none; sigemptyset(&none); sigprocmask(SIG_SETMASK,&none,NULL);
        char *const args[]={(char *)"diskos-audiodec",NULL};
        char *const env[]={"PATH=/usr/bin:/bin",NULL};
        eh_exec(5,args,env);   /* retries ETXTBSY */
        stock_process_unavailable(3);
    }
    close(ctl); close(media); close(exe); close(sockets[1]);
    if(pid<0){ close(sockets[0]); return STOCK_SPAWN_FAILED; }
    *output=sockets[0]; return pid;
#endif
}
#endif
