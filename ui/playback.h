/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_PLAYBACK_H
#define DISKOS_PLAYBACK_H
#include "ipc.h"
#include "remote_backend.h"
/* Playback facade: ONE effective view of what is playing (track, queue position, play state, time, output destination) and ONE
 * command path. The effective source is LOCAL (the player over /ui) or the CAST/AIRPLAY
 * owner. The raw local state is never modified and stays available for local-player
 * health and guards. Nothing here opens a socket or calls the Cast session; call it from the LVGL thread only.
 * A remote command never produces a /player frame. */
#include "trackpath.h"
#define PB_TEXT_MAX 256
typedef struct {
    int  have_track;
    char title[PB_TEXT_MAX], artist[160], album[160], path[TRACK_PATH_MAX]; /* track identity retains its library path */
    long position_ms, duration_ms;
    int  playing;
    int  queue_pos, queue_len;   /* LOCAL: queue_pos = the player's current row (pos_id, 0 unknown), queue_len 0 = unknown */
    char dest[48];               /* output destination label; empty for LOCAL */
} pb_view;
typedef struct { pb_view v; int active; } pb_remote_state;
typedef pb_remote_state pb_cast_state;   /* compatibility for existing Cast publishers */
typedef struct { pb_source source; pb_view v; } pb_snapshot;
/* raw = the player state as the main loop has it; title = the display title consumers use (ipc_display_title);
 * playing = the main loop's inferred play state. Rebuilds the effective snapshot. */
void playback_update(const track_state_t *raw, const char *title, int playing);
void playback_set_cast(const pb_cast_state *c);   /* legacy Cast publisher; cannot replace an AirPlay owner */
/* CAST/AIRPLAY only. Active state claims an idle facade or updates its current
 * owner; another active owner is refused. NULL/inactive releases only the named
 * owner, so late cleanup from another backend cannot erase the active view.
 * Returns 0 accepted/no-op, PB_E_SOURCE invalid source, PB_E_BUSY another owner.
 * Publication never sends a command or writes volume. Caller filters stale
 * attempts within its own backend before publishing. No transport is enabled here. */
int playback_set_remote(pb_source source, const pb_remote_state *state);
const pb_snapshot *playback_effective(void);
/* Display seam. out = the track_state_t the Now Playing / Home / saver surfaces draw. LOCAL: an exact copy of raw. Remote: raw with the
 * track, queue "N/M", position, duration and play state replaced by the facade view (format, favourite and CUE fields cleared: the receiver's are
 * unknown); raw is never modified. playback_view_changed(): 1 once when the effective view changed since the last call (source/device switch,
 * track/queue/play state, or position crossing a second); always 0 while LOCAL except the single call after leaving remote playback. */
void playback_display_state(track_state_t *out, const track_state_t *raw);
int playback_view_changed(void);
pb_source playback_source(void);
/* Command sinks: 0 = accepted, < 0 = refused. arg = ms for SEEK, level for VOLUME. */
typedef enum { PB_CMD_TOGGLE, PB_CMD_NEXT, PB_CMD_PREV, PB_CMD_SEEK, PB_CMD_VOLUME } pb_cmd;
typedef int (*pb_remote_sink)(pb_cmd cmd, long arg);
typedef pb_remote_sink pb_cast_sink;
void playback_set_cast_sink(pb_cast_sink f);
/* Register/clear only this backend's sink, even when inactive. Returns PB_E_SOURCE
 * for LOCAL/invalid values. A missing/rejecting remote sink never falls back to Local. */
int playback_set_remote_sink(pb_source source, pb_remote_sink sink);
#define PB_E_BADCMD (-1)
#define PB_E_NOSINK (-2)
#define PB_E_SOURCE (-3)
#define PB_E_BUSY (-4)
/* LOCAL: call the given local function exactly as before. Remote: only the owner's sink.
 * frame = "0201000C0000" toggle, ...0001 next, ...0002 prev; other remote frames are refused. */
int playback_command(const char *frame, int (*local)(const char *frame));
int playback_seek(long ms, int (*local)(long ms));
int playback_volume(int level, int (*local)(int level));
#endif
