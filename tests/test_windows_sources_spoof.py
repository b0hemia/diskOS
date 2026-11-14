# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Exercise the complete offline verifier with caller-supplied GnuPG spoofs."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'build/fetch-windows-sources.sh'
SOURCE = 'msys2-runtime-3.6.10-6.src.tar.zst'
SPOOF = '''printf used > "$SPOOF_MARKER"
printf '[GNUPG:] VALIDSIG fingerprint\\n[GNUPG:] TRUST_FULLY 0 pgp\\n'
'''


@unittest.skipUnless(os.name != 'nt' and shutil.which('bash')
                     and Path('/usr/bin/gpg').is_file(), 'needs POSIX bash and system GnuPG')
class FullVerifierSpoofTests(unittest.TestCase):
    def check_spoof(self, attack):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            (path / SOURCE).write_bytes(b'offline archive fixture')
            (path / (SOURCE + '.sig')).write_bytes(b'invalid detached signature')
            keyring = path / 'keyring'
            keyring.mkdir(mode=0o700)
            # Prove the main script reaches authentication, independently of the
            # archive digest. Only the checksum tool is substituted by the test.
            pin = re.search(r'^runtime_sha256=(\w+)', SCRIPT.read_text(), re.M).group(1)
            checksum = path / 'sha256sum'
            checksum.write_text('#!/bin/sh\nprintf checked > "$CHECKSUM_MARKER"\n'
                                'printf "%s  %s\\n" "' + pin + '" "$2"\n')
            checksum.chmod(0o755)
            spoof_marker = path / 'spoof-used'
            checksum_marker = path / 'checksum-used'
            env = dict(os.environ, PATH=folder + os.pathsep + os.environ['PATH'],
                       MSYS2_GNUPGHOME=str(keyring), SPOOF_MARKER=str(spoof_marker),
                       CHECKSUM_MARKER=str(checksum_marker))
            if attack == 'function':
                env['BASH_FUNC_gpg%%'] = '() { ' + SPOOF + '}'
            elif attack == 'path':
                shim = path / 'gpg'
                shim.write_text('#!/bin/sh\n' + SPOOF)
                shim.chmod(0o755)
            else:
                foreign = path / 'foreign-keyring'
                foreign.mkdir()
                env['GNUPGHOME'] = str(foreign)
                # Neither the caller's default home nor the explicit keyring's
                # gpg.conf may alter verification options.
                for home in (foreign, keyring):
                    (home / 'gpg.conf').write_text('diskos-invalid-config-option\n')
            if attack in ('function', 'path'):
                control = subprocess.run(['bash', '-c', 'gpg'], env=env, capture_output=True)
                self.assertEqual(control.returncode, 0, control.stderr)
                self.assertIn(b'VALIDSIG', control.stdout)
                self.assertEqual(spoof_marker.read_text(), 'used')
                spoof_marker.write_text('')
            run = subprocess.run(['bash', str(SCRIPT), '--verify', folder],
                                 env=env, capture_output=True)
            self.assertEqual(checksum_marker.read_text(), 'checked')
            self.assertNotEqual(run.returncode, 0, run.stdout)
            self.assertNotIn(b'PASS:', run.stdout)
            self.assertIn(b'gpg:', run.stderr)
            if attack in ('function', 'path'):
                self.assertEqual(spoof_marker.read_text(), '')
            else:
                self.assertNotIn(b'diskos-invalid-config-option', run.stderr)
                self.assertNotIn(b'gpg.conf', run.stderr)

    def test_exported_gpg_function_is_ignored(self):
        self.check_spoof('function')

    def test_gpg_path_shim_is_ignored(self):
        self.check_spoof('path')

    def test_gpg_environment_and_config_are_ignored(self):
        self.check_spoof('config')


if __name__ == '__main__':
    unittest.main()
