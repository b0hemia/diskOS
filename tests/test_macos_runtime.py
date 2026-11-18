# SPDX-License-Identifier: GPL-3.0-or-later
"""macOS boundaries tested offline with injected commands; no USB or display."""
import importlib.util
from contextlib import ExitStack
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

from diskos_installer import bundle, firmware_resolve, imagebuild, macos_runtime, platform_probe
from diskos_installer.errors import FlashError, PreflightError

ROOT = Path(__file__).resolve().parents[1]


def load_build(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'build' / (name + '.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


links = load_build('macos_links')


class RuntimeTests(unittest.TestCase):
    def setUp(self):
        macos_runtime._prepared.clear()
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        resource_root = mock.patch.object(bundle, 'resource_root', return_value=str(self.root))
        resource_root.start()
        self.addCleanup(resource_root.stop)
        self.vendor = self.root / 'vendor/macos-arm64'
        (self.vendor / 'lib').mkdir(parents=True)
        for name in ('usbboot', 'mksquashfs', 'unsquashfs', 'lib/libusb-1.0.0.dylib'):
            (self.vendor / name).write_bytes(b'tool')
        self.run = mock.Mock(return_value=types.SimpleNamespace(stdout='com.apple.quarantine\n'))

    def test_quarantine_only_known_files(self):
        (self.vendor / 'private.txt').write_text('leave alone')
        with mock.patch.object(bundle, 'resource_root', return_value=str(self.root)):
            macos_runtime.prepare_tools(self.vendor, self.run)
            macos_runtime.prepare_tools(self.vendor, self.run)
        self.assertEqual(self.run.call_count, 8)
        for call in self.run.call_args_list:
            self.assertEqual(call.args[0][0], '/usr/bin/xattr')
            self.assertNotIn('-r', call.args[0])
            self.assertNotIn('private.txt', call.args[0][-1])

    def test_absent_attribute_does_not_delete(self):
        self.run.return_value.stdout = 'user.other\n'
        macos_runtime.prepare_tools(self.vendor, self.run)
        self.assertEqual(self.run.call_count, 4)
        self.assertTrue(all('-d' not in call.args[0] for call in self.run.call_args_list))

    def test_quarantine_denied_has_recovery(self):
        self.run.side_effect = subprocess.CalledProcessError(1, 'xattr')
        with self.assertRaises(PreflightError) as caught:
            macos_runtime.prepare_tools(self.vendor, self.run)
        self.assertIn('writable folder', str(caught.exception))
        self.assertIn('fresh complete release', str(caught.exception))
        self.assertNotIn('xattr -dr', str(caught.exception))

    def test_external_vendor_directory_symlink_refused(self):
        package = self.root / 'checkout'
        alias = package / 'vendor/macos-arm64'
        alias.parent.mkdir(parents=True)
        alias.symlink_to(self.vendor, target_is_directory=True)
        with mock.patch.object(bundle, 'resource_root', return_value=str(package)):
            with self.assertRaises(PreflightError):
                macos_runtime.prepare_tools(alias, self.run)
        self.run.assert_not_called()

    def test_external_symlink_refused(self):
        outside = self.root / 'external.dylib'
        outside.write_bytes(b'outside')
        (self.vendor / 'lib/escape.dylib').symlink_to(outside)
        with self.assertRaises(PreflightError):
            macos_runtime.prepare_tools(self.vendor, self.run)
        self.assertFalse(any(str(outside) == c.args[0][-1] for c in self.run.call_args_list))

    def process(self):
        proc = mock.Mock()
        proc.wait.side_effect = [subprocess.TimeoutExpired('caffeinate', .1), 0]
        proc.poll.return_value = None
        return proc

    def test_caffeinate_scoped_pid_and_log(self):
        proc, rep = self.process(), mock.Mock()
        popen = mock.Mock(return_value=proc)
        with macos_runtime.NoSleep(rep, popen, 123):
            rep.log.assert_called_once()
            self.assertIn('sleep prevention active', rep.log.call_args.args[0])
        self.assertEqual(popen.call_args.args[0], ['/usr/bin/caffeinate', '-dims', '-w', '123'])
        proc.terminate.assert_called_once()

    def test_caffeinate_early_exit_refuses_before_flash(self):
        proc = mock.Mock()
        proc.wait.return_value = 1
        proc.poll.return_value = 1
        with self.assertRaises(FlashError) as caught:
            with macos_runtime.NoSleep(popen=mock.Mock(return_value=proc)):
                self.fail('must not start flash')
        self.assertIn('nothing was sent', str(caught.exception))

    def test_caffeinate_missing_refuses(self):
        with self.assertRaises(FlashError):
            macos_runtime.NoSleep(popen=mock.Mock(side_effect=FileNotFoundError())).__enter__()

    def test_caffeinate_released_on_flash_error(self):
        proc = self.process()
        with self.assertRaisesRegex(RuntimeError, 'flash error'):
            with macos_runtime.NoSleep(popen=mock.Mock(return_value=proc)):
                raise RuntimeError('flash error')
        proc.terminate.assert_called_once()

    def test_bundled_backend_explicit_path_and_loaded_report(self):
        backend = types.SimpleNamespace(lib=types.SimpleNamespace(_name=str(self.vendor / 'lib/libusb-1.0.0.dylib')))
        libusb = types.ModuleType('usb.backend.libusb1')
        libusb.get_backend = mock.Mock(return_value=backend)
        usb = types.ModuleType('usb'); usb.__path__ = []
        be = types.ModuleType('usb.backend'); be.__path__ = []
        usb.backend = be; be.libusb1 = libusb
        with mock.patch.dict(sys.modules, {'usb': usb, 'usb.backend': be, 'usb.backend.libusb1': libusb}), \
                mock.patch.object(platform_probe, 'host', return_value=('macos', 'arm64')), \
                mock.patch.object(bundle, 'vendor_dir', return_value=str(self.vendor)), \
                mock.patch.object(macos_runtime, 'prepare_tools'):
            self.assertIs(platform_probe._bundled_libusb_backend(), backend)
        finder = libusb.get_backend.call_args.kwargs['find_library']
        self.assertEqual(finder('usb-1.0'), str(self.vendor / 'lib/libusb-1.0.0.dylib'))
        self.assertEqual(platform_probe.LIBUSB_LOADED, finder('ignored'))

    def test_frozen_missing_backend_never_falls_back(self):
        libusb = types.ModuleType('usb.backend.libusb1')
        libusb.get_backend = mock.Mock(return_value=None)
        usb = types.ModuleType('usb'); usb.__path__ = []
        be = types.ModuleType('usb.backend'); be.__path__ = []
        usb.backend = be; be.libusb1 = libusb
        with mock.patch.dict(sys.modules, {'usb': usb, 'usb.backend': be, 'usb.backend.libusb1': libusb}), \
                mock.patch.object(platform_probe, 'host', return_value=('macos', 'arm64')), \
                mock.patch.object(bundle, 'vendor_dir', return_value=str(self.vendor / 'missing')), \
                mock.patch.object(macos_runtime, 'prepare_tools'), mock.patch.object(sys, 'frozen', True, create=True):
            with self.assertRaisesRegex(RuntimeError, 'Missing bundled libusb'):
                platform_probe._bundled_libusb_backend()
        libusb.get_backend.assert_not_called()

    def test_safari_folder_rejected_without_changing_zip_policy(self):
        with mock.patch.object(sys, 'platform', 'darwin'):
            with self.assertRaises(firmware_resolve.UnknownFirmware) as caught:
                firmware_resolve.identify_zip(str(self.root))
            self.assertIn('Download Linked File', str(caught.exception))
            with self.assertRaises(imagebuild.BuildError) as caught:
                imagebuild.extract_stock_rootfs(str(self.root), 'unused', str(self.root))
            self.assertIn('Download Linked File', str(caught.exception))
        self.assertEqual(sorted(self.root.iterdir()), [self.root / 'vendor'])


class LinksTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.exe = self.root / 'Contents/MacOS/app'
        self.lib = self.root / 'Contents/Frameworks/libusb.dylib'
        for path in (self.exe, self.lib):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(bytes.fromhex('cffaedfe'))

    def test_otool_fixture_with_spaces(self):
        fixture = '/app with space:\n\t@loader_path/../Frameworks/a.dylib (compatibility version 1.0.0, current version 2.0.0)\n\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1.0.0)\n'
        self.assertEqual(links.dependencies(fixture), ['@loader_path/../Frameworks/a.dylib', '/usr/lib/libSystem.B.dylib'])

    def test_rpath_fixture(self):
        text = 'Load command 1\n          cmd LC_RPATH\n      cmdsize 48\n         path @executable_path/../Frameworks (offset 12)\n'
        self.assertEqual(links.rpaths(text), ['@executable_path/../Frameworks'])
        self.assertEqual(links.resolve('@rpath/libusb.dylib', self.exe, self.exe, self.root, links.rpaths(text)), self.lib)

    def test_system_paths_allowed(self):
        for path in ('/usr/lib/libSystem.B.dylib', '/System/Library/Frameworks/Tk.framework/Tk'):
            self.assertIsNone(links.resolve(path, self.exe, self.exe, self.root))

    def test_external_missing_and_relative_refused(self):
        for path in ('/opt/homebrew/lib/libusb.dylib', '/usr/local/lib/libusb.dylib',
                     '@rpath/missing.dylib', 'libusb.dylib', '@loader_path/../../../../outside'):
            with self.subTest(path=path), self.assertRaises(ValueError):
                links.resolve(path, self.exe, self.exe, self.root)

    def test_loader_path_internal(self):
        self.assertEqual(links.resolve('@loader_path/../Frameworks/libusb.dylib', self.exe, self.exe, self.root), self.lib)

    def test_fixture_tree_rewrites_and_checks_arch(self):
        def run(args, **kwargs):
            if args[0].endswith('lipo'):
                out = 'arm64\n'
            elif args[1] == '-L':
                out = args[-1] + ':\n\t@loader_path/../Frameworks/libusb.dylib (compatibility version 1.0.0, current version 1.0.0)\n' if args[-1] == str(self.exe) else args[-1] + ':\n'
            else:
                out = ''
            return types.SimpleNamespace(stdout=out)
        runner = mock.Mock(side_effect=run)
        self.assertEqual(len(links.check_tree(self.root, self.exe, 'arm64', True, runner)), 2)
        self.assertTrue(any(c.args[0][0].endswith('install_name_tool') for c in runner.call_args_list))
        with self.assertRaisesRegex(ValueError, 'Wrong architecture'):
            links.check_tree(self.root, self.exe, 'x86_64', run=runner)

    def test_bare_loader_path_rpath(self):
        self.assertEqual(links.expand('@loader_path', self.lib, self.exe), self.lib.parent)
        self.assertEqual(links.expand('@executable_path', self.lib, self.exe), self.exe.parent)

    def test_system_path_cannot_escape_with_dotdot(self):
        self.assertFalse(links.system('/usr/lib/../../opt/homebrew/lib/libusb.dylib'))

    def test_external_rpath_refused(self):
        def run(args, **kwargs):
            out = 'arm64' if args[0].endswith('lipo') else ''
            if args[1] == '-l':
                out = 'cmd LC_RPATH\ncmdsize 48\npath /opt/homebrew/lib (offset 12)\n'
            return types.SimpleNamespace(stdout=out)
        with self.assertRaisesRegex(ValueError, 'LC_RPATH'):
            links.check_tree(self.root, self.exe, 'arm64', run=run)

    def test_escaping_bundle_symlink_refused(self):
        (self.root / 'escape').symlink_to(self.root.parent)
        with self.assertRaisesRegex(ValueError, 'symlink'):
            links.check_tree(self.root, self.exe, 'arm64', run=lambda *a, **k: types.SimpleNamespace(stdout=''))


class DoctorTests(unittest.TestCase):
    def doctor(self, tk, backend_error=False):
        from diskos_installer import __main__ as cli
        with ExitStack() as stack:
            for obj, name, value in [(platform_probe, 'host', ('macos', 'arm64')),
                                     (platform_probe, 'distro_name', 'test'),
                                     (platform_probe, 'host_status', 'supported'),
                                     (platform_probe, 'maskrom_count', 0),
                                     (cli.state, 'state_dir', 'test-state'),
                                     (cli.state, 'load', {}),
                                     (cli.state, 'have_stock_image', False),
                                     (bundle, 'native', __file__), (bundle, 'data', __file__),
                                     (cli.imagebuild, 'resolve_ota_config', {}),
                                     (cli.imagebuild, 'describe_ota', 'OFF')]:
                stack.enter_context(mock.patch.object(obj, name, return_value=value))
            stack.enter_context(mock.patch.object(cli.os, 'access', return_value=True))
            stack.enter_context(mock.patch.object(cli.imagebuild, 'check_squashfs_tools'))
            stack.enter_context(mock.patch.dict(sys.modules, {'tkinter': types.SimpleNamespace(TkVersion=tk)}))
            stack.enter_context(mock.patch.object(platform_probe, '_bundled_libusb_backend',
                                                side_effect=RuntimeError('cannot load') if backend_error else None))
            stack.enter_context(mock.patch('sys.stdout', new_callable=io.StringIO))
            stack.enter_context(mock.patch('sys.stderr', new_callable=io.StringIO))
            return cli.cmd_doctor(None)

    def test_doctor_old_tk_fails(self):
        self.assertEqual(self.doctor(8.5), 1)

    def test_doctor_missing_libusb_fails(self):
        self.assertEqual(self.doctor(8.6, backend_error=True), 1)

    def test_doctor_modern_tk_and_loaded_libusb_pass(self):
        self.assertEqual(self.doctor(8.6), 0)


class PythonSelectionTests(unittest.TestCase):
    def select(self, framework, brew, apple):
        script = ROOT / 'build/macos-python.sh'
        # Fake interpreters still receive the real Tk probe expression.
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for name, status in [('framework', framework), ('brew', brew), ('apple', apple)]:
                p = root / name
                p.write_text('#!/bin/sh\n[ "$1" = -c ] || exit 90\ncase "$2" in *TkVersion*8.6*) exit ' + str(status) + ';; *) exit 91;; esac\n')
                p.chmod(0o755)
            code = '. "$1"\ncommand() {\ncase "$2" in\n'
            code += ' /Library/Frameworks/*/Current/*) printf "%s/framework\\n" "$2ROOT";;\n'.replace('$2ROOT', '$FAKE_ROOT')
            code += ' /opt/homebrew/*) printf "%s/brew\\n" "$FAKE_ROOT";;\n python3) printf "%s/apple\\n" "$FAKE_ROOT";;\n *) return 1;;\nesac\n}\nselect_macos_python\n'
            return subprocess.run(['sh', '-c', code, 'check', str(script)],
                                  env=dict(os.environ, FAKE_ROOT=str(root)), capture_output=True, text=True)

    def test_framework_preferred(self):
        result = self.select(0, 0, 1)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.strip().endswith('/framework'))

    def test_brew_preferred_over_apple(self):
        result = self.select(1, 0, 1)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.strip().endswith('/brew'))

    def test_old_tk_all_candidates_print_fix(self):
        result = self.select(1, 1, 1)
        self.assertEqual(result.returncode, 1)
        self.assertIn('https://www.python.org/downloads/macos/', result.stderr)

    def test_real_probe_rejects_85_accepts_86_without_display(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for version, want in [('8.5', 1), ('8.6', 0), ('9.0', 0)]:
                (root / 'tkinter.py').write_text('TkVersion = ' + version + '\n')
                result = subprocess.run(['sh', '-c', '. "$1"; macos_python_ok "$2"',
                                         'check', str(ROOT / 'build/macos-python.sh'), sys.executable],
                                        env=dict(os.environ, PYTHONPATH=temp, PYTHONDONTWRITEBYTECODE='1'),
                                        capture_output=True)
                self.assertEqual(result.returncode, want)


if __name__ == '__main__':
    unittest.main()
