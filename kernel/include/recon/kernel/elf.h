/* Turning a file into a running program.
 *
 * This is the half of checkpoint 19 that was left when processes got address
 * spaces of their own: the spaces existed and nothing could put a *file* into
 * one. A "user program" was a byte array compiled into the kernel and copied to
 * a fixed address, which is not a program in any sense a person would use the
 * word -- you cannot write one, build one, or replace one without rebuilding
 * the kernel around it.
 *
 * --- Why this is not the bootloader's ELF reader ---
 *
 * `boot/src/main.c` has read ELF since checkpoint 4, and it is right to be a
 * different piece of code, because it is answering a different question. The
 * loader is loading **our own kernel**, whose signature it has already checked;
 * a field it dislikes means the file is corrupt and the machine should stop. A
 * malformed header there is an accident.
 *
 * This one loads **somebody else's program**. Every number in the file is
 * chosen by whoever produced it, every one of them is an input, and a
 * malformed header here is the ordinary case rather than the exceptional one.
 * So the two differ in the way that matters: the bootloader reads the fields it
 * needs, and this refuses everything it has not been given a reason to accept.
 *
 * The specific things it will not do, each of which is a way a loader hands a
 * program the machine:
 *
 *   - A segment that is not entirely inside the file. Computed so that the sum
 *     cannot wrap: a file offset near the top of the range plus a length is how
 *     "inside the file" becomes true of memory that is not the file.
 *   - A segment outside the half of the address space a program owns. The
 *     kernel's half is mapped in every address space, because that is what
 *     makes a system call work, so a program that could name an address in it
 *     could rewrite the kernel.
 *   - Two segments claiming the same page. Whichever were mapped second would
 *     decide the permissions of a page the first is using.
 *   - A segment that is both writable and executable, which is a program
 *     entitled to run its own input.
 *   - An entry point that is not inside an executable segment.
 *
 * It also refuses `ET_DYN`. A position-independent executable has to be
 * relocated, which means processing relocation entries -- a second parser, over
 * a second untrusted table, whose whole job is to write to addresses the file
 * chooses. There is no caller for it yet, and refusing is a message rather than
 * a half-built relocator.
 *
 * --- What it does not do yet ---
 *
 * No dynamic linking, no interpreter (`PT_INTERP` is refused rather than
 * ignored -- a program that asked for one and did not get it would run with its
 * libraries missing), no symbols, no sections. Sections are a linker's
 * business; a loader that reads them is reading a table the program does not
 * need it to read.
 */
#ifndef RECON_KERNEL_ELF_H
#define RECON_KERNEL_ELF_H

#include <recon/kernel/types.h>

struct addrspace;

/* Why a program was refused.
 *
 * Distinct values rather than one failure, and the reason is the same one the
 * FAT32 reader has for its four refusals: a loader that answers "no" to
 * everything passes a test that only asks whether it complained. Each of these
 * is reachable by a deliberately damaged file, and the test requires the right
 * one.
 */
enum elf_result {
	ELF_OK = 0,

	ELF_TOO_SMALL,		/* not even a header's worth of bytes */
	ELF_NOT_AN_ELF,		/* the magic is not there */
	ELF_WRONG_CLASS,	/* not 64-bit, or not little-endian */
	ELF_WRONG_MACHINE,	/* built for a different architecture */
	ELF_NOT_EXECUTABLE,	/* not ET_EXEC -- see the header about ET_DYN */
	ELF_BAD_PHDRS,		/* the program headers are not inside the file */
	ELF_NO_SEGMENTS,	/* nothing to load */
	ELF_TOO_MANY_SEGMENTS,
	ELF_WANTS_INTERPRETER,	/* PT_INTERP, and there is no dynamic linker */
	ELF_SEGMENT_OUTSIDE,	/* its bytes are not inside the file */
	ELF_SEGMENT_TOO_BIG,	/* more memory than a program is given */
	ELF_SEGMENT_NOT_USER,	/* an address that is not the program's to name */
	ELF_SEGMENT_UNALIGNED,	/* offset and address disagree modulo a page */
	ELF_SEGMENT_OVERLAP,	/* two segments claim the same page */
	ELF_WRITABLE_AND_EXEC,	/* refused, always */
	ELF_ENTRY_NOT_IN_CODE,	/* the entry point is not in anything executable */
	ELF_NO_MEMORY,		/* the machine, not the file */
};

/* A sentence, for a person reading a boot log. */
const char *elf_why(enum elf_result r);

/* Loads `image` into `as` and reports where to start.
 *
 * Nothing is mapped until every header has been checked, so a file that is
 * refused leaves the address space exactly as it was found rather than
 * half-populated. That matters because the caller's response to a refusal is to
 * destroy the space, and a space that is half a program is one that could be
 * run by mistake.
 */
enum elf_result elf_load(struct addrspace *as, const void *image, u64 len,
			 u64 *entry_out);

/* The program built alongside the kernel and carried in its image. Real bytes
 * from the real linker -- see core/user_elf.S. */
extern const unsigned char user_elf_image[];
extern const u64 user_elf_image_len;

bool elf_self_test(void);

#endif /* RECON_KERNEL_ELF_H */
