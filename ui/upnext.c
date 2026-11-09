/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "config.h"
#include "musicdb.h"
#include "ipc.h"
#include "playback.h"
#include "cast_ctlw.h"
#include "airplay_ipc.h"
#include "audio_stock.h"
extern unsigned ui_airplay_attempt(void);
extern int ui_airplay_queue_view(cw_view *);
extern int ui_airplay_queue_edit(uint32_t, int, unsigned);
extern int ui_airplay_queue_jump(uint32_t, int);
static int remote_queue_active(void){ return playback_source() == PB_SRC_CAST || playback_source() == PB_SRC_AIRPLAY; }
static int remote_view(cw *w, cw_view *v){ return playback_source() == PB_SRC_AIRPLAY ? ui_airplay_queue_view(v) : w ? cw_view_get(w, v) : 0; }
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- Up Next (SCR_UPNEXT): reached from the Now Playing right-hub. The stock player owns the queue (LIST_SONG_0,
 * shuffle order in LIST_SONG_3); this screen only reads it (mdb_upnext) and jumps within it (ui_queue_jump). It
 * shows the current song and what plays after it, follows track changes while open, and never shows a guessed
 * order: while the player is rebuilding the queue it says so and retries. */
#define UN_SHOW   100                  /* upcoming rows shown; the rest is summarised in a footer */
#define UN_FIRST  12                   /* rows built at once (a screenful), the rest a few per tick */
#define UN_RETRY  8                    /* 1 s retries while the queue is being rebuilt */
static lv_obj_t   *g_un_list, *g_un_title;
static lv_timer_t *g_un_fill, *g_un_watch;
static mdb_qrow_t  g_un_rows[UN_SHOW + 1];
static int         g_un_n, g_un_fill_i, g_un_more, g_un_retries, g_un_retry;   /* g_un_retry: last read was transient */
static int         g_un_mode = -1;     /* the play mode the list was read in */
static char        g_un_path[TRACK_PATH_MAX];     /* the song that was current when the list was read */
static int         g_un_pos;           /* its pos_id then (0 = unknown) */
static unsigned    g_un_seq;           /* its path_seq then: a track change since means the list is stale */
/* Cast (slice 14f): while playback_source() is CAST the list is the Cast queue in PLAY order, read from the worker's published view (a copy;
 * the screen never touches the cursor or the snapshot). The list is current while the view's qgen, pos and mode are unchanged; a tap posts a
 * JUMP (qgen, occurrence) that the worker re-checks. Rows hold the occurrence in g_un_view.occ[i] (g_un_rows[i].ord is not overloaded). */
static cw_view     g_un_view;
static int         g_un_source; static unsigned g_un_attempt;
static int         g_un_cast;          /* the list shows the Cast queue */

static void un_rebuild(void);

static void un_fmt_dur(int ms, char *out, size_t n){
    if(ms <= 0){ out[0] = 0; return; }
    long long t = ((long long)ms + 500) / 1000; snprintf(out, n, "%lld:%02lld", t / 60, t % 60);   /* no int overflow near INT32_MAX */
}

static lv_obj_t *un_note(const char *msg){
    lv_obj_t *l = lv_label_create(g_un_list);
    lv_label_set_text(l, msg);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, 260);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(l, TC(TEXT_MUTED), 0);
    lv_obj_set_style_text_font(l, TF(UI_14), 0);
    return l;
}

static void un_empty(const char *msg){
    theme_kit()->empty_state(KIT_EMPTY_UPNEXT, un_note(msg));
}

/* The list must still describe what is playing: same track (identity epoch), same queue row (an unknown pos_id on
 * either side counts only if it is unknown on both - a zero-to-known change means the list was read blind), same
 * play mode. */
static cw *un_cast_cw(void){ return playback_source() == PB_SRC_CAST ? cw_ui() : NULL; }

static int un_still_current(void){
    cw *w = un_cast_cw();
    if(g_un_cast || remote_queue_active()){   /* Cast: same queue, position and mode in the published view; a source change either way is stale */
        cw_view v;
        int act = remote_view(w, &v);
        return g_un_cast && remote_queue_active() && g_un_source == (int)playback_source() &&
               (g_un_source != PB_SRC_AIRPLAY || g_un_attempt == ui_airplay_attempt()) &&
               (act ? cw_view_same(&v, &g_un_view) : !g_un_view.active);   /* still no view published = nothing changed */
    }
    track_state_t st; ipc_get_state(&st);
    if(!st.have_track || st.path_seq != g_un_seq || strcmp(st.path, g_un_path) != 0) return 0;
    return st.pos_id == g_un_pos && ui_play_mode_now() == g_un_mode;
}

/* Only the current label follows live metadata; queue order and scroll stay intact. */
static void un_update_title(const track_state_t *st){
    if(!g_un_title) return;
    const char *text = "";
    if(st->have_track && st->path_seq == g_un_seq && st->pos_id == g_un_pos &&
       !strcmp(st->path, g_un_path) && !strcmp(st->path, g_un_rows[0].path))
        text = st->title_ready ? (st->title[0] ? st->title : "Untitled") : "";
    if(strcmp(lv_label_get_text(g_un_title), text)) lv_label_set_text(g_un_title, text);
}

static lv_obj_t *g_un_edit;
static int g_un_suppress = -1, g_un_edit_occ; static uint32_t g_un_edit_gen;
static void un_edit_deleted(lv_event_t *e){ (void)e; g_un_edit = NULL; }
static void un_edit_close(void){ if(g_un_edit) lv_obj_delete(g_un_edit); }
static void un_edit_choose(lv_event_t *e){
    unsigned operation = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    if(operation && (!un_still_current() || ui_airplay_queue_edit(g_un_edit_gen, g_un_edit_occ, operation))) ui_toast("Queue changed");
    un_edit_close();
}
static void un_edit_open(lv_event_t *e){
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(playback_source() != PB_SRC_AIRPLAY || !g_un_cast || i <= 0 || i >= g_un_n || !un_still_current()) return;
    g_un_suppress = i; un_edit_close();
    g_un_edit_occ = g_un_view.occ[i]; g_un_edit_gen = g_un_view.qgen;
    g_un_edit = lv_obj_create(lv_obj_get_parent(g_un_list)); lv_obj_remove_style_all(g_un_edit);
    lv_obj_set_size(g_un_edit, 360, 360); lv_obj_remove_flag(g_un_edit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(g_un_edit, TC(CANVAS), 0); lv_obj_set_style_bg_opa(g_un_edit, LV_OPA_COVER, 0);
    ui_on(g_un_edit, un_edit_deleted, LV_EVENT_DELETE, NULL, "upnext.edit.deleted", UI_CORE);
    lv_obj_t *title = lv_label_create(g_un_edit); lv_label_set_text(title, g_un_rows[i].title);
    lv_obj_set_pos(title, 65, 75); lv_obj_set_size(title, 230, 40); lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(title, TF(USER_16), 0); lv_obj_set_style_text_color(title, TC(TEXT_PRIMARY), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    const char *labels[] = {"Play next", "Remove from Up Next", "Cancel"};
    const unsigned operations[] = {APS_QUEUE_MOVE_NEXT, APS_QUEUE_REMOVE, 0};
    for(unsigned k = 0; k < 3; k++){
        lv_obj_t *button = lv_button_create(g_un_edit); lv_obj_remove_style_all(button);
        lv_obj_set_pos(button, 58, 135+(int)k*52); lv_obj_set_size(button, 244, 44);
        lv_obj_set_style_radius(button, 12, 0); lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(button, TC(SURFACE_RAISED), 0); lv_obj_set_style_bg_color(button, TC(RAISED_PRESSED), LV_STATE_PRESSED);
        lv_obj_t *label = lv_label_create(button); lv_label_set_text(label, labels[k]); lv_obj_center(label);
        lv_obj_set_style_text_font(label, TF(UI_14), 0); lv_obj_set_style_text_color(label, TC(TEXT_PRIMARY), 0);
        ui_on(button, un_edit_choose, LV_EVENT_CLICKED, (void *)(uintptr_t)operations[k], "upnext.edit.choose", UI_CORE);
    }
    kit_pass(g_un_edit);
}
static void un_row_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(i < 0 || i >= g_un_n) return;
    int suppressed = g_un_suppress; g_un_suppress = -1; if(i == suppressed) return;
    if(i == 0){ screen_return_to(SCR_NOWPLAYING); return; }        /* the current song */
    if(!un_still_current()){ un_rebuild(); ui_toast("Queue changed"); return; }   /* never jump from a stale list */
    if(g_un_cast && playback_source() == PB_SRC_AIRPLAY && (g_un_view.fl[i] & (CQ_F_CUE | CQ_F_ISO))){
        ui_toast(g_un_view.fl[i] & CQ_F_ISO ? "SACD ISO can't be beamed" : "CUE tracks can't be beamed"); return;
    }
    if(g_un_cast){   /* the worker re-checks the qgen: a queue replaced since says "Queue changed", a refused post "updating" */
        if((playback_source() == PB_SRC_AIRPLAY ? ui_airplay_queue_jump(g_un_view.qgen, g_un_view.occ[i]) : cw_jump(un_cast_cw(), g_un_view.qgen, g_un_view.occ[i])) != 0){ ui_toast("Queue is updating - try again"); return; }
        screen_return_to(SCR_NOWPLAYING);
        return;
    }
    /* and the target row itself must still be that song at that position (a rebuild can keep the current song) */
    int v = mdb_queue_row_valid(g_un_rows[i].base_id, g_un_rows[i].ord, g_un_rows[i].path);
    if(v < 0){ ui_toast("Queue is busy - try again"); return; }
    if(v == 0){ un_rebuild(); ui_toast("Queue changed"); return; }
    if(ui_queue_jump(g_un_rows[i].ord) == 0) screen_return_to(SCR_NOWPLAYING);
}

static void un_add_row(int i){
    const mdb_qrow_t *q = &g_un_rows[i];
    int cur = (i == 0);
    lv_obj_t *r = lv_button_create(g_un_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 268, 52);
    lv_obj_set_style_radius(r, 10, 0);
    lv_obj_set_style_bg_color(r, (cur ? TC(SURFACE_RAISED) : TC(SURFACE)), 0);
    lv_obj_set_style_bg_opa(r, cur ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_bg_color(r, TC(RAISED_PRESSED), LV_STATE_PRESSED);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    ui_on(r, un_edit_open, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i, "upnext.edit", UI_CORE);
    ui_on(r, un_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i, "upnext.un_row", UI_CORE);

    lv_obj_t *t = lv_label_create(r);                           /* title (may be non-Latin) */
    if(cur && !g_un_cast){
        g_un_title = t;
        track_state_t st; ipc_get_state(&st); un_update_title(&st);
    } else lv_label_set_text(t, q->title[0] ? q->title : "Untitled");
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(t, 14, 6); lv_obj_set_size(t, 186, 21);
    lv_obj_set_style_text_font(t, TF(USER_16), 0);
    lv_obj_set_style_text_color(t, TC(TEXT_PRIMARY), 0);

    lv_obj_t *s = lv_label_create(r);                           /* artist; "Now Playing" on the current row */
    const char *subtitle = cur ? "Now Playing" : q->artist;
    if(g_un_cast && playback_source() == PB_SRC_AIRPLAY){
        if(audio_stock_unavailable_label(q->path)) subtitle = audio_stock_unavailable_label(q->path);
        else if(g_un_view.fl[i] & CQ_F_ISO) subtitle = "SACD ISO can't be beamed";
        else if(g_un_view.fl[i] & CQ_F_CUE) subtitle = "CUE track can't be beamed";
    }
    lv_label_set_text(s, subtitle);
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s, 14, 28); lv_obj_set_size(s, 186, 17);
    lv_obj_set_style_text_font(s, cur ? TF(UI_14) : TF(USER_14), 0);
    lv_obj_set_style_text_color(s, cur ? TC(ACCENT_PRIMARY) : TC(TEXT_MUTED), 0);

    char dur[12]; un_fmt_dur(q->dur_ms, dur, sizeof dur);
    if(dur[0]){
        lv_obj_t *d = lv_label_create(r);                       /* length, right-aligned */
        lv_label_set_text(d, dur);
        lv_obj_set_pos(d, 202, 17); lv_obj_set_size(d, 54, 18);
        lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(d, TF(UI_12), 0);
        lv_obj_set_style_text_color(d, TC(TEXT_MUTED), 0);
        kit_row_value(r, t, s, d, KIT_VALUE_DURATION);
    }
}

static void un_footer(void){
    if(g_un_mode == 2) return;                                  /* Repeat One: the caption already says it */
    if(g_un_n == 1 && !g_un_more){
        un_note(g_un_mode == 3 ? "End of the queue - then it starts again" : "Nothing after this song");
        return;
    }
    if(g_un_more > 0){
        char b[48]; snprintf(b, sizeof b, "+ %d more song%s", g_un_more, g_un_more == 1 ? "" : "s");
        un_note(b);
    }
}

static void un_fill_stop(void){ if(g_un_fill){ lv_timer_delete(g_un_fill); g_un_fill = NULL; } }
static void un_fill_cb(lv_timer_t *t){
    (void)t;
    int end = g_un_fill_i + 20; if(end > g_un_n) end = g_un_n;
    for(; g_un_fill_i < end; g_un_fill_i++) un_add_row(g_un_fill_i);
    if(g_un_fill_i >= g_un_n){ un_fill_stop(); un_footer(); }
}

/* A caption for the play modes where "what plays next" is not simply the next row. */
static void un_mode_caption(int mode){
    const char *c = mode == 1 ? "Shuffle is on - this is the shuffled order"
                  : mode == 2 ? "Repeat One - this song repeats"
                  : mode == 3 ? "Repeat All - the queue starts again after the last song"
                  : mode == 4 ? "Single - playback stops after this song"
                  : NULL;
    if(c) un_note(c);
}

static void un_show(int n, int mode){   /* n rows are read: caption, the first screenful, the rest a few per tick, the footer */
    g_un_retries = 0;
    g_un_n = n;
    un_mode_caption(mode);
    g_un_fill_i = 0;
    int first = n < UN_FIRST ? n : UN_FIRST;
    for(; g_un_fill_i < first; g_un_fill_i++) un_add_row(g_un_fill_i);
    if(g_un_fill_i < g_un_n) g_un_fill = lv_timer_create(un_fill_cb, 16, NULL);
    else un_footer();
}

static void un_transient(void){ g_un_retry = 1; un_empty(g_un_retries < UN_RETRY ? "Queue updating..." : "The queue isn't available right now"); }

/* Cast: the rows are the view's occurrences in play order (current first), fetched by id: what is shown is what the cursor will play. */
static void un_rebuild_cast(cw *w){
    g_un_cast = 1; g_un_source = playback_source();
    g_un_attempt = g_un_source == PB_SRC_AIRPLAY ? ui_airplay_attempt() : 0;
    memset(&g_un_view, 0, sizeof g_un_view);
    if(!remote_view(w, &g_un_view)){ un_transient(); return; }   /* no cast queue published yet (ENTER still reading it) */
    const cw_view *v = &g_un_view;
    g_un_mode = v->mode;
    int n = cast_rows_by_ids(v->src, v->id, v->fl, v->occ, v->cnt, g_un_rows);
    if(n == -CQ_REBUILDING){ un_transient(); return; }
    if(n <= 0 || n != v->cnt){ un_empty(n == 0 ? "The queue is empty" : "Couldn't read the queue"); return; }
    g_un_more = v->n_queue - v->pos - n;
    un_show(n, v->mode);
}

static void un_rebuild(void){
    un_edit_close();
    if(!g_un_list) return;
    un_fill_stop();
    g_un_title = NULL;
    lv_obj_clean(g_un_list);
    lv_obj_scroll_to_y(g_un_list, 0, LV_ANIM_OFF);
    g_un_n = 0; g_un_more = 0; g_un_path[0] = 0; g_un_pos = 0; g_un_retry = 0; g_un_mode = -1; g_un_cast = 0;
    cw *cwk = un_cast_cw();
    if(remote_queue_active()){ un_rebuild_cast(cwk); return; }
    track_state_t st; ipc_get_state(&st);
    g_un_seq = st.path_seq;
    g_un_pos = st.pos_id;                                        /* what the list was read against, even when empty, */
    int mode = ui_play_mode_now();                               /* so the watcher only rebuilds on a real change   */
    g_un_mode = mode;
    if(!st.have_track || !st.path[0]){ un_empty("Nothing playing"); return; }
    if(mdb_is_book_path(st.path)){ un_empty("Audiobooks play one book at a time - use Chapters"); return; }
    snprintf(g_un_path, sizeof g_un_path, "%s", st.path);
    int n = mode < 0 ? MDB_UPNEXT_NOTFOUND : mdb_upnext(mode == 1, st.pos_id, st.path, g_un_rows, UN_SHOW + 1, &g_un_more);
    if(n == MDB_UPNEXT_NOTFOUND || n == MDB_UPNEXT_BUSY){           /* transient: the watch timer retries */
        g_un_retry = 1;
        un_empty(g_un_retries < UN_RETRY ? "Queue updating..." : "The queue isn't available right now");
        return;
    }
    if(n == MDB_UPNEXT_ERROR){ un_empty("Couldn't read the queue"); return; }
    if(n == MDB_UPNEXT_EMPTY){ un_empty("The queue is empty"); return; }
    un_show(n, mode);
}

/* While open: follow track changes, and retry a queue that was being rebuilt. */
static void un_watch_cb(lv_timer_t *t){
    (void)t;
    if(screen_current() != SCR_UPNEXT){ un_edit_close(); lv_timer_delete(g_un_watch); g_un_watch = NULL; return; }
    if(g_un_retry && g_un_retries < UN_RETRY){ g_un_retries++; un_rebuild(); return; }
    if(g_un_cast || remote_queue_active()){ if(!un_still_current()){ g_un_retries = 0; un_rebuild(); } return; }   /* Cast: the published view changed (or the source did) */
    /* a new track, a new queue row for the same file (CUE), a pos_id that became known, or a new play mode */
    track_state_t st; ipc_get_state(&st);
    if(st.path_seq != g_un_seq || st.pos_id != g_un_pos || ui_play_mode_now() != g_un_mode){ g_un_retries = 0; un_rebuild(); }
    else un_update_title(&st);
}

void upnext_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    ui_header(root, "Up Next");

    g_un_list = lv_obj_create(root);
    lv_obj_remove_style_all(g_un_list);
    lv_obj_set_pos(g_un_list, 30, 72); lv_obj_set_size(g_un_list, 300, 272);
    lv_obj_set_style_pad_bottom(g_un_list, 44, 0);                 /* last row scrolls clear of the round bezel */
    lv_obj_set_style_bg_opa(g_un_list, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(g_un_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_un_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_un_list, 6, 0);
    lv_obj_set_scroll_dir(g_un_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_un_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(g_un_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
}

/* Called on every SCR_UPNEXT show (screenmgr), so a deep link or a back-navigation also reads the queue fresh. */
void upnext_refresh(void){
    g_un_retries = 0;
    un_rebuild();
    if(!g_un_watch) g_un_watch = lv_timer_create(un_watch_cb, 1000, NULL);
}

void upnext_open(void){ screen_show(SCR_UPNEXT); }
