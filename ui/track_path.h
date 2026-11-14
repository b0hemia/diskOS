/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef TRACK_PATH_H
#define TRACK_PATH_H
/* Capacity (including the NUL) of every buffer that carries one track's absolute path end to end:
 * the player report, the library rows, the playback view, Cast/AirPlay hand-off and the per-track
 * caches. It equals the scanner's MAXPATH (1024), so a file the scanner indexes (up to 1023 path
 * bytes) is never cut by a consumer. The stock folder command refuses a payload over 511 bytes
 * (docs/COMMAND_MAP.md, type 4); the largest path the stock player reports in song_file_path is not
 * documented for every route (UNVERIFIED), so the limit keeps 2x margin over that 511-byte figure.
 * A path that still does not fit is never truncated: the producer treats it as unavailable. */
#ifndef TRACK_PATH_MAX
#define TRACK_PATH_MAX 1024
#endif
#endif
