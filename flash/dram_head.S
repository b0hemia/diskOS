/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 diskOS contributors */
/* Entry stub of the DRAM NAND tools (my_write5/6): usbboot --start1 0xa0c00030 lands here. Saves the mask-ROM ra/sp
 * in TCSM scratch, switches to a private DRAM stack, calls the C entry (my_write at 0xa0c00060, see dram.ld), then
 * restores ra/sp and returns to the mask-ROM. dram_head.bin = this file assembled and linked by build_nand.sh. */
    .set noreorder
    .section .text.start,"ax"
    .globl _start
_start:
    lui   $8,  0xb240          /* t0 */
    sw    $31, 0x17f0($8)      /* save mask-ROM ra to TCSM scratch */
    sw    $29, 0x17ec($8)      /* save mask-ROM sp */
    lui   $29, 0xa0bf          /* sp = 0xa0bffff0 (DRAM stack) */
    ori   $29, $29, 0xfff0
    jal   my_write
    nop
    lui   $8,  0xb240
    lw    $31, 0x17f0($8)      /* restore mask-ROM ra */
    lw    $29, 0x17ec($8)      /* restore mask-ROM sp */
    jr    $31                  /* return to mask-ROM (so dbg is readable) */
    nop
