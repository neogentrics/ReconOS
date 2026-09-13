/* See evict.h. The list of ways this can be wrong is there; this is what falls
 * out of it. */
#include <recon/kernel/evict.h>
#include <recon/kernel/swap.h>
#include <recon/kernel/pageage.h>
#include <recon/kernel/addrspace.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/time.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/arch.h>

/* One lock across choosing a page and replacing its entry.
 *
 * Anomaly 7: two processors that both find the same page cold, both write it
 * out and both replace the entry leak one slot and free the other twice. The
 * window is between reading the entry and writing the token, so the lock has to
 * span both -- and the swap write sits inside it, which is the one place in this
 * kernel where a lock is deliberately held across a write to storage.
 *
 * That is a real cost and it is the lesser one. Dropping the lock for the write
 * means re-checking the entry afterwards and unwinding if it changed, which is a
 * second correctness problem to have instead of a first performance one. When
 * eviction is hot enough to measure, this comment is where to start.
 */
static struct spinlock evict_lock = SPINLOCK_INIT("evict");

/* Anomaly 14: nothing on this path may allocate. Set while evicting, checked by
 * anything that might recurse into it. */
static volatile bool evicting;

static u64 evicted, faulted_back, dropped_pages;

/* Every refusal has a counter, so that "this cannot happen" is a claim with
 * evidence rather than a comment. The numbers are printed whether they are zero
 * or not -- a counter nobody can see is a counter nobody reads. */
static u64 refused_zero_page;		/* 1 */
static u64 refused_not_mapped;
static u64 refused_hot;
static u64 refused_entry_kind;		/* large page, table, kernel half */
static u64 swap_full;			/* 11 */
static u64 write_failed;		/* 12 */
static u64 read_failed;			/* 13 */
static u64 recursion_refused;		/* 14 */
static u64 impossible_present_token;	/* 16 */
static u64 impossible_zero_slot;	/* 17 */
static u64 impossible_unallocated;	/* 18 */
static u64 impossible_no_region;	/* 19 */
static u64 freed_nothing;		/* 20 */

/* Anomaly 15: thrashing. When a page was evicted, and whether it came back
 * almost immediately. One number, because a policy that acts on it does not
 * exist yet and a ring of timestamps nobody reads is memory spent on nothing. */
static u64 last_evict_tick;
static u64 quick_returns;

bool evict_available(void)
{
	return swap_present();
}

unsigned evict_pages(struct addrspace *as, unsigned want)
{
	unsigned freed = 0;
	unsigned i;
	u64 flags;
	u64 free_before;

	if (!as || !want || !swap_present())
		return 0;

	/* Anomaly 14. A caller reaching here from inside eviction is a caller
	 * that has just added an allocation to this path. Refused rather than
	 * recursed, because the recursion would be the *second* symptom. */
	if (evicting) {
		recursion_refused++;
		return 0;
	}

	/* The walk reads the root the processor is using, so a space that is
	 * not loaded would be somebody else's tables reported as this one's.
	 * Checked rather than documented: the wrong answer here looks entirely
	 * reasonable. */
	if (addrspace_active() != as)
		return 0;

	free_before = pmm_free_page_count();

	flags = spin_lock_irq(&evict_lock);
	evicting = true;

	for (i = 0; i < as->region_count && i < AS_REGIONS_MAX && freed < want;
	     i++) {
		const struct as_region *r = &as->regions[i];
		vaddr_t va;

		for (va = r->start; va < r->end && freed < want;
		     va += PAGE_SIZE) {
			paddr_t have = vm_lookup(va);
			swap_slot_t slot;

			/* Anomaly 2 is answered here by *absence*: shared
			 * memory is mapped with addrspace_map, which creates no
			 * region, so a walk over regions never reaches one.
			 * That is structural rather than a check, and it is
			 * written down because a later change that gave shared
			 * memory a region would silently make it evictable. */

			if (!have) {
				refused_not_mapped++;
				continue;
			}

			/* Anomaly 1. Every untouched readable page in every
			 * space points here. */
			if (have == addrspace_zero_page()) {
				refused_zero_page++;
				continue;
			}

			/* The policy, such as it is: cold pages only. Read
			 * without clearing -- a sweep clears, and doing it here
			 * as well would mean eviction destroyed the very
			 * measurement it is deciding from. */
			if (vm_page_touched(va, false)) {
				refused_hot++;
				continue;
			}

			slot = swap_write_page(phys_to_virt(have));

			if (slot == SWAP_NONE) {
				/* Anomaly 11 and 12 are different and are
				 * counted apart: a full store means stop, and a
				 * failed write means try the next page. Both
				 * leave this page mapped and intact, which is
				 * the property that matters. */
				if (swap_present())
					swap_full++;
				else
					write_failed++;
				goto done;
			}

			/* Anomaly 12 again, from the other side: if the entry
			 * cannot be replaced, the slot goes back. A slot
			 * holding a page that is still mapped is a slot
			 * nothing will ever free. */
			if (!vm_swap_out(va, slot)) {
				refused_entry_kind++;
				swap_free(slot);
				continue;
			}

			pmm_free_page(have);
			freed++;
			evicted++;
			last_evict_tick = time_ticks();
		}
	}

done:
	evicting = false;
	spin_unlock_irq(&evict_lock, flags);

	/* Anomaly 20. Eviction frees pages, so the count must move. A run that
	 * reports having evicted pages while the count stood still did work for
	 * no benefit, and every other number here would still look right. */
	if (freed && pmm_free_page_count() <= free_before)
		freed_nothing++;

	return freed;
}

bool evict_fault_in(vaddr_t va, unsigned region_flags)
{
	vaddr_t page = va & ~(vaddr_t)(PAGE_SIZE - 1);
	u32 slot = vm_swap_slot(page);
	paddr_t fresh;
	u64 flags;
	bool ok;

	/* Anomaly 17. Slot zero is never handed out, so a token naming it means
	 * an entry was partly written. */
	if (slot == 0) {
		impossible_zero_slot++;
		return false;
	}

	fresh = pmm_alloc_page();
	if (!fresh) {
		/* No memory to read into, while faulting a page back in
		 * because there was no memory. Nothing to do but fail: evicting
		 * more here would be eviction inside a fault inside eviction. */
		return false;
	}

	flags = spin_lock_irq(&evict_lock);

	/* Read under the lock, because anomaly 8 is a processor evicting this
	 * very page between the token being read and the page being mapped. */
	ok = swap_read_page(slot, phys_to_virt(fresh));

	if (!ok) {
		/* Anomaly 13 and 18. The page is gone and the only copy is the
		 * one that did not read. **A zeroed page would be worse than
		 * the failure** -- it is a program carrying on with silently
		 * corrupted memory, which is what this whole file exists to
		 * prevent. The caller ends the program. */
		spin_unlock_irq(&evict_lock, flags);
		pmm_free_page(fresh);
		read_failed++;
		return false;
	}

	if (!vm_map(page, fresh, PAGE_SIZE, region_flags | VM_USER)) {
		spin_unlock_irq(&evict_lock, flags);
		pmm_free_page(fresh);
		return false;
	}

	/* Only now. A slot freed before the page is mapped is a slot another
	 * eviction can take while this fault is still using it. */
	swap_free(slot);

	faulted_back++;

	/* Anomaly 15. Not acted on -- there is no policy to act with -- but
	 * counted, so the first person to watch this machine crawl has a number
	 * rather than a suspicion. */
	if (time_ticks() - last_evict_tick <= 1)
		quick_returns++;

	spin_unlock_irq(&evict_lock, flags);
	return true;
}

void evict_release_space(struct addrspace *as)
{
	unsigned i;

	/* Anomaly 10. A slot leaked here is a slot nothing will ever reuse, and
	 * a swap device that fills up over a machine's lifetime with nothing to
	 * say why. */
	struct addrspace *was;
	u64 irq;

	if (!as || !swap_present())
		return;

	/* A space being torn down is usually not the loaded one, and the walk
	 * reads the root the processor is using -- so it is pointed here for
	 * the length of the walk and put back.
	 *
	 * Interrupts off across the whole of it, for exactly the reason
	 * addrspace_map gives: the processor is looking at a map that is not
	 * the running thread's, and a tick landing in that window would
	 * schedule somebody else into it. */
	irq = arch_irq_save();
	was = addrspace_active();
	addrspace_activate(as);

	for (i = 0; i < as->region_count && i < AS_REGIONS_MAX; i++) {
		const struct as_region *r = &as->regions[i];
		vaddr_t va;

		for (va = r->start; va < r->end; va += PAGE_SIZE) {
			u32 slot = vm_swap_slot(va);

			if (slot)
				swap_free(slot);
		}
	}

	addrspace_activate(was);
	arch_irq_restore(irq);
}

void evict_print_summary(void)
{
	kprintf("\nEviction\n");

	if (!swap_present()) {
		kputs("  policy       : nothing to evict into, so nothing is "
		      "evicted\n");
		return;
	}

	kprintf("  policy       : cold pages first, everything written out, "
		"below %u free pages\n", (unsigned)EVICT_LOW_WATER);
	kprintf("  moved        : %lu evicted, %lu faulted back, %lu of those "
		"within a tick\n", evicted, faulted_back, quick_returns);
	kprintf("  refused      : %lu hot, %lu shared zeroes, %lu unmapped, "
		"%lu wrong kind of entry\n", refused_hot, refused_zero_page,
		refused_not_mapped, refused_entry_kind);

	/* Printed whether or not any of them fired. A counter nobody can see is
	 * a counter nobody reads, and these are the ones that mean something
	 * above has already gone wrong. */
	kprintf("  failures     : %lu store full, %lu writes failed, %lu reads "
		"failed\n", swap_full, write_failed, read_failed);
	kprintf("  impossible   : %lu present-and-token, %lu zero slot, "
		"%lu unallocated, %lu no region, %lu freed nothing, "
		"%lu recursions\n",
		impossible_present_token, impossible_zero_slot,
		impossible_unallocated, impossible_no_region, freed_nothing,
		recursion_refused);
}

void evict_note_impossible_no_region(void)  { impossible_no_region++; }
void evict_note_impossible_present(void)    { impossible_present_token++; }
void evict_note_impossible_unallocated(void) { impossible_unallocated++; }
void evict_note_dropped(void)               { dropped_pages++; }

/* --- the self-test --------------------------------------------------------
 *
 * "It evicted a page" is not the assertion. A policy that evicted a page and
 * lost it would satisfy that, and so would one that evicted a page and handed
 * back zeroes -- which is the outcome this file most exists to prevent.
 *
 * The assertion is that **the bytes come back**. A page is filled with a pattern
 * that could not be there by accident, evicted, verified to be genuinely gone
 * from the page tables, and then touched -- and every word of it has to be what
 * was written.
 *
 * The middle step is the one that makes it a test of eviction rather than a test
 * of memory. Without checking that the mapping actually went, a policy that did
 * nothing at all would pass: the page would still be mapped, the touch would not
 * fault, and the contents would of course be intact.
 */
bool evict_self_test(void)
{
	struct addrspace *as;
	struct addrspace *was;
	vaddr_t at = 0x60000000;
	volatile u32 *p;
	unsigned i;
	unsigned n;
	u64 back_before;
	bool ok = true;

	if (!swap_present()) {
		kputs("  evict: no store on this machine, so nothing can be "
		      "evicted\n");
		return true;
	}

	as = addrspace_create();
	if (!as) {
		kputs("  evict: no address space\n");
		return false;
	}

	if (!addrspace_reserve(as, at, PAGE_SIZE,
			       VM_READ | VM_WRITE | VM_USER)) {
		addrspace_release(as);
		kputs("  evict: could not reserve a page\n");
		return false;
	}

	was = addrspace_active();
	addrspace_activate(as);

	/* Written end to end, so that a store writing only the first block of a
	 * page is caught -- a page is more than one block on most devices. */
	p = (volatile u32 *)(uintptr_t)at;
	for (i = 0; i < PAGE_SIZE / sizeof(u32); i++)
		p[i] = 0x45564943u ^ i;		/* "EVIC" */

	/* Cold, as far as the policy is concerned. The write above set the
	 * flag, so it has to be cleared or nothing here is evictable -- and
	 * clearing it through the same call a sweep uses is the point: this is
	 * the measurement deciding, not the test. */
	vm_page_touched(at, true);

	back_before = faulted_back;

	n = evict_pages(as, 1);

	if (n != 1) {
		kprintf("  evict: asked for one page and got %u\n", n);
		ok = false;
	}

	/* Genuinely gone. Without this the rest of the test passes on a policy
	 * that did nothing whatever. */
	if (vm_lookup(at)) {
		kputs("  evict: the page is still mapped, so nothing was "
		      "evicted\n");
		ok = false;
	}

	/* And the entry says where it went, rather than being empty. An empty
	 * entry is the "never mapped" case and the fault would hand back a
	 * fresh page of zeroes. */
	if (ok && vm_swap_slot(at) == 0) {
		kputs("  evict: the entry does not name a swap slot\n");
		ok = false;
	}

	/* The touch that brings it back. */
	if (ok) {
		for (i = 0; i < PAGE_SIZE / sizeof(u32); i++)
			if (p[i] != (0x45564943u ^ i)) {
				kprintf("  evict: word %u came back as 0x%x "
					"rather than 0x%x\n", i, p[i],
					0x45564943u ^ i);
				ok = false;
				break;
			}
	}

	/* And it came back through the fault path rather than having quietly
	 * still been there. */
	if (ok && faulted_back == back_before) {
		kputs("  evict: the page was readable without ever being "
		      "faulted back\n");
		ok = false;
	}

	evict_release_space(as);
	addrspace_activate(was);
	addrspace_release(as);
	return ok;
}
