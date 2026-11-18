/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_NAMES_H
#define DISKOS_NAMES_H
#define _GNU_SOURCE
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef NAMES_DIR
#define NAMES_DIR "/tmp/diskos-names"
#endif
#ifndef NAMES_SAVED
#define NAMES_SAVED "/usr/data/diskos-network-name"
#endif
#ifndef NAMES_BUSYBOX
#define NAMES_BUSYBOX "/bin/busybox"
#endif
#ifndef NAMES_UID
#define NAMES_UID 0
#endif
#ifndef NAMES_BOOT_ID
#define NAMES_BOOT_ID "/proc/sys/kernel/random/boot_id"
#endif
#define NAMES_ACTIVE NAMES_DIR "/active"
#define NAMES_ENV "DISKOS_NETWORK_NAMES"
/* Empty means stock. ASCII bytes only; preserve case. */
static int valid_name(const char *s) {
    size_t n = strlen(s);
    if (n > 32) return 0;
    for (size_t i = 0; i < n; ++i)
        if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') ||
              (s[i] >= '0' && s[i] <= '9') || (s[i] == '-' && i && i + 1 < n))) return 0;
    return 1;
}
/* Bound both size and reads; open cannot block on a FIFO or follow a symlink. */
static int read_record(const char *path, char *out, size_t cap, int trusted) {
    struct stat st;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    int ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1 &&
        st.st_size >= 0 && (size_t)st.st_size < cap &&
        (!trusted || (st.st_uid == NAMES_UID && (st.st_mode & 07777) == 0444));
    size_t got = 0;
    while (ok && got < cap) {
        ssize_t n = read(fd, out + got, cap - got);
        if (n < 0) { ok = 0; break; }
        if (!n) break;
        got += (size_t)n;
    }
    if (close(fd)) ok = 0;
    if (!ok || got >= cap || got != (size_t)st.st_size || memchr(out, 0, got)) return -1;
    out[got] = 0;
    return (int)got;
}
static int boot_id(char out[37]) {
    /* procfs reports zero st_size; use one bounded read instead of read_record. */
    char b[38];
    int fd = open(NAMES_BOOT_ID, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, b, sizeof b);
    int rc = close(fd);
    if (rc || n != 37 || b[36] != '\n') return 0;
    for (int i = 0; i < 36; ++i)
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (b[i] != '-') return 0; }
        else if (!((b[i] >= '0' && b[i] <= '9') || (b[i] >= 'a' && b[i] <= 'f'))) return 0;
    memcpy(out, b, 36); out[36] = 0; return 1;
}
static int active_name(char name[33]) {
    char b[96], id[37]; struct stat st;
    if (lstat(NAMES_DIR, &st) || !S_ISDIR(st.st_mode) || st.st_uid != NAMES_UID ||
        (st.st_mode & 07777) != 0755 || !boot_id(id)) return 0;
    int n = read_record(NAMES_ACTIVE, b, sizeof b, 1);
    if (n < 51 || strncmp(b, "v1\nboot=", 8) || memcmp(b + 8, id, 36) ||
        strncmp(b + 44, "\nname=", 6) || b[n-1] != '\n' || n > 83) return 0;
    b[n-1] = 0;
    if (!valid_name(b + 50)) return 0;
    strcpy(name, b + 50); return 1;
}
#endif
