# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""One edit phase on both filesystems, including hostile image namespaces."""
import contextlib
import hashlib
import os
from pathlib import Path
from types import SimpleNamespace
import stat
import shutil
import tempfile
import unittest
from unittest import mock

from diskos_installer import bundle, imagebuild as ib
from diskos_installer.buildfs import ArchiveFS, TreeFS
from diskos_installer.pseudofs import Archive, MARK

CLOCK = 1791417600
BOOT = b'usr/project/fiio_init.sh'
RAW = ib._LAUNCH.encode('utf-8')
FLASH_ID = b'0123456789abcdef0123456789abcdef\n'
CFG = {'rootpub': b'offline-test-key\n', 'epochs': b'MIN_KEY_EPOCH=1\nMIN_UI_EPOCH=1\n'}


class EditTests(unittest.TestCase):
    def setUp(self):
        # Minimal script, explicitly test-pinned; the full comparison uses the real pin.
        self.pin = mock.patch.object(ib, 'KNOWN_FIIO_INIT_SHA256', {hashlib.sha256(RAW).hexdigest()})
        self.pin.start()
        self.addCleanup(self.pin.stop)
        if os.name != 'nt':
            mask = os.umask(0o022)
            self.addCleanup(os.umask, mask)

    def backends(self, busybox=True):
        temp = tempfile.TemporaryDirectory(prefix='s3-edits-', dir=os.environ.get('DISKOS_WIN_SCRATCH'))
        self.addCleanup(temp.cleanup)
        root = Path(temp.name) / 'tree'
        root.mkdir()
        archive = Archive(b'/ D 1 755 0 0\n' + MARK)
        self.addCleanup(archive.close)
        systems = [ArchiveFS(archive, clock=CLOCK)]
        if os.name != 'nt':
            systems.append(TreeFS(root, clock=CLOCK))
        for fs in systems:
            fs.replace_file(BOOT, RAW, 0o751)
            fs.replace_file(b'etc/init.d/S50avahi-daemon', ib._STOCK_AVAHI, 0o755)
            if busybox:
                fs.replace_file(b'bin/busybox', b'busybox', 0o755)
        return systems

    def link(self, fs, path, target, hard=False):
        # The stock Avahi fixture creates init.d; these tests replace that parent.
        if path == b'etc/init.d':
            fs.unlink(b'etc/init.d/S50avahi-daemon')
            if isinstance(fs, ArchiveFS):
                del fs.archive.entries[path]
            else:
                fs._path(path).rmdir()
        if isinstance(fs, ArchiveFS):
            if hard:
                fs.archive.entries[path] = [b'L', target]
            else:
                fs.archive.symlink(path, target)
        elif hard:
            os.link(fs._path(target), fs._path(path))
        else:
            fs._path(path).symlink_to(os.fsdecode(target))

    def run_edit(self, fs, variant='public', cfg=None, flash_id=FLASH_ID):
        return ib.edit_image(fs, bundle.data('mq_ui'), variant, cfg, flash_id=flash_id)

    def error(self, fs, code, pattern=None, **kw):
        with self.assertRaises(ib.BuildError) as caught:
            self.run_edit(fs, **kw)
        self.assertEqual(caught.exception.code, code)
        if pattern:
            self.assertRegex(str(caught.exception), pattern)

    def test_variants_and_ota(self):
        for variant in ('public', 'dev'):
            for cfg in (None, CFG):
                for fs in self.backends():
                    with self.subTest(backend=type(fs).__name__, variant=variant, ota=bool(cfg)):
                        # Case collisions and a contained symlink parent remain distinct/safe.
                        fs.replace_file(b'Case', b'upper')
                        fs.replace_file(b'case', b'lower')
                        fs.mkdir(b'hooks')
                        fs.mkdir(b'etc')
                        self.link(fs, b'etc/init.d', b'../hooks')
                        fs.replace_file(b'etc/init.d/S50avahi-daemon', ib._STOCK_AVAHI, 0o755)
                        fs.replace_file(b'etc/init.d/S96diskos_select', b'old', 0o4777)
                        self.link(fs, b'bin/rm', b'/outside')
                        patched, bb, sha, size = self.run_edit(fs, variant, cfg)
                        self.assertEqual(fs.read_bytes(BOOT), patched)
                        self.assertEqual(stat.S_IMODE(fs.stat(BOOT).st_mode), 0o751)
                        self.assertEqual(bb, hashlib.sha256(b'busybox').hexdigest())
                        self.assertIn(("opt/diskos/bin/diskos-flashstate", "diskos-flashstate"),
                                      ib._BOOT_DECISION_FILES)
                        copies = (*ib._BOOT_DECISION_FILES, *ib._SUPPORT_FILES,
                                  ('bin/rm', 'diskos-rmguard'), ('opt/diskos/bin/rm', 'diskos-rmguard'),
                                  ('opt/diskos/mq_ui', 'mq_ui'), ('usr/project/dropbearmulti', 'dropbearmulti'),
                                  ('usr/project/diskos-debug.sh', 'diskos-debug.sh'))
                        for rel, name in copies:
                            self.assertEqual(fs.read_bytes(rel.encode()), Path(bundle.data(name)).read_bytes())
                            self.assertEqual(stat.S_IMODE(fs.lstat(rel.encode()).st_mode), 0o755)
                        self.assertEqual(sha, hashlib.sha256(fs.read_bytes(b'opt/diskos/mq_ui')).hexdigest())
                        self.assertEqual(fs.read_bytes(b'etc/diskos_manifest'),
                                         f'SHA256={sha}\nSIZE={size}\nARCH=mips-le\nVARIANT={variant}\nBUILT=2026-10-08T00:00:00Z\n'.encode())
                        if cfg:
                            for name, mode in ib._OTA_FILES:
                                path = b'etc/diskos-ota/' + name.encode()
                                want = Path(bundle.data(name)).read_bytes() if name == 'diskos-verify.sh' else cfg['rootpub' if name == 'root.pub.pem' else 'epochs']
                                self.assertEqual(fs.read_bytes(path), want)
                                self.assertEqual(stat.S_IMODE(fs.lstat(path).st_mode), mode)
                        else:
                            with self.assertRaises(FileNotFoundError):
                                fs.lstat(b'etc/diskos-ota')
                        if variant == 'dev':
                            self.assertEqual(fs.read_bytes(b'etc/init.d/S99usbserial'), Path(bundle.data('S99usbserial')).read_bytes())
                        else:
                            with self.assertRaises(FileNotFoundError):
                                fs.lstat(b'etc/init.d/S99usbserial')
                        self.assertEqual((fs.read_bytes(b'Case'), fs.read_bytes(b'case')), (b'upper', b'lower'))

    def test_error_codes_and_boot_refusals(self):
        for raw, pattern in ((b'unknown\r\n', 'not the stock'),
                             (RAW + b'diskOS override', 'already contains'),
                             (b'no anchor', 'not found'), (RAW * 2, 'ambiguous')):
            for fs in self.backends():
                fs.replace_file(BOOT, raw)
                pins = set() if pattern == 'not the stock' else {hashlib.sha256(raw).hexdigest()}
                with mock.patch.object(ib, 'KNOWN_FIIO_INIT_SHA256', pins):
                    self.error(fs, 'E223', pattern)
                self.assertEqual(fs.read_bytes(BOOT), raw)
        for fs in self.backends():
            self.error(fs, 'E250', variant='other')
            self.assertEqual(fs.read_bytes(BOOT), RAW)

    def test_missing_payload_is_preflight(self):
        # A controlled payload folder with one file removed: the real bundle.data
        # lookup must raise its E102 unchanged, not a build error.
        from diskos_installer.errors import PreflightError
        ui = bundle.data('mq_ui')
        source = bundle.payload_dir()
        for missing, variant in (('S96diskos_select', 'public'), ('S99usbserial', 'dev')):
            temp = tempfile.TemporaryDirectory(prefix='s3-payload-', dir=os.environ.get('DISKOS_WIN_SCRATCH'))
            self.addCleanup(temp.cleanup)
            for name in os.listdir(source):
                if name != missing and os.path.isfile(os.path.join(source, name)):
                    Path(temp.name, name).write_bytes(Path(source, name).read_bytes())
            with mock.patch.object(bundle, 'payload_dir', return_value=temp.name):
                for fs in self.backends():
                    with self.subTest(backend=type(fs).__name__, missing=missing):
                        with self.assertRaises(PreflightError) as caught:
                            ib.edit_image(fs, ui, variant, None, flash_id=FLASH_ID)
                        self.assertNotIsInstance(caught.exception, ib.BuildError)
                        self.assertEqual(caught.exception.code, 'E102')
                        self.assertEqual(caught.exception.message, f"bundled payload '{missing}' not found")
                        self.assertEqual(caught.exception.action,
                                         'the install is incomplete - re-download the installer')

    def test_hostile_paths(self):
        for path in (BOOT, b'etc/init.d', b'etc/diskos_manifest', b'opt/diskos',
                     b'etc/diskos-ota', b'bin'):
            for target in (b'../../../../outside', b'/outside', path.rpartition(b'/')[2]):
                for fs in self.backends(busybox=path != b'bin'):
                    with self.subTest(backend=type(fs).__name__, path=path, target=target):
                        if path == BOOT:
                            fs.unlink(path)
                        fs.mkdir(path.rpartition(b'/')[0] or b'/')
                        self.link(fs, path, target)
                        self.error(fs, 'E233', cfg=CFG)
        for path in (BOOT, b'etc/diskos_manifest', b'bin/busybox', b'bin/rm'):
            for fs in self.backends():
                if path in (BOOT, b'bin/busybox'):
                    fs.unlink(path)
                fs.mkdir(path)
                self.error(fs, 'E233')
        for path in (BOOT, b'etc/diskos_manifest', b'opt/diskos/mq_ui'):
            for fs in self.backends():
                if path != BOOT:
                    fs.replace_file(path, b'sentinel')
                self.link(fs, b'alias', path, hard=True)
                before = fs.read_bytes(b'alias')
                self.error(fs, 'E233')
                self.assertEqual(fs.read_bytes(b'alias'), before)

    def test_rm_exception_and_busybox_identity(self):
        for fs in self.backends():
            self.link(fs, b'bin/rm', b'bin/busybox', hard=True)
            self.run_edit(fs)
            self.assertEqual(fs.read_bytes(b'bin/busybox'), b'busybox')
        for fs in self.backends():
            real = fs.replace_file
            def corrupt(path, source, mode=None):
                real(path, source, mode)
                if path == b'opt/diskos/bin/rm':
                    real(b'bin/busybox', b'changed')
            with mock.patch.object(fs, 'replace_file', side_effect=corrupt):
                self.error(fs, 'E233', 'BusyBox changed')
        for fs in self.backends():
            self.link(fs, b'escape', b'../outside')
            with self.assertRaises(ib.BuildError) as caught:
                ib._edit_within(fs, b'escape/file', code='E232')
            self.assertEqual(caught.exception.code, 'E232')


class FlashIdentityEditTests(unittest.TestCase):
    setUp = EditTests.setUp
    backends = EditTests.backends
    link = EditTests.link
    run_edit = EditTests.run_edit
    error = EditTests.error

    def test_identity_modes_and_replacement(self):
        for mask in (0o022, 0o077):
            for fs in self.backends():
                with self.subTest(backend=type(fs).__name__, mask=oct(mask)):
                    old_mask = os.umask(mask)
                    try:
                        fs.replace_file(b'etc/diskos-flash-id', b'old identity', 0o777)
                        self.run_edit(fs)
                    finally:
                        os.umask(old_mask)
                    info = fs.lstat(b'etc/diskos-flash-id')
                    self.assertTrue(stat.S_ISREG(info.st_mode))
                    self.assertEqual(stat.S_IMODE(info.st_mode), 0o644)
                    self.assertEqual(fs.read_bytes(b'etc/diskos-flash-id'), FLASH_ID)
                    self.assertNotIn(FLASH_ID.strip(), fs.read_bytes(b'etc/diskos_manifest'))

    def test_identity_write_refusals(self):
        path = b'etc/diskos-flash-id'
        for kind in ('escape', 'cycle', 'directory', 'hardlink'):
            for fs in self.backends():
                fs.mkdir(b'etc')
                if kind == 'directory':
                    fs.mkdir(path)
                elif kind == 'hardlink':
                    fs.replace_file(path, b'old identity')
                    self.link(fs, b'alias', path, hard=True)
                else:
                    self.link(fs, path, b'../../outside' if kind == 'escape' else b'diskos-flash-id')
                with self.subTest(backend=type(fs).__name__, kind=kind):
                    self.error(fs, 'E233')
                    if kind == 'hardlink':
                        self.assertEqual(fs.read_bytes(b'alias'), b'old identity')

    def test_bad_expected_identity_refused_before_edit(self):
        for identity in (None, b'', b'a' * 32, b'A' * 32 + b'\n',
                         b'g' * 32 + b'\n', FLASH_ID + b'\n', FLASH_ID.decode()):
            for fs in self.backends():
                with self.subTest(identity=identity, backend=type(fs).__name__):
                    self.error(fs, 'E233', flash_id=identity)
                    self.assertEqual(fs.read_bytes(BOOT), RAW)
                    with self.assertRaises(FileNotFoundError):
                        fs.lstat(b'etc/diskos-flash-id')


class FlashIdentityBuildTests(unittest.TestCase):
    setUp = EditTests.setUp

    @contextlib.contextmanager
    def build_environment(self, host, cfg, corruption=False):
        # Exercise dispatch, the real editor and independent content validation.
        # Only stock qualification and external tools use synthetic fixtures.
        with tempfile.TemporaryDirectory(prefix='c1-build-', dir=os.environ.get('DISKOS_WIN_SCRATCH')) as td:
            root = Path(td)
            ui = root / 'ui'; ui.write_bytes(b'UI')
            stock = root / 'stock.sq'; stock.write_bytes(b'hsqs')
            packed = root / 'packed'
            calls = {'edited': [], 'validated': [], 'outputs': []}
            real_edit, real_validate = ib.edit_image, ib.validate_image

            def edit(fs, *args, **kw):
                result = real_edit(fs, *args, **kw)
                calls['edited'].append(fs.read_bytes(b'etc/diskos-flash-id'))
                return result

            def validate(fs, *args, **kw):
                # validate_image receives the expected bytes positionally from wrappers.
                expected = kw.get('flash_id', args[-1])
                calls['validated'].append(expected)
                result = real_validate(fs, *args, **kw)
                calls['outputs'].append(fs.read_bytes(b'etc/diskos-flash-id'))
                return result

            def damage(fs):
                path = b'etc/diskos-flash-id'
                fs.unlink(path)
                if corruption is not None:
                    fs.replace_file(path, corruption, 0o644)

            def run(cmd, **kw):
                if cmd[0] == 'mksquashfs':
                    if '-pf' in cmd:
                        calls['archive'] = Path(cmd[cmd.index('-pf') + 1]).read_bytes()
                        if corruption is not False:
                            with Archive(calls['archive']) as archive:
                                damage(ArchiveFS(archive))
                                calls['archive'] = archive.dump()
                    else:
                        shutil.copytree(cmd[1], packed, dirs_exist_ok=True)
                        if corruption is not False:
                            damage(TreeFS(packed))
                    Path(cmd[2]).write_bytes(b'hsqs')
                elif '-pf' in cmd:
                    Path(cmd[cmd.index('-pf') + 1]).write_bytes(calls['archive'])
                else:
                    dst = Path(cmd[cmd.index('-d') + 1])
                    if Path(cmd[-1]) == stock:
                        fs = TreeFS(dst)
                        fs.replace_file(BOOT, RAW, 0o755)
                        fs.replace_file(b'etc/init.d/S50avahi-daemon', ib._STOCK_AVAHI, 0o755)
                        fs.replace_file(b'bin/busybox', b'busybox', 0o755)
                    else:
                        shutil.copytree(packed, dst)
                return SimpleNamespace(returncode=0, stderr='')

            @contextlib.contextmanager
            def export(path, *args, **kw):
                # Output export uses the real strict archive reader below.
                if Path(path) != stock:
                    with real_export(path, *args, **kw) as archive:
                        yield archive
                    return
                with Archive(b'/ D 1 755 0 0\n' + MARK) as archive:
                    fs = ArchiveFS(archive)
                    fs.replace_file(BOOT, RAW, 0o755)
                    fs.replace_file(b'etc/init.d/S50avahi-daemon', ib._STOCK_AVAHI, 0o755)
                    fs.replace_file(b'bin/busybox', b'busybox', 0o755)
                    yield archive

            real_export = ib._export_archive
            with contextlib.ExitStack() as stack:
                patches = (
                    mock.patch.object(bundle.platform_probe, 'host', return_value=(host, 'x86_64')),
                    mock.patch.object(bundle, 'native', side_effect=lambda name: name),
                    mock.patch.object(ib, 'IMG_SIZE', 4096),
                    mock.patch.object(ib, 'resolve_ota_config', return_value=cfg),
                    mock.patch.object(ib, 'check_squashfs_tools'),
                    mock.patch.object(ib, 'check_archive_squashfs_tools'),
                    mock.patch.object(ib, 'validate_stock_rootfs', return_value='257'),
                    mock.patch.object(ib, 'validate_stock_archive', return_value='257'),
                    mock.patch.object(ib, '_validate_ui_elf'),
                    mock.patch.object(ib, '_run', side_effect=run),
                    mock.patch.object(ib, '_export_archive', side_effect=export),
                    mock.patch.object(ib, 'edit_image', side_effect=edit),
                    mock.patch.object(ib, 'validate_image', side_effect=validate))
                for patch in patches:
                    stack.enter_context(patch)
                yield root, stock, ui, calls

    def test_fresh_identity_per_build_through_both_backends(self):
        import secrets
        for host in ('linux', 'windows'):
            if host == 'linux' and os.name == 'nt':
                continue  # The tree backend requires a POSIX host filesystem.
            for cfg in (None, CFG):
                for variant in ('public', 'dev'):
                    with self.subTest(host=host, ota=bool(cfg), variant=variant):
                        with self.build_environment(host, cfg) as (root, stock, ui, calls):
                            with mock.patch.object(secrets, 'token_hex', wraps=secrets.token_hex) as generate:
                                for number in range(2):
                                    work = root / str(number); work.mkdir()
                                    out = work / 'image.bin'
                                    result = ib.build_image(str(stock), str(ui), variant, str(out),
                                                            str(work), rep=mock.Mock())
                                    self.assertEqual(result, str(out))
                                    self.assertEqual(out.stat().st_size, 4096)
                                self.assertEqual(generate.call_args_list, [mock.call(16), mock.call(16)])
                            self.assertEqual(len(calls['edited']), 2)
                            self.assertEqual(calls['edited'], calls['validated'])
                            self.assertEqual(calls['edited'], calls['outputs'])
                            for identity in calls['edited']:
                                self.assertRegex(identity, rb'^[0-9a-f]{32}\n$')
                            self.assertNotEqual(*calls['edited'])

    def test_repacker_identity_corruption_prevents_finalization(self):
        for host in ('linux', 'windows'):
            if host == 'linux' and os.name == 'nt':
                continue
            for corruption in (None, b'broken\n', b'f' * 32 + b'\n'):
                with self.subTest(host=host, corruption=corruption):
                    with self.build_environment(host, None, corruption) as (root, stock, ui, calls):
                        out = root / 'image.bin'
                        # A fixed source makes the well-formed mismatch deterministic.
                        with mock.patch('secrets.token_hex', return_value='0' * 32) as generate:
                            with self.assertRaises(ib.BuildError) as caught:
                                ib.build_image(str(stock), str(ui), 'public', str(out),
                                               str(root), rep=mock.Mock())
                            generate.assert_called_once_with(16)
                        self.assertEqual(caught.exception.code, 'E232')
                        self.assertIn('flash identity', str(caught.exception))
                        self.assertFalse(out.exists())
                        self.assertFalse(Path(str(out) + '.partial').exists())
                        self.assertEqual(calls['edited'], [b'0' * 32 + b'\n'])
