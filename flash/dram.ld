/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 diskOS contributors */
/* DRAM NAND tools: body linked right after the 0x60-byte header+stub (dram_head.bin) loaded at 0xa0c00000.
 * The stub (at 0xa0c00030, the usbboot --start1 address) saves ra/sp, switches to a private stack at
 * 0xa0bffff0, calls the C entry at 0xa0c00060 (.text.entry), restores and returns to the mask-ROM loop. */
SECTIONS {
  . = 0xa0c00060;
  .text : { *(.text.entry) *(.text*) *(.rodata*) }
  .data : { *(.data*) *(.sdata*) }
  .bss : { *(.bss*) *(.sbss*) *(COMMON) }
  /DISCARD/ : { *(.MIPS.*) *(.comment) *(.reginfo) *(.pdr) *(.gnu.*) *(.note*) }
}
