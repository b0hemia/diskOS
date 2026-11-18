#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Assemble the USB stage-1 from the GPL-built SPL + the W63AH6NKB DDR parameter block.

  stage1 = BDIF record + DDR record (35 W63AH6NKB reg words) + zero-pad to 0x800 + u-boot-spl.bin

The mask ROM loads this at 0xb2401000 and executes the SPL at 0xb2401800; the SPL reads the
parameter records at 0xb2401000. Reproducible: same inputs -> identical output.

Usage: assemble_stage1.py <u-boot-spl.bin> <ginfo_w63ah6nkb.bin> <out disc_spl_lpddr3.bin>
"""
import struct, sys, hashlib

def main(spl_path, ginfo_path, out_path):
    g = open(ginfo_path, 'rb').read()
    # BDIF (board-info) record verbatim: [magic][size=184][184B payload] = 0xc0 bytes
    bdif = g[0x000:0x0c0]
    if struct.unpack_from('<I', bdif, 0)[0] != 0x42444946:
        raise SystemExit("BDIF magic missing in ginfo")
    # DDR record payload = the 35 register words (old ABI: no name[32] header), i.e. vendor
    # DDR payload bytes [+0x2c .. +0xb8] which map 1:1 onto struct ddr_registers (with ddr_mr11).
    ddr_payload = g[0xc8 + 0x2c : 0xc8 + 0x2c + 35 * 4]
    if len(ddr_payload) != 140:
        raise SystemExit("unexpected DDR payload length")
    ddr_magic = (ord('D') << 24) | (ord('D') << 16) | (ord('R') << 8) | 0
    ginfo = bdif + struct.pack('<II', ddr_magic, len(ddr_payload)) + ddr_payload + struct.pack('<I', 0)
    if len(ginfo) > 0x7c0:
        raise SystemExit("param block collides with the 0x7c0 diagnostic scratch")
    spl = open(spl_path, 'rb').read()
    buf = bytearray(ginfo) + b'\x00' * (0x800 - len(ginfo)) + spl
    if len(buf) > 0x3000:
        raise SystemExit("stage-1 exceeds the ~0x3000 TCSM window: %d bytes" % len(buf))
    open(out_path, 'wb').write(buf)
    print("wrote %s: %d bytes (0x%X), TCSM headroom %d, sha256=%s"
          % (out_path, len(buf), len(buf), 0x3000 - len(buf), hashlib.sha256(buf).hexdigest()))

if __name__ == '__main__':
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    main(*sys.argv[1:])
