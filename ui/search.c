/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "indic.h"
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "musicdb.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_RESULTS 80

static lv_obj_t *g_back;      /* back chevron, anchored across query states */
static lv_obj_t *g_bar;       /* the tappable search field */
static lv_obj_t *g_bar_lbl;   /* the search-bar text (query or placeholder) */
static lv_obj_t *g_clear;     /* clear-query (X) button, shown only with a query */
static lv_obj_t *g_hint;      /* "Songs, artists, albums" sub-hint (empty state only) */
static lv_obj_t *g_results;

/* Two layouts: EMPTY (nothing typed) centres the field in the middle of the screen; ACTIVE (a query)
 * pins the field to the top and gives the whole screen to results. */
static void search_layout(int active){
    if(!indic_text(lv_label_get_text(g_bar_lbl)) && lv_obj_has_state(g_bar_lbl, LV_STATE_USER_4)) lv_obj_remove_state(g_bar_lbl, LV_STATE_USER_4);
    if(active){
        lv_obj_set_size(g_bar, 240, 40);
        lv_obj_align(g_bar, LV_ALIGN_TOP_LEFT, 60, 76);
        lv_obj_set_size(g_bar_lbl, 184, indic_text(lv_label_get_text(g_bar_lbl)) ? indic_line_height(lv_label_get_text(g_bar_lbl), lv_obj_get_style_text_font(g_bar_lbl, 0)) : 18);
        lv_obj_set_style_text_align(g_bar_lbl, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(g_bar_lbl, LV_ALIGN_LEFT_MID, 14, 0);
        if(g_clear) lv_obj_set_pos(g_clear, 264, 81);
        if(g_hint) lv_obj_add_flag(g_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(g_results, 40, 124); lv_obj_set_size(g_results, 280, 198);
        lv_obj_remove_flag(g_results, LV_OBJ_FLAG_HIDDEN);
    } else {
        if(g_back){ if(ui_edge_nav()) lv_obj_set_pos(g_back, 0, 140); else lv_obj_set_pos(g_back, 72, 24); }   /* shared header position in both empty and result states */
        if(g_back && !ui_edge_nav()) lv_obj_set_ext_click_area(g_back, 10);
        lv_obj_set_size(g_bar, ui_edge_nav() ? 150 : 128, 40);  /* the header Back (x72..116) stays clear of it */
        lv_obj_align(g_bar, LV_ALIGN_CENTER, 0, -8);            /* centred; narrow enough that the arrow clears its left edge */
        lv_obj_set_width(g_bar_lbl, LV_SIZE_CONTENT);
        lv_obj_set_style_text_align(g_bar_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(g_bar_lbl, LV_ALIGN_CENTER, 0, 0);
        if(g_hint){ lv_obj_remove_flag(g_hint, LV_OBJ_FLAG_HIDDEN); lv_obj_align(g_hint, LV_ALIGN_CENTER, 0, 40); }
        lv_obj_add_flag(g_results, LV_OBJ_FLAG_HIDDEN);
    }
    ui_headers_fit(lv_obj_get_parent(g_bar));
    if(g_back && ui_edge_nav()) lv_obj_move_foreground(g_back);   /* above the results list it overlaps */
}
static library_song_click_cb_t g_song_cb;
static char g_query[96];

void search_set_song_click_cb(library_song_click_cb_t cb){ g_song_cb = cb; }

static void result_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    int id = (int)(uintptr_t)lv_event_get_user_data(e);
    if(!g_song_cb || !g_song_cb(id)) return;   /* refused / not sent: already toasted, stay on the results */
    screen_show(SCR_NOWPLAYING);
}
static void back_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) screen_back(); }

static void rebuild(const char *q){
    lv_obj_clean(g_results);
    static const mdb_song_t *buf[MAX_RESULTS + 1];   /* +1 to detect "more than 80" vs "exactly 80" */
    int n = (q && q[0]) ? mdb_search(q, buf, MAX_RESULTS + 1) : 0;
    int shown = n > MAX_RESULTS ? MAX_RESULTS : n;
    if(n<=0){
        lv_obj_t *l = indic_label_create(g_results);
        lv_label_set_text(l, (q && q[0]) ? "No results" : "Tap the bar to search");
        lv_obj_set_style_text_color(l, TC(TEXT_MUTED), 0);
        lv_obj_set_style_text_font(l, TF(UI_14), 0);
        if(q && q[0]) theme_kit()->empty_state(KIT_EMPTY_SEARCH, l);
        return;
    }
    for(int i=0;i<shown;i++){
        const mdb_song_t *s = buf[i];
        lv_obj_t *row = lv_obj_create(g_results);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 268, 46);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_color(row, TC(LIST_PRESSED), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_70, LV_STATE_PRESSED);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        ui_on(row, result_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)s->id, "search.result", UI_CORE);
        lv_obj_t *t = indic_label_create(row);
        lv_label_set_text(t, s->title[0]?s->title:"Untitled");
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(t, 12, 5); lv_obj_set_size(t, 244, 19);
        lv_obj_set_style_text_font(t, TF(USER_16), 0);   /* CJK titles via Source Han Sans fallback */
        lv_obj_set_style_text_color(t, TC(TEXT_PRIMARY), 0);
        lv_obj_t *a = indic_label_create(row);
        lv_label_set_text(a, s->artist);
        lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(a, 12, 25); lv_obj_set_size(a, 244, 16);
        lv_obj_set_style_text_font(a, TF(USER_14), 0);
        lv_obj_set_style_text_color(a, TC(TEXT_SECONDARY), 0);
        indic_row_fit(row, t, a);
    }
    if(n > MAX_RESULTS){    /* genuinely truncated - tell the user to narrow down */
        lv_obj_t *f = indic_label_create(g_results);
        lv_label_set_text(f, "Showing first 80 - refine to narrow");
        lv_obj_set_style_text_color(f, TC(TEXT_MUTED), 0);
        lv_obj_set_style_text_font(f, TF(UI_14), 0);
    }
}

static lv_color_t query_color(int has_query){
    /* Kits can turn a neutral field into paper: its foreground must follow that
     * surface when the query changes after the one-time styling pass. */
    if(lv_color_eq(lv_obj_get_style_bg_color(g_bar, 0), TC(TEXT_PRIMARY))) return TC(CANVAS);
    return has_query ? TC(TEXT_PRIMARY) : TC(TEXT_MUTED);
}
static void on_query(const char *text){
    if(!text) return;
    snprintf(g_query, sizeof g_query, "%s", text);
    if(g_bar_lbl) lv_label_set_text(g_bar_lbl, g_query[0] ? g_query : "Search library");
    if(g_bar_lbl) lv_obj_set_style_text_color(g_bar_lbl,
        query_color(g_query[0] != 0), 0);
    if(g_clear){
        if(g_query[0]) lv_obj_clear_flag(g_clear, LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(g_clear, LV_OBJ_FLAG_HIDDEN);
    }
    search_layout(g_query[0] != 0);   /* centre when empty, top+results when searching */
    rebuild(g_query);
}
#ifdef DISKOS_HOST
void search_host_query(const char *text){ on_query(text); }
#endif
static void clear_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) on_query(""); }
static void on_query_done(const char *text){
    if(text && text[0]) on_query(text);   /* Cancel or empty Save keeps the results; the X button clears */
}
static void bar_cb(lv_event_t *e){
    if(lv_event_get_code(e)==LV_EVENT_CLICKED) kbinput_open_limited("Search", g_query, on_query_done, sizeof g_query-1);
}

void search_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    ui_header_cb(root, "Search", back_cb);
    g_back = lv_obj_get_child(root, 0);   /* the shared builders create Back first */

    /* search field - tap to open the keyboard modal. Position/size is set by search_layout(). */
    g_bar = lv_button_create(root);
    lv_obj_remove_style_all(g_bar);
    lv_obj_set_ext_click_area(g_bar, 2);
    lv_obj_set_style_radius(g_bar, 12, 0);
    lv_obj_set_style_bg_color(g_bar, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(g_bar, LV_OPA_COVER, 0);
    ui_on(g_bar, bar_cb, LV_EVENT_CLICKED, NULL, "search.bar", UI_CORE);
    g_bar_lbl = indic_label_create(g_bar);
    lv_label_set_text(g_bar_lbl, "Search library");
    lv_label_set_long_mode(g_bar_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_size(g_bar_lbl, 100, 18);   /* fixed height = ONE line: LONG_DOT truncates instead of wrapping */
    lv_obj_set_style_text_font(g_bar_lbl, TF(USER_14), 0);   /* CJK/Cyrillic queries don't tofu in the pill */
    lv_obj_set_style_text_color(g_bar_lbl, TC(TEXT_MUTED), 0);
    lv_obj_align(g_bar_lbl, LV_ALIGN_LEFT_MID, 14, 0);

    /* clear (X) - sits at the bar's right edge, only visible with a query.
     * Pulled in from x=274/34x38 (top-right corner ~200px from screen center,
     * well outside the r=180 round bezel) to x=236/30x30, vertically centered
     * on the 38px bar (y 26-64 -> button y 30-60). Worst corner (266,30) is
     * ~172.9px from center (180,180), inside the ~176px safe radius with a
     * few px to spare; ext_click_area below still gives a >=44px tap target. */
    g_clear = lv_button_create(root);
    lv_obj_remove_style_all(g_clear);
    lv_obj_set_pos(g_clear, 219, 31);      /* inside the centred bar's right edge (bar 105..255) */
    lv_obj_set_size(g_clear, 30, 30);
    lv_obj_set_ext_click_area(g_clear, 7);
    lv_obj_set_style_radius(g_clear, 10, 0);
    lv_obj_set_style_bg_color(g_clear, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(g_clear, LV_OPA_70, LV_STATE_PRESSED);
    ui_on(g_clear, clear_cb, LV_EVENT_CLICKED, NULL, "search.clear", UI_CORE);
    lv_obj_t *ci = indic_label_create(g_clear);
    lv_label_set_text(ci, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(ci, TC(TEXT_SECONDARY), 0);
    lv_obj_center(ci);
    lv_obj_add_flag(g_clear, LV_OBJ_FLAG_HIDDEN);

    /* sub-hint under the centred field (empty state only) */
    g_hint = indic_label_create(root);
    lv_label_set_text(g_hint, "Songs, artists, albums");
    lv_obj_set_style_text_font(g_hint, TF(UI_14), 0);
    lv_obj_set_style_text_color(g_hint, TC(TEXT_DISABLED), 0);

    /* results - now own the whole lower screen */
    g_results = lv_obj_create(root);
    lv_obj_remove_style_all(g_results);
    lv_obj_set_pos(g_results, 32, 74);
    lv_obj_set_size(g_results, 300, 268);
    lv_obj_set_style_bg_opa(g_results, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(g_results, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_results, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_results, 6, 0);
    lv_obj_set_style_pad_bottom(g_results, 44, 0);   /* last rows scroll clear of the round bottom bezel */
    lv_obj_set_scroll_dir(g_results, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_results, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(g_results, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    search_layout(g_query[0] != 0);   /* centred empty state on first entry */
    rebuild(g_query);
}

void search_refresh(void){
    if(g_results) rebuild(g_query);
}

lv_obj_t *search_scroller(void){ return g_results; }
