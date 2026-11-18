/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "names.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#ifndef NAMES_DEADLINE_MS
#define NAMES_DEADLINE_MS 750
#endif
static long long now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return -1;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static int apply_hostname(const char *name) {
#ifdef NAMES_TEST_HOSTNAME
    int fd = open(NAMES_TEST_HOSTNAME, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    int ok = write(fd, name, strlen(name)) == (ssize_t)strlen(name);
    if (close(fd)) ok = 0;
    return ok ? 0 : -1;
#else
    return sethostname(name, strlen(name));
#endif
}
static void worker(int fd, pid_t parent) {
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) _exit(1);
    /* No external descriptors keep a shell caller waiting when NAND stalls. */
    close(0); close(1); close(2);
    char b[42], name[33] = "";
#ifdef NAMES_TEST_DELAY_MS
    usleep(NAMES_TEST_DELAY_MS * 1000);
#endif
    int n = read_record(NAMES_SAVED, b, sizeof b, 0);
    if (n >= 9 && n <= 41 && !strncmp(b, "v1\nname=", 8) && b[n-1] == '\n') {
        b[n-1] = 0;
        if (valid_name(b+8)) strcpy(name, b+8);
    }
    /* Worker has no publication path and never calls sethostname. */
    if (write(fd, name, sizeof name) != sizeof name) _exit(1);
    _exit(0);
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--active")) {
        char name[33]; return active_name(name) ? 0 : 1;
    }
    if (argc != 1) return 2;
    /* mkdir is a once-per-boot latch, including failures. Never retry on userdata. */
    if (mkdir(NAMES_DIR, 0755)) return 1;
    if (chmod(NAMES_DIR, 0755)) return 1;
    long long start = now_ms();
    if (start < 0) return 1;
    struct sigaction sa = { .sa_handler = SIG_DFL };
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGCHLD, &sa, NULL)) return 1;
    int p[2];
    if (pipe2(p, O_CLOEXEC)) return 1;
    pid_t parent = getpid(), pid = fork();
    if (pid < 0) return 1;
    if (!pid) { close(p[0]); worker(p[1], parent); }
    close(p[1]);
    char name[33]; size_t got = 0; int done = 0;
    while (!done && got < sizeof name) {
        long long now = now_ms(), left = start + NAMES_DEADLINE_MS - now;
        if (now < 0 || left <= 0) break;
        struct pollfd pf = { .fd = p[0], .events = POLLIN };
        int rc = poll(&pf, 1, (int)left);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) break;
        ssize_t n = read(p[0], name + got, sizeof name - got);
        if (n <= 0) { done = 1; break; }
        got += (size_t)n;
    }
    long long finished = now_ms();
    /* Only the parent can commit. Closing the private result channel revokes the
     * worker before kill; no wait for a task stuck in uninterruptible NAND I/O. */
    close(p[0]);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, WNOHANG);
    if (got != sizeof name || finished < 0 || finished >= start + NAMES_DEADLINE_MS ||
        !memchr(name, 0, sizeof name) || !valid_name(name)) return 1;
    char id[37], record[96];
    if (!boot_id(id)) return 1;
    int n = snprintf(record, sizeof record, "v1\nboot=%s\nname=%s\n", id, name);
    int fd = open(NAMES_DIR "/pending", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0444);
    if (fd < 0) return 1;
    int ok = write(fd, record, (size_t)n) == n && fchmod(fd, 0444) == 0;
    if (close(fd)) ok = 0;
    if (!ok) return 1;
    if (name[0] && apply_hostname(name)) return 1;
    if (rename(NAMES_DIR "/pending", NAMES_ACTIVE)) {
        if (name[0]) apply_hostname("ingenic");
        return 1;
    }
    return 0;
}
