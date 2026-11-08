/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_INDIC_H
#define DISKOS_INDIC_H
#include "lvgl/lvgl.h"

/* Opt in before setting metadata. Ordinary text keeps the LVGL path. */
lv_obj_t *indic_label_create(lv_obj_t *parent);
void indic_label_enable(lv_obj_t *label);
int indic_text(const char *text);
void indic_font_register(const lv_font_t *font, int px);
int indic_font_px(const lv_font_t *font);
void indic_text_get_size(lv_point_t *size, const char *text, const lv_font_t *font,
                        int32_t tracking, int32_t line_gap, int32_t width, lv_text_flag_t flags);
/* Byte boundary in the original shaped string, including unsafe-break flags. */
int indic_boundary(const char *text, size_t byte, const lv_font_t *font);
int indic_label_cached_size(lv_obj_t *obj, lv_point_t *size, int last_line);
int indic_line_height_cached(lv_obj_t *obj);
int indic_font_height(const lv_font_t *font);
unsigned indic_scripts(const char *text);
int indic_font_height_scripts(const lv_font_t *font, unsigned scripts);
int indic_line_height(const char *text, const lv_font_t *font);
/* Grow two fixed metadata boxes and their row, only for Indic contents. */
void indic_stack_fit(lv_obj_t *first, lv_obj_t *second, lv_obj_t *third);
void indic_row_fit(lv_obj_t *row, lv_obj_t *title, lv_obj_t *subtitle);
#ifdef DISKOS_HOST
unsigned indic_shape_count(void);
unsigned indic_allocation_count(void);
int indic_host_glyph_keys(void);
void indic_fail_after(int count);
void indic_cache_clear(void);
void indic_cache_hold(int hold);
size_t indic_layout_bytes(void);
size_t indic_mask_bytes(void);
#endif
#endif
