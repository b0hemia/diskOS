/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "config.h"
#include "anim.h"
#include "musicdb.h"
#include "fwcaps.h"
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
    const char *group;
    const char *label;
    st_type_t   type;
    const char *cfg_key;
    int         min, max, step;            /* slider */
    const char *const *opts; int nopts;    /* cycler */
    const char *ro_val;                    /* readonly */
    void (*apply)(int v);                  /* optional apply hook / decode seam */
    int  def;
    const char *desc;                      /* detail-page description */
    const char *const *opt_descs;          /* per-option descriptions (cyclers) */
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
static void apply_debug_mode(int v){ (void)v; debug_open(); }           /* Settings -> System -> Debug Mode */
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
static void detail_refresh_async(void *u){ (void)u; setting_detail_refresh(); }
static void theme_reload_or_revert(const char *key){
    if(ui_theme_reload("settings") == 0) return;        /* not reached on success: the exec replaced us */
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
    theme_reload_or_revert("tz_idx");                  /* the UI re-reads the zone rules at start */
}
static void apply_import_m3u(int v){ (void)v;
    int n = mdb_import_m3u_sd("/tmp/sdcard");   /* every folder on the card (bounded walk), like stock V2.57 */
    char b[48];
    if(n <= 0) snprintf(b, sizeof b, "No new playlists found");
    else       snprintf(b, sizeof b, "Imported %d playlist%s", n, n==1 ? "" : "s");
    ui_toast(b);
}
static void apply_wifi(int v){ (void)v; wifi_open(); }   /* opens SCR_WIFI */
static void apply_bt(int v){ (void)v; bt_open(); }       /* opens SCR_BT */
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
    if(v < 0) v = 0; if(v > 40) v = 40;
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
    if(v < 1) v = 1; if(v > 40) v = 40;
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
 * Index here maps 1:1 to STYLE_PRESET, so the cycler value is exactly what ui_apply_eq() selects. */
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
static const char *const OPT_SAVERSTYLE[] = { "Cover", "Analog", "Minimal", "Digital", "Vinyl" };
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
static const char *const D_ARTCACHE[] = {
    "Album covers preload in the background so browsing stays smooth. Per-track art is decoded only as you play it (default).",
    "Covers preload, plus every track's art is pre-decoded while idle. Decoding can warm the player; it pauses automatically if it gets hot.",
    "Covers preload, plus every track's art is pre-decoded while idle AND charging. Decoding can warm the player; it pauses automatically if it gets hot.",
};

/* per-option descriptions for the cyclers (parallel to the OPT_ arrays) */
static const char *const D_MODE[] = {
    "Play through the list in order, then stop.",
    "Play tracks in a random order.",
    "Repeat the current track over and over.",
    "Loop the whole list when it ends.",
    "Play the current track once, then stop.",
};
static const char *const D_NPSTYLE[] = {
    "Album cover shown as a rounded square.",
    "Spinning vinyl record while a track plays.",
};

static const setting_t TABLE[] = {
    { "Playback", "Play Mode",   ST_CYCLER, "work_mode", 0,0,0, OPT_MODE, 5, NULL, apply_mode, 0,
      "How the player advances through tracks.", D_MODE },
    { "Playback", "Equalizer",   ST_CYCLER, "eq_preset", 0,0,0, OPT_EQ,  21, NULL, apply_eq,   0,
      "Tone preset sent to the player; audible effect is still being verified.", NULL },
    { "Playback", "Custom EQ",    ST_ACTION, NULL, 0,0,0, NULL,0, LV_SYMBOL_RIGHT, apply_eq_custom, 0,
      "Adjust the 10-band custom EQ. Applies live and saves on the device.", NULL },
    { "Playback", "DSD Output",  ST_READONLY, NULL, 0,0,0, NULL,0, "Auto", NULL, 0,
      "The player auto-selects the DSD mode; this control isn't user-adjustable yet.", NULL },
    { "Playback", "Resume Playback", ST_CYCLER, "memory_play", 0,0,0, OPT_MEMORY, 3, NULL, apply_memory, 0,
      "On power-on: Off = start fresh, Position = resume the exact spot, Song = reopen the last track.", NULL },
    { "Playback", "Artists",     ST_CYCLER, "artist_class", 0,0,0, OPT_ARTISTCLASS, 2, NULL, apply_artist_class, 0,
      "Group the Artists list by each song's artist, or by its album artist - the same choice as the stock player.", NULL },
    { "Playback", "Play Through Folders", ST_TOGGLE, "folder_jump", 0,1,1, NULL, 0, NULL, apply_folder_jump, 0,
      "When an album, artist, genre or folder finishes, keep playing the next one. Works with Sequential, Shuffle and Loop All.", NULL },
    /* Audio/DAC cluster - cyclers with min=-1 so they can read "System default" (unmanaged):
     * until you pick a value diskOS sends nothing + the player keeps its own setting. */
    { "Audio",    "Working Mode", ST_ACTION, NULL, 0,0,0, NULL,0, LV_SYMBOL_RIGHT, apply_workmode, 0,
      "Switch the audio source: local playback, USB DAC, USB Audio, Bluetooth receiving, or USB storage.", NULL },
    { "Audio",    "Gain",        ST_CYCLER, "audio_gain",   0,0,0, OPT_GAIN, 2, NULL, apply_gain, 0,
      "Headphone output gain. High drives demanding headphones louder.", NULL },
    { "Audio",    "DAC Filter",  ST_CYCLER, "audio_filter", 0,0,0, OPT_DFILTER, 6, NULL, apply_dac_filter, 1,
      "CS43131 digital filter roll-off. A subtle tone/transient tradeoff.", NULL },
    { "Audio",    "ReplayGain",  ST_CYCLER, "replay_gain",  0,0,0, OPT_REPLAYGAIN, 3, NULL, apply_replay_gain, 0,
      "Level the volume across tracks. Track uses each song's gain; Album keeps an album's relative dynamics.", NULL },
    { "Audio",    "DRE",         ST_CYCLER, "audio_dre",    0,0,0, OPT_DRE, 2, NULL, apply_dre, 1,
      "Dynamic Range Enhancement (CS43131) for quieter listening.", NULL },
    { "Audio",    "Gapless",     ST_CYCLER, "gapless",      0,0,0, OPT_DRE, 2, NULL, apply_gapless, 0,
      "Play tracks with no silent gap between them.", NULL },
    { "Audio",    "Max Volume",  ST_SLIDER, "max_vol",      10,120,5, NULL,0, NULL, apply_maxvol, 120,
      "Cap the maximum volume level (protects your ears / headphones).", NULL },
    { "Audio",    "Balance",     ST_SLIDER, "balance",      -10,10,1, NULL,0, NULL, apply_balance, 0,
      "Left/right channel balance. 0 = centred; negative = left, positive = right.", NULL },
#ifdef DISKOS_TEST_OUTPUTS   /* device-unverified: test builds only */
    { "Audio",    "SPDIF Output", ST_TOGGLE, "spdif",   0,1,1, NULL,0, NULL, apply_spdif, 0,
      "Send audio to the digital SPDIF output instead of the internal DAC. Playback pauses for a moment while it switches.", NULL },
#endif
    { "Display",  "Brightness",  ST_SLIDER, "brightness", 4,40,2, NULL,0, NULL, apply_brightness_row, 16,
      "Screen backlight level. While Outdoor Mode is on the screen stays at full brightness.", NULL },
    { "Display",  "Theme",       ST_CHOICE, "theme_preset", 0,0,0, theme_preset_names, THEME_PRESET_COUNT, NULL, apply_theme_preset, 0,
      "Colour theme for the whole interface. Each has dark and light screens (see Appearance).", NULL },
    { "Display",  "Appearance",  ST_CYCLER, "theme_variant", 0,0,0, OPT_APPEARANCE, 2, NULL, apply_appearance, 0,
      "Dark or light screens. Changing it redraws the screen in a moment; music keeps playing.", NULL },
    { "Display",  "Font Size",   ST_CYCLER, "font_size", 0,0,0, OPT_FONTSIZE, 3, NULL, apply_font_size, 1,
      "Size of list and body text: Small, Medium or Large. Titles and clocks keep their size. The screen redraws in a moment.", NULL },
    { "Display",  "Screen Rotation", ST_CYCLER, CTL_CFG_ROT, 0,0,0, OPT_ROTATION, CTL_ROT_N, NULL, apply_screen_rot, 0,
      "Turn the picture and touch clockwise: Normal, 90, 180 or 270 degrees. Stored by the player, as in the stock menu.", NULL },
    { "Display",  "Outdoor Mode", ST_TOGGLE, "outdoor",  0,1,1, NULL, 0, NULL, apply_outdoor, 0,
      "Light screens at full brightness, easier to read in sunlight. Uses more battery. Turning it off returns your own brightness.", NULL },
    { "Display",  "Track Numbers", ST_TOGGLE, "track_numbers", 0,1,1, NULL, 0, NULL, apply_track_numbers, 0,
      "Show each song's track number in album lists.", NULL },
    { "Display",  "Online Album Art", ST_TOGGLE, "online_art", 0,1,1, NULL, 0, NULL, apply_online_art, 0,
      "Songs with no cover of their own get one from the internet (Wi-Fi). Each album is looked up once; playback is never touched.", NULL },
    { "Display",  "Online Lyrics", ST_TOGGLE, "online_lyrics", 0,1,1, NULL, 0, NULL, NULL, 1,
      "Songs with no lyrics of their own (no .lrc file, none in the song's tags) are looked up on lrclib.net (Wi-Fi). Off stops diskOS's own lookup; lyrics from the SD card or the song's tags still show.", NULL },
    { "Display",  "Quick Settings", ST_ACTION, NULL, 0,0,0, NULL,0, LV_SYMBOL_RIGHT, apply_qsconfig, 0,
      "Choose which tiles appear in the pull-down Quick Settings drawer.", NULL },
    { "Display",  "Now Playing", ST_CYCLER, "np_style",   0,0,0, OPT_NPSTYLE, 2, NULL, apply_np_style, 0,
      "Album art style on the Now Playing screen.", D_NPSTYLE },
    { "Display",  "Album View",  ST_CYCLER, "album_view", 0,0,0, OPT_ALBUMVIEW, 2, NULL, NULL, 0,
      "How the Albums list looks: a text list, or a cover flow you flick through.", NULL },
    { "Display",  "Accent Colour", ST_ACTION, NULL, 0,0,0, NULL,0, LV_SYMBOL_RIGHT, apply_open_colorpick, 0,
      "Now Playing accent: derived from album art, or any fixed colour you pick.", NULL },
    { "Display",  "Album Art Cache", ST_CYCLER, "artcache", 0,0,0, OPT_ARTCACHE, 3, NULL, apply_artcache, 0,
      "Pre-decode album art so covers load instantly. Background decoding can warm the player; it throttles on heat.", D_ARTCACHE },
    { "Display",  "Screensaver", ST_CYCLER, "saver_idx",  0,0,0, OPT_POWER, 5, NULL, NULL, 2,
      "Idle time before the clock screensaver appears and the screen dims.", NULL },
    { "Display",  "Saver Style", ST_CYCLER, "saver_style", 0,0,0, OPT_SAVERSTYLE, 5, NULL, NULL, 0,
      "Screensaver look: Cover art, Analog clock, Minimal, Digital, or spinning Vinyl.", NULL },
    { "Display",  "Screen Off",  ST_CYCLER, "screenoff_idx", 0,0,0, OPT_POWER, 5, NULL, NULL, 3,
      "How long after the screensaver the screen turns fully off.", NULL },
    { "Display",  "24-Hour Time", ST_TOGGLE, "time_24h",  0,1,1, NULL, 0, NULL, apply_time, 1,
      "Use a 24-hour clock instead of AM/PM.", NULL },
    { "Display",  "Animations",  ST_TOGGLE, "anim",      0,1,1, NULL, 0, NULL, apply_anim, 1,
      "Slide animations between screens.", NULL },
    { "Display",  "Weather on Home", ST_TOGGLE, "weather_on", 0,1,1, NULL, 0, NULL, apply_weather, 1,
      "Show the weather glance on the home screen and screensaver (tap it to open the full forecast). Off skips the background fetch to save battery.", NULL },
    { "Display",  "Back-swipe",  ST_SLIDER, "swipe_thresh", 30,120,5, NULL,0, NULL, apply_swipe, 60,
      "Swipe distance needed to go back. Lower is more sensitive.", NULL },
    { "Network",  "Wi-Fi",       ST_ACTION, NULL, 0,0,0, NULL,0, LV_SYMBOL_RIGHT, apply_wifi, 0,
      "Scan for and connect to Wi-Fi networks.", NULL },
    { "Network",  "Bluetooth", ST_ACTION, NULL, 0,0,0, NULL,0, LV_SYMBOL_RIGHT, apply_bt, 0,
      "Pair Bluetooth devices. Audio routes to connected headphones or speakers (SBC, beta).", NULL },
    { "Network",  "Bluetooth Codec", ST_CYCLER, "bt_codec", 0,0,0, BT_CODEC_LABEL, BT_CODEC_N, NULL, NULL, 0,
      "Codec for Bluetooth headphones, as in the stock player. Applies the next time the headphones connect. If they lack the codec, SBC is used. AAC and LDAC are heavier for this player and may stutter.", NULL },
    { "System",   "Language",    ST_CHOICE, "language", 0,0,0, i18n_lang_names, LANG_COUNT, NULL, apply_language, LANG_EN,
      "Interface language. Song, artist and album names are always shown as they are tagged.", NULL },
    { "System",   "Sleep Timer", ST_CYCLER, "sleep_idx", 0,0,0, OPT_SLEEP, 6, NULL, apply_sleep, 0,
      "Pause playback, or shut the device down (see When Sleep Ends), after this long. Resets on restart.", NULL },
    { "System",   "When Sleep Ends", ST_CYCLER, "sleep_action", 0,0,0, OPT_SLEEP_ACT, 2, NULL, NULL, POWER_SLEEP_PAUSE,
      "What the Sleep Timer does. Shut down closes the card safely first, then powers the device off.", NULL },
    { "System",   "Idle Power-off", ST_CYCLER, "idle_off_idx", 0,0,0, OPT_IDLE_OFF, POWER_IDLE_N, NULL, NULL, 0,
      "Power the device off after this long with nothing playing and no touch or key press. Closes the card safely first.", NULL },
    { "System",   "Charging Limit", ST_TOGGLE, "charge_protect", 0,1,1, NULL, 0, NULL, apply_charge_protect, 0,
      "The player's Charging optimization: stops charging at about 80% to slow battery wear.", NULL },
    { "System",   "Vol Keys: Press", ST_CYCLER, "key_single", 0,0,0, OPT_VOLKEY, 2, NULL, apply_key_single, CTL_ACT_VOLUME,
      "What the volume keys do on a single press: adjust the volume or switch track. Stored by the player, as in the stock menu.", NULL },
    { "System",   "Vol Keys: Double Press", ST_CYCLER, "key_double", 0,0,0, OPT_VOLKEY, 2, NULL, apply_key_double, CTL_ACT_VOLUME,
      "What the volume keys do on a double press: adjust the volume or switch track.", NULL },
    { "System",   "Vol Keys: Long Press", ST_CYCLER, "key_long", 0,0,0, OPT_VOLKEY, 2, NULL, apply_key_long, CTL_ACT_VOLUME,
      "What the volume keys do when held: adjust the volume or switch track.", NULL },
    { "System",   "Old tags", ST_CYCLER, "old_tags", 0,0,0, OPT_OLDTAGS, 3, NULL, apply_old_tags, OLD_TAGS_AUTO,
      "Legacy text: guess, Latin-1 or CP1251. Rescan Library to apply.", NULL },
    { "System",   "Rescan Library", ST_ACTION, NULL, 0,0,0, NULL,0, "Scan", apply_rescan, 0,
      "Re-scan the SD card for new or removed music.", NULL },
    { "System",   "Import Playlists", ST_ACTION, NULL, 0,0,0, NULL,0, "Import", apply_import_m3u, 0,
      "Import .m3u / .m3u8 playlists found on the SD card.", NULL },
    { "System",   "Default UI",  ST_CYCLER, "boot_default", 0,0,0, OPT_BOOTDEF, 2, NULL, apply_boot_default, 0,
      "Which UI boots by default. To boot the other one, hold Vol-Up from power-on until it appears.", NULL },
    { "System",   "Automatic Time", ST_TOGGLE, "auto_time", 0,1,1, NULL, 0, NULL, apply_auto_time, 1,
      "Set the time and time zone from the internet whenever Wi-Fi connects. A change applies after the next restart.", NULL },
    { "System",   "Set Date & Time", ST_ACTION, NULL, 0,0,0, NULL,0, LV_SYMBOL_RIGHT, apply_open_datetime, 0,
      "Set the clock by hand. With Automatic Time on, Wi-Fi may correct it later.", NULL },
    { "System",   "Time Zone",   ST_CHOICE, "tz_idx", 0,0,0, TZ_LABEL, TZ_COUNT, NULL, apply_timezone, 0,
      "Automatic follows your location when Wi-Fi connects (with Automatic Time on). Pick a zone to set it yourself.", NULL },
    { "System",   "Restart",     ST_ACTION, NULL, 0,0,0, NULL,0, "Restart", apply_restart, 0,
      "Restart the device. Boots your default UI; hold Vol-Up for the other one.", NULL },
    { "System",   "Device Info", ST_ACTION, NULL, 0,0,0, NULL,0, LV_SYMBOL_RIGHT, system_about_open, 0,
      "Model, stock firmware, diskOS build, MAC addresses, storage and battery.", NULL },
    { "System",   "Update from SD Card", ST_ACTION, NULL, 0,0,0, NULL,0, LV_SYMBOL_RIGHT, system_update_sd_open, 0,
      "Look for a file on the SD card named like a stock update and show how it is used. Information only: diskOS never runs it.", NULL },
    { "System",   "Allow diskOS Updates", ST_TOGGLE, "ota_allow", 0,1,1, NULL, 0, NULL, system_ota_allow_changed, 1,
      "diskOS only updates when you tap Update diskOS, and only with signed releases.", NULL },
    { "System",   "Update diskOS", ST_ACTION, NULL, 0,0,0, NULL,0, "Check", system_check_update_open, 0,
      "Look up the newest diskOS release on GitHub (needs Wi-Fi). If it carries a signed update, download and verify it, then restart to finish. Nothing changes until you confirm.", NULL },
    { "System",   "Reset diskOS Settings", ST_ACTION, NULL, 0,0,0, NULL,0, "Reset", system_reset_open, 0,
      "Put diskOS's look and behaviour settings back to defaults. Music, Wi-Fi, Bluetooth, Last.fm, EQ and audio settings are kept.", NULL },
    { "System",   "Debug Mode",  ST_ACTION, NULL, 0,0,0, NULL,0, LV_SYMBOL_RIGHT, apply_debug_mode, 0,
      "Enable temporary SSH over Wi-Fi (fresh random password) for debugging. Off by default.", NULL },
    { "System",   "Temperature", ST_READONLY, NULL, 0,0,0, NULL,0, "@temp", NULL, 0,
      "Battery/board temperature from the fuel gauge (this SoC exposes no core sensor).", NULL },
    { "System",   "About",       ST_READONLY, NULL, 0,0,0, NULL,0, "@about", NULL, 0,
      "diskOS - a custom music player UI. The code in brackets identifies this exact build.", NULL },
};
#define N_SETTINGS ((int)(sizeof(TABLE)/sizeof(TABLE[0])))

static int  get_val(const setting_t *s){ return s->cfg_key ? cfg_get_int(s->cfg_key, s->def) : 0; }
static void set_val(const setting_t *s, int v){
    /* EQ persists itself only on a successful send (apply_eq -> ui_eq_select), so a failed send never
     * leaves a stale eq_preset; every other setting persists up front here. */
    g_set_prev = get_val(s);
    if(s->cfg_key && strcmp(s->cfg_key, "eq_preset")) cfg_set_int(s->cfg_key, v);
    if(s->apply) s->apply(v);
}
/* battery/board temp from the fuel gauge; power-supply ABI = tenths of a degree C */
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
void settings_diskos_id(char *buf, int n){ read_about(buf, n); }   /* system.c About */
void settings_restart_prompt(const char *note){ g_restart_note = note; apply_restart(0); }
static void read_batt_temp(char *buf, int n){
    FILE *f = fopen("/sys/class/power_supply/cw221X-bat/temp", "r");
    int t;
    if(f && fscanf(f, "%d", &t) == 1){ fclose(f);
        unsigned at = t<0 ? 0u-(unsigned)t : (unsigned)t;   /* unsigned negate: INT_MIN-safe; sign explicit */
        snprintf(buf, n, "%s%u.%u\xC2\xB0""C", t<0?"-":"", at/10, at%10);   /* 305->"30.5 degrees C", -5->"-0.5 degrees C" */
    } else { if(f) fclose(f); snprintf(buf, n, "--"); }
}
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
                       || ((!strcmp(s->cfg_key, "idle_off_idx") || !strcmp(s->cfg_key, "charge_protect")
                            || !strcmp(s->cfg_key, "sleep_action")) && !power_supported())
                       || ((!strcmp(s->cfg_key, CTL_CFG_ROT) || !strcmp(s->cfg_key, "key_single")
                            || !strcmp(s->cfg_key, "key_double") || !strcmp(s->cfg_key, "key_long")) && !ctl_supported()));
}
static const char *val_text(const setting_t *s, char *buf, int n){
    if(setting_unavailable(s)) return tr(s->cfg_key && !strcmp(s->cfg_key, "ota_allow") ? "Not supported on this install" : "Not on this firmware");
    /* Outdoor Mode overrides these two while it is on: show what is really in effect, not the saved choice (which
     * stays untouched and comes back when Outdoor Mode is turned off) */
    if(theme_outdoor() && s->cfg_key && !strcmp(s->cfg_key, "theme_variant")) return "Light (Outdoor)";
    if(theme_outdoor() && s->cfg_key && !strcmp(s->cfg_key, "brightness")) return "Full (Outdoor)";
    int v = get_val(s);
    switch(s->type){
        case ST_TOGGLE:   return tr(v?"On":"Off");
        case ST_SLIDER:   fmt_slider(s, v, buf, n); break;
        case ST_CYCLER: case ST_CHOICE: return (v>=0&&v<s->nopts)?tr(s->opts[v]):"?";
        case ST_READONLY:
            if(s->ro_val && !strcmp(s->ro_val, "@temp")) read_batt_temp(buf, n);
            else if(s->ro_val && !strcmp(s->ro_val, "@about")) read_about(buf, n);
            else return tr(s->ro_val?s->ro_val:"");
            break;
        case ST_ACTION:   return tr((s->apply==apply_rescan && scanner_active()) ? "Stop" : (s->ro_val?s->ro_val:""));
    }
    return buf;
}

/* ---- shared widgets ----------------------------------------------------- */

static lv_obj_t *g_list_rows[N_SETTINGS];   /* value labels, to refresh in place */

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
        lv_label_set_text(rr->lbl, tr("Rescan Library"));
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
static const char *g_active_group;          /* which category SCR_SETLIST is currently showing */
/* Category order for the top-level Settings screen (must match the group strings used in TABLE). */
static const char *const GROUPS[] = { "Playback", "Audio", "Display", "Network", "System" };
#define N_GROUPS ((int)(sizeof(GROUPS)/sizeof(GROUPS[0])))



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
static void detail_toggle_cb(lv_event_t *e){
    lv_obj_t *sw = lv_event_get_target(e);
    set_val(g_active, lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0);
}
static lv_obj_t *make_list(lv_obj_t *root);
static lv_obj_t *setting_card(lv_obj_t *list);
static void detail_choice_async(void *u){ (void)u; setting_detail_refresh(); }
static void detail_choice_cb(lv_event_t *e){
    int i = (int)(intptr_t)lv_event_get_user_data(e);
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
        lv_obj_set_ext_click_area(sl, 14);
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
    } else if(s->type == ST_TOGGLE){
        lv_obj_t *sw = lv_switch_create(g_detail_root);
        lv_obj_align(sw, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(sw, ui_current_accent(), LV_PART_INDICATOR | LV_STATE_CHECKED);
        if(v) lv_obj_add_state(sw, LV_STATE_CHECKED);
        ui_on(sw, detail_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL, "settings.detail_toggle.value", UI_CORE);
    } else if(s->type == ST_CHOICE){
        /* every option as a row; the current one ticked. Themes also show three colour dots (background, surface,
         * accent) in the current Dark/Light variant, so each can be judged before picking. */
        lv_obj_t *list = make_list(g_detail_root);
        if(s->opts == i18n_lang_names) lv_obj_set_height(list, 215);
        int is_theme = !strcmp(s->cfg_key, "theme_preset");
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
            if(cur){
                lv_obj_t *ck = lv_label_create(row);
                lv_label_set_text(ck, LV_SYMBOL_OK);
                lv_obj_align(ck, LV_ALIGN_RIGHT_MID, -16, 0);
                lv_obj_set_style_text_color(ck, ui_current_accent(), 0);
            }
            theme_list_row(row);
        }
    } else if(s->type == ST_CYCLER){
        /* < value > stepper */
        lv_obj_t *vl = lv_label_create(g_detail_root);
        char b[24]; lv_label_set_text(vl, val_text(s,b,sizeof b));
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
        lv_obj_set_size(bt, 240, 36);
        lv_obj_align(bt, LV_ALIGN_CENTER, 0, 28);
        old_tags_rescan_watch(bt);
        lv_obj_t *l = lv_label_create(bt);
        lv_label_set_text(l, tr("Rescan Library"));
        lv_obj_set_style_text_font(l, TF(UI_14), 0);
        lv_obj_center(l);
        ui_on(bt, old_tags_rescan_cb, LV_EVENT_CLICKED, NULL, "settings.old_tags_rescan", UI_CORE);
    }

    /* description at the bottom - per-option for cyclers (refreshes on change) */
    const char *desc = s->desc;
    if(s->type==ST_CYCLER && s->opt_descs && v>=0 && v<s->nopts) desc = s->opt_descs[v];
    if(s->type == ST_CHOICE) desc = (s->opts == i18n_lang_names) ? "Screen redraws after selection." : NULL;
    if(desc && desc[0]){
        lv_obj_t *d = lv_label_create(g_detail_root);
        lv_label_set_text(d, tr(desc));
        lv_label_set_long_mode(d, s->type == ST_CHOICE ? LV_LABEL_LONG_DOT : LV_LABEL_LONG_WRAP);
        lv_obj_set_width(d, 240);
        if(s->type == ST_CHOICE) lv_obj_set_height(d, 22);
        lv_obj_align(d, LV_ALIGN_BOTTOM_MID, 0, -40);
        lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(d, TF(UI_14), 0);
        lv_obj_set_style_text_color(d, TC(TEXT_MUTED), 0);
    }
}

void setting_detail_create(lv_obj_t *root){ g_detail_root = root; }

/* open a specific setting's detail page directly (verification / deep-link) */
void settings_open_detail(int idx){
    if(idx<0 || idx>=N_SETTINGS) return;
    g_active = &TABLE[idx];
    screen_show(SCR_SETTING_DETAIL);
    setting_detail_refresh();
}

/* open a setting's detail page by its cfg key (robust to TABLE reordering). No-op if not found. */
void settings_open_key(const char *key){
    if(!key) return;
    for(int i=0;i<N_SETTINGS;i++)
        if(TABLE[i].cfg_key && !strcmp(TABLE[i].cfg_key, key)){
            if(setting_unavailable(&TABLE[i])){ ui_toast("Not supported on this firmware yet"); return; }
            settings_open_detail(i); return;
        }
}

/* ---- master list -------------------------------------------------------- */
static void list_back_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) screen_back(); }

static void row_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    const setting_t *s = &TABLE[idx];
    if(s->type == ST_TOGGLE){
        if(setting_unavailable(s)){ ui_toast(tr("Not supported on this install")); return; }   /* only Allow diskOS Updates is a toggle that can be unavailable */
        int nv = !get_val(s); set_val(s, nv);
        char b[16];
        if(g_list_rows[idx]) lv_label_set_text(g_list_rows[idx], val_text(s,b,sizeof b));
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

/* ---- top-level category list (SCR_SETTINGS): Playback / Audio / Display / Network / System ---- */
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
        lv_obj_set_style_text_font(lbl, TF(UI_16), 0);
        lv_obj_set_style_text_color(lbl, TC(TEXT_PRIMARY), 0);

        lv_obj_t *ch = lv_label_create(row);   /* drill-down chevron */
        lv_obj_clear_flag(ch, LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_text(ch, LV_SYMBOL_RIGHT);
        lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -14, 0);
        lv_obj_set_style_text_font(ch, TF(UI_16), 0);
        lv_obj_set_style_text_color(ch, TC(TEXT_MUTED), 0);
        if(theme_trait(THEME_TRAIT_FLAT_LISTS)) lv_obj_set_style_text_font(lbl, TF(UI_20), 0);   /* big mono rows */
        theme_list_row(row);
    }
}

/* Category list is static; nothing to re-sync when SCR_SETTINGS is (re)shown. */
void settings_refresh_list(void){ }
void settings_row_values_refresh(void){
    char b[40];
    for(int i=0;i<N_SETTINGS;i++) if(g_list_rows[i] && lv_obj_is_valid(g_list_rows[i])) lv_label_set_text(g_list_rows[i], val_text(&TABLE[i],b,sizeof b));
}

/* ---- one category's rows (SCR_SETLIST), rebuilt on every entry so values are always live ---- */
void setlist_create(lv_obj_t *root){ g_setlist_root = root; }

void setlist_refresh(void){
    if(!g_setlist_root || !g_active_group) return;
    lv_obj_clean(g_setlist_root);
    lv_obj_set_style_bg_color(g_setlist_root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(g_setlist_root, LV_OPA_COVER, 0);
    ui_header_cb(g_setlist_root, g_active_group, list_back_cb);   /* title = category; back pops to categories */

    lv_obj_t *list = make_list(g_setlist_root);
    for(int i=0;i<N_SETTINGS;i++){
        const setting_t *s = &TABLE[i];
        if(strcmp(s->group, g_active_group)) continue;   /* only rows in this category */

        lv_obj_t *row = setting_card(list);
        ui_on(row, row_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)i, "settings.row", UI_CORE);

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
        if(s->type == ST_ACTION){
            /* the action word gets the width it needs (46..96 px, measured in this theme's face: "Import" was cut to
             * "Imp..." in a fixed 46 px box), right-aligned at x 250; the label takes the rest */
            lv_point_t sz;
            lv_text_get_size(&sz, value, TF(UI_14), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            int32_t vw = sz.x + 4; if(vw < 46) vw = 46; if(vw > 96) vw = 96;
            lv_obj_set_pos(lbl, 18, 15);
            lv_obj_set_size(lbl, 250 - vw - 8 - 18, 20);
            lv_obj_set_pos(vl, 250 - vw, 17);
            lv_obj_set_size(vl, vw, 18);
            lv_obj_set_style_text_align(vl, LV_TEXT_ALIGN_RIGHT, 0);
        }
        g_list_rows[i] = vl;
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
}
