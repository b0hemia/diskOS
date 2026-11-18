# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Install/restore orchestration: cache/acquire/manual ZIP, restore planning, space budget."""
import collections
import hashlib
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import zipfile

from diskos_installer import basegate, firmware_catalogue as fw, firmware_download as dl
from diskos_installer import firmware_resolve as fr, flasher, imagebuild, state

Usage = collections.namedtuple('Usage', 'total used free')


class FakeHandle:
    def __init__(self, error=None):
        self.error, self.cancelled = error, False

    def wait(self, timeout=None):
        if self.error:
            raise self.error
        return '/fake/path.zip'

    def cancel(self):
        self.cancelled = True


class FakeSupervisor:
    def __init__(self, start_error=None, wait_error=None):
        self.calls, self.start_error, self.handle = [], start_error, FakeHandle(wait_error)

    def start(self, version, on_event=None, run_lock=None):
        self.calls.append(version)
        if self.start_error:
            raise self.start_error
        return self.handle


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        env = mock.patch.dict(os.environ, DISKOS_INSTALLER_HOME=str(self.root / 'state'))
        env.start()
        self.addCleanup(env.stop)
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, 'w') as z:
            z.writestr(zipfile.ZipInfo('firmware'), b'firmware bytes')
        self.data = buf.getvalue()
        self.pin = fw.Firmware('https://fiio-firmware.fiio.net/test.zip', len(self.data),
                               hashlib.sha256(self.data).hexdigest(), 'FiiO V2.09')
        patch = mock.patch.dict(fw.__dict__, CATALOGUE=dict(fw.CATALOGUE, **{'209': self.pin}))
        patch.start()
        self.addCleanup(patch.stop)
        from diskos_installer.__main__ import _Lock
        if os.name == 'nt':
            self.skipTest('POSIX run-lock fixture')
        self.lock = _Lock(windows=False)
        self.lock.path = str(self.root / 'run.lock')
        self.lock.__enter__()
        self.addCleanup(self.lock.__exit__, None, None, None)
        flash = mock.patch.object(flasher, 'flash', side_effect=AssertionError('flash writer called'))
        self.flash = flash.start()
        self.addCleanup(flash.stop)

    def tearDown(self):
        self.flash.assert_not_called()

    def write_zip(self, name='in.zip', data=None):
        path = self.root / name
        path.write_bytes(self.data if data is None else data)
        return str(path)


class ResolveTests(Base):
    def test_cached_valid_no_acquisition(self):
        cache = fw.store(self.write_zip(), '209', self.lock)
        sup = FakeSupervisor()
        res = fr.resolve_firmware('209', self.lock, supervisor=sup)
        self.assertEqual((res.kind, res.path), ('cached', cache))
        self.assertEqual(sup.calls, [])

    def test_cached_corrupt_reacquires_and_removes(self):
        path = fw._cache_path(self.pin)
        Path(path).write_bytes(self.data[:-1] + b'X')       # same length, wrong bytes
        sup = FakeSupervisor()
        res = fr.resolve_firmware('209', self.lock, supervisor=sup)
        self.assertEqual((res.kind, sup.calls, res.handle), ('acquiring', ['209'], sup.handle))
        self.assertFalse(os.path.exists(path))

    def test_absent_acquires(self):
        sup = FakeSupervisor()
        self.assertEqual(fr.resolve_firmware('209', self.lock, supervisor=sup).kind, 'acquiring')

    def test_unknown_version(self):
        sup = FakeSupervisor()
        with self.assertRaises(fr.UnknownVersion):
            fr.resolve_firmware('999', self.lock, supervisor=sup)
        self.assertEqual(sup.calls, [])

    def test_typed_errors_propagate_unchanged(self):
        err = dl.Cancelled('Firmware download cancelled')
        with self.assertRaises(dl.Cancelled) as c:
            fr.resolve_firmware('209', self.lock, supervisor=FakeSupervisor(start_error=err))
        self.assertIs(c.exception, err)
        err2 = dl.HttpStatusError(503)
        res = fr.resolve_firmware('209', self.lock, supervisor=FakeSupervisor(wait_error=err2))
        with self.assertRaises(dl.HttpStatusError) as c:
            res.handle.wait()
        self.assertIs(c.exception, err2)


class ManualZipTests(Base):
    def test_right_version(self):
        path = self.write_zip()
        self.assertEqual(fr.identify_zip(path, expected='209'), fr.ZipIdentity('209', path))
        self.assertEqual(fr.identify_zip(path).version, '209')

    def test_wrong_version(self):
        path = self.write_zip()
        with self.assertRaises(fr.WrongVersion) as c:
            fr.identify_zip(path, expected='240')
        self.assertIn('V2.09', c.exception.message)
        self.assertIn('V2.40', c.exception.message)
        self.assertEqual(Path(path).read_bytes(), self.data)

    def test_unknown_hash_same_size(self):
        path = self.write_zip(data=self.data[:-1] + b'X')
        with self.assertRaises(fr.UnknownFirmware):
            fr.identify_zip(path)
        self.assertTrue(os.path.exists(path))

    def test_unknown_size_and_missing(self):
        with self.assertRaises(fr.UnknownFirmware):
            fr.identify_zip(self.write_zip(data=b'short'))
        with self.assertRaises(fr.UnknownFirmware):
            fr.identify_zip(str(self.root / 'nope.zip'))


class RestoreTests(Base):
    def stage(self, mver='240', record=True, commit=True, payload=b'hsqs' + b'\0' * 64):
        src = self.root / 'img.bin'
        src.write_bytes(payload)
        manifest = {'main_os_ver': mver} if record else {}
        gen, _ = state.stage_stock_generation(str(src), manifest)
        if commit:
            state.promote_generation(gen)
        return gen

    def snapshot(self):
        root = Path(state.state_dir()) / 'stock'
        return {str(p.relative_to(root)): p.read_bytes() for p in sorted(root.rglob('*')) if p.is_file()}

    def plan(self, version=None, image_ver='240'):
        before = self.snapshot()
        with mock.patch.object(imagebuild, 'validate_stock_rootfs',
                               return_value=image_ver) as self.validate:
            try:
                return fr.plan_restore(version)
            finally:
                self.assertEqual(self.snapshot(), before)

    def test_valid_backup_used(self):
        self.stage()
        plan = self.plan('240')
        self.assertEqual((plan.action, plan.version), ('backup', '240'))
        self.assertEqual(plan.backup_path, state.stock_paths()[0])
        self.assertEqual(self.validate.call_args[0][0], state.stock_paths()[0])
        self.assertEqual(self.plan().action, 'backup')
        self.assertEqual(self.validate.call_args[0][0], state.stock_paths()[0])

    def test_missing_backup(self):
        plan = self.plan('257')
        self.assertEqual((plan.action, plan.version), ('firmware', '257'))
        self.assertIn('no saved', plan.reason)
        with self.assertRaises(fr.FirmwareResolveError):
            self.plan()

    def tree(self):
        out = set()
        for base, dirs, files in os.walk(self.root):
            out.update(os.path.join(base, n) + '/' for n in dirs)
            out.update(os.path.join(base, n) for n in files)
        return out

    def test_fresh_state_planning_creates_nothing(self):
        before = self.tree()
        with mock.patch.object(imagebuild, 'validate_stock_rootfs') as validate:
            plan = fr.plan_restore('257')
            with self.assertRaises(fr.FirmwareResolveError):
                fr.plan_restore()
        self.assertEqual(self.tree(), before)
        self.assertFalse(os.path.exists(os.path.join(state._state_base(), 'stock')))
        self.assertEqual((plan.action, plan.version, plan.reason),
                         ('firmware', '257', 'no saved stock backup'))
        validate.assert_not_called()

    def test_damaged_backup_bytes(self):
        self.stage()
        Path(state.stock_paths()[0]).write_bytes(b'hsqs' + b'\1' * 64)
        plan = self.plan('240')
        self.assertEqual(plan.action, 'firmware')
        self.assertIn('checksum', plan.reason)

    def test_damaged_pointer(self):
        self.stage()
        (Path(state.state_dir()) / 'stock' / 'current.json').write_text('{')
        self.assertIn('damaged', self.plan('240').reason)

    def test_not_a_valid_image(self):
        self.stage()
        with mock.patch.object(imagebuild, 'validate_stock_rootfs',
                               side_effect=imagebuild.BuildError('bad', code='E224')) as validate:
            self.assertEqual(fr.plan_restore('240').action, 'firmware')
        self.assertEqual(validate.call_args[0][0], state.stock_paths()[0])

    def test_backup_for_another_version(self):
        self.stage()
        plan = self.plan('257')
        self.assertEqual((plan.action, plan.version), ('firmware', '257'))
        self.assertIn('V2.40', plan.reason)

    def test_record_disagrees_with_image(self):
        self.stage(mver='228')
        self.assertIn('disagrees', self.plan('240').reason)

    def test_unsupported_version(self):
        self.stage()
        with mock.patch.dict(basegate.CATALOGUE, clear=True):
            self.assertIn('unsupported', self.plan('240', image_ver='240').reason)


class SpaceTests(Base):
    # check_space: the state tree and the system temp directory are measured separately.
    def dirs(self):
        state_dir, temp_dir = self.root / 'st', self.root / 'tmpdir'
        state_dir.mkdir()
        temp_dir.mkdir()
        return str(state_dir), str(temp_dir)

    def separate_volumes(self, state_dir, temp_dir):
        real = os.stat
        devs = {state_dir: 101, temp_dir: 202}

        def fake(path, *a, **k):
            if os.fspath(path) in devs:
                fields = list(real(path, *a, **k))
                fields[2] = devs[os.fspath(path)]       # st_dev
                return os.stat_result(fields)
            return real(path, *a, **k)
        return mock.patch.object(fr.os, 'stat', side_effect=fake)

    def check(self, state_dir, temp_dir, state_free, temp_free):
        free = {state_dir: state_free, temp_dir: temp_free}
        return fr.check_space('209', where=state_dir, temp_root=temp_dir,
                              usage=lambda p: Usage(1, 1, free[p]))

    def test_shortfall_reports_both_numbers_and_touches_nothing(self):
        src = self.root / 'img.bin'
        src.write_bytes(b'hsqs')
        state.save_stock_image(str(src))
        before = sorted(p.name for p in (Path(state.state_dir()) / 'stock').rglob('*'))
        need = fr.space_needed('209')
        state_dir, temp_dir = self.dirs()
        with self.separate_volumes(state_dir, temp_dir):
            with self.assertRaises(fr.InsufficientSpace) as c:
                self.check(state_dir, temp_dir, need - 1, need)
        e = c.exception
        self.assertEqual((e.needed, e.available), (need, need - 1))
        self.assertIn('%d MB needed' % -(-need // fr.MIB), e.message)
        self.assertIn('%d MB available' % ((need - 1) // fr.MIB), e.message)
        self.assertEqual(before, sorted(p.name for p in (Path(state.state_dir()) / 'stock').rglob('*')))

    def test_enough(self):
        need = fr.space_needed('209')
        state_dir, temp_dir = self.dirs()
        with self.separate_volumes(state_dir, temp_dir):
            self.assertEqual(self.check(state_dir, temp_dir, need, fr.HEADROOM), (need, need))

    def test_budget_counts_zip_expansion_and_backup_staging(self):
        path = self.write_zip()
        small = fr.space_needed('209', path)
        floor = imagebuild.PINNED_ROOTFS['209'][1] + 3 * imagebuild.IMG_SIZE + fr.HEADROOM
        self.assertGreater(small, floor)

    def test_full_temp_volume_fails_naming_temp_path(self):
        state_dir, temp_dir = self.dirs()
        need = fr.space_needed('209')
        with self.separate_volumes(state_dir, temp_dir):
            with self.assertRaises(fr.InsufficientSpace) as c:
                self.check(state_dir, temp_dir, need * 10, fr.HEADROOM - 1)
        e = c.exception
        self.assertEqual((e.where, e.needed, e.available), (temp_dir, fr.HEADROOM, fr.HEADROOM - 1))
        self.assertIn(temp_dir, e.message)

    def test_one_filesystem_combines_requirements(self):
        state_dir, temp_dir = self.dirs()
        self.assertEqual(os.stat(state_dir).st_dev, os.stat(temp_dir).st_dev)
        need = fr.space_needed('209')
        total = need + fr.HEADROOM
        for free in (need, total - 1):          # >= larger single need, < the sum
            with self.assertRaises(fr.InsufficientSpace) as c:
                self.check(state_dir, temp_dir, free, free)
            self.assertEqual((c.exception.needed, c.exception.available), (total, free))
        self.assertEqual(self.check(state_dir, temp_dir, total, total), (total, total))

    def test_different_filesystems_checked_against_own_need(self):
        state_dir, temp_dir = self.dirs()
        need = fr.space_needed('209')
        with self.separate_volumes(state_dir, temp_dir):
            self.assertEqual(self.check(state_dir, temp_dir, need, fr.HEADROOM), (need, need))
            with self.assertRaises(fr.InsufficientSpace) as c:
                self.check(state_dir, temp_dir, need - 1, fr.HEADROOM * 10)
        self.assertEqual((c.exception.where, c.exception.needed), (state_dir, need))

    def test_windows_archive_copies_budget_in_temp(self):
        state_dir, temp_dir = self.dirs()
        need = fr.space_needed('209')
        temp_free = fr.HEADROOM + fr.MIB
        with self.separate_volumes(state_dir, temp_dir):
            self.assertEqual(self.check(state_dir, temp_dir, need, temp_free), (need, need))
            with mock.patch.object(fr.sys, 'platform', 'win32'):
                with self.assertRaises(fr.InsufficientSpace) as c:
                    self.check(state_dir, temp_dir, need, temp_free)
                want = fr.HEADROOM + 3 * fr.UNPACKED_BUDGET
                self.assertEqual((c.exception.where, c.exception.needed), (temp_dir, want))
                self.assertEqual(self.check(state_dir, temp_dir, need, want), (need, need))

    def test_no_flash_import(self):
        self.assertNotIn('flasher', Path(fr.__file__).read_text().replace('flasher.flash', ''))


if __name__ == '__main__':
    unittest.main()
