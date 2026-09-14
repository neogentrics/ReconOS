/* The same thing on aarch64: the copy, done from somewhere the kernel will not
 * land on.
 *
 * See reloc_x86_64.S for why this exists at all. The constraints are identical
 * and so is the shape -- no absolute addresses, no relocations, no calls, and
 * nothing referenced that the linker would have fixed up, because this is
 * copied byte for byte to an address chosen at run time.
 *
 * The arguments arrive in the AAPCS registers the C caller uses:
 *
 *   x0  the copy table: {dest, src, len} triples, ending with len == 0
 *   x1  the handoff, which the kernel wants in x0
 *   x2  where to jump when the copying is done, which the kernel wants in x16
 */
	.section .text
	.globl	reloc_trampoline
	.globl	reloc_trampoline_end

	.balign 16
reloc_trampoline:
	mov	x9,  x0			/* the table */
	mov	x10, x1			/* the handoff */
	mov	x16, x2			/* the entry point */

1:	ldr	x12, [x9, #16]		/* len */
	cbz	x12, 3f

	ldr	x13, [x9]		/* dest */
	ldr	x14, [x9, #8]		/* src */

	/* A zero source means fill rather than copy. A segment's .bss tail is
	 * zeroed at the destination, which is the memory that might be this
	 * loader -- so it waits for the same reason the copy does. */
	cbnz	x14, 2f

4:	strb	wzr, [x13], #1
	subs	x12, x12, #1
	b.ne	4b
	b	5f

2:

	/* A byte at a time. This runs once, on at most a couple of megabytes,
	 * with the machine otherwise idle -- and a byte loop cannot get the
	 * alignment or the tail wrong, which a wider one can and which would
	 * show up as a kernel that is almost correct. */
6:	ldrb	w15, [x14], #1
	strb	w15, [x13], #1
	subs	x12, x12, #1
	b.ne	6b

5:	add	x9, x9, #24		/* the next triple */
	b	1b

3:	/* The caches, before anything tries to execute what was just written.
	 *
	 * These bytes arrived through the data side and the instruction side
	 * does not know about them. On this architecture that is not a
	 * theoretical concern the way it is on x86: the two are not coherent,
	 * and jumping to freshly written memory without this runs whatever the
	 * instruction cache happened to be holding. */
	dsb	sy
	ic	ialluis
	dsb	sy
	isb

	/* The handoff first, then everything else cleared here rather than by
	 * the caller -- which can no longer do it, having set three registers
	 * to reach this code. The loader's reasoning is unchanged: a leftover a
	 * kernel reads by accident works on one firmware and fails on the next.
	 *
	 * **x16 keeps the entry point until the branch uses it, and x16
	 * specifically.** Nothing can clear the register it is branching
	 * through, so one register has to arrive dirty -- and which one is the
	 * kernel's to say. It names x0, x1 and x16 in boot.c and checks the
	 * other twenty-eight. This trampoline used x11 at first and the kernel
	 * reported "x11 arrived holding something" and failed the handoff,
	 * which is exactly what that check is for. */
	mov	x0, x10
	mov	x1, #0
	mov	x2, #0
	mov	x3, #0
	mov	x4, #0
	mov	x5, #0
	mov	x6, #0
	mov	x7, #0
	mov	x8, #0
	mov	x9, #0
	mov	x10, #0
	mov	x11, #0
	mov	x12, #0
	mov	x13, #0
	mov	x14, #0
	mov	x15, #0
	mov	x17, #0
	mov	x18, #0
	mov	x19, #0
	mov	x20, #0
	mov	x21, #0
	mov	x22, #0
	mov	x23, #0
	mov	x24, #0
	mov	x25, #0
	mov	x26, #0
	mov	x27, #0
	mov	x28, #0
	mov	x29, #0
	mov	x30, #0
	br	x16

reloc_trampoline_end:
