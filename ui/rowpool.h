/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef ROWPOOL_H
#define ROWPOOL_H
#include "lvgl/lvgl.h"

typedef struct row_pool row_pool_t;
typedef struct row_slot {
    row_pool_t *pool;
    lv_obj_t *row, *label[3];
    unsigned generation;
    int index, pinned, releasing;
} row_slot_t;
typedef void (*row_bind_t)(row_slot_t *slot, int first);
/* At most a full 360 px viewport, with the shortest supported 46 px rows.
 * Active capacity is still derived from each viewport, including partial rows. */
#define ROW_POOL_MAX_SLOTS ((360+45)/46+1+9)
struct row_pool {
    lv_obj_t *list, *extent;
    row_slot_t *slots;
    row_slot_t storage[ROW_POOL_MAX_SLOTS];
    int capacity, count, pitch, height, start, x, phase, numbered_prefix, two_line, busy;
    unsigned generation;
    uintptr_t viewport_token;
    row_bind_t bind;
    row_pool_t *next;
};
/* Clear before replacing any backing data. Presses and queued releases are cancelled. */
void row_pool_clear(row_pool_t *pool);
int row_pool_start(row_pool_t *pool, lv_obj_t *list, int count, int height,
                   int gap, int start, int x, int phase, int numbered_prefix, int two_line, row_bind_t bind);
void row_pool_update(row_pool_t *pool);
int row_slot_index(const row_slot_t *slot);
row_slot_t *row_pool_slot(const lv_obj_t *row);
int row_pool_model(const lv_obj_t *list, int *count, int *two_line);
int row_pool_phase(const lv_obj_t *row);
int row_pool_number(const lv_obj_t *row);
/* Reset reusable labels and theme decoration, keeping the slot's action registrations. */
void row_slot_reset(row_slot_t *slot);
void row_slot_style(row_slot_t *slot);
#ifdef DISKOS_HOST
lv_obj_t *row_pool_host_bind(row_pool_t *pool, int index);
#endif
#endif
