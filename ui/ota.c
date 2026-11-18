/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Over-the-air diskOS UI updates, core (no LVGL; host-tested). Contract: update-system/OTA_CONTRACT.md.
 * Flow: releases/latest -> tag + "are the six bundle assets listed" -> wget each file into <updates>/incoming (bounded,
 * cancellable, stall-guarded) -> the read-only rootfs verifier (the same sh file boot runs) -> fsync -> ONE atomic
 * rename(incoming, pending). Nothing here installs a binary or touches /usr/data/mq_ui. */
#include "ota.h"
#include "system.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <glob.h>
#include <signal.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <sys/wait.h>

const ota_paths_t OTA_DEV = {
    "/usr/data/updates", "/etc/diskos-ota/root.pub.pem", "/etc/diskos-ota/epochs", "/etc/diskos-ota/diskos-verify.sh",
    SYS_GITHUB_LATEST, SYS_GITHUB_DOWNLOAD, "/usr/data", "/sys/class/net/wlan0/operstate", "/tmp/.diskos_run", "/tmp/.diskos_launched", "/proc/asound/card*/pcm*p/sub*/status", "/proc/self/exe"
};

/* the release asset (all under one tag), the name it is saved as, and its size cap (0 = the manifest's size) */
static const struct { const char *asset, *local; long cap; } OTA_FILES[OTA_NFILES] = {
    { "diskos-ota-keyauthz",              "keyauthz",             8192 },
    { "diskos-ota-keyauthz.sig",          "keyauthz.sig",         4096 },
    { "diskos-ota-leafpub.pem",           "leafpub.pem",          4096 },
    { "diskos-ota-update.manifest",       "update.manifest",      8192 },
    { "diskos-ota-update.manifest.sig",   "update.manifest.sig",  4096 },
    { "diskos-ota-mq_ui",                 "mq_ui",                0 },
};
#define OTA_MANIFEST_IDX 3
#define OTA_UI_IDX 5
#define OTA_JSON_CAP (256 * 1024)
#define OTA_MARGIN (1024 * 1024)

int ota_asset_name(int i, const char **asset, const char **local){
    if(i < 0 || i >= OTA_NFILES) return -1;
    if(asset) *asset = OTA_FILES[i].asset;
    if(local) *local = OTA_FILES[i].local;
    return 0;
}

/* ---------------------------------------------------------------- release JSON */
/* Minimal JSON walk: only the "assets" array of the top-level release object counts, and only each element's own
 * top-level "name" string. A release title or a nested "name" is never an asset. Malformed input = no assets. */
static const char *jws(const char *s){ while(*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++; return s; }
static const char *jstr_end(const char *s){                       /* s at an opening quote; returns the closing quote or NULL */
    for(s++; *s; s++){ if(*s == '\\'){ if(!*++s) return NULL; continue; } if(*s == '"') return s; }
    return NULL;
}
static const char *jskip(const char *s, int depth){               /* skip one value; returns the char after it, or NULL */
    s = jws(s);
    if(depth > 12 || !*s) return NULL;
    if(*s == '"'){ const char *e = jstr_end(s); return e ? e + 1 : NULL; }
    if(*s == '{' || *s == '['){
        char close = *s == '{' ? '}' : ']';
        s = jws(s + 1);
        if(*s == close) return s + 1;
        for(;;){
            if(close == '}'){ if(*s != '"') return NULL; const char *e = jstr_end(s); if(!e) return NULL; s = jws(e + 1); if(*s != ':') return NULL; s++; }
            s = jskip(s, depth + 1); if(!s) return NULL;
            s = jws(s);
            if(*s == ','){ s = jws(s + 1); continue; }
            return *s == close ? s + 1 : NULL;
        }
    }
    while(*s && *s != ',' && *s != '}' && *s != ']' && *s != ' ' && *s != '\n' && *s != '\r' && *s != '\t') s++;   /* number/true/null */
    return s;
}
int ota_assets_ok(const char *json){
    if(!json) return 0;
    int seen[OTA_NFILES] = {0}, have_assets = 0;
    const char *s = jws(json);
    if(*s != '{') return 0;
    s = jws(s + 1);
    while(*s == '"'){
        const char *ke = jstr_end(s); if(!ke) return 0;
        int is_assets = (ke - s - 1 == 6 && !strncmp(s + 1, "assets", 6));
        s = jws(ke + 1); if(*s != ':') return 0;
        s = jws(s + 1);
        if(is_assets){
            if(have_assets || *s != '[') return 0;                    /* duplicate or non-array "assets" */
            have_assets = 1;
            s = jws(s + 1);
            while(*s && *s != ']'){
                if(*s != '{'){ s = jskip(s, 1); if(!s) return 0; }
                else {
                    s = jws(s + 1);
                    while(*s == '"'){
                        const char *ne = jstr_end(s); if(!ne) return 0;
                        int is_name = (ne - s - 1 == 4 && !strncmp(s + 1, "name", 4));
                        s = jws(ne + 1); if(*s != ':') return 0;
                        s = jws(s + 1);
                        if(is_name && *s == '"'){
                            const char *ve = jstr_end(s); if(!ve) return 0;
                            for(int i = 0; i < OTA_NFILES; i++){
                                size_t l = strlen(OTA_FILES[i].asset);
                                if((size_t)(ve - s - 1) == l && !strncmp(s + 1, OTA_FILES[i].asset, l)) seen[i]++;
                            }
                        }
                        s = jskip(s, 2); if(!s) return 0;
                        s = jws(s);
                        if(*s == ','){ s = jws(s + 1); continue; }
                        break;
                    }
                    if(*s != '}') return 0;
                    s++;
                }
                s = jws(s);
                if(*s == ','){ s = jws(s + 1); continue; }
                break;
            }
            if(*s != ']') return 0;
            s++;
        } else { s = jskip(s, 1); if(!s) return 0; }
        s = jws(s);
        if(*s == ','){ s = jws(s + 1); continue; }
        break;
    }
    if(*s != '}' || !have_assets) return 0;
    for(int i = 0; i < OTA_NFILES; i++) if(seen[i] != 1) return 0;    /* every asset exactly once */
    return 1;
}

/* Strict version grammar: [v]N.N.N, each 1-5 digits, nothing before or after. */
int ota_ver_parse(const char *s, int allow_v, int v[3]){
    if(!s) return -1;
    if(allow_v && *s == 'v') s++;
    for(int i = 0; i < 3; i++){
        int n = 0, d = 0;
        while(isdigit((unsigned char)*s) && d < 5){ n = n * 10 + (*s - '0'); s++; d++; }
        if(!d || isdigit((unsigned char)*s)) return -1;
        v[i] = n;
        if(i < 2){ if(*s != '.') return -1; s++; }
    }
    return *s ? -1 : 0;
}
int ota_ver_cmp(const char *a, int a_v, const char *b, int b_v){    /* -1 a<b, 0, 1, or -2 if either is not strict */
    int x[3], y[3];
    if(ota_ver_parse(a, a_v, x) != 0 || ota_ver_parse(b, b_v, y) != 0) return -2;
    for(int i = 0; i < 3; i++) if(x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

/* ---------------------------------------------------------------- child processes */
/* Run argv, capture stdout (and stderr when merge_err) into buf (cap bytes, NUL-terminated). Returns the exit status,
 * or -1 for a signal/spawn failure. *capped = the output hit the cap (child killed; the caller treats it as failure). */
static int run_capture(char *const argv[], int use_path, int merge_err, char *buf, size_t cap, size_t *got, int *capped){
    int pfd[2];
    *got = 0; *capped = 0;
    if(pipe(pfd) != 0) return -1;
    pid_t p = fork();
    if(p < 0){ close(pfd[0]); close(pfd[1]); return -1; }
    if(p == 0){
        setpgid(0, 0);
        int nul = open("/dev/null", O_RDWR);
        if(nul >= 0) dup2(nul, 0);
        dup2(pfd[1], 1);
        if(merge_err) dup2(pfd[1], 2); else if(nul >= 0) dup2(nul, 2);
        close(pfd[0]); close(pfd[1]);
        if(use_path) execvp(argv[0], argv); else execv(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);
    while(*got < cap){
        ssize_t r = read(pfd[0], buf + *got, cap - *got);
        if(r < 0 && errno == EINTR) continue;
        if(r <= 0) break;
        *got += (size_t)r;
    }
    if(*got >= cap){ *capped = 1; killpg(p, SIGKILL); kill(p, SIGKILL); }
    close(pfd[0]);
    int st = 0, waited = 0;
    while(!waited){ if(waitpid(p, &st, 0) >= 0) waited = 1; else if(errno != EINTR) break; }
    buf[*got < cap ? *got : cap - 1] = 0;
    return (waited && WIFEXITED(st)) ? WEXITSTATUS(st) : -1;
}

/* One deferred child at most. No new fetch is admitted until it is collected.
 * The cleanup thread sleeps when idle and never uses blocking waitpid. */
static atomic_flag fetch_busy = ATOMIC_FLAG_INIT;
static _Atomic pid_t fetch_deferred;
static pthread_once_t fetch_reaper_once = PTHREAD_ONCE_INIT;
static sem_t fetch_reap_ready;
static int fetch_reaper_ok;

static void *fetch_reaper(void *unused){
    (void)unused;
    for(;;){
        while(sem_wait(&fetch_reap_ready) != 0) {}
        pid_t child = atomic_load(&fetch_deferred);
        for(;;){
            pid_t w = waitpid(child, NULL, WNOHANG);
            if(w == child || (w < 0 && errno == ECHILD)) break;
            poll(NULL, 0, 100);
        }
        atomic_store(&fetch_deferred, 0);
    }
    return NULL;
}

static void fetch_reaper_init(void){
    pthread_attr_t attr;
    pthread_t th;
    if(sem_init(&fetch_reap_ready, 0, 0) != 0) return;
    if(pthread_attr_init(&attr) != 0){ sem_destroy(&fetch_reap_ready); return; }
    int rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if(rc == 0) rc = pthread_create(&th, &attr, fetch_reaper, NULL);
    pthread_attr_destroy(&attr);
    if(rc != 0){ sem_destroy(&fetch_reap_ready); return; }
    fetch_reaper_ok = 1;
}

/* Release lookup only: the verifier deliberately retains run_capture above. */
static int64_t fetch_now_ms(void){
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int fetch_capture(char *const argv[], char *buf, size_t cap, const _Atomic int *cancel){
    int64_t start = fetch_now_ms();
    if(start < 0 || (cancel && atomic_load(cancel))) return -1;
    int64_t deadline = start + OTA_FETCH_DEADLINE_S * 1000;
    int fds[2];
    if(pipe2(fds, O_CLOEXEC) != 0) return -1;
    if(fcntl(fds[0], F_SETFL, O_NONBLOCK) != 0){ close(fds[0]); close(fds[1]); return -1; }
    pid_t child = fork();
    if(child < 0){ close(fds[0]); close(fds[1]); return -1; }
    if(child == 0){
        /* Never exec in diskOS's group, even if group setup fails. The parent
         * also owns the deadline while this setup (including open) is running. */
        if(setpgid(0, 0) != 0) _exit(127);
        int nul = open("/dev/null", O_RDWR);
        if(nul < 0 || dup2(nul, 0) < 0 || dup2(fds[1], 1) < 0 || dup2(nul, 2) < 0) _exit(127);
        if(nul > 2) close(nul);
        close(fds[0]); close(fds[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    /* Resolve the pre-setpgid scheduling race; a group ID of child can never
     * name diskOS's own group. Always kill the direct child too on failure. */
    setpgid(child, child);
    size_t got = 0;
    int eof = 0, status;
    for(;;){
        int64_t now = fetch_now_ms();
        if(now < 0 || now >= deadline || (cancel && atomic_load(cancel))) break;
        if(!eof){
            ssize_t nread = read(fds[0], buf + got, cap - got);
            if(nread > 0){
                got += (size_t)nread;
                if(got == cap) break;                 /* same JSON cap as before */
                continue;                            /* recheck deadline on every read */
            }
            if(nread == 0){ eof = 1; close(fds[0]); }
            else if(errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) break;
        }
        if(eof){
            pid_t w = waitpid(child, &status, WNOHANG);
            if(w == child){
                buf[got] = 0;
                return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
            if(w < 0 && errno != EINTR) break;
        }
        int ms = (int)(deadline - now);
        if(ms > 50) ms = 50;                          /* cancellation latency */
        struct pollfd fd = { eof ? -1 : fds[0], POLLIN, 0 };
        if(poll(&fd, 1, ms) < 0 && errno != EINTR) break;
    }
    if(!eof) close(fds[0]);
    killpg(child, SIGKILL);
    kill(child, SIGKILL);
    /* No blocking waitpid, including after SIGKILL. Give normal kernel exit
     * up to one extra second; EINTR neither restarts nor shortens that budget. */
    int64_t reap_deadline = fetch_now_ms() + 1000;
    for(;;){
        pid_t w = waitpid(child, &status, WNOHANG);
        if(w == child || (w < 0 && errno == ECHILD)) return -1;
        int64_t now = fetch_now_ms();
        if(now < 0 || now >= reap_deadline) break;
        poll(NULL, 0, 10);
    }
    atomic_store(&fetch_deferred, child);
    sem_post(&fetch_reap_ready);
    return -1;
}

int ota_fetch_release(const ota_paths_t *p, char *tag, size_t n, int *has_bundle){
    return ota_fetch_release_cancel(p, tag, n, has_bundle, NULL);
}

int ota_fetch_release_cancel(const ota_paths_t *p, char *tag, size_t n, int *has_bundle, const _Atomic int *cancel){
    if(has_bundle) *has_bundle = 0;
    if(atomic_flag_test_and_set(&fetch_busy)) return -1;
    pthread_once(&fetch_reaper_once, fetch_reaper_init);
    if(!fetch_reaper_ok || atomic_load(&fetch_deferred)){
        atomic_flag_clear(&fetch_busy);
        return -1;
    }
    char *buf = malloc(OTA_JSON_CAP + 1);
    if(!buf){ atomic_flag_clear(&fetch_busy); return -1; }
    char *argv[] = { "wget", "-qO-", "-T", "12", (char *)p->latest_url, NULL };
    int rc = fetch_capture(argv, buf, OTA_JSON_CAP + 1, cancel);
    int ok = -1;
    if(rc == 0 && sys_parse_tag(buf, tag, n) == 0){
        ok = 0;
        int tv[3];
        if(has_bundle) *has_bundle = ota_ver_parse(tag, 1, tv) == 0 && ota_assets_ok(buf);   /* a tag outside the grammar is never offered */
    }
    free(buf);
    atomic_flag_clear(&fetch_busy);
    return ok;
}

int ota_wifi_down(const ota_paths_t *p){
    char b[24] = "";
    int fd = open(p->operstate, O_RDONLY | O_CLOEXEC);
    if(fd < 0) return 0;                                  /* unknown state is not "down": let the fetch decide */
    ssize_t r = read(fd, b, sizeof b - 1);
    close(fd);
    if(r <= 0) return 0;
    b[r] = 0;
    while(r > 0 && (b[r - 1] == '\n' || b[r - 1] == '\r' || b[r - 1] == ' ')) b[--r] = 0;
    return !strcmp(b, "down");
}

/* ---------------------------------------------------------------- small file helpers */
/* /tmp/.diskos_run grows GOOD_* and PREV_* lines while a good build exists (S97 write_run, about 540 bytes at most);
 * the launcher accepts up to 1023 bytes (diskos_launch.c), so the UI reads it with the same limit. */
enum { OTA_RUN_MAX = 1024 };
static int read_file(const char *path, char *buf, size_t n){          /* regular non-symlink file, NUL-terminated; length or -1 */
    struct stat st;
    if(lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || (size_t)st.st_size >= n) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if(fd < 0) return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if(r < 0) return -1;
    buf[r] = 0;
    return (int)r;
}
/* Decision predicate from installer/payload/S97diskos_install:318-326:
 * [ -f marker ] && [ ! -L marker ]; value=$(head -n 1 marker); compare value.
 * V2.57 BusyBox head stops at the first LF (or EOF); ash drops NUL bytes and
 * strips the final LF from command substitution. Every other byte, including CR,
 * is significant. Later lines and the total file size do not matter to boot.
 * The single intentional divergence from boot: examine at most 64 KiB total,
 * including NULs; reaching the cap before LF/EOF returns no valid decision so
 * a corrupt/hostile file cannot make the UI thread scan without bound. This
 * may re-ask the user; diskOS never writes such files. Compare retained bytes
 * without allocation; an extra/mismatched byte cannot be undone. The host
 * parity table runs real boot expressions and marks capped cases divergent. */
#define OTA_DECISION_READ_CAP (64 * 1024)
static int boot_marker_matches(const char *path, const char *expected){
    struct stat st;
    if(lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if(fd < 0) return 0;
    if(fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)){ close(fd); return 0; }
    unsigned char buf[4096];
    size_t examined = 0, used = 0, len = strlen(expected);
    int match = 0;
    while(examined < OTA_DECISION_READ_CAP){
        size_t remaining = OTA_DECISION_READ_CAP - examined;
        ssize_t r = read(fd, buf, remaining < sizeof buf ? remaining : sizeof buf);
        if(r < 0 && errno == EINTR) continue;
        /* Boot compares head's emitted output even if head reports a read error. */
        if(r <= 0){ match = used == len; break; }
        examined += (size_t)r;
        for(ssize_t i = 0; i < r; i++){
            if(buf[i] == '\n'){ match = used == len; goto done; }
            if(buf[i] == 0) continue;
            if(used == len || buf[i] != (unsigned char)expected[used]) goto done;
            used++;
        }
    }
done:
    close(fd);
    return match;
}
static int read_at(int dfd, const char *name, char *buf, size_t n){   /* same, relative to a directory fd (no path re-resolution) */
    int fd = openat(dfd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if(fd < 0) return -1;
    struct stat st;
    if(fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || (size_t)st.st_size >= n){ close(fd); return -1; }
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if(r < 0) return -1;
    buf[r] = 0;
    return (int)r;
}
static int all_digits(const char *s, size_t max){ size_t l = strlen(s); if(!l || l > max) return 0; for(; *s; s++) if(!isdigit((unsigned char)*s)) return 0; return 1; }
static int hex64(const char *s){ if(strlen(s) != 64) return 0; for(int i = 0; i < 64; i++) if(!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0; return 1; }
static void rm_at(int dfd, const char *name, int depth){                /* unlinkat; recurse into real directories only (never follows links) */
    struct stat st;
    if(fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) return;
    if(S_ISDIR(st.st_mode)){
        if(depth < 4){
            int fd = openat(dfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if(fd >= 0){
                DIR *d = fdopendir(fd);
                if(d){
                    struct dirent *e;
                    while((e = readdir(d))) if(strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) rm_at(dirfd(d), e->d_name, depth + 1);
                    closedir(d);
                } else close(fd);
            }
        }
        unlinkat(dfd, name, AT_REMOVEDIR);
    } else unlinkat(dfd, name, 0);
}
static int fsync_at(int dfd, const char *name){                        /* 0 = durable; the error is propagated, never dropped */
    int fd = openat(dfd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if(fd < 0) return -1;
    int r = fsync(fd);
    close(fd);
    return r;
}
static int64_t ota_free_bytes(const char *dir){
    struct statvfs sv;
    if(statvfs(dir, &sv) != 0) return -1;
    return (int64_t)((unsigned long long)sv.f_bavail * (unsigned long long)sv.f_frsize);
}
/* the staging root: created 0700, must be a real directory (not a link) owned by the running user with no group/other bits */
static int open_updates(const ota_paths_t *p){
    if(mkdir(p->updates, 0700) != 0 && errno != EEXIST) return -1;
    int fd = open(p->updates, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if(fd < 0) return -1;
    struct stat st;
    if(fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid()){ close(fd); return -1; }
    if((st.st_mode & 077) != 0 && fchmod(fd, 0700) != 0){ close(fd); return -1; }
    return fd;
}

/* ---------------------------------------------------------------- SHA-256 (health binding: hash of the running executable) */
typedef struct { uint32_t h[8]; uint64_t len; unsigned char buf[64]; size_t n; } sha_t;
static const uint32_t SHA_K[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(sha_t *c, const unsigned char *b){
    uint32_t w[64], a[8];
    for(int i = 0; i < 16; i++) w[i] = (uint32_t)b[i*4] << 24 | (uint32_t)b[i*4+1] << 16 | (uint32_t)b[i*4+2] << 8 | b[i*4+3];
    for(int i = 16; i < 64; i++){
        uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3), s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    memcpy(a, c->h, sizeof a);
    for(int i = 0; i < 64; i++){
        uint32_t t1 = a[7] + (ROR(a[4], 6) ^ ROR(a[4], 11) ^ ROR(a[4], 25)) + ((a[4] & a[5]) ^ (~a[4] & a[6])) + SHA_K[i] + w[i];
        uint32_t t2 = (ROR(a[0], 2) ^ ROR(a[0], 13) ^ ROR(a[0], 22)) + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
        a[7] = a[6]; a[6] = a[5]; a[5] = a[4]; a[4] = a[3] + t1; a[3] = a[2]; a[2] = a[1]; a[1] = a[0]; a[0] = t1 + t2;
    }
    for(int i = 0; i < 8; i++) c->h[i] += a[i];
}
static void sha_add(sha_t *c, const unsigned char *d, size_t l){
    c->len += l;
    while(l){ size_t k = 64 - c->n < l ? 64 - c->n : l; memcpy(c->buf + c->n, d, k); c->n += k; d += k; l -= k; if(c->n == 64){ sha_block(c, c->buf); c->n = 0; } }
}
static int sha256_file(const char *path, char hex[65]){
    static const uint32_t init[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    sha_t c; memcpy(c.h, init, sizeof init); c.len = 0; c.n = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if(fd < 0) return -1;
    unsigned char b[4096]; ssize_t r;
    while((r = read(fd, b, sizeof b)) > 0) sha_add(&c, b, (size_t)r);
    close(fd);
    if(r < 0) return -1;
    uint64_t bits = c.len * 8;
    unsigned char pad = 0x80; sha_add(&c, &pad, 1);
    unsigned char z = 0; while(c.n != 56) sha_add(&c, &z, 1);
    unsigned char lb[8]; for(int i = 0; i < 8; i++) lb[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha_add(&c, lb, 8);
    for(int i = 0; i < 8; i++) snprintf(hex + i * 8, 9, "%08x", c.h[i]);
    return 0;
}

int ota_supported(const ota_paths_t *p){
    return access(p->rootpub, R_OK) == 0 && access(p->epochs, R_OK) == 0 && access(p->verifier, R_OK) == 0;
}
/* MIN_KEY_EPOCH / MIN_UI_EPOCH from the RO epochs file; both strict digits or the update is refused (fail closed). */
static int read_epochs(const ota_paths_t *p, char *key, char *ui, size_t n){
    char t[256];
    if(read_file(p->epochs, t, sizeof t) < 0) return -1;
    if(sys_kv(t, "MIN_KEY_EPOCH", key, n) != 0 || sys_kv(t, "MIN_UI_EPOCH", ui, n) != 0) return -1;
    if(!all_digits(key, 9) || !all_digits(ui, 9)) return -1;
    return 0;
}

/* ---------------------------------------------------------------- download one file */
/* wget writes into the incoming directory THROUGH ITS FD (/proc/self/fd/N/...), so a swapped pathname cannot redirect it;
 * RLIMIT_FSIZE = cap + 1 stops it on the device even if this poller is slow (the +1 makes an over-cap file detectable). */
static int dl_file(const ota_paths_t *p, const char *tag, const char *asset, int ifd, const char *local, long long cap, ota_prog_t *pg, long long base){
    char url[400], part[80], target[160];
    if(snprintf(url, sizeof url, "%s/%s/%s", p->dl_base, tag, asset) >= (int)sizeof url) return OTA_E_BAD;
    if(snprintf(part, sizeof part, "%s.part", local) >= (int)sizeof part) return OTA_E_BAD;
    snprintf(target, sizeof target, "/proc/self/fd/%d/%s", ifd, part);
    unlinkat(ifd, part, 0);
    pid_t pid = fork();
    if(pid < 0) return OTA_E_IO;
    if(pid == 0){
        setpgid(0, 0);
        if(cap > 0){ struct rlimit rl = { (rlim_t)(cap + 1), (rlim_t)(cap + 1) }; setrlimit(RLIMIT_FSIZE, &rl); }
        int nul = open("/dev/null", O_RDWR);
        if(nul >= 0){ dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        execlp("wget", "wget", "-q", "-T", "15", "-t", "1", "-O", target, url, (char *)NULL);
        _exit(127);
    }
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    time_t last = now.tv_sec; long long lastsz = -1;
    int st = 0, rc = OTA_OK;
    for(;;){
        pid_t r = waitpid(pid, &st, WNOHANG);
        struct stat sb; long long sz = fstatat(ifd, part, &sb, AT_SYMLINK_NOFOLLOW) == 0 ? (long long)sb.st_size : 0;
        atomic_store(&pg->done, (long)(base + sz));
        if(r == pid) break;
        if(r < 0 && errno != EINTR){ rc = OTA_E_IO; break; }
        int kill_it = 0;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if(atomic_load(&pg->cancel)){ rc = OTA_E_CANCEL; kill_it = 1; }
        else if(cap > 0 && sz > cap){ rc = OTA_E_BAD; kill_it = 1; }                    /* bigger than the signed size: stop now */
        else if(sz != lastsz){ lastsz = sz; last = now.tv_sec; }
        else if(now.tv_sec - last >= OTA_STALL_S){ rc = OTA_E_NET; kill_it = 1; }
        if(kill_it){
            killpg(pid, SIGKILL); kill(pid, SIGKILL);
            while(waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
            unlinkat(ifd, part, 0);
            return rc;
        }
        struct timespec ts = { 0, 50 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    if(rc != OTA_OK){ unlinkat(ifd, part, 0); return rc; }
    struct stat sb;
    int have = fstatat(ifd, part, &sb, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(sb.st_mode);
    if(have && cap > 0 && (long long)sb.st_size > cap){ unlinkat(ifd, part, 0); return OTA_E_BAD; }   /* incl. wget stopped by the rlimit */
    if(!WIFEXITED(st) || WEXITSTATUS(st) != 0 || !have || sb.st_size <= 0){ unlinkat(ifd, part, 0); return OTA_E_NET; }
    if(renameat(ifd, part, ifd, local) != 0){ unlinkat(ifd, part, 0); return OTA_E_IO; }
    return OTA_OK;
}

/* ---------------------------------------------------------------- verify */
static int reject_class(const char *out){
    if(strstr(out, "signature invalid") || strstr(out, "substituted key") || strstr(out, "bad magic") || strstr(out, "role '")) return OTA_E_SIG;
    if(strstr(out, "downgrade") || strstr(out, "revoked key")) return OTA_E_OLDER;
    return OTA_E_BAD;
}
static int tok(const char *line, const char *key, char *out, size_t n){    /* " key=value" token of the verifier's OK line */
    char pat[24]; snprintf(pat, sizeof pat, " %s=", key);
    const char *k = strstr(line, pat);
    if(!k) return -1;
    k += strlen(pat);
    size_t i = 0;
    while(*k && *k != ' ' && *k != '\n'){
        if(!(isalnum((unsigned char)*k) || *k == '.' || *k == '-' || *k == '_') || i + 1 >= n) return -1;
        out[i++] = *k++;
    }
    if(!i) return -1;
    out[i] = 0;
    return 0;
}

static int fin(int ufd, int ifd, int rc){                                  /* failure exit: drop the work dir, release the fds */
    if(rc != OTA_OK && ufd >= 0) rm_at(ufd, "incoming", 0);
    if(ifd >= 0) close(ifd);
    if(ufd >= 0) close(ufd);
    return rc;
}

/* updates/rejected: the mq_ui SHA256 of each build the boot script rolled back, one per line (it keeps the last 8).
 * Boot refuses to try those again, so staging one would only download it for nothing. */
static int sha_rejected(int ufd, const char *sha){
    char r[1024];
    if(read_at(ufd, "rejected", r, sizeof r) < 0) return 0;
    size_t n = strlen(sha);
    for(char *s = r, *e; *s; s = *e ? e + 1 : e){
        if(!(e = strchr(s, '\n'))) e = s + strlen(s);
        if((size_t)(e - s) == n && memcmp(s, sha, n) == 0) return 1;
    }
    return 0;
}

int ota_stage(const ota_paths_t *p, const char *running, const char *tag, ota_prog_t *pg){
    char kep[16], uep[16], path[300];
    atomic_store(&pg->done, 0); atomic_store(&pg->total, 0); atomic_store(&pg->phase, 1); pg->need_free = 0; pg->version[0] = 0;

    if(!ota_supported(p)) return OTA_E_UNSUPPORTED;
    if(read_epochs(p, kep, uep, sizeof kep) != 0) return OTA_E_UNSUPPORTED;
    int tv[3];
    if(ota_ver_parse(running, 0, tv) != 0) return OTA_E_OLDER;
    if(ota_ver_parse(tag, 1, tv) != 0) return OTA_E_BAD;                       /* the whole tag must be vN.N.N */
    if(ota_ver_cmp(running, 0, tag, 1) != -1) return OTA_E_OLDER;              /* only strictly newer */
    int64_t fr = ota_free_bytes(p->data_root);
    if(fr < OTA_MARGIN){ pg->need_free = OTA_MARGIN; return OTA_E_SPACE; }

    int ufd = open_updates(p), ifd = -1;
    if(ufd < 0) return OTA_E_IO;                                               /* a link, a foreign owner or an unfixable mode: refuse */
    /* defence in depth: the last accepted epoch (wipeable, so never lower than the baked floor); same floor boot uses: accepted + 1 */
    char acc[128], av[16];
    if(read_at(ufd, "accepted", acc, sizeof acc) > 0 && sys_kv(acc, "epoch", av, sizeof av) == 0 && all_digits(av, 9) && atol(av) + 1 > atol(uep))
        snprintf(uep, sizeof uep, "%ld", atol(av) + 1);
    /* an interrupted pending replacement: put the aside copy back, or drop a stale one */
    struct stat sb;
    if(fstatat(ufd, "pending", &sb, AT_SYMLINK_NOFOLLOW) != 0 && fstatat(ufd, "pending.old", &sb, AT_SYMLINK_NOFOLLOW) == 0) renameat(ufd, "pending.old", ufd, "pending");
    else rm_at(ufd, "pending.old", 0);
    rm_at(ufd, "incoming", 0);
    if(mkdirat(ufd, "incoming", 0700) != 0) return fin(ufd, -1, OTA_E_IO);
    ifd = openat(ufd, "incoming", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);        /* NOT close-on-exec: wget/the verifier reach it as /proc/self/fd/N */
    if(ifd < 0) return fin(ufd, -1, OTA_E_IO);
    char ipath[48]; snprintf(ipath, sizeof ipath, "/proc/self/fd/%d", ifd);

    long long base = 0, ui_size = 0;
    for(int i = 0; i < OTA_NFILES; i++){
        long long cap = OTA_FILES[i].cap;
        if(i == OTA_UI_IDX){
            char mt[8192 + 1], sz[16];                                         /* the manifest is downloaded but NOT trusted yet: size only bounds work */
            if(read_at(ifd, OTA_FILES[OTA_MANIFEST_IDX].local, mt, sizeof mt) < 0 || sys_kv(mt, "size", sz, sizeof sz) != 0 || !all_digits(sz, 9)) return fin(ufd, ifd, OTA_E_BAD);
            char msha[72];                                                     /* a build rolled back here before: refuse before the big download */
            if(sys_kv(mt, "sha256", msha, sizeof msha) == 0 && hex64(msha) && sha_rejected(ufd, msha)) return fin(ufd, ifd, OTA_E_REJECTED);
            ui_size = atoll(sz);
            if(ui_size <= 0 || ui_size > OTA_MAX_UI) return fin(ufd, ifd, OTA_E_BAD);
            int64_t need = 2 * ui_size + OTA_MARGIN;                            /* boot copies pending/mq_ui next to the live one */
            fr = ota_free_bytes(p->data_root);
            if(fr < need){ pg->need_free = need; return fin(ufd, ifd, OTA_E_SPACE); }
            atomic_store(&pg->total, (long)(base + ui_size));
            cap = ui_size;
        }
        int rc = dl_file(p, tag, OTA_FILES[i].asset, ifd, OTA_FILES[i].local, cap, pg, base);
        if(rc != OTA_OK) return fin(ufd, ifd, rc);
        if(fstatat(ifd, OTA_FILES[i].local, &sb, AT_SYMLINK_NOFOLLOW) == 0) base += (long long)sb.st_size;
    }

    /* verify BEFORE staging, with the RO root key and floors */
    atomic_store(&pg->phase, 2);
    if(atomic_load(&pg->cancel)) return fin(ufd, ifd, OTA_E_CANCEL);
    char ui[80], out[512]; size_t got; int capped;
    snprintf(ui, sizeof ui, "%s/mq_ui", ipath);
    char *vargv[] = { "/bin/sh", (char *)p->verifier, (char *)p->rootpub, kep, uep, ipath, ui, NULL };
    int vrc = run_capture(vargv, 0, 1, out, sizeof out, &got, &capped);
    if(vrc != 0 || capped) return fin(ufd, ifd, (vrc == 1 && !capped) ? reject_class(out) : OTA_E_BAD);
    char ver[40], ep[16];
    if(strncmp(out, "OK ", 3) != 0 || tok(out, "version", ver, sizeof ver) != 0 || tok(out, "epoch", ep, sizeof ep) != 0 || !all_digits(ep, 18)) return fin(ufd, ifd, OTA_E_BAD);
    if(ota_ver_parse(ver, 0, tv) != 0) return fin(ufd, ifd, OTA_E_BAD);                  /* signed version must be strict N.N.N too */
    if(ota_ver_cmp(running, 0, ver, 0) != -1) return fin(ufd, ifd, OTA_E_OLDER);         /* a validly signed OLD bundle replayed under a new tag */
    if(ota_ver_cmp(tag, 1, ver, 0) != 0) return fin(ufd, ifd, OTA_E_BAD);                /* signed version must be the release's */

    /* commit: ready last, fsync everything (errors are failures), keep the old pending until the new one is durable */
    if(!ota_begin_commit(pg)) return fin(ufd, ifd, OTA_E_CANCEL);
    int wfd = openat(ifd, "ready.tmp", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if(wfd < 0) return fin(ufd, ifd, OTA_E_IO);
    char rt[80]; int rl = snprintf(rt, sizeof rt, "version=%s\nepoch=%s\n", ver, ep);
    int okw = write(wfd, rt, (size_t)rl) == rl && fsync(wfd) == 0;
    okw = (close(wfd) == 0) && okw;
    if(!okw || renameat(ifd, "ready.tmp", ifd, "ready") != 0) return fin(ufd, ifd, OTA_E_IO);
    for(int i = 0; i < OTA_NFILES; i++) if(fsync_at(ifd, OTA_FILES[i].local) != 0) return fin(ufd, ifd, OTA_E_IO);
    if(fsync_at(ifd, "ready") != 0 || fsync(ifd) != 0) return fin(ufd, ifd, OTA_E_IO);
    int had = fstatat(ufd, "pending", &sb, AT_SYMLINK_NOFOLLOW) == 0;
    if(had && renameat(ufd, "pending", ufd, "pending.old") != 0) return fin(ufd, ifd, OTA_E_IO);
    if(renameat(ufd, "incoming", ufd, "pending") != 0){
        if(had) renameat(ufd, "pending.old", ufd, "pending");
        return fin(ufd, ifd, OTA_E_IO);
    }
    if(fsync(ufd) != 0){                                                        /* not durable: put things back exactly as they were */
        renameat(ufd, "pending", ufd, "incoming");
        if(had) renameat(ufd, "pending.old", ufd, "pending");
        return fin(ufd, ifd, OTA_E_IO);
    }
    if(had){ rm_at(ufd, "pending.old", 0); fsync(ufd); }                        /* the old bundle goes only now (best effort: a leftover is swept next time) */
    snprintf(pg->version, sizeof pg->version, "%s", ver);
    (void)path;
    return fin(ufd, ifd, OTA_OK);
}

int ota_allowed(void){ return cfg_get_int("ota_allow", 1) != 0; }

/* Drop a staged bundle the user no longer wants. Only pending/ and its aside copy pending.old/ (which recovery would
 * otherwise rename back to pending/) are removed, by name relative to the updates directory fd, never following links:
 * active/, good/, good.prev/, accepted, rejected, trial* and the running build are not touched. pending.old goes first so
 * a power cut in between leaves a pending/ that can simply be discarded again. */
int ota_discard_pending(const ota_paths_t *p){
    int ufd = open_updates(p);
    if(ufd < 0) return 0;
    rm_at(ufd, "pending.old", 0);
    rm_at(ufd, "pending", 0);
    int ok = fsync(ufd) == 0;
    struct stat sb;
    if(fstatat(ufd, "pending", &sb, AT_SYMLINK_NOFOLLOW) == 0 || fstatat(ufd, "pending.old", &sb, AT_SYMLINK_NOFOLLOW) == 0) ok = 0;
    close(ufd);
    return ok;
}

int ota_pending(const ota_paths_t *p, char *ver, size_t n){
    char path[300], t[128];
    snprintf(path, sizeof path, "%s/pending/ready", p->updates);
    if(read_file(path, t, sizeof t) <= 0) return 0;
    if(sys_kv(t, "version", ver, n) != 0) return 0;
    return 1;
}

/* Trial boot = the launcher's intent (/tmp/.diskos_run) AND its statement of what it really started (/tmp/.diskos_launched)
 * both say BUILD=trial with the same 64-hex SHA256. No hashing here (cheap enough for the UI thread). */
int ota_trial_version(const ota_paths_t *p, char *ver, size_t n){
    char t[OTA_RUN_MAX], u[256], b1[16], b2[16], s1[80], s2[80];
    if(read_file(p->run_file, t, sizeof t) <= 0 || read_file(p->launched_file, u, sizeof u) <= 0) return 0;
    if(sys_kv(t, "BUILD", b1, sizeof b1) != 0 || strcmp(b1, "trial") != 0) return 0;
    if(sys_kv(u, "BUILD", b2, sizeof b2) != 0 || strcmp(b2, "trial") != 0) return 0;
    if(sys_kv(t, "SHA256", s1, sizeof s1) != 0 || sys_kv(u, "SHA256", s2, sizeof s2) != 0) return 0;
    if(!hex64(s1) || strcmp(s1, s2) != 0) return 0;
    if(ver){ if(sys_kv(t, "VERSION", ver, n) != 0) snprintf(ver, n, "%s", "update"); }
    return 1;
}
/* 0 only when the running executable is certainly not the trial: its size differs from the run file's SIZE. Cheap (no
 * hashing); a missing SIZE or an unreadable executable cannot rule the trial out and answers 1. */
int ota_trial_maybe_me(const ota_paths_t *p){
    char t[OTA_RUN_MAX], sz[32];
    struct stat st;
    if(read_file(p->run_file, t, sizeof t) <= 0 || sys_kv(t, "SIZE", sz, sizeof sz) != 0) return 1;
    if(stat(p->self_exe, &st) != 0) return 1;
    char *end; long long want = strtoll(sz, &end, 10);
    if(end == sz || *end) return 1;
    return want == (long long)st.st_size;
}
/* the strict form: as above, and the executable that is RUNNING hashes to that SHA256 (a fallback/flashed UI never vouches
 * for a trial it did not run, even with identical-looking run files). Fills sha. */
static int trial_is_me(const ota_paths_t *p, char sha[80]){
    char t[OTA_RUN_MAX], self[65];
    if(!ota_trial_version(p, NULL, 0)) return 0;
    if(read_file(p->run_file, t, sizeof t) <= 0 || sys_kv(t, "SHA256", sha, 80) != 0) return 0;
    if(sha256_file(p->self_exe, self) != 0 || strcmp(self, sha) != 0) return 0;
    return 1;
}
/* A saved choice counts only for this running trial. Avoid hashing when neither marker matches. */
int ota_trial_decided(const ota_paths_t *p){
    char path[300], t[OTA_RUN_MAX], sha[80], running_sha[80], expected[sizeof "SHA256=" + 64];
    if(!ota_trial_version(p, NULL, 0) || read_file(p->run_file, t, sizeof t) <= 0 ||
       sys_kv(t, "SHA256", sha, sizeof sha) != 0 || !hex64(sha)) return 0;
    int len = snprintf(expected, sizeof expected, "SHA256=%s", sha);
    if(len < 0 || (size_t)len >= sizeof expected) return 0;
    const char *names[] = { "healthy", "revert" };
    for(size_t i = 0; i < sizeof names / sizeof names[0]; i++){
        len = snprintf(path, sizeof path, "%s/%s", p->updates, names[i]);
        if(len < 0 || (size_t)len >= sizeof path) continue;
        if(boot_marker_matches(path, expected))
            return trial_is_me(p, running_sha) && strcmp(running_sha, sha) == 0;
    }
    return 0;
}
int ota_pcm_running(const ota_paths_t *p){
    glob_t g; int found = 0;
    if(glob(p->pcm_glob, 0, NULL, &g) != 0) return 0;
    for(size_t i = 0; i < g.gl_pathc && !found; i++){
        char t[512];
        if(read_file(g.gl_pathv[i], t, sizeof t) > 0 && strstr(t, "state: RUNNING")) found = 1;
    }
    globfree(&g);
    return found;
}
static int write_marker(const ota_paths_t *p, const char *name, const char *sha){          /* SHA256=<hex>\n, tmp + fsync + rename + dir fsync */
    char tmp[300], dst[300], b[80];
    snprintf(tmp, sizeof tmp, "%s/%s.tmp", p->updates, name);
    snprintf(dst, sizeof dst, "%s/%s", p->updates, name);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if(fd < 0) return 0;
    int l = snprintf(b, sizeof b, "SHA256=%s\n", sha);
    int ok = write(fd, b, (size_t)l) == l && fsync(fd) == 0;
    ok = (close(fd) == 0) && ok;
    if(!ok || rename(tmp, dst) != 0){ unlink(tmp); return 0; }
    int dfd = open(p->updates, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if(dfd < 0) return 0;
    ok = fsync(dfd) == 0;
    close(dfd);
    return ok;
}
/* Health proof for the trial build (contract 4a): only the RUNNING trial may write exactly SHA256=<its hash>. */
int ota_health_write(const ota_paths_t *p){
    char sha[80];
    return trial_is_me(p, sha) && write_marker(p, "healthy", sha);
}
/* "Go back": the user asked to drop the trial; boot honours <updates>/revert only when it names the trial's sha. */
int ota_revert_write(const ota_paths_t *p){
    char sha[80];
    return trial_is_me(p, sha) && write_marker(p, "revert", sha);
}
int ota_rollback_take(const ota_paths_t *p){
    char path[300], t[512];
    snprintf(path, sizeof path, "%s/rollback", p->updates);
    if(read_file(path, t, sizeof t) < 0) return 0;              /* absent, not a regular file, or oversize: not a marker */
    unlink(path);
    return 1;
}

const char *ota_err_text(int code){
    switch(code){
    case OTA_OK: return NULL;
    case OTA_E_CANCEL: return "Update cancelled";
    case OTA_E_NET: return "Couldn't reach GitHub. Check that Wi-Fi is connected.";
    case OTA_E_SPACE: return "Not enough space on the device";
    case OTA_E_UNSUPPORTED: return "Updates are not supported on this install";
    case OTA_E_OFF: return "Updates are turned off in Settings";
    case OTA_E_SIG: return "Update rejected - not signed by diskOS";
    case OTA_E_OLDER: return "Update refused - older than what you have";
    case OTA_E_BAD: return "Update rejected - the download is damaged";
    case OTA_E_REJECTED: return "Update refused - this version was rolled back on this device";
    default: return "Couldn't save the update. Try again.";
    }
}
