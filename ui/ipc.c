/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "ipc.h"
#include "receiver.h"
#include "path_nosymlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <mqueue.h>
#include <sys/stat.h>
#include <time.h>
#include "jsmn.h"

static mqd_t g_rx=(mqd_t)-1, g_tx=(mqd_t)-1;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static track_state_t g_state;

/* ---- player-restart recovery ---------------------------
 * The stock player can unlink+recreate /ui and /player on restart, orphaning any
 * descriptor we still hold (reads/writes on the old queue object keep succeeding, so
 * failure alone can't detect it). We detect recreation by comparing the fstat()
 * identity (dev,ino) of our descriptor against a fresh probe open - verified stable on
 * this device even though /dev/mqueue isn't mounted. All recovery actions are gated
 * behind positive identity-change evidence, so they are INERT during normal operation.
 * Only the RX thread may replace/close g_rx; the health timer only requests. */
/* g_recov_mu guards the RX-side state shared between the RX thread and the main thread
 * (identity + signal flags): dev_t/ino_t can be 64-bit (torn on 32-bit MIPS) and volatile
 * is not synchronization, so a real lock with defined ordering is required. The TX-side
 * state (descriptor, identity, stale flag and send error) is guarded by g_tx_mu. g_rx itself is touched only by the
 * RX thread (+ single-threaded startup), so it too is unlocked. */
static pthread_mutex_t g_tx_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_recov_mu = PTHREAD_MUTEX_INITIALIZER;
static dev_t g_rx_dev = 0;  static ino_t g_rx_ino = 0;   /* [g_recov_mu] identity of /ui g_rx consumes */
static int g_rx_ready    = 0;   /* [g_recov_mu] 1 once g_rx is open + being consumed */
static int g_rx_reopen   = 0;   /* [g_recov_mu] health timer -> RX thread: /ui recreated, reopen it */
static int g_reconnected = 0;   /* [g_recov_mu] RX thread -> main thread: recovery done, run resync */
static unsigned g_rx_frames = 0; /* [g_recov_mu] count of /ui frames received FROM the player. /ui is the
                                  * player->UI queue (we O_RDONLY it), so any frame proves the player is up
                                  * and responding - a positive-readiness signal that does NOT depend on the
                                  * cold-boot /ui reopen firing (see ipc_rx_frames / main.c initial apply). */
static unsigned g_generation = 0; /* [g_recov_mu] bumps each time /ui is (re)attached to a new player queue
                                   * (rx_do_reopen) = a new player generation. Lets the v2.40 work-mode
                                   * one-shot re-arm after a player restart. See ipc_generation(). */
/* Complete A2 evidence, separate from presentation and position updates. */
static track_state_t g_a2_report;
static unsigned g_a2_serial, g_a2_playing_report;
#ifdef DISKOS_TEST_BT_PREPARE
static unsigned g_prepare_playing_frames;
#endif
static int g_a2_stopped, g_a2_candidate_unloaded;
/* Main-thread send evidence only; no command or player-state mutation. */
static unsigned g_resume_toggle, g_resume_after_report;
static int g_confirmed;         /* [g_recov_mu] accepted a2 state report this generation */
static uint64_t g_mode_change_sequence; /* [g_recov_mu] durable departures */
static uint64_t g_arm_next, g_arm_request, g_arm_ack, g_arm_sequence; /* [g_recov_mu] */
static uint64_t g_source_sequence; /* [g_recov_mu] accepted source reports this generation */
static uint64_t g_obs_id, g_adopt_id; /* [g_recov_mu] observation arm, and the one that adopted */
static unsigned g_tx_generation; /* [g_tx_mu] descriptor generation */
static int g_player_mode = -1;   /* [g_recov_mu] last a607 external-mode announced by the player (8=LOCALPLAYER).
                                  * -1 = none received THIS player generation. NOTE (RE-verified vs v2.57): a607 is
                                  * NOT a reliable *solicited* readiness oracle - a 0607 query's reply does not come
                                  * back on /ui (design trace: a separate socket transport); the player only announces a607 to /ui after a 0657 mode
                                  * CHANGE. Readiness is established by the a2 probe + settle (main.c); when an a607
                                  * does arrive here, mode==8 is a valid "LOCALPLAYER set" confirmation.
                                  * Reset to -1 on /ui reopen (new player generation). See ipc_player_mode(). */
static dev_t g_tx_dev = 0;  static ino_t g_tx_ino = 0;   /* [g_tx_mu] identity of /player g_tx sends to */
static int g_tx_stale    = 0;   /* [g_tx_mu] health -> sender: /player recreated, drop g_tx */
static int g_thread_started = 0;         /* idempotency for ipc_start (NOT g_rx, which recovery swaps) */

/* Record the (dev,ino) identity of an open queue descriptor. Returns 0 on success. */
static int mq_identity(mqd_t q, dev_t *dev, ino_t *ino){
    struct stat s;
    if(q==(mqd_t)-1 || fstat((int)q, &s) != 0) return -1;
    *dev = s.st_dev; *ino = s.st_ino; return 0;
}

/* ---- helpers ---- */
static track_state_t g_a2_candidate;
static char g_stock_title[IPC_TITLE_CAPACITY];
static int g_title_frozen, g_title_stock;
static uint64_t g_title_revision;
/* Strict unsigned raw identifiers, including zero; display pos_id is separate. */
static int raw_id(const char *js, const jsmntok_t *t, uint64_t *out){
    uint64_t n=0;
    if(t->type!=JSMN_PRIMITIVE || t->end==t->start) return 0;
    for(int i=t->start;i<t->end;i++){
        unsigned d=(unsigned)(js[i]-'0');
        if(d>9 || n>(UINT64_MAX-d)/10) return 0;
        n=n*10+d;
    }
    *out=n; return 1;
}
static int kind_bool(const char *js, const jsmntok_t *t, int *out){
    int n=t->end-t->start;
    if(t->type!=JSMN_PRIMITIVE) return 0;
    if(n==4 && !memcmp(js+t->start,"true",4)){ *out=1; return 1; }
    if(n==5 && !memcmp(js+t->start,"false",5)){ *out=0; return 1; }
    return 0;
}
static int title_pending(const char *path, const char *title){
    const char *base=strrchr(path,'/'); base=base ? base+1 : path;
    const char *ext=strrchr(base,'.');
    return path[0]=='/' && ext && (!strcasecmp(ext,".opus") ||
        !strcasecmp(ext,".oga") || !strcasecmp(ext,".ogg")) &&
        (!title[0] || !strcmp(base,title));
}
static int tok_eq(const char*js, jsmntok_t*t, const char*s){
    int n=t->end-t->start;
    return t->type==JSMN_STRING && (int)strlen(s)==n && strncmp(js+t->start,s,n)==0;
}
static int find_val(const char*js, jsmntok_t*tk, int ntok, const char*key){
    /* Match only OBJECT KEYS, not string values: in jsmn a key string has size!=0
     * (it owns its value), a plain value string has size==0 (same test jsmn itself
     * uses at parse time). Without this, a value like a song titled "seq"/"state"
     * could be mistaken for the key and return the wrong following token. */
    for(int i=0;i+1<ntok;i++) if(tk[i].size!=0 && tok_eq(js,&tk[i],key)) return i+1;
    return -1;
}
static void copy_tok(const char*js, jsmntok_t*t, char*dst, int dstsz){
    int n=t->end-t->start; if(n>=dstsz) n=dstsz-1;
    memcpy(dst, js+t->start, n); dst[n]=0;
}
static long tok_long(const char*js, jsmntok_t*t){
    char b[32]; copy_tok(js,t,b,sizeof b); return strtol(b,0,10);
}
static int tok_bool(const char*js, jsmntok_t*t){
    char b[8]; copy_tok(js,t,b,sizeof b); return (b[0]=='t'||b[0]=='1'||b[0]=='T');
}
/* JSON string unescape (handles \" \\ \/ \n \t \r and \uXXXX -> UTF-8 BMP).
 * Returns the decoded length on success, or -1 if the input did not fit dst
 * (truncated). A silently-truncated path can alias a DIFFERENT real file, so
 * callers that key off the result (e.g. song_file_path) must drop a -1. */
static int unescape(const char*s,int n,char*dst,int dstsz){
    int o=0, i=0;
    for(;i<n && o<dstsz-4;i++){
        if(s[i]!=2 && s[i]==92 && i+1<n){ /* backslash */
            char c=s[++i];
            if(c==110) dst[o++]=10; else if(c==116) dst[o++]=9; else if(c==114) dst[o++]=13;
            else if(c==117 && i+4<n){
                char h[5]={s[i+1],s[i+2],s[i+3],s[i+4],0}; i+=4;
                unsigned cp=strtoul(h,0,16);
                if(cp<0x80) dst[o++]=cp;
                else if(cp<0x800){ dst[o++]=0xC0|(cp>>6); dst[o++]=0x80|(cp&0x3F); }
                else { dst[o++]=0xE0|(cp>>12); dst[o++]=0x80|((cp>>6)&0x3F); dst[o++]=0x80|(cp&0x3F); }
            } else dst[o++]=c; /* \" \\ \/ and others -> literal */
        } else dst[o++]=s[i];
    }
    dst[o]=0;
    return (i<n) ? -1 : o;   /* i<n -> stopped on the o limit, input left over = truncated */
}

/* Zero the current-track fields. Caller must hold g_mu. */
static void clear_track(void){
    if(g_state.player_confirmed) g_state.playback_token++;
    g_state.player_confirmed=0; g_state.provisional=0; g_state.title_ready=0;
    g_state.song_id_valid=g_state.pos_id_valid=0;
    g_state.is_cue_valid=g_state.is_sacd_valid=0;
    g_state.song_id_raw=g_state.pos_id_raw=0;
    g_title_frozen=g_title_stock=0; g_stock_title[0]=0;
    g_state.have_track=0;
    g_state.title[0]=0; g_state.artist[0]=0; g_state.album[0]=0; g_state.path[0]=0;
    g_state.duration_ms=0; g_state.position_ms=0; g_state.pos_id=0;
    g_state.sample_rate=0; g_state.is_dsd=0;
}

/* Stock snprintf metadata can be RAW text or transport-escaped strings.
 * Verified mq_player format VAs: V2.09 0x64f3bc/0x650200/0x65057c/0x66006c,
 * V2.28 0x6bb2b4, V2.40 0x6ccfc8, V2.57 0x6d7c78 (call 0x4d87b4).
 * Normalize before JSON decoding; refuse duplicate boundaries rather than guess. */
static int stock_a2_space(char c){ return c==' ' || c=='\t' || c=='\r' || c=='\n'; }
/* Whitespace between fields is present in the older firmware formats. */
static int stock_a2_match(const char *src, int len, int at, const char *pattern){
    for(int i=0;pattern[i];i++){
        if(at>=len || src[at++]!=pattern[i]) return -1;
        if(pattern[i]==',') while(at<len && stock_a2_space(src[at])) at++;
    }
    return at;
}
static int stock_a2_scalar(const char *src, int n, int boolean){
    if(boolean) return (n==4 && !memcmp(src,"true",4)) || (n==5 && !memcmp(src,"false",5));
    int i=(n>0 && src[0]=='-') ? 1 : 0;
    if(i==n || (src[i]=='0' && n-i>1)) return 0;
    for(;i<n;i++) if(src[i]<'0' || src[i]>'9') return 0;
    return 1;
}
/* jsmn tokenizes primitives without validating their JSON type. Validate every
 * suffix field here, including the closing brace and exact end of input. */
static int stock_a2_suffix(const char *src, int len, int at){
    static const char *keys[]={"love","playerflag","state","playing_num","work_mode"};
    for(int k=0;k<5;k++){
        char head[32]; snprintf(head,sizeof head,",\"%s\":",keys[k]);
        at=stock_a2_match(src,len,at,head);
        if(at<0) return 0;
        int start=at;
        if(k>=3){
            if(at>=len || src[at++]!='"') return 0;
            while(at<len && src[at]!='"'){
                if((unsigned char)src[at]<32 || src[at]=='\\') return 0;
                at++;
            }
            if(at>=len) return 0;
            at++;
        } else {
            while(at<len && src[at]!=',' && src[at]!='}') at++;
            if(!stock_a2_scalar(src+start,at-start,k==0)) return 0;
        }
    }
    return at<len && src[at]=='}' && at+1==len;
}
static int stock_a2_append(char *out, int *used, int cap, const char *s, int n){
    if(n<0 || n>=cap-*used) return 0;
    memcpy(out+*used,s,n); *used+=n; out[*used]=0; return 1;
}
/* The leading path slash distinguishes raw, one-layer and two-layer paths. */
static int stock_a2_depth(const char *s, int n){
    int i=0;
    while(i<n && s[i]=='\\') i++;
    if(i==0) return 0;   /* raw path bytes stay unchanged */
    if(i>=n || s[i]!='/') return -1;
    return i==1 ? 1 : i==3 ? 2 : -1;
}
/* Decode only stock's slash, quote and backslash escapes, then protect the real
 * bytes for the two JSON decoders. Unknown escapes are never interpreted. */
static int stock_a2_string(const char *s, int n, int depth, int path,
                           char *out, int *used, int cap){
    int unit=1<<depth, bytes=0;
    for(int i=0;i<n;){
        unsigned char c=(unsigned char)s[i++];
        int slashes=0;
        if(c=='\\'){
            slashes=1;
            while(i<n && s[i]=='\\'){ slashes++; i++; }
            int tail=slashes%unit;
            if(depth && i<n && (s[i]=='/' || s[i]=='"') && tail==unit-1){
                slashes/=unit; c=(unsigned char)s[i++];
            } else {
                if(depth && tail && path) return 0;
                if(!tail) slashes/=unit;
                c=0;   /* leave the following byte for the next iteration */
            }
        }
        bytes+=slashes+(c!=0);
        if(path==1 && bytes>(int)sizeof g_state.path-4) return 0;
        while(slashes--){
            if(!stock_a2_append(out,used,cap,"\\\\\\\\",4)) return 0;
        }
        if(c=='"'){
            if(!stock_a2_append(out,used,cap,"\\\\\\\"",4)) return 0;
        } else if(c && c<32){
            char esc[8]; int en=snprintf(esc,sizeof esc,"\\\\u%04x",c);
            if(!stock_a2_append(out,used,cap,esc,en)) return 0;
        } else if(c && !stock_a2_append(out,used,cap,(const char *)&c,1)) return 0;
    }
    return 1;
}
static int stock_a2_normalize(const char *src, int len, char *out, int cap){
    static const char *keys[]={"id","pos_id","song_name","song_duration_time",
        "song_file_path","song_sample_rate","song_encoding_rate","song_bit_rate",
        "song_artist_name","song_album_name","song_style_name","song_track",
        "song_channel","is_sacd","is_cue","is_dsd","is_m3u","m3u_file_path"};
    /* 0 = integer, 1 = raw string, 2 = boolean. */
    static const unsigned char types[]={0,0,1,0,1,0,0,0,1,1,1,0,0,2,2,2,2,1};
    const char *prefix="{\"song\":\"{";
    int used=0, at=(int)strlen(prefix), starts[18], ends[18], count=0, depth=0;
    if(len<at || memcmp(src,prefix,at)) return 0;
    for(int k=0;k<18;k++){
        char head[64], boundary[64];
        snprintf(head,sizeof head,"%s\\\"%s\\\":%s",k ? "," : "",keys[k],types[k]==1 ? "\\\"" : "");
        int next=stock_a2_match(src,len,at,head);
        if(next<0) return 0;
        at=next;
        /* V2.09 ends immediately after is_dsd; newer layouts carry m3u. */
        int last=(k==17 || (k==15 &&
            (stock_a2_match(src,len,at,"true}\"")>=0 ||
             stock_a2_match(src,len,at,"false}\"")>=0)));
        int bn;
        if(last) bn=snprintf(boundary,sizeof boundary,"%s}\"",types[k]==1 ? "\\\"" : "");
        else bn=snprintf(boundary,sizeof boundary,"%s,\\\"%s\\\":",types[k]==1 ? "\\\"" : "",keys[k+1]);
        int end=-1;
        for(int i=at;i+bn<=len;i++){
            if(stock_a2_match(src,len,i,boundary)>=0){
                if(end>=0) return 0;   /* ambiguous raw data: refuse to invent an identity */
                end=i;
            }
        }
        if(end<0) return 0;
        if(types[k]==1){
            starts[count]=at; ends[count++]=end;
            if(k==4){
                depth=stock_a2_depth(src+at,end-at);
                if(depth<0) return 0;
            }
            at=end+2;
        } else {
            int n=end-at;
            if(!stock_a2_scalar(src+at,n,types[k]==2)) return 0;
            at=end;
        }
        if(last) break;
    }
    if(stock_a2_match(src,len,at,"}\"")<0 || !stock_a2_suffix(src,len,at+2)) return 0;
    int copied=0;
    for(int k=0;k<count;k++){
        if(!stock_a2_append(out,&used,cap,src+copied,starts[k]-copied) ||
           !stock_a2_string(src+starts[k],ends[k]-starts[k],k==1 ? depth : 0,k==1,
                            out,&used,cap)) return 0;
        copied=ends[k];
    }
    if(!stock_a2_append(out,&used,cap,src+copied,len-copied)) return 0;
    return used;
}

/* Receiver publication is serialized with source/generation evidence, never g_mu. */
static ipc_receiver_meta g_receiver_meta;
static receiver_art_key g_receiver_art;
static uint64_t g_receiver_art_revision;
static char g_receiver_ids[2][32];
static uint64_t g_receiver_fields[3];
static uint64_t g_receiver_arm, g_receiver_session;
static unsigned g_receiver_generation;
static int g_receiver_selected;
/* Local publication has one owner, under g_recov_mu, with no new lock order.
 * RESERVED blocks entry frames but rolls back on cancellation before dispatch.
 * BLOCKED covers sent entry, selection and recovery; RETURN_ARM/READY cover exit.
 * No a1/a2 reaches local publication in these states, including old-queue frames.
 * Fresh Local after the return boundary, an observed AirPlay->Local departure,
 * unsent cancellation, or a new generation opens publication again. READY survives
 * cancellation/retry: stock suppresses duplicate a607 (MP 0x4f1174-0x4f119c), so
 * neither already observed departure nor late Local may require another report.
 * Outside these sessions the local parsers, including parse_a2, are unchanged. */
static struct {
    enum { LOCAL_OPEN, LOCAL_RESERVED, LOCAL_BLOCKED,
           LOCAL_RETURN_ARM, LOCAL_RETURN_READY } state;
    uint64_t return_arm;
} g_local_publication;
static int receiver_owned_locked(void){
    return g_receiver_arm && !g_rx_reopen && g_receiver_generation==g_generation &&
        g_arm_ack==g_receiver_arm;
}
static void receiver_clear_locked(void){
    g_receiver_arm=0; g_receiver_selected=0;
    memset(&g_receiver_meta,0,sizeof g_receiver_meta);
    memset(g_receiver_ids,0,sizeof g_receiver_ids); memset(g_receiver_fields,0,sizeof g_receiver_fields);
    g_receiver_meta.generation=g_generation;
}
void ipc_receiver_begin(unsigned generation, uint64_t arm){
    pthread_mutex_lock(&g_recov_mu);
    receiver_clear_locked();
    g_receiver_meta.generation=generation;
    g_receiver_meta.session_revision=++g_receiver_session;
    g_receiver_generation=generation; g_receiver_arm=arm;
    if(g_local_publication.state==LOCAL_OPEN)
        g_local_publication.state=g_player_mode==10 ? LOCAL_BLOCKED : LOCAL_RESERVED;
    pthread_mutex_unlock(&g_recov_mu);
}
void ipc_receiver_end(void){
    pthread_mutex_lock(&g_recov_mu);
    receiver_clear_locked();
    if(!g_rx_reopen && g_local_publication.state==LOCAL_RESERVED)
        g_local_publication.state=LOCAL_OPEN;
    /* An unacknowledged return cannot supply freshness after cancellation. */
    if(g_local_publication.state==LOCAL_RETURN_ARM){
        g_local_publication.state=LOCAL_BLOCKED;
        g_local_publication.return_arm=0;
    }
    pthread_mutex_unlock(&g_recov_mu);
}
void ipc_receiver_return(unsigned generation, uint64_t arm){
    pthread_mutex_lock(&g_recov_mu);
    receiver_clear_locked();
    if(generation==g_generation && !g_rx_reopen){
        if(g_local_publication.state==LOCAL_RESERVED)
            g_local_publication.state=LOCAL_OPEN;
        if(g_local_publication.state==LOCAL_BLOCKED ||
           g_local_publication.state==LOCAL_RETURN_ARM){
            g_local_publication.state=LOCAL_RETURN_ARM;
            g_local_publication.return_arm=arm;
        }
    }
    pthread_mutex_unlock(&g_recov_mu);
}
void ipc_receiver_metadata(ipc_receiver_meta *out){
    pthread_mutex_lock(&g_recov_mu);
    if(g_rx_reopen || g_receiver_meta.generation!=g_generation){
        memset(&g_receiver_meta,0,sizeof g_receiver_meta);
        memset(g_receiver_ids,0,sizeof g_receiver_ids); memset(g_receiver_fields,0,sizeof g_receiver_fields);
        g_receiver_meta.generation=g_generation; g_receiver_arm=0;
    }
    *out=g_receiver_meta;
    pthread_mutex_unlock(&g_recov_mu);
}
/* Strict JSON string decoder. Validate all input, even after the bounded prefix
 * fills. Never split a codepoint; reject invalid UTF-8, NUL and lone surrogates. */
/* Receipt-order art key shares the locked metadata snapshot, not file provenance. */
void receiver_art_get(receiver_art_key *out){
    pthread_mutex_lock(&g_recov_mu);
    ipc_receiver_meta *m=&g_receiver_meta;
    *out=g_receiver_art;
    if(g_rx_reopen || m->generation!=g_generation || out->generation!=m->generation ||
       out->session!=m->session_revision || out->item!=m->item_revision)
        *out=(receiver_art_key){g_generation,m->session_revision,m->item_revision,0,0};
    if(g_rx_reopen || m->generation!=g_generation) out->session=out->item=0;
    pthread_mutex_unlock(&g_recov_mu);
}

static uint64_t receiver_now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000+(uint64_t)ts.tv_nsec/1000000;
}
static int receiver_decode(const char *s,int n,char *out,int cap,uint64_t *identity){
    int used=0, full=0;
    uint64_t hash=UINT64_C(14695981039346656037);
    for(int i=0;i<n;){
        unsigned cp=(unsigned char)s[i++], min=0; int more=0;
        if(cp=='\\'){
            if(i==n) return -1;
            cp=(unsigned char)s[i++];
            if(cp=='u'){
                cp=0;
                for(int k=0;k<4;k++){
                    if(i==n) return -1;
                    unsigned c=(unsigned char)s[i++], d=c>='0'&&c<='9' ? c-'0' :
                        c>='a'&&c<='f' ? c-'a'+10 : c>='A'&&c<='F' ? c-'A'+10 : 16;
                    if(d==16) return -1;
                    cp=cp*16+d;
                }
                /* Supplementary characters may arrive as raw UTF-8. Refuse
                 * surrogate escapes rather than publish an invalid scalar. */
                if(cp>=0xd800 && cp<=0xdfff) return -1;
            } else if(cp=='n') cp='\n'; else if(cp=='r') cp='\r';
            else if(cp=='t') cp='\t'; else if(cp=='b') cp='\b'; else if(cp=='f') cp='\f';
            else if(cp!='"' && cp!='/' && cp!='\\') return -1;
        } else {
            if(cp<32 || cp=='"') return -1;
            if(cp>=0xc2 && cp<=0xdf){ cp&=31; more=1; min=0x80; }
            else if(cp>=0xe0 && cp<=0xef){ cp&=15; more=2; min=0x800; }
            else if(cp>=0xf0 && cp<=0xf4){ cp&=7; more=3; min=0x10000; }
            else if(cp>=0x80) return -1;
            for(int k=0;k<more;k++){
                if(i==n || ((unsigned char)s[i]&0xc0)!=0x80) return -1;
                cp=(cp<<6)|((unsigned char)s[i++]&63);
            }
            if(cp<min || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff)) return -1;
        }
        if(!cp) return -1;
        char bytes[4]; int count=0;
        if(cp<0x80) bytes[count++]=cp;
        else {
            if(cp<0x800) bytes[count++]=0xc0|(cp>>6);
            else {
                if(cp<0x10000) bytes[count++]=0xe0|(cp>>12);
                else { bytes[count++]=0xf0|(cp>>18); bytes[count++]=0x80|((cp>>12)&63); }
                bytes[count++]=0x80|((cp>>6)&63);
            }
            bytes[count++]=0x80|(cp&63);
        }
        for(int k=0;k<count;k++)
            hash=(hash^(unsigned char)bytes[k])*UINT64_C(1099511628211);
        if(used+count>=cap) full=1;
        if(!full){ memcpy(out+used,bytes,count); used+=count; }
    }
    if(identity) *identity=hash;
    out[used]=0; return full ? -2 : used;
}
static int receiver_text(const char *s,int n,char *out,int cap){
    return receiver_decode(s,n,out,cap,NULL);
}
/* jsmn alone accepts missing separators. Check the flat stock objects' exact
 * punctuation, duplicate keys and scalar types as well as token boundaries. */
static int receiver_object(const char *s,int n,jsmntok_t *t,int cap){
    jsmn_parser p; jsmn_init(&p); int nt=jsmn_parse(&p,s,n,t,cap), at=1;
    if(nt<1 || t[0].type!=JSMN_OBJECT || t[0].start!=0 || t[0].end!=n) return 0;
    for(int i=1;i<nt;i+=2){
        if(i+1>=nt || t[i].type!=JSMN_STRING || t[i].size!=1) return 0;
        while(at<n && stock_a2_space(s[at])) at++;
        if(i>1){ if(s[at++]!=',') return 0; while(at<n && stock_a2_space(s[at])) at++; }
        char key[1];
        if(at!=t[i].start-1 || memchr(s+t[i].start,'\\',t[i].end-t[i].start) ||
           receiver_text(s+t[i].start,t[i].end-t[i].start,key,1)==-1) return 0;
        for(int j=1;j<i;j+=2) if(t[j].end-t[j].start==t[i].end-t[i].start &&
            !memcmp(s+t[j].start,s+t[i].start,t[i].end-t[i].start)) return 0;
        at=t[i].end+1; while(at<n && stock_a2_space(s[at])) at++;
        if(s[at++]!=':') return 0;
        while(at<n && stock_a2_space(s[at])) at++;
        jsmntok_t *v=&t[i+1];
        if(v->type==JSMN_STRING){
            char check[1];
            if(at!=v->start-1 || receiver_text(s+v->start,v->end-v->start,check,1)==-1) return 0;
            at=v->end+1;
        } else if(v->type==JSMN_PRIMITIVE){
            if(at!=v->start || (!stock_a2_scalar(s+at,v->end-at,0) &&
                !stock_a2_scalar(s+at,v->end-at,1))) return 0;
            at=v->end;
        } else return 0;
    }
    while(at<n && stock_a2_space(s[at])) at++;
    return at==n-1 && s[at]=='}' ? nt : 0;
}
static int receiver_parse(const char *payload,int len){
    if(memchr(payload,0,len)) return 0;
    char repaired[8192], song[8192]; jsmntok_t tk[64], st[96];
    const char *head="{\"song\":\"{\\\"id\\\":";
    if(len>=(int)strlen(head) && !memcmp(payload,head,strlen(head))){
        int normalized=stock_a2_normalize(payload,len,repaired,sizeof repaired);
        /* Partial id-first JSON is not a complete stock snprintf layout. It
         * must still pass the strict object decoder below without repair. */
        if(normalized){ len=normalized; payload=repaired; }
    }
    int nt=receiver_object(payload,len,tk,64), v, n2=0;
    if(!nt) return 0;
    ipc_receiver_meta next=g_receiver_meta;
    v=find_val(payload,tk,nt,"state");
    if(v>=0){
        uint64_t state;
        if(!raw_id(payload,&tk[v],&state) || state>INT32_MAX) return 0;
        next.transport_state=(int)state; next.transport_valid=1;
    }
    v=find_val(payload,tk,nt,"song");
    if(v>=0){
        if(tk[v].type!=JSMN_STRING ||
           receiver_text(payload+tk[v].start,tk[v].end-tk[v].start,song,sizeof song)<0) return 0;
        if(song[0]){ n2=receiver_object(song,(int)strlen(song),st,96); if(!n2) return 0; }
        const char *keys[]={"song_name","song_artist_name","song_album_name"};
        char *fields[]={next.title,next.artist,next.album};
        int caps[]={sizeof next.title,sizeof next.artist,sizeof next.album};
        int changed=0;
        uint64_t hashes[3]; memcpy(hashes,g_receiver_fields,sizeof hashes);
        char ids[2][32]; memcpy(ids,g_receiver_ids,sizeof ids);
        const char *idkeys[]={"id","pos_id"};
        for(int k=0;k<2;k++){
            int sv=find_val(song,st,n2,idkeys[k]);
            if(sv<0) continue;
            int n=st[sv].end-st[sv].start;
            if(st[sv].type!=JSMN_PRIMITIVE || n>=32 ||
               !stock_a2_scalar(song+st[sv].start,n,0)) return 0;
            char id[32]; copy_tok(song,&st[sv],id,sizeof id);
            if(strcmp(ids[k],id)) changed=1;
            strcpy(ids[k],id);
        }
        for(int k=0;k<3;k++){
            int sv=find_val(song,st,n2,keys[k]);
            if(sv<0) continue;
            if(st[sv].type!=JSMN_STRING) return 0;
            char text[IPC_TITLE_CAPACITY];
            uint64_t hash;
            if(receiver_decode(song+st[sv].start,st[sv].end-st[sv].start,
                               text,caps[k],&hash)==-1) return 0;
            /* Hash decoded text, including the tail beyond the display prefix. */
            /* Stock sends the literal "(null)" for a missing field: empty. */
            if(!strcmp(text,"(null)")){ text[0]=0; hash=UINT64_C(14695981039346656037); }
            if(hashes[k]!=hash) changed=1;
            hashes[k]=hash; strcpy(fields[k],text);
        }
        uint64_t now=receiver_now_ms();
        if(changed){
            /* a103 carries no item id: position and duration belong to the old item. */
            next.item_revision++; next.item_rx_ms=now;
            next.progress_valid=0; next.position_ms=next.duration_ms=0;
        }
        int dv=find_val(song,st,n2,"song_duration_time"); uint64_t dur;
        if(dv>=0 && raw_id(song,&st[dv],&dur) && dur<=INT32_MAX){
            next.duration_ms=(long)dur; next.dur_rx_ms=now; next.dur_seq++;
        }
        memcpy(g_receiver_ids,ids,sizeof ids); memcpy(g_receiver_fields,hashes,sizeof hashes);
    }
    g_receiver_meta=next; return 1;
}

/* Source-bearing a202: strictly parsed, with work_mode exactly "AIRPLAY" (an A2 string is source evidence only here). */
static int receiver_airplay_frame(const char *payload,int len){
    char repaired[8192]; jsmntok_t tk[64]; int found=0, v;
    for(int i=0;i+7<=len;i++) if(!memcmp(payload+i,"AIRPLAY",7)) found=1;
    if(!found || memchr(payload,0,len)) return 0;
    const char *head="{\"song\":\"{\\\"id\\\":";
    if(len>=(int)strlen(head) && !memcmp(payload,head,strlen(head))){
        int normalized=stock_a2_normalize(payload,len,repaired,sizeof repaired);
        if(normalized){ len=normalized; payload=repaired; }
    }
    int nt=receiver_object(payload,len,tk,64);
    v=nt ? find_val(payload,tk,nt,"work_mode") : -1;
    return v>=0 && tok_eq(payload,&tk[v],"AIRPLAY");
}
/* Under g_recov_mu. Fresh evidence only: the observation boundary was acknowledged, nothing owns the receiver. */
static int adopt_ready_locked(void){
    return g_obs_id && g_obs_id==g_arm_ack && !g_receiver_arm && !g_rx_reopen;
}
/* The adopting frame is the one that identifies the source: ownership, selection and wire mode are published with
 * its metadata. A complete frame from the current player also confirms the generation (Return to Local needs it). */
static void adopt_locked(void){
    g_receiver_generation=g_generation; g_receiver_arm=g_adopt_id=g_obs_id;
    g_receiver_selected=1; g_confirmed=1;
    if(g_local_publication.state==LOCAL_OPEN || g_local_publication.state==LOCAL_RESERVED)
        g_local_publication.state=LOCAL_BLOCKED;
}
static void adopt_begin_locked(void){
    receiver_clear_locked();
    g_receiver_meta.generation=g_generation; g_receiver_meta.session_revision=++g_receiver_session;
}

static int parse_a2(const char*payload,int len){
    jsmn_parser p; jsmntok_t tk[64];
    static char repaired[8192], song[8192];
    jsmntok_t st[96]; int nt, n2=0, v;
    /* An id-first song object identifies the stock layout. A recognized but
     * malformed/ambiguous frame must never fall through to permissive JSON. */
    const char *stock_head="{\"song\":\"{\\\"id\\\":";
    int head_len=(int)strlen(stock_head);
    if(len>=head_len && !memcmp(payload,stock_head,head_len)){
        int normalized=stock_a2_normalize(payload,len,repaired,sizeof repaired);
        if(!normalized) return 0;
        payload=repaired; len=normalized;
    }
    jsmn_init(&p);
    nt=jsmn_parse(&p,payload,len,tk,64);
    v=nt>0 ? find_val(payload,tk,nt,"song") : -1;
    if(v>=0 && tk[v].type==JSMN_STRING && (tk[v].end-tk[v].start)>2){
        int decoded=unescape(payload+tk[v].start,tk[v].end-tk[v].start,song,sizeof song);
        jsmn_parser p2; jsmn_init(&p2);
        n2=decoded>=0 ? jsmn_parse(&p2,song,decoded,st,96) : -1;
    }
    if(nt<1 || n2<0) return 0;
    if(n2>0 && (st[0].type!=JSMN_OBJECT || st[0].start!=0 || st[0].end!=(int)strlen(song))) return 0;
    /* A truncated title cannot be used as eligibility evidence. Reject before publication. */
    if(n2>0){
        char title[sizeof g_state.title]; int sv=find_val(song,st,n2,"song_name");
        if(sv>=0 && unescape(song+st[sv].start,st[sv].end-st[sv].start,title,sizeof title)<0) return 0;
    }
    /* Confirmation requires a complete object and a numeric state, not arbitrary JSON.
     * Partial metadata still follows the existing publication rules below. */
    int state_val=-1, duplicate=0;
    for(int i=1;i+1<nt;){
        int value=i+1;
        if(tk[i].size==1 && tok_eq(payload,&tk[i],"state")){
            if(state_val>=0) duplicate=1;
            state_val=value;
        }
        /* Skip the entire value: a nested song state is not a player state. */
        int end=tk[value].end; i=value+1;
        while(i<nt && tk[i].start<end) i++;
    }
    int accepted=!duplicate && tk[0].type==JSMN_OBJECT && tk[0].start==0 && tk[0].end==len &&
        state_val>=0 && tk[state_val].type==JSMN_PRIMITIVE &&
        stock_a2_scalar(payload+tk[state_val].start,tk[state_val].end-tk[state_val].start,0);
    pthread_mutex_lock(&g_mu);
    char oldpath[TRACK_PATH_MAX]; snprintf(oldpath, sizeof oldpath, "%s", g_state.path);   /* detect a track-identity change */
    v=find_val(payload,tk,nt,"state");        if(v>=0) g_state.state=(int)tok_long(payload,&tk[v]);
    v=find_val(payload,tk,nt,"playing_num");  if(v>=0) copy_tok(payload,&tk[v],g_state.playing_num,sizeof g_state.playing_num);
    v=find_val(payload,tk,nt,"work_mode");    if(v>=0) copy_tok(payload,&tk[v],g_state.work_mode,sizeof g_state.work_mode);
    v=find_val(payload,tk,nt,"love");         if(v>=0) g_state.is_favorite=tok_bool(payload,&tk[v]);
    v=find_val(payload,tk,nt,"song");
    if(v>=0 && tk[v].type==JSMN_STRING && (tk[v].end-tk[v].start)>2){
        if(n2>0){
            /* Parse into TEMP state and publish only when the song carries a complete IDENTITY (a valid
             * path). song_name alone must NOT make it a "track": an omitted song_file_path would otherwise
             * leave the PREVIOUS path (and path_seq) intact while the title changes, so the new song's
             * positions would bind to the old track and corrupt its bookmark. unescape (not copy_tok) every
             * string: the inner song JSON still carries \/ \" \uXXXX after the outer unescape. */
            char t_title[sizeof g_state.title]={0}, t_artist[sizeof g_state.artist]={0};
            char t_album[sizeof g_state.album]={0}, t_path[sizeof g_state.path]={0};
            long t_dur=-1; int t_sr=-1, t_dsd=-1, have_path=0, sv;
            uint64_t song_id=0, pos_id=0; int sid_ok=0, pid_ok=0, cue=0, sacd=0, cue_ok=0, sacd_ok=0;
            sv=find_val(song,st,n2,"id"); if(sv>=0) sid_ok=raw_id(song,&st[sv],&song_id);
            sv=find_val(song,st,n2,"pos_id"); if(sv>=0) pid_ok=raw_id(song,&st[sv],&pos_id);
            sv=find_val(song,st,n2,"is_cue"); if(sv>=0) cue_ok=kind_bool(song,&st[sv],&cue);
            sv=find_val(song,st,n2,"is_sacd"); if(sv>=0) sacd_ok=kind_bool(song,&st[sv],&sacd);
            sv=find_val(song,st,n2,"song_name");          if(sv>=0) unescape(song+st[sv].start, st[sv].end-st[sv].start, t_title, sizeof t_title);
            sv=find_val(song,st,n2,"song_artist_name");   if(sv>=0) unescape(song+st[sv].start, st[sv].end-st[sv].start, t_artist, sizeof t_artist);
            sv=find_val(song,st,n2,"song_album_name");    if(sv>=0) unescape(song+st[sv].start, st[sv].end-st[sv].start, t_album,  sizeof t_album);
            sv=find_val(song,st,n2,"song_file_path");
            /* a truncated path can alias a DIFFERENT real file -> unescape overflow means NO usable identity */
            if(sv>=0){
                int decoded=unescape(song+st[sv].start, st[sv].end-st[sv].start, t_path, sizeof t_path);
                if(decoded<0) accepted=0;
                else if(t_path[0]) have_path=1;
            }
            sv=find_val(song,st,n2,"song_duration_time"); if(sv>=0) t_dur=tok_long(song,&st[sv]);
            sv=find_val(song,st,n2,"song_sample_rate");   if(sv>=0) t_sr=(int)tok_long(song,&st[sv]);
            sv=find_val(song,st,n2,"is_dsd");             if(sv>=0) t_dsd=tok_bool(song,&st[sv]);
            if(have_path){                 /* complete identity -> publish the whole track atomically */
                int changed=!g_state.player_confirmed || strcmp(g_state.path,t_path) ||
                    g_state.song_id_valid!=(unsigned)sid_ok || g_state.pos_id_valid!=(unsigned)pid_ok ||
                    g_state.song_id_raw!=song_id || g_state.pos_id_raw!=pos_id;
                if(changed){
                    g_state.playback_token++; g_title_frozen=g_title_stock=0;
                    /* Previous timing does not belong to this identity. An omitted
                     * duration stays unknown until this track reports its length. */
                    g_state.position_ms=0; g_state.pos_seq=0; g_state.duration_ms=0;
                }
                g_state.player_confirmed=1; g_state.provisional=0;
                g_state.song_id_raw=song_id; g_state.pos_id_raw=pos_id;
                g_state.song_id_valid=sid_ok; g_state.pos_id_valid=pid_ok;
                if(strcmp(g_stock_title,t_title) || g_state.is_cue!=cue || g_state.is_sacd!=sacd ||
                   g_state.is_cue_valid!=(unsigned)cue_ok || g_state.is_sacd_valid!=(unsigned)sacd_ok)
                    g_title_revision++;
                g_state.is_cue=cue; g_state.is_sacd=sacd;
                g_state.is_cue_valid=cue_ok; g_state.is_sacd_valid=sacd_ok;
                strcpy(g_stock_title,t_title);
                snprintf(g_state.path, sizeof g_state.path, "%s", t_path);
                if(!g_title_frozen){
                    strcpy(g_state.title,t_title);
                    if(!title_pending(t_path,t_title) || (cue_ok && cue) || (sacd_ok && sacd)) g_title_stock=1;
                    g_state.title_ready=g_title_stock;
                }
                snprintf(g_state.artist, sizeof g_state.artist, "%s", t_artist);
                snprintf(g_state.album,  sizeof g_state.album,  "%s", t_album);
                if(t_dur>=0) g_state.duration_ms=t_dur;
                if(t_sr>=0)  g_state.sample_rate=t_sr;
                if(t_dsd>=0) g_state.is_dsd=t_dsd;
                g_state.pos_id = (pid_ok && pos_id>0 && pos_id<=0x7FFFFFFF) ? (int)pos_id : 0;   /* per track: never carried over from the previous one */
                g_state.have_track=1;
            } else {
                /* a present song with NO usable path can't establish identity -> clear rather than bind new
                 * positions to the previous track (which would corrupt its bookmark). */
                clear_track();
            }
        }
    } else if(v>=0 && tk[v].type==JSMN_STRING){
        /* "song" present as an empty/short STRING ("{}"/"" -> len<=2): the player is
         * reporting no current track (playback stopped/ended) -> clear so the UI
         * doesn't keep showing the song that just ended.
         * Restricted to STRING tokens on purpose:
         *   - an ABSENT key (v<0) is a partial state update (love/work_mode change)
         *     and must NOT wipe the track (short frames like {"state":1} exist);
         *   - a bare-object "song":{...} (non-string) would be a VALID track in any
         *     encoder that doesn't stringify it, so we never clear on a non-string.
         * NB: this only handles the present-but-empty stop encoding. If the player
         * ever signals stop via a song-ABSENT short frame, that needs a captured
         * stop frame to confirm + a state-based oracle. */
        clear_track();
    }
    g_state.seq++;
    if(strcmp(oldpath, g_state.path) != 0) g_state.path_seq = g_state.seq;   /* track changed -> new identity epoch */
    int song_val=find_val(payload,tk,nt,"song");
    g_a2_candidate_unloaded = song_val>=0 && tk[song_val].type==JSMN_STRING &&
                             tk[song_val].end-tk[song_val].start<=2;
    g_a2_candidate = g_state;
    if(accepted) g_a2_candidate.state=(int)tok_long(payload,&tk[state_val]);
    /* Short state-only replies cannot establish that a session is loaded. */
    if(n2 <= 0) g_a2_candidate.have_track = 0;
    pthread_mutex_unlock(&g_mu);
    return accepted;
}

static int all_hex(const char*s,int n){
    if(n<=0) return 0;
    for(int i=0;i<n;i++){ char c=s[i];
        if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) return 0; }
    return 1;
}

/* the player's screen sleep/wake ("aa23000C0000" / "...0001", echo_sys_control.c V2.57) and power-off ("aa03"):
 * counters so the main loop sees every event (the latest value wins between its polls) */
static unsigned g_scr_seq, g_aa03_seq; static int g_scr_on = -1;
/* Called under g_recov_mu: frame state belongs only to this queue generation. */
static void screen_new_generation(void){
    g_confirmed = 0; g_a2_stopped = 0; g_a2_playing_report = 0;
    g_local_publication.state=LOCAL_OPEN; g_local_publication.return_arm=0;
    g_generation++; g_scr_on = -1; g_scr_seq = g_aa03_seq = 0;
}
/* The stock player's charging report, aa1f (V2.57 mq_player, adc_control.c loop_adc_check_thread; static RE of the
 * fixture binary). The player's charging flag (0x8990ec) is only ever stored as 0 or 1 (0x4f2390, 0x4f27ac, 0x4f27d4,
 * 0x4f2828, 0x4f286c). When it differs from the last sent value (0x8990e8) for 4 loop passes, it is sent with
 * 0x4ddaec(1, "aa1f", flag, NULL) (0x4f2458-0x4f246c), which formats "%s%04X%04X" with length 12 (0x4ddc54-0x4ddc84):
 * "aa1f000C0000" / "aa1f000C0001". While state byte 0x83a756 (read as the battery percentage) is 100 the send carries 0
 * instead (0x4f2450 -> 0x4f26d0), and one 0 is sent when it reaches 100 (0x4f2538-0x4f2558). Nothing else touches
 * 0x8990e8, so there is no resend on request: a UI restart has no report until the flag next changes. Only that exact
 * 12-character form is accepted; any other aa1f frame is counted and ignored and never changes the state. Layout pinned
 * by tests/release/test_parity_power.py; docs/COMMAND_MAP.md "aa1f". Live frames UNVERIFIED. [g_recov_mu] */
static int g_charge_state = -1; static uint64_t g_charge_at_ms; static unsigned g_charge_seq, g_charge_bad;
/* 0 or 1 for a well-formed report, -1 for anything else. */
static int charge_parse(const char *buf, int n){
    if(n != 12 || memcmp(buf, "aa1f000C000", 11)) return -1;
    return buf[11] == '0' ? 0 : buf[11] == '1' ? 1 : -1;
}
/* Called under g_recov_mu with the generation change: the new player's last-sent value starts over, so an older
 * report says nothing about it (it reports only a change from its own start-up value). */
static void charge_new_generation(void){ g_charge_state = -1; g_charge_at_ms = 0; }
/* diskos_boot.log line per received aa1f (raw bytes + decision), at most 8 lines a minute, the rest counted. RX only. */
static void charge_log(const char *buf, int n, int value, unsigned bad){
    static int have; static uint64_t window; static unsigned lines, dropped;
    uint64_t now = receiver_now_ms();
    if(!have || now - window >= 60000u){ have = 1; window = now; lines = 0; }
    if(lines >= 8){ dropped++; return; }
    lines++;
    char raw[17]; int k = n < 16 ? (n > 0 ? n : 0) : 16;
    for(int i = 0; i < k; i++){ unsigned char c = (unsigned char)buf[i]; raw[i] = (c >= 0x20 && c < 0x7f && c != '"') ? (char)c : '?'; }
    raw[k] = 0;
    if(value >= 0)
        fprintf(stderr, "charge: aa1f \"%s\" -> %s (stock report wins)", raw, value ? "charging" : "not charging");
    else
        fprintf(stderr, "charge: aa1f \"%s\" (%d bytes) malformed, ignored (%u so far)", raw, n, bad);
    if(dropped) fprintf(stderr, " [%u earlier lines not logged]", dropped);
    fputc('\n', stderr);
    dropped = 0;
}
static void charge_on_frame(const char *buf, int n){
    int v = charge_parse(buf, n);
    pthread_mutex_lock(&g_recov_mu);
    unsigned bad = g_charge_bad;
    if(v >= 0){ g_charge_state = v; g_charge_at_ms = receiver_now_ms(); g_charge_seq++; }
    else bad = ++g_charge_bad;
    pthread_mutex_unlock(&g_recov_mu);
    charge_log(buf, n, v, bad);
}
/* V2.57 MP@42a720 -> 4ddc94 emits a1030010%08X with NO item identity: the
 * position is bound to the item current at receipt (Disc capture 2026-10-16:
 * ms, ~1 Hz while playing, stops on pause, restarts at 0 on a skip). Exactly
 * 8 hex digits within int32; anything else is ignored. Under g_recov_mu. */
static void receiver_position(const char *buf,int n){
    if(n!=16 || memcmp(buf,"a1030010",8) || !all_hex(buf+8,8)) return;
    char h[9]; memcpy(h,buf+8,8); h[8]=0;
    unsigned long ms=strtoul(h,0,16);
    if(ms>INT32_MAX) return;
    g_receiver_meta.position_ms=(long)ms; g_receiver_meta.progress_valid=1;
    g_receiver_meta.pos_rx_ms=receiver_now_ms();
    g_receiver_meta.pos_item_revision=g_receiver_meta.item_revision; g_receiver_meta.pos_seq++;
}
static void conv_on_reply(const char*buf,int n);   /* still-image conversion (end of file) */
static void conv_new_generation(unsigned generation);
static void parse_frame(const char*buf,int n){
    if(n<4) return;
    if(!memcmp(buf,"aa05",4)){
        /* STOCK-PARITY (owner decision 55): bind by receipt, not verified item.
         * V2.40 converter 0x45b69c -> aa05=1 at 0x45b6fc-0x45b710;
         * V2.57 0x4620c4 -> 0x462124-0x462138. Neither checks the return.
         * V2.57 can return at 0x480bd4-0x480c08 before writer 0x480d54.
         * A late notice or failed conversion can show the previous cover for
         * the current item. The key only rejects jobs superseded after receipt. */
        pthread_mutex_lock(&g_recov_mu);
        if(receiver_owned_locked())
            g_receiver_art=(receiver_art_key){g_receiver_meta.generation,g_receiver_meta.session_revision,
                g_receiver_meta.item_revision,++g_receiver_art_revision,
                g_receiver_meta.item_revision && n==12 && !memcmp(buf,"aa05000C0001",12)};
        pthread_mutex_unlock(&g_recov_mu);
        return;
    }
    if(n<8) return;
    if(buf[0]=='a'&&buf[1]=='a'&&buf[2]=='2'&&buf[3]=='3' && n>=12){
        char h[5]={buf[8],buf[9],buf[10],buf[11],0};
        if(!all_hex(h,4)) return;
        pthread_mutex_lock(&g_recov_mu); g_scr_on=(int)strtol(h,0,16)!=0; g_scr_seq++; pthread_mutex_unlock(&g_recov_mu);
        return;
    }
    if(buf[0]=='a'&&buf[1]=='a'&&buf[2]=='0'&&buf[3]=='3'){
        pthread_mutex_lock(&g_recov_mu); g_aa03_seq++; pthread_mutex_unlock(&g_recov_mu);
        return;
    }
    if((buf[0]=='a' && buf[1]=='1') || (buf[0]=='a' && buf[1]=='2')){
        pthread_mutex_lock(&g_recov_mu);
        char fh[5]={buf[4],buf[5],buf[6],buf[7],0};
        if(!memcmp(buf,"a202",4) && !receiver_owned_locked() && all_hex(fh,4) &&
           (int)strtol(fh,0,16)==n && receiver_airplay_frame(buf+8,n-8)){
            /* Quarantine: AirPlay metadata never reaches the Local parser (no path, so it would clear the Local
             * track). Fresh evidence after the observation boundary adopts the source and publishes this frame. */
            if(adopt_ready_locked() && (g_player_mode<0 || g_player_mode==10)){   /* a wire report of another source conflicts */
                adopt_begin_locked();
                if(receiver_parse(buf+8,n-8)){
                    adopt_locked();
                    if(g_player_mode!=10){ g_player_mode=10; g_mode_change_sequence++; g_source_sequence++; }
                } else receiver_clear_locked();
            }
            /* The observation boundary gates ownership, not quarantine. Treat an
             * unconflicted AirPlay report like a607 mode 10 even before adoption. */
            if(!g_rx_reopen && (g_player_mode<0 || g_player_mode==10)){
                if(g_local_publication.state==LOCAL_OPEN || g_local_publication.state==LOCAL_RESERVED)
                    g_local_publication.state=LOCAL_BLOCKED;
                if(g_player_mode!=10){ g_player_mode=10; g_mode_change_sequence++; g_source_sequence++; }
            }
            if(!g_rx_reopen) g_confirmed=1;   /* a complete frame from the current player */
            pthread_mutex_unlock(&g_recov_mu); return;
        }
        if(g_local_publication.state!=LOCAL_OPEN){
            char lh[5]={buf[4],buf[5],buf[6],buf[7],0};
            if(receiver_owned_locked() && !memcmp(buf,"a202",4) && all_hex(lh,4) && (int)strtol(lh,0,16)==n)
                receiver_parse(buf+8,n-8);
            else if(receiver_owned_locked() && !memcmp(buf,"a103",4)) receiver_position(buf,n);
            /* No receiver frame may reach local position/track publication. */
            pthread_mutex_unlock(&g_recov_mu); return;
        }
        pthread_mutex_unlock(&g_recov_mu);
    }
    if(buf[0]=='a'&&buf[1]=='8'&&buf[2]=='1'&&buf[3]=='0'){ conv_on_reply(buf,n); return; }
    /* Every aa1f of 8 bytes or more lands here (shorter frames end at the n<8 guard above, like every report). */
    if(!memcmp(buf,"aa1f",4)){ charge_on_frame(buf,n); return; }
    if(buf[0]==97 && buf[1]==49){ /* "a1<type2><len4><payload>" position, e.g. a1030010 + 8-hex ms */
        if(n<8) return;
        char lh[5]={buf[4],buf[5],buf[6],buf[7],0};
        if(!all_hex(lh,4)) return;               /* no valid length field -> reject */
        int flen=(int)strtol(lh,0,16);           /* advertised TOTAL frame length (chars) */
        if(flen<9 || n<flen) return;             /* TRUNCATED (fewer chars than advertised) -> keep last position,
                                                  * never publish a short/bogus value that could corrupt a bookmark */
        int pn=flen-8; if(pn>15) return;         /* payload wider than a position value -> not a position frame */
        char h[16]={0}; memcpy(h,buf+8,pn);
        if(!all_hex(h,pn)) return;               /* malformed payload -> keep last position */
        errno=0; long ms=strtol(h,0,16);
        if(errno==ERANGE || ms<0) return;        /* overflow/negative -> reject, don't publish garbage */
        pthread_mutex_lock(&g_mu); g_state.position_ms=ms; g_state.seq++; g_state.pos_seq=g_state.seq; pthread_mutex_unlock(&g_mu);
    } else if(buf[0]==97 && buf[1]==50){ /* "a2" state/metadata JSON */
        char lh[5]={buf[4],buf[5],buf[6],buf[7],0};
        int complete=buf[2]=='0' && buf[3]=='2' && all_hex(lh,4) && (int)strtol(lh,0,16)==n;
        pthread_mutex_lock(&g_recov_mu);
        unsigned generation=g_generation; int pending=g_rx_reopen;
        pthread_mutex_unlock(&g_recov_mu);
        int accepted=parse_a2(buf+8, n-8);
        pthread_mutex_lock(&g_recov_mu);
        if(complete && accepted && !pending && !g_rx_reopen && generation==g_generation){
            g_confirmed=1;
            pthread_mutex_lock(&g_mu);
            /* An edge belongs to one loaded identity, even between UI ticks. */
            if(g_a2_candidate.playback_token != g_a2_report.playback_token)
                g_a2_playing_report=0;
            g_a2_report=g_a2_candidate;
            g_a2_serial++;
#ifdef DISKOS_TEST_BT_PREPARE
            if(g_a2_report.state == 0) g_prepare_playing_frames++;
#endif
            if(g_a2_stopped && g_a2_report.have_track && g_a2_report.state==0)
                g_a2_playing_report=g_a2_serial;
            /* A state-only playing report is not an unloaded session. */
            if(g_a2_candidate_unloaded || g_a2_report.state==2)
                g_a2_stopped=1;
            else if(g_a2_report.have_track)
                g_a2_stopped=0;
            pthread_mutex_unlock(&g_mu);
        }
        pthread_mutex_unlock(&g_recov_mu);
    } else if(buf[0]=='a'&&buf[1]=='7'&&buf[2]=='1'&&buf[3]=='4' && n>=12){
        /* "a714000C00<VV><pos16>" - VV (hex chars 10-11) = volume level */
        char h[3]={buf[10],buf[11],0};
        if(!all_hex(h,2)) return;    /* malformed -> keep last volume, don't reset to 0 */
        int v=(int)strtol(h,0,16);
        pthread_mutex_lock(&g_mu); g_state.volume=v; g_state.volume_seq++; pthread_mutex_unlock(&g_mu);
    } else if(buf[0]=='a'&&buf[1]=='6'&&buf[2]=='0'&&buf[3]=='7' && n==12 && !memcmp(buf+4,"000C",4)){
        /* "a607000C<MODE>" - the player's external/input mode, announced to /ui after a 0657 mode CHANGE.
         * (A 0607 query's reply does not arrive here (separate transport, unverified on the Disc), so we do NOT rely on soliciting it - see
         * g_player_mode.) MODE 0008 = LOCALPLAYER; when this arrives, ==8 confirms LOCALPLAYER is set. VALUE is
         * the 4 hex chars after the 000C length (buf[8..11]). */
        char h[5]={buf[8],buf[9],buf[10],buf[11],0};
        if(!all_hex(h,4)) return;
        int mode=(int)strtol(h,0,16);
        pthread_mutex_lock(&g_recov_mu);
        if(!g_rx_reopen){
            if(mode==10 && adopt_ready_locked()){ adopt_begin_locked(); adopt_locked(); }
            if(mode!=g_player_mode) g_mode_change_sequence++;
            if(mode==10 && (g_local_publication.state==LOCAL_OPEN ||
                g_local_publication.state==LOCAL_RESERVED))
                g_local_publication.state=LOCAL_BLOCKED;   /* wire AirPlay: no receiver frame is a Local frame */
            if(mode==8 && (g_local_publication.state==LOCAL_RETURN_READY ||
               (g_local_publication.state==LOCAL_BLOCKED && g_player_mode==10))){
                g_local_publication.state=LOCAL_OPEN;
                g_local_publication.return_arm=0;
            }
            if(receiver_owned_locked()){
                if(g_receiver_selected && mode!=10){
                    g_receiver_arm=0; memset(&g_receiver_meta,0,sizeof g_receiver_meta);
                    memset(g_receiver_ids,0,sizeof g_receiver_ids); memset(g_receiver_fields,0,sizeof g_receiver_fields);
                    g_receiver_meta.generation=g_generation;
                } else if(mode==10) g_receiver_selected=1;
            }
            g_player_mode=mode; g_source_sequence++;
        }
        pthread_mutex_unlock(&g_recov_mu);
    }
    /* NB: for playback the player emits a1/a2/a714 to /ui (a2=state/love/work_mode/track, a1=position,
     * a714=volume) - no a622/a639/a704 completion replies. SEPARATELY, a 0657 mode CHANGE makes it announce
     * a607 (external/input mode) to /ui, parsed above (a bare 0607 query is not answered on /ui). */
}

static char  *g_rxbuf = NULL;
static size_t g_rxbufsz = 0;

/* RX thread only: reattach g_rx to the CURRENT named /ui after the player recreated it.
 * Open-before-close so there's never a descriptor-less gap; no O_CREAT (the player owns
 * recreation - if it's momentarily absent we retry on the next wake). */
/* Returns 0 once g_rx holds a fresh valid descriptor, -1 if the reopen could not complete
 * (queue transiently absent / new msgsize won't fit) - g_rx is then UNCHANGED and still
 * invalid, so the caller MUST back off rather than immediately re-receiving (else 100% CPU). */
static int rx_do_reopen(void){
    pthread_mutex_lock(&g_recov_mu); g_rx_reopen = 1; pthread_mutex_unlock(&g_recov_mu);
    mqd_t nw = mq_open("/ui", O_RDONLY);
    if(nw==(mqd_t)-1){   /* transient ENOENT (player mid-recreate) -> retry next wake */
        pthread_mutex_lock(&g_recov_mu); g_rx_reopen = 1; pthread_mutex_unlock(&g_recov_mu); return -1;
    }
    struct mq_attr at;
    if(mq_getattr(nw,&at)==0 && at.mq_msgsize>0 && (size_t)at.mq_msgsize > g_rxbufsz){
        char *nb = malloc((size_t)at.mq_msgsize);
        if(nb){ free(g_rxbuf); g_rxbuf = nb; g_rxbufsz = (size_t)at.mq_msgsize; }
        else {   /* can't fit the new queue's msgsize: installing it would EMSGSIZE-loop and never
                  * drain. Keep the old descriptor, retry next wake. (Unreached on the 8192-byte
                  * queue vs the 8200 buffer floor, but must not silently install an unreadable fd.) */
            mq_close(nw);
            pthread_mutex_lock(&g_recov_mu); g_rx_reopen = 1; pthread_mutex_unlock(&g_recov_mu);
            return -1;
        }
    }
    mqd_t old = g_rx;
    g_rx = nw;                              /* g_rx is RX-thread-only; publish the fresh descriptor */
    dev_t d=0; ino_t io=0; mq_identity(nw, &d, &io);
    pthread_mutex_lock(&g_recov_mu);
    g_rx_reopen = 0; g_rx_dev = d; g_rx_ino = io; g_rx_ready = 1; g_reconnected = 1;
    g_player_mode = -1; g_source_sequence = g_mode_change_sequence = 0;
    g_arm_request = g_arm_ack = 0; g_obs_id = g_adopt_id = 0; screen_new_generation();
    charge_new_generation();
    unsigned generation=g_generation;
    pthread_mutex_unlock(&g_recov_mu);
    pthread_mutex_lock(&g_mu);
    int had_path=g_state.path[0]!=0;
    clear_track(); g_state.playback_token++; g_state.player_generation=generation; g_state.seq++;
    if(had_path) g_state.path_seq=g_state.seq;
    pthread_mutex_unlock(&g_mu);
    conv_new_generation(generation);   /* a conversion in flight died with the old player */
    if(old!=(mqd_t)-1 && old!=nw) mq_close(old);   /* open-before-close, but NEVER close a descriptor number
                                                    * the new mq_open reused (EBADF path: the old fd was dead,
                                                    * so its number can come back as nw - closing it kills the
                                                    * fresh queue and re-spins EBADF). */
    fprintf(stderr,"ipc: /ui reattached after player restart\n");
    return 0;
}

/* RX only, after the previous receive has finished publication. Drain through
 * the normal parser before publishing a boundary. No request ID exists in a607:
 * this gives receipt freshness, not command-response correlation. */
static void rx_arm_barrier(void){
    pthread_mutex_lock(&g_recov_mu);
    uint64_t id=g_arm_request;
    int pending=id && id!=g_arm_ack && !g_rx_reopen;
    pthread_mutex_unlock(&g_recov_mu);
    if(!pending) return;
    for(;;){
        pthread_mutex_lock(&g_recov_mu);
        int cancelled=g_rx_reopen || g_arm_request!=id;
        pthread_mutex_unlock(&g_recov_mu);
        if(cancelled) return;
        struct timespec ts={0,0};
        ssize_t n=mq_timedreceive(g_rx,g_rxbuf,g_rxbufsz,NULL,&ts);
        if(n>=0){
            pthread_mutex_lock(&g_recov_mu); g_rx_frames++; pthread_mutex_unlock(&g_recov_mu);
            parse_frame(g_rxbuf,(int)n);
            continue;
        }
        if(errno==EINTR) continue;
        if(errno!=ETIMEDOUT && errno!=EAGAIN) return;
        pthread_mutex_lock(&g_recov_mu);
        if(!g_rx_reopen && g_arm_request==id){
            g_arm_sequence=g_source_sequence; g_arm_ack=id;
            if(g_local_publication.state==LOCAL_RETURN_ARM &&
               g_local_publication.return_arm==id){
                g_local_publication.state=LOCAL_RETURN_READY;
                g_local_publication.return_arm=0;
            }
        }
        pthread_mutex_unlock(&g_recov_mu);
        return;
    }
}

static void *ipc_thread(void*arg){
    (void)arg;
    /* Wait (up to 1s) for a /ui frame - mq_timedreceive instead of a permanent block so a
     * player-restart recovery request can't be starved by silence OR steady traffic (checked
     * before every receive). ~1 idle wake/sec, no busy-loop. Commands go out directly from
     * ipc_send_cmd() on the caller thread, so no command ring to drain here. The buffer MUST
     * be >= the queue's mq_msgsize or receive fails EMSGSIZE without removing the message. */
    for(;;){
        pthread_mutex_lock(&g_recov_mu); int want = g_rx_reopen; pthread_mutex_unlock(&g_recov_mu);
        if(want) rx_do_reopen();
        rx_arm_barrier();
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 1;
        ssize_t n = mq_timedreceive(g_rx, g_rxbuf, g_rxbufsz, NULL, &ts);
        if(n>=0){ pthread_mutex_lock(&g_recov_mu); g_rx_frames++; pthread_mutex_unlock(&g_recov_mu); parse_frame(g_rxbuf,(int)n); }
        else if(errno==ETIMEDOUT) continue;   /* normal idle wake */
        else if(errno==EINTR) continue;
        else if(errno==EBADF){ if(rx_do_reopen()!=0) usleep(50000); }  /* lost fd -> reattach now (we're on the RX thread); if the queue is still absent, back off 50ms instead of busy-spinning on the dead fd */
        else usleep(50000);                   /* unexpected error: back off rather than spin */
    }
    return 0;
}

int ipc_start(void){
    if(g_thread_started) return 0;   /* idempotent: recovery swaps g_rx, so guard on the thread, not the fd */
    /* Prefer the player's /ui queue (so we inherit its exact attrs); but at cold
     * boot the player can be slow to create it, so create it ourselves if absent
     * - we're the receiver, the player just opens it O_WRONLY to send. */
    g_rx=mq_open("/ui", O_RDONLY);   /* blocking: thread sleeps until a frame arrives */
    if(g_rx==(mqd_t)-1){
        struct mq_attr ca; ca.mq_flags=0; ca.mq_maxmsg=10; ca.mq_msgsize=8192; ca.mq_curmsgs=0;
        g_rx=mq_open("/ui", O_RDONLY|O_CREAT, 0666, &ca);
    }
    if(g_rx==(mqd_t)-1){ fprintf(stderr,"mq_open /ui fail: %s\n",strerror(errno)); return -1; }
    struct mq_attr at;
    g_rxbufsz = (mq_getattr(g_rx,&at)==0 && at.mq_msgsize>0) ? (size_t)at.mq_msgsize : 8200;
    if(g_rxbufsz < 8200) g_rxbufsz = 8200;
    g_rxbuf = malloc(g_rxbufsz);
    if(!g_rxbuf){
        fprintf(stderr,"ipc rxbuf alloc %zu failed\n",g_rxbufsz);
        mq_close(g_rx); g_rx=(mqd_t)-1;        /* don't leak the open /ui queue */
        return -1;
    }
    pthread_mutex_lock(&g_tx_mu);
    g_tx=mq_open("/player", O_WRONLY|O_NONBLOCK);
    if(g_tx==(mqd_t)-1) fprintf(stderr,"mq_open /player fail: %s\n",strerror(errno));
    else mq_identity(g_tx, &g_tx_dev, &g_tx_ino);
    pthread_mutex_unlock(&g_tx_mu);
    mq_identity(g_rx, &g_rx_dev, &g_rx_ino);
    memset(&g_state,0,sizeof g_state);
    pthread_t t;
    if(pthread_create(&t,0,ipc_thread,0)!=0){   /* no rx thread -> tear down, don't half-init */
        fprintf(stderr,"ipc_thread create failed: %s\n",strerror(errno));
        mq_close(g_rx); g_rx=(mqd_t)-1;
        pthread_mutex_lock(&g_tx_mu);
        if(g_tx!=(mqd_t)-1){ mq_close(g_tx); g_tx=(mqd_t)-1; }
        pthread_mutex_unlock(&g_tx_mu);
        free(g_rxbuf); g_rxbuf=NULL;
        return -1;
    }
    pthread_detach(t);
    g_thread_started = 1;   /* main-thread-only (ipc_start caller) */
    pthread_mutex_lock(&g_recov_mu); g_rx_ready = 1; pthread_mutex_unlock(&g_recov_mu);
    return 0;
}
/* Health probe (main thread, ~30s): non-destructively compare the CURRENT named queues'
 * fstat identity to what g_rx/g_tx point at; a mismatch means the player recreated them.
 * We only REQUEST recovery here - the RX thread swaps g_rx; the next send swaps g_tx. */
void ipc_health_check(void){
    mqd_t p = mq_open("/ui", O_RDONLY);
    if(p!=(mqd_t)-1){
        struct stat s;
        if(fstat((int)p,&s)==0){
            pthread_mutex_lock(&g_recov_mu);
            /* (g_rx_dev||g_rx_ino): only compare when identity was actually captured - a failed
             * fstat at open leaves (0,0), and a real queue ino is nonzero, so this skips false
             * mismatches from a transient capture failure rather than forcing a spurious reopen. */
            if(g_rx_ready && (g_rx_dev||g_rx_ino) && (s.st_dev!=g_rx_dev || s.st_ino!=g_rx_ino)) g_rx_reopen = 1;
            pthread_mutex_unlock(&g_recov_mu);
        }
        mq_close(p);
    }
    pthread_mutex_lock(&g_tx_mu);
    mqd_t q = mq_open("/player", O_WRONLY|O_NONBLOCK);
    if(q!=(mqd_t)-1){
        struct stat s;
        if(g_tx!=(mqd_t)-1 && (g_tx_dev||g_tx_ino) && fstat((int)q,&s)==0 &&
           (s.st_dev!=g_tx_dev || s.st_ino!=g_tx_ino))
            g_tx_stale = 1;
        mq_close(q);
    }
    pthread_mutex_unlock(&g_tx_mu);
}
/* 1 (and clears) once after a recovery reattach - main thread resyncs UI-owned state. */
int ipc_take_reconnected(void){
    pthread_mutex_lock(&g_recov_mu); int r=g_reconnected; g_reconnected=0; pthread_mutex_unlock(&g_recov_mu);
    return r;
}
/* Number of /ui frames received from the player so far (>0 => the player is up and responding). Used by
 * the main loop to guarantee the initial audio apply even if the cold-boot /ui reopen never fired. */
unsigned ipc_rx_frames(void){
    pthread_mutex_lock(&g_recov_mu); unsigned r=g_rx_frames; pthread_mutex_unlock(&g_recov_mu);
    return r;
}
/* Last a607 external-mode reported by the player THIS generation: -1 = none yet (player not confirmed at
 * its command dispatcher), else the mode (8 = LOCALPLAYER). The v2.40 work-mode handshake oracle. */
int ipc_player_mode(void){
    pthread_mutex_lock(&g_recov_mu); int r=g_player_mode; pthread_mutex_unlock(&g_recov_mu);
    return r;
}
/* Player generation: bumps on each /ui reattach (player restart). The v2.40 work-mode one-shot uses this
 * to re-arm after a restart (send the init once per generation). */
void ipc_screen_snapshot(int *on, unsigned *scr_seq, unsigned *poweroff_seq, unsigned *generation){
    pthread_mutex_lock(&g_recov_mu);
    if(on) *on=g_scr_on; if(scr_seq) *scr_seq=g_scr_seq; if(poweroff_seq) *poweroff_seq=g_aa03_seq;
    if(generation) *generation=g_generation;
    pthread_mutex_unlock(&g_recov_mu);
}
int ipc_charge_snapshot(ipc_charge_evidence *out){
    ipc_charge_evidence e;
    pthread_mutex_lock(&g_recov_mu);
    e.state=g_charge_state; e.at_ms=g_charge_at_ms; e.seq=g_charge_seq; e.bad=g_charge_bad; e.generation=g_generation;
    pthread_mutex_unlock(&g_recov_mu);
    if(out) *out=e;
    return e.state;
}
void ipc_source_snapshot(ipc_source_evidence *out){
    pthread_mutex_lock(&g_recov_mu);
    out->generation=g_generation; out->sequence=g_source_sequence;
    out->mode_change_sequence=g_mode_change_sequence;
    out->arm_id=g_arm_ack; out->arm_sequence=g_arm_sequence; out->adopt_id=g_adopt_id;
    out->mode=g_player_mode; out->valid=g_rx_ready && !g_rx_reopen;
    pthread_mutex_unlock(&g_recov_mu);
}
uint64_t ipc_source_arm(unsigned generation){
    pthread_mutex_lock(&g_recov_mu);
    uint64_t id=0;
    if(g_rx_ready && !g_rx_reopen && g_confirmed && generation==g_generation){
        id=++g_arm_next; g_arm_request=id; g_arm_ack=0;
    }
    pthread_mutex_unlock(&g_recov_mu);
    return id;
}
uint64_t ipc_observe_arm(unsigned generation){
    pthread_mutex_lock(&g_recov_mu);
    uint64_t id=0;
    if(g_rx_ready && !g_rx_reopen && generation==g_generation && !g_arm_request && !g_receiver_arm){
        id=++g_arm_next; g_arm_request=g_obs_id=id; g_arm_ack=0;
    }
    pthread_mutex_unlock(&g_recov_mu);
    return id;
}
void ipc_source_cancel_arm(uint64_t id){
    pthread_mutex_lock(&g_recov_mu);
    if(id && g_arm_request==id) g_arm_request=g_arm_ack=0;
    pthread_mutex_unlock(&g_recov_mu);
}
unsigned ipc_generation(void){
    pthread_mutex_lock(&g_recov_mu); unsigned r=g_generation; pthread_mutex_unlock(&g_recov_mu);
    return r;
}
/* Read-only readiness evidence: complete numeric A2 from this queue generation.
 * A2 0=playing, 1=paused, 2=stopped (V2.57 0x4248c0..0x4249f0).
 * Do not substitute presentation state or a position frame for this report. */
int ipc_resume_snapshot(track_state_t *out, unsigned *serial, unsigned *toggle, unsigned *after_report){
    pthread_mutex_lock(&g_recov_mu);
    *out=g_a2_report; *serial=g_a2_serial;
    *toggle=g_resume_toggle; *after_report=g_resume_after_report;
    int valid=g_confirmed && !g_rx_reopen && out->player_generation==g_generation;
    pthread_mutex_unlock(&g_recov_mu);
    return valid;
}
/* Serial of the last complete stopped/unloaded -> loaded playing transition.
 * Kept at receipt so two replies between UI ticks cannot hide the edge. */
unsigned ipc_resume_playing_report(void){
    pthread_mutex_lock(&g_recov_mu);
    unsigned serial=g_a2_playing_report;
    pthread_mutex_unlock(&g_recov_mu);
    return serial;
}
void ipc_get_state(track_state_t*out){
    pthread_mutex_lock(&g_mu); *out=g_state; pthread_mutex_unlock(&g_mu);
}
void ipc_get_title_event(ipc_title_event *out){
    pthread_mutex_lock(&g_mu);
    out->state=g_state; strcpy(out->player_title,g_stock_title); out->evidence_revision=g_title_revision;
    pthread_mutex_unlock(&g_mu);
}
int ipc_publish_title(ipc_title_event *event, const char *title, int freeze){
    pthread_mutex_lock(&g_mu);
    int accepted=g_state.player_confirmed && g_state.playback_token==event->state.playback_token &&
        g_title_revision==event->evidence_revision;
    if(accepted){
        /* Change presentation only: retain newer position, volume and metadata. */
        snprintf(g_state.title,sizeof g_state.title,"%s",freeze ? title : g_stock_title);
        g_title_frozen=freeze; g_title_stock=!freeze; g_state.title_ready=1; g_state.seq++;
    } else {
        event->state=g_state; strcpy(event->player_title,g_stock_title);
        event->evidence_revision=g_title_revision;
    }
    pthread_mutex_unlock(&g_mu);
    return accepted;
}
/* Seed the track fields at startup (from the DB resume state) so the UI shows
 * the current song before the player sends its first a2 frame.  Guarded: a real
 * a2 frame (have_track already set) is never overwritten. */
void ipc_seed_state(const track_state_t *s){
    if(!s) return;
    pthread_mutex_lock(&g_mu);
    if(!g_state.have_track){
        snprintf(g_state.title,  sizeof g_state.title,  "%s", s->title);
        snprintf(g_state.artist, sizeof g_state.artist, "%s", s->artist);
        snprintf(g_state.album,  sizeof g_state.album,  "%s", s->album);
        snprintf(g_state.path,   sizeof g_state.path,   "%s", s->path);
        g_state.duration_ms = s->duration_ms;
        g_state.position_ms = s->position_ms;
        g_state.state       = s->state;
        g_state.is_favorite = s->is_favorite;
        g_state.have_track  = 1;
        g_state.provisional = 1; g_state.player_confirmed = 0; g_state.title_ready = 0;
        g_state.seq++;
    }
    pthread_mutex_unlock(&g_mu);
}
#ifdef DISKOS_HOST
void ipc_host_set_state(const track_state_t *s){
    if(!s) return;
    pthread_mutex_lock(&g_mu);
    g_state = *s;
    snprintf(g_stock_title, sizeof g_stock_title, "%s", s->title);
    g_title_frozen = 0; g_title_stock = s->title_ready; g_title_revision++;
    pthread_mutex_unlock(&g_mu);
}
#endif
/* Send to /player with bounded nonblocking retries, serialized by g_tx_mu. */
static int g_send_err = 0;   /* sticky until ipc_take_send_error() reads it */

enum { SEND_USER, SEND_PROBE, SEND_AUTOMATIC };
/* Called with g_tx_mu held (g_send_err and the once-per-boot flag are guarded by it). */
static void ipc_note_send_error(int policy){
    static int automatic_reported;
    if(policy == SEND_PROBE) return;
    if(policy == SEND_AUTOMATIC){
        if(automatic_reported) return;
        automatic_reported = 1;
        int fd = open("/tmp/.diskos-player-send-notice", O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC, 0600);
        if(fd >= 0) close(fd);
        else if(errno == EEXIST) return;
    }
    g_send_err = 1;
}
/* Probes stay silent (background state-sync / health probes and the still-image conversion requests must not raise
 * "Player didn't respond" during the startup connect race); automatic failures notify once per boot. User failures
 * always report. Caller holds g_tx_mu. */
static int ipc_send_wire_locked(const char*frame, int policy, unsigned generation){
    if(g_tx_generation != generation){ g_tx_stale = 1; g_tx_generation = generation; }
    if(g_tx_stale && g_tx!=(mqd_t)-1){ mq_close(g_tx); g_tx=(mqd_t)-1; }  /* health saw /player recreated */
    g_tx_stale = 0;
    if(g_tx==(mqd_t)-1){
        g_tx=mq_open("/player", O_WRONLY|O_NONBLOCK);  /* (re)open if absent/healed */
        g_tx_dev = 0; g_tx_ino = 0;   /* clear first: a failed capture must not leave a stale identity */
        if(g_tx!=(mqd_t)-1) mq_identity(g_tx, &g_tx_dev, &g_tx_ino);
    }
    if(g_tx==(mqd_t)-1){ ipc_note_send_error(policy); return -1; }
    size_t len = strlen(frame);
    /* g_tx is O_NONBLOCK so a full /player queue returns EAGAIN immediately. A user
     * action (play/seek/volume) shouldn't be silently dropped, so retry briefly
     * (~15ms max) to let the player drain; only EAGAIN is retried, other errors fail. */
    int err = 0;
    for(int a=0;a<5;a++){
        if(mq_send(g_tx, frame, len, 0) == 0) return 0;
        err = errno;                 /* capture before usleep, which may touch errno */
        if(err != EAGAIN) break;
        usleep(3000);
    }
    /* ANY failure -> drop the fd so the next send re-opens the CURRENT named /player. Covers a
     * non-EAGAIN error (player went away) AND EAGAIN-exhaustion (queue full 5x - an orphaned
     * queue after a restart looks exactly like this; H1). Reopening a merely transiently-full
     * healthy queue is harmless: same queue object, no messages lost. */
    if(g_tx!=(mqd_t)-1){ mq_close(g_tx); g_tx=(mqd_t)-1; }
    ipc_note_send_error(policy);
    if(policy != SEND_PROBE) fprintf(stderr,"ipc_send_cmd '%s' failed: %s\n", frame, strerror(err));
    return -1;
}
/* #28 experiment: stock owns EOF and queue policy. Only its gapless scalar is
 * overridden; TX serializes restoration with every route/source command.
 * An explicit diskOS preference is required so System default is never guessed. */
#ifndef BT_AUTONEXT_PATH
#define BT_AUTONEXT_PATH "/usr/data/diskos_bt_autonext_exp"
#endif
static struct { unsigned gen; int active, restore, bt_mux, departed; } g_bt_autonext; /* g_tx_mu */
static int bt_autonext_mode(void){
    int fd = open(BT_AUTONEXT_PATH, O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW);
    if(fd < 0) return 0;
    struct stat st; char buf[32];
    ssize_t n = -1;
    if(fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size < (off_t)sizeof buf)
        n = read(fd, buf, sizeof buf);
    close(fd);
    return (n == 10 && !memcmp(buf, "gapless-on", 10)) ||
           (n == 11 && !memcmp(buf, "gapless-on\n", 11));
}
static void bt_autonext_generation(unsigned gen){
    if(g_bt_autonext.gen != gen){
        g_bt_autonext.gen = gen;
        g_bt_autonext.active = 0; /* normal reconnect scalar restoration owns the new player */
        g_bt_autonext.bt_mux = g_bt_autonext.departed = 0;
    }
}
static int bt_autonext_restore(int policy, unsigned gen){
    if(!g_bt_autonext.active) return 0;
    const char *f = g_bt_autonext.restore ? "0647000C0001" : "0647000C0000";
    if(ipc_send_wire_locked(f, policy, gen) != 0) return -1;
    g_bt_autonext.active = 0;
    fprintf(stderr, "BT autonext experiment: restored gapless=%d\n", g_bt_autonext.restore);
    return 0;
}
static int ipc_send_locked(const char *frame, int policy, unsigned generation){
    bt_autonext_generation(generation);
    /* Restore before output mux, work/source mode or USB gadget changes, even
     * between polls. A failed restoration refuses the departure frame. */
    int departure = !strncmp(frame, "0666", 4) || !strncmp(frame, "0657", 4) || !strncmp(frame, "0642", 4);
    if(departure && bt_autonext_restore(policy, generation) != 0) return -1;
    if(g_bt_autonext.active &&
       (!strcmp(frame, "0647000C0000") || !strcmp(frame, "0647000C0001"))){
        g_bt_autonext.restore = frame[11] == '1';
        frame = "0647000C0001";
    }
    int rc = ipc_send_wire_locked(frame, policy, generation);
    if(rc == 0 && departure){
        /* The UI retains its old BT MAC across partial route failures. Once a
         * departure lands, require BT mux + Local work-mode before re-entry. */
        if(!strncmp(frame, "0666", 4))
            g_bt_autonext.bt_mux = !strcmp(frame, "0666000C0002");
        g_bt_autonext.departed = !g_bt_autonext.bt_mux || strcmp(frame, "0657000C0008") != 0;
    }
    return rc;
}
void ipc_bt_autonext_poll(uint32_t now, int eligible, int preference){
    static uint32_t last;
    static int polled, previous_eligible, pending;
    /* Main thread only. Route departures also restore synchronously above. */
    if(polled && eligible == previous_eligible && (uint32_t)(now - last) < 1000) return;
    polled = 1; last = now; previous_eligible = eligible;
    int enabled = bt_autonext_mode() && eligible && (preference == 0 || preference == 1);
    if(!enabled && !pending) return; /* only this main-thread poll can activate */
    pthread_mutex_lock(&g_recov_mu);
    pthread_mutex_lock(&g_tx_mu);
    bt_autonext_generation(g_generation);
    enabled = enabled && !g_bt_autonext.departed;
    if(g_confirmed && !g_rx_reopen){
        if(g_bt_autonext.active && (preference == 0 || preference == 1))
            g_bt_autonext.restore = preference;
        if(!enabled){
            (void)bt_autonext_restore(SEND_PROBE, g_generation);
        } else if(!g_bt_autonext.active &&
                  ipc_send_wire_locked("0647000C0001", SEND_PROBE, g_generation) == 0){
            g_bt_autonext.restore = preference;
            g_bt_autonext.active = 1;
            fprintf(stderr, "BT autonext experiment: stock gapless on (saved=%d)\n", preference);
        }
    }
    pending = g_bt_autonext.active;
    pthread_mutex_unlock(&g_tx_mu);
    pthread_mutex_unlock(&g_recov_mu);
}
/* One lock covers descriptor inspection, retries, close/reopen and health inspection.
 * Lock order: conversion reservation -> TX, and g_recov_mu -> TX (ipc_send_automatic); TX never enters conversion or
 * RX state. */
static int ipc_send_internal(const char *frame, int policy, unsigned generation){
    pthread_mutex_lock(&g_tx_mu);
    int rc = ipc_send_locked(frame, policy, generation);
    pthread_mutex_unlock(&g_tx_mu);
    return rc;
}
/* user-action send: a real failure raises the toast flag. */
static unsigned g_transport_seq; /* main-thread sends and recovery only */
unsigned ipc_transport_seq(void){ return g_transport_seq; }
int ipc_send_cmd(const char*frame){
    if(!strncmp(frame,"0100",4) || !strncmp(frame,"0103",4) || !strncmp(frame,"0201",4))
        g_transport_seq++; /* cancel recovery even if the transport send fails */
    /* Capture intent before sending: a toggle of externally playing audio is
     * Pause and must not become a deferred position restore. */
    pthread_mutex_lock(&g_recov_mu);
    int pause=g_confirmed && !g_rx_reopen && g_a2_report.player_generation==g_generation &&
              g_a2_report.player_confirmed && !g_a2_report.provisional && g_a2_report.state==0;
    pthread_mutex_unlock(&g_recov_mu);
    int rc = ipc_send_internal(frame, SEND_USER, ipc_generation());
    if(rc == 0 && !strcmp(frame,"0201000C0000")){
        pthread_mutex_lock(&g_recov_mu);
        g_resume_toggle=pause ? 0 : g_transport_seq; g_resume_after_report=g_a2_serial;
        pthread_mutex_unlock(&g_recov_mu);
    }
    return rc;
}
/* background send (state-sync / health probe): silent, never toasts. */
int ipc_send_probe(const char*frame){ return ipc_send_internal(frame, SEND_PROBE, ipc_generation()); }
/* Snapshot and admission share the recovery lock; receipt is not track ownership. */
int ipc_confirmed_snapshot(unsigned *generation){
    pthread_mutex_lock(&g_recov_mu);
    if(generation) *generation=g_generation;
    int confirmed=g_confirmed && !g_rx_reopen;
    pthread_mutex_unlock(&g_recov_mu);
    return confirmed;
}
int ipc_send_automatic(const char *frame, unsigned generation){
    pthread_mutex_lock(&g_recov_mu);
    int r=IPC_SEND_DEFERRED;
    if(generation==g_generation && g_confirmed && !g_rx_reopen){
        r=ipc_send_internal(frame, SEND_AUTOMATIC, generation);
        /* Dispatch and cancellation serialize with RX; a reservation is not a send. */
        if(r==0 && !strcmp(frame,"0657000C000A") &&
           g_receiver_generation==generation && g_receiver_arm){
            g_local_publication.state=LOCAL_BLOCKED;
            g_local_publication.return_arm=0;
        }
    }
    pthread_mutex_unlock(&g_recov_mu);
    return r;
}
#ifdef DISKOS_TEST_BT_PREPARE
int ipc_prepare_snapshot(track_state_t *out, unsigned *serial, unsigned *playing_frames){
    pthread_mutex_lock(&g_recov_mu);
    *out = g_a2_report; *serial = g_a2_serial; *playing_frames = g_prepare_playing_frames;
    int valid = g_confirmed && !g_rx_reopen && out->player_generation == g_generation;
    pthread_mutex_unlock(&g_recov_mu); return valid;
}
int ipc_prepare_send(const char *frame, unsigned generation, uint64_t token, unsigned transport,
                     unsigned report, unsigned playing_frames, int held, int release, unsigned poweroff){
    pthread_mutex_lock(&g_recov_mu); pthread_mutex_lock(&g_mu);
    int rc = IPC_SEND_DEFERRED;
    char volume[16]; snprintf(volume, sizeof volume, "0715000C%04X", (unsigned)g_state.volume);
    if(generation == g_generation && g_confirmed && !g_rx_reopen &&
       g_local_publication.state == LOCAL_OPEN && token == g_state.playback_token && transport == g_transport_seq &&
       poweroff == g_aa03_seq && playing_frames == g_prepare_playing_frames &&
       (!held || g_state.state != 0) &&
       (strncmp(frame, "0715", 4) || (g_state.volume_seq && !strcmp(frame, volume))) &&
       (release || strcmp(frame, "0201000C0000") ||
        (report == g_a2_serial && g_a2_report.state == 0 && g_state.state == 0)) &&
       (!release || (!strcmp(frame, "0201000C0000") && report == g_a2_serial &&
        playing_frames == g_prepare_playing_frames && g_a2_report.have_track &&
        g_a2_report.player_confirmed && !g_a2_report.provisional && g_a2_report.state == 1 && g_state.state == 1 &&
        g_a2_report.playback_token == token && g_a2_report.player_generation == generation)))
        rc = ipc_send_internal(frame, SEND_AUTOMATIC, generation);
    pthread_mutex_unlock(&g_mu); pthread_mutex_unlock(&g_recov_mu); return rc;
}
#endif
/* 1 once the /ui receive queue is open (i.e. ipc_start() succeeded). */
int ipc_is_ready(void){
    pthread_mutex_lock(&g_recov_mu); int r=g_rx_ready; pthread_mutex_unlock(&g_recov_mu); return r;
}
/* 1 (and clears) if a command send failed since the last call - UI surfaces a toast. */
int ipc_take_send_error(void){ pthread_mutex_lock(&g_tx_mu); int e=g_send_err; g_send_err=0; pthread_mutex_unlock(&g_tx_mu); return e; }

/* Separate mailbox: scanner never enters the controller or holds IPC's state lock. */
static pthread_mutex_t g_library_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_library_epoch;
void ipc_note_library_commit(void){
    pthread_mutex_lock(&g_library_mu); g_library_epoch++; pthread_mutex_unlock(&g_library_mu);
}
uint64_t ipc_library_epoch(void){
    pthread_mutex_lock(&g_library_mu); uint64_t epoch=g_library_epoch; pthread_mutex_unlock(&g_library_mu);
    return epoch;
}

/* ---- stock still-image conversion (0810 value 5 + source path -> a810 result) ----
 * mq_ui@0x48b28c frames a request as "%s%04X%04X%s" (tag, value, strlen+12, string); mq_player@0x4f169c
 * (V2.57) handles 0810: value 5 with a non-NULL string at +0x18 runs set_custom_cover_by_file(path)
 * (0x4812c8) and replies a810 through 0x4ddaec(1, "a810", code, NULL): queue index 1 is the /ui queue
 * (0x4dc944 stores the name), the NULL-string form is snprintf(buf, 13, "%s%04X%04X") sent as 12 bytes
 * (0x4ddc54..0x4ddc88). So the reply is exactly 12 chars: "a810" + %04X(code) + "000C", and a code that
 * needs more than 4 hex digits (the converter returns -1) pushes the length field out: "a810FFFFFFFF".
 * The reply has no operation id, so correlation is one pending operation + player generation + deadline.
 * Value 5 with a NULL path is the GIF case: no conversion, no reply (mq_player@0x4f16dc: NULL string -> straight to
 * the LOCK_THEME store at 0x4f16b4); see ipc_gif_select. */
#define CONV_PREFIX "/tmp/sdcard/CustomCover/"   /* mq_ui picker folder, same path it sends (0x6a077c) */
#define CONV_PATH_MAX 255                        /* mq_ui builds the path in a 256-byte buffer (0x47c06c) */
static pthread_mutex_t g_conv_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { ipc_conv_state state; unsigned op, code, gen; uint64_t deadline; } g_conv;
static int g_apply_reserved; /* g_conv_mu; held through database COMMIT and recovery */
static unsigned g_conv_ops, g_conv_rejected, g_conv_ignored;   /* [g_conv_mu] */
static uint64_t conv_now_ms(void){
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec*1000u + (uint64_t)t.tv_nsec/1000000u;
}
/* Only the stock picker's flat folder and the formats the converter decodes (PNG/JPEG/BMP by magic,
 * 0x482f80). GIF is refused here: stock never converts it.
 * SD sources have their own lexical rules; stored assets use allowed_path().
 * Both use the shared component symlink check. */
static int conv_path_ok(const char *p){
    size_t n = p ? strlen(p) : 0, pl = sizeof(CONV_PREFIX)-1;
    if(n <= pl || n > CONV_PATH_MAX || strncmp(p, CONV_PREFIX, pl)) return 0;
    const char *name = p + pl, *dot = NULL;
    if(!strcmp(name,".") || !strcmp(name,"..")) return 0;
    for(const char *c = name; *c; c++){
        unsigned char u = (unsigned char)*c;
        if(u < 0x20 || u == 0x7f || u == '/') return 0;
        if(u == '.') dot = c;
    }
    return dot && dot[1] && (!strcasecmp(dot,".jpg") || !strcasecmp(dot,".jpeg") ||
                             !strcasecmp(dot,".png") || !strcasecmp(dot,".bmp")) &&
           custom_saver_path_no_symlinks(p);
}
/* Caller holds g_conv_mu. PENDING past its deadline or from an older player generation is UNRESOLVED. */
static void conv_settle(void){
    if(g_conv.state != IPC_CONV_PENDING) return;
    if(g_conv.gen != ipc_generation() || conv_now_ms() >= g_conv.deadline) g_conv.state = IPC_CONV_UNRESOLVED;
}
static void conv_new_generation(unsigned generation){
    (void)generation;
    pthread_mutex_lock(&g_conv_mu); conv_settle(); pthread_mutex_unlock(&g_conv_mu);
}
/* RX thread. Malformed or unexpected replies change nothing (the deadline still bounds the operation). */
static void conv_on_reply(const char *buf, int n){
    unsigned code = 0; int ok = n == 12 && all_hex(buf+4, 8);
    if(ok){
        if(!strncasecmp(buf+8, "000C", 4)) code = (unsigned)strtoul((char[5]){buf[4],buf[5],buf[6],buf[7],0}, 0, 16);
        else { code = (unsigned)strtoul((char[9]){buf[4],buf[5],buf[6],buf[7],buf[8],buf[9],buf[10],buf[11],0}, 0, 16);
               /* %04X adds no leading zero once the value exceeds four digits.
                * A zero-leading body with no 000C suffix is malformed. */
               ok = buf[4] != '0' && code != 0; }
    }
    pthread_mutex_lock(&g_conv_mu);
    conv_settle();
    if(!ok) g_conv_rejected++;
    else if((g_conv.state != IPC_CONV_PENDING &&
             g_conv.state != IPC_CONV_UNRESOLVED) ||
            g_conv.gen != ipc_generation()) g_conv_ignored++;   /* nothing outstanding / released / other generation */
    else { g_conv.code = code; g_conv.state = code ? IPC_CONV_FAILED : IPC_CONV_DONE; }
    pthread_mutex_unlock(&g_conv_mu);
}
/* Caller holds g_conv_mu and has settled: an operation is pending, holds an unreleased result, or may still be
 * writing (UNRESOLVED in this player generation). */
static int conv_blocked(unsigned gen){
    (void)gen;
    return g_conv.state == IPC_CONV_PENDING || g_conv.state == IPC_CONV_DONE || g_conv.state == IPC_CONV_FAILED ||
           g_conv.state == IPC_CONV_UNRESOLVED;
}
int ipc_conv_busy(void){
    pthread_mutex_lock(&g_conv_mu); conv_settle();
    int busy = conv_blocked(ipc_generation());
    pthread_mutex_unlock(&g_conv_mu);
    return busy;
}
int ipc_apply_reserve(void){
    pthread_mutex_lock(&g_conv_mu); conv_settle();
    int ok = !g_apply_reserved && !conv_blocked(ipc_generation());
    if(ok) g_apply_reserved = 1;
    pthread_mutex_unlock(&g_conv_mu);
    return ok ? 0 : -1;
}
void ipc_apply_release(void){
    pthread_mutex_lock(&g_conv_mu); g_apply_reserved = 0; pthread_mutex_unlock(&g_conv_mu);
}
/* Pathless 0810 values 0..4 select system themes; 5 reselects saved Custom without conversion.
 * mq_player@0x4f16ac-0x4f16e0: both routes store config id 37. Live effect unverified. */
int ipc_saver_reselect(unsigned style){
    if(style > 5 || !ipc_is_ready()) return -1;
    char frame[13]; snprintf(frame, sizeof frame, "0810%04X000C", style);
    return ipc_send_internal(frame, SEND_PROBE, ipc_generation());
}
/* Stock's GIF selection: mq_ui@0x47c628 calls 0x48b28c("0810", 5, NULL), whose NULL-string form is
 * snprintf(buf, 13, "%s%04X%04X", tag, 5, 12) (format 0x6a38e0, 0x48b3bc..0x48b3dc). The lock must be held through
 * the send so a conversion cannot start between the check and the frame. */
ipc_gif_select_status ipc_gif_select(void){
    if(!ipc_is_ready()) return IPC_GIF_SELECT_NOT_READY;
    pthread_mutex_lock(&g_conv_mu); conv_settle();
    ipc_gif_select_status st = conv_blocked(ipc_generation()) ? IPC_GIF_SELECT_BUSY :
        ipc_send_internal("08100005000C", SEND_PROBE, ipc_generation()) ? IPC_GIF_SELECT_SEND : IPC_GIF_SELECT_OK;
    pthread_mutex_unlock(&g_conv_mu);
    return st;
}
ipc_conv_start_status ipc_conv_start(const char *src, unsigned *op){
    if(!conv_path_ok(src)) return IPC_CONV_START_PATH;
    if(!ipc_is_ready()) return IPC_CONV_START_NOT_READY;
    char frame[CONV_PATH_MAX + 13];
    snprintf(frame, sizeof frame, "08100005%04X%s", (unsigned)(strlen(src) + 12), src);
    unsigned gen = ipc_generation();
    pthread_mutex_lock(&g_conv_mu);
    conv_settle();
    /* A result must be released first. UNRESOLVED blocks because neither a timeout
     * nor a queue reattachment proves that the converter stopped writing. */
    if(g_apply_reserved || conv_blocked(gen)){
        pthread_mutex_unlock(&g_conv_mu); return IPC_CONV_START_BUSY;
    }
    g_conv.state = IPC_CONV_PENDING; g_conv.code = 0; g_conv.gen = gen;   /* armed BEFORE the send: the reply can race it */
    g_conv.deadline = conv_now_ms() + IPC_CONV_TIMEOUT_MS;
    if(!++g_conv_ops) g_conv_ops = 1;
    g_conv.op = g_conv_ops;
    unsigned id = g_conv.op;
    pthread_mutex_unlock(&g_conv_mu);
    if(ipc_send_internal(frame, SEND_PROBE, ipc_generation()) != 0){
        pthread_mutex_lock(&g_conv_mu);
        if(g_conv.op == id && g_conv.state == IPC_CONV_PENDING) g_conv.state = IPC_CONV_IDLE;
        pthread_mutex_unlock(&g_conv_mu);
        return IPC_CONV_START_SEND;
    }
    if(op) *op = id;
    return IPC_CONV_START_OK;
}
ipc_conv_state ipc_conv_poll(unsigned op, unsigned *code){
    pthread_mutex_lock(&g_conv_mu);
    conv_settle();
    ipc_conv_state st = op == g_conv.op && op ? g_conv.state : IPC_CONV_STALE;
    if(code) *code = st == IPC_CONV_FAILED ? g_conv.code : 0;
    pthread_mutex_unlock(&g_conv_mu);
    return st;
}
int ipc_conv_release(unsigned op){
    pthread_mutex_lock(&g_conv_mu);
    conv_settle();
    int ok = op && op == g_conv.op &&
             (g_conv.state == IPC_CONV_DONE || g_conv.state == IPC_CONV_FAILED);
    if(ok) g_conv.state = IPC_CONV_IDLE;
    pthread_mutex_unlock(&g_conv_mu);
    return ok ? 0 : -1;
}
