#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Create candidate provenance or check it against an archived release tree."""
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

GLIBC = '2.39-0ubuntu8.8'
LIBUSB = 'corresponding-source/libusb-1.0_1.0.27.orig.tar.bz2'
LIBUSB_SHA = 'ffaa41d741a8a3bee244ac8e54a72ea05bf2879663c098c82fc5757853441575'
INPUTS = ('src/usbboot/usbboot.c', 'build/build-usbboot-pinned.sh',
          'build/prepare-usbboot-env.sh', 'build/usbboot_manifest.py', LIBUSB,
          LIBUSB + '.asc', 'corresponding-source/libusb-1.0_1.0.27-1.dsc',
          'corresponding-source/libusb-1.0_1.0.27-1.debian.tar.xz',
          'corresponding-source/glibc_2.39-0ubuntu8.8.dsc',
          'corresponding-source/glibc_2.39-0ubuntu8.8.debian.tar.xz',
          'corresponding-source/glibc_2.39.orig.tar.xz',
          'corresponding-source/glibc_2.39.orig.tar.xz.asc')
PACKAGES = ('libc6', 'libc6-dev', 'libc-bin', 'libc-dev-bin')
CFLAGS = '-O2 -march=x86-64 -mtune=generic -ffile-prefix-map=/src=. -ffile-prefix-map=/out=.'.split()
CONFIGURE = '--disable-udev --enable-static --disable-shared --disable-examples-build --disable-tests-build'.split()
LINK = CFLAGS + '-std=c99 -Ilibusb -static -no-pie -Wl,-Map,/out/usbboot.map -lpthread'.split()
FLAGS = dict(libusb_cflags=CFLAGS, configure=CONFIGURE, link=LINK)


def regular(path):
    if path.is_symlink() or not path.is_file() or any(p.is_symlink() for p in path.parents):
        raise ValueError('Missing/nonregular file: ' + str(path))
    return path


def sha(path):
    with regular(path).open('rb') as stream:
        result = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def digest(value):
    return isinstance(value, str) and re.fullmatch('[0-9a-f]{64}', value)


def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate manifest key: ' + key)
        result[key] = value
    return result


def static_elf(path):
    data = regular(path).read_bytes()
    if len(data) < 64 or data[:7] != b'\x7fELF\x02\x01\x01':
        raise ValueError('usbboot must be a Linux x86-64 ELF')
    header = struct.unpack_from('<HHIQQQIHHHHHH', data, 16)
    kind, machine, version, _, offset, _, _, size, entry_size, count, *_ = header
    if (kind, machine, version, size, entry_size) != (2, 62, 1, 64, 56) or not count or count == 65535:
        raise ValueError('Invalid x86-64 executable header')
    if offset < 64 or offset + count * entry_size > len(data):
        raise ValueError('Invalid ELF program table')
    load = False
    for i in range(count):
        segment, _, start, _, _, length, _, _ = struct.unpack_from('<IIQQQQQQ', data, offset + i * 56)
        if start + length > len(data):
            raise ValueError('Truncated ELF segment')
        if segment in (2, 3):
            raise ValueError('Dynamic usbboot: INTERP/DYNAMIC (including NEEDED) is forbidden')
        load |= segment == 1
    if not load:
        raise ValueError('ELF has no load segment')


def validate(root, vendor, target):
    if target != 'linux-x86_64':
        raise ValueError('No pinned usbboot recipe for target: ' + target)
    path = regular(vendor / 'usbboot.manifest')
    manifest = json.loads(path.read_text(encoding='ascii'), object_pairs_hook=unique)
    fields = {'schema', 'target', 'inputs', 'image_id', 'source_date_epoch', 'glibc_version',
              'glibc_packages', 'compiler', 'flags', 'output'}
    if not isinstance(manifest, dict) or set(manifest) != fields:
        raise ValueError('Malformed manifest fields')
    if type(manifest['schema']) is not int or manifest['schema'] != 1 or manifest['target'] != target:
        raise ValueError('Unsupported manifest schema/target')
    if not isinstance(manifest['image_id'], str) or not re.fullmatch(r'sha256:[0-9a-f]{64}', manifest['image_id']):
        raise ValueError('Malformed image ID')
    epoch = manifest['source_date_epoch']
    if type(epoch) is not int or epoch < 0:
        raise ValueError('Malformed source epoch')
    if manifest['glibc_version'] != GLIBC or manifest['glibc_packages'] != dict.fromkeys(PACKAGES, GLIBC):
        raise ValueError('Wrong glibc revision')
    compiler = manifest['compiler']
    if (not isinstance(compiler, dict) or set(compiler) != {'command', 'version'} or
            compiler['command'] != 'gcc-13' or not isinstance(compiler['version'], str) or
            not re.fullmatch(r'13\.[0-9]+\.[0-9]+', compiler['version'])):
        raise ValueError('Malformed compiler version')
    if manifest['flags'] != FLAGS:
        raise ValueError('Wrong build flags')
    inputs = manifest['inputs']
    if not isinstance(inputs, dict) or set(inputs) != set(INPUTS):
        raise ValueError('Incomplete/unsafe manifest input inventory')
    if inputs[LIBUSB] != LIBUSB_SHA:
        raise ValueError('Wrong upstream libusb archive hash')
    for name, expected in inputs.items():
        if not digest(expected) or sha(root / name) != expected:
            raise ValueError('Archived input hash mismatch: ' + name)
    output = manifest['output']
    if (not isinstance(output, dict) or set(output) != {'file', 'sha256'} or
            output['file'] != 'usbboot' or not digest(output['sha256']) or
            sha(vendor / 'usbboot') != output['sha256']):
        raise ValueError('Output hash mismatch: staged usbboot')
    static_elf(vendor / 'usbboot')


def create(root, out):
    inputs = unique(line.split('  ', 1)[::-1] for line in (out / 'input-sha256.txt').read_text().splitlines())
    packages = {}
    for line in (out / 'packages.tsv').read_text().splitlines():
        name, version, arch, *_ = line.split('\t')
        name = name.removesuffix(':amd64')
        if name in PACKAGES:
            if arch != 'amd64' or name in packages:
                raise ValueError('Unexpected glibc package architecture/duplicate')
            packages[name] = version
    if (out / 'libusb-cflags.txt').read_text().split() != CFLAGS or (out / 'link-flags.txt').read_text().split() != LINK:
        raise ValueError('Evidence flags differ from recipe')
    manifest = dict(schema=1, target='linux-x86_64', inputs=inputs,
                    image_id=(out / 'image.id').read_text().strip(),
                    source_date_epoch=int((out / 'source-date-epoch.txt').read_text()),
                    glibc_version=GLIBC, glibc_packages=packages,
                    compiler=dict(command='gcc-13', version=(out / 'compiler-version.txt').read_text().strip()),
                    flags=FLAGS, output=dict(file='usbboot', sha256=sha(out / 'usbboot')))
    with (out / 'usbboot.manifest').open('x', encoding='ascii') as stream:
        json.dump(manifest, stream, sort_keys=True, indent=2)
        stream.write('\n')
    validate(root, out, 'linux-x86_64')


if __name__ == '__main__':
    try:
        if len(sys.argv) == 4 and sys.argv[1] == 'create':
            create(Path(sys.argv[2]).resolve(), Path(sys.argv[3]).resolve())
        elif len(sys.argv) == 5 and sys.argv[1] == 'check':
            validate(Path(sys.argv[2]).resolve(), Path(sys.argv[3]).resolve(), sys.argv[4])
        else:
            raise ValueError('Usage: usbboot_manifest.py create ROOT OUT | check ARCHIVE VENDOR TARGET')
    except (ValueError, OSError, TypeError, KeyError, struct.error) as error:
        raise SystemExit('FAIL: usbboot manifest: ' + str(error))
