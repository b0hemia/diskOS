# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Windows archive dispatch, failure gates and complete pack comparison."""
import contextlib
import os
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

from diskos_installer import bundle, imagebuild as ib
from diskos_installer.pseudofs import Archive, MARK


class WindowsBuildTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=os.environ.get('DISKOS_WIN_SCRATCH'))
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.out = self.work / 'image.bin'
        self.calls = []
        def patch(target, **kw):
            p = mock.patch(target, **kw)
            result = p.start()
            self.addCleanup(p.stop)
            return result
        self.host = patch('diskos_installer.bundle.platform_probe.host', return_value=('windows', 'x86_64'))
        patch('diskos_installer.bundle.native', side_effect=lambda name: name)
        self.probe = patch('diskos_installer.imagebuild.check_archive_squashfs_tools')
        self.stock = patch('diskos_installer.imagebuild.validate_stock_archive', return_value='257')
        patch('diskos_installer.imagebuild._validate_ui_elf')
        self.export = patch('diskos_installer.imagebuild._export_archive', side_effect=self.archive)
        self.edit = patch('diskos_installer.imagebuild.edit_image', return_value=(b'boot', 'bb', 'ui', 2))
        self.validate = patch('diskos_installer.imagebuild.validate_archive_output')
        self.pack = patch('diskos_installer.imagebuild._run', side_effect=self.pack_run)
        self.tree = patch('diskos_installer.imagebuild.check_squashfs_tools',
                          side_effect=ib.BuildError('tree marker', code='E104'))
        self.rep = mock.Mock()

    @contextlib.contextmanager
    def archive(self, *args, **kw):
        with Archive(b'/ D 17 755 99 88\nf R 1 6755 99 88 2 0 0\n' + MARK + b'UI') as archive:
            yield archive

    def pack_run(self, cmd, **kw):
        self.calls.append((cmd, kw))
        Path(cmd[2]).write_bytes(b'hsqs')
        return SimpleNamespace(returncode=0, stderr='')

    def build(self):
        return ib.build_image('stock', 'ui', 'public', str(self.out), str(self.work),
                              rep=self.rep, no_ota=True)

    def test_windows_dispatch_and_pack(self):
        self.assertEqual(self.build(), str(self.out))
        self.tree.assert_not_called()
        self.stock.assert_called_once_with('stock', 'unsquashfs', str(self.work), self.rep, allow_override=False)
        self.assertEqual(self.out.stat().st_size, ib.IMG_SIZE)
        cmd, kw = self.calls[0]
        self.assertEqual(cmd[3:], ['-pf', cmd[4], '-comp', 'lzo', '-b', '131072',
                                  '-no-xattrs', '-all-root', '-noappend',
                                  '-root-time', '17', '-root-mode', '755'])
        self.assertEqual(kw['msys_paths'], (1, 2, 4))
        self.assertIn('intended_archive', self.validate.call_args.kwargs)

    def test_posix_dispatch_unchanged(self):
        for host in ('linux', 'macos'):
            self.host.return_value = (host, 'x86_64')
            with self.subTest(host=host), self.assertRaises(ib.BuildError) as caught:
                self.build()
            self.assertEqual(caught.exception.code, 'E104')
        self.stock.assert_not_called()
        self.probe.assert_not_called()

    def test_msys_or_cygwin_python_refused(self):
        # MSYS2/Cygwin Python reports a non-Windows system name; never fall back to the tree path.
        from diskos_installer.errors import PreflightError
        for host in ('msys_nt-10.0-26100', 'cygwin_nt-10.0-26100'):
            self.host.return_value = (host, 'x86_64')
            with self.subTest(host=host), self.assertRaises(PreflightError) as caught:
                self.build()
            self.assertEqual(caught.exception.code, 'E101')
        self.tree.assert_not_called()
        self.probe.assert_not_called()
        self.assertFalse(self.out.exists())

    def test_failure_codes_no_final_image(self):
        stages = [(self.stock, 'E220'), (self.stock, 'E221'), (self.stock, 'E224'),
                  (self.stock, 'E230'), (self.probe, 'E104'), (self.export, 'E230'),
                  (self.validate, 'E232'), (self.validate, 'E231')]
        for stage, code in stages:
            before = stage.side_effect
            stage.side_effect = ib.BuildError('injected', code=code)
            with self.subTest(code=code), self.assertRaises(ib.BuildError) as caught:
                self.build()
            self.assertEqual(caught.exception.code, code)
            self.assertFalse(self.out.exists())
            stage.side_effect = before
        self.stock.return_value = '177'
        with self.assertRaises(ib.BuildError) as caught:
            self.build()
        self.assertEqual(caught.exception.code, 'E225')
        self.stock.return_value = '257'
        for failure in (OSError('missing'), ValueError('DLL'), subprocess.TimeoutExpired('pack', 1),
                        SimpleNamespace(returncode=1, stderr='pack failed')):
            self.pack.side_effect = failure if isinstance(failure, Exception) else None
            self.pack.return_value = failure
            with self.subTest(failure=failure), self.assertRaises(ib.BuildError) as caught:
                self.build()
            self.assertEqual(caught.exception.code, 'E230')
            self.assertFalse(self.out.exists())

    def test_actual_size_gate(self):
        # Invoke the real size gate on the build's output, before any content export.
        self.validate.side_effect = lambda *a, **kw: ib_validate(*a, **kw)
        with mock.patch.object(ib.os.path, 'getsize', return_value=ib.IMG_SIZE + 1):
            with self.assertRaises(ib.BuildError) as caught:
                self.build()
        self.assertEqual(caught.exception.code, 'E231')
        self.assertFalse(self.out.exists())

    def test_msys_conversion_uses_host_checks(self):
        with mock.patch.object(bundle, 'native_env', return_value={'PATH': 'private'}) as env, mock.patch.object(
                bundle, 'msys_path', side_effect=lambda p: '/mapped/' + p) as convert, mock.patch.object(
                    ib.subprocess, 'run') as run:
            ib_run(['tool', 'source', 'output', '-pf', 'pseudo'], msys_paths=(1, 2, 4))
            env.assert_called_once_with('tool')
            self.assertEqual(convert.call_count, 3)
            self.assertEqual(run.call_args.args[0], ['tool', '/mapped/source', '/mapped/output', '-pf', '/mapped/pseudo'])

    def test_converter_local_drives(self):
        cases = (
            (r'C:\image', '/cygdrive/c/image'),
            (r'z:\folder\image', '/cygdrive/z/folder/image'),
            ('D:/folder/image', '/cygdrive/d/folder/image'),
            (r'E:\folder with spaces\image', '/cygdrive/e/folder with spaces/image'),
            ('F:\\caf\u00e9\\\u97f3.sq', '/cygdrive/f/caf\u00e9/\u97f3.sq'),
            ('G:\\folder\\', '/cygdrive/g/folder/'),
            ('H:/', '/cygdrive/h/'),
        )
        with mock.patch.object(bundle, 'native_env') as env, mock.patch.object(subprocess, 'run') as run:
            for source, expected in cases:
                with self.subTest(source=source):
                    self.assertEqual(bundle.msys_path(source), expected)
            self.assertEqual(bundle.msys_path(Path('I:/image')), '/cygdrive/i/image')
            env.assert_not_called()
            run.assert_not_called()

    def test_converter_refuses_non_local_absolute_drives(self):
        for source in (r'\\server\share\image', '//server/share/image',
                       r'\\?\C:\image', r'\\.\C:\image', r'\Device\HarddiskVolume1\image',
                       'image', 'C:image', r'\image', '/image', '/c/image',
                       '1:/image', 'C:/bad\nimage', 'C:/bad\rimage', 'C:/bad\0image'):
            with self.subTest(source=source), self.assertRaises(ValueError):
                bundle.msys_path(source)

    def test_converter_posix_unchanged(self):
        for host in ('linux', 'macos'):
            self.host.return_value = (host, 'x86_64')
            self.assertEqual(bundle.msys_path('/tmp/image'), '/tmp/image')

    def test_fixed_windows_extraction_policy(self):
        with self.archive() as archive, mock.patch('diskos_installer.buildfs.os.name', 'nt'), mock.patch(
                'diskos_installer.buildfs.os.umask') as mask, mock.patch(
                'diskos_installer.buildfs.os.geteuid', create=True) as uid:
            fs = ib.archive_extraction_fs(archive)
            self.assertEqual(archive.entries[b'f'][2], 0o755)
            self.assertEqual((fs.directory_mode, fs.file_mode), (0o755, 0o644))
            mask.assert_not_called()
            uid.assert_not_called()

    def test_complete_metadata_comparison(self):
        sq = self.work / 'out.sq'; sq.write_bytes(b'hsqs')
        raw = (b'/ D 1 755 9 8\nf R 2 755 9 8 2 0 0\nf x user.keep=abc\nh L f\n'
               b's S 3 777 9 8 f\n' + MARK + b'UI')
        with Archive(raw) as intended:
            for change in ('none', 'bytes', 'mode', 'uid', 'symlink', 'hardlink', 'missing', 'extra', 'mtime', 'xattrs'):
                with Archive(raw) as actual:
                    actual.attrs.clear()
                    for e in actual.entries.values():
                        if e[0] != b'L':
                            e[3:5] = [0, 0]
                    actual.entries[b'/'][1] = 99
                    if change == 'bytes':
                        actual._store.seek(actual._base)
                        actual._store.write(b'XX')
                    if change == 'mode': actual.entries[b'f'][2] = 0o644
                    if change == 'uid': actual.entries[b'f'][3] = 1
                    if change == 'symlink': actual.entries[b's'][5] = b'other'
                    if change == 'hardlink': actual.entries[b'h'] = actual.entries[b'f'].copy()
                    if change == 'missing': del actual.entries[b's']
                    if change == 'extra': actual.file(b'extra', b'UI')
                    if change == 'mtime': actual.entries[b'f'][1] = 99
                    if change == 'xattrs': actual.attrs[b'f'] = {b'user.keep': b'abc'}
                    with mock.patch.object(ib, '_export_archive', return_value=contextlib.nullcontext(actual)), mock.patch.object(ib, 'validate_image'):
                        args = (str(sq), 'unsq', '', 0, str(self.work))
                        if change == 'none':
                            ib_validate(*args, intended_archive=intended)
                        else:
                            with self.subTest(change=change), self.assertRaises(ib.BuildError) as caught:
                                ib_validate(*args, intended_archive=intended)
                            self.assertEqual(caught.exception.code, 'E232')

    def test_comparison_read_failure_is_e232(self):
        # Export succeeded; reading a spool while hashing the manifests fails.
        sq = self.work / 'out.sq'; sq.write_bytes(b'hsqs')
        with self.archive() as intended, self.archive() as actual:
            def broken(*a, **kw):
                raise OSError('spool read failed')
            with mock.patch.object(ib, '_export_archive', return_value=contextlib.nullcontext(actual)), \
                    mock.patch.object(ib, 'validate_image'), mock.patch.object(actual, 'chunks', side_effect=broken):
                with self.assertRaises(ib.BuildError) as caught:
                    ib_validate(str(sq), 'unsq', '', 0, str(self.work), intended_archive=intended)
        self.assertEqual(caught.exception.code, 'E232')
        self.assertIn('could not be compared', str(caught.exception))

    def test_io_failures_coded_and_destination_preserved(self):
        real_copy = ib._copyfile
        def copy_then_fail(src, dst):
            real_copy(src, dst)
            raise OSError('disk full while copying')
        cases = (
            ('edit', lambda: mock.patch.object(ib, 'edit_image', side_effect=OSError('disk full')), 'archive edit failed'),
            ('workspace', lambda: mock.patch('tempfile.TemporaryDirectory', side_effect=OSError('no temp')), 'workspace failed'),
            ('finalize', lambda: mock.patch.object(ib, '_copyfile', side_effect=copy_then_fail), 'finalizing image failed'),
        )
        for name, patcher, message in cases:
            self.out.write_bytes(b'previous image')
            with self.subTest(stage=name), patcher(), self.assertRaises(ib.BuildError) as caught:
                self.build()
            self.assertEqual(caught.exception.code, 'E230')
            self.assertIn(message, str(caught.exception))
            self.assertEqual(self.out.read_bytes(), b'previous image')
            self.assertFalse(Path(str(self.out) + '.partial').exists())


ib_run = ib._run
ib_validate = ib.validate_archive_output
