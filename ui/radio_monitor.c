/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "radio_monitor.h"
#include "ui_wake.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
/* Reuse the installed monitors. Their text is an invalidation, never evidence
 * of Powered, Connected or PCM availability. Queries still verify those facts.
 * Names/service-owner signals cover replacement; EOF/read errors force fallback. */
struct monitor { pid_t pid;int fd,ready;uint64_t retry; };
static atomic_uint revision;
static atomic_int ready,enabled=1;
static int started,control[2]={-1,-1};
static atomic_uint reload_requested,reload_stopped;
static unsigned reload_serial,reload_token;
static uint64_t start_retry;
static void poke(void){char b=0;if(control[1]>=0)while(write(control[1],&b,1)<0 && errno==EINTR){}}
static void drain(void){char b[64];if(control[0]>=0)while(read(control[0],b,sizeof b)>0){}}
static void changed(void){atomic_fetch_add(&revision,1);ui_notify(UI_WAKE_BT);}
static uint64_t now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static void retire(struct monitor *m){
    if(m->fd>=0)close(m->fd);
    m->fd=-1;m->ready=0;
    if(m->pid>0){
        pid_t p=waitpid(m->pid,NULL,WNOHANG);
        if(p==m->pid || (p<0 && errno==ECHILD))m->pid=0;
        else if(!p || errno==EINTR)kill(m->pid,SIGKILL);
    }
    m->retry=now_ms()+30000;changed();
}
static void launch(struct monitor *m,int bluealsa){
    if(m->pid>0){
        pid_t p=waitpid(m->pid,NULL,WNOHANG);
        if(p==m->pid || (p<0 && errno==ECHILD))m->pid=0;
        else return;
    }
    struct sigaction child;
    if(sigaction(SIGCHLD,NULL,&child) || child.sa_handler!=SIG_DFL ||
       (child.sa_flags&SA_NOCLDWAIT))return;
    int f[2];if(pipe2(f,O_NONBLOCK|O_CLOEXEC))return;
    pid_t parent=getpid(),pid=fork();
    if(!pid){
        if(prctl(PR_SET_PDEATHSIG,SIGTERM) || getppid()!=parent)_exit(127);
        close(f[0]);if(dup2(f[1],1)<0)_exit(127);close(f[1]);
        int flags=fcntl(1,F_GETFL);if(flags<0 || fcntl(1,F_SETFL,flags&~O_NONBLOCK))_exit(127);
        int nul=open("/dev/null",O_RDWR);if(nul<0 || dup2(nul,0)<0 || dup2(nul,2)<0)_exit(127);if(nul>2)close(nul);
        if(bluealsa)execlp("bluealsa-cli","bluealsa-cli","monitor",(char *)NULL);
        else execlp("dbus-monitor","dbus-monitor","--system",
            "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.bluez'",
            "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.ObjectManager'",
            "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties'",(char *)NULL);
        _exit(127);
    }
    close(f[1]);if(pid<0){close(f[0]);return;}
    m->pid=pid;m->fd=f[0];
}
static void *run(void *unused){
    (void)unused;struct monitor m[2]={{.fd=-1},{.fd=-1}};int was_reloading=0;
    for(;;){
        uint64_t now=now_ms(),next=now+3000;
        unsigned requested=atomic_load(&reload_requested);
        if(requested || !atomic_load(&enabled)){
            was_reloading=1;int live=0;
            for(int i=0;i<2;i++){
                if(m[i].fd>=0)retire(&m[i]);
                if(m[i].pid>0){
                    pid_t p=waitpid(m[i].pid,NULL,WNOHANG);
                    if(p==m[i].pid || (p<0 && errno==ECHILD))m[i].pid=0;
                }
                live|=m[i].pid>0;
            }
            atomic_store(&ready,0);
            if(requested && !live && atomic_load(&reload_requested)==requested && atomic_load(&reload_stopped)!=requested){
                atomic_store(&reload_stopped,requested);changed();
            }
            struct pollfd stop={control[0],POLLIN,0};poll(&stop,1,live ? 10 : -1);drain();
            continue; /* exec cannot inherit persistent helpers with the same parent PID */
        }
        unsigned stopped=atomic_exchange(&reload_stopped,0);
        if(was_reloading || stopped){
            was_reloading=0;for(int i=0;i<2;i++)m[i].retry=0;
            changed();
        }
        for(int i=0;i<2;i++){
            if(m[i].fd>=0 && !m[i].ready && now>=m[i].retry)retire(&m[i]);
            if(m[i].fd<0 && now>=m[i].retry){launch(&m[i],i);m[i].retry=now+(m[i].fd>=0 ? 3000 : 30000);}
            if(m[i].fd<0 && m[i].retry<next)next=m[i].retry;
        }
        struct pollfd p[3]={{m[0].fd,POLLIN,0},{m[1].fd,POLLIN,0},{control[0],POLLIN,0}};
        int both=m[0].ready && m[1].ready;
        int r=poll(p,3,both ? -1 : (int)(next-now));
        if(p[2].revents)drain();
        if(r<0 && errno==EINTR)continue;
        if(r<0){for(int i=0;i<2;i++)retire(&m[i]);}
        for(int i=0;i<2;i++)if(p[i].revents){
            int dirty=0;
            for(int batch=0;batch<16;batch++){
                char b[4096];ssize_t n=read(m[i].fd,b,sizeof b);
                if(n>0){dirty=1;m[i].ready=1;continue;}
                if(!n || (errno!=EAGAIN && errno!=EINTR))retire(&m[i]);
                break;
            }
            if(dirty)changed();
        }
        atomic_store(&ready,m[0].ready && m[1].ready);
        if(!both)changed(); /* bounded 3-s fallback if a helper is unavailable */
    }
    return NULL;
}
int radio_monitor_start(void){
    if(started)return 0;
    uint64_t now=now_ms();if(now<start_retry)return -1;
    start_retry=now+30000;
    if(pipe2(control,O_CLOEXEC|O_NONBLOCK))return -1;
    pthread_t t;if(pthread_create(&t,NULL,run,NULL)){
        close(control[0]);close(control[1]);control[0]=control[1]=-1;return -1;
    }
    pthread_detach(t);started=1;return 0;
}
unsigned radio_monitor_revision(void){return atomic_load(&revision);}
int radio_monitor_ready(void){return atomic_load(&ready);}
int radio_monitor_prepare_reload(void){
    if(!started)return 1;
    if(!reload_token){if(!++reload_serial)++reload_serial;reload_token=reload_serial;}
    atomic_store(&reload_requested,reload_token);poke();return atomic_load(&reload_stopped)==reload_token;
}
void radio_monitor_reload_cancel(void){reload_token=0;atomic_store(&reload_requested,0);poke();}
int radio_monitor_quiescing(void){return atomic_load(&reload_requested);}
void radio_monitor_set_active(int active){if(atomic_exchange(&enabled,!!active)!=!!active)poke();}
