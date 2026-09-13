/* Choosing which page goes.
 *
 * The store landed first, the measurement second, and this is the half that
 * decides. It is the dangerous one: the store either writes a page or reports
 * that it could not, and the measurement either reports a number or reports
 * zero, but a wrong *decision* here silently hands a program somebody else's
 * memory, or loses its own, and every value in the machine stays plausible while
 * it happens.
 *
 * So most of this header is a list of the ways it can be wrong. That list is the
 * design; the code is what falls out of it.
 *
 * ==========================================================================
 * THE POLICY
 * ==========================================================================
 *
 * When free memory falls below a watermark, sweep the active address space for
 * pages the processor says have not been touched since the last sweep, write
 * them to swap, and replace their page-table entries with a token naming the
 * slot. A later touch faults, reads the page back, and maps it again.
 *
 * Cold-first, and nothing cleverer. Not because a cleverer policy would not be
 * better -- second-chance, working-set and LRU approximations all exist and all
 * beat this -- but because the only honest reason to prefer one is a measurement
 * comparing them, and there is one boot's worth of page-age data in this kernel
 * so far. A policy chosen from a paper rather than from this machine would be a
 * guess wearing a citation.
 *
 * **Everything goes to swap. Nothing is dropped.** A clean page backed by a file
 * could be discarded and read again for free, and that is a real and worthwhile
 * optimisation -- and it needs to know the page is *clean*. x86_64 keeps a dirty
 * bit; aarch64 before ARMv8.2 does not, and software dirty tracking is a second
 * fault-driven mechanism with its own failure modes. Guessing wrong loses a
 * program's writes with no error anywhere, so until dirty tracking exists,
 * everything is written out. The cost is swap traffic; the alternative is data
 * loss.
 *
 * ==========================================================================
 * WHAT MUST NEVER BE EVICTED, AND WHY EACH ONE WOULD BE FATAL
 * ==========================================================================
 *
 * Each of these is refused by construction, and each also has a counter -- so
 * that "this cannot happen" is a claim with evidence rather than a comment.
 *
 *  1. THE SHARED ZERO PAGE. Every untouched readable page in every address
 *     space points at one physical page. Evicting it would swap out the zeroes
 *     that half the machine is reading, and the fault-in would hand one program
 *     a private copy while the rest kept reading a page that no longer exists.
 *
 *  2. A PAGE OF SHARED MEMORY. Two address spaces point at it and this code can
 *     only see one of them. Unmapping it here leaves the other space pointing at
 *     a physical page that has been freed and reused -- silent corruption in a
 *     process that was never touched.
 *
 *  3. A PAGE TABLE. Walking a table to evict the table you are walking is the
 *     obvious version; the subtle one is that a table is reachable from the root
 *     register and not from any region, so nothing would fault it back in.
 *
 *  4. ANYTHING IN THE KERNEL'S HALF. The kernel does not fault its own pages in
 *     and would not survive the attempt. The sweep only walks a space's regions,
 *     which are user addresses by construction, and the check is still made.
 *
 *  5. THE STACK OF THE THREAD DOING THE EVICTING. It is in the kernel's half so
 *     rule 4 covers it, and it is listed separately because it is the one that
 *     would look like a hang rather than a fault.
 *
 *  6. A PAGE WITH A DEVICE TRANSFER IN FLIGHT. No driver in this kernel does DMA
 *     into user memory -- every one of them copies through a kernel buffer -- so
 *     this cannot happen today. It is written down because the day one does, this
 *     list is where the answer has to be added, and a subsystem that never
 *     enumerated it would find out from corruption.
 *
 * ==========================================================================
 * THE RACES
 * ==========================================================================
 *
 *  7. TWO PROCESSORS EVICTING THE SAME PAGE. Both find it cold, both write it to
 *     swap, both replace the entry -- and one slot is leaked while the other is
 *     freed twice. One lock, held across choosing and replacing.
 *
 *  8. EVICTING A PAGE ANOTHER PROCESSOR IS FAULTING IN. The fault-in path takes
 *     the same lock, so one of them goes second: either the page is still there
 *     and the fault is spurious, or the entry is a token and the fault reads it
 *     back.
 *
 *  9. EVICTING A PAGE THE KERNEL IS IN THE MIDDLE OF READING. A system call
 *     copying a program's buffer touches user memory directly. If the page goes
 *     between the range check and the copy, the copy faults -- in the kernel, on
 *     a user address, which both architectures already route to vm_fault_user.
 *     It reads back and the copy continues. This works only because that path
 *     existed first; it is the reason eviction did not have to invent one.
 *
 * 10. A PROCESS DYING WITH PAGES IN SWAP. Its slots must go back. They are freed
 *     when the address space is torn down, walking the same regions -- and a
 *     slot leaked here is a slot nothing will ever reuse, which is a swap device
 *     that fills up over a machine's lifetime with no way to see why.
 *
 * ==========================================================================
 * THE FAILURES
 * ==========================================================================
 *
 * 11. SWAP IS FULL. Eviction stops and says so. It does not spin looking for a
 *     slot that is not coming, and it does not evict fewer pages than it needs
 *     and report success.
 *
 * 12. THE SWAP WRITE FAILS. The page is left exactly where it was, mapped and
 *     intact. A page whose write failed and whose mapping was replaced anyway is
 *     a program that reads zeroes from its own memory.
 *
 * 13. THE SWAP READ FAILS ON THE WAY BACK. This one cannot be recovered from:
 *     the page is gone and the only copy was the one that did not read. The
 *     program is ended, with a message that says so. **Returning a zeroed page
 *     would be worse than the crash** -- it is a program continuing with silently
 *     corrupted memory, which is the outcome this whole file exists to avoid.
 *
 * 14. EVICTION RECURSING INTO ITSELF. Nothing on this path allocates: the swap
 *     write does not, the entry replacement does not, and the page being freed
 *     is a free rather than an allocation. Asserted with a flag, because "does
 *     not allocate" is a property that survives exactly as long as nobody adds a
 *     kmalloc to a helper.
 *
 * 15. THRASHING. Pages evicted and immediately faulted back. This is not a
 *     failure the machine can detect from one event, and it is the one whose only
 *     symptom is duration -- so it is *counted*: how many faulted back within a
 *     tick of leaving. Nothing acts on the number yet. It exists so that the
 *     first person to see this machine crawl has something to read.
 *
 * ==========================================================================
 * THE ONES THAT SHOULD BE IMPOSSIBLE
 * ==========================================================================
 *
 * Each of these means something above has already gone wrong. They are checked
 * anyway, because the alternative to noticing is corruption, and each has its own
 * counter so a machine that hits one says which.
 *
 * 16. AN ENTRY THAT IS BOTH PRESENT AND A SWAP TOKEN. The encoding makes the
 *     token depend on the entry being absent, so this means memory was corrupted
 *     or two writers raced. Refused, page left alone.
 *
 * 17. A SWAP TOKEN OF ZERO. Slot zero is never handed out precisely so that a
 *     zeroed entry cannot read as a valid token. Finding one means an entry was
 *     partly written.
 *
 * 18. A SWAP TOKEN NAMING A SLOT THAT IS NOT ALLOCATED. The store refuses the
 *     read, and this is where that refusal is turned into an ended program rather
 *     than a page of whatever the partition held.
 *
 * 19. A SWAP TOKEN IN AN ADDRESS WITH NO REGION. The address was never the
 *     program's, so nothing should have evicted it. Refused.
 *
 * 20. THE FREE PAGE COUNT GOING UP WHILE EVICTING. Eviction frees pages, so it
 *     should only rise -- but if a sweep reports having evicted pages and the
 *     count did not move, nothing was actually freed and the whole exercise did
 *     work for no benefit. Counted, because it is the difference between a policy
 *     that works and one that only appears to.
 */
#ifndef RECON_KERNEL_EVICT_H
#define RECON_KERNEL_EVICT_H

#include <recon/kernel/types.h>

struct addrspace;

/* Below this many free pages, eviction is worth doing. A visible number, and
 * deliberately not a fraction of memory: the question "is there enough left to
 * get through a fault" has an absolute answer in pages, and a percentage of a
 * large machine is a number nobody chose. */
#define EVICT_LOW_WATER 512

/* Tries to free `want` pages out of the *active* address space.
 *
 * Returns how many it actually evicted, which may be fewer -- including zero.
 * Zero is not an error: a space whose pages are all hot, or all ineligible, has
 * nothing to give, and reporting failure would make a caller retry something
 * that will not work.
 *
 * The space must be the active one, for the same reason a sweep must be: the
 * walk reads the root the processor is using. */
unsigned evict_pages(struct addrspace *as, unsigned want);

/* Whether eviction is possible at all: there is a store, and it has room. */
bool evict_available(void);

/* Called by the fault path when an address has a swap token rather than a
 * mapping. Reads the page back and maps it.
 *
 * False means the program cannot continue -- the read failed, or the token was
 * one of the impossible ones -- and the caller must end it rather than carry on
 * with a page of zeroes. */
bool evict_fault_in(vaddr_t va, unsigned region_flags);

/* Frees every swap slot an address space still holds. Called once, when it is
 * torn down. */
void evict_release_space(struct addrspace *as);

void evict_print_summary(void);
bool evict_self_test(void);

/* --- what the architecture provides -------------------------------------- */

/* Replaces the mapping at `va` with a token naming a swap slot, and invalidates
 * the translation. The physical page is the caller's to free afterwards.
 *
 * False if there was nothing mapped there, or if the entry is not one this may
 * touch -- a large page, or a table. */
bool vm_swap_out(vaddr_t va, u32 slot);

/* The slot named by the entry at `va`, or zero if it is not a swap token.
 *
 * Zero is unambiguous because slot zero is never handed out, which is why the
 * store reserves it. */
u32 vm_swap_slot(vaddr_t va);

#endif /* RECON_KERNEL_EVICT_H */
