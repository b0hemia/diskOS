# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Local fallbacks: Downloads scan, Downloads folder lookup, browser adapter. No network/device/browser."""
import hashlib
import os
import stat
import threading
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from diskos_installer import firmware_catalogue as fw, firmware_local as fl, firmware_resolve as fr

DATA = bytes(range(256)) * 40


class Base(unittest.TestCase):
    def setUp(self):
        if os.name == 'nt':
            self.skipTest('POSIX run-lock fixture')
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.dl = self.root / 'Downloads dir \u00e9'
        self.dl.mkdir()
        env = mock.patch.dict(os.environ, DISKOS_INSTALLER_HOME=str(self.root / 'state'))
        env.start()
        self.addCleanup(env.stop)
        self.pin = fw.Firmware('https://fiio-firmware.fiio.net/DISC/test_v240.zip', len(DATA),
                               hashlib.sha256(DATA).hexdigest(), 'FiiO V2.40')
        patch = mock.patch.dict(fw.__dict__, CATALOGUE=dict(fw.CATALOGUE, **{'240': self.pin}))
        patch.start()
        self.addCleanup(patch.stop)
        from diskos_installer.__main__ import _Lock
        self.lock = _Lock(windows=False)
        self.lock.path = str(self.root / 'run.lock')
        self.lock.__enter__()
        self.addCleanup(self.lock.__exit__, None, None, None)

    def put(self, name, data=DATA):
        path = self.dl / name
        path.write_bytes(data)
        return path

    def scan(self, **kw):
        return fr.check_downloads('240', str(self.dl), self.lock, **kw)

    def cache_files(self):
        base = Path(os.environ['DISKOS_INSTALLER_HOME']) / 'firmware'
        return sorted(p.name for p in base.glob('*')) if base.is_dir() else []


class ScanTests(Base):
    def test_renamed_match_found_and_user_file_untouched(self):
        src = self.put('holiday photos.bin')
        before = (src.read_bytes(), src.stat().st_mtime_ns)
        res = self.scan()
        self.assertEqual((res.kind, res.source), ('found', str(src)))
        self.assertEqual(os.path.basename(res.path), 'test_v240.zip')
        self.assertEqual(Path(res.path).read_bytes(), DATA)
        self.assertEqual((src.read_bytes(), src.stat().st_mtime_ns), before)
        self.assertEqual(self.cache_files(), ['test_v240.zip'])

    def test_wrong_size_ignored_without_hashing(self):
        self.put('a.zip', DATA + b'x')
        with mock.patch.object(fw, '_verify_stream', side_effect=AssertionError('hashed')):
            self.assertEqual(self.scan().kind, 'none')

    def test_same_size_wrong_hash_rejected(self):
        self.put('a.zip', b'\x00' * len(DATA))
        self.assertEqual(self.scan().kind, 'none')
        self.assertEqual(self.cache_files(), [])

    def test_partial_names_skipped(self):
        for name in ('x.zip.part', 'x.crdownload', 'Y.DOWNLOAD'):
            self.put(name)
        with mock.patch.object(fw, '_verify_stream', side_effect=AssertionError('hashed')):
            self.assertEqual(self.scan().kind, 'none')

    def test_file_changing_during_inspection_skipped(self):
        src = self.put('a.bin')
        real = fw._verify_stream

        def grow(stream, pin, output=None, check=None):
            real(stream, pin, output, check)
            os.utime(src, ns=(1, 1))
        with mock.patch.object(fw, '_verify_stream', grow):
            self.assertEqual(self.scan().kind, 'none')
        self.assertEqual(self.cache_files(), [])

    def test_symlink_and_directory_skipped(self):
        outside = self.root / 'real.bin'
        outside.write_bytes(DATA)
        os.symlink(outside, self.dl / 'link.bin')
        (self.dl / 'dir.bin').mkdir()
        (self.dl / 'sub').mkdir()
        (self.dl / 'sub' / 'inner.bin').write_bytes(DATA)
        self.assertEqual(self.scan().kind, 'none')

    def test_denied_directory_is_recoverable(self):
        with mock.patch('os.scandir', side_effect=PermissionError(13, 'Permission denied')):
            res = self.scan()
        self.assertEqual(res.kind, 'denied')
        self.assertIn('Permission denied', res.detail)

    def test_missing_folder_is_recoverable(self):
        res = fr.check_downloads('240', str(self.root / 'nope'), self.lock)
        self.assertEqual(res.kind, 'no_folder')

    def test_unreadable_file_skipped_not_fatal(self):
        self.put('a.bin')
        with mock.patch('os.open', side_effect=PermissionError(13, 'denied')):
            self.assertEqual(self.scan().kind, 'none')

    def test_cancel_mid_scan_stops_promptly_and_leaves_no_cache(self):
        self.put('a.bin')
        self.put('b.bin')
        calls = []

        def cancel():
            calls.append(1)
            return len(calls) >= 3  # after the first file starts hashing
        res = self.scan(cancel=cancel)
        self.assertEqual(res.kind, 'cancelled')
        self.assertEqual(self.cache_files(), [])
        self.assertEqual(len(calls), 3)

    def test_corrupt_copy_rejected_and_removed(self):
        src = self.put('a.bin')
        real = fw._publish

        def corrupt(partial, pin, path, check=None):
            with open(partial, 'r+b') as f:
                f.write(b'\xff')
            return real(partial, pin, path, check)
        with mock.patch.object(fw, '_publish', corrupt):
            self.assertEqual(self.scan().kind, 'none')
        self.assertEqual(self.cache_files(), [])
        self.assertEqual(src.read_bytes(), DATA)

    def test_unknown_version(self):
        with self.assertRaises(fl.LocalError):
            fr.check_downloads('999', str(self.dl), self.lock)


class HardeningTests(Base):
    def leftovers(self):
        return [n for n in self.cache_files() if n.endswith('.part')]

    def assert_untouched(self, src, data=DATA):
        self.assertTrue(src.exists())
        self.assertEqual(src.read_bytes(), data)

    def cancel_when(self, state):
        return lambda: state['on']

    def test_cancel_during_copy_publishes_nothing(self):
        src = self.put('a.bin')
        state = {'on': False}
        real = fw._verify_stream

        def spy(stream, pin, output=None, check=None):
            real(stream, pin, output, check)
            if output is not None:
                state['on'] = True
        with mock.patch.object(fw, '_verify_stream', spy):
            res = self.scan(cancel=self.cancel_when(state))
        self.assertEqual(res.kind, 'cancelled')
        self.assertEqual(self.cache_files(), [])
        self.assert_untouched(src)

    def test_cancel_during_saved_byte_verification_preserves_old_entry(self):
        old = self.root / 'old.bin'
        old.write_bytes(DATA)
        fw.store(str(old), '240', self.lock)
        cache = Path(fw.cached('240', self.lock))
        src = self.put('a.bin')
        state = {'on': False}
        real = fw._verify_stream

        def spy(stream, pin, output=None, check=None):
            copied = state.get('copied')
            if output is None and copied:
                state['on'] = True  # the saved-copy verification pass, after the copy
            real(stream, pin, output, check)
            if output is not None:
                state['copied'] = True
        with mock.patch.object(fw, '_verify_stream', spy):
            res = self.scan(cancel=self.cancel_when(state))
        self.assertEqual(res.kind, 'cancelled')
        self.assertEqual(self.cache_files(), ['test_v240.zip'])
        self.assertEqual(cache.read_bytes(), DATA)
        self.assert_untouched(src)

    def test_cancel_before_empty_directory_scan(self):
        self.assertEqual(self.scan(cancel=lambda: True).kind, 'cancelled')
        self.assertEqual(self.cache_files(), [])

    def test_cancel_during_enumeration_stops_reading_entries(self):
        for i in range(6):
            self.put('f%d.bin' % i, b'x')
        state, seen = {'on': False}, []
        real = os.scandir

        class Spy:
            def __init__(self, path):
                self.it = real(path)

            def __enter__(self):
                return self

            def __exit__(self, *a):
                self.it.close()

            def __iter__(self):
                for entry in self.it:
                    seen.append(entry.name)
                    if len(seen) == 2:
                        state['on'] = True
                    yield entry
        with mock.patch('os.scandir', Spy):
            res = self.scan(cancel=self.cancel_when(state))
        self.assertEqual(res.kind, 'cancelled')
        self.assertLess(len(seen), 6)

    def test_metadata_change_during_copy_rejected(self):
        src = self.put('a.bin')
        real = fw._verify_stream

        def touch(stream, pin, output=None, check=None):
            real(stream, pin, output, check)
            if output is not None:
                os.utime(src, ns=(5, 5))
        with mock.patch.object(fw, '_verify_stream', touch):
            self.assertEqual(self.scan().kind, 'none')
        self.assertEqual(self.cache_files(), [])
        self.assert_untouched(src)

    def _swap_case(self, make_replacement):
        """Replace the candidate between lstat and open (and again after reading) with a link/FIFO."""
        src = self.put('a.bin')
        kept = self.root / 'kept.bin'
        target = self.root / 'target.bin'
        target.write_bytes(DATA)
        done = []

        def swap():
            if not done:
                done.append(1)
                os.rename(src, kept)
                make_replacement(src, target)
        real_verify, real_store = fw._verify_stream, fw.store

        def verify(stream, pin, output=None, check=None):
            real_verify(stream, pin, output, check)
            if output is not None:
                swap()

        def store(source, version, run_lock=None):
            swap()
            return real_store(source, version, run_lock)
        out = []
        with mock.patch.object(fw, '_verify_stream', verify), mock.patch.object(fw, 'store', store):
            thread = threading.Thread(target=lambda: out.append(self.scan()), daemon=True)
            thread.start()
            thread.join(5)
            hung = thread.is_alive()
            if hung and stat.S_ISFIFO(os.lstat(src).st_mode):
                fd = os.open(src, os.O_WRONLY | os.O_NONBLOCK)  # release the blocked open
                os.close(fd)
                thread.join(5)
        self.assertFalse(hung, 'scan blocked on the replacement')
        self.assertEqual(out[0].kind, 'none')
        self.assertEqual(self.cache_files(), [])
        self.assertEqual(kept.read_bytes(), DATA)
        self.assertEqual(target.read_bytes(), DATA)

    def test_symlink_swap_not_followed(self):
        self._swap_case(lambda src, target: os.symlink(target, src))

    @unittest.skipUnless(hasattr(os, 'mkfifo'), 'no mkfifo')
    def test_fifo_swap_does_not_hang(self):
        self._swap_case(lambda src, target: os.mkfifo(src))


class LocateTests(unittest.TestCase):
    def test_windows_redirected_known_folder(self):
        got = fl.downloads_dir('win32', {'USERPROFILE': 'C:\\Users\\u'}, 'C:\\Users\\u',
                               known_folder=lambda: '/D/Dl\u00e9 x')
        self.assertEqual(got, '/D/Dl\u00e9 x')

    def test_windows_lookup_failure_falls_back_to_profile(self):
        def boom():
            raise OSError('x')
        got = fl.downloads_dir('win32', {'USERPROFILE': '/u'}, '/h', known_folder=boom)
        self.assertEqual(got, os.path.join('/u', 'Downloads'))

    def test_macos(self):
        self.assertEqual(fl.downloads_dir('darwin', {}, '/Users/a b'), '/Users/a b/Downloads')

    def test_linux_xdg_with_spaces_and_unicode(self):
        text = '# c\nXDG_DOWNLOAD_DIR="$HOME/T\u00e9l\u00e9 chargements"\n'
        got = fl.downloads_dir('linux', {}, '/home/a b', read=lambda p: text)
        self.assertEqual(got, '/home/a b/T\u00e9l\u00e9 chargements')

    def test_linux_xdg_absolute_and_fallbacks(self):
        got = fl.downloads_dir('linux', {'XDG_CONFIG_HOME': '/c'}, '/h',
                               read=lambda p: 'XDG_DOWNLOAD_DIR="/mnt/dl"\n' if p == '/c/user-dirs.dirs' else '')
        self.assertEqual(got, '/mnt/dl')
        self.assertEqual(fl.downloads_dir('linux', {}, '/h', read=lambda p: 'XDG_DOWNLOAD_DIR="$HOME"\n'),
                         '/h/Downloads')

        def missing(p):
            raise FileNotFoundError(p)
        self.assertEqual(fl.downloads_dir('linux', {}, '/h', read=missing), '/h/Downloads')


class BrowserTests(unittest.TestCase):
    def test_not_opened_until_called_and_opens_official_page(self):
        calls = []
        res = fr.open_download_page('240', opener=lambda url, new=0: calls.append(url) or True)
        self.assertEqual((res.ok, res.url, calls), (True, fl.OFFICIAL_PAGE, [fl.OFFICIAL_PAGE]))

    def test_no_browser_returns_url(self):
        res = fr.open_download_page('240', opener=lambda url, new=0: False)
        self.assertFalse(res.ok)
        self.assertEqual(res.url, fl.OFFICIAL_PAGE)

    def test_browser_exception_returns_url(self):
        def boom(url, new=0):
            raise RuntimeError('no display')
        res = fr.open_download_page('240', opener=boom)
        self.assertFalse(res.ok)
        self.assertEqual(res.url, fl.OFFICIAL_PAGE)
        self.assertIn('no display', res.error)

    def test_unknown_version(self):
        with self.assertRaises(fl.LocalError):
            fr.open_download_page('1', opener=lambda *a, **k: True)


if __name__ == '__main__':
    unittest.main()
