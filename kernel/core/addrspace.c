/* Address spaces. See addrspace.h for what one is and what is shared.
 *
 * A fixed table rather than a list, for the same reason the process table and
 * the block layer use one: the cap is a number somebody can see, and running
 * out is a refusal with a message rather than an allocation failure three
 * layers down.
 */
#include <recon/kernel/addrspace.h>
#include <recon/kernel/vfs.h>
#include <recon/kernel/evict.h>

#include <recon/kernel/arch.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/panic.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/smp.h>
#include <recon/kernel/user.h>
#include <recon/kernel/vm.h>

#define ADDRSPACE_MAX 32

static struct addrspace table[ADDRSPACE_MAX];
static struct spinlock table_lock = SPINLOCK_INIT("addrspace");

/* What each processor is translating through. Per processor and not one
 * variable, for the same reason the page tables' own copy of this is: two
 * processors run two programs at the same instant. */
static struct addrspace *active[MAX_CPUS];

struct addrspace *addrspace_create(void)
{
	struct addrspace *as = NULL;
	paddr_t root;
	unsigned i;
	u64 flags;

	/* Outside the lock. Allocating a page can be slow and the table lock is
	 * taken by the scheduler's switch path -- holding it across an
	 * allocation is how a lock that guards a table starts guarding the page
	 * allocator as well. */
	root = arch_as_new_root();
	if (!root)
		return NULL;

	flags = spin_lock_irq(&table_lock);

	for (i = 0; i < ADDRSPACE_MAX; i++)
		if (!table[i].refs) {
			as = &table[i];
			break;
		}

	if (as) {
		/* The whole slot, not the fields this function happens to know
		 * about. It used to name three of them, and what it left behind
		 * was the previous occupant's `regions` -- which are not data.
		 * A region is *permission to touch an address*: it is what
		 * vm_fault_user reads to decide whether a fault is demand paging
		 * or a wild pointer. So a process handed a recycled slot
		 * inherited the last one's permissions, and a stray pointer got
		 * a fresh zero page instead of ending the program. (BG-147)
		 *
		 * The other two tables of this shape already do it this way:
		 * thread_create allocates with kzalloc, process_create clears the
		 * whole entry. This was the one that named fields, which is why
		 * adding one to the structure was enough to make it wrong. */
		kmemset(as, 0, sizeof(*as));

		as->root = root;
		as->refs = 1;
	}

	spin_unlock_irq(&table_lock, flags);

	if (!as)
		arch_as_free_root(root);

	return as;
}

struct addrspace *addrspace_hold(struct addrspace *as)
{
	u64 flags;

	if (!as)
		return NULL;

	flags = spin_lock_irq(&table_lock);
	as->refs++;
	spin_unlock_irq(&table_lock, flags);

	return as;
}

void addrspace_release(struct addrspace *as)
{
	paddr_t root = 0;
	struct file *files[AS_REGIONS_MAX] = { 0 };
	unsigned file_count = 0;
	unsigned i;
	u64 flags;

	if (!as)
		return;

	flags = spin_lock_irq(&table_lock);

	if (as->refs && --as->refs == 0) {
		/* The root is taken out of the entry under the lock and freed
		 * outside it. Freeing page tables walks them, which is slow,
		 * and doing it with the table lock held would stop every
		 * processor's next context switch for the length of a walk. */
		root = as->root;
		as->root = 0;
		as->mapped_bytes = 0;

		/* And the files any of its regions were backed by. Taken out of
		  * the regions under the lock and released outside it, for the
		  * same reason the root is: releasing the last reference to a
		  * file commits it to a disk.
		  *
		  * Without this a mapping would hold its file for the life of the
		  * machine -- and because a slot is reused, the next program to
		  * get this space would inherit the reference as well. That is
		  * the same shape as BG-147. */
		{
			unsigned i;

			for (i = 0; i < as->region_count &&
			     i < AS_REGIONS_MAX; i++) {
				files[i] = as->regions[i].file;
				as->regions[i].file = NULL;
			}

			file_count = as->region_count;
			if (file_count > AS_REGIONS_MAX)
				file_count = AS_REGIONS_MAX;
		}
	}

	spin_unlock_irq(&table_lock, flags);

	for (i = 0; i < file_count; i++)
		if (files[i])
			file_release(files[i]);

	if (root) {
		/* Nobody can be translating through it: the count reached zero,
		 * which means no thread holds it, and a processor only holds
		 * one while a thread of it is running there. */
		arch_as_free_root(root);
	}
}

void addrspace_activate(struct addrspace *as)
{
	unsigned cpu = arch_cpu_id();

	/* Already there. Worth checking rather than doing anyway: a switch
	 * flushes translations, and two threads of one program switching
	 * between each other would pay that for no change at all. */
	if (active[cpu] == as)
		return;

	active[cpu] = as;
	arch_as_activate(as ? as->root : 0);
}

struct addrspace *addrspace_active(void)
{
	return active[arch_cpu_id()];
}

bool addrspace_map(struct addrspace *as, vaddr_t va, paddr_t pa, u64 size,
		   unsigned flags)
{
	struct addrspace *was;
	bool ok;
	u64 irq;

	if (!as)
		return false;

	/* Interrupts off across the whole of this. The processor is pointed at
	 * a space that is not the one the running thread belongs to, and a tick
	 * landing in that window would schedule somebody else into a map that
	 * is not theirs. */
	irq = arch_irq_save();

	was = addrspace_active();
	addrspace_activate(as);

	ok = vm_map(va, pa, size, flags);
	if (ok)
		as->mapped_bytes += size;

	addrspace_activate(was);
	arch_irq_restore(irq);

	return ok;
}

unsigned addrspace_count(void)
{
	unsigned i, n = 0;

	for (i = 0; i < ADDRSPACE_MAX; i++)
		if (table[i].refs)
			n++;

	return n;
}

void addrspace_print_summary(void)
{
	unsigned i, n = 0;
	u64 total = 0;

	for (i = 0; i < ADDRSPACE_MAX; i++)
		if (table[i].refs) {
			n++;
			total += table[i].mapped_bytes;
		}

	kprintf("\nAddress spaces\n");
	kprintf("  live         : %u of %u, %lu KB mapped between them\n",
		n, (unsigned)ADDRSPACE_MAX, (unsigned long)(total / 1024));
	kprintf("  this cpu     : %s\n",
		addrspace_active() ? "a program's" : "the kernel's own");
}

/* --- demand paging -------------------------------------------------------
 *
 * A region says memory *will* exist if the program touches it. Nothing is
 * mapped until it does, which is why a stack can be reserved at a size nobody
 * would want to allocate up front.
 *
 * THE SHARED ZERO PAGE, AND WHY IT IS NOT AN OPTIMISATION LOOKING FOR A JOB.
 * A program that reads memory it has never written must see zeroes -- if it
 * saw whatever the last program left there, every freed page in the machine
 * would be readable by the next one to ask. The obvious way to guarantee that
 * is to allocate a page and clear it. But a page of zeroes is the same page of
 * zeroes wherever it appears, so every read-only fault in the machine can point
 * at *one* of them. It costs one page for the whole system and it makes the
 * guarantee structural: there is no path where a program is handed a page
 * somebody else wrote, because on the read path it is never handed a page at
 * all.
 *
 * That page is mapped read-only, and the read-only bit is doing real work
 * rather than being tidy. The first *write* traps, and the handler gives that
 * program a page of its own before the write lands. That is copy-on-write --
 * the same mechanism `fork` needs, with the shared thing being zeroes rather
 * than a parent's memory. Building it here means the harder caller arrives to
 * a mechanism that already exists and has been exercised on every boot.
 */
static paddr_t zero_page;

static unsigned faults_served;		/* addresses made to exist */
static unsigned zero_shares;		/* answered with the shared page */
static unsigned zero_copies;		/* and then written to, so copied */
static unsigned faults_refused;		/* never the program's to touch */

static struct as_region *region_for(struct addrspace *as, vaddr_t addr)
{
	unsigned i;

	for (i = 0; i < as->region_count; i++)
		if (addr >= as->regions[i].start && addr < as->regions[i].end)
			return &as->regions[i];

	return NULL;
}

/* Gives the program a page of its own at `page`, in place of whatever was
 * there. `replacing` is the page whose contents must be carried over, or zero
 * when there is nothing to carry.
 *
 * The copy from the shared zero page is a copy of zeroes, and it is written as
 * a copy rather than as a second clear on purpose: when the shared thing is a
 * parent process's memory instead of zeroes, this is already the line that
 * does the right thing, and the difference between the two cases stays in the
 * caller where it belongs.
 */
static bool fill_from_file(struct as_region *r, vaddr_t page, void *into);

static bool give_own_page(struct addrspace *as, vaddr_t page,
			  const struct as_region *r, paddr_t replacing)
{
	paddr_t fresh = pmm_alloc_page();

	if (!fresh)
		return false;

	if (replacing)
		kmemcpy(phys_to_virt(fresh), phys_to_virt(replacing),
			PAGE_SIZE);
	else
		kmemset(phys_to_virt(fresh), 0, PAGE_SIZE);

	/* And then whatever backs it, over the zeroes rather than instead of
	  * them: the part of the page past the end of the file stays zero, which
	  * is what a mapping longer than its file has always meant.
	  *
	  * Only when this page is being made from nothing. A copy-on-write copy
	  * takes the page the program was already reading, and re-reading the
	  * file there would throw away whatever it had written before the copy. */
	if (!replacing && r->file &&
	    !fill_from_file((struct as_region *)r, page, phys_to_virt(fresh))) {
		pmm_free_page(fresh);
		return false;
	}

	/* vm_map replaces the live entry and invalidates it in the same call,
	 * which is the ordering that matters here: the shared page has to stop
	 * being reachable through this address before the write is retried, or
	 * the retry writes into the zeroes every other program is reading. */
	if (!vm_map(page, fresh, PAGE_SIZE, r->flags | VM_USER)) {
		pmm_free_page(fresh);
		return false;
	}

	if (replacing)
		zero_copies++;

	as->mapped_bytes += PAGE_SIZE;
	faults_served++;
	return true;
}

bool addrspace_reserve(struct addrspace *as, vaddr_t va, u64 size,
		       unsigned flags)
{
	struct as_region *r;
	u64 irq;

	if (!as || !size)
		return false;

	irq = arch_irq_save();

	if (as->region_count >= AS_REGIONS_MAX) {
		arch_irq_restore(irq);
		return false;
	}

	r = &as->regions[as->region_count++];
	r->start = va & ~(vaddr_t)(PAGE_SIZE - 1);
	r->end   = (va + size + PAGE_SIZE - 1) & ~(vaddr_t)(PAGE_SIZE - 1);
	r->flags = flags;
	r->file  = NULL;
	r->file_offset = 0;
	r->file_len = 0;

	arch_irq_restore(irq);
	return true;
}

/* One lock for filling a file-backed page.
 *
 * A `struct file` has a position, and filling a page means seeking to an
 * offset and reading -- two operations that are only one operation if nothing
 * else touches the file in between. Two processors faulting on the same
 * mapping would otherwise each read from where the other had just seeked to,
 * and each would get a page of somebody else's file with no error anywhere.
 *
 * A lock rather than a read-at-offset call in the interface, because adding
 * one would mean every implementation growing a function whose only caller is
 * this. When there is a second caller there will also be a reason.
 */
static struct spinlock fill_lock = SPINLOCK_INIT("mmap-fill");

bool addrspace_map_file(struct addrspace *as, vaddr_t va, u64 size,
			unsigned flags, struct file *f, u64 offset,
			u64 len)
{
	struct as_region *r;
	u64 irq;

	/* A file that cannot be read or sought cannot back a mapping, and
	  * finding that out at the first fault would mean a program dying on
	  * an address rather than being refused a mapping. */
	if (!as || !size || !f || !f->ops->read || !f->ops->seek)
		return false;

	irq = arch_irq_save();

	if (as->region_count >= AS_REGIONS_MAX) {
		arch_irq_restore(irq);
		return false;
	}

	r = &as->regions[as->region_count++];
	r->start = va & ~(vaddr_t)(PAGE_SIZE - 1);
	r->end   = (va + size + PAGE_SIZE - 1) & ~(vaddr_t)(PAGE_SIZE - 1);
	r->flags = flags;
	r->file  = file_hold(f);
	r->file_offset = offset;
	r->file_len = len;

	arch_irq_restore(irq);
	return true;
}

/* Fills one page from the region's file. The page is already allocated and
 * already zeroed, so a short read is not an error -- it is the tail of a
 * mapping that reaches past the end of what the file backs, which is exactly
 * what an ELF segment with a .bss looks like. */
static bool fill_from_file(struct as_region *r, vaddr_t page, void *into)
{
	u64 within = (u64)(page - r->start);
	u64 want = PAGE_SIZE;
	u64 flags;
	i64 n;

	if (within >= r->file_len)
		return true;		/* entirely past the file: zeroes */

	if (r->file_len - within < want)
		want = r->file_len - within;

	flags = spin_lock_irq(&fill_lock);

	if (r->file->ops->seek(r->file, (i64)(r->file_offset + within),
				  SEEK_START) < 0) {
		spin_unlock_irq(&fill_lock, flags);
		return false;
	}

	n = r->file->ops->read(r->file, into, want);
	spin_unlock_irq(&fill_lock, flags);

	/* A read that failed is a fault. A read that was short is not: the
	  * rest of the page is already zero, and a file that ended is a file
	  * that ended. */
	return n >= 0;
}

paddr_t addrspace_zero_page(void)
{
	return zero_page;
}

bool vm_fault_user(vaddr_t addr, bool write)
{
	struct addrspace *as = addrspace_active();
	struct as_region *r;
	vaddr_t page = addr & ~(vaddr_t)(PAGE_SIZE - 1);
	paddr_t have;

	/* No space means a kernel thread faulted, which is not this function's
	 * business and must not be quietly papered over. */
	if (!as)
		return false;

	r = region_for(as, addr);
	if (!r) {
		/* The address was never promised. This is the case that keeps
		 * a wild pointer a wild pointer: demand paging must not turn
		 * every address in the lower half into memory on request, or a
		 * program that runs off the end of its stack grows a new one
		 * instead of failing. */
		faults_refused++;
		return false;
	}

	have = vm_lookup(page);

	if (!have) {
		/* Absent does not mean never mapped. An entry can name a
		  * swap slot instead of a page, and this is the only place
		  * that can tell -- vm_lookup answers zero for both.
		  *
		  * Checked before anything else, because every branch below
		  * would otherwise hand the program a fresh page of zeroes
		  * over the top of memory it still owns. */
		if (vm_swap_slot(page)) {
			if (evict_fault_in(page, r->flags)) {
				faults_served++;
				return true;
			}

			/* The page is gone and could not be read back. There
			  * is nothing to hand over: a zeroed page here is a
			  * program carrying on with silently corrupted
			  * memory, which is worse than the fault. */
			faults_refused++;
			return false;
		}

		/* A file-backed page has contents, so there is nothing to share:
		  * the shared page of zeroes is the right answer only when zeroes
		  * are the right answer. Every page here is its own from the first
		  * touch, read or write. */
		if (r->file)
			return give_own_page(as, page, r, 0);

		if (!write && !(r->flags & VM_WRITE)) {
			/* A read-only region: the shared page is the whole
			 * answer and there will never be a copy. */
			if (!vm_map(page, zero_page, PAGE_SIZE,
				    (r->flags & ~VM_WRITE) | VM_USER))
				return false;

			zero_shares++;
			faults_served++;
			return true;
		}

		if (!write) {
			/* A readable-and-writable region, read first. Share
			 * the zeroes and wait to see whether it is ever
			 * written -- mapped without VM_WRITE precisely so that
			 * the write traps and can be answered properly. */
			if (!vm_map(page, zero_page, PAGE_SIZE,
				    (r->flags & ~VM_WRITE) | VM_USER))
				return false;

			zero_shares++;
			faults_served++;
			return true;
		}

		/* Written before it was ever read: no point sharing a page it
		 * is about to stop sharing. */
		return give_own_page(as, page, r, 0);
	}

	/* Something is there and the program still faulted, so it wrote to a
	 * page it may only read. If that page is the shared zeroes, this is the
	 * copy half of copy-on-write and the answer is a page of its own. If it
	 * is not, the program is writing to memory that is genuinely read-only
	 * -- its own code, for instance -- and that is a fault. */
	if (write && have == zero_page && (r->flags & VM_WRITE))
		return give_own_page(as, page, r, zero_page);

	faults_refused++;
	return false;
}

void vm_fault_print_summary(void)
{
	kprintf("\nDemand paging\n");
	kprintf("  faults       : %u made to exist, %u refused\n",
		faults_served, faults_refused);
	kprintf("  zero page    : %u shares, %u of them later copied\n",
		zero_shares, zero_copies);
}

/* One page of zeroes for the whole machine. Allocated at boot rather than on
 * the first fault, because the first fault is inside a fault handler and an
 * allocation that fails there has no good answer. */
void addrspace_init(void)
{
	zero_page = pmm_alloc_page();
	if (!zero_page)
		panic("addrspace: no page for the shared zeroes");

	kmemset(phys_to_virt(zero_page), 0, PAGE_SIZE);
}

/* --- the self-test --------------------------------------------------------
 *
 * The property worth asserting is the one the old design could not have: that
 * the *same address* means different memory in two different spaces. A test
 * that merely creates two spaces and maps something into each proves that two
 * allocations succeeded.
 *
 * So both spaces map the same virtual address to different pages holding
 * different values, and each is read back through that one address. If the
 * roots were not really separate -- or if switching did not flush what the
 * previous program had cached -- the second read returns the first program's
 * value, and every page table involved still looks correct. That is the exact
 * fault checkpoint 10 found one level down, and it is invisible to anything
 * except reading the memory.
 */
bool addrspace_self_test(void)
{
	struct addrspace *a = addrspace_create();
	struct addrspace *b = addrspace_create();
	paddr_t pa = pmm_alloc_page();
	paddr_t pb = pmm_alloc_page();
	const vaddr_t at = USER_BASE;
	struct addrspace *was;
	bool ok = true;
	u64 irq;

	if (!a || !b || !pa || !pb) {
		kputs("  addrspace: could not build two spaces to compare\n");
		ok = false;
		goto done;
	}

	*(volatile u32 *)phys_to_virt(pa) = 0xAAAA0001U;
	*(volatile u32 *)phys_to_virt(pb) = 0xBBBB0002U;

	if (!addrspace_map(a, at, pa, PAGE_SIZE, VM_READ | VM_WRITE | VM_USER) ||
	    !addrspace_map(b, at, pb, PAGE_SIZE, VM_READ | VM_WRITE | VM_USER)) {
		kputs("  addrspace: could not map a page into each space\n");
		ok = false;
		goto done;
	}

	irq = arch_irq_save();
	was = addrspace_active();

	addrspace_activate(a);
	if (*(volatile u32 *)at != 0xAAAA0001U) {
		kputs("  addrspace: the first space does not read its own "
		      "page\n");
		ok = false;
	}

	addrspace_activate(b);
	if (*(volatile u32 *)at == 0xAAAA0001U) {
		kputs("  addrspace: the second space reads the first one's "
		      "memory at the same address -- the spaces are not "
		      "separate\n");
		ok = false;
	} else if (*(volatile u32 *)at != 0xBBBB0002U) {
		kputs("  addrspace: the second space reads neither page\n");
		ok = false;
	}

	/* And back, because a switch that works once in one direction is not a
	 * switch. This is the read that catches a flush that only happens on
	 * the way out. */
	addrspace_activate(a);
	if (*(volatile u32 *)at != 0xAAAA0001U) {
		kputs("  addrspace: switching back does not restore the first "
		      "space's view\n");
		ok = false;
	}

	addrspace_activate(was);
	arch_irq_restore(irq);

	/* --- demand paging, the shared zeroes, and the copy ------------------
	 *
	 * Every read and write below goes through the *real* fault handler:
	 * nothing here calls vm_fault_user, it touches an address that is not
	 * mapped and lets the processor do what a program would make it do.
	 * A test that called the handler directly would prove the handler
	 * works and say nothing about whether anything reaches it.
	 *
	 * The last assertion is the one that matters. After the write, the page
	 * the read had been sharing must still be entirely zero -- because if
	 * the copy had not happened, the write went into the one page that every
	 * untouched read in the machine points at, and every other program's
	 * blank memory now reads this value. Nothing would fault, nothing would
	 * be reported, and the tables would all be correct.
	 */
	{
		struct addrspace *c = addrspace_create();

		/* A page inside the user half that nothing else uses: above the
		 * program image, well below the stack. */
		const vaddr_t at = USER_BASE + 0x100000;
		volatile u32 *seen = (volatile u32 *)at;
		paddr_t shared;

		if (!c) {
			kputs("  addrspace: no space to test demand paging "
			      "in\n");
			ok = false;
		} else {
			irq = arch_irq_save();
			was = addrspace_active();

			if (!addrspace_reserve(c, at, PAGE_SIZE,
					       VM_READ | VM_WRITE | VM_USER)) {
				kputs("  addrspace: could not reserve a page to "
				      "fault in\n");
				ok = false;
			}

			addrspace_activate(c);

			/* Nothing is mapped here. Reading it is the fault. */
			if (*seen != 0) {
				kputs("  addrspace: memory that was never "
				      "written did not read as zero\n");
				ok = false;
			}

			shared = vm_lookup(at);
			if (!shared) {
				kputs("  addrspace: a read of reserved memory "
				      "did not map anything\n");
				ok = false;
			}

			/* And now the write, which must not land in it. */
			*seen = 0xC0FFEE01U;

			if (*seen != 0xC0FFEE01U) {
				kputs("  addrspace: a write to demand-paged "
				      "memory did not read back\n");
				ok = false;
			}

			if (shared && vm_lookup(at) == shared) {
				kputs("  addrspace: the write went to the same "
				      "page the read shared -- nothing was "
				      "copied\n");
				ok = false;
			}

			if (shared &&
			    *(volatile u32 *)phys_to_virt(shared) != 0) {
				kputs("  addrspace: the write landed in the "
				      "page every untouched read in the machine "
				      "shares\n");
				ok = false;
			}

			addrspace_activate(was);
			arch_irq_restore(irq);
			addrspace_release(c);
		}
	}

	/* --- a slot handed on, and what it must not carry with it -----------
	 *
	 * The table is a fixed array and a released space's slot is given to the
	 * next process that asks. What that next process must not inherit is the
	 * last one's *regions* -- because a region is not data, it is permission
	 * to touch an address. `vm_fault_user` consults it to decide whether a
	 * fault is demand paging or a wild pointer, so an inherited region turns
	 * one program's mistake into another program's zero page.
	 *
	 * Asserted by *behaviour* rather than by reading `region_count`. The
	 * count being zero is the mechanism; what matters is that the address
	 * the old space was allowed to touch is refused in the new one, and that
	 * is the sentence somebody would want to be true.
	 */
	{
		const vaddr_t at = USER_BASE + 0x200000;
		struct addrspace *first = addrspace_create();
		struct addrspace *second;

		if (!first) {
			kputs("  addrspace: no space to test slot reuse with\n");
			ok = false;
		} else {
			addrspace_reserve(first, at, PAGE_SIZE,
					  VM_READ | VM_WRITE | VM_USER);
			addrspace_release(first);

			/* The very next create takes the slot just freed, which
			 * is what makes this a test of reuse rather than of
			 * allocation. */
			second = addrspace_create();

			if (!second) {
				kputs("  addrspace: no second space\n");
				ok = false;
			} else {
				if (second->region_count != 0) {
					kprintf("  addrspace: a fresh space "
						"arrived holding %u region(s) "
						"from whoever had the slot "
						"before it\n",
						second->region_count);
					ok = false;
				}

				irq = arch_irq_save();
				was = addrspace_active();
				addrspace_activate(second);

				/* The old space was allowed to touch this. The
				 * new one must not be. */
				if (vm_fault_user(at, false)) {
					kputs("  addrspace: a new program was "
					      "given memory at an address only "
					      "the previous one had asked "
					      "for\n");
					ok = false;
				}

				addrspace_activate(was);
				arch_irq_restore(irq);

				addrspace_release(second);
			}
		}
	}

done:
	if (pa)
		pmm_free_page(pa);
	if (pb)
		pmm_free_page(pb);
	if (a)
		addrspace_release(a);
	if (b)
		addrspace_release(b);

	return ok;
}
