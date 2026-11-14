#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 diskOS contributors
# Reproducible build of the DRAM NAND tools (mask-ROM USB). Proves the recipe first: my_write5.c must rebuild
# the shipped my_write5_dram.bin byte-for-byte, else nothing is written. Usage: sh build_nand.sh [outdir]
# Needs mipsel-linux-gnu-gcc-12 / -ld / -objcopy (Debian: gcc-12-mipsel-linux-gnu binutils-mipsel-linux-gnu).
set -eu
cd "$(dirname "$0")"
OUT=${1:-build}
CC=${CC:-mipsel-linux-gnu-gcc-12}; LD=${LD:-mipsel-linux-gnu-ld}; OBJCOPY=${OBJCOPY:-mipsel-linux-gnu-objcopy}
CFLAGS="-O2 -march=mips32r2 -EL -mno-abicalls -fno-pic -G0 -ffreestanding -nostdlib -fno-builtin"
HEAD_SHA=cb7feac9eade134bb42a6faa1166b5640947d7d6e515ea5782157809b603e5bc
[ "$(sha256sum dram_head.bin | cut -d" " -f1)" = "$HEAD_SHA" ] || { echo "dram_head.bin changed" >&2; exit 1; }
mkdir -p "$OUT"
# dram_head.bin is not an opaque blob: it is dram_head.S (our own entry stub) assembled and linked with dram_head.ld
# (the 0x30 bytes before the stub are the linker's .reginfo/.MIPS.abiflags sections). Prove it regenerates byte-exactly.
$CC -march=mips32r2 -EL -mno-abicalls -fno-pic -G0 -Os -nostdlib -ffreestanding -c dram_head.S -o "$OUT/h.o"
$LD -EL -T dram_head.ld --defsym my_write=0xa0c00060 "$OUT/h.o" -o "$OUT/h.elf"
$OBJCOPY -O binary "$OUT/h.elf" "$OUT/h.bin"
cmp -s "$OUT/h.bin" dram_head.bin || { echo "REFUSING: dram_head.S does not regenerate dram_head.bin" >&2; exit 1; }
rm -f "$OUT/h.o" "$OUT/h.elf" "$OUT/h.bin"
echo "dram_head.bin regenerated from dram_head.S byte-exactly"
build(){  # build <src> <out.bin> [extra cflags...]; keeps <out>.elf for the checks below
    src=$1; bin=$2; shift 2
    $CC $CFLAGS "$@" -c "$src" -o "$OUT/t.o"
    $LD -EL -T dram.ld "$OUT/t.o" -o "$bin.elf"
    if ${NM:-mipsel-linux-gnu-nm} -u "$bin.elf" | grep -q .; then echo "REFUSING: undefined symbols in $bin" >&2; exit 1; fi
    $OBJCOPY -O binary "$bin.elf" "$OUT/t.body"
    cat dram_head.bin "$OUT/t.body" > "$bin"
    rm -f "$OUT/t.o" "$OUT/t.body"
    # the tool is loaded at 0xa0c00000 and must end below the plan blob (0xa0b00000 is BELOW it; the next region up is
    # the stack top 0xa0bffff0 growing down, and SRC at 0xa1000000): keep it well under 1 MiB
    [ "$(stat -c %s "$bin")" -lt 262144 ] || { echo "REFUSING: $bin too large" >&2; exit 1; }
}
# A no-write build must not contain ANY erase/program/unlock function or write command template.
no_write_check(){
    elf=$1
    if ${NM:-mipsel-linux-gnu-nm} "$elf" | grep -E ' (block_erase|program_page|write_block_verify|set_features|cdt_init_write|wr_bulk|wait_ready)$'; then
        echo "REFUSING: write code present in $elf" >&2; exit 1; fi
    # the four write CDT command words (WREN 0x06, PROGRAM_LOAD 0x02, PROGRAM_EXECUTE 0x10, BLOCK_ERASE 0xd8) as built
    # by CDT_XFER must not be materialised anywhere in the code
    ${OBJDUMP:-mipsel-linux-gnu-objdump} -d "$elf" > "$elf.dis"
    for w in 0x1000006 0x9010002 0xd000010 0xd0000d8; do
        lo=$(printf '0x%x' $(( w & 0xffff ))); hi=$(printf '0x%x' $(( w >> 16 )))
        if grep -qiE "lui[^,]*,$hi\$" "$elf.dis" && grep -qiE "(ori|addiu)[^,]*,[^,]*,$lo\$" "$elf.dis"; then
            echo "REFUSING: write command word $w may be present in $elf" >&2; exit 1; fi
    done
}
build my_write5.c "$OUT/my_write5_repro.bin"
if ! cmp -s "$OUT/my_write5_repro.bin" my_write5_dram.bin; then
    echo "REFUSING: this toolchain does not reproduce the shipped my_write5_dram.bin - recipe drift" >&2; exit 1
fi
echo "recipe verified: my_write5_dram.bin reproduces byte-exactly"
build my_write6.c "$OUT/my_write6_dram.bin"
build my_write6.c "$OUT/my_write6_probe_dram.bin" -DPROBE_ONLY
build my_write6.c "$OUT/my_write6_gate_dram.bin" -DGATE_ONLY
no_write_check "$OUT/my_write6_probe_dram.bin.elf"
no_write_check "$OUT/my_write6_gate_dram.bin.elf"
${NM:-mipsel-linux-gnu-nm} "$OUT/my_write6_dram.bin.elf" | grep -q ' cdt_init_write$' || { echo "REFUSING: production writer lost its write path" >&2; exit 1; }
for f in my_write6_dram.bin my_write6_probe_dram.bin my_write6_gate_dram.bin; do
    echo "$(sha256sum < "$OUT/$f" | cut -d' ' -f1)  $(stat -c %s "$OUT/$f")  $f"
done
