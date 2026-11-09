/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "sdio.h"
#include "ipc.h"
#include "playback.h"   /* playback_display_state: the Cast view */
#include "musicdb.h"
#include "config.h"
#include "lrc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

/* Lyrics view: a sibling .lrc on the SD card first, then lyrics stored inside the audio file (lrc.h), then - unless
 * Settings > Library > Online Lyrics is off - lrclib.net by track + artist (no API key). The blocking
 * work runs on a detached thread; the result is applied on the main thread by lyrics_poll(). Timed lyrics (a .lrc,
 * or lrclib's syncedLyrics) show one row per line: the line at the playback position is highlighted and kept in
 * the middle, unless the user has just scrolled. Untimed lyrics show as one block of text. */

static lv_obj_t *g_scroll;
static lv_obj_t *g_text;           /* plain lyrics / status messages */
static lv_obj_t *g_lines_box;      /* timed lyrics: one label per line (hidden otherwise) */

#define LY_MAXL 400
static lrc_line_t g_ll[LY_MAXL];   /* the timed lines on screen (main thread only) */
static char g_ltext[16384];        /* their text */
static char g_lplain[16384];       /* untimed rendering of a .lrc */
static int g_nll = 0;              /* timed lines shown; 0 = block-text mode */
static int g_lcur = -2;            /* highlighted line (-1 = before the first, -2 = not drawn yet) */
static int g_holding = 0;          /* the user scrolled: no auto-scroll until g_hold_until */
static uint32_t g_hold_until = 0;
static int32_t g_scroll_target = -1; /* the scroll position last asked for (-1 = none) */
static unsigned g_last_pos_seq = 0;
static long g_pos_base = 0;        /* last reported position (a1 frames arrive about once a second) */
static uint32_t g_pos_tick = 0;    /* ...and when it arrived, to interpolate between frames */

/* worker->main handoff guarded by a mutex (volatile gives no cross-thread memory
 * ordering - on the dual-core X2000 the main thread could see g_lready set before the
 * g_lbuf writes are visible). The lock release/acquire pairs publish with consume. */
static pthread_mutex_t g_ly_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_lbuf[16384];         /* result text (lyrics or message)  (see handoff note) */
static int g_lkind = 0;            /* what g_lbuf holds: 0 = a message, 1 = plain lyrics, 2 = LRC text (guarded) */
static int g_lready = 0;           /* a fetch finished                 (guarded) */
static int g_linflight = 0;        /* a fetch thread is running        (guarded) */
static int g_lphase = 0;           /* 0=local, 1=online, 2=local found, 3=local absent (guarded) */
static unsigned g_req = 0;         /* bumps on each lyrics_open         (main thread only) */
static unsigned g_done_req = 0;    /* request id the finished fetch was for (guarded) */
static char g_q_title[512], g_q_artist[512];  /* url-encoded query (main thread only); 3x MDB_STR(160)
                                               * expansion for %-encoded non-ASCII/CJK titles + margin */
/* Path lifecycle (main thread): local outcome survives same-path identities. */
static int g_local_done, g_local_found;
/* Online-title identity (main thread): only identities first seen pending arm it. */
static int g_title_pending;
static uint64_t g_token;
static unsigned g_generation;
static char g_path[TRACK_PATH_MAX];                       /* current track path (main thread only) */

/* Per-fetch inputs snapshotted on the MAIN thread before pthread_create and handed to
 * the worker as its arg, so the worker never reads the g_req / g_q_title / g_q_artist /
 * g_path globals directly (they are main-thread-only; a re-open could otherwise race
 * the worker's reads of them). */
typedef struct { unsigned req; int online, online_only; char title[512], artist[512], path[TRACK_PATH_MAX]; } ly_job_t;  /* enc query, see g_q_* */


static void urlenc(const char *s, char *out, int cap)
{
    static const char *hex = "0123456789ABCDEF";
    int o = 0;
    for(; *s && o < cap - 4; s++){
        unsigned char c = (unsigned char)*s;
        if((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.')
            out[o++] = (char)c;
        else { out[o++]='%'; out[o++]=hex[c>>4]; out[o++]=hex[c&15]; }
    }
    out[o] = 0;
}

/* audio path -> sibling .lrc path */
static void derive_lrc(const char *audio, char *out, int cap)
{
    snprintf(out, cap, "%s", audio);
    char *slash = strrchr(out, '/');
    char *dot   = strrchr(out, '.');
    if(dot && (!slash || dot > slash)) snprintf(dot, cap - (int)(dot - out), ".lrc");
    else { int n = (int)strlen(out); snprintf(out + n, cap - n, ".lrc"); }
}

/* read a whole .lrc (up to cap-1 bytes; a longer file is cut at its last complete line) */
static int read_lrc(FILE *f, char *out, int cap)
{
    size_t n = fread(out, 1, (size_t)cap - 1, f);
    out[n] = 0;
    if(n == (size_t)cap - 1){ char *nl = strrchr(out, '\n'); if(nl) nl[1] = 0; }
    return (int)strlen(out);
}

/* parse 4 hex digits -> value, or -1 if any is not hex */
static int hex4(const char *s){
    int v = 0;
    for(int i=0;i<4;i++){
        char c = s[i]; int d;
        if(c>='0'&&c<='9') d = c-'0';
        else if(c>='a'&&c<='f') d = c-'a'+10;
        else if(c>='A'&&c<='F') d = c-'A'+10;
        else return -1;
        v = (v<<4) | d;
    }
    return v;
}
/* encode a Unicode codepoint as UTF-8 into out (up to 4 bytes); returns bytes written (0 if no room) */
static int utf8_enc(unsigned cp, char *out, int cap){
    if(cp < 0x80)   { if(cap<1) return 0; out[0]=(char)cp; return 1; }
    if(cp < 0x800)  { if(cap<2) return 0; out[0]=(char)(0xC0|(cp>>6));  out[1]=(char)(0x80|(cp&0x3F)); return 2; }
    if(cp < 0x10000){ if(cap<3) return 0; out[0]=(char)(0xE0|(cp>>12)); out[1]=(char)(0x80|((cp>>6)&0x3F)); out[2]=(char)(0x80|(cp&0x3F)); return 3; }
    if(cap<4) return 0;
    out[0]=(char)(0xF0|(cp>>18)); out[1]=(char)(0x80|((cp>>12)&0x3F)); out[2]=(char)(0x80|((cp>>6)&0x3F)); out[3]=(char)(0x80|(cp&0x3F)); return 4;
}

/* the first top-level result object of an lrclib search reply ("[{...},{...}]"): [*b, *e) - quotes and escapes are
 * followed so a brace or quote inside a value can't end it early. 0 if there is none (or it is cut off). */
static int json_first_object(const char *resp, const char **b, const char **e)
{
    const char *p = strchr(resp, '{');
    if(!p) return 0;
    int depth = 0, instr = 0;
    for(const char *q = p; *q; q++){
        if(instr){
            if(*q == '\\'){ if(!q[1]) return 0; q++; }
            else if(*q == '"') instr = 0;
        } else if(*q == '"') instr = 1;
        else if(*q == '{') depth++;
        else if(*q == '}' && --depth == 0){ *b = p; *e = q + 1; return 1; }
    }
    return 0;
}

/* pull the `key` JSON string ("key":"...") of the FIRST search result out of resp into out (unescaped); only a key
 * of that object counts, whatever order its fields come in */
static int json_lyrics(const char *resp, const char *key, char *out, int cap)
{
    char pat[40]; snprintf(pat, sizeof pat, "\"%s\":\"", key);
    out[0] = 0;
    const char *ob, *oe;
    if(!json_first_object(resp, &ob, &oe)) return 0;
    const char *p = strstr(ob, pat);
    if(!p || p + strlen(pat) > oe) return 0;
    p += strlen(pat);
    int len = 0;
    while(*p && len < cap - 2){
        if(*p == '\\'){
            p++; char e = *p;
            if(e=='n') out[len++]='\n';
            else if(e=='t') out[len++]=' ';
            else if(e=='"') out[len++]='"';
            else if(e=='\\') out[len++]='\\';
            else if(e=='/') out[len++]='/';
            else if(e=='r') { /* drop */ }
            else if(e=='u'){   /* \uXXXX -> UTF-8 (CJK etc.); combine surrogate pairs */
                int u = (p[1]&&p[2]&&p[3]&&p[4]) ? hex4(p+1) : -1;
                if(u >= 0){
                    unsigned cp = (unsigned)u;
                    p += 4;        /* consume the 4 hex digits (trailing p++ steps off the last one) */
                    if(cp>=0xD800 && cp<=0xDBFF && p[1]=='\\' && p[2]=='u' &&
                       p[3]&&p[4]&&p[5]&&p[6]){
                        int lo = hex4(p+3);
                        if(lo>=0xDC00 && lo<=0xDFFF){
                            cp = 0x10000u + ((cp-0xD800u)<<10) + (unsigned)(lo-0xDC00);
                            p += 6;   /* consume the "\uXXXX" low surrogate too */
                        }
                    }
                    /* drop any leftover lone/unpaired surrogate (0xD800..0xDFFF) - it
                     * has no valid UTF-8 encoding; only emit real scalar values. */
                    if(!(cp>=0xD800 && cp<=0xDFFF)) len += utf8_enc(cp, out+len, cap-1-len);
                } else {
                    out[len++]='u';   /* malformed \u -> keep the literal */
                }
            }
            else if(e) out[len++]=e;
            if(*p) p++;
        } else if(*p == '"'){
            break;            /* end of the JSON string */
        } else {
            out[len++] = *p++;
        }
    }
    out[len] = 0;
    return len;
}

static void *lyrics_thread(void *arg)
{
    ly_job_t *job = (ly_job_t *)arg;   /* our private snapshot; no shared-global reads */
    char cmd[1200];   /* holds two %-encoded query fields (2x512) + the wget URL template */
    static char resp[32768];           /* single worker at a time (inflight guard) -> static ok */
    resp[0] = 0;
    g_lbuf[0] = 0;                      /* sole writer until we publish; main reads after the lock */
    g_lkind = 0;

    /* 1) local sibling .lrc FIRST - instant for offline users who sideloaded lyrics (no 12s wait) */
    if(!job->online_only && job->path[0] && sd_io_begin()){
        char lrc[TRACK_PATH_MAX + 8]; derive_lrc(job->path, lrc, sizeof lrc);
        FILE *f = fopen(lrc, "r");
        if(f){ read_lrc(f, g_lbuf, sizeof g_lbuf); fclose(f); }
        sd_io_end();
        g_lkind = 2;
    }

    /* 2) lyrics inside the audio file (ID3 USLT/SYLT, Vorbis comments, MP4 (c)lyr): still on the card, no network */
    if(!job->online_only && !g_lbuf[0] && job->path[0] && sd_io_begin()){
        FILE *f = fopen(job->path, "rb");
        if(f){
            int synced = 0;
            if(lyr_embedded_read(f, g_lbuf, sizeof g_lbuf, &synced) > 0) g_lkind = synced ? 2 : 1;
            fclose(f);
        }
        sd_io_end();
    }

    int local_found = g_lbuf[0] != 0;

    /* 3) fall back to online (lrclib) by track + artist, unless the user turned it off */
    if(!g_lbuf[0] && job->title[0] && job->online){
        pthread_mutex_lock(&g_ly_mu); g_lphase = 1; pthread_mutex_unlock(&g_ly_mu);   /* -> poll shows "Searching online..." */
        if(job->artist[0])
            snprintf(cmd, sizeof cmd,
                "wget -qO- -T 12 'https://lrclib.net/api/search?track_name=%s&artist_name=%s' 2>/dev/null",
                job->title, job->artist);
        else
            snprintf(cmd, sizeof cmd,
                "wget -qO- -T 12 'https://lrclib.net/api/search?track_name=%s' 2>/dev/null", job->title);
        FILE *f = popen(cmd, "r");
        if(f){ int n = fread(resp, 1, sizeof(resp)-1, f); resp[n>0?n:0] = 0; pclose(f); }
        else fprintf(stderr, "lyrics popen failed: %s\n", strerror(errno));
        g_lkind = 2;
        if(!json_lyrics(resp, "syncedLyrics", g_lbuf, sizeof g_lbuf)){
            g_lkind = 1;
            json_lyrics(resp, "plainLyrics", g_lbuf, sizeof g_lbuf);
        }
        if(f && !g_lbuf[0]) fprintf(stderr, "lyrics: lrclib returned no lyrics for '%s'\n", job->title);
    }

    if(!g_lbuf[0]){
        snprintf(g_lbuf, sizeof g_lbuf, "No lyrics found.");
        g_lkind = 0;
    }

    /* publish atomically (pairs with lyrics_poll's lock so g_lbuf is visible) */
    pthread_mutex_lock(&g_ly_mu);
    g_done_req = job->req;
    g_lphase = local_found ? 2 : 3;
    g_linflight = 0;
    g_lready = 1;
    pthread_mutex_unlock(&g_ly_mu);
    free(job);
    return NULL;
}

static void show_text(const char *txt, int message);
/* a status message in the theme's message style (a bare label text change would skip the theme's sticker/plate) */
static void show_status(const char *txt)
{
    if(g_text && strcmp(lv_label_get_text(g_text), txt)) show_text(txt, 1);
}
/* Claim the single fetch slot and launch a worker with a snapshot of the current
 * request (req + query + path), all read on the main thread before pthread_create. */
static void start_fetch(void)
{
    int start = 0, online_only = 0;
    pthread_mutex_lock(&g_ly_mu);
    if(!g_linflight){
        /* A consumed miss for this path request needs only the online stage.
         * New path/view requests bump g_req, so they always start locally. */
        online_only = g_done_req == g_req && g_lphase == 3;
        g_linflight = 1; g_lready = 0; g_lphase = online_only ? 1 : 0; start = 1;
    }
    pthread_mutex_unlock(&g_ly_mu);
    if(!start) return;
    ly_job_t *job = malloc(sizeof *job);
    if(!job){
        pthread_mutex_lock(&g_ly_mu); g_linflight = 0; pthread_mutex_unlock(&g_ly_mu);
        show_status("Lyrics unavailable.");   /* don't leave "Fetching..." stuck */
        return;
    }
    job->req = g_req;
    job->online_only = online_only;
    job->online = cfg_get_int("online_lyrics", 1);   /* Settings > Library > Online Lyrics (on unless turned off) */
    snprintf(job->title,  sizeof job->title,  "%s", g_q_title);
    snprintf(job->artist, sizeof job->artist, "%s", g_q_artist);
    snprintf(job->path,   sizeof job->path,   "%s", g_path);
    pthread_t th;
    if(pthread_create(&th, NULL, lyrics_thread, job) == 0) pthread_detach(th);
    else {
        free(job);
        pthread_mutex_lock(&g_ly_mu); g_linflight = 0; pthread_mutex_unlock(&g_ly_mu);
        show_status("Lyrics unavailable.");
    }
}

/* block-text mode: one label (status messages, untimed lyrics) */
static void show_text(const char *txt, int message)
{
    g_nll = 0; g_lcur = -2;
    if(g_lines_box){ lv_obj_clean(g_lines_box); lv_obj_add_flag(g_lines_box, LV_OBJ_FLAG_HIDDEN); }
    if(g_text){ lv_obj_remove_flag(g_text, LV_OBJ_FLAG_HIDDEN); lv_label_set_text(g_text, txt);
        lv_obj_set_style_translate_y(g_text, 0, 0);
        if(message) theme_kit()->empty_state(KIT_EMPTY_LYRICS, g_text);
        else {
            lv_obj_set_width(g_text, 280);
            lv_obj_set_style_pad_all(g_text, 0, 0);   /* a message style may have padded it */
            lv_obj_set_style_translate_x(g_text, 0, 0);
            lv_obj_set_style_text_align(g_text, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(g_text, TF(USER_16), 0);
            lv_obj_set_style_text_color(g_text, TC(TEXT_LYRICS), 0);
        } }
    if(g_scroll) lv_obj_scroll_to_y(g_scroll, 0, LV_ANIM_OFF);
}
#ifdef DISKOS_HOST
void lyrics_host_show_plain(const char *txt){ show_text(txt, 0); }
#endif

/* timed mode: one label per line in g_ll; returns 0 (and leaves the view alone) if the text isn't timed */
static int show_timed(const char *lrc)
{
    int synced = 0;
    int n = lrc_parse(lrc, g_ll, LY_MAXL, g_ltext, sizeof g_ltext, g_lplain, sizeof g_lplain, &synced);
    if(!synced || !g_lines_box){
        show_text(g_lplain[0] ? g_lplain : "No lyrics found.", !g_lplain[0]);
        return 0;
    }
    lv_obj_clean(g_lines_box);
    for(int i = 0; i < n; i++){
        lv_obj_t *l = lv_label_create(g_lines_box);
        lv_obj_set_width(l, 280);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(l, TF(USER_16), 0);
        lv_obj_set_style_text_color(l, TC(TEXT_MUTED), 0);
        lv_label_set_text(l, g_ltext[g_ll[i].text] ? g_ltext + g_ll[i].text : " ");   /* an empty timed line is a pause */
    }
    g_nll = n; g_lcur = -2; g_holding = 0; g_scroll_target = -1;
    if(g_text) lv_obj_add_flag(g_text, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(g_lines_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(g_scroll);
    lv_obj_scroll_to_y(g_scroll, 0, LV_ANIM_OFF);
    return 1;
}

/* Slice 15b: the track the lyrics follow. LOCAL: exactly the player's state. CAST: the facade's track (its library path keys the
 * local .lrc / tag lookup; position and play state are the Cast ones). No receiver data is read. */
static void ly_state(track_state_t *out){ track_state_t raw; ipc_get_state(&raw); playback_display_state(out, &raw); }

/* the playback position now: the last reported one, plus the time since it arrived while playing (capped) */
static long lyrics_position(void)
{
    track_state_t st; ly_state(&st);
    uint32_t now = lv_tick_get();
    if(st.pos_seq != g_last_pos_seq || st.position_ms != g_pos_base){   /* a new report (or a seeded/first one) */
        g_last_pos_seq = st.pos_seq; g_pos_base = st.position_ms; g_pos_tick = now;
    }
    long est = g_pos_base;
    if(ui_view_playing()){ uint32_t d = now - g_pos_tick; est += d > 1500 ? 1500 : (long)d; }
    return est;
}

/* highlight the line at the playback position and keep it in the middle */
static void lyrics_follow(void)
{
    if(!g_nll || !g_lines_box) return;
    int idx = lrc_index_at(g_ll, g_nll, lyrics_position());
    if(idx != g_lcur){
        if(g_lcur >= 0 && g_lcur < g_nll) lv_obj_set_style_text_color(lv_obj_get_child(g_lines_box, g_lcur), TC(TEXT_MUTED), 0);
        g_lcur = idx;
        if(idx >= 0) lv_obj_set_style_text_color(lv_obj_get_child(g_lines_box, idx), TC(TEXT_LYRICS), 0);
    }
    if(g_lcur < 0) return;
    if(g_holding && (int32_t)(lv_tick_get() - g_hold_until) < 0) return;   /* the user is reading elsewhere */
    if(g_holding){ g_holding = 0; g_scroll_target = -1; }                   /* hold over: come back to the line */
    /* checked every poll, not only when the line changes: the first layout after a rebuild can still be settling */
    lv_obj_update_layout(g_scroll);
    lv_obj_t *l = lv_obj_get_child(g_lines_box, g_lcur);
    lv_area_t la, sa;                               /* on-screen: how far the line's middle is from the view's middle */
    lv_obj_get_coords(l, &la); lv_obj_get_coords(g_scroll, &sa);
    int32_t y = lv_obj_get_scroll_y(g_scroll) + (la.y1 + la.y2) / 2 - (sa.y1 + sa.y2) / 2;
    if(y < 0) y = 0;
    if(y != g_scroll_target){ g_scroll_target = y; lv_obj_scroll_to_y(g_scroll, y, LV_ANIM_ON); }
}

static void scroll_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if(c == LV_EVENT_PRESSED || (c == LV_EVENT_SCROLL_BEGIN && lv_indev_active()))
        { g_holding = 1; g_hold_until = lv_tick_get() + 4000; }
}

/* Two independent state pieces: g_path/g_req and the local outcome own the
 * view/fetch lifecycle; g_token/g_generation/g_title_pending own online readiness.
 * Invariant: a title ready on the first snapshot (non-Opus, CUE/SACD,
 * streams) preserves the pre-Opus PATH lifecycle exactly: same path means
 * no restart, loading state, scroll reset or refetch, with the same DB-title
 * fallback for an empty ready live title. Only a pending identity adds work:
 * local/path work proceeds immediately; only online search waits, then runs
 * once with the committed title for that playback identity. Pending-empty
 * never uses the DB fallback; ready-empty does. Ready-first open/retry
 * requires a nonempty resolved query title, as before; only pending
 * identities may start local work with a path alone.
 *
 * path    identity title                  local/decode       action
 * changed any      ready nonempty          any                normal local + online fallback
 * changed any      ready empty             any                DB fallback; fetch only if title resolves,
 *                                                           else "No track playing."; no work
 * changed any      pending (empty or not)  any                local only; arm online
 * same    changed  ready (empty or not)    any                keep view/scroll/fetch; disarm
 * same    changed  pending (empty or not)  any                keep path work; arm new identity
 * same    same     pending                any                no DB/online query
 * same    same     committed              running/unconsumed wait for local result
 * same    same     committed              present            disarm; keep local lyrics/scroll
 * same    same     committed nonempty     absent             disarm; one online-only search
 * same    same     committed empty        absent             DB fallback; one search if available
 * Commitment before/after decode uses the same consumed-result barrier. */
static void lyrics_query(const track_state_t *st)
{
    char title[IPC_TITLE_CAPACITY] = "", artist[200] = "";
    int ready = !st->have_track || st->title_ready;
    if(ready) snprintf(title, sizeof title, "%s", st->title);
    snprintf(artist, sizeof artist, "%s", st->artist);
    if(ready && !title[0] && playback_source() == PB_SRC_LOCAL){   /* 15b: the DB "current play" is the LOCAL track, never a Cast track's title */
        mdb_song_t cur; int pm, pl;
        if(mdb_current_play(&cur, &pm, &pl)){
            snprintf(title, sizeof title, "%s", cur.title);
            snprintf(artist, sizeof artist, "%s", cur.artist);
            if(!g_path[0]) mdb_song_path(cur.id, g_path, sizeof g_path);
        }
    }
    urlenc(title, g_q_title, sizeof g_q_title);
    urlenc(artist, g_q_artist, sizeof g_q_artist);
}

void lyrics_open(void)
{
    track_state_t st; ly_state(&st);
    int needs_db=playback_source()==PB_SRC_LOCAL && (!st.have_track || st.title_ready) && !st.title[0];
    if(needs_db && !mdb_ui_read_begin()){ ui_toast("Library busy - try again"); return; }
    g_req++;   /* path/view request supersedes any in-flight fetch */
    g_local_done = g_local_found = 0;
    g_title_pending = st.have_track && !st.title_ready;
    g_token = st.playback_token; g_generation = st.player_generation;
    snprintf(g_path, sizeof g_path, "%s", st.path);
    lyrics_query(&st);
    if(needs_db) mdb_ui_read_end();
    show_text("Checking SD card...", 1);
    screen_show(SCR_LYRICS);
    if(g_q_title[0] || (g_title_pending && g_path[0])) start_fetch();
    else show_text("No track playing.", 1);
}

/* MAIN thread only. Local work is consumed before submitting online-only work. */
static void lyrics_commit_title(const track_state_t *st)
{
    if(!st->have_track){ g_title_pending = 0; return; }
    if(st->playback_token != g_token || st->player_generation != g_generation){
        g_token = st->playback_token; g_generation = st->player_generation;
        g_title_pending = !st->title_ready;
    }
    if(!g_title_pending || !st->title_ready || strcmp(st->path, g_path)) return;
    pthread_mutex_lock(&g_ly_mu);
    int waiting = g_linflight || g_lready;
    pthread_mutex_unlock(&g_ly_mu);
    if(waiting || !g_local_done) return;
    if(g_local_found){ g_title_pending=0; return; }
    int needs_db=playback_source()==PB_SRC_LOCAL && !st->title[0];
    if(needs_db && !mdb_ui_read_begin()) return;
    lyrics_query(st);
    if(needs_db) mdb_ui_read_end();
    g_title_pending=0;
    if(g_q_title[0] && cfg_get_int("online_lyrics", 1)) start_fetch();
}

void lyrics_poll(lv_timer_t *t)
{
    (void)t;
    /* If the track changed while the lyrics screen is open, reload for the new track
     * (g_path is the track we last fetched for). lyrics_open() re-resolves the live
     * track, bumps g_req to supersede any in-flight fetch, and updates g_path so this
     * fires once per change. Only while SCR_LYRICS is visible - no wasted fetches. */
    if(screen_current() == SCR_LYRICS){
        track_state_t st; ly_state(&st);
        if(st.path[0] && strcmp(st.path, g_path)){ lyrics_open(); return; }
        lyrics_commit_title(&st);
    }
    int on = screen_current() == SCR_LYRICS;
    if(t) lv_timer_set_period(t, (on && g_nll) ? 200 : 500);   /* follow timed lines closely only while they show */
    if(on) lyrics_follow();
    int ready = 0, inflight, phase; unsigned done = 0;
    pthread_mutex_lock(&g_ly_mu);
    if(g_lready){ g_lready = 0; ready = 1; done = g_done_req; }
    inflight = g_linflight; phase = g_lphase;
    pthread_mutex_unlock(&g_ly_mu);
    if(!ready){
        /* still fetching: reflect the phase so the wait isn't a silent "Checking SD card..." */
        if(inflight && g_text && !lv_obj_has_flag(g_text, LV_OBJ_FLAG_HIDDEN))
            show_status(phase ? "Searching online..." : "Checking SD card...");
        return;
    }
    if(done == g_req){
        g_local_done = 1; g_local_found = phase == 2;
        /* result is for the current track; g_lbuf is visible (published under the lock)
         * and no worker is running, so reading it here is race-free. */
        if(g_lkind == 2){ if(show_timed(g_lbuf) && on) lyrics_follow(); }
        else show_text(g_lbuf, g_lkind == 0);
    } else if(g_q_title[0] || (g_title_pending && g_path[0])){
        /* finished fetch was for a superseded track - fetch the current one
         * (start_fetch no-ops if a newer fetch is already running). */
        (void)inflight;
        start_fetch();
    }
}

void lyrics_create(lv_obj_t *root)
{
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    ui_header(root, "Lyrics");   /* shared standard header */

    g_scroll = lv_obj_create(root);
    lv_obj_remove_style_all(g_scroll);
    lv_obj_set_pos(g_scroll, 40, 76); lv_obj_set_size(g_scroll, 280, 250);
    lv_obj_set_style_pad_bottom(g_scroll, 44, 0);   /* last lyric lines scroll clear of the round bezel */
    lv_obj_set_style_bg_opa(g_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(g_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_scroll, LV_SCROLLBAR_MODE_OFF);

    g_text = lv_label_create(g_scroll);
    lv_obj_set_width(g_text, 280);
    lv_label_set_long_mode(g_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(g_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_text, TF(USER_16), 0);   /* CJK lyrics render via Source Han Sans fallback */
    lv_obj_set_style_text_color(g_text, TC(TEXT_LYRICS), 0);
    lv_label_set_text(g_text, "");

    g_lines_box = lv_obj_create(g_scroll);
    lv_obj_remove_style_all(g_lines_box);
    lv_obj_set_width(g_lines_box, 280);
    lv_obj_set_height(g_lines_box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g_lines_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_lines_box, 10, 0);
    lv_obj_set_style_pad_top(g_lines_box, 100, 0);       /* the first and last lines can reach the middle */
    lv_obj_set_style_pad_bottom(g_lines_box, 110, 0);
    lv_obj_remove_flag(g_lines_box, LV_OBJ_FLAG_CLICKABLE);   /* presses go to the scroller (manual-scroll hold) */
    lv_obj_add_flag(g_lines_box, LV_OBJ_FLAG_HIDDEN);
    g_nll = 0; g_lcur = -2;
    lv_obj_add_event_cb(g_scroll, scroll_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(g_scroll, scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);
}
