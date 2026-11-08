/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Over-the-air diskOS UI updates ("Tier A"): find a release bundle, download it to a staging dir, verify it with the
 * read-only rootfs verifier, and leave it for the boot side to install. This file never installs anything and never
 * touches /usr/data/mq_ui. The paths, asset names and markers are the contract in update-system/OTA_CONTRACT.md. */
#ifndef DISKOS_OTA_H
#define DISKOS_OTA_H
#include <stddef.h>
#include <stdatomic.h>

typedef struct {
    const char *updates;      /* /usr/data/updates (incoming/, pending/, accepted, rollback live here) */
    const char *rootpub;      /* /etc/diskos-ota/root.pub.pem   (read-only rootfs) */
    const char *epochs;       /* /etc/diskos-ota/epochs         (MIN_KEY_EPOCH / MIN_UI_EPOCH, read-only rootfs) */
    const char *verifier;     /* /etc/diskos-ota/diskos-verify.sh (read-only rootfs; same file boot runs) */
    const char *latest_url;   /* releases/latest API */
    const char *dl_base;      /* https://github.com/b0hemia/diskos/releases/download (then /<tag>/<asset>) */
    const char *data_root;    /* /usr/data (free-space check) */
    const char *operstate;    /* /sys/class/net/wlan0/operstate */
    const char *run_file;     /* /tmp/.diskos_run (S97: SHA256= SIZE= BUILD=trial|good|flashed EPOCH= VERSION=) */
    const char *launched_file; /* /tmp/.diskos_launched (launcher: what it ACTUALLY started: SHA256= BUILD= ...) */
    const char *pcm_glob;     /* glob of the ALSA playback substream status files ("state: RUNNING" = audio is flowing) */
    const char *self_exe;     /* /proc/self/exe: hashed and compared with the run file before a health mark is written */
} ota_paths_t;
extern const ota_paths_t OTA_DEV;

enum { OTA_OK = 0, OTA_E_CANCEL, OTA_E_NET, OTA_E_SPACE, OTA_E_UNSUPPORTED, OTA_E_SIG, OTA_E_OLDER, OTA_E_BAD, OTA_E_IO, OTA_E_OFF, OTA_E_REJECTED };

#define OTA_FETCH_DEADLINE_S 20              /* whole release lookup, CLOCK_MONOTONIC */
#define OTA_NFILES 6
#define OTA_MAX_UI (32LL * 1024 * 1024)      /* same cap the verifier applies to mq_ui */
#ifndef OTA_STALL_S
#define OTA_STALL_S 40                       /* no download progress for this long = network failure */
#endif

typedef struct {
    _Atomic int cancel;                      /* 0 open, 1 cancellation accepted, 2 commit reserved */
    _Atomic int phase;                       /* 0 idle, 1 downloading, 2 verifying, 3 committing (cancel no longer honoured) */
    _Atomic long done, total;                 /* bytes (32-bit: MIPS32 has no 64-bit atomics) of the current bundle (total = 0 until the manifest is known) */
    long long need_free;                     /* OTA_E_SPACE: bytes that would have been needed (written before return) */
    char version[40];                        /* OTA_OK: verified version */
} ota_prog_t;

static inline int ota_request_cancel(ota_prog_t *pg){
    int expected = 0;
    return atomic_compare_exchange_strong(&pg->cancel, &expected, 1);
}

static inline int ota_begin_commit(ota_prog_t *pg){
    int expected = 0;
    if(!atomic_compare_exchange_strong(&pg->cancel, &expected, 2)) return 0;
    atomic_store(&pg->phase, 3);
    return 1;
}

int  ota_asset_name(int i, const char **asset, const char **local);   /* the six bundle files; 0 ok */
int  ota_ver_parse(const char *s, int allow_v, int v[3]);              /* strict [v]N.N.N, whole string; 0 ok */
int  ota_ver_cmp(const char *a, int a_v, const char *b, int b_v);     /* -1, 0, 1, or -2 if either is not strict */
int  ota_assets_ok(const char *json);                                 /* 1 = all six assets are listed in the release JSON */
int  ota_fetch_release(const ota_paths_t *p, char *tag, size_t n, int *has_bundle);   /* blocking; 0 ok */
/* Optional caller-owned atomic flag, alive until return; nonzero cancels. NULL is allowed.
 * Failure (including deadline/cancel/cap) returns -1 and clears has_bundle. */
int  ota_fetch_release_cancel(const ota_paths_t *p, char *tag, size_t n, int *has_bundle, const _Atomic int *cancel);
int  ota_wifi_down(const ota_paths_t *p);                             /* 1 = the Wi-Fi interface reports "down" */
int  ota_supported(const ota_paths_t *p);                             /* 1 = RO verifier + root key + epochs are present */
int  ota_stage(const ota_paths_t *p, const char *running, const char *tag, ota_prog_t *pg);   /* blocking; OTA_*; caller checks ota_allowed() on the UI thread */
int  ota_allowed(void);                                               /* 1 = Settings > Device > Allow diskOS Updates is on (cfg ota_allow, default on) */
int  ota_discard_pending(const ota_paths_t *p);                       /* 1 = <updates>/pending (and pending.old) is gone; touches nothing else */
int  ota_pending(const ota_paths_t *p, char *ver, size_t n);          /* 1 = a verified bundle waits for boot */
int  ota_trial_version(const ota_paths_t *p, char *ver, size_t n);    /* 1 = this boot is a trial (run + launched files agree); cheap, no hashing */
int  ota_trial_maybe_me(const ota_paths_t *p);                        /* 0 = the running exe is certainly not the trial (size differs); cheap */
/* 1 = boot recognizes healthy/revert for this running trial (ota.c boot_marker_matches).
 * Single intentional divergence from boot: at most 64 KiB total bytes per marker,
 * including NULs; cap reached before LF/EOF means no valid decision (prompt may
 * re-ask). Bounds UI-thread scanning of corrupt/hostile files diskOS never writes. */
int  ota_trial_decided(const ota_paths_t *p);
int  ota_pcm_running(const ota_paths_t *p);                           /* 1 = some playback substream is RUNNING */
int  ota_health_write(const ota_paths_t *p);                          /* 1 = wrote <updates>/healthy for the running trial build */
int  ota_revert_write(const ota_paths_t *p);                          /* 1 = wrote <updates>/revert (user chose "Go back") for the running trial */
int  ota_rollback_take(const ota_paths_t *p);                         /* 1 = boot reported a rolled-back update (marker removed) */
const char *ota_err_text(int code);                                   /* exact user text (contract section 6); NULL for OK */
#endif
