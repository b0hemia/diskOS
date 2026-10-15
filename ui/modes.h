/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef DISKOS_MODES_H
#define DISKOS_MODES_H

/* Output route (stock: Settings > SPDIF). See modes.c. The SPDIF row exists
 * only in builds made with -DDISKOS_TEST_OUTPUTS (device-unverified); the code below is always
 * built and is a no-op equivalent of the old preamble while the route is the internal DAC. */
enum { OUT_INTERNAL = 0, OUT_SPDIF = 1 };

int  modes_output_route(void);          /* the route diskOS last set THIS player generation (else OUT_INTERNAL) */
int  modes_output_switch(int target);   /* Settings SPDIF toggle: pause -> silent -> 0666 -> 0657 8 -> resume; 0 = started, -1 = refused */
void modes_output_reset(void);          /* another path (source change, BT route) put the player back on the internal DAC */
int  modes_output_busy(void);           /* 1 while a switch waits for silence or recovers: play/next/source/BT must refuse */
int  modes_local_init(int with_gadget); /* route-aware local init: 0642/0666/0657 for the current route; -1 if a send failed */

/* main.c: NULL when an output switch may run now, else a short reason for the toast */
const char *ui_output_blocked(void);

#endif
