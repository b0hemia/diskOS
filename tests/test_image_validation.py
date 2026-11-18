# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Shared output checks and independent complete archive export failures."""
import os
from pathlib import Path
import struct
import stat
import sys
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

from diskos_installer import bundle, imagebuild as ib
from diskos_installer.buildfs import ArchiveFS, archive_extraction_fs
from diskos_installer.pseudofs import Archive, MARK
import test_image_edits as edits
from test_image_edits import BOOT, CFG, FLASH_ID


class ValidationTests(unittest.TestCase):
    setUp = edits.EditTests.setUp
    backends = edits.EditTests.backends
    link = edits.EditTests.link

    def prepared(self, cfg=None):
        systems = self.backends()
        for fs in systems:
            fs.replace_file(b'ui-input', b'UI')
            # Keep synthetic builds small; UI ELF qualification is a separate gate.
            with tempfile.TemporaryDirectory(dir=os.environ.get('DISKOS_WIN_SCRATCH')) as td:
                ui = Path(td, 'ui'); ui.write_bytes(b'UI')
                expected = ib.edit_image(fs, str(ui), 'public', cfg, flash_id=FLASH_ID)
            yield fs, expected

    def validate(self, fs, expected, cfg=None):
        boot, bb, sha, size = expected
        if not isinstance(fs, ArchiveFS):
            return ib.validate_image(fs, sha, size, bb, boot, cfg, flash_id=FLASH_ID)
        with tempfile.TemporaryDirectory(dir=os.environ.get('DISKOS_WIN_SCRATCH')) as td:
            sq = Path(td, 'out.sq'); sq.write_bytes(b'hsqs')
            def export(cmd, **kw):
                Path(cmd[cmd.index('-pf') + 1]).write_bytes(fs.archive.dump())
                return SimpleNamespace(returncode=0, stderr='')
            with mock.patch.object(ib, '_run', side_effect=export):
                ib.validate_archive_output(str(sq), 'unsq', sha, size, td, bb, boot, cfg, flash_id=FLASH_ID)

    def test_shared_content_cases(self):
        paths = [BOOT, b'opt/diskos/mq_ui', b'etc/diskos_manifest', b'bin/busybox',
                 b'bin/rm', b'opt/diskos/bin/rm']
        self.assertIn(("opt/diskos/bin/diskos-flashstate", "diskos-flashstate"),
                      ib._BOOT_DECISION_FILES)
        paths += [rel.encode() for rel, _ in ib._BOOT_DECISION_FILES + ib._SUPPORT_FILES + ib._NAMES_FILES]
        for path in paths:
            for change in ('missing', 'bytes', 'symlink', 'mode', 'size'):
                if change == 'size' and path != b'opt/diskos/mq_ui':
                    continue
                if change == 'mode' and path in (BOOT, b'etc/diskos_manifest', b'bin/busybox'):
                    continue  # These retain the existing content-only contract.
                for fs, expected in self.prepared():
                    with self.subTest(path=path, change=change, backend=type(fs).__name__):
                        self.validate(fs, expected)
                        before = fs.read_bytes(path)
                        fs.unlink(path)
                        if change == 'bytes':
                            fs.replace_file(path, b'X' * len(before), 0o755)
                        elif change == 'size':
                            fs.replace_file(path, before + b'X', 0o755)
                        elif change == 'mode':
                            fs.replace_file(path, before, 0o644)
                        elif change == 'symlink':
                            target = b'/outside'
                            if path not in (BOOT, b'opt/diskos/mq_ui', b'etc/diskos_manifest'):
                                fs.replace_file(b'copy', before, 0o755)
                                target = b'../' * path.count(b'/') + b'copy'
                            self.link(fs, path, target)
                        with self.assertRaises(ib.BuildError) as caught:
                            self.validate(fs, expected)
                        self.assertEqual(caught.exception.code, 'E232')
        for path in (b'etc/diskos-ota/root.pub.pem', b'etc/diskos-ota/epochs', b'etc/diskos-ota/diskos-verify.sh'):
            for change in ('missing', 'bytes', 'mode', 'symlink'):
                for fs, expected in self.prepared(CFG):
                    before = fs.read_bytes(path); fs.unlink(path)
                    if change == 'bytes':
                        fs.replace_file(path, b'bad', 0o644)
                    elif change == 'mode':
                        fs.replace_file(path, before, 0o755)
                    elif change == 'symlink':
                        fs.replace_file(b'copy', before, 0o644)
                        self.link(fs, path, b'../../copy')
                    with self.subTest(path=path, change=change), self.assertRaises(ib.BuildError) as caught:
                        self.validate(fs, expected, CFG)
                    self.assertEqual(caught.exception.code, 'E232')
        for fs, expected in self.prepared():
            fs.mkdir(b'etc/diskos-ota')
            with self.assertRaises(ib.BuildError) as caught:
                self.validate(fs, expected)
            self.assertEqual(caught.exception.code, 'E232')

    def test_export_failures_and_complete_reads(self):
        for fs, expected in self.prepared():
            if not isinstance(fs, ArchiveFS):
                continue
            fs.replace_file(b'unrelated', b'entire-file-must-be-read')
            raw = fs.archive.dump()
            temp = tempfile.TemporaryDirectory(dir=os.environ.get('DISKOS_WIN_SCRATCH'))
            self.addCleanup(temp.cleanup)
            sq = Path(temp.name, 'out.sq'); sq.write_bytes(b'hsqs')
            def exporter(cmd, **kw):
                Path(cmd[cmd.index('-pf') + 1]).write_bytes(raw)
                return SimpleNamespace(returncode=0, stderr='')
            boot, bb, sha, size = expected
            args = (str(sq), 'unsquashfs', sha, size, temp.name, bb, boot)
            with mock.patch.object(ib, '_run', side_effect=exporter):
                ib.validate_archive_output(*args, flash_id=FLASH_ID)
            for failure in (FileNotFoundError('tool'), subprocess.TimeoutExpired('tool', 1),
                            SimpleNamespace(returncode=1, stderr='failed'), b'', raw[:-1]):
                def broken(cmd, **kw):
                    if isinstance(failure, Exception):
                        raise failure
                    if isinstance(failure, bytes):
                        Path(cmd[cmd.index('-pf') + 1]).write_bytes(failure)
                        return SimpleNamespace(returncode=0, stderr='')
                    return failure
                with mock.patch.object(ib, '_run', side_effect=broken):
                    with self.assertRaises(ib.BuildError) as caught:
                        ib.validate_archive_output(*args, flash_id=FLASH_ID)
                    self.assertEqual(caught.exception.code, 'E232')
            real = Archive.chunks
            def unreadable(archive, off, size):
                if size == len(b'entire-file-must-be-read'):
                    raise OSError('unrelated file read failed')
                yield from real(archive, off, size)
            with mock.patch.object(ib, '_run', side_effect=exporter), mock.patch.object(Archive, 'chunks', unreadable):
                with self.assertRaises(ib.BuildError) as caught:
                    ib.validate_archive_output(*args, flash_id=FLASH_ID)
                self.assertEqual(caught.exception.code, 'E232')
            with mock.patch.object(ib.os.path, 'getsize', return_value=ib.IMG_SIZE + 1):
                with self.assertRaises(ib.BuildError) as caught:
                    ib.validate_archive_output(*args, flash_id=FLASH_ID)
                self.assertEqual(caught.exception.code, 'E231')

    def test_real_export_corruption_and_probe(self):
        if os.name == 'nt':
            if not os.environ.get('DISKOS_SQUASHFS_BIN'):
                self.skipTest('Windows native tools require adjacent-DLL DISKOS_SQUASHFS_BIN')
            location = os.environ.get('DISKOS_WINDOWS_TEST_HELPERS')
            if not location:
                self.skipTest('Set DISKOS_WINDOWS_TEST_HELPERS to the directory containing test_pseudofile_roundtrip.py')
            windows_tests = Path(location)
            if not (windows_tests / 'test_pseudofile_roundtrip.py').is_file():
                self.skipTest('Windows native test helper not present')
            with mock.patch.object(sys, 'path', [str(windows_tests), *sys.path]):
                from test_pseudofile_roundtrip import require_unprivileged, TOOLS
            require_unprivileged()
            mksq, unsq = (str(TOOLS / (name + '.exe')) for name in ('mksquashfs', 'unsquashfs'))
        else:
            mksq, unsq = bundle.native('mksquashfs'), bundle.native('unsquashfs')
        with tempfile.TemporaryDirectory(dir=os.environ.get('DISKOS_WIN_SCRATCH')) as td:
            ib.check_archive_squashfs_tools(mksq, unsq, td)
            src = Path(td, 'src'); src.mkdir()
            Path(src, 'data').write_bytes(bytes(range(256)) * 4096)
            sq = Path(td, 'out.sq')
            result = ib._run([mksq, str(src), str(sq), '-comp', 'lzo', '-b', '131072',
                              '-noappend', '-no-xattrs', '-processors', '1'],
                             msys_paths=(1, 2), capture_output=True)
            self.assertEqual(result.returncode, 0)
            original = sq.read_bytes()
            used = struct.unpack_from('<Q', original, 40)[0]
            for pos, length in ((0, 4), (len(original) // 2, 256), (used - 64, 64), (used // 2, len(original))):
                sq.write_bytes(original[:pos] + (b'\0' * length if length != len(original) else b'') + original[pos + length:])
                with self.subTest(pos=pos), self.assertRaises(ib.BuildError) as caught:
                    if pos == 0:
                        ib.validate_archive_output(str(sq), unsq, '', 0, td)
                    else:
                        with ib._export_archive(str(sq), unsq, td):
                            pass
                self.assertEqual(caught.exception.code, 'E232')

            # Repack an edited image too: the identity contract must survive real
            # pseudo-file serialization, LZO packing and independent export.
            for fs, expected in self.prepared():
                if not isinstance(fs, ArchiveFS):
                    continue
                empty = Path(td, 'empty'); empty.mkdir()
                pf = Path(td, 'edited.pf'); pf.write_bytes(fs.archive.dump())
                image = Path(td, 'identity.sq')
                result = ib._run([mksq, str(empty), str(image), '-pf', str(pf),
                                  '-comp', 'lzo', '-b', '131072', '-noappend',
                                  '-no-xattrs', '-all-root', '-processors', '1'],
                                 msys_paths=(1, 2, 4), capture_output=True)
                self.assertEqual(result.returncode, 0)
                boot, bb, sha, size = expected
                ib.validate_archive_output(str(image), unsq, sha, size, td, bb, boot,
                                           intended_archive=fs.archive, flash_id=FLASH_ID)
                with ib._export_archive(str(image), unsq, td) as output:
                    identity = ArchiveFS(output)
                    self.assertEqual(identity.read_bytes(b'etc/diskos-flash-id'), FLASH_ID)
                    info = identity.lstat(b'etc/diskos-flash-id')
                    self.assertTrue(stat.S_ISREG(info.st_mode))
                    self.assertEqual(stat.S_IMODE(info.st_mode), 0o644)
                with self.assertRaises(ib.BuildError) as caught:
                    ib.validate_archive_output(str(image), unsq, sha, size, td, bb, boot,
                                               flash_id=b'f' * 32 + b'\n')
                self.assertEqual(caught.exception.code, 'E232')

    def test_policy_and_capability_failures(self):
        for result in (FileNotFoundError('tool'), SimpleNamespace(returncode=1, stderr='bad')):
            with mock.patch.object(ib, '_run', side_effect=result if isinstance(result, Exception) else None, return_value=result):
                with self.assertRaises(ib.BuildError) as caught:
                    ib.check_archive_squashfs_tools('mksq', 'unsq', os.environ.get('DISKOS_WIN_SCRATCH'))
                self.assertEqual(caught.exception.code, 'E104')

    def test_extraction_modes(self):
        cases = ((False, 0o022),) if os.name == 'nt' else (
            (False, 0o077), (False, 0o022), (True, 0o077), (True, 0o022))
        for root, mask in cases:
            with self.subTest(root=root, mask=oct(mask)), Archive(
                    b'/ D 1 2755 0 0\nd D 1 3770 0 0\n'
                    b'f R 1 6755 0 0 0 0 0\ns R 1 7755 0 0 0 0 0\n' + MARK) as archive:
                with mock.patch('diskos_installer.buildfs.os.geteuid',
                                return_value=0 if root else 1000, create=True), mock.patch(
                                    'diskos_installer.buildfs.os.umask', return_value=mask):
                    fs = archive_extraction_fs(archive)
                for path, want in ((b'/', 0o2755 if root else 0o755),
                                   (b'd', 0o3770 if root else 0o1770),
                                   (b'f', 0o6755 if root else 0o755 & ~mask),
                                   (b's', 0o7755 if root else 0o1755)):
                    self.assertEqual(archive.entries[path][2], want)
                fs.mkdir(b'new/child')
                fs.replace_file(b'other/file', b'new bytes')
                for path in (b'new', b'new/child', b'other'):
                    self.assertEqual(archive.entries[path][2], 0o777 & ~mask)
                self.assertEqual(archive.entries[b'other/file'][2], 0o666 & ~mask)

    def test_probe_byte_mismatch_message(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get('DISKOS_WIN_SCRATCH')) as td:
            with Archive(b'/ D 1 755 0 0\n' + MARK) as archive:
                archive.file(b'probe.bin', b'wrong bytes')
                raw = archive.dump()
            def export(cmd, **kw):
                if '-pf' in cmd and cmd[0] == 'unsq':
                    Path(cmd[cmd.index('-pf') + 1]).write_bytes(raw)
                return SimpleNamespace(returncode=0, stderr='')
            with mock.patch.object(ib, '_run', side_effect=export):
                with self.assertRaises(ib.BuildError) as caught:
                    ib.check_archive_squashfs_tools('mksq', 'unsq', td)
            self.assertEqual(caught.exception.code, 'E104')
            self.assertEqual(str(caught.exception),
                             '[E104] squashfs LZO pseudo capability check failed: '
                             'pack/export round-trip bytes differ - repair/rebuild squashfs tools '
                             'with working LZO and pseudo-file support')

    def test_stock_policy(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get('DISKOS_WIN_SCRATCH')) as td:
            sq = Path(td, 'stock.sq'); sq.write_bytes(b'hsqs')
            for version, product, code, override, allow in (
                    ('257', 'OTHER', 'E220', False, True),
                    ('999', 'SNOWSKY_DISC', 'E221', False, True),
                    ('257', 'SNOWSKY_DISC', 'E224', False, True),
                    ('999', 'SNOWSKY_DISC', None, True, True),
                    ('999', 'SNOWSKY_DISC', 'E221', True, False),
                    ('257', 'SNOWSKY_DISC', None, True, True)):
                with Archive(b'/ D 1 755 0 0\n' + MARK) as archive:
                    fs = ArchiveFS(archive)
                    fs.replace_file(b'etc/product_version/version.in',
                                    f'PRODUCT={product}\nMAIN_OS_VER={version}\n'.encode())
                    raw = archive.dump()
                def export(cmd, **kw):
                    Path(cmd[cmd.index('-pf') + 1]).write_bytes(raw)
                    return SimpleNamespace(returncode=0, stderr='')
                with mock.patch.object(ib, '_run', side_effect=export), mock.patch.dict(os.environ, {'DISKOS_ALLOW_UNTESTED_FW': '1' if override else '0'}):
                    if code:
                        with self.assertRaises(ib.BuildError) as caught:
                            ib.validate_stock_archive(str(sq), 'unsq', td, allow_override=allow)
                        self.assertEqual(caught.exception.code, code)
                    else:
                        self.assertEqual(ib.validate_stock_archive(str(sq), 'unsq', td, allow_override=allow), version)
            raw = b'/ D 1 755 0 0\n' + MARK
            with mock.patch.object(ib, '_run', side_effect=export):
                with self.assertRaises(ib.BuildError) as caught:
                    ib.validate_stock_archive(str(sq), 'unsq', td)
                self.assertEqual(caught.exception.code, 'E230')
                self.assertEqual(str(caught.exception),
                                 "[E230] could not read version.in from the stock rootfs: "
                                 "b'etc/product_version/version.in'")
            with mock.patch.object(ib, '_run', side_effect=FileNotFoundError('tool')):
                with self.assertRaises(ib.BuildError) as caught:
                    ib.validate_stock_archive(str(sq), 'unsq', td)
                self.assertEqual(caught.exception.code, 'E230')
            sq.write_bytes(b'bad!')
            with self.assertRaises(ib.BuildError) as caught:
                ib.validate_stock_archive(str(sq), 'unsq', td)
            self.assertEqual(caught.exception.code, 'E220')


class FlashIdentityValidationTests(unittest.TestCase):
    setUp = ValidationTests.setUp
    backends = ValidationTests.backends
    link = ValidationTests.link
    prepared = ValidationTests.prepared
    validate = ValidationTests.validate

    def test_missing_malformed_and_different_identity(self):
        cases = (None, b'', b'a' * 31 + b'\n', b'a' * 33 + b'\n',
                 FLASH_ID.rstrip(b'\n'), FLASH_ID + b'\n', FLASH_ID[:-1] + b'\r\n',
                 FLASH_ID.upper(), b'g' * 32 + b'\n', b'0' * 32 + b'\n',
                 b' ' + FLASH_ID, FLASH_ID + b'\0', b'\xff' * 32 + b'\n')
        for cfg in (None, CFG):
            for bad in cases:
                for fs, expected in self.prepared(cfg):
                    with self.subTest(backend=type(fs).__name__, identity=bad, ota=bool(cfg)):
                        self.validate(fs, expected, cfg)
                        fs.unlink(b'etc/diskos-flash-id')
                        if bad is not None:
                            fs.replace_file(b'etc/diskos-flash-id', bad, 0o644)
                        with self.assertRaises(ib.BuildError) as caught:
                            self.validate(fs, expected, cfg)
                        self.assertEqual(caught.exception.code, 'E232')
                        self.assertIn('flash identity', str(caught.exception))

    def test_identity_type_and_mode(self):
        path = b'etc/diskos-flash-id'
        for change in ('directory', 'symlink', 'outside', 0o600, 0o444, 0o755, 0o664, 0o4644):
            for fs, expected in self.prepared():
                with self.subTest(backend=type(fs).__name__, change=change):
                    fs.unlink(path)
                    if change == 'directory':
                        fs.mkdir(path)
                    elif change in ('symlink', 'outside'):
                        fs.replace_file(b'copy', FLASH_ID, 0o644)
                        self.link(fs, path, b'../copy' if change == 'symlink' else b'/outside')
                    else:
                        fs.replace_file(path, FLASH_ID, change)
                    with self.assertRaises(ib.BuildError) as caught:
                        self.validate(fs, expected)
                    self.assertEqual(caught.exception.code, 'E232')

    def test_validation_requires_external_expected_identity(self):
        for fs, expected in self.prepared():
            boot, bb, sha, size = expected
            for identity in (None, b'', FLASH_ID.decode(), b'X' * 32 + b'\n',
                             b'0' * 32 + b'\n'):
                with self.subTest(backend=type(fs).__name__, identity=identity):
                    with self.assertRaises(ib.BuildError) as caught:
                        ib.validate_image(fs, sha, size, bb, boot, flash_id=identity)
                    self.assertEqual(caught.exception.code, 'E232')
            # Validation must never generate a replacement or repair the output.
            import secrets
            with mock.patch.object(secrets, 'token_hex', side_effect=AssertionError('unexpected generation')):
                self.validate(fs, expected)
            self.assertEqual(fs.read_bytes(b'etc/diskos-flash-id'), FLASH_ID)
            self.assertEqual(stat.S_IMODE(fs.lstat(b'etc/diskos-flash-id').st_mode), 0o644)
