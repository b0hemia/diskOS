/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fontft.h"
#include "fontft_internal.h"
#include "i18n.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#define TTC_PATH "/usr/project/config/fonts/SourceHanSans-Regular.ttc"
#define CACHE_LIMIT (1024u * 1024u - sizeof buckets - sizeof charmap)
#define ENTRY_LIMIT 4096u

typedef fontft_glyph glyph_t;

static FT_Library library;
static FT_Face face;
/* Indexed by pixel size; objects are initialized only when a role requests them. */
static lv_font_t fonts[141];
static glyph_t *newest, *oldest;
static glyph_t *buckets[256];
/* Pixel-independent nominal cmap cache; included in the shared byte budget. */
static struct { uint32_t cp, index; } charmap[4096];
static size_t used_bytes;
static unsigned entries, raster_count;
static pthread_t lv_thread;
static int initialized, enabled;

static unsigned bucket_for(FT_Face f, uint32_t id, uint8_t px, int32_t flags)
{
    return ((uintptr_t)f ^ id * 2654435761u ^ px * 97u ^ (uint32_t)flags) & 255u;
}

static int on_lv_thread(void)
{
    /* LVGL and the FreeType face are owned by the thread that calls init. Another thread gets "not found"
     * (the chain continues) rather than an abort: a crash here would take the whole UI down. */
    return pthread_equal(lv_thread, pthread_self());
}

static void unlink_glyph(glyph_t *g)
{
    if(g->prev) g->prev->next = g->next;
    else newest = g->next;
    if(g->next) g->next->prev = g->prev;
    else oldest = g->prev;
    g->prev = g->next = NULL;
}

static void promote(glyph_t *g)
{
    if(g == newest) return;
    if(g->prev || g->next || g == oldest) unlink_glyph(g);
    g->next = newest;
    if(newest) newest->prev = g;
    newest = g;
    if(!oldest) oldest = g;
}

static glyph_t *lookup(FT_Face f, uint32_t id, uint8_t px, int32_t flags)
{
    for(glyph_t *g = buckets[bucket_for(f, id, px, flags)]; g; g = g->hnext) {
        if(g->face == f && g->index == id && g->px == px && g->flags == flags) {
            promote(g);
            return g;
        }
    }
    return NULL;
}

static void evict_oldest(void)
{
    glyph_t *victim = oldest;
    if(!victim) return;
    unlink_glyph(victim);
    glyph_t **link = &buckets[bucket_for(victim->face, victim->index, victim->px, victim->flags)];
    while(*link != victim) link = &(*link)->hnext;
    *link = victim->hnext;
    used_bytes -= victim->bytes;
    entries--;
    free(victim);
}

static glyph_t *rasterize(FT_Face f, uint32_t index, uint8_t px, int32_t flags)
{
    if(FT_Set_Pixel_Sizes(f, 0, px)) return NULL;
    raster_count++;
    if(!index || FT_Load_Glyph(f, index, flags))
        return NULL;
    FT_GlyphSlot slot = f->glyph;
    FT_Bitmap *bm = &slot->bitmap;
    if(((bm->width || bm->rows) && bm->pixel_mode != FT_PIXEL_MODE_GRAY) ||
       bm->width > 256 || bm->rows > 256 || bm->pitch < (int)bm->width ||
       (bm->rows && !bm->buffer)) return NULL;
    size_t pixels = (size_t)bm->width * bm->rows;
    size_t bytes = sizeof(glyph_t) + pixels;
    if(bytes > CACHE_LIMIT) return NULL;
    int32_t advance = slot->advance.x >> 6;
    if(advance < 0 || advance > UINT16_MAX) return NULL;
    while(oldest && (used_bytes + bytes > CACHE_LIMIT || entries >= ENTRY_LIMIT)) {
        evict_oldest();
    }
    glyph_t *g = malloc(bytes);
    if(!g && oldest) { evict_oldest(); g = malloc(bytes); }
    if(!g) return NULL;
    memset(g, 0, sizeof(*g));
    g->face = f; g->index = index; g->flags = flags;
    g->px = px;
    g->width = bm->width;
    g->height = bm->rows;
    g->advance = (uint16_t)advance;
    g->left = slot->bitmap_left;
    g->bottom = slot->bitmap_top - (int)bm->rows;
    g->bytes = bytes;
    for(unsigned y = 0; y < bm->rows; y++)
        memcpy(g->bitmap + y * bm->width, bm->buffer + y * bm->pitch, bm->width);
    unsigned bucket = bucket_for(f, index, px, flags);
    g->hnext = buckets[bucket];
    buckets[bucket] = g;
    promote(g);
    used_bytes += bytes;
    entries++;
    return g;
}

static bool get_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc, uint32_t cp, uint32_t next)
{
    (void)next;
    if(!enabled || !on_lv_thread()) return false;
    uint8_t px = (uint8_t)(uintptr_t)font->user_data;
    unsigned slot = (cp * 2654435761u) & 4095u;
    if(charmap[slot].cp != cp) {
        charmap[slot].cp = cp;
        charmap[slot].index = FT_Get_Char_Index(face, cp);
    }
    uint32_t index = charmap[slot].index;
    if(!index) return false;
    const glyph_t *g = fontft_raster(face, index, px, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL | FT_LOAD_NO_AUTOHINT);
    if(!g) return false;
    dsc->adv_w = g->advance;
    dsc->box_w = g->width;
    dsc->box_h = g->height;
    dsc->ofs_x = g->left;
    dsc->ofs_y = g->bottom;
    dsc->format = g->width && g->height ? LV_FONT_GLYPH_FORMAT_A8 : LV_FONT_GLYPH_FORMAT_NONE;
    dsc->is_placeholder = 0;
    dsc->gid.index = index;
    dsc->entry = NULL;
    return true;
}

static const void *get_bitmap(lv_font_glyph_dsc_t *dsc, lv_draw_buf_t *buf)
{
    if(!enabled || !on_lv_thread() || !buf || !buf->data) return NULL;
    uint8_t px = (uint8_t)(uintptr_t)dsc->resolved_font->user_data;
    const glyph_t *g = fontft_raster(face, dsc->gid.index, px, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL | FT_LOAD_NO_AUTOHINT);
    if(!g || g->width != dsc->box_w || g->height != dsc->box_h) return NULL;
    uint32_t stride = buf->header.stride;
    if(stride < g->width || (g->height && buf->data_size < (g->height - 1u) * stride + g->width))
        return NULL;
    for(unsigned y = 0; y < g->height; y++)
        memcpy((uint8_t *)buf->data + y * stride, g->bitmap + y * g->width, g->width);
    return buf;
}

void fontft_init(void)
{
    if(initialized) return;
    lv_thread = pthread_self();
    initialized = 1;
    const char *path = TTC_PATH;
#ifdef DISKOS_HOST
    const char *override = getenv("DISKOS_TTC_PATH");
    if(override) path = override;
#endif
    int lang = i18n_lang();
    FT_Long face_index = lang == LANG_KO ? 3 : lang == LANG_JA ? 2 : lang == LANG_TW ? 1 : 0;
    if(!fontft_library() || FT_New_Face(library, path, face_index, &face) ||
       face->num_faces != 4 ||
       FT_Set_Pixel_Sizes(face, 0, 14) ||
       !FT_Get_Char_Index(face, 0xADF8) ||
       FT_Load_Char(face, 0xADF8, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL | FT_LOAD_NO_AUTOHINT)) {
        if(face) FT_Done_Face(face);
        face = NULL;
        fprintf(stderr, "fontft: stock TTC unavailable; using built-in fonts\n");
        return;
    }
    enabled = 1;
}

const lv_font_t *fontft_font(int px)
{
    if(!initialized || !enabled || !on_lv_thread() || px < 10 || px > 140) return NULL;
    lv_font_t *font = &fonts[px];
    if(!font->get_glyph_dsc) {
        font->get_glyph_dsc = get_dsc;
        font->get_glyph_bitmap = get_bitmap;
        font->line_height = px + 5;
        font->base_line = 4;
        font->user_data = (void *)(uintptr_t)px;
    }
    return font;
}

FT_Library fontft_library(void)
{
    if(!initialized) { initialized = 1; lv_thread = pthread_self(); }
    if(!on_lv_thread()) return NULL;
    if(!library && FT_Init_FreeType(&library)) return NULL;
    return library;
}
int fontft_reclaim(void)
{
    if(!initialized || !on_lv_thread() || !oldest) return 0;
    evict_oldest();
    return 1;
}
const fontft_glyph *fontft_raster(FT_Face f, uint32_t index, int px, int32_t flags)
{
    if(!initialized || !on_lv_thread() || !f || px < 1 || px > 140) return NULL;
    glyph_t *g = lookup(f, index, px, flags);
    if(!g) g = rasterize(f, index, px, flags);
    return g;
}
#ifdef DISKOS_HOST
unsigned fontft_raster_count(void) { return raster_count; }
size_t fontft_cache_bytes(void) { return used_bytes + sizeof buckets + sizeof charmap; }
#endif
