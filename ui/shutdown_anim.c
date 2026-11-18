/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* shutdown_anim.c - the boot splash's closing act (PROTOTYPE; see SHUTDOWN_PROTO.md).
 *
 * The same disc as splash.c, run backwards and then out:
 *   1. CLOSE   the current screen is the hub; the disc returns from past the screen's edge (the opening, reversed) and
 *              the hub goes dark and warms in the theme's accent with the "diskOS" wordmark.
 *   2. SAVE    one light comes on and travels once round the grooves (two small spectra, white streak, lit rim). Its
 *              angle IS the save: it follows the real power.c phase (shutdown_anim_target), an accent line fills the
 *              rim behind it. If a phase ends early the light catches up quickly; if the save is slow it waits.
 *   3. LEAVE   (sync is done, the player is about to power off) the ring is closed, the light leaves, the hub cools from
 *              accent to dark, the wordmark fades, a last glint at the spindle, black.
 * The animation never starts, delays or ends a shutdown phase: it is only fed the phase. A failed shutdown reopens the
 * screen (shutdown_anim_cancel). A dark panel gets no animation and is never woken for one.
 *
 * Why a file of its own and not a splash.c extension: the boot splash is device-verified and stays byte-for-byte as it is.
 * The few primitives shared with it (shape/ring/arc, sector boxes, the disc geometry) are copied here; if the owner
 * approves the design they move into one shared header in a mechanical step.
 *
 * Cost (GPU-less renderer): the splash's plain circles, rings and arcs on one integer timeline, no images, transforms,
 * shadows or masks. While the light moves only the two spectrum boxes and the fresh piece of the accent line are
 * repainted; the hub glow repaints the hub; the whole screen is repainted only while the disc closes/reopens and
 * while the line and the rim fade. */
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "shutdown_anim.h"
#include <string.h>

LV_FONT_DECLARE(diskos_brand_disk_32)
LV_FONT_DECLARE(diskos_brand_os_32)

#define SD_CX      180
#define SD_R       178
#define SD_SLICE   5
#define SD_GAPS    7
#define SD_GLOW    4
#define SD_NGAP    ((SD_GAPS + 1) / 2)

/* the disc, in 1/1000 of its radius: the same geometry as splash.c */
#define K_HUB   460
#define K_B1    560
#define K_LIT0  640
#define K_DATA  985

/* timeline, ms */
#define T_CLOSE      540        /* the disc returns: 0..540 */
#define T_LIGHT_ON   500        /* the light's spread opens 500..800 */
#define T_LIGHT_OPEN 300
#define T_LEAVE      820        /* from the ring closing to black */
#define T_CANCEL     320        /* a failed shutdown: the disc opens again */
#define RING_START   285        /* the light starts and ends at the top, where the boot splash's light ended */

enum { M_NONE, M_RUN, M_LEAVE, M_CANCEL, M_DONE };

static struct {
    lv_obj_t *root, *face, *rim, *trail, *rim_hl, *lobe[2], *slice[2][SD_SLICE], *streak, *shade[2], *gap[2][SD_NGAP],
             *glow[SD_GLOW], *hub, *hub_edge, *word, *halo, *glint;
    lv_timer_t *tm;
    int mode, phase, lit, hole, p, th, spread, warm, oc, n_obj, trail_n;
    uint32_t t0, tl, tc, ph_ms, last_ms;
    int gopa[SD_GLOW];  /* the glow's last drawn opacities */
    int oc_c, warm_c, hd, hd_c;  /* the disc's openness and the glow when a cancel began */
} g;

/* 0..1024 fixed point */
static int clampk(int v){ return v < 0 ? 0 : v > 1024 ? 1024 : v; }
static int win_k(int64_t t, int a, int b){ return clampk((int)((t - a) * 1024 / (b - a))); }
static int ease_out(int k){ int r = 1024 - k; return 1024 - (int)((int64_t)r * r * r >> 20); }
static int ease_io(int k){
    if(k < 512) return (int)(4 * (int64_t)k * k * k >> 20);
    int r = 1024 - k; return 1024 - (int)(4 * (int64_t)r * r * r >> 20);
}
static int scl(int v, int k){ return (int)((int64_t)v * k >> 10); }
static lv_opa_t opa_k(int max, int k){ return (lv_opa_t)(max * clampk(k) >> 10); }
static int q8(int k){ return (clampk(k) + 64) & ~127; }   /* 8 fade steps: an opacity change repaints the whole ring, so fewer is cheaper */

static void sty(lv_obj_t *o, lv_style_prop_t p, int32_t v, lv_style_selector_t sel){
    lv_style_value_t cur;
    if(lv_obj_get_local_style_prop(o, p, &cur, sel) == LV_STYLE_RES_FOUND && cur.num == v) return;
    lv_style_value_t nv = { .num = v };
    lv_obj_set_local_style_prop(o, p, nv, sel);
}
static void show(lv_obj_t *o, int on){
    if(on == !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
    if(on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *shape(lv_obj_t *parent){
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    kit_keep(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    g.n_obj++;
    return o;
}
static lv_obj_t *ring(lv_obj_t *parent, lv_color_t c, lv_opa_t opa){
    lv_obj_t *o = shape(parent);
    lv_obj_set_style_border_color(o, c, 0);
    lv_obj_set_style_border_opa(o, opa, 0);
    return o;
}
static lv_obj_t *arc(lv_obj_t *parent, lv_color_t c, lv_opa_t opa){
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_remove_style_all(a);
    kit_keep(a);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_arc_color(a, c, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, opa, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_arc_set_bg_angles(a, 0, 1);
    g.n_obj++;
    return a;
}
static void place_band(lv_obj_t *o, int r0, int r1, int is_arc){
    if(r1 < 1) r1 = 1;
    if(r0 > r1 - 1) r0 = r1 - 1;
    lv_obj_set_pos(o, SD_CX - r1, SD_CX - r1);
    lv_obj_set_size(o, 2 * r1, 2 * r1);
    sty(o, is_arc ? LV_STYLE_ARC_WIDTH : LV_STYLE_BORDER_WIDTH, r1 - r0, 0);
}
static void place_disc(lv_obj_t *o, int r){
    if(r < 1) r = 1;
    lv_obj_set_pos(o, SD_CX - r, SD_CX - r);
    lv_obj_set_size(o, 2 * r, 2 * r);
}
static int ang(int a){ a %= 360; return a < 0 ? a + 360 : a; }
static void span(lv_obj_t *a, int mid, int half){ lv_arc_set_bg_angles(a, ang(mid - half), ang(mid + half)); }
static int gap_r(int i){ return SD_R * (K_B1 + (K_DATA - K_B1) * (i + 1) / 8) / 1000; }

static void sector_box(int r0, int r1, int a0, int a1, lv_area_t *box){
    int x1 = 9999, y1 = 9999, x2 = -9999, y2 = -9999;
    for(int a = a0;; a += 5){
        if(a > a1) a = a1;
        int s = lv_trigo_sin((int16_t)ang(a)), c = lv_trigo_cos((int16_t)ang(a));
        for(int k = 0; k < 2; k++){
            int r = k ? r1 : r0, x = SD_CX + (r * c >> 15), y = SD_CX + (r * s >> 15);
            if(x < x1) x1 = x;
            if(x > x2) x2 = x;
            if(y < y1) y1 = y;
            if(y > y2) y2 = y;
        }
        if(a == a1) break;
    }
    box->x1 = x1 - 4; box->y1 = y1 - 4; box->x2 = x2 + 4; box->y2 = y2 + 4;
}
static void inv_sector(int r0, int r1, int a0, int a1){
    lv_area_t box;
    sector_box(r0, r1, a0, a1, &box);
    lv_obj_invalidate_area(g.root, &box);
}

/* ---- the one place where power.c phases become the light's angle ------------------------------------------------- */
/* The light travels once round (progress 0..1000). The bands are what each phase has actually achieved:
 *   PW_DRAIN   100 (the request was accepted and the SD admission is closed) + a slow creep to 220 while writers finish
 *   PW_SYNC    400 (writers drained, sync running)                           + a slow creep to 820 while sync runs
 *   PW_HANDOFF 1000 (sync exited 0, the player has been told to power off)
 * The creep only shows the wait is alive; it is bounded inside the band, so the light can never claim a phase that has not
 * finished (sync can take up to POWER_SYNC_MS = 15 s). */
static int creep(uint32_t ms, int span_){ return (int)((int64_t)span_ * ms / (ms + 2500u)); }
int shutdown_anim_target(int phase, uint32_t phase_ms){
    switch(phase){
    case PW_DRAIN:   return 100 + creep(phase_ms, 120);
    case PW_SYNC:    return 400 + creep(phase_ms, 420);
    case PW_HANDOFF: return 1000;
    default:         return -1;
    }
}

/* ---- one frame ---------------------------------------------------------------------------------------------------- */
static void set_hole(int hole){
    /* hole: the hub is a window onto the current screen (root paints nothing, the disc face is a ring around it).
     * closed: the whole screen is the disc's face, in the dark. Nothing on screen changes in the frame it flips. */
    if(hole == g.hole) return;
    g.hole = hole;
    lv_obj_set_style_bg_opa(g.root, hole ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    show(g.face, hole);
}

static void dispose(void){
    lv_timer_t *tm = g.tm;
    lv_obj_t *root = g.root;
    memset(&g, 0, sizeof g);
    if(tm) lv_timer_delete(tm);
    if(root) lv_obj_delete(root);
}

static void frame(uint32_t now){
    int t = (int)(now - g.t0);
    int dt = (int)(now - g.last_ms);
    if(dt > 100) dt = 100;
    g.last_ms = now;

    /* -- the disc: oc = 0 open past the edge, 1024 closed -- */
    int oc;
    if(g.mode == M_CANCEL) oc = scl(g.oc_c, 1024 - ease_io(win_k((int)(now - g.tc), 0, T_CANCEL)));
    else oc = ease_out(win_k(t, 0, T_CLOSE));       /* the disc rushes in from the edge and settles */
    int ok = 1024 - oc;
    int S = 1024 + scl((SD_CX + 10) * 1024 / (SD_R * K_HUB / 1000) - 1024, ok);
    int R = scl(SD_R, S);
#define RK(k) (R * (k) / 1000)
    int closed = oc >= 1024 && g.mode != M_CANCEL;
    set_hole(!closed);
    if(g.hole) place_band(g.face, RK(K_HUB), R + 2, 0);
    place_band(g.rim, RK(K_DATA) + 1, R, 1);

    /* -- progress: the light follows the real phase, never leads it -- */
    int rl = g.mode == M_LEAVE ? (int)(now - g.tl) : -1;
    if(g.mode == M_RUN && closed){
        int tgt = shutdown_anim_target(g.phase, now - g.ph_ms);
        if(tgt > 1000) tgt = 1000;
        if(t >= T_LIGHT_ON && tgt > g.p){
            int gap = tgt - g.p;
            int v = gap * dt / 140;                        /* eases in to the target */
            if(v > 5 * dt / 4) v = 5 * dt / 4;             /* catching up: the whole ring in 0.8 s at most */
            if(v < dt * 6 / 10) v = dt * 6 / 10;           /* never stalls, so it always arrives */
            if(v < 1) v = 1;
            g.p += v < gap ? v : gap;
        }
        if(g.p >= 1000){ g.mode = M_LEAVE; g.tl = now; rl = 0; }
    }

    /* -- the light: visible once the disc is closed; its spread opens after the disc has settled, closes in LEAVE -- */
    int sp = 0;
    if(g.mode == M_RUN || g.mode == M_LEAVE){
        sp = ease_out(win_k(t, T_LIGHT_ON, T_LIGHT_ON + T_LIGHT_OPEN));
        if(rl >= 0) sp = scl(sp, 1024 - ease_io(win_k(rl, 100, 400)));
    }
    int th = RING_START + 360 * g.p / 1000;
    int lights = sp > 0 && closed;
    show(g.lobe[0], lights); show(g.lobe[1], lights); show(g.rim_hl, lights);
    int trail_n = g.p >= 4 && closed && g.mode != M_CANCEL;
    show(g.trail, trail_n);
    if(lights && (th != g.th || sp != g.spread)){
        int lo = LV_MIN(th, g.th) - 24, hi = LV_MAX(th, g.th) + 24;
        if(g.th < 0) lo = th - 24, hi = th + 24;
        int r0 = RK(K_LIT0) - 8;
        inv_sector(r0, R, lo, hi);
        inv_sector(r0, R, lo + 180, hi + 180);
        g.th = th; g.spread = sp;
        place_band(g.rim_hl, RK(K_DATA) + 1, R, 1);
        span(g.rim_hl, th, scl(22, sp));
        int half = sp > 0 ? LV_MAX(1, scl(7, sp)) : 0;
        for(int s = 0; s < 2; s++){
            int axis = th + 180 * s;
            lv_area_t box;
            sector_box(r0, R, axis - 24, axis + 24, &box);
            lv_obj_set_pos(g.lobe[s], box.x1, box.y1);
            lv_obj_set_size(g.lobe[s], lv_area_get_width(&box), lv_area_get_height(&box));
            int ox = box.x1, oy = box.y1;
            for(int i = 0; i < SD_SLICE; i++){
                lv_obj_t *a = g.slice[s][i];
                place_band(a, RK(K_LIT0), RK(K_DATA), 1);
                lv_obj_set_pos(a, SD_CX - RK(K_DATA) - ox, SD_CX - RK(K_DATA) - oy);
                span(a, axis + scl((2 * i - (SD_SLICE - 1)) * 7 / 2 * (s ? -1 : 1), sp), half);
            }
            int rs = RK(K_LIT0 + (K_DATA - K_LIT0) * 45 / 100);
            place_band(g.shade[s], RK(K_LIT0), rs, 1);
            lv_obj_set_pos(g.shade[s], SD_CX - rs - ox, SD_CX - rs - oy);
            span(g.shade[s], axis, half > 0 ? 24 : 0);
            for(int k = 0; k < SD_NGAP; k++){
                int gr = gap_r(2 * k);
                lv_obj_set_pos(g.gap[s][k], SD_CX - gr - 1 - ox, SD_CX - gr - 1 - oy);
            }
            if(s == 0){
                place_band(g.streak, RK(K_LIT0), RK(K_DATA), 1);
                lv_obj_set_pos(g.streak, SD_CX - RK(K_DATA) - ox, SD_CX - RK(K_DATA) - oy);
                span(g.streak, th, sp > 0 ? 1 : 0);
            }
        }
    }
    /* the accent line on the rim: filled behind the light, closed when the save is done. Its angles invalidate only the
     * piece that grew; its fade (LEAVE) is the one whole-ring repaint. */
    if(trail_n){
        place_band(g.trail, RK(K_DATA) - 3, R, 1);
        if(g.p >= 1000) lv_arc_set_bg_angles(g.trail, 0, 360);
        else lv_arc_set_bg_angles(g.trail, RING_START, ang(th));
        int fade = rl >= 0 ? 1024 - ease_io(win_k(rl, 160, 460)) : 1024;
        sty(g.trail, LV_STYLE_ARC_OPA, opa_k(235, q8(fade)), LV_PART_MAIN);
    }
    /* the disc's outline goes with the light at the very end */
    sty(g.rim, LV_STYLE_ARC_OPA, opa_k(255, q8(rl >= 0 ? 1024 - ease_io(win_k(rl, 420, 720)) : 1024)), LV_PART_MAIN);

    /* -- the hub: Home goes dark inside it, then it warms in the accent; the glow also follows the save and breathes
     *    while the light waits -- */
    int hr = RK(K_HUB);
    static const int GW[SD_GLOW] = { 4, 10, 18, 28 };
    static const int GO[SD_GLOW] = { 54, 30, 15, 6 };
    int warm = 0;
    if(g.mode == M_CANCEL){
        warm = scl(g.warm_c, 1024 - win_k((int)(now - g.tc), 0, 160));
    } else {
        int base = ease_out(win_k(t, 250, 800));
        int level = 700 + 324 * g.p / 1000;                                   /* brighter as the save completes */
        warm = scl(base, level);
        if(g.mode == M_RUN && closed){
            int a = (int)((now - g.t0) * 360 / 1800 % 360);
            int br = (32768 - lv_trigo_cos((int16_t)a)) >> 6;                 /* 0..1024, a slow breath */
            warm = scl(warm, 1024 - scl(110, br));
        }
        if(rl >= 0) warm = scl(warm, 1024 - ease_io(win_k(rl, 200, 560)));
        if(rl >= 0 && rl < 200) warm = LV_MIN(1024, warm + scl(90, ease_out(win_k(rl, 0, 100)) - ease_io(win_k(rl, 100, 200))));   /* the ring closing: a small swell */
    }
    int gchg = 0;
    for(int i = 0; i < SD_GLOW; i++){ int o = opa_k(GO[i], warm); if(o != g.gopa[i]) gchg = 1; g.gopa[i] = o; }
    if(gchg && !g.hole){   /* the glow repaints its box only when an opacity step actually changed (the breath moves few steps) */
        lv_area_t gb = { SD_CX - hr - GW[SD_GLOW - 1] - 1, SD_CX - hr - GW[SD_GLOW - 1] - 1,
                         SD_CX + hr + GW[SD_GLOW - 1] + 1, SD_CX + hr + GW[SD_GLOW - 1] + 1 };
        lv_obj_invalidate_area(g.root, &gb);
    }
    g.warm = warm;
    for(int i = 0; i < SD_GLOW; i++){
        place_band(g.glow[i], hr, hr + scl(GW[i], S), 0);
        sty(g.glow[i], LV_STYLE_BORDER_OPA, g.gopa[i], 0);
    }
    place_disc(g.hub, hr);
    int hd = g.mode == M_CANCEL ? scl(g.hd_c, 1024 - ease_io(win_k((int)(now - g.tc), 0, T_CANCEL))) : ease_io(win_k(t, 160, 460));
    g.hd = hd;
    sty(g.hub, LV_STYLE_BG_OPA, opa_k(255, hd), 0);   /* the screen inside the hub goes dark while the disc closes */
    place_band(g.hub_edge, hr - 1, hr, 0);
    sty(g.hub_edge, LV_STYLE_BORDER_OPA, opa_k(36, warm), 0);

    /* -- the name: arrives as the hub closes, fades when the save is done -- */
    int wm = ease_out(win_k(t, 440, 860));
    if(g.mode == M_CANCEL) wm = scl(wm, 1024 - win_k((int)(now - g.tc), 0, 120));
    if(rl >= 0) wm = scl(wm, 1024 - ease_io(win_k(rl, 150, 420)));
    sty(g.word, LV_STYLE_OPA, opa_k(255, wm), 0);
    lv_obj_set_y(g.word, SD_CX - lv_obj_get_height(g.word) / 2 + 2 + scl(6, 1024 - ease_out(win_k(t, 440, 860))));

    /* -- the last glint at the spindle -- */
    int gl = 0, ha = 0;
    if(rl >= 0){
        gl = scl(ease_out(win_k(rl, 430, 520)), 1024 - ease_io(win_k(rl, 620, T_LEAVE - 20)));
        ha = scl(ease_out(win_k(rl, 430, 560)), 1024 - ease_io(win_k(rl, 600, T_LEAVE - 20)));
    }
    show(g.glint, gl > 0); show(g.halo, ha > 0);
    if(gl > 0){
        place_disc(g.glint, 1 + scl(2, gl)); sty(g.glint, LV_STYLE_BG_OPA, opa_k(255, gl), 0);
        place_disc(g.halo, 4 + scl(8, ha)); sty(g.halo, LV_STYLE_BG_OPA, opa_k(60, ha), 0);
    }
    g.oc = oc;
#undef RK

    if(g.mode == M_LEAVE && rl >= T_LEAVE){
        /* black: everything has gone; the root (the splash's face colour, 07080A) stays, swallowing taps, until the player powers off */
        g.mode = M_DONE;
        lv_obj_set_style_bg_opa(g.root, LV_OPA_COVER, 0);
        for(uint32_t i = 0; i < lv_obj_get_child_count(g.root); i++) lv_obj_add_flag(lv_obj_get_child(g.root, i), LV_OBJ_FLAG_HIDDEN);
        g.hole = 0;
        lv_obj_invalidate(g.root);
        if(g.tm) lv_timer_pause(g.tm);
    }
    if(g.mode == M_CANCEL && now - g.tc >= T_CANCEL){
        dispose();
    }
}

static void tm_cb(lv_timer_t *t){ (void)t; if(g.root) frame(lv_tick_get()); }

/* ---- API ---------------------------------------------------------------------------------------------------------- */
int shutdown_anim_start(int screen_lit){
    if(g.root) dispose();   /* every accepted request owns a fresh visual lifetime */
    if(!screen_lit) return 0;                           /* a dark panel is never lit for an animation */
    static const theme_color_role_t BAND[SD_SLICE] = { THEME_CLR_FIXED_SPLASH_BAND_5, THEME_CLR_FIXED_SPLASH_BAND_4,
        THEME_CLR_FIXED_SPLASH_BAND_3, THEME_CLR_FIXED_SPLASH_BAND_2, THEME_CLR_FIXED_SPLASH_BAND_1 };
    static const lv_opa_t BAND_OPA[SD_SLICE] = { 76, 104, 96, 104, 84 };
    lv_color_t accent = TC(ACCENT_PRIMARY);
    memset(&g, 0, sizeof g);
    g.th = -1; g.hole = -1; g.lit = 1;

    g.root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g.root);
    kit_keep(g.root);
    lv_obj_remove_flag(g.root, LV_OBJ_FLAG_SCROLLABLE);
    g.n_obj++;
    lv_obj_set_style_radius(g.root, 0, 0);
    lv_obj_set_size(g.root, 360, 360);
    lv_obj_set_style_bg_color(g.root, TC(FIXED_SPLASH_BG), 0);
    lv_obj_add_flag(g.root, LV_OBJ_FLAG_CLICKABLE);      /* swallow taps for the rest of this power cycle */

    g.face = ring(g.root, TC(FIXED_SPLASH_BG), LV_OPA_COVER);
    for(int s = 0; s < 2; s++){
        g.lobe[s] = shape(g.root);
        lv_obj_set_style_radius(g.lobe[s], 0, 0);
        lv_obj_add_flag(g.lobe[s], LV_OBJ_FLAG_HIDDEN);
        for(int i = 0; i < SD_SLICE; i++)
            g.slice[s][i] = arc(g.lobe[s], theme_color(BAND[i]), (lv_opa_t)(BAND_OPA[i] * (s ? 2 : 3) / 3));
        if(s == 0) g.streak = arc(g.lobe[s], TC(FIXED_SPLASH_3), 70);
        g.shade[s] = arc(g.lobe[s], TC(FIXED_SPLASH_BG), 130);
        for(int k = 0; k < SD_NGAP; k++){
            int gr = gap_r(2 * k);
            lv_obj_t *o = ring(g.lobe[s], TC(FIXED_SPLASH_HOLE), 130);
            lv_obj_set_size(o, 2 * (gr + 1), 2 * (gr + 1));
            lv_obj_set_style_border_width(o, 2, 0);
            if(2 * k + 1 < SD_GAPS){
                lv_obj_set_style_outline_color(o, TC(FIXED_SPLASH_HOLE), 0);
                lv_obj_set_style_outline_opa(o, 130, 0);
                lv_obj_set_style_outline_width(o, 2, 0);
                lv_obj_set_style_outline_pad(o, gap_r(2 * k + 1) - 1 - (gr + 1), 0);
            }
            g.gap[s][k] = o;
        }
    }
    g.rim = arc(g.root, TC(FIXED_SPLASH_SPINDLE), LV_OPA_COVER);
    lv_arc_set_bg_angles(g.rim, 0, 360);
    g.trail = arc(g.root, accent, 235);
    lv_obj_add_flag(g.trail, LV_OBJ_FLAG_HIDDEN);
    g.rim_hl = arc(g.root, TC(FIXED_SPLASH_HUB_LINE), 150);
    lv_obj_add_flag(g.rim_hl, LV_OBJ_FLAG_HIDDEN);
    for(int i = SD_GLOW - 1; i >= 0; i--) g.glow[i] = ring(g.root, accent, 0);
    g.hub = shape(g.root);
    lv_obj_set_style_bg_color(g.hub, TC(FIXED_SPLASH_BG), 0);
    lv_obj_set_style_bg_opa(g.hub, LV_OPA_TRANSP, 0);
    g.hub_edge = ring(g.root, TC(FIXED_SPLASH_3), 0);

    g.word = shape(g.root);
    lv_obj_set_style_radius(g.word, 0, 0);
    lv_obj_set_size(g.word, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g.word, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g.word, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    const lv_font_t *F[2] = { &diskos_brand_disk_32, &diskos_brand_os_32 };
    const char *T[2] = { "disk", "OS" };
    for(int i = 0; i < 2; i++){
        lv_obj_t *l = lv_label_create(g.word);
        kit_keep(l);
        lv_label_set_text(l, T[i]);
        lv_obj_set_style_text_font(l, F[i], 0);
        lv_obj_set_style_text_letter_space(l, -1, 0);
        lv_obj_set_style_text_color(l, TC(FIXED_SPLASH_3), 0);
    }
    lv_obj_update_layout(g.word);
    lv_obj_set_x(g.word, SD_CX - lv_obj_get_width(g.word) / 2);

    g.halo = shape(g.root);
    lv_obj_set_style_bg_color(g.halo, accent, 0);
    lv_obj_set_style_bg_opa(g.halo, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(g.halo, LV_OBJ_FLAG_HIDDEN);
    g.glint = shape(g.root);
    lv_obj_set_style_bg_color(g.glint, TC(FIXED_SPLASH_3), 0);
    lv_obj_set_style_bg_opa(g.glint, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(g.glint, LV_OBJ_FLAG_HIDDEN);

    g.mode = M_RUN; g.phase = PW_DRAIN;
    g.t0 = g.ph_ms = g.last_ms = lv_tick_get();
    frame(g.t0);
    g.tm = lv_timer_create(tm_cb, LV_DEF_REFR_PERIOD, NULL);
    ui_toast_dismiss();   /* the animation says it: "Shutting down..." would show through the opening hole */
    return 1;
}

void shutdown_anim_update(int phase, int screen_lit){
    if(!g.root) return;
    int was_lit = g.lit;
    g.lit = !!screen_lit;
    if(g.mode == M_CANCEL && !g.lit){ dispose(); return; }
    if(phase != g.phase){ g.phase = phase; g.ph_ms = lv_tick_get(); }
    if(g.mode == M_RUN || g.mode == M_LEAVE || g.mode == M_DONE){
        if(phase == PW_OFF || phase == PW_PROTECTED){ shutdown_anim_cancel(); return; }
    }
    if(g.tm && g.mode != M_DONE){                         /* a dark panel: no timer, no drawing, no invalidation */
        if(g.lit && !was_lit){ g.last_ms = lv_tick_get(); lv_timer_resume(g.tm); }
        else if(!g.lit && was_lit) lv_timer_pause(g.tm);
    }
}

void shutdown_anim_cancel(void){
    if(!g.root || g.mode == M_CANCEL) return;
    if(!g.lit){ dispose(); return; }
    g.oc_c = g.oc; g.warm_c = g.warm; g.hd_c = g.hd; g.tc = lv_tick_get(); g.mode = M_CANCEL;
    for(uint32_t i = 0; i < lv_obj_get_child_count(g.root); i++) lv_obj_remove_flag(lv_obj_get_child(g.root, i), LV_OBJ_FLAG_HIDDEN);
    g.hole = -1;                                                 /* frame() re-derives hole/closed and hides what is not shown */
    g.lit = 1;
    if(g.tm){ lv_timer_resume(g.tm); lv_timer_reset(g.tm); }
    frame(g.tc);
}

int shutdown_anim_active(void){ return g.root != NULL; }
int shutdown_anim_host_progress(void){ return g.p; }
int shutdown_anim_host_state(void){ return g.mode; }
int shutdown_anim_host_objects(void){ return g.n_obj; }
