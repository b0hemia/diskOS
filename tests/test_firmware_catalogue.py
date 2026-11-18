# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Whole-package pins and crash-safe owned cache."""
import contextlib
import hashlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

from diskos_installer import firmware_catalogue as fw, state


class CatalogueTests(unittest.TestCase):
    def test_local_pins(self):
        roots = [Path(__file__).resolve().parents[2] / 'research',
                 Path.home() / 'diskos' / 'research']
        for version, pin in fw.CATALOGUE.items():
            name = pin.url.rsplit('/', 1)[1]
            paths = [r / d / name for r in roots
                     for d in ('firmware', 'firmware-archives')]
            path = next((p for p in paths if p.is_file()), None)
            with self.subTest(version=version):
                if path is None:
                    self.skipTest('Research firmware ZIP absent: ' + name)
                fw.verify(path, version)

    def test_immutable(self):
        with self.assertRaises(TypeError):
            fw.CATALOGUE['209'] = fw.CATALOGUE['257']
        with self.assertRaises(AttributeError):
            fw.CATALOGUE['209'].size = 1

    def test_drift(self):
        fw.check_consistency()
        for module, attr in ((fw.imagebuild, 'INSTALL_FW'),
                             (fw.imagebuild, 'PINNED_ROOTFS'),
                             (fw.basegate, 'CATALOGUE')):
            with self.subTest(attr=attr), mock.patch.object(module, attr, {}):
                with self.assertRaisesRegex(RuntimeError, 'catalogue.*drift'):
                    fw.check_consistency()
        with mock.patch.object(fw, 'CATALOGUE', {}):
            with self.assertRaises(RuntimeError):
                fw.check_consistency()


class CacheTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.env = mock.patch.dict(os.environ, DISKOS_INSTALLER_HOME=str(self.root / 'state'))
        self.env.start()
        self.addCleanup(self.env.stop)
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, 'w') as z:
            z.writestr(zipfile.ZipInfo('firmware'), b'firmware bytes')
        self.data = buf.getvalue()
        pin = fw.Firmware('https://fiio-firmware.fiio.net/test.zip', len(self.data),
                          hashlib.sha256(self.data).hexdigest(), 'FiiO V2.09')
        self.patch = mock.patch.dict(fw.__dict__, CATALOGUE=dict(fw.CATALOGUE, **{'209': pin}))
        self.patch.start()
        self.addCleanup(self.patch.stop)
        # Exercise the real lock at a private path, never the live installer lock.
        from diskos_installer.__main__ import _Lock
        if os.name == 'nt':
            import ctypes
            api = mock.Mock()
            api.mutex_security.return_value = ctypes.c_int(0)
            api.CreateMutexExW.return_value = 1
            api.WaitForSingleObject.return_value = 0
            self.lock = _Lock(windows=True, api=api)
        else:
            self.lock = _Lock(windows=False)
            self.lock.path = str(self.root / 'run.lock')
        self.lock.__enter__()
        self.addCleanup(self.lock.__exit__, None, None, None)
        self.src = self.root / 'original.zip'
        self.src.write_bytes(self.data)

    def test_verification(self):
        fw.verify(self.src, '209')
        for data in (self.data[:-1], self.data + b'x',
                     bytes([self.data[0] ^ 1]) + self.data[1:]):
            with self.subTest(length=len(data)):
                self.src.write_bytes(data)
                with self.assertRaises(ValueError):
                    fw.verify(self.src, '209')

    def test_bounded_reads(self):
        stream = io.BytesIO(self.data + b'x' * 10000)
        with mock.patch('builtins.open', return_value=contextlib.nullcontext(stream)):
            with self.assertRaises(ValueError):
                fw.verify(self.src, '209')
            self.assertEqual(stream.tell(), len(self.data) + 1)

    def test_cache_rehash_and_remove_corrupt(self):
        self.assertIsNone(fw.cached('209', self.lock))
        dst = Path(fw.store(self.src, '209', self.lock))
        self.assertEqual(dst.parent, Path(state.state_dir()) / 'firmware')
        self.assertEqual(dst.name, 'test.zip')
        self.assertEqual(fw.cached('209', self.lock), str(dst))
        dst.write_bytes(b'x' * len(self.data))
        self.assertIsNone(fw.cached('209', self.lock))
        self.assertFalse(dst.exists())
        self.assertEqual(self.src.read_bytes(), self.data)

    def test_cache_uses_fiio_filename_and_distrusts_same_name_wrong_bytes(self):
        for version, pin in fw.CATALOGUE.items():
            if version != '209':
                self.assertEqual(os.path.basename(fw._cache_path(pin)), pin.url.rsplit('/', 1)[1])
        folder = Path(state.state_dir()) / 'firmware'
        wrong = folder / 'test.zip'
        wrong.write_bytes(bytes([self.data[0] ^ 1]) + self.data[1:])
        self.assertIsNone(fw.cached('209', self.lock))
        self.assertFalse(wrong.exists())
        wrong.write_bytes(b'wrong')
        dst = Path(fw.store(self.src, '209', self.lock))
        self.assertEqual(dst, wrong)
        self.assertEqual(dst.read_bytes(), self.data)

    def test_unsafe_catalogue_filenames_rejected(self):
        for url in ('https://h/a/..', 'https://h/a/x.txt', 'https://h/a/b c.zip',
                    'https://h/a/', 'https://h/a/%2e%2e%5cx.zip', 'https://h/a/x.zip?y=1/z'):
            with self.subTest(url=url):
                pin = fw.CATALOGUE['209']._replace(url=url)
                with self.assertRaises(ValueError):
                    fw._cache_path(pin)

    def test_part_files_adjacent_and_unique(self):
        seen = []
        replace = os.replace
        def promote(src, dst):
            seen.append(Path(src))
            self.assertEqual(Path(src).parent, Path(dst).parent)
            self.assertTrue(Path(src).name.startswith('test.zip.'))
            self.assertTrue(Path(src).name.endswith('.part'))
            replace(src, dst)
        with mock.patch.object(fw.os, 'replace', side_effect=promote):
            fw.store(self.src, '209', self.lock)
            fw.store(self.src, '209', self.lock)
        self.assertEqual(len(set(seen)), 2)

    @unittest.skipIf(os.name == 'nt', 'POSIX descriptor lock')
    def test_exited_lock_rejected(self):
        from diskos_installer.__main__ import _Lock
        lock = _Lock(windows=False)
        lock.path = str(self.root / 'other.lock')
        with lock:
            pass
        with self.assertRaisesRegex(RuntimeError, 'held installer run lock'):
            fw.cached('209', lock)
        with self.assertRaisesRegex(RuntimeError, 'held installer run lock'):
            fw.store(self.src, '209', lock)
        self.assertFalse((Path(state.state_dir()) / 'firmware').exists()
                         and any((Path(state.state_dir()) / 'firmware').iterdir()))

    @unittest.skipIf(os.name == 'nt', 'POSIX descriptor lock')
    def test_descriptor_for_another_file_rejected(self):
        from diskos_installer.__main__ import _Lock
        lock = _Lock(windows=False)
        lock.path = str(self.root / 'third.lock')
        with lock:
            lock.path = str(self.root / 'unrelated.lock')
            Path(lock.path).write_bytes(b'')
            with self.assertRaisesRegex(RuntimeError, 'held installer run lock'):
                fw.cached('209', lock)
            lock.path = str(self.root / 'third.lock')

    def test_crash_before_promotion(self):
        with mock.patch.object(fw.os, 'replace', side_effect=OSError('simulated crash')):
            with self.assertRaisesRegex(OSError, 'simulated crash'):
                fw.store(self.src, '209', self.lock)
        self.assertIsNone(fw.cached('209', self.lock))
        self.assertEqual(list((Path(state.state_dir()) / 'firmware').iterdir()), [])
        self.assertTrue(Path(fw.store(self.src, '209', self.lock)).is_file())

    def test_fsync_before_rename_and_reverify_copy(self):
        events = []
        replace = os.replace
        def promote(src, dst):
            self.assertEqual(Path(src).parent, Path(dst).parent)
            self.assertEqual(Path(src).read_bytes(), self.data)
            self.assertIn('fsync', events)
            events.append('rename')
            replace(src, dst)
        with mock.patch.object(fw.os, 'fsync', side_effect=lambda fd: events.append('fsync')), \
                mock.patch.object(fw.os, 'replace', side_effect=promote):
            fw.store(self.src, '209', self.lock)
        self.assertEqual(events[0], 'fsync')
        self.src.write_bytes(self.data[:-1])
        with self.assertRaises(ValueError):
            fw.store(self.src, '209', self.lock)
        self.assertIsNotNone(fw.cached('209', self.lock))

    def staged(self, data=None):
        part = Path(fw.new_partial('209', self.lock))
        part.write_bytes(self.data if data is None else data)
        return part

    def firmware_files(self):
        return sorted(p.name for p in (Path(state.state_dir()) / 'firmware').iterdir())

    def test_new_partial_is_empty_owned_and_unique(self):
        a, b = Path(fw.new_partial('209', self.lock)), Path(fw.new_partial('209', self.lock))
        self.assertNotEqual(a, b)
        for part in (a, b):
            self.assertEqual(part.read_bytes(), b'')
            self.assertEqual(part.parent, Path(state.state_dir()) / 'firmware')
            self.assertTrue(part.name.startswith('test.zip.') and part.name.endswith('.part'))

    def test_promote_publishes_and_rejects_bad_input(self):
        path = fw.promote(str(self.staged()), '209', self.lock)
        self.assertEqual(Path(path).read_bytes(), self.data)
        self.assertEqual(self.firmware_files(), ['test.zip'])
        for data in (self.data[:-1], self.data + b'x', bytes([self.data[0] ^ 1]) + self.data[1:]):
            part = self.staged(data)
            with self.assertRaises(ValueError):
                fw.promote(str(part), '209', self.lock)
            part.unlink()
        foreign = self.root / 'elsewhere.part'
        foreign.write_bytes(self.data)
        with self.assertRaises(ValueError):
            fw.promote(str(foreign), '209', self.lock)
        plain = Path(state.state_dir()) / 'firmware' / 'plain.bin'
        plain.write_bytes(self.data)
        with self.assertRaises(ValueError):
            fw.promote(str(plain), '209', self.lock)
        self.assertTrue(foreign.exists() and plain.exists())

    def test_check_hook_stops_verification_and_publication(self):
        # The small fixture is read in one chunk: call 1 precedes that read, call 2
        # precedes the read that returns EOF, call 3 is the last stop before replace.
        class Stop(Exception):
            pass
        for stop_at in (1, 2, 3):
            with self.subTest(stop_at=stop_at):
                part = self.staged()
                calls = []

                def check():
                    calls.append(1)
                    if len(calls) == stop_at:
                        raise Stop
                with mock.patch.object(fw.os, 'replace') as replace:
                    with self.assertRaises(Stop):
                        fw.promote(str(part), '209', self.lock, check=check)
                replace.assert_not_called()
                self.assertEqual(len(calls), stop_at)
                self.assertEqual(self.firmware_files(), [part.name])
                self.assertIsNone(fw.cached('209', self.lock))
                part.unlink()
        part = self.staged()
        calls = []
        path = fw.promote(str(part), '209', self.lock, check=lambda: calls.append(1))
        self.assertEqual(len(calls), 3)
        self.assertEqual(Path(path).read_bytes(), self.data)

    def test_check_hook_runs_before_every_bounded_read(self):
        order = []

        class Spy(io.BytesIO):
            def read(self, n=-1):
                order.append('read')
                return super().read(n)
        pin = fw.CATALOGUE['209']._replace(size=3 << 20)
        with self.assertRaises(ValueError):
            fw._verify_stream(Spy(b'0' * (3 << 20)), pin, check=lambda: order.append('check'))
        self.assertEqual(order[:4], ['check', 'read', 'check', 'read'])

    def test_removal_covers_cache_and_partials(self):
        dst = Path(fw.store(self.src, '209', self.lock))
        partial = dst.parent / 'interrupted.part'
        partial.write_bytes(b'partial')
        with contextlib.ExitStack() as stack:
            if os.name == 'nt':
                stack.enter_context(mock.patch.object(
                    state.os, 'getuid',
                    return_value=os.stat(self.root).st_uid, create=True))
            base, errors = state.wipe_all()
        self.assertEqual(errors, [])
        self.assertFalse(Path(base).exists())
        self.assertTrue(self.src.exists())

    @unittest.skipIf(os.name == 'nt', 'POSIX subprocess lock contention')
    def test_cross_process_lock(self):
        code = f"""from diskos_installer.__main__ import _Lock
from diskos_installer import firmware_catalogue as f
original = _Lock.__init__
def init(self):
    original(self, windows=False)
    self.path = {self.lock.path!r}
_Lock.__init__ = init
f.cached('209')
"""
        result = subprocess.run([sys.executable, '-c', code], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('another diskOS installer', result.stderr)


if __name__ == '__main__':
    unittest.main()
