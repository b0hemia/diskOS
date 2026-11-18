#include "indic.h"
/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Books screen: list audiobooks (.m4b) with saved progress; tap a book to play and resume where
 * you left off. Audiobooks are kept out of the music Songs/Albums/Artists views (mdb_load excludes
 * .m4b); this is the one place they show. */
#include "books.h"
#include "theme.h"
#include "theme_kit.h"
#include "screens.h"
#include "musicdb.h"
#include "artcache.h"
#include "ui.h"
#include "i18n.h"
#include "ipc.h"       /* ipc_get_state: current book + position for the chapter list */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>

static lv_obj_t *g_list;
#define BOOKS_MAX 256
static book_t g_books[BOOKS_MAX];
static int    g_nbooks;

/* Per-open generation for the row-cover temp files. Each books_open bumps it so the
 * materialised BMP paths are fresh - LVGL caches decoded images by path, so reusing a
 * path after its file changed could show a stale cover. Fresh path each open avoids that. */
static unsigned g_cover_gen;
/* Bound the RAM cost: each 42px thumb is a few KB in tmpfs and /tmp is tiny (~19MB free),
 * so only materialise covers for the first N books; the rest fall back to the glyph. */
#define BOOKS_COVER_MAX 96

/* Remove every /tmp/bthumb_*.bmp left by a previous open so the temp files never
 * accumulate across screen opens (tmpfs is RAM). Called before writing this open's set. */
static void books_covers_cleanup(void){
    DIR *d = opendir("/tmp");
    if(!d) return;
    struct dirent *e;
    char p[300];
    while((e = readdir(d))){
        if(strncmp(e->d_name, "bthumb_", 7) != 0) continue;
        int n = snprintf(p, sizeof p, "/tmp/%s", e->d_name);
        if(n > 0 && n < (int)sizeof p) unlink(p);
    }
    closedir(d);
}

/* Human progress line: "Finished", "Not started", "3h 12m in" / "7m in". */
static void progress_text(const book_t *b, char *out, size_t len){
    if(b->completed){ snprintf(out, len, "Finished"); return; }
    if(b->position_ms <= 0){ snprintf(out, len, "Not started"); return; }
    long s = b->position_ms / 1000, h = s / 3600, m = (s / 60) % 60;
    if(h > 0) snprintf(out, len, "%ldh %ldm in", h, m);
    else      snprintf(out, len, "%ldm in", m > 0 ? m : 1);
}

static void book_row_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(i < 0 || i >= g_nbooks) return;
    /* re-read the bookmark, completion AND its age now (the list snapshot may be stale if the book
     * finished or advanced since the screen was built) so we resume at the LATEST saved position, and
     * restart a finished book from the beginning rather than its last-second tail. */
    if(!g_books[i].path[0]){ ui_toast("Path too long"); return; }   /* the library path did not fit: refuse, never a prefix */
    long pos = 0, updated = 0; int completed = 0;
    int r = mdb_book_progress(g_books[i].path, NULL, 0, &pos, &completed, &updated);
    if(r < 0){ ui_toast("Couldn't read progress"); return; }   /* read error -> don't start a session that could erase the bookmark */
    long resume = completed ? 0 : pos;
    if(resume > 0 && updated > 0){       /* smart rewind: back up by how long it's been idle */
        long idle = (long)time(NULL) - updated;
        long rw = ui_smart_rewind_ms(idle);
        resume -= rw; if(resume < 0) resume = 0;
    }
    if(ui_play_book(g_books[i].path, resume)) screen_show(SCR_NOWPLAYING);   /* a book tap opens Now Playing, like a song tap does (not on a refusal) */
}

/* Left-edge cover for one row: the cached 42px thumb if this book has been played (and so
 * has art in the cache), otherwise a music glyph on the accent tile. Books that have never
 * been opened have no cached art yet - they show the glyph until first play. */
static void books_add_cover(lv_obj_t *row, const book_t *b, int i){
    lv_obj_t *tile = lv_obj_create(row);
    lv_obj_remove_style_all(tile);
    lv_obj_set_pos(tile, 12, 8); lv_obj_set_size(tile, 42, 42);
    lv_obj_set_style_radius(tile, 8, 0);
    lv_obj_set_style_clip_corner(tile, true, 0);
    lv_obj_set_style_bg_color(tile, TC(SURFACE_STRONG), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    /* lv_obj_create() is CLICKABLE by default; if the tile keeps that it eats the tap
     * meant for the row's play handler. Clear both flags so taps pass through to the row. */
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    char tmp[64], src[72];
    int have = 0;
    if(i < BOOKS_COVER_MAX && b->path[0]){
        snprintf(tmp, sizeof tmp, "/tmp/bthumb_%u_%d.bmp", g_cover_gen, i);
        have = (artcache_get_thumb(b->path, tmp) == 0);
    }
    if(have){
        snprintf(src, sizeof src, "A:/tmp/bthumb_%u_%d.bmp", g_cover_gen, i);
        lv_obj_t *img = lv_image_create(tile);
        lv_image_set_src(img, src);              /* LVGL copies the path string */
        lv_obj_center(img);
    } else {
        lv_obj_t *g = lv_label_create(tile);     /* no cached cover yet -> glyph */
        lv_label_set_text(g, LV_SYMBOL_AUDIO);
        lv_obj_set_style_text_font(g, TF(UI_20), 0);
        lv_obj_set_style_text_color(g, TC(TEXT_SECONDARY), 0);
        lv_obj_center(g);
    }
}

static void books_rebuild_read(void){
    if(!g_list) return;
    lv_obj_clean(g_list);
    books_covers_cleanup();
    g_cover_gen++;
    int rc = mdb_books(g_books, BOOKS_MAX);
    if(rc < 0){                                  /* DB read error (not "empty") -> a retryable failure, not "add files" */
        g_nbooks = 0;
        lv_obj_t *l = lv_label_create(g_list);
        lv_label_set_text(l, "Couldn't read audiobooks\nReopen to try again");
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, 260);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(l, TC(TEXT_MUTED), 0);
        lv_obj_set_style_text_font(l, TF(UI_16), 0);
        return;
    }
    g_nbooks = rc;
    if(g_nbooks <= 0){
        lv_obj_t *l = lv_label_create(g_list);
        lv_label_set_text(l, "No audiobooks\nAdd .m4b files to the card");
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, 260);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(l, TC(TEXT_MUTED), 0);
        lv_obj_set_style_text_font(l, TF(UI_16), 0);
        return;
    }
    for(int i = 0; i < g_nbooks; i++){
        book_t *b = &g_books[i];
        lv_obj_t *r = lv_button_create(g_list);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, 268, 58);
        lv_obj_set_style_radius(r, 10, 0);
        lv_obj_set_style_bg_color(r, TC(SURFACE), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(r, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        ui_on(r, book_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i, "books.book_row", UI_CORE);

        books_add_cover(r, b, i);                               /* 42px cover / glyph on the left */

        lv_obj_t *t = indic_label_create(r);                       /* title (book names may be non-Latin) */
        lv_label_set_text(t, b->title[0] ? b->title : "Untitled");
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(t, 64, 8); lv_obj_set_size(t, 190, 20);
        lv_obj_set_style_text_font(t, TF(USER_16), 0);
        lv_obj_set_style_text_color(t, TC(TEXT_PRIMARY), 0);

        char prog[32]; progress_text(b, prog, sizeof prog);
        char sub[224];
        if(b->author[0]) snprintf(sub, sizeof sub, "%s - %s", b->author, prog);
        else             snprintf(sub, sizeof sub, "%s", prog);
        lv_obj_t *s = indic_label_create(r);                       /* author - progress */
        lv_label_set_text(s, sub);
        lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(s, 64, 32); lv_obj_set_size(s, 190, 18);
        lv_obj_set_style_text_font(s, TF(USER_14), 0);
        lv_obj_set_style_text_color(s, TC(TEXT_MUTED), 0);
        indic_row_fit(r,t,s);
    }
    lv_obj_scroll_to_y(g_list, 0, LV_ANIM_OFF);
}
static void books_rebuild(void){
    if(!mdb_ui_read_begin()){ ui_toast("Library busy - reopen to refresh"); return; }
    books_rebuild_read();
    mdb_ui_read_end();
}

void books_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    ui_header(root, "Books");

    g_list = lv_obj_create(root);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_pos(g_list, 30, 72); lv_obj_set_size(g_list, 300, 272);
    lv_obj_set_style_pad_bottom(g_list, 44, 0);   /* last row clears the round bottom bezel */
    lv_obj_set_style_bg_opa(g_list, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_list, 6, 0);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(g_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
}

void books_open(void){
    books_rebuild();
    screen_show(SCR_BOOKS);
}

/* ---- Chapter navigation (SCR_CHAPTERS): reached from the Now Playing right-hub for an audiobook.
 * Lists the current book's chapters; tapping one seeks to its start and returns to Now Playing. */
#define CHAPS_MAX 256
static lv_obj_t *g_chap_list;
static chapter_t g_chlist[CHAPS_MAX];
static int       g_chlist_n;
static uint64_t g_chlist_request, g_chlist_token;
static unsigned g_chlist_player;
static bookmeta_state g_chlist_state;
static char      g_chlist_path[1024];   /* the book these chapters belong to (a jump must target THAT book) */

static void chap_fmt_dur(long ms, char *out, size_t n){
    if(ms < 0) ms = 0;
    long s = ms / 1000, h = s / 3600, m = (s / 60) % 60;
    if(h > 0)      snprintf(out, n, "%ldh %02ldm", h, m);
    else if(m > 0) snprintf(out, n, "%ldm", m);
    else           snprintf(out, n, "%lds", s % 60);
}

static void chap_row_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(i < 0 || i >= g_chlist_n) return;
    /* Only seek if the track that was current when this list was built is STILL current - otherwise a
     * queue advance or a transport change since the screen opened would seek the wrong track. */
    track_state_t st; ipc_get_state(&st);
    const bookmeta_snapshot *m = ui_bookmeta_snapshot(&st);
    if(!m || m->request_generation != g_chlist_request ||
       st.playback_token != g_chlist_token || st.player_generation != g_chlist_player ||
       strcmp(st.path, g_chlist_path) != 0){ screen_show(SCR_NOWPLAYING); return; }
    long tgt = g_chlist[i].start_ms;
    if(st.duration_ms > 0 && tgt >= st.duration_ms) tgt = st.duration_ms - 1;   /* clamp to a valid position */
    if(tgt < 0) tgt = 0;
    if(ui_seek_to(tgt) == 0) ui_book_user_seeked(tgt);  /* drop pending resume only if the jump went out */
    screen_show(SCR_NOWPLAYING);
}

static void chap_empty(const char *msg){
    lv_obj_t *l = lv_label_create(g_chap_list);
    lv_label_set_text(l, tr(msg));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, 260);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(l, TC(TEXT_MUTED), 0);
    lv_obj_set_style_text_font(l, TF(UI_16), 0);
}

void chapters_update(const track_state_t *current){
    if(!g_chap_list || screen_current() != SCR_CHAPTERS) return;
    track_state_t st = *current;
    const bookmeta_snapshot *m = ui_bookmeta_snapshot(&st);
    uint64_t request = m ? m->request_generation : 0;
    bookmeta_state state = m ? m->state : BOOKMETA_NONE;
    static int shown_chapter = -1;
    static long shown_duration = -1;
    int cur = 0;
    if(m && state == BOOKMETA_COMPLETE && m->result.chapters == BOOK_META_OK){
        for(int i = 0; i < m->result.count; i++){
            if(m->chapters[i].start_ms <= st.position_ms) cur = i;
            else break;
        }
    }
    int same_book = !strcmp(g_chlist_path, st.path) &&
        g_chlist_token == st.playback_token && g_chlist_player == st.player_generation;
    int keep_scroll = same_book && shown_chapter == cur &&   /* not on a forced reopen or placeholder -> content */
        g_chlist_state == BOOKMETA_COMPLETE && state == BOOKMETA_COMPLETE &&
        g_chlist_request == request && g_chlist_n > 0;
    int32_t scroll_y = lv_obj_get_scroll_y(g_chap_list);
    if(!strcmp(g_chlist_path, st.path) && g_chlist_token == st.playback_token &&
       g_chlist_player == st.player_generation && g_chlist_request == request &&
       g_chlist_state == state && shown_chapter == cur &&
       shown_duration == st.duration_ms) return;
    shown_chapter = cur; shown_duration = st.duration_ms;
    lv_obj_clean(g_chap_list);
    g_chlist_n = 0;
    snprintf(g_chlist_path, sizeof g_chlist_path, "%s", st.path);
    g_chlist_token = st.playback_token; g_chlist_player = st.player_generation;
    g_chlist_request = request; g_chlist_state = state;
    if(!st.have_track || !mdb_is_book_path(st.path)){ chap_empty("No audiobook playing"); return; }
    if(m && m->state == BOOKMETA_PENDING){ chap_empty("Loading chapters..."); return; }
    if(!m || m->state != BOOKMETA_COMPLETE ||
       (m->result.chapters != BOOK_META_OK && m->result.chapters != BOOK_META_ABSENT)){
        chap_empty("Chapters unavailable"); return;
    }
    g_chlist_n = m->result.count;
    if(g_chlist_n <= 0){ chap_empty("This book has no chapters"); return; }
    memcpy(g_chlist, m->chapters, g_chlist_n * sizeof g_chlist[0]);
    long dur = st.duration_ms;
    lv_obj_t *cur_row = NULL;
    for(int i = 0; i < g_chlist_n; i++){
        long endms = (i + 1 < g_chlist_n) ? g_chlist[i+1].start_ms : (dur > 0 ? dur : g_chlist[i].start_ms);
        char durbuf[16]; chap_fmt_dur(endms - g_chlist[i].start_ms, durbuf, sizeof durbuf);

        lv_obj_t *r = lv_button_create(g_chap_list);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, 268, 52);
        lv_obj_set_style_radius(r, 10, 0);
        lv_obj_set_style_bg_color(r, (i == cur ? TC(SURFACE_RAISED) : TC(SURFACE)), 0);
        lv_obj_set_style_bg_opa(r, i == cur ? LV_OPA_COVER : LV_OPA_50, 0);
        lv_obj_set_style_bg_color(r, TC(RAISED_PRESSED), LV_STATE_PRESSED);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        ui_on(r, chap_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i, "books.chap_row", UI_CORE);

        lv_obj_t *num = lv_label_create(r);                     /* chapter number, accent when current */
        char nb[8]; snprintf(nb, sizeof nb, "%d", i + 1);
        lv_label_set_text(num, nb);
        lv_obj_set_pos(num, 12, 16); lv_obj_set_size(num, 34, 20);
        lv_obj_set_style_text_font(num, TF(UI_14), 0);
        lv_obj_set_style_text_color(num, (i == cur ? TC(STATUS_INFO) : TC(TEXT_MUTED)), 0);

        lv_obj_t *t = indic_label_create(r);                       /* chapter title (may be non-Latin) */
        lv_label_set_text(t, g_chlist[i].title);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(t, 50, 16); lv_obj_set_size(t, 148, 20);
        lv_obj_set_style_text_font(t, TF(USER_16), 0);
        lv_obj_set_style_text_color(t, TC(TEXT_PRIMARY), 0);

        lv_obj_t *d = lv_label_create(r);                       /* chapter duration, right-aligned */
        lv_label_set_text(d, durbuf);
        lv_obj_set_pos(d, 200, 16); lv_obj_set_size(d, 56, 20);
        lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(d, TF(UI_12), 0);
        lv_obj_set_style_text_color(d, TC(TEXT_MUTED), 0);

        indic_row_fit(r,t,NULL);
        if(i == cur) cur_row = r;
    }
    if(keep_scroll) lv_obj_scroll_to_y(g_chap_list, scroll_y, LV_ANIM_OFF);
    else if(cur_row) lv_obj_scroll_to_view(cur_row, LV_ANIM_OFF);    /* open centred on the current chapter */
}

void chapters_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    ui_header(root, "Chapters");

    g_chap_list = lv_obj_create(root);
    lv_obj_remove_style_all(g_chap_list);
    lv_obj_set_pos(g_chap_list, 30, 72); lv_obj_set_size(g_chap_list, 300, 272);
    lv_obj_set_style_pad_bottom(g_chap_list, 44, 0);
    lv_obj_set_style_bg_opa(g_chap_list, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(g_chap_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_chap_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_chap_list, 6, 0);
    lv_obj_set_scroll_dir(g_chap_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_chap_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(g_chap_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
}

void chapters_open(void){
    track_state_t st; ipc_get_state(&st);
    ui_bookmeta_retry(&st); /* only a failed request is retried; pending/absence stay put */
    g_chlist_state = (bookmeta_state)-1; /* rebuild even for an unchanged completed book */
    screen_show(SCR_CHAPTERS);
    chapters_update(&st);
}
