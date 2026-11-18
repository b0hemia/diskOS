/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* System rows S21-S27. See system.h for the stock facts. The top half is pure/OS-only (host-tested with
 * SYSTEM_CORE_ONLY); the bottom half is the LVGL flow. */
#include "system.h"
#include "config.h"
#include "ota_policy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>

const sys_paths_t SYS_DEV_PATHS = {
    "/etc/product_version/version.in", "/sys/class/net/wlan0/address", "/sys/class/bluetooth/hci0/address",
    "/sys/class/power_supply/cw221X-bat", "/tmp/sdcard", "/usr/data", "/proc/mounts"
};

/* ---------------------------------------------------------------- small helpers */
static int read_small(const char *path, char *buf, size_t n){   /* whole small text file, NUL-terminated, trimmed; 0 ok */
    if(!buf || n < 2) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if(fd < 0) return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if(r < 0) return -1;
    buf[r] = 0;
    while(r > 0 && (buf[r-1] == '\n' || buf[r-1] == '\r' || buf[r-1] == ' ')) buf[--r] = 0;
    return 0;
}
static int ends_with_ci(const char *s, const char *suffix){
    size_t a = strlen(s), b = strlen(suffix);
    if(a < b) return 0;
    for(size_t i = 0; i < b; i++) if(tolower((unsigned char)s[a-b+i]) != tolower((unsigned char)suffix[i])) return 0;
    return 1;
}

int sys_kv(const char *text, const char *key, char *out, size_t n){
    size_t kl = strlen(key);
    for(const char *l = text; l && *l; ){
        const char *nl = strchr(l, '\n');
        size_t len = nl ? (size_t)(nl - l) : strlen(l);
        if(len > kl && !strncmp(l, key, kl) && l[kl] == '='){
            size_t vl = len - kl - 1;
            while(vl && (l[kl+1+vl-1] == '\r' || l[kl+1+vl-1] == ' ')) vl--;
            if(vl >= n) vl = n - 1;
            memcpy(out, l + kl + 1, vl); out[vl] = 0;
            return 0;
        }
        l = nl ? nl + 1 : NULL;
    }
    return -1;
}
void sys_fw_label(int ver, char *buf, size_t n){
    if(ver <= 0) snprintf(buf, n, "--");
    else snprintf(buf, n, "V%d.%02d", ver / 100, ver % 100);
}
void sys_fmt_bytes(unsigned long long b, char *buf, size_t n){
    if(b >= (1ULL << 30)) snprintf(buf, n, "%.1f GB", (double)b / (double)(1ULL << 30));
    else if(b >= (1ULL << 20)) snprintf(buf, n, "%.0f MB", (double)b / (double)(1ULL << 20));
    else snprintf(buf, n, "%llu KB", b >> 10);
}

/* ---------------------------------------------------------------- About */
int sys_read_battery(const sys_paths_t *pp){
    const sys_paths_t *p = pp ? pp : &SYS_DEV_PATHS;
    char path[200], b[16];
    snprintf(path, sizeof path, "%s/capacity", p->batt_dir);
    if(read_small(path, b, sizeof b) != 0 || !b[0]) return -1;
    char *e; long v = strtol(b, &e, 10);
    if(*e || v < 0 || v > 100) return -1;
    return (int)v;
}
unsigned long long sys_sd_free(const sys_paths_t *pp){
    const sys_paths_t *p = pp ? pp : &SYS_DEV_PATHS;
    struct statvfs sv;
    if(statvfs(p->sd_root, &sv) != 0) return 0;
    return (unsigned long long)sv.f_bavail * (unsigned long long)sv.f_frsize;
}
static void add_row(sys_about_row_t *rows, int *n, int max, const char *label, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
#include <stdarg.h>
static void add_row(sys_about_row_t *rows, int *n, int max, const char *label, const char *fmt, ...){
    if(*n >= max) return;
    snprintf(rows[*n].label, sizeof rows[*n].label, "%s", label);
    va_list ap; va_start(ap, fmt);
    vsnprintf(rows[*n].value, sizeof rows[*n].value, fmt, ap);
    va_end(ap);
    (*n)++;
}
int sys_is_mounted(const char *mounts, const char *dir){   /* 1 = dir is a mount point listed in mounts (/proc/mounts) */
    FILE *f = fopen(mounts, "r");
    if(!f) return 0;
    char line[512]; int found = 0;
    while(!found && fgets(line, sizeof line, f)){
        char dev[256], mp[256];
        if(sscanf(line, "%255s %255s", dev, mp) == 2 && !strcmp(mp, dir)) found = 1;
    }
    fclose(f);
    return found;
}
/* The fuel gauge's temperature (tenths of a degree C in the power-supply ABI) as "30.5 C" with a degree sign; 0 ok, -1 unreadable. */
static int battery_temp_text(const sys_paths_t *p, char *buf, size_t n){
    char path[200], raw[16];
    snprintf(path, sizeof path, "%s/temp", p->batt_dir);
    if(read_small(path, raw, sizeof raw) != 0 || !raw[0]) return -1;
    char *e; long t = strtol(raw, &e, 10);
    if(*e || t < -2000 || t > 2000) return -1;
    unsigned long at = t < 0 ? (unsigned long)(-t) : (unsigned long)t;
    snprintf(buf, n, "%s%lu.%lu\xC2\xB0""C", t < 0 ? "-" : "", at / 10, at % 10);   /* 305 -> "30.5 degrees C", -5 -> "-0.5 degrees C" */
    return 0;
}
/* One line about the battery as the fuel gauge reports it: percent, health, cycle count and (with_temp) temperature.
 * A reading that is not there is left out; no percent reads "--". Used by Device Info and by Battery & Power. */
void sys_battery_text(const sys_paths_t *pp, int with_temp, char *buf, size_t n){
    const sys_paths_t *p = pp ? pp : &SYS_DEV_PATHS;
    char path[200], hl[24] = "", cy[16] = "", tp[24] = "";
    int pct = sys_read_battery(p);
    snprintf(path, sizeof path, "%s/health", p->batt_dir);   if(read_small(path, hl, sizeof hl) != 0) hl[0] = 0;
    snprintf(path, sizeof path, "%s/cycle_count", p->batt_dir); if(read_small(path, cy, sizeof cy) != 0) cy[0] = 0;
    if(pct >= 0) snprintf(buf, n, "%d%%", pct); else snprintf(buf, n, "--");
    if(hl[0]){ size_t l = strlen(buf); snprintf(buf + l, n - l, ", %s", hl); }
    if(cy[0]){ size_t l = strlen(buf); snprintf(buf + l, n - l, ", %s cycles", cy); }
    if(with_temp && battery_temp_text(p, tp, sizeof tp) == 0){ size_t l = strlen(buf); snprintf(buf + l, n - l, ", %s", tp); }
}
int sys_about_collect(const sys_paths_t *pp, const char *diskos_id, sys_about_row_t *rows, int max){
    const sys_paths_t *p = pp ? pp : &SYS_DEV_PATHS;
    int n = 0;
    char vi[512] = "", prod[48] = "", mainv[16] = "", recv[16] = "", mac[32], b[64];
    read_small(p->version_in, vi, sizeof vi);
    if(sys_kv(vi, "PRODUCT", prod, sizeof prod) != 0) snprintf(prod, sizeof prod, "--");
    for(char *c = prod; *c; c++) if(*c == '_') *c = ' ';
    add_row(rows, &n, max, "Model", "%s", prod);
    int mv = sys_kv(vi, "MAIN_OS_VER", mainv, sizeof mainv) == 0 ? atoi(mainv) : 0;
    int rv = sys_kv(vi, "RECOVERY_OS_VER", recv, sizeof recv) == 0 ? atoi(recv) : 0;
    char fw[16]; sys_fw_label(mv, fw, sizeof fw);
    if(rv > 0) add_row(rows, &n, max, "Firmware", "%s (recovery %d)", fw, rv);
    else       add_row(rows, &n, max, "Firmware", "%s", fw);
    add_row(rows, &n, max, "diskOS", "%s", diskos_id && *diskos_id ? diskos_id : "--");
    add_row(rows, &n, max, "Wi-Fi MAC", "%s", read_small(p->wlan_mac, mac, sizeof mac) == 0 && mac[0] ? mac : "--");
    add_row(rows, &n, max, "Bluetooth MAC", "%s", read_small(p->bt_mac, mac, sizeof mac) == 0 && mac[0] ? mac : "unavailable");
    struct statvfs sv;
    if(sys_is_mounted(p->mounts, p->sd_root) && statvfs(p->sd_root, &sv) == 0 && sv.f_blocks){
        unsigned long long tot = (unsigned long long)sv.f_blocks * sv.f_frsize, fr = (unsigned long long)sv.f_bavail * sv.f_frsize;
        char a[16], c[16]; sys_fmt_bytes(tot - (unsigned long long)sv.f_bfree * sv.f_frsize, a, sizeof a); sys_fmt_bytes(fr, c, sizeof c);
        char t[16]; sys_fmt_bytes(tot, t, sizeof t);
        add_row(rows, &n, max, "SD card", "%s used, %s free of %s", a, c, t);
    } else add_row(rows, &n, max, "SD card", "unavailable");
    if(statvfs(p->data_root, &sv) == 0){
        char c[16]; sys_fmt_bytes((unsigned long long)sv.f_bavail * sv.f_frsize, c, sizeof c);
        add_row(rows, &n, max, "Internal", "%s free", c);
    }
    sys_battery_text(p, 0, b, sizeof b);
    add_row(rows, &n, max, "Battery", "%s", b);
    return n;
}

/* ---------------------------------------------------------------- local update: detection */
int sys_update_name_ok(const char *name){
    if(!name || !*name || strlen(name) >= sizeof(((sys_update_t *)0)->name)) return 0;
    if(name[0] == '.' && name[1] == '_') return 0;             /* macOS resource forks (stock skips "._*") */
    if(!strstr(name, SYS_UPDATE_PRODUCT)) return 0;            /* stock: strstr(name, product) */
    return ends_with_ci(name, ".zip");
}
int sys_update_version(const char *name){
    if(!name) return -1;
    for(const char *v = strchr(name, 'v'); v; v = strchr(v + 1, 'v')){
        const char *d = v + 1; int n = 0;
        if(!isdigit((unsigned char)*d)) continue;
        while(isdigit((unsigned char)*d)){ if(n > 100000) return -1; n = n * 10 + (*d - '0'); d++; }
        if(ends_with_ci(d, ".zip") && (d[4] == 0)) return n;   /* stock: sscanf(strchr(name,'v'), "v%d.zip") */
    }
    return -1;
}
int sys_find_update(const char *dir, sys_update_t *out){
    DIR *d = opendir(dir);
    if(!d) return 0;
    int found = 0;
    struct dirent *e;
    while((e = readdir(d))){
        if(!sys_update_name_ok(e->d_name)) continue;
        int ver = sys_update_version(e->d_name);
        if(ver <= 0) continue;
        sys_update_t c; memset(&c, 0, sizeof c);
        if(snprintf(c.path, sizeof c.path, "%s/%s", dir, e->d_name) >= (int)sizeof c.path) continue;
        struct stat st;
        if(stat(c.path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) continue;
        snprintf(c.name, sizeof c.name, "%.*s", (int)sizeof c.name - 1, e->d_name);
        c.version = ver; c.size = (long long)st.st_size;
        if(!found || c.version > out->version || (c.version == out->version && strcmp(c.name, out->name) > 0)){ *out = c; found = 1; }
    }
    closedir(d);
    return found;
}
/* Battery and free space are shown as information only; they never gate anything (diskOS starts no update). */

/* ---------------------------------------------------------------- check for diskOS updates */
static int parse_ver(const char *s, int v[3]){
    v[0] = v[1] = v[2] = 0;
    if(*s == 'v' || *s == 'V') s++;
    if(!isdigit((unsigned char)*s)) return -1;
    for(int i = 0; i < 3; i++){
        long x = 0; int digits = 0;
        while(isdigit((unsigned char)*s)){ x = x * 10 + (*s - '0'); if(x > 100000) return -1; s++; digits++; }
        if(!digits) return -1;
        v[i] = (int)x;
        if(*s == '.') { s++; continue; }
        break;
    }
    return 0;
}
int sys_ver_cmp(const char *a, const char *b){
    int x[3], y[3];
    if(!a || !b || parse_ver(a, x) != 0 || parse_ver(b, y) != 0) return -2;
    for(int i = 0; i < 3; i++) if(x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}
int sys_parse_tag(const char *json, char *out, size_t n){
    if(!json || !out || n < 2) return -1;
    const char *k = strstr(json, "\"tag_name\"");
    if(!k) return -1;
    k += 10;
    while(*k == ' ' || *k == '\t' || *k == '\n') k++;
    if(*k != ':') return -1;
    k++;
    while(*k == ' ' || *k == '\t' || *k == '\n') k++;
    if(*k != '"') return -1;
    k++;
    size_t i = 0;
    while(*k && *k != '"'){
        if(!(isalnum((unsigned char)*k) || *k == '.' || *k == '-' || *k == '_') || i + 1 >= n) return -1;
        out[i++] = *k++;
    }
    if(*k != '"' || i == 0) return -1;
    out[i] = 0;
    return 0;
}
#define FETCH_CAP (96 * 1024)
int sys_fetch_latest(char *tag, size_t n){
    int pfd[2];
    if(pipe(pfd) != 0) return -1;
    pid_t p = fork();
    if(p < 0){ close(pfd[0]); close(pfd[1]); return -1; }
    if(p == 0){
        setsid(); dup2(pfd[1], 1);
        int nul = open("/dev/null", O_WRONLY); if(nul >= 0) dup2(nul, 2);
        close(pfd[0]); close(pfd[1]);
        execlp("wget", "wget", "-qO-", "-T", "12", SYS_GITHUB_LATEST, (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    char *buf = malloc(FETCH_CAP + 1);
    size_t got = 0;
    while(buf && got < FETCH_CAP){
        ssize_t r = read(pfd[0], buf + got, FETCH_CAP - got);
        if(r < 0 && errno == EINTR) continue;
        if(r <= 0) break;
        got += (size_t)r;
    }
    int capped = (got >= FETCH_CAP);
    close(pfd[0]);
    if(capped) kill(p, SIGKILL);         /* an oversized reply is not the releases JSON: stop wget, fail below */
    int st = 0, waited = 0;
    while(!waited){ if(waitpid(p, &st, 0) >= 0) waited = 1; else if(errno != EINTR) break; }
    int rc = -1;
    if(buf && !capped && waited && WIFEXITED(st) && WEXITSTATUS(st) == 0){ buf[got] = 0; rc = sys_parse_tag(buf, tag, n); }   /* only a clean wget exit is parsed */
    free(buf);
    return rc;
}

/* ---------------------------------------------------------------- reset diskOS settings */
/* Only the diskOS UI's own look-and-behaviour keys. Deliberately NOT here: keys that mirror the stock player's state
 * (audio_*, eq_*, eqN_*, work_mode, volume, max_vol, balance, replay_gain, gapless, folder_jump, artist_class,
 * charge_protect, auto_time, tz_idx, memory_play, language), radio intents (wifi_on, bt_on), Last.fm accounts
 * (lastfm_*) and stock_power_save (the user's stock idle value that diskOS parked). Resetting those would change the
 * config without changing the player, or drop an account. Wi-Fi/Bluetooth pairings and the SD card are outside
 * diskos.conf entirely. */
static const char *const RESET_KEYS[] = {
    "brightness", "swipe_thresh", "saver_idx", "saver_style", "screenoff_idx",
    "anim", "time_24h", "accent_mode", "accent_color", "np_style", "theme_preset", "theme_variant",
    "outdoor", "boot_default", "weather_on", "weather_loc", "artcache", "sleep_idx", "sleep_action", "idle_off_idx",
    "qs_transport", "track_numbers", "album_view", "online_art", "online_lyrics", "ota_auto",
    "custom_ov_mask", "custom_ov_font", "custom_ov_pos", NULL
};
int sys_reset_match(const char *key){
    if(!key) return 0;
    for(int i = 0; RESET_KEYS[i]; i++) if(!strcmp(key, RESET_KEYS[i])) return 1;
    return 0;
}
int sys_reset_settings(void){
    int r=cfg_remove_if(sys_reset_match);
    /* A previous replacement may have removed every key but missed directory
     * sync. The no-keys retry must flush that pending durable save as well. */
    return r ? r : cfg_flush();
}

#ifndef SYSTEM_CORE_ONLY
/* ================================================================ LVGL flow */
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "ui_dialog.h"
#include "device_names_ui.h"
#include "scanner.h"
#include "version.h"
#include "ota.h"
#include "i18n.h"
#include <pthread.h>
#include <stdatomic.h>

void settings_diskos_id(char *buf, int n);    /* settings.c */
void settings_restart_prompt(const char *note);           /* settings.c */

static lv_obj_t *g_m;                          /* the one open modal (top layer) */
static void dl_forget(lv_obj_t *o);
static void modal_close(void){ if(g_m){ dl_forget(g_m); lv_obj_delete_async(g_m); g_m = NULL; } }
static void cb_close(lv_event_t *e){ if(lv_event_get_code(e) == LV_EVENT_CLICKED){ modal_close(); } }

/* All floating dialogs use the shared content-sized shell and theme panel identity. */
static ui_dialog_t dlg_open(const char *title){
    modal_close();
    ui_dialog_cfg_t cfg = { .width = 296, .margin = 4, .scrim = 1, .actions = 1,
                            .fit_width = 1 };
    ui_dialog_t d = ui_dialog_create(lv_layer_top(), &cfg);
    g_m = d.scrim;
    lv_obj_t *t = ui_dialog_label(&d, title);
    lv_obj_set_style_text_font(t, TF(UI_16), 0);
    lv_obj_set_style_text_color(t, TC(TEXT_PRIMARY), 0);
    return d;
}
static lv_obj_t *dlg_text(const ui_dialog_t *d, const char *txt, theme_color_role_t col){
    lv_obj_t *s = ui_dialog_label(d, txt);
    lv_obj_set_style_text_font(s, TF(UI_14), 0);
    lv_obj_set_style_text_color(s, theme_color(col), 0);
    return s;
}
static void modal_simple(const char *title, const char *body){          /* one "OK" */
    ui_dialog_t d = dlg_open(title);
    dlg_text(&d, body, THEME_CLR_TEXT_MUTED);
    ui_dialog_button(&d, "OK", THEME_CLR_ACCENT_EMPHASIS, cb_close, "system.ok");
    ui_dialog_finalize(&d);
}

/* ---- Device Info (what the old About row showed is in its diskOS line) ---- */
void system_about_open(int v){
    (void)v;
    sys_about_row_t rows[12];
    char id[40]; settings_diskos_id(id, sizeof id);
    int n = sys_about_collect(NULL, id, rows, 12);
    int rb = ota_rollback_take(&OTA_DEV);                    /* boot rolled a failed update back: tell the user once */
    ui_dialog_t d = dlg_open("Device Info");
    if(rb){
        char note[80]; snprintf(note, sizeof note, "Update failed to start - back on %s", DISKOS_VERSION);
        dlg_text(&d, note, THEME_CLR_STATUS_WARNING);
    }
    lv_obj_set_style_pad_row(d.content, 6, 0);
    for(int i = 0; i < n; i++){                              /* the rows live in the dialog body: it scrolls, no nested list */
        lv_obj_t *l = lv_label_create(d.content);
        lv_label_set_text(l, rows[i].label);
        lv_obj_set_width(l, LV_PCT(100));                    /* rows read left to right, as before */
        lv_obj_set_style_text_font(l, TF(UI_12), 0);
        lv_obj_set_style_text_color(l, TC(TEXT_MUTED), 0);
        lv_obj_t *v = lv_label_create(d.content);
        lv_label_set_text(v, rows[i].value);
        lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(v, LV_PCT(100));
        lv_obj_set_style_text_font(v, TF(UI_14), 0);
        lv_obj_set_style_text_color(v, TC(TEXT_PRIMARY), 0);
    }
    ui_dialog_button(&d, "Close", THEME_CLR_ACCENT_EMPHASIS, cb_close, "system.about_close");
    ui_dialog_finalize(&d);
}

/* ---- Update from SD card: information only. diskOS never starts a firmware update. ---- */
void system_update_sd_open(int v){
    (void)v;
    sys_update_t u; memset(&u, 0, sizeof u);
    if(!sys_is_mounted(SYS_DEV_PATHS.mounts, SYS_DEV_PATHS.sd_root) || !sys_find_update(SYS_DEV_PATHS.sd_root, &u)){ ui_toast("No update file found on the SD card"); return; }
    char body[300], ver[16]; sys_fw_label(u.version, ver, sizeof ver);
    snprintf(body, sizeof body, "Found a file named like a stock update (%s, unchecked). diskOS does not use it. A stock update replaces diskOS: set Default UI to stock, run stock's updater, then reinstall diskOS with the installer.", ver);
    ui_dialog_t d = dlg_open("Update file");
    dlg_text(&d, body, THEME_CLR_STATUS_WARNING);
    ui_dialog_button(&d, "OK", THEME_CLR_ACCENT_EMPHASIS, cb_close, "system.ok");
    ui_dialog_finalize(&d);
}

/* ---- Check for diskOS updates, then (when the release carries the signed bundle) download + verify + stage ---- */
/* UI thread owns origin/drop and the timer; the worker owns tag/bundle until
 * its release store. A cancelled job retains its slot until completion is reaped. */
enum { CHECK_MANUAL, CHECK_AUTO };
static struct { pthread_t th; _Atomic int state; char tag[40]; int bundle; } g_chk; /* worker: 0 idle, 1 running, 2 ok, 3 failed */
static struct { _Atomic int cancel; int origin, drop; } g_check;
static lv_timer_t *g_chk_tm;
/* UI-thread cache: publish only durably saved records. Failed storage blocks
 * notices until reinitialization, including uncertain directory fsync results. */
static struct { char available[24], announced[24], dismissed[24]; int bundle, blocked, waiting; } g_notice;
static int notice_version(const char *s, char out[24]){
    unsigned v[3] = {0};
    if(*s == 'v' || *s == 'V') s++;
    for(int i = 0; i < 3; i++){
        if(*s < '0' || *s > '9') return 0;
        do { v[i] = v[i] * 10 + (unsigned)(*s++ - '0'); if(v[i] > 100000) return 0; }
        while(*s >= '0' && *s <= '9');
        if(i < 2 && *s++ != '.') return 0;
    }
    if(*s) return 0;
    snprintf(out, 24, "%u.%u.%u", v[0], v[1], v[2]); return 1;
}
static int notice_save(const char *key, const char *version, int bundle){
    if(g_notice.blocked) return 0;
    cfg_begin();
    cfg_set_str_deferred(key, version);
    if(bundle >= 0) cfg_set_str_deferred("ota_bundle", bundle ? "1" : "0");
    if(cfg_commit() != 0){ g_notice.blocked = 1; return 0; }
    return 1;
}
int system_ota_available(void){
    return !g_notice.blocked && ota_allowed() && *g_notice.available &&
        sys_ver_cmp(DISKOS_VERSION, g_notice.available) == -1 &&
        sys_ver_cmp(g_notice.dismissed, g_notice.available) == -1;
}
static void notice_result(const char *tag, int bundle){
    char v[24];
    if(!notice_version(tag, v)) return;
    if(!notice_save("ota_available", v, bundle)) return;
    strcpy(g_notice.available, v); g_notice.bundle = !!bundle;
    settings_row_values_refresh();
}
static void notice_poll(void){
    char pending[40];
    int cmp = sys_ver_cmp(g_notice.announced, g_notice.available);
    if(!system_ota_available() || (cmp != -1 && !(g_notice.waiting && cmp == 0)) ||
       !ui_ota_idle() || g_m ||
       ota_pcm_running(&OTA_DEV) || ota_pending(&OTA_DEV, pending, sizeof pending)) return;
    /* Save once before display, then retry a refused hint on later idle polls.
     * Reboot clears waiting: a crash may suppress a note, but cannot repeat it. */
    if(cmp == -1){
        if(!notice_save("ota_announced", g_notice.available, -1)) return;
        strcpy(g_notice.announced, g_notice.available);
        g_notice.waiting = 1;
    }
    char msg[80]; snprintf(msg, sizeof msg, tr("diskOS %s is available"), g_notice.available);
    if(ui_toast_hint(msg)) g_notice.waiting = 0;
}
static void cb_ota_later(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char *tag = lv_obj_get_user_data(lv_event_get_target(e));
    char v[24];
    if(notice_version(tag, v) && sys_ver_cmp(g_notice.dismissed, v) == -1){
        if(!notice_save("ota_dismissed", v, -1)){
            ui_toast(tr("Couldn't save your choice. Try again.")); return;
        }
        strcpy(g_notice.dismissed, v);
    }
    modal_close(); settings_row_values_refresh();
}

static void *chk_thread(void *a){
    (void)a;
    int ok = ota_fetch_release_cancel(&OTA_DEV, g_chk.tag, sizeof g_chk.tag, &g_chk.bundle, &g_check.cancel) == 0;
    atomic_store_explicit(&g_chk.state, ok ? 2 : 3, memory_order_release);
    return NULL;
}

/* Download state: 0 idle, 1 running, 2 finished (release publishes rc), 3 joined, awaiting presentation. */
static struct { pthread_t th; _Atomic int state; ota_prog_t pg; char tag[40]; int rc; } g_dl;
static lv_timer_t *g_dl_tm;
static lv_obj_t *g_dl_lbl, *g_dl_bar, *g_dl_cancel, *g_dl_owner;
static int g_dl_value, g_dl_disabled;
static char g_dl_text[128];
static void dl_forget(lv_obj_t *o){
    if(o != g_dl_owner) return;
    g_dl_lbl = g_dl_bar = g_dl_cancel = g_dl_owner = NULL;
}
static void dl_delete(lv_event_t *e){
    lv_obj_t *o = lv_event_get_target(e);
    dl_forget(o);
    if(g_m == o) g_m = NULL;
}
static void *dl_thread(void *a){ (void)a; g_dl.rc = ota_stage(&OTA_DEV, DISKOS_VERSION, g_dl.tag, &g_dl.pg); atomic_store_explicit(&g_dl.state, 2, memory_order_release); return NULL; }
static void dl_status(void){
    if(!ui_screen_backlight_on() || !g_dl_owner || g_m != g_dl_owner) return;
    char t[128]; int ph = atomic_load(&g_dl.pg.phase);
    int cancel = atomic_load(&g_dl.pg.cancel);
    if(cancel == 2) ph = 3;
    long long tot = atomic_load(&g_dl.pg.total), done = atomic_load(&g_dl.pg.done);
    int value = ph >= 2 ? 100 : tot <= 0 ? -1 : done <= 0 ? 0 : done >= tot ? 100 : (int)(done * 100 / tot);
    if(cancel == 1 && ph < 3) snprintf(t, sizeof t, "%s", tr("Cancelling..."));
    else if(ph == 2) snprintf(t, sizeof t, "%s", tr("Verifying..."));
    else if(ph == 3) snprintf(t, sizeof t, "%s", tr("Saving..."));
    else if(value >= 0) snprintf(t, sizeof t, tr("Downloading... %d%%"), value);
    else snprintf(t, sizeof t, "%s", tr("Downloading..."));
    if(strcmp(t, g_dl_text)){ lv_label_set_text(g_dl_lbl, t); snprintf(g_dl_text, sizeof g_dl_text, "%s", t); }
    if(value != g_dl_value){
        lv_bar_set_start_value(g_dl_bar, 0, LV_ANIM_OFF);
        lv_bar_set_value(g_dl_bar, value < 0 ? 65 : value, LV_ANIM_OFF);
        if(value < 0) lv_bar_set_start_value(g_dl_bar, 35, LV_ANIM_OFF);
        g_dl_value = value;
    }
    if((ph >= 3) != g_dl_disabled){
        if(ph >= 3) lv_obj_add_state(g_dl_cancel, LV_STATE_DISABLED);
        else lv_obj_remove_state(g_dl_cancel, LV_STATE_DISABLED);
        g_dl_disabled = ph >= 3;
    }
}
static void dl_tick(lv_timer_t *t){
    if(t != g_dl_tm) return;
    int state = atomic_load_explicit(&g_dl.state, memory_order_acquire);
    if(state < 2){ dl_status(); return; }
    if(state == 2){ pthread_join(g_dl.th, NULL); atomic_store(&g_dl.state, 3); }
    if(!ui_screen_backlight_on() || (g_m && g_m != g_dl_owner)) return;
    lv_timer_delete(t); g_dl_tm = NULL;
    int rc = g_dl.rc; char body[120];
    atomic_store(&g_dl.state, 0);
    modal_close();
    if(rc == OTA_OK){
        snprintf(body, sizeof body, "diskOS %s is ready. Restart to finish updating.", g_dl.pg.version);
        settings_restart_prompt(body);
    } else if(rc == OTA_E_CANCEL) ui_toast("Update cancelled");
    else if(rc == OTA_E_SPACE){
        snprintf(body, sizeof body, "%s (need %d MB free).", ota_err_text(rc), (int)((g_dl.pg.need_free + (1 << 20) - 1) >> 20));
        modal_simple("Update failed", body);
    } else modal_simple("Update failed", ota_err_text(rc));
}
static void cb_dl_cancel(lv_event_t *e){
    if(lv_event_get_code(e) == LV_EVENT_CLICKED)
        (void)ota_request_cancel(&g_dl.pg);
}
static void offer_delete(lv_event_t *e){ free(lv_event_get_user_data(e)); }
static void cb_ota_go(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if(!ota_allowed()){ ui_toast(ota_err_text(OTA_E_OFF)); return; }
    int idle = 0;
    if(!atomic_compare_exchange_strong(&g_dl.state, &idle, 1)){ ui_toast("Already updating"); return; }
    memset(&g_dl.pg, 0, sizeof g_dl.pg);
    const char *tag = lv_obj_get_user_data(lv_event_get_target(e));
    snprintf(g_dl.tag, sizeof g_dl.tag, "%s", tag);
    if(pthread_create(&g_dl.th, NULL, dl_thread, NULL) != 0){ atomic_store(&g_dl.state, 0); modal_close(); ui_toast("Couldn't start the update"); return; }
    ui_dialog_t d = dlg_open("Updating diskOS");
    g_dl_owner = g_m;
    lv_obj_add_event_cb(g_dl_owner, dl_delete, LV_EVENT_DELETE, NULL);
    g_dl_lbl = dlg_text(&d, tr("Downloading..."), THEME_CLR_TEXT_MUTED);
    g_dl_bar = lv_bar_create(d.content);
    lv_obj_set_size(g_dl_bar, lv_pct(100), 12);
    lv_obj_clear_flag(g_dl_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_bar_set_range(g_dl_bar, 0, 100); lv_bar_set_mode(g_dl_bar, LV_BAR_MODE_RANGE);
    g_dl_cancel = ui_dialog_button(&d, tr("Cancel"), THEME_CLR_TEXT_SECONDARY, cb_dl_cancel, "system.ota_cancel");
    ui_dialog_finalize(&d);
    g_dl_value = -2; g_dl_disabled = 0; g_dl_text[0] = 0;
    dl_status();
    g_dl_tm = lv_timer_create(dl_tick, 200, NULL);
}
static void chk_tick(lv_timer_t *t){
    int st = atomic_load_explicit(&g_chk.state, memory_order_acquire);
    if(st < 2 || t != g_chk_tm) return;
    pthread_join(g_chk.th, NULL);
    lv_timer_delete(t); g_chk_tm = NULL;
    if(g_check.drop){ atomic_store(&g_chk.state, 0); return; }
    if(st == 2) notice_result(g_chk.tag, g_chk.bundle);
    if(g_check.origin == CHECK_AUTO){ atomic_store(&g_chk.state, 0); return; }
    if(st == 3){ atomic_store(&g_chk.state, 0); modal_simple("Check failed", "Couldn't reach GitHub. Check the Wi-Fi connection and try again."); return; }
    char body[260]; int c = sys_ver_cmp(DISKOS_VERSION, g_chk.tag);
    atomic_store(&g_chk.state, 0);
    if(c == -1 && g_chk.bundle && ota_supported(&OTA_DEV) && !ota_allowed()){    /* reported, never downloaded */
        snprintf(body, sizeof body, tr("diskOS %s is available (you have %s). Updates are turned off in Settings. Turn on Allow diskOS Updates, or install it with the installer from github.com/b0hemia/diskos."), g_chk.tag, DISKOS_VERSION);
        modal_simple(tr("diskOS updates"), body);
        return;
    }
    if(c == -1 && g_chk.bundle && ota_supported(&OTA_DEV)){
        snprintf(body, sizeof body, tr("diskOS %s is available (you have %s). Download and install it now?"), g_chk.tag, DISKOS_VERSION);
        char *offer = strdup(g_chk.tag);
        if(!offer){ ui_toast("Couldn't start the check"); return; }
        ui_dialog_t d = dlg_open("diskOS update");
        dlg_text(&d, body, THEME_CLR_TEXT_MUTED);
        lv_obj_t *later = ui_dialog_button(&d, "Later", THEME_CLR_TEXT_SECONDARY, cb_ota_later, "system.ota_later");
        lv_obj_t *go = ui_dialog_button(&d, "Update", THEME_CLR_ACCENT_EMPHASIS, cb_ota_go, "system.ota_go");
        lv_obj_set_user_data(later, offer);
        lv_obj_set_user_data(go, offer);
        lv_obj_add_event_cb(go, offer_delete, LV_EVENT_DELETE, offer);
        ui_dialog_finalize(&d);
        return;
    }
    if(c == -1) snprintf(body, sizeof body, "diskOS %s is available (you have %s), but it has no in-app update. Download it from github.com/b0hemia/diskos and install it with the installer.", g_chk.tag, DISKOS_VERSION);
    else if(c == 0) snprintf(body, sizeof body, "You have the latest release (%s).", DISKOS_VERSION);
    else if(c == 1) snprintf(body, sizeof body, "You have %s, newer than the latest release (%s).", DISKOS_VERSION, g_chk.tag);
    else snprintf(body, sizeof body, "Latest release: %s. Couldn't compare it with %s.", g_chk.tag, DISKOS_VERSION);
    modal_simple("diskOS updates", body);
}
/* All coordinator entry points run on the UI thread. Eligibility wiring
 * cancels automatic-only work; attached manual work is unaffected. */
void system_ota_check_cancel_auto(void){
    if(atomic_load(&g_chk.state) && g_check.origin == CHECK_AUTO){
        g_check.drop = 1;
        atomic_store(&g_check.cancel, 1);
    }
}
static int chk_start(int origin){
    g_check.origin = origin; g_check.drop = 0;
    atomic_store(&g_check.cancel, 0);
    atomic_store(&g_chk.state, 1);
    if(pthread_create(&g_chk.th, NULL, chk_thread, NULL) != 0){ atomic_store(&g_chk.state, 0); return 0; }
    g_chk_tm = lv_timer_create(chk_tick, 300, NULL);
    return 1;
}
int system_ota_check_auto(void){
    char pv[40];
    if(atomic_load(&g_chk.state) || atomic_load(&g_dl.state)) return 0; /* busy: defer */
    if(!ota_allowed() || ota_pending(&OTA_DEV, pv, sizeof pv)) return 0;
    return chk_start(CHECK_AUTO);
}
/* Automatic scheduling belongs to the UI thread. Poll cheaply from the main
 * loop: eligibility/cancellation once a second, new attempts once a minute.
 * No timer, wake request, radio command or worker touches configuration. */
#ifdef DISKOS_HOST
/* The renderer has no live media/power ownership snapshot. Never admit traffic. */
int ui_ota_idle(void){ return 0; }
#endif
static ota_policy_t g_auto_policy;
static int g_auto_enabled;
static uint32_t g_auto_poll_at, g_auto_attempt_at;
static int auto_wifi_up(void){
    char b[8] = {0};
    FILE *f = fopen(OTA_DEV.operstate, "r");
    if(!f) return 0;
    size_t n = fread(b, 1, sizeof b, f);
    int bad = ferror(f);
    if(fclose(f) != 0) bad = 1;
    return !bad && n == 3 && !memcmp(b, "up\n", 3);
}
void system_ota_auto_init(void){
    memset(&g_notice, 0, sizeof g_notice);
    const char *keys[] = {"ota_available", "ota_announced", "ota_dismissed"};
    char *values[] = {g_notice.available, g_notice.announced, g_notice.dismissed};
    for(int i = 0; i < 3; i++){
        const char *v = cfg_get_str(keys[i], "0.0.0");
        if(!notice_version(v, values[i]) || strcmp(v, values[i])) g_notice.blocked = 1;
    }
    const char *bundle = cfg_get_str("ota_bundle", "0");
    if(strcmp(bundle, "0") && strcmp(bundle, "1")) g_notice.blocked = 1;
    g_notice.bundle = !strcmp(bundle, "1");
    ota_policy_io_t io = ota_policy_device_io();
    ota_policy_init(&g_auto_policy, &io);
    g_auto_enabled = !strcmp(cfg_get_str("ota_auto", "0"), "1") && ota_policy_enable(&g_auto_policy);
    g_auto_poll_at = lv_tick_get(); g_auto_attempt_at = g_auto_poll_at - 60000;
}
void system_ota_auto_changed(int v){
    /* Generic set_val must not save this key first: cfg memory may already
     * contain the new value after an unsuccessful durable rewrite. */
    g_auto_enabled = 0;
    system_ota_check_cancel_auto();
    if(cfg_set_str("ota_auto", v ? "1" : "0") != 0){
        ui_toast("Couldn't save your choice. Try again."); return;
    }
    if(v) g_auto_enabled = ota_policy_enable(&g_auto_policy);
}
void system_ota_auto_poll(void){
    if(!g_auto_enabled) return;
    if(strcmp(cfg_get_str("ota_auto", "0"), "1")){
        g_auto_enabled = 0; system_ota_check_cancel_auto(); return;
    }
    uint32_t now = lv_tick_get();
    if((uint32_t)(now - g_auto_poll_at) < 1000) return;
    g_auto_poll_at = now;
    int active = atomic_load(&g_chk.state) && g_check.origin == CHECK_AUTO;
    notice_poll();
    if(!active && (uint32_t)(now - g_auto_attempt_at) < 60000) return;
    if(!active) g_auto_attempt_at = now;
    char pending[40];
    int eligible = ota_allowed() && ota_supported(&OTA_DEV) && ui_ota_idle() &&
        !g_m && !atomic_load(&g_dl.state) && !ota_pcm_running(&OTA_DEV) && auto_wifi_up() &&
        !ota_pending(&OTA_DEV, pending, sizeof pending);
    if(!eligible){ system_ota_check_cancel_auto(); return; }
    if(atomic_load(&g_chk.state)) return;
    if(ota_policy_reserve(&g_auto_policy, g_auto_enabled)) system_ota_check_auto();
}
/* Promotion proof for a TRIAL build (contract 4a). Navigation alone never promotes. After >= 3 minutes up, the trial is
 * kept when (a) a real playback start was confirmed (main.c: g_play_pending cleared by a genuine track change) AND a
 * playback PCM substream is RUNNING, or (b) the user answers the one-time "Keep diskOS <ver>?" prompt with Keep. "Go back"
 * writes the revert request marker (boot rolls the trial back) and offers the restart. No proof = never promoted: the
 * boot's trial counter keeps running and rolls it back. The identity checks (running exe hash, run + launched files) are
 * in ota_health_write / ota_revert_write. */
#define OTA_HEALTH_MIN_MS 180000
#define OTA_HEALTH_RETRY_MS 30000     /* a failed save of the proof (storage full or busy) is tried again this often, */
#define OTA_HEALTH_TRIES 10           /* this many times in all: each try hashes the running build */
static struct { uint32_t t0, tw; int tries; _Atomic int play, ok, asked, busy, saved; char ver[24]; } g_hl;
static lv_timer_t *g_hl_tm;
static void *ota_health_thread(void *a){
    (void)a;
    if(ota_health_write(&OTA_DEV)) atomic_store(&g_hl.saved, 1);
    atomic_store(&g_hl.busy, 0);
    return NULL;
}
static void cb_keep(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if(!ota_health_write(&OTA_DEV)){ ui_toast(tr("Couldn't save your choice. Try again.")); return; }
    modal_close();
}
static void cb_goback(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if(!ota_revert_write(&OTA_DEV)){ ui_toast(tr("Couldn't save your choice. Try again.")); return; }
    modal_close();
    settings_restart_prompt(tr("Restart to go back to the previous version."));
}
static void ota_keep_prompt(void){
    char t[64]; snprintf(t, sizeof t, tr("Keep diskOS %s?"), g_hl.ver);
    ui_dialog_t d = dlg_open(t);
    dlg_text(&d, tr("This update has been running for a few minutes. Keep it, or go back to the previous version (the device restarts)."), THEME_CLR_TEXT_MUTED);
    ui_dialog_button(&d, tr("Go back"), THEME_CLR_STATUS_DANGER, cb_goback, "system.ota_goback");
    ui_dialog_button(&d, tr("Keep"), THEME_CLR_ACCENT_EMPHASIS, cb_keep, "system.ota_keep");
    ui_dialog_finalize(&d);
}
static void ota_health_cb(lv_timer_t *t){
    if(atomic_load(&g_hl.busy)) return;                                     /* a save is still running */
    if(atomic_load(&g_hl.saved) || g_hl.tries >= OTA_HEALTH_TRIES){ lv_timer_delete(t); g_hl_tm = NULL; return; }
    if(lv_tick_elaps(g_hl.t0) < OTA_HEALTH_MIN_MS) return;
    if(!atomic_load(&g_hl.ok) && ota_trial_decided(&OTA_DEV)){ lv_timer_delete(t); g_hl_tm = NULL; return; }
    if(!atomic_load(&g_hl.ok) && atomic_load(&g_hl.play) && ota_pcm_running(&OTA_DEV)) atomic_store(&g_hl.ok, 1);
    if(atomic_load(&g_hl.ok)){
        if(g_hl.tries && lv_tick_elaps(g_hl.tw) < OTA_HEALTH_RETRY_MS) return;
        pthread_t th;
        g_hl.tries++; g_hl.tw = lv_tick_get(); atomic_store(&g_hl.busy, 1);
        if(pthread_create(&th, NULL, ota_health_thread, NULL) == 0) pthread_detach(th);
        else atomic_store(&g_hl.busy, 0);
        return;
    }
    /* Let an existing dialog or update job finish before asking: its result must not replace Keep. */
    if(g_m || atomic_load(&g_chk.state) != 0 || atomic_load(&g_dl.state) != 0) return;
    if(atomic_load(&g_hl.asked)) return;
    atomic_store(&g_hl.asked, 1);
    ota_keep_prompt();
    lv_timer_delete(t); g_hl_tm = NULL;
}
void system_ota_note_playback(void){ atomic_store(&g_hl.play, 1); }
void system_ota_health_init(void){
    if(!ota_trial_version(&OTA_DEV, g_hl.ver, sizeof g_hl.ver)) return;     /* only a trial build has anything to prove */
    if(!ota_trial_maybe_me(&OTA_DEV)) return;   /* another build (fallback, hand-run) could never save Keep or Go back */
    if(ota_trial_decided(&OTA_DEV)) return;
    g_hl.t0 = lv_tick_get();
    g_hl_tm = lv_timer_create(ota_health_cb, 2000, NULL);
}
/* Settings > System > Allow diskOS Updates was switched (set_val already saved cfg ota_allow). Turning it OFF while a verified
 * bundle waits would still let boot install it, so OFF then needs the bundle discarded: the switch goes back to ON and the
 * user is asked. Discard removes pending/ and turns the setting OFF; Cancel leaves both as they were. With nothing staged the
 * switch simply stays OFF. The running build is never touched. */
static void cb_discard_go(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    modal_close();
    if(ota_discard_pending(&OTA_DEV) && cfg_set_int("ota_allow", 0) == 0) ui_toast(tr("Update discarded. Updates are off."));
    else ui_toast(tr("Couldn't discard the update"));
    settings_row_values_refresh();
}
void system_ota_allow_changed(int v){
    char pv[40];
    if(!v) system_ota_check_cancel_auto();
    if(v || !ota_pending(&OTA_DEV, pv, sizeof pv)){ settings_row_values_refresh(); return; }
    cfg_set_int("ota_allow", 1);                                  /* stays ON until the user confirms the discard */
    settings_row_values_refresh();
    ui_dialog_t d = dlg_open(tr("Turn off updates?"));
    dlg_text(&d, tr("Turning updates off discards the downloaded update."), THEME_CLR_TEXT_MUTED);
    ui_dialog_button(&d, tr("Cancel"), THEME_CLR_TEXT_SECONDARY, cb_close, "system.ota_off_cancel");
    ui_dialog_button(&d, tr("Discard"), THEME_CLR_STATUS_DANGER, cb_discard_go, "system.ota_discard");
    ui_dialog_finalize(&d);
}
void system_check_update_open(int v){
    (void)v;
    char pv[40];
    if(atomic_load(&g_dl.state) != 0){ ui_toast("Already updating"); return; }
    if(ota_allowed() && ota_pending(&OTA_DEV, pv, sizeof pv)){   /* a verified bundle is already staged: only a restart is left */
        char n[120]; snprintf(n, sizeof n, "diskOS %s is ready. Restart to finish updating.", pv);
        settings_restart_prompt(n);
        return;
    }
    if(ota_wifi_down(&OTA_DEV)){ modal_simple("Check failed", "Wi-Fi is not connected. Connect to Wi-Fi and try again."); return; }
    if(atomic_load(&g_chk.state) != 0){
        if(g_check.origin == CHECK_AUTO && !g_check.drop){
            ota_policy_note_manual(&g_auto_policy, g_auto_enabled);
            g_check.origin = CHECK_MANUAL;
            ui_toast("Checking for updates...");
        } else ui_toast("Already checking");
        return;
    }
    ota_policy_note_manual(&g_auto_policy, g_auto_enabled);
    if(!chk_start(CHECK_MANUAL)){ ui_toast("Couldn't start the check"); return; }
    ui_toast("Checking for updates...");
}

/* ---- Reset diskOS settings ---- */
static void cb_reset_go(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    modal_close();
    dnui_reset();
}
void system_names_reset_applied(void){
    /* Only synchronize after committed worker progress. A refused intent must
     * leave the automatic-check policy as it was. */
    if(!cfg_get_int("ota_auto",0)){ g_auto_enabled=0; system_ota_check_cancel_auto(); }
    settings_row_values_refresh();
}
void system_reset_open(int v){
    (void)v;
    ui_dialog_t d = dlg_open("Reset diskOS settings?");
    dlg_text(&d, "Resets look, behaviour, both Disc names and paired nicknames. Keeps music, pairings, Wi-Fi, Last.fm, EQ and audio. Reboot for network; Bluetooth on for nicknames.", THEME_CLR_TEXT_MUTED);
    ui_dialog_button(&d, "Cancel", THEME_CLR_TEXT_SECONDARY, cb_close, "system.reset_cancel");
    ui_dialog_button(&d, "Reset", THEME_CLR_STATUS_DANGER, cb_reset_go, "system.reset_go");
    ui_dialog_finalize(&d);
}
#endif
