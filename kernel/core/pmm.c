#include <recon/kernel/pmm.h>
#include <recon/kernel/boot.h>
#include <recon/kernel/sched.h>
#include <recon/kernel/time.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/panic.h>
#include <recon/kernel/vm.h>

/* One bit per page: set means in use, clear means free. Starting from "all
 * used" and clearing what the firmware called usable is the safe direction --
 * memory nobody told us about stays untouchable, which is the correct
 * treatment of an unknown. */
static u8    *bitmap;
/* One processor at a time in the bitmap. (KF-149)
 *
 * This file had no lock and no atomic in it until 10 September, under a comment
 * in heap.h explaining that there is one processor running kernel code "until
 * checkpoint 9". Checkpoint 9 brought threads and 9b woke every processor; the
 * sentence stayed, and nothing in the build or the rig was watching its date.
 *
 * What made it reachable is newer than the hazard. Demand paging calls
 * pmm_alloc_page() from *inside a page-fault handler*, on whichever processor
 * the faulting program is running on, and the scheduler's reaper frees a
 * finished thread's stack from a different thread on a different processor. Two
 * processors scanning at once can both find the same clear bit and both claim
 * it -- the same page handed to two owners, which is not a crash but two
 * subsystems overwriting each other for as long as the machine runs.
 *
 * INTERRUPTS OFF WHILE IT IS HELD, and not for the other processor's sake. The
 * lock is taken inside a fault handler; a timer interrupt arriving while it is
 * held can preempt into another thread that allocates *on this same processor*,
 * which would then wait for a lock its own processor is holding and never get
 * it. spin_lock_irq, everywhere, no exceptions. */
static struct spinlock pmm_lock = SPINLOCK_INIT("pmm");

static size_t bitmap_bytes;
static size_t total_pages;
static size_t free_pages;

/* The address the bitmap's first bit describes.
 *
 * Not zero, and this matters. On x86 RAM starts near zero and the distinction
 * is invisible, but on ARM it commonly starts at 1GB, and on server parts at
 * 4GB or higher. A bitmap indexed from physical zero would spend its first
 * quarter-million bits describing addresses that are not memory -- reporting
 * them as "used", which is both wasteful and a lie, since nothing is using
 * them and nothing could. */
static paddr_t base_paddr;

/* Where the bitmap physically lives, kept because the address it is *reached*
 * at changes when the kernel stops using the map it was handed. */
static paddr_t bitmap_base;

static inline size_t page_index(paddr_t addr)
{
	return (size_t)((addr - base_paddr) / PAGE_SIZE);
}

static inline paddr_t page_addr(size_t index)
{
	return base_paddr + (paddr_t)index * PAGE_SIZE;
}

/* Where the last search stopped. Without it, allocating n pages costs a scan
 * from zero every time and boot gets quadratic in the number of allocations. */
static size_t search_hint;

static inline bool bit_test(size_t page)
{
	return (bitmap[page / 8] >> (page % 8)) & 1u;
}

static inline void bit_set(size_t page)
{
	bitmap[page / 8] |= (u8)(1u << (page % 8));
}

static inline void bit_clear(size_t page)
{
	bitmap[page / 8] &= (u8)~(1u << (page % 8));
}

static void mark_used(size_t page, size_t count)
{
	for (size_t i = 0; i < count && page + i < total_pages; i++)
		if (!bit_test(page + i)) {
			bit_set(page + i);
			free_pages--;
		}
}

static void mark_free(size_t page, size_t count)
{
	for (size_t i = 0; i < count && page + i < total_pages; i++)
		if (bit_test(page + i)) {
			bit_clear(page + i);
			free_pages++;
		}
}

/* The bitmap has to live somewhere, and it has to be found before there is an
 * allocator to find it with. So it is placed by hand, in the first usable
 * region large enough to hold it, and then marked used by the very allocator
 * it makes possible. */
static paddr_t place_bitmap(size_t bytes)
{
	const struct boot_info *info = boot_info();

	for (unsigned i = 0; i < info->region_count; i++) {
		const struct mem_region *r = &info->regions[i];
		paddr_t base;
		u64 end;

		if (r->kind != MEM_USABLE)
			continue;

		base = PAGE_ALIGN_UP(r->base);
		end  = r->base + r->size;

		/* The page at physical zero is never used, so a region starting
		 * there must give up its first page before it can be measured. */
		if (base == 0)
			base = PAGE_SIZE;

		if (end > base && end - base >= bytes)
			return base;
	}

	return 0;
}

void pmm_init(void)
{
	const struct boot_info *info = boot_info();
	u64 lowest = ~0ULL;
	u64 highest = 0;
	paddr_t bitmap_phys;

	/* The bitmap spans the lowest to the highest *usable* address, and
	 * neither bound comes from the full map. Machines commonly report
	 * reserved ranges far above their RAM -- a memory-mapped device at
	 * sixty-something gigabytes would otherwise demand a bitmap of tens of
	 * megabytes to describe memory that will never be allocated. */
	for (unsigned i = 0; i < info->region_count; i++)
		if (info->regions[i].kind == MEM_USABLE) {
			u64 start = info->regions[i].base;
			u64 end = start + info->regions[i].size;

			if (start < lowest)
				lowest = start;
			if (end > highest)
				highest = end;
		}

	if (highest == 0)
		panic("pmm: the memory map contains no usable memory");

	base_paddr   = (paddr_t)PAGE_ALIGN_DOWN(lowest);
	total_pages  = (size_t)((PAGE_ALIGN_UP(highest) - base_paddr) / PAGE_SIZE);
	bitmap_bytes = (total_pages + 7) / 8;

	bitmap_phys = place_bitmap(bitmap_bytes);
	if (!bitmap_phys)
		panic("pmm: no usable region large enough to hold the page bitmap");

	/* Identity while the kernel is still on the map it was handed. Once the
	 * kernel builds its own, the bitmap has to be reached through the direct
	 * map instead -- see pmm_remap(), which vm_init() calls the moment the
	 * new tables are live. */
	bitmap_base = bitmap_phys;
	bitmap = (u8 *)(uintptr_t)bitmap_phys;

	kmemset(bitmap, 0xFF, bitmap_bytes);
	free_pages = 0;

	/* Now give back what the firmware said we may have. Partial pages at
	 * either end are dropped rather than rounded outward: rounding a usable
	 * range up would claim a page that overlaps something else's. */
	for (unsigned i = 0; i < info->region_count; i++) {
		const struct mem_region *r = &info->regions[i];
		u64 start, end;

		if (r->kind != MEM_USABLE)
			continue;

		start = PAGE_ALIGN_UP(r->base);
		end   = PAGE_ALIGN_DOWN(r->base + r->size);

		if (end > start)
			mark_free(page_index(start),
				  (size_t)((end - start) / PAGE_SIZE));
	}

	/* Two things the allocator must never hand out. The bitmap, because it
	 * is standing on it. And the page at physical zero, so that a null
	 * return from pmm_alloc_page() and a null pointer bug cannot be
	 * confused -- which only arises where memory starts at zero at all. */
	mark_used(page_index(bitmap_phys),
		  (size_t)(PAGE_ALIGN_UP(bitmap_bytes) / PAGE_SIZE));
	if (base_paddr == 0)
		mark_used(0, 1);

	search_hint = 0;
}

paddr_t pmm_alloc_pages(size_t count)
{
	size_t run = 0;
	size_t scanned = 0;
	size_t i;
	u64 flags;

	if (count == 0 || count > total_pages)
		return 0;

	flags = spin_lock_irq(&pmm_lock);
	i = search_hint;

	/* One pass over every page, starting where the last search finished and
	 * wrapping once. Bounded by construction: a full circuit means there is
	 * no run of this length and the answer is genuinely no. */
	while (scanned < total_pages) {
		if (bit_test(i)) {
			run = 0;
		} else {
			run++;
			if (run == count) {
				size_t start = i + 1 - count;
				paddr_t at;

				mark_used(start, count);
				search_hint = (i + 1) % total_pages;
				at = page_addr(start);

				/* The bits are set, so these pages are this
				 * caller's and nobody else can be handed them.
				 * The lock goes before the clearing rather than
				 * after: holding it across a page-sized memset
				 * would serialise every allocation in the
				 * machine behind one, which at thirty-two
				 * processors is a different bug wearing this
				 * one's clothes.
				 *
				 * Marked first and cleared second, never the
				 * other way round -- a page cleared before it
				 * is claimed can be handed out again while
				 * still holding the last owner's bytes. */
				spin_unlock_irq(&pmm_lock, flags);

				/* Cleared on the way OUT, not on the way in.
				 *
				 * Clearing on free trusts every caller to have
				 * called free, to have called it on the whole
				 * allocation, and not to have kept a copy.
				 * Clearing on allocate is unconditional and
				 * cannot be forgotten by anybody -- including
				 * code that has not been written yet.
				 *
				 * That is the difference between a guarantee and
				 * a convention, and it is what stops a page that
				 * held a password reaching the next process that
				 * asks for memory. It costs a page-zeroing per
				 * allocation, which is real and is the right
				 * thing to pay. */
				kmemset(phys_to_virt(at), 0,
					count * PAGE_SIZE);

				return at;
			}
		}

		i++;
		scanned++;

		if (i >= total_pages) {
			i = 0;
			/* A run cannot straddle the wrap: page total_pages-1 and
			 * page 0 are not adjacent in physical memory. */
			run = 0;
		}
	}

	spin_unlock_irq(&pmm_lock, flags);
	return 0;
}

paddr_t pmm_alloc_page(void)
{
	return pmm_alloc_pages(1);
}

/* A page below a given physical address.
 *
 * Not a convenience. Hardware imposes ceilings that have nothing to do with how
 * much memory there is: a processor started by its neighbour begins in a mode
 * that can only address the first megabyte, and older bus masters can only
 * reach the first four gigabytes. A caller that needs one of those cannot use
 * pmm_alloc_pages and then check, because by then the page it wanted has been
 * handed out to somebody else.
 *
 * The limit is the caller's, not this file's -- nothing here knows what a real
 * mode is. That is what keeps the constraint expressible without putting a
 * machine into core/.
 */
paddr_t pmm_alloc_page_below(paddr_t limit)
{
	size_t i;
	u64 flags = spin_lock_irq(&pmm_lock);

	/* Searched from the bottom rather than from the hint. The hint exists to
	 * spread allocations out, which is exactly the wrong instinct here:
	 * pages under a ceiling are scarce and the ones nearest the bottom are
	 * the ones nothing else can be made to want. */
	for (i = 0; i < total_pages; i++) {
		paddr_t at = page_addr(i);

		if (at + PAGE_SIZE > limit)
			break;		/* addresses only rise from here */

		if (bit_test(i))
			continue;

		mark_used(i, 1);
		spin_unlock_irq(&pmm_lock, flags);

		/* Outside the lock, for the reason in pmm_alloc_pages. */
		kmemset(phys_to_virt(at), 0, PAGE_SIZE);
		return at;
	}

	spin_unlock_irq(&pmm_lock, flags);
	return 0;
}

void pmm_free_pages(paddr_t page, size_t count)
{
	size_t index;

	if (page == 0 || (page % PAGE_SIZE) != 0)
		panic("pmm: asked to free something that is not a page address");

	/* Below the base is as wrong as beyond the end, and the subtraction in
	 * page_index() would wrap into an enormous index that passes the upper
	 * bound check. Caught here rather than corrupting a distant bit. */
	if (page < base_paddr)
		panic("pmm: asked to free memory below the start of the bitmap");

	index = page_index(page);

	if (index + count > total_pages)
		panic("pmm: asked to free memory past the end of the bitmap");

	{
		u64 flags = spin_lock_irq(&pmm_lock);

		mark_free(index, count);
		spin_unlock_irq(&pmm_lock, flags);
	}
}

void pmm_free_page(paddr_t page)
{
	pmm_free_pages(page, 1);
}

/* Called once, by vm_init(), immediately after the kernel switches to its own
 * page tables. Before that the bitmap is reachable at its physical address
 * because the map we were handed is an identity map; afterwards it is not,
 * because the kernel maps only what it means to. Without this the first
 * allocation after the switch writes into an unmapped page. */
void pmm_remap(void)
{
	bitmap = (u8 *)phys_to_virt(bitmap_base);
}

size_t pmm_total_pages(void)     { return total_pages; }
size_t pmm_free_page_count(void) { return free_pages; }

void pmm_print_summary(void)
{
	size_t used = total_pages - free_pages;

	kprintf("\nPhysical memory\n");
	kprintf("  page size    : %lu bytes\n", (u64)PAGE_SIZE);
	kprintf("  pages        : %lu total, %lu free, %lu used\n",
		(u64)total_pages, (u64)free_pages, (u64)used);
	kprintf("  free         : %lu MB\n",
		(u64)(free_pages * PAGE_SIZE) / (1024 * 1024));
	kprintf("  describes    : %p upward, %lu MB\n",
		(void *)(uintptr_t)base_paddr,
		(u64)(total_pages * PAGE_SIZE) / (1024 * 1024));
	kprintf("  bitmap       : %p, %lu bytes\n",
		(void *)bitmap, (u64)bitmap_bytes);
}

bool pmm_self_test(void)
{
	size_t before = free_pages;
	paddr_t a, b, run;
	bool ok = true;

	/* Distinct pages, and both actually free beforehand. */
	a = pmm_alloc_page();
	b = pmm_alloc_page();
	if (!a || !b || a == b) {
		kputs("  pmm: two allocations were not two distinct pages\n");
		ok = false;
	}

	/* A contiguous run really is contiguous, and really is marked used. */
	run = pmm_alloc_pages(4);
	if (!run) {
		kputs("  pmm: could not allocate four contiguous pages\n");
		ok = false;
	} else {
		for (size_t i = 0; i < 4; i++)
			if (!bit_test(page_index(run) + i)) {
				kputs("  pmm: a page inside a fresh run was not marked used\n");
				ok = false;
				break;
			}
	}

	if (free_pages != before - 6) {
		kputs("  pmm: the free count did not fall by the six pages taken\n");
		ok = false;
	}

	if (run)
		pmm_free_pages(run, 4);
	if (b)
		pmm_free_page(b);
	if (a)
		pmm_free_page(a);

	/* The point of the whole test: everything taken came back. A leak here
	 * is one that would otherwise be found as a machine that slows down
	 * over hours. */
	if (free_pages != before) {
		kprintf("  pmm: %lu pages did not come back after being freed\n",
			(u64)(before - free_pages));
		ok = false;
	}

	return ok;
}

/* --- several processors in the allocator at once --------------------------
 *
 * The test above allocates everything, frees it, and requires the count back.
 * It is a real test and it runs on one processor, so it cannot see the fault
 * this lock exists to prevent: two processors finding the same clear bit and
 * both claiming it.
 *
 * That fault does not look like a crash. It looks like two subsystems writing
 * over each other for as long as the machine runs -- so the assertion has to be
 * about *ownership*, not about a return value. Each thread writes its own name
 * into every page it is given, yields to make the window as wide as possible,
 * and then requires the page to still say its name.
 *
 * A page handed to two owners is a page whose contents change under the first
 * one. There is no other way for that to happen: the allocator clears every
 * page it hands out, so a page holding somebody else's marker was given away
 * twice.
 *
 * Watched to fail, and the numbers are here rather than a claim that it
 * works: with the lock removed from pmm_alloc_pages, five of six boots at
 * -smp 4 fail, splitting between the two symptoms below. With the lock in
 * place, eight of eight pass. One unlocked boot in six still passes, so this
 * is a detector rather than a proof -- but the first version of it passed
 * four times out of four with the bug present, which is not a test at all.
 */
#define PMM_RACERS  4
#define PMM_BATCH   8
#define PMM_ROUNDS  64

static volatile unsigned racers_ready;
static volatile unsigned racers_done;
static volatile unsigned race_go;
static volatile unsigned race_release;
static volatile unsigned pages_stolen;
static volatile unsigned pages_missed;

static void racer(void *arg)
{
	u32 me = (u32)(uintptr_t)arg;
	unsigned round;

	/* Two barriers, and they are here for the free-page count rather than
	 * for the race.
	 *
	 * A thread costs pages -- a stack, and whatever its structure needed
	 * from the heap -- allocated by thread_create and freed by the reaper
	 * long afterwards. Counting from before the threads exist to after
	 * they finish therefore measures the threads and not the allocator:
	 * the first version of this check reported a drift of exactly twenty
	 * pages on every run, locked or unlocked, which is four thread stacks
	 * and not a bug.
	 *
	 * So the window opens once every racer is running and closes before
	 * any of them is allowed to leave. Inside it every page allocated is
	 * also freed, and the count has to be unchanged. */
	__atomic_add_fetch(&racers_ready, 1, __ATOMIC_RELEASE);
	while (!__atomic_load_n(&race_go, __ATOMIC_ACQUIRE))
		sched_yield();

	for (round = 0; round < PMM_ROUNDS; round++) {
		paddr_t got[PMM_BATCH];
		unsigned n = 0, k;

		/* A batch rather than a single page. One page at a time makes
		 * this a test of whether two processors are inside the same
		 * scan at the same instant, which is rare enough that the
		 * first version of this test passed every time with the lock
		 * removed. A batch touches many more bitmap bytes and holds
		 * them for longer. */
		for (k = 0; k < PMM_BATCH; k++) {
			paddr_t p = pmm_alloc_page();

			if (p)
				got[n++] = p;
		}

		if (n == 0) {
			/* Running out is not this test's subject and is not a
			 * failure of it -- but it is counted, because a run
			 * that allocated nothing would otherwise report a
			 * clean pass having proved nothing. */
			__atomic_add_fetch(&pages_missed, 1, __ATOMIC_RELAXED);
			sched_yield();
			continue;
		}

		/* Two markers, because one cannot tell "somebody wrote here"
		 * from "this was never mine": the first word says who, the
		 * second says which page of which round. */
		for (k = 0; k < n; k++) {
			volatile u32 *at = phys_to_virt(got[k]);

			at[0] = 0xACCE5500u | me;
			at[1] = (u32)((round << 8) | k);
		}

		/* The window. Without it the whole sequence can run inside one
		 * slice and the processors never overlap where it matters. */
		sched_yield();

		for (k = 0; k < n; k++) {
			volatile u32 *at = phys_to_virt(got[k]);

			if (at[0] != (0xACCE5500u | me) ||
			    at[1] != (u32)((round << 8) | k))
				__atomic_add_fetch(&pages_stolen, 1,
						   __ATOMIC_RELAXED);
		}

		for (k = 0; k < n; k++)
			pmm_free_page(got[k]);
	}

	__atomic_add_fetch(&racers_done, 1, __ATOMIC_RELEASE);

	/* Parked here so the count is read while these threads still own
	 * their stacks. Released unconditionally by the caller, including on
	 * every failure path -- a barrier nobody lifts is a hang. */
	while (!__atomic_load_n(&race_release, __ATOMIC_ACQUIRE))
		sched_yield();
}

bool pmm_concurrent_test(void)
{
	unsigned i;
	u64 deadline;
	bool ok = true;

	size_t before, after;

	racers_ready = 0;
	racers_done  = 0;
	race_go      = 0;
	race_release = 0;
	pages_stolen = 0;
	pages_missed = 0;

	for (i = 0; i < PMM_RACERS; i++)
		if (!thread_create("pmm-race", racer, (void *)(uintptr_t)i)) {
			kputs("  pmm: could not create the threads to race "
			      "with\n");
			return false;
		}

	/* Every racer running, and only then does the window open. */
	deadline = time_monotonic_ns() + 5000000000ULL;
	while (__atomic_load_n(&racers_ready, __ATOMIC_ACQUIRE) < PMM_RACERS &&
	       time_monotonic_ns() < deadline)
		sched_yield();

	if (__atomic_load_n(&racers_ready, __ATOMIC_ACQUIRE) < PMM_RACERS) {
		kprintf("  pmm: only %u of %u threads reached the start\n",
			racers_ready, (unsigned)PMM_RACERS);
		__atomic_store_n(&race_go, 1, __ATOMIC_RELEASE);
		__atomic_store_n(&race_release, 1, __ATOMIC_RELEASE);
		return false;
	}

	before = free_pages;
	__atomic_store_n(&race_go, 1, __ATOMIC_RELEASE);

	/* Bounded, and the bound is checked rather than assumed -- a test that
	 * waits forever for a thread that never ran is a hang, and a hang is
	 * the least informative failure there is. */
	deadline = time_monotonic_ns() + 5000000000ULL;
	while (__atomic_load_n(&racers_done, __ATOMIC_ACQUIRE) < PMM_RACERS &&
	       time_monotonic_ns() < deadline)
		sched_yield();

	if (__atomic_load_n(&racers_done, __ATOMIC_ACQUIRE) < PMM_RACERS) {
		kprintf("  pmm: only %u of %u threads finished\n",
			racers_done, (unsigned)PMM_RACERS);
		__atomic_store_n(&race_release, 1, __ATOMIC_RELEASE);
		return false;
	}

	/* Read while they are all still parked, and then let them go. */
	after = free_pages;
	__atomic_store_n(&race_release, 1, __ATOMIC_RELEASE);

	if (pages_stolen) {
		kprintf("  pmm: %u page(s) were handed to two owners at once\n",
			pages_stolen);
		ok = false;
	}

	/* And then the bookkeeping, which is the assertion that actually
	 * catches an unlocked allocator -- the ownership check above is not.
	 * The first version of this test had only that one, and it passed
	 * every time with the lock removed, because two processors landing on
	 * the *same bit* in the same instant is rare.
	 *
	 * Losing an *update* is not rare. bit_set and bit_clear are
	 * read-modify-write on a shared byte, and free_pages is a shared
	 * counter that both of them touch, so two processors anywhere in the
	 * same eight pages lose one another's changes. Every racer frees
	 * exactly what it took, so the count has to come back to where it
	 * started; drift either way is a page leaked or a page handed out
	 * twice. */
	if (after != before) {
		kprintf("  pmm: %lu pages free before and %lu after, with "
			"every allocation freed -- the bitmap lost updates\n"
			, (unsigned long)before, (unsigned long)after);
		ok = false;
	}

	/* And that the test did the work it claims to have done. Every thread
	 * finishing is not the same as every thread allocating: if the machine
	 * had no memory, all four would finish having done nothing and the
	 * absence of a collision would mean nothing at all. */
	if (pages_missed > (PMM_RACERS * PMM_ROUNDS) / 4) {
		kprintf("  pmm: %u of %u allocations found no memory, so this "
			"proved little\n", pages_missed,
			(unsigned)(PMM_RACERS * PMM_ROUNDS));
		ok = false;
	}

	return ok;
}
