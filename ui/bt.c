/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "fwcaps.h"
#include "device_names_ui.h"
#include "i18n.h"
#include "procrun.h"
#include "ui_wake.h"
#include "radio_monitor.h"
#include "screens.h"
extern int ui_screen_backlight_on(void);
#include "theme.h"
#include "theme_kit.h"
#include "config.h"       /* cfg_get_int/cfg_set_int: persist the BT on/off intent */
#include "bt_codec_observe.h"
#include "bt_prepare.h"
#include "musicdb.h"
#include "modes.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>

/* Bluetooth settings (SCR_BT) + a details screen (SCR_BT_INFO).
 * The BT chip (BCM43438 / AP6212, 2.4GHz-only) sits on UART /dev/ttyS0. RE of the
 * stock mq_player (NOT BSA - bsa_server is dead code for the wrong chip) shows the
 * stock production stack is ALSO bluez, but it FIRST downloads the chip firmware
 * patch with `brcm_patchram_plus ... --patchram <BCM4343A1...hcd> /dev/ttyS0` @3Mbaud
 * to create a working hci0 - nothing attaches hci0 at boot (bcmdhd.ko only loads the
 * driver/GPIOs). Without that patchram step inquiry/scan is broken, which is why our
 * earlier bluez-only bring-up scanned poorly. bt_enable() now mirrors stock exactly:
 * patchram -> /usr/project/bluetoothd (a2dp,avrcp,source) -> hciconfig up/piscan/class
 * -> bluetoothctl agent. `bluetoothctl` drives scan/pair/connect; `bluealsa` is the
 * a2dp-source audio engine (SBC/LDAC).  Layout matches the Wi-Fi page (control row in the
 * centre band; the list carries on/scanning/empty state; connected checkmark row taps
 * through to details).  NOTE: routing the player's audio onto a connected BT
 * sink is the stock mq_player's job (it has bluealsa support) - building the
 * profile here is stage 1; confirm real headphone playback with the user. */

static lv_obj_t *g_sw, *g_list, *g_info_list;
static lv_timer_t *g_scan_timer;
static lv_timer_t *g_scanwait_timer;   /* bt_open's non-destructive "wait for adapter then scan" poll */
static uint32_t    g_scanwait_start;
static lv_timer_t *g_bt_autoroute_timer;
static int g_autoroute_wait;
static char g_sel_mac[20];     /* device selected for the details screen */
static char g_bt_autorouted[20];
/* ---- routing ownership ---- */
/* Automatic routing obeys the connection coordinator. Before any explicit connection choice (startup) a connected
 * sink may be routed; afterwards only the authorized MAC may. A pending, failed, cancelled or superseded attempt
 * authorizes nothing, and a revocation is never undone by a PCM disappearing, a timeout or a player restart. */
static int  g_route_owned;
static char g_route_auth[20];
static int bt_route_allowed(const char *mac){ return !g_route_owned || !strcmp(mac, g_route_auth); }
static void bt_route_own(const char *mac){ g_route_owned = 1; snprintf(g_route_auth, sizeof g_route_auth, "%s", mac ? mac : ""); }
static void bt_route_revoke(const char *mac){
    if(!g_route_owned) bt_route_own(NULL);
    else if(!strcmp(mac, g_route_auth)) g_route_auth[0] = 0;
}
/* ---- end routing ownership ---- */

static void start_scan(void);             /* fwd */
static void scan_timer_cb(lv_timer_t *t);  /* fwd */
static void scan_abort(void);             /* fwd */
static void scan_kick(void);              /* fwd - off-thread re-enumerate (no discovery window) */
static void bt_autoroute_start(void);      /* fwd */
static void bt_autoroute_stop(void);       /* fwd */
static void bootrestore_cancel(void);      /* fwd - every explicit enable/disable supersedes boot observation */

/* The refresh glyph INSIDE the "Scanning" message spins while a scan runs. It lives
 * in g_list, so it must be stopped before g_list is cleaned (else the anim references
 * a freed object). scan_stop() is called at the top of every g_list-clearing path. */
static lv_obj_t *g_scan_icon;
static void spin_anim_cb(void *o, int32_t v){ lv_obj_set_style_transform_rotation((lv_obj_t*)o, v, 0); }
static void scan_stop(void){
    if(g_scan_icon){ lv_anim_delete(g_scan_icon, spin_anim_cb); g_scan_icon = NULL; }
}

/* ---- header helpers (local copies) -------------------------------------- */

/* UI polling never waits for a child. Separate keys preserve the existing timer callers. */
enum { JOB_DRAIN = 1, JOB_FORGET };
static void bt_job_add(unsigned h, int kind, const char *mac);
static struct { char cmd[512]; unsigned handle; } g_queries[4];
static void bt_capture_drop_except(const char *keep){
    for(int i = 0; i < 4; i++){
        if(!g_queries[i].handle || (keep && !strcmp(g_queries[i].cmd, keep))) continue;
        proc_cancel(g_queries[i].handle);
        bt_job_add(g_queries[i].handle, JOB_DRAIN, NULL);
        g_queries[i].handle = 0; g_queries[i].cmd[0] = 0;
    }
}
static int run_cap_bounded(const char *cmd, char *out, int cap, int timeout_ms){
    out[0] = 0;
    if(strlen(cmd) >= sizeof g_queries[0].cmd) return PROC_FAILED;
    int i = -1;
    for(int k = 0; k < 4; k++)
        if(!strcmp(g_queries[k].cmd, cmd)){ i = k; break; }
    if(i < 0) for(int k = 0; k < 4; k++)
        if(!g_queries[k].cmd[0]){ i = k; break; }
    if(i >= 0){
        if(!g_queries[i].handle){
            strcpy(g_queries[i].cmd, cmd);
            g_queries[i].handle = proc_start(cmd, timeout_ms, cap);
            return g_queries[i].handle ? 0 : PROC_FAILED;
        }
        int r = proc_poll(g_queries[i].handle, out, cap);
        if(!r) return 0;
        g_queries[i].handle = 0; g_queries[i].cmd[0] = 0;
        if(r != PROC_COMPLETE) out[0] = 0; /* incomplete query is unknown */
        return r;
    }
    return PROC_FAILED;
}
static void scan_spin_start(void){
    if(!g_scan_icon)return;
    lv_anim_delete(g_scan_icon,spin_anim_cb);
    lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, g_scan_icon);
    lv_anim_set_exec_cb(&a, spin_anim_cb);
    lv_anim_set_values(&a, 0, 3600); lv_anim_set_time(&a, 900);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}
static void scan_spinner_sync(int visible){
    static int was_visible=1;
    if(visible==was_visible)return;
    was_visible=visible;
    if(!g_scan_icon)return;
    if(visible)scan_spin_start();else lv_anim_delete(g_scan_icon,spin_anim_cb);
}
/* Legacy workflows and scan/codec workers: bounded wait; their state migration is separate. */
static int bt_capture_wait(const char *cmd, char *out, int cap, int timeout_ms){
    unsigned h = proc_start(cmd, timeout_ms, cap);
    out[0] = 0;
    if(!h) return 0;
    int r;
    r = proc_wait(h, out, cap);
    if(r != PROC_COMPLETE) out[0] = 0;
    return (int)strlen(out);
}
int ui_run_cap_bounded(const char *cmd, char *out, int cap, int timeout_ms){ return bt_capture_wait(cmd, out, cap, timeout_ms); }
static int run_cap(const char *cmd, char *out, int cap){ return bt_capture_wait(cmd, out, cap, 1000); }

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
    if(ui_screen_backlight_on() && screen_current()==SCR_BT)scan_spin_start();
}

/* powered = bluetoothd up AND adapter Powered: yes */
static int g_bt_probe_pending;
static int bt_on(void){
    static int last;static unsigned seen=~0u;static uint32_t retry;
    unsigned revision=radio_monitor_revision();
    if(!g_bt_probe_pending && seen==revision && (radio_monitor_ready() || (int32_t)(lv_tick_get()-retry)<0))return last;
    seen=revision;
    char s[2048];int result=run_cap_bounded("bluetoothctl show 2>/dev/null",s,sizeof s,250);
    g_bt_probe_pending=result==0;
    if(result){retry=lv_tick_get()+1000;if(s[0])last=strstr(s,"Powered: yes")!=NULL;}
    return last;
}

/* Cheap "is the BT radio enabled?" for the status icon + QS tile: reads the bluetooth rfkill soft-block
 * from /sys (no process spawn). Reflects reality regardless of who set it - diskOS bt_disable() rfkill-
 * blocks, and the stock boot can bring BT up per its own SYSCONFIG - unlike the persisted cfg intent,
 * which goes stale when the stock firmware enables BT out from under us. Falls back to the intent if no
 * bluetooth rfkill node exists. */
static atomic_int g_bt_radio_observed;
static int bt_radio_native(void){
    int found = 0;
    for(int i = 0; i < 12; i++){
        char p[64]; snprintf(p, sizeof p, "/sys/class/rfkill/rfkill%d/type", i);
        FILE *f = fopen(p, "r"); if(!f) continue;
        char t[16] = {0}; char *r = fgets(t, sizeof t, f); fclose(f);
        if(!r || strncmp(t, "bluetooth", 9) != 0) continue;
        found = 1;
        snprintf(p, sizeof p, "/sys/class/rfkill/rfkill%d/soft", i);
        f = fopen(p, "r"); if(!f) continue;
        int soft = 1; if(fscanf(f, "%d", &soft) != 1) soft = 1; fclose(f);
        if(soft != 0) return 0;   /* any bluetooth rfkill soft-blocked -> BT off */
    }
    return found ? 1 : -1;
}
int bt_radio_on(void){int on=bt_radio_native();if(on<0)on=cfg_get_int("bt_on",0);atomic_store(&g_bt_radio_observed,on);return on;}
int bt_radio_cached(void){return atomic_load(&g_bt_radio_observed);}
int bt_radio_probe(void){int on=bt_radio_native();return on<0 ? bt_radio_cached() : on;}
/* What the page's switch and the Settings row's switch show: the saved intent OR the live radio (see bt_open). */
int bt_control_on(void){ return cfg_get_int("bt_on", 0) || bt_radio_on(); }

/* ---- enable / disable --------------------------------------------------- */
/* idempotent: ensure the pairing agent + a2dp-source audio endpoint are up.
 * Must run whenever the radio is on - without bt-agent pairing fails, and
 * without bluealsa there is no audio sink for the player to route to. */
/* a2dp-source audio engine. --sbc-quality=medium (bit-pool ~33) keeps SBC encode load
 * under this X2000 CPU's ceiling: stock's default-quality stereo SBC stutters, but medium
 * quality plays clean STEREO with headroom (on-device: ~86% idle). bluetoothctl's own agent
 * (set up in bt_enable) handles just-works pairing, so no separate bt-agent. */
static void bt_ensure_services(void){
    system("pidof bluealsa >/dev/null 2>&1 || bluealsa -S --device=hci0 --profile=a2dp-source "
           "--sbc-quality=medium --ldac-abr --ldac-quality=standard "
           "--codec=sbc --initial-volume=48 >/tmp/bluealsa.log 2>&1 &");
}
/* Stock V2.57 codec menu: mq_ui 0x455bd0, payloadless VALUE1 0..4.
 * Only a listener's Settings choice sends this command. The stock connection
 * worker owns saved-codec application; diskOS never reconciles it on a timer. */
const char *const BT_CODEC_LABEL[BT_CODEC_N] = { "SBC", "AAC", "LDAC Mobile", "LDAC Standard", "LDAC High" };
static int bt_mac_valid(const char *mac);
static int bt_codec_frame_value(const char *mac, int v, char *out, int cap){
    if(!bt_mac_valid(mac)) return -1;
    int r = snprintf(out, cap, "06b3%04X%04X%s", (unsigned)(12 + 17), (unsigned)v, mac);
    return (r > 0 && r < cap) ? 0 : -1;
}
/* Legacy routing retains its shipped SBC frame. */
int ui_bt_codec_frame(const char *mac, char *out, int cap){
    return bt_codec_frame_value(mac, 0, out, cap);
}
int ui_bt_codec_set(int value){
    if(fw_os_ver() != 257 || value < 0 || value >= BT_CODEC_N) return -1;
    char frame[16]; snprintf(frame, sizeof frame, "06b3000C%04X", (unsigned)value);
    return ipc_send_cmd(frame);
}
#ifdef DISKOS_TEST_BT_PREPARE
#define BTC_AAC  2u
#define BTC_LDAC 4u
static unsigned bt_codec_need(int v){ return v == 1 ? BTC_AAC : (v >= 2 && v <= 4) ? BTC_LDAC : 0u; }
/* The stock VALUE1 to send: the saved choice, or SBC when the sink is known not to offer that codec. */
int bt_codec_pick(int choice, unsigned avail, int avail_known){
    if(choice < 0 || choice >= BT_CODEC_N) return 0;
    unsigned need = bt_codec_need(choice);
    if(need && !(avail_known && (avail & need))) return 0;   /* offered, or unknown: SBC */
    return choice;
}
/* Codecs a sink offers, from `bluealsa-cli info <pcm>`: the words on the "Available codecs:" line and on the lines
 * after it that carry no ':'. *known = 0 when that header is missing (unparseable/failed query). */
unsigned bt_codec_parse_avail(const char *info, int *known){
    unsigned m = 0;
    if(known) *known = 0;
    const char *h = info ? strstr(info, "Available codecs:") : NULL;
    if(!h) return 0;
    if(known) *known = 1;
    const char *p = h + 17;
    for(int first = 1; *p;){
        const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p);
        if(!first && memchr(p, ':', n)) break;
        first = 0;
        for(size_t i = 0; i < n;){
            size_t j = i;
            while(j < n && ((p[j]>='A'&&p[j]<='Z')||(p[j]>='a'&&p[j]<='z')||(p[j]>='0'&&p[j]<='9'))) j++;
            if(j - i == 3 && !strncasecmp(p + i, "AAC", 3)) m |= BTC_AAC;
            else if(j - i == 4 && !strncasecmp(p + i, "LDAC", 4)) m |= BTC_LDAC;
            i = j > i ? j : i + 1;
        }
        if(!e) break;
        p = e + 1;
    }
    return m;
}
/* Shell command that prints "PCM=<path>" then the A2DP source PCM's info for a device (MAC already validated);
 * it prints nothing while BlueALSA has no such PCM. */
static int bt_codec_query_cmd(const char *mac, char *out, int cap){
    if(!bt_mac_valid(mac)) return -1;
    char d[24]; int n = 0;
    for(int i = 0; i < 17; i++) d[n++] = mac[i] == ':' ? '_' : mac[i];
    d[n] = 0;
    int r = snprintf(out, cap, "p=$(bluealsa-cli list-pcms 2>/dev/null) || exit 1; "
                               "p=$(printf '%%s\\n' \"$p\" | grep -i '^/org/bluealsa/hci0/dev_%s/a2dpsrc/sink$' | head -n 1); "
                               "[ -n \"$p\" ] && { echo \"PCM=$p\"; bluealsa-cli info \"$p\" 2>/dev/null; }", d);
    return (r > 0 && r < cap) ? 0 : -1;
}
static int64_t bt_probe_now(void){
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC, &t) != 0) return -1;
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
/* Thread-safe capture for the probe worker. Only complete zero-exit output is
 * evidence. One deadline covers output AND child exit, including EINTR and a
 * descendant holding stdout open. Failure discards all bytes and reaps the child. */
static int bt_probe_run(const char *cmd, char *out, int cap, int timeout_ms){
    if(!out || cap < 1) return 0;
    out[0] = 0;
    if(!cmd || cap < 2 || timeout_ms <= 0 || timeout_ms > 30000) return 0;
    struct sigaction chld;
    if(sigaction(SIGCHLD, NULL, &chld) != 0 || chld.sa_handler != SIG_DFL ||
       (chld.sa_flags & SA_NOCLDWAIT)) return 0;
    int64_t start = bt_probe_now();
    if(start < 0) return 0;
    int64_t deadline = start + timeout_ms;
    int fds[2];
    if(pipe(fds) != 0) return 0;
    pid_t pid = fork();
    if(pid < 0){ close(fds[0]); close(fds[1]); return 0; }
    if(pid == 0){
        close(fds[0]);
        if(setpgid(0, 0) != 0 || dup2(fds[1], 1) < 0) _exit(127);
        close(fds[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL); _exit(127);
    }
    setpgid(pid, pid);
    close(fds[1]);
    int n = 0, eof = 0, reaped = 0, success = 0, status;
    for(;;){
        int64_t now = bt_probe_now();
        if(now < 0 || now >= deadline) break;
        int left = (int)(deadline - now);
        if(eof){
            pid_t r = waitpid(pid, &status, WNOHANG);
            if(r == pid){ reaped = 1; success = WIFEXITED(status) && WEXITSTATUS(status) == 0; break; }
            if(r < 0 && errno != EINTR) break;
            /* EOF alone does not mean the shell exited. Wait in bounded steps. */
            if(poll(NULL, 0, left < 10 ? left : 10) < 0 && errno != EINTR) break;
            continue;
        }
        struct pollfd pf = { fds[0], POLLIN, 0 };
        int ready = poll(&pf, 1, left);
        if(ready < 0){ if(errno == EINTR) continue; break; }
        if(!ready || (pf.revents & (POLLERR | POLLNVAL))) break;
        char chunk[256]; ssize_t r = read(fds[0], chunk, sizeof chunk);
        if(r < 0){ if(errno == EINTR) continue; break; }
        if(!r){ eof = 1; continue; }
        if(r > cap - 1 - n) break;
        memcpy(out + n, chunk, (size_t)r);
        n += r;
    }
    close(fds[0]);
    if(!reaped){
        kill(-pid, SIGKILL); kill(pid, SIGKILL);
        while(waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
    }
    out[success ? n : 0] = 0;
    return success ? n : 0;
}
/* A separate read-only job for the preparation owner. A cancelled worker keeps
 * the single busy latch until it exits, so route churn cannot accumulate jobs. */
static struct {
    pthread_mutex_t mu;
    uint64_t serial, id;
    unsigned generation;
    int busy, done, good, known;
    unsigned available;
    bc_snapshot selected;
} g_bp = {.mu = PTHREAD_MUTEX_INITIALIZER};
typedef struct { uint64_t id; char mac[18]; } bp_probe_arg;
static void *bt_prepare_probe_worker(void *arg){
    bp_probe_arg *a = arg;
    char cmd[384], info[4097]; bc_snapshot s = {0}; int n = 0, known = 0; unsigned avail = 0;
    if(bt_codec_query_cmd(a->mac, cmd, sizeof cmd) == 0) n = bt_probe_run(cmd, info, sizeof info, 700);
    int good = n > 0 && bc_parse(info, (size_t)n, a->mac, &s) == BC_OK && s.adapter == 0;
    if(good) avail = bt_codec_parse_avail(info, &known);
    pthread_mutex_lock(&g_bp.mu);
    if(g_bp.id == a->id){
        g_bp.selected = s; g_bp.known = known; g_bp.available = avail;
        g_bp.good = good; g_bp.done = 1;
    }
    g_bp.busy = 0;
    pthread_mutex_unlock(&g_bp.mu);
    free(a); return NULL;
}
int bt_prepare_probe_start(const char *mac, unsigned generation, uint64_t *id){
    if(!id || !bt_mac_valid(mac) || generation != ipc_generation()) return -1;
    bp_probe_arg *a = malloc(sizeof *a); if(!a) return -1;
    pthread_mutex_lock(&g_bp.mu);
    if(g_bp.busy || g_bp.serial == UINT64_MAX){
        int rc = g_bp.busy ? 1 : -1; pthread_mutex_unlock(&g_bp.mu); free(a); return rc;
    }
    a->id = ++g_bp.serial; snprintf(a->mac, sizeof a->mac, "%s", mac);
    g_bp.id = a->id; g_bp.generation = generation; g_bp.busy = 1; g_bp.done = 0;
    pthread_t thread;
    if(pthread_create(&thread, NULL, bt_prepare_probe_worker, a) != 0){
        g_bp.busy = 0; g_bp.id = 0; pthread_mutex_unlock(&g_bp.mu); free(a); return -1;
    }
    *id = a->id; pthread_detach(thread); pthread_mutex_unlock(&g_bp.mu); return 0;
}
int bt_prepare_probe_read(uint64_t id, unsigned generation, bc_snapshot *s, int *known, unsigned *avail){
    if(!s || !known || !avail) return -1;
    pthread_mutex_lock(&g_bp.mu);
    int rc = -1;
    if(id && id == g_bp.id && generation == g_bp.generation && generation == ipc_generation()){
        rc = g_bp.done ? (g_bp.good ? 1 : -1) : 0;
        if(rc == 1){ *s = g_bp.selected; *known = g_bp.known; *avail = g_bp.available; }
    }
    pthread_mutex_unlock(&g_bp.mu); return rc;
}
void bt_prepare_probe_cancel(uint64_t id){
    pthread_mutex_lock(&g_bp.mu);
    if(g_bp.id == id){ g_bp.id = 0; g_bp.done = 0; }
    pthread_mutex_unlock(&g_bp.mu);
}
int bt_prepare_codec_frame(const char *mac, int choice, char *out, int capacity){
    return choice < 0 || choice >= BT_CODEC_N ? -1 : bt_codec_frame_value(mac, choice, out, capacity);
}
#endif
/* Full stock bring-up: download the chip firmware patch over /dev/ttyS0 (creates a
 * working hci0 - the step our old bluez-only path lacked), then bluetoothd + agent. */
static void bt_enable(void){
    bootrestore_cancel();   /* an explicit enable has its own radio poll */
    /* Guard against a SECOND concurrent bring-up: if the marker is already present a bring-up is
     * in flight, and starting another detached subshell would have the two kill each other's
     * patchram/daemons (e.g. boot-restore + a user toggle firing close together) -> BT left broken.
     * bt_disable() removes the marker, so a real off->on still re-enables. */
    if(access("/tmp/bt_enabling", F_OK) == 0) return;
    /* Create the cancel marker SYNCHRONOUSLY before the backgrounded subshell, so it exists by
     * the time we return. bt_enable()/bt_disable() are both main-thread + serialized, so a
     * later bt_disable() `rm` always beats the async subshell - no touch-vs-rm race. */
    system("touch /tmp/bt_enabling 2>/dev/null");
    system(
        "( killall -9 fiio_bluetoothctl brcm_patchram_plus bluetoothd bluealsa 2>/dev/null; "
        "  hciconfig hci0 down 2>/dev/null; "
        /* power-cycle the BT core (BT_REG_ON via rfkill) BEFORE patchram, so the chip
         * re-syncs whether this is a fresh boot OR a re-enable after a prior patchram
         * (without the block/unblock cycle, re-patchram on an already-firmwared chip
         * hangs and hci0 never appears - verified on-device). */
        "  rfkill block bluetooth; sleep 1; "
        "  [ -e /tmp/bt_enabling ] || exit; "     /* disabled during the block? leave BT blocked (off) */
        "  rfkill unblock bluetooth; sleep 1; "
        "  [ -e /tmp/bt_enabling ] || exit; "
        "  brcm_patchram_plus --enable_lpm --enable_hci --no2bytes --tosleep 200000 --baudrate 3000000 "
        "    --patchram /lib/firmware/bt_bcm/BCM4343A1_001.002.009.1026.1055.hcd /dev/ttyS0 >/tmp/patchram.log 2>&1 & "
        /* patchram can take well over 5s to attach hci0; keep retrying `up` until it's
         * RUNNING (up to ~20s) so we never give up before the chip is ready and leave hci0
         * DOWN (which made bt_on() report "off" and the toggle time out). Each pass re-checks
         * the marker so a toggle-off aborts the loop promptly. */
        "  i=0; while [ \"$i\" -lt 40 ]; do [ -e /tmp/bt_enabling ] || exit; hciconfig hci0 up 2>/dev/null; "
        "    hciconfig hci0 2>/dev/null | grep -q RUNNING && break; sleep 0.5; i=$((i+1)); done; "
        "  [ -e /tmp/bt_enabling ] || exit; "     /* disabled during bring-up -> don't start the daemons */
        "  /usr/project/bluetoothd --noplugin=sap --plugin=a2dp,avrcp --mode=source >/tmp/btd.log 2>&1 & "
        "  bluealsa -S --device=hci0 --profile=a2dp-source --sbc-quality=medium "
        "    --ldac-abr --ldac-quality=standard --codec=sbc --initial-volume=48 >/tmp/bluealsa.log 2>&1 & "
        /* hci0 UP alone leaves bluez Powered:no - power the controller on explicitly (needs
         * bluetoothd, hence after its start) so bt_on()'s "Powered: yes" check passes. */
        "  sleep 1; [ -e /tmp/bt_enabling ] || exit; "
        "  bluetoothctl power on; hciconfig hci0 piscan; hciconfig hci0 class 0x200414; "
        "  bluetoothctl agent on; bluetoothctl default-agent; bluetoothctl pairable on; "
        "  rm -f /tmp/bt_enabling "     /* enable finished: drop the marker */
        ") >/dev/null 2>&1 &");
    /* NB: bluealsa is started INSIDE the subshell above (after hci0 is RUNNING +
     * bluetoothd), not here, so it never races the patchram attach. */
}
static void bt_disable(void){
    bootrestore_cancel();   /* Off must also cancel the independent boot observer */
#ifdef DISKOS_TEST_BT_PREPARE
    ui_bt_route_prepare_cancel();
#endif
    scan_abort();          /* cancel any pending/active scan + the bt_open observer (covers the radio-timeout
                            * OFF path, which reaches here without a caller-side scan_abort). LVGL-touching,
                            * so it MUST stay on the main thread. */
    bt_autoroute_stop();
    /* Tear the stack down in a BACKGROUNDED subshell so the UI never blocks: `bluetoothctl power off`
     * stalls for the D-Bus timeout if bluetoothd is wedged, and running that synchronously on the main
     * thread froze the whole UI on a BT-off tap (Reddit report 2026-09-21). The device has no `timeout`
     * binary, so instead of a graceful daemon power-off (which could hang the subshell before the kill)
     * we KILL the daemons FIRST - direct, fast, no D-Bus round-trip - then drop the interface and
     * rfkill-block. rm the enable marker first so any in-flight bt_enable() subshell aborts. */
    system("( rm -f /tmp/bt_enabling; "
           "killall -9 bluealsa bluetoothd brcm_patchram_plus fiio_bluetoothctl bt-agent 2>/dev/null; "
           "hciconfig hci0 down >/dev/null 2>&1; "
           "rfkill block bluetooth >/dev/null 2>&1 ) >/dev/null 2>&1 &");
}

/* A Bluetooth MAC must be exactly AA:BB:CC:DD:EE:FF (hex + colons) before it is ever
 * interpolated into a bluetoothctl shell command. bluez scan output is already this
 * format, so this is defensive: a malformed/hostile address can carry no shell
 * metacharacters past this gate. */
static int bt_mac_valid(const char *mac){
    if(!mac || strlen(mac) != 17) return 0;
    for(int i = 0; i < 17; i++){
        char c = mac[i];
        if((i % 3) == 2){ if(c != ':') return 0; }
        else if(!((c>='0'&&c<='9')||(c>='A'&&c<='F')||(c>='a'&&c<='f'))) return 0;
    }
    return mac[17] == 0;
}

static int bt_info_prop(const char *buf, const char *key, char *out, int cap);
/* A missing PCM is not a missing radio link (codec rebuilds remove PCMs).
 * Return 1 for confirmed down, 0 for connected, -1 for unknown, -2 for pending. */
static int bt_link_disconnected(const char *mac, int timeout_ms){
    if(timeout_ms <= 0 || !bt_mac_valid(mac)) return -1;
    char cmd[160], info[4097], value[16], device[32];
    const char *marker = "\nDISKOS_INFO_DONE\n";
    snprintf(cmd, sizeof cmd, "bluetoothctl info %s 2>/dev/null && printf '\\nDISKOS_INFO_DONE\\n'", mac);
    bt_capture_drop_except(cmd);
    if(run_cap_bounded(cmd, info, sizeof info, timeout_ms) == 0) return -2;
    char *done = strstr(info, marker);
    if(!done || done[strlen(marker)]) return -1;
    *done = 0;
    snprintf(device, sizeof device, "Device %s", mac);
    size_t n = strlen(device);
    /* A Name/Alias containing a property is not evidence. A failed, truncated
     * or unrecognised query is unknown, never a confirmed disconnect. */
    if(strncasecmp(info, device, n) || (info[n] != ' ' && info[n] != '\r' && info[n] != '\n')) return -1;
    if(!bt_info_prop(info, "Connected:", value, sizeof value)) return -1;
    return !strcmp(value, "no") ? 1 : !strcmp(value, "yes") ? 0 : -1;
}

/* Route once per connected bluealsa A2DP sink. Keeping the MAC latched while the
 * PCM exists preserves a manual switch back to analog until the sink reconnects. */
static void bt_autoroute_poll_cb(lv_timer_t *t){
    if(t)lv_timer_pause(t);
    g_autoroute_wait=0;
    /* Receiver audio stays on the current route: no PCM probe, no route, and nothing latched, so the first poll
     * after Return to Local routes a connected speaker as usual (route_bt in main.c refuses it too). */
    if(ui_receiver_active()){ bt_capture_drop_except(NULL); return; }
    /* The post-restart settle guard lives in ui_route_bt (covers every routing path); this poll keeps
     * firing during the window and routes on the first tick past it (ui_route_bt returns "not routed"
     * meanwhile, so g_bt_autorouted is not latched and the retry stands). */
    static char missing[20];
    static unsigned misses, absent_polls;
    static int recovery_started;
    static uint32_t loss_sample_due;static int loss_sample_armed;
    char path[2048] = {0}, mac[20] = "";
    int found = 0, routed_found = 0;
    const char *routed = ui_route_mac();
    /* No pipeline: only the successful CLI's final marker confirms a full list.
     * A timeout, failed command or full capture without the marker is unknown. */
    uint32_t probe_start = lv_tick_get();
    int result = run_cap_bounded(
        "bluealsa-cli list-pcms 2>/dev/null && printf '\nDISKOS_LIST_DONE\n'",
        path, sizeof path, 250);
    if(!result){g_autoroute_wait=1;return;} /* Pending is not a completed unknown observation. */
    char *done = strstr(path, "\nDISKOS_LIST_DONE\n");
    if(!done || done[strlen("\nDISKOS_LIST_DONE\n")]){
        if(misses < 2) misses = 0;
        absent_polls = 0;
        if(t)lv_timer_resume(t);
        return;
    }
    *done = 0;
    for(char *line = path; line && *line; ){
        char *nl = strchr(line, '\n'); if(nl) *nl = 0;
        char *dev = strstr(line, "dev_");
        if(dev && strlen(dev + 4) >= 25 && !strncmp(dev + 21, "/a2dpsrc/", 9)){
            char candidate[20];
            memcpy(candidate, dev + 4, 17); candidate[17] = 0;
            for(int i = 0; i < 17; i++){
                if(candidate[i] == '_') candidate[i] = ':';
                if(candidate[i] >= 'a' && candidate[i] <= 'f') candidate[i] -= 32;
            }
            if(bt_mac_valid(candidate)){
                found = 1;
                if(!mac[0] && bt_route_allowed(candidate)) strcpy(mac, candidate);   /* routed_found stays complete */
                if(!strcmp(candidate, routed)) routed_found = 1;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    int loss_accepted = ui_route_bt_loss_accepted();
    if(routed_found || loss_accepted ||
       (!routed[0] && (found || !g_bt_autorouted[0])))
        bt_capture_drop_except(NULL);
    if(routed[0] && (!routed_found || loss_accepted)){
        if(t)lv_timer_resume(t);
        if(strcmp(missing, routed)){
            snprintf(missing, sizeof missing, "%s", routed);
            misses = absent_polls = 0; recovery_started = 0;loss_sample_armed=0;
        }
        if(loss_sample_armed && (int32_t)(lv_tick_get()-loss_sample_due)<0)return;
        loss_sample_armed=1;loss_sample_due=lv_tick_get()+3000;
        if(!loss_accepted){
            /* Share a 350 ms probe budget, reserving 100 ms for cleanup. */
            int left = 250 - (int)lv_tick_elaps(probe_start);
            int disconnected = bt_link_disconnected(routed, left);
            if(disconnected == 0){ misses = absent_polls = 0; recovery_started = 0; return; }
            if(absent_polls < 10) absent_polls++;
            if(disconnected == -2 && absent_polls < 10){g_autoroute_wait=1;return;}
            if(disconnected < 0){
                misses = 0;
                /* Ten consecutive successful lists missing this PCM. Pending
                 * list queries do not count. Brief rebuild gaps do not qualify;
                 * connected evidence or a returning PCM resets the grace. */
                if(absent_polls < 10) return;
                misses = 2;
            } else if(absent_polls >= 10) misses = 2;
        }
        /* Two confirmed-down polls, or the sustained unknown fallback.
         * Once accepted, finish teardown even if the PCM reappears. */
        if(misses < 2) misses++;
        if(misses == 2){
#ifdef DISKOS_TEST_BT_PREPARE
            ui_bt_route_prepare_cancel();
#endif
            int rc = ui_route_bt_lost(!recovery_started && !loss_accepted);
            recovery_started = 1;
            if(rc == 0){ g_bt_autorouted[0] = 0; misses = absent_polls = 0; missing[0] = 0; }
        }
        return;
    }
    misses = absent_polls = 0; missing[0] = 0; recovery_started = 0; loss_sample_armed=0;
    if(routed_found && bt_route_allowed(routed)) snprintf(mac, sizeof mac, "%s", routed);
    if(!found){
        /* PCM rebuilds do not end a manual Analog choice. Clear the latch only
         * after a full BlueZ snapshot establishes a disconnected link. */
        if(g_bt_autorouted[0]){
            int disconnected=bt_link_disconnected(g_bt_autorouted,250);
            if(disconnected==-2){g_autoroute_wait=1;return;}
            if(disconnected!=1){if(disconnected<0 && t)lv_timer_resume(t);return;}
        }
        g_bt_autorouted[0] = 0;
#ifdef DISKOS_TEST_BT_PREPARE
        ui_bt_route_prepare_cancel();
#endif
        return;
    }
    if(mac[0] && strcmp(mac, g_bt_autorouted)){
        if(ui_route_bt_automatic(mac) == 0)     /* latch only on a successful route, else retry on the next poll */
            snprintf(g_bt_autorouted, sizeof g_bt_autorouted, "%s", mac);
        else if(t)lv_timer_resume(t);
    }
}

static void bt_autoroute_start(void){
    if(g_bt_autoroute_timer) return;
    g_bt_autoroute_timer = lv_timer_create(bt_autoroute_poll_cb, 3000, NULL);
}
static void bt_autoroute_stop(void){
    bt_capture_drop_except(NULL);
    if(g_bt_autoroute_timer){ lv_timer_del(g_bt_autoroute_timer); g_bt_autoroute_timer = NULL; }
    g_bt_autorouted[0] = 0;
}
/* The player restarted: a fresh mq_player defaults to local/analog output, so any "already routed to X"
 * memory is stale. Forget it so the auto-route poll re-routes the still-connected speaker (and so the
 * local re-init isn't wrongly suppressed). If BT is on but the poll timer died, re-arm it. */
void bt_notify_player_restart(void){
#ifdef DISKOS_TEST_BT_PREPARE
    ui_bt_route_prepare_cancel();
#endif
    g_bt_autorouted[0] = 0;
    if(g_bt_autoroute_timer){lv_timer_resume(g_bt_autoroute_timer);lv_timer_ready(g_bt_autoroute_timer);}
    else if(bt_on())bt_autoroute_start();
}

/* ---- pair + connect ----------------------------------------------------- */
/* Extract a `bluetoothctl info` property value: the line whose first non-blank token is `key`
 * (e.g. "Connected:" / "Icon:"), returning its trimmed value. Line-anchored so a Name:/Alias:
 * value that merely CONTAINS the text can't spoof it. Fills out + returns 1 on match, else out="",0. */
static int bt_info_prop(const char *buf, const char *key, char *out, int cap){
    if(cap <= 0) return 0;
    size_t klen = strlen(key);
    for(const char *l = buf; l && *l; ){
        const char *p = l; while(*p==' '||*p=='\t') p++;
        if(!strncmp(p, key, klen)){
            p += klen; while(*p==' '||*p=='\t') p++;
            int i=0; while(p[i] && p[i]!='\n' && p[i]!='\r' && i<cap-1){ out[i]=p[i]; i++; }
            while(i>0 && (out[i-1]==' '||out[i-1]=='\t')) i--;   /* trim trailing whitespace so "yes " compares == "yes" */
            out[i]=0; return 1;
        }
        const char *nl = strchr(l, '\n'); l = nl ? nl+1 : NULL;
    }
    out[0]=0;
    return 0;
}
/* ---- operations: the details query and drained children ----------------------------------------------
 * Nothing here waits: a command goes to the bounded child facility (procrun.c) and one 200 ms UI timer polls the
 * handles. A handle is single-use and belongs to exactly one owner, so a cancelled query is only drained (its
 * result is read and thrown away) and can never change what the UI reports. */
#define BT_PAIR_MS    20000
#define BT_TRUST_MS    4000
#define BT_CONNECT_MS 12000
#define BT_INFO_MS     1500
#define BT_JOB_MS      6000
enum { AT_IDLE, AT_PAIR, AT_TRUST, AT_CONNECT, AT_ROUTE };   /* AT_ROUTE: error naming only */
static struct { unsigned h; int stage; char mac[20]; uint32_t begin; } g_at;   /* the one current attempt */
static struct { unsigned h; int kind; char mac[20]; } g_jobs[PROC_SLOTS];                    /* each holds a facility slot */
static struct { unsigned h; int on; char cmd[128]; uint32_t begin; } g_inf;    /* the details query */
static lv_obj_t *g_row_obj, *g_row_val, *g_inf_name, *g_inf_conn, *g_inf_paired;   /* last info_row; details value labels */
static void info_row(const char *key, const char *val);   /* fwd */
static lv_timer_t *g_ops_timer;
static void bt_ops_ensure(void);

static void bt_job_add(unsigned h, int kind, const char *mac){
    if(!h) return;
    for(int i = 0; i < PROC_SLOTS; i++) if(!g_jobs[i].h){
        g_jobs[i].h = h;
        g_jobs[i].kind = kind;
        snprintf(g_jobs[i].mac, sizeof g_jobs[i].mac, "%s", mac ? mac : "");
        bt_ops_ensure();
        return;
    }
    proc_cancel(h);   /* unreachable: each entry holds one of the facility's PROC_SLOTS slots */
}
static int bt_job_pending(const char *mac){
    for(int i = 0; i < PROC_SLOTS; i++)
        if(g_jobs[i].h && !strcmp(g_jobs[i].mac, mac)) return 1;
    return 0;
}
static void bt_jobs_step(void){
    for(int i = 0; i < PROC_SLOTS; i++){
        char out[16];
        if(!g_jobs[i].h) continue;
        int r = proc_poll(g_jobs[i].h, out, sizeof out);
        if(!r) continue;
        int kind = g_jobs[i].kind;
        g_jobs[i].h = 0;
        if(kind == JOB_FORGET){ ui_toast(r == PROC_COMPLETE ? "Device forgotten" : "Couldn't forget device"); scan_kick(); }
    }
}
static int bt_disconnect_job(const char *mac){      /* 0: not submitted (every facility slot is in use) */
    char cmd[128];
    snprintf(cmd, sizeof cmd, "bluetoothctl disconnect %s >/dev/null 2>&1", mac);
    unsigned h = proc_start(cmd, BT_JOB_MS, 16);
    if(!h) return 0;
    bt_job_add(h, JOB_DRAIN, mac);
    return 1;
}
/* Cancel the current attempt: its child group is killed and its result drained. cleanup: also ask the daemon to drop
 * a connect that may still complete late (the killed client cannot take a Connect back). Routing permission is
 * revoked either way: a late Connected or PCM for this MAC must not route. */
static void bt_attempt_cancel(int cleanup){
    if(g_at.stage == AT_IDLE) return;
    if(g_at.h){ proc_cancel(g_at.h); bt_job_add(g_at.h, JOB_DRAIN, NULL); }
    bt_route_revoke(g_at.mac);
    if(cleanup && g_at.stage == AT_CONNECT) bt_disconnect_job(g_at.mac);
    g_at.stage = AT_IDLE; g_at.h = 0;
}
static const char *bt_stage_error(int stage){
    switch(stage){
    case AT_PAIR:    return tr("Pairing failed");
    case AT_TRUST:   return tr("Trust failed");
    case AT_CONNECT: return tr("Connection failed");
    default:         return tr("Audio route failed");
    }
}
static void bt_attempt_fail(void){
    int stage = g_at.stage;
    g_at.stage = AT_IDLE; g_at.h = 0;
    ui_toast(bt_stage_error(stage));
    scan_kick();                        /* re-list the current devices, no fresh discovery */
}
/* Each stage is "<verb> then bluetoothctl info" and passes only when info shows the property is "yes": a command's
 * exit status alone is never trusted. Only the verified Connected of the CURRENT attempt authorizes routing. */
static void bt_attempt_step(void){
    static const struct { const char *verb, *prop; int ms; } S[] = {
        {0, 0, 0}, {"pair", "Paired:", BT_PAIR_MS}, {"trust", "Trusted:", BT_TRUST_MS}, {"connect", "Connected:", BT_CONNECT_MS} };
    if(g_at.stage == AT_IDLE) return;
    if(g_at.stage == AT_PAIR && !g_at.h && bt_job_pending(g_at.mac)){
        g_at.begin = lv_tick_get();
        return;
    }
    int left = S[g_at.stage].ms - (int)lv_tick_elaps(g_at.begin);
    if(!g_at.h){                        /* not submitted yet (or no facility slot free): retry until the stage budget ends */
        char cmd[256];
        if(left <= 0){ bt_attempt_fail(); return; }
        snprintf(cmd, sizeof cmd, "bluetoothctl %s %s >/dev/null 2>&1; bluetoothctl info %s 2>/dev/null",
                 S[g_at.stage].verb, g_at.mac, g_at.mac);
        g_at.h = proc_start(cmd, left, 2048);
        return;
    }
    char out[2048], v[32];
    int r = proc_poll(g_at.h, out, sizeof out);
    if(!r) return;
    g_at.h = 0;
    if(r != PROC_COMPLETE || !bt_info_prop(out, S[g_at.stage].prop, v, sizeof v) || strcmp(v, "yes")){ bt_attempt_fail(); return; }
    if(g_at.stage < AT_CONNECT){ g_at.stage++; g_at.begin = lv_tick_get(); return; }
    /* Connected at the BT layer. Only now authorize and route; a failed route keeps the authorization so the
     * autoroute poll retries (g_bt_autorouted stays unset). */
    char mac[20];
    snprintf(mac, sizeof mac, "%s", g_at.mac);
    g_at.stage = AT_IDLE;
    bt_route_own(mac);
    if(ui_route_bt(mac) == 0){
        snprintf(g_bt_autorouted, sizeof g_bt_autorouted, "%s", mac);
        ui_toast("Connected");
    } else {
        ui_toast(bt_stage_error(AT_ROUTE));
    }
    scan_kick();                        /* instant re-list (device already known) -> checkmark, no 13s re-scan */
}
/* Details: a pending query shows "Checking..."; any incomplete query (timeout, failure, no slot) shows Unknown. */
static lv_obj_t *g_rename, *g_audio, *g_info_button, *g_info_button_label;
static int g_info_connected;
static char g_info_name[249];
static void bt_info_apply(const char *buf){
    char v[249];
    const char *display=
        ((bt_info_prop(buf,"Alias:",v,sizeof v) && v[0]) ||
         (bt_info_prop(buf,"Name:",v,sizeof v) && v[0])) ? v : buf && *buf ? g_sel_mac : tr("Unknown");
    snprintf(g_info_name,sizeof g_info_name,"%s",display);
    if(g_inf_name) lv_label_set_text(g_inf_name,g_info_name);
    int paired=bt_info_prop(buf,"Paired:",v,sizeof v) && !strcmp(v,"yes");
    if(g_rename) { if(paired) lv_obj_remove_state(g_rename,LV_STATE_DISABLED); else lv_obj_add_state(g_rename,LV_STATE_DISABLED); }
    g_info_connected=bt_info_prop(buf,"Connected:",v,sizeof v) && !strcmp(v,"yes");
    if(g_audio){ if(g_info_connected) lv_obj_remove_state(g_audio,LV_STATE_DISABLED); else lv_obj_add_state(g_audio,LV_STATE_DISABLED); }
    if(g_info_button_label) lv_label_set_text(g_info_button_label,tr(g_info_connected ? "Disconnect" : "Connect"));
    if(g_info_button){
        if(bt_info_prop(buf,"Connected:",v,sizeof v)) lv_obj_remove_state(g_info_button,LV_STATE_DISABLED);
        else lv_obj_add_state(g_info_button,LV_STATE_DISABLED);
    }
    if(g_inf_conn) lv_label_set_text(g_inf_conn, bt_info_prop(buf, "Connected:", v, sizeof v) ? (!strcmp(v, "yes") ? "Yes" : "No") : tr("Unknown"));
    if(g_inf_paired) lv_label_set_text(g_inf_paired, bt_info_prop(buf, "Paired:", v, sizeof v) ? (!strcmp(v, "yes") ? "Yes" : "No") : tr("Unknown"));
    if(g_inf_name && bt_info_prop(buf, "Icon:", v, sizeof v)){
        info_row("Type", v);
        lv_obj_move_to_index(g_row_obj, 4);   /* between Paired and Audio, as before */
    }
}
static void bt_info_cancel(void){
    if(g_inf.h){ proc_cancel(g_inf.h); bt_job_add(g_inf.h, JOB_DRAIN, NULL); }
    g_inf.h = 0; g_inf.on = 0;
}
void bt_info_close(void){ bt_info_cancel(); }
static void bt_info_begin(const char *mac){
    bt_info_cancel();
    snprintf(g_inf.cmd, sizeof g_inf.cmd, "bluetoothctl info %s 2>/dev/null", mac);
    g_inf.on = 1; g_inf.begin = lv_tick_get();
    g_inf.h = proc_start(g_inf.cmd, BT_INFO_MS, 2048);
    bt_ops_ensure();
}
static void bt_info_step(void){
    char out[2048];
    if(!g_inf.on) return;
    int left = BT_INFO_MS - (int)lv_tick_elaps(g_inf.begin);
    if(!g_inf.h){                       /* no facility slot yet: retry until the budget ends, then Unknown */
        if(left > 0){ g_inf.h = proc_start(g_inf.cmd, left, sizeof out); return; }
        out[0] = 0;
    } else {
        int r = proc_poll(g_inf.h, out, sizeof out);
        if(!r) return;
        if(r != PROC_COMPLETE) out[0] = 0;
    }
    g_inf.h = 0; g_inf.on = 0;
    bt_info_apply(out);
}
static int bt_ops_busy(void){
    for(int i = 0; i < PROC_SLOTS; i++) if(g_jobs[i].h) return 1;
    return g_at.stage != AT_IDLE || g_inf.on;
}
static void bt_ops_cb(lv_timer_t *t){
    for(int i=0;i<3;i++){
        int before=g_at.stage;bt_attempt_step();
        if(g_at.stage==before || g_at.h || g_at.stage==AT_IDLE)break;
    }
    bt_jobs_step();bt_info_step();
    if(!bt_ops_busy()){if(t)lv_timer_del(t);g_ops_timer=NULL;}
    else if(t){
        if((g_at.stage!=AT_IDLE && !g_at.h) || (g_inf.on && !g_inf.h))lv_timer_resume(t);
        else lv_timer_pause(t);
    }
}
static void bt_ops_ensure(void){ if(!g_ops_timer) g_ops_timer = lv_timer_create(bt_ops_cb, 200, NULL); }
/* ---- end operations ---- */

/* A new attempt (or a cancel) supersedes the old one; pairing can be slow, so each stage has its own budget. */
static void bt_connect(const char *mac){
    if(!bt_mac_valid(mac)){ ui_toast("Bad device address"); return; }
    bt_attempt_cancel(strcmp(g_at.mac, mac) != 0);   /* a re-tap of the same device needs no disconnect */
    bt_route_own(NULL);                               /* before submission: nothing is authorized while this is pending */
    snprintf(g_at.mac, sizeof g_at.mac, "%s", mac);
    g_at.stage = AT_PAIR; g_at.h = 0; g_at.begin = lv_tick_get();
    ui_toast("Connecting...");
    bt_ops_ensure();
    bt_attempt_step();
}
static void bt_disconnect(const char *mac){
    if(!bt_mac_valid(mac)) return;
    if(!strcmp(mac, g_bt_autorouted)) g_bt_autorouted[0] = 0;
    bt_route_revoke(mac);
    if(g_at.stage != AT_IDLE && !strcmp(g_at.mac, mac)) bt_attempt_cancel(0);
    if(!strcmp(mac, ui_route_mac())) ui_route_analog();
    ui_toast(bt_disconnect_job(mac) ? "Disconnecting..." : tr("Connection failed"));
}
/* Forget = remove (BlueZ drops the link first). The result toast arrives when the job finishes. */
static void bt_forget(const char *mac){
    if(!bt_mac_valid(mac)) return;
    if(!strcmp(mac, g_bt_autorouted)) g_bt_autorouted[0] = 0;
    bt_route_revoke(mac);
    if(g_at.stage != AT_IDLE && !strcmp(g_at.mac, mac)) bt_attempt_cancel(0);
    if(!strcmp(mac, ui_route_mac())) ui_route_analog();
    char cmd[128];
    snprintf(cmd, sizeof cmd, "bluetoothctl remove %s >/dev/null 2>&1", mac);
    unsigned h = proc_start(cmd, BT_JOB_MS, 16);
    if(!h){ ui_toast("Couldn't forget device"); return; }
    bt_job_add(h, JOB_FORGET, mac);
}
/* Tapping a connected sink is an explicit choice: it replaces any earlier ownership, ends an older attempt and
 * authorizes this MAC. */
static void bt_route_select(const char *mac){
    if(bt_job_pending(mac)){
        bt_connect(mac);
        return;
    }
    bt_attempt_cancel(strcmp(g_at.mac, mac) != 0);
    bt_route_own(mac);
    ui_route_bt(mac);
}

/* ---- details screen (SCR_BT_INFO) --------------------------------------- */
static void info_row(const char *key, const char *val){
    lv_obj_t *r = lv_obj_create(g_info_list);
    g_row_obj = r;
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 250, 40);
    lv_obj_set_style_radius(r, 8, 0);
    lv_obj_set_style_bg_color(r, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_50, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *k = lv_label_create(r);
    lv_label_set_text(k, key);
    lv_obj_set_pos(k, 12, 11);
    lv_obj_set_style_text_font(k, TF(UI_14), 0);
    lv_obj_set_style_text_color(k, TC(TEXT_MUTED), 0);
    lv_obj_t *v = lv_label_create(r);
    g_row_val = v;
    lv_label_set_text(v, val && val[0] ? val : "-");
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(v, 96, 11); lv_obj_set_size(v, 142, 18);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(v, TF(USER_14), 0);   /* "Name" value = device name: Cyrillic/CJK-capable (issue #3) */
    lv_obj_set_style_text_color(v, TC(TEXT_PRIMARY), 0);
}
/* Forget (unpair + untrust) the selected device, then return to the list + rescan (C13). */
static void info_forget_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    bt_info_cancel();
    bt_forget(g_sel_mac);       /* nonblocking: the result toast follows when the job ends */
    screen_back();
    if(g_scan_timer) lv_timer_del(g_scan_timer);
    g_scan_timer = lv_timer_create(scan_timer_cb, 3000, NULL);
    lv_timer_set_repeat_count(g_scan_timer, 1);
}
static void info_action_row(const char *label, lv_event_cb_t cb){
    lv_obj_t *r = lv_button_create(g_info_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 250, 44);
    lv_obj_set_style_radius(r, 8, 0);
    int danger=!strcmp(label,"Forget This Device");
    lv_obj_set_style_bg_color(r, danger ? TC(DANGER_SURFACE) : TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, TC(DANGER_SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    ui_on(r, cb, LV_EVENT_CLICKED, NULL, "bt.cb", UI_CORE);
    lv_obj_t *t = lv_label_create(r);
    lv_label_set_text(t, label); lv_obj_center(t);
    lv_obj_set_style_text_font(t, TF(UI_14), 0);
    lv_obj_set_style_text_color(t, danger ? TC(STATUS_DANGER) : TC(TEXT_PRIMARY), 0);
}
static void info_audio_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED || !g_info_connected) return;
    if(ui_receiver_active()) ui_toast("Return to Local first");
    else if(ui_cast_session_active()) ui_toast("Stop beaming to change this");
    else bt_route_select(g_sel_mac);
}
static void info_rename_cb(lv_event_t *e){
    if(lv_event_get_code(e)==LV_EVENT_CLICKED && g_inf_name)
        dnui_paired(g_sel_mac,g_info_name);
}
static void info_disc_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    bt_info_cancel();
    if(g_info_connected) bt_disconnect(g_sel_mac); else bt_connect(g_sel_mac);
    screen_back();
    /* re-list shortly so the now-disconnected device loses its checkmark / blue styling
     * (mirrors the connect path; disconnect is async via bluetoothctl). */
    if(g_scan_timer) lv_timer_del(g_scan_timer);
    g_scan_timer = lv_timer_create(scan_timer_cb, 4000, NULL);
    lv_timer_set_repeat_count(g_scan_timer, 1);
}
static void info_back_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED){ bt_info_cancel(); screen_back(); } }

void bt_info_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    ui_header_cb(root, "Device", info_back_cb);   /* shared header */
    g_info_list = lv_obj_create(root);
    lv_obj_remove_style_all(g_info_list);
    lv_obj_set_pos(g_info_list, 55, 78); lv_obj_set_size(g_info_list, 250, 190);
    lv_obj_set_style_bg_opa(g_info_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_row(g_info_list, 8, 0);
    lv_obj_set_flex_flow(g_info_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_info_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(g_info_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_info_list, LV_SCROLLBAR_MODE_OFF);

    /* Disconnect button under the info rows */
    lv_obj_t *db = lv_button_create(root);
    g_info_button=db;
    lv_obj_remove_style_all(db);
    lv_obj_set_pos(db, 188, 278); lv_obj_set_size(db, 108, 38);
    lv_obj_set_ext_click_area(db,6);
    lv_obj_set_style_radius(db, 12, 0);
    lv_obj_set_style_bg_color(db, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(db, LV_OPA_COVER, 0);
    ui_on(db, info_disc_cb, LV_EVENT_CLICKED, NULL, "bt.info_disc", UI_CORE);
    lv_obj_t *dl = lv_label_create(db);
    g_info_button_label=dl;
    lv_label_set_text(dl, "Disconnect");
    lv_obj_center(dl);
    lv_obj_set_style_text_font(dl, TF(UI_14), 0);
    lv_obj_set_style_text_color(dl, TC(TEXT_PRIMARY), 0);
    g_rename=lv_button_create(root); lv_obj_remove_style_all(g_rename);
    lv_obj_set_pos(g_rename,64,278); lv_obj_set_size(g_rename,108,38);
    lv_obj_set_ext_click_area(g_rename,6);
    lv_obj_set_style_radius(g_rename,12,0);
    lv_obj_set_style_bg_color(g_rename,TC(SURFACE),0);
    lv_obj_set_style_bg_opa(g_rename,LV_OPA_COVER,0);
    ui_on(g_rename,info_rename_cb,LV_EVENT_CLICKED,NULL,"bt.rename",UI_CORE);
    lv_obj_t *rename_label=lv_label_create(g_rename); lv_label_set_text(rename_label,tr("Rename"));
    lv_obj_set_style_text_font(rename_label,TF(UI_14),0);
    lv_obj_set_style_text_color(rename_label,TC(TEXT_PRIMARY),0); lv_obj_center(rename_label);

}

static void bt_info_build(void){
    bt_info_cancel();
    lv_obj_clean(g_info_list);
    g_inf_name = g_inf_conn = g_inf_paired = NULL;
    g_info_name[0]=0;
    if(g_info_button) lv_obj_add_state(g_info_button,LV_STATE_DISABLED);
    const char *wait = tr("Checking...");   /* the query runs in the background: opening never waits for it */
    info_row("Name", wait);      g_inf_name = g_row_val;
    int lh=lv_font_get_line_height(lv_obj_get_style_text_font(g_inf_name,LV_PART_MAIN));
    int ls=lv_obj_get_style_text_line_space(g_inf_name,LV_PART_MAIN);
    lv_obj_set_height(g_inf_name,2*lh+ls);
    lv_obj_set_height(g_row_obj,2*lh+ls+22);
    info_row("Address", g_sel_mac);
    info_row("Connected", wait); g_inf_conn = g_row_val;
    info_row("Paired", wait);    g_inf_paired = g_row_val;
    info_row("Audio", "On (beta)");   /* routing works; SBC over this CPU can be rough. short: value label is 142px */
    info_action_row("Use for audio",info_audio_cb);
    g_audio=lv_obj_get_child(g_info_list,-1); lv_obj_add_state(g_audio,LV_STATE_DISABLED);
    if(g_rename) lv_obj_add_state(g_rename,LV_STATE_DISABLED);
    info_action_row("Forget This Device", info_forget_cb);   /* C13: unpair + untrust */
}
void bt_info_open(void){
    if(!g_info_list || !bt_mac_valid(g_sel_mac)) return;
    bt_info_build(); screen_show(SCR_BT_INFO); bt_info_begin(g_sel_mac);
}
#ifdef DISKOS_HOST
void bt_info_fixture(const char *alias,int connected){
    strcpy(g_sel_mac,"AA:BB:CC:DD:EE:FF"); bt_info_build();
    char info[768]; snprintf(info,sizeof info,"Device %s (public)\n\tAlias: %s\n\tName: Original headphones\n\tConnected: %s\n\tPaired: yes\n\tIcon: audio-headphones\n",g_sel_mac,alias,connected?"yes":"no");
    bt_info_apply(info); screen_show(SCR_BT_INFO);
}
#endif

void bt_info_refresh(void){
    if(g_inf_name) lv_label_set_text(g_inf_name,tr("Checking..."));
    bt_info_begin(g_sel_mac);
}

/* ---- device list -------------------------------------------------------- */
static void row_free_cb(lv_event_t *e){ free(lv_obj_get_user_data(lv_event_get_target(e))); }
static void dev_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    lv_obj_t *row = lv_event_get_target(e);
    const char *mac = (const char*)lv_obj_get_user_data(row);
    int connected = (intptr_t)lv_event_get_user_data(e);
    if(!mac || !mac[0]) return;
    if(connected){
        snprintf(g_sel_mac, sizeof g_sel_mac, "%s", mac);
        bt_info_open();
    } else {
        bt_connect(mac);
        list_msg("Connecting " LV_SYMBOL_BLUETOOTH);
        /* re-list shortly so the new connection picks up its checkmark */
        if(g_scan_timer) lv_timer_del(g_scan_timer);
        g_scan_timer = lv_timer_create(scan_timer_cb, 4000, NULL);
        lv_timer_set_repeat_count(g_scan_timer, 1);
    }
}

static void add_dev_row(const char *mac, const char *name, int connected){
    lv_obj_t *r = lv_button_create(g_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 268, 46);
    lv_obj_set_style_radius(r, 8, 0);
    lv_obj_set_style_bg_color(r, ((connected&1) ? TC(CONNECTED_SURFACE) : TC(SURFACE)), 0);
    lv_obj_set_style_bg_opa(r, (connected&1) ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_bg_color(r, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    char *dup = strdup(mac);
    lv_obj_set_user_data(r, dup);
    ui_on(r, dev_cb, LV_EVENT_CLICKED, (void*)(intptr_t)connected, "bt.dev", UI_CORE);
    lv_obj_add_event_cb(r, row_free_cb, LV_EVENT_DELETE, NULL);

    int tx = 12;
    if(connected & 1){
        lv_obj_t *ck = lv_label_create(r);
        lv_label_set_text(ck, LV_SYMBOL_OK);
        lv_obj_set_pos(ck, 12, 15);
        lv_obj_set_style_text_font(ck, TF(UI_14), 0);
        lv_obj_set_style_text_color(ck, TC(STATUS_INFO), 0);
        tx = 34;
    }
    lv_obj_t *t = lv_label_create(r);
    lv_label_set_text(t, name);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(t, tx, 13); lv_obj_set_size(t, 220 - tx, 20);
    lv_obj_set_style_text_font(t, TF(USER_16), 0);   /* BT device names are user data: Cyrillic/CJK-capable (issue #3) */
    lv_obj_set_style_text_color(t, TC(TEXT_PRIMARY), 0);

    lv_obj_t *ic = lv_label_create(r);
    lv_label_set_text(ic, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_pos(ic, 240, 14);
    lv_obj_set_style_text_font(ic, TF(UI_14), 0);
    lv_obj_set_style_text_color(ic, ((connected&1) ? TC(STATUS_INFO) : TC(TEXT_MUTED)), 0);
}

/* Show only real AUDIO devices (bluez Icon = audio-card / audio-headset / ...),
 * so BLE gadgets and the "LE-" advertisement entries (which carry no A2DP) are
 * hidden and the user can only tap a connectable sink.  Connected checkmark comes from
 * bluez's own "Connected: yes" (not a raw ACL link, which over-reports). */
/* Enumeration runs OFF the main thread: `bluetoothctl devices` plus a per-device
 * `bluetoothctl info` is up to ~60 blocking popen() spawns - running that inline froze
 * the LVGL loop for seconds (H5) and risked the fiio_init hardware watchdog. The worker
 * builds ONLY plain data; the main thread renders rows from it (LVGL is main-thread-only).
 * A generation counter drops a superseded worker's result if a newer scan started. */
typedef struct { char mac[20]; char name[249]; int connected; } bt_scan_dev_t;
static pthread_mutex_t g_scan_mu = PTHREAD_MUTEX_INITIALIZER;
static bt_scan_dev_t   g_scan_res[40];
static int             g_scan_n = 0;         /* [g_scan_mu] result count */
static int             g_scan_done = 0;      /* [g_scan_mu] worker finished -> main renders */
static int             g_scan_running = 0;   /* [g_scan_mu] a worker is in flight */
static unsigned        g_scan_gen = 0;       /* [g_scan_mu] bumped per scan; worker tags its result */
static lv_timer_t     *g_scanpoll_timer;
static int             g_scanpoll_n = 0;
static int             g_scan_pending = 0;   /* main-thread: a re-enumerate was requested while a worker ran */

static void *scan_worker(void *arg){
    unsigned my_gen = (unsigned)(intptr_t)arg;
    char buf[8192]; run_cap("bluetoothctl devices 2>/dev/null", buf, sizeof buf);
    bt_scan_dev_t res[40]; int n = 0;
    char *l = buf;
    while(l && *l && n < 40){
        char *nl = strchr(l, '\n'); if(nl) *nl = 0;
        /* line: "Device AA:BB:CC:DD:EE:FF Friendly Name" */
        if(!strncmp(l, "Device ", 7) && strlen(l) > 7+17){
            char mac[20]; memcpy(mac, l+7, 17); mac[17]=0;
            const char *name = l + 7 + 17 + 1;
            if(name[0] && bt_mac_valid(mac)){
                char cmd[128], info[2048];
                snprintf(cmd, sizeof cmd, "bluetoothctl info %s 2>/dev/null", mac);
                run_cap(cmd, info, sizeof info);
                char icon[64], cv[16], alias[249];   /* line-anchored: a spoofed device name can't fake Icon/Connected */
                if(bt_info_prop(info, "Icon:", icon, sizeof icon) && !strncmp(icon, "audio", 5)){  /* audio sinks only */
                    snprintf(res[n].mac,  sizeof res[n].mac,  "%s", mac);
                    const char *display=(bt_info_prop(info,"Alias:",alias,sizeof alias) && alias[0]) ||
                        (bt_info_prop(info,"Name:",alias,sizeof alias) && alias[0]) ? alias : name;
                    snprintf(res[n].name, sizeof res[n].name, "%s", display);
                    res[n].connected = (bt_info_prop(info,"Connected:",cv,sizeof cv) && !strcmp(cv,"yes"));
                    if(bt_info_prop(info,"Paired:",cv,sizeof cv) && !strcmp(cv,"yes")) res[n].connected|=2;
                    n++;
                }
            }
        }
        if(!nl) break;
        l = nl + 1;
    }
    pthread_mutex_lock(&g_scan_mu);
    if(my_gen == g_scan_gen){        /* still current (not abandoned): publish results + free the latch.
                                      * Only the current-gen worker clears g_scan_running, so an
                                      * abandoned worker can't stomp a newer scan's state. */
        memcpy(g_scan_res, res, (size_t)n * sizeof(bt_scan_dev_t));
        g_scan_n = n; g_scan_done = 1;
        g_scan_running = 0;
    }
    pthread_mutex_unlock(&g_scan_mu);
    ui_notify(UI_WAKE_BT);
    return NULL;
}

/* main thread only: render the worker's results into g_list (LVGL touched only here) */
static void scan_render(void){
    if(!g_list) return;
    scan_stop();
    lv_obj_clean(g_list);
    lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);  /* rows top-aligned */
    static bt_scan_dev_t local[40]; int n;
    pthread_mutex_lock(&g_scan_mu);
    n = g_scan_n; if(n > 40) n = 40;
    memcpy(local, g_scan_res, (size_t)n * sizeof(bt_scan_dev_t));
    pthread_mutex_unlock(&g_scan_mu);
    for(int i = 0; i < n; i++) if(local[i].connected & 1)  add_dev_row(local[i].mac, local[i].name, local[i].connected);  /* connected first */
    for(int i = 0; i < n; i++) if(!(local[i].connected & 1)) add_dev_row(local[i].mac, local[i].name, local[i].connected);
    if(n == 0) list_msg("No audio devices found");
    /* NOTE: audio output routing to the BT sink is intentionally DISABLED.
     * bluez+bluealsa does SBC encoding in software, which is too heavy for this
     * MIPS CPU (laggy audio + watchdog reboots), and the 0666 output-mux command
     * destabilises mq_player's local audio engine.  The page still pairs/connects
     * devices, but the player always uses its native DAC output.  Smooth BT audio
     * would require the stock Broadcom BSA (hardware a2dp), a separate effort. */
}

/* main thread: poll until the worker finishes, then render. Keeps waiting (does NOT
 * render stale results early); only a wedged worker (~30s) is abandoned - and then the
 * generation is bumped + the latch freed so its late write is dropped and new scans work. */
static void scanpoll_cb(lv_timer_t *t){
    (void)t;
    int done;
    pthread_mutex_lock(&g_scan_mu); done = g_scan_done; pthread_mutex_unlock(&g_scan_mu);
    if(!done){
        /* The one timeout is the original 30-s abandonment deadline. */
        pthread_mutex_lock(&g_scan_mu); g_scan_gen++; g_scan_running = 0; pthread_mutex_unlock(&g_scan_mu);
    }
    if(g_scanpoll_timer){ lv_timer_del(g_scanpoll_timer); g_scanpoll_timer = NULL; }
    if(g_scan_pending){ g_scan_pending = 0; scan_kick(); return; }   /* a refresh was queued mid-run -> re-enumerate fresh instead of rendering the stale result */
    scan_render();
}

/* (re)start the render poll timer (main thread) */
static void scan_ensure_poll(void){
    g_scanpoll_n = 0;
    if(g_scanpoll_timer) lv_timer_del(g_scanpoll_timer);
    g_scanpoll_timer = lv_timer_create(scanpoll_cb, 30000, NULL);
}

/* main thread: spawn the off-thread enumeration. If one is already running, just make
 * sure the poll is live so its result still renders (don't pile up a second worker). */
static void scan_kick(void){
    pthread_mutex_lock(&g_scan_mu);
    if(g_scan_running){   /* a worker is mid-run: queue a fresh re-enumerate for when it finishes,
                           * so a stale (e.g. pre-connect) result isn't the final render */
        pthread_mutex_unlock(&g_scan_mu); g_scan_pending = 1; scan_ensure_poll(); return;
    }
    g_scan_running = 1; g_scan_done = 0; g_scan_n = 0; g_scan_gen++;   /* clear stale results: a timed-out/failed scan then renders honestly, not the last list */
    unsigned gen = g_scan_gen;
    pthread_mutex_unlock(&g_scan_mu);
    g_scan_pending = 0;   /* this fresh worker satisfies any queued refresh */
    pthread_t th;
    if(pthread_create(&th, NULL, scan_worker, (void*)(intptr_t)gen) == 0){
        pthread_detach(th);
        scan_ensure_poll();     /* render when the worker signals done */
    } else {   /* couldn't spawn -> drop the latch, kill any stale poll, render what we have */
        pthread_mutex_lock(&g_scan_mu); g_scan_running = 0; pthread_mutex_unlock(&g_scan_mu);
        if(g_scanpoll_timer){ lv_timer_del(g_scanpoll_timer); g_scanpoll_timer = NULL; }
        scan_render();
    }
}

static void scan_timer_cb(lv_timer_t *t){
    (void)t;
    if(g_scan_timer){ lv_timer_del(g_scan_timer); g_scan_timer = NULL; }
    scan_kick();   /* enumerate off-thread; scanpoll_cb renders when done (no main-thread freeze) */
}
/* Cancel every pending scan artifact and abandon any in-flight worker (bump gen so its
 * late result is dropped). Used when Bluetooth is turned off so a delayed scan can't
 * render device rows over the "Bluetooth is off" message. */
static void scan_abort(void){
    if(g_scan_timer){ lv_timer_del(g_scan_timer); g_scan_timer = NULL; }
    if(g_scanpoll_timer){ lv_timer_del(g_scanpoll_timer); g_scanpoll_timer = NULL; }
    if(g_scanwait_timer){ lv_timer_del(g_scanwait_timer); g_scanwait_timer = NULL; }  /* cancel the bt_open observer too */
    pthread_mutex_lock(&g_scan_mu);
    g_scan_gen++; g_scan_running = 0; g_scan_n = 0; g_scan_done = 0;
    pthread_mutex_unlock(&g_scan_mu);
}
static void start_scan(void){
    if(!bt_on()){ list_msg("Bluetooth is off"); return; }
    list_msg_scanning();
    /* Classic BR/EDR inquiry - what BT speakers/headphones use - needs ~10-12s to find and
     * resolve a device's name/class; the old 6s window quit before speakers ever appeared. */
    system("bluetoothctl --timeout 13 scan on >/dev/null 2>&1 &");
    if(g_scan_timer) lv_timer_del(g_scan_timer);
    g_scan_timer = lv_timer_create(scan_timer_cb, 13500, NULL);
    lv_timer_set_repeat_count(g_scan_timer, 1);
}

/* ---- radio toggle ------------------------------------------------------- */
/* bt_enable() is slow (rfkill cycle + patchram firmware download + hci0 bring-up,
 * several seconds). A fixed settle timer would either fire too early ("No audio
 * devices found" before the radio is up) or never actively scan. Poll bt_on()
 * until the adapter is Powered, then run a real scan. */
static lv_timer_t *g_radio_timer;
static uint32_t    g_radio_start;
static void radio_on_poll_cb(lv_timer_t *t){
    if(t != g_radio_timer) return;
    if(cfg_get_int("bt_on", 0) != 1){ lv_timer_del(t); g_radio_timer = NULL; return; }
    if(bt_on()){
        lv_timer_del(g_radio_timer); g_radio_timer = NULL;
        bt_ensure_services();
        bt_autoroute_start();
        start_scan();
        return;
    }
    if(lv_tick_elaps(g_radio_start) > 28000){   /* 2s rfkill + ~20s up-loop + daemon/power settle */
        lv_timer_del(g_radio_timer); g_radio_timer = NULL;
        /* bring-up failed: drop the persisted intent so the QS tile + boot-restore don't keep
         * showing/enforcing "on" for a radio that never came up. Tear the stack down so the cleared
         * intent matches a real off state (a late/half-started enable can't linger powered), and
         * sync the screen switch. */
        cfg_set_int("bt_on", 0);
        if(g_sw) lv_obj_clear_state(g_sw, LV_STATE_CHECKED);
        bt_disable();
        list_msg("Couldn't turn on Bluetooth");
    }
}
/* The one Bluetooth power path: the page's switch and the Settings > Network row both call it. An explicit enable
 * replaces the boot-restore observer with its own radio poll (bt_enable); Off cancels both (bt_disable). */
int bt_power_pending(void){ return g_radio_timer != NULL; }   /* a start made here is still waiting for the adapter */
static int bt_power_off_ready(void){
    if(!ui_route_mac()[0] || ui_route_analog() == 0) return 1;
    if(g_sw) lv_obj_add_state(g_sw, LV_STATE_CHECKED);
    ui_toast(tr("Couldn't switch output - try again"));
    return 0;
}
void bt_power_set(int on){
    if(!on && !bt_power_off_ready()) return;
    if(on){
        cfg_set_int("bt_on", 1);atomic_store(&g_bt_radio_observed,1);   /* persist intent so boot restores it (mirror WiFi) */
        bt_enable();
        list_msg("Turning on " LV_SYMBOL_BLUETOOTH);
        g_radio_start = lv_tick_get();
        if(g_scanwait_timer){ lv_timer_del(g_scanwait_timer); g_scanwait_timer = NULL; }  /* radio poll owns bring-up now - keep the two mutually exclusive */
        if(g_radio_timer) lv_timer_del(g_radio_timer);
        g_radio_timer = lv_timer_create(radio_on_poll_cb, 1000, NULL);
    } else {
        cfg_set_int("bt_on", 0);atomic_store(&g_bt_radio_observed,0);   /* OFF before teardown so boot-restore won't resurrect it */
        if(g_radio_timer){ lv_timer_del(g_radio_timer); g_radio_timer = NULL; }
        bt_attempt_cancel(0);  /* a pending attempt must not scan_kick() or route while BT is off */
        scan_abort();          /* cancel any pending/active scan so it can't render over "off" */
        bt_disable();
        list_msg("Bluetooth is off");
    }
    /* a choice made on the Settings row also shows on the page's switch */
    if(g_sw){ if(on) lv_obj_add_state(g_sw, LV_STATE_CHECKED); else lv_obj_clear_state(g_sw, LV_STATE_CHECKED); }
}
static void sw_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_VALUE_CHANGED) return;
    bt_power_set(lv_obj_has_state(g_sw, LV_STATE_CHECKED));
}

/* Quick Settings tile short-press: flip BT + persist intent, no screen-specific UI.
 * Mirrors sw_cb's actions (bt_boot_restore/keepalive enforce the intent). Returns new state, -1 if refused. */
int bt_toggle(void){
    int on = !bt_radio_on();   /* flip the ACTUAL radio state, not the (possibly stale) persisted intent */
    if(!on && !bt_power_off_ready()) return -1;
    cfg_set_int("bt_on", on);atomic_store(&g_bt_radio_observed,!!on);
    if(on){
        bt_enable();
        g_radio_start = lv_tick_get();
        if(g_scanwait_timer){ lv_timer_del(g_scanwait_timer); g_scanwait_timer = NULL; }
        if(g_radio_timer) lv_timer_del(g_radio_timer);
        g_radio_timer = lv_timer_create(radio_on_poll_cb, 1000, NULL);
    } else {
        if(g_radio_timer){ lv_timer_del(g_radio_timer); g_radio_timer = NULL; }        /* cancel the bring-up poll too */
        bt_attempt_cancel(0);  /* a pending attempt must not scan_kick() or route while BT is off */
        scan_abort();          /* cancel any pending/active scan */
        bt_disable();
    }
    /* keep the BT screen's switch in sync so it reflects reality when opened later */
    if(g_sw){ if(on) lv_obj_add_state(g_sw, LV_STATE_CHECKED); else lv_obj_clear_state(g_sw, LV_STATE_CHECKED); }
    ui_status_refresh();   /* update the home BT icon immediately (don't wait for the next status poll) */
    return on;
}

/* ---- Bluetooth persistence: restore the radio at boot if it was on (mirrors WiFi) -------
 * diskOS owns a "bt_on" intent (cfg), which starts OFF on a fresh install (bt_init_intent below - it is NOT
 * seeded from stock SYSCONFIG.BT_STATUS); the toggle above persists every change. bt_boot_restore() (called at startup) brings the stack up if
 * the intent is on - bluez then auto-reconnects trusted speakers and bt_autoroute picks them
 * up, so BT audio survives reboots with no BT-screen visit. Uses its OWN poll timer (not
 * g_radio_timer) and never scans (no BT screen at boot). */
void bt_init_intent(void){
    if(cfg_get_int("bt_on", -1) >= 0) return;             /* already owned by diskOS - keep the user's choice */
    /* Default OFF on a fresh diskOS: do NOT inherit stock's SYSCONFIG.BT_STATUS. BT on-by-default put
     * users one tap from the freeze bug (now fixed), and most listening here is wired - so a new install
     * starts with Bluetooth off and the user enables it when they want it. */
    cfg_set_int("bt_on", 0);
}
static lv_timer_t *g_bootrestore_timer;
static uint32_t    g_bootrestore_start;
static int         g_bootrestore_observe;
static void bootrestore_cancel(void){
    if(g_bootrestore_timer){ lv_timer_del(g_bootrestore_timer); g_bootrestore_timer = NULL; }
}
static void bootrestore_poll_cb(lv_timer_t *t){
    if(t != g_bootrestore_timer) return;
    if(cfg_get_int("bt_on", 0) != 1){ bootrestore_cancel(); return; }
    if(bt_on()){
        bootrestore_cancel();
        bt_ensure_services();
        bt_autoroute_start();          /* speaker auto-reconnects -> auto-route routes it */
        return;
    }
    if(g_bootrestore_observe && !g_bt_probe_pending){
        g_bootrestore_observe=0;
        bt_enable(); /* cancels this observer before starting bring-up */
        g_bootrestore_start=lv_tick_get();
        g_bootrestore_timer=lv_timer_create(bootrestore_poll_cb,1000,NULL);
        return;
    }
    if(lv_tick_elaps(g_bootrestore_start) > 28000){       /* patchram + power-on settle */
        bootrestore_cancel();
    }
}
void bt_boot_restore(void){
    bootrestore_cancel();   /* replacement/off/already-up paths must not leave an old observer */
    bt_init_intent();
    if(cfg_get_int("bt_on", 0) != 1) return;              /* was off -> stay off */
    if(bt_on()){ bt_ensure_services(); bt_autoroute_start(); return; }   /* already up (UI restart) */
    g_bootrestore_observe=g_bt_probe_pending;
    if(!g_bootrestore_observe)bt_enable();
    g_bootrestore_start = lv_tick_get();
    if(g_bootrestore_timer) lv_timer_del(g_bootrestore_timer);
    g_bootrestore_timer = lv_timer_create(bootrestore_poll_cb, 1000, NULL);
}

static void rescan_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) start_scan(); }
static void back_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) screen_back(); }

void bt_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    ui_header_cb(root, "Bluetooth", back_cb);   /* shared header */

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
    lv_label_set_text(tl, "Bluetooth");
    lv_obj_set_pos(tl, 16, 14);
    lv_obj_set_style_text_font(tl, TF(UI_16), 0);
    lv_obj_set_style_text_color(tl, TC(TEXT_PRIMARY), 0);

    g_sw = lv_switch_create(trow);
    lv_obj_set_size(g_sw, 46, 24);
    lv_obj_set_ext_click_area(g_sw, 10);
    lv_obj_align(g_sw, LV_ALIGN_RIGHT_MID, -14, 0);
    ui_on(g_sw, sw_cb, LV_EVENT_VALUE_CHANGED, NULL, "bt.sw.value", UI_CORE);

    lv_obj_t *rb = lv_button_create(root);
    lv_obj_remove_style_all(rb);
    lv_obj_set_pos(rb, 266, 72); lv_obj_set_size(rb, 36, 32);
    lv_obj_set_ext_click_area(rb, 8);
    lv_obj_set_style_radius(rb, 10, 0);
    lv_obj_set_style_bg_color(rb, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(rb, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(rb, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    ui_on(rb, rescan_cb, LV_EVENT_CLICKED, NULL, "bt.rescan", UI_CORE);
    lv_obj_t *rl = lv_label_create(rb);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_font(rl, TF(UI_16), 0);
    lv_obj_set_style_text_color(rl, TC(TEXT_SECONDARY), 0);
    lv_obj_center(rl);

    /* Honesty: audio routing to a BT speaker works now (auto-routes on connect), but SBC
     * sw-encode on this MIPS CPU can be rough (occasional artifacts) - label it beta. */
    lv_obj_t *note = lv_label_create(root);
    lv_label_set_text(note, "Connect a speaker to play audio (beta)");
    lv_obj_set_pos(note, 30, 120); lv_obj_set_width(note, 300);   /* clear of the card (and a theme's edge under it) */
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(note, TF(UI_12), 0);
    lv_obj_set_style_text_color(note, TC(TEXT_FOOTNOTE), 0);

    g_list = lv_obj_create(root);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_pos(g_list, 40, 138); lv_obj_set_size(g_list, 280, 180);
    lv_obj_set_style_pad_bottom(g_list, 44, 0);   /* last row scrolls clear of the round bottom bezel */
    lv_obj_set_style_bg_opa(g_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_row(g_list, 6, 0);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(g_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
}

/* Non-destructive "wait for the adapter, then scan" poll used when the BT screen is opened while the
 * radio is on/coming up (from a QS toggle, or brought up externally). Unlike radio_on_poll_cb it NEVER
 * powers BT off on a timeout - it just stops trying, so a flaky bt_on() probe can't kill a live radio. */
static void scanwait_poll_cb(lv_timer_t *t){
    (void)t;
    if(screen_current() != SCR_BT){                 /* user left the BT screen -> stop waiting */
        lv_timer_del(g_scanwait_timer); g_scanwait_timer = NULL; return;
    }
    if(bt_on()){
        lv_timer_del(g_scanwait_timer); g_scanwait_timer = NULL;
        bt_ensure_services(); bt_autoroute_start(); start_scan();
        return;
    }
    if(lv_tick_elaps(g_scanwait_start) > 28000){    /* gave up waiting - do NOT tear the radio down */
        lv_timer_del(g_scanwait_timer); g_scanwait_timer = NULL;
        list_msg(bt_radio_on() ? "Bluetooth is starting up" : "Bluetooth is off");
    }
}

void bt_open(void){
    /* "should BT be on" = the intent OR the live radio. Intent is the reliable signal right after a QS
     * toggle, because bt_enable() rfkill-blocks-then-unblocks, so bt_radio_on() reads 0 for a moment. */
    int want = bt_control_on();
    if(g_sw){
        if(want) lv_obj_add_state(g_sw, LV_STATE_CHECKED);
        else     lv_obj_clear_state(g_sw, LV_STATE_CHECKED);
    }
    screen_show(SCR_BT);
    /* bt_open is authoritative for this screen entry: cancel any in-flight bring-up observer (sw_cb's radio
     * poll or a prior scanwait) BEFORE deciding a path, so none of the branches below can race a pending
     * timer into a second start_scan() (e.g. reopen after the radio powered up but before g_radio_timer
     * fired would otherwise scan here AND on the next poll tick). */
    if(g_radio_timer){ lv_timer_del(g_radio_timer); g_radio_timer = NULL; }
    if(g_scanwait_timer){ lv_timer_del(g_scanwait_timer); g_scanwait_timer = NULL; }
    if(bt_on()){                                   /* fully up -> scan immediately */
        bt_ensure_services(); bt_autoroute_start(); start_scan();
    } else if(want){                                /* on/coming up (e.g. just toggled from Quick Settings)
                                                     * -> poll until Powered, then scan. NON-destructive:
                                                     * unlike radio_on_poll_cb, this never powers BT off on a
                                                     * timeout, so observing an externally-enabled radio (or a
                                                     * flaky bt_on() probe) can't kill a working controller. */
        list_msg_scanning();
        g_scanwait_start = lv_tick_get();
        if(g_scanwait_timer) lv_timer_del(g_scanwait_timer);
        g_scanwait_timer = lv_timer_create(scanwait_poll_cb, 1000, NULL);
    } else {
        list_msg("Bluetooth is off");
    }
}

static int bt_monitor_wanted(void){
    return cfg_get_int("bt_on",0) || bt_radio_cached() || g_radio_timer ||
        g_bootrestore_timer || g_scanwait_timer || g_bt_autoroute_timer;
}
/* Main-thread delivery. Raw monitor text never drives routing directly. */
void bt_event_deliver(unsigned topics){
    scan_spinner_sync(ui_screen_backlight_on() && screen_current()==SCR_BT);
    static unsigned seen,player;static int receiver=-1;
    if(radio_monitor_quiescing())return;
    int watch=bt_monitor_wanted();radio_monitor_set_active(watch);
    if(watch && radio_monitor_start()<0 && g_bt_autoroute_timer)
        lv_timer_resume(g_bt_autoroute_timer); /* no worker: retain the original observation deadline */
    unsigned revision=radio_monitor_revision(),generation=ipc_generation();
    int rx=ui_receiver_active();
    int changed=revision!=seen || generation!=player || rx!=receiver;
    seen=revision;player=generation;receiver=rx;
    if(changed){bt_capture_drop_except(NULL);g_autoroute_wait=0;}
    if(g_ops_timer && (changed || (topics&UI_WAKE_BT)))bt_ops_cb(g_ops_timer);
    int done;pthread_mutex_lock(&g_scan_mu);done=g_scan_done;pthread_mutex_unlock(&g_scan_mu);
    if(done && g_scanpoll_timer && ui_screen_backlight_on())scanpoll_cb(g_scanpoll_timer);
    if(watch && (changed || (topics&UI_WAKE_BT)))(void)bt_on();
    if(changed){
        dnui_radio();
        if(g_scanwait_timer)lv_timer_ready(g_scanwait_timer);
    }
    if(g_bt_autoroute_timer && (changed || (g_autoroute_wait && (topics&UI_WAKE_BT))))bt_autoroute_poll_cb(g_bt_autoroute_timer);
}
