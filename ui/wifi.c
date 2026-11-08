/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "wifi.h"
#include "i18n.h"
#include "theme.h"
#include "theme_kit.h"
#include "config.h"
#include "fwcaps.h"
#include "ipc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <arpa/inet.h>

/* Wi-Fi settings (SCR_WIFI) + a details screen (SCR_WIFI_INFO).
 * Radio on/off reuses the stock /usr/bin/wifi_up.sh / wifi_down.sh (rfkill +
 * wpa_supplicant + udhcpc).  Scan/connect drive the running supplicant via
 * `wpa_cli -i wlan0`.  Connecting writes the network in and `save_config`s it
 * to /usr/data/wpa_supplicant.conf (update_config=1), so it persists.
 * Round-screen-aware: the top corners stay clear (only back chevron + centred
 * title); the on/off switch + scan button share one row in the wide centre
 * band.  The list itself carries state (connected checkmark row / "Wi-Fi is off" /
 * "Scanning" / "No networks") so there is no redundant status line.
 * NB: connecting to a DIFFERENT AP drops the laptop deploy link - recover over
 * serial if needed. */

#define WCLI "/usr/sbin/wpa_cli -i wlan0 "
#define WIFI_ATTEMPT_MARKER "/tmp/diskos-wifi-unsaved"
#define WIFI_SAVED_CONFIG "/usr/data/wpa_supplicant.conf"

static lv_obj_t *g_sw, *g_list;
static lv_timer_t *g_scan_timer;
/* 0: clean, 1: awaiting outcome, 2: rollback required. Never save in 1 or 2. */
static int g_conn_dirty;
static int g_save_pending;  /* verified attempt or sanitation changes awaiting SAVE_CONFIG */
static int g_initial_reload = 1;
static int wifi_discard_attempt(void);
static int wifi_reply_ok(const char *out);
static lv_timer_t *g_conn_timer;      /* non-NULL while a user connect is being polled */
static uint32_t    g_wifi_last_start = 0;   /* last wifi_up.sh launch (shared: toggle + keepalive) */
static int         g_sanitize_pending = 1;  /* sanitize saved nets once the supplicant is up (set on 1st run + each (re)launch) */
static int         g_wifi_theme_reload;
static unsigned    g_intent_epoch = 0;      /* bumped by every explicit user Wi-Fi choice (switch or QS tile) */
static char g_pending_ssid[64];   /* SSID awaiting a password from the keyboard */
static char g_cur_ssid[64];       /* currently-connected SSID (to mark the list) */
static lv_obj_t *g_info_list;     /* details screen */

static void start_scan(void);     /* fwd */
static int wifi_list_ready(void);
static lv_timer_t *g_radio_timer;
static void radio_on_poll_cb(lv_timer_t *t);
static void wifi_lease_retry_reset(void);
static void scan_fill(void);
static void wifi_info_refresh(void);
extern int ui_screen_backlight_on(void);

/* The refresh glyph INSIDE the "Scanning" message spins while a scan runs. It lives
 * in g_list, so it must be stopped before g_list is cleaned (else the anim references
 * a freed object). scan_stop() is called at the top of every g_list-clearing path. */
static lv_obj_t *g_scan_icon;
static void spin_anim_cb(void *o, int32_t v){ lv_obj_set_style_transform_rotation((lv_obj_t*)o, v, 0); }
static void scan_stop(void){
    if(g_scan_icon){ lv_anim_delete(g_scan_icon, spin_anim_cb); g_scan_icon = NULL; }
}

/* ---- tiny local copies of settings.c's header helpers ------------------- */

/* ---- shell helpers ------------------------------------------------------ */
/* Run cmd and return ALL of its output (malloc'd, NUL-terminated; caller frees), read to EOF, capped at
 * `max` bytes (a bigger output is cut there, which the row parser treats as a partial last row). */
static char *run_cap_all(const char *cmd, size_t max){
    char *b = malloc(max); if(!b) return NULL;
    ui_run_cap_bounded(cmd, b, (int)max, 2000);
    return b;
}
static int run_cap(const char *cmd, char *out, int cap){
    return ui_run_cap_bounded(cmd, out, cap, 1000);
}

/* Write ahead of ADD/SET/SELECT, so a UI crash cannot hide unsaved daemon state.
 * /tmp is tmpfs on V2.57 (etc/fstab:6); a reboot discards daemon RAM as well.
 * No credentials or IDs belong in this marker. Failure must prevent mutation. */
static int wifi_attempt_mark(void){
    int fd = open(WIFI_ATTEMPT_MARKER, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if(fd < 0) return 0;
    return close(fd) == 0;
}
static int wifi_attempt_clear(void){
    return unlink(WIFI_ATTEMPT_MARKER) == 0 || errno == ENOENT;
}
/* Reload only when an earlier UI left an attempt unsaved. Absence is the cheap
 * clean-start path; unreadable marker state or uncertain replies keep saves blocked.
 * Prepared theme exec follows the same rule: its rollback already cleared it. */
static int wifi_initial_reload(void){
    if(!g_initial_reload) return 1;
    struct stat st;
    if(lstat(WIFI_ATTEMPT_MARKER, &st) < 0){
        if(errno != ENOENT) return 0;
    }else{
        char out[64]; run_cap(WCLI "reconfigure 2>/dev/null", out, sizeof out);
        if(!wifi_reply_ok(out) || !wifi_attempt_clear()) return 0;
    }
    g_initial_reload = 0;
    return 1;
}

/* Short-lived edit session: IDs from LIST/ADD must never reach a replacement
 * daemon. Follow wpa_ctrl_open/request, without ATTACH or reconnection. Close
 * after setup; rollback uses RECONFIGURE, never retained numeric IDs.
 * https://www.w1.fi/wpa_supplicant/devel/ctrl_iface_page.html */
static int g_edit_fd = -1;
static void wifi_edit_close(void){
    if(g_edit_fd >= 0) close(g_edit_fd);
    g_edit_fd = -1;
}
static int wifi_edit_open(void){
    wifi_edit_close();
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if(fd < 0) return 0;
    struct sockaddr_un local = { .sun_family = AF_UNIX };
    struct sockaddr_un remote = { .sun_family = AF_UNIX };
    snprintf(remote.sun_path, sizeof remote.sun_path, "/var/run/wpa_supplicant/wlan0");
    /* Linux abstract autobind: no temporary pathname to unlink. */
    if(bind(fd, (struct sockaddr *)&local, sizeof local.sun_family) ||
       connect(fd, (struct sockaddr *)&remote, sizeof remote)){
        close(fd); return 0;
    }
    g_edit_fd = fd;
    return 1;
}
static int wifi_edit_request(const char *cmd, char *out, int cap){
    out[0] = 0;
    if(g_edit_fd < 0 || cap < 2) return 0;
    if(send(g_edit_fd, cmd, strlen(cmd), MSG_NOSIGNAL) != (ssize_t)strlen(cmd))
        goto failed;
    struct pollfd p = { .fd = g_edit_fd, .events = POLLIN };
    if(poll(&p, 1, 1000) <= 0 || !(p.revents & POLLIN)) goto failed;
    int n = recv(g_edit_fd, out, cap - 1, MSG_TRUNC);
    if(n <= 0 || n >= cap - 1) goto failed;
    out[n] = 0;
    return n;
failed:
    /* A late reply cannot authorize a subsequent command. No liveness probes. */
    out[0] = 0;
    wifi_edit_close();
    return 0;
}

static int wifi_dhcp_absent(void);

/* Radio-up stays asynchronous. Its children belong to stock, not our probe group. */
static int wifi_shell(const char *cmd){
    static pid_t launch;
    if(launch > 0){
        if(waitpid(launch, NULL, WNOHANG) != 0) launch = 0;
    }
    if(strstr(cmd, "wifi_up.sh")){
        /* With a surviving client, restore only the supplicant. Never run the
         * stock script here: it starts another DHCP client unconditionally. */
        int absent = wifi_dhcp_absent();
        if(absent < 0) return -1;
        if(!absent) return wifi_shell("rfkill unblock wifi; ifconfig wlan0 up; "
            "wpa_supplicant -B -i wlan0 -c /usr/data/wpa_supplicant.conf");
        if(launch > 0) return -1;
        launch = fork();
        if(launch == 0){ execl("/bin/sh", "sh", "-c", ". /usr/bin/wifi_up.sh; wait", (char *)NULL); _exit(127); }
        return launch < 0 ? -1 : 0;
    }
    char bounded[1024], out[64];
    snprintf(bounded, sizeof bounded, "%s; printf '\nRC=%%d\n' \"$?\"", cmd);
    ui_run_cap_bounded(bounded, out, sizeof out,
                       strstr(cmd, WCLI "scan ") ? 1700 : 2000);
    return strstr(out, "\nRC=0\n") ? 0 : -1;
}
#define system wifi_shell

/* wpa_cli prints SSIDs (scan_results, list_networks, status) printf-encoded: \\ \" \n \r \t \e and \xNN
 * for every non-printable / non-ASCII byte. Decode to the real bytes into out[cap]. Returns the byte
 * count, or -1 if the SSID is longer than 32 bytes (the 802.11 limit), does not fit, has a malformed
 * escape, or holds a control byte / NUL (unusable as a label and unsafe in a wpa_cli command). */
static int wifi_ssid_decode(const char *in, int inlen, char *out, int cap){
    int o = 0;
    for(int i=0;i<inlen;i++){
        unsigned char c = (unsigned char)in[i], v = c;
        if(c == '\\'){
            if(++i >= inlen) return -1;
            char e = in[i];
            if(e == '\\' || e == '"') v = (unsigned char)e;
            else if(e == 'x'){
                int h = 0;
                for(int k=1;k<=2;k++){
                    if(i + k >= inlen) return -1;
                    char d = in[i+k]; int dv;
                    if(d >= '0' && d <= '9') dv = d - '0';
                    else if(d >= 'a' && d <= 'f') dv = d - 'a' + 10;
                    else if(d >= 'A' && d <= 'F') dv = d - 'A' + 10;
                    else return -1;
                    h = h * 16 + dv;
                }
                v = (unsigned char)h; i += 2;
            }
            else if(e == 'n') v = '\n';
            else if(e == 'r') v = '\r';
            else if(e == 't') v = '\t';
            else if(e == 'e') v = 27;
            else return -1;
        }
        if(v < 0x20 || v == 0x7f) return -1;
        if(o >= 32 || o >= cap - 1) return -1;
        out[o++] = (char)v;
    }
    out[o] = 0;
    return o;
}
/* does the list_networks ssid field fld[0..flen) name exactly the real SSID `ssid`? */
static int wifi_ssid_field_eq(const char *fld, int flen, const char *ssid){
    char d[64]; int n = wifi_ssid_decode(fld, flen, d, sizeof d);
    return n >= 0 && (size_t)n == strlen(ssid) && memcmp(d, ssid, (size_t)n) == 0;
}

static int wifi_lease_ready(const char *status);
static int wifi_status(char *ssid, int scap, char *ip, int icap){
    (void)scap; (void)icap;
    ssid[0] = 0; ip[0] = 0;
    char buf[2048];
    if(!run_cap(WCLI "status 2>/dev/null", buf, sizeof buf)) return 0;
    int connected = (strstr(buf, "wpa_state=COMPLETED") != NULL);
    char *l = buf;
    while(l && *l){
        if(!strncmp(l, "ssid=", 5)){
            char *e = strchr(l+5, '\n'); int n = e ? (int)(e - (l+5)) : (int)strlen(l+5);
            if(wifi_ssid_decode(l+5, n, ssid, 64) < 0) ssid[0] = 0;
        }
        else if(!strncmp(l, "ip_address=", 11)){ sscanf(l+11, "%31[^\n]", ip); }
        l = strchr(l, '\n'); if(l) l++;
    }
    return connected && wifi_lease_ready(buf);
}

static int wifi_radio_on(void){
    char buf[256];
    run_cap("pidof wpa_supplicant 2>/dev/null", buf, sizeof buf);
    return buf[0] ? 1 : 0;
}

/* ---- persistence / auto-reconnect hardening ----------------------------- */
/* Make the running supplicant's config writable, keep EVERY saved network a candidate
 * for auto-join (not just the last-selected), and drop stale per-network channel pins
 * (a network locked to a frequency the AP has since left never associates). Run on each
 * supplicant bring-up and after a user connect. */
/* wpa_cli only responds once the control interface is up - pidof can be true well before
 * that, so a ping (PONG) is the real "ready" signal. */
static int wpa_ready(void){
    char buf[64];
    ui_run_cap_bounded(WCLI "ping 2>/dev/null", buf, sizeof buf, 200);
    return strstr(buf, "PONG") != NULL;
}
/* Read the saved global field directly, so enabling persistence still gets
 * saved when needed. No file writes. */
static int wifi_config_writable(void){
    FILE *f = fopen(WIFI_SAVED_CONFIG, "r");
    if(!f) return errno == ENOENT ? 0 : -1;
    char line[512]; int value = 0, v;
    while(fgets(line, sizeof line, f))
        if(sscanf(line, " update_config = %d", &v) == 1) value = v;
    int failed = ferror(f); fclose(f);
    return failed ? -1 : value == 1;
}
static int wifi_network_int(int id, const char *field, int *value){
    char cmd[128], out[64], *end;
    snprintf(cmd, sizeof cmd, WCLI "get_network %d %s 2>/dev/null", id, field);
    run_cap(cmd, out, sizeof out);
    errno = 0;
    long v = strtol(out, &end, 10);
    if(end == out || errno || v < 0 || v > 2147483647L) return 0;
    while(*end == '\r' || *end == '\n') end++;
    if(*end) return 0;
    *value = (int)v; return 1;
}
/* Returns 1 when sanitation is complete. Rollback never writes the saved file.
 * Keep pending saves independent of differences: a failed save leaves RAM clean. */
static int wifi_sanitize_savenets(void){
    if(!wpa_ready()) return 0;
    if(g_conn_dirty){
        if(g_conn_timer) return 0;
        return wifi_discard_attempt();
    }
    if(!wifi_initial_reload()) return 0;
    int writable = wifi_config_writable();
    if(writable < 0) return 0;
    char out[64];
    run_cap(WCLI "set update_config 1 2>/dev/null", out, sizeof out);
    if(!wifi_reply_ok(out)) return 0;
    if(!writable) g_save_pending = 1;
    char buf[4096];
    int n = run_cap(WCLI "list_networks 2>/dev/null", buf, sizeof buf);
    if(n <= 0 || n >= (int)sizeof buf - 1 || strncmp(buf, "network id", 10) || buf[n-1] != '\n') return 0;
    char *l = strchr(buf, '\n'); if(l) l++;
    int enable = 0;
    while(l && *l){
        int nid, disabled, frequency;
        if(sscanf(l, "%d", &nid) != 1 || nid < 0 ||
           !wifi_network_int(nid, "disabled", &disabled) ||
           !wifi_network_int(nid, "frequency", &frequency)) return 0;
        if(disabled == 1) enable = 1;  /* stock enable-all leaves disabled=2 alone (0x4e75e8-0x4e760c) */
        if(frequency){
            char cmd[128];
            /* Preserve recovery across UI restart after RAM normalization,
             * before SAVE_CONFIG has confirmed persistence. */
            if(!wifi_attempt_mark()) return 0;
            g_save_pending = 1;  /* retain retries even if this reply is lost */
            snprintf(cmd, sizeof cmd, WCLI "set_network %d frequency 0 2>/dev/null", nid);
            run_cap(cmd, out, sizeof out);
            if(!wifi_reply_ok(out)) return 0;
        }
        l = strchr(l, '\n'); if(l) l++;
    }
    if(enable){
        if(!wifi_attempt_mark()) return 0;
        g_save_pending = 1;
        run_cap(WCLI "enable_network all 2>/dev/null", out, sizeof out);
        if(!wifi_reply_ok(out)) return 0;
    }
    if(g_save_pending){
        run_cap(WCLI "save_config 2>/dev/null", out, sizeof out);
        if(!wifi_reply_ok(out)) return 0;
        g_save_pending = 0;
    }
    return wifi_attempt_clear();
}

/* Seed diskOS's own Wi-Fi on/off intent from stock SYSCONFIG.WIFI_STATUS the FIRST time
 * only; thereafter diskOS owns "wifi_on" (the toggle persists every change), so the
 * keepalive can distinguish "supplicant died" from "user turned Wi-Fi off". */
void wifi_init_intent(void){
    g_wifi_theme_reload = getenv("DISKOS_THEME_RELOAD") != NULL;
    if(cfg_get_int("wifi_on", -1) >= 0) return;           /* already owned */
    char buf[32];
    run_cap("sqlite3 /usr/data/fiio/db/sysconfig.db \"SELECT WIFI_STATUS FROM SYSCONFIG WHERE ID=1\" 2>/dev/null",
            buf, sizeof buf);
    /* only persist a VALID read (0/1); a failed/empty query leaves it unseeded so it
     * retries next boot AND the keepalive's default (on) applies meanwhile - never
     * silently latch Wi-Fi off because sqlite hiccuped. */
    if(buf[0]=='0' || buf[0]=='1') cfg_set_int("wifi_on", buf[0]-'0');
}

/* Keepalive: whenever Wi-Fi should be on but wpa_supplicant isn't running (died, never
 * started, radio glitch) (re)start it, so a known network returning in range auto-joins.
 * Sanitize saved nets once the supplicant is up after each (re)launch - driven by a pending
 * flag (set on first run + every launch we initiate, cleared after one run), so it's
 * deterministic and idempotent regardless of pid reuse or a transient pidof miss. Enforce a
 * deliberate OFF against a late in-flight wifi_up. Self-rate-limited: safe to call each loop. */
/* ---- V2.57 radio reconciliation ---------------------------------------------------------------------
 * V2.57's player turns Wi-Fi off at boot when both radios were saved on (keeps Bluetooth, saves WIFI_STATUS=0).
 * diskOS keeps the user's saved "wifi_on" intent. Wait for stock startup to settle before restoring it.
 * Stock arbitration must not turn that preference into a permanent OFF. Explicit user choices end the hold.
 * A player that never answers releases the hold unchanged after RECO_GIVEUP_MS. */
#define RECO_SETTLE_MS 9000      /* same settle as the local-init handshake (past the ~7 s mode-control window) */
#define RECO_GIVEUP_MS 30000     /* silent player: release unchanged this long after arming */
#define RECO_WINDOW_MS 30000     /* answered player: keep re-checking this long after its first frame, since stock's
                                  * radio work can finish well after the settle (our own BT restore allows 28 s) */
#define RECO_PROBE_MS  1000      /* every probe (sqlite read, pidof) is killed at this bound: never blocks the UI */
static int      g_reco_hold = 0;           /* 1 = keepalive passive while this generation is being reconciled */
static unsigned g_reco_gen  = 0xFFFFFFFFu; /* generation armed */
static unsigned g_reco_rx0, g_reco_epoch0;
static uint32_t g_reco_armed_at, g_reco_first_rx_at, g_reco_probe_at, g_reco_eval_at;
static int      g_reco_seen_rx;

/* The decision, pure: 1 = stock boot arbitration has settled. Inputs must be clean 0/1 reads
 * (callers pass -1 for anything unknown, which never matches). */
static int wifi_reco_decide(int db_wifi, int db_bt, int eff_wifi, int eff_bt, int want_wifi, int user_changed){
    return db_wifi == 0 && db_bt == 1 && eff_wifi == 0 && eff_bt == 1 && want_wifi == 1 && !user_changed;
}

/* Saved radio state from stock SYSCONFIG: 1 and fills both on a clean "d|d" read, else 0. Bounded. */
static int read_saved_radios(int *wifi, int *bt){
    char buf[32] = {0};
    ui_run_cap_bounded("sqlite3 /usr/data/fiio/db/sysconfig.db \"SELECT WIFI_STATUS||'|'||BT_STATUS FROM SYSCONFIG WHERE ID=1\" 2>/dev/null",
                       buf, sizeof buf, RECO_PROBE_MS);
    if((buf[0] == '0' || buf[0] == '1') && buf[1] == '|' && (buf[2] == '0' || buf[2] == '1') && (buf[3] == '\n' || buf[3] == 0)){
        *wifi = buf[0] - '0'; *bt = buf[2] - '0'; return 1;
    }
    return 0;
}
/* Wi-Fi rfkill state: 1 blocked, 0 unblocked, -1 unknown. */
static int wifi_rfkill_blocked(void){
    for(int i = 0; i < 12; i++){
        char p[64]; snprintf(p, sizeof p, "/sys/class/rfkill/rfkill%d/type", i);
        FILE *f = fopen(p, "r"); if(!f) continue;
        char t[16] = {0}; char *r = fgets(t, sizeof t, f); fclose(f);
        if(!r || strncmp(t, "wlan", 4) != 0) continue;
        snprintf(p, sizeof p, "/sys/class/rfkill/rfkill%d/soft", i);
        f = fopen(p, "r"); if(!f) return -1;
        int soft = -1; if(fscanf(f, "%d", &soft) != 1) soft = -1; fclose(f);
        snprintf(p, sizeof p, "/sys/class/rfkill/rfkill%d/hard", i);
        f = fopen(p, "r"); if(!f) return -1;
        int hard = -1; if(fscanf(f, "%d", &hard) != 1) hard = -1; fclose(f);
        return soft == 1 || hard == 1 ? 1 : (soft == 0 && hard == 0 ? 0 : -1);
    }
    return -1;
}
/* Effective Wi-Fi the way stock's wifi_down.sh leaves it: 0 = no supplicant AND radio rfkill-blocked, 1 = either one
 * says on, -1 = could not tell (probe failed / no rfkill switch). */
static int wifi_effective(void){
    char buf[64] = {0};
    int rc = ui_run_cap_bounded("pidof wpa_supplicant 2>/dev/null; echo done", buf, sizeof buf, RECO_PROBE_MS);
    if(rc <= 0 || !strstr(buf, "done")) return -1;              /* probe did not finish */
    int daemon = (buf[0] >= '0' && buf[0] <= '9');
    int blocked = wifi_rfkill_blocked();
    if(daemon || blocked == 0) return 1;
    return blocked == 1 ? 0 : -1;
}

/* Controls show the saved user choice, including while startup or recovery is pending.
 * Readiness belongs in the page message; rfkill must not make an ON tap mean OFF. */
int wifi_control_on(void){
    return cfg_get_int("wifi_on", 1) != 0;
}

static void wifi_reconcile_poll(void){
    if(!fw_has_radio_boot_arbitration()){ g_reco_hold = 0; return; }
    unsigned gen = ipc_generation(), rx = ipc_rx_frames();
    uint32_t now = lv_tick_get();
    int first_generation = 0;
    if(gen != g_reco_gen){                          /* new player generation: arm */
        first_generation = (g_reco_gen == 0xFFFFFFFFu);
        g_reco_gen = gen; g_reco_hold = 1; g_reco_rx0 = rx; g_reco_epoch0 = g_intent_epoch;
        g_reco_armed_at = now; g_reco_seen_rx = 0; g_reco_probe_at = now - 1000; g_reco_eval_at = now - RECO_PROBE_MS;
    }
    if(!g_reco_hold) return;
    if(g_intent_epoch != g_reco_epoch0){ g_reco_hold = 0; return; }        /* the user chose: their choice wins */
    if(cfg_get_int("wifi_on", 1) == 0){ g_reco_hold = 0; return; }         /* Wi-Fi intent already off: nothing to undo */
    if(first_generation){
        if(g_wifi_theme_reload){ g_reco_hold = 0; return; }
        int dbw, dbb;
        /* Stock arbitrates from saved SYSCONFIG state: BT_STATUS=0 means it cannot turn Wi-Fi off at boot.
         * Only use this shortcut on UI start; Bluetooth state may change before a later player restart. */
        if(read_saved_radios(&dbw, &dbb) && dbb == 0){
            g_reco_hold = 0;
            fprintf(stderr, "radio reconcile (gen %u): saved Bluetooth off, released boot hold\n", gen);
            return;
        }
    }
    if(!g_reco_seen_rx){
        if(rx == g_reco_rx0){
            if(now - g_reco_armed_at >= RECO_GIVEUP_MS){ g_reco_hold = 0; return; }   /* silent player: unchanged */
            if(now - g_reco_probe_at >= 1000){ g_reco_probe_at = now; ipc_send_probe("02020008"); }   /* solicit, 1/s */
            return;
        }
        g_reco_seen_rx = 1; g_reco_first_rx_at = now;
    }
    if(now - g_reco_first_rx_at < RECO_SETTLE_MS) return;
    if(now - g_reco_eval_at < 1000) return;         /* re-check about once a second */
    g_reco_eval_at = now;
    int dbw = -1, dbb = -1;
    if(!read_saved_radios(&dbw, &dbb)) dbw = dbb = -1;                       /* unknown: never adopts */
    int effw = wifi_effective(), effb = bt_radio_on();
    /* the probes themselves take time (each is bounded, not instant): judge the window on the clock AFTER them, so a
     * check that started just inside it can never adopt once it has closed */
    if(lv_tick_get() - g_reco_first_rx_at >= RECO_WINDOW_MS){ g_reco_hold = 0; return; }
    if(g_intent_epoch != g_reco_epoch0){ g_reco_hold = 0; return; }        /* the user chose while we probed */
    if(wifi_reco_decide(dbw, dbb, effw, effb, cfg_get_int("wifi_on", 1), 0)){
        fprintf(stderr, "radio reconcile (gen %u): stock boot settled, retaining wifi_on=1\n", gen);
        g_reco_hold = 0;
        return;
    }
    if(now - g_reco_first_rx_at >= RECO_WINDOW_MS) g_reco_hold = 0;       /* no conflict appeared: release unchanged */
}

/* Stock owns DHCP. Only signal a uniquely identified process. */
struct wifi_proc { int pid; unsigned long long start; };
struct wifi_obs { struct wifi_proc wpa, dhcp; int id, ip, idle, sockets, allowed; char ssid[64]; };
static struct {
    struct wifi_obs net; unsigned epoch, intent, player; uint32_t since;
    int phase, seen_empty, release_evidence, released, resume;
} g_lease;
enum { LEASE_NONE, LEASE_RELEASE, LEASE_ACQUIRE, LEASE_READY, LEASE_FAILED };
static char g_lease_dir[64];
static long g_lease_offset;
static pid_t g_lease_helper;
static uint32_t g_lease_check, g_lease_retry_ms = 100;
static int g_lease_settled;   /* a lease step has seen offline, ready or failed */

static int wifi_proc_stat(int pid, struct wifi_proc *p, int *parent){
    char path[64], buf[1024]; snprintf(path, sizeof path, "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r"); if(!f) return 0;
    errno = 0;
    char *r = fgets(buf, sizeof buf, f); int err = errno; fclose(f); errno = err;
    char *q = r ? strrchr(buf, ')') : NULL; if(!q) return 0;
    char *save, *t = strtok_r(q + 2, " ", &save); unsigned long long start = 0;
    for(int field = 3; t && field <= 22; field++, t = strtok_r(NULL, " ", &save)){
        if(field == 4) *parent = atoi(t);
        if(field == 22) start = strtoull(t, NULL, 10);
    }
    p->pid = pid; p->start = start; return start != 0;
}
static int wifi_proc_match(int pid, const char *name){
    char path[64], buf[2048]; snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
    FILE *f = fopen(path, "r"); if(!f) return errno == ENOENT ? 0 : -1;
    size_t n = fread(buf, 1, sizeof buf - 1, f); int err = ferror(f); fclose(f); buf[n] = 0;
    if(err || n == sizeof buf - 1) return -1;
    if(!n) return 0;
    const char *first = strrchr(buf, '/'); first = first ? first + 1 : buf;
    int app = 0, iface = 0;
    for(size_t i = 0; i < n; ){
        const char *arg = buf + i, *base = strrchr(arg, '/'); base = base ? base + 1 : arg;
        if((i == 0 || (i == strlen(buf) + 1 && !strcmp(first, "busybox"))) && !strcmp(base, name)) app = 1;
        if(!strcmp(arg, "-iwlan0")) iface = 1;
        if(!strcmp(arg, "-i") && i + strlen(arg) + 1 < n && !strcmp(arg + strlen(arg) + 1, "wlan0")) iface = 1;
        i += strlen(arg) + 1;
    }
    return app && iface;
}
static int wifi_proc_find(const char *name, struct wifi_proc *p){
    DIR *d = opendir("/proc"); if(!d) return 0;
    struct dirent *e; int count = 0, parent; struct wifi_proc found = {0};
    while((e = readdir(d))){
        int pid = atoi(e->d_name); if(pid <= 0) continue;
        int match = wifi_proc_match(pid, name);
        if(match < 0){ count += 2; continue; }
        if(!match) continue;
        count++;
        if(wifi_proc_stat(pid, &found, &parent)) *p = found; else count += 2;
    }
    closedir(d); if(count != 1) memset(p, 0, sizeof *p); return count == 1;
}
/* Fail closed on unreadable proc data; multiple clients also forbid startup. */
static int wifi_dhcp_absent(void){
    DIR *d = opendir("/proc"); if(!d) return -1;
    struct dirent *e; int absent = 1;
    while((e = readdir(d))){
        int pid = atoi(e->d_name); if(pid <= 0) continue;
        int match = wifi_proc_match(pid, "udhcpc");
        if(match < 0) absent = -1;
        else if(match){ absent = 0; break; }
    }
    closedir(d); return absent;
}
static int wifi_proc_eq(struct wifi_proc a, struct wifi_proc b){ return a.pid && a.pid == b.pid && a.start == b.start; }
static int wifi_lease_signal_context(int sig);
static int wifi_dhcp_signal(struct wifi_proc expected, int sig){
    struct wifi_proc current = {0};
    return wifi_lease_signal_context(sig) && wifi_proc_find("udhcpc", &current) && wifi_proc_eq(expected, current) && kill(current.pid, sig) == 0;
}
/* udhcpc waits for its script. A live child or unreadable proc data blocks the barrier. */
static int wifi_dhcp_idle(struct wifi_proc p, int *sockets){
    char path[64], link[128]; int idle = 1; *sockets = 0;
    DIR *d = opendir("/proc"); if(!d) return 0;
    struct dirent *e; struct wifi_proc child; int parent;
    while((e = readdir(d))){
        int pid = atoi(e->d_name);
        if(pid <= 0) continue;
        errno = 0;
        if(!wifi_proc_stat(pid, &child, &parent)){
            if(errno != ENOENT && errno != ESRCH) idle = 0;
        } else if(parent == p.pid) idle = 0;
    }
    closedir(d); snprintf(path, sizeof path, "/proc/%d/fd", p.pid);
    d = opendir(path); if(!d) return 0;
    while((e = readdir(d))){
        if(e->d_name[0] == '.') continue;
        char fd[128]; snprintf(fd, sizeof fd, "%s/%.32s", path, e->d_name);
        ssize_t n = readlink(fd, link, sizeof link - 1); if(n < 0){ idle = 0; continue; }
        link[n] = 0; if(!strncmp(link, "socket:[", 8)) (*sockets)++;
    }
    closedir(d); return idle;
}
static int wifi_ipv4(void){
    int fd = socket(AF_INET, SOCK_DGRAM, 0); if(fd < 0) return -1;
    struct ifreq req; memset(&req, 0, sizeof req); strcpy(req.ifr_name, "wlan0");
    int flags = ioctl(fd, SIOCGIFFLAGS, &req);
    int up = flags == 0 && (req.ifr_flags & IFF_UP);
    int rc = ioctl(fd, SIOCGIFADDR, &req), err = errno; close(fd);
    if(!up) return -1;
    if(rc < 0) return err == EADDRNOTAVAIL ? 0 : -1;
    return ((struct sockaddr_in *)&req.ifr_addr)->sin_addr.s_addr != 0;
}
static void wifi_lease_observe(struct wifi_obs *o, const char *status){
    memset(o, 0, sizeof *o); o->id = -1; o->ip = wifi_ipv4();
    wifi_proc_find("wpa_supplicant", &o->wpa); wifi_proc_find("udhcpc", &o->dhcp);
    for(const char *l = status; l && *l; ){
        if(!strncmp(l, "id=", 3)) o->id = atoi(l + 3);
        if(!strncmp(l, "ssid=", 5)){
            const char *end = strchr(l + 5, '\n');
            if(wifi_ssid_decode(l + 5, end ? end - l - 5 : strlen(l + 5), o->ssid, sizeof o->ssid) < 0) o->ssid[0] = 0;
        }
        l = strchr(l, '\n'); if(l) l++;
    }
    o->allowed = !g_reco_hold && cfg_get_int("wifi_on", 1) && wifi_rfkill_blocked() == 0 &&
        o->wpa.pid && o->dhcp.pid && o->ip >= 0 && o->id >= 0 && o->ssid[0] &&
        (strstr(status, "\nwpa_state=COMPLETED\n") || !strncmp(status, "wpa_state=COMPLETED\n", 20));
    if(o->dhcp.pid) o->idle = wifi_dhcp_idle(o->dhcp, &o->sockets);
}
static int wifi_lease_matches(const struct wifi_obs *o){
    return wifi_proc_eq(o->wpa, g_lease.net.wpa) && wifi_proc_eq(o->dhcp, g_lease.net.dhcp) &&
        o->id == g_lease.net.id && !strcmp(o->ssid, g_lease.net.ssid) &&
        g_lease.intent == g_intent_epoch && g_lease.player == ipc_generation();
}
static int wifi_lease_signal_context(int sig){
    char status[2048]; run_cap(WCLI "status 2>/dev/null", status, sizeof status);
    struct wifi_obs o; wifi_lease_observe(&o, status);
    if(!o.allowed || !wifi_lease_matches(&o)) return 0;
    if(sig == SIGUSR2) g_lease.release_evidence |= o.ip == 1 || o.sockets || !o.idle;
    return sig != SIGUSR1 || (o.ip == 0 && o.idle && !o.sockets);
}
/* This step never starts DHCP or raises wlan0. Failure stays uncertified. */
static void wifi_lease_advance(const struct wifi_obs *o, uint32_t now){
    int same_owner = wifi_proc_eq(o->dhcp, g_lease.net.dhcp);
    if(o->dhcp.pid && !same_owner) g_lease.released = g_lease.release_evidence = 0;
    /* A release belongs to the DHCP process, even across disconnects or profiles.
     * Acquisition consumes it; only the unchanged transaction may resume. */
    if(same_owner && g_lease.release_evidence &&
       (g_lease.phase == LEASE_RELEASE || g_lease.phase == LEASE_FAILED) &&
       now != g_lease.since && o->ip == 0 && o->idle && !o->sockets)
        g_lease.released = 1;
    if(o->ip != 0 || !o->idle || o->sockets) g_lease.released = 0;
    if(!o->allowed){
        if(o->ip == 0 && !g_lease.epoch) g_lease.seen_empty = 1;
        if(g_lease.phase == LEASE_RELEASE || g_lease.phase == LEASE_ACQUIRE){
            g_lease.resume = g_lease.phase; g_lease.phase = LEASE_FAILED;
        }
        if(g_lease.epoch && o->dhcp.pid && !same_owner) g_lease.phase = LEASE_NONE;
        return;
    }
    if(g_lease.phase == LEASE_FAILED && g_lease.resume == LEASE_ACQUIRE &&
       wifi_lease_matches(o) && now - g_lease.since <= 30000){
        g_lease.phase = LEASE_ACQUIRE; g_lease.resume = LEASE_NONE;
    }
    if(!wifi_lease_matches(o) || g_lease.phase == LEASE_NONE ||
       (g_lease.phase == LEASE_FAILED && now - g_lease.since >= 1000)){
        int first = !g_lease.epoch && g_lease.seen_empty && o->ip == 0;
        g_lease.net = *o; g_lease.intent = g_intent_epoch; g_lease.player = ipc_generation();
        g_lease.epoch++; g_lease.since = now; g_lease.seen_empty = first; g_lease.resume = LEASE_NONE;
        if(first) g_lease.phase = LEASE_ACQUIRE;
        else if(g_lease.released){
            g_lease.seen_empty = 1;
            g_lease.phase = wifi_dhcp_signal(o->dhcp, SIGUSR1) ? LEASE_ACQUIRE : LEASE_FAILED;
            if(g_lease.phase == LEASE_ACQUIRE) g_lease.released = g_lease.release_evidence = 0;
        } else {
            g_lease.release_evidence |= o->ip == 1 || o->sockets || !o->idle;
            g_lease.phase = wifi_dhcp_signal(o->dhcp, SIGUSR2) ? LEASE_RELEASE : LEASE_FAILED;
        }
        return;
    }
    if(g_lease.phase == LEASE_READY){ if(!o->ip) g_lease.phase = LEASE_FAILED; return; }
    if(now - g_lease.since > 30000){ g_lease.phase = LEASE_FAILED; return; }
    if(g_lease.phase == LEASE_RELEASE){
        if(o->ip == 1 || o->sockets || !o->idle) g_lease.release_evidence = 1;
        if(g_lease.released){
            g_lease.seen_empty = 1; g_lease.since = now;
            g_lease.phase = wifi_dhcp_signal(o->dhcp, SIGUSR1) ? LEASE_ACQUIRE : LEASE_FAILED;
            if(g_lease.phase == LEASE_ACQUIRE) g_lease.released = g_lease.release_evidence = 0;
        }
    } else if(g_lease.phase == LEASE_ACQUIRE && g_lease.seen_empty && o->ip == 1 && o->idle)
        g_lease.phase = LEASE_READY;
}
static int wifi_lease_ready(const char *status){
    if(g_lease.phase != LEASE_READY) return 0;
    struct wifi_obs o; wifi_lease_observe(&o, status);
    return o.allowed && o.ip == 1 && o.idle && wifi_lease_matches(&o);
}
/* The action helper only records events, so a late helper can never signal DHCP. */
static void wifi_lease_events(void){
    if(g_lease_helper && waitpid(g_lease_helper, NULL, WNOHANG) == 0) return;
    g_lease_helper = 0;
    if(!g_lease_dir[0]){
        strcpy(g_lease_dir, "/tmp/diskos-wifi-XXXXXX");
        if(!mkdtemp(g_lease_dir)){ g_lease_dir[0] = 0; return; }
        char path[96]; snprintf(path, sizeof path, "%s/action", g_lease_dir);
        FILE *f = fopen(path, "w"); if(!f) return;
        fprintf(f, "#!/bin/sh\nprintf '%%s %%s\\n' \"$$\" \"$2\" >> %s/event\n", g_lease_dir);
        fclose(f); if(chmod(path, 0700)) return;
    }
    char path[96]; snprintf(path, sizeof path, "%s/action", g_lease_dir);
    pid_t owner = getpid(); g_lease_helper = fork();
    if(g_lease_helper == 0){
        if(prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != owner) _exit(127);
        int fd = open("/dev/null", O_RDWR);
        if(fd >= 0){ dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); if(fd > 2) close(fd); }
        execl("/usr/sbin/wpa_cli", "wpa_cli", "-i", "wlan0", "-a", path, (char *)NULL); _exit(127);
    }
    if(g_lease_helper < 0) g_lease_helper = 0;
}
/* Failed leases back off, except an acquisition still resumable in its window. */
static int wifi_lease_slow(void){
    return g_lease.phase == LEASE_FAILED && !g_conn_timer && !g_scan_timer &&
        !(g_lease.resume == LEASE_ACQUIRE && lv_tick_elaps(g_lease.since) <= 30000);
}
static void wifi_lease_step(void){
    static unsigned intent, player;
    static int want_prev = -1, lit_prev, screen_prev = -1;
    static uint32_t input_prev;
    int want_now = cfg_get_int("wifi_on", 1), lit = ui_screen_backlight_on(), screen = screen_current();
    unsigned gen = ipc_generation();
    /* Last input tick, read against one unchanged tick so it is exact. */
    uint32_t tick, idle;
    do { tick = lv_tick_get(); idle = lv_display_get_inactive_time(NULL); } while(lv_tick_get() != tick);
    uint32_t input = tick - idle;
    if(intent != g_intent_epoch || player != gen || want_prev != want_now ||
       (lit && !lit_prev) || screen_prev != screen || input != input_prev)
        wifi_lease_retry_reset();
    intent = g_intent_epoch; player = gen; want_prev = want_now;
    lit_prev = lit; screen_prev = screen; input_prev = input;
    if(!wifi_lease_slow()) g_lease_retry_ms = 100;
    else if(lv_tick_elaps(g_lease_check) < g_lease_retry_ms) return;
    g_lease_check = lv_tick_get();
    if(!cfg_get_int("wifi_on", 1)) {
        g_lease.phase = LEASE_FAILED; g_lease.resume = LEASE_NONE;
        if(g_lease_helper > 0){
            if(waitpid(g_lease_helper, NULL, WNOHANG) == 0) kill(g_lease_helper, SIGTERM);
            else g_lease_helper = 0;
        }
        if(screen_current() == SCR_WIFI_INFO) wifi_info_refresh();
        if(g_lease_retry_ms < 4000) g_lease_retry_ms *= 2;
        if(g_lease_retry_ms > 4000) g_lease_retry_ms = 4000;
        g_lease_settled = 1;
        return;
    }
    wifi_lease_events();
    char status[2048];
    run_cap(WCLI "status 2>/dev/null", status, sizeof status);
    struct wifi_obs o; wifi_lease_observe(&o, status); wifi_lease_advance(&o, lv_tick_get());
    g_lease.net.allowed = o.allowed && o.ip == 1 && o.idle;
    if(!o.allowed || g_lease.phase == LEASE_READY || g_lease.phase == LEASE_FAILED) g_lease_settled = 1;
    if(wifi_lease_slow()){
        g_lease_retry_ms *= 2;
        if(g_lease_retry_ms > 4000) g_lease_retry_ms = 4000;
    } else g_lease_retry_ms = 100;
    if(ui_screen_backlight_on()){
        const char *cur = o.allowed && g_lease.phase == LEASE_READY ? o.ssid : "";
        if(screen_current() == SCR_WIFI && !g_scan_timer && strcmp(cur, g_cur_ssid)) scan_fill();
        if(screen_current() == SCR_WIFI_INFO) wifi_info_refresh();
    }
}

/* Only failed leases back off; recovery cues make the next step due. */
static void wifi_lease_retry_reset(void){
    g_lease_retry_ms = 100; g_lease_check = lv_tick_get() - 100;
}

/* Append-only records preserve order. A partial final line is retried next loop. */
static int wifi_lease_consume(void){
    if(!g_lease_dir[0]) return 0;
    char path[96], event[128]; snprintf(path, sizeof path, "%s/event", g_lease_dir);
    FILE *f = fopen(path, "r"); if(!f) return 0;
    if(fseek(f, g_lease_offset, SEEK_SET)){ fclose(f); return 0; }
    int consumed = 0;
    while(fgets(event, sizeof event, f)){
        if(!strchr(event, '\n')) break;
        g_lease_offset = ftell(f);
        if(strstr(event, "DISCONNECTED")){
            g_lease.net.allowed = 0;
            if(g_lease.phase == LEASE_RELEASE || g_lease.phase == LEASE_ACQUIRE){
                g_lease.resume = g_lease.phase; g_lease.phase = LEASE_FAILED;
            }
        } else if(!strstr(event, "CONNECTED")) continue;
        consumed = 1;
        wifi_lease_retry_reset();
        wifi_lease_step();
    }
    fclose(f); return consumed;
}

void wifi_supervise(void){
    wifi_reconcile_poll();
    if(g_reco_hold) return;                         /* stock's radio decision is still being read: stay passive */
    static uint32_t lease_check;
    int event = wifi_lease_consume();
    if(!event && lv_tick_elaps(lease_check) >= 100 &&
       (g_lease.phase == LEASE_RELEASE || g_lease.phase == LEASE_ACQUIRE ||
        g_lease.phase == LEASE_FAILED)){
        lease_check = lv_tick_get(); wifi_lease_step();
    }
    static uint32_t last_check = 0;
    static int      first = 1;
    if(!first && lv_tick_elaps(last_check) < 5000) return;   /* check ~every 5s */
    first = 0; last_check = lv_tick_get();

    wifi_lease_step();
    int up   = wifi_radio_on();
    int want = cfg_get_int("wifi_on", 1);

    /* A surviving PID can have lost its control service. Allow startup time,
     * then replace only that daemon; wifi_shell preserves a surviving DHCP client. */
    static uint32_t control_failed_at;
    static unsigned control_epoch;
    static int control_failed;
    int ready = up && want ? wpa_ready() : 0;
    want = cfg_get_int("wifi_on", 1);
    if(control_epoch != g_intent_epoch || !up || !want || ready) control_failed = 0;
    control_epoch = g_intent_epoch;
    if(up && want && !ready){
        if(!control_failed){ control_failed = 1; control_failed_at = lv_tick_get(); }
        else if(lv_tick_elaps(control_failed_at) >= 30000){
            control_failed_at = lv_tick_get();
            if(system("killall -9 wpa_supplicant >/dev/null 2>&1") == 0){
                up = 0; control_failed = 0;
            }
        }
    }

    /* re-enable all nets + clear freq locks + persist once the supplicant is up, unless a
     * user connect is mid-flight (conn_poll_cb sanitizes on its outcome). */
    if(up && want && g_sanitize_pending && !g_conn_timer && wifi_sanitize_savenets()) g_sanitize_pending = 0;

    /* Probe first, then re-read intent: no stale ON decision after a bounded probe.
     * Restore only the interface for a surviving supplicant, leaving stock DHCP alone. */
    int blocked = wifi_rfkill_blocked();
    want = cfg_get_int("wifi_on", 1);
    if(up && want && blocked == 1)
        system("rfkill unblock wifi; ifconfig wlan0 up");
    if(!up && want){                        /* should be on but isn't -> (re)start */
        if(!g_wifi_last_start || lv_tick_elaps(g_wifi_last_start) > 30000){  /* 30s backoff */
            g_sanitize_pending = 1;         /* sanitize once this new instance is up */
            if(system("/usr/bin/wifi_up.sh >/dev/null 2>&1 &") == 0)
                g_wifi_last_start = lv_tick_get();
        }
    } else if(!want && (up || blocked != 1)){                 /* should be off but a late wifi_up won -> enforce off */
        system("/usr/bin/wifi_down.sh >/dev/null 2>&1");
    }
}

/* UI-thread snapshot of the lease observations: 1 connected, 0 offline, -1 not known yet
 * (radio decision pending, or no lease step has settled since start). */
int wifi_connectivity(void){
    if(!cfg_get_int("wifi_on", 1)) return 0;
    if(g_reco_hold || !g_lease_settled) return -1;
    return g_lease.net.allowed && g_lease.phase == LEASE_READY &&
           g_lease.intent == g_intent_epoch && g_lease.player == ipc_generation();
}

/* centred grey placeholder shown in the list area for non-network states */
/* a single status message (Scanning / off / empty) - centered in the list area */
static void list_msg(const char *m){
    if(!g_list) return;
    scan_stop();
    lv_obj_clean(g_list);
    lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *e = lv_label_create(g_list);
    lv_label_set_text(e, m);
    lv_obj_set_style_text_color(e, TC(TEXT_MUTED), 0);
    lv_obj_set_style_text_font(e, TF(UI_14), 0);
}
/* "Scanning" + a spinning refresh glyph, centered in the list area */
static void list_msg_scanning(void){
    if(!g_list) return;
    scan_stop();
    lv_obj_clean(g_list);
    lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *row = lv_obj_create(g_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_t *t = lv_label_create(row);
    lv_label_set_text(t, "Scanning");
    lv_obj_set_style_text_color(t, TC(TEXT_MUTED), 0);
    lv_obj_set_style_text_font(t, TF(UI_14), 0);
    lv_obj_t *ic = lv_label_create(row);
    lv_label_set_text(ic, LV_SYMBOL_REFRESH);
    lv_obj_set_size(ic, 24, 24);
    lv_obj_set_style_text_align(ic, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(ic, TC(TEXT_MUTED), 0);
    lv_obj_set_style_text_font(ic, TF(UI_14), 0);
    g_scan_icon = ic;
    lv_obj_set_style_transform_pivot_x(ic, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(ic, lv_pct(50), 0);
    lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, ic);
    lv_anim_set_exec_cb(&a, spin_anim_cb);
    lv_anim_set_values(&a, 0, 3600); lv_anim_set_time(&a, 900);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

/* default gateway from /proc/net/route (no external tools). */
static int get_gateway(char *out, int cap){
    out[0] = 0;
    FILE *f = fopen("/proc/net/route", "r");
    if(!f) return 0;
    char line[256]; int got = 0;
    if(!fgets(line, sizeof line, f)){ fclose(f); return 0; }   /* header */
    while(fgets(line, sizeof line, f)){
        char iface[32]; unsigned long dest, gw;
        if(sscanf(line, "%31s %lx %lx", iface, &dest, &gw) == 3 && dest == 0 && gw != 0){
            snprintf(out, cap, "%lu.%lu.%lu.%lu",
                     gw & 0xFF, (gw>>8)&0xFF, (gw>>16)&0xFF, (gw>>24)&0xFF);
            got = 1; break;
        }
    }
    fclose(f);
    return got;
}

/* The control protocol takes the bytes between the first and last quote
 * literally. There is no shell involved in profile editing. */
static void wpa_q(const char *v, char *out, int cap){
    snprintf(out, cap, "\"%s\"", v);
}
/* SSID as unquoted hex (wpa_supplicant reads an unquoted ssid value as hex bytes): any byte sequence,
 * including " and \, arrives exactly. */
static void wpa_hex(const char *v, char *out, int cap){
    int o = 0;
    for(; *v && o + 2 < cap; v++){ snprintf(out + o, 3, "%02x", (unsigned char)*v); o += 2; }
    out[o] = 0;
}

/* ---- connect completion poll (honest feedback) -------------------------
 * A connect is async (association + DHCP take seconds). Instead of re-listing
 * immediately and implying success, poll wpa_state + IP for up to 15s and toast
 * the real outcome. */
static uint32_t    g_conn_start;
static char        g_conn_ssid[64];
static int         g_conn_id = -1;
static struct wifi_proc g_conn_owner;

/* Did the supplicant give up on network `id` after auth failures? `list_networks` rows are
 * "id \t ssid \t bssid \t flags"; flags holds [TEMP-DISABLED] once it has temp-disabled the network
 * (the string is in this wpa_supplicant 2.9 build's flag table next to [CURRENT]/[DISABLED]). Only the
 * 4th field is checked; the ssid field is printf-encoded by wpa_cli but is never looked at here. */
static int wifi_net_temp_disabled(const char *list, int id){
    const char *l = strchr(list, '\n');           /* skip the header row */
    l = l ? l + 1 : "";
    while(*l){
        const char *nl = strchr(l, '\n'); size_t len = nl ? (size_t)(nl - l) : strlen(l);
        char *end; long v = strtol(l, &end, 10);
        if(end != l && *end == '\t' && v == id && (size_t)(end - l) < len){
            const char *f = end;                  /* skip ssid and bssid fields: the 3rd tab starts flags */
            int tabs = 0;
            while(f < l + len && tabs < 3){ if(*f == '\t') tabs++; if(tabs < 3) f++; }
            if(tabs == 3){
                f++;
                size_t rest = (size_t)(l + len - f);
                for(size_t i = 0; i + 15 <= rest; i++)
                    if(!strncmp(f + i, "[TEMP-DISABLED]", 15)) return 1;
            }
        }
        if(!nl) break;
        l = nl + 1;
    }
    return 0;
}

/* Explicit rollback is also safe after a daemon restart or socket recreation.
 * V2.57: RECONFIGURE at 0x4cf924-0x4cf944 replaces config at 0x4e6098-0x4e60ac.
 * A missing/FAIL reply leaves the barrier closed, even if reload actually ran. */
static int wifi_discard_attempt(void){
    if(!g_conn_dirty) return 1;
    g_conn_dirty = 2;
    struct wifi_proc owner = {0};
    int parent;
    int found = wifi_proc_stat(g_conn_owner.pid, &owner, &parent);
    /* Only process lifetime, never endpoint lifetime, proves unsaved RAM gone.
     * A replacement loads the saved file (stock wifi_up.sh:19,36). */
    if((found && !wifi_proc_eq(owner, g_conn_owner)) ||
       (!found && (errno == ENOENT || errno == ESRCH))){
        if(!wifi_attempt_clear()) return 0;
        g_conn_dirty = g_save_pending = 0;
        g_initial_reload = 0;
        return 1;
    }
    char out[64];
    run_cap(WCLI "reconfigure 2>/dev/null", out, sizeof out);
    if(!wifi_reply_ok(out) || !wifi_attempt_clear()) return 0;
    g_conn_dirty = g_save_pending = 0;
    g_initial_reload = 0;
    return 1;
}
static void conn_fail(const char *why){
    lv_timer_del(g_conn_timer); g_conn_timer = NULL;
    /* Restore the saved file; supervision can rejoin without persisting a typo. */
    g_sanitize_pending = 1;
    if(wifi_discard_attempt()) g_sanitize_pending = 0;
    ui_toast(tr(why));
    start_scan();
}
static void conn_poll_cb(lv_timer_t *t){
    (void)t;
    struct wifi_proc owner = {0};
    if(!wifi_proc_find("wpa_supplicant", &owner) || !wifi_proc_eq(owner, g_conn_owner)){
        conn_fail("Couldn't connect"); return;
    }
    char ss[64], ip[32];
    /* Association and witnessed DHCP must belong to this selected profile. */
    if(wifi_status(ss, sizeof ss, ip, sizeof ip) && ip[0] && g_lease.net.id == g_conn_id && !strcmp(ss, g_conn_ssid)){
        lv_timer_del(g_conn_timer); g_conn_timer = NULL;
        snprintf(g_cur_ssid, sizeof g_cur_ssid, "%s", g_conn_ssid);
        g_conn_dirty = 0;  /* witnessed success is the only commit path */
        g_save_pending = 1;
        g_sanitize_pending = 1;
        int saved = wifi_sanitize_savenets();
        if(saved) g_sanitize_pending = 0;
        ui_toast(saved ? tr("Connected") : tr("Wi-Fi settings not saved"));
        start_scan();                        /* refresh list so the joined net gets its checkmark */
        return;
    }
    char nl[2048]; run_cap(WCLI "list_networks 2>/dev/null", nl, sizeof nl);
    if(wifi_net_temp_disabled(nl, g_conn_id)){ conn_fail("Couldn't connect - check the password"); return; }
    if(lv_tick_elaps(g_conn_start) > 75000)   /* association plus two 30s DHCP stages */
        conn_fail("Couldn't connect");
}

/* wpa_cli 2.9 prints FAIL and still exits 0, so a step succeeded only if its reply is exactly "OK". */
static int wifi_reply_ok(const char *out){
    return out[0] == 'O' && out[1] == 'K' && (out[2] == 0 || out[2] == '\n' || out[2] == '\r');
}
static int wifi_step(const char *cmd, const char *step){
    char out[64]; wifi_edit_request(cmd, out, sizeof out);
    if(wifi_reply_ok(out)) return 0;
    fprintf(stderr, "wifi connect: %s failed (reply '%.20s')\n", step, out);
    return -1;
}
/* WPA passphrase: 8..63 bytes (wpa_supplicant's own limit; a 64-char value would have to be a hex PSK) */
static int wifi_psk_len_ok(const char *key){ size_t n = strlen(key); return n >= 8 && n <= 63; }

/* Write the profile into the supplicant and select it. Every step must reply OK; the first that
 * does not aborts BEFORE anything later runs (a half-built profile or an old saved key must never be
 * selected). Returns NULL on success, else the toast text. */
static const char *wifi_apply_net(int id, const char *ssid, const char *key){
    char cmd[512], qssid[160], qkey[300];
    wpa_hex(ssid, qssid, sizeof qssid);
    snprintf(cmd, sizeof cmd, "SET_NETWORK %d ssid %s", id, qssid);
    if(wifi_step(cmd, "set ssid")) return "Couldn't configure Wi-Fi";
    if(key && key[0]){
        wpa_q(key, qkey, sizeof qkey);
        /* set key_mgmt WPA-PSK BEFORE psk: a REUSED profile that was previously open still has
         * key_mgmt NONE, and setting only psk would leave it open-auth (connect fails w/ right pass). */
        snprintf(cmd, sizeof cmd, "SET_NETWORK %d key_mgmt WPA-PSK", id);
        if(wifi_step(cmd, "set key_mgmt")) return "Couldn't configure Wi-Fi";
        snprintf(cmd, sizeof cmd, "SET_NETWORK %d psk %s", id, qkey);
        if(wifi_step(cmd, "set psk")) return "Couldn't set Wi-Fi password";
    } else {
        snprintf(cmd, sizeof cmd, "SET_NETWORK %d key_mgmt NONE", id);
        if(wifi_step(cmd, "set open")) return "Couldn't configure Wi-Fi";
    }
    snprintf(cmd, sizeof cmd, "ENABLE_NETWORK %d", id);
    if(wifi_step(cmd, "enable_network")) return "Couldn't configure Wi-Fi";
    snprintf(cmd, sizeof cmd, "SELECT_NETWORK %d", id);
    if(wifi_step(cmd, "select_network")) return "Couldn't connect to Wi-Fi";
    return NULL;
}

/* ---- connect ------------------------------------------------------------ */
static void wifi_connect(const char *ssid, const char *key){
    wifi_lease_retry_reset();
    if(!wifi_list_ready()) return;
    char buf[2048];
    if(key && key[0] && !wifi_psk_len_ok(key)){ ui_toast("Password must be 8 to 63 characters"); return; }
    if(g_conn_timer){ lv_timer_del(g_conn_timer); g_conn_timer = NULL; }
    if(!wifi_discard_attempt()){ ui_toast(tr("Couldn't configure Wi-Fi")); return; }
    if(!wifi_initial_reload() || !wifi_proc_find("wpa_supplicant", &g_conn_owner) || !wifi_edit_open()){
        ui_toast(tr("Couldn't configure Wi-Fi")); return;
    }
    struct wifi_proc owner = {0};
    if(!wifi_proc_find("wpa_supplicant", &owner) || !wifi_proc_eq(owner, g_conn_owner)){
        wifi_edit_close(); ui_toast(tr("Couldn't configure Wi-Fi")); return;
    }
    int id = -1;
    int n = wifi_edit_request("LIST_NETWORKS", buf, sizeof buf);
    if(n <= 0 || n >= (int)sizeof buf - 1 || strncmp(buf, "network id", 10) || buf[n-1] != '\n'){
        wifi_edit_close(); ui_toast(tr("Couldn't configure Wi-Fi")); return;
    }
    char *l = strchr(buf, '\n'); if(l) l++;               /* skip the header row */
    while(l && *l){
        /* exact SSID match by literal-tab field split. sscanf("%d\t%63[^\t]") swallowed leading
         * whitespace in the SSID, so " Home" and "Home" compared equal - selecting one could
         * reconfigure the other saved profile. Split on the real tabs instead. */
        char *nl = strchr(l, '\n');
        char *t1 = strchr(l, '\t');                       /* end of id field */
        if(t1 && (!nl || t1 < nl)){
            char *sfld = t1 + 1;                          /* ssid field */
            char *t2 = strchr(sfld, '\t');                /* the SSID is always followed by a tab (bssid) */
            /* require a COMPLETE ssid field: a truncated capture ("17\tHom") must NOT match a shorter
             * prefix of a longer real SSID (e.g. select "Home" and reconfigure saved "HomeOffice"). */
            if(t2 && (!nl || t2 < nl) && wifi_ssid_field_eq(sfld, (int)(t2 - sfld), ssid)){
                id = atoi(l); break;
            }
        }
        l = nl ? nl + 1 : NULL;
    }
    int existing = id >= 0;
    if(!wifi_attempt_mark()){
        wifi_edit_close(); ui_toast(tr("Couldn't configure Wi-Fi")); return;
    }
    g_conn_dirty = 1;  /* includes an ADD whose reply may be lost */
    g_sanitize_pending = 1;
    if(id < 0){
        char idbuf[32];
        wifi_edit_request("ADD_NETWORK", idbuf, sizeof idbuf);
        char *end = idbuf;
        long nid = strtol(idbuf, &end, 10);
        if(end == idbuf || nid < 0){   /* empty / "FAIL\n" -> id would be 0 and clobber network 0 */
            fprintf(stderr, "wifi add_network failed: '%s'\n", idbuf);
            wifi_edit_close();
            g_sanitize_pending = !wifi_discard_attempt();
            ui_toast(tr("Couldn't add network"));
            return;
        }
        id = (int)nid;
    }
    g_intent_epoch++;                         /* selection invalidates any earlier transaction */
    /* A saved profile selected without a typed key keeps its security settings. */
    const char *why;
    if(!key && existing){
        char cmd[64];
        snprintf(cmd, sizeof cmd, "SELECT_NETWORK %d", id);
        why = wifi_step(cmd, "select_network") ? "Couldn't connect to Wi-Fi" : NULL;
    }else why = wifi_apply_net(id, ssid, key);
    wifi_edit_close();
    if(why){ g_sanitize_pending = !wifi_discard_attempt(); ui_toast(tr(why)); return; }
    if(g_conn_timer){ lv_timer_del(g_conn_timer); g_conn_timer = NULL; }
    /* don't claim success yet - poll for the real outcome (assoc + DHCP). */
    snprintf(g_conn_ssid, sizeof g_conn_ssid, "%s", ssid);
    g_conn_id = id;
    ui_toast(tr("Connecting..."));
    g_conn_start = lv_tick_get();
    g_conn_timer = lv_timer_create(conn_poll_cb, 1000, NULL);
}

static void psk_done(const char *text){
    if(text && text[0]) wifi_connect(g_pending_ssid, text);
}

/* ---- details screen (SCR_WIFI_INFO) ------------------------------------- */
static lv_obj_t *g_info_values[7];
static unsigned g_info_count;
static void info_row(const char *key, const char *val){
    lv_obj_t *r = lv_obj_create(g_info_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 250, 40);
    lv_obj_set_style_radius(r, 8, 0);
    lv_obj_set_style_bg_color(r, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_50, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *k = lv_label_create(r);
    lv_label_set_text(k, tr(key));
    lv_obj_set_pos(k, 12, 11);
    lv_obj_set_style_text_font(k, TF(UI_14), 0);
    lv_obj_set_style_text_color(k, TC(TEXT_MUTED), 0);
    lv_obj_t *v = lv_label_create(r);
    g_info_values[g_info_count++] = v;
    lv_label_set_text(v, val && val[0] ? val : "-");
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(v, 110, 11); lv_obj_set_size(v, 128, 18);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(v, TF(USER_14), 0);   /* "Network" value = SSID: Cyrillic/CJK-capable (issue #3) */
    lv_obj_set_style_text_color(v, TC(TEXT_PRIMARY), 0);
}
static void info_back_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) screen_back(); }

void wifi_info_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    ui_header_cb(root, "Network", info_back_cb);   /* shared header */
    g_info_list = lv_obj_create(root);
    lv_obj_remove_style_all(g_info_list);
    lv_obj_set_pos(g_info_list, 55, 84); lv_obj_set_size(g_info_list, 250, 230);
    lv_obj_set_style_pad_bottom(g_info_list, 44, 0);   /* last row scrolls clear of the round bottom bezel */
    lv_obj_set_style_bg_opa(g_info_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_row(g_info_list, 8, 0);
    lv_obj_set_flex_flow(g_info_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_info_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(g_info_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_info_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(g_info_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
}

/* A tappable destructive-action row appended to the details list (Forget). */
static void info_action_row(const char *label, lv_event_cb_t cb){
    lv_obj_t *r = lv_button_create(g_info_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 250, 44);
    lv_obj_set_style_radius(r, 8, 0);
    lv_obj_set_style_bg_color(r, TC(DANGER_SURFACE), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, TC(DANGER_SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    ui_on(r, cb, LV_EVENT_CLICKED, NULL, "wifi.cb", UI_CORE);
    lv_obj_t *t = lv_label_create(r);
    lv_label_set_text(t, label); lv_obj_center(t);
    lv_obj_set_style_text_font(t, TF(UI_14), 0);
    lv_obj_set_style_text_color(t, TC(STATUS_DANGER), 0);
}
static char g_info_ssid[64];   /* the exact SSID shown on the details screen (retained for Forget) */
/* Remove EVERY saved network whose SSID EXACTLY equals `ssid`, then persist. 1 if any removed.
 * list_networks is TAB-separated (id\tssid\tbssid\tflags); we split on literal tabs and compare the
 * exact ssid field - NOT sscanf "%[^\t]" after a "\t", which would swallow a leading space so " Home"
 * and "Home" collide and Forget could delete the wrong network. */
static int wifi_remove_ssid(const char *ssid){
    if(g_conn_timer){ lv_timer_del(g_conn_timer); g_conn_timer = NULL; }
    if(g_conn_dirty){
        g_sanitize_pending = !wifi_discard_attempt();
        return 0;  /* cancellation only: a second explicit Forget may delete */
    }
    wifi_lease_retry_reset();
    if(!wpa_ready() || !ssid || !ssid[0] || !wifi_initial_reload()) return 0;
    if(!wifi_edit_open()) return 0;
    char buf[4096];
    int n = wifi_edit_request("LIST_NETWORKS", buf, sizeof buf);
    if(n <= 0 || n >= (int)sizeof buf - 1 || strncmp(buf, "network id", 10) || buf[n-1] != '\n'){
        wifi_edit_close(); return 0;
    }
    char *l = strchr(buf, '\n'); if(l) l++;               /* skip the header row */
    int removed = 0, failed = 0;
    while(l && *l){
        char *nl = strchr(l, '\n');
        char *t1 = strchr(l, '\t');                       /* end of id field */
        if(t1 && (!nl || t1 < nl)){
            char *sfld = t1 + 1;                           /* ssid field start */
            char *t2 = strchr(sfld, '\t');                 /* end of ssid field (always a tab: bssid follows) */
            /* require a COMPLETE ssid field so a truncated capture can't match a shorter prefix of a longer real SSID. */
            if(t2 && (!nl || t2 < nl) && wifi_ssid_field_eq(sfld, (int)(t2 - sfld), ssid)){
                int nid = atoi(l);
                /* wpa_cli prints OK/FAIL and exits 0 either way, so check its output, not the
                 * shell rc - otherwise a failed remove would still report "forgotten". */
                char cmd[80]; snprintf(cmd, sizeof cmd, "REMOVE_NETWORK %d", nid);
                char out[64]; wifi_edit_request(cmd, out, sizeof out);
                if(strstr(out, "OK")) removed = 1; else failed = 1;   /* a duplicate-SSID profile we could not remove -> report failure */
            }
        }
        l = nl ? nl + 1 : NULL;
    }
    if(removed){
        char so[64]; wifi_edit_request("SAVE_CONFIG", so, sizeof so);
        if(!strstr(so, "OK")) failed = 1;   /* removed from the running config but not persisted (it returns on reboot) */
    }
    wifi_edit_close();
    return removed && !failed;   /* success only when something was removed AND nothing failed */
}
static void wifi_forget_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    ui_toast(wifi_remove_ssid(g_info_ssid) ? "Network forgotten" : "Couldn't forget network");
    screen_return_to(SCR_WIFI);
    wifi_open();   /* back to the list + fresh scan */
}
/* Keep the rows and their geometry alive while values change under a finger. */
static void info_value(unsigned row, const char *value){
    const char *text = value && value[0] ? value : "-";
    if(strcmp(lv_label_get_text(g_info_values[row]), text)) lv_label_set_text(g_info_values[row], text);
}
static void wifi_info_refresh(void){
    if(!g_info_list || !g_info_count) return;
    char ssid[64] = "", ip[32] = "", buf[2048], val[64], gw[32];
    int on = cfg_get_int("wifi_on", 1);
    int ready = on && wifi_status(ssid, sizeof ssid, ip, sizeof ip);
    if(!ready) ip[0] = 0;
    /* The action always belongs to the network opened, even after a roam. */
    int same = !strcmp(ssid, g_info_ssid);
    info_value(0, g_info_ssid);
    if(!same || !on){
        for(unsigned i = 1; i < 7; i++) info_value(i, "");
        return;
    }
    info_value(1, ip);
    info_value(2, ready && get_gateway(gw, sizeof gw) ? gw : "");
    run_cap(WCLI "signal_poll 2>/dev/null", buf, sizeof buf);
    val[0] = 0;
    char *p = strstr(buf, "RSSI=");
    if(p){ int r; if(sscanf(p+5, "%d", &r)==1){ snprintf(val,sizeof val,"%d dBm", r); } }
    info_value(3, val);
    run_cap(WCLI "status 2>/dev/null", buf, sizeof buf);
    val[0] = 0;
    p = strstr(buf, "key_mgmt=");
    if(p) sscanf(p+9, "%63[^\n]", val);
    info_value(4, strstr(val,"NONE") ? tr("Open") : val);
    val[0] = 0;
    p = strstr(buf, "\nfreq=");
    if(p){ int fr; if(sscanf(p+6,"%d",&fr)==1){ snprintf(val,sizeof val,"%d MHz", fr); } }
    info_value(5, val);
    val[0] = 0;
    p = strstr(buf, "bssid=");
    if(p) sscanf(p+6, "%63[^\n]", val);
    info_value(6, val);
}
void wifi_info_open(void){
    wifi_lease_retry_reset();
    char ip[32]; wifi_status(g_info_ssid, sizeof g_info_ssid, ip, sizeof ip);
    lv_obj_clean(g_info_list); g_info_count = 0;
    info_row("Network", g_info_ssid); info_row("IP Address", "");
    info_row("Router", ""); info_row("Signal", ""); info_row("Security", "");
    info_row("Frequency", ""); info_row("BSSID", "");
    info_action_row(tr("Forget This Network"), wifi_forget_cb);
    wifi_info_refresh(); screen_show(SCR_WIFI_INFO);
}

/* ---- scan list ---------------------------------------------------------- */
static void row_free_cb(lv_event_t *e){ free(lv_obj_get_user_data(lv_event_get_target(e))); }
static void net_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    lv_obj_t *row = lv_event_get_target(e);
    const char *ssid = (const char*)lv_obj_get_user_data(row);
    intptr_t flags = (intptr_t)lv_event_get_user_data(e);
    int secured   = flags & 1;
    int connected = flags & 2;
    if(!ssid || !ssid[0]) return;
    if(!wifi_list_ready()) return;
    if(connected){
        wifi_info_open();                 /* tap the joined network -> details */
    } else if(secured){
        snprintf(g_pending_ssid, sizeof g_pending_ssid, "%s", ssid);
        char prompt[80]; snprintf(prompt, sizeof prompt, "Password for %s", ssid);
        kbinput_open_password(prompt, "", psk_done);   /* masked entry; prompt, not the bare SSID */
    } else {
        wifi_connect(ssid, NULL);
    }
}

/* signal-strength bars: RSSI(dBm) -> 1..4 filled bars, bottom-aligned. */
static void add_signal_bars(lv_obj_t *parent, int x, int y, int sig){
    int bars = sig >= -55 ? 4 : sig >= -67 ? 3 : sig >= -78 ? 2 : 1;
    static const int h[4] = {5, 9, 13, 17};
    for(int i=0;i<4;i++){
        lv_obj_t *b = lv_obj_create(parent);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 4, h[i]);
        lv_obj_set_pos(b, x + i*6, y + (17 - h[i]));
        lv_obj_set_style_radius(b, 1, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, (i < bars ? TC(SIGNAL_ACTIVE) : TC(SIGNAL_INACTIVE)), 0);
    }
}

static void add_net_row(const char *ssid, int signal, int secured, int connected){
    lv_obj_t *r = lv_button_create(g_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 268, 46);
    lv_obj_set_style_radius(r, 8, 0);
    lv_obj_set_style_bg_color(r, (connected ? TC(CONNECTED_SURFACE) : TC(SURFACE)), 0);
    lv_obj_set_style_bg_opa(r, connected ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_bg_color(r, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    char *dup = strdup(ssid);
    lv_obj_set_user_data(r, dup);
    intptr_t flags = (secured?1:0) | (connected?2:0);
    ui_on(r, net_cb, LV_EVENT_CLICKED, (void*)flags, "wifi.net", UI_CORE);
    lv_obj_add_event_cb(r, row_free_cb, LV_EVENT_DELETE, NULL);

    int tx = 12;
    if(connected){
        lv_obj_t *ck = lv_label_create(r);
        lv_label_set_text(ck, LV_SYMBOL_OK);
        lv_obj_set_pos(ck, 12, 15);
        lv_obj_set_style_text_font(ck, TF(UI_14), 0);
        lv_obj_set_style_text_color(ck, TC(STATUS_INFO), 0);
        tx = 34;
    }
    lv_obj_t *t = lv_label_create(r);
    lv_label_set_text(t, ssid);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(t, tx, 13); lv_obj_set_size(t, 190 - tx, 20);
    lv_obj_set_style_text_font(t, TF(USER_16), 0);   /* SSIDs are user data: Cyrillic/CJK-capable (issue #3) */
    lv_obj_set_style_text_color(t, TC(TEXT_PRIMARY), 0);

    if(secured){
        /* drawn padlock (no lock glyph exists in the fonts): a shackle loop with the body covering
         * its lower half, muted grey, in the clear lane left of the signal bars. */
        lv_obj_t *shk = lv_obj_create(r);            /* shackle: border-only circle, lower half hidden by body */
        lv_obj_remove_style_all(shk);
        lv_obj_clear_flag(shk, LV_OBJ_FLAG_CLICKABLE);   /* decorative: don't steal taps from the row */
        lv_obj_set_size(shk, 8, 8); lv_obj_set_pos(shk, 217, 15);
        lv_obj_set_style_radius(shk, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(shk, 2, 0);
        lv_obj_set_style_border_color(shk, TC(TEXT_MUTED), 0);
        lv_obj_t *body = lv_obj_create(r);           /* body: rounded rect */
        lv_obj_remove_style_all(body);
        lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(body, 10, 8); lv_obj_set_pos(body, 216, 20);
        lv_obj_set_style_radius(body, 2, 0);
        lv_obj_set_style_bg_color(body, TC(TEXT_MUTED), 0);
        lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    }
    add_signal_bars(r, 232, 14, signal);
}

/* one de-duplicated network of a scan */
struct wnet { char ssid[64]; int sig; int secured; int connected; };
#define WNET_MAX 40

/* Parse `wpa_cli scan_results` (bssid \t freq \t signal \t flags \t ssid) into out[] (max WNET_MAX).
 * SSIDs are printf-encoded by wpa_cli and are decoded to real bytes first (wifi_ssid_decode), so names
 * compare, dedupe, display and connect as what they are; undecodable/over-long/hidden rows are skipped.
 * Only newline-terminated rows count: a capture cut off mid-row (buffer full) loses its last row rather
 * than showing a partial name. Access points with the same SSID collapse into one row at the first
 * one's position, showing the strongest signal (duplicates are merged BEFORE the row cap applies).
 * The connected network (cur, may be "") is always row 0; the rest keep scan order. If cur is not in the
 * scan it is still shown first, using cur_rssi (dBm or WNET_NO_RSSI -> weakest bars) and cur_secured. */
#define WNET_NO_RSSI (-1000)
static int wifi_scan_order(char *buf, const char *cur, int cur_rssi, int cur_secured, struct wnet *out){
    int n = 0;
    char *l = strchr(buf, '\n'); if(l) l++;   /* skip header */
    while(l && *l){
        char *nl = strchr(l, '\n');
        if(!nl) break;                         /* unterminated final row: truncated capture, discard */
        *nl = 0;
        char *f[5] = {0,0,0,0,0}; int nf = 0; char *p = l;
        f[nf++] = p;
        while(nf < 5 && (p = strchr(p, '\t'))){ *p++ = 0; f[nf++] = p; }
        char ssid[64];
        if(nf == 5 && f[4][0] && wifi_ssid_decode(f[4], (int)strlen(f[4]), ssid, sizeof ssid) > 0){
            int sig = atoi(f[2]), dup = -1;
            for(int i=0;i<n;i++) if(!strcmp(out[i].ssid, ssid)){ dup = i; break; }
            if(dup >= 0){
                if(sig > out[dup].sig) out[dup].sig = sig;
            } else if(n < WNET_MAX){
                snprintf(out[n].ssid, sizeof out[n].ssid, "%s", ssid);
                out[n].sig = sig;
                out[n].secured = (strstr(f[3], "WPA") || strstr(f[3], "PSK") || strstr(f[3], "WEP")) != 0;
                out[n].connected = 0;
                n++;
            }
        }
        l = nl + 1;
    }
    if(!cur || !cur[0]) return n;
    int at = -1;
    for(int i=0;i<n;i++) if(!strcmp(out[i].ssid, cur)){ at = i; break; }
    struct wnet c;
    if(at >= 0){ c = out[at]; memmove(&out[1], &out[0], sizeof(struct wnet) * at); }
    else{
        memset(&c, 0, sizeof c);
        snprintf(c.ssid, sizeof c.ssid, "%s", cur);
        c.sig = (cur_rssi == WNET_NO_RSSI) ? -100 : cur_rssi;
        c.secured = cur_secured;
        if(n >= WNET_MAX) n--;                 /* keep the cap: drop the last row */
        memmove(&out[1], &out[0], sizeof(struct wnet) * n);
        n++;
    }
    c.connected = 1;
    out[0] = c;
    return n;
}

/* RSSI from `signal_poll` output: a complete signed integer after "RSSI=", else WNET_NO_RSSI */
static int wifi_parse_rssi(const char *b){
    const char *r = strstr(b, "RSSI=");
    if(!r) return WNET_NO_RSSI;
    r += 5;
    char *end; long v = strtol(r, &end, 10);
    if(end == r || (*end && *end != '\n' && *end != '\r') || v > 0 || v < -200) return WNET_NO_RSSI;
    return (int)v;
}
/* is the current link encrypted, from `status` output? (key_mgmt=NONE -> open; absent -> unknown, treated open) */
static int wifi_status_secured(const char *st){
    for(const char *l = st; l && *l; ){
        if(!strncmp(l, "key_mgmt=", 9)){
            const char *k = l + 9;
            return !(!strncmp(k, "NONE", 4) && (k[4] == 0 || k[4] == '\n' || k[4] == '\r'));
        }
        l = strchr(l, '\n'); if(l) l++;
    }
    return 0;
}
static int wifi_cur_rssi(void){
    char b[512];
    if(!run_cap(WCLI "signal_poll 2>/dev/null", b, sizeof b)) return WNET_NO_RSSI;
    return wifi_parse_rssi(b);
}

/* A missing service says nothing about the saved profiles. Only the active page
 * owns readiness polling and list messages (password completion can run elsewhere). */
static int wifi_list_ready(void){
    const char *msg = NULL;
    if(!cfg_get_int("wifi_on", 1)) msg = "Wi-Fi is off";
    else if(g_reco_hold || wifi_rfkill_blocked() == 1 || !wpa_ready()) msg = "Starting Wi-Fi...";
    if(!msg) return 1;
    if(screen_current() == SCR_WIFI && g_list){
        list_msg(msg);
        if(cfg_get_int("wifi_on", 1) && !g_radio_timer)
            g_radio_timer = lv_timer_create(radio_on_poll_cb, 2500, NULL);
    } else ui_toast(msg);
    return 0;
}

static void scan_fill(void){
    if(screen_current() != SCR_WIFI || !g_list || !wifi_list_ready()) return;
    scan_stop();
    lv_obj_clean(g_list);
    lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);  /* rows top-aligned */
    char cs[64], cip[32];
    if(!wifi_status(cs, sizeof cs, cip, sizeof cip)) cs[0]=0;
    snprintf(g_cur_ssid, sizeof g_cur_ssid, "%s", cs);

    char *buf = run_cap_all(WCLI "scan_results 2>/dev/null", 1 << 20);
    if(!buf){ list_msg("Couldn't scan Wi-Fi"); return; }
    if(!wifi_list_ready()){ free(buf); return; }
    struct wnet nets[WNET_MAX];
    int rssi = WNET_NO_RSSI, sec = 0;
    if(g_cur_ssid[0]){
        char st[2048]; run_cap(WCLI "status 2>/dev/null", st, sizeof st);
        sec = wifi_status_secured(st);
        rssi = wifi_cur_rssi();
    }
    int n = wifi_scan_order(buf, g_cur_ssid, rssi, sec, nets);
    free(buf);
    for(int i=0;i<n;i++) add_net_row(nets[i].ssid, nets[i].sig, nets[i].secured, nets[i].connected);
    if(n == 0) list_msg("No networks found");
}

static void scan_timer_cb(lv_timer_t *t){
    if(t != g_scan_timer) return;
    g_scan_timer = NULL; lv_timer_del(t);
    if(screen_current() == SCR_WIFI) scan_fill();
}
static void start_scan(void){
    wifi_lease_retry_reset();
    if(screen_current() != SCR_WIFI) return;
    if(g_scan_timer){ lv_timer_del(g_scan_timer); g_scan_timer = NULL; }
    if(!wifi_list_ready()) return;
    if(g_radio_timer){ lv_timer_del(g_radio_timer); g_radio_timer = NULL; }
    /* wpa_cli returns non-zero when it can't reach the control socket / issue the scan; surface
     * that honestly instead of letting scan_fill() report a false "No networks found". */
    if(system("" WCLI "scan >/dev/null 2>&1") != 0){ list_msg("Couldn't scan Wi-Fi"); return; }
    list_msg_scanning();
    if(g_scan_timer) lv_timer_del(g_scan_timer);
    g_scan_timer = lv_timer_create(scan_timer_cb, 2500, NULL);
    lv_timer_set_repeat_count(g_scan_timer, 1);
}

/* ---- radio toggle ------------------------------------------------------- */
/* The readiness timer belongs only to the Wi-Fi page. Detach before start_scan:
 * it may schedule a replacement if the service is still unavailable. */
static void radio_on_poll_cb(lv_timer_t *t){
    if(t != g_radio_timer) return;
    g_radio_timer = NULL; lv_timer_del(t);
    if(screen_current() == SCR_WIFI) start_scan();
}
/* The one Wi-Fi power path: the page's switch and the Settings > Network row both call it. An explicit choice ends any
 * radio reconciliation hold; the keepalive enforces the saved intent. */
int wifi_power_pending(void){ return g_radio_timer != NULL; }   /* a start made here is still waiting for the radio */
void wifi_power_set(int on){
    wifi_lease_retry_reset();
    g_intent_epoch++;                            /* an explicit choice: ends any radio reconciliation hold */
    if(on){
        cfg_set_int("wifi_on", 1);               /* persist intent -> keepalive keeps it up */
        if(!g_wifi_last_start || lv_tick_elaps(g_wifi_last_start) > 30000){
            if(system("/usr/bin/wifi_up.sh >/dev/null 2>&1 &") == 0)
                g_wifi_last_start = lv_tick_get();
        }
        g_sanitize_pending = 1;                  /* sanitize this instance once it's up */
        if(screen_current() == SCR_WIFI){
            list_msg("Starting Wi-Fi...");
            if(!g_radio_timer) g_radio_timer = lv_timer_create(radio_on_poll_cb, 2500, NULL);
        }
    } else {
        cfg_set_int("wifi_on", 0);               /* set OFF before down so keepalive won't resurrect */
        if(g_radio_timer){ lv_timer_del(g_radio_timer); g_radio_timer = NULL; }
        if(g_scan_timer){ lv_timer_del(g_scan_timer); g_scan_timer = NULL; }  /* cancel a pending scan, else scan_fill overwrites "Wi-Fi is off" with stale networks */
        if(g_conn_timer){ lv_timer_del(g_conn_timer); g_conn_timer = NULL; }
        wifi_discard_attempt();
        wifi_lease_step();
        system("/usr/bin/wifi_down.sh >/dev/null 2>&1");
        /* Do not infer daemon death from the script exit code. */
        g_wifi_last_start = 0; /* a later explicit ON starts a new attempt */
        if(screen_current() == SCR_WIFI) list_msg("Wi-Fi is off");
    }
    /* a choice made on the Settings row also shows on the page's switch */
    if(g_sw){ if(on) lv_obj_add_state(g_sw, LV_STATE_CHECKED); else lv_obj_clear_state(g_sw, LV_STATE_CHECKED); }
}
static void sw_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_VALUE_CHANGED) return;
    wifi_power_set(lv_obj_has_state(g_sw, LV_STATE_CHECKED));
}

/* Quick Settings tile short-press: flip the radio + persist intent, no screen-specific UI.
 * Mirrors sw_cb's radio actions (keepalive enforces the intent). Returns the new state. */
int wifi_toggle(void){
    g_intent_epoch++;                        /* an explicit choice: ends any radio reconciliation hold */
    int on = !wifi_control_on();
    cfg_set_int("wifi_on", on);
    if(on){
        if(!g_wifi_last_start || lv_tick_elaps(g_wifi_last_start) > 30000){
            if(system("/usr/bin/wifi_up.sh >/dev/null 2>&1 &") == 0)
                g_wifi_last_start = lv_tick_get();
        }
        g_sanitize_pending = 1;              /* sanitize this instance once it's up */
    } else {
        if(g_radio_timer){ lv_timer_del(g_radio_timer); g_radio_timer = NULL; }
        if(g_scan_timer){ lv_timer_del(g_scan_timer); g_scan_timer = NULL; }  /* cancel any pending radio/scan work on the open Wi-Fi screen */
        if(g_conn_timer){ lv_timer_del(g_conn_timer); g_conn_timer = NULL; }
        wifi_discard_attempt();
        wifi_lease_step();
        system("/usr/bin/wifi_down.sh >/dev/null 2>&1");
        /* Do not infer daemon death from the script exit code. */
        g_wifi_last_start = 0; /* a later explicit ON starts a new attempt */
    }
    /* keep the Wi-Fi screen's switch in sync so it reflects reality when opened later */
    if(g_sw){ if(on) lv_obj_add_state(g_sw, LV_STATE_CHECKED); else lv_obj_clear_state(g_sw, LV_STATE_CHECKED); }
    if(screen_current() == SCR_WIFI){
        if(on){
            list_msg("Starting Wi-Fi...");
            if(!g_radio_timer) g_radio_timer = lv_timer_create(radio_on_poll_cb, 2500, NULL);
        } else list_msg("Wi-Fi is off");
    }
    return on;
}

static void rescan_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) start_scan(); }
static void back_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) screen_back(); }

void wifi_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    ui_header_cb(root, "Wi-Fi", back_cb);   /* shared header */

    /* control row: pill with [label + switch]; scan button OUTSIDE, to the
     * right of the switch (still inside the round screen). */
    lv_obj_t *trow = lv_obj_create(root);
    lv_obj_remove_style_all(trow);
    lv_obj_set_pos(trow, 50, 64); lv_obj_set_size(trow, 208, 48);
    lv_obj_set_style_radius(trow, 12, 0);
    lv_obj_set_style_bg_color(trow, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(trow, LV_OPA_70, 0);
    lv_obj_clear_flag(trow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tl = lv_label_create(trow);
    lv_label_set_text(tl, "Wi-Fi");
    lv_obj_set_pos(tl, 16, 14);
    lv_obj_set_style_text_font(tl, TF(UI_16), 0);
    lv_obj_set_style_text_color(tl, TC(TEXT_PRIMARY), 0);

    g_sw = lv_switch_create(trow);
    lv_obj_set_size(g_sw, 46, 24);
    lv_obj_set_ext_click_area(g_sw, 10);
    lv_obj_align(g_sw, LV_ALIGN_RIGHT_MID, -14, 0);
    ui_on(g_sw, sw_cb, LV_EVENT_VALUE_CHANGED, NULL, "wifi.sw.value", UI_CORE);

    lv_obj_t *rb = lv_button_create(root);
    lv_obj_remove_style_all(rb);
    lv_obj_set_pos(rb, 266, 72); lv_obj_set_size(rb, 36, 32);
    lv_obj_set_ext_click_area(rb, 8);
    lv_obj_set_style_radius(rb, 10, 0);
    lv_obj_set_style_bg_color(rb, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(rb, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(rb, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    ui_on(rb, rescan_cb, LV_EVENT_CLICKED, NULL, "wifi.rescan", UI_CORE);
    lv_obj_t *rl = lv_label_create(rb);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_font(rl, TF(UI_16), 0);
    lv_obj_set_style_text_color(rl, TC(TEXT_SECONDARY), 0);
    lv_obj_center(rl);

    /* network list (carries connected/off/scanning state itself) */
    g_list = lv_obj_create(root);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_pos(g_list, 40, 126); lv_obj_set_size(g_list, 280, 192);
    lv_obj_set_style_pad_bottom(g_list, 44, 0);   /* last row scrolls clear of the round bottom bezel */
    lv_obj_set_style_bg_opa(g_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_row(g_list, 6, 0);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(g_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
}

/* called from settings when the Wi-Fi row is tapped */
void wifi_open(void){
    wifi_lease_retry_reset();
    if(g_sw){
        if(wifi_control_on()) lv_obj_add_state(g_sw, LV_STATE_CHECKED);
        else                lv_obj_clear_state(g_sw, LV_STATE_CHECKED);
    }
    screen_show(SCR_WIFI);
    start_scan();
}

/* exec preserves the parent PID, so PDEATHSIG cannot retire this helper. */
int wifi_prepare_reload(void){
    g_sanitize_pending = 1;
    if(g_conn_timer){ lv_timer_del(g_conn_timer); g_conn_timer = NULL; }
    if(!wifi_discard_attempt()) return 0;
    if(g_lease_helper > 0){
        struct timespec began, now;
        if(clock_gettime(CLOCK_MONOTONIC, &began)) return 0;
        int sent = 0;
        for(;;){
            pid_t reaped = waitpid(g_lease_helper, NULL, WNOHANG);
            if(reaped == g_lease_helper || (reaped < 0 && errno == ECHILD)){
                g_lease_helper = 0;
                break;
            }
            if(reaped < 0 && errno != EINTR) return 0;
            if(!sent){
                if(kill(g_lease_helper, SIGKILL) < 0 && errno != ESRCH) return 0;
                sent = 1;
            }
            /* Even SIGKILL may wait on the kernel. Keep the PID and forbid exec
             * until a later call confirms reaping; EINTR shares this deadline. */
            if(clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
            long ms = (now.tv_sec - began.tv_sec) * 1000 + (now.tv_nsec - began.tv_nsec) / 1000000;
            if(ms >= 1000) return 0;
            usleep(10000);
        }
    }
    return 1;
}
