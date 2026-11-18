/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cast_art.h"
#include "artcache.h"
#include "sdio.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The PNG encoder is the LodePNG that ships inside LVGL (zlib licence), compiled into this one file: LVGL itself keeps it switched off
 * (LV_USE_LODEPNG 0) and this is not LVGL's PNG decoder. Only the encoder is built: no decoder, disk, ancillary-chunk, error-text or C++ code,
 * and the allocator below is plain malloc with a ceiling instead of LVGL's, so the encode can run on any thread without LVGL. */
#include "lvgl/lvgl.h"      /* reads lv_conf.h, where LV_USE_LODEPNG is 0: that switch is for LVGL's own PNG decoder and stays off */
static void *ca_copy(void *d, const void *s, size_t n) { return n ? memcpy(d, s, n) : d; }
#define lv_memcpy ca_copy
#undef LV_USE_LODEPNG
#define LV_USE_LODEPNG 1     /* only so that lodepng.c below is not compiled out */
#define LODEPNG_NO_COMPILE_DECODER
#define LODEPNG_NO_COMPILE_DISK
#define LODEPNG_NO_COMPILE_ANCILLARY_CHUNKS
#define LODEPNG_NO_COMPILE_ERROR_TEXT
#define LODEPNG_NO_COMPILE_CPP
#define LODEPNG_NO_COMPILE_ALLOCATORS
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function" /* the colour conversion helper the decoder would use */
#include "lvgl/src/libs/lodepng/lodepng.c"
#undef lv_memcpy
#pragma GCC diagnostic pop
void *lodepng_malloc(size_t n) { return n > CA_ALLOC_MAX ? NULL : malloc(n); }
void *lodepng_realloc(void *p, size_t n) { return n > CA_ALLOC_MAX ? NULL : realloc(p, n); }
void lodepng_free(void *p) { free(p); }

/* Define wrapping arithmetic for stb's signed bit accumulator.
 * stb has no per-format switches. Static inline drops unused writers at -Os.
 * NO_STDIO disables file output and HDR; the stub completes its leftover
 * declaration so -Wall -Werror remains clean without warning suppression. */
#pragma GCC push_options
#pragma GCC optimize ("wrapv", "no-strict-aliasing")
#define STB_IMAGE_WRITE_STATIC
#define STBIWDEF static inline
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb-1ee679ca/stb_image_write.h"
static inline int stbi_write_hdr_to_func(stbi_write_func *f, void *c, int w, int h, int n, const float *p)
{
    (void)f; (void)c; (void)w; (void)h; (void)n; (void)p;
    return 0;
}
#pragma GCC pop_options
#undef STBIWDEF
#undef STB_IMAGE_WRITE_IMPLEMENTATION
#undef STBI_WRITE_NO_STDIO
#undef STB_IMAGE_WRITE_STATIC

typedef struct { unsigned char *data; size_t used; int failed; } ca_jpeg;
static void jpeg_write(void *ctx, void *data, int size)
{
    ca_jpeg *out = ctx;
    if (out->failed) return;
    if (size < 0 || (size_t)size > CA_JPEG_MAX - out->used) {
        out->failed = 1;
        return;
    }
    if (size) memcpy(out->data + out->used, data, (size_t)size);
    out->used += (size_t)size;
}
/* AirPlay receivers expect a full-size cover (Apple senders use 600x600); the
 * cache holds 148x148, so scale it up bilinearly before encoding. */
#define CA_AIRPLAY_PX 600
static unsigned char *scale_up(const unsigned char *rgb)
{
    unsigned char *big = malloc((size_t)CA_AIRPLAY_PX * CA_AIRPLAY_PX * 3);
    if (!big) return NULL;
    for (int y = 0; y < CA_AIRPLAY_PX; y++) {
        unsigned fy = (unsigned)y * (CA_PX - 1) * 256 / (CA_AIRPLAY_PX - 1), y0 = fy >> 8, wy = fy & 255;
        unsigned y1 = y0 + 1 < CA_PX ? y0 + 1 : y0;
        for (int x = 0; x < CA_AIRPLAY_PX; x++) {
            unsigned fx = (unsigned)x * (CA_PX - 1) * 256 / (CA_AIRPLAY_PX - 1), x0 = fx >> 8, wx = fx & 255;
            unsigned x1 = x0 + 1 < CA_PX ? x0 + 1 : x0;
            const unsigned char *a = rgb + ((size_t)y0 * CA_PX + x0) * 3, *b = rgb + ((size_t)y0 * CA_PX + x1) * 3;
            const unsigned char *c = rgb + ((size_t)y1 * CA_PX + x0) * 3, *d = rgb + ((size_t)y1 * CA_PX + x1) * 3;
            unsigned char *o = big + ((size_t)y * CA_AIRPLAY_PX + x) * 3;
            for (int k = 0; k < 3; k++) {
                unsigned top = a[k] * (256 - wx) + b[k] * wx, bottom = c[k] * (256 - wx) + d[k] * wx;
                o[k] = (unsigned char)((top * (256 - wy) + bottom * wy + 32768) >> 16);
            }
        }
    }
    return big;
}
static int encode_jpeg(const unsigned char *rgb, unsigned char **data, size_t *len)
{
    ca_jpeg out = {0};
    unsigned char *big = scale_up(rgb);
    int px = big ? CA_AIRPLAY_PX : CA_PX;
    if (CA_JPEG_MAX > CA_ALLOC_MAX || !(out.data = malloc(CA_JPEG_MAX))) { free(big); return -1; }
    if (!stbi_write_jpg_to_func(jpeg_write, &out, px, px, 3, big ? big : rgb, 85) ||
        out.failed || !out.used) {
        free(out.data); free(big);
        return -1;
    }
    free(big);
    *data = out.data; *len = out.used;
    return 0;
}

#define STRIDE (CA_PX * 3)            /* 444: already a multiple of 4, so a BMP row has no padding */
#define BMP_BYTES (54 + STRIDE * CA_PX)
static uint32_t le32(const unsigned char *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
/* Whole file or nothing: regular file of exactly BMP_BYTES, read under an sd_io lease. */
static unsigned char *read_bmp_fd(int fd)
{
    struct stat st; unsigned char *raw = NULL; size_t got = 0;
    if (fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == BMP_BYTES && (raw = malloc(BMP_BYTES))) {
        while (got < BMP_BYTES) { ssize_t n = pread(fd, raw + got, BMP_BYTES - got, (off_t)got); if (n <= 0) break; got += (size_t)n; }
        if (got != BMP_BYTES) { free(raw); raw = NULL; }
    }
    return raw;
}
static unsigned char *read_bmp(const char *path)
{
    if (!sd_io_begin()) return NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    unsigned char *raw = read_bmp_fd(fd);
    if (fd >= 0) close(fd);
    sd_io_end(); return raw;
}
static int encode_bmp(unsigned char *raw, unsigned char **png, size_t *len, int prefer_jpeg)
{
    unsigned char *rgb, *out = NULL; size_t n = 0; int rc = -1;
    *png = NULL; *len = 0;
    if (!raw) return -1;
    /* the layout art.c writes: BITMAPINFOHEADER, 24 bit, bottom-up, no compression, pixels at 54 */
    if (raw[0] != 'B' || raw[1] != 'M' || le32(raw + 2) != BMP_BYTES || le32(raw + 10) != 54 || le32(raw + 14) != 40 ||
        le32(raw + 18) != CA_PX || le32(raw + 22) != CA_PX || (raw[26] | raw[27] << 8) != 1 || (raw[28] | raw[29] << 8) != 24 || le32(raw + 30) != 0) {
        free(raw); return -1;
    }
    rgb = malloc((size_t)CA_PX * STRIDE);
    if (!rgb) { free(raw); return -1; }
    for (int y = 0; y < CA_PX; y++) {
        const unsigned char *s = raw + 54 + (size_t)(CA_PX - 1 - y) * STRIDE; unsigned char *d = rgb + (size_t)y * STRIDE;
        for (int x = 0; x < CA_PX; x++) { d[x * 3] = s[x * 3 + 2]; d[x * 3 + 1] = s[x * 3 + 1]; d[x * 3 + 2] = s[x * 3]; } /* BGR -> RGB */
    }
    free(raw);
    if (prefer_jpeg && !encode_jpeg(rgb, png, len)) { free(rgb); return 0; }
    if (lodepng_encode24(&out, &n, rgb, CA_PX, CA_PX) == 0 && out && n && n <= CA_PNG_MAX) { *png = out; *len = n; out = NULL; rc = 0; }
    free(out); free(rgb);
    return rc;
}

int cast_art_png(const char *bmp, unsigned char **png, size_t *len)
{
    return encode_bmp(bmp ? read_bmp(bmp) : NULL, png, len, 0);
}
int cast_art_empty_png(unsigned char **png, size_t *len)
{
    const unsigned char transparent[4] = {0};
    *png = NULL; *len = 0;
    return lodepng_encode32(png, len, transparent, 1, 1) ? -1 : 0;
}
int cast_art_png_fd(int fd, unsigned char **png, size_t *len)
{
    return encode_bmp(read_bmp_fd(fd), png, len, 0);
}

int cast_art_airplay_fd(int fd, unsigned char **data, size_t *len)
{
    return encode_bmp(read_bmp_fd(fd), data, len, 1);
}

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_track[1024]; static unsigned g_gen;      /* guarded by g_mu; g_gen 0 never names a track */
static unsigned char *g_png;
static size_t g_png_len;
static unsigned g_png_gen, g_png_cache;
static uint64_t g_png_storage;
static void bump(void) {
    if (++g_gen == 0) ++g_gen;
    free(g_png); g_png = NULL; g_png_len = 0;
}
unsigned cast_art_arm(const char *track)
{
    unsigned gen;
    if (!track || strlen(track) >= sizeof g_track) { cast_art_clear(); return 0; }
    pthread_mutex_lock(&g_mu);
    bump();
    snprintf(g_track, sizeof g_track, "%s", track); gen = g_gen;
    pthread_mutex_unlock(&g_mu);
    return gen;
}
int cast_art_fetch(unsigned gen, unsigned char **png, size_t *len)
{
    char track[sizeof g_track], cover[600];
    unsigned cache = artcache_gen(); uint64_t storage = sd_io_generation();
    *png = NULL; *len = 0;
    if(!sd_io_allowed()) return -1;
    pthread_mutex_lock(&g_mu);
    if(!gen || gen != g_gen || !g_track[0]){ pthread_mutex_unlock(&g_mu); return -1; }
    if(g_png && g_png_gen == gen && g_png_cache == cache && g_png_storage == storage &&
       cache == artcache_gen() && storage == sd_io_generation() && sd_io_allowed()){
        *png = malloc(g_png_len);
        if(*png){ memcpy(*png, g_png, g_png_len); *len = g_png_len; }
        pthread_mutex_unlock(&g_mu);
        return *png ? 0 : -1;
    }
    memcpy(track, g_track, sizeof track);
    pthread_mutex_unlock(&g_mu);
    if(artcache_cover_candidate(track, cover, sizeof cover) || cast_art_png(cover, png, len)) return -1;
    pthread_mutex_lock(&g_mu);
    int stale = gen != g_gen || cache != artcache_gen() || storage != sd_io_generation() || !sd_io_allowed();
    if(!stale){
        free(g_png); g_png = malloc(*len); g_png_len = 0;
        if(g_png){ memcpy(g_png, *png, *len); g_png_len = *len; }
        g_png_gen = gen; g_png_cache = cache; g_png_storage = storage;
    }
    pthread_mutex_unlock(&g_mu);
    if(stale){ free(*png); *png = NULL; *len = 0; return -1; }
    return 0;
}
void cast_art_clear(void)
{
    pthread_mutex_lock(&g_mu); bump(); g_track[0] = 0; pthread_mutex_unlock(&g_mu);
}
