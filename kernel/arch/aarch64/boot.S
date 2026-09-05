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
