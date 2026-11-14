/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 diskOS contributors */
/* Links dram_head.S at 0xa0c00000. With this layout the linker places its own .reginfo (0x18 bytes) and
 * .MIPS.abiflags (0x18 bytes) sections first, so the stub lands at 0xa0c00030 (the usbboot --start1 address)
 * and the whole image is 0x60 bytes; the C body (dram.ld) is appended at 0xa0c00060. Do not add /DISCARD/ or
 * reorder: those two linker sections are part of dram_head.bin. build_nand.sh cmp's the result to the committed blob. */
ENTRY(_start)
SECTIONS {
  . = 0xa0c00000;
  .text : { *(.text.start) *(.text.entry) *(.text*) *(.rodata*) *(.data*) }
  .bss  : { *(.bss*) *(COMMON) }
}
