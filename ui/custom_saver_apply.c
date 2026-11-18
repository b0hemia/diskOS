/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ui_wake.h"
#include "custom_saver_apply.h"
#include "custom_saver_db.h"
#include "ipc.h"
#include "path_nosymlink.h"
#include "sdio.h"
#include "lvgl/src/libs/gif/gifdec.h"
#define STB_IMAGE_IMPLEMENTATION   /* PNG only: validates the converter's output; no stdio, no floats */
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_FAILURE_STRINGS
#define STBI_MAX_DIMENSIONS 1024
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"   /* overflow helpers the PNG-only build never calls */
#include "tools/stb_image.h"
#pragma GCC diagnostic pop
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Stock constants (V2.57 mq_ui): picker folder 0x6a07b0, target 0x6a0850, NAME 0x6a08bc, TYPE 204 (li 0xcc at
 * 0x47c5c8), row (0, 0), size and dimension limits at 0x47b6cc-0x47b724. */
#define GA_SD_DIR "/tmp/sdcard/CustomCover/"
#define GA_TARGET "/usr/data/fiio/custom_gif.gif"
#define GA_NAME "custom_gif.gif"
#define GA_MAX_BYTES 4194304
#define GA_MAX_DIM 364
#define GA_PATH 1024
#ifdef CUSTOM_SAVER_APPLY_TEST   /* host tests only: a temp tree stands in for /tmp/sdcard and /usr/data */
#ifdef __mips__
#error "CUSTOM_SAVER_APPLY_TEST must never be built for the device"
#endif
extern const char *custom_saver_apply_root;
extern int custom_saver_apply_busy_ms;
#define GA_ROOT custom_saver_apply_root
#define GA_BUSY_MS custom_saver_apply_busy_ms
#define GA_FS ""
void custom_saver_apply_cache_drop(const char *src);   /* test hook: counts the call */
#define GA_CACHE_DROP custom_saver_apply_cache_drop
#else
#include "lvgl/lvgl.h"
#define GA_CACHE_DROP lv_image_cache_drop
#define GA_ROOT ""
#define GA_BUSY_MS CUSTOM_SAVER_BUSY_MS
#define GA_FS "A:"               /* lv_fs stdio drive (lv_conf.h LV_FS_STDIO_LETTER) */
#endif

static void ga_path(char *out, const char *p, const char *suffix) {
    snprintf(out, GA_PATH, "%s%s%s", GA_ROOT, p, suffix);
}
static custom_gif_status ga_werr(int e) {
    return e == ENOSPC || e == EDQUOT ? CUSTOM_GIF_STORAGE_FULL : CUSTOM_GIF_STORAGE;
}
/* Folder, no control characters, the 255-byte buffer stock builds the path in (0x47c06c). Returns the extension. */
static const char *ga_name_ok(const char *p) {
    size_t pl = strlen(GA_SD_DIR), n = strlen(p);
    if(n <= pl || n > 255 || strncmp(p, GA_SD_DIR, pl)) return NULL;
    for(const char *c = p + pl; *c; c++)
        if((unsigned char)*c < 0x20 || *c == 0x7f || *c == '/') return NULL;
    return strrchr(p + pl, '.');
}
/* ".gif" in any case. */
static int ga_source_ok(const char *p) {
    const char *dot = ga_name_ok(p);
    return dot && !strcasecmp(dot, ".gif");
}
/* Logical screen descriptor (offsets 6 and 8, as 0x42d8b0 reads them). */
static custom_gif_status ga_header(int fd, off_t size) {
    unsigned char h[10];
    if(size > GA_MAX_BYTES) return CUSTOM_GIF_TOO_LARGE;
    if(pread(fd, h, sizeof h, 0) != (ssize_t)sizeof h || memcmp(h, "GIF", 3)) return CUSTOM_GIF_DECODE;
    if(h[6] + 256 * h[7] > GA_MAX_DIM || h[8] + 256 * h[9] > GA_MAX_DIM) return CUSTOM_GIF_DIMENSIONS;
    return CUSTOM_GIF_OK;
}
/* Bounded copy; the byte count must equal the size seen when the source was opened. */
static custom_gif_status ga_pump(int in, int out, off_t want, off_t cap, custom_gif_status rerr) {
    char buf[16384];
    off_t total = 0;
    for(;;) {
        ssize_t n = read(in, buf, sizeof buf);
        if(n < 0 && errno == EINTR) continue;
        if(n < 0) return rerr;
        if(!n) break;
        if(total + n > cap) return CUSTOM_GIF_TOO_LARGE;
        for(ssize_t o = 0; o < n;) {
            ssize_t w = write(out, buf + o, n - o);
            if(w < 0 && errno == EINTR) continue;
            if(w <= 0) return ga_werr(w < 0 ? errno : ENOSPC);
            o += w;
        }
        total += n;
    }
    return total == want ? CUSTOM_GIF_OK : rerr;
}
/* Flush and close a file written by ga_pump; a failed one is removed. */
static custom_gif_status ga_seal(int out, custom_gif_status st, const char *dst) {
    if(!st && fsync(out)) st = ga_werr(errno);
    if(close(out) && !st) st = ga_werr(errno);
    if(st) unlink(dst);
    return st;
}
/* Walk from an anchored directory handle. No ancestor can redirect the leaf open. */
static int ga_open_source(const char *src) {
    char path[GA_PATH]; snprintf(path, sizeof path, "%s", src);
    int dir = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    char *part = path + 1;
    while(dir >= 0) {
        char *slash = strchr(part, '/');
        if(slash) *slash = 0;
        int next = openat(dir, part, slash ? O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC :
                         O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        int e = errno; close(dir); errno = e;
        if(!slash || next < 0) return next;
        dir = next; part = slash + 1;
    }
    return -1;
}
/* Card file -> staging file. The lease covers only the open card handle; the flush happens after it. */
static custom_gif_status ga_stage(const char *src, const char *stage) {
    if(!sd_io_begin()) return CUSTOM_GIF_SD;
    custom_gif_status st = CUSTOM_GIF_SD;
    struct stat sb;
    int out = -1, in = ga_open_source(src);
    if(in >= 0 && !fstat(in, &sb)) {
        st = !S_ISREG(sb.st_mode) ? CUSTOM_GIF_SOURCE : ga_header(in, sb.st_size);
        if(!st) {
            out = open(stage, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
            st = out < 0 ? ga_werr(errno) : ga_pump(in, out, sb.st_size, GA_MAX_BYTES, CUSTOM_GIF_SD);
        }
    }
    if(in < 0 && (errno == ELOOP || errno == ENOTDIR)) st = CUSTOM_GIF_SOURCE;
    if(in >= 0) close(in);
    int staged = out >= 0;
    if(staged && close(out) && !st) st = ga_werr(errno);
    sd_io_end();
    if(!st && staged) {
        out = open(stage, O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
        if(out < 0) st = ga_werr(errno);
        else return ga_seal(out, st, stage);
    }
    if(staged) unlink(stage);
    return st;
}
#if LV_USE_GIF
static int ga_decode(const char *path) {
    char p[GA_PATH + 4];
    snprintf(p, sizeof p, "%s%s", GA_FS, path);
    gd_GIF *g = gd_open_gif_file(p);
    if(!g) return -1;
    int first = gd_get_frame(g);
    gd_close_gif(g);
    return first == 1 ? 0 : -1;
}
#else
static int ga_decode(const char *path) { (void)path; return -1; }   /* decoder not built: fail closed */
#endif
/* The staged bytes, not the card copy, decide: size, header limits, then the first frame. */
static custom_gif_status ga_validate(const char *stage, custom_gif_decode_fn decode) {
    struct stat sb;
    int fd = open(stage, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if(fd < 0) return ga_werr(errno);
    custom_gif_status st = fstat(fd, &sb) ? CUSTOM_GIF_STORAGE : ga_header(fd, sb.st_size);
    close(fd);
    if(!st && (decode ? decode : ga_decode)(stage)) st = CUSTOM_GIF_DECODE;
    return st;
}
/* Copy of the current target, kept until the database commit so a failure can put it back. */
static custom_gif_status ga_backup(const char *target, const char *bak) {
    struct stat sb;
    int in = open(target, O_RDONLY | O_NOFOLLOW | O_CLOEXEC), out = -1;
    if(in < 0) return ga_werr(errno);
    custom_gif_status st = CUSTOM_GIF_STORAGE;
    if(!fstat(in, &sb)) {
        out = open(bak, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        st = out < 0 ? ga_werr(errno) : ga_pump(in, out, sb.st_size, sb.st_size, CUSTOM_GIF_STORAGE);
    }
    close(in);
    return out < 0 ? st : ga_seal(out, st, bak);
}
static int ga_syncdir(const char *file) {
    char dir[GA_PATH];
    snprintf(dir, sizeof dir, "%s", file);
    *strrchr(dir, '/') = 0;
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC), rc = fd < 0 || fsync(fd);
    if(fd >= 0) close(fd);
    return rc;
}
typedef struct { ipc_gif_select_status ipc; int sent; } ga_notify_ctx;
/* Runs inside the database transaction: if the player cannot be told, nothing is committed. */
static int ga_notify(void *arg) {
    ga_notify_ctx *c = arg;
    c->ipc = ipc_gif_select();
    c->sent = c->ipc == IPC_GIF_SELECT_OK;
    return !c->sent;
}
static custom_gif_status ga_db_status(custom_saver_status cs, const ga_notify_ctx *c) {
    switch(cs) {
    case CUSTOM_SAVER_OK: return CUSTOM_GIF_OK;
    case CUSTOM_SAVER_BUSY: return CUSTOM_GIF_DB_BUSY;
    case CUSTOM_SAVER_UNKNOWN: case CUSTOM_SAVER_NULL: case CUSTOM_SAVER_PATH: case CUSTOM_SAVER_SCHEMA:
        return CUSTOM_GIF_DB_ROW;
    case CUSTOM_SAVER_NOTIFY:
        return c->ipc == IPC_GIF_SELECT_BUSY ? CUSTOM_GIF_CONVERSION : CUSTOM_GIF_PLAYER;
    default: return CUSTOM_GIF_DB;
    }
}
static pthread_mutex_t ga_mu = PTHREAD_MUTEX_INITIALIZER;
/* In-process protection only; no recovery record or restart replay. */
static int ga_backup_retained;

static int ga_remove(const char *p) {
    return unlink(p) && errno != ENOENT;
}

/* Never truncate the installed target during fallback copying.
 * retain is set only while an unconsumed backup must be protected.
 * A successful fallback still reports the original restore-rename error. */
static int ga_restore(const char *tgt, const char *bak, const char *stage,
                      int had, int *retain) {
    int bad = 0;
    *retain = 0;
    if(had) {
        if(rename(bak, tgt)) {
            bad = 1;
            *retain = 1;
            if(ga_remove(stage) ||
               ga_backup(bak, stage) ||
               rename(stage, tgt))
                return -1;
        }
    } else if(ga_remove(tgt)) {
        return -1;
    }
    if(ga_syncdir(tgt)) return 1;
    *retain = 0;
    return bad;
}

static custom_gif_status ga_apply(const char *src, custom_gif_decode_fn decode) {
    char s[GA_PATH], tgt[GA_PATH], stage[GA_PATH], bak[GA_PATH], db[GA_PATH];
    if(!src || !ga_source_ok(src)) return CUSTOM_GIF_SOURCE;
    if(ga_backup_retained) return CUSTOM_GIF_RESTORE;
    ga_path(s, src, "");
    ga_path(tgt, GA_TARGET, "");
    ga_path(stage, GA_TARGET, ".stage");
    ga_path(bak, GA_TARGET, ".bak");
    ga_path(db, "/usr/data/fiio/db/theme.db", "");
    if(!custom_saver_path_no_symlinks(tgt)) return CUSTOM_GIF_STORAGE;

    /* Restart leftovers have no recovery meaning under decision 70. */
    if(ga_remove(stage) || ga_remove(bak)) return CUSTOM_GIF_STORAGE;

    ga_notify_ctx ctx = { IPC_GIF_SELECT_OK, 0 };
    unsigned previous;
    custom_gif_status st =
        ga_db_status(custom_saver_db_previous(db, &previous), &ctx);
    if(st) return st;

    struct stat sb;
    int had = !lstat(tgt, &sb), swapped = 0, retain = 0;
    if((had && !S_ISREG(sb.st_mode)) || (!had && errno != ENOENT))
        return CUSTOM_GIF_STORAGE;

    st = ga_stage(s, stage);
    if(!st) st = ga_validate(stage, decode);
    if(!st && had) st = ga_backup(tgt, bak);
    if(!st) {
        if(rename(stage, tgt)) st = ga_werr(errno);
        else {
            swapped = 1;
            if(ga_syncdir(tgt)) st = CUSTOM_GIF_RESTORE;
            /* Accepted: power loss between swap and COMMIT may leave
             * the new GIF unselected. */
            else st = ga_db_status(
                custom_saver_db_install(db, GA_NAME, GA_TARGET, 204,
                                       GA_BUSY_MS, ga_notify, &ctx),
                &ctx);
        }
    }
    if(st && swapped) {
        if(ga_restore(tgt, bak, stage, had, &retain))
            st = CUSTOM_GIF_RESTORE;
        ga_backup_retained = retain;
        if(ctx.sent && ipc_saver_reselect(previous))
            st = CUSTOM_GIF_RESTORE;
    }

    /* Cleanup cannot change an already completed operation's result. */
    if(ga_remove(stage))
        fprintf(stderr, "custom GIF: staging cleanup failed\n");
    if(!retain && ga_remove(bak))
        fprintf(stderr, "custom GIF: backup cleanup failed\n");
    return st;
}

/* ---- still images (slice 7). Stock constants (V2.57 mq_ui): output 0x6a08d8, NAME 0x6a08f8, TYPE 200 (li 0xc8 at
 * 0x47c4ac), extension test 0x47b560 (jpg, jpeg, png, bmp, lower-cased), magic test mq_player@0x482f80. ---- */
#define GS_TARGET "/usr/data/fiio/custom_cover.png"
#define GS_NAME "custom_cover.png"
#define GS_MAX_BYTES (16 << 20)    /* diskOS limits, see the header */
#define GS_MAX_PIXELS 8000000ULL
#define GS_OUT_BYTES (2 << 20)
#define GS_OUT_DIM 1024
enum { GS_IDLE, GS_START, GS_WAIT, GS_FINAL, GS_DONE };
/* gs_mu guards every field. start/tick/poll run on the UI thread; the worker only finishes a FINAL operation. */
static struct { int st, unresolved, stuck, reply, had, drop; unsigned op, prev; custom_gif_status res; pthread_t th; int th_live; } gs;
static pthread_mutex_t gs_mu = PTHREAD_MUTEX_INITIALIZER;

static int gs_idle(void) {   /* callers hold ga_mu; DONE is only an unread result */
    pthread_mutex_lock(&gs_mu);
    int idle = (gs.st == GS_IDLE || gs.st == GS_DONE) && !gs.stuck;
    pthread_mutex_unlock(&gs_mu);
    return idle;
}
static int gs_ext_ok(const char *p) {
    const char *d = ga_name_ok(p);
    return d && (!strcasecmp(d, ".jpg") || !strcasecmp(d, ".jpeg") || !strcasecmp(d, ".png") || !strcasecmp(d, ".bmp"));
}
/* Format by magic and size by header, as the converter picks its decoder; the card file is only read here. */
static custom_gif_status gs_header(int fd, off_t size) {
    unsigned char h[32], m[9];
    unsigned long long w = 0, ht = 0;
    ssize_t n = pread(fd, h, sizeof h, 0);
    if(n < 0) return CUSTOM_GIF_SD;
    if(size > GS_MAX_BYTES) return CUSTOM_GIF_TOO_LARGE;
    if(n >= 24 && !memcmp(h, "\x89PNG\r\n\x1a\n", 8)) {
        w = (unsigned)h[16] << 24 | h[17] << 16 | h[18] << 8 | h[19];
        ht = (unsigned)h[20] << 24 | h[21] << 16 | h[22] << 8 | h[23];
    } else if(n >= 26 && h[0] == 'B' && h[1] == 'M') {
        int32_t bh = (int32_t)((unsigned)h[22] | h[23] << 8 | h[24] << 16 | (unsigned)h[25] << 24);
        w = (unsigned)h[18] | h[19] << 8 | h[20] << 16 | (unsigned)h[21] << 24;
        ht = bh < 0 ? -(long long)bh : bh;
    } else if(n >= 4 && h[0] == 0xff && h[1] == 0xd8) {
        off_t pos = 2;
        for(int i = 0; i < 256 && !w; i++) {   /* walk segments to the first SOF (not DHT, JPG, DAC) */
            if(pread(fd, m, 4, pos) != 4 || m[0] != 0xff) return CUSTOM_GIF_FORMAT;
            if(m[1] == 0xff) { pos++; continue; }
            if(m[1] >= 0xc0 && m[1] <= 0xcf && m[1] != 0xc4 && m[1] != 0xc8 && m[1] != 0xcc) {
                if(pread(fd, m, 5, pos + 4) != 5) return CUSTOM_GIF_FORMAT;
                ht = m[1] << 8 | m[2]; w = m[3] << 8 | m[4];
                break;
            }
            if(m[1] == 0xda || m[1] == 0xd9) return CUSTOM_GIF_FORMAT;
            pos += 2 + (m[2] << 8 | m[3]);
        }
    }
    return !w || !ht ? CUSTOM_GIF_FORMAT : w * ht > GS_MAX_PIXELS ? CUSTOM_GIF_DIMENSIONS : CUSTOM_GIF_OK;
}
/* Picker entry points (custom_saver_picker.c): the same rules as apply, callable without installing anything. */
custom_gif_status custom_gif_check_fd(int fd, off_t size) { return ga_header(fd, size); }
custom_gif_status custom_still_check_fd(int fd, off_t size) { return gs_header(fd, size); }
int custom_saver_source_kind(const char *name) {
    char q[256], full[GA_PATH];
    size_t pl = strlen(GA_SD_DIR), n = strlen(name), keep = n, bad = 0;
    if(pl + keep > 255) keep = 255 - pl;   /* keep the tail, so the extension is still judged */
    memcpy(q, name + n - keep, keep + 1);
    for(size_t i = 0; i < keep; i++)
        if((unsigned char)q[i] < 0x20 || q[i] == 0x7f) { q[i] = '_'; bad = 1; }
    snprintf(full, sizeof full, "%s%s", GA_SD_DIR, q);
    int kind = ga_source_ok(full) ? CUSTOM_SOURCE_GIF : gs_ext_ok(full) ? CUSTOM_SOURCE_STILL : 0;
    return !kind ? 0 : keep < n ? CUSTOM_SOURCE_TOO_LONG : bad ? CUSTOM_SOURCE_BAD_NAME : kind;
}
/* The converter reads the card itself (a player-owned handle). diskOS holds a lease only for this probe: a lease
 * across the 30 s window would stall the media handoff without protecting the player's handle (sdio.h:5). If the
 * card is handed off mid-conversion the player's read fails and a810 reports it, which rolls back. */
static custom_gif_status gs_probe(const char *src) {
    struct stat sb;
    custom_gif_status st = CUSTOM_GIF_SD;
    if(!sd_io_begin()) return st;
    int in = ga_open_source(src);
    if(in >= 0 && !fstat(in, &sb)) st = !S_ISREG(sb.st_mode) ? CUSTOM_GIF_SOURCE : gs_header(in, sb.st_size);
    if(in < 0 && (errno == ELOOP || errno == ENOTDIR)) st = CUSTOM_GIF_SOURCE;
    if(in >= 0) close(in);
    sd_io_end();
    return st;
}
static void gs_paths(char *tgt, char *stage, char *bak, char *db) {
    ga_path(tgt, GS_TARGET, ""); ga_path(stage, GS_TARGET, ".stage");
    ga_path(bak, GS_TARGET, ".bak"); ga_path(db, "/usr/data/fiio/db/theme.db", "");
}
/* Everything before the frame is sent; the caller owns the START state. */
static custom_gif_status gs_begin(const char *src) {
    char s[GA_PATH], tgt[GA_PATH], stage[GA_PATH], bak[GA_PATH], db[GA_PATH];
    struct stat sb;
    unsigned prev, op = 0;
    ga_path(s, src, "");
    gs_paths(tgt, stage, bak, db);
    if(!custom_saver_path_no_symlinks(tgt) || ga_remove(stage) || ga_remove(bak)) return CUSTOM_GIF_STORAGE;
    if(ipc_conv_busy()) return CUSTOM_GIF_CONVERSION;
    custom_gif_status st = ga_db_status(custom_saver_db_previous(db, &prev), &(ga_notify_ctx){ IPC_GIF_SELECT_OK, 0 });
    if(st) return st;
    int had = !lstat(tgt, &sb);
    if((had && !S_ISREG(sb.st_mode)) || (!had && errno != ENOENT)) return CUSTOM_GIF_STORAGE;
    if((st = gs_probe(s)) || (had && (st = ga_backup(tgt, bak)))) return st;
    switch(ipc_conv_start(src, &op)) {
    case IPC_CONV_START_OK: gs.op = op; gs.prev = prev; gs.had = had; gs.unresolved = 0; return CUSTOM_GIF_OK;
    case IPC_CONV_START_BUSY: st = CUSTOM_GIF_CONVERSION; break;
    case IPC_CONV_START_PATH: st = CUSTOM_GIF_SOURCE; break;
    default: st = CUSTOM_GIF_PLAYER;
    }
    ga_remove(bak);
    return st;
}
custom_gif_status custom_still_start(const char *src) {
    if(!src || !gs_ext_ok(src)) return CUSTOM_GIF_SOURCE;
    if(pthread_mutex_trylock(&ga_mu)) return CUSTOM_GIF_BUSY;   /* a GIF apply is running */
    custom_gif_status st = CUSTOM_GIF_CONVERSION;
    custom_still_poll(NULL);   /* an unread result is discarded; reaps the worker */
    pthread_mutex_lock(&gs_mu);
    if(gs.stuck) st = CUSTOM_GIF_RESTORE;
    else if(gs.st == GS_IDLE) gs.st = GS_START;
    int mine = gs.st == GS_START;
    pthread_mutex_unlock(&gs_mu);
    if(mine) {
        st = gs_begin(src);
        pthread_mutex_lock(&gs_mu); gs.st = st ? GS_IDLE : GS_WAIT; pthread_mutex_unlock(&gs_mu);
    }
    pthread_mutex_unlock(&ga_mu);
    ui_notify(UI_WAKE_STILL);
    return st;
}
/* a810 success is not trusted until the PNG exists, is a regular file within bounds and decodes. */
static custom_gif_status gs_output(const char *path) {
    struct stat sb;
    int w, h, c, fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    custom_gif_status st = CUSTOM_GIF_OUTPUT;
    if(fd < 0) return st;
    unsigned char *buf = NULL;
    if(!fstat(fd, &sb) && S_ISREG(sb.st_mode) && sb.st_size > 0 && sb.st_size <= GS_OUT_BYTES &&
       (buf = malloc(sb.st_size))) {
        ssize_t n = 0, r = 0;
        while(n < sb.st_size && (r = read(fd, buf + n, sb.st_size - n)) != 0 && (r > 0 || errno == EINTR)) n += r > 0 ? r : 0;
        unsigned char *px = n == sb.st_size && stbi_info_from_memory(buf, n, &w, &h, &c) && w <= GS_OUT_DIM &&
                            h <= GS_OUT_DIM ? stbi_load_from_memory(buf, n, &w, &h, &c, 0) : NULL;
        if(px) { st = CUSTOM_GIF_OK; stbi_image_free(px); }
    }
    free(buf);
    close(fd);
    return st;
}
/* Read-only: a saved PNG still as 8-bit BGRA (LVGL ARGB8888 byte order) for the lock screen. Same bounds as the output
 * check above (regular file, no symlink at the leaf, at most GS_OUT_BYTES and GS_OUT_DIM square). NULL = unusable. */
unsigned char *custom_still_load(const char *path, int *w, int *h) {
    struct stat sb;
    int c, fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    unsigned char *buf = NULL, *px = NULL;
    if(fd < 0) return NULL;
    if(!fstat(fd, &sb) && S_ISREG(sb.st_mode) && sb.st_size > 0 && sb.st_size <= GS_OUT_BYTES && (buf = malloc(sb.st_size))) {
        ssize_t n = 0, r = 0;
        while(n < sb.st_size && (r = read(fd, buf + n, sb.st_size - n)) != 0 && (r > 0 || errno == EINTR)) n += r > 0 ? r : 0;
        if(n == sb.st_size && stbi_info_from_memory(buf, n, w, h, &c) && *w <= GS_OUT_DIM && *h <= GS_OUT_DIM)
            px = stbi_load_from_memory(buf, n, w, h, &c, 4);
    }
    free(buf);
    close(fd);
    for(int i = 0; px && i < *w * *h; i++) { unsigned char t = px[4 * i]; px[4 * i] = px[4 * i + 2]; px[4 * i + 2] = t; }
    return px;
}
void custom_still_free(unsigned char *px) { stbi_image_free(px); }
/* Worker: verify the output and select it, or put everything back. No other apply can start meanwhile. */
static void *gs_worker(void *unused) {
    (void)unused;
    char tgt[GA_PATH], stage[GA_PATH], bak[GA_PATH], db[GA_PATH];
    gs_paths(tgt, stage, bak, db);
    custom_gif_status st = gs.reply ? gs_output(tgt) : CUSTOM_GIF_CONVERT;
    if(!st) st = ga_db_status(custom_saver_db_install(db, GS_NAME, GS_TARGET, 200, GA_BUSY_MS, NULL, NULL),
                              &(ga_notify_ctx){ IPC_GIF_SELECT_OK, 0 });
    int retain = 0;
    if(st) {   /* the player already removed the old PNG and stored LOCK_THEME 5 */
        if(ga_restore(tgt, bak, stage, gs.had, &retain)) st = CUSTOM_GIF_RESTORE;
        if(ipc_saver_reselect(gs.prev)) st = CUSTOM_GIF_RESTORE;
    }
    if(ga_remove(stage) || (!retain && ga_remove(bak))) fprintf(stderr, "custom still: cleanup failed\n");
    ipc_conv_release(gs.op);
    pthread_mutex_lock(&gs_mu);
    gs.res = st; gs.drop = !st; gs.stuck = retain; gs.st = GS_DONE;
    pthread_mutex_unlock(&gs_mu);
    ui_notify(UI_WAKE_STILL);
    return NULL;
}
/* UI thread: join a finished worker, then drop the cached image so the next draw reads the new file. */
static void gs_reap(void) {
    pthread_mutex_lock(&gs_mu);
    int join = gs.st == GS_DONE && gs.th_live, drop = gs.st == GS_DONE && gs.drop;
    if(join) gs.th_live = 0;
    if(drop) gs.drop = 0;
    pthread_t th = gs.th;
    pthread_mutex_unlock(&gs_mu);
    if(join) pthread_join(th, NULL);
    if(drop) GA_CACHE_DROP("D:" GS_TARGET);
}
void custom_still_tick(void) {
    gs_reap();
    pthread_mutex_lock(&gs_mu);
    if(gs.st != GS_WAIT) { pthread_mutex_unlock(&gs_mu); return; }
    unsigned code;
    ipc_conv_state c = ipc_conv_poll(gs.op, &code);
    /* Only a confirmed completion permits finalization. A queue generation
     * change does not prove that the old converter stopped writing. */
    int done = c == IPC_CONV_DONE || c == IPC_CONV_FAILED;
    gs.unresolved = !done && c != IPC_CONV_PENDING;
    if(done) {
        gs.reply = c == IPC_CONV_DONE; gs.st = GS_FINAL; gs.th_live = 1;
        pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 256 * 1024);
        int bad = pthread_create(&gs.th, &at, gs_worker, NULL);
        pthread_attr_destroy(&at);
        if(bad) {
            gs.th_live = 0;
            gs.st = GS_WAIT;   /* retain the IPC result; retry on the next tick */
        }
    }
    pthread_mutex_unlock(&gs_mu);
}
custom_still_state custom_still_poll(custom_gif_status *result) {
    gs_reap();
    pthread_mutex_lock(&gs_mu);
    custom_still_state r = gs.st == GS_IDLE ? CUSTOM_STILL_IDLE : gs.st == GS_DONE ? CUSTOM_STILL_DONE :
                           gs.unresolved ? CUSTOM_STILL_UNRESOLVED : CUSTOM_STILL_PENDING;
    if(r == CUSTOM_STILL_DONE) { if(result) *result = gs.res; gs.st = GS_IDLE; }
    pthread_mutex_unlock(&gs_mu);
    return r;
}

custom_gif_status custom_gif_apply(const char *src, custom_gif_decode_fn decode) {
    if(pthread_mutex_trylock(&ga_mu)) return CUSTOM_GIF_BUSY;
    custom_gif_status st = CUSTOM_GIF_CONVERSION;
    if(gs_idle() && !ipc_apply_reserve()) {
        st = ga_apply(src, decode);
        ipc_apply_release();
    }
    pthread_mutex_unlock(&ga_mu);
    return st;
}

int custom_still_waiting(void){
    pthread_mutex_lock(&gs_mu);int waiting=gs.st==GS_WAIT;pthread_mutex_unlock(&gs_mu);return waiting;
}
