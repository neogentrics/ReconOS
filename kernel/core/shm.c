/* Memory two programs can both reach.
 *
 * A pipe moves bytes; this shares the place they live. They are the two halves
 * of the IPC row and they are not variations on each other -- a pipe is a
 * stream with an order and a producer and a consumer, and shared memory is a
 * region with no order at all, where the only thing the kernel promises is that
 * both programs are looking at the same physical pages.
 *
 * --- Why it could not exist before, and why it is small now ---
 *
 * Before per-process address spaces, sharing memory between processes was not a
 * mechanism: every process shared *all* of it. The audit said exactly that. Now
 * that each has a map of its own, sharing is a thing you can ask for -- and
 * because the map already knows how to point at a physical page, asking for it
 * is the only part that had to be built.
 *
 * So this file is a lifetime and nothing else. The pages are ordinary pages from
 * the ordinary allocator; the mapping is the ordinary mapping call. What is here
 * is the answer to "when do these pages go back", which is: when the last space
 * that mapped them has gone.
 *
 * --- Mapped eagerly, and that is deliberate ---
 *
 * Every page is mapped when the region is attached, rather than faulted in on
 * first touch. Demand paging exists and is not used here, because the thing
 * being shared is the *page*: two programs faulting on the same shared address
 * must arrive at one page, and a demand-paged region hands each faulter a page
 * of its own unless something coordinates them. That coordination is the whole
 * difficulty, and mapping up front does not have it.
 *
 * The cost is that a shared region is resident from the moment it is attached.
 * For the sizes anything here asks for, that is the right trade; when something
 * wants a large one, this comment is where to start.
 *
 * --- No system call yet, and that is on purpose ---
 *
 * There is no `shm_open`. A system call invented before its first caller gets
 * its arguments wrong in a way that is expensive to change afterwards, and the
 * shape of this one -- named or anonymous, inherited or looked up -- is decided
 * by whatever wants it first. The mechanism is here and tested; the call arrives
 * with its caller.
 */
#include <recon/kernel/shm.h>
#include <recon/kernel/addrspace.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>

static struct spinlock shm_lock = SPINLOCK_INIT("shm");
static u64 regions_made, pages_held;

struct shm {
	unsigned refs;
	unsigned pages;
	paddr_t page[SHM_PAGES_MAX];
};

struct shm *shm_create(u64 bytes)
{
	struct shm *s;
	u64 want = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
	unsigned i;

	if (!want || want > SHM_PAGES_MAX)
		return NULL;

	s = kzalloc(sizeof(*s));
	if (!s)
		return NULL;

	s->refs = 1;

	/* One page at a time rather than one run, because a shared region does
	 * not need to be physically contiguous and asking for a run that big
	 * fails on a fragmented machine for no reason. */
	for (i = 0; i < want; i++) {
		s->page[i] = pmm_alloc_page();

		if (!s->page[i]) {
			while (i--)
				pmm_free_page(s->page[i]);

			kfree(s);
			return NULL;
		}
	}

	s->pages = (unsigned)want;

	{
		u64 flags = spin_lock_irq(&shm_lock);

		regions_made++;
		pages_held += want;
		spin_unlock_irq(&shm_lock, flags);
	}

	return s;
}

struct shm *shm_hold(struct shm *s)
{
	if (s)
		__atomic_add_fetch(&s->refs, 1, __ATOMIC_RELAXED);

	return s;
}

void shm_release(struct shm *s)
{
	unsigned i;

	if (!s)
		return;

	if (__atomic_sub_fetch(&s->refs, 1, __ATOMIC_ACQ_REL) != 0)
		return;

	/* The last holder. The pages go back now -- and they go back cleared by
	 * the allocator on the way out, not on the way in, so whatever two
	 * programs were saying to each other does not become the next
	 * program's first page of memory. */
	for (i = 0; i < s->pages; i++)
		pmm_free_page(s->page[i]);

	{
		u64 flags = spin_lock_irq(&shm_lock);

		pages_held -= s->pages;
		spin_unlock_irq(&shm_lock, flags);
	}

	kfree(s);
}

u64 shm_size(const struct shm *s)
{
	return s ? (u64)s->pages * PAGE_SIZE : 0;
}

bool shm_attach(struct addrspace *as, vaddr_t at, struct shm *s, unsigned flags)
{
	unsigned i;

	if (!as || !s || (at & (PAGE_SIZE - 1)))
		return false;

	for (i = 0; i < s->pages; i++) {
		if (!addrspace_map(as, at + (vaddr_t)i * PAGE_SIZE,
				   s->page[i], PAGE_SIZE, flags | VM_USER)) {
			/* Half a shared region is worse than none, and there is no
			 * addrspace_unmap to take the rest back out.
			 *
			 * So the contract is stated instead: **a failed attach
			 * leaves the space with part of the region mapped, and the
			 * caller must discard that space rather than reuse it.** The
			 * only way to reach here is the page-table allocation
			 * failing, and a caller that has just run out of memory
			 * building a map is not going to carry on with it.
			 *
			 * Writing an unmap for a path nothing has ever taken would
			 * be a function with no tested caller. When something needs
			 * to detach a live region it will want one for its own sake,
			 * and this can use it then. */
			return false;
		}
	}

	/* The space holds it now. Released when the space is torn down, which
	 * the caller arranges -- see the note in shm.h about why that is the
	 * caller's job and not this function's. */
	shm_hold(s);
	return true;
}

void shm_print_summary(void)
{
	kprintf("  shared memory : %lu region(s) made, %lu page(s) held\n",
		regions_made, pages_held);
}

/* --- the self-test --------------------------------------------------------
 *
 * The assertion is not that a mapping succeeded. It is that a write through one
 * address space is *visible* through another -- which is the only thing shared
 * memory promises and the one thing a mapping that quietly gave each space its
 * own pages would fail.
 *
 * So the test writes through one map and reads through the other, in both
 * directions, and checks a value that could not be there by accident.
 */
bool shm_self_test(void)
{
	struct addrspace *a = NULL, *b = NULL;
	struct shm *s = NULL;
	vaddr_t at = 0x40000000;
	volatile u64 *pa, *pb;
	bool ok = true;

	s = shm_create(2 * PAGE_SIZE);
	if (!s) {
		kputs("  shm: could not make a region\n");
		return false;
	}

	a = addrspace_create();
	b = addrspace_create();

	if (!a || !b) {
		kputs("  shm: could not make two address spaces\n");
		ok = false;
		goto out;
	}

	/* Attached at the *same* address in both, which is not required by
	 * anything here and is what a caller will usually want. */
	if (!shm_attach(a, at, s, VM_READ | VM_WRITE) ||
	    !shm_attach(b, at, s, VM_READ | VM_WRITE)) {
		kputs("  shm: attaching to two spaces failed\n");
		ok = false;
		goto out;
	}

	pa = (volatile u64 *)(uintptr_t)at;
	pb = (volatile u64 *)(uintptr_t)at;

	addrspace_activate(a);
	pa[0] = 0x5348415245440001ULL;			/* "SHARED" */
	pa[1] = 0;

	addrspace_activate(b);

	if (pb[0] != 0x5348415245440001ULL) {
		kputs("  shm: a write in one space was not visible in the "
		      "other\n");
		ok = false;
	}

	/* The other direction, and on the second page, so that a region that
	 * shared only its first page would be caught. */
	{
		volatile u64 *second_b =
			(volatile u64 *)(uintptr_t)(at + PAGE_SIZE);

		second_b[0] = 0x4241434B57415244ULL;	/* "BACKWARD" */

		addrspace_activate(a);

		{
			volatile u64 *second_a =
				(volatile u64 *)(uintptr_t)(at + PAGE_SIZE);

			if (second_a[0] != 0x4241434B57415244ULL) {
				kputs("  shm: the second page was not "
				      "shared\n");
				ok = false;
			}
		}
	}

	addrspace_activate(NULL);

out:
	addrspace_activate(NULL);

	if (a)
		addrspace_release(a);
	if (b)
		addrspace_release(b);

	/* Once for each attach that succeeded, and once for the create. The
	 * counts in the summary are what would show a leak. */
	if (a)
		shm_release(s);
	if (b)
		shm_release(s);

	shm_release(s);
	return ok;
}
