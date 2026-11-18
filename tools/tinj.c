/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* tinj.c - touch injector for the FiiO Snowsky Disc (cst816t capacitive controller).
 *
 * Drives the diskOS or stock UI without a finger, for automated navigation and testing. Writes
 * synthetic multitouch events directly to /dev/input/event1 (the cst816t is MT type-B there; on this
 * kernel, writing input_event records to the evdev node injects them).
 *
 * COORDINATES: the panel is mounted rotated 180 degrees, and both diskOS and the stock UI rotate raw
 * touch 180 internally, so a point shown at screen (x, y) is panel (360-x, 360-y). This tool takes
 * SCREEN coordinates (the same ones you read off an upright screenshot from diskos-shot.sh) and
 * applies the flip for you. Coordinates are 0..359, origin top-left.
 *
 * BUILD (with the diskOS musl cross toolchain, e.g. inside the ui/ Docker builder):
 *   mipsel-linux-musl-gcc -O2 -static -o tinj tinj.c
 * Then push it to the device (e.g. /usr/data/tinj) and run it there.
 *
 * USAGE (on the device):
 *   tinj tap   <x> <y>
 *   tinj swipe <x1> <y1> <x2> <y2>
 *
 * LIMITATION: a synthetic tap reliably wakes the screen and drives list rows and swipe navigation.
 * Small non-list buttons can occasionally be missed if the press and release land in a single UI
 * input-read cycle, which is why the tap holds the contact for ~90ms with intermediate frames. If a
 * particular control resists, prefer a list-row equivalent or a swipe gesture.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>

/* 32-bit-ABI input_event. The kernel here is 32-bit; a 64-bit time_t (musl default) would mismatch
 * the struct layout the kernel expects, so we lay it out explicitly. */
struct ev32 { uint32_t sec, usec; uint16_t type, code; int32_t value; };

#define EV_SYN              0x00
#define EV_KEY              0x01
#define EV_ABS              0x03
#define SYN_REPORT          0x00
#define BTN_TOUCH           0x14a
#define ABS_MT_SLOT         0x2f
#define ABS_MT_POSITION_X   0x35
#define ABS_MT_POSITION_Y   0x36
#define ABS_MT_TRACKING_ID  0x39

static int g_fd = -1;
static int g_err = 0;   /* set on any short/failed write so we can exit nonzero */

static void emit(uint16_t type, uint16_t code, int32_t value)
{
    struct ev32 e;
    memset(&e, 0, sizeof e);
    e.type = type; e.code = code; e.value = value;
    if (write(g_fd, &e, sizeof e) != (ssize_t)sizeof e) { perror("write"); g_err = 1; }
}
static void syn(void) { emit(EV_SYN, SYN_REPORT, 0); }

/* screen -> panel (180 rotation). Verified on-device with the UI's debug-dot: a tap sent for screen
 * (x,y) registers at (360-x, 360-y), so this maps a screen coordinate to the panel coordinate to send. */
static int fx(int x) { return 360 - x; }
static int fy(int y) { return 360 - y; }

static void usage(const char *p)
{
    fprintf(stderr, "usage: %s tap <x> <y> | swipe <x1> <y1> <x2> <y2>\n", p);
}

/* parse a screen coordinate, rejecting junk / negatives / out-of-range (valid range 0..359) */
static int parse_coord(const char *s)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v < 0 || v > 359) {
        fprintf(stderr, "coordinate out of range (0..359): %s\n", s);
        exit(2);
    }
    return (int)v;
}

int main(int argc, char **argv)
{
    if (argc < 4) { usage(argv[0]); return 2; }

    g_fd = open("/dev/input/event1", O_WRONLY);
    if (g_fd < 0) { perror("open /dev/input/event1"); return 1; }

    if (!strcmp(argv[1], "tap") && argc == 4) {
        int x = fx(parse_coord(argv[2])), y = fy(parse_coord(argv[3]));
        emit(EV_ABS, ABS_MT_SLOT, 0);
        emit(EV_ABS, ABS_MT_TRACKING_ID, 1);
        emit(EV_ABS, ABS_MT_POSITION_X, x);
        emit(EV_ABS, ABS_MT_POSITION_Y, y);
        emit(EV_KEY, BTN_TOUCH, 1);
        syn();
        /* Hold ~220ms with periodic no-move frames. The UI drains all pending evdev events each input
         * poll and keeps the last state, so a too-short tap can be read as press+release in one poll and
         * never register a click. A generous hold guarantees at least one poll sees a steady PRESSED
         * before the release lands as a separate poll. */
        usleep(80000);
        for (int i = 0; i < 4; i++) {
            emit(EV_ABS, ABS_MT_POSITION_X, x);
            emit(EV_ABS, ABS_MT_POSITION_Y, y);
            syn();
            usleep(35000);
        }
        emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
        emit(EV_KEY, BTN_TOUCH, 0);
        syn();
    } else if (!strcmp(argv[1], "swipe") && argc == 6) {
        int x1 = fx(parse_coord(argv[2])), y1 = fy(parse_coord(argv[3]));
        int x2 = fx(parse_coord(argv[4])), y2 = fy(parse_coord(argv[5]));
        emit(EV_ABS, ABS_MT_SLOT, 0);
        emit(EV_ABS, ABS_MT_TRACKING_ID, 1);
        emit(EV_ABS, ABS_MT_POSITION_X, x1);
        emit(EV_ABS, ABS_MT_POSITION_Y, y1);
        emit(EV_KEY, BTN_TOUCH, 1);
        syn();
        int steps = 16;
        for (int i = 1; i <= steps; i++) {
            emit(EV_ABS, ABS_MT_POSITION_X, x1 + (x2 - x1) * i / steps);
            emit(EV_ABS, ABS_MT_POSITION_Y, y1 + (y2 - y1) * i / steps);
            syn();
            usleep(12000);
        }
        emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
        emit(EV_KEY, BTN_TOUCH, 0);
        syn();
    } else {
        usage(argv[0]);
        close(g_fd);
        return 2;
    }

    close(g_fd);
    return g_err ? 1 : 0;
}
