/* How recently a page was touched, and nothing decided from it.
 *
 * This is instrumentation, which is a different thing from a test and a
 * different thing from a feature. It reports numbers the kernel does not
 * currently know, with no assertion about what they should be -- because nobody
 * knows yet. It exists so that when an eviction policy is written, it is chosen
 * from evidence rather than from a guess.
 *
 * --- Why this and not the policy ---
 *
 * A page-eviction policy answers "which page goes". Getting it wrong does not
 * produce a wrong answer: it produces a *slow machine*. Pages evicted and
 * immediately faulted back keep every value correct and every ordering intact
 * while the machine grinds -- and this kernel has already been caught twice by
 * faults whose only symptom was duration, with the whole self-test battery green
 * through both (BG-157, BG-158).
 *
 * So the order is: measure, then choose, then assert against the measurement.
 * Choosing first would mean a policy nothing could argue with.
 *
 * --- What the hardware gives, and what it costs ---
 *
 * Both architectures have a bit the processor sets when a page is touched --
 * `PTE_ACCESSED` on x86_64, the Access Flag on aarch64 -- and both constants
 * have been defined in this kernel since paging was written. **Neither has ever
 * been read.** Sampling means clearing the bit, waiting, and seeing which came
 * back set.
 *
 * The two architectures are not the same shape here, and the difference is
 * expensive rather than cosmetic:
 *
 *   - x86_64 sets the bit in hardware. Clearing it requires invalidating the
 *     translation, because the processor only writes the bit when it fills a
 *     TLB entry -- leave the entry cached and a hot page reports as cold
 *     forever. That is a silent wrong answer, which is the worst kind here.
 *
 *   - aarch64 before ARMv8.1 has no hardware update at all. A valid descriptor
 *     with the Access Flag clear does not get it set on access -- it **faults**,
 *     and software sets it. Cortex-A72, which is what the rig runs, is one of
 *     those. So sampling on this architecture means handling a fault this kernel
 *     has never answered.
 *
 *     The abort decoder could already *describe* three of the four levels of
 *     it, which was assumed otherwise until it was read. That changed nothing:
 *     a named fault is still a dead program, because the name goes into the
 *     report and then the program ends. Being able to say what happened and
 *     being able to do something about it are different things, and only the
 *     first was there.
 *
 * The second is more accurate than the first -- a fault is exact where a bit is
 * a sample -- and it costs one trap per page per sweep. That is the reason a
 * sweep samples rather than clearing everything, and it is a number worth having
 * before anybody designs around it.
 */
#ifndef RECON_KERNEL_PAGEAGE_H
#define RECON_KERNEL_PAGEAGE_H

#include <recon/kernel/types.h>

struct addrspace;

/* One sweep's worth of counts. Every field is a measurement; none is a verdict.
 */
struct page_age_report {
	unsigned sampled;	/* pages looked at */
	unsigned touched;	/* found with the bit set since the last sweep */
	unsigned cold;		/* found with it clear */

	/* What evicting each kind would cost, which is the other half of the
	 * question and is not derivable from the age. A clean page backed by a
	 * file can be dropped and read again; a dirty anonymous page has to be
	 * written somewhere first. */
	unsigned file_backed;
	unsigned anonymous;
};

/* Samples every mapped page of one address space, clears the bits it read, and
 * fills in the report. Returns false if the space has nothing mapped.
 *
 * **Clearing is what makes the next sweep mean anything**, and it is also what
 * costs: on aarch64 every page cleared here will fault on its next access. A
 * caller sweeping constantly is a caller measuring the cost of measuring. */
bool page_age_sweep(struct addrspace *as, struct page_age_report *out);

void page_age_print_summary(void);
bool page_age_self_test(void);

/* --- what the architecture provides -------------------------------------- */

/* Whether the page at `va` has been touched since the bit was last cleared, and
 * clears it if asked.
 *
 * False for an address with nothing mapped, which is the same answer as "not
 * touched" and is deliberate: a caller sweeping a range does not want to know
 * the difference, and one that does can ask vm_lookup.
 *
 * Clearing invalidates the translation, because on x86_64 the processor will
 * not set the bit again while the old entry is cached -- which would make every
 * page look cold from the second sweep onward. */
bool vm_page_touched(vaddr_t va, bool clear);

/* Whether this machine can sample at all, and what it costs when it does.
 *
 * A processor with hardware access-flag update pays nothing; one without pays a
 * fault per page per sweep. Reported rather than assumed, because the answer
 * changes what a sensible sweep interval is by orders of magnitude. */
bool vm_page_age_is_cheap(void);

/* How many faults sampling has cost so far. Zero on a processor that keeps the
 * flag itself; one per sampled page per sweep on one that does not.
 *
 * Both architectures answer, and the one that answers zero is not answering a
 * different question -- it is reporting that the measurement is free there,
 * which is exactly the number a caller deciding how often to sweep needs. */
u64 vm_page_age_faults(void);

#endif /* RECON_KERNEL_PAGEAGE_H */
