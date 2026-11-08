/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "fwcaps.h"
#include "theme.h"
#include "theme_kit.h"
#include "screens.h"
#include "ipc.h"
#include "config.h"
#include "i18n.h"
#include "playback.h"   /* playback_display_state: the Cast view */
#include "cast_ui.h"     /* BEAM_GLYPH, cast_ui_open, the session as the UI shows it */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Quick Settings: an iOS-Control-Center-style pull-down panel (top-edge swipe-down; gesture handled
 * in main.c). It is USER-CUSTOMISABLE: the brightness bar sits on top, an optional transport row below
 * it, then a grid of up to six circular tiles chosen in Settings > Controls > Quick Settings. The tile
 * palette is Wi-Fi, Bluetooth, EQ, Search, Rescan, Audio Source and Screen off, and the opt-in tiles listed below. "On" tiles fill with the
 * live accent colour (album-art derived, or the user's fixed pick); everything else is a dark disc.
 * The panel is rebuilt from config on every open so a customisation change shows up immediately.
 * Dismissed by the back-swipe (screen_back). */

LV_FONT_DECLARE(font_icons_28)          /* FontAwesome 28px - brightness sun */
LV_FONT_DECLARE(font_icons_20)          /* FontAwesome 20px - EQ + Search glyphs (match the LVGL symbols) */
#define WI_SUN    "\xEF\x86\x85"        /* f185 sun */
#define WI_EQ     "\xEF\x87\x9E"        /* f1de sliders = equalizer */
#define WI_SEARCH "\xEF\x80\x82"        /* f002 magnifier = search */
#define WI_HEART  "\xEF\x80\x84"        /* f004 heart = favourite */
#define WI_CLOCK  "\xEF\x80\x97"        /* f017 clock = sleep timer */
#define WI_HP     "\xEF\x80\xA5"        /* f025 headphones = DRE */
#define WI_LINK   "\xEF\x83\x81"        /* f0c1 chain = gapless */
#define WI_MIC    "\xEF\x84\xB0"        /* f130 microphone = lyrics */
#define WI_TINT   "\xEF\x81\x83"        /* f043 drop = colour theme */


#define QS_MAX_TILES 6                  /* grid holds two rows of three */

/* ---- tile palette ---------------------------------------------------------------------------- */
enum { QT_WIFI, QT_BT, QT_EQ, QT_SEARCH, QT_RESCAN, QT_MODE, QT_SCREENOFF,
       QT_SHUFFLE, QT_FAV, QT_LYRICS, QT_TIMER, QT_GAPLESS, QT_GAIN, QT_DRE, QT_RG, QT_OUTDOOR, QT_THEME,
       QT_BEAM, QT_COUNT };

typedef struct {
    const char       *cfg;      /* per-tile enable key */
    const char       *cap;      /* caption under the circle */
    const char       *glyph;
    theme_font_role_t font;     /* a font ROLE (a table initialiser can't call theme_font) */
    int               def_on;   /* enabled by default */
    int               has_long; /* has a long-press action (opens a screen) */
} qtile_t;

static const qtile_t QTILES[QT_COUNT] = {
    [QT_WIFI]      = { "qs_t_wifi",      "Wi-Fi",      LV_SYMBOL_WIFI,      THEME_FONT_UI_20, 1, 1 },
    [QT_BT]        = { "qs_t_bt",        "Bluetooth",  LV_SYMBOL_BLUETOOTH, THEME_FONT_UI_20, 1, 1 },
    [QT_EQ]        = { "qs_t_eq",        "EQ",         WI_EQ,               THEME_FONT_ICON_20,         1, 1 },
    [QT_SEARCH]    = { "qs_t_search",    "Search",     WI_SEARCH,           THEME_FONT_ICON_20,         1, 0 },
    [QT_RESCAN]    = { "qs_t_rescan",    "Rescan",     LV_SYMBOL_REFRESH,   THEME_FONT_UI_20, 1, 0 },
    [QT_MODE]      = { "qs_t_mode",      "Source",     LV_SYMBOL_AUDIO,     THEME_FONT_UI_20, 1, 0 },
    [QT_SCREENOFF] = { "qs_t_screenoff", "Screen off", LV_SYMBOL_EYE_CLOSE, THEME_FONT_UI_20, 0, 0 },
    /* opt-in audio/playback tiles (default off; enable in Settings > Controls > Quick Settings) */
    [QT_SHUFFLE]   = { "qs_t_shuffle",   "Shuffle",    LV_SYMBOL_SHUFFLE,   THEME_FONT_UI_20, 0, 1 },
    [QT_FAV]       = { "qs_t_fav",       "Favourite",  WI_HEART,            THEME_FONT_ICON_20,         0, 0 },
    [QT_LYRICS]    = { "qs_t_lyrics",    "Lyrics",     WI_MIC,              THEME_FONT_ICON_20,         0, 0 },
    [QT_TIMER]     = { "qs_t_timer",     "Timer",      WI_CLOCK,            THEME_FONT_ICON_20,         0, 0 },
    [QT_GAPLESS]   = { "qs_t_gapless",   "Gapless",    WI_LINK,             THEME_FONT_ICON_20,         0, 1 },
    [QT_GAIN]      = { "qs_t_gain",      "High Gain",  LV_SYMBOL_VOLUME_MAX,THEME_FONT_UI_20, 0, 1 },
    [QT_DRE]       = { "qs_t_dre",       "DRE",        WI_HP,               THEME_FONT_ICON_20,         0, 1 },
    [QT_RG]        = { "qs_t_rg",        "ReplayGain", LV_SYMBOL_VOLUME_MID,THEME_FONT_UI_20, 0, 0 },
    [QT_OUTDOOR]   = { "qs_t_outdoor",   "Outdoor",    WI_SUN,              THEME_FONT_ICON_20,         0, 1 },
    [QT_THEME]     = { "qs_t_theme",     "Theme",      WI_TINT,             THEME_FONT_ICON_20,         0, 1 },
    [QT_BEAM]      = { "qs_t_beam",      "Beam",       BEAM_GLYPH,          THEME_FONT_ICON_20,         0, 0 },   /* Cast 21: opt-in (owner decision 85); opens the Beam picker, lit + the device name while beaming */
};

/* Local song/book actions have no meaning while another source owns playback (the retained local path would
 * otherwise be favourited, its lyrics fetched or the local play mode changed). */
static int tile_local_blocked(int id){
    return ui_receiver_in_use() &&
        (id == QT_SHUFFLE || id == QT_FAV || id == QT_LYRICS);
}

static lv_obj_t *g_qs_root;
static lv_obj_t *g_bright;
static lv_obj_t *g_prev_glyph, *g_next_glyph;
static lv_obj_t *g_pp_glyph;                 /* transport play/pause glyph (NULL when transport is off) */
static lv_obj_t *g_tile_dot[QT_COUNT];       /* the circle per shown tile, recoloured on refresh */
static lv_obj_t *g_tile_lbl[QT_COUNT];       /* its caption (the ring style colours it too) */
static lv_obj_t *dim_tile, *dim_cap;         /* the Favourite tile (circle, caption) currently dimmed while casting (cleared when the tiles are rebuilt) */

/* the drawer shows at most this many tiles - one fewer with the transport row, so the second row never
 * pushes its outer captions off the round bezel. */
static int qs_cap(void){ return cfg_get_int("qs_transport", 0) ? QS_MAX_TILES - 1 : QS_MAX_TILES; }

/* which tiles are enabled, in palette order, capped. Returns the count. */
static int qs_enabled(int ids[QT_COUNT], int cap){
    int n = 0;
    for(int i=0; i<QT_COUNT && n<cap; i++)
        if(cfg_get_int(QTILES[i].cfg, QTILES[i].def_on)) ids[n++] = i;
    return n;
}

static int beam_active(void){ return ui_cast_phase() == 2 && *ui_cast_device(); }   /* a session is playing or paused: the same test as the status line */

/* is a toggle tile currently "on"? action tiles are never lit. */
static int qtile_is_on(int id){
    switch(id){
        case QT_WIFI:    return wifi_control_on();
        case QT_BT:      return bt_radio_on();   /* actual radio state, not just the persisted intent */
        case QT_EQ:      return cfg_get_int("eq_preset", 0) > 0;
        case QT_SHUFFLE: return cfg_get_int("work_mode", 0) == 1;
        case QT_GAPLESS: return cfg_get_int("gapless", 0) == 1;
        case QT_GAIN:    return fw_gain_tag() && cfg_get_int("audio_gain", 0) == 1;   /* unmapped firmware: never lit */
        case QT_DRE:     return cfg_get_int("audio_dre", 1) == 1;
        case QT_OUTDOOR: return theme_outdoor();
        case QT_BEAM:    return beam_active();   /* lit while beaming (drawn lit; the tile itself is an action) */
        case QT_FAV:   { track_state_t raw, st; ipc_get_state(&raw); playback_display_state(&st, &raw); return st.path[0] && st.is_favorite; }   /* 15b: never lit while casting (the receiver's favourite state is unknown and the write is refused) */
        default:         return 0;   /* action tiles (Search/Rescan/Source/Screen off/Lyrics/Timer/RG) never lit */
    }
}

static void tile_paint(int id, int on);
/* paint a tile, then centre its icon by INK (after the theme's final font: icon glyphs sit high or low in their line, so
 * centring the line box left them 2 px low in some themes); word tiles (terminal) keep their baseline */
static void tile_recolor(int id, int on){
    tile_paint(id, on);
    if(g_tile_dot[id]) ui_glyph_center_ink(lv_obj_get_child(g_tile_dot[id], 0));
}
static void tile_paint(int id, int on){
    if(!g_tile_dot[id]) return;
    lv_obj_t *c = g_tile_dot[id], *g = lv_obj_get_child(c, 0);
    if(theme_kit()->qs_tile){                         /* the theme's own tile */
        int toggle = id == QT_WIFI || id == QT_BT || id == QT_EQ || id == QT_SHUFFLE || id == QT_FAV || id == QT_GAPLESS
                  || id == QT_GAIN || id == QT_DRE || id == QT_OUTDOOR || id == QT_BEAM;   /* the tiles qtile_is_on() can light */
        kit_keep(c);                                  /* the theme painted it: the styling pass leaves it */
        theme_kit()->qs_tile(c, g, g_tile_lbl[id], on, toggle); return;
    }
    if(theme_trait(THEME_TRAIT_QS_RING)){
        /* lit = accent ring + a faint accent halo (a wide translucent outline: no blur, cheap on this CPU) with an
         * accent glyph and caption; off = a plain dark disc */
        lv_obj_set_style_bg_color(c, on ? TC(SURFACE) : TC(SURFACE_RAISED), 0);
        lv_obj_set_style_border_width(c, on ? 2 : 0, 0);
        lv_obj_set_style_border_color(c, TC(ACCENT_PRIMARY), 0);
        lv_obj_set_style_outline_width(c, on ? 5 : 0, 0);
        lv_obj_set_style_outline_color(c, TC(ACCENT_PRIMARY), 0);
        lv_obj_set_style_outline_opa(c, LV_OPA_20, 0);
        if(g) lv_obj_set_style_text_color(g, on ? TC(ACCENT_PRIMARY) : TC(TEXT_PRIMARY), 0);
        if(g_tile_lbl[id]) lv_obj_set_style_text_color(g_tile_lbl[id], on ? TC(ACCENT_PRIMARY) : TC(TEXT_SECONDARY), 0);
        return;
    }
    lv_obj_set_style_bg_color(c, on ? ui_current_accent() : TC(CONTROL_OFF), 0);
    if(g) lv_obj_set_style_text_color(g, on ? TC(ON_ACCENT) : TC(TEXT_PRIMARY), 0);   /* on the accent fill when lit */
}

/* ---- EQ A/B toggle (drawer): flip Off <-> the last-used preset, all persisted centrally --------- */
static void eq_toggle(void){
    int cur = cfg_get_int("eq_preset", 0); if(cur < 0 || cur > 20) cur = 0;
    if(cur > 0){                                  /* on -> off; remember what was on only if the send took */
        if(ui_eq_select(0) == 0){ cfg_set_int("eq_last", cur); tile_recolor(QT_EQ, 0); ui_toast("EQ off"); }
        else ui_toast("Couldn't apply EQ");
    }else{                                        /* off -> on; restore last (Rock the first time) */
        int last = cfg_get_int("eq_last", 0);
        if(last <= 0 || last > 20) last = 2;      /* 2 = Rock: an audible default before any is chosen */
        if(ui_eq_select(last) == 0){ tile_recolor(QT_EQ, 1); char m[40]; snprintf(m, sizeof m, "EQ: %s", ui_eq_name(last)); ui_toast(m); }
        else ui_toast("Couldn't apply EQ");
    }
}

/* ---- unified tile handlers (tile id in user_data) ---------------------------------------------- */
static void tile_short_cb(lv_event_t *e){
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    if(tile_local_blocked(id)) return;
    switch(id){
        case QT_WIFI:      { int on = wifi_toggle(); tile_recolor(QT_WIFI, on); ui_toast(on ? "Turning on Wi-Fi..." : "Wi-Fi off"); } break;
        case QT_BT:        { int on = bt_toggle(); if(on < 0) break; tile_recolor(QT_BT, on); ui_toast(on ? "Turning on Bluetooth..." : "Bluetooth off"); } break;
        case QT_EQ:        eq_toggle(); break;
        case QT_SEARCH:    screen_show(SCR_SEARCH); break;
        case QT_RESCAN:    ui_rescan_library(); break;
        case QT_MODE:      modes_open(); break;
        case QT_BEAM:      cast_ui_open(); break;   /* the picker explains a missing Wi-Fi itself; nothing is sent to the player */
        case QT_SCREENOFF: ui_request_sleep(); screen_show(SCR_SAVER); break;
        case QT_SHUFFLE: {
            if(ui_book_active()){ ui_toast("Not available in books"); break; }   /* books stay in Single */
            int on = cfg_get_int("work_mode", 0) == 1 ? 0 : 1;                    /* Shuffle <-> Sequential */
            cfg_set_int("work_mode", on); ui_set_workmode(on);
            tile_recolor(QT_SHUFFLE, on); ui_toast(on ? "Shuffle on" : "Shuffle off");
        } break;
        case QT_FAV: {
            track_state_t raw, st; ipc_get_state(&raw); playback_display_state(&st, &raw);
            if(!st.path[0] || (ui_book_active() && playback_source() == PB_SRC_LOCAL)){ ui_toast("No track playing"); break; }   /* casting: ui_set_favorite refuses with its own toast */
            int nv = !st.is_favorite; if(!ui_set_favorite(nv)) break;   /* refused while casting: no tile recolour, no "Loved" toast */
            tile_recolor(QT_FAV, nv); ui_toast(nv ? "Loved" : "Unloved");
        } break;
        case QT_LYRICS: lyrics_open(); break;
        case QT_TIMER:  settings_open_key("sleep_idx"); break;      /* opens the Sleep Timer chooser */
        case QT_GAPLESS: {
            int on = cfg_get_int("gapless", 0) ? 0 : 1;
            cfg_set_int("gapless", on); ui_set_gapless(on);
            tile_recolor(QT_GAPLESS, on); ui_toast(on ? "Gapless on" : "Gapless off");
        } break;
        case QT_GAIN: {
            if(!fw_gain_tag()){ ui_toast("Not supported on this firmware yet"); break; }   /* nothing would be sent */
            int on = cfg_get_int("audio_gain", 0) ? 0 : 1;
            cfg_set_int("audio_gain", on); ui_set_gain(on);
            tile_recolor(QT_GAIN, on); ui_toast(on ? "High gain" : "Low gain");
        } break;
        case QT_DRE: {
            int on = cfg_get_int("audio_dre", 1) ? 0 : 1;
            cfg_set_int("audio_dre", on); ui_set_dre(on);
            tile_recolor(QT_DRE, on); ui_toast(on ? "DRE on" : "DRE off");
        } break;
        case QT_RG:     settings_open_key("replay_gain"); break;    /* opens the ReplayGain chooser */
        case QT_OUTDOOR: {                                          /* redraws the UI in the new palette */
            int was = cfg_get_int("outdoor", 0) == 1;
            cfg_set_int("outdoor", !was);
            if(ui_theme_reload("quick") != 0) cfg_set_int("outdoor", was);   /* refused (scan): nothing changes */
        } break;
        case QT_THEME: {                                            /* next colour preset */
            int was = theme_preset();
            cfg_set_int("theme_preset", (was + 1) % THEME_PRESET_COUNT);
            if(ui_theme_reload("quick") != 0) cfg_set_int("theme_preset", was);
        } break;
    }
}
static void tile_long_cb(lv_event_t *e){
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    if(tile_local_blocked(id)) return;
    switch(id){
        case QT_WIFI: wifi_open(); break;
        case QT_BT:   bt_open();   break;
        case QT_EQ:   screen_show(SCR_EQ); break;
        case QT_SHUFFLE: settings_open_key("work_mode");  break;   /* full Play Mode chooser */
        case QT_GAPLESS: settings_open_key("gapless");    break;
        case QT_GAIN:    settings_open_key("audio_gain"); break;
        case QT_DRE:     settings_open_key("audio_dre");  break;
        case QT_OUTDOOR: settings_open_key("outdoor");    break;
        case QT_THEME:   settings_open_key("theme_preset"); break;
        default: break;
    }
}

/* one circular tile + caption, parent = root, centre at (x, y_top). Records its circle for refresh. */
static void build_tile(lv_obj_t *root, int id, int x, int y){
    const qtile_t *t = &QTILES[id];
    lv_obj_t *c = lv_button_create(root);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, 58, 58);
    lv_obj_align(c, LV_ALIGN_TOP_MID, x, y);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, qtile_is_on(id) ? ui_current_accent() : TC(CONTROL_OFF), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(c, LV_OPA_80, LV_STATE_PRESSED);   /* gentle press dim on either state colour */
    lv_obj_set_ext_click_area(c, 4);
    char an[32], al[32];
    lv_snprintf(an, sizeof an, "qs.tile.%d", id); lv_snprintf(al, sizeof al, "qs.tile.%d.long", id);
    if(t->has_long){
        /* SHORT_CLICKED so a long press fires ONLY the long action, never the toggle too */
        ui_on(c, tile_short_cb, LV_EVENT_SHORT_CLICKED, (void*)(intptr_t)id, an, UI_CORE);
        ui_on(c, tile_long_cb, LV_EVENT_LONG_PRESSED, (void*)(intptr_t)id, al, UI_CORE);
    }else{
        ui_on(c, tile_short_cb, LV_EVENT_CLICKED, (void*)(intptr_t)id, an, UI_CORE);
    }
    lv_obj_t *g = lv_label_create(c);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(g, t->glyph);
    lv_obj_set_style_text_font(g, theme_font(t->font), 0);
    lv_obj_set_style_text_color(g, qtile_is_on(id) ? TC(ON_ACCENT) : TC(TEXT_PRIMARY), 0);
    lv_obj_center(g);

    lv_obj_t *lb = lv_label_create(root);
    lv_obj_clear_flag(lb, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(lb, tr(t->cap));
    lv_obj_set_style_text_font(lb, TF(UI_14), 0);
    lv_obj_set_style_text_color(lb, TC(TEXT_MUTED), 0);
    lv_obj_align(lb, LV_ALIGN_TOP_MID, x, y + 62);

    g_tile_dot[id] = c;
    g_tile_lbl[id] = lb;
    if(theme_trait(THEME_TRAIT_QS_RING)){               /* upper-case bold captions, ring styling */
        char up[24];
        lv_label_set_text(lb, theme_upper(up, sizeof up, tr(t->cap)));
        lv_obj_set_style_text_font(lb, TF(UI_12), 0);
        lv_obj_align(lb, LV_ALIGN_TOP_MID, x, y + 64);
        tile_recolor(id, qtile_is_on(id));
    }
    if(theme_kit()->qs_tile) tile_recolor(id, qtile_is_on(id));
}

/* ---- Beam tile caption + live state (Cast 21) -------------------------------------------------------
 * The caption is "Beam", or the receiver's name while a session plays. The drawer follows the session while it is open: a timer exists only
 * while the drawer is shown on a lit screen (the nphub_poll_sync pattern: started by a build or a refresh, deleted by its own next tick once
 * the drawer is left or the screen is dark; the wake refresh starts it again), and it touches the tile only when the session changed. */
int ui_screen_backlight_on(void);
static lv_timer_t *g_beam_tm;
static char g_beam_raw[100];   /* the caption text last set, before any theme case change or shortening */
static int  g_beam_on = -1, g_beam_maxw;
static const lv_font_t *g_beam_font;
/* Repaints the tile when the session, the caption font or the room for the caption changed. A long name is shortened here with "..." to fit its
 * column (not by the label's own dot mode: that rewrites the label text, and the theme tile styles lower- or upper-case what the label holds). */
static int beam_sync(void){
    lv_obj_t *lb = g_tile_lbl[QT_BEAM];
    if(!lb) return 0;
    int on = beam_active() ? 1 : 0;
    const char *want = on ? ui_cast_device() : tr("Beam");
    lv_obj_t *par = lv_obj_get_parent(lb);
    if(par != g_qs_root) lv_obj_update_layout(par);   /* a checklist row (Terminal) is sized by its theme: its own width, less the box that leads it */
    int maxw = par == g_qs_root ? 96 : lv_obj_get_width(par) - 64;   /* 84 px column pitch, a name gets 96 */
    if(on == g_beam_on && maxw == g_beam_maxw && lv_obj_get_style_text_font(lb, 0) == g_beam_font && !strcmp(want, g_beam_raw)) return 0;
    g_beam_on = on; g_beam_maxw = maxw; snprintf(g_beam_raw, sizeof g_beam_raw, "%s", want);
    char up[100];
    lv_label_set_text(lb, theme_trait(THEME_TRAIT_QS_RING) ? theme_upper(up, sizeof up, g_beam_raw) : g_beam_raw);
    tile_recolor(QT_BEAM, on);                            /* the theme's own caption pass first (case, font) ... */
    const lv_font_t *fn = lv_obj_get_style_text_font(lb, 0); g_beam_font = fn;
    char buf[100]; snprintf(buf, sizeof buf, "%s", lv_label_get_text(lb));
    int ls = lv_obj_get_style_text_letter_space(lb, 0); lv_point_t sz;
    lv_text_get_size(&sz, buf, fn, ls, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    for(int n = (int)strlen(buf); sz.x > maxw && n > 0;){   /* ... then the name is cut to what the final font fits */
        do n--; while(n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80);
        char t[104]; snprintf(t, sizeof t, "%.*s...", n, buf);
        lv_text_get_size(&sz, t, fn, ls, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if(sz.x <= maxw) lv_label_set_text(lb, t);
    }
    return 1;
}
static void beam_tm_cb(lv_timer_t *t){
    if(screen_current() != SCR_QUICK || !g_tile_dot[QT_BEAM] || !ui_screen_backlight_on()){ lv_timer_delete(t); g_beam_tm = NULL; return; }
    beam_sync();
}
static void beam_poll_sync(void){
    int want = g_tile_dot[QT_BEAM] && screen_current() == SCR_QUICK && ui_screen_backlight_on();
    if(want && !g_beam_tm) g_beam_tm = lv_timer_create(beam_tm_cb, 250, NULL);
    else if(!want && g_beam_tm){ lv_timer_delete(g_beam_tm); g_beam_tm = NULL; }
}
#ifdef DISKOS_HOST
int qs_host_beam_timer_live(void){ return g_beam_tm != NULL; }   /* host probe */
#endif

/* ---- optional transport row -------------------------------------------------------------------- */
static void cmd_cb(lv_event_t *e){
    const char *c = (const char*)lv_event_get_user_data(e); if(!c) return;
    if(ui_transport_command(c) == 0) quicksettings_refresh(ui_transport_playing());   /* receiver first, else the view (Cast facade or local) */
}
static lv_obj_t *tp_btn(lv_obj_t *root, const char *sym, const lv_font_t *font, int x, int y, int sz, void *cmd){
    lv_obj_t *b = lv_button_create(root);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, sz, sz);
    lv_obj_align(b, LV_ALIGN_TOP_MID, x, y);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(b, 6);
    char an[24];
    ui_on(b, cmd_cb, LV_EVENT_CLICKED, cmd, ui_transport_action(cmd, "qs", an, sizeof an), UI_CORE);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, TC(TEXT_PRIMARY), 0);
    lv_obj_center(l);
    return l;
}
static void build_transport(lv_obj_t *root, int y){
    g_prev_glyph = tp_btn(root, LV_SYMBOL_PREV, TF(UI_22), -84, y + 5, 46, (void*)"0201000C0002");
    g_pp_glyph = tp_btn(root, LV_SYMBOL_PAUSE, TF(UI_28), 0, y, 56, (void*)"0201000C0000");
    g_next_glyph = tp_btn(root, LV_SYMBOL_NEXT, TF(UI_22), 84, y + 5, 46, (void*)"0201000C0001");
    if(g_pp_glyph) ui_pp_glyph(g_pp_glyph, ui_transport_playing());
}

/* ---- brightness bar ---------------------------------------------------------------------------- */
static void bright_change_cb(lv_event_t *e){ if(!theme_outdoor()) ui_backlight(lv_slider_get_value(lv_event_get_target(e))); }
static void bright_release_cb(lv_event_t *e){ if(!theme_outdoor()) ui_set_brightness(lv_slider_get_value(lv_event_get_target(e))); }
static void build_brightness(lv_obj_t *root, int y){
    g_bright = lv_slider_create(root);
    lv_obj_set_size(g_bright, 216, 34);
    lv_obj_set_ext_click_area(g_bright, 8);
    lv_obj_align(g_bright, LV_ALIGN_TOP_MID, 0, y);
    lv_slider_set_range(g_bright, 4, 40);
    lv_slider_set_value(g_bright, ui_effective_brightness(), LV_ANIM_OFF);
    if(theme_outdoor()) lv_obj_add_state(g_bright, LV_STATE_DISABLED);   /* Outdoor holds full brightness */
    lv_obj_set_style_bg_color(g_bright, TC(CONTROL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_radius(g_bright, 17, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_bright, TC(CONTROL_FILL), LV_PART_INDICATOR);
    lv_obj_set_style_radius(g_bright, 17, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_bright, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(g_bright, 0, LV_PART_KNOB);
    ui_on(g_bright, bright_change_cb, LV_EVENT_VALUE_CHANGED, NULL, "quicksettings.bright_change.value", UI_CORE);
    lv_obj_add_event_cb(g_bright, bright_release_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(g_bright, bright_release_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_t *sun = lv_label_create(g_bright);
    lv_obj_clear_flag(sun, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(sun, WI_SUN);
    lv_obj_set_style_text_font(sun, TF(ICON_28), 0);
    lv_obj_set_style_text_color(sun, TC(ON_CONTROL), 0);
    lv_obj_align(sun, LV_ALIGN_LEFT_MID, 12, 0);
    if(theme_trait(THEME_TRAIT_QS_RING)){
        /* a thin bar with the sun beside it (the whole old height stays touchable) */
        lv_obj_set_size(g_bright, 196, 6);
        lv_obj_align(g_bright, LV_ALIGN_TOP_MID, 14, y + 14);
        lv_obj_set_ext_click_area(g_bright, 16);
        lv_obj_set_style_radius(g_bright, 3, LV_PART_MAIN);
        lv_obj_set_style_radius(g_bright, 3, LV_PART_INDICATOR);
        lv_obj_set_parent(sun, root);
        lv_obj_set_style_text_font(sun, TF(ICON_20), 0);
        lv_obj_set_style_text_color(sun, TC(TEXT_PRIMARY), 0);
        lv_obj_align_to(sun, g_bright, LV_ALIGN_OUT_LEFT_MID, -12, 0);
    }
}

/* ---- build / refresh --------------------------------------------------------------------------- */
void quicksettings_create(lv_obj_t *root){ g_qs_root = root; }   /* defer to quicksettings_build on open */

/* Rebuild the whole panel from config. Called on every entry so customisation changes show at once. */
void quicksettings_build(void){
    if(!g_qs_root) return;
    lv_obj_clean(g_qs_root);
    g_bright = NULL; g_pp_glyph = NULL; g_prev_glyph = NULL; g_next_glyph = NULL;
    for(int i=0;i<QT_COUNT;i++){ g_tile_dot[i] = NULL; g_tile_lbl[i] = NULL; }
    dim_tile = dim_cap = NULL;
    g_beam_raw[0] = 0; g_beam_on = -1;

    lv_obj_set_style_bg_color(g_qs_root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(g_qs_root, LV_OPA_COVER, 0);

    /* grabber at the bottom edge, where the swipe up that closes the panel starts */
    lv_obj_t *grab = lv_obj_create(g_qs_root);
    lv_obj_remove_style_all(grab);
    lv_obj_clear_flag(grab, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(grab, 36, 5);
    lv_obj_align(grab, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_radius(grab, 3, 0);
    lv_obj_set_style_bg_color(grab, TC(GRABBER), 0);
    lv_obj_set_style_bg_opa(grab, LV_OPA_COVER, 0);

    int have_tr = cfg_get_int("qs_transport", 0) ? 1 : 0;
    int ids[QT_COUNT]; int nt = qs_enabled(ids, qs_cap());
    int rows = nt > 3 ? 2 : (nt > 0 ? 1 : 0);

    const int H_BR = 34, H_TR = 56, H_ROW = 80, GAP = 14;
    int nblocks = 1 + have_tr + rows;                 /* brightness is always present */
    int total = H_BR + (have_tr ? H_TR : 0) + rows*H_ROW + (nblocks > 1 ? (nblocks-1)*GAP : 0);
    int y = 185 - total/2;                            /* centre the stack a touch below the middle */
    if(y < 44) y = 44;                                /* clear of the round top edge */

    build_brightness(g_qs_root, y);  y += H_BR + GAP;
    if(have_tr){ build_transport(g_qs_root, y); y += H_TR + GAP; }

    int placed = 0;
    for(int r=0; r<rows; r++){
        int k = (r == 0) ? (nt > 3 ? 3 : nt) : (nt - 3);   /* row 1 up to 3, row 2 the remainder */
        for(int i=0; i<k; i++){
            int x = (int)((i - (k-1)/2.0) * 84);           /* centre the row, 84px column pitch */
            build_tile(g_qs_root, ids[placed++], x, y);
        }
        y += H_ROW + GAP;
    }
    if(theme_kit()->quicksettings){                   /* the active theme lays the drawer out its own way */
        qs_parts_t q = { g_qs_root, grab, g_bright, g_prev_glyph ? lv_obj_get_parent(g_prev_glyph) : NULL,
                         g_pp_glyph ? lv_obj_get_parent(g_pp_glyph) : NULL,
                         g_next_glyph ? lv_obj_get_parent(g_next_glyph) : NULL, nt, {0}, {0} };
        for(int i = 0; i < nt; i++){ q.tile[i] = g_tile_dot[ids[i]]; q.cap[i] = g_tile_lbl[ids[i]]; }
        theme_kit()->quicksettings(&q);
    }
    quicksettings_refresh(ui_transport_playing());
}

/* light update while the panel is open (playstate ticks, radio/EQ state changes) - no rebuild */
void quicksettings_refresh(int playing){
    if(g_bright && !lv_obj_has_state(g_bright, LV_STATE_PRESSED))
        lv_slider_set_value(g_bright, ui_effective_brightness(), LV_ANIM_OFF);
    if(ui_receiver_active()) playing = ui_transport_playing();   /* main.c passes the frozen local state */
    if(g_pp_glyph) ui_pp_glyph(g_pp_glyph, ui_pp_icon_playing(playing));
    int book = ui_book_active() && !ui_receiver_active() && playback_source() == PB_SRC_LOCAL;   /* a retained book session does not turn receiver skips into +30/-15; a Cast track has no -15/+30 either */
    if(g_prev_glyph){
        lv_label_set_text(g_prev_glyph, book ? "-15" : LV_SYMBOL_PREV);
        lv_obj_set_style_text_font(g_prev_glyph, book ? TF(UI_16) : TF(UI_22), 0);
    }
    if(g_next_glyph){
        lv_label_set_text(g_next_glyph, book ? "+30" : LV_SYMBOL_NEXT);
        lv_obj_set_style_text_font(g_next_glyph, book ? TF(UI_16) : TF(UI_22), 0);
    }
    beam_sync();
    for(int i=0;i<QT_COUNT;i++){
        if(!g_tile_dot[i]) continue;
        if(tile_local_blocked(i))
            lv_obj_add_state(g_tile_dot[i], LV_STATE_DISABLED);
        else
            lv_obj_remove_state(g_tile_dot[i], LV_STATE_DISABLED);
        tile_recolor(i, qtile_is_on(i));
    }
    beam_poll_sync();
    /* Favourite is refused while casting: the tile is shown dimmed (a tap says why) instead of looking available. Only a change touches it. */
    lv_obj_t *ft = g_tile_dot[QT_FAV]; int cast = playback_source() != PB_SRC_LOCAL;
    lv_obj_t *fc = g_tile_lbl[QT_FAV];
    if(cast && ft && dim_tile != ft){ lv_obj_set_style_opa(ft, LV_OPA_50, 0); dim_tile = ft; }
    else if(!cast && dim_tile){ if(dim_tile == ft) lv_obj_set_style_opa(ft, LV_OPA_COVER, 0); dim_tile = NULL; }
    if(cast && fc && dim_cap != fc){ lv_obj_set_style_opa(fc, LV_OPA_50, 0); dim_cap = fc; }   /* 15c: the caption goes with its circle */
    else if(!cast && dim_cap){ if(dim_cap == fc) lv_obj_set_style_opa(fc, LV_OPA_COVER, 0); dim_cap = NULL; }
}

/* ---- customisation screen (SCR_QSCONFIG): Choose tiles - pick which tiles the drawer shows ---------- *
 * One list: "Playback controls" first, then every tile, each a whole-row switch (a tap anywhere on the row flips it). A
 * counter under the title says how many are chosen of how many fit. The saved choices are the same per-tile keys
 * (qs_t_*) and qs_transport as always, so an existing choice shows as it was; a tile that does not fit is refused with
 * a toast and nothing a user chose is ever switched off for them. */
static lv_obj_t *g_qscfg_root, *g_qscfg_count;

static void qscfg_back_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) screen_back(); }
static int qscfg_chosen(void){
    int cnt = 0;
    for(int i=0;i<QT_COUNT;i++) if(cfg_get_int(QTILES[i].cfg, QTILES[i].def_on)) cnt++;
    return cnt;
}
static void qscfg_count_paint(void){
    if(!g_qscfg_count) return;
    char b[64];
    lv_snprintf(b, sizeof b, tr("%d of %d tiles selected"), qscfg_chosen(), qs_cap());
    lv_label_set_text(g_qscfg_count, b);
}
static void qscfg_sw_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_VALUE_CHANGED) return;
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *sw = lv_event_get_target(e);
    int on = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    if(id < 0){
        if(on && qscfg_chosen() > QS_MAX_TILES - 1){
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
            ui_toast("Turn off a tile first");
            return;
        }
        cfg_set_int("qs_transport", on);
        qscfg_count_paint();
        return;
    }   /* id -1 = the transport row */
    if(on){                                                  /* refuse to enable past the drawer's tile cap */
        int cap = qs_cap();
        if(qscfg_chosen() >= cap){                          /* this tile is still off in cfg, so the count excludes it */
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
            char m[32]; snprintf(m, sizeof m, "Up to %d tiles shown", cap); ui_toast(m);
            return;
        }
    }
    cfg_set_int(QTILES[id].cfg, on);
    qscfg_count_paint();
}
/* The row is the tap target: it flips the switch like a finger on the switch would. */
static void qscfg_row_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    lv_obj_t *sw = lv_obj_get_child(lv_event_get_target(e), 1);
    if(lv_obj_has_state(sw, LV_STATE_CHECKED)) lv_obj_remove_state(sw, LV_STATE_CHECKED); else lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, NULL);
}
/* a labelled row with an LVGL switch on the right (the Settings switch-row look: card, name left, switch right) */
static void qscfg_row(lv_obj_t *list, const char *label, int id, int on){
    lv_obj_t *row = lv_button_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 268, 50);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_bg_color(row, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(row, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    ui_on(row, qscfg_row_cb, LV_EVENT_CLICKED, NULL, "quicksettings.qscfg_row", UI_CORE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, tr(label));
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_set_style_text_font(lbl, TF(UI_16), 0);
    lv_obj_set_style_text_color(lbl, TC(TEXT_PRIMARY), 0);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_clear_flag(sw, LV_OBJ_FLAG_CLICKABLE);               /* the row takes the tap; the switch shows the state */
    theme_settings_switch(sw);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -12, 0);
    if(on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, qscfg_sw_cb, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)id);
    theme_list_row(row);
}

void qsconfig_create(lv_obj_t *root){ g_qscfg_root = root; }
static void qscfg_count_deleted(lv_event_t *e){ (void)e; g_qscfg_count = NULL; }

void qsconfig_refresh(void){
    if(!g_qscfg_root) return;
    lv_obj_clean(g_qscfg_root);
    lv_obj_set_style_bg_color(g_qscfg_root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(g_qscfg_root, LV_OPA_COVER, 0);
    ui_header_cb(g_qscfg_root, "Quick Settings", qscfg_back_cb);

    g_qscfg_count = lv_label_create(g_qscfg_root);               /* "4 of 6 tiles selected", live */
    lv_obj_add_event_cb(g_qscfg_count, qscfg_count_deleted, LV_EVENT_DELETE, NULL);
    lv_label_set_long_mode(g_qscfg_count, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(g_qscfg_count, 50, 66);
    lv_obj_set_size(g_qscfg_count, 260, 20);
    lv_obj_set_style_text_align(g_qscfg_count, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_qscfg_count, TF(UI_14), 0);
    lv_obj_set_style_text_color(g_qscfg_count, TC(TEXT_SECONDARY), 0);
    qscfg_count_paint();

    lv_obj_t *list = lv_obj_create(g_qscfg_root);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 30, 92);
    lv_obj_set_size(list, 300, 228);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_style_pad_bottom(list, 44, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    qscfg_row(list, "Playback controls", -1, cfg_get_int("qs_transport", 0));
    for(int i=0;i<QT_COUNT;i++)
        qscfg_row(list, QTILES[i].cap, i, cfg_get_int(QTILES[i].cfg, QTILES[i].def_on));
}
