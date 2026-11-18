/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* cast_ui.c - the Beam picker (Cast slice 17a). It lists the Cast receivers found on the LAN and starts a session with the one tapped.
 * Discovery runs only while the picker is on screen with the screen lit and no session exists: ONE worker thread at a time, bounded
 * (PK_MS), cancelled and joined (the avahi child is killed and reaped inside cast_disc_run) on every exit. The results are dropped
 * with the worker, so a list never outlives the visit, and a tap is refused when its row belongs to an older list or the list is
 * older than PK_FRESH_MS (the receiver may have left): it searches again instead. AirPlay: rows come from pk_tag() and P.d only;
 * the AirPlay sender merge adds its own receivers and tag there. The model (pk_*) has no LVGL, so the tests compile it alone. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "remote_browse.h"
#include "cast_ui.h"
#ifndef CAST_UI_MODEL_ONLY
#include "screens.h"
#include "wifi.h"
#include "i18n.h"
#include "theme.h"
#include "theme_kit.h"
#include "airplay_pin.h"
#include "airplay_ipc.h"
int ui_screen_backlight_on(void);
#else
int ui_cast_pick_request(const char *host, int port);
#endif
enum { PK_IDLE, PK_SEARCH, PK_LIST, PK_NONE, PK_FAIL, PK_NOWIFI };
#define PK_MS 5000u          /* one search, avahi's own deadline */
#define PK_FRESH_MS 30000u   /* a list older than this is searched again, never tapped */
static struct { int st, run, rc; unsigned gen; uint32_t t; pthread_t th; cast_disc_job job; remote_disc d; _Atomic int done; } P;
static const char *pk_tag(int i){ return P.d.r[i].protocol == RD_CAST ? "Cast" : P.d.r[i].protocol == RD_RAOP ? "AirPlay 1" : "AirPlay"; }   /* the seam: the AirPlay merge returns "AirPlay" for its rows */
static void *pk_work(void *a){
    (void)a;
#ifdef DISKOS_HOST
    P.rc = CD_NONE; /* host rendering must never browse the local network */
#else
    P.rc = remote_browse_run(&P.job, &P.d, PK_MS);
#endif
    atomic_store(&P.done, 1); return NULL;
}
static void pk_stop(void){   /* cancel + join: no worker, no child, no results survive */
    if(P.run){ cast_disc_cancel(&P.job); pthread_join(P.th, NULL); P.run = 0; }
    P.st = PK_IDLE; P.gen++; P.d.n = 0;
}
static void pk_start(uint32_t now, int wifi){
    P.gen++; P.d.n = 0;
    if(wifi != 1){ P.st = PK_NOWIFI; return; }
    memset(&P.job, 0, sizeof P.job); atomic_store(&P.done, 0);
    if(pthread_create(&P.th, NULL, pk_work, NULL)){ P.st = PK_FAIL; return; }
    P.run = 1; P.st = PK_SEARCH; P.t = now;
}
/* One poll: vis = picker on screen and lit; ph = ui_cast_phase() (0 = no session). Returns the state. */
static int pk_step(int vis, int ph, int wifi, uint32_t now){
    if(!vis || ph){ if(P.run || P.st != PK_IDLE) pk_stop(); }
    else if(wifi != 1){
        if(P.run || P.st != PK_NOWIFI) pk_stop();
        P.st = PK_NOWIFI;
    }
    else if(P.st == PK_IDLE || (P.st == PK_NOWIFI && wifi == 1)) pk_start(now, wifi);
    else if(P.run && atomic_load(&P.done)){
        pthread_join(P.th, NULL); P.run = 0; P.t = now;
        P.st = P.rc == CD_FOUND ? PK_LIST : P.rc == CD_NONE || P.rc == CD_TIMEOUT ? PK_NONE : PK_FAIL;
    }
    return P.st;
}
/* A tap on row i of the list drawn at generation gen: 0 session requested, 1 list too old (searching again), -1 refused. */
static int pk_tap(unsigned gen, int i, uint32_t now, int wifi){
    if(P.st != PK_LIST || gen != P.gen || i < 0 || (size_t)i >= P.d.n) return -1;
    if(wifi != 1){ pk_stop(); P.st = PK_NOWIFI; return 1; }
    if(now - P.t > PK_FRESH_MS){ pk_stop(); pk_start(now, wifi); return 1; }
    ui_cast_set_device(P.d.r[i].name);   /* keep the selected name even when admission refuses */
    if((P.d.r[i].protocol == RD_CAST ? ui_cast_pick_request(P.d.r[i].addr, P.d.r[i].port) : ui_airplay_pick_request(&P.d.r[i])) != 0) return -1;
    return 0;
}
#ifndef CAST_UI_MODEL_ONLY
static lv_obj_t *s_root, *s_body, *s_search; static lv_timer_t *s_tm; static int s_key, s_ph;
static int s_return_choice, s_keys_ok, s_dark; static unsigned s_return_attempt;
static void tick_cb(lv_timer_t *t);
static lv_obj_t *lbl(const char *t, int big, int muted){
    lv_obj_t *l = lv_label_create(s_body);
    lv_label_set_text(l, t); lv_label_set_long_mode(l, LV_LABEL_LONG_DOT); lv_obj_set_width(l, 230);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(l, big ? TF(UI_18) : TF(UI_12), 0);
    lv_obj_set_style_text_color(l, muted ? TC(TEXT_MUTED) : TC(TEXT_PRIMARY), 0);
    return l;
}
static void btn(lv_obj_t *par, const char *t, lv_event_cb_t cb, int primary, int w, int live){
    lv_obj_t *b = lv_button_create(par); lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, 44); lv_obj_set_style_radius(b, 20, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, primary ? ui_current_accent() : TC(SURFACE_RAISED), 0);
    lv_obj_set_style_bg_color(b, TC(RAISED_PRESSED), LV_STATE_PRESSED);
    if(live) ui_on(b, cb, LV_EVENT_CLICKED, NULL, "beam.button", UI_CORE);
    lv_obj_t *l = lv_label_create(b); lv_label_set_text(l, tr(t)); lv_obj_center(l);
    lv_obj_set_style_text_font(l, TF(UI_16), 0);
    lv_obj_set_style_text_color(l, !live ? TC(TEXT_DISABLED) : primary ? TC(ON_ACCENT) : TC(TEXT_PRIMARY), 0);
}
static void again_cb(lv_event_t *e){ (void)e; ui_airplay_clear_error(); pk_stop(); s_key = -1; tick_cb(NULL); }
static void volume_cb(lv_event_t *e){ (void)e; (void)ui_airplay_volume_query(); ui_show_volume(0); }
static void forget_cb(lv_event_t *e){ (void)e; (void)ui_airplay_forget(); }
static void return_cb(lv_event_t *e){ (void)e; s_return_choice = 1; s_return_attempt = ui_airplay_attempt(); s_key = -1; tick_cb(NULL); }
static void return_play_cb(lv_event_t *e){
    (void)e;
    if(s_return_attempt == ui_airplay_attempt()) (void)ui_airplay_return();
    s_return_choice = 0; s_key = -1; tick_cb(NULL);
}
static void return_keep_cb(lv_event_t *e){ (void)e; s_return_choice = 0; s_key = -1; tick_cb(NULL); }
static void back_cb(lv_event_t *e){ (void)e; screen_back(); }
static void wifi_cb(lv_event_t *e){ (void)e; wifi_open(); }
static void keys_cb(lv_event_t *e){ (void)e; setlist_open("Controls"); }
static void disc_cb(lv_event_t *e){ (void)e; if(ui_cast_return() < 0){ s_key = -1; tick_cb(NULL); } }
static void end_cb(lv_event_t *e){
    if(ui_cast_phase() != 2){ ui_cast_end_request(); return; }
    (void)e;
    int rc = ui_airplay_active() ? ui_airplay_return() : ui_cast_return();
    if(rc < 0) ui_cast_end_request();
}
static void row_cb(lv_event_t *e){
    uintptr_t u = (uintptr_t)lv_event_get_user_data(e);
    if(pk_tap((unsigned)(u >> 8), (int)(u & 0xff), lv_tick_get(), wifi_connectivity()) >= 0){ s_key = -1; tick_cb(NULL); }
}
int cast_ui_status(char *out, size_t cap){   /* decision 86: the receiver playing or buffering = "Beaming to <device>", paused = "Paused - <device>" */
    const char *d = ui_cast_device(); int p = ui_cast_paused();
    if(cap) out[0] = 0;
    if(!cap || ui_cast_phase() != 2 || !*d) return 0;
    snprintf(out, cap, "%s%s%s", tr(p ? "Paused" : "Beaming to"), p ? " - " : " ", d);
    return 1;
}
static void render(int ph){
    if(s_search){ lv_anim_delete(s_search, NULL); s_search = NULL; }
    lv_obj_clean(s_body);
    lv_obj_set_flex_align(s_body, P.st == PK_LIST && !ph ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    int error = ui_airplay_error();
    if(s_return_choice){
        lbl(tr("Play on Disc?"), 1, 0);
        lv_obj_t *note = lbl(tr(ui_airplay_return_single() ? "Only this track will play on Disc." : "Continue the original queue on Disc."), 0, 0);
        lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
        note = lbl(tr(error == APS_ERR_STOP ? "The TV may still be playing. Stop it first." : "You may hear the start before playback resumes."), 0, 1);
        lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
        btn(s_body, ui_airplay_return_single() ? "Play this track" : "Play on Disc", return_play_cb, 1, 190, 1);
        btn(s_body, ph ? "Keep beaming" : "Back", return_keep_cb, 0, 190, 1);
    } else if(!ph && !s_keys_ok){
        lbl(tr("Set volume keys to beam"), 1, 0);
        lv_obj_t *note = lbl(tr("In Settings > Controls, set press, double press and long press to Adjust Volume."), 0, 1);
        lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
        btn(s_body, "Controls", keys_cb, 1, 190, 1);
        btn(s_body, "Back", back_cb, 0, 190, 1);
    } else if(ph != 2 && !error && *ui_beam_error()){
        lbl(tr("Couldn't start beaming"), 1, 0);
        lv_obj_t *note = lbl(tr(ui_beam_error()), 0, 1);
        lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
        btn(s_body, "Try again", again_cb, 1, 190, !ph);
        btn(s_body, "Back", back_cb, 0, 190, 1);
    } else if(error){
        const char *title = error == APS_ERR_PIN ? "Wrong PIN" : error == APS_ERR_BUSY ? "Receiver is in use" :
            error == APS_ERR_TIMEOUT ? "Connection timed out" : error == APS_ERR_REMOVED ? "Pairing not accepted" :
            error == APS_ERR_VERIFY ? "Could not verify this receiver" : error == APS_ERR_FORMAT ? "This track can't be beamed" : error == APS_ERR_STORAGE ? "Pairing storage is unavailable" : error == APS_ERR_PASSWORD ? "Receiver needs a password" : error == APS_ERR_STOP ? "TV stop was not confirmed" : error == APS_ERR_DECODER ? "Couldn't prepare the audio decoder" : "Connection lost";
        char pairing[160];
        int pair_failed = error == APS_ERR_PIN || error == APS_ERR_REMOVED || error == APS_ERR_VERIFY || error == APS_ERR_CONNECT;
        if(pair_failed || error == APS_ERR_START){
            snprintf(pairing, sizeof pairing, error == APS_ERR_START ? "Couldn't start playback on %s" : "Pairing with %s failed",
                *ui_cast_device() ? ui_cast_device() : "receiver"); title = pairing;
        }
        lv_obj_t *heading = lbl(tr(title), 1, 0);
        lv_label_set_long_mode(heading, LV_LABEL_LONG_WRAP);
        int can_forget = ui_airplay_can_forget();
        const char *hint = error == APS_ERR_STOP ? "Music stays paused. The TV may still be playing." :
            error == APS_ERR_PASSWORD ? "Password-protected AirPlay 1 is unavailable." :
            error == APS_ERR_FORMAT ? "Choose an MP3, FLAC or PCM WAV track." :
            error == APS_ERR_PIN ? "The PIN was not accepted. Check the code on the TV and try again." :
            error == APS_ERR_VERIFY || error == APS_ERR_REMOVED ?
                (can_forget ? "Forget pairing, then pair with this receiver again." : "Receiver pairing was not accepted. Try again.") :
            error == APS_ERR_BUSY ? "Stop the other app on the receiver, then try again." :
            error == APS_ERR_STORAGE ? "Restart the Disc, then try again." :
            error == APS_ERR_DECODER ? "Choose another music track, then try again." : "Check the receiver and Wi-Fi, then try again.";
        lv_obj_t *note = lbl(tr(hint), 0, 1);
        lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
        btn(s_body, "Try again", again_cb, 1, 190, !ph);
        if(can_forget) btn(s_body, "Forget pairing", forget_cb, 0, 190, 1);
        if(ph){ lbl(tr("Beam is stopping. Wait, then try again."), 0, 1); btn(s_body, "Back", back_cb, 0, 190, 1); }
        if(ui_airplay_can_return()) btn(s_body, "Play on Disc", return_cb, 0, 190, 1);
    } else if(ph){   /* connecting (1) or beaming (2): the session, not the list */
        char n[200];
        if(ph == 2){ if(cast_ui_status(n, sizeof n)) lbl(n, 1, 0); }   /* the live session's own name and state: also right after a reopen or a reload */
        else{
            lbl(tr("Beaming to"), 1, 0);
            if(*ui_cast_device()){ snprintf(n, sizeof n, "%s...", ui_cast_device()); lbl(n, 1, 0); }
            lbl(tr("Music keeps playing on the Disc until it is ready."), 0, 1);
        }
        if(ph == 2 && ui_airplay_active()){
            int rate = ui_airplay_rate(); char format[80];
            if(rate > 0){ snprintf(format, sizeof format, tr("AirPlay - %s / %d bit"), rate == 48000 ? "48 kHz" : "44.1 kHz", ui_airplay_bits()); lbl(format, 0, 1); }
        }
        if(ph == 2 && ui_airplay_active()) btn(s_body, "Volume", volume_cb, 0, 190, 1);
        btn(s_body, ph == 1 ? "Cancel" : "Stop beaming", end_cb, ph == 2, 190, 1);
        if(ph == 2) btn(s_body, "Play on Disc", ui_airplay_active() ? return_cb : disc_cb, 0, 190, 1);
    } else if(P.st == PK_LIST){
        lv_obj_t *l = lv_obj_create(s_body); lv_obj_remove_style_all(l);
        lv_obj_set_size(l, 300, 250); lv_obj_set_style_pad_bottom(l, 44, 0); lv_obj_set_style_pad_row(l, 6, 0);
        lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN); lv_obj_set_flex_align(l, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scroll_dir(l, LV_DIR_VER); lv_obj_set_scrollbar_mode(l, LV_SCROLLBAR_MODE_OFF);
        for(int i = 0; i < (int)P.d.n; i++){
            lv_obj_t *r = lv_button_create(l); lv_obj_remove_style_all(r); lv_obj_set_size(r, 266, 40); lv_obj_set_style_radius(r, 12, 0);
            lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0); lv_obj_set_style_bg_color(r, TC(SURFACE), 0); lv_obj_set_style_bg_color(r, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
            ui_on(r, row_cb, LV_EVENT_CLICKED, (void *)(((uintptr_t)P.gen << 8) | (unsigned)i), "beam.row", UI_CORE);
            lv_obj_t *n = lv_label_create(r); lv_label_set_text(n, P.d.r[i].name); lv_label_set_long_mode(n, LV_LABEL_LONG_DOT); lv_obj_set_size(n, 154, 20);
            lv_obj_set_style_text_font(n, TF(UI_14), 0); lv_obj_set_style_text_color(n, TC(TEXT_PRIMARY), 0); lv_obj_align(n, LV_ALIGN_LEFT_MID, 14, 0);
            lv_obj_t *g = lv_label_create(r); lv_label_set_text(g, pk_tag(i)); lv_obj_align(g, LV_ALIGN_RIGHT_MID, -12, 0);
            lv_obj_set_style_text_font(g, TF(UI_12), 0); lv_obj_set_style_text_color(g, TC(TEXT_MUTED), 0);
        }
        btn(l, "Search again", again_cb, 0, 150, 1);
    } else if(P.st == PK_SEARCH){
        lv_obj_t *a = P.run ? (s_search = lv_spinner_create(s_body)) : lv_arc_create(s_body);
        lv_obj_set_size(a, 44, 44);
        if(P.run) lv_spinner_set_anim_params(a, 1000, 90);
        lv_obj_remove_style(a, NULL, LV_PART_KNOB); lv_arc_set_bg_angles(a, 0, 360); lv_arc_set_angles(a, 270, 360);
        lv_obj_set_style_arc_width(a, 4, 0); lv_obj_set_style_arc_width(a, 4, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(a, TC(CONTROL_TRACK), 0); lv_obj_set_style_arc_color(a, ui_current_accent(), LV_PART_INDICATOR);
        lbl(tr("Searching..."), 1, 0); lbl(tr("Looking for speakers and TVs on your Wi-Fi"), 0, 1);
        btn(s_body, "Search again", again_cb, 0, 150, 0);
    } else {
        int w = P.st == PK_NOWIFI;
        if(w){ lv_obj_t *i = lbl(LV_SYMBOL_WIFI, 1, 0); lv_obj_set_style_text_color(i, TC(STATUS_WARNING), 0); }
        lbl(tr(w ? "Connect to Wi-Fi to beam" : P.st == PK_NONE ? "No devices found" : "Search failed"), 1, 0);
        lbl(tr(w ? "Speakers and TVs are found over your home Wi-Fi." : P.st == PK_NONE ? "Use the same home Wi-Fi. Guest networks may block discovery." : "Couldn't look for devices. Try again."), 0, 1);
        btn(s_body, w ? "Wi-Fi settings" : "Search again", w ? wifi_cb : again_cb, 1, 190, 1);
        if(w) btn(s_body, "Back", back_cb, 0, 190, 1);
    }
    kit_pass(s_root);
}
static void pin_done(uint32_t attempt, const char *pin){
    if(!ui_airplay_pin_wanted() || attempt != ui_airplay_attempt()) return;
    if(!pin || ui_airplay_pin_submit(attempt, pin) < 0) ui_cast_end_request();
}
static void tick_cb(lv_timer_t *t){
    (void)t;
    if(!ui_screen_backlight_on()){
        if(!s_keys_ok || *ui_beam_error() || ui_airplay_error()){
            if(s_search){ lv_anim_delete(s_search, NULL); s_search = NULL; }
            if(P.run || P.st != PK_IDLE) pk_stop();
            if(!s_dark && s_tm) lv_timer_set_period(s_tm, 1000);
            s_dark = 1; return;   /* keep guidance until an action, including across automatic sleep */
        }
        screen_back(); return;
    }
    if(s_dark){ s_dark = 0; if(s_tm) lv_timer_set_period(s_tm, 150); }
    int ph = ui_cast_phase();
    if(s_return_attempt != ui_airplay_attempt()) s_return_choice = 0;
    if(ui_airplay_pin_wanted() && !airplay_pin_active()) airplay_pin_open(ui_airplay_attempt(), ui_cast_device(), pin_done);
    if(!ui_airplay_pin_wanted() && airplay_pin_active()) airplay_pin_close();
    int st = pk_step(screen_current() == SCR_CASTPICK && s_keys_ok && !*ui_beam_error() && !ui_airplay_error(), ph, wifi_connectivity(), lv_tick_get());
    if(s_ph == 1 && ph == 2){ s_ph = ph; screen_back(); return; }   /* the receiver is playing: back to where the user came from */
    s_ph = ph;
    int key = st | ui_cast_paused() << 3 | ph << 4 | (int)(P.gen << 8) | (ui_airplay_error() << 24);
    static char last_error[192];
    if(strcmp(last_error, ui_beam_error())){ snprintf(last_error, sizeof last_error, "%s", ui_beam_error()); s_key = -1; }
    if(key != s_key){ s_key = key; render(ph); }
    if(s_search && (!P.run || st != PK_SEARCH || lv_tick_elaps(P.t) >= PK_MS)){
        lv_anim_delete(s_search, NULL); s_search = NULL;
    }
}
void cast_ui_enter(void){   /* every entry (the screen manager) */
    pk_stop(); s_return_choice = 0; s_key = -1; s_ph = ui_cast_phase(); s_keys_ok = ui_cast_keys_ok(); s_dark = 0;
    if(!s_tm) s_tm = lv_timer_create(tick_cb, 150, NULL);
    tick_cb(NULL);
}
void cast_ui_leave(void){   /* every exit: Back, Home, Quick Settings, screen off, theme reload (via cast_ui_stop) */
    if(s_search){ lv_anim_delete(s_search, NULL); s_search = NULL; }
    pk_stop();
    if(airplay_pin_active()){ airplay_pin_close(); ui_cast_end_request(); }
    if(s_tm){ lv_timer_delete(s_tm); s_tm = NULL; }
}
void cast_ui_stop(void){ cast_ui_leave(); }
void cast_ui_open(void){ screen_show(SCR_CASTPICK); }
void cast_ui_create(lv_obj_t *root){
    s_root = root;
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0); lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    ui_header(root, "Beam");
    s_body = lv_obj_create(root); lv_obj_remove_style_all(s_body);
    lv_obj_set_size(s_body, 300, 270); lv_obj_set_pos(s_body, 30, 70);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN); lv_obj_set_style_pad_row(s_body, 8, 0);
    lv_obj_set_scroll_dir(s_body, LV_DIR_VER); lv_obj_set_scrollbar_mode(s_body, LV_SCROLLBAR_MODE_OFF);
}
#ifdef DISKOS_HOST
void cast_ui_return_preview(int single){
    (void)single; screen_show(SCR_CASTPICK); s_return_choice = 1;
    s_return_attempt = ui_airplay_attempt(); render(ui_cast_phase());
}
void cast_ui_preview(int st, const char *name){   /* host render scenes only: a fixed state, no discovery */
    static const char *N[] = { "Bedroom TV", "Kitchen speaker", "Living Room Nest Audio Max", "Study speaker" };
    screen_show(SCR_CASTPICK); pk_stop(); P.st = st; P.gen++; s_keys_ok = 1; P.d.n = st == PK_LIST ? 4 : 0; ui_cast_set_device(name);
    for(size_t i = 0; i < P.d.n; i++) snprintf(P.d.r[i].name, sizeof P.d.r[i].name, "%s", N[i]);
    render(ui_cast_phase());
}
#endif
#endif
