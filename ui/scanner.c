/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "sdio.h"
#include "ipc.h"
#include <stdatomic.h>
/* First-run / rescan music scanner for diskOS.
 *
 * The stock V2.09 scanner (tag 0622) is a no-op - it never populates song.db from the SD.
 * So diskOS scans itself: walk the SD for audio files, read their tags, and rebuild the
 * SONG table of /usr/data/fiio/db/song.db (the same DB the library UI reads). Playlists,
 * favourites and resume state (CUSTOM_PLAYLIST/PLAYLIST_INFO/MY_LOVE/MEMORY_PLAY) are keyed
 * by PATH and left intact, so a rescan doesn't lose them.
 *
 * Tag support: see is_audio() - MP3 (ID3v2 + ID3v1), FLAC/OGG (Vorbis comments), M4A/M4B (MP4 atoms), APE (APEv2),
 * AIFF (ID3 chunk, NAME/AUTH), DSF (ID3 block), WMA (ASF), AAC (leading ID3v2); WAV/DFF/DTS by filename. Anything
 * without usable tags falls back to the filename (minus extension) as the title.
 * External .cue sheets and SACD .iso images are expanded into one row per track, the way stock V2.57 writes them (see
 * cue_finish, iso_finish).
 *
 * Runs on a detached worker thread; progress is published under a mutex for the UI to poll.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <ctype.h>
#include <iconv.h>
#include <unistd.h>   /* usleep (used before the later include) */
#include <dirent.h>
#include <errno.h>
#include "audiodur.h"   /* exact song lengths for SONG.DURATION (header-authoritative only) */
#include <limits.h>
#include <pthread.h>
#include <sys/stat.h>
#include "sqlite3.h"
#include "scanner.h"
#include "musicdb.h"   /* DISKOS_AUDIOBOOKS gate: whether .m4b files are indexed */

#ifndef DB_PATH
#define DB_PATH   "/usr/data/fiio/db/song.db"
#endif
#ifndef SCAN_ROOT
#define SCAN_ROOT "/tmp/sdcard"
#endif
#define MAXPATH   1024
#define TAGLEN    256

/* ---- progress (published to the LVGL thread) ---- */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
/* Published by Settings on the main thread; workers never read the config store. */
static atomic_int g_old_tags;
static _Thread_local int g_decode_old_tags = -1;   /* -1 = non-scan reader */
void scanner_set_old_tags(int mode){
    atomic_store(&g_old_tags, mode >= OLD_TAGS_AUTO && mode <= OLD_TAGS_CYRILLIC ? mode : OLD_TAGS_AUTO);
}
static int g_active;          /* a scan is running */
static int g_done;            /* files inserted so far */
static int g_total;           /* files found (0 until the walk completes) */
static int g_finished_seq;    /* bumped when a scan finishes (UI edge-detect) */
static int g_committed_pending; /* a scan committed since the UI last took it; a start never clears it (under g_mu) */
static int g_no_sd;           /* last scan aborted because the SD wasn't mounted */
static int g_commit_decided;  /* worker passed the commit decision: a cancel is no longer accepted (under g_mu) */
#define OUT_NONE SCAN_OUT_NONE
#define OUT_COMMITTED SCAN_OUT_COMMITTED
#define OUT_CANCELLED SCAN_OUT_CANCELLED
#define OUT_FAILED SCAN_OUT_FAILED
#define OUT_NO_SD SCAN_OUT_NO_SD
static int g_outcome;         /* the finished scan's one terminal outcome (under g_mu) */
static int g_cancelled;       /* the user (not an SD revoke) asked to stop; under g_mu
                               * so the completion poll can say "Scan cancelled" */
static _Atomic int g_abort;   /* set from the UI thread to stop a running walk (SD revoked); atomic, not volatile: two threads */
static int g_scan_err;        /* worker-thread only: any I/O or DB-insert failure during the walk
                               * -> the rebuild is incomplete and must NOT commit over the library */
static int g_skipped_result; /* completed scan snapshot under g_mu */
static int g_skipped;         /* files skipped this scan (unstat-able junk / overlong path) - logged, not fatal */
static int g_unsupported;     /* audio-ish files present but not indexable (AAC/M4A/OGG/...) - drives a UI notice */
static int g_prune_blocked;   /* preservation infrastructure failed: retain globally */

int scanner_active(void){ pthread_mutex_lock(&g_mu); int a=g_active; pthread_mutex_unlock(&g_mu); return a; }
void scanner_progress(int *done, int *total){
    pthread_mutex_lock(&g_mu);
    if(done)*done=g_done; if(total)*total=g_total;
    pthread_mutex_unlock(&g_mu);
}
int scanner_take_finished(void){
    pthread_mutex_lock(&g_mu);
    static int last=0; int f = (g_finished_seq!=last); last=g_finished_seq;
    pthread_mutex_unlock(&g_mu);
    return f;
}
int scanner_take_committed(void){
    pthread_mutex_lock(&g_mu);
    int changed = g_committed_pending; g_committed_pending = 0;
    pthread_mutex_unlock(&g_mu);
    return changed;
}
/* 1 if the most recent scan did nothing because no SD was mounted (library was kept). */
int scanner_outcome(void){ pthread_mutex_lock(&g_mu); int v=g_outcome; pthread_mutex_unlock(&g_mu); return v; }
int scanner_no_sd(void){ pthread_mutex_lock(&g_mu); int v=g_no_sd; pthread_mutex_unlock(&g_mu); return v; }
int scanner_skipped(void){ pthread_mutex_lock(&g_mu); int v=g_skipped_result; pthread_mutex_unlock(&g_mu); return v; }
int scanner_unsupported(void){ pthread_mutex_lock(&g_mu); int v=g_unsupported; pthread_mutex_unlock(&g_mu); return v; }

/* ---- helpers ---- */
static void str_trim(char *s){
    char *p=s; while(*p==' '||*p=='\t') p++;
    if(p!=s) memmove(s,p,strlen(p)+1);
    int n=(int)strlen(s);
    while(n>0 && (unsigned char)s[n-1]<=' ') s[--n]=0;
}
/* NAME_CODE/TITLE_CODE/... : first 4 chars packed big-endian, 31-bit (matches the stock DB) */
static int code4(const char *s){
    char b[5]="\0\0\0\0"; int j=0;
    for(const char *p=s?s:""; *p && j<4; p++) b[j++]=(char)tolower((unsigned char)*p);
    unsigned v=0; for(int i=0;i<4;i++) v=(v<<8)|((unsigned char)b[i]);
    return (int)(v & 0x7fffffff);
}
static int has_ext(const char *name, const char *ext){
    size_t nl=strlen(name), el=strlen(ext);
    return nl>el && !strcasecmp(name+nl-el, ext);
}
/* Audio files diskOS indexes: stock V2.57's list plus Ogg Opus. Real tags from MP3 (ID3), FLAC and OGG (Vorbis comments),
 * M4A/M4B (MP4 atoms), APE (APEv2, ID3v1 fallback), AIFF (ID3 chunk, NAME/AUTH), DSF (ID3 block), WMA (ASF) and a
 * leading ID3v2 on AAC; WAV/DFF/DTS fall back to the filename. External .cue sheets become per-track rows after the
 * walk (cue_finish); SACD .iso images likewise become per-track rows (iso_finish). */
static int is_audio(const char *name){
    if(has_ext(name,".m4b")) return DISKOS_AUDIOBOOKS;   /* books indexed only when the audiobook feature is enabled */
    return has_ext(name,".mp3") || has_ext(name,".flac") || has_ext(name,".wav")
        || has_ext(name,".m4a")
        /* the rest of stock V2.57's scanner list (P257 extension table @0x6cb6ec..): the stock player decodes them */
        || has_ext(name,".aac") || has_ext(name,".ogg") || has_ext(name,".ape") || has_ext(name,".aif")
        || has_ext(name,".aiff")|| has_ext(name,".wma") || has_ext(name,".dsf") || has_ext(name,".dff")
        || has_ext(name,".dts") || has_ext(name,".opus") || has_ext(name,".oga");
}
/* Audio-ish files we do NOT index yet (no parser). Counted during the walk so the UI can tell a user
 * whose library is all AAC/ALAC/etc WHY it looks empty, instead of a bare "No music found". */
static int is_unsupported_audio(const char *name){
    /* ALAC extension and WavPack are not indexed. */
    return has_ext(name,".alac")
        || has_ext(name,".wv");
}

/* ---- text encoding -> UTF-8 (bounded) ---- */
static void put_u8(char **o, char *end, unsigned cp){
    char *p=*o;
    if(cp<0x80){ if(p<end) *p++=(char)cp; }
    else if(cp<0x800){ if(p+1<end){ *p++=(char)(0xC0|(cp>>6)); *p++=(char)(0x80|(cp&0x3F)); } }
    else { if(p+2<end){ *p++=(char)(0xE0|(cp>>12)); *p++=(char)(0x80|((cp>>6)&0x3F)); *p++=(char)(0x80|(cp&0x3F)); } }
    *o=p;
}
/* Auto is deliberately only a guess: three consecutive CP1251 letters, with a 0x98 veto.
 * Western and Cyrillic let the owner correct ambiguous legacy bytes. */
static int utf8_ok(const unsigned char *s, size_t n);
static int cp1251_letter(unsigned c){
    if(c >= 0xC0) return 1;                                   /* A..ya */
    switch(c){                                                /* Ukrainian, Belarusian, Serbian, Macedonian letters */
    case 0x80: case 0x81: case 0x83: case 0x8A: case 0x8C: case 0x8D: case 0x8E: case 0x8F: case 0x90: case 0x9A:
    case 0x9C: case 0x9D: case 0x9E: case 0x9F: case 0xA1: case 0xA2: case 0xA3: case 0xA5: case 0xA8: case 0xAA:
    case 0xAF: case 0xB2: case 0xB3: case 0xB4: case 0xB8: case 0xBA: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        return 1;
    }
    return 0;
}
static int looks_cp1251(const unsigned char *s, size_t n){
    int run = 0, best = 0;
    for(size_t i = 0; i < n; i++){
        if(s[i] == 0x98) return 0;
        run = cp1251_letter(s[i]) ? run + 1 : 0;
        if(run > best) best = run;
    }
    return best >= 3;
}
/* Legacy bytes only; valid UTF-8 and UTF-16 never reach this decoder. */
static void legacy_decode(const unsigned char *s, size_t n, char *out, int cap){
    size_t len = 0; while(len < n && s[len]) len++;
    if(cap <= 0) return;
    int mode = g_decode_old_tags >= 0 ? g_decode_old_tags : atomic_load(&g_old_tags);
    if(mode == OLD_TAGS_CYRILLIC || (mode == OLD_TAGS_AUTO && looks_cp1251(s, len))){
        iconv_t cd = iconv_open("UTF-8", "CP1251");
        if(cd != (iconv_t)-1){
            char *ip = (char *)s, *op = out; size_t il = len, ol = (size_t)cap - 1;
            size_t r;
            int err = 0;
            for(;;){
                r = iconv(cd, &ip, &il, &op, &ol);
                err = errno;
                if(r != (size_t)-1 || err != EILSEQ || mode != OLD_TAGS_CYRILLIC) break;
                /* Undefined CP1251 bytes become U+FFFD; keep the following text. */
                if(ol < 3){ err = E2BIG; break; }
                memcpy(op, "\xEF\xBF\xBD", 3); op += 3; ol -= 3;
                ip++; il--;
            }
            iconv_close(cd);
            if(r != (size_t)-1 || err == E2BIG){            /* all of it, or as much as fits (whole characters) */
                *op = 0; return;
            }
        }
    }
    char *o = out, *end = out + cap - 1;
    for(size_t i = 0; i < len; i++) put_u8(&o, end, s[i]);
    *o = 0;
}

/* Decode an ID3 text-frame body (enc byte already consumed by caller). enc: 0=Latin1,
 * 1=UTF-16 w/ BOM, 2=UTF-16BE, 3=UTF-8. Writes a NUL-terminated UTF-8 string into out. */
static void id3_decode(int enc, const unsigned char *in, int n, char *out, int cap){
    char *o=out, *end=out+cap-1;
    if(enc==0){                                  /* Latin-1 (or CP1251 that says Latin-1) */
        legacy_decode(in, n > 0 ? (size_t)n : 0, out, cap); str_trim(out); return;
    } else if(enc==3){                           /* UTF-8 (copy, stop at NUL); not valid UTF-8 = legacy 8-bit text */
        int len=0; while(len<n && in[len]) len++;
        int mode = g_decode_old_tags >= 0 ? g_decode_old_tags : atomic_load(&g_old_tags);
        if(mode != OLD_TAGS_WESTERN && !utf8_ok(in, (size_t)len)){ legacy_decode(in, (size_t)len, out, cap); str_trim(out); return; }
        for(int i=0;i<len;i++){ if(o<end) *o++=(char)in[i]; }
    } else {                                     /* UTF-16 (1=BOM, 2=BE) */
        int be = (enc==2), i=0;
        if(enc==1 && n>=2){ if(in[0]==0xFF && in[1]==0xFE) be=0; else if(in[0]==0xFE && in[1]==0xFF) be=1; i=2; }
        for(; i+1<n; i+=2){
            unsigned u = be ? (in[i]<<8|in[i+1]) : (in[i+1]<<8|in[i]);
            if(u==0) break;
            if(u>=0xD800 && u<=0xDBFF && i+3<n){  /* surrogate pair */
                unsigned lo = be ? (in[i+2]<<8|in[i+3]) : (in[i+3]<<8|in[i+2]);
                if(lo>=0xDC00 && lo<=0xDFFF){ u=0x10000+((u-0xD800)<<10)+(lo-0xDC00); i+=2;
                    char *p=o; if(p+3<end){ *p++=(char)(0xF0|(u>>18)); *p++=(char)(0x80|((u>>12)&0x3F)); *p++=(char)(0x80|((u>>6)&0x3F)); *p++=(char)(0x80|(u&0x3F)); } o=p; continue; }
            }
            if(u>=0xD800 && u<=0xDFFF) continue;  /* lone surrogate */
            put_u8(&o,end,u);
        }
    }
    *o=0; str_trim(out);
}
/* "(13)" or "13" style ID3 numeric genre -> leave as-is if it's plain text; we don't map the
 * 148 legacy IDs (rarely used in modern tags), just strip a leading "(N)" wrapper. */
static void genre_clean(char *g){
    if(g[0]=='(' ){ char *e=strchr(g,')'); if(e && e[1]) memmove(g, e+1, strlen(e+1)+1); }
    str_trim(g);
}

/* unsigned shifts: p[0]<<24 with the byte >=128 would shift into the int sign bit (signed-overflow UB). */
static uint32_t be32(const unsigned char *p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|(uint32_t)p[3]; }
static uint32_t synch32(const unsigned char *p){ return ((uint32_t)p[0]<<21)|((uint32_t)p[1]<<14)|((uint32_t)p[2]<<7)|(uint32_t)p[3]; }

/* leading positive integer of an ID3/Vorbis track/disc value ("5" or "5/12" -> 5, "01" -> 1). Returns 0
 * for junk, overflow, or a non-positive value, so a bogus tag can never store garbage or clobber a good
 * existing value (the UPDATE keeps the old value when this is 0). */
static int tag_int(const char *s){
    if(!s) return 0;
    errno = 0;
    char *end;
    long v = strtol(s, &end, 10);
    if(end == s || errno == ERANGE || v <= 0 || v > INT_MAX) return 0;
    return (int)v;
}

/* The fields a tag reader fills. A reader that streams its tag (reading only the parts it uses) saves them first and
 * puts them back if a read fails part way, so a failed read leaves them untouched, as the old whole-tag read did. */
typedef struct { char title[TAGLEN], artist[TAGLEN], album[TAGLEN], genre[TAGLEN], album_artist[TAGLEN]; int track, disc; } tag_fields;
static void tags_save(tag_fields *s, const char *title,const char *artist,const char *album,const char *genre,
                      const char *album_artist, const int *track, const int *disc){
    snprintf(s->title,TAGLEN,"%s",title); snprintf(s->artist,TAGLEN,"%s",artist); snprintf(s->album,TAGLEN,"%s",album);
    snprintf(s->genre,TAGLEN,"%s",genre); snprintf(s->album_artist,TAGLEN,"%s",album_artist);
    s->track = *track; s->disc = *disc;
}
static void tags_restore(const tag_fields *s, char *title,char *artist,char *album,char *genre,
                         char *album_artist, int *track, int *disc){
    snprintf(title,TAGLEN,"%s",s->title); snprintf(artist,TAGLEN,"%s",s->artist); snprintf(album,TAGLEN,"%s",s->album);
    snprintf(genre,TAGLEN,"%s",s->genre); snprintf(album_artist,TAGLEN,"%s",s->album_artist);
    *track = s->track; *disc = s->disc;
}

/* Move a forward tag walk from file offset *at to `to`. A short gap is read through: a seek drops stdio's buffer, so
 * the next read would fetch the same block again. A long one (artwork) is seeked past, unread. *at < 0 = unknown.
 * 0 = done, -1 = short read, -2 = seek error (ferror won't catch it). */
static int skip_to(FILE *f, off_t *at, off_t to){
    if(*at < 0 || to < *at || to - *at > 4096){
        if(fseeko(f, to, SEEK_SET) != 0) return -2;
    } else while(*at < to){
        unsigned char junk[1024];
        size_t n = to - *at > (off_t)sizeof junk ? sizeof junk : (size_t)(to - *at);
        if(fread(junk,1,n,f) != n) return -1;
        *at += (off_t)n;
    }
    *at = to;
    return 0;
}

/* The ID3 text frames id3v2_read uses; every other frame body (APIC artwork above all) is never read. */
static int id3_frame_used(const char *id){
    static const char *const used[] = { "TIT2","TT2","TPE1","TP1","TALB","TAL","TCON","TCO","TRCK","TRK","TPOS","TPA","TPE2","TP2" };
    for(unsigned i = 0; i < sizeof used / sizeof used[0]; i++) if(!strcmp(id, used[i])) return 1;
    return 0;
}

/* Read ID3v2 text frames (TIT2/TPE1/TALB/TCON/TRCK/TPOS/TPE2, or the v2.2 3-char ids). Returns 1 if the
 * header was present. Fields it doesn't find are left untouched. Walks the frame headers in the file and reads only
 * the bodies of the frames above: the rest are seeked past. A tag that runs past the end of the file is ignored
 * whole (the whole-tag read it replaces got a short read there). */
static int id3v2_read(FILE *f, char *title,char *artist,char *album,char *genre,
                      char *album_artist, int *track, int *disc, int *rerr){
    unsigned char h[10];
    size_t nh=fread(h,1,10,f);
    if(nh!=10){ if(nh>=3 && !memcmp(h,"ID3",3) && rerr)*rerr=1; return 0; }
    if(memcmp(h,"ID3",3)!=0) return 0;
    int ver=h[3];
    if(ver<2 || ver>4 || ((h[6]|h[7]|h[8]|h[9])&0x80)){ if(rerr)*rerr=1; return 1; }
    int unsync=(h[5]&0x80)!=0, exthdr=(h[5]&0x40)!=0;
    long tagsize = synch32(h+6);
    if(tagsize==0) return 1;
    if(tagsize<0 || tagsize>20*1024*1024){ if(rerr)*rerr=1; return 1; }
    off_t base = ftello(f);
    struct stat st;
    if(base < 0 || fstat(fileno(f), &st) != 0){ if(rerr)*rerr=1; return 1; }
    if(st.st_size - base < tagsize){ if(rerr)*rerr=1; return 1; }                                  /* truncated tag: nothing read */
    tag_fields saved; tags_save(&saved, title, artist, album, genre, album_artist, track, disc);
    int err0 = ferror(f), ok = 1;
    long p=0;
    off_t at=base;                                /* the stream position */
    if(exthdr && ver>=3 && tagsize<6) ok=0;
    if(exthdr && ver>=3 && tagsize>=6){           /* skip the extended header if present */
        unsigned char e[4] = {0};
        if(fread(e,1,4,f)!=4) ok=0;
        at += 4;
        if(ver>=4){ long es=synch32(e);      if(es>=6 && es<=tagsize) p+=es; else ok=0; }        /* v2.4: size incl itself */
        else      { long es=be32(e);         if(es>=6 && es<=tagsize-4) p+=4+es; else ok=0; }    /* v2.3: excl the 4 size bytes */
    }
    (void)unsync;
    int idlen = (ver==2)?3:4, fhdr=(ver==2)?6:10;
    while(ok && p + fhdr <= tagsize){
        unsigned char fh[10];
        int sk = skip_to(f, &at, base + p);
        if(sk){ if(sk == -2 && rerr) *rerr=1; ok=0; break; }
        if(fread(fh,1,(size_t)fhdr,f) != (size_t)fhdr){ ok=0; break; }
        at += fhdr;
        char id[5]={0}; memcpy(id, fh, idlen);
        if(id[0]==0) break;                       /* padding */
        long fsize;
        if(ver==2) fsize = (fh[3]<<16)|(fh[4]<<8)|fh[5];
        else if(ver==4) fsize = synch32(fh+4);
        else fsize = be32(fh+4);
        if(fsize<=0 || fsize > tagsize - p - fhdr){ ok=0; break; }   /* overflow-safe (tagsize-p-fhdr >= 0) */
        if(id[0]=='T' && id3_frame_used(id)){     /* text frame: enc byte + text */
            unsigned char *body = malloc((size_t)fsize);
            if(!body){ if(rerr)*rerr=1; ok=0; break; }                             /* OOM: a real read failure (ferror won't catch it) */
            if(fread(body,1,(size_t)fsize,f) != (size_t)fsize){ free(body); ok=0; break; }
            at += fsize;
            int enc=body[0];
            char val[TAGLEN]; id3_decode(enc, body+1, (int)fsize-1, val, sizeof val);
            free(body);
            if(val[0]){
                const char *k = id;
                if(!strcmp(k,"TIT2")||!strcmp(k,"TT2")) snprintf(title,TAGLEN,"%s",val);
                else if(!strcmp(k,"TPE1")||!strcmp(k,"TP1")) snprintf(artist,TAGLEN,"%s",val);
                else if(!strcmp(k,"TALB")||!strcmp(k,"TAL")) snprintf(album,TAGLEN,"%s",val);
                else if(!strcmp(k,"TCON")||!strcmp(k,"TCO")){ snprintf(genre,TAGLEN,"%s",val); genre_clean(genre); }
                else if(!strcmp(k,"TRCK")||!strcmp(k,"TRK")) *track = tag_int(val);  /* "5" or "5/12" */
                else if(!strcmp(k,"TPOS")||!strcmp(k,"TPA")) *disc  = tag_int(val);  /* part-of-set */
                else if(!strcmp(k,"TPE2")||!strcmp(k,"TP2")){ if(!album_artist[0]) snprintf(album_artist,TAGLEN,"%s",val); }
            }
        }
        p += fhdr + fsize;
    }
    if(!ok || (!err0 && ferror(f))){ if(rerr)*rerr=1; tags_restore(&saved, title, artist, album, genre, album_artist, track, disc); }
    return 1;
}
/* ID3v1: last 128 bytes "TAG" + 30+30+30 title/artist/album (Latin-1). Fallback only. */
static void id3v1_read(FILE *f, char *title,char *artist,char *album, int *rerr){
    /* fseek does NOT set the stream error indicator, so ferror() won't catch a seek IO failure.
     * A file smaller than 128 bytes fails this seek with EINVAL (benign -> just no ID3v1); only a
     * real media error (EIO) should preserve the existing row. */
    if(fseek(f,-128,SEEK_END)!=0){ if(rerr && errno==EIO) *rerr=1; return; }
    unsigned char t[128];
    if(fread(t,1,128,f)!=128) return;
    if(memcmp(t,"TAG",3)!=0) return;
    char tmp[64];
    if(!title[0]){  id3_decode(0, t+3,  30, tmp,sizeof tmp); if(tmp[0]) snprintf(title, TAGLEN,"%s",tmp); }
    if(!artist[0]){ id3_decode(0, t+33, 30, tmp,sizeof tmp); if(tmp[0]) snprintf(artist,TAGLEN,"%s",tmp); }
    if(!album[0]){  id3_decode(0, t+63, 30, tmp,sizeof tmp); if(tmp[0]) snprintf(album, TAGLEN,"%s",tmp); }
}

static uint32_t le32(const unsigned char *p){ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }

/* A Vorbis comment body (vendor length + vendor + count + "KEY=value" items, little-endian lengths), as in a FLAC
 * VORBIS_COMMENT block and an Ogg Vorbis comment packet after its 7-byte "\x03vorbis" id. Fills only empty fields. */
static int comments_with_provenance(const unsigned char *b, uint32_t len, char *title,char *artist,char *album,char *genre,
                            char *album_artist, int *track, int *disc, int *title_tag){
    /* overflow-safe bounds: need 4(vendor-len field) + vlen(vendor) + 4(comment count) <= len, i.e. vlen <= len-8.
     * Use subtraction so a hostile vlen like 0xFFFFFFFA can't wrap an addition past the guard. */
    if(len<8) return 0;
    uint32_t vlen=le32(b);
    if(vlen > len-8) return 0;
    uint32_t off=4+vlen;                          /* off <= len-4, so le32(b+off) is in-bounds */
    uint32_t cnt=le32(b+off); off+=4;
    /* Validate the complete declared table before publishing any field. A damaged late
     * entry must not leave a believable-looking partial title/artist in the caller. */
    uint32_t check=off;
    for(uint32_t i=0;i<cnt;i++){
        if(check > len-4) return 0;
        uint32_t clen=le32(b+check); check+=4;
        if(clen>len-check) return 0;
        check+=clen;
    }
    for(uint32_t i=0;i<cnt;i++){
        uint32_t clen=le32(b+off); off+=4;
        char kv[1088];
        if(clen>=sizeof kv && ad_comment_wanted(b+off,clen)) return 0;
        if(clen < sizeof kv){
            memcpy(kv,b+off,clen); kv[clen]=0;
            char *eq=strchr(kv,'=');
            if(eq){ *eq=0; const char *k=kv, *v=eq+1;
                if(!strcasecmp(k,"TITLE")){
                    if(title_tag) *title_tag=1;
                    if(!title[0]) snprintf(title, TAGLEN,"%s",v);
                }
                else if(!strcasecmp(k,"ARTIST") && !artist[0]) snprintf(artist,TAGLEN,"%s",v);
                else if(!strcasecmp(k,"ALBUM")  && !album[0])  snprintf(album, TAGLEN,"%s",v);
                else if(!strcasecmp(k,"GENRE")  && !genre[0])  snprintf(genre, TAGLEN,"%s",v);
                else if(!strcasecmp(k,"TRACKNUMBER") && !*track) *track = tag_int(v);
                else if(!strcasecmp(k,"DISCNUMBER")  && !*disc)  *disc  = tag_int(v);
                else if((!strcasecmp(k,"ALBUMARTIST")||!strcasecmp(k,"ALBUM ARTIST")) && !album_artist[0])
                    snprintf(album_artist,TAGLEN,"%s",v);
            }
        }
        off+=clen;
    }
    return 1;
}

static int vorbis_comments(const unsigned char *b, uint32_t len, char *title,char *artist,char *album,char *genre,
                           char *album_artist, int *track, int *disc){
    return comments_with_provenance(b,len,title,artist,album,genre,album_artist,track,disc,NULL);
}

/* FLAC: "fLaC" magic + metadata blocks; parse the VORBIS_COMMENT block (type 4) for
 * TITLE/ARTIST/ALBUM/GENRE (UTF-8, little-endian lengths). Bounded like the ID3 path. */
static void flac_read(FILE *f, char *title,char *artist,char *album,char *genre,
                      char *album_artist, int *track, int *disc, int *rerr){
    unsigned char magic[4];
    if(fread(magic,1,4,f)!=4 || memcmp(magic,"fLaC",4)!=0) { if(rerr)*rerr=1; return; }
    struct stat st;
    if(fstat(fileno(f),&st)!=0){ if(rerr)*rerr=1; return; }
    for(int guard=0; guard<256; guard++){
        unsigned char h[4];
        if(fread(h,1,4,f)!=4){ if(rerr)*rerr=1; return; }
        int last = h[0]&0x80, type = h[0]&0x7f;
        uint32_t len = ((uint32_t)h[1]<<16)|((uint32_t)h[2]<<8)|h[3];
        off_t at=ftello(f);
        if(at<0 || at>st.st_size || len>(uint64_t)(st.st_size-at)){ if(rerr)*rerr=1; return; }
        if(type==4){                                    /* VORBIS_COMMENT */
            if(len<8 || len>1024*1024){ if(rerr)*rerr=1; return; }
            unsigned char *b=malloc(len); if(!b){ if(rerr)*rerr=1; return; }   /* OOM: signal a real read failure (ferror won't catch it) */
            if(fread(b,1,len,f)!=len){ if(rerr)*rerr=1; free(b); return; }                      /* short read: ferror() in tags_from_file distinguishes IO error from EOF */
            if(!vorbis_comments(b,len,title,artist,album,genre,album_artist,track,disc)){
                if(rerr) *rerr=1;
            }
            free(b);
            return;                                      /* got the comment block */
        }
        if(last) return;
        if(fseek(f,(long)len,SEEK_CUR)!=0){ if(rerr) *rerr=1; return; }   /* seek failure (ferror will not catch every errno) */
    }
    if(rerr)*rerr=1;
}

/* ---- MP4 / M4A (iTunes-style metadata) ----
 * Walk the atom tree moov -> udta -> meta -> ilst and read the text/number tag atoms. Fully bounded:
 * every atom size is validated against its parent before use, so a malformed/hostile file can't read
 * out of the buffer. The song length comes separately from audiodur.h (the audio track's mdhd). */

/* Find the first child atom of `type` in buf[pos..end). On success set cs/ce to its PAYLOAD range and
 * return 1. Handles 32-bit, 64-bit (size==1) and to-end (size==0) atoms; rejects anything that overruns. */
static int mp4_child(const unsigned char *buf, long pos, long end, const char *type, long *cs, long *ce){
    while(pos + 8 <= end){
        uint64_t sz = be32(buf + pos);
        long hdr = 8;
        if(sz == 1){                                    /* 64-bit extended size */
            if(pos + 16 > end) break;
            sz = ((uint64_t)be32(buf + pos + 8) << 32) | be32(buf + pos + 12);
            hdr = 16;
        } else if(sz == 0){                             /* runs to the end of the parent */
            sz = (uint64_t)(end - pos);
        }
        if(sz < (uint64_t)hdr) break;                   /* smaller than its own header */
        if(sz > (uint64_t)(end - pos)) break;           /* overruns the parent */
        if(memcmp(buf + pos + 4, type, 4) == 0){ *cs = pos + hdr; *ce = pos + (long)sz; return 1; }
        pos += (long)sz;
    }
    return 0;
}

static void mp4_str(const unsigned char *v, long vn, char *out){
    long n = vn < TAGLEN - 1 ? vn : TAGLEN - 1;         /* iTunes text atoms are UTF-8 */
    if(n < 0) n = 0;
    memcpy(out, v, (size_t)n); out[n] = 0; str_trim(out);
}

/* `key` = the 4-byte ilst atom id; v/vn = the value inside its `data` atom (past the 8-byte type/flags). */
static void mp4_take(const unsigned char *key, const unsigned char *v, long vn,
                     char *title,char *artist,char *album,char *genre,char *album_artist,int *track,int *disc){
    if(vn <= 0) return;
    if     (key[0]==0xA9 && key[1]=='n'&&key[2]=='a'&&key[3]=='m' && !title[0])        mp4_str(v,vn,title);
    else if(key[0]==0xA9 && key[1]=='A'&&key[2]=='R'&&key[3]=='T' && !artist[0])       mp4_str(v,vn,artist);
    else if(key[0]==0xA9 && key[1]=='a'&&key[2]=='l'&&key[3]=='b' && !album[0])        mp4_str(v,vn,album);
    else if(key[0]==0xA9 && key[1]=='g'&&key[2]=='e'&&key[3]=='n' && !genre[0])        mp4_str(v,vn,genre);
    else if(!memcmp(key,"aART",4) && !album_artist[0])                                 mp4_str(v,vn,album_artist);
    else if(!memcmp(key,"trkn",4) && vn>=4){ int t = ((int)v[2]<<8)|v[3]; if(t>0) *track = t; }  /* binary: [2 rsv][2 num][2 tot] */
    else if(!memcmp(key,"disk",4) && vn>=4){ int d = ((int)v[2]<<8)|v[3]; if(d>0) *disc  = d; }
}

/* Stream-walk the atoms in the file range [start,end) looking for a child of `type`, WITHOUT reading
 * any atom body: siblings (a multi-MB mdat or a long book's trak sample tables) are seek-skipped, so
 * this costs a handful of 8-byte header reads no matter how big the file is. On success set *cpay (the
 * child's PAYLOAD file offset) + *cpsz (payload size) and return 1. Handles 32-bit, 64-bit (size==1)
 * and to-end (size==0) atoms; guards every offset against overflow/rewind so a hostile size can't loop
 * forever or rewind. fseek/ftell failures set rerr (ferror never catches those). */
/* off_t (64-bit on musl for every arch) + fseeko/ftello so a book >=2GiB is walked correctly instead of
 * failing the whole tag/chapter read on a 32-bit `long` file offset. */
static int mp4_find_child_checked(FILE *f, off_t start, off_t end, const char *type,
                          off_t *cpay, uint64_t *cpsz, int *rerr, int *bad){
    if(fseeko(f, start, SEEK_SET) != 0){ if(rerr) *rerr = 1; return 0; }
    for(;;){
        off_t here = ftello(f);
        if(here < 0){ if(rerr) *rerr = 1; return 0; }
        if(here >= end) break;
        if(end - here < 8){ if(bad) *bad = 1; break; }          /* no room for another header (end-here avoids here+8 overflow near the max) */
        unsigned char h[16];
        if(fread(h,1,8,f) != 8){ if(bad && rerr) *rerr = 1; break; }                   /* clean EOF/short (ferror catches real IO) */
        uint64_t sz = be32(h); int hdr = 8;
        if(sz == 1){
            if(end - here < 16){ if(bad) *bad = 1; break; }
            if(fread(h+8,1,8,f)!=8){ if(bad && rerr) *rerr = 1; break; }
            sz = ((uint64_t)be32(h+8)<<32)|be32(h+12); hdr = 16;
        }
        else if(sz == 0) sz = (uint64_t)(end - here);    /* runs to the end of the parent */
        if(sz < (uint64_t)hdr){ if(bad) *bad = 1; break; }                    /* smaller than its own header */
        if(sz > (uint64_t)(end - here)){ if(bad) *bad = 1; break; }           /* overruns the parent */
        if(memcmp(h+4,type,4)==0){ *cpay = here + hdr; *cpsz = sz - (uint64_t)hdr; return 1; }
        if(sz > (uint64_t)(INT64_MAX - here)) break;     /* next offset would overflow/rewind */
        if(fseeko(f, here + (off_t)sz, SEEK_SET) != 0){ if(rerr) *rerr = 1; return 0; }
    }
    return 0;
}

static int mp4_find_child(FILE *f, off_t start, off_t end, const char *type,
                          off_t *cpay, uint64_t *cpsz, int *rerr){
    int bad=0;
    int found=mp4_find_child_checked(f,start,end,type,cpay,cpsz,rerr,&bad);
    if(bad && rerr)*rerr=1;
    return found;
}

/* Locate moov/udta: its payload's file offset + size. All the tags + chapters + cover live under udta; the
 * multi-MB sample tables (trak) are never read. Returns 1 with upay + usz, else 0. */
static int mp4_find_udta(FILE *f, off_t *upay, uint64_t *usz, int *rerr){
    if(fseeko(f, 0, SEEK_END) != 0){ if(rerr) *rerr = 1; return 0; }
    off_t fsz = ftello(f);
    if(fsz < 0){ if(rerr) *rerr = 1; return 0; }   /* ftello error -> a real IO failure, not "no metadata" */
    if(fsz < 8){ if(rerr)*rerr=1; return 0; }
    off_t moov_pay; uint64_t moov_psz;
    if(!mp4_find_child(f, 0, fsz, "moov", &moov_pay, &moov_psz, rerr)) return 0;
    off_t moov_end = moov_pay + (off_t)moov_psz;         /* find_child already bounded moov to the file */
    off_t udta_pay; uint64_t udta_sz;
    if(!mp4_find_child(f, moov_pay, moov_end, "udta", &udta_pay, &udta_sz, rerr)) return 0;
    if(udta_sz == 0) return 0;                           /* genuinely no udta -> no tags (filename fallback is correct) */
    if(udta_sz > 4u*1024*1024){ if(rerr) *rerr = 1; return 0; }  /* too big to load (e.g. a music m4a with multi-MB cover art) -> a READ failure, not "no tags": preserve the existing row instead of clobbering good title/artist/album with the filename fallback */
    *upay = udta_pay; *usz = udta_sz;
    return 1;
}

/* Locate moov/udta and load ONLY the udta payload into a fresh malloc'd buffer (caller frees). This keeps
 * the allocation tiny (a few KB, plus an embedded cover if present) even for a 20-hour book, instead of
 * loading a whole many-MB moov on a ~19MB-free device. Returns 1 with out_buf + out_sz, else 0. */
static int mp4_load_udta(FILE *f, unsigned char **out_buf, uint64_t *out_sz, int *rerr){
    off_t udta_pay; uint64_t udta_sz;
    if(!mp4_find_udta(f, &udta_pay, &udta_sz, rerr)) return 0;
    unsigned char *buf = malloc((size_t)udta_sz);
    if(!buf){ if(rerr) *rerr = 1; return 0; }            /* OOM: a real read failure (don't clobber the row) */
    if(fseeko(f, udta_pay, SEEK_SET) != 0){ if(rerr) *rerr = 1; free(buf); return 0; }
    if(fread(buf,1,(size_t)udta_sz,f) != (size_t)udta_sz){ if(rerr)*rerr=1; free(buf); return 0; }
    *out_buf = buf; *out_sz = udta_sz;
    return 1;
}

/* The ilst items mp4_take uses; every other item (the covr artwork above all) is seeked past, never read. */
static int mp4_item_used(const unsigned char *key){
    static const unsigned char used[][4] = { {0xA9,'n','a','m'}, {0xA9,'A','R','T'}, {0xA9,'a','l','b'},
                                             {0xA9,'g','e','n'}, {'a','A','R','T'}, {'t','r','k','n'}, {'d','i','s','k'} };
    for(unsigned i = 0; i < sizeof used / sizeof used[0]; i++) if(!memcmp(key, used[i], 4)) return 1;
    return 0;
}

/* udta -> meta -> ilst, walked in the file with mp4_child's bounds; only the items above are read. The udta size
 * rule in mp4_find_udta still applies. A read that fails part way leaves the fields untouched (see tag_fields). */
static void mp4_read(FILE *f, char *title,char *artist,char *album,char *genre,
                     char *album_artist, int *track, int *disc, int *rerr){
    off_t up; uint64_t usz;
    if(!mp4_find_udta(f, &up, &usz, rerr)) return;
    tag_fields saved; tags_save(&saved, title, artist, album, genre, album_artist, track, disc);
    int err0 = ferror(f), ok = 1;
    off_t mts, is; uint64_t msz, isz;
    if(mp4_find_child(f, up, up + (off_t)usz, "meta", &mts, &msz, rerr)){
        if(msz && msz<4){ if(rerr)*rerr=1; return; }
        /* QuickTime may omit FullBox flags. Select that layout only from a
         * bounded child header with a real atom type, never from a size-zero
         * interpretation of the flags. Parse the selected layout just once. */
        off_t children = mts + (msz >= 4 ? 4 : 0);
        unsigned char h[16];
        if(msz >= 8){
            if(fseeko(f,mts,SEEK_SET) || fread(h,1,8,f)!=8){ if(rerr)*rerr=1; return; }
            uint64_t sz=be32(h); unsigned hdr=8;
            if(sz==1 && msz>=16){
                if(fread(h+8,1,8,f)!=8){ if(rerr)*rerr=1; return; }
                sz=((uint64_t)be32(h+8)<<32)|be32(h+12); hdr=16;
            }
            int type=1;
            for(int i=4;i<8;i++) if(h[i]<32 || h[i]>126) type=0;
            /* A zero first size is indistinguishable from FullBox flags.
             * Require an explicit bounded size before choosing flags-less. */
            if(type && sz>=hdr && sz<=msz) children=mts;
        }
        int bad=0,err=0;
        int found=mp4_find_child_checked(f,children,mts+(off_t)msz,"ilst",&is,&isz,&err,&bad);
        if((bad || err) && rerr)*rerr=1;
        if(found){
            off_t p = is, ie = is + (off_t)isz, at = ftello(f);   /* at = the stream position */
            while(ie - p >= 8){
                unsigned char h[16];
                int sk = skip_to(f, &at, p);
                if(sk){ if(sk == -2 && rerr) *rerr = 1; ok = 0; break; }
                if(fread(h,1,8,f) != 8){ ok = 0; break; }
                uint64_t sz = be32(h); int hdr = 8;
                if(sz == 1){ if(ie - p < 16){ ok=0; break; } if(fread(h+8,1,8,f) != 8){ ok = 0; break; } sz = ((uint64_t)be32(h+8)<<32)|be32(h+12); hdr = 16; }
                else if(sz == 0) sz = (uint64_t)(ie - p);
                if(sz < (uint64_t)hdr || sz > (uint64_t)(ie - p)){ ok=0; break; }
                at = p + hdr;
                if(mp4_item_used(h+4)){
                    size_t n = (size_t)(sz - (uint64_t)hdr);
                    unsigned char *b = malloc(n ? n : 1);
                    if(!b){ if(rerr) *rerr = 1; ok = 0; break; }                       /* OOM: a real read failure (don't clobber the row) */
                    if(fread(b,1,n,f) != n){ free(b); ok = 0; break; }
                    at += (off_t)n;
                    long dts, dte;
                    if(mp4_child(b, 0, (long)n, "data", &dts, &dte) && dte - dts >= 8)
                        mp4_take(h+4, b+dts+8, dte-dts-8, title,artist,album,genre,album_artist,track,disc);
                    else ok=0;
                    free(b);
                    if(!ok) break;
                }
                p += (off_t)sz;
            }
            if(p!=ie) ok=0;
        }
    }
    if(!ok || (!err0 && ferror(f))){ if(rerr)*rerr=1; tags_restore(&saved, title, artist, album, genre, album_artist, track, disc); }
}

static int mp4_chapters(const unsigned char *buf, uint64_t udta_sz, chapter_t *out, int max, int *valid){
    *valid = 0;
    int n = 0;
    long cs, ce;
    if(mp4_child(buf, 0, (long)udta_sz, "chpl", &cs, &ce) && ce - cs >= 5){   /* buf IS the udta payload */
        int ver = buf[cs];
        long cnt_off = (ver == 1) ? 8 : (ver == 0) ? 4 : -1;   /* count position by version; reject others */
        if(cnt_off >= 0 && ce - cs > cnt_off){
            int count = buf[cs + cnt_off];
            long q = cs + cnt_off + 1;
            int bad = 0;
            long prev_ms = -1;                                 /* chapters must be chronological */
            for(int i = 0; i < count; i++){                    /* validate EVERY declared entry */
                if(q + 9 > ce){ bad = 1; break; }              /* 8-byte start + 1-byte length */
                uint64_t ticks = ((uint64_t)be32(buf+q) << 32) | be32(buf+q+4); q += 8;
                int tl = buf[q]; q += 1;
                if(q + tl > ce){ bad = 1; break; }
                uint64_t ms64 = ticks / 10000;                 /* 100ns ticks -> ms */
                if(ms64 > (uint64_t)LONG_MAX){ bad = 1; break; }  /* unrepresentable on a 32-bit long */
                if((long)ms64 < prev_ms){ bad = 1; break; }    /* out-of-order start -> reject the whole list (the UI assumes ascending) */
                prev_ms = (long)ms64;
                if(n < max){                                   /* store up to max, but keep validating the rest */
                    out[n].start_ms = (long)ms64;
                    int cl = tl;
                    if(cl > CHAP_TITLE - 1){ cl = CHAP_TITLE - 1; while(cl > 0 && (buf[q+cl] & 0xC0) == 0x80) cl--; }  /* don't split UTF-8 */
                    memcpy(out[n].title, buf+q, (size_t)cl); out[n].title[cl] = 0;
                    str_trim(out[n].title);
                    if(!out[n].title[0]) snprintf(out[n].title, CHAP_TITLE, "Chapter %d", n+1);
                    n++;
                }
                q += tl;
            }
            *valid = !bad;
            if(bad) n = 0;                                     /* a malformed/truncated list -> report none, not a partial one */
        }
    }
    return n;
}

/* Read Nero 'chpl' chapters (moov/udta/chpl) from an M4B/M4A into out[0..max); returns the count (0 if
 * none/unsupported/malformed). Handles chpl version 0 (count at payload+4) and version 1 (count at
 * payload+8); every field is bounds-checked against the atom. Start times are 100ns ticks -> ms. Titles
 * are length-prefixed UTF-8 (no NUL), truncated only at a code-point boundary; an empty title becomes
 * "Chapter N". QuickTime chapter-track chapters are NOT read here (a later stage). */
static int scan_read_chapters_leased(const char *path, chapter_t *out, int max){
    if(!path || !out || max <= 0) return 0;
    FILE *f = fopen(path, "rb"); if(!f) return 0;
    unsigned char *buf; uint64_t udta_sz; int rerr = 0;
    int ok = mp4_load_udta(f, &buf, &udta_sz, &rerr);
    fclose(f);
    if(!ok) return 0;
    int valid;
    int n = mp4_chapters(buf, udta_sz, out, max, &valid);
    free(buf);
    return n;
}

/* Read the narrator of an m4b into out[0..cap): the iTunes composer atom (0xA9 + "wrt"), the near-universal
 * convention for an audiobook's narrator. Returns 1 if a non-empty value was found. Reuses the udta-only
 * loader + bounded ilst walk. */
static int scan_read_narrator_leased(const char *path, char *out, int cap){
    if(!path || !out || cap <= 0) return 0;
    out[0] = 0;
    FILE *f = fopen(path, "rb"); if(!f) return 0;
    unsigned char *buf; uint64_t udta_sz; int rerr = 0;
    int ok = mp4_load_udta(f, &buf, &udta_sz, &rerr);
    fclose(f);
    if(!ok) return 0;
    long mts, mte, is, ie; int found = 0;
    if(mp4_child(buf, 0, (long)udta_sz, "meta", &mts, &mte) &&
       (mp4_child(buf, mts+4, mte, "ilst", &is, &ie) || mp4_child(buf, mts, mte, "ilst", &is, &ie))){
        long p = is;
        while(p + 8 <= ie){
            uint64_t sz = be32(buf+p); long hdr = 8;
            if(sz == 1){ if(p+16 > ie) break; sz = ((uint64_t)be32(buf+p+8)<<32)|be32(buf+p+12); hdr = 16; }
            else if(sz == 0) sz = (uint64_t)(ie - p);
            if(sz < (uint64_t)hdr || sz > (uint64_t)(ie - p)) break;
            const unsigned char *key = buf + p + 4;
            if(key[0]==0xA9 && key[1]=='w' && key[2]=='r' && key[3]=='t'){   /* 0xA9 + "wrt" = composer = narrator */
                long dts, dte;
                if(mp4_child(buf, p+hdr, p+(long)sz, "data", &dts, &dte) && dte - dts >= 8){
                    long vn = dte - dts - 8; if(vn > cap-1) vn = cap-1; if(vn < 0) vn = 0;
                    memcpy(out, buf+dts+8, (size_t)vn); out[vn] = 0; str_trim(out);
                    found = out[0] != 0;
                }
                break;
            }
            p += (long)sz;
        }
    }
    free(buf);
    return found;
}

static int scan_cancelled(void *ctx){
    (void)ctx;
    return atomic_load(&g_abort);
}

/* ---- Ogg Vorbis ----
 * Pages ("OggS", 27-byte header + segment table). The comment header is the stream's SECOND packet
 * ("\x03vorbis" + a Vorbis comment body); it may span pages, so packets are reassembled from the lacing values.
 * Streams comments under AD_TAG_BYTES / AD_TAG_PAGES; only wanted text is retained. */
static void ogg_read(FILE *f, char *title,char *artist,char *album,char *genre,
                     char *album_artist, int *track, int *disc, int *rerr, int *title_tag){
    unsigned char id[8];
    if(ad_read_at(f,0,id,8) && !memcmp(id,"OggS",4)){
        unsigned char first[8], ns;
        if(ad_read_at(f,26,&ns,1) && ad_read_at(f,27+ns,first,8) && !memcmp(first,"OpusHead",8)){
            unsigned char *tags=NULL; uint32_t len=0;
            long long rc=ad_opus(f,&tags,&len,scan_cancelled,NULL);
            if(rc==AD_CANCELLED){ if(rerr) *rerr=AD_CANCELLED; }
            else if(rc<=0){ if(rerr) *rerr=1; }
            if(rc>0){
                if(!comments_with_provenance(tags+8,len-8,title,artist,album,genre,album_artist,track,disc,title_tag) && rerr) *rerr=1;
                free(tags);
            }
            return;
        }
    }
    if(fseek(f,0,SEEK_SET)!=0){ if(rerr) *rerr=1; return; }
    unsigned char *pkt=malloc(1024*1024), *full=malloc(27+255+65025);
    unsigned char *body=full ? full+27+255 : NULL;
    if(!pkt || !body){ if(rerr)*rerr=1; free(pkt); free(full); return; }
    ad_comment_stream comments={0};
    uint32_t serial=0, seq=0, ident=0, budget=AD_TAG_BYTES;
    int pending=0, vorbis=0, complete=0;
    for(unsigned page=0; page<AD_TAG_PAGES; page++){
        if(scan_cancelled(NULL)){ if(rerr)*rerr=AD_CANCELLED; break; }
        unsigned char h[27], seg[255];
        if(budget<27 || ad_opus_read(f,h,27,scan_cancelled,NULL)!=1 || memcmp(h,"OggS",4) || h[4]) break;
        unsigned ns=h[26], size=0;
        if(ad_opus_read(f,seg,ns,scan_cancelled,NULL)!=1) break;
        for(unsigned i=0;i<ns;i++) size+=seg[i];
        if(27+ns+size>budget) break;
        budget-=27+ns+size;
        if(!page) serial=le32(h+14);
        body=full+27+255;
        int rr=ad_opus_read(f,body,size,scan_cancelled,NULL);
        if(rr!=1){ if(rr==AD_CANCELLED && rerr)*rerr=rr; break; }
        /* CRC covers the original bytes, including skipped pictures. */
        memcpy(full,h,27); memcpy(full+27,seg,ns); memmove(full+27+ns,body,size);
        body=full+27+ns;
        if(ad_ogg_crc(full,27+ns+size)!=le32(h+22)) break;
        if(le32(h+14)!=serial) continue;   /* interleaved logical stream, still charged to the work budget */
        if(le32(h+18)!=seq++ || !!(h[5]&1)!=pending) break;
        unsigned off=0;
        for(unsigned i=0;i<ns;i++){
            unsigned n=seg[i]; pending=n==255;
            if(!vorbis){
                if(n>1024*1024-ident) goto ogg_done;
                memcpy(pkt+ident,body+off,n); ident+=n;
                if(!pending){
                    if(ident<7 || pkt[0]!=1 || memcmp(pkt+1,"vorbis",6)) goto ogg_done;
                    vorbis=1;
                }
            } else {
                if(ad_comment_feed(&comments,pkt,body+off,n)!=1) goto ogg_done;
                if(!pending){
                    if(ad_comment_finish(&comments,pkt))
                        complete=comments_with_provenance(pkt+7,comments.used-7,title,artist,album,genre,album_artist,track,disc,title_tag);
                    goto ogg_done;
                }
            }
            off+=n;
        }
    }
ogg_done:
    if(!complete && rerr && *rerr!=AD_CANCELLED) *rerr=scan_cancelled(NULL) ? AD_CANCELLED : 1;
    free(full); free(pkt);
}

/* ---- APE (APEv2 tag) ----
 * A 32-byte footer "APETAGEX" at the end of the file (or just before a 128-byte ID3v1 tag): version, tag size
 * (items + footer), item count. Items: value size (LE), flags (LE), NUL-terminated key, UTF-8 value.
 * Bounded: tag <= 1 MiB, <= 512 items; binary items (flags bits 1-2 != 0) are skipped. */
static void ape_read(FILE *f, char *title,char *artist,char *album,char *genre,
                     char *album_artist, int *track, int *disc, int *rerr){
    unsigned char ft[32];
    long ends[2] = { -32, -32-128 };
    for(int k = 0; k < 2; k++){
        if(fseek(f,ends[k],SEEK_END)!=0){ if(rerr && errno==EIO) *rerr=1; return; }
        if(fread(ft,1,32,f)!=32) return;
        if(memcmp(ft,"APETAGEX",8)!=0) continue;
        uint32_t size = le32(ft+12), items = le32(ft+16);
        if(size < 32 || size > 1024*1024 || items > 512){ if(rerr)*rerr=1; return; }
        uint32_t blen = size - 32;                                  /* the items, before the footer */
        if(fseek(f, ends[k] - (long)blen, SEEK_END)!=0){ if(rerr)*rerr=1; return; }
        unsigned char *b = malloc(blen ? blen : 1);
        if(!b){ if(rerr)*rerr=1; return; }
        if(fread(b,1,blen,f)!=blen){ if(rerr)*rerr=1; free(b); return; }
        uint32_t off = 0;
        for(uint32_t i = 0; i < items; i++){
            if(blen-off<8){ if(rerr)*rerr=1; break; }
            uint32_t vlen = le32(b+off), flags = le32(b+off+4); off += 8;
            uint32_t kend = off; while(kend < blen && b[kend]) kend++;
            if(kend >= blen){ if(rerr)*rerr=1; break; }                                  /* key must be NUL-terminated inside */
            const char *key = (const char *)b+off; off = kend + 1;
            if(vlen > blen - off){ if(rerr)*rerr=1; break; }                             /* overflow-safe */
            if(((flags >> 1) & 3) == 0){                             /* UTF-8 text item */
                char v[TAGLEN];
                uint32_t n = vlen < sizeof v - 1 ? vlen : (uint32_t)sizeof v - 1;
                memcpy(v, b+off, n); v[n] = 0;
                if(v[0]){
                    if(!strcasecmp(key,"Title") && !title[0]) snprintf(title,TAGLEN,"%s",v);
                    else if(!strcasecmp(key,"Artist") && !artist[0]) snprintf(artist,TAGLEN,"%s",v);
                    else if(!strcasecmp(key,"Album") && !album[0]) snprintf(album,TAGLEN,"%s",v);
                    else if(!strcasecmp(key,"Genre") && !genre[0]){ snprintf(genre,TAGLEN,"%s",v); genre_clean(genre); }
                    else if(!strcasecmp(key,"Track") && !*track) *track = tag_int(v);
                    else if(!strcasecmp(key,"Disc") && !*disc) *disc = tag_int(v);
                    else if((!strcasecmp(key,"Album Artist")||!strcasecmp(key,"AlbumArtist")) && !album_artist[0])
                        snprintf(album_artist,TAGLEN,"%s",v);
                }
            }
            off += vlen;
        }
        free(b);
        return;
    }
}

/* ---- AIFF / AIFF-C ----
 * "FORM" <size BE> "AIFF"/"AIFC", then chunks (id, size BE, data, padded to even). Tags come from an "ID3 "
 * chunk (an embedded ID3v2, read by id3v2_read) and, as a fallback, the NAME / AUTH text chunks.
 * Bounded: at most 256 chunks; text chunks copied up to TAGLEN. */
/* the file's size (64-bit off_t on the Disc's musl and on hosts), -1 if it can't be read */
static off_t file_size(FILE *f){
    off_t here = ftello(f);
    if(here < 0 || fseeko(f, 0, SEEK_END) != 0) return -1;
    off_t sz = ftello(f);
    if(fseeko(f, here, SEEK_SET) != 0) return -1;
    return sz;
}
/* An ID3v2 tag starting at the current position that fits entirely in `room` bytes (so an embedded tag can never read
 * into the chunks after it): read it with id3v2_read; else skip it. */
static void id3v2_read_bounded(FILE *f, off_t room, char *title,char *artist,char *album,char *genre,
                               char *album_artist, int *track, int *disc, int *rerr){
    off_t at = ftello(f);
    unsigned char h[10];
    if(at < 0 || room < 10 || fread(h,1,10,f)!=10){ if(rerr)*rerr=1; return; }
    if(memcmp(h,"ID3",3)!=0 || ((h[6]|h[7]|h[8]|h[9]) & 0x80)){ if(rerr)*rerr=1; return; }
    off_t need = 10 + (off_t)synch32(h+6);
    if(need > room){ if(rerr)*rerr=1; return; }                                   /* claims more than its chunk: not read */
    if(fseeko(f, at, SEEK_SET)!=0){ if(rerr) *rerr=1; return; }
    id3v2_read(f,title,artist,album,genre,album_artist,track,disc,rerr);
}
static void aiff_read(FILE *f, char *title,char *artist,char *album,char *genre,
                      char *album_artist, int *track, int *disc, int *rerr){
    unsigned char h[12];
    if(fread(h,1,12,f)!=12 || memcmp(h,"FORM",4)!=0 || (memcmp(h+8,"AIFF",4)!=0 && memcmp(h+8,"AIFC",4)!=0)) { if(rerr)*rerr=1; return; }
    off_t fsz = file_size(f);
    if(fsz < 12 || be32(h+4)>(uint64_t)(fsz-8)){ if(rerr)*rerr=1; return; }
    char name[TAGLEN] = "", auth[TAGLEN] = "";
    int guard;
    for(guard = 0; guard < 256; guard++){
        unsigned char c[8];
        size_t nh=fread(c,1,8,f);
        if(nh!=8){ if(nh && rerr)*rerr=1; break; }
        off_t here = ftello(f);
        if(here < 0){ if(rerr) *rerr = 1; break; }
        off_t len = (off_t)be32(c+4);
        if(len > fsz - here){ if(rerr)*rerr=1; break; }                           /* checked BEFORE any arithmetic: a chunk inside the file */
        if(!memcmp(c,"ID3 ",4) || !memcmp(c,"id3 ",4)){
            id3v2_read_bounded(f,len,title,artist,album,genre,album_artist,track,disc,rerr);
        } else if((!memcmp(c,"NAME",4) || !memcmp(c,"AUTH",4)) && len > 0){
            char *dst = !memcmp(c,"NAME",4) ? name : auth;
            size_t n = len < TAGLEN - 1 ? (size_t)len : TAGLEN - 1;
            unsigned char t[TAGLEN];
            if(fread(t,1,n,f)!=n){ if(rerr)*rerr=1; break; }
            id3_decode(0, t, (int)n, dst, TAGLEN);               /* plain text (Latin-1 per the spec) */
        }
        if(len >= fsz - here) break;                          /* this chunk (pad byte aside) runs to the end: nothing follows */
        off_t next = here + len + (len & 1);                  /* no overflow: len < fsz - here, so here+len+1 <= fsz */
        if(next >= fsz) break;
        if(fseeko(f,next,SEEK_SET)!=0){ if(rerr) *rerr=1; break; }
    }
    if(guard==256 && rerr)*rerr=1;
    if(!title[0] && name[0]) snprintf(title,TAGLEN,"%s",name);
    if(!artist[0] && auth[0]) snprintf(artist,TAGLEN,"%s",auth);
}

/* ---- DSF (DSD stream file) ----
 * "DSD " chunk: size (8 LE, = 28), total file size (8 LE), pointer to the metadata chunk (8 LE, 0 = none). The
 * metadata chunk is an ID3v2 tag, read by id3v2_read at that offset (only when it lies inside the file). */
static void dsf_read(FILE *f, char *title,char *artist,char *album,char *genre,
                     char *album_artist, int *track, int *disc, int *rerr){
    unsigned char h[28];
    if(fread(h,1,28,f)!=28 || memcmp(h,"DSD ",4)!=0) { if(rerr)*rerr=1; return; }
    uint64_t meta  = (uint64_t)le32(h+20) | (uint64_t)le32(h+24) << 32;
    off_t fsz = file_size(f);                                /* the REAL size, not the header's claim */
    if(fsz < 0){ if(rerr) *rerr = 1; return; }
    if(!meta) return;
    if(meta < 28 || meta >= (uint64_t)fsz){ if(rerr)*rerr=1; return; }           /* 0 = no metadata; must lie inside the file */
    if(fseeko(f,(off_t)meta,SEEK_SET)!=0){ if(rerr) *rerr=1; return; }
    id3v2_read_bounded(f,fsz-(off_t)meta,title,artist,album,genre,album_artist,track,disc,rerr);
}

/* ---- WMA (ASF) ----
 * Header Object (GUID 75B22630-...) holds sub-objects: Content Description (75B22633-...: title, author as
 * UTF-16LE with a 5 x u16 length table) and Extended Content Description (D2D0A440-...: named attributes such
 * as WM/AlbumTitle, WM/AlbumArtist, WM/Genre, WM/TrackNumber, WM/PartOfSet). The header is read whole
 * (bounded to 1 MiB) and every object/field length is checked against it. */
static const unsigned char ASF_HDR[16]  = {0x30,0x26,0xB2,0x75,0x8E,0x66,0xCF,0x11,0xA6,0xD9,0x00,0xAA,0x00,0x62,0xCE,0x6C};
static const unsigned char ASF_CD[16]   = {0x33,0x26,0xB2,0x75,0x8E,0x66,0xCF,0x11,0xA6,0xD9,0x00,0xAA,0x00,0x62,0xCE,0x6C};
static const unsigned char ASF_ECD[16]  = {0x40,0xA4,0xD0,0xD2,0x07,0xE3,0xD2,0x11,0x97,0xF0,0x00,0xA0,0xC9,0x5E,0xA8,0x50};
static void utf16le_decode(const unsigned char *in, uint32_t n, char *out, int cap){
    unsigned char tmp[2 + 2*TAGLEN];                       /* id3_decode wants a BOM for little-endian UTF-16 */
    uint32_t m = n < 2*TAGLEN ? n : 2*TAGLEN;
    tmp[0] = 255; tmp[1] = 254; memcpy(tmp+2, in, m);        /* the UTF-16LE byte-order mark FF FE */
    id3_decode(1, tmp, (int)(m+2), out, cap);
}
static void wma_read(FILE *f, char *title,char *artist,char *album,char *genre,
                     char *album_artist, int *track, int *disc, int *rerr){
    unsigned char h[30];
    if(fread(h,1,30,f)!=30 || memcmp(h,ASF_HDR,16)!=0) { if(rerr)*rerr=1; return; }
    uint64_t hsize = (uint64_t)le32(h+16) | (uint64_t)le32(h+20) << 32;
    uint32_t nobj = le32(h+24);
    if(hsize <= 30 || hsize > 1024*1024 || nobj > 1024){ if(rerr)*rerr=1; return; }
    uint32_t blen = (uint32_t)hsize - 30;
    unsigned char *b = malloc(blen);
    if(!b){ if(rerr)*rerr=1; return; }
    if(fread(b,1,blen,f)!=blen){ if(rerr)*rerr=1; free(b); return; }
    uint32_t off = 0;
    for(uint32_t o = 0; o < nobj; o++){
        if(blen-off<24){ if(rerr)*rerr=1; break; }
        uint64_t osz = (uint64_t)le32(b+off+16) | (uint64_t)le32(b+off+20) << 32;
        if(osz < 24 || osz > blen - off){ if(rerr)*rerr=1; break; }
        const unsigned char *d = b + off + 24; uint32_t dl = (uint32_t)osz - 24;
        if((!memcmp(b+off,ASF_CD,16) && dl<10) || (!memcmp(b+off,ASF_ECD,16) && dl<2)){ if(rerr)*rerr=1; break; }
        if(!memcmp(b+off,ASF_CD,16) && dl >= 10){
            uint32_t tl = d[0]|d[1]<<8, al = d[2]|d[3]<<8, cl = d[4]|d[5]<<8, dsl = d[6]|d[7]<<8, rl = d[8]|d[9]<<8;
            if(tl+al+cl+dsl+rl>dl-10){ if(rerr)*rerr=1; break; }
            uint32_t p = 10;
            if(tl <= dl - p){ if(!title[0]) utf16le_decode(d+p, tl, title, TAGLEN); p += tl; } else p = dl;
            if(al <= dl - p){ if(!artist[0]) utf16le_decode(d+p, al, artist, TAGLEN); }
        } else if(!memcmp(b+off,ASF_ECD,16) && dl >= 2){
            uint32_t cnt = d[0]|d[1]<<8, p = 2;
            if(cnt>512){ if(rerr)*rerr=1; break; }
            for(uint32_t i = 0; i < cnt; i++){
                if(dl-p<2){ if(rerr)*rerr=1; break; }
                uint32_t nl = d[p]|d[p+1]<<8; p += 2;
                if(nl > dl - p){ if(rerr)*rerr=1; break; }
                char nm[64]; utf16le_decode(d+p, nl, nm, sizeof nm); p += nl;
                if(dl-p<4){ if(rerr)*rerr=1; break; }
                uint32_t vt = d[p]|d[p+1]<<8, vl = d[p+2]|d[p+3]<<8; p += 4;
                if(vl > dl - p){ if(rerr)*rerr=1; break; }
                const unsigned char *v = d+p;
                char val[TAGLEN] = "";
                if(vt == 0) utf16le_decode(v, vl, val, TAGLEN);                        /* Unicode string */
                else if(vt == 3 && vl >= 4) snprintf(val, sizeof val, "%u", (unsigned)le32(v));   /* DWORD */
                else if(vt == 5 && vl >= 2) snprintf(val, sizeof val, "%u", (unsigned)(v[0]|v[1]<<8)); /* WORD */
                if(val[0]){
                    if(!strcmp(nm,"WM/AlbumTitle") && !album[0]) snprintf(album,TAGLEN,"%s",val);
                    else if(!strcmp(nm,"WM/AlbumArtist") && !album_artist[0]) snprintf(album_artist,TAGLEN,"%s",val);
                    else if(!strcmp(nm,"WM/Genre") && !genre[0]){ snprintf(genre,TAGLEN,"%s",val); genre_clean(genre); }
                    else if(!strcmp(nm,"WM/TrackNumber") && !*track) *track = tag_int(val);
                    else if(!strcmp(nm,"WM/PartOfSet") && !*disc) *disc = tag_int(val);
                }
                p += vl;
            }
        }
        off += (uint32_t)osz;
    }
    free(b);
}

/* Folder albums: every folder on the card is one album, named after the folder itself (the file's
 * parent directory), whatever the files' ALBUM tags say. Files directly in the card root have no album
 * folder and get "Unknown album". Audiobooks (.m4b) keep their tags. */
#define FOLDER_ALBUM_MAX 159   /* bytes: MDB_STR (musicdb.h) - 1 */
static void folder_album(const char *path, char *out, int cap){
    const char *slash = strrchr(path, '/');
    size_t dlen = slash ? (size_t)(slash - path) : 0;
    size_t rlen = strlen(SCAN_ROOT);
    if(!slash || dlen <= rlen || strncmp(path, SCAN_ROOT, rlen) != 0 || path[rlen] != '/'){
        snprintf(out, cap, "%s", "Unknown album"); return;
    }
    const char *b = path + dlen;                 /* walk back to the start of the parent's name */
    while(b > path && b[-1] != '/') b--;
    size_t n = (size_t)(path + dlen - b);
    /* The library keeps album names in MDB_STR (160) byte buffers and plays an album by its exact stored ALBUM value,
     * so a longer name would be shown and requested cut short and match no row. Store it already cut to what fits,
     * on a UTF-8 character boundary, so the list, the drill-in and the player all use the same value. */
    if(n > FOLDER_ALBUM_MAX){
        n = FOLDER_ALBUM_MAX;
        while(n > 0 && ((unsigned char)b[n] & 0xC0) == 0x80) n--;   /* don't split a character */
    }
    snprintf(out, cap, "%.*s", (int)n, b);
    str_trim(out);
    if(!out[0]) snprintf(out, cap, "%s", "Unknown album");
}

/* Read tags + fill a filename fallback. Return code drives how the caller writes the row:
 *   1  = tags read (or a no-tag container like WAV) -> safe to INSERT a new row OR UPDATE in place.
 *   0  = a tag-bearing file that could NOT be OPENED (transient I/O) -> preserve existing tags and
 *        do NOT insert a new one (title..genre are left empty; a file we can't open at all is skipped).
 *  -1  = OPENED but its tags could not be LOADED (a mid-read I/O error, or oversized/hostile atoms):
 *        title..genre ARE filled with the filename fallback. The caller must NOT overwrite an existing
 *        row's tags (never clobber good tags), but may INSERT a fallback row when the path is new so the file
 *        is still playable. */
static int tags_with_provenance(const char *path, const char *fname,
                          char *title,char *artist,char *album,char *genre,
                          char *album_artist, int *track, int *disc, long long *dur_ms, int *title_tag){
    title[0]=artist[0]=album[0]=genre[0]=album_artist[0]=0; *track=0; *disc=0; *dur_ms=0;
    if(title_tag) *title_tag=0;
    int rerr=0;
    FILE *f=fopen(path,"rb");
    if(!f && (has_ext(fname,".flac") || has_ext(fname,".mp3") || has_ext(fname,".m4a") || has_ext(fname,".m4b")
              || has_ext(fname,".ogg") || has_ext(fname,".opus") || has_ext(fname,".oga") || has_ext(fname,".ape") || has_ext(fname,".aif") || has_ext(fname,".aiff")
              || has_ext(fname,".wma") || has_ext(fname,".dsf") || has_ext(fname,".aac")))
        return 0;   /* unopened: preserve the entire existing row (incl. length), no new row */
    if(f){
        if(has_ext(fname,".flac")) flac_read(f,title,artist,album,genre,album_artist,track,disc,&rerr);
        else if(has_ext(fname,".mp3")){ id3v2_read(f,title,artist,album,genre,album_artist,track,disc,&rerr); id3v1_read(f,title,artist,album,&rerr); }
        else if(has_ext(fname,".m4a") || has_ext(fname,".m4b")) mp4_read(f,title,artist,album,genre,album_artist,track,disc,&rerr);
        else if(has_ext(fname,".ogg") || has_ext(fname,".opus") || has_ext(fname,".oga")) ogg_read(f,title,artist,album,genre,album_artist,track,disc,&rerr,title_tag);
        else if(has_ext(fname,".ape")){ ape_read(f,title,artist,album,genre,album_artist,track,disc,&rerr);
                                        if(!title[0] && fseek(f,0,SEEK_SET)==0) id3v1_read(f,title,artist,album,&rerr); }
        else if(has_ext(fname,".aif") || has_ext(fname,".aiff")) aiff_read(f,title,artist,album,genre,album_artist,track,disc,&rerr);
        else if(has_ext(fname,".dsf")) dsf_read(f,title,artist,album,genre,album_artist,track,disc,&rerr);
        else if(has_ext(fname,".wma")) wma_read(f,title,artist,album,genre,album_artist,track,disc,&rerr);
        else if(has_ext(fname,".aac")) id3v2_read(f,title,artist,album,genre,album_artist,track,disc,&rerr);  /* ADTS may lead with ID3v2 */
        /* .wav/.dff/.dts (and any other accepted container) -> filename fallback below; no tag probing,
         * so a WAV whose last 128 bytes happen to start with "TAG" isn't misread as ID3v1. */
        if(ferror(f)) rerr=1;   /* ANY stream IO error during tag reads (headers/seeks/body) -> a read failure,
                                 * not "no tags" (feof/clean EOF does NOT set this, so a short valid file still
                                 * falls back to filename). Catches the header/seek paths rerr doesn't. */
        if(rerr==AD_CANCELLED || scan_cancelled(NULL)){
            fclose(f); return AD_CANCELLED;
        }
        *dur_ms = audio_duration_ms_cancel(f, fname, scan_cancelled, NULL);   /* AFTER the ferror check: its clearerr can't hide a tag read error;
                                                  * negative = read/resource/work failure (store unknown for SONG) */
        fclose(f);
        if(*dur_ms==AD_CANCELLED || scan_cancelled(NULL)) return AD_CANCELLED;
    }
    str_trim(album_artist);   /* whitespace-only album-artist -> empty, so it never overwrites a good value */
    if(title_tag && title[0]) *title_tag=1;
    if(!title[0]){                               /* filename (minus extension) */
        snprintf(title,TAGLEN,"%s",fname);
        char *dot=strrchr(title,'.'); if(dot) *dot=0;
        str_trim(title);
    }
    if(!artist[0]) snprintf(artist,TAGLEN,"%s","Unknown artist");
    if(!album[0])  snprintf(album, TAGLEN,"%s","Unknown album");
    if(!genre[0])  snprintf(genre, TAGLEN,"%s","Unknown genre");
    if(!has_ext(fname,".m4b")) folder_album(path, album, TAGLEN);   /* the folder is the album, not the ALBUM tag */
    return rerr ? -1 : 1;   /* -1: fallback-only (preserve existing tags, insert-if-new); 1: normal upsert */
}

static int tags_from_file(const char *path, const char *fname,
                          char *title,char *artist,char *album,char *genre,
                          char *album_artist, int *track, int *disc, long long *dur_ms){
    return tags_with_provenance(path,fname,title,artist,album,genre,album_artist,track,disc,dur_ms,NULL);
}

/* ---- SQLite: schema + insert ---- */
static const char *SCHEMA_SONG =
    "CREATE TABLE IF NOT EXISTS SONG (ID INTEGER PRIMARY KEY autoincrement, PATH TEXT, NAME TEXT,"
    "TITLE TEXT, ALBUM TEXT, ARTIST TEXT, GENRE TEXT, DISC INT, TRACK INT, IS_CUE INT, IS_ISO INT,"
    "IS_DSD INT, OFFSET BIGINT, DURATION BIGINT, NAME_CODE INT, TITLE_CODE INT, ALBUM_CODE INT,"
    "ARTIST_CODE INT, GENRE_CODE INT, ADD_TIME INT8, SAMPLE_RATE INT, BIT_PER_SAMPLE INT, CHANNELS INT,"
    "BIT_RATE INT, SONG_MIMETYPE TEXT, SONG_PRODUCTION_YEAR TEXT, IS_SELECT INT, ALBUM_ARTIST TEXT,"
    /* IS_M3U/M3U_PATH match the V2.28 stock SONG superset (its playback SQL SELECTs them); ACCENT is
     * our private per-song art-accent cache. TEXT for ALBUM_ARTIST matches stock (was wrongly INT). */
    "ALBUM_ARTIST_CODE INT, IS_M3U INT DEFAULT 0, M3U_PATH TEXT DEFAULT '', ACCENT INTEGER DEFAULT 0, TITLE_TAG INTEGER);";
static const char *INS_SONG =
    "INSERT INTO SONG (PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,"
    "DURATION,NAME_CODE,TITLE_CODE,ALBUM_CODE,ARTIST_CODE,GENRE_CODE,ADD_TIME,SAMPLE_RATE,"
    "BIT_PER_SAMPLE,CHANNELS,BIT_RATE,SONG_MIMETYPE,SONG_PRODUCTION_YEAR,IS_SELECT,ALBUM_ARTIST,"
    /* Explicit ?N params so the existing bind_tags indices are unchanged: 1=PATH, 2..6=tags, 7..11=codes,
     * 12=ADD_TIME; 13=DISC, 14=TRACK, 15=ALBUM_ARTIST (NULL when absent), 16=ALBUM_ARTIST_CODE. DISC/TRACK/
     * ALBUM_ARTIST were hardcoded 0/0/NULL before (the 2026-09-05 audit flagged the missing metadata).
     * IS_M3U/M3U_PATH/ACCENT are omitted so they take their schema DEFAULTs. 17=DURATION (ms, 0 = unknown). */
    "ALBUM_ARTIST_CODE,TITLE_TAG) VALUES (?1,?2,?3,?4,?5,?6,?13,?14,0,0,0,0,?17,?7,?8,?9,?10,?11,?12,0,0,0,0,'','',0,?15,?16,?18);";
/* MERGE (not delete+reinsert): UPDATE an existing PATH's metadata in place so its ID and ACCENT are
 * preserved - MEMORY_PLAY.MUSIC_ID (resume) and MY_LOVE.ID (favourites) are ID-keyed, so a delete+
 * reinsert with fresh autoincrement IDs used to break resume + favourites and wipe the art-accent cache
 * on every rescan. ADD_TIME is intentionally left untouched here (keep the original add time). */
static const char *UPD_SONG =
    /* Explicit ?N: 1..5=tags, 6..10=codes, 11=PATH (WHERE); 12=DISC, 13=TRACK, 14=ALBUM_ARTIST,
     * 15=ALBUM_ARTIST_CODE. Now re-populates disc/track/album-artist on every rescan too (was omitted,
     * so existing rows never gained the metadata). */
    "UPDATE SONG SET NAME=?1,TITLE=?2,ALBUM=?3,ARTIST=?4,GENRE=?5,"
    "NAME_CODE=?6,TITLE_CODE=?7,ALBUM_CODE=?8,ARTIST_CODE=?9,GENRE_CODE=?10,"
    /* Only FILL IN disc/track/album-artist we actually extracted; keep any existing (e.g. stock-written)
     * value when our read yields nothing, so a rescan never clobbers richer metadata with 0/NULL. */
    "DISC=CASE WHEN ?12>0 THEN ?12 ELSE DISC END,"
    "TRACK=CASE WHEN ?13>0 THEN ?13 ELSE TRACK END,"
    "ALBUM_ARTIST=CASE WHEN ?14 IS NOT NULL THEN ?14 ELSE ALBUM_ARTIST END,"
    "ALBUM_ARTIST_CODE=CASE WHEN ?14 IS NOT NULL THEN ?15 ELSE ALBUM_ARTIST_CODE END,"
    /* Store this opened file's length, including 0 on inspection failure; cancellation rolls back. */
    "DURATION=?16,TITLE_TAG=?17 "
    "WHERE PATH=?11;";
/* per-connection temp table of PATHs seen this scan; drives the post-walk delete of vanished songs. */
static const char *SEEN_DDL = "CREATE TEMP TABLE IF NOT EXISTS seen(PATH TEXT PRIMARY KEY);"
    "CREATE TEMP TABLE IF NOT EXISTS unreadable(PATH TEXT PRIMARY KEY); DELETE FROM unreadable;";   /* listed but unstat-able audio: a CUE it backs cannot be rewritten, so its rows must not be retired */
static const char *SEEN_INS = "INSERT OR IGNORE INTO seen(PATH) VALUES(?);";
/* Books whose tags were SUCCESSFULLY read this scan (tags_from_file rc==1). The BOOKS relocate refreshes an
 * existing book's metadata ONLY for these paths, so a later good read fixes stale/fallback tags while a read
 * FAILURE (filename fallback) never clobbers previously-good metadata. */
static const char *BOOKOK_DDL = "CREATE TEMP TABLE IF NOT EXISTS book_ok(PATH TEXT PRIMARY KEY);";
static const char *BOOKOK_INS = "INSERT OR IGNORE INTO book_ok(PATH) VALUES(?);";
/* Books whose LENGTH was read without an I/O error this scan (0 = read fine, length unknown). Only these may replace a
 * book's stored length; a read failure keeps the old one. */
static const char *DUROK_DDL = "CREATE TEMP TABLE IF NOT EXISTS book_dur_ok(PATH TEXT PRIMARY KEY);";
static const char *DUROK_INS = "INSERT OR IGNORE INTO book_dur_ok(PATH) VALUES(?);";

static sqlite3 *g_db;
static sqlite3_stmt *g_ins, *g_upd, *g_seen, *g_cuechk, *g_bookok, *g_durok;
/* One byte predicate for SONG, BOOKS and CUE retirement. No text collation or wildcard rules.
 * ROOT has no trailing slash; the explicit boundary keeps Album2 outside Album. */
#define PRUNE_ALLOWED(path) \
    "NOT EXISTS (SELECT 1 FROM protected_roots WHERE ROOT=CAST(" path " AS BLOB)) AND " \
    "NOT EXISTS (WITH RECURSIVE cuts(n) AS (" \
    "SELECT instr(CAST(" path " AS BLOB),x'2f') UNION ALL " \
    "SELECT n+instr(substr(CAST(" path " AS BLOB),n+1),x'2f') FROM cuts " \
    "WHERE n>0 AND instr(substr(CAST(" path " AS BLOB),n+1),x'2f')>0) " \
    "SELECT 1 FROM cuts JOIN protected_roots AS P " \
    "ON P.ROOT=substr(CAST(" path " AS BLOB),1,n-1) AND P.SUBTREE=1 WHERE n>0)"
static const char *PROTECT_DDL =
    "CREATE TEMP TABLE IF NOT EXISTS protected_roots(ROOT BLOB NOT NULL, SUBTREE INTEGER NOT NULL,"
    "PRIMARY KEY(ROOT,SUBTREE)); DELETE FROM protected_roots;";
static sqlite3_stmt *g_protect;
static unsigned g_protect_count;
static void protect_root(const char *path, int subtree, const char *reason){
    /* Log at most 16 bounded paths, as hex bytes so names cannot inject log lines. */
    if(g_protect_count++ < 16){
        char hex[129]; size_t n = strlen(path); if(n > 64) n = 64;
        for(size_t i = 0; i < n; i++) snprintf(hex + 2*i, 3, "%02x", (unsigned char)path[i]);
        hex[2*n] = 0;
        fprintf(stderr,"scanner: protect %s %s path_hex=%s%s\n", reason,
                subtree ? "subtree" : "exact", hex, strlen(path) > n ? "..." : "");
    }
    if(!g_protect || sqlite3_reset(g_protect) != SQLITE_OK
       || sqlite3_clear_bindings(g_protect) != SQLITE_OK
       || sqlite3_bind_blob(g_protect, 1, path, (int)strlen(path), SQLITE_TRANSIENT) != SQLITE_OK
       || sqlite3_bind_int(g_protect, 2, subtree) != SQLITE_OK
       || sqlite3_step(g_protect) != SQLITE_DONE){
        if(!g_prune_blocked) fprintf(stderr,"scanner: protection failed -> rollback\n");
        g_prune_blocked = 1;
        g_scan_err = 1;   /* CUE retirement may already have run before an ISO read failed. */
    }
}
static void protect_parent(const char *path, const char *reason){
    char parent[MAXPATH]; const char *slash = strrchr(path, '/');
    if(!slash || slash == path){ protect_root(SCAN_ROOT, 1, reason); return; }
    size_t n = (size_t)(slash - path);
    if(n >= sizeof parent){ protect_root(SCAN_ROOT, 1, reason); return; }
    memcpy(parent, path, n); parent[n] = 0;
    protect_root(parent, 1, reason);
}

/* L39: a CUE/ISO backing file (e.g. one .flac holding many virtual tracks) has DB rows that share its
 * PATH but carry IS_CUE=1/IS_ISO=1 + per-track TITLE/TRACK/OFFSET. A rescan must NOT overwrite those
 * (UPDATE ... WHERE PATH=? would hit them) nor add a duplicate plain row - so we skip such a path. */
static const char *CUE_CHECK =
    "SELECT 1 FROM SONG WHERE PATH=?1 AND (COALESCE(IS_CUE,0)=1 OR COALESCE(IS_ISO,0)=1) LIMIT 1;";

/* True iff SONG has column `col`. Used to PROVE the V2.28-required columns really exist after the
 * migration ALTERs before we rebuild the library - a silently-failed ALTER (SQLITE_BUSY/FULL/IOERR)
 * must not produce a library the stock V2.28 player can't query. PRAGMA table_info is universally
 * supported (no dependency on the pragma-function feature). */
static int song_has_col(sqlite3 *db, const char *col){
    sqlite3_stmt *st = NULL; int found = 0;
    if(sqlite3_prepare_v2(db, "PRAGMA table_info(SONG);", -1, &st, NULL) == SQLITE_OK){
        while(sqlite3_step(st) == SQLITE_ROW){
            const unsigned char *n = sqlite3_column_text(st, 1);   /* col 1 = name */
            if(n && strcmp((const char*)n, col) == 0){ found = 1; break; }
        }
        sqlite3_finalize(st);
    }
    return found;
}

/* bind the 5 text tags + their 5 codes (fname/title/album/artist/genre) to a prepared stmt starting at
 * parameter index `p0` (used for both the UPDATE's SET list and the INSERT's value list). */
static void bind_tags(sqlite3_stmt *s, int p0, const char *fname, const char *title,
                      const char *album, const char *artist, const char *genre){
    sqlite3_bind_text(s, p0+0, fname, -1, SQLITE_TRANSIENT);   /* NAME = filename */
    sqlite3_bind_text(s, p0+1, title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, p0+2, album, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, p0+3, artist,-1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, p0+4, genre, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (s, p0+5, code4(fname));
    sqlite3_bind_int (s, p0+6, code4(title));
    sqlite3_bind_int (s, p0+7, code4(album));
    sqlite3_bind_int (s, p0+8, code4(artist));
    sqlite3_bind_int (s, p0+9, code4(genre));
}
/* bind DISC, TRACK, ALBUM_ARTIST, ALBUM_ARTIST_CODE at params d0..d0+3. Album-artist absent -> NULL. */
static void bind_meta(sqlite3_stmt *s, int d0, int disc, int track, const char *aa){
    sqlite3_bind_int(s, d0+0, disc);
    sqlite3_bind_int(s, d0+1, track);
    if(aa && aa[0]){ sqlite3_bind_text(s, d0+2, aa, -1, SQLITE_TRANSIENT); sqlite3_bind_int(s, d0+3, code4(aa)); }
    else          { sqlite3_bind_null(s, d0+2);                            sqlite3_bind_int(s, d0+3, 0); }
}
/* Record a path in `seen` WITHOUT touching its row - used to PRESERVE an existing row for a file we
 * couldn't index this pass (ENOENT / unreadable), so the vanished-row prune doesn't delete it. If we
 * can't even record it, block the prune (conservative: never delete a file we failed to preserve). */
static void mark_seen(const char *path){
    if(!g_seen){ g_prune_blocked = 1; return; }
    sqlite3_reset(g_seen); sqlite3_clear_bindings(g_seen);
    sqlite3_bind_text(g_seen, 1, path, -1, SQLITE_TRANSIENT);
    if(sqlite3_step(g_seen) != SQLITE_DONE) g_prune_blocked = 1;
}
/* Merge one file into SONG: record it as seen, then UPDATE the existing PATH in place (preserving ID +
 * ACCENT), or INSERT a new row if the PATH is new. Any DB error sets g_scan_err (blocks the commit). */
static void upsert_song(const char *path, const char *fname){
    if(g_scan_err) return;   /* an earlier write failed -> don't add writes to a possibly rolled-back txn */
    char title[TAGLEN],artist[TAGLEN],album[TAGLEN],genre[TAGLEN],album_artist[TAGLEN];
    int track=0, disc=0, title_tag=0; long long dur_ms=0;
    int tst = tags_with_provenance(path, fname, title, artist, album, genre, album_artist, &track, &disc, &dur_ms, &title_tag);
    if(tst==AD_CANCELLED || atomic_load(&g_abort)){ g_scan_err=1; return; }
    if(tst == 0){
        mark_seen(path);   /* observed, but it may back virtual CUE tracks */
        protect_root(path, 0, "audio-open");
        g_skipped++;
        return;
    }
    if(tst < 0 || dur_ms < 0){
        protect_root(path, 0, "audio-read");
        if(g_scan_err) return;
    }
    /* record as seen (drives the post-walk delete of paths that vanished from the SD) */
    sqlite3_reset(g_seen); sqlite3_clear_bindings(g_seen);
    sqlite3_bind_text(g_seen, 1, path, -1, SQLITE_TRANSIENT);
    if(sqlite3_step(g_seen)!=SQLITE_DONE){ g_scan_err=1; return; }
    /* Track books we read REAL tags for this scan (rc==1), so the post-walk relocate can refresh their
     * BOOKS metadata without a fallback read ever clobbering previously-good tags. */
    if(tst == 1 && has_ext(fname, ".m4b") && g_bookok){
        sqlite3_reset(g_bookok); sqlite3_clear_bindings(g_bookok);
        sqlite3_bind_text(g_bookok, 1, path, -1, SQLITE_TRANSIENT);
        if(sqlite3_step(g_bookok)!=SQLITE_DONE){ g_scan_err=1; return; }
    }
    if(dur_ms >= 0 && has_ext(fname, ".m4b") && g_durok){
        sqlite3_reset(g_durok); sqlite3_clear_bindings(g_durok);
        sqlite3_bind_text(g_durok, 1, path, -1, SQLITE_TRANSIENT);
        if(sqlite3_step(g_durok)!=SQLITE_DONE){ g_scan_err=1; return; }
    }
    /* An opened SONG file replaces its previous length even on inspection failure.
     * Keep the separate BOOKS read-error policy above; cancellation has already returned. */
    if(dur_ms < 0) dur_ms = 0;
    /* L39: if this path already backs stock CUE/ISO virtual tracks, leave them entirely alone (marked
     * seen above so they survive the prune) - do NOT overwrite their per-track metadata via UPDATE, and
     * do NOT insert a duplicate plain row. */
    sqlite3_reset(g_cuechk); sqlite3_clear_bindings(g_cuechk);
    if(sqlite3_bind_text(g_cuechk, 1, path, -1, SQLITE_TRANSIENT)!=SQLITE_OK){ g_scan_err=1; return; }
    int cchk = sqlite3_step(g_cuechk);
    if(cchk==SQLITE_ROW){ pthread_mutex_lock(&g_mu); g_done++; pthread_mutex_unlock(&g_mu); return; }  /* CUE/ISO backing -> preserve */
    if(cchk!=SQLITE_DONE){ g_scan_err=1; return; }   /* query error -> fail closed, never risk the UPDATE on unknown state */
    if(tst < 0){
        /* File opened but tags could not be loaded (oversized/hostile atoms, mid-read IO).
         * The filename fallback is filled:
         * NEVER overwrite an existing row's good tags. Insert a filename-fallback row only when the path
         * is new, so a new file (e.g. an m4a with an oversized embedded cover) is still indexed + playable. */
        sqlite3_stmt *chk = NULL; int exists = 0;
        if(sqlite3_prepare_v2(g_db, "SELECT 1 FROM SONG WHERE PATH=?1 LIMIT 1;", -1, &chk, NULL) != SQLITE_OK){ g_scan_err=1; return; }
        if(sqlite3_bind_text(chk, 1, path, -1, SQLITE_TRANSIENT) != SQLITE_OK){ sqlite3_finalize(chk); g_scan_err=1; return; }
        int erc = sqlite3_step(chk);
        sqlite3_finalize(chk);
        if(erc == SQLITE_ROW) exists = 1;
        else if(erc != SQLITE_DONE){ g_scan_err=1; return; }   /* query error -> fail closed; never risk a duplicate INSERT on unknown state */
        if(exists){
            /* Preserve its tags while replacing the length with this scan's value (0 on failure). */
            sqlite3_stmt *du = NULL;
            /* the album comes from the folder, not the unreadable tags, so it is still refreshed */
            if(sqlite3_prepare_v2(g_db, "UPDATE SONG SET DURATION=?2,ALBUM=?3,ALBUM_CODE=?4 WHERE PATH=?1 AND COALESCE(IS_CUE,0)=0 "
                                        "AND COALESCE(IS_ISO,0)=0;", -1, &du, NULL) != SQLITE_OK){ g_scan_err=1; return; }
            sqlite3_bind_text(du, 1, path, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(du, 2, (sqlite3_int64)dur_ms);
            sqlite3_bind_text(du, 3, album, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(du, 4, code4(album));
            int urc = sqlite3_step(du);
            sqlite3_finalize(du);
            if(urc != SQLITE_DONE){ g_scan_err=1; return; }
            pthread_mutex_lock(&g_mu); g_done++; pthread_mutex_unlock(&g_mu);
            return;   /* preserve the existing tags */
        }
        /* new path -> fall through to the shared INSERT below with the filename fallback */
    } else {
        /* tst == 1: UPDATE in place (keeps ID/ACCENT/ADD_TIME). params 1..10 = tags, 11 = PATH (WHERE). */
        sqlite3_reset(g_upd); sqlite3_clear_bindings(g_upd);
        bind_tags(g_upd, 1, fname, title, album, artist, genre);
        sqlite3_bind_text(g_upd, 11, path, -1, SQLITE_TRANSIENT);
        bind_meta(g_upd, 12, disc, track, album_artist);
        sqlite3_bind_int64(g_upd, 16, (sqlite3_int64)dur_ms);
        sqlite3_bind_int(g_upd,17,title_tag);
        if(sqlite3_step(g_upd)!=SQLITE_DONE){ g_scan_err=1; return; }
        if(sqlite3_changes(g_db) > 0){                     /* existing PATH updated in place */
            pthread_mutex_lock(&g_mu); g_done++; pthread_mutex_unlock(&g_mu);
            return;
        }
    }
    /* new PATH -> INSERT (new autoincrement ID, ACCENT = schema default 0). params: 1=PATH, 2..11 tags, 12=ADD_TIME */
    sqlite3_reset(g_ins); sqlite3_clear_bindings(g_ins);
    sqlite3_bind_text(g_ins, 1, path, -1, SQLITE_TRANSIENT);
    bind_tags(g_ins, 2, fname, title, album, artist, genre);
    sqlite3_bind_int64(g_ins, 12, (sqlite3_int64)time(NULL));   /* ADD_TIME = now (new rows only; UPDATE keeps the original) */
    bind_meta(g_ins, 13, disc, track, album_artist);
    sqlite3_bind_int64(g_ins, 17, (sqlite3_int64)dur_ms);   /* normalized above: failures are unknown */
    if(tst<0) sqlite3_bind_null(g_ins,18);
    else sqlite3_bind_int(g_ins,18,title_tag);
    if(sqlite3_step(g_ins)==SQLITE_DONE){ pthread_mutex_lock(&g_mu); g_done++; pthread_mutex_unlock(&g_mu); }
    else g_scan_err=1;   /* insert failure (disk full, DB corruption) must block the commit */
}

/* ---- CUE sheets (L16) ----
 * Stock V2.57's scanner turns an EXTERNAL .cue sheet into one SONG row per track (add_cue_song, P257 0x443160;
 * internal RE notes) and drops the whole-file row of the audio it describes; the player then plays a row
 * from OFFSET for DURATION (both ms). diskOS writes the same rows: PATH = the audio file the sheet names, TRACK = the
 * sheet's track number, IS_CUE=1, DISC=0x7fffffff, ALBUM_ARTIST NULL; TITLE / PERFORMER / REM GENRE from the sheet,
 * else the audio file's own tags. Deliberate differences from stock: frames are 1/75 s (stock reads them as 10 ms),
 * NAME is the file name without its extension (stock cuts 4 characters), and sort codes use diskOS's code4 like
 * every other row. A sheet is skipped - the file keeps its ordinary row - when it names no usable file, has a track
 * without INDEX 01, offsets that don't increase or run past the file, or when the file's length is unknown (the last
 * track's length is file length - its offset). Embedded sheets are not read (stock keeps the ordinary row there too).
 * Sheets are processed after the walk, inside the scan transaction; rows keep their IDs across rescans (matched on
 * PATH + TRACK), and the file's ordinary row BECOMES the first track (same ID: resume and favourites keep pointing at
 * it). Files diskOS expanded are listed in DISKOS_CUE_FILES: when no sheet writes such a file any more (deleted, or now
 * unusable) its first track turns back into the ordinary row and the other tracks go. CUE rows stock wrote are left
 * alone, with one exception: when a sheet diskOS reads describes a file whose only rows are stock CUE rows (stock's
 * external-sheet shape), that sheet is the source of truth - its tracks update the stock rows in place (same IDs),
 * tracks it no longer lists go, and the file becomes diskOS's. A file with stock CUE rows BESIDE its ordinary row
 * (stock's embedded-sheet shape) is never touched by an external sheet. A sheet may only name a file the walk itself indexed (no "..", no absolute path; "." and empty parts
 * are dropped), so symlinks and paths outside the card are never reached. A sheet that cannot be READ (I/O error)
 * protects its parent subtree from retirement and pruning. Sheets for .m4b audiobooks and sheets with non-AUDIO
 * tracks are ignored. Work is bounded: 256 KB per sheet, 32 MB of sheets per scan. */
#define CUE_MAX_BYTES  (256 * 1024)
#define CUE_MAX_TRACKS 99
#define CUE_MAX_FILES  64
#define CUE_MAX_SHEETS 4096
#define CUE_MAX_TOTAL  (32LL * 1024 * 1024)   /* sheet bytes read per scan; the rest wait for the next one */
static long long g_cue_bytes;
static sqlite3_stmt *g_cue_up, *g_cue_in, *g_cue_seen;   /* prepared once per scan (cue_finish) */
typedef struct { int num; long long ms; char title[TAGLEN], perf[TAGLEN]; } cue_track_t;
typedef struct { char name[MAXPATH]; int first, ntr; } cue_file_t;   /* its tracks: tr[first .. first+ntr) */
typedef struct { char title[TAGLEN], perf[TAGLEN], genre[TAGLEN]; int nfiles, ntr, bad;
                 cue_file_t f[CUE_MAX_FILES]; cue_track_t tr[CUE_MAX_TRACKS]; } cue_sheet_t;   /* ~120 KB, one per scan */
static char (*g_cues)[MAXPATH];   /* .cue paths seen by the walk (processed after it) */
static int g_ncues, g_cues_cap;

static void cue_note(const char *path){
    if(g_ncues >= CUE_MAX_SHEETS){ protect_parent(path, "cue-count"); return; }   /* can't track it -> never delete rows it may own */
    if(g_ncues == g_cues_cap){
        int nc = g_cues_cap ? g_cues_cap * 2 : 64;
        if(nc > CUE_MAX_SHEETS) nc = CUE_MAX_SHEETS;
        void *p = realloc(g_cues, (size_t)nc * sizeof *g_cues);
        if(!p){ protect_parent(path, "cue-allocation"); return; }
        g_cues = p; g_cues_cap = nc;
    }
    snprintf(g_cues[g_ncues++], MAXPATH, "%s", path);
}
/* 1 if s[0..n) is well-formed UTF-8 (no overlongs/surrogates) */
static int utf8_ok(const unsigned char *s, size_t n){
    for(size_t i = 0; i < n; ){
        unsigned c = s[i];
        if(c < 0x80){ i++; continue; }
        int k = (c >= 0xC2 && c <= 0xDF) ? 1 : (c >= 0xE0 && c <= 0xEF) ? 2 : (c >= 0xF0 && c <= 0xF4) ? 3 : -1;
        if(k < 0 || i + (size_t)k >= n) return 0;          /* the continuation bytes must all be inside s */
        unsigned cp = c & (0x3F >> k);
        for(int j = 1; j <= k; j++){ if((s[i + j] & 0xC0) != 0x80) return 0; cp = (cp << 6) | (s[i + j] & 0x3F); }
        if((k == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (k == 3 && (cp < 0x10000 || cp > 0x10FFFF))) return 0;
        i += (size_t)k + 1;
    }
    return 1;
}
/* a sheet value: "quoted" (to the closing quote) or the rest of the word run, trimmed; UTF-8 in */
static void cue_value(const char *p, char *out, int cap){
    while(*p == ' ' || *p == '\t') p++;
    int n = 0;
    if(*p == '"'){ p++; while(*p && *p != '"' && n < cap - 1) out[n++] = *p++; }
    else { while(*p && n < cap - 1) out[n++] = *p++; }
    out[n] = 0;
    str_trim(out);
}
/* "mm:ss:ff" (ff = 1/75 s frames) -> ms, -1 if malformed */
static long long cue_time(const char *p){
    while(*p == ' ' || *p == '\t') p++;
    long long v[3] = {0, 0, 0};
    for(int k = 0; k < 3; k++){
        int nd = 0;
        while(*p >= '0' && *p <= '9' && nd < 6){ v[k] = v[k] * 10 + (*p - '0'); p++; nd++; }
        if(nd == 0) return -1;
        if(k < 2){ if(*p != ':') return -1; p++; }
    }
    if(v[1] > 59 || v[2] > 74) return -1;
    return (v[0] * 60 + v[1]) * 1000 + v[2] * 1000 / 75;
}
static int kw(const char *p, const char *k){ size_t n = strlen(k); return !strncasecmp(p, k, n) && (p[n] == ' ' || p[n] == '\t'); }
/* parse UTF-8 sheet text into *cs (bounded); cs->bad = 1 on a structural error */
static void cue_parse(char *text, cue_sheet_t *cs){
    memset(cs, 0, sizeof *cs);
    cue_file_t *f = NULL; cue_track_t *t = NULL;
    for(char *line = text, *next; line && *line; line = next){
        next = strpbrk(line, "\r\n");
        if(next){ *next++ = 0; while(*next == '\r' || *next == '\n') next++; }
        char *p = line; while(*p == ' ' || *p == '\t') p++;
        if(kw(p, "FILE")){
            if(cs->nfiles >= CUE_MAX_FILES){ cs->bad = 1; return; }
            f = &cs->f[cs->nfiles++]; t = NULL; f->first = cs->ntr; f->ntr = 0;
            char v[MAXPATH]; cue_value(p + 4, v, sizeof v);
            if(strchr(p + 4, '"') == NULL){                /* unquoted: FILE name TYPE -> drop the type word */
                char *sp = strrchr(v, ' '); if(sp) *sp = 0;
            }
            snprintf(f->name, sizeof f->name, "%s", v);
        } else if(kw(p, "TRACK")){
            if(!f || cs->ntr >= CUE_MAX_TRACKS){ cs->bad = 1; return; }
            t = &cs->tr[cs->ntr++]; f->ntr++;
            char *q = p + 5; while(*q == ' ' || *q == '\t') q++;
            t->num = atoi(q); t->ms = -1;
            if(t->num < 1 || t->num > CUE_MAX_TRACKS){ cs->bad = 1; return; }
            while(*q >= '0' && *q <= '9') q++;
            while(*q == ' ' || *q == '\t') q++;
            if(strncasecmp(q, "AUDIO", 5) != 0){ cs->bad = 1; return; }   /* a data track (mixed-mode disc): not handled */
        } else if(kw(p, "INDEX")){
            char *q = p + 5; while(*q == ' ' || *q == '\t') q++;
            if(t && atoi(q) == 1){ while(*q >= '0' && *q <= '9') q++; t->ms = cue_time(q); }
        } else if(kw(p, "TITLE")){
            cue_value(p + 5, t ? t->title : cs->title, TAGLEN);
        } else if(kw(p, "PERFORMER")){
            cue_value(p + 9, t ? t->perf : cs->perf, TAGLEN);
        } else if(kw(p, "REM") && !t){
            char *q = p + 3; while(*q == ' ' || *q == '\t') q++;
            if(kw(q, "GENRE")) cue_value(q + 5, cs->genre, TAGLEN);
        }
    }
}
/* 1 if the walk indexed exactly this path this scan (in `seen`) */
static int cue_seen(const char *path){
    sqlite3_reset(g_cue_seen); sqlite3_clear_bindings(g_cue_seen);
    sqlite3_bind_text(g_cue_seen, 1, path, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(g_cue_seen);
    if(rc != SQLITE_ROW && rc != SQLITE_DONE) g_scan_err = 1;
    return rc == SQLITE_ROW;
}
/* the audio file a FILE line names, relative to the sheet's folder: no absolute path, no ".." part; "." and empty parts
 * are dropped so the path matches the walk's spelling, and the walk must have indexed it. When it isn't there, the same
 * name with another audio extension (a sheet written for a .wav that was converted to .flac). */
static int cue_backing_ex(const char *cue_path, const char *name, char *out, int cap, int (*seen)(const char *)){
    if(!name[0] || name[0] == '/' || name[0] == '\\') return 0;
    char dir[MAXPATH]; snprintf(dir, sizeof dir, "%s", cue_path);
    char *sl = strrchr(dir, '/'); if(!sl) return 0; *sl = 0;
    char rel[MAXPATH]; int rl = 0;
    for(const char *p = name; *p; ){
        const char *e = p; while(*e && *e != '/' && *e != '\\') e++;
        size_t n = (size_t)(e - p);
        if(n == 2 && p[0] == '.' && p[1] == '.') return 0;
        if(n && !(n == 1 && p[0] == '.')){
            if(rl + (int)n + 2 >= (int)sizeof rel) return 0;
            if(rl) rel[rl++] = '/';
            memcpy(rel + rl, p, n); rl += (int)n;
        }
        p = *e ? e + 1 : e;
    }
    rel[rl] = 0;
    if(!rl) return 0;
    if(snprintf(out, cap, "%s/%s", dir, rel) < cap && is_audio(out) && seen(out)) return 1;
    static const char *const EXT[] = { ".flac", ".wav", ".ape", ".m4a", ".mp3", ".wma", ".ogg", ".aiff", ".aif", ".dsf" };
    char *dot = strrchr(rel, '.'); char *rs = strrchr(rel, '/');
    if(!dot || (rs && dot < rs)) return 0;
    *dot = 0;
    for(unsigned i = 0; i < sizeof EXT / sizeof EXT[0]; i++)
        if(snprintf(out, cap, "%s/%s%s", dir, rel, EXT[i]) < cap && is_audio(out) && seen(out)) return 1;
    return 0;
}
static int cue_backing(const char *cue_path, const char *name, char *out, int cap){ return cue_backing_ex(cue_path, name, out, cap, cue_seen); }
static const char *CUE_UPD =
    "UPDATE SONG SET NAME=?3,TITLE=?4,ALBUM=?5,ARTIST=?6,GENRE=?7,OFFSET=?8,DURATION=?9,NAME_CODE=?10,TITLE_CODE=?11,"
    "ALBUM_CODE=?12,ARTIST_CODE=?13,GENRE_CODE=?14 WHERE PATH=?1 AND TRACK=?2 AND COALESCE(IS_CUE,0)=1;";
static const char *CUE_INS =
    "INSERT INTO SONG (PATH,TRACK,NAME,TITLE,ALBUM,ARTIST,GENRE,OFFSET,DURATION,NAME_CODE,TITLE_CODE,ALBUM_CODE,ARTIST_CODE,"
    "GENRE_CODE,ADD_TIME,DISC,IS_CUE,IS_ISO,IS_DSD,SAMPLE_RATE,BIT_PER_SAMPLE,CHANNELS,BIT_RATE,SONG_MIMETYPE,"
    "SONG_PRODUCTION_YEAR,IS_SELECT,ALBUM_ARTIST,ALBUM_ARTIST_CODE) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,"
    "?15,2147483647,1,0,0,0,0,0,0,NULL,NULL,0,NULL,2147483647);";
static int db_exec_path(const char *sql, const char *path, const char *extra){
    sqlite3_stmt *s = NULL;
    if(sqlite3_prepare_v2(g_db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(s, 1, path, -1, SQLITE_TRANSIENT);
    if(extra) sqlite3_bind_text(s, 2, extra, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return (rc == SQLITE_DONE || rc == SQLITE_ROW) ? 0 : -1;
}
/* write one FILE of a sheet as CUE rows; 1 done, 0 skipped (the file keeps its ordinary row), -1 DB error */
static int cue_write_file(const cue_sheet_t *cs, const cue_file_t *f, const char *audio){
    if(f->ntr < 1) return 0;
    const cue_track_t *tr = cs->tr + f->first;
    { sqlite3_stmt *q = NULL; int claimed = 0;           /* another sheet already described this file */
      if(sqlite3_prepare_v2(g_db, "SELECT 1 FROM cue_claimed WHERE PATH=?1;", -1, &q, NULL) != SQLITE_OK) return -1;
      sqlite3_bind_text(q, 1, audio, -1, SQLITE_TRANSIENT);
      int rc = sqlite3_step(q); sqlite3_finalize(q);
      if(rc == SQLITE_ROW) claimed = 1; else if(rc != SQLITE_DONE) return -1;
      if(claimed) return 0; }
    const char *base = strrchr(audio, '/'); base = base ? base + 1 : audio;
    char title[TAGLEN], artist[TAGLEN], album[TAGLEN], genre[TAGLEN], aa[TAGLEN]; int trk = 0, disc = 0; long long dur = 0;
    int tst = tags_from_file(audio, base, title, artist, album, genre, aa, &trk, &disc, &dur);
    if(tst <= 0 || dur < 0){ protect_root(audio, 0, "cue-audio-read"); return 0; }
    if(dur == 0) return 0;                  /* readable, length unknown -> ordinary row */
    if(has_ext(audio, ".m4b")) return 0;                /* an audiobook: never a CUE row in the music table */
    { sqlite3_stmt *sh = NULL; int shape = -1;          /* stock embedded shape: CUE rows + ordinary row, not ours */
      if(sqlite3_prepare_v2(g_db, "SELECT EXISTS(SELECT 1 FROM SONG WHERE PATH=?1 AND COALESCE(IS_CUE,0)=1) AND "
            "EXISTS(SELECT 1 FROM SONG WHERE PATH=?1 AND COALESCE(IS_CUE,0)=0 AND COALESCE(IS_ISO,0)=0) AND NOT "
            "EXISTS(SELECT 1 FROM DISKOS_CUE_FILES WHERE PATH=?1);", -1, &sh, NULL) != SQLITE_OK) return -1;
      sqlite3_bind_text(sh, 1, audio, -1, SQLITE_TRANSIENT);
      if(sqlite3_step(sh) == SQLITE_ROW) shape = sqlite3_column_int(sh, 0);
      sqlite3_finalize(sh);
      if(shape < 0) return -1;
      if(shape) return 0; }
    for(int i = 0; i < f->ntr; i++){                    /* offsets present, increasing, inside the file; numbers unique */
        if(tr[i].ms < 0 || tr[i].ms >= dur) return 0;
        if(i && (tr[i].ms <= tr[i - 1].ms || tr[i].num <= tr[i - 1].num)) return 0;
    }
    char name[MAXPATH]; snprintf(name, sizeof name, "%s", base);
    char *dot = strrchr(name, '.'); if(dot && dot != name) *dot = 0;
    sqlite3_stmt *up = g_cue_up, *in = g_cue_in;
    /* the ordinary row becomes the first track (keeps its ID) unless that track already exists */
    { sqlite3_stmt *cv = NULL;
      if(sqlite3_prepare_v2(g_db, "UPDATE SONG SET IS_CUE=1,IS_ISO=0,TRACK=?2,DISC=2147483647,ALBUM_ARTIST=NULL,"
            "ALBUM_ARTIST_CODE=2147483647 WHERE ID=(SELECT MIN(ID) FROM SONG WHERE PATH=?1 AND COALESCE(IS_CUE,0)=0 "
            "AND COALESCE(IS_ISO,0)=0) AND NOT EXISTS (SELECT 1 FROM SONG WHERE PATH=?1 AND COALESCE(IS_CUE,0)=1 "
            "AND TRACK=?2);", -1, &cv, NULL) != SQLITE_OK) return -1;
      sqlite3_bind_text(cv, 1, audio, -1, SQLITE_TRANSIENT); sqlite3_bind_int(cv, 2, tr[0].num);
      int crc = sqlite3_step(cv); sqlite3_finalize(cv);
      if(crc != SQLITE_DONE) return -1; }
    int rc = 1;
    char keep[CUE_MAX_TRACKS * 4 + 8] = ""; int kl = 0;
    for(int i = 0; i < f->ntr && rc == 1; i++){
        const cue_track_t *t = &tr[i];
        long long len = (i + 1 < f->ntr ? tr[i + 1].ms : dur) - t->ms;
        char tt[TAGLEN], ta[TAGLEN], tb[TAGLEN], tg[TAGLEN];
        snprintf(tt, sizeof tt, "%.*s", TAGLEN - 1, t->title[0] ? t->title : name);   /* tags are TAGLEN, like every row */
        snprintf(tb, sizeof tb, "%s", album);   /* the audio file's folder (folder_album), not the sheet's TITLE */
        snprintf(ta, sizeof ta, "%s", t->perf[0] ? t->perf : cs->perf[0] ? cs->perf : artist);
        snprintf(tg, sizeof tg, "%s", cs->genre[0] ? cs->genre : genre);
        for(int pass = 0; pass < 2 && rc == 1; pass++){
            sqlite3_stmt *s = pass ? in : up;
            sqlite3_reset(s); sqlite3_clear_bindings(s);
            sqlite3_bind_text(s, 1, audio, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(s, 2, t->num);
            sqlite3_bind_text(s, 3, name, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 4, tt, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 5, tb, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 6, ta, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 7, tg, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 8, (sqlite3_int64)t->ms);
            sqlite3_bind_int64(s, 9, (sqlite3_int64)len);
            sqlite3_bind_int(s, 10, code4(name)); sqlite3_bind_int(s, 11, code4(tt)); sqlite3_bind_int(s, 12, code4(tb));
            sqlite3_bind_int(s, 13, code4(ta)); sqlite3_bind_int(s, 14, code4(tg));
            if(pass) sqlite3_bind_int64(s, 15, (sqlite3_int64)time(NULL));
            if(sqlite3_step(s) != SQLITE_DONE){ rc = -1; break; }
            if(!pass && sqlite3_changes(g_db) > 0) break;   /* existing row updated in place (keeps its ID) */
        }
        kl += snprintf(keep + kl, sizeof keep - (size_t)kl, "%s%d", kl ? "," : "", t->num);
    }
    if(rc != 1) return rc;
    char sql[sizeof keep + 128];                        /* tracks the sheet no longer lists (keep = digits and commas) */
    snprintf(sql, sizeof sql, "DELETE FROM SONG WHERE PATH=?1 AND COALESCE(IS_CUE,0)=1 AND TRACK NOT IN (%s);", keep);
    if(db_exec_path(sql, audio, NULL) != 0) return -1;
    if(db_exec_path("DELETE FROM SONG WHERE PATH=?1 AND COALESCE(IS_CUE,0)=0 AND COALESCE(IS_ISO,0)=0;", audio, NULL) != 0) return -1;
    if(db_exec_path("INSERT OR IGNORE INTO cue_claimed(PATH) VALUES(?1);", audio, NULL) != 0) return -1;
    if(db_exec_path("INSERT OR IGNORE INTO DISKOS_CUE_FILES(PATH) VALUES(?1);", audio, NULL) != 0) return -1;
    return 1;
}
enum { CR_OK, CR_ENOENT, CR_OPEN_ERR, CR_IOERR, CR_SKIP, CR_OOM, CR_LIMIT };
/* Read and parse one sheet (<= CUE_MAX_BYTES, UTF-8 or Latin-1, BOM dropped) into *cs. *nread = bytes read (budget). */
static int cue_read_sheet(const char *cue_path, cue_sheet_t *cs, size_t *nread){
    *nread = 0;
    FILE *fp = fopen(cue_path, "rb");
    if(!fp) return errno == ENOENT ? CR_ENOENT : CR_OPEN_ERR;
    char *raw = malloc(CUE_MAX_BYTES + 1);
    if(!raw){ fclose(fp); return CR_OOM; }
    size_t n = fread(raw, 1, CUE_MAX_BYTES + 1, fp);
    int err = ferror(fp); fclose(fp);
    *nread = n;
    if(err){ free(raw); return CR_IOERR; }
    if(n > CUE_MAX_BYTES){ free(raw); return CR_LIMIT; }         /* not a sheet we read */
    size_t off = (n >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF) ? 3 : 0;
    char *text;
    if(memchr(raw + off, 0, n - off)){ free(raw); return CR_SKIP; }   /* binary / UTF-16: not handled */
    if(utf8_ok((unsigned char *)raw + off, n - off)){ raw[n] = 0; text = raw + off; }
    else {                                              /* legacy 8-bit sheet: CP1251 if it looks Cyrillic, else Latin-1 */
        text = malloc((n - off) * 3 + 1);               /* a byte becomes at most 3 UTF-8 bytes (CP1251's dashes, quotes) */
        if(!text){ free(raw); return CR_OOM; }
        raw[n] = 0;
        legacy_decode((unsigned char *)raw + off, n - off, text, (int)((n - off) * 3 + 1));
        free(raw); raw = text;
    }
    cue_parse(text, cs);
    free(raw);
    return CR_OK;
}
/* Browse Files: the audio files a sheet names, resolved exactly like the scan does (same bounds, same path
 * normalisation and traversal guard); `seen(path)` says whether a candidate file is in the library. Writes up to `max`
 * distinct MAXPATH-sized paths to `out`; returns the count (0 = unreadable, not a usable sheet, or nothing resolved). */
int scan_cue_files(const char *cue_path, char (*out)[MAXPATH], int max, int (*seen)(const char *)){
    if(!cue_path || !out || max <= 0 || !seen) return 0;
    cue_sheet_t *cs = malloc(sizeof *cs);
    if(!cs) return 0;
    size_t n; int n_out = 0;
    if(cue_read_sheet(cue_path, cs, &n) == CR_OK && !cs->bad){
        for(int i = 0; i < cs->nfiles && n_out < max; i++){
            char audio[MAXPATH]; int dup = 0;
            if(!cue_backing_ex(cue_path, cs->f[i].name, audio, sizeof audio, seen)) continue;
            for(int j = 0; j < n_out; j++) if(!strcmp(out[j], audio)) dup = 1;
            if(!dup) snprintf(out[n_out++], MAXPATH, "%s", audio);
        }
    }
    free(cs);
    return n_out;
}
static void cue_process(const char *cue_path, cue_sheet_t *cs){
    if(g_cue_bytes >= CUE_MAX_TOTAL){ protect_parent(cue_path, "cue-budget"); return; }   /* over budget: its rows wait, untouched */
    size_t n; int rc = cue_read_sheet(cue_path, cs, &n);
    g_cue_bytes += (long long)n;
    if(rc == CR_SKIP) return;   /* readable invalid replacement: existing retirement policy */
    if(rc == CR_ENOENT || rc == CR_LIMIT || rc == CR_OPEN_ERR || rc == CR_IOERR){
        protect_parent(cue_path, "cue-read"); return;
    }   /* unreadable sheet: its rows must not be dropped */
    if(rc == CR_OOM){ protect_parent(cue_path, "cue-allocation"); return; }
    if(cs->bad) return;
    for(int i = 0; i < cs->nfiles && !g_scan_err; i++){
        char audio[MAXPATH];
        if(!cue_backing(cue_path, cs->f[i].name, audio, sizeof audio)) continue;
        if(cue_write_file(cs, &cs->f[i], audio) < 0) g_scan_err = 1;
    }
}
/* after the walk: write every sheet's rows, then retire rows of sheets that are gone */
static void cue_finish(void){
    if(sqlite3_exec(g_db, "CREATE TEMP TABLE IF NOT EXISTS cue_claimed(PATH TEXT PRIMARY KEY); DELETE FROM cue_claimed;"
                          "CREATE TABLE IF NOT EXISTS DISKOS_CUE_FILES(PATH TEXT PRIMARY KEY);", 0,0,0) != SQLITE_OK){
        g_scan_err = 1; return;
    }
    g_cue_bytes = 0; g_cue_up = g_cue_in = g_cue_seen = NULL;
    cue_sheet_t *cs = malloc(sizeof *cs);
    if(!cs){
        for(int i = 0; i < g_ncues && !g_scan_err; i++) protect_parent(g_cues[i], "cue-allocation");
    } else if(sqlite3_prepare_v2(g_db, CUE_UPD, -1, &g_cue_up, NULL) != SQLITE_OK
           || sqlite3_prepare_v2(g_db, CUE_INS, -1, &g_cue_in, NULL) != SQLITE_OK
           || sqlite3_prepare_v2(g_db, "SELECT 1 FROM seen WHERE PATH=?1;", -1, &g_cue_seen, NULL) != SQLITE_OK) g_scan_err = 1;
    for(int i = 0; cs && i < g_ncues && !g_scan_err && !atomic_load(&g_abort); i++) cue_process(g_cues[i], cs);
    free(cs);
    sqlite3_finalize(g_cue_up); sqlite3_finalize(g_cue_in); sqlite3_finalize(g_cue_seen);
    g_cue_up = g_cue_in = g_cue_seen = NULL;
    if(g_scan_err || g_prune_blocked) return;           /* preservation failed: keep all rows */
    /* files diskOS expanded that no sheet wrote this scan (deleted or now unusable): the first track turns back into the
     * ordinary row (same ID) and the other tracks go (only DISKOS_CUE_FILES - rows stock wrote are never removed here).
     * At most CUE_MAX_SHEETS per scan; the rest follow on the next one. */
    sqlite3_stmt *q = NULL;
    if(sqlite3_prepare_v2(g_db, "SELECT PATH, PATH IN (SELECT PATH FROM seen) FROM DISKOS_CUE_FILES WHERE PATH LIKE '" SCAN_ROOT "/%' "
                                "AND PATH NOT IN (SELECT PATH FROM cue_claimed) AND PATH NOT IN (SELECT PATH FROM unreadable) AND "
                                PRUNE_ALLOWED("DISKOS_CUE_FILES.PATH") " LIMIT 4096;", -1, &q, NULL) != SQLITE_OK){
        g_scan_err = 1; return;
    }
    struct retired_path { char path[MAXPATH]; int observed; } *gone = NULL;
    int ng = 0, cap = 0, rc;
    while((rc = sqlite3_step(q)) == SQLITE_ROW){
        const unsigned char *p = sqlite3_column_text(q, 0);
        if(!p) continue;
        if(ng == cap){ int nc = cap ? cap * 2 : 16; void *m = realloc(gone, (size_t)nc * sizeof *gone); if(!m){ rc = SQLITE_NOMEM; break; } gone = m; cap = nc; }
        gone[ng].observed = sqlite3_column_int(q, 1);
        snprintf(gone[ng++].path, MAXPATH, "%s", (const char *)p);
    }
    sqlite3_finalize(q);
    if(rc != SQLITE_DONE){ free(gone); g_scan_err = 1; return; }
    for(int i = 0; i < ng && !g_scan_err; i++){
        if(atomic_load(&g_abort)){ g_scan_err = 1; break; }   /* cancel: stop per item */
        if(gone[i].observed){
            FILE *f = fopen(gone[i].path, "rb");
            if(!f){ protect_root(gone[i].path, 0, "cue-retirement-open"); continue; }
            int bad = fgetc(f) == EOF && ferror(f);
            fclose(f);
            if(bad){ protect_root(gone[i].path, 0, "cue-retirement-read"); continue; }
        }
        if(db_exec_path("UPDATE SONG SET IS_CUE=0,TRACK=0,DISC=0,OFFSET=0,ALBUM_ARTIST_CODE=0 WHERE ID=(SELECT MIN(ID) FROM SONG WHERE PATH=?1 "
                        "AND COALESCE(IS_CUE,0)=1) AND NOT EXISTS (SELECT 1 FROM SONG WHERE PATH=?1 AND COALESCE(IS_CUE,0)=0 "
                        "AND COALESCE(IS_ISO,0)=0);", gone[i].path, NULL) != 0
           || db_exec_path("DELETE FROM SONG WHERE PATH=?1 AND COALESCE(IS_CUE,0)=1;", gone[i].path, NULL) != 0
           || db_exec_path("DELETE FROM DISKOS_CUE_FILES WHERE PATH=?1;", gone[i].path, NULL) != 0){ g_scan_err = 1; break; }
        struct stat st;
        const char *b = strrchr(gone[i].path, '/');
        unsigned protections = g_protect_count;
        if(lstat(gone[i].path, &st) == 0 && S_ISREG(st.st_mode) && b && is_audio(b + 1))
            upsert_song(gone[i].path, b + 1);
        else if(gone[i].observed) g_scan_err = 1;
        /* A failed refresh was discovered after retirement writes: undo the scan. */
        if(g_protect_count != protections) g_scan_err = 1;
    }
    free(gone);
}

/* ---- SACD disc images (L17) ----
 * Stock V2.57's scanner turns a SACD .iso into one SONG row per track of its first readable area (add_iso_song, P257
 * 0x443894; parser 0x621140..; internal RE notes), and the player re-opens the image and plays a row by its
 * TRACK (OFFSET is 0; the image itself never gets an ordinary row). diskOS writes the same rows from the same
 * structures: 2048-byte sectors; Master TOC "SACDMTOC" at LSN 510 (version <= 1.20; area 1 start/size at +64/+84,
 * area 2 at +72/+86); an area's "TWOCHTOC"/"MULCHTOC" TOC (track count at +69, text locale charset at +90), then its
 * "SACDTTxt" (per-track TITLE item 1 / PERFORMER item 2), "SACD_IGL" (2 sectors), "SACD_ACC" (32), "SACDTRL1" and
 * "SACDTRL2" (per-track duration min/sec/frame at +8+4*255+4*i). An image whose track list (SACDTRL1) points past its end
 * (cut short) gets no rows - stock doesn't check, but those tracks could not play. Rows: PATH = the image, NAME = its file name without
 * the extension, TRACK 1..n, IS_ISO=1, OFFSET 0, DISC 0x7fffffff, ALBUM_ARTIST NULL. Differences from stock, on
 * purpose: DURATION counts frames as 1/75 s (stock 10 ms), ALBUM/ARTIST fall back to the disc's own album title and
 * artist (Master Text +16/+18; stock always writes "unknown album"), and text in the disc's charset (Shift-JIS,
 * EUC-KR, GB2312, Big5) is converted to UTF-8 with iconv. A missing TITLE is "Track N". The rows come from the
 * STEREO area when the disc has one (the player plays that area; stock's scanner takes the first readable area, which
 * on a multichannel-first disc lists tracks that don't match playback), else the first readable area. An image that
 * reads fine but is no longer a usable SACD image loses its rows. Rows keep their IDs across
 * rescans (PATH + TRACK); tracks the image no longer has go; an unreadable image keeps its rows (the walk marked it
 * seen) and a deleted one is pruned with the other formats. */
#include <iconv.h>
#define ISO_MAX 1024
#define ISO_SECT 2048
static char (*g_isos)[MAXPATH];
static int g_nisos, g_isos_cap;
static void iso_note(const char *path){
    if(g_nisos >= ISO_MAX){ protect_root(path, 0, "iso-count"); return; }
    if(g_nisos == g_isos_cap){
        int nc = g_isos_cap ? g_isos_cap * 2 : 16; if(nc > ISO_MAX) nc = ISO_MAX;
        void *p = realloc(g_isos, (size_t)nc * sizeof *g_isos);
        if(!p){ protect_root(path, 0, "iso-allocation"); return; }
        g_isos = p; g_isos_cap = nc;
    }
    snprintf(g_isos[g_nisos++], MAXPATH, "%s", path);
}
/* one sector: 1 read, 0 not in the file (structure problem), -1 I/O error (musl's fseeko does not set the stream error
 * flag on a failed seek, so it is reported here, not left to ferror) */
static int g_iso_ioerr;
static int iso_read(FILE *f, off_t fsz, long lsn, unsigned char *buf){
    off_t at = (off_t)lsn * ISO_SECT;
    if(lsn < 0 || at + ISO_SECT > fsz) return 0;
    if(fseeko(f, at, SEEK_SET) != 0 || fread(buf, 1, ISO_SECT, f) != ISO_SECT){ g_iso_ioerr = 1; return -1; }
    return 1;
}
static unsigned be16u(const unsigned char *p){ return (unsigned)p[0] << 8 | p[1]; }
static unsigned long be32u(const unsigned char *p){ return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned long)p[2] << 8 | p[3]; }
/* disc text (NUL-terminated within the sector, from byte `off`) -> UTF-8 in out, by the locale's charset byte */
static void iso_text(const unsigned char *sec, unsigned off, int charset, char *out, int cap){
    out[0] = 0;
    if(off < 8 || off >= ISO_SECT) return;
    size_t n = 0; while(off + n < ISO_SECT && sec[off + n]) n++;
    if(!n) return;
    static const char *const CS[8] = { NULL, NULL, NULL, "SHIFT_JIS", "EUC-KR", "GB2312", "BIG5", NULL };
    const char *cs = CS[charset & 7];
    if(cs){
        iconv_t cd = iconv_open("UTF-8", cs);
        if(cd != (iconv_t)-1){
            char in[ISO_SECT]; memcpy(in, sec + off, n);
            char *ip = in, *op = out; size_t il = n, ol = (size_t)cap - 1;
            size_t r = iconv(cd, &ip, &il, &op, &ol);
            iconv_close(cd);
            if(r != (size_t)-1){ *op = 0; str_trim(out); return; }
        }
    }
    if(utf8_ok(sec + off, n)){ snprintf(out, cap, "%.*s", (int)n, (const char *)sec + off); str_trim(out); return; }
    char *o = out, *end = out + cap - 1;                /* ISO 646 / 8859-1 (1, 2, 7) or undecodable: Latin-1 */
    for(size_t i = 0; i < n; i++) put_u8(&o, end, sec[off + i]);
    *o = 0; str_trim(out);
}
typedef struct { int n; long long dur[255]; char title[255][TAGLEN], perf[255][TAGLEN]; char album[TAGLEN], artist[TAGLEN]; } iso_disc_t;
/* 1 = parsed into *d, 0 = not a SACD image / unsupported, -1 = I/O error */
static int iso_parse(const char *path, iso_disc_t *d){
    memset(d, 0, sizeof *d);
    FILE *f = fopen(path, "rb");
    if(!f) return -1;   /* listed image became unopenable: not an invalid replacement */
    off_t fsz = file_size(f);
    unsigned char m[ISO_SECT], t[ISO_SECT], s[ISO_SECT];
    int ok = 0;
    g_iso_ioerr = 0;
    if(fsz < 0){ fclose(f); return -1; }
    if(iso_read(f, fsz, 510, m) != 1 || memcmp(m, "SACDMTOC", 8) || m[8] > 1 || m[9] > 20) goto out;   /* stock's limits */
    { unsigned char mt[ISO_SECT];                       /* the disc's own album title / artist (Master Text) */
      if(iso_read(f, fsz, 511, mt) == 1 && !memcmp(mt, "SACDText", 8)){
          iso_text(mt, be16u(mt + 16), m[138], d->album, TAGLEN);
          iso_text(mt, be16u(mt + 18), m[138], d->artist, TAGLEN);
      } }
    /* The player PLAYS the stereo area (it stores that area's index), stock's scanner lists the first area it can read.
     * Prefer the stereo area so the rows are the tracks that actually play; else the first readable one. */
    for(int pass = 0; pass < 2 && !ok && !g_iso_ioerr; pass++)           /* pass 0: stereo areas, pass 1: the others */
    for(int area = 0; area < 2 && !ok && !g_iso_ioerr; area++){
        long start = (long)be32u(m + (area ? 72 : 64));
        if(!start || iso_read(f, fsz, start, t) != 1) continue;
        if((memcmp(t, "TWOCHTOC", 8) && memcmp(t, "MULCHTOC", 8)) || t[8] > 1 || t[9] > 20) continue;
        int stereo = t[32] == 2 && t[33] == 0;                            /* channel count 2, speaker config 0 */
        if(stereo != (pass == 0)) continue;
        unsigned size = be16u(t + 10);                                    /* the area's own sector count (stock) */
        int n = t[69], charset = t[90];
        if(n < 1 || size < 1 || size > 4096) continue;
        memset(d->title, 0, sizeof d->title); memset(d->perf, 0, sizeof d->perf); memset(d->dur, 0, sizeof d->dur);
        int have_trl2 = 0, have_trl1 = 0, got_text = 0;
        for(long lsn = start + 1; lsn < start + (long)size; lsn++){   /* the area's other sectors, in order */
            if(atomic_load(&g_abort)){ g_scan_err = 1; break; }   /* user cancel inside a long image read */
            if(iso_read(f, fsz, lsn, s) != 1) break;
            if(!memcmp(s, "SACDTTxt", 8)){
                if(!got_text){
                    for(int i = 0; i < n; i++){
                        unsigned off = be16u(s + 8 + 2 * i);
                        if(off < 8 || off + 4 >= ISO_SECT) continue;
                        int items = s[off]; unsigned p = off + 4;
                        for(int k = 0; k < items && p + 2 < ISO_SECT; k++){
                            int type = s[p]; p += 2;
                            char txt[TAGLEN]; iso_text(s, p, charset, txt, sizeof txt);
                            if(type == 1) snprintf(d->title[i], TAGLEN, "%s", txt);
                            else if(type == 2) snprintf(d->perf[i], TAGLEN, "%s", txt);
                            while(p < ISO_SECT && s[p]) p++;                 /* past the text */
                            while(p < ISO_SECT && !s[p]) p++;                /* and its padding */
                        }
                    }
                    got_text = 1;
                }
            } else if(!memcmp(s, "SACD_IGL", 8)) lsn += 1;
            else if(!memcmp(s, "SACD_ACC", 8)) lsn += 31;
            else if(!memcmp(s, "SACDTRL1", 8)){                         /* every track's audio must be in the file */
                unsigned long long total = (unsigned long long)(fsz / ISO_SECT);
                for(int i = 0; i < n; i++){
                    unsigned long long st0 = be32u(s + 8 + 4 * i), ln = be32u(s + 8 + 4 * 255 + 4 * i);
                    if(!ln || st0 < (unsigned long long)start || st0 > total || ln > total - st0){ have_trl1 = -1; break; }
                }
                if(have_trl1 == 0) have_trl1 = 1;
            }
            else if(!memcmp(s, "SACDTRL2", 8)){
                for(int i = 0; i < n; i++){
                    const unsigned char *r = s + 8 + 4 * 255 + 4 * i;
                    if(r[1] > 59 || r[2] > 74){ have_trl2 = 0; break; }
                    d->dur[i] = ((long long)r[0] * 60 + r[1]) * 1000 + r[2] * 1000 / 75;
                    have_trl2 = 1;
                }
            } else break;                                                     /* unknown sector: the area ends */
        }
        if(!have_trl2 || have_trl1 != 1) continue;     /* no track list, or audio cut off: not a playable image */
        d->n = n; ok = 1;
    }
out:
    { int err = ferror(f) || g_iso_ioerr; fclose(f); if(err) return -1; }   /* any I/O error: never "not an image" */
    return ok;
}
static void iso_write(const char *path, const iso_disc_t *d){
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    char name[MAXPATH]; snprintf(name, sizeof name, "%s", base);
    char *dot = strrchr(name, '.'); if(dot && dot != name) *dot = 0;
    static const char *UPD =
        "UPDATE SONG SET NAME=?3,TITLE=?4,ALBUM=?5,ARTIST=?6,GENRE=?7,DURATION=?8,NAME_CODE=?9,TITLE_CODE=?10,ALBUM_CODE=?11,"
        "ARTIST_CODE=?12,GENRE_CODE=?13 WHERE PATH=?1 AND TRACK=?2 AND COALESCE(IS_ISO,0)=1;";
    static const char *INS =
        "INSERT INTO SONG (PATH,TRACK,NAME,TITLE,ALBUM,ARTIST,GENRE,DURATION,NAME_CODE,TITLE_CODE,ALBUM_CODE,ARTIST_CODE,"
        "GENRE_CODE,ADD_TIME,DISC,IS_CUE,IS_ISO,IS_DSD,OFFSET,SAMPLE_RATE,BIT_PER_SAMPLE,CHANNELS,BIT_RATE,SONG_MIMETYPE,"
        "SONG_PRODUCTION_YEAR,IS_SELECT,ALBUM_ARTIST,ALBUM_ARTIST_CODE) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,"
        "2147483647,0,1,0,0,0,0,0,0,NULL,NULL,0,NULL,2147483647);";
    sqlite3_stmt *up = NULL, *in = NULL;
    if(sqlite3_prepare_v2(g_db, UPD, -1, &up, NULL) != SQLITE_OK || sqlite3_prepare_v2(g_db, INS, -1, &in, NULL) != SQLITE_OK){
        sqlite3_finalize(up); g_scan_err = 1; return;
    }
    for(int i = 0; i < d->n && !g_scan_err; i++){
        char tt[TAGLEN], ta[TAGLEN];
        if(d->title[i][0]) snprintf(tt, sizeof tt, "%s", d->title[i]); else snprintf(tt, sizeof tt, "Track %d", i + 1);
        snprintf(ta, sizeof ta, "%s", d->perf[i][0] ? d->perf[i] : d->artist[0] ? d->artist : "Unknown artist");
        char al[TAGLEN]; folder_album(path, al, sizeof al);   /* the image's folder is its album */
        const char *ge = "Unknown genre";
        for(int pass = 0; pass < 2; pass++){
            sqlite3_stmt *s = pass ? in : up;
            sqlite3_reset(s); sqlite3_clear_bindings(s);
            sqlite3_bind_text(s, 1, path, -1, SQLITE_TRANSIENT); sqlite3_bind_int(s, 2, i + 1);
            sqlite3_bind_text(s, 3, name, -1, SQLITE_TRANSIENT); sqlite3_bind_text(s, 4, tt, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 5, al, -1, SQLITE_TRANSIENT);   sqlite3_bind_text(s, 6, ta, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 7, ge, -1, SQLITE_TRANSIENT);   sqlite3_bind_int64(s, 8, (sqlite3_int64)d->dur[i]);
            sqlite3_bind_int(s, 9, code4(name)); sqlite3_bind_int(s, 10, code4(tt)); sqlite3_bind_int(s, 11, code4(al));
            sqlite3_bind_int(s, 12, code4(ta));  sqlite3_bind_int(s, 13, code4(ge));
            if(pass) sqlite3_bind_int64(s, 14, (sqlite3_int64)time(NULL));
            if(sqlite3_step(s) != SQLITE_DONE){ g_scan_err = 1; break; }
            if(!pass && sqlite3_changes(g_db) > 0) break;      /* updated in place: same ID */
        }
        if(!g_scan_err){ pthread_mutex_lock(&g_mu); g_done++; pthread_mutex_unlock(&g_mu); }   /* a card of only images still commits */
    }
    sqlite3_finalize(up); sqlite3_finalize(in);
    if(g_scan_err) return;
    sqlite3_stmt *del = NULL;                               /* tracks the image no longer has */
    if(sqlite3_prepare_v2(g_db, "DELETE FROM SONG WHERE PATH=?1 AND COALESCE(IS_ISO,0)=1 AND TRACK>?2;", -1, &del, NULL) != SQLITE_OK){ g_scan_err = 1; return; }
    sqlite3_bind_text(del, 1, path, -1, SQLITE_TRANSIENT); sqlite3_bind_int(del, 2, d->n);
    if(sqlite3_step(del) != SQLITE_DONE) g_scan_err = 1;
    sqlite3_finalize(del);
}
static void iso_finish(void){
    iso_disc_t *d = malloc(sizeof *d);
    if(!d){
        for(int i = 0; i < g_nisos && !g_scan_err; i++) protect_root(g_isos[i], 0, "iso-allocation");
        return;
    }
    for(int i = 0; i < g_nisos && !g_scan_err && !atomic_load(&g_abort); i++){
        int r = iso_parse(g_isos[i], d);
        if(r > 0) iso_write(g_isos[i], d);
        else if(r < 0) protect_root(g_isos[i], 0, "iso-read");                 /* unreadable image: retain all its tracks */
        else {                                              /* read fine but no longer a usable SACD image: its rows go */
            pthread_mutex_lock(&g_mu); g_done++; pthread_mutex_unlock(&g_mu);   /* a card read in full: the removal commits */
            sqlite3_stmt *del = NULL;
            if(sqlite3_prepare_v2(g_db, "DELETE FROM SONG WHERE PATH=?1 AND COALESCE(IS_ISO,0)=1;", -1, &del, NULL) != SQLITE_OK){ g_scan_err = 1; break; }
            sqlite3_bind_text(del, 1, g_isos[i], -1, SQLITE_TRANSIENT);
            if(sqlite3_step(del) != SQLITE_DONE) g_scan_err = 1;
            sqlite3_finalize(del);
        }
    }
    free(d);
}

/* recursive walk; inserts every audio file found under dir. lstat (not stat) so symlinks
 * are never followed, + a depth cap, so a hostile/looping tree can't run away. */
static void walk(const char *dir, int depth){
    if(depth > 24){ g_scan_err=1; return; }   /* absurdly deep -> treat as incomplete, don't commit */
    DIR *d=opendir(dir);
    if(!d){ g_scan_err=1; return; }           /* the SD is exFAT/vfat (no perms): a failed opendir means
                                               * I/O error or the card was pulled -> incomplete scan */
    struct dirent *e;
    char path[MAXPATH];
    for(errno=0; (e=readdir(d)); errno=0){    /* errno reset before each readdir so we can detect a read error */
        if(g_scan_err) break;                 /* an earlier write failed (may have auto-rolled-back the txn): stop issuing writes */
        if(atomic_load(&g_abort)){ g_scan_err = 1; break; }  /* SD access revoked mid-walk: stop reading the card at once
                                                * (g_scan_err also blocks the commit, keeping the library) */
        if(e->d_name[0]=='.'){
            if(e->d_name[1]==0 || (e->d_name[1]=='.'&&e->d_name[2]==0)) continue;   /* . / .. */
            /* Root .diskos is our cache, with no indexed audio. A nested .diskos is a
             * user's directory and receives the same subtree protection as other dot-directories. */
            if(depth == 0 && strcmp(e->d_name, ".diskos") == 0) continue;
            int wl = snprintf(path,sizeof path,"%s/%s",dir,e->d_name);
            struct stat ds;
            if(wl<=0 || wl>=(int)sizeof path) protect_root(dir, 1, "dot-path-length");
            else if(lstat(path,&ds)!=0) protect_root(dir, 1, "dot-unresolved");
            else if(S_ISDIR(ds.st_mode)) protect_root(path, 1, "dot-directory");
            continue;
        }
        int w = snprintf(path,sizeof path,"%s/%s",dir,e->d_name);
        if(w<0 || w>=(int)sizeof path){
            g_skipped++; protect_root(dir, 1, "path-length"); continue;
        }
        struct stat st;
        /* Failing the whole scan on ANY per-file lstat error was the killer bug: every real exFAT SD
         * has entries that readdir lists but lstat can't resolve (ENOENT) - orphaned entries and, on
         * this device, real files whose special-character names don't round-trip through the mount's
         * encoding. Those always set g_scan_err=1, so the commit gate rolled back a fully-successful
         * index -> empty library (why the DB had to be hand-built). Skip ONLY the benign ENOENT case
         * (the file can't be opened to index anyway); any OTHER errno (EIO/EACCES = the card going bad)
         * stays fatal so we never commit a silently-incomplete library over a good one. */
        if(lstat(path,&st)!=0){
            if(errno == ENOENT){
                /* Neither a suffix nor d_type proves identity with earlier database bytes. */
                protect_root(dir, 1, "entry-unresolved");
                g_skipped++; continue;                     /* unresolvable entry -> skip, best-effort */
            }
            g_scan_err = 1; continue;                        /* real I/O/access error -> don't commit a partial index */
        }
        if(S_ISDIR(st.st_mode)) walk(path, depth+1);
        else if(S_ISREG(st.st_mode) && is_audio(e->d_name)) upsert_song(path, e->d_name);
        else if(S_ISREG(st.st_mode) && has_ext(e->d_name, ".cue")) cue_note(path);   /* written after the walk (cue_finish) */
        else if(S_ISREG(st.st_mode) && has_ext(e->d_name, ".iso")){ mark_seen(path); iso_note(path); }   /* SACD image (iso_finish) */
        else if(S_ISREG(st.st_mode) && is_unsupported_audio(e->d_name)){  /* AAC/M4A/OGG/... not indexed yet */
            pthread_mutex_lock(&g_mu); g_unsupported++; pthread_mutex_unlock(&g_mu);
        }
    }
    if(errno) g_scan_err=1;                   /* readdir error -> this directory listing was incomplete */
    closedir(d);
}

/* True only when SCAN_ROOT is a REAL, readable mountpoint (its st_dev differs from its
 * parent's). A missing/late SD, or a bare placeholder /tmp/sdcard dir, returns 0 - so we
 * never wipe the live library rebuilding from an unmounted card. */
static int scan_root_ready(void){
    struct stat sroot, sparent;
    if(stat(SCAN_ROOT, &sroot)!=0 || !S_ISDIR(sroot.st_mode)) return 0;
    char parent[MAXPATH];
    if(snprintf(parent,sizeof parent,"%s/..",SCAN_ROOT) >= (int)sizeof parent) return 0;
    if(stat(parent, &sparent)!=0) return 0;
    if(sroot.st_dev == sparent.st_dev) return 0;   /* not a separate mount -> SD not mounted */
    DIR *d=opendir(SCAN_ROOT); if(!d) return 0; closedir(d);
    return 1;
}

/* The worker's one decision point: commit (want=1) or fail. Taken under g_mu together with cancel acceptance, so a
 * cancel is either accepted before it (=> cancelled, no commit) or refused after it (the outcome stays committed or
 * failed). *go = 1 only if the worker may COMMIT. */
static void scan_decide(int want, int *go){
    pthread_mutex_lock(&g_mu);
    *go = 0;
    if(!g_cancelled){
        g_commit_decided = 1;
        *go = want && !atomic_load(&g_abort);
    }
    pthread_mutex_unlock(&g_mu);
#ifdef SCAN_HOOK_AFTER_DECISION
    SCAN_HOOK_AFTER_DECISION();   /* host tests only: pause the worker right after the decision */
#endif
}
static int scan_busy(void *p, int n){
    (void)p;
    if(atomic_load(&g_abort) || n >= 800) return 0;   /* give up: cancelled, or ~8 s of waiting */
    usleep(10000);
    return 1;
}
static int scan_sql_cancel(void *p){
    (void)p;
    return atomic_load(&g_abort) != 0;
}
static void *scan_thread(void *arg){
    g_decode_old_tags = (int)(intptr_t)arg;   /* immutable for this whole scan */
    int ok=0, found=0;
    /* Guard: if the SD isn't actually mounted at SCAN_ROOT, do NOT open a transaction or
     * DELETE anything - keep the existing library intact. This is the #1 data-loss guard:
     * an absent/late mount must never commit an empty SONG table. */
    if(!scan_root_ready()){
        pthread_mutex_lock(&g_mu);
        g_no_sd=1; g_total=0; g_outcome=OUT_NO_SD;      /* 0 => library kept; g_no_sd lets the UI say "insert SD" */
        g_active=0; g_finished_seq++;
        pthread_mutex_unlock(&g_mu);
        sd_io_end();
        return NULL;
    }
    if(sqlite3_open_v2(DB_PATH,&g_db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,NULL)==SQLITE_OK){
        sqlite3_busy_handler(g_db, scan_busy, NULL);   /* up to 8 s, but gives up at once when a cancel/abort lands */
        sqlite3_progress_handler(g_db, 1000, scan_sql_cancel, NULL);
        /* Everything that can change song.db (schema migration, index, scratch tables) runs INSIDE the scan
         * transaction, so a cancelled or failed scan rolls back to a byte-identical file. */
        int in_txn = (sqlite3_exec(g_db,"BEGIN IMMEDIATE;",0,0,0)==SQLITE_OK);
        if(!in_txn) g_scan_err = 1;
        if(in_txn){
        sqlite3_exec(g_db, SCHEMA_SONG, 0,0,0);
        /* Bring an EXISTING (older diskOS or stock) SONG table up to the V2.28 superset BEFORE the
         * scan transaction, so the stock player's playback SQL (which SELECTs IS_M3U/M3U_PATH) always
         * finds the columns instead of depending on the player winning a startup migration race.
         * ALTERs are idempotent here - a duplicate column just errors harmlessly. */
        sqlite3_exec(g_db, "ALTER TABLE SONG ADD COLUMN IS_M3U INT DEFAULT 0;", 0,0,0);
        sqlite3_exec(g_db, "ALTER TABLE SONG ADD COLUMN M3U_PATH TEXT DEFAULT '';", 0,0,0);
        sqlite3_exec(g_db, "ALTER TABLE SONG ADD COLUMN ACCENT INTEGER DEFAULT 0;", 0,0,0);
        /* NULL = legacy/unknown; 0 = fallback; 1 = TITLE read before filename fallback. */
        sqlite3_exec(g_db, "ALTER TABLE SONG ADD COLUMN TITLE_TAG INTEGER;", 0,0,0);
        /* Non-unique index on SONG(PATH): every scanned file looks its path up (UPDATE ... WHERE PATH=?, the
         * cue check, the insert-if-new check), which without it is a full-table scan per file - quadratic, and
         * all inside the scan's write transaction. Non-unique on purpose: stock rows may repeat a PATH (cue/iso
         * tracks). Checked (sampled, not exhaustive) not to change the order of the stock player's queries, on the
         * firmware's own SQLite over a real library plus duplicate cue/iso paths (tools/path_index_order_check.py:
         * V2.40 + V2.57, 3,493 queries, 0 differences); the player's joins on SONG.PATH use it instead of building
         * a temporary index each time. Best effort: without it a scan is only slower. */
        sqlite3_exec(g_db, "CREATE INDEX IF NOT EXISTS diskos_song_path ON SONG(PATH);", 0,0,0);
        /* PROVE the required columns exist (a duplicate-column ALTER error is fine, but a real
         * failure - BUSY/FULL/IOERR/corruption - is not). If any is missing, flag a scan error so
         * the commit gate below rolls back and KEEPS the existing library rather than rebuilding one
         * the stock V2.28 player can't query (its playback SQL SELECTs IS_M3U/M3U_PATH). */
        if(!song_has_col(g_db,"IS_M3U") || !song_has_col(g_db,"M3U_PATH") || !song_has_col(g_db,"ACCENT") || !song_has_col(g_db,"TITLE_TAG")){
            g_scan_err = 1;
            fprintf(stderr, "scanner: SONG schema migration incomplete -> keeping existing library\n");
        }
        }   /* in_txn */
        int decided = 0;
        g_ins=NULL; g_upd=NULL; g_seen=NULL; g_cuechk=NULL; g_bookok=NULL; g_durok=NULL; g_protect=NULL;
        /* a required column still missing: roll back and finish now, no walk (retried on the next scan) */
        if(in_txn && !g_scan_err && sqlite3_prepare_v2(g_db, INS_SONG, -1, &g_ins,  NULL)==SQLITE_OK
           && sqlite3_prepare_v2(g_db, UPD_SONG, -1, &g_upd,  NULL)==SQLITE_OK
           && sqlite3_prepare_v2(g_db, CUE_CHECK, -1, &g_cuechk, NULL)==SQLITE_OK
           && sqlite3_exec(g_db, SEEN_DDL, 0,0,0)==SQLITE_OK
           && sqlite3_prepare_v2(g_db, SEEN_INS, -1, &g_seen, NULL)==SQLITE_OK
           && sqlite3_exec(g_db, BOOKOK_DDL, 0,0,0)==SQLITE_OK
           && sqlite3_prepare_v2(g_db, BOOKOK_INS, -1, &g_bookok, NULL)==SQLITE_OK
           && sqlite3_exec(g_db, DUROK_DDL, 0,0,0)==SQLITE_OK
           && sqlite3_prepare_v2(g_db, DUROK_INS, -1, &g_durok, NULL)==SQLITE_OK
           && sqlite3_exec(g_db, PROTECT_DDL, 0,0,0)==SQLITE_OK
           && sqlite3_prepare_v2(g_db, "INSERT OR IGNORE INTO protected_roots(ROOT,SUBTREE) VALUES(?1,?2);",
                                 -1, &g_protect, NULL)==SQLITE_OK){
            /* ONE atomic transaction: the whole MERGE (UPDATE existing / INSERT new / DELETE vanished)
             * commits together, or ROLLBACK on any failure - a failed/partial scan can never corrupt or
             * wipe the library. MERGE (not delete+reinsert) so existing rows KEEP their ID + ACCENT,
             * preserving resume (MEMORY_PLAY.MUSIC_ID) + favourites (MY_LOVE.ID) + art-accent across a
             * rescan. Only mp3/flac/wav rows are ever removed; m4a/ape/dsf/... are never touched. */
            {
                g_skipped=0; g_prune_blocked=0; g_protect_count=0;
                /* Audiobooks live in their OWN table, never SONG (the player queues all of SONG for
                 * music, so a book in SONG would leak into Play-All/shuffle). */
                sqlite3_exec(g_db,"CREATE TABLE IF NOT EXISTS BOOKS (ID INTEGER PRIMARY KEY autoincrement,"
                    "PATH TEXT UNIQUE, NAME TEXT, TITLE TEXT, ARTIST TEXT, ALBUM TEXT, GENRE TEXT,"
                    "DURATION BIGINT, ADD_TIME INT8);",0,0,0);
                sqlite3_exec(g_db,"DELETE FROM seen;",0,0,0);   /* start from an empty seen-set */
                sqlite3_exec(g_db,"DELETE FROM book_ok;",0,0,0);   /* and an empty real-tags-book set */
                g_ncues = 0; g_nisos = 0;
                walk(SCAN_ROOT, 0);
                if(!g_scan_err) cue_finish();   /* CUE sheets found by the walk -> per-track rows */
                if(!g_scan_err) iso_finish();   /* SACD images -> per-track rows */
                pthread_mutex_lock(&g_mu); found = g_done; pthread_mutex_unlock(&g_mu);
                /* observability: one concise line per scan */
                fprintf(stderr,"scanner: merged %d songs, skipped %d unreadable file(s), scan_err=%d\n",
                        found, g_skipped, g_scan_err);
                fprintf(stderr,"scanner: protection events=%u global_retention=%d\n", g_protect_count, g_prune_blocked);
                /* Commit only if the walk found audio AND the SD is STILL mounted afterwards (guards a
                 * card pulled / gone-I/O mid-scan; zero-found rolls back too). Then delete scanned-format
                 * rows whose PATH vanished from the SD - inside the txn, so it rolls back on any failure. */
                if(found > 0 && !g_scan_err && !atomic_load(&g_abort) && scan_root_ready()){
                    /* Local uncertainty retains only protected roots. Infrastructure failure keeps
                     * the global fallback; format and mount restrictions remain unchanged. */
                    static const char *DEL_ABSENT =
                        "DELETE FROM SONG WHERE (lower(PATH) LIKE '%.mp3' OR lower(PATH) LIKE '%.flac'"
                        " OR lower(PATH) LIKE '%.wav' OR lower(PATH) LIKE '%.m4a' OR lower(PATH) LIKE '%.m4b'"
                        " OR lower(PATH) LIKE '%.aac' OR lower(PATH) LIKE '%.ogg' OR lower(PATH) LIKE '%.ape'"
                        " OR lower(PATH) LIKE '%.aif' OR lower(PATH) LIKE '%.aiff' OR lower(PATH) LIKE '%.wma'"
                        " OR lower(PATH) LIKE '%.dsf' OR lower(PATH) LIKE '%.dff' OR lower(PATH) LIKE '%.dts' OR lower(PATH) LIKE '%.iso'"
                        " OR lower(PATH) LIKE '%.opus' OR lower(PATH) LIKE '%.oga')"
                        " AND PATH LIKE '" SCAN_ROOT "/%' "
                        "AND PATH NOT IN (SELECT PATH FROM seen) AND " PRUNE_ALLOWED("SONG.PATH") ";";
                    int del_ok = (g_prune_blocked) ? 1
                                                   : (sqlite3_exec(g_db,DEL_ABSENT,0,0,0)==SQLITE_OK);
                    /* Relocate the .m4b rows the walk indexed into BOOKS, then remove them from SONG, all
                     * inside this txn so the player never sees a book in the music queue. Only SEEN (present)
                     * books move (never lose a book under a prune-blocked skipped dir); vanished books are
                     * pruned from BOOKS only when pruning is unblocked. */
                    if(del_ok && DISKOS_AUDIOBOOKS){
                        int r = sqlite3_exec(g_db,
                            /* Insert new books; for an EXISTING book, refresh NAME/TITLE/ARTIST/ALBUM/GENRE
                             * ONLY when this scan actually read its tags (PATH in book_ok) - so a good re-read
                             * fixes stale/fallback metadata, while a read FAILURE (filename fallback) never
                             * clobbers previously-good tags. ADD_TIME is left out of the SET (kept on conflict).
                             * DURATION is set by the separate UPDATE below from this scan's reading whenever the
                             * length was READ (book_dur_ok), even if unknown (0) and even if the tags failed; a
                             * read error keeps the old length. CUE/ISO virtual rows of an .m4b are never relocated
                             * or deleted (the player times those by OFFSET+DURATION). */
                            "INSERT INTO BOOKS(PATH,NAME,TITLE,ARTIST,ALBUM,GENRE,DURATION,ADD_TIME) "
                            "SELECT PATH,NAME,TITLE,ARTIST,ALBUM,GENRE,DURATION,ADD_TIME FROM SONG "
                            "WHERE lower(PATH) LIKE '%.m4b' AND PATH IN (SELECT PATH FROM seen) "
                            "AND COALESCE(IS_CUE,0)=0 AND COALESCE(IS_ISO,0)=0 "
                            "ON CONFLICT(PATH) DO UPDATE SET "
                            "NAME=excluded.NAME,TITLE=excluded.TITLE,ARTIST=excluded.ARTIST,"
                            "ALBUM=excluded.ALBUM,GENRE=excluded.GENRE "
                            "WHERE BOOKS.PATH IN (SELECT PATH FROM book_ok);",0,0,0)==SQLITE_OK
                          /* the LENGTH, independently of the tags: every book whose length was READ this scan */
                          && sqlite3_exec(g_db,
                            "UPDATE BOOKS SET DURATION=(SELECT MAX(0,IFNULL(S.DURATION,0)) FROM SONG S WHERE S.PATH=BOOKS.PATH "
                            "AND COALESCE(S.IS_CUE,0)=0 AND COALESCE(S.IS_ISO,0)=0 LIMIT 1) "
                            "WHERE PATH IN (SELECT PATH FROM book_dur_ok) AND EXISTS (SELECT 1 FROM SONG S WHERE "
                            "S.PATH=BOOKS.PATH AND COALESCE(S.IS_CUE,0)=0 AND COALESCE(S.IS_ISO,0)=0);",0,0,0)==SQLITE_OK
                          && sqlite3_exec(g_db,
                            "DELETE FROM SONG WHERE lower(PATH) LIKE '%.m4b' AND PATH IN (SELECT PATH FROM seen) "
                            "AND COALESCE(IS_CUE,0)=0 AND COALESCE(IS_ISO,0)=0;",0,0,0)==SQLITE_OK;
                        if(r && !g_prune_blocked)
                            r = sqlite3_exec(g_db,
                                "DELETE FROM BOOKS WHERE PATH LIKE '" SCAN_ROOT "/%' AND PATH NOT IN (SELECT PATH FROM seen) AND "
                                PRUNE_ALLOWED("BOOKS.PATH") ";",0,0,0)==SQLITE_OK;
                        if(!r) del_ok = 0;   /* relocate failed -> roll back the whole scan */
                    }
                    /* A cancellation that lands AFTER the walk (or during the pruning SQL) must still keep the
                     * library as it was: check again immediately before COMMIT, and roll back instead. */
                    /* The commit decision and a cancel's acceptance are ONE transition under g_mu: once the worker
                     * decides to commit, scanner_cancel() is refused, so a "Cancelling" toast is never followed by a
                     * committed scan. */
                    int go = 0;
                    scan_decide(del_ok, &go); decided = 1;
                    if(go) ok = (sqlite3_exec(g_db,"COMMIT;",0,0,0)==SQLITE_OK);
                    else ok = 0;
                }
            }
        }
        if(!decided){ int go_; scan_decide(0, &go_); }   /* a failure before the commit gate is decided too */
        if(in_txn && !ok) sqlite3_exec(g_db,"ROLLBACK;",0,0,0);   /* also covers a failed prepare / walk error */
        sqlite3_finalize(g_ins);  g_ins=NULL;
        sqlite3_finalize(g_upd);  g_upd=NULL;
        sqlite3_finalize(g_cuechk); g_cuechk=NULL;
        sqlite3_finalize(g_seen); g_seen=NULL;
        sqlite3_finalize(g_bookok); g_bookok=NULL;
        sqlite3_finalize(g_durok); g_durok=NULL;
        sqlite3_finalize(g_protect); g_protect=NULL;
    }
    if(ok) ipc_note_library_commit();
    sqlite3_close(g_db); g_db=NULL;   /* close even on a failed open: sqlite3_open_v2 may still return a handle */
    pthread_mutex_lock(&g_mu);
    g_skipped_result = g_skipped;
    g_outcome = ok ? OUT_COMMITTED : (g_cancelled ? OUT_CANCELLED : OUT_FAILED);
    if(ok) g_committed_pending = 1;   /* held until scanner_take_committed(): a later start must not hide it */
    g_total = ok ? g_done : 0;    /* committed count; 0 signals a failed rebuild (library kept) */
    g_active=0; g_finished_seq++;
    pthread_mutex_unlock(&g_mu);
    sd_io_end();
        return NULL;
}

/* Stop a running walk. The scan holds a single lease for its whole run, so closing admission alone
 * cannot stop it - without this, revoking SD access left the scanner traversing the card. */
void scanner_abort(void){ atomic_store(&g_abort, 1); }
/* User cancel: same stop as scanner_abort (the walk sets g_scan_err, so nothing commits and the txn rolls
 * back), but remembered so the UI reports "Scan cancelled" rather than a failure. */
int scanner_cancel(void){
    pthread_mutex_lock(&g_mu);
    int accepted = g_active && !g_commit_decided;   /* refused once the worker has decided to commit */
    if(accepted) g_cancelled = 1;
    pthread_mutex_unlock(&g_mu);
    if(accepted) atomic_store(&g_abort, 1);
    return accepted;
}
int scanner_start(void){
    if(!sd_io_begin()) return -1;
    pthread_mutex_lock(&g_mu);
    /* Refuse an overlapping start BEFORE touching the cancellation flag: clearing it first erased a
     * cancellation aimed at the scan that is already running. */
    if(g_active){ pthread_mutex_unlock(&g_mu); sd_io_end(); return -1; }   /* already scanning */
    atomic_store(&g_abort, 0);
    g_active=1; g_done=0; g_total=0; g_no_sd=0; g_cancelled=0; g_commit_decided=0; g_outcome=OUT_NONE; g_skipped=0; g_skipped_result=0; g_scan_err=0; g_unsupported=0;
    pthread_mutex_unlock(&g_mu);
    int mode = atomic_load(&g_old_tags);
    pthread_t th;
    if(pthread_create(&th,NULL,scan_thread,(void *)(intptr_t)mode)!=0){
        pthread_mutex_lock(&g_mu); g_active=0; pthread_mutex_unlock(&g_mu);
        sd_io_end(); return -1;
    }
    pthread_detach(th);
    return 0;
}

/* ===================== [isolated test harness] =========================== */
#ifdef SCANNER_TEST
#include <unistd.h>
int main(void){
    fprintf(stderr,"scan test -> %s (root %s)\n", DB_PATH, SCAN_ROOT);
    if(scanner_start()!=0){ fprintf(stderr,"start failed\n"); return 1; }
    int done,total;
    while(scanner_active()){ scanner_progress(&done,NULL); fprintf(stderr,"  %d...\r",done); usleep(200000); }
    scanner_progress(&done,&total);
    fprintf(stderr,"\nDONE: %d songs\n", total);
    return 0;
}
#endif

int scan_read_chapters(const char *path, chapter_t *out, int max){
    if(!sd_io_begin()) return 0;
    int result = scan_read_chapters_leased(path, out, max);
    sd_io_end();
    return result;
}

int scan_read_narrator(const char *path, char *out, int cap){
    if(!sd_io_begin()) return 0;
    int result = scan_read_narrator_leased(path, out, cap);
    sd_io_end();
    return result;
}

/* Same bounded atom walk as the scanner, with malformed and I/O exits kept distinct. */
static book_meta_outcome_t book_child(FILE *f, off_t p, uint64_t n, const char *key,
                                      off_t *pay, uint64_t *size){
    int err = 0, bad = 0;
    int found = mp4_find_child_checked(f, p, p + (off_t)n, key, pay, size, &err, &bad);
    if(err || ferror(f)) return BOOK_META_READ_FAILURE;
    if(bad) return BOOK_META_UNSUPPORTED;
    return found ? BOOK_META_OK : BOOK_META_ABSENT;
}

book_meta_result_t scan_read_book_meta(const char *path, chapter_t *out, int max,
                                      char *narrator, int cap){
    book_meta_result_t r = { BOOK_META_READ_FAILURE, BOOK_META_READ_FAILURE, 0 };
    if(narrator && cap > 0) narrator[0] = 0;
    if(!path || !out || max <= 0 || !narrator || cap <= 0) return r;
    if(!sd_io_begin()){
        r.chapters = r.narrator = BOOK_META_ADMISSION_REFUSED;
        return r;
    }
    FILE *f = fopen(path, "rb");
    if(!f){ sd_io_end(); return r; }
    off_t p; uint64_t n;
    /* No udta allocation/size cap: even multi-MB artwork is only seeked past. */
    if(fseeko(f, 0, SEEK_END) != 0 || (p = ftello(f)) < 0) goto done;
    r.chapters = book_child(f, 0, (uint64_t)p, "moov", &p, &n);
    if(r.chapters == BOOK_META_OK) r.chapters = book_child(f, p, n, "udta", &p, &n);
    r.narrator = r.chapters;
    if(r.chapters != BOOK_META_OK) goto done;
    off_t up = p; uint64_t un = n;
    r.chapters = book_child(f, up, un, "chpl", &p, &n);
    if(r.chapters == BOOK_META_OK){
        /* One byte count and title lengths bound all meaningful chapter bytes. */
        size_t len = n > 9u + 255u*264u ? 9u + 255u*264u : (size_t)n;
        unsigned char *b = malloc(len + 8);
        r.chapters = BOOK_META_READ_FAILURE;
        if(b){
            uint32_t sz = (uint32_t)len + 8;
            b[0] = sz>>24; b[1] = sz>>16; b[2] = sz>>8; b[3] = sz;
            memcpy(b+4, "chpl", 4);
            if(fread(b+8, 1, len, f) == len){
                int valid;
                r.count = mp4_chapters(b, len+8, out, max, &valid);
                r.chapters = !valid ? BOOK_META_UNSUPPORTED :
                             r.count ? BOOK_META_OK : BOOK_META_ABSENT;
            }
            free(b);
        }
    }
    clearerr(f);                           /* chapter errors must not poison narrator reads */
    /* Locate only the first composer and its data, preserving the legacy convention.
     * book_child seeks over covr and every other ilst item without reading payloads. */
    r.narrator = book_child(f, up, un, "meta", &p, &n);
    if(r.narrator == BOOK_META_OK){
        off_t mp = p; uint64_t mn = n;
        r.narrator = n >= 4 ? book_child(f, p+4, n-4, "ilst", &p, &n) : BOOK_META_UNSUPPORTED;
        if(r.narrator != BOOK_META_OK && r.narrator != BOOK_META_READ_FAILURE){
            book_meta_outcome_t fallback = book_child(f, mp, mn, "ilst", &p, &n);
            if(fallback != BOOK_META_UNSUPPORTED || r.narrator != BOOK_META_ABSENT)
                r.narrator = fallback;
        }
        if(r.narrator == BOOK_META_OK) r.narrator = book_child(f, p, n, "\251wrt", &p, &n);
        if(r.narrator == BOOK_META_OK) r.narrator = book_child(f, p, n, "data", &p, &n);
        if(r.narrator == BOOK_META_OK){
            if(n < 8) r.narrator = BOOK_META_UNSUPPORTED;
            else {
                size_t len = n-8 > (uint64_t)(cap-1) ? (size_t)(cap-1) : (size_t)(n-8);
                if(fseeko(f, p+8, SEEK_SET) != 0 || fread(narrator, 1, len, f) != len)
                    r.narrator = BOOK_META_READ_FAILURE;
                else { narrator[len] = 0; str_trim(narrator); r.narrator = narrator[0] ? BOOK_META_OK : BOOK_META_ABSENT; }
            }
        }
    }
done:
    if(fclose(f) != 0) r.chapters = r.narrator = BOOK_META_READ_FAILURE;
    sd_io_end();                            /* descriptor closes before lease release */
    if(r.chapters != BOOK_META_OK) r.count = 0;
    if(r.narrator != BOOK_META_OK) narrator[0] = 0;
    return r;
}
