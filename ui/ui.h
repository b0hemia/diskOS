/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef UI_H
#define UI_H
#include "lvgl/lvgl.h"
#include "ipc.h"
#include "bookmeta.h"
void ui_create(lv_obj_t *root);
void ui_update(const track_state_t *st);
int ui_bookmeta_refresh(const track_state_t *st); /* poll only when drawing is allowed */
const bookmeta_snapshot *ui_bookmeta_snapshot(const track_state_t *st); /* matching UI-owned memory */
void ui_bookmeta_retry(const track_state_t *st); /* explicit failed-request retry, no I/O */
const char *ui_current_cover_src(void);
void ui_set_np_style(int vinyl);
void ui_vinyl_spin(int want);

/* Now Playing seek recognizer - fed raw touch from main.c (ring is display-only).
 * press/move arm/own the gesture; release returns 1 if it committed a seek
 * (so the caller skips its own nav-swipe handling). */
int  ui_np_seek_press(int x, int y);
int  ui_np_seek_move(int x, int y);
int  ui_np_seek_release(int x, int y);
void ui_np_seek_cancel(void);
void ui_np_seek_refused(void);
void ui_np_hold_return(const char *path, long ms);
#endif
