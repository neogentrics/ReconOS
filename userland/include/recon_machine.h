/*
 * What the machine is, for a program with no host to ask.
 *
 * Laid out to match the kernel's `struct recon_machine` field for field --
 * `kernel/include/recon/kernel/user.h` says growing it is allowed and
 * reordering it is not, and this is the other side of that promise. Two
 * copies rather than one shared header because the kernel's headers are the
 * kernel's; what crosses the boundary is a *layout*, and a layout is the one
 * thing both sides have to agree on in writing.
 *
 * `size` is passed in and the number of bytes the kernel would have written is
 * returned, so a program built against an older kernel and one built against a
 * newer one both work -- and either can see it was given a short answer.
 */

#ifndef RECON_MACHINE_H
#define RECON_MACHINE_H

#include <recon.h>

struct recon_machine {
	u32 size;		/* what the caller had room for */
	u32 version;		/* 1 */

	u32 processors_found;	/* what the machine has */
	u32 processors_online;	/* what this kernel is using -- not the same */

	u64 memory_bytes;
	u64 memory_free_bytes;

	u32 entropy_bits;	/* zero means keys will be refused */
	u32 page_size;

	char architecture[16];
	char cpu_vendor[16];
	char cpu_model[64];
};

/*
 * Fills `into` and returns the size the kernel has, which may be larger than
 * the size it was given. A caller that gets back more than it asked for has
 * been given a valid prefix by a newer kernel and can say so.
 */
static inline i64 recon_machine_facts(struct recon_machine *into, u64 length)
{
	return RECON_CALL2(SYS_MACHINE, into, length);
}

#endif /* RECON_MACHINE_H */
