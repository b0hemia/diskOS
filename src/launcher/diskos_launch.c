/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 diskOS contributors */
/* diskos-launch: read-only launcher for the diskOS UI (installed at /opt/diskos/bin/diskos-launch).
 *
 * The boot hook runs `diskos-launch /usr/data/mq_ui [args]` instead of exec'ing the writable /usr/data path
 * directly. S97 (the boot installer) has ALREADY verified the build it installed and published its sha256 and
 * size in /tmp/.diskos_run (tmpfs, root only). This launcher closes the remaining gap between "verified" and
 * "executed": it reads the target ONCE through a single open descriptor, hashing the bytes while it copies them
 * into a private, already-unlinked file on tmpfs, refuses anything that does not match the published sha256 /
 * size / ELF32-LE-MIPS header, and then execs THAT copy (fexecve). The bytes that run are exactly the bytes that
 * were hashed; nothing on /usr/data can change them afterwards.
 *
 * ONLY FOR OTA BUILDS. The boot hook uses this launcher only when S97 selected a signed, non-flashed build for the
 * boot; a user who never updates never reaches it.
 *
 * SINGLE-WINNER STARTUP. The stock watchdog in fiio_init.sh checks `pgrep -x mq_ui` about 5 s after the launch and, if
 * it finds nothing, kills mq_player (which can reboot the Disc) and starts the stock UI. So the very first thing this
 * program does is rewrite its own argv[0] to "mq_ui" IN PLACE (plus PR_SET_NAME), and only then try a clean re-exec
 * under that name; if the re-exec fails it simply carries on under the already-normalized name. It then walks the
 * ladder in-process by exec, never exiting between steps:
 *   1. the target (/usr/data/mq_ui), from a private verified copy
 *   2. the last GOOD bundle (only while a trial is active), verified against the sha256/size S97 published
 *   3. the previous good bundle (good.prev), same
 *   4. the flashed copy /opt/diskos/mq_ui, verified against the read-only /etc/diskos_manifest (private copy, or
 *      exec by path after the hash check: the rootfs is read-only so there is nothing to race)
 *   5. the stock UI /usr/bin/mq_ui
 * A descriptor on a WRITABLE source (/usr/data) is never exec'd: hashing it does not freeze its bytes. If the private
 * copy cannot be made or run, the candidate is skipped. All hashing shares one time budget; once it is spent only the
 * stock step remains. A read stuck in the kernel cannot be interrupted from here, so the hook runs a bounded startup
 * supervisor around this program (see imagebuild.py) that kills ONLY this process and starts the flashed build.
 *
 * LAUNCH TOKEN. The hook creates /tmp/.diskos_launch_token before starting this program and its startup supervisor
 * REVOKES it (deletes it) before it kills a stuck launcher and starts the fallback. The launcher checks the token
 * immediately before every exec; a launcher that wakes up late (I/O finally returned) finds it gone and exits
 * without starting anything, so a slow launcher can never become a second UI. (SIGKILL is the primary revocation:
 * once sent, a process in uninterruptible sleep dies on its way back to user mode; the token covers the rest.)
 *
 * Launch records describe a SUCCESSFUL exec only. Before each exec attempt a watcher (double-forked, so it is
 * orphaned, not the UI's child) is armed on a close-on-exec pipe: if exec succeeds the pipe closes and the watcher
 * writes /tmp/.diskos_launched (SHA256, BUILD=trial|good|flashed, EPOCH, VERSION) and, for a trial only,
 * /usr/data/updates/trial.launched; if exec fails the parent writes a byte and the watcher writes nothing. Both
 * records are removed at the start of every launch, so a fallback can never inherit the trial's proof.
 * (If the launcher is killed before exec the pipe closes too; the watcher then checks the parent is still alive.)
 * The UI reads /tmp/.diskos_launched (never /tmp/.diskos_run) before it writes its health marker.
 *
 * Crash loops inside one boot: every start of a TRIAL build is counted in /tmp/.diskos_starts (tmpfs, per boot).
 * After LAUNCH_MAX_TRIAL_STARTS starts the trial is skipped and /usr/data/updates/trial.crashed is left, which makes
 * the next boot roll the trial back at once.
 *
 * The paths are compile-time constants (overridable with -D only for the host tests; the shipped binary is
 * built without overrides). No environment variable or argument can point the verification elsewhere. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef RUN_PATH
#define RUN_PATH "/tmp/.diskos_run"
#endif
#ifndef MANIFEST_PATH
#define MANIFEST_PATH "/etc/diskos_manifest"
#endif
#ifndef FLASHED_PATH
#define FLASHED_PATH "/opt/diskos/mq_ui"
#endif
#ifndef PRIVATE_DIR
#define PRIVATE_DIR "/tmp"
#endif
#ifndef LAUNCHED_PATH
#define LAUNCHED_PATH "/tmp/.diskos_launched"
#endif
#ifndef STARTS_PATH
#define STARTS_PATH "/tmp/.diskos_starts"
#endif
#ifndef GOOD_PATH
#define GOOD_PATH "/usr/data/updates/good/mq_ui"
#endif
#ifndef CRASH_FLAG
#define CRASH_FLAG "/usr/data/updates/trial.crashed"
#endif
#ifndef TRIAL_LAUNCHED_PATH
#define TRIAL_LAUNCHED_PATH "/usr/data/updates/trial.launched"
#endif
#ifndef TOKEN_PATH
#define TOKEN_PATH "/tmp/.diskos_launch_token"
#endif
#ifndef PREV_PATH
#define PREV_PATH "/usr/data/updates/good.prev/mq_ui"
#endif
#ifndef STOCK_PATH
#define STOCK_PATH "/usr/bin/mq_ui"
#endif
#ifndef LOAD_BUDGET_MS
#define LOAD_BUDGET_MS 6000                 /* total wall time for reading+hashing candidates */
#endif
#ifndef LAUNCH_MAX_TRIAL_STARTS
#define LAUNCH_MAX_TRIAL_STARTS 3
#endif
#ifndef LOG_PATH
#define LOG_PATH "/usr/data/diskos_install.log"
#endif
#ifndef ELF_CLASS_WANT              /* ELFCLASS32: the device is 32-bit little-endian MIPS (EM_MIPS = 8) */
#define ELF_CLASS_WANT 1
#endif
#ifndef ELF_MACHINE_WANT
#define ELF_MACHINE_WANT 8
#endif
#define MAX_UI (64u * 1024u * 1024u)

extern char **environ;

/* ---- SHA-256 (FIPS 180-4) ---- */
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } sha_t;
static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(sha_t *s, const uint8_t *p) {
    uint32_t w[64], a, b, c, d, e, f, g, h, t1, t2;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) | ((uint32_t)p[i*4+2] << 8) | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; i++) {
        t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
static void sha_init(sha_t *s) {
    static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(s->h, iv, sizeof iv); s->len = 0; s->n = 0;
}
static void sha_update(sha_t *s, const uint8_t *p, size_t n) {
    s->len += n;
    while (n) {
        size_t k = 64 - s->n; if (k > n) k = n;
        memcpy(s->buf + s->n, p, k); s->n += k; p += k; n -= k;
        if (s->n == 64) { sha_block(s, s->buf); s->n = 0; }
    }
}
static void sha_hex(sha_t *s, char out[65]) {
    uint64_t bits = s->len * 8; uint8_t pad = 0x80, z = 0, lb[8];
    sha_update(s, &pad, 1);
    while (s->n != 56) sha_update(s, &z, 1);
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(s, lb, 8);
    for (int i = 0; i < 8; i++) snprintf(out + i * 8, 9, "%08x", s->h[i]);
}

/* ---- log (best effort, never blocks: O_NONBLOCK + regular-file check) ---- */
static void logmsg(const char *what, const char *path) {
    int fd = open(LOG_PATH, O_WRONLY | O_APPEND | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    struct stat st; char line[300]; int n;
    if (fd < 0) return;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
        n = snprintf(line, sizeof line, "launch: %s %s\n", what, path ? path : "");
        if (n > 0 && write(fd, line, (size_t)n) < 0) { /* best effort */ }
    }
    close(fd);
}

/* ---- S97's run file (or the RO manifest): KEY=VALUE, strict, duplicates rejected ---- */
struct meta { char sha[65]; unsigned long size; char build[16]; char epoch[20]; char version[40]; };
struct run  { struct meta cur; struct meta good; struct meta prev; int has_good; int has_prev; int ok; };

static int tok_ok(const char *v, size_t max) {           /* [A-Za-z0-9._-]{1,max} */
    size_t i, n = strlen(v);
    if (n == 0 || n > max) return 0;
    for (i = 0; i < n; i++)
        if (!((v[i] >= '0' && v[i] <= '9') || (v[i] >= 'a' && v[i] <= 'z') || (v[i] >= 'A' && v[i] <= 'Z') ||
              v[i] == '.' || v[i] == '_' || v[i] == '-')) return 0;
    return 1;
}
static int hex_ok(const char *v) {
    size_t i;
    if (strlen(v) != 64) return 0;
    for (i = 0; i < 64; i++) if (!((v[i] >= '0' && v[i] <= '9') || (v[i] >= 'a' && v[i] <= 'f'))) return 0;
    return 1;
}
static int size_ok(const char *v, unsigned long *out) {
    const char *e;
    if (!*v || strlen(v) > 9) return 0;
    for (e = v; *e; e++) if (*e < '0' || *e > '9') return 0;
    *out = strtoul(v, NULL, 10);
    return *out > 0 && *out <= MAX_UI;
}

static void read_run(const char *path, struct run *r) {
    char buf[1024], *p; struct stat st; ssize_t n; int fd;
    unsigned seen = 0;                                  /* bit per key: a key may appear once */
    memset(r, 0, sizeof *r);
    fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size >= (off_t)sizeof buf) { close(fd); return; }
    n = read(fd, buf, sizeof buf - 1); close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    strcpy(r->cur.build, "flashed"); strcpy(r->cur.epoch, "0"); strcpy(r->cur.version, "flashed");
    for (p = buf; p && *p; ) {
        char *nl = strchr(p, '\n'); const char *v; unsigned bit = 0;
        if (nl) *nl = 0;
        if ((v = (!strncmp(p, "SHA256=", 7) ? p + 7 : NULL))) { bit = 1; if (!hex_ok(v)) return; memcpy(r->cur.sha, v, 65); }
        else if ((v = (!strncmp(p, "SIZE=", 5) ? p + 5 : NULL))) { bit = 2; if (!size_ok(v, &r->cur.size)) return; }
        else if ((v = (!strncmp(p, "BUILD=", 6) ? p + 6 : NULL))) { bit = 4; if (!tok_ok(v, 15)) return; strcpy(r->cur.build, v); }
        else if ((v = (!strncmp(p, "EPOCH=", 6) ? p + 6 : NULL))) { bit = 8; if (!tok_ok(v, 19)) return; strcpy(r->cur.epoch, v); }
        else if ((v = (!strncmp(p, "VERSION=", 8) ? p + 8 : NULL))) { bit = 16; if (!tok_ok(v, 39)) return; strcpy(r->cur.version, v); }
        else if ((v = (!strncmp(p, "GOOD_SHA256=", 12) ? p + 12 : NULL))) { bit = 32; if (!hex_ok(v)) return; memcpy(r->good.sha, v, 65); }
        else if ((v = (!strncmp(p, "GOOD_SIZE=", 10) ? p + 10 : NULL))) { bit = 64; if (!size_ok(v, &r->good.size)) return; }
        else if ((v = (!strncmp(p, "GOOD_EPOCH=", 11) ? p + 11 : NULL))) { bit = 128; if (!tok_ok(v, 19)) return; strcpy(r->good.epoch, v); }
        else if ((v = (!strncmp(p, "GOOD_VERSION=", 13) ? p + 13 : NULL))) { bit = 256; if (!tok_ok(v, 39)) return; strcpy(r->good.version, v); }
        else if ((v = (!strncmp(p, "PREV_SHA256=", 12) ? p + 12 : NULL))) { bit = 512; if (!hex_ok(v)) return; memcpy(r->prev.sha, v, 65); }
        else if ((v = (!strncmp(p, "PREV_SIZE=", 10) ? p + 10 : NULL))) { bit = 1024; if (!size_ok(v, &r->prev.size)) return; }
        else if ((v = (!strncmp(p, "PREV_EPOCH=", 11) ? p + 11 : NULL))) { bit = 2048; if (!tok_ok(v, 19)) return; strcpy(r->prev.epoch, v); }
        else if ((v = (!strncmp(p, "PREV_VERSION=", 13) ? p + 13 : NULL))) { bit = 4096; if (!tok_ok(v, 39)) return; strcpy(r->prev.version, v); }
        if (bit) { if (seen & bit) return; seen |= bit; }
        p = nl ? nl + 1 : NULL;
    }
    if (!(seen & 1) || !(seen & 2)) return;
    r->ok = 1;
    if ((seen & (32 | 64)) == (32 | 64)) {              /* a usable good fallback needs its sha AND size */
        r->has_good = 1; strcpy(r->good.build, "good");
        if (!(seen & 128)) strcpy(r->good.epoch, "0");
        if (!(seen & 256)) strcpy(r->good.version, "good");
    }
    if ((seen & (512 | 1024)) == (512 | 1024)) {
        r->has_prev = 1; strcpy(r->prev.build, "good");
        if (!(seen & 2048)) strcpy(r->prev.epoch, "0");
        if (!(seen & 4096)) strcpy(r->prev.version, "good");
    }
}

/* write body to path atomically (tmp + rename), best effort */
static void put_file(const char *path, const char *body, int n, mode_t mode) {
    char tmp[300]; int fd;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    unlink(tmp);
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
    if (fd < 0) return;
    if (n > 0 && write(fd, body, (size_t)n) == n) { close(fd); if (rename(tmp, path) != 0) unlink(tmp); }
    else { close(fd); unlink(tmp); }
}

/* the launch records for a build whose exec SUCCEEDED (written by the watcher, never before) */
static void write_records(const struct meta *m) {
    char body[300]; int n;
    n = snprintf(body, sizeof body, "SHA256=%s\nBUILD=%s\nEPOCH=%s\nVERSION=%s\n", m->sha, m->build, m->epoch, m->version);
    put_file(LAUNCHED_PATH, body, n, 0644);
    if (!strcmp(m->build, "trial")) {
        n = snprintf(body, sizeof body, "SHA256=%s\n", m->sha);
        put_file(TRIAL_LAUNCHED_PATH, body, n, 0600);
    }
}
static void clear_records(void) {
    unlink(LAUNCHED_PATH); unlink(TRIAL_LAUNCHED_PATH);
}

/* per-boot count of TRIAL starts (tmpfs). Returns the number of THIS start (1 on any error). */
static int bump_starts(void) {
    char b[16] = {0}; int fd = open(STARTS_PATH, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600), n = 0;
    ssize_t r; int len;
    if (fd < 0) return 1;
    r = read(fd, b, sizeof b - 1);
    if (r > 0) n = atoi(b);
    if (n < 0 || n > 1000) n = 0;
    n++;
    len = snprintf(b, sizeof b, "%d\n", n);
    if (ftruncate(fd, 0) == 0 && lseek(fd, 0, SEEK_SET) == 0 && write(fd, b, (size_t)len) < 0) { /* best effort */ }
    close(fd);
    return n;
}

static void touch_flag(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    if (fd >= 0) close(fd);
}

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static long g_t0;                                       /* start of this launch */
static int over_budget(void) { return now_ms() - g_t0 > LOAD_BUDGET_MS; }

static int write_all(int fd, const uint8_t *p, size_t n) {
    while (n) { ssize_t w = write(fd, p, n); if (w < 0) { if (errno == EINTR) continue; return -1; } p += w; n -= (size_t)w; }
    return 0;
}

/* Open + hash `path` through ONE descriptor, copying into a private unlinked file when possible.
 * Returns 0 and leaves *src (the verified source fd, position irrelevant) and *cpy (read-only fd of the private
 * copy, or -1 if it could not be made) on success. */
static int load_verified(const char *path, const char *sha, unsigned long size, int *cpy) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC), wfd = -1, rfd = -1;
    struct stat st; sha_t s; char hex[65], tmpl[256]; uint8_t buf[8192], hdr[20]; size_t got = 0, hn = 0;
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || (unsigned long)st.st_size != size) { close(fd); return -1; }
    snprintf(tmpl, sizeof tmpl, "%s/.diskos-run-XXXXXX", PRIVATE_DIR);
    wfd = mkstemp(tmpl);
    if (wfd >= 0) {
        fchmod(wfd, 0700);
        rfd = open(tmpl, O_RDONLY | O_CLOEXEC);          /* second, read-only handle: writing fd must be closed before exec */
        unlink(tmpl);                                    /* private from here on: nobody can reach it by name */
        if (rfd < 0) { close(wfd); wfd = -1; }
    }
    sha_init(&s);
    for (;;) {
        ssize_t r;
        if (over_budget()) goto bad;                     /* hard cap on the hashing work */
        r = read(fd, buf, sizeof buf);
        if (r < 0) { if (errno == EINTR) continue; goto bad; }
        if (r == 0) break;
        got += (size_t)r; if (got > size) goto bad;
        for (ssize_t i = 0; i < r && hn < sizeof hdr; i++) hdr[hn++] = buf[i];
        sha_update(&s, buf, (size_t)r);
        if (wfd >= 0 && write_all(wfd, buf, (size_t)r) != 0) { close(wfd); close(rfd); wfd = rfd = -1; }   /* copy failed: keep verifying */
    }
    if (got != size) goto bad;
    sha_hex(&s, hex);
    if (strcmp(hex, sha) != 0) goto bad;
    if (hn < 20 || memcmp(hdr, "\x7f" "ELF", 4) != 0 || hdr[4] != ELF_CLASS_WANT || hdr[5] != 1 || hdr[18] != (ELF_MACHINE_WANT & 0xff) || hdr[19] != (ELF_MACHINE_WANT >> 8)) goto bad;
    if (wfd >= 0) { close(wfd); wfd = -1; }
    close(fd);                                          /* the source descriptor is never executed */
    *cpy = rfd;
    return 0;
bad:
    if (wfd >= 0) close(wfd);
    if (rfd >= 0) close(rfd);
    close(fd);
    return -1;
}

#ifdef TEST_EXEC_FAIL_FIRST                            /* host tests only: make the first N exec attempts fail */
static int g_test_fail = TEST_EXEC_FAIL_FIRST;
#endif
static void do_fexec(int fd, char **argv) {
#ifdef TEST_EXEC_FAIL_FIRST
    if (g_test_fail > 0) { g_test_fail--; return; }
#endif
    fexecve(fd, argv, environ);          /* returns only on failure; the C library falls back to /proc/self/fd */
}
static char *g_argv0;                                   /* the process's own (writable) argv[0] string */

/* exec `fd`; on SUCCESS the launch records for `m` are written by an orphaned watcher (see the header). Returns
 * only if the exec failed. */
/* True while the hook's supervisor has not revoked this launch. */
static int token_valid(void) {
    return access(TOKEN_PATH, F_OK) == 0;
}
static void revoked_exit(void) {
    logmsg("launch token revoked; not starting anything", "");
    _exit(3);
}

static void exec_proof(int fd, char **argv, const struct meta *m) {
    int pp[2]; pid_t parent = getpid(), c;
    if (!token_valid()) revoked_exit();
    if (!m || pipe2(pp, O_CLOEXEC) != 0) { do_fexec(fd, argv); return; }
    c = fork();
    if (c < 0) { close(pp[0]); close(pp[1]); do_fexec(fd, argv); return; }      /* no watcher: run, leave no proof */
    if (c == 0) {                                       /* intermediate child: fork the watcher and vanish */
#ifdef TEST_WATCHER_FORK_FAIL
        _exit(0);
#else
        if (fork() != 0) _exit(0);                      /* fork failure (-1) also lands here: no watcher, no proof */
#endif
        close(pp[1]);                                   /* only the launcher's copy may keep the pipe open */
        if (g_argv0) strncpy(g_argv0, "dlw", 4);        /* not a second "mq_ui" for pgrep */
        {   char b; ssize_t r;
            do r = read(pp[0], &b, 1); while (r < 0 && errno == EINTR);
            if (r == 0) {                               /* pipe closed: exec succeeded, or the launcher died */
                char path[64], st[256]; ssize_t n; int fd2, alive = 0;
                snprintf(path, sizeof path, "/proc/%d/stat", (int)parent);
                fd2 = open(path, O_RDONLY | O_CLOEXEC);
                if (fd2 >= 0) {
                    n = read(fd2, st, sizeof st - 1); close(fd2);
                    if (n > 0) { char *rp; st[n] = 0; rp = strrchr(st, ')'); alive = rp && rp[1] == ' ' && rp[2] != 'Z' && rp[2] != 'X'; }
                }
                if (alive) write_records(m);
            }
        }
        _exit(0);
    }
    close(pp[0]);
    waitpid(c, NULL, 0);
    if (!token_valid()) revoked_exit();                 /* the last look before the exec itself */
    signal(SIGPIPE, SIG_DFL);                           /* never leak an ignored SIGPIPE into the UI */
    do_fexec(fd, argv);
    signal(SIGPIPE, SIG_IGN);                           /* exec failed: a dead watcher must not kill us on the write */
    if (write(pp[1], "F", 1) < 0) { /* watcher gone or pipe broken: nothing to tell it */ }
    close(pp[1]);
    signal(SIGPIPE, SIG_DFL);
    usleep(20000);                                      /* let the watcher read the failure byte before we go on */
}

static void run_candidate(const char *path, const struct meta *m, char **argv, int read_only) {
    int cpy = -1;
    if (over_budget()) { logmsg("hash budget spent, skipping:", path); return; }
    if (load_verified(path, m->sha, m->size, &cpy) != 0) { logmsg("REFUSED (missing/size/sha/ELF/budget):", path); return; }
    if (cpy >= 0) { exec_proof(cpy, argv, m); logmsg("exec of private copy failed:", path); close(cpy); }
    if (read_only) { if (!token_valid()) revoked_exit(); logmsg("exec by path (verified, read-only):", path); execv(path, argv); }
    else logmsg("no usable private copy; writable source skipped:", path);
}

int main(int argc, char **argv) {
    struct run run, man;
    char *av[64]; int i, n = 0, demote = 0;
    if (argc < 2 || argc > 60) return 2;
    /* FIRST, before touching any file: be a process named mq_ui, so the stock watchdog's pgrep -x sees a UI. Rewrite
     * argv[0] in place (works even if the re-exec below fails), then try the clean re-exec. */
    {   size_t old_len = strlen(argv[0]);
        int need_reexec = strcmp(argv[0], "mq_ui") != 0;
        if (old_len >= 5) {
            memcpy(argv[0], "mq_ui", 6);
            memset(argv[0] + 6, 0, old_len + 1 - 6 > 0 ? old_len + 1 - 6 : 0);
        }
        prctl(PR_SET_NAME, (unsigned long)"mq_ui", 0, 0, 0);
#ifndef TEST_NO_REEXEC
        if (need_reexec) {
            char *na[64];
            na[0] = (char *)"mq_ui";
            for (i = 1; i < argc; i++) na[i] = argv[i];
            na[argc] = NULL;
            execv("/proc/self/exe", na);                /* if this fails we carry on under the normalized name */
        }
#else
        (void)need_reexec;
#endif
    }
    g_argv0 = argv[0];
    g_t0 = now_ms();
#ifdef TEST_STALL_SECS
    sleep(TEST_STALL_SECS);
#endif
    av[n++] = (char *)"mq_ui";
    for (i = 2; i < argc; i++) av[n++] = argv[i];
    av[n] = NULL;
    clear_records();                                    /* no fallback may inherit an earlier build's proof */
    read_run(RUN_PATH, &run);
    read_run(MANIFEST_PATH, &man);
    if (run.ok && !strcmp(run.cur.build, "trial") && bump_starts() > LAUNCH_MAX_TRIAL_STARTS) {
        demote = 1;
        logmsg("trial started too often this boot; not starting it again", argv[1]);
        touch_flag(CRASH_FLAG);
    }
    if (run.ok && !demote) run_candidate(argv[1], &run.cur, av, 0);
    else if (!run.ok) logmsg("no usable run file; using the flashed build", "");
    if (run.ok && run.has_good && !strcmp(run.cur.build, "trial")) run_candidate(GOOD_PATH, &run.good, av, 0);
    if (run.ok && run.has_prev) run_candidate(PREV_PATH, &run.prev, av, 0);
    if (man.ok) {
        struct meta f = man.cur;
        strcpy(f.build, "flashed"); strcpy(f.epoch, "0"); strcpy(f.version, "flashed");
        run_candidate(FLASHED_PATH, &f, av, 1);
    }
    logmsg("falling back to the stock UI", STOCK_PATH);
    if (!token_valid()) revoked_exit();
    execv(STOCK_PATH, av);
    return 1;
}
