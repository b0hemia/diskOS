# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Offline checkout and raw-blob regression tests; no build or device."""
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'build'))
from verify_git_blobs import BlobGuard, DEVICE, PAYLOAD


class GitBlobTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root = self.base / 'source'
        self.root.mkdir()
        self.env = dict(os.environ, GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=os.devnull,
                        GIT_AUTHOR_NAME='b0hemia', GIT_COMMITTER_NAME='b0hemia',
                        GIT_AUTHOR_EMAIL='50309975+b0hemia@users.noreply.github.com',
                        GIT_COMMITTER_EMAIL='50309975+b0hemia@users.noreply.github.com')
        self.git('init', '-q')
        self.git('config', 'core.autocrlf', 'false')
        self.files = {'payload/' + n: b'#!/bin/sh\nexit 0\n' for n in PAYLOAD}
        self.files.update({'flash/' + n: b'\x00\xff\r\n\x80' for n in DEVICE})
        self.files.update({
            '.gitattributes': (ROOT / '.gitattributes').read_bytes(),
            'install.sh': b'#!/bin/sh\nexit 0\n',
            'flash/build_nand.sh': b'#!/bin/sh\nexit 0\n',
            'payload/mq_ui': b'\x7fELF\x00\xff\r\n\x80',
            'build/example.ps1': b'Write-Output "ok"\n',
            'build/example.bat': b'@echo off\r\necho ok\r\n',
            'build/example.cmd': b'@echo off\r\necho ok\r\n',
            'licenses/upstream.txt': b'Original notice\r\n',
            'README.md': b'Read me\n',
        })
        for name, raw in self.files.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(raw)
        self.git('add', '--', *self.files)
        self.git('commit', '-qm', 'fixture')
        self.commit = self.git('rev-parse', 'HEAD').decode().strip()
        self.data = self.base / 'frozen'
        for name in PAYLOAD:
            target = self.data / 'payload' / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(self.files['payload/' + name])
        for name in DEVICE:
            target = self.data / 'vendor/linux-x86_64' / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(self.files['flash/' + name])

    def git(self, *args):
        return subprocess.run(['git', '-C', str(self.root), *args], env=self.env,
                              check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout

    def test_attributes_and_autocrlf_clone_preserve_every_byte(self):
        paths = list(self.files)
        attrs = self.git('-c', 'core.autocrlf=true', 'check-attr', '-z', 'text', '--', *paths)
        fields = attrs.rstrip(b'\0').split(b'\0')
        self.assertEqual(len(fields), len(paths) * 3)
        for index, name in enumerate(paths):
            self.assertEqual(fields[3 * index:3 * index + 3],
                             [name.encode(), b'text', b'unset'])
        clone = self.base / 'clone'
        self.git('-c', 'core.autocrlf=true', 'clone', '-q', str(self.root), str(clone))
        for name, expected in self.files.items():
            self.assertEqual((clone / name).read_bytes(), expected, name)

        raw = self.git('-c', 'core.autocrlf=true', 'archive', '--format=tar', 'HEAD')
        with tarfile.open(fileobj=io.BytesIO(raw)) as archive:
            for name, expected in self.files.items():
                self.assertEqual(archive.extractfile(name).read(), expected, name)

    def test_frozen_and_source_guard(self):
        guard = BlobGuard(self.root)
        guard.devices(self.data, 'linux-x86_64', frozen=True)
        shutil.copytree(self.data / 'vendor', self.root / 'vendor')
        guard.devices(self.root, 'linux-x86_64')
        self.assertEqual(guard.commit, self.commit)
        (self.root / 'flash/build_nand.sh').write_bytes(b'#!/bin/sh\r\nexit 0\r\n')
        with self.assertRaisesRegex(ValueError, 'Committed blob mismatch'):
            guard.devices(self.root, 'linux-x86_64')

    def test_identically_corrupted_checkout_and_frozen_reference_fail(self):
        name = 'payload/S97diskos_install'
        bad = self.files[name].replace(b'\n', b'\r\n')
        (self.root / name).write_bytes(bad)
        (self.data / name).write_bytes(bad)
        with self.assertRaisesRegex(ValueError, 'Committed blob mismatch'):
            BlobGuard(self.root).devices(self.data, 'linux-x86_64', frozen=True)

    def test_binary_corruption_missing_file_and_uncommitted_path_fail(self):
        path = self.data / 'vendor/linux-x86_64' / DEVICE[0]
        good = path.read_bytes()
        path.write_bytes(good[:-1] + b'X')
        with self.assertRaisesRegex(ValueError, 'Committed blob mismatch'):
            BlobGuard(self.root).devices(self.data, 'linux-x86_64', frozen=True)
        path.write_bytes(good)
        path.unlink()
        with self.assertRaises(OSError):
            BlobGuard(self.root).devices(self.data, 'linux-x86_64', frozen=True)
        target = self.data / 'payload/mq_ui'
        with self.assertRaises(subprocess.CalledProcessError):
            BlobGuard(self.root).verify([(target, 'payload/not-committed')])

    def test_non_repository_and_symlink_fail(self):
        with self.assertRaises(subprocess.CalledProcessError):
            BlobGuard(self.base)
        target = self.data / 'payload/mq_ui'
        target.unlink()
        target.symlink_to(self.root / 'payload/mq_ui')
        with self.assertRaisesRegex(ValueError, 'Links/reparse points'):
            BlobGuard(self.root).devices(self.data, 'linux-x86_64', frozen=True)

    def test_cli_fails_closed(self):
        cmd = [sys.executable, '-I', str(ROOT / 'build/verify_git_blobs.py'),
               str(self.root), str(self.data), 'linux-x86_64', '--ref', self.commit, '--frozen']
        good = subprocess.run(cmd, capture_output=True)
        self.assertEqual(good.returncode, 0, good.stderr)
        self.assertEqual(good.stdout.strip(), self.commit.encode())
        (self.data / 'payload/diskos-selected').write_bytes(b'changed')
        bad = subprocess.run(cmd, capture_output=True)
        self.assertNotEqual(bad.returncode, 0)
        self.assertIn(b'Committed blob mismatch', bad.stderr)
