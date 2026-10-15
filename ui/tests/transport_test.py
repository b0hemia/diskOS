#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Exercise shared transport and pause feedback, with IPC failures and storage holds."""
from pathlib import Path
import os,subprocess,tempfile
app=Path(__file__).resolve().parents[1]
s=(app/'main.c').read_text();hint=s[s.index('static int      g_pp_hint'):s.index('/* set absolute volume')]
scan=s[s.index('void ui_rescan_library(void)'):s.index('/* Poll for scan completion')]
s=(app/'ui.c').read_text();action=s[s.index('int ui_transport_command('):s.index('static void transport_cb(')]
headers=r'''
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#define LV_SYMBOL_PAUSE "pause"
#define LV_SYMBOL_PLAY "play"
static int g_playing,local=1,book,send_fail,seeks,commands,cancelled,disarmed,hints;
static int scan_busy,scan_fail,sd_allowed=1,scans;
static uint32_t now;
static long g_seek_target_ms,requested_seek;
static uint32_t g_seek_hold_until;
static char *btn_pp="";
static char toast[100],glyph[20];
static void ui_note_transport_sent(void){}
static int modes_output_busy(void){ return 0; }   /* no output switch in flight: the transport path is under test */
static int source_mode = 0, dac_connected = 1;
static int ui_get_source_mode(void){ return source_mode; }
static int ui_usb_dac_connected(void){ return dac_connected; }
static int cancel_ok=1,cancels; static int scanner_cancel(void){ cancels++; return cancel_ok; }   /* Rescan during a scan = Stop */
typedef struct { char path[40]; long position_ms,duration_ms; } track_state_t;
static track_state_t state;
static uint32_t lv_tick_get(void){return now;}
static int ui_is_playing(void){return g_playing;}
static int ui_local_playback_allowed(void){return local;}
static void ui_toast(const char *s){snprintf(toast,sizeof toast,"%s",s);}
static void ui_defer_sleep(void){}
static void ipc_get_state(track_state_t *s){*s=state;}
static int mdb_is_book_path(const char *s){(void)s;return book;}
static int ipc_send_cmd(const char *s){(void)s;commands++;return send_fail?-1:0;}
static int ui_seek_to(long p){seeks++;requested_seek=p;return send_fail?-1:0;}
static void ui_book_user_seeked(long p){(void)p;hints++;}
static void ui_cancel_book_resume(void){cancelled++;}
static void ui_disarm_book_eoc(void){disarmed++;}
static void ui_pp_glyph(const char *obj,int playing){(void)obj;snprintf(glyph,sizeof glyph,"%s",playing?LV_SYMBOL_PAUSE:LV_SYMBOL_PLAY);}
static int scanner_active(void){return scan_busy;}
static int sd_io_allowed(void){return sd_allowed;}
static int scanner_start(void){scans++;return scan_fail?-1:0;}
int ui_pp_icon_playing(int real);
'''
tests=r'''
int main(void){
    g_playing=1; now=1000;
    assert(ui_transport_command("0201000C0000")==0);
    assert(!strcmp(glyph,"play") && ui_pp_icon_playing(1)==0);
    now=1100; assert(ui_transport_command("0201000C0000")==0);
    assert(!strcmp(glyph,"pause") && ui_pp_icon_playing(0)==1);
    now=1200; assert(ui_transport_command("0201000C0000")==0);
    assert(ui_pp_icon_playing(1)==0);
    now=3000; assert(ui_pp_icon_playing(0)==0); /* no bounce at the inference grace */
    now=3401; assert(ui_pp_icon_playing(1)==1); /* reconcile after hold */
    now=UINT32_MAX-100; ui_pp_tap_hint(); now=50;
    assert(ui_pp_icon_playing(1)==0); now=2300; assert(ui_pp_icon_playing(1)==1);
    puts("PASS instant feedback, rapid repeated taps, reconciliation and tick wrap");

    send_fail=1; g_pp_hint=-1; assert(ui_transport_command("0201000C0000")<0);
    assert(g_pp_hint==-1 && !strcmp(toast,"Player busy - try again"));
    int sent=commands; local=0;
    assert(ui_transport_command("0201000C0000")<0);
    assert(ui_transport_command("0201000C0001")<0);
    assert(commands==sent && seeks==0);
    puts("PASS no optimistic success on IPC failure and no commands during USB ownership");

    local=1; source_mode=4; dac_connected=0; sent=commands;
    assert(ui_transport_command("0201000C0000")<0);
    assert(!strcmp(toast,"USB DAC not connected") && commands==sent);
    assert(ui_transport_command("0201000C0001")<0 && commands==sent);
    assert(ui_transport_command("0201000C0002")<0 && commands==sent);
    dac_connected=1; send_fail=0;
    assert(ui_transport_command("0201000C0000")==0 && commands==sent+1);
    source_mode=0; sent=commands;
    puts("PASS commands blocked when USB DAC unplugged in DAC output mode");

    local=1; send_fail=0; book=1; state.position_ms=10000;state.duration_ms=20000;
    assert(ui_transport_command("0201000C0002")==0 && requested_seek==0);
    assert(ui_transport_command("0201000C0001")==0 && requested_seek==20000);
    assert(commands==sent && hints==2 && !cancelled && !disarmed);
    send_fail=1; assert(ui_transport_command("0201000C0001")<0 && hints==2);
    book=0; assert(ui_transport_command("0201000C0001")<0 && !cancelled && !disarmed);
    send_fail=0;assert(ui_transport_command("0201000C0001")==0 && cancelled==1 && disarmed==1);
    puts("PASS bounded book seeks, failed seek preserves resume, successful music skip clears it");

    local=0; ui_rescan_library(); assert(scans==0);
    local=1;scan_busy=1;toast[0]=0;ui_rescan_library();assert(scans==0 && cancels==1 && !strcmp(toast,"Cancelling scan..."));
    cancel_ok=0;toast[0]=0;ui_rescan_library();assert(scans==0 && cancels==2 && toast[0]==0);   /* committing: refused, completion reports */
    cancel_ok=1;
    scan_busy=0;sd_allowed=0;ui_rescan_library();assert(scans==0 && strstr(toast,"not ready"));
    sd_allowed=1;scan_fail=1;ui_rescan_library();assert(scans==1 && strstr(toast,"Couldn't start"));
    scan_fail=0;ui_rescan_library();assert(scans==2 && !strcmp(toast,"Scanning library..."));
    puts("PASS honest scan admission, busy/error feedback and the started toast");
    puts("HARNESS COMPLETE");
}
'''
def run_harness(exe, timeout):
    """Run a compiled harness; its LAST stdout line must be HARNESS COMPLETE, so an early return 0 fails."""
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=timeout)
    print(r.stdout, end='')
    if r.stderr:
        print(r.stderr, end='')
    if r.returncode != 0:
        raise SystemExit(f'FAIL harness exited {r.returncode}')
    lines = r.stdout.strip().splitlines()
    if not lines or lines[-1] != 'HARNESS COMPLETE':
        raise SystemExit('FAIL harness did not reach its HARNESS COMPLETE line')


with tempfile.TemporaryDirectory(prefix='diskos-transport-test-') as t:
    p=Path(t);(p/'test.c').write_text(headers+hint+action+scan+tests)
    subprocess.run([os.environ.get('CC','cc'),'-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    run_harness(p/'test', 10)

print('COMPLETE transport_test.py')   # last line: the runner requires it, so an early clean exit cannot pass
