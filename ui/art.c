/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "sdio.h"
#include "art.h"
#include "receiver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/syscall.h>

/* int64 so tv_sec*1000 can't overflow a 32-bit long after ~24.9 days of monotonic uptime (which would
 * corrupt every deadline comparison below and kill fresh decodes on sight). */
static int64_t art_now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec*1000LL + t.tv_nsec/1000000LL; }

/* Artwork is made by diskos-artdec, a small helper shipped in the image: it extracts the embedded cover (or reads
 * a sibling cover file), decodes it and writes the 148px cover, 42px Home thumb and 360px blurred backdrop as 24-bit
 * BMPs. It replaced the stock `ffmpeg` command, which stock V2.57 ships without any image decoders. It runs as a
 * separate process (no shell): a hostile file can only kill the helper, and the kill/timeout/cancel logic below
 * applies unchanged. See tools/diskos_artdec.c for its limits and exit statuses. */
#define ARTDEC_PATH "/opt/diskos/bin/diskos-artdec"
/* The exact artwork layout the readers (LVGL BMP decoder, cover/accent/Album Wall readers) assume: 14-byte file
 * header + 40-byte BITMAPINFOHEADER, pixel offset 54, n x n (positive height = bottom-up), 1 plane, 24 bpp,
 * BI_RGB, rows padded to 4 bytes, and a file of exactly that length. Every field is checked in full. Used for
 * fresh helper output AND for cache hits. */
static uint32_t le32_(const unsigned char *p){ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
int art_bmp_valid(const char *path, int n){
    uint32_t stride = ((uint32_t)n * 3 + 3) & ~3u, dsz = stride * (uint32_t)n, want = 54 + dsz;
    unsigned char h[54];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if(fd < 0) return 0;
    struct stat st;
    int ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && (uint64_t)st.st_size == want && read(fd, h, 54) == 54 &&
             h[0] == 'B' && h[1] == 'M' && le32_(h + 2) == want && le32_(h + 6) == 0 && le32_(h + 10) == 54 &&
             le32_(h + 14) == 40 && le32_(h + 18) == (uint32_t)n && le32_(h + 22) == (uint32_t)n &&
             (h[26] | h[27] << 8) == 1 && (h[28] | h[29] << 8) == 24 && le32_(h + 30) == 0 &&
             (le32_(h + 34) == dsz || le32_(h + 34) == 0);
    close(fd);
    return ok;
}
/* In the CHILD, after fork: close every descriptor >= 3. Lists /proc/self/fd with raw openat/getdents64 (no malloc,
 * no stdio: safe after fork in a threaded process), so no descriptor escapes, however high. Returns 0 only if
 * closing is known to be complete; the child refuses to exec otherwise. If /proc cannot be opened, closes
 * 3..fallback_max, which is complete only when fallback_max covers the whole descriptor limit. */
struct art_dirent64 { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; };
static int art_child_close_fds(int fallback_max, int fallback_complete){
    long dfd;
    do dfd = syscall(SYS_openat, AT_FDCWD, "/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    while(dfd < 0 && errno == EINTR);
    if(dfd < 0){
        for(int fd = 3; fd < fallback_max; fd++) close(fd);
        return fallback_complete ? 0 : -1;
    }
    char buf[2048];
    for(;;){
        long n = syscall(SYS_getdents64, (int)dfd, buf, sizeof buf);
        if(n < 0 && errno == EINTR) continue;
        if(n < 0){ close((int)dfd); return -1; }          /* listing incomplete: do not trust it */
        if(n == 0) break;
        for(long off = 0; off < n;){
            struct art_dirent64 *d = (struct art_dirent64 *)(buf + off);
            if(d->reclen == 0) { close((int)dfd); return -1; }
            int v = 0, ok = d->name[0] != 0;
            for(const char *c = d->name; *c; c++){ if(*c < '0' || *c > '9'){ ok = 0; break; } v = v * 10 + (*c - '0'); }
            if(ok && v >= 3 && v != (int)dfd) close(v);
            off += d->reclen;
        }
    }
    close((int)dfd);
    return 0;
}
/* The LIVE (user-driven) decode registers its child here so art_cancel() can kill
 * it when the user skips ahead. Prewarm / fallback decodes pass cancellable=0 and
 * are never registered, so cancelling a skip only targets the on-screen track. */
static pthread_mutex_t g_pid_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile pid_t  g_live_pid = 0;
/* EVERY decoder child is also recorded here, cancellable or not. art_cancel() deliberately targets only
 * the on-screen track, but losing SD access is different: when the card must be released, a prewarm or
 * fallback decode holding it open has to go too. Small fixed array - the decoders are serialised by
 * g_decode_mu, so one or two entries are live at a time. */
#define ART_MAX_CHILDREN 8
static volatile pid_t g_all_pids[ART_MAX_CHILDREN];
static int art_child_slot_free(void){       /* caller holds g_pid_mu */
    for(int i = 0; i < ART_MAX_CHILDREN; i++) if(g_all_pids[i] == 0) return 1;
    return 0;
}
static void art_child_add(pid_t p){            /* caller holds g_pid_mu; a slot was reserved before fork */
    for(int i = 0; i < ART_MAX_CHILDREN; i++) if(g_all_pids[i] == 0){ g_all_pids[i] = p; return; }
}
static void art_child_del_locked(pid_t p){     /* caller holds g_pid_mu */
    for(int i = 0; i < ART_MAX_CHILDREN; i++) if(g_all_pids[i] == p){ g_all_pids[i] = 0; break; }
    if(g_live_pid == p) g_live_pid = 0;
}
/* Collect artwork children that outlived their request (still registered after the bounded reap). Decodes are
 * serialised by the callers' decode lock, so at the start of a new attempt every registered child is such a
 * leftover. The SD fault raised for it stays; this only stops it lingering as a zombie. Caller holds g_pid_mu. */
static void art_reap_leftovers_locked(void){
    for(int i = 0; i < ART_MAX_CHILDREN; i++){
        pid_t p = g_all_pids[i];
        if(p > 0 && waitpid(p, NULL, WNOHANG) == p) art_child_del_locked(p);
    }
}
/* Bumped by art_cancel(). A cancellable decode snapshots it on entry and re-checks it (under
 * g_pid_mu) before starting EACH helper child, so a skip is honored not just by killing the
 * running child but by refusing to launch a follow-up cover-fallback decode for the old track. */
static volatile unsigned g_cancel_gen = 0;
/* Bumped ONLY by art_kill_all() (SD access revoked). Unlike g_cancel_gen - which a track skip bumps and which
 * non-cancellable prewarm/fallback decodes deliberately ignore - EVERY decode request checks this before each
 * attempt, so revocation also stops the sibling-cover fallback from starting a new decoder. */
static volatile unsigned g_kill_gen = 0;
/* The g_kill_gen value a decode REQUEST started under, for this thread. art_run_pipeline compares it under
 * g_pid_mu immediately before every fork, so a revocation landing anywhere between the request's start and a
 * decoder launch - including during the sibling-cover access() probes - stops that launch. */
static __thread int      t_kill_armed = 0;
static __thread unsigned t_kill0 = 0;
/* Set by art_make_all_ex_gen: the token captured BEFORE it acquired SD admission, so a revocation that lands
 * between admission and the request start is still seen. Consumed (once) by the request. */
static __thread int      t_kill_preset_valid = 0;
static __thread unsigned t_kill_preset = 0;

/* Kill the in-flight LIVE decode (process group), if any, and supersede any between-attempt work.
 * Safe to call anytime. */
void art_cancel(void){
    pthread_mutex_lock(&g_pid_mu);
    g_cancel_gen++;
    pid_t p = g_live_pid;
    if(p > 0) kill(-p, SIGKILL);
    pthread_mutex_unlock(&g_pid_mu);
}
/* Kill EVERY decoder child, including the deliberately non-cancellable prewarm and fallback ones. Used
 * when SD access is revoked: a decoder still reading the card is exactly what must not outlive that. */
void art_kill_all(void){
    pthread_mutex_lock(&g_pid_mu);
    g_cancel_gen++;
    g_kill_gen++;
    for(int i = 0; i < ART_MAX_CHILDREN; i++){
        pid_t p = g_all_pids[i];
        if(p > 0){ kill(-p, SIGKILL); kill(p, SIGKILL); }
    }
    pthread_mutex_unlock(&g_pid_mu);
}

/* Run the artwork helper on ONE input as a killable child (execv, own process group).
 * cancellable=1 registers the child for art_cancel(). Returns 0 on success, -1 on error/no-art,
 * -2 if a newer request superseded this one (gen changed) before the child was launched. */
/* One helper run. saver_raw == NULL: "art" mode, publishing cover/thumb/backdrop. saver_raw != NULL: "saver" mode,
 * publishing one 360x360 BGRA file there (the three BMP paths are unused). */
/* The helper's own exit status from this thread's last run (its documented codes: 3 no picture, 4 unsupported,
 * 5 malformed, 6 over a limit, 7 I/O), or -1 when it was killed, cancelled, timed out or never started. Lets a caller
 * tell "this picture cannot be decoded" from "this attempt was interrupted" - the return value can't. */
static __thread int t_art_exit = -1;
int art_last_exit(void){ return t_art_exit; }
static int art_run_helper(const char *input, const char *saver_raw, const char *cover_bmp,
                    const char *thumb_bmp, const char *backdrop_bmp, int cancellable, unsigned gen0,
                    int64_t deadline_ms){
    t_art_exit = -1;
    if(art_now_ms() >= deadline_ms) return -1;     /* request deadline already spent: never start another child */
    /* A fresh directory per attempt, on the same filesystem as the outputs: the helper creates its three files
     * inside it exclusively, and only a complete, validated set is renamed into place. Leftovers from a killed
     * or failed attempt can never be mistaken for a result. */
    char stage[64]; snprintf(stage, sizeof stage, "/tmp/.diskos-artdec-XXXXXX");
    if(!mkdtemp(stage)) return -1;
    char sc[96], st_[96], sb[96], sv[96];
    snprintf(sc, sizeof sc, "%s/c.bmp", stage); snprintf(st_, sizeof st_, "%s/t.bmp", stage); snprintf(sb, sizeof sb, "%s/b.bmp", stage);
    snprintf(sv, sizeof sv, "%s/v.bgra", stage);
    /* everything the child needs is prepared before fork: after fork it only calls async-signal-safe functions */
    char *argv_art[] = { "diskos-artdec", "art", (char *)input, stage, NULL };
    char *argv_sav[] = { "diskos-artdec", "saver", (char *)input, sv, NULL };
    char **argv = saver_raw ? argv_sav : argv_art;
    /* fallback for when /proc is unreadable: close 3..limit, which is complete only if limit covers every fd */
    struct rlimit nof; int have_lim = getrlimit(RLIMIT_NOFILE, &nof) == 0;
    int maxfd = (have_lim && nof.rlim_cur <= 65536) ? (int)nof.rlim_cur : 65536;
    int fallback_complete = have_lim && nof.rlim_cur <= 65536;

    /* Hold g_pid_mu across fork + setpgid + register - for EVERY decoder, cancellable or not - so neither
     * art_cancel() nor art_kill_all() can run in the window between fork and registration (they would miss
     * this child). Capacity is reserved before forking: a child we could not record could not be killed.
     * Both parent and child call setpgid so the group exists before we unlock. */
    pthread_mutex_lock(&g_pid_mu);
    int ret = -1;
    if(cancellable && g_cancel_gen != gen0){ pthread_mutex_unlock(&g_pid_mu); ret = -2; goto cleanup; }  /* superseded - don't start */
    if(t_kill_armed && g_kill_gen != t_kill0){ pthread_mutex_unlock(&g_pid_mu); ret = -2; goto cleanup; }  /* SD revoked - don't start */
    if(!art_child_slot_free()){ pthread_mutex_unlock(&g_pid_mu); goto cleanup; }
    pid_t pid = fork();
    if(pid < 0){ pthread_mutex_unlock(&g_pid_mu); goto cleanup; }
    if(pid == 0){
        setpgid(0, 0);                       /* own group so a single kill takes the whole helper */
        if(art_child_close_fds(maxfd, fallback_complete) != 0) _exit(126);   /* never exec with unknown fds open */
        execv(ARTDEC_PATH, argv);
        _exit(127);                          /* exec failed */
    }
    setpgid(pid, pid);                       /* parent side of the race; ignore EACCES/ESRCH */
    if(cancellable) g_live_pid = pid;
    art_child_add(pid);
    pthread_mutex_unlock(&g_pid_mu);
    /* BOUNDED wait against a SHARED absolute monotonic deadline (art_now_ms), so the whole decode request
     * (embedded + every sibling-cover attempt) can't exceed ~15s of held g_decode_mu regardless of how many
     * attempts run, scheduling jitter, or EINTR storms. We check REAL elapsed time each iteration (not the
     * count of requested sleeps), and never round a near-expired deadline back up.
     * Each waitpid and the matching unregister happen under g_pid_mu: once waitpid collects the child its
     * pid may be reused, so a kill sweep must never see it still registered after that point. */
    int status = 0, killed = 0; pid_t w = 0;
    for(;;){
        if(art_now_ms() >= deadline_ms){                           /* over the shared deadline -> kill */
            pthread_mutex_lock(&g_pid_mu); kill(-pid, SIGKILL); kill(pid, SIGKILL); pthread_mutex_unlock(&g_pid_mu);
            killed = 1;
            int64_t rdl = art_now_ms() + 1000;                     /* bounded reap ~1s (real time) */
            int reaped = 0;
            while(art_now_ms() < rdl){
                pthread_mutex_lock(&g_pid_mu);
                if(waitpid(pid,&status,WNOHANG)==pid){ art_child_del_locked(pid); reaped=1; }
                pthread_mutex_unlock(&g_pid_mu);
                if(reaped) break;
                usleep(10*1000);
            }
            /* Not collected: it is still our (unreaped) child, so its pid cannot be reused - keep it
             * registered so later kill sweeps still reach it, and hold SD admission while it may still
             * be reading the card. */
            if(!reaped) sd_io_fault();
            break;
        }
        pthread_mutex_lock(&g_pid_mu);
        w = waitpid(pid, &status, WNOHANG);
        if(w == pid) art_child_del_locked(pid);                    /* collected: unregister in the same step */
        pthread_mutex_unlock(&g_pid_mu);
        if(w == pid) break;                                        /* child exited */
        if(w < 0 && errno == EINTR) continue;
        if(w < 0){                                                 /* waitpid error: not ours to track */
            pthread_mutex_lock(&g_pid_mu); art_child_del_locked(pid); pthread_mutex_unlock(&g_pid_mu);
            sd_io_fault(); killed = 1; break;
        }
        usleep(20*1000);
    }
    if(killed) goto cleanup;                                       /* waitpid failed / killed on timeout */
    if(WIFEXITED(status)) t_art_exit = WEXITSTATUS(status);        /* killed by a signal (cancel): stays -1 */
    if(!WIFEXITED(status) || WEXITSTATUS(status) != 0) goto cleanup;   /* killed, no picture, or bad input */
    /* publish only a complete set with the exact expected layout, and only if this request is still current */
    if(saver_raw){
        struct stat vs;
        if(stat(sv, &vs) != 0 || !S_ISREG(vs.st_mode) || vs.st_size != 360 * 360 * 4) goto cleanup;
    } else if(!art_bmp_valid(sc, 148) || !art_bmp_valid(st_, 42) || !art_bmp_valid(sb, 360)) goto cleanup;
    /* The final "still wanted?" check and the renames happen under g_pid_mu, the lock every cancellation and
     * revocation takes: no cancel can land between the check and the publish. (Renames are within tmpfs.) */
    pthread_mutex_lock(&g_pid_mu);
    if((cancellable && g_cancel_gen != gen0) || (t_kill_armed && g_kill_gen != t_kill0)) ret = -2;
    else if(art_now_ms() >= deadline_ms) ret = -1;          /* finished past the request deadline: not used */
    else if(saver_raw) ret = rename(sv, saver_raw) == 0 ? 0 : -1;
    else if(rename(sc, cover_bmp) != 0) ret = -1;
    /* all or nothing: a later rename failing takes back the earlier ones */
    else if(rename(st_, thumb_bmp) != 0){ unlink(cover_bmp); ret = -1; }
    else if(rename(sb, backdrop_bmp) != 0){ unlink(cover_bmp); unlink(thumb_bmp); ret = -1; }
    else ret = 0;
    pthread_mutex_unlock(&g_pid_mu);
cleanup:
    unlink(sc); unlink(st_); unlink(sb); unlink(sv);               /* whatever is left of this attempt */
    rmdir(stage);
    return ret;
}

static int art_run_pipeline(const char *input, const char *cover_bmp,
                    const char *thumb_bmp, const char *backdrop_bmp, int cancellable, unsigned gen0,
                    int64_t deadline_ms){
    return art_run_helper(input, NULL, cover_bmp, thumb_bmp, backdrop_bmp, cancellable, gen0, deadline_ms);
}

/* Folder covers: every folder is one album (the scanner names it after the folder), and a .jpg/.jpeg in the
 * folder is that album's cover. It wins over embedded art. With several JPEGs the usual cover names win (in this
 * order, any casing), then the first other JPEG by name, so the choice doesn't depend on directory order. Hidden
 * files (macOS "._x.jpg" resource forks) are skipped. Embedded art is the fallback when the folder has no JPEG. */
static const char *ART_JPG_PREFER[] = { "cover", "folder", "front", "albumart", "album" };
#define ART_JPG_PREFER_N (sizeof ART_JPG_PREFER / sizeof ART_JPG_PREFER[0])
#define ART_DIR_SCAN_MAX 4096   /* entries read per folder: a huge flat folder can't stall the art worker */
static int art_jpg_rank(const char *name){       /* -1 = not a cover JPEG; lower is better */
    if(name[0] == '.') return -1;
    const char *dot = strrchr(name, '.');
    if(!dot || dot == name || (strcasecmp(dot, ".jpg") != 0 && strcasecmp(dot, ".jpeg") != 0)) return -1;
    size_t stem = (size_t)(dot - name);
    for(unsigned i = 0; i < ART_JPG_PREFER_N; i++)
        if(strlen(ART_JPG_PREFER[i]) == stem && strncasecmp(name, ART_JPG_PREFER[i], stem) == 0) return (int)i;
    return (int)ART_JPG_PREFER_N;
}
/* The folder cover for `track` (full path into out, its stat into *st). 1 = found, 0 = none. */
static int art_folder_jpg(const char *track, char *out, size_t cap, struct stat *st){
    const char *slash = strrchr(track, '/');
    if(!slash) return 0;
    /* `track` is itself a picture (an Online Album Art download in the card's .diskos cache): decode just that,
     * never a neighbouring picture */
    static const char *IMG[] = { ".jpg", ".jpeg", ".png", ".bmp", ".gif", ".webp" };
    const char *ext = strrchr(slash, '.');
    for(unsigned i = 0; ext && i < sizeof IMG / sizeof IMG[0]; i++) if(strcasecmp(ext, IMG[i]) == 0) return 0;
    size_t dlen = (size_t)(slash - track);
    if(dlen == 0 || dlen > 900) return 0;
    char dir[1024]; memcpy(dir, track, dlen); dir[dlen] = 0;
    DIR *d = opendir(dir);
    if(!d) return 0;
    char best[256] = ""; int best_rank = -1; unsigned seen = 0;
    struct dirent *e;
    while((e = readdir(d)) && seen++ < ART_DIR_SCAN_MAX){
        int r = art_jpg_rank(e->d_name);
        if(r < 0 || strlen(e->d_name) >= sizeof best) continue;
        if(best_rank < 0 || r < best_rank || (r == best_rank && strcmp(e->d_name, best) < 0)){
            char p[1024]; struct stat s;
            int n = snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            if(n <= 0 || n >= (int)sizeof p || stat(p, &s) != 0 || !S_ISREG(s.st_mode)) continue;
            snprintf(best, sizeof best, "%s", e->d_name); best_rank = r;
        }
    }
    closedir(d);
    if(best_rank < 0) return 0;
    int n = snprintf(out, cap, "%s/%s", dir, best);
    if(n <= 0 || (size_t)n >= cap || stat(out, st) != 0 || !S_ISREG(st->st_mode)) return 0;
    return 1;
}
/* Non-JPEG sibling covers, tried after embedded art when the folder has no JPEG. The helper recognises PNG, BMP and
 * GIF by content, so the name only decides precedence. */
static const char *ART_COVER_NAMES[] = {
    "cover.png","folder.png","cover.bmp","folder.bmp",
};
static const unsigned ART_COVER_N = sizeof ART_COVER_NAMES / sizeof ART_COVER_NAMES[0];
/* Mix the identity of the track's sidecar cover files (the folder JPEG's name, size, mtime, then each fallback
 * file present, in lookup order) into *h, so replacing, adding or removing a cover invalidates the cache entry.
 * No inode: it is not stable on exfat/vfat (see artcache.c fingerprint). */
void art_sidecar_signature(const char *track, uint64_t *h){
    const char *slash = strrchr(track, '/');
    if(!slash) return;
    size_t dlen = (size_t)(slash - track);
    if(dlen == 0 || dlen > 900) return;
    { char jp[1024]; struct stat js;
      if(art_folder_jpg(track, jp, sizeof jp, &js)){
          for(const char *p = jp + dlen; *p; p++){ *h ^= (unsigned char)*p; *h *= 1099511628211ULL; }
          uint64_t v[2] = { (uint64_t)js.st_size, (uint64_t)js.st_mtime };
          for(int k = 0; k < 2; k++) for(int b = 0; b < 8; b++){ *h ^= (v[k] >> (b * 8)) & 0xff; *h *= 1099511628211ULL; }
      } }
    for(unsigned i = 0; i < ART_COVER_N; i++){
        char cov[1024]; struct stat st;
        int n = snprintf(cov, sizeof cov, "%.*s/%s", (int)dlen, track, ART_COVER_NAMES[i]);
        if(n <= 0 || n >= (int)sizeof cov || stat(cov, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        uint64_t v[3] = { i + 1u, (uint64_t)st.st_size, (uint64_t)st.st_mtime };
        for(int k = 0; k < 3; k++) for(int b = 0; b < 8; b++){ *h ^= (v[k] >> (b * 8)) & 0xff; *h *= 1099511628211ULL; }
    }
}

/* Render cover/thumb/backdrop for a track. Prefer the folder's JPEG (art_folder_jpg: the folder is the
 * album); if there is none or it can't be decoded, the track's EMBEDDED art, then a PNG/BMP sibling cover. A cancellable
 * decode snapshots the cancel generation and stops (returns -1, no wasted work) as soon as a skip
 * supersedes it, whether during the embedded run or between it and the cover fallback. */
/* Read the current cancel generation (for a caller that wants to capture it ATOMICALLY with its own
 * request identity, then pass it to art_make_all_ex_gen). art_cancel() is the only bumper and it's called
 * by the live-art setter while holding that setter's request lock, so a caller holding the same lock across
 * this read gets a token consistent with its request snapshot. */
unsigned art_cancel_gen(void){
    unsigned g; pthread_mutex_lock(&g_pid_mu); g = g_cancel_gen; pthread_mutex_unlock(&g_pid_mu); return g;
}

/* gen0 = the cancel token the CALLER captured with its request. A superseding skip bumps g_cancel_gen (via
 * art_cancel), so the per-child gen check trips and no obsolete decode runs - even if the skip lands after
 * the caller validated its request but before we get here. */
static int art_make_all_ex_gen_leased(const char *track, const char *cover_bmp,
                    const char *thumb_bmp, const char *backdrop_bmp, int cancellable, unsigned gen0){
    /* ONE 15s deadline across the whole request (embedded + every sibling-cover attempt), so a slow/hung
     * album can't hold g_decode_mu for attempts x 15s. Absolute monotonic so every attempt shares it. */
    int64_t deadline = art_now_ms() + 15000;
    /* SD revocation (art_kill_all) stops EVERY request: arm this thread's token, checked before each fork. The
     * token comes from before admission when the public wrapper captured one; direct callers snapshot here. */
    if(t_kill_preset_valid){ t_kill0 = t_kill_preset; t_kill_preset_valid = 0; }
    else { pthread_mutex_lock(&g_pid_mu); t_kill0 = g_kill_gen; pthread_mutex_unlock(&g_pid_mu); }
    t_kill_armed = 1;
    int r = -1;
    { char jp[1024]; struct stat js;                 /* the folder JPEG first */
      if(art_folder_jpg(track, jp, sizeof jp, &js))
          r = art_run_pipeline(jp, cover_bmp, thumb_bmp, backdrop_bmp, cancellable, gen0, deadline);
      if(r == 0){ t_kill_armed = 0; return 0; }
      if(r == -2){ t_kill_armed = 0; return -1; } }   /* superseded or revoked - stop */
    r = art_run_pipeline(track, cover_bmp, thumb_bmp, backdrop_bmp, cancellable, gen0, deadline);
    if(r == 0){ t_kill_armed = 0; return 0; }     /* embedded art */
    if(r == -2){ t_kill_armed = 0; return -1; }   /* superseded or revoked - stop */

    const char *slash = strrchr(track, '/');       /* else try a sibling cover file */
    if(!slash){ t_kill_armed = 0; return -1; }
    size_t dlen = (size_t)(slash - track);
    if(dlen == 0 || dlen > 900){ t_kill_armed = 0; return -1; }
    for(unsigned i=0; i<sizeof ART_COVER_NAMES/sizeof ART_COVER_NAMES[0]; i++){
        if(art_now_ms() >= deadline) break;        /* request deadline spent -> stop trying */
        unsigned kg; pthread_mutex_lock(&g_pid_mu); kg = g_kill_gen; pthread_mutex_unlock(&g_pid_mu);
        if(kg != t_kill0) break;                   /* SD revoked since this request began - stop probing */
        char cov[1024];
        int n = snprintf(cov, sizeof cov, "%.*s/%s", (int)dlen, track, ART_COVER_NAMES[i]);
        if(n <= 0 || n >= (int)sizeof cov) continue;
        if(access(cov, R_OK) != 0) continue;       /* not present */
        r = art_run_pipeline(cov, cover_bmp, thumb_bmp, backdrop_bmp, cancellable, gen0, deadline);
        if(r == 0)  break;                          /* rendered from the external cover */
        if(r == -2) break;                          /* superseded or revoked between attempts - stop */
    }
    t_kill_armed = 0;
    return r == 0 ? 0 : -1;
}

/* The vinyl screensaver's sharp 360x360 cover for `track`, written to out_raw as top-down BGRA. Same rules as the
 * artwork requests: SD admission for the whole request, the caller's absolute deadline (CLOCK_MONOTONIC ms, set
 * when the request was made, so time spent queued counts), revocation stops every attempt,
 * the folder JPEG, then embedded art, then the other sidecar cover files. Not cancellable by track skips (the saver checks its own
 * request generation before using the result). Blocking: call it from a worker thread, never the UI thread. */
int art_make_saver(const char *track, const char *out_raw, int64_t deadline_ms){
    unsigned kill0;
    /* collect abandoned children first: after an unreaped timeout admission stays closed, so this must not
     * depend on getting past sd_io_begin (the SD fault itself is kept) */
    pthread_mutex_lock(&g_pid_mu); art_reap_leftovers_locked(); kill0 = g_kill_gen; pthread_mutex_unlock(&g_pid_mu);
    if(!sd_io_begin()) return -1;
    t_kill0 = kill0; t_kill_armed = 1;
    int64_t deadline = deadline_ms;                           /* fixed when the request was POSTED, not now */
    int r = -1;
    { char jp[1024]; struct stat js;                          /* the folder JPEG first, like the artwork requests */
      if(art_folder_jpg(track, jp, sizeof jp, &js)) r = art_run_helper(jp, out_raw, NULL, NULL, NULL, 0, 0, deadline); }
    if(r == -1) r = art_run_helper(track, out_raw, NULL, NULL, NULL, 0, 0, deadline);
    if(r == -1){
        const char *slash = strrchr(track, '/');
        size_t dlen = slash ? (size_t)(slash - track) : 0;
        for(unsigned i = 0; dlen > 0 && dlen <= 900 && i < ART_COVER_N; i++){
            if(art_now_ms() >= deadline) break;
            unsigned kg; pthread_mutex_lock(&g_pid_mu); kg = g_kill_gen; pthread_mutex_unlock(&g_pid_mu);
            if(kg != t_kill0) break;
            char cov[1024];
            int n = snprintf(cov, sizeof cov, "%.*s/%s", (int)dlen, track, ART_COVER_NAMES[i]);
            if(n <= 0 || n >= (int)sizeof cov || access(cov, R_OK) != 0) continue;
            r = art_run_helper(cov, out_raw, NULL, NULL, NULL, 0, 0, deadline);
            if(r == 0 || r == -2) break;
        }
    }
    t_kill_armed = 0;
    sd_io_end();
    return r == 0 ? 0 : -1;
}

/* Snapshotting wrapper: captures the cancel generation internally (fine for callers that don't need to
 * couple it to an external request identity - non-cancellable ones ignore gen0 entirely). */
int art_make_all_ex(const char *track, const char *cover_bmp,
                    const char *thumb_bmp, const char *backdrop_bmp, int cancellable){
    unsigned gen0;
    pthread_mutex_lock(&g_pid_mu); gen0 = g_cancel_gen; pthread_mutex_unlock(&g_pid_mu);
    return art_make_all_ex_gen(track, cover_bmp, thumb_bmp, backdrop_bmp, cancellable, gen0);
}

/* Non-cancellable convenience wrapper (prewarm + synchronous fallback). */
int art_make_all(const char *track, const char *cover_bmp,
                 const char *thumb_bmp, const char *backdrop_bmp){
    return art_make_all_ex(track, cover_bmp, thumb_bmp, backdrop_bmp, 0);   /* NOLINT */
}

int art_make_all_ex_gen(const char *track, const char *cover_bmp, const char *thumb_bmp, const char *backdrop_bmp, int cancellable, unsigned gen0){
    /* Capture the revocation token BEFORE admission: taken after sd_io_begin, a revocation landing in between
     * would be absorbed into the snapshot and the request would still fork a decoder. */
    unsigned kill0;
    /* collect abandoned children first (independent of admission, which an unreaped child keeps closed) */
    pthread_mutex_lock(&g_pid_mu); art_reap_leftovers_locked(); kill0 = g_kill_gen; pthread_mutex_unlock(&g_pid_mu);
    if(!sd_io_begin()) return -1;
    t_kill_preset = kill0; t_kill_preset_valid = 1;
    int result = art_make_all_ex_gen_leased(track, cover_bmp, thumb_bmp, backdrop_bmp, cancellable, gen0);
    t_kill_preset_valid = 0;
    sd_io_end();
    return result;
}

#ifndef RECEIVER_ART_PATH
#define RECEIVER_ART_PATH "/usr/data/fiio/cover.jpg"
#endif
static int receiver_art_same_file(const struct stat *a, const struct stat *b){
    return a->st_dev==b->st_dev && a->st_ino==b->st_ino && a->st_size==b->st_size &&
        a->st_mtim.tv_sec==b->st_mtim.tv_sec && a->st_mtim.tv_nsec==b->st_mtim.tv_nsec &&
        a->st_ctim.tv_sec==b->st_ctim.tv_sec && a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;
}
/* One immutable, bounded tmpfs snapshot. No sibling/online fallback or SD cache.
 * aa05 does not prove publication succeeded or that these bytes own this item
 * (see the binding in ipc.c). File checks protect the copy interval only. */
int art_receiver_decode(const receiver_art_key *key, const char *cover,
                        const char *thumb, const char *backdrop, unsigned gen){
    int rc=-1, src=-1, dst=-1;
    char temp[]="/tmp/.diskos-receiver-XXXXXX", buf[8192];
    struct stat before, after, path;
    if(!receiver_art_current(key)) return -2;
    src=open(RECEIVER_ART_PATH,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(src<0 || fstat(src,&before) || !S_ISREG(before.st_mode) ||
       before.st_size<=0 || before.st_size>(8<<20)) goto done;
    dst=mkstemp(temp);
    if(dst<0) goto done;
    off_t left=before.st_size;
    while(left>0){
        ssize_t n=read(src,buf,left<(off_t)sizeof buf ? (size_t)left : sizeof buf);
        if(n<0 && errno==EINTR) continue;
        if(n<=0 || !receiver_art_current(key)) goto done;
        for(ssize_t off=0;off<n;){
            ssize_t w=write(dst,buf+off,(size_t)(n-off));
            if(w<0 && errno==EINTR) continue;
            if(w<=0) goto done;
            off+=w;
        }
        left-=n;
    }
    if(fstat(src,&after) || lstat(RECEIVER_ART_PATH,&path) ||
       !receiver_art_same_file(&before,&after) || !receiver_art_same_file(&before,&path)) goto done;
    close(dst); dst=-1;
    rc=art_run_pipeline(temp,cover,thumb,backdrop,1,gen,art_now_ms()+15000);
    if(!receiver_art_current(key)) rc=-2;
done:
    if(src>=0) close(src);
    if(dst>=0) close(dst);
    unlink(temp);
    return rc;
}
