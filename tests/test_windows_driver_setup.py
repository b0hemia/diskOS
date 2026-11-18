# SPDX-License-Identifier: GPL-3.0-or-later
"""Driver guidance and elevation integration; no Windows, network or USB needed."""
import contextlib
import ctypes
import json
import queue
import subprocess
import threading
import unittest
from unittest import mock

from diskos_installer import __main__ as cli, flasher, gui, windows_usb as usb
from diskos_installer.reporter import QueueReporter


def inventory(bound=False, **changes):
    row = dict(instance_id=r'USB\VID_A108&PID_EAEF\DISC', problem_code=0 if bound else 28,
               service='WINUSB' if bound else None, inf='winusb.inf' if bound else None,
               provider='Microsoft' if bound else None, location_paths=['USBROOT(0)#USB(1)'])
    row.update(changes)
    return dict(state='winusb' if bound else 'driver', count=1, present=[row], phantom=[])


def output(status='installed', reason=None, error=None, **changes):
    success = status in ('installed', 'already_bound', 'reboot_required')
    data = dict(schema=1, status=status, reason=reason or ('none' if success else status),
                error=(0 if success else 13) if error is None else error,
                installed=status in ('installed', 'reboot_required', 'installed_status_unknown'),
                reboot_required=status in ('reboot_required', 'installed_status_unknown'))
    data.update(changes)
    return subprocess.CompletedProcess([], 0 if success else 1, json.dumps(data) + '\n')


class SetupTests(unittest.TestCase):
    def setUp(self):
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        def patch(obj, name, **kwargs):
            return self.stack.enter_context(mock.patch.object(obj, name, **kwargs))
        patch(usb.sys, 'platform', new='win32')
        patch(usb.platform_probe, 'host', return_value=('windows', 'x86_64'))
        self.probe = patch(usb, 'driver_inventory', side_effect=[inventory(), inventory(True), inventory(True)])
        self.native = patch(usb.bundle, 'native', side_effect=lambda name: 'C:\\bundle\\' + name + '.exe')
        self.verify_driver = patch(usb.bundle, 'verify_windows_driver_bundle')
        self.verify_flash = patch(usb.bundle, 'verify_windows_flash_bundle')
        self.env = patch(usb.bundle, 'native_env', return_value={'PATH': 'C:\\bundle'})
        self.elevate = patch(usb, '_elevated_helper', return_value=output())
        self.run = patch(usb.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0,
                         json.dumps(dict(schema=1, status='raw', count=1, error=0, length=5, hex='5832303030',
                                         instance_id=inventory()['present'][0]['instance_id']))))
        self.install = patch(cli.service, 'do_install', side_effect=AssertionError('automatic install'))
        self.restore = patch(cli.service, 'do_restore', side_effect=AssertionError('automatic restore'))
        self.confirm, self.report = mock.Mock(return_value=True), mock.Mock()

    def setup(self):
        return usb.setup_driver(self.report, self.confirm)

    def test_success_reprobes_and_proves_access_with_bundled_tools(self):
        result = self.setup()
        self.assertIn('No flash has started', result['message'])
        self.assertEqual(self.probe.call_count, 3)
        self.verify_driver.assert_called_once_with()
        self.elevate.assert_called_once_with(r'C:\bundle\bind_winusb.exe')
        self.verify_flash.assert_called_once_with()
        self.run.assert_called_once_with([r'C:\bundle\usbboot.exe', '--native-diagnostic'],
                                        env={'PATH': 'C:\\bundle'}, capture_output=True, text=True, timeout=15)
        text = self.report.call_args_list[0].args[0]
        for detail in ('service=none', 'INF=none', 'provider=none', 'problem code=28'):
            self.assertIn(detail, text)
        self.install.assert_not_called()
        self.restore.assert_not_called()

    def test_existing_binding_checks_without_elevation_or_confirmation(self):
        self.probe.side_effect = [inventory(True), inventory(True), inventory(True)]
        result = self.setup()
        self.assertIn('succeeded', result['message'])
        self.elevate.assert_not_called()
        self.verify_driver.assert_not_called()
        self.confirm.assert_not_called()
        self.run.assert_called_once()

    def test_success_message_uses_shared_flash_eligibility(self):
        for eligible, ending in ((True, 'The Disc is ready to install.'),
                                 (False, 'This build cannot flash on Windows.')):
            for bound in (False, True):
                with self.subTest(eligible=eligible, bound=bound):
                    self.probe.side_effect = [inventory(bound), inventory(True), inventory(True)]
                    with mock.patch.object(flasher, 'flash_eligible', return_value=eligible) as gate:
                        message = self.setup()['message']
                    gate.assert_called_once_with()
                    self.assertEqual(message, 'USB open, claim and control succeeded. '
                                     'No flash has started. ' + ending)
                    self.assertTrue(message.isascii())

    def test_frozen_setup_does_not_change_the_flash_gate(self):
        with mock.patch.object(usb.sys, 'frozen', True, create=True):
            self.assertIn('succeeded', self.setup()['message'])
            self.assertFalse(cli.platform_probe.windows_dev_runtime())

    def test_user_cancels_before_uac(self):
        self.confirm.return_value = False
        self.assertTrue(self.setup()['aborted'])
        self.elevate.assert_not_called()
        self.run.assert_not_called()
        self.assertEqual(self.probe.call_count, 1)

    def test_cancelled_uac_and_missing_credentials(self):
        for error, code in ((1223, 'E118'), (5, 'E119'), (1326, 'E119'), (1385, 'E119'), (1314, 'E119')):
            exc = OSError('Windows denied elevation')
            exc.winerror = error
            self.probe.side_effect = [inventory()]
            self.elevate.side_effect = exc
            with self.subTest(error=error), self.assertRaises(usb.PreflightError) as caught:
                self.setup()
            self.assertEqual(caught.exception.code, code)
        self.run.assert_not_called()

    def test_unknown_binding_refused_before_helper(self):
        for changes in ({'service': 'usbser'}, {'inf': 'oem7.inf'}, {'provider': 'Other'}, {'problem_code': 10}):
            self.probe.side_effect = [inventory(True, **changes)]
            with self.subTest(changes=changes), self.assertRaises(usb.PreflightError) as caught:
                self.setup()
            self.assertEqual(caught.exception.code, 'E127')
        self.elevate.assert_not_called()
        self.confirm.assert_not_called()

    def test_absent_phantom_and_ambiguous_stop_before_setup(self):
        for state, count, code in (('absent', 0, 'E110'), ('phantom', 0, 'E110'), ('ambiguous', 2, 'E111')):
            self.probe.side_effect = [dict(state=state, count=count, present=[], phantom=[])]
            with self.subTest(state=state), self.assertRaises(usb.PreflightError) as caught:
                self.setup()
            self.assertEqual(caught.exception.code, code)
        self.elevate.assert_not_called()

    def test_reboot_and_unknown_install_status_end_setup(self):
        for status in ('reboot_required', 'installed_status_unknown'):
            self.probe.reset_mock()
            self.probe.side_effect = [inventory()]
            self.elevate.return_value = output(status)
            result = self.setup()
            self.assertTrue(result['reboot_required'])
            self.assertIn('Restart Windows', result['message'])
            self.assertIn('reconnect', result['message'])
            self.assertIn('Windows error ' + ('13' if status == 'installed_status_unknown' else '0'), result['message'])
            self.assertTrue(result['message'].isascii())
            self.assertEqual(self.probe.call_count, 1)
        self.run.assert_not_called()
        self.verify_flash.assert_not_called()

    def test_helper_failures_have_codes_and_do_not_probe(self):
        for status, reason, code in (('refused', 'unfamiliar_driver', 'E127'),
                ('api_error', 'install_failed', 'E126'), ('target_changed', None, 'E126'),
                ('elevation_required', None, 'E119'), ('refused', 'not_unique', 'E126')):
            self.probe.reset_mock()
            self.probe.side_effect = [inventory()]
            self.elevate.return_value = output(status, reason)
            with self.subTest(status=status), self.assertRaises(usb.PreflightError) as caught:
                self.setup()
            self.assertEqual(caught.exception.code, code)
            self.assertEqual(self.probe.call_count, 1)
        self.run.assert_not_called()

    def test_launcher_failure_and_unverified_bundle(self):
        self.elevate.side_effect = OSError('timeout')
        with self.assertRaises(usb.PreflightError) as caught:
            self.setup()
        self.assertEqual(caught.exception.code, 'E126')
        self.probe.side_effect = [inventory()]
        self.verify_driver.side_effect = cli.errors.FlashError('stale helper', code='E117')
        self.elevate.reset_mock()
        with self.assertRaises(cli.errors.FlashError):
            self.setup()
        self.elevate.assert_not_called()
        self.run.assert_not_called()

    def test_changed_device_location_and_driver_never_get_access_check(self):
        for changes in ({'instance_id': r'USB\VID_A108&PID_EAEF\OTHER'},
                        {'location_paths': ['USB(2)']}, {'location_paths': []}, {'inf': 'oem8.inf'}):
            self.probe.side_effect = [inventory(), inventory(True, **changes)]
            with self.subTest(changes=changes), self.assertRaises(usb.PreflightError) as caught:
                self.setup()
            self.assertEqual(caught.exception.code, 'E126')
        self.run.assert_not_called()

    def test_access_check_requires_same_single_identity_after_diagnostic(self):
        changed = [inventory(True, instance_id=r'USB\VID_A108&PID_EAEF\OTHER'),
                   inventory(True, location_paths=['USBROOT(0)#USB(2)']),
                   inventory(True, location_paths=[]), inventory(True, inf='oem9.inf'),
                   dict(state='absent', count=0, present=[], phantom=[]),
                   dict(state='ambiguous', count=2, present=inventory(True)['present'] * 2, phantom=[])]
        for after in changed:
            events = []
            def probe():
                events.append('inventory')
                return after if 'diagnostic' in events else inventory(True)
            def diagnostic(*args, **kwargs):
                events.append('diagnostic')
                return self.run.return_value
            self.probe.side_effect = probe
            self.run.side_effect = diagnostic
            with self.subTest(after=after), self.assertRaises(usb.PreflightError):
                self.setup()
            self.assertEqual(events[-2:], ['diagnostic', 'inventory'])
        self.install.assert_not_called()
        self.restore.assert_not_called()

    def test_inventory_brackets_access_after_bundle_preparation(self):
        events = []
        def probe():
            events.append('inventory')
            return inventory(True)
        self.probe.side_effect = probe
        self.verify_flash.side_effect = lambda: events.append('verify')
        self.native.side_effect = lambda name: events.append('resolve') or r'C:\bundle\usbboot.exe'
        self.env.side_effect = lambda tool: events.append('environment') or {}
        self.report.side_effect = lambda text: events.append('report')
        self.run.side_effect = lambda *args, **kwargs: events.append('diagnostic') or self.run.return_value
        self.assertIn('succeeded', self.setup()['message'])
        index = events.index('diagnostic')
        self.assertEqual(events[index-1:index+2], ['inventory', 'diagnostic', 'inventory'])
        self.assertLess(events.index('environment'), index - 1)

    def test_a_to_b_to_a_swap_during_native_access_is_rejected(self):
        events = []
        a = inventory(True)
        b = inventory(True, instance_id=r'USB\VID_A108&PID_EAEF\6&21FC78A&0&2',
                      location_paths=['PCIROOT(0)#USBROOT(0)#USB(2)', 'ACPI(0)#ACPI(PRT2)'])
        attached = a
        def probe():
            events.append(('inventory', attached['present'][0]['instance_id']))
            return attached
        def access(*args, **kwargs):
            nonlocal attached
            attached = b
            data = json.loads(self.run.return_value.stdout)
            data['instance_id'] = attached['present'][0]['instance_id']
            events.append(('opened', data['instance_id']))
            attached = a
            return subprocess.CompletedProcess([], 0, json.dumps(data))
        self.probe.side_effect = probe
        self.run.side_effect = access
        with self.assertRaises(usb.PreflightError) as caught:
            self.setup()
        self.assertEqual(caught.exception.code, 'E126')
        self.assertEqual([item[0] for item in events], ['inventory', 'inventory', 'opened'])
        self.assertIs(attached, a)  # A is back before any final inventory could see B.
        self.assertEqual(self.report.call_count, 1)
        self.assertNotIn('succeeded', self.report.call_args.args[0])
        self.install.assert_not_called()
        self.restore.assert_not_called()

    def test_native_access_requires_well_formed_opened_device_identity(self):
        good = json.loads(self.run.return_value.stdout)
        cases = [dict(good)] + [good | {'instance_id': value} for value in
                 (None, '', 1, True, [], {}, 'DISC', 'USB\\VID_A108&PID_EAEF\\',
                  r'USB\VID_A108&PID_EAE0\DISC', 'USB\\VID_A108&PID_EAEF\\DISC\n')]
        del cases[0]['instance_id']
        for data in cases:
            self.probe.side_effect = [inventory(True), inventory(True)]
            self.report.reset_mock()
            self.run.return_value = subprocess.CompletedProcess([], 0, json.dumps(data))
            with self.subTest(data=data), self.assertRaises(usb.PreflightError) as caught:
                self.setup()
            self.assertEqual(caught.exception.code, 'E128')
            self.assertEqual(self.report.call_count, 1)
        self.install.assert_not_called()
        self.restore.assert_not_called()

    def test_native_identity_is_case_insensitive_and_keeps_both_location_paths(self):
        target = inventory(True, instance_id=r'USB\VID_A108&PID_EAEF\6&21FC78A&0&1',
                           location_paths=['PCIROOT(0)#USBROOT(0)#USB(1)', 'ACPI(0)#ACPI(PRT1)'])
        self.probe.side_effect = [target, target, target]
        data = json.loads(self.run.return_value.stdout)
        data['instance_id'] = target['present'][0]['instance_id'].lower()
        self.run.return_value = subprocess.CompletedProcess([], 0, json.dumps(data))
        self.assertIn('succeeded', self.setup()['message'])
        self.assertEqual(self.probe.call_count, 3)

    def test_helper_failures_use_plain_english_with_numeric_errors(self):
        reasons = {
            'refused': ('inventory_error', 'invalid_target', 'not_unique', 'unfamiliar_driver',
                        'inf_unavailable', 'inf_path_too_long', 'invalid_driver_detail',
                        'provider_mismatch', 'inf_mismatch', 'ambiguous_winusb_node', 'no_winusb_node'),
            'api_error': ('get_params_failed', 'set_params_failed', 'build_list_failed',
                          'driver_enum_failed', 'driver_detail_failed', 'select_driver_failed', 'install_failed')}
        cases = [(status, reason) for status, group in reasons.items() for reason in group]
        cases += [(status, status) for status in ('loader_policy_refused', 'arguments_refused',
                   'elevation_required', 'system_path_refused', 'target_changed')]
        for status, reason in cases:
            self.probe.side_effect = [inventory()]
            self.elevate.return_value = output(status, reason, error=31)
            with self.subTest(reason=reason), self.assertRaises(usb.PreflightError) as caught:
                self.setup()
            text = str(caught.exception)
            self.assertTrue(text.isascii())
            self.assertNotIn(reason, text)
            self.assertIn('Windows error 31', text)

    def test_launcher_errors_never_display_localized_exception_text(self):
        for error in (None, 31, 1223, 5, 1326, 1385, 1314):
            exc = OSError('Zugriff verweigert: \u00dcberpr\u00fcfung')
            exc.winerror = error
            self.probe.side_effect = [inventory()]
            self.elevate.side_effect = exc
            with self.subTest(error=error), self.assertRaises(usb.PreflightError) as caught:
                self.setup()
            text = str(caught.exception)
            self.assertTrue(text.isascii())
            self.assertNotIn('Zugriff', text)
            if error is not None:
                self.assertIn('Windows error ' + str(error), text)

    def test_driver_still_unbound_or_device_gone(self):
        for result, code in ((inventory(), 'E114'),
                            (dict(state='absent', count=0, present=[], phantom=[]), 'E110')):
            self.probe.side_effect = [inventory(), result]
            with self.assertRaises(usb.PreflightError) as caught:
                self.setup()
            self.assertEqual(caught.exception.code, code)
        self.run.assert_not_called()

    def test_failed_open_claim_control_and_malformed_diagnostic(self):
        failures = [OSError('access denied'), subprocess.TimeoutExpired('usbboot', 15),
                    subprocess.CompletedProcess([], 1, '{"status":"claim","error":-6}'),
                    subprocess.CompletedProcess([], 0, 'bad JSON')]
        for failure in failures:
            self.probe.side_effect = [inventory(), inventory(True)]
            self.run.side_effect = failure if isinstance(failure, Exception) else None
            self.run.return_value = failure
            with self.subTest(failure=failure), self.assertRaises(usb.PreflightError) as caught:
                self.setup()
            self.assertEqual(caught.exception.code, 'E128')

    def test_already_bound_helper_result_is_reprobed(self):
        self.elevate.return_value = output('already_bound')
        self.assertIn('succeeded', self.setup()['message'])
        self.assertEqual(self.probe.call_count, 3)

    def test_cli_has_own_locked_action_and_no_automatic_flash(self):
        with mock.patch.object(cli, '_Lock', return_value=contextlib.nullcontext()) as lock, \
                mock.patch.object(cli.ui, 'info'), mock.patch.object(cli.ui, 'confirm', return_value=True):
            self.assertEqual(cli.main(['setup-driver']), 0)
        lock.assert_called_once_with()
        self.assertFalse(cli.flasher._WINDOWS_DEV_FLASH.get())
        self.install.assert_not_called()
        self.restore.assert_not_called()


class ResultTests(unittest.TestCase):
    def test_bad_or_inconsistent_results(self):
        bad = [subprocess.CompletedProcess([], 0, 'not JSON'),
               subprocess.CompletedProcess([], 0, output().stdout + '{}\n'),
               subprocess.CompletedProcess([], 0, '[]'),
               subprocess.CompletedProcess([], 0, output().stdout.replace('"schema": 1', '"schema":1,"schema":1')),
               subprocess.CompletedProcess([], 1, output().stdout)]
        bad += [output(**change) for change in ({'schema': True}, {'error': True}, {'error': -1},
                {'error': 2**32}, {'installed': 1}, {'installed': False}, {'reboot_required': True},
                {'status': 'unknown'}, {'status': []}, {'reason': 'bogus'}, {'error': 5})]
        for proc in bad:
            with self.subTest(proc=proc), self.assertRaises(usb.PreflightError) as caught:
                usb._helper_result(proc)
            self.assertEqual(caught.exception.code, 'E126')

    def test_inventory_failure_is_coded_and_description_handles_disconnected_states(self):
        with mock.patch.object(usb, 'inventory', side_effect=OSError('SetupAPI failed')):
            with self.assertRaises(usb.PreflightError) as caught:
                usb.driver_inventory()
        self.assertEqual(caught.exception.code, 'E126')
        for state in ('absent', 'phantom', 'ambiguous'):
            usb.driver_description(dict(state=state, count=0)).encode('ascii')


class ElevationTests(unittest.TestCase):
    def setUp(self):
        self.kernel, self.shell, self.adv, self.ole = mock.Mock(), mock.Mock(), mock.Mock(), mock.Mock()
        self.ole.CoInitializeEx.return_value = 0
        self.last_error = 997
        self.kernel.GetSystemDirectoryW.side_effect = self.system
        self.kernel.CreateNamedPipeW.return_value = 11
        self.kernel.CreateEventW.return_value = 17
        self.kernel.ConnectNamedPipe.return_value = False
        self.kernel.WaitForSingleObject.return_value = 0
        self.kernel.WaitForMultipleObjects.return_value = 0
        self.kernel.GetOverlappedResult.return_value = 1
        self.kernel.GetNamedPipeClientProcessId.side_effect = lambda handle, pid: self.set_value(pid, 44)
        self.kernel.GetProcessId.return_value = 44
        self.kernel.GetExitCodeProcess.side_effect = lambda handle, code: self.set_value(code, 0)
        self.kernel.ReadFile.side_effect = self.read
        self.shell.ShellExecuteExW.side_effect = self.launch
        self.adv.ConvertStringSecurityDescriptorToSecurityDescriptorW.side_effect = \
            lambda text, rev, sd, size: self.set_value(sd, 77)
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(mock.patch.object(usb.C, 'WinDLL', side_effect=lambda name, **kw:
                {'kernel32': self.kernel, 'shell32': self.shell, 'advapi32': self.adv, 'ole32': self.ole}[name], create=True))
        self.stack.enter_context(mock.patch.object(usb.C, 'get_last_error', side_effect=lambda: self.last_error, create=True))
        self.stack.enter_context(mock.patch.object(usb.C, 'WinError', side_effect=self.error, create=True))

    @staticmethod
    def set_value(pointer, value):
        pointer._obj.value = value
        return 1

    @staticmethod
    def system(buf, size):
        buf.value = r'C:\Windows\System32'
        return len(buf.value)

    @staticmethod
    def error(number):
        exc = OSError('Windows error')
        exc.winerror = number
        return exc

    def launch(self, pointer):
        self.info = pointer._obj
        self.info.hProcess = 33
        return 1

    @staticmethod
    def read(handle, buf, size, read, ov):
        data = output().stdout.encode('ascii')
        ctypes.memmove(buf, data, len(data))
        read._obj.value = len(data)
        return 1

    def test_fixed_elevated_launch_quotes_paths_and_checks_pipe_client(self):
        tool = 'C:\\bundle with space\\\u97f3\\bind_winusb.exe'
        result = usb._elevated_helper(tool)
        self.assertEqual(usb._helper_result(result)['status'], 'installed')
        self.assertEqual(self.info.lpFile, r'C:\Windows\System32\cmd.exe')
        self.assertEqual(self.info.lpVerb, 'runas')
        self.assertIn('/d /v:off /s /c ""' + tool + '" > "\\\\.\\pipe\\diskos-driver-', self.info.lpParameters)
        self.assertEqual(self.info.lpDirectory, 'C:\\bundle with space\\\u97f3')
        self.kernel.GetNamedPipeClientProcessId.assert_called_once()
        self.assertEqual(self.kernel.CloseHandle.call_args_list, [mock.call(11), mock.call(17), mock.call(33)])
        self.kernel.LocalFree.assert_called_once()
        self.ole.CoUninitialize.assert_called_once()
        self.assertEqual(ctypes.sizeof(usb._ShellInfo), 112)
        self.assertEqual(ctypes.sizeof(usb._Overlapped), 32)

    def test_expansion_or_argument_injection_refused_before_launch(self):
        for tool in ('bind_winusb.exe', r'\\server\share\bind_winusb.exe', r'C:bind_winusb.exe',
                     'C:\\%TEMP%\\bind_winusb.exe', 'C:\\wow!\\bind_winusb.exe',
                     'C:\\bad" & whoami & "\\bind_winusb.exe', 'C:\\bad\n\\bind_winusb.exe'):
            with self.subTest(tool=tool), self.assertRaises(ValueError):
                usb._elevated_helper(tool)
        self.shell.ShellExecuteExW.assert_not_called()

    def test_pipe_grants_rehearsed_redirect_access_and_has_no_client_read_endpoint(self):
        usb._elevated_helper(r'C:\bundle\bind_winusb.exe')
        # Windows rehearsal: GW fails cmd's redirect; GRGW succeeds. Keep the
        # known working grant. Reject remote connections by pipe mode;
        # local clients can legitimately carry the Network SID.
        convert = self.adv.ConvertStringSecurityDescriptorToSecurityDescriptorW
        self.assertEqual(convert.call_args.args[0],
                         'D:(D;;GA;;;AN)(A;;GRGW;;;WD)')
        args = self.kernel.CreateNamedPipeW.call_args.args
        # An inbound pipe has only a write endpoint for clients, regardless of
        # the DACL. A single instance prevents a second server from reading.
        self.assertEqual(args[1], 0x40080001)  # OVERLAPPED | FIRST_PIPE_INSTANCE | INBOUND
        self.assertEqual(args[2], 8)  # PIPE_REJECT_REMOTE_CLIENTS
        self.assertEqual(args[3], 1)
        self.assertFalse(args[7]._obj.inherit)

    def test_uac_denial_cleans_pending_pipe(self):
        self.last_error = 1223
        self.kernel.ConnectNamedPipe.return_value = True
        self.shell.ShellExecuteExW.side_effect = lambda pointer: 0
        with self.assertRaises(OSError) as caught:
            usb._elevated_helper(r'C:\bundle\bind_winusb.exe')
        self.assertEqual(caught.exception.winerror, 1223)
        self.kernel.CancelIoEx.assert_called_once_with(11, None)
        self.assertEqual(self.kernel.CloseHandle.call_args_list, [mock.call(11), mock.call(17)])

    def test_spoofed_client_and_timeout_stop_without_reading(self):
        self.kernel.GetProcessId.return_value = 99
        with self.assertRaisesRegex(OSError, 'another process'):
            usb._elevated_helper(r'C:\bundle\bind_winusb.exe')
        self.kernel.ReadFile.assert_not_called()
        self.kernel.GetProcessId.return_value = 44
        self.kernel.WaitForSingleObject.return_value = 258
        with self.assertRaisesRegex(OSError, 'timed out'):
            usb._elevated_helper(r'C:\bundle\bind_winusb.exe')
        self.kernel.ReadFile.assert_not_called()

    def test_exit_before_connect_and_invalid_system_directory_cleanup(self):
        self.kernel.WaitForMultipleObjects.return_value = 1
        with self.assertRaisesRegex(OSError, 'exited without output'):
            usb._elevated_helper(r'C:\bundle\bind_winusb.exe')
        self.kernel.ReadFile.assert_not_called()
        self.kernel.GetSystemDirectoryW.side_effect = None
        self.kernel.GetSystemDirectoryW.return_value = 0
        self.shell.ShellExecuteExW.reset_mock()
        with self.assertRaisesRegex(OSError, 'system directory'):
            usb._elevated_helper(r'C:\bundle\bind_winusb.exe')
        self.shell.ShellExecuteExW.assert_not_called()


class GuiDriverTests(unittest.TestCase):
    def test_remove_panel_text_per_platform(self):
        prefix = "Deletes cached firmware, partial downloads, saved recovery images, installer state and owned GUI logs from THIS computer. "
        suffix = "If diskOS is still on the device, use 'Restore stock firmware' first."
        for host in ('windows', 'linux', 'macos'):
            with self.subTest(host=host):
                app = self.app()
                app.mode.get.return_value = 'remove'
                app.cfg.winfo_children.return_value = []
                app.ttk = mock.Mock()
                with mock.patch.object(gui.platform_probe, 'host', return_value=(host, 'x86_64')), \
                        mock.patch.object(gui.gui_theme, 'rule'), mock.patch.object(gui.gui_theme, 'kicker'):
                    app._build_config()                 # hairline + kicker are real Tk widgets: stubbed here
                text = app.ttk.Label.call_args.kwargs['text']
                middle = ("The boot-mode driver choice stays. To undo: Device Manager > "
                          "boot-mode device > Uninstall device; leave the driver-package box unticked. "
                          if host == 'windows' else "Nothing was installed system-wide. ")
                self.assertEqual(text, prefix + middle + suffix)
                self.assertTrue(text.isascii())

    def app(self):
        app = gui.App.__new__(gui.App)
        app.state, app.flashing, app._driver_running = gui.IDLE, False, False
        app._inventory_running, app._inventory_worker = False, None
        app._ready_running, app._ready_gen = False, 0
        app._ready_probe = app._ready_cancel = app._ready_view = None
        app._closing = False
        app.ready_frame, app.ready_title = mock.Mock(), mock.Mock()
        app._ready_render = mock.Mock()
        app.q = queue.Queue()
        app.reporter = QueueReporter(app.q)
        for name in ('root', 'phase_lbl', 'status_lbl', 'driver_lbl', 'host_lbl', 'driver_btn',
                     'action_btn', 'mode_frame', 'cfg', 'log', 'bar', 'warn_banner', 'mode'):
            setattr(app, name, mock.Mock())
        app._set_controls = mock.Mock()
        app._clear_finish_extra = mock.Mock()
        app._worker_confirm = mock.Mock(return_value=True)
        app._refresh_state_labels = mock.Mock()
        app._log = mock.Mock()
        return app

    def test_worker_and_completion_never_enter_flash_flow(self):
        for result in ({'message': 'Access proved'}, {'reboot_required': True, 'message': 'Restart Windows'},
                       {'aborted': True, 'message': 'Cancelled'}):
            app = self.app()
            def setup(report, confirm):
                report('service=none, INF=none, provider=none, problem code=28')
                self.assertTrue(confirm())
                return result
            with mock.patch.object(gui.windows_usb, 'setup_driver', side_effect=setup), \
                    mock.patch.object(app, '_finish') as flash_finish, mock.patch.object(app, '_on_action') as action:
                app._on_driver_setup()
                app.worker.join(2)
                self.assertFalse(app.worker.is_alive())
                self.assertTrue(app._driver_running)
                while not app.q.empty():
                    app._handle(*app.q.get_nowait())
                self.assertEqual(app.state, gui.IDLE)
                self.assertFalse(app._driver_running)
                self.assertFalse(app.flashing)
                flash_finish.assert_not_called()
                action.assert_not_called()
                app._set_controls.assert_has_calls([mock.call(False), mock.call(True)])
                app.status_lbl.config.assert_called_with(text=result['message'], wraplength=520)
                self.assertIn('No flash', app._worker_confirm.call_args.args[0]['requires'])

    def test_driver_setup_cannot_overlap_an_operation(self):
        app = self.app()
        app.state = gui.RUNNING
        with mock.patch.object(gui.windows_usb, 'setup_driver') as setup:
            app._on_driver_setup()
        setup.assert_not_called()
        app._set_controls.assert_not_called()

    def test_worker_error_clears_setup_state(self):
        app = self.app()
        with mock.patch.object(gui.windows_usb, 'setup_driver', side_effect=usb.PreflightError('cancelled', code='E118')):
            app._on_driver_setup()
            app.worker.join(2)
        with mock.patch.object(app, '_fail') as fail:
            app._handle(*app.q.get_nowait())
        self.assertFalse(app._driver_running)
        fail.assert_called_once_with('[E118] cancelled')

    def test_window_close_waits_for_driver_setup(self):
        app = self.app()
        app._driver_running = True
        with mock.patch('tkinter.messagebox.showwarning') as warning:
            app._on_close()
        warning.assert_called_once()
        app.root.destroy.assert_not_called()

    def test_windows_header_uses_inventory_and_displays_all_driver_fields(self):
        app = self.app()
        with mock.patch.object(gui.platform_probe, 'host', return_value=('windows', 'x86_64')), \
                mock.patch.object(gui.windows_usb, 'driver_inventory', return_value=inventory(True)), \
                mock.patch.object(gui.readiness, '_tools', return_value=[]), \
                mock.patch.object(gui.platform_probe, 'maskrom_count') as count:
            gui.App._refresh_state_labels(app)
            app._inventory_worker.join(2)
            self.assertFalse(app._inventory_worker.is_alive())
            for expected in ('driver_inventory', 'ready'):
                kind, payload = app.q.get(timeout=2)
                self.assertEqual(kind, expected)
                app._handle(kind, payload)
        text = app.driver_lbl.config.call_args.kwargs['text']
        for detail in ('service=WINUSB', 'INF=winusb.inf', 'provider=Microsoft', 'problem code=0'):
            self.assertIn(detail, text)
        app.driver_lbl.pack.assert_called_once_with(anchor='w', pady=(6, 2))
        self.assertFalse(app._ready_running)
        count.assert_not_called()

    def test_windows_driver_header_is_visible_after_inventory(self):
        try:
            import tkinter as tk
            root = tk.Tk()
        except Exception as e:
            self.skipTest('no Tk display: %s' % e)
        root.withdraw()
        self.addCleanup(root.destroy)
        with mock.patch.object(gui, 'ota_key_available', return_value=True), \
                mock.patch.object(gui.platform_probe, 'host', return_value=('windows', 'x86_64')), \
                mock.patch.object(gui.windows_usb, 'driver_inventory', return_value=inventory(True)), \
                mock.patch.object(gui.readiness, '_tools', return_value=[]), \
                mock.patch.object(gui.state, 'stock_image_exists', return_value=False):
            app = gui.App(root)
            app._inventory_worker.join(2)
            self.assertFalse(app._inventory_worker.is_alive())
            for expected in ('driver_inventory', 'ready'):
                kind, payload = app.q.get(timeout=2)
                self.assertEqual(kind, expected)
                app._handle(kind, payload)
            root.deiconify()
            root.update_idletasks()
            self.assertTrue(app.driver_lbl.winfo_ismapped())
            text = str(app.driver_lbl.cget('text'))
            for detail in ('service=WINUSB', 'INF=winusb.inf', 'provider=Microsoft', 'problem code=0'):
                self.assertIn(detail, text)

    def test_startup_and_periodic_inventory_use_one_worker_and_queue(self):
        caller = threading.get_ident()
        entered, release = threading.Event(), threading.Event()
        ready_entered, ready_release = threading.Event(), threading.Event()
        calls, ready_calls = [], []
        inv = inventory(True)
        real_probe = gui.readiness.probe
        def probe():
            calls.append(threading.get_ident())
            entered.set()
            release.wait(5)
            return inv
        def ready_probe(found):
            ready_calls.append(threading.get_ident())
            self.assertIs(found, inv)
            ready_entered.set()
            ready_release.wait(5)
            return real_probe(found)
        def widgets(app):
            app.host_lbl, app.driver_lbl = mock.Mock(), mock.Mock()
            app.ready_frame, app.ready_title = mock.Mock(), mock.Mock()
        with mock.patch.object(gui.App, '_build_style'), \
                mock.patch.object(gui.App, '_build_widgets', autospec=True, side_effect=widgets), \
                mock.patch.object(gui.App, '_ready_render') as render, \
                mock.patch.object(gui.platform_probe, 'host', return_value=('windows', 'x86_64')), \
                mock.patch.object(gui.windows_usb, 'driver_inventory', side_effect=probe) as scan, \
                mock.patch.object(gui.readiness, 'probe', side_effect=ready_probe) as ready, \
                mock.patch.object(gui.readiness, '_tools', return_value=[]):
            app = None
            try:
                app = gui.App(mock.Mock())
                self.assertTrue(entered.wait(2))
                self.assertNotEqual(calls, [caller])
                app.driver_lbl.config.assert_not_called()
                self.assertTrue(app._inventory_running)
                for _ in range(3):
                    app._tick_device()
                    app._on_ready_refresh()
                self.assertEqual(scan.call_count, 1)
                release.set()
                self.assertTrue(ready_entered.wait(2))
                app.driver_lbl.config.assert_not_called()
                kind, payload = app.q.get(timeout=2)
                self.assertEqual(kind, 'driver_inventory')
                app._handle(kind, payload)
                app.driver_lbl.config.assert_called_once()
                self.assertTrue(app._ready_running)
                self.assertTrue(app._inventory_worker.is_alive())
                # Inventory is drained, but readiness still owns the same worker.
                for _ in range(3):
                    app._tick_device()
                    app._on_ready_refresh()
                self.assertEqual(scan.call_count, 1)
                self.assertEqual(ready.call_count, 1)
                ready_release.set()
                app._inventory_worker.join(2)
                self.assertFalse(app._inventory_worker.is_alive())
                app._tick_device()  # Completed but not drained: still no overlap.
                app._on_ready_refresh()
                self.assertEqual(scan.call_count, 1)
                kind, payload = app.q.get(timeout=2)
                self.assertEqual(kind, 'ready')
                app._handle(kind, payload)
                self.assertFalse(app._ready_running)
                self.assertFalse(app._inventory_running)
                render.assert_called_once_with()
                entered.clear()
                app._tick_device()
                self.assertTrue(entered.wait(2))
                app._inventory_worker.join(2)
                self.assertFalse(app._inventory_worker.is_alive())
                self.assertEqual(scan.call_count, 2)
                self.assertEqual(ready.call_count, 2)
                self.assertEqual(calls, ready_calls)
                self.assertTrue(all(thread != caller for thread in calls))
                for expected in ('driver_inventory', 'ready'):
                    kind, payload = app.q.get(timeout=2)
                    self.assertEqual(kind, expected)
                    app._handle(kind, payload)
                self.assertFalse(app._ready_running)
                self.assertFalse(app._inventory_running)
                self.assertEqual(app.driver_lbl.config.call_count, 2)
                self.assertEqual(render.call_count, 2)
                app.state = gui.RUNNING
                app._tick_device()
                app._on_ready_refresh()
                app._ready_start()
                self.assertEqual(scan.call_count, 2)
                self.assertEqual(ready.call_count, 2)
            finally:
                release.set()
                ready_release.set()
                if app is not None and app._inventory_worker is not None:
                    app._inventory_worker.join(2)


if __name__ == '__main__':
    unittest.main()
