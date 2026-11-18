# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile and exercise the real helper with a device-free SetupAPI mock."""
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('gcc') and os.name != 'nt', 'POSIX GCC mock harness')
class BindWinUSBTests(unittest.TestCase):
    no_crt = False

    @classmethod
    def setUpClass(cls):
        # Keep the harness as evidence; no cleanup/deletion is needed.
        cls.work = Path(tempfile.mkdtemp(prefix='win-s5-'))
        cls.exe = cls.work / 'bind-mock'
        subprocess.run(['gcc', '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror',
                        *(['-DBIND_NOCRT', '-fno-builtin'] if cls.no_crt else []),
                        '-DBIND_MOCK', '-I' + str(ROOT / 'tests'),
                        str(ROOT / 'src/windows-driver/bind_winusb.c'), '-o', str(cls.exe)],
                       check=True, capture_output=True, text=True)

    def run_case(self, case='ok', *args, env=None):
        p = subprocess.run([str(self.exe), *args], env={**os.environ, 'BIND_CASE': case,
                           'BIND_COMMAND': subprocess.list2cmdline([str(self.exe), *args]),
                           **(env or {})}, capture_output=True, text=True, timeout=3)
        self.assertEqual(len(p.stdout.splitlines()), 1, p.stdout + p.stderr)
        data = json.loads(p.stdout)
        self.assertEqual(data['schema'], 1)
        self.assertEqual(set(data), {'schema', 'status', 'reason', 'error', 'installed', 'reboot_required'})
        self.assertRegex(data['reason'], r'^[a-z][a-z_]*$')
        if p.returncode:
            self.assertNotEqual(data['reason'], 'none')
        else:
            self.assertEqual(data['reason'], 'none')
        self.assertIs(type(data['installed']), bool)
        self.assertIs(type(data['reboot_required']), bool)
        return p, data, p.stderr.splitlines()

    def test_install_sequence_and_revision_ids(self):
        for case in ('ok', 'revision'):
            p, data, calls = self.run_case(case)
            self.assertEqual(p.returncode, 0)
            self.assertEqual(data['status'], 'installed')
            self.assertEqual(calls, ['loader_policy', *(['load_system_dll'] * 4),
                'inventory', 'system_path', 'get_params', 'set_params',
                'build_list', 'enum_driver', 'detail', 'enum_driver', 'select', 'inventory',
                'final_snapshot', 'install', 'get_params', 'destroy_drivers', 'destroy_set', 'destroy_set'])

    def test_hostile_arguments_never_inventory_or_install(self):
        for args in (('--help',), ('--inf', 'evil.inf'), ('--command', 'cmd.exe'),
                     ('USB\\VID_A108&PID_EAEF\\ONE',), ('--bind',), ('',),
                     (';calc.exe',), ('winusb.inf',), ('--',)):
            with self.subTest(args=args):
                p, data, calls = self.run_case('ok', *args)
                self.assertEqual(p.returncode, 1)
                self.assertEqual(data['status'], 'arguments_refused')
                self.assertEqual(data['reason'], 'arguments_refused')
                self.assertEqual(calls, ['loader_policy', *(['load_system_dll'] * 4)])

    def test_loader_failures_refuse_before_inventory(self):
        for case in ('policy_error', 'load_error', 'resolve_error',
                     'newdev_load_error', 'newdev_resolve_error'):
            with self.subTest(case=case):
                p, data, calls = self.run_case(case)
                self.assertEqual(p.returncode, 1)
                self.assertEqual(data['status'], 'loader_policy_refused')
                self.assertEqual(data['reason'], 'loader_policy_refused')
                self.assertFalse(data['installed'])
                self.assertNotIn('inventory', calls)
                self.assertEqual(calls[0], 'loader_policy')
                if case == 'policy_error': self.assertEqual(calls, ['loader_policy'])
                self.assertEqual(data['error'], 127 if 'resolve' in case else 5)

    def test_identity_driver_environment_and_package_refusals(self):
        reasons = {
            'not_unique': ('absent', 'ambiguous', 'wrong_instance'),
            'invalid_target': ('wrong_ids', 'interface_id', 'revision_only', 'missing_location',
                               'bad_type', 'bad_size', 'unterminated', 'cm_error', 'instance_read_error'),
            'unfamiliar_driver': ('foreign_driver', 'foreign_inf', 'foreign_provider', 'problem_code'),
            'inventory_error': ('enum_error', 'device_enum_error'),
            'elevation_required': ('token_error', 'not_admin'),
            'system_path_refused': ('bad_system_path', 'system_path_too_long'),
            'inf_unavailable': ('inf_reparse', 'inf_missing', 'inf_directory'),
            'inf_path_too_long': ('inf_path_too_long',),
            'get_params_failed': ('get_params_error',),
            'set_params_failed': ('set_params_error',),
            'build_list_failed': ('build_error',),
            'driver_enum_failed': ('driver_enum_error',),
            'driver_detail_failed': ('detail_error', 'detail_insufficient'),
            'invalid_driver_detail': ('detail_short', 'detail_oversize', 'detail_odd_size',
                                      'section_unterminated', 'ids_unterminated', 'ids_empty'),
            'no_winusb_node': ('no_driver', 'other_models', 'wrong_section', 'wrong_node_id',
                               'node_id_suffix', 'node_id_prefix'),
            'ambiguous_winusb_node': ('two_drivers',),
            'provider_mismatch': ('node_provider', 'provider_unterminated'),
            'inf_mismatch': ('node_inf', 'inf_unterminated'),
            'select_driver_failed': ('select_error',),
        }
        for reason, cases in reasons.items():
            for case in cases:
                with self.subTest(case=case):
                    p, data, calls = self.run_case(case)
                    self.assertEqual(p.returncode, 1)
                    self.assertEqual(data['reason'], reason)
                    self.assertFalse(data['installed'])
                    self.assertNotIn('install', calls)

    def test_three_model_inf_selects_only_winusb_in_any_order(self):
        for case in ('three_models', 'three_models_first', 'three_models_last'):
            with self.subTest(case=case):
                p, data, calls = self.run_case(case)
                self.assertEqual((p.returncode, data['status'], data['installed']), (0, 'installed', True))
                self.assertEqual(calls.count('detail'), 3)
                self.assertEqual(calls.count('enum_driver'), 4)
                self.assertEqual(calls.count('select'), 1)
                self.assertEqual(calls.count('install'), 1)

    def test_duplicate_winusb_and_only_other_models_never_select(self):
        for case, reason in (('two_drivers', 'ambiguous_winusb_node'),
                             ('other_models', 'no_winusb_node')):
            with self.subTest(case=case):
                p, data, calls = self.run_case(case)
                self.assertEqual(p.returncode, 1)
                self.assertEqual(data['reason'], reason)
                self.assertEqual(calls.count('detail'), 2)
                self.assertNotIn('select', calls)
                self.assertNotIn('install', calls)

    def test_node_matching_is_case_insensitive_and_searches_id_list(self):
        for case in ('node_case', 'node_id_list'):
            with self.subTest(case=case):
                p, data, calls = self.run_case(case)
                self.assertEqual((p.returncode, data['status']), (0, 'installed'))
                self.assertEqual(calls.count('select'), 1)

    def test_documented_hardware_and_compatible_id_layouts(self):
        for case in ('ok', 'ids_single_null', 'hardware_offset_one', 'compatible_only',
                     'both_ids', 'both_hardware_match'):
            with self.subTest(case=case):
                p, data, calls = self.run_case(case)
                self.assertEqual((p.returncode, data['status'], data['installed']), (0, 'installed', True))
                self.assertEqual(calls.count('select'), 1)
                self.assertEqual(calls.count('install'), 1)

    def test_malformed_id_ranges_and_terminators_refuse_with_reason(self):
        for case in ('offset_beyond', 'offset_huge', 'length_beyond', 'length_huge',
                     'length_without_offset', 'hardware_truncated', 'compat_bad_offset_one',
                     'compat_bad_single_null', 'compat_bad_unterminated', 'compat_bad_empty',
                     'compat_bad_trailing'):
            with self.subTest(case=case):
                p, data, calls = self.run_case(case)
                self.assertEqual((p.returncode, data['status'], data['reason']),
                                 (1, 'refused', 'invalid_driver_detail'))
                self.assertFalse(data['installed'])
                self.assertNotIn('select', calls)
                self.assertNotIn('install', calls)

    def test_id_matches_outside_declared_strings_are_ignored(self):
        for case in ('hardware_padding', 'compat_padding', 'both_no_match'):
            with self.subTest(case=case):
                p, data, calls = self.run_case(case)
                self.assertEqual((p.returncode, data['reason']), (1, 'no_winusb_node'))
                self.assertNotIn('select', calls)
                self.assertNotIn('install', calls)

    def test_final_inventory_rejects_changes_and_disappearance(self):
        for case in ('disappear', 'new_device', 'instance_change', 'location_change',
                     'driver_change', 'devnode_change', 'ids_change', 'inf_change', 'provider_change'):
            with self.subTest(case=case):
                p, data, calls = self.run_case(case)
                self.assertEqual(p.returncode, 1)
                self.assertEqual(data['status'], 'target_changed')
                self.assertEqual(data['reason'], 'target_changed')
                self.assertNotIn('install', calls)
                self.assertEqual(calls.count('inventory'), 2)

    def test_original_target_is_reread_after_final_enumeration(self):
        for change in ('instance_change', 'location_change', 'ids_change', 'driver_change',
                       'inf_change', 'provider_change', 'problem_change', 'cm_error', 'read_error'):
            with self.subTest(change=change):
                p, data, calls = self.run_case('boundary_' + change)
                self.assertEqual(p.returncode, 1)
                self.assertEqual(data['status'], 'target_changed')
                self.assertEqual(data['reason'], 'target_changed')
                self.assertFalse(data['installed'])
                self.assertNotIn('install', calls)
                self.assertEqual(calls.count('inventory'), 2)
                self.assertEqual(calls.count('final_snapshot'), 1)

    def test_existing_inbox_binding_is_rechecked_without_install(self):
        p, data, calls = self.run_case('bound')
        self.assertEqual(p.returncode, 0)
        self.assertEqual(data['status'], 'already_bound')
        self.assertFalse(data['installed'])
        self.assertEqual(calls.count('inventory'), 2)
        self.assertNotIn('install', calls)

    def test_reboot_restart_install_failure_and_unknown_status(self):
        for case, status, rc, installed, reboot in (
                ('reboot', 'reboot_required', 0, True, True),
                ('restart', 'reboot_required', 0, True, True),
                ('api_reboot', 'reboot_required', 0, True, True),
                ('install_error', 'api_error', 1, False, False),
                ('install_error_raw', 'api_error', 1, False, False),
                ('post_error', 'installed_status_unknown', 1, True, True),
                ('api_reboot_post_error', 'installed_status_unknown', 1, True, True)):
            with self.subTest(case=case):
                p, data, calls = self.run_case(case)
                self.assertEqual((p.returncode, data['status'], data['installed'],
                                  data['reboot_required']), (rc, status, installed, reboot))
                self.assertEqual(data['reason'], 'install_failed' if case.startswith('install_error') else
                                 'installed_status_unknown' if case.endswith('post_error') else 'none')
                self.assertEqual(data['error'], 3758096900 if case == 'install_error_raw' else
                                 5 if rc else 0)
                self.assertEqual(calls.count('install'), 1)

    def test_caller_environment_cannot_choose_package_or_command(self):
        p, data, calls = self.run_case(env={'SystemRoot': 'Z:\\evil', 'WINDIR': 'relative',
            'COMSPEC': 'evil.exe', 'INF': 'evil.inf', 'PATH': '', 'TEMP': 'relative'})
        self.assertEqual(p.returncode, 0)
        self.assertEqual(data['status'], 'installed')
        self.assertEqual(calls.count('install'), 1)


class BindNoCRTTests(BindWinUSBTests):
    no_crt = True

    def test_windows_executable_token_and_argument_refusal(self):
        for command, accepted in (
                ('C:\\tools\\bind_winusb.exe', True),
                ('"C:\\space dir\\bind_winusb.exe" \t', True),
                ('C:\\space" dir"\\bind_winusb.exe', True),
                ('"C:\\space dir\\bind_winusb.exe" ""', False),
                ('bind_winusb.exe\t--help', False),
                ('bind_winusb.exe "unterminated', False)):
            with self.subTest(command=command):
                p, data, calls = self.run_case(env={'BIND_COMMAND': command})
                self.assertEqual(p.returncode, 0 if accepted else 1)
                self.assertEqual(data['status'], 'installed' if accepted else 'arguments_refused')
                self.assertEqual('install' in calls, accepted)


class BindBundleTests(unittest.TestCase):
    @staticmethod
    def helper_pe(imports):
        from test_windows_host import pe
        data = pe(imports)
        struct.pack_into('<H', data, 132, 0x8664)
        return data

    def test_helper_import_policy_is_closed_and_os_names_cannot_be_shadowed(self):
        from diskos_installer import bundle
        self.assertIn('newdev.dll', bundle._WINDOWS_BIND_OS)
        work = Path(tempfile.mkdtemp(prefix='win-s5-pe-'))
        exe = work / 'bind_winusb.exe'
        for name in bundle._WINDOWS_BIND_IMPORTS:
            exe.write_bytes(self.helper_pe([name]))
            bundle._verify_windows_dlls(exe)
        for name in bundle._WINDOWS_BIND_OS | bundle._WINDOWS_USBBOOT_UCRT | {
                'libusb-1.0.dll', 'msys-2.0.dll', 'private.dll', 'shell32.dll',
                'ucrtbase.dll', 'ntdll.dll', 'api-ms-win-crt-future-l1-1-0.dll'}:
            exe.write_bytes(self.helper_pe([name]))
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'unexpected binding helper import'):
                bundle._verify_windows_dlls(exe)
        for name in bundle._WINDOWS_BIND_OS | bundle._WINDOWS_BIND_IMPORTS | bundle._WINDOWS_USBBOOT_UCRT:
            exe.write_bytes(self.helper_pe(['kernel32.dll']))
            (work / name).write_bytes(self.helper_pe([]))
            with self.subTest(shadow=name), self.assertRaisesRegex(ValueError, 'shadows an OS name'):
                bundle._verify_windows_dlls(exe)

    def test_helper_rejects_empty_import_table_wrong_machine_and_delay_imports(self):
        from diskos_installer import bundle
        work = Path(tempfile.mkdtemp(prefix='win-s5-pe-'))
        exe = work / 'bind_winusb.exe'
        for kind in ('empty', 'missing_directory', 'machine', 'delay'):
            data = self.helper_pe([] if kind == 'empty' else ['kernel32.dll'])
            if kind == 'missing_directory': struct.pack_into('<II', data, 272, 0, 0)
            if kind == 'machine': struct.pack_into('<H', data, 132, 0x14c)
            if kind == 'delay': struct.pack_into('<II', data, 264 + 13 * 8, 0x1000, 20)
            exe.write_bytes(data)
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                bundle._verify_windows_dlls(exe)

    def test_build_recipe_driver_import_gate_matches_bundle_policy(self):
        import re
        from diskos_installer import bundle
        candidates = ([r'C:\msys64\usr\bin\bash.exe'] if os.name == 'nt' else [])
        candidates += [shutil.which(name) for name in ('bash', 'sh')]
        shell = None
        failures = []
        for candidate in dict.fromkeys(filter(None, candidates)):
            try:
                probe = subprocess.run([candidate, '-s', '--', 'shell-ready'],
                    input=b'shell_probe() { local value=$1; printf "%s" "$value"; }\nshell_probe "$1"\n',
                    capture_output=True, timeout=10)
            except (OSError, subprocess.TimeoutExpired) as exc:
                failures.append(f'{candidate}: {exc}')
                continue
            if probe.returncode == 0 and probe.stdout == b'shell-ready':
                shell = candidate
                break
            failures.append(f'{candidate}: exit {probe.returncode}, stderr={probe.stderr!r}')
        if shell is None:
            reason = 'No working POSIX shell for build recipe gate: ' + ('; '.join(failures) or 'bash/sh not on PATH')
            if os.name == 'nt':
                self.skipTest(reason)
            self.fail(reason)
        recipe = (ROOT / 'build/build-windows-tools.sh').read_text()
        body = re.search(r'^import_allowed\(\) \{\n.*?^\}\n', recipe, re.S | re.M).group(0)
        for name in bundle._WINDOWS_BIND_IMPORTS | bundle._WINDOWS_BIND_OS | bundle._WINDOWS_USBBOOT_UCRT | {'private.dll'}:
            p = subprocess.run([shell, '-s', '--', name],
                               input=(body + 'import_allowed bind_winusb.exe "$1"\n').encode('ascii'),
                               capture_output=True, timeout=10)
            with self.subTest(name=name):
                self.assertIn(p.returncode, (0, 1), p.stderr.decode('ascii', 'backslashreplace'))
                self.assertEqual(p.returncode == 0, name in bundle._WINDOWS_BIND_IMPORTS,
                                 p.stderr.decode('ascii', 'backslashreplace'))

    def test_helper_system_imports_are_not_granted_to_other_tools(self):
        from diskos_installer import bundle
        from test_windows_host import pe
        for name in bundle._WINDOWS_BIND_OS:
            work = Path(tempfile.mkdtemp(prefix='win-s5-pe-'))
            exe = work / 'usbboot.exe'
            exe.write_bytes(pe([name]))
            with self.assertRaises(FileNotFoundError):
                bundle._verify_windows_dlls(exe)

    def test_manifest_requests_x64_elevation(self):
        import xml.etree.ElementTree as ET
        root = ET.parse(ROOT / 'src/windows-driver/bind_winusb.manifest').getroot()
        identity = root.find('{urn:schemas-microsoft-com:asm.v1}assemblyIdentity')
        level = root.find('.//{urn:schemas-microsoft-com:asm.v3}requestedExecutionLevel')
        self.assertEqual(identity.get('processorArchitecture'), 'amd64')
        self.assertEqual(level.attrib, {'level': 'requireAdministrator', 'uiAccess': 'false'})

    def test_driver_manifest_rejects_missing_duplicate_and_stale_hashes(self):
        import hashlib
        from unittest import mock
        from diskos_installer import bundle
        from diskos_installer.errors import FlashError
        work = Path(tempfile.mkdtemp(prefix='win-s5-manifest-'))
        vendor = work / 'vendor/windows-x86_64'
        vendor.mkdir(parents=True)
        sources = ('src/windows-driver/bind_winusb.c', 'src/windows-driver/bind_winusb.rc',
                   'src/windows-driver/bind_winusb.manifest', 'build/build-windows-driver.sh')
        lines = {}
        for name in (*sources, 'bind_winusb.exe'):
            path = work / name if '/' in name else vendor / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(self.helper_pe(['kernel32.dll']) if name == 'bind_winusb.exe' else b'fixture')
            lines[name] = hashlib.sha256(path.read_bytes()).hexdigest() + '  ' + name + '\n'
        manifest = vendor / 'MANIFEST.txt'
        def write(changes=None):
            data = {**lines, **(changes or {})}
            manifest.write_text('\nInput SHA256 (paths relative to installer):\n' +
                ''.join(data[n] for n in sources) + '\nOutput SHA256:\n' + data['bind_winusb.exe'],
                encoding='ascii')
        with mock.patch.object(bundle, 'resource_root', return_value=str(work)), mock.patch.object(
                bundle, 'vendor_dir', return_value=str(vendor)):
            with self.assertRaises(FlashError): bundle.verify_windows_driver_bundle()
            write()
            bundle.verify_windows_driver_bundle()
            manifest.write_text(manifest.read_text().replace('  ', ' *'), encoding='ascii')
            bundle.verify_windows_driver_bundle()
            for name, line in lines.items():
                for wrong in ('', line * 2, '0' * 64 + line[64:]):
                    write({name: wrong})
                    with self.subTest(name=name, wrong=wrong), self.assertRaises(FlashError):
                        bundle.verify_windows_driver_bundle()
            write()
            (vendor / 'bind_winusb.exe').write_bytes(b'changed')
            with self.assertRaises(FlashError): bundle.verify_windows_driver_bundle()
            # Matching source/output hashes cannot authorize pre-entry DLL loads.
            (vendor / 'bind_winusb.exe').write_bytes(self.helper_pe(['kernel32.dll', 'setupapi.dll']))
            lines['bind_winusb.exe'] = hashlib.sha256((vendor / 'bind_winusb.exe').read_bytes()).hexdigest() + '  bind_winusb.exe\n'
            write()
            with self.assertRaisesRegex(FlashError, 'loader policy'):
                bundle.verify_windows_driver_bundle()
