/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FONTFT_H
#define FONTFT_H
#include "lvgl/lvgl.h"

/* Initialize once on the LVGL thread, after lv_init() and after config load. */
void fontft_init(void);
/* Lazy 10..140 px font, or NULL when the stock TTC cannot be used. Callbacks run on the LVGL thread. */
const lv_font_t *fontft_font(int px);
#ifdef DISKOS_HOST
size_t fontft_cache_bytes(void);
unsigned fontft_raster_count(void);
#endif
#endif
