# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Exercise staging offline with a fixed archive export and fake git reader."""
import hashlib
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tarfile
import tempfile
import unittest
import zipfile
from diskos_installer.errors import DEVICE_RESULT_CODES
ROOT = Path(__file__).resolve().parents[1]
SCRIPT = (ROOT / 'build/stage-release.sh').read_text()
SOURCE_COMMIT = 'a' * 40
NATIVE = 'usbboot.exe mksquashfs.exe unsquashfs.exe bind_winusb.exe msys-2.0.dll'.split()
INPUTS = ('src/usbboot/usbboot.c build/build-windows-tools.sh build/build-windows-driver.sh '
          'src/windows-driver/bind_winusb.c src/windows-driver/bind_winusb.manifest '
          'src/windows-driver/bind_winusb.rc corresponding-source/msys2-squashfs.patch '
          'corresponding-source/squashfs-tools_4.6.1.orig.tar.gz '
          'corresponding-source/lzo2_2.10.orig.tar.gz corresponding-source/libusb-1.0_1.0.27.orig.tar.bz2').split()

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

class StageRelease(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'repo'
        self.export = Path(self.temp.name) / 'export'
        self.root.mkdir(); self.export.mkdir()
        self.env = dict(os.environ, FAKE_ROOT=str(self.root), DISKOS_REF='qualified',
                        FAKE_COMMIT=SOURCE_COMMIT, FAKE_GIT_LOG=str(Path(self.temp.name) / 'git.log'))
        names = INPUTS + ['RECOVERY.txt', 'vendor/setup-macos.sh', 'diskos_installer/service.py']
        for variable in ('REQ_ARCHIVE', 'REQ_PAYLOAD'):
            items = shlex.split(re.search(variable + r'=\((.*?)\)', SCRIPT, re.S)[1])
            names += [('payload/' + n if variable == 'REQ_PAYLOAD' else n) for n in items]
        for name in names:
            path = self.export / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(('archived ' + name).encode())
        shutil.copyfile(ROOT / 'payload/diskos-root.pub.pem', self.export / 'payload/diskos-root.pub.pem')
        shutil.copytree(self.export, self.root, dirs_exist_ok=True)
        (self.root / 'build/stage-release.sh').write_text(SCRIPT)
        for name in ('verify_git_blobs.py', 'deterministic_zip.py'):
            shutil.copyfile(ROOT / 'build' / name, self.root / 'build' / name)
        self.env['FAKE_EXPORT'] = str(self.export)
        self.vendor = self.root / 'vendor/windows-x86_64'
        self.vendor.mkdir()
        for name in NATIVE + ['my_write6_dram.bin', 'disc_spl_lpddr3.bin']:
            (self.vendor / name).write_bytes(('native ' + name).encode())
        for name in ('my_write6_dram.bin', 'disc_spl_lpddr3.bin'):
            shutil.copyfile(self.export / 'flash' / name, self.vendor / name)
        text = 'Input SHA256 (paths relative to installer):\n'
        text += ''.join(sha(self.export / n) + '  ' + n + '\n' for n in INPUTS)
        text += '\nOutput SHA256:\n' + ''.join(sha(self.vendor / n) + '  ' + n + '\n' for n in NATIVE)
        (self.vendor / 'MANIFEST.txt').write_text(text)
        self.zip = Path(self.temp.name) / 'qualified.zip'
        with zipfile.ZipFile(self.zip, 'w') as package:
            prefix = 'diskos-installer-windows-x86_64/'
            for path in list(self.export.rglob('*')) + list(self.vendor.iterdir()):
                if path.is_file():
                    name = str(path.relative_to(self.export if path.is_relative_to(self.export) else self.root))
                    package.write(path, prefix + ('' if name == 'RECOVERY.txt' else '_internal/') + name)
        self.archive = Path(self.temp.name) / 'export.tar'
        self.export_archive()
        commands = Path(self.temp.name) / 'commands'
        commands.mkdir()
        for name, body in {'git': 'printf "%s\\n" "$*" >> "$FAKE_GIT_LOG"\n'
                           'case "$*" in *--show-toplevel*) echo "$FAKE_ROOT";; '
                           '*--verify*) echo "$FAKE_COMMIT";; *archive*) cat "' + str(self.archive) + '";; '
                           '*ls-tree*) python3 -c \'import os,pathlib,sys; r=pathlib.Path(os.environ["FAKE_EXPORT"]); '
                           'sys.stdout.buffer.write(b"".join(b"100644 blob "+b"0"*40+b"\\t"+p.relative_to(r).as_posix().encode()+b"\\0" '
                           'for d in ("payload","flash") for p in sorted((r/d).rglob("*")) if p.is_file()))\';; '
                           '*cat-file*) for arg; do object=$arg; done; cat "$FAKE_EXPORT/${object#*:}";; esac',
                           'file': 'echo "ELF statically linked"'}.items():
            path = commands / name
            path.write_text('#!/bin/sh\n' + body + '\n'); path.chmod(0o755)
        self.env['PATH'] = str(commands) + os.pathsep + os.environ['PATH']
        self.rel = self.root / 'build/release'
    def export_archive(self):
        with tarfile.open(self.archive, 'w') as tar:
            for path in self.export.iterdir():
                tar.add(path, arcname=path.name)
    def run_stage(self, *args):
        return subprocess.run(['bash', 'build/stage-release.sh', *args], cwd=self.root,
                              env=self.env, capture_output=True, text=True)
    def windows(self, *args):
        return self.run_stage('--windows-zip', str(self.zip), '--windows-sha256', sha(self.zip),
                              '--windows-source-commit', SOURCE_COMMIT,
                              *args, 'windows-x86_64')
    def test_windows_preserves_zip_and_archive_allowlist(self):
        (self.vendor / 'scratch.exe').write_bytes(b'unlisted')
        (self.root / 'src/windows-driver/untracked.c').write_bytes(b'untracked')
        result = self.windows()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.rel / 'diskos-installer-windows-x86_64.zip').read_bytes(), self.zip.read_bytes())
        with tarfile.open(self.rel / 'diskos-installer-windows-x86_64.tar.gz') as tar:
            names = tar.getnames()
            self.assertIn('diskos-installer/src/windows-driver/bind_winusb.manifest', names)
            self.assertFalse(any(n.endswith(('scratch.exe', 'untracked.c')) for n in names))
        self.assertEqual(len((self.rel / 'SHA256SUMS').read_text().splitlines()), 3)
        self.assertIn('archive --format=tar ' + SOURCE_COMMIT,
                      Path(self.env['FAKE_GIT_LOG']).read_text())
    def test_device_copy_must_match_committed_flash_blob(self):
        for name in ('my_write6_dram.bin', 'disc_spl_lpddr3.bin'):
            with self.subTest(name=name):
                path = self.vendor / name
                original = path.read_bytes()
                path.write_bytes(b'wrong device input')
                result = self.windows()
                path.write_bytes(original)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('Committed blob mismatch', result.stderr)
                self.assertIn(' != flash/' + name, result.stderr)
                self.assertEqual(list(self.rel.iterdir()), [])

    def test_matching_zip_and_vendor_corruption_still_refused(self):
        name = 'my_write6_dram.bin'
        bad = b'wrong device input'
        (self.vendor / name).write_bytes(bad)
        with zipfile.ZipFile(self.zip) as package:
            entries = {entry.filename: package.read(entry) for entry in package.infolist()}
        entries['diskos-installer-windows-x86_64/_internal/vendor/windows-x86_64/' + name] = bad
        with zipfile.ZipFile(self.zip, 'w') as package:
            for relative, raw in entries.items():
                package.writestr(relative, raw)
        result = self.windows()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Committed blob mismatch', result.stderr)
        self.assertEqual(list(self.rel.iterdir()), [])

    def test_later_platform_device_corruption_produces_no_artifacts(self):
        self.linux_fixture()
        (self.vendor / 'disc_spl_lpddr3.bin').write_bytes(b'wrong device input')
        result = self.run_stage(
            '--windows-zip', str(self.zip), '--windows-sha256', sha(self.zip),
            '--windows-source-commit', SOURCE_COMMIT, 'linux-x86_64', 'windows-x86_64')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Committed blob mismatch', result.stderr)
        self.assertEqual(list(self.rel.iterdir()), [])

    def test_complete_checksums_and_previous_artifact_omission(self):
        self.rel.mkdir(parents=True)
        extra = self.rel / 'linux-source.tar.gz'; extra.write_bytes(b'previous platform')
        self.assertEqual(self.windows('--artifact', extra.name).returncode, 0)
        previous = (self.rel / 'SHA256SUMS').read_bytes()
        self.assertNotEqual(self.windows().returncode, 0)
        self.assertEqual((self.rel / 'SHA256SUMS').read_bytes(), previous)
        self.assertEqual(self.windows('--artifact', extra.name).returncode, 0)
        check = subprocess.run(['sha256sum', '--check', '--strict', 'SHA256SUMS'], cwd=self.rel, capture_output=True)
        self.assertEqual(check.returncode, 0, check.stderr)
    def test_windows_requires_qualification_source_commit(self):
        result = self.run_stage('--windows-zip', str(self.zip), '--windows-sha256', sha(self.zip),
                                'windows-x86_64')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('source commit is required', result.stderr)
    def test_different_installer_only_revision_is_rejected_before_archive(self):
        (self.export / 'diskos_installer/service.py').write_bytes(b'changed installer only')
        self.export_archive()
        self.env['FAKE_COMMIT'] = 'b' * 40
        result = self.windows()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('archive revision differs from qualified Windows source commit', result.stderr)
        self.assertNotIn('archive --format=tar', Path(self.env['FAKE_GIT_LOG']).read_text())
        self.assertEqual(list(self.rel.iterdir()), [])
    def test_qualification_commit_must_be_full_and_windows_only(self):
        for commit in ('', 'a' * 12, 'g' * 40):
            with self.subTest(commit=commit):
                result = self.run_stage('--windows-zip', str(self.zip), '--windows-sha256', sha(self.zip),
                                        '--windows-source-commit', commit, 'windows-x86_64')
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('source commit is required', result.stderr)
        result = self.run_stage('--windows-source-commit', SOURCE_COMMIT, 'linux-x86_64')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Windows ZIP options require windows-x86_64', result.stderr)
    def test_windows_requires_qualification_zip_hash(self):
        result = self.run_stage('--windows-zip', str(self.zip), '--windows-source-commit', SOURCE_COMMIT,
                                'windows-x86_64')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('qualified Windows SHA256 is required', result.stderr)
    def test_unlisted_release_file_is_rejected_on_first_run_and_with_manifest(self):
        attachments = []
        for existing_manifest in (False, True):
            with self.subTest(existing_manifest=existing_manifest):
                if existing_manifest:
                    self.assertEqual(self.windows(*attachments).returncode, 0)
                else:
                    self.rel.mkdir(parents=True)
                extra = self.rel / ('unlisted-' + str(existing_manifest) + '.tar.gz')
                extra.write_bytes(b'unlisted attachment')
                previous = {p.name: p.read_bytes() for p in self.rel.iterdir()}
                result = self.windows(*attachments)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('Unexpected release entries', result.stderr)
                self.assertEqual({p.name: p.read_bytes() for p in self.rel.iterdir()}, previous)
                attachments += ['--artifact', extra.name]
                self.assertEqual(self.windows(*attachments).returncode, 0)
    def test_listed_missing_artifact_is_rejected_with_and_without_manifest(self):
        for existing_manifest in (False, True):
            with self.subTest(existing_manifest=existing_manifest):
                if existing_manifest:
                    self.assertEqual(self.windows().returncode, 0)
                result = self.windows('--artifact', 'missing-source.tar.gz')
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('missing regular artifact', result.stderr)
    def test_final_inventory_rejects_extra_and_missing_entries_with_and_without_manifest(self):
        self.rel.mkdir(parents=True)
        present = self.rel / 'present.tar.gz'
        present.write_bytes(b'attachment')
        code = SCRIPT.split("<<'PYSUM'\n", 1)[1].split('\nPYSUM', 1)[0]
        for existing_manifest in (False, True):
            if existing_manifest:
                (self.rel / 'SHA256SUMS').write_text(sha(present) + '  ' + present.name + '\n')
            for names, reason in (([present.name, 'missing.tar.gz'], 'Missing release artifacts'),
                                  (['missing.tar.gz'], 'Unexpected release entries')):
                with self.subTest(existing_manifest=existing_manifest, names=names):
                    result = subprocess.run(['python3', '-B', '-', str(self.rel), 'final', *names],
                                            input=code, capture_output=True, text=True)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(reason, result.stderr)
    def test_refuses_tampered_native_manifest_and_qualification_hash(self):
        helper = self.vendor / 'bind_winusb.exe'
        helper.write_bytes(b'stale')
        self.assertNotEqual(self.windows().returncode, 0)
        helper.write_bytes(b'native bind_winusb.exe')
        manifest = self.vendor / 'MANIFEST.txt'
        manifest.write_text(manifest.read_text().replace('src/windows-driver/bind_winusb.rc', '../escape'))
        self.assertNotEqual(self.windows().returncode, 0)
        result = self.run_stage('--windows-zip', str(self.zip), '--windows-sha256', '0' * 64,
                                '--windows-source-commit', SOURCE_COMMIT, 'windows-x86_64')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('hash mismatch', result.stderr)
    def linux_fixture(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location('linux_fixture', ROOT / 'tests/test_usbboot_manifest.py')
        fixtures = importlib.util.module_from_spec(spec); spec.loader.exec_module(fixtures)
        vendor = self.root / 'vendor/linux-x86_64'; vendor.mkdir()
        for name in 'usbboot mksquashfs unsquashfs my_write6_dram.bin disc_spl_lpddr3.bin'.split():
            (vendor / name).write_bytes(b'static fixture')
        for name in ('my_write6_dram.bin', 'disc_spl_lpddr3.bin'):
            shutil.copyfile(self.export / 'flash' / name, vendor / name)
        record = fixtures.fixture(self.export, vendor)
        shutil.copyfile(ROOT / 'build/usbboot_manifest.py', self.root / 'build/usbboot_manifest.py')
        self.export_archive()
        return vendor, record, fixtures
    def test_linux_still_stages_unsuffixed_tools(self):
        self.linux_fixture()
        result = self.run_stage('linux-x86_64')
        self.assertEqual(result.returncode, 0, result.stderr)
        with tarfile.open(self.rel / 'diskos-installer-linux-x86_64.tar.gz') as tar:
            self.assertIn('diskos-installer/vendor/linux-x86_64/usbboot.manifest', tar.getnames())
        # Source in the working tree still agrees with the manifest; only the
        # archived release input changes. Staging must check the archive.
        (self.export / 'src/usbboot/usbboot.c').write_bytes(b'different archived source')
        self.export_archive()
        result = self.run_stage('linux-x86_64')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Archived input hash mismatch', result.stderr)
    def test_linux_manifest_rejections(self):
        import copy
        import json
        vendor, good, fixtures = self.linux_fixture()
        manifest_path = vendor / 'usbboot.manifest'
        # No vendor manifest: reject before creating a release tarball.
        manifest_path.rename(vendor / 'saved-manifest')
        result = self.run_stage('linux-x86_64')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('usbboot.manifest', result.stderr)
        cases = ['malformed', 'missing-input', 'source', 'recipe', 'archive', 'output', 'glibc', 'interp', 'needed']
        for case in cases:
            with self.subTest(case=case):
                record = copy.deepcopy(good)
                (vendor / 'usbboot').write_bytes(fixtures.elf())
                if case == 'missing-input':
                    record['inputs'].pop('src/usbboot/usbboot.c')
                elif case in ('source', 'recipe', 'archive'):
                    name = {'source': 'src/usbboot/usbboot.c', 'recipe': 'build/build-usbboot-pinned.sh',
                            'archive': fixtures.manifest.LIBUSB}[case]
                    record['inputs'][name] = '0' * 64
                elif case == 'output':
                    (vendor / 'usbboot').write_bytes(b'stale binary')
                elif case == 'glibc':
                    record['glibc_packages']['libc-dev-bin'] = '2.39-0ubuntu8.9'
                elif case in ('interp', 'needed'):
                    (vendor / 'usbboot').write_bytes(fixtures.elf(3 if case == 'interp' else 2))
                    record['output']['sha256'] = sha(vendor / 'usbboot')
                manifest_path.write_text('{' if case == 'malformed' else json.dumps(record))
                result = self.run_stage('linux-x86_64')
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn('usbboot manifest', result.stderr)
                self.assertFalse((self.rel / 'diskos-installer-linux-x86_64.tar.gz').exists())

class RecoveryInstructions(unittest.TestCase):
    def test_base_gate_codes_have_distinct_actions(self):
        text = (ROOT / 'RECOVERY.txt').read_text(encoding='ascii')
        bullets = text.split('\n- ')
        gate_codes = {code for code, _ in DEVICE_RESULT_CODES.values() if code.startswith('F2')}
        documented = set(re.findall(r'\bF2\d\d\b', text))
        self.assertEqual(documented, gate_codes)
        for code in gate_codes:
            with self.subTest(code=code):
                bullet = next(b for b in bullets if code in b)
                if code == 'F202':
                    self.assertIn('full', bullet)
                    self.assertIn('boot once', bullet)
                elif code in ('F201', 'F203'):
                    self.assertIn('rebuild', bullet)
                    self.assertIn('fresh mask-ROM', bullet)
                elif code == 'F204':
                    self.assertIn('fully off', bullet)
                    self.assertIn('fresh mask-ROM', bullet)
                else:
                    self.assertIn('get help', bullet)
                    self.assertIn('Never bypass', bullet)
        self.assertNotIn('E320/F2xx', text)
