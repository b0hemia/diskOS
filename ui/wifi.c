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
#include <pthread.h>
#include <stdatomic.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include "ui_wake.h"
#include "procrun.h"

/* Wi-Fi settings (SCR_WIFI) + a details screen (SCR_WIFI_INFO).
 * Radio on/off reuses the stock /usr/bin/wifi_up.sh / wifi_down.sh (rfkill +
 * wpa_supplicant + udhcpc).  Scan/connect drive the running supplicant via
 * native supplicant control sockets. Connecting writes the network in and `save_config`s it
 * to /usr/data/wpa_supplicant.conf (update_config=1), so it persists.
 * Round-screen-aware: the top corners stay clear (only back chevron + centred
 * title); the on/off switch + scan button share one row in the wide centre
 * band.  The list itself carries state (connected checkmark row / "Wi-Fi is off" /
 * "Scanning" / "No networks") so there is no redundant status line.
 * NB: connecting to a DIFFERENT AP drops the laptop deploy link - recover over
 * serial if needed. */

#define WCLI "/usr/sbin/wpa_cli -i wlan0 "
struct wifi_proc { int pid; unsigned long long start; };
struct wifi_obs { struct wifi_proc wpa, dhcp; int id, ip, idle, sockets, allowed; char ssid[64]; };

#define WIFI_ATTEMPT_MARKER "/tmp/diskos-wifi-unsaved"
#define WIFI_SAVED_CONFIG "/usr/data/wpa_supplicant.conf"

struct wnet { char ssid[64]; int sig; int secured; int connected; };
#define WNET_MAX 40


/* The worker owns all supplicant, DHCP and transaction state below. The UI owns
 * objects and config; neither side reads the other's mutable state. */
enum { WIFI_POWER=1,WIFI_CONNECT,WIFI_FORGET,WIFI_RELOAD };
enum { WIFI_SCAN=1,WIFI_DETAILS=2,WIFI_INPUT=4 };
struct wifi_request { unsigned epoch; int kind,on; char ssid[64],key[64]; int has_key; };
struct wifi_snapshot {
    unsigned epoch,player,toast_seq;
    int ready,online,seed,reloaded,n,scanning;
    char ssid[64],ip[32],info[7][64],toast[128];
    struct wnet nets[WNET_MAX];
};
static pthread_mutex_t wifi_mu=PTHREAD_MUTEX_INITIALIZER;
static struct wifi_request wifi_queued;
static int wifi_scan_requested;
static uint32_t wifi_scan_retry_due;
static int wifi_has_request,wifi_started,wifi_seed=-1,wifi_pipe[2]={-1,-1};
static unsigned wifi_hints,wifi_worker_epoch=1,wifi_worker_player,wifi_capture_handle;
static atomic_int wifi_desired=1,wifi_page,wifi_lit,wifi_reload_freeze;
static unsigned wifi_reload_requested,wifi_reload_player;
static pid_t g_wifi_launch;
static int g_wifi_launch_grouped;
static int wifi_launch_stop(int off);
static atomic_uint wifi_activity;
static struct wifi_snapshot g_work={.seed=-1,.online=-1},g_mail,g_view={.seed=-1,.online=-1};
static int wifi_mail_ready,g_control_fd=-1,g_monitor_fd=-1,g_netlink_fd=-1,g_rolling_back;
static int g_monitor_ping;static uint32_t g_monitor_due;
static uint32_t wifi_now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint32_t)((uint64_t)t.tv_sec*1000+t.tv_nsec/1000000);}
static uint32_t wifi_elapsed(uint32_t at){return wifi_now()-at;}
static int wifi_want(void){return atomic_load(&wifi_desired);}
static int wifi_current(void);
static int wifi_socket_open(void);
static int wifi_request_on(int *,const char *,char *,int);
static int wifi_capture(const char *,char *,int,int);
static void wifi_session_end(int *);
static void wifi_rollback_end(int *unused){(void)unused;g_rolling_back=0;}
static void wifi_hint(unsigned);
static void wifi_submit(int,const char *,const char *,int);
static void wifi_lease_step(void);
static int wifi_list_ready(void);
static void scan_fill(void);

static lv_obj_t *g_sw, *g_list;
static int g_scanning;
static uint32_t g_scan_due;
/* 0: clean, 1: awaiting outcome, 2: rollback required. Never save in 1 or 2. */
static int g_conn_dirty;
static int g_save_pending;  /* verified attempt or sanitation changes awaiting SAVE_CONFIG */
static int g_initial_reload = 1;
static int wifi_discard_attempt(void);
static int wifi_reply_ok(const char *out);
static int g_conn_active;      /* non-NULL while a user connect is being polled */
static uint32_t    g_wifi_last_start = 0;   /* last wifi_up.sh launch (shared: toggle + keepalive) */
static int         g_sanitize_pending = 1;  /* sanitize saved nets once the supplicant is up (set on 1st run + each (re)launch) */
static int         g_wifi_theme_reload;
static atomic_uint g_intent_epoch = 1;      /* bumped by every explicit user Wi-Fi choice (switch or QS tile) */
static char g_pending_ssid[64];   /* SSID awaiting a password from the keyboard */
static char g_cur_ssid[64];       /* currently-connected SSID (to mark the list) */
static lv_obj_t *g_info_list;     /* details screen */

static void start_scan(void);     /* UI submission */
static void wifi_scan_start(void);
static void wifi_submit_connect(const char *,const char *);
static int wifi_list_ready(void);


static void wifi_lease_retry_reset(void);
static void scan_fill(void);
static void wifi_feedback(const char *text);
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
/* Native observations share the control protocol; stock scripts use the bounded worker runner. */

static int g_edit_fd=-1;
static int run_cap(const char *cmd,char *out,int cap){
    if(!strncmp(cmd,WCLI,strlen(WCLI))){
        char request[512]; snprintf(request,sizeof request,"%s",cmd+strlen(WCLI));
        char *redirect=strstr(request," 2>"); if(redirect) *redirect=0;
        for(char *c=request;*c && *c!=' ';c++) if(*c>='a' && *c<='z') *c-=32;
        int *fd=g_edit_fd>=0 ? &g_edit_fd : &g_control_fd;
        if(*fd<0) *fd=wifi_socket_open();
        return wifi_request_on(fd,request,out,cap);
    }
    return wifi_capture(cmd,out,cap,1000);
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

static void wifi_edit_close(void){
    if(g_edit_fd >= 0) close(g_edit_fd);
    g_edit_fd = -1;
}
static int wifi_edit_open(void){
    wifi_edit_close(); g_edit_fd=wifi_socket_open(); return g_edit_fd>=0;
}
static int wifi_edit_request(const char *cmd,char *out,int cap){
    return wifi_request_on(&g_edit_fd,cmd,out,cap);
}

static int wifi_dhcp_absent(void);

/* Radio-up stays asynchronous. Its children belong to stock, not our probe group. */
static int wifi_shell(const char *cmd){
    if(!wifi_current()) return -1;
    if(g_wifi_launch>0){
        pid_t reaped=waitpid(g_wifi_launch,NULL,WNOHANG);
        if(reaped==g_wifi_launch || (reaped<0 && errno==ECHILD))g_wifi_launch=0;
    }
    /* Off cancels the owned starter before the stock down script. Its process
     * group can include the DHCP client: Off explicitly asks stock to stop it.
     * Reload retires only the wrapper, preserving stock-owned daemons. */
    if(strstr(cmd,"wifi_down.sh") && !wifi_launch_stop(1))return -1;
    if(strstr(cmd, "wifi_up.sh")){
        /* With a surviving client, restore only the supplicant. Never run the
         * stock script here: it starts another DHCP client unconditionally. */
        int absent = wifi_dhcp_absent();
        if(absent < 0) return -1;
        if(!absent) return wifi_shell("rfkill unblock wifi; ifconfig wlan0 up; "
            "wpa_supplicant -B -i wlan0 -c /usr/data/wpa_supplicant.conf");
        if(g_wifi_launch > 0) return -1;
        struct sigaction chld;
        if(sigaction(SIGCHLD,NULL,&chld) || chld.sa_handler!=SIG_DFL ||
           (chld.sa_flags&SA_NOCLDWAIT))return -1; /* an auto-reaped PID cannot certify ownership */
        pthread_mutex_lock(&wifi_mu);
        if(!wifi_current()){pthread_mutex_unlock(&wifi_mu);return -1;}
        g_wifi_launch = fork();
        if(g_wifi_launch>0)g_wifi_launch_grouped=setpgid(g_wifi_launch,g_wifi_launch)==0 || (errno==EACCES && getpgid(g_wifi_launch)==g_wifi_launch);
        if(g_wifi_launch != 0)pthread_mutex_unlock(&wifi_mu);
        if(g_wifi_launch == 0){ if(setpgid(0,0))_exit(126); execl("/bin/sh", "sh", "-c", ". /usr/bin/wifi_up.sh; wait", (char *)NULL); _exit(127); }
        return g_wifi_launch < 0 ? -1 : 0;
    }
    char bounded[1024], out[64];
    snprintf(bounded, sizeof bounded, "%s; printf '\nRC=%%d\n' \"$?\"", cmd);
    wifi_capture(bounded, out, sizeof out,
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

static int wifi_proc_find(const char *,struct wifi_proc *);
static int wifi_radio_on(void){
    struct wifi_proc p={0}; return wifi_proc_find("wpa_supplicant",&p);
}

/* ---- persistence / auto-reconnect hardening ----------------------------- */
/* Make the running supplicant's config writable, keep EVERY saved network a candidate
 * for auto-join (not just the last-selected), and drop stale per-network channel pins
 * (a network locked to a frequency the AP has since left never associates). Run on each
 * supplicant bring-up and after a user connect. */
/* wpa_cli only responds once the control interface is up - pidof can be true well before
 * that, so a ping (PONG) is the real "ready" signal. */
static int wpa_ready(void){
    char buf[64]; run_cap(WCLI "ping 2>/dev/null",buf,sizeof buf);
    return strstr(buf,"PONG")!=NULL;
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
    if(!wpa_ready() || !wifi_edit_open()) return 0;
    int session __attribute__((cleanup(wifi_session_end))) = 1;
    (void)session;
    if(g_conn_dirty){
        if(g_conn_active) return 0;
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
    int daemon=wifi_radio_on(),blocked=wifi_rfkill_blocked();
    if(daemon || blocked==0) return 1;
    return blocked==1 ? 0 : -1;
}

/* Controls show the saved user choice, including while startup or recovery is pending.
 * Readiness belongs in the page message; rfkill must not make an ON tap mean OFF. */
int wifi_control_on(void){
    return cfg_get_int("wifi_on", 1) != 0;
}

static void wifi_reconcile_poll(void){
    if(!fw_has_radio_boot_arbitration()){ g_reco_hold = 0; return; }
    unsigned gen = ipc_generation(), rx = ipc_rx_frames();
    uint32_t now = wifi_now();
    int first_generation = 0;
    if(gen != g_reco_gen){                          /* new player generation: arm */
        first_generation = (g_reco_gen == 0xFFFFFFFFu);
        g_reco_gen = gen; g_reco_hold = 1; g_reco_rx0 = rx; g_reco_epoch0 = g_intent_epoch;
        g_reco_armed_at = now; g_reco_seen_rx = 0; g_reco_probe_at = now - 1000; g_reco_eval_at = now - RECO_PROBE_MS;
    }
    if(!g_reco_hold) return;
    if(g_intent_epoch != g_reco_epoch0){ g_reco_hold = 0; return; }        /* the user chose: their choice wins */
    if(wifi_want() == 0){ g_reco_hold = 0; return; }         /* Wi-Fi intent already off: nothing to undo */
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
    int effw = wifi_effective(), effb = bt_radio_probe();
    /* the probes themselves take time (each is bounded, not instant): judge the window on the clock AFTER them, so a
     * check that started just inside it can never adopt once it has closed */
    if(wifi_now() - g_reco_first_rx_at >= RECO_WINDOW_MS){ g_reco_hold = 0; return; }
    if(g_intent_epoch != g_reco_epoch0){ g_reco_hold = 0; return; }        /* the user chose while we probed */
    if(wifi_reco_decide(dbw, dbb, effw, effb, wifi_want(), 0)){
        fprintf(stderr, "radio reconcile (gen %u): stock boot settled, retaining wifi_on=1\n", gen);
        g_reco_hold = 0;
        return;
    }
    if(now - g_reco_first_rx_at >= RECO_WINDOW_MS) g_reco_hold = 0;       /* no conflict appeared: release unchanged */
}

/* Stock owns DHCP. Only signal a uniquely identified process. */
static struct {
    struct wifi_obs net; unsigned epoch, intent, player; uint32_t since;
    int phase, seen_empty, release_evidence, released, resume;
} g_lease;
enum { LEASE_NONE, LEASE_RELEASE, LEASE_ACQUIRE, LEASE_READY, LEASE_FAILED };

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
    if(!wifi_current() || !wifi_lease_signal_context(sig) || !wifi_proc_find("udhcpc",&current) || !wifi_proc_eq(expected,current))return 0;
    pthread_mutex_lock(&wifi_mu);
    int ok=wifi_current() && kill(current.pid,sig)==0;
    pthread_mutex_unlock(&wifi_mu);return ok;
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
    o->allowed = !g_reco_hold && wifi_want() && wifi_rfkill_blocked() == 0 &&
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

/* Failed leases back off, except an acquisition still resumable in its window. */
static int wifi_lease_slow(void){
    return g_lease.phase == LEASE_FAILED && !g_conn_active && !g_scanning &&
        !(g_lease.resume == LEASE_ACQUIRE && wifi_elapsed(g_lease.since) <= 30000);
}


/* Only failed leases back off; recovery cues make the next step due. */
static void wifi_lease_retry_reset(void){
    g_lease_retry_ms = 100; g_lease_check = wifi_now() - 100;
}





/* UI-thread snapshot of the lease observations: 1 connected, 0 offline, -1 not known yet
 * (radio decision pending, or no lease step has settled since start). */


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
    int cleanup __attribute__((cleanup(wifi_rollback_end))) = 1;
    (void)cleanup; g_rolling_back=1;
    char out[64];
    run_cap(WCLI "reconfigure 2>/dev/null", out, sizeof out);
    if(!wifi_reply_ok(out) || !wifi_attempt_clear()) return 0;
    g_conn_dirty = g_save_pending = 0;
    g_initial_reload = 0;
    return 1;
}
static void conn_fail(const char *why){
    g_conn_active = 0;
    /* Restore the saved file; supervision can rejoin without persisting a typo. */
    g_sanitize_pending = 1;
    if(wifi_discard_attempt()) g_sanitize_pending = 0;
    wifi_feedback(why);
    wifi_scan_start();
}
static void conn_poll_cb(void *t){
    (void)t;
    struct wifi_proc owner = {0};
    if(!wifi_proc_find("wpa_supplicant", &owner) || !wifi_proc_eq(owner, g_conn_owner)){
        conn_fail("Couldn't connect"); return;
    }
    char ss[64], ip[32];
    /* Association and witnessed DHCP must belong to this selected profile. */
    if(wifi_current() && wifi_status(ss, sizeof ss, ip, sizeof ip) && wifi_current() && ip[0] && g_lease.net.id == g_conn_id && !strcmp(ss, g_conn_ssid)){
        g_conn_active = 0;
        snprintf(g_cur_ssid, sizeof g_cur_ssid, "%s", g_conn_ssid);
        g_conn_dirty = 0;  /* witnessed success is the only commit path */
        g_save_pending = 1;
        g_sanitize_pending = 1;
        int saved = wifi_sanitize_savenets();
        if(saved) g_sanitize_pending = 0;
        wifi_feedback(saved ? ("Connected") : ("Wi-Fi settings not saved"));
        wifi_scan_start();                        /* refresh list so the joined net gets its checkmark */
        return;
    }
    char nl[2048]; run_cap(WCLI "list_networks 2>/dev/null", nl, sizeof nl);
    if(wifi_net_temp_disabled(nl, g_conn_id)){ conn_fail("Couldn't connect - check the password"); return; }
    if(wifi_elapsed(g_conn_start) > 75000)   /* association plus two 30s DHCP stages */
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
    if(key && key[0] && !wifi_psk_len_ok(key)){ wifi_feedback("Password must be 8 to 63 characters"); return; }
    if(g_conn_active){ g_conn_active = 0; }
    if(!wifi_discard_attempt()){ wifi_feedback(("Couldn't configure Wi-Fi")); return; }
    if(!wifi_initial_reload() || !wifi_proc_find("wpa_supplicant", &g_conn_owner) || !wifi_edit_open()){
        wifi_feedback(("Couldn't configure Wi-Fi")); return;
    }
    struct wifi_proc owner = {0};
    if(!wifi_proc_find("wpa_supplicant", &owner) || !wifi_proc_eq(owner, g_conn_owner)){
        wifi_edit_close(); wifi_feedback(("Couldn't configure Wi-Fi")); return;
    }
    int id = -1;
    int n = wifi_edit_request("LIST_NETWORKS", buf, sizeof buf);
    if(n <= 0 || n >= (int)sizeof buf - 1 || strncmp(buf, "network id", 10) || buf[n-1] != '\n'){
        wifi_edit_close(); wifi_feedback(("Couldn't configure Wi-Fi")); return;
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
        wifi_edit_close(); wifi_feedback(("Couldn't configure Wi-Fi")); return;
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
            wifi_feedback(("Couldn't add network"));
            return;
        }
        id = (int)nid;
    }
    /* The UI assigned the intent epoch before enqueuing this selection. */
    /* A saved profile selected without a typed key keeps its security settings. */
    const char *why;
    if(!key && existing){
        char cmd[64];
        snprintf(cmd, sizeof cmd, "SELECT_NETWORK %d", id);
        why = wifi_step(cmd, "select_network") ? "Couldn't connect to Wi-Fi" : NULL;
    }else why = wifi_apply_net(id, ssid, key);
    wifi_edit_close();
    if(why){ g_sanitize_pending = !wifi_discard_attempt(); wifi_feedback(why); return; }
    if(g_conn_active){ g_conn_active = 0; }
    /* don't claim success yet - poll for the real outcome (assoc + DHCP). */
    snprintf(g_conn_ssid, sizeof g_conn_ssid, "%s", ssid);
    g_conn_id = id;
    wifi_feedback(("Connecting..."));
    g_conn_start = wifi_now();
    g_conn_active = 1;
}

static void psk_done(const char *text){ if(text && *text) wifi_submit_connect(g_pending_ssid,text); }

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
    if(g_conn_active){ g_conn_active = 0; }
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
    if(!wifi_attempt_mark()){wifi_edit_close();return 0;} /* before any REMOVE, after the read-only snapshot */
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
    if(!failed && !wifi_attempt_clear()) failed=1;
    if(failed) g_initial_reload=1;
    return removed && !failed;
}
static void wifi_forget_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    wifi_submit(WIFI_FORGET,g_info_ssid,NULL,-1);
    screen_return_to(SCR_WIFI); wifi_open();
}
/* Keep the rows and their geometry alive while values change under a finger. */
static void info_value(unsigned row, const char *value){
    const char *text = value && value[0] ? value : "-";
    if(strcmp(lv_label_get_text(g_info_values[row]), text)) lv_label_set_text(g_info_values[row], text);
}
static void wifi_info_refresh(void){
    if(!g_info_list || !g_info_count) return;
    info_value(0,g_info_ssid);
    for(unsigned i=1;i<7;i++) info_value(i,
        wifi_control_on() && !strcmp(g_view.ssid,g_info_ssid) ? g_view.info[i] : "");
}
void wifi_info_open(void){

    snprintf(g_info_ssid,sizeof g_info_ssid,"%s",g_view.ssid);
    wifi_hint(WIFI_DETAILS);
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
    if(!wifi_control_on()) return;
    if(connected){
        wifi_info_open();                 /* tap the joined network -> details */
    } else if(secured){
        snprintf(g_pending_ssid, sizeof g_pending_ssid, "%s", ssid);
        char prompt[80]; snprintf(prompt, sizeof prompt, "Password for %s", ssid);
        kbinput_open_password(prompt, "", psk_done);   /* masked entry; prompt, not the bare SSID */
    } else {
        wifi_submit_connect(ssid, NULL);
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







/* ---- radio toggle ------------------------------------------------------- */
/* The readiness timer belongs only to the Wi-Fi page. Detach before start_scan:
 * it may schedule a replacement if the service is still unavailable. */

/* The one Wi-Fi power path: the page's switch and the Settings > Network row both call it. An explicit choice ends any
 * radio reconciliation hold; the keepalive enforces the saved intent. */
   /* a start made here is still waiting for the radio */

static void sw_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_VALUE_CHANGED) return;
    wifi_power_set(lv_obj_has_state(g_sw, LV_STATE_CHECKED));
}

/* Quick Settings tile short-press: flip the radio + persist intent, no screen-specific UI.
 * Mirrors sw_cb's radio actions (keepalive enforces the intent). Returns the new state. */


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

    if(g_sw){
        if(wifi_control_on()) lv_obj_add_state(g_sw, LV_STATE_CHECKED);
        else                lv_obj_clear_state(g_sw, LV_STATE_CHECKED);
    }
    screen_show(SCR_WIFI);
    start_scan();
}

/* exec preserves the parent PID, so PDEATHSIG cannot retire this helper. */


/* Linux control-interface adapter: reuse the pre-existing abstract autobind
 * transport. Separate request, edit and ATTACH sockets follow upstream wpa_ctrl.
 * A timeout closes the socket; late datagrams cannot satisfy a later request. */
#ifndef WIFI_CONTROL_PATH
#define WIFI_CONTROL_PATH "/var/run/wpa_supplicant/wlan0"
#endif
static int wifi_current(void){
    return wifi_worker_epoch==atomic_load(&g_intent_epoch) && wifi_worker_player==ipc_generation();
}
static int wifi_socket_open(void){
    int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if(fd<0) return -1;
    struct sockaddr_un local={.sun_family=AF_UNIX},remote={.sun_family=AF_UNIX};
    snprintf(remote.sun_path,sizeof remote.sun_path,"%s",WIFI_CONTROL_PATH);
    if(bind(fd,(struct sockaddr *)&local,sizeof local.sun_family) ||
       connect(fd,(struct sockaddr *)&remote,sizeof remote)){close(fd);return -1;}
    return fd;
}
static int wifi_request_on(int *fd,const char *cmd,char *out,int cap){
    out[0]=0;
    if(*fd<0 || cap<2) return 0;
    /* This short lock linearizes submission with a new user choice. Never hold
     * it across a reply, child wait, filesystem operation or LVGL call. */
    pthread_mutex_lock(&wifi_mu);
    int allowed=wifi_current() || (g_rolling_back && !strcmp(cmd,"RECONFIGURE"));
    ssize_t sent=allowed ? send(*fd,cmd,strlen(cmd),MSG_NOSIGNAL) : -1;
    pthread_mutex_unlock(&wifi_mu);
    if(sent!=(ssize_t)strlen(cmd)) goto failed;
    uint32_t began=wifi_now();
    for(;;){
        int left=1000-(int)wifi_elapsed(began);
        struct pollfd p[2]={{.fd=*fd,.events=POLLIN},{.fd=wifi_pipe[0],.events=POLLIN}};
        if(left<=0) goto failed;
        int r=poll(p,2,left);
        if(r<0 && errno==EINTR) continue;
        if(r<=0)goto failed;
        if(p[1].revents){
            char drain[64];while(read(wifi_pipe[0],drain,sizeof drain)>0){}
            if(!wifi_current() && !g_rolling_back)goto failed;
        }
        if(!(p[0].revents&POLLIN)){
            if(p[0].revents)goto failed;
            continue;
        }
        int n=recv(*fd,out,cap-1,MSG_TRUNC);
        if(n<=0 || n>=cap-1) goto failed;
        out[n]=0;
        /* Request connections are never attached. A protocol/event mix-up
         * invalidates this connection instead of authorizing a mutation. */
        if(out[0]=='<') goto failed;
        return n;
    }
failed:
    out[0]=0; close(*fd); *fd=-1; return 0;
}
static int wifi_capture(const char *cmd,char *out,int cap,int timeout){
    out[0]=0;
    pthread_mutex_lock(&wifi_mu);
    unsigned h=wifi_current() ? proc_start(cmd,timeout,cap) : 0;
    wifi_capture_handle=h;
    pthread_mutex_unlock(&wifi_mu);
    if(!h) return 0;
    int r=proc_wait(h,out,cap);
    pthread_mutex_lock(&wifi_mu); if(wifi_capture_handle==h) wifi_capture_handle=0; pthread_mutex_unlock(&wifi_mu);
    if(r!=PROC_COMPLETE) out[0]=0;
    return (int)strlen(out);
}
static void wifi_session_end(int *unused){(void)unused;wifi_edit_close();}
static void wifi_feedback(const char *text){
    if(!wifi_current()) return;
    snprintf(g_work.toast,sizeof g_work.toast,"%s",text); g_work.toast_seq++;
}
static int wifi_list_ready(void){
    return wifi_current() && wifi_want() && !g_reco_hold && wifi_rfkill_blocked()!=1 && wpa_ready();
}
static void wifi_lease_step(void){
    if(!wifi_want()){
        g_lease.phase=LEASE_FAILED;g_lease.resume=LEASE_NONE;g_lease.net.allowed=0;g_lease_settled=1;return;
    }
    char status[2048];run_cap(WCLI "status 2>/dev/null",status,sizeof status);
    struct wifi_obs o;wifi_lease_observe(&o,status);wifi_lease_advance(&o,wifi_now());
    g_lease.net.allowed=o.allowed && o.ip==1 && o.idle;
    if(!o.allowed || g_lease.phase==LEASE_READY || g_lease.phase==LEASE_FAILED) g_lease_settled=1;
    g_lease_check=wifi_now();
    if(wifi_lease_slow()){g_lease_retry_ms*=2;if(g_lease_retry_ms>4000)g_lease_retry_ms=4000;}
    else g_lease_retry_ms=100;
}
static void wifi_scan_start(void){
    if(!wifi_list_ready()) return;
    char out[64];run_cap(WCLI "scan 2>/dev/null",out,sizeof out);
    if(!wifi_reply_ok(out) && !strstr(out,"FAIL-BUSY")){wifi_feedback("Couldn't scan Wi-Fi");return;}
    g_scanning=1;g_scan_due=wifi_now()+2500;
}
static void wifi_snapshot_read(void){
    char status[2048],signal[512],gw[32],*p;
    g_work.ready=wifi_want() && !g_reco_hold && wpa_ready();
    g_work.online=!wifi_want() ? 0 : g_reco_hold || !g_lease_settled ? -1 :
        g_lease.net.allowed && g_lease.phase==LEASE_READY &&
        g_lease.intent==g_intent_epoch && g_lease.player==ipc_generation();
    g_work.ssid[0]=g_work.ip[0]=0;
    if(g_work.ready) wifi_status(g_work.ssid,sizeof g_work.ssid,g_work.ip,sizeof g_work.ip);
    memset(g_work.info,0,sizeof g_work.info);
    snprintf(g_work.info[1],64,"%s",g_work.online==1 ? g_work.ip : "");
    if(g_work.online==1 && get_gateway(gw,sizeof gw)) snprintf(g_work.info[2],64,"%s",gw);
    if(atomic_load(&wifi_page)==SCR_WIFI_INFO && g_work.ready){
        run_cap(WCLI "signal_poll 2>/dev/null",signal,sizeof signal);
        int r=wifi_parse_rssi(signal);
        if(r!=WNET_NO_RSSI) snprintf(g_work.info[3],64,"%d dBm",r);
        run_cap(WCLI "status 2>/dev/null",status,sizeof status);
        if((p=strstr(status,"key_mgmt="))) sscanf(p+9,"%63[^\n]",g_work.info[4]);
        if(strstr(g_work.info[4],"NONE")) strcpy(g_work.info[4],"Open");
        int freq;if((p=strstr(status,"\nfreq=")) && sscanf(p+6,"%d",&freq)==1) snprintf(g_work.info[5],64,"%d MHz",freq);
        if((p=strstr(status,"bssid="))) sscanf(p+6,"%63[^\n]",g_work.info[6]);
    }
    g_work.scanning=g_scanning;
}
static void wifi_scan_snapshot(void){
    if(!wifi_list_ready()){g_scanning=0;wifi_scan_requested=1;wifi_scan_retry_due=wifi_now()+2500;return;}
    char *buf=malloc(1<<20);if(!buf){wifi_feedback("Couldn't scan Wi-Fi");return;}
    run_cap(WCLI "scan_results 2>/dev/null",buf,1<<20);
    char ssid[64],ip[32],status[2048];
    if(!wifi_status(ssid,sizeof ssid,ip,sizeof ip)) ssid[0]=0;
    run_cap(WCLI "status 2>/dev/null",status,sizeof status);
    g_work.n=wifi_scan_order(buf,ssid,wifi_cur_rssi(),wifi_status_secured(status),g_work.nets);
    free(buf);g_scanning=0;
}
/* Loss/truncation/overflow make old association evidence unusable. The DHCP
 * barrier keeps its owner identity, but must witness the resumed transaction. */
static void wifi_monitor_lost(void){
    if(g_monitor_fd>=0)close(g_monitor_fd);
    g_monitor_fd=-1;g_monitor_ping=0;
    if(g_control_fd>=0)close(g_control_fd);
    g_control_fd=-1;
    g_lease.net.allowed=0;g_lease_settled=0;
    if(g_lease.phase==LEASE_RELEASE || g_lease.phase==LEASE_ACQUIRE)g_lease.resume=g_lease.phase;
    g_lease.phase=LEASE_FAILED;wifi_lease_retry_reset();
}
static int wifi_monitor_expired(void){
    if(g_monitor_ping && (int32_t)(wifi_now()-g_monitor_due)>=0){wifi_monitor_lost();return 1;}
    return 0;
}
static int wifi_monitor_open(void){
    if(g_monitor_fd>=0)return 1;
    g_monitor_fd=wifi_socket_open();char out[64];
    return wifi_request_on(&g_monitor_fd,"ATTACH",out,sizeof out)>0 && wifi_reply_ok(out);
}
static int wifi_monitor_drain(void){
    int changed=0;
    for(int i=0;i<64 && g_monitor_fd>=0;i++){
        char b[4096];int n=recv(g_monitor_fd,b,sizeof b-1,MSG_TRUNC|MSG_DONTWAIT);
        if(n<0 && (errno==EAGAIN || errno==EINTR))break;
        if(n<=0 || n>=(int)sizeof b-1){wifi_monitor_lost();return 1;}
        b[n]=0;
        if(!strcmp(b,"PONG\n") || !strcmp(b,"PONG")){g_monitor_ping=0;continue;}
        changed=1;
        if(strstr(b,"CTRL-EVENT-TERMINATING")){wifi_monitor_lost();break;}
        if(strstr(b,"CTRL-EVENT-DISCONNECTED")){
            g_lease.net.allowed=0;
            if(g_lease.phase==LEASE_RELEASE || g_lease.phase==LEASE_ACQUIRE){g_lease.resume=g_lease.phase;g_lease.phase=LEASE_FAILED;}
        }
        if(strstr(b,"CTRL-EVENT-SCAN-RESULTS") && g_scanning)g_scan_due=wifi_now();
    }
    if(changed)wifi_lease_retry_reset();
    return changed;
}
static void wifi_netlink_open(void){
    if(g_netlink_fd>=0)return;
    g_netlink_fd=socket(AF_NETLINK,SOCK_RAW|SOCK_NONBLOCK|SOCK_CLOEXEC,NETLINK_ROUTE);
    if(g_netlink_fd<0)return;
    struct sockaddr_nl a={.nl_family=AF_NETLINK,.nl_groups=RTMGRP_LINK|RTMGRP_IPV4_IFADDR|RTMGRP_IPV4_ROUTE};
    if(bind(g_netlink_fd,(struct sockaddr *)&a,sizeof a)){close(g_netlink_fd);g_netlink_fd=-1;}
}
static int wifi_netlink_drain(void){
    int changed=0;
    for(int i=0;i<64 && g_netlink_fd>=0;i++){
        union {struct nlmsghdr align;char bytes[8192];} packet;
        char *b=packet.bytes;int n=recv(g_netlink_fd,b,sizeof packet.bytes,MSG_TRUNC|MSG_DONTWAIT);
        if(n<0 && (errno==EAGAIN || errno==EINTR))break;
        if(n<=0 || n>(int)sizeof packet.bytes){
            close(g_netlink_fd);g_netlink_fd=-1;wifi_monitor_lost();return 1;
        }
        changed=1;
        int left=n;struct nlmsghdr *h=(struct nlmsghdr *)b;
        for(;NLMSG_OK(h,left);h=NLMSG_NEXT(h,left))
            if(h->nlmsg_type==NLMSG_OVERRUN || h->nlmsg_type==NLMSG_ERROR){wifi_monitor_lost();return 1;}
        if(left){wifi_monitor_lost();return 1;}
    }
    return changed;
}
static void wifi_publish(void){
    if(!wifi_current())return;
    g_work.epoch=wifi_worker_epoch;g_work.player=wifi_worker_player;
    pthread_mutex_lock(&wifi_mu);
    int changed=memcmp(&g_mail,&g_work,sizeof g_work)!=0;
    if(changed){g_mail=g_work;wifi_mail_ready=1;}
    pthread_mutex_unlock(&wifi_mu);
    if(changed)ui_notify(UI_WAKE_WIFI);
}
static void wifi_power_apply(void){
    g_conn_active=0;
                if(wifi_want()){
                    wifi_scan_requested=1;wifi_scan_retry_due=wifi_now();
                    if(!g_wifi_last_start || wifi_elapsed(g_wifi_last_start)>30000)
                        if(system("/usr/bin/wifi_up.sh >/dev/null 2>&1 &")==0)g_wifi_last_start=wifi_now();
                    g_sanitize_pending=1;
                }else{
                    g_scanning=0;wifi_scan_requested=0;wifi_discard_attempt();wifi_lease_step();
                    system("/usr/bin/wifi_down.sh >/dev/null 2>&1");g_wifi_last_start=0;
                }

}
static int wifi_launch_stop(int off){
    if(g_wifi_launch<=0)return 1;
    pid_t prior=waitpid(g_wifi_launch,NULL,WNOHANG);
    if(prior==g_wifi_launch || (prior<0 && errno==ECHILD)){g_wifi_launch=0;return 1;}
    if(prior<0)return 0;
    pthread_mutex_lock(&wifi_mu);
    int allowed=wifi_current() && (!off || !wifi_want());
    if(allowed){
        if(off && g_wifi_launch_grouped)kill(-g_wifi_launch,SIGKILL);
        kill(g_wifi_launch,SIGKILL);
    }
    pthread_mutex_unlock(&wifi_mu);
    if(!allowed)return 0;
    uint32_t began=wifi_now();
    for(;;){
        pid_t reaped=waitpid(g_wifi_launch,NULL,WNOHANG);
        if(reaped==g_wifi_launch || (reaped<0 && errno==ECHILD)){g_wifi_launch=0;return 1;}
        if((reaped<0 && errno!=EINTR) || wifi_elapsed(began)>=1000)return 0;
        /* Owned-child lifecycle gap only; no polling on the LVGL thread. */
        poll(NULL,0,10);
    }
}
static int wifi_reload_transaction(void){
    g_sanitize_pending=1;g_conn_active=0;
    if(!wifi_discard_attempt() || !wifi_launch_stop(0))return 0;
    wifi_edit_close();wifi_monitor_lost();return 1;
}
static int wifi_reload_park(void){
    pthread_mutex_lock(&wifi_mu);
    int prepared=wifi_current() && !wifi_has_request;
    if(prepared)atomic_store(&wifi_reload_freeze,1);
    pthread_mutex_unlock(&wifi_mu);return prepared;
}
static uint32_t wifi_seed_due;
static void wifi_seed_poll(void){
    if(wifi_seed<0 || (int32_t)(wifi_now()-wifi_seed_due)<0)return;
    char b[32];run_cap("sqlite3 /usr/data/fiio/db/sysconfig.db \"SELECT WIFI_STATUS FROM SYSCONFIG WHERE ID=1\" 2>/dev/null",b,sizeof b);
    pthread_mutex_lock(&wifi_mu);
    if((b[0]=='0' || b[0]=='1') && wifi_current() && atomic_load(&g_intent_epoch)==1){
        g_work.seed=b[0]-'0';atomic_store(&wifi_desired,g_work.seed);wifi_seed=-1;
    }
    pthread_mutex_unlock(&wifi_mu);
    wifi_seed_due=wifi_now()+5000; /* transient DB failure must not latch a default as saved intent */
}
struct wifi_worker_state {
    uint32_t health,reco,conn,control_failed_at;
    struct stat peer;
    struct wifi_proc daemon;
    int control_failed,hold;
};
/* One worker iteration; only the caller blocks on its returned deadline.
 * A parked reload skips the wait and re-enters the freeze check. */
static int wifi_worker_step(struct wifi_worker_state *state){
    struct wifi_request request={0};unsigned hints;
    pthread_mutex_lock(&wifi_mu);
    if(wifi_has_request){request=wifi_queued;wifi_has_request=0;memset(wifi_queued.key,0,sizeof wifi_queued.key);}
    hints=wifi_hints;wifi_hints=0;
    unsigned epoch=atomic_load(&g_intent_epoch),player=ipc_generation();
    pthread_mutex_unlock(&wifi_mu);
    if(epoch!=wifi_worker_epoch || player!=wifi_worker_player){
        g_conn_active=0;g_work.toast[0]=0; /* feedback belongs to its original generation */
        if(g_save_pending && !g_conn_dirty)g_initial_reload=1;
        wifi_discard_attempt();
        wifi_worker_epoch=epoch;wifi_worker_player=player;wifi_lease_retry_reset();state->health=state->reco=wifi_now();
    }
    if(wifi_seed>=0 && !request.kind)wifi_seed_poll();
    if(request.kind)wifi_seed=-1;
    if(request.kind && request.epoch==epoch && wifi_current()){
        g_work.reloaded=0;
        if(request.kind==WIFI_POWER){wifi_power_apply();
        }else if(request.kind==WIFI_CONNECT){wifi_connect(request.ssid,request.has_key ? request.key : NULL);state->conn=wifi_now();}
        else if(request.kind==WIFI_FORGET){wifi_feedback(wifi_remove_ssid(request.ssid) ? "Network forgotten" : "Couldn't forget network");hints|=WIFI_SCAN;}
        else if(request.kind==WIFI_RELOAD){
            g_work.reloaded=wifi_reload_transaction();
            if(g_work.reloaded && !wifi_want() && wifi_radio_on())
                g_work.reloaded=system("/usr/bin/wifi_down.sh >/dev/null 2>&1")==0;
            if(g_work.reloaded && wifi_reload_park()){
                g_work.ready=0;g_work.online=wifi_want() ? -1 : 0;g_work.scanning=0;
                wifi_publish();return -1;
            }
            g_work.reloaded=0;
        }
        memset(request.key,0,sizeof request.key);state->health=wifi_now();
    }
    uint32_t now=wifi_now();int refresh=wifi_monitor_drain();
    if(wifi_monitor_expired()){refresh=1;state->health=now;}
    if((int32_t)(now-state->reco)>=0){wifi_reconcile_poll();state->reco=wifi_now()+1000;}
    if(!!g_reco_hold!=state->hold){state->hold=!!g_reco_hold;refresh=1;}   /* the UI snapshot follows the radio decision at once */
    if((int32_t)(now-state->health)>=0){
        state->health=now+5000;refresh=1;
        struct stat st={0};struct wifi_proc current={0};wifi_proc_find("wpa_supplicant",&current);
        if(stat(WIFI_CONTROL_PATH,&st) || st.st_dev!=state->peer.st_dev || st.st_ino!=state->peer.st_ino || !wifi_proc_eq(current,state->daemon)){
            wifi_monitor_lost();state->peer=st;state->daemon=current;
        }
        /* Subscribe before any authoritative initial snapshot. Monitor PING
         * detects a disconnected or replaced state->peer even in complete silence. */
        if(wifi_want() && !g_reco_hold){
            if(!wifi_monitor_open())wifi_monitor_lost();
            else if(!g_monitor_ping){
                g_monitor_ping=1;g_monitor_due=wifi_now()+1000;
                if(send(g_monitor_fd,"PING",4,MSG_NOSIGNAL)!=4)wifi_monitor_lost();
            }
        }
        wifi_netlink_open();
        int up=wifi_radio_on(),want=wifi_want(),ready=up && want ? wpa_ready() : 0;
        if(!up || !want || ready)state->control_failed=0;
        if(up && want && !ready){
            if(!state->control_failed){state->control_failed=1;state->control_failed_at=now;}
            else if(wifi_elapsed(state->control_failed_at)>=30000){
                if(system("killall -9 wpa_supplicant >/dev/null 2>&1")==0){up=0;state->control_failed=0;}
            }
        }
        if(!g_reco_hold && wifi_current()){
            if(up && want && g_sanitize_pending && !g_conn_active && wifi_sanitize_savenets())g_sanitize_pending=0;
            int blocked=wifi_rfkill_blocked();want=wifi_want();
            if(up && want && blocked==1)system("rfkill unblock wifi; ifconfig wlan0 up");
            if(!up && want && (!g_wifi_last_start || wifi_elapsed(g_wifi_last_start)>30000)){
                g_sanitize_pending=1;
                if(system("/usr/bin/wifi_up.sh >/dev/null 2>&1 &")==0)g_wifi_last_start=wifi_now();
            }else if(!want && (up || blocked!=1))system("/usr/bin/wifi_down.sh >/dev/null 2>&1");
        }
    }
    refresh|=wifi_netlink_drain();
    if(hints&WIFI_SCAN){wifi_scan_requested=1;wifi_scan_retry_due=wifi_now();}
    if(!g_reco_hold && wifi_current()){
        if(hints&WIFI_INPUT)wifi_lease_retry_reset();
        if(refresh || (g_lease.phase!=LEASE_READY && wifi_elapsed(g_lease_check)>=g_lease_retry_ms)){
            int phase=g_lease.phase,allowed=g_lease.net.allowed,settled=g_lease_settled;
            wifi_lease_step();
            refresh|=phase!=g_lease.phase || allowed!=g_lease.net.allowed || settled!=g_lease_settled;
        }
        if(wifi_scan_requested && atomic_load(&wifi_page)==SCR_WIFI &&
           (refresh || (int32_t)(wifi_now()-wifi_scan_retry_due)>=0)){
            if(wifi_list_ready()){wifi_scan_requested=0;wifi_scan_start();}
            else wifi_scan_retry_due=wifi_now()+2500;
        }
        if(g_scanning && (int32_t)(wifi_now()-g_scan_due)>=0){wifi_scan_snapshot();refresh=1;}
        if(g_conn_active && (refresh || (int32_t)(wifi_now()-state->conn)>=0)){conn_poll_cb(NULL);state->conn=wifi_now()+1000;refresh=1;}
        const char *current=g_lease.net.allowed && g_lease.phase==LEASE_READY ? g_lease.net.ssid : "";
        if(atomic_load(&wifi_page)==SCR_WIFI && !g_scanning && strcmp(current,g_cur_ssid)){
            snprintf(g_cur_ssid,sizeof g_cur_ssid,"%s",current);
            wifi_scan_snapshot();refresh=1;
        }
    }
    if(refresh || hints || request.kind){wifi_snapshot_read();wifi_publish();}
    now=wifi_now();uint32_t wait=5000;
    wait=ui_deadline_min(wait,now,state->health);
    if(wifi_seed>=0)wait=ui_deadline_min(wait,now,wifi_seed_due);
    if(g_monitor_ping)wait=ui_deadline_min(wait,now,g_monitor_due);
    if(g_reco_hold)wait=ui_deadline_min(wait,now,state->reco);
    if(!g_reco_hold){
        if(g_conn_active)wait=ui_deadline_min(wait,now,state->conn);
        if(g_scanning)wait=ui_deadline_min(wait,now,g_scan_due);
        if(wifi_scan_requested && atomic_load(&wifi_page)==SCR_WIFI)
            wait=ui_deadline_min(wait,now,wifi_scan_retry_due);
    }
    if(!g_reco_hold && wifi_want() && g_lease.phase!=LEASE_READY)wait=ui_deadline_min(wait,now,g_lease_check+g_lease_retry_ms);
    return (int)wait;
}
static void *wifi_worker(void *unused){
    (void)unused;
    uint32_t now=wifi_now();
    struct wifi_worker_state state={.health=now,.reco=now,.conn=now};
    for(;;){
        if(atomic_load(&wifi_reload_freeze)){
            struct pollfd f={wifi_pipe[0],POLLIN,0};poll(&f,1,-1);
            char b[64];while(read(wifi_pipe[0],b,sizeof b)>0){}
            continue;
        }
        int wait=wifi_worker_step(&state);
        if(wait<0)continue;
        struct pollfd fds[3]={{wifi_pipe[0],POLLIN,0},{g_monitor_fd,POLLIN,0},{g_netlink_fd,POLLIN,0}};
        poll(fds,3,wait);char b[64];while(read(wifi_pipe[0],b,sizeof b)>0){}
    }
    return NULL;
}
static void wifi_poke(void){char b=0;if(wifi_pipe[1]>=0)while(write(wifi_pipe[1],&b,1)<0 && errno==EINTR){}}
void wifi_init_intent(void){
    if(wifi_started)return;
    int want=cfg_get_int("wifi_on",-1);wifi_seed=want<0 ? 0 : -1;
    (void)bt_radio_on(); /* publish initial native observation before the worker reads it */
    atomic_store(&wifi_desired,want!=0);wifi_seed_due=wifi_now();g_wifi_theme_reload=getenv("DISKOS_THEME_RELOAD")!=NULL;
    if(pipe2(wifi_pipe,O_CLOEXEC|O_NONBLOCK))return;
    pthread_t t;
    if(pthread_create(&t,NULL,wifi_worker,NULL)){close(wifi_pipe[0]);close(wifi_pipe[1]);wifi_pipe[0]=wifi_pipe[1]=-1;return;}
    pthread_detach(t);wifi_started=1;
}
static void wifi_hint(unsigned hints){
    wifi_init_intent();pthread_mutex_lock(&wifi_mu);wifi_hints|=hints;wifi_poke();pthread_mutex_unlock(&wifi_mu);
}
static void wifi_submit(int kind,const char *ssid,const char *key,int on){
    wifi_init_intent();pthread_mutex_lock(&wifi_mu);
    if(kind!=WIFI_RELOAD)atomic_store(&wifi_reload_freeze,0);
    if(kind==WIFI_POWER)atomic_store(&wifi_desired,!!on);
    unsigned epoch=atomic_fetch_add(&g_intent_epoch,1)+1;
    wifi_queued=(struct wifi_request){.epoch=epoch,.kind=kind,.on=on,.has_key=key!=NULL};
    snprintf(wifi_queued.ssid,sizeof wifi_queued.ssid,"%s",ssid ? ssid : "");
    snprintf(wifi_queued.key,sizeof wifi_queued.key,"%s",key ? key : "");
    wifi_has_request=1;
    if(wifi_capture_handle)proc_cancel(wifi_capture_handle);
    wifi_poke();pthread_mutex_unlock(&wifi_mu);
}
static void wifi_submit_connect(const char *ssid,const char *key){
    if(key && *key && !wifi_psk_len_ok(key)){ui_toast("Password must be 8 to 63 characters");return;}
    wifi_submit(WIFI_CONNECT,ssid,key,-1);
}
void wifi_power_set(int on){
    g_view.ready=0;g_view.online=on ? -1 : 0;
    wifi_submit(WIFI_POWER,NULL,NULL,on);
    cfg_set_int("wifi_on",!!on);
    if(g_sw){if(on)lv_obj_add_state(g_sw,LV_STATE_CHECKED);else lv_obj_clear_state(g_sw,LV_STATE_CHECKED);}
    if(screen_current()==SCR_WIFI)list_msg(on ? "Starting Wi-Fi..." : "Wi-Fi is off");
}
int wifi_toggle(void){int on=!wifi_control_on();wifi_power_set(on);return on;}
int wifi_power_pending(void){return wifi_control_on() && !g_view.ready;}
int wifi_connectivity(void){
    if(!wifi_control_on())return 0;
    if(g_view.epoch!=atomic_load(&g_intent_epoch) || g_view.player!=ipc_generation())return -1;
    return g_view.online;
}
static void start_scan(void){
    if(screen_current()!=SCR_WIFI)return;
    if(!wifi_control_on()){list_msg("Wi-Fi is off");return;}
    if(g_view.ready)list_msg_scanning();else list_msg("Starting Wi-Fi...");
    wifi_hint(WIFI_SCAN);
}
static void scan_fill(void){
    if(screen_current()!=SCR_WIFI || !g_list)return;
    if(!wifi_control_on()){list_msg("Wi-Fi is off");return;}
    if(!g_view.ready){list_msg("Starting Wi-Fi...");return;}
    if(g_view.scanning){list_msg_scanning();return;}
    scan_stop();lv_obj_clean(g_list);
    lv_obj_set_flex_align(g_list,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
    for(int i=0;i<g_view.n;i++)add_net_row(g_view.nets[i].ssid,g_view.nets[i].sig,g_view.nets[i].secured,g_view.nets[i].connected);
    if(!g_view.n)list_msg("No networks found");
}
void wifi_supervise(void){
    wifi_init_intent();
    static unsigned player;
    unsigned generation=ipc_generation();
    int page=screen_current(),lit=ui_screen_backlight_on();
    uint32_t activity=lv_tick_get()-lv_display_get_inactive_time(NULL);
    int changed=generation!=player;player=generation;
    changed|=atomic_exchange(&wifi_page,page)!=page;
    changed|=atomic_exchange(&wifi_lit,lit)!=lit;changed|=atomic_exchange(&wifi_activity,activity)!=activity;
    if(changed)
        wifi_hint(WIFI_INPUT|(page==SCR_WIFI_INFO ? WIFI_DETAILS : 0));
    struct wifi_snapshot mail;int ready;
    pthread_mutex_lock(&wifi_mu);ready=wifi_mail_ready;mail=g_mail;wifi_mail_ready=0;pthread_mutex_unlock(&wifi_mu);
    if(!lit)scan_stop();
    if(!ready || mail.epoch!=atomic_load(&g_intent_epoch) || mail.player!=ipc_generation())return;
    unsigned toast=g_view.toast_seq;g_view=mail;
    if(mail.seed>=0 && cfg_get_int("wifi_on",-1)<0)cfg_set_int("wifi_on",mail.seed);
    if(mail.toast_seq!=toast && mail.toast[0])ui_toast(tr(mail.toast));
    if(lit){if(page==SCR_WIFI)scan_fill();if(page==SCR_WIFI_INFO)wifi_info_refresh();}
}
int wifi_prepare_reload(void){
    if(wifi_reload_requested && g_view.epoch==wifi_reload_requested)return g_view.reloaded;
    if(!wifi_reload_requested){
        wifi_reload_player=ipc_generation();
        wifi_submit(WIFI_RELOAD,NULL,NULL,-1);wifi_reload_requested=atomic_load(&g_intent_epoch);
    }
    return 0;
}
int wifi_reload_result(void){
    if(!wifi_reload_requested || wifi_reload_requested!=atomic_load(&g_intent_epoch) || wifi_reload_player!=ipc_generation())return -1;
    if(g_view.epoch!=wifi_reload_requested)return 2;
    return g_view.reloaded ? 1 : 0;
}
void wifi_reload_cancel(void){
    wifi_reload_requested=0;atomic_store(&wifi_reload_freeze,0);wifi_hint(WIFI_INPUT);
}
