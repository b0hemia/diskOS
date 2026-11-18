/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "anim.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>

/* Homebrew app launcher. Lists /usr/data/apps/<name>/ entries; each may carry an
 * app.conf ("name=...", "exec=..."), else defaults to dir name + .../app. Tapping
 * a row asks main.c to fork/exec it (app owns fb0 + touch while running). */

#define APPS_DIR "/usr/data/apps"
#define MAX_APPS 24

LV_FONT_DECLARE(font_icons_28)          /* FontAwesome 28px: play-mode + app-tile glyphs */
#define LFM_ICON "\xEF\x88\x82"         /* f202 lastfm    -> Last.fm */
/* Settings/File reuse LV_SYMBOL_SETTINGS (f013) / LV_SYMBOL_FILE (f15b), now in this font */

typedef struct { char name[320]; char exec[320]; } app_t;
static app_t g_apps[MAX_APPS];
static int g_napps;
static lv_obj_t *g_list;

static void scan_apps(void){
    g_napps = 0;
    DIR *d = opendir(APPS_DIR);
    if(!d) return;
    struct dirent *de;
    while((de = readdir(d)) && g_napps < MAX_APPS){
        if(de->d_name[0] == '.') continue;
        app_t *a = &g_apps[g_napps];
        snprintf(a->name, sizeof a->name, "%s", de->d_name);
        snprintf(a->exec, sizeof a->exec, APPS_DIR "/%s/app", de->d_name);
        char conf[320]; snprintf(conf, sizeof conf, APPS_DIR "/%s/app.conf", de->d_name);
        FILE *f = fopen(conf, "r");
        if(f){
            char line[320];
            while(fgets(line, sizeof line, f)){
                char *nl = strpbrk(line, "\r\n"); if(nl) *nl = 0;   /* strip CRLF too (Windows-edited app.conf) */
                if(!strncmp(line, "name=", 5)) snprintf(a->name, sizeof a->name, "%s", line+5);
                else if(!strncmp(line, "exec=", 5)) snprintf(a->exec, sizeof a->exec, "%s", line+5);
            }
            fclose(f);
        }
        g_napps++;
    }
    closedir(d);
}

static void app_row_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    int i = (int)(uintptr_t)lv_event_get_user_data(e);
    if(i>=0 && i<g_napps) app_launch(g_apps[i].exec);
}
static void lastfm_row_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) lastfm_open(); }
static void settings_row_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) screen_show(SCR_SETTINGS); }

/* one app tile in the 2-wide grid (g_list is ROW_WRAP): a big glyph over a small caption */
static void make_tile(const char *icon, const lv_font_t *ifont, const char *name, lv_event_cb_t cb, void *ud){
    lv_obj_t *r = lv_button_create(g_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 128, 108);
    lv_obj_set_style_radius(r, 18, 0);
    lv_obj_set_style_bg_color(r, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(r, 4);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    ui_on(r, cb, LV_EVENT_CLICKED, ud, "apps.cb", UI_CORE);
    lv_obj_t *ic = lv_label_create(r);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_font(ic, ifont, 0);
    lv_obj_set_style_text_color(ic, TC(TEXT_PRIMARY), 0);
    lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_t *l = lv_label_create(r);
    lv_label_set_text(l, name);
    lv_obj_set_style_text_font(l, TF(UI_14), 0);
    lv_obj_set_style_text_color(l, TC(TEXT_SECONDARY), 0);
    lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -14);
}

void apps_reload(void){
    if(!g_list) return;
    lv_obj_clean(g_list);
    scan_apps();
    /* Weather is not a tile here anymore - it opens by tapping the home weather glance (glance->detail). */
    /* built-in: Last.fm scrobbling (the FA lastfm brand glyph) */
    make_tile(LFM_ICON, TF(ICON_28), "Last.fm", lastfm_row_cb, NULL);
    /* homebrew apps from /usr/data/apps */
    for(int i=0;i<g_napps;i++) make_tile(LV_SYMBOL_FILE, TF(ICON_28), g_apps[i].name, app_row_cb, (void*)(uintptr_t)i);
    /* built-in: Settings */
    make_tile(LV_SYMBOL_SETTINGS, TF(ICON_28), "Settings", settings_row_cb, NULL);
}

void apps_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    ui_header(root, "Apps");   /* shared standard header */

    g_list = lv_obj_create(root);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_pos(g_list, 44, 74); lv_obj_set_size(g_list, 272, 252);
    lv_obj_set_style_pad_bottom(g_list, 44, 0);   /* bottom tile row scrolls clear of the round bezel */
    lv_obj_set_style_bg_opa(g_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_row(g_list, 10, 0);
    lv_obj_set_style_pad_column(g_list, 10, 0);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_ROW_WRAP);   /* 2-wide tile grid (128px tiles + 10 gap) */
    lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_list, LV_SCROLLBAR_MODE_OFF);

    apps_reload();
}

lv_obj_t *apps_scroller(void){ return g_list; }
