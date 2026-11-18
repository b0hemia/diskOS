/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_ipc.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#define MAGIC UINT32_C(0x41505331)
static int rights(unsigned type){ return type == APS_GO || type == APS_NEXT_FILE || type == APS_ART; }
int aps_ipc_send(int sock, uint32_t attempt, aps_message *b, int fd){
    if(!attempt || b->type < APS_TARGET || b->type > APS_STREAM_READY || rights(b->type) != (fd >= 0)) return -1;
    b->magic = MAGIC; b->attempt = attempt;
    struct iovec v = { b, sizeof *b }; struct msghdr m = { .msg_iov = &v, .msg_iovlen = 1 };
    union { struct cmsghdr h; uint8_t bytes[CMSG_SPACE(sizeof(int))]; } control;
    if(fd >= 0){
        memset(&control, 0, sizeof control); m.msg_control = control.bytes; m.msg_controllen = sizeof control.bytes;
        struct cmsghdr *c = CMSG_FIRSTHDR(&m); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof fd); memcpy(CMSG_DATA(c), &fd, sizeof fd);
    }
    ssize_t n; do { n = sendmsg(sock, &m, MSG_NOSIGNAL | MSG_DONTWAIT); } while(n < 0 && errno == EINTR);
    return n == sizeof *b ? 0 : -1;
}
int aps_ipc_receive_reason(int sock, uint32_t attempt, aps_message *b, int *fd, const char **reason){
    *reason = "IPC invalid message";
    union { struct cmsghdr h; uint8_t bytes[CMSG_SPACE(16 * sizeof(int))]; } control;
    struct iovec v = { b, sizeof *b }; struct msghdr m = { .msg_iov = &v, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = sizeof control.bytes };
    *fd = -1;
    ssize_t n; do { n = recvmsg(sock, &m, MSG_DONTWAIT | MSG_CMSG_CLOEXEC); } while(n < 0 && errno == EINTR);
    if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    if(n <= 0){ *reason = n == 0 ? "IPC EOF" : "IPC receive failed"; return -1; }
    int count = 0, valid = !(m.msg_flags & (MSG_TRUNC | MSG_CTRUNC));
    for(struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)){
        if(c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS || c->cmsg_len < CMSG_LEN(0)){ valid = 0; continue; }
        size_t k = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for(size_t i = 0; i < k; i++){
            int f; memcpy(&f, (uint8_t *)CMSG_DATA(c) + i * sizeof f, sizeof f);
            if(count++ == 0) *fd = f; else close(f);
        }
    }
    if(n != sizeof *b || b->magic != MAGIC || b->type < APS_TARGET || b->type > APS_STREAM_READY || count != rights(b->type)) valid = 0;
    if(count == 1){ struct stat st; if(fstat(*fd, &st) || !S_ISREG(st.st_mode)) valid = 0; }
    if(!valid || b->attempt != attempt){ if(*fd >= 0) close(*fd); *fd = -1; return valid ? 0 : -1; }
    b->pin[8] = 0; b->target.id[127] = 0; b->target.name[63] = 0; b->target.addr[15] = 0;
    b->view.title[PB_TEXT_MAX - 1] = 0; b->view.artist[159] = 0; b->view.album[159] = 0;
    b->view.path[sizeof b->view.path - 1] = 0; b->view.dest[47] = 0;
    return 1;
}
int aps_ipc_receive(int sock, uint32_t attempt, aps_message *b, int *fd){
    const char *reason; return aps_ipc_receive_reason(sock, attempt, b, fd, &reason);
}
static int dispatch_rejected(void){ fprintf(stderr, "AirPlay worker end reason=dispatch rejected\n"); return 1; }
int aps_worker_dispatch(int argc, char **argv, int *status){
    const char *name = argc > 0 && argv[0] ? strrchr(argv[0], '/') : NULL;
    name = name ? name + 1 : argc > 0 && argv[0] ? argv[0] : "";
    int worker = !strcmp(name, APS_WORKER_NAME);
    for(int i = 1; i < argc; i++) if(!strcmp(argv[i], APS_WORKER_MARKER)) worker = 1;
    if(!worker) return 0;
    *status = 125;
    if(argc != 6 || strcmp(name, APS_WORKER_NAME) || strcmp(argv[1], APS_WORKER_MARKER) ||
       strcmp(argv[2], "--ctl") || strcmp(argv[3], "3") || strcmp(argv[4], "--attempt")) return dispatch_rejected();
    uint32_t attempt = 0; const char *p = argv[5];
    if(!p || !*p) return dispatch_rejected();
    for(; *p; p++){
        if(*p < '0' || *p > '9' || attempt > (UINT32_MAX - (unsigned)(*p - '0')) / 10) return dispatch_rejected();
        attempt = attempt * 10 + (unsigned)(*p - '0');
    }
    int type = 0; socklen_t n = sizeof type;
    if(!attempt || getsockopt(3, SOL_SOCKET, SO_TYPE, &type, &n) || type != SOCK_SEQPACKET) return dispatch_rejected();
    *status = aps_worker_main(3, attempt); return 1;
}
