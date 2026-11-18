/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_DEVICE_NAMES_UI_H
#define DISKOS_DEVICE_NAMES_UI_H
#include "screens.h"
void dnui_init(void);
void dnui_deliver(void);
void dnui_radio(void);
void dnui_create(lv_obj_t *);
void dnui_open(int bluetooth); /* Settings action: 0 Device, 1 Bluetooth */
void dnui_paired(const char *mac, const char *alias);
void dnui_reset(void);
#ifdef DISKOS_HOST
void dnui_fixture(int field, const char *value, int capability, int active);
void dnui_fixture_pending(void);
void dnui_fixture_corrupt(void);
#endif
#endif
