/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "fwcaps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Verified RE data points (see RE_CATALOGUE / the V228 delta):
 *   V2.09  gain set = tag 0645   (no 0649 handler)
 *   V2.28  gain set = tag 0649   (the 0645 handler is a NULL pointer -> silent no-op)
 *   V2.40  gain set = tag 0649   (firmware audit 2026-09-05: on the v2.40 player, 0649 resolves
 *                                 0x413930 -> callback 0x4e8654 = the gain setter; 0645 is a NULL
 *                                 callback. Same tag as V2.28.)
 * We map only versions with RE data and fail closed on anything else (unknown/unreadable version ->
 * send no gain command) rather than guessing a tag that might hit an unrelated handler. Each new
 * firmware is gain-tested on-device before publish; add its verified {version, tag} entry here then.
 * NOTE: the v2.40 entry is RE-verified but the Low/High acoustic effect still wants an on-device check. */
static const struct { int ver; const char *tag; } GAIN_MAP[] = {
    { 209, "0645" },
    { 228, "0649" },
    { 240, "0649" },
    /* V2.57: 0649 (RE-confirmed handler identity), Low/High verified by ear on the owner's Disc 2026-09-29. */
    { 257, "0649" },
};

static int parse_os_ver(void){
    FILE *f = fopen("/etc/product_version/version.in", "r");
    if(!f) return 0;
    char line[128];
    int ver = 0;
    while(fgets(line, sizeof line, f)){
        size_t ll = strlen(line);
        if(!(ll && line[ll-1]=='\n') && ll == sizeof(line)-1){   /* overlong physical line -> drain + skip, so a
                                                                  * padded/truncated value can't be read as a real
                                                                  * MAIN_OS_VER and enable a wrong-firmware capability */
            int c; while((c=fgetc(f))!=EOF && c!='\n'){}
            continue;
        }
        char *p = line;
        while(*p == ' ' || *p == '\t') p++;                 /* tolerate leading whitespace */
        if(strncmp(p, "MAIN_OS_VER=", 12) == 0){
            char *num = p + 12, *end = NULL;
            long n = strtol(num, &end, 10);
            if(end == num){ ver = 0; break; }               /* no digits -> unknown */
            while(*end==' '||*end=='\t'||*end=='\r'||*end=='\n') end++;
            ver = (*end == '\0' && n > 0 && n < 100000) ? (int)n : 0;  /* reject trailing garbage (e.g. "228junk") */
            break;
        }
    }
    fclose(f);
    return ver > 0 ? ver : 0;
}

int fw_os_ver(void){
    static int cached = -1;                                 /* -1 = not read yet */
    if(cached < 0) cached = parse_os_ver();
    return cached;
}

/* The player generations whose local-init handshake / direct SD mount diskOS drives. Kept as two
 * distinct predicates (not a single >= check) so a future firmware can gain one without the other. */
int fw_needs_localplayer_init(void){ int v = fw_os_ver(); return v == 240 || v == 257; }
int fw_needs_direct_sd_mount(void){  int v = fw_os_ver(); return v == 240 || v == 257; }

/* Firmware whose player accepts the Custom EQ editor's fixed 10-band curve (32 Hz..16 kHz) as written. V2.57's PEQ
 * loader rejects and REPAIRS (and may persist) any band above half its internal processing rate, which diskOS
 * cannot read yet; at low source rates the 8 k/16 k bands exceed it. So the editor is view-only there (and on
 * unknown firmware) until the processing rate is measured on a V2.57 device. */
int fw_custom_peq_writable(void){ int v = fw_os_ver(); return v == 209 || v == 228 || v == 240; }

/* V2.57's player arbitrates the radios at boot: with both Wi-Fi and Bluetooth saved on it keeps Bluetooth, turns
 * Wi-Fi off and saves WIFI_STATUS=0 (P257@0x4ecf94..0x4ed030; absent from V2.40). diskOS must accept that decision
 * instead of its keepalive restarting Wi-Fi straight away (wifi.c radio reconciliation). */
int fw_has_radio_boot_arbitration(void){ return fw_os_ver() == 257; }
int fw_fav_play_by_love_id(void){ return fw_os_ver() == 257; }
int fw_power_key_driveable(void){ return fw_os_ver() == 257; }
/* 0648 = set ARTIST_CLASS_TYPE (Artist / Album Artist grouping) on every supported player: resolved tag -> dispatch
 * entry -> handler with tools/fw_tag_handler.py (V2.57 0x4f1ed0, V2.40 0x4e8c68, V2.28 0x4da3bc: all
 * player_handle_set_artist_class_type; V2.09 the same table slot, thunk 0x4127b0, RE_CATALOGUE). Unknown: never. */
int fw_artist_class_settable(void){ int v = fw_os_ver(); return v == 209 || v == 228 || v == 240 || v == 257; }
/* 0687 = player_handle_set_folder_jump_song on V2.28 0x4da28c, V2.40 0x4e8a84, V2.57 0x4f1c58 (tools/fw_tag_handler.py).
 * V2.09's 0687 reaches an unnamed handler that COMMAND_MAP lists as a media getter: never send it there. */
int fw_folder_jump_settable(void){ int v = fw_os_ver(); return v == 228 || v == 240 || v == 257; }
/* 064d = player_handle_set_track_display on V2.40 0x4e8b24 and V2.57 0x4f1cf8; no 064d tag on V2.09/V2.28. The player
 * only stores it (stock's own UI draws the numbers), so diskOS's Track Numbers works everywhere and sends it here. */
int fw_track_display_settable(void){ int v = fw_os_ver(); return v == 240 || v == 257; }
/* USB DAC host mode output (DWC2 host OTG + USB_HOST out_dev + SGM41513 5V boost).
 * Supported and verified on V2.40 and V2.57 (0642 5 + 0666 3 + 0657 8). Older versions (V2.09, V2.28) do not support it. */
int fw_has_usb_audio_out(void){ int v = fw_os_ver(); return v == 240 || v == 257; }

const char *fw_gain_tag(void){
    int v = fw_os_ver();
    for(unsigned i = 0; i < sizeof GAIN_MAP / sizeof GAIN_MAP[0]; i++)
        if(GAIN_MAP[i].ver == v) return GAIN_MAP[i].tag;
    return NULL;   /* unverified/unreadable firmware -> caller must not send a gain command */
}
