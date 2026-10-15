/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "modes.h"
#include "config.h"
#include "fwcaps.h"
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <glob.h>

/* Working Mode (audio source) picker - mirrors stock's "Working mode" list. Tapping a mode replays
 * the captured V2.28 switch sequence via ui_set_source_mode() and marks it selected.
 * 0=Local 1=USB-DAC 2=BT-Receiving 3=USB-Storage. The fifth row, USB Audio (stock's "USB AUDIO"), is an OUTPUT
 * route of the Local source (the Disc feeds an external USB DAC), not a source mode: see the output section. */

typedef struct {
    int mode;
    const char *name;
    const char *sub;
    const char *toast;
} modeinfo_t;

static const modeinfo_t MODES[] = {
    { 0, "Local Playback",      "Play from the microSD card",         "Switching to local playback" },
    { 1, "USB DAC",             "Be a USB sound card for a PC",       "Switching to USB DAC" },
    { 4, "USB Audio",           "Play through an external USB DAC",   "Switching to USB audio output" },
    { 2, "Bluetooth Receiving", "Play audio sent from a phone",        "Switching to Bluetooth receiving" },
    { 3, "USB Storage",         "Open the card on a computer",        "Switching to USB storage" },
};
#define N_MODES ((int)(sizeof(MODES)/sizeof(MODES[0])))

static lv_obj_t *g_check[N_MODES];   /* per-row checkmark label */
static lv_obj_t *g_row[N_MODES];     /* per-row button (for the selected highlight) */

/* OUTPUT-ROUTE-BEGIN */
/* Output route: Internal DAC / SPDIF (Settings SPDIF toggle: -DDISKOS_TEST_OUTPUTS builds only).
 * Stock V2.57 (mq_ui_257.dis):
 *   Settings SPDIF toggle (0x480510 -> 0x464f0c):  0666 <6|4>, 0657 8                       (no 0642, no sleep)
 *   Working Mode callback 0x46bbac (200 ms one-shot): internal/SPDIF (0x46bd28): 0666 <6|4>, usleep 65 ms, 0642 0, 0657 8
 *                                                    USB AUDIO (0x46bd90):       0666 3, 0642 5, 0657 8
 * 0666 payloads are the player's out modes (mq_player_257 set_out_device 0x47bf24): 6 local analog, 4 SPDIF_TX, 3
 * USB_HOST. The player's 0666 handler (0x4f1294) closes the player and re-inits the output; 0657 8 then sets
 * LOCALPLAYER again, so a 0666 not followed by 0657 8 leaves NO_WORK_MODE (dead player). Stock sends no pause;
 * diskOS does (0666 into a live stream killed mq_player): a switch is pause ->
 * PCM proven quiet for OUT_QUIET_MS -> route -> work mode -> resume. Leaving USB audio always uses the full sequence
 * (0642 0 drops the host request). USB_HOST needs the USB DAC card (else the player uses a silent USB_HOST_NULL
 * sink, 0x47c040), so diskOS re-checks it right before the frames and refuses instead.
 * If a send fails after the route moved, the internal sequence is retried until it fully lands on a live player
 * connection (recovery-pending: every play/route/source command is refused meanwhile). */
#ifndef OUT_PCM_GLOB
#define OUT_PCM_GLOB "/proc/asound/card*/pcm*p/sub*/status"
#endif
#define OUT_QUIET_MS 500
#define OUT_WAIT_MS  3000
#define OUT_WORKMODE "0657000C0008"
#define OUT_PAUSE    "0201000C0000"
typedef struct { const char *route, *gadget; unsigned settle_us; } out_seq_t;
static const out_seq_t OUT_SEQ[2] = {
    { "0666000C0006", "0642000C0000", 65000 },   /* OUT_INTERNAL */
    { "0666000C0004", "0642000C0000", 65000 },   /* OUT_SPDIF */
};
static void modes_ui_refresh(void);
static int g_out_route = OUT_INTERNAL;
static unsigned g_out_gen = 0;
static struct { int target, was_playing, full; unsigned gen; char path[256]; long pos; uint32_t t0, quiet0; lv_timer_t *tm; } g_osw;
static int g_rec = 0; static unsigned g_rec_gen = 0; static lv_timer_t *g_rec_tm = NULL;

static void out_set_state(int r){
    g_out_route = r; g_out_gen = ipc_generation();
    if(cfg_get_int("spdif", 0) != (r == OUT_SPDIF)) cfg_set_int("spdif", r == OUT_SPDIF);   /* the Settings toggle mirrors the real route, never a wish */
}
int modes_output_route(void){
    if(g_out_route != OUT_INTERNAL && g_out_gen != ipc_generation()){ out_set_state(OUT_INTERNAL); }   /* a restarted player is back on the internal DAC */
    return g_out_route;
}
void modes_output_reset(void){ out_set_state(OUT_INTERNAL); }
int modes_output_busy(void){ return g_osw.tm != NULL || g_rec; }
/* Route-aware local init. with_gadget (the boot timer): internal/SPDIF send 0642 0 first. */
int modes_local_init(int with_gadget){
    int r = modes_output_route();
    if(with_gadget && ipc_send_cmd(OUT_SEQ[r].gadget) < 0) return -1;
    int rc = ipc_send_cmd(OUT_SEQ[r].route) < 0 ? -1 : 0;
    if(rc < 0 && with_gadget) return -1;   /* the boot timer retries the whole init next tick */
    if(ipc_send_cmd(OUT_WORKMODE) < 0) rc = -1;
    return rc;
}
/* 1 only when every playback PCM node is readable and none is RUNNING or DRAINING; unreadable / no nodes = not proven */
static int out_pcm_quiet(void){
    glob_t g; int quiet = 1;
    if(glob(OUT_PCM_GLOB, 0, NULL, &g) != 0) return 0;
    for(size_t i = 0; i < g.gl_pathc && quiet; i++){
        FILE *f = fopen(g.gl_pathv[i], "r"); char l[96] = "";
        if(!f){ quiet = 0; break; }
        if(!fgets(l, sizeof l, f)) l[0] = 0;
        fclose(f);
        if(!l[0] || strstr(l, "RUNNING") || strstr(l, "DRAINING")) quiet = 0;
    }
    globfree(&g);
    return quiet;
}
/* 0 = whole sequence sent; -1 = the 0666 was refused (nothing changed); -2 = the route moved but the tail failed */
static int out_seq_send(int t, int full){
    if(ipc_send_cmd(OUT_SEQ[t].route) < 0) return -1;
    if(full){
        if(OUT_SEQ[t].settle_us) usleep(OUT_SEQ[t].settle_us);
        if(ipc_send_cmd(OUT_SEQ[t].gadget) < 0) return -2;
    }
    if(ipc_send_cmd(OUT_WORKMODE) < 0) return -2;
    return 0;
}
/* recovery-pending: retry the full internal sequence until it lands on the same live player connection */
static void out_rec_clear(void){ if(g_rec_tm){ lv_timer_del(g_rec_tm); g_rec_tm = NULL; } g_rec = 0; }
static void out_recover_tick(lv_timer_t *t){
    (void)t;
    if(ipc_generation() != g_rec_gen){ out_rec_clear(); out_set_state(OUT_INTERNAL); modes_ui_refresh(); return; }   /* a new player initialises itself */
    if(!ipc_is_ready()) return;
    if(out_seq_send(OUT_INTERNAL, 1) == 0){
        out_rec_clear(); out_set_state(OUT_INTERNAL);
        ui_toast("Internal DAC restored"); modes_ui_refresh();
    }
}
static void out_rec_enter(void){
    out_set_state(OUT_INTERNAL);
    if(g_rec) return;
    g_rec = 1; g_rec_gen = ipc_generation();
    g_rec_tm = lv_timer_create(out_recover_tick, 500, NULL);
}
/* resume only the same track, and only if the seek and the play toggle were accepted */
static void out_resume(int moved){
    if(!g_osw.was_playing) return;
    track_state_t st; ipc_get_state(&st);
    if(moved && (ipc_generation() != g_osw.gen || !st.have_track || strcmp(st.path, g_osw.path) != 0)){ ui_toast("Output switched - press play"); return; }
    if(moved && g_osw.pos > 0 && ui_seek_to(g_osw.pos) < 0){ ui_toast("Couldn't resume - press play"); return; }
    if(ipc_send_cmd(OUT_PAUSE) < 0) ui_toast("Couldn't resume - press play");   /* toggle: plays again */
}
static int out_run(void){
    int target = g_osw.target;
    int rc = out_seq_send(target, g_osw.full);
    if(rc == -1){ out_set_state(modes_output_route()); ui_toast("Couldn't switch output"); out_resume(0); modes_ui_refresh(); return -1; }
    if(rc == 0){ out_set_state(target); out_resume(1); modes_ui_refresh(); return 0; }
    /* the route moved but the tail failed: back to the internal DAC; never end without 0657 8 landing */
    if(out_seq_send(OUT_INTERNAL, 1) == 0){
        out_set_state(OUT_INTERNAL);
        ui_toast("Output switch failed - internal DAC restored");
        out_resume(1); modes_ui_refresh();
        return -1;
    }
    out_rec_enter();
    ui_toast("Output state uncertain - recovering");
    modes_ui_refresh();
    return -1;
}
static void osw_stop(const char *why, int resume){
    lv_timer_del(g_osw.tm); g_osw.tm = NULL;
    out_set_state(modes_output_route());
    ui_toast(why);
    if(resume) out_resume(0);
    modes_ui_refresh();
}
static void osw_tick(lv_timer_t *t){
    (void)t;
    track_state_t st; ipc_get_state(&st);
    if(ipc_generation() != g_osw.gen){ osw_stop("Player restarted - try again", 0); return; }
    if(strcmp(st.path, g_osw.path) != 0){ osw_stop("Track changed - try again", 0); return; }
    if(lv_tick_elaps(g_osw.t0) >= OUT_WAIT_MS){ osw_stop("Player is busy - try again", 0); return; }   /* deadline first: a late tick never routes, even with the hold complete */
    if(ui_is_playing() || !out_pcm_quiet()) g_osw.quiet0 = 0;
    else if(!g_osw.quiet0) g_osw.quiet0 = lv_tick_get() ? lv_tick_get() : 1;
    if(!g_osw.quiet0 || lv_tick_elaps(g_osw.quiet0) < OUT_QUIET_MS){
        return;
    }
    const char *why = ui_output_blocked();
    if(why){ osw_stop(why, g_osw.was_playing); return; }
    lv_timer_del(g_osw.tm); g_osw.tm = NULL;
    out_run();
}
int modes_output_switch(int target){
    if(target < OUT_INTERNAL || target > OUT_SPDIF) return -1;
    if(modes_output_busy()){ ui_toast("Switching..."); return -1; }
    const char *why = ui_output_blocked();
    if(why){ out_set_state(modes_output_route()); ui_toast(why); return -1; }
    int was_playing = ui_is_playing();
    if(!was_playing && !out_pcm_quiet()){ out_set_state(modes_output_route()); ui_toast("Player is busy - try again"); return -1; }   /* PCM live but not "playing": a pause toggle would START it */
    track_state_t st; ipc_get_state(&st);
    memset(&g_osw, 0, sizeof g_osw);
    g_osw.target = target; g_osw.was_playing = was_playing;
    g_osw.full = 0;   /* Settings toggle = stock's 2-frame form: 0666 then 0657 8 */
    g_osw.gen = ipc_generation(); g_osw.pos = st.have_track ? st.position_ms : 0;
    snprintf(g_osw.path, sizeof g_osw.path, "%s", st.have_track ? st.path : "");
    if(was_playing && ipc_send_cmd(OUT_PAUSE) < 0){ out_set_state(modes_output_route()); ui_toast("Player is busy - try again"); return -1; }
    g_osw.t0 = lv_tick_get();
    g_osw.tm = lv_timer_create(osw_tick, 100, NULL);
    return 0;
}
/* OUTPUT-ROUTE-END */

static void mark_selected_mode(int cur){
    for(int i=0;i<N_MODES;i++){
        int m = MODES[i].mode;
        if(g_check[i]){ lv_label_set_text(g_check[i], m==cur ? LV_SYMBOL_OK : "");
                        lv_obj_set_style_text_color(g_check[i], ui_current_accent(), 0); }  /* track accent changes */
        if(g_row[i]){   /* selected row gets an accent ring + slightly lifted fill */
            lv_obj_set_style_border_width(g_row[i], m==cur ? 2 : 0, 0);
            lv_obj_set_style_border_color(g_row[i], ui_current_accent(), 0);
            lv_obj_set_style_bg_color(g_row[i], (m==cur ? TC(SURFACE_SELECTED) : TC(SURFACE)), 0);
        }
    }
}
static void mark_selected(void){ mark_selected_mode(ui_source_switch_failed() ? -1 : ui_get_source_mode()); }
/* an output switch finished, failed or recovered (or the player restarted): show the real route in the picker and Settings */
static void modes_ui_refresh(void){
    mark_selected();
    setting_detail_refresh();
    setlist_refresh();
}


static uint32_t g_last_switch = 0;   /* debounce: a switch takes a few seconds to apply in the player */

/* Pending state: the tapped row shows a "switching" glyph (not the confirmed checkmark) while the
 * gadget switch is in flight - there is no source-mode completion readback, so after the switch
 * window we settle to the selection best-effort (matches the honest "Switching..." toast). */
static void mark_pending(int m){
    for(int i=0;i<N_MODES;i++){
        int row_m = MODES[i].mode;
        if(g_check[i]){ lv_label_set_text(g_check[i], row_m==m ? LV_SYMBOL_REFRESH : "");
                        lv_obj_set_style_text_color(g_check[i], ui_current_accent(), 0); }
        if(g_row[i]){   /* highlight the row being switched to */
            lv_obj_set_style_border_width(g_row[i], row_m==m ? 2 : 0, 0);
            lv_obj_set_style_border_color(g_row[i], ui_current_accent(), 0);
            lv_obj_set_style_bg_color(g_row[i], (row_m==m ? TC(SURFACE_SELECTED) : TC(SURFACE)), 0);
        }
    }
}

static lv_timer_t *g_settle = NULL;
static int g_pending_mode = -1;   /* the mode a switch is settling to (so reopening the screen keeps showing "switching") */
/* M17: after the switch window, settle to the CONFIRMED mode read from the real USB gadget state,
 * not a blind assumption. If the gadget shows the switch didn't take, reflect reality + say so. */
static void settle_cb(lv_timer_t *t){
    (void)t;
    if(ui_source_switch_pending()) return;
    if(lv_tick_elaps(g_last_switch) < 3200) return;
    lv_timer_del(g_settle); g_settle = NULL;
    int intended = g_pending_mode; g_pending_mode = -1;
    if(ui_source_switch_failed()){ mark_selected_mode(-1); return; }
    /* M17: DISPLAY the ACTUAL gadget state (read-only) instead of a blind timer assumption. Do NOT
     * mutate the intent mirror (g_source_mode) - it also guards coldplug, and a transient mid-transition
     * sample must not flip that guard. */
    int show = (intended >= 0) ? intended : ui_get_source_mode();
    if(intended == 0 || intended == 1 || intended == 3 || intended == 4){   /* USB modes are readback-confirmable */
        int actual = ui_detect_source_mode();
        show = actual;
        if(actual != intended) ui_toast("Mode didn't switch");
    }
    /* intended == 2 (BT receiving) is gadget-invisible and needs a phone to connect - no reliable
     * readback here, so show the intent without asserting a false confirmation. */
    mark_selected_mode(show);
}

static void row_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if(idx < 0 || idx >= N_MODES) return;
    int m = MODES[idx].mode;
    /* Serialise: ignore taps while the previous switch is still applying (the player's gadget
     * state-machine is asynchronous). NB we do NOT early-return on "same mode" - re-issuing must
     * always be allowed so Local works as a recover even if our cached mode is stale. */
    if(g_last_switch && lv_tick_elaps(g_last_switch) < 3000){ ui_toast("Switching..."); return; }
    g_last_switch = lv_tick_get();
    if(ui_set_source_mode(m) == 0){
        g_pending_mode = m;
        mark_pending(m);      /* async switch in flight: show "switching", not a confirmed selection */
        if(g_settle) lv_timer_del(g_settle);
        g_settle = lv_timer_create(settle_cb, 500, NULL);   /* settle to the checkmark after the switch window */
        ui_toast(MODES[idx].toast);
    }
}

void modes_open(void){
    if(g_settle && g_pending_mode >= 0) mark_pending(g_pending_mode);  /* a switch is still settling - keep showing it */
    else mark_selected();
    screen_show(SCR_WORKMODE);
}

void modes_create(lv_obj_t *root){
    if(cfg_get_int("spdif", 0)) cfg_set_int("spdif", 0);   /* the route is per player run: a fresh UI has not switched anything */
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    /* back button (kept out of the clipped top-left corner) */
    ui_header(root, "Working Mode");   /* shared standard header */

    /* vertical list of mode rows */
    lv_obj_t *col = lv_obj_create(root);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 300, 250); lv_obj_set_pos(col, 30, 76);
    lv_obj_set_style_pad_bottom(col, 44, 0);   /* last mode row scrolls clear of the round bezel */
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 8, 0);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);

    for(int i=0;i<N_MODES;i++){
        if(MODES[i].mode == 4 && !fw_has_usb_audio_out()){
            g_row[i] = NULL;
            g_check[i] = NULL;
            continue;
        }
        lv_obj_t *row = lv_button_create(col);
        g_row[i] = row;
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 268, 54);
        lv_obj_set_style_radius(row, 14, 0);
        lv_obj_set_style_bg_color(row, TC(SURFACE), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
        ui_on(row, row_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)i, "modes.row", UI_CORE);

        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, MODES[i].name);
        lv_obj_set_pos(nm, 16, 9);
        lv_obj_set_style_text_font(nm, TF(UI_16), 0);
        lv_obj_set_style_text_color(nm, TC(TEXT_PRIMARY), 0);

        lv_obj_t *sb = lv_label_create(row);
        lv_label_set_text(sb, MODES[i].sub);
        lv_obj_set_pos(sb, 16, 30);
        lv_obj_set_width(sb, 202);                       /* keep clear of the right-side checkmark */
        lv_label_set_long_mode(sb, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(sb, TF(UI_12), 0);
        lv_obj_set_style_text_color(sb, TC(TEXT_MUTED), 0);

        g_check[i] = lv_label_create(row);
        lv_label_set_text(g_check[i], "");
        lv_obj_align(g_check[i], LV_ALIGN_RIGHT_MID, -14, 0);
        lv_obj_set_style_text_color(g_check[i], ui_current_accent(), 0);
        lv_obj_set_style_text_font(g_check[i], TF(UI_18), 0);
    }
    mark_selected();
}
