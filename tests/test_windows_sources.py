# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Offline checks for the release source pin and output containment."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'build/fetch-windows-sources.sh'
PIN = '1d06b1482199ca8adaea770c4b596ec3adb2749345ef81a5d111cb792f0f1643'


def function(name):
    return re.search(r'^' + name + r'\(\) \{\n.*?^\}\n',
                     SCRIPT.read_text(), re.S | re.M).group(0)


@unittest.skipUnless(shutil.which('bash') and os.name != 'nt', 'needs a POSIX bash')
class WindowsSources(unittest.TestCase):
    def test_pin_and_local_fixture(self):
        text = SCRIPT.read_text()
        self.assertIn('runtime_source=msys2-runtime-3.6.10-6.src.tar.zst', text)
        self.assertIn('runtime_sha256=' + PIN, text)
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'msys2-runtime-3.6.10-6.src.tar.zst'
            (Path(folder) / (path.name + '.sig')).write_bytes(b'offline signature')
            path.write_bytes(b'offline fixture, not runtime source')
            code = 'runtime_sha256=' + PIN + '\n' + function('verify_runtime_source')
            # Real fixture digest refuses a substituted archive without network.
            run = subprocess.run(['bash', '-c', code + 'verify_runtime_source "$1"',
                                  'test', str(path)], capture_output=True)
            self.assertNotEqual(run.returncode, 0)
            # Simulate the trusted checksum tool returning the pinned digest.
            mock = 'sha256sum() { printf "%s  %s\\n" "$runtime_sha256" "$2"; }\n'
            run = subprocess.run(['bash', '-c', code + mock + 'verify_runtime_source "$1"',
                                  'test', str(path)], capture_output=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            run = subprocess.run(['bash', str(SCRIPT), '--verify', folder], capture_output=True)
            self.assertNotEqual(run.returncode, 0)

    def test_output_refusal(self):
        root = SCRIPT.parents[1]
        code = function('check_output_dir') + 'root=$1; check_output_dir "$2"'
        with tempfile.TemporaryDirectory() as folder:
            alias = Path(folder) / 'checkout'
            alias.symlink_to(root, target_is_directory=True)
            for path in (root, root / 'vendor/assets', alias / 'vendor/assets'):
                run = subprocess.run(['bash', '-c', code, 'test', str(root), str(path)],
                                     capture_output=True)
                self.assertNotEqual(run.returncode, 0)
            run = subprocess.run(['bash', '-c', code, 'test', str(root), folder],
                                 capture_output=True)
            self.assertEqual(run.returncode, 0, run.stderr)


if __name__ == '__main__':
    unittest.main()
