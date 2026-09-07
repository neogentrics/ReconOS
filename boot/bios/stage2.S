/* Stage 2 -- the part with room to work in.
 *
 * Loaded to 0x8000 by stage 1, in 16-bit real mode, with the boot drive in DL.
 * It lives in the BIOS Boot Partition, so it has a megabyte rather than 440
 * bytes, and everything stage 1 could not afford belongs here:
 *
 *   - the memory map, from INT 15h AX=E820
 *   - a FAT32 reader, because BIOS hands out sectors and the kernel is a file
 *   - the signature check, because **a BIOS path that skips it is the
 *     off-switch the UEFI path deliberately does not have**
 *   - A20, a GDT, page tables, and the switch to long mode
 *   - the ReconBoot handoff, identical to the one the UEFI loader builds, so
 *     that the kernel cannot tell which loader started it
 *
 * None of that is here yet. What is here is the far end of the jump, printing
 * enough to prove the chain: BIOS -> our 440 bytes -> our stage 2, on a machine
 * with no UEFI anywhere in it.
 *
 * That is worth having on its own, because every item in the list above is
 * written against a machine state that is either real or assumed, and until
 * something arrives here it was assumed.
 */

	.code16
	.section .text
	.globl _start

/* The magic stage 1 checks, at offset zero.
 *
 * Stage 1 jumps to offset 4, not offset 0 -- so this is data that is never
 * executed, and the first instruction below is the entry point. */
_start:
	.ascii	"RCB2"

entry:
	cli
	xorw	%ax, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss

	/* Well clear of both stage 1 at 0x7C00 and stage 2 at 0x8000, and
	 * still inside conventional memory. */
	movw	$0x7000, %sp
	sti

	/* DL is the boot drive, handed over by stage 1 because the BIOS gave it
	 * to stage 1 and there is no other way to learn it. Saved before
	 * anything can clobber it. */
	movb	%dl, boot_drive

	movw	$msg, %si
	call	print

	/* Print the drive number, because "which disk did the firmware pick"
	 * is the first question when a machine boots the wrong thing, and it
	 * is free to answer here. */
	movb	boot_drive, %al
	call	print_hex_byte

	movw	$crlf, %si
	call	print

	cli
1:	hlt
	jmp	1b

/* --- printing ------------------------------------------------------------- */

/* The screen and the serial port, for the two different readers -- see the same
 * routine in stage1.S for why the serial half is written by hand rather than
 * left to the firmware. Stage 1 has already initialised the port; stage 2 is
 * only ever reached through it.
 *
 * Everything is saved around the BIOS call: AH=0Eh promises AX and says nothing
 * about any other register, so anything that must survive one lives in memory
 * or on the stack.
 */
putc:
	pushaw
	movb	$0x0E, %ah
	movw	$0x0007, %bx
	int	$0x10
	popaw

	pushaw
	movb	%al, %ah
	movw	$0x03FD, %dx
1:	inb	%dx, %al
	testb	$0x20, %al
	jz	1b
	movw	$0x03F8, %dx
	movb	%ah, %al
	outb	%al, %dx
	popaw
	ret

print:
1:	lodsb
	testb	%al, %al
	jz	2f
	call	putc
	jmp	1b
2:	ret

/* The byte is kept in memory rather than in a register, for the reason above.
 * A register would work on this BIOS today and fail on the next one. */
print_hex_byte:
	movb	%al, hex_byte
	shrb	$4, %al
	call	1f
	movb	hex_byte, %al
	andb	$0x0F, %al
	call	1f
	ret
1:	addb	$'0', %al
	cmpb	$'9', %al
	jbe	2f
	addb	$('a' - '0' - 10), %al
2:	jmp	putc

/* --- data ----------------------------------------------------------------- */

msg:
	.asciz	"stage2 ok, drive 0x"
crlf:
	.asciz	"\r\n"

boot_drive:
	.byte	0
hex_byte:
	.byte	0
