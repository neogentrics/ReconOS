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

#include <recon/kernel/vm.h>
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

paddr_t virt_to_phys(const void *virt)
{
	u64 v = (u64)(uintptr_t)virt;

	if (direct_map_live && v >= DIRECT_MAP_BASE)
		return (paddr_t)(v - DIRECT_MAP_BASE);
	return (paddr_t)v;
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

		/* Intermediate entries are permissive: the CPU takes the *most*
		 * restrictive of the levels for write and user, but the least
		 * restrictive for NX -- so NX must be cleared here and set on
		 * the leaf, or nothing below is ever executable. */
		table[index] = (phys & PTE_ADDR_MASK) | PTE_PRESENT | PTE_WRITE;
		return fresh;
	}

	/* A large page where a table was expected. The caller is trying to map
	 * something inside a range already mapped at a coarser granularity, and
	 * silently splitting it would be a surprise; refuse instead. */
	if (table[index] & PTE_LARGE)
		return 0;

	return table_at(table[index] & PTE_ADDR_MASK);
}

static u64 leaf_bits(unsigned flags, bool large)
{
	u64 bits = PTE_PRESENT;

	if (flags & VM_WRITE)
		bits |= PTE_WRITE;
	if (!(flags & VM_EXEC) && caps.no_execute)
		bits |= PTE_NX;
	if (flags & VM_GLOBAL)
		bits |= PTE_GLOBAL;
	if (flags & VM_DEVICE)
		bits |= PTE_PCD | PTE_PWT;
	if (large)
		bits |= PTE_LARGE;

	return bits;
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

		u64 *pdpt = next_level(kernel_pml4, i4, true);
		u64 *pd, *pt;

		if (!pdpt)
			return false;

		/* Largest page the alignment and the remaining length allow.
		 * Checked in order, so a request that could be one 1GB page is
		 * never made from five hundred and twelve 2MB ones. */
		if (caps.page_1g && size >= SIZE_1G &&
		    !((va | pa) & (SIZE_1G - 1))) {
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

		pt[i1] = (pa & PTE_ADDR_MASK) | leaf_bits(flags, false);
		mapped_4k++;
		va += PAGE_SIZE;
		pa += PAGE_SIZE;
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

	pdpt = next_level(kernel_pml4, i4, false);
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

	/* The kernel image, identity mapped, and this is the mapping the switch
	 * itself depends on: CR3 takes effect on the next instruction fetch, so
	 * the address the CPU is executing from has to mean the same thing in
	 * both maps. Text and data together for now, writable and executable,
	 * because splitting them needs the linker to say where the boundary is
	 * and that is a separate change. */
	{
		paddr_t start = PAGE_ALIGN_DOWN((u64)(uintptr_t)__kernel_start);
		u64 len = PAGE_ALIGN_UP((u64)(uintptr_t)__kernel_end) - start;

		if (!vm_map((vaddr_t)start, start, len,
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

	/* The framebuffer, if there is one, needs to be uncached: it is memory
	 * on a device across a bus, and a write that sits in a cache line is a
	 * pixel that does not appear. */
	if (info->fb.width && info->fb.base) {
		paddr_t base = PAGE_ALIGN_DOWN(info->fb.base);
		u64 len = PAGE_ALIGN_UP(info->fb.size + (info->fb.base - base));

		vm_map(DIRECT_MAP_BASE + base, base, len,
		       VM_READ | VM_WRITE | VM_DEVICE | VM_GLOBAL);
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
	if (vm_lookup((vaddr_t)(uintptr_t)__kernel_start) !=
	    (paddr_t)(uintptr_t)__kernel_start) {
		kputs("  vm: the kernel image is no longer identity mapped\n");
		ok = false;
	}

	pmm_free_page(page);
	return ok;
}
