/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_RTL_H
#define DISKOS_RTL_H
#include <stddef.h>
#include <stdint.h>
#define RTL_MAX_BYTES 4096
/* Byte-indexed maps are copied into glyphs; nothing retains the engine objects. */
typedef struct {
    uint8_t level[RTL_MAX_BYTES], base[RTL_MAX_BYTES], reset[RTL_MAX_BYTES];
} rtl_model;
const rtl_model *rtl_resolve(const char *text, int direction);
const rtl_model *rtl_resolve_bases(const char *text, int direction, const uint8_t *bases);
const rtl_model *rtl_resolve_lines(const char *text, int direction, const uint8_t *bases,
    const uint16_t *cuts, unsigned ncuts);
int rtl_control(uint32_t cp);
unsigned rtl_count(void);
size_t rtl_peak(void);
size_t rtl_live(void);
unsigned rtl_allocations(void);
#endif
