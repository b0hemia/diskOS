/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CUSTOM_SAVER_APPLY_H
#define CUSTOM_SAVER_APPLY_H

/* Install a GIF from the SD picker folder as the custom lock-screen image, as stock does (mq_ui@0x47c37c) but
 * without its failure ordering. Stock limits, re-checked here: name ends in .gif (any case), regular file,
 * <= 4194304 bytes (mq_ui@0x47b6cc slt 0x400001), logical screen width and height <= 364 (sltiu 365 at
 * 0x47b710/0x47b720). Normal failures preserve the previous custom_gif.gif bytes,
 * custom row and selection. Failure of restoration or player compensation returns
 * CUSTOM_GIF_RESTORE. There is no restart recovery; power loss between the swap
 * and COMMIT may leave the new GIF unselected (owner decision 70).
 * Blocks (SD copy, fsync, database busy wait up to 3 s): call it from a worker
 * thread. One apply at a time. Precondition shared with custom_saver_db_install: the caller keeps stock's own
 * theme-selection writer out for the duration. The built-in trial decode uses LVGL's allocator and lv_fs, so it
 * must run in LVGL's context; a worker thread passes a decode hook that runs on the UI thread instead. */
typedef enum {
    CUSTOM_GIF_OK,
    CUSTOM_GIF_BUSY,        /* another apply is running */
    CUSTOM_GIF_CONVERSION,  /* a still-image conversion is pending, unreleased or unresolved */
    CUSTOM_GIF_PLAYER,      /* player queue not ready, or the 0810 frame was not queued */
    CUSTOM_GIF_SOURCE,      /* not a regular .gif directly in /tmp/sdcard/CustomCover, or a symlink on the path */
    CUSTOM_GIF_SD,          /* no SD lease, or the card could not be opened or read completely */
    CUSTOM_GIF_TOO_LARGE,   /* more than 4194304 bytes */
    CUSTOM_GIF_DIMENSIONS,  /* logical screen wider or taller than 364 */
    CUSTOM_GIF_STORAGE_FULL,/* ENOSPC or EDQUOT while staging, backing up or syncing */
    CUSTOM_GIF_STORAGE,     /* any other internal storage failure */
    CUSTOM_GIF_DECODE,      /* the staged copy is not a GIF whose first frame decodes */
    CUSTOM_GIF_DB_BUSY,     /* theme.db locked longer than the busy wait */
    CUSTOM_GIF_DB_ROW,      /* custom row or schema missing, ambiguous or unusable */
    CUSTOM_GIF_DB,          /* any other database refusal (WAL, hot journal, read-only, missing file) */
    CUSTOM_GIF_RESTORE,     /* file restore, directory flush or player compensation failed */
    /* stills only */
    CUSTOM_GIF_FORMAT,      /* not a PNG, JPEG or BMP by magic (the converter's own test, mq_player@0x482f80) */
    CUSTOM_GIF_CONVERT,     /* the converter reported failure (a810 nonzero): previous image kept */
    CUSTOM_GIF_OUTPUT       /* a810 said success but the output PNG is missing, oversized or does not decode */
} custom_gif_status;

/* Returns 0 when the first frame of the staged file decodes. NULL selects the built-in decoder, which needs
 * LV_USE_GIF=1 (otherwise every apply fails closed with CUSTOM_GIF_DECODE). */
typedef int (*custom_gif_decode_fn)(const char *staged_path);
custom_gif_status custom_gif_apply(const char *src, custom_gif_decode_fn decode);

/* Still-image installation through the stock converter (mq_ui@0x47c488-0x47c4f0 -> 0810 value 5 + path ->
 * mq_player@0x4f169c -> /usr/data/fiio/custom_cover.png + a810). Asynchronous: start returns at once, a main-loop
 * timer calls custom_still_tick(), the UI reads the outcome with custom_still_poll(). Both run on the UI thread
 * (the LVGL image cache is dropped there); the database and decode work runs on a short-lived worker thread.
 * Stock accepts jpg/jpeg/png/bmp (any case) with no size check. diskOS adds a limit on what it hands the player,
 * whose memory is unknown (120 MB machine, about 64 MB available): at most 16 MiB and 8 million pixels
 * (unverified on the Disc; a larger image would be decoded at 3-4 bytes per pixel inside mq_player, and
 * losing that process reboots the device). Start refuses with BUSY/CONVERSION while a GIF or still apply, an unreleased conversion
 * or an unresolved conversion is outstanding. Success needs a810 success AND an output PNG that decodes (at most
 * 2 MiB and 1024x1024); only then is the row (TYPE 200) updated and the image cache dropped. Every other
 * confirmed outcome restores the previous PNG and sends one compensating pathless 0810 for the previous style.
 * No a810 before the deadline (IPC_CONV_TIMEOUT_MS) is UNRESOLVED: nothing is restored and everything stays
 * blocked until a same-generation a810 confirms completion. A queue generation
 * change alone leaves the operation unresolved because it does not prove the
 * old converter stopped writing. */
typedef enum { CUSTOM_STILL_IDLE, CUSTOM_STILL_PENDING, CUSTOM_STILL_UNRESOLVED, CUSTOM_STILL_DONE } custom_still_state;
custom_gif_status custom_still_start(const char *src);   /* OK = started; else refused with nothing changed */
void custom_still_tick(void);
int custom_still_waiting(void);
custom_still_state custom_still_poll(custom_gif_status *result);   /* DONE reports the result once */

/* Read-only checks for the SD picker (custom_saver_picker.c), so the list never offers what apply would refuse.
 * *_check_fd: the same size and header limits apply enforces (OK, TOO_LARGE, DIMENSIONS, DECODE/FORMAT for a bad
 * header, SD when the header cannot be read); `size` is the fstat size of the open card file. source_kind classifies
 * a bare file name inside the picker folder: CUSTOM_SOURCE_GIF / _STILL by extension (any case), 0 when the
 * extension is neither, or _BAD_NAME (control character) / _TOO_LONG (folder path + name over stock's 255 bytes)
 * when apply would refuse the name. All three touch no shared state. */
enum { CUSTOM_SOURCE_GIF = 1, CUSTOM_SOURCE_STILL = 2, CUSTOM_SOURCE_BAD_NAME = -1, CUSTOM_SOURCE_TOO_LONG = -2 };
#include <sys/types.h>
custom_gif_status custom_gif_check_fd(int fd, off_t size);
custom_gif_status custom_still_check_fd(int fd, off_t size);
int custom_saver_source_kind(const char *name);
/* The lock screen's reader for a saved PNG still: BGRA pixels (w * h * 4 bytes, malloc'd by the decoder; free with
 * custom_still_free), NULL when the file is not a regular PNG within the converter-output bounds. Reads only. */
unsigned char *custom_still_load(const char *path, int *w, int *h);
void custom_still_free(unsigned char *px);
#endif
