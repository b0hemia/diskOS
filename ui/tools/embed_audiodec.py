#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Carry a finished helper inside mq_ui, and inspect the result.

image HELPER OUT [--arch ARCH] [--kind KIND]
    Validate HELPER (a finished ELF executable, at most 256 KiB) and write OUT:
    a 48-byte descriptor (magic, length, zero, SHA-256) followed by the exact
    helper bytes. The kind's stub (audio_stock_image.S, artdec_image.S) includes
    both parts. A missing, empty, placeholder, oversized, foreign or malformed
    helper fails.
verify UI HELPER RAW [--kind KIND]
    The final UI carries exactly HELPER's bytes as the kind's read-only section
    (.diskos_audiodec, .diskos_artdec), inside a non-writable load segment, plus
    exactly one matching descriptor, and stays within the deployed 32 MiB
    verifier cap.

KIND is the helper: audiodec (the default, the stock-library decoder) or artdec
(the artwork helper); each has its own descriptor magic and section. ARCH is the
helper's target. Production uses the default (32-bit little-endian MIPS); host
tests embed a host helper with --arch x86_64 or aarch64.
"""
import argparse
import hashlib
import os
import struct
import sys

DESCRIPTOR = 48
CAP = 256 * 1024
UI_CAP = 32 * 1024 * 1024
KIND = {'audiodec': (b'DADEC001', b'.diskos_audiodec'), 'artdec': (b'DARTD001', b'.diskos_artdec')}
ARCH = {'mips32le': (1, 8), 'x86_64': (2, 62), 'aarch64': (2, 183)}


class Refused(Exception):
    pass


def elf_header(data, what):
    """Return (class, header fields) of a little-endian ELF, or refuse."""
    if len(data) < 16 or data[:4] != b'\x7fELF':
        raise Refused(what + ' is not an ELF file (placeholder or wrong artifact?)')
    cls = data[4]
    if cls not in (1, 2) or data[5] != 1 or data[6] != 1:
        raise Refused(what + ' is not a current-version little-endian ELF')
    if cls == 1:
        if len(data) < 52:
            raise Refused(what + ' has a truncated ELF header')
        e_type, e_machine, _, _, e_phoff, e_shoff, _, _, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = \
            struct.unpack_from('<HHIIIIIHHHHHH', data, 16)
    else:
        if len(data) < 64:
            raise Refused(what + ' has a truncated ELF header')
        e_type, e_machine, _, _, e_phoff, e_shoff, _, _, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = \
            struct.unpack_from('<HHIQQQIHHHHHH', data, 16)
    return cls, dict(type=e_type, machine=e_machine, phoff=e_phoff, shoff=e_shoff, phentsize=e_phentsize,
                     phnum=e_phnum, shentsize=e_shentsize, shnum=e_shnum, shstrndx=e_shstrndx)


def check_helper(data, arch):
    want_class, want_machine = ARCH[arch]
    if not data:
        raise Refused('helper is empty')
    if len(data) > CAP:
        raise Refused('helper is %d bytes, over the %d byte cap' % (len(data), CAP))
    cls, h = elf_header(data, 'helper')
    if cls != want_class or h['machine'] != want_machine:
        raise Refused('helper is not a %s ELF (class %d, machine %d)' % (arch, cls, h['machine']))
    if h['type'] not in (2, 3):
        raise Refused('helper is not an executable ELF (type %d)' % h['type'])
    if not h['phnum'] or h['phentsize'] < (32 if cls == 1 else 56) or \
            h['phoff'] + h['phentsize'] * h['phnum'] > len(data):
        raise Refused('helper program headers are missing or outside the file')


def descriptor(data, kind):
    return KIND[kind][0] + struct.pack('<II', len(data), 0) + hashlib.sha256(data).digest()


def image(helper, out, arch, kind):
    try:
        try:
            with open(helper, 'rb') as f:
                data = f.read(CAP + 1)
        except OSError as error:
            raise Refused('cannot read helper %s: %s' % (helper, error.strerror))
        check_helper(data, arch)
        temporary = out + '.part'
        with open(temporary, 'wb') as f:
            f.write(descriptor(data, kind) + data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(temporary, out)
    except Refused:
        for stale in (out, out + '.part'):
            try:
                os.unlink(stale)
            except FileNotFoundError:
                pass
        raise
    print('embedded helper: %s %d bytes sha256 %s' % (helper, len(data), hashlib.sha256(data).hexdigest()))


def sections(ui, section):
    cls, h = elf_header(ui, 'UI')
    entry = 40 if cls == 1 else 64
    if not h['shnum'] or h['shentsize'] < entry or h['shoff'] + h['shentsize'] * h['shnum'] > len(ui) or \
            h['shstrndx'] >= h['shnum']:
        raise Refused('UI section headers are missing or outside the file')
    rows = []
    for i in range(h['shnum']):
        at = h['shoff'] + i * h['shentsize']
        if cls == 1:
            name, kind, flags, addr, offset, size = struct.unpack_from('<IIIIII', ui, at)
        else:
            name, kind, flags, addr, offset, size = struct.unpack_from('<IIQQQQ', ui, at)
        rows.append((name, kind, flags, addr, offset, size))
    strtab = rows[h['shstrndx']]
    names = ui[strtab[4]:strtab[4] + strtab[5]]
    found = []
    for name, kind, flags, addr, offset, size in rows:
        end = names.find(b'\0', name)
        if names[name:end] == section:
            found.append((kind, flags, addr, offset, size))
    segments = []
    for i in range(h['phnum']):
        at = h['phoff'] + i * h['phentsize']
        if cls == 1:
            kind, offset, vaddr, _, filesz, _, flags, _ = struct.unpack_from('<IIIIIIII', ui, at)
        else:
            kind, flags, offset, vaddr, _, filesz, _, _ = struct.unpack_from('<IIQQQQQQ', ui, at)
        segments.append((kind, flags, offset, vaddr, filesz))
    return found, segments


def verify(ui_path, helper_path, raw_path, kind):
    section = KIND[kind][1]
    with open(ui_path, 'rb') as f:
        ui = f.read()
    with open(helper_path, 'rb') as f:
        helper = f.read()
    with open(raw_path, 'rb') as f:
        raw = f.read()
    if len(ui) > UI_CAP:
        raise Refused('UI is %d bytes, over the deployed %d byte verifier cap' % (len(ui), UI_CAP))
    if raw[:DESCRIPTOR] != descriptor(helper, kind) or raw[DESCRIPTOR:] != helper:
        raise Refused('generated image does not match the helper')
    found, segments = sections(ui, section)
    if len(found) != 1:
        raise Refused('UI has %d %s sections, expected exactly one' % (len(found), section.decode()))
    stype, flags, addr, offset, size = found[0]
    if stype != 1 or flags & 0x7 != 0x2:
        raise Refused('%s is not an allocated read-only data section (type %d flags %#x)' % (section.decode(), stype, flags))
    if ui[offset:offset + size] != helper:
        raise Refused('%s does not hold the exact helper bytes' % section.decode())
    load = [s for s in segments if s[0] == 1 and s[2] <= offset and offset + size <= s[2] + s[4] and
            s[3] - s[2] == addr - offset]
    if len(load) != 1 or load[0][1] & 0x2:
        raise Refused('%s is not inside exactly one non-writable load segment' % section.decode())
    marker = descriptor(helper, kind) + struct.pack('<I', len(helper))
    if ui.count(marker) != 1:
        raise Refused('UI carries %d matching helper descriptors, expected one' % ui.count(marker))
    seg = ''.join(c for bit, c in ((4, 'R'), (2, 'W'), (1, 'E')) if load[0][1] & bit)
    print('%s carries %s: offset %#x, %d bytes, sha256 %s, load segment %s' % (
        ui_path, section.decode(), offset, size, hashlib.sha256(helper).hexdigest(), seg))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='action', required=True)
    p = sub.add_parser('image')
    p.add_argument('helper')
    p.add_argument('out')
    p.add_argument('--arch', choices=sorted(ARCH), default='mips32le')
    p.add_argument('--kind', choices=sorted(KIND), default='audiodec')
    p = sub.add_parser('verify')
    p.add_argument('ui')
    p.add_argument('helper')
    p.add_argument('raw')
    p.add_argument('--kind', choices=sorted(KIND), default='audiodec')
    args = parser.parse_args(argv)
    try:
        if args.action == 'image':
            image(args.helper, args.out, args.arch, args.kind)
        else:
            verify(args.ui, args.helper, args.raw, args.kind)
    except (Refused, OSError, struct.error) as error:
        print('embed_audiodec: ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
