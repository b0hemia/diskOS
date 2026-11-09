/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Last.fm scrobbling for diskOS.
 *
 * Per-user provisioning: the api_key + shared secret are NOT baked into the binary -
 * each user creates their own Last.fm app and provisions their credentials on-device
 * (via the setup web server), stored in config. So nothing sensitive ships.
 *
 * This file is internally sectioned:
 *   [transport]  form-encode + api_sig (md5) + POST via wget (body in a memfd, off argv)
 *   [parse]      jsmn extraction of token / session key / lfm error
 *   [auth]       getToken / getSession jobs
 *   [watch]      play-state watcher -> updateNowPlaying + scrobble eligibility
 *   [queue]      offline scrobble queue (/usr/data/lastfm.queue)
 *   [worker]     single detached network worker + mutex-published result
 *   [ui]         Settings screen + QR, consumed on the LVGL main thread
 *
 * VERIFIED ON-DEVICE before writing: GNU Wget 1.20.3; HTTPS to ws.audioscrobbler.com
 * works (GET+POST); --post-file=- does NOT read stdin (GNU limitation) but
 * --post-file=/proc/self/fd/N works; /tmp is tmpfs; kernel 4.4.94 has memfd_create.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include "md5.h"
#define JSMN_STATIC          /* file-local jsmn (ipc.c already exports the global copy) */
#include "jsmn.h"
#include "musicdb.h"   /* mdb_is_book_path: audiobooks are never scrobbled */

#define LFM_ENDPOINT      "https://ws.audioscrobbler.com/2.0/"
#define LFM_MAX_FORM      (32u * 1024u)
#define LFM_MAX_RESPONSE  (64u * 1024u)
#define LFM_HTTP_DEADLINE_MS 20000

/* Secure wipe - volatile so the compiler can't optimize the zeroing away (used on any
 * buffer that held the shared secret, sig source, session key, or a signed form body). */
static void lfm_wipe(void *p, size_t n){ volatile unsigned char *v=(volatile unsigned char*)p; while(n--) *v++=0; }

/* ===================== [transport] ===================================== */

typedef struct { const char *key; const char *value; } lfm_param_t;  /* value = RAW (unencoded) */

/* RFC3986 percent-encode into dst (cap incl NUL). Unreserved = A-Za-z0-9-_.~ ; all
 * else -> %XX. Returns bytes written (excl NUL), or -1 on overflow. */
static int lfm_pct(const char *s, char *dst, int cap){
    static const char hx[] = "0123456789ABCDEF";
    if(!s || !dst || cap <= 0) return -1;
    int o = 0;
    for(; *s; s++){
        unsigned char c = (unsigned char)*s;
        int unreserved = (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||
                         c=='-'||c=='_'||c=='.'||c=='~';
        if(unreserved){ if(o+1>=cap) return -1; dst[o++]=(char)c; }
        else          { if(o+3>=cap) return -1; dst[o++]='%'; dst[o++]=hx[c>>4]; dst[o++]=hx[c&15]; }
    }
    dst[o]=0; return o;
}

static int lfm_param_cmp(const void *a_, const void *b_){
    const lfm_param_t *a=a_, *b=b_; return strcmp(a->key, b->key);
}

/* api_sig = md5( sorted key+RAWvalue concatenated (excl format/api_sig) + secret ). Hex into out[33]. */
static int lfm_make_sig(const lfm_param_t *p, size_t n, const char *secret, char out[33]){
    if((!p && n) || !secret) return -1;
    lfm_param_t s[24]; if(n > 24) return -1;
    if(n) memcpy(s, p, n*sizeof *s);
    qsort(s, n, sizeof *s, lfm_param_cmp);
    size_t need = strlen(secret) + 1;
    for(size_t i=0;i<n;i++){
        if(!s[i].key || !s[i].value || !s[i].key[0]) return -1;
        if(i && !strcmp(s[i-1].key, s[i].key)) return -1;       /* ambiguous dup */
        if(!strcmp(s[i].key,"format") || !strcmp(s[i].key,"api_sig")) continue;
        size_t kl=strlen(s[i].key), vl=strlen(s[i].value);
        if(kl > 65535 || vl > 65535) return -1;                 /* sane per-field cap -> need can't overflow */
        need += kl + vl;
    }
    char *src = malloc(need); if(!src) return -1;
    size_t o=0;
    for(size_t i=0;i<n;i++){
        if(!strcmp(s[i].key,"format") || !strcmp(s[i].key,"api_sig")) continue;
        size_t kl=strlen(s[i].key), vl=strlen(s[i].value);
        memcpy(src+o, s[i].key, kl); o+=kl;
        memcpy(src+o, s[i].value, vl); o+=vl;
    }
    size_t sl=strlen(secret); memcpy(src+o, secret, sl); o+=sl;
    md5_hex(src, o, out);
    lfm_wipe(src, o); free(src);                                /* don't leave sig source in heap */
    return 0;
}

/* Build a signed, form-urlencoded POST body from base params: appends api_sig (computed
 * on RAW values) + format=json, then percent-encodes keys+values. *out is heap (caller frees). */
static int lfm_signed_form(const lfm_param_t *base, size_t n, const char *secret,
                           char **out, size_t *out_len){
    if(!out || !out_len || n > 24 || (!base && n)) return -1;
    *out=NULL; *out_len=0;
    for(size_t i=0;i<n;i++)                              /* caller must not supply reserved keys */
        if(!base[i].key || !strcmp(base[i].key,"api_sig") || !strcmp(base[i].key,"format")) return -1;
    char sig[33];
    if(lfm_make_sig(base, n, secret, sig) != 0) return -1;
    lfm_param_t all[26];
    if(n) memcpy(all, base, n*sizeof *all);
    all[n].key="api_sig"; all[n].value=sig;            /* appended AFTER sig computed (excluded from it) */
    all[n+1].key="format"; all[n+1].value="json";
    size_t total = n+2;
    char *buf = malloc(LFM_MAX_FORM); if(!buf){ lfm_wipe(sig,sizeof sig); return -1; }
    int o=0;
    for(size_t i=0;i<total;i++){
        int w;
        if(i){ if(o+1>=(int)LFM_MAX_FORM) goto over; buf[o++]='&'; }
        w = lfm_pct(all[i].key, buf+o, (int)LFM_MAX_FORM-o); if(w<0) goto over; o+=w;
        if(o+1>=(int)LFM_MAX_FORM){ goto over; }
        buf[o++]='=';
        w = lfm_pct(all[i].value, buf+o, (int)LFM_MAX_FORM-o); if(w<0) goto over; o+=w;
    }
    buf[o]=0; *out=buf; *out_len=(size_t)o; lfm_wipe(sig,sizeof sig); return 0;
over:
    lfm_wipe(buf,LFM_MAX_FORM); free(buf); lfm_wipe(sig,sizeof sig); return -1;
}

typedef enum { LFM_HTTP_OK=0, LFM_HTTP_SPAWN, LFM_HTTP_TIMEOUT, LFM_HTTP_IO,
               LFM_HTTP_TOO_LARGE, LFM_HTTP_EMPTY } lfm_http_err_t;
typedef struct { char *body; size_t body_len; int wget_status; } lfm_http_result_t;

/* Put the POST body in an anonymous file (memfd, off argv), falling back to a tmpfs
 * mkostemp file that is UNLINKED immediately (a /proc/self/fd/N path keeps working on an
 * unlinked file, so nothing is left on disk and there is no predictable-name collision).
 * Returns an fd positioned at 0, or -1. The child clears FD_CLOEXEC before exec. */
static int lfm_body_fd(const void *body, size_t len){
    if(len && !body) return -1;
    int fd = memfd_create("lfm-post", 0);
    if(fd < 0){
        char tmpl[] = "/tmp/.lfm-XXXXXX";
        fd = mkostemp(tmpl, O_CLOEXEC);
        if(fd < 0) return -1;
        if(unlink(tmpl) != 0){ close(fd); return -1; }   /* never send creds via a persistent file */
    }
    const unsigned char *p=body; size_t left=len;
    while(left){
        size_t chunk = left > 0x7ffff000u ? 0x7ffff000u : left;   /* keep one write < SSIZE_MAX */
        ssize_t w = write(fd, p, chunk);
        if(w>0){ p+=w; left-=(size_t)w; }
        else if(w<0 && errno==EINTR) continue;
        else { close(fd); return -1; }
    }
    off_t r; do { r = lseek(fd,0,SEEK_SET); } while(r<0 && errno==EINTR);
    if(r<0){ close(fd); return -1; }
    return fd;
}

static int64_t lfm_now_ms(void){    /* 64-bit: 32-bit ms would wrap after ~24.9 days of uptime */
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (int64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000;
}

/* POST body to Last.fm, capture response. Body + session key never touch argv. */
static lfm_http_err_t lfm_http_post(const void *body, size_t body_len, lfm_http_result_t *res){
    res->body=NULL; res->body_len=0; res->wget_status=-1;
    int body_fd = lfm_body_fd(body, body_len);
    if(body_fd < 0) return LFM_HTTP_IO;
    int pfd[2];
    if(pipe2(pfd, O_CLOEXEC) < 0){ close(body_fd); return LFM_HTTP_IO; }
    pid_t pid = fork();
    if(pid < 0){ close(body_fd); close(pfd[0]); close(pfd[1]); return LFM_HTTP_SPAWN; }
    if(pid == 0){
        char post_arg[64];
        if(dup2(pfd[1], STDOUT_FILENO) < 0) _exit(126);
        close(pfd[0]); close(pfd[1]);
        int fl = fcntl(body_fd, F_GETFD);                 /* body_fd must survive exec */
        if(fl<0 || fcntl(body_fd, F_SETFD, fl & ~FD_CLOEXEC)<0) _exit(126);
        snprintf(post_arg, sizeof post_arg, "--post-file=/proc/self/fd/%d", body_fd);
        execlp("wget","wget","-qO-","--content-on-error","-T","15",post_arg,LFM_ENDPOINT,(char*)NULL);
        _exit(errno==ENOENT ? 127 : 126);
    }
    close(pfd[1]); close(body_fd);
    int rfl = fcntl(pfd[0], F_GETFL);                 /* nonblock only the read end; keep other flags */
    if(rfl < 0 || fcntl(pfd[0], F_SETFL, rfl | O_NONBLOCK) < 0){
        close(pfd[0]); kill(pid,SIGKILL); while(waitpid(pid,NULL,0)<0 && errno==EINTR){} return LFM_HTTP_IO;
    }
    char *buf = malloc(LFM_MAX_RESPONSE); size_t blen=0;
    if(!buf){ close(pfd[0]); kill(pid,SIGKILL); while(waitpid(pid,NULL,0)<0 && errno==EINTR){} return LFM_HTTP_IO; }
    int64_t deadline = lfm_now_ms() + LFM_HTTP_DEADLINE_MS;
    lfm_http_err_t err = LFM_HTTP_OK; int status=0, reaped=0;
    for(;;){
        int64_t remain = deadline - lfm_now_ms();
        if(remain <= 0){ err=LFM_HTTP_TIMEOUT; break; }
        struct pollfd pf = { pfd[0], POLLIN, 0 };
        int pr = poll(&pf, 1, remain>1000?1000:(int)remain);
        if(pr < 0){ if(errno==EINTR) continue; err=LFM_HTTP_IO; break; }
        if(pr > 0 && (pf.revents & (POLLIN|POLLHUP))){
            ssize_t r = read(pfd[0], buf+blen, LFM_MAX_RESPONSE-1-blen);
            if(r > 0){ blen += (size_t)r; if(blen >= LFM_MAX_RESPONSE-1){ err=LFM_HTTP_TOO_LARGE; break; } }
            else if(r == 0) break;                           /* EOF */
            else if(r < 0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR){ err=LFM_HTTP_IO; break; }
        }
        if(waitpid(pid,&status,WNOHANG)==pid){ reaped=1; /* drain remaining then EOF */ }
    }
    close(pfd[0]);
    if(!reaped){
        kill(pid, SIGTERM);
        int64_t t2 = lfm_now_ms()+250;
        while(waitpid(pid,&status,WNOHANG)==0 && lfm_now_ms()<t2) usleep(10000);
        if(waitpid(pid,&status,WNOHANG)==0){ kill(pid,SIGKILL); }
        while(waitpid(pid,&status,0)<0 && errno==EINTR){}
    }
    buf[blen<LFM_MAX_RESPONSE?blen:LFM_MAX_RESPONSE-1]=0;
    res->wget_status = WIFEXITED(status)?WEXITSTATUS(status):(WIFSIGNALED(status)?128+WTERMSIG(status):-1);
    if(err != LFM_HTTP_OK){ lfm_wipe(buf,LFM_MAX_RESPONSE); free(buf); return err; }
    if(blen == 0){ free(buf); return LFM_HTTP_EMPTY; }
    res->body = buf; res->body_len = blen; return LFM_HTTP_OK;
}
static void lfm_http_result_free(lfm_http_result_t *r){
    if(r && r->body){ lfm_wipe(r->body,r->body_len); free(r->body); r->body=NULL; r->body_len=0; }
}

/* ===================== [parse] ========================================= */

typedef struct {
    int  api_error;          /* 0 = none; else the Last.fm numeric error code */
    int  scrobble_ack;       /* 1 = response carried the top-level "scrobbles" object (a real ack) */
    char message[160];
    char token[64];
    char session_key[64];
    char username[128];
} lfm_api_response_t;

/* jsmn: index just past token i (skip its whole subtree) */
static int lfm_tok_after(const jsmntok_t *tk, int nt, int i){
    if(i < 0 || i >= nt) return nt;
    int end = tk[i].end; i++;
    while(i < nt && tk[i].start < end) i++;
    return i;
}
/* value-token index for `key` in object at index `obj`, or -1 */
static int lfm_object_get(const char *js, size_t jslen, const jsmntok_t *tk, int nt, int obj, const char *key){
    if(obj < 0 || obj >= nt || tk[obj].type != JSMN_OBJECT) return -1;
    int i = obj + 1; size_t kl = strlen(key);
    for(int pair=0; pair < tk[obj].size && i+1 < nt; pair++){
        int k=i, v=i+1;
        if(tk[k].type==JSMN_STRING && tk[k].start>=0 && tk[k].end>=tk[k].start &&
           (size_t)tk[k].end<=jslen && (size_t)(tk[k].end-tk[k].start)==kl &&
           memcmp(js+tk[k].start, key, kl)==0) return v;
        i = lfm_tok_after(tk, nt, v);
    }
    return -1;
}
/* Copy + UNESCAPE a jsmn STRING token into dst (bounded). Returns len>=0, or -1 (not a
 * string / bad span / overflow / bad escape). Handles \" \\ \/ \b \f \n \r \t and \uXXXX (BMP). */
static int lfm_copy_str(const char *js, size_t jslen, const jsmntok_t *t, char *dst, int cap){
    if(t->type != JSMN_STRING || t->start < 0 || t->end < t->start || (size_t)t->end > jslen) return -1;
    int o=0;
    for(int i=t->start; i<t->end; i++){
        unsigned char c=(unsigned char)js[i];
        if(c=='\\' && i+1<t->end){
            char e=js[++i], out;
            switch(e){
                case '"':out='"';break; case '\\':out='\\';break; case '/':out='/';break;
                case 'b':out='\b';break; case 'f':out='\f';break;  case 'n':out='\n';break;
                case 'r':out='\r';break; case 't':out='\t';break;
                case 'u': {
                    if(i+4>=t->end) return -1;
                    unsigned cp=0;
                    for(int k=0;k<4;k++){ char h=js[i+1+k]; cp<<=4;
                        if(h>='0'&&h<='9')cp|=(unsigned)(h-'0'); else if(h>='a'&&h<='f')cp|=(unsigned)(h-'a'+10);
                        else if(h>='A'&&h<='F')cp|=(unsigned)(h-'A'+10); else return -1; }
                    i+=4;
                    if(cp>=0xD800 && cp<=0xDBFF){          /* high surrogate: expect a low next */
                        if(i+6<t->end && js[i+1]=='\\' && js[i+2]=='u'){
                            unsigned lo=0; int ok=1;
                            for(int k=0;k<4;k++){ char h=js[i+3+k]; lo<<=4;
                                if(h>='0'&&h<='9')lo|=(unsigned)(h-'0'); else if(h>='a'&&h<='f')lo|=(unsigned)(h-'a'+10);
                                else if(h>='A'&&h<='F')lo|=(unsigned)(h-'A'+10); else { ok=0; break; } }
                            if(ok && lo>=0xDC00 && lo<=0xDFFF){ cp = 0x10000 + ((cp-0xD800)<<10) + (lo-0xDC00); i+=6; }
                            else continue;                  /* drop unpaired high surrogate */
                        } else continue;
                    } else if(cp>=0xDC00 && cp<=0xDFFF){ continue; }  /* drop lone low surrogate */
                    if(cp==0) continue;                     /* never emit an embedded NUL */
                    if(cp<0x80){ if(o+1>=cap) return -1; dst[o++]=(char)cp; }
                    else if(cp<0x800){ if(o+2>=cap) return -1; dst[o++]=(char)(0xC0|(cp>>6)); dst[o++]=(char)(0x80|(cp&0x3F)); }
                    else if(cp<0x10000){ if(o+3>=cap) return -1; dst[o++]=(char)(0xE0|(cp>>12)); dst[o++]=(char)(0x80|((cp>>6)&0x3F)); dst[o++]=(char)(0x80|(cp&0x3F)); }
                    else { if(o+4>=cap) return -1; dst[o++]=(char)(0xF0|(cp>>18)); dst[o++]=(char)(0x80|((cp>>12)&0x3F)); dst[o++]=(char)(0x80|((cp>>6)&0x3F)); dst[o++]=(char)(0x80|(cp&0x3F)); }
                    continue;
                }
                default: out=e; break;
            }
            if(o+1>=cap){ return -1; }
            dst[o++]=out;
        } else { if(o+1>=cap){ return -1; } dst[o++]=(char)c; }
    }
    dst[o]=0; return o;
}

/* Parse a Last.fm JSON response into out. Returns 0 if parseable (check out->api_error),
 * <0 if the JSON is unusable or out is NULL. */
static int lfm_parse_response(const char *json, size_t len, lfm_api_response_t *out){
    if(!out) return -1;
    memset(out, 0, sizeof *out);
    if(!json || len == 0) return -1;
    jsmn_parser jp; jsmntok_t tk[96];
    jsmn_init(&jp);
    int nt = jsmn_parse(&jp, json, len, tk, 96);
    if(nt < 1 || tk[0].type != JSMN_OBJECT) return -1;
    /* error? (numeric primitive) */
    int ei = lfm_object_get(json, len, tk, nt, 0, "error");
    if(ei >= 0){
        out->api_error = -1;
        if(tk[ei].type==JSMN_PRIMITIVE && tk[ei].start>=0 && tk[ei].end>tk[ei].start && (size_t)tk[ei].end<=len){
            int bl=tk[ei].end-tk[ei].start; char b[16];
            if(bl>0 && bl<(int)sizeof b){ memcpy(b, json+tk[ei].start, bl); b[bl]=0;
                errno=0; char *end; long v=strtol(b,&end,10);
                if(end!=b && *end==0 && errno==0 && v>0 && v<100000) out->api_error=(int)v; }
        }
        int mi = lfm_object_get(json, len, tk, nt, 0, "message");
        if(mi >= 0) lfm_copy_str(json, len, &tk[mi], out->message, sizeof out->message);
        return 0;
    }
    /* token? (string) */
    int ti = lfm_object_get(json, len, tk, nt, 0, "token");
    if(ti >= 0){ if(lfm_copy_str(json, len, &tk[ti], out->token, sizeof out->token) <= 0) return -1; return 0; }
    /* session { key, name } ? */
    int si = lfm_object_get(json, len, tk, nt, 0, "session");
    if(si >= 0 && tk[si].type == JSMN_OBJECT){
        int ki = lfm_object_get(json, len, tk, nt, si, "key");
        int ni = lfm_object_get(json, len, tk, nt, si, "name");
        if(ki < 0 || lfm_copy_str(json, len, &tk[ki], out->session_key, sizeof out->session_key) <= 0) return -1;
        if(ni >= 0) lfm_copy_str(json, len, &tk[ni], out->username, sizeof out->username);
        return 0;
    }
    /* scrobble ack? A real track.scrobble reply has a top-level "scrobbles" object (accepted OR
     * ignored - either way Last.fm processed it, so the queue entry is done). Its ABSENCE means we
     * must NOT treat the reply as a scrobble success (e.g. an HTTP-error "{}" body via
     * --content-on-error would otherwise pop a still-unsent scrobble). */
    { int spi = lfm_object_get(json, len, tk, nt, 0, "scrobbles");
      if(spi >= 0 && tk[spi].type == JSMN_OBJECT) out->scrobble_ack = 1; }   /* require an OBJECT, not {"scrobbles":null} */
    return 0;   /* parseable; scrobble_ack set above iff it was a real ack */
}

/* ===================== [auth / api calls] ============================== */

/* Sign + POST + parse one API call. Returns 0 if a response was parsed (inspect
 * out->api_error), <0 on build/transport/parse failure. */
static int lfm_api_call(const lfm_param_t *base, size_t n, const char *secret, lfm_api_response_t *out){
    char *body=NULL; size_t blen=0;
    if(lfm_signed_form(base, n, secret, &body, &blen) != 0) return -1;
    lfm_http_result_t r;
    lfm_http_err_t e = lfm_http_post(body, blen, &r);
    lfm_wipe(body, blen); free(body);
    if(e != LFM_HTTP_OK) return -2;
    int pr = lfm_parse_response(r.body, r.body_len, out);
    lfm_http_result_free(&r);
    return pr;
}
static int lfm_auth_get_token(const char *api_key, const char *secret, lfm_api_response_t *out){
    lfm_param_t p[] = { {"api_key",api_key}, {"method","auth.getToken"} };
    return lfm_api_call(p, 2, secret, out);
}
static int lfm_auth_get_session(const char *api_key, const char *secret, const char *token, lfm_api_response_t *out){
    lfm_param_t p[] = { {"api_key",api_key}, {"method","auth.getSession"}, {"token",token} };
    return lfm_api_call(p, 3, secret, out);
}

/* ===================== [engine: config + worker + watch + queue] ======= */
#ifndef LFM_SELFTEST
#include <pthread.h>
#include <stdatomic.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include "config.h"
#include "lastfm.h"

#define LFM_CFG_API   "lastfm_api_key"
#define LFM_CFG_SEC   "lastfm_secret"
#define LFM_CFG_SK    "lastfm_sk"
#define LFM_CFG_USER  "lastfm_user"
#define LFM_CFG_EN    "lastfm_enabled"
#define LFM_QUEUE     "/usr/data/lastfm.queue"
#define LFM_QUEUE_TMP "/usr/data/lastfm.queue.tmp"
#define LFM_QUEUE_MAX 2000                 /* cap the offline backlog */

enum { JOB_GET_TOKEN=1, JOB_GET_SESSION, JOB_NOWPLAYING, JOB_SCROBBLE };

typedef struct {
    int kind; uint32_t gen;
    char api_key[48], secret[48], sk[48];
    char token[64];
    char artist[160], track[IPC_TITLE_CAPACITY], album[160];
    long duration_s, timestamp;
} lfm_job_t;

typedef struct { int kind; uint32_t gen; int ok; int transport_err; lfm_api_response_t api; } lfm_result_t;

/* worker/result handoff (the ONLY cross-thread state) */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int      g_inflight, g_result_ready;
static lfm_result_t g_result;

/* main-thread-only state */
static uint32_t g_gen = 1;
static char g_api_key[48], g_secret[48], g_sk[48], g_user[128];
static int  g_have_creds, g_connected, g_enabled, g_inited;
static int  g_auth_state; static char g_auth_token[64]; static char g_auth_url[360];
static int  g_auth_want_token;              /* getToken requested; started when the worker is idle */
static int64_t g_auth_next_ms, g_auth_deadline_ms, g_auth_started_ms, g_backoff_ms;
/* watcher candidate */
static char g_cur_artist[160], g_cur_track[IPC_TITLE_CAPACITY], g_cur_album[160];
static char g_cur_path[TRACK_PATH_MAX];   /* file path: part of track identity so consecutive distinct files with the
                                * SAME title+artist each scrobble separately (not merged into one) */
static long g_cur_dur_ms, g_cur_lastpos_ms, g_cur_listened_ms, g_cur_start_unix;
static int  g_np_pending, g_scrobbled, g_have_cur, g_title_pending;
int ui_is_playing(void);   /* main.c: authoritative normalized play state (raw st->state is unreliable) */

static int lfm_busy(void){ pthread_mutex_lock(&g_mu); int b=g_inflight||g_result_ready; pthread_mutex_unlock(&g_mu); return b; }

/* ---- the API calls the worker performs (build params + sign+post+parse) ---- */
static int lfm_do_nowplaying(const lfm_job_t *j, lfm_api_response_t *out){
    char dur[16]; snprintf(dur,sizeof dur,"%ld", j->duration_s>0?j->duration_s:0);
    lfm_param_t p[7]; int n=0;
    p[n++]=(lfm_param_t){"api_key",j->api_key};
    p[n++]=(lfm_param_t){"artist", j->artist};
    p[n++]=(lfm_param_t){"method","track.updateNowPlaying"};
    p[n++]=(lfm_param_t){"sk",     j->sk};
    p[n++]=(lfm_param_t){"track",  j->track};
    if(j->album[0])    p[n++]=(lfm_param_t){"album",   j->album};
    if(j->duration_s>0)p[n++]=(lfm_param_t){"duration",dur};
    return lfm_api_call(p,(size_t)n,j->secret,out);
}
static int lfm_do_scrobble(const lfm_job_t *j, lfm_api_response_t *out){
    char dur[16], ts[16];
    snprintf(dur,sizeof dur,"%ld", j->duration_s>0?j->duration_s:0);
    snprintf(ts, sizeof ts, "%ld", j->timestamp);
    lfm_param_t p[8]; int n=0;
    p[n++]=(lfm_param_t){"api_key",  j->api_key};
    p[n++]=(lfm_param_t){"artist",   j->artist};
    p[n++]=(lfm_param_t){"method",   "track.scrobble"};
    p[n++]=(lfm_param_t){"sk",       j->sk};
    p[n++]=(lfm_param_t){"timestamp",ts};
    p[n++]=(lfm_param_t){"track",    j->track};
    if(j->album[0])    p[n++]=(lfm_param_t){"album",   j->album};
    if(j->duration_s>0)p[n++]=(lfm_param_t){"duration",dur};
    return lfm_api_call(p,(size_t)n,j->secret,out);
}

static void *lfm_worker(void *arg){
    lfm_job_t *j = arg;
    lfm_result_t res; memset(&res,0,sizeof res); res.kind=j->kind; res.gen=j->gen;
    lfm_api_response_t ar; memset(&ar,0,sizeof ar); int rc=-1;
    switch(j->kind){
        case JOB_GET_TOKEN:   rc=lfm_auth_get_token(j->api_key,j->secret,&ar); break;
        case JOB_GET_SESSION: rc=lfm_auth_get_session(j->api_key,j->secret,j->token,&ar); break;
        case JOB_NOWPLAYING:  rc=lfm_do_nowplaying(j,&ar); break;
        case JOB_SCROBBLE:    rc=lfm_do_scrobble(j,&ar); break;
    }
    if(rc==0){ res.ok=1; res.api=ar; } else res.transport_err=1;
    pthread_mutex_lock(&g_mu);
    if(!g_result_ready){ g_result=res; g_result_ready=1; }
    g_inflight=0;
    pthread_mutex_unlock(&g_mu);
    lfm_wipe(j,sizeof *j); free(j);        /* job held secret + session key */
    return NULL;
}

/* alloc + populate a job with the current cred snapshot (main thread) */
static lfm_job_t *lfm_new_job(int kind){
    lfm_job_t *j = calloc(1,sizeof *j); if(!j) return NULL;
    j->kind=kind; j->gen=g_gen;
    snprintf(j->api_key,sizeof j->api_key,"%s",g_api_key);
    snprintf(j->secret ,sizeof j->secret ,"%s",g_secret);
    snprintf(j->sk     ,sizeof j->sk     ,"%s",g_sk);
    return j;
}
static int lfm_start_job(lfm_job_t *j){
    if(!j) return -1;
    pthread_mutex_lock(&g_mu);
    if(g_inflight||g_result_ready){ pthread_mutex_unlock(&g_mu); lfm_wipe(j,sizeof*j); free(j); return -1; }
    g_inflight=1;
    pthread_mutex_unlock(&g_mu);
    pthread_t th;
    if(pthread_create(&th,NULL,lfm_worker,j)!=0){
        pthread_mutex_lock(&g_mu); g_inflight=0; pthread_mutex_unlock(&g_mu);
        lfm_wipe(j,sizeof*j); free(j); return -1;
    }
    pthread_detach(th);
    return 0;
}

/* ---- offline queue: one scrobble per line "ts\tdur\tartist\ttrack\talbum" ---- */
static void lfm_q_san(char *dst, const char *src, int cap){
    int o=0; for(int i=0; src[i] && o<cap-1; i++){ char c=src[i]; if(c=='\t'||c=='\n'||c=='\r') c=' '; dst[o++]=c; } dst[o]=0;
}
/* Line count of the queue file, cached after a clean read (UI thread only); -1 = read the file. */
static int g_qcount = -1, g_qopen;   /* g_qopen: file ends in an unterminated line */
int lastfm_queue_count(void){
    if(g_qcount>=0) return g_qcount;
    FILE *f=fopen(LFM_QUEUE,"r"); if(!f){ if(errno==ENOENT){ g_qcount=0; g_qopen=0; } return 0; }
    int n=0, c, last=0;
    while((c=fgetc(f))!=EOF){ last=c; if(c=='\n') n++; }
    int ok=!ferror(f);
    fclose(f);
    if(last!=0 && last!='\n') n++;       /* final line without a trailing newline */
    if(ok){ g_qcount=n; g_qopen=last!=0 && last!='\n'; }
    return n;
}
/* 1=appended, 0=intentionally dropped (cap/empty), -1=I/O error (caller should retry) */
static int lfm_queue_append(const char *artist,const char *track,const char *album,long dur_s,long ts){
    if(lastfm_queue_count() >= LFM_QUEUE_MAX) return 0;   /* cap: drop, but don't retry-loop */
    char a[160],t[IPC_TITLE_CAPACITY],al[160]; lfm_q_san(a,artist,sizeof a); lfm_q_san(t,track,sizeof t); lfm_q_san(al,album,sizeof al);
    if(!a[0]||!t[0]) return 0;
    FILE *f=fopen(LFM_QUEUE,"a"); if(!f) return -1;
    int rc = fprintf(f,"%ld\t%ld\t%s\t%s\t%s\n", ts, dur_s>0?dur_s:0, a, t, al);
    if(fclose(f)!=0 || rc<0){ g_qcount=-1; return -1; }   /* a partial row may be on disk: recount */
    if(g_qcount>=0){ if(g_qopen) g_qopen=0; else g_qcount++; }   /* one '\n'-terminated row */
    return 1;
}
/* read the oldest queued scrobble into j (0 ok, -1 empty) */
static int lfm_queue_peek(lfm_job_t *j){
    FILE *f=fopen(LFM_QUEUE,"r"); if(!f) return -1;
    char line[IPC_TITLE_CAPACITY + 2*160 + 64]; if(!fgets(line,sizeof line,f)){ fclose(f); return -1; }
    fclose(f);
    char *nl=strpbrk(line,"\r\n"); if(nl)*nl=0;
    /* ts \t dur \t artist \t track \t album */
    char *p=line, *ts=p; char *f2=strchr(p,'\t'); if(!f2) return -1; *f2=0; p=f2+1;
    char *dur=p; f2=strchr(p,'\t'); if(!f2) return -1; *f2=0; p=f2+1;
    char *ar=p;  f2=strchr(p,'\t'); if(!f2) return -1; *f2=0; p=f2+1;
    char *tr=p;  f2=strchr(p,'\t'); char *al="";
    if(f2){ *f2=0; al=f2+1; }
    j->timestamp=strtol(ts,NULL,10); j->duration_s=strtol(dur,NULL,10);
    snprintf(j->artist,sizeof j->artist,"%s",ar);
    snprintf(j->track ,sizeof j->track ,"%s",tr);
    snprintf(j->album ,sizeof j->album ,"%s",al);
    if(!j->artist[0]||!j->track[0]) return -1;
    return 0;
}
/* drop the oldest queued scrobble (rewrite without line 1) */
static void lfm_queue_pop(void){
    FILE *f=fopen(LFM_QUEUE,"r"); if(!f){ g_qcount=-1; return; }   /* unreadable: recount from the file */
    FILE *o=fopen(LFM_QUEUE_TMP,"w"); if(!o){ fclose(f); return; }
    char line[IPC_TITLE_CAPACITY + 2*160 + 64]; int first=1, ok=1, n=0, last=0;
    while(fgets(line,sizeof line,f)){
        if(first){ first=0; continue; }
        if(fputs(line,o)==EOF){ ok=0; break; }
        size_t l=strlen(line); if(l){ last=line[l-1]; n+=last=='\n'; }   /* count what was written */
    }
    if(ferror(f)) ok=0;
    fclose(f);
    if(fflush(o)!=0) ok=0;
    if(ok && fsync(fileno(o))!=0) ok=0;   /* durably flush tmp BEFORE the rename (a power cut mid-rename could otherwise drop the backlog) */
    if(fclose(o)!=0) ok=0;   /* separate stmts: a || short-circuit would skip fclose on fflush failure (leak) */
    /* Only replace the real queue if the rewrite completed. On any write/close failure, unlink the
     * partial tmp and leave the queue intact - a partial rename would DROP still-unsent scrobbles. */
    if(ok){
        if(rename(LFM_QUEUE_TMP, LFM_QUEUE)!=0) unlink(LFM_QUEUE_TMP);
        else { g_qopen=last!=0 && last!='\n'; g_qcount=n+g_qopen;
               int dfd=open("/usr/data", O_RDONLY|O_DIRECTORY); if(dfd>=0){ fsync(dfd); close(dfd); } }  /* persist the rename */
    }
    else unlink(LFM_QUEUE_TMP);
}

/* ---- result handling + drivers (main thread) ---- */
/* Retain the latest requested account state across config rollback.
 * Retry on the main thread, including while disabled or disconnected. */
enum { LFM_SAVE_SESSION = 1, LFM_SAVE_CREDS = 2 };
static unsigned g_cfg_pending;
static int64_t g_cfg_retry_ms;

static void lfm_save_config(void){
    if(!g_cfg_pending) return;
    cfg_begin();
    int bad = 0;
    if(g_cfg_pending & LFM_SAVE_CREDS){
        bad |= cfg_set_str_deferred(LFM_CFG_API, g_api_key) < 0;
        bad |= cfg_set_str_deferred(LFM_CFG_SEC, g_secret) < 0;
    }
    bad |= cfg_set_str_deferred(LFM_CFG_SK, g_sk) < 0;
    bad |= cfg_set_str_deferred(LFM_CFG_USER, g_user) < 0;
    int rc = cfg_commit();
    if(rc == 0 && !bad){
        g_cfg_pending = 0;
    } else {
        g_cfg_retry_ms = lfm_now_ms() + 30000;
    }
}

static void lfm_request_config_save(unsigned fields){
    g_cfg_pending |= fields;
    lfm_save_config();
}

static void lfm_build_auth_url(void){
    char ka[144], ta[144];
    if(lfm_pct(g_api_key,ka,sizeof ka)<0 || lfm_pct(g_auth_token,ta,sizeof ta)<0){ g_auth_url[0]=0; return; }
    snprintf(g_auth_url,sizeof g_auth_url,"https://www.last.fm/api/auth/?api_key=%s&token=%s", ka, ta);
}
/* Last.fm error 9 = "invalid session key". Wipe the session from memory AND config so a restart
 * (which treats a nonempty stored key as connected) doesn't resurrect the dead session - forces a
 * clean re-auth instead of silently reporting "connected" until the next request fails. */
static void lfm_clear_session(void){
    g_sk[0]=0; g_user[0]=0; g_connected=0;
    lfm_request_config_save(LFM_SAVE_SESSION);
}
static void lfm_handle_result(const lfm_result_t *r){
    if((r->kind==JOB_GET_TOKEN||r->kind==JOB_GET_SESSION) && r->gen!=g_gen) return;  /* stale auth */
    switch(r->kind){
    case JOB_GET_TOKEN:
        if(r->ok && !r->api.api_error && r->api.token[0]){
            snprintf(g_auth_token,sizeof g_auth_token,"%s",r->api.token);
            lfm_build_auth_url();
            g_auth_state=LFM_AUTH_WAIT; g_auth_next_ms=lfm_now_ms()+3000;
        } else g_auth_state=LFM_AUTH_ERR;
        break;
    case JOB_GET_SESSION:
        if(r->ok && !r->api.api_error && r->api.session_key[0]){
            if(strlen(r->api.session_key) < sizeof g_sk){
                snprintf(g_sk,sizeof g_sk,"%s",r->api.session_key);
                snprintf(g_user,sizeof g_user,"%s",r->api.username);
                lfm_request_config_save(LFM_SAVE_SESSION);
                g_connected=1; g_auth_state=LFM_AUTH_OK;
            } else g_auth_state=LFM_AUTH_ERR;      /* absurd key length -> refuse */
        } else if(r->ok && r->api.api_error==14){  /* pending user authorization: keep polling */
            /* stay WAIT; drive_auth reschedules */
        } else if(r->transport_err){
            /* transient: stay WAIT, retry next interval */
        } else g_auth_state=LFM_AUTH_ERR;          /* real API error */
        break;
    case JOB_NOWPLAYING:
        if(r->ok && r->api.api_error==9) lfm_clear_session();   /* invalid session -> wipe it, force re-auth (not just in-memory) */
        break;
    case JOB_SCROBBLE:
        if(r->ok && r->api.scrobble_ack){ lfm_queue_pop(); }                         /* Last.fm confirmed (ack) -> drop */
        else if(r->ok && r->api.api_error==9){ lfm_clear_session(); }               /* bad session -> wipe + re-auth (keep the queue) */
        else if(r->ok && (r->api.api_error==11||r->api.api_error==16||r->api.api_error==29)){
            g_backoff_ms = lfm_now_ms()+60000;                                       /* service down / rate-limit -> keep, back off */
        }
        else if(r->ok && r->api.api_error>0){ lfm_queue_pop(); }                     /* permanent bad entry -> drop, don't block */
        else { g_backoff_ms = lfm_now_ms()+30000; }                                  /* no ack / unrecognized / transport fail -> KEEP + back off
                                                                                      * (was: popped on any !api_error -> silent scrobble loss on a "{}" reply) */
        break;
    }
}
static void lfm_drive_token(void){          /* start getToken once the worker is free (decoupled from auth_begin) */
    if(!g_auth_want_token) return;
    if(lfm_now_ms() > g_auth_deadline_ms){ g_auth_want_token=0; g_auth_state=LFM_AUTH_ERR; return; }
    if(lfm_busy()) return;
    if(lfm_start_job(lfm_new_job(JOB_GET_TOKEN))==0) g_auth_want_token=0;
}
static void lfm_drive_auth(void){
    if(g_auth_state!=LFM_AUTH_WAIT) return;
    int64_t now=lfm_now_ms();
    if(now>g_auth_deadline_ms){ g_auth_state=LFM_AUTH_ERR; return; }
    if(now<g_auth_next_ms || lfm_busy()) return;
    lfm_job_t *j=lfm_new_job(JOB_GET_SESSION); if(!j) return;
    snprintf(j->token,sizeof j->token,"%s",g_auth_token);
    if(lfm_start_job(j)==0){
        int64_t elapsed = now - g_auth_started_ms;
        g_auth_next_ms = now + (elapsed<60000 ? 3000 : 5000);
    }
}
static void lfm_drive_jobs(void){
    if(!g_enabled||!g_connected) return;
    if(lfm_now_ms()<g_backoff_ms || lfm_busy()) return;
    if(!g_title_pending && g_np_pending && g_have_cur && ui_is_playing()){   /* a candidate started while playing but dispatch was delayed
                                                          * (backoff/busy) then paused -> do not send Now Playing while paused */
        lfm_job_t *j=lfm_new_job(JOB_NOWPLAYING); if(!j) return;
        snprintf(j->artist,sizeof j->artist,"%s",g_cur_artist);
        snprintf(j->track ,sizeof j->track ,"%s",g_cur_track);
        snprintf(j->album ,sizeof j->album ,"%s",g_cur_album);
        j->duration_s = g_cur_dur_ms>0 ? g_cur_dur_ms/1000 : 0;
        if(lfm_start_job(j)==0) g_np_pending=0;
        return;
    }
    if(lastfm_queue_count()>0){
        lfm_job_t *j=lfm_new_job(JOB_SCROBBLE); if(!j) return;
        if(lfm_queue_peek(j)==0){ lfm_start_job(j); }
        else { lfm_queue_pop(); lfm_wipe(j,sizeof*j); free(j); }   /* corrupt head -> drop */
    }
}

/* ===================== [setup server] (per-user cred provisioning) ====== */
/* A tiny, transient HTTP server: the user scans a QR of http://<wlan0-ip>:PORT/<token>,
 * their phone loads a page served here, they paste their OWN Last.fm api_key + secret,
 * and it lands in config. Runs only during setup; LAN-only; unguessable path token. */
#define LFM_SETUP_PORT 8080

static pthread_mutex_t g_setup_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_setup_th; static int g_setup_th_live;
static atomic_int g_setup_run; static int g_setup_fd = -1;   /* atomic: worker reads, main writes */
static int  g_setup_got, g_setup_received;
static char g_setup_api[48], g_setup_sec[48];
static char g_setup_url[80], g_setup_token[20];

static int lfm_wlan_ip(char *out, int cap){
    int s = socket(AF_INET, SOCK_DGRAM, 0); if(s<0) return -1;
    struct ifreq ifr; memset(&ifr,0,sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "wlan0");
    int r = ioctl(s, SIOCGIFADDR, &ifr); close(s);
    if(r<0) return -1;
    struct sockaddr_in *sin = (struct sockaddr_in*)&ifr.ifr_addr;
    const char *p = inet_ntoa(sin->sin_addr); if(!p) return -1;
    snprintf(out, cap, "%s", p);
    return 0;
}
static int lfm_gen_token(char *out, int cap){        /* 0 ok, -1 if no secure randomness */
    unsigned char b[8]; int f=open("/dev/urandom",O_RDONLY); if(f<0) return -1;
    int n=0; while(n<8){ int r=read(f,b+n,8-n); if(r>0) n+=r; else if(r<0 && errno==EINTR) continue; else break; }
    close(f);
    if(n!=8) return -1;                              /* fail setup rather than use a guessable token */
    static const char hx[]="0123456789abcdef"; int o=0;
    for(int i=0;i<8 && o<cap-2;i++){ out[o++]=hx[b[i]>>4]; out[o++]=hx[b[i]&15]; }
    out[o]=0; return 0;
}
static int lfm_hexv(char c){ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return -1; }
static void lfm_urldec(char *s){                 /* in place */
    char *o=s;
    for(char *p=s; *p; p++){
        if(*p=='+') *o++=' ';
        else if(*p=='%' && lfm_hexv(p[1])>=0 && lfm_hexv(p[2])>=0){ *o++=(char)((lfm_hexv(p[1])<<4)|lfm_hexv(p[2])); p+=2; }
        else *o++=*p;
    }
    *o=0;
}
/* extract urlencoded field `key` from a form body into out (decoded, trimmed).
 * Returns 0 ok, -1 not found or raw value too long (reject rather than silently truncate). */
static int lfm_form_field(const char *body, const char *key, char *out, int cap){
    out[0]=0; size_t kl=strlen(key); const char *p=body;
    while(p && *p){
        if(!strncmp(p,key,kl) && p[kl]=='='){
            const char *v=p+kl+1; const char *e=strchr(v,'&');
            size_t n=e?(size_t)(e-v):strlen(v);
            if(n>=(size_t)cap) return -1;            /* too long -> reject, don't corrupt via truncation */
            memcpy(out,v,n); out[n]=0; lfm_urldec(out);
            for(int i=(int)strlen(out)-1;i>=0 && (out[i]==' '||out[i]=='\r'||out[i]=='\n');i--) out[i]=0;  /* rtrim */
            return 0;
        }
        p=strchr(p,'&'); if(p)p++;
    }
    return -1;
}
static void lfm_send(int c, const char *status, const char *body){
    char hdr[160]; int bl=(int)strlen(body);
    int h=snprintf(hdr,sizeof hdr,
        "HTTP/1.0 %s\r\nContent-Type:text/html; charset=utf-8\r\nContent-Length:%d\r\nConnection:close\r\n\r\n",status,bl);
    send(c,hdr,h,MSG_NOSIGNAL); send(c,body,bl,MSG_NOSIGNAL);
}
/* split so the form's action carries the token: action='/<token>/save' (a relative
 * 'save' would resolve to /save and 404). % in the CSS are literal here (not a format). */
static const char LFM_PAGE_A[] =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>diskOS Last.fm setup</title>"
    "<style>body{font:16px/1.5 -apple-system,system-ui,sans-serif;max-width:30em;margin:2em auto;padding:0 1em;color:#eee;background:#111}"
    "h2{color:#d51007}input{width:100%;box-sizing:border-box;padding:.6em;margin:.3em 0 1em;font-size:1em;border-radius:8px;border:1px solid #444;background:#222;color:#fff}"
    "button{width:100%;padding:.8em;font-size:1em;border:0;border-radius:8px;background:#d51007;color:#fff;font-weight:600}"
    "a{color:#d51007}ol{padding-left:1.2em}li{margin:.4em 0}</style>"
    "<h2>Connect this player to Last.fm</h2>"
    "<p style='background:#2a1a1a;border:1px solid #d51007;border-radius:8px;padding:.6em .8em;font-size:.85em;color:#f2c2c2'>"
    "This page is served over your local Wi-Fi <b>without encryption</b>. Only enter your keys on a network you trust, "
    "and remove the app's credentials from your Last.fm account if you stop using this player.</p>"
    "<ol><li>Open <a href='https://www.last.fm/api/account/create' target=_blank>last.fm/api/account/create</a> and sign in.</li>"
    "<li>Fill in any <b>Application name</b> (e.g. diskOS) and description; leave callback/homepage blank. Submit.</li>"
    "<li>Copy your <b>API key</b> and <b>Shared secret</b> and paste them below.</li></ol>"
    "<form method=POST action='/";
static const char LFM_PAGE_B[] =
    "/save'>"
    "<label>API key</label><input name=api_key autocomplete=off autocapitalize=off spellcheck=false required>"
    "<label>Shared secret</label><input name=secret autocomplete=off autocapitalize=off spellcheck=false required>"
    "<button type=submit>Save to player</button></form>"
    "<p style='color:#888;font-size:.85em'>These stay on your player only. Nothing is shipped in the app.</p>";
static const char LFM_OK_PAGE[] =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<style>body{font:16px/1.5 system-ui,sans-serif;max-width:30em;margin:3em auto;padding:0 1em;color:#eee;background:#111;text-align:center}h2{color:#3c3}</style>"
    "<h2>&#10003; Saved</h2><p>Back on the player, approve access to finish connecting.</p>";
static void lfm_setup_send_form(int c){
    char page[2400];
    int nn=snprintf(page,sizeof page,"%s%s%s",LFM_PAGE_A,g_setup_token,LFM_PAGE_B);
    if(nn<0 || nn>=(int)sizeof page){ lfm_send(c,"500 Internal Server Error","err"); return; }
    lfm_send(c,"200 OK",page);
}

static void lfm_setup_handle(int c){
    struct timeval tv={2,0};
    setsockopt(c,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
    setsockopt(c,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof tv);
    char buf[4096]; int n=0; int64_t deadline=lfm_now_ms()+8000;
    for(;;){
        if(!g_setup_run || lfm_now_ms()>deadline) break;      /* total deadline + stop flag: no slow-client hang */
        if(n >= (int)sizeof buf-1) break;
        int r=recv(c,buf+n,sizeof buf-1-n,0); if(r<=0) break; n+=r; buf[n]=0;
        char *he=strstr(buf,"\r\n\r\n"); if(!he) continue;
        int hlen=(int)(he-buf); char sv=buf[hlen]; buf[hlen]=0;   /* search Content-Length only within headers */
        char *cl=strcasestr(buf,"\r\ncontent-length:");
        long len = cl ? strtol(cl+17,NULL,10) : -1;
        buf[hlen]=sv;
        if(len>0){ long have=n-(hlen+4); if(have<len) continue; } /* wait for the full declared body */
        break;
    }
    buf[n<(int)sizeof buf?n:(int)sizeof buf-1]=0;
    char method[8]={0}, path[300]={0};
    if(sscanf(buf,"%7s %299s",method,path)!=2){ lfm_wipe(buf,sizeof buf); return; }
    char base[24]; snprintf(base,sizeof base,"/%s",g_setup_token); size_t bl=strlen(base);
    if(strncmp(path,base,bl)!=0){ lfm_send(c,"404 Not Found","not found"); lfm_wipe(buf,sizeof buf); return; }
    const char *rest = path+bl;                                   /* "" | "/" | "/save" */
    if(!strcmp(method,"POST") && !strcmp(rest,"/save")){
        char *body=strstr(buf,"\r\n\r\n"); body=body?body+4:(char*)"";
        char api[48]={0}, sec[48]={0};
        int ok = (lfm_form_field(body,"api_key",api,sizeof api)==0) &&
                 (lfm_form_field(body,"secret", sec,sizeof sec)==0);
        if(ok && api[0] && sec[0] && g_setup_run){
            pthread_mutex_lock(&g_setup_mu);
            lfm_wipe(g_setup_api,sizeof g_setup_api); lfm_wipe(g_setup_sec,sizeof g_setup_sec);  /* no stale tail */
            snprintf(g_setup_api,sizeof g_setup_api,"%s",api); snprintf(g_setup_sec,sizeof g_setup_sec,"%s",sec);
            g_setup_got=1; pthread_mutex_unlock(&g_setup_mu);
            lfm_send(c,"200 OK",LFM_OK_PAGE);
        } else lfm_setup_send_form(c);
        lfm_wipe(api,sizeof api); lfm_wipe(sec,sizeof sec); lfm_wipe(buf,sizeof buf);
        return;
    }
    if(!strcmp(method,"GET") && (!strcmp(rest,"") || !strcmp(rest,"/"))) lfm_setup_send_form(c);
    else lfm_send(c,"404 Not Found","not found");
    lfm_wipe(buf,sizeof buf);
}
static void *lfm_setup_worker(void *arg){
    (void)arg;
    while(g_setup_run){
        struct pollfd pf={g_setup_fd,POLLIN,0};
        int pr=poll(&pf,1,500); if(pr<=0) continue;
        int cfd=accept(g_setup_fd,NULL,NULL); if(cfd<0) continue;
        lfm_setup_handle(cfd);
        close(cfd);
    }
    return NULL;
}
int lastfm_setup_start(void){
    if(g_setup_th_live) return 0;
    char ip[24]; if(lfm_wlan_ip(ip,sizeof ip)!=0) return -1;               /* needs Wi-Fi */
    if(lfm_gen_token(g_setup_token,sizeof g_setup_token)!=0) return -1;    /* require secure randomness */
    int fd=socket(AF_INET,SOCK_STREAM,0); if(fd<0) return -1;
    int one=1; setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
    struct sockaddr_in a; memset(&a,0,sizeof a);
    a.sin_family=AF_INET; a.sin_port=htons(LFM_SETUP_PORT);
    a.sin_addr.s_addr=inet_addr(ip);                                       /* bind to wlan0 only, not all ifaces */
    if(a.sin_addr.s_addr==INADDR_NONE){ close(fd); return -1; }            /* refuse rather than bind everywhere */
    if(bind(fd,(struct sockaddr*)&a,sizeof a)!=0 || listen(fd,4)!=0){ close(fd); return -1; }
    fcntl(fd, F_SETFL, O_NONBLOCK);                                        /* accept() won't block after poll() */
    snprintf(g_setup_url,sizeof g_setup_url,"http://%s:%d/%s/",ip,LFM_SETUP_PORT,g_setup_token);  /* trailing slash */
    g_setup_fd=fd; g_setup_run=1; g_setup_got=0; g_setup_received=0;
    if(pthread_create(&g_setup_th,NULL,lfm_setup_worker,NULL)!=0){ close(fd); g_setup_fd=-1; g_setup_run=0; return -1; }
    g_setup_th_live=1;
    return 0;
}
void lastfm_setup_stop(void){
    if(!g_setup_th_live) return;
    g_setup_run=0;
    pthread_join(g_setup_th,NULL);
    if(g_setup_fd>=0){ close(g_setup_fd); g_setup_fd=-1; }
    g_setup_th_live=0;
    pthread_mutex_lock(&g_setup_mu);
    lfm_wipe(g_setup_api,sizeof g_setup_api); lfm_wipe(g_setup_sec,sizeof g_setup_sec); g_setup_got=0;
    pthread_mutex_unlock(&g_setup_mu);
}
const char *lastfm_setup_url(void){ return g_setup_th_live ? g_setup_url : ""; }
int lastfm_setup_received(void){ return g_setup_received; }
/* consume creds handed over by the server thread (main thread) */
static void lfm_setup_consume(void){
    char api[48]={0}, sec[48]={0}; int got=0;
    pthread_mutex_lock(&g_setup_mu);
    if(g_setup_got){ snprintf(api,sizeof api,"%s",g_setup_api); snprintf(sec,sizeof sec,"%s",g_setup_sec);
        lfm_wipe(g_setup_api,sizeof g_setup_api); lfm_wipe(g_setup_sec,sizeof g_setup_sec); g_setup_got=0; got=1; }
    pthread_mutex_unlock(&g_setup_mu);
    if(got){
        lastfm_set_credentials(api,sec);
        g_setup_received=1;
        lastfm_auth_begin();                 /* creds in -> immediately start the account-auth (QR) step */
        lfm_wipe(api,sizeof api); lfm_wipe(sec,sizeof sec);
    }
}

/* ---- public API ---- */
void lastfm_init(void){
    if(g_inited) return;
    g_inited=1;
    g_enabled = cfg_get_int(LFM_CFG_EN,0);
    snprintf(g_api_key,sizeof g_api_key,"%s",cfg_get_str(LFM_CFG_API,""));
    snprintf(g_secret ,sizeof g_secret ,"%s",cfg_get_str(LFM_CFG_SEC,""));
    snprintf(g_sk     ,sizeof g_sk     ,"%s",cfg_get_str(LFM_CFG_SK,""));
    snprintf(g_user   ,sizeof g_user   ,"%s",cfg_get_str(LFM_CFG_USER,""));
    g_have_creds = g_api_key[0] && g_secret[0];
    g_connected  = g_have_creds && g_sk[0];
    g_auth_state = LFM_AUTH_IDLE;
}
void lastfm_poll(void){
    if(!g_inited) return;
    lfm_setup_consume();                    /* pick up creds posted by the setup server thread */
    int have=0; lfm_result_t res;
    pthread_mutex_lock(&g_mu);
    if(g_result_ready){ res=g_result; g_result_ready=0; have=1; }
    pthread_mutex_unlock(&g_mu);
    if(have){ lfm_handle_result(&res); lfm_wipe(&res,sizeof res); }
    if(g_cfg_pending && lfm_now_ms() >= g_cfg_retry_ms)
        lfm_save_config();
    lfm_drive_token();
    lfm_drive_auth();
    lfm_drive_jobs();
}
/* enqueue the current candidate iff it earned a scrobble (>30s track, listened>=min(dur/2,4min))
 * and hasn't been queued yet. On a queue I/O error, leave g_scrobbled clear to retry next tick. */
static void lfm_finalize_candidate(void){
    if(!g_have_cur || g_scrobbled || g_cur_dur_ms <= 30000) return;
    long thresh = g_cur_dur_ms/2 < 240000 ? g_cur_dur_ms/2 : 240000;
    if(g_cur_listened_ms >= thresh &&
       lfm_queue_append(g_cur_artist,g_cur_track,g_cur_album,g_cur_dur_ms/1000,g_cur_start_unix) >= 0)
        g_scrobbled=1;
}
static void lfm_start_candidate(const track_state_t *st){
    snprintf(g_cur_track ,sizeof g_cur_track ,"%s",st->title);
    snprintf(g_cur_artist,sizeof g_cur_artist,"%s",st->artist);
    snprintf(g_cur_album ,sizeof g_cur_album ,"%s",st->album);
    snprintf(g_cur_path  ,sizeof g_cur_path  ,"%s",st->path);
    g_cur_dur_ms=st->duration_ms; g_cur_lastpos_ms=st->position_ms; g_cur_listened_ms=0;
    g_cur_start_unix=(long)time(NULL);
    g_np_pending=1; g_scrobbled=0; g_have_cur=1;
}
/* 15c: the watcher's input is a VIEW (the Cast track while casting, else the raw local state) plus an explicit play state and the source. While
 * casting `playing` is the receiver-confirmed flag (PLAYING on the track's own media), so a failed / excluded / never-played Cast track is never a
 * candidate and never sends Now Playing. A CAST<->LOCAL switch on the SAME path is one listening: the candidate (listened, scrobbled flag) carries over. */
static int g_cur_cast;
void lastfm_watch_view(const track_state_t *st, int playing, int cast){
    if(!g_inited || !g_enabled || !g_connected || !st) return;
    /* st->state is raw A2 metadata, UNRELIABLE for play-state (reports 0 while playing -- see ipc.h),
     * so use the main loop's authoritative g_playing. A track with no real playback (boot-seeded pause,
     * a paused seek) must never advertise Now Playing. */
    /* Audiobooks are not music: never advertise Now Playing or scrobble them. Treated as "no track" so
     * the outgoing music candidate still finalizes and a book never becomes a candidate. */
    g_title_pending = st->have_track && !st->title_ready;
    if(g_title_pending) return; /* Hold the outgoing candidate; pending is not a stop. */
    int have = st->have_track && st->title[0] && st->artist[0] && !mdb_is_book_path(st->path);
    if(!have){                          /* truly stopped / no track / a book: finalize + forget so a replay counts as new */
        lfm_finalize_candidate();
        g_have_cur=0; g_cur_track[0]=0; g_cur_artist[0]=0;
        return;
    }
    if(!playing){                       /* track loaded but PAUSED: hold the current candidate (don't reset it,
                                         * don't start one, don't send Now Playing) until playback resumes. */
        if(g_have_cur) g_cur_lastpos_ms = st->position_ms;   /* absorb paused seeks so the jump isn't counted as listening on resume */
        return;
    }
    if(g_have_cur && (!cast != !g_cur_cast) && !strcmp(st->path,g_cur_path)){   /* source switch mid-track: same listening, never a new candidate */
        g_cur_cast = cast;
        snprintf(g_cur_track,sizeof g_cur_track,"%s",st->title); snprintf(g_cur_artist,sizeof g_cur_artist,"%s",st->artist);
        g_cur_lastpos_ms = st->position_ms;      /* the receiver's clock is not the local player's: the jump is not listening */
        return;
    }
    if(!g_have_cur || strcmp(st->path,g_cur_path) || strcmp(st->title,g_cur_track) || strcmp(st->artist,g_cur_artist)
       || (cast && g_cur_cast && cast != g_cur_cast)){   /* a new receiver media on the same path (repeat-one, replay) is a new play */
        lfm_finalize_candidate();       /* the outgoing track, if it earned it */
        lfm_start_candidate(st); g_cur_cast = cast;
        return;
    }
    /* same track: accumulate REAL playback only (ignore seeks / jumps > 4s / rewinds) */
    long pos = st->position_ms, delta = pos - g_cur_lastpos_ms;
    if(delta > 0 && delta <= 4000) g_cur_listened_ms += delta;
    g_cur_lastpos_ms = pos;
    if(st->duration_ms > 0) g_cur_dur_ms = st->duration_ms;
    if(delta < -3000 && pos < 3000){    /* same-track restart (rewound to ~0) -> count as a new play */
        lfm_finalize_candidate();
        g_cur_listened_ms=0; g_cur_start_unix=(long)time(NULL); g_np_pending=1; g_scrobbled=0;
    }
    if(!g_scrobbled && g_cur_dur_ms > 30000){
        long thresh = g_cur_dur_ms/2 < 240000 ? g_cur_dur_ms/2 : 240000;
        if(g_cur_listened_ms >= thresh &&
           lfm_queue_append(g_cur_artist,g_cur_track,g_cur_album,g_cur_dur_ms/1000,g_cur_start_unix) >= 0)
            g_scrobbled=1;
    }
}
void lastfm_watch(const track_state_t *st){ lastfm_watch_view(st, ui_is_playing(), 0); }   /* the local watcher, unchanged */
int lastfm_enabled(void){ return g_enabled; }
int lastfm_connected(void){ return g_connected; }
int lastfm_has_creds(void){ return g_have_creds; }
const char *lastfm_username(void){ return g_connected ? g_user : ""; }
void lastfm_set_enabled(int on){ g_enabled = on?1:0; cfg_set_int(LFM_CFG_EN,g_enabled); }
void lastfm_set_credentials(const char *api_key,const char *secret){
    if(!api_key||!secret) return;
    snprintf(g_api_key,sizeof g_api_key,"%s",api_key);
    snprintf(g_secret ,sizeof g_secret ,"%s",secret);
    /* one atomic write for all four credential keys: a mid-run save failure can no longer leave
     * api/secret persisted without clearing the stale session (or the reverse). */
    g_have_creds = g_api_key[0] && g_secret[0];
    g_sk[0]=0; g_user[0]=0; g_connected=0;                 /* new app -> re-auth */
    lfm_request_config_save(LFM_SAVE_CREDS | LFM_SAVE_SESSION);
    g_gen++; g_auth_state=LFM_AUTH_IDLE;
}
void lastfm_logout(void){
    g_sk[0]=0; g_user[0]=0; g_connected=0;
    lfm_request_config_save(LFM_SAVE_SESSION);
    g_gen++; g_auth_state=LFM_AUTH_IDLE;
}
void lastfm_auth_begin(void){
    if(!g_have_creds){ g_auth_state=LFM_AUTH_ERR; return; }
    g_gen++;                                                /* invalidate any prior auth result */
    g_auth_token[0]=0; g_auth_url[0]=0;
    g_auth_started_ms=lfm_now_ms(); g_auth_deadline_ms=g_auth_started_ms+600000;
    g_auth_state=LFM_AUTH_TOKEN; g_auth_want_token=1;       /* lfm_drive_token() starts it when idle */
}
int lastfm_auth_state(void){ return g_auth_state; }
const char *lastfm_auth_url(void){ return g_auth_url; }
#endif /* !LFM_SELFTEST */

/* ===================== [self-test] ===================================== */
#ifdef LFM_SELFTEST
/* Build with: cc -DLFM_SELFTEST -D_GNU_SOURCE lastfm.c md5.c -o /tmp/lfmtest */
int main(void){
    /* 1) percent-encoder */
    char e[128]; lfm_pct("a b&c=d/e:f", e, sizeof e);
    printf("pct: %s\n", e);   /* expect a%20b%26c%3Dd%2Fe%3Af */
    /* 2) api_sig - Last.fm's own doc example is not published, so verify md5 wiring:
     * params {api_key=xx, method=auth.getToken}, secret "sss"
     * source (sorted): "api_keyxxmethodauth.getTokensss" */
    lfm_param_t p[] = { {"method","auth.getToken"}, {"api_key","xx"} };
    char sig[33]; lfm_make_sig(p, 2, "sss", sig);
    char want[33]; md5_hex("api_keyxxmethodauth.getTokensss", 31, want);
    printf("sig: %s  %s\n", sig, strcmp(sig,want)?"MISMATCH":"ok(matches manual md5)");
    /* 3) signed form */
    char *body; size_t bl;
    if(lfm_signed_form(p, 2, "sss", &body, &bl)==0){ printf("form: %s\n", body); free(body); }
    /* 4) parser */
    lfm_api_response_t r;
    const char *j1 = "{\"token\":\"abc123DEF\"}";
    lfm_parse_response(j1, strlen(j1), &r);
    printf("parse token: '%s' %s\n", r.token, strcmp(r.token,"abc123DEF")?"FAIL":"ok");
    const char *j2 = "{\"session\":{\"name\":\"testuser\",\"key\":\"sk_9f8e\",\"subscriber\":0}}";
    lfm_parse_response(j2, strlen(j2), &r);
    printf("parse session: key='%s' user='%s' %s\n", r.session_key, r.username,
           (!strcmp(r.session_key,"sk_9f8e") && !strcmp(r.username,"testuser"))?"ok":"FAIL");
    const char *j3 = "{\"error\":14,\"message\":\"This token has not been authorized\"}";
    lfm_parse_response(j3, strlen(j3), &r);
    printf("parse error: code=%d msg='%s' %s\n", r.api_error, r.message, r.api_error==14?"ok":"FAIL");
    const char *j4 = "{\"error\":10,\"message\":\"Invalid API key\"}";
    lfm_parse_response(j4, strlen(j4), &r);
    printf("parse error2: code=%d %s\n", r.api_error, r.api_error==10?"ok":"FAIL");
    /* 5) escapes + unicode: \/ \n, BMP U+00E9 (C3 A9), surrogate pair U+1F600 (F0 9F 98 80) */
    const char *j5 = "{\"session\":{\"name\":\"a\\/b\\nc\\u00e9\\uD83D\\uDE00\",\"key\":\"K1\"}}";
    lfm_parse_response(j5, strlen(j5), &r);
    const char *want5 = "a/b\nc\xc3\xa9\xf0\x9f\x98\x80";
    printf("parse esc/unicode: user='%s' %s\n", r.username, strcmp(r.username,want5)?"FAIL":"ok");
    /* embedded NUL must not truncate silently -> dropped */
    const char *j6 = "{\"token\":\"a\\u0000b\"}";
    lfm_parse_response(j6, strlen(j6), &r);
    printf("parse NUL-drop: token='%s' (len must be 2) %s\n", r.token, strlen(r.token)==2?"ok":"FAIL");
    return 0;
}
#endif
