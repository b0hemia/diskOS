/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "file_observer.h"
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/inotify.h>
#include <time.h>
#include <unistd.h>
static int fd=-1, wd=-1, dirty=1;
static const char *parent, *const *targets;
static size_t target_count;
static uint64_t retry;
static uint64_t observer_now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static void watch_parent(void){
    if(fd<0) fd=inotify_init1(IN_NONBLOCK|IN_CLOEXEC);
    if(fd>=0) wd=inotify_add_watch(fd,parent,IN_CLOSE_WRITE|IN_MOVED_TO|IN_MOVED_FROM|
        IN_CREATE|IN_DELETE|IN_ATTRIB|IN_DELETE_SELF|IN_MOVE_SELF);
    retry=observer_now()+5000;
    dirty=1; /* Every reattachment requires a snapshot after subscription. */
}
int file_observer_start(const char *directory,const char *const *names,size_t count){
    parent=directory; targets=names; target_count=count;
    watch_parent(); return wd>=0 ? 0 : -1;
}
int file_observer_fd(void){return fd;}
void file_observer_events(const void *buffer,size_t size){
    const unsigned char *p=buffer;
    while(size){
        struct inotify_event e;
        if(size<sizeof e){dirty=1;break;}
        memcpy(&e,p,sizeof e);
        if(e.len>size-sizeof e){dirty=1;break;}
        if(e.mask&IN_Q_OVERFLOW) dirty=1;
        if(e.mask&(IN_Q_OVERFLOW|IN_IGNORED|IN_DELETE_SELF|IN_MOVE_SELF)){
            /* Close the whole instance: pending IN_IGNORED cannot invalidate a
             * newly recycled watch descriptor on the replacement directory. */
            if(fd>=0) close(fd);
            fd=wd=-1; retry=0; dirty=1; return;
        }
        if(e.len && memchr(p+sizeof e,0,e.len))
            for(size_t i=0;i<target_count;i++)
                if(!strcmp((const char *)p+sizeof e,targets[i])) dirty=1;
        p+=sizeof e+e.len; size-=sizeof e+e.len;
    }
}
int file_observer_take(void){
    if(parent && wd<0 && observer_now()>=retry) watch_parent();
    union { uint64_t align; char bytes[8192]; } b;
    if(fd>=0) for(int batch=0;batch<16;batch++){
        ssize_t n=read(fd,b.bytes,sizeof b.bytes);
        if(n>0){file_observer_events(b.bytes,(size_t)n);if(fd<0)break;}
        else if(n<0 && errno==EINTR) continue;
        else {
            if(n==0 || (errno!=EAGAIN && errno!=EWOULDBLOCK)){
                close(fd);fd=wd=-1;retry=0;dirty=1;
            }
            break;
        }
    }
    /* Overflow/read failure can close the instance during this drain. Restore
     * the subscription before the caller takes its replacement snapshot. */
    if(parent && wd<0 && observer_now()>=retry)watch_parent();
    /* Every failed attachment requests an authoritative snapshot. The next
     * retry does so again after 5 s, preserving the old fallback cadence. */
    int result=dirty; dirty=0; return result;
}
