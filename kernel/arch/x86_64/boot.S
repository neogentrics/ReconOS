/* x86_64 entry.
 *
 * Unlike aarch64, an x86 CPU does not start in the mode the kernel wants. It
 * starts in 32-bit protected mode with paging off, and getting to 64-bit long
 * mode is something the kernel has to do for itself: build page tables, turn
 * on PAE, set the long-mode-enable bit, turn on paging, then far-jump through
 * a 64-bit code segment. That switch is the whole of this file, and it is why
 * this architecture's boot code is four times the length of ARM's.
 *
 * Three boot protocols land here:
 *
 *   Multiboot2 -- what GRUB uses, and therefore how this boots from an ISO or
 *   from real hardware. Hands us a pointer in EBX and a magic number in EAX.
 *
 *   PVH -- an ELF note that QEMU's -kernel understands directly, so a build
 *   can be booted in a second without producing a disc image first. Hands us
 *   a different structure, also in EBX.
 *
 *   ReconBoot -- our own UEFI loader, which arrives already in long mode.
 *
 * --- The kernel runs high, and this file is where it gets there ---
 *
 * The image is loaded at 1MB and linked at 0xFFFFFFFF80000000, so every
 * address in every other file is a virtual address that does not exist yet
 * when this file starts running. Only .boot -- this trampoline, the descriptor
 * table it loads, the tables it builds -- is linked where it is loaded.
 *
 * So the shape of every path below is the same: do the minimum at low
 * addresses, build a map in which the kernel exists at both its loaded and its
 * linked address, and jump to the linked one. After that jump nothing is low
 * again, and the whole lower half of the address space belongs to user
 * programs.
 */

#define MULTIBOOT2_MAGIC        0xE85250D6
#define MULTIBOOT2_ARCH_I386    0

/* --- Multiboot2 header ------------------------------------------------- */

	.section .multiboot, "a"
	.align 8
multiboot_header:
	.long	MULTIBOOT2_MAGIC
	.long	MULTIBOOT2_ARCH_I386
	.long	multiboot_header_end - multiboot_header
	.long	-(MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH_I386 + \
		  (multiboot_header_end - multiboot_header))

	/* End tag: type 0, flags 0, size 8. */
	.align 8
	.short	0
	.short	0
	.long	8
multiboot_header_end:

/* --- PVH entry note ---------------------------------------------------- */

/* XEN_ELFNOTE_PHYS32_ENTRY (18). QEMU reads this out of the ELF and jumps
 * straight to the address it names, which is what makes `make run` fast. The
 * address must be a loaded one: this is a 32-bit field, and the CPU is in
 * 32-bit mode when it arrives. */
	.section .note.Xen, "a"
	.align 4
	.long	4			/* namesz: "Xen\0" */
	.long	4			/* descsz: one 32-bit address */
	.long	18			/* type */
	.asciz	"Xen"
	.align 4
	.long	pvh_entry
	.align 4

/* --- ReconBoot header ---------------------------------------------------
 *
 * Our own loader's way in. It cannot use the ELF entry point, because that
 * belongs to the 32-bit trampoline below and UEFI has already put the machine
 * in 64-bit mode -- arriving there would execute 32-bit code as 64-bit and
 * fault immediately. So the image names a second entry, the loader finds this
 * header by scanning for the magic, and one kernel binary stays bootable by
 * every protocol it supports.
 *
 * reconboot_entry is in .boot, so this is a loaded address -- which is what
 * the loader needs, because it is running on the firmware's identity map and
 * nothing has mapped the kernel's linked address yet.
 *
 * Must match struct reconboot_kernel_header in boot/include/reconboot.h.
 */
	.section .reconboot, "a"
	.align 8
	.quad	0x00544f4f424e4352	/* "RCNBOOT\0" */
	.long	1			/* version */
	.long	24			/* sizeof(header) */
	.quad	reconboot_entry

/* --- 32-bit trampoline ------------------------------------------------- */

	.section .text.boot, "ax"
	.code32

	.global _start
_start:					/* Multiboot2 lands here */
	movl	$1, %esi		/* protocol tag: 1 = multiboot2 */
	jmp	common_entry

	.global pvh_entry
pvh_entry:				/* QEMU's PVH loader lands here */
	movl	$2, %esi		/* protocol tag: 2 = PVH */
	jmp	common_entry

common_entry:
	cli
	cld

	/* Record who booted us and where their data is, before any of this is
	 * clobbered. Into low scratch rather than into the C variables that
	 * will hold them: those are linked high, and high does not exist yet.
	 * The high code copies them across once it can reach both. */
	movl	%esi, boot_scratch_protocol
	movl	%ebx, boot_scratch_info
	movl	%eax, boot_scratch_magic

	/* A stack, so the rest of this can use one. Its own, low, and small:
	 * the real one is linked high and is 64KB. */
	movl	$boot_stack_top, %esp

	/* Zero the page tables. They are outside every BSS marker on purpose:
	 * BSS is cleared after the switch, and clearing the tables the CPU is
	 * walking would end the boot immediately. */
	movl	$__pagetables_start, %edi
	xorl	%eax, %eax
	movl	$__pagetables_size, %ecx
	shrl	$2, %ecx		/* bytes -> 32-bit words */
	rep stosl

	/* --- The map, which has to contain the kernel twice ---------------
	 *
	 * Once at the address the CPU is executing from right now, because
	 * paging takes effect on the very next instruction fetch and that fetch
	 * must still find this code. Once at the address the kernel is linked
	 * at, because that is where we are going.
	 *
	 * Both are the same 512 two-megabyte pages -- the first gigabyte of
	 * physical memory -- reached through two different chains. Only the
	 * chains differ, which is why there is one page directory and two
	 * pointer tables above it.
	 *
	 * 0xFFFFFFFF80000000 picks PML4 entry 511 and PDPT entry 510. Those two
	 * numbers are the whole of the arithmetic, and they are the same two on
	 * aarch64, which uses the same address for the same reason. */
	movl	$boot_pdpt, %eax
	orl	$0x03, %eax		/* present, writable */
	movl	%eax, boot_pml4			/* entry 0: identity */

	movl	$boot_pdpt_hi, %eax
	orl	$0x03, %eax
	movl	%eax, boot_pml4 + 511 * 8	/* entry 511: the top half */

	movl	$boot_pd, %eax
	orl	$0x03, %eax
	movl	%eax, boot_pdpt			/* identity -> the directory */
	movl	%eax, boot_pdpt_hi + 510 * 8	/* high -> the same directory */

	/* 512 two-megabyte pages covering the first gigabyte. Bits: 0 present,
	 * 1 writable, 7 page-size. */
	movl	$boot_pd, %edi
	movl	$0x83, %eax
	movl	$512, %ecx
1:	movl	%eax, (%edi)
	movl	$0, 4(%edi)
	addl	$0x200000, %eax
	addl	$8, %edi
	loop	1b

	/* PAE. Long mode requires it; there is no 64-bit paging without it. */
	movl	%cr4, %eax
	orl	$(1 << 5), %eax
	movl	%eax, %cr4

	movl	$boot_pml4, %eax
	movl	%eax, %cr3

	/* EFER.LME -- "long mode enable". The CPU is not in long mode yet; this
	 * only says that turning on paging should enter it. */
	movl	$0xC0000080, %ecx
	rdmsr
	orl	$(1 << 8), %eax
	wrmsr

	/* Paging on. The CPU enters long mode at this instruction, but is still
	 * executing through a 32-bit code segment (compatibility mode). */
	movl	%cr0, %eax
	orl	$0x80000001, %eax	/* PG | PE */
	movl	%eax, %cr0

	/* The far jump through a 64-bit code descriptor is what actually leaves
	 * 32-bit code behind. The descriptor register is loaded with the
	 * six-byte form, and with the table's *loaded* address -- the table
	 * itself is linked high, and the linker computed the difference. */
	lgdt	gdt64_descriptor32
	ljmp	$0x08, $long_mode_entry

/* --- 64-bit, still low -------------------------------------------------- */

	.code64
long_mode_entry:
	movw	$0x10, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	%ax, %fs
	movw	%ax, %gs

	/* An absolute jump through a register: the only way to reach an address
	 * this far away, since a direct jump's displacement is 32 bits and the
	 * distance from here to there is not. This instruction is the move to
	 * the higher half; everything after it runs at linked addresses. */
	movabsq	$high_entry, %rax
	jmp	*%rax

/* --- ReconBoot entry ----------------------------------------------------
 *
 * Arrived at from our own UEFI loader, already in 64-bit long mode on the
 * identity map the firmware built. There is no mode switch to perform -- which
 * is most of the argument for writing the loader before the drivers -- but
 * there is still the move to the higher half, and the firmware's map does not
 * contain it.
 *
 * Rather than building a whole address space to replace one that already
 * works, this adds a single entry to the one the firmware is using: entry 511
 * of its top-level table, which no firmware uses and which is exactly where
 * 0xFFFFFFFF80000000 looks. The firmware's map of everything else -- which
 * covers all of memory, including wherever it put the handoff structure --
 * stays exactly as it was.
 *
 * RDI holds the handoff structure, because the loader declares this call
 * System V even though the loader itself is compiled for the Microsoft ABI.
 */
	.global reconboot_entry
reconboot_entry:
	cli
	cld

	movq	%rdi, %r15		/* the handoff, kept across all of this */

	/* The high chain: a pointer table and a directory, both of which the
	 * loader did not zero -- .boot.bss carries no bytes in the file, so
	 * there was nothing for it to copy or clear. */
	movl	$boot_pdpt_hi, %edi
	xorl	%eax, %eax
	movl	$512, %ecx
	rep stosq

	movl	$boot_pd, %edi
	movq	$0x83, %rax
	movl	$512, %ecx
1:	movq	%rax, (%rdi)
	addq	$0x200000, %rax
	addq	$8, %rdi
	loop	1b

	movl	$boot_pd, %eax
	orl	$0x03, %eax
	movl	%eax, boot_pdpt_hi + 510 * 8

	/* Entry 511 of whatever table CR3 names. The low twelve bits of CR3 are
	 * flags, not address, and masking them off matters: with process-context
	 * identifiers in use they are not zero.
	 *
	 * CR0.WP has to come off around the write, and finding that out cost a
	 * boot: OVMF maps its own page tables read-only, and the write-protect
	 * bit is what makes a read-only page read-only *to the kernel as well*.
	 * Without it the store faults with error code 3 -- present, write --
	 * which reads like a bug in the address rather than in the permission,
	 * and the address was perfectly correct.
	 *
	 * Turned back on immediately. It is off for four instructions, and
	 * leaving it off would quietly make every read-only kernel mapping
	 * writable for the rest of the boot. */
	movq	%cr0, %r14
	movq	%r14, %rdx
	andq	$~(1 << 16), %rdx
	movq	%rdx, %cr0

	movq	%cr3, %rax
	andq	$-4096, %rax
	movl	$boot_pdpt_hi, %edx
	orl	$0x03, %edx
	movq	%rdx, 511 * 8(%rax)

	movq	%r14, %cr0

	/* Nothing has walked entry 511 yet, but the processor is allowed to
	 * have cached its absence. Reloading CR3 with itself throws that away. */
	movq	%cr3, %rax
	movq	%rax, %cr3

	movabsq	$reconboot_high, %rax
	jmp	*%rax

/* --- Linked high, and everything below runs there ----------------------- */

	.section .text

high_entry:
	/* The real stack, the linked one. An immediate this large has to go
	 * through a register. */
	movabsq	$__stack_top, %rax
	movq	%rax, %rsp
	xorq	%rbp, %rbp

	/* The descriptor table again, now by its linked address and in the
	 * ten-byte form 64-bit mode uses. The contents are identical; what
	 * changes is that the register no longer points into memory that is
	 * about to stop being mapped. vm_init replaces the address space and
	 * the low half goes with it, and a GDTR left pointing there would fault
	 * on the next segment load with nothing to say why. */
	lgdt	gdt64_descriptor(%rip)

	/* Clear BSS. Now, and not before, because everything the trampoline
	 * needed to survive lives outside it. */
	movabsq	$__bss_start, %rdi
	movabsq	$__bss_end, %rcx
	subq	%rdi, %rcx
	xorq	%rax, %rax
	rep stosb

	/* Carry the trampoline's three words across from low memory into the
	 * variables C reads. This is the last instruction that touches a low
	 * address, and it has to happen before vm_init unmaps them. */
	movl	boot_scratch_protocol, %eax
	movl	%eax, boot_protocol(%rip)
	movl	boot_scratch_info, %eax
	movl	%eax, boot_info_phys(%rip)
	movl	boot_scratch_magic, %eax
	movl	%eax, boot_magic(%rip)

	call	kmain

	.global halt
halt:
	cli
	hlt
	jmp	halt

reconboot_high:
	movabsq	$__stack_top, %rax
	movq	%rax, %rsp
	xorq	%rbp, %rbp

	/* The firmware's descriptor table is at a low address on a map we are
	 * about to replace. Ours is in the image. */
	lgdt	gdt64_descriptor(%rip)

	/* BSS is NOT cleared here. The loader zeroed every byte of each segment
	 * that has no bytes in the file, which is exactly BSS, and clearing it
	 * again would wipe the handoff pointer stored next. */
	movq	%r15, reconboot_handoff(%rip)
	movl	$3, boot_protocol(%rip)

	call	kmain
	jmp	halt

/* --- Descriptor table ---------------------------------------------------
 *
 * One table, two ways of naming it. The trampoline is in 32-bit mode and can
 * only load the six-byte form with a 32-bit base, and it needs the loaded
 * address; the kernel proper loads the ten-byte form with the linked one.
 */
	.section .rodata
	.align 16
	.global gdt64
gdt64:
	.quad	0				/* null */
	.quad	0x00AF9A000000FFFF		/* code: exec/read, L=1 */
	.quad	0x00CF92000000FFFF		/* data: read/write */
gdt64_end:

gdt64_descriptor:
	.word	gdt64_end - gdt64 - 1
	.quad	gdt64

	.section .rodata.boot, "a"
	.align 8
gdt64_descriptor32:
	.word	gdt64_end - gdt64 - 1
	.long	gdt64_phys		/* computed by the linker script */

/* --- State handed over by the bootloader ------------------------------- */

/* boot_scratch_protocol, boot_scratch_info and boot_scratch_magic are reserved
 * by the linker script rather than declared here: they belong in memory that
 * carries no bytes in the file, so that .boot can stay read-and-execute. The
 * trampoline writes them and high_entry reads them once. */

	.section .data
	.align 8
	.global boot_protocol
boot_protocol:
	.long	0
	.global boot_info_phys
boot_info_phys:
	.long	0
	.global boot_magic
boot_magic:
	.long	0

	/* In .data rather than .bss on purpose: the ReconBoot path stores this
	 * and never clears BSS, and the other two paths clear BSS after the
	 * trampoline has already run. Either way, nothing zeroes it afterwards. */
	.align 8
	.global reconboot_handoff
reconboot_handoff:
	.quad	0
