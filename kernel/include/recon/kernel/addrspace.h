/* An address space: what a program can see, as opposed to what it is.
 *
 * Until this existed there was exactly one map, and "a user program" was a
 * byte array mapped at fixed addresses inside it. That worked and it had a
 * consequence the process table had to state out loud: one program could be
 * live at a time, because a second one would be mapped on top of the first.
 *
 * --- What is shared and what is not ---
 *
 * The kernel's half is shared by every address space and every processor: the
 * same memory at the same addresses whoever is running. That is not a
 * convenience, it is what makes a system call work -- the kernel is reached by
 * an instruction that changes privilege, not one that changes maps, so the
 * kernel has to already be mapped in whatever the program was using.
 *
 * The low half is the program's alone. On aarch64 the architecture already
 * splits the two into separate root registers; on x86_64 one table holds both
 * halves, so a new address space copies the kernel's top-level entries. It
 * copies the *entries*, which point at the tables the kernel is already using,
 * so a mapping the kernel makes later appears in every address space at once.
 * Copying deeper would give each process a snapshot that silently stopped
 * matching the kernel.
 *
 * --- Reference counted, and why that is not over-engineering ---
 *
 * A space outlives neither its process nor its threads reliably: threads end
 * in any order, a thread can be running in a space on another processor when
 * the last one here exits, and tearing down page tables underneath a processor
 * that is translating through them is a fault with nothing left to read. The
 * count says how many things still need it, and the tables go when the answer
 * is none.
 */
#ifndef RECON_KERNEL_ADDRSPACE_H
#define RECON_KERNEL_ADDRSPACE_H

#include <recon/kernel/types.h>

/* What *should* be at an address, as opposed to what is.
 *
 * A mapping is memory that exists. A region is a promise that memory will
 * exist if the program touches it -- which is the whole of demand paging, and
 * the reason a stack can be a megabyte without costing a megabyte.
 *
 * A fixed few per space rather than a list: this kernel gives a program a
 * stack and a heap, and a cap that is a visible number is worth more here than
 * a list that can grow to any size a bad loader asks for.
 */
#define AS_REGIONS_MAX 8

struct as_region {
	vaddr_t  start;
	vaddr_t  end;		/* exclusive */
	unsigned flags;		/* what a page faulted in here is mapped with */

	/* What fills a page faulted in here, or null for zeroes.
	 *
	 * This is what makes a mapping *file-backed*: the region is still a
	 * promise that the memory will exist, and now it is also a promise
	 * about what will be in it. Nothing here knows which filesystem the
	 * file came from, which is the whole reason it took a VFS before this
	 * row could be built.
	 *
	 * **Private, not shared.** A write to a file-backed page changes the
	 * page and never the file. Shared mappings -- where a write is a
	 * write to the file, visible to everybody else who mapped it -- need
	 * a page cache that two address spaces can point at, and this kernel
	 * has no such thing yet. Stated rather than discovered, because a
	 * program that expected its changes to be saved would find out by
	 * losing them. */
	struct file *file;
	u64 file_offset;	/* where in the file this region begins */
	u64 file_len;		/* how much is backed; past it is zeroes */
};

struct addrspace {
	/* The physical address of the top-level table. Physical because that
	 * is what the processor's root register takes, on both architectures. */
	paddr_t root;

	/* Threads and processes that still need this. Never zero while
	 * anything can reach it. */
	unsigned refs;

	/* How much of it the program has been given, for the summary. Counted
	 * rather than derived, because deriving it means walking the tables and
	 * a number in a report is not worth a walk. */
	u64 mapped_bytes;

	/* What the program is allowed to touch and does not have yet. */
	struct as_region regions[AS_REGIONS_MAX];
	unsigned region_count;
};

/* A new, empty address space with the kernel's half in place. Null if there is
 * no memory for one -- which is refused rather than worked around, because the
 * alternative is a program running in somebody else's map. */
/* Allocates the one page of zeroes every untouched read points at. */
void addrspace_init(void);

struct addrspace *addrspace_create(void);

/* One more holder, and one fewer. The tables are freed when the last holder
 * lets go, and never before. */
struct addrspace *addrspace_hold(struct addrspace *as);
void addrspace_release(struct addrspace *as);

/* Makes `as` the space this processor translates the low half through. Null
 * means the kernel's own, which is what a kernel thread runs in and what every
 * processor boots into.
 *
 * Cheap when it is already active: switching costs a translation flush, and a
 * scheduler that switches between two threads of the same program would pay it
 * for nothing. */
void addrspace_activate(struct addrspace *as);

/* What this processor is translating through right now, or null for the
 * kernel's own. */
struct addrspace *addrspace_active(void);

/* Maps into a space that is not necessarily the active one. Activating it
 * first is what makes that possible, and it is put back afterwards -- a
 * loader building a program's map must not leave the processor pointing at a
 * half-built one. */
bool addrspace_map(struct addrspace *as, vaddr_t va, paddr_t pa, u64 size,
		   unsigned flags);

/* Promises that `size` bytes at `va` will exist if the program touches them,
 * without mapping anything now. Returns false when the space has no room for
 * another region, which is a refusal rather than a silent merge -- two regions
 * quietly joined into one would give a program the permissions of whichever
 * was written second. */
/* Reserves a range whose pages are filled from a file when they are first
 * touched. Takes a reference to the file, which is held until the address
 * space is torn down -- a mapping that outlived the thing it maps would fault
 * on a file that had been closed.
 *
 * `len` may be shorter than the range: the tail is zeroes, which is what an
 * ELF segment with a .bss needs and is why the two are separate numbers. */
bool addrspace_map_file(struct addrspace *as, vaddr_t va, u64 size,
			unsigned flags, struct file *f, u64 offset,
			u64 len);

bool addrspace_reserve(struct addrspace *as, vaddr_t va, u64 size,
		       unsigned flags);

/* Called from the architecture's fault handler when a user program touches an
 * address it does not have. Returns true if the fault was resolved and the
 * instruction should be retried, false if the address was never the program's
 * to touch -- in which case the caller ends the program, as it always did.
 *
 * `write` distinguishes the two halves of this: a read of untouched memory can
 * be answered with one shared page of zeroes that every such read in the
 * machine points at, and only a write has to be given a page of its own. */
bool vm_fault_user(vaddr_t addr, bool write);

/* The one page of zeroes every untouched readable page points at.
 *
 * Exposed so that eviction can refuse to swap it out. Evicting it would
 * write out the zeroes half the machine is reading and hand one program a
 * private copy while the rest kept pointing at a page that no longer
 * exists. */
paddr_t addrspace_zero_page(void);

void vm_fault_print_summary(void);

unsigned addrspace_count(void);
void addrspace_print_summary(void);
bool addrspace_self_test(void);

#endif /* RECON_KERNEL_ADDRSPACE_H */
