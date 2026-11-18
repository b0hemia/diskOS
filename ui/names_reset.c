/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ui_wake.h"
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "names_reset.h"
#include "btalias.h"
#include "names.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>
extern int sys_reset_settings(void);
static const char intent_defaults[]="v1\nstage=defaults\n";
static const char intent_settings[]="v1\nstage=settings\n";
static _Thread_local int on_worker;
static _Thread_local int removal_pending;
/* Why the last nr_recover returned pending: at the aliases stage (with the
 * radio off that is the only wait) or waiting for the UI settings ack. */
static _Thread_local int at_aliases, awaiting_ack;
static int settings_step(void);
static void settings_begin(void);
static const char intent_aliases[]="v1\nstage=aliases\n";
int nr_recover(int radio_on, int create){
    at_aliases=awaiting_ack=0;
    /* A new reset settles an uncertain removal with its own durable intent. */
    if(removal_pending){
        if(!create){
            if(dn_remove(DN_INTENT)!=DN_OK) return 1;
            removal_pending=0; return 0;
        }
    }
    char b[64]; size_t n; int r=dn_read_file(DN_INTENT,b,sizeof b,&n);
    if(r==1 && !create) return 0;
    if(create){
        /* A new reset restarts at defaults even over a pending stage: the
         * user may have changed settings since the earlier attempt. */
        if(on_worker) settings_begin();
        r=dn_replace(DN_INTENT,intent_defaults,sizeof intent_defaults-1);
        if(r!=DN_FAILED) removal_pending=0;
        /* An uncertain intent may exist: changing anything waits for a later
         * successful durable replacement, and new names remain blocked. */
        if(r) return r==DN_UNCERTAIN ? 1 : -1;
        strcpy(b,intent_defaults);
    } else if(r || (strcmp(b,intent_defaults) && strcmp(b,intent_settings) && strcmp(b,intent_aliases))) return 1;
    at_aliases=!strcmp(b,intent_aliases);
    if(!radio_on && !strcmp(b,intent_aliases)) return 1;
    /* Recommit even a recovered record before proceeding. A process restart
     * alone cannot prove the directory fsync of the previous writer succeeded. */
    if(dn_replace(DN_INTENT,b,strlen(b))) return 1;
    if(!strcmp(b,intent_defaults)){
        if(dn_save_network("") || dn_save_bt(DN_DEFAULT_BT)) return 1;
        if(dn_replace(DN_INTENT,intent_settings,sizeof intent_settings-1)) return 1;
        strcpy(b,intent_settings);
    }
    if(!strcmp(b,intent_settings)){
        if(settings_step()) return 1;
        if(dn_replace(DN_INTENT,intent_aliases,sizeof intent_aliases-1)) return 1;
        at_aliases=1;
    }
    if(!radio_on) return 1;
    char actual[DN_BT_BYTES+1];
    if(btalias_get(NULL,actual,sizeof actual,800)!=BTALIAS_OK ||
       (strcmp(actual,DN_DEFAULT_BT) && btalias_replay(DN_DEFAULT_BT,1200)!=BTALIAS_OK)) return 1;
    /* Re-enumeration is the remaining-work record. Repeating an empty Alias
     * is idempotent; no nickname database or pairing-store writes are needed. */
    char before[64],after[64];
    if(btalias_owner(before,800)!=BTALIAS_OK) return 1;
    char (*macs)[18]=calloc(4096,18); size_t count=0;
    if(!macs) return 1;
    r=btalias_paired(macs,4096,&count,12000)==BTALIAS_OK ? 0 : 1;
    for(size_t i=0; !r && i<count; i++)
        if(btalias_set(macs[i],"",1200)!=BTALIAS_OK) r=1;
    free(macs);
    if(r || btalias_owner(after,800)!=BTALIAS_OK || strcmp(before,after)) return 1;
    /* Readback confirms live properties only. BlueZ owns durable pairing
     * storage; storage survival under sudden power loss needs a Disc check. */
    int removed=dn_remove(DN_INTENT);
    removal_pending=removed==DN_UNCERTAIN;
    return removed==DN_OK ? 0 : 1;
}
static struct {
    pthread_mutex_t mu;
    int wake[2];
    int started, radio, hint, queued, reset_queued, resetting;
    uint64_t serial, id, reset_id, settings_serial;
    int settings_state, settings_result, completed_reset;
    nr_kind kind;
    char mac[18], value[DN_BT_BYTES+1];
    nr_snapshot view;
} owner={.mu=PTHREAD_MUTEX_INITIALIZER,.wake={-1,-1}};
static int64_t mono_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000; }
static void wake_owner(void){ if(owner.wake[1]>=0){ ssize_t n=write(owner.wake[1],"x",1); (void)n; } }
/* The configuration store is owned by the UI thread. The worker posts only
 * an immutable request after stage=settings is durable, then waits for its ack. */
static void settings_begin(void){
    pthread_mutex_lock(&owner.mu); owner.settings_state=0; owner.view.settings_request=0; pthread_mutex_unlock(&owner.mu);
}
static int settings_step(void){
    if(!on_worker) return sys_reset_settings();
    pthread_mutex_lock(&owner.mu);
    int result=1;
    if(owner.settings_state==2){
        result=owner.settings_result;
        if(result) owner.settings_state=0;
    } else if(!owner.settings_state && owner.settings_serial!=UINT64_MAX){
        owner.view.settings_request=++owner.settings_serial; owner.settings_state=1;
    }
    awaiting_ack=owner.settings_state==1;
    pthread_mutex_unlock(&owner.mu); ui_notify(UI_WAKE_NAMES); return result;
}
void nr_settings_complete(uint64_t request,int result){
    pthread_mutex_lock(&owner.mu);
    if(owner.settings_state==1 && request==owner.view.settings_request){
        owner.settings_state=2; owner.settings_result=result;
        owner.view.settings_request=0; owner.hint=1; wake_owner();
    }
    pthread_mutex_unlock(&owner.mu);
}
/* Filtered lifecycle/property notifications are hints. Readback decides whether
 * anything needs writing; self-generated Alias signals therefore settle quietly. */
typedef struct { pid_t pid; int fd; } watch;
static void watch_close(watch *w){
    if(w->fd>=0) close(w->fd);
    if(w->pid>0){ kill(w->pid,SIGKILL); while(waitpid(w->pid,NULL,0)<0 && errno==EINTR) {} }
    w->pid=0; w->fd=-1;
}
static void watch_open(watch *w){
    int p[2]; if(pipe2(p,O_CLOEXEC|O_NONBLOCK)) return;
    pid_t pid=fork();
    if(!pid){
        if(prctl(PR_SET_PDEATHSIG,SIGTERM) || getppid()==1) _exit(127);
        close(p[0]); if(dup2(p[1],1)<0) _exit(127); close(p[1]);
        int flags=fcntl(1,F_GETFL); if(flags<0 || fcntl(1,F_SETFL,flags&~O_NONBLOCK)) _exit(127);
        int nul=open("/dev/null",O_RDWR); if(nul<0 || dup2(nul,0)<0 || dup2(nul,2)<0) _exit(127); if(nul>2) close(nul);
        execlp("dbus-monitor","dbus-monitor","--system",
          "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.bluez'",
          "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.ObjectManager'",
          "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties',path='/org/bluez/hci0'",(char *)NULL);
        _exit(127);
    }
    close(p[1]); if(pid<0){ close(p[0]); return; } w->pid=pid; w->fd=p[0];
}
static int watch_hint(watch *w){
    if(w->fd<0) return 0;
    char b[4096]; int changed=0;
    /* Coalesce a bounded batch. Large event bursts never monopolize the owner. */
    for(int i=0;i<16;i++){
        ssize_t n=read(w->fd,b,sizeof b);
        if(n>0){ changed=1; continue; }
        if(!n || (errno!=EAGAIN && errno!=EINTR)){ watch_close(w); changed=1; }
        break;
    }
    return changed;
}
static int reconcile(int radio, const dn_snapshot *s){
    if(!radio) return 1;
    /* Invalid stored files are not repaired or silently applied. */
    if(!s->bluetooth_valid) return 1;
    char actual[DN_BT_BYTES+1];
    if(btalias_get(NULL,actual,sizeof actual,800)!=BTALIAS_OK) return 1;
    return strcmp(actual,s->bluetooth) && btalias_replay(s->bluetooth,1200)!=BTALIAS_OK;
}
static void *run(void *unused){
    (void)unused; on_worker=1;
    watch w={0,-1}; unsigned backoff=1; int64_t retry=0, monitor_retry=0;
    uint64_t reset_id=0; int reset_result=0; /* accepted reset not yet reported */
    for(;;){
        pthread_mutex_lock(&owner.mu);
        int radio=owner.radio, hint=owner.hint; owner.hint=0;
        int queued=owner.queued; uint64_t id=owner.id; nr_kind kind=owner.kind;
        char mac[18],value[DN_BT_BYTES+1]; strcpy(mac,owner.mac); strcpy(value,owner.value);
        if(queued) owner.queued=0;
        else if(owner.reset_queued){ queued=1; kind=NR_RESET; id=owner.reset_id; owner.reset_queued=0; owner.resetting=1; }
        pthread_mutex_unlock(&owner.mu);
        if(w.fd<0 && mono_ms()>=monitor_retry){ watch_open(&w); monitor_retry=mono_ms()+30000; hint=1; }
        hint|=watch_hint(&w);
        if(hint){ retry=0; backoff=1; }
        int result=0;
        if(queued){
            if(kind==NR_NETWORK) result=dn_save_network(value);
            else if(kind==NR_BLUETOOTH) result=dn_save_bt(value);
            else if(kind==NR_PAIRED) result=radio && btalias_set(mac,value,1500)==BTALIAS_OK ? 0 : -1;
            else { reset_id=id; reset_result=nr_recover(radio,1); } /* replaces an unreported reset */
            retry=0;
        }
        pthread_mutex_lock(&owner.mu);
        int need=!owner.view.ready || queued || hint || mono_ms()>=retry;
        pthread_mutex_unlock(&owner.mu);
        if(need){
        int pending=nr_recover(radio,0);
        /* A reset reports once only its own remaining work is left. It waits
         * while the UI owes the settings acknowledgement, but an early read or
         * recommit failure clears awaiting_ack with a settings request still
         * posted, so a pending result can be published before that ack.
         * poll_names handles the posted request before the result. */
        int settle=reset_id && (reset_result<0 || !pending || !awaiting_ack);
        if(settle && reset_result>=0) reset_result=pending!=0;
        dn_snapshot names; dn_read(&names);
        int bt_pending=reconcile(radio,&names);
        /* Publish one immutable operation result and fresh data together. */
        pthread_mutex_lock(&owner.mu);
        if(!queued && !pending && owner.completed_reset && owner.view.result>=0) owner.view.result=0;
        owner.view.names=names; owner.view.ready=1; owner.view.reset_pending=pending!=0;
        owner.view.reset_radio=pending && !radio && at_aliases;
        owner.view.bt_pending=bt_pending; owner.view.generation++;
        if(queued && kind!=NR_RESET){ owner.view.completed=id; owner.view.result=result; owner.completed_reset=0; }
        if(settle){ owner.view.completed=reset_id; owner.view.result=reset_result; owner.completed_reset=1; owner.resetting=0; reset_id=0; }
        owner.view.busy=owner.queued || owner.reset_queued;
        pthread_mutex_unlock(&owner.mu);
        ui_notify(UI_WAKE_NAMES);
        retry=mono_ms()+backoff*1000; if(backoff<30) backoff=backoff<16 ? backoff*2 : 30;
        }
        int64_t deadline=retry;
        if(w.fd<0 && monitor_retry<deadline) deadline=monitor_retry;
        int64_t left=deadline-mono_ms();
        struct pollfd fds[2]={{owner.wake[0],POLLIN,0},{w.fd,POLLIN,0}};
        poll(fds,2,left>0 ? (int)left : 0);
        char drain[64]; while(read(owner.wake[0],drain,sizeof drain)>0) {}

    }
    return NULL;
}
int nr_start(void){
    pthread_mutex_lock(&owner.mu);
    if(!owner.started){
        pthread_t thread; owner.hint=1;
        if(pipe2(owner.wake,O_CLOEXEC|O_NONBLOCK)){ pthread_mutex_unlock(&owner.mu); return -1; }
        if(pthread_create(&thread,NULL,run,NULL)){ close(owner.wake[0]); close(owner.wake[1]); owner.wake[0]=owner.wake[1]=-1; pthread_mutex_unlock(&owner.mu); return -1; }
        pthread_detach(thread); owner.started=1;
    }
    pthread_mutex_unlock(&owner.mu); return 0;
}
uint64_t nr_submit_at(nr_kind kind, const char *mac, const char *value, unsigned epoch){
    if(kind<NR_NETWORK || kind>NR_RESET || (kind!=NR_RESET && !value)) return 0;
    if(kind==NR_NETWORK && *value && names_check(NAMES_NETWORK,value,NULL)!=NAMES_OK) return 0;
    if((kind==NR_BLUETOOTH || (kind==NR_PAIRED && *value)) && names_check(NAMES_BT,value,NULL)!=NAMES_OK) return 0;
    if(kind==NR_PAIRED){
        if(!mac || strlen(mac)!=17) return 0;
        for(int i=0;i<17;i++) if(i%3==2 ? mac[i]!=':' : !((mac[i]>='0'&&mac[i]<='9')||(mac[i]>='A'&&mac[i]<='F')||(mac[i]>='a'&&mac[i]<='f'))) return 0;
    }
    if(nr_start()) return 0;
    pthread_mutex_lock(&owner.mu);
    uint64_t id=0;
    /* A reset is always queued: the worker restarts any pending or running
     * reset from the beginning. Edits wait for every reset and the epoch. */
    if(epoch==owner.view.reset_epoch && owner.view.ready && owner.serial!=UINT64_MAX &&
       (kind==NR_RESET || (!owner.reset_queued && !owner.resetting && !owner.view.reset_pending && !owner.view.busy))){
        id=++owner.serial; owner.view.busy=1;
        if(kind==NR_RESET){ owner.view.reset_epoch++; owner.reset_queued=1; owner.reset_id=id; }
        else { owner.queued=1; owner.id=id; owner.kind=kind; snprintf(owner.value,sizeof owner.value,"%s",value); snprintf(owner.mac,sizeof owner.mac,"%s",mac?mac:""); }
        wake_owner();
    }
    pthread_mutex_unlock(&owner.mu); return id;
}
uint64_t nr_submit(nr_kind kind,const char *mac,const char *value){
    nr_snapshot s; nr_read(&s); return nr_submit_at(kind,mac,value,s.reset_epoch);
}
void nr_read(nr_snapshot *s){ pthread_mutex_lock(&owner.mu); *s=owner.view; pthread_mutex_unlock(&owner.mu); }
void nr_hint(void){ pthread_mutex_lock(&owner.mu); owner.hint=1; wake_owner(); pthread_mutex_unlock(&owner.mu); }
void nr_radio(int on){
    pthread_mutex_lock(&owner.mu);
    if(owner.radio!=!!on){ owner.radio=!!on; owner.hint=1; wake_owner(); }
    pthread_mutex_unlock(&owner.mu);
}
