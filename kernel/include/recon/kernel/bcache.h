/* Blocks the kernel has already read, kept.
 *
 * A filesystem reads the same handful of blocks constantly. Every path lookup
 * reads the superblock; every file read walks the same inode blocks; opening
 * ten files in one directory reads that directory ten times. On a spinning disk
 * each of those is milliseconds, five orders of magnitude more than the memory
 * copy it could have been, and on a machine with 512 MB of memory the working
 * set of a filesystem is a few hundred kilobytes. This is the cheapest
 * performance in the whole storage stack and the audit names it under 1.6.
 *
 * --- It is not inside block_read, deliberately ---
 *
 * The obvious design is to put the cache inside `block_read` so that everything
 * gets it for free. That would be wrong for three of this kernel's four block
 * readers, each for a different reason:
 *
 *   - the **partition reader** reads a disk once, to find out what is on it. It
 *     must see the disk as it is now, and caching what it read would only make
 *     a second look agree with the first whether or not the disk did;
 *   - the **installer** writes a disk it has just claimed. A cache in front of
 *     it would hand back the partition table from before the install;
 *   - **swap** exists to move pages that are not in memory. A cache in front of
 *     swap keeps in memory the very pages that were evicted for not fitting,
 *     which is not a cache but a leak with good manners.
 *
 * So this is a separate call, and a caller opts in. `reconfs` is the caller it
 * was built for. That also means the raw path stays exactly what it was, which
 * matters because `block_claim_raw` and the layout checker are safety
 * machinery, and safety machinery with a cache in front of it is worth
 * re-examining rather than inheriting.
 *
 * --- What is cached, and the aliasing problem that decides it ---
 *
 * **A partition and the disk it lives on are the same sectors under two
 * names.** Reading block 0 of `virtio0p1` and block 2048 of `virtio0` reach one
 * physical sector. A cache keyed on the device it was handed would hold two
 * entries for that sector, and a write through one name would leave the other
 * holding what used to be there -- a filesystem reading its own superblock back
 * as the version from before it wrote it.
 *
 * So the key is not the device the caller named. Every request is resolved to
 * the **root device and the absolute block number** first, by the same code
 * `block_read` uses to do it, and the cache is keyed on that. One sector, one
 * entry, however it was addressed. `block_resolve` exists in block.c rather than
 * here for exactly this reason: two implementations of that arithmetic would
 * drift, and the day they drifted the cache would start aliasing again.
 *
 * The unit is one page, not one block. Devices have 512-byte blocks and the
 * page allocator hands out 4096-byte pages, so an entry holds the whole
 * page-aligned run a block falls in -- eight sectors on such a device, one on a
 * device with 4096-byte blocks. Read-ahead for free, and no partial pages to
 * account for. **The alignment is in absolute coordinates**, which is the other
 * half of the aliasing fix: a partition starting at an odd block would
 * otherwise have its own units offset from its parent's.
 *
 * A device whose blocks are larger than a page is not cached at all. One block
 * would span several entries and a partial write would have to read the rest
 * back -- the same reason swap refuses such a device, and the same answer.
 *
 * --- Write-through, and why that is not a compromise ---
 *
 * A write goes to the device first and updates the cache only if the device
 * took it. Not write-back: no dirty list, no ordered flush, no window in which
 * the only copy of somebody's data is in volatile memory.
 *
 * The cost is real -- no write coalescing, so a filesystem writing the same
 * block twice writes the disk twice. The gain is that **every entry in this
 * cache is clean, always**, and that single property is what makes the rest of
 * it safe:
 *
 *   - it can be dropped entirely, at any instant, with no loss. That is what
 *     lets memory pressure reclaim it (see `bcache_reclaim`) without a way for
 *     reclaim to fail or to have to write to a disk to make progress -- which,
 *     on the eviction path, would be the deadlock swap.h describes;
 *   - a power cut loses nothing this cache was responsible for;
 *   - `flush_is_durable` keeps meaning what block.h says it means. A write-back
 *     cache would have quietly added a second volatile layer above the one that
 *     header goes to such lengths not to lie about.
 *
 * Write-back is the right answer once there is a wait queue and a flush that
 * can be ordered. It is the wrong answer to reach for first.
 *
 * --- What it costs the page allocator ---
 *
 * At most BCACHE_UNITS pages, once. An entry that is evicted keeps its page for
 * whatever takes the slot next, so after the cache has filled out it does not
 * ask the allocator for anything again -- and a write never grows it at all.
 *
 * That is a deliberate shape rather than an optimisation. The allocator is what
 * eviction hangs off, eviction writes to swap, and swap writes through the
 * block layer back into this file; a cache that reached for memory on every
 * miss would be threading that loop thousands of times a second and relying on
 * it never closing. Reaching for memory sixty-four times and then stopping does
 * not.
 *
 * --- Coherence with everything that is not this cache ---
 *
 * A cache is only correct while nothing changes the disk behind it. Three
 * things can, and each is handled where it happens rather than hoped about:
 *
 *   - `block_write` on any device invalidates the range it wrote. Not optional
 *     and not the caller's job: the installer and the partition writer use the
 *     raw path precisely because they are not filesystem traffic, and requiring
 *     them to remember would make the cache's correctness depend on code that
 *     has no idea it exists;
 *   - `block_discard` invalidates too. The device is permitted to return
 *     zeroes or the old contents afterwards and both are legal, so what is
 *     cached is no longer known to be anything;
 *   - `block_claim_raw` drops every entry for that disk. A claim is a
 *     declaration that the whole device is about to be rewritten, which makes
 *     every cached block on it a statement about a disk that is about to stop
 *     existing.
 *
 * Generation is part of the key, so a device that vanished and came back cannot
 * match entries belonging to the disk that used to be there.
 *
 * --- What it does not do ---
 *
 * No read-ahead beyond the unit. Guessing the next block is a policy, and a
 * policy that is wrong costs a read the caller never asked for; the unit
 * already gives sequential access seven hits out of eight on a 512-byte device.
 *
 * No write-behind, no elevator, no queue. All three need a thread that can
 * wait, which block.h explains this kernel does not have yet.
 */
#ifndef RECON_KERNEL_BCACHE_H
#define RECON_KERNEL_BCACHE_H

#include <recon/kernel/types.h>
#include <recon/kernel/block.h>

/* How many pages the cache may hold.
 *
 * A visible number. 64 pages is 256 KB, which on the smallest machine in the
 * verification matrix is a twentieth of memory and holds a filesystem's whole
 * working set several times over. Bigger is not better here: past the working
 * set the extra pages are memory taken from programs to hold blocks nobody will
 * ask for again. */
#define BCACHE_UNITS 64

/* Reads through the cache. Same arguments and same statuses as `block_read`,
 * and identical in effect -- the only difference is how long it takes.
 *
 * Bounds are checked against the device the caller named, in that device's own
 * coordinates, before anything is resolved or looked up. That is the order
 * block_read uses and the order matters: a slice's length is the bound, and
 * checking after translation would check the disk's. */
enum block_status bcache_read(struct block_device *dev, u64 lba, u32 count,
			      void *buf);

/* Writes through the cache: to the device, then into the cache if the device
 * accepted it.
 *
 * A partial failure leaves nothing cached for the affected range rather than
 * either the old contents or the new. When a write's outcome is unknown, "not
 * cached" is the only honest state -- a guess in either direction is a cache
 * that will confidently return something that was never on the disk. */
enum block_status bcache_write(struct block_device *dev, u64 lba, u32 count,
			       const void *buf);

/* --- Coherence hooks, called from block.c ---------------------------------
 *
 * These take no locks that the block layer holds and never allocate, because
 * they are called from inside the write path -- which, by way of swap, is a
 * path the page allocator can be waiting on. Something on the invalidation path
 * that needed a page would close that loop. */
void bcache_invalidate(struct block_device *dev, u64 lba, u64 count);
void bcache_invalidate_device(struct block_device *dev);

/* Frees up to `pages` cached pages and returns how many it actually freed.
 *
 * Every entry is clean, so this cannot fail and cannot block. It is the reason
 * a cache that grows to fill memory is not a problem: under pressure it is the
 * first thing asked to give memory back, and it always can. */
unsigned bcache_reclaim(unsigned pages);

void bcache_init(void);
void bcache_print_summary(void);
bool bcache_self_test(void);

#endif /* RECON_KERNEL_BCACHE_H */
