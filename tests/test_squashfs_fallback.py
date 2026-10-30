"""Linux capability fallback uses a complete, checked pair for the process run."""
from pathlib import Path
import subprocess
import contextlib
import unittest
from unittest import mock

from diskos_installer import bundle, imagebuild as ib


class SquashfsFallbackTests(unittest.TestCase):
    bundled = ('/bundle/mksquashfs', '/bundle/unsquashfs')
    system = ('/system/mksquashfs', '/system/unsquashfs')

    def setUp(self):
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        def patch(target, **kwargs):
            return self.stack.enter_context(mock.patch(target, **kwargs))
        patch('diskos_installer.imagebuild._selected_squashfs_pair', new=None, create=True)
        self.host = patch('diskos_installer.bundle.platform_probe.host', return_value=('linux', 'x86_64'))
        patch('diskos_installer.bundle.is_vendored', side_effect=lambda p: p.startswith('/bundle/'))
        self.which = patch('diskos_installer.bundle._which_abs', side_effect=dict(zip(
            ('mksquashfs', 'unsquashfs'), self.system)).get)
        patch('diskos_installer.bundle.native', side_effect=dict(zip(
            ('mksquashfs', 'unsquashfs'), self.bundled)).get)
        self.run = patch('diskos_installer.imagebuild._run', side_effect=self.tool)
        self.rep = mock.Mock()
        self.system_fails = False
        self.bundled_fails = True

    def tool(self, cmd, **kwargs):
        if ((cmd[0].startswith('/bundle/') and self.bundled_fails) or
                (cmd[0].startswith('/system/') and self.system_fails)):
            return subprocess.CompletedProcess(cmd, -8, '', 'SIGFPE marker')
        if cmd[0].endswith('/mksquashfs'):
            self.payload = Path(cmd[1], 'probe.bin').read_bytes()
            Path(cmd[2]).write_bytes(b'hsqs')
        else:
            dest = Path(cmd[cmd.index('-d') + 1])
            dest.mkdir()
            (dest / 'probe.bin').write_bytes(self.payload)
        return subprocess.CompletedProcess(cmd, 0, '', '')

    def test_failed_bundle_selects_system_pair_for_later_operations(self):
        ib.check_squashfs_tools(*self.bundled, self.rep)
        self.assertEqual(ib._squashfs_tools(), self.system)
        self.assertEqual([c.args[0][0] for c in self.run.call_args_list],
                         [self.bundled[0], *self.system])
        self.assertTrue(any('system squashfs-tools pair' in c.args[0]
                            for c in self.rep.log.call_args_list))
        # Native resolution must still expose the broken vendored tools.
        self.assertEqual(bundle.native('mksquashfs'), self.bundled[0])

    def assert_original_failure(self):
        with self.assertRaises(ib.BuildError) as baseline:
            ib._probe_squashfs_tools(*self.bundled)
        self.run.reset_mock()
        with self.assertRaises(ib.BuildError) as caught:
            ib.check_squashfs_tools(*self.bundled, self.rep)
        self.assertEqual(caught.exception.code, 'E104')
        self.assertEqual(str(caught.exception), str(baseline.exception))
        self.assertEqual(caught.exception.action, baseline.exception.action)
        self.assertIn(self.bundled[0], str(caught.exception))
        self.assertIn('SIGFPE marker', str(caught.exception))
        self.assertIn('install squashfs-tools', str(caught.exception))
        self.assertIsNone(ib._selected_squashfs_pair)

    def test_system_absent(self):
        self.which.side_effect = None
        self.which.return_value = None
        self.assert_original_failure()

    def test_system_also_fails(self):
        self.system_fails = True
        self.assert_original_failure()

    def test_incomplete_system_pair_never_mixed(self):
        for missing in ('mksquashfs', 'unsquashfs'):
            with self.subTest(missing=missing):
                self.which.side_effect = lambda name: None if name == missing else '/system/' + name
                self.run.reset_mock()
                self.assert_original_failure()
                self.assertEqual(len(self.run.call_args_list), 1)

    def test_path_pointing_back_to_bundle_is_not_system_pair(self):
        self.which.side_effect = dict(zip(('mksquashfs', 'unsquashfs'), self.bundled)).get
        self.assert_original_failure()

    def test_path_aliases_into_vendor_are_rejected_before_retry(self):
        for physical_vendor in ('/bundle', '/physical-vendor'):
            with self.subTest(physical_vendor=physical_vendor):
                targets = {'/bundle': physical_vendor}
                targets.update(zip(
                    self.system,
                    (physical_vendor + '/mksquashfs',
                     physical_vendor + '/unsquashfs')))
                original = ib.BuildError(
                    'vendor failure', code='E104', action='original hint')
                with mock.patch.object(
                        bundle, 'vendor_dir', return_value='/bundle'), \
                     mock.patch.object(
                        ib.os.path, 'realpath',
                        side_effect=lambda p: targets.get(p, p)), \
                     mock.patch.object(
                        ib, '_probe_squashfs_tools',
                        side_effect=[original, None]) as probe:
                    with self.assertRaises(ib.BuildError) as caught:
                        ib.check_squashfs_tools(*self.bundled, self.rep)
                    self.assertIs(caught.exception, original)
                    probe.assert_called_once_with(*self.bundled, self.rep)
                self.assertIsNone(ib._selected_squashfs_pair)

    def test_successful_bundle_does_not_search_system(self):
        self.bundled_fails = False
        ib.check_squashfs_tools(*self.bundled, self.rep)
        self.which.assert_not_called()

    def test_other_hosts_keep_original_failure(self):
        for host in ('macos', 'windows'):
            with self.subTest(host=host):
                self.host.return_value = (host, 'x86_64')
                self.assert_original_failure()
        self.which.assert_not_called()

    def test_system_failure_does_not_retry(self):
        self.system_fails = True
        with self.assertRaises(ib.BuildError):
            ib.check_squashfs_tools(*self.system, self.rep)
        self.which.assert_not_called()

    def test_archive_probe_retries_same_capabilities(self):
        original = ib.BuildError('pseudo marker', code='E104', action='original hint')
        with mock.patch.object(ib, '_probe_archive_squashfs_tools',
                               side_effect=[original, None]) as probe:
            ib.check_archive_squashfs_tools(*self.bundled, '/spool', self.rep)
        self.assertEqual(probe.call_args_list, [
            mock.call(*self.bundled, '/spool', self.rep),
            mock.call(*self.system, '/spool', self.rep)])
        self.assertEqual(ib._squashfs_tools(), self.system)

    def test_mixed_initial_resolution_is_never_executed(self):
        ib.check_squashfs_tools(self.bundled[0], self.system[1], self.rep)
        self.assertEqual([c.args[0][0] for c in self.run.call_args_list], list(self.system))

    def test_stock_validation_reuses_selected_extractor(self):
        import tempfile
        ib.check_squashfs_tools(*self.bundled, self.rep)
        with tempfile.TemporaryDirectory() as td:
            stock = Path(td, 'stock.sq')
            stock.write_bytes(b'hsqs')
            def extract(cmd, **kwargs):
                self.assertEqual(cmd[0], self.system[1])
                version = Path(cmd[2], 'etc/product_version/version.in')
                version.parent.mkdir(parents=True)
                version.write_text('PRODUCT=X\nMAIN_OS_VER=257\n')
                return subprocess.CompletedProcess(cmd, 0, '', '')
            self.run.side_effect = extract
            with mock.patch.object(ib, '_validate_stock_version', return_value='257'):
                self.assertEqual(ib.validate_stock_rootfs(stock, self.rep), '257')

    def test_selected_pair_is_kept_for_subsequent_probes(self):
        ib.check_squashfs_tools(*self.bundled, self.rep)
        self.run.reset_mock()
        self.which.reset_mock()
        ib.check_squashfs_tools(*self.bundled, self.rep)
        self.assertEqual([c.args[0][0] for c in self.run.call_args_list], list(self.system))
        self.which.assert_not_called()

    def test_build_uses_the_selected_pair_after_probe(self):
        import tempfile
        class ReachedUnpack(Exception):
            pass
        raw_tool = self.tool
        def run(cmd, **kwargs):
            if 'stock.sq' in cmd:
                self.assertEqual(cmd[0], self.system[1])
                self.assertEqual(ib._squashfs_tools(), self.system)
                raise ReachedUnpack
            return raw_tool(cmd, **kwargs)
        self.run.side_effect = run
        with tempfile.TemporaryDirectory() as td:
            with mock.patch.object(ib, '_case_sensitive_extract_root',
                                   return_value=contextlib.nullcontext(td)):
                with self.assertRaises(ReachedUnpack):
                    ib.build_image('stock.sq', 'ui', 'public', 'out', td,
                                   rep=self.rep, no_ota=True)
