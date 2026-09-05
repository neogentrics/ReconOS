/* aarch64 translation tables.
 *
 * Four levels like x86, but the vocabulary and the awkwardnesses are different,
 * and three of them are worth knowing before reading the code.
 *
 * TWO ROOT REGISTERS, NOT ONE. TTBR0_EL1 translates addresses whose top bits
 * are zero and TTBR1_EL1 translates addresses whose top bits are ones. That is
 * a hardware split of the address space into a low half and a high half, and it
 * is why the kernel's direct map at the top costs nothing to keep out of a
 * user process's way -- there is no shared table to walk past.
 *
 * MEMORY TYPE IS AN INDEX, NOT A FLAG. A descriptor holds three bits selecting
 * one of eight attribute bytes in MAIR_EL1. Getting it wrong on x86 makes
 * hardware slow; here it makes hardware *wrong* -- Normal memory permits
 * speculative reads, write merging and reordering, all of which destroy a UART
 * that expects one byte per store.
 *
 * THE ACCESS FLAG IS NOT OPTIONAL. Bit 10 set, on every leaf, or the first
 * touch takes an access flag fault. It exists so an operating system can track
 * which pages are used; a kernel that does not track that yet must still set
 * it, and forgetting is a fault that looks like the mapping never happened.
 */
#include "aarch64.h"

#include <recon/kernel/vm.h>
#include <recon/kernel/arch.h>
#include <recon/kernel/boot.h>
#include <recon/kernel/cpu.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/panic.h>

/* Descriptor types. Bit 1 distinguishes a table from a block at levels 1 and 2;
 * at level 3 the same encoding means "page", which is a wart of the format
 * rather than anything meaningful. */
#define DESC_INVALID 0x0ULL
#define DESC_BLOCK   0x1ULL
#define DESC_TABLE   0x3ULL
#define DESC_PAGE    0x3ULL

/* Lower attributes. */
#define ATTR_IDX(n)  ((u64)(n) << 2)
#define ATTR_NS      (1ULL << 5)
#define ATTR_AP_RW   (0ULL << 6)	/* read/write at EL1, nothing at EL0 */
#define ATTR_AP_RO   (2ULL << 6)	/* read-only at EL1 */
#define ATTR_SH_INNER (3ULL << 8)	/* inner shareable */
#define ATTR_AF      (1ULL << 10)
#define ATTR_NG      (1ULL << 11)

/* Upper attributes: execute-never, separately for each privilege level. */
#define ATTR_PXN     (1ULL << 53)
#define ATTR_UXN     (1ULL << 54)

/* Which MAIR slot means what. Programmed into MAIR_EL1 below. */
#define MAIR_DEVICE  0		/* Device-nGnRnE: no gathering, reordering or early ack */
#define MAIR_NORMAL  1		/* Normal, write-back, read and write allocate */

#define MAIR_VALUE   ((0x00ULL << (8 * MAIR_DEVICE)) | \
		      (0xFFULL << (8 * MAIR_NORMAL)))

#define ADDR_MASK 0x0000FFFFFFFFF000ULL

#define SIZE_2M (2ULL * 1024 * 1024)
#define SIZE_1G (1024ULL * 1024 * 1024)

#define DIRECT_MAP_BASE 0xFFFF800000000000ULL

/* Two roots, because the hardware has two root registers. */
static u64 *ttbr0_root;		/* low half: the kernel image, identity mapped */
static u64 *ttbr1_root;		/* high half: the direct map */

/* Kept because the pointers above stop being valid the moment the tables are
 * installed: they were obtained through the map we were handed, and the new map
 * covers only what the kernel meant to map. Re-pointed at the end of
 * vm_init(). */
static paddr_t ttbr0_phys, ttbr1_phys;
static bool direct_map_live;

static u64 mapped_1g, mapped_2m, mapped_4k, table_pages;

static struct cpu_caps caps;

void *phys_to_virt(paddr_t phys)
{
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

static u64 *next_level(u64 *table, unsigned index, bool create)
{
	u64 entry = table[index];

	if ((entry & 3) == DESC_TABLE)
		return table_at(entry & ADDR_MASK);

	if ((entry & 3) == DESC_BLOCK)
		return 0;	/* already mapped coarsely; refuse to split silently */

	if (!create)
		return 0;

	{
		u64 *fresh = alloc_table();

		if (!fresh)
			return 0;

		/* Table descriptors carry no permissions here. ARM has
		 * hierarchical permission bits in the upper attributes of a
		 * table entry, and leaving them clear means "impose nothing",
		 * so the leaf alone decides. That is the behaviour the rest of
		 * this file assumes. */
		table[index] = (virt_to_phys(fresh) & ADDR_MASK) | DESC_TABLE;
		return fresh;
	}
}

static u64 leaf_attrs(unsigned flags, bool block)
{
	u64 a = block ? DESC_BLOCK : DESC_PAGE;

	a |= ATTR_AF;

	if (flags & VM_DEVICE) {
		a |= ATTR_IDX(MAIR_DEVICE);
		/* Device memory is not cacheable, so shareability is
		 * meaningless for it and left alone. */
	} else {
		a |= ATTR_IDX(MAIR_NORMAL) | ATTR_SH_INNER;
	}

	a |= (flags & VM_WRITE) ? ATTR_AP_RW : ATTR_AP_RO;

	/* Never executable from EL0 -- nothing the kernel maps is user code.
	 * Executable from EL1 only where asked. Device memory is never
	 * executable, which matters: a speculative instruction fetch from a
	 * memory-mapped register is a real way to hang a bus. */
	a |= ATTR_UXN;
	if (!(flags & VM_EXEC) || (flags & VM_DEVICE))
		a |= ATTR_PXN;

	return a;
}

/* Which root a virtual address belongs to. The hardware decides this by the
 * top bits, and so does this function, for the same reason. */
static u64 *root_for(vaddr_t va)
{
	return (va >> 63) ? ttbr1_root : ttbr0_root;
}

bool vm_map(vaddr_t va, paddr_t pa, u64 size, unsigned flags)
{
	if ((va | pa | size) & (PAGE_SIZE - 1))
		panic("vm_map: unaligned request");

	while (size) {
		u64 *l0 = root_for(va);
		unsigned i0 = (unsigned)((va >> 39) & 0x1FF);
		unsigned i1 = (unsigned)((va >> 30) & 0x1FF);
		unsigned i2 = (unsigned)((va >> 21) & 0x1FF);
		unsigned i3 = (unsigned)((va >> 12) & 0x1FF);

		u64 *l1 = next_level(l0, i0, true);
		u64 *l2, *l3;

		if (!l1)
			return false;

		if (caps.page_1g && size >= SIZE_1G &&
		    !((va | pa) & (SIZE_1G - 1))) {
			l1[i1] = (pa & ADDR_MASK) | leaf_attrs(flags, true);
			mapped_1g++;
			va += SIZE_1G; pa += SIZE_1G; size -= SIZE_1G;
			continue;
		}

		l2 = next_level(l1, i1, true);
		if (!l2)
			return false;

		if (caps.page_2m && size >= SIZE_2M &&
		    !((va | pa) & (SIZE_2M - 1))) {
			l2[i2] = (pa & ADDR_MASK) | leaf_attrs(flags, true);
			mapped_2m++;
			va += SIZE_2M; pa += SIZE_2M; size -= SIZE_2M;
			continue;
		}

		l3 = next_level(l2, i2, true);
		if (!l3)
			return false;

		l3[i3] = (pa & ADDR_MASK) | leaf_attrs(flags, false);
		mapped_4k++;
		va += PAGE_SIZE; pa += PAGE_SIZE; size -= PAGE_SIZE;
	}

	return true;
}

paddr_t vm_lookup(vaddr_t va)
{
	u64 *l0 = root_for(va);
	unsigned i0 = (unsigned)((va >> 39) & 0x1FF);
	unsigned i1 = (unsigned)((va >> 30) & 0x1FF);
	unsigned i2 = (unsigned)((va >> 21) & 0x1FF);
	unsigned i3 = (unsigned)((va >> 12) & 0x1FF);
	u64 *l1, *l2, *l3;

	if (!l0)
		return 0;

	l1 = next_level(l0, i0, false);
	if (!l1)
		return 0;

	if ((l1[i1] & 3) == DESC_BLOCK)
		return (l1[i1] & ADDR_MASK) + (va & (SIZE_1G - 1));

	l2 = next_level(l1, i1, false);
	if (!l2)
		return 0;

	if ((l2[i2] & 3) == DESC_BLOCK)
		return (l2[i2] & ADDR_MASK) + (va & (SIZE_2M - 1));

	l3 = next_level(l2, i2, false);
	if (!l3 || (l3[i3] & 3) != DESC_PAGE)
		return 0;

	return (l3[i3] & ADDR_MASK) + (va & (PAGE_SIZE - 1));
}

/* IPS / PS field encoding, shared with the CPU's PARange. */
static u64 ips_from_parange(unsigned phys_bits)
{
	switch (phys_bits) {
	case 32: return 0;
	case 36: return 1;
	case 40: return 2;
	case 42: return 3;
	case 44: return 4;
	case 48: return 5;
	case 52: return 6;
	default: return 2;	/* 40 bits: the conservative choice, always legal */
	}
}

static void activate(void)
{
	u64 tcr, sctlr;

	/* T0SZ and T1SZ of 16 give 48-bit address spaces at both ends.
	 * TG0 and TG1 both select a 4KB granule -- and note they use
	 * *different encodings* for the same size, which is a genuine trap in
	 * the architecture: 0b00 for TTBR0 and 0b10 for TTBR1.
	 * IRGN/ORGN of 0b01 make the table walker use the caches, which is what
	 * lets the tables be written normally and read by hardware without
	 * explicit cache maintenance. */
	tcr = (16ULL << 0)		/* T0SZ */
	    | (1ULL << 8)		/* IRGN0: write-back, write-allocate */
	    | (1ULL << 10)		/* ORGN0: write-back, write-allocate */
	    | (3ULL << 12)		/* SH0: inner shareable */
	    | (0ULL << 14)		/* TG0: 4KB */
	    | (16ULL << 16)		/* T1SZ */
	    | (1ULL << 24)		/* IRGN1 */
	    | (1ULL << 26)		/* ORGN1 */
	    | (3ULL << 28)		/* SH1 */
	    | (2ULL << 30)		/* TG1: 4KB, and yes a different encoding */
	    | (ips_from_parange(caps.phys_addr_bits) << 32);

	__asm__ volatile(
		"dsb sy\n"
		"msr mair_el1, %0\n"
		"msr tcr_el1, %1\n"
		"msr ttbr0_el1, %2\n"
		"msr ttbr1_el1, %3\n"
		"isb\n"
		/* Every translation cached from the map we were handed is now
		 * wrong. Invalidate the lot before anything can use one. */
		"tlbi vmalle1\n"
		"dsb sy\n"
		"isb\n"
		:
		: "r"(MAIR_VALUE), "r"(tcr),
		  "r"((u64)ttbr0_phys), "r"((u64)ttbr1_phys)
		: "memory");

	/* The MMU may already be on -- it is when the firmware started us, and
	 * it is not when a bare loader did. Reading SCTLR rather than assuming
	 * means one code path serves both, and the write is harmless when the
	 * bits are already set. */
	__asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
	sctlr |= (1ULL << 0)	/* M: MMU on */
	       | (1ULL << 2)	/* C: data cache on */
	       | (1ULL << 12);	/* I: instruction cache on */
	__asm__ volatile("msr sctlr_el1, %0\n isb\n" : : "r"(sctlr) : "memory");
}

void vm_init(void)
{
	const struct boot_info *info = boot_info();

	arch_cpu_caps(&caps);

	ttbr0_root = alloc_table();
	ttbr1_root = alloc_table();
	if (!ttbr0_root || !ttbr1_root)
		panic("vm: no memory for the root translation tables");

	/* The kernel image, identity mapped in the low half. This is the
	 * mapping the switch itself stands on: the instruction after the one
	 * that changes TTBR is fetched through the new tables. */
	{
		paddr_t start = PAGE_ALIGN_DOWN((u64)(uintptr_t)__kernel_start);
		u64 len = PAGE_ALIGN_UP((u64)(uintptr_t)__kernel_end) - start;

		if (!vm_map((vaddr_t)start, start, len,
			    VM_READ | VM_WRITE | VM_EXEC | VM_GLOBAL))
			panic("vm: could not map the kernel image");
	}

	/* The machine's fixed hardware, as Device memory, identity mapped.
	 *
	 * The console first and for its own reason: without it the kernel goes
	 * silent the instant the tables change, and a kernel that cannot report
	 * why it stopped is very hard to fix.
	 *
	 * The others are here because they are touched before any driver exists
	 * to discover them. Every one of these was added after something faulted
	 * reaching for it -- which is the right way round, since a map that
	 * covers what nothing uses is a map that hides what nothing checked. */
	{
		static const paddr_t fixed_devices[] = {
			PL011_BASE,	/* console */
			GICD_BASE,	/* interrupt controller, distributor */
			GICC_BASE,	/* interrupt controller, CPU interface */
			PL031_BASE,	/* real-time clock */
		};

		for (unsigned i = 0; i < RK_ARRAY_LEN(fixed_devices); i++)
			if (!vm_map((vaddr_t)fixed_devices[i], fixed_devices[i],
				    PAGE_SIZE * 16,
				    VM_READ | VM_WRITE | VM_DEVICE | VM_GLOBAL))
				panic("vm: could not map the machine's fixed hardware");
	}

	/* Physical memory in the high half. Unlike x86 this is done region by
	 * region rather than as one span, because the memory type has to be
	 * right: RAM is Normal and everything else is Device, and mapping a
	 * memory-mapped register as Normal permits speculative reads and write
	 * merging that make the hardware behave randomly rather than slowly. */
	for (unsigned i = 0; i < info->region_count; i++) {
		const struct mem_region *r = &info->regions[i];
		paddr_t base = PAGE_ALIGN_DOWN(r->base);
		u64 len = PAGE_ALIGN_UP(r->base + r->size) - base;
		bool is_ram;

		switch (r->kind) {
		case MEM_USABLE:
		case MEM_BOOTLOADER:
		case MEM_KERNEL:
		case MEM_ACPI_RECLAIM:
		case MEM_ACPI_NVS:
			is_ram = true;
			break;
		default:
			is_ram = false;
			break;
		}

		vm_map(DIRECT_MAP_BASE + base, base, len,
		       VM_READ | VM_WRITE | VM_GLOBAL |
		       (is_ram ? 0u : VM_DEVICE));
	}

	if (info->fb.width && info->fb.base) {
		paddr_t base = PAGE_ALIGN_DOWN(info->fb.base);
		u64 len = PAGE_ALIGN_UP(info->fb.size + (info->fb.base - base));

		vm_map(DIRECT_MAP_BASE + base, base, len,
		       VM_READ | VM_WRITE | VM_DEVICE | VM_GLOBAL);
	}

	ttbr0_phys = virt_to_phys(ttbr0_root);
	ttbr1_phys = virt_to_phys(ttbr1_root);

	activate();

	direct_map_live = true;

	/* Both roots were reached through the map we were handed. Re-point them
	 * through the direct map before anything walks them. */
	ttbr0_root = table_at(ttbr0_phys);
	ttbr1_root = table_at(ttbr1_phys);

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

	if (vm_lookup((vaddr_t)(uintptr_t)__kernel_start) !=
	    (paddr_t)(uintptr_t)__kernel_start) {
		kputs("  vm: the kernel image is no longer identity mapped\n");
		ok = false;
	}

	pmm_free_page(page);
	return ok;
}
