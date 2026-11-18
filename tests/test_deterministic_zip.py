# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline archive determinism and package boundary tests."""
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'build/deterministic_zip.py'
spec = importlib.util.spec_from_file_location('deterministic_zip', SCRIPT)
zipper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(zipper)


class DeterministicZipTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.work = Path(temp.name)
        self.root = self.work / 'machine-specific-stage'
        self.root.mkdir()
        self.contents = {'z.exe': b'MZ fixture', '_internal/a.dll': b'DLL',
                         'README.md': b'portable archive\n'}
        for name, data in reversed(list(self.contents.items())):
            path = self.root / name
            path.parent.mkdir(exist_ok=True)
            path.write_bytes(data)
        self.manifest()
        self.output = self.work / 'package.zip'

    def manifest(self):
        text = ''.join(hashlib.sha256(data).hexdigest() + '  ' + name + '\n'
                       for name, data in sorted(self.contents.items()))
        (self.root / 'SHA256SUMS').write_text(text, encoding='utf-8', newline='\n')

    def test_same_bytes_after_location_permissions_and_mtime_change(self):
        first = zipper.archive(self.root, self.output)
        other = self.work / 'different-machine-path'
        self.root.rename(other)
        for path in other.rglob('*'):
            os.utime(path, (1800000000, 1800000000))
            if path.is_file():
                path.chmod(0o755)
        second = self.work / 'second.zip'
        with mock.patch.dict(os.environ, TZ='Pacific/Auckland'):
            self.assertEqual(first, zipper.archive(other, second))
        self.assertEqual(self.output.read_bytes(), second.read_bytes())
        with zipfile.ZipFile(second) as zipped:
            self.assertEqual(zipped.comment, b'')
            self.assertEqual(zipped.namelist(), sorted(zipped.namelist()))
            self.assertEqual(len(zipped.namelist()), len(self.contents) + 1)
            for info in zipped.infolist():
                self.assertTrue(info.filename.startswith(zipper.PREFIX + '/'))
                self.assertNotIn(str(self.work), info.filename)
                self.assertEqual(info.date_time, zipper.STAMP)
                self.assertEqual(info.compress_type, zipfile.ZIP_STORED)
                self.assertEqual(info.external_attr, (stat.S_IFREG | 0o644) << 16)
                self.assertEqual((info.create_system, info.extra, info.comment), (3, b'', b''))

    def test_missing_extra_and_changed_files_refused_before_output(self):
        for change in ('missing', 'extra', 'changed'):
            with self.subTest(change=change):
                target = self.root / ('extra.txt' if change == 'extra' else 'z.exe')
                if change == 'missing':
                    target.rename(self.work / 'missing')
                else:
                    target.write_bytes(b'changed')
                with self.assertRaises((ValueError, KeyError)):
                    zipper.archive(self.root, self.output)
                self.assertFalse(self.output.exists())
                if change == 'extra':
                    target.rename(self.work / 'extra')
                else:
                    target.write_bytes(self.contents['z.exe'])

    def test_hostile_inventory_records(self):
        original = (self.root / 'SHA256SUMS').read_text()
        for text in ('', original + original.splitlines()[0] + '\n',
                     '0' * 64 + '  SHA256SUMS\n', '0' * 64 + '  ../escape\n',
                     '0' * 64 + '  C:/machine/path\n', 'bad record\n'):
            with self.subTest(text=text):
                (self.root / 'SHA256SUMS').write_text(text)
                with self.assertRaises(ValueError):
                    zipper.archive(self.root, self.output)
                self.assertFalse(self.output.exists())

    @unittest.skipIf(os.name == 'nt', 'POSIX link fixtures')
    def test_links_and_special_files_refused(self):
        for kind in ('file-link', 'directory-link', 'fifo', 'hard-link'):
            with self.subTest(kind=kind):
                path = self.root / kind
                if kind == 'hard-link':
                    os.link(self.root / 'z.exe', path)
                elif kind == 'fifo':
                    os.mkfifo(path)
                else:
                    path.symlink_to(self.root / ('_internal' if kind == 'directory-link' else 'z.exe'))
                with self.assertRaises(ValueError):
                    zipper.archive(self.root, self.output)
                path.rename(self.work / kind)
        alias = self.work / 'alias'
        alias.symlink_to(self.root)
        with self.assertRaises(ValueError):
            zipper.archive(alias, self.output)

    def test_unsafe_windows_names_and_case_collisions(self):
        for name in ('/absolute', 'a/../b', 'a\\b', 'nul.txt', 'COM1.exe', 'x:stream', 'x.', 'x ', 'x\n'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                zipper.safe_name(name)
        with mock.patch.object(zipper.os, 'walk', return_value=[(str(self.root), ['DATA', 'data'], [])]):
            with mock.patch.object(zipper, 'checked_stat', return_value=mock.Mock(st_mode=stat.S_IFDIR)):
                with self.assertRaisesRegex(ValueError, 'Case-colliding'):
                    zipper.inventory(self.root)

    def test_no_output_inside_input_or_overwrite(self):
        with self.assertRaises(ValueError):
            zipper.archive(self.root, self.root / 'out.zip')
        self.output.write_bytes(b'keep')
        with self.assertRaises(FileExistsError):
            zipper.archive(self.root, self.output)
        self.assertEqual(self.output.read_bytes(), b'keep')

    def test_all_reserved_devices_in_every_component(self):
        devices = ['CON', 'PRN', 'AUX', 'NUL', 'CONIN$', 'CONOUT$']
        devices += [prefix + digit for prefix in ('COM', 'LPT')
                    for digit in '0123456789\u00b9\u00b2\u00b3']
        for device in devices:
            for base in (device, device.lower(), device.swapcase()):
                for suffix in ('', '.txt', '.tar.gz', ' .txt', '..txt', ' ', '.', '.txt .'):
                    for name in (base + suffix, 'dir/' + base + suffix,
                                 'dir/' + base + suffix + '/file'):
                        with self.subTest(name=name), self.assertRaises(ValueError):
                            zipper.safe_name(name)
        for name in ('console.txt', 'COM10.exe', 'LPT10', 'xCONIN$', 'dir/auxiliary.txt'):
            zipper.safe_name(name)

    def test_zip64_preflight_rejects_before_output_or_hashing(self):
        for limit, count in ((100, 65535), (350, 65535), (zipfile.ZIP64_LIMIT, 3)):
            with self.subTest(limit=limit, count=count), \
                    mock.patch.object(zipfile, 'ZIP64_LIMIT', limit), \
                    mock.patch.object(zipfile, 'ZIP_FILECOUNT_LIMIT', count), \
                    mock.patch.object(hashlib, 'file_digest') as digest:
                with self.assertRaisesRegex(ValueError, 'ZIP64'):
                    zipper.archive(self.root, self.output)
                digest.assert_not_called()
                self.assertFalse(self.output.exists())

    def test_zip32_preflight_member_offset_directory_and_count_boundaries(self):
        # Small synthetic limits exercise the real arithmetic without giant fixtures.
        path = mock.Mock()
        size = 0
        def info(_):
            return types.SimpleNamespace(st_mode=stat.S_IFREG, st_nlink=1, st_size=size)
        files = {'x': path}
        name_bytes = len((zipper.PREFIX + '/x').encode('utf-8'))
        local = zipfile.sizeFileHeader + name_bytes
        directory = zipfile.sizeCentralDir + name_bytes
        with mock.patch.object(zipper, 'checked_stat', side_effect=info):
            for limit, count, size, message in (
                    (100, 1, 96, 'file size'),
                    (local + 10, 1, 11, 'offset'),
                    (directory - 1, 1, 0, 'directory size'),
                    (1000, 0, 0, 'file count')):
                with self.subTest(message=message), \
                        mock.patch.object(zipfile, 'ZIP64_LIMIT', limit), \
                        mock.patch.object(zipfile, 'ZIP_FILECOUNT_LIMIT', count):
                    with self.assertRaisesRegex(ValueError, message):
                        zipper.zip32_preflight(files)
            size = 0
            with mock.patch.object(zipfile, 'ZIP64_LIMIT', directory), \
                    mock.patch.object(zipfile, 'ZIP_FILECOUNT_LIMIT', 1):
                zipper.zip32_preflight(files)
            # UTF-8 filename bytes, not character counts, determine both headers.
            unicode_files = {'\u00e9' * 20: path}
            with mock.patch.object(zipfile, 'ZIP64_LIMIT', 110):
                with self.assertRaisesRegex(ValueError, 'directory size'):
                    zipper.zip32_preflight(unicode_files)

    def test_writer_disables_zip64(self):
        with mock.patch.object(zipfile, 'ZipFile', wraps=zipfile.ZipFile) as writer:
            zipper.archive(self.root, self.output)
        self.assertIs(writer.call_args.kwargs.get('allowZip64'), False)

    def test_cli_oversize_error_has_no_traceback_or_output(self):
        code = ('import runpy, zipfile; zipfile.ZIP64_LIMIT = 100; '
                'runpy.run_path(' + repr(str(SCRIPT)) + ', run_name="__main__")')
        run = subprocess.run([sys.executable, '-B', '-c', code, str(self.root), str(self.output)],
                             cwd=self.work, capture_output=True, text=True)
        self.assertEqual(run.returncode, 1)
        self.assertIn('ZIP64', run.stderr)
        self.assertNotIn('Traceback', run.stderr)
        self.assertEqual(run.stdout, '')
        self.assertFalse(self.output.exists())

    def test_cli_from_unrelated_directory(self):
        run = subprocess.run([sys.executable, str(SCRIPT), str(self.root), str(self.output)],
                             cwd=self.work, capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout.strip(), hashlib.sha256(self.output.read_bytes()).hexdigest())

    def test_windows_reparse_point_refused(self):
        info = mock.Mock(st_mode=stat.S_IFDIR, st_file_attributes=0x400)
        with mock.patch.object(Path, 'lstat', return_value=info):
            with self.assertRaisesRegex(ValueError, 'reparse'):
                zipper.checked_stat(self.root)

    def test_lock_pins_runtime_and_has_no_unhashed_fallback(self):
        lock = (ROOT / 'build/windows-build.lock').read_text()
        entries = {}
        for line in lock.splitlines():
            if line and not line.startswith('#'):
                match = re.fullmatch(r'([\w-]+)==([\d.]+) --hash=sha256:([0-9a-f]{64}|\[VERIFY\])', line)
                self.assertIsNotNone(match, line)
                name, version, digest = match.groups()
                self.assertNotIn(name.lower(), entries)
                entries[name.lower()] = version
        for name, version in re.findall(r'^([\w-]+)==([\d.]+)', (ROOT / 'requirements.txt').read_text(), re.M):
            self.assertEqual(entries[name.lower()], version)
        self.assertEqual(entries['pyinstaller'], '6.16.0')
