/* The far end of stage 1's jump, and the smallest possible amount of it.
 *
 * Everything stage 2 does beyond this file is C, compiled with `clang -m16`.
 * That is worth the two-file split: what remains of checkpoint 16 is a FAT32
 * reader, an RSA-2048 signature check over a SHA-256 hash, page tables and a
 * handoff structure -- and writing those in 16-bit assembly would produce
 * something nobody could review against the UEFI loader that already does the
 * same jobs in C.
 *
 * This file exists because three things cannot be said in C: the magic number
 * has to be the first four bytes, the segment registers have to be right before
 * any C code touches a pointer, and there has to be a stack before there can be
 * a call.
 */

	.code16
	.section .text.entry, "ax"
	.globl _start

_start:
	/* Stage 1 checks these four bytes and jumps to the fifth. */
	.ascii	"RCB2"

	cli
	xorw	%ax, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss

	/* Clear of stage 1 at 0x7C00 and stage 2 at 0x8000, growing down
	 * through conventional memory that nothing else is using yet. */
	movw	$0x7000, %sp
	sti

	/* Zero the statics.
	 *
	 * objcopy -O binary drops .bss, so it occupies no bytes in the image
	 * and whatever the firmware left at those addresses would otherwise be
	 * the initial value of every static in the C. On this machine that is
	 * usually zero, which is the worst case: it would work here and fail on
	 * a machine whose firmware used the memory for something. */
	movw	$__bss_start, %di
	movw	$__bss_end, %cx
	subw	%di, %cx
	xorb	%al, %al
	cld
	rep stosb

	/* DL is the drive the firmware booted from, handed across by stage 1.
	 * Widened to an int for the C side, which has no way to see a register.
	 */
	xorl	%eax, %eax
	movb	%dl, %al
	pushl	%eax
	calll	stage2_main

	/* stage2_main does not return in normal operation -- it either starts
	 * the kernel or reports why it cannot. Reaching here at all is a fault,
	 * and halting is the only honest thing left. */
	cli
1:	hlt
	jmp	1b
