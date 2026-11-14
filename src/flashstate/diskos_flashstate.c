/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef RO_DIR
#define RO_DIR "/etc"
#endif
#ifndef DATA_DIR
#define DATA_DIR "/usr/data"
#endif
#define RECORD "diskos-flash-id"
#define TEMP ".diskos-flash-id.tmp"
/* check: match, missing writable, different valid, malformed writable,
 * storage/path error, invalid/missing RO. complete: 0 only after read-back.
 * Usage errors return 64. No record contents are printed or interpreted. */
enum { MATCH, MISSING, DIFFERENT, MALFORMED, STORAGE, BAD_RO };

static int close_result(int fd, int result)
{
    return close(fd) < 0 ? STORAGE : result;
}

static int read_id(int dir, const char *name, char id[34])
{
    struct stat st;
    size_t used = 0;
    int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return errno == ENOENT ? MISSING : STORAGE;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode))
        return close_result(fd, STORAGE);
    while (used < 34) {
        ssize_t n = read(fd, id + used, 34 - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return close_result(fd, STORAGE);
        if (!n) break;
        used += (size_t)n;
    }
    int result = used == 33 && id[32] == '\n' ? MATCH : MALFORMED;
    for (size_t i = 0; i < 32 && result == MATCH; ++i)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f')))
            result = MALFORMED;
    return close_result(fd, result);
}

/* Only remove our ordinary temporary record; never follow unexpected types. */
static int clear_temp(int dir)
{
    struct stat st;
    if (fstatat(dir, TEMP, &st, AT_SYMLINK_NOFOLLOW) < 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISREG(st.st_mode)) return -1;
    return unlinkat(dir, TEMP, 0);
}

static int complete(int dir, const char id[34])
{
    char verify[34];
    size_t used = 0;
    int fd, result = STORAGE;
    if (clear_temp(dir) < 0) return STORAGE;
    fd = openat(dir, TEMP, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return STORAGE;
    while (used < 33) {
        ssize_t n = write(fd, id + used, 33 - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto out;
        used += (size_t)n;
    }
    if (fsync(fd) < 0) goto out;
    if (close(fd) < 0) { fd = -1; goto out; }
    fd = -1;
    if (renameat(dir, TEMP, dir, RECORD) < 0 || fsync(dir) < 0) goto out;
    if (read_id(dir, RECORD, verify) == MATCH && memcmp(id, verify, 33) == 0)
        result = MATCH;
out:
    if (fd >= 0 && close(fd) < 0) result = STORAGE;
    if (result != MATCH && unlinkat(dir, TEMP, 0) < 0 && errno != ENOENT)
        return STORAGE;
    return result;
}

int main(int argc, char **argv)
{
    char ro[34], data[34];
    int dir, result, writing;
    if (argc != 2 || (strcmp(argv[1], "check") && strcmp(argv[1], "complete"))) {
        fputs("usage: diskos-flashstate check|complete\n", stderr);
        return 64;
    }
    writing = !strcmp(argv[1], "complete");
    dir = open(RO_DIR, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) return STORAGE;
    result = read_id(dir, RECORD, ro);
    if (result == MISSING || result == MALFORMED) result = BAD_RO;
    result = close_result(dir, result);
    if (result != MATCH) return result;
    dir = open(DATA_DIR, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) return STORAGE; /* Missing data root is not a missing record. */
    result = read_id(dir, RECORD, data);
    if (result == MATCH && memcmp(ro, data, 33)) result = DIFFERENT;
    if (writing && result != STORAGE) result = complete(dir, ro);
    return close_result(dir, result);
}
