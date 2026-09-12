/* x86_64 page tables.
 *
 * Four levels: PML4 -> PDPT -> PD -> PT, nine bits of index each, twelve bits
 * of offset. A "large page" is a level stopping early: setting the page-size
 * bit in a PD entry maps 2MB with one entry, and in a PDPT entry maps 1GB.
 *
 * The kernel builds the map it wants and then loads CR3, which is a single
 * instruction and takes effect on the next memory access -- including the one
 * that fetches the next instruction. So the code doing the switch must be
 * mapped identically in both maps, or the CPU triple-faults on the instruction
 * after the write. That is why the identity mapping of the kernel image is not
 * optional and is built first.
 */
#include "x86_64.h"
#include <recon/kernel/time.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/smp.h>

#include <recon/kernel/user.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/addrspace.h>
#include <recon/kernel/pageage.h>
#include <recon/kernel/evict.h>
#include <recon/kernel/arch.h>
#include <recon/kernel/boot.h>
#include <recon/kernel/cpu.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/panic.h>

/* Entry bits. */
#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITE     (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_PWT       (1ULL << 3)	/* write-through */
#define PTE_PCD       (1ULL << 4)	/* cache disable */

/* The third selector bit. Together with PCD and PWT it indexes the eight-entry
 * page attribute table: index = (PAT << 2) | (PCD << 1) | PWT.
 *
 * It sits at a different bit for a large page, because bit 7 is what *makes* a
 * page large. Using the small-page bit on a large mapping would ask for a
 * different cache type and get a page of the wrong size. */
#define PTE_PAT_SMALL (1ULL << 7)
#define PTE_PAT_LARGE (1ULL << 12)
#define PTE_ACCESSED  (1ULL << 5)
#define PTE_DIRTY     (1ULL << 6)
#define PTE_LARGE     (1ULL << 7)	/* this entry *is* the page */
#define PTE_GLOBAL    (1ULL << 8)
#define PTE_NX        (1ULL << 63)

/* The address bits of an entry. The top bit is NX and the low twelve are
 * flags, so the frame is what is left between them. */
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

#define SIZE_2M (2ULL * 1024 * 1024)
#define SIZE_1G (1024ULL * 1024 * 1024)

/* Physical memory is mapped here. PML4 slot 256 -- the first address with the
 * top bit set, which on x86_64 is where the kernel half of the address space
 * begins. Chosen at the bottom of that half so there is room above it for
 * everything else the kernel will eventually want to map. */
#define DIRECT_MAP_BASE 0xFFFF800000000000ULL

static u64 *kernel_pml4;

/* Kept separately because the *pointer* stops being valid at the moment the
 * switch happens. kernel_pml4 was obtained through the identity map we were
 * handed; afterwards only the kernel image is identity mapped, and the root
 * table is not in the kernel image. See the re-point at the end of vm_init(). */
static paddr_t kernel_pml4_phys;
static bool direct_map_live;

/* Counted so the summary can say what the map actually cost, which is the
 * whole point of large pages and would otherwise be a claim. */
static u64 mapped_1g, mapped_2m, mapped_4k, table_pages;

static struct cpu_caps caps;

void *phys_to_virt(paddr_t phys)
{
	/* Before the direct map exists, the only map is the identity one we are
	 * executing on. Returning the physical address unchanged is correct
	 * then, and wrong afterwards -- so it is a branch and not an
	 * assumption. */
	if (!direct_map_live)
		return (void *)(uintptr_t)phys;
	return (void *)(uintptr_t)(DIRECT_MAP_BASE + phys);
}

/* The physical address behind a direct-map pointer, or zero.
 *
 * **Zero is the important answer**, and it used to be unreachable. The test was
 * `v >= DIRECT_MAP_BASE`, and the kernel image is mapped at KERNEL_VMA
 * (0xFFFFFFFF80000000), which is *above* the direct map base
 * (0xFFFF800000000000) -- so a stack address, a pointer into the kernel image,
 * anything in the higher half at all, passed the test. The subtraction then
 * produced a number: for a stack buffer at 0xFFFFFFFF801DB7F0 it produced
 * 0x7FFF801DB7F0, about 140 terabytes in, which is not memory on any machine
 * this runs on.
 *
 * That number is *non-zero*, and every caller checks for zero. virtio-blk's
 * `reachable()` has carried the right comment since it was written -- "a stack
 * address or anything else would translate to a physical page that has nothing
 * to do with the buffer" -- and its guard could not fire, because the thing it
 * guards against never returned zero.
 *
 * What it looked like from above: a read that was entered, queued, issued to
 * the driver and reported BLOCK_OK, with the caller's buffer untouched. The
 * device wrote where it was told. Nothing anywhere was in a position to
 * notice, and it stayed invisible because every other caller in the kernel
 * hands drivers pages from the page allocator, which are direct-map addresses
 * by construction. It took a filesystem reading a 512-byte superblock into a
 * stack array to find it. (BG-193)
 *
 * The bound at the top is the kernel image. Physical memory is mapped from
 * DIRECT_MAP_BASE upward and there is no machine here with 128TB of it, so
 * anything at or above KERNEL_VMA is by construction not a direct-map address.
 */
paddr_t virt_to_phys(const void *virt)
{
	u64 v = (u64)(uintptr_t)virt;

	if (direct_map_live && v >= DIRECT_MAP_BASE && v < KERNEL_VMA)
		return (paddr_t)(v - DIRECT_MAP_BASE);

	/* Below the higher half at all: an identity-mapped address from before
	 * the switch, which is its own physical address. This is the path
	 * early boot takes and it is still correct. */
	if (v < DIRECT_MAP_BASE)
		return (paddr_t)v;

	/* In the higher half and not in the direct map -- the kernel image,
	 * a stack, a device mapping. There is no physical address this can
	 * answer with, and answering anyway is what BG-193 was. */
	return 0;
}

static u64 *table_at(paddr_t phys)
{
	return (u64 *)phys_to_virt(phys);
}

static u64 *alloc_table(void)
{
	paddr_t p = pmm_alloc_page();
	u64 *t;

	if (!p)
		return 0;

	t = table_at(p);
	kmemset(t, 0, PAGE_SIZE);
	table_pages++;
	return t;
}

/* Walks to the next level, creating it if asked. Returns the table, or null
 * when there is no memory left for one. */
static u64 *next_level(u64 *table, unsigned index, bool create)
{
	if (!(table[index] & PTE_PRESENT)) {
		u64 *fresh;
		paddr_t phys;

		if (!create)
			return 0;

		fresh = alloc_table();
		if (!fresh)
			return 0;

		phys = virt_to_phys(fresh);

		/* Intermediate entries are permissive, and deliberately so.
		 *
		 * The processor takes the *most* restrictive of every level for
		 * write and user, and the *least* restrictive for no-execute.
		 * So NX must be clear here and set on the leaf, or nothing below
		 * is ever executable -- and USER must be set here, or nothing
		 * below is ever reachable from user mode however the leaf is
		 * marked. The leaf decides; the levels above must not veto. */
		table[index] = (phys & PTE_ADDR_MASK)
			     | PTE_PRESENT | PTE_WRITE | PTE_USER;
		return fresh;
	}

	/* A large page where a table was expected. The caller is trying to map
	 * something inside a range already mapped at a coarser granularity, and
	 * silently splitting it would be a surprise; refuse instead. */
	if (table[index] & PTE_LARGE)
		return 0;

	return table_at(table[index] & PTE_ADDR_MASK);
}

/* Replacing a live mapping means the old translation may still be sitting in
 * the processor's cache of them, and a cached translation is consulted before
 * the table it came from. So the entry has to be knocked out by hand.
 *
 * This was missing until user mode arrived, and it could not have been noticed
 * before: every mapping the kernel had ever made was made once, at an address
 * nothing had touched yet, and there was nothing stale to find. The second user
 * program, mapped at the same address as the first, read the first program's
 * page and ran it -- correct tables, wrong memory.
 *
 * Only replacements are invalidated. A first mapping has nothing cached, and
 * invalidating unconditionally would mean five hundred invalidations while
 * building the direct map for no reason at all.
 */
static unsigned shootdown_timeouts;
static unsigned shootdowns_sent;
static unsigned live_invalidations;

static void shoot_down_others(vaddr_t va);

static void invalidate_if_live(u64 entry, vaddr_t va)
{
	if (!(entry & PTE_PRESENT))
		return;

	live_invalidations++;
	__asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");

	/* And every other processor, which `invlpg` does not reach. */
	shoot_down_others(va);
}

/* --- The page attribute table ---------------------------------------------
 *
 * Eight entries of three bits, in one model-specific register, saying what each
 * combination of the three selector bits in a page table entry *means*. The
 * architecture defines a default layout; this changes exactly one entry.
 *
 * Entry four becomes write-combining. Every other entry keeps its architectural
 * value, so a page table entry written by anything that has not heard of this
 * behaves exactly as it did -- which matters because the direct map is built
 * before this runs.
 *
 * **Per processor.** The register is not shared, and a secondary that never
 * programmed it would read entry four as its default, write-back, and cache
 * writes to a framebuffer. Nothing would fault; the screen would simply stop
 * being updated by whichever processor drew last.
 */
#define MSR_PAT 0x277u

#define PAT_UNCACHEABLE   0x00ull
#define PAT_WRITE_COMBINE 0x01ull
#define PAT_WRITE_THROUGH 0x04ull
#define PAT_WRITE_BACK    0x06ull
#define PAT_UNCACHED_MINUS 0x07ull

void x86_pat_init(void)
{
	u64 pat = PAT_WRITE_BACK			/* 0, unchanged */
		| (PAT_WRITE_THROUGH   << 8)		/* 1, unchanged */
		| (PAT_UNCACHED_MINUS  << 16)		/* 2, unchanged */
		| (PAT_UNCACHEABLE     << 24)		/* 3, unchanged */
		| (PAT_WRITE_COMBINE   << 32)		/* 4, the one change */
		| (PAT_WRITE_THROUGH   << 40)		/* 5, unchanged */
		| (PAT_UNCACHED_MINUS  << 48)		/* 6, unchanged */
		| (PAT_UNCACHEABLE     << 56);		/* 7, unchanged */

	x86_wrmsr(MSR_PAT, pat);
}

static u64 leaf_bits(unsigned flags, bool large)
{
	u64 bits = PTE_PRESENT;

	if (flags & VM_WRITE)
		bits |= PTE_WRITE;
	if (!(flags & VM_EXEC) && caps.no_execute)
		bits |= PTE_NX;
	if (flags & VM_USER)
		bits |= PTE_USER;
	else if (flags & VM_GLOBAL)
		/* Global and user are mutually exclusive here on purpose: a
		 * global mapping survives an address space switch, and a user
		 * mapping belongs to one address space by definition. */
		bits |= PTE_GLOBAL;
	if (flags & VM_DEVICE)
		bits |= PTE_PCD | PTE_PWT;
	else if (flags & VM_WRITE_COMBINE)
		/* Entry four: PAT set, PCD and PWT clear. Programmed to
		 * write-combining by pat_init below -- and *only* by it. The
		 * architectural default for that entry is write-back, so a
		 * mapping made before the table is programmed would be cached,
		 * which for a framebuffer means pixels that never appear. */
		bits |= large ? PTE_PAT_LARGE : PTE_PAT_SMALL;

	if (large)
		bits |= PTE_LARGE;

	return bits;
}

/* --- whose tables a mapping goes into ------------------------------------
 *
 * The kernel's half is one map shared by every processor and every program: it
 * is the same memory seen from the same addresses no matter who is running.
 * The half below it is not, once processes have address spaces of their own,
 * and the difference has to be decided per *address* rather than per call --
 * because callers do not know or care which half they are in.
 *
 * Held per processor rather than as one variable, because two processors run
 * two different programs at the same instant and there is no single answer to
 * "which address space is current". This is the same fact that made the
 * task-state segment per-processor at checkpoint 9b, and the fault it would
 * cause is quieter: a mapping made into the wrong program's tables.
 *
 * Null means the kernel's own, which is what every processor starts with and
 * what a kernel thread runs in.
 */
static u64 *active_user_root[MAX_CPUS];

static u64 *root_for(vaddr_t va)
{
	u64 *user;

	if (va >= USER_LIMIT)
		return kernel_pml4;

	user = active_user_root[arch_cpu_id()];
	return user ? user : kernel_pml4;
}

bool vm_map(vaddr_t va, paddr_t pa, u64 size, unsigned flags)
{
	if ((va | pa | size) & (PAGE_SIZE - 1))
		panic("vm_map: unaligned request");

	while (size) {
		unsigned i4 = (unsigned)((va >> 39) & 0x1FF);
		unsigned i3 = (unsigned)((va >> 30) & 0x1FF);
		unsigned i2 = (unsigned)((va >> 21) & 0x1FF);
		unsigned i1 = (unsigned)((va >> 12) & 0x1FF);

		u64 *pdpt = next_level(root_for(va), i4, true);
		u64 *pd, *pt;

		if (!pdpt)
			return false;

		/* Largest page the alignment and the remaining length allow.
		 * Checked in order, so a request that could be one 1GB page is
		 * never made from five hundred and twelve 2MB ones. */
		if (caps.page_1g && size >= SIZE_1G &&
		    !((va | pa) & (SIZE_1G - 1))) {
			invalidate_if_live(pdpt[i3], va);
			pdpt[i3] = (pa & PTE_ADDR_MASK) | leaf_bits(flags, true);
			mapped_1g++;
			va += SIZE_1G;
			pa += SIZE_1G;
			size -= SIZE_1G;
			continue;
		}

		pd = next_level(pdpt, i3, true);
		if (!pd)
			return false;

		if (caps.page_2m && size >= SIZE_2M &&
		    !((va | pa) & (SIZE_2M - 1))) {
			invalidate_if_live(pd[i2], va);
			pd[i2] = (pa & PTE_ADDR_MASK) | leaf_bits(flags, true);
			mapped_2m++;
			va += SIZE_2M;
			pa += SIZE_2M;
			size -= SIZE_2M;
			continue;
		}

		pt = next_level(pd, i2, true);
		if (!pt)
			return false;

		invalidate_if_live(pt[i1], va);
		pt[i1] = (pa & PTE_ADDR_MASK) | leaf_bits(flags, false);
		mapped_4k++;
		va += PAGE_SIZE;
		pa += PAGE_SIZE;
		size -= PAGE_SIZE;
	}

	return true;
}

/* Takes a mapping away, and tells the other processors it has gone.
 *
 * The kernel has been able to map since checkpoint 5 and never to unmap, which
 * was honest while every mapping it made was permanent. It is not any more:
 * the first caller is the trampoline page, which has to stop being mapped in
 * the half a process is meant to own, and every later one is a process
 * releasing memory.
 *
 * THE ORDER MATTERS AND IT IS THE OPPOSITE OF MAPPING. The entry is cleared
 * first and the invalidation follows -- a processor that reads the entry
 * between those two steps sees "not present" and faults, which is the correct
 * answer arriving early. Invalidating first would leave a window where a
 * processor could reload the very translation being removed, and then keep it.
 *
 * IT REFUSES TO SPLIT A LARGE PAGE. Unmapping four kilobytes out of the middle
 * of a two-megabyte mapping means allocating a page table, copying five hundred
 * and twelve entries into it, and swapping it in underneath processors that are
 * using it -- a different operation with a different set of races, and one no
 * caller here needs. Refusing is a return of false at a known place; splitting
 * badly is a fault somewhere else entirely.
 *
 * Nothing is freed. The physical page belongs to whoever allocated it and this
 * has no way to know whether it is still wanted -- the direct map and a process
 * mapping can point at the same page, and a filesystem cache will one day.
 */
bool vm_unmap(vaddr_t va, u64 size)
{
	if ((va | size) & (PAGE_SIZE - 1))
		panic("vm_unmap: unaligned request");

	while (size) {
		unsigned i4 = (unsigned)((va >> 39) & 0x1FF);
		unsigned i3 = (unsigned)((va >> 30) & 0x1FF);
		unsigned i2 = (unsigned)((va >> 21) & 0x1FF);
		unsigned i1 = (unsigned)((va >> 12) & 0x1FF);

		u64 *pdpt = next_level(root_for(va), i4, false);
		u64 *pd, *pt;
		u64 old;

		/* Nothing there is not an error. A caller tearing down a range
		 * it only partly mapped is doing the right thing, and making
		 * it track which parts it got to is how a teardown path grows
		 * a bug that only fires when an earlier step failed. */
		if (!pdpt) {
			va += PAGE_SIZE;
			size -= PAGE_SIZE;
			continue;
		}

		if (pdpt[i3] & PTE_PRESENT && pdpt[i3] & PTE_LARGE) {
			if (size < SIZE_1G || (va & (SIZE_1G - 1)))
				return false;

			old = pdpt[i3];
			pdpt[i3] = 0;
			invalidate_if_live(old, va);
			mapped_1g--;
			va += SIZE_1G;
			size -= SIZE_1G;
			continue;
		}

		pd = next_level(pdpt, i3, false);
		if (!pd) {
			va += PAGE_SIZE;
			size -= PAGE_SIZE;
			continue;
		}

		if (pd[i2] & PTE_PRESENT && pd[i2] & PTE_LARGE) {
			if (size < SIZE_2M || (va & (SIZE_2M - 1)))
				return false;

			old = pd[i2];
			pd[i2] = 0;
			invalidate_if_live(old, va);
			mapped_2m--;
			va += SIZE_2M;
			size -= SIZE_2M;
			continue;
		}

		pt = next_level(pd, i2, false);
		if (pt && (pt[i1] & PTE_PRESENT)) {
			old = pt[i1];
			pt[i1] = 0;
			invalidate_if_live(old, va);
			mapped_4k--;
		}

		va += PAGE_SIZE;
		size -= PAGE_SIZE;
	}

	return true;
}

paddr_t vm_lookup(vaddr_t va)
{
	unsigned i4 = (unsigned)((va >> 39) & 0x1FF);
	unsigned i3 = (unsigned)((va >> 30) & 0x1FF);
	unsigned i2 = (unsigned)((va >> 21) & 0x1FF);
	unsigned i1 = (unsigned)((va >> 12) & 0x1FF);

	u64 *pdpt, *pd, *pt;

	pdpt = next_level(root_for(va), i4, false);
	if (!pdpt)
		return 0;

	if (pdpt[i3] & PTE_LARGE)
		return (pdpt[i3] & PTE_ADDR_MASK) + (va & (SIZE_1G - 1));

	pd = next_level(pdpt, i3, false);
	if (!pd)
		return 0;

	if (pd[i2] & PTE_LARGE)
		return (pd[i2] & PTE_ADDR_MASK) + (va & (SIZE_2M - 1));

	pt = next_level(pd, i2, false);
	if (!pt || !(pt[i1] & PTE_PRESENT))
		return 0;

	return (pt[i1] & PTE_ADDR_MASK) + (va & (PAGE_SIZE - 1));
}

void vm_init(void)
{
	const struct boot_info *info = boot_info();

	arch_cpu_caps(&caps);

	/* Enable no-execute before any table uses it.
	 *
	 * On this architecture, bit 63 of a page table entry is the NX bit only
	 * once EFER.NXE says so. Until then it is a *reserved* bit, and setting
	 * a reserved bit does not mean "executable" -- it means every access
	 * through that entry takes a page fault with the reserved-bit error
	 * code, whatever the entry otherwise says.
	 *
	 * This cost an evening, and the shape of it is worth remembering: the
	 * two UEFI boot paths worked and the two that use our own trampoline did
	 * not, because the firmware enables NXE for itself and our trampoline
	 * never had a reason to. CPUID says the feature is *supported*. It does
	 * not say it is *enabled*, and code that reads the first as the second
	 * works on whatever machine happened to enable it already. */
	if (caps.no_execute) {
		u32 lo, hi;

		__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080u));
		lo |= (1u << 11);	/* NXE */
		__asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0xC0000080u));
	}

	kernel_pml4 = alloc_table();
	if (!kernel_pml4)
		panic("vm: no memory for the top-level page table");

	/* The kernel image, at the address it is linked at -- and this is the
	 * mapping the switch itself depends on. CR3 takes effect on the next
	 * instruction fetch, and by the time this runs the processor is already
	 * executing high, because boot.S moved it there before any C ran. So
	 * the high mapping is the one that has to exist, and the identity
	 * mapping is the one that must not: leaving it would keep a copy of the
	 * kernel in the lower half of every address space, which is precisely
	 * what moving the kernel was for.
	 *
	 * Text and data together, writable and executable, because splitting
	 * them needs the linker to say where the boundary is and that is a
	 * separate change. */
	{
		paddr_t start = PAGE_ALIGN_DOWN((u64)(uintptr_t)__kernel_phys_start);
		u64 len = PAGE_ALIGN_UP((u64)(uintptr_t)__kernel_phys_end) - start;

		if (!vm_map((vaddr_t)(KERNEL_VMA + start), start, len,
			    VM_READ | VM_WRITE | VM_EXEC | VM_GLOBAL))
			panic("vm: could not map the kernel image");
	}

	/* Everything the firmware told us about, at the direct map offset, and
	 * mapped *region by region* rather than as one span from zero to the
	 * highest address.
	 *
	 * The difference is not cosmetic. This machine's map ends with twelve
	 * gigabytes of reserved space at the 1TB mark, and covering the whole
	 * span meant 524,288 two-megabyte entries costing four megabytes of page
	 * tables to describe a hole. Region by region it is a few hundred
	 * entries. The same mistake the page bitmap made in checkpoint 2, in a
	 * different disguise: a span is not a size, and firmware puts things at
	 * the top of the address space precisely because nothing is there.
	 *
	 * Not just usable memory: ACPI tables have to be readable, the
	 * framebuffer has to be writable, and reserved ranges are exactly the
	 * ones a driver will later need to reach. */
	for (unsigned i = 0; i < info->region_count; i++) {
		const struct mem_region *r = &info->regions[i];
		paddr_t base = PAGE_ALIGN_DOWN(r->base);
		u64 len = PAGE_ALIGN_UP(r->base + r->size) - base;
		bool is_ram = r->kind != MEM_RESERVED;

		/* Mapped as data. Nothing is ever executed through the direct
		 * map, and saying so costs nothing. */
		if (!vm_map(DIRECT_MAP_BASE + base, base, len,
			    VM_READ | VM_WRITE | VM_GLOBAL |
			    (is_ram ? 0u : VM_DEVICE)))
			panic("vm: could not build the direct map");
	}

	/* Before the framebuffer is mapped, and that order is the whole point:
	 * the entry this asks for defaults to write-back, so a mapping made
	 * first would be cached. */
	x86_pat_init();

	/* The framebuffer, if there is one. Write-combining rather than
	 * uncached: it is memory on a device across a bus, so a write that sits
	 * in a cache line is a pixel that does not appear -- but uncached makes
	 * every pixel its own bus transaction, and the console redraws whole
	 * screens when it scrolls. Combining keeps the writes going out while
	 * letting them travel together. */
	if (info->fb.width && info->fb.base) {
		paddr_t base = PAGE_ALIGN_DOWN(info->fb.base);
		u64 len = PAGE_ALIGN_UP(info->fb.size + (info->fb.base - base));

		vm_map(DIRECT_MAP_BASE + base, base, len,
		       VM_READ | VM_WRITE | VM_WRITE_COMBINE | VM_GLOBAL);
	}

	/* Switch. From the instruction after this, the direct map exists and
	 * every borrowed mapping is gone. */
	kernel_pml4_phys = virt_to_phys(kernel_pml4);
	__asm__ volatile("mov %0, %%cr3" : : "r"(kernel_pml4_phys) : "memory");

	direct_map_live = true;

	/* Every pointer to a page table taken before this instant was an
	 * identity-map pointer, and identity now covers only the kernel image.
	 * The root has to be re-reached through the direct map before anything
	 * walks it -- the same fault as the allocator bitmap, one level up, and
	 * it presents as a page fault at the root table plus the index of
	 * whatever was being looked up. */
	kernel_pml4 = table_at(kernel_pml4_phys);

	/* The allocator keeps its bitmap at a physical address it has been
	 * reaching directly, because until this instant the map was an identity
	 * map. It is not any more, so the bitmap has to be re-reached through the
	 * direct map before the next allocation writes into an unmapped page. */
	pmm_remap();
}

/* The same switch, on a processor that is not the boot processor.
 *
 * The tables are the machine's and were built once; what is per-processor is
 * the register that points at them. A secondary reaches this having already
 * loaded CR3 in the trampoline -- it had to, because it could not have executed
 * the instruction after enabling paging otherwise -- so on x86_64 this is a
 * confirmation rather than a change, and it is written out rather than left
 * empty because "the trampoline already did it" is a fact about another file.
 *
 * It also sets the bit that makes global mappings global. That is per-processor
 * and is easy to miss: the kernel's mappings are marked global so they survive
 * an address-space change, and on a processor where CR4.PGE is clear the flag
 * is simply ignored, so nothing fails -- the mappings are merely flushed more
 * often than they should be, invisibly and only on the secondaries. */
void vm_activate_this_cpu(void)
{
	u64 cr4;

	__asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
	cr4 |= (1ull << 7);				/* page global enable */
	__asm__ volatile("movq %0, %%cr4" : : "r"(cr4) : "memory");

	__asm__ volatile("mov %0, %%cr3"
			 : : "r"(kernel_pml4_phys) : "memory");

	/* Its own page attribute table. The register is per processor, and a
	 * secondary that skipped it would read entry four as write-back and
	 * cache its writes to the framebuffer -- no fault, just a screen that
	 * stops changing when that processor is the one drawing. */
	x86_pat_init();
}

/* --- what the kernel has in the half a process is meant to own -------------
 *
 * Not "is the map correct". The question is which parts of the *user* half the
 * kernel keeps something of its own in, because every one of those has to be
 * accounted for before two processes can have different low halves -- an entry
 * the kernel needs and a per-process root does not carry is a fault on a
 * processor that was working a moment earlier.
 *
 * It is reported rather than reasoned about because reasoning about it already
 * produced a confident wrong answer. Two comments in this file said the kernel
 * image was identity mapped down here; vm_init has said since checkpoint 10,
 * in its own comment, that it deliberately is not. A walk of the live tables
 * cannot hold an opinion about which of those is true.
 */
struct low_walk {
	vaddr_t  start;
	vaddr_t  end;
	bool     user;
	bool     open;
	unsigned runs;
	unsigned kernel_runs;
};

static void low_close(struct low_walk *w)
{
	if (!w->open)
		return;

	/* Enough to see the shape, bounded so that a map gone wrong cannot
	 * fill a serial log with it. */
	if (w->runs < 8)
		kprintf("  %s : 0x%lx-0x%lx\n",
			w->user ? "a program " : "the kernel",
			(unsigned long)w->start,
			(unsigned long)(w->end - 1));

	w->runs++;
	if (!w->user)
		w->kernel_runs++;

	w->open = false;
}

static void low_add(struct low_walk *w, vaddr_t va, u64 span, bool user)
{
	/* Runs are joined only when they are adjacent *and* owned the same way.
	 * Two neighbouring ranges with different owners are the whole point of
	 * this walk and must not be merged into one. */
	if (w->open && w->user == user && w->end == va) {
		w->end = va + span;
		return;
	}

	low_close(w);

	w->open  = true;
	w->start = va;
	w->end   = va + span;
	w->user  = user;
}

/* Level 4 is the PML4, level 1 the page table. */
static void low_level(struct low_walk *w, u64 *table, unsigned level,
		      vaddr_t base)
{
	u64 span = 1ULL << (12 + 9 * (level - 1));
	unsigned i;

	for (i = 0; i < 512; i++) {
		u64 e = table[i];
		vaddr_t va = base + (vaddr_t)i * span;

		/* The top half is the kernel's by architecture and by design.
		 * Only the half a process is supposed to own is in question. */
		if (level == 4 && i >= 256)
			break;

		if (!(e & PTE_PRESENT))
			continue;

		if (level == 1 || (level < 4 && (e & PTE_LARGE)))
			low_add(w, va, span, (e & PTE_USER) != 0);
		else
			low_level(w, table_at(e & PTE_ADDR_MASK), level - 1, va);
	}
}

unsigned vm_user_half_report(void)
{
	struct low_walk w;

	kmemset(&w, 0, sizeof(w));

	kprintf("\nThe user half\n");
	low_level(&w, kernel_pml4, 4, 0);
	low_close(&w);

	if (!w.runs)
		kputs("  nothing is mapped below the kernel\n");

	kprintf("  kernel-owned : %u of %u range%s\n",
		w.kernel_runs, w.runs, w.runs == 1 ? "" : "s");

	return w.kernel_runs;
}

/* --- an address space of its own -----------------------------------------
 *
 * A process address space is one page: a top-level table whose upper half is
 * the kernel's, entry for entry, and whose lower half is this program's alone.
 *
 * THE UPPER HALF IS COPIED, AND COPYING IS WHAT MAKES IT SHARED RATHER THAN
 * DUPLICATED. What is copied is the 256 entries, each of which is a *pointer*
 * to the same next-level table the kernel is already using. So a mapping the
 * kernel makes afterwards -- a driver's registers, more direct map -- appears
 * in every address space at once, because they are all looking at the same
 * tables one level down. Copying the entries deeper would give each process a
 * private snapshot of the kernel that silently stopped matching it, which is
 * the bug this shape exists to make impossible.
 *
 * The kernel's own top-level entries are therefore never rewritten after boot
 * for the range covered here. Nothing does that today; if something ever needs
 * to, it has to walk every live address space, and this comment is where
 * whoever writes it will look.
 */
paddr_t arch_as_new_root(void)
{
	u64 *root = alloc_table();
	unsigned i;

	if (!root)
		return 0;

	/* The top half, shared. The bottom half is left zero: a program with
	 * nothing mapped yet, rather than a program that inherits whatever the
	 * last one had. */
	for (i = 256; i < 512; i++)
		root[i] = kernel_pml4[i];

	return virt_to_phys(root);
}

/* Frees the tables the *program* caused to exist, and nothing else.
 *
 * Only the lower half is walked. Walking the upper half would free the
 * kernel's own page tables -- which every other address space and every
 * processor is still using -- and would do it without any of them noticing
 * until something wrote to a page that had been handed to somebody else.
 *
 * **The pages a program's mappings pointed at are freed here too**, and that
 * sentence used to read the other way round -- "this frees tables; whoever
 * allocated the memory frees the memory, and the two are not the same list."
 * There was no other list. A program's code, stack and every page it faulted
 * in were never given back: measured at exactly twelve pages per program, the
 * same twelve every time, on a machine that is supposed to launch
 * applications. (BG-182)
 *
 * Only the user half is walked -- the loop stops at 256 entries at the top
 * level -- so the kernel's own mappings are not reachable from here, which is
 * what makes freeing leaves safe at all.
 *
 * A page that somebody else is also using is left alone. Today that is the
 * shared page of zeroes, mapped into every space in the machine.
 */
static u64 leaves_freed;

u64 vm_leaves_freed(void)
{
	return leaves_freed;
}

static void free_lower_tables(u64 *table, unsigned level)
{
	unsigned i;
	unsigned last = (level == 4) ? 256 : 512;

	for (i = 0; i < last; i++) {
		u64 e = table[i];

		if (!(e & PTE_PRESENT))
			continue;

		/* A large page is a leaf: it is memory, not a table. Left
		 * alone rather than freed, because nothing maps one into a
		 * user space and a page allocator handed the first page of a
		 * two-megabyte block would give out the other 511 as well. */
		if (level < 4 && (e & PTE_LARGE))
			continue;

		if (level > 1) {
			free_lower_tables(table_at(e & PTE_ADDR_MASK), level - 1);
			pmm_free_page(e & PTE_ADDR_MASK);
			table_pages--;
		} else {
			/* Whatever kind of page it is. A leaf here may be the
			 * machine's shared zeroes, a page of a file somebody
			 * else still has mapped, or this space's own -- and a
			 * page table entry looks identical for all three. */
			addrspace_release_page(e & PTE_ADDR_MASK);
			leaves_freed++;
		}
	}
}

void arch_as_free_root(paddr_t root)
{
	u64 *table;

	if (!root)
		return;

	table = table_at(root);
	free_lower_tables(table, 4);

	pmm_free_page(root);
	table_pages--;
}

/* Makes an address space the one this processor is translating through.
 *
 * Root zero means the kernel's own, which is what a kernel thread runs in and
 * what every processor starts in.
 *
 * The write to CR3 invalidates every non-global translation on this processor
 * by itself, which is exactly the set that could belong to the outgoing
 * program -- the kernel's mappings are marked global and survive on purpose,
 * because reloading them on every switch is the cost this bit exists to avoid.
 *
 * Nothing is sent to the other processors and nothing needs to be: they are
 * translating through their own roots, and a root this one stops using does not
 * become wrong for anybody else. Tearing an address space *down* is the case
 * that needs care, and that is why it happens after the last thread using it
 * has ended rather than when it is switched away from.
 */
void arch_as_activate(paddr_t root)
{
	unsigned cpu = arch_cpu_id();
	u64 target = root ? (u64)root : (u64)kernel_pml4_phys;

	active_user_root[cpu] = root ? table_at(root) : NULL;

	__asm__ volatile("mov %0, %%cr3" : : "r"(target) : "memory");
}

void vm_print_summary(void)
{
	kprintf("\nVirtual memory\n");
	kprintf("  direct map   : %p\n", (void *)DIRECT_MAP_BASE);
	kprintf("  pages mapped : %lu x 1GB, %lu x 2MB, %lu x 4KB\n",
		mapped_1g, mapped_2m, mapped_4k);
	kprintf("  table cost   : %lu pages (%lu KB)\n",
		table_pages, (table_pages * PAGE_SIZE) / 1024);
}

bool vm_self_test(void)
{
	bool ok = true;
	paddr_t page = pmm_alloc_page();
	volatile u32 *through_direct_map;

	if (!page) {
		kputs("  vm: could not allocate a page to test with\n");
		return false;
	}

	/* The direct map has to actually reach the page the allocator just
	 * handed out, and a write through it has to be visible when the same
	 * physical address is read back by a different route. */
	through_direct_map = phys_to_virt(page);
	*through_direct_map = 0x5245434FU;	/* 'RECO' */

	if (vm_lookup((vaddr_t)through_direct_map) != page) {
		kputs("  vm: the direct map does not resolve to the page it points at\n");
		ok = false;
	}

	if (*through_direct_map != 0x5245434FU) {
		kputs("  vm: a write through the direct map did not read back\n");
		ok = false;
	}

	/* The kernel's own code is still where it was, which is the thing that
	 * would have gone wrong at the CR3 write rather than here -- but if it
	 * had gone subtly wrong instead of fatally, this is what would catch it. */
	if (vm_lookup((vaddr_t)(KERNEL_VMA + (u64)(uintptr_t)__kernel_phys_start)) !=
	    (paddr_t)(uintptr_t)__kernel_phys_start) {
		kputs("  vm: the kernel image is not where it is linked\n");
		ok = false;
	}

	/* And the other half of the same claim, which is the one checkpoint 10
	 * actually bought: the address the kernel was *loaded* at means nothing
	 * any more. Without this the move would be half done -- the kernel
	 * reachable from the top and still sitting in the middle of every
	 * program's address space -- and nothing would say so. */
	if (vm_lookup((vaddr_t)(uintptr_t)__kernel_phys_start) != 0) {
		kputs("  vm: the kernel is still mapped where it was loaded, so "
		      "the lower half is not free after all\n");
		ok = false;
	}

	/* --- taking a mapping away, and proving the processor believes it ---
	 *
	 * Two separate claims, and only the second is hard.
	 *
	 * The easy one is that the entry is gone from the tables, which
	 * `vm_lookup` answers by walking them.
	 *
	 * The one that matters is that the *translation* is gone, which is a
	 * different question: a cached translation is consulted before the table
	 * it came from, so a page table can be perfect and the processor still
	 * reach the old page. That is exactly the fault checkpoint 10 found,
	 * where the second user program read the first program's memory through
	 * tables that were correct.
	 *
	 * So this maps a second, differently-filled page at the same address and
	 * reads it. If the unmap forgot to invalidate, the entry it left behind
	 * is not present -- so the map that follows sees nothing live, skips its
	 * own invalidation, and the stale translation survives to answer with the
	 * *first* page. The read is the assertion; every table involved would
	 * look right.
	 */
	{
		/* A slot nothing else uses: above the direct map, below the
		 * kernel image, and never mapped by anything at boot. */
		const vaddr_t at = 0xFFFF900000000000ULL;
		paddr_t first = pmm_alloc_page();
		paddr_t second = pmm_alloc_page();

		if (!first || !second) {
			kputs("  vm: could not allocate two pages to test "
			      "unmapping with\n");
			ok = false;
		} else {
			volatile u32 *seen = (volatile u32 *)at;

			*(volatile u32 *)phys_to_virt(first)  = 0x1111FFFFU;
			*(volatile u32 *)phys_to_virt(second) = 0x2222FFFFU;

			if (!vm_map(at, first, PAGE_SIZE, VM_READ | VM_WRITE) ||
			    *seen != 0x1111FFFFU) {
				kputs("  vm: the page it was about to unmap was "
				      "not readable in the first place\n");
				ok = false;
			}

			if (!vm_unmap(at, PAGE_SIZE)) {
				kputs("  vm: it refused to unmap a four-kilobyte "
				      "page it had just mapped\n");
				ok = false;
			}

			if (vm_lookup(at) != 0) {
				kputs("  vm: an unmapped address still resolves "
				      "to a page\n");
				ok = false;
			}

			if (!vm_map(at, second, PAGE_SIZE, VM_READ | VM_WRITE)) {
				kputs("  vm: could not map a second page where "
				      "the first had been\n");
				ok = false;
			} else if (*seen == 0x1111FFFFU) {
				kputs("  vm: after unmapping and mapping another "
				      "page, the address still reads the first "
				      "one -- the translation was never "
				      "invalidated\n");
				ok = false;
			} else if (*seen != 0x2222FFFFU) {
				kputs("  vm: the address reads neither page\n");
				ok = false;
			}

			vm_unmap(at, PAGE_SIZE);
			pmm_free_page(first);
			pmm_free_page(second);
		}
	}

	/* Unmapping something that was never mapped is not an error. A caller
	 * unwinding a range it only partly built has to be able to say "take all
	 * of this away" without tracking how far it got. */
	if (!vm_unmap(0xFFFF900000000000ULL, PAGE_SIZE)) {
		kputs("  vm: unmapping an address that was not mapped was "
		      "reported as a failure\n");
		ok = false;
	}

	pmm_free_page(page);
	return ok;
}

/* --- Telling the other processors a translation has changed ---------------
 *
 * `invlpg` invalidates the processor that executes it and no other. There is no
 * broadcast form of it; on x86 the only way to reach the others is to interrupt
 * them and ask.
 *
 * **This is a gap checkpoint 9b created rather than one it found.** With one
 * processor, unmapping a page and invalidating locally was complete. The moment
 * the others were woken, every one of them could be holding a cached
 * translation for an address this processor has just changed -- and it would go
 * on using it. Not a crash: a read of memory that is no longer what the page
 * tables say it is, on a processor that never fails.
 *
 * The other architecture needs none of this. `tlbi vaae1is` carries an Inner
 * Shareable suffix, and the hardware broadcasts the invalidation to every
 * processor in the domain. The same line in two files: one architecture does it
 * for you and the other does not, which is exactly the kind of difference that
 * survives review because both look correct.
 *
 * --- What this deliberately does not try to be ---
 *
 * There is one shootdown at a time, guarded by a lock, and it invalidates one
 * address. A real implementation batches ranges and tracks which processors
 * could possibly hold the mapping. Both are optimisations of something that has
 * to be correct first, and neither changes what a caller sees.
 */

/* The request in flight, and the count of processors that have honoured it.
 * Volatile because two processors read and write these and the compiler has no
 * reason to expect either to change under it. */
static volatile vaddr_t shootdown_address;
static volatile unsigned shootdown_acks;
static struct spinlock shootdown_lock = SPINLOCK_INIT("shootdown");

/* Runs on every *other* processor, from the interrupt handler. */
void x86_tlb_shootdown_service(void)
{
	__asm__ volatile("invlpg (%0)" : : "r"(shootdown_address) : "memory");

	/* Released, so that the processor waiting on this count cannot see it
	 * rise before the invalidation above has actually happened. */
	__atomic_add_fetch(&shootdown_acks, 1, __ATOMIC_RELEASE);
}

static void shoot_down_others(vaddr_t va)
{
	unsigned others = smp_cpus_online();
	u64 flags;
	u64 deadline;

	/* Nothing to tell, and this is the common case: one processor, or a
	 * machine still bringing the others up. */
	if (others <= 1)
		return;

	others -= 1;

	flags = spin_lock_irq(&shootdown_lock);

	shootdown_address = va;
	shootdown_acks = 0;

	/* The address must be visible before the interrupt that asks anybody to
	 * read it. */
	__atomic_thread_fence(__ATOMIC_RELEASE);

	shootdowns_sent++;
	x86_apic_send_ipi_all_but_self(VECTOR_TLB_SHOOTDOWN);

	/* Waited for, because the point of a shootdown is that it has happened
	 * by the time the caller continues -- a caller that unmaps a page and
	 * frees it while another processor still has the translation has handed
	 * that page to somebody else while it is still readable.
	 *
	 * Bounded, because a processor that has stopped answering must not take
	 * the machine with it. The bound is generous: this is a few hundred
	 * cycles of work on the far side.
	 */
	deadline = time_monotonic_ns() + 100ull * 1000 * 1000;	/* 100 ms */

	while (__atomic_load_n(&shootdown_acks, __ATOMIC_ACQUIRE) < others &&
	       time_monotonic_ns() < deadline)
		arch_cpu_relax();

	if (__atomic_load_n(&shootdown_acks, __ATOMIC_ACQUIRE) < others)
		shootdown_timeouts++;

	spin_unlock_irq(&shootdown_lock, flags);
}

unsigned x86_tlb_shootdown_timeouts(void)
{
	return shootdown_timeouts;
}

unsigned x86_tlb_shootdowns_sent(void)
{
	return shootdowns_sent;
}

void vm_print_shootdowns(void)
{
	kprintf("  shootdowns   : %u live invalidations, %u sent, %u unanswered\n",
		live_invalidations, shootdowns_sent, shootdown_timeouts);
}

/* --- how recently a page was touched -------------------------------------
 *
 * PTE_ACCESSED has been defined at the top of this file since paging was
 * written and has never been read. The processor sets it when it fills a TLB
 * entry for the page; software clears it, waits, and looks again.
 *
 * THE INVALIDATION IS NOT OPTIONAL, and leaving it out produces the worst kind
 * of wrong answer. The processor writes the bit when it *fills* a translation,
 * not on every access -- so a page whose entry is still cached is touched a
 * million times and the bit stays clear. Every hot page would report as cold
 * from the second sweep onward, and an eviction policy built on that would
 * evict exactly the pages in use.
 *
 * It is the same reason vm_map invalidates, and it is here rather than left to
 * the caller because a caller that forgot would get plausible numbers.
 */
static u64 *leaf_entry_no_present(vaddr_t va)
{
	unsigned i4 = (unsigned)((va >> 39) & 0x1FF);
	unsigned i3 = (unsigned)((va >> 30) & 0x1FF);
	unsigned i2 = (unsigned)((va >> 21) & 0x1FF);
	unsigned i1 = (unsigned)((va >> 12) & 0x1FF);

	u64 *pdpt, *pd, *pt;

	pdpt = next_level(root_for(va), i4, false);
	if (!pdpt)
		return 0;

	/* A large page has its accessed bit in the entry that describes it, and
	 * that entry covers a gigabyte or two megabytes rather than a page. The
	 * bit is still readable and still means "something in here was
	 * touched", which is a different measurement from the one this is for
	 * -- so it is refused rather than silently answered at the wrong
	 * granularity. Nothing user-facing is mapped with large pages. */
	if (pdpt[i3] & PTE_LARGE)
		return 0;

	pd = next_level(pdpt, i3, false);
	if (!pd)
		return 0;

	if (pd[i2] & PTE_LARGE)
		return 0;

	pt = next_level(pd, i2, false);
	if (!pt)
		return 0;

	/* The slot, whether or not anything is in it. A swapped page
	 * has an entry that is deliberately *not* present, so a walk
	 * that stopped at absent could never find one. */
	return &pt[i1];
}

static u64 *leaf_entry(vaddr_t va)
{
	u64 *entry = leaf_entry_no_present(va);

	return (entry && (*entry & PTE_PRESENT)) ? entry : 0;
}

bool vm_page_touched(vaddr_t va, bool clear)
{
	vaddr_t page = va & ~(vaddr_t)(PAGE_SIZE - 1);
	u64 *entry = leaf_entry(page);
	bool was;

	if (!entry)
		return false;

	was = (*entry & PTE_ACCESSED) != 0;

	if (clear && was) {
		*entry &= ~PTE_ACCESSED;

		/* And the translation goes, on this processor and every other.
		 * Without it the entry stays cached and the bit is never
		 * written again. */
		invalidate_if_live(*entry, page);
	}

	return was;
}

bool vm_page_age_is_cheap(void)
{
	/* The processor maintains the bit itself. Sampling costs a table walk
	 * and an invalidation per page, and no faults at all. */
	return true;
}

u64 vm_page_age_faults(void)
{
	/* None. The processor writes the bit as part of filling a translation,
	 * so nothing traps and there is nothing to count. */
	return 0;
}

/* --- an entry that names a swap slot instead of a page --------------------
 *
 * A page-table entry with the present bit clear is ignored by the processor
 * entirely -- every other bit in it is software's. That is what makes swapping
 * possible without a second table: the entry stays where it is and stops being a
 * mapping, while still saying where the page went.
 *
 * The encoding puts the slot at bit 12 and upward, where a physical address
 * would have been, and sets bit 1 as the marker. Bit 1 is the writable bit when
 * an entry is present and means nothing when it is not.
 *
 * The marker matters for one specific reason: an entry of *zero* is the ordinary
 * "nothing was ever here" case, and a slot number alone at bit 12 could not be
 * told from it if the slot were zero. The store never hands out slot zero, so
 * either check would do -- and having both means the two conditions have to
 * disagree before anything is misread, rather than one of them being load
 * bearing on its own.
 */
#define PTE_SWAPPED   (1ULL << 1)
#define PTE_SLOT_SHIFT 12

bool vm_swap_out(vaddr_t va, u32 slot)
{
	vaddr_t page = va & ~(vaddr_t)(PAGE_SIZE - 1);
	u64 *entry = leaf_entry(page);
	u64 old;

	/* leaf_entry refuses a large page and a missing table, which is what
	 * keeps this off anything that is not an ordinary user page. */
	if (!entry || !slot)
		return false;

	old = *entry;

	if (!(old & PTE_PRESENT))
		return false;

	*entry = ((u64)slot << PTE_SLOT_SHIFT) | PTE_SWAPPED;

	/* The translation goes now, not later. A processor still holding the old
	 * entry writes into a page that is about to be handed to somebody else,
	 * and the write lands nowhere anybody will look. */
	invalidate_if_live(old, page);
	return true;
}

u32 vm_swap_slot(vaddr_t va)
{
	vaddr_t page = va & ~(vaddr_t)(PAGE_SIZE - 1);
	u64 *entry = leaf_entry_no_present(page);
	u64 value;

	if (!entry)
		return 0;

	value = *entry;

	/* Both conditions, and they must agree. An entry that is present and
	 * carries the marker is one of the states that should be impossible --
	 * see evict.h -- and answering it either way would be worse than
	 * answering "not swapped" and leaving the page alone. */
	if (value & PTE_PRESENT)
		return 0;

	if (!(value & PTE_SWAPPED))
		return 0;

	return (u32)(value >> PTE_SLOT_SHIFT);
}
