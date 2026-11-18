/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef INDIC_LAYOUT_H
#define INDIC_LAYOUT_H
#include "indic.h"
#define INDIC_MAX_GLYPHS 1024
#define INDIC_MAX_BYTES 4096
#define INDIC_MAX_MASKS 192
/* All pen positions stay signed 26.6 until final placement. */
typedef struct {
    int32_t x, y, advance;
    uint16_t cluster, mask;
    uint8_t unsafe, newline, space;
    uint8_t level, base, reset, joining, control;
} indic_glyph;
typedef struct {
    lv_image_dsc_t image;
    const void *face;
    uint32_t id;
    int16_t left, top;
    uint8_t px;
} indic_mask;
typedef struct {
    unsigned count, masks;
    int32_t advance, left, right, top, bottom;
    indic_glyph glyph[INDIC_MAX_GLYPHS];
    indic_mask mask[INDIC_MAX_MASKS];
    uint8_t pixels[64 * 1024];
    size_t pixels_used;
} indic_scratch;
/* Scratch is shared on the LVGL thread. No callers retain it. */
indic_scratch *indic_shape(const char *text, const lv_font_t *font, int px);
indic_scratch *indic_shape_config(const char *text, const lv_font_t *font, int px, int direction,
    const uint16_t *cuts, unsigned ncuts, const uint8_t *bases);
int indic_allocation_allowed(void);
void *indic_alloc(size_t size);
/* Retry missing faces only when preparing a new model/resource event. */
void indic_faces_retry(void);
#endif
