/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "indic.h"
#include "screens.h"
#include "i18n.h"
#include "theme.h"
#include "theme_kit.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ui_label_input_changed(lv_obj_t *obj, const char *txt);   /* ui.c: a label's text only when its input changes */
int ui_label_set_changed(lv_obj_t *obj, const char *txt);

LV_FONT_DECLARE(font_weather16)
static lv_font_t s_wfont;   /* montserrat_14 with the weather-icon font as fallback */





static lv_obj_t *g_clock;
static int g_art_on = 1;         /* 0: the theme's Home shows no album art in the now-playing pill */
static lv_obj_t *g_clock_sub;
static lv_obj_t *g_home_bg;      /* full-screen blurred album backdrop (matches Now Playing) */
static lv_obj_t *g_home_scrim;   /* dark overlay over the backdrop so text stays readable */
static lv_obj_t *g_status;       /* top status row: wifi / bt / battery */
static lv_obj_t *g_weather;      /* weather line under the date */
static lv_obj_t *g_status_arc;
static lv_obj_t *g_np_capsule;
static lv_obj_t *g_np_thumb;
static lv_obj_t *g_np_art_img;
void home_set_art_enabled(int on){ g_art_on = on; if(!on && g_np_art_img) lv_obj_add_flag(g_np_art_img, LV_OBJ_FLAG_HIDDEN); }
static lv_obj_t *g_np_thumb_glyph;
static lv_obj_t *g_np_title;
static lv_obj_t *g_np_artist;
static lv_obj_t *g_np_state;
/* "something" theme (THEME_TRAIT_MATRIX_CLOCK): a block-matrix clock drawn into one canvas. Two can exist: Home's
 * (handle 0) and the screensaver's (handle 1); each has its own canvas, buffer and AM/PM label. */
typedef struct { lv_obj_t *canvas, *ampm, *colon[2]; uint8_t *buf; int y; int drawn; char time[16]; } mclock_t;   /* time: drawn text */
static mclock_t g_mc[2];
#define g_mclock (g_mc[0].canvas)
#define MC_CELL 9      /* 7 px block + 2 px gap */
#define MC_W   360     /* the full screen width, so the time is centred on the screen itself */
#define MC_H   63      /* 7 rows */
static const uint8_t MC_DIGIT[10][7] = {   /* 5x7, bit 4 = leftmost column */
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C} };
static void mc_block(uint8_t *buf, int px, int cy, uint32_t rgb){   /* a block at pixel column px, cell row cy */
    for(int y = 0; y < MC_CELL - 2; y++) for(int x = 0; x < MC_CELL - 2; x++){
        uint8_t *p = buf + ((cy * MC_CELL + y) * MC_W + px + x) * 4;
        p[0] = rgb & 255; p[1] = rgb >> 8 & 255; p[2] = rgb >> 16 & 255; p[3] = 255;
    }
}
/* draw "H:MM" / "HH:MM" centred on the screen. With AM/PM (ampm_w > 0: the label's width) the digits and the label
 * are centred together, and the label's x is returned through *ampm_x. */
static void mc_draw(mclock_t *m, const char *t, int ampm_w, int *ampm_x){
    if(!m->canvas || !m->buf) return;
    uint8_t *buf = m->buf;
    int d[4], n = 0;
    for(const char *c = t; *c && n < 4; c++) if(*c >= '0' && *c <= '9') d[n++] = *c - '0';
    memset(buf, 0, MC_W * MC_H * 4);
    if(n >= 3){
        int hd = n - 2;
        /* each digit is 5 blocks + 1 gap, the colon 3 blocks + 1 gap; the last digit has no trailing gap */
        int width = (hd * 6 + 4 + 2 * 6 - 1) * MC_CELL - 2;
        /* centre what is LIT, not the cell box: a "1" leaves its outer columns empty, which would push the time
         * off centre. lead/trail = empty columns at the far left of the first digit / far right of the last. */
        int lead = 5, trail = 5;
        for(int r = 0; r < 7; r++) for(int col = 0; col < 5; col++){
            if(MC_DIGIT[d[0]][r] >> (4 - col) & 1){ if(col < lead) lead = col; }
            if(MC_DIGIT[d[n - 1]][r] >> (4 - col) & 1){ if(4 - col < trail) trail = 4 - col; }
        }
        int ink = width - (lead + trail) * MC_CELL;
        int group = ink + (ampm_w > 0 ? 6 + ampm_w : 0);
        int x = (MC_W - group) / 2 - lead * MC_CELL;
        if(ampm_x) *ampm_x = x + width - trail * MC_CELL + 6;
        uint32_t fg = theme_rgb(THEME_CLR_TEXT_PRIMARY);
        for(int i = 0; i < n; i++){
            if(i == hd){                               /* the colon: two accent plus signs, one row apart */
                for(int k = 0; k < 2; k++) lv_obj_set_pos(m->colon[k], x, m->y + k * 4 * MC_CELL);
                x += 4 * MC_CELL;
            }
            for(int r = 0; r < 7; r++) for(int col = 0; col < 5; col++)
                if(MC_DIGIT[d[i]][r] >> (4 - col) & 1) mc_block(buf, x + col * MC_CELL, r, fg);
            x += 6 * MC_CELL;
        }
    }
    lv_obj_invalidate(m->canvas);
}
/* The colon is steady: it is shown exactly while its clock's canvas is shown (mclock_place sets both). This only
 * re-asserts that after navigation and backlight changes, and touches a flag only when it is wrong, so a settled
 * clock costs no invalidation. */
void mclock_sync(int backlight_on){
    (void)backlight_on;
    for(int h = 0; h < 2; h++){
        if(!g_mc[h].canvas) continue;
        int hide = lv_obj_has_flag(g_mc[h].canvas, LV_OBJ_FLAG_HIDDEN);
        for(int k = 0; k < 2; k++){
            if(lv_obj_has_flag(g_mc[h].colon[k], LV_OBJ_FLAG_HIDDEN) == hide) continue;
            if(hide) lv_obj_add_flag(g_mc[h].colon[k], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_remove_flag(g_mc[h].colon[k], LV_OBJ_FLAG_HIDDEN);
        }
    }
}
static void mc_colon_new(mclock_t *m, lv_obj_t *parent){
    static const uint8_t xy[5][2] = {{1,0},{0,1},{1,1},{2,1},{1,2}};
    for(int k = 0; k < 2; k++){
        lv_obj_t *group = lv_obj_create(parent);
        lv_obj_remove_style_all(group);
        lv_obj_set_size(group, 3 * MC_CELL, 3 * MC_CELL);
        lv_obj_clear_flag(group, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        m->colon[k] = group;
        for(int i = 0; i < 5; i++){
            lv_obj_t *dot = lv_obj_create(group);
            lv_obj_remove_style_all(dot);
            lv_obj_set_size(dot, MC_CELL - 2, MC_CELL - 2);
            lv_obj_set_pos(dot, xy[i][0] * MC_CELL, xy[i][1] * MC_CELL);
            lv_obj_set_style_bg_color(dot, TC(ACCENT_PRIMARY), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        }
    }
}
/* a matrix clock on parent at y (handle 0 = Home, 1 = screensaver); 0 on success */
int mclock_new(int h, lv_obj_t *parent, int y){
    if(h < 0 || h > 1 || g_mc[h].canvas) return g_mc[h].canvas ? 0 : -1;
    mclock_t *m = &g_mc[h];
    m->buf = calloc(1, MC_W * MC_H * 4);
    if(!m->buf) return -1;
    m->canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(m->canvas, m->buf, MC_W, MC_H, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_set_pos(m->canvas, 0, y);
    m->y = y;
    mc_colon_new(m, parent);
    m->ampm = lv_label_create(parent);
    lv_label_set_text(m->ampm, "");
    lv_obj_set_style_text_font(m->ampm, TF(UI_12), 0);
    lv_obj_set_style_text_color(m->ampm, TC(TEXT_SECONDARY), 0);
    return 0;
}
/* draw the time ("H:MM", "HH:MM", with " AM"/" PM" shown beside the digits) */
void mclock_set(int h, const char *time_text){
    if(h < 0 || h > 1 || !g_mc[h].canvas) return;
    mclock_t *m = &g_mc[h];
    const char *t = time_text ? time_text : "";
    if(m->drawn && !strcmp(m->time, t)) return;   /* the buffer, colon and AM/PM follow from the text alone */
    m->drawn = strlen(t) < sizeof m->time;
    if(m->drawn) strcpy(m->time, t);
    const char *a = time_text ? strpbrk(time_text, "AaPp") : NULL;
    const char *mer = (a && (a[1] == 'M' || a[1] == 'm')) ? (a[0] == 'A' || a[0] == 'a' ? "AM" : "PM") : "";
    int ax = 0;
    lv_label_set_text(m->ampm, mer);
    lv_obj_update_layout(m->ampm);
    mc_draw(m, time_text ? time_text : "", mer[0] ? lv_obj_get_width(m->ampm) : 0, &ax);
    lv_obj_set_pos(m->ampm, ax, m->y + MC_H - lv_obj_get_height(m->ampm) - 2);   /* on the clock's baseline */
}
void mclock_place(int h, int y, int shown){
    if(h < 0 || h > 1 || !g_mc[h].canvas) return;
    mclock_t *m = &g_mc[h];
    int old_y = m->y;
    m->y = y;
    lv_obj_set_y(m->canvas, y);
    lv_obj_set_y(m->ampm, y + MC_H - lv_obj_get_height(m->ampm) - 2);
    for(int k = 0; k < 2; k++){
        lv_obj_set_y(m->colon[k], lv_obj_get_style_y(m->colon[k], 0) + y - old_y);
        if(!shown) lv_obj_add_flag(m->colon[k], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(m->colon[k], LV_OBJ_FLAG_HIDDEN);
    }
    if(shown){ lv_obj_remove_flag(m->canvas, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(m->ampm, LV_OBJ_FLAG_HIDDEN); }
    else     { lv_obj_add_flag(m->canvas, LV_OBJ_FLAG_HIDDEN);    lv_obj_add_flag(m->ampm, LV_OBJ_FLAG_HIDDEN); }
}
#ifdef DISKOS_HOST
int mclock_host_colons_shown(int h){
    int n = 0;
    if(h >= 0 && h < 2 && g_mc[h].canvas) for(int k = 0; k < 2; k++) n += !lv_obj_has_flag(g_mc[h].colon[k], LV_OBJ_FLAG_HIDDEN);
    return n;
}
#endif
static void something_home(lv_obj_t *root, lv_obj_t *lib_pill, lv_obj_t *search_btn);
static home_settings_click_cb_t g_settings_cb;
static lv_color_t g_accent;
static int g_progress, g_has_track;

#define HOME_TAP_SLOP 16   /* a press that moved further was a swipe (Home -> Apps), not a tap */
static void nav_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (ui_press_travel() > HOME_TAP_SLOP) return;
    screen_show((int)(uintptr_t)lv_event_get_user_data(e));
}

/* Tap the home weather glance -> open the full weather app (the natural glance->detail flow, so the
 * app no longer needs a buried Apps entry). No-op when the line is empty (weather off). */
static void weather_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (ui_press_travel() > HOME_TAP_SLOP) return;   /* the line spans the screen: a swipe usually starts on it */
    const char *t = g_weather ? lv_label_get_text(g_weather) : NULL;
    if (t && t[0]) weather_app_open();
}

static void pp_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED){
        if(ui_press_travel() > HOME_TAP_SLOP) return;   /* a swipe across the key must not pause the music */
        if(ui_transport_command("0201000C0000") < 0) return;
        if (g_np_state) ui_pp_glyph(g_np_state, ui_pp_icon_playing(ui_transport_playing()));
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}

/* A wide rounded "pill" button: icon + label, left-aligned, vertically centered. */
static lv_obj_t *make_pill(lv_obj_t *parent, int x, int y, int w, int h,
                           const char *symbol, const char *text,
                           lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, h / 2, 0);
    lv_obj_set_style_bg_color(btn, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(btn, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, TC(OUTLINE_BRIGHT), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_10, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    ui_on(btn, cb, LV_EVENT_CLICKED, user_data, "home.library", UI_CORE);   /* the only pill: Library */

    lv_obj_t *ic = lv_label_create(btn);
    lv_label_set_text(ic, symbol);
    lv_obj_set_style_text_font(ic, TF(UI_24), 0);
    lv_obj_set_style_text_color(ic, TC(TEXT_PRIMARY), 0);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 30, 0);

    lv_obj_t *title = lv_label_create(btn);
    lv_label_set_text(title, text);
    lv_obj_set_style_text_font(title, TF(UI_18), 0);
    lv_obj_set_style_text_color(title, TC(TEXT_PRIMARY), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 76, 0);
    return btn;
}

void home_set_settings_click_cb(home_settings_click_cb_t cb)
{
    g_settings_cb = cb;
}

void home_set_clock(const char *time_text, const char *sub_text)
{
    /* minute-resolution text on a 10 s tick: a theme's clock drawing follows from the time text alone */
    if (ui_label_set_changed(g_clock, time_text ? time_text : "--:--") && theme_kit()->home_clock)
        theme_kit()->home_clock(time_text ? time_text : "--:--");
    if (ui_label_input_changed(g_clock_sub, sub_text)){
        theme_case_text(g_clock_sub, sub_text);            /* the theme's case (Default: as given) */
    }
    if (g_mclock) mclock_set(0, time_text);
}

void home_set_weather(const char *text)
{
    if (!g_weather) return;
    const char *t = text ? text : "";
    /* weather.c writes "<icon>  <temp>  <condition>". A line too narrow for all of it (a theme's side column) drops the
     * icon before it cuts the words: "5 C Overcast" reads, "5 C Over..." does not. The icon is one private-use glyph
     * (3-byte UTF-8, lead 0xEF). */
    if ((unsigned char)t[0] == 0xEF && t[1] && t[2]) {
        int32_t room = lv_obj_get_style_width(g_weather, 0);
        if (room > 0 && room != LV_SIZE_CONTENT && !LV_COORD_IS_PCT(room)) {
            room -= lv_obj_get_style_pad_left(g_weather, 0) + lv_obj_get_style_pad_right(g_weather, 0);
            lv_point_t sz;
            lv_text_get_size(&sz, t, lv_obj_get_style_text_font(g_weather, 0),
                             lv_obj_get_style_text_letter_space(g_weather, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            if (sz.x > room) { t += 3; while (*t == ' ') t++; }
        }
    }
    if (!strcmp(lv_label_get_text(g_weather), t)) return;   /* unchanged fallback: no redraw every poll */
    lv_label_set_text(g_weather, t);
}

/* Update the top status row. batt 0-100, charging/wifi/bt are booleans. */
void home_set_status(int batt, int charging, int wifi, int bt)
{
    if (!g_status) return;
    char buf[80]; char *p = buf; *p = 0;
    if (wifi) { p += sprintf(p, LV_SYMBOL_WIFI "  "); }
    if (bt)   { p += sprintf(p, LV_SYMBOL_BLUETOOTH "  "); }
    const char *bs = batt >= 90 ? LV_SYMBOL_BATTERY_FULL :
                     batt >= 65 ? LV_SYMBOL_BATTERY_3 :
                     batt >= 40 ? LV_SYMBOL_BATTERY_2 :
                     batt >= 15 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    /* while charging show just the bolt + % (the % gives the level); a bolt jammed against the
     * battery glyph looked mashed. Not charging -> the battery-level glyph. */
    if (batt >= 0) p += sprintf(p, "%s %d%%", charging ? LV_SYMBOL_CHARGE : bs, batt);
    ui_label_set_changed(g_status, buf);
}

/* Fit the now-playing text in its box, whatever the theme did to the box or the fonts: the title + artist pair is
 * centred vertically as one block (each line exactly its font's height), and both lines run up to the play key. Each
 * theme sets its own box height and faces, and fixed y offsets left taller faces sitting low (the owner's Disc, hi-fi:
 * the artist line touched the bottom edge) and stopped the title well short of the key. */
static void np_fit_run(void){
    lv_obj_update_layout(g_np_capsule);
    int32_t h = lv_obj_get_content_height(g_np_capsule);
    /* keep the theme's own line spacing (fonts carry different leading); move the pair so the space above the title
     * equals the space below the artist's line, and give each line at least its font's height (no clipped descenders) */
    int32_t lt = indic_line_height(lv_label_get_text(g_np_title), lv_obj_get_style_text_font(g_np_title, 0));
    int32_t la = indic_line_height(lv_label_get_text(g_np_artist), lv_obj_get_style_text_font(g_np_artist, 0));
    int32_t gap = lv_obj_get_y(g_np_artist) - lv_obj_get_y(g_np_title);
    if(gap < lt * 3 / 4 || gap > lt + 8) gap = lt;    /* a theme that stacked them oddly: plain line spacing */
    /* centre the INK, not the line boxes (those include descender room, which centred the text low): from the top of a
     * capital in the title to the artist's baseline */
    const lv_font_t *ft = lv_obj_get_style_text_font(g_np_title, 0), *fa = lv_obj_get_style_text_font(g_np_artist, 0);
    int32_t cap_top = 0;   /* line top -> top of 'H' in the title face */
    lv_font_glyph_dsc_t g;
    if(lv_font_get_glyph_dsc(ft, &g, 'H', 0)) cap_top = (lt - ft->base_line) - (g.ofs_y + g.box_h);
    int32_t a_base = la - fa->base_line;   /* line top -> baseline in the artist face */
    int32_t top = (h - (gap + a_base - cap_top)) / 2 - cap_top; if(top < 0) top = 0;
    lv_obj_set_y(g_np_title, top);
    lv_obj_set_y(g_np_artist, top + gap);
    if(lv_obj_get_height(g_np_title) < lt) lv_obj_set_height(g_np_title, lt);
    if(lv_obj_get_height(g_np_artist) < la) lv_obj_set_height(g_np_artist, la);
    lv_obj_t *pp = g_np_state ? lv_obj_get_parent(g_np_state) : NULL;
    if(pp && lv_obj_get_parent(pp) == g_np_capsule && !lv_obj_has_flag(pp, LV_OBJ_FLAG_HIDDEN)){
        int32_t w = lv_obj_get_x(pp) - 8 - lv_obj_get_x(g_np_title);
        if(w > 40){ lv_obj_set_width(g_np_title, w); lv_obj_set_width(g_np_artist, w); }
    }
}
/* What the fit reads, as requested (styles and flags): a value set after a layout pass is already here, while
 * coordinates still show the old one until the next refresh. New text refits as well (np_fit's force). */
#define NP_GEOM_N 17
static void np_geom(intptr_t g[4][NP_GEOM_N]){
    lv_obj_t *o[4] = { g_np_capsule, g_np_title, g_np_artist, g_np_state ? lv_obj_get_parent(g_np_state) : NULL };
    memset(g, 0, sizeof(intptr_t) * 4 * NP_GEOM_N);
    for(int i = 0; i < 4; i++){
        if(!o[i]) continue;
        intptr_t *v = g[i];
        v[0] = (intptr_t)lv_obj_get_parent(o[i]); v[1] = lv_obj_has_flag(o[i], LV_OBJ_FLAG_HIDDEN);
        v[2] = lv_obj_get_style_x(o[i], 0); v[3] = lv_obj_get_style_y(o[i], 0);
        v[4] = lv_obj_get_style_width(o[i], 0); v[5] = lv_obj_get_style_height(o[i], 0);
        v[6] = lv_obj_get_style_align(o[i], 0);
        v[7] = lv_obj_get_style_pad_left(o[i], 0); v[8] = lv_obj_get_style_pad_right(o[i], 0);
        v[9] = lv_obj_get_style_pad_top(o[i], 0); v[10] = lv_obj_get_style_pad_bottom(o[i], 0);
        v[11] = lv_obj_get_style_border_width(o[i], 0);
        v[12] = lv_obj_get_style_translate_x(o[i], 0); v[13] = lv_obj_get_style_translate_y(o[i], 0);
        v[14] = (intptr_t)lv_obj_get_style_text_font(o[i], 0);
        v[15] = lv_obj_get_style_text_letter_space(o[i], 0); v[16] = lv_obj_get_style_text_line_space(o[i], 0);
    }
}
/* The fit reads what it last set (the two lines' y and width), so a run that changed them runs once more; once a run
 * leaves them as it found them, the same geometry gives the same fit and an update (a position tick) skips it. */
static int g_np_fit_settled;
static intptr_t g_np_fit_geom[4][NP_GEOM_N];
static void np_fit(int force){
    if(!g_np_capsule || !g_np_title || !g_np_artist) return;
    intptr_t before[4][NP_GEOM_N];
    np_geom(before);
    if(!force && g_np_fit_settled && !memcmp(before, g_np_fit_geom, sizeof before)) return;
    np_fit_run();
    np_geom(g_np_fit_geom);
    g_np_fit_settled = !memcmp(before, g_np_fit_geom, sizeof before);
}
static void np_fit_async(void *p){ (void)p; np_fit(1); }

static int g_rx_owned;   /* a receiver drives the capsule: main.c's local pushes are ignored until it lets go */
static void now_playing_apply(const char *title, const char *artist, lv_color_t accent, bool playing)
{
    g_accent = accent;

    int changed = ui_label_set_changed(g_np_title, title ? title : tr("Not Playing"));
    changed |= ui_label_set_changed(g_np_artist, artist ? artist : tr("Library"));
    if (g_np_state) ui_pp_glyph(g_np_state, ui_pp_icon_playing(playing));
    np_fit(changed);

    if (g_np_thumb) lv_obj_set_style_bg_color(g_np_thumb, accent, 0);
    if (g_status_arc) lv_obj_set_style_arc_color(g_status_arc, accent, LV_PART_INDICATOR);
}

void home_set_now_playing(const char *title, const char *artist, lv_color_t accent, bool playing)
{
    if (!g_rx_owned) now_playing_apply(title, artist, accent, playing);
}

static void progress_apply(int progress, int have_track){
    g_progress = progress; g_has_track = have_track;
    theme_kit()->home_progress(g_np_capsule, progress, have_track, g_accent);
}
void home_set_progress(int progress, int have_track){ if (!g_rx_owned) progress_apply(progress, have_track); }

/* A receiver's title/artist/state, no ring (the local progress is frozen); home_release_receiver() hands the capsule back. */
void home_set_receiver(const char *title, const char *artist, lv_color_t accent, bool playing)
{
    g_rx_owned = 1;
    now_playing_apply(title, artist, accent, playing);
    progress_apply(0, 0);
}
void home_release_receiver(void){ g_rx_owned = 0; }

/* repaint the now-playing capsule with the live accent (called from ui.c apply_accent so
 * Home tracks the same colour as Now Playing - static or album-dynamic). */
void home_set_accent(lv_color_t accent)
{
    g_accent = accent;
    if (g_np_thumb) lv_obj_set_style_bg_color(g_np_thumb, accent, 0);
    if (g_status_arc) lv_obj_set_style_arc_color(g_status_arc, accent, LV_PART_INDICATOR);
    if(g_np_capsule) theme_kit()->home_progress(g_np_capsule, g_progress, g_has_track, accent);
}

/* full-screen blurred backdrop (the 360px gblur'd cover), or NULL to clear -> black */
static int g_home_bg_on = 1;     /* 0: the theme keeps its own background behind Home */
void home_set_backdrop_enabled(int on){ g_home_bg_on = on; if (!on) home_set_backdrop(NULL); }
void home_set_backdrop(const char *src)
{
    if (!g_home_bg || !g_home_scrim) return;
    if (src && g_home_bg_on) {
        lv_image_set_src(g_home_bg, src);
        lv_obj_clear_flag(g_home_bg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_home_scrim, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_home_bg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_home_scrim, LV_OBJ_FLAG_HIDDEN);
    }
}

void home_set_art_src(const void *src)
{
    if (!g_np_art_img || !g_np_thumb_glyph) return;
    if (g_mclock || !g_art_on) return;                     /* the theme's pill shows no art (something: ring + waveform) */

    if (src) {
        /* src is a native-size 42px thumb (decoded by ui.c) - display 1:1, no
         * runtime scaling. The 42px circular thumb clips it to a disc. */
        lv_image_set_src(g_np_art_img, src);
        lv_obj_center(g_np_art_img);
        lv_obj_clear_flag(g_np_art_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_np_thumb_glyph, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_np_art_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_np_thumb_glyph, LV_OBJ_FLAG_HIDDEN);
    }
}

void home_create(lv_obj_t *root)
{
    if (theme_screen_plain(THEME_PLAIN_HOME)) theme_screen_solid(root);   /* the theme draws Home plain */
    g_accent = TC(ACCENT_EMPHASIS);

    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    /* full-screen blurred album backdrop (same image Now Playing uses), behind everything;
     * a dark scrim over it keeps the clock/text readable. Both created first = lowest z. */
    g_home_bg = lv_image_create(root);
    lv_obj_remove_style_all(g_home_bg);
    lv_obj_set_pos(g_home_bg, 0, 0); lv_obj_set_size(g_home_bg, 360, 360);
    lv_obj_clear_flag(g_home_bg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(g_home_bg, LV_OBJ_FLAG_HIDDEN);
    g_home_scrim = lv_obj_create(root);
    lv_obj_remove_style_all(g_home_scrim);
    lv_obj_set_pos(g_home_scrim, 0, 0); lv_obj_set_size(g_home_scrim, 360, 360);
    lv_obj_set_style_bg_color(g_home_scrim, TC(IMAGE_TINT), 0);   /* readability veil over the art */
    lv_obj_set_style_bg_opa(g_home_scrim, LV_OPA_50, 0);
    lv_obj_clear_flag(g_home_scrim, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(g_home_scrim, LV_OBJ_FLAG_HIDDEN);

    g_status_arc = NULL;   /* decorative status arc removed (read as a stray progress line) */

    /* Top status row: wifi / bt / battery (populated by main.c poll). */
    g_status = make_label(root, "", TF(UI_14), TC(TEXT_SECONDARY));
    lv_obj_set_pos(g_status, 0, 18);
    lv_obj_set_width(g_status, 360);

    g_clock = make_label(root, "--:--", TF(CLOCK),
                         TC(TEXT_PRIMARY));
    lv_obj_set_pos(g_clock, 0, 52);
    lv_obj_set_width(g_clock, 360);

    g_clock_sub = make_label(root, "diskOS", TF(UI_14),
                             TC(TEXT_MUTED));
    lv_obj_set_pos(g_clock_sub, 0, 88);
    lv_obj_set_width(g_clock_sub, 360);

    /* Weather line (populated by weather.c via wttr.in). Uses montserrat_14 with
     * the FA weather-icon font chained as fallback so the icon glyph renders. */
    s_wfont = *TF(UI_14);
    s_wfont.fallback = &font_weather16;
    g_weather = make_label(root, "", &s_wfont, TC(TEXT_SECONDARY));
    lv_obj_set_pos(g_weather, 0, 120);
    lv_obj_set_width(g_weather, 360);
    lv_label_set_long_mode(g_weather, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(g_weather, LV_OBJ_FLAG_CLICKABLE);       /* glance -> tap opens the weather app */
    lv_obj_set_ext_click_area(g_weather, 12);                /* thin line: enlarge the touch target */
    ui_on(g_weather, weather_event_cb, LV_EVENT_CLICKED, NULL, "home.weather", UI_CORE);

    /* search button (top-right) with a drawn magnifier glyph */
    lv_obj_t *sbtn = lv_button_create(root);
    lv_obj_remove_style_all(sbtn);
    lv_obj_set_pos(sbtn, 244, 40);         /* nudged in from the round top-right bezel */
    lv_obj_set_size(sbtn, 44, 44);
    lv_obj_set_ext_click_area(sbtn, 8);
    lv_obj_set_style_radius(sbtn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(sbtn, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(sbtn, LV_OPA_COVER, LV_STATE_PRESSED);
    ui_on(sbtn, nav_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)SCR_SEARCH, "home.search", UI_CORE);
    lv_obj_t *ring = lv_obj_create(sbtn);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, 16, 16);
    lv_obj_set_pos(ring, 11, 9);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, TC(TEXT_SECONDARY), 0);
    lv_obj_t *handle = lv_obj_create(sbtn);
    lv_obj_remove_style_all(handle);
    lv_obj_set_size(handle, 8, 2);
    lv_obj_set_pos(handle, 25, 25);
    lv_obj_set_style_bg_color(handle, TC(TEXT_SECONDARY), 0);
    lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_rotation(handle, 450, 0);
    /* decorative: a tap on the glass itself must reach the button (a clickable child swallowed it - only the
     * button's edge opened Search) */
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(handle, LV_OBJ_FLAG_CLICKABLE);

    /* Library is the primary action: a full-width pill. Apps + Settings live on
     * the swipe-left panel (SCR_APPS). */
    lv_obj_t *lib_pill = make_pill(root, 80, 162, 200, 52, LV_SYMBOL_DIRECTORY, tr("Library"),
              nav_event_cb, (void *)(uintptr_t)SCR_LIBRARY);

    /* subtle hint that swiping left reveals more */
    lv_obj_t *hint = make_label(root, LV_SYMBOL_RIGHT, TF(UI_14),
                                TC(TEXT_HINT));
    lv_obj_align(hint, LV_ALIGN_RIGHT_MID, -6, 0);

    g_np_capsule = lv_button_create(root);
    lv_obj_remove_style_all(g_np_capsule);
    lv_obj_set_pos(g_np_capsule, 48, 246);
    lv_obj_set_size(g_np_capsule, 264, 58);
    lv_obj_set_style_radius(g_np_capsule, 29, 0);
    lv_obj_set_style_bg_color(g_np_capsule, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(g_np_capsule, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(g_np_capsule, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(g_np_capsule, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(g_np_capsule, TC(OUTLINE_BRIGHT), 0);
    lv_obj_set_style_border_opa(g_np_capsule, LV_OPA_10, 0);
    lv_obj_set_style_border_width(g_np_capsule, 1, 0);
    lv_obj_clear_flag(g_np_capsule, LV_OBJ_FLAG_SCROLLABLE);
    ui_on(g_np_capsule, nav_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)SCR_NOWPLAYING, "home.nowplaying", UI_CORE);

    g_np_thumb = lv_obj_create(g_np_capsule);
    lv_obj_remove_style_all(g_np_thumb);
    lv_obj_set_pos(g_np_thumb, 9, 8);
    lv_obj_set_size(g_np_thumb, 42, 42);
    lv_obj_set_style_radius(g_np_thumb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(g_np_thumb, true, 0);
    lv_obj_set_style_bg_color(g_np_thumb, g_accent, 0);
    lv_obj_set_style_bg_opa(g_np_thumb, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_np_thumb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(g_np_thumb, LV_OBJ_FLAG_CLICKABLE);   /* a tap on the cover opens Now Playing like the rest of the card */

    g_np_art_img = lv_image_create(g_np_thumb);
    lv_obj_set_size(g_np_art_img, 42, 42);
    lv_obj_center(g_np_art_img);
    lv_obj_add_flag(g_np_art_img, LV_OBJ_FLAG_HIDDEN);

    g_np_thumb_glyph = make_label(g_np_thumb, LV_SYMBOL_AUDIO, TF(UI_20),
                                  TC(TEXT_PRIMARY));
    lv_obj_center(g_np_thumb_glyph);

    g_np_title = indic_label_create(g_np_capsule);
    lv_label_set_text(g_np_title, tr("Not Playing"));
    lv_label_set_long_mode(g_np_title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(g_np_title, 62, 10);
    lv_obj_set_size(g_np_title, 148, 20);
    lv_obj_set_style_text_font(g_np_title, TF(USER_16), 0);   /* CJK/long titles render (was tofu) */
    lv_obj_set_style_text_color(g_np_title, TC(TEXT_PRIMARY), 0);

    g_np_artist = indic_label_create(g_np_capsule);
    lv_label_set_text(g_np_artist, tr("Library"));
    lv_label_set_long_mode(g_np_artist, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(g_np_artist, 62, 31);
    lv_obj_set_size(g_np_artist, 148, 18);
    lv_obj_set_style_text_font(g_np_artist, TF(USER_14), 0);   /* CJK artist names render */
    lv_obj_set_style_text_color(g_np_artist, TC(TEXT_SECONDARY), 0);

    lv_obj_t *pp_btn = lv_button_create(g_np_capsule);
    lv_obj_remove_style_all(pp_btn);
    lv_obj_set_pos(pp_btn, 208, 7);
    lv_obj_set_size(pp_btn, 48, 44);
    lv_obj_set_style_radius(pp_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pp_btn, TC(RAISED_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(pp_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    ui_on(pp_btn, pp_event_cb, LV_EVENT_CLICKED, NULL, "home.pp", UI_CORE);

    g_np_state = make_label(pp_btn, LV_SYMBOL_PLAY, TF(UI_20),
                            TC(TEXT_PRIMARY));
    lv_obj_center(g_np_state);
    if (theme_trait(THEME_TRAIT_MATRIX_CLOCK)) something_home(root, lib_pill, sbtn);
    if (theme_kit()->home){                            /* the active theme lays Home out its own way */
        home_parts_t hp = { root, g_status, g_clock, g_clock_sub, g_weather, sbtn, lib_pill, hint, g_np_capsule,
                            g_np_thumb, g_np_art_img, g_np_thumb_glyph, g_np_title, g_np_artist, pp_btn, g_np_state };
        theme_kit()->home(&hp);
    }
    lv_async_call(np_fit_async, NULL);   /* after the theme pass has set the final faces */
}

/* The "something" Home (assets/reference/something/home_dark.png): block-matrix clock with an accent colon, dot-
 * matrix date, a now-playing pill with an accent ring and its play key, and two round buttons: Library and Search. */
static void something_home(lv_obj_t *root, lv_obj_t *lib_pill, lv_obj_t *search_btn){
    home_set_backdrop_enabled(0);                      /* the mockup Home is plain black: no blurred cover behind it */
    if(mclock_new(0, root, 50) == 0)
        lv_obj_add_flag(g_clock, LV_OBJ_FLAG_HIDDEN);          /* the text clock stays as the data source */
    lv_obj_set_pos(g_clock_sub, 0, 124);
    lv_obj_set_style_text_font(g_clock_sub, TF(DATE), 0);     /* dot-matrix face */
    lv_obj_set_style_text_color(g_clock_sub, TC(TEXT_SECONDARY), 0);
    lv_obj_set_pos(g_weather, 0, 152);
    lv_obj_add_flag(lib_pill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(search_btn, LV_OBJ_FLAG_HIDDEN);
    /* the pill: dark capsule with a hairline, accent ring + waveform instead of the art thumb, and its play key */
    lv_obj_set_pos(g_np_capsule, 44, 190);
    lv_obj_set_size(g_np_capsule, 272, 64);
    lv_obj_set_style_radius(g_np_capsule, 32, 0);
    lv_obj_set_style_bg_opa(g_np_capsule, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_np_capsule, TC(BORDER), 0);
    lv_obj_set_style_border_opa(g_np_capsule, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_np_capsule, 1, 0);
    lv_obj_set_pos(g_np_thumb, 12, 10);
    lv_obj_set_size(g_np_thumb, 44, 44);
    lv_obj_set_style_bg_opa(g_np_thumb, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_np_thumb, 2, 0);
    lv_obj_set_style_border_color(g_np_thumb, TC(ACCENT_PRIMARY), 0);
    lv_obj_add_flag(g_np_thumb_glyph, LV_OBJ_FLAG_HIDDEN);
    static const int bars[5] = { 8, 14, 20, 14, 8 };
    for(int i = 0; i < 5; i++){
        lv_obj_t *b = lv_obj_create(g_np_thumb);
        lv_obj_remove_style_all(b);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(b, 2, bars[i]);
        lv_obj_align(b, LV_ALIGN_CENTER, (i - 2) * 5, 0);
        lv_obj_set_style_bg_color(b, TC(TEXT_PRIMARY), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    }
    lv_obj_set_pos(g_np_title, 70, 12); lv_obj_set_size(g_np_title, 142, 22);
    lv_obj_set_pos(g_np_artist, 70, 35); lv_obj_set_size(g_np_artist, 142, 18);
    lv_obj_t *pp = lv_obj_get_parent(g_np_state);          /* play / pause stays on the pill (rule 1: CORE) */
    lv_obj_set_pos(pp, 216, 10); lv_obj_set_size(pp, 44, 44);
    lv_obj_set_style_text_color(g_np_state, TC(ACCENT_PRIMARY), 0);
    /* two round buttons: Library (folder) and Search (magnifier) */
    static const struct { int dx; int scr; const char *glyph; int icon; const char *act; } RB[2] = {
        { -40, SCR_LIBRARY, LV_SYMBOL_DIRECTORY, 0, "home.library" },
        {  40, SCR_SEARCH,  "\xEF\x80\x82",       1, "home.search" } };
    for(int i = 0; i < 2; i++){
        lv_obj_t *b = lv_button_create(root);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 60, 60);
        lv_obj_align(b, LV_ALIGN_TOP_MID, RB[i].dx, 268);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(b, TC(SURFACE), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(b, TC(BORDER_STRONG), 0);
        lv_obj_set_style_border_width(b, 1, 0);
        ui_on(b, nav_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)RB[i].scr, RB[i].act, UI_CORE);
        lv_obj_t *ic = make_label(b, RB[i].glyph, RB[i].icon ? TF(ICON_20) : TF(UI_20), TC(TEXT_PRIMARY));
        lv_obj_center(ic);
    }
}
