/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "theme.h"
#include "sdio.h"
#include "musicdb.h"
#include "artcache.h"
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <pthread.h>

/* Album Wall: an Apple-style COVER FLOW that FLOWS. The centred album faces the viewer; the covers to
 * each side are angled (an affine squash, dimmed) with a faded reflection. A swipe slides the whole strip
 * one album over, with the two covers changing orientation cross-fading between a "facing" and a "turned"
 * pose. No GPU: each cover is decoded once and BAKED into two ready-to-blit sprites (a 148px facing sprite
 * and a 64px turned sprite, reflection included), kept in an LRU + prefetched in a window - so the flow
 * animation only translates + blends static bitmaps (no per-frame transforms) and stays smooth. Covers
 * come from the art cache (a track from the album must have been played); un-cached albums show a dark
 * placeholder. Tap the centre to play, long-press to open its tracks (dispatched from main.c). */

#define AW_SRC    148      /* cached cover native size (must match artcache's c.bmp) */
#define AW_REFL   32       /* reflection height */
#define AW_GAP    2        /* gap between a cover and its reflection */
#define AW_SPR_H  (AW_SRC + AW_GAP + AW_REFL)   /* 182: baked sprite height (cover + gap + reflection) */
#define AW_SIDE_W 64       /* turned (side) sprite width */
#define AW_DIM    174      /* side brightness: RGB * 174/256 ~= 0.68 */
#define AW_CARDS  9        /* rendered cards: bases -4..+4 (buffers for entering/leaving) */
#define AW_LRU    13       /* baked sprites resident (9 visible span + prefetch) */
#define AW_PREFETCH 5      /* decode this far each side while browsing */

/* slot centres at integer positions -2..+2; the strip interpolates between them */
static const float SLOT_X[5] = { 52, 90, 180, 270, 308 };

static lv_obj_t *g_root;
static lv_obj_t *g_front[AW_CARDS];   /* facing sprite widget per card */
static lv_obj_t *g_side[AW_CARDS];    /* turned sprite widget per card */
static lv_obj_t *g_initial;
static lv_obj_t *g_name, *g_artist, *g_counter;

static char  (*g_names)[MDB_STR];
static char  (*g_artists)[MDB_STR];
static int    *g_counts;
static int     g_names_cap = 0;   /* allocated entries in the three album buffers (grows with the library) */
static int     g_aw_inited = 0;   /* one-time blank-sprite + LRU init done */
static int     g_nalb;
static int     g_cur;
static float    g_off;                /* strip offset during a step animation (0 at rest) */
static int      g_animating;
static uint32_t g_tickctr;

/* a baked cover: facing sprite (front) + turned sprite (side), each cover + reflection */
typedef struct {
    int      idx;
    int      has_cover;
    int      asked;
    uint8_t *fbuf;                     /* AW_SRC   * AW_SPR_H * 4 ARGB */
    uint8_t *sbuf;                     /* AW_SIDE_W* AW_SPR_H * 4 ARGB */
    lv_image_dsc_t fdsc, sdsc;
    uint32_t tick;
} sprite_t;
static sprite_t g_lru[AW_LRU];
static sprite_t g_blank;               /* dark placeholder (no art) */

/* ---- decode + bake ------------------------------------------------------------------------------ */
static int aw_read_bmp(const char *path, uint8_t *buf){
    FILE *f = fopen(path, "rb"); if(!f) return -1;
    uint8_t hdr[54]; if(fread(hdr,1,54,f)!=54){ fclose(f); return -1; }
    uint32_t off = hdr[10]|(hdr[11]<<8)|(hdr[12]<<16)|((uint32_t)hdr[13]<<24);
    int w = hdr[18]|(hdr[19]<<8), h = hdr[22]|(hdr[23]<<8), bpp = hdr[28]|(hdr[29]<<8);
    uint32_t comp = hdr[30]|(hdr[31]<<8)|(hdr[32]<<16)|((uint32_t)hdr[33]<<24);
    if(w!=AW_SRC || h!=AW_SRC || bpp!=24 || comp!=0){ fclose(f); return -1; }
    uint8_t row[AW_SRC*3];
    if(fseek(f, off, SEEK_SET)!=0){ fclose(f); return -1; }
    for(int yy=0; yy<AW_SRC; yy++){
        if(fread(row,1,AW_SRC*3,f)!=(size_t)(AW_SRC*3)){ fclose(f); return -1; }
        uint8_t *d = buf + (AW_SRC-1-yy)*AW_SRC*4;
        for(int x=0;x<AW_SRC;x++){ d[x*4]=row[x*3]; d[x*4+1]=row[x*3+1]; d[x*4+2]=row[x*3+2]; d[x*4+3]=0xFF; }
    }
    fclose(f); return 0;
}
static void aw_set_dsc(lv_image_dsc_t *d, uint8_t *buf, int w, int h){
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_ARGB8888; d->header.w = w; d->header.h = h; d->header.stride = w*4;
    d->data = buf; d->data_size = (uint32_t)w*h*4;
}
static inline int refl_alpha(int y){ return 64 - (64*y)/(AW_REFL-1); }   /* 64 -> 0 */

/* facing sprite: cover rows, a transparent gap, then the mirrored+faded reflection */
static void aw_bake_front(const uint8_t *src, uint8_t *f){
    for(int y=0;y<AW_SRC;y++) memcpy(f + y*AW_SRC*4, src + y*AW_SRC*4, AW_SRC*4);   /* cover, opaque */
    memset(f + AW_SRC*AW_SRC*4, 0, AW_GAP*AW_SRC*4);                                /* gap */
    for(int y=0;y<AW_REFL;y++){
        const uint8_t *s = src + (AW_SRC-1-y)*AW_SRC*4; int a = refl_alpha(y);
        uint8_t *d = f + (AW_SRC+AW_GAP+y)*AW_SRC*4;
        for(int x=0;x<AW_SRC;x++){ d[x*4]=s[x*4]; d[x*4+1]=s[x*4+1]; d[x*4+2]=s[x*4+2]; d[x*4+3]=(uint8_t)a; }
    }
}
/* turned sprite: horizontally squashed 148->64 + dimmed, same cover/gap/reflection structure */
static void aw_bake_side(const uint8_t *src, uint8_t *s){
    for(int y=0;y<AW_SRC;y++){
        const uint8_t *sr = src + y*AW_SRC*4; uint8_t *d = s + y*AW_SIDE_W*4;
        for(int dx=0;dx<AW_SIDE_W;dx++){
            const uint8_t *p = sr + (dx*AW_SRC/AW_SIDE_W)*4;
            d[dx*4+0]=(uint8_t)(p[0]*AW_DIM>>8); d[dx*4+1]=(uint8_t)(p[1]*AW_DIM>>8);
            d[dx*4+2]=(uint8_t)(p[2]*AW_DIM>>8); d[dx*4+3]=0xFF;
        }
    }
    memset(s + AW_SRC*AW_SIDE_W*4, 0, AW_GAP*AW_SIDE_W*4);
    for(int y=0;y<AW_REFL;y++){
        const uint8_t *sr = src + (AW_SRC-1-y)*AW_SRC*4; int a = refl_alpha(y);
        uint8_t *d = s + (AW_SRC+AW_GAP+y)*AW_SIDE_W*4;
        for(int dx=0;dx<AW_SIDE_W;dx++){
            const uint8_t *p = sr + (dx*AW_SRC/AW_SIDE_W)*4;
            d[dx*4+0]=(uint8_t)(p[0]*AW_DIM>>8); d[dx*4+1]=(uint8_t)(p[1]*AW_DIM>>8);
            d[dx*4+2]=(uint8_t)(p[2]*AW_DIM>>8); d[dx*4+3]=(uint8_t)a;
        }
    }
}

/* Same handoff as the sharp-cover worker: private pixels, mutex, UI poll. */
static pthread_mutex_t g_wmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wcv = PTHREAD_COND_INITIALIZER;
static unsigned g_wgen, g_wac_gen;              /* g_wac_gen: artcache_gen() when the request was made */
static int g_wstarted, g_wbusy, g_wwant, g_wready, g_widx;
static char g_wpaths[6][512];
static int g_wn;
static sprite_t g_wres;
static unsigned g_wres_gen, g_wres_ac_gen;

static void aw_pixels_free(sprite_t *sp){
    free(sp->fbuf); free(sp->sbuf); sp->fbuf = sp->sbuf = NULL;
}
static void aw_cancel(void){
    pthread_mutex_lock(&g_wmu);
    g_wgen++;
    if(g_wwant) g_wbusy = 0;
    g_wwant = 0;
    aw_pixels_free(&g_wres); g_wready = 0;
    pthread_mutex_unlock(&g_wmu);
}
static void *aw_worker(void *arg){
    (void)arg;
    for(;;){
        char paths[6][512]; int n, idx; unsigned gen, ac_gen;
        pthread_mutex_lock(&g_wmu);
        while(!g_wwant) pthread_cond_wait(&g_wcv, &g_wmu);
        memcpy(paths, g_wpaths, sizeof paths); n = g_wn; idx = g_widx; gen = g_wgen; ac_gen = g_wac_gen;
        g_wwant = 0;
        pthread_mutex_unlock(&g_wmu);
        sprite_t sp = { .idx = idx, .asked = 1 };
        uint8_t src[AW_SRC*AW_SRC*4];
        for(int i=0;i<n;i++){
            pthread_mutex_lock(&g_wmu);
            int stale = gen != g_wgen;
            pthread_mutex_unlock(&g_wmu);
            if(stale) break;
            if(!paths[i][0]) continue;
            if(!sd_io_begin()) break;
            char cov[600];
            int loaded = artcache_cover_path(paths[i], cov, sizeof cov)==0 && aw_read_bmp(cov, src)==0;
            sd_io_end();
            if(!loaded) continue;
            sp.fbuf = malloc(AW_SRC*AW_SPR_H*4); sp.sbuf = malloc(AW_SIDE_W*AW_SPR_H*4);
            if(sp.fbuf && sp.sbuf){
                aw_bake_front(src, sp.fbuf); aw_bake_side(src, sp.sbuf); sp.has_cover = 1;
            }else aw_pixels_free(&sp);
            break;
        }
        pthread_mutex_lock(&g_wmu);
        g_wbusy = 0;
        if(gen == g_wgen){ g_wres = sp; g_wres_gen = gen; g_wres_ac_gen = ac_gen; g_wready = 1; }
        else aw_pixels_free(&sp);
        pthread_mutex_unlock(&g_wmu);
    }
    return NULL;
}
/* Detach borrowers and decoder caches before changing or freeing a slot. */
static void aw_drop(sprite_t *sp){
    for(int c=0;c<AW_CARDS;c++){
        if(g_front[c] && lv_image_get_src(g_front[c]) == &sp->fdsc) lv_image_set_src(g_front[c], NULL);
        if(g_side[c] && lv_image_get_src(g_side[c]) == &sp->sdsc) lv_image_set_src(g_side[c], NULL);
    }
    lv_image_cache_drop(&sp->fdsc); lv_image_cache_drop(&sp->sdsc);
    aw_pixels_free(sp); memset(sp, 0, sizeof *sp); sp->idx = -1;
}
/* is this album currently on screen (a card)? Such entries must never be evicted while their image
 * widget still points at their pixel buffer, or the prefetch would swap the cover under a visible card. */
static int aw_visible(int idx){
    if(g_nalb <= 0 || idx < 0) return 0;
    int d = ((idx - g_cur) % g_nalb + g_nalb) % g_nalb;   /* circular distance 0..g_nalb-1 */
    int half = AW_CARDS/2;
    return d <= half || d >= g_nalb - half;
}
static sprite_t *aw_sprite(int idx){
    for(int i=0;i<AW_LRU;i++)
        if(g_lru[i].idx == idx){ g_lru[i].tick = ++g_tickctr; return &g_lru[i]; }
    sprite_t *lru = NULL;
    for(int i=0;i<AW_LRU;i++){                              /* evict the oldest NON-visible entry */
        if(aw_visible(g_lru[i].idx)) continue;
        if(!lru || g_lru[i].tick < lru->tick) lru = &g_lru[i];
    }
    if(!lru) lru = &g_lru[0];                               /* unreachable: AW_LRU > visible span */
    aw_drop(lru); lru->idx = idx; lru->tick = ++g_tickctr;
    return lru;
}

/* ---- flow geometry ------------------------------------------------------------------------------ */
static int aw_wrap(int idx){ return ((idx % g_nalb) + g_nalb) % g_nalb; }
static float aw_xmap(float p){
    if(p <= -2) return SLOT_X[0] + (p + 2) * (SLOT_X[1]-SLOT_X[0]);
    if(p >=  2) return SLOT_X[4] + (p - 2) * (SLOT_X[4]-SLOT_X[3]);
    int i = (int)floorf(p) + 2; if(i<0) i=0; if(i>3) i=3;
    float frac = p - floorf(p);
    return SLOT_X[i] + (SLOT_X[i+1]-SLOT_X[i]) * frac;
}
/* render one card at continuous position epos (0 = centre). Sets both its sprites' pos + opacity. */
static void aw_render_card(int c){
    float epos = c - (AW_CARDS/2) + g_off;      /* card c's base is (c - centre) */
    lv_obj_t *fi = g_front[c], *si = g_side[c];
    float a = fabsf(epos);
    if(a > 2.75f){ lv_obj_add_flag(fi, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(si, LV_OBJ_FLAG_HIDDEN); return; }

    int album = aw_wrap(g_cur + (c - AW_CARDS/2));
    sprite_t *sp = aw_sprite(album); if(!sp || !sp->has_cover) sp = &g_blank;
    if(lv_image_get_src(fi) != &sp->fdsc) lv_image_set_src(fi, &sp->fdsc);
    if(lv_image_get_src(si) != &sp->sdsc) lv_image_set_src(si, &sp->sdsc);

    int fade = 255;                              /* edge fade in/out */
    if(a > 2.25f){ fade = (int)((2.75f - a)/0.5f * 255); if(fade<0) fade=0; }
    int fo, so;                                  /* facing vs turned crossfade */
    if(a <= 1.0f){ fo = (int)((1.0f - a)*255); so = (int)(a*255); }
    else { fo = 0; so = 255; }
    fo = fo*fade/255; so = so*fade/255;

    float x = aw_xmap(epos);
    lv_obj_set_pos(fi, (int)(x - AW_SRC/2), 72);
    lv_obj_set_style_image_opa(fi, (lv_opa_t)fo, 0);
    if(fo>0) lv_obj_clear_flag(fi, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(fi, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(si, (int)(x - AW_SIDE_W/2), 78);
    lv_obj_set_style_image_opa(si, (lv_opa_t)so, 0);
    if(so>0) lv_obj_clear_flag(si, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(si, LV_OBJ_FLAG_HIDDEN);
}
static void aw_labels(void){
    if(g_nalb<=0) return;
    sprite_t *c = aw_sprite(g_cur);
    if(c && !c->has_cover){
        char in[8]={0}; const char *nm=g_names[g_cur];
        if(nm && nm[0]){ unsigned char c0=(unsigned char)nm[0];
            int len=(c0<0x80)?1:((c0>>5)==0x6)?2:((c0>>4)==0xE)?3:((c0>>3)==0x1E)?4:1;
            for(int i=0;i<len && nm[i];i++) in[i]=nm[i];
            if(len==1 && in[0]>='a'&&in[0]<='z') in[0]-=32;
        }else in[0]='?';
        lv_label_set_text(g_initial, in); lv_obj_clear_flag(g_initial, LV_OBJ_FLAG_HIDDEN);
    }else lv_obj_add_flag(g_initial, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(g_name, g_names[g_cur]);
    lv_label_set_text(g_artist, mdb_artist_missing(g_artists[g_cur]) ? "Unknown artist" : g_artists[g_cur]);
    char cc[24]; snprintf(cc,sizeof cc,"%d / %d", g_cur+1, g_nalb); lv_label_set_text(g_counter, cc);
}
static void aw_render_all(void){ for(int c=0;c<AW_CARDS;c++) aw_render_card(c); }

/* ---- continuous position + animation ------------------------------------------------------------
 * The strip is a single continuous coordinate g_posf (fractional album index, wraps mod g_nalb).
 * Everything - finger drag, inertial fling, discrete step, rim scroll - just moves g_posf; the render
 * derives g_cur (nearest album, drives the labels) and g_off (sub-album offset, drives the slide). */
static float g_posf;                     /* continuous album position; g_cur=round(g_posf), g_off=g_cur-g_posf */
static void aw_apply_pos(float P){
    if(g_nalb<=0) return;
    while(P < 0)          P += g_nalb;
    while(P >= g_nalb)    P -= g_nalb;
    g_posf = P;
    int r = (int)lroundf(P);                             /* nearest album (0..g_nalb) */
    int newcur = aw_wrap(r);
    g_off = (float)r - P;                                /* in [-0.5,0.5]; see aw_render_card sign */
    if(newcur != g_cur){ g_cur = newcur; aw_render_all(); aw_labels(); }  /* label only when the centre album flips */
    else                 aw_render_all();
}
static int   g_anim_tag;
static float g_anim_from, g_anim_to;
static void aw_anim_exec(void *v, int32_t val){ (void)v; float k=val/1000.0f; aw_apply_pos(g_anim_from + (g_anim_to-g_anim_from)*k); }
static void aw_anim_done(lv_anim_t *a){ (void)a; aw_apply_pos(g_anim_to); g_animating = 0; }
static void aw_anim_to(float to, uint32_t dur){          /* glide g_posf -> to (ease-out), then snap exact */
    lv_anim_delete(&g_anim_tag, NULL);
    g_anim_from = g_posf; g_anim_to = to; g_animating = 1;
    lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, &g_anim_tag);
    lv_anim_set_values(&a, 0, 1000); lv_anim_set_duration(&a, dur);
    lv_anim_set_exec_cb(&a, aw_anim_exec); lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, aw_anim_done); lv_anim_start(&a);
}
void albumwall_step(int dir){                            /* discrete: one album (keys / demo) */
    if(g_nalb<=0) return;
    aw_anim_to(roundf(g_posf) + (dir>=0?1.0f:-1.0f), 210);
}
/* continuous nudge (rim scroll) + snap-to-nearest (rim release) */
void albumwall_scroll_rel(float d_alb){ if(g_nalb<=0) return; lv_anim_delete(&g_anim_tag,NULL); g_animating=0; aw_apply_pos(g_posf + d_alb); }
void albumwall_settle(void){ if(g_nalb<=0) return; aw_anim_to(roundf(g_posf), 200); }

/* ---- finger drag + inertial fling --------------------------------------------------------------- */
#define AW_PX_PER_ALB 110.0f                             /* horizontal finger travel that advances one album */
static int      g_drag_on;
static float    g_drag_base;                             /* g_posf at press */
static int      g_drag_sx;                               /* press x */
static float    g_drag_vel;                              /* smoothed velocity, albums/ms (>0 = toward next) */
static int      g_drag_lastx, g_drag_renderx; static uint32_t g_drag_lastt;
void albumwall_drag_begin(int px){
    if(g_nalb<=0) return;
    lv_anim_delete(&g_anim_tag, NULL); g_animating = 0;  /* grab: stop any fling/step in flight */
    g_drag_on = 1; g_drag_base = g_posf; g_drag_sx = px;
    g_drag_vel = 0; g_drag_lastx = px; g_drag_renderx = px; g_drag_lastt = lv_tick_get();
}
void albumwall_drag(int px){
    if(!g_drag_on || g_nalb<=0) return;
    if(px != g_drag_renderx){                            /* only re-render when the finger actually moved */
        aw_apply_pos(g_drag_base - (float)(px - g_drag_sx)/AW_PX_PER_ALB);  /* finger left => next */
        g_drag_renderx = px;
    }
    uint32_t now = lv_tick_get(), dt = now - g_drag_lastt;
    if(dt >= 8){                                         /* resample velocity on a stable interval */
        float v = -(float)(px - g_drag_lastx)/AW_PX_PER_ALB/(float)dt;
        g_drag_vel = g_drag_vel*0.4f + v*0.6f;
        g_drag_lastx = px; g_drag_lastt = now;
    }
}
void albumwall_drag_cancel(void){ g_drag_on = 0; }
void albumwall_drag_end(void){                           /* release: throw a few albums, decelerate, snap */
    if(!g_drag_on || g_nalb<=0){ g_drag_on = 0; return; }
    g_drag_on = 0;
    float thr = g_drag_vel * 220.0f;                     /* coast ~220ms of the release velocity */
    if(thr >  8) thr =  8; else if(thr < -8) thr = -8;   /* cap the throw so a hard flick stays controllable */
    float target = roundf(g_posf + thr);
    float dist = target - g_posf; if(dist<0) dist=-dist;
    uint32_t dur = (uint32_t)(dist*120.0f) + 130; if(dur > 700) dur = 700;
    aw_anim_to(target, dur);
}
void albumwall_play(void){
    if(g_nalb<=0) return;
    if(ui_get_source_mode() == 4 && !ui_usb_dac_connected()){
        ui_toast("USB DAC not connected"); return;
    }
    cfg_set_int("work_mode",0); ui_set_workmode(0); if(ui_play_list(3,g_names[g_cur],1)) screen_show(SCR_NOWPLAYING);
}
void albumwall_open(void){ if(g_nalb<=0) return; library_open_album(g_names[g_cur]); screen_show(SCR_LIBRARY); }

/* ---- prefetch ----------------------------------------------------------------------------------- */
/* When the background prewarm lands a new cover, drop any placeholder sprites so they re-bake with the
 * now-available art. Re-baking a still-coverless album reproduces the same placeholder pixels, so this
 * is visually a no-op for albums whose art hasn't arrived yet (no flicker). */
static unsigned g_last_ac_gen;
static void aw_rebake_new_covers(void){
    int vis_changed = 0;
    for(int i=0;i<AW_LRU;i++){
        if(g_lru[i].idx>=0 && !g_lru[i].has_cover){        /* only placeholders can gain art */
            if(aw_visible(g_lru[i].idx)) vis_changed = 1;
            g_lru[i].asked = 0;                             /* retry in the worker */
        }
    }
    if(vis_changed){ aw_render_all(); aw_labels(); }       /* repaint the on-screen cards + centre initial */
}

/* ---- background album-cover prewarm seed (decoupled from the Cover Flow view) --------------------
 * Enumerates the album list on the MAIN thread (mdb in-memory caches are not thread-safe) and enqueues
 * the representative track of every album lacking a cached cover, for the ui.c worker to decode. Runs
 * incrementally off its own timer so it never hitches boot/scan, re-walks the whole list each start
 * (cheap: one indexed lookup per album - the WORKER skips albums whose cover is already cached, so no SD
 * file checks run on the UI thread) so it catches same-count library changes, and needs no visit to the
 * Album view. Started at boot, after each scan, and on Cover Flow open. */
static char (*g_seed_names)[MDB_STR];
static int         g_seed_n, g_seed_cur;
static lv_timer_t *g_seed_timer;
static int *g_seed_rep;                                     /* per album: representative SONG.ID (0 = none) */
static void aw_seed_cb(lv_timer_t *t){
    if(!g_seed_names || g_seed_cur >= g_seed_n){ lv_timer_delete(t); g_seed_timer = NULL; return; }
    lv_timer_set_period(t, 50);                                 /* back to full pace (a full queue slows it below) */
    for(int k=0; k<3 && g_seed_cur < g_seed_n; k++){
        /* The album's representative song (its first in library order, recorded when the album cache was built):
         * one indexed ID lookup instead of comparing the album name against every song. Only if that path can't
         * be read fall back to the album's other songs, as before. */
        int ids[6], m = 0;
        if(g_seed_rep && g_seed_rep[g_seed_cur] > 0){ ids[0] = g_seed_rep[g_seed_cur]; m = 1; }
        for(int pass = 0; pass < 2; pass++){
            int found = 0;
            for(int i=0;i<m;i++){
                char track[512]; track[0]=0;
                if(mdb_song_path(ids[i], track, sizeof track) && track[0]){
                    found = 1;
                    if(!ui_prewarm_enqueue(track)){            /* queue full: the worker isn't draining it right now
                                                                 * (heat, card access, a live decode) - retry this album
                                                                 * slowly instead of re-querying it 20x a second */
                        lv_timer_set_period(t, 2000);
                        return;
                    }
                    break;                                      /* one representative track per album */
                }
            }
            if(found || pass) break;
            m = mdb_album_track_ids(g_seed_names[g_seed_cur], ids, 6);   /* fallback: the slow scan */
        }
        g_seed_cur++;
    }
}
void albumwall_prewarm_seed(void){                          /* MAIN thread: (re)start the incremental enqueue walk */
    static char (*artists)[MDB_STR]; static int *counts; static int seed_cap = 0;
    int want = mdb_album_count();                           /* size to the real album count (no >600 truncation) */
    if(want < 1) return;                                    /* <0 = OOM building the cache, 0 = empty -> nothing to seed (retry next start) */
    if(!g_seed_names || seed_cap < want){                   /* main-thread only + repopulated below -> free+malloc safe */
        free(g_seed_names); free(artists); free(counts); free(g_seed_rep);
        g_seed_names = malloc((size_t)want*MDB_STR);
        artists      = malloc((size_t)want*MDB_STR);
        counts       = malloc((size_t)want*sizeof(int));
        g_seed_rep   = malloc((size_t)want*sizeof(int));
        if(!g_seed_names || !artists || !counts || !g_seed_rep){ free(g_seed_names); free(artists); free(counts); free(g_seed_rep); g_seed_names=NULL; artists=NULL; counts=NULL; g_seed_rep=NULL; seed_cap=0; return; }
        seed_cap = want;
    }
    g_seed_n = mdb_albums(g_seed_names, artists, counts, seed_cap); g_seed_cur = 0;
    if(mdb_album_rep_ids(g_seed_rep, seed_cap) != g_seed_n)
        for(int i=0;i<seed_cap;i++) g_seed_rep[i] = 0;      /* reps unavailable -> every album uses the fallback scan */
    if(g_seed_n > 0 && !g_seed_timer) g_seed_timer = lv_timer_create(aw_seed_cb, 50, NULL);
}

static lv_timer_t *g_prefetch;
static int g_active;
static int g_prefetch_cur = -1;                  /* centre whose neighbours are settled; -1 = work left */
/* After leaving: stop the worker at once; a fling still runs to its target, so the sprites go only once the
 * root is hidden and the motion has settled. 1 = nothing left to clean up. */
static int aw_leave(void){
    if(g_active){ aw_cancel(); g_active = 0; g_drag_on = 0; }
    int held = 0;
    for(int i=0;i<AW_LRU;i++) if(g_lru[i].idx>=0) held = 1;
    if(!held) return 1;
    if(!g_root || !lv_obj_has_flag(g_root, LV_OBJ_FLAG_HIDDEN) || g_animating) return 0;
    for(int i=0;i<AW_LRU;i++) if(g_lru[i].idx>=0) aw_drop(&g_lru[i]);
    return 1;
}
/* Collect a finished result, then ask for the nearest missing neighbour (one request at a time).
 * 1 = work left: a request in flight or uncollected, or a neighbour not yet asked. */
static int aw_prefetch_step(unsigned g){
    pthread_mutex_lock(&g_wmu);
    if(g_wready){
        sprite_t sp = g_wres; unsigned gen = g_wres_gen, ac_gen = g_wres_ac_gen;
        memset(&g_wres, 0, sizeof g_wres); g_wready = 0;
        pthread_mutex_unlock(&g_wmu);
        if(!sp.has_cover && ac_gen != g) sp.asked = 0;   /* a cover landed after this miss was read: retry */
        int installed = 0;
        if(gen == g_wgen && sd_io_allowed()) for(int i=0;i<AW_LRU;i++){
            sprite_t *dst = &g_lru[i];
            if(dst->idx != sp.idx) continue;
            uint32_t tick = dst->tick; aw_drop(dst); *dst = sp; dst->tick = tick;
            if(sp.has_cover){
                aw_set_dsc(&dst->fdsc, dst->fbuf, AW_SRC, AW_SPR_H);
                aw_set_dsc(&dst->sdsc, dst->sbuf, AW_SIDE_W, AW_SPR_H);
            }
            installed = 1; break;
        }
        if(!installed) aw_pixels_free(&sp);
        if(installed && sp.has_cover && aw_visible(sp.idx)){
            aw_render_all(); if(sp.idx == g_cur) aw_labels();
        }
    }else pthread_mutex_unlock(&g_wmu);
    for(int d=0;d<=AW_PREFETCH;d++) for(int side=-1;side<=1;side+=2){
        sprite_t *sp = aw_sprite(aw_wrap(g_cur + side*d));
        if(sp->has_cover || sp->asked) continue;
        pthread_mutex_lock(&g_wmu);
        if(!g_wbusy && !g_wready){
            if(!g_wstarted){
                pthread_t th;
                if(pthread_create(&th, NULL, aw_worker, NULL)==0){ pthread_detach(th); g_wstarted = 1; }
            }
            if(g_wstarted){
                int ids[6]; g_wn = mdb_album_track_ids(g_names[sp->idx], ids, 6);
                for(int i=0;i<g_wn;i++){ g_wpaths[i][0]=0; mdb_song_path(ids[i], g_wpaths[i], sizeof g_wpaths[i]); }
                g_widx = sp->idx; g_wac_gen = g; sp->asked = 1; g_wbusy = g_wwant = 1;
                pthread_cond_signal(&g_wcv);
            }
        }
        pthread_mutex_unlock(&g_wmu);
        return 1;
    }
    pthread_mutex_lock(&g_wmu);
    int pending = g_wbusy || g_wready;
    pthread_mutex_unlock(&g_wmu);
    return pending;
}
/* Runs only while there is work: albumwall_poll_sync resumes it on the lit wall when the centre or the art
 * cache changed or work is left; it pauses itself once settled. Leaving is cleaned up by albumwall_poll_sync. */
static void aw_prefetch_cb(lv_timer_t *t){
    if(screen_current()!=SCR_ALBUMWALL){ if(aw_leave()) lv_timer_pause(t); return; }
    if(g_nalb<=0){ lv_timer_pause(t); return; }
    unsigned g = artcache_gen();
    if(g != g_last_ac_gen){ g_last_ac_gen = g; aw_rebake_new_covers(); }
    if(aw_prefetch_step(g)){ g_prefetch_cur = -1; return; }
    g_prefetch_cur = g_cur;
    lv_timer_pause(t);
}

/* Main loop, every pass: no callbacks while dark or off the wall; leaving cleans up here without the timer. */
void albumwall_poll_sync(int lit){
    if(!g_prefetch) return;
    if(screen_current()!=SCR_ALBUMWALL){ aw_leave(); lv_timer_pause(g_prefetch); }
    else if(!lit || g_nalb<=0) lv_timer_pause(g_prefetch);
    else if(g_prefetch_cur != g_cur || g_last_ac_gen != artcache_gen()) lv_timer_resume(g_prefetch);
}

/* ---- lifecycle ---------------------------------------------------------------------------------- */
static void aw_back_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) screen_back(); }
static lv_obj_t *aw_img(void){ lv_obj_t *im = lv_image_create(g_root); lv_obj_add_flag(im, LV_OBJ_FLAG_HIDDEN); return im; }

void albumwall_create(lv_obj_t *root){
    g_root = root;
    if(!g_prefetch) g_prefetch = lv_timer_create(aw_prefetch_cb, 40, NULL);
    if(g_prefetch) lv_timer_pause(g_prefetch);
}

void albumwall_refresh(void){
    if(!g_root) return;
    aw_cancel(); g_active = 1;
    g_prefetch_cur = -1;
    if(!g_aw_inited){
        /* placeholder sprites (built once; BGRA from the theme's art-wall roles) */
        static uint8_t fb[AW_SRC*AW_SPR_H*4], sb[AW_SIDE_W*AW_SPR_H*4];
        memset(fb,0,sizeof fb); memset(sb,0,sizeof sb);
        uint32_t fc = theme_rgb(THEME_CLR_ART_WALL_FRONT), sc = theme_rgb(THEME_CLR_ART_WALL_SIDE);
        for(int y=0;y<AW_SRC;y++){ for(int x=0;x<AW_SRC;x++){ uint8_t *d=fb+(y*AW_SRC+x)*4; d[0]=fc&255;d[1]=fc>>8&255;d[2]=fc>>16&255;d[3]=0xFF; }
                                   for(int x=0;x<AW_SIDE_W;x++){ uint8_t *d=sb+(y*AW_SIDE_W+x)*4; d[0]=sc&255;d[1]=sc>>8&255;d[2]=sc>>16&255;d[3]=0xFF; } }
        g_blank.has_cover=0; g_blank.idx=-2; aw_set_dsc(&g_blank.fdsc, fb, AW_SRC, AW_SPR_H); aw_set_dsc(&g_blank.sdsc, sb, AW_SIDE_W, AW_SPR_H);
        for(int i=0;i<AW_LRU;i++) g_lru[i].idx=-1;
        g_aw_inited = 1;
    }
    /* Size the album buffers to the REAL album count so a >600-album library is never truncated (the
     * Library list sizes dynamically too). They're repopulated fresh below, so free+malloc on grow is safe. */
    { int want = mdb_album_count();
      if(want < 0){                       /* album cache couldn't be built (OOM) -> don't fake a capacity-1 buffer;
                                           * drop to the "Out of memory" path below instead of a 1-album list */
          free(g_names); free(g_artists); free(g_counts); g_names=NULL; g_artists=NULL; g_counts=NULL; g_names_cap=0;
      } else {
          if(want < 1) want = 1;
          if(!g_names || g_names_cap < want){
              free(g_names); free(g_artists); free(g_counts);
              g_names   = malloc((size_t)want * MDB_STR);
              g_artists = malloc((size_t)want * MDB_STR);
              g_counts  = malloc((size_t)want * sizeof(int));
              if(!g_names||!g_artists||!g_counts){ free(g_names); free(g_artists); free(g_counts); g_names=NULL; g_artists=NULL; g_counts=NULL; g_names_cap=0; }
              else g_names_cap = want;
          }
      }
    }
    lv_anim_delete(&g_anim_tag, NULL);           /* stop any in-flight fling BEFORE lv_obj_clean frees the widgets it animates */
    for(int i=0;i<AW_LRU;i++) aw_drop(&g_lru[i]);   /* album list may have changed */
    g_off = 0; g_animating = 0; g_drag_on = 0;

    lv_obj_clean(g_root);
    for(int c=0;c<AW_CARDS;c++){ g_front[c]=NULL; g_side[c]=NULL; }
    lv_obj_set_style_bg_color(g_root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    ui_header_cb(g_root, "Albums", aw_back_cb);

    g_nalb = g_names ? mdb_albums(g_names, g_artists, g_counts, g_names_cap) : 0;
    if(g_nalb <= 0){
        lv_obj_t *e = lv_label_create(g_root);
        lv_label_set_text(e, g_names ? "No albums found" : "Out of memory");
        lv_obj_center(e); lv_obj_set_style_text_color(e, TC(TEXT_MUTED), 0);
        return;
    }
    if(g_cur >= g_nalb) g_cur = 0;
    g_posf = g_cur; g_drag_on = 0;   /* sync the continuous coordinate to the (possibly clamped) centre album */
    albumwall_prewarm_seed();        /* (re)kick the background cover fill for the current album set */

    /* create cards: sides (turned) first, then fronts (facing) so a facing cover draws over a turned one;
     * within each, outer cards first so the centre is on top */
    static const int zc[AW_CARDS] = { 0,8,1,7,2,6,3,5,4 };
    for(int k=0;k<AW_CARDS;k++) g_side[zc[k]]  = aw_img();
    for(int k=0;k<AW_CARDS;k++) g_front[zc[k]] = aw_img();

    g_initial = lv_label_create(g_root);
    lv_obj_set_style_text_font(g_initial, TF(USER_20), 0);
    lv_obj_set_style_text_color(g_initial, TC(TEXT_MUTED), 0);
    lv_obj_align(g_initial, LV_ALIGN_TOP_MID, 0, 72 + AW_SRC/2 - 12);
    lv_obj_add_flag(g_initial, LV_OBJ_FLAG_HIDDEN);

    g_name = lv_label_create(g_root);
    lv_label_set_long_mode(g_name, LV_LABEL_LONG_DOT); lv_obj_set_size(g_name, 260, 24);
    lv_obj_set_style_text_align(g_name, LV_TEXT_ALIGN_CENTER, 0); lv_obj_align(g_name, LV_ALIGN_TOP_MID, 0, 266);
    lv_obj_set_style_text_font(g_name, TF(USER_18), 0); lv_obj_set_style_text_color(g_name, TC(TEXT_PRIMARY), 0);

    g_artist = lv_label_create(g_root);
    lv_label_set_long_mode(g_artist, LV_LABEL_LONG_DOT); lv_obj_set_size(g_artist, 220, 20);
    lv_obj_set_style_text_align(g_artist, LV_TEXT_ALIGN_CENTER, 0); lv_obj_align(g_artist, LV_ALIGN_TOP_MID, 0, 292);
    lv_obj_set_style_text_font(g_artist, TF(USER_16), 0); lv_obj_set_style_text_color(g_artist, TC(TEXT_MUTED), 0);

    g_counter = lv_label_create(g_root);
    lv_obj_align(g_counter, LV_ALIGN_TOP_MID, 0, 318);
    lv_obj_set_style_text_font(g_counter, TF(UI_14), 0); lv_obj_set_style_text_color(g_counter, TC(TEXT_DISABLED), 0);

    aw_render_all(); aw_labels();
}
