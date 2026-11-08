/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "config.h"
#include "musicdb.h"
#include "fwcaps.h"
#include "ipc.h"
#include "i18n.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Ten-band graphic EQ. Older players use the existing cfg/write-and-select path.
 * V2.57 uses stock single-control commands and treats song.db as authoritative. */

int ui_apply_eq(int preset);             /* main.c: 0689 select; <0 = send failed */
#define NBAND 10
#define SLOT_MIN 11                      /* USER1  = STYLE_PRESET 11 */
#define SLOT_MAX 20                      /* USER10 = STYLE_PRESET 20 */

static const int         FREQHZ[NBAND] = { 32, 64, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };
static const char *const FREQLBL[NBAND]= { "32","64","125","250","500","1k","2k","4k","8k","16k" };

static int       g_slot = SLOT_MIN;      /* currently-edited USER slot (11..20) */
static lv_obj_t *g_band[NBAND];          /* band sliders */
static lv_obj_t *g_bval[NBAND];          /* band value labels */
static lv_obj_t *g_master;               /* master gain slider */
static lv_obj_t *g_mval;                 /* master value label */
static lv_obj_t *g_slotlbl;              /* "USER1" selector label */
static int       g_curve_known;          /* editing requires a successful slot read; a failed refresh keeps
                                          * the previously displayed curve and its slot identity */
static int       g_slot_parametric = 0;  /* 1 when the loaded stock slot uses parametric params (filterType/Q/
                                          * fractional gain) the graphic editor can't represent: block edits so
                                          * an accidental drag doesn't FLATTEN a real stock PEQ preset */
static int       g_slot_foreign = 0;     /* 1 when a slot we own in cfg has DIVERGED from the live DB curve
                                          * (the stock player changed it under us): select it, never overwrite -
                                          * a plain re-select must not clobber the external change */

/* The existing generation-checked sender refuses edits across player reattach. */
static mdb_peq_snapshot g_peq, g_expected;
static lv_timer_t *g_peq_timer;
static int g_peq_pending, g_peq_slot, g_drag_slot = -1;
static unsigned g_peq_gen;
static uint32_t g_peq_started;
static void peq_poll(lv_timer_t *t);
static int g_peq_flat;   /* the pending edit is Flat: 0675 zeroes gains and master; the Disc keeps each band's Q */

/* U257@4717bc..4717d4 -> 48b680; 41a4e8 serializes a one-element array,
 * 48b700..48b724 formats tag + length + count(1) + JSON. P257@4ef774 ->
 * 45e9e4 consumes it by position and persists at 45ec40..45ec4c. */
static int peq_send_band(int band, int gain){
    char json[180], frame[200];
    int n = snprintf(json, sizeof json,
        "[{\"filterType\":0,\"frequency\":%d,\"position\":%d,\"gain\":\"%d.0\",\"qValue\":\"%s\"}]",
        FREQHZ[band], band, gain, g_peq.q[band]);
    snprintf(frame, sizeof frame, "0678%04X0001%s", n + 12, json);
    return ipc_send_automatic(frame, g_peq_gen);
}
/* U257@471844..47188c passes signed slider tenths to 48b28c, masked to
 * 16 bits at 48b3c0. P257@4f025c..4f0270 loads signed16 and divides by 10. */
static int peq_send_master(int gain){
    char frame[16]; snprintf(frame, sizeof frame, "0630000C%04X", (unsigned)(gain * 10) & 0xffff);
    return ipc_send_automatic(frame, g_peq_gen);
}
/* U257@471500..471510: 0675, value 0, no string -> 48b3bc..48b3e0.
 * P257@4ef4a8 -> 45ee30..45effc resets and persists the current USER slot. */
static int peq_send_flat(void){ return ipc_send_automatic("0675000C0000", g_peq_gen); }

/* per-slot cfg keys: slot 11 (USER1) -> "eq1_b0".."eq1_b9" + "eq1_master", etc. So every
 * user slot keeps its own 10-band curve + master preamp independently. */
static void band_key(char *buf, int n, int slot, int band){ snprintf(buf, n, "eq%d_b%d", slot-10, band); }
static void master_key(char *buf, int n, int slot){ snprintf(buf, n, "eq%d_master", slot-10); }

/* One-time upgrade: the old single-slot editor stored USER1's curve in the flat keys
 * eq_b0..eq_b9 + eq_master. Seed the new per-slot USER1 keys (eq1_*) from them so an
 * upgraded install keeps its Custom EQ curve. Never overwrites a value the user has already
 * saved under a new key (a new key that is present, even 0, is left alone). A sentinel well
 * outside the -12..12 range distinguishes "absent" from a real 0. */
#define EQ_ABSENT (-1000)
static void migrate_legacy_user1(void){
    int moved = 0;
    for(int i=0;i<NBAND;i++){
        char nk[24]; band_key(nk, sizeof nk, SLOT_MIN, i);
        if(cfg_get_int(nk, EQ_ABSENT) != EQ_ABSENT) continue;   /* already has a new value */
        char ok[16]; snprintf(ok, sizeof ok, "eq_b%d", i);
        int lv = cfg_get_int(ok, EQ_ABSENT);
        if(lv != EQ_ABSENT){ cfg_set_int_deferred(nk, lv); moved = 1; }
    }
    char nmk[24]; master_key(nmk, sizeof nmk, SLOT_MIN);
    if(cfg_get_int(nmk, EQ_ABSENT) == EQ_ABSENT){
        int lm = cfg_get_int("eq_master", EQ_ABSENT);
        if(lm != EQ_ABSENT){ cfg_set_int_deferred(nmk, lm); moved = 1; }
    }
    if(moved) cfg_flush();   /* every migrated key in one rewrite */
}

/* build the stock PARAMS_JSON from the current band values + write+select the current PEQ
 * slot. Returns 1 if the curve actually reached the player (DB write + select), 0 otherwise. */
static int apply_now(void){
    if(!fw_custom_peq_writable()) return 0;   /* view-only firmware: never write or select a PEQ slot */
    char json[1100]; int n = 0;
    n += snprintf(json+n, sizeof json-n, "[");
    for(int i=0;i<NBAND;i++){
        if(n < 0 || n > (int)sizeof json - 90) return 0;   /* truncation guard (never hit at 10 bands) */
        int g = g_band[i] ? lv_slider_get_value(g_band[i]) : 0;
        n += snprintf(json+n, sizeof json-n,
            "%s{\"filterType\":0,\"frequency\":%d,\"position\":%d,\"gain\":\"%d.0\",\"qValue\":\"0.7\"}",
            i?",":"", FREQHZ[i], i, g);
    }
    if(n < 0 || n > (int)sizeof json - 2) return 0;
    n += snprintf(json+n, sizeof json-n, "]");
    int master = g_master ? lv_slider_get_value(g_master) : 0;
    if(mdb_set_peq(g_slot, (double)master, json)){
        if(ui_eq_select(g_slot) < 0) return 0;   /* select + persist eq_preset/eq_last; failed = curve never reached the player */
        return 1;
    }
    return 0;
}

/* push explicit band + master values into the sliders and value labels (display only). */
static void show_curve(const int *gains, int master){
    for(int i=0;i<NBAND;i++){
        int v = gains ? gains[i] : 0;
        if(v < -12) v = -12; else if(v > 12) v = 12;
        if(g_band[i]) lv_slider_set_value(g_band[i], v, LV_ANIM_OFF);
        if(g_bval[i]){ char b[8]; snprintf(b,sizeof b,"%+d", v); lv_label_set_text(g_bval[i], v?b:"0"); }
    }
    if(master < -12) master = -12; else if(master > 12) master = 12;
    if(g_master) lv_slider_set_value(g_master, master, LV_ANIM_OFF);
    if(g_mval){ char b[8]; snprintf(b,sizeof b,"%+d", master); lv_label_set_text(g_mval, master?b:"0"); }
}

/* does diskOS already hold a saved curve for this slot? (any band or master key present) */
static int slot_has_cfg(int slot){
    char k[24];
    for(int i=0;i<NBAND;i++){ band_key(k,sizeof k,slot,i); if(cfg_get_int(k,EQ_ABSENT)!=EQ_ABSENT) return 1; }
    master_key(k,sizeof k,slot); return cfg_get_int(k,EQ_ABSENT)!=EQ_ABSENT;
}

/* persist the WHOLE current curve to this slot's cfg keys (one flush). Persisting every band
 * (not just the touched one) captures a stock-derived slot in full the first time it is edited,
 * so its untouched bands keep their real values instead of snapping to 0 on the next select. */
static void persist_slot(int slot){
    char k[24];
    for(int i=0;i<NBAND;i++){ int v=g_band[i]?lv_slider_get_value(g_band[i]):0; band_key(k,sizeof k,slot,i); cfg_set_int_deferred(k,v); }
    int m=g_master?lv_slider_get_value(g_master):0; master_key(k,sizeof k,slot); cfg_set_int_deferred(k,m);
    cfg_flush();
}

/* pull a slot's curve into the sliders (no apply). A diskOS-owned slot comes from cfg; a slot we
 * have never edited shows the player's EXISTING (stock) PEQ curve, so selecting or first-editing
 * it never silently zeroes a stock USER preset. */
static int load_slot_read(int slot){
    if(fw_peq_band_edit()){
        mdb_peq_snapshot row;
        int r = mdb_get_peq_snapshot(slot, &row);
        if(r < 0) return 0;
        g_curve_known = r == 1;
        g_slot_parametric = r == 0;
        g_slot_foreign = 0;
        if(r == 1){ g_peq = row; show_curve(row.gains, row.master); }
        return 1;
    }
    int dg[NBAND]={0}, dm=0;
    int r=mdb_get_peq(slot,&dm,dg);
    if(r<0) return 0;
    int gr=r>0 ? mdb_peq_is_graphic(slot) : 1;
    if(gr<0) return 0;
    g_slot_parametric=(gr==0);
    g_slot_foreign=0;
    g_curve_known=(gr==1);
    if(slot_has_cfg(slot)){
        int cg[NBAND]; char k[24];
        for(int i=0;i<NBAND;i++){ band_key(k,sizeof k,slot,i); cg[i]=cfg_get_int(k,0); }
        master_key(k,sizeof k,slot);
        int cm=cfg_get_int(k,0);
        g_slot_foreign=gr==0 || dm!=cm || memcmp(dg,cg,sizeof dg)!=0;
    }
    show_curve(dg,dm);
    return 1;
}

static int load_slot(int slot){
    if(!mdb_ui_read_begin()){
        g_curve_known=0; g_slot_foreign=1;
        ui_toast("EQ read failed, try again"); return 0;
    }
    int ok=load_slot_read(slot);
    mdb_ui_read_end();
    if(!ok){
        g_curve_known=0; g_slot_foreign=1;
        ui_toast("EQ read failed, try again");
    }
    return ok;
}

static int peq_same_gains(const mdb_peq_snapshot *a, const mdb_peq_snapshot *b){
    return a->master == b->master && !memcmp(a->gains, b->gains, sizeof a->gains);
}
/* Stock persistence can spell 0.7 as 0.70; compare values, retain the fresh spelling. */
static int peq_same(const mdb_peq_snapshot *a, const mdb_peq_snapshot *b){
    if(!peq_same_gains(a, b)) return 0;
    for(int i=0;i<NBAND;i++){
        int aq = !strcmp(a->q[i], "0.71"), bq = !strcmp(b->q[i], "0.71");
        if(aq != bq) return 0;
    }
    return 1;
}
/* One outstanding operation. Each poll ends its transaction so delayed player writes
 * become visible. Never show the dragged value as a confirmed save. No cfg curve on V257. */
static void peq_wait(int selecting, int slot){
    g_curve_known = 0;
    g_peq_pending = selecting ? 1 : 2;
    g_peq_slot = slot; g_peq_started = lv_tick_get();
    if(!g_peq_timer) g_peq_timer = lv_timer_create(peq_poll, 100, NULL);
    if(!g_peq_timer){ g_peq_pending = 0; ui_toast(tr("Couldn't apply EQ")); }
}
static void peq_poll(lv_timer_t *t){
    mdb_peq_snapshot row;
    int r = -1, selected = mdb_peq_selected();
    if(g_peq_gen == ipc_generation() && selected == g_peq_slot && mdb_ui_read_begin()){
        r = mdb_get_peq_snapshot(g_peq_slot, &row);
        mdb_ui_read_end();
    }
    int done = r == 1 && (g_peq_flat ? peq_same_gains(&row, &g_expected) : peq_same(&row, &g_expected));
    if(r == 1 && (g_peq_pending == 1 || (done && !peq_same(&row, &g_peq)))){
        if(g_peq_pending == 1){
            g_slot = g_peq_slot; cfg_set_int("eq_slot", g_slot);
            if(g_slotlbl){ char b[16]; snprintf(b, sizeof b, "USER%d", g_slot-10); lv_label_set_text(g_slotlbl, b); }
        }
        g_peq = row; show_curve(row.gains, row.master); g_curve_known = 1; g_slot_parametric = 0;
    } else if(lv_tick_elaps(g_peq_started) < 2000 && g_peq_gen == ipc_generation()) return;
    else ui_toast(tr("Couldn't apply EQ"));
    g_peq_pending = 0; g_peq_timer = NULL; lv_timer_del(t);
}
static void peq_edit(int band, int flat){
    int value = flat ? 0 : lv_slider_get_value(band == NBAND ? g_master : g_band[band]);
    show_curve(g_peq.gains, g_peq.master);
    if(g_peq_pending) return;
    /* A drag started on an earlier slot must never be applied to this one. */
    if((g_drag_slot >= 0 && g_drag_slot != g_slot) || !g_curve_known ||
       g_peq_gen != ipc_generation() || cfg_get_int("eq_preset", g_slot) != g_slot || mdb_peq_selected() != g_slot){
        g_curve_known = 0; ui_toast(tr("Couldn't apply EQ")); return;
    }
    mdb_peq_snapshot row;
    int r = -1;
    if(mdb_ui_read_begin()){
        r = mdb_get_peq_snapshot(g_slot, &row); mdb_ui_read_end();
    }
    if(r != 1 || g_peq_gen != ipc_generation() ||
       cfg_get_int("eq_preset", g_slot) != g_slot || mdb_peq_selected() != g_slot){ g_curve_known = 0; ui_toast(tr("EQ read failed, try again")); return; }
    g_peq = row; show_curve(row.gains, row.master); g_expected = row;
    g_peq_flat = flat;
    if(flat){
        g_expected.master = 0;
        for(int i=0;i<NBAND;i++) g_expected.gains[i] = 0;
    } else if(band == NBAND) g_expected.master = value;
    else g_expected.gains[band] = value;
    /* No change needs no command or acknowledgement (also drops duplicate release events). */
    if(flat ? peq_same_gains(&row, &g_expected) : peq_same(&row, &g_expected)) return;
    int rc = flat ? peq_send_flat() : band == NBAND ? peq_send_master(value) : peq_send_band(band, value);
    g_curve_known = 0;
    if(rc != 0){ ui_toast(tr("Couldn't apply EQ")); return; }
    peq_wait(0, g_slot);
}

/* switch the edited slot: show its curve, then apply it live. A diskOS-owned slot is written
 * back + selected; a slot we have never edited is only SELECTED (0689) - never overwritten with
 * a flat curve, which would destroy a stock USER preset. */
static void set_slot(int slot){
    if(fw_peq_band_edit() && (g_peq_pending || g_drag_slot >= 0)) return;
    if(slot < SLOT_MIN) slot = SLOT_MIN;
    if(slot > SLOT_MAX) slot = SLOT_MAX;
    if(fw_peq_band_edit()){
        g_curve_known = 0; g_peq_gen = ipc_generation();
        if(ui_eq_select(slot) < 0){ ui_toast(tr("Couldn't apply EQ")); return; }
        peq_wait(1, slot); return;
    }
    if(!load_slot(slot)) return;
    g_slot = slot;
    cfg_set_int("eq_slot", g_slot);
    if(g_slotlbl){ char b[16]; snprintf(b,sizeof b,"USER%d", g_slot-10); lv_label_set_text(g_slotlbl, b); }
    if(!fw_custom_peq_writable()) return;               /* view-only: show the slot's curve, select nothing */
    if(slot_has_cfg(g_slot) && !g_slot_foreign){
        if(!apply_now()) ui_toast("Couldn't apply EQ");   /* our curve, DB agrees -> write back + select */
    } else if(ui_eq_select(g_slot) < 0){
        ui_toast("Couldn't apply EQ");                    /* stock OR diverged slot -> select only, never overwrite */
    }
}

/* Opening the already-active USER slot must not add another loader repair. */
static void peq_open(int slot){
    if(g_peq_pending || g_drag_slot >= 0) return;
    if(cfg_get_int("eq_preset", -1) != slot || mdb_peq_selected() != slot){ set_slot(slot); return; }
    if(!load_slot(slot)) return;
    if(!g_curve_known){ ui_toast(tr("Couldn't apply EQ")); return; }
    g_slot = slot; g_peq_gen = ipc_generation();
    cfg_set_int("eq_slot", slot);
    if(g_slotlbl){ char b[16]; snprintf(b, sizeof b, "USER%d", slot-10); lv_label_set_text(g_slotlbl, b); }
}

/* A press the input layer reset without a release must not block navigation forever. */
static void peq_cancel_drag(void){
    if(g_drag_slot < 0) return;
    g_drag_slot = -1;
    show_curve(g_peq.gains, g_peq.master);
}

static void slot_dir_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    if(fw_peq_band_edit()) peq_cancel_drag();
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    set_slot(g_slot + dir);              /* clamps at USER1 / USER10 (no wrap) */
}

/* live preview of the value label (no persist/apply on every drag tick) */
static void band_cb(lv_event_t *e){
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    int v = lv_slider_get_value(lv_event_get_target(e));
    lv_obj_t *lbl = (i==NBAND) ? g_mval : (i>=0 && i<NBAND ? g_bval[i] : NULL);
    if(lbl){ char b[8]; snprintf(b,sizeof b,"%+d", v); lv_label_set_text(lbl, v?b:"0"); }
}
/* persist this control (to the current slot) + apply the whole curve when the finger lifts */
static void band_release_cb(lv_event_t *e){
    if(fw_peq_band_edit()){
        if(g_drag_slot < 0) return;
        int band = (int)(intptr_t)lv_event_get_user_data(e);
        if(band >= 0 && band <= NBAND) peq_edit(band, 0);
        g_drag_slot = -1; return;
    }
    if(!fw_custom_peq_writable()){ ui_toast("Custom EQ: not on this firmware yet"); load_slot(g_slot); return; }
    /* Blocked edit: either a stock PARAMETRIC preset (which the 10-band graphic editor would flatten), or a
     * slot whose stock curve we FAILED to read (persisting the placeholder-flat would clobber it). Either
     * way, drop the edit and reload the real curve rather than destroy it. */
    if(!g_curve_known){
        ui_toast(g_slot_parametric ? "Advanced preset - edit it on the stock player" : "EQ read failed, try again");
        load_slot(g_slot); return;
    }
    /* apply first; persist the whole curve only once it actually reached the player, so a PEQ
     * write/select failure can't leave cfg/UI showing a band the audio engine never received.
     * Persisting every band (not just the touched one) captures a stock-derived slot in full on
     * its first edit, so the untouched bands keep their real values. */
    if(apply_now()) persist_slot(g_slot);
    else ui_toast("Couldn't apply EQ");
}

static void flat_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    if(fw_peq_band_edit()){ peq_edit(0, 1); return; }
    if(!fw_custom_peq_writable()){ ui_toast("Custom EQ: not on this firmware yet"); return; }
    /* LV_ANIM_OFF so the slider VALUES are actually 0 when apply_now() reads them below - with
     * LV_ANIM_ON the sliders are still mid-animation and apply_now() would send the old/intermediate
     * curve to the player while cfg + labels already said 0. */
    for(int i=0;i<NBAND;i++){
        lv_slider_set_value(g_band[i], 0, LV_ANIM_OFF);
        if(g_bval[i]) lv_label_set_text(g_bval[i], "0");
    }
    lv_slider_set_value(g_master, 0, LV_ANIM_OFF);
    if(g_mval) lv_label_set_text(g_mval, "0");
    /* apply FIRST; persist the flat curve (to the current slot) only if it reached the player.
     * Flat is an explicit user choice, so it is allowed even after a failed read - and it makes the
     * slot's curve known again (flat), so a following band edit persists normally. */
    if(apply_now()){ persist_slot(g_slot); g_curve_known = 1; }
    else ui_toast("Couldn't apply EQ");
}

static void band_press_cb(lv_event_t *e){ (void)e; g_drag_slot = g_slot; }
static void peq_delete_cb(lv_event_t *e){
    (void)e;
    if(g_peq_timer){ lv_timer_del(g_peq_timer); g_peq_timer = NULL; }
    g_peq_pending = 0; g_curve_known = 0; g_drag_slot = -1; g_slotlbl = NULL;
}

/* one EQ column: value label on top, vertical slider, freq label below. idx==NBAND = master.
 * Built inside the horizontal scroller. Slider starts at 0; load_slot() fills the real value. */
static int g_eq_drop;   /* px the header's filled title cap pushes everything down (0 for a plain title) */
static void make_col(lv_obj_t *parent, int idx, const char *flabel,
                     lv_obj_t **slot_slider, lv_obj_t **slot_val){
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 42, 206 - g_eq_drop);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *val = lv_label_create(col);
    lv_obj_set_pos(val, 0, 0); lv_obj_set_width(val, 42);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(val, TF(UI_14), 0);
    lv_obj_set_style_text_color(val, TC(ACCENT_PRIMARY), 0);
    lv_label_set_text(val, "0");

    lv_obj_t *sl = lv_slider_create(col);
    lv_obj_set_size(sl, 16, 150 - g_eq_drop);
    lv_obj_set_ext_click_area(sl, 8);
    lv_obj_set_pos(sl, 13, 22);
    lv_slider_set_mode(sl, LV_SLIDER_MODE_SYMMETRICAL);   /* fill from 0 dB centre */
    lv_slider_set_range(sl, -12, 12);
    lv_slider_set_value(sl, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, TC(CONTROL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, TC(ACCENT_PRIMARY), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, TC(CONTROL_KNOB), LV_PART_KNOB);
    ui_on(sl, band_cb, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)idx, "eqcustom.band.value", UI_CORE);
    lv_obj_add_event_cb(sl, band_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(sl, band_release_cb, LV_EVENT_RELEASED, (void*)(intptr_t)idx);
    lv_obj_add_event_cb(sl, band_release_cb, LV_EVENT_PRESS_LOST, (void*)(intptr_t)idx);

    lv_obj_t *fl = lv_label_create(col);
    lv_obj_set_pos(fl, 0, 182 - g_eq_drop); lv_obj_set_width(fl, 42);
    lv_label_set_text(fl, flabel);
    lv_obj_set_style_text_align(fl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(fl, TF(UI_14), 0);
    lv_obj_set_style_text_color(fl, (idx==NBAND ? TC(TEXT_SECONDARY) : TC(TEXT_MUTED)), 0);

    *slot_slider = sl;
    *slot_val = val;
}

/* View-only firmware: a slider looks unavailable, and a tap on its column says why. The slider stops taking the
 * touch, so the tap lands on the column; a drag still scrolls the row of bands (no tap is sent after a scroll). */
static void view_only_tap_cb(lv_event_t *e){
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) ui_toast("Not on this firmware");
}
static void view_only_slider(lv_obj_t *sl){
    if(!sl) return;
    lv_obj_add_state(sl, LV_STATE_DISABLED);
    lv_obj_set_style_opa(sl, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_remove_flag(sl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *col = lv_obj_get_parent(sl);
    lv_obj_add_flag(col, LV_OBJ_FLAG_CLICKABLE);
    ui_on(col, view_only_tap_cb, LV_EVENT_CLICKED, NULL, "eqcustom.view_only", UI_CORE);
}

/* small "< USERn >" slot selector; two 44px-touch arrows around a centred label. */
static void make_slot_selector(lv_obj_t *root){
    lv_obj_t *sel = lv_obj_create(root);
    lv_obj_remove_style_all(sel);
    lv_obj_set_pos(sel, 100, 48 + g_eq_drop); lv_obj_set_size(sel, 160, 32);   /* its arrows clear the back chevron's column */
    lv_obj_clear_flag(sel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lb = lv_button_create(sel); lv_obj_remove_style_all(lb);
    lv_obj_set_size(lb, 40, 30); lv_obj_align(lb, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_ext_click_area(lb, 8);
    lv_obj_t *li = lv_label_create(lb); lv_label_set_text(li, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(li, TC(TEXT_PRIMARY), 0); lv_obj_center(li);
    ui_on(lb, slot_dir_cb, LV_EVENT_CLICKED, (void*)(intptr_t)-1, "eqcustom.slot_dir", UI_CORE);

    g_slotlbl = lv_label_create(sel);
    lv_obj_set_width(g_slotlbl, 80); lv_obj_align(g_slotlbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_align(g_slotlbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_slotlbl, TF(UI_16), 0);
    lv_obj_set_style_text_color(g_slotlbl, TC(ACCENT_PRIMARY), 0);
    { char b[16]; snprintf(b,sizeof b,"USER%d", g_slot-10); lv_label_set_text(g_slotlbl, b); }

    lv_obj_t *rb = lv_button_create(sel); lv_obj_remove_style_all(rb);
    lv_obj_set_size(rb, 40, 30); lv_obj_align(rb, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_ext_click_area(rb, 8);
    lv_obj_t *ri = lv_label_create(rb); lv_label_set_text(ri, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(ri, TC(TEXT_PRIMARY), 0); lv_obj_center(ri);
    ui_on(rb, slot_dir_cb, LV_EVENT_CLICKED, (void*)(intptr_t)1, "eqcustom.slot_dir", UI_CORE);
}

/* Re-resolve the edited slot on every entry (display-only, no audio re-apply): if a USER slot is the
 * live preset, edit that one; else keep the last-edited slot. Without this the editor stays on the slot
 * it was BUILT with, so selecting a different USER slot via Tune/Settings would edit the wrong curve. */
void eqcustom_refresh(void){
    if(!g_slotlbl) return;   /* not built yet */
    int init = cfg_get_int("eq_slot", SLOT_MIN);
    int pre  = cfg_get_int("eq_preset", -1);
    if(pre >= SLOT_MIN && pre <= SLOT_MAX) init = pre;
    if(init < SLOT_MIN || init > SLOT_MAX) init = SLOT_MIN;
    if(fw_peq_band_edit()){ peq_cancel_drag(); peq_open(init); return; }
    if(!load_slot(init)) return;
    g_slot = init;
    cfg_set_int("eq_slot", g_slot);
    char b[16]; snprintf(b, sizeof b, "USER%d", g_slot-10); lv_label_set_text(g_slotlbl, b);
}

void eqcustom_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    if(!fw_peq_band_edit()) migrate_legacy_user1();   /* carry a pre-slots Custom EQ curve into USER1 (one-time, non-destructive) */

    /* Resolve which slot to edit: if a USER slot is the live preset, edit that one; else fall
     * back to the editor's last-edited slot; default USER1. */
    int init = cfg_get_int("eq_slot", SLOT_MIN);
    int pre  = cfg_get_int("eq_preset", -1);
    if(pre >= SLOT_MIN && pre <= SLOT_MAX) init = pre;
    if(init < SLOT_MIN || init > SLOT_MAX) init = SLOT_MIN;
    g_slot = init;
    cfg_set_int("eq_slot", g_slot);

    lv_obj_t *title = ui_header(root, "Custom EQ");   /* shared standard header */
    /* the slot selector sits under the title as it does under Default's (whose title ends at y 56): a taller title
     * face moves it down by the difference, a filled title cap puts it below the cap; the sliders give up the room */
    g_eq_drop = 0;
    kit_pass(root);
    lv_obj_update_layout(root);
    if(title){
        lv_area_t a; lv_obj_get_coords(title, &a);
        g_eq_drop = a.y2 + 8 - 48;
        if(g_eq_drop < 0) g_eq_drop = 0;
    }

    make_slot_selector(root);

    /* horizontally-scrollable row of EQ columns (master + 10 bands) */
    lv_obj_t *scr = lv_obj_create(root);
    lv_obj_remove_style_all(scr);
    /* exactly SIX columns wide (6 x 42 + 5 x 4 = 272, x 44..316): the next column is either fully in view or fully
     * hidden, never cut by the round glass - a full-width strip left the 7th column's label sliced by the rim ("1k"
     * read "1'"). x 44 still clears an edge back arrow; the label row (y ~262) is inside the circle across 44..316. */
    lv_obj_set_pos(scr, 44, 80 + g_eq_drop);
    lv_obj_set_size(scr, 272, 212 - g_eq_drop);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(scr, LV_DIR_HOR);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_column(scr, 4, 0);
    lv_obj_set_scroll_snap_x(scr, LV_SCROLL_SNAP_START);   /* scrolling stops on a whole column */

    make_col(scr, NBAND, "MSTR", &g_master, &g_mval);    /* master first */
    for(int i=0;i<NBAND;i++)
        make_col(scr, i, FREQLBL[i], &g_band[i], &g_bval[i]);

    load_slot(g_slot);   /* construction is display-only (screens are built at startup); refresh handles entry */
    lv_obj_add_event_cb(root, peq_delete_cb, LV_EVENT_DELETE, NULL);

    lv_obj_t *flat = lv_button_create(root);
    lv_obj_remove_style_all(flat);
    lv_obj_set_size(flat, 120, 34); lv_obj_align(flat, LV_ALIGN_TOP_MID, 0, 296);
    lv_obj_set_ext_click_area(flat, 6);   /* 34px pill -> ~46px touch target */
    lv_obj_set_style_radius(flat, 17, 0);
    lv_obj_set_style_bg_color(flat, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(flat, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(flat, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    ui_on(flat, flat_cb, LV_EVENT_CLICKED, NULL, "eqcustom.flat", UI_CORE);
    lv_obj_t *fll=lv_label_create(flat); lv_label_set_text(fll, "Flat");
    lv_obj_set_style_text_font(fll, TF(UI_14), 0);
    lv_obj_set_style_text_color(fll, TC(TEXT_PRIMARY), 0); lv_obj_center(fll);
    if(!fw_custom_peq_writable()){
        /* View-only firmware: sliders and Flat are disabled (the slot arrows still browse curves), and the
         * Flat pill says why instead of offering a change the player would reject or repair. */
        for(int i=0;i<=NBAND;i++) view_only_slider(i < NBAND ? g_band[i] : g_master);
        lv_obj_set_width(flat, 196);   /* inside the round glass at this height (y 296..330), square corners too */
        lv_label_set_text(fll, "Not on this firmware");
        lv_obj_set_style_text_color(fll, TC(TEXT_MUTED), 0);
    }
}
