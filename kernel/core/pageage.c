/* See pageage.h. This measures; it decides nothing. */
#include <recon/kernel/pageage.h>
#include <recon/kernel/addrspace.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>

static u64 sweeps;
static u64 pages_sampled, pages_touched;

/* The largest sweep this will do in one go.
 *
 * A sweep clears the bits it reads, and on a processor with no hardware update
 * that means a fault on the next touch of every page it cleared. Sweeping an
 * enormous space in one pass is therefore not just slow -- it is a burst of
 * faults arriving all at once, at whatever moment the sweep happened to run.
 *
 * A visible cap, so that the cost of measuring is bounded and stated rather
 * than proportional to how much memory a program happens to have. */
#define SWEEP_PAGES_MAX 4096

bool page_age_sweep(struct addrspace *as, struct page_age_report *out)
{
	unsigned i;

	if (!as || !out)
		return false;

	kmemset(out, 0, sizeof(*out));

	/* THE SPACE HAS TO BE THE ACTIVE ONE.
	 *
	 * Reading a descriptor means walking from the root the processor is
	 * using, and this kernel's walk takes that root from the register
	 * rather than from the structure. Sweeping a space that is not loaded
	 * would read another program's tables and report its pages as these --
	 * numbers that look entirely reasonable and describe the wrong process.
	 *
	 * Checked rather than documented, because a caller that got it wrong
	 * would get plausible output. */
	if (addrspace_active() != as)
		return false;

	for (i = 0; i < as->region_count && i < AS_REGIONS_MAX; i++) {
		const struct as_region *r = &as->regions[i];
		vaddr_t va;

		for (va = r->start; va < r->end; va += PAGE_SIZE) {
			if (out->sampled >= SWEEP_PAGES_MAX)
				break;

			/* Nothing mapped here yet. Not counted at all: a
			 * reserved-but-untouched page is not a cold page, it is
			 * a page that does not exist, and counting it as cold
			 * would make every program look mostly idle. */
			if (!vm_lookup(va))
				continue;

			out->sampled++;

			if (r->file)
				out->file_backed++;
			else
				out->anonymous++;

			if (vm_page_touched(va, true))
				out->touched++;
			else
				out->cold++;
		}
	}

	sweeps++;
	pages_sampled += out->sampled;
	pages_touched += out->touched;

	return out->sampled != 0;
}

void page_age_print_summary(void)
{
	kprintf("\nPage age\n");
	kprintf("  sampling     : %s\n",
		vm_page_age_is_cheap()
		? "the processor keeps the flag, so a sweep costs a walk"
		: "no hardware update, so every sampled page faults once");

	kprintf("  sweeps       : %lu, %lu page(s) sampled, %lu found touched\n",
		sweeps, pages_sampled, pages_touched);

	/* What the measuring itself cost. On a processor with no hardware
	  * update this is one trap per sampled page, and it is the number that
	  * decides how often a sweep is affordable. */
	kprintf("  cost         : %lu fault(s) taken to keep this count\n",
		vm_page_age_faults());

	/* Deliberately no verdict. There is no policy yet, so there is nothing
	 * these numbers are good or bad for -- and a summary that graded them
	 * would be inventing the policy in a printf. */
}

/* --- the self-test --------------------------------------------------------
 *
 * Instrumentation still has to be right, and "it reported some numbers" is not
 * evidence that it is. What can be asserted, without any policy existing, is
 * that the measurement distinguishes the two cases it claims to:
 *
 *   a page that was touched since the last sweep reports touched;
 *   a page that was not reports cold.
 *
 * Both halves are needed. An implementation that always said "touched" would
 * pass the first; one that always said "cold" would pass the second. Together
 * they can only be satisfied by something that actually reads the bit.
 *
 * And the third assertion is the one that catches the mistake that matters most
 * on x86_64: a sweep that clears the flag without invalidating the translation
 * leaves the processor using a cached entry, so it never writes the flag again
 * and **every page looks cold from the second sweep onward**. That is a silent
 * wrong answer, and it is only visible if you touch a page and sweep twice.
 */
bool page_age_self_test(void)
{
	struct addrspace *as;
	struct addrspace *was;
	struct page_age_report first, second;
	vaddr_t hot = 0x50000000;
	vaddr_t cold = 0x50010000;
	volatile u8 *p;
	bool ok = true;

	as = addrspace_create();
	if (!as) {
		kputs("  pageage: no address space to sweep\n");
		return false;
	}

	if (!addrspace_reserve(as, hot, PAGE_SIZE, VM_READ | VM_WRITE | VM_USER) ||
	    !addrspace_reserve(as, cold, PAGE_SIZE, VM_READ | VM_WRITE | VM_USER)) {
		addrspace_release(as);
		kputs("  pageage: could not reserve two pages\n");
		return false;
	}

	was = addrspace_active();
	addrspace_activate(as);

	/* Both faulted in, so both exist and both have been touched by the act
	 * of creating them. */
	p = (volatile u8 *)(uintptr_t)hot;
	p[0] = 1;
	p = (volatile u8 *)(uintptr_t)cold;
	p[0] = 1;

	/* First sweep: clears whatever the faulting left set. Its counts are
	 * not asserted -- what it is for is putting both pages in a known
	 * state, and asserting the state it found would be asserting how the
	 * fault handler happens to touch memory. */
	page_age_sweep(as, &first);

	/* Now one of them, and only one. */
	p = (volatile u8 *)(uintptr_t)hot;
	p[0] = 2;

	if (!page_age_sweep(as, &second)) {
		kputs("  pageage: the second sweep found nothing mapped\n");
		ok = false;
	} else {
		if (second.touched < 1) {
			kputs("  pageage: a page that was written since the "
			      "last sweep reported cold\n");
			ok = false;
		}

		if (second.cold < 1) {
			kputs("  pageage: a page that was not touched since "
			      "the last sweep reported touched\n");
			ok = false;
		}

		if (second.sampled != second.touched + second.cold) {
			kprintf("  pageage: %u sampled but %u touched and %u "
				"cold\n", second.sampled, second.touched,
				second.cold);
			ok = false;
		}

		/* Anonymous, both of them, because neither was mapped from a
		 * file. A count that said otherwise would mean the cost side of
		 * the measurement is reading the wrong field. */
		if (second.file_backed != 0 ||
		    second.anonymous != second.sampled) {
			kprintf("  pageage: %u file-backed and %u anonymous of "
				"%u\n", second.file_backed, second.anonymous,
				second.sampled);
			ok = false;
		}
	}

	addrspace_activate(was);
	addrspace_release(as);
	return ok;
}
