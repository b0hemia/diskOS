/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <crypt.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PWFILE "/usr/data/sshd/current_pw"   /* persisted plaintext (0600) so the screen survives an mq_ui restart */

/* Debug Mode: enable/disable diskOS remote debug access from the UI.
 *   - SSH (dropbear over WiFi) with a RANDOM per-enable password shown ONLY here (never the stock
 *     password, never anything shipped in the repo). We hash it with crypt() and hand the hash to
 *     /usr/project/diskos-debug.sh, which overlays a private shadow over /etc/shadow.
 *   - a USB serial root shell on /dev/ttyGS0 (local-USB only).
 * State is intentionally NOT persisted across reboots: debug access comes back OFF after a restart. */

#define DBG "/usr/project/diskos-debug.sh"

static lv_obj_t *g_ssh, *g_pw, *g_serial, *g_warn, *g_btnlbl;
static int  g_on = 0;
static char g_pwtext[16] = "";

extern int ui_run_bounded(char *const argv[], int timeout_ms);   /* main.c: bounded fork+exec */

/* Returns 0 on success, -1 if secure entropy is unavailable. A network-root password must NEVER fall
 * back to a fixed/weak value, so the caller refuses to enable SSH on failure. */
static int rnd_alnum(char *out, int n){
    static const char cs[] = "abcdefghijkmnpqrstuvwxyz23456789";   /* 32 chars, no ambiguous 0/o/1/l */
    unsigned char b[32];
    int fd = open("/dev/urandom", O_RDONLY);
    if(fd < 0) return -1;
    ssize_t got = read(fd, b, sizeof b);
    close(fd);
    if(got != (ssize_t)sizeof b) return -1;
    if(n > (int)sizeof b) n = sizeof b;
    for(int i = 0; i < n; i++) out[i] = cs[b[i] & 31];
    out[n] = 0;
    return 0;
}

static int wlan_ip(char *out, int n){
    int s = socket(AF_INET, SOCK_DGRAM, 0); if(s < 0) return 0;
    struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.ifr_name, "wlan0", IFNAMSIZ-1);
    int ok = (ioctl(s, SIOCGIFADDR, &ifr) == 0);
    if(ok){ struct sockaddr_in *a = (struct sockaddr_in *)&ifr.ifr_addr;
            snprintf(out, n, "%s", inet_ntoa(a->sin_addr)); }
    close(s);
    return ok;
}

/* Is dropbear actually running? Scan /proc/<pid>/comm - so the screen reflects REAL state (e.g. after
 * an mq_ui restart while SSH kept running), not a stale in-memory flag. */
static int ssh_running(void){
    DIR *d = opendir("/proc"); if(!d) return 0;
    struct dirent *e; int found = 0;
    while((e = readdir(d))){
        if(e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char p[sizeof e->d_name + sizeof "/proc//comm"]; snprintf(p, sizeof p, "/proc/%s/comm", e->d_name);
        FILE *f = fopen(p, "r"); if(!f) continue;
        char c[32] = ""; if(fgets(c, sizeof c, f) && strncmp(c, "dropbearmulti", 13) == 0) found = 1;
        fclose(f);
        if(found) break;
    }
    closedir(d);
    return found;
}

/* Is our private shadow overlay currently bind-mounted over /etc/shadow? SSH is only truly OFF when
 * BOTH dropbear is gone AND this overlay is dropped - otherwise a blocked/killed ssh-off could leave
 * the overlay mounted, and reporting OFF would be a lie (and re-enable logic could misbehave). */
static int overlay_mounted(void){
    /* Fail CLOSED: if we cannot read /proc/mounts we must not claim the overlay is gone (that could
     * let the UI report OFF while stock shadow is still overlaid). Unknown -> treat as still mounted. */
    FILE *f = fopen("/proc/mounts", "r"); if(!f) return 1;
    char line[512]; int m = 0;
    while(fgets(line, sizeof line, f)){ if(strstr(line, " /etc/shadow ")){ m = 1; break; } }
    fclose(f);
    return m;
}

static void pw_load(void){
    g_pwtext[0] = 0;
    FILE *f = fopen(PWFILE, "r");
    if(f){ if(fgets(g_pwtext, sizeof g_pwtext, f)){ char *nl = strchr(g_pwtext, '\n'); if(nl) *nl = 0; } fclose(f); }
}
static void pw_store(void){
    FILE *f = fopen(PWFILE, "w");
    if(f){ fchmod(fileno(f), 0600); fprintf(f, "%s\n", g_pwtext); fclose(f); }
}

static void refresh(void){
    char b[96];
    if(g_on){
        char ip[40] = "";
        if(wlan_ip(ip, sizeof ip)) snprintf(b, sizeof b, "ssh root@%s", ip);
        else                       snprintf(b, sizeof b, "SSH: connect to WiFi first");
        lv_label_set_text(g_ssh, b);
        snprintf(b, sizeof b, "Password:  %s", g_pwtext);       lv_label_set_text(g_pw, b);
        /* Only claim serial when the USB gadget actually exists (dev builds); public builds have no
         * ttyGS0, so don't advertise a serial shell that can't be reached. */
        if(access("/dev/ttyGS0", F_OK) == 0) lv_label_set_text(g_serial, "Serial: USB-C (/dev/ttyACM0)");
        else                                 lv_label_set_text(g_serial, "Serial: not available (SSH only)");
        lv_label_set_text(g_warn, "This gives ROOT access to the device. Turn it OFF when finished.");
        lv_label_set_text(g_btnlbl, "Disable Debug");
        lv_obj_remove_flag(g_pw, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(g_serial, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(g_ssh, "Debug access is OFF");
        lv_obj_add_flag(g_pw, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_serial, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(g_warn, "Enables SSH over Wi-Fi with a fresh random password.");
        lv_label_set_text(g_btnlbl, "Enable Debug");
    }
}

static void enable_dbg(void){
    char pw[16], salt[16], setting[24];
    if(rnd_alnum(pw, 10) != 0 || rnd_alnum(salt, 8) != 0){
        lv_label_set_text(g_warn, "No secure random available - NOT enabled."); return;
    }
    snprintf(setting, sizeof setting, "$6$%s$", salt);
    char *h = crypt(pw, setting);
    if(!h || h[0] != '$'){ lv_label_set_text(g_warn, "Could not hash password - NOT enabled."); return; }
    strncpy(g_pwtext, pw, sizeof g_pwtext - 1); g_pwtext[sizeof g_pwtext - 1] = 0;
    char *a1[] = { DBG, "ssh-on", h, NULL };     ui_run_bounded(a1, 12000);
    char *a2[] = { DBG, "serial-on", NULL };     ui_run_bounded(a2, 5000);
    /* Trust the REAL state, not the (best-effort) return: only claim a working session (ON + password)
     * if dropbear actually came up. If it failed to start but the shadow overlay is still mounted (its
     * cleanup unmount failed, or we were killed mid-enable), that is NOT a clean OFF - show ON with no
     * password so the user can Disable to fully tear the overlay down. Only a confirmed
     * daemon-absent AND overlay-absent state reads as OFF. */
    if(ssh_running()){ pw_store(); g_on = 1; }
    else if(overlay_mounted()){ memset(g_pwtext, 0, sizeof g_pwtext); g_on = 1; }
    else { memset(g_pwtext, 0, sizeof g_pwtext); g_on = 0; }
    refresh();
    if(g_on && !g_pwtext[0])
        lv_label_set_text(g_warn, "SSH did not start cleanly - press Disable to reset, then try again.");
    else if(!g_on)
        lv_label_set_text(g_warn, "Could not start SSH - check WiFi and try again.");
}

static void disable_dbg(void){
    /* 12s bound MUST exceed the worst-case ssh-off duration (~6s: kill-wait + settle + umount) so the
     * helper always finishes the umount before we can kill it - otherwise ssh_running() could read
     * false while the shadow overlay is still mounted, and we'd wrongly report OFF. */
    char *a1[] = { DBG, "ssh-off", NULL };     ui_run_bounded(a1, 12000);
    char *a2[] = { DBG, "serial-off", NULL };  ui_run_bounded(a2, 5000);
    /* Verify it actually stopped: report OFF only if dropbear is really down AND the shadow overlay is
     * really dropped. A blocked/killed ssh-off could leave the overlay mounted with no daemon; claiming
     * OFF then would be false (and stock shadow must never be silently re-exposed). Require BOTH. */
    if(!ssh_running() && !overlay_mounted()){ unlink(PWFILE); memset(g_pwtext, 0, sizeof g_pwtext); g_on = 0; }
    else g_on = 1;
    refresh();
    if(g_on) lv_label_set_text(g_warn, "SSH did not fully stop - still ON. Try Disable again.");
}

static void btn_cb(lv_event_t *e){ (void)e; if(g_on) disable_dbg(); else enable_dbg(); }

/* On open, reflect the REAL state (dropbear running?) + reload the persisted password, so a restart
 * or a re-open never offers "Enable" while SSH is live (which would rotate the password needlessly). */
void debug_open(void){
    /* ON if dropbear is up OR the overlay is still mounted (a partial/stuck state must not read as OFF,
     * so the user can Disable again to fully tear it down). */
    g_on = ssh_running() || overlay_mounted();
    if(g_on) pw_load(); else g_pwtext[0] = 0;
    refresh();
    screen_show(SCR_DEBUG);
}

void debug_create(lv_obj_t *root){
    ui_header(root, "Debug Mode");   /* shared header: gives Debug Mode the standard back chevron so it
                                      * isn't a dead-end for users who don't know the edge-swipe gesture */

    /* The ssh command + password are what you read off the screen and type, so they get the HIGHEST
     * contrast (bright white); the password also gets the accent colour + bigger font to stand out. */
    g_ssh = lv_label_create(root);
    lv_obj_set_width(g_ssh, 320); lv_obj_set_style_text_align(g_ssh, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g_ssh, TC(TEXT_PRIMARY), 0);
    lv_obj_align(g_ssh, LV_ALIGN_TOP_MID, 0, 94);

    g_pw = lv_label_create(root);
    lv_obj_set_width(g_pw, 320); lv_obj_set_style_text_align(g_pw, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_pw, TF(UI_24), 0);
    lv_obj_set_style_text_color(g_pw, ui_current_accent(), 0);
    lv_obj_align(g_pw, LV_ALIGN_TOP_MID, 0, 122);

    g_serial = lv_label_create(root);
    lv_obj_set_width(g_serial, 300); lv_obj_set_style_text_align(g_serial, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g_serial, TC(TEXT_MUTED), 0);
    lv_obj_align(g_serial, LV_ALIGN_TOP_MID, 0, 158);

    g_warn = lv_label_create(root);
    lv_obj_set_width(g_warn, 280); lv_label_set_long_mode(g_warn, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(g_warn, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g_warn, TC(STATUS_WARNING), 0);
    lv_obj_align(g_warn, LV_ALIGN_TOP_MID, 0, 190);

    lv_obj_t *btn = lv_button_create(root);
    lv_obj_set_size(btn, 190, 50);
    lv_obj_set_style_shadow_width(btn, 0, 0);   /* no LVGL default-theme drop shadow: every other button is flat */
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -46);
    ui_on(btn, btn_cb, LV_EVENT_CLICKED, NULL, "debug_ui.btn", UI_CORE);
    /* the accent action, like Date & Time's Set: LVGL's default button blue and an inherited text colour read poorly
     * (green on green, grey on orange) under the themes */
    lv_color_t acc = ui_current_accent();
    lv_obj_set_style_bg_color(btn, acc, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    g_btnlbl = lv_label_create(btn); lv_obj_center(g_btnlbl);
    lv_obj_set_style_text_color(g_btnlbl, theme_on_color(acc), 0);   /* the live accent can be any album colour */

    refresh();
}
