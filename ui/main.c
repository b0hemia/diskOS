/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <ucontext.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>   /* mmap /dev/mem: read the Vol-Up GPIO pin level directly at boot */
#include <stdint.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/mount.h>     /* mount(): direct SD mount fallback when the player's uevent listener won't (V2.40) */
#include <sys/stat.h>
#include <glob.h>
#include <dirent.h>      /* mkdir() for the SD mount point */
#include <linux/input.h>   /* EVIOCGKEY / KEY_MAX for the boot-time Vol-Up override */
#include <net/if.h>
#include "lvgl/lvgl.h"
#include "lvgl/src/core/lv_refr_private.h"
#include "theme.h"
#include "theme_kit.h"
#include "fontft.h"
#include "fb_pan.h"
#include "controls.h"
#include "screens.h"
#include "modes.h"
#include "anim.h"
#include "ipc.h"
#include "opustitle_worker.h"
#include "fwcaps.h"
#include "musicdb.h"
#include "prof.h"
#include "lvgl/src/drivers/evdev/lv_evdev.h"
#include "config.h"
#include "lastfm.h"
#include "scanner.h"
#include "playstate.h"
#include "sdio.h"
#include "art.h"
#include "power.h"

void mclock_sync(int backlight_on);
void ui_np_title_scroll(int visible);
void library_jump_cancel(void);

static lv_indev_t *g_touch = NULL;
static int         g_screen_off = 0;   /* mirrors (bl_state==2), including changes before navigation;
                                          slow polls also use it to skip work while the panel is off. */
static int         g_touch_raw = -1;   /* a 2nd, RAW read-only fd on the touch evdev, used ONLY to
                                          wake a screen-off panel: LVGL coalesces a quick tap into a
                                          final RELEASED (no press edge -> no wake), so we watch the
                                          raw stream where ANY event means "a finger touched". */

/* Manual "Sleep" (Quick Settings tile): a deliberate screen-off request. It must dim+power
 * the panel even when the auto-screensaver is Off, so it can't ride the saver_timeout>0 path.
 * g_manual_sleep is armed by ui_request_sleep(); the saver block below honours it and clears
 * it once the panel is woken (touch takes us off SCR_SAVER). */
static volatile int g_manual_sleep = 0;
static uint32_t g_manual_sleep_at = 0;
void ui_request_sleep(void){ g_manual_sleep = 1; g_manual_sleep_at = lv_tick_get(); }
int ui_screen_backlight_on(void){ return !g_screen_off; }
void ui_backlight_hold(void);              /* settings.c: positive levels wait for the release */
void ui_backlight_release(int fallback);   /* settings.c: light the latest held level (else fallback) */
/* UI thread: hold drawing and brightness until wake content has caught up. The brightness hold is in settings.c's
 * ui_backlight, so every writer (callbacks included) waits for the repaint; release lights the latest level. */
static int g_draw_dark;
static void draw_set_dark(int dark){
    if(dark == g_draw_dark) return;
    lv_display_t *d = lv_display_get_default();
    if(!d) return;
    g_draw_dark = dark;
    if(dark){
        /* Discard pending redraws before disabling the queue, including a saver pop. */
        lv_inv_area(d, NULL);
        lv_display_enable_invalidation(d, 0);
        lv_timer_pause(lv_display_get_refr_timer(d));
        ui_backlight_hold();
    } else {
        lv_display_enable_invalidation(d, 1);
        lv_obj_invalidate(lv_display_get_screen_active(d));
        lv_obj_invalidate(lv_display_get_layer_bottom(d));
        lv_obj_invalidate(lv_display_get_layer_top(d));
        lv_obj_invalidate(lv_display_get_layer_sys(d));
        lv_refr_now(d);
        ui_backlight_release(ui_effective_brightness());
    }
}
static void wake_backlight(int v){ ui_backlight(v); }   /* a wake's light: held by ui_backlight while dark */
void albumwall_poll_sync(int lit);
static lv_obj_t   *g_dbgdot = NULL;
static int         g_dbg = 0;          /* show tap dot (only if /usr/data/touch_dbg) */

#define SWIPE_THRESH_DEFAULT 60
/* The back-swipe must START on the left side of the 360px screen (and travel
 * rightwards), so a horizontal swipe on the right side never goes back. */
#define BACK_START_MAX_X 64    /* back-swipe must START near the left edge (iOS-style) so it
                                * doesn't fight horizontal drift during a vertical list scroll */
/* On Now Playing the seek ring fills the screen, so a seek can start anywhere.
 * A navigation swipe (back / hub) is told apart from a seek by being a LONG,
 * STRAIGHT, horizontal slide - a seek drag follows the ring's curve, so it is
 * shorter and/or carries a vertical component. */
#define NP_NAV_DIST 95    /* min horizontal travel for a back/hub swipe on NP (eased from 110) */
#define NP_NAV_STRAIGHT 3 /* require |dx| > this * |dy| (nearly horizontal) */
static int g_swipe_thresh = SWIPE_THRESH_DEFAULT;   /* cached for the hot loop */

int ui_get_swipe_thresh(void){ return g_swipe_thresh; }
/* How far the current / last touch travelled from where it went down (px, the larger axis). LVGL still reports a
 * CLICKED on release after a swipe that didn't scroll anything, so a tap target a swipe happens to start on (Home's
 * full-width weather line) fired alongside the swipe - Weather opened over the page swiped to. Tap handlers that sit
 * under swipe gestures check this and ignore a press that was really a swipe. */
static int g_press_sx, g_press_sy, g_press_lx, g_press_ly;
int ui_press_travel(void){
    /* the release can arrive in the same read as the last movement, and CLICKED runs (inside lv_timer_handler) before
     * the main loop records it - so fold in the position LVGL itself is processing right now */
    lv_indev_t *in = lv_indev_active();
    if(in){ lv_point_t p; lv_indev_get_point(in, &p); if(p.x >= 0 && p.x < 360 && p.y >= 0 && p.y < 360){ g_press_lx = p.x; g_press_ly = p.y; } }
    int dx = g_press_lx - g_press_sx, dy = g_press_ly - g_press_sy;
    if(dx < 0) dx = -dx;
    if(dy < 0) dy = -dy;
    return dx > dy ? dx : dy;
}

/* idle mirror for the prewarm worker: 1 once the screen has dimmed/off (saver), i.e.
 * the user isn't actively looking. Updated each main-loop iteration from bl_state.
 * _Atomic: written here (main thread), read by the prewarm worker thread. */
static _Atomic int g_bl_idle = 0;
int ui_main_is_idle(void){ return g_bl_idle; }
void ui_apply_swipe_thresh(int px){   /* live, no flash write */
    if(px < 20) px = 20; if(px > 200) px = 200;
    g_swipe_thresh = px;
}
void ui_set_swipe_thresh(int px){     /* live + persist */
    ui_apply_swipe_thresh(px);
    cfg_set_int("swipe_thresh", g_swipe_thresh);
}
static void swipe_thresh_load(void){
    ui_apply_swipe_thresh(cfg_get_int("swipe_thresh", SWIPE_THRESH_DEFAULT));   /* same clamp as the setter */
}

static void dbgdot_init(void){
    g_dbgdot = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_dbgdot);
    lv_obj_set_size(g_dbgdot, 26, 26);
    lv_obj_set_style_radius(g_dbgdot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_dbgdot, TC(FIXED_DEBUG_TOUCH), 0);
    lv_obj_set_style_bg_opa(g_dbgdot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_dbgdot, TC(OUTLINE_BRIGHT), 0);
    lv_obj_set_style_border_width(g_dbgdot, 2, 0);
    lv_obj_add_flag(g_dbgdot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_dbgdot, LV_OBJ_FLAG_CLICKABLE);
}

static void go_settings(void){ screen_show(SCR_SETTINGS); }

/* small sysfs int reader (backlight brightness/bl_power polling for the power-button rail fix). */
static int read_int_file(const char *path){
    FILE *f = fopen(path, "r"); if(!f) return -1;
    int v = -1; if(fscanf(f, "%d", &v) != 1) v = -1; fclose(f); return v;
}

/* The power button belongs to the player (it grabs event0). A short press toggles the player's screen: to sleep it
 * suspends the LCD panel (SUSPEND_CMD on /dev/lcd_st77916) and the touch chip (CST8xx) and writes brightness 0; to wake
 * it resumes both and writes its own saved level (mq_player V2.57 lcd_handler.c / echo_panel_sleep_in / _out strings;
 * device-measured 2026-10-07: 6 -> 0 -> 9). diskOS can only follow: brightness is the one trace it can read, and a
 * backlight lit over a sleeping panel shows nothing (device test 2026-10-07: a "wake" on the 0 write flashed and went
 * black). So: the player sleeping (0) powers diskOS's rail down; the player waking (its level after a 0) lights the
 * screen at the USER's level (it came back at the player's 9 instead of the user's 23). After diskOS's own Screen Off
 * (rail down, panel still awake) the first press only sleeps the panel and the second wakes it. A write that is neither
 * (the player rewriting a level while awake) keeps diskOS's level. A press that writes the value already there is
 * invisible here (e.g. a user level equal to the player's). */
typedef struct { int seen; int player_off; unsigned off_seq; } powerkey_t;
    /* seen: brightness last seen; player_off: panel asleep; off_seq: diskOS's write count when it went to sleep */
enum { PK_NONE, PK_OFF, PK_WAKE, PK_RELEVEL };
/* ours / ours_seq: the value and write count of diskOS's own brightness writes. After a player sleep, a level is the
 * player's wake unless diskOS has written since (its startup write, a tap) - so a player level that EQUALS the
 * user's still wakes the screen. */
static int powerkey_step(powerkey_t *k, int cbr, int ours, unsigned ours_seq, int dark){
    if(cbr < 0 || cbr == k->seen) return PK_NONE;
    k->seen = cbr;
    if(cbr == 0){ k->player_off = 1; k->off_seq = ours_seq; return dark ? PK_NONE : PK_OFF; }   /* panel asleep */
    if(k->player_off){
        if(cbr == ours && ours_seq != k->off_seq) return PK_NONE;     /* diskOS's own write while the panel sleeps */
        k->player_off = 0; return PK_WAKE;                    /* panel awake again: light it at the user's level */
    }
    if(cbr == ours) return PK_NONE;                            /* our own write landing */
    return PK_RELEVEL;                                         /* the player rewrote a level - keep ours */
}
/* Zero during early boot may precede the player's first level. Prefer a lit boot if uptime is unknown too.
 * After a conservative 150s boot window, preserve zero so an equal-level wake remains observable. */
static int powerkey_defer_startup(int brightness){
    if(brightness != 0 || fw_power_key_driveable()) return 0;
    double up = -1; FILE *f = fopen("/proc/uptime", "r");
    if(f){ if(fscanf(f, "%lf", &up) != 1) up = -1; fclose(f); }
    return up > 150.0;
}
/* V2.57: one player screen frame (aa23 0 = it slept its panel and touch, 1 = it woke them) -> what diskOS does.
 * Dark with the player awake can only be diskOS's own Screen Off, so a sleep frame then is a press meant to WAKE:
 * the player is asked to undo its sleep (SF_INJECT; its wake frame follows), unless it is powering off. The press is
 * sent only after the player's sleep has finished (~240 ms: two 120 ms waits after the frame) and only if no newer
 * screen frame came in meanwhile (a USB plug or a library scan can wake the player on its own; a toggle then would
 * put it back to sleep). That narrows, but cannot close, the window for such a wake - an event already queued in
 * the key device cannot be recalled. */
enum { SF_NONE, SF_OFF, SF_WAKE, SF_INJECT };
static int screen_frame_step(int on, int bl_state, int *player_off, int poweroff_inhibit){
    if(on){ *player_off = 0; return SF_WAKE; }
    int own_off = bl_state == 2 && !*player_off;
    *player_off = 1;
    if(own_off) return poweroff_inhibit ? SF_NONE : SF_INJECT;
    return SF_OFF;
}
/* One short power-button press into the player's key device (V2.57: mq_player reads event0 "x2000_key" without a
 * grab and toggles its screen on EV_KEY 265 value 1). Used only to undo the
 * player's sleep after a press that was meant to wake diskOS's own Screen Off, so the player and diskOS agree on
 * whether the screen is on. Never codes 264 (power-off) or a held key. 1 = written. */
static int power_key_inject(void){
    char path[64], name[32];
    for(int i = 0; i < 8; i++){
        snprintf(path, sizeof path, "/sys/class/input/event%d/device/name", i);
        FILE *f = fopen(path, "r"); if(!f) continue;
        int got = fgets(name, sizeof name, f) != NULL; fclose(f);
        if(!got || strncmp(name, "x2000_key", 9) || (name[9] && name[9] != '\n')) continue;
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if(fd < 0) return 0;
        struct input_event ev[4]; memset(ev, 0, sizeof ev);
        ev[0].type = EV_KEY; ev[0].code = 265; ev[0].value = 1;  ev[1].type = EV_SYN; ev[1].code = SYN_REPORT;
        ev[2].type = EV_KEY; ev[2].code = 265; ev[2].value = 0;  ev[3].type = EV_SYN; ev[3].code = SYN_REPORT;
        int ok = write(fd, ev, sizeof ev) == (ssize_t)sizeof ev;
        close(fd);
        fprintf(stderr, "power key: %s a press to wake the player's screen\n", ok ? "sent" : "could not send");
        return ok;
    }
    return 0;
}
/* the dimmed level: the vinyl art-showcase saver keeps the user's brightness, everything else crushes to 6 */
static int dim_level(void){
    int dim = ui_effective_brightness();
    if(dim > 6 && !(screen_current() == SCR_SAVER && saver_wants_bright())) dim = 6;
    return dim;
}

static void clock_tick(lv_timer_t *t){
    (void)t;
    time_t now = time(NULL);
    struct tm lt; localtime_r(&now, &lt);
    char hm[12], date[24];
    int h24 = cfg_get_int("time_24h", 1);
    if(h24){
        strftime(hm, sizeof hm, "%H:%M", &lt);
    } else {
        strftime(hm, sizeof hm, "%I:%M %p", &lt);   /* 12-hour with AM/PM */
        if(hm[0]=='0') memmove(hm, hm+1, strlen(hm)); /* drop leading zero */
    }
    strftime(date, sizeof date, "%a %d %b", &lt);
    home_set_clock(hm, date);
    saver_set_clock(hm, date);
}
void ui_clock_refresh(void){ clock_tick(NULL); }

/* Status indicators: poll battery (cw221X fuel gauge), wifi (wlan0 has an IP),
 * and bt (any active HCI connection) from sysfs/tools, push to the home row.
 * One short popen on a slow timer - battery/link state changes slowly. */
/* battery + wifi read directly (no shell spawn); only BT keeps a light call since the
 * device exposes no sysfs connection indicator and the HCI ioctl layout is fragile. */
static void status_poll_cb(lv_timer_t *t){
    (void)t;
    /* Runs on the fast (3s) timer so charging/battery/wifi track plug/unplug promptly. */
    int batt = -1, charging = 0, wifi = 0, bt;

    FILE *bf = fopen("/sys/class/power_supply/cw221X-bat/capacity", "r");
    if(bf){ if(fscanf(bf, "%d", &batt) != 1) batt = -1; fclose(bf); }
    /* This cw221X fuel gauge exposes NO `status` node - charging shows via `current_now`, which is a
     * crude boolean on this driver: 1 while charging (USB/charger present), 0 on battery. (Live-probed
     * 2026-08-11: plugged=1 @3.99V, unplugged=0 @3.72V.) */
    long cur = 0;
    FILE *cf = fopen("/sys/class/power_supply/cw221X-bat/current_now", "r");
    if(cf){ if(fscanf(cf, "%ld", &cur) != 1) cur = 0; fclose(cf); }
    charging = (cur > 0);

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if(s >= 0){
        struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
        strncpy(ifr.ifr_name, "wlan0", IFNAMSIZ-1);
        if(ioctl(s, SIOCGIFADDR, &ifr) == 0) wifi = 1;   /* has an IPv4 address */
        close(s);
    }
    /* "has an IP" alone is NOT enough: a DHCP lease lingers on wlan0 after the AP vanishes, so the
     * icon stayed lit while actually disconnected. Require the link to be ASSOCIATED. Measured on this
     * bcmdhd chip (2026-08-23): on disconnect `carrier` STAYS 1 (unreliable), but `operstate` goes
     * up->dormant and wpa_state->DISCONNECTED. So gate on operstate=="up". A failed/absent read
     * (iface down, no wlan0) falls through to disconnected - the correct/safe default. */
    if(wifi){
        char op[16] = {0};
        FILE *of = fopen("/sys/class/net/wlan0/operstate", "r");
        if(of){ if(!fgets(op, sizeof op, of)) op[0] = 0; fclose(of); }
        wifi = (strncmp(op, "up", 2) == 0);
    }

    /* BT icon = the ACTUAL radio state via bt_radio_on() (a cheap rfkill /sys read, no process spawn).
     * Using the cfg "bt_on" intent alone went stale when the stock firmware brought BT up at boot out
     * from under us (icon/tile said off while BT was really on). rfkill reflects reality either way. */
    bt = bt_radio_on();

    home_set_status(batt, charging, wifi, bt);
}
void ui_status_refresh(void){ status_poll_cb(NULL); }

/* Persistent clock: the device has a (backed) RTC but the boot init never loads
 * it, so the system clock starts at the 2020 kernel default. Like stock
 * mq_player, load the system clock FROM the RTC at startup (offline-safe) and
 * write it BACK periodically so the RTC stays current (ntpd corrects the system
 * clock when WiFi is up, and that corrected time then gets saved to the RTC). */
static int run_bounded(char *const argv[], int timeout_ms);   /* defined below (killable child + hard timeout) */
static void hwclock_save_tick(lv_timer_t *t);   /* defined below: starts the RTC save WITHOUT waiting for it */

/* ---- IPC command frames (decoded from mq_player's command table @0x7b7c90
 * and live-verified against the player log oracle) --------------------------
 * Frame = TAG(4) + LEN(4hex total strlen) + DATA.  DATA field classes:
 *   class1: <idx 4hex>          + string
 *   class2: <f1 4hex><f2 4hex>  + string
 * SEEK     : 0103 (class2) <ms hi16><ms lo16>   e.g. 0103001000001388 = 5000ms  [VERIFIED]
 * PLAY     : 0100 (class2) <startpos-1 4hex><list_type 4hex><name>             [VERIFIED]
 *            list_type 0001=all-songs 0002=artist 0003=album 0006=favourites
 *            000A=genre (0000=resume current, no rebuild); player rebuilds
 *            LIST_SONG_0 from SONG via its own SQL, then starts at the track.
 * WORKMODE : 0102 000C <4-hex mode>   (Roon loop/shuffle; no-op in LOCAL - verify by ear)
 * EQ       : 0689 000C <4-hex preset> (selector - verify)
 * RESCAN   : 0622000C0001 (NAS-family; unverified) */
int ui_local_playback_allowed(void);   /* defined with the launch verdict, below */
int ui_seek_to(long ms){
    /* A seek drives the player's current (card-backed) track, so it is card access like any other: never
     * before this boot's launch verdict qualified the player, and never while SD access is held. */
    if(!ui_local_playback_allowed()) return -1;
    if(ms < 0) ms = 0;
    char f[32]; snprintf(f, sizeof f, "01030010%08lX", (unsigned long)ms);
    int rc = ipc_send_cmd(f);   /* 0 = queued OK, -1 = send failed */
    fprintf(stderr,"seek %ldms -> %s (rc=%d)\n", ms, f, rc); fflush(stderr);
    return rc;
}
int ui_apply_eq(int preset){
    /* 0..10 = built-in presets (off..retro, sibilance 1/2); 11..20 = user/custom PEQ slots
     * (User 1 = 11). Cap at 20 so the custom EQ can select its slot. Returns the send rc
     * (<0 = the 0689 select never reached the player) so the editor can report honestly. */
    if(preset < 0) preset = 0; if(preset > 20) preset = 20;
    char f[16]; snprintf(f, sizeof f, "0689000C%04X", preset);
    int rc = ipc_send_cmd(f);
    fprintf(stderr,"eq %d -> %s\n", preset, f); fflush(stderr);
    return rc;
}
/* Central EQ selection used by the drawer, Settings, Tune and the editor so all four stay in sync.
 * Applies the preset, and ONLY on a successful send persists eq_preset + remembers the last non-off
 * preset (eq_last) for the drawer's A/B toggle. Returns 0 on success, -1 if the send failed (nothing
 * persisted, so callers can keep their previous state). */
int ui_eq_select(int preset){
    if(preset < 0) preset = 0; if(preset > 20) preset = 20;
    if(ui_apply_eq(preset) < 0) return -1;
    if(preset > 0) cfg_set_int_deferred("eq_last", preset);
    cfg_set_int_deferred("eq_preset", preset);
    cfg_flush();   /* both keys in one rewrite */
    return 0;
}

/* --- Audio/DAC cluster (commands reverse-engineered 2026-06-30, RE_CATALOGUE section 5c) ---------
 * These apply LIVE only; the player persists to SYSCONFIG on shutdown, not on the command.
 * So diskOS owns persistence: settings.c stores the chosen value in diskos.conf and we
 * re-send the commands once the player is ready (ui_reapply_audio). A value of -1 in cfg =
 * "unmanaged / System default" -> we send nothing (don't change the user's sound). */
void ui_set_dre(int on){        if(on<0) return; ipc_send_cmd(on   ? "0812000C0000" : "0812000C0001"); }  /* on=0000, off=0001 */
void ui_set_gain(int high){     if(high<0) return; const char *tag=fw_gain_tag(); if(!tag){ fprintf(stderr,"gain: unverified firmware (MAIN_OS_VER=%d) -> not sending\n", fw_os_ver()); return; } char f[16]; snprintf(f,sizeof f,"%s000C%04X", tag, high?1:0); ipc_send_cmd(f); } /* Low=0, High=1; tag firmware-gated (V2.09=0645, V2.28=0649); fail closed on unknown fw */
void ui_set_output(int spdif){  if(spdif<0) return; modes_output_switch(spdif ? OUT_SPDIF : OUT_INTERNAL); } /* stop -> 0666 route -> 0657 -> resume, see modes.c */
void ui_set_dac_filter(int idx){ if(idx<0||idx>5) return; char f[16]; snprintf(f,sizeof f,"0653000C%04X",idx); ipc_send_cmd(f); }
/* ReplayGain / volume-levelling: 0718 (RE-confirmed, on-device-safe 2026-08-24). 0=Off 1=Track 2=Album. */
void ui_set_replay_gain(int v){ if(v<0||v>2) return; char f[16]; snprintf(f,sizeof f,"0718000C%04X",(unsigned)v); ipc_send_cmd(f); }
/* more settings decoded 2026-06-30 (RE_CATALOGUE section 5d) - config/mixer commands, applied on
 * live user change only (never blind-sent at boot). */
void ui_set_gapless(int on){   if(on<0) return; ipc_send_cmd(on ? "0647000C0001" : "0647000C0000"); }
/* Artist / Album Artist grouping (0648, the player's ARTIST_CLASS_TYPE): the player builds its artist queues with it,
 * so the Artists view switches with it. Returns 0 sent, -1 not sent (unknown firmware or the send failed). */
int ui_set_artist_class(int album_artist){
    if(!fw_artist_class_settable()){ fprintf(stderr, "artist class: unverified firmware (MAIN_OS_VER=%d)\n", fw_os_ver()); return -1; }
    if(ipc_send_cmd(album_artist ? "0648000C0001" : "0648000C0000") < 0) return -1;
    mdb_set_artist_mode(album_artist);
    library_artist_mode_changed();
    return 0;
}
/* Play Through Folders (0687): at the end of an artist/album/folder/genre queue the player continues with the next
 * one (folder_jump_server; list once / shuffle / list loop only). Returns 0 sent, -1 not (firmware or send). */
int ui_set_folder_jump(int on){
    if(!fw_folder_jump_settable()){ fprintf(stderr, "folder jump: unverified firmware (MAIN_OS_VER=%d)\n", fw_os_ver()); return -1; }
    return ipc_send_cmd(on ? "0687000C0001" : "0687000C0000") < 0 ? -1 : 0;
}
/* Track Numbers: diskOS draws them itself; 064d only keeps the player's saved TRACK_DISPLAY in step with it. */
void ui_set_track_display(int on){ if(fw_track_display_settable()) ipc_send_cmd(on ? "064d000C0001" : "064d000C0000"); }
void ui_set_memory(int mode){  if(mode<0||mode>2) return; char f[16]; snprintf(f,sizeof f,"0684000C%04X",mode); ipc_send_cmd(f); }
void ui_set_maxvol(int v){     if(v<0) v=0; if(v>120) v=120; char f[16]; snprintf(f,sizeof f,"0711000C%04X",v); ipc_send_cmd(f); }
/* balance: v in -N..+N; 0=center, v>0 -> 0x00NN (one side), v<0 -> 0x01NN (other side). */
void ui_set_balance(int v){ if(v < -10 || v > 10) return;   /* enforce UI range; avoids -INT_MIN UB */
                            int fv = (v>0) ? (v & 0xFF) : (v<0) ? (0x0100 | ((-v) & 0xFF)) : 0;
                            char f[16]; snprintf(f,sizeof f,"0713000C%04X",fv); ipc_send_cmd(f); }

/* ---- Bluetooth audio output routing (live-captured from working stock mq_ui) --------------
 * Follow stock's pre-stop/mode/work/BT-init/route/work/codec/volume transition. The
 * out_dev=LOCAL(6) pre-stop normalizes player state before out_dev=BT(2), avoiding the
 * observed mq_player crash/reboot. Codec VALUE1=0 is SBC; keep the MAC payload so the
 * frame shape matches stock. Pause/seek/play resumes the current track after routing.
 * g_route_mac = the MAC we've routed to ("" = local/analog; kept for validation + dedup). */
static char g_route_mac[20] = "";
static int g_startup_route_checked = 0;
static void route_uppercase(const char *in, char *out, int cap);
#ifndef ROUTE_STATE_PATH
#define ROUTE_STATE_PATH "/tmp/.diskos-ui/route"
#endif
#ifndef ROUTE_STATE_DIR
#define ROUTE_STATE_DIR "/tmp/.diskos-ui"
#endif
#ifndef ROUTE_STATE_UID
#define ROUTE_STATE_UID 0
#endif
#ifndef ROUTE_PROC_ROOT
#define ROUTE_PROC_ROOT "/proc"
#endif
/* /proc start time distinguishes a surviving player from a reused PID. The IPC
 * generation is recorded for diagnosis, but resets when the UI process restarts. */
static int route_player_identity(long *pid, unsigned long long *start){
    DIR *dir = opendir(ROUTE_PROC_ROOT);
    if(!dir) return -1;
    int found = 0;
    struct dirent *ent;
    while((ent = readdir(dir))){
        char *end; errno = 0;
        long p = strtol(ent->d_name, &end, 10);
        if(errno || p <= 0 || *end) continue;
        char path[256];
        if(snprintf(path, sizeof path, "%s/%ld/stat", ROUTE_PROC_ROOT, p) >= (int)sizeof path) continue;
        FILE *f = fopen(path, "r");
        if(!f) continue;
        char line[1024]; int ok = fgets(line, sizeof line, f) != NULL && strchr(line, '\n') != NULL;
        fclose(f);
        if(!ok) continue;
        char *lp = strchr(line, '('), *rp = strrchr(line, ')');
        if(!lp || !rp || rp <= lp || (size_t)(rp-lp-1) != strlen("mq_player") ||
           strncmp(lp+1, "mq_player", rp-lp-1) || rp[1] != ' ') continue;
        char *save = NULL, *tok = strtok_r(rp+2, " \n", &save);
        if(!tok || tok[0] == 'Z' || tok[0] == 'X') continue;
        for(int field=3; field<22 && tok; field++) tok = strtok_r(NULL, " \n", &save);
        if(!tok) continue;
        errno = 0;
        unsigned long long ticks = strtoull(tok, &end, 10);
        if(errno || !ticks || *end) continue;
        *pid = p; *start = ticks;
        found++;
        if(found > 1) break;  /* ambiguous player ownership: never persist a route */
    }
    closedir(dir);
    return found == 1 ? 0 : -1;
}
static int route_state_dir(int create){
    struct stat st;
    if(create && mkdir(ROUTE_STATE_DIR, 0700) < 0 && errno != EEXIST) return 0;
    return lstat(ROUTE_STATE_DIR, &st) == 0 && S_ISDIR(st.st_mode) &&
           st.st_uid == ROUTE_STATE_UID && (st.st_mode & 07777) == 0700;
}
static void route_state_remove(void){ if(route_state_dir(0)) (void)unlink(ROUTE_STATE_PATH); }
static void route_state_save(const char *mac){
    if(!route_state_dir(1)) return;
    long pid; unsigned long long start;
    if(route_player_identity(&pid, &start) < 0){ route_state_remove(); return; }
    char data[128];
    int n = snprintf(data, sizeof data, "pid=%ld start=%llu gen=%u mac=%s\n",
                     pid, start, ipc_generation(), mac);
    if(n <= 0 || n >= (int)sizeof data){ route_state_remove(); return; }
    char tmp[sizeof ROUTE_STATE_PATH + 16];
    if(snprintf(tmp, sizeof tmp, "%s.XXXXXX", ROUTE_STATE_PATH) >= (int)sizeof tmp){ route_state_remove(); return; }
    int fd = mkstemp(tmp);
    if(fd < 0){ route_state_remove(); return; }
    size_t pos = 0;
    while(pos < (size_t)n){ ssize_t w = write(fd, data+pos, (size_t)n-pos);
        if(w < 0 && errno == EINTR) continue;
        if(w <= 0) break;
        pos += (size_t)w;
    }
    int ok = pos == (size_t)n && fchmod(fd, 0600) == 0;
    close(fd);
    if(!ok || rename(tmp, ROUTE_STATE_PATH) != 0){ unlink(tmp); route_state_remove(); }
}
/* Return 1 only for this running player. Any other file is discarded. */
static int route_state_restore(void){
    if(!route_state_dir(0)) return 0;
    int fd = open(ROUTE_STATE_PATH, O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd < 0){ if(errno != ENOENT) route_state_remove(); return 0; }
    struct stat st;
    char data[128]; ssize_t n = -1;
    if(fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == ROUTE_STATE_UID &&
       (st.st_mode & 07777) == 0600 && st.st_size > 0 && st.st_size < (off_t)sizeof data)
        n = read(fd, data, sizeof data);
    close(fd);
    if(n <= 0 || n >= (ssize_t)sizeof data || n != st.st_size || memchr(data, 0, (size_t)n)){
        route_state_remove(); return 0;
    }
    data[n] = 0;
    long pid = -1, current_pid; unsigned long long start = 0, current_start;
    unsigned gen; char mac[20], canonical[128];
    if(sscanf(data, "pid=%ld start=%llu gen=%u mac=%19s", &pid, &start, &gen, mac) != 4 ||
       snprintf(canonical, sizeof canonical, "pid=%ld start=%llu gen=%u mac=%s\n", pid, start, gen, mac) <= 0 ||
       strcmp(data, canonical) || pid <= 0 || !start ||
       route_player_identity(&current_pid, &current_start) < 0 ||
       pid != current_pid || start != current_start){ route_state_remove(); return 0; }
    char norm[20]; route_uppercase(mac, norm, sizeof norm);
    if(strlen(norm) != 17){ route_state_remove(); return 0; }
    for(int i=0;i<17;i++){
        if((i+1)%3 == 0 ? norm[i] != ':' :
           !((norm[i]>='0'&&norm[i]<='9') || (norm[i]>='A'&&norm[i]<='F'))){
            route_state_remove(); return 0;
        }
    }
    snprintf(g_route_mac, sizeof g_route_mac, "%s", norm);
    return 1;
}
static int g_playing = 0;   /* published each tick by the main loop's play/pause inference; the routing
                             * fns read THIS, not the raw st.state (which reports 0 while playing). */
/* After a player restart, the fresh player runs a ~7s late-init on its mode-control thread that can
 * overwrite an output route sent too early. The v2.40 local-init already waits this out (localplayer_workmode_cb
 * settles ~9s). BT routing must wait the same window: a speaker's bluealsa PCM is already present, so a
 * route sent within the window can be reverted to local by the player's late init - leaving g_route_mac +
 * g_bt_autorouted latched while the player is actually on analog (no recovery). An EXPLICIT armed flag (not
 * a bare deadline) avoids a false "settling" when lv_tick_get()'s absolute CLOCK_MONOTONIC ms sits in the
 * upper half of the 32-bit range; it disarms itself once the window passes. Set on each reconnect; the
 * guard lives in ui_route_bt so every routing path (poll, connect-completion, tap) defers uniformly. */
static int      g_settle_armed = 0;
static uint32_t g_settle_until = 0;
int ui_player_settling(void){
    if(!g_settle_armed) return 0;
    if((int32_t)(lv_tick_get() - g_settle_until) >= 0){ g_settle_armed = 0; return 0; }   /* window passed -> disarm */
    return 1;
}
static void route_uppercase(const char *in, char *out, int cap){
    int j=0; for(int i=0; in[i] && j<cap-1; i++){ char c=in[i]; if(c>='a'&&c<='z') c-=32; out[j++]=c; } out[j]=0;
}
const char *ui_route_mac(void){ return g_route_mac; }   /* device the player is routed to ('' = local) */

/* Best-effort mirror of the current source mode. diskOS boots the player forced to LOCALPLAYER, so
 * 0 is correct at startup; it is NOT authoritative after a bare mq_ui restart mid-mode, so it is used
 * ONLY to show the picker checkmark, never to block a switch (Local must always be re-issuable). */
/* _Atomic: WRITTEN on the UI thread (ui_set_source_mode) and READ on the coldplug worker thread
 * (coldplug_should_run / coldplug_thread). A plain int here is a C11 data race; the atomic makes the
 * load/store well-defined (seq_cst) without a mutex for this single word. */
static _Atomic int g_source_mode = 0;
int ui_get_source_mode(void){ return g_source_mode; }
int ui_local_playback_allowed(void);
int ui_route_bt(const char *mac){
    if(modes_output_busy()) return -1;                  /* an output switch is in flight or recovering: the auto-route poll retries */
    if(!ui_local_playback_allowed()) return -1;
    if(!mac) return -1;
    char norm[20]; route_uppercase(mac, norm, sizeof norm);
    if((int)strlen(norm) != 17) return -1;             /* must be AA:BB:CC:DD:EE:FF */
    for(int i=0;i<17;i++){
        if((i+1)%3 == 0){ if(norm[i] != ':') return -1; }
        else if(!((norm[i]>='0'&&norm[i]<='9') || (norm[i]>='A'&&norm[i]<='F'))) return -1;
    }
    if(!strcmp(norm, g_route_mac)){ ui_bt_codec_upgrade_arm(norm, 0); return 0; }   /* already routed to this device */
    /* Defer during the player's post-restart settle window: a route sent now could be reverted by the
     * player's late init and leave the caches latched on a stale route. Return "not routed" so no caller
     * latches g_bt_autorouted; the auto-route poll (which keeps firing) routes on the first tick past the
     * window. Guarding HERE covers every path into routing (poll, connect-completion, and a device tap). */
    if(ui_player_settling()) return -1;
    track_state_t st; ipc_get_state(&st);
    int was_playing = g_playing;   /* real play state; raw st.state is unreliable (reports 0 while playing) so
                                    * the old `st.state==1` was false during playback -> pause was SKIPPED */
    if(was_playing && ipc_send_cmd("0201000C0000") < 0) return -1;   /* pause; abort route if it can't be sent (don't switch output unpaused) */
    if(ipc_send_cmd("0666000C0006") < 0) return -1;     /* stock pre-stop: normalize local output state */
    ipc_send_cmd("0642000C0000");                       /* reset network/output mode */
    ipc_send_cmd("0657000C0008");                       /* work-mode 8 (required orchestration, not our work_mode) */
    ipc_send_cmd("06c1000C0000");                       /* initialize the player's BT subsystem */
    if(ipc_send_cmd("0666000C0002") < 0) return -1;     /* out_dev = BT source */
    modes_output_reset();                               /* no longer on SPDIF / USB audio */
    if(atomic_load(&g_source_mode) == 4) g_source_mode = 0;
    ipc_send_cmd("0657000C0008");
    char f[48];
    if(ui_bt_codec_frame(norm, f, sizeof f) < 0) return -1;   /* 06b3 with SBC (VALUE1 0) + MAC payload, as always */
    if(ipc_send_cmd(f) < 0) return -1;
    if(st.volume_seq){ char v[16]; snprintf(v, sizeof v, "0715000C%04X", st.volume); ipc_send_cmd(v); }
    /* Safe minimal resume; deterministic load/play remains a future improvement. */
    if(st.have_track && st.position_ms > 0) ui_seek_to(st.position_ms);  /* resume position, not restart */
    if(was_playing) ipc_send_cmd("0201000C0000");       /* play */
    snprintf(g_route_mac, sizeof g_route_mac, "%s", norm);
    route_state_save(norm);
    g_startup_route_checked = 1;                       /* an intentional BT route supersedes startup recovery */
    ui_bt_codec_upgrade_arm(norm, 1);                      /* AAC/LDAC chosen: switch once BlueALSA has the PCM (no-op for SBC) */
    fprintf(stderr,"route BT %s (playing=%d pos=%ldms)\n", norm, was_playing, st.position_ms); fflush(stderr);
    return 0;
}
static int route_analog(int force){
    if(modes_output_busy()) return -1;
    if(!ui_local_playback_allowed()) return -1;
    if(!force && !g_route_mac[0]){ route_state_remove(); return 0; }
    track_state_t st; ipc_get_state(&st);
    int was_playing = g_playing;   /* real play state (raw st.state reports 0 while playing) */
    if(was_playing && ipc_send_cmd("0201000C0000") < 0) return -1;   /* pause; abort route if it can't be sent (don't switch output unpaused) */
    /* the route commands MUST land; if either fails, keep g_route_mac set + return failure so a
     * retry re-runs - clearing it would falsely say "on analog" and leave the player stuck on BT. */
    if(ipc_send_cmd("0666000C0006") < 0) return -1;     /* out_dev = analog/local DAC */
    if(ipc_send_cmd("0657000C0008") < 0) return -1;
    if(st.have_track && st.position_ms > 0) ui_seek_to(st.position_ms);
    if(was_playing) ipc_send_cmd("0201000C0000");       /* play */
    g_route_mac[0] = 0;
    if(atomic_load(&g_source_mode) == 4) g_source_mode = 0;   /* BT routing reset gadget to 0642 0; return to local analog */
    route_state_remove();
    ui_bt_codec_upgrade_cancel();                       /* a pending codec switch must not outlive this route */
    fprintf(stderr,"route analog (playing=%d)\n", was_playing); fflush(stderr);
    return 0;
}
int ui_route_analog(void){
    if(modes_output_busy()) return -1;
    return route_analog(0);
}

/* Working-mode (audio SOURCE) switch. Replays the stock V2.28 sequences captured + live-verified
 * 2026-08-23: each sets the gadget selector byte (0642) + audio route (0657); the
 * player's state-machine thread reads the 0642 byte edge-triggered and (re)builds the USB gadget.
 * 0=Local 1=USB-DAC 2=BT-Receiving 3=USB-Storage. Pause first so the path isn't reconfigured mid-
 * output; only Local resumes (the others hand audio to the host/BT, where local playback is moot). */

/* SD operations share an admission gate. It closes before an export is queued and
 * reopens only after that export was observed and its return mount is confirmed. */
static int sd_exported_to_host(void);
static int coldplug_mounted(void);
static int rmguard_dir_ok(const char *dir);
static _Atomic int g_sd_writable = 1;
static _Atomic int g_sd_writers = 0;
static _Atomic int g_sd_hold = 0;
/* Player-launch verdict as the UI sees it: -1 = still pending, 0 = launched unprotected/unconfirmed,
 * 1 = this player's launch was confirmed guarded AND locally owned. Atomic because worker threads read
 * it through storage_media_local(). Admission stays CLOSED for anything but 1, which is why this is
 * separate from g_sd_hold: a PENDING verdict must not poison g_sd_phase the way a real hold does. */
static _Atomic int g_launch_verdict = -1;
static pthread_mutex_t g_sd_mode_mu = PTHREAD_MUTEX_INITIALIZER;
#define SD_EXPORT_MARKER "/tmp/.diskos_sd_export_intent"
typedef enum { SD_LOCAL, SD_DRAIN, SD_WAIT_HOST, SD_HOST, SD_WAIT_LOCAL, SD_UNKNOWN } sd_phase_t;
static sd_phase_t g_sd_phase = SD_LOCAL;
static uint32_t g_sd_deadline;
static int g_sd_return_mode;
static int g_sd_reissue;
static void storage_tick(lv_timer_t *t);
static int storage_media_local(void);
static int storage_host_confirmed(void);
int ui_local_playback_allowed(void){
    int m = atomic_load(&g_source_mode);
    if(m != 0 && m != 4) return 0;
    return atomic_load(&g_launch_verdict) == 1 && g_sd_phase == SD_LOCAL && !g_sd_hold && sd_io_healthy();
}

/* SD quiesce for a device Restart: closes BOTH admission paths (sd_io leases and sd_write_begin writers) and keeps
 * storage_tick from reopening them while set (it otherwise resumes a closed-but-local card within ~100 ms). The caller
 * polls ui_sd_quiesce_drained() and either reboots or ends the quiesce, which reopens only a still-local card. */
static _Atomic int g_sd_quiesce = 0;
void ui_sd_quiesce_begin(void){
    atomic_store(&g_sd_quiesce, 1);
    sd_io_hold();
    atomic_store(&g_sd_writable, 0);
}
int ui_sd_quiesce_drained(void){ return sd_io_active() == 0 && atomic_load(&g_sd_writers) == 0; }
void ui_sd_quiesce_end(void){
    atomic_store(&g_sd_quiesce, 0);
    if(g_sd_phase == SD_LOCAL && !g_sd_hold){ atomic_store(&g_sd_writable, 1); sd_io_resume(); }
}
int sd_write_begin(void){
    atomic_fetch_add(&g_sd_writers, 1);
    if(!atomic_load(&g_sd_writable) || sd_exported_to_host()){
        atomic_fetch_sub(&g_sd_writers, 1); return 0;
    }
    return 1;
}
void sd_write_end(void){ atomic_fetch_sub(&g_sd_writers, 1); }
int ui_source_switch_failed(void){ return g_sd_phase == SD_UNKNOWN || g_sd_hold; }
int ui_source_switch_pending(void){
    return g_sd_phase == SD_DRAIN || g_sd_phase == SD_WAIT_HOST || g_sd_phase == SD_WAIT_LOCAL;
}
/* State is recorded BEFORE sending a request, so a UI restart cannot forget a
 * queued export. E/H/R distinguish an unobserved export from a confirmed one. */
static int storage_mark(char phase, int mode){
    char tmp[] = SD_EXPORT_MARKER ".XXXXXX";
    int fd = mkstemp(tmp); if(fd < 0) return 0;
    char b[8]; int n = snprintf(b, sizeof b, "%c %d\n", phase, mode);
    int ok = write(fd, b, (size_t)n) == n && fsync(fd) == 0;
    if(close(fd) != 0) ok = 0;
    if(!ok || rename(tmp, SD_EXPORT_MARKER) != 0){ unlink(tmp); return 0; }
    return 1;
}
/* Verifying our own PATH is not enough: the already-running stock player can
 * have inherited an unprotected environment from an older boot image. */
static int storage_player_guarded(void){
    DIR *d = opendir("/proc"); if(!d) return 0;
    struct dirent *e; int found = 0, ok = 1;
    while(ok && (e = readdir(d))){
        if(e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char p[320], comm[64]; snprintf(p, sizeof p, "/proc/%s/comm", e->d_name);
        FILE *f = fopen(p, "r"); if(!f) continue;
        char *got = fgets(comm, sizeof comm, f); fclose(f);
        if(!got || strcmp(comm, "mq_player\n")) continue;
        found++;
        snprintf(p, sizeof p, "/proc/%s/environ", e->d_name);
        f = fopen(p, "r"); if(!f){ ok = 0; break; }
        char env[8192]; size_t n = fread(env, 1, sizeof env - 1, f);
        int err = ferror(f); fclose(f); env[n] = 0;
        if(err || n == sizeof env - 1){ ok = 0; break; }
        int guarded = 0;
        for(size_t i = 0; i < n; i += strlen(env + i) + 1){
            if(strncmp(env + i, "PATH=", 5)) continue;
            char *path = env + i + 5, *sep = strchr(path, ':');
            if(sep) *sep = 0;
            guarded = path[0] == '/' && rmguard_dir_ok(path);
            break;
        }
        if(!guarded) ok = 0;
    }
    closedir(d);
    return found > 0 && ok;
}
static int source_send(int mode){
    modes_output_reset();                                /* every source change starts from the internal DAC route */
    switch(mode){
        case 0:
            if(ipc_send_cmd("0666000C0006") < 0) return -1;
            if(ipc_send_cmd("0642000C0000") < 0) return -1;
            return ipc_send_cmd("0657000C0008");
        case 1:
            if(ipc_send_cmd("0666000C0006") < 0) return -1;
            if(ipc_send_cmd("0642000C0002") < 0) return -1;
            return ipc_send_cmd("0657000C0008");
        case 2:
            if(ipc_send_cmd("0666000C0006") < 0) return -1;
            if(ipc_send_cmd("0818000C0000") < 0 || ipc_send_cmd("0642000C0000") < 0) return -1;
            return ipc_send_cmd("0657000C0006");
        case 3:
            if(ipc_send_cmd("0666000C0006") < 0) return -1;
            if(ipc_send_cmd("0642000C0001") < 0) return -1;
            return ipc_send_cmd("0657000C0008");
        case 4:
            if(ipc_send_cmd("0666000C0003") < 0) return -1;
            if(ipc_send_cmd("0642000C0005") < 0) return -1;
            return ipc_send_cmd("0657000C0008");
    }
    return -1;
}
static void storage_unknown(const char *why){
    sd_io_hold(); atomic_store(&g_sd_writable, 0); g_sd_phase = SD_UNKNOWN;
    fprintf(stderr, "storage: %s; SD access remains held\n", why); fflush(stderr);
    ui_toast("Storage state uncertain - reboot device");
}
int ui_set_source_mode(int mode){
    if(modes_output_busy()){ ui_toast("Switching output - try again"); return -1; }
    if(mode < 0 || mode > 4) return -1;
    if(mode == 4 && !fw_has_usb_audio_out()){ ui_toast("USB Audio not supported on this firmware"); return -1; }
    if(ui_source_switch_pending()){ ui_toast("Storage is switching"); return -1; }
    if(g_sd_phase == SD_UNKNOWN || g_sd_hold || !sd_io_healthy()){ ui_toast("SD access is held this boot"); return -1; }
    /* Every source change drives the player (and USB/card ownership with it), so none may happen before this
     * boot's launch verdict has qualified the player - a PENDING or negative verdict refuses it too. */
    if(atomic_load(&g_launch_verdict) != 1){ ui_toast("SD access is not ready yet"); return -1; }
    if(mode == 3){
        if(g_sd_phase == SD_HOST) return 0;
        if(scanner_active()){ ui_toast("Let the library scan finish first"); return -1; }
        if(!storage_player_guarded()){ ui_toast("Storage protection is unavailable"); return -1; }
        if(!storage_media_local()){ ui_toast("SD card is not ready"); return -1; }
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200000000L; if(ts.tv_nsec >= 1000000000L){ ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        if(pthread_mutex_timedlock(&g_sd_mode_mu, &ts) != 0){ ui_toast("SD card is busy - try again"); return -1; }
        if(!storage_mark('D', 3)){ pthread_mutex_unlock(&g_sd_mode_mu); ui_toast("Couldn't prepare USB storage"); return -1; }
        sd_io_hold(); atomic_store(&g_sd_writable, 0); g_source_mode = 3;
        g_sd_phase = SD_DRAIN; g_sd_deadline = lv_tick_get() + 20000;
        art_cancel();
        pthread_mutex_unlock(&g_sd_mode_mu);
        return 0;
    }
    if(g_sd_phase == SD_HOST){
        if(!storage_mark('R', mode)){ ui_toast("Couldn't leave USB storage"); return -1; }
        g_sd_return_mode = mode; g_sd_phase = SD_WAIT_LOCAL;
        g_sd_deadline = lv_tick_get() + 20000;
        if(source_send(mode) < 0){ storage_unknown("return request failed"); return -1; }
        return 0;
    }
    int was_playing = g_playing;
    if(was_playing && ipc_send_cmd("0201000C0000") < 0){ ui_toast("Player is busy - try again"); return -1; }
    if(source_send(mode) < 0){ ui_toast("Couldn't switch mode"); return -1; }
    g_source_mode = mode;
    if(mode == 0 && was_playing) ipc_send_cmd("0201000C0000");
    return 0;
}
static void storage_resume_local(void){
    if(unlink(SD_EXPORT_MARKER) != 0 && errno != ENOENT){ storage_unknown("cannot clear handoff state"); return; }
    if(!sd_io_resume()){ storage_unknown("local mount changed during return"); return; }
    atomic_store(&g_sd_writable, 1); g_sd_phase = SD_LOCAL; g_source_mode = g_sd_return_mode;
    g_sd_reissue = 0;
    albumwall_prewarm_seed();
    fprintf(stderr, "storage: local mount confirmed; SD readers and artwork cache resumed\n"); fflush(stderr);
}
static void storage_tick(lv_timer_t *t){
    (void)t;
    if(g_sd_hold || atomic_load(&g_sd_quiesce)) return;   /* a Restart is draining the card: never reopen it here */
    if(!sd_io_healthy()){
        if(g_sd_phase != SD_UNKNOWN) storage_unknown("artwork decoder did not exit");
        return;
    }
    if(g_sd_phase == SD_LOCAL){
        if(!sd_io_allowed() && storage_media_local()){
            if(sd_io_resume() && mdb_total_song_count() == 0 && !mdb_load_failed()) scanner_start();
        }
        return;
    }
    if(g_sd_phase == SD_DRAIN){
        if(sd_io_active() == 0 && atomic_load(&g_sd_writers) == 0){
            if(!storage_mark('E', 3)){ storage_unknown("cannot record export request"); return; }
            g_sd_phase = SD_WAIT_HOST; g_sd_deadline = lv_tick_get() + 20000;
            if(g_playing && ipc_send_cmd("0201000C0000") < 0){ storage_unknown("pause request failed"); return; }
            if(source_send(3) < 0) storage_unknown("export request failed");
            return;
        }
        if((int32_t)(lv_tick_get() - g_sd_deadline) >= 0){
            /* No export command was sent. Existing readers must finish before the
             * gate can reopen; never claim a transfer occurred. */
            storage_unknown("SD readers did not finish");
        }
    } else if(g_sd_phase == SD_WAIT_HOST){
        if(storage_host_confirmed() && !coldplug_mounted()){
            if(!storage_mark('H', 3)){ storage_unknown("cannot record completed export"); return; }
            g_sd_phase = SD_HOST;
            g_sd_reissue = 0;
            fprintf(stderr, "storage: export confirmed with no local mount\n"); fflush(stderr);
        } else if((int32_t)(lv_tick_get() - g_sd_deadline) >= 0) storage_unknown("export not confirmed");
        /* A recovered E/R marker must not drive the player before this launch is qualified: the reissue
         * is a storage TRANSITION, so it belongs behind the same gate as every other card access. */
        else if(g_sd_reissue && atomic_load(&g_launch_verdict) == 1 && source_send(3) == 0) g_sd_reissue = 0;
    } else if(g_sd_phase == SD_WAIT_LOCAL){
        if(storage_media_local()){ storage_resume_local(); return; }
        if((int32_t)(lv_tick_get() - g_sd_deadline) >= 0) storage_unknown("return mount not confirmed");
        /* same gate as the recovered export above: a recovered R marker must not drive the player before
         * this launch is qualified */
        else if(g_sd_reissue && atomic_load(&g_launch_verdict) == 1 && source_send(g_sd_return_mode) == 0)
            g_sd_reissue = 0;
    }
}
static void storage_init(void){
    sd_io_init(storage_media_local);
    FILE *f = fopen(SD_EXPORT_MARKER, "r");
    if(f){
        char phase = 0; int mode = 0;
        int n = fscanf(f, "%c %d", &phase, &mode); fclose(f);
        atomic_store(&g_sd_writable, 0);
        if(n == 2 && (phase == 'H' || phase == 'E') && storage_host_confirmed() && !coldplug_mounted()){
            g_sd_phase = storage_mark('H', 3) ? SD_HOST : SD_UNKNOWN; g_source_mode = 3;
        } else if(n == 2 && phase == 'E' && mode == 3){
            g_sd_phase = SD_WAIT_HOST; g_source_mode = 3; g_sd_reissue = 1;
            g_sd_deadline = lv_tick_get() + 20000;
        } else if(n == 2 && phase == 'R' && mode >= 0 && mode <= 4 && mode != 3){
            g_sd_phase = SD_WAIT_LOCAL; g_sd_return_mode = mode; g_source_mode = 3; g_sd_reissue = 1;
            g_sd_deadline = lv_tick_get() + 20000;
        } else if(n == 2 && phase == 'D' && !sd_exported_to_host()){
            /* D is always replaced by E before any export command is sent. */
            if(unlink(SD_EXPORT_MARKER) == 0){ g_sd_phase = SD_LOCAL; atomic_store(&g_sd_writable, 1); }
            else g_sd_phase = SD_UNKNOWN;
        } else g_sd_phase = SD_UNKNOWN;
    } else if(errno != ENOENT || sd_exported_to_host()){
        g_sd_phase = SD_UNKNOWN; atomic_store(&g_sd_writable, 0);
    }
    if(g_sd_hold){ g_sd_phase = SD_UNKNOWN; atomic_store(&g_sd_writable, 0); }
    if(g_sd_phase == SD_LOCAL && !g_sd_hold) sd_io_resume();
}

static int book_session_active(void);   /* fwd: an audiobook is the active playback context (defined with the book statics) */

/* Re-send every MANAGED audio setting (cfg value >= 0). Called once the player is confirmed
 * ready and again on reconnect; unmanaged (-1) settings are left as the player has them. */
/* The play mode the player was last SENT successfully (0..4), -1 = unknown (player reconnect until the re-send, a
 * failed send), -2 = nothing sent yet by THIS UI process. cfg work_mode is the user's PREFERRED mode; this is the
 * applied one, which Up Next needs to pick the right order. Every 0102 send goes through send_play_mode. */
static int g_mode_applied = -2;
static int send_play_mode(int mode){
    char m[16]; snprintf(m, sizeof m, "0102000C%04X", mode & 0xFFFF);
    int rc = ipc_send_cmd(m);
    g_mode_applied = (rc == 0 && mode >= 0 && mode <= 4) ? mode : -1;
    return rc;
}
/* The mode the player is using now, -1 if unknown. A UI restart under a still-running player (crash respawn, deploy)
 * gets no reconnect and so no re-send: the player is then still on the mode the previous UI process sent, which it
 * stored in cfg BEFORE sending. If diskOS has never stored a mode it never sends one, so the player is on its own
 * saved PLAY_MODE. */
int ui_play_mode_now(void){
    if(g_mode_applied >= 0) return g_mode_applied;
    if(book_session_active()) return -1;
    int wm = cfg_get_int("work_mode", -1);
    if(wm < 0) return mdb_sysconfig_play_mode();
    if(g_mode_applied == -2 && wm <= 4) return wm;
    return -1;
}
void ui_reapply_audio(void){
    /* NOTE: the v2.40 local-init (0666 route + 0657 LOCALPLAYER work-mode, which fixes the "g_fiio_local
     * is null" / NO_WORK_MODE start_local failure) is DELIBERATELY NOT sent here. This runs at boot-ready
     * and on reconnect, when a freshly-booted player is still idle-fresh - sending 0666 then either wedges
     * it or doesn't stick (device-verified: work-mode stayed NULL despite this running at cold boot). The
     * local-init is instead handled by the v2.40 work-mode handshake (localplayer_workmode_cb), which uses the
     * player's a607 mode oracle to set + confirm LOCALPLAYER once the player is ready - see there. */
    /* defaults match the device's observed current state (DRE on, Gain low, analog out,
     * Slow-LL filter) so a boot re-apply doesn't change the sound until the user does. */
    g_mode_applied = -1;   /* a fresh or reconnected player: unknown until the mode below is re-sent */
    modes_output_reset();  /* a fresh or reconnected player is on the internal DAC: the SPDIF setting mirrors that */
    if(atomic_load(&g_source_mode) == 4) g_source_mode = 0;   /* reconnected player is back on internal DAC */
    ui_set_dre(cfg_get_int("audio_dre",    1));
    ui_set_gain(cfg_get_int("audio_gain",  0));
    /* Output route (raw 0666) is deliberately NOT reapplied here: this also runs on player
     * RECONNECT, and sending 0666 to a freshly-booted player can wedge it (see the reconnect
     * warning below). Analog is the boot default; SPDIF / USB audio are chosen per player run
     * (modes.c, never persisted), so there is no output route to restore here. */
    ui_set_dac_filter(cfg_get_int("audio_filter", 1));
    ui_set_replay_gain(cfg_get_int("replay_gain", 0));
    /* The rest of the managed settings, reapplied so a player restart/reconnect can't leave the
     * player out of sync with what Settings shows. Each is sent ONLY when
     * a real value is stored - the setters have inconsistent unmanaged handling, so guard here
     * rather than trust their internal clamps (e.g. ui_set_maxvol(-1) would send volume 0). */
    {
        int wm = cfg_get_int("work_mode", -1);
        if(book_session_active()) send_play_mode(4);                      /* an active book must stay in Single mode across a reconnect, not be reset to the music mode */
        else if(wm >= 0 && wm <= 4) ui_set_workmode(wm);                  /* stored play mode; don't let a garbage cfg get rewritten to 0 */
        int mp = cfg_get_int("memory_play", -1); if(mp >= 0)             ui_set_memory(mp);    /* Resume Playback */
        int gp = cfg_get_int("gapless", -1);     if(gp >= 0)             ui_set_gapless(gp);
        int mv = cfg_get_int("max_vol", -1);     if(mv >= 10)            ui_set_maxvol(mv);    /* <10 or unset = skip */
        int bl = cfg_get_int("balance", -100);   if(bl >= -10 && bl<=10) ui_set_balance(bl);   /* -1 is a VALID balance */
    }
}
/* When a play triggers a full list rebuild (seconds), we show "Starting..." and
 * confirm via the next a2 track update (the verified oracle); g_play_pending holds
 * the start tick (0 = nothing pending). Track and stream evidence retire it. */
/* Main loop is the controller's only owner. RX hands off copied evidence. */
static void opus_title_tick(void){
    static opus_title_worker *worker;
    static int attempted, active;
    static uint64_t token=UINT64_MAX;
    ipc_title_event event; ipc_get_title_event(&event);
    if(!attempted){ worker=otw_create(NULL,NULL); attempted=1; }
    if(!worker){
        if(event.state.player_confirmed && !event.state.title_ready)
            ipc_publish_title(&event,event.player_title,1);
        return;
    }
    otw_library_commit(worker,ipc_library_epoch());
    for(unsigned polls=0;;polls++){
        track_state_t *s=&event.state;
        if(token!=s->playback_token){ otw_identity_end(worker); token=s->playback_token; active=0; }
        otw_player_generation(worker,s->player_generation);
        if(s->player_confirmed && (active || !s->title_ready)){
            active=1;
            unsigned flags=((s->is_cue_valid && s->is_sacd_valid) ||
                (s->is_cue_valid && s->is_cue) || (s->is_sacd_valid && s->is_sacd) ? OTW_KIND_KNOWN : 0) |
                (s->is_cue_valid && s->is_cue ? OTW_CUE : 0) |
                (s->is_sacd_valid && s->is_sacd ? OTW_SACD : 0);
            otw_identity id={s->player_generation,s->song_id_raw,s->pos_id_raw,s->song_id_valid,s->pos_id_valid};
            otw_submit(worker,id,s->path,event.player_title,flags);
        }
        /* Bound retries under sustained RX changes, after reconciling the latest evidence. */
        if(polls==2) return;
        struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
        otw_commit commit;
        if(!otw_poll(worker,(uint64_t)now.tv_sec*1000+now.tv_nsec/1000000,&commit)) return;
        int freeze=commit.result.outcome!=OPUS_TITLE_NOT_APPLICABLE;
        if(ipc_publish_title(&event,commit.result.title,freeze)){
            if(!freeze) active=0;
            return;
        }
        /* IPC copied newer evidence on rejection. No controller call spans its lock. */
        otw_reject_commit(worker);
    }
}
static uint32_t g_play_pending = 0;
/* Playback-start monitor: metadata is progress; raw a2 state is not play proof. */
#ifndef PLAY_PCM_GLOB
#define PLAY_PCM_GLOB "/proc/asound/card*/pcm*p/sub*/status"
#endif
static int play_pcm_running(void){
    glob_t paths;
    if(glob(PLAY_PCM_GLOB, 0, NULL, &paths) != 0){
        globfree(&paths);
        return 0;
    }
    int running = 0;
    for(size_t i = 0; i < paths.gl_pathc && i < 16 && !running; i++){
        char buf[512];
        int fd = open(paths.gl_pathv[i],
                      O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if(fd < 0) continue;
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if(n > 0){
            buf[n] = 0;
            running = strstr(buf, "state: RUNNING") != NULL;
        }
    }
    globfree(&paths);
    return running;
}

/* Track fields are copied before the send. The transport epoch includes
 * the successfully queued request. Serial distinguishes requests at one tick. */
static struct {
    uint32_t request, at;
    unsigned serial, pos_seq, generation, transport;
    uint64_t token;
    long position, duration;
    int have_track, playing, provisional;
} g_play_pre;

static void play_request_arm(const track_state_t *cur, int was_playing){
    g_play_pending = lv_tick_get();
    if(!g_play_pending) g_play_pending = 1;

    g_play_pre.serial++;
    g_play_pre.request = g_play_pre.at = g_play_pending;
    g_play_pre.pos_seq = cur->pos_seq;
    g_play_pre.position = cur->position_ms;
    g_play_pre.generation = cur->player_generation;
    g_play_pre.transport = ipc_transport_seq();
    g_play_pre.token = cur->playback_token;
    g_play_pre.duration = cur->duration_ms;
    g_play_pre.have_track = cur->have_track;
    g_play_pre.playing = was_playing;
    g_play_pre.provisional = cur->provisional;
}

/* Routine observations are at least 500 ms apart. A terminal decision
 * may force one fresh observation, without duplicating a read at this tick. */
static int play_pcm_seen(uint32_t now, int fresh){
    static uint32_t at;
    static int valid, running;
    if(!valid || now - at >= 500 || (fresh && at != now)){
        running = play_pcm_running();
        at = now;
        valid = 1;
    }
    return running;
}

/* Only a track/restart transition attributes advances to this request.
 * A stopped snapshot near zero is ambiguous: preserve possible start
 * evidence to avoid a late error, but never use it to record health. */
static int play_start_failed(const track_state_t *st){
    static uint32_t request, progress, pos_at, adv_at;
    static unsigned serial, pos_seq, generation, transport;
    static uint64_t token;
    static long position, duration;
    static int active, had_track, seed, advances;
    static int attrib, started, possible, may_start, seen;

    uint32_t now = lv_tick_get();
    if(!g_play_pending){
        active = 0;
        return 0;
    }
    if(!active || request != g_play_pending ||
       serial != g_play_pre.serial){
        active = 1;
        request = g_play_pending;
        serial = g_play_pre.serial;
        progress = request;
        advances = attrib = started = possible = seen = 0;
        may_start = seed = 0;

        if(g_play_pre.request == request){
            pos_at = g_play_pre.at;
            pos_seq = g_play_pre.pos_seq;
            position = g_play_pre.position;
            generation = g_play_pre.generation;
            transport = g_play_pre.transport;
            token = g_play_pre.token;
            duration = g_play_pre.duration;
            had_track = g_play_pre.have_track;
            seed = g_play_pre.provisional;
            may_start = !g_play_pre.playing &&
                        (!had_track || position <= 1500);
        } else {
            pos_at = now;
            pos_seq = st->pos_seq;
            position = st->position_ms;
            generation = st->player_generation;
            transport = ipc_transport_seq();
            token = st->playback_token;
            duration = st->duration_ms;
            had_track = st->have_track;
        }
    }

    /* A new player or another transport attempt supersedes this request. */
    if(generation != st->player_generation ||
       transport != ipc_transport_seq()){
        g_play_pending = 0;
        active = 0;
        return 0;
    }

    if(seed && st->player_confirmed){
        /* First real metadata corrects a presentation seed, not a start. */
        seed = 0;
        attrib = 0;
        advances = 0;
        progress = now;
        token = st->playback_token;
        duration = st->duration_ms;
        had_track = st->have_track;
        pos_seq = st->pos_seq;
        position = st->position_ms;
        pos_at = now;
    } else if(token != st->playback_token ||
              had_track != st->have_track){
        progress = now;
        advances = 0;
        attrib = !seed && st->have_track &&
                 st->player_confirmed && !st->provisional;
        token = st->playback_token;
        duration = st->duration_ms;
        had_track = st->have_track;
        pos_seq = st->pos_seq;
        position = st->position_ms;
        pos_at = now;
    } else if(duration != st->duration_ms){
        /* Metadata progress buys time but establishes no attribution. */
        progress = now;
        advances = 0;
        duration = st->duration_ms;
        pos_seq = st->pos_seq;
        position = st->position_ms;
        pos_at = now;
    }

    if(st->pos_seq != pos_seq){
        long long delta = (long long)st->position_ms - position;
        uint32_t gap = now - pos_at;
        long long slack = (long long)gap + 1500;
        int fresh = st->have_track && st->player_confirmed &&
                    !st->provisional &&
                    (int32_t)(st->pos_seq - st->path_seq) > 0;
        int wrap = delta < 0 && duration > 0 &&
                   position >= 0 && position <= duration &&
                   st->position_ms >= 0 &&
                   st->position_ms <= duration &&
                   (long long)duration - position +
                       st->position_ms <= slack;

        if(fresh && delta != 0) progress = now;
        if(fresh && ((delta > 0 && delta <= slack) || wrap)){
            advances++;
            seen = 1;
            adv_at = now;
            if(attrib) started = 1;
            else if(may_start) possible = 1;
        } else {
            advances = 0;
            if(fresh && !wrap && delta < -1500)
                attrib = 1;
        }

        pos_seq = st->pos_seq;
        position = st->position_ms;
        pos_at = now;
        if(advances >= 2 && (attrib || possible)){
            g_play_pending = 0;
            if(attrib){
                void system_ota_note_playback(void);
                system_ota_note_playback();
            }
            active = 0;
            return 0;
        }
    }

    /* Unobserved rebuild: 15s. Progress: 6s quiet window. Hard limit: 45s. */
    uint32_t age = now - request;
    if(age < 45000 && (age < 15000 || now - progress < 6000))
        return 0;

    /* Established or ambiguous brief starts must not produce a late error.
     * Neither branch records health here. */
    if(started || possible){
        g_play_pending = 0;
        active = 0;
        return 0;
    }

    int final = age >= 45000;
    int out = seen && now - adv_at < 6000;
    if(!out) out = play_pcm_seen(now, final);
    if(!out && !final) out = play_pcm_seen(now, 1);
    if(out){
        if(final){
            g_play_pending = 0;
            active = 0;
        }
        return 0;
    }
    return 1;
}
/* End playback-start monitor. */
/* Startup route recovery and V2.40 LOCALPLAYER work-mode one-shot. The latter's mechanism (RE'd from mq_player_v240):
 *  - v2.40's player needs 0666(local route)+0657(LOCALPLAYER work-mode) set at runtime or local
 *    start_local fails ("work mode NULL"/NO_WORK_MODE -> g_fiio_local null). Protocol 0657=8 maps to
 *    internal work-mode 1 (LOCALPLAYER); 0 = NO_WORK_MODE.
 *  - WHY an early 0657 doesn't stick: an async mode-control thread (player 0x4dca64) keeps running AFTER
 *    the /player command server goes live. It runs ~50x100ms polls + up to ~2s of 1s sleeps (~7s worst
 *    case), calling close_player() (resets work-mode to 0) and re-initing per late hardware state - which
 *    clobbers any 0657 sent inside that window. So we wait past it (~8s guard; we use 9s) before sending.
 *  - WHY there is no a607 oracle on /ui: the 0607 query handler writes its a607 reply to a SEPARATE
 *    fd-based transport (mask 0, raw write to fd 0x83e5f4), never the /ui mqueue - so we can never see it.
 *    There is NO deterministic /ui frame meaning "late init finished". a1=needs playback, a2=server
 *    liveness only, a714=volume. The best anchor is: solicit a2 with 02020008, then a fixed settle delay.
 *  - IDLE PLAYER IS SILENT: a fresh cold-boot player emits NOTHING unsolicited, so ipc_rx_frames() stays
 *    0 forever unless we poke it. We therefore actively send 02020008 every tick until it answers (a2),
 *    anchor the settle timer on that first response. The V2.40/V2.57 local init then sends 0666+0657
 *    once per player generation; the saved-BT-route check runs only once per UI process. Both require
 *    Local source + analog output intent (g_route_mac empty), and wait while a play is pending. */
static unsigned g_play_sent_gen = 0xFFFFFFFFu;   /* ipc generation in which a play command was last sent: that generation's one-shot is moot */
static uint32_t g_play_sent_at;
static unsigned g_play_track_seen_gen = 0xFFFFFFFFu;
void ui_note_transport_sent(void){ g_play_sent_gen = ipc_generation(); g_play_sent_at = lv_tick_get(); }
/* A sent play can precede its first track reply. A loaded track may still be paused,
 * so wait for the player to unload it and for the last play evidence to age out. */
static int startup_playback_busy(unsigned gen){
    if(g_playing) g_play_track_seen_gen = gen;
    if(g_play_pending || g_playing || book_session_active()) return 1;
    if(g_play_sent_gen != gen) return 0;
    track_state_t st; ipc_get_state(&st);
    if(st.have_track) g_play_track_seen_gen = gen;
    return st.have_track || g_play_track_seen_gen != gen || lv_tick_elaps(g_play_sent_at) < 10000;
}
/* Read the live player's age once per generation. An old idle player need not answer
 * the track probe before a route recovery; a young player still uses the reply/settle path. */
static int startup_player_old(void){
    long pid, hz = sysconf(_SC_CLK_TCK);
    unsigned long long start;
    if(hz <= 0 || route_player_identity(&pid, &start) < 0) return 0;
    char path[sizeof ROUTE_PROC_ROOT + 16];
    if(snprintf(path, sizeof path, "%s/uptime", ROUTE_PROC_ROOT) >= (int)sizeof path) return 0;
    FILE *f = fopen(path, "r");
    if(!f) return 0;
    double uptime = 0;
    int ok = fscanf(f, "%lf", &uptime) == 1;
    fclose(f);
    return ok && uptime >= 0 && uptime - (double)start / (double)hz >= 12.0;
}
/* 0666 updates the player's live output first; its saved OUT_DEV is written by the
 * player's bulk config save. There is no verified /ui reply for the live out_dev.
 * A saved BT route is therefore a conservative startup recovery hint. */
static int startup_saved_bt_route(void){
    char buf[32] = {0};
    ui_run_cap_bounded("sqlite3 /usr/data/fiio/db/sysconfig.db 'SELECT OUT_DEV FROM SYSCONFIG WHERE ID=1' 2>/dev/null",
                       buf, sizeof buf, 250);
    if(!strcmp(buf, "2\n") || !strcmp(buf, "2")) return 1;
    if(!strcmp(buf, "6\n") || !strcmp(buf, "6") || !strcmp(buf, "4\n") || !strcmp(buf, "4")) return 0;
    return -1;   /* absent, locked, malformed or unreadable: do not infer BT */
}
/* Same A2DP PCM evidence as bt.c's auto-route poll. The completion marker makes
 * an empty successful list distinct from a failed or timed-out bluealsa probe. */
static int startup_connected_sink(const char *mac){
    if(cfg_get_int("bt_on", 0) != 1) return 0;   /* Bluetooth is off by user intent: no auto-route */
    char buf[2048] = {0};
    ui_run_cap_bounded("if ! pidof bluealsa >/dev/null 2>&1; then echo DISKOS_NO_BLUEALSA; "
                       "else bluealsa-cli list-pcms 2>/dev/null && printf '\\nDISKOS_LIST_DONE\\n'; fi",
                       buf, sizeof buf, 250);
    if(strstr(buf, "DISKOS_NO_BLUEALSA\n")) return 0;
    if(!strstr(buf, "DISKOS_LIST_DONE\n")) return -1;
    if(!mac || !mac[0]) return strstr(buf, "a2dpsrc") != NULL;
    char needle[32]; snprintf(needle, sizeof needle, "dev_%s/a2dpsrc", mac);
    for(char *p=needle; *p; p++) if(*p == ':') *p = '_';
    route_uppercase(needle, needle, sizeof needle);
    route_uppercase(buf, buf, sizeof buf);
    return strstr(buf, needle) != NULL;
}
/* 1 = this generation needs no other local init, 0 = normal init may continue,
 * -1 = evidence or a route send is pending. An inconclusive sink probe waits
 * at least five seconds before another shell probe on this generation. */
static int startup_route_reconcile(void){
    static unsigned sink_retry_gen = 0xFFFFFFFFu;
    static uint32_t sink_retry_at = 0;
    if(g_startup_route_checked) return 0;
    unsigned gen = ipc_generation();
    if(modes_output_route() != OUT_INTERNAL || cfg_get_int("spdif", 0)){
        g_startup_route_checked = 1; return 0;
    }
    if(sink_retry_gen == gen && lv_tick_elaps(sink_retry_at) < 5000) return -1;
    /* A route from this live player is newer than its bulk-saved OUT_DEV. */
    if(!g_route_mac[0]){
        int saved_bt = startup_saved_bt_route();
        if(saved_bt <= 0){ g_startup_route_checked = 1; return 0; }
    }
    int sink = startup_connected_sink(g_route_mac);
    if(sink < 0){ sink_retry_gen = gen; sink_retry_at = lv_tick_get(); return -1; }
    if(sink){ g_startup_route_checked = 1; return 1; }   /* the auto-route poll owns this speaker */
    if(route_analog(1) < 0) return -1;
    g_startup_route_checked = 1;
    return 1;   /* route_analog logs the single recovery line */
}
static void localplayer_workmode_cb(lv_timer_t *t){
    /* These commands drive the player and its USB/card ownership, so they need the same admission as any
     * other card access: a qualified launch verdict, SD not held, no decoder fault. A pending verdict waits
     * (the timer retries), it is never a reason to go ahead. */
    if(!ui_local_playback_allowed()) return;
    static unsigned done_gen = 0xFFFFFFFFu;   /* generation whose one-shot we've already sent */
    static unsigned wait_gen = 0xFFFFFFFFu;   /* generation whose settle timer is running */
    static uint32_t wait_start = 0;
    static unsigned rx_gen  = 0xFFFFFFFFu;    /* generation the rx_base snapshot belongs to */
    static unsigned rx_base = 0;              /* cumulative rx_frames at the start of THIS generation */
    static unsigned age_gen = 0xFFFFFFFFu;
    static int player_old = 0;
    if(ui_get_source_mode() != 0) return;                     /* only while Local source */
    if(modes_output_busy()) return;                            /* an output switch owns 0666/0642/0657 right now */
    unsigned gen = ipc_generation();
    if(age_gen != gen){ age_gen = gen; player_old = !g_startup_route_checked && startup_player_old(); }
    if(!g_startup_route_checked && player_old && !startup_playback_busy(gen)){
        int startup = startup_route_reconcile();
        if(startup > 0) done_gen = gen;
        if(startup > 0 && !fw_needs_localplayer_init()){
            lv_timer_del(t); return;
        }
    }
    if(g_play_pending || book_session_active()) return;
    if(gen == done_gen) return;
    unsigned rx  = ipc_rx_frames();           /* CUMULATIVE across generations -> anchor per-generation via rx_base */
    if(rx_gen != gen){ rx_gen = gen; rx_base = rx; }           /* new generation -> snapshot the count */
    if(rx == rx_base){ ipc_send_probe("02020008"); return; }   /* SOLICIT: THIS generation's player hasn't answered yet (idle player is silent) */
    uint32_t now = lv_tick_get();
    if(wait_gen != gen){ wait_gen = gen; wait_start = now; return; }   /* first response this gen -> start settle */
    if(now - wait_start < 9000) return;                        /* settle past the ~7s mode-control-thread window */
    if(!g_startup_route_checked && startup_playback_busy(gen)) return;
    int startup = startup_route_reconcile();
    if(startup < 0) return;
    if(g_route_mac[0]){ done_gen = gen; if(!fw_needs_localplayer_init()) lv_timer_del(t); return; }
    if(!fw_needs_localplayer_init()){ lv_timer_del(t); return; }
    if(startup > 0){ done_gen = gen; return; }
    if(g_play_sent_gen == gen){                                /* a play was started this generation (the play path sends its own preamble while stopped): g_playing is only inferred, so never risk 0666 mid-stream */
        done_gen = gen;
        fprintf(stderr,"v2.40 workmode: local-init skipped, play already sent (gen %u)\n", gen); fflush(stderr);
        return;
    }
    /* Already PLAYING locally (the user started a song inside the settle window): the player has a working local
     * route and work-mode and the card isn't exported - exactly the state this one-shot exists to establish - and
     * 0666 would close the stream mid-song (seen live on V2.57). Count this generation as initialised. */
    if(g_playing){
        done_gen = gen;
        fprintf(stderr,"v2.40 workmode: local-init skipped, already playing (gen %u)\n", gen); fflush(stderr);
        return;
    }
    /* USB gadget selector -> Local/no-export: tears down any mass-storage the stock player
     * auto-bound from a persisted WORK_MODE=4 (recovers hand-deploys + belt-and-suspenders for
     * the flashed boot gate). Sent past the settle window so the async mode-control thread can't
     * clobber it. If the send fails, DON'T mark this generation done - retry next tick rather than
     * leave the card exported. */
    if(modes_local_init(1) < 0) return;   /* 0642 gadget (0 = none, 5 = USB host), 0666 local route (6 internal, 4 SPDIF, 3 USB audio: (re)inits the local device, creates g_fiio_local), 0657 8 LOCALPLAYER work-mode (fixes NO_WORK_MODE) */
    done_gen = gen;   /* only mark this generation initialised once ALL three commands were actually sent */
    fprintf(stderr,"v2.40 workmode: local-init sent once (gen %u)\n", gen); fflush(stderr);
}
void ui_rescan_library(void){
    if(scanner_active()){ if(scanner_cancel()) ui_toast("Cancelling scan..."); return; }   /* refused once it is committing: completion reports */   /* the row reads "Stop" while a scan runs */
    if(!ui_local_playback_allowed()){ ui_toast("Return to local playback to scan"); return; }
    if(!sd_io_allowed()){ ui_toast("SD card is not ready"); return; }
    if(scanner_start() != 0){ ui_toast("Couldn't start library scan"); return; }
    ui_toast("Scanning library...");
}
/* Poll for scan completion: reload the library from the rebuilt DB, refresh the view, toast. */
/* 1 once every .m4b has been moved out of SONG into BOOKS. Until then a music play (which the stock player
 * builds server-side from the unfiltered SONG table) could queue a book, so the play path gates on this. */
static int g_books_migrated = 0;
static int books_ensure_migrated(void){
    if(g_books_migrated) return 1;
    if(mdb_migrate_books()){ g_books_migrated = 1; return 1; }   /* idempotent: a no-op when SONG has no .m4b */
    return 0;
}
/* Retry book migration off the main loop until it succeeds, then stop. mdb_migrate_books can fail on a
 * transient reader lock at startup; without a retry a .m4b left in SONG would keep leaking into the stock
 * player's music queue for the whole session. Idempotent, so re-running once the lock clears is safe. */
static void migrate_books_retry_cb(lv_timer_t *t){
    if(books_ensure_migrated()) lv_timer_del(t);   /* 1 = clean or committed -> done */
}
/* Port playlists on upgrade (e.g. a pre-1.2.0 diskOS library on V2.57): stock's player creates its own playlist index
 * (placeholder names "custom list <id>", missing later diskOS playlists) when it first starts, possibly after the UI. So
 * reconcile from boot, retrying until the index exists and agrees - never waiting for the user to open Playlists.
 * Bounded: 5 s apart for 5 minutes; every playlist listing syncs again anyway. */
static void playlist_port_cb(lv_timer_t *t){
    static int tries;
    if(mdb_playlist_sync_registry() == 2 || ++tries >= 60) lv_timer_del(t);
}
static void scanner_poll(lv_timer_t *t){
    (void)t;
    if(scanner_take_finished()){
        int oc = scanner_outcome();
        /* Only a committed scan changed song.db. Test the commit latch, not the outcome: a scan started before this
         * poll resets the outcome, and its cancel or failure must not hide the earlier commit. Reload anyway when the
         * last load failed, so a scan's end still recovers. */
        if(scanner_take_committed() || mdb_load_failed()){
            mdb_load();               /* reload the in-memory library (also invalidates the group caches) */
            library_ensure_capacity(); /* grow row buffers if a clean first-boot 1-song alloc just gained thousands */
            library_refresh();        /* rebuild whatever Library view is showing */
            albumwall_prewarm_seed(); /* queue covers for any newly-scanned albums (no Album-view visit required) */
        }
        int done=0, total=0; scanner_progress(&done, &total);
        int skipped = scanner_skipped();
        int unsup = scanner_unsupported();   /* AAC/M4A/OGG/... present but not indexable yet */
        char b[80];
        if(mdb_load_failed()) snprintf(b, sizeof b, "Library busy - reopen to refresh");  /* DB error on RELOAD (busy/IO): the shown list is stale/empty, so don't claim the scan's count */
        else if(oc == SCAN_OUT_CANCELLED) snprintf(b, sizeof b, "Scan cancelled");   /* rolled back: the library is exactly as it was */
        else if(oc == SCAN_OUT_NO_SD)  snprintf(b, sizeof b, "Insert an SD card to scan");    /* SD not mounted; library kept */
        else if(oc == SCAN_OUT_FAILED) snprintf(b, sizeof b, "Scan failed - library kept");   /* before any count branch: a failure with 0 files counted is not "No music found" */
        else if(total>0 && skipped>0) snprintf(b, sizeof b, "Scanned %d song%s; %d files skipped", total, total==1?"":"s", skipped);
        else if(total>0 && unsup>0)
                             snprintf(b, sizeof b, "Scanned %d song%s (%d unsupported)", total, total==1?"":"s", unsup);
        else if(total>0)     snprintf(b, sizeof b, "Scanned %d song%s", total, total==1?"":"s");
        else if(done>0)      snprintf(b, sizeof b, "Scan failed - library kept");   /* rolled back */
        else if(skipped>0)   snprintf(b, sizeof b, "Couldn't read %d files - library kept", skipped);
        else if(unsup>0)     snprintf(b, sizeof b, "No music found (%d unsupported file%s)", unsup, unsup==1?"":"s");
        else                 snprintf(b, sizeof b, "No music found");
        ui_toast(b);
    }
}
static unsigned g_play_last_gen = 0xFFFFFFFFu; static uint32_t g_play_last_tick = 0;   /* generation + tick of the last play command we sent */
/* Is a previous play still starting? g_playing lags the stream by seconds and 0666 under it releases the local output. */
static int play_recent(void){ return g_play_pending || (g_play_last_gen == ipc_generation() && lv_tick_elaps(g_play_last_tick) < 10000); }
static void play_note_sent(void){ g_play_last_gen = ipc_generation(); g_play_last_tick = lv_tick_get(); }
/* Authoritative play state (the main loop normalizes the unreliable raw st.state into g_playing:
 * 1=playing while position advances, else 0). Exposed so lastfm/route logic share ONE source of
 * truth instead of re-deriving play/pause from position deltas. */
int ui_is_playing(void){ return g_playing; }
/* Admission for an output-route switch (modes.c): NULL = go, else the toast reason. 0666 re-inits the output, so never
 * during a start-up, a book, another source, a BT route or the post-restart settle. */
const char *ui_output_blocked(void){
    if(!ui_local_playback_allowed()) return "SD card is not ready";
    if(ui_get_source_mode() != 0) return "Return to local playback first";
    if(g_route_mac[0]) return "Bluetooth output is active";
    if(ui_player_settling()) return "Player is starting - try again";
    if(play_recent()) return "Player is busy - try again";
    if(book_session_active()) return "Stop the audiobook first";
    return NULL;
}

/* Optimistic play/pause icon hint. Play state is INFERRED from position advance with a grace period,
 * so the transport glyph lags ~1s when PAUSING (the position stops but the inference waits before it
 * flips to paused); resuming is instant because the position advances immediately. On a play/pause tap
 * we predict the new state and show it at once; the real inference reconciles a beat later. Main-thread
 * only (taps + the glyph refresh both run on the LVGL loop), so no locking. */
static int      g_pp_hint = -1;          /* -1 none; 0 predict paused; 1 predict playing */
static uint32_t g_pp_hint_until = 0;
void ui_pp_tap_hint(void){               /* a toggle tap -> predict the opposite of the current state */
    g_pp_hint = ui_pp_icon_playing(g_playing) ? 0 : 1;
    g_pp_hint_until = lv_tick_get() + 2200; /* exceeds the 1600ms position-inference grace */
}
int ui_pp_icon_playing(int real_playing){
    if(g_pp_hint >= 0){
        if((int32_t)(lv_tick_get() - g_pp_hint_until) >= 0){ g_pp_hint = -1; }        /* window lapsed -> trust real */
        else return g_pp_hint;                                        /* still lagging -> show prediction */
    }
    return real_playing;
}
/* set absolute volume 0..120 via the decoded 0715 command (class-2:
 * 0715 + LEN(000C) + <level 4hex>).  The player maps this through the same
 * cs43131 gain path as the hardware vol keys. */
int ui_set_volume(int vol){
    if(vol < 0) vol = 0; if(vol > VOL_MAX) vol = VOL_MAX;
    char f[16]; snprintf(f, sizeof f, "0715000C%04X", vol);
    int rc = ipc_send_cmd(f);
    fprintf(stderr,"set volume %d -> %s (rc=%d)\n", vol, f, rc); fflush(stderr);
    return rc;   /* 0 = queued OK, -1 = send failed (caller can retry) */
}
/* Play a library list. The player builds the list itself (drop LIST_SONG_0 then
 * INSERT...SELECT FROM SONG WHERE <type filter>) then starts at the given
 * 1-based track. Every play sends the build frame (measured 0.1-1.0 s on the Disc): the old type-0 "play position
 * in current list" shortcut is gone because the player can replace its queue on its own (Play Through Folders)
 * and diskOS cannot prove which list is loaded. */
void ui_disarm_book_eoc(void);   /* fwd: defined with the sleep statics below (also in screens.h) */
static int g_book_single_mode = 0;  /* 1 while a book has forced Single play-mode; the next music play restores the user's configured mode */
static uint32_t g_book_noadopt_until = 0;  /* after an explicit play, don't let book_tick adopt the (possibly still-reported) old book during the transition */
/* build_idx: the 0100 start index sent when the list is (re)built; -1 = the 0-based position (pos1-1). A jump
 * inside the already-loaded list always uses pos1. */
/* Returns 1 if the play frame was sent, 0 if it was refused or the send failed. */
static int ui_play_list_ex(int list_type, const char *name, int pos1, int build_idx){
    if(modes_output_busy()){ ui_toast("Switching output - try again"); return 0; }   /* an output switch (0666 re-init) is in flight or recovering */
    if(!ui_local_playback_allowed()){ ui_toast("Return to local playback first"); return 0; }
    if(ui_get_source_mode() == 4 && !ui_usb_dac_connected()){
        ui_toast("USB DAC not connected"); return 0;
    }
    /* A music list play (not a custom playlist, type 5) makes the stock player build its queue from the
     * UNFILTERED SONG table. If a .m4b hasn't migrated out yet, it would leak into that queue - so ensure
     * migration first, and refuse the play (rather than queue a book) if it still can't complete. */
    if(list_type != 5 && !books_ensure_migrated()){ ui_toast("Library is finishing setup - try again"); return 0; }
    ui_cancel_book_resume();   /* a new list play supersedes any pending audiobook resume seek */
    ui_disarm_book_eoc();      /* explicit navigation disarms any end-of-chapter sleep (so a later path change can only be an auto-rollover) */
    g_book_noadopt_until = lv_tick_get() + 4000;   /* the player may still report the OLD book while this play loads -> don't adopt it */
    if(list_type != 5 && g_book_single_mode){   /* leaving book playback for music -> restore the user's play-mode (a book forced Single) */
        send_play_mode(cfg_get_int("work_mode", 0));
        g_book_single_mode = 0;
    }
    if(pos1 < 1) pos1 = 1;
    if(!name) name = "";
    if(strlen(name) >= 380){ ui_toast("Couldn't play that list"); return 0; }   /* the frame must hold the WHOLE name (a type-7 artist+album name is up to ~2x160 chars) */
    char f[420];
    int sent = 0;
    ui_note_transport_sent();
    /* V2.40 local-play preamble (live-confirmed on device 2026-09-01). The player's mode-control
     * thread drifts the output route to BT (out_dev=2) and resets the work-mode to NULL when idle,
     * so a play from stopped fails: it hangs ~15s trying to open a BT PCM ("Failed to get BT device
     * MAC"), or reports "work mode NULL". Re-assert the stock local-play preamble TIGHT before the
     * play command - local output route, then LOCALPLAYER work-mode - exactly as stock mq_ui does on
     * a local-play transition (the player processes /player frames serially, so 0666's close_player
     * runs, 0657 re-sets the mode, then 0100 plays with output=local + mode=LOCALPLAYER). Gated:
     * v2.40 only (V2.09/V2.28 don't drift, and 0666's close_player could disturb their working path);
     * Local source + analog output only (never a BT A2DP route); and only while STOPPED (an active
     * track-jump already has the route+mode correct, and 0666 would gap the audio). */
    /* Not while a previous play is still starting (g_playing lags the stream by seconds): 0666 releases the local
     * output under it (seen live). Only the idle / long-gap case needs the preamble. */
    int recent_play = play_recent();
    if(fw_needs_localplayer_init() && !g_route_mac[0] && !g_playing && !recent_play){
        if(ui_get_source_mode() == 0){
            modes_local_init(0);   /* 0666 local route (6 internal, 4 SPDIF), then 0657 8 LOCALPLAYER work-mode */
        } else if(ui_get_source_mode() == 4){
            ipc_send_cmd("0666000C0003");   /* out_dev = USB_HOST (3) */
            ipc_send_cmd("0657000C0008");   /* LOCALPLAYER work-mode */
        }
    }
    {   /* always rebuild: the player's queue can change under us (Play Through Folders), so no type-0 shortcut */
        int datalen = 8 + (int)strlen(name);          /* f1(4)+f2(4)+name */
        snprintf(f, sizeof f, "0100%04X%04X%04X%s",
                 8 + datalen, (build_idx >= 0 ? build_idx : pos1-1) & 0xFFFF, list_type & 0xFFFF, name);
        track_state_t cur; ipc_get_state(&cur);   /* snapshot BEFORE the send: the player's reply may already update the state */
        if(ipc_send_cmd(f) == 0){
            sent = 1;
            play_note_sent();
            fprintf(stderr,"play(build) type=%d pos=%d name='%s' -> %s\n", list_type, pos1, name, f);
            /* rebuild can take seconds - acknowledge + arm completion/timeout.
             * Capture the current track path so we clear only on a REAL track change
             * (a1 position frames from the old track must not falsely clear it). */
            play_request_arm(&cur, g_playing);   /* cur and g_playing are the pre-send state */
            ui_toast("Starting...");
        } else {
            ui_toast("Couldn't start playback");
        }
    }
    fflush(stderr);
    return sent;
}
int ui_play_list(int list_type, const char *name, int pos1){ return ui_play_list_ex(list_type, name, pos1, -1); }
/* V2.57 favourites: play stock's type-6 queue from the favourite at 1-based row pos1 of the stock-ordered list
 * (mdb_favorites stock_order), whose MY_LOVE.ID is love_id. The build starts at that ID (the player's contract);
 * a jump inside the loaded favourites queue uses the row. */
int ui_play_favorite(int love_id, int pos1){
    if(love_id <= 0 || pos1 < 1){ ui_toast("Couldn't find that song"); return 0; }
    /* prove this exact track starts before caching scope; a row gone since the list filled would only be refused */
    char lpath[256];
    if(!mdb_love_path(love_id, lpath, sizeof lpath)){ ui_toast("Couldn't find that song"); return 0; }
    return ui_play_list_ex(6, "", pos1, love_id);
}
/* Play a custom playlist (list_type 5) by its id, from 1-based track pos. */
int ui_play_playlist(long pid, int pos){
    if(modes_output_busy()){ ui_toast("Switching output - try again"); return 0; }
    if(!ui_local_playback_allowed()){ ui_toast("Return to local playback first"); return 0; }
    if(ui_get_source_mode() == 4 && !ui_usb_dac_connected()){
        ui_toast("USB DAC not connected"); return 0;
    }
    /* diskOS's type-5 play always resolves to seq 0, so passing the playlist id as the name never
     * targeted that playlist (and, once the book slot exists, it played the BOOK). Instead copy the
     * playlist into the reserved slot and play seq 0 - isolated, and it finally plays the right list.
     * The caller (playlistview) has already set the music play-mode via ui_set_workmode, so this is
     * NOT Single. */
    int n = mdb_reserved_slot_set_playlist(pid);
    if(n <= 0){ ui_toast("Playlist is empty"); return 0; }
    g_book_single_mode = 0;                                 /* not a book */
    send_play_mode(cfg_get_int("work_mode", 0));  /* set the music play-mode explicitly - a song-row tap in a playlist does not, and a prior book left Single */
    if((ui_get_source_mode() == 0 || ui_get_source_mode() == 4) && !g_route_mac[0]) ipc_send_cmd("0657000C0008");  /* local work-mode: a type-5 play from an idle player needs it (same as a book) */
    if(pos < 1) pos = 1;
    int ppos = mdb_reserved_slot_player_pos(pid, pos);      /* the tapped row in the player's read of the slot */
    if(ppos < 1){ ui_toast("Couldn't find that song"); return 0; }   /* never start a guessed, possibly different song */
    pos = ppos;
    return ui_play_list(5, "", pos);                        /* seq 0 = reserved slot, now the playlist */
}
/* Up Next: play the song at 1-based position ord1 of the CURRENT queue (LIST_SONG_0 ID order - the same index in
 * shuffle, device-verified). A type-0 jump: no rebuild, so the loaded scope stays valid. Refused while a rebuild is
 * pending (the list being jumped in may not be the one on screen). Same local-play preamble rule as ui_play_list. */
int ui_queue_jump(int ord1){
    if(modes_output_busy()){ ui_toast("Switching output - try again"); return -1; }
    if(!ui_local_playback_allowed()){ ui_toast("Return to local playback first"); return -1; }
    if(ui_get_source_mode() == 4 && !ui_usb_dac_connected()){
        ui_toast("USB DAC not connected"); return -1;
    }
    if(g_play_pending){ ui_toast("Queue is updating - try again"); return -1; }
    if(ord1 < 1 || ord1 > 0xFFFF) return -1;
    ui_cancel_book_resume();
    ui_disarm_book_eoc();
    if(fw_needs_localplayer_init() && !g_route_mac[0] && !g_playing && !play_recent()){
        if(ui_get_source_mode() == 0){
            modes_local_init(0);   /* 0666 local route (6 internal, 4 SPDIF), then 0657 8 LOCALPLAYER work-mode */
        } else if(ui_get_source_mode() == 4){
            ipc_send_cmd("0666000C0003");   /* out_dev = USB_HOST (3) */
            ipc_send_cmd("0657000C0008");   /* LOCALPLAYER work-mode */
        }
    }
    ui_note_transport_sent();
    char f[24]; snprintf(f, sizeof f, "0100%04X%04X0000", 16, (ord1 - 1) & 0xFFFF);
    int rc = ipc_send_cmd(f);
    if(rc == 0) play_note_sent();
    fprintf(stderr, "queue jump pos=%d -> %s (rc=%d)\n", ord1, f, rc); fflush(stderr);
    return rc;
}
/* Play `song_id` through a play plan (musicdb mdb_album_plan): a stock queue (type 3/7) when the plan found one that
 * holds exactly the visible songs, else the exact reserved-slot queue (type 5), whose real order is adopted before
 * the position is taken. Returns 1 if a play was sent (0 when ui_play_list_ex refused it or the send failed). Never sends a position computed against a different list. */
int ui_play_plan(mdb_plan_t *plan, int song_id){
    if(modes_output_busy()){ ui_toast("Switching output - try again"); return 0; }
    if(!ui_local_playback_allowed()){ ui_toast("Return to local playback first"); return 0; }
    if(ui_get_source_mode() == 4 && !ui_usb_dac_connected()){
        ui_toast("USB DAC not connected"); return 0;
    }
    /* every failure path toasts once (here or in ui_play_list_ex), so callers only return */
    if(!plan || !plan->ids || plan->count <= 0){ ui_toast("Couldn't play that list"); return 0; }
    if(plan->list_type == 5){
        if(!mdb_plan_materialize(plan)){ ui_toast("Couldn't play that list"); return 0; }
        int pos = mdb_plan_pos(plan, song_id);
        if(pos < 1){ ui_toast("Couldn't find that song"); return 0; }
        g_book_single_mode = 0;
        send_play_mode(cfg_get_int("work_mode", 0));
        if((ui_get_source_mode() == 0 || ui_get_source_mode() == 4) && !g_route_mac[0]) ipc_send_cmd("0657000C0008");
        return ui_play_list_ex(5, "", pos, -1);
    }
    if(plan->list_type != 2 && plan->list_type != 3 && plan->list_type != 7){ ui_toast("Couldn't play that list"); return 0; }
    int pos = mdb_plan_pos(plan, song_id);
    if(pos < 1){ ui_toast("Couldn't find that song"); return 0; }
    return ui_play_list_ex(plan->list_type, plan->name, pos, -1);
}
/* Favourite/unfavourite the CURRENT song (0104: 1=love -> MY_LOVE, 0=unlove). */
void ui_set_favorite(int on){ ipc_send_cmd(on ? "0104000C0001" : "0104000C0000"); }
/* Song tap -> play the exact track.  Inside an album/artist/genre drill we use
 * that list as the context; from a flat list we fall back to the song's album
 * (else all-songs).  The 1-based position is computed with the player's own
 * ORDER BY so playback lands on the tapped song. */
static int on_song_play(int id){
    int lt = 0; char name[256] = "";
    /* Inside an album/artist/genre drill, play THAT list (so the playing
     * context - and shuffle/next - stays within what the user opened).  From a
     * flat list (Songs/Search/Favourites) play the WHOLE library so shuffle and
     * next/prev span the entire collection, not a single album.
     * NB: in the 0100 jump table, list_type 1 = all songs (rebuilds the full
     * LIST_SONG_0); type 0 is "resume current list" and does NOT rebuild. */
    if(!library_drill_context(&lt, name, sizeof name)){
        lt = 1; name[0] = 0;                       /* all songs (full library) */
    }
    if(lt == 2 && mdb_artist_mode()){              /* Artists by album artist: the player's queue only if it matches */
        mdb_plan_t plan; int ok = 0;
        if(mdb_artist_plan(name, &plan)){ ok = ui_play_plan(&plan, id); mdb_plan_free(&plan); }   /* ui_play_plan toasts its own failures */
        else ui_toast("Couldn't play that song");   /* never another scope: next/prev must stay in the group */
        return ok;
    }
    int pos = mdb_play_pos(id, lt, name);
    if(pos < 1 && lt != 1){
        /* the player's exact list (e.g. ARTIST='X') doesn't contain this song - happens
         * for a multi-artist track "A, X" shown under split artist X (the player can't
         * tokenise). Fall back to the all-songs list so the tap still plays it. */
        lt = 1; name[0] = 0;
        pos = mdb_play_pos(id, 1, "");
    }
    if(pos < 1){ ui_toast("Couldn't find that song"); return 0; }  /* don't fall back to track 1 */
    return ui_play_list(lt, name, pos);
}
/* Folder browser -> play a track by absolute path (L24). Resolves the path to its library row and
 * plays it in the all-songs scope, so it starts on THIS track. 1 on success. */
int ui_play_song_by_path(const char *path){
    if(!path || !*path) return 0;
    int id = mdb_song_id_by_path(path);
    if(id <= 0){ ui_toast("Not in library"); return 0; }
    int pos = mdb_play_pos(id, 1, "");
    if(pos < 1){ ui_toast("Couldn't find that song"); return 0; }
    return ui_play_list(1, "", pos);
}

/* ---- Audiobook playback session ---------------------------------------------------------------------
 * A "book session" is a DELIBERATE book play started from the Books screen via ui_play_book. Resume and
 * progress-checkpointing are gated on an active session, so a book incidentally pulled into a music
 * queue (Songs -> Play All builds its queue from the whole SONG table, which we can't filter) never has
 * its bookmark overwritten. A single ~400ms tick drives the state machine: it waits for the player to
 * actually load THIS book (not a blind delay), applies the resume seek once, then checkpoints at a ~10s
 * cadence and detects the end. Ending the session on any explicit track change keeps the last save. */
static char     g_book_sess[256];        /* the book of the active session ("" = none) */
static long     g_book_resume_ms = -1;   /* resume position owed, or <=0 = none */
static int      g_book_resume_done;      /* 1 once resume is applied (or not needed) -> checkpoint may run */
static int      g_book_confirmed;        /* 1 once the player reports THIS book as the current track */
static uint32_t g_book_deadline;         /* give up waiting for the book to load by this tick */
static uint32_t g_book_last_save;        /* last checkpoint tick (throttles SD writes) */
static int      g_book_was_playing;      /* previous play state, to catch a pause->resume for smart rewind */
static uint32_t g_book_pause_tick;       /* when the book was paused (0 = not paused) */
static long     g_book_last_pos = -1;    /* last fresh position, to spot a loop/restart (big backward jump) */
static long     g_book_resume_target = 0; /* the position the resume seek aimed at (0 = none) */
static int      g_book_ckpt_ok = 0;      /* 1 once checkpointing is safe: resume seek CONFIRMED applied (a position at/after the target was observed), or no resume was owed, or the user took manual control. Gates the checkpoint so a sent-but-ignored resume seek can't save ~0 over a deep bookmark. */
static int book_session_active(void){ return g_book_sess[0] != 0; }   /* an audiobook is the active playback context */
int ui_book_active(void){ return g_book_sess[0] != 0; }   /* public: a book is the active context (Tune/Settings suppress the Play Mode change so it can't drop the book's Single mode) */
static uint32_t g_sleep_seek_guard;      /* hold the sleep pause off until this tick after a manual seek (play-state read can be transiently wrong) */

/* End the current book session (if any). Called on EVERY explicit playback change (new list play, next,
 * prev) so a queued resume can't seek a track the user switched away from, and checkpointing stops the
 * moment the book is no longer the deliberate context. Keeps whatever was last saved. */
void ui_cancel_book_resume(void){
    /* Final checkpoint before dropping the session: switching book/track/menu otherwise loses up to a
     * full ~10s throttle window of progress. Only when the position is confirmed and belongs to THIS
     * book (the player may already be loading the next track when this runs). */
    if(g_book_sess[0] && g_book_ckpt_ok){
        track_state_t s; ipc_get_state(&s);
        if(s.have_track && strcmp(s.path, g_book_sess) == 0 && s.position_ms > 0
           && (int32_t)(s.pos_seq - s.path_seq) > 0)
            mdb_book_save(g_book_sess, g_book_sess, s.position_ms, 0);
    }
    g_book_sess[0] = 0;
    g_book_resume_ms = -1;
    g_book_resume_done = 0;
    g_book_confirmed = 0;
    g_book_was_playing = 0;
    g_book_pause_tick = 0;
    g_book_last_pos = -1;
    g_book_resume_target = 0;
    g_book_ckpt_ok = 0;
}

/* The user dragged the seek ring: they now own the position, so drop the pending resume but keep the
 * session (we still checkpoint their new position). No-op when no book session is active. */
/* Hold the sleep pause off briefly. Called on an on-screen transport tap so a manual play/pause can't
 * race the sleep check: the toggle we'd send could land on an already-flipped state and restart audio. */
void ui_defer_sleep(void){ g_sleep_seek_guard = lv_tick_get() + 2500; }   /* > the ~1600ms play-state inference grace + margin */

void ui_book_user_seeked(long target_ms){
    g_sleep_seek_guard = lv_tick_get() + 2500;   /* a seek can transiently look like "playing" for longer than the old 2000ms -> a sleep/EOC toggle in that window would START a paused book; match ui_defer_sleep's 2500ms (device-measure the real echo window to tune) */
    /* Gate on g_book_confirmed: only attribute a seek to the session book once the player CONFIRMS it is the
     * current track. During a book SWITCH, Now Playing can still report the OLD book for a beat; treating a
     * +30/-15 or ring seek in that window as the new book's would write the old position over the new book's
     * bookmark AND cancel its pending resume (blocker). Unconfirmed -> leave the pending resume intact. */
    if(g_book_sess[0] && g_book_confirmed){
        g_book_resume_ms = -1; g_book_resume_done = 1; g_book_pause_tick = 0;  /* a deliberate seek cancels any pending pause-rewind and puts the user in control */
        /* Persist the seek target to BOOK_PROGRESS IMMEDIATELY. A deliberate seek is the strongest bookmark
         * intent, and making it durable at once means a restart (player or UI) before the next ~10s
         * checkpoint resumes at exactly where the user seeked - not the pre-seek bookmark, and not undone by
         * the resume-aware adoption (which reads this saved value). target_ms is the clamped target the
         * caller actually sent, so it never depends on a post-seek position read. */
        if(target_ms >= 0){
            g_book_last_pos = target_ms;                        /* keep the loop/restart backward-jump baseline consistent with the new position */
            g_book_resume_target = target_ms;                   /* re-arm the confirmation gate: keep checkpoint/cancel-flush DISABLED until a real post-seek position reaches this target, so a stale pre-seek frame can't overwrite this seek's bookmark (the immediate save below already makes the target durable) */
            g_book_ckpt_ok = 0;
            mdb_book_save(g_book_sess, g_book_sess, target_ms, 0);
        } else {
            g_book_ckpt_ok = 1;                                 /* no concrete target to confirm -> allow checkpointing */
        }
    }
}

/* Play an audiobook (v1: single-file .m4b) and resume at resume_ms (0 = start). */
int ui_play_book(const char *path, long resume_ms){
    if(modes_output_busy()){ ui_toast("Switching output - try again"); return 0; }
    if(!ui_local_playback_allowed()){ ui_toast("Return to local playback first"); return 0; }
    if(ui_get_source_mode() == 4 && !ui_usb_dac_connected()){
        ui_toast("USB DAC not connected"); return 0;
    }
    if(!path || !*path) return 0;
    /* The path must round-trip the player's 256-byte track path AND survive the a2 frame's JSON
     * re-escape (the decoder reserves 4 bytes), or st.path won't match and resume/checkpoint would
     * silently no-op. Reject early with feedback rather than start a book we can't track. */
    if(strlen(path) >= sizeof g_book_sess - 4){ ui_toast("Path too long"); return 0; }
    /* ISOLATED playback: a book plays from a RESERVED custom-playlist slot (list_type 5, seq 0), never
     * the all-songs scope, so it can never share the music queue. Device-verified 2026-09-18: the player
     * reads the reserved slot's membership AND registry fresh on each play (no mq_player restart), a
     * member with no SONG row still decodes, and Single play-mode stops cleanly at the book's end without
     * rolling into the stale alternate buffer. Set the slot to this book, force Single mode (transient -
     * the next music play restores the user's mode via play_list_mode), then play seq 0. */
    if(!mdb_reserved_slot_set(path)){ ui_toast("Couldn't start book"); return 0; }
    /* A type-5 play from an idle player can leave the work-mode NULL (player logs NO_WORK_MODE and never
     * starts). Re-assert the local-play route + work-mode first - device-verified this is what a book
     * play needs; ui_play_list's own preamble is gated to V2.40, but a book must start on V2.09/V2.28 too.
     * Local analog or USB DAC route (never a BT A2DP path). */
    if((ui_get_source_mode() == 0 || ui_get_source_mode() == 4) && !g_route_mac[0]){
        ipc_send_cmd("0657000C0008");                      /* LOCALPLAYER work-mode (fixes NO_WORK_MODE). NOT 0666:
                                                            * we're already on the local/DAC route (gated above), and a
                                                            * redundant out_dev re-init pauses the stream ~1.5s in. */
    }
    if(send_play_mode(4) != 0){ ui_toast("Couldn't start book"); return 0; }   /* Single play-mode is REQUIRED for clean EOF isolation; if it can't be set, don't start the book unguarded */
    g_book_single_mode = 1;                                 /* remember to restore the music mode on the next music play */
    if(!ui_play_list(5, "", 1)) return 0;                   /* frame 0100001000000005 -> reserved slot (seq 0): an isolated 1-item queue; not sent -> no session */
    g_play_pending = 0;                                     /* a book owns its own load-confirmation via the book session (book_tick's deadline), not the music-scope playback-start monitor - which false-fires when re-tapping the already-current book (path unchanged) */
    /* Arm the session AFTER the play call: ui_play_list clears the session, so setting it here makes
     * THIS book the active context. */
    snprintf(g_book_sess, sizeof g_book_sess, "%s", path);
    g_book_resume_ms   = (resume_ms > 1000) ? resume_ms : 0;
    g_book_resume_done = (g_book_resume_ms > 0) ? 0 : 1;
    g_book_resume_target = g_book_resume_ms;                 /* checkpointing waits until a position confirms the seek reached here */
    g_book_ckpt_ok     = (g_book_resume_ms > 0) ? 0 : 1;     /* no resume owed -> checkpoint immediately */
    g_book_confirmed   = 0;
    g_book_deadline    = lv_tick_get() + 15000;             /* never loads in 15s -> give up (nothing was checkpointed) */
    g_book_last_save   = lv_tick_get();
    return 1;
}

/* ~400ms: confirm the book loaded, apply resume once, then checkpoint + detect finish. */
static void book_tick(lv_timer_t *t){
    (void)t;
    if(!ui_local_playback_allowed()) return;
    if(!g_book_sess[0]){
        /* Adopt an orphaned book: after a UI-only restart the player still plays the book but our
         * session was lost, so checkpointing stops. If a book is the current track, re-establish the
         * session (no resume - it's already positioned; checkpoint immediately) and re-assert Single. */
        if(DISKOS_AUDIOBOOKS && lv_tick_get() >= g_book_noadopt_until){
            track_state_t s; ipc_get_state(&s);
            if(s.have_track && mdb_is_book_path(s.path) && ui_is_playing()){   /* only a genuinely-PLAYING book (post-restart), not a transition or an ended/stopped book */
                long cur = (s.position_ms < 0) ? 0 : s.position_ms;
                long saved = 0;
                int have_bm = mdb_book_progress(s.path, NULL, 0, &saved, NULL, NULL);   /* 1=bookmark, 0=none, -1=read error */
                if(have_bm < 0) return;   /* a DB read error (BUSY/IOERR): don't adopt yet - adopting now could enable checkpointing a shallow position over an unread deep bookmark. Retry next tick. */
                snprintf(g_book_sess, sizeof g_book_sess, "%s", s.path);
                g_book_confirmed = 1; g_book_single_mode = 1;
                g_book_was_playing = 1; g_book_pause_tick = 0;
                g_book_last_save = lv_tick_get();
                /* RESUME-AWARE adoption: if a saved bookmark is much DEEPER than where playback currently is
                 * (a restarted/reattached player can be replaying from ~0, or a resume seek may never have
                 * taken), the shallow current position is NOT authoritative - set up a resume to the bookmark
                 * and gate checkpointing, so book_tick re-seeks there instead of saving the shallow position
                 * over the deep bookmark. (35s margin matches the loop/restart backward-jump guard.) When the
                 * saved bookmark is at/behind the current position, adopt normally and checkpoint from here. */
                if(have_bm == 1 && saved > cur + 35000){
                    g_book_resume_ms = saved; g_book_resume_done = 0; g_book_resume_target = saved;
                    g_book_ckpt_ok = 0; g_book_last_pos = -1;
                } else {
                    g_book_resume_ms = 0; g_book_resume_done = 1; g_book_resume_target = 0;
                    g_book_ckpt_ok = 1; g_book_last_pos = cur;
                }
                send_play_mode(4);              /* an adopted book must be in Single mode too */
            }
        }
        return;                                            /* nothing to do this tick (adopted next tick) */
    }
    track_state_t st; ipc_get_state(&st);
    uint32_t now = lv_tick_get();
    int past_deadline = (int32_t)(now - g_book_deadline) >= 0;
    int is_ours = (st.path[0] && strcmp(st.path, g_book_sess) == 0);
    if(!is_ours){
        if(g_book_confirmed){ ui_cancel_book_resume(); return; }   /* was playing the book, now a different track -> session over */
        if(past_deadline) ui_cancel_book_resume();                 /* never loaded -> give up (no corruption: checkpoint was gated) */
        return;                                             /* still waiting for the player to load it */
    }
    if(!g_book_confirmed){ g_book_confirmed = 1; return; }  /* one grace tick before acting on the freshly-matched track */
    if(!g_book_resume_done){
        if(g_book_resume_ms > 0){
            if(st.duration_ms <= 0 && !past_deadline) return;   /* wait for metadata (ready to seek); after the deadline try anyway */
            long rt = g_book_resume_ms;
            if(st.duration_ms > 0 && rt > st.duration_ms - 2000){ rt = st.duration_ms - 2000; if(rt < 0) rt = 0; }  /* clamp: a replaced/shorter book must not get an out-of-range seek */
            g_book_resume_target = rt;                          /* checkpointing waits until a position confirms the (clamped) target */
            if(ui_seek_to(rt) != 0) return;                     /* send failed -> retry next tick, stay PENDING (never mark done without a successful seek, or checkpoint could save ~0 over the bookmark) */
        }
        g_book_resume_done = 1;
        g_book_last_save = now;                             /* first checkpoint ~10s from now, after the seek settles */
        g_book_was_playing = ui_is_playing();
        return;
    }
    /* Smart rewind across an in-session pause: when the book resumes, back up by how long it was paused
     * so the narrator re-finishes the half-heard sentence. Uses the reported position directly (valid
     * whether or not the freshness gate below passes). */
    {
        int np = ui_is_playing();
        if(g_book_was_playing && !np){ g_book_pause_tick = now; }                 /* just paused */
        else if(!g_book_was_playing && np && g_book_pause_tick){                  /* just resumed */
            long rw = ui_smart_rewind_ms((long)(lv_tick_elaps(g_book_pause_tick) / 1000u));  /* unsigned divide: no negative on a very long pause */
            if(rw > 0){ long t = st.position_ms - rw; if(t < 0) t = 0; ui_seek_to(t); }
            g_book_pause_tick = 0;
        }
        g_book_was_playing = np;
    }
    /* (No auto "mark finished" at EOF: ui_is_playing() can't distinguish a natural end from a user pause
     * near the end, so any heuristic false-marks a paused book complete. The book stops cleanly at EOF
     * (Single mode); a re-tap simply resumes near the end. An explicit Restart/Mark-finished control is
     * the correct way to reset a completed book - future work.) */
    /* Only trust a position reported AFTER this track's path change: a stale position frame from the
     * previous track must never be saved under this book (blocker #2). Wrap-safe signed compare so a
     * counter rollover can't flip the freshness test. */
    if((int32_t)(st.pos_seq - st.path_seq) <= 0) return;   /* not a position reported for THIS track yet -> wait */
    /* Loop/restart guard: in an isolated 1-item scope the book repeats itself at the end under some play
     * modes, and a physical next restarts it - either way the position jumps back toward 0. Saving that
     * over the near-end bookmark would corrupt it. A big backward jump that is NOT a user seek means the
     * instance restarted, so end the session and let the last in-range checkpoint stand. (35s > the max
     * smart-rewind of 30s, so a rewind never trips it.) A legitimate seek to chapter 0 is a user seek, so
     * it's guarded; the baseline must still track fresh zero positions or the next real position would
     * falsely look like a huge backward jump. */
    int seek_guarded = lv_tick_get() < g_sleep_seek_guard;
    if(!seek_guarded && g_book_last_pos >= 0 && st.position_ms < g_book_last_pos - 35000){
        g_book_ckpt_ok = 0;   /* this is a bookmark-PROTECTION cancel (loop/restart) - do NOT let the cancel-flush save the bad position */
        ui_cancel_book_resume();
        return;
    }
    g_book_last_pos = st.position_ms;   /* fresh position (0 included) -> keep the baseline current, even while seek-guarded */
    if(st.position_ms <= 0) return;     /* don't checkpoint a zero position (start of book) */
    /* A book's own position can never exceed its duration. If it does, the book ended and playback has
     * bled into the NEXT queue item (v1 plays a book in the all-songs scope) while the path frame for
     * that item hasn't landed yet - saving that overflowing position under this book would corrupt the
     * bookmark. End the session; the last in-range checkpoint stands. (The real cure is an isolated
     * single-book scope so a book never auto-advances into music.) */
    if(st.duration_ms > 0 && st.position_ms > st.duration_ms + 2000){ g_book_ckpt_ok = 0; ui_cancel_book_resume(); return; }   /* overflow into the next item -> protect the bookmark; don't flush the bad position */
    /* Resume/seek-confirmation gate: don't checkpoint until a fresh position NEAR the target is seen -
     * within a window from EITHER side, so a stale PRE-seek position can't falsely confirm. A forward
     * target's stale position sits below it; a BACKWARD seek's stale position sits above it (a >=target
     * test would wrongly confirm that one and let the cancel-flush overwrite the just-saved bookmark). A
     * deep bookmark whose seek was sent-but-ignored keeps the position far from the target, so it never
     * confirms and the bookmark is preserved; once a real post-seek position lands in the window it latches. */
    if(!g_book_ckpt_ok){
        if(labs(st.position_ms - g_book_resume_target) <= 3000) g_book_ckpt_ok = 1;
        else return;
    }
    if(!ui_is_playing()) return;                            /* paused (not EOF - that's handled before the freshness gate): don't refresh the bookmark timestamp, keeps idle time accurate for the reopen rewind */
    if((int32_t)(now - g_book_last_save) < 10000) return;   /* throttle SD writes to ~10s */
    /* v1 does NOT auto-mark completion: its ~3s window races the track-advance that ends the session
     * and a UI stall can skip it, so a book simply resumes where it stopped (the user can seek back).
     * The COMPLETED column stays for a future explicit "mark finished" control - books.c already treats
     * completed as start-from-zero. */
    if(mdb_book_save(g_book_sess, g_book_sess, st.position_ms, 0) == 0) return;   /* write failed -> retry next tick, don't advance the throttle (else the bookmark is silently dropped for ~10s) */
    g_book_last_save = now;
}

/* Search result tap: ALWAYS play in the all-songs scope. Search spans the whole
 * library and must not inherit a stale Library album/artist/genre drill context. */
static int on_search_play(int id){
    int pos = mdb_play_pos(id, 1, "");          /* lt=1 = full library */
    if(pos < 1){ ui_toast("Couldn't find that song"); return 0; }
    return ui_play_list(1, "", pos);
}
void ui_set_workmode(int mode){
    /* LOCAL play-mode setter = command 0102 (class 1): 0102 000C <mode 4hex>.
     * GROUND TRUTH captured 2026-06-25 by strace'ing the stock UI's mq_timedsend
     * while tapping its play-mode control (see COMMAND_MAP.md). Stock cycles 5
     * modes 0..4: 0=Sequential, 1=Shuffle, 2=Repeat One, 3=Repeat All, 4=Single.
     * (This replaces the WRONG 0657, which is the audio SOURCE switch and could
     * wedge the device.) */
    if(mode < 0 || mode > 4) mode = 0;
    /* Persist here so cfg is the single source of truth: every play path that sets the
     * mode (Playlist Play/Shuffle, Library long-press, NP toggle, Settings, Tune) goes
     * through here, so the NP icon / Settings / Tune (all read cfg) can't drift from the
     * mode the player is actually using. Redundant cfg_set_int() at other call sites are
     * harmless. */
    cfg_set_int("work_mode", mode);
    int rc = send_play_mode(mode);
    fprintf(stderr,"workmode %d -> 0102000C%04X (rc=%d)\n", mode, mode, rc); fflush(stderr);
}

/* sleep timer: when armed, pause playback once the interval elapses (duration mode) or once the
 * current chapter ends (end-of-chapter mode, for audiobooks). The two modes are mutually exclusive. */
static uint32_t g_sleep_start=0, g_sleep_ms=0;
static long     g_sleep_eoc = -1;        /* >=0 = pause when the book position reaches this ms */
static char     g_sleep_eoc_path[256];   /* the book the EOC target belongs to (disarm if the track changes) */
static long     g_sleep_eoc_lastpos = -1; /* last position seen on the EOC book: if the track rolls over while this was near the target, the chapter completed -> fulfil the sleep instead of just disarming */
static int      g_sleep_eoc_atend = 0;    /* 1 iff the EOC target is the book's FILE END (last / chapterless chapter). Only then can a track rollover fulfil the sleep; a mid-book chapter target is always reached on-path, so a path change there is the user navigating away, never a rollover. */
/* Disarm any armed end-of-chapter sleep. Called on every EXPLICIT navigation (ui_play_list, and the
 * quick-settings next/prev) so that a still-armed EOC seen at a later path change can only be an
 * AUTOMATIC rollover, never user navigation. */
void ui_disarm_book_eoc(void){ g_sleep_eoc = -1; g_sleep_eoc_lastpos = -1; g_sleep_eoc_atend = 0; }
/* g_sleep_seek_guard is declared up near the book-session statics (used by ui_book_user_seeked). */
void ui_set_sleep_timer(int minutes){
    ui_disarm_book_eoc();
    if(minutes>0){ g_sleep_start=lv_tick_get(); g_sleep_ms=(uint32_t)minutes*60000u; }
    else g_sleep_ms=0;
}
/* Smart-rewind: how far to back up when resuming a book, by how long it sat idle. The longer since you
 * last listened, the more context you've lost, so the narrator re-finishes the sentence you half-heard. */
long ui_smart_rewind_ms(long idle_seconds){
    if(idle_seconds < 30)    return 0;       /* momentary -> exact spot */
    if(idle_seconds < 1800)  return 8000;    /* under 30 min -> a sentence */
    if(idle_seconds < 28800) return 18000;   /* under 8 h -> a little more */
    return 30000;                            /* overnight+ -> a cold-start recap */
}

/* Arm end-of-chapter sleep for a specific book: pause when THAT book's position reaches target_ms. */
void ui_set_sleep_eoc(long target_ms, const char *path, int at_book_end){
    g_sleep_ms = 0;
    g_sleep_eoc_lastpos = -1;
    if(target_ms > 0 && path && path[0]){ g_sleep_eoc = target_ms; g_sleep_eoc_atend = at_book_end ? 1 : 0; snprintf(g_sleep_eoc_path, sizeof g_sleep_eoc_path, "%s", path); }
    else { g_sleep_eoc = -1; g_sleep_eoc_atend = 0; }
}
/* Sleep state for the UI: 0 = off, 1 = duration (fills *secs_left), 2 = end-of-chapter. */
int ui_sleep_state(int *secs_left){
    if(g_sleep_ms){
        if(secs_left){ long r = (long)g_sleep_ms - (long)lv_tick_elaps(g_sleep_start); *secs_left = (int)((r>0?r:0)/1000); }
        return 1;
    }
    return g_sleep_eoc >= 0 ? 2 : 0;
}

/* apps: tapping an app row sets a pending exec; the main loop runs it between
 * frames so LVGL isn't mid-render. While the app runs we block in waitpid (so
 * diskOS draws nothing and the app owns fb0 + touch); on exit we force a full
 * redraw. mq_player keeps playing throughout. */
static char g_app_exec[256];
static int  g_app_pending = 0;
void app_launch(const char *exec){ snprintf(g_app_exec, sizeof g_app_exec, "%s", exec); g_app_pending = 1; }
/* Live Vol-Up pin level; 1 = held. Same register map as the boot override (x2000 pinctrl, GPB PxPIN,
 * bit 13, active low) - the input layer is unusable here because the stock player grabs event0. */
/* Mapped ONCE and kept: re-opening and re-mapping /dev/mem five times a second is pure waste. NULL if
 * the mapping is unavailable, in which case the escape below is simply unavailable too (and says so). */
static volatile uint32_t *gpio_gpb_pin(void){
    static volatile uint32_t *pin = NULL;
    static int tried = 0;
    if(!tried){
        tried = 1;
        int mem = open("/dev/mem", O_RDONLY | O_SYNC);
        if(mem >= 0){
            void *map = mmap(NULL, 4096, PROT_READ, MAP_SHARED, mem, 0x10010000);
            if(map != MAP_FAILED) pin = (volatile uint32_t *)((char *)map + 0x100);
            close(mem);
        }
    }
    return pin;
}
static int volup_held(void){
    volatile uint32_t *pin = gpio_gpb_pin();
    return pin ? (((*pin >> 13) & 1u) ? 0 : 1) : 0;
}
static void app_run(const char *exec){
    /* preflight: a missing/non-executable app shouldn't blank the screen for nothing */
    if(access(exec, X_OK) != 0){
        lv_obj_invalidate(lv_screen_active()); lv_refr_now(NULL);
        ui_toast("Can't launch app");
        return;
    }
    if(!gpio_gpb_pin()) ui_toast("Note: hold-to-close is unavailable");   /* no silent loss of the escape */
    int failed = 0, killed = 0;
    pid_t pid = fork();
    if(pid == 0){
        setpgid(0, 0);                 /* own group so one kill takes any grandchildren too */
        execl(exec, exec, (char*)NULL);
        _exit(127);
    } else if(pid > 0){
        int status = 0;
        /* EACCES means the child already exec'd, which only happens after IT called setpgid - so the
         * group exists either way. Any other error means grouping genuinely failed. */
        int grouped = (setpgid(pid, pid) == 0 || errno == EACCES);
        /* The app owns the screen until it exits, so a HUNG app used to own it forever - taking Settings,
         * and with it the user's route back to the stock firmware, for the rest of the session. Wait
         * without a deadline (a long-running app is legitimate) but keep an escape: hold Vol-Up for ~3s
         * to force-close it. Measured as elapsed MONOTONIC time, not a count of loop iterations. */
        struct timespec t0 = {0,0}; int holding = 0;
        for(;;){
            pid_t w = waitpid(pid, &status, WNOHANG);
            if(w == pid) break;
            if(w < 0 && errno != EINTR){ failed = 1; break; }
            if(volup_held()){
                struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
                if(!holding){ holding = 1; t0 = now; }
                else if(now.tv_sec - t0.tv_sec >= 3){
                    if(grouped) kill(-pid, SIGKILL);   /* the app and anything it spawned */
                    kill(pid, SIGKILL);                /* and the app itself, in case grouping failed
                                                        * or it moved itself to another group */
                    for(int k = 0; k < 20; k++){       /* bounded reap: never block on a D-state child */
                        if(waitpid(pid, &status, WNOHANG) == pid) break;
                        usleep(100000);
                    }
                    killed = 1; break;
                }
            } else holding = 0;
            usleep(200000);
        }
        if(!failed && !killed && WIFEXITED(status) && WEXITSTATUS(status) == 127) failed = 1;  /* exec failed */
    } else {
        failed = 1;  /* fork failed */
    }
    /* reclaim the screen: invalidate everything and force an immediate redraw */
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    if(killed)      ui_toast("App force-closed");
    else if(failed) ui_toast("Launch failed");
}

/* one-shot: re-request the player's current-track metadata shortly after launch,
 * in case the player wasn't ready when we sent the first 0202 at startup. */
static void state_sync_retry_cb(lv_timer_t *t){
    (void)t;
    ipc_send_probe("02020008");   /* silent: a miss during the connect race must not toast */
}

/* Bounded recovery; only a new identity or active transport cancellation restores eligibility. */
static void advance_probe_tick(int visible){
    static uint64_t token;
    static unsigned transport;
    static unsigned pos_seq;
    static uint32_t started, sent, pos_at;
    static long position;
    static int active, used, have_pos, was_visible = 1;
    uint32_t now = lv_tick_get();
    unsigned tx = ipc_transport_seq();
    track_state_t s; ipc_get_state(&s);
    int wake = visible && !was_visible;
    was_visible = visible;
    int changed = s.playback_token != token;
    if(changed){
        token = s.playback_token; active = used = have_pos = 0; pos_seq = 0;
    }
    if(!changed && tx != transport){
        if(active){ active = used = 0; }
        have_pos = 0; pos_seq = s.pos_seq; /* require progress after the action */
    }
    if(g_play_pending || book_session_active()){
        active = 0; used = 1;
    }
    transport = tx;
    int forward = 0;
    if(s.pos_seq != pos_seq && (int32_t)(s.pos_seq - s.path_seq) > 0){
        forward = have_pos && (uint32_t)(now - pos_at) <= 3000 && s.position_ms > position;
        if(have_pos && s.position_ms < position) active = 0;
        position = s.position_ms; pos_at = now; pos_seq = s.pos_seq; have_pos = 1;
    }
    if(active && (uint32_t)(now - started) >= 60000) active = 0;
    if(!visible) return;
    if(wake){ ipc_send_probe("02020008"); sent = now; }
    if(!s.have_track || !s.player_confirmed || s.provisional){ active = 0; return; }
    if(!used && forward && s.duration_ms > 1000 && s.position_ms >= s.duration_ms - 1000){
        used = active = 1; started = now;
        if(!wake){ ipc_send_probe("02020008"); sent = now; }
    }
    if(active && (uint32_t)(now - sent) >= 2000){
        ipc_send_probe("02020008"); sent = now;
    }
}

static int run_bounded(char *const argv[], int timeout_ms);   /* fork+exec with a hard timeout (defined below) */

/* Background ipc connect: at cold boot the player creates its /ui queue well
 * after we start (slower than launched-standalone), so a one-shot retry isn't
 * enough. Keep trying until /ui exists, then sync state and stop. */
static void ipc_connect_retry_cb(lv_timer_t *t){
    if(ipc_start()==0){
        fprintf(stderr,"ipc connected (deferred)\n"); fflush(stderr);
        /* NB: we no longer force SYSCONFIG.WORK_MODE here. A player restart bumps the IPC
         * generation, so localplayer_workmode_cb re-asserts Local per generation (0642=no-export +
         * 0666 route + 0657 LOCALPLAYER audio) once the new player settles. Forcing WORK_MODE
         * on every reconnect would clobber a deliberate per-session USB-Storage/USB-DAC pick;
         * the persisted default is set once at boot + by the flashed pre-launch gate. */
        ipc_send_probe("02020008");   /* request live track once connected (silent) */
        /* audio re-apply is handled by the main-loop seq gate (covers this deferred connect
         * AND a later player restart via seq-reset detection) - no premature send here. */
        lv_timer_del(t);
    }
}

/* Periodic IPC health check: re-establish the /ui receive queue if it was ever
 * lost. Does NOT poll the player (a periodic 02020008 would bump seq every tick
 * and needlessly re-push the Now-Playing/backdrop surfaces). A player restart is
 * self-healed on the next real send instead (ipc_send_internal reopens /player). */
static void ipc_health_cb(lv_timer_t *t){
    (void)t;
    if(!ipc_is_ready()) ipc_start();   /* cold-start retry if the rx thread never came up (idempotent) */
    else ipc_health_check();           /* detect + request recovery from a player queue restart */
}

/* async-signal-safe: log the fatal signal number to the boot log so a hard crash
 * is visible after the watchdog respawns. Runs on a dedicated alt stack. */
static void crash_log(int sig, siginfo_t *si, void *ucv){
    const char *m = "=== diskos CAUGHT FATAL SIGNAL ";
    int fd = open("/usr/data/diskos_boot.log", O_WRONLY|O_APPEND);
    if(fd>=0){ (void)si; (void)ucv;
        char b[8]; int n=sig, i=0; b[i++]=' ';
        if(n>=10){ b[i++]='0'+(n/10); } b[i++]='0'+(n%10); b[i++]='\n';
        write(fd,m,30); write(fd,b,i); close(fd); }
    signal(sig, SIG_DFL); raise(sig);
}

/* Boot-hang protection ------------------------------------------------------
 * Two independent guards keep the device from ever sitting in a black-screen
 * hang at boot (fiio_init's watchdog only respawns a DEAD mq_ui via `pgrep -x`,
 * never one that's alive-but-stuck in init - so an init hang is invisible to it):
 *
 *  1) run_bounded(): the blocking boot `system()` calls (sqlite WORK_MODE fix,
 *     hwclock -s) run as a killable child in its own process group with a hard
 *     timeout, so a locked DB or a busy RTC/I2C can't wedge init forever.
 *  2) an alarm(BOOT_DEADLINE_S) armed before risky init and disarmed once the
 *     first frame paints: if we never reach the main loop, SIGALRM _exit()s us
 *     (only our own process - never mq_player) so fiio_init respawns a fresh
 *     mq_ui instead of leaving a permanent black screen. */
/* PGID of the in-flight run_bounded child (0 = none), so the boot watchdog can
 * KILL it before exiting instead of abandoning a stuck child. A child stuck
 * before exec is still named "mq_ui" and would satisfy fiio_init's `pgrep -x mq_ui`
 * -> suppress our respawn; killing its group closes that hole. */
static volatile sig_atomic_t g_bounded_pgid = 0;
/* ...and its PID. The group only exists if setpgid worked; the PID always identifies the child, so every
 * kill path signals both. Registered and cleared with SIGALRM blocked (see run_bounded). */
static volatile sig_atomic_t g_bounded_pid = 0;

static void boot_alarm_handler(int sig){
    (void)sig;
    if(g_bounded_pgid > 0) kill(-(pid_t)g_bounded_pgid, SIGKILL);  /* never abandon a stuck child */
    if(g_bounded_pid > 0) kill((pid_t)g_bounded_pid, SIGKILL);     /* ...even if it never got its group */
    /* The diagnostic must never be able to PREVENT the exit. stderr points at the boot log on NAND, and
     * a write to the same storage that wedged us can block indefinitely - so re-arm first with the
     * DEFAULT disposition: if the write does block, SIGALRM then terminates us anyway and the firmware
     * watchdog still gets its respawn. All async-signal-safe. */
    signal(SIGALRM, SIG_DFL);
    { sigset_t s; sigemptyset(&s); sigaddset(&s, SIGALRM); sigprocmask(SIG_UNBLOCK, &s, NULL); }
    alarm(5);
    static const char m[] = "=== diskos BOOT WATCHDOG timeout -> exit for respawn ===\n";
    (void)write(2, m, sizeof m - 1);
    _exit(124);   /* async-signal-safe; fiio_init's `pgrep -x mq_ui` misses -> respawn */
}

/* Deferred reaper: a child SIGKILLed on timeout may sit in uninterruptible I/O past our ~1s bounded reap;
 * when it finally exits it becomes a zombie (we're still its parent). Without collection, repeated timeouts
 * (e.g. RTC saves on a flaky bus) accumulate zombies and can exhaust the process table. Retain the PID and
 * sweep it with a non-blocking waitpid on later run_bounded calls until it's collected. */
#define DEFER_REAP_MAX 16
static pid_t g_defer_reap[DEFER_REAP_MAX];
static void defer_reap_sweep(void){
    for(int i = 0; i < DEFER_REAP_MAX; i++){
        if(g_defer_reap[i] > 0){
            pid_t w = waitpid(g_defer_reap[i], NULL, WNOHANG);
            if(w == g_defer_reap[i] || (w < 0 && errno == ECHILD)) g_defer_reap[i] = 0;  /* collected or already gone */
        }
    }
}
static int defer_reap_has_slot(void){
    for(int i = 0; i < DEFER_REAP_MAX; i++) if(g_defer_reap[i] == 0) return 1;
    return 0;
}
static void defer_reap_add(pid_t p){
    for(int i = 0; i < DEFER_REAP_MAX; i++) if(g_defer_reap[i] == 0){ g_defer_reap[i] = p; return; }
    /* unreachable: run_bounded refuses to fork unless a slot is free, so a slot always exists here */
}

/* Run argv[] as a child in its own process group; wait up to timeout_ms.
 * On timeout, SIGKILL the group and reap with a bound. Returns 0 on clean exit,
 * -1 otherwise. Registers the child in g_bounded_pgid so the boot watchdog can
 * kill it on timeout rather than leaving an "mq_ui"-named process alive. */
static int run_bounded(char *const argv[], int timeout_ms){
    defer_reap_sweep();   /* opportunistically collect any previously-abandoned timed-out child */
    if(!defer_reap_has_slot()) return -1;   /* no reaping capacity (16 children stuck) -> refuse to fork rather
                                             * than spawn a child we couldn't track/reap -> bounds the leak at 16 */
    /* Block the deadline signal across fork + grouping + registration. A SIGALRM landing inside that
     * window used to exec the stock player while this child was not yet in g_bounded_pgid, leaving it
     * running - and, in the launcher, still able to mutate the settings database after stock started. */
    sigset_t alrm, prev;
    sigemptyset(&alrm); sigaddset(&alrm, SIGALRM);
    sigprocmask(SIG_BLOCK, &alrm, &prev);
    pid_t pid = fork();
    if(pid < 0){ sigprocmask(SIG_SETMASK, &prev, NULL); return -1; }
    if(pid == 0){
        setpgid(0, 0);                 /* own group: one kill takes any grandchildren too */
        sigprocmask(SIG_SETMASK, &prev, NULL);   /* the child must not inherit our blocked mask */
        for(int fd = 3; fd < 256; fd++) close(fd);   /* don't inherit our fb/mqueue/etc fds */
        int nul = open("/dev/null", O_RDWR);
        if(nul >= 0){ dup2(nul, 0); if(nul > 2) close(nul); }  /* no stdin; keep 1/2 = boot log */
        execvp(argv[0], argv);
        _exit(127);                    /* exec failed */
    }
    /* EACCES means the child already exec'd, which says nothing about whether ITS setpgid worked - so the
     * group is only claimed when it is verified to exist. */
    int grouped = (setpgid(pid, pid) == 0 || (errno == EACCES && getpgid(pid) == pid));
    g_bounded_pid = pid;                  /* always: the direct child is killable even without a group */
    g_bounded_pgid = grouped ? pid : 0;   /* only claim group-kill when the group really exists */
    sigprocmask(SIG_SETMASK, &prev, NULL);
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = -1;
    for(;;){
        int status = 0;
        /* Reap and unregister with the deadline BLOCKED: once waitpid collects the child its PID is free
         * for reuse, so a deadline firing between the reap and the unregister would signal a stranger. */
        sigprocmask(SIG_BLOCK, &alrm, NULL);
        pid_t w = waitpid(pid, &status, WNOHANG);
        if(w == pid){
            g_bounded_pid = 0; g_bounded_pgid = 0;
            sigprocmask(SIG_SETMASK, &prev, NULL);
            rc = (WIFEXITED(status) && WEXITSTATUS(status)==0) ? 0 : -1; break;
        }
        sigprocmask(SIG_SETMASK, &prev, NULL);
        if(w < 0 && errno != EINTR && errno != ECHILD){ rc = -1; break; }
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (now.tv_sec - t0.tv_sec)*1000 + (now.tv_nsec - t0.tv_nsec)/1000000;
        if(ms >= timeout_ms){
            if(grouped) kill(-pid, SIGKILL);   /* the group, when we know one was established */
            kill(pid, SIGKILL);                /* and the child itself, always */
            /* bounded reap: try up to ~1s, then hand the PID to the deferred reaper (a D-state child that
             * outlives this window would otherwise zombie on its eventual exit) - never block indefinitely. */
            int reaped = 0;
            sigprocmask(SIG_BLOCK, &alrm, NULL);        /* same reap/unregister rule as above */
            for(int k = 0; k < 100; k++){ if(waitpid(pid, &status, WNOHANG) == pid){ reaped = 1; break; } usleep(10000); }
            if(!reaped) defer_reap_add(pid);
            g_bounded_pid = 0; g_bounded_pgid = 0;
            sigprocmask(SIG_SETMASK, &prev, NULL);
            rc = -1; break;
        }
        usleep(10000);                 /* 10ms poll granularity */
    }
    sigprocmask(SIG_BLOCK, &alrm, NULL);
    g_bounded_pid = 0; g_bounded_pgid = 0;
    sigprocmask(SIG_SETMASK, &prev, NULL);
    return rc;
}

/* the frequent polls (album-art 120ms, lyrics 500ms, weather 1s); paused while the
 * screen is fully off so the main loop can deep-idle instead of waking ~8x/s. */
static lv_timer_t *g_t_art, *g_t_lyr, *g_t_wx, *g_t_status;
static void polls_set_paused(int paused){
    if(!g_t_art) return;
    /* IMPORTANT: g_t_status (battery/wifi/BT, 3s) is DELIBERATELY left running at screen-off. Its
     * periodic fuel-gauge I2C read + timer wake is a keep-alive that prevents a device-level idle
     * FREEZE: with NOTHING polling at full screen-off, the stock player's idle DAC power-down leaves
     * the CS43131 (I2C bus 3) half-powered and a later transaction NOACKs, wedging the Ingenic I2C
     * controller and the whole SoC (device-verified 2026-08-26: stock never sleeps the screen and
     * never freezes; pausing this poll at screen-off is what introduced the freeze). Only the visual
     * polls (art/lyrics/weather) pause - they wake nothing useful behind a dark screen. */
    if(paused){ lv_timer_pause(g_t_art);  lv_timer_pause(g_t_lyr);  lv_timer_pause(g_t_wx);  }
    else      { lv_timer_resume(g_t_art); lv_timer_resume(g_t_lyr); lv_timer_resume(g_t_wx); }
}

/* Periodic RTC save WITHOUT blocking the UI thread. It used to run `hwclock -w` through run_bounded, which
 * waits on the UI thread: normally a few ms, but with a busy RTC/I2C bus up to its 3 s deadline plus ~1 s of
 * reaping, every 5 minutes. Now the tick only STARTS the child (own process group, same child setup as
 * run_bounded) and a short poll timer, alive only while a save is in flight, collects it; the first poll after the
 * deadline kills the group and hands the PID to the deferred reaper. The poll runs on the UI loop, so while that loop
 * is busy elsewhere (e.g. an app owns the screen) a stuck save lives until the loop resumes - it only ever costs a
 * process, never UI time. At most one save is in flight. */
#define RTC_SAVE_TIMEOUT_MS 3000
static pid_t g_rtc_pid;              /* in-flight `hwclock -w` child, 0 = none */
static int g_rtc_grouped;            /* its process group was verified to exist */
static uint32_t g_rtc_t0;            /* lv_tick when it started */
static void rtc_save_poll(lv_timer_t *t){
    int status = 0;
    pid_t w = waitpid(g_rtc_pid, &status, WNOHANG);
    if(w == g_rtc_pid || (w < 0 && errno == ECHILD)){ g_rtc_pid = 0; lv_timer_del(t); return; }   /* done */
    if(lv_tick_elaps(g_rtc_t0) < RTC_SAVE_TIMEOUT_MS) return;                                     /* still running */
    if(g_rtc_grouped) kill(-g_rtc_pid, SIGKILL);
    kill(g_rtc_pid, SIGKILL);
    if(waitpid(g_rtc_pid, &status, WNOHANG) != g_rtc_pid) defer_reap_add(g_rtc_pid);   /* collected later */
    fprintf(stderr, "rtc save: timed out after %d ms, killed\n", RTC_SAVE_TIMEOUT_MS);
    g_rtc_pid = 0; lv_timer_del(t);
}
static void hwclock_save_tick(lv_timer_t *t){
    (void)t;
    if(g_rtc_pid) return;                    /* the previous save is still in flight: its poll times it out */
    defer_reap_sweep();
    if(!defer_reap_has_slot()) return;       /* same leak bound as run_bounded: never a child we couldn't reap */
    pid_t pid = fork();
    if(pid < 0) return;
    if(pid == 0){
        setpgid(0, 0);
        for(int fd = 3; fd < 256; fd++) close(fd);
        int nul = open("/dev/null", O_RDWR);
        if(nul >= 0){ dup2(nul, 0); if(nul > 2) close(nul); }
        char *a[] = { "hwclock", "-w", NULL };
        execvp(a[0], a);
        _exit(127);
    }
    g_rtc_grouped = (setpgid(pid, pid) == 0 || (errno == EACCES && getpgid(pid) == pid));
    g_rtc_pid = pid;
    g_rtc_t0 = lv_tick_get();
    if(!lv_timer_create(rtc_save_poll, 100, NULL)){     /* no poll timer: fall back to a bounded kill now */
        if(g_rtc_grouped) kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
        if(waitpid(pid, NULL, WNOHANG) != pid) defer_reap_add(pid);
        g_rtc_pid = 0;
    }
}

void ui_rtc_save_now(void){ hwclock_save_tick(NULL); }   /* Date & Time: persist a hand-set clock right away */

/* Bounded external command, exposed so saver.c can decode the vinyl cover without an unbounded popen. */
int ui_run_bounded(char *const argv[], int timeout_ms){ return run_bounded(argv, timeout_ms); }

/* ---- rim (circumference) scroll -------------------------------------------
 * Drag a finger around the screen EDGE to fly through long lists, like a crown.
 * State machine IDLE->CANDIDATE->OWNED:
 *  - press inside the rim band arms a CANDIDATE but does NOT consume (taps/normal
 *    centre scroll keep working; LVGL still owns the touch).
 *  - on move, commit to OWNED only when travel is mostly TANGENTIAL (around the
 *    rim) and exceeds a threshold -> this is what distinguishes a rim drag from
 *    the radial back-swipe / pull-down. On commit, lv_indev_wait_release() makes
 *    LVGL drop its own list-scroll so we don't double-scroll; we drive
 *    lv_obj_scroll_by() ourselves from the angular delta.
 *  - release consumes the gesture (skips nav) only if it actually committed. */
#define RIM_R_MIN       146
#define RIM_R_MAX       182
#define RIM_REL_R_MIN   122      /* hysteresis: keep owning until finger drifts well inside */
#define RIM_COMMIT_TANG 22.0f    /* tangential travel (px) to commit */
#define RIM_PX_PER_RAD  180.0f   /* ~quarter-turn = one screen on the ~268px list */
#define RIM_ALB_PER_RAD 1.5f     /* cover flow: albums advanced per radian of rim travel */
#define RIM_MAX_OVER    34       /* max elastic overscroll (px) past either end before it springs back */
enum { RIM_IDLE, RIM_CAND, RIM_OWN };
static int       rim_state = RIM_IDLE;
static float     rim_last_ang = 0;
static int       rim_sx = 0, rim_sy = 0;
static float     rim_accum = 0;          /* fractional scroll carry, so slow drags don't quantise to 0 */
static uint32_t  rim_last_ms = 0;
static lv_obj_t *rim_scroller = NULL;
static int       rim_cover = 0;          /* rim gesture is driving the cover flow (no list scroller) */

static lv_obj_t *ui_active_scroller(void){
    switch(screen_current()){
        case SCR_LIBRARY: return library_scroller();
        case SCR_PLVIEW:  return playlistview_scroller();
        case SCR_SEARCH:  return search_scroller();
        case SCR_APPS:    return apps_scroller();
        default:          return NULL;
    }
}
static float rim_ang(int x, int y){ return atan2f((float)(y-180), (float)(x-180)); }
static float rim_wrap(float d){ if(d>(float)M_PI) d-=2.0f*(float)M_PI; else if(d<-(float)M_PI) d+=2.0f*(float)M_PI; return d; }

static void rim_press(int x, int y){
    rim_state = RIM_IDLE; rim_scroller = NULL; rim_cover = 0;
    int dx=x-180, dy=y-180; float r=sqrtf((float)(dx*dx+dy*dy));
    if(r < RIM_R_MIN || r > RIM_R_MAX) return;        /* not on the rim band */
    if(screen_current()==SCR_ALBUMWALL){              /* cover flow: rim drives the flow, not a list */
        rim_cover=1; rim_sx=x; rim_sy=y; rim_last_ang=rim_ang(x,y);
        rim_accum=0; rim_last_ms=lv_tick_get(); rim_state=RIM_CAND; return;
    }
    lv_obj_t *sc = ui_active_scroller();
    if(!sc) return;                                   /* screen has no long list */
    lv_anim_delete(sc, NULL);                         /* cancel any in-flight spring-back so it won't fight the new drag */
    rim_scroller=sc; rim_sx=x; rim_sy=y; rim_last_ang=rim_ang(x,y);
    rim_accum=0; rim_last_ms=lv_tick_get(); rim_state=RIM_CAND;
}
/* returns 1 once OWNED so the caller skips other gestures */
static int rim_move(int x, int y){
    if(rim_state==RIM_IDLE) return 0;
    int rdx=x-180, rdy=y-180; float r=sqrtf((float)(rdx*rdx+rdy*rdy)); if(r<1) r=1;
    if(rim_state==RIM_CAND){
        int dx=x-rim_sx, dy=y-rim_sy;
        float radial = (dx*rdx + dy*rdy)/r;           /* + = outward/inward along radius */
        float tang   = fabsf((float)dx*rdy - (float)dy*rdx)/r;  /* perpendicular (around-rim) travel */
        if(tang < RIM_COMMIT_TANG || tang < fabsf(radial)) return 0;  /* not a rim drag (yet) */
        if(r < RIM_REL_R_MIN){ rim_state=RIM_IDLE; rim_scroller=NULL; return 0; }  /* wandered off the rim before committing -> let normal handling have it */
        lv_indev_wait_release(g_touch);               /* LVGL: drop this touch, we own it now */
        if(rim_scroller && rim_scroller == library_scroller()) library_jump_cancel();  /* no scroll yet, so no SCROLL_BEGIN */
        if(rim_scroller) lv_obj_scroll_to_y(rim_scroller, lv_obj_get_scroll_y(rim_scroller), LV_ANIM_OFF); /* stop any momentum */
        rim_state=RIM_OWN; rim_last_ang=rim_ang(x,y); rim_last_ms=lv_tick_get(); rim_accum=0;
        return 1;
    }
    /* OWNED */
    if(rim_cover){                                      /* cover flow: angular travel -> continuous album scroll */
        if(screen_current()!=SCR_ALBUMWALL) return 1;   /* screen changed under us: hold, don't scroll */
        if(r < RIM_REL_R_MIN) return 1;                 /* drifted inward: hold, no scroll this frame */
        float ang=rim_ang(x,y); float dAng=rim_wrap(ang-rim_last_ang); rim_last_ang=ang; rim_last_ms=lv_tick_get();
        if(dAng > 1.2f || dAng < -1.2f) return 1;       /* clamp coalesced jumps */
        albumwall_scroll_rel(dAng * RIM_ALB_PER_RAD);   /* clockwise (dAng>0) = next album */
        return 1;
    }
    if(rim_scroller != ui_active_scroller()) return 1;  /* screen changed under us: hold gesture, don't scroll a stale list */
    if(r < RIM_REL_R_MIN) return 1;                   /* drifted inward: hold gesture, no scroll this frame */
    float ang=rim_ang(x,y); float dAng=rim_wrap(ang-rim_last_ang); rim_last_ang=ang;
    uint32_t now=lv_tick_get(); uint32_t dt=now-rim_last_ms; rim_last_ms=now;
    if(dAng > 1.2f || dAng < -1.2f) return 1;         /* clamp coalesced jumps (>~70deg/frame = noise) */
    float omega = dt>0 ? fabsf(dAng)/((float)dt/1000.0f) : 0.0f;
    float gain  = 1.0f + omega/4.0f; if(gain>6.0f) gain=6.0f;   /* mild acceleration for the 3000-item list */
    rim_accum += dAng * RIM_PX_PER_RAD * gain;        /* clockwise (dAng>0) = scroll down */
    int step=(int)rim_accum; rim_accum -= (float)step;
    if(step){
        int dy  = -step;
        int top = lv_obj_get_scroll_top(rim_scroller);     /* room to scroll up (neg = already past top) */
        int bot = lv_obj_get_scroll_bottom(rim_scroller);  /* room to scroll down (neg = past bottom) */
        int roomUp = top>0?top:0, roomDn = bot>0?bot:0;
        int inb = dy;                                      /* in-bounds portion of this step */
        if(dy>0 && inb >  roomUp) inb =  roomUp;
        if(dy<0 && inb < -roomDn) inb = -roomDn;
        if(inb) lv_obj_scroll_by(rim_scroller, 0, inb, LV_ANIM_OFF);
        int excess = dy - inb;                             /* travel past an edge (same sign as dy) */
        if(excess){                                        /* elastic rubber-band, damped + capped */
            int over = excess>0 ? (top<0?-top:0) : (bot<0?-bot:0);  /* overscroll depth this side (>=0) */
            int remain = RIM_MAX_OVER - over;              /* rubber-band px still available */
            if(remain > 0){
                float damp = 1.0f - (float)over/(float)RIM_MAX_OVER;
                int add = (int)((float)excess * 0.40f * damp);
                if(add==0) add = excess>0?1:-1;
                if(add >  remain) add =  remain;           /* never let one big step blow past the cap */
                if(add < -remain) add = -remain;
                lv_obj_scroll_by(rim_scroller, 0, add, LV_ANIM_OFF);
            }
        }
    }
    if(rim_scroller == library_scroller()) library_scroll_letter_tick();  /* show A-Z position */
    return 1;
}
/* Animate an overscrolled list back to its bound (the "bounce") - the rim path bypasses
 * LVGL's indev release-snap, so we snap it ourselves with an ease-out scroll animation. */
static void rim_spring_back(lv_obj_t *sc){
    if(!sc || !lv_obj_is_valid(sc)) return;
    int top = lv_obj_get_scroll_top(sc);
    int bot = lv_obj_get_scroll_bottom(sc);
    int dy = 0;
    if(top < 0)      dy = top;    /* past the top -> scroll down to remove it */
    else if(bot < 0) dy = -bot;   /* past the bottom -> scroll up */
    if(dy) lv_obj_scroll_by(sc, 0, dy, LV_ANIM_ON);
}
static int rim_release(void){
    int owned=(rim_state==RIM_OWN);
    if(owned && rim_cover)          albumwall_settle();        /* cover flow: snap to the nearest album */
    else if(owned && rim_scroller)  rim_spring_back(rim_scroller);
    rim_state=RIM_IDLE; rim_scroller=NULL; rim_cover=0; return owned;
}

/* Last.fm: every 1s, feed the current play-state to the watcher (now-playing + scrobble
 * eligibility) and pump the network worker / offline queue. All on the LVGL thread. */
static void lastfm_tick(lv_timer_t *t){
    (void)t; track_state_t st; ipc_get_state(&st);
    lastfm_watch(&st); lastfm_poll();
}

static int launch_status_get(void);   /* player-launcher verdict; defined with the launch gate below */
/* ---- microSD coldplug worker (native thread) -------------------------------
 * mq_player mounts the card by listening for the DISK 'add' uevent on
 * /sys/block/mmcblk0 (device-verified: re-emitting the PARTITION uevent does
 * nothing). Its Netlink listener isn't bound until ~100s into boot, so it MISSES
 * the already-inserted card's boot-time add event and the library comes up empty
 * until a physical reinsert. This worker re-emits the disk 'add' every few
 * seconds until the card mounts (bounded), then exits - same effect as a reinsert.
 * A native thread (not a shell subprocess) can't be orphaned/reaped ambiguously,
 * runs on every UI generation, and logs observably. */
static int coldplug_mounted(void){
    FILE *m = fopen("/proc/mounts", "r");
    if(!m) return 0;
    char line[512], dev[128], target[128]; int ok = 0;
    while(fgets(line, sizeof line, m)){
        if(sscanf(line, "%127s %127s", dev, target) != 2) continue;
        if(!strcmp(target, "/tmp/sdcard") &&
           (!strcmp(dev, "/dev/mmcblk0p1") || !strcmp(dev, "/dev/mmcblk0"))){ ok = 1; break; }
    }
    fclose(m);
    return ok;
}
static void coldplug_log(const char *msg){
    FILE *f = fopen("/usr/data/coldplug.log", "a");
    if(f){ fprintf(f, "%ld %s\n", (long)time(NULL), msg); fclose(f); }
}
/* Is the mass-storage gadget exporting the card to a USB host (so the host owns /dev/mmcblk0 and a
 * device-side mount would dual-access-corrupt exFAT)? Reads the REAL gadget state, not our g_source_mode
 * mirror - the player can enter a USB mode without going through ui_set_source_mode(). storage_demo is
 * NOT configured at boot (S99usbserial only builds the serial gadget); it appears only when a storage
 * session starts. The stock storage_config.sh sets the LUN backing file BEFORE binding the UDC and blanks
 * the UDC BEFORE clearing the LUN, so a blank UDC alone does NOT prove the card is free - we also check
 * the LUN. Fail-CLOSED: node present but UDC unreadable -> assume exported. */
static int sd_exported_to_host(void){
    const char *udcp = "/sys/kernel/config/usb_gadget/storage_demo/UDC";
    const char *fndir = "/sys/kernel/config/usb_gadget/storage_demo/functions";
    struct stat sb;
    /* FULLY fail-closed: only a confirmed ENOENT counts as "node absent"; any other stat/read failure is
     * ambiguous gadget state -> assume exported. */
    errno = 0; int have_udc = (stat(udcp, &sb) == 0); int udc_err = (!have_udc && errno != ENOENT);
    if(udc_err) return 1;                                       /* ambiguous -> exported */
    if(have_udc){
        FILE *f = fopen(udcp, "r");
        if(!f) return 1;                                        /* present but unreadable -> exported */
        char b[64] = {0}; size_t n = fread(b, 1, sizeof b - 1, f); int rerr = ferror(f); fclose(f);
        if(rerr) return 1;                                      /* read error -> exported */
        for(size_t i = 0; i < n; i++) if(b[i] > ' ') return 1;  /* UDC bound -> exported */
    }
    /* UDC blank/absent: the LUN backing file is authoritative during the blank-UDC transition windows.
     * The mass-storage function name is path-dependent (RE'd player builder: mass_storage.0; stock
     * storage_config.sh: mass_storage.msg) -> check EVERY mass_storage.* function. A function directory
     * whose LUN file is missing/unreadable is a gadget mid-construction -> ambiguous -> exported. */
    errno = 0;
    DIR *d = opendir(fndir);
    if(!d) return (errno == ENOENT) ? 0 : 1;                    /* functions dir genuinely absent -> not exported */
    int exported = 0; struct dirent *e;
    while(!exported && (e = readdir(d))){
        if(strncmp(e->d_name, "mass_storage.", 13) != 0) continue;
        char lunp[300]; snprintf(lunp, sizeof lunp, "%s/%s/lun.0/file", fndir, e->d_name);
        FILE *g = fopen(lunp, "r");
        if(!g){ exported = 1; break; }                           /* function present, LUN unreadable -> exported */
        char l[300] = {0}; size_t m = fread(l, 1, sizeof l - 1, g); int rerr = ferror(g); fclose(g);
        if(rerr || (m && strstr(l, "mmcblk0"))) exported = 1;   /* read error, or LUN backs /dev/mmcblk0 -> exported */
    }
    closedir(d);
    return exported;
}
static int storage_media_local(void){
    /* The launch verdict gates EVERY admission path (sd_io_init's callback, sd_io_resume, storage_tick,
     * the play path) - not just the three startup calls. Until the current player's launch is confirmed
     * we must not read the card at all, because we cannot yet know the delete guard is in effect. */
    if(atomic_load(&g_launch_verdict) != 1) return 0;
    return !g_sd_hold && !sd_exported_to_host() && coldplug_mounted();
}
/* A conservative "possibly exported" result cannot acknowledge our request.
 * Require both a bound controller and the actual card backing file. */
static int storage_host_confirmed(void){
    const char *base = "/sys/kernel/config/usb_gadget/storage_demo";
    char p[512], b[320];
    snprintf(p, sizeof p, "%s/UDC", base);
    FILE *f = fopen(p, "r"); if(!f) return 0;
    size_t n = fread(b, 1, sizeof b - 1, f); int err = ferror(f); fclose(f); b[n] = 0;
    int bound = 0;
    for(size_t i = 0; i < n; i++) if(b[i] > ' ') bound = 1;
    if(err || !bound) return 0;
    snprintf(p, sizeof p, "%s/functions", base);
    DIR *d = opendir(p); if(!d) return 0;
    struct dirent *e; int backed = 0;
    while((e = readdir(d))){
        if(strncmp(e->d_name, "mass_storage.", 13)) continue;
        snprintf(p, sizeof p, "%s/functions/%s/lun.0/file", base, e->d_name);
        f = fopen(p, "r"); if(!f) continue;
        n = fread(b, 1, sizeof b - 1, f); err = ferror(f); fclose(f); b[n] = 0;
        while(n && (b[n-1] == '\n' || b[n-1] == '\r')) b[--n] = 0;
        if(!err && (!strcmp(b, "/dev/mmcblk0") || !strcmp(b, "/dev/mmcblk0p1"))) backed = 1;
    }
    closedir(d);
    return backed;
}
/* Is the USB-DAC (uac_demo) gadget bound? Mirror of sd_exported_to_host for the audio gadget. Not a
 * corruption path (no SD block access), so a read failure is treated as "not bound" (fail-open). */
static int uac_bound(void){
    const char *udc = "/sys/kernel/config/usb_gadget/uac_demo/UDC";
    FILE *f = fopen(udc, "r");
    if(!f) return 0;                                            /* absent/unreadable -> not bound */
    char b[64] = {0}; size_t n = fread(b, 1, sizeof b - 1, f); int rerr = ferror(f); fclose(f);
    if(rerr) return 0;
    for(size_t i = 0; i < n; i++) if(b[i] > ' ') return 1;      /* non-blank UDC -> bound */
    return 0;
}
int ui_usb_dac_connected(void){
    if(access("/proc/asound/card1", F_OK) == 0) return 1;
    FILE *f = fopen("/proc/asound/cards", "r");
    if(!f) return 0;
    char line[256];
    int found = 0;
    while(fgets(line, sizeof line, f)){
        if(strstr(line, "USB-Audio") || strstr(line, "USB Audio")){
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}
/* M17: detect the source mode from the REAL USB gadget state, not our intent mirror: Storage =
 * storage_demo exported, USB-DAC = uac_demo bound, else no USB gadget (Local/USB Audio out;
 * BT sink is gadget-invisible so it reads as Local here). */
int ui_detect_source_mode(void){
    if(sd_exported_to_host()) return 3;
    if(uac_bound())           return 1;
    return (atomic_load(&g_source_mode) == 4) ? 4 : 0;
}
/* May the V2.40 worker direct-mount the card RIGHT NOW? Checked (under g_sd_mode_mu) immediately before
 * EACH mount attempt: absolute cold-boot window (fail-closed on unreadable uptime) AND the card is not
 * exported to a host. Re-evaluated per attempt so a slow first mount can't let a second begin outside the
 * window / after a host grabbed the card. The window matches coldplug_should_run()'s 150s worker-start
 * gate (a 45s cliff here starved first-boot mounts when the S97 install delay pushed mq_ui start past
 * 45s: worker ran but every mount hit "window elapsed" -> SD never mounted). The authoritative host-safety
 * guard is sd_exported_to_host() (fail-closed UDC+LUN); the uptime bound is a belt-and-suspenders proxy. */
static int sd_cold_mount_allowed(void){
    double up = -1.0; FILE *pu = fopen("/proc/uptime", "r");
    if(pu){ if(fscanf(pu, "%lf", &up) != 1) up = -1.0; fclose(pu); }
    /* g_sd_writable==0 means a Storage export is intended/pending (quiesced) - exclude that window even
     * if the gadget has not bound YET, so a mount can't race a queued-but-unexecuted export (M19). */
    return (up >= 0.0 && up <= 150.0 && !sd_exported_to_host() && atomic_load(&g_sd_writable));
}
static void *coldplug_thread(void *arg){
    (void)arg;
    const int direct_sd_mount = fw_needs_direct_sd_mount();
    /* Mount the already-inserted microSD at /tmp/sdcard at COLD boot. V2.09/V2.28: the player's uevent
     * listener mounts it once bound (~100s in); we re-emit the disk 'add' to trigger it. V2.40: the
     * player no longer mounts on that nudge, so we mount the card DIRECTLY. Either way this is ONE-SHOT:
     * we exit as soon as it's mounted and never re-mount (an indefinite writable remounter is unsafe -
     * it could mount during an async Storage->Local transition before the gadget detaches). Non-tethered
     * users - the shipping case - keep the card mounted after this; the tethered "player releases it"
     * case is dev-only. Every mount is under g_sd_mode_mu AND gated on Local mirror + real gadget state
     * (sd_exported_to_host) so we never touch the block device while a USB host owns it. */
    if(coldplug_mounted()) return NULL;            /* already mounted (player did it / warm restart) */
    coldplug_log("coldplug worker start");
    /* Do NOTHING to the card until the player launcher has published its verdict. We start before the
     * player (fiio_init runs mq_ui, sleeps 2s, then the player), so acting on our own checks alone could
     * mount and read the card before the launcher has established whether the delete guard is actually in
     * effect. A failure verdict aborts the worker outright - including the synthetic insert event, which
     * is itself what invokes the stock player's destructive cleanup. No verdict within the window is
     * treated as a failure. This is a background thread, so waiting here never delays the UI. */
    for(int w = 0; ; w++){
        int v = launch_status_get();
        if(v == 1) break;
        if(v == 0){ coldplug_log("launcher reported unprotected/unconfirmed - SD left untouched this boot"); return NULL; }
        if(w >= 40){ coldplug_log("no launcher verdict within 20s - SD left untouched this boot"); return NULL; }
        struct timespec hs = { 0, 500L*1000*1000 }; nanosleep(&hs, NULL);
    }
    /* SD-WIPE SAFETY (2026-09-21): the synthetic disk 'add' makes the stock player run its SD-mount routine,
     * which does "umount /tmp/sdcard; rm -rf /tmp/sdcard" WITHOUT checking the umount - if the card is
     * mounted and busy (our scan/prewarm/playback) that empties the card. The rootfs rm guard defuses the
     * delete; independently we must never create the ordering "event queued -> we direct-mount and read ->
     * event handled". So on direct-mount firmware the worker has two ONE-WAY phases: direct-mount attempts
     * first (no event); only if those fail does it fall back to the event, and once an event has been
     * emitted it NEVER direct-mounts again (a still-queued event could arrive after a later mount). */
    int nudged = 0;
    for(int i = 0; i < 120; i++){                  /* ~6 min cap (120 x 3s) covers the ~100s listener */
        if(coldplug_mounted()){ coldplug_log("SD mounted - done"); return NULL; }
        pthread_mutex_lock(&g_sd_mode_mu);
        if((ui_get_source_mode() == 0 || ui_get_source_mode() == 4) && !sd_exported_to_host() && !coldplug_mounted() && atomic_load(&g_sd_writable)){
            if(!direct_sd_mount || nudged || i >= 10){
                /* V2.09/V2.28: the player's uevent listener mounts the card (bound ~100s in) - bounded retries,
                 * each only while genuinely unmounted (checked above). V2.40/V2.57: fallback only, after ~30s
                 * of failed direct mounts, and one-way. */
                if(direct_sd_mount && !nudged) coldplug_log("direct mount failed -> falling back to the player's uevent listener (one-way: no more direct mounts)");
                nudged = 1;
                int fd = open("/sys/block/mmcblk0/uevent", O_WRONLY | O_CLOEXEC);
                if(fd >= 0){ ssize_t w = write(fd, "add\n", 4); (void)w; close(fd); }
            }
            else {                                 /* V2.40/V2.57: direct-mount phase (no event) */
                /* Guard EACH mount attempt with sd_cold_mount_allowed() (absolute cold-boot window + not
                 * exported), re-evaluated per attempt so a slow exfat mount can't let the vfat fallback
                 * begin outside the window or after a host grabbed the card. The window keeps the safety
                 * argument on "early boot, no storage session yet" rather than the check-then-act race
                 * (the stock Storage transition doesn't take g_sd_mode_mu). */
                mkdir("/tmp/sdcard", 0755);
                /* M19: the stock player owns the USB gadget cross-process - g_sd_mode_mu cannot exclude
                 * it, so a mount can still race an export begun in the window after sd_exported_to_host().
                 * Re-check the REAL gadget state AFTER a successful mount: if the card became host-exported,
                 * unmount immediately (fail-closed) rather than dual-access; the one-shot then gives up. */
                if(sd_cold_mount_allowed() && mount("/dev/mmcblk0p1", "/tmp/sdcard", "exfat", 0, NULL) == 0){
                    if(sd_exported_to_host()){
                        if(umount("/tmp/sdcard") != 0 && umount2("/tmp/sdcard", MNT_DETACH) != 0)
                            coldplug_log("WARNING: post-mount export + unmount BOTH failed (card busy) - possible transient dual-access");
                        else
                            coldplug_log("post-mount export detected -> unmounted, host owns card (one-shot gives up)");
                        pthread_mutex_unlock(&g_sd_mode_mu); return NULL;    /* explicit: don't fight the host */
                    }
                    coldplug_log("SD direct-mounted (V2.40 exfat)"); pthread_mutex_unlock(&g_sd_mode_mu); return NULL;
                }
                else if(sd_cold_mount_allowed() && mount("/dev/mmcblk0p1", "/tmp/sdcard", "vfat", 0, NULL) == 0){
                    if(sd_exported_to_host()){
                        if(umount("/tmp/sdcard") != 0 && umount2("/tmp/sdcard", MNT_DETACH) != 0)
                            coldplug_log("WARNING: post-mount export + unmount BOTH failed (card busy) - possible transient dual-access");
                        else
                            coldplug_log("post-mount export detected -> unmounted, host owns card (one-shot gives up)");
                        pthread_mutex_unlock(&g_sd_mode_mu); return NULL;
                    }
                    coldplug_log("SD direct-mounted (V2.40 vfat)"); pthread_mutex_unlock(&g_sd_mode_mu); return NULL;
                }
                if(!sd_cold_mount_allowed()){      /* window elapsed / card exported -> give up (one-shot) */
                    pthread_mutex_unlock(&g_sd_mode_mu);
                    coldplug_log("V2.40 SD one-shot: window elapsed or card exported (SD not mounted)");
                    return NULL;
                }
                { char b[96]; snprintf(b, sizeof b, "SD direct-mount failed errno=%d(%s)", errno, strerror(errno)); coldplug_log(b); }
            }
        }
        pthread_mutex_unlock(&g_sd_mode_mu);
        struct timespec ts = { 3, 0 }; nanosleep(&ts, NULL);
    }
    coldplug_log("coldplug TIMEOUT - SD never mounted");
    return NULL;
}
/* Only nudge on a genuine COLD boot, while the player's SD listener is not yet bound (~first 100s).
 * On a late/bare mq_ui RESTART the listener is long up (a normal insert auto-mounts) AND the device
 * may be in Storage/USB-DAC mode with the card EXPORTED to a USB host - re-emitting the disk 'add'
 * then risks concurrent host/device access + SD corruption. Gate on system uptime (a restart-proof
 * proxy for "cold boot") plus the Local-mode mirror. */
static int coldplug_should_run(void){
    double up = -1.0;
    FILE *u = fopen("/proc/uptime", "r");
    if(u){ if(fscanf(u, "%lf", &up) != 1) up = -1.0; fclose(u); }
    /* fail-closed: an unreadable/malformed uptime is NOT treated as a cold boot (0 would be fail-open,
     * enabling the SD mount on a late restart where the card may be exported to a host). */
    if(up < 0.0 || up > 150.0){ coldplug_log("skip: uptime unreadable or late (not a verified cold boot)"); return 0; }
    if(ui_get_source_mode() != 0 && ui_get_source_mode() != 4){ coldplug_log("skip: not in Local/Host mode (card may be exported to a host)"); return 0; }
    return 1;
}
static void coldplug_start(void){
    if(!coldplug_should_run()) return;
    pthread_t th;
    if(pthread_create(&th, NULL, coldplug_thread, NULL) == 0) pthread_detach(th);
}

/* ---- stock-player launcher prep + SD guard ---------------------------------
 * The stock mount routine ignores failed unmounts before recursive cleanup.
 * Rebuilt images guard /bin/rm itself, plus the first PATH entry, including stock
 * fallback/watchdog launches. Hand-deploys can establish a verified NAND fallback.
 * The launcher verifies protection and resets/reads back persisted local mode
 * itself before exec. Failure holds the launcher instead of starting the player.
 */
#define RMGUARD_ROOTFS     "/opt/diskos/bin"
#define RMGUARD_SYSTEM     "/bin"
#define RMGUARD_FALLBACK   "/usr/data/diskos/bin"
static const char RMGUARD_SCRIPT[] =
    "#!/bin/sh\n"
    "# diskOS SD guard - installed read-only at /opt/diskos/bin/rm and put FIRST in PATH by the boot hook.\n"
    "#\n"
    "# The stock player prepares the card with \"umount /tmp/sdcard\" followed by \"rm -rf /tmp/sdcard\" and never\n"
    "# checks whether the umount succeeded. When the card is busy (still mounted) the delete runs on the\n"
    "# mounted card and empties it. This wrapper refuses any rm whose operand is the card mountpoint or\n"
    "# anything under it; the exact mountpoint gets the stock's legitimate \"remove a stale empty dir\" via a\n"
    "# NON-recursive rmdir (EBUSY while mounted, fine when empty and unmounted). Everything else is passed to\n"
    "# the real rm unchanged. The decision is made before any logging; logging is best-effort and can never\n"
    "# fall through to the real rm. Canonical paths also cover relative paths, repeated slashes,\n"
    "# dot components and symlink aliases. This is a guard for rm, not a block-device write filter.\n"
    "# Logging is evidence, not a prerequisite: it writes to NAND, and the caller here is the stock player\n"
    "# performing its mount cleanup, so a stalled write would stall THAT. Detached and size-capped, it can\n"
    "# never delay the decision above, which has already been made by the time this runs.\n"
    "log_bg() {\n"
    "    { [ -f /usr/data/diskos_rmguard.log ] && [ \"$(/bin/busybox wc -c < /usr/data/diskos_rmguard.log 2>/dev/null)\" -gt 65536 ]; } \\\n"
    "        || echo \"$(/bin/busybox date '+%F %T' 2>/dev/null) $1\" >> /usr/data/diskos_rmguard.log 2>/dev/null\n"
    "}\n"
    "\n"
    "options=1\n"
    "for a in \"$@\"; do\n"
    "    if [ \"$options\" = 1 ]; then\n"
    "        case \"$a\" in --) options=0; continue ;; -*) continue ;; esac\n"
    "    fi\n"
    "    resolved=$(/bin/busybox readlink -f -- \"$a\" 2>/dev/null)\n"
    "    for operand in \"$a\" \"$resolved\"; do\n"
    "    case \"$operand\" in\n"
    "        /tmp/sdcard|/tmp/sdcard/)\n"
    "            /bin/busybox rmdir /tmp/sdcard 2>/dev/null\n"
    "            log_bg \"rmdir-only: rm $*\" &\n"
    "            exit 0 ;;\n"
    "        /|/tmp|/tmp/|/tmp/sdcard/*)\n"
    "            log_bg \"blocked: rm $*\" &\n"
    "            exit 0 ;;\n"
    "    esac\n"
    "    done\n"
    "done\n"
    "exec /bin/busybox rm \"$@\"\n"
    "";
static int rmguard_dir_ok(const char *dir){
    char p[256];
    int n = snprintf(p, sizeof p, "%s/rm", dir);
    if(n < 0 || n >= (int)sizeof p) return 0;
    /* An executable called rm may be the real destructive command. Verify the complete
     * wrapper, including existing fallback copies; a header alone is not sufficient. */
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if(fd < 0) return 0;
    struct stat sb;
    const size_t len = sizeof RMGUARD_SCRIPT - 1;
    int ok = fstat(fd, &sb) == 0 && S_ISREG(sb.st_mode) &&
             sb.st_size == (off_t)len && access(p, X_OK) == 0;
    size_t pos = 0;
    char buf[512];
    while(ok && pos < len){
        size_t want = len - pos;
        if(want > sizeof buf) want = sizeof buf;
        ssize_t got = read(fd, buf, want);
        if(got < 0 && errno == EINTR) continue;
        if(got <= 0 || memcmp(buf, RMGUARD_SCRIPT + pos, (size_t)got) != 0){ ok = 0; break; }
        pos += (size_t)got;
    }
    close(fd);
    return ok && pos == len;
}
/* Publish the fallback guard on /usr/data (tmp + fsync + rename) and VERIFY it (exact content + exec bit). */
static int rmguard_write_fallback(void){
    mkdir("/usr/data/diskos", 0755); mkdir(RMGUARD_FALLBACK, 0755);
    const size_t len = sizeof RMGUARD_SCRIPT - 1;
    char tmp[256], dst[256];
    int nt = snprintf(tmp, sizeof tmp, "%s/.rm.XXXXXX", RMGUARD_FALLBACK);
    int nd = snprintf(dst, sizeof dst, "%s/rm", RMGUARD_FALLBACK);
    if(nt < 0 || nt >= (int)sizeof tmp || nd < 0 || nd >= (int)sizeof dst) return 0;
    /* UI startup and the player launcher may publish concurrently. Each owns its
     * temporary file, then atomically replaces the same verified destination. */
    int fd = mkstemp(tmp);
    if(fd < 0) return 0;
    int ok = fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
    size_t pos = 0;
    while(ok && pos < len){
        ssize_t w = write(fd, RMGUARD_SCRIPT + pos, len - pos);
        if(w < 0 && errno == EINTR) continue;
        if(w <= 0){ ok = 0; break; }
        pos += (size_t)w;
    }
    if(ok) ok = fchmod(fd, 0755) == 0 && fsync(fd) == 0;
    if(close(fd) != 0) ok = 0;
    if(!ok || rename(tmp, dst) != 0){ unlink(tmp); return 0; }
    int dfd = open(RMGUARD_FALLBACK, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if(dfd < 0) return 0;
    ok = fsync(dfd) == 0;
    close(dfd);
    return ok && rmguard_dir_ok(RMGUARD_FALLBACK);
}
/* Ensure an executable guard exists and PATH starts with its directory. Returns 1 if guarded, 0 if NO
 * guard could be established (caller decides the fail policy). Idempotent; safe to call from the UI and
 * from the launcher. Only mutates PATH of THIS process (and, for the launcher, the exec'd player). */
static int rmguard_ensure(void){
    const char *cur = getenv("PATH");
    if(!cur || !*cur) cur = "/bin:/sbin:/usr/bin:/usr/sbin";
    const char *gdir = NULL;
    if(rmguard_dir_ok(RMGUARD_ROOTFS)) gdir = RMGUARD_ROOTFS;
    else if(rmguard_dir_ok(RMGUARD_SYSTEM)) gdir = RMGUARD_SYSTEM;
    else if(rmguard_dir_ok(RMGUARD_FALLBACK) || rmguard_write_fallback()) gdir = RMGUARD_FALLBACK;
    if(!gdir) return 0;
    size_t glen = strlen(gdir);
    if(strncmp(cur, gdir, glen) == 0 && (cur[glen] == ':' || cur[glen] == '\0')) return 1;
    char np[1024]; int n = snprintf(np, sizeof np, "%s:%s", gdir, cur);
    if(n <= 0 || n >= (int)sizeof np) return 0;
    return setenv("PATH", np, 1) == 0;
}
static int player_config_absent_without_card(void){
    struct stat sb;
    if(stat("/usr/data/fiio/db/sysconfig.db", &sb) == 0 || errno != ENOENT) return 0;
    /* Let stock create its full default schema only with the card removed.
     * A corrupt/unreadable existing database never takes this first-run path. */
    return stat("/sys/class/block/mmcblk0", &sb) != 0 && errno == ENOENT;
}
/* ---- launch outcome, published by the player launcher for the UI ------------------------------
 * On tmpfs, so its lifetime is exactly one boot and it can never carry a stale success into the next.
 * The UI treats "absent" and "unreadable" as NOT safe, so a launcher that never got to publish (crash,
 * deadline, read-only /tmp) keeps diskOS's own SD features off without ever delaying the player. */
#ifndef LAUNCH_STATUS_PATH
#define LAUNCH_STATUS_PATH "/tmp/.diskos_launch_status"
#endif
#ifndef PLAYER_COMM_NAME
#define PLAYER_COMM_NAME "mq_player"   /* what the launcher becomes after exec; overridden by tests */
#endif
#ifndef BOOT_LOG_PATH
#define BOOT_LOG_PATH "/usr/data/diskos_boot.log"
#endif
#ifndef LAUNCH_DEADLINE_S
#define LAUNCH_DEADLINE_S  20      /* hard cap on the WHOLE preparation path before we exec stock */
#endif

/* Invalidate any record from an EARLIER player before we start preparing. Without this, player A's
 * success stays readable while player B is still deciding - and a crash, a deadline or a failed publish
 * would leave A's success standing in for B. */
static void launch_status_invalidate(void){ unlink(LAUNCH_STATUS_PATH); }

static void launch_status_publish(int guard, int local){
    char buf[96];
    int n = snprintf(buf, sizeof buf, "guard=%d local=%d pid=%ld\n", guard, local, (long)getpid());
    if(n <= 0 || n >= (int)sizeof buf) return;
    char tmp[sizeof LAUNCH_STATUS_PATH + 16];
    if(snprintf(tmp, sizeof tmp, "%s.XXXXXX", LAUNCH_STATUS_PATH) >= (int)sizeof tmp) return;
    int fd = mkstemp(tmp);              /* unique: a shared name is unsafe with overlapping writers */
    if(fd < 0) return;
    size_t len = (size_t)n, pos = 0;
    while(pos < len){ ssize_t w = write(fd, buf+pos, len-pos); if(w < 0 && errno == EINTR) continue; if(w <= 0) break; pos += (size_t)w; }
    int ok = (pos == len) && (fchmod(fd, 0644) == 0);
    close(fd);
    if(!ok || rename(tmp, LAUNCH_STATUS_PATH) != 0) unlink(tmp);
}
/* 1 = THIS player's launch was confirmed; 0 = it was reported unprotected/unconfirmed; -1 = not published
 * yet, or published by a player that is no longer the one running.
 *
 * tmpfs gives the record a BOOT lifetime, which is not the same as a PLAYER-GENERATION lifetime: the
 * firmware watchdog can respawn the stock player directly, bypassing our launcher entirely, and that
 * player inherits no verdict. So the recorded pid is verified to still be the live mq_player. Parsing is
 * strict - a substring match would accept "guard=10". */
static int launch_status_get(void){
    int fd = open(LAUNCH_STATUS_PATH, O_RDONLY|O_CLOEXEC);
    if(fd < 0) return -1;
    char buf[96]; ssize_t got = read(fd, buf, sizeof buf - 1); close(fd);
    if(got <= 0) return -1;
    buf[got] = 0;
    int guard = -1, local = -1, used = -1; long pid = -1;
    /* %n gives the consumed length, so trailing junk ("pid=123junk") is rejected rather than ignored. */
    if(sscanf(buf, "guard=%d local=%d pid=%ld%n", &guard, &local, &pid, &used) != 3) return -1;
    if(used < 0 || guard < 0 || guard > 1 || local < 0 || local > 1 || pid <= 0) return -1;
    for(const char *t = buf + used; *t; t++) if(*t != '\n' && *t != ' ' && *t != '\t') return -1;
    /* identity: the launcher execs INTO the stock player, so the recorded pid is the player's pid */
    char cp[64]; snprintf(cp, sizeof cp, "/proc/%ld/comm", pid);
    int cfd = open(cp, O_RDONLY|O_CLOEXEC);
    if(cfd < 0) return -1;                       /* that player is gone -> no verdict applies */
    char cn[32]; ssize_t cg = read(cfd, cn, sizeof cn - 1); close(cfd);
    if(cg <= 0) return -1;
    cn[cg] = 0;
    char *nl = strchr(cn, '\n'); if(nl) *nl = 0;
    if(strcmp(cn, PLAYER_COMM_NAME) != 0) return -1;  /* pid reused by something else */
    /* A zombie keeps its name, so it would otherwise pass as a live player. */
    { char sp[64]; snprintf(sp, sizeof sp, "/proc/%ld/stat", pid);
      int sfd = open(sp, O_RDONLY|O_CLOEXEC);
      if(sfd < 0) return -1;
      char sb[256]; ssize_t sg = read(sfd, sb, sizeof sb - 1); close(sfd);
      if(sg <= 0) return -1;
      sb[sg] = 0;
      const char *rp = strrchr(sb, ')');                 /* comm can contain spaces; state follows ") " */
      if(!rp || !rp[1] || !rp[2]) return -1;
      if(rp[2] == 'Z' || rp[2] == 'X') return -1;        /* zombie/dead is not a running player */ }
    /* PIN the generation: once a verdict has been accepted, a DIFFERENT player may not inherit it.
     * (A replacement writes its own record, or none at all if the firmware watchdog respawned stock
     * directly - either way this rejects it and the gate timer revokes.) */
    { static long pinned = 0;
      if(pinned == 0) pinned = pid;
      else if(pinned != pid) return -1; }
    return (guard == 1 && local == 1) ? 1 : 0;
}
/* Bounded, best-effort APPEND to the boot log. Usable before stderr is redirected (the launcher runs
 * long before that) and it never truncates, so a launcher note survives the UI opening the same file. */
static void boot_note(const char *msg){
    int fd = open(BOOT_LOG_PATH, O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC, 0644);
    if(fd < 0) return;
    (void)write(fd, msg, strlen(msg));
    (void)write(fd, "\n", 1);
    close(fd);
}
/* Local ownership confirmed? Deliberately SEPARATE from the guard check: the two failures are
 * independent, mean different things, and are reported independently (a single boolean lost that). */
static int player_local_mode_confirmed(void){
    /* Check actual host ownership; a saved database mode is not a detach acknowledgement. */
    if(sd_exported_to_host()) return 0;
    if(player_config_absent_without_card()) return 1;
    /* Fresh, bounded reset and read-back in the launcher itself. An old marker from
     * another UI instance cannot authorize this launch, and elapsed time is not success. */
    char *a[] = { "sh", "-c",
        "[ -f /usr/data/fiio/db/sysconfig.db ] && "
        "sqlite3 -cmd '.timeout 3000' /usr/data/fiio/db/sysconfig.db 'UPDATE SYSCONFIG SET WORK_MODE=0 WHERE ID=1 AND WORK_MODE IS NOT 0' && "
        "[ \"$(sqlite3 -cmd '.timeout 3000' /usr/data/fiio/db/sysconfig.db 'SELECT WORK_MODE FROM SYSCONFIG WHERE ID=1')\" = \"0\" ]", NULL };
    if(run_bounded(a, 8000) != 0) return 0;
    return !sd_exported_to_host();
}
/* Normalise signal state before ANY exec. Three separate problems, all of which outlive the exec:
 *  - a pending alarm is INHERITED, so our watchdog could fire inside the program we hand off to;
 *  - alarm(0) does not discard a signal that has ALREADY been generated, so we set SIG_IGN and unblock,
 *    which discards it, before restoring the default;
 *  - SIG_IGN itself is inherited across exec, so the disposition must end at SIG_DFL, and the blocked
 *    mask must be cleared (a handler runs with its own signal blocked, so exec'ing from inside one
 *    would otherwise hand the stock binary a blocked SIGALRM).
 * Every call here is async-signal-safe, so this is valid from a handler as well as the normal path. */
static void handoff_signal_reset(void){
    alarm(0);
    signal(SIGALRM, SIG_IGN);
    sigset_t s; sigemptyset(&s); sigaddset(&s, SIGALRM);
    sigprocmask(SIG_UNBLOCK, &s, NULL);   /* discards an already-generated SIGALRM */
    signal(SIGALRM, SIG_DFL);             /* SIG_IGN would survive the exec */
}
/* The ONE place that hands off to the stock player. Kills any preparation child first: a shell or
 * sqlite left running would otherwise keep mutating state after stock has started. */
static void exec_stock_player(void){
    if(g_bounded_pgid > 0) kill(-(pid_t)g_bounded_pgid, SIGKILL);
    if(g_bounded_pid > 0) kill((pid_t)g_bounded_pid, SIGKILL);   /* the child itself, grouped or not */
    handoff_signal_reset();
    char *pa[2]; pa[0] = (char*)"mq_player"; pa[1] = NULL;
    execv("/usr/bin/mq_player", pa);
    _exit(127);                            /* exec failed: let the watchdog respawn stock */
}
/* Deadline handler. Deliberately does NO I/O: a write to the boot log or to stderr can itself block on
 * the very storage that wedged preparation, which would defeat the whole point of having a deadline. */
static void launch_deadline_exec(int sig){
    (void)sig;
    exec_stock_player();
}
/* HARD RULE (owner constraint, 2026-09-23): this must NEVER prevent the stock player from running.
 * diskOS may switch off its OWN features; it may not take away the user's player or their route back
 * to stock firmware. The earlier version ended in for(;;)pause() while keeping the name mq_player, so
 * the watchdog saw a live player and never respawned stock - a transient database lock could therefore
 * deny the device a player until it was reflashed, and it also broke the "Default UI = Stock" and
 * hold-Vol-Up recovery paths, which only redirect the UI and still route the PLAYER through us.
 *
 * Now: bounded attempts under an overall deadline, publish the outcome for the UI, always fall through
 * to the stock exec. An unprotected launch is NOT claimed to be safe - it is unprotected recovery, and
 * the UI says so - but it is strictly the user's device to run. */
static void player_launch_prepare(void){
    launch_status_invalidate();            /* no earlier player's verdict may stand in for this one */
    { struct sigaction sa; memset(&sa, 0, sizeof sa);
      sa.sa_handler = launch_deadline_exec; sigemptyset(&sa.sa_mask);
      sa.sa_flags = 0;                     /* no SA_RESTART: blocking syscalls return EINTR */
      sigaction(SIGALRM, &sa, NULL); alarm(LAUNCH_DEADLINE_S); }

    int guard = 0, local = 0;
    for(int attempt = 0; attempt < 2; attempt++){
        if(attempt) usleep(250000);        /* one retry for a transient db lock / startup overlap */
        if(!guard) guard = rmguard_ensure();
        if(!local) local = player_local_mode_confirmed();
        if(guard && local) break;
    }
    /* The diagnostics stay INSIDE the deadline. They touch NAND, so a stalled write here would
     * otherwise recreate exactly the failure this deadline exists to prevent: a live launcher that
     * never reaches the stock player. If the alarm fires during them, the handler execs stock and the
     * missing note is the acceptable loss. alarm(0) happens in exec_stock_player(), not here. */
    launch_status_publish(guard, local);
    if(!guard)      boot_note("launcher: SD guard unavailable - stock player launched UNPROTECTED; diskOS SD features held");
    else if(!local) boot_note("launcher: local ownership unconfirmed - stock player launched; diskOS SD features held");
}

/* Start the SD-backed features (first-run scan, cover prewarm) ONLY once the player launcher has
 * confirmed both the delete guard and local ownership. We run before the player, so our own checks
 * passing is not sufficient evidence that the launch was protected. A failure verdict, or no verdict
 * inside the window, holds diskOS's SD features for the boot; it never affects the player. */
static void sd_degraded_notice_cb(lv_timer_t *t){
    (void)t;
    if(g_sd_hold) ui_toast("SD protection unavailable - library and artwork are off");
}
static void sd_features_gate_cb(lv_timer_t *t){
    static int waited = 0, started = 0;
    int v = launch_status_get();
    if(v < 0 && !started){
        if(++waited < 60) return;                    /* ~30s for a launcher that starts 2s after us */
        v = 0;                                       /* no NAND diagnostic on the UI thread (see below) */
    }
    if(v == 1){
        if(!started){
            started = 1;
            atomic_store(&g_launch_verdict, 1);      /* this OPENS admission: storage_tick resumes leases
                                                      * and runs the first-run scan through its own gate */
            coldplug_start();                        /* auto-mount the already-inserted card */
            ui_art_retry();                          /* a cover asked for before this point failed: ask again */
            ui_start_art_prewarm();
            albumwall_prewarm_seed();
            lv_timer_set_period(t, 5000);            /* keep re-validating, just less often */
        }
        return;
    }
    /* Negative, OR the player that earned the verdict is gone. The firmware watchdog can respawn the
     * stock player directly, which never runs our launcher and so inherits no verdict - we cannot prove
     * what that replacement did, so this is a ONE-WAY revocation for the rest of the boot. Revoking means
     * closing admission AND stopping work already in flight, not just setting a flag. */
    atomic_store(&g_launch_verdict, 0);
    g_sd_hold = 1;
    atomic_store(&g_sd_writable, 0);
    sd_io_hold();                                    /* no new leases */
    /* Closing admission does NOT stop work already running: a scan holds ONE lease for its whole walk,
     * and prewarm/fallback decoders are deliberately non-cancellable. Both have to be stopped explicitly
     * or they keep reading the card after we have declared it unsafe. */
    scanner_abort();
    art_kill_all();
    g_sd_phase = SD_UNKNOWN;
    g_play_pending = 0;                              /* the pending-playback repair would re-drive the player */
    /* No NAND diagnostic here: this runs on the UI thread with no deadline, and the earlier version forked
     * an unsupervised child for it that kept the UI's identity. The on-screen notice is the report. */
    ui_toast("SD protection unavailable - library and artwork are off");
    lv_timer_create(sd_degraded_notice_cb, 300000, NULL);   /* recurring, not a single toast */
    lv_timer_del(t);
}

/* ---- boot select record (tests slice between these markers - keep them) ----
 * S96diskos_select writes exactly "diskos\n" or "stock\n". Returns 1 only for a regular, non-symlink file
 * holding exactly those 7 bytes "diskos\n" - the same rule as payload/diskos-selected, the shell reader
 * used by S97 and the boot hook, so the three consumers cannot disagree about one record. (The shell side
 * needs the exact-size rule because BusyBox ash `read` drops NUL bytes.) Missing, unreadable, FIFO/device,
 * wrong size, NUL-containing or any other content is 0 = stock. Non-blocking open so a FIFO planted at the
 * path cannot stall us. */
static int boot_select_is_diskos(const char *path){
    char rec[8] = {0};
    ssize_t n = -1;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if(fd < 0) return 0;
    struct stat st;
    if(fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 7)
        n = read(fd, rec, sizeof rec);
    close(fd);
    return n == 7 && memcmp(rec, "diskos\n", 7) == 0;
}

/* Enforce the record. Anything but a positive diskOS record hands this process to the stock UI, and that
 * is TERMINAL: if the stock exec fails we exit rather than run diskOS against the boot's decision.
 * fiio_init's watchdog then sees no mq_ui and respawns the stock UI by its bare name. */
static void boot_select_enforce(const char *path){
    if(boot_select_is_diskos(path)) return;
    handoff_signal_reset();                  /* exec KEEPS a pending timer AND a blocked mask -
                                              * neither may cross into the stock UI */
    char *sa[2]; sa[0]="mq_ui"; sa[1]=NULL;
    execv("/usr/bin/mq_ui", sa);
    _exit(127);                              /* never fall through into diskOS */
}
/* ---- end boot select record ---- */

/* Apply a theme change: re-launch the UI (same PID, same argv[0]="mq_ui", so the boot watchdog never sees a gap), at
 * `screen` (a deep-link name). The player is a separate process and keeps playing. Refused while a library scan is
 * running (its transaction must finish or roll back first). If the exec fails nothing is lost: every fd was only
 * marked close-on-exec, so this process keeps running and the change applies at the next start. */
int ui_theme_reload(const char *screen){
    if(scanner_active()){ ui_toast("Library scan running - try again when it finishes"); return -1; }
    art_kill_all();
    if(g_bounded_pgid > 0) kill(-(pid_t)g_bounded_pgid, SIGKILL);
    if(g_bounded_pid > 0) kill((pid_t)g_bounded_pid, SIGKILL);
    for(int fd = 3; fd < 1024; fd++){ int fl = fcntl(fd, F_GETFD); if(fl >= 0) fcntl(fd, F_SETFD, fl | FD_CLOEXEC); }
    fflush(stderr);
    handoff_signal_reset();
    setenv("DISKOS_THEME_RELOAD", "1", 1);
    char *ua[3]; ua[0] = "mq_ui"; ua[1] = (char *)(screen ? screen : "home"); ua[2] = NULL;
    /* Re-exec the image that is RUNNING (the launcher's verified private copy, or whichever build fell back), never the
     * mutable /usr/data/mq_ui pathname: that could swap a running fallback for the unverified/trial file. /proc/self/exe
     * still works when the running file was unlinked. The boot markers (/tmp/.diskos_run, .diskos_launched) describe this
     * same build, so they are neither rewritten nor re-checked here. A failed exec keeps us running (no path fallback). */
    execv("/proc/self/exe", ua);
    unsetenv("DISKOS_THEME_RELOAD");
    alarm(0);
    ui_toast("Couldn't apply the theme now - it applies next start");
    return -1;
}
int main(int argc, char **argv){
    /* Arm the boot watchdog BEFORE anything else - in particular before the argv dispatch and the
     * stock-UI selection below. Those read /usr/data and /dev/mem, so they can block; until now they
     * ran with no deadline at all, and a hang there took away the user's route to the stock firmware
     * with no recovery but a reflash. On timeout the handler _exit()s, fiio_init's `pgrep -x mq_ui`
     * misses us, and its watchdog relaunches the bare name - which resolves to the STOCK UI. So a wedge
     * here now ends up in stock rather than nowhere. Re-armed again below for the init phase proper. */
    { struct sigaction al; memset(&al,0,sizeof al);
      al.sa_handler = boot_alarm_handler; sigemptyset(&al.sa_mask); al.sa_flags = 0;
      sigaction(SIGALRM,&al,NULL); alarm(45); }
#ifdef DISKOS_DIAG_FBMARK
    /* DIAGNOSTIC: paint the framebuffer magenta the instant our main() runs, so a hung boot can be
     * told apart by eye: WHITE (rcS done) but no magenta => our binary never reached main(); magenta
     * but no real UI => we reached main() and died during init. Never ship. */
    system("/bin/sh /etc/diag_fb.sh magenta");
#endif
    /* argv[0] dispatch for the read-only fiio_init watchdog.
     * busybox `pgrep -x NAME` matches the FULL cmdline (argv[0]), not comm, and
     * the watchdog greps bare "mq_ui"/"mq_player". fiio_init launches the override
     * with FULL paths (/usr/data/mq_ui, /usr/data/mq_player) -> pgrep -x never
     * matches -> it kills us and respawns stock. So normalise argv[0] to the bare
     * name the watchdog expects:
     *   - invoked as .../mq_player (the player symlink -> this binary): exec the
     *     real stock player with argv[0]="mq_player".
     *   - invoked as anything other than exactly "mq_ui": re-exec self with
     *     argv[0]="mq_ui" so `pgrep -x mq_ui` matches. */
    {   const char *a0 = (argv[0] && argv[0][0]) ? argv[0] : "mq_ui";
        const char *slash = strrchr(a0,'/');
        const char *base = slash ? slash+1 : a0;
        if(strcmp(base,"mq_player")==0){
            if(strcmp(a0,"mq_player") != 0){
                /* argv strings are writable. Normalize before any safe hold,
                 * without re-opening a NAND pathname that could disappear. */
                size_t old_len = strlen(argv[0]);
                memmove(argv[0], "mq_player", sizeof "mq_player");
                memset(argv[0] + sizeof "mq_player", 0, old_len + 1 - sizeof "mq_player");
            }
            player_launch_prepare();   /* verified guard + fresh local-mode confirmation */
            exec_stock_player();       /* single handoff: child cleanup + signal normalisation + exec */
        }
        if(strcmp(a0,"mq_ui")!=0){
            char *ua[3]; ua[0]="mq_ui"; ua[1]=(argc>1?argv[1]:NULL); ua[2]=NULL;
            handoff_signal_reset();         /* the pending 45s timer must not cross into the new image */
            execv("/proc/self/exe", ua);    /* the running image (same bytes), not a mutable path; fall through and run if exec fails */
            alarm(45);                      /* exec failed: we keep running, so re-arm */
        }
    }
    /* Boot-default UI + Vol-Up override: CONSUME the boot's one decision, never re-sample it.
     * S96diskos_select read /usr/data/boot_default_stock and the Vol-Up pin once, before any diskOS code
     * ran, and wrote exactly "diskos" or "stock" to /tmp/.diskos_boot_select (tmpfs: this boot only).
     * We used to sample the pin again here, which could disagree with S96: default Stock + Vol-Up held
     * selected diskOS at S96, the user let go while S97 ran, and we switched back to Stock.
     * FAILURE POLICY matches S96/S97/the boot hook: only a positive "diskos" record runs diskOS; missing,
     * unreadable, non-regular or malformed means stock. The read is non-blocking and tmpfs-only, and the
     * 45s boot watchdog above is already armed if anything here stalls. */
    {
        /* Diagnostic, gated behind /usr/data/volup_dbg so a release image logs nothing by default. */
        FILE *bl = (access("/usr/data/volup_dbg", F_OK) == 0) ? fopen("/usr/data/volup_boot.log", "a") : NULL;
        if(bl){
            fprintf(bl, "boot select record run_diskos=%d\n", boot_select_is_diskos("/tmp/.diskos_boot_select"));
            fclose(bl);
        }
        boot_select_enforce("/tmp/.diskos_boot_select");
    }
    /* Boot diagnostics: route BOTH stdout and stderr through ONE fd (dup2) so they
     * share a file offset and don't clobber each other; line-buffered so the last
     * step before a crash is flushed. Truncates each launch (boot launch is what
     * we care about; avoids unbounded NAND growth). */
    /* APPEND, so a launcher note written before this point survives; rotate once when it grows past a
     * cap rather than truncating on every launch (which lost the previous boot AND the launcher's lines). */
    { struct stat lb; if(stat("/usr/data/diskos_boot.log", &lb) == 0 && lb.st_size > 256*1024)
          freopen("/usr/data/diskos_boot.log", "w", stderr);
      else freopen("/usr/data/diskos_boot.log", "a", stderr); }
    dup2(fileno(stderr), fileno(stdout));
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    { static char altstk[32768];   /* run the handler on its own stack so a stack-overflow crash can still report */
      stack_t ss; ss.ss_sp = altstk; ss.ss_size = sizeof altstk; ss.ss_flags = 0; sigaltstack(&ss, NULL);
      struct sigaction sa; memset(&sa, 0, sizeof sa);
      sa.sa_sigaction = crash_log; sa.sa_flags = SA_SIGINFO | SA_ONSTACK; sigemptyset(&sa.sa_mask);
      sigaction(SIGSEGV,&sa,NULL); sigaction(SIGBUS,&sa,NULL);
      sigaction(SIGABRT,&sa,NULL); sigaction(SIGFPE,&sa,NULL); }
    /* Boot-hang backstop: if we don't reach the main loop + first paint within
     * BOOT_DEADLINE_S, SIGALRM _exit()s us so fiio_init respawns a fresh mq_ui
     * (an alive-but-hung mq_ui is invisible to its `pgrep -x` watchdog). The
     * ceiling is generous so a slow-but-progressing boot never trips it; it's
     * disarmed (alarm(0)) the instant the first frame is on screen. */
    #define BOOT_DEADLINE_S 45
    { struct sigaction al; memset(&al,0,sizeof al);
      al.sa_handler = boot_alarm_handler; sigemptyset(&al.sa_mask); al.sa_flags = 0;
      sigaction(SIGALRM,&al,NULL); alarm(BOOT_DEADLINE_S); }
    fprintf(stderr,"=== diskos main start argc=%d ===\n", argc); fflush(stderr);
    /* WiFi bring-up + keep-alive is handled by wifi_supervise() in the main loop (which
     * also restarts the supplicant if it dies, so a known net returning in range auto-
     * joins). Intent is seeded from SYSCONFIG.WIFI_STATUS after cfg_load, below. */
    /* Default the persisted work-mode to 0 (Local) at boot. The stock player's startup jump
     * table (RE 2026-09-05) maps SYSCONFIG.WORK_MODE -> USB gadget: 4=mass-storage EXPORT of the
     * SD, 1=USB-DAC, 0/3/5=Local/no gadget. diskOS previously forced 4 believing it was
     * "LOCALPLAYER", which silently exported the card whenever a cable was attached at boot.
     * Runtime local AUDIO is asserted separately via 0666+0657 (localplayer_workmode_cb), so 0 here does
     * not affect playback. The player launcher independently resets this before stock exec; this
     * is the belt-and-suspenders for hand-deploys. USB-DAC/Storage stay opt-in per session. */
    { char *a[] = { "sh", "-c",
                    "[ -f /usr/data/fiio/db/sysconfig.db ] && sqlite3 -cmd '.timeout 3000' /usr/data/fiio/db/sysconfig.db 'UPDATE SYSCONFIG SET WORK_MODE=0 WHERE ID=1 AND WORK_MODE IS NOT 0'", NULL };
      run_bounded(a, 8000);   /* bounded: a locked/corrupt sysconfig.db can't wedge boot */ }
    /* Confirm the row rather than trusting an UPDATE that may have matched no row.
     * Unconfirmed -> hold this UI's SD activity for the boot. The player launcher
     * performs its own fresh reset/read-back instead of trusting a shared marker. */
    { char *v[] = { "sh", "-c",
                    "[ -f /usr/data/fiio/db/sysconfig.db ] && [ \"$(sqlite3 -cmd '.timeout 3000' /usr/data/fiio/db/sysconfig.db 'SELECT WORK_MODE FROM SYSCONFIG WHERE ID=1')\" = \"0\" ]", NULL };
      if(run_bounded(v, 5000) != 0){ g_sd_hold = 1; fprintf(stderr,"WORK_MODE reset UNCONFIRMED -> diskOS SD activity held this boot\n"); fflush(stderr); } }
    /* SD guard: establish it NOW (before the player is launched 2s after us) so the launcher finds it. If no
     * guard can exist on this system, hold our SD activity: nothing of ours may keep the card busy. */
    if(!rmguard_ensure()){ g_sd_hold = 1; fprintf(stderr,"SD guard could NOT be established -> diskOS SD activity held this boot\n"); fflush(stderr); }
    /* Start the SSH server at boot (if installed) so a wedged USB serial can never
     * strand dev access again: dropbear listens once wlan0 gets an address. Harmless
     * if /usr/data/sshd isn't present; start-ssh.sh no-ops if already running. */
    system("[ -x /usr/data/sshd/start-ssh.sh ] && /usr/data/sshd/start-ssh.sh >/dev/null 2>&1 &");
    (void)fw_os_ver();  /* populate fwcaps' static cache on THIS (main) thread before coldplug_start()
                         * creates the worker - pthread_create is a memory barrier, so the worker reads
                         * the already-initialised cache (no first-init data race between the threads). */
    storage_init();
    /* coldplug_start() now runs from sd_features_gate_cb, after the launcher publishes its verdict */
    { unsigned seed=0; FILE *r=fopen("/dev/urandom","rb"); if(r){ if(fread(&seed,1,sizeof seed,r)!=sizeof seed) seed=(unsigned)time(NULL); fclose(r);} else seed=(unsigned)time(NULL); srand(seed); }  /* seed RNG (shuffle start pos) */
    lv_init();
    /* /dev/fb0 may not be ready the instant fiio_init launches us at boot; retry. */
    int fbok=0;
    for(int i=0;i<20;i++){ if(fbpan_create("/dev/fb0")){ fbok=1; break; }
        fprintf(stderr,"fbpan try %d failed, retry\n",i); fflush(stderr); usleep(250000); }
    if(!fbok){ fprintf(stderr,"fbpan failed (gave up)\n"); return 1; }
    cfg_load();
    (void)route_state_restore();  /* restore the live player's route before BT-off/disconnect handlers run */
    fontft_init();
    theme_init();   /* the palette for this run: before any screen is built */
    swipe_thresh_load();
    int boot_br = read_int_file("/sys/class/backlight/backlight/brightness");   /* the player's value before ours */
    int startup_deferred = powerkey_defer_startup(boot_br);
    settings_apply_startup(!startup_deferred);   /* all settings now; late sleeping fallback keeps brightness 0 */
    if(boot_br == 0 && !startup_deferred && !fw_power_key_driveable())
        (void)ui_backlight_take_foreign(NULL, NULL);   /* discard only the zero our boot restore overwrote */
    wifi_init_intent();         /* seed wifi_on intent from stock WIFI_STATUS (first run only) */
    fprintf(stderr,"step:screens_init\n");fflush(stderr); screens_init();
    home_set_settings_click_cb(go_settings);
    library_set_song_click_cb(on_song_play);   /* tap a song -> play it */
    search_set_song_click_cb(on_search_play);   /* search result -> play in all-songs scope (no stale drill ctx) */
    { char *a[] = { "hwclock", "-s", NULL };
      run_bounded(a, 6000); }                    /* load system clock from RTC; bounded so a busy I2C/RTC can't wedge boot */
    lv_timer_create(hwclock_save_tick, 300000, NULL); /* write system time back to the RTC every 5 min */
    lv_timer_create(clock_tick, 10000, NULL);   /* refresh wall clock every 10s */
    lv_timer_create(book_tick, 400, NULL);   /* drive the audiobook session: confirm load, resume, checkpoint */
    clock_tick(NULL);                           /* set immediately */
    g_t_status = lv_timer_create(status_poll_cb, 3000, NULL); /* charging/battery/wifi every 3s (BT every ~12s inside); paused at screen-off */
    status_poll_cb(NULL);                        /* populate immediately */
    bt_boot_restore();                           /* re-enable BT + arm auto-route if it was on (persist like WiFi) */
    g_t_wx  = lv_timer_create(weather_poll, 1000, NULL);  /* apply weather + retry/refresh */
    if(cfg_get_int("weather_on", 1)) weather_fetch_async();   /* first fetch - only with Weather on (Settings off = no network use) */
    g_t_lyr = lv_timer_create(lyrics_poll, 500, NULL);    /* apply finished lyrics fetch */
    lastfm_init();                                        /* load Last.fm config + offline queue */
    lv_timer_create(lastfm_tick, 1000, NULL);            /* watch play-state + drive scrobbles */
    lv_timer_create(scanner_poll, 500, NULL);            /* apply a finished library rescan */
    if(!books_ensure_migrated())                         /* move any .m4b left in SONG out to BOOKS (upgrade / no-rescan devices) so books never sit in the music queue */
        lv_timer_create(migrate_books_retry_cb, 3000, NULL);   /* failed (transient reader lock) -> retry off the main loop until it succeeds */
    lv_timer_create(playlist_port_cb, 5000, NULL);       /* bring pre-1.2.0 / stock-migrated playlists into agreement (see playlist_port_cb) */
    lv_timer_create(sd_features_gate_cb, 500, NULL);     /* start SD-backed features only on a positive launcher verdict */
    g_t_art = lv_timer_create(ui_art_poll, 120, NULL);    /* apply finished album-art decode (worker thread) */
    lv_timer_create(localplayer_workmode_cb, 500, NULL);  /* solicit a2 and settle before startup route recovery / V2.40 local init */
    /* Background cover/accent prewarm. The worker self-gates on the user's "Album art
     * caching" setting (off/idle/charging) + a battery-temp throttle, so it's safe to
     * always spawn - it just sleeps while disabled or while the player is warm. */
    /* auto-scan + prewarm now start from sd_features_gate_cb, once the launcher verdict is known */
    /* Try to connect now; if the player's /ui queue isn't up yet (cold boot),
     * keep retrying in the background so the UI still comes up immediately. */
    if(ipc_start()!=0){
        fprintf(stderr,"ipc not ready, retrying in background\n"); fflush(stderr);
        lv_timer_create(ipc_connect_retry_cb, 1000, NULL);
    }
    /* Startup state-sync: ask the player to re-emit its a2 metadata for the LIVE
     * track so Now Playing is populated on launch (instead of "No Track").
     * 0202 (frame "02020008") is the decoded "request current state" command -
     * verified: player responds with an a202 full-metadata frame.
     * (MEMORY_PLAY was tried before and reverted: that row is the resume
     * BOOKMARK, not the live track, so it showed the wrong song/art/time.)
     * Sent once now (player already running in our launch) plus a one-shot retry
     * to cover the boot case where the player comes up slightly after the UI. */
    ipc_send_probe("02020008");   /* silent: pre-connect miss must not toast "Player didn't respond" */
    /* Retry a few times: at cold boot the player's /player queue appears a few
     * seconds after we start, so a single retry can miss it. */
    lv_timer_t *sync_t = lv_timer_create(state_sync_retry_cb, 2000, NULL);
    lv_timer_set_repeat_count(sync_t, 5);   /* 2,4,6,8,10s then auto-deletes */
    lv_timer_create(ipc_health_cb, 30000, NULL);   /* keep IPC warm + self-heal a player restart */

    /* Touch is default-ON: the whole UI is dead without it, and a fresh install carries no
     * /usr/data/touch_on marker, so opt-in gating meant a stock image booted untouchable.
     * /usr/data/touch_off is the recovery override (e.g. a wedged panel). */
    if(access("/usr/data/touch_off", 0)!=0){
        g_touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, "/dev/input/event1");
        /* 2nd RAW fd on the same evdev (evdev fans events out to every open fd) - drained each loop
         * purely to detect ANY touch for the screen-off wake, immune to LVGL's tap-coalescing. */
        g_touch_raw = open("/dev/input/event1", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if(g_touch){
            /* panel mounted 180deg, fb does reverse-copy -> invert both axes (or the stored Screen rotation's inverse) */
            fbpan_bind_touch(g_touch);
            /* cst816t is noisy on a small round panel: require more travel before a
             * press becomes a scroll, so deliberate taps aren't eaten as scrolls
             * (default scroll_limit=10). NB: scroll_throw is a slowdown-%, left at its
             * default - coast feel is a user-testing call, not safe to guess blind. */
            lv_indev_set_scroll_limit(g_touch, 18);
            g_dbg = (access("/usr/data/touch_dbg", 0)==0);
            if(g_dbg) dbgdot_init();
            fprintf(stderr,"touch ENABLED indev=%p dbg=%d thresh=%d\n",(void*)g_touch,g_dbg,g_swipe_thresh);
        } else fprintf(stderr,"touch create FAILED\n");
    } else fprintf(stderr,"touch OFF (no /usr/data/touch_on)\n");
    fflush(stderr);

    int start = SCR_HOME;   /* no-arg default = home (boot supervisor launches with no args) */
    if(argc>1){
        if(!strcmp(argv[1],"home"))    start = SCR_HOME;
        else if(!strcmp(argv[1],"library")) start = SCR_LIBRARY;
        else if(!strcmp(argv[1],"nowplaying")) start = SCR_NOWPLAYING;
        else if(!strcmp(argv[1],"settings")) start = SCR_SETTINGS;
        else if(!strcmp(argv[1],"workmode")) start = SCR_WORKMODE;
        else if(!strcmp(argv[1],"search")) start = SCR_SEARCH;
        else if(!strcmp(argv[1],"quick")) start = SCR_QUICK;
        else if(!strcmp(argv[1],"songinfo")) start = SCR_SONGINFO;
        else if(!strcmp(argv[1],"tune")) start = SCR_TUNE;
        else if(!strcmp(argv[1],"ctx")) start = SCR_NPMENU;
        else if(!strcmp(argv[1],"eq")) start = SCR_EQ;
        else if(!strcmp(argv[1],"apps")) start = SCR_APPS;
        else if(!strcmp(argv[1],"weather")) start = SCR_WEATHER;
        else if(!strcmp(argv[1],"lyrics")) start = SCR_LYRICS;
        else if(!strcmp(argv[1],"saver")) start = SCR_SAVER;
        else if(!strcmp(argv[1],"hub")) start = SCR_NPHUB;
        else if(!strcmp(argv[1],"upnext")) start = SCR_UPNEXT;
        else if(!strcmp(argv[1],"datetime")) start = SCR_DATETIME;
    }
    screen_show(start);
    /* Force ONE full-screen repaint at startup. The framebuffer may still hold the stock
     * boot splash (switching stock->diskOS, or our own relaunch); the partial-render engine
     * only copies widget-dirty areas, so background regions would show stale splash pixels
     * until the next full transition. Invalidating the whole active screen marks the entire
     * 360x360 dirty -> fb_pan copies the full frame -> clean screen from the first frame. */
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    if(argc>1 && !strcmp(argv[1],"wifi")) wifi_open();   /* deep-link: open Wi-Fi + scan */
    if(argc>1 && !strcmp(argv[1],"bt"))   bt_open();     /* deep-link: open Bluetooth + scan */
    if(argc>1 && !strcmp(argv[1],"lyrics")) lyrics_open(); /* deep-link: load + show lyrics */
    printf("diskOS up (screen %d)\n", start); fflush(stdout);

    track_state_t st; unsigned last=~0u;
    lv_indev_state_t prev_ts = LV_INDEV_STATE_RELEASED;
    int sx=0, sy=0, lastx=0, lasty=0; uint32_t sms=0; int s_press_scr=SCR_HOME;   /* swipe tracking + the screen the press began on */
    int cover_tap=0;   /* press landed on the NP cover -> LVGL's cover click opens full-screen art (not a seek) */
    int fsart_touch=0; /* press began while full-screen art was up -> LVGL's overlay click closes it; we only swallow */
    int sleep_touch=0; /* press began while the sleep popover was up -> swallow the whole gesture (no ring seek) */
    playstate_t g_ps = { -1, 0 }; int last_playing=-1;   /* play/pause inference (playstate.c, host-tested) */
    char np_last_path[256] = "";   /* last track whose art/backdrop was pushed - so a mere position tick doesn't re-invalidate the 360px backdrop */
    char np_last_title[sizeof st.title] = "", np_last_artist[sizeof st.artist] = "";
    /* screensaver + screen-off timeouts come from the Settings cyclers (index ->
     * seconds via TMAP), re-read each loop so changes apply live. */
    static const int TMAP[5] = {0,30,60,120,300};
    uint32_t last_activity = lv_tick_get();
    int woke = 0;   /* a press that only wakes the saver is swallowed */
    int bl_state = 0;   /* 0 = normal, 1 = saver-dim, 2 = off */
    uint32_t last_blpoll = 0;   /* PW-10 backlight-rail poll cadence */
    /* A late fallback zero stays untouched until the player's wake. Early boot zeros are treated as startup,
     * not sleep: light promptly and follow the next player write without retaining a stale sleep flag. */
    powerkey_t pk = { boot_br, boot_br == 0, 0 };   /* off_seq 0: no diskOS write yet when boot_br was read */
    if(boot_br == 0 && !startup_deferred){
        pk.seen = ui_backlight_written(); pk.player_off = 0;
    }
    if(startup_deferred){ ui_backlight(0); bl_state = 2; g_screen_off = 1; }
    /* V2.57: the player's own screen frames (aa23) drive the state; aa03 = it is powering off or prompting to (never
     * inject until the player restarts). The cursors start at 0 like the ipc counters, so frames that arrived during
     * startup are handled on the first pass, not skipped. */
    unsigned scr_seq_seen = 0, aa03_seq_seen = 0, screen_gen_seen = 0, inject_for = 0, inject_gen = 0;
    int poweroff_inhibit = 0, inject_pending = 0; uint32_t inject_due = 0;
    unsigned last_vol_seq = 0;
    /* Start the boot splash HERE - after all the blocking startup init (hwclock, bt_boot_restore,
     * IPC connect, etc.). Created earlier, its time-based animation would elapse during that
     * blocking init (the loop isn't ticking yet) and get skipped. Here it animates cleanly in the
     * loop from t=0. It's on lv_layer_top so it covers whatever's already drawn, then reveals it. */
    if(ui_source_switch_pending()) g_sd_deadline = lv_tick_get() + 20000;
    lv_timer_create(storage_tick, 100, NULL);
    { void system_ota_health_init(void); system_ota_health_init(); }   /* system.c: proves a trial OTA build healthy after 15 s */
    if(getenv("DISKOS_THEME_RELOAD")) unsetenv("DISKOS_THEME_RELOAD");   /* a theme re-launch: no boot splash */
    else splash_start();
    /* Init is complete. Force the FIRST FRAME to actually paint (lv_refr_now flushes
     * synchronously on this fb), then disarm the boot watchdog - a painted frame, not
     * merely "lv_timer_handler returned", is what proves we're alive. Log BEFORE
     * disarming (a blocked write must never leave us alive-but-disarmed), and mask
     * SIGALRM across alarm(0) so an already-pending timer can't fire after we disarm. */
    lv_refr_now(NULL);
    if(bl_state == 2) draw_set_dark(1);
    if(g_sd_hold){
        ui_toast("SD protection unavailable - library and artwork are off");
        lv_timer_create(sd_degraded_notice_cb, 300000, NULL);   /* keep saying so; a toast is missable */
    }
    { static const char okm[] = "=== diskos reached main loop (first frame painted; boot watchdog disarmed) ===\n";
      (void)write(2, okm, sizeof okm - 1);
      sigset_t as; sigemptyset(&as); sigaddset(&as, SIGALRM);
      sigprocmask(SIG_BLOCK, &as, NULL);
      alarm(0);
      signal(SIGALRM, SIG_IGN);
      sigprocmask(SIG_UNBLOCK, &as, NULL); }
    for(;;){
        if(ipc_take_reconnected()){       /* player restarted + /ui reattached: resync UI-owned state */
            /* Arm the post-restart settle window so BT routing (and the v2.40 local-init) wait out the
             * player's ~7s late-init before sending an output route that it could otherwise overwrite. */
            g_settle_armed = 1; g_settle_until = lv_tick_get() + 9000;
            /* A player restart stopped the book (the fresh player is idle) while our session is still set -
             * and a stale book-path in the reconnected state could keep book_tick treating it as playing,
             * which suppresses the local re-init (localplayer_workmode_cb bails under an active book) and wedges.
             * END the session WITHOUT flushing: the reconnected state can carry the restarted player's
             * shallow replay position (a valid path + fresh seq, but from the NEW instance), and saving that
             * would clobber a deep bookmark. So force g_book_ckpt_ok=0 before the cancel - BOOK_PROGRESS
             * keeps whatever was last durably saved (normal ~10s checkpoints, and a deliberate seek is saved
             * immediately by ui_book_user_seeked). The resume-aware adoption then restores from that saved
             * bookmark if the book is genuinely still playing. A brief embargo lets stale state settle. */
            if(book_session_active()){
                g_book_ckpt_ok = 0;
                ui_cancel_book_resume();
                g_book_noadopt_until = lv_tick_get() + 4000;
            }
            /* the fresh player defaults to local/analog output, so our "routed to X" cache is stale: clear it
             * so ui_reapply_audio's local re-init isn't suppressed, then recover the still-connected speaker
             * via the (settle-deferred) auto-route poll. */
            g_route_mac[0] = 0;
            route_state_remove();
            bt_notify_player_restart();
            ui_reapply_audio();           /* re-send managed audio config (DRE/filter/etc.); the v2.40 local-init
                                           * is NOT here - it's sent from the play path when the player is ready. */
        }
        if(g_settle_armed) (void)ui_player_settling();   /* service the settle expiry every loop so a dormant armed flag can never wrap (~24.9d) into a false "settling" */
        opus_title_tick();
        ipc_get_state(&st);
        wifi_supervise();   /* keep wpa_supplicant alive when Wi-Fi should be on (auto-reconnect) */
        /* NOTE: the MAIN LOOP never re-sends audio settings per-tick. Managed settings are (re)applied
         * only by ui_reapply_audio() on a CONFIRMED player reconnect (the ipc_take_reconnected() gate
         * above) and by the live apply_* handlers on a user change. Sending 0666 (output route) to an
         * IDLE, freshly-booted player re-inits its path and wedges it (g_fiio_local null) - which is
         * exactly why the v2.40 Local-init in ui_reapply_audio() rides on the reconnect (the player is
         * re-initialised by then), gated to Local + fw==240, rather than a blind boot send. At cold boot
         * the player recreates /ui after we start (main.c ~L590 note), so that reconnect reliably fires.
         * Volume persistence relies on the player's shutdown-save. */
        /* VOLUME MEMORY: handled NATIVELY by the player, which saves SYSCONFIG.VOLUME on real
         * shutdown and restores it on boot. diskOS does NOT restore/save volume - an earlier
         * attempt to infer it from a714 frames could corrupt cfg under a lossy/backlogged /ui
         * (no ack to correlate our restore's echo). The "boots at 0" seen in dev was our
         * `reboot`s skipping the player's shutdown-save, not a real power-off. VERIFY on device:
         * set a volume, power OFF with the button, power ON -> should return to that volume. */
        /* volume change (hardware buttons -> player -> a714 frame): show the bar + count activity,
         * but ONLY while the screen is already on. The player owns the physical keys, so a VOLUME
         * press - and any play-triggered or BT-autoroute volume RE-report (bt.c resends 0715 ->
         * a714 echo) - lands here as an a714 frame. Waking a dimmed/off panel from a player frame is
         * exactly the "random wake" bug: never call ui_backlight() from player IPC. Deliberate touch
         * (the raw-evdev path below) is the only thing that may wake the screen. Always consume the
         * seq so it can't re-fire. */
        if(st.volume_seq != last_vol_seq){
            last_vol_seq = st.volume_seq;
            power_note_input();   /* a volume key press counts as input for idle power-off (last_activity only moves when the panel is off) */
            if(bl_state == 0){
                last_activity = lv_tick_get();
                if(screen_current()==SCR_SAVER) screen_back();
                ui_show_volume(st.volume);
            }
        }
        /* the metadata 'state' field is unreliable (reports 0 while playing); infer play/pause from
         * whether position is advancing. One source of truth in playstate.c (host-tested); matches the
         * original heuristic except a backward/rewind jump no longer counts as advancing. max_adv_ms=0
         * keeps "any forward change counts"; the paused-forward-seek bound is device-tunable. */
        int playing = playstate_playing(&g_ps, st.have_track, st.position_ms, lv_tick_get(), 1600, 0);
        if(playing) ui_note_transport_sent();   /* playback observed this generation (any start, incl. hardware keys): its one-shot is moot */
        g_playing = playing;   /* publish for ui_route_bt/ui_route_analog (raw st.state is unreliable) */
        st.state = playing ? 2 : 1;

        /* USB DAC (mode 4) connection tracking:
         * Follows the stock firmware mechanism (uevent_handler.c / ui_ctrl_response.c):
         * - On disconnect: mq_player's on_usb_audio_remove automatically stops audio
         *   streaming (audio_track_stop), sets out_device to USB_HOST_NULL (7), closes ALSA
         *   mixer, and emits a60a TIP_INFO_EVENT 531 (0x0213).
         *   UI reflects playback paused, clears active playstate, and toasts "USB DAC disconnected".
         * - On reconnect: mq_player's on_usb_audio_add automatically re-binds out_device to
         *   USB_HOST (3), reopens ALSA mixer, and emits a60a TIP_INFO_EVENT 530 (0x0212).
         *   UI toasts "USB DAC connected" and keeps the current track paused at its position.
         *   A normal Play tap (0201 toggle) cleanly unpauses and resumes from where it stopped. */
        static uint32_t last_usb_dac_poll = 0;
        static int usb_dac_disconnected_latched = 0;
        static unsigned last_tip_seq = 0;

        if(ui_get_source_mode() == 4){
            unsigned tip_seq = 0;
            int tip_evt = ipc_tip_event(&tip_seq);
            int tip_updated = (tip_seq != last_tip_seq);
            if(tip_updated) last_tip_seq = tip_seq;

            int poll_due = (lv_tick_elaps(last_usb_dac_poll) >= 200);
            if(poll_due || tip_updated){
                last_usb_dac_poll = lv_tick_get();
                int connected = ui_usb_dac_connected();
                if(tip_updated && tip_evt == 0x0213) connected = 0;
                else if(tip_updated && tip_evt == 0x0212) connected = 1;

                if(!connected){
                    if(!usb_dac_disconnected_latched){
                        usb_dac_disconnected_latched = 1;
                        if(playing){
                            playstate_reset(&g_ps);
                            playing = 0;
                            g_playing = 0;
                            st.state = 1;
                        }
                        ui_toast("USB DAC disconnected");
                    }
                } else {
                    if(usb_dac_disconnected_latched){
                        usb_dac_disconnected_latched = 0;
                        ui_toast("USB DAC connected");
                    }
                }
            }
        } else {
            usb_dac_disconnected_latched = 0;
        }
        /* when the backlight is off (deep idle) nothing is visible - skip the whole
         * UI refresh; it catches up on wake (last/ last_playing stay stale). */
        /* playback-start timeout: retire only after a quiet, bounded wait with no stream. */
        if(play_start_failed(&st)){
            if(g_play_pending && lv_tick_elaps(g_play_pending) > 6000){
                g_play_pending = 0;
                ui_toast("Couldn't start playback");
                /* No automatic 0666/0657: 0666 releases a live local output. */
            }
        }
        ui_np_title_scroll(bl_state != 2);
        if(bl_state != 2 && (g_draw_dark || st.seq != last || playing != last_playing || ui_take_art_force())){
            last = st.seq; last_playing = playing;
            ui_update(&st);
            home_set_now_playing(st.have_track?ipc_display_title(&st):NULL,
                                 st.have_track?st.artist:NULL,
                                 ui_current_accent(), playing);
            { void home_set_progress(int, int); int ui_np_progress(void);
              home_set_progress(ui_np_progress(), st.have_track); }
            { void saver_set_playing(int); saver_set_playing(playing); }
            /* The art + backdrop surfaces only change on a TRACK change (or when a decode completes -
             * re-pushed below), NOT on every position tick. Re-setting the 360px backdrop image each
             * second re-invalidated the whole screen and defeated partial rendering. Track identity is
             * "" when nothing is playing (so stopping clears the art). */
            const char *idnow = st.have_track ? st.path : "";
            const char *tnow = st.have_track ? ipc_display_title(&st) : "", *anow = st.have_track ? st.artist : "";
            int art_changed = strcmp(idnow, np_last_path) != 0;
            int song_changed = strcmp(tnow, np_last_title) != 0 || strcmp(anow, np_last_artist) != 0;
            if(art_changed){
                snprintf(np_last_path, sizeof np_last_path, "%s", idnow);
                home_set_art_src(ui_current_thumb_src());
                home_set_backdrop(ui_current_backdrop_src());
            }
            if(art_changed || song_changed){
                snprintf(np_last_title, sizeof np_last_title, "%s", tnow);
                snprintf(np_last_artist, sizeof np_last_artist, "%s", anow);
                saver_set_track(st.have_track?ipc_display_title(&st):NULL,
                                st.have_track?st.artist:NULL,
                                ui_current_backdrop_src(), st.have_track?st.path:NULL);
            }
            quicksettings_refresh(playing);
            songinfo_set(&st);
            npmenu_set(st.have_track ? &st : NULL, playing, ui_current_thumb_src());
        }
        if(bl_state != 2 && g_draw_dark){   /* waking: art that landed while dark, checked against the track set above */
            ui_art_poll(NULL);
            lv_timer_pause(g_t_art);
            clock_tick(NULL);
        }
        /* album art decodes off-thread now, so it usually lands AFTER the seq-gated
         * push above - re-push the art-dependent surfaces when a decode completes.
         * Test bl_state FIRST so the one-shot flag is NOT consumed while the screen is
         * off (bl_state==2): it stays set and is applied on wake, else art goes stale. */
        if(bl_state != 2 && ui_take_art_applied()){
            home_set_art_src(ui_current_thumb_src());
            home_set_backdrop(ui_current_backdrop_src());
            saver_set_track(st.have_track?ipc_display_title(&st):NULL,
                            st.have_track?st.artist:NULL,
                            ui_current_backdrop_src(), st.have_track?st.path:NULL);
            npmenu_set(st.have_track ? &st : NULL, playing, ui_current_thumb_src());
        }
        /* spin the vinyl only while it's the visible, playing, lit Now Playing */
        ui_vinyl_spin(playing && screen_current()==SCR_NOWPLAYING && bl_state==0);
        /* spin while the saver is visible, the screen is on, AND audio is actually
         * playing - a real platter stops when paused. `playing` (position advancing)
         * is the reliable signal; it freezes the vinyl ~1.6s after pause/stop. The saver
         * dims to bl_state==1 the moment it appears, so gate on !=2 (not ==0) for the
         * lit test; a FULLY-off screen (bl_state==2) stops the spin anyway. */
        saver_vinyl_spin(playing && screen_current()==SCR_SAVER && bl_state != 2);

        /* surface silent failures (sticky flags persist until the screen is on, so a
         * failure during screen-off is shown on wake rather than lost). */
        if(bl_state != 2){
            /* one toast per tick - toast.c shows only one at a time, so an `if/if`
             * would let the 2nd clobber the 1st. else-if leaves the other flag set
             * to surface on the next tick (both are rare; the 1-tick stagger is unseen). */
            if(cfg_take_save_error())       ui_toast("Couldn't save settings");
            else if(ipc_take_send_error())  ui_toast("Player didn't respond");
        }
#ifdef DISKOS_PROFILE
        struct timespec pa, pb; clock_gettime(CLOCK_MONOTONIC, &pa);
#endif
        mclock_sync(bl_state != 2);
        kit_cursor_sync(bl_state != 2);
        albumwall_poll_sync(bl_state != 2);
        uint32_t wait = lv_timer_handler();
        if(bl_state != 2 && g_draw_dark){   /* waking: repaint everything, then light it (draw_set_dark) */
            lv_timer_resume(g_t_art);
            draw_set_dark(0);
        }
#ifdef DISKOS_PROFILE
        clock_gettime(CLOCK_MONOTONIC, &pb);
        unsigned long long prof_h = (unsigned long long)(pb.tv_sec - pa.tv_sec) * 1000000000ULL + (unsigned long long)(pb.tv_nsec - pa.tv_nsec);
#endif
        /* (boot watchdog was already disarmed before the loop, after the first frame
         * actually painted - see lv_refr_now above.) */

        if(g_app_pending){          /* run a launched app between frames */
            g_app_pending = 0;
            screen_back();          /* leave the Apps screen first */
            app_run(g_app_exec);
            last = ~0u;             /* force a state refresh after returning */
            last_activity = lv_tick_get();   /* using an app counts as activity */
        }

        if(g_touch){
            lv_indev_state_t ts = lv_indev_get_state(g_touch);
            lv_point_t p; lv_indev_get_point(g_touch, &p);
            if(kbinput_active()){
                /* the modal keyboard owns all touch - don't run nav gestures */
            } else if(ts==LV_INDEV_STATE_PRESSED && prev_ts==LV_INDEV_STATE_RELEASED){
                /* press edge: start of a potential swipe */
                last_activity = lv_tick_get();
                if(screen_current()==SCR_SAVER){
                    /* any touch wakes the saver; restore backlight, swallow gesture */
                    if(bl_state && !pk.player_off){ wake_backlight(ui_effective_brightness()); bl_state = 0; g_screen_off = 0; }
                    screen_back();
                    woke = 1;
                } else {
                    sx=p.x; sy=p.y; lastx=p.x; lasty=p.y; sms=lv_tick_get();
                    g_press_sx = g_press_lx = p.x; g_press_sy = g_press_ly = p.y;
                    s_press_scr=screen_current();   /* a gesture belongs to the screen it STARTED on */
                    cover_tap = 0;
                    fsart_touch = ui_np_fsart_active();   /* latch: overlay owns this whole gesture */
                    sleep_touch = ui_np_overlay_active(); /* latch: the sleep popover owns this whole gesture */
                    if(fsart_touch || sleep_touch){
                        /* a modal overlay is up -> LVGL handles it; arm no ring seek / rim scroll here */
                    } else if(screen_current()==SCR_NOWPLAYING){
                        /* A press within the album-cover square (centre 180,116; ~148px) is a
                         * candidate to open full-screen art - DON'T arm a seek there, so the tap
                         * can't be eaten by the ring (LVGL's cover click opens it). Presses
                         * elsewhere on NP drive the seek. */
                        if(ui_np_cover_hit(sx, sy)) cover_tap = 1;   /* wherever the theme put the cover */
                        else ui_np_seek_press(sx, sy);
                    }
                    else {
                        rim_press(sx, sy);    /* arm rim-scroll candidate (long lists AND the cover flow) */
                        if(rim_state==RIM_IDLE && screen_current()==SCR_ALBUMWALL && sx >= BACK_START_MAX_X)
                            albumwall_drag_begin(sx);   /* CENTRE press -> linear drag. Rim-band press is rim-only, and */
                    }                                   /* the left edge is reserved for the back-swipe (not armed as a drag) */
                    fprintf(stderr,"TAP x=%d y=%d\n",sx,sy); fflush(stderr);
                    if(g_dbg && g_dbgdot){
                        lv_obj_set_pos(g_dbgdot, sx-13, sy-13);
                        lv_obj_clear_flag(g_dbgdot, LV_OBJ_FLAG_HIDDEN);
                    }
                }
            } else if(ts==LV_INDEV_STATE_PRESSED){
                last_activity = lv_tick_get();
                lastx=p.x; lasty=p.y; g_press_lx=p.x; g_press_ly=p.y;
                if(screen_current()==SCR_NOWPLAYING && !ui_np_fsart_active() && !sleep_touch) ui_np_seek_move(p.x, p.y);
                else if(rim_move(p.x, p.y)){ /* rim drag owns the gesture (list or cover flow) */ }
                else if(screen_current()==SCR_ALBUMWALL && rim_state==RIM_IDLE) albumwall_drag(p.x);  /* linear drag only when no rim gesture is armed */
            } else if(ts==LV_INDEV_STATE_RELEASED && prev_ts==LV_INDEV_STATE_PRESSED){
                /* The evdev driver can fuse the final movement into the release frame, so the last PRESSED
                 * sample (lastx/lasty) may lag the true lift point. Fold the release coordinates in (bounds-
                 * guarded against a garbage frame) BEFORE classifying, so a fast flick can't read as a small
                 * tap and open/play an album (cover flow) or misfire a seek/swipe. */
                if(p.x >= 0 && p.x < 360 && p.y >= 0 && p.y < 360){ lastx = p.x; lasty = p.y; g_press_lx = p.x; g_press_ly = p.y; }
                if(woke){
                    woke = 0;   /* swallow the release that woke the saver */
                } else if(rim_release()){
                    /* rim-scroll owned this gesture -> no nav/tap classification */
                } else {
                    /* release edge: classify the swipe */
                    int dx=lastx-sx, dy=lasty-sy;
                    int adx=dx<0?-dx:dx, ady=dy<0?-dy:dy;
                    uint32_t dt=lv_tick_elaps(sms);
                    int cur=screen_current();
                    /* fsart is owned entirely by LVGL (cover click opens, overlay click closes).
                     * Swallow (never open/close/seek/nav) when the overlay is up / the press began
                     * on it (fsart_touch), or when this is a genuine TAP that began on the cover
                     * (cover_tap + small travel) - LVGL's cover click opens that one. A SWIPE that
                     * merely started on the cover is NOT LVGL's (its cover click ignores slides), so
                     * it must still fall through to nav (back/hub). Order-independent: only LVGL
                     * mutates fsart state, so there's no open/close race. */
                    int cover_tap_gesture = cover_tap && adx < 20 && ady < 20;
                    int lvgl_owned = fsart_touch || cover_tap_gesture || sleep_touch;
                    /* a committed seek on Now Playing consumes the gesture. Replay the (fused) final
                     * position through the move handler first so the ring commits the release point, not
                     * the last PRESSED sample. */
                    int seek_consumed = 0;
                    if(!lvgl_owned && cur==SCR_NOWPLAYING && !ui_np_fsart_active()){
                        ui_np_seek_move(lastx, lasty);
                        seek_consumed = ui_np_seek_release(lastx, lasty);
                    }
                    int vert = ady > adx*2, horiz = adx > ady*2;
                    if(lvgl_owned || ui_np_fsart_active()){
                        /* swallow - LVGL handled (or will handle) the open/close */
                    } else if(!seek_consumed){
                    if(cur==SCR_QUICK && vert && dy<0 && ady>=g_swipe_thresh && dt<700){
                        screen_back();                       /* swipe up closes quick settings */
                    } else if(cur!=SCR_QUICK && cur!=SCR_SAVER && sy<40 &&
                              vert && dy>0 && dy>=g_swipe_thresh && dt<700){
                        screen_show(SCR_QUICK);              /* pull down from top edge */
                        quicksettings_refresh(playing);
                    } else if(cur==SCR_NOWPLAYING){
                        /* On Now Playing a nav swipe must be a LONG, STRAIGHT,
                         * horizontal slide (so it's not confused with a seek drag
                         * along the ring): right = hub, left = back. */
                        if(adx >= NP_NAV_DIST && adx > ady*NP_NAV_STRAIGHT && dt<900){
                            if(dx < 0) screen_show(SCR_NPHUB);
                            else       screen_back();
                        }
                    } else if(cur==SCR_HOME && horiz && dx<0 && adx>=g_swipe_thresh && dt<700){
                        apps_reload();
                        screen_show(SCR_APPS);    /* slide in the Apps panel */
                    } else if(cur==SCR_ALBUMWALL && s_press_scr==SCR_ALBUMWALL){
                        /* gated on s_press_scr so the very tap that OPENED the cover flow (a click on the
                         * Library "Albums" row) is never re-read here as a cover tap that plays. */
                        if(sx < BACK_START_MAX_X && horiz && dx>0 && adx>=g_swipe_thresh && dt<700){
                            albumwall_drag_cancel();            /* left-edge right-swipe = back (drag wasn't armed here) */
                            screen_back();
                        } else if(adx<18 && ady<18 && sx>=100 && sx<=260 && sy>=76 && sy<=214 && dt<900){
                            albumwall_drag_cancel();            /* a still touch on the centre cover = tap */
                            if(dt >= 500) albumwall_play();     /* long-press the cover = play the whole album */
                            else          albumwall_open();     /* tap the cover = open the album's track list */
                        } else {
                            albumwall_drag(lastx);              /* apply the (fused) final finger position first */
                            albumwall_drag_end();               /* then finger drag -> inertial fling + snap */
                        }
                    } else if(horiz && dx>0 && adx>=g_swipe_thresh && dt<700 &&
                              sx < BACK_START_MAX_X){
                        fprintf(stderr,"BACK swipe sx=%d dx=%d dy=%d dt=%u\n",sx,dx,dy,dt); fflush(stderr);
                        /* inside Library, step through its sub-views first */
                        if(!(cur==SCR_LIBRARY && library_back())) screen_back();
                    }
                    }
                }
            }
            prev_ts = ts;
        }

        /* 0201 is a play/pause TOGGLE, so we must only ever send it while genuinely playing (sending it
         * to a paused player would START playback). Skip briefly after a seek (the play-state read can be
         * transiently wrong), disarm without toggling if already paused, and keep the timer armed if the
         * pause command fails to send. */
        int sleep_guarded = lv_tick_get() < g_sleep_seek_guard;
        /* sleep timer: pause when it elapses */
        switch(power_sleep_decide(g_sleep_ms, g_sleep_ms ? lv_tick_elaps(g_sleep_start) : 0, sleep_guarded, playing,
                                  cfg_get_int("sleep_action", POWER_SLEEP_PAUSE))){
        case PW_SLEEP_SHUTDOWN:        /* stock "sleep" powers off: close the card, sync, then the player's own power-off */
            g_sleep_ms = 0; cfg_set_int("sleep_idx", 0);
            power_shutdown_request();
            break;
        case PW_SLEEP_DISARM: g_sleep_ms = 0; cfg_set_int("sleep_idx", 0); break;                 /* already stopped -> just disarm */
        case PW_SLEEP_PAUSE:
            if(ipc_send_cmd("0201000C0000") == 0){ g_sleep_ms = 0; cfg_set_int("sleep_idx", 0); }  /* paused OK -> disarm */
            /* else: send failed -> keep armed, retry next loop */
            break;
        case PW_SLEEP_WAIT: break;
        }
        /* end-of-chapter sleep: pause once THIS book reaches the target position; disarm if the track
         * changed (the target is a position in a specific book, meaningless against another track). */
        if(g_sleep_eoc >= 0){
            track_state_t sst; ipc_get_state(&sst);
            if(strcmp(sst.path, g_sleep_eoc_path) != 0){
                /* the book rolled over to another track. If the last position we saw on it was near the
                 * target, the chapter/book finished and playback advanced before we caught the exact
                 * target (a last chapter, or a chapterless book, ends at the file end which may never be
                 * reported as a position) -> fulfil the sleep by pausing the track it rolled into. A path
                 * change is an unambiguous completion signal, so it overrides the post-seek guard. If the
                 * last position was NOT near the target, the user navigated away -> just disarm. */
                int completed = (g_sleep_eoc_atend && g_sleep_eoc_lastpos >= 0 && g_sleep_eoc_lastpos >= g_sleep_eoc - 8000);
                if(completed && playing && ipc_send_cmd("0201000C0000") != 0){
                    /* pause send failed -> keep armed, retry next loop (path still differs) */
                } else {
                    ui_disarm_book_eoc();
                }
            } else {
                g_sleep_eoc_lastpos = sst.position_ms;
                if(sst.position_ms >= g_sleep_eoc && !sleep_guarded){
                    if(!playing){ ui_disarm_book_eoc(); }                          /* already stopped -> disarm */
                    else if(ipc_send_cmd("0201000C0000") == 0){ ui_disarm_book_eoc(); }  /* paused OK -> disarm */
                    /* else: send failed -> keep armed, retry */
                }
            }
        }

        /* PW-10: power-button presses, seen only as the player's brightness writes - see powerkey_step(). */
        if(fw_power_key_driveable()){
            int on; unsigned sq, pq, gen; ipc_screen_snapshot(&on, &sq, &pq, &gen);
            if(gen != screen_gen_seen){
                screen_gen_seen = gen; scr_seq_seen = aa03_seq_seen = 0;
                poweroff_inhibit = inject_pending = 0; pk.player_off = 0;
            }
            if(pq != aa03_seq_seen){ aa03_seq_seen = pq; poweroff_inhibit = 1; }
            if(on >= 0 && sq != scr_seq_seen){
                scr_seq_seen = sq;
                inject_pending = 0;                       /* a newer frame cancels a press not yet sent */
                power_note_input();                       /* a press is input: restarts the idle power-off countdown */
                switch(screen_frame_step(on, bl_state, &pk.player_off, poweroff_inhibit)){
                case SF_INJECT: inject_pending = 1; inject_for = sq; inject_gen = gen; inject_due = lv_tick_get() + 300; break;
                case SF_OFF:      /* the player slept its panel: power the rail down to match */
                    ui_backlight(0); bl_state = 2; g_screen_off = 1;
                    mclock_sync(0);
                    kit_cursor_sync(0);
                    if(screen_current() == SCR_SAVER) screen_back();
                    break;
                case SF_WAKE:     /* the player woke its panel: light it at the user's level */
                    wake_backlight(ui_effective_brightness());
                    bl_state = 0; g_screen_off = 0; last_activity = lv_tick_get();
                    if(screen_current() == SCR_SAVER) screen_back();
                    break;
                default: break;
                }
            }
            if(inject_pending && (int32_t)(lv_tick_get() - inject_due) >= 0){   /* the player's sleep has finished */
                inject_pending = 0;
                int on2; unsigned sq2, pq2, gen2; ipc_screen_snapshot(&on2, &sq2, &pq2, &gen2);
                if(gen2 == inject_gen && sq2 == inject_for && on2 == 0 && pq2 == aa03_seq_seen && !poweroff_inhibit) (void)power_key_inject();
            }
            /* the player writes its own level ~50 ms after its wake frame: keep the user's (lit or dimmed) */
            if(lv_tick_elaps(last_blpoll) >= 250){
                last_blpoll = lv_tick_get();
                int cbr = read_int_file("/sys/class/backlight/backlight/brightness");
                (void)ui_backlight_take_foreign(NULL, NULL);   /* the frames carry the presses here */
                if(cbr > 0 && cbr != ui_backlight_written() && !pk.player_off){
                    if(bl_state == 0) wake_backlight(ui_effective_brightness());
                    else if(bl_state == 1) wake_backlight(dim_level());
                }
            }
        } else if(lv_tick_elaps(last_blpoll) >= 250){
            last_blpoll = lv_tick_get();
            /* a press our own write overwrote since the last poll first (a tap or the dim timer in the same 250 ms),
             * then the live value */
            for(int pass = 0; pass < 2; pass++){
                int was_dark = bl_state == 2; unsigned at_seq = ui_backlight_write_seq();
                int cbr = pass == 0 ? ui_backlight_take_foreign(&was_dark, &at_seq) : read_int_file("/sys/class/backlight/backlight/brightness");
                if(cbr < 0) continue;
                if(pk.seen < 0){ pk.seen = cbr; pk.player_off = (cbr == 0); pk.off_seq = ui_backlight_write_seq(); continue; }
                int act = powerkey_step(&pk, cbr, pass == 0 ? -1 : ui_backlight_written(), at_seq, was_dark);
                if(act != PK_NONE) power_note_input();   /* a press is input: restarts the idle power-off countdown */
                switch(act){
                case PK_OFF:      /* the player put the panel to sleep: power the rail down to match */
                    ui_backlight(0); bl_state = 2; g_screen_off = 1;
                    mclock_sync(0);
                    kit_cursor_sync(0);
                    if(screen_current() == SCR_SAVER) screen_back();
                    break;
                case PK_WAKE:     /* the player woke the panel: light it at the user's level (Outdoor: full), not the player's */
                    wake_backlight(ui_effective_brightness());
                    bl_state = 0; g_screen_off = 0; last_activity = lv_tick_get();
                    if(screen_current() == SCR_SAVER) screen_back();
                    break;
                case PK_RELEVEL:  /* the player rewrote a level on a showing screen: keep diskOS's (lit or dimmed) */
                    if(bl_state == 0) wake_backlight(ui_effective_brightness());
                    else if(bl_state == 1) wake_backlight(dim_level());
                    break;
                default: break;
                }
            }
        }

        /* screensaver + backlight power saving (the screen is the biggest drain):
         * idle > saver_timeout       -> show saver, dim backlight
         * idle > saver+screenoff      -> backlight off entirely */
        int si = cfg_get_int("saver_idx", 2), oi = cfg_get_int("screenoff_idx", 3);
        int saver_timeout   = (si>=0 && si<5) ? TMAP[si] : 60;
        int screenoff_extra = (oi>=0 && oi<5) ? TMAP[oi] : 120;
        /* Never sleep while the on-screen keyboard is open: it owns all touch, so a
         * dimmed/off saver couldn't be woken mid-typing (e.g. entering a Wi-Fi password). */
        /* Screen-off/dim wake on RAW touch: drain the raw evdev fd; ANY event means a finger touched,
         * so we wake even when LVGL coalesced the whole tap into a no-press-edge RELEASED (the reason
         * a dark panel needed many taps to wake). Drained every loop (even awake) so it never backs
         * up; only acts when the panel is dimmed/off (bl_state != 0). */
        if(g_touch_raw >= 0){
            char rb[512]; int any = 0;
            while(read(g_touch_raw, rb, sizeof rb) > 0) any = 1;
            if(any && bl_state && !pk.player_off){   /* the player's sleep (panel off): only its button wakes */
                last_activity = lv_tick_get();
                wake_backlight(ui_effective_brightness()); bl_state = 0; g_screen_off = 0;
                if(screen_current() == SCR_SAVER) screen_back();
            }
        }
        if(kbinput_active()) last_activity = lv_tick_get();
        power_tick(last_activity, playing);   /* AFTER all input is consumed (a pending touch must win the race): SD-safe shutdown, diskOS idle power-off, temperature notice */
        int in_saver = (screen_current()==SCR_SAVER);
        /* a manual Sleep request ends the moment the panel is woken (we leave SCR_SAVER) */
        if(g_manual_sleep && !in_saver) g_manual_sleep = 0;
        int manual = g_manual_sleep;
        /* Art-based savers (cover=0, vinyl=4) are a NOW-PLAYING display - only show them while
         * something is playing; with nothing playing they're pointless (and were showing when
         * they shouldn't). Time/info savers (analog=1, minim=2, weather=3) stay useful idle.
         * A suppressed art saver still power-saves the backlight below. */
        int sstyle    = cfg_get_int("saver_style", 0);
        int art_saver = (sstyle == 0 || sstyle == 4);
        int saver_ok  = (!art_saver || playing);
        if(saver_timeout > 0 || manual){
            /* manual sleep runs its dim/off countdown from the tile tap, independent of the
             * (possibly disabled) auto-saver timer */
            uint32_t idle = manual ? lv_tick_elaps(g_manual_sleep_at) : lv_tick_elaps(last_activity);
            int past_dim = (saver_timeout > 0 && idle > (uint32_t)saver_timeout*1000);
            /* auto-enter only in timeout mode AND only when the saver is appropriate now (art
             * savers need playback). The Sleep tile already opened SCR_SAVER (manual).
             * Gate on bl_state==0: enter ONLY from the fully-lit state. Once dimmed (1) or off (2)
             * we never (re-)enter - after full-off we deliberately LEAVE the saver (below), and
             * re-entering each loop would churn screen_show and revive saver animation behind a
             * dark panel. */
            if(past_dim && !in_saver && bl_state==0 && (saver_ok || manual)){
                screen_show(SCR_SAVER); in_saver = 1;
            }
            /* dim once idle past the timeout (manual: immediately) - whether or not a saver
             * screen is up, so a suppressed art saver still sleeps the screen normally. */
            if((in_saver || past_dim || manual) && bl_state==0){
                /* the vinyl art-showcase saver stays at the user's brightness; everything else
                 * (incl. a suppressed art saver just dimming the current screen) crushes to a
                 * low dim. bl_state still -> 1 so the screen-off timer powers the panel down. */
                ui_backlight(dim_level()); bl_state = 1;
            }
            /* full off after the screen-off delay. Manual sleep uses the configured Screen Off
             * delay when set, else a 10s default so the tile actually powers the panel down. */
            uint32_t off_at = manual ? (uint32_t)(screenoff_extra > 0 ? screenoff_extra : 10)*1000
                                     : (uint32_t)(saver_timeout+screenoff_extra)*1000;
            if(bl_state==1 && (manual || screenoff_extra > 0) && idle > off_at){
                ui_backlight(0); bl_state = 2; g_screen_off = 1;
                mclock_sync(0);
                kit_cursor_sync(0);
                /* screen fully off -> STOP the screensaver. Leaving SCR_SAVER: (a) satisfies "the
                 * saver stops when the screen turns off"; (b) pops back to the screen shown BEFORE
                 * the saver, so a later wake lands there, not on the saver; (c) makes saver_anim_cb
                 * a no-op (it self-gates on screen_current()==SCR_SAVER) so the analog seconds hand
                 * stops. SAVER transitions are instant (no slide) - one cheap repaint behind a dark
                 * panel. Guard on ==SCR_SAVER: a merely-dimmed non-saver screen has nothing to pop. */
                if(screen_current()==SCR_SAVER) screen_back();
            }
        }

        /* adaptive sleep: poll fast (5ms) while animating or finger-down for
         * responsiveness, otherwise let the CPU idle longer (30ms) to save power.
         * lv_timer_handler already returns a large 'wait' when nothing is pending. */
        /* screen fully off (pocket playback): pause the frequent polls + idle longer
         * so the loop wakes ~5x/s (touch poll) instead of ~33x/s. Derived from
         * bl_state each iteration so every wake path is covered. */
        mclock_sync(bl_state != 2);
        kit_cursor_sync(bl_state != 2);
        polls_set_paused(bl_state == 2);
        if(bl_state == 2) draw_set_dark(1);
        albumwall_poll_sync(bl_state != 2);
        g_screen_off = (bl_state == 2);   /* publish for the slow polls (skip the hcitool spawn while off) */
        advance_probe_tick(bl_state != 2);
        g_bl_idle = (bl_state >= 1);   /* prewarm worker reads this: only work while dimmed/off */
        int busy = lv_anim_count_running() > 0 || prev_ts == LV_INDEV_STATE_PRESSED;
        if(bl_state == 2){
            /* deep idle (panel off): sleep ~5x/s REGARDLESS of any running animation. Nothing is
             * visible, so a stray infinite anim (e.g. a Wi-Fi/BT scan glyph left spinning, or the
             * saver's own motion) must NOT drop us to the 5ms 'busy' cap and spin the CPU at 200Hz -
             * that was the real reason "screen off" wasn't saving power. Wake stays reliable because
             * the raw evdev fd (drained every loop, above) holds a real finger-press in its kernel
             * queue across the 200ms sleep; we no longer rely on catching an LVGL press-edge. */
            wait = 200;
        } else {
            uint32_t cap = busy ? 5 : 30;
            if(wait > cap) wait = cap;
        }
#ifdef DISKOS_PROFILE
        { struct timespec pc; clock_gettime(CLOCK_MONOTONIC, &pc);
          unsigned long long body = (unsigned long long)(pc.tv_sec - pa.tv_sec) * 1000000000ULL + (unsigned long long)(pc.tv_nsec - pa.tv_nsec) - prof_h;
          prof_loop(busy, bl_state, lv_anim_count_running(), prof_h, body); }
#endif
        usleep(wait*1000);
    }
    return 0;
}
