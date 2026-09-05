/* aarch64 entry.
 *
 * The firmware (or QEMU's -kernel loader) drops us here at the load address
 * with x0 holding a pointer to the device tree. There is no long-mode switch
 * to perform -- unlike x86_64, the CPU is already in the addressing mode the
 * kernel wants -- so this file is short.
 *
 * --- The kernel runs high, and this file is where it gets there ---
 *
 * Since checkpoint 10 the image is loaded at 0x40080000 and linked at
 * 0xFFFFFFFF80080000, so every symbol in every other file names an address
 * that does not exist yet when this code starts. Nothing here is linked low,
 * because nothing here needs to be: aarch64 addressing is PC-relative, and
 * `adrp` gives the address a symbol has *right now* rather than the one the
 * linker assigned it. Every address formed before the MMU is on is formed that
 * way, and the difference is the entire trick.
 *
 * `ldr xN, =symbol`, which loads the linked address out of a literal pool, may
 * not be used until the MMU is on. It is the natural thing to write and it is
 * wrong here, which is worth saying out loud because the failure is a jump into
 * unmapped memory with no message.
 */

	.section .text.boot, "ax"
	.global _start

/* The arm64 Image header.
 *
 * aarch64 has no equivalent of x86's Multiboot: what it has instead is a
 * 64-byte header at the front of a flat binary, and every loader that matters
 * -- U-Boot, UEFI stubs, QEMU -- reads it to decide where to place the image
 * and what to pass in. Without it a loader treats the file as bare metal,
 * jumps to it with every register zeroed, and hands over no device tree at
 * all. That is exactly what QEMU did before this header existed, and it is
 * why the first memory map on this architecture came back empty.
 */
_start:
	b	real_start		/* code0: must be a valid instruction */
	.long	0			/* code1: reserved */
	.quad	0x80000			/* text_offset: where in RAM we want to be */
	.quad	__image_size		/* how much to reserve, BSS and stack included */
	.quad	0			/* flags: little-endian, load at text_offset */
	.quad	0			/* reserved */
	.quad	0			/* reserved */
	.quad	0			/* reserved */
	.ascii	"ARM\x64"		/* magic */
	.long	0			/* reserved (PE header offset, if EFI) */

real_start:
	/* The device tree pointer, out of the way of everything below. x19 and
	 * up are callee-saved, and nothing this file calls is C. */
	mov	x19, x0

	/* Which core is this? MPIDR_EL1's low byte is the CPU number within its
	 * cluster. QEMU releases every configured core at the entry point at
	 * once, so without this, four CPUs race to run the same kernel with the
	 * same stack. Secondaries sleep until there is an SMP story to wake
	 * them into. */
	mrs	x1, mpidr_el1
	and	x1, x1, #0xff
	cbnz	x1, secondary_park

	/* A stack, at the address it is loaded at rather than the one it is
	 * linked at. The ABI requires SP to be 16-byte aligned. */
	adrp	x1, __stack_top
	add	x1, x1, #:lo12:__stack_top
	mov	sp, x1

	bl	mmu_early_on

	/* Everything above ran at loaded addresses. Everything below runs at
	 * linked ones, and this is the instruction between them. It has to go
	 * through a register: the distance is far beyond a branch's range. */
	ldr	x1, =high_start
	br	x1

secondary_park:
	wfe
	b	secondary_park

/* --- Where a processor started by PSCI begins ----------------------------
 *
 * Arrives with the MMU off, caches off, at EL1, with x0 holding the context
 * identifier the boot processor passed -- which this kernel uses for the
 * processor's own index.
 *
 * The MMU has to be turned on here too, and on the same tables the boot
 * processor built: this processor's stack is a linked address, written into a
 * table by a processor that could reach it, and this one cannot until it has a
 * map. The real tables replace these a moment later, inside
 * smp_secondary_main.
 */
	.global secondary_entry
secondary_entry:
	mov	x19, x0			/* our index, kept across everything */

	/* Install, do not build. The boot processor built these tables and is
	 * running on them right now, along with every secondary started before
	 * this one -- zeroing them here to rebuild them identically is a window
	 * in which their translations do not exist, and a processor that takes
	 * an instruction fetch in that window is simply gone. It failed about a
	 * quarter of the time on four processors and not at all on two, which
	 * is exactly the shape of a race that a smaller machine hides. */
	bl	mmu_install

	ldr	x1, =secondary_high
	br	x1

/* --- ReconBoot ----------------------------------------------------------
 *
 * Our own UEFI loader's way in. aarch64 needs no mode switch -- unlike x86 it
 * is already where the kernel wants to be -- so the only reason this is a
 * separate entry from _start is the handoff: the loader passes a structure and
 * the firmware passes a device tree, and they are not the same thing.
 *
 * The MMU is already on here, on the firmware's identity map, which is a
 * different situation from the other path and turns out not to matter: the map
 * this kernel installs is also an identity map for the low half, so the
 * instruction after the switch is fetched from the same place either way.
 */
	.global reconboot_entry
reconboot_entry:
	/* x0 = struct reconboot *. Both ABIs agree on this architecture, so
	   there is nothing to reconcile the way there is on x86_64. */
	mov	x19, x0

	adrp	x1, __stack_top
	add	x1, x1, #:lo12:__stack_top
	mov	sp, x1

	bl	mmu_early_on

	ldr	x1, =reconboot_high
	br	x1

/* --- Building a map, before there is anything to build one with ----------
 *
 * Three tables, all of them statically reserved, and one of them serving both
 * halves of the address space:
 *
 *   entry 0   -> the identity map, which is what the switch itself stands on:
 *                the instruction after the one that turns the MMU on is
 *                fetched at the address the processor is already executing.
 *   entry 256 -> the direct map at 0xFFFF800000000000, so that a secondary
 *                processor can reach a device between here and the real
 *                tables.
 *   entry 511 -> the kernel image at 0xFFFFFFFF80000000, which is where we
 *                are going.
 *
 * All three point at 1GB blocks, and the first two point at the *same* second
 * -level table -- a plain identity of the first 512 gigabytes, which is more
 * physical address space than anything this will run on.
 *
 * Memory type is decided by one rule: blocks below the one the kernel is
 * loaded into are Device, and that block and everything above it is Normal. On
 * every machine this has met, peripherals sit below RAM and RAM is where the
 * kernel was put. It is a heuristic and it is temporary -- vm_init replaces
 * all of this with a map built from the device tree -- but a heuristic that is
 * wrong makes a UART unwritable, so it is written down rather than assumed.
 *
 * Split in two on purpose. Building is the boot processor's job and happens
 * exactly once; installing is every processor's job and touches no memory at
 * all. A secondary that rebuilt the tables would be writing over the map the
 * processors already running are using.
 *
 * Clobbers x0-x13 and x22, and preserves x19. Returns with the MMU, the data
 * cache and the instruction cache on, still executing at loaded addresses.
 */
mmu_early_on:
	mov	x22, x30		/* our return address, across no calls */

	/* Where the tables are, right now. */
	adrp	x10, boot_l0
	add	x10, x10, #:lo12:boot_l0
	adrp	x11, boot_l1_id
	add	x11, x11, #:lo12:boot_l1_id
	adrp	x12, boot_l1_hi
	add	x12, x12, #:lo12:boot_l1_hi

	/* Zero all three. Nothing is using them: this runs once, on the boot
	 * processor, before any other processor exists. */
	mov	x0, x10
	mov	x1, #(3 * 4096 / 8)
1:	str	xzr, [x0], #8
	subs	x1, x1, #1
	b.ne	1b

	/* The block of physical memory the kernel was loaded into, rounded down
	 * to a gigabyte -- discovered from the program counter, so that nothing
	 * here depends on the load address the linker script chose. */
	adrp	x9, _start
	lsr	x9, x9, #30
	lsl	x9, x9, #30		/* x9 = kernel's 1GB block, physical */

	/* The identity table: 512 blocks of a gigabyte each. */
	mov	x0, x11			/* where to write */
	mov	x1, xzr			/* physical address of this block */
	mov	x2, #512		/* how many left */
	mov	x13, #1
	lsl	x13, x13, #30		/* one gigabyte, in a register because
					 * ADD's immediate cannot reach it */
2:	cmp	x1, x9
	b.hs	3f
	/* Below the kernel: Device-nGnRnE, never executable at either level. */
	mov	x3, #0x00000401		/* block, AF, AttrIndx=0 (Device) */
	movk	x3, #0x0060, lsl #48	/* UXN | PXN */
	b	4f
3:	/* At or above the kernel: Normal write-back, inner shareable, and
	 * executable by the kernel -- this is where its own code is. */
	mov	x3, #0x00000705		/* block, AF, SH inner, AttrIndx=1 */
	movk	x3, #0x0040, lsl #48	/* UXN, but not PXN */
4:	orr	x3, x3, x1
	str	x3, [x0], #8
	add	x1, x1, x13
	subs	x2, x2, #1
	b.ne	2b

	/* The kernel's own block, in the top-half table. The virtual address is
	 * KERNEL_VMA plus the physical one, and the two indices fall out of it;
	 * computing them rather than writing them down keeps this correct if the
	 * load address ever changes. */
	mov	x4, xzr
	movk	x4, #0x8000, lsl #16
	movk	x4, #0xFFFF, lsl #32
	movk	x4, #0xFFFF, lsl #48	/* x4 = 0xFFFFFFFF80000000 */
	add	x4, x4, x9		/* the block's virtual address */

	lsr	x5, x4, #30
	and	x5, x5, #511		/* second-level index */
	mov	x3, #0x00000705		/* Normal, executable by the kernel */
	movk	x3, #0x0040, lsl #48
	orr	x3, x3, x9
	str	x3, [x12, x5, lsl #3]

	/* Top-level entries. A table descriptor is the next table's address
	 * with the low two bits saying "table". */
	orr	x3, x11, #3
	str	x3, [x10]			/* entry 0:   identity */
	str	x3, [x10, #(256 * 8)]		/* entry 256: the direct map */
	orr	x3, x12, #3
	str	x3, [x10, #(511 * 8)]		/* entry 511: the kernel image */

	/* The tables are written; make sure they are visible to the hardware
	 * that is about to walk them. On the path where the MMU is off these
	 * stores bypassed the caches; on the path where it is on they did not.
	 * A barrier covers both. */
	dsb	sy

	/* Fall through into the install half. Safe with the register save
	 * below: nothing above this point was a call, so x30 still holds the
	 * address mmu_early_on was called from and re-saving it is a no-op. */

/* Points this processor at tables somebody has already built, and turns the MMU
 * on. Writes no memory, so any number of processors may run it at once.
 *
 * Clobbers x0, x6, x7, x8, x10 and x22, and preserves x19. */
mmu_install:
	mov	x22, x30

	adrp	x10, boot_l0
	add	x10, x10, #:lo12:boot_l0

	/* MAIR: attribute 0 is Device-nGnRnE, attribute 1 is Normal write-back.
	 * Must match MAIR_VALUE in vm.c -- these are two statements of one
	 * fact, and vm_init reprograms the register with the same value. */
	mov	x6, #0xFF00
	msr	mair_el1, x6

	/* TCR. T0SZ and T1SZ of 16 give 48-bit address spaces at both ends. TG0
	 * and TG1 both select a 4KB granule and use *different encodings* for
	 * it -- 0b00 for TTBR0 and 0b10 for TTBR1 -- which is a genuine trap in
	 * the architecture. IRGN/ORGN of 0b01 let the table walker use the
	 * caches, so the tables can be written normally. */
	mov	x7, #16			/* T0SZ */
	movk	x7, #0x0010, lsl #16	/* T1SZ = 16 */
	orr	x7, x7, #(1 << 8)	/* IRGN0 */
	orr	x7, x7, #(1 << 10)	/* ORGN0 */
	orr	x7, x7, #(3 << 12)	/* SH0: inner shareable */
	orr	x7, x7, #(1 << 24)	/* IRGN1 */
	orr	x7, x7, #(1 << 26)	/* ORGN1 */
	orr	x7, x7, #(3 << 28)	/* SH1 */
	orr	x7, x7, #(2 << 30)	/* TG1: 4KB, in the other encoding */

	/* IPS, from what this processor says it supports. PARange's encoding is
	 * the same one the field wants, which is one of the architecture's
	 * kinder details. */
	mrs	x8, id_aa64mmfr0_el1
	and	x8, x8, #0xF
	orr	x7, x7, x8, lsl #32
	msr	tcr_el1, x7

	/* Both halves point at the same top-level table; each simply never
	 * looks at the other's entries. */
	msr	ttbr0_el1, x10
	msr	ttbr1_el1, x10
	isb

	/* Anything cached from the map we were handed -- and on the ReconBoot
	 * path we were handed a real one -- is now wrong. */
	tlbi	vmalle1
	dsb	sy
	isb

	/* Read rather than assume: the MMU is already on when the firmware
	 * started us and off when a bare loader did, and one code path serves
	 * both. */
	mrs	x0, sctlr_el1
	orr	x0, x0, #(1 << 0)	/* M: MMU on */
	orr	x0, x0, #(1 << 2)	/* C: data cache on */
	orr	x0, x0, #(1 << 12)	/* I: instruction cache on */
	msr	sctlr_el1, x0
	isb

	mov	x30, x22
	ret

/* --- Linked high, and everything below runs there ----------------------- */

	.section .text

high_start:
	ldr	x1, =__stack_top
	mov	sp, x1

	/* Zero BSS. C code is entitled to assume its statics start at zero, and
	 * on bare metal nobody has arranged that. */
	ldr	x1, =__bss_start
	ldr	x2, =__bss_end
1:	cmp	x1, x2
	b.hs	2f
	str	xzr, [x1], #8
	b	1b
2:
	/* The device tree pointer the firmware passed. It is the only
	 * description of this machine's hardware that exists. */
	ldr	x1, =arch_dtb_pointer
	str	x19, [x1]

	bl	kmain

	/* kmain does not return. If it somehow does, stop rather than execute
	 * whatever bytes follow. */
halt:
	wfi
	b	halt

reconboot_high:
	ldr	x1, =__stack_top
	mov	sp, x1

	/* BSS is not cleared here. The loader zeroed every byte of each segment
	 * with no bytes in the file, which is exactly BSS, and clearing it
	 * again would wipe the handoff pointer stored next. */
	ldr	x1, =reconboot_handoff
	str	x19, [x1]

	bl	kmain
	b	halt

secondary_high:
	/* Our own slot, not a shared word. One word shared between every
	 * processor being started is a race: the boot processor writes the next
	 * one's stack while this one may not have read its own yet. */
	ldr	x1, =secondary_stacks
	lsl	x2, x19, #3
	add	x1, x1, x2
	ldr	x1, [x1]
	mov	sp, x1

	mov	x0, x19
	bl	smp_secondary_main

	/* smp_secondary_main does not return. */
1:	wfi
	b	1b

/* Placed in .data, not .bss: nothing clears BSS on the ReconBoot path, and on
 * the device-tree path BSS is cleared before this would be written. */
	.section .data
	.align 8
	.global reconboot_handoff
reconboot_handoff:
	.quad	0

/* --- ReconBoot header ---------------------------------------------------
 *
 * Must match struct reconboot_kernel_header in boot/include/reconboot.h. The
 * loader finds it by scanning the loaded image for the magic, which is how one
 * kernel binary stays bootable by every protocol it supports.
 *
 * The entry point is stored as a *loaded* address: the loader is running on
 * the firmware's map, where the kernel's linked address does not exist yet.
 */
	.section .reconboot, "a"
	.align 8
	.quad	0x00544f4f424e4352	/* "RCNBOOT" */
	.long	1			/* version */
	.long	24			/* sizeof(header) */
	.quad	reconboot_entry - 0xFFFFFFFF80000000
