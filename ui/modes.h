/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef DISKOS_MODES_H
#define DISKOS_MODES_H

/* Output route (stock: Settings > SPDIF, Working Mode > USB AUDIO). See modes.c. The SPDIF row and the USB Audio
 * picker row exist only in builds made with -DDISKOS_TEST_OUTPUTS (device-unverified); the code below is always
 * built and is a no-op equivalent of the old preamble while the route is the internal DAC. */
enum { SOURCE_AIRPLAY = 0x101, SOURCE_USB_AUDIO = 0x200 };
int modes_receiver_quiet(void); /* strict closed-PCM proof, no mutation */
int modes_local_teardown(unsigned generation); /* one guarded Local reselect, no start */
int modes_beam_lock(unsigned generation);       /* beam interlock: 0657000C0002 (NO_WORK_MODE) so the stock play/pause key cannot start the Disc */
int modes_beam_unlock(void);                    /* the matching restore: 0657000C0008, for the CURRENT player generation (a restarted player loads the saved mode) */

enum { OUT_INTERNAL = 0, OUT_SPDIF = 1, OUT_USB = 2 };

int  modes_output_route(void);          /* the route diskOS last set THIS player generation (else OUT_INTERNAL) */
int  modes_output_switch(int target);   /* Settings SPDIF toggle: pause -> silent -> 0666 -> 0657 8 -> resume; 0 = started, -1 = refused */
int  modes_output_mode_switch(int target); /* Working Mode entry (adds 0642 + stock's 65 ms) */
void modes_output_reset(void);          /* another path (source change, BT route) put the player back on the internal DAC */
int  modes_pcm_quiet(void);             /* 1 only when every playback PCM node is readable and explicitly closed */
int  modes_output_busy(void);           /* 1 while a switch waits for silence or recovers: play/next/source/BT must refuse */
int  modes_local_init(int with_gadget); /* route-aware local init: 0642/0666/0657 for the current route; -1 if a send failed */

/* main.c: NULL when an output switch may run now, else a short reason for the toast */
const char *ui_output_blocked(void);

#endif
