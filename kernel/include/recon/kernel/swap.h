/* Somewhere to put a page that is not memory.
 *
 * The backing store half of swap, and deliberately only that half. Evicting a
 * page is two separate problems -- *where does it go* and *which one goes* --
 * and they fail in different ways, are tested by different means, and one of
 * them is a measurement question this kernel cannot yet answer. Building them
 * together would mean a policy chosen against a store nobody had exercised.
 *
 * --- Why a partition and not a file on ReconFS ---
 *
 * This is the decision this file exists to record, because it is not obvious and
 * the obvious answer is wrong.
 *
 * ReconFS is copy-on-write with one atomic commit. Writing a page out through it
 * means **allocating**: a new block for the data, blocks for the rewritten tree
 * above it, and heap for the transaction that commits them. And swap runs
 * *because memory is short*. So the path taken to free memory would need memory
 * to walk down -- which is not a slow path, it is a deadlock, and one that
 * appears only on a machine actually under pressure. That is the single machine
 * where it cannot be afforded.
 *
 * A swap partition is a flat array of blocks. Page N goes to slot N. **No
 * allocation, no metadata, no transaction, no checksum tree** -- nothing on the
 * write path can need memory, which is the entire requirement.
 *
 * The middle option is worth naming and rejecting: Linux swap *files* work by
 * pre-allocating the file, resolving its block map once, and then writing
 * straight to those blocks, bypassing the filesystem. That works because ext4
 * blocks stay where they are. Under copy-on-write they do not -- a written block
 * moves -- so the extents would have to be pinned and the filesystem forbidden
 * from relocating them. At which point it is a partition wearing a file's
 * clothes: the complexity of both and the benefit of neither.
 *
 * The cost, stated because it is real: the size is fixed at install and changing
 * it means repartitioning. That is the ordinary limitation, and the alternative
 * buys flexibility with a deadlock.
 *
 * --- What is not here ---
 *
 * No eviction policy, no page-table involvement, and nothing that decides *when*
 * to swap. This layer answers "put this page somewhere and give me a token" and
 * "give me back the page this token names", and nothing else. The half that
 * chooses victims needs to know which pages are cold, and nothing in this kernel
 * measures that yet -- so choosing a policy now would be choosing it from
 * nothing, which is how the two faults of 10 September happened.
 */
#ifndef RECON_KERNEL_SWAP_H
#define RECON_KERNEL_SWAP_H

#include <recon/kernel/types.h>

struct block_device;

/* A token naming one page in the store. Zero is never a valid slot, so a zeroed
 * field is "not swapped" without a second flag to disagree with it -- the same
 * reason a process identifier is never zero. */
typedef u32 swap_slot_t;

#define SWAP_NONE ((swap_slot_t)0)

/* Takes a device and prepares it as swap, claiming it for raw writes.
 *
 * **It does not check whether the device carries anything.** That is not an
 * omission: this writes over the whole device from the first eviction, and no
 * inspection of the blocks can answer "did you mean this one". Choosing the
 * device is the caller's decision and it is made once, out loud -- from a
 * partition the installer marked, or from the command line until it marks one.
 *
 * Returns false and says why on the console. A machine with no swap device is
 * ordinary and is not a failure. */
bool swap_attach(struct block_device *dev);

/* Whether there is one, for callers that must behave differently rather than
 * fail. */
bool swap_present(void);

/* Writes a page and returns the token for it, or SWAP_NONE if the store is full
 * or the write failed.
 *
 * `page` is a kernel-mapped address of one whole page. Nothing here allocates:
 * that is the property the whole design exists to preserve, and a change that
 * introduces an allocation on this path has broken it whether or not the tests
 * still pass. */
swap_slot_t swap_write_page(const void *page);

/* Reads a page back. The slot stays allocated -- a page read in is not
 * necessarily a page that will not be evicted again, and freeing it here would
 * mean the caller could not retry a failed fault. */
bool swap_read_page(swap_slot_t slot, void *page);

/* Gives a slot back. Idempotent for SWAP_NONE so that a caller tearing down a
 * mapping does not have to test first. */
void swap_free(swap_slot_t slot);

/* Reads `swap=<name>` from the command line and attaches that device. Nothing
 * picks a device by looking at it -- see the note in swap.c. */
void swap_init_from_cmdline(void);

void swap_print_summary(void);
bool swap_self_test(void);

#endif /* RECON_KERNEL_SWAP_H */
