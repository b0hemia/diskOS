/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "ipc.h"
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
 * state (g_tx, g_tx_dev/ino, g_tx_stale) is touched ONLY on the main thread (both senders
 * and the health timer run there), so it needs no lock. g_rx itself is touched only by the
 * RX thread (+ single-threaded startup), so it too is unlocked. */
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
static int g_player_mode = -1;   /* [g_recov_mu] last a607 external-mode announced by the player (8=LOCALPLAYER).
                                  * -1 = none received THIS player generation. NOTE (RE-verified vs v2.57): a607 is
                                  * NOT a reliable *solicited* readiness oracle - a 0607 query's reply goes to the
                                  * /player queue, not /ui; the player only announces a607 to /ui after a 0657 mode
                                  * CHANGE. Readiness is established by the a2 probe + settle (main.c); when an a607
                                  * does arrive here, mode==8 is a valid "LOCALPLAYER set" confirmation.
                                  * Reset to -1 on /ui reopen (new player generation). See ipc_player_mode(). */
static int g_tip_event = 0;       /* [g_recov_mu] last a60a TIP_INFO_EVENT (0x0212=connect, 0x0213=disconnect) */
static unsigned g_tip_seq = 0;    /* [g_recov_mu] bumps on each a60a event */
static dev_t g_tx_dev = 0;  static ino_t g_tx_ino = 0;   /* [main-thread only] identity of /player g_tx sends to */
static int g_tx_stale    = 0;   /* [main-thread only] health -> sender: /player recreated, drop g_tx */
static int g_thread_started = 0;         /* idempotency for ipc_start (NOT g_rx, which recovery swaps) */

/* Record the (dev,ino) identity of an open queue descriptor. Returns 0 on success. */
static int mq_identity(mqd_t q, dev_t *dev, ino_t *ino){
    struct stat s;
    if(q==(mqd_t)-1 || fstat((int)q, &s) != 0) return -1;
    *dev = s.st_dev; *ino = s.st_ino; return 0;
}

/* ---- helpers ---- */
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

static void parse_a2(const char*payload,int len){
    jsmn_parser p; jsmntok_t tk[64];
    static char repaired[8192], song[8192];
    jsmntok_t st[96]; int nt, n2=0, v;
    /* An id-first song object identifies the stock layout. A recognized but
     * malformed/ambiguous frame must never fall through to permissive JSON. */
    const char *stock_head="{\"song\":\"{\\\"id\\\":";
    int head_len=(int)strlen(stock_head);
    if(len>=head_len && !memcmp(payload,stock_head,head_len)){
        int normalized=stock_a2_normalize(payload,len,repaired,sizeof repaired);
        if(!normalized) return;
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
    if(nt<1 || n2<0) return;
    /* A truncated title cannot be used as eligibility evidence. Reject before publication. */
    if(n2>0){
        char title[sizeof g_state.title]; int sv=find_val(song,st,n2,"song_name");
        if(sv>=0 && unescape(song+st[sv].start,st[sv].end-st[sv].start,title,sizeof title)<0) return;
    }
    pthread_mutex_lock(&g_mu);
    char oldpath[256]; snprintf(oldpath, sizeof oldpath, "%s", g_state.path);   /* detect a track-identity change */
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
            if(sv>=0 && unescape(song+st[sv].start, st[sv].end-st[sv].start, t_path, sizeof t_path) >= 0 && t_path[0]) have_path=1;
            sv=find_val(song,st,n2,"song_duration_time"); if(sv>=0) t_dur=tok_long(song,&st[sv]);
            sv=find_val(song,st,n2,"song_sample_rate");   if(sv>=0) t_sr=(int)tok_long(song,&st[sv]);
            sv=find_val(song,st,n2,"is_dsd");             if(sv>=0) t_dsd=tok_bool(song,&st[sv]);
            if(have_path){                 /* complete identity -> publish the whole track atomically */
                int changed=!g_state.player_confirmed || strcmp(g_state.path,t_path) ||
                    g_state.song_id_valid!=(unsigned)sid_ok || g_state.pos_id_valid!=(unsigned)pid_ok ||
                    g_state.song_id_raw!=song_id || g_state.pos_id_raw!=pos_id;
                if(changed){
                    g_state.playback_token++; g_title_frozen=g_title_stock=0;
                    /* The previous position does not belong to this identity. */
                    g_state.position_ms=0; g_state.pos_seq=0;
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
    pthread_mutex_unlock(&g_mu);
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
    g_generation++; g_scr_on = -1; g_scr_seq = g_aa03_seq = 0;
}
static void parse_frame(const char*buf,int n){
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
        parse_a2(buf+8, n-8);
    } else if(buf[0]=='a'&&buf[1]=='7'&&buf[2]=='1'&&buf[3]=='4' && n>=12){
        /* "a714000C00<VV><pos16>" - VV (hex chars 10-11) = volume level */
        char h[3]={buf[10],buf[11],0};
        if(!all_hex(h,2)) return;    /* malformed -> keep last volume, don't reset to 0 */
        int v=(int)strtol(h,0,16);
        pthread_mutex_lock(&g_mu); g_state.volume=v; g_state.volume_seq++; pthread_mutex_unlock(&g_mu);
    } else if(buf[0]=='a'&&buf[1]=='6'&&buf[2]=='0'&&buf[3]=='7' && n>=12){
        /* "a607000C<MODE>" - the player's external/input mode, announced to /ui after a 0657 mode CHANGE.
         * (A 0607 query's reply goes to the /player queue, NOT here, so we do NOT rely on soliciting it - see
         * g_player_mode.) MODE 0008 = LOCALPLAYER; when this arrives, ==8 confirms LOCALPLAYER is set. VALUE is
         * the 4 hex chars after the 000C length (buf[8..11]). */
        char h[5]={buf[8],buf[9],buf[10],buf[11],0};
        if(!all_hex(h,4)) return;
        int mode=(int)strtol(h,0,16);
        pthread_mutex_lock(&g_recov_mu); g_player_mode=mode; pthread_mutex_unlock(&g_recov_mu);
    } else if(buf[0]=='a'&&buf[1]=='6'&&buf[2]=='0'&&buf[3]=='a' && n>=12){
        /* "a60a000C<PARAM4HEX>" - TIP_INFO_EVENT from player:
         * 0212 (530) = on_usb_connect
         * 0213 (531) = on_usb_disconnect */
        char h[5]={buf[8],buf[9],buf[10],buf[11],0};
        if(!all_hex(h,4)) return;
        int evt=(int)strtol(h,0,16);
        pthread_mutex_lock(&g_recov_mu); g_tip_event=evt; g_tip_seq++; pthread_mutex_unlock(&g_recov_mu);
    }
    /* NB: for playback the player emits a1/a2/a714 to /ui (a2=state/love/work_mode/track, a1=position,
     * a714=volume) - no a622/a639/a704 completion replies. SEPARATELY, a 0657 mode CHANGE makes it announce
     * a607 (external/input mode) to /ui, parsed above (a bare 0607 query replies on /player, not /ui). */
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
    pthread_mutex_lock(&g_recov_mu); g_rx_reopen = 0; pthread_mutex_unlock(&g_recov_mu);
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
    g_rx_dev = d; g_rx_ino = io; g_rx_ready = 1; g_reconnected = 1; g_player_mode = -1; g_tip_event = 0; screen_new_generation();
    unsigned generation=g_generation;
    pthread_mutex_unlock(&g_recov_mu);
    pthread_mutex_lock(&g_mu);
    int had_path=g_state.path[0]!=0;
    clear_track(); g_state.playback_token++; g_state.player_generation=generation; g_state.seq++;
    if(had_path) g_state.path_seq=g_state.seq;
    pthread_mutex_unlock(&g_mu);
    if(old!=(mqd_t)-1 && old!=nw) mq_close(old);   /* open-before-close, but NEVER close a descriptor number
                                                    * the new mq_open reused (EBADF path: the old fd was dead,
                                                    * so its number can come back as nw - closing it kills the
                                                    * fresh queue and re-spins EBADF). */
    fprintf(stderr,"ipc: /ui reattached after player restart\n");
    return 0;
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
    g_tx=mq_open("/player", O_WRONLY|O_NONBLOCK);
    if(g_tx==(mqd_t)-1) fprintf(stderr,"mq_open /player fail: %s\n",strerror(errno));
    else mq_identity(g_tx, &g_tx_dev, &g_tx_ino);
    mq_identity(g_rx, &g_rx_dev, &g_rx_ino);
    memset(&g_state,0,sizeof g_state);
    pthread_t t;
    if(pthread_create(&t,0,ipc_thread,0)!=0){   /* no rx thread -> tear down, don't half-init */
        fprintf(stderr,"ipc_thread create failed: %s\n",strerror(errno));
        mq_close(g_rx); g_rx=(mqd_t)-1;
        if(g_tx!=(mqd_t)-1){ mq_close(g_tx); g_tx=(mqd_t)-1; }
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
    mqd_t q = mq_open("/player", O_WRONLY|O_NONBLOCK);   /* TX state is main-thread-only: no lock */
    if(q!=(mqd_t)-1){
        struct stat s;
        if(g_tx!=(mqd_t)-1 && (g_tx_dev||g_tx_ino) && fstat((int)q,&s)==0 &&
           (s.st_dev!=g_tx_dev || s.st_ino!=g_tx_ino))
            g_tx_stale = 1;
        mq_close(q);
    }
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
unsigned ipc_generation(void){
    pthread_mutex_lock(&g_recov_mu); unsigned r=g_generation; pthread_mutex_unlock(&g_recov_mu);
    return r;
}
/* Last a60a TIP_INFO_EVENT from player (0x0212=connect, 0x0213=disconnect). If seq != NULL, assigns event sequence. */
int ipc_tip_event(unsigned *seq){
    pthread_mutex_lock(&g_recov_mu);
    int r = g_tip_event;
    if(seq) *seq = g_tip_seq;
    pthread_mutex_unlock(&g_recov_mu);
    return r;
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
/* Send a command frame straight to /player. g_tx is O_NONBLOCK so this can never
 * stall the caller or the player; mq_send is thread-safe. */
static volatile int g_send_err = 0;   /* sticky until ipc_take_send_error() reads it */

/* quiet=1 -> never set the user-facing error flag (background state-sync / health
 * probes must not raise "Player didn't respond" during the startup connect race). */
static int ipc_send_internal(const char*frame, int quiet){
    if(g_tx_stale && g_tx!=(mqd_t)-1){ mq_close(g_tx); g_tx=(mqd_t)-1; }  /* health saw /player recreated */
    g_tx_stale = 0;
    if(g_tx==(mqd_t)-1){
        g_tx=mq_open("/player", O_WRONLY|O_NONBLOCK);  /* (re)open if absent/healed */
        g_tx_dev = 0; g_tx_ino = 0;   /* clear first: a failed capture must not leave a stale identity */
        if(g_tx!=(mqd_t)-1) mq_identity(g_tx, &g_tx_dev, &g_tx_ino);
    }
    if(g_tx==(mqd_t)-1){ if(!quiet) g_send_err=1; return -1; }
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
    if(!quiet){ g_send_err = 1; fprintf(stderr,"ipc_send_cmd '%s' failed: %s\n", frame, strerror(err)); }
    return -1;
}
/* user-action send: a real failure raises the toast flag. */
static unsigned g_transport_seq; /* main-thread sends and recovery only */
unsigned ipc_transport_seq(void){ return g_transport_seq; }
int ipc_send_cmd(const char*frame){
    if(!strncmp(frame,"0100",4) || !strncmp(frame,"0103",4) || !strncmp(frame,"0201",4))
        g_transport_seq++; /* cancel recovery even if the transport send fails */
    return ipc_send_internal(frame, 0);
}
/* background send (state-sync / health probe): silent, never toasts. */
int ipc_send_probe(const char*frame){ return ipc_send_internal(frame, 1); }
/* 1 once the /ui receive queue is open (i.e. ipc_start() succeeded). */
int ipc_is_ready(void){
    pthread_mutex_lock(&g_recov_mu); int r=g_rx_ready; pthread_mutex_unlock(&g_recov_mu); return r;
}
/* 1 (and clears) if a command send failed since the last call - UI surfaces a toast. */
int ipc_take_send_error(void){ int e=g_send_err; g_send_err=0; return e; }

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
