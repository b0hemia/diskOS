/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef TRACKPATH_H
#define TRACKPATH_H
/* Bytes for one track's full path (/tmp/sdcard/...), including the NUL. Every buffer that holds a track path - the
 * player's now-playing report, library rows, art/lyrics/Beam jobs - uses this, so a long path is never cut (a cut path
 * can name a different file, so code that can't fit one refuses it). Matches the scanner's MAXPATH: anything the scanner
 * indexes fits. Card paths of 300+ bytes are real (long non-Latin folder and file names are 3 bytes per character). */
#define TRACK_PATH_MAX 1024
#endif
