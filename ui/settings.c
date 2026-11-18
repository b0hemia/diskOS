/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "config.h"
#include "anim.h"
#include "musicdb.h"
#include "fwcaps.h"
#include "device_names_ui.h"
#include "version.h"
#include "md5.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include "sdio.h"
#include "art.h"
#include "scanner.h"
#include "power.h"
#include "system.h"
#include "ota.h"
#include "controls.h"
#include "i18n.h"
#include "ipc.h"
#include "custom_saver_db.h"
#include "custom_picker_ui.h"
#include "custom_saver_overlay.h"
#include <pthread.h>
#include <stdatomic.h>

/* Killed children are always collected, so none is left a zombie: kill_and_reap() waits up to 300 ms (a SIGKILLed
 * child normally exits at once), and anything still unreaped (e.g. stuck in kernel I/O) is parked and swept by a
 * small self-deleting timer until it has been collected - also after the restart attempt that killed it has ended. */
#define RST_REAP_MAX 8
static pid_t g_rst_reap[RST_REAP_MAX];
static lv_timer_t *g_rst_reaper;
static int rst_reap_sweep(void){                     /* returns how many are still parked */
    int left = 0;
    for(int i = 0; i < RST_REAP_MAX; i++)
        if(g_rst_reap[i] > 0){
            pid_t w = waitpid(g_rst_reap[i], NULL, WNOHANG);
            if(w == g_rst_reap[i] || (w < 0 && errno == ECHILD)) g_rst_reap[i] = 0; else left++;
        }
    return left;
}
static void rst_reaper_cb(lv_timer_t *t){ if(rst_reap_sweep() == 0){ lv_timer_delete(t); g_rst_reaper = NULL; } }
/* Free parking slots after a sweep. Every caller that may later kill children checks it has enough BEFORE forking,
 * so kill_and_reap() can always park - a killed child is never forgotten unreaped. */
static int rst_reap_free(void){ rst_reap_sweep(); int n = 0; for(int i = 0; i < RST_REAP_MAX; i++) if(g_rst_reap[i] <= 0) n++; return n; }
static void kill_and_reap(pid_t p){
    if(p <= 0) return;
    kill(-p, SIGKILL); kill(p, SIGKILL);
    for(int k = 0; k < 30; k++){
        pid_t w = waitpid(p, NULL, WNOHANG);
        if(w == p || (w < 0 && errno == ECHILD)) return;
        usleep(10000);
    }
    for(int i = 0; i < RST_REAP_MAX; i++) if(g_rst_reap[i] <= 0){ g_rst_reap[i] = p; break; }
    if(!g_rst_reaper) g_rst_reaper = lv_timer_create(rst_reaper_cb, 1000, NULL);
}

/* Run a shell command as a bounded child in its OWN process group, and NEVER blocking-reap it: a child
 * stuck in kernel I/O must not be able to hold the UI thread - or a restart - even for a moment. */
static void bounded_detached(const char *cmd, int ms){
    pid_t p = fork();
    if(p == 0){ setsid(); execl("/bin/sh", "sh", "-c", cmd, (char*)NULL); _exit(127); }
    if(p <= 0) return;
    for(int i = 0; i < ms/100; i++){
        if(waitpid(p, NULL, WNOHANG) == p) return;
        usleep(100000);
    }
    kill_and_reap(p);
}

/* ---- setting model ------------------------------------------------------ */
typedef enum { ST_TOGGLE, ST_SLIDER, ST_CYCLER, ST_READONLY, ST_ACTION, ST_CHOICE } st_type_t;   /* CHOICE: pick from a list */

typedef struct setting_s {
    const char *group;                     /* the category page it is listed on (GROUPS) */
    const char *section;                   /* the heading above it on that page, NULL = none */
    const char *label;
    st_type_t   type;
    const char *cfg_key;
    int         min, max, step;            /* slider */
    const char *const *opts; int nopts;    /* cycler */
    const char *ro_val;                    /* readonly */
    void (*apply)(int v);                  /* optional apply hook / decode seam */
    int  def;
    const char *desc;                      /* one short line, only where the name alone is unclear; else NULL */
} setting_t;

/* apply hooks (decode seams live in main.c) */
void screen_set_anim(int on);
void ui_set_workmode(int mode);
int ui_apply_eq(int preset);
void ui_clock_refresh(void);
void ui_set_accent_config(int mode, int rgb);   /* accent: 0=dynamic / 1=static(rgb) */
void ui_set_prewarm_mode(int m);                /* art cache: 0=covers only 1=+sweep idle 2=+sweep idle&charging */
void ui_set_dre(int on);                        /* audio cluster (main.c) */
void ui_set_gain(int high);
int  ui_set_artist_class(int album_artist);      /* 0648; 0 sent, -1 not (main.c) */
#include "netart.h"
int  ui_set_folder_jump(int on);                 /* 0687; 0 sent, -1 not (main.c) */
void ui_set_track_display(int on);               /* 064d where the player has it (main.c) */
void ui_set_output(int spdif);
void ui_set_dac_filter(int idx);
void ui_set_gapless(int on);
void ui_set_memory(int mode);
void ui_set_maxvol(int v);
void ui_set_balance(int v);

static void apply_open_colorpick(int v){ (void)v; colorpick_open(); }   /* seed sliders from cfg + open */
static void apply_debug_mode(int v){ (void)v; debug_open(); }           /* Settings -> Device -> Debug Mode */
static void apply_artcache(int v){ ui_set_prewarm_mode(v); }   /* every mode preloads album covers; the mode picks the per-track sweep */
void ui_sd_quiesce_begin(void); int ui_sd_quiesce_drained(void); void ui_sd_quiesce_end(void);   /* main.c: Restart card drain */

static void apply_swipe(int v){ ui_apply_swipe_thresh(v); }   /* live; persisted on slider release */
static void apply_anim(int v){ screen_set_anim(v); }
static void apply_weather(int v){ weather_set_enabled(v); }
static void apply_mode(int v){ if(ui_book_active()) return; ui_set_workmode(v); }   /* an active book stays in Single; cfg still updates for later music */
static void apply_eq(int v){ ui_eq_select(v); }   /* central: also records eq_last so the drawer A/B stays in sync */
/* audio cluster: v<0 = "System default" (unmanaged) -> ui_set_* no-ops, leaving the player's
 * own state untouched. A real value sends the live command; persistence is via cfg + the
 * player-ready re-apply in main.c (ui_reapply_audio). */
static void apply_dre(int v){ ui_set_dre(v); }
static void apply_replay_gain(int v){ ui_set_replay_gain(v); }
static void apply_gain(int v){ ui_set_gain(v); }
static void apply_dac_filter(int v){ ui_set_dac_filter(v); }
static void apply_gapless(int v){ ui_set_gapless(v); }
#ifdef DISKOS_TEST_OUTPUTS
static void apply_spdif(int v){ ui_set_output(v); }   /* SPDIF: stock toggle form: pause -> silent -> 0666 -> 0657 8 -> resume (modes.c); the toggle then mirrors the real route */
#endif
static void apply_memory(int v){ ui_set_memory(v); }
static void apply_maxvol(int v){ ui_set_maxvol(v); }
static void apply_balance(int v){ ui_set_balance(v); }
static void apply_time(int v){ (void)v; ui_clock_refresh(); }
static void apply_sleep(int idx){
    static const int M[] = {0,15,30,45,60,90};
    ui_set_sleep_timer((idx>=0 && idx<6) ? M[idx] : 0);
}
static void apply_np_style(int v){ ui_set_np_style(v); }
/* Theme settings apply by re-launching the UI (palette + Outdoor backlight are read at startup). set_val has already
 * saved the new value; if the reload is refused (a library scan is running) the old value goes back, and the detail
 * view is rebuilt after this event finishes (it cannot be deleted from inside its own switch/button callback). */
static int g_set_prev;                                  /* the value set_val replaced */
static void detail_refresh_async(void *u){
    (void)u;
    if(screen_current() == SCR_SETLIST) settings_row_values_refresh();
    else setting_detail_refresh();
}
static void theme_reload_or_revert(const char *key){
    const char *destination = !strcmp(key, "outdoor") ? "settings:outdoor" : "settings";
    if(ui_theme_reload_setting(destination,key,g_set_prev) == 0) return;        /* not reached on success: the exec replaced us */
    cfg_set_int(key, g_set_prev);
    lv_async_call(detail_refresh_async, NULL);
}
static void apply_appearance(int v){ (void)v; theme_reload_or_revert("theme_variant"); }
static void apply_outdoor(int v){ (void)v; theme_reload_or_revert("outdoor"); }
static void apply_theme_preset(int v){ (void)v; theme_reload_or_revert("theme_preset"); }
/* Font Size and Language change every screen's text: like a theme change, the UI re-launches and re-reads them */
static void apply_font_size(int v){ (void)v; theme_reload_or_revert("font_size"); }
static void apply_language(int v){ (void)v; theme_reload_or_revert("language"); }

static void apply_old_tags(int v){
    (void)v;
    scanner_set_old_tags(cfg_get_int("old_tags", OLD_TAGS_AUTO));
}
static void old_tags_rescan_cb(lv_event_t *e){
    if(lv_event_get_code(e) == LV_EVENT_CLICKED && !scanner_active()) ui_rescan_library();
}
/* The button owns this timer; cleaning or deleting the page deletes it too. */
static void old_tags_rescan_tick(lv_timer_t *t){
    lv_obj_t *bt = lv_timer_get_user_data(t);
    if(scanner_active()) lv_obj_add_state(bt, LV_STATE_DISABLED);
    else lv_obj_remove_state(bt, LV_STATE_DISABLED);
}
static void old_tags_rescan_del_cb(lv_event_t *e){
    lv_timer_delete(lv_event_get_user_data(e));
}
static void old_tags_rescan_watch(lv_obj_t *bt){
    lv_timer_t *t = lv_timer_create(old_tags_rescan_tick, 400, bt);
    old_tags_rescan_tick(t);
    lv_obj_add_event_cb(bt, old_tags_rescan_del_cb, LV_EVENT_DELETE, t);
}
static void apply_rescan(int v){ (void)v;
    ui_rescan_library();
}

/* ---- Date & Time (parity with stock: automatic time switch, set by hand, time zone) ---------------------------- */
static void apply_bt_codec(int v){
    ui_bt_codec_set(v);  /* the normal IPC failure toast handles a rejected send */
}
/* Drop the old diskOS-only preference without touching stock or playback.
 * Idempotent: after a durable removal there is nothing to migrate again. */
static int legacy_bt_codec_key(const char *key){ return !strcmp(key, "bt_codec"); }
static void settings_bt_codec_migrate(void){ cfg_remove_if(legacy_bt_codec_key); }
static void apply_auto_time(int v){
    if(mdb_sysconfig_set_auto_time(v) != 0){           /* the player may hold its database: put the switch back */
        cfg_set_int("auto_time", g_set_prev);
        ui_toast("Couldn't change it - try again");
        lv_async_call(detail_refresh_async, NULL);
        return;
    }
    ui_toast("Applies after the next restart");        /* the player reads it only when it starts (see musicdb.c) */
}
/* Artists grouping (stock "Artist / Album Artist", ARTIST_CLASS_TYPE via 0648): the player owns the value (mirrored
 * at startup); a send that fails puts the choice back so the menu never shows a grouping the player isn't using. */
static void apply_artist_class(int v){
    if(ui_set_artist_class(v) != 0){
        cfg_set_int("artist_class", g_set_prev);
        ui_toast("Couldn't change it - try again");
        lv_async_call(detail_refresh_async, NULL);
    }
}
/* Play Through Folders (stock FOLDER_JUMP via 0687): the player owns the value (mirrored at startup); a failed send
 * puts the switch back. */
static void apply_folder_jump(int v){
    if(ui_set_folder_jump(v) != 0){
        cfg_set_int("folder_jump", g_set_prev);
        ui_toast("Couldn't change it - try again");
        lv_async_call(detail_refresh_async, NULL);
    }
}
/* Charging optimization (stock 0808, SYSCONFIG.CHARGE_PROTECT): the player owns it (mirrored at startup, power.c); a
 * failed send puts the choice back. Idle Power-off has NO apply hook on purpose: diskOS runs that timer itself (power.c)
 * and keeps the player's own idle power-off (0664) off, because the player's path powers off without syncing the card. */
/* Screen Rotation and the volume keys' actions (stock 0646 / 0820-0822, controls.c): the player stores them; a refused
 * send puts the choice back so the menu never shows what the player isn't using. */
static void apply_screen_rot(int v){
    if(ui_set_screen_rot(v) != 0){
        cfg_set_int(CTL_CFG_ROT, g_set_prev);
        ui_toast("Couldn't change it - try again");
        lv_async_call(detail_refresh_async, NULL);
    } else if(ctl_rotation_pending())
        cfg_set_int(CTL_CFG_ROT, g_set_prev);   /* stored only when the user taps Keep (controls.c); the row keeps the old one till then */
}
static void key_act_apply(int key, const char *cfg_key, int v){
    if(v != CTL_ACT_VOLUME && ui_cast_session_active()){   /* 16b: a track action would start local audio behind the receiver */
        cfg_set_int(cfg_key, g_set_prev);
        ui_toast("Stop beaming to change this");
        lv_async_call(detail_refresh_async, NULL);
        return;
    }
    if(ui_set_volume_key(key, v) != 0){
        cfg_set_int(cfg_key, g_set_prev);
        ui_toast("Couldn't change it - try again");
        lv_async_call(detail_refresh_async, NULL);
    }
}
static void apply_key_single(int v){ key_act_apply(CTL_KEY_SINGLE, "key_single", v); }
static void apply_key_double(int v){ key_act_apply(CTL_KEY_DOUBLE, "key_double", v); }
static void apply_key_long(int v){   key_act_apply(CTL_KEY_LONG, "key_long", v); }
static void apply_charge_protect(int v){
    if(power_apply_charge(v) != 0){
        cfg_set_int("charge_protect", g_set_prev);
        ui_toast("Couldn't change it - try again");
        lv_async_call(detail_refresh_async, NULL);
    }
}
/* Track Numbers: album song lists show each song's track number (stock "TRACK_DISPLAY", drawn by diskOS) */
static void apply_track_numbers(int v){ ui_set_track_display(v); library_refresh(); }
/* Online Album Art (stock "Online cover", done by diskOS - netart.c): on tries the current song now; off drops any
 * queued lookup and redraws the current cover without an online picture */
static void apply_online_art(int v){ if(!v) netart_cancel(); ui_art_redo(); }
static void apply_open_datetime(int v){ (void)v; screen_show(SCR_DATETIME); }
/* Time Zone: "Automatic" leaves the zone to the player (looked up online when Wi-Fi connects, with Automatic Time
 * on); any other entry copies that zone's rules to /usr/data/localtime (what /etc/localtime points at, for every
 * process), atomically, then re-launches the UI so its clock uses them. */
static const char *const TZ_LABEL[] = { "Automatic", "UTC", "London", "Paris / Berlin", "Athens / Helsinki",
    "Moscow", "Cairo", "Johannesburg", "Dubai", "Karachi", "India", "Bangkok / Jakarta", "China / Singapore",
    "Hong Kong", "Tokyo", "Seoul", "Perth", "Adelaide", "Darwin", "Brisbane", "Sydney / Melbourne", "Hobart",
    "Auckland", "Honolulu", "Anchorage", "Los Angeles", "Denver", "Phoenix", "Chicago", "Mexico City", "New York",
    "Toronto", "Sao Paulo", "Buenos Aires" };
static const char *const TZ_ZONE[] = { NULL, "UTC", "Europe/London", "Europe/Berlin", "Europe/Athens",
    "Europe/Moscow", "Africa/Cairo", "Africa/Johannesburg", "Asia/Dubai", "Asia/Karachi", "Asia/Kolkata",
    "Asia/Bangkok", "Asia/Shanghai", "Asia/Hong_Kong", "Asia/Tokyo", "Asia/Seoul", "Australia/Perth",
    "Australia/Adelaide", "Australia/Darwin", "Australia/Brisbane", "Australia/Sydney", "Australia/Hobart",
    "Pacific/Auckland", "Pacific/Honolulu", "America/Anchorage", "America/Los_Angeles", "America/Denver",
    "America/Phoenix", "America/Chicago", "America/Mexico_City", "America/New_York", "America/Toronto",
    "America/Sao_Paulo", "America/Argentina/Buenos_Aires" };
#define TZ_COUNT ((int)(sizeof TZ_LABEL / sizeof TZ_LABEL[0]))
_Static_assert(sizeof TZ_LABEL / sizeof TZ_LABEL[0] == sizeof TZ_ZONE / sizeof TZ_ZONE[0], "time zone tables");
/* write n bytes over /usr/data/localtime: temp + fsync + rename + directory fsync */
static int tz_write(const unsigned char *buf, size_t n){
    const char *tmp = "/usr/data/localtime.diskos-tmp", *dst = "/usr/data/localtime";
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    if(fd < 0) return -1;
    int ok = write(fd, buf, n) == (ssize_t)n && fsync(fd) == 0;
    if(close(fd) != 0) ok = 0;
    if(!ok || rename(tmp, dst) != 0){ unlink(tmp); return -1; }
    int dfd = open("/usr/data", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if(dfd >= 0){ fsync(dfd); close(dfd); }
    return 0;
}
static size_t tz_read(const char *path, unsigned char *buf, size_t cap){   /* 0 = unreadable, too big, or not TZif */
    FILE *in = fopen(path, "rb");
    if(!in) return 0;
    size_t n = fread(buf, 1, cap, in);
    int more = fgetc(in) != EOF;
    fclose(in);
    return (more || n < 44 || memcmp(buf, "TZif", 4)) ? 0 : n;
}
/* Install a zone. This firmware keeps the RTC in LOCAL time and every UI start reloads the clock from it (hwclock -s),
 * so the RTC is rewritten in the NEW zone's local time straight away (hwclock -w, bounded); if that fails the old
 * zone goes back - a relaunch would otherwise read the old local time as the new zone's and shift the clock by the
 * zones' difference (device-verified 2026-09-26: +10 h going Sydney -> UTC without it). */
static int tz_install(const char *zone){
    static unsigned char nbuf[65536], obuf[65536];
    char src[160]; snprintf(src, sizeof src, "/usr/share/zoneinfo/%s", zone);
    size_t n = tz_read(src, nbuf, sizeof nbuf);
    if(!n) return -1;
    size_t on = tz_read("/usr/data/localtime", obuf, sizeof obuf);
    if(tz_write(nbuf, n) != 0) return -1;
    char *a[] = { "hwclock", "-w", NULL };
    if(ui_run_bounded(a, 4000) != 0){
        if(on) tz_write(obuf, on);                     /* back to the zone the RTC still matches */
        return -1;
    }
    return 0;
}
static void apply_timezone(int v){
    if(v > 0 && v < TZ_COUNT && tz_install(TZ_ZONE[v]) != 0){
        cfg_set_int("tz_idx", g_set_prev);
        ui_toast("Couldn't set the time zone");
        lv_async_call(detail_refresh_async, NULL);
        return;
    }
    if(ui_theme_reload("settings") != 0){              /* the installed zone and RTC already match */
        ui_toast("Time zone saved - restart to update clock");
        lv_async_call(detail_refresh_async, NULL);
    }
}
static void apply_import_m3u(int v){ (void)v;
    int n = mdb_import_m3u_sd("/tmp/sdcard");   /* every folder on the card (bounded walk), like stock V2.57 */
    char b[48];
    if(n < 0) snprintf(b, sizeof b, "Couldn't import - try again");
    else if(n == 0) snprintf(b, sizeof b, "No new playlists found");
    else       snprintf(b, sizeof b, "Imported %d playlist%s", n, n==1 ? "" : "s");
    ui_toast(b);
}
static void apply_wifi(int v){ (void)v; wifi_open(); }   /* opens SCR_WIFI */
static void apply_bluetooth_name(int v){ (void)v; dnui_open(1); }
static void apply_bt(int v){ (void)v; bt_open(); }       /* opens SCR_BT */
/* Network radio rows (owner 2026-10-22): the Wi-Fi and Bluetooth rows carry the radio's power switch. The row only
 * calls the page's own power path (wifi.c / bt.c), so saved intent, reconciliation, boot restore and pending states
 * stay in one place. The row shows what the page's switch shows. */
typedef struct {
    void (*open)(int);      /* the row's apply hook: opens the page */
    int  (*shown)(void);    /* the page switch's state source */
    int  (*pending)(void);  /* a start from the power path is still settling */
    void (*power)(int on);  /* the page switch's power path */
} radio_row_t;
static const radio_row_t RADIO_ROWS[] = {
    { apply_wifi, wifi_control_on, wifi_power_pending, wifi_power_set },
    { apply_bt,   bt_control_on,   bt_power_pending,   bt_power_set },
};
static const radio_row_t *radio_row(const void *apply){
    for(unsigned i = 0; i < sizeof RADIO_ROWS / sizeof RADIO_ROWS[0]; i++)
        if(apply == (const void *)RADIO_ROWS[i].open) return &RADIO_ROWS[i];
    return NULL;
}
/* Pending shows on (the choice made) and dimmed; taps are ignored until it settles. */
static void radio_switch_paint(const radio_row_t *r, lv_obj_t *sw){
    int pending = r->pending();
    if(pending || r->shown()) lv_obj_add_state(sw, LV_STATE_CHECKED); else lv_obj_remove_state(sw, LV_STATE_CHECKED);
    if(pending) lv_obj_add_state(sw, LV_STATE_DISABLED); else lv_obj_remove_state(sw, LV_STATE_DISABLED);
}
static void apply_workmode(int v){ (void)v; modes_open(); }  /* opens SCR_WORKMODE (source-mode picker) */
static void apply_eq_custom(int v){ (void)v; screen_show(SCR_EQ); }  /* opens Custom EQ */
static void apply_qsconfig(int v){ (void)v; screen_show(SCR_QSCONFIG); }  /* opens Quick Settings tile picker */

/* ---- Default-UI preference + Restart ------------------------------------- *
 * Boot model: the user picks a persistent default UI (diskOS or Stock) here;
 * holding Vol-Up at power-on boots the OTHER one for that boot. The boot hook
 * (fiio_init.sh) reads a flag file: /usr/data/boot_default_stock present => the
 * default is Stock; absent => the default is diskOS. We mirror the cycler value
 * to that flag so the choice persists across reboots. */
/* This is the user's route to the STOCK firmware, so it must not be able to hang. It used to shell out
 * to `touch|rm && sync`: a full filesystem sync can block indefinitely on a stuck device, and it ran
 * after the boot watchdog was already disarmed. Done natively now - create/remove the flag and fsync only
 * the directory holding it, which is what actually makes the choice durable - with no shell and no
 * global sync. Failure is reported rather than silently diverging from the shown setting. */
static void apply_boot_default(int v){
    const char *flag = "/usr/data/boot_default_stock";
    /* The whole persistence operation runs in a BOUNDED child, pathname operations included. fsync is
     * the obvious blocker, but open() and unlink() on a stalled device block too, and this is the screen
     * the user needs in order to select stock - it must never be the thing that freezes. The child's exit
     * status tells us whether the choice was actually recorded: EVERY step that makes it durable (file
     * create/fsync/close or unlink, then directory open/fsync/close) must succeed, or it exits nonzero. */

    /* A child that times out is killed and collected by the shared reaper (kill_and_reap), never waited on. Make
     * sure it can be parked BEFORE forking: with every slot held by a stuck child, refuse rather than start another. */
    if(rst_reap_free() < 1){ ui_toast("Couldn't change boot UI"); return; }

    int ok = 0;
    pid_t p = fork();
    if(p == 0){
        setsid();                                  /* best effort: the parent also kills the pid directly */
        /* Drop the UI's identity BEFORE any storage I/O. A child forked from the UI still carries its argv
         * ("mq_ui"), which is what fiio_init's watchdog looks for, so a child stuck in storage I/O would look
         * like a live UI and could hide a dead one. The kernel reports /proc/<pid>/cmdline from
         * [arg_start, arg_end) - ordinary writable memory of this process - so overwrite it. /proc is read
         * with stdio, not with the storage calls below. */
        {
            int renamed = 0;                           /* identity removal is MANDATORY (see above) */
            FILE *sf = fopen("/proc/self/stat", "r");
            if(sf){
                char buf[1024];
                size_t n = fread(buf, 1, sizeof buf - 1, sf);
                fclose(sf);
                buf[n] = 0;
                char *q = strrchr(buf, ')');       /* comm may contain spaces; fields resume after ')' */
                unsigned long long field[64] = {0};
                if(q){
                    int i = 3;                     /* the first field after ')' is field 3 (state) */
                    char *save = NULL;
                    for(char *t = strtok_r(q + 2, " ", &save); t && i < 60; t = strtok_r(NULL, " ", &save), i++)
                        field[i] = strtoull(t, NULL, 10);
                }
                unsigned long long start = field[48], end = field[49];   /* arg_start, arg_end (Linux >= 3.5) */
                if(start && end > start && end - start <= 65536){
                    static const char name[] = "diskos-pref";
                    size_t len = (size_t)(end - start);
                    memset((void *)(uintptr_t)start, 0, len);
                    memcpy((void *)(uintptr_t)start, name, len < sizeof name ? len - 1 : sizeof name - 1);
                    renamed = 1;
                }
            }
            /* If the identity could not be dropped, touch no storage at all: a child that might block while
             * still looking like "mq_ui" is exactly what this protects against. Report failure instead. */
            if(!renamed) _exit(2);
        }
        int r = 0;
        if(v == 1){
            int fd = open(flag, O_WRONLY|O_CREAT|O_CLOEXEC, 0644);
            if(fd < 0) r = -1;
            else {
                if(fsync(fd) != 0) r = -1;
                if(close(fd) != 0) r = -1;
            }
        } else {
            if(unlink(flag) != 0 && errno != ENOENT) r = -1;
        }
        if(r == 0){
            int dfd = open("/usr/data", O_RDONLY|O_DIRECTORY|O_CLOEXEC);
            if(dfd < 0) r = -1;
            else {
                if(fsync(dfd) != 0) r = -1;
                if(close(dfd) != 0) r = -1;
            }
        }
        _exit(r == 0 ? 0 : 1);
    }
    if(p > 0){
        int st = 0;
        for(int i = 0; i < 30; i++){                    /* ~3s */
            if(waitpid(p, &st, WNOHANG) == p){ ok = WIFEXITED(st) && WEXITSTATUS(st) == 0; p = -1; break; }
            usleep(100000);
        }
        if(p > 0) kill_and_reap(p);   /* timed out: kill it (group + pid) and have it collected, never blocking */
    }
    if(!ok) ui_toast("Couldn't change boot UI");
}

static lv_obj_t *g_boot_modal;
static void boot_modal_close(void){ if(g_boot_modal){ lv_obj_delete_async(g_boot_modal); g_boot_modal = NULL; } }
static lv_timer_t *g_rst_timer;
static void boot_cancel_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED && !g_rst_timer) boot_modal_close(); }   /* no cancel once a restart is under way */
/* Restart: only reboots once every admitted SD writer has finished AND a full `sync` has SUCCEEDED; otherwise it
 * refuses, reopens card access and says so. (v1.1.3 withheld it: it gave the flush 2 s, then rebooted regardless.)
 * Runs as a timer state machine, so the UI never blocks while it waits. */
#define RESTART_WRITERS_MS 10000   /* admitted writers must drain within this */
#define RESTART_SYNC_MS    15000   /* and the flush must complete within this */
static int         g_rst_phase;    /* 0 = waiting for writers, 1 = waiting for sync */
static uint32_t    g_rst_t0;
static pid_t       g_rst_sync, g_rst_clock;
static pid_t spawn_detached(const char *cmd){
    pid_t p = fork();
    if(p == 0){ setsid(); execl("/bin/sh", "sh", "-c", cmd, (char*)NULL); _exit(127); }
    return p;
}
static void stop_child(pid_t *p){ kill_and_reap(*p); *p = 0; }
static void restart_fail(const char *why){
    stop_child(&g_rst_sync); stop_child(&g_rst_clock);
    if(g_rst_timer){ lv_timer_delete(g_rst_timer); g_rst_timer = NULL; }
    ui_sd_quiesce_end();           /* reopen card access (readers + writers) if the card is still local */
    boot_modal_close();
    ui_toast(why);
}
static void restart_tick(lv_timer_t *t){
    (void)t;
    rst_reap_sweep();
    if(g_rst_phase == 0){
        if(ui_sd_quiesce_drained()){                           /* every admitted reader AND writer has finished */
            g_rst_sync = spawn_detached("sync");
            if(g_rst_sync <= 0){ g_rst_sync = 0; restart_fail("Couldn't restart - try again"); return; }
            g_rst_phase = 1; g_rst_t0 = lv_tick_get();
        } else if(lv_tick_elaps(g_rst_t0) >= RESTART_WRITERS_MS){
            restart_fail("Couldn't restart - card busy, try again");
        }
        return;
    }
    int st = 0;
    pid_t w = waitpid(g_rst_sync, &st, WNOHANG);
    if(w == g_rst_sync){
        g_rst_sync = 0;
        if(!WIFEXITED(st) || WEXITSTATUS(st) != 0){ restart_fail("Couldn't restart - card flush failed"); return; }
        stop_child(&g_rst_clock);   /* the RTC save had the whole wait; a still-running one is stuck */
        reboot(RB_AUTOBOOT);        /* direct syscall: no shell, no PATH, no binary on disk */
        bounded_detached("reboot", 5000);   /* only reached if the syscall was refused */
        restart_fail("Couldn't restart - try again");
        return;
    }
    if(lv_tick_elaps(g_rst_t0) >= RESTART_SYNC_MS) restart_fail("Couldn't restart - card busy, try again");
}
static void boot_confirm_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    if(g_rst_timer) return;                         /* already restarting */
    if(rst_reap_free() < 3){ ui_toast("Couldn't restart - try again"); return; }   /* sync + clock + reboot fallback */
    /* Stop our own SD work: closing admission alone is not enough - a scan holds one lease for its whole walk and
     * the prewarm decoder is non-cancellable, so both are stopped explicitly. Then wait for the rest to drain. */
    ui_sd_quiesce_begin();          /* closes sd_io leases AND sd_write_begin writers; storage_tick won't reopen them */
    scanner_abort();
    art_kill_all();
    /* Save the (NTP-corrected) clock to the RTC alongside - the periodic save runs only every 5 min and the stock
     * player saves the RTC only on ITS power-off path. Best effort, its own process group, never waited on. */
    g_rst_clock = spawn_detached("hwclock -w 2>/dev/null");
    if(g_rst_clock < 0) g_rst_clock = 0;
    g_rst_phase = 0; g_rst_t0 = lv_tick_get();
    g_rst_timer = lv_timer_create(restart_tick, 100, NULL);
    if(!g_rst_timer){ restart_fail("Couldn't restart - try again"); return; }
    ui_toast("Restarting...");
}
static void boot_modal_pill(lv_obj_t *card, int x, const char *txt, theme_color_role_t col, lv_event_cb_t cb){
    lv_obj_t *b = lv_button_create(card);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 108, 42); lv_obj_align(b, LV_ALIGN_BOTTOM_MID, x, -16);
    lv_obj_set_ext_click_area(b, 4);   /* 42px pill -> ~50px touch target */
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, TC(SURFACE_RAISED), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    ui_on(b, cb, LV_EVENT_CLICKED, NULL, "settings.cb", UI_CORE);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, TF(UI_16), 0);
    lv_obj_set_style_text_color(l, theme_color(col), 0);
    lv_obj_center(l);
}
static const char *g_restart_note;   /* one-shot subtitle for the next restart modal (settings_restart_prompt) */
static void apply_restart(int v){ (void)v;
    boot_modal_close();
    g_boot_modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_boot_modal);
    lv_obj_set_size(g_boot_modal, 360, 360); lv_obj_center(g_boot_modal);
    lv_obj_set_style_bg_color(g_boot_modal, TC(SCRIM), 0);
    lv_obj_set_style_bg_opa(g_boot_modal, LV_OPA_70, 0);
    lv_obj_clear_flag(g_boot_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_boot_modal, LV_OBJ_FLAG_CLICKABLE);                       /* absorb taps */
    ui_on(g_boot_modal, boot_cancel_cb, LV_EVENT_CLICKED, NULL, "settings.boot_cancel", UI_CORE);  /* tap outside = cancel */
    lv_obj_t *card = lv_obj_create(g_boot_modal);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 280, 184); lv_obj_center(card);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_bg_color(card, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, "Restart now?");
    lv_obj_set_style_text_font(t, TF(UI_16), 0);
    lv_obj_set_style_text_color(t, TC(TEXT_PRIMARY), 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_t *s = lv_label_create(card);
    lv_label_set_text(s, g_restart_note ? g_restart_note : "Boots your default UI. Hold Vol-Up at power-on for the other one.");
    g_restart_note = NULL;
    lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s, 236);
    lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s, TF(UI_14), 0);
    lv_obj_set_style_text_color(s, TC(TEXT_MUTED), 0);
    lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 50);
    boot_modal_pill(card, -58, "Cancel",  THEME_CLR_TEXT_SECONDARY, boot_cancel_cb);
    boot_modal_pill(card,  58, "Restart", THEME_CLR_ACCENT_EMPHASIS, boot_confirm_cb);
}
/* write the backlight sysfs only (no config change) - used for transient dimming.
 * v==0 fully powers the panel backlight DOWN: writing brightness 0 alone leaves the
 * LED driver enabled at its minimum (screen looks black but the backlight glows), so
 * we also toggle bl_power (0 = FB_BLANK_UNBLANK on, 4 = FB_BLANK_POWERDOWN off). */
static int g_bl_written = -1;   /* the brightness diskOS itself last wrote (-1 = none yet) */
static unsigned g_bl_write_seq; /* bumps on every brightness diskOS writes (equal values stay distinguishable) */
unsigned ui_backlight_write_seq(void){ return g_bl_write_seq; }
static int g_bl_foreign = -1;   /* a player value (a power press) our write is about to overwrite, for the poll */
static int g_bl_foreign_dark;   /* the panel was off (rail down) when that press landed */
static unsigned g_bl_foreign_seq; /* diskOS's write count when it landed (before the write that overwrote it) */
int ui_backlight_written(void){ return g_bl_written; }
int ui_backlight_take_foreign(int *dark, unsigned *seq){
    int v = g_bl_foreign; g_bl_foreign = -1;
    if(dark) *dark = g_bl_foreign_dark;
    if(seq) *seq = g_bl_foreign_seq;
    return v;
}
static void backlight_write(int v){
    if(v > 0){                     /* set target level first, then power on (no flash) */
        FILE *r = fopen("/sys/class/backlight/backlight/brightness", "r");
        if(r){ int cur = -1;
               if(fscanf(r, "%d", &cur) == 1 && cur >= 0 && cur != g_bl_written && cur != v){
                   FILE *pw = fopen("/sys/class/backlight/backlight/bl_power", "r"); int bp = 0;
                   if(pw){ if(fscanf(pw, "%d", &bp) != 1) bp = 0; fclose(pw); }
                   g_bl_foreign = cur; g_bl_foreign_dark = (bp != 0); g_bl_foreign_seq = g_bl_write_seq;
               }
               fclose(r); }
        FILE *f = fopen("/sys/class/backlight/backlight/brightness", "w");
        if(f){ fprintf(f, "%d", v); fclose(f); g_bl_written = v; g_bl_write_seq++; }
        else  fprintf(stderr, "backlight brightness open failed: %s\n", strerror(errno));
    }
    FILE *p = fopen("/sys/class/backlight/backlight/bl_power", "w");
    if(p){ fprintf(p, "%d", v ? 0 : 4); fclose(p); }
    else  fprintf(stderr, "backlight bl_power open failed: %s\n", strerror(errno));
}
/* Every brightness write passes here. While main.c holds drawing (screen off, until the wake's full repaint) a
 * positive level only replaces the deferred one - a callback, a Settings row, a wake or a player-level correction
 * alike - and the release lights the latest. Zero still goes out at once (screen off is immediate). */
static int g_bl_hold, g_bl_held;   /* UI thread */
void ui_backlight(int v){
    if(v < 0) v = 0;
    if(v > 40) v = 40;
    if(g_bl_hold){ g_bl_held = v; if(v > 0) return; }
    backlight_write(v);
}
void ui_backlight_hold(void){ g_bl_hold = 1; g_bl_held = 0; }
void ui_backlight_release(int fallback){   /* after the repaint: the latest level, else fallback */
    if(!g_bl_hold) return;
    g_bl_hold = 0;
    ui_backlight(g_bl_held > 0 ? g_bl_held : fallback);
}
static void apply_brightness(int v){ if(v < 1) v = 1; ui_backlight(v); }
void settings_apply_startup(int restore_brightness){
    scanner_set_old_tags(cfg_get_int("old_tags", OLD_TAGS_AUTO));
    if(restore_brightness) apply_brightness(ui_effective_brightness());
    settings_bt_codec_migrate();
    { int at = mdb_sysconfig_auto_time();              /* mirror the player's switch (it owns the value) */
      if(at >= 0 && at != cfg_get_int("auto_time", 1)) cfg_set_int("auto_time", at); }
    { int ac = mdb_artist_class_type();                 /* the player's Artist / Album Artist grouping (it owns it) */
      if(ac >= 0 && ac != cfg_get_int("artist_class", 0)) cfg_set_int("artist_class", ac);
      mdb_set_artist_mode(ac == 1); }                   /* unknown -> by artist, as the player's queue can't be matched */
    { int fj = mdb_sysconfig_folder_jump();             /* the player's Play Through Folders switch (it owns it) */
      if(fj >= 0 && fj != cfg_get_int("folder_jump", 0)) cfg_set_int("folder_jump", fj); }
    power_startup_mirror();        /* the player's own Idle poweroff / Charging optimization values */
    controls_startup_mirror();     /* the player's Screen rotation and volume-key actions */
    cfg_set_int("sleep_idx", 0);   /* never auto-arm a sleep timer across reboots */
    apply_boot_default(cfg_get_int("boot_default", 0));  /* keep the boot-hook flag in sync */
    ui_set_accent_config(cfg_get_int("accent_mode", 0), cfg_get_int("accent_color", 0xF23260));
    ui_set_prewarm_mode(cfg_get_int("artcache", 0));     /* default "Covers only". Mode only: the boot album walk
                                                          * starts later, behind the SD-features gate (main.c) */
}

/* shared brightness API (used by Quick Settings too) - persists the value */
void ui_set_brightness(int v){
    if(v < 1) v = 1;
    if(v > 40) v = 40;
    apply_brightness(v);
    cfg_set_int("brightness", v);
}
int ui_get_brightness(void){
    int v = cfg_get_int("brightness", 16);
    return v < 1 ? 16 : v;   /* never 0/negative: wake paths pass this to ui_backlight,
                              * and 0 would power the panel DOWN while logically awake */
}
/* the level the screen actually runs at: full in Outdoor mode, else the user's. The user's own level stays saved
 * and comes back when Outdoor mode is turned off. */
int ui_effective_brightness(void){ return theme_outdoor() ? 40 : ui_get_brightness(); }
static void apply_brightness_row(int v){ if(!theme_outdoor()) apply_brightness(v); }   /* Outdoor holds full */

static const char *const OPT_MODE[] = { "Sequential", "Shuffle", "Repeat One", "Repeat All", "Single" };
/* 0..10 = built-in presets; 11..20 = the ten user PEQ slots (USER1..USER10), edited in Custom EQ.
 * Index here maps 1:1 to STYLE_PRESET, so the chosen row is exactly what ui_apply_eq() selects. */
static const char *const OPT_EQ[]   = { "Off","Jazz","Rock","R&B","Hip-Hop","Pop","Dance","Classical","Retro","Sibilance 1","Sibilance 2",
                                        "USER1","USER2","USER3","USER4","USER5","USER6","USER7","USER8","USER9","USER10" };
static const char *const OPT_SLEEP[] = { "Off","15 min","30 min","45 min","60 min","90 min" };
static const char *const OPT_SLEEP_ACT[] = { "Pause","Shut down" };
static const char *const OPT_IDLE_OFF[POWER_IDLE_N] = { "Off","5 min","10 min","30 min","60 min","90 min","120 min" };
static const char *const OPT_POWER[] = { "Off","30 sec","1 min","2 min","5 min" };  /* idx->TMAP secs in main.c */
static const char *const OPT_NPSTYLE[] = { "Cover", "Vinyl" };
static const char *const OPT_APPEARANCE[] = { "Dark", "Light" };
static const char *const OPT_FONTSIZE[] = { "Small", "Medium", "Large" };
static const char *const OPT_ALBUMVIEW[] = { "List", "Cover Flow" };
static const char *const OPT_SAVERSTYLE[] = { "Cover", "Analog", "Minimal", "Digital", "Vinyl", "Custom" };
static const char *const OPT_OLDTAGS[] = { "Auto", "Western", "Cyrillic" };
static const char *const OPT_BOOTDEF[] = { "diskOS", "Stock" };
static const char *const OPT_ARTCACHE[] = { "Covers only","When Idle","Idle & Charging" };

/* Public EQ preset name lookup (Quick Settings drawer toast, etc.); mirrors OPT_EQ indexing 0..20. */
const char *ui_eq_name(int i){ return (i >= 0 && i < 21) ? OPT_EQ[i] : "Off"; }
/* audio cluster cyclers - all use min=-1 so the value can be "System default" (unmanaged) */
static const char *const OPT_DRE[]    = { "Off", "On" };
static const char *const OPT_REPLAYGAIN[] = { "Off", "Track", "Album" };
static const char *const OPT_GAIN[]   = { "Low", "High" };
static const char *const OPT_DFILTER[]= { "Fast LL","Slow LL","Slow PC","Fast PC","NOS","Wideband" };
static const char *const OPT_MEMORY[] = { "Off", "Position", "Song" };
static const char *const OPT_ARTISTCLASS[] = { "By Artist", "By Album Artist" };
static const char *const OPT_ROTATION[] = { "Normal", "Turn 90", "Turn 180", "Turn 270" };   /* stock values 0..3 */
static const char *const OPT_VOLKEY[] = { "Switch Track", "Adjust Volume" };

static const setting_t TABLE[] = {
    /* Display order: a category page lists its rows in this order, and a heading appears wherever "section" changes
     * (NULL = no heading). The rows of one section are contiguous (tests/host/test_settings_reorg.py). */
    /* ---- Playback ---- */
    { "Playback", "Playing music", "Play Mode",   ST_CYCLER, "work_mode", 0,0,0, OPT_MODE, 5, NULL, apply_mode, 0 },
    { "Playback", "Playing music", "Resume Playback", ST_CYCLER, "memory_play", 0,0,0, OPT_MEMORY, 3, NULL, apply_memory, 0 },
    { "Playback", "Playing music", "Play Through Folders", ST_TOGGLE, "folder_jump", 0,1,1, NULL, 0, NULL, apply_folder_jump, 0 },
    { "Playback", "Bedtime", "Sleep Timer", ST_CYCLER, "sleep_idx", 0,0,0, OPT_SLEEP, 6, NULL, apply_sleep, 0 },
    { "Playback", "Bedtime", "When Sleep Ends", ST_CYCLER, "sleep_action", 0,0,0, OPT_SLEEP_ACT, 2, NULL, NULL, POWER_SLEEP_PAUSE },
    /* ---- Audio ---- */
    { "Audio", "Equalizer", "Equalizer",   ST_CHOICE, "eq_preset", 0,0,0, OPT_EQ,  21, NULL, apply_eq,   0 },
    { "Audio", "Equalizer", "Custom EQ",    ST_ACTION, NULL, 0,0,0, NULL,0, LV_SYMBOL_RIGHT, apply_eq_custom, 0 },
    { "Audio", "Sound", "Gain",        ST_TOGGLE, "audio_gain",   0,0,0, OPT_GAIN, 2, NULL, apply_gain, 0,
      "For demanding headphones" },
    { "Audio", "Sound", "DAC Filter",  ST_CYCLER, "audio_filter", 0,0,0, OPT_DFILTER, 6, NULL, apply_dac_filter, 1,
      "Digital filter shape" },
    { "Audio", "Sound", "ReplayGain",  ST_CYCLER, "replay_gain",  0,0,0, OPT_REPLAYGAIN, 3, NULL, apply_replay_gain, 0,
      "Evens out track volume" },
    { "Audio", "Sound", "DRE",         ST_TOGGLE, "audio_dre",    0,0,0, OPT_DRE, 2, NULL, apply_dre, 1,
      "Dynamic Range Enhancement" },
    { "Audio", "Sound", "Gapless",     ST_TOGGLE, "gapless",      0,0,0, OPT_DRE, 2, NULL, apply_gapless, 0 },
#ifdef DISKOS_TEST_OUTPUTS   /* device-unverified: test builds only */
    { "Audio", "Sound", "SPDIF Output", ST_TOGGLE, "spdif",   0,1,1, NULL,0, NULL, apply_spdif, 0 },
#endif
    { "Audio", "Output limits", "Max Volume",  ST_SLIDER, "max_vol",      10,120,5, NULL,0, NULL, apply_maxvol, 120 },
    { "Audio", "Output limits", "Balance",     ST_SLIDER, "balance",      -10,10,1, NULL,0, NULL, apply_balance, 0 },
    /* ---- Network ---- */
    { "Network", NULL, "Wi-Fi",       ST_ACTION, NULL, 0,0,0, NULL,0, NULL, apply_wifi, 0 },   /* radio row: inline power switch */
    { "Network", NULL, "Bluetooth", ST_ACTION, NULL, 0,0,0, NULL,0, NULL, apply_bt, 0 },          /* radio row: inline power switch */
    { "Network", NULL, "Bluetooth Codec", ST_CYCLER, "bt_codec", 0,0,0, BT_CODEC_LABEL, BT_CODEC_N, NULL, apply_bt_codec, 0 },
    { "Network", NULL, "Device name", ST_ACTION, NULL, 0,0,0, NULL,0, LV_SYMBOL_RIGHT, dnui_open, 0 },
    { "Network", NULL, "Bluetooth name", ST_ACTION, NULL, 0,0,0, NULL,0, LV_SYMBOL_RIGHT, apply_bluetooth_name, 0 },
    { "Network", NULL, "Audio Source", ST_ACTION, NULL, 0,0,0, NULL,0, LV_SYMBOL_RIGHT, apply_workmode, 0 },
    /* ---- Display ---- */
    { "Display", "Screen", "Brightness",  ST_SLIDER, "brightness", 4,40,2, NULL,0, NULL, apply_brightness_row, 16 },
    { "Display", "Screen", "Font Size",   ST_CYCLER, "font_size", 0,0,0, OPT_FONTSIZE, 3, NULL, apply_font_size, 1 },
    { "Display", "Screen", "Screen Rotation", ST_CYCLER, CTL_CFG_ROT, 0,0,0, OPT_ROTATION, CTL_ROT_N, NULL, apply_screen_rot, 0 },
    { "Display", "Screen", "Outdoor Mode", ST_TOGGLE, "outdoor",  0,1,1, NULL, 0, NULL, apply_outdoor, 0 },
    { "Display", "Appearance", "Theme",       ST_CHOICE, "theme_preset", 0,0,0, theme_preset_names, THEME_PRESET_COUNT, NULL, apply_theme_preset, 0 },
    { "Display", "Appearance", "Appearance",  ST_CYCLER, "theme_variant", 0,0,0, OPT_APPEARANCE, 2, NULL, apply_appearance, 0 },
    { "Display", "Appearance", "Accent Colour", ST_ACTION, NULL, 0,0,0, NULL,0, LV_SYMBOL_RIGHT, apply_open_colorpick, 0 },
    { "Display", "Appearance", "Animations",  ST_TOGGLE, "anim",      0,1,1, NULL, 0, NULL, apply_anim, 1 },
    { "Display", "Music and Home", "Now Playing", ST_CYCLER, "np_style",   0,0,0, OPT_NPSTYLE, 2, NULL, apply_np_style, 0 },
    { "Display", "Music and Home", "Weather on Home", ST_TOGGLE, "weather_on", 0,1,1, NULL, 0, NULL, apply_weather, 1 },
    { "Display", "Screensaver", "Screensaver", ST_CYCLER, "saver_idx",  0,0,0, OPT_POWER, 5, NULL, NULL, 2 },
    { "Display", "Screensaver", "Saver Style", ST_CHOICE, "saver_style", 0,0,0, OPT_SAVERSTYLE, 6, NULL, NULL, 0 },
    { "Display", "Screensaver", "Screen Off",  ST_CYCLER, "screenoff_idx", 0,0,0, OPT_POWER, 5, NULL, NULL, 3 },
    /* ---- Library ---- */
    { "Library", "Browsing", "Artists",     ST_CYCLER, "artist_class", 0,0,0, OPT_ARTISTCLASS, 2, NULL, apply_artist_class, 0 },
    { "Library", "Browsing", "Track Numbers", ST_TOGGLE, "track_numbers", 0,1,1, NULL, 0, NULL, apply_track_numbers, 0 },
    { "Library", "Browsing", "Album View",  ST_CYCLER, "album_view", 0,0,0, OPT_ALBUMVIEW, 2, NULL, NULL, 0 },
    { "Library", "Artwork and lyrics", "Online Album Art", ST_TOGGLE, "online_art", 0,1,1, NULL, 0, NULL, apply_online_art, 0 },
    { "Library", "Artwork and lyrics", "Album Art Cache", ST_CYCLER, "artcache", 0,0,0, OPT_ARTCACHE, 3, NULL, apply_artcache, 0,
      "Loads covers in advance" },
    { "Library", "Artwork and lyrics", "Online Lyrics", ST_TOGGLE, "online_lyrics", 0,1,1, NULL, 0, NULL, NULL, 1 },
    { "Library", "Maintenance", "Tag Encoding", ST_CYCLER, "old_tags", 0,0,0, OPT_OLDTAGS, 3, NULL, apply_old_tags, OLD_TAGS_AUTO,
      "Text encoding of old tags" },
    { "Library", "Maintenance", "Rescan Library", ST_ACTION, NULL, 0,0,0, NULL,0, "Scan", apply_rescan, 0 },
    { "Library", "Maintenance", "Import Playlists", ST_ACTION, NULL, 0,0,0, NULL,0, "Import", apply_import_m3u, 0 },
    /* ---- Controls ---- */
    { "Controls", NULL, "Quick Settings", ST_ACTION, NULL, 0,0,0, NULL,0, LV_SYMBOL_RIGHT, apply_qsconfig, 0 },
    { "Controls", NULL, "Vol Keys: Press", ST_CYCLER, "key_single", 0,0,0, OPT_VOLKEY, 2, NULL, apply_key_single, CTL_ACT_VOLUME },
    { "Controls", NULL, "Vol Keys: Double Press", ST_CYCLER, "key_double", 0,0,0, OPT_VOLKEY, 2, NULL, apply_key_double, CTL_ACT_VOLUME },
    { "Controls", NULL, "Vol Keys: Long Press", ST_CYCLER, "key_long", 0,0,0, OPT_VOLKEY, 2, NULL, apply_key_long, CTL_ACT_VOLUME },
    { "Controls", NULL, "Back-swipe",  ST_SLIDER, "swipe_thresh", 30,120,5, NULL,0, NULL, apply_swipe, 60 },
    /* ---- Battery & Power ---- */
    { "Battery & Power", NULL, "Charging Limit", ST_TOGGLE, "charge_protect", 0,1,1, NULL, 0, NULL, apply_charge_protect, 0 },
    { "Battery & Power", NULL, "Idle Power-off", ST_CYCLER, "idle_off_idx", 0,0,0, OPT_IDLE_OFF, POWER_IDLE_N, NULL, NULL, 0 },
    /* ---- Device ---- */
    { "Device", "Language and clock", "Language",    ST_CHOICE, "language", 0,0,0, i18n_lang_names, LANG_COUNT, NULL, apply_language, LANG_EN },
    { "Device", "Language and clock", "Automatic Time", ST_TOGGLE, "auto_time", 0,1,1, NULL, 0, NULL, apply_auto_time, 1 },
    { "Device", "Language and clock", "Set Date & Time", ST_ACTION, NULL, 0,0,0, NULL,0, LV_SYMBOL_RIGHT, apply_open_datetime, 0 },
    { "Device", "Language and clock", "Time Zone",   ST_CHOICE, "tz_idx", 0,0,0, TZ_LABEL, TZ_COUNT, NULL, apply_timezone, 0 },
    { "Device", "Language and clock", "24-Hour Time", ST_TOGGLE, "time_24h",  0,1,1, NULL, 0, NULL, apply_time, 1 },
    { "Device", "Startup", "Default UI",  ST_CYCLER, "boot_default", 0,0,0, OPT_BOOTDEF, 2, NULL, apply_boot_default, 0 },
    { "Device", "Information", "Device Info", ST_ACTION, NULL, 0,0,0, NULL,0, LV_SYMBOL_RIGHT, system_about_open, 0 },
    { "Device", "Updates", "Allow diskOS Updates", ST_TOGGLE, "ota_allow", 0,1,1, NULL, 0, NULL, system_ota_allow_changed, 1 },
    { "Device", "Updates", "Automatic Update Checks", ST_TOGGLE, "ota_auto", 0,1,1, NULL, 0, NULL, system_ota_auto_changed, 0 },
    { "Device", "Updates", "Update diskOS", ST_ACTION, NULL, 0,0,0, NULL,0, "Check", system_check_update_open, 0 },
    { "Device", "Updates", "Stock Update Guide", ST_ACTION, NULL, 0,0,0, NULL,0, LV_SYMBOL_RIGHT, system_update_sd_open, 0 },
    { "Device", "Maintenance", "Restart",     ST_ACTION, NULL, 0,0,0, NULL,0, "Restart", apply_restart, 0 },
    { "Device", "Maintenance", "Reset diskOS Settings", ST_ACTION, NULL, 0,0,0, NULL,0, "Reset", system_reset_open, 0 },
    { "Device", "Maintenance", "Debug Mode",  ST_ACTION, NULL, 0,0,0, NULL,0, LV_SYMBOL_RIGHT, apply_debug_mode, 0 },
};
#define N_SETTINGS ((int)(sizeof(TABLE)/sizeof(TABLE[0])))

static int get_val(const setting_t *s){
    if(s->cfg_key && !strcmp(s->cfg_key, "bt_codec")) return mdb_sysconfig_bt_codec();
    if(s->cfg_key && !strcmp(s->cfg_key, "ota_auto"))
        return !strcmp(cfg_get_str(s->cfg_key, "0"), "1");
    return s->cfg_key ? cfg_get_int(s->cfg_key, s->def) : 0;
}
static void set_val(const setting_t *s, int v){
    if(s->cfg_key && !strcmp(s->cfg_key, "work_mode") && ui_receiver_in_use()){
        ui_toast("Return to Local first");
        return;
    }
    /* EQ and automatic checks persist after success; stock owns codec storage. */
    g_set_prev = get_val(s);
    if(s->cfg_key && strcmp(s->cfg_key, "eq_preset") && strcmp(s->cfg_key, "bt_codec") &&
       strcmp(s->cfg_key, "ota_auto")){
        cfg_begin();
        int bad = cfg_set_int_deferred(s->cfg_key, v);
        int rc = cfg_commit();
        if(bad || (rc && rc != -2)){
            ui_toast("Could not save");
            lv_async_call(detail_refresh_async, NULL);
            return;
        }
        if(rc == -2) ui_toast("Saved - not confirmed yet");
    }
    if(s->apply) s->apply(v);
}
/* "1.2.0 (8c29ecbf)": the release plus the first 8 hex of the RUNNING binary's md5 - the build id used in every
 * test record, and true even for a hand-deployed build. Hashed once (the binary is ~2.5 MB), then cached. */
static void read_about(char *buf, int n){
    static char id[9];
    if(!id[0]){
        snprintf(id, sizeof id, "?");
        int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
        struct stat st;
        if(fd >= 0 && fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size < 64*1024*1024){
            unsigned char *b = malloc((size_t)st.st_size);
            size_t got = 0;
            while(b && got < (size_t)st.st_size){
                ssize_t r = read(fd, b + got, (size_t)st.st_size - got);
                if(r < 0 && errno == EINTR) continue;
                if(r <= 0) break;
                got += (size_t)r;
            }
            if(b && got == (size_t)st.st_size){ char hex[33]; md5_hex(b, got, hex); snprintf(id, sizeof id, "%.8s", hex); }
            free(b);
        }
        if(fd >= 0) close(fd);
    }
    snprintf(buf, n, "%s (%s)", DISKOS_VERSION, id);
}
void settings_diskos_id(char *buf, int n){ read_about(buf, n); }   /* system.c Device Info */
void settings_restart_prompt(const char *note){ g_restart_note = note; apply_restart(0); }
/* Format a slider value WITH its unit - shared by val_text (committed value) and the live drag
 * callback (uncommitted value) so brightness shows "%" and swipe-thresh "px" while dragging too. */
static void fmt_slider(const setting_t *s, int v, char *buf, int n){
    if(s->cfg_key && !strcmp(s->cfg_key, "swipe_thresh"))    snprintf(buf,n, "%d px", v);
    else if(s->cfg_key && !strcmp(s->cfg_key, "brightness")) snprintf(buf,n, "%d%%", v*100/40);  /* 4..40 -> 10..100% */
    else                                                    snprintf(buf,n, "%d", v);
}
/* A setting the running firmware cannot apply (only Gain today: its player command is mapped per verified
 * firmware in fwcaps.c, and an unmapped firmware gets no command). The row stays visible so it is not a mystery,
 * but says so and is not editable - otherwise the menu would show a choice that silently does nothing. */
static int setting_unavailable(const setting_t *s){
    return s->cfg_key && ((!strcmp(s->cfg_key, "audio_gain") && fw_gain_tag() == NULL)
                       || (!strcmp(s->cfg_key, "artist_class") && !fw_artist_class_settable())
                       || (!strcmp(s->cfg_key, "folder_jump") && !fw_folder_jump_settable())
                       || (!strcmp(s->cfg_key, "bt_codec") && fw_os_ver() != 257)
                       || (!strcmp(s->cfg_key, "ota_allow") && !ota_supported(&OTA_DEV))
                       || (!strcmp(s->cfg_key, "ota_auto") && !ota_supported(&OTA_DEV))
                       || ((!strcmp(s->cfg_key, "idle_off_idx") || !strcmp(s->cfg_key, "charge_protect")
                            || !strcmp(s->cfg_key, "sleep_action")) && !power_supported())
                       || ((!strcmp(s->cfg_key, CTL_CFG_ROT) || !strcmp(s->cfg_key, "key_single")
                            || !strcmp(s->cfg_key, "key_double") || !strcmp(s->cfg_key, "key_long")) && !ctl_supported()));
}
static const char *val_text(const setting_t *s, char *buf, int n){
    if(setting_unavailable(s)) return tr(s->cfg_key && (!strcmp(s->cfg_key, "ota_allow") || !strcmp(s->cfg_key, "ota_auto")) ? "Not supported on this install" : "Not on this firmware");
    /* Outdoor Mode overrides these two while it is on: show what is really in effect, not the saved choice (which
     * stays untouched and comes back when Outdoor Mode is turned off) */
    if(theme_outdoor() && s->cfg_key && !strcmp(s->cfg_key, "theme_variant")) return "Light (Outdoor)";
    if(theme_outdoor() && s->cfg_key && !strcmp(s->cfg_key, "brightness")) return "Full (Outdoor)";
    int v = get_val(s);
    switch(s->type){
        case ST_TOGGLE:   return tr(v?"On":"Off");
        case ST_SLIDER:   fmt_slider(s, v, buf, n); break;
        case ST_CYCLER: case ST_CHOICE: return (v>=0&&v<s->nopts)?tr(s->opts[v]):"?";
        case ST_READONLY: return tr(s->ro_val?s->ro_val:"");
        case ST_ACTION:   return tr((s->apply==apply_rescan && scanner_active()) ? "Stop" : (s->ro_val?s->ro_val:""));
    }
    return buf;
}

/* ---- shared widgets ----------------------------------------------------- */

static lv_obj_t *g_list_rows[N_SETTINGS];   /* value labels, or inline switches */

static void setting_value_deleted(lv_event_t *e){
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if(idx >= 0 && idx < N_SETTINGS && g_list_rows[idx] == lv_event_get_target(e))
        g_list_rows[idx] = NULL;
}

/* Only the Update diskOS row owns this dot; deletion clears its observer. */
static lv_obj_t *g_ota_dot;
static void ota_dot_deleted(lv_event_t *e){ (void)e; g_ota_dot = NULL; }
static void ota_dot_refresh(void){
    if(!g_ota_dot) return;
    if(system_ota_available()) lv_obj_clear_flag(g_ota_dot, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(g_ota_dot, LV_OBJ_FLAG_HIDDEN);
}

/* Rescan Library row: title shows "Scanning... N songs" and the action word reads "Stop" ("Cancel" is cut off in the 96 px box) while the scanner thread
 * runs. The timer lives and dies with the row, so leaving the screen and coming back rebuilds it from the counter. */
typedef struct { lv_obj_t *lbl, *vl; lv_timer_t *tm; } rescan_row_t;
static void rescan_row_tick(lv_timer_t *t){
    rescan_row_t *rr = lv_timer_get_user_data(t);
    char b[40];
    if(scanner_active()){
        int done = 0; scanner_progress(&done, NULL);
        if(done >= 1000) snprintf(b, sizeof b, "Scanning... %d,%03d", done / 1000, done % 1000);
        else             snprintf(b, sizeof b, "Scanning... %d", done);
        lv_label_set_text(rr->lbl, b);
        lv_label_set_text(rr->vl, tr("Stop"));
    } else {
        lv_label_set_text(rr->lbl, tr(mdb_sort_rescan_needed() ? "Rescan for sort tags" : "Rescan Library"));
        lv_label_set_text(rr->vl, tr("Scan"));
    }
}
static void rescan_row_del_cb(lv_event_t *e){
    rescan_row_t *rr = lv_event_get_user_data(e);
    lv_timer_delete(rr->tm);
    free(rr);
}
static const setting_t *g_active;           /* setting shown in the detail screen */
static lv_obj_t *g_detail_root;
static lv_obj_t *g_setlist_root;            /* SCR_SETLIST root: one category's rows, rebuilt per entry */
static lv_obj_t *g_setlist_list;            /* current column; old-list delete events must not reset its offset */
static const char *g_active_group;          /* which category SCR_SETLIST is currently showing */
/* Category order for the top-level Settings screen (must match the group strings used in TABLE). */
static const char *const GROUPS[] = { "Playback", "Audio", "Network", "Display", "Library", "Controls", "Battery & Power", "Device" };
#define N_GROUPS ((int)(sizeof(GROUPS)/sizeof(GROUPS[0])))
static int32_t g_group_scroll[N_GROUPS];     /* returning from a detail keeps each category's place */

static int active_group_index(void){
    for(int i = 0; i < N_GROUPS; i++) if(g_active_group == GROUPS[i]) return i;
    return -1;
}

static void setlist_scroll_cb(lv_event_t *e){
    int gi = active_group_index();
    if(gi >= 0 && lv_event_get_target(e) == g_setlist_list)
        g_group_scroll[gi] = lv_obj_get_scroll_y(g_setlist_list);
}

static void setlist_scroll_remember(void){
    int gi = active_group_index();
    if(gi >= 0 && g_setlist_list) g_group_scroll[gi] = lv_obj_get_scroll_y(g_setlist_list);
}



/* ---- detail screen ------------------------------------------------------ */
static void detail_back_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) screen_back(); }

/* VALUE_CHANGED: apply live only (no flash write); RELEASED persists once. This
 * avoids rewriting the whole config file on every pixel of a slider drag. */
static void detail_slider_cb(lv_event_t *e){
    lv_obj_t *sl = lv_event_get_target(e);
    int v = lv_slider_get_value(sl);
    if(g_active->apply) g_active->apply(v);
    lv_obj_t *vl = lv_event_get_user_data(e);
    if(vl){ char b[16]; fmt_slider(g_active, v, b, sizeof b); lv_label_set_text(vl, b); }   /* keep %/px unit while dragging */
}
static int outdoor_holds(const setting_t *s){ return theme_outdoor() && s->cfg_key && !strcmp(s->cfg_key, "brightness"); }
static void detail_slider_release_cb(lv_event_t *e){
    lv_obj_t *sl = lv_event_get_target(e);
    if(outdoor_holds(g_active)) return;                 /* Outdoor keeps the user's own level untouched */
    /* brightness is applied again: a wake during the drag lit the saved level */
    if(g_active->apply == apply_brightness_row) ui_set_brightness(lv_slider_get_value(sl));
    else if(g_active->cfg_key) cfg_set_int(g_active->cfg_key, lv_slider_get_value(sl));
}
static void bt_codec_saved_tick(lv_timer_t *t){
    if(screen_current() != SCR_SETTING_DETAIL) return;
    lv_obj_t *label = lv_timer_get_user_data(t);
    int v = mdb_sysconfig_bt_codec();
    if(v >= 0 && v < BT_CODEC_N) lv_label_set_text(label, tr(BT_CODEC_LABEL[v]));   /* busy: keep the last read */
}
static void bt_codec_saved_del(lv_event_t *e){
    lv_timer_delete(lv_event_get_user_data(e));
}
static void detail_cycle_cb(lv_event_t *e){
    if(theme_outdoor() && g_active->cfg_key && !strcmp(g_active->cfg_key, "theme_variant")){
        ui_toast("Outdoor Mode keeps Light on");   /* a Dark/Light change could not show until Outdoor Mode is off */
        return;
    }
    int dir = (int)(uintptr_t)lv_event_get_user_data(e);
    int v = get_val(g_active) + dir;
    if(v < 0) v = g_active->nopts-1;
    if(v >= g_active->nopts) v = 0;
    set_val(g_active, v);
    /* rebuild detail body to reflect new value */
    void setting_detail_refresh(void); setting_detail_refresh();
}
static lv_obj_t *make_list(lv_obj_t *root);
static lv_obj_t *setting_card(lv_obj_t *list);
static void detail_choice_async(void *u){ (void)u; setting_detail_refresh(); }
/* ---- Saver Style page (slice 17a): six choices, the sixth is Custom (saver.c SAVER_STYLE_CUSTOM) ---- */
#define SS_CUSTOM 5
static int is_saver_row(const setting_t *s){ return s && s->cfg_key && !strcmp(s->cfg_key, "saver_style"); }
/* The one writer of the Custom selection: this page, and a successful Use Image (decision 80). */
int settings_saver_select_custom(void){ return cfg_set_int("saver_style", SS_CUSTOM); }

/* Leaving Custom for a built-in style (decision 35) selects stock's Clock theme row, the order of mq_ui@0x47c6fc: 0810
 * value 0, then USE on POS_ID 0. The custom files stay. The database write blocks up to 3 s, so a detached worker does
 * it and the UI thread polls; apply_reserve keeps it out of a running image apply. A failure puts the player back to
 * Custom and leaves saver_style unchanged. */
static int bg_start(void *(*fn)(void *)){   /* a detached 256 KiB worker for a blocking player/database step */
    pthread_t th; pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 * 1024);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int bad = pthread_create(&th, &at, fn, NULL);
    pthread_attr_destroy(&at);
    return bad;
}
static struct { int busy, style; atomic_int fin, rc; } g_ss;
static struct { int busy; atomic_int fin, rc; } g_cu;
static void *ss_worker(void *u){
    (void)u;
    int rc = -1;
    if(!ipc_apply_reserve()){
        if(!ipc_saver_reselect(0)){
            rc = (int)custom_saver_db_select(NULL, 0, 1, NULL);
            if(rc) ipc_saver_reselect(SS_CUSTOM);
        }
        ipc_apply_release();
    }
    atomic_store(&g_ss.rc, rc);
    atomic_store(&g_ss.fin, 1);
    return NULL;
}
static void ss_tick(lv_timer_t *t){
    if(!atomic_load(&g_ss.fin)) return;
    lv_timer_delete(t);
    g_ss.busy = 0;
    if(atomic_load(&g_ss.rc)) ui_toast("Could not change style - Custom kept");
    else cfg_set_int("saver_style", g_ss.style);
    if(is_saver_row(g_active)) setting_detail_refresh();
}
static void ss_leave_custom(int style){
    if(g_ss.busy) return;
    g_ss.busy = 1; g_ss.style = style; atomic_store(&g_ss.fin, 0);
    if(bg_start(ss_worker)){ g_ss.busy = 0; ui_toast("Could not change style - Custom kept"); return; }
    lv_timer_create(ss_tick, 50, NULL);
}
/* Custom always enters its page (selected already or not: it is the only way in). It is SELECTED here only when stock's
 * database already has a custom image active, i.e. the saver would show it; otherwise nothing is written: Use Saved Image
 * or a successful Choose Image (decision 80) selects it, so the check never sits on a Minimal fallback. */
static void saver_style_tap(int i){
    if(g_ss.busy || g_cu.busy){
        ui_toast("Player busy - try again");
        return;
    }
    int cur = get_val(g_active);
    if(i == SS_CUSTOM){
        custom_saver_asset a;
        if(cur != SS_CUSTOM && custom_saver_db_read(NULL, &a) == CUSTOM_SAVER_OK){ settings_saver_select_custom(); lv_async_call(detail_choice_async, NULL); }
        settings_custom_open();
    } else if(i != cur){
        if(cur == SS_CUSTOM) ss_leave_custom(i);
        else { set_val(g_active, i); lv_async_call(detail_choice_async, NULL); }
    }
}
static void detail_choice_cb(lv_event_t *e){
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(g_active && is_saver_row(g_active)){ saver_style_tap(i); return; }
    if(!g_active || i == get_val(g_active)) return;
    set_val(g_active, i);                               /* a theme re-launches the UI from here */
    lv_async_call(detail_choice_async, NULL);           /* the tick moves after this event (rows can't delete themselves) */
}

void setting_detail_refresh(void){
    if(!g_detail_root || !g_active) return;
    lv_obj_clean(g_detail_root);
    lv_obj_set_style_bg_color(g_detail_root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(g_detail_root, LV_OPA_COVER, 0);
    ui_header_cb(g_detail_root, g_active->label, detail_back_cb);   /* shared header */

    const setting_t *s = g_active;
    int v = get_val(s);

    if(s->type == ST_SLIDER){
        lv_obj_t *sl = lv_slider_create(g_detail_root);
        lv_obj_set_size(sl, 220, 12);
        lv_obj_set_ext_click_area(sl, 16);             /* 12 px rail + 16 above/below = 44 px touch height */
        lv_obj_align(sl, LV_ALIGN_CENTER, 0, -6);
        lv_slider_set_range(sl, s->min, s->max);
        if(outdoor_holds(s)){ v = ui_effective_brightness(); lv_obj_add_state(sl, LV_STATE_DISABLED); }
        lv_slider_set_value(sl, v, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(sl, TC(CONTROL_TRACK), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sl, ui_current_accent(), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sl, TC(CONTROL_KNOB), LV_PART_KNOB);
        lv_obj_t *vl = lv_label_create(g_detail_root);
        char b[16]; lv_label_set_text(vl, val_text(s,b,sizeof b));
        lv_obj_set_width(vl, 360); lv_obj_align(vl, LV_ALIGN_CENTER, 0, 40);
        lv_obj_set_style_text_align(vl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(vl, ui_current_accent(), 0);
        lv_obj_set_style_text_font(vl, TF(UI_20), 0);
        ui_on(sl, detail_slider_cb, LV_EVENT_VALUE_CHANGED, vl, "settings.detail_slider.value", UI_CORE);
        lv_obj_add_event_cb(sl, detail_slider_release_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(sl, detail_slider_release_cb, LV_EVENT_PRESS_LOST, NULL);
    } else if(s->type == ST_CHOICE){
        /* every option as a row; the current one ticked. Themes also show three colour dots (background, surface,
         * accent) in the current Dark/Light variant, so each can be judged before picking. */
        lv_obj_t *list = make_list(g_detail_root);
        if(s->opts == i18n_lang_names) lv_obj_set_height(list, 215);
        int is_theme = !strcmp(s->cfg_key, "theme_preset");
        int is_saver = is_saver_row(s);
        lv_obj_t *sel = NULL;
        for(int i = 0; i < s->nopts; i++){
            lv_obj_t *row = setting_card(list);
            int cur = (i == v);
            if(cur) lv_obj_set_style_bg_color(row, TC(SURFACE_SELECTED), 0);
            ui_on(row, detail_choice_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i, "settings.detail_choice", UI_CORE);
            if(is_theme) kit_row_layout(row, KIT_ROW_SWATCH);
            int x = 18;
            if(is_theme){
                static const theme_color_role_t DOTS[3] = { THEME_CLR_CANVAS, THEME_CLR_SURFACE_RAISED, THEME_CLR_ACCENT_PRIMARY };
                for(int k = 0; k < 3; k++){
                    lv_obj_t *dot = lv_obj_create(row);
                    lv_obj_remove_style_all(dot);
                    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
                    lv_obj_set_size(dot, 16, 16);
                    lv_obj_align(dot, LV_ALIGN_LEFT_MID, x + k * 12, 0);
                    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
                    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
                    lv_obj_set_style_bg_color(dot, theme_preset_color(i, DOTS[k]), 0);
                    lv_obj_set_style_border_width(dot, 1, 0);
                    lv_obj_set_style_border_color(dot, TC(BORDER_STRONG), 0);
                }
                x += 3 * 12 + 14;
            }
            lv_obj_t *lbl = lv_label_create(row);
            lv_label_set_text(lbl, tr(s->opts[i]));
            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, x, 0);
            lv_obj_set_style_text_font(lbl, s->opts == i18n_lang_names ? TF(USER_16) : TF(UI_16), 0);   /* native names: any script */
            lv_obj_set_style_text_color(lbl, TC(TEXT_PRIMARY), 0);
            int custom = is_saver && i == SS_CUSTOM;   /* Custom: check + chevron (it opens the Custom page) */
            if(cur){
                lv_obj_t *ck = lv_label_create(row);
                lv_label_set_text(ck, LV_SYMBOL_OK);
                lv_obj_align(ck, LV_ALIGN_RIGHT_MID, custom ? -40 : -16, 0);
                lv_obj_set_style_text_color(ck, ui_current_accent(), 0);
                sel = row;
            }
            if(custom){
                lv_obj_t *ch = lv_label_create(row);
                lv_label_set_text(ch, LV_SYMBOL_RIGHT);
                lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -16, 0);
                lv_obj_set_style_text_color(ch, TC(TEXT_SECONDARY), 0);
            }
            theme_list_row(row);
        }
        if(sel) lv_obj_scroll_to_view(sel, LV_ANIM_OFF);   /* every picker opens with its current choice in view */
    } else if(s->type == ST_CYCLER){
        /* < value > stepper */
        lv_obj_t *vl = lv_label_create(g_detail_root);
        char b[24]; lv_label_set_text(vl, val_text(s,b,sizeof b));
        if(s->apply == apply_bt_codec){
            lv_timer_t *poll = lv_timer_create(bt_codec_saved_tick, 1000, vl);
            if(poll) lv_obj_add_event_cb(vl, bt_codec_saved_del, LV_EVENT_DELETE, poll);
        }
        lv_obj_set_size(vl, 220, 32); lv_label_set_long_mode(vl, LV_LABEL_LONG_DOT);
        lv_obj_align(vl, LV_ALIGN_CENTER, 0, s->apply == apply_old_tags ? -20 : 0);
        lv_obj_set_style_text_align(vl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(vl, ui_current_accent(), 0);
        lv_obj_set_style_text_font(vl, TF(UI_24), 0);
        lv_obj_t *lb = lv_button_create(g_detail_root);
        lv_obj_remove_style_all(lb); lv_obj_set_size(lb, 48, 48);
        lv_obj_align(lb, LV_ALIGN_CENTER, -110, s->apply == apply_old_tags ? -20 : 0);
        lv_obj_t *li = lv_label_create(lb); lv_label_set_text(li, LV_SYMBOL_LEFT);
        lv_obj_set_style_text_color(li, TC(TEXT_PRIMARY), 0); lv_obj_center(li);
        ui_on(lb, detail_cycle_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)-1, "settings.detail_cycle", UI_CORE);
        lv_obj_t *rb = lv_button_create(g_detail_root);
        lv_obj_remove_style_all(rb); lv_obj_set_size(rb, 48, 48);
        lv_obj_align(rb, LV_ALIGN_CENTER, 110, s->apply == apply_old_tags ? -20 : 0);
        lv_obj_t *ri = lv_label_create(rb); lv_label_set_text(ri, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(ri, TC(TEXT_PRIMARY), 0); lv_obj_center(ri);
        ui_on(rb, detail_cycle_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)1, "settings.detail_cycle", UI_CORE);
    } else { /* readonly */
        lv_obj_t *vl = lv_label_create(g_detail_root);
        lv_label_set_text(vl, s->ro_val?s->ro_val:"");
        lv_obj_set_width(vl, 360); lv_obj_align(vl, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_text_align(vl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(vl, TC(TEXT_SECONDARY), 0);
        lv_obj_set_style_text_font(vl, TF(UI_20), 0);
    }

    /* Offer the existing scan action here; choosing a decoder never starts or cancels a scan. */
    if(s->apply == apply_old_tags){
        lv_obj_t *bt = lv_button_create(g_detail_root);
        lv_obj_remove_style_all(bt);
        lv_obj_set_size(bt, 240, 44);
        lv_obj_set_style_radius(bt, 12, 0);
        lv_obj_set_style_bg_opa(bt, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bt, TC(SURFACE), 0);
        lv_obj_set_style_bg_color(bt, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
        lv_obj_set_style_opa(bt, LV_OPA_40, LV_STATE_DISABLED);
        lv_obj_clear_flag(bt, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(bt, LV_ALIGN_CENTER, 0, 28);
        old_tags_rescan_watch(bt);
        lv_obj_t *l = lv_label_create(bt);
        lv_label_set_text(l, tr("Rescan Library"));
        lv_obj_set_style_text_font(l, TF(UI_16), 0);
        lv_obj_set_style_text_color(l, TC(TEXT_PRIMARY), 0);
        lv_obj_center(l);
        ui_on(bt, old_tags_rescan_cb, LV_EVENT_CLICKED, NULL, "settings.old_tags_rescan", UI_CORE);
    }

    /* one short line at the bottom, only for the few settings whose name alone is unclear */
    const char *desc = s->desc;
    if(desc && desc[0]){
        lv_obj_t *d = lv_label_create(g_detail_root);
        lv_label_set_text(d, tr(desc));
        lv_label_set_long_mode(d, LV_LABEL_LONG_DOT);   /* never wraps: a long translation ends in "..." */
        lv_obj_set_size(d, 240, 22);
        lv_obj_align(d, LV_ALIGN_BOTTOM_MID, 0, -40);
        lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(d, TF(UI_14), 0);
        lv_obj_set_style_text_color(d, TC(TEXT_MUTED), 0);
    }
}

void setting_detail_create(lv_obj_t *root){ g_detail_root = root; }

static void settings_reveal_row(int idx);

/* Deep links to toggles reveal their row; other controls keep their detail page. */
void settings_open_detail(int idx){
    if(idx<0 || idx>=N_SETTINGS) return;
    if(TABLE[idx].type == ST_TOGGLE){ settings_reveal_row(idx); return; }
    g_active = &TABLE[idx];
    screen_show(SCR_SETTING_DETAIL);
    setting_detail_refresh();
}

/* open a setting's detail page by its cfg key (robust to TABLE reordering). No-op if not found. */
void settings_open_key(const char *key){
    if(!key) return;
    for(int i=0;i<N_SETTINGS;i++)
        if(TABLE[i].cfg_key && !strcmp(TABLE[i].cfg_key, key)){
            if(TABLE[i].type != ST_TOGGLE && setting_unavailable(&TABLE[i])){ ui_toast("Not supported on this firmware yet"); return; }
            settings_open_detail(i); return;
        }
}

/* ---- master list -------------------------------------------------------- */
static void list_back_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) screen_back(); }

static void row_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    setlist_scroll_remember();
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    const setting_t *s = &TABLE[idx];
    if(s->type == ST_TOGGLE){
        if(setting_unavailable(s)){ ui_toast(tr("Not supported on this install")); return; }
        set_val(s, !get_val(s));
        settings_row_values_refresh();   /* read back AFTER the apply hook, including a refusal */
        return;
    }
    if(s->type == ST_ACTION){
        /* Actions provide their own completion feedback via ui_toast (in-place
         * actions like Rescan/Import) or by navigating to a screen (Wi-Fi/BT).
         * No permanent "Working..." label (it never resolved). */
        if(s->apply) s->apply(0);
        return;
    }
    if(s->type == ST_READONLY) return;   /* info rows (Temperature/About) aren't tappable */
    if(setting_unavailable(s)){ ui_toast("Not supported on this firmware yet"); return; }
    g_active = s;
    screen_show(SCR_SETTING_DETAIL);
    setting_detail_refresh();
}

/* the scrolling column that holds setting rows, positioned in the round screen's safe area */
static lv_obj_t *make_list(lv_obj_t *root){
    lv_obj_t *list = lv_obj_create(root);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 30, 70);
    lv_obj_set_size(list, 300, 250);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_row(list, 8, 0);       /* small gap between cards (matches the design) */
    lv_obj_set_style_pad_bottom(list, 44, 0);   /* round bottom bezel: last row must scroll fully clear */
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);   /* rows on the centre line */
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    return list;
}

/* one row card: a plain dark rounded pill (no accent bar - kept clean). */
static lv_obj_t *setting_card(lv_obj_t *list){
    lv_obj_t *row = lv_button_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 268, 50);   /* every list's rows are 268 wide (Library's): one margin everywhere */
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_bg_color(row, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(row, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

/* ---- Custom page (slice 17b): SCR_CUSTOM_PAGE, one level below Saver Style. Rows: Use Saved Image, Choose Image, Position
 * (inline), Save. Overlays, Font and Preview (17c/17d) get no placeholder rows: a row that does nothing is worse than none.
 * Position edits a draft (custom_ov_draft_*): Save commits it, Back discards it, neither touches the image. Use Saved Image
 * reselects the saved custom row on a worker, the leave-Custom pattern mirrored (reserve, player, database, compensate). ---- */
static lv_obj_t *g_cp_root, *g_cp_seg[CUSTOM_OV_POS_COUNT];
#define CO_N 5   /* the overlay switches, in the order of the Overlays page */
static const unsigned CO_BIT[CO_N] = { CUSTOM_OV_TIME, CUSTOM_OV_DATE, CUSTOM_OV_BATTERY, CUSTOM_OV_TRACK, CUSTOM_OV_WEATHER };
static const char *const CO_NAME[CO_N] = { "Time", "Date", "Battery", "Now Playing", "Weather" };
#define CU_FAILED "Could not use the saved image - previous kept"
static int cu_saved(custom_saver_asset *a){   /* the read waits for nothing (saver.c reads the same way on the UI thread) */
    struct stat sb;
    return custom_saver_db_saved(NULL, a) == CUSTOM_SAVER_OK && !stat(a->path, &sb) && S_ISREG(sb.st_mode);
}
static void *cu_worker(void *u){
    (void)u;
    int rc = -1; unsigned prev; custom_saver_asset a; struct stat sb;
    if(!ipc_apply_reserve()){
        if(!custom_saver_db_previous(CUSTOM_SAVER_DB_PATH, &prev) && !custom_saver_db_saved(NULL, &a) && !stat(a.path, &sb) && S_ISREG(sb.st_mode) &&
           !ipc_saver_reselect(SS_CUSTOM)){   /* the file may have gone since the page was built: check before the player is asked */
            rc = (int)custom_saver_db_select(NULL, 0, 0, a.path);   /* the custom row (0, 0) with the asset it already holds */
            if(rc) ipc_saver_reselect(prev);
        }
        ipc_apply_release();
    }
    atomic_store(&g_cu.rc, rc);
    atomic_store(&g_cu.fin, 1);
    return NULL;
}
static void cu_tick(lv_timer_t *t){
    if(!atomic_load(&g_cu.fin)) return;
    lv_timer_delete(t);
    g_cu.busy = 0;
    if(atomic_load(&g_cu.rc)) ui_toast(CU_FAILED);
    else { settings_saver_select_custom(); ui_toast("Image set"); }
    if(is_saver_row(g_active)) setting_detail_refresh();   /* the Saver Style page below shows the new check, visible or stacked */
    if(screen_current() == SCR_CUSTOM_PAGE) settings_custom_refresh();
}
static void cu_use_cb(lv_event_t *e){
    (void)e;
    if(g_ss.busy || g_cu.busy){
        ui_toast("Player busy - try again");
        return;
    }
    g_cu.busy = 1; atomic_store(&g_cu.fin, 0);
    if(bg_start(cu_worker)){ g_cu.busy = 0; ui_toast(CU_FAILED); return; }
    lv_timer_create(cu_tick, 50, NULL);
}
static void cp_choose_cb(lv_event_t *e){ (void)e; custom_picker_open(); }
static void cp_overlays_cb(lv_event_t *e){ (void)e; screen_show(SCR_CUSTOM_OVERLAYS); }
static void cp_font_cb(lv_event_t *e){ (void)e; screen_show(SCR_CUSTOM_FONT); }
static void cp_preview_cb(lv_event_t *e){ (void)e; screen_show(SCR_CUSTOM_PREVIEW); }
static void cp_back_cb(lv_event_t *e){ if(lv_event_get_code(e) == LV_EVENT_CLICKED) screen_back(); }   /* every Custom header: screenmgr.c drops the draft when the Custom area is left */
static void cp_save_cb(lv_event_t *e){
    (void)e;
    int rc = custom_ov_draft_save();
    ui_toast(rc == 0 ? "Saved" : rc == -2 ? "Saved - not confirmed yet" : "Could not save");
}
static void cp_seg_paint(void){   /* in place: a rebuild would reset the scroll */
    int pos = custom_ov_draft_get().pos;
    for(int k = 0; k < CUSTOM_OV_POS_COUNT; k++){
        if(!g_cp_seg[k]) continue;
        lv_obj_set_style_bg_color(g_cp_seg[k], k == pos ? ui_current_accent() : TC(SURFACE_RAISED), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(g_cp_seg[k], 0), k == pos ? theme_on_color(ui_current_accent()) : TC(TEXT_PRIMARY), 0);
    }
}
static void cp_pos_cb(lv_event_t *e){ custom_ov_draft_set_pos((int)(intptr_t)lv_event_get_user_data(e)); cp_seg_paint(); }
static lv_obj_t *cp_label(lv_obj_t *row, const char *text, int y, int w, const lv_font_t *f, lv_color_t c){
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, 18, y); lv_obj_set_size(l, w, y == 6 ? 20 : 18);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}
/* title + subtitle row; cb NULL = disabled (muted text, not clickable); chevron for rows that open a screen */
static lv_obj_t *cp_row(lv_obj_t *list, const char *title, const char *sub, lv_event_cb_t cb, const char *id, int chevron){
    lv_obj_t *row = setting_card(list);
    if(cb) ui_on(row, cb, LV_EVENT_CLICKED, NULL, id, UI_CORE); else lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    int w = chevron ? 214 : 190;   /* clear of the chevron, or of the format chip */
    cp_label(row, tr(title), 6, w, TF(UI_16), cb ? TC(TEXT_PRIMARY) : TC(TEXT_MUTED));
    cp_label(row, tr(sub), 27, w, TF(UI_14), cb ? TC(TEXT_SECONDARY) : TC(TEXT_MUTED));
    if(chevron){
        lv_obj_t *ch = lv_label_create(row);
        lv_label_set_text(ch, LV_SYMBOL_RIGHT);
        lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -14, 0);
        lv_obj_set_style_text_color(ch, TC(TEXT_SECONDARY), 0);
    }
    theme_list_row(row);
    return row;
}
void settings_custom_create(lv_obj_t *root){ g_cp_root = root; }
void settings_custom_open(void){ custom_ov_draft_begin(); screen_show(SCR_CUSTOM_PAGE); }
void settings_custom_refresh(void){
    if(!g_cp_root) return;
    lv_obj_clean(g_cp_root);
    memset(g_cp_seg, 0, sizeof g_cp_seg);
    lv_obj_set_style_bg_color(g_cp_root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(g_cp_root, LV_OPA_COVER, 0);
    ui_header_cb(g_cp_root, "Custom", cp_back_cb);
    lv_obj_t *list = make_list(g_cp_root);
    custom_saver_asset a;
    int have = cu_saved(&a);
    lv_obj_t *use = cp_row(list, "Use Saved Image", have ? "Last image set" : "No saved image yet", have ? cu_use_cb : NULL, "settings.custom.use", 0);
    if(have && (a.type == 204 || a.type == 200)){   /* GIF, or a converted still (always a PNG) */
        lv_obj_t *chip = lv_obj_create(use);
        lv_obj_remove_style_all(chip);
        lv_obj_clear_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(chip, 48, 24);
        lv_obj_align(chip, LV_ALIGN_RIGHT_MID, -12, 0);
        lv_obj_set_style_radius(chip, 12, 0);
        lv_obj_set_style_bg_color(chip, ui_current_accent(), 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_20, 0);
        lv_obj_t *t = lv_label_create(chip);
        lv_label_set_text(t, a.type == 204 ? "GIF" : "PNG");
        lv_obj_set_style_text_font(t, TF(UI_14), 0);
        lv_obj_set_style_text_color(t, ui_current_accent(), 0);
        lv_obj_center(t);
    }
    cp_row(list, "Choose Image", "From CustomCover on SD card", cp_choose_cb, "settings.custom.choose", 1);
    char ov[96] = "";   /* the DRAFT's values, so a change made on a sub-page shows at once */
    for(int i = 0; i < CO_N; i++)
        if(custom_ov_draft_get().mask & CO_BIT[i]) snprintf(ov + strlen(ov), sizeof ov - strlen(ov), "%s%s", ov[0] ? ", " : "", tr(CO_NAME[i]));
    cp_row(list, "Overlays", ov[0] ? ov : tr("None"), cp_overlays_cb, "settings.custom.overlays", 1);
    cp_row(list, "Font", custom_ov_font_name(custom_ov_draft_get().font), cp_font_cb, "settings.custom.font", 1);
    lv_obj_t *pc = setting_card(list);   /* Position: label over three 80 x 44 segments, inline (decision 78) */
    lv_obj_remove_flag(pc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_height(pc, 86);
    cp_label(pc, tr("Position"), 6, 232, TF(UI_16), TC(TEXT_PRIMARY));
    for(int k = 0; k < CUSTOM_OV_POS_COUNT; k++){
        lv_obj_t *b = lv_button_create(pc);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 80, 44);
        lv_obj_set_pos(b, 10 + k * 84, 32);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        ui_on(b, cp_pos_cb, LV_EVENT_CLICKED, (void*)(intptr_t)k, "settings.custom.pos", UI_CORE);
        lv_obj_t *t = lv_label_create(b);
        lv_label_set_text(t, tr(custom_ov_pos_name(k)));
        lv_obj_set_style_text_font(t, TF(UI_16), 0);
        lv_obj_center(t);
        g_cp_seg[k] = b;
    }
    cp_seg_paint();
    cp_row(list, "Preview", have ? "Shows the full result" : "Save an image first", have ? cp_preview_cb : NULL, "settings.custom.preview", have);   /* decision 83: disabled without a saved image */
    lv_obj_t *sv = lv_button_create(list);
    lv_obj_remove_style_all(sv);
    kit_button(sv);
    lv_obj_set_size(sv, 268, 50);
    lv_obj_set_style_radius(sv, 25, 0);
    lv_obj_set_style_bg_color(sv, ui_current_accent(), 0);
    lv_obj_set_style_bg_opa(sv, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(sv, TC(RAISED_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(sv, TC(SURFACE_RAISED), LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(sv, LV_OPA_COVER, LV_STATE_DISABLED);
    lv_obj_set_style_text_color(sv, theme_on_color(ui_current_accent()), 0);
    lv_obj_set_style_text_color(sv, TC(TEXT_PRIMARY), LV_STATE_DISABLED);
    lv_obj_set_style_text_color(sv, TC(TEXT_PRIMARY), LV_STATE_PRESSED);
    ui_on(sv, cp_save_cb, LV_EVENT_CLICKED, NULL, "settings.custom.save", UI_CORE);
    lv_obj_t *st = lv_label_create(sv);
    lv_label_set_text(st, tr("Save"));
    lv_obj_set_style_text_font(st, TF(UI_16), 0);
    /* Inherit the button's state-dependent foreground. */
    lv_obj_center(st);
}

/* ---- Overlays and Font pages (slice 17c): SCR_CUSTOM_OVERLAYS and SCR_CUSTOM_FONT, one level below the Custom page. Both edit
 * only the draft the Custom page began; their Back keeps it, the Custom page's Save commits it, and screenmgr.c discards it on
 * any exit from the Custom area. ---- */
static lv_obj_t *g_co_root, *g_cf_root, *g_cf_row[CUSTOM_OV_FONT_COUNT], *g_cf_ck[CUSTOM_OV_FONT_COUNT];
static void co_toggle_cb(lv_event_t *e){   /* the whole row is the tap target; the switch only shows the state */
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *sw = lv_obj_get_child(lv_event_get_target(e), 1);
    int on = !lv_obj_has_state(sw, LV_STATE_CHECKED);
    if(on) lv_obj_add_state(sw, LV_STATE_CHECKED); else lv_obj_remove_state(sw, LV_STATE_CHECKED);
    custom_ov_draft_set_bit(CO_BIT[i], on);
    if(CO_BIT[i] == CUSTOM_OV_WEATHER) custom_ov_feed_tick();   /* a visible group re-reads weather now, not on the next 10 s clock tick */
}
static lv_obj_t *cc_page(lv_obj_t *root, const char *title){
    lv_obj_clean(root);
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    ui_header_cb(root, title, cp_back_cb);
    return make_list(root);
}
static lv_obj_t *cc_label(lv_obj_t *row, const char *text){   /* the row's name, centred vertically on the left */
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, tr(text));
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_set_style_text_font(l, TF(UI_16), 0);
    lv_obj_set_style_text_color(l, TC(TEXT_PRIMARY), 0);
    return l;
}
void settings_overlays_create(lv_obj_t *root){ g_co_root = root; }
void settings_overlays_refresh(void){
    if(!g_co_root) return;
    lv_obj_t *list = cc_page(g_co_root, "Overlays");
    for(int i = 0; i < CO_N; i++){
        lv_obj_t *row = setting_card(list);
        ui_on(row, co_toggle_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i, "settings.custom.overlay", UI_CORE);
        cc_label(row, CO_NAME[i]);
        lv_obj_t *sw = lv_switch_create(row);
        lv_obj_clear_flag(sw, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(sw, 52, 28);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -14, 0);
        lv_obj_set_style_bg_color(sw, ui_current_accent(), LV_PART_INDICATOR | LV_STATE_CHECKED);
        if(custom_ov_draft_get().mask & CO_BIT[i]) lv_obj_add_state(sw, LV_STATE_CHECKED);
        theme_list_row(row);
    }
}
static void cf_paint(void){   /* in place: a row cannot delete itself inside its own event */
    int f = custom_ov_draft_get().font;
    for(int k = 0; k < CUSTOM_OV_FONT_COUNT; k++){
        if(!g_cf_row[k]) continue;
        lv_obj_set_style_bg_color(g_cf_row[k], k == f ? TC(SURFACE_SELECTED) : TC(SURFACE), 0);
        lv_obj_set_style_bg_opa(g_cf_row[k], k == f || !theme_trait(THEME_TRAIT_FLAT_LISTS) ? LV_OPA_COVER : LV_OPA_TRANSP, 0);   /* selected rows and all non-flat rows use COVER; unselected flat-list rows stay transparent */
        if(k == f) lv_obj_remove_flag(g_cf_ck[k], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(g_cf_ck[k], LV_OBJ_FLAG_HIDDEN);
    }
}
static void cf_pick_cb(lv_event_t *e){ custom_ov_draft_set_font((int)(intptr_t)lv_event_get_user_data(e)); cf_paint(); }
void settings_font_create(lv_obj_t *root){ g_cf_root = root; }
void settings_font_refresh(void){
    if(!g_cf_root) return;
    lv_obj_t *list = cc_page(g_cf_root, "Font");
    for(int k = 0; k < CUSTOM_OV_FONT_COUNT; k++){
        lv_obj_t *row = setting_card(list);
        lv_obj_set_height(row, 56);   /* room for the sample */
        ui_on(row, cf_pick_cb, LV_EVENT_CLICKED, (void*)(intptr_t)k, "settings.custom.font_pick", UI_CORE);
        cc_label(row, custom_ov_font_name(k));
        lv_obj_t *box = lv_obj_create(row);   /* the sample time in the choice's REAL clock face, scaled to a 26 px digit height */
        lv_obj_remove_style_all(box);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(box, 96, 56);
        lv_obj_align(box, LV_ALIGN_RIGHT_MID, -44, 0);
        const lv_font_t *f = custom_ov_faces(k, CUSTOM_OV_SIZE_NORMAL).clock;
        lv_font_glyph_dsc_t g;
        if(f){
            lv_obj_t *s = lv_label_create(box);
            kit_keep(s);   /* keep the specimen in its chosen face through theme styling */
            lv_label_set_text(s, "11:33");
            lv_obj_set_style_text_font(s, f, 0);
            lv_obj_set_style_text_color(s, TC(TEXT_PRIMARY), 0);
            lv_obj_update_layout(s);
            lv_obj_center(s);
            int sc = 256;   /* 26 px digit height, and never wider than the 96 px box (a wide theme clock face was cut at both ends there) */
            if(lv_font_get_glyph_dsc(f, &g, '8', 0) && g.box_h > 26) sc = 26 * 256 / g.box_h;
            if(lv_obj_get_width(s) * sc / 256 > 92) sc = 92 * 256 / lv_obj_get_width(s);
            if(sc < 256){
                lv_obj_set_style_transform_pivot_x(s, lv_obj_get_width(s) / 2, 0);
                lv_obj_set_style_transform_pivot_y(s, lv_obj_get_height(s) / 2, 0);
                lv_obj_set_style_transform_scale(s, sc, 0);
            }
        }
        g_cf_ck[k] = lv_label_create(row);
        lv_label_set_text(g_cf_ck[k], LV_SYMBOL_OK);
        lv_obj_align(g_cf_ck[k], LV_ALIGN_RIGHT_MID, -16, 0);
        lv_obj_set_style_text_color(g_cf_ck[k], ui_current_accent(), 0);
        g_cf_row[k] = row;
        theme_list_row(row);
    }
    kit_pass(g_cf_root);   /* style new rows before restoring the draft selection */
    cf_paint();
}

/* ---- top-level category list (SCR_SETTINGS): Playback / Audio / Network / Display / Library / Controls / Battery & Power / Device ---- */
static void cat_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    int gi = (int)(uintptr_t)lv_event_get_user_data(e);
    if(gi<0 || gi>=N_GROUPS) return;
    g_active_group = GROUPS[gi];
    screen_show(SCR_SETLIST);   /* transition() rebuilds the rows via setlist_refresh */
}

/* open one category's rows by its name (deep links, host tests); no-op for an unknown name */
void setlist_open(const char *group){
    for(int gi = 0; group && gi < N_GROUPS; gi++)
        if(!strcmp(GROUPS[gi], group)){ g_active_group = GROUPS[gi]; screen_show(SCR_SETLIST); return; }
}

void settings_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    ui_header_cb(root, "Settings", list_back_cb);   /* shared header */

    lv_obj_t *list = make_list(root);
    for(int g=0; g<N_GROUPS; g++){
        lv_obj_t *row = setting_card(list);
        ui_on(row, cat_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)g, "settings.cat", UI_CORE);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, tr(GROUPS[g]));
        lv_obj_set_pos(lbl, 18, 15);
        lv_obj_set_width(lbl, 210);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(lbl, TF(UI_16), 0);
        lv_obj_set_style_text_color(lbl, TC(TEXT_PRIMARY), 0);

        lv_obj_t *ch = lv_label_create(row);   /* drill-down chevron */
        lv_obj_clear_flag(ch, LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_text(ch, LV_SYMBOL_RIGHT);
        lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -14, 0);
        lv_obj_set_style_text_font(ch, TF(UI_16), 0);
        lv_obj_set_style_text_color(ch, TC(TEXT_MUTED), 0);
        if(theme_trait(THEME_TRAIT_FLAT_LISTS)) lv_obj_set_style_text_font(lbl, TF(UI_20), 0);   /* big mono rows */
        lv_obj_set_height(lbl, lv_font_get_line_height(lv_obj_get_style_text_font(lbl, 0)));
        theme_list_row(row);
    }
}

/* Category list is static; nothing to re-sync when SCR_SETTINGS is (re)shown. */
void settings_refresh_list(void){ }
void settings_row_values_refresh(void){
    ota_dot_refresh();
    char b[40];
    for(int i=0;i<N_SETTINGS;i++) if(g_list_rows[i] && lv_obj_is_valid(g_list_rows[i])){
        lv_obj_t *o = g_list_rows[i];
        const radio_row_t *rr = radio_row((const void *)TABLE[i].apply);
        if(rr) radio_switch_paint(rr, o);
        else if(TABLE[i].type == ST_TOGGLE){
            if(get_val(&TABLE[i])) lv_obj_add_state(o, LV_STATE_CHECKED);
            else lv_obj_remove_state(o, LV_STATE_CHECKED);
            if(setting_unavailable(&TABLE[i])) lv_obj_add_state(o, LV_STATE_DISABLED);
            else lv_obj_remove_state(o, LV_STATE_DISABLED);
        } else lv_label_set_text(o, val_text(&TABLE[i],b,sizeof b));
    }
}

/* ---- one category's rows (SCR_SETLIST), rebuilt on every entry so values are always live ---- */
void setlist_create(lv_obj_t *root){ g_setlist_root = root; }

/* A radio row's body: with the radio on it opens the page; with it off it turns it on here, so a page holding only
 * its switch is never opened. While a start is pending the tap is ignored: the change under way decides. */
static void radio_row_cb(lv_event_t *e){
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    const radio_row_t *rr = idx >= 0 && idx < N_SETTINGS ? radio_row((const void *)TABLE[idx].apply) : NULL;
    if(!rr || rr->pending()) return;
    setlist_scroll_remember();
    if(rr->shown()){ TABLE[idx].apply(0); return; }
    rr->power(1);
    settings_row_values_refresh();
}
/* A radio row's switch is its own target: it turns the radio on or off and never opens the page. It cannot flip
 * itself (not checkable): the power path decides, then the switch is repainted from the state source. */
static void radio_switch_cb(lv_event_t *e){
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    const radio_row_t *rr = idx >= 0 && idx < N_SETTINGS ? radio_row((const void *)TABLE[idx].apply) : NULL;
    if(!rr || rr->pending()) return;
    setlist_scroll_remember();
    rr->power(!rr->shown());
    settings_row_values_refresh();
}
/* Visible radio rows follow outside changes (Quick Settings, stock, reconciliation, a start settling) once a second.
 * The reads are rfkill files and saved settings only; nothing runs while the screen is dark or Settings is covered. */
static void radio_rows_tick(lv_timer_t *t){
    (void)t;
    if(screen_current() != SCR_SETLIST || !ui_screen_backlight_on()) return;
    for(int i = 0; i < N_SETTINGS; i++){
        const radio_row_t *rr = radio_row((const void *)TABLE[i].apply);
        if(rr && g_list_rows[i] && lv_obj_is_valid(g_list_rows[i])) radio_switch_paint(rr, g_list_rows[i]);
    }
}
static void radio_rows_del_cb(lv_event_t *e){ lv_timer_delete(lv_event_get_user_data(e)); }

/* A section heading on a category page: a small label in the list, not a row (it has no action, no background and no
 * switch). The theme kit styles it as KIT_SECTION; 13 px of space above separates it from the section before. */
static void setlist_heading(lv_obj_t *list, const char *text, int first){
    lv_obj_t *h = lv_label_create(list);
    kit_section(h);
    lv_obj_clear_flag(h, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_label_set_text(h, tr(text));
    lv_label_set_long_mode(h, LV_LABEL_LONG_DOT);               /* one line; a long translation ends in "..." */
    lv_obj_set_style_text_font(h, TF(UI_12), 0);
    lv_obj_set_style_text_color(h, TC(TEXT_SECONDARY), 0);
    lv_obj_set_style_pad_left(h, 18, 0);
    lv_obj_set_style_pad_top(h, first ? 0 : 13, 0);
    lv_obj_set_size(h, 268, (first ? 0 : 13) + lv_font_get_line_height(TF(UI_12)));
}
/* Battery & Power opens with a summary of the fuel gauge (percent, health, cycles, temperature). It is
 * information, not a row: nothing in it is tappable. Read when the page is built. */
static void setlist_battery_summary(lv_obj_t *list){
    char b[96], t[128];
    sys_battery_text(NULL, 1, b, sizeof b);
    /* Each comma-separated gauge item gets its own line. LVGL must never
     * split a number to squeeze the last item onto a preceding line. */
    for(char *item = b; (item = strstr(item, ", ")); item++){
        *item = '\n'; memmove(item + 1, item + 2, strlen(item + 2) + 1);
    }
    snprintf(t, sizeof t, "%s %s", tr("Battery"), b);
    lv_obj_t *l = lv_label_create(list);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_label_set_text(l, t);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(l, 268);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(l, TF(UI_14), 0);
    lv_obj_set_style_text_color(l, TC(TEXT_SECONDARY), 0);
    lv_obj_set_style_pad_top(l, 4, 0);
    lv_obj_set_style_pad_bottom(l, 6, 0);
}

void setlist_refresh(void){
    if(!g_setlist_root || !g_active_group) return;
    int gi = active_group_index();
    int32_t saved_scroll = gi >= 0 ? g_group_scroll[gi] : 0;
    g_setlist_list = NULL;                   /* deletion can emit SCROLL as the old range collapses */
    memset(g_list_rows, 0, sizeof g_list_rows);
    lv_obj_clean(g_setlist_root);
    lv_obj_set_style_bg_color(g_setlist_root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(g_setlist_root, LV_OPA_COVER, 0);
    ui_header_cb(g_setlist_root, g_active_group, list_back_cb);   /* title = category; back pops to categories */

    lv_obj_t *list = make_list(g_setlist_root);
    g_setlist_list = list;
    lv_obj_add_event_cb(list, setlist_scroll_cb, LV_EVENT_SCROLL, NULL);
    int radio_rows = 0;
    const char *section = NULL;                          /* the heading drawn last on this page */
    if(!strcmp(g_active_group, "Battery & Power")) setlist_battery_summary(list);
    for(int i=0;i<N_SETTINGS;i++){
        const setting_t *s = &TABLE[i];
        if(strcmp(s->group, g_active_group)) continue;   /* only rows in this category */
        if(s->section && (!section || strcmp(section, s->section))) setlist_heading(list, s->section, lv_obj_get_child_count(list) == 0);
        section = s->section;
        const radio_row_t *rr = radio_row((const void *)s->apply);
        radio_rows += rr != NULL;

        lv_obj_t *row = setting_card(list);
        ui_on(row, rr ? radio_row_cb : row_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)i, "settings.row", UI_CORE);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, tr(s->label));
        lv_obj_set_pos(lbl, 18, 6);
        lv_obj_set_size(lbl, 232, 20);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(lbl, TF(UI_16), 0);
        lv_obj_set_style_text_color(lbl, TC(TEXT_PRIMARY), 0);

        char b[24]; const char *value = val_text(s, b, sizeof b);
        lv_obj_t *vl = lv_label_create(row);
        lv_label_set_text(vl, value);
        lv_obj_set_pos(vl, 18, 27);
        lv_obj_set_size(vl, 232, 18);
        lv_label_set_long_mode(vl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(vl, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_font(vl, s->opts == i18n_lang_names ? TF(USER_14) : TF(UI_14), 0);
        lv_obj_set_style_text_color(vl, (s->type==ST_READONLY || setting_unavailable(s)) ? TC(TEXT_MUTED) : TC(TEXT_SECONDARY), 0);
        if(s->type == ST_ACTION && !rr){
            /* Measure both captions with their final theme faces; a long name wraps beside the action. */
            lv_point_t sz;
            int32_t vw;
            if(theme_kit()->style){
                theme_kit()->style(row, KIT_ROW);
                theme_kit()->style(lbl, KIT_ROW_TEXT);
                theme_kit()->style(vl, KIT_ROW_SUB);
            }
            kit_isolate(row);
            lv_text_get_size(&sz, value, lv_obj_get_style_text_font(vl, 0),
                             lv_obj_get_style_text_letter_space(vl, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            vw = sz.x + 4; if(vw < 46) vw = 46; if(vw > 116) vw = 116;
            lv_obj_set_pos(lbl, 18, 10);
            lv_obj_set_size(lbl, 250 - vw - 8 - 18, LV_SIZE_CONTENT);
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
            lv_obj_set_size(vl, vw, LV_SIZE_CONTENT);
            lv_label_set_long_mode(vl, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_align(vl, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_update_layout(row);
            int h = lv_obj_get_height(lbl) > lv_obj_get_height(vl) ? lv_obj_get_height(lbl) : lv_obj_get_height(vl);
            lv_obj_set_y(lbl, 10 + (h - lv_obj_get_height(lbl)) / 2);
            lv_obj_set_pos(vl, 250 - vw, 10 + (h - lv_obj_get_height(vl)) / 2);
            lv_obj_set_height(row, h + 20);
            if(theme_kit()->row_decor) theme_kit()->row_decor(row, lbl);
        }
        if(s->apply == system_check_update_open){
            g_ota_dot = lv_obj_create(row);
            lv_obj_remove_style_all(g_ota_dot);
            lv_obj_clear_flag(g_ota_dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_size(g_ota_dot, 6, 6);
            lv_obj_set_pos(g_ota_dot, 8, 22);
            lv_obj_set_style_radius(g_ota_dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(g_ota_dot, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(g_ota_dot, TC(ACCENT_EMPHASIS), 0);
            lv_obj_add_event_cb(g_ota_dot, ota_dot_deleted, LV_EVENT_DELETE, NULL);
            ota_dot_refresh();
        }
        if(s->type == ST_TOGGLE || rr){
            /* The row owns the tap. A switch cannot toggle itself before an apply hook refuses it.
             * A row has no help line unless its name alone is unclear (setting_t.desc): then one short line sits
             * below the label. The row is as tall as its content and the switch is centred on it.
             * A radio row's switch is its own target (radio_switch_cb); the rest of the row opens or starts it. */
            if(theme_kit()->style){
                theme_kit()->style(row, KIT_ROW);
                theme_kit()->style(lbl, KIT_ROW_TEXT);
                theme_kit()->style(vl, KIT_ROW_SUB);
            }
            kit_isolate(row);  /* preserve the measured height and the real switch through later passes */
            const int has_help = s->desc && s->desc[0];
            lv_obj_set_size(lbl, rr ? 170 : 174, LV_SIZE_CONTENT);   /* a radio caption ends before the switch target, also at a theme's x24 */
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(vl, 232);
            lv_label_set_long_mode(vl, LV_LABEL_LONG_DOT);   /* one line; a long translation ends in "..." */
            lv_label_set_text(vl, has_help ? tr(s->desc) : "");
            lv_obj_set_height(vl, lv_font_get_line_height(lv_obj_get_style_text_font(vl, 0)));
            lv_obj_t *reason = NULL;
            if(setting_unavailable(s)){
                reason = lv_label_create(row);
                lv_label_set_text_fmt(reason, "%s.", val_text(s,b,sizeof b));
                lv_obj_set_style_text_font(reason, TF(UI_12), 0);
                lv_obj_set_style_text_color(reason, TC(TEXT_MUTED), 0);
                lv_obj_set_width(reason, 232);
                lv_label_set_long_mode(reason, LV_LABEL_LONG_WRAP);
                lv_obj_set_x(reason, lv_obj_get_style_x(vl, 0));
            }
            lv_obj_t *sw = lv_switch_create(row);
            lv_obj_remove_flag(sw, (rr ? LV_OBJ_FLAG_CHECKABLE : LV_OBJ_FLAG_CLICKABLE) | LV_OBJ_FLAG_SCROLLABLE);
            if(rr){
                lv_obj_set_ext_click_area(sw, 10);   /* 64 x 44 target, clear of the 170 px caption */
                ui_on(sw, radio_switch_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i, "settings.radio_switch", UI_CORE);
            }
            theme_settings_switch(sw);
            lv_obj_set_x(sw, 206);
            if(rr) radio_switch_paint(rr, sw);
            else if(get_val(s)) lv_obj_add_state(sw, LV_STATE_CHECKED);
            if(!rr && setting_unavailable(s)) lv_obj_add_state(sw, LV_STATE_DISABLED);
            lv_obj_update_layout(row);
            if(theme_kit()->row_decor) theme_kit()->row_decor(row, lbl);
            lv_obj_update_layout(row);
            /* Caption and switch share one line (10 px above, the taller of the two); a reason and a kept help line stack
             * below it; the row ends 10 px under its last line. No help line: the row is exactly that tall, no gap. */
            const int lbl_h = lv_obj_get_height(lbl);
            const int body = lbl_h > 24 ? lbl_h : 24;
            const int lbl_y = 10 + (body - lbl_h) / 2;
            int end_y = 10 + body;
            lv_obj_set_y(lbl, lbl_y);
            lv_obj_set_y(sw, 10 + (body - 24) / 2);
            if(reason){
                lv_obj_set_y(reason, end_y + 2);
                lv_obj_update_layout(row);
                end_y += 2 + lv_obj_get_height(reason);
            }
            if(has_help){
                lv_obj_set_y(vl, end_y + 4);
                end_y += 4 + lv_obj_get_height(vl);
            } else lv_obj_delete(vl);
            lv_obj_set_height(row, end_y + 10);
            g_list_rows[i] = sw;
        } else g_list_rows[i] = vl;
        lv_obj_add_event_cb(g_list_rows[i], setting_value_deleted, LV_EVENT_DELETE, (void *)(uintptr_t)i);
        if(s->apply == apply_rescan){                  /* live scan progress on this row while a scan runs */
            rescan_row_t *rr = malloc(sizeof *rr);
            if(rr){
                rr->lbl = lbl; rr->vl = vl;
                rr->tm = lv_timer_create(rescan_row_tick, 400, rr);
                lv_obj_add_event_cb(row, rescan_row_del_cb, LV_EVENT_DELETE, rr);
                rescan_row_tick(rr->tm);
            }
        }
        theme_list_row(row);
    }
    if(radio_rows){                            /* the list owns the timer; cleaning the page deletes both */
        lv_timer_t *t = lv_timer_create(radio_rows_tick, 1000, NULL);
        if(t) lv_obj_add_event_cb(list, radio_rows_del_cb, LV_EVENT_DELETE, t);
    }
    if(gi >= 0){
        lv_obj_update_layout(list);            /* establish the new scroll range before restoring it */
        lv_obj_scroll_to_y(list, saved_scroll, LV_ANIM_OFF);
    }
}

/* A short, static focus mark explains a Quick Settings long press without changing the value. */
static void reveal_done(lv_timer_t *t){
    lv_obj_remove_state(lv_timer_get_user_data(t), LV_STATE_FOCUSED);
    lv_timer_pause(t);
}
static void reveal_deleted(lv_event_t *e){ lv_timer_delete(lv_event_get_user_data(e)); }
static void settings_reveal_row(int idx){
    setlist_scroll_remember();
    setlist_open(TABLE[idx].group);
    lv_obj_t *value = g_list_rows[idx];
    if(!value) return;
    lv_obj_t *row = lv_obj_get_parent(value);
    lv_obj_update_layout(g_setlist_root);
    lv_obj_scroll_to_view(row, LV_ANIM_OFF);
    lv_obj_set_style_outline_color(row, ui_current_accent(), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(row, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(row, 2, LV_STATE_FOCUSED);
    lv_obj_add_state(row, LV_STATE_FOCUSED);
    lv_timer_t *t = lv_timer_create(reveal_done, 1500, row);
    lv_obj_add_event_cb(row, reveal_deleted, LV_EVENT_DELETE, t);
}

#ifdef DISKOS_HOST
/* host tests: the table as the page builder reads it (see tests/host/test_settings_reorg.py) */
int settings_host_row(int i, const char **group, const char **section, const char **label, const char **key){
    if(i < 0 || i >= N_SETTINGS) return 0;
    *group = TABLE[i].group; *section = TABLE[i].section; *label = TABLE[i].label; *key = TABLE[i].cfg_key;
    return 1;
}
const char *settings_host_category(int i){ return i >= 0 && i < N_GROUPS ? GROUPS[i] : NULL; }
#endif
