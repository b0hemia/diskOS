# SPDX-License-Identifier: MIT
"""Development flash gates and failure outcomes, with host/device calls mocked."""
from contextlib import ExitStack
import hashlib
import io
import os
from pathlib import Path
import queue
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from diskos_installer import bundle, flash_child, flasher, platform_probe, service, state, windows_usb
from diskos_installer import __main__ as cli
from diskos_installer.reporter import QueueReporter
from test_windows_host import API, Process
from test_windows_usb import row


class WindowsFlashTests(unittest.TestCase):
    def setUp(self):
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.tmp = self.stack.enter_context(tempfile.TemporaryDirectory())
        self.root = Path(self.tmp)
        self.image = self.root / 'image.bin'
        with self.image.open('wb') as f:
            f.write(b'hsqs')
            f.truncate(flasher.IMG_SIZE)
        self.rep = QueueReporter(queue.Queue())
        self.patch(platform_probe, 'host', return_value=('windows', 'x86_64'))
        self.stack.enter_context(mock.patch.object(platform_probe.sys, 'platform', 'win32'))
        self.patch(platform_probe.platform, 'system', return_value='Windows')
        self.patch(platform_probe.platform, 'python_implementation', return_value='CPython')
        self.stack.enter_context(mock.patch.dict(os.environ, DISKOS_WINDOWS_DEV_FLASH='1'))

    def patch(self, obj, name, **kw):
        return self.stack.enter_context(mock.patch.object(obj, name, **kw))

    def preflight_mocks(self):
        self.patch(bundle, 'verify_windows_flash_bundle')
        self.patch(flasher, '_probe_helpers')
        self.patch(bundle, 'native', return_value='usbboot.exe')
        self.patch(bundle, 'native_env', return_value={'PATH': 'private'})
        self.patch(windows_usb, 'inventory', return_value=dict(
            state='winusb', count=1, present=[row()], phantom=[]))
        return self.patch(flasher.subprocess, 'run', return_value=subprocess.CompletedProcess(
            [], 0, '{"schema":1,"status":"raw","count":1,"error":0,"length":5,"hex":"5832303030"}'))

    def test_no_flag_or_environment_refused(self):
        for requested, env in ((False, '1'), (True, ''), (True, 'true')):
            with self.subTest(requested=requested, env=env), mock.patch.dict(
                    os.environ, DISKOS_WINDOWS_DEV_FLASH=env):
                with self.assertRaises(flasher.FlashError) as caught:
                    with flasher.windows_dev_flash(requested):
                        flasher.preflight(str(self.image), self.rep)
                self.assertEqual(caught.exception.code, 'E101')
        self.assertFalse(flasher._WINDOWS_DEV_FLASH.get())

    def test_refuse_unqualified_runtimes(self):
        cases = [(platform_probe.sys, 'frozen', True),
                 (platform_probe.sys, 'platform', 'cygwin'),
                 (platform_probe.sys, 'platform', 'msys'),
                 (platform_probe.platform, 'system', mock.Mock(return_value='MSYS_NT-10.0')),
                 (platform_probe.struct, 'calcsize', mock.Mock(return_value=4)),
                 (platform_probe, 'host', mock.Mock(return_value=('windows', 'arm64'))),
                 (platform_probe.platform, 'python_implementation', mock.Mock(return_value='PyPy'))]
        for obj, name, value in cases:
            with self.subTest(name=name, value=value), mock.patch.object(obj, name, value, create=True):
                with self.assertRaises(flasher.FlashError) as caught:
                    with flasher.windows_dev_flash(True): pass
                self.assertEqual(caught.exception.code, 'E101')

    def test_cli_flag_scoped_to_install_and_restore(self):
        run = self.patch(cli, 'cmd_install', return_value=3)
        self.patch(cli, '_Lock')
        self.assertEqual(cli.main(['install', '--stock', 'stock.sq', '--windows-dev-flash']), 3)
        self.assertTrue(run.call_args.args[0].windows_dev_flash)
        self.assertFalse(flasher._WINDOWS_DEV_FLASH.get())
        for cmd in ('install', 'restore-stock'):
            self.assertFalse(cli.build_parser().parse_args([cmd]).windows_dev_flash)
        with mock.patch('sys.stderr', io.StringIO()), self.assertRaises(SystemExit):
            cli.build_parser().parse_args(['doctor', '--windows-dev-flash'])

    def test_inventory_states_preserve_specific_codes(self):
        inventory = windows_usb.inventory
        runner = self.preflight_mocks()
        for present, phantom, code in (([], [], 'E110'), ([], [row()], 'E110'),
                ([row(problem=28, service=None)], [], 'E114'),
                ([row(), row(instance=r'USB\VID_A108&PID_EAEF\SECOND')], [], 'E111'),
                ([row()], [], None)):
            api = mock.Mock()
            api.devices.side_effect = [present + phantom, present]
            result = inventory(api)
            with self.subTest(state=result['state']), mock.patch.object(
                    windows_usb, 'inventory', return_value=result), flasher.windows_dev_flash(True):
                if code:
                    with self.assertRaises(flasher.FlashError) as caught:
                        flasher.preflight(str(self.image), self.rep)
                    self.assertEqual(caught.exception.code, code)
                    self.assertTrue(caught.exception.action)
                else:
                    flasher.preflight(str(self.image), self.rep)
        runner.assert_called_once()
        self.assertEqual(runner.call_args.kwargs['env'], {'PATH': 'private'})

    def test_inventory_failure_and_access_refused(self):
        runner = self.preflight_mocks()
        with flasher.windows_dev_flash(True):
            with mock.patch.object(windows_usb, 'inventory', side_effect=OSError('failed')):
                with self.assertRaises(flasher.FlashError) as caught:
                    flasher.preflight(str(self.image), self.rep)
                self.assertEqual(caught.exception.code, 'E112')
            for reply in ('{}', '{"schema":1,"status":"raw","count":1,"error":0,"length":5,"hex":"0000000000"}'):
                runner.return_value = subprocess.CompletedProcess([], 0, reply)
                with self.assertRaises(flasher.FlashError) as caught:
                    flasher.preflight(str(self.image), self.rep)
                self.assertEqual(caught.exception.code, 'E115')

    def test_manifest_missing_stale_duplicate_or_changed_output(self):
        source = self.root / 'src/usbboot/usbboot.c'
        source.parent.mkdir(parents=True)
        source.write_bytes(b'current source')
        vendor = self.root / 'vendor/windows-x86_64'
        vendor.mkdir(parents=True)
        exe = vendor / 'usbboot.exe'
        exe.write_bytes(b'current executable')
        manifest = vendor / 'MANIFEST.txt'
        self.patch(bundle, 'resource_root', return_value=str(self.root))
        good_input = hashlib.sha256(source.read_bytes()).hexdigest() + '  src/usbboot/usbboot.c\n'
        good_output = hashlib.sha256(exe.read_bytes()).hexdigest() + '  usbboot.exe\n'
        def write(inputs=good_input, outputs=good_output):
            manifest.write_text('\nInput SHA256 (paths relative to installer):\n' + inputs
                                + '\nOutput SHA256:\n' + outputs, encoding='ascii')
        with self.assertRaises(flasher.FlashError): bundle.verify_windows_flash_bundle()
        write()
        bundle.verify_windows_flash_bundle()
        # MSYS2 sha256sum writes the binary-mode form "<hash> *<name>" (seen on the Windows test box).
        star_input, star_output = good_input.replace('  ', ' *'), good_output.replace('  ', ' *')
        write(star_input, star_output)
        bundle.verify_windows_flash_bundle()
        for inputs, outputs in ((good_input * 2, good_output), ('0' * 64 + '  src/usbboot/usbboot.c\n', good_output),
                                (good_input, '0' * 64 + '  usbboot.exe\n'), (good_input + star_input, good_output),
                                (good_input, '0' * 64 + ' *usbboot.exe\n'),
                                (good_input.replace('  ', ' '), good_output)):
            write(inputs, outputs)
            with self.assertRaises(flasher.FlashError) as caught: bundle.verify_windows_flash_bundle()
            self.assertEqual(caught.exception.code, 'E117')
            self.assertIn('build/build-windows-tools.sh', caught.exception.action)

    def test_cancel_before_backup_or_flash(self):
        self.patch(service.bundle, 'data', return_value=str(self.image))
        self.patch(service.state, 'build_dir', return_value=self.tmp)
        self.patch(service.state, 'load', return_value={})
        self.patch(service.state, 'save')
        for name in ('_copyfile', 'require_installable', 'build_image'):
            self.patch(service.imagebuild, name)
        self.patch(service.imagebuild, 'validate_stock_rootfs', return_value='257')
        self.patch(service.imagebuild, 'resolve_ota_config', return_value={})
        self.patch(service.imagebuild, 'describe_ota', return_value='disabled')
        self.patch(flasher, 'preflight')
        stage = self.patch(service, '_stage_stock')
        flash = self.patch(flasher, 'flash')
        with flasher.windows_dev_flash(True):
            result = service.do_install({'stock': str(self.image)}, self.rep, lambda summary: False)
        self.assertTrue(result['aborted'])
        stage.assert_not_called()
        flash.assert_not_called()

    def test_backup_retained_on_unknown_and_committed_on_proven_base(self):
        self.stack.enter_context(mock.patch.dict(os.environ, DISKOS_INSTALLER_HOME=str(self.root / 'state')))
        old, _ = state.stage_stock_generation(str(self.image), {'main_os_ver': '257'})
        state.promote_generation(old)
        pending, _ = state.stage_stock_generation(str(self.image), {'main_os_ver': '257'})
        for proven in (False, True):
            error = flasher.FlashError('unknown', action=flasher.WAIT_THEN_RECOVER)
            if proven: error.verdict = {'result': 1}
            with mock.patch.object(flasher, 'flash', side_effect=error):
                with self.assertRaises(flasher.FlashError):
                    service._flash_with_backup(str(self.image), '257', pending, 'last-flash.log', self.rep)
            self.assertEqual(state.current_generation(), pending if proven else old)
            self.assertTrue(Path(state.generation_paths(pending)[0]).is_file())
            self.assertTrue(Path(state.generation_paths(old)[0]).is_file())

    def test_job_assignment_and_parent_eof(self):
        api = API(assign=False)
        popen = mock.Mock(side_effect=lambda *a, **kw: Process(api.events))
        with self.assertRaises(OSError):
            with flasher.windows_dev_flash(True):
                flasher._WindowsChild([str(self.root / 'usbboot.exe'), '--help'], api=api, popen=popen)
        self.assertNotIn('gate', api.events)
        self.assertIn(('close', 11), api.events)
        # Parent death before assignment closes the pipe; the native tool never starts.
        with mock.patch.object(sys, 'stdin', mock.Mock(buffer=io.BytesIO(b''))), \
                mock.patch.object(subprocess, 'call') as native:
            self.assertEqual(flash_child.main(popen.call_args.args[0][3:]), flash_child.VALIDATION_REFUSED)
            native.assert_not_called()

    def test_bundled_writer_pin_and_descriptor(self):
        root = Path(__file__).resolve().parents[1]
        source = root / 'flash' / flasher.WRITER_NAME
        bundled = root / 'vendor/linux-x86_64' / flasher.WRITER_NAME
        data = source.read_bytes()
        self.assertEqual(bundled.read_bytes(), data)
        self.assertEqual(hashlib.sha256(data).hexdigest(), flasher.WRITER_SHA256)
        for path in (source, bundled):
            self.assertEqual(flasher._writer_descriptor(str(path)), (768, 1))
        changed = self.root / flasher.WRITER_NAME
        changed.write_bytes(data[:-1] + bytes([data[-1] ^ 1]))
        self.assertIsNone(flasher._writer_descriptor(str(changed)))

    def test_writer_hash_and_capacity_still_gate_launch(self):
        self.patch(flasher, 'preflight')
        self.patch(bundle, 'native', return_value='unused')
        start = self.patch(flasher, '_start_child')
        for descriptor, code in ((None, 'E123'), ((580, 1), 'E124')):
            with mock.patch.object(flasher, '_writer_descriptor', return_value=descriptor):
                with self.assertRaises(flasher.FlashError) as caught:
                    flasher.flash(str(self.image), '257', rep=self.rep)
                self.assertEqual(caught.exception.code, code)
                self.assertTrue(caught.exception.pre_launch)
        start.assert_not_called()

    def outcome_mocks(self):
        self.patch(flasher, 'preflight')
        self.patch(bundle, 'native', return_value='unused')
        self.patch(bundle, 'native_env', return_value={})
        self.patch(flasher, '_writer_descriptor', return_value=(768, 1))
        self.patch(flasher.basegate, 'build_plan', return_value=b'plan')
        self.patch(bundle, '_windows_api', return_value=API())
        make_tmp = tempfile.mkdtemp
        self.patch(flasher.tempfile, 'mkdtemp', side_effect=lambda **kw: make_tmp(dir=self.tmp, **kw))
        # Keep every retained log inside this test's TemporaryDirectory.
        original_file = tempfile.mkstemp
        self.patch(flasher.tempfile, 'mkstemp', side_effect=lambda **kw: original_file(dir=self.tmp, **kw))
        return self.patch(flasher, '_start_child')

    def test_proven_child_refusal_is_e304_after_wait_without_interruption(self):
        start = self.outcome_mocks()
        child = mock.Mock(spec=flasher._WindowsChild)
        child.wait.return_value = getattr(flash_child, 'VALIDATION_REFUSED', 65)
        start.return_value = child
        kill = self.patch(flasher, '_kill_child')
        with self.assertRaises(flasher.FlashError) as caught:
            flasher.flash(str(self.image), '257', rep=self.rep)
        error = caught.exception
        self.assertEqual(error.code, 'E304')
        self.assertTrue(error.pre_launch)
        self.assertEqual(error.message, 'The flash could not start because Windows flash validation failed. '
                         'Nothing was sent by the flash transfer.')
        self.assertEqual(error.action, 'See Details for the validation failure. Correct the reported problem '
                         'and retry; no flash-recovery wait is needed.')
        self.assertTrue(str(error).isascii())
        child.wait.assert_called_once_with(timeout=flasher.FLASH_HARD_TIMEOUT_SECS)
        child.close.assert_called_once()
        kill.assert_not_called()
        events = list(self.rep.q.queue)
        self.assertFalse(any('interrupted' in str(e) for e in events))

    def test_refusal_log_text_does_not_prove_pre_launch_failure(self):
        start = self.outcome_mocks()
        child = mock.Mock(spec=flasher._WindowsChild)
        child.wait.return_value = 1
        def launch(cmd, **kw):
            kw['stdout'].write('Windows flash child: refused: path validation failed\n')
            kw['stdout'].flush()
            return child
        start.side_effect = launch
        with self.assertRaises(flasher.FlashError) as caught:
            flasher.flash(str(self.image), '257', rep=self.rep)
        self.assertEqual(caught.exception.code, 'E301')
        self.assertFalse(getattr(caught.exception, 'pre_launch', False))
        self.assertEqual(caught.exception.action, flasher.WAIT_THEN_RECOVER)

    def test_only_windows_child_completed_refusal_status_is_trusted(self):
        start = self.outcome_mocks()
        kill = self.patch(flasher, '_kill_child')
        for windows, result in ((True, 1), (True, 0), (True, 0xc0000005),
                                (False, getattr(flash_child, 'VALIDATION_REFUSED', 65)),
                                (True, OSError('post-launch failure')),
                                (True, subprocess.TimeoutExpired('child', 1))):
            with self.subTest(windows=windows, result=result):
                child = mock.Mock(spec=flasher._WindowsChild if windows else subprocess.Popen)
                if isinstance(result, Exception): child.wait.side_effect = result
                else: child.wait.return_value = result
                start.return_value = child
                with self.assertRaises(flasher.FlashError) as caught:
                    flasher.flash(str(self.image), '257', rep=self.rep)
                error = caught.exception
                self.assertEqual(error.code, 'E303' if isinstance(result, subprocess.TimeoutExpired) else 'E301')
                self.assertFalse(getattr(error, 'pre_launch', False))
                self.assertEqual(error.action, flasher.WAIT_THEN_RECOVER)
        self.assertEqual(kill.call_count, 2)

    def test_sleep_failure_timeout_and_incomplete_result(self):
        self.patch(flasher, 'preflight')
        self.patch(bundle, 'native', return_value='unused')
        self.patch(bundle, 'native_env', return_value={})
        self.patch(flasher, '_writer_descriptor', return_value=(768, 1))
        self.patch(flasher.basegate, 'build_plan', return_value=b'plan')
        make_tmp = tempfile.mkdtemp
        self.patch(flasher.tempfile, 'mkdtemp', side_effect=lambda **kw: make_tmp(dir=self.tmp, **kw))
        child = mock.Mock()
        start = self.patch(flasher, '_start_child', return_value=child)
        kill = self.patch(flasher, '_kill_child')
        with mock.patch.object(bundle, '_windows_api', return_value=API(sleep=0)):
            with self.assertRaises(flasher.FlashError) as caught: flasher.flash(str(self.image), '257', rep=self.rep)
            self.assertEqual(caught.exception.code, 'E116')
            self.assertTrue(caught.exception.pre_launch)
            start.assert_not_called()
        with mock.patch.object(bundle, '_windows_api', return_value=API()):
            child.wait.side_effect = subprocess.TimeoutExpired('usbboot', 1)
            with self.assertRaises(flasher.FlashError) as caught: flasher.flash(str(self.image), '257', rep=self.rep)
            self.assertEqual(caught.exception.code, 'E303', str(caught.exception))
            self.assertEqual(caught.exception.action, flasher.WAIT_THEN_RECOVER)
            kill.assert_called_once_with(child)
            child.wait.assert_called_with(timeout=flasher.FLASH_HARD_TIMEOUT_SECS)
            cmd = start.call_args.args[0]
            self.assertIn(flasher.FLASH_WAIT, cmd)
            child.wait.side_effect = None
            for raw in (None, b'short', bytes(1024)):
                def launch(cmd, **kw):
                    if raw is not None: Path(cmd[-1]).write_bytes(raw)
                    return child
                start.side_effect = launch
                with self.assertRaises(flasher.FlashError) as caught: flasher.flash(str(self.image), '257', rep=self.rep)
                self.assertEqual(caught.exception.action, flasher.WAIT_THEN_RECOVER)
                self.assertFalse(getattr(caught.exception, 'pre_launch', False))
                self.assertTrue(Path(start.call_args.args[0][-1]).parent.joinpath('plan.bin').is_file())


if __name__ == '__main__':
    unittest.main()
