/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef FWCAPS_H
#define FWCAPS_H
/* Firmware-capability layer.
 *
 * The diskOS installer is version-AGNOSTIC (it gates on the boot-hook structure, not the version
 * number), so ONE diskOS binary may run on top of different stock firmware versions. A few stock
 * IPC command tags moved between firmware generations, so where behaviour differs we detect the
 * stock MAIN_OS_VER at runtime and adapt. Each firmware is still tested on-device before we
 * publish a base built on it.
 *
 * Source of truth = /etc/product_version/version.in (MAIN_OS_VER=NNN), read once and cached. */

/* Stock main-OS version (e.g. 209, 228); 0 if it can't be read. Cached after the first call. */
int fw_os_ver(void);

/* True for firmware generations whose player needs diskOS's runtime LOCALPLAYER init handshake
 * (0666 route + 0657 work-mode, sent once per player generation), and separately for those that
 * need diskOS to mount the SD directly. Both are false for unknown firmware. They are independent
 * capabilities that currently cover the same firmware set (V2.40 and V2.57). */
int fw_needs_localplayer_init(void);
int fw_needs_direct_sd_mount(void);

/* DAC gain-set command tag for the running firmware, or NULL if the firmware is not a verified
 * version (unknown/unreadable) - in which case the caller MUST NOT send a gain command (fail closed;
 * a guessed tag could hit an unrelated handler). V2.09 uses 0645; V2.28 uses 0649 (its 0645 handler
 * is a NULL no-op). Verified by RE of both mq_player dispatch tables. Returns a 4-hex-char prefix. */
const char *fw_gain_tag(void);

/* True where the Custom EQ editor may write + select its USER PEQ slots (see fwcaps.c); false = view-only. */
int fw_custom_peq_writable(void);

/* True where the stock player turns Wi-Fi off at boot when both radios were saved on (V2.57 only). */
int fw_has_radio_boot_arbitration(void);

/* True where a favourites (0100 type 6) play takes the MY_LOVE.ID of the first track as its start index and
 * queues MY_LOVE in title order (V2.57, device-verified 2026-09-27; index 0 or an unknown ID is refused).
 * False elsewhere: the V2.40 type-6 contract is static RE only, so those builds keep the older path. */
int fw_fav_play_by_love_id(void);
/* 1 = the player's power-button screen toggle is known well enough for diskOS to drive it (V2.57 static RE):
 * it announces sleep/wake as aa23 frames, does not grab event0, and toggles on an
 * EV_KEY 265/259 value 1. Other firmware: diskOS only follows the player's brightness writes. */
int fw_power_key_driveable(void);
/* 1 = this player takes 0648 (Artist / Album Artist grouping); 0 = unknown firmware, never send it */
int fw_artist_class_settable(void);
int fw_folder_jump_settable(void);    /* 0687 = folder jump (V2.28/V2.40/V2.57) */
int fw_track_display_settable(void);  /* 064d = track display (V2.40/V2.57) */
int fw_has_usb_audio_out(void);       /* 1 = firmware supports external USB Audio host mode (0642 5 + 0666 3) */

#endif /* FWCAPS_H */
