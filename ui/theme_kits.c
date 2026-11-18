/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* theme_kits.c - each unique theme's component kit (theme_kit.h): its styler and the builders it replaces. */
#include "indic.h"
#include "rowpool.h"
/* Theme-only harnesses have no data lists. */
#pragma weak row_pool_model
#pragma weak row_pool_slot
#pragma weak row_slot_index
#pragma weak row_pool_phase
#pragma weak row_pool_number
#include "theme.h"
#include "theme_kit.h"
#include "ui_dialog.h"
#include "screens.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "theme_fonts.h"

lv_obj_t *kit_edge_header(lv_obj_t *root, const char *title, lv_event_cb_t back_cb);   /* ui.c */
int ui_screen_backlight_on(void);
int screen_figure_number(int which);

void kit_default_empty_state(kit_empty_kind_t kind, lv_obj_t *message){
    if(strcmp(theme_kit()->id, "default") || !message) return;
    (void)kind;
    lv_obj_set_style_text_font(message, TF(UI_14), 0);
    lv_obj_set_style_text_color(message, TC(TEXT_MUTED), 0);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_update_layout(message);
    lv_area_t a; lv_obj_get_coords(message, &a);
    lv_obj_set_style_translate_y(message, 180 - (a.y1 + a.y2) / 2, 0);
}

void kit_default_np_cover_state(lv_obj_t *cover, lv_obj_t *note, int show, int loading){
    if(strcmp(theme_kit()->id, "default") || !cover || !note) return;
    /* The cover itself is the rounded placeholder and remains the art tap target. */
    lv_obj_set_style_bg_color(cover, TC(ART_PLACEHOLDER), 0);
    lv_obj_set_style_radius(cover, 16, 0);
    lv_label_set_text(note, loading ? LV_SYMBOL_REFRESH : LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(note, TF(UI_32), 0);   /* the built-in symbol font contains the music note */
    lv_obj_set_style_text_color(note, loading ? TC(TEXT_PRIMARY) : TC(ACCENT_PRIMARY), 0);
    if(show) lv_obj_center(note);
}

/* The capsule is a display-only perimeter: parent MAIN_END precedes every child.
 * Five native draw primitives, no extra object or rounded clipping layer.
 * Arc radius is the OUTER radius in LVGL; lengths use its stroke centreline. */
typedef struct {
    int progress;
    lv_color_t accent, color;
    int variant;
} island_progress_t;

static float island_luminance(lv_color_t c){
    float v[3] = {c.red / 255.f, c.green / 255.f, c.blue / 255.f};
    for(int i = 0; i < 3; i++) v[i] = v[i] <= .04045f ? v[i]/12.92f : powf((v[i]+.055f)/1.055f, 2.4f);
    return .2126f*v[0] + .7152f*v[1] + .0722f*v[2];
}
static lv_color_t island_color(lv_color_t accent){
    /* Default dark historically passes arbitrary album colours through. Fit this
     * new indicator to 3:1 on both sides of its border, retaining the hue. */
    float bg[3] = {island_luminance(TC(CANVAS)), island_luminance(TC(SURFACE)),
                   island_luminance(TC(SURFACE_PRESSED))};
    lv_color_t c = accent;
    for(int n = 0; n < 256; n++){
        float l = island_luminance(c);
        int pass = 1;
        for(int i = 0; i < 3; i++) if((fmaxf(l,bg[i])+.05f)/(fminf(l,bg[i])+.05f) < 3.f) pass = 0;
        if(pass) break;
        int r = c.red, g = c.green, b = c.blue;
        if(theme_variant()){ if(r) r--; if(g) g--; if(b) b--; }
        else { if(r < 255) r++; if(g < 255) g++; if(b < 255) b++; }
        c = theme_color_from_rgb((uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b);
    }
    return c;
}

/* Visit only the requested interval, in clockwise order from the left end's
 * 127-degree point, matching Now Playing's rotation. Used for drawing AND damage. */
static void island_visit(lv_obj_t *pill, float from, float to, lv_layer_t *layer, lv_color_t color){
    if(to <= from) return;
    lv_area_t a; lv_obj_get_coords(pill, &a);
    int radius = lv_obj_get_height(pill)/2;
    int straight = lv_obj_get_width(pill) - 2*radius;
    if(radius < 3 || straight < 0) return;
    float unit = (radius - 1.5f) * (float)M_PI / 180.f;
    float lengths[5] = {143*unit, straight, 180*unit, straight, 37*unit};
    float total = 2*straight + 360*unit, cursor = 0;
    from *= total; to *= total;
    for(int i = 0; i < 5; i++){
        float lo = fmaxf(from-cursor, 0), hi = fminf(to-cursor, lengths[i]);
        cursor += lengths[i];
        if(hi <= lo) continue;
        lv_area_t damage;
        if(i == 1 || i == 3){
            int x = i == 1 ? a.x1+radius : a.x2+1-radius;
            int y = i == 1 ? a.y1+1 : a.y2-1;
            int sign = i == 1 ? 1 : -1;
            lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
            d.p1 = (lv_point_precise_t){x+sign*(int)lroundf(lo), y};
            d.p2 = (lv_point_precise_t){x+sign*(int)lroundf(hi), y};
            if(d.p1.x == d.p2.x) continue;
            d.width = 3; d.color = color; d.opa = LV_OPA_COVER;
            d.round_start = d.round_end = 1;
            damage = (lv_area_t){LV_MIN(d.p1.x,d.p2.x)-3, y-3, LV_MAX(d.p1.x,d.p2.x)+3, y+3};
            if(layer) lv_draw_line(layer, &d);
        } else {
            int start = i == 0 ? 127 : i == 2 ? 270 : 90;
            lv_draw_arc_dsc_t d; lv_draw_arc_dsc_init(&d);
            d.center = (lv_point_t){i == 2 ? a.x2+1-radius : a.x1+radius, a.y1+radius};
            d.radius = radius; d.width = 3; d.color = color;
            d.opa = LV_OPA_COVER; d.rounded = 1;
            d.start_angle = start + (int)lroundf(lo/unit);
            d.end_angle = start + (int)lroundf(hi/unit);
            if(d.start_angle == d.end_angle) continue;
            lv_draw_arc_get_area(d.center.x, d.center.y, d.radius,
                                 d.start_angle, d.end_angle, d.width, true, &damage);
            if(layer) lv_draw_arc(layer, &d);
        }
        if(!layer) lv_obj_invalidate_area(pill, &damage);
    }
}
static void island_event(lv_event_t *e){
    island_progress_t *s = lv_event_get_user_data(e);
    if(lv_event_get_code(e) == LV_EVENT_DELETE){ free(s); return; }
    if(lv_event_get_code(e) == LV_EVENT_DRAW_MAIN_END)
        island_visit(lv_event_get_target(e), 0, s->progress/1000.f, lv_event_get_layer(e), s->color);
}
void kit_default_home_progress(lv_obj_t *pill, int progress, int have_track, lv_color_t accent){
    if(strcmp(theme_kit()->id, "default") || !pill) return;
    island_progress_t *s = NULL;
    for(uint32_t i = 0; i < lv_obj_get_event_count(pill); i++){
        lv_event_dsc_t *d = lv_obj_get_event_dsc(pill, i);
        if(lv_event_dsc_get_cb(d) == island_event){ s = lv_event_dsc_get_user_data(d); break; }
    }
    if(!s){
        s = calloc(1, sizeof(*s));
        if(!s) return;
        s->variant = -1;
        lv_obj_add_event_cb(pill, island_event, LV_EVENT_ALL, s);
    }
    progress = have_track ? LV_CLAMP(0, progress, 1000) : 0;
    int recolor = s->variant != theme_variant() || !lv_color_eq(s->accent, accent);
    if(recolor){ s->accent = accent; s->color = island_color(accent); s->variant = theme_variant(); }
    int old = s->progress;
    s->progress = progress;
    if(recolor) island_visit(pill, 0, LV_MAX(old,progress)/1000.f, NULL, s->color);
    else if(old != progress) island_visit(pill, LV_MIN(old,progress)/1000.f, LV_MAX(old,progress)/1000.f, NULL, s->color);
}


/* big words and numbers set in Default's large faces (the volume readout, the album-wall letter): the theme's own
 * largest text face instead. Glyph-only labels (icons) keep the icon-bearing face. */
static int is_large_default_text(lv_obj_t *o){
    const char *t = lv_label_get_text(o);
    if(!t || !*t || ((unsigned char)t[0] == 0xEF && strlen(t) == 3)) return 0;
    const lv_font_t *f = lv_obj_get_style_text_font(o, 0);
    return f == theme_font(THEME_FONT_UI_28) || f == theme_font(THEME_FONT_UI_32) || f == theme_font(THEME_FONT_UI_36)
        || f == theme_font(THEME_FONT_UI_40);
}

/* a clock string ("11:33", "5:53 PM") as its time and its meridiem ("" in 24-hour mode) */
static void clock_parts(const char *t, char *hm, int hcap, char *mer, int mcap){
    const char *sp = t ? strchr(t, ' ') : NULL;
    int n = sp ? (int)(sp - t) : (t ? (int)strlen(t) : 0);
    if(n >= hcap) n = hcap - 1;
    memcpy(hm, t ? t : "", (size_t)n); hm[n] = 0;
    lv_snprintf(mer, mcap, "%s", sp ? sp + 1 : "");
}
static lv_obj_t *mer_label(lv_obj_t *parent, const lv_font_t *f, lv_color_t c){
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    kit_keep(l);
    return l;
}
/* Every theme with the round seek ring puts Now Playing's play-order and heart (or moon) buttons exactly where the
 * default theme does (ui.c: TOP_MID +-118, y 126, glyph not re-centred) - owner's rule 2026-09-29; the times lifted
 * clear of the rim */
#define NP_SIDES_Y 126
static void np_sides_on_cover(const np_parts_t *p, int times_y){
    lv_obj_update_layout(p->root);
    lv_obj_t *k[3] = { p->mode, p->fav, p->sleep };
    for(int i = 0; i < 3; i++) lv_obj_set_y(k[i], NP_SIDES_Y);
    if(times_y > 0){ lv_obj_set_y(p->elapsed, times_y); lv_obj_set_y(p->sep, times_y); lv_obj_set_y(p->remain, times_y); }
}
/* Now Playing's title/artist/album rows moved as a block (y of each label's top): used where a theme's play key sits
 * higher than the default's, so the album line clears it (tests/host/np_overlap.py checks the ink clearance) */
static void np_text_rows(const np_parts_t *p, int title_y, int artist_y, int album_y){
    lv_obj_set_y(p->title, title_y);
    lv_obj_set_y(p->artist, artist_y);
    lv_obj_set_y(p->album, album_y);
}
/* ---- AirPlay receiving (rx_parts_t): guarded setters, because a receiver hook runs every tick ---- */
static void rx_place(lv_obj_t *o, lv_align_t al, int x, int y, int w, const lv_font_t *f){
    if(lv_obj_get_style_align(o, 0) != al) lv_obj_set_style_align(o, al, 0);
    lv_obj_set_pos(o, x, y);
    if(w && lv_obj_get_style_width(o, 0) != w) lv_obj_set_width(o, w);
    if(f && lv_obj_get_style_text_font(o, 0) != f) lv_obj_set_style_text_font(o, f, 0);
}
static void rx_show(lv_obj_t *o, int on){
    if(o && !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) != !!on){
        if(on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}
static void rx_wrap(lv_obj_t *l, int wrap){
    lv_label_long_mode_t m = wrap ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT;
    int32_t h = wrap ? LV_SIZE_CONTENT :
        indic_line_height(lv_label_get_text(l),lv_obj_get_style_text_font(l, 0)) +
        lv_obj_get_style_pad_top(l, 0) +
        lv_obj_get_style_pad_bottom(l, 0);
    if(lv_obj_get_style_height(l, 0) != h) lv_obj_set_height(l, h);
    if(lv_label_get_long_mode(l) != m) lv_label_set_long_mode(l, m);
}
static void rx_box(lv_obj_t *k, int x, int y, int w, int h){   /* a key: TOP_MID, x from the centre */
    rx_place(k, LV_ALIGN_TOP_MID, x, y, w, NULL);
    if(lv_obj_get_style_height(k, 0) != h) lv_obj_set_height(k, h);
}
static void rx_label(lv_obj_t *l, const lv_font_t *f, lv_color_t c){
    lv_obj_set_style_text_font(l, f, 0); lv_obj_set_style_text_color(l, c, 0);
}
/* the transport row every receiving theme shares: previous / play-pause / next; y = top of the play key */
static void rx_transport(const rx_parts_t *p, int y, int pp, int side){
    rx_box(p->key[0], -side, y + pp / 2 - 22, 44, 44);
    rx_box(p->key[1], 0, y, pp, pp);
    rx_box(p->key[2], side, y + pp / 2 - 22, 44, 44);
}
static void rx_bg(lv_obj_t *o, lv_color_t c){ if(!lv_color_eq(lv_obj_get_style_bg_color(o, 0), c)) lv_obj_set_style_bg_color(o, c, 0); }
/* Progress, display only (rx_state_t.show: the model trusts the position). The fill of a track at x,y,w,h and the two
 * times under it; both are hidden whenever the model says no. A paused view keeps the bar at the frozen position and
 * shows "Paused" where the times were. */
static void rx_fill(const rx_parts_t *p, const rx_state_t *s, int x, int y, int w, int h, lv_color_t c){
    int fw = s->show && (s->mode == RX_RECEIVING || s->mode == RX_PAUSED) ? (int)((int64_t)w * s->fraction / 1000) : 0;
    rx_show(p->bar, fw > 0);
    if(fw <= 0) return;
    if(lv_obj_get_style_x(p->bar, 0) != x || lv_obj_get_style_y(p->bar, 0) != y) lv_obj_set_pos(p->bar, x, y);
    if(lv_obj_get_style_width(p->bar, 0) != fw) lv_obj_set_width(p->bar, fw);
    if(lv_obj_get_style_height(p->bar, 0) != h) lv_obj_set_height(p->bar, h);
    rx_bg(p->bar, c);
}
static void rx_times(const rx_parts_t *p, const rx_state_t *s, int x, int y, int w){
    int on = s->show && s->mode == RX_RECEIVING;
    rx_show(p->elapsed, on); rx_show(p->remain, on);
    if(!on) return;
    rx_place(p->elapsed, LV_ALIGN_TOP_LEFT, x, y, w / 2, NULL); rx_place(p->remain, LV_ALIGN_TOP_LEFT, x + w / 2, y, w / 2, NULL);
}
static void rx_times_style(const rx_parts_t *p, const lv_font_t *f, lv_color_t c){
    rx_label(p->elapsed, f, c); rx_label(p->remain, f, c);
    lv_obj_set_style_text_align(p->elapsed, LV_TEXT_ALIGN_LEFT, 0); lv_obj_set_style_text_align(p->remain, LV_TEXT_ALIGN_RIGHT, 0);
}
/* The ring form (Default's arc, driven by a hook): the indicator follows the model's fraction, dimmed while paused and
 * frozen, transparent whenever the model says no. The theme styles the indicator (width, colour) once, in `first`. */
static void rx_ring(const rx_parts_t *p, const rx_state_t *s){
    int on = s->show && (s->mode == RX_RECEIVING || s->mode == RX_PAUSED), v = on ? s->fraction : 0;
    if(lv_arc_get_value(p->arc) != v) lv_arc_set_value(p->arc, v);
    lv_opa_t o = !on || v <= 0 ? LV_OPA_TRANSP : s->mode == RX_PAUSED ? LV_OPA_50 : LV_OPA_COVER;
    if(lv_obj_get_style_arc_opa(p->arc, LV_PART_INDICATOR) != o) lv_obj_set_style_arc_opa(p->arc, o, LV_PART_INDICATOR);
}
static void rx_frame(lv_obj_t *f, int x, int y, int w, int h){   /* the cover frame, from the top left */
    rx_place(f, LV_ALIGN_TOP_LEFT, x, y, w, NULL);
    if(lv_obj_get_style_height(f, 0) != h) lv_obj_set_height(f, h);
}
/* A theme that left-aligns the rows gets Default's centred placement back, once, for the states it does not own. */
static void rx_recenter(const rx_parts_t *p, int *owned){
    if(!*owned) return;
    *owned = 0;
    for(int i = 0; i < 5; i++){
        rx_place(p->text[i], LV_ALIGN_TOP_MID, 0, 0, i == 3 ? 230 : 280, NULL);
        lv_obj_set_style_text_align(p->text[i], LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_set_style_max_height(p->text[1], LV_COORD_MAX, 0);
    rx_show(p->text[3], 1);
}
/* The text rows, "Paused", the glyph and the waiting block. y: title, artist, album, paused, glyph, heading, Disc name,
 * hint; f: title face, plain-title face (no metadata yet), waiting-heading face. */
static void rx_rows(const rx_parts_t *p, const rx_state_t *s, const int y[8], const lv_font_t *const f[3]){
    int wait = s->mode == RX_WAITING;
    rx_place(p->text[1], LV_ALIGN_TOP_MID, 0, wait ? y[5] : s->has_title ? y[0] : y[0] + 12, 250, wait ? f[2] : s->has_title ? f[0] : f[1]);
    rx_wrap(p->text[1], wait ||
        (!s->has_title && !s->text[2][0] && !s->text[3][0]));
    rx_place(p->text[2], LV_ALIGN_TOP_MID, 0, wait ? y[6] : y[1], 238, NULL);
    rx_wrap(p->text[2], 0);
    rx_place(p->text[3], LV_ALIGN_TOP_MID, 0, wait ? y[7] : y[2], wait ? 230 : 218, NULL);
    rx_wrap(p->text[3], wait);
    rx_place(p->text[4], LV_ALIGN_TOP_MID, 0, y[3], 200, NULL);
    rx_place(p->icon, LV_ALIGN_TOP_MID, 0, y[4], 0, NULL);
}
/* Keep saver song blocks below the rendered date, including a larger user font. */
static int saver_song_y(lv_obj_t *date, int base){
    if(date && !lv_obj_has_flag(date, LV_OBJ_FLAG_HIDDEN)){
        lv_obj_update_layout(date);
        int below = lv_obj_get_y(date) + lv_obj_get_height(date) + 8;
        if(below > base) base = below;
    }
    return base;
}
/* A rectangular song block needs room at its lower corners on the round glass. */
static int saver_song_width(lv_obj_t *bottom_row, int width){
    lv_obj_update_layout(bottom_row);
    int bottom = lv_obj_get_y(bottom_row) + lv_obj_get_height(bottom_row);
    int dy = bottom - 180;
    if((width / 2) * (width / 2) + dy * dy > 176 * 176){
        int fit = ui_round_width(bottom, 10);
        if(fit < width) width = fit;
    }
    return width;
}
/* the unique themes keep their own backgrounds: no blurred cover behind Home and Now Playing */
static void own_backgrounds(void){ home_set_backdrop_enabled(0); ui_np_backdrop_enabled(0); }

/* Home's Search: the drawn magnifier replaced by the icon glyph in one colour (for filled or small buttons) */
static void home_search_glyph(lv_obj_t *btn, lv_color_t c){
    for(uint32_t i = 0; i < lv_obj_get_child_count(btn); i++) lv_obj_add_flag(lv_obj_get_child(btn, (int32_t)i), LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *mg = lv_label_create(btn);
    lv_label_set_text(mg, "\xEF\x80\x82");                                   /* f002 search */
    lv_obj_set_style_text_font(mg, TF(ICON_20), 0);
    lv_obj_set_style_text_color(mg, c, 0);
    lv_obj_center(mg);
}
/* Home's Library pill resized by a theme: its icon and word laid out again inside the new width */
static void home_library_fit(lv_obj_t *btn){
    lv_obj_t *icon = NULL, *word = NULL;
    for(uint32_t i = 0; i < lv_obj_get_child_count(btn); i++){
        lv_obj_t *c = lv_obj_get_child(btn, (int32_t)i);
        if(!lv_obj_check_type(c, &lv_label_class)) continue;
        if(strlen(lv_label_get_text(c)) == 3) icon = c; else word = c;
    }
    if(icon) lv_obj_align(icon, LV_ALIGN_LEFT_MID, 16, 0);
    if(word){ lv_obj_align(word, LV_ALIGN_LEFT_MID, 46, 0); }
}

/* ================================================================ hi-fi
 * A 1980s hi-fi deck: flat faceplate rows with hairlines, machined plates (6 px corners, a hairline edge) for buttons
 * and popups, spaced small-caps titles, an LCD clock whose unlit segments glow faintly behind the time. */
static void hifi_led_scroll_event(lv_event_t *e);
static void hifi_led_scroll_hooks(lv_obj_t *row, lv_obj_t *led, int on){
    for(lv_obj_t *p = row; p; p = lv_obj_get_parent(p)){
        if(!lv_obj_has_flag(p, LV_OBJ_FLAG_SCROLLABLE)) continue;
        lv_obj_remove_event_cb_with_user_data(p, hifi_led_scroll_event, led);
        if(on) lv_obj_add_event_cb(p, hifi_led_scroll_event, LV_EVENT_SCROLL_BEGIN, led);
    }
}
static void hifi_led_event(lv_event_t *e){
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *led = lv_event_get_user_data(e);
    if(code == LV_EVENT_DELETE){
        hifi_led_scroll_hooks(lv_event_get_target(e), led, 0);
        return;
    }
    if(code != LV_EVENT_PRESSED && code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    hifi_led_scroll_hooks(lv_event_get_target(e), led, code == LV_EVENT_PRESSED);
    lv_obj_set_style_bg_color(led, code == LV_EVENT_PRESSED ? TC(ACCENT_PRIMARY) : TC(BORDER_STRONG), 0);
}
static void hifi_led_scroll_event(lv_event_t *e){
    lv_obj_t *led = lv_event_get_user_data(e);
    lv_obj_set_style_bg_color(led, TC(BORDER_STRONG), 0);
}
static void hifi_led_attach(lv_obj_t *led){
    lv_obj_t *row = lv_obj_get_parent(led);
    for(uint32_t i = 0; i < lv_obj_get_event_count(row); i++){
        lv_event_dsc_t *d = lv_obj_get_event_dsc(row, i);
        if(lv_event_dsc_get_cb(d) == hifi_led_event && lv_event_dsc_get_user_data(d) == led) return;
    }
    lv_obj_add_event_cb(row, hifi_led_event, LV_EVENT_ALL, led);
}
static void hifi_style(lv_obj_t *o, kit_role_t r){
    switch(r){
    case KIT_TITLE:
        lv_obj_set_style_text_letter_space(o, 2, 0);
        lv_obj_set_style_text_color(o, TC(TEXT_SECONDARY), 0);
        break;
    case KIT_ROW:
        lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(o, 6, 0);
        lv_obj_set_style_border_side(o, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(o, 1, 0);
        lv_obj_set_style_border_color(o, TC(BORDER), 0);
        lv_obj_set_style_bg_color(o, TC(SURFACE_RAISED), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_STATE_PRESSED);
        break;
    case KIT_ROW_TEXT:
        lv_obj_set_style_text_letter_space(o, 1, 0);
        break;
    case KIT_ROW_CHEVRON:                             /* the drill-down mark becomes an unlit LED */
        lv_label_set_text(o, "");
        lv_obj_set_size(o, 6, 6);
        lv_obj_set_style_bg_color(o, TC(BORDER_STRONG), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
        lv_obj_align(o, LV_ALIGN_RIGHT_MID, -8, 0);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
        hifi_led_attach(o);
        break;
    case KIT_BUTTON: case KIT_CARD: case KIT_POPUP:
        lv_obj_set_style_radius(o, r == KIT_POPUP ? 8 : 6, 0);
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10){
            lv_obj_set_style_border_width(o, 1, 0);
            lv_obj_set_style_border_color(o, TC(BORDER_STRONG), 0);
            lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
        }
        if(r == KIT_BUTTON){ lv_obj_t *l = lv_obj_get_child(o, 0); if(l) lv_obj_set_style_text_letter_space(l, 1, 0); }
        break;
    case KIT_TOGGLE:
        lv_obj_set_style_radius(o, 4, LV_PART_MAIN);
        lv_obj_set_style_radius(o, 4, LV_PART_INDICATOR);
        lv_obj_set_style_radius(o, 3, LV_PART_KNOB);
        break;
    case KIT_SLIDER:
        lv_obj_set_style_radius(o, 2, LV_PART_MAIN);
        lv_obj_set_style_radius(o, 2, LV_PART_INDICATOR);
        lv_obj_set_style_radius(o, 3, LV_PART_KNOB);
        break;
    case KIT_ROLLER: case KIT_TEXTAREA:
        lv_obj_set_style_radius(o, 6, 0);
        break;
    case KIT_KEYBOARD:
        lv_obj_set_style_radius(o, 4, LV_PART_ITEMS);
        lv_obj_set_style_border_width(o, 1, LV_PART_ITEMS);
        lv_obj_set_style_border_color(o, TC(BORDER_STRONG), LV_PART_ITEMS);
        lv_obj_set_style_border_opa(o, LV_OPA_COVER, LV_PART_ITEMS);
        break;
    case KIT_TEXT:
        if(is_large_default_text(o)) lv_obj_set_style_text_font(o, TF(UI_24), 0);
        break;
    default: break;
    }
}
static void plate(lv_obj_t *o, int radius){
    kit_keep(o);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, TC(BORDER_STRONG), 0);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, TC(SURFACE_RAISED), LV_STATE_PRESSED);
}
static lv_obj_t *g_hifi_clock, *g_hifi_mer, *g_hifi_ghost;
static void hifi_clock(const char *t){
    if(!g_hifi_clock) return;
    char hm[16], mer[8]; clock_parts(t, hm, sizeof hm, mer, sizeof mer);
    lv_label_set_text(g_hifi_clock, hm);
    lv_label_set_text(g_hifi_mer, mer);
    /* 12-hour: the digits and an AM/PM column beside them are centred as ONE group, so "PM" never sits on the last
     * digit (it was pinned to the well's corner, over the 4th digit's lower segments); 24-hour: the digits alone */
    lv_obj_t *well = lv_obj_get_parent(g_hifi_ghost);
    lv_obj_update_layout(g_hifi_ghost);
    int32_t gw = lv_obj_get_width(g_hifi_ghost), mw = 0;
    if(mer[0]){ lv_point_t sz; lv_text_get_size(&sz, "PM", lv_obj_get_style_text_font(g_hifi_mer, 0), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE); mw = sz.x + 4; }
    /* the LCD well grows by the AM/PM column in 12-hour mode (the 88:88 digits alone nearly fill 232 px); still well
     * inside the round screen at this height */
    lv_obj_set_width(well, mer[0] ? gw + mw + 24 : 232);
    lv_obj_update_layout(well);
    int32_t well_w = lv_obj_get_content_width(well);
    lv_obj_align(g_hifi_ghost, LV_ALIGN_LEFT_MID, (well_w - gw - mw) / 2, 0);
    lv_obj_update_layout(g_hifi_clock);
    lv_obj_align_to(g_hifi_clock, g_hifi_ghost, LV_ALIGN_RIGHT_MID, 0, 0);   /* right-aligned: "6:02" lights the last digits */
    if(mer[0]) lv_obj_align_to(g_hifi_mer, g_hifi_ghost, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -6);   /* beside the minutes, near their foot */
}
static void hifi_home(const home_parts_t *p){
    own_backgrounds();
    lv_obj_set_pos(p->status, 0, 38);
    lv_obj_set_style_text_color(p->status, TC(TEXT_MUTED), 0);
    /* the LCD: a dark well; the unlit segments ("88:88") sit behind the time in the ghost colour */
    lv_obj_t *well = lv_obj_create(p->root);
    lv_obj_remove_style_all(well);
    lv_obj_clear_flag(well, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(well, 232, 92);
    lv_obj_align(well, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_set_style_radius(well, 10, 0);
    lv_obj_set_style_bg_color(well, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(well, LV_OPA_COVER, 0);
    lv_obj_move_to_index(well, lv_obj_get_index(p->clock));
    lv_obj_t *ghost = lv_label_create(well);
    lv_label_set_text(ghost, "88:88");
    lv_obj_set_style_text_font(ghost, TF(CLOCK), 0);
    lv_obj_set_style_text_color(ghost, TC(DECOR_1), 0);
    lv_obj_center(ghost);
    lv_obj_set_parent(p->clock, well);
    lv_obj_set_width(p->clock, LV_SIZE_CONTENT);
    lv_obj_set_style_text_color(p->clock, TC(ACCENT_PRIMARY), 0);
    lv_obj_set_style_text_align(p->clock, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align_to(p->clock, ghost, LV_ALIGN_RIGHT_MID, 0, 0);   /* digits line up with their unlit segments */
    g_hifi_clock = p->clock; g_hifi_ghost = ghost;
    g_hifi_mer = mer_label(well, TF(UI_12), TC(ACCENT_PRIMARY));   /* 12-hour: AM / PM under the minutes */
    hifi_clock(lv_label_get_text(p->clock));   /* lays out the digits + AM/PM group */
    lv_obj_set_pos(p->date, 0, 162);
    lv_obj_set_style_text_letter_space(p->date, 4, 0);
    lv_obj_set_style_text_color(p->date, TC(TEXT_SECONDARY), 0);
    lv_obj_set_pos(p->weather, 0, 186);
    /* Library + Search side by side on plates */
    /* Library + Search share the now-playing plate's edges (60..300) */
    lv_obj_set_pos(p->library, 60, 220); lv_obj_set_size(p->library, 184, 40);
    plate(p->library, 6);
    home_library_fit(p->library);
    lv_obj_set_pos(p->search, 252, 220); lv_obj_set_size(p->search, 48, 40);
    plate(p->search, 6);
    /* now playing: a plate with an amber indicator where the art would be */
    lv_obj_set_pos(p->np, 60, 268); lv_obj_set_size(p->np, 240, 46);
    plate(p->np, 8);
    lv_obj_set_style_bg_opa(p->np, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(p->np, LV_OPA_COVER, 0);
    lv_obj_set_pos(p->np_thumb, 10, 15); lv_obj_set_size(p->np_thumb, 16, 16);
    home_set_art_enabled(0);                          /* an indicator, not art */
    lv_obj_add_flag(p->np_glyph, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(p->np_title, 36, 7);  lv_obj_set_width(p->np_title, 150);
    lv_obj_set_pos(p->np_artist, 36, 27); lv_obj_set_width(p->np_artist, 150);
    lv_obj_set_pos(p->pp, 190, 1); lv_obj_set_size(p->pp, 44, 44);
    lv_obj_set_style_text_color(p->pp_label, TC(ACCENT_PRIMARY), 0);
}
static void hifi_np(const np_parts_t *p){
    own_backgrounds();
    /* The real ring remains the seek geometry; its fill is drawn by the bezel ticks. */
    lv_obj_set_style_arc_width(p->ring, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(p->ring, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(p->ring, TC(BORDER_STRONG), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(p->ring, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(p->ring, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(p->ring, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(p->ring, 3, LV_PART_KNOB);
    lv_obj_set_style_radius(p->cover, 6, 0);
    lv_obj_set_style_border_width(p->cover, 1, 0);
    lv_obj_set_style_border_color(p->cover, TC(BORDER_STRONG), 0);
    lv_obj_set_style_border_opa(p->cover, LV_OPA_COVER, 0);
    lv_obj_t *t[2] = { p->elapsed, p->remain };
    for(int i = 0; i < 2; i++){
        lv_obj_set_style_text_font(t[i], &diskos_hifi_seg_20, 0);
        lv_obj_set_style_text_color(t[i], TC(ACCENT_PRIMARY), 0);
        lv_obj_align(t[i], LV_ALIGN_TOP_MID, i ? 61 : -61, 310);   /* up and in: clear of the rim and of the plate */
    }
    lv_obj_add_flag(p->sep, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(p->pp, 50, 50);
    lv_obj_set_style_radius(p->pp, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(p->pp, TC(SURFACE_RAISED), 0);
    lv_obj_set_style_bg_opa(p->pp, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p->pp, 1, 0);
    lv_obj_set_style_border_color(p->pp, TC(BORDER_STRONG), 0);
    lv_obj_set_style_text_align(p->pp, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(p->pp, 9, 0);
    lv_obj_align(p->pp, LV_ALIGN_TOP_MID, 0, 262);
    lv_obj_align(p->prev, LV_ALIGN_TOP_MID, -66, 272);
    lv_obj_align(p->next, LV_ALIGN_TOP_MID, 66, 272);
    lv_obj_set_ext_click_area(p->pp, 4);                     /* the plate is the target; no reach into prev/next */
    np_text_rows(p, 193, 221, 240);                          /* the album line clears the plate at 262 */
    np_sides_on_cover(p, 0);
}
static void hifi_qs_tile(lv_obj_t *c, lv_obj_t *g, lv_obj_t *cap, int on, int toggle){
    (void)toggle;
    /* a square key on the faceplate; a lit key glows amber through its legend */
    lv_obj_set_style_radius(c, 12, 0);
    lv_obj_set_style_bg_color(c, TC(SURFACE_RAISED), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, on ? TC(ACCENT_PRIMARY) : TC(BORDER_STRONG), 0);
    lv_obj_set_style_border_opa(c, LV_OPA_COVER, 0);
    if(g) lv_obj_set_style_text_color(g, on ? TC(ACCENT_PRIMARY) : TC(TEXT_PRIMARY), 0);
    if(cap){ lv_obj_set_style_text_color(cap, on ? TC(ACCENT_PRIMARY) : TC(TEXT_MUTED), 0);
             lv_obj_set_style_text_letter_space(cap, 1, 0); }
}
static void hifi_saver(const saver_parts_t *p){
    if(p->clock) lv_obj_set_style_text_color(p->clock, TC(ACCENT_PRIMARY), 0);
    if(p->date){ lv_obj_set_style_text_letter_space(p->date, 3, 0); lv_obj_set_style_text_color(p->date, TC(TEXT_SECONDARY), 0); }
    if(p->track && p->artist){
        lv_obj_t *well = lv_obj_get_parent(p->track);
        if(well == p->root){
            well = lv_obj_create(p->root);
            lv_obj_remove_style_all(well);
            kit_keep(well);
            lv_obj_clear_flag(well, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_size(well, 204, 49);
            lv_obj_set_style_radius(well, 6, 0);
            lv_obj_set_style_bg_color(well, TC(SURFACE), 0);
            lv_obj_set_style_bg_opa(well, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(well, 1, 0);
            lv_obj_set_style_border_color(well, TC(BORDER_STRONG), 0);
            lv_obj_set_style_border_opa(well, LV_OPA_COVER, 0);
            lv_obj_set_parent(p->track, well);
            lv_obj_set_parent(p->artist, well);
        }
        int well_y = saver_song_y(p->date, p->style == 4 ? 272 : 278);
        /* Keep the full text width; shorten the well at the lower arc instead. */
        int well_h = 49;
        while(well_h > 1){
            int dy = well_y + well_h - 180;
            if(102 * 102 + dy * dy <= 176 * 176) break;
            well_h--;
        }
        lv_obj_set_size(well, 204, well_h);
        lv_obj_align(well, LV_ALIGN_TOP_MID, 0, well_y);
        lv_obj_set_width(p->track, 192);
        lv_obj_set_width(p->artist, 192);
        lv_obj_set_style_text_font(p->track, TF(USER_16), 0);
        lv_obj_set_style_text_font(p->artist, TF(USER_14), 0);
        lv_obj_set_style_text_color(p->track, TC(ACCENT_PRIMARY), 0);
        lv_obj_set_style_text_color(p->artist, TC(TEXT_SECONDARY), 0);
        lv_obj_align(p->track, LV_ALIGN_TOP_MID, 0, well_h < 45 ? 3 : 5);
        lv_obj_align(p->artist, LV_ALIGN_TOP_MID, 0, well_h < 45 ? 24 : 26);
    }
}
static void hifi_empty(kit_empty_kind_t kind, lv_obj_t *message){
    (void)kind;
    if(!message) return;
    lv_obj_set_style_text_font(message, TF(USER_16), 0);
    lv_obj_set_style_text_color(message, TC(TEXT_MUTED), 0);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_translate_y(message, 0, 0);
    lv_obj_update_layout(message);
    lv_area_t a; lv_obj_get_coords(message, &a);
    lv_obj_set_style_translate_y(message, 180 - (a.y1 + a.y2) / 2, 0);
}
static void hifi_cover(lv_obj_t *cover, lv_obj_t *note, int show, int loading){
    if(!cover || !note) return;
    lv_obj_set_style_bg_color(cover, TC(SURFACE), 0);
    lv_obj_set_style_radius(cover, 2, 0);
    if(!show) return;
    lv_label_set_text(note, loading ? LV_SYMBOL_REFRESH : LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(note, TF(UI_32), 0);   /* LVGL's symbol fallback has the music note */
    lv_obj_set_style_text_color(note, TC(ACCENT_PRIMARY), 0);
    lv_obj_center(note);
}
/* AirPlay receiving: the bezel ticks (the texture) stay unlit, an amber tracked tag, a machined play plate. */
static void hifi_rx(const rx_parts_t *p, rx_state_t *s){
    if(s->first){
        lv_obj_set_style_arc_opa(p->arc, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_radius(p->frame, 6, 0);
        lv_obj_set_style_border_width(p->frame, 1, 0);
        lv_obj_set_style_border_color(p->frame, TC(BORDER_STRONG), 0);
        lv_obj_set_style_border_opa(p->frame, LV_OPA_COVER, 0);
        rx_label(p->text[0], TF(UI_12), TC(ACCENT_PRIMARY)); lv_obj_set_style_text_letter_space(p->text[0], 2, 0);
        rx_label(p->text[1], TF(USER_20), TC(TEXT_PRIMARY)); rx_label(p->text[2], TF(USER_16), TC(TEXT_SECONDARY));
        rx_label(p->text[3], TF(USER_14), TC(TEXT_MUTED));
        rx_label(p->text[4], TF(UI_12), TC(TEXT_MUTED)); lv_obj_set_style_text_letter_space(p->text[4], 2, 0);
        lv_obj_set_style_radius(p->key[1], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(p->key[1], TC(SURFACE_RAISED), 0);
        lv_obj_set_style_border_width(p->key[1], 1, 0);
        lv_obj_set_style_border_color(p->key[1], TC(BORDER_STRONG), 0);
        lv_obj_set_style_bg_opa(p->key[1], LV_OPA_COVER, 0);
        rx_label(p->elapsed, &diskos_hifi_seg_20, TC(ACCENT_PRIMARY)); rx_label(p->remain, &diskos_hifi_seg_20, TC(ACCENT_PRIMARY));
        lv_obj_set_style_text_align(p->elapsed, LV_TEXT_ALIGN_RIGHT, 0); lv_obj_set_style_text_align(p->remain, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_long_mode(p->elapsed, LV_LABEL_LONG_CLIP); lv_label_set_long_mode(p->remain, LV_LABEL_LONG_CLIP);   /* never wrap a long time */
        return;
    }
    int tm = s->show && s->mode == RX_RECEIVING;   /* the times in the segment face, no bar: "2:31   -1:32" under the keys */
    rx_show(p->bar, 0); rx_show(p->elapsed, tm); rx_show(p->remain, tm);
    if(s->mode == RX_OTHER) return;
    if(tm){ rx_place(p->elapsed, LV_ALIGN_TOP_LEFT, 50, 318, 120, NULL); rx_place(p->remain, LV_ALIGN_TOP_LEFT, 190, 318, 120, NULL); }
    if(s->mode == RX_PAUSED) theme_upper(s->text[4], sizeof s->text[4], s->text[4]);   /* PAUSED, as the display reads it */
    rx_place(p->text[0], LV_ALIGN_TOP_MID, 0, 20, 160, NULL);
    rx_rows(p, s, (const int[8]){ 193, 221, 240, 322, 84, 150, 184, 210 }, (const lv_font_t *const[3]){ TF(USER_20), TF(USER_16), TF(USER_20) });
    rx_transport(p, 262, 50, 66);
}
const theme_kit_t theme_kit_hifi = { .id = "hi-fi", .header = kit_edge_header, .style = hifi_style, .home = hifi_home, .receiver = hifi_rx,
                                     .home_clock = hifi_clock,
                                     .nowplaying = hifi_np, .qs_tile = hifi_qs_tile, .saver = hifi_saver,
                                     .empty_state = hifi_empty, .np_cover_state = hifi_cover,
                                     .saver_mer_apart = 1 };   /* the segment face draws "PM" as "PN" */

/* ================================================================ terminal
 * A command line: one monospace face, square corners, ~/path titles, rows that highlight with a cursor bar, bracketed
 * keys, [x] checkboxes in Quick Settings, and a Now Playing that is a readout with a straight seek bar (no ring). */
/* a plain surface fill (not an accent, brand or status colour): themes may redraw it their own way */
static int is_neutral_fill(lv_obj_t *o){
    lv_color_t c = lv_obj_get_style_bg_color(o, 0);
    static const theme_color_role_t N[] = { THEME_CLR_SURFACE, THEME_CLR_SURFACE_RAISED, THEME_CLR_SURFACE_STRONG,
        THEME_CLR_CONTROL_OFF, THEME_CLR_KEY_SURFACE, THEME_CLR_SURFACE_SELECTED, THEME_CLR_LIST_PRESSED, THEME_CLR_CANVAS };
    for(unsigned i = 0; i < sizeof N / sizeof N[0]; i++) if(lv_color_eq(c, theme_color(N[i]))) return 1;
    return 0;
}
static void term_square(lv_obj_t *o){ lv_obj_set_style_radius(o, 0, 0); }
static const char term_cursor_tag;
static lv_timer_t *term_cursor_timer;
static lv_obj_t *term_cursor_active;
static lv_obj_t *term_cursor_for(lv_obj_t *root){
    if(!root) return NULL;
    for(uint32_t i = 0; i < lv_obj_get_child_count(root); i++){
        lv_obj_t *c = lv_obj_get_child(root, (int32_t)i);
        if(lv_obj_get_user_data(c) == (void *)&term_cursor_tag) return c;
    }
    return NULL;
}
static void term_cursor_place(lv_obj_t *title){
    lv_obj_t *cur = term_cursor_for(lv_obj_get_parent(title));
    if(!cur) return;
    const lv_font_t *f = lv_obj_get_style_text_font(title, 0);
    const char *text = lv_label_get_text(title), *last = strrchr(text, '\n');
    int lines = 0; for(const char *p = text; *p; p++) if(*p == '\n') lines++;
    lv_point_t sz;
    if(!indic_label_cached_size(title, &sz, 1)) lv_text_get_size(&sz, last ? last + 1 : text, f, lv_obj_get_style_text_letter_space(title, 0),
                     0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int room = lv_obj_get_content_width(title);
    if(sz.x > room) sz.x = room;
    int x = lv_obj_get_x(title) + (room + sz.x) / 2 + 3;
    if(x > 318) x = 318;
    int lh = indic_line_height_cached(title);
    lv_obj_set_pos(cur, x, lv_obj_get_y(title) + lv_obj_get_style_pad_top(title, 0) +
                   lines * (lh + lv_obj_get_style_text_line_space(title, 0)) + (lh - 16) / 2);
}
static void term_cursor_resize(lv_event_t *e){ term_cursor_place(lv_event_get_target(e)); }
static void term_cursor_tick(lv_timer_t *timer){
    (void)timer;
    lv_obj_t *root = screen_get_root(screen_current());
    if(!ui_screen_backlight_on() || !term_cursor_active || !root ||
       lv_obj_get_parent(term_cursor_active) != root || lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)){
        lv_timer_pause(term_cursor_timer); term_cursor_active = NULL; return;
    }
    if(lv_obj_has_flag(term_cursor_active, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(term_cursor_active, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(term_cursor_active, LV_OBJ_FLAG_HIDDEN);
}
void kit_cursor_sync(int backlight_on){
    if(strcmp(theme_kit()->id, "terminal")) return;
    lv_obj_t *cur = backlight_on ? term_cursor_for(screen_get_root(screen_current())) : NULL;
    if(!cur){ if(term_cursor_timer) lv_timer_pause(term_cursor_timer); term_cursor_active = NULL; return; }
    if(cur == term_cursor_active && term_cursor_timer && !lv_timer_get_paused(term_cursor_timer)) return;
    term_cursor_active = cur;
    lv_obj_remove_flag(cur, LV_OBJ_FLAG_HIDDEN); /* every entry/wake starts ON */
    if(!term_cursor_timer) term_cursor_timer = lv_timer_create(term_cursor_tick, 550, NULL);
    lv_timer_reset(term_cursor_timer);
    lv_timer_resume(term_cursor_timer);
}
#ifdef DISKOS_HOST
int kit_cursor_host_paused(void){ return !term_cursor_timer || lv_timer_get_paused(term_cursor_timer); }
int kit_cursor_host_on(void){ return term_cursor_active && !lv_obj_has_flag(term_cursor_active, LV_OBJ_FLAG_HIDDEN); }
void kit_cursor_host_phase(int on){
    if(!term_cursor_active) return;
    if(on) lv_obj_remove_flag(term_cursor_active, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(term_cursor_active, LV_OBJ_FLAG_HIDDEN);
    if(term_cursor_timer) lv_timer_reset(term_cursor_timer);
}
#endif
static void term_title_cursor(lv_obj_t *title){
    lv_obj_t *root = lv_obj_get_parent(title);
    if(!root || term_cursor_for(root)) return;
    lv_obj_t *cur = lv_obj_create(root);
    lv_obj_remove_style_all(cur); kit_keep(cur);
    lv_obj_set_user_data(cur, (void *)&term_cursor_tag);
    lv_obj_clear_flag(cur, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(cur, 8, 16);
    lv_obj_set_style_bg_color(cur, TC(ACCENT_PRIMARY), 0);
    lv_obj_set_style_bg_opa(cur, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(title, term_cursor_resize, LV_EVENT_SIZE_CHANGED, NULL);
    term_cursor_place(title);
}
static void term_row_value(lv_obj_t *row, lv_obj_t *title, lv_obj_t *sub, lv_obj_t *value, kit_value_kind_t kind){
    (void)row;
    if(kind != KIT_VALUE_DURATION) return;
    lv_obj_set_pos(value, 198, 17); lv_obj_set_size(value, 58, 18);
    lv_obj_set_style_text_font(value, TF(UI_14), 0);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(value, TC(TEXT_SECONDARY), 0);
    lv_obj_set_width(title, 178); /* reserve a fixed column before long text is dotted */
    if(sub) lv_obj_set_width(sub, 178);
}
/* a switch drawn as "[x]" / "[ ]" at draw time, so every state change shows (the user's and the code's) */
static void term_switch_draw(lv_event_t *e){
    lv_obj_t *o = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d);
    d.font = TF(UI_16);
    d.color = lv_obj_has_state(o, LV_STATE_CHECKED) ? TC(ACCENT_PRIMARY) : TC(TEXT_SECONDARY);
    d.text = lv_obj_has_state(o, LV_STATE_CHECKED) ? "[x]" : "[ ]";
    d.align = LV_TEXT_ALIGN_RIGHT;
    lv_area_t a; lv_obj_get_coords(o, &a);
    int lh = lv_font_get_line_height(d.font);
    a.y1 = (a.y1 + a.y2) / 2 - lh / 2; a.y2 = a.y1 + lh;
    lv_draw_label(layer, &d, &a);
}
static void term_style(lv_obj_t *o, kit_role_t r){
    if(lv_color_eq(lv_obj_get_style_text_color(o, 0), TC(STATUS_INFO)))
        lv_obj_set_style_text_color(o, TC(ACCENT_PRIMARY), 0);
    switch(r){
    case KIT_TITLE:
        term_title_cursor(o);
        break;
    case KIT_ROW:
        lv_obj_set_style_radius(o, 0, 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(o, 0, 0);
        lv_obj_set_style_bg_color(o, TC(SURFACE_SELECTED), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_border_side(o, LV_BORDER_SIDE_LEFT, LV_STATE_PRESSED);   /* the cursor bar */
        lv_obj_set_style_border_width(o, 4, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(o, TC(ACCENT_PRIMARY), LV_STATE_PRESSED);
        break;
    case KIT_ROW_CHEVRON:
        lv_label_set_text(o, "/");
        lv_obj_set_style_text_font(o, TF(UI_16), 0);
        lv_obj_set_style_text_color(o, TC(TEXT_MUTED), 0);
        break;
    case KIT_BUTTON: case KIT_ICON_BUTTON:
        term_square(o);
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10 && is_neutral_fill(o)){
            lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);      /* a key is its bracket box, not a fill */
            lv_obj_set_style_border_width(o, 1, 0);
            lv_obj_set_style_border_color(o, TC(TEXT_SECONDARY), 0);
            lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
        }
        lv_obj_set_style_bg_color(o, TC(SURFACE_SELECTED), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_STATE_PRESSED);
        break;
    case KIT_CARD:                                       /* list cards are flat lines of text, like the rows */
        term_square(o);
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10 && is_neutral_fill(o)){
            lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(o, 0, 0);
        }
        break;
    case KIT_POPUP:                                      /* a popup is a framed window */
        term_square(o);
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10){
            lv_obj_set_style_bg_color(o, TC(CANVAS), 0);
            lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);   /* a see-through window let the screen's text through a toast */
            lv_obj_set_style_border_width(o, 1, 0);
            lv_obj_set_style_border_color(o, TC(ACCENT_PRIMARY), 0);
            lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
        }
        break;
    case KIT_TOGGLE:
        for(int p = 0; p < 3; p++){
            lv_part_t part = p == 0 ? LV_PART_MAIN : p == 1 ? LV_PART_INDICATOR : LV_PART_KNOB;
            lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, part);
            lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, part | LV_STATE_CHECKED);
            lv_obj_set_style_border_width(o, 0, part);
            lv_obj_set_style_shadow_width(o, 0, part);
        }
        lv_obj_add_event_cb(o, term_switch_draw, LV_EVENT_DRAW_MAIN_END, NULL);
        break;
    case KIT_SLIDER: case KIT_BAR:
        for(int p = 0; p < 3; p++){
            lv_part_t part = p == 0 ? LV_PART_MAIN : p == 1 ? LV_PART_INDICATOR : LV_PART_KNOB;
            lv_obj_set_style_radius(o, 0, part);
        }
        break;
    case KIT_ROLLER: case KIT_TEXTAREA:
        term_square(o);
        lv_obj_set_style_radius(o, 0, LV_PART_SELECTED);
        break;
    case KIT_KEYBOARD:
        lv_obj_set_style_radius(o, 0, LV_PART_ITEMS);
        lv_obj_set_style_border_width(o, 1, LV_PART_ITEMS);
        lv_obj_set_style_border_color(o, TC(BORDER), LV_PART_ITEMS);
        lv_obj_set_style_border_opa(o, LV_OPA_COVER, LV_PART_ITEMS);
        break;
    case KIT_TEXT: case KIT_ROW_SUB:
        if(is_large_default_text(o)) lv_obj_set_style_text_font(o, TF(UI_24), 0);
        if(!strcmp(lv_label_get_text(o), LV_SYMBOL_OK)){                 /* a tick is a star, as a shell marks it */
            lv_label_set_text(o, "*");
            lv_obj_set_style_text_font(o, TF(UI_20), 0);
        }
        break;
    default: break;
    }
}
/* the title as a working directory ("~/library"), Back as a "<" key on the left edge */
static lv_obj_t *term_header(lv_obj_t *root, const char *title, lv_event_cb_t back_cb){
    lv_obj_t *t = kit_edge_header(root, title, back_cb);
    lv_obj_set_style_text_color(t, TC(TEXT_SECONDARY), 0);
    return t;
}
static void term_title(lv_obj_t *label, const char *text){   /* "~/library" (already lower case) */
    char path[160]; int n = lv_snprintf(path, sizeof path, "~/%s", text);
    for(int i = 2; i < n && i < (int)sizeof path; i++) if(path[i] == ' ') path[i] = '_';
    lv_label_set_text(label, path);
    term_cursor_place(label);
}
static lv_obj_t *term_text(lv_obj_t *parent, const char *txt, lv_color_t c, int x, int y){
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, TF(UI_14), 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}
static void term_key(lv_obj_t *o){            /* a bracketed key: square box, 1 px border */
    kit_keep(o);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, TC(TEXT_SECONDARY), 0);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, TC(SURFACE_SELECTED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_STATE_PRESSED);
}
static lv_obj_t *g_term_clock, *g_term_cur, *g_term_mer;
static void term_clock(const char *t){
    if(!g_term_clock) return;
    char hm[16], mer[8]; clock_parts(t, hm, sizeof hm, mer, sizeof mer);
    lv_label_set_text(g_term_clock, hm);
    char low[8]; lv_label_set_text(g_term_mer, theme_lower(low, sizeof low, mer));
    lv_point_t tw; lv_text_get_size(&tw, hm, TF(CLOCK), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_obj_align(g_term_cur, LV_ALIGN_TOP_MID, tw.x / 2 + 14, 84);             /* the cursor right after the time */
    lv_obj_align_to(g_term_mer, g_term_cur, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, 0);
}
static void term_home(const home_parts_t *p){
    own_backgrounds();
    lv_obj_t *banner = term_text(p->root, "diskOS 1.2 | tty0", TC(TEXT_MUTED), 0, 30);
    lv_obj_set_width(banner, 360); lv_obj_set_style_text_align(banner, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(p->status, 0, 50);
    lv_obj_set_style_text_color(p->status, TC(TEXT_MUTED), 0);
    lv_obj_set_pos(p->clock, 0, 70);
    lv_obj_t *cur = lv_obj_create(p->root);                  /* the block cursor after the time */
    lv_obj_remove_style_all(cur);
    lv_obj_clear_flag(cur, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(cur, 12, 52);
    g_term_clock = p->clock; g_term_cur = cur;
    g_term_mer = mer_label(p->root, TF(UI_16), TC(TEXT_SECONDARY));
    term_clock(lv_label_get_text(p->clock));
    lv_obj_set_style_bg_color(cur, TC(ACCENT_PRIMARY), 0);
    lv_obj_set_style_bg_opa(cur, LV_OPA_COVER, 0);
    term_text(p->root, "$ date", TC(TEXT_MUTED), 62, 148);
    lv_obj_set_style_text_align(p->date, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(p->date, 62, 164); lv_obj_set_width(p->date, 240);
    lv_obj_set_style_text_color(p->date, TC(TEXT_PRIMARY), 0);
    lv_obj_set_style_text_align(p->weather, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(p->weather, 62, 182); lv_obj_set_width(p->weather, 240);
    term_text(p->root, "$ now", TC(TEXT_MUTED), 62, 208);
    /* $ now: the now-playing line, with its play key. Everything hangs from x 62 and ends at 298 (mirrored) */
    lv_obj_set_pos(p->np, 54, 224); lv_obj_set_size(p->np, 252, 44);
    kit_keep(p->np);
    lv_obj_set_style_radius(p->np, 0, 0);
    lv_obj_set_style_bg_opa(p->np, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p->np, 0, 0);
    lv_obj_set_style_bg_color(p->np, TC(SURFACE_SELECTED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(p->np, LV_OPA_COVER, LV_STATE_PRESSED);
    home_set_art_enabled(0);
    lv_obj_set_pos(p->np_thumb, 8, 5); lv_obj_set_size(p->np_thumb, 18, 20);   /* holds the 19 px '>' line */
    lv_obj_set_style_radius(p->np_thumb, 0, 0);
    lv_obj_set_style_bg_opa(p->np_thumb, LV_OPA_TRANSP, 0);
    lv_label_set_text(p->np_glyph, ">");
    lv_obj_set_style_text_font(p->np_glyph, theme_font_size() == 2 ? TF(UI_14) : TF(UI_16), 0);
    lv_obj_set_style_text_color(p->np_glyph, TC(ACCENT_PRIMARY), 0);
    lv_obj_center(p->np_glyph);
    lv_obj_set_pos(p->np_title, 30, 4);  lv_obj_set_width(p->np_title, 164);
    lv_obj_set_pos(p->np_artist, 30, 24); lv_obj_set_width(p->np_artist, 164);
    lv_obj_set_pos(p->pp, 200, 4); lv_obj_set_size(p->pp, 44, 38);
    term_key(p->pp);
    /* [ library ] [ find ] */
    lv_obj_set_pos(p->library, 62, 276); lv_obj_set_size(p->library, 136, 38);
    term_key(p->library);
    lv_obj_t *ll = NULL; for(uint32_t i = 0; i < lv_obj_get_child_count(p->library); i++){
        lv_obj_t *c = lv_obj_get_child(p->library, (int32_t)i);
        if(lv_obj_check_type(c, &lv_label_class)){ if(!ll && strlen(lv_label_get_text(c)) == 3) lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN); else ll = c; } }
    if(ll){ lv_label_set_text(ll, "library"); lv_obj_center(ll); }
    lv_obj_set_pos(p->search, 208, 276); lv_obj_set_size(p->search, 90, 38);
    term_key(p->search);
    for(uint32_t i = 0; i < lv_obj_get_child_count(p->search); i++) lv_obj_add_flag(lv_obj_get_child(p->search, (int32_t)i), LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *fl = lv_label_create(p->search);
    lv_label_set_text(fl, "find");
    lv_obj_set_style_text_font(fl, TF(UI_16), 0);
    lv_obj_set_style_text_color(fl, TC(TEXT_PRIMARY), 0);
    lv_obj_center(fl);
    lv_label_set_text(p->hint, ">");
    lv_obj_set_style_text_font(p->hint, TF(UI_14), 0);
}
static lv_obj_t *g_term_state;   /* the "> playing" readout line: follows the real play state */
static void term_np_state(int playing, int have_track){
    if(!g_term_state) return;
    const char *t = !have_track ? "> no track" : playing ? "> playing" : "> paused";
    if(strcmp(lv_label_get_text(g_term_state), t)) lv_label_set_text(g_term_state, t);
}
static void term_np(const np_parts_t *p){
    own_backgrounds();
    /* a readout: no ring, no art; the seek is a straight bar */
    lv_obj_add_flag(p->ring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->cover, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->backdrop, LV_OBJ_FLAG_HIDDEN);
    g_term_state = term_text(p->root, "> playing", TC(TEXT_MUTED), 60, 52);   /* set from the play state (np_state) */
    static const char *const K[3] = { "title", "artist", "album" };
    lv_obj_t *v[3] = { p->title, p->artist, p->album };
    for(int i = 0; i < 3; i++){
        term_text(p->root, K[i], TC(TEXT_MUTED), 60, 80 + i * 24);
        lv_obj_set_style_text_align(v[i], LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_width(v[i], 176);
        /* each value sits on its key's baseline (the faces differ in height) */
        int kh = lv_font_get_line_height(TF(UI_14)), vh = lv_font_get_line_height(i ? TF(USER_14) : TF(USER_16));
        lv_obj_align(v[i], LV_ALIGN_TOP_LEFT, 124, 80 + i * 24 + kh - vh);
        lv_obj_set_style_text_font(v[i], i ? TF(USER_14) : TF(USER_16), 0);
        lv_obj_set_style_text_color(v[i], i ? TC(TEXT_SECONDARY) : TC(TEXT_PRIMARY), 0);
    }
    lv_obj_t *bar = lv_bar_create(p->root);
    lv_obj_set_size(bar, 240, 14);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 164);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, TC(SURFACE_SELECTED), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, TC(TEXT_SECONDARY), LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, TC(ACCENT_PRIMARY), LV_PART_INDICATOR);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);             /* seek is driven by the NP touch recognizer */
    ui_np_seek_line(bar);
    lv_obj_align(p->elapsed, LV_ALIGN_TOP_LEFT, 60, 184);
    lv_obj_set_style_text_align(p->elapsed, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(p->remain, LV_ALIGN_TOP_RIGHT, -60, 184);
    lv_obj_set_style_text_align(p->remain, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_flag(p->sep, LV_OBJ_FLAG_HIDDEN);
    /* [<<] [||] [>>] */
    lv_obj_t *tr[3] = { p->prev, p->pp, p->next };
    for(int i = 0; i < 3; i++){
        lv_obj_set_style_text_font(tr[i], TF(UI_20), 0);
        lv_obj_set_style_text_color(tr[i], TC(TEXT_PRIMARY), 0);
        lv_obj_set_style_bg_opa(tr[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(tr[i], 0, 0);
        lv_obj_set_style_border_width(tr[i], 1, 0);
        lv_obj_set_style_border_color(tr[i], i == 1 ? TC(ACCENT_PRIMARY) : TC(TEXT_SECONDARY), 0);
        lv_obj_set_style_border_opa(tr[i], LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(tr[i], 14, 0);
        lv_obj_set_style_pad_ver(tr[i], 8, 0);
        lv_obj_set_size(tr[i], 56, LV_SIZE_CONTENT);          /* equal keys: equal gaps, a symmetric row */
        lv_obj_set_style_text_align(tr[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_hor(tr[i], 0, 0);
        lv_obj_align(tr[i], LV_ALIGN_TOP_MID, (i - 1) * 70, 216);
        lv_obj_set_ext_click_area(tr[i], 4);                 /* the key box is the target; keys never share a touch */
    }
    lv_obj_set_style_text_color(p->pp, TC(ACCENT_PRIMARY), 0);
    /* mode / fav / sleep as a row of keys */
    lv_obj_t *k[4] = { p->mode, p->fav, p->sleep, p->beam };
    for(int i = 0; i < 4; i++){ lv_obj_set_size(k[i], 50, 40); term_key(k[i]); lv_obj_set_ext_click_area(k[i], 4); }
    /* the moon replaces the heart for audiobooks (ui.c): they share the middle slot; Beam is the third key */
    lv_obj_align(p->mode, LV_ALIGN_TOP_MID, -64, 272);
    lv_obj_align(p->fav, LV_ALIGN_TOP_MID, 0, 272);
    lv_obj_align(p->sleep, LV_ALIGN_TOP_MID, 0, 272);
    lv_obj_align(p->beam, LV_ALIGN_TOP_MID, 64, 272);
}
static void term_qs(const qs_parts_t *p){
    lv_obj_t *prompt = term_text(p->root, "$ quick", TC(TEXT_SECONDARY), 0, 26);
    lv_obj_set_width(prompt, 360); lv_obj_set_style_text_align(prompt, LV_TEXT_ALIGN_CENTER, 0);
    if(p->grab) lv_obj_add_flag(p->grab, LV_OBJ_FLAG_HIDDEN);       /* the prompt is the handle */
    /* the tiles become a checklist: "[x] wi-fi", one per line */
    lv_obj_update_layout(p->root);
    int y = p->bright ? lv_obj_get_y(p->bright) + 44 : 90;
    if(p->pp) y = lv_obj_get_y(p->pp) + 60;
    for(int i = 0; i < p->n; i++){
        lv_obj_set_size(p->tile[i], 210, 30);
        lv_obj_align(p->tile[i], LV_ALIGN_TOP_MID, 0, y + i * 32);
        lv_obj_t *g = lv_obj_get_child(p->tile[i], 0);
        if(g) lv_obj_align(g, LV_ALIGN_LEFT_MID, 8, 0);
        if(p->cap[i]){
            lv_obj_set_parent(p->cap[i], p->tile[i]);
            lv_obj_align(p->cap[i], LV_ALIGN_LEFT_MID, 56, 0);
            lv_obj_set_style_text_font(p->cap[i], TF(UI_16), 0);
        }
    }
    if(p->bright){ lv_obj_set_style_radius(p->bright, 0, 0); lv_obj_set_style_radius(p->bright, 0, LV_PART_INDICATOR); }
}
static void term_qs_tile(lv_obj_t *c, lv_obj_t *g, lv_obj_t *cap, int on, int toggle){
    lv_obj_set_style_radius(c, 0, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_bg_color(c, TC(SURFACE_SELECTED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_STATE_PRESSED);
    if(g){
        lv_label_set_text(g, toggle ? (on ? "[x]" : "[ ]") : " > ");
        lv_obj_set_style_text_font(g, TF(UI_16), 0);
        lv_obj_set_style_text_color(g, on ? TC(ACCENT_PRIMARY) : TC(TEXT_SECONDARY), 0);
    }
    if(cap){
        char low[32]; lv_label_set_text(cap, theme_lower(low, sizeof low, lv_label_get_text(cap)));
        lv_obj_set_style_text_color(cap, on ? TC(ACCENT_PRIMARY) : TC(TEXT_PRIMARY), 0);
    }
}
static void term_saver(const saver_parts_t *p){
    if(p->date) lv_obj_set_style_text_color(p->date, TC(TEXT_SECONDARY), 0);
    if(p->track && p->artist){
        int y = saver_song_y(p->date, p->style == 4 ? 274 : 280);
        lv_obj_set_width(p->track, 230); lv_obj_set_width(p->artist, 230);
        lv_obj_set_style_text_font(p->track, TF(USER_16), 0);
        lv_obj_set_style_text_font(p->artist, TF(USER_14), 0);
        lv_obj_set_style_text_color(p->track, TC(ACCENT_PRIMARY), 0);
        lv_obj_set_style_text_color(p->artist, TC(TEXT_MUTED), 0);
        lv_obj_set_style_text_align(p->track, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_align(p->artist, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(p->track, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_align(p->artist, LV_ALIGN_TOP_MID, 0, y + 22);
        int width = saver_song_width(p->artist, 230);
        lv_obj_set_width(p->track, width); lv_obj_set_width(p->artist, width);
    }
}
#define TERM_PROMPT LV_OBJ_FLAG_USER_1
static void term_prompt_deleted(lv_event_t *e){ free(lv_obj_get_user_data(lv_event_get_target(e))); }
static void term_prompt_draw(lv_event_t *e){
    lv_obj_t *message = lv_event_get_target(e);
    const char *saved = lv_obj_get_user_data(message);
    if(!saved || strcmp(saved, lv_label_get_text(message))) return;
    lv_area_t a; lv_obj_get_coords(message, &a);
    lv_area_t mark = { a.x1 + 2, a.y1, a.x1 + 30, a.y2 };
    lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d);
    d.text = "$ ";
    d.font = TF(UI_16);
    d.color = TC(ACCENT_PRIMARY);
    lv_draw_label(lv_event_get_layer(e), &d, &mark);
}
static void term_empty(kit_empty_kind_t kind, lv_obj_t *message){
    (void)kind;
    if(!message) return;
    lv_obj_set_style_text_font(message, TF(UI_16), 0);
    lv_obj_set_style_text_color(message, TC(TEXT_MUTED), 0);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(message, 272);
    lv_obj_set_style_pad_left(message, 24, 0);
    lv_obj_set_style_translate_x(message, 0, 0);
    lv_obj_set_style_translate_y(message, 0, 0);
    lv_obj_update_layout(message);
    lv_area_t a; lv_obj_get_coords(message, &a);
    lv_obj_set_style_translate_x(message, 38 - a.x1, 0);
    lv_obj_set_style_translate_y(message, 174 - a.y1, 0);
    /* Draw only for this empty message; a reused Lyrics label sheds the mark when its text changes. */
    if(lv_obj_has_flag(message, TERM_PROMPT)) free(lv_obj_get_user_data(message));
    lv_obj_set_user_data(message, strdup(lv_label_get_text(message)));
    if(!lv_obj_has_flag(message, TERM_PROMPT)){
        lv_obj_add_flag(message, TERM_PROMPT);
        lv_obj_add_event_cb(message, term_prompt_draw, LV_EVENT_DRAW_MAIN_END, NULL);
        lv_obj_add_event_cb(message, term_prompt_deleted, LV_EVENT_DELETE, NULL);
    }
}
/* AirPlay receiving: the readout. "> airplay" is the source tag, key / value rows, a flat empty bar, boxed keys. */
static lv_obj_t *g_rx_term_key[4], *g_rx_term_bar;
static int g_rx_term_mode;
static void term_rx(const rx_parts_t *p, rx_state_t *s){
    if(s->first){
        g_rx_term_mode = -1;
        lv_obj_set_style_arc_opa(p->arc, LV_OPA_TRANSP, LV_PART_MAIN);
        static const char *const K[4] = { "title", "artist", "album", "name" };
        for(int i = 0; i < 4; i++) g_rx_term_key[i] = term_text(p->root, K[i], TC(TEXT_MUTED), 60, 0);
        g_rx_term_bar = lv_bar_create(p->root);
        lv_obj_set_size(g_rx_term_bar, 240, 14); lv_obj_align(g_rx_term_bar, LV_ALIGN_TOP_MID, 0, 164);
        lv_obj_set_style_radius(g_rx_term_bar, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(g_rx_term_bar, TC(SURFACE_SELECTED), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(g_rx_term_bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(g_rx_term_bar, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(g_rx_term_bar, TC(TEXT_SECONDARY), LV_PART_MAIN);
        lv_obj_clear_flag(g_rx_term_bar, LV_OBJ_FLAG_CLICKABLE);
        rx_times_style(p, TF(UI_14), TC(TEXT_SECONDARY)); lv_obj_move_foreground(p->bar);   /* the fill sits inside the track's border */
        rx_label(p->text[0], TF(UI_14), TC(TEXT_MUTED));
        rx_label(p->text[1], TF(USER_16), TC(TEXT_PRIMARY)); rx_label(p->text[2], TF(USER_14), TC(TEXT_SECONDARY));
        rx_label(p->text[3], TF(UI_12), TC(TEXT_SECONDARY)); rx_label(p->text[4], TF(UI_14), TC(TEXT_MUTED));
        for(int i = 0; i < 5; i++) lv_obj_set_style_text_align(p->text[i], LV_TEXT_ALIGN_LEFT, 0);
        for(int i = 0; i < 3; i++){
            term_key(p->key[i]);
            lv_obj_set_style_border_color(p->key[i], i == 1 ? TC(ACCENT_PRIMARY) : TC(TEXT_SECONDARY), 0);
            lv_obj_set_style_text_color(p->label[i], i == 1 ? TC(ACCENT_PRIMARY) : TC(TEXT_PRIMARY), 0);
            lv_obj_set_style_text_font(p->label[i], TF(UI_20), 0);
        }
        return;
    }
    int rx = s->mode == RX_RECEIVING || s->mode == RX_PAUSED, wait = s->mode == RX_WAITING;
    rx_show(g_rx_term_bar, rx);
    if(!rx || s->mode == RX_OTHER){ rx_show(p->bar, 0); rx_show(p->elapsed, 0); rx_show(p->remain, 0); }
    for(int i = 0; i < 4; i++) rx_show(g_rx_term_key[i], i < 3 ? rx && (!i || s->text[i + 1][0]) : wait);
    if(s->mode == RX_OTHER){   /* Default's centred placement again, once, for the states the readout does not own */
        if(g_rx_term_mode){
            for(int i = 0; i < 5; i++){
                rx_place(p->text[i], LV_ALIGN_TOP_MID, 0, lv_obj_get_y(p->text[i]), i == 3 ? 230 : 280, NULL);
                lv_obj_set_style_text_align(p->text[i], LV_TEXT_ALIGN_CENTER, 0);
            }
            g_rx_term_mode = 0;
        }
        return;
    }
    g_rx_term_mode = 1;
    for(int i = 0; i < 5; i++){
        if(lv_obj_get_style_text_align(p->text[i], 0) != LV_TEXT_ALIGN_LEFT)
            lv_obj_set_style_text_align(p->text[i], LV_TEXT_ALIGN_LEFT, 0);
    }
    s->frame = s->icon = 0;                                   /* a readout: no art slot, no glyph */
    char tag[48], pz[48];
    theme_lower(tag, sizeof tag, s->text[0][0] ? s->text[0] : "AIRPLAY"); theme_lower(pz, sizeof pz, s->text[4]);
    lv_snprintf(s->text[0], sizeof s->text[0], s->mode == RX_PAUSED ? "> %s [%s]" : "> %s", tag, pz);
    lv_snprintf(s->text[4], sizeof s->text[4], "%s", pz);
    if(wait){ theme_lower(s->text[1], sizeof s->text[1], s->text[1]); strcat(s->text[1], "_"); }   /* heading + block cursor */
    int kh = lv_font_get_line_height(TF(UI_14));
    rx_place(p->text[0], LV_ALIGN_TOP_LEFT, 60, wait ? 70 : 50, 250, NULL);
    rx_place(p->text[4], LV_ALIGN_TOP_LEFT, 60, 184, 200, NULL);
    const lv_font_t *vf[3] = { wait ? TF(UI_20) : TF(USER_16), TF(USER_14), wait ? TF(UI_12) : TF(USER_14) };
    for(int i = 0; i < 3; i++){   /* rows: key + value on one baseline; waiting: heading, name row, hint */
        int x = wait && i != 1 ? 60 : 124, w = wait ? (i == 1 ? 176 : 230) : 176;
        int top = wait ? (i == 0 ? 104 : i == 1 ? 148 : 176) : 80 + 24 * i;
        rx_place(p->text[i + 1], LV_ALIGN_TOP_LEFT, x, top + (wait && i != 1 ? 0 : kh - lv_font_get_line_height(vf[i])), w, vf[i]);
        rx_wrap(p->text[i + 1], wait ? i == 2 :
            i == 0 && !s->has_title &&
            !s->text[2][0] && !s->text[3][0]);
    }
    lv_opa_t fo = s->mode == RX_PAUSED ? LV_OPA_50 : LV_OPA_COVER;
    if(lv_obj_get_style_opa(p->bar, 0) != fo) lv_obj_set_style_opa(p->bar, fo, 0);
    rx_fill(p, s, 61, 165, 238, 12, TC(ACCENT_PRIMARY)); rx_times(p, s, 60, 184, 240);   /* a real fill in the track, "2:31" / "-1:32" under it */
    lv_obj_t *dim[4] = { p->text[1], p->text[2], p->text[3], g_rx_term_bar };   /* paused: the art-dimming rule, on text and bar */
    for(int i = 0; i < 4; i++){
        lv_opa_t o = s->mode == RX_PAUSED ? LV_OPA_70 : LV_OPA_COVER;
        if(i < 3){ if(lv_obj_get_style_text_opa(dim[i], 0) != o) lv_obj_set_style_text_opa(dim[i], o, 0); }
        else if(lv_obj_get_style_opa(dim[i], 0) != (s->mode == RX_PAUSED ? LV_OPA_50 : LV_OPA_COVER)) lv_obj_set_style_opa(dim[i], s->mode == RX_PAUSED ? LV_OPA_50 : LV_OPA_COVER, 0);
    }
    lv_color_t hc = wait ? TC(ACCENT_PRIMARY) : TC(TEXT_PRIMARY);
    if(!lv_color_eq(lv_obj_get_style_text_color(p->text[1], 0), hc)) lv_obj_set_style_text_color(p->text[1], hc, 0);
    for(int i = 0; i < 4; i++) lv_obj_set_pos(g_rx_term_key[i], 60, i < 3 ? 80 + 24 * i : 148);
    for(int i = 0; i < 3; i++) rx_box(p->key[i], (i - 1) * 70, 216, 56, 38);
}
const theme_kit_t theme_kit_term = { .id = "terminal", .header = term_header, .title_placed = term_cursor_place, .style = term_style, .home = term_home, .receiver = term_rx,
                                     .home_clock = term_clock,
                                     .nowplaying = term_np, .quicksettings = term_qs, .qs_tile = term_qs_tile,
                                     .saver = term_saver, .empty_state = term_empty, .row_value = term_row_value,
                                     .title = term_title, .np_state = term_np_state, .back_text = "<" };

/* ================================================================ blueprint
 * A technical drawing on drafting paper: thin 1 px line work, square corners, spaced monospace titles with a rule
 * under them, numbered rows with arrow marks, the pressed row outlined in the highlight colour (DECOR_1), crop marks
 * round the cover, and a Home laid out as a figure with crosshairs. */
static void blue_outline(lv_obj_t *o, lv_color_t c){
    kit_keep(o);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, c, 0);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
}
static lv_obj_t *g_bnum_pending[16];   /* lists whose rows this styling pass touched: numbered when it ends */
static int g_bnum_n;
static const char g_bnum_tag = 0;      /* user_data of our number labels */
#define BLUE_SHIFTED  LV_OBJ_FLAG_USER_1   /* a row child moved right for the number (moved back exactly) */
#define BLUE_NARROWED LV_OBJ_FLAG_USER_2   /* ... and narrowed with it */
static void blue_renumber(lv_obj_t *par);
static void blue_queue(lv_obj_t *par);
static void blue_row(lv_obj_t *o){
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(o, 1, LV_STATE_PRESSED);
    lv_obj_set_style_border_side(o, LV_BORDER_SIDE_FULL, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(o, TC(DECOR_1), LV_STATE_PRESSED);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, LV_STATE_PRESSED);
    /* numbered once the styling pass is done with the whole list (blue_pass_end) */
    lv_obj_t *par = lv_obj_get_parent(o);
    if(!par || lv_obj_get_parent(par) == lv_screen_active()) return;   /* a lone control on a screen, not a list */
    blue_queue(par);
}
/* Rows never leave a list one by one here: a list whose rows change is rebuilt (lv_obj_clean, then new rows; fixed
 * lists such as the Settings categories are built once), and new rows are styled, which queues the list again. So no watch on the list is needed (an event callback on it would also
 * make the list itself look tappable to the touch-area rules). The queue lives only within one styling pass. */
static void blue_queue(lv_obj_t *par){
    for(int k = 0; k < g_bnum_n; k++) if(g_bnum_pending[k] == par) return;
    if(g_bnum_n == (int)(sizeof g_bnum_pending / sizeof g_bnum_pending[0])){   /* full: number what is queued now */
        for(int k = 0; k < g_bnum_n; k++) blue_renumber(g_bnum_pending[k]);
        g_bnum_n = 0;
    }
    g_bnum_pending[g_bnum_n++] = par;
}
static int blue_is_row(lv_obj_t *c){ return lv_obj_has_flag(c, LV_OBJ_FLAG_CLICKABLE) && lv_obj_get_width(c) >= 200; }
static lv_obj_t *blue_number_of(lv_obj_t *row){
    for(uint32_t i = 0; i < lv_obj_get_child_count(row); i++){
        lv_obj_t *c = lv_obj_get_child(row, (int32_t)i);
        if(lv_obj_get_user_data(c) == (void *)&g_bnum_tag) return c;
    }
    return NULL;
}
static void blue_shift_left_half(lv_obj_t *row){           /* the row's left half moves for the number (text, icons, dots) */
    for(uint32_t i = 0; i < lv_obj_get_child_count(row); i++){
        lv_obj_t *c = lv_obj_get_child(row, (int32_t)i);
        /* Right aligned values and controls stay anchored to the row's right edge. */
        lv_align_t align = lv_obj_get_style_align(c, 0);
        if(align == LV_ALIGN_TOP_RIGHT || align == LV_ALIGN_RIGHT_MID || align == LV_ALIGN_BOTTOM_RIGHT) continue;
        if(lv_obj_get_user_data(c) == (void *)&g_bnum_tag || lv_obj_get_x(c) > lv_obj_get_width(row) / 2) continue;
        lv_obj_set_x(c, lv_obj_get_x(c) + 26);
        lv_obj_add_flag(c, BLUE_SHIFTED);
        int32_t w = lv_obj_get_style_width(c, 0);
        if(w != LV_SIZE_CONTENT && !LV_COORD_IS_PCT(w) && w > 86){ lv_obj_set_width(c, w - 26); lv_obj_add_flag(c, BLUE_NARROWED); }
    }
}
static void blue_unshift(lv_obj_t *row){                   /* exactly what blue_shift_left_half did, undone */
    for(uint32_t i = 0; i < lv_obj_get_child_count(row); i++){
        lv_obj_t *c = lv_obj_get_child(row, (int32_t)i);
        if(!lv_obj_has_flag(c, BLUE_SHIFTED)) continue;
        lv_obj_set_x(c, lv_obj_get_x(c) - 26);
        if(lv_obj_has_flag(c, BLUE_NARROWED)) lv_obj_set_width(c, lv_obj_get_style_width(c, 0) + 26);
        lv_obj_remove_flag(c, BLUE_SHIFTED | BLUE_NARROWED);
    }
}
/* The row numbers, drawing-index style, for a real list (three or more rows). A list with any two-line row (a title and
 * a subtitle) is unnumbered as a whole: the number column would cut the subtitles (blueprint's small face is
 * monospace). One walk over the list: every row's number is added, updated or removed so the list always agrees,
 * however its rows arrived (all at once or in batches). */
int playlistview_row_pos(lv_obj_t *row, int *rows, int *two_line);   /* playlistview.c */
static void blue_renumber(lv_obj_t *par){
    uint32_t cnt = lv_obj_get_child_count(par);
    int rows = 0, two_line = 0;
    /* a playlist streams its rows in: its shape and numbers come from the whole playlist, not the rows built yet */
    int pooled = row_pool_model && row_pool_model(par, &rows, &two_line);
    int streamed = !pooled && par == playlistview_scroller();
    if(streamed) playlistview_row_pos(NULL, &rows, &two_line);
    else if(!pooled) for(uint32_t j = 0; j < cnt; j++){
        lv_obj_t *row = lv_obj_get_child(par, (int32_t)j);
        if(!blue_is_row(row)) continue;
        rows++;
        int texts = 0;
        for(uint32_t i = 0; i < lv_obj_get_child_count(row); i++){
            lv_obj_t *c = lv_obj_get_child(row, (int32_t)i);
            if(!lv_obj_check_type(c, &lv_label_class) || lv_obj_get_user_data(c) == (void *)&g_bnum_tag) continue;
            if(lv_label_get_text(c)[0] && lv_obj_get_x(c) <= lv_obj_get_width(row) / 2) texts++;
        }
        if(texts >= 2) two_line = 1;
    }
    int want = rows >= 3 && !two_line, n = 0;
    for(uint32_t j = 0; j < cnt; j++){
        lv_obj_t *row = lv_obj_get_child(par, (int32_t)j);
        if(!blue_is_row(row)) continue;
        n++;
        lv_obj_t *num = blue_number_of(row);
        if(!want){
            if(num){ lv_obj_delete(num); blue_unshift(row); }
            continue;
        }
        if(!num){
            blue_shift_left_half(row);
            num = lv_label_create(row);
            lv_obj_set_user_data(num, (void *)&g_bnum_tag);        /* marks it as our number, not the row's own text */
            lv_obj_set_style_text_font(num, TF(UI_12), 0);
            lv_obj_set_style_text_color(num, TC(TEXT_MUTED), 0);
            lv_obj_align(num, LV_ALIGN_LEFT_MID, 8, 0);
            lv_obj_add_flag(num, LV_OBJ_FLAG_USER_4);             /* already styled: the pass leaves it as drawn here */
        }
        char b[8]; lv_snprintf(b, sizeof b, "%02d", pooled && row_pool_slot(row) ? row_pool_number(row) : streamed ? playlistview_row_pos(row, NULL, NULL) : n);
        if(strcmp(lv_label_get_text(num), b)) lv_label_set_text(num, b);
    }
}
static void blue_pass_end(void){
    for(int k = 0; k < g_bnum_n; k++) blue_renumber(g_bnum_pending[k]);   /* the pass that noted them has just ended */
    g_bnum_n = 0;
}
/* Blueprint's face and tracking; page geometry is resolved by the shared header. */
static void blue_title_fit(lv_obj_t *o, const char *text){
    (void)text;
    lv_obj_set_style_text_font(o, TF(HEADER), 0);
    lv_obj_set_style_text_letter_space(o, 3, 0);
}
/* A top-layer title inherits the current screen's permanent figure number. */
static int blue_figure(lv_obj_t *o){
    for(lv_obj_t *p = o; p; p = lv_obj_get_parent(p))
        for(int id = SCR_HOME; id < SCR_COUNT; id++) if(p == screen_get_root(id)) return screen_figure_number(id);
    return screen_figure_number(screen_current());
}
static void blue_title_form(lv_obj_t *o, const char *text, char *out, size_t cap){
    /* EQ has slot arrows in the header band. Its figure is drawn beside the title at a smaller face. */
    char full[192]; lv_snprintf(full, sizeof full, "fig.%d  %s", blue_figure(o), text ? text : "");
    lv_snprintf(out, cap, "%s", full);
}
static void blue_title(lv_obj_t *o, const char *text){
    char shown[192]; blue_title_form(o, text, shown, sizeof shown);
    blue_title_fit(o, shown);
    lv_label_set_text(o, shown);
}
static void blue_mark(lv_layer_t *layer, int x1, int y1, int x2, int y2, int dashed){
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.p1 = (lv_point_precise_t){x1, y1}; d.p2 = (lv_point_precise_t){x2, y2};
    d.color = TC(ACCENT_PRIMARY); d.width = 1; d.opa = LV_OPA_COVER;
    if(dashed){ d.dash_width = 5; d.dash_gap = 4; }
    lv_draw_line(layer, &d);
}
static void blue_row_draw(lv_event_t *e){
    lv_area_t a; lv_obj_get_coords(lv_event_get_target(e), &a);
    for(int x = a.x1 + 3; x <= a.x2 - 3; x += a.x2 - a.x1 - 6)
        for(int y = a.y1 + 3; y <= a.y2 - 3; y += a.y2 - a.y1 - 6){
            blue_mark(lv_event_get_layer(e), x - 2, y, x + 2, y, 0);
            blue_mark(lv_event_get_layer(e), x, y - 2, x, y + 2, 0);
        }
}
static void blue_row_decor(lv_obj_t *row, lv_obj_t *title){
    if(!title) return;
    lv_obj_remove_event_cb(row, blue_row_draw);
    lv_obj_add_event_cb(row, blue_row_draw, LV_EVENT_DRAW_MAIN_END, NULL);
}
static void blue_style(lv_obj_t *o, kit_role_t r){
    switch(r){
    case KIT_TITLE: {
        char txt[160]; lv_snprintf(txt, sizeof txt, "%s", lv_label_get_text(o));
        blue_title_fit(o, txt);                                   /* widest box first, then the padding */
        lv_obj_set_style_pad_hor(o, 10, 0);
        lv_label_set_text(o, txt);                                /* laid out again at the fitted face and box */
        lv_obj_set_style_text_color(o, TC(ACCENT_PRIMARY), 0);
        lv_obj_set_style_pad_bottom(o, 3, 0);
        lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);   /* the rule and padding add to the face, never cut it */
        lv_obj_set_style_border_side(o, LV_BORDER_SIDE_BOTTOM, 0);     /* the rule under a drawing title */
        lv_obj_set_style_border_width(o, 1, 0);
        lv_obj_set_style_border_color(o, TC(ACCENT_PRIMARY), 0);
        lv_obj_set_y(o, lv_obj_get_y(o) - 3);
        lv_obj_align(o, LV_ALIGN_TOP_MID, 0, lv_obj_get_y(o));

        break; }
    case KIT_ROW: blue_row(o); break;
    case KIT_ROW_CHEVRON:
        lv_label_set_text(o, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_font(o, TF(UI_12), 0);
        lv_obj_set_style_text_color(o, TC(ACCENT_PRIMARY), 0);
        break;
    case KIT_BUTTON: case KIT_ICON_BUTTON:
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10 && is_neutral_fill(o)) blue_outline(o, TC(ACCENT_PRIMARY));
        else if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10 && lv_color_eq(lv_obj_get_style_bg_color(o, 0), TC(ACTION_SURFACE))){
            /* a primary action is line work too, drawn in the accent (brand and danger buttons keep their fills) */
            blue_outline(o, TC(ACCENT_PRIMARY));
            for(uint32_t i = 0; i < lv_obj_get_child_count(o); i++){
                lv_obj_t *c = lv_obj_get_child(o, (int32_t)i);
                if(lv_obj_check_type(c, &lv_label_class)) lv_obj_set_style_text_color(c, TC(ACCENT_PRIMARY), 0);
            }
        }
        else lv_obj_set_style_radius(o, 0, 0);
        lv_obj_set_style_bg_color(o, TC(SURFACE_RAISED), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_STATE_PRESSED);
        break;
    case KIT_CARD: case KIT_POPUP:
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10){
            lv_obj_set_style_radius(o, 0, 0);
            lv_obj_set_style_bg_color(o, TC(SURFACE), 0);
            lv_obj_set_style_border_width(o, 1, 0);
            lv_obj_set_style_border_color(o, TC(ACCENT_PRIMARY), 0);
            lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
        }
        break;
    case KIT_TOGGLE:
        lv_obj_set_style_radius(o, 0, LV_PART_MAIN); lv_obj_set_style_radius(o, 0, LV_PART_INDICATOR);
        lv_obj_set_style_radius(o, 0, LV_PART_KNOB);
        break;
    case KIT_SLIDER: case KIT_BAR:
        lv_obj_set_style_radius(o, 0, LV_PART_MAIN); lv_obj_set_style_radius(o, 0, LV_PART_INDICATOR);
        lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, LV_PART_KNOB);
        /* line work, not a solid block: the track is drawn as a 1 px outline and the level as a translucent wash of
         * the accent (a thick slider - Quick Settings' brightness - was a solid blue slab in a drawing of thin lines) */
        if(lv_obj_get_height(o) >= 20){
            lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_border_width(o, 1, LV_PART_MAIN);
            lv_obj_set_style_border_color(o, TC(ACCENT_PRIMARY), LV_PART_MAIN);
            lv_obj_set_style_border_opa(o, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(o, TC(ACCENT_PRIMARY), LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(o, LV_OPA_40, LV_PART_INDICATOR);
            for(uint32_t i = 0; i < lv_obj_get_child_count(o); i++){   /* an icon on it (the sun): ink, not on-fill dark */
                lv_obj_t *c = lv_obj_get_child(o, (int32_t)i);
                if(lv_obj_check_type(c, &lv_label_class)){ lv_obj_set_style_text_color(c, TC(ACCENT_PRIMARY), 0); kit_keep(c); }
            }
        }
        break;
    case KIT_ROLLER: case KIT_TEXTAREA:
        lv_obj_set_style_radius(o, 0, 0);
        lv_obj_set_style_border_width(o, 1, 0);
        lv_obj_set_style_border_color(o, TC(BORDER_STRONG), 0);
        lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
        break;
    case KIT_KEYBOARD:
        lv_obj_set_style_radius(o, 0, LV_PART_ITEMS);
        lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_ITEMS);
        lv_obj_set_style_border_width(o, 1, LV_PART_ITEMS);
        lv_obj_set_style_border_color(o, TC(BORDER_STRONG), LV_PART_ITEMS);
        lv_obj_set_style_border_opa(o, LV_OPA_COVER, LV_PART_ITEMS);
        break;
    default: break;
    }
}
static lv_obj_t *blue_line(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t c){
    lv_obj_t *l = lv_obj_create(parent);
    lv_obj_remove_style_all(l);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(l, x, y); lv_obj_set_size(l, w, h);
    lv_obj_set_style_bg_color(l, c, 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    return l;
}
static lv_obj_t *g_blue_clock, *g_blue_mer;
static void blue_clock(const char *t){
    if(!g_blue_clock) return;
    char hm[16], mer[8]; clock_parts(t, hm, sizeof hm, mer, sizeof mer);
    lv_label_set_text(g_blue_clock, hm);
    lv_label_set_text(g_blue_mer, mer);
    /* AM/PM just past the time's text (the block around it is wider) */
    lv_point_t tw; lv_text_get_size(&tw, hm, lv_obj_get_style_text_font(g_blue_clock, 0), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_obj_align_to(g_blue_mer, g_blue_clock, LV_ALIGN_OUT_RIGHT_BOTTOM, 2 - (192 - tw.x) / 2, -11);
}
static void blue_home(const home_parts_t *p){
    own_backgrounds();
    /* the figure: crosshairs on the sheet's own lines (the grid is 12 px, its centre rule at x 180), the time as fig.1 */
    lv_obj_t *h = blue_line(p->root, 0, 144, 360, 1, TC(BORDER_STRONG));
    lv_obj_t *v = blue_line(p->root, 180, 0, 1, 360, TC(BORDER_STRONG));
    lv_obj_move_to_index(h, 0); lv_obj_move_to_index(v, 0);
    lv_obj_t *fig = lv_label_create(p->root);
    lv_label_set_text(fig, "fig.1  TIME");
    lv_obj_set_style_text_font(fig, TF(UI_12), 0);
    lv_obj_set_style_text_color(fig, TC(DECOR_1), 0);
    lv_obj_set_pos(fig, 84, 68);                                     /* on the time block's left edge */
    lv_obj_set_pos(p->status, 0, 40);
    lv_obj_set_style_text_color(p->status, TC(TEXT_MUTED), 0);
    /* the time sits on the paper, over the lines, in a block of whole grid cells (16 x 4: x 84..276, y 84..132) */
    lv_obj_set_style_bg_color(p->clock, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(p->clock, LV_OPA_COVER, 0);
    lv_obj_set_size(p->clock, 192, 48);
    lv_obj_set_style_pad_all(p->clock, 0, 0);
    lv_obj_set_style_pad_top(p->clock, (48 - lv_font_get_line_height(lv_obj_get_style_text_font(p->clock, 0))) / 2, 0);
    lv_obj_set_style_text_align(p->clock, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(p->clock, 84, 84);
    g_blue_clock = p->clock;
    g_blue_mer = mer_label(p->root, TF(UI_12), TC(DECOR_1));
    blue_clock(lv_label_get_text(p->clock));
    lv_obj_set_pos(p->date, 0, 160);
    lv_obj_set_style_text_letter_space(p->date, 2, 0);
    lv_obj_set_style_text_color(p->date, TC(ACCENT_PRIMARY), 0);
    lv_obj_set_pos(p->weather, 0, 180);
    /* Library and Search as outlined callouts (clear of the weather line's touch area). Every edge on a grid line:
     * Library 72..228, Search 240..288 (the row centred), y 216..256; now playing 60..300, y 264..312 */
    lv_obj_set_pos(p->library, 72, 216); lv_obj_set_size(p->library, 156, 40);
    blue_outline(p->library, TC(ACCENT_PRIMARY));
    home_library_fit(p->library);
    lv_obj_set_pos(p->search, 240, 216); lv_obj_set_size(p->search, 48, 40);
    blue_outline(p->search, TC(ACCENT_PRIMARY));
    lv_obj_set_pos(p->np, 60, 264); lv_obj_set_size(p->np, 240, 48);
    blue_outline(p->np, TC(ACCENT_PRIMARY));
    lv_obj_set_style_bg_color(p->np, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(p->np, LV_OPA_COVER, 0);
    home_set_art_enabled(0);
    lv_obj_set_pos(p->np_thumb, 8, 12); lv_obj_set_size(p->np_thumb, 24, 24);
    lv_obj_set_style_radius(p->np_thumb, 0, 0);
    lv_obj_set_style_bg_opa(p->np_thumb, LV_OPA_TRANSP, 0);
    lv_label_set_text(p->np_glyph, ">");
    lv_obj_set_style_text_font(p->np_glyph, TF(UI_16), 0);
    lv_obj_set_style_text_color(p->np_glyph, TC(DECOR_1), 0);
    lv_obj_center(p->np_glyph);
    lv_obj_set_pos(p->np_title, 36, 6);  lv_obj_set_width(p->np_title, 150);
    lv_obj_set_pos(p->np_artist, 36, 26); lv_obj_set_width(p->np_artist, 150);
    lv_obj_set_pos(p->pp, 190, 3); lv_obj_set_size(p->pp, 42, 42);   /* 8 px from the end, as the marker is from the start */
    lv_obj_set_style_text_color(p->pp_label, TC(ACCENT_PRIMARY), 0);
}
static void blue_np(const np_parts_t *p){
    own_backgrounds();
    lv_obj_set_style_arc_width(p->ring, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_width(p->ring, 2, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(p->ring, TC(BORDER_STRONG), LV_PART_MAIN);
    lv_obj_set_style_pad_all(p->ring, 4, LV_PART_KNOB);
    lv_obj_set_style_radius(p->cover, 0, 0);
    lv_obj_set_style_border_width(p->cover, 1, 0);
    lv_obj_set_style_border_color(p->cover, TC(ACCENT_PRIMARY), 0);
    lv_obj_set_style_border_opa(p->cover, LV_OPA_COVER, 0);
    /* crop marks just outside the cover's corners */
    lv_obj_update_layout(p->root);
    lv_area_t a; lv_obj_get_coords(p->cover, &a);
    int cx[2] = { a.x1 - 10, a.x2 + 10 }, cy[2] = { a.y1 - 10, a.y2 + 10 };
    for(int i = 0; i < 4; i++){
        int x = cx[i & 1], y = cy[i >> 1];
        blue_line(p->root, x - 7, y, 15, 1, TC(DECOR_1));
        blue_line(p->root, x, y - 7, 1, 15, TC(DECOR_1));
    }
    lv_obj_set_style_text_letter_space(p->title, 1, 0);
    lv_obj_set_size(p->pp, 48, 48);
    lv_obj_set_style_radius(p->pp, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(p->pp, TC(CANVAS), 0);                 /* the grid stops at the circle: a clear key */
    lv_obj_set_style_bg_opa(p->pp, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p->pp, 2, 0);
    lv_obj_set_style_border_color(p->pp, TC(DECOR_1), 0);
    lv_obj_set_style_border_opa(p->pp, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(p->pp, TC(DECOR_1), 0);
    lv_obj_set_style_text_align(p->pp, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(p->pp, TF(UI_28), 0);                 /* a size down: the glyph clears the 2 px ring */
    lv_obj_set_style_pad_top(p->pp, 8, 0);                           /* ui_pp_glyph re-centres it by its ink */
    lv_obj_align(p->pp, LV_ALIGN_TOP_MID, 0, 264);
    lv_obj_set_ext_click_area(p->pp, 4);
    lv_obj_align(p->prev, LV_ALIGN_TOP_MID, -62, 274);
    lv_obj_align(p->next, LV_ALIGN_TOP_MID, 62, 274);
    lv_obj_set_style_text_color(p->prev, TC(ACCENT_PRIMARY), 0);
    lv_obj_set_style_text_color(p->next, TC(ACCENT_PRIMARY), 0);
    np_text_rows(p, 191, 219, 241);                          /* the album line clears the ring at 264, no two rows touch */
    np_sides_on_cover(p, 0);                                 /* times stay at 318: the play ring ends at 312 */
    /* Dimension rules sit under the time labels, split around the page dots. The existing
     * circular seek remains the full input surface and its progress stays visible. */
    for(int i = 0; i < 2; i++){
        int x = i ? 198 : 112;
        blue_line(p->root, x, 340, 50, 1, TC(ACCENT_PRIMARY));
        blue_line(p->root, x, 337, 1, 7, TC(ACCENT_PRIMARY));
        blue_line(p->root, x + 49, 337, 1, 7, TC(ACCENT_PRIMARY));
    }
}
static void blue_cover_draw(lv_event_t *e){
    lv_obj_t *cover = lv_event_get_target(e);
    if(!lv_obj_has_flag(cover, LV_OBJ_FLAG_USER_3)) return;
    lv_area_t a; lv_obj_get_coords(cover, &a);
    int x1 = a.x1 + 4, y1 = a.y1 + 4, x2 = a.x2 - 4, y2 = a.y2 - 4;
    lv_layer_t *layer = lv_event_get_layer(e);
    blue_mark(layer, x1, y1, x2, y1, 1); blue_mark(layer, x2, y1, x2, y2, 1);
    blue_mark(layer, x2, y2, x1, y2, 1); blue_mark(layer, x1, y2, x1, y1, 1);
    blue_mark(layer, x1 + 10, y1 + 10, x2 - 10, y2 - 10, 0);
    blue_mark(layer, x2 - 10, y1 + 10, x1 + 10, y2 - 10, 0);
}
static void blue_cover(lv_obj_t *cover, lv_obj_t *note, int show, int loading){
    if(!cover || !note) return;
    lv_obj_remove_event_cb(cover, blue_cover_draw);
    lv_obj_add_event_cb(cover, blue_cover_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    if(show && !loading){ lv_obj_add_flag(cover, LV_OBJ_FLAG_USER_3); lv_obj_add_flag(note, LV_OBJ_FLAG_HIDDEN); }
    else { lv_obj_remove_flag(cover, LV_OBJ_FLAG_USER_3); if(show) lv_obj_remove_flag(note, LV_OBJ_FLAG_HIDDEN); }
    lv_obj_invalidate(cover);
}
static void blue_empty_draw(lv_event_t *e){
    lv_obj_t *o = lv_event_get_user_data(e);
    const char *saved = lv_obj_get_user_data(o);
    if(lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
    if(!saved || strcmp(saved, lv_label_get_text(o))) return; /* Lyrics reuses this label for real lyrics */
    lv_area_t a; lv_obj_get_coords(o, &a);
    lv_point_t sz; lv_text_get_size(&sz, saved, lv_obj_get_style_text_font(o, 0), 0, 2, 256, LV_TEXT_FLAG_NONE);
    int fw = sz.x + 24; if(fw < 100) fw = 100; if(fw > 280) fw = 280;
    int x = (a.x1 + a.x2 - fw) / 2;
    a.x1 = x; a.x2 = x + fw;
    if(!lv_obj_has_flag(o, LV_OBJ_FLAG_USER_2)){ a.y1 -= 12; a.y2 += 12; }
    lv_layer_t *layer = lv_event_get_layer(e);
    blue_mark(layer, a.x1, a.y1, a.x2, a.y1, 1);
    blue_mark(layer, a.x2, a.y1, a.x2, a.y2, 1);
    blue_mark(layer, a.x2, a.y2, a.x1, a.y2, 1);
    blue_mark(layer, a.x1, a.y2, a.x1, a.y1, 1);
}
static void blue_empty_deleted(lv_event_t *e){
    lv_obj_t *o = lv_event_get_target(e);
    lv_obj_remove_event_cb_with_user_data(lv_obj_get_parent(o), blue_empty_draw, o);
    free(lv_obj_get_user_data(o));
}
static void blue_empty(kit_empty_kind_t kind, lv_obj_t *message){
    if(!message) return;
    const char *s = lv_label_get_text(message);
    lv_obj_set_style_text_font(message, TF(UI_14), 0);
    lv_obj_set_style_text_color(message, TC(ACCENT_PRIMARY), 0);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    if(kind == KIT_EMPTY_LYRICS){
        lv_obj_remove_flag(message, LV_OBJ_FLAG_USER_2);
        lv_obj_set_style_pad_ver(message, 0, 0);
        lv_obj_update_layout(message);
        lv_area_t a; lv_obj_get_coords(message, &a);
        lv_obj_set_style_translate_y(message, 180 - (a.y1 + a.y2) / 2, 0);
    } else {
        lv_obj_add_flag(message, LV_OBJ_FLAG_USER_2);
        lv_obj_set_style_pad_ver(message, 12, 0);
        if(kind == KIT_EMPTY_UPNEXT){
            lv_obj_add_flag(message, LV_OBJ_FLAG_IGNORE_LAYOUT);
            lv_obj_set_style_translate_y(message, 0, 0);
            lv_obj_align(message, LV_ALIGN_CENTER, 0, 0);
        }
    }
    free(lv_obj_get_user_data(message));
    lv_obj_set_user_data(message, strdup(s));
    lv_obj_t *parent = lv_obj_get_parent(message);
    lv_obj_remove_event_cb_with_user_data(parent, blue_empty_draw, message);
    lv_obj_add_event_cb(parent, blue_empty_draw, LV_EVENT_DRAW_MAIN_END, message);
    lv_obj_invalidate(parent);
    lv_obj_remove_event_cb(message, blue_empty_deleted);
    lv_obj_add_event_cb(message, blue_empty_deleted, LV_EVENT_DELETE, NULL);
}
static void blue_qs_tile(lv_obj_t *c, lv_obj_t *g, lv_obj_t *cap, int on, int toggle){
    (void)toggle;
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, on ? TC(DECOR_1) : TC(ACCENT_PRIMARY), 0);
    lv_obj_set_style_border_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_outline_width(c, on ? 1 : 0, 0);            /* a lit tile is a double circle */
    lv_obj_set_style_outline_pad(c, 3, 0);
    lv_obj_set_style_outline_color(c, TC(DECOR_1), 0);
    lv_obj_set_style_outline_opa(c, LV_OPA_COVER, 0);
    if(g) lv_obj_set_style_text_color(g, on ? TC(DECOR_1) : TC(ACCENT_PRIMARY), 0);
    if(cap){
        char up[32]; lv_label_set_text(cap, theme_upper(up, sizeof up, lv_label_get_text(cap)));
        lv_obj_set_style_text_font(cap, TF(UI_12), 0);
        lv_obj_set_style_text_color(cap, on ? TC(DECOR_1) : TC(TEXT_SECONDARY), 0);
    }
}
static void blue_saver(const saver_parts_t *p){
    static lv_obj_t *bar[9];
    if(p->date){ lv_obj_set_style_text_letter_space(p->date, 2, 0); lv_obj_set_style_text_color(p->date, TC(ACCENT_PRIMARY), 0); }
    if(!bar[0]){
        for(int i = 0; i < 8; i++) bar[i] = blue_line(p->root, 0, 0, 16, 8,
                                                       i % 2 ? TC(CANVAS) : TC(ACCENT_PRIMARY));
        bar[8] = blue_line(p->root, 0, 0, 128, 1, TC(ACCENT_PRIMARY));
    }
    int bottom = 0;
    lv_obj_t *above[] = {p->date, p->weather};
    for(int i = 0; i < 2; i++) if(above[i] && !lv_obj_has_flag(above[i], LV_OBJ_FLAG_HIDDEN)){
        lv_obj_update_layout(above[i]);
        int edge = lv_obj_get_y(above[i]) + lv_obj_get_height(above[i]);
        if(edge > bottom) bottom = edge;
    }
    int y = bottom + 8;
    if(y < (p->style == 1 ? 282 : 252)) y = p->style == 1 ? 282 : 252;
    if(p->track && p->artist){
        lv_obj_set_style_text_font(p->track, TF(UI_14), 0);
        lv_obj_set_style_text_font(p->artist, TF(UI_12), 0);
        lv_obj_set_style_text_color(p->track, TC(ACCENT_PRIMARY), 0);
        lv_obj_set_style_text_color(p->artist, TC(TEXT_MUTED), 0);
        lv_obj_set_style_text_align(p->track, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_align(p->artist, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_width(p->track, 160); lv_obj_set_width(p->artist, 160);
        lv_obj_align(p->track, LV_ALIGN_TOP_LEFT, 100, y);
        int ay = y + lv_font_get_line_height(TF(UI_14)) + 3;
        lv_obj_align(p->artist, LV_ALIGN_TOP_LEFT, 100, ay);
        y = ay + lv_font_get_line_height(TF(UI_12)) + 9;
    }
    for(int i = 0; i < 8; i++) lv_obj_set_pos(bar[i], 116 + 16 * i, y);
    lv_obj_set_pos(bar[8], 116, y + 8);
}
/* AirPlay receiving: the drawing-sheet tag, a thin display-only ring, crop marks on the cover, a ringed play key. */
static lv_obj_t *g_rx_blue_mark[4], *g_rx_blue_slot;
static void blue_rx(const rx_parts_t *p, rx_state_t *s){
    if(s->first){
        lv_obj_set_size(p->arc, 336, 336); lv_obj_center(p->arc);
        lv_obj_set_style_arc_width(p->arc, 3, LV_PART_MAIN);
        lv_obj_set_style_arc_color(p->arc, TC(BORDER_STRONG), LV_PART_MAIN);
        lv_obj_set_style_arc_width(p->arc, 3, LV_PART_INDICATOR); lv_obj_set_style_arc_color(p->arc, TC(ACCENT_PRIMARY), LV_PART_INDICATOR);
        rx_times_style(p, TF(UI_12), TC(TEXT_MUTED));
        lv_obj_set_style_radius(p->frame, 0, 0);
        lv_obj_set_style_border_width(p->frame, 1, 0);
        lv_obj_set_style_border_color(p->frame, TC(ACCENT_PRIMARY), 0);
        lv_obj_set_style_border_opa(p->frame, LV_OPA_COVER, 0);
        for(int i = 0; i < 2; i++){   /* registration marks outside the cover's top corners (the lower ones would meet the title) */
            int x = i ? 264 + 10 : 106 - 10, y = 44 - 10;
            g_rx_blue_mark[2 * i] = blue_line(p->root, x - 7, y, 15, 1, TC(DECOR_1));
            g_rx_blue_mark[2 * i + 1] = blue_line(p->root, x, y - 7, 1, 15, TC(DECOR_1));
        }
        static lv_point_precise_t box[5] = { {0, 0}, {88, 0}, {88, 88}, {0, 88}, {0, 0} };   /* the empty art slot, dashed */
        g_rx_blue_slot = lv_line_create(p->root);
        lv_line_set_points(g_rx_blue_slot, box, 5); lv_obj_set_pos(g_rx_blue_slot, 136, 62);
        lv_obj_set_style_line_width(g_rx_blue_slot, 1, 0); lv_obj_set_style_line_color(g_rx_blue_slot, TC(BORDER_STRONG), 0);
        lv_obj_set_style_line_dash_width(g_rx_blue_slot, 4, 0); lv_obj_set_style_line_dash_gap(g_rx_blue_slot, 3, 0);
        rx_label(p->text[0], TF(UI_12), TC(ACCENT_PRIMARY)); lv_obj_set_style_text_letter_space(p->text[0], 1, 0);
        rx_label(p->text[1], TF(USER_20), TC(TEXT_PRIMARY)); rx_label(p->text[2], TF(USER_16), TC(TEXT_SECONDARY));
        rx_label(p->text[3], TF(USER_14), TC(TEXT_MUTED)); rx_label(p->text[4], TF(UI_12), TC(TEXT_MUTED));
        lv_obj_set_style_radius(p->key[1], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(p->key[1], TC(CANVAS), 0);
        lv_obj_set_style_bg_opa(p->key[1], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(p->key[1], 2, 0);
        lv_obj_set_style_border_color(p->key[1], TC(DECOR_1), 0);
        lv_obj_set_style_text_color(p->label[1], TC(DECOR_1), 0);
        lv_obj_set_style_text_color(p->label[0], TC(ACCENT_PRIMARY), 0); lv_obj_set_style_text_color(p->label[2], TC(ACCENT_PRIMARY), 0);
        return;
    }
    int rx = s->mode == RX_RECEIVING || s->mode == RX_PAUSED, wait = s->mode == RX_WAITING;
    for(int i = 0; i < 4; i++) rx_show(g_rx_blue_mark[i], rx);
    rx_show(g_rx_blue_slot, wait);
    rx_ring(p, s); rx_show(p->bar, 0);   /* the thin outer ring is the progress; no bar */
    rx_times(p, s, 117, 316, 126);   /* inside the ring: its inner edge is y 334 at the outer label ends */
    if(s->mode == RX_OTHER) return;
    if(wait) lv_snprintf(s->text[0], sizeof s->text[0], "AIRPLAY");
    char tag[96]; lv_snprintf(tag, sizeof tag, "|-- %s --|", s->text[0]); lv_snprintf(s->text[0], sizeof s->text[0], "%s", tag);
    rx_place(p->text[0], LV_ALIGN_TOP_MID, 0, 19, 200, NULL);
    rx_rows(p, s, (const int[8]){ 191, 219, 241, 328, 80, 156, 186, 210 }, (const lv_font_t *const[3]){ TF(USER_20), TF(USER_16), TF(USER_20) });
    rx_transport(p, 264, 48, 62);
}
const theme_kit_t theme_kit_blue = { .id = "blueprint", .header = kit_edge_header, .style = blue_style, .home = blue_home, .receiver = blue_rx,
                                     .title_fit = blue_title_fit,
                                     .home_clock = blue_clock,
                                     .nowplaying = blue_np, .np_cover_state = blue_cover, .empty_state = blue_empty,
                                     .row_decor = blue_row_decor, .title = blue_title,
                                     .qs_tile = blue_qs_tile, .saver = blue_saver,
                                     .pass_end = blue_pass_end };

/* ================================================================ stone
 * Soft and tactile: everything is a rounded pill sitting on the surface, with a solid 3 px edge underneath in the
 * shadow colour (DOT_GRID). The edge is below the pill, not inside it: the pill keeps its whole shape and its content
 * stays centred. It is drawn as the pill's own shape, filled and dropped 3 px, just before the pill (one more rounded
 * fill: LVGL's shadow path would rebuild a corner buffer on every draw here, with no shadow cache in this build).
 * Chunky rounded controls; the Home time as two overlapping pills, hours and minutes. */
/* Three overlapping rounded fills give each end a different shoulder. All drawing stays inside the
 * original rectangular object, so its input box and the position of its children are unchanged. */
static void stone_blob(lv_layer_t *layer, const lv_area_t *box, lv_color_t color, lv_opa_t opa){
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_color = color; d.bg_opa = opa; d.radius = LV_RADIUS_CIRCLE;
    lv_area_t a = *box;
    a.x1 += 5; a.x2 -= 5;
    lv_draw_rect(layer, &d, &a);
    a = *box; a.x2 -= 11; a.y1 += 3; a.y2 -= 8;
    lv_draw_rect(layer, &d, &a);
    a = *box; a.x1 += 11; a.y1 += 8; a.y2 -= 2;
    lv_draw_rect(layer, &d, &a);
}
static void stone_edge_cb(lv_event_t *e){
    lv_obj_t *o = lv_event_get_target(e);
    int drop = lv_obj_has_state(o, LV_STATE_PRESSED) ? 1 : (int)(intptr_t)lv_event_get_user_data(e);   /* pressed: sinks */
    if(lv_event_get_code(e) == LV_EVENT_REFR_EXT_DRAW_SIZE){ lv_event_set_ext_draw_size(e, 4); return; }
    lv_area_t a; lv_obj_get_coords(o, &a);
    lv_area_move(&a, 0, drop);
    stone_blob(lv_event_get_layer(e), &a, TC(DOT_GRID), lv_obj_get_style_opa_recursive(o, 0));
    lv_obj_get_coords(o, &a);
    stone_blob(lv_event_get_layer(e), &a, lv_obj_get_style_bg_color(o, LV_PART_MAIN),
               lv_obj_get_style_opa_recursive(o, 0));
}
static void stone_edge(lv_obj_t *o, int drop){
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_remove_event_cb_with_user_data(o, stone_edge_cb, (void *)(intptr_t)3);   /* restyled: one edge, not two */
    lv_obj_remove_event_cb_with_user_data(o, stone_edge_cb, (void *)(intptr_t)4);
    lv_obj_add_event_cb(o, stone_edge_cb, LV_EVENT_DRAW_MAIN_BEGIN, (void *)(intptr_t)drop);
    lv_obj_add_event_cb(o, stone_edge_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, (void *)(intptr_t)drop);
    lv_obj_refresh_ext_draw_size(o);
}
static void stone_pill(lv_obj_t *o, lv_color_t bg){
    kit_keep(o);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_STATE_PRESSED);
    stone_edge(o, 3);
    lv_obj_set_style_translate_y(o, 2, LV_STATE_PRESSED);            /* pressed: it sinks into the surface */
}
static void stone_style(lv_obj_t *o, kit_role_t r){
    switch(r){
    case KIT_ROW:
        stone_pill(o, TC(SURFACE));
        lv_obj_set_style_bg_color(o, TC(SURFACE_RAISED), LV_STATE_PRESSED);
        break;
    case KIT_ROW_CHEVRON:
        lv_obj_set_style_text_color(o, TC(ACCENT_PRIMARY), 0);
        break;
    case KIT_BUTTON: case KIT_ICON_BUTTON:
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10) stone_pill(o, lv_obj_get_style_bg_color(o, 0));
        else lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
        break;
    case KIT_CARD:
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10) lv_obj_set_style_radius(o, 22, 0);
        break;
    case KIT_POPUP:
        lv_obj_set_style_radius(o, 26, 0);
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10) stone_edge(o, 4);
        break;
    case KIT_SLIDER: case KIT_BAR:
        lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
        lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, LV_PART_KNOB);
        break;
    case KIT_ROLLER: case KIT_TEXTAREA:
        lv_obj_set_style_radius(o, 18, 0);
        lv_obj_set_style_radius(o, 14, LV_PART_SELECTED);
        break;
    case KIT_KEYBOARD:
        lv_obj_set_style_radius(o, 12, LV_PART_ITEMS);
        lv_obj_set_style_border_side(o, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS);
        lv_obj_set_style_border_width(o, 2, LV_PART_ITEMS);
        lv_obj_set_style_border_color(o, TC(DOT_GRID), LV_PART_ITEMS);
        lv_obj_set_style_border_opa(o, LV_OPA_COVER, LV_PART_ITEMS);
        break;
    case KIT_TEXT:
        if(is_large_default_text(o)) lv_obj_set_style_text_font(o, TF(UI_24), 0);
        break;
    default: break;
    }
}
static void stone_empty_draw(lv_event_t *e){
    if(lv_event_get_code(e) == LV_EVENT_REFR_EXT_DRAW_SIZE){ lv_event_set_ext_draw_size(e, 26); return; }
    lv_obj_t *message = lv_event_get_target(e);
    const char *saved = lv_obj_get_user_data(message);
    if(!saved || strcmp(saved, lv_label_get_text(message))) return; /* Lyrics can reuse this label. */
    lv_point_t size;
    lv_text_get_size(&size, saved, lv_obj_get_style_text_font(message, 0), 0, 2,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int w = size.x + 32; if(w < 112) w = 112; if(w > 282) w = 282;
    int h = lv_obj_get_height(message) + 28; if(h < 46) h = 46;
    lv_area_t label; lv_obj_get_coords(message, &label);
    int cx = (label.x1 + label.x2) / 2, cy = (label.y1 + label.y2) / 2;
    lv_area_t pebble = { cx - w/2, cy - h/2, cx + (w-1)/2, cy + (h-1)/2 };
    stone_blob(lv_event_get_layer(e), &pebble, TC(SURFACE_RAISED), LV_OPA_COVER);
}
static void stone_empty_deleted(lv_event_t *e){ free(lv_obj_get_user_data(lv_event_get_target(e))); }
static int stone_empty_center(lv_obj_t *message){
    lv_obj_t *parent = lv_obj_get_parent(message);
    lv_obj_update_layout(parent);
    lv_area_t a; lv_obj_get_coords(parent, &a);
    return a.y1 + lv_obj_get_style_pad_top(parent, 0) + lv_obj_get_content_height(parent)/2;
}
static void stone_empty_at(lv_obj_t *message, int center){
    /* Anchor to the viewport so a taller header can resize it without stale offsets. */
    int offset = center - stone_empty_center(message);
    lv_obj_add_flag(message, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_style_translate_y(message, 0, 0);
    lv_obj_align(message, LV_ALIGN_CENTER, 0, offset);
}
static void stone_empty(kit_empty_kind_t kind, lv_obj_t *message){
    if(!message) return;
    lv_obj_set_style_text_color(message, TC(TEXT_PRIMARY), 0);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(message, TF(UI_14), 0);
    lv_obj_update_layout(message);
    if(kind == KIT_EMPTY_LYRICS) lv_obj_align(message, LV_ALIGN_CENTER, 0, 0);
    else stone_empty_at(message, stone_empty_center(message));
    lv_obj_remove_event_cb(message, stone_empty_draw);
    lv_obj_remove_event_cb(message, stone_empty_deleted);
    free(lv_obj_get_user_data(message));
    lv_obj_set_user_data(message, strdup(lv_label_get_text(message)));
    lv_obj_add_event_cb(message, stone_empty_draw, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
    lv_obj_add_event_cb(message, stone_empty_draw, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
    lv_obj_refresh_ext_draw_size(message);
    lv_obj_add_event_cb(message, stone_empty_deleted, LV_EVENT_DELETE, NULL);
}
static void zine_scrap(lv_obj_t *o, lv_color_t bg);
void kit_stone_empty_scan(lv_obj_t *message, lv_obj_t *button){
    int zine = !strcmp(theme_kit()->id, "zine");
    if((strcmp(theme_kit()->id, "stone") && !zine) || !message || !button) return;
    if(zine){   /* a yellow scrap like Home's search button: it reads as something to press, not a second message */
        zine_scrap(button, TC(DECOR_1));
        lv_obj_set_style_bg_color(button, TC(DECOR_1), LV_STATE_PRESSED);
    }
    int center = stone_empty_center(message);
    int pebble_h = lv_obj_get_height(message) + (zine ? 0 : 28);
    if(pebble_h < 46) pebble_h = 46;
    int gap = 20, button_h = lv_obj_get_height(button);
    int top = center - (pebble_h + gap + button_h)/2;
    stone_empty_at(message, top + pebble_h/2);
    stone_empty_at(button, top + pebble_h + gap + button_h/2);
    /* the scrap's press nudge is +2 px from where it now sits (a bare pressed translate_y would replace the
     * placement and drop the button back under the message) */
    if(zine) lv_obj_set_style_translate_y(button, lv_obj_get_style_translate_y(button, 0) + 2, LV_STATE_PRESSED);
}
static void stone_cover_draw(lv_event_t *e){
    lv_obj_t *cover = lv_event_get_target(e);
    lv_obj_t *note = lv_event_get_user_data(e);
    if(!note || lv_obj_has_flag(note, LV_OBJ_FLAG_HIDDEN)) return;
    lv_area_t a; lv_obj_get_coords(cover, &a);
    stone_blob(lv_event_get_layer(e), &a, TC(ART_PLACEHOLDER), LV_OPA_COVER);
}
static void stone_cover(lv_obj_t *cover, lv_obj_t *note, int show, int loading){
    if(!cover || !note) return;
    lv_obj_set_style_bg_opa(cover, LV_OPA_TRANSP, 0);
    lv_label_set_text(note, loading ? LV_SYMBOL_REFRESH : LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(note, TF(UI_20), 0); /* this face has LVGL's audio symbol */
    lv_obj_set_style_text_color(note, TC(TEXT_PRIMARY), 0);
    if(show) lv_obj_center(note);
}
static lv_obj_t *g_stone_arc[5];
static int g_stone_arc_bucket = -1;
static void stone_progress(int value){
    if(!g_stone_arc[0]) return;
    int bucket = (value * 143 + 500) / 1000; /* two-degree steps over the 286-degree sweep */
    if(bucket == g_stone_arc_bucket) return;
    g_stone_arc_bucket = bucket;
    int end = bucket * 2;
    for(int i = 0; i < 5; i++){
        int from = i == 0 ? 0 : end - (5-i)*5;
        int to = i == 0 ? end - 20 : from + 5;
        if(from < 0) from = 0;
        if(to > end) to = end;
        if(to <= from){ lv_obj_add_flag(g_stone_arc[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_arc_set_angles(g_stone_arc[i], from, to);
        lv_obj_remove_flag(g_stone_arc[i], LV_OBJ_FLAG_HIDDEN);
    }
}
static void stone_progress_create(lv_obj_t *ring){
    for(int i = 0; i < 5; i++){
        lv_obj_t *arc = lv_arc_create(lv_obj_get_parent(ring));
        lv_obj_set_size(arc, lv_obj_get_width(ring), lv_obj_get_height(ring));
        lv_obj_align(arc, LV_ALIGN_CENTER, 0, 0);
        lv_arc_set_rotation(arc, 127);
        lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_arc_width(arc, 12, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(arc, TC(ACCENT_PRIMARY), LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(arc, i == 0 ? LV_OPA_COVER : (lv_opa_t)(190 - 43*(i-1)), LV_PART_INDICATOR);
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(arc, LV_OBJ_FLAG_HIDDEN);
        g_stone_arc[i] = arc;
    }
    lv_obj_set_style_arc_opa(ring, LV_OPA_TRANSP, LV_PART_INDICATOR);
    g_stone_arc_bucket = -1;
}
/* a clock drawn as separate hours and minutes (12-hour: AM/PM is dropped here; the date line keeps the day) */
static void split_clock(const char *t, lv_obj_t *hl, lv_obj_t *ml){
    if(!hl || !ml) return;
    char hh[8] = "--", mm[16] = "--";
    const char *c = t ? strchr(t, ':') : NULL;
    if(c){ int n = (int)(c - t); if(n > 0 && n < 4){ memcpy(hh, t, n); hh[n] = 0; } lv_snprintf(mm, sizeof mm, "%s", c + 1); }
    char *sp = strchr(mm, ' '); if(sp) *sp = 0;
    lv_label_set_text(hl, hh);
    lv_label_set_text(ml, mm);
}
static lv_obj_t *g_stone_hh, *g_stone_mm, *g_stone_mer;
static void stone_clock(const char *t){
    split_clock(t, g_stone_hh, g_stone_mm);
    char hm[16], mer[8]; clock_parts(t, hm, sizeof hm, mer, sizeof mer);
    char low[8]; if(g_stone_mer) lv_label_set_text(g_stone_mer, theme_lower(low, sizeof low, mer));
}
static lv_obj_t *stone_clock_pill(lv_obj_t *root, lv_color_t bg, lv_color_t fg, int x, int y){
    lv_obj_t *p = lv_obj_create(root);
    lv_obj_remove_style_all(p);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(p, 130, 76);
    lv_obj_set_pos(p, x, y);
    stone_pill(p, bg);
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, TF(CLOCK), 0);
    lv_obj_set_style_text_color(l, fg, 0);
    lv_obj_center(l);
    return l;
}
static void stone_home(const home_parts_t *p){
    own_backgrounds();
    lv_obj_set_pos(p->status, 0, 30);
    lv_obj_add_flag(p->clock, LV_OBJ_FLAG_HIDDEN);                /* the time is the two pills */
    /* the two pills together are centred (62..298); their vertical step is the design */
    g_stone_hh = stone_clock_pill(p->root, TC(SURFACE_RAISED), TC(TEXT_PRIMARY), 62, 56);
    g_stone_mm = stone_clock_pill(p->root, TC(ACCENT_PRIMARY), TC(ON_ACCENT), 168, 84);
    g_stone_mer = mer_label(p->root, TF(UI_16), TC(TEXT_SECONDARY));   /* "pm" just past the minutes pill */
    lv_obj_align_to(g_stone_mer, lv_obj_get_parent(g_stone_mm), LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -6);
    stone_clock(lv_label_get_text(p->clock));
    lv_obj_set_pos(p->date, 0, 168);
    lv_obj_set_style_text_color(p->date, TC(TEXT_SECONDARY), 0);
    lv_obj_set_pos(p->weather, 0, 188);
    /* Library + 8 + Search share the now-playing pill's edges (62..298) */
    lv_obj_set_pos(p->library, 62, 222); lv_obj_set_size(p->library, 186, 42);
    stone_pill(p->library, TC(SURFACE));
    home_library_fit(p->library);
    lv_obj_set_pos(p->search, 256, 222); lv_obj_set_size(p->search, 42, 42);
    stone_pill(p->search, TC(SURFACE));
    lv_obj_set_pos(p->np, 62, 274); lv_obj_set_size(p->np, 236, 48);
    stone_pill(p->np, TC(SURFACE));
    lv_obj_set_pos(p->np_thumb, 8, 6); lv_obj_set_size(p->np_thumb, 38, 38);
    lv_obj_set_pos(p->np_title, 54, 6);  lv_obj_set_width(p->np_title, 130);
    lv_obj_set_pos(p->np_artist, 54, 26); lv_obj_set_width(p->np_artist, 130);
    lv_obj_set_pos(p->pp, 184, 1); lv_obj_set_size(p->pp, 44, 44);   /* 8 px from the pill's end, as the art is from its start */
    lv_obj_set_style_bg_color(p->pp, TC(ACCENT_PRIMARY), 0);
    lv_obj_set_style_bg_opa(p->pp, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(p->pp_label, TC(ON_ACCENT), 0);
}
static void stone_np(const np_parts_t *p){
    own_backgrounds();
    lv_obj_set_style_arc_width(p->ring, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(p->ring, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(p->ring, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(p->ring, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(p->ring, TC(SURFACE_RAISED), LV_PART_MAIN);
    stone_progress_create(p->ring);
    lv_obj_set_style_bg_opa(p->cover, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(p->cover, stone_cover_draw, LV_EVENT_DRAW_MAIN_BEGIN, p->cover_note);
    lv_obj_set_style_pad_all(p->ring, 2, LV_PART_KNOB);
    lv_obj_set_style_radius(p->cover, 26, 0);
    lv_obj_set_size(p->pp, 64, 44);
    stone_pill(p->pp, TC(ACCENT_PRIMARY));
    ui_np_pp_filled(1);                                      /* accent changes recolour the fill, not the glyph */
    lv_obj_set_style_text_color(p->pp, TC(ON_ACCENT), 0);
    lv_obj_set_style_text_align(p->pp, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(p->pp, 5, 0);
    lv_obj_set_style_text_font(p->pp, TF(UI_28), 0);
    lv_obj_align(p->pp, LV_ALIGN_TOP_MID, 0, 266);
    lv_obj_set_ext_click_area(p->pp, 4);
    lv_obj_align(p->prev, LV_ALIGN_TOP_MID, -70, 274);
    lv_obj_align(p->next, LV_ALIGN_TOP_MID, 70, 274);
    np_sides_on_cover(p, 0);                                 /* times stay at 318: the play pill ends at 310 */
}
/* AirPlay receiving: a thick display-only track, the cover in a soft circle, the AIRPLAY chip, the mint play pill. */
static void stone_rx(const rx_parts_t *p, rx_state_t *s){
    if(s->first){
        lv_obj_set_style_arc_width(p->arc, 12, LV_PART_MAIN); lv_obj_set_style_arc_rounded(p->arc, true, LV_PART_MAIN);
        lv_obj_set_style_arc_color(p->arc, TC(SURFACE_RAISED), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(p->arc, LV_OPA_TRANSP, LV_PART_INDICATOR);   /* no progress: a round cap at 0 would be a dot */
        lv_obj_set_style_arc_width(p->arc, 12, LV_PART_INDICATOR); lv_obj_set_style_arc_rounded(p->arc, true, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(p->arc, TC(ACCENT_PRIMARY), LV_PART_INDICATOR);
        lv_obj_move_foreground(p->icon);                  /* the glyph sits on the quiet circle */
        lv_obj_set_width(p->text[0], LV_SIZE_CONTENT);
        rx_label(p->text[0], TF(UI_12), TC(ACCENT_PRIMARY)); lv_obj_set_style_text_letter_space(p->text[0], 1, 0);
        lv_obj_set_style_bg_color(p->text[0], TC(SURFACE), 0); lv_obj_set_style_bg_opa(p->text[0], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(p->text[0], LV_RADIUS_CIRCLE, 0); lv_obj_set_style_pad_hor(p->text[0], 12, 0);
        lv_obj_set_style_pad_ver(p->text[0], 1, 0);
        rx_label(p->text[1], TF(USER_20), TC(TEXT_PRIMARY)); rx_label(p->text[2], TF(USER_16), TC(TEXT_SECONDARY));
        rx_label(p->text[3], TF(USER_14), TC(TEXT_MUTED)); rx_label(p->text[4], TF(UI_12), TC(TEXT_MUTED));
        stone_pill(p->key[1], TC(ACCENT_PRIMARY));
        lv_obj_set_style_text_color(p->label[1], TC(ON_ACCENT), 0);
        return;
    }
    int wait = s->mode == RX_WAITING;
    rx_show(p->text[0], s->mode != RX_OTHER && !wait);   /* the mock-up's Waiting has the heading only */
    rx_ring(p, s); rx_show(p->bar, 0); rx_show(p->elapsed, 0); rx_show(p->remain, 0);   /* the ring is the progress; the times are text[4] */
    if(s->mode == RX_OTHER) return;
    if(s->show && s->mode == RX_RECEIVING) lv_snprintf(s->text[4], sizeof s->text[4], "%s / %s", s->elapsed, s->remain);
    s->frame = 1;                                         /* the cover circle, or the quiet one holding the glyph */
    rx_bg(p->frame, wait ? TC(SURFACE_RAISED) : TC(ART_PLACEHOLDER));
    if(lv_obj_get_style_radius(p->frame, 0) != LV_RADIUS_CIRCLE) lv_obj_set_style_radius(p->frame, LV_RADIUS_CIRCLE, 0);   /* the styling pass rounds it less */
    rx_place(p->text[0], LV_ALIGN_TOP_MID, 0, 22, 0, NULL);
    rx_rows(p, s, (const int[8]){ 194, 224, 242, 314, 96, 204, 232, 256 }, (const lv_font_t *const[3]){ TF(USER_20), TF(USER_16), TF(USER_20) });
    if(wait) rx_place(p->text[3], LV_ALIGN_TOP_MID, 0, 256, 270, NULL);   /* the hint on one line, as in the mock-up */
    rx_transport(p, 266, 44, 70);
    rx_box(p->key[1], 0, 266, 64, 44);
}
static void stone_qs_tile(lv_obj_t *c, lv_obj_t *g, lv_obj_t *cap, int on, int toggle){
    (void)toggle;
    lv_obj_set_size(c, 70, 50);
    stone_pill(c, on ? TC(ACCENT_PRIMARY) : TC(SURFACE));
    if(g) lv_obj_set_style_text_color(g, on ? TC(ON_ACCENT) : TC(TEXT_PRIMARY), 0);
    if(cap) lv_obj_set_style_text_color(cap, TC(TEXT_SECONDARY), 0);
}
/* the screensaver keeps Home's two-pill clock (in every style that shows a clock); the date and weather sit under it */
static lv_obj_t *g_stone_shh, *g_stone_smm, *g_stone_smer;
static lv_obj_t *g_stone_song_track, *g_stone_song_artist, *g_stone_song_pebble;
static void stone_song_draw(lv_event_t *e){
    if(!g_stone_song_track || !g_stone_song_artist ||
       lv_obj_has_flag(g_stone_song_track, LV_OBJ_FLAG_HIDDEN) ||
       lv_obj_has_flag(g_stone_song_artist, LV_OBJ_FLAG_HIDDEN)) return;
    lv_area_t a; lv_obj_get_coords(lv_event_get_target(e), &a);
    stone_blob(lv_event_get_layer(e), &a, TC(ACCENT_PRIMARY), LV_OPA_COVER);
}
static void stone_song_layout(const saver_parts_t *p){
    if(!p->track || !p->artist) return;
    int y = saver_song_y(p->date, p->style == 1 ? 278 : 270);
    if(p->weather && !lv_obj_has_flag(p->weather, LV_OBJ_FLAG_HIDDEN)){
        lv_obj_update_layout(p->weather);
        int below = lv_obj_get_y(p->weather) + lv_obj_get_height(p->weather) + 8;
        if(below > y) y = below;
    }
    lv_obj_align(p->track, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_update_layout(p->track);
    lv_obj_align(p->artist, LV_ALIGN_TOP_MID, 0, y + lv_obj_get_height(p->track) + 2);
    int width = saver_song_width(p->artist, 218);
    int pebble_fit = ui_round_width(lv_obj_get_y(p->artist) + lv_obj_get_height(p->artist) + 7, 10);
    if(pebble_fit < width) width = pebble_fit;
    if(width < 120) width = 120;
    lv_obj_set_width(p->track, width - 24);
    lv_obj_set_width(p->artist, width - 24);
    lv_obj_set_style_text_color(p->track, TC(ON_ACCENT), 0);
    lv_obj_set_style_text_color(p->artist, TC(ON_ACCENT), 0);
    g_stone_song_track = p->track; g_stone_song_artist = p->artist;
    if(!g_stone_song_pebble){
        g_stone_song_pebble = lv_obj_create(p->root);
        lv_obj_remove_style_all(g_stone_song_pebble);
        lv_obj_clear_flag(g_stone_song_pebble, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(g_stone_song_pebble, stone_song_draw, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
        lv_obj_move_to_index(g_stone_song_pebble, lv_obj_get_index(p->track));
    }
    lv_obj_set_size(g_stone_song_pebble, width, lv_obj_get_y(p->artist) + lv_obj_get_height(p->artist) - y + 14);
    lv_obj_align(g_stone_song_pebble, LV_ALIGN_TOP_MID, 0, y - 7);
}
static void stone_saver_clock(const char *t){
    split_clock(t, g_stone_shh, g_stone_smm);
    char hm[16], mer[8]; clock_parts(t, hm, sizeof hm, mer, sizeof mer);
    char low[8]; if(g_stone_smer) lv_label_set_text(g_stone_smer, theme_lower(low, sizeof low, mer));
}
static void stone_saver(const saver_parts_t *p){
    int shown = p->style != 1 && p->style != 4;               /* analog and vinyl have no digital clock */
    if(!g_stone_shh && shown){
        g_stone_shh = stone_clock_pill(p->root, TC(SURFACE_RAISED), TC(TEXT_PRIMARY), 62, 0);
        g_stone_smm = stone_clock_pill(p->root, TC(ACCENT_PRIMARY), TC(ON_ACCENT), 168, 0);
        g_stone_smer = mer_label(p->root, TF(UI_16), TC(TEXT_SECONDARY));
        stone_saver_clock(lv_label_get_text(p->clock));
    }
    if(!g_stone_shh){ stone_song_layout(p); return; }
    lv_obj_t *ph = lv_obj_get_parent(g_stone_shh), *pm = lv_obj_get_parent(g_stone_smm);
    if(!shown){ lv_obj_add_flag(ph, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(pm, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(g_stone_smer, LV_OBJ_FLAG_HIDDEN); stone_song_layout(p); return; }
    int y = p->style == 2 ? 112 : 78;                         /* Minimal: lower, nothing under the date */
    lv_obj_remove_flag(ph, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(pm, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(g_stone_smer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(ph, y); lv_obj_set_y(pm, y + 28);            /* Home's step between the pills */
    lv_obj_align_to(g_stone_smer, pm, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -6);
    lv_obj_add_flag(p->clock, LV_OBJ_FLAG_HIDDEN);
    if(p->date){ lv_obj_align(p->date, LV_ALIGN_TOP_MID, 0, y + 28 + 76 + 10); lv_obj_update_layout(p->date); }
    if(p->weather && p->date) lv_obj_align(p->weather, LV_ALIGN_TOP_MID, 0, lv_obj_get_y(p->date) + 26);
    stone_song_layout(p);
}
const theme_kit_t theme_kit_stone = { .id = "stone", .style = stone_style, .home = stone_home, .home_clock = stone_clock,
                                      .nowplaying = stone_np, .receiver = stone_rx, .qs_tile = stone_qs_tile, .saver = stone_saver,
                                      .saver_clock = stone_saver_clock, .np_progress = stone_progress,
                                      .empty_state = stone_empty, .np_cover_state = stone_cover };

/* ================================================================ bauhaus
 * Poster geometry: square blocks, bold upper-case type, the three primaries. Titles are solid caps of yellow, blue or
 * red across the top of the round screen; Home is two giant numerals with its controls as shapes (a yellow circle is
 * Library, a blue square is Search); Now Playing is split in half by a red rule. DOT_GRID is the ink colour. */
static lv_color_t bauh_cap_color(const char *title, lv_color_t *ink){
    unsigned h = 0; for(const char *c = title; c && *c; c++) h = h * 31 + (unsigned char)*c;
    switch(h % 3){
    case 0:  *ink = TC(DOT_GRID);  return TC(DECOR_2);         /* yellow, ink type */
    case 1:  *ink = TC(ON_ACCENT); return TC(DECOR_1);         /* blue, white type */
    default: *ink = TC(ON_ACCENT); return TC(ACCENT_PRIMARY);  /* red, white type */
    }
}
static void bauh_title(lv_obj_t *label, const char *text){
    lv_label_set_text(label, text);
    lv_color_t ink, cap = bauh_cap_color(text, &ink);
    lv_obj_set_style_bg_color(label, cap, 0);
    lv_obj_set_style_text_color(label, ink, 0);
}
static lv_obj_t *bauh_header(lv_obj_t *root, const char *title, lv_event_cb_t back_cb){
    lv_obj_t *t = kit_edge_header(root, title, back_cb);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    theme_title_text(t, title);
    return t;
}
static void bauh_block(lv_obj_t *o, lv_color_t bg){
    kit_keep(o);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
}
/* Reserve a fixed bullet slot. Only the leading content moves; right values and controls stay put. */
#define BAUH_ROW_MARK LV_OBJ_FLAG_USER_1
static void bauh_triangle(lv_event_t *e){
    lv_obj_t *o = lv_event_get_target(e); lv_area_t a; lv_obj_get_coords(o, &a);
    lv_draw_triangle_dsc_t d; lv_draw_triangle_dsc_init(&d);
    d.bg_color = lv_obj_get_style_bg_color(o, 0); d.bg_opa = LV_OPA_COVER;
    d.p[0] = (lv_point_precise_t){a.x2, a.y2};
    d.p[1] = (lv_point_precise_t){a.x1, a.y2};
    d.p[2] = (lv_point_precise_t){(a.x1 + a.x2)/2, a.y1};
    lv_draw_triangle(lv_event_get_layer(e), &d);
}
static lv_obj_t *bauh_shape(lv_obj_t *parent, int x, int y, int w, int h, int kind, lv_color_t color){
    lv_obj_t *o = lv_obj_create(parent); lv_obj_remove_style_all(o); kit_keep(o);
    lv_obj_set_pos(o, x, y); lv_obj_set_size(o, w, h);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(o, color, 0);
    if(kind == 2) lv_obj_add_event_cb(o, bauh_triangle, LV_EVENT_DRAW_MAIN_END, NULL);
    else { lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
           lv_obj_set_style_radius(o, kind == 0 ? LV_RADIUS_CIRCLE : 0, 0); }
    return o;
}
static void bauh_row_press(lv_event_t *e){
    lv_obj_t *row = lv_event_get_target(e);
    lv_obj_t *mark = lv_obj_get_child(row, lv_obj_get_child_count(row) - 1);
    int kind = (row_pool_phase ? row_pool_phase(row) : lv_obj_get_index(row)) % 3;
    lv_color_t color = kind == 0 ? TC(ACCENT_PRIMARY) : kind == 1 ? TC(DECOR_2) : TC(DECOR_1);
    lv_obj_set_style_bg_color(mark, lv_event_get_code(e) == LV_EVENT_PRESSED ? TC(DOT_GRID) : color, 0);
    lv_obj_invalidate(mark);
}
static void bauh_row_decor(lv_obj_t *row, lv_obj_t *title){
    if(!title || lv_obj_has_flag(row, BAUH_ROW_MARK)) return;
    kit_row_layout_t layout = kit_row_layout_get(row);
    const char *s = lv_label_get_text(title);
    if(!s || !*s) return;
    /* kit_pass laid out the screen once before styling. Use those coordinates:
     * flushing here repeats the entire growing list's flex layout for each row. */
    lv_area_t r, t; lv_obj_get_coords(row, &r); lv_obj_get_coords(title, &t);
    int inset = t.x1 - r.x1;
    if(lv_obj_get_width(row) < 260) return;
    if(layout == KIT_ROW_PLAIN && (inset < 11 || inset > 24 || (unsigned char)s[0] == 0xEF)) return;
    uint32_t count = lv_obj_get_child_count(row);
    for(uint32_t i = 0; i < count; i++){
        lv_obj_t *child = lv_obj_get_child(row, (int32_t)i);
        lv_area_t a; lv_obj_get_coords(child, &a);
        int x = a.x1 - r.x1;
        if(layout == KIT_ROW_ICON){
            if(i == 0) lv_obj_set_x(child, 24);
            else if(i == 1){ lv_obj_set_x(child, 50); lv_obj_set_width(child, lv_obj_get_width(child) - 10); }
        } else if(layout == KIT_ROW_SWATCH){
            if(i < 3) lv_obj_set_x(child, x + 6);
            else if(i == 3){
                lv_obj_set_x(child, x + 6);
                lv_obj_set_width(child, 148);
                lv_label_set_long_mode(child, LV_LABEL_LONG_DOT);
            }
        } else if(lv_obj_check_type(child, &lv_label_class) && x >= 11 && x <= 24){
            int shift = 24 - x;
            if(shift > 0){
                lv_obj_set_x(child, 24);
                if(lv_label_get_long_mode(child) != LV_LABEL_LONG_WRAP)
                    lv_obj_set_width(child, lv_obj_get_width(child) - shift);
            }
        }
    }
    int kind = (row_pool_phase ? row_pool_phase(row) : lv_obj_get_index(row)) % 3;
    lv_color_t c = kind == 0 ? TC(ACCENT_PRIMARY) : kind == 1 ? TC(DECOR_2) : TC(DECOR_1);
    lv_obj_t *mark = bauh_shape(row, 8, 0, 8, 8, kind, c);
    lv_obj_align(mark, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_add_event_cb(row, bauh_row_press, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(row, bauh_row_press, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(row, bauh_row_press, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_flag(row, BAUH_ROW_MARK);
}
static void bauh_style(lv_obj_t *o, kit_role_t r){
    switch(r){
    case KIT_ROW:
        bauh_block(o, TC(SURFACE));
        lv_obj_set_style_bg_color(o, TC(ACCENT_PRIMARY), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_STATE_PRESSED);
        break;
    /* row words keep their own case (titles are the capitals): upper-casing only the rows set in UI faces made
     * Settings shout while Library and the menus did not */
    case KIT_ROW_CHEVRON:
        lv_obj_set_style_text_color(o, TC(ACCENT_PRIMARY), 0);
        break;
    case KIT_BUTTON: case KIT_ICON_BUTTON:
        lv_obj_set_style_radius(o, r == KIT_ICON_BUTTON && lv_obj_get_width(o) < 70 ? LV_RADIUS_CIRCLE : 0, 0);
        lv_obj_set_style_border_width(o, 0, 0);
        break;
    case KIT_CARD:
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10) lv_obj_set_style_radius(o, 0, 0);
        break;
    case KIT_POPUP:
        lv_obj_set_style_radius(o, 0, 0);
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10){
            lv_obj_set_style_border_width(o, 3, 0);
            lv_obj_set_style_border_color(o, TC(TEXT_PRIMARY), 0);
            lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
        }
        break;
    case KIT_TOGGLE:
        lv_obj_set_style_radius(o, 0, LV_PART_MAIN); lv_obj_set_style_radius(o, 0, LV_PART_INDICATOR);
        lv_obj_set_style_radius(o, 0, LV_PART_KNOB);
        break;
    case KIT_SLIDER: case KIT_BAR:
        lv_obj_set_style_radius(o, 0, LV_PART_MAIN); lv_obj_set_style_radius(o, 0, LV_PART_INDICATOR);
        lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, LV_PART_KNOB);
        lv_obj_set_style_bg_color(o, TC(DECOR_2), LV_PART_KNOB);
        break;
    case KIT_ROLLER: case KIT_TEXTAREA:
        lv_obj_set_style_radius(o, 0, 0);
        lv_obj_set_style_radius(o, 0, LV_PART_SELECTED);
        break;
    case KIT_KEYBOARD:
        lv_obj_set_style_radius(o, 0, LV_PART_ITEMS);
        break;
    case KIT_TEXT:
        if(is_large_default_text(o)) lv_obj_set_style_text_font(o, TF(UI_24), 0);
        break;
    default: break;
    }
}
static lv_obj_t *g_bauh_hh, *g_bauh_mm, *g_bauh_mer;
static lv_obj_t *g_bauh_date, *g_bauh_wx;   /* the right column under the square */
static void bauh_clock(const char *t){
    split_clock(t, g_bauh_hh, g_bauh_mm);
    char hm[16], mer[8]; clock_parts(t, hm, sizeof hm, mer, sizeof mer);
    if(g_bauh_mer) lv_label_set_text(g_bauh_mer, mer);
    /* 12-hour: PM takes the top of the column (right under the square, which ends at 195), and the date and weather step
     * down to clear it (they collided by 7 px); the weather line keeps its full 20 px (a 28 px tap target with its reach) */
    if(g_bauh_mer) lv_obj_set_y(g_bauh_mer, mer[0] ? 196 : 200);
    if(g_bauh_date) lv_obj_set_y(g_bauh_date, mer[0] ? 222 : 218);
    if(g_bauh_wx) lv_obj_set_y(g_bauh_wx, mer[0] ? 240 : 236);
}
static lv_obj_t *bauh_numeral(lv_obj_t *root, lv_color_t c, int x, int y){
    lv_obj_t *l = lv_label_create(root);
    lv_obj_set_style_text_font(l, TF(CLOCK), 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_style_text_letter_space(l, -6, 0);
    lv_obj_set_pos(l, x, y);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}
static void bauh_home(const home_parts_t *p){
    own_backgrounds();
    lv_obj_set_pos(p->status, 0, 14);
    lv_obj_add_flag(p->clock, LV_OBJ_FLAG_HIDDEN);
    /* place the numerals by their ink, not their line box: the digit tops sit at y 40 and y 146 */
    const lv_font_t *f = TF(CLOCK);
    lv_font_glyph_dsc_t g; int top = 0;
    if(lv_font_get_glyph_dsc(f, &g, '3', 0))
        top = (lv_font_get_line_height(f) - f->base_line) - (g.ofs_y + g.box_h);   /* ink top inside the label box */
    g_bauh_hh = bauh_numeral(p->root, TC(TEXT_PRIMARY), 62, 44 - top);          /* inside the glass at its corner */
    /* each numeral sits against the round edge at its own height: the circle is wider where the minutes are */
    g_bauh_mm = bauh_numeral(p->root, TC(ACCENT_PRIMARY), 34, 146 - top);
    g_bauh_mer = mer_label(p->root, TF(DATE), TC(ACCENT_PRIMARY));             /* PM, beside the red minutes */
    lv_obj_set_width(g_bauh_mer, 100); lv_obj_set_style_text_align(g_bauh_mer, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(g_bauh_mer, 197, 200);                                       /* the right column: x 197..297, under the square */
    bauh_clock(lv_label_get_text(p->clock));
    /* Library: the yellow circle; Search: the blue square */
    lv_obj_set_pos(p->library, 226, 44); lv_obj_set_size(p->library, 84, 84);
    bauh_block(p->library, TC(DECOR_2));
    lv_obj_set_style_radius(p->library, LV_RADIUS_CIRCLE, 0);
    for(uint32_t i = 0; i < lv_obj_get_child_count(p->library); i++){
        lv_obj_t *c = lv_obj_get_child(p->library, (int32_t)i);
        if(!lv_obj_check_type(c, &lv_label_class)) continue;
        lv_obj_set_style_text_color(c, TC(DOT_GRID), 0);
        if(strlen(lv_label_get_text(c)) == 3) lv_obj_align(c, LV_ALIGN_CENTER, 0, -10);
        else { char up[24]; lv_label_set_text(c, theme_upper(up, sizeof up, lv_label_get_text(c)));
               lv_obj_set_style_text_font(c, TF(UI_14), 0); lv_obj_align(c, LV_ALIGN_CENTER, 0, 16); }
    }
    lv_obj_set_pos(p->search, 239, 140); lv_obj_set_size(p->search, 58, 58);   /* centred under the Library circle (268) */
    bauh_block(p->search, TC(DECOR_1));
    home_search_glyph(p->search, TC(ON_ACCENT));
    lv_obj_set_style_text_align(p->date, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(p->date, 197, 218); lv_obj_set_width(p->date, 100);         /* right edge = the square's (297) */
    g_bauh_date = p->date; g_bauh_wx = p->weather;                               /* bauh_clock moves them in 12-hour */
    lv_obj_set_style_text_font(p->date, TF(UI_16), 0);
    lv_obj_set_style_text_align(p->weather, LV_TEXT_ALIGN_RIGHT, 0);
    /* the column's own 100 px: wider would run under the minutes (a round "00" reaches x ~193 at these rows). A forecast
     * too long for it drops its icon first (home_set_weather), then ends in "..." */
    lv_obj_set_pos(p->weather, 197, 236); lv_obj_set_size(p->weather, 100, 20);
    lv_obj_set_ext_click_area(p->weather, 4);                                   /* tight column: no reach into the band */
    bauh_clock(lv_label_get_text(p->clock));                                     /* 12-hour: step the column clear of PM */
    /* now playing: an ink band with a red play disc */
    lv_obj_set_pos(p->np, 54, 262); lv_obj_set_size(p->np, 252, 46);   /* its corners inside the round glass */
    bauh_block(p->np, TC(TEXT_PRIMARY));
    home_set_art_enabled(0);
    lv_obj_add_flag(p->np_thumb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(p->np_title, 14, 4);  lv_obj_set_width(p->np_title, 180);
    lv_obj_set_pos(p->np_artist, 14, 24); lv_obj_set_width(p->np_artist, 180);
    lv_obj_set_style_text_color(p->np_title, TC(CANVAS), 0);
    lv_obj_set_style_text_color(p->np_artist, TC(CANVAS), 0);
    lv_obj_set_pos(p->pp, 202, 2); lv_obj_set_size(p->pp, 42, 42);   /* 8 px from the band's end */
    bauh_block(p->pp, TC(ACCENT_PRIMARY));
    lv_obj_set_style_radius(p->pp, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_text_color(p->pp_label, TC(ON_ACCENT), 0);
    lv_obj_add_flag(p->hint, LV_OBJ_FLAG_HIDDEN);
}
static lv_obj_t *g_bauh_triangle;
static void bauh_np(const np_parts_t *p){
    own_backgrounds();
    /* the split: the art block on the left, a red rule, everything else on the right; seek is a straight bar */
    lv_obj_add_flag(p->ring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(p->cover, 176, 360); lv_obj_align(p->cover, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_radius(p->cover, 0, 0);
    lv_obj_set_style_bg_color(p->cover, TC(DECOR_1), 0);
    lv_obj_align(p->cover_img, LV_ALIGN_CENTER, 0, 0);
    lv_image_set_scale(p->cover_img, 623); /* 148 px art fills the 360 px height; parent clips its blue half */
    lv_obj_clear_flag(p->cover, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    g_bauh_triangle = bauh_shape(p->cover, 27, 110, 122, 140, 2, TC(ACCENT_PRIMARY));
    lv_obj_t *rule = lv_obj_create(p->root);
    lv_obj_remove_style_all(rule);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(rule, 176, 0); lv_obj_set_size(rule, 5, 360);
    lv_obj_set_style_bg_color(rule, TC(ACCENT_PRIMARY), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_t *txt[3] = { p->title, p->artist, p->album };
    for(int i = 0; i < 3; i++){
        lv_obj_set_style_text_align(txt[i], LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_width(txt[i], 140);
        lv_obj_align(txt[i], LV_ALIGN_TOP_LEFT, 194, i == 0 ? 74 : 106 + i * 18);
    }
    /* the queue position gets its own line under the album (owner 2026-10-07: "Album - 1605/3285" was cut off) */
    lv_obj_remove_flag(p->pos, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_align(p->pos, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(p->pos, 140);
    lv_obj_align(p->pos, LV_ALIGN_TOP_LEFT, 194, 160);
    lv_obj_set_style_text_font(p->title, TF(UI_20), 0);
    lv_obj_set_width(p->title, 128);   /* a 2-line title starts at y 72, where the glass ends at x ~335: 194+128 fits */
    lv_label_set_long_mode(p->title, LV_LABEL_LONG_WRAP);
    /* the title stands on the artist: its last line ends at y 122 whether it takes one line or two (moved up 18 px
     * into the free top so the position fits on its own line) */
    lv_obj_set_height(p->title, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(p->title, 50, 0);
    lv_obj_align(p->title, LV_ALIGN_BOTTOM_LEFT, 194, -(360 - 122));
    lv_obj_align(p->prev, LV_ALIGN_TOP_LEFT, 196, 204);
    lv_obj_align(p->next, LV_ALIGN_TOP_LEFT, 300, 204);
    lv_obj_set_ext_click_area(p->prev, 8); lv_obj_set_ext_click_area(p->next, 8);   /* never into the play disc */
    lv_obj_set_ext_click_area(p->cover, 0);                                        /* the art stops at the rule */
    lv_obj_set_size(p->pp, 54, 54);
    bauh_block(p->pp, TC(ACCENT_PRIMARY));
    ui_np_pp_filled(1);
    lv_obj_set_style_radius(p->pp, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_text_color(p->pp, TC(ON_ACCENT), 0);
    lv_obj_set_style_text_align(p->pp, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(p->pp, 10, 0);
    lv_obj_align(p->pp, LV_ALIGN_TOP_LEFT, 234, 194);
    lv_obj_set_ext_click_area(p->pp, 4);
    lv_obj_t *bar = lv_bar_create(p->root);
    lv_obj_set_size(bar, 116, 8);
    lv_obj_set_pos(bar, 194, 264);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN); lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, TC(SURFACE_STRONG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, TC(ACCENT_PRIMARY), LV_PART_INDICATOR);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    ui_np_seek_line(bar);
    lv_obj_align(p->elapsed, LV_ALIGN_TOP_LEFT, 194, 276);
    lv_obj_set_style_text_align(p->elapsed, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(p->remain, LV_ALIGN_TOP_LEFT, 230, 276);
    lv_obj_set_style_text_align(p->remain, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_flag(p->sep, LV_OBJ_FLAG_HIDDEN);
    /* shuffle and heart (or moon) in the free top of the right column, fully on the glass */
    lv_obj_set_size(p->mode, 38, 36); lv_obj_align(p->mode, LV_ALIGN_TOP_LEFT, 186, 36);   /* y 36: the heart's box; three keys of 38 */
    lv_obj_set_size(p->fav, 38, 36);  lv_obj_align(p->fav, LV_ALIGN_TOP_LEFT, 227, 36);    /* with Beam (18) in the third, all of them  */
    lv_obj_set_size(p->sleep, 38, 36); lv_obj_align(p->sleep, LV_ALIGN_TOP_LEFT, 227, 36); /* inside the glass (the last ends at x 306) */
    lv_obj_set_size(p->beam, 38, 36); lv_obj_align(p->beam, LV_ALIGN_TOP_LEFT, 268, 36);
    lv_obj_add_flag(p->dots[0], LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(p->dots[1], LV_OBJ_FLAG_HIDDEN);
}
static void bauh_cover(lv_obj_t *cover, lv_obj_t *note, int show, int loading){
    if(!cover || !note) return;
    if(g_bauh_triangle){
        if(show && !loading) lv_obj_remove_flag(g_bauh_triangle, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(g_bauh_triangle, LV_OBJ_FLAG_HIDDEN);
    }
    if(show && !loading) lv_obj_add_flag(note, LV_OBJ_FLAG_HIDDEN);
    else if(show){ lv_obj_remove_flag(note, LV_OBJ_FLAG_HIDDEN); lv_obj_center(note); }
}
/* AirPlay receiving: the split kept. Art half on the left (blue field + red triangle while waiting), the red rule, the
 * column on the right with AIRPLAY where the shuffle/heart row is, a red play disc, an empty bar. */
static lv_obj_t *g_rx_bauh_rule, *g_rx_bauh_bar, *g_rx_bauh_tri;
static int g_rx_bauh_own;
static void bauh_rx(const rx_parts_t *p, rx_state_t *s){
    if(s->first){
        g_rx_bauh_own = 0;
        lv_obj_set_style_arc_opa(p->arc, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_radius(p->frame, 0, 0);
        lv_image_set_scale(p->art, 623);   /* the 148 px art fills the 360 px height; the frame clips its right part */
        g_rx_bauh_tri = bauh_shape(p->frame, 28, 110, 120, 140, 2, TC(ACCENT_PRIMARY));   /* 2 px narrower than Now Playing's, so the two stay tellable apart */
        g_rx_bauh_rule = bauh_shape(p->root, 176, 0, 5, 360, 1, TC(ACCENT_PRIMARY));
        g_rx_bauh_bar = bauh_shape(p->root, 194, 264, 116, 8, 1, TC(SURFACE_STRONG));
        rx_label(p->text[0], TF(UI_12), TC(ACCENT_PRIMARY)); lv_obj_set_style_text_letter_space(p->text[0], 2, 0);
        rx_label(p->text[1], TF(UI_20), TC(TEXT_PRIMARY)); rx_label(p->text[2], TF(USER_16), TC(TEXT_SECONDARY));
        rx_label(p->text[3], TF(USER_14), TC(TEXT_MUTED)); rx_label(p->text[4], TF(UI_12), TC(TEXT_MUTED));
        bauh_block(p->key[1], TC(ACCENT_PRIMARY)); lv_obj_set_style_radius(p->key[1], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_text_color(p->label[1], TC(ON_ACCENT), 0);
        rx_times_style(p, TF(UI_12), TC(TEXT_MUTED)); lv_obj_move_foreground(p->bar);   /* above the track */
        return;
    }
    int rx = s->mode == RX_RECEIVING || s->mode == RX_PAUSED, wait = s->mode == RX_WAITING;
    rx_show(g_rx_bauh_rule, rx || wait); rx_show(g_rx_bauh_bar, rx); rx_show(g_rx_bauh_tri, wait);
    if(s->mode == RX_OTHER){ rx_show(p->bar, 0); rx_show(p->elapsed, 0); rx_show(p->remain, 0); rx_recenter(p, &g_rx_bauh_own); return; }
    g_rx_bauh_own = 1;
    s->frame = 1; s->icon = 0;                            /* the triangle is the waiting picture */
    rx_bg(p->frame, wait ? TC(DECOR_1) : TC(ART_PLACEHOLDER));
    rx_frame(p->frame, 0, 0, 176, 360);
    for(int i = 0; i < 5; i++) if(lv_obj_get_style_text_align(p->text[i], 0) != LV_TEXT_ALIGN_LEFT) lv_obj_set_style_text_align(p->text[i], LV_TEXT_ALIGN_LEFT, 0);
    lv_opa_t dim = s->mode == RX_PAUSED ? LV_OPA_50 : LV_OPA_COVER;
    if(lv_obj_get_style_opa(g_rx_bauh_bar, 0) != dim) lv_obj_set_style_opa(g_rx_bauh_bar, dim, 0);
    if(lv_obj_get_style_opa(p->bar, 0) != dim) lv_obj_set_style_opa(p->bar, dim, 0);
    rx_fill(p, s, 194, 264, 116, 8, TC(ACCENT_PRIMARY)); rx_times(p, s, 194, 276, 116);
    if(wait) lv_snprintf(s->text[0], sizeof s->text[0], "AIRPLAY");
    const lv_font_t *tf = wait || s->has_title ? TF(UI_20) : TF(USER_16);
    rx_place(p->text[0], LV_ALIGN_TOP_LEFT, 194, 34, 120, NULL);
    if(wait) rx_place(p->text[1], LV_ALIGN_TOP_LEFT, 194, 70, 120, tf);
    else rx_place(p->text[1], LV_ALIGN_BOTTOM_LEFT, 194, -238, 120, tf);   /* the title stands on the artist, one or two lines */
    int lh = indic_line_height(s->text[1],tf), cap = wait ? LV_COORD_MAX : 2 * lh;
    if(lv_obj_get_style_max_height(p->text[1], 0) != cap) lv_obj_set_style_max_height(p->text[1], cap, 0);
    if(wait) rx_wrap(p->text[1], 1);
    else{   /* one line or two, then an ellipsis; the box is exactly that tall (ui.c leaves a capped label alone) */
        lv_point_t sz; indic_text_get_size(&sz, s->text[1], tf, 0, 0, 120, LV_TEXT_FLAG_NONE);
        int h = sz.y > lh ? cap : lh;
        if(lv_label_get_long_mode(p->text[1]) != LV_LABEL_LONG_DOT) lv_label_set_long_mode(p->text[1], LV_LABEL_LONG_DOT);
        if(lv_obj_get_style_height(p->text[1], 0) != h) lv_obj_set_height(p->text[1], h);
    }
    rx_place(p->text[2], LV_ALIGN_TOP_LEFT, 194, wait ? 124 : 126, 120, NULL); rx_wrap(p->text[2], 0);
    rx_place(p->text[3], LV_ALIGN_TOP_LEFT, 194, 148, 118, NULL); rx_wrap(p->text[3], wait);
    rx_place(p->text[4], LV_ALIGN_TOP_LEFT, 194, 276, 116, NULL);
    rx_box(p->key[0], 32, 199, 44, 44); rx_box(p->key[1], 87, 194, 54, 54); rx_box(p->key[2], 142, 199, 44, 44);
}
static void bauh_empty_draw(lv_event_t *e){
    lv_obj_t *o = lv_event_get_target(e);
    const char *saved = lv_obj_get_user_data(o);
    if(!saved || strcmp(saved, lv_label_get_text(o))) return;
    lv_area_t a; lv_obj_get_coords(o, &a);
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_color = TC(DECOR_2); d.bg_opa = LV_OPA_COVER;
    lv_draw_rect(lv_event_get_layer(e), &d, &a);
}
static void bauh_empty_deleted(lv_event_t *e){ free(lv_obj_get_user_data(lv_event_get_target(e))); }
static void bauh_empty(kit_empty_kind_t kind, lv_obj_t *message){
    if(!message) return;
    lv_obj_set_style_text_font(message, TF(USER_16), 0);
    lv_obj_set_style_text_color(message, TC(DOT_GRID), 0);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_point_t sz;
    lv_text_get_size(&sz, lv_label_get_text(message), TF(USER_16), 0, 0, 240, LV_TEXT_FLAG_NONE);
    int w = sz.x + 28; if(w > 270) w = 270; if(w < 80) w = 80;
    lv_obj_set_width(message, w);
    lv_obj_set_height(message, LV_SIZE_CONTENT);
    lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_hor(message, 14, 0); lv_obj_set_style_pad_ver(message, 12, 0);
    lv_obj_set_style_translate_x(message, 0, 0); lv_obj_set_style_translate_y(message, 0, 0);
    lv_obj_update_layout(message);
    lv_area_t a; lv_obj_get_coords(message, &a);
    lv_obj_set_style_translate_x(message, 180 - (a.x1 + a.x2)/2, 0);
    lv_obj_set_style_translate_y(message, (kind == KIT_EMPTY_LIBRARY ? 205 : 180) - (a.y1 + a.y2)/2, 0);
    if(lv_obj_has_flag(message, LV_OBJ_FLAG_USER_2)) free(lv_obj_get_user_data(message));
    lv_obj_set_user_data(message, strdup(lv_label_get_text(message)));
    if(!lv_obj_has_flag(message, LV_OBJ_FLAG_USER_2)){
        lv_obj_add_flag(message, LV_OBJ_FLAG_USER_2);
        lv_obj_add_event_cb(message, bauh_empty_draw, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
        lv_obj_add_event_cb(message, bauh_empty_deleted, LV_EVENT_DELETE, NULL);
    }
}
static void bauh_qs_tile(lv_obj_t *c, lv_obj_t *g, lv_obj_t *cap, int on, int toggle){
    /* switches are squares, actions are circles; a lit switch is a solid red block */
    lv_obj_set_style_radius(c, toggle ? 0 : LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, on ? TC(ACCENT_PRIMARY) : TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, on ? 0 : 3, 0);
    lv_obj_set_style_border_color(c, TC(TEXT_PRIMARY), 0);
    lv_obj_set_style_border_opa(c, LV_OPA_COVER, 0);
    if(g) lv_obj_set_style_text_color(g, on ? TC(ON_ACCENT) : TC(TEXT_PRIMARY), 0);
    if(cap){ char up[32]; lv_label_set_text(cap, theme_upper(up, sizeof up, lv_label_get_text(cap)));
             lv_obj_set_style_text_color(cap, on ? TC(ACCENT_PRIMARY) : TC(TEXT_SECONDARY), 0); }
}
static void bauh_saver(const saver_parts_t *p){
    if(p->clock) lv_obj_set_style_text_color(p->clock, TC(ACCENT_PRIMARY), 0);
    if(!p->track || !p->artist) return;
    int panel_y = 270;
    if(p->date && !lv_obj_has_flag(p->date, LV_OBJ_FLAG_HIDDEN)){
        lv_obj_update_layout(p->date);
        int below_date = lv_obj_get_y(p->date) + lv_obj_get_height(p->date) + 3;
        if(below_date > panel_y) panel_y = below_date;
    }
    lv_obj_t *panel = lv_obj_get_parent(p->track);
    if(panel == p->root){
        lv_obj_t *bar = bauh_shape(p->root, 24, 95, 26, 135, 1, TC(DECOR_1));
        lv_obj_move_background(bar);
        panel = bauh_shape(p->root, 84, panel_y, 192, 52, 1, TC(DECOR_2));
        bauh_shape(panel, 0, 0, 40, 5, 1, TC(DECOR_1));
        bauh_shape(panel, 180, 0, 12, 12, 0, TC(ACCENT_PRIMARY));
        lv_obj_set_parent(p->track, panel); lv_obj_set_parent(p->artist, panel);
    }
    lv_obj_set_pos(panel, 84, panel_y);
    lv_obj_set_width(p->track, 176); lv_obj_set_width(p->artist, 176);
    lv_obj_set_style_text_font(p->track, TF(USER_16), 0);
    lv_obj_set_style_text_font(p->artist, TF(USER_14), 0);
    lv_obj_set_style_text_color(p->track, TC(DOT_GRID), 0);
    lv_obj_set_style_text_color(p->artist, TC(DOT_GRID), 0);
    lv_obj_align(p->track, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_align(p->artist, LV_ALIGN_TOP_MID, 0, 29);
    lv_obj_move_foreground(panel);
}
const theme_kit_t theme_kit_bauh = { .id = "bauhaus", .header = bauh_header, .style = bauh_style, .home = bauh_home,
                                     .home_clock = bauh_clock, .nowplaying = bauh_np, .receiver = bauh_rx, .qs_tile = bauh_qs_tile,
                                     .saver = bauh_saver, .title = bauh_title, .row_decor = bauh_row_decor,
                                     .np_cover_state = bauh_cover, .empty_state = bauh_empty,
                                     .saver_mer_apart = 2 };   /* the saver's PM beside the numerals, as on Home */

/* ================================================================ zine
 * A photocopied music zine: paper grain, typewriter text, glitch-face titles in magenta, a yellow highlighter
 * (DECOR_1) with ink type (DECOR_2) for the pressed row, stamped paper scraps for buttons and popups, the Home time as
 * cut-out digit tiles, the cover taped on. Paper is light in both variants (dark: a light scrap on black). */
static lv_color_t zine_paper(void){ return theme_variant() ? TC(SURFACE) : TC(TEXT_PRIMARY); }
static lv_color_t zine_ink(void){ return theme_variant() ? TC(TEXT_PRIMARY) : TC(CANVAS); }
static void zine_rect(lv_layer_t *layer, int x1, int y1, int x2, int y2, lv_color_t color){
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_color = color; d.bg_opa = LV_OPA_COVER;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(layer, &d, &a);
}
static void zine_title_draw(lv_event_t *e){
    lv_obj_t *o = lv_event_get_target(e);
    if(lv_obj_get_user_data(o) != (void *)&theme_title_tag && lv_obj_get_y(o) >= 100) return;
    lv_area_t a; lv_obj_get_coords(o, &a);
    lv_point_t text;
    if(!indic_label_cached_size(o, &text, 0)) lv_text_get_size(&text, lv_label_get_text(o), lv_obj_get_style_text_font(o, 0),
                     lv_obj_get_style_text_letter_space(o, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int w = text.x + 18;
    if(w > a.x2 - a.x1 - 2) w = a.x2 - a.x1 - 2;
    if(w < 28) w = 28;
    int x = (a.x1 + a.x2 - w)/2, y = a.y1 + 1;
    /* An offset at each end reads as hand-cut tape; all ink stays on top. */
    zine_rect(lv_event_get_layer(e), x, y + 2, x + w - 1, a.y2 - 2, TC(DECOR_1));
    zine_rect(lv_event_get_layer(e), x + 4, y, x + w - 5, a.y2, TC(DECOR_1));
}
static void zine_title(lv_obj_t *o, const char *text){ lv_label_set_text(o, text); }
static void zine_row_scroll(lv_event_t *e){
    lv_obj_t *row = lv_event_get_user_data(e);
    lv_obj_add_flag(row, LV_OBJ_FLAG_USER_1);
    lv_obj_invalidate(row);
}
static void zine_row_scroll_hooks(lv_obj_t *row, int on){
    for(lv_obj_t *p = row; p; p = lv_obj_get_parent(p)){
        if(!lv_obj_has_flag(p, LV_OBJ_FLAG_SCROLLABLE)) continue;
        lv_obj_remove_event_cb_with_user_data(p, zine_row_scroll, row);
        if(on) lv_obj_add_event_cb(p, zine_row_scroll, LV_EVENT_SCROLL_BEGIN, row);
    }
}
static void zine_row_event(lv_event_t *e){
    lv_obj_t *row = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    if(code != LV_EVENT_DELETE && code != LV_EVENT_PRESSED && code != LV_EVENT_RELEASED &&
       code != LV_EVENT_PRESS_LOST) return;
    if(code == LV_EVENT_DELETE || code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
        zine_row_scroll_hooks(row, 0);
    if(code == LV_EVENT_PRESSED){
        lv_obj_remove_flag(row, LV_OBJ_FLAG_USER_1);
        zine_row_scroll_hooks(row, 1);
    }
    if(code != LV_EVENT_DELETE) lv_obj_invalidate(row);
}
static void zine_row_draw(lv_event_t *e){
    lv_obj_t *row = lv_event_get_target(e), *title = lv_event_get_user_data(e);
    if(!title || !lv_obj_has_state(row, LV_STATE_PRESSED) || lv_obj_has_flag(row, LV_OBJ_FLAG_USER_1)) return;
    lv_area_t a, t; lv_obj_get_coords(row, &a); lv_obj_get_coords(title, &t);
    lv_point_t text;
    if(!indic_label_cached_size(title, &text, 0)) lv_text_get_size(&text, lv_label_get_text(title), lv_obj_get_style_text_font(title, 0),
                     lv_obj_get_style_text_letter_space(title, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int x1 = t.x1 + 2, x2 = x1 + (text.x < 116 ? text.x : 116);
    if(x2 > a.x2 - 16) x2 = a.x2 - 16;
    if(x2 > x1 + 8){
        int y = t.y2 - 2;
        lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
        d.color = TC(ACCENT_PRIMARY); d.opa = LV_OPA_COVER; d.width = 2;
        d.p1 = (lv_point_precise_t){x1, y}; d.p2 = (lv_point_precise_t){x1 + (x2-x1)*2/3, y + 1};
        lv_draw_line(lv_event_get_layer(e), &d);
        d.p1 = d.p2; d.p2 = (lv_point_precise_t){x2, y - 1};
        lv_draw_line(lv_event_get_layer(e), &d);
    }
}
static void zine_row_decor(lv_obj_t *row, lv_obj_t *title){
    if(!title) return;
    lv_obj_remove_event_cb(row, zine_row_event);
    lv_obj_remove_event_cb(row, zine_row_draw);
    lv_obj_add_event_cb(row, zine_row_event, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(row, zine_row_draw, LV_EVENT_DRAW_POST, title);
}
static void zine_ink_type(lv_obj_t *o);
static void zine_scrap(lv_obj_t *o, lv_color_t bg){      /* a stamped paper scrap with an ink edge */
    kit_keep(o);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 2, 0);
    lv_obj_set_style_border_color(o, TC(DECOR_2), 0);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_translate_x(o, 2, LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(o, 2, LV_STATE_PRESSED);
    zine_ink_type(o);
}
static void zine_ink_type(lv_obj_t *o){
    for(uint32_t i = 0; i < lv_obj_get_child_count(o); i++){       /* ink type on the scrap */
        lv_obj_t *c = lv_obj_get_child(o, (int32_t)i);
        if(ui_dialog_part_of(c) == UI_DLG_VIEWPORT || ui_dialog_part_of(c) == UI_DLG_CONTENT) zine_ink_type(c);   /* a dialog's body */
        else if(lv_obj_check_type(c, &lv_label_class)) lv_obj_set_style_text_color(c, zine_ink(), 0);
        /* drawn glyph marks (the playlist menu dots) are small filled objects: ink them too, or they vanish on paper */
        else if(lv_obj_check_type(c, &lv_obj_class) && lv_obj_get_style_width(c, 0) <= 6 && lv_obj_get_style_height(c, 0) <= 6
                && lv_obj_get_style_bg_opa(c, 0) > LV_OPA_50) lv_obj_set_style_bg_color(c, zine_ink(), 0);
    }
}
static void zine_row(lv_obj_t *o){
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_bg_color(o, TC(DECOR_1), LV_STATE_PRESSED);    /* the highlighter */
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_STATE_PRESSED);
    /* the row's text inherits the row's colour, so a pressed row turns its type to ink */
    lv_obj_set_style_text_color(o, TC(TEXT_PRIMARY), 0);
    lv_obj_set_style_text_color(o, TC(DECOR_2), LV_STATE_PRESSED);
    for(uint32_t i = 0; i < lv_obj_get_child_count(o); i++){
        lv_obj_t *c = lv_obj_get_child(o, (int32_t)i);
        if(lv_obj_check_type(c, &lv_label_class) && lv_color_eq(lv_obj_get_style_text_color(c, 0), TC(TEXT_PRIMARY)))
            lv_obj_remove_local_style_prop(c, LV_STYLE_TEXT_COLOR, 0);
    }
}
static void zine_style(lv_obj_t *o, kit_role_t r){
    if(ui_dialog_part_of(o) == UI_DLG_VIEWPORT)
        lv_obj_set_style_bg_color(o, zine_ink(), LV_PART_SCROLLBAR);
    switch(r){
    case KIT_TITLE:
        int header = lv_obj_get_user_data(o) == (void *)&theme_title_tag || lv_obj_get_y(o) < 100;
        lv_obj_set_style_text_color(o, header ? TC(DECOR_2) : TC(ACCENT_PRIMARY), 0);
        if(header){
            lv_obj_remove_event_cb(o, zine_title_draw);
            lv_obj_add_event_cb(o, zine_title_draw, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
        }
        break;
    case KIT_ROW: zine_row(o); break;
    /* row words keep their own case: lower-casing only some rows (menu words in UI faces, not library names in user
     * faces) read as a mistake, and it made "Auto (by IP)" into "auto (by ip)". The typewriter face is the voice. */
    case KIT_ROW_CHEVRON:
        lv_label_set_text(o, ">>");
        lv_obj_set_style_text_font(o, TF(UI_14), 0);
        lv_obj_set_style_text_color(o, TC(ACCENT_PRIMARY), 0);
        break;
    case KIT_BUTTON:
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10 && is_neutral_fill(o)) zine_scrap(o, zine_paper());
        else if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10){ lv_obj_set_style_radius(o, 0, 0); }
        break;
    case KIT_ICON_BUTTON:
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10 && is_neutral_fill(o)) zine_scrap(o, zine_paper());
        else lv_obj_set_style_radius(o, 0, 0);
        break;
    case KIT_CARD:
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10 && is_neutral_fill(o)){
            lv_obj_set_style_radius(o, 0, 0);
            lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_side(o, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_width(o, 1, 0);
            lv_obj_set_style_border_color(o, TC(BORDER_STRONG), 0);
            lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
        }
        break;
    case KIT_POPUP:
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10 && lv_obj_get_width(o) < 340) zine_scrap(o, zine_paper());
        break;
    case KIT_TOGGLE:
        lv_obj_set_style_radius(o, 0, LV_PART_MAIN); lv_obj_set_style_radius(o, 0, LV_PART_INDICATOR);
        lv_obj_set_style_radius(o, 0, LV_PART_KNOB);
        break;
    case KIT_SLIDER: case KIT_BAR:
        lv_obj_set_style_radius(o, 0, LV_PART_MAIN); lv_obj_set_style_radius(o, 0, LV_PART_INDICATOR);
        lv_obj_set_style_radius(o, 0, LV_PART_KNOB);
        lv_obj_set_style_bg_color(o, TC(DECOR_2), LV_PART_KNOB);
        lv_obj_set_style_border_width(o, 2, LV_PART_KNOB);
        lv_obj_set_style_border_color(o, TC(DECOR_1), LV_PART_KNOB);
        break;
    case KIT_TEXTAREA:
        /* Match the search scrap's paper/ink pair, including editing states. */
        lv_obj_set_style_bg_color(o, zine_paper(), 0);
        lv_obj_set_style_text_color(o, zine_ink(), 0);
        lv_obj_set_style_text_color(o, zine_ink(), LV_PART_TEXTAREA_PLACEHOLDER);
        lv_obj_set_style_border_color(o, zine_ink(), LV_PART_CURSOR);
        lv_obj_set_style_border_width(o, 2, LV_PART_CURSOR);
        lv_obj_set_style_border_side(o, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
        lv_obj_set_style_bg_color(o, zine_ink(), LV_PART_CURSOR);
        lv_obj_set_style_text_color(o, zine_paper(), LV_PART_CURSOR);
        /* fall through */
    case KIT_ROLLER:
        lv_obj_set_style_radius(o, 0, 0);
        lv_obj_set_style_radius(o, 0, LV_PART_SELECTED);
        lv_obj_set_style_bg_color(o, TC(DECOR_1), LV_PART_SELECTED);
        lv_obj_set_style_text_color(o, TC(DECOR_2), LV_PART_SELECTED);
        break;
    case KIT_KEYBOARD:
        lv_obj_set_style_radius(o, 0, LV_PART_ITEMS);
        lv_obj_set_style_bg_color(o, zine_paper(), LV_PART_ITEMS);
        lv_obj_set_style_text_color(o, zine_ink(), LV_PART_ITEMS);
        break;
    case KIT_TEXT:
        if(is_large_default_text(o)) lv_obj_set_style_text_font(o, TF(UI_24), 0);
        break;
    default: break;
    }
}
/* the Home time as cut-out tiles: one per digit, alternating paper / magenta / yellow, set a little off the line */
static lv_obj_t *g_zine_digit[4], *g_zine_colon, *g_zine_mer;
static void zine_clock(const char *t){
    if(!g_zine_digit[0]) return;
    char hm[16], mer[8]; clock_parts(t, hm, sizeof hm, mer, sizeof mer);
    char low[8]; lv_label_set_text(g_zine_mer, theme_lower(low, sizeof low, mer));
    if(mer[0]) lv_obj_clear_flag(g_zine_mer, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(g_zine_mer, LV_OBJ_FLAG_HIDDEN);
    t = hm;
    char d[4] = { '-', '-', '-', '-' };
    const char *c = t ? strchr(t, ':') : NULL;
    if(c){
        int n = (int)(c - t);
        d[0] = n >= 2 ? t[n - 2] : ' '; d[1] = n >= 1 ? t[n - 1] : '-';
        d[2] = c[1] ? c[1] : '-'; d[3] = c[1] && c[2] ? c[2] : '-';
    }
    static const int X[4] = { 48, 112, 192, 256 };
    int shift = d[0] == ' ' ? -32 : 0;                          /* a one-digit hour: the tiles close up, centred */
    for(int i = 0; i < 4; i++){
        char s[2] = { d[i], 0 };
        lv_label_set_text(g_zine_digit[i], s);
        lv_obj_t *tile = lv_obj_get_parent(g_zine_digit[i]);
        if(i == 0 && d[0] == ' ') lv_obj_add_flag(tile, LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(tile, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(tile, X[i] + shift);
    }
    lv_obj_set_x(g_zine_colon, 165 + shift);             /* its 30 px cell centred between the pairs */
    lv_obj_align_to(g_zine_mer, lv_obj_get_parent(g_zine_digit[3]), LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 4);
}
static void zine_home(const home_parts_t *p){
    own_backgrounds();
    lv_obj_set_pos(p->status, 0, 22);
    lv_obj_add_flag(p->clock, LV_OBJ_FLAG_HIDDEN);
    /* 264 px wide, centred: 8 px between a pair, 24 for the colon; low enough that no corner meets the round glass.
     * Mirrored about the centre; the jitter is vertical */
    static const int X[4] = { 48, 112, 192, 256 }, Y[4] = { 64, 58, 66, 60 };
    lv_color_t bg[4] = { zine_paper(), TC(ACCENT_PRIMARY), TC(DECOR_1), zine_paper() };
    lv_color_t fg[4] = { zine_ink(), TC(ON_ACCENT), TC(DECOR_2), zine_ink() };
    for(int i = 0; i < 4; i++){
        lv_obj_t *tile = lv_obj_create(p->root);
        lv_obj_remove_style_all(tile);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(tile, 56, 70);
        lv_obj_set_pos(tile, X[i], Y[i]);
        lv_obj_set_style_bg_color(tile, bg[i], 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        kit_keep(tile);
        g_zine_digit[i] = lv_label_create(tile);
        lv_obj_set_style_text_font(g_zine_digit[i], TF(CLOCK), 0);
        lv_obj_set_style_text_color(g_zine_digit[i], fg[i], 0);
        lv_obj_center(g_zine_digit[i]);
    }
    lv_obj_t *colon = lv_label_create(p->root);
    lv_label_set_text(colon, ":");
    lv_obj_set_style_text_font(colon, TF(CLOCK), 0);
    lv_obj_set_style_text_color(colon, TC(TEXT_PRIMARY), 0);
    lv_obj_set_pos(colon, 165, 68);
    g_zine_colon = colon;
    g_zine_mer = mer_label(p->root, TF(UI_12), TC(DECOR_2));
    lv_obj_set_style_bg_color(g_zine_mer, TC(ACCENT_PRIMARY), 0);           /* "pm" on a strip of magenta tape */
    lv_obj_set_style_bg_opa(g_zine_mer, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(g_zine_mer, 4, 0);
    zine_clock(lv_label_get_text(p->clock));
    /* the date on a highlighter stroke */
    lv_obj_set_style_bg_color(p->date, TC(DECOR_1), 0);
    lv_obj_set_style_bg_opa(p->date, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(p->date, TC(DECOR_2), 0);
    lv_obj_set_style_pad_hor(p->date, 14, 0);
    lv_obj_set_style_pad_ver(p->date, 2, 0);
    lv_obj_set_width(p->date, LV_SIZE_CONTENT);
    lv_obj_align(p->date, LV_ALIGN_TOP_MID, -6, 146);
    lv_obj_set_pos(p->weather, 0, 176);
    lv_obj_set_pos(p->library, 66, 210); lv_obj_set_size(p->library, 150, 42);
    zine_scrap(p->library, zine_paper());
    home_library_fit(p->library);
    lv_obj_set_pos(p->search, 232, 210); lv_obj_set_size(p->search, 56, 42);
    zine_scrap(p->search, TC(DECOR_1));
    home_search_glyph(p->search, TC(DECOR_2));
    /* now playing: a paper scrap with a strip of magenta tape */
    lv_obj_set_pos(p->np, 58, 266); lv_obj_set_size(p->np, 236, 44);
    zine_scrap(p->np, zine_paper());
    home_set_art_enabled(0);
    lv_obj_set_pos(p->np_thumb, 8, 12); lv_obj_set_size(p->np_thumb, 22, 20);
    lv_obj_set_style_radius(p->np_thumb, 0, 0);
    lv_obj_set_style_bg_opa(p->np_thumb, LV_OPA_TRANSP, 0);
    lv_label_set_text(p->np_glyph, ">>");
    lv_obj_set_style_text_font(p->np_glyph, TF(UI_14), 0);
    lv_obj_set_style_text_color(p->np_glyph, TC(ACCENT_PRIMARY), 0);
    lv_obj_center(p->np_glyph);
    lv_obj_set_pos(p->np_title, 34, 4);  lv_obj_set_width(p->np_title, 150);
    lv_obj_set_pos(p->np_artist, 34, 23); lv_obj_set_width(p->np_artist, 150);
    lv_obj_set_style_text_color(p->np_title, zine_ink(), 0);
    lv_obj_set_style_text_color(p->np_artist, zine_ink(), 0);
    lv_obj_set_pos(p->pp, 186, 0); lv_obj_set_size(p->pp, 42, 38);
    lv_obj_set_style_text_color(p->pp_label, TC(ACCENT_PRIMARY), 0);
    lv_obj_t *tape = lv_obj_create(p->root);
    lv_obj_remove_style_all(tape);
    lv_obj_clear_flag(tape, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(tape, 34, 10); lv_obj_set_pos(tape, 50, 260);
    lv_obj_set_style_bg_color(tape, TC(ACCENT_PRIMARY), 0);
    lv_obj_set_style_bg_opa(tape, LV_OPA_80, 0);
    lv_label_set_text(p->hint, ">>");
    lv_obj_set_style_text_font(p->hint, TF(UI_12), 0);
}
static lv_obj_t *zine_tape(lv_obj_t *root, int x, int y){
    lv_obj_t *t = lv_obj_create(root);
    lv_obj_remove_style_all(t);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(t, 40, 12); lv_obj_set_pos(t, x, y);
    lv_obj_set_style_bg_color(t, TC(DECOR_1), 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_90, 0);
    return t;
}
static void zine_np(const np_parts_t *p){
    own_backgrounds();
    lv_obj_add_flag(p->ring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_radius(p->cover, 0, 0);
    lv_obj_align(p->cover, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_update_layout(p->root);
    lv_area_t a; lv_obj_get_coords(p->cover, &a);
    zine_tape(p->root, a.x1 + 6, a.y1 - 6);                 /* taped on at two corners */
    zine_tape(p->root, a.x2 - 44, a.y2 - 6);
    lv_obj_set_style_text_font(p->title, TF(HEADER), 0);
    lv_obj_set_style_text_color(p->title, TC(ACCENT_PRIMARY), 0);
    lv_obj_align(p->title, LV_ALIGN_TOP_MID, 0, 190);
    lv_obj_align(p->artist, LV_ALIGN_TOP_MID, 0, 222);
    lv_obj_add_flag(p->album, LV_OBJ_FLAG_HIDDEN);          /* music albums stay on Song Info; book status uses the paper strip */
    /* Chapter status and the beam destination share Zine's paper strip at the cover's foot. */
    lv_obj_set_width(p->album, 208);
    lv_obj_set_style_text_color(p->album, zine_ink(), 0);
    lv_obj_set_style_pad_hor(p->album, 6, 0);
    lv_obj_set_style_text_align(p->album, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(p->album, zine_paper(), 0);
    lv_obj_set_style_bg_opa(p->album, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_ver(p->album, 2, 0);
    lv_obj_align(p->album, LV_ALIGN_TOP_MID, 0, 152);
    lv_obj_t *bar = lv_bar_create(p->root);                 /* the highlighter stroke is the seek bar */
    lv_obj_set_size(bar, 200, 10);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 246);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN); lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, TC(BORDER_STRONG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, TC(ACCENT_PRIMARY), LV_PART_INDICATOR);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    ui_np_seek_line(bar);
    lv_obj_align(p->elapsed, LV_ALIGN_TOP_LEFT, 80, 258);
    lv_obj_set_style_text_align(p->elapsed, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(p->remain, LV_ALIGN_TOP_RIGHT, -80, 258);
    lv_obj_set_style_text_align(p->remain, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_flag(p->sep, LV_OBJ_FLAG_HIDDEN);
    /* stamped keys: prev / play / next on paper, play in magenta */
    lv_obj_t *tr[3] = { p->prev, p->pp, p->next };
    for(int i = 0; i < 3; i++){
        lv_obj_set_style_text_font(tr[i], TF(UI_20), 0);
        lv_obj_set_size(tr[i], i == 1 ? 56 : 48, 44);
        lv_obj_set_style_text_align(tr[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(tr[i], 6, 0);                  /* 44 - 6 - 2 x 2 border = 34 >= the glyph face */
        zine_scrap(tr[i], i == 1 ? TC(ACCENT_PRIMARY) : zine_paper());
        lv_obj_set_style_text_color(tr[i], i == 1 ? TC(ON_ACCENT) : zine_ink(), 0);
        lv_obj_align(tr[i], LV_ALIGN_TOP_MID, (i - 1) * 66, 280);
        lv_obj_set_ext_click_area(tr[i], 4);
    }
    ui_np_pp_filled(1);
    /* shuffle / heart (or moon) out towards the rim, clear of the taped cover (cover 106..254) */
    lv_obj_set_size(p->mode, 40, 36); lv_obj_align(p->mode, LV_ALIGN_TOP_MID, -118, 136);
    lv_obj_set_size(p->fav, 40, 36);  lv_obj_align(p->fav, LV_ALIGN_TOP_MID, 118, 136);
    lv_obj_set_size(p->sleep, 40, 36); lv_obj_align(p->sleep, LV_ALIGN_TOP_MID, 118, 136);
    lv_obj_set_size(p->beam, 40, 36); lv_obj_align(p->beam, LV_ALIGN_TOP_MID, 118, 94);    /* 18: above the heart: below it the title runs */
    lv_obj_add_flag(p->dots[0], LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(p->dots[1], LV_OBJ_FLAG_HIDDEN);
}
/* AirPlay receiving: AIRPLAY on a strip of yellow tape, the cover taped on, the glitch title, an empty highlighter bar,
 * paper keys; Waiting is a taped note card holding the glyph. No album line (Zine's own Now Playing has none). */
static lv_obj_t *g_rx_zine_tape[2], *g_rx_zine_bar;
static int g_rx_zine_own;
static void zine_rx(const rx_parts_t *p, rx_state_t *s){
    if(s->first){
        g_rx_zine_own = 0;
        lv_obj_set_style_arc_opa(p->arc, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_radius(p->frame, 0, 0);
        lv_obj_move_foreground(p->icon);                  /* the glyph sits on the note card */
        for(int i = 0; i < 2; i++) g_rx_zine_tape[i] = zine_tape(p->root, 0, 0);
        g_rx_zine_bar = bauh_shape(p->root, 80, 246, 200, 10, 1, TC(BORDER_STRONG));
        lv_obj_set_width(p->text[0], LV_SIZE_CONTENT);
        rx_label(p->text[0], TF(UI_12), TC(DECOR_2)); lv_obj_set_style_text_letter_space(p->text[0], 2, 0);
        lv_obj_set_style_bg_color(p->text[0], TC(DECOR_1), 0); lv_obj_set_style_bg_opa(p->text[0], LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(p->text[0], 8, 0); lv_obj_set_style_pad_ver(p->text[0], 1, 0);
        lv_obj_set_style_transform_pivot_x(p->text[0], LV_PCT(50), 0); lv_obj_set_style_transform_pivot_y(p->text[0], LV_PCT(50), 0);
        lv_obj_set_style_transform_rotation(p->text[0], -20, 0);   /* stuck on a little crooked */
        rx_label(p->text[1], TF(HEADER), TC(ACCENT_PRIMARY)); rx_label(p->text[2], TF(USER_16), TC(TEXT_SECONDARY));
        rx_label(p->text[3], TF(USER_14), TC(TEXT_MUTED)); rx_label(p->text[4], TF(UI_12), TC(TEXT_SECONDARY));
        for(int i = 0; i < 3; i++){
            zine_scrap(p->key[i], i == 1 ? TC(ACCENT_PRIMARY) : zine_paper());
            lv_obj_set_style_text_font(p->label[i], TF(UI_20), 0);
        }
        lv_obj_set_style_text_color(p->label[1], TC(ON_ACCENT), 0);
        rx_times_style(p, TF(UI_12), TC(TEXT_SECONDARY)); lv_obj_move_foreground(p->bar);
        return;
    }
    int rx = s->mode == RX_RECEIVING || s->mode == RX_PAUSED, wait = s->mode == RX_WAITING;
    rx_show(p->text[0], rx || wait); rx_show(g_rx_zine_bar, rx);
    rx_show(g_rx_zine_tape[0], rx || wait); rx_show(g_rx_zine_tape[1], rx);
    if(s->mode == RX_OTHER){ rx_show(p->bar, 0); rx_show(p->elapsed, 0); rx_show(p->remain, 0); rx_recenter(p, &g_rx_zine_own); return; }
    g_rx_zine_own = 1;
    s->frame = 1;                                         /* the taped cover, or the note card */
    rx_show(p->text[3], wait);                            /* the album stays off the page */
    rx_bg(p->frame, wait ? zine_paper() : TC(ART_PLACEHOLDER));
    if(lv_obj_get_style_bg_opa(p->frame, 0) != LV_OPA_COVER) lv_obj_set_style_bg_opa(p->frame, LV_OPA_COVER, 0);   /* the styling pass clears it */
    if(!lv_color_eq(lv_obj_get_style_text_color(p->text[1], 0), TC(ACCENT_PRIMARY))) lv_obj_set_style_text_color(p->text[1], TC(ACCENT_PRIMARY), 0);
    if(lv_obj_get_style_border_width(p->frame, 0) != (wait ? 2 : 0)){
        lv_obj_set_style_border_width(p->frame, wait ? 2 : 0, 0); lv_obj_set_style_border_color(p->frame, TC(DECOR_2), 0);
    }
    for(int i = 0; i < 2; i++){                           /* the glyph is ink on the card */
        lv_obj_t *l = lv_obj_get_child(p->icon, i);
        lv_color_t c = wait ? zine_ink() : TC(ACCENT_PRIMARY);
        if(!lv_color_eq(lv_obj_get_style_line_color(l, 0), c)) lv_obj_set_style_line_color(l, c, 0);
    }
    if(wait){ lv_snprintf(s->text[0], sizeof s->text[0], "AIRPLAY"); theme_upper(s->text[1], sizeof s->text[1], s->text[1]); }
    rx_frame(p->frame, wait ? 130 : 106, wait ? 40 : 34, wait ? 100 : 148, wait ? 100 : 148);
    rx_place(g_rx_zine_tape[0], LV_ALIGN_TOP_LEFT, wait ? 136 : 112, wait ? 34 : 28, 0, NULL);
    rx_place(g_rx_zine_tape[1], LV_ALIGN_TOP_LEFT, 210, 176, 0, NULL);
    lv_opa_t dim = s->mode == RX_PAUSED ? LV_OPA_50 : LV_OPA_COVER;
    if(lv_obj_get_style_opa(g_rx_zine_bar, 0) != dim) lv_obj_set_style_opa(g_rx_zine_bar, dim, 0);
    if(lv_obj_get_style_opa(p->bar, 0) != dim) lv_obj_set_style_opa(p->bar, dim, 0);
    rx_fill(p, s, 80, 246, 200, 10, TC(ACCENT_PRIMARY)); rx_times(p, s, 80, 258, 200);
    rx_place(p->text[0], LV_ALIGN_TOP_MID, 0, 12, 0, NULL);
    rx_rows(p, s, (const int[8]){ 190, 222, 222, 258, 66, 154, 216, 238 }, (const lv_font_t *const[3]){ TF(HEADER), TF(USER_16), TF(HEADER) });
    for(int i = 0; i < 5; i++) if(lv_obj_get_style_text_align(p->text[i], 0) != (i == 4 ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER)) lv_obj_set_style_text_align(p->text[i], i == 4 ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
    rx_place(p->text[4], LV_ALIGN_TOP_LEFT, 80, 258, 200, NULL);
    rx_transport(p, 280, 44, 66);
    rx_box(p->key[1], 0, 280, 56, 44);
}
static void zine_qs_tile(lv_obj_t *c, lv_obj_t *g, lv_obj_t *cap, int on, int toggle){
    (void)toggle;
    zine_scrap(c, on ? TC(DECOR_1) : zine_paper());
    if(g) lv_obj_set_style_text_color(g, on ? TC(DECOR_2) : zine_ink(), 0);
    if(cap){ char low[32]; lv_label_set_text(cap, theme_lower(low, sizeof low, lv_label_get_text(cap)));
             /* on: the word gets a stroke of highlighter, like the date - readable on dark and on paper */
             lv_obj_set_style_text_color(cap, on ? TC(DECOR_2) : TC(TEXT_SECONDARY), 0);
             lv_obj_set_style_bg_color(cap, TC(DECOR_1), 0);
             lv_obj_set_style_bg_opa(cap, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
             lv_obj_set_style_pad_hor(cap, 3, 0); }
}
static void zine_empty_draw(lv_event_t *e){
    lv_obj_t *o = lv_event_get_target(e);
    const char *saved = lv_obj_get_user_data(o);
    if(!saved || strcmp(saved, lv_label_get_text(o))) return; /* Lyrics reuses its label. */
    lv_area_t a; lv_obj_get_coords(o, &a);
    lv_layer_t *layer = lv_event_get_layer(e);
    zine_rect(layer, a.x1 + 4, a.y1 + 5, a.x2, a.y2, TC(ACCENT_PRIMARY));
    zine_rect(layer, a.x1 + 2, a.y1 + 2, a.x2 - 5, a.y2 - 5, zine_paper());
    zine_rect(layer, a.x1 + 7, a.y1, a.x2 - 9, a.y2 - 3, zine_paper());
    zine_rect(layer, a.x1, a.y1 + 8, a.x2 - 4, a.y2 - 9, zine_paper());
}
static void zine_empty_deleted(lv_event_t *e){ free(lv_obj_get_user_data(lv_event_get_target(e))); }
static void zine_empty(kit_empty_kind_t kind, lv_obj_t *message){
    if(!message) return;
    lv_obj_set_style_text_font(message, TF(UI_14), 0);
    lv_obj_set_style_text_color(message, zine_ink(), 0);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
    lv_point_t sz;
    lv_text_get_size(&sz, lv_label_get_text(message), TF(UI_14), 0, 0, 246, LV_TEXT_FLAG_NONE);
    int w = sz.x + 40; if(w < 114) w = 114; if(w > 286) w = 286;
    lv_obj_set_width(message, w);
    lv_obj_set_height(message, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(message, 20, 0);
    lv_obj_set_style_pad_ver(message, 13, 0);
    lv_obj_set_style_translate_x(message, 0, 0);
    lv_obj_set_style_translate_y(message, 0, 0);
    lv_obj_update_layout(message);
    lv_area_t a; lv_obj_get_coords(message, &a);
    lv_obj_set_style_translate_x(message, 180 - (a.x1 + a.x2)/2, 0);
    stone_empty_at(message, kind == KIT_EMPTY_LYRICS ? 180 : stone_empty_center(message));
    lv_obj_remove_event_cb(message, zine_empty_draw);
    lv_obj_remove_event_cb(message, zine_empty_deleted);
    free(lv_obj_get_user_data(message));
    lv_obj_set_user_data(message, strdup(lv_label_get_text(message)));
    lv_obj_add_event_cb(message, zine_empty_draw, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
    lv_obj_add_event_cb(message, zine_empty_deleted, LV_EVENT_DELETE, NULL);
}
static void zine_cover_draw(lv_event_t *e){
    lv_obj_t *cover = lv_event_get_target(e);
    lv_obj_t *note = lv_event_get_user_data(e);
    if(!note || lv_obj_has_flag(note, LV_OBJ_FLAG_HIDDEN)) return;
    lv_area_t a; lv_obj_get_coords(cover, &a);
    lv_layer_t *layer = lv_event_get_layer(e);
    zine_rect(layer, a.x1 + 3, a.y1 + 1, a.x2 - 4, a.y2 - 2, zine_paper());
    zine_rect(layer, a.x1, a.y1 + 7, a.x2 - 2, a.y2 - 8, zine_paper());
    zine_rect(layer, a.x1 + 8, a.y1, a.x2 - 10, a.y2, zine_paper());
    zine_rect(layer, a.x1 + 5, a.y1 + 3, a.x2 - 7, a.y2 - 5, zine_paper());
}
static void zine_cover(lv_obj_t *cover, lv_obj_t *note, int show, int loading){
    if(!cover || !note) return;
    lv_obj_set_style_bg_opa(cover, LV_OPA_TRANSP, 0);
    lv_label_set_text(note, loading ? LV_SYMBOL_REFRESH : LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(note, TF(UI_32), 0);
    lv_obj_set_style_text_color(note, TC(ACCENT_PRIMARY), 0);
    if(show) lv_obj_center(note);
    lv_obj_remove_event_cb(cover, zine_cover_draw);
    lv_obj_add_event_cb(cover, zine_cover_draw, LV_EVENT_DRAW_MAIN_BEGIN, note);
}
static lv_obj_t *g_zine_song_tape;
void kit_zine_song_visibility(int show){
    if(g_zine_song_tape){
        if(show) lv_obj_remove_flag(g_zine_song_tape, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(g_zine_song_tape, LV_OBJ_FLAG_HIDDEN);
    }
}
static void zine_song_layout(const saver_parts_t *p){
    if(!p->track || !p->artist) return;
    int y = saver_song_y(p->date, p->style == 1 ? 278 : 270);
    if(p->weather && !lv_obj_has_flag(p->weather, LV_OBJ_FLAG_HIDDEN)){
        lv_obj_update_layout(p->weather);
        int below = lv_obj_get_y(p->weather) + lv_obj_get_height(p->weather) + 8;
        if(below > y) y = below;
    }
    y += 2; /* the tape starts two pixels above the song's text */
    /* one line each, so a long title ends in "..." on the tape instead of wrapping past it */
    int track_h = lv_font_get_line_height(lv_obj_get_style_text_font(p->track, 0));
    int artist_h = lv_font_get_line_height(lv_obj_get_style_text_font(p->artist, 0));
    lv_obj_set_height(p->track, track_h);
    lv_obj_set_height(p->artist, artist_h);
    int bottom = y + track_h + 2 + artist_h + 8;
    int dy = bottom - 180;
    int width = dy < 170 ? 2 * (int)sqrtf((float)(170*170 - dy*dy)) - 2 : 0;
    if(width > 204) width = 204;
    if(width < 70) width = 70;
    lv_obj_set_width(p->track, width - 20);
    lv_obj_set_width(p->artist, width - 20);
    lv_obj_align(p->track, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_align(p->artist, LV_ALIGN_TOP_MID, 0, y + track_h + 2);
    lv_obj_set_style_text_color(p->track, TC(DECOR_2), 0);
    lv_obj_set_style_text_color(p->artist, TC(DECOR_2), 0);
    if(!g_zine_song_tape){
        g_zine_song_tape = lv_obj_create(p->root);
        lv_obj_remove_style_all(g_zine_song_tape);
        lv_obj_clear_flag(g_zine_song_tape, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(g_zine_song_tape, TC(DECOR_1), 0);
        lv_obj_set_style_bg_opa(g_zine_song_tape, LV_OPA_COVER, 0);
        kit_keep(g_zine_song_tape);
        lv_obj_move_to_index(g_zine_song_tape, lv_obj_get_index(p->track));
    }
    lv_obj_set_size(g_zine_song_tape, width, bottom - y + 1);
    lv_obj_align(g_zine_song_tape, LV_ALIGN_TOP_MID, 0, y - 2);
}
static void zine_saver(const saver_parts_t *p){
    if(p->date){
        lv_obj_set_style_bg_color(p->date, TC(DECOR_1), 0);
        lv_obj_set_style_bg_opa(p->date, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(p->date, TC(DECOR_2), 0);
        lv_obj_set_style_pad_hor(p->date, 12, 0);
        lv_obj_set_width(p->date, LV_SIZE_CONTENT);              /* the stroke hugs the words, as on Home */
    }
    if(p->clock) lv_obj_set_style_text_color(p->clock, TC(ACCENT_PRIMARY), 0);
    zine_song_layout(p);
}
const theme_kit_t theme_kit_zine = { .id = "zine", .header = kit_edge_header, .style = zine_style, .home = zine_home,
                                     .home_clock = zine_clock, .nowplaying = zine_np, .receiver = zine_rx, .qs_tile = zine_qs_tile,
                                     .saver = zine_saver, .empty_state = zine_empty, .np_cover_state = zine_cover,
                                     .row_decor = zine_row_decor, .title = zine_title, .back_text = "<<" };

/* ================================================================ something
 * The owner's design (assets/reference/something/): dot-grid paper, dot-matrix titles, flat hairline rows with accent
 * chevrons, one red. The trait code (theme.c, home.c, ui.c, quicksettings.c) draws Home, Now Playing and Quick
 * Settings; this styler carries the same language to every other screen and popup. */
static void some_style(lv_obj_t *o, kit_role_t r){
    switch(r){
    case KIT_ROW: theme_list_row(o); break;
    case KIT_CARD:
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10 && is_neutral_fill(o)){   /* list cards: flat, a hairline */
            lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
            lv_obj_set_style_radius(o, 0, 0);
            lv_obj_set_style_border_side(o, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_width(o, 1, 0);
            lv_obj_set_style_border_color(o, TC(BORDER), 0);
            lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
        }
        break;
    case KIT_BUTTON:
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10 && is_neutral_fill(o)){     /* a hairline pill */
            lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(o, TC(SURFACE), 0);
            lv_obj_set_style_border_width(o, 1, 0);
            lv_obj_set_style_border_color(o, TC(BORDER_STRONG), 0);
            lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(o, TC(ACCENT_PRIMARY), LV_STATE_PRESSED);
        } else if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10) lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
        break;
    case KIT_POPUP:
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10 && lv_obj_get_width(o) < 340){
            lv_obj_set_style_radius(o, 24, 0);
            lv_obj_set_style_border_width(o, 1, 0);
            lv_obj_set_style_border_color(o, TC(BORDER_STRONG), 0);
            lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
        }
        break;
    case KIT_KEYBOARD:
        lv_obj_set_style_radius(o, 10, LV_PART_ITEMS);
        break;
    case KIT_TEXT:
        if(is_large_default_text(o)) lv_obj_set_style_text_font(o, TF(UI_24), 0);
        break;
    case KIT_ROW_CHEVRON:
        kit_some_dot_arrow(o, 0);
        break;
    default: break;
    }
}
void kit_some_dot_arrow(lv_obj_t *glyph, int left){
    if(!glyph || lv_obj_get_child_count(glyph)) return;
    static const uint8_t x_right[5] = {0, 5, 10, 5, 0};
    lv_obj_set_style_text_opa(glyph, LV_OPA_TRANSP, 0);
    for(int i = 0; i < 5; i++){
        lv_obj_t *dot = lv_obj_create(glyph);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 4, 4);
        lv_obj_set_pos(dot, left ? 10 - x_right[i] : x_right[i], i * 5);
        lv_obj_set_style_bg_color(dot, TC(ACCENT_PRIMARY), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
}
static void some_empty(kit_empty_kind_t kind, lv_obj_t *message){
    (void)kind;
    if(!message) return;
    lv_obj_set_style_text_font(message, TF(UI_16), 0);
    lv_obj_set_style_text_color(message, TC(TEXT_MUTED), 0);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_translate_y(message, 0, 0); /* repeated list/search refreshes must not shift the message */
    lv_obj_update_layout(message);
    lv_area_t a; lv_obj_get_coords(message, &a);
    lv_obj_set_style_translate_y(message, 180 - (a.y1 + a.y2) / 2, 0);
}
static void some_cover(lv_obj_t *cover, lv_obj_t *note, int show, int loading){
    if(!cover || !note) return;
    static lv_obj_t *pattern;
    static uint8_t pixels[140 * 140 * 4];
    if(!pattern){
        pattern = lv_canvas_create(cover);
        lv_canvas_set_buffer(pattern, pixels, 140, 140, LV_COLOR_FORMAT_ARGB8888);
        lv_obj_center(pattern);
        lv_obj_clear_flag(pattern, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        uint32_t dim = theme_rgb(THEME_CLR_BORDER), red = theme_rgb(THEME_CLR_ACCENT_PRIMARY);
        for(int y = 0; y < 14; y++) for(int x = 0; x < 14; x++){
            for(int yy = 0; yy < 2; yy++) for(int xx = 0; xx < 2; xx++){
                uint8_t *p = pixels + ((y * 10 + yy + 4) * 140 + x * 10 + xx + 4) * 4;
                p[0] = dim & 255; p[1] = dim >> 8 & 255; p[2] = dim >> 16 & 255; p[3] = 100;
            }
        }
        static const uint8_t note_xy[][2] = {
            {7,3},{8,3},{9,3},{7,4},{9,4},{7,5},{9,5},{7,6},{9,6},{7,7},{9,7},
            {6,8},{7,8},{8,8},{9,8},{5,9},{6,9},{8,9},{9,9},{5,10},{6,10},{8,10},{9,10}
        };
        for(unsigned i = 0; i < sizeof note_xy / sizeof note_xy[0]; i++){
            for(int yy = 0; yy < 5; yy++) for(int xx = 0; xx < 5; xx++){
                uint8_t *p = pixels + ((note_xy[i][1] * 8 + 14 + yy) * 140 + note_xy[i][0] * 8 + 14 + xx) * 4;
                p[0] = red & 255; p[1] = red >> 8 & 255; p[2] = red >> 16 & 255; p[3] = 255;
            }
        }
        lv_obj_invalidate(pattern);
    }
    lv_obj_set_style_radius(cover, 0, 0);
    if(show && !loading){ lv_obj_remove_flag(pattern, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(note, LV_OBJ_FLAG_HIDDEN); }
    else { lv_obj_add_flag(pattern, LV_OBJ_FLAG_HIDDEN); if(show) lv_obj_remove_flag(note, LV_OBJ_FLAG_HIDDEN); }
}
static void some_np(const np_parts_t *p){ np_sides_on_cover(p, 0); }   /* times stay: the play circle ends at 316 */
/* the screensaver keeps Home's block-matrix clock (in every style that shows a clock) */
static int g_some_smc = -1;
static void some_saver_clock(const char *t){ if(g_some_smc == 0) mclock_set(1, t); }
static void some_saver(const saver_parts_t *p){
    int song_y = saver_song_y(p->date, p->style == 1 ? 278 : 260);
    lv_obj_set_style_text_font(p->track, TF(DATE), 0);
    lv_obj_set_style_text_font(p->artist, TF(UI_16), 0);
    lv_obj_set_style_text_color(p->track, TC(TEXT_PRIMARY), 0);
    lv_obj_set_style_text_color(p->artist, TC(TEXT_MUTED), 0);
    lv_obj_set_width(p->track, 230); lv_obj_set_width(p->artist, 230);
    lv_obj_align(p->track, LV_ALIGN_TOP_MID, 0, song_y);
    lv_obj_align(p->artist, LV_ALIGN_TOP_MID, 0, song_y + 25);
    if(p->style == 1){
        int width = saver_song_width(p->artist, 230);
        lv_obj_set_width(p->track, width); lv_obj_set_width(p->artist, width);
    }
    int shown = p->style != 1 && p->style != 4;               /* analog and vinyl have no digital clock */
    if(g_some_smc < 0 && shown){ g_some_smc = mclock_new(1, p->root, 0); some_saver_clock(lv_label_get_text(p->clock)); }
    if(g_some_smc != 0) return;
    int y = p->style == 2 ? 120 : 92;                         /* Minimal: lower, nothing under the date */
    mclock_place(1, y, shown);
    if(!shown) return;
    lv_obj_add_flag(p->clock, LV_OBJ_FLAG_HIDDEN);
    if(p->date){ lv_obj_align(p->date, LV_ALIGN_TOP_MID, 0, y + 63 + 12); lv_obj_update_layout(p->date); }
    if(p->weather && p->date) lv_obj_align(p->weather, LV_ALIGN_TOP_MID, 0, lv_obj_get_y(p->date) + 28);
}
/* AirPlay receiving: the dotted ring as a display-only track, Doto title, the red filled play circle. */
#define SOME_RX_DOTS 128
static void some_rx(const rx_parts_t *p, rx_state_t *s){
    if(s->first){
        lv_obj_set_style_arc_opa(p->arc, LV_OPA_TRANSP, LV_PART_MAIN);
        for(int i = 0; i < SOME_RX_DOTS; i++){
            double a = 6.283185307179586 * i / SOME_RX_DOTS;
            lv_obj_set_style_radius(blue_line(p->root, 178 + (int)lround(174 * cos(a)), 178 + (int)lround(174 * sin(a)), 4, 4,
                                              TC(CONTROL_TRACK)), LV_RADIUS_CIRCLE, 0);
        }
        lv_obj_set_style_radius(p->frame, 0, 0);
        rx_label(p->text[0], TF(DATE), TC(ACCENT_PRIMARY)); rx_label(p->text[1], TF(HEADER), TC(TEXT_PRIMARY));
        rx_label(p->text[2], TF(USER_16), TC(TEXT_SECONDARY)); rx_label(p->text[3], TF(USER_14), TC(TEXT_MUTED));
        rx_label(p->text[4], TF(UI_12), TC(TEXT_MUTED));
        lv_obj_set_style_radius(p->key[1], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(p->key[1], TC(ACCENT_PRIMARY), 0);
        lv_obj_set_style_bg_opa(p->key[1], LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(p->label[1], TC(ON_ACCENT), 0);
        lv_obj_set_style_text_color(p->label[0], TC(TEXT_PRIMARY), 0);
        lv_obj_set_style_text_color(p->label[2], TC(TEXT_PRIMARY), 0);
        return;
    }
    if(s->mode == RX_OTHER) return;
    if(s->show && s->mode == RX_RECEIVING) lv_snprintf(s->text[4], sizeof s->text[4], "%s / %s", s->elapsed, s->remain);   /* "2:31 / -1:32" */
    if(s->mode == RX_WAITING) theme_upper(s->text[1], sizeof s->text[1], s->text[1]);   /* the theme's title case */
    rx_place(p->text[0], LV_ALIGN_TOP_MID, 0, 16, 200, NULL);
    rx_rows(p, s, (const int[8]){ 194, 224, 242, 328, 76, 138, 222, 246 }, (const lv_font_t *const[3]){ TF(HEADER), TF(USER_18), TF(HEADER) });
    rx_transport(p, 264, 52, 70);
}
const theme_kit_t theme_kit_something = { .id = "something", .style = some_style, .nowplaying = some_np, .receiver = some_rx,
                                          .empty_state = some_empty, .np_cover_state = some_cover,
                                          .saver = some_saver, .saver_clock = some_saver_clock };

void kit_row_decor_reset(lv_obj_t *row){
    while(lv_obj_remove_event_cb(row, blue_row_draw)) {}
    while(lv_obj_remove_event_cb(row, bauh_row_press)) {}
}
