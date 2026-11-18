# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Host checks of the shipped identity helper source; no device access."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(os.environ.get('FLASHSTATE_ROOT', Path(__file__).resolve().parents[1]))
ID = 'diskos-flash-id'
OLD = b'0' * 32 + b'\n'
NEW = b'0123456789abcdef' * 2 + b'\n'
WRAPPERS = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
int __real_close(int);
int __real_unlinkat(int, const char *, int);
int __real_fstatat(int, const char *, struct stat *, int);
ssize_t __real_write(int, const void *, size_t);
int __real_fsync(int);
int __real_renameat(int, const char *, int, const char *);
ssize_t __real_read(int, void *, size_t);
static int fault(const char *s) {
    const char *v = getenv("FAULT");
    return v && !strcmp(v, s);
}
static int published;
static int same_file(int fd, const char *path) {
    struct stat a, b;
    return !fstat(fd, &a) && !stat(path, &b) &&
           a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}
int __wrap_close(int fd) {
    int temporary = same_file(fd, DATA_DIR "/.diskos-flash-id.tmp");
    int fail = (fault("ro-close") && same_file(fd, RO_DIR "/diskos-flash-id")) ||
               (fault("data-close") && same_file(fd, DATA_DIR "/diskos-flash-id")) ||
               (fault("temp-close") && same_file(fd, DATA_DIR "/.diskos-flash-id.tmp"));
    int result = __real_close(fd);
    if (!result && temporary && fault("after-close")) _exit(99);
    if (fail) { errno = EIO; return -1; }
    return result;
}
int __wrap_unlinkat(int dir, const char *name, int flags) {
    if (fault("temp-unlink")) { errno = EIO; return -1; }
    return __real_unlinkat(dir, name, flags);
}
int __wrap_fstatat(int dir, const char *name, struct stat *st, int flags) {
    if (fault("temp-stat")) { errno = EIO; return -1; }
    return __real_fstatat(dir, name, st, flags);
}
ssize_t __wrap_write(int fd, const void *p, size_t n) {
    static int calls;
    if (fault("write-error")) { errno = ENOSPC; return -1; }
    if (fault("write-zero")) return 0;
    if (fault("short-write") || fault("mid-write")) {
        if (!calls++) { errno = EINTR; return -1; }
        if (fault("mid-write") && calls > 2) _exit(99);
        if (n > 7) n = 7;
    }
    return __real_write(fd, p, n);
}
int __wrap_fsync(int fd) {
    struct stat st;
    if (fstat(fd, &st)) return -1;
    if (!S_ISDIR(st.st_mode) && fault("crash")) _exit(99);
    if (fault(S_ISDIR(st.st_mode) ? "dirsync" : "filesync")) {
        errno = EIO; return -1;
    }
    int result = __real_fsync(fd);
    if (!result && !S_ISDIR(st.st_mode) && fault("after-filesync")) _exit(99);
    if (!result && S_ISDIR(st.st_mode) && fault("after-dirsync")) _exit(99);
    return result;
}
int __wrap_renameat(int a, const char *b, int c, const char *d) {
    if (fault("rename")) { errno = EIO; return -1; }
    int result = __real_renameat(a, b, c, d);
    if (!result) published = 1;
    if (!result && fault("after-rename")) _exit(99);
    return result;
}
ssize_t __wrap_read(int fd, void *p, size_t n) {
    static int calls;
    if (fault("read") ||
        (fault("ro-read") && same_file(fd, RO_DIR "/diskos-flash-id")) ||
        (fault("data-read") && same_file(fd, DATA_DIR "/diskos-flash-id")) ||
        (fault("readback") && published)) { errno = EIO; return -1; }
    if (fault("short-read")) {
        if (!calls++) { errno = EINTR; return -1; }
        if (n > 7) n = 7;
    }
    return __real_read(fd, p, n);
}
'''
WRAP_FLAGS = ('-Wl,--wrap=fsync,--wrap=renameat,--wrap=read,--wrap=write,--wrap=close,'
              '--wrap=unlinkat,--wrap=fstatat')


class FlashstateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='flashstate-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.root = Path(cls.temp.name)
        cls.ro, cls.data = cls.root / 'ro', cls.root / 'data'
        cls.ro.mkdir(); cls.data.mkdir()
        wrapper = cls.root / 'fault.c'
        wrapper.write_text(WRAPPERS, encoding='ascii')
        cls.exe = cls.root / 'flashstate'
        subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                        '-DRO_DIR="' + str(cls.ro) + '"',
                        '-DDATA_DIR="' + str(cls.data) + '"',
                        str(ROOT / 'src/flashstate/diskos_flashstate.c'), str(wrapper),
                        WRAP_FLAGS,
                        '-o', str(cls.exe)], check=True, capture_output=True)

    def setUp(self):
        for directory in (self.ro, self.data):
            for p in directory.iterdir():
                if p.is_dir() and not p.is_symlink():
                    p.rmdir()
                else:
                    p.unlink()
        (self.ro / ID).write_bytes(NEW)

    def run_helper(self, code, command='check', fault=''):
        proc = subprocess.run([str(self.exe), command],
                              env={**os.environ, 'FAULT': fault}, capture_output=True)
        self.assertEqual(proc.returncode, code, proc.stderr)
        self.assertEqual(proc.stdout, b'')

    def test_read_outcomes_and_exact_format(self):
        self.run_helper(1)
        for raw, code in ((NEW, 0), (OLD, 2), (b'', 3), (NEW[:-1], 3),
                          (NEW + b'x', 3), (NEW[:-1] + b'\r\n', 3),
                          (b'A' * 32 + b'\n', 3), (b'g' * 32 + b'\n', 3),
                          (b'\x00' * 32 + b'\n', 3)):
            with self.subTest(raw=raw):
                (self.data / ID).write_bytes(raw)
                self.run_helper(code)
        (self.data / ID).unlink()
        for raw in (None, b'bad', NEW + b'\n'):
            if raw is None:
                (self.ro / ID).unlink()
            else:
                (self.ro / ID).write_bytes(raw)
            self.run_helper(5)
            self.run_helper(5, 'complete')
            self.assertFalse((self.data / ID).exists())
        self.run_helper(64, 'unknown')

    def test_path_errors_are_not_absence_or_corruption(self):
        sentinel = self.root / 'sentinel'
        sentinel.write_bytes(OLD)
        for directory in (self.ro, self.data):
            p = directory / ID
            if p.exists(): p.unlink()
            p.mkdir()
            self.run_helper(4); self.run_helper(4, 'complete')
            p.rmdir(); p.symlink_to(sentinel)
            self.run_helper(4); self.run_helper(4, 'complete')
            p.unlink(); os.mkfifo(p)
            self.run_helper(4); p.unlink()
            p.write_bytes(NEW)
        self.run_helper(4, fault='read')
        self.data.rename(self.root / 'saved')
        try:
            self.run_helper(4)  # ENOENT for data root is not missing record.
            self.data.symlink_to(self.root / 'saved')
            self.run_helper(4, 'complete')
            self.data.unlink()
        finally:
            (self.root / 'saved').rename(self.data)
        self.assertEqual(sentinel.read_bytes(), OLD)

    def test_unreadable_writable_record_preserves_history(self):
        if os.geteuid() == 0:
            self.skipTest('permission denial requires an unprivileged host')
        p = self.data / ID
        p.write_bytes(OLD)
        p.chmod(0)
        try:
            self.run_helper(4)
            self.run_helper(4, 'complete')
        finally:
            p.chmod(0o600)
        self.assertEqual(p.read_bytes(), OLD)
        self.assertFalse((self.data / '.diskos-flash-id.tmp').exists())

    def test_completion_and_interruption(self):
        for before in (None, OLD, b'bad', NEW):
            p = self.data / ID
            if p.exists(): p.unlink()
            if before is not None: p.write_bytes(before)
            self.run_helper(0, 'complete')
            self.assertEqual(p.read_bytes(), NEW)
            self.assertEqual(p.stat().st_size, 33)
            self.run_helper(0)
        for fault in ('crash', 'filesync', 'rename', 'dirsync'):
            with self.subTest(fault=fault):
                p.write_bytes(OLD)
                self.run_helper(99 if fault == 'crash' else 4, 'complete', fault)
                self.assertEqual(p.read_bytes(), NEW if fault == 'dirsync' else OLD)
                self.run_helper(0, 'complete')  # Also recovers stale exclusive temp.
                self.assertEqual(p.read_bytes(), NEW)
                self.assertFalse((self.data / '.diskos-flash-id.tmp').exists())
        temp = self.data / '.diskos-flash-id.tmp'
        temp.symlink_to(self.ro / ID)
        self.run_helper(4, 'complete')
        self.assertEqual((self.ro / ID).read_bytes(), NEW)

    def test_partial_io_and_completion_cuts_recover(self):
        p = self.data / ID
        temp = self.data / '.diskos-flash-id.tmp'
        for fault in ('short-read', 'short-write', 'mid-write', 'temp-close',
                      'write-error', 'write-zero', 'after-filesync', 'after-close',
                      'after-rename', 'after-dirsync', 'readback'):
            with self.subTest(fault=fault):
                p.write_bytes(OLD)
                success = fault in ('short-read', 'short-write')
                cut = fault in ('mid-write', 'after-filesync', 'after-close',
                                'after-rename', 'after-dirsync')
                self.run_helper(0 if success else 99 if cut else 4, 'complete', fault)
                committed = success or fault in ('after-rename', 'after-dirsync', 'readback')
                self.assertEqual(p.read_bytes(), NEW if committed else OLD)
                if fault == 'mid-write':
                    self.assertEqual(temp.read_bytes(), NEW[:7])
                if fault in ('after-filesync', 'after-close'):
                    self.assertEqual(temp.read_bytes(), NEW)
                self.run_helper(0, 'complete')
                self.run_helper(0)
                self.assertEqual(p.read_bytes(), NEW)
                self.assertFalse(temp.exists())

    def test_stale_temp_lookup_and_removal_errors_are_retryable(self):
        p = self.data / ID
        temp = self.data / '.diskos-flash-id.tmp'
        for fault in ('temp-stat', 'temp-unlink'):
            with self.subTest(fault=fault):
                p.write_bytes(OLD)
                temp.write_bytes(NEW[:5])
                self.run_helper(4, 'complete', fault)
                self.assertEqual(p.read_bytes(), OLD)
                self.assertEqual(temp.read_bytes(), NEW[:5])
                self.run_helper(0, 'complete')
                self.assertEqual(p.read_bytes(), NEW)
                self.assertFalse(temp.exists())

    def test_identity_io_errors_never_authorize_completion(self):
        p = self.data / ID
        p.write_bytes(OLD)
        for fault in ('ro-read', 'data-read', 'ro-close', 'data-close'):
            with self.subTest(fault=fault):
                self.run_helper(4, fault=fault)
                self.run_helper(4, 'complete', fault)
                self.assertEqual(p.read_bytes(), OLD)
                self.assertFalse((self.data / '.diskos-flash-id.tmp').exists())
        self.run_helper(0, 'complete')
        self.assertEqual(p.read_bytes(), NEW)
        self.assertFalse((self.data / '.diskos-flash-id.tmp').exists())

    def test_stale_temporary_types_preserve_external_bytes(self):
        temp = self.data / '.diskos-flash-id.tmp'
        sentinel = self.root / 'external'
        sentinel.write_bytes(b'external\x00\xff')
        p = self.data / ID
        for kind in ('directory', 'fifo', 'symlink', 'regular'):
            with self.subTest(kind=kind):
                p.write_bytes(OLD)
                if kind == 'directory': temp.mkdir()
                elif kind == 'fifo': os.mkfifo(temp)
                elif kind == 'symlink': temp.symlink_to(sentinel)
                else: temp.write_bytes(NEW[:11])
                self.run_helper(0 if kind == 'regular' else 4, 'complete')
                self.assertEqual(p.read_bytes(), NEW if kind == 'regular' else OLD)
                self.assertEqual(sentinel.read_bytes(), b'external\x00\xff')
                if kind == 'directory': temp.rmdir()
                elif kind != 'regular': temp.unlink()
                self.run_helper(0, 'complete')
                self.assertFalse(temp.exists())


class PackagingTests(unittest.TestCase):
    def test_required_boot_and_package_inventory(self):
        import importlib.util
        from diskos_installer import imagebuild
        self.assertIn(('opt/diskos/bin/diskos-flashstate', 'diskos-flashstate'),
                      imagebuild._BOOT_DECISION_FILES)
        spec = importlib.util.spec_from_file_location('blob_inventory', ROOT / 'build/verify_git_blobs.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.assertIn('diskos-flashstate', module.PAYLOAD)
        self.assertIn('diskos-flashstate', (ROOT / 'build/stage-release.sh').read_text())
        self.assertTrue((ROOT / 'payload/diskos-flashstate').is_file())
