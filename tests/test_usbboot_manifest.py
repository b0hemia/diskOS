# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Offline provenance fixtures; no compiler, container, network or USB device."""
import copy
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('usbboot_manifest', ROOT / 'build/usbboot_manifest.py')
manifest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(manifest)


def elf(segment=1):
    ident = b'\x7fELF\x02\x01\x01' + bytes(9)
    header = struct.pack('<HHIQQQIHHHHHH', 2, 62, 1, 0x400000, 64, 0, 0, 64, 56, 1, 0, 0, 0)
    program = struct.pack('<IIQQQQQQ', segment, 5, 0, 0x400000, 0, 120, 120, 4096)
    return ident + header + program


def fixture(root, vendor):
    for name in manifest.INPUTS:
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.exists():
            path.write_bytes(('archived ' + name).encode())
    shutil.copyfile(ROOT / manifest.LIBUSB, root / manifest.LIBUSB)
    shutil.copyfile(ROOT / 'build/usbboot_manifest.py', root / 'build/usbboot_manifest.py')
    vendor.mkdir(parents=True, exist_ok=True)
    (vendor / 'usbboot').write_bytes(elf())
    record = dict(schema=1, target='linux-x86_64',
                  inputs={n: manifest.sha(root / n) for n in manifest.INPUTS},
                  image_id='sha256:' + 'a' * 64, source_date_epoch=1700000000,
                  glibc_version=manifest.GLIBC, glibc_packages=dict.fromkeys(manifest.PACKAGES, manifest.GLIBC),
                  compiler=dict(command='gcc-13', version='13.3.0'), flags=copy.deepcopy(manifest.FLAGS),
                  output=dict(file='usbboot', sha256=manifest.sha(vendor / 'usbboot')))
    (vendor / 'usbboot.manifest').write_text(json.dumps(record), encoding='ascii')
    return record


class UsbbootManifest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'archive'
        self.vendor = self.root / 'vendor/linux-x86_64'
        self.record = fixture(self.root, self.vendor)

    def check(self):
        manifest.validate(self.root, self.vendor, 'linux-x86_64')

    def write(self):
        (self.vendor / 'usbboot.manifest').write_text(json.dumps(self.record))

    def test_good_case(self):
        self.check()

    def test_missing_manifest(self):
        self.vendor.rename(self.root / 'saved-vendor')
        self.vendor.mkdir()
        with self.assertRaisesRegex(ValueError, 'Missing/nonregular'):
            self.check()

    def test_malformed_json_and_duplicate_fields(self):
        for text in ('{', '[]', '{"schema":1,"schema":1}'):
            with self.subTest(text=text):
                (self.vendor / 'usbboot.manifest').write_text(text)
                with self.assertRaises(ValueError):
                    self.check()

    def test_malformed_fields(self):
        good = copy.deepcopy(self.record)
        for field, value in (('schema', True), ('target', 'linux-arm64'), ('image_id', 'latest'),
                             ('source_date_epoch', True), ('source_date_epoch', -1),
                             ('compiler', {}), ('flags', {}), ('inputs', {}), ('output', {})):
            with self.subTest(field=field, value=value):
                self.record = {**good, field: value}
                self.write()
                with self.assertRaises(ValueError):
                    self.check()

    def test_every_archived_input_mismatch(self):
        for name in manifest.INPUTS:
            with self.subTest(name=name):
                path = self.root / name
                original = path.read_bytes()
                path.write_bytes(original + b'changed')
                with self.assertRaisesRegex(ValueError, 'Archived input hash mismatch'):
                    self.check()
                path.write_bytes(original)

    def test_glibc_revision_and_each_package(self):
        good = copy.deepcopy(self.record)
        for field in ('glibc_version', *manifest.PACKAGES):
            with self.subTest(field=field):
                self.record = copy.deepcopy(good)
                if field == 'glibc_version':
                    self.record[field] = '2.39'
                else:
                    self.record['glibc_packages'][field] = '2.39-0ubuntu8.9'
                self.write()
                with self.assertRaisesRegex(ValueError, 'Wrong glibc revision'):
                    self.check()

    def test_output_hash_mismatch(self):
        (self.vendor / 'usbboot').write_bytes(elf() + b'changed')
        with self.assertRaisesRegex(ValueError, 'Output hash mismatch'):
            self.check()

    def test_dynamic_interp_needed_and_invalid_elf(self):
        # PT_DYNAMIC is rejected even before examining its DT_NEEDED records.
        for data in (elf(3), elf(2), b'not ELF', elf()[:100]):
            with self.subTest(data=data[:8]):
                (self.vendor / 'usbboot').write_bytes(data)
                self.record['output']['sha256'] = manifest.sha(self.vendor / 'usbboot')
                self.write()
                with self.assertRaisesRegex(ValueError, 'Dynamic|ELF'):
                    self.check()

    def test_unsafe_path_and_unpinned_archive(self):
        self.record['inputs']['../escape'] = '0' * 64
        self.write()
        with self.assertRaisesRegex(ValueError, 'inventory'):
            self.check()
        self.record['inputs'].pop('../escape')
        self.record['inputs'][manifest.LIBUSB] = '0' * 64
        self.write()
        with self.assertRaisesRegex(ValueError, 'upstream libusb'):
            self.check()

    def test_create_from_evidence(self):
        out = Path(self.temp.name) / 'candidate'
        out.mkdir()
        (out / 'usbboot').write_bytes(elf())
        evidence = {'image.id': self.record['image_id'], 'source-date-epoch.txt': '1700000000',
                    'compiler-version.txt': '13.3.0', 'libusb-cflags.txt': ' '.join(manifest.CFLAGS),
                    'link-flags.txt': ' '.join(manifest.LINK),
                    'input-sha256.txt': ''.join(f'{v}  {k}\n' for k, v in self.record['inputs'].items()),
                    'packages.tsv': ''.join(f'{p}:amd64\t{manifest.GLIBC}\tamd64\tglibc\t{manifest.GLIBC}\n'
                                            for p in manifest.PACKAGES)}
        for name, text in evidence.items():
            (out / name).write_text(text)
        manifest.create(self.root, out)
        manifest.validate(self.root, out, 'linux-x86_64')
        with self.assertRaises(FileExistsError):
            manifest.create(self.root, out)
