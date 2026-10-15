#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Exercise the actual launcher helpers with temporary host paths, never /usr/data.

Extracting the helpers keeps this regression test linked to main.c without linking
LVGL or device startup. Cases cover ordinary rm, altered wrappers and publication.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class GuardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="diskos-rmguard-tests-")
        cls.root = Path(cls.tmp.name)
        app = Path(__file__).resolve().parents[1]
        source = (app / "main.c").read_text()
        start = source.index("static const char RMGUARD_SCRIPT[]")
        end = source.index("static int player_config_absent_without_card(", start)
        helpers = source[start:end].replace('mkdir("/usr/data/diskos",', 'mkdir(test_parent,')
        harness = r'''
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
static char test_root[256], test_parent[256], test_fallback[256];
#define RMGUARD_SYSTEM "/nonexistent-diskos-system-test"
#define RMGUARD_ROOTFS test_root
#define RMGUARD_FALLBACK test_fallback
''' + helpers + r'''
static void put(const char *dir, const char *data, int mode){
    char p[300]; snprintf(p, sizeof p, "%s/rm", dir);
    int fd = open(p, O_CREAT|O_TRUNC|O_WRONLY, 0600);
    if(fd < 0 || write(fd, data, strlen(data)) != (ssize_t)strlen(data)) abort();
    if(fchmod(fd, mode) != 0 || close(fd) != 0) abort();
}
int main(int argc, char **argv){
    if(argc != 3) return 2;
    snprintf(test_root, sizeof test_root, "%s/ro", argv[1]);
    snprintf(test_parent, sizeof test_parent, "%s/data", argv[1]);
    snprintf(test_fallback, sizeof test_fallback, "%s/data/bin", argv[1]);
    mkdir(test_root, 0755);
    setenv("PATH", "/bin:/usr/bin", 1);
    if(!strcmp(argv[2], "root")) put(test_root, RMGUARD_SCRIPT, 0755);
    if(!strcmp(argv[2], "header")) put(test_root, "#!/bin/sh\n# diskOS SD guard\nexec /bin/rm \"$@\"\n", 0755);
    if(!strcmp(argv[2], "changed")){
        char bad[sizeof RMGUARD_SCRIPT]; memcpy(bad, RMGUARD_SCRIPT, sizeof bad);
        bad[sizeof bad - 3] ^= 1; put(test_root, bad, 0755);
    }
    if(!strcmp(argv[2], "nonexec")) put(test_root, RMGUARD_SCRIPT, 0644);
    if(!strcmp(argv[2], "symlink")){
        char p[300]; snprintf(p, sizeof p, "%s/rm", test_root); symlink("/bin/rm", p);
    }
    if(!strcmp(argv[2], "fifo")){
        char p[300]; snprintf(p, sizeof p, "%s/rm", test_root); mkfifo(p, 0755);
    }
    if(!strcmp(argv[2], "fallback_changed")){
        mkdir(test_parent, 0755); mkdir(test_fallback, 0755);
        put(test_fallback, "#!/bin/sh\nexec /bin/rm \"$@\"\n", 0755);
    }
    if(!strcmp(argv[2], "unwritable")){
        int fd = open(test_parent, O_CREAT|O_WRONLY, 0600); if(fd < 0) abort(); close(fd);
    }
    int result = rmguard_ensure();
    char *first = strdup(getenv("PATH"));
    int again = rmguard_ensure();
    printf("%d %d %d\n%s\n", result, again, !strcmp(first, getenv("PATH")), getenv("PATH"));
    free(first);
    return 0;
}
'''
        c = cls.root / "test.c"
        c.write_text(harness)
        cls.exe = cls.root / "test"
        subprocess.run([os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-g", str(c), "-o", str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_case(self, case, expected):
        with tempfile.TemporaryDirectory(dir=self.root) as base:
            lines = subprocess.check_output([str(self.exe), base, case], text=True, timeout=5).splitlines()
            self.assertEqual(lines[0], "0 0 1" if expected is None else "1 1 1")
            if expected is None:
                self.assertEqual(lines[1], "/bin:/usr/bin")
            else:
                self.assertEqual(lines[1], f"{base}/{expected}:/bin:/usr/bin")
                guard = Path(base) / expected / "rm"
                payload = Path(__file__).resolve().parents[2] / "payload/diskos-rmguard"
                self.assertEqual(guard.read_bytes(), payload.read_bytes())

    def test_real_rm_does_not_count_as_guard(self):
        self.run_case("ordinary", "data/bin")

    def test_valid_rootfs_guard_is_preferred_and_idempotent(self):
        self.run_case("root", "ro")

    def test_invalid_rootfs_guards_are_rejected(self):
        for case in ("header", "changed", "nonexec", "symlink", "fifo"):
            with self.subTest(case=case):
                self.run_case(case, "data/bin")

    def test_existing_bad_fallback_is_replaced(self):
        self.run_case("fallback_changed", "data/bin")

    def test_no_guard_available_reports_failure(self):
        self.run_case("unwritable", None)


if __name__ == "__main__":
    unittest.main()
