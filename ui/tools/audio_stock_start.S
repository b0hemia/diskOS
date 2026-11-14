/* SPDX-License-Identifier: MIT */
/* o32 NaN-2008 entry; no executable constructors or finalizers are used.
 * Shared-library initialization/finalization belongs to the stock loader. */
.set noreorder
.text
.globl __start
.ent __start
__start:
 bal 1f
 nop
1:
 .cpload $ra
 move $ra,$zero
 lw $a0,%got(main)($gp)
 lw $a1,0($sp)
 addiu $a2,$sp,4
 li $t0,-8
 and $sp,$sp,$t0
 addiu $sp,$sp,-32
 move $a3,$zero
 sw $zero,16($sp)
 sw $v0,20($sp)
 sw $sp,24($sp)
 lw $t9,%call16(__libc_start_main)($gp)
 jalr $t9
 nop
2: b 2b
 nop
.end __start
.section .note.GNU-stack,"",@progbits
/* glibc uses this exported marker to select the current stdio layout. */
.section .rodata
.globl _IO_stdin_used
.type _IO_stdin_used,@object
.size _IO_stdin_used,4
_IO_stdin_used:
.word 0x20001
