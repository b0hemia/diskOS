# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""The Windows tools recipe refuses delay imports before publishing a bundle."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import unittest

RECIPE = Path(__file__).resolve().parents[1] / 'build' / 'build-windows-tools.sh'
ZERO = 'Entry d 0000000000000000 00000000 Delay Import Directory\n'


def delay_free(text):
    body = re.search(r'^delay_free\(\) \{\n.*?^\}\n', RECIPE.read_text(), re.S | re.M).group(0)
    return subprocess.run(['bash', '-c', body + 'delay_free'], input=text, text=True).returncode


@unittest.skipUnless(shutil.which('bash') and os.name != 'nt', 'needs a POSIX bash')
class RecipeDelayImports(unittest.TestCase):
    def test_delay_import_gate(self):
        self.assertEqual(delay_free('header\n' + ZERO + 'more\n'), 0)
        for text in ('Entry d 000000000000a000 00000000 Delay Import Directory\n',
                     'Entry d 0000000000000000 00000040 Delay Import Directory\n',
                     'no data directory\n', ZERO + ZERO, ''):
            with self.subTest(text=text):
                self.assertNotEqual(delay_free(text), 0)


@unittest.skipUnless(shutil.which('bash') and os.name != 'nt', 'needs a POSIX bash')
class RecipePinsAndImports(unittest.TestCase):
    def run_function(self, name, *args):
        body = re.search(r'^' + name + r'\(\) \{\n.*?^\}\n',
                         RECIPE.read_text(), re.S | re.M).group(0)
        return subprocess.run(['bash', '-c', body + name + ' "$@"', 'test', *args],
                              capture_output=True).returncode

    def test_runtime_pin(self):
        self.assertEqual(self.run_function('check_runtime_version', 'msys2-runtime 3.6.10-6'), 0)
        for package in ('msys2-runtime 3.6.10-5', 'msys2-runtime 3.6.10-7',
                        'msys2-runtime 3.7.0-1', 'gcc-libs 3.6.10-6', ''):
            self.assertNotEqual(self.run_function('check_runtime_version', package), 0)

    def test_closed_import_policy(self):
        from diskos_installer.bundle import _WINDOWS_OS_DLLS, _WINDOWS_USBBOOT_UCRT, _WINDOWS_BIND_OS
        for file in ('mksquashfs.exe', 'unsquashfs.exe', 'usbboot.exe', 'bind_winusb.exe', 'msys-2.0.dll'):
            permitted = {'kernel32.dll'}
            if file in ('mksquashfs.exe', 'unsquashfs.exe'):
                permitted.add('msys-2.0.dll')
            elif file == 'msys-2.0.dll':
                permitted.add('ntdll.dll')
            elif file == 'usbboot.exe':
                permitted.update(_WINDOWS_USBBOOT_UCRT)
            candidates = permitted | _WINDOWS_OS_DLLS | _WINDOWS_USBBOOT_UCRT | _WINDOWS_BIND_OS | {
                'msys-lzo2-2.dll', 'msys-z.dll', 'msys-gcc_s-seh-1.dll',
                'api-ms-win-crt-future-l1-1-0.dll', 'other.dll'}
            for name in candidates:
                with self.subTest(file=file, name=name):
                    self.assertEqual(self.run_function('import_allowed', file, name) == 0,
                                     name in permitted)


if __name__ == '__main__':
    unittest.main()
