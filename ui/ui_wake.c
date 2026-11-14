/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ui_wake.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
static int wake_pipe[2]={-1,-1};
static atomic_uint pending;
int ui_wake_init(void){
    return wake_pipe[0]>=0 ? 0 : pipe2(wake_pipe,O_CLOEXEC|O_NONBLOCK);
}
void ui_wake_signal(unsigned topics){
    atomic_fetch_or_explicit(&pending,topics,memory_order_release);
    char b=0;
    if(wake_pipe[1]>=0)
        while(write(wake_pipe[1],&b,1)<0 && errno==EINTR){}
}
unsigned ui_wake_take(void){
    char b[256];
    if(wake_pipe[0]>=0) while(read(wake_pipe[0],b,sizeof b)>0){}
    return atomic_exchange_explicit(&pending,0,memory_order_acquire);
}
uint64_t ui_wake_now(void){
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
uint32_t ui_deadline_min(uint32_t wait,uint32_t now,uint32_t at){
    int32_t left=(int32_t)(at-now);
    uint32_t ms=left>0 ? (uint32_t)left : 0;
    return ms<wait ? ms : wait;
}
void ui_wake_wait(uint64_t deadline,const int *inputs,unsigned count){
    struct pollfd fds[8]={{.fd=wake_pipe[0],.events=POLLIN}};
    unsigned n=1;
    for(unsigned i=0;i<count && n<8;i++) if(inputs[i]>=0)
        fds[n++]=(struct pollfd){.fd=inputs[i],.events=POLLIN};
    for(;;){
        /* pending covers signals before pipe initialization and saturation. */
        if(atomic_load_explicit(&pending,memory_order_acquire)) return;
        uint64_t now=ui_wake_now();
        int timeout=deadline<=now ? 0 : deadline-now>INT_MAX ? INT_MAX : (int)(deadline-now);
        int r=poll(fds,n,timeout);
        if(r>=0 || errno!=EINTR) return;
    }
}
