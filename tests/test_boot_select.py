# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""S96diskos_select: the one boot decision, and the promise that it resolves to stock on every failure.

Runs the REAL shipped script under the host /bin/sh AND under the V2.40 firmware's BusyBox ash via
qemu-mipsel-static (with the firmware's own mkdir/rm/mv/cat applets), and uses the REAL probe: a host build of
tools/diskos_bootprobe.c for the host shell, and the SHIPPED mipsel payload binary under qemu for ash. The pin
register is supplied through the probe's MEMDEV argument as a sparse file holding the value at the GPB
offset, so nothing in the decision path is stubbed except where a test deliberately substitutes the probe to
feed S96 malformed output.

The environment is hybrid, and these tests do not pretend otherwise: qemu wrappers are host scripts, commands
without a wrapper resolve through the host PATH, and -L is a loader prefix, not a filesystem boundary. The
BusyBox is checked against the full md5 of the known V2.40 build.

The probe's own supervision (deadline, kill, parent death, late results) is tested directly in
ui/tests/boot_probe_test.py. Not established here: physical GPIO timing, how long Vol-Up
must be held, /usr/data mount timing on a real boot, and uninterruptible NAND stalls.
"""
import errno
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

PAYLOAD = Path(__file__).resolve().parents[1] / 'payload'
S96 = PAYLOAD / 'S96diskos_select'
S97 = PAYLOAD / 'S97diskos_install'
# the stock firmware whose shell runs S96/S97: DISKOS_FW_BASE (default V2.40) and its extracted rootfs
FW_BASE = os.environ.get('DISKOS_FW_BASE', '240')
_fw_location = os.environ.get('DISKOS_FW_ROOT') or os.environ.get('DISKOS_V240_ROOT')
FW_ROOT = Path(_fw_location) if _fw_location else None
QEMU = shutil.which('qemu-mipsel-static')
RELEASED = 0xF6EFE127     # live V2.40 read, all keys released (bit 13 high)
HELD = 0xF6EFC127         # the same with bit 13 low = Vol-Up held
FW_BUSYBOX_MD5 = {'209': 'e6b2bb328e1c08238878991c0aae062d', '228': 'c1af60e81cea22b040b957c00404de9e',
                  '240': '5d8a5e60ee7d40002fe70f43228596cb', '257': '8651f07f6387df9f10a22a73f0f9450f'}
FW_APPLETS = ('mkdir', 'rm', 'mv', 'cat')
PROBE_SRC = Path(__file__).resolve().parents[1] / 'ui' / 'tools' / 'diskos_bootprobe.c'
_HOST_PROBE = None


def _host_probe():
    """A host build of the SAME source that is cross-built into the shipped payload binary."""
    global _HOST_PROBE
    if _HOST_PROBE is None:
        d = Path(tempfile.mkdtemp(prefix='diskos-bootprobe-'))
        exe = d / 'diskos-bootprobe'
        subprocess.run([os.environ.get('CC', 'cc'), '-Wall', '-Wextra', '-Werror', str(PROBE_SRC), '-o', str(exe)],
                       check=True)
        _HOST_PROBE = exe
    return _HOST_PROBE


def _host_only_mount():
    """A real host mountpoint with no counterpart under the firmware root (qemu -L redirects any absolute
    path that exists under the prefix; the probe itself runs without -L, but keep one fixture for both)."""
    for line in Path('/proc/mounts').read_text().splitlines():
        m = line.split()[1]
        if m != '/' and (FW_ROOT is None or not (FW_ROOT / m.lstrip('/')).exists()) and os.path.ismount(m):
            # Container credential mounts can share their parent's device. The real probe
            # requires a directory whose stat device differs from its parent (or root inode).
            try:
                st, parent = os.stat(m), os.stat(m + '/..')
            except OSError:
                continue
            if st.st_dev != parent.st_dev or st.st_ino == parent.st_ino:
                return m
    raise RuntimeError('no host-only mountpoint found for the userdata-mounted fixture')


def _have_fw_ash():
    if FW_ROOT is None:
        return False
    bb = FW_ROOT / 'bin' / 'busybox'
    return (bool(QEMU) and bb.is_file()
            and hashlib.md5(bb.read_bytes()).hexdigest() == FW_BUSYBOX_MD5.get(FW_BASE))


def _fake_mem(path, value):
    with open(path, 'wb') as f:
        f.truncate(0x10011000)
        f.seek(0x10010100)
        f.write(struct.pack('<I', value))


def _proc_alive(pid):
    """True if pid exists and is not a zombie. Only a missing entry counts as gone; other errors raise."""
    try:
        stat = Path(f'/proc/{pid}/stat').read_text()
    except (FileNotFoundError, ProcessLookupError):
        return False
    except OSError as e:
        if e.errno in (errno.ENOENT, errno.ESRCH):
            return False
        raise
    return stat.rsplit(')', 1)[1].split()[0] != 'Z'


def _pids_with(needle):
    """Live (non-zombie) processes whose command line contains needle."""
    found = []
    for d in Path('/proc').iterdir():
        if not d.name.isdigit():
            continue
        try:
            cmd = (d / 'cmdline').read_bytes()
        except OSError:
            continue
        if needle.encode() in cmd and _proc_alive(int(d.name)):
            found.append(int(d.name))
    return found


class _Env:
    """A private copy of S96 with a private record, preference, fake pin register and probe."""

    def __init__(self, shell, pin=RELEASED, pref_present=False, deadline_ms=5000):
        self.tmp = Path(tempfile.mkdtemp(prefix='diskos-s96-'))
        self.bin = self.tmp / 'bin'
        self.bin.mkdir()
        self.rec = self.tmp / 'boot_select'
        self.pref = self.tmp / 'boot_default_stock'
        if pref_present:
            self.pref.write_text('')
        self.mem = self.tmp / 'mem'
        if pin is not None:
            _fake_mem(self.mem, pin)
        text = S96.read_text()
        if shell == 'ash':
            ashw = self.tmp / 'ashw'
            ashw.write_text(f'#!/bin/sh\nexec {QEMU} -L {FW_ROOT} {FW_ROOT}/bin/busybox sh "$@"\n')
            ashw.chmod(0o755)
            text = f'#!{ashw}\n' + text.split('\n', 1)[1]
            for applet in FW_APPLETS:
                w = self.bin / applet
                w.write_text(f'#!/bin/sh\nexec {QEMU} -L {FW_ROOT} {FW_ROOT}/bin/busybox {applet} "$@"\n')
                w.chmod(0o755)
        else:
            # S96 uses plain mkdir, without -p. Keep the lock exclusive even on
            # hosts whose mkdir accepts a directory created by a competing call.
            w = self.bin / 'mkdir'
            w.write_text(f'#!{sys.executable}\n' + '''import os
import sys

paths = sys.argv[1:]
if not paths or any(path.startswith('-') for path in paths):
    sys.exit(2)
status = 0
for path in paths:
    try:
        os.mkdir(path)
    except OSError as error:
        print(f'mkdir: {path}: {error}', file=sys.stderr)
        status = 1
sys.exit(status)
''')
            w.chmod(0o755)
        self.script = self.tmp / 'S96diskos_select'
        self.script.write_text(text)
        self.script.chmod(0o755)
        self.probe = self.tmp / 'probe'
        if shell == 'ash':
            self.probe.write_text(f'#!/bin/sh\nexec {QEMU} {PAYLOAD / "diskos-bootprobe"} "$@"\n')
        else:
            self.probe.write_text(f'#!/bin/sh\nexec {_host_probe()} "$@"\n')
        self.probe.chmod(0o755)
        self.env = dict(os.environ, PATH=f'{self.bin}:{os.environ["PATH"]}',
                        DISKOS_BOOT_SELECT=str(self.rec), DISKOS_PREF_FILE=str(self.pref),
                        DISKOS_DATA_MOUNT=_host_only_mount(), DISKOS_BOOTPROBE=str(self.probe),
                        DISKOS_MEMDEV=str(self.mem), DISKOS_SELECT_DEADLINE_MS=str(deadline_ms))

    def stub_probe(self, body):
        """Replace the probe with a shell stub - only to feed S96 output the real probe never produces."""
        self.probe.write_text('#!/bin/sh\n' + body)

    def run(self, timeout=60, arg='start', cwd=None):
        t0 = time.monotonic()
        r = subprocess.run([str(self.script), arg], env=self.env, timeout=timeout,
                           capture_output=True, text=True, cwd=cwd)
        return r, time.monotonic() - t0

    def record(self):
        return self.rec.read_text() if self.rec.is_file() else None

    def why(self):
        p = Path(str(self.rec) + '.why')
        return p.read_text() if p.exists() else ''

    def close(self):
        for pid in _pids_with(str(self.tmp)):
            try:
                os.kill(pid, 9)
            except OSError:
                pass
        shutil.rmtree(self.tmp, ignore_errors=True)


class _Base:
    SHELL = None

    def make(self, *a, **k):
        e = _Env(self.SHELL, *a, **k)
        self.addCleanup(e.close)
        return e

    def test_truth_table(self):
        for pref, pin, want in ((False, RELEASED, 'diskos\n'), (True, RELEASED, 'stock\n'),
                                (False, HELD, 'stock\n'), (True, HELD, 'diskos\n')):
            with self.subTest(pref=pref, pin=hex(pin)):
                e = self.make(pin=pin, pref_present=pref)
                r, _ = e.run()
                self.assertEqual(r.returncode, 0)
                self.assertEqual(e.record(), want, e.why())

    def test_preference_lookup_errors_are_stock_not_absent(self):
        """Default absent + released would be diskOS; a lookup that FAILS (any errno but ENOENT) must not be
        read as absent. Real lstat errors through the real probe."""
        for name in ('ENOTDIR', 'ELOOP'):
            with self.subTest(errno=name):
                e = self.make()
                if name == 'ENOTDIR':
                    (e.tmp / 'afile').write_text('')
                    e.env['DISKOS_PREF_FILE'] = str(e.tmp / 'afile' / 'boot_default_stock')
                else:
                    (e.tmp / 'loop').symlink_to(e.tmp / 'loop')
                    e.env['DISKOS_PREF_FILE'] = str(e.tmp / 'loop' / 'boot_default_stock')
                e.run()
                self.assertEqual(e.record(), 'stock\n', e.why())
                self.assertIn('preference lookup failed', e.why())

    def test_userdata_not_mounted_is_stock(self):
        e = self.make()
        e.env['DISKOS_DATA_MOUNT'] = str(e.tmp)      # an ordinary directory, not a mountpoint
        e.run()
        self.assertEqual(e.record(), 'stock\n', e.why())

    def test_pin_unreadable_is_stock(self):
        e = self.make(pin=None)                      # no memory device at all
        e.run()
        self.assertEqual(e.record(), 'stock\n', e.why())
        self.assertIn('pin read failed', e.why())

    def test_probe_missing_or_failing_is_stock(self):
        for name in ('missing', 'exit 1', 'killed'):
            with self.subTest(probe=name):
                e = self.make()
                if name == 'missing':
                    e.env['DISKOS_BOOTPROBE'] = str(e.tmp / 'no-such-probe')
                elif name == 'exit 1':
                    e.stub_probe('echo "0 0 0xF6EFE127"\nexit 1\n')
                else:
                    e.stub_probe('echo "0 0 0xF6EFE127"\nkill -9 $$\n')
                r, _ = e.run()
                self.assertEqual(r.returncode, 0)
                self.assertEqual(e.record(), 'stock\n', e.why())

    def test_malformed_probe_output_is_stock(self):
        cases = {
            'empty': '', 'timeout flag': 'T 1 -', 'supervisor failure': 'X 1 -',
            'malformed hex (would abort ash arithmetic)': '0 0 0xZZZZZZZZ', 'short value': '0 0 0x2000',
            'no 0x prefix': '0 0 F6EFE127', 'extra field': '0 0 0xF6EFE127 x', 'two fields': '0 0',
            'bad flag': '2 0 0xF6EFE127', 'pin rc nonzero': '0 1 0xF6EFE127', 'glob': '0 0 *',
        }
        for name, out in cases.items():
            with self.subTest(case=name):
                e = self.make()
                e.stub_probe(f"printf '%s\\n' '{out}'\n")
                r, _ = e.run()
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertEqual(e.record(), 'stock\n', f'{name}: {e.why()}')

    def test_glob_shaped_value_is_not_expanded(self):
        """With a file named like a valid value in the cwd, a glob-shaped value must still be rejected."""
        e = self.make()
        e.stub_probe("printf '%s\\n' '0 0 0x????????'\n")
        (e.tmp / '0xF6EFE127').write_text('')
        r, _ = e.run(cwd=e.tmp)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(e.record(), 'stock\n', e.why())

    def test_hung_probe_is_bounded_and_killed(self):
        """A pin read that blocks (MEMDEV is a FIFO nobody writes) ends in stock within the deadline, and
        leaves nothing running. The real probe; nothing stubbed."""
        e = self.make(pin=None, deadline_ms=1500)
        os.mkfifo(e.mem)
        r, elapsed = e.run()
        self.assertEqual(r.returncode, 0)
        self.assertEqual(e.record(), 'stock\n', e.why())
        self.assertIn('timeout', e.why())
        self.assertGreaterEqual(elapsed, 1.4, 'returned before its deadline - was the hang reached?')
        self.assertLess(elapsed, 12, 'the deadline was not enforced')
        time.sleep(0.5)
        self.assertEqual(_pids_with(str(e.mem)), [], 'a blocked probe outlived the selector')

    def test_decides_once_per_boot(self):
        e = self.make()
        e.run()
        self.assertEqual(e.record(), 'diskos\n')
        _fake_mem(e.mem, HELD)                       # the button is now "held"
        for arg in ('start', 'reload', 'restart', 'stop', ''):
            with self.subTest(arg=arg):
                e.run(arg=arg)
                self.assertEqual(e.record(), 'diskos\n', f'invocation {arg!r} replaced the boot decision')

    def test_only_start_decides(self):
        for arg in ('reload', 'stop', ''):
            with self.subTest(arg=arg):
                e = self.make()
                e.run(arg=arg)
                self.assertIsNone(e.record())

    def test_concurrent_starts_decide_once(self):
        e = self.make()
        count = e.tmp / 'probes'
        e.stub_probe(f"echo x >> {count}\nsleep 0.5\necho '0 0 0xF6EFE127'\n")
        procs = [subprocess.Popen([str(e.script), 'start'], env=e.env, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL) for _ in range(2)]
        for p in procs:
            p.wait(timeout=60)
        self.assertEqual(len(count.read_text().split()), 1, 'both invocations probed')
        self.assertEqual(e.record(), 'diskos\n', e.why())

    def test_stale_diskos_record_never_survives(self):
        e = self.make(pin=None)
        e.rec.write_text('diskos\n')
        e.run()
        self.assertEqual(e.record(), 'stock\n')

    def test_record_path_that_is_a_directory_never_reads_as_diskos(self):
        e = self.make()
        e.rec.mkdir()
        e.run()
        r = subprocess.run([str(PAYLOAD / 'diskos-selected')], env=dict(os.environ, DISKOS_BOOT_SELECT=str(e.rec)))
        self.assertNotEqual(r.returncode, 0)

    def test_unwritable_record_leaves_no_diskos(self):
        e = self.make()
        e.env['DISKOS_BOOT_SELECT'] = str(e.tmp / 'no-such-dir' / 'boot_select')
        r, _ = e.run()
        self.assertEqual(r.returncode, 0)
        self.assertFalse((e.tmp / 'no-such-dir').exists())


@unittest.skipUnless(os.name != 'nt' and os.access('/bin/sh', os.X_OK)
                     and Path('/proc/mounts').is_file(), 'needs POSIX /bin/sh and Linux /proc')
@unittest.skipUnless(shutil.which(os.environ.get('CC', 'cc')), 'needs a host C compiler (CC or cc)')
class HostShellSelectTests(_Base, unittest.TestCase):
    SHELL = 'host'

    def test_host_mkdir_refuses_existing_directory(self):
        e = self.make()
        directory = e.tmp / 'lock'
        command = ['mkdir', str(directory)]
        first = subprocess.run(command, env=e.env, capture_output=True, text=True)
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertTrue(directory.is_dir())
        second = subprocess.run(command, env=e.env, capture_output=True, text=True)
        self.assertNotEqual(second.returncode, 0, 'mkdir accepted an existing directory')


@unittest.skipUnless(_have_fw_ash(), 'Set DISKOS_FW_ROOT (or DISKOS_V240_ROOT) to the selected stock rootfs; needs qemu-mipsel-static and pinned BusyBox')
@unittest.skipUnless(os.name != 'nt' and os.access('/bin/sh', os.X_OK)
                     and Path('/proc/mounts').is_file(), 'needs POSIX /bin/sh and Linux /proc')
class FirmwareAshSelectTests(_Base, unittest.TestCase):
    SHELL = 'ash'


@unittest.skipUnless(os.name != 'nt' and os.access('/bin/sh', os.X_OK), 'needs POSIX /bin/sh and FIFO support')
class InstallHookHonoursRecordTests(unittest.TestCase):
    """S97 does NAND work only on a positive "diskos" record; everything else exits without starting it."""

    def _run(self, record):
        tmp = Path(tempfile.mkdtemp(prefix='diskos-s97-rec-'))
        self.addCleanup(shutil.rmtree, tmp, True)
        rec = tmp / 'boot_select'
        if record == 'FIFO':
            os.mkfifo(rec)
        elif record is not None:
            rec.write_bytes(record)
        started = tmp / 'worker-started'
        text = S97.read_text()
        b = text.index('# >>> SUPERVISOR BEGIN')
        e = text.index('# <<< SUPERVISOR END')
        script = tmp / 'S97test'
        script.write_text('#!/bin/sh\n' + text[b:e].replace('/tmp/', str(tmp) + '/') +
                          f': > "{started}"\nexit 0\n')
        script.chmod(0o755)
        env = dict(os.environ, DISKOS_BOOT_SELECT=str(rec), DISKOS_SELECTED=str(PAYLOAD / 'diskos-selected'))
        subprocess.run([str(script), 'start'], env=env, timeout=30, check=True)
        time.sleep(0.5)
        return started.exists()

    def test_only_positive_diskos_starts_the_worker(self):
        for record, want in ((b'diskos\n', True), (None, False), (b'stock\n', False), (b'', False),
                             (b'diskos \n', False), (b'diskosx\n', False), (b'DISKOS\n', False),
                             (b'dis\0kos\n', False), (b'diskos', False), ('FIFO', False)):
            with self.subTest(record=record):
                self.assertEqual(self._run(record), want)


if __name__ == '__main__':
    unittest.main()
