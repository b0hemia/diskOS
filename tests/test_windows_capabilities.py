# SPDX-License-Identifier: GPL-3.0-or-later
"""Host admission policy; all native and device operations are mocked."""
from contextlib import ExitStack
import io
import os
import sys
import types
import unittest
from unittest import mock

from diskos_installer import flasher, platform_probe
from diskos_installer import __main__ as cli, gui


class WindowsCapabilityTests(unittest.TestCase):
    def setUp(self):
        self.stack = ExitStack(); self.addCleanup(self.stack.close)
        self.cap = types.SimpleNamespace(WINDOWS_FLASH=False, SOURCE_COMMIT='')
        self.stack.enter_context(mock.patch.object(flasher, '_build_capabilities', self.cap, create=True))
        self.stack.enter_context(mock.patch.object(sys, 'platform', 'win32'))
        self.stack.enter_context(mock.patch.object(sys, 'frozen', False, create=True))
        self.stack.enter_context(mock.patch.object(platform_probe.platform, 'system', return_value='Windows'))
        self.stack.enter_context(mock.patch.object(platform_probe.platform, 'machine', return_value='AMD64'))
        self.stack.enter_context(mock.patch.dict(os.environ, DISKOS_WINDOWS_DEV_FLASH=''))

    def test_source_and_frozen_matrix(self):
        for frozen in (False, True):
            for enabled in (False, True):
                for requested in (False, True):
                    for env in ('', '0', 'true', '1'):
                        with self.subTest(frozen=frozen, enabled=enabled, requested=requested, env=env), \
                                mock.patch.object(sys, 'frozen', frozen), \
                                mock.patch.dict(os.environ, DISKOS_WINDOWS_DEV_FLASH=env):
                            self.cap.WINDOWS_FLASH = enabled
                            self.assertEqual(flasher.flash_eligible(dev_requested=requested),
                                             enabled if frozen else requested and env == '1')

    def test_enabled_artifact_requires_native_x64_cpython(self):
        self.cap.WINDOWS_FLASH = True
        with mock.patch.object(sys, 'frozen', True):
            self.assertTrue(flasher.flash_eligible())
            for obj, name, value in ((sys, 'platform', 'cygwin'), (sys, 'platform', 'msys'),
                    (platform_probe.platform, 'system', mock.Mock(return_value='MSYS_NT-10.0')),
                    (platform_probe.platform, 'machine', mock.Mock(return_value='ARM64')),
                    (platform_probe.struct, 'calcsize', mock.Mock(return_value=4)),
                    (platform_probe.platform, 'python_implementation', mock.Mock(return_value='PyPy'))):
                with self.subTest(name=name), mock.patch.object(obj, name, value):
                    self.assertFalse(flasher.flash_eligible(dev_requested=True))

    def test_cli_and_gui_use_public_admission_without_environment(self):
        self.cap.WINDOWS_FLASH = True
        with mock.patch.dict(os.environ), mock.patch.object(sys, 'frozen', True), \
                mock.patch.object(cli.service, 'do_install', return_value={'aborted': True}) as install, \
                mock.patch.object(cli, '_Lock'), mock.patch.object(sys, 'stdout', io.StringIO()):
            os.environ.pop('DISKOS_WINDOWS_DEV_FLASH', None)
            self.assertTrue(flasher.flash_eligible())
            self.assertEqual(cli.main(['install', '--stock', 'stock.sq']), 3)
            install.assert_called_once()
            app = types.SimpleNamespace(host_lbl=mock.Mock(), _ready_start=mock.Mock())
            gui.App._refresh_state_labels(app)
            self.assertIn('flashing enabled', app.host_lbl.config.call_args.kwargs['text'])
            app._ready_start.assert_called_once_with()
            for command in ('install', 'restore-stock'):
                with mock.patch.object(sys, 'stderr', io.StringIO()), self.assertRaises(SystemExit):
                    cli.build_parser().parse_args([command, '--windows-dev-flash'])

    def test_host_label_fallback_uses_ascii(self):
        app = types.SimpleNamespace(host_lbl=mock.Mock())
        with mock.patch.object(platform_probe, 'host', return_value=('linux', 'x86_64')), \
                mock.patch.object(platform_probe, 'maskrom_count', return_value=1):
            gui.App._refresh_state_labels(app)
        text = app.host_lbl.config.call_args_list[0].kwargs['text']
        self.assertIn('1 device(s) in flash mode', text)
        self.assertTrue(text.isascii())

    def test_enabled_artifact_still_checks_image_and_manifest(self):
        self.cap.WINDOWS_FLASH = True
        with mock.patch.object(sys, 'frozen', True), mock.patch.object(flasher.os.path, 'exists', return_value=True), \
                mock.patch.object(flasher.os.path, 'getsize', return_value=4):
            with self.assertRaises(flasher.FlashError) as caught: flasher.preflight('image')
            self.assertEqual(caught.exception.code, 'E121')
            with mock.patch.object(flasher.os.path, 'getsize', return_value=flasher.IMG_SIZE), \
                    mock.patch('builtins.open', mock.mock_open(read_data=b'hsqs')), \
                    mock.patch.object(flasher.bundle, 'verify_windows_flash_bundle',
                                      side_effect=flasher.FlashError('stale manifest', code='E117')), \
                    mock.patch.object(flasher, '_probe_helpers') as probes:
                with self.assertRaises(flasher.FlashError) as caught: flasher.preflight('image')
                self.assertEqual(caught.exception.code, 'E117'); probes.assert_not_called()

    def test_posix_admission_unchanged(self):
        for host in ('linux', 'macos', 'freebsd'):
            with mock.patch.object(platform_probe, 'host', return_value=(host, 'x86_64')):
                self.assertEqual(flasher.flash_eligible(), host in ('linux', 'macos'))
