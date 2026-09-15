/*
 * Where the allocator's memory comes from on ReconOS.
 *
 * `malloc.c` asks a `struct recon_memory_source` for ranges and knows nothing
 * about system calls. This is the source a program on this kernel installs,
 * and it is the only file in the library that has an opinion about how a
 * program gets memory from the machine.
 *
 * --- What the kernel still owes this file ---
 *
 * `SYS_MAP` today puts **a file** into a program's address space, and the only
 * file that answers is `/dev/fb0`. Asking it for a range with no file behind
 * it is what `docs/KERNEL-WANTS.md` proposes -- *"SYS_MAP with no file -- an
 * fd of -1, or its own number -- returning such a range, and a companion that
 * releases one, is two calls over machinery that exists"* -- and the machinery
 * really does exist: every process already gets a reserved, demand-paged stack.
 *
 * So this asks for fd -1 **now**. On today's kernel `fd_get` finds nothing and
 * the call is refused with EBADF, so `malloc` answers NULL and
 * `recon_malloc_stats().refusals` counts it. The day the kernel treats -1 as
 * "no file, just memory", this works with nothing here rebuilt and nothing
 * relinked.
 *
 * That is deliberate, and it is the reason no new system call number is taken
 * here. Two sessions build this kernel; a number claimed in advance by the
 * half that does not own `core/` is a number claimed twice, which has happened
 * and is written down in `docs/BUGS.md`.
 *
 * --- Giving it back ---
 *
 * There is no call that releases a range, so `give_back` says so. That is not
 * a stub standing in for something: `malloc.c` treats "cannot give back" as a
 * supported configuration and keeps the region for reuse, and the suite runs
 * every scenario that way as well as the other. When a release call exists,
 * `give_back` is the one function that changes.
 */

#include "internal.h"

#include "../include/recon.h"

/* The fd that means "no file".
 *
 * -1 rather than a constant of our own, because that is the spelling
 * KERNEL-WANTS proposes and the spelling every other system uses for the same
 * idea. If the kernel picks its own number instead, this line is the change.
 */
#define NO_FILE ((u64)-1)

static void *take(size_t bytes)
{
	i64 at = recon_map((int)NO_FILE, (u64)bytes);

	if (at < 0)
		return 0;

	return (void *)(unsigned long)at;
}

static int give_back(void *at, size_t bytes)
{
	(void)at;
	(void)bytes;

	/* Not "it failed" -- *there is no call*. The allocator asks once per
	 * region and carries on without releasing when the answer is no, so
	 * this costs a machine nothing but memory it will reuse. */
	return -1;
}

static const struct recon_memory_source recon_source = {
	take,
	give_back,
};

/* The strong definition of the name `malloc.c` declares weakly.
 *
 * Linking this file into a program is what gives that program an allocator;
 * leaving it out leaves `malloc` answering NULL rather than leaving a program
 * that never allocates with an undefined symbol. The allocator asks for it
 * once per region, not once per call.
 *
 * A program that wants a different source -- the suite does -- calls
 * `recon_memory_from` before its first allocation and never reaches here.
 */
void recon_memory_default(void)
{
	recon_memory_from(&recon_source);
}
