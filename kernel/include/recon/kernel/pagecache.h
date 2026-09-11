/* One copy of a file's page, for everyone who has it mapped.
 *
 * Until now a file-backed mapping filled every page privately: each fault
 * allocated a page, zeroed it, and read the file over the top. Two programs
 * mapping the same file got two copies of every page, and a program that
 * mapped a file twice got two more. Nothing was wrong with any of them, and
 * the machine held four copies of a thing that had not changed.
 *
 * --- This already exists, with one entry ---
 *
 * The shared page of zeroes is a page cache whose key is "zeroes". It is
 * mapped read-only into every address space in the machine, a write to it
 * traps, and the fault handler answers by copying it into a page of the
 * writer's own. That is copy-on-write, and it is the whole mechanism this
 * generalises: the same three moves with a key on the front.
 *
 * Which is why the fault path barely changes. `have == zero_page` becomes "is
 * this a page somebody else is also using", and the copy that already happens
 * for zeroes happens for file contents too.
 *
 * --- What a file has to be able to say ---
 *
 * A key needs a name for the file that two different opens agree on. A
 * `struct file *` is not one: `open` twice and there are two of them, and
 * keying on the pointer would give the two mappings separate entries and share
 * nothing -- which is the aliasing the block cache was bitten by, where a
 * partition and its disk were the same sectors under two names.
 *
 * So a filesystem says what a file *is*, through `identity` in its file_ops,
 * and a filesystem that cannot answer stably says zero. Zero is not a failure:
 * it means "fill this the old way", and the old way still works.
 */
#ifndef RECON_KERNEL_PAGECACHE_H
#define RECON_KERNEL_PAGECACHE_H

#include <recon/kernel/types.h>
#include <recon/kernel/pmm.h>

struct file;

/* The page of `f` at `offset`, read in if it is not already here.
 *
 * `offset` must be page-aligned; the cache is keyed on whole pages of a file
 * and a caller asking about the middle of one is asking a question this cannot
 * answer.
 *
 * Returns a page with **a reference taken**, or zero -- which is the ordinary
 * answer for a file with no identity, an unaligned offset, or a cache with no
 * room left, and means the caller should fill a page of its own as it always
 * did. Never an error worth reporting.
 */
paddr_t pagecache_get(struct file *f, u64 offset);

/* Gives one back. Safe for a page the cache does not own, which is what makes
 * it callable from a teardown walk that does not know what it is looking at. */
void pagecache_put(paddr_t page);

/* Whether the cache owns this page. Asked by anything about to free a page it
 * found in a page table, because a cached page is somebody else's too. */
bool pagecache_owns(paddr_t page);

/* Names the fill lock, so it reads as something in the lock summary. */
void pagecache_init(void);

void pagecache_print_summary(void);
bool pagecache_self_test(void);

#endif /* RECON_KERNEL_PAGECACHE_H */
