/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FONTFT_INTERNAL_H
#define FONTFT_INTERNAL_H
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdint.h>
#include <stddef.h>
typedef struct fontft_glyph {
    struct fontft_glyph *prev, *next, *hnext;
    FT_Face face;
    uint32_t index;
    int32_t flags;
    uint8_t px;
    uint16_t width, height, advance;
    int16_t left, bottom;
    size_t bytes;
    uint8_t bitmap[];
} fontft_glyph;
FT_Library fontft_library(void);
int fontft_reclaim(void);
/* Borrowed until the next cache operation. Copy pixels before another lookup. */
const fontft_glyph *fontft_raster(FT_Face face, uint32_t index, int px, int32_t flags);
FT_Face fontft_indic_face(int script);
#ifdef DISKOS_HOST
size_t fontft_cache_bytes(void);
unsigned fontft_raster_count(void);
#endif
#endif
