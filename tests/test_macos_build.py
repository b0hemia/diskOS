# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline freeze validation and release archive contracts."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
# Build helpers use sibling imports, just as the isolated command entry does.
sys.path.insert(0, str(ROOT / 'build'))
import macos_inputs
import macos_release


class BuildTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.repo = self.root / 'repo'
        self.repo.mkdir()
        self.prefix = self.root / 'Python.framework/Versions/3.12'
        self.prefix.mkdir(parents=True)
        for name in ('LICENSE', 'NOTICE.md', 'README.md', 'SPL_SOURCE.md', 'requirements.txt',
                     'docs/INSTALL.md', 'docs/MACOS_RECOVERY.txt', 'ui/source.c', 'spl-src/source.c',
                     'src/usbboot/usbboot.c', 'build/build-helper.sh',
                     'flash/assemble_stage1.py', 'flash/ginfo_w63ah6nkb.bin',
                     'flash/my_write6.c', 'flash/build_nand.sh'):
            path = self.repo / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('fixture\n')
        (self.prefix / 'Python').write_text('runtime')
        self.lock = self.root / 'inputs.json'

    def write_lock(self):
        records = {}
        for name, root in [('repo', self.repo), ('python', self.prefix)]:
            records[name] = {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                             for p in root.rglob('*') if p.is_file()}
        self.lock.write_text(json.dumps(records))
        return hashlib.sha256(self.lock.read_bytes()).hexdigest()

    def verify(self, digest):
        with mock.patch.object(macos_release, 'check_notices'), \
                mock.patch.object(macos_inputs.platform, 'python_version', return_value='3.12.10'), \
                mock.patch.object(macos_inputs.importlib.metadata, 'version', side_effect=macos_inputs.VERSIONS.get), \
                mock.patch.object(sys, 'prefix', str(self.prefix)), \
                mock.patch.object(sys, 'base_prefix', str(self.prefix)), \
                mock.patch.dict(sys.modules, {'tkinter': types.SimpleNamespace(TkVersion=8.6)}):
            return macos_inputs.verify(self.repo, self.lock, digest)

    def test_complete_lock_passes(self):
        self.assertIn('python', self.verify(self.write_lock()))

    def test_changed_input_fails(self):
        digest = self.write_lock()
        (self.prefix / 'Python').write_text('changed')
        with self.assertRaisesRegex(ValueError, 'hash mismatch'):
            self.verify(digest)

    def test_unrecorded_gpl_source_fails(self):
        digest = self.write_lock()
        (self.repo / 'src/new.c').write_text('new')
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            self.verify(digest)

    def test_wrong_lock_digest_fails(self):
        self.write_lock()
        with self.assertRaisesRegex(ValueError, 'lock SHA256'):
            self.verify('0' * 64)

    def test_missing_notices_fails(self):
        with self.assertRaisesRegex(ValueError, 'Missing selected-runtime notice'):
            macos_release.check_notices(self.repo)

    def test_isolated_entry_imports_sibling(self):
        result = subprocess.run([sys.executable, '-B', '-I', str(ROOT / 'build/macos_inputs.py'),
                                 str(self.repo), str(self.lock), '0' * 64],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Missing selected-runtime notice', result.stderr)
        self.assertNotIn('ModuleNotFoundError', result.stderr)

    def test_release_preserves_modes_symlinks_and_sources(self):
        app = self.root / 'diskOS Installer.app'
        exe = app / 'Contents/MacOS/diskos-installer'
        exe.parent.mkdir(parents=True)
        exe.write_bytes(bytes.fromhex('cffaedfe') + b'fixture')
        exe.chmod(0o755)
        (app / 'internal').symlink_to('Contents/MacOS/diskos-installer')
        for folder in ('licenses', 'corresponding-source'):
            (self.repo / folder).mkdir()
            (self.repo / folder / 'source.txt').write_text('fixture')
        for name in ('vendor/setup-macos.sh', 'build/NATIVE_TOOLS.md',
                     'build/MACOS_FROZEN_DEPENDENCIES.md', 'build/build-macos.sh',
                     'build/diskos-installer.spec', 'build/macos_inputs.py',
                     'build/macos_links.py', 'build/macos_release.py'):
            path = self.repo / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('fixture')
        self.write_lock()
        run = self.root / 'run'
        run.mkdir()
        with mock.patch.object(Path, 'cwd', return_value=self.repo), \
                mock.patch.object(macos_release, 'check_notices'):
            macos_release.release(app, run, self.lock)
        archive = next(run.glob('*.tar.gz'))
        with tarfile.open(archive) as tar:
            names = tar.getnames()
            top = names[0]
            self.assertEqual(tar.getmember(top + '/diskOS Installer.app/Contents/MacOS/diskos-installer').mode, 0o755)
            self.assertTrue(tar.getmember(top + '/diskOS Installer.app/internal').issym())
            for source in ('ui/source.c', 'spl-src/source.c', 'src/usbboot/usbboot.c', 'build/build-helper.sh',
                           'flash/assemble_stage1.py', 'flash/ginfo_w63ah6nkb.bin',
                           'flash/my_write6.c', 'flash/build_nand.sh', 'docs/MACOS_RECOVERY.txt'):
                self.assertIn(top + '/' + source, names)
            manifest = tar.extractfile(top + '/MANIFEST.txt').read().decode()
            self.assertIn(hashlib.sha256(exe.read_bytes()).hexdigest(), manifest)
            self.assertIn('LINK  diskOS Installer.app/internal -> Contents/MacOS/diskos-installer', manifest)
        self.assertEqual((run / (archive.name + '.sha256')).read_text().split()[0],
                         hashlib.sha256(archive.read_bytes()).hexdigest())


if __name__ == '__main__':
    unittest.main()
