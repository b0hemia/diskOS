/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "modes.h"
#include "receiver.h"
#include "cast_handoff.h"
#include "config.h"
#include "i18n.h"
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <glob.h>

/* Audio Source picker (stock: Working mode) - mirrors stock's "Working mode" list. Tapping a mode replays
 * the captured V2.28 switch sequence via ui_set_source_mode() and marks it selected.
 * 0=Local 1=USB-DAC 2=BT-Receiving 3=USB-Storage. USB Audio (stock's "USB AUDIO"), is an OUTPUT
 * route of the Local source (the Disc feeds an external USB DAC), not a source mode: see the output section. */

typedef struct { int action; const char *name, *sub; } modeinfo_t;
static const modeinfo_t MODES[] = {
    { 0, "Local Playback",      "Play from the microSD card" },
    { 2, "Bluetooth Receiving", "Play audio sent from a phone" },
    { SOURCE_AIRPLAY, "AirPlay Receiving", "Play audio sent from an iPhone or Mac" },
    { 1, "USB DAC",             "Be a USB sound card for a PC" },
    { 3, "USB Storage",         "Open the card on a computer" },
#ifdef DISKOS_TEST_OUTPUTS
    { SOURCE_USB_AUDIO, "USB Audio",           "Play through an external USB DAC" },   /* device-unverified: test builds only */
#endif
};
#define N_MODES ((int)(sizeof(MODES)/sizeof(MODES[0])))
static int mode_row(int action){
    for(int i=0;i<N_MODES;i++) if(MODES[i].action == action) return i;
    return -1;
}

static lv_obj_t *g_check[N_MODES];   /* per-row checkmark label */
static lv_obj_t *g_row[N_MODES];     /* per-row button (for the selected highlight) */

/* OUTPUT-ROUTE-BEGIN */
/* Output route: Internal DAC / SPDIF / USB Audio (SPDIF row + USB Audio picker row: -DDISKOS_TEST_OUTPUTS builds only).
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
#ifndef OUT_USB_CARD
#define OUT_USB_CARD "/proc/asound/card1"   /* the USB DAC card; its usbid node exists for USB audio cards only (UNVERIFIED on this kernel) */
#endif
#define OUT_QUIET_MS HO_QUIET_MS
#define OUT_WAIT_MS  3000
#define OUT_WORKMODE "0657000C0008"
#define OUT_PAUSE    "0201000C0000"
typedef struct { const char *route, *gadget; unsigned settle_us; } out_seq_t;
static const out_seq_t OUT_SEQ[3] = {
    { "0666000C0006", "0642000C0000", 65000 },   /* OUT_INTERNAL */
    { "0666000C0004", "0642000C0000", 65000 },   /* OUT_SPDIF */
    { "0666000C0003", "0642000C0005", 0 },       /* OUT_USB */
};
static void modes_ui_refresh(void);
static int g_out_route = OUT_INTERNAL;
static unsigned g_out_gen = 0;
static struct { int target, was_playing, full; unsigned gen; char path[TRACK_PATH_MAX]; long pos; uint32_t t0, quiet0; lv_timer_t *tm; } g_osw;
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
static struct { int target, via_mode; uint32_t t0; lv_timer_t *tm; } g_cp;   /* 16b: an output selection waiting for Cast to end */
int modes_output_busy(void){ return g_osw.tm != NULL || g_rec || g_cp.tm != NULL; }
static int out_send(const char *f, int automatic, unsigned gen){
    return automatic ? ipc_send_automatic(f, gen) : ipc_send_cmd(f);
}
/* Route-aware local init. with_gadget (the boot timer): internal/SPDIF send 0642 0 first; USB always pairs 0666 3 with 0642 5. */
int modes_local_init(int with_gadget){
    unsigned gen = ipc_generation();
    if(with_gadget && ui_player_settling()) return -1;
    int r = modes_output_route();
    if(r != OUT_USB && with_gadget && out_send(OUT_SEQ[r].gadget, with_gadget, gen) != 0) return -1;
    int rc = out_send(OUT_SEQ[r].route, with_gadget, gen) != 0 ? -1 : 0;
    if(rc != 0 && with_gadget) return -1;   /* the boot timer retries the whole init next tick */
    /* the play preamble (with_gadget 0) is best effort like the old inline pair: 0657 8 is attempted whatever 0666 returned */
    if(r == OUT_USB && out_send(OUT_SEQ[r].gadget, with_gadget, gen) != 0) rc = -1;
    if(out_send(OUT_WORKMODE, with_gadget, gen) != 0) rc = -1;
    return rc;
}
/* 1 only when every playback PCM node is readable and none is RUNNING or DRAINING; unreadable / no nodes = not proven */
static int out_pcm_probe(int require_closed){
    glob_t g; int quiet = 1;
    if(glob(OUT_PCM_GLOB, 0, NULL, &g) != 0) return 0;
    for(size_t i = 0; i < g.gl_pathc && quiet; i++){
        FILE *f = fopen(g.gl_pathv[i], "r"); char l[96] = "";
        if(!f){ quiet = 0; break; }
        if(!fgets(l, sizeof l, f)) l[0] = 0;
        fclose(f);
        if(!l[0] || strstr(l, "RUNNING") || strstr(l, "DRAINING")) quiet = 0;
        if(require_closed && strcmp(l, "closed\n") != 0 && strcmp(l, "closed") != 0) quiet = 0;
    }
    globfree(&g);
    return quiet;
}
static int out_pcm_quiet(void){ return out_pcm_probe(0); }
int modes_receiver_quiet(void){ return out_pcm_probe(1); }
int modes_local_teardown(unsigned generation){ return ipc_send_automatic("0657000C0008", generation); }
static int out_usb_card_ok(void){
    char p[96]; snprintf(p, sizeof p, "%s/usbid", OUT_USB_CARD);
    return access(p, F_OK) == 0;
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
    unsigned gen;
    if(!ipc_confirmed_snapshot(&gen) || gen != g_rec_gen) return;
    if(ipc_send_automatic(OUT_SEQ[OUT_INTERNAL].route, g_rec_gen) != 0) return;
    if(OUT_SEQ[OUT_INTERNAL].settle_us) usleep(OUT_SEQ[OUT_INTERNAL].settle_us);
    if(ipc_send_automatic(OUT_SEQ[OUT_INTERNAL].gadget, g_rec_gen) != 0) return;
    if(ipc_send_automatic(OUT_WORKMODE, g_rec_gen) == 0){
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
    if(!ho_quiet_step(&g_osw.quiet0, lv_tick_get(), ui_is_playing(), out_pcm_quiet(), OUT_QUIET_MS)) return;   /* shared with the Cast handoff */
    const char *why = ui_output_blocked();
    if(why){ osw_stop(why, g_osw.was_playing); return; }
    if(g_osw.target == OUT_USB && !out_usb_card_ok()){ osw_stop("USB DAC not found", g_osw.was_playing); return; }   /* re-checked right before 0666 3 */
    lv_timer_del(g_osw.tm); g_osw.tm = NULL;
    out_run();
}
static int out_switch(int target, int via_mode);
/* 16b: an explicit local output selection while a Cast session exists ends the session first (the existing user-stop path: receiver STOP, serving
 * revoked, release), then runs the same switch. The user sees "Stopping beam to switch output", then the normal switch toasts. Bounded by CP_WAIT_MS
 * (the machine's own STOP wait is 4 s); if the session is still there then, nothing is switched and the route is reported as it is. */
#define CP_WAIT_MS 6000u
static void cp_tick(lv_timer_t *t){
    (void)t;
    if(ui_cast_session_active() && lv_tick_elaps(g_cp.t0) < CP_WAIT_MS) return;
    int target = g_cp.target, via = g_cp.via_mode, still = ui_cast_session_active();
    lv_timer_del(g_cp.tm); g_cp.tm = NULL;
    if(still){ out_set_state(modes_output_route()); ui_toast("Couldn't switch output"); modes_ui_refresh(); return; }
    out_switch(target, via);
}
static int cast_then_switch(int target, int via_mode){
    g_cp.target = target; g_cp.via_mode = via_mode; g_cp.t0 = lv_tick_get();
    g_cp.tm = lv_timer_create(cp_tick, 100, NULL);
    if(!g_cp.tm) return -1;
    ui_cast_end_request();
    ui_toast("Stopping beam to switch output");
    return 0;
}
static int out_switch(int target, int via_mode){
    if(target < OUT_INTERNAL || target > OUT_USB) return -1;
    if(modes_output_busy()){ ui_toast("Switching..."); return -1; }
    if(ui_cast_session_active()) return cast_then_switch(target, via_mode);
    const char *why = ui_output_blocked();
    if(why){ out_set_state(modes_output_route()); ui_toast(why); return -1; }
    if(target == OUT_USB && !out_usb_card_ok()){ out_set_state(modes_output_route()); ui_toast("Connect a USB DAC first"); return -1; }
    int was_playing = ui_is_playing();
    if(!was_playing && !out_pcm_quiet()){ out_set_state(modes_output_route()); ui_toast("Player is busy - try again"); return -1; }   /* PCM live but not "playing": a pause toggle would START it */
    track_state_t st; ipc_get_state(&st);
    memset(&g_osw, 0, sizeof g_osw);
    g_osw.target = target; g_osw.was_playing = was_playing;
    g_osw.full = via_mode || target == OUT_USB || modes_output_route() == OUT_USB;   /* Settings toggle = stock's 2-frame form, except to/from USB */
    g_osw.gen = ipc_generation(); g_osw.pos = st.have_track ? st.position_ms : 0;
    snprintf(g_osw.path, sizeof g_osw.path, "%s", st.have_track ? st.path : "");
    if(was_playing && ipc_send_cmd(OUT_PAUSE) < 0){ out_set_state(modes_output_route()); ui_toast("Player is busy - try again"); return -1; }
    g_osw.t0 = lv_tick_get();
    g_osw.tm = lv_timer_create(osw_tick, 100, NULL);
    return 0;
}
int modes_output_switch(int target){ return out_switch(target, 0); }
int modes_output_mode_switch(int target){ return out_switch(target, 1); }
/* OUTPUT-ROUTE-END */
int modes_pcm_quiet(void){ return out_pcm_probe(1); }   /* Cast requires every playback PCM explicitly closed */

/* Actions are independent of row order in production and test layouts. */
static int shown_mode(int src){ return mode_row(src == 0 && modes_output_route() == OUT_USB ? SOURCE_USB_AUDIO : src); }

static void mark_selected_mode(int cur){
    for(int i=0;i<N_MODES;i++){
        if(g_check[i]){ lv_label_set_text(g_check[i], i==cur ? LV_SYMBOL_OK : "");
                        lv_obj_set_style_text_color(g_check[i], ui_current_accent(), 0); }  /* track accent changes */
        if(g_row[i]){   /* selected row gets an accent ring + slightly lifted fill */
            lv_obj_set_style_border_width(g_row[i], i==cur ? 2 : 0, 0);
            lv_obj_set_style_border_color(g_row[i], ui_current_accent(), 0);
            lv_obj_set_style_bg_color(g_row[i], (i==cur ? TC(SURFACE_SELECTED) : TC(SURFACE)), 0);
        }
    }
}
static void mark_selected(void){ mark_selected_mode(ui_source_switch_failed() ? -1 : shown_mode(ui_get_source_mode())); }
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
        if(g_check[i]){ lv_label_set_text(g_check[i], i==m ? LV_SYMBOL_REFRESH : "");
                        lv_obj_set_style_text_color(g_check[i], ui_current_accent(), 0); }
        if(g_row[i]){   /* highlight the row being switched to */
            lv_obj_set_style_border_width(g_row[i], i==m ? 2 : 0, 0);
            lv_obj_set_style_border_color(g_row[i], ui_current_accent(), 0);
            lv_obj_set_style_bg_color(g_row[i], (i==m ? TC(SURFACE_SELECTED) : TC(SURFACE)), 0);
        }
    }
}

static lv_timer_t *g_settle = NULL;
static int g_pending_mode = -1;   /* the mode a switch is settling to (so reopening the screen keeps showing "switching") */
/* M17: after the switch window, settle to the CONFIRMED mode read from the real USB gadget state,
 * not a blind assumption. If the gadget shows the switch didn't take, reflect reality + say so. */
static void settle_cb(lv_timer_t *t){
    (void)t;
    if(g_osw.tm) return;   /* an output switch is still waiting for the pause */
    if(ui_source_switch_pending()) return;
    if(lv_tick_elaps(g_last_switch) < 3200) return;
    lv_timer_del(g_settle); g_settle = NULL;
    int intended = g_pending_mode; g_pending_mode = -1;
    if(ui_source_switch_failed()){ mark_selected_mode(-1); return; }
    /* M17: DISPLAY the ACTUAL gadget state (read-only) instead of a blind timer assumption. Do NOT
     * mutate the intent mirror (g_source_mode) - it also guards coldplug, and a transient mid-transition
     * sample must not flip that guard. */
    int show = (intended >= 0) ? intended : ui_get_source_mode();
    if(intended == 0 || intended == 1 || intended == 3){   /* USB gadget modes are readback-confirmable */
        int actual = ui_detect_source_mode();
        show = actual;
        if(actual != intended) ui_toast("Mode didn't switch");
    }
    /* intended == 2 (BT receiving) is gadget-invisible and needs a phone to connect - no reliable
     * readback here, so show the intent without asserting a false confirmation. */
    if(intended == SOURCE_USB_AUDIO || intended == SOURCE_AIRPLAY) show = ui_get_source_mode();   /* USB Audio: not visible in the gadget state; the route we last set decides */
    mark_selected_mode(shown_mode(show));
}

static void row_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    int m = (int)(uintptr_t)lv_event_get_user_data(e);
    /* Serialise: ignore taps while the previous switch is still applying (the player's gadget
     * state-machine is asynchronous). NB we do NOT early-return on "same mode" - re-issuing must
     * always be allowed so Local works as a recover even if our cached mode is stale. */
    if(g_last_switch && lv_tick_elaps(g_last_switch) < 3000){ ui_toast("Switching..."); return; }
    if(m == SOURCE_USB_AUDIO){   /* USB Audio: an output route of the Local source, not a source mode */
        if(modes_output_mode_switch(OUT_USB) == 0){
            g_last_switch = lv_tick_get();
            g_pending_mode = m; mark_pending(mode_row(m));
            if(g_settle) lv_timer_del(g_settle);
            g_settle = lv_timer_create(settle_cb, 500, NULL);
            ui_toast("Switching to USB audio output");
        }
        return;
    }
    if(m == 0 && (ui_receiver_active() || ui_receiver_exit_needs_confirm())){
        ui_receiver_return();
        return;
    }
    if(ui_set_source_mode(m) == 0){
        g_last_switch = lv_tick_get();
        g_pending_mode = m;
        mark_pending(mode_row(m));      /* async switch in flight: show "switching", not a confirmed selection */
        if(g_settle) lv_timer_del(g_settle);
        g_settle = lv_timer_create(settle_cb, 500, NULL);   /* settle to the checkmark after the switch window */
        /* honest wording: the frames are queued; the async switch completes a moment later. */
        if(ui_get_source_mode() != -1){
            switch(m){
                case 0: ui_toast("Switching to local playback"); break;
                case 1: ui_toast("Switching to USB DAC"); break;
                case 2: ui_toast("Switching to Bluetooth receiving"); break;
                case 3: ui_toast("Switching to USB storage"); break;
            }
        }
    }
}

void modes_open(void){
    if(g_settle && g_pending_mode >= 0) mark_pending(mode_row(g_pending_mode));  /* a switch is still settling - keep showing it */
    else mark_selected();
    screen_show(SCR_WORKMODE);
}

void modes_create(lv_obj_t *root){
    if(cfg_get_int("spdif", 0)) cfg_set_int("spdif", 0);   /* the route is per player run: a fresh UI has not switched anything */
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    /* back button (kept out of the clipped top-left corner) */
    lv_obj_t *title = ui_header(root, tr("Source"));
    if(theme_preset() == 0) lv_obj_set_y(title, 54);

    /* vertical list of mode rows */
    lv_obj_t *col = lv_obj_create(root);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 260, 176); lv_obj_set_pos(col, 50, 100);
    lv_obj_set_style_pad_bottom(col, 44, 0);   /* last mode row scrolls clear of the round bezel */
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 6, 0);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);

    for(int i=0;i<N_MODES;i++){
        if(MODES[i].action == SOURCE_AIRPLAY && !receiver_available()) continue;
        lv_obj_t *row = lv_button_create(col);
        g_row[i] = row;
        lv_obj_remove_style_all(row);
        int subtitle = MODES[i].action != 0 && MODES[i].action != 2;
        lv_obj_set_size(row, 260, subtitle ? 60 : 52);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_bg_color(row, TC(SURFACE), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
        ui_on(row, row_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)MODES[i].action, "modes.row", UI_CORE);

        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, tr(MODES[i].name));
        lv_obj_set_pos(nm, 16, subtitle ? 7 : 16);
        lv_obj_set_width(nm, 218);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(nm, TF(UI_16), 0);
        lv_obj_set_style_text_color(nm, TC(TEXT_PRIMARY), 0);

        lv_obj_t *sb = lv_label_create(row);
        lv_label_set_text(sb, tr(MODES[i].sub));
        if(!subtitle) lv_obj_add_flag(sb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(sb, 16, 25);
        lv_obj_set_height(sb, 30);                      /* two lines keep the full receiver hint in the row */
        lv_obj_set_width(sb, 228);
        lv_label_set_long_mode(sb, LV_LABEL_LONG_WRAP);
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
