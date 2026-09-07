/* Stage 1 -- the 440 bytes a BIOS will run.
 *
 * --- What this is, and why it is so small ------------------------------------
 *
 * A PC BIOS reads the first sector of the boot disk to 0x7C00, checks for
 * 0xAA55 in its last two bytes, and jumps to it in 16-bit real mode. That is
 * the entire interface. There is no filesystem, no memory map, no console
 * beyond an interrupt that prints one character, and no more than **440 bytes**
 * before the partition table starts at offset 446.
 *
 * So stage 1 does not load the kernel, and it does not try. Its whole job is to
 * read stage 2 off the disk and jump to it, and to say something a person can
 * act on if it cannot.
 *
 * --- Where stage 2 is, and why it is not searched for ------------------------
 *
 * The LBA of stage 2 is a value **patched into this binary by the installer**,
 * at the label `stage2_lba` below. It is not searched for, because searching
 * needs a partition table reader and a filesystem reader, and neither fits in
 * what is left after the code that reads a sector.
 *
 * This is the one place in the project where a constant is written into a
 * binary by a tool rather than derived at runtime, and it is worth being uneasy
 * about: an installer that patches the wrong number produces a machine that
 * does not start, and the fault is in a program that has already finished
 * running by then. It is mitigated by stage 2 carrying a magic number that this
 * code checks before jumping -- so the failure is a message rather than a jump
 * into whatever happened to be at that sector.
 *
 * --- The failure messages ----------------------------------------------------
 *
 * Four bytes each, because that is what there is room for, and each one is
 * distinct so that the message identifies the step:
 *
 *   E:13   the BIOS has no INT 13h extensions -- it cannot address the disk by
 *          block number at all, which on a GPT disk means it cannot reach
 *          anything we wrote
 *   E:RD   the read itself failed, and the BIOS said so
 *   E:S2   the read succeeded and what came back is not stage 2
 *
 * A machine that halts with four characters is not friendly. A machine that
 * jumps into an unread sector is worse, and the difference between the three is
 * the difference between "the firmware is too old", "the disk is failing" and
 * "the installer wrote the wrong number".
 */

	.code16
	.section .text
	.globl _start

/* Where stage 2 is loaded. 0x8000 is above this code at 0x7C00 and below the
 * 640K line, in the region every PC has as conventional memory. */
	.set STAGE2_SEG,   0x0000
	.set STAGE2_OFF,   0x8000
	.set STAGE2_MAGIC, 0x32424352	/* "RCB2" as the four bytes appear */

/* Stage 2 begins with its magic number, so its first instruction is four bytes
 * in. Putting the magic first rather than after a jump means the check reads
 * offset zero of what was loaded -- the one place that is certainly the start
 * of the sector we asked for, whatever the file's internal layout turns out to
 * be later. */
	.set STAGE2_ENTRY, STAGE2_OFF + 4

_start:
	cli
	xorw	%ax, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss

	/* The stack grows down from where we are loaded, so it runs away from
	 * this code rather than into it. */
	movw	$0x7C00, %sp
	sti

	/* The BIOS passes the drive it booted from in DL, and it is the only
	 * way to know which disk this is: a machine may have several, and the
	 * one we came from is not necessarily the first. Saved immediately,
	 * because DL does not survive the calls below. */
	movb	%dl, boot_drive

	call	serial_init

	movw	$msg_boot, %si
	call	print

/* --- can this BIOS address the disk by block number? ----------------------- */

	movb	$0x41, %ah
	movw	$0x55AA, %bx
	movb	boot_drive, %dl
	int	$0x13
	jc	err_ext
	cmpw	$0xAA55, %bx
	jne	err_ext

/* --- read stage 2 --------------------------------------------------------- */

	movw	$dap, %si
	movb	boot_drive, %dl
	movb	$0x42, %ah
	int	$0x13
	jc	err_read

	/* Is what came back actually ours? The LBA was patched in by a program
	 * that is no longer running, so this is the only chance to notice it
	 * was wrong before jumping into the result. */
	movw	$STAGE2_OFF, %bx
	cmpl	$STAGE2_MAGIC, (%bx)
	jne	err_magic

	/* DL still holds the drive: stage 2 needs it for every read it makes,
	 * and there is no other way for it to find out. */
	movb	boot_drive, %dl
	ljmp	$STAGE2_SEG, $STAGE2_ENTRY

/* --- the three ways this ends badly --------------------------------------- */

err_ext:
	movw	$msg_ext, %si
	jmp	die
err_read:
	movw	$msg_read, %si
	jmp	die
err_magic:
	movw	$msg_magic, %si
die:
	call	print
	cli
1:	hlt
	jmp	1b

/* --- printing, which is one interrupt ------------------------------------- */

/* Two places, because they are for two different readers.
 *
 * INT 10h puts the character on the screen, which is where a person standing in
 * front of a machine that will not start is looking. COM1 puts it on the serial
 * port, which is where the test harness is looking -- and on a headless machine
 * it is the only place anybody is looking at all.
 *
 * Writing to COM1 directly rather than relying on the firmware is not
 * belt-and-braces. Under `-nographic` QEMU mirrors the VGA text console to
 * stdout, which made INT 10h output *appear* on the harness's stdin and hid
 * this gap for an evening -- and that mirror drops characters, so `E:RD` was
 * read as `E:RD` sometimes and `E:R` others. With `-serial stdio` and no VGA
 * mirror, the same loader produced **nothing at all**: none of it had ever
 * reached a serial port.
 *
 * Everything is saved around the BIOS call. AH=0Eh promises AX and says nothing
 * about any other register.
 */
serial_init:
	pushaw
	movw	$0x03F9, %dx	/* interrupt enable: none */
	xorb	%al, %al
	outb	%al, %dx
	movw	$0x03FB, %dx	/* line control: DLAB, to set the divisor */
	movb	$0x80, %al
	outb	%al, %dx
	movw	$0x03F8, %dx	/* divisor low: 1, for 115200 */
	movb	$0x01, %al
	outb	%al, %dx
	movw	$0x03F9, %dx	/* divisor high */
	xorb	%al, %al
	outb	%al, %dx
	movw	$0x03FB, %dx	/* line control: 8 bits, no parity, 1 stop */
	movb	$0x03, %al
	outb	%al, %dx
	popaw
	ret

putc:
	pushaw
	movb	$0x0E, %ah	/* teletype output */
	movw	$0x0007, %bx	/* page 0, light grey */
	int	$0x10
	popaw

	pushaw
	movb	%al, %ah	/* the character, kept out of the poll below */
	movw	$0x03FD, %dx	/* line status */
1:	inb	%dx, %al
	testb	$0x20, %al	/* transmit holding register empty */
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

/* --- data ----------------------------------------------------------------- */

msg_boot:
	.asciz	"ReconOS\r\n"
msg_ext:
	.asciz	"E:13\r\n"
msg_read:
	.asciz	"E:RD\r\n"
msg_magic:
	.asciz	"E:S2\r\n"

boot_drive:
	.byte	0x80

/* The disk address packet the BIOS reads block numbers out of.
 *
 * `stage2_lba` and `stage2_count` are the two values the installer patches. The
 * label is exported so that a tool can find the offset by reading the symbol
 * table rather than by counting bytes in a hex dump -- an offset worked out by
 * hand is a number that goes wrong silently the first time this file is
 * edited.
 */
	.align 4
dap:
	.byte	0x10		/* packet size */
	.byte	0
	.globl	stage2_count
stage2_count:
	.word	4		/* sectors to read; patched by the installer */
	.word	STAGE2_OFF
	.word	STAGE2_SEG
	.globl	stage2_lba
stage2_lba:
	.quad	2048		/* patched by the installer */

/* Fits, or the build stops here.
 *
 * 440 bytes is where the partition table starts. A stage 1 that overran it
 * would assemble, link, and then destroy the partition table of the first disk
 * it was written to -- which is the single worst thing this project could ship,
 * and it is caught by a subtraction at assembly time. */
	.space	440 - (. - _start), 0
