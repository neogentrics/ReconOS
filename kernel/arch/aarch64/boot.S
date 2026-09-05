/* aarch64 entry.
 *
 * The firmware (or QEMU's -kernel loader) drops us here at the load address
 * with the MMU off, caches off, and x0 holding a pointer to the device tree.
 * There is no long-mode switch to perform -- unlike x86_64, the CPU is already
 * in the addressing mode the kernel wants -- so this file is short.
 *
 * Everything a boot file must do and no more: silence the cores that are not
 * the boot core, give the boot core a stack, clear BSS because nothing else
 * will, and hand the device tree pointer to C.
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
 *
 * With it, the handoff is the documented one: the image is loaded at the base
 * of RAM plus text_offset, and x0 holds the device tree.
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
	/* Which core is this? MPIDR_EL1's low byte is the CPU number within its
	 * cluster. QEMU releases every configured core at the entry point at
	 * once, so without this, four CPUs race to run the same kernel with the
	 * same stack. Secondaries sleep until there is an SMP story to wake
	 * them into. */
	mrs	x1, mpidr_el1
	and	x1, x1, #0xff
	cbnz	x1, secondary_park

	/* Stack. It grows down from the top of the reserved region, and the
	 * ABI requires SP to be 16-byte aligned. */
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

	/* x0 still holds the device tree pointer the firmware passed. Keep it
	 * somewhere C can find it later; it is the only description of this
	 * machine's hardware that exists. */
	ldr	x1, =arch_dtb_pointer
	str	x0, [x1]

	bl	kmain

	/* kmain does not return. If it somehow does, stop rather than execute
	 * whatever bytes follow. */
halt:
	wfi
	b	halt

secondary_park:
	wfe
	b	secondary_park

/* --- Where a processor started by PSCI begins ----------------------------
 *
 * Arrives with the MMU off, caches off, at EL1, with x0 holding the context
 * identifier the boot processor passed -- which this kernel uses for the
 * processor's own index.
 *
 * Almost nothing may happen here. There is no stack yet, so no C, and the only
 * memory that can be trusted is what the boot processor cleaned out of its
 * cache before starting us. That is exactly one word: the stack pointer.
 */
	.global secondary_entry
secondary_entry:
	mov	x19, x0			/* our index, kept across the loads below */

	/* Our own slot, not a shared word. One word shared between every
	   processor being started is a race: the boot processor writes the next
	   one's stack while this one may not have read its own yet. */
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

/* --- ReconBoot ----------------------------------------------------------
 *
 * Our own UEFI loader's way in. aarch64 needs no mode switch -- unlike x86 it
 * is already where the kernel wants to be -- so the only reason this is a
 * separate entry from _start is the handoff: the loader passes a structure and
 * the firmware passes a device tree, and they are not the same thing.
 *
 * BSS is not cleared here. The loader zeroed every byte of each segment with no
 * bytes in the file, which is exactly BSS, and clearing it again would wipe the
 * handoff pointer stored below.
 */
	.global reconboot_entry
reconboot_entry:
	/* x0 = struct reconboot *. Both ABIs agree on this architecture, so
	   there is nothing to reconcile the way there is on x86_64. */
	ldr	x1, =__stack_top
	mov	sp, x1

	ldr	x1, =reconboot_handoff
	str	x0, [x1]

	bl	kmain
	b	halt

/* Placed in .data, not .bss: nothing clears BSS on this path, and on the
 * device-tree path BSS is cleared before this would be written. */
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
 */
	.section .reconboot, "a"
	.align 8
	.quad	0x00544f4f424e4352	/* "RCNBOOT" */
	.long	1			/* version */
	.long	24			/* sizeof(header) */
	.quad	reconboot_entry
