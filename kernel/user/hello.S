/* The first ReconOS program that is a *file* rather than a byte array.
 *
 * Everything the kernel has run in user mode until now was a blob of
 * position-independent instructions compiled into the kernel and copied to a
 * fixed address. This is linked -- by the ordinary cross linker, against
 * user.ld -- into a real ELF with real program headers, and the kernel's loader
 * reads those headers to decide what goes where.
 *
 * That distinction is the whole point of the exercise, so this program is
 * written to *fail* if any part of it were skipped:
 *
 *   - it prints from a read-only segment, so a missing R-X mapping is silent
 *     no longer;
 *   - it reads a value out of the writable segment, which only holds that value
 *     if the loader copied the file's bytes rather than zeroing;
 *   - it reads a byte of .bss, which the file does *not* contain, and requires
 *     it to be zero -- so a loader that maps `filesz` and forgets the gap up to
 *     `memsz` is caught, and so is one that leaves whatever the page allocator
 *     last had there;
 *   - it writes to .bss and reads it back, so a segment mapped without write
 *     permission faults rather than passing.
 *
 * Every failure exits with its own number. A test that only checks for zero
 * also passes when the program never ran at all -- and the kernel cannot tell
 * "exited 0" from "never reached its exit call" unless the numbers differ.
 *
 * ONE FILE, TWO ARCHITECTURES. Assembled through the C preprocessor, so the
 * program is described once and the instructions twice. The syscall numbers and
 * the register convention are the same on both; only the instruction that
 * traps and the names of the registers differ.
 */

#define SYS_EXIT   0
#define SYS_WRITE  1

/* 55 rather than 0 for success, and this is not fussiness. The kernel reads
 * this number out of a field that is zero before the program runs and zero if
 * the program never reached its exit call -- so a test that treats zero as
 * success passes in both of the cases it exists to catch. */
#define OK              55
#define BAD_DATA        2	/* the writable segment did not arrive */
#define BAD_BSS_DIRTY   3	/* .bss was not zero */
#define BAD_BSS_WRITE   4	/* .bss did not hold what was written to it */

/* --------------------------------------------------------------------------
 * The read-only half: instructions, and the message.
 */
	.section .text
	.balign 16
	.global _start

#if defined(__x86_64__)

_start:
	/* write(1, msg, len). The length comes from the linker rather than
	 * being counted here, so the two cannot disagree. */
	movq	$1, %rdi
	leaq	msg(%rip), %rsi
	movq	$(msg_end - msg), %rdx
	movq	$SYS_WRITE, %rax
	syscall

	/* The writable segment: a value the *file* carries. If the loader
	 * zeroed this page instead of copying it, this is 0. */
	movl	from_file(%rip), %eax
	cmpl	$0x5245434F, %eax	/* 'RECO' */
	jne	.Lbad_data

	/* .bss: not in the file at all. It must be zero. */
	movzbl	scratch(%rip), %eax
	testl	%eax, %eax
	jne	.Lbad_bss_dirty

	/* And it must be writable. A page mapped read-only faults here rather
	 * than reaching the comparison, which is the correct outcome and is
	 * reported by the kernel rather than by this program. */
	movb	$0x5A, scratch(%rip)
	movzbl	scratch(%rip), %eax
	cmpl	$0x5A, %eax
	jne	.Lbad_bss_write

	movq	$OK, %rdi
	jmp	.Lexit
.Lbad_data:
	movq	$BAD_DATA, %rdi
	jmp	.Lexit
.Lbad_bss_dirty:
	movq	$BAD_BSS_DIRTY, %rdi
	jmp	.Lexit
.Lbad_bss_write:
	movq	$BAD_BSS_WRITE, %rdi
.Lexit:
	movq	$SYS_EXIT, %rax
	syscall

	/* Not reached. Spinning beats falling off the end of the segment into
	 * whatever follows it. */
.Lhang:
	jmp	.Lhang

#elif defined(__aarch64__)

_start:
	mov	x0, #1
	adrp	x1, msg
	add	x1, x1, #:lo12:msg
	mov	x2, #(msg_end - msg)
	mov	x8, #SYS_WRITE
	svc	#0

	adrp	x9, from_file
	add	x9, x9, #:lo12:from_file
	ldr	w10, [x9]
	movz	w11, #0x434F
	movk	w11, #0x5245, lsl #16	/* 'RECO' */
	cmp	w10, w11
	b.ne	.Lbad_data

	adrp	x9, scratch
	add	x9, x9, #:lo12:scratch
	ldrb	w10, [x9]
	cbnz	w10, .Lbad_bss_dirty

	mov	w10, #0x5A
	strb	w10, [x9]
	ldrb	w11, [x9]
	cmp	w11, w10
	b.ne	.Lbad_bss_write

	mov	x0, #OK
	b	.Lexit
.Lbad_data:
	mov	x0, #BAD_DATA
	b	.Lexit
.Lbad_bss_dirty:
	mov	x0, #BAD_BSS_DIRTY
	b	.Lexit
.Lbad_bss_write:
	mov	x0, #BAD_BSS_WRITE
.Lexit:
	mov	x8, #SYS_EXIT
	svc	#0

.Lhang:
	b	.Lhang

#else
#error "the ELF test program has no body for this architecture"
#endif

	.section .rodata
	.balign 8
msg:
	.ascii	"hello from a program the kernel loaded out of an ELF\n"
msg_end:

/* --------------------------------------------------------------------------
 * The writable half. Two separate things on purpose: `from_file` has bytes in
 * the file and `scratch` does not, so one segment exercises both halves of a
 * program header -- `filesz` and the gap between it and `memsz`.
 */
	.section .data
	.balign 8
from_file:
	.long	0x5245434F		/* 'RECO', little-endian */

	.section .bss
	.balign 8
scratch:
	.skip	64
