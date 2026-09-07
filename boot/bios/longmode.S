/* Real mode to 64-bit long mode, and the jump into the kernel.
 *
 * The one part of stage 2 that cannot be C. Not because the operations are
 * exotic -- they are half a dozen register writes -- but because between the
 * first and the last of them the machine is not running the language: the
 * segment registers change meaning, the address width changes twice, and there
 * is a moment after `mov %eax, %cr0` where the next instruction fetched must
 * already be the far jump that reloads CS. A compiler is entitled to put
 * anything it likes between two C statements.
 *
 * --- The order, and why it is that order -----------------------------------
 *
 *   1. A20, or the first megabyte repeats every 1MB and the kernel at 0x100000
 *      is a mirror of the interrupt vector table
 *   2. page tables, built while addressing is still simple
 *   3. interrupts off and the IDT emptied, because the real-mode vector table
 *      stops being valid the instant protected mode starts
 *   4. long mode enabled in EFER, paging pointed at CR3
 *   5. protection and paging on together, then a far jump that reloads CS with
 *      a 64-bit code segment
 *
 * Steps 4 and 5 are one step really: on x86_64 you do not enter 32-bit
 * protected mode and then long mode, you enable long mode *first* and then turn
 * on protection and paging in the same write. The CPU goes straight to
 * compatibility mode and the far jump takes it the rest of the way.
 *
 * --- What is handed over ----------------------------------------------------
 *
 * The ReconBoot handoff, exactly as the UEFI loader builds it, with its address
 * in RDI -- the first argument in the ABI the kernel was compiled for. The
 * kernel must not be able to tell which loader started it.
 */

	.code16
	.section .text, "ax"
	.globl enter_long_mode

/* void enter_long_mode(u32 entry_lo, u32 entry_hi, u32 handoff, u32 pml4) */

enter_long_mode:
	/* The arguments, saved before anything below can disturb the stack.
	 * clang -m16 passes them on the stack in order. */
	movl	4(%esp), %eax
	movl	%eax, entry_low
	movl	8(%esp), %eax
	movl	%eax, entry_high
	movl	12(%esp), %eax
	movl	%eax, handoff_addr
	movl	16(%esp), %eax
	movl	%eax, pml4_addr

/* --- A20 ------------------------------------------------------------------
 *
 * Without it, address line 20 is held low and every address above 1MB wraps
 * back into the first megabyte. The kernel at 0x100000 would be the interrupt
 * vector table at 0, and the failure would be a machine that behaves as though
 * the load had silently done nothing.
 *
 * Three ways to open it, tried in order of how likely they are to work without
 * side effects. The last is the keyboard controller, which is the oldest and
 * involves waiting on a chip designed in 1984.
 */
	/* The BIOS's own call first: it knows this machine. */
	movw	$0x2401, %ax
	int	$0x15

	call	a20_enabled
	testb	%al, %al
	jnz	a20_done

	/* System Control Port A. One bit, no handshake. */
	inb	$0x92, %al
	testb	$0x02, %al
	jnz	1f
	orb	$0x02, %al
	andb	$0xFE, %al	/* leave bit 0 alone: it is a fast reset */
	outb	%al, $0x92
1:
	call	a20_enabled
	testb	%al, %al
	jnz	a20_done

	/* And the keyboard controller, which is where this began. */
	call	kbd_wait_in
	movb	$0xAD, %al	/* disable the keyboard */
	outb	%al, $0x64
	call	kbd_wait_in
	movb	$0xD0, %al	/* read the output port */
	outb	%al, $0x64
	call	kbd_wait_out
	inb	$0x60, %al
	pushw	%ax
	call	kbd_wait_in
	movb	$0xD1, %al	/* write the output port */
	outb	%al, $0x64
	call	kbd_wait_in
	popw	%ax
	orb	$0x02, %al
	outb	%al, $0x60
	call	kbd_wait_in
	movb	$0xAE, %al	/* enable the keyboard again */
	outb	%al, $0x64
	call	kbd_wait_in

	call	a20_enabled
	testb	%al, %al
	jnz	a20_done

	/* Every way this machine offers has been tried and the line is still
	 * held low. Said out loud rather than pressed on: continuing means
	 * jumping to a kernel that is not there. */
	movw	$msg_a20, %si
	call	s2_print_asm
	cli
1:	hlt
	jmp	1b

a20_done:

/* --- into long mode ------------------------------------------------------- */

	cli

	/* The real-mode interrupt vector table stops being meaningful the
	 * moment protection is on, and an interrupt taken between here and the
	 * kernel's own IDT would dispatch through whatever the firmware left at
	 * zero. Loading a null-limit IDT makes any such interrupt a fault
	 * rather than a jump into arbitrary memory -- which is the difference
	 * between a machine that stops and one that does something. */
	lidt	idt_null

	lgdt	gdt_descriptor

	/* Physical Address Extension: long mode requires it, and it is what
	 * makes the four-level page tables below the ones the CPU walks. */
	movl	%cr4, %eax
	orl	$(1 << 5), %eax
	movl	%eax, %cr4

	movl	pml4_addr, %eax
	movl	%eax, %cr3

	/* EFER.LME -- long mode enabled, but not yet active: it becomes active
	 * when paging is turned on below. */
	movl	$0xC0000080, %ecx
	rdmsr
	orl	$(1 << 8), %eax
	wrmsr

	/* Protection and paging in one write. Between this instruction and the
	 * far jump the CPU is in compatibility mode with CS still describing a
	 * real-mode segment, which is why nothing may come between them. */
	movl	%cr0, %eax
	orl	$0x80000001, %eax
	movl	%eax, %cr0

	ljmpl	$0x08, $long_entry

/* --- helpers, still in real mode ------------------------------------------ */

/* Is the first megabyte still a mirror of the second?
 *
 * Written rather than assumed: every method above reports success on machines
 * where it did nothing. The test writes two different values to 0x000500 and
 * 0x100500 and looks at whether the first changed. Returns AL non-zero when the
 * line is open.
 */
a20_enabled:
	pushw	%ds
	pushw	%es
	pushw	%di
	pushw	%si

	xorw	%ax, %ax
	movw	%ax, %ds
	movw	$0x0500, %si

	movw	$0xFFFF, %ax
	movw	%ax, %es
	movw	$0x0510, %di		/* 0xFFFF:0x0510 == 0x100500 */

	movb	(%si), %al
	pushw	%ax
	movb	%es:(%di), %al
	pushw	%ax

	movb	$0x00, (%si)
	movb	$0xFF, %es:(%di)

	movb	(%si), %al
	cmpb	$0xFF, %al		/* did the far write reach the near one? */

	popw	%bx
	movb	%bl, %es:(%di)
	popw	%bx
	movb	%bl, (%si)

	movb	$1, %al
	jne	1f
	movb	$0, %al			/* they are the same address: closed */
1:
	popw	%si
	popw	%di
	popw	%es
	popw	%ds
	ret

kbd_wait_in:
	inb	$0x64, %al
	testb	$0x02, %al
	jnz	kbd_wait_in
	ret

kbd_wait_out:
	inb	$0x64, %al
	testb	$0x01, %al
	jz	kbd_wait_out
	ret

/* A message, in real mode, without the C. Only used on the path where the
 * machine is about to stop. */
s2_print_asm:
1:	lodsb
	testb	%al, %al
	jz	2f
	pushaw
	movb	$0x0E, %ah
	movw	$0x0007, %bx
	int	$0x10
	popaw
	pushaw
	movb	%al, %ah
	movw	$0x03FD, %dx
3:	inb	%dx, %al
	testb	$0x20, %al
	jz	3b
	movw	$0x03F8, %dx
	movb	%ah, %al
	outb	%al, %dx
	popaw
	jmp	1b
2:	ret

/* --- and the other side --------------------------------------------------- */

	.code64
long_entry:
	/* Data segments are ignored in 64-bit mode, but their registers still
	 * hold real-mode values and some instructions consult them. Set to the
	 * null-ish data selector so nothing inherits a stale base. */
	movw	$0x10, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	%ax, %fs
	movw	%ax, %gs

	/* A stack well clear of everything: below stage 2 at 0x8000 and the
	 * page tables above it, in memory the kernel's own map will reclaim. */
	movq	$0x00007000, %rsp

	/* RDI is the first argument in the ABI the kernel was compiled for.
	 * The kernel must not be able to tell which loader started it. */
	movl	handoff_addr, %edi

	movl	entry_high, %eax
	shlq	$32, %rax
	movl	entry_low, %ecx
	orq	%rcx, %rax

	jmp	*%rax

/* --- data ----------------------------------------------------------------- */

	.section .data, "aw"
	.align 8

entry_low:	.long 0
entry_high:	.long 0
handoff_addr:	.long 0
pml4_addr:	.long 0

msg_a20:
	.asciz	"E:A20 -- the first megabyte still mirrors itself\r\n"

/* Three entries: null, 64-bit code, data. Long mode ignores the bases and
 * limits, so these are the flag bits and nothing else -- but the entries must
 * exist, because the far jump needs a selector to load. */
	.align 8
gdt:
	.quad	0
	.quad	0x00AF9A000000FFFF	/* code: present, ring 0, exec, L=1 */
	.quad	0x00CF92000000FFFF	/* data: present, ring 0, read-write */
gdt_end:

gdt_descriptor:
	.word	gdt_end - gdt - 1
	.long	gdt

/* Limit zero: any interrupt taken before the kernel installs its own IDT
 * becomes a fault instead of a jump through the firmware's table. */
idt_null:
	.word	0
	.long	0
