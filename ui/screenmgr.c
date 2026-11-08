/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "device_names_ui.h"
#include "theme.h"
#include "theme_kit.h"
#include "folderbrowser.h"
#include "custom_picker_ui.h"
#include "custom_saver_overlay.h"
#include "books.h"
#include "anim.h"
#include "config.h"
#include <unistd.h>

void mclock_sync(int backlight_on);
int ui_screen_backlight_on(void);
void ui_np_title_scroll(int visible);

#define SCR_W 360

static lv_obj_t *s_roots[SCR_COUNT];
static lv_obj_t *s_scrim;        /* depth overlay dimming the screen beneath the moving panel */
static lv_obj_t *s_scrim_panel;

/* The opaque sliding panel replaces these pixels. Clip only the scrim's main
 * draw; LVGL restores the layer clip before post drawing and siblings. */
static void scrim_clip(lv_event_t *e)
{
    lv_obj_t *o = s_scrim_panel;
    if (!o || lv_obj_get_style_bg_opa(o, 0) != LV_OPA_COVER ||
        lv_obj_get_style_opa_recursive(o, 0) != LV_OPA_COVER ||
        lv_obj_get_style_opa_layered(o, 0) != LV_OPA_COVER ||
        lv_obj_get_style_radius(o, 0) != 0) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    if (a.y1 <= layer->_clip_area.y1 && a.y2 >= layer->_clip_area.y2 &&
        a.x2 >= layer->_clip_area.x2 && a.x1 <= layer->_clip_area.x2)
        layer->_clip_area.x2 = a.x1 - 1;
}
static int s_current = SCR_HOME;
/* Permanent drawing figures. Keep existing assignments when adding screens. */
static const unsigned char s_figure[SCR_COUNT] = {
    [SCR_HOME] = 1, [SCR_LIBRARY] = 2, [SCR_NOWPLAYING] = 3,
    [SCR_SETTINGS] = 4, [SCR_SEARCH] = 5, [SCR_APPS] = 6,
    [SCR_WEATHER] = 7, [SCR_UPNEXT] = 8, [SCR_LYRICS] = 9,
    [SCR_SONGINFO] = 10, [SCR_NPMENU] = 11, [SCR_NPHUB] = 12,
    [SCR_TUNE] = 13, [SCR_EQ] = 14, [SCR_SAVER] = 15,
    [SCR_FOLDER] = 16, [SCR_ALBUMWALL] = 17, [SCR_PLPICK] = 18,
    [SCR_PLVIEW] = 19, [SCR_BOOKS] = 20, [SCR_CHAPTERS] = 21,
    [SCR_SETTING_DETAIL] = 22, [SCR_SETLIST] = 23, [SCR_QUICK] = 24,
    [SCR_QSCONFIG] = 25, [SCR_COLORPICK] = 26, [SCR_DATETIME] = 27,
    [SCR_WIFI] = 28, [SCR_WIFI_INFO] = 29, [SCR_BT] = 30,
    [SCR_BT_INFO] = 31, [SCR_WORKMODE] = 32, [SCR_LASTFM] = 33,
    [SCR_DEBUG] = 34, [SCR_CASTPICK] = 40, [SCR_CUSTOM_PICKER] = 35, [SCR_CUSTOM_PAGE] = 36,
    [SCR_CUSTOM_OVERLAYS] = 37, [SCR_CUSTOM_FONT] = 38, [SCR_CUSTOM_PREVIEW] = 39, [SCR_DEVICE_NAMES] = 41,
};
int screen_figure_number(int which){
    return which >= 0 && which < SCR_COUNT ? s_figure[which] : 0;
}
/* the screens among which the Custom presentation draft lives; leaving them by ANY route (header Back, swipe, Home, drawer, saver) drops it.
 * A plain dim or screen-off without the saver leaves the screen current, so it keeps the draft (slice 17d rule). */
static int custom_area(int s){ return s == SCR_CUSTOM_PAGE || s == SCR_CUSTOM_OVERLAYS || s == SCR_CUSTOM_FONT || s == SCR_CUSTOM_PICKER || s == SCR_CUSTOM_PREVIEW; }
static int s_stack[16];
static int s_sp = 0;
#define PUSH_MS 300   /* entrance (cubic ease-out: covers distance fast, settles gently) */
#define POP_MS  240   /* exit faster than entrance - a premium-motion reflex */
static int s_anim = 1;   /* slide transitions; disabled by /usr/data/anim_off */

static lv_obj_t *screen_make_root(lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(root, 0, 0);
    theme_screen_bg(root);   /* canvas colour (+ the theme's dot grid, if it has one) */
    /* full black square; the physical round bezel masks the shape. Clipping to a
     * circle here just exposed the lighter screen behind at the corners. */
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    /* Slide is the only GPU-less-smooth transition (a blit, not a resample). Each root carries a
     * 1px leading-edge hairline so the sliding boundary reads as a card edge. */
    anim_panel_shadow(root);
    return root;
}

/* Reset a root to its rest state (x=0, full opacity, 100% scale) - used by the up-front cleanup and
 * the completion callbacks so a screen left mid-zoom by an interrupt is always normalized. */
static void root_rest(lv_obj_t *o)
{
    lv_obj_set_x(o, 0);
    lv_obj_set_style_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_scale_x(o, 256, 0);
    lv_obj_set_style_transform_scale_y(o, 256, 0);
}


static void show_raw(int which)
{
    if (which < 0 || which >= SCR_COUNT) return;

    for (int i = 0; i < SCR_COUNT; i++) {
        if (s_roots[i]) lv_obj_add_flag(s_roots[i], LV_OBJ_FLAG_HIDDEN);
    }

    if (s_roots[which]) {
        lv_obj_clear_flag(s_roots[which], LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_roots[which]);
    }
    ui_np_title_scroll(ui_screen_backlight_on());
}

/* hide + reset the depth scrim (transition finished or was interrupted). Delete any live scrim
 * fade FIRST: with the anim-cap snap path a fade can still be alive here and would otherwise
 * rewrite the opacity back to 36 after this reset, breaking the hidden-rest invariant. */
static void scrim_off(void)
{
    s_scrim_panel = NULL;
    if (s_scrim) {
        lv_anim_delete(s_scrim, NULL);
        lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(s_scrim, LV_OPA_TRANSP, 0);
    }
}

static void anim_done_hide(lv_anim_t *a)
{
    lv_obj_t *o = (lv_obj_t *)a->var;
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    root_rest(o);        /* it faded/scaled out - restore rest state for next time it's shown */
    ui_np_title_scroll(ui_screen_backlight_on());
    scrim_off();
}

/* Forward-push completion: the incoming screen finished sliding in on top, so hide the screen it
 * now fully covers (tracked in s_push_hide). lv_anim_delete never fires a completed-cb, so an
 * interrupted push leaves the stale index unused until the next push overwrites it - and the
 * up-front straggler cleanup hides any covered root in that case. */
static int s_push_hide = -1;
static void anim_done_push(lv_anim_t *a)
{
    (void)a;
    int h = s_push_hide; s_push_hide = -1;
    if (h >= 0 && h < SCR_COUNT && s_roots[h]) {
        lv_obj_add_flag(s_roots[h], LV_OBJ_FLAG_HIDDEN);
        root_rest(s_roots[h]);
    }
    ui_np_title_scroll(ui_screen_backlight_on());
    scrim_off();
}

/* Leave hooks also run for entries discarded by a history unwind. */
static void screen_leave(int from, int to)
{
    /* Any screen change dismisses transient lv_layer_top popups (e.g. the duplicate-add
     * confirm dialog) so they can't survive onto another screen and act on stale state. */
    npmenu_close_transients();
    ui_np_close_overlays();   /* dismiss the sleep-timer popover so it can't float onto another screen */
    static int saver_custom;   /* the saver temporarily covers the custom area */
    if ((custom_area(from) || (from == SCR_SAVER && saver_custom)) &&
        !custom_area(to) && to != SCR_SAVER) custom_ov_draft_discard();
    if (to != SCR_SAVER) saver_custom = 0;
    else if (from != SCR_SAVER) saver_custom = custom_area(from);
    if (from == SCR_CUSTOM_PREVIEW && to != from) custom_preview_leave();   /* every way off the Preview: STOP */
    if (from == SCR_CUSTOM_PICKER && to != from) custom_picker_leave();   /* every way off the image list: timer and scan end */
    if (from == SCR_SAVER && to != SCR_SAVER) saver_leave();   /* every way off the saver: wake, screen off, Back, a screen_show; deletes Custom (slice 14) */

    if (from == SCR_CASTPICK && to != from) cast_ui_leave();   /* 17a: every exit stops discovery */
    if(from == SCR_NPHUB && to != from) nphub_poll_sync(0);
}

/* Slide between screens. dir = +1 push (new from right), -1 pop (new from left). */
static void transition(int from, int to, int dir)
{
    lv_obj_t *nw = (to >= 0 && to < SCR_COUNT) ? s_roots[to] : NULL;
    lv_obj_t *od = (from >= 0 && from < SCR_COUNT) ? s_roots[from] : NULL;

    screen_leave(from, to);

    /* Re-sync screens built once, on EVERY entry incl. back-navigation (screen_back also
     * routes through here - screen_show alone missed the back case, leaving stale labels
     * e.g. after editing Custom EQ / a setting detail page). */
    if (to == SCR_CASTPICK && to != from) cast_ui_enter();
    if (to == SCR_SETTINGS) settings_refresh_list();
    else if (to == SCR_SETLIST) setlist_refresh();          /* one category's rows, rebuilt per entry */
    else if (to == SCR_SETTING_DETAIL) setting_detail_refresh();  /* stay live if changed elsewhere (e.g. drawer EQ) */
    else if (to == SCR_QSCONFIG) qsconfig_refresh();        /* tile picker, rebuilt per entry */
    else if (to == SCR_QUICK) quicksettings_build();        /* drawer rebuilt from config on every open */
    else if (to == SCR_EQ) eqcustom_refresh();              /* re-resolve edited USER slot */
    else if (to == SCR_ALBUMWALL) albumwall_refresh();      /* cover-flow album browser */
    else if (to == SCR_TUNE)  tune_refresh();
    else if (to == SCR_UPNEXT) upnext_refresh();   /* the live queue, read on every entry */
    else if (to == SCR_DATETIME) datetime_refresh();   /* starts from the current time on every entry */
    else if (to == SCR_CUSTOM_PAGE) settings_custom_refresh();   /* the draft survives the picker; saved-image state is read fresh */
    else if (to == SCR_CUSTOM_OVERLAYS) settings_overlays_refresh();   /* from the draft, which the Custom page owns */
    else if (to == SCR_CUSTOM_FONT) settings_font_refresh();
    else if (to == SCR_CUSTOM_PREVIEW) custom_preview_enter();   /* reads the draft; never begins one explicitly (a read begins one only if none exists) */
    else if (to == SCR_CUSTOM_PICKER) custom_picker_enter();   /* a fresh scan of CustomCover on every entry */
    else if (to == SCR_NPHUB) nphub_refresh();   /* book-aware hub (Chapters for audiobooks) */
    else if (to == SCR_NOWPLAYING) ui_np_enter();
    else if (to == SCR_SAVER) saver_show_sync();
    else if (to == SCR_PLVIEW) plview_refresh();   /* fresh song list every entry (no stale tap positions) */
    else if (to == SCR_LIBRARY) library_enter(); /* pick up playlists created (NP New Playlist) or imported
                                                    * (Settings) elsewhere, without needing a restart */

    kit_pass(nw);   /* the active theme styles anything new on the incoming screen (no-op for Default) */
    mclock_sync(ui_screen_backlight_on()); /* set the incoming colon before its root becomes visible */
    kit_cursor_sync(ui_screen_backlight_on());

    /* Cancel any in-flight animation on BOTH screens + reset their offset up front, so a
     * stale anim_done_hide from a prior fast transition can't fire later and hide the new
     * current screen. Covers every path below incl. show_raw and the NP/hub special case. */
    if (nw) { lv_anim_delete(nw, NULL); root_rest(nw); }
    if (od) { lv_anim_delete(od, NULL); root_rest(od); }

    /* Also normalize any OTHER root a prior interrupted transition left mid-flight (e.g. a screen
     * still scaling/fading on top): delete its anim, hide it, restore rest state. Runs BEFORE every
     * path below so a stray root can never linger over - or steal input from - the new screen. */
    for (int i = 0; i < SCR_COUNT; i++) {
        if (i == to || i == from || !s_roots[i]) continue;
        lv_anim_delete(s_roots[i], NULL);
        lv_obj_add_flag(s_roots[i], LV_OBJ_FLAG_HIDDEN);
        root_rest(s_roots[i]);
    }
    if (s_scrim) { lv_anim_delete(s_scrim, NULL); scrim_off(); }   /* scrim unused by zoom; keep clear */

    if (!nw || to == from || !s_anim) { show_raw(to); return; }
    /* Full-screen overlays (screensaver, quick-settings pulldown) are takeovers, not hierarchical
     * navigation - show/hide instantly. */
    if (to == SCR_SAVER || from == SCR_SAVER) { show_raw(to); return; }
    if (to == SCR_QUICK || from == SCR_QUICK) { show_raw(to); return; }

    /* ---- UNIFIED push/pop - ONE motion language for the whole UI ----------------------------
     * The top screen SLIDES horizontally over a STATIC screen beneath (a blit - the only smooth
     * motion on this GPU-less renderer; scale/zoom resamples and stutters). Leading-edge hairline
     * + a subtle depth scrim on the covered screen. Only ONE screen moves; the changing scrim
     * still redraws the scene beneath. Expo-out easing;
     * exit faster than entrance. Forward: new screen in from the right. Back: current out to the
     * right, revealing the previous. */
    if (dir > 0) {                                  /* FORWARD: `nw` slides in over static `od` */
        s_scrim_panel = nw;
        if (od) { lv_obj_set_x(od, 0); lv_obj_clear_flag(od, LV_OBJ_FLAG_HIDDEN); }
        if (s_scrim && od) {                        /* dim the covered screen: recedes into depth */
            lv_obj_set_style_opa(s_scrim, LV_OPA_TRANSP, 0);
            lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_scrim);        /* scrim above od, below nw */
            anim_scrim_fade(s_scrim, 1, PUSH_MS);
        }
        lv_obj_set_x(nw, SCR_W);
        lv_obj_clear_flag(nw, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(nw);                 /* incoming on top: gets input */
        s_push_hide = (od && from != to) ? from : -1;   /* hide the covered screen when the slide ends */
        anim_page_slide(nw, SCR_W, 0, PUSH_MS, anim_done_push);
    } else {                                        /* BACK: `od` slides out, revealing static `nw` */
        s_scrim_panel = od;
        lv_obj_set_x(nw, 0);
        lv_obj_clear_flag(nw, LV_OBJ_FLAG_HIDDEN);
        if (s_scrim) {                              /* revealed screen lifts out of depth */
            lv_obj_set_style_opa(s_scrim, 36, 0);
            lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_scrim);        /* scrim above nw, below od */
            anim_scrim_fade(s_scrim, 0, POP_MS);
        }
        if (od) {
            lv_obj_set_x(od, 0);
            lv_obj_clear_flag(od, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(od);             /* outgoing on top slides away to the right */
            anim_page_slide(od, 0, SCR_W, POP_MS, anim_done_hide);
        } else {
            lv_obj_move_foreground(nw);
        }
    }
    ui_np_title_scroll(ui_screen_backlight_on());
}

/* These views describe the one playback session, including receiver playback
 * on the NP root and its Beam picker. Browsers, source/output settings and
 * temporary takeovers (Quick Settings and saver) retain ordinary push behavior. */
static int screen_single_instance(int which)
{
    switch (which) {
    case SCR_NOWPLAYING: case SCR_UPNEXT: case SCR_LYRICS: case SCR_CHAPTERS:
    case SCR_SONGINFO: case SCR_NPMENU: case SCR_NPHUB: case SCR_TUNE:
    case SCR_EQ: case SCR_CASTPICK:
        return 1;
    default:
        return 0;
    }
}

/* Discard a suffix without entering or drawing the screens in between. Leave
 * hooks are idempotent: covered screens have already left once, but resources
 * such as BT info can remain live until their history entry is removed. */
static void screen_unwind(int depth, int which)
{
    int from = s_current;
    if (from == SCR_BT_INFO) bt_info_close();
    for (int i = s_sp - 1; i > depth; i--) {
        int removed = s_stack[i];
        if (removed == SCR_BT_INFO) bt_info_close();
        screen_leave(removed, which);
    }
    s_sp = depth;
    s_current = which;
    transition(from, which, -1);
}

void screen_show(int which)
{
    if (which < 0 || which >= SCR_COUNT) return;
    if (screen_single_instance(which) && which != s_current) {
        for (int i = s_sp - 1; i >= 0; i--) {
            if (s_stack[i] == which) { screen_return_to(which); return; }
        }
    }
    int from = s_current;
    if (which != s_current) {
        int cap = (int)(sizeof(s_stack)/sizeof(s_stack[0]));
        if (s_sp >= cap) {            /* full: keep the root (s_stack[0]) so Back still
                                       * reaches Home; drop the 2nd-oldest instead */
            for (int i = 2; i < cap; i++) s_stack[i-1] = s_stack[i];
            s_sp = cap - 1;
        }
        s_stack[s_sp++] = s_current;
    }
    s_current = which;
    transition(from, which, +1);
}

/* Explicit returns also support ordinary screens: use their nearest occurrence. */
void screen_return_to(int which)
{
    if (which < 0 || which >= SCR_COUNT) return;
    if (which == s_current) { screen_show(which); return; }
    for (int i = s_sp - 1; i >= 0; i--) {
        if (s_stack[i] != which) continue;
        screen_unwind(i, which);
        return;
    }
    screen_show(which);
}

/* Pop the nav stack (swipe / back gesture). Home is the root: no-op. */
void screen_back(void)
{
    if (s_current == SCR_CUSTOM_PICKER && custom_picker_back()) return;   /* a preview is open: Back closes it, the list stays */
    if (s_sp <= 0) return;
    screen_unwind(s_sp - 1, s_stack[s_sp - 1]);
}

/* Home swipe: close every screen above Home in one step. The current screen goes the way a Back takes it (its
 * leave hooks run in transition) and Home is revealed directly, so no screen in between is drawn. Screens further
 * down get leave cleanup without being entered or drawn. Library reopens at its top level. Refused like Back
 * while the image list applies a picture. Returns 1 when it went Home. */
int screen_home(void)
{
    if (s_current == SCR_HOME) return 0;
    if (s_current == SCR_CUSTOM_PICKER && custom_picker_busy()) return 0;
    library_reset_top();
    screen_unwind(0, SCR_HOME);
    return 1;
}

/* Something visible on the top layer that takes touch owns the screen (the same rule the update check uses; toasts and
 * the touch dot take none). */
int screen_modal_open(void)
{
    lv_obj_t *top = lv_layer_top();
    uint32_t n = lv_obj_get_child_count(top);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *o = lv_obj_get_child(top, (int32_t)i);
        if (!lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE)) return 1;
    }
    return 0;
}

/* The press landed on a control that owns a drag of its own: a slider, an arc or roller, or a container that scrolls
 * sideways (the Custom EQ strip). Such a press keeps today's time-limited Back rule: a fast slide still goes Back, a
 * slow one is the control's. */
int screen_on_drag_control(int x, int y)
{
    lv_point_t p = { x, y };
    lv_obj_t *o = lv_indev_search_obj(lv_layer_top(), &p);
    if (!o) o = lv_indev_search_obj(lv_screen_active(), &p);
    for (; o; o = lv_obj_get_parent(o)) {
        if (lv_obj_check_type(o, &lv_slider_class) || lv_obj_check_type(o, &lv_arc_class) ||
            lv_obj_check_type(o, &lv_roller_class)) return 1;
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE) && (lv_obj_get_scroll_dir(o) & LV_DIR_HOR) &&
            (lv_obj_get_scroll_left(o) > 0 || lv_obj_get_scroll_right(o) > 0)) return 1;
    }
    return 0;
}

lv_obj_t *screen_get_root(int which)
{
    if (which < 0 || which >= SCR_COUNT) return NULL;
    return s_roots[which];
}

int screen_current(void){ return s_current; }

void screens_init(void)
{
    if (s_roots[SCR_HOME]) {
        screen_show(SCR_HOME);
        return;
    }

    s_anim = cfg_get_int("anim", 1);
    if (access("/usr/data/anim_off", 0) == 0) s_anim = 0;   /* legacy override */

    kit_install();                                   /* the theme's styling pass, before any screen is shown */
    lv_obj_t *parent = lv_screen_active();
    lv_obj_set_style_bg_color(parent, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    /* The screen container must not scroll: during a slide, the incoming screen sits off-screen
     * to the right (x=+360), which overflows the parent and makes LVGL draw a horizontal
     * scrollbar along the bottom that slides in with it. Fixed container -> no stray bar. */
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);

    s_roots[SCR_HOME] = screen_make_root(parent);
    s_roots[SCR_LIBRARY] = screen_make_root(parent);
    s_roots[SCR_NOWPLAYING] = screen_make_root(parent);
    s_roots[SCR_SETTINGS] = screen_make_root(parent);
    s_roots[SCR_SETTING_DETAIL] = screen_make_root(parent);
    s_roots[SCR_SEARCH] = screen_make_root(parent);
    s_roots[SCR_SAVER] = screen_make_root(parent);
    s_roots[SCR_QUICK] = screen_make_root(parent);
    s_roots[SCR_SONGINFO] = screen_make_root(parent);
    s_roots[SCR_NPMENU] = screen_make_root(parent);
    s_roots[SCR_TUNE] = screen_make_root(parent);
    s_roots[SCR_EQ] = screen_make_root(parent);
    s_roots[SCR_APPS] = screen_make_root(parent);
    s_roots[SCR_NPHUB] = screen_make_root(parent);
    s_roots[SCR_PLPICK] = screen_make_root(parent);
    s_roots[SCR_PLVIEW] = screen_make_root(parent);
    s_roots[SCR_WIFI] = screen_make_root(parent);
    s_roots[SCR_WIFI_INFO] = screen_make_root(parent);
    s_roots[SCR_BT] = screen_make_root(parent);
    s_roots[SCR_BT_INFO] = screen_make_root(parent);
    s_roots[SCR_WEATHER] = screen_make_root(parent);
    s_roots[SCR_LYRICS] = screen_make_root(parent);
    s_roots[SCR_COLORPICK] = screen_make_root(parent);
    s_roots[SCR_LASTFM] = screen_make_root(parent);
    s_roots[SCR_WORKMODE] = screen_make_root(parent);
    s_roots[SCR_DEBUG] = screen_make_root(parent);
    s_roots[SCR_FOLDER] = screen_make_root(parent);
    s_roots[SCR_BOOKS] = screen_make_root(parent);
    s_roots[SCR_CHAPTERS] = screen_make_root(parent);
    s_roots[SCR_SETLIST] = screen_make_root(parent);
    s_roots[SCR_QSCONFIG] = screen_make_root(parent);
    s_roots[SCR_ALBUMWALL] = screen_make_root(parent);
    s_roots[SCR_UPNEXT] = screen_make_root(parent);
    s_roots[SCR_DATETIME] = screen_make_root(parent);
    s_roots[SCR_CASTPICK] = screen_make_root(parent);
    s_roots[SCR_CUSTOM_PICKER] = screen_make_root(parent);
    s_roots[SCR_CUSTOM_PAGE] = screen_make_root(parent);
    s_roots[SCR_CUSTOM_OVERLAYS] = screen_make_root(parent);
    s_roots[SCR_CUSTOM_FONT] = screen_make_root(parent);
    s_roots[SCR_CUSTOM_PREVIEW] = screen_make_root(parent);
    s_roots[SCR_DEVICE_NAMES] = screen_make_root(parent);

    /* depth scrim: a full-screen translucent-black overlay, created LAST so it sits above the
     * roots in sibling order; re-parented in z during a transition to dim the screen beneath the
     * moving panel. Hidden at rest. */
    s_scrim = lv_obj_create(parent);
    lv_obj_remove_style_all(s_scrim);
    lv_obj_set_size(s_scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_scrim, 0, 0);
    lv_obj_set_style_bg_color(s_scrim, TC(SCRIM), 0);
    lv_obj_set_style_bg_opa(s_scrim, LV_OPA_COVER, 0);   /* object opacity (animated) gates visibility */
    lv_obj_set_style_opa(s_scrim, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_scrim, scrim_clip, LV_EVENT_DRAW_MAIN_BEGIN, NULL);

    home_create(s_roots[SCR_HOME]);
    weather_app_create(s_roots[SCR_WEATHER]);
    lyrics_create(s_roots[SCR_LYRICS]);
    library_create(s_roots[SCR_LIBRARY]);
    ui_create(s_roots[SCR_NOWPLAYING]);
    settings_create(s_roots[SCR_SETTINGS]);
    setting_detail_create(s_roots[SCR_SETTING_DETAIL]);
    setlist_create(s_roots[SCR_SETLIST]);
    qsconfig_create(s_roots[SCR_QSCONFIG]);
    albumwall_create(s_roots[SCR_ALBUMWALL]);
    search_create(s_roots[SCR_SEARCH]);
    saver_create(s_roots[SCR_SAVER]);
    quicksettings_create(s_roots[SCR_QUICK]);
    songinfo_create(s_roots[SCR_SONGINFO]);
    npmenu_create(s_roots[SCR_NPMENU]);
    tune_create(s_roots[SCR_TUNE]);
    eqcustom_create(s_roots[SCR_EQ]);
    colorpick_create(s_roots[SCR_COLORPICK]);
    modes_create(s_roots[SCR_WORKMODE]);
    debug_create(s_roots[SCR_DEBUG]);
    folderbrowser_create(s_roots[SCR_FOLDER]);
    books_create(s_roots[SCR_BOOKS]);
    chapters_create(s_roots[SCR_CHAPTERS]);
    upnext_create(s_roots[SCR_UPNEXT]);
    datetime_create(s_roots[SCR_DATETIME]);
    cast_ui_create(s_roots[SCR_CASTPICK]);
    custom_picker_create(s_roots[SCR_CUSTOM_PICKER]);
    settings_custom_create(s_roots[SCR_CUSTOM_PAGE]);
    settings_overlays_create(s_roots[SCR_CUSTOM_OVERLAYS]);
    settings_font_create(s_roots[SCR_CUSTOM_FONT]);
    custom_preview_create(s_roots[SCR_CUSTOM_PREVIEW]);
    dnui_create(s_roots[SCR_DEVICE_NAMES]);
    apps_create(s_roots[SCR_APPS]);
    nphub_create(s_roots[SCR_NPHUB]);
    plpick_create(s_roots[SCR_PLPICK]);
    plview_create(s_roots[SCR_PLVIEW]);
    wifi_create(s_roots[SCR_WIFI]);
    wifi_info_create(s_roots[SCR_WIFI_INFO]);
    bt_create(s_roots[SCR_BT]);
    bt_info_create(s_roots[SCR_BT_INFO]);
    lastfm_create(s_roots[SCR_LASTFM]);

    screen_show(SCR_HOME);
}

void screen_set_anim(int on){ s_anim = on ? 1 : 0; }
int screen_anim_on(void){ return s_anim; }
