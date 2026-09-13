/* See bcache.h for the design, particularly why the key is the root device's
 * absolute block number and why every entry is clean.
 *
 * --- The anomalies this accounts for -------------------------------------
 *
 * Written down rather than discovered later. Each is handled at the line that
 * names it, and each refusal is counted, because a cache that quietly declines
 * to cache is indistinguishable from one that is working until somebody
 * measures the disk.
 *
 *  1. A zero-length request. Touches nothing, returns OK.
 *  2. A null buffer. Refused as ALIGN, as the raw path does.
 *  3. A device whose blocks are larger than a page. Not cached; passed
 *     through. One block would span entries and a partial write would need a
 *     read to complete it.
 *  4. A block size that is not a power of two. block.h says it is one; this
 *     does not assume so, and passes such a device through.
 *  5. A request past the end of the named device. Bound checked in the
 *     caller's coordinates before resolution, by block_resolve.
 *  6. **A slice and its parent naming the same sector.** One entry, keyed
 *     absolutely, so a write through either name is seen through both.
 *  7. A slice whose parent has gone. Resolution fails, nothing is cached, and
 *     the caller gets NO_DEVICE rather than a hit on a disk that is not there.
 *  8. A device removed and replaced. Generation is in the key, so old entries
 *     can never match; the sweep in bcache_init's reclaim path frees them.
 *  9. block_claim_raw. Every entry for that root is dropped before the caller
 *     is allowed to start destroying the disk.
 * 10. A write on the raw path. Invalidated from inside block_write, so the
 *     cache's correctness does not depend on the installer knowing it exists.
 * 11. A discard. The device may return zeroes or the old contents and both are
 *     legal, so the range stops being known.
 * 12. Two processors missing on the same unit at once. The second does an
 *     uncached read rather than waiting. A duplicate read in a rare race
 *     costs one I/O; waiting on a lock across a device that yields is a
 *     deadlock.
 * 13. Evicting an entry that is being filled. Refused -- the loader is writing
 *     into that page.
 * 14. A read that fails halfway through filling an entry. The entry goes back
 *     to empty, never to valid. A cache that keeps a failed read serves
 *     garbage for as long as it lives.
 * 15. A request covering part of a unit. Served from the unit; the copy is a
 *     slice of it.
 * 16. A request spanning several units. Looped, and a failure partway is
 *     reported as a failure -- the caller's buffer is partly filled and saying
 *     otherwise would be a lie about their memory.
 * 17. A write updating the cache before the device took it. Never: device
 *     first, cache second.
 * 18. A write that fails partway. The affected units are invalidated rather
 *     than left holding old or new contents.
 * 19. A read-only device. block_write refuses; the cache is not updated,
 *     because it is only updated on success.
 * 20. Memory pressure. Every entry is clean, so bcache_reclaim always
 *     succeeds and never writes anything.
 * 21. No page available when filling. The read still happens, straight to the
 *     caller. A cache that failed a read for want of memory would be worse
 *     than no cache.
 * 22. No memory at all at init. Every call passes through, and the summary
 *     says so rather than reporting a hit rate of zero.
 * 23. Reentry through the eviction path: pmm -> evict -> swap -> block_write
 *     -> invalidate. Invalidation allocates nothing and takes no lock outside
 *     this file, so the loop does not close.
 * 24. A read and a write on the same unit at once. The copies happen under
 *     this file's lock; the ordering between them is the caller's problem and
 *     was already.
 * 25. A partition starting at an odd block. Units are aligned in absolute
 *     coordinates, so a slice's units are its parent's units.
 * 26. **An entry invalidated while it was being filled.** The read may have
 *     returned what was on the disk before the write that invalidated it, so
 *     marking it valid would cache a version that no longer exists. The entry
 *     carries a poison flag for the window in which its lock is not held, and
 *     a poisoned fill is delivered to its caller and then discarded.
 * 28. **The two locks, and the order they must always be taken in.** This file
 *     calls into the block layer while holding its own lock -- block_resolve
 *     and block_device_by_id, to key an entry -- and the block layer calls back
 *     into this one, from block_write and block_claim_raw. Two modules calling
 *     each other is a lock inversion whenever both hold a lock.
 *
 *     It is safe today because **the block layer holds no lock at all**: the
 *     registry is a lock-free scan and a device is serialised by a yielding
 *     busy flag, which block.c chose deliberately because a spinlock cannot be
 *     held across the sched_yield() its drivers poll with. So there is only one
 *     lock in the pair and no order to get wrong.
 *
 *     If the block layer ever takes one, the order is **block first, then
 *     bcache**, because block_write already holds the outer position. The calls
 *     out of this file would then have to be hoisted above the lock, the way
 *     the allocation in anomaly 27 already was.
 * 27. **A page allocated while this file's lock is held.** Not done, and the
 *     read path is shaped around not doing it. See the long note in
 *     bcache_read: it is correct today and becomes a one-processor deadlock
 *     the moment the page allocator learns to evict.
 */
#include <recon/kernel/bcache.h>
#include <recon/kernel/block.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>

enum entry_state {
	ENTRY_EMPTY = 0,	/* holds nothing; its page may be null */
	ENTRY_LOADING,		/* a read is in flight into its page */
	ENTRY_VALID,		/* holds what the disk said */
};

struct entry {
	u32 root_id;
	u32 root_generation;
	u64 unit;		/* absolute block number / blocks_per_unit */
	u32 blocks_per_unit;	/* so the key is unambiguous across devices */

	paddr_t page;
	u8 state;
	bool referenced;	/* second chance; cleared as the hand passes */
	bool poisoned;		/* invalidated mid-fill -- see anomaly 26 */
};

static struct entry entries[BCACHE_UNITS];
static struct spinlock bcache_lock = SPINLOCK_INIT("bcache");
static unsigned hand;			/* the clock */
static unsigned pages_held;		/* how many entries own a page */
static bool ready;

static u64 hits, misses, fills, evictions, invalidations;
static u64 passthrough_size, passthrough_memory;
static u64 fill_failures, race_uncached, poisoned_fills, write_holes;
static u64 passthrough_tail;

/* --- keying ---------------------------------------------------------------
 *
 * Everything below works in the root device's absolute block numbers. Nothing
 * in this file may key on the device the caller named; see anomaly 6.
 */
struct key {
	u32 root_id;
	u32 root_generation;
	u64 abs_lba;
	u32 blocks_per_unit;
};

static bool cacheable(const struct block_device *dev, u32 *blocks_per_unit)
{
	u32 bs;

	if (!dev)
		return false;

	bs = dev->block_size;

	/* A power of two, and no larger than a page. Anomalies 3 and 4. */
	if (!bs || (bs & (bs - 1)) != 0)
		return false;

	if (bs > PAGE_SIZE)
		return false;

	*blocks_per_unit = (u32)(PAGE_SIZE / bs);
	return true;
}

/* Whether a whole unit exists on the disk. The last unit of a device whose
 * size is not a multiple of the unit does not, and is never cached -- see the
 * fill path. */
static bool unit_fits(const struct key *k, u64 unit)
{
	struct block_device *root =
		block_device_by_id(k->root_id, k->root_generation);

	if (!root)
		return false;

	return unit * k->blocks_per_unit + k->blocks_per_unit <=
	       root->block_count;
}

static bool same_unit(const struct entry *e, const struct key *k, u64 unit)
{
	return e->root_id == k->root_id &&
	       e->root_generation == k->root_generation &&
	       e->blocks_per_unit == k->blocks_per_unit &&
	       e->unit == unit;
}

/* Called with the lock held. */
static struct entry *find(const struct key *k, u64 unit)
{
	unsigned i;

	for (i = 0; i < BCACHE_UNITS; i++) {
		struct entry *e = &entries[i];

		if (e->state == ENTRY_EMPTY)
			continue;

		if (same_unit(e, k, unit))
			return e;
	}

	return 0;
}

/* Empties an entry, keeping its page. Lock held.
 *
 * The page stays because the next entry to use this slot needs one, and taking
 * it back to the allocator only to ask for it again is work on the path a cache
 * miss already made expensive. The consequence is that this cache allocates at
 * most BCACHE_UNITS pages in its whole life and then never troubles the page
 * allocator again -- which matters more than it looks, because the allocator is
 * the thing eviction is downstream of.
 *
 * `release` is the other direction, for when the memory really is wanted back.
 */
static void forget(struct entry *e)
{
	e->state = ENTRY_EMPTY;
	e->poisoned = false;
	e->referenced = false;
	e->root_id = 0;
	e->root_generation = 0;
	e->unit = 0;
	e->blocks_per_unit = 0;
}

/* Empties an entry and gives its page back. Lock held. */
static void release(struct entry *e)
{
	if (e->page) {
		pmm_free_page(e->page);
		e->page = 0;
		pages_held--;
	}

	forget(e);
}

/* A victim, by second chance. Lock held.
 *
 * An entry being filled is never a candidate (anomaly 13): its page is the
 * destination of a read that is in flight, and handing it to somebody else
 * would have two callers writing one page and one of them returning the
 * other's disk blocks.
 *
 * Returns null when every entry is loading, which is possible with enough
 * processors and is not an error -- the caller reads uncached. */
static struct entry *victim(void)
{
	unsigned tries;

	for (tries = 0; tries < BCACHE_UNITS * 2; tries++) {
		struct entry *e = &entries[hand];

		hand = (hand + 1) % BCACHE_UNITS;

		if (e->state == ENTRY_LOADING)
			continue;

		if (e->state == ENTRY_EMPTY)
			return e;

		if (e->referenced) {
			e->referenced = false;
			continue;
		}

		evictions++;
		forget(e);
		return e;
	}

	return 0;
}

/* --- the read path --------------------------------------------------------
 */

/* Copies the part of `unit` that the caller asked for out of `page`.
 *
 * `from` is the first absolute block wanted, `n` how many, and both are already
 * known to lie inside the unit. */
static void copy_out(const void *page, u32 block_size, u32 offset_blocks,
		     u32 n, void *dst)
{
	kmemcpy(dst, (const u8 *)page + (u64)offset_blocks * block_size,
		(u64)n * block_size);
}

enum block_status bcache_read(struct block_device *dev, u64 lba, u32 count,
			      void *buf)
{
	struct key k;
	u32 bs;
	u32 done = 0;
	paddr_t spare = 0;
	enum block_status failed = BLOCK_ERR_IO;

	if (count == 0)
		return BLOCK_OK;		/* anomaly 1 */

	if (!buf)
		return BLOCK_ERR_ALIGN;		/* anomaly 2 */

	if (!ready) {
		passthrough_memory++;		/* anomaly 22 */
		return block_read(dev, lba, count, buf);
	}

	if (!cacheable(dev, &k.blocks_per_unit)) {
		passthrough_size++;		/* anomalies 3 and 4 */
		return block_read(dev, lba, count, buf);
	}

	/* The bound, in the caller's own coordinates, before anything is
	 * translated. Anomaly 5, and the order is block_read's. */
	{
		enum block_status s = block_resolve(dev, lba, count,
						    &k.root_id,
						    &k.root_generation,
						    &k.abs_lba);

		if (s != BLOCK_OK)
			return s;		/* anomalies 5 and 7 */
	}

	bs = dev->block_size;

	while (done < count) {
		u64 abs = k.abs_lba + done;
		u64 unit = abs / k.blocks_per_unit;
		u32 within = (u32)(abs % k.blocks_per_unit);
		u32 n = k.blocks_per_unit - within;	/* anomaly 15 */
		struct entry *e;
		struct block_device *root;
		u64 flags;
		paddr_t page;
		enum block_status s;
		bool poisoned;
		bool need_page;

		if (n > count - done)
			n = count - done;

		flags = spin_lock_irq(&bcache_lock);

		e = find(&k, unit);

		if (e && e->state == ENTRY_VALID) {
			e->referenced = true;
			copy_out(phys_to_virt(e->page), bs, within, n,
				 (u8 *)buf + (u64)done * bs);
			hits++;
			spin_unlock_irq(&bcache_lock, flags);
			done += n;
			continue;
		}

		if (e && e->state == ENTRY_LOADING) {
			/* Somebody else is already reading this. Anomaly 12:
			 * read it again rather than wait, because the wait
			 * would be on a lock held across a device that yields.
			 */
			spin_unlock_irq(&bcache_lock, flags);
			misses++;
			race_uncached++;

			s = block_read(dev, lba + done, n,
				       (u8 *)buf + (u64)done * bs);
			if (s != BLOCK_OK) {
				failed = s;
				goto fail;
			}

			done += n;
			continue;
		}

		/* A unit that runs off the end of the disk is not cached at
		 * all. It is a real case -- the last one, on a disk whose size
		 * is not a multiple of the unit -- and reading short and
		 * keeping the rest as whatever the page held would be a cache
		 * that invents the end of every disk. */
		root = block_device_by_id(k.root_id, k.root_generation);

		if (!root || !unit_fits(&k, unit)) {
			spin_unlock_irq(&bcache_lock, flags);
			misses++;
			passthrough_tail++;

			s = block_read(dev, lba + done, n,
				       (u8 *)buf + (u64)done * bs);
			if (s != BLOCK_OK) {
				failed = s;
				goto fail;
			}

			done += n;
			continue;
		}

		need_page = pages_held < BCACHE_UNITS;

		spin_unlock_irq(&bcache_lock, flags);

		/* --- the page is obtained with no lock held -----------------
		 *
		 * **Anomaly 27, and it is the reason this function is shaped
		 * the way it is.** The obvious version allocates inside the
		 * lock, next to the entry it is for. That works today and is a
		 * deadlock waiting for one ordinary change: the moment the page
		 * allocator learns to evict when it is short -- which is the
		 * next thing anybody would do with the eviction policy -- the
		 * path becomes
		 *
		 *     bcache_read (holding bcache_lock)
		 *       -> pmm_alloc_page -> evict_pages -> swap_write_page
		 *       -> block_write -> bcache_invalidate -> bcache_lock
		 *
		 * which is one processor waiting for a lock it is already
		 * holding. Nothing in the allocator does that today, so this
		 * would have been correct when written and fatal later, with
		 * the change that broke it looking unrelated. Allocating out
		 * here costs a re-lookup and closes it permanently.
		 */
		if (need_page && !spare) {
			spare = pmm_alloc_page();

			if (!spare) {
				/* Anomaly 21: no memory is a reason not to
				 * cache, never a reason not to read. */
				misses++;
				passthrough_memory++;

				s = block_read(dev, lba + done, n,
					       (u8 *)buf + (u64)done * bs);
				if (s != BLOCK_OK) {
					failed = s;
					goto fail;
				}

				done += n;
				continue;
			}
		}

		flags = spin_lock_irq(&bcache_lock);

		/* The lock was dropped, so the world may have moved: another
		 * processor may have filled this very unit while this one was
		 * getting a page for it. Looked up again rather than assumed --
		 * installing a second entry here would be the aliasing bug
		 * arriving by a different road. */
		e = find(&k, unit);

		if (e && e->state != ENTRY_EMPTY) {
			/* Round again and let the top of the loop deal with
			 * it: it is a hit now, or somebody else is loading it.
			 * Nothing is counted here, because the count belongs
			 * to whichever of those it turns out to be -- a miss
			 * recorded provisionally and then also counted as a
			 * hit would make the hit rate a number that adds up to
			 * more than the reads. */
			spin_unlock_irq(&bcache_lock, flags);
			continue;
		}

		e = victim();
		if (!e) {
			spin_unlock_irq(&bcache_lock, flags);
			misses++;
			race_uncached++;

			s = block_read(dev, lba + done, n,
				       (u8 *)buf + (u64)done * bs);
			if (s != BLOCK_OK) {
				failed = s;
				goto fail;
			}

			done += n;
			continue;
		}

		/* An evicted entry keeps its page, so usually there is nothing
		 * to do here -- which is the point of forget() and the reason
		 * this cache stops allocating once it is warm. A slot that has
		 * never held anything, or was reclaimed under memory pressure,
		 * takes the page obtained above. */
		if (!e->page) {
			if (!spare) {
				/* Between the check and here, somebody
				 * reclaimed. Not an error and not worth
				 * retrying for: read it and move on. */
				spin_unlock_irq(&bcache_lock, flags);
				misses++;
				passthrough_memory++;

				s = block_read(dev, lba + done, n,
					       (u8 *)buf + (u64)done * bs);
				if (s != BLOCK_OK) {
					failed = s;
					goto fail;
				}

				done += n;
				continue;
			}

			e->page = spare;
			spare = 0;
			pages_held++;
		}

		e->root_id = k.root_id;
		e->root_generation = k.root_generation;
		e->unit = unit;
		e->blocks_per_unit = k.blocks_per_unit;
		e->state = ENTRY_LOADING;
		e->poisoned = false;
		misses++;
		e->referenced = true;
		page = e->page;

		spin_unlock_irq(&bcache_lock, flags);

		/* Outside the lock, because block_read yields. The entry is
		 * LOADING, which is what stops anybody evicting the page this
		 * is about to be written into.
		 *
		 * Read through the root device, in absolute coordinates: that
		 * is a device with no parent, so this is the same bound-checked
		 * path as any other read and not a way around it. It is not
		 * read through `dev`, because a unit at the edge of a slice
		 * extends past that slice's end and the range check would
		 * refuse it, correctly. */
		s = block_read(root, unit * k.blocks_per_unit,
			       k.blocks_per_unit, phys_to_virt(page));

		flags = spin_lock_irq(&bcache_lock);

		/* The entry may have been invalidated while the read was in
		 * flight. It is still LOADING -- nothing may take it -- but the
		 * data it now holds is from before whatever invalidated it.
		 * Anomaly 26. */
		poisoned = e->poisoned;

		if (s != BLOCK_OK) {
			/* Anomaly 14. Empty, never valid. */
			fill_failures++;
			forget(e);
			spin_unlock_irq(&bcache_lock, flags);
			failed = s;
			goto fail;
		}

		copy_out(phys_to_virt(page), bs, within, n,
			 (u8 *)buf + (u64)done * bs);

		if (poisoned) {
			poisoned_fills++;
			forget(e);
		} else {
			e->state = ENTRY_VALID;
			fills++;
		}

		spin_unlock_irq(&bcache_lock, flags);
		done += n;
	}

	if (spare)
		pmm_free_page(spare);

	return BLOCK_OK;

fail:
	/* A page obtained and not used goes back. Leaking one per failed read
	 * would be a slow leak on the path a failing disk takes most often. */
	if (spare)
		pmm_free_page(spare);

	/* The device's own answer, not a summary of it. "The hardware failed"
	 * and "there is no such disk any more" send somebody to different
	 * places, and this is the layer that would flatten them. */
	return failed;
}

/* --- the write path -------------------------------------------------------
 *
 * The device first, always. block_write invalidates the range on its way
 * through (anomaly 10), so by the time this returns the cache holds nothing for
 * what was written -- and this then fills it in from the caller's own buffer,
 * which is a copy of what the device just accepted.
 *
 * Filling from the caller's buffer rather than re-reading is safe *because* the
 * write succeeded: the device has said those blocks now hold these bytes. It is
 * also the only version that does not turn every write into a read.
 */
enum block_status bcache_write(struct block_device *dev, u64 lba, u32 count,
			       const void *buf)
{
	struct key k;
	u32 bs;
	u32 done = 0;
	enum block_status s;

	if (count == 0)
		return BLOCK_OK;

	if (!buf)
		return BLOCK_ERR_ALIGN;

	s = block_write(dev, lba, count, buf);
	if (s != BLOCK_OK)
		return s;			/* anomalies 18 and 19 */

	if (!ready)
		return BLOCK_OK;

	if (!cacheable(dev, &k.blocks_per_unit))
		return BLOCK_OK;

	if (block_resolve(dev, lba, count, &k.root_id, &k.root_generation,
			  &k.abs_lba) != BLOCK_OK)
		return BLOCK_OK;

	bs = dev->block_size;

	while (done < count) {
		u64 abs = k.abs_lba + done;
		u64 unit = abs / k.blocks_per_unit;
		u32 within = (u32)(abs % k.blocks_per_unit);
		u32 n = k.blocks_per_unit - within;
		struct entry *e;
		u64 flags;

		if (n > count - done)
			n = count - done;

		flags = spin_lock_irq(&bcache_lock);

		e = find(&k, unit);

		/* Only an entry that is already valid is updated. A partial
		 * write into an empty entry would leave the rest of the unit
		 * holding whatever the page happened to contain, and that
		 * page's previous contents are somebody else's disk blocks --
		 * so a hole in the middle of a unit is a reason to hold
		 * nothing, not a reason to invent the rest. */
		if (e && e->state == ENTRY_VALID) {
			/* Any part of a unit that is already valid may be
			 * updated in place: the rest of the unit is the disk's
			 * current contents, and these blocks now are too. */
			kmemcpy((u8 *)phys_to_virt(e->page) +
					(u64)within * bs,
				(const u8 *)buf + (u64)done * bs,
				(u64)n * bs);
			e->referenced = true;
		} else if (e && e->state == ENTRY_LOADING) {
			/* A read of this unit is in flight and may already
			 * have passed the blocks being written. Anomaly 26. */
			e->poisoned = true;
		} else if (!e && n == k.blocks_per_unit &&
			   unit_fits(&k, unit)) {
			/* A whole unit written and nothing cached for it: the
			 * caller's buffer is a complete and correct copy, so
			 * it is worth keeping. A partial one is not, which is
			 * the hole counted below. */
			/* **This path never allocates**, for two reasons.
			 *
			 * The first is the deadlock in anomaly 27: this is
			 * under the lock, and asking the page allocator for
			 * memory from under it closes a loop through eviction
			 * and swap the moment the allocator learns to evict.
			 *
			 * The second is a judgement rather than a hazard. A
			 * cache exists to make *reads* cheap. Growing it on a
			 * write means taking a page from programs to remember
			 * something nobody has asked to read, on the guess
			 * that somebody will -- so a slot that already owns a
			 * page is used and one that does not is left alone. */
			e = victim();

			if (e && e->page) {
				kmemcpy(phys_to_virt(e->page),
					(const u8 *)buf + (u64)done * bs,
					(u64)n * bs);
				e->root_id = k.root_id;
				e->root_generation = k.root_generation;
				e->unit = unit;
				e->blocks_per_unit = k.blocks_per_unit;
				e->state = ENTRY_VALID;
				e->poisoned = false;
				e->referenced = true;
				fills++;
			} else {
				write_holes++;
			}
		} else {
			write_holes++;
		}

		spin_unlock_irq(&bcache_lock, flags);
		done += n;
	}

	return BLOCK_OK;
}

/* --- coherence ------------------------------------------------------------
 */
void bcache_invalidate(struct block_device *dev, u64 lba, u64 count)
{
	struct key k;
	u64 flags;
	u64 first, last;
	unsigned i;

	if (!ready || !dev || !count)
		return;

	if (!cacheable(dev, &k.blocks_per_unit))
		return;

	/* count is a u64 here and a u32 in the transfer calls, because this is
	 * also how a whole device is named. Clamped for the resolve, which only
	 * needs the start to be right. */
	if (block_resolve(dev, lba, 1, &k.root_id, &k.root_generation,
			  &k.abs_lba) != BLOCK_OK)
		return;

	first = k.abs_lba / k.blocks_per_unit;
	last = (k.abs_lba + count - 1) / k.blocks_per_unit;

	flags = spin_lock_irq(&bcache_lock);

	for (i = 0; i < BCACHE_UNITS; i++) {
		struct entry *e = &entries[i];

		if (e->state == ENTRY_EMPTY)
			continue;

		if (e->root_id != k.root_id ||
		    e->root_generation != k.root_generation ||
		    e->blocks_per_unit != k.blocks_per_unit)
			continue;

		if (e->unit < first || e->unit > last)
			continue;

		invalidations++;

		if (e->state == ENTRY_LOADING)
			e->poisoned = true;	/* anomaly 26 */
		else
			forget(e);
	}

	spin_unlock_irq(&bcache_lock, flags);
}

void bcache_invalidate_device(struct block_device *dev)
{
	struct key k;
	u64 flags;
	unsigned i;

	if (!ready || !dev)
		return;

	if (!cacheable(dev, &k.blocks_per_unit))
		return;

	if (block_resolve(dev, 0, 1, &k.root_id, &k.root_generation,
			  &k.abs_lba) != BLOCK_OK)
		return;

	flags = spin_lock_irq(&bcache_lock);

	for (i = 0; i < BCACHE_UNITS; i++) {
		struct entry *e = &entries[i];

		if (e->state == ENTRY_EMPTY)
			continue;

		if (e->root_id != k.root_id)
			continue;

		invalidations++;

		if (e->state == ENTRY_LOADING)
			e->poisoned = true;
		else
			forget(e);
	}

	spin_unlock_irq(&bcache_lock, flags);
}

unsigned bcache_reclaim(unsigned pages)
{
	u64 flags;
	unsigned freed = 0;
	unsigned i;

	if (!ready || !pages)
		return 0;

	flags = spin_lock_irq(&bcache_lock);

	for (i = 0; i < BCACHE_UNITS && freed < pages; i++) {
		struct entry *e = &entries[i];

		/* Anomaly 13 again: a page being filled is not free memory. */
		if (e->state == ENTRY_LOADING || !e->page)
			continue;

		release(e);
		freed++;
	}

	spin_unlock_irq(&bcache_lock, flags);
	return freed;
}

void bcache_init(void)
{
	unsigned i;

	for (i = 0; i < BCACHE_UNITS; i++)
		entries[i].state = ENTRY_EMPTY;

	hand = 0;
	ready = true;
}

void bcache_print_summary(void)
{
	u64 total = hits + misses;
	unsigned held = pages_held;

	kprintf("\nBlock cache\n");

	if (!ready) {
		kprintf("  not started -- every read went to the device\n");
		return;
	}

	kprintf("  holding      : %u of %u pages (%u KB)\n", held,
		(unsigned)BCACHE_UNITS,
		(unsigned)(held * (PAGE_SIZE / 1024)));
	kprintf("  reads        : %llu hit, %llu missed",
		(unsigned long long)hits, (unsigned long long)misses);

	if (total)
		kprintf(" (%llu%% hit)",
			(unsigned long long)((hits * 100) / total));
	kprintf("\n");

	kprintf("  filled       : %llu, evicted %llu, invalidated %llu\n",
		(unsigned long long)fills, (unsigned long long)evictions,
		(unsigned long long)invalidations);

	/* The refusals, always, and named. A cache reporting a low hit rate
	 * because it declined to cache anything looks exactly like one whose
	 * working set does not fit, and the two call for opposite responses. */
	if (passthrough_size || passthrough_tail || passthrough_memory ||
	    fill_failures ||
	    race_uncached || poisoned_fills || write_holes) {
		kprintf("  not cached   :");
		if (passthrough_size)
			kprintf(" %llu block size",
				(unsigned long long)passthrough_size);
		if (passthrough_tail)
			kprintf(" %llu partial unit at end of disk",
				(unsigned long long)passthrough_tail);
		if (passthrough_memory)
			kprintf(" %llu no memory",
				(unsigned long long)passthrough_memory);
		if (fill_failures)
			kprintf(" %llu read failed",
				(unsigned long long)fill_failures);
		if (race_uncached)
			kprintf(" %llu already loading",
				(unsigned long long)race_uncached);
		if (poisoned_fills)
			kprintf(" %llu written while loading",
				(unsigned long long)poisoned_fills);
		if (write_holes)
			kprintf(" %llu partial write",
				(unsigned long long)write_holes);
		kprintf("\n");
	}
}

/* --- the self-test --------------------------------------------------------
 *
 * A cache is unusually easy to test wrongly, because **a cache that never
 * caches anything is correct**. Every read returns the right bytes; only the
 * disk knows the difference. So a test that reads twice and compares passes
 * just as happily on a cache that has been disabled, on one whose keying is
 * broken in a way that always misses, and on one that works.
 *
 * Every check below therefore asserts against the *counters* as well as the
 * bytes. The counters are the only place the caching is visible from inside.
 *
 * The two that matter most, because they are the two the design turns on:
 *
 *   - the second read of the same blocks must be a **hit**, not merely
 *     correct;
 *   - the same physical sector reached through a partition and through its
 *     disk must be **one entry**. That is checked by reading through one name
 *     and requiring the read through the other name to hit -- which can only
 *     happen if the key resolved to the same place. A cache keyed on the
 *     device would return the right bytes here too, from a second entry, and
 *     be wrong in exactly the way that corrupts a filesystem later.
 */
static struct block_device *pick_readable(void)
{
	unsigned i;

	for (i = 0; i < block_device_count(); i++) {
		struct block_device *d = block_device_at(i);

		if (d && d->block_size <= PAGE_SIZE && d->block_count)
			return d;
	}

	return 0;
}

/* A slice, and the disk it lives on. Both, or neither. */
static bool pick_slice_and_parent(struct block_device **slice,
				  struct block_device **parent)
{
	unsigned i;

	for (i = 0; i < block_device_count(); i++) {
		struct block_device *d = block_device_at(i);
		struct block_device *p;

		if (!d || !d->parent || !d->block_count)
			continue;

		p = block_device_by_id(d->parent, d->parent_generation);
		if (!p || p->block_size > PAGE_SIZE)
			continue;

		*slice = d;
		*parent = p;
		return true;
	}

	return false;
}

/* A whole device with no table and no slices, as block.c's own test requires:
 * anything else is somebody's disk. */
static struct block_device *pick_writable(void)
{
	unsigned i;

	for (i = 0; i < block_device_count(); i++) {
		struct block_device *d = block_device_at(i);

		if (!d || d->read_only || d->parent || d->slice_count)
			continue;

		if (d->block_size > PAGE_SIZE || !d->block_count)
			continue;

		return d;
	}

	return 0;
}

bool bcache_self_test(void)
{
	struct block_device *d, *slice = 0, *parent = 0;
	paddr_t pa, pb;
	u8 *a, *b;
	bool ok = true;
	u64 hits_before, misses_before;
	u32 n;

	if (!ready) {
		kputs("  bcache: not started\n");
		return false;
	}

	d = pick_readable();
	if (!d) {
		/* Same reasoning as block.c: a machine with no disk is a
		 * machine this kernel must still boot on, and a pass reported
		 * for a test that did not run is worse than saying so. */
		kputs("  bcache: no device to test against\n");
		return true;
	}

	pa = pmm_alloc_page();
	pb = pmm_alloc_page();
	if (!pa || !pb) {
		kputs("  bcache: no memory to test with\n");
		if (pa)
			pmm_free_page(pa);
		if (pb)
			pmm_free_page(pb);
		return false;
	}

	a = phys_to_virt(pa);
	b = phys_to_virt(pb);

	n = (u32)(PAGE_SIZE / d->block_size);
	if ((u64)n > d->block_count)
		n = (u32)d->block_count;

	/* --- it returns what the device returns ---------------------------
	 *
	 * The uncached read first, so that a cache which hands back its own
	 * page unmodified cannot pass by comparing itself with itself. */
	if (block_read(d, 0, n, a) != BLOCK_OK) {
		kputs("  bcache: the device could not be read at all\n");
		ok = false;
		goto out;
	}

	kmemset(b, 0, PAGE_SIZE);

	if (bcache_read(d, 0, n, b) != BLOCK_OK) {
		kputs("  bcache: a read through the cache failed where the "
		      "raw read succeeded\n");
		ok = false;
		goto out;
	}

	if (kmemcmp(a, b, (u64)n * d->block_size) != 0) {
		kputs("  bcache: the cache returned different bytes from the "
		      "device\n");
		ok = false;
	}

	/* --- and the second time, it is a hit ------------------------------
	 *
	 * Without this the whole file could be a no-op and every other check
	 * here would still pass. */
	hits_before = hits;
	kmemset(b, 0, PAGE_SIZE);

	if (bcache_read(d, 0, n, b) != BLOCK_OK) {
		kputs("  bcache: the second read failed\n");
		ok = false;
	} else if (hits == hits_before) {
		kputs("  bcache: reading the same blocks twice did not hit -- "
		      "nothing is being cached\n");
		ok = false;
	} else if (kmemcmp(a, b, (u64)n * d->block_size) != 0) {
		kputs("  bcache: the cached copy differs from the disk\n");
		ok = false;
	}

	/* --- a partition and its disk are one entry ------------------------ */
	if (pick_slice_and_parent(&slice, &parent)) {
		u32 sn = (u32)(PAGE_SIZE / slice->block_size);
		u64 abs;

		if ((u64)sn > slice->block_count)
			sn = (u32)slice->block_count;

		/* The slice's block 0 in the parent's coordinates. Only one
		 * level is walked here on purpose: the test is that the two
		 * names meet, and a deeper chain is block.c's arithmetic,
		 * which has its own test. */
		abs = slice->first_lba;

		if (bcache_read(slice, 0, sn, a) == BLOCK_OK) {
			hits_before = hits;
			misses_before = misses;

			if (bcache_read(parent, abs, sn, b) != BLOCK_OK) {
				kputs("  bcache: could not read the same "
				      "sectors through the disk\n");
				ok = false;
			} else if (kmemcmp(a, b, (u64)sn * slice->block_size)
				   != 0) {
				kputs("  bcache: a partition and its disk "
				      "returned different bytes for the same "
				      "sectors\n");
				ok = false;
			} else if (hits == hits_before &&
				   misses > misses_before) {
				kputs("  bcache: the same sectors reached "
				      "through a partition and through its "
				      "disk are two entries, so a write "
				      "through one will not be seen through "
				      "the other\n");
				ok = false;
			}
		}
	} else {
		kputs("  bcache: no partitioned disk here, so the aliasing "
		      "check did not run\n");
	}

	/* --- a raw write is seen through the cache -------------------------
	 *
	 * The hook in block_write is what makes this pass. Without it the read
	 * below returns what was on the disk before, which is precisely the
	 * failure a cache introduces and nothing else can.
	 */
	{
		struct block_device *w = pick_writable();

		if (w) {
			u32 wn = (u32)(PAGE_SIZE / w->block_size);
			u64 first;
			u32 i, bytes;

			if ((u64)wn > w->block_count)
				wn = (u32)w->block_count;

			first = w->block_count - wn;
			bytes = wn * w->block_size;

			/* Keep what is there, and put it back. */
			if (block_read(w, first, wn, a) != BLOCK_OK) {
				kputs("  bcache: could not read the blocks it "
				      "meant to borrow\n");
				ok = false;
			} else {
				/* Into the cache, so there is something stale
				 * to catch. */
				bcache_read(w, first, wn, b);

				for (i = 0; i < bytes; i++)
					b[i] = (u8)(i * 11 + 5);

				if (block_write(w, first, wn, b) != BLOCK_OK) {
					kputs("  bcache: could not write\n");
					ok = false;
				} else {
					kmemset(b, 0, PAGE_SIZE);

					if (bcache_read(w, first, wn, b) !=
					    BLOCK_OK) {
						kputs("  bcache: read back "
						      "failed\n");
						ok = false;
					} else {
						for (i = 0; i < bytes; i++) {
							if (b[i] == (u8)(i * 11 + 5))
								continue;

							kputs("  bcache: a "
							      "write went to "
							      "the disk and "
							      "the cache still "
							      "returned the "
							      "old contents\n");
							ok = false;
							break;
						}
					}
				}

				/* Put it back before reporting, so the test
				 * can be run twice. */
				if (block_write(w, first, wn, a) != BLOCK_OK) {
					kputs("  bcache: could not restore "
					      "what it borrowed\n");
					ok = false;
				}
			}
		} else {
			kputs("  bcache: no blank disk here, so the "
			      "write-coherence check did not run\n");
		}
	}

	/* --- the edges ----------------------------------------------------- */
	if (bcache_read(d, 0, 0, b) != BLOCK_OK) {
		kputs("  bcache: a zero-length read was refused\n");
		ok = false;
	}

	if (bcache_read(d, 0, n, 0) != BLOCK_ERR_ALIGN) {
		kputs("  bcache: a null buffer was accepted\n");
		ok = false;
	}

	if (bcache_read(d, d->block_count, 1, b) != BLOCK_ERR_RANGE) {
		kputs("  bcache: reading past the end of the device was "
		      "allowed\n");
		ok = false;
	}

	/* --- reclaim gives memory back, and costs only speed ----------------
	 *
	 * Both halves are asserted. Freeing nothing means the cache cannot
	 * answer memory pressure; a read that misses afterwards is what proves
	 * the pages actually went, rather than the counter being decremented
	 * over a cache that kept them.
	 */
	{
		unsigned freed;

		if (block_read(d, 0, n, a) != BLOCK_OK) {
			kputs("  bcache: the device stopped answering\n");
			ok = false;
			goto out;
		}

		bcache_read(d, 0, n, b);	/* make sure it is cached */

		freed = bcache_reclaim(BCACHE_UNITS);

		if (!freed) {
			kputs("  bcache: reclaim freed nothing, so the cache "
			      "cannot give memory back under pressure\n");
			ok = false;
		}

		hits_before = hits;
		misses_before = misses;
		kmemset(b, 0, PAGE_SIZE);

		if (bcache_read(d, 0, n, b) != BLOCK_OK) {
			kputs("  bcache: a read after reclaim failed\n");
			ok = false;
		} else if (kmemcmp(a, b, (u64)n * d->block_size) != 0) {
			kputs("  bcache: a read after reclaim returned the "
			      "wrong bytes\n");
			ok = false;
		} else if (misses == misses_before && hits > hits_before) {
			kputs("  bcache: a read after reclaim hit, so the "
			      "pages were counted as freed and kept\n");
			ok = false;
		}
	}

out:
	pmm_free_page(pa);
	pmm_free_page(pb);
	return ok;
}
