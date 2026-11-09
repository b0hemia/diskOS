/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Online album art (stock V2.57 "Online cover", 064b - internal RE notes).
 *
 * Stock's player fetches a cover from the iTunes Search API on EVERY track change (even one with its own cover),
 * decodes and resizes it inside the player process while music plays, overwrites the current cover file and keeps
 * nothing. diskOS does the same search itself instead, so the player is never involved:
 *  - only for a song whose own cover (embedded or a cover file beside it) is missing - the art worker says so;
 *  - on this module's own thread at the lowest CPU and disk priority; the wget children inherit both;
 *  - pictures live on the SD card (NETART_DIR, beside the cover cache), written under the SD write guard, only with
 *    1 GB free, capped at 64 MB (oldest first, never one saved in the last 10 minutes);
 *  - an album search result is kept for the whole album (artist + album); when the store has no such album, a song
 *    search result is kept for that song only (artist + title) - never a song's cover for a whole album;
 *  - a result only counts when its first artist equals ours and its album (or title) equals ours, ignoring case,
 *    punctuation and bracketed/"feat." parts: a wrong cover is worse than none;
 *  - a miss is remembered per key for 30 days (<key>.none); a network failure backs off 5 minutes;
 *  - an image is read through a pipe and cut off at 2 MB; an interrupted or non-JPEG/PNG download is thrown away.
 * Same storefront as stock (the player's LANGUAGE indexes stock's country table) and the same size (the 100x100
 * artwork URL becomes 400x400). The picture is decoded later by the normal cover pipeline (the art helper, a separate
 * process), only while the setting is on - it is never put in the permanent cover cache, so turning it off takes
 * effect at once. */
#include "netart.h"
#include "md5.h"
#include "musicdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/syscall.h>

int  sd_io_begin(void);  void sd_io_end(void);         /* SD read admission (sdio.c) */
int  sd_write_begin(void); void sd_write_end(void);     /* SD write ownership (main.c) */

#ifndef NETART_SD_ROOT
#define NETART_SD_ROOT "/tmp/sdcard/.diskos"
#endif
#define NETART_MAX_BYTES (64LL * 1024 * 1024)
#ifndef NETART_MIN_FREE
#define NETART_MIN_FREE  (1024ULL * 1024 * 1024)
#endif
#define NETART_KEEP_SECS 600                            /* never prune a picture saved in the last 10 minutes */
#define NETART_MISS_SECS (30L * 24 * 3600)
#define NETART_BACKOFF_SECS 300
#define NETART_MAX_RESP  (256 * 1024)
#define NETART_MAX_IMG   (2 * 1024 * 1024)

typedef struct { char track[TRACK_PATH_MAX], artist[256], album[256], title[256]; unsigned gen; } na_job_t;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_file_mu = PTHREAD_MUTEX_INITIALIZER;   /* prune vs touch: a picture in use is never removed */
static na_job_t g_pending;               /* the latest request (latest wins)          (guarded) */
static int g_has_pending, g_running;     /*                                           (guarded) */
static unsigned g_gen;                   /* bumped by netart_cancel                   (guarded) */
static char g_ready[TRACK_PATH_MAX];                /* a picture became available for this song   (guarded) */
static time_t g_offline_until;           /* network failed: no tries before this        (guarded) */

static const char *const COUNTRY[] = { "CN", "TW", "US", "FR", "RU", "ES", "PL", "CZ", "DE", "JP", "KR", "TH", "IT" };
const char *netart_country(int language){
    return (language >= 0 && language < (int)(sizeof COUNTRY / sizeof COUNTRY[0])) ? COUNTRY[language] : "US";
}

/* ---- names ---- */
/* the first credited artist: "A, B" / "A & B" / "A feat. B" -> "A" */
static void first_artist(const char *a, char *out, int cap){
    snprintf(out, cap, "%s", a ? a : "");
    static const char *const SEP[] = { ",", " & ", " feat", " ft.", " x ", ";", " and ", " with " };
    for(unsigned i = 0; i < sizeof SEP / sizeof SEP[0]; i++){ char *p = strcasestr(out, SEP[i]); if(p && p != out) *p = 0; }
}
/* compare form: bracketed parts, " - ..." suffixes and "feat." tails dropped; ASCII letters/digits lower-cased;
 * other ASCII dropped; non-ASCII bytes kept (so Hindi/CJK names still compare) */
static void norm(const char *s, char *out, int cap){
    char t[512]; snprintf(t, sizeof t, "%s", s ? s : "");
    char *p;
    if((p = strstr(t, " - "))) *p = 0;
    if((p = strcasestr(t, " feat"))) *p = 0;
    if((p = strcasestr(t, " ft."))) *p = 0;
    int o = 0, depth = 0;
    for(const char *q = t; *q && o < cap - 1; q++){
        unsigned char c = (unsigned char)*q;
        if(c == '(' || c == '['){ depth++; continue; }
        if(c == ')' || c == ']'){ if(depth) depth--; continue; }
        if(depth) continue;
        if(c >= 0x80) out[o++] = (char)c;
        else if(isalnum(c)) out[o++] = (char)tolower(c);
    }
    out[o] = 0;
}
static int same(const char *a, const char *b){ char x[256], y[256]; norm(a, x, sizeof x); norm(b, y, sizeof y); return x[0] && !strcmp(x, y); }
static int same_artist(const char *a, const char *b){ char x[256], y[256]; first_artist(a, x, sizeof x); first_artist(b, y, sizeof y); return same(x, y); }
static int known(const char *s){ return s && s[0] && strcasecmp(s, "Unknown album") && strcasecmp(s, "Unknown artist"); }

/* key for an album picture (by_album) or a song's own picture */
static int key_for(const char *artist, const char *what, int by_album, char *hex){
    char a[256], n[256], w[256], k[800];
    first_artist(artist, a, sizeof a); norm(a, n, sizeof n); norm(what, w, sizeof w);
    if(!known(artist) || !n[0] || !w[0] || (by_album && !known(what))) return 0;
    snprintf(k, sizeof k, "%c\x1f%s\x1f%s", by_album ? 'a' : 't', n, w);
    md5_hex(k, strlen(k), hex);
    return 1;
}
static int file_ok(const char *p){ struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 64; }
static int recent_miss(const char *hex){
    char p[300]; struct stat st;
    snprintf(p, sizeof p, "%s/%s.none", NETART_DIR, hex);
    return stat(p, &st) == 0 && time(NULL) - st.st_mtime < NETART_MISS_SECS;
}
/* (unguarded) the saved picture for this song: the album's, else its own */
static int have_unlocked(const char *artist, const char *album, const char *title, char *out, int cap){
    char hex[33];
    if(key_for(artist, album, 1, hex) && snprintf(out, cap, "%s/%s.jpg", NETART_DIR, hex) < cap && file_ok(out)) return 1;
    if(key_for(artist, title, 0, hex) && snprintf(out, cap, "%s/%s.jpg", NETART_DIR, hex) < cap && file_ok(out)) return 1;
    return 0;
}
int netart_have(const char *artist, const char *album, const char *title, char *out, int cap){
    if(!sd_io_begin()) return 0;
    int r = have_unlocked(artist, album, title, out, cap);
    sd_io_end();
    return r;
}

/* ---- JSON (the iTunes Search reply) ---- */
static int hexv(char c){ return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int jspace(char c){ return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
/* the string value of "key" between p and end ("key" : "value", any spacing); \uXXXX (BMP) decoded to UTF-8 */
static void json_str(const char *p, const char *end, const char *key, char *out, int cap){
    char pat[48]; int pl = snprintf(pat, sizeof pat, "\"%s\"", key);
    out[0] = 0;
    for(const char *q = p; q && q < end; q++){
        q = strstr(q, pat);
        if(!q || q >= end) return;
        const char *v = q + pl;
        while(v < end && jspace(*v)) v++;
        if(v >= end || *v != ':') continue;
        v++;
        while(v < end && jspace(*v)) v++;
        if(v >= end || *v != '"') continue;
        v++;
        int o = 0;
        while(v < end && *v && *v != '"' && o < cap - 4){
            if(*v == '\\' && v + 1 < end){
                v++;
                if(*v == 'u' && v + 4 < end){
                    int a = hexv(v[1]), b = hexv(v[2]), c = hexv(v[3]), d = hexv(v[4]);
                    if(a < 0 || b < 0 || c < 0 || d < 0){ out[o++] = '?'; v++; continue; }
                    unsigned cp = (unsigned)(a << 12 | b << 8 | c << 4 | d);
                    if(cp < 0x80) out[o++] = (char)cp;
                    else if(cp < 0x800){ out[o++] = (char)(0xC0 | cp >> 6); out[o++] = (char)(0x80 | (cp & 63)); }
                    else if(cp < 0xD800 || cp > 0xDFFF){ out[o++] = (char)(0xE0 | cp >> 12); out[o++] = (char)(0x80 | (cp >> 6 & 63)); out[o++] = (char)(0x80 | (cp & 63)); }
                    v += 5; continue;
                }
                out[o++] = *v++; continue;
            }
            out[o++] = *v++;
        }
        out[o] = 0;
        return;
    }
}

/* %-encode for a URL query, inside single shell quotes (no quote ever survives) */
static void urlenc(const char *s, char *out, int cap){
    static const char *hex = "0123456789ABCDEF";
    int o = 0;
    for(; *s && o < cap - 4; s++){
        unsigned char c = (unsigned char)*s;
        if(isalnum(c) || c == '-' || c == '_' || c == '.') out[o++] = (char)c;
        else if(c == ' ') out[o++] = '+';
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    out[o] = 0;
}
/* an artwork URL we will fetch: https on Apple's image host, and only URL-safe characters (it goes into a shell) */
static int safe_art_url(const char *u){
    if(strncmp(u, "https://", 8) != 0) return 0;
    const char *h = u + 8, *slash = strchr(h, '/');
    if(!slash) return 0;
    static const char SUF[] = ".mzstatic.com";           /* Apple's artwork host, e.g. is1-ssl.mzstatic.com */
    size_t hl = (size_t)(slash - h), sl = sizeof SUF - 1;
    if(hl <= sl || memcmp(h + hl - sl, SUF, sl) != 0) return 0;
    for(const char *p = u; *p; p++)
        if(!(isalnum((unsigned char)*p) || strchr(":/._-%?=&~+", *p))) return 0;
    return 1;
}

/* ---- the work ---- */
static void set_lowest_priority(void){
    long tid = syscall(SYS_gettid);
    setpriority(PRIO_PROCESS, (id_t)tid, 19);                            /* CPU: this thread; children inherit */
#ifdef SYS_ioprio_set
    syscall(SYS_ioprio_set, 1 /* IOPRIO_WHO_PROCESS */, (int)tid, 3 << 13 /* IOPRIO_CLASS_IDLE */);   /* disk: idle */
#endif
}
static void make_dirs(void){ mkdir(NETART_SD_ROOT, 0755); mkdir(NETART_DIR, 0755); }
typedef struct { time_t t; long long sz; char name[80]; } na_ent_t;
static int ent_cmp(const void *a, const void *b){ time_t x = ((const na_ent_t *)a)->t, y = ((const na_ent_t *)b)->t; return (x > y) - (x < y); }
/* (under the SD write guard) pictures over NETART_MAX_BYTES go oldest first, never one saved in the last
 * NETART_KEEP_SECS; stale miss notes go */
static void prune_locked(void);
static void prune(void){ pthread_mutex_lock(&g_file_mu); prune_locked(); pthread_mutex_unlock(&g_file_mu); }
static void prune_locked(void){
    DIR *d = opendir(NETART_DIR);
    if(!d) return;
    na_ent_t *v = NULL; int n = 0, cap = 0; long long total = 0; time_t now = time(NULL);
    struct dirent *e;
    while((e = readdir(d))){
        size_t l = strlen(e->d_name);
        if(l < 6 || l >= sizeof v->name) continue;
        char p[300]; struct stat st;
        if(snprintf(p, sizeof p, "%s/%s", NETART_DIR, e->d_name) >= (int)sizeof p || stat(p, &st)) continue;
        if(!strcmp(e->d_name + l - 5, ".none")){ if(now - st.st_mtime >= NETART_MISS_SECS) unlink(p); continue; }
        if(strcmp(e->d_name + l - 4, ".jpg")) continue;
        if(n == cap){ int nc = cap ? cap * 2 : 256; void *m = realloc(v, (size_t)nc * sizeof *v); if(!m) break; v = m; cap = nc; }
        v[n].t = st.st_mtime; v[n].sz = (long long)st.st_size; memcpy(v[n].name, e->d_name, l + 1); total += v[n].sz; n++;
    }
    closedir(d);
    if(total > NETART_MAX_BYTES){
        qsort(v, (size_t)n, sizeof *v, ent_cmp);
        for(int i = 0; i < n && total > NETART_MAX_BYTES; i++){
            if(now - v[i].t < NETART_KEEP_SECS) break;                       /* the rest are newer still */
            char p[300]; snprintf(p, sizeof p, "%s/%s", NETART_DIR, v[i].name);
            if(unlink(p) == 0) total -= v[i].sz;
        }
    }
    free(v);
}
/* one iTunes search: 1 = a matching artwork URL in `url`, 0 = the store has nothing suitable, -1 = network trouble */
static int search(const na_job_t *j, int by_album, const char *country, char *url, int cap){
    char a1[256], term[600], q[1800], cmd[2100];
    first_artist(j->artist, a1, sizeof a1);
    snprintf(term, sizeof term, "%s %s", a1, by_album ? j->album : j->title);
    urlenc(term, q, sizeof q);
    snprintf(cmd, sizeof cmd, "wget -qO- -T 8 'https://itunes.apple.com/search?term=%s&media=music&entity=%s&limit=10&country=%s' 2>/dev/null",
             q, by_album ? "album" : "song", country);
    url[0] = 0;
    FILE *p = popen(cmd, "r");
    if(!p) return -1;
    char *resp = malloc(NETART_MAX_RESP + 1);
    if(!resp){ pclose(p); return -1; }
    size_t n = fread(resp, 1, NETART_MAX_RESP, p);
    resp[n] = 0;
    int st = pclose(p);
    if(st != 0 || n == 0 || n == NETART_MAX_RESP || !strstr(resp, "\"resultCount\"")){ free(resp); return -1; }   /* cut off / failed */
    const char *end = resp + n;
    for(const char *r = strstr(resp, "\"wrapperType\""); r && !url[0]; ){
        const char *next = strstr(r + 1, "\"wrapperType\"");
        const char *rend = next ? next : end;
        char an[256], cn[256], tn[256], au[600];
        json_str(r, rend, "artistName", an, sizeof an);
        json_str(r, rend, "collectionName", cn, sizeof cn);
        json_str(r, rend, "trackName", tn, sizeof tn);
        json_str(r, rend, "artworkUrl100", au, sizeof au);
        if(au[0] && same_artist(an, a1) && (by_album ? same(cn, j->album) : same(tn, j->title))){
            char *sz = strstr(au, "100x100");      /* stock's size: 400x400 */
            int w = sz ? snprintf(url, cap, "%.*s400x400%s", (int)(sz - au), au, sz + 7) : snprintf(url, cap, "%s", au);
            if(w < 0 || w >= cap || !safe_art_url(url)) url[0] = 0;   /* never fetch a cut-off or unsafe URL */
        }
        r = next;
    }
    free(resp);
    return url[0] ? 1 : 0;
}
static int enough_free(void){
    struct statvfs sv;
    return statvfs(NETART_DIR, &sv) == 0 && (unsigned long long)sv.f_bavail * sv.f_frsize >= NETART_MIN_FREE;
}
/* download url as <hex>.jpg: 1 saved, 0 not a picture / too big, -1 network or card trouble. The download goes to
 * memory first (cut at NETART_MAX_IMG); the SD card is only touched for the short save, so a slow network never
 * holds the SD write guard (a USB export waits for writers). */
static int download(const char *url, const char *hex){
    char tmp[300], dst[300], cmd[800];
    snprintf(tmp, sizeof tmp, "%s/%s.part", NETART_DIR, hex);
    snprintf(dst, sizeof dst, "%s/%s.jpg", NETART_DIR, hex);
    snprintf(cmd, sizeof cmd, "wget -qO- -T 8 '%s' 2>/dev/null", url);
    unsigned char *img = malloc(NETART_MAX_IMG + 1);
    if(!img) return -1;
    FILE *in = popen(cmd, "r");
    if(!in){ free(img); return -1; }
    size_t got = 0, k; int over = 0;
    while((k = fread(img + got, 1, NETART_MAX_IMG + 1 - got, in)) > 0){
        got += k;
        if(got > NETART_MAX_IMG){ over = 1; break; }                   /* never keeps more than 2 MB */
    }
    int rc = pclose(in);                                                 /* after an early stop wget gets SIGPIPE */
    int ret;
    int pic = got >= 64 && ((img[0] == 0xFF && img[1] == 0xD8) || (img[0] == 0x89 && img[1] == 'P' && img[2] == 'N' && img[3] == 'G'));
    if(over) ret = 0;                                                    /* too big: not a cover */
    else if(rc != 0) ret = -1;                                           /* interrupted: try later */
    else if(!pic) ret = 0;
    else if(!sd_write_begin()) ret = -1;
    else {
        make_dirs();
        FILE *out = enough_free() ? fopen(tmp, "wb") : NULL;
        int ok = out && fwrite(img, 1, got, out) == got;
        if(out && fclose(out) != 0) ok = 0;
        if(ok && rename(tmp, dst) == 0){ utimes(dst, NULL); prune(); ret = 1; }   /* stamp: saved now */
        else { unlink(tmp); ret = -1; }
        sd_write_end();
    }
    free(img);
    return ret;
}
static void note_miss(const char *hex){
    if(!sd_write_begin()) return;
    make_dirs();
    char p[300]; snprintf(p, sizeof p, "%s/%s.none", NETART_DIR, hex);
    FILE *m = fopen(p, "w"); if(m) fclose(m);
    sd_write_end();
}
static int miss_known(const char *hex){ if(!sd_io_begin()) return -1; int m = recent_miss(hex); sd_io_end(); return m; }
/* 1 = a picture for this song now exists, 0 = nothing suitable, -1 = network or card trouble */
static int lookup(const na_job_t *j, const char *country){
    char url[600], hex[33];
    int r, m;
    if(key_for(j->artist, j->album, 1, hex)){                          /* the album's picture first */
        if((m = miss_known(hex)) < 0) return -1;
        if(!m){
            r = search(j, 1, country, url, sizeof url);
            if(r < 0) return -1;
            if(r > 0 && (r = download(url, hex)) != 0) return r;
            note_miss(hex);                                          /* no such album in the store (or a bad picture) */
        }
    }
    if(!key_for(j->artist, j->title, 0, hex)) return 0;                /* then the song's own */
    if((m = miss_known(hex)) != 0) return m < 0 ? -1 : 0;
    r = search(j, 0, country, url, sizeof url);
    if(r < 0) return -1;
    if(r > 0 && (r = download(url, hex)) != 0) return r;
    note_miss(hex);
    return 0;
}
static void *worker(void *arg){
    (void)arg;
    set_lowest_priority();
    const char *country = netart_country(mdb_sysconfig_language());   /* read here, never on the UI thread */
    for(;;){
        na_job_t j;
        pthread_mutex_lock(&g_mu);
        if(!g_has_pending || time(NULL) < g_offline_until){ g_has_pending = 0; g_running = 0; pthread_mutex_unlock(&g_mu); return NULL; }
        j = g_pending; g_has_pending = 0;
        pthread_mutex_unlock(&g_mu);
        char path[300];
        int r = netart_have(j.artist, j.album, j.title, path, sizeof path) ? 1 : lookup(&j, country);
        pthread_mutex_lock(&g_mu);
        if(j.gen == g_gen){                                  /* not cancelled meanwhile */
            if(r > 0) snprintf(g_ready, sizeof g_ready, "%s", j.track);
            else if(r < 0) g_offline_until = time(NULL) + NETART_BACKOFF_SECS;
        }
        pthread_mutex_unlock(&g_mu);
    }
}
void netart_request(const char *track, const char *artist, const char *album, const char *title){
    char ha[33], ht[33];
    int ka = key_for(artist, album, 1, ha), kt = key_for(artist, title, 0, ht);
    if(!track || !track[0] || (!ka && !kt)) return;
    int launch = 0;
    pthread_mutex_lock(&g_mu);
    if(time(NULL) >= g_offline_until){
        snprintf(g_pending.track, sizeof g_pending.track, "%s", track);
        snprintf(g_pending.artist, sizeof g_pending.artist, "%s", artist ? artist : "");
        snprintf(g_pending.album, sizeof g_pending.album, "%s", album ? album : "");
        snprintf(g_pending.title, sizeof g_pending.title, "%s", title ? title : "");
        g_pending.gen = g_gen;
        g_has_pending = 1;
        if(!g_running){ g_running = 1; launch = 1; }
    }
    pthread_mutex_unlock(&g_mu);
    if(launch){
        pthread_t th;
        if(pthread_create(&th, NULL, worker, NULL) == 0) pthread_detach(th);
        else { pthread_mutex_lock(&g_mu); g_running = 0; g_has_pending = 0; pthread_mutex_unlock(&g_mu); }
    }
}
void netart_cancel(void){
    pthread_mutex_lock(&g_mu);
    g_has_pending = 0; g_gen++; g_ready[0] = 0;
    pthread_mutex_unlock(&g_mu);
}
/* the art worker is about to decode `path`: mark it used now, so the prune (which never removes a picture younger
 * than NETART_KEEP_SECS) leaves it alone */
int netart_touch(const char *path){        /* 1 = stamped and still there */
    if(!sd_write_begin()) return 0;
    pthread_mutex_lock(&g_file_mu);          /* never between a prune's snapshot and its unlink */
    int ok = utimes(path, NULL) == 0;
    pthread_mutex_unlock(&g_file_mu);
    sd_write_end();
    return ok;
}
/* the art helper could not decode `path`: remove it and remember its key as a miss, so it is never offered again */
void netart_reject(const char *path){
    const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
    size_t l = strlen(b);
    if(l != 36 || strcmp(b + 32, ".jpg")) return;                       /* only our own <md5>.jpg files */
    char hex[33]; memcpy(hex, b, 32); hex[32] = 0;
    char p[300]; snprintf(p, sizeof p, "%s/%s.jpg", NETART_DIR, hex);
    if(!sd_write_begin()) return;
    unlink(p);
    sd_write_end();
    note_miss(hex);
}
int netart_take_ready(char *track, int cap){
    int got = 0;
    pthread_mutex_lock(&g_mu);
    if(g_ready[0]){ snprintf(track, cap, "%s", g_ready); g_ready[0] = 0; got = 1; }
    pthread_mutex_unlock(&g_mu);
    return got;
}
