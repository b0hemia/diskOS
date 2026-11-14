/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ui_wake.h"
/* Copyright (C) 2026 diskOS contributors */
#include "indic.h"
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "config.h"
#include "i18n.h"
#include "custom_saver_overlay.h"
#include "custom_saver_db.h"
#include "custom_saver_apply.h"
#include <fcntl.h>
#include <time.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Idle screensaver with selectable STYLES (cfg "saver_style"):
 *   0 Cover   - dim blurred album-art backdrop + digital clock + track (classic)
 *   1 Analog  - round analog clock face (hour/minute hands) + date
 *   2 Minimal - just the big time, centered, black
 *   3 Digital - big clock + date + weather, no art, no track (clean info)
 * Any touch wakes (handled in main.c). Kept cheap: static blit + labels;
 * analog hands recompute only on the ~10s clock tick. */

int ui_label_input_changed(lv_obj_t *obj, const char *txt);   /* ui.c: a label's text only when its input changes */
int ui_label_set_changed(lv_obj_t *obj, const char *txt);

LV_FONT_DECLARE(font_weather16)
static lv_font_t s_swfont;   /* montserrat_16 + weather-icon fallback */

static lv_timer_t *g_analog_timer;
static lv_obj_t *g_bg;
static lv_obj_t *g_clock;
static void saver_clock_style_cb(lv_event_t *e);   /* refits the time when the theme changes its face */
static void saver_clock_fit(void);                  /* lays out the time (+ AM/PM beside it, hidden with the clock) */
static lv_obj_t *g_date;
static lv_obj_t *g_weather;
static lv_obj_t *g_mer;   /* AM/PM in a text face, for a theme whose clock face has no letters (saver_mer_apart) */
static lv_obj_t *g_track;
static lv_obj_t *g_artist;
static int g_playing;
static lv_obj_t *g_root;   /* the saver's screen root */

/* ---- Custom style (slice 14): the installed lock-screen image with the overlay group, in ONE container that exists
 * only while the lit saver is on screen. Every exit or dark path deletes it (screenmgr.c -> saver_leave, main.c ->
 * saver_dark): the GIF object (its destructor closes the decoder and the file and deletes its timer), the overlay
 * group, the decode-cache entry of a still, and the weather hold. Hiding it would leave all of those running. ---- */
#define SAVER_STYLE_CUSTOM   5
#define SAVER_STYLE_FALLBACK 2   /* Minimal: what a missing or corrupt image shows */
typedef struct {
    lv_obj_t *box, *img;
    custom_ov_group_t *grp;
    char still[1100];            /* lv_fs path of a JPEG/BMP still (its cache entry is dropped on teardown), "" otherwise */
    unsigned char *px;           /* a PNG still decoded to BGRA (the PNG decoder is stb's, not LVGL's); freed on teardown */
    lv_image_dsc_t dsc;
    int lang, held;              /* held: this build acquired one weather_hold, not yet released */
} cu_t;
static cu_t g_cu;                /* the lock screen's */
static cu_t g_cp;                /* the settings Preview's (slice 17d): the same builder on another parent, with the draft as the model */
static int g_cfg_style = -1;     /* the saver_style last laid out, so a tick re-lays-out only when it changes */
static int g_cu_msg;             /* a fallback happened: one message is due on the next wake */
static int cu_on(void){ return g_cu.box != NULL; }
int ui_screen_backlight_on(void);

/* analog clock */
static lv_obj_t *g_face;
static lv_obj_t *g_tick[12];
static lv_obj_t *g_hour;
static lv_obj_t *g_min;
static lv_obj_t *g_sec;
static lv_obj_t *g_hub;
static lv_point_precise_t g_hpts[2], g_mpts[2], g_spts[2];

/* vinyl (stock-style: just the spinning square cover - no disc/label/spindle) */
static lv_obj_t *g_vinyl;    /* the spinning square cover image */
static int g_vspin = 0;      /* spin state (idempotent) */
static int g_have_track = 0; /* is a track currently loaded? The art savers (Cover backdrop + Vinyl
                                cover) show ONLY when a track is loaded, so an idle player shows
                                nothing (blank/clock) instead of a stale cover. Set by
                                saver_set_track. The spin is separately gated on actually-playing
                                (main.c), so a paused-but-loaded track shows the cover, frozen. */

static int g_style = -1;   /* currently-applied style */
static int g_song_dirty = 1;  /* an input of the tick's layout changed (a text, the style): run it on the next tick */
static int blue_saver(void){ return !strcmp(theme_kit()->id, "blueprint"); }
static void blue_song_layout(void){
    if(!blue_saver() || !g_clock || !theme_kit()->saver) return;
    saver_parts_t sp = { lv_obj_get_parent(g_clock), g_clock, g_date, g_weather, g_track, g_artist, g_face, g_style };
    theme_kit()->saver(&sp);
}

/* Shared sharp cover: one unrotated 360px current-source buffer, borrowed by
 * bauhaus NP and read by the vinyl rotation code. UI thread owns its lifetime. */
#define VIN_W 360
static uint8_t *g_vbuf = NULL;       /* 360x360 BGRA (== LVGL ARGB8888 byte order) */
static int g_vbound;                /* vinyl has bound the current shared source */
static int g_vbuf_valid = 0;         /* does g_vbuf hold a good decode of the CURRENT track's cover?
                                        A failed/absent decode must NOT display the previous track's
                                        buffer, so visibility + brightness gate on this, not on g_vbuf. */
static lv_image_dsc_t g_vdsc;

/* The spin: diskOS rotates the cover itself into g_vout (a second 360x360 ARGB image the widget shows as-is) instead of
 * asking LVGL to transform the image every frame. LVGL's generic transform cost ~19 ms/frame with anti-aliasing OFF
 * (jagged edges) and ~52 ms with it on; this fixed-point bilinear loop over the round screen only measured ~6 ms/frame
 * on the Disc with smooth edges, so the spin costs a fraction of the CPU (heat/battery)
 * and looks better. Outside the rotated square stays transparent, as before. */
static uint8_t *g_vout = NULL;       /* 360x360 BGRA: the rotated cover the widget shows */
static lv_image_dsc_t g_vodsc;
static int32_t g_vangle = 0;         /* current angle, 0.1 degree units */
static int g_vrandom_on_wake;
static inline uint32_t vin_px(const uint32_t *src, int x, int y){
    return ((unsigned)x < VIN_W && (unsigned)y < VIN_W) ? src[y * VIN_W + x] : 0;   /* outside: transparent */
}
static inline uint32_t vin_lerp(uint32_t a, uint32_t b, uint32_t w){   /* per-channel a..b, w in 0..256 */
    uint32_t rb = (((a & 0x00FF00FFu) * (256 - w) + (b & 0x00FF00FFu) * w) >> 8) & 0x00FF00FFu;
    uint32_t ag = ((((a >> 8) & 0x00FF00FFu) * (256 - w) + ((b >> 8) & 0x00FF00FFu) * w)) & 0xFF00FF00u;
    return rb | ag;
}
__attribute__((optimize("O2")))
static void vinyl_rotate(int32_t tenths)
{
    if (!g_vout || !g_vbuf) return;
    const uint32_t *src = (const uint32_t *)g_vbuf;
    uint32_t *out = (uint32_t *)g_vout;
    double a = (double)tenths * 3.14159265358979 / 1800.0;
    int32_t c = (int32_t)lround(cos(a) * 65536.0), sn = (int32_t)lround(sin(a) * 65536.0);
    const int32_t h = VIN_W / 2;
    /* Fixed geometry, prepared once on the UI thread. Keep the original sqrt
     * and integer conversion so every span has exactly the previous endpoints. */
    static struct { int16_t dy, x0, x1; } rows[VIN_W];
    static int rows_ready;
    if(!rows_ready){
        for(int y = 0; y < VIN_W; y++){
            int32_t dy = y - h;
            int32_t hw = (int32_t)sqrt((double)(h * h - dy * dy));
            int x0 = h - hw, x1 = h + hw;
            if(x0 < 0) x0 = 0;
            if(x1 > VIN_W) x1 = VIN_W;
            rows[y].dy = dy; rows[y].x0 = x0; rows[y].x1 = x1;
        }
        rows_ready = 1;
    }
    for (int y = 0; y < VIN_W; y++) {
        int32_t dy = rows[y].dy;
        int x0 = rows[y].x0, x1 = rows[y].x1;
        /* inverse map (destination -> source), stepped along the row; +0.5 px so pixel centres line up */
        int32_t u = (x0 - h) * c + dy * sn + (h << 16), v = -(x0 - h) * sn + dy * c + (h << 16);
        uint32_t *o = out + y * VIN_W;
        for (int x = x0; x < x1; x++, u += c, v -= sn) {
            int xi = u >> 16, yi = v >> 16;
            uint32_t fx = (u >> 8) & 0xFF, fy = (v >> 8) & 0xFF;
            uint32_t p00, p10, p01, p11;
            if ((unsigned)xi < VIN_W - 1 && (unsigned)yi < VIN_W - 1) {   /* interior: no bounds checks */
                const uint32_t *q = src + yi * VIN_W + xi;
                p00 = q[0]; p10 = q[1]; p01 = q[VIN_W]; p11 = q[VIN_W + 1];
            } else {                                                      /* the square's edge: soft, transparent out */
                p00 = vin_px(src, xi, yi); p10 = vin_px(src, xi + 1, yi);
                p01 = vin_px(src, xi, yi + 1); p11 = vin_px(src, xi + 1, yi + 1);
            }
            o[x] = vin_lerp(vin_lerp(p00, p10, fx), vin_lerp(p01, p11, fx), fy) | 0xFF000000u;   /* opaque: see below */
        }
    }
    lv_image_cache_drop(&g_vodsc);
    if (g_vinyl) lv_obj_invalidate(g_vinyl);
}

static void song_visibility(void);
static void vinyl_update_vis(void)
{
    if (!g_vinyl) return;
    if (g_style == 4 && g_vbound && g_vbuf_valid && g_have_track) lv_obj_remove_flag(g_vinyl, LV_OBJ_FLAG_HIDDEN);
    else                                                        lv_obj_add_flag(g_vinyl, LV_OBJ_FLAG_HIDDEN);
    song_visibility();   /* the song label steps aside while the spinning cover shows (and returns if it cannot) */
}

/* The vinyl saver is an album-art SHOWCASE, so it should stay at the user's set
 * brightness rather than crushing to the clock-saver dim level (which reads as a
 * washed-out/blurry cover on the panel). It still powers fully off after the
 * screen-off delay, so there's no power regression. */
int saver_wants_bright(void) { return g_style == 4 && g_have_track && g_vbuf_valid; }

/* Sharp vinyl cover, decoded OFF the UI thread.
 *
 * The UI thread only posts a request (the track's path + a generation number) and returns. A worker thread takes
 * the shared decode lock and runs diskos-artdec in "saver" mode on the TRACK ITSELF (embedded art, else its sidecar
 * cover), through art_make_saver: the same SD lease, deadline, revocation and staged-publish rules as all artwork.
 * It reads the result into a NEW buffer and hands it back. A poll timer on the UI thread installs it only if its
 * generation is still the current one, so a track change during the decode can never show the previous cover.
 * A track change hides the old cover at once. If the worker cannot start, the cover simply stays hidden. */
#include <pthread.h>
#include <fcntl.h>
#include "art.h"
void ui_decode_lock(void); void ui_decode_unlock(void);
static art_picture_id g_vpicture;
static int g_vbook, g_vheld_book, g_vheld_valid, g_vawait_identity;
static art_picture_id g_vheld_picture;
static char g_vheld_path[1024];
static char g_vpath[1024];               /* UI thread: the track the vinyl cover is for ("" = none) */
static unsigned g_vgen;                  /* UI thread: bumped on every track change */
static pthread_mutex_t g_vmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_vcv = PTHREAD_COND_INITIALIZER;
static int g_vwork_started, g_vwant;     /* under g_vmu: worker running; a request is pending */
static char g_vwant_path[1024];          /* under g_vmu */
static unsigned g_vwant_gen;             /* under g_vmu */
static int64_t g_vwant_deadline;         /* under g_vmu: CLOCK_MONOTONIC ms, fixed when the request was posted */
static int g_vfails;                     /* UI thread: failed attempts for the current track (retry limit) */
#define VIN_RETRIES 3
static int64_t vin_now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static uint8_t *g_vres; static unsigned g_vres_gen; static int g_vres_ready;   /* under g_vmu: finished decode */
static unsigned g_vasked_gen = ~0u;      /* UI thread: generation last requested (no duplicate requests) */
static lv_timer_t *g_vpoll;
static int g_vpending;

static void *vinyl_worker(void *arg)
{
    (void)arg;
    const size_t need = (size_t)VIN_W * VIN_W * 4;
    for (;;) {
        pthread_mutex_lock(&g_vmu);
        while (!g_vwant) pthread_cond_wait(&g_vcv, &g_vmu);
        char path[1024]; snprintf(path, sizeof path, "%s", g_vwant_path);
        art_picture_id picture = g_vpicture;
        unsigned gen = g_vwant_gen; int64_t deadline = g_vwant_deadline; g_vwant = 0;
        pthread_mutex_unlock(&g_vmu);

        char out[64]; snprintf(out, sizeof out, "/tmp/.diskos-vinyl-%u.bgra", gen);
        uint8_t *buf = NULL;
        ui_decode_lock();                               /* one decoder at a time, shared with NP + prewarm */
        /* after waiting for the lock: a newer request, or a spent deadline, means this one is not worth decoding */
        pthread_mutex_lock(&g_vmu);
        int superseded = g_vgen != gen;
        pthread_mutex_unlock(&g_vmu);
        int rc = (superseded || vin_now_ms() >= deadline) ? -1 : art_make_saver_checked(path, out, deadline, picture.length ? &picture : NULL);
        ui_decode_unlock();
        if (rc == 0) {
            int fd = open(out, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            buf = malloc(need);
            size_t got = 0;
            if (fd >= 0 && buf) {
                ssize_t n;
                while (got < need && (n = read(fd, buf + got, need - got)) > 0) got += (size_t)n;
                char extra; if (got == need && read(fd, &extra, 1) != 0) got = 0;   /* too long is as bad as too short */
            }
            if (fd >= 0) close(fd);
            if (got != need) { free(buf); buf = NULL; }
        }
        unlink(out);
        pthread_mutex_lock(&g_vmu);
        if (gen != g_vgen) { free(buf); pthread_mutex_unlock(&g_vmu); continue; }
        free(g_vres);                                   /* an older, never-collected result */
        g_vres = buf; g_vres_gen = gen; g_vres_ready = 1;
        pthread_mutex_unlock(&g_vmu);
        ui_notify(UI_WAKE_SHARP);
    }
    return NULL;
}

/* UI thread: vinyl alone owns the optional rotation output. */
static void vinyl_bind(void)
{
    if (!g_vinyl || !g_vbuf_valid || g_vbound) return;
    int reuse_rotation = g_vout && g_vodsc.data == g_vout;
    if (!g_vout) g_vout = calloc((size_t)VIN_W * VIN_W, 4);   /* zeroed: outside the round area stays transparent */
    if (g_vout) {                                            /* show the diskOS-rotated copy (see vinyl_rotate) */
        g_vodsc = g_vdsc;
        g_vodsc.data = g_vout;
        /* OPAQUE: a square cover turned about its centre always covers the round screen, so no visible pixel is ever
         * see-through - and an opaque image is a plain copy for LVGL instead of a per-pixel alpha blend (~4-5 ms of
         * the frame). Outside the circle (invisible on the round panel) stays black. */
        g_vodsc.header.cf = LV_COLOR_FORMAT_XRGB8888;
        if(!reuse_rotation) vinyl_rotate(g_vangle);
        lv_image_set_src(g_vinyl, &g_vodsc);
        lv_image_set_rotation(g_vinyl, 0);
    } else {                                                 /* no memory for the copy: LVGL rotates, as before */
        lv_image_set_src(g_vinyl, &g_vdsc);
        lv_image_set_pivot(g_vinyl, VIN_W / 2, VIN_W / 2);
        lv_image_set_rotation(g_vinyl, g_vangle);
    }
    lv_image_set_scale(g_vinyl, 256);                    /* 1x - already 360px, no upscale */
    lv_obj_center(g_vinyl);
    g_vbound = 1;
    vinyl_update_vis();
}

const lv_image_dsc_t *sharp_cover_get(void)
{
    return g_vbuf_valid ? &g_vdsc : NULL;
}

/* Detach both borrowers and evict caches before freeing any pixel allocation. */
static void sharp_pixels_clear(void){
    g_vheld_valid = 0; memset(&g_vheld_picture, 0, sizeof g_vheld_picture); g_vheld_path[0] = 0;
    g_vbuf_valid = g_vbound = 0;
    if(g_vinyl) lv_image_set_src(g_vinyl, NULL);
    ui_np_cover_changed();
    lv_image_cache_drop(&g_vdsc); lv_image_cache_drop(&g_vodsc);
    free(g_vbuf); g_vbuf = NULL; free(g_vout); g_vout = NULL;
    g_vdsc.data = g_vodsc.data = NULL;
    vinyl_update_vis();
}
void sharp_cover_invalidate(void)
{
    pthread_mutex_lock(&g_vmu);
    memset(&g_vpicture, 0, sizeof g_vpicture);
    g_vgen++; g_vwant = 0;
    free(g_vres); g_vres = NULL; g_vres_ready = 0;
    pthread_mutex_unlock(&g_vmu);
    g_vpending = 0; g_vawait_identity = 1;
    if(g_vpoll) lv_timer_pause(g_vpoll);
    g_vfails = 0; g_vasked_gen = ~0u;
    sharp_pixels_clear();
}

void sharp_cover_set_source(const char *path)
{
    const char *p = path ? path : "";
    if (!strcmp(p, g_vpath)) return;
    sharp_cover_invalidate();
    snprintf(g_vpath, sizeof g_vpath, "%s", p);
}

static void sharp_supersede(void){
    pthread_mutex_lock(&g_vmu);
    g_vgen++; g_vwant = 0;
    free(g_vres); g_vres = NULL; g_vres_ready = 0;
    pthread_mutex_unlock(&g_vmu);
    g_vpending = 0; g_vasked_gen = ~0u; g_vfails = 0;
    if(g_vpoll) lv_timer_pause(g_vpoll);
}
void sharp_cover_set_identity(const char *path, const art_picture_id *picture, int book)
{
    const char *p = path ? path : "";
    if(!p[0]){ sharp_cover_set_source(""); return; }
    if(picture && !picture->length) picture = NULL;
    int same = !strcmp(p, g_vpath);
    if(picture && g_vheld_valid && art_picture_equal(picture, &g_vheld_picture) &&
       (!book || (g_vheld_book && !strcmp(p, g_vheld_path)))){
        /* Restore retained content with fresh request ownership, even if a
         * different picture was pending. Never adopt that obsolete result. */
        if(!same || g_vawait_identity || !art_picture_equal(picture, &g_vpicture)) sharp_supersede();
        pthread_mutex_lock(&g_vmu); g_vpicture = *picture; pthread_mutex_unlock(&g_vmu);
        g_vawait_identity = 0; g_vbuf_valid = 1; g_vbook = g_vheld_book = book;
        snprintf(g_vpath, sizeof g_vpath, "%s", p); snprintf(g_vheld_path, sizeof g_vheld_path, "%s", p);
        if(g_style == 4 && screen_current() == SCR_SAVER) vinyl_bind();
        return;
    }
    if(same && ((!picture && g_vawait_identity) ||
       (picture && !g_vawait_identity && art_picture_equal(picture, &g_vpicture)))) return;
    sharp_supersede();
    g_vbuf_valid = g_vbound = 0; g_vawait_identity = picture == NULL; g_vbook = book;
    snprintf(g_vpath, sizeof g_vpath, "%s", p);
    pthread_mutex_lock(&g_vmu);
    if(picture) g_vpicture = *picture; else memset(&g_vpicture, 0, sizeof g_vpicture);
    pthread_mutex_unlock(&g_vmu);
    if(g_vinyl) lv_image_set_src(g_vinyl, NULL);
    ui_np_cover_changed(); vinyl_update_vis();
    /* Keep old pixels hidden until a different picture actually lands, so a
     * rapid return can reuse them. Unknown sources wait for NP identity. */
}

/* Who keeps the sharp cover: Bauhaus Now Playing, the vinyl saver, and the full-screen art while it is open.
 * Unknown sources hide retained pixels; a verified different identity detaches before freeing. */
static int sharp_retained(void)
{
    return !strcmp(theme_kit()->id, "bauhaus") || cfg_get_int("saver_style", 0) == 4 || ui_np_fsart_active();
}

/* Install without requiring a vinyl widget. The descriptor belongs to the provider. */
static void vinyl_poll(lv_timer_t *t)
{
    pthread_mutex_lock(&g_vmu);
    if (!g_vres_ready) { pthread_mutex_unlock(&g_vmu); lv_timer_pause(t); return; }
    uint8_t *buf = g_vres; unsigned gen = g_vres_gen;
    g_vres = NULL; g_vres_ready = 0;
    pthread_mutex_unlock(&g_vmu);
    if (gen != g_vgen) { free(buf); if (!g_vpending) lv_timer_pause(t); return; }
    g_vpending = 0;
    lv_timer_pause(t);
    if (!sharp_retained()) { free(buf); g_vasked_gen = ~0u; return; }   /* nobody keeps it now: a later demand asks again */
    if (!buf) {
        if (++g_vfails < VIN_RETRIES) g_vasked_gen = ~0u;
        return;                         /* retry only on another visible demand */
    }
    if(g_vbuf || g_vout) sharp_pixels_clear();
    g_vbuf = buf; g_vbuf_valid = 1; g_vheld_valid = g_vpicture.length != 0;
    g_vheld_picture = g_vpicture; g_vheld_book = g_vbook;
    snprintf(g_vheld_path, sizeof g_vheld_path, "%s", g_vpath);
    memset(&g_vdsc, 0, sizeof g_vdsc);
    g_vdsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    g_vdsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    g_vdsc.header.w = g_vdsc.header.h = VIN_W;
    g_vdsc.header.stride = VIN_W * 4;
    g_vdsc.data = buf; g_vdsc.data_size = (uint32_t)VIN_W * VIN_W * 4;
    ui_np_cover_changed();
    if (g_style == 4 && screen_current() == SCR_SAVER) vinyl_bind();
}

/* UI thread: visible consumers coalesce requests for the exact current source. */
void sharp_cover_request(void)
{
    if (!g_vpath[0] || g_vawait_identity || !g_vpicture.length ||
        !sharp_retained() || g_vbuf_valid || g_vasked_gen == g_vgen) return;
    if (!g_vpoll) g_vpoll = lv_timer_create(vinyl_poll, 150, NULL);
    if (!g_vpoll) { if (++g_vfails >= VIN_RETRIES) g_vasked_gen = g_vgen; return; }
    lv_timer_pause(g_vpoll);
    pthread_mutex_lock(&g_vmu);
    if (!g_vwork_started) {
        pthread_t th;
        if (pthread_create(&th, NULL, vinyl_worker, NULL) == 0) { pthread_detach(th); g_vwork_started = 1; }
    }
    if (g_vwork_started) {
        snprintf(g_vwant_path, sizeof g_vwant_path, "%s", g_vpath);
        g_vwant_gen = g_vgen; g_vwant = 1;
        g_vwant_deadline = vin_now_ms() + 15000;
        pthread_cond_signal(&g_vcv);
        g_vasked_gen = g_vgen;
        g_vpending = 1;
        /* Completion notification readies the paused UI timer. */
    } else if (++g_vfails >= VIN_RETRIES) g_vasked_gen = g_vgen;
    pthread_mutex_unlock(&g_vmu);
}

static void vinyl_load_sharp_cover(void)
{
    if (g_vinyl && g_have_track) { sharp_cover_request(); vinyl_bind(); }
    vinyl_update_vis();
}

#define CX 180
#define CY 180
static int default_saver(void){ return !strcmp(theme_kit()->id, "default"); }
static int something_saver(void){ return !strcmp(theme_kit()->id, "something"); }
static int hifi_saver(void){ return !strcmp(theme_kit()->id, "hi-fi"); }
static int term_saver(void){ return !strcmp(theme_kit()->id, "terminal"); }
static int bauh_saver(void){ return !strcmp(theme_kit()->id, "bauhaus"); }
static int stone_saver(void){ return !strcmp(theme_kit()->id, "stone"); }
static int zine_saver(void){ return !strcmp(theme_kit()->id, "zine"); }
static int analog_y(void){ return default_saver() || something_saver() || hifi_saver() || term_saver() || bauh_saver() || blue_saver() || stone_saver() || zine_saver() ? 138 : CY; }

static lv_obj_t *mk(lv_obj_t *p, const lv_font_t *font, lv_color_t c, int y)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_width(l, 320);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *mk_hand(lv_obj_t *p, int w, lv_color_t c, lv_point_precise_t *pts)
{
    lv_obj_t *ln = lv_line_create(p);
    lv_obj_set_style_line_width(ln, w, 0);
    lv_obj_set_style_line_color(ln, c, 0);
    lv_obj_set_style_line_rounded(ln, true, 0);
    pts[0].x = CX; pts[0].y = analog_y(); pts[1].x = CX; pts[1].y = analog_y() - 1;
    lv_line_set_points(ln, pts, 2);
    return ln;
}

static void set_hand(lv_obj_t *ln, lv_point_precise_t *pts, double deg, int len)
{
    double r = deg * 3.14159265 / 180.0;
    int y = analog_y();
    pts[0].x = CX; pts[0].y = y;
    pts[1].x = (lv_value_precise_t)(CX + len * sin(r));
    pts[1].y = (lv_value_precise_t)(y - len * cos(r));
    lv_line_set_points(ln, pts, 2);
    lv_obj_invalidate(ln);
}

/* 1s tick: animate the analog hands (incl. seconds) only while the analog saver
 * is actually on screen - cheap no-op otherwise (battery). */
/* one length for both the 1 s hand animation and the clock update, so the hand never changes length between them */
static int min_hand_len(void)
{
    if(hifi_saver()) return 68;
    return default_saver() || something_saver() || term_saver() || bauh_saver() || blue_saver() || stone_saver()
        || zine_saver() ? 80 : 104;
}
static void analog_sync(void)
{
    if (g_style != 1) return;
    time_t now = time(NULL); struct tm lt; localtime_r(&now, &lt);
    int def = default_saver();
    set_hand(g_hour, g_hpts, (lt.tm_hour % 12) * 30.0 + lt.tm_min * 0.5, hifi_saver() ? 60 : 70);
    set_hand(g_min,  g_mpts, lt.tm_min * 6.0, min_hand_len());
    set_hand(g_sec,  g_spts, lt.tm_sec * 6.0, hifi_saver() ? 68 : def || something_saver() || term_saver() || bauh_saver() || blue_saver() || stone_saver() || zine_saver() ? 83 : 118);
}

static void saver_anim_cb(lv_timer_t *t)
{
    (void)t;
    if(screen_current() == SCR_SAVER) analog_sync();
}

/* vinyl spin - idempotent; main.c calls it each loop with the live condition
 * (vinyl style && saver visible && backlight on). Freezes on stop. */
/* Throttle the actual rotation to ~20 Hz: on this GPU-less SoC a full-bleed rotate
 * is close to a full-screen software transform, so applying it every LVGL frame
 * (~30 Hz) is wasteful. Skipping a callback causes NO invalidation, so capping to
 * 50 ms keeps the redraw cost down while ~2 degrees/step stays smooth for a slow spin. */
static void vspin_cb(void *var, int32_t v){
    static uint32_t last = 0;
    uint32_t now = lv_tick_get();
    /* 30 fps. With diskOS's own rotation (vinyl_rotate) a frame costs ~7 ms, so the loop would otherwise run ~60 fps
     * and spend the saving on frames nobody needs: at 27 s/rev, 30 fps is under half a degree per frame (smooth), and
     * the cap sits far above the render time, so the cadence stays even. Device: ~58% CPU (old LVGL transform) ->
     * ~44% uncapped (measured with /proc stat ticks over 10 s); the cap avoids extra frames. */
    if (now - last < 33) return;
    last = now;
    g_vangle = v % 3600;
    if (g_vout && lv_image_get_src((lv_obj_t *)var) == (const void *)&g_vodsc) vinyl_rotate(g_vangle);
    else lv_image_set_rotation((lv_obj_t *)var, g_vangle);
}
void saver_vinyl_spin(int want)
{
    want = want && g_style == 4 && g_vinyl &&
           !lv_obj_has_flag(g_vinyl, LV_OBJ_FLAG_HIDDEN);   /* never spin a hidden/no-cover image */
    if (want == g_vspin) return;
    g_vspin = want;
    if (want) {
        int32_t cur = g_vout ? g_vangle : lv_image_get_rotation(g_vinyl);
        lv_anim_t a; lv_anim_init(&a);
        lv_anim_set_var(&a, g_vinyl);
        lv_anim_set_exec_cb(&a, vspin_cb);
        lv_anim_set_values(&a, cur, cur + 3600);
        lv_anim_set_time(&a, 27000);                /* ~27s/rev - slow, calm screensaver spin */
        lv_anim_set_path_cb(&a, lv_anim_path_linear);   /* constant angular speed */
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    } else {
        lv_anim_delete(g_vinyl, vspin_cb);
    }
}

/* show/hide + position elements for a style */
static void song_visibility(void){
    if(cu_on()) return;   /* Custom keeps its own container: the built-in song labels stay hidden */
    int hifi = hifi_saver();
    int show = default_saver() || something_saver() || hifi || term_saver() || bauh_saver() || blue_saver() || stone_saver() || zine_saver()
               ? g_playing && g_have_track : cfg_get_int("saver_style", 0) == 0;
    /* Vinyl is the cover alone (owner 2026-10-07: no song label on the spinning saver); with no cover to spin the
     * label stays, so the saver never goes blank */
    if (g_style == 4 && g_vinyl && !lv_obj_has_flag(g_vinyl, LV_OBJ_FLAG_HIDDEN)) show = 0;
    if(g_track){ if(show) lv_obj_remove_flag(g_track, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(g_track, LV_OBJ_FLAG_HIDDEN); }
    if(g_artist){ if(show) lv_obj_remove_flag(g_artist, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(g_artist, LV_OBJ_FLAG_HIDDEN); }
    if(zine_saver()){
        extern void kit_zine_song_visibility(int show);
        kit_zine_song_visibility(show);
    }
    if((hifi || bauh_saver()) && g_track && lv_obj_get_parent(g_track) != lv_obj_get_parent(g_clock)){
        lv_obj_t *well = lv_obj_get_parent(g_track);
        if(show) lv_obj_remove_flag(well, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(well, LV_OBJ_FLAG_HIDDEN);
    }
}

void saver_set_playing(int playing){
    custom_ov_feed_playing(playing);   /* the Custom overlay's pause marker (no-op without a group) */
    if(g_playing == !!playing) return;
    g_playing = !!playing;
    song_visibility();
    if(cu_on()) return;
    if(blue_saver() && screen_current() == SCR_SAVER) blue_song_layout();
    if(zine_saver() && screen_current() == SCR_SAVER && g_clock){
        saver_parts_t sp = { lv_obj_get_parent(g_clock), g_clock, g_date, g_weather, g_track, g_artist, g_face, g_style };
        theme_kit()->saver(&sp);
    }
}

static void default_song_layout(int style)
{
    int analog = style == 1;
    int song_y = analog ? 278 : 254;
    if((style == 0 || style == 3) && g_weather){
        lv_obj_update_layout(g_weather);
        int below = lv_obj_get_y(g_weather) + lv_obj_get_height(g_weather) + 12;
        if(below > song_y) song_y = below;
    }
    if(analog && g_date && !lv_obj_has_flag(g_date, LV_OBJ_FLAG_HIDDEN)){
        lv_obj_update_layout(g_date);
        int below = lv_obj_get_y(g_date) + lv_obj_get_height(g_date) + 8;
        if(below > song_y) song_y = below;
    }
    if(g_track) lv_obj_align(g_track, LV_ALIGN_TOP_MID, 0, song_y);
    if(g_artist) lv_obj_align(g_artist, LV_ALIGN_TOP_MID, 0, song_y + 24);
    int width = analog ? 200 : 230;
    if(analog && g_artist){
        lv_obj_update_layout(g_artist);
        int bottom = lv_obj_get_y(g_artist) + lv_obj_get_height(g_artist);
        int dy = bottom - 180;
        if((width / 2) * (width / 2) + dy * dy > 176 * 176){
            int fit = ui_round_width(bottom, 10);
            if(fit < width) width = fit;
        }
    }
    if(g_track) lv_obj_set_width(g_track, width);
    if(g_artist) lv_obj_set_width(g_artist, width);
}

/* The installed image the stock theme database selects, if it is usable: a regular file (no symlink on the path,
 * opened here and checked as the apply and picker code check it: size, magic, dimensions) that is a GIF or a still
 * LVGL can read. 0 = none (no Custom row, a system style selected, missing or corrupt file). */
enum { CU_GIF, CU_PNG, CU_OTHER };
#define CU_BASE_RGB 0x000000u
static int cu_asset(char *path, size_t cap, int *kind, int saved, int *alpha)   /* saved: the custom row whatever style is active (the Preview), else the active one */
{
    custom_saver_asset a;
    if((saved ? custom_saver_db_saved(NULL, &a) : custom_saver_db_read(NULL, &a)) != CUSTOM_SAVER_OK) return 0;
    int fd = open(a.path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if(fd < 0) return 0;
    struct stat sb;
    unsigned char m[4];
    int ok = 0;
    if(!fstat(fd, &sb) && S_ISREG(sb.st_mode) && pread(fd, m, sizeof m, 0) == (ssize_t)sizeof m){
        *kind = !memcmp(m, "GIF", 3) ? CU_GIF : !memcmp(m, "\x89PNG", 4) ? CU_PNG : CU_OTHER;
        ok = (*kind == CU_GIF ? custom_gif_check_fd(fd, sb.st_size) : custom_still_check_fd(fd, sb.st_size)) == CUSTOM_GIF_OK;
    }
    close(fd);
    if(ok && strlen(a.path) >= cap) ok = 0;
    if(ok){ snprintf(path, cap, "%s", a.path); *alpha = a.alpha; }
    return ok;
}

static void cu_free_still(cu_t *c)
{
    if(c->still[0]){ lv_image_cache_drop(c->still); c->still[0] = 0; }
    if(c->px){ lv_image_cache_drop(&c->dsc); custom_still_free(c->px); c->px = NULL; }
}

/* Delete everything Custom built, in the order the overlay feed requires: detach the feed, destroy the overlay group,
 * then the container (the GIF object goes with it), then release the weather hold exactly once. Safe to call twice. */
static void cu_stop(cu_t *c)
{
    if(c->box) custom_ov_feed_attach(NULL, c->lang, NULL);
    if(c->grp){ custom_ov_group_destroy(c->grp); c->grp = NULL; }
    if(c->box){ lv_obj_delete(c->box); c->box = c->img = NULL; }
    cu_free_still(c);
    if(c->held){ c->held = 0; weather_hold(0); }
}

/* Build the container: black base, the image centred, the overlay group from the saved model on top. The first GIF
 * frame is decoded here; further frames come from the GIF's own 10 ms timer (no timer of ours). 0 = nothing usable,
 * nothing left behind. */
static int cu_start(cu_t *c, lv_obj_t *parent, const custom_ov_t *model, int saved)
{
    char path[1024], src[1100];
    int kind = CU_GIF, alpha = 100;
    if(!parent || c->box || !cu_asset(path, sizeof path, &kind, saved, &alpha)) return 0;
    snprintf(src, sizeof src, "A:%s", path);
    lv_obj_t *box = lv_obj_create(parent);
    kit_isolate(box);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 360, 360);
    lv_obj_set_pos(box, 0, 0);
    lv_obj_set_style_bg_color(box, theme_color_from_rgb(CU_BASE_RGB), 0);   /* black whatever the theme: what a smaller image leaves bare */
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *img = NULL;
    int ok = 0, w = 0, h = 0;
    if(kind == CU_GIF){
        img = lv_gif_create(box);
        lv_gif_set_src(img, src);
        ok = lv_gif_is_loaded(img) && lv_gif_get_status(img) > 0;   /* opened AND its first frame decoded */
    } else if(kind == CU_PNG){
        c->px = custom_still_load(path, &w, &h);
        if(c->px){
            c->dsc = (lv_image_dsc_t){ .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .w = (uint32_t)w,
                                                     .h = (uint32_t)h, .stride = (uint32_t)w * 4 }, .data_size = (uint32_t)w * (uint32_t)h * 4,
                                         .data = c->px };
            img = lv_image_create(box);
            lv_image_set_src(img, &c->dsc);
            ok = 1;
        }
    } else {
        lv_image_header_t hd;
        if(lv_image_decoder_get_info(src, &hd) == LV_RESULT_OK && hd.w > 0 && hd.h > 0){
            w = hd.w; h = hd.h;
            img = lv_image_create(box);
            lv_image_set_src(img, src);
            snprintf(c->still, sizeof c->still, "%s", src);
            ok = 1;
        }
    }
    if(!ok){ lv_obj_delete(box); cu_free_still(c); return 0; }
    if(kind != CU_GIF && (w > 360 || h > 360)) lv_image_set_scale(img, 256 * 360 / (w > h ? w : h));   /* a larger still is shown whole */
    lv_obj_set_style_image_opa(img, (lv_opa_t)(alpha * LV_OPA_COVER / 100), 0);
    lv_obj_center(img);
    c->box = box; c->img = img;
    c->lang = i18n_lang();
    custom_ov_data_t d;
    custom_ov_feed_current(c->lang, &d);
    c->grp = custom_ov_group_create(box, model, c->lang, &d);
    if(c->grp) custom_ov_feed_attach(c->grp, c->lang, &d);
    weather_hold(1);   /* no weather request starts while Custom is visible (slice 10); released by cu_stop */
    c->held = 1;
    return 1;
}

/* The settings Preview (slice 17d) draws the REAL Custom composition with the draft as the model: same builder, own state, so the lock
 * screen's container, feed and hold are never shared. Refused while the lock screen's Custom is up or the panel is dark. */
int saver_custom_preview_start(lv_obj_t *parent)
{
    custom_ov_t draft = custom_ov_draft_get();   /* never custom_ov_get: Preview shows what Save would store */
    if(!parent || cu_on() || !ui_screen_backlight_on()) return 0;
    int ok = cu_start(&g_cp, parent, &draft, 1);
    if(!ok && !g_cp.held){
        weather_hold(1);   /* the no-image Preview also blocks weather starts */
        g_cp.held = 1;
    }
    return ok;
}
void saver_custom_preview_stop(void){ cu_stop(&g_cp); }
int saver_custom_preview_on(void){ return g_cp.box != NULL; }

/* The style to lay out for the configured one. Custom builds its container when the lit saver is on screen and the
 * image is usable, else it falls back to Minimal (one message on the next wake); hidden or dark builds nothing.
 * Any other style deletes the Custom container first. */
static int saver_resolve_style(int want)
{
    g_cfg_style = want;
    if(want != SAVER_STYLE_CUSTOM){ cu_stop(&g_cu); return want; }
    if(cu_on()) return SAVER_STYLE_CUSTOM;
    if(screen_current() != SCR_SAVER || !ui_screen_backlight_on()) return SAVER_STYLE_FALLBACK;
    custom_ov_t model = custom_ov_get(NULL);
    if(cu_start(&g_cu, g_root, &model, 0)) return SAVER_STYLE_CUSTOM;
    g_cu_msg = 1;
    return SAVER_STYLE_FALLBACK;
}

static void relayout(int style)
{
    if(style == SAVER_STYLE_CUSTOM){
        /* Custom is its own container: hide the built-in parts and run no theme hook over them. Leaving Custom runs
         * the ordinary path below again, which shows exactly what each of the five styles needs. */
        lv_obj_t *parts[] = { g_bg, g_vinyl, g_face, g_hour, g_min, g_sec, g_hub, g_clock, g_mer, g_date, g_weather, g_track, g_artist };
        for(unsigned i = 0; i < sizeof parts / sizeof parts[0]; i++) if(parts[i]) lv_obj_add_flag(parts[i], LV_OBJ_FLAG_HIDDEN);
        for(int i = 0; i < 12; i++) if(g_tick[i]) lv_obj_add_flag(g_tick[i], LV_OBJ_FLAG_HIDDEN);
        g_song_dirty = 1;
        return;
    }
    int analog = (style == 1);
    int cover  = (style == 0);
    int minim  = (style == 2);
    int vinyl  = (style == 4);
    /* a unique theme's clock + date block sits on the round screen's optical centre (Default keeps its layout) */
    int theme_dy = (theme_def() && style == 3) ? 24 : 0;   /* Cover keeps its stack: with a track it is centred */

    /* backdrop only in Cover AND only when a track is loaded (never reveal a stale cover on an
     * idle player when the style changes or the saver is entered manually) */
    if (g_bg) { if (cover && g_have_track) lv_obj_remove_flag(g_bg, LV_OBJ_FLAG_HIDDEN);
                else                        lv_obj_add_flag(g_bg, LV_OBJ_FLAG_HIDDEN); }

    /* vinyl: just the spinning square cover (shown only in vinyl style + when a cover
     * has been decoded; saver_show_sync/saver_set_track drive the decode + visibility) */
    (void)vinyl;
    if (!sharp_retained()) {
        /* Release pixels without forgetting the resolved source identity. */
        sharp_supersede();
        sharp_pixels_clear();
    }
    else if (style != 4 && g_vout) {
        if (g_vinyl) lv_image_set_src(g_vinyl, NULL);
        g_vbound = 0; lv_image_cache_drop(&g_vodsc);
        free(g_vout); g_vout = NULL; g_vodsc.data = NULL;
    }
    vinyl_update_vis();

    /* analog parts */
    lv_obj_t *aparts[] = { g_face, g_hour, g_min, g_sec, g_hub };
    for (unsigned i = 0; i < sizeof(aparts)/sizeof(aparts[0]); i++)
        if (aparts[i]) { if (analog) lv_obj_remove_flag(aparts[i], LV_OBJ_FLAG_HIDDEN);
                         else        lv_obj_add_flag(aparts[i], LV_OBJ_FLAG_HIDDEN); }
    for (int i = 0; i < 12; i++)
        if (g_tick[i]) { if (analog) lv_obj_remove_flag(g_tick[i], LV_OBJ_FLAG_HIDDEN);
                         else         lv_obj_add_flag(g_tick[i], LV_OBJ_FLAG_HIDDEN); }

    /* digital clock: shown in every style except analog + vinyl */
    if (g_clock) {
        if (analog || vinyl) lv_obj_add_flag(g_clock, LV_OBJ_FLAG_HIDDEN);
        else { lv_obj_remove_flag(g_clock, LV_OBJ_FLAG_HIDDEN);
               lv_obj_align(g_clock, LV_ALIGN_TOP_MID, 0, minim ? 150 : 110 + theme_dy); }
        saver_clock_fit();   /* a style without a clock (Analog, Vinyl) must hide the AM/PM beside it too, now */
    }
    /* date: every style except vinyl, position varies */
    if (g_date) {
        if (vinyl) lv_obj_add_flag(g_date, LV_OBJ_FLAG_HIDDEN);
        else { lv_obj_remove_flag(g_date, LV_OBJ_FLAG_HIDDEN);
               /* under the clock with room for the theme's clock face (Default's 40 px face: 170 / 206 as before) */
               int lh = lv_font_get_line_height(TF(SAVER_CLOCK));
               int below = minim ? 56 : 60; if (lh + 12 > below) below = lh + 12;
               lv_obj_align(g_date, LV_ALIGN_TOP_MID, 0, analog ? 250 : (minim ? 150 : 110 + theme_dy) + below); }
    }
    /* weather: Cover + Digital, 30 px under the date (Default: y 200 as before) */
    if (g_weather) { if (cover || style == 3){ lv_obj_remove_flag(g_weather, LV_OBJ_FLAG_HIDDEN);
                         if (g_date){ lv_obj_update_layout(g_date); lv_obj_align(g_weather, LV_ALIGN_TOP_MID, 0, lv_obj_get_y(g_date) + 30); } }
                     else lv_obj_add_flag(g_weather, LV_OBJ_FLAG_HIDDEN); }
    if (theme_kit()->saver) {                         /* the active theme's touches on this style */
        saver_parts_t sp = { lv_obj_get_parent(g_clock), g_clock, g_date, g_weather, g_track, g_artist, g_face, style };
        theme_kit()->saver(&sp);
    }
    if(default_saver()) default_song_layout(style);
    song_visibility();
    g_song_dirty = 1;   /* the tick's layout runs again, as it always followed a relayout */
}

void saver_create(lv_obj_t *root)
{
    g_root = root;
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    g_bg = lv_image_create(root);
    lv_obj_set_size(g_bg, 360, 360);
    lv_obj_align(g_bg, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_image_recolor(g_bg, TC(IMAGE_TINT), 0);
    lv_obj_set_style_image_recolor_opa(g_bg, 185, 0);
    lv_obj_add_flag(g_bg, LV_OBJ_FLAG_HIDDEN);

    /* analog face: 12 hour ticks around a circle + hands + hub */
    for (int i = 0; i < 12; i++) {
        lv_obj_t *d = lv_obj_create(root);
        lv_obj_remove_style_all(d);
        int big = (i % 3 == 0);
        lv_obj_set_size(d, big ? 8 : 5, big ? 8 : 5);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, (big ? TC(TEXT_PRIMARY) : TC(TEXT_MUTED)), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        double r = i * 30 * 3.14159265 / 180.0;
        int radius = stone_saver() || zine_saver() ? 99 : default_saver() || something_saver() || hifi_saver() || term_saver() || bauh_saver() || blue_saver() ? 105 : 150;
        int x = (int)(CX + radius * sin(r)), y = (int)(analog_y() - radius * cos(r));
        lv_obj_set_pos(d, x - (big?4:2), y - (big?4:2));
        g_tick[i] = d;
    }
    g_hour = mk_hand(root, 6, TC(TEXT_PRIMARY), g_hpts);
    g_min  = mk_hand(root, 4, TC(TEXT_SECONDARY), g_mpts);
    g_sec  = mk_hand(root, 2, ui_current_accent(), g_spts);
    g_hub  = lv_obj_create(root);
    lv_obj_remove_style_all(g_hub);
    lv_obj_set_size(g_hub, 14, 14);
    lv_obj_set_style_radius(g_hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_hub, ui_current_accent(), 0);
    lv_obj_set_style_bg_opa(g_hub, LV_OPA_COVER, 0);
    lv_obj_set_pos(g_hub, CX - 7, analog_y() - 7);

    /* vinyl: STOCK-STYLE - the plain SQUARE album cover free-rotates; the round
     * screen cuts its corners (black shows at the corners mid-tilt). No record
     * graphic (no disc/label/spindle) - copies stock, not our earlier invention.
     * g_vdisc/g_vlabel/g_vhole are intentionally left unused (NULL). */
    g_vinyl = lv_image_create(root);
    lv_obj_center(g_vinyl);
    /* AA OFF for FASTER frames: measured render is ~19ms with AA off (~50fps) vs ~52ms
     * with AA on (~19fps). On the native-res + bright cover the nearest-neighbour edges
     * are acceptable, and the ~50fps makes the spin much smoother. */
    lv_image_set_antialias(g_vinyl, false);   /* pivot + scale set per-cover in saver_set_track */
    lv_obj_add_flag(g_vinyl, LV_OBJ_FLAG_HIDDEN);

    s_swfont = *TF(UI_16);
    s_swfont.fallback = &font_weather16;

    g_clock   = mk(root, TF(SAVER_CLOCK), TC(TEXT_PRIMARY), 110);
    g_date    = mk(root, TF(UI_16), TC(TEXT_SECONDARY), 170);
    g_weather = mk(root, &s_swfont,              TC(TEXT_SECONDARY), 200);
    int def = default_saver();
    g_track   = mk(root, def ? TF(UI_18) : TF(UI_16), TC(TEXT_PRIMARY), 254);
    g_artist  = mk(root, def ? TF(UI_16) : TF(UI_14), TC(TEXT_MUTED), 278);
    indic_label_enable(g_track);
    indic_label_enable(g_artist);
    lv_label_set_text(g_clock, "--:--");
    lv_obj_add_event_cb(g_clock, saver_clock_style_cb, LV_EVENT_STYLE_CHANGED, NULL);   /* refit after the theme's face lands */

    g_style = saver_resolve_style(cfg_get_int("saver_style", 0));   /* not on screen yet: Custom builds nothing here */
    relayout(g_style);
    g_analog_timer=lv_timer_create(saver_anim_cb, 1000, NULL); lv_timer_pause(g_analog_timer);   /* analog seconds hand */
}

/* repaint the saver's accent-bearing decorations (analog second hand + hub, vinyl
 * record label) with the live accent - called from ui.c apply_accent so they match the
 * rest of the UI instead of staying hardcoded pink. Elements are created once, so the
 * pointers are always valid. */
void saver_set_accent(lv_color_t c)
{
    if (g_sec)    lv_obj_set_style_line_color(g_sec, c, 0);
    if (g_hub)    lv_obj_set_style_bg_color(g_hub, c, 0);
}

/* One fresh pose per opening/wake, including paused playback and a cover arriving later. */
static void vinyl_random_pose(void)
{
    saver_vinyl_spin(0);   /* an old animation must not overwrite the new starting angle */
    g_vangle = rand() % 3600;   /* main.c seeds the existing RNG once at startup */
    if(g_vout && g_vbuf_valid){
        vinyl_rotate(g_vangle);
        if(g_vinyl) lv_obj_invalidate(g_vinyl);
    } else {
        g_vodsc.data = NULL;   /* retained pixels must be rotated at the new pose when their identity returns */
        if(g_vinyl) lv_image_set_rotation(g_vinyl, g_vangle);
    }
    g_vrandom_on_wake = 0;
}

/* apply the saved style + current accent immediately when the saver is shown, rather
 * than waiting for the next clock tick (which is where style changes were applied). */
void saver_show_sync(void)
{
    int s = saver_resolve_style(cfg_get_int("saver_style", 0));   /* Custom builds its container here (or falls back) */
    g_style = s; relayout(s);   /* style first: relayout's vinyl/label visibility reads g_style */
    if(g_style == 4){
        if(ui_screen_backlight_on()) vinyl_random_pose();
        else g_vrandom_on_wake = 1;
    }
    saver_set_accent(ui_current_accent());
    analog_sync();   /* all hands are correct on entry, before the first 1 s timer */
    /* decode the sharp cover as the vinyl saver appears - but only if a track is loaded;
     * otherwise make sure the vinyl stays hidden (no stale cover on an idle player). */
    if (g_style == 4) { if (g_have_track) vinyl_load_sharp_cover(); else vinyl_update_vis(); }
}

/* The one deferred message of a Custom fallback, once the panel is lit (a dark wake or the leave of a lit saver). */
void saver_wake(void)
{
    if(g_vrandom_on_wake && g_style == 4 && screen_current() == SCR_SAVER && ui_screen_backlight_on())
        vinyl_random_pose();
    if(!g_cu_msg || !ui_screen_backlight_on()) return;
    g_cu_msg = 0;
    ui_toast("Custom image unavailable");
}

/* The saver is being left (screenmgr.c transition(): wake touch, screen off, Back, any screen_show away from it).
 * Custom is deleted, not hidden, and the built-in layout of the fallback style is restored for the next entry. */
void saver_leave(void)
{
    if(cu_on()){
        cu_stop(&g_cu);
        g_style = SAVER_STYLE_FALLBACK;
        relayout(g_style);
    }
    saver_wake();
}

/* The panel went dark while the saver may still be current (main.c draw_set_dark): nothing Custom keeps running and
 * nothing is rebuilt until a lit saver entry. Built-in Vinyl keeps only a pending wake pose. */
void saver_dark(void)
{
    g_vrandom_on_wake = 1;
    custom_preview_dark();   /* the settings Preview (slice 17d) stops the same way, and rebuilds on the first lit draw */
    if(!cu_on()) return;
    cu_stop(&g_cu);
    g_style = SAVER_STYLE_FALLBACK;
    relayout(g_style);
}

/* The time as set (a dotted label edits its own copy, so the fit always starts from this). */
static char g_clock_text[32] = "--:--";
/* Fit the whole time in the saver: a big theme face can make "12:59 PM" wider than the 320 px box, and the label would
 * end in "..." (bauhaus showed "5:53..."). Then the label takes the text's natural width (one line; its alignment keeps
 * it centred) and is drawn scaled down about its centre. Runs on every time change AND whenever the label's style
 * changes - the theme's styling pass sets the final (bigger) face after the first time is set. */
static void saver_clock_fit(void)
{
    if (!g_clock) return;
    char digits[32]; snprintf(digits, sizeof digits, "%s", g_clock_text);
    const char *mer = "";
    char *sp = strrchr(digits, ' ');
    if (theme_kit()->saver_mer_apart && sp && (!strcmp(sp + 1, "AM") || !strcmp(sp + 1, "PM"))) { *sp = 0; mer = g_clock_text + (sp - digits) + 1; }
    lv_label_set_text(g_clock, digits);
    lv_point_t sz;
    lv_text_get_size(&sz, digits, lv_obj_get_style_text_font(g_clock, 0),
                     lv_obj_get_style_text_letter_space(g_clock, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t room = 320 - 4;
    int32_t sc = (sz.x > room && sz.x > 0) ? (int32_t)(256L * room / sz.x) : 256;
    lv_obj_set_width(g_clock, sc < 256 ? sz.x + 4 : 320);
    lv_obj_update_layout(g_clock);
    lv_obj_set_style_transform_pivot_x(g_clock, lv_obj_get_width(g_clock) / 2, 0);
    lv_obj_set_style_transform_pivot_y(g_clock, lv_obj_get_height(g_clock) / 2, 0);
    lv_obj_set_style_transform_scale(g_clock, sc, 0);
    /* AM/PM beside the digits' foot, the pair centred as one group */
    if (mer[0] && !g_mer) {
        g_mer = lv_label_create(lv_obj_get_parent(g_clock));
        kit_keep(g_mer);
        lv_obj_set_style_text_font(g_mer, theme_kit()->saver_mer_apart == 2 ? TF(DATE) : TF(UI_14), 0);
    }
    if (g_mer) {
        int show = mer[0] && !lv_obj_has_flag(g_clock, LV_OBJ_FLAG_HIDDEN);
        if (!show) { lv_obj_add_flag(g_mer, LV_OBJ_FLAG_HIDDEN); lv_obj_set_style_translate_x(g_clock, 0, 0); }
        else {
            lv_label_set_text(g_mer, mer);
            lv_obj_set_style_text_color(g_mer, lv_obj_get_style_text_color(g_clock, 0), 0);
            lv_obj_remove_flag(g_mer, LV_OBJ_FLAG_HIDDEN);
            lv_obj_update_layout(g_mer);
            int32_t mw = lv_obj_get_width(g_mer) + 6;
            lv_obj_set_width(g_clock, sz.x + 4);                  /* the digits' own width, so AM/PM sits right beside them */
            lv_obj_set_style_translate_x(g_clock, -mw / 2, 0);   /* centre digits + AM/PM together */
            lv_obj_update_layout(g_clock);
            lv_obj_align_to(g_mer, g_clock, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -lv_obj_get_height(g_clock) / 5);   /* coords include the translate */
        }
    }
}
static void saver_clock_style_cb(lv_event_t *e)
{
    (void)e;
    static int busy;                     /* the fit sets styles itself: don't recurse */
    if (busy) return;
    busy = 1; saver_clock_fit(); busy = 0;
}

/* What the tick's layout reads besides its texts, as requested (styles and flags): a width a theme sets after its last
 * layout pass is already here, while coordinates still show the old one until the next refresh. The song block's
 * parent is a theme's well or panel. */
#define SONG_GEOM_N 17
static void song_geom(intptr_t g[6][SONG_GEOM_N])
{
    lv_obj_t *o[6] = { g_clock, g_date, g_weather, g_track, g_artist, g_track ? lv_obj_get_parent(g_track) : NULL };
    memset(g, 0, sizeof(intptr_t) * 6 * SONG_GEOM_N);
    for (int i = 0; i < 6; i++) {
        if (!o[i]) continue;
        intptr_t *v = g[i];
        v[0] = (intptr_t)lv_obj_get_parent(o[i]); v[1] = lv_obj_has_flag(o[i], LV_OBJ_FLAG_HIDDEN);
        v[14] = (intptr_t)lv_obj_get_style_text_font(o[i], 0);
        v[15] = lv_obj_get_style_text_letter_space(o[i], 0); v[16] = lv_obj_get_style_text_line_space(o[i], 0);
        if (o[i] == g_clock) continue;   /* its box and scale come from the time alone (saver_clock_fit) */
        v[2] = lv_obj_get_style_x(o[i], 0); v[3] = lv_obj_get_style_y(o[i], 0);
        v[4] = lv_obj_get_style_width(o[i], 0); v[5] = lv_obj_get_style_height(o[i], 0);
        v[6] = lv_obj_get_style_align(o[i], 0);
        v[7] = lv_obj_get_style_pad_left(o[i], 0); v[8] = lv_obj_get_style_pad_right(o[i], 0);
        v[9] = lv_obj_get_style_pad_top(o[i], 0); v[10] = lv_obj_get_style_pad_bottom(o[i], 0);
        v[11] = lv_obj_get_style_border_width(o[i], 0);
        v[12] = lv_obj_get_style_translate_x(o[i], 0); v[13] = lv_obj_get_style_translate_y(o[i], 0);
    }
}

void saver_set_clock(const char *t, const char *date)
{
    /* re-apply layout if the style setting changed */
    int s = g_style;
    if (cfg_get_int("saver_style", 0) != g_cfg_style) { s = g_style = saver_resolve_style(cfg_get_int("saver_style", 0)); relayout(s); }
    if (cu_on()) { custom_ov_feed_tick(); return; }   /* Custom: its own container, no built-in label, theme hook or fit runs */

    /* The layout below reads what it last set (a theme's song widths, the date's place), so it ran on every 10 s tick to
     * settle. It runs when a text changed, and again while a run moves what it reads; once a run leaves that as it found
     * it, the same inputs give the same layout and an unchanged tick skips it. */
    static int settled;
    static intptr_t after[6][SONG_GEOM_N];
    const char *tt = t ? t : "--:--", *dd = date ? date : "";
    int time_new = strcmp(g_clock_text, tt) != 0;
    int date_new = ui_label_input_changed(g_date, dd);
    intptr_t before[6][SONG_GEOM_N];
    song_geom(before);
    if (time_new || date_new || g_song_dirty || !settled || memcmp(before, after, sizeof before)) {
        if (g_clock && time_new) {
            snprintf(g_clock_text, sizeof g_clock_text, "%s", tt);
            saver_clock_fit();
        }
        if (g_date && date_new) theme_case_text(g_date, dd);   /* the theme's case, as on Home (was always "Thu 14 Nov") */
        if(blue_saver()) blue_song_layout();
        if (time_new && theme_kit()->saver_clock) theme_kit()->saver_clock(tt);
        if ((bauh_saver() || hifi_saver() || something_saver() || term_saver() || stone_saver() || zine_saver()) && theme_kit()->saver) {
            saver_parts_t sp = { lv_obj_get_parent(g_clock), g_clock, g_date, g_weather, g_track, g_artist, g_face, s };
            theme_kit()->saver(&sp);   /* the date now has its final text height */
        }
        if(default_saver()) default_song_layout(s);
        indic_stack_fit(g_track, g_artist, NULL);
        g_song_dirty = 0;
        song_geom(after);
        settled = !memcmp(before, after, sizeof before);
    }

    /* analog hands from the live time */
    analog_sync();

    custom_ov_feed_tick();   /* the existing clock tick drives the Custom overlay's time, date and weather (no-op without a group) */
}

void saver_set_weather(const char *text)
{
    if (ui_label_set_changed(g_weather, text ? text : "")) g_song_dirty = 1;
    if(g_cp.box) custom_ov_feed_tick();
}

void saver_set_track(const char *title, const char *artist, const void *backdrop_src, const char *path)
{
    /* The caller passes a non-NULL title iff st.have_track (NULL when no track), so key off
     * NULL-ness, not emptiness - a valid but untitled file still counts as a loaded track. */
    custom_ov_feed_track(title, artist);   /* the Custom overlay's title and artist (no-op without a group) */
    g_have_track = (title != NULL);
    const char *p = (g_have_track && path) ? path : "";
    sharp_cover_set_source(p);
    /* the original input: a dotted label edits its own copy */
    int changed = ui_label_set_changed(g_track, title ? title : "");
    changed |= ui_label_set_changed(g_artist, artist ? artist : "");
    if(changed) { g_song_dirty = 1; indic_stack_fit(g_track, g_artist, NULL); }
    song_visibility();
    if(!cu_on() && blue_saver() && screen_current() == SCR_SAVER) blue_song_layout();
    if(!cu_on() && zine_saver() && screen_current() == SCR_SAVER && g_clock){
        saver_parts_t sp = { lv_obj_get_parent(g_clock), g_clock, g_date, g_weather, g_track, g_artist, g_face, g_style };
        theme_kit()->saver(&sp);
    }

    /* vinyl cover: re-decode the sharp 360px cover only when the vinyl saver is
     * actually on-screen (track auto-advanced while idle); otherwise just refresh
     * visibility - the decode runs on the next saver show. Never run the decoder for a
     * track change while NOT showing the vinyl saver (would hitch the live UI). */
    if (g_vinyl) {
        if (g_style == 4 && screen_current() == SCR_SAVER) vinyl_load_sharp_cover();
        else                                               vinyl_update_vis();
    }

    if (g_bg) {
        if (g_have_track && backdrop_src) {
            lv_image_set_src(g_bg, backdrop_src);
            if (g_style == 0) lv_obj_remove_flag(g_bg, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_background(g_bg);
        } else {
            lv_obj_add_flag(g_bg, LV_OBJ_FLAG_HIDDEN);   /* no track -> no backdrop art */
        }
    }
}

void sharp_cover_deliver(void){if(g_vpoll)vinyl_poll(g_vpoll);}

void saver_poll_sync(int lit){
    if(!g_analog_timer)return;
    if(lit && screen_current()==SCR_SAVER && g_style==1)lv_timer_resume(g_analog_timer);
    else lv_timer_pause(g_analog_timer);
}
