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
#include <recon/kernel/pageage.h>
#include <recon/kernel/evict.h>
#include <recon/kernel/arch.h>
#include <recon/kernel/smp.h>
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
#define ATTR_AP_RW      (0ULL << 6)	/* read/write at EL1, nothing at EL0 */
#define ATTR_AP_USER_RW (1ULL << 6)	/* read/write at both levels */
#define ATTR_AP_RO      (2ULL << 6)	/* read-only at EL1, nothing at EL0 */
#define ATTR_AP_USER_RO (3ULL << 6)	/* read-only at both levels */
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
static void invalidate_if_live(u64 entry, vaddr_t va)
{
	if ((entry & 3) == DESC_INVALID)
		return;

	/* The address goes in as a page number, not a byte address, and the
	 * ordering around it is not optional: the table write has to be visible
	 * before the invalidation, and the invalidation complete before any
	 * instruction that might use the new translation is fetched. */
	__asm__ volatile(
		"dsb ishst\n"
		"tlbi vaae1is, %0\n"
		"dsb ish\n"
		"isb\n"
		:
		: "r"(va >> 12)
		: "memory");
}

static u64 leaf_attrs(unsigned flags, bool block)
{
	u64 a = block ? DESC_BLOCK : DESC_PAGE;

	a |= ATTR_AF;

	if (flags & (VM_DEVICE | VM_WRITE_COMBINE)) {
		/* Write-combining falls back to device memory here, and that
		 * is a deliberate choice rather than an omission.
		 *
		 * This architecture can express something close to it -- Normal
		 * Non-Cacheable, which permits gathering -- but it would need
		 * another MAIR entry and the only caller is a framebuffer that
		 * this architecture's firmware does not currently provide one
		 * of. Falling back is correct and slower; guessing at an
		 * attribute nothing exercises is neither. */
		a |= ATTR_IDX(MAIR_DEVICE);
		/* Device memory is not cacheable, so shareability is
		 * meaningless for it and left alone. */
	} else {
		a |= ATTR_IDX(MAIR_NORMAL) | ATTR_SH_INNER;
	}

	/* The access permission bits carry both questions at once on this
	 * architecture: who may reach it, and whether they may write. Four
	 * combinations, and picking the wrong one is the difference between a
	 * page a user program cannot see and one it can rewrite. */
	if (flags & VM_USER)
		a |= (flags & VM_WRITE) ? ATTR_AP_USER_RW : ATTR_AP_USER_RO;
	else
		a |= (flags & VM_WRITE) ? ATTR_AP_RW : ATTR_AP_RO;

	/* Execute-never, separately for each privilege level.
	 *
	 * A kernel mapping is never executable from EL0 -- nothing the kernel
	 * maps is user code. A *user* mapping is never executable from EL1,
	 * which is the more important half: it means the kernel cannot be talked
	 * into running a user program's bytes with kernel privilege, which is
	 * the shape of a whole family of exploits.
	 *
	 * Device memory is never executable at all: a speculative instruction
	 * fetch from a memory-mapped register is a real way to hang a bus. */
	if (flags & VM_USER) {
		a |= ATTR_PXN;			/* never executable by the kernel */
		if (!(flags & VM_EXEC) || (flags & VM_DEVICE))
			a |= ATTR_UXN;
	} else {
		a |= ATTR_UXN;			/* never executable by user mode */
		if (!(flags & VM_EXEC) || (flags & VM_DEVICE))
			a |= ATTR_PXN;
	}

	return a;
}

/* Which root a virtual address belongs to. The hardware decides this by the
 * top bits, and so does this function, for the same reason. */
/* Held per processor: two processors run two different programs at the same
 * instant, so there is no single answer to "which address space is current".
 * Null means the kernel's own. */
static u64 *active_user_root[MAX_CPUS];

static u64 *root_for(vaddr_t va)
{
	u64 *user;

	if (va >> 63)
		return ttbr1_root;

	user = active_user_root[arch_cpu_id()];
	return user ? user : ttbr0_root;
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
			invalidate_if_live(l1[i1], va);
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
			invalidate_if_live(l2[i2], va);
			l2[i2] = (pa & ADDR_MASK) | leaf_attrs(flags, true);
			mapped_2m++;
			va += SIZE_2M; pa += SIZE_2M; size -= SIZE_2M;
			continue;
		}

		l3 = next_level(l2, i2, true);
		if (!l3)
			return false;

		invalidate_if_live(l3[i3], va);
		l3[i3] = (pa & ADDR_MASK) | leaf_attrs(flags, false);
		mapped_4k++;
		va += PAGE_SIZE; pa += PAGE_SIZE; size -= PAGE_SIZE;
	}

	return true;
}

/* Takes a mapping away. See the x86_64 file for the reasoning, which is the
 * same on both: the entry is cleared before the invalidation rather than after,
 * a large mapping is never split to satisfy a small request, and nothing is
 * freed because this cannot know who else points at the page.
 *
 * The one difference is that the invalidation here reaches every processor by
 * itself -- `tlbi vaae1is` carries an Inner Shareable suffix and the hardware
 * broadcasts it. The x86_64 side needs an interrupt and an acknowledgement to
 * achieve the same sentence.
 */
bool vm_unmap(vaddr_t va, u64 size)
{
	if ((va | size) & (PAGE_SIZE - 1))
		panic("vm_unmap: unaligned request");

	while (size) {
		u64 *l0 = root_for(va);
		unsigned i0 = (unsigned)((va >> 39) & 0x1FF);
		unsigned i1 = (unsigned)((va >> 30) & 0x1FF);
		unsigned i2 = (unsigned)((va >> 21) & 0x1FF);
		unsigned i3 = (unsigned)((va >> 12) & 0x1FF);

		u64 *l1 = next_level(l0, i0, false);
		u64 *l2, *l3;
		u64 old;

		/* Nothing there is not an error: a caller tearing down a range
		 * it only partly mapped is doing the right thing. */
		if (!l1) {
			va += PAGE_SIZE;
			size -= PAGE_SIZE;
			continue;
		}

		if ((l1[i1] & 3) == DESC_BLOCK) {
			if (size < SIZE_1G || (va & (SIZE_1G - 1)))
				return false;

			old = l1[i1];
			l1[i1] = DESC_INVALID;
			invalidate_if_live(old, va);
			mapped_1g--;
			va += SIZE_1G;
			size -= SIZE_1G;
			continue;
		}

		l2 = next_level(l1, i1, false);
		if (!l2) {
			va += PAGE_SIZE;
			size -= PAGE_SIZE;
			continue;
		}

		if ((l2[i2] & 3) == DESC_BLOCK) {
			if (size < SIZE_2M || (va & (SIZE_2M - 1)))
				return false;

			old = l2[i2];
			l2[i2] = DESC_INVALID;
			invalidate_if_live(old, va);
			mapped_2m--;
			va += SIZE_2M;
			size -= SIZE_2M;
			continue;
		}

		l3 = next_level(l2, i2, false);
		if (l3 && (l3[i3] & 3) != DESC_INVALID) {
			old = l3[i3];
			l3[i3] = DESC_INVALID;
			invalidate_if_live(old, va);
			mapped_4k--;
		}

		va += PAGE_SIZE;
		size -= PAGE_SIZE;
	}

	return true;
}

/* How many pages have been touched for the first time since their flag
 * was cleared. On a processor with no hardware update this is the cost of
 * measuring, one trap at a time, and it is reported rather than absorbed. */
static unsigned access_flag_faults;

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

/* Turns this processor's MMU on using the tables the boot processor built.
 *
 * Called by the boot processor once, from vm_init(), and by every secondary on
 * itself. The tables are shared; what is per-processor is the registers that
 * point at them, which is why this is a separate function rather than part of
 * vm_init(). */
void vm_activate_this_cpu(void)
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

	/* The kernel image, at the address it is linked at. This is the mapping
	 * the switch itself stands on: the instruction after the one that
	 * changes TTBR is fetched through the new tables, and by the time this
	 * runs the processor is already executing high -- boot.S moved it there
	 * before any C ran.
	 *
	 * There is deliberately no identity mapping of it. Leaving one would
	 * keep a copy of the kernel in the lower half of every address space,
	 * which is exactly what moving the kernel was meant to stop. */
	{
		paddr_t start = PAGE_ALIGN_DOWN((u64)(uintptr_t)__kernel_phys_start);
		u64 len = PAGE_ALIGN_UP((u64)(uintptr_t)__kernel_phys_end) - start;

		if (!vm_map((vaddr_t)(KERNEL_VMA + start), start, len,
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
		/* Each one carries its own size, because they are not all the
		 * same size. Sixteen pages was enough for every register block
		 * here until the GICv3 redistributors, which are a *pair* of
		 * 64KiB frames per processor laid end to end -- and a machine
		 * with sixteen processors puts the sixteenth frame two
		 * megabytes past the start. Mapping them all at one fixed size
		 * left the far frames unmapped, and the read that walked to
		 * them took a translation fault. */
		static const struct {
			paddr_t base;
			unsigned pages;
		} fixed_devices[] = {
			{ PL011_BASE, 16 },	/* console */
			{ GICD_BASE,  16 },	/* interrupt controller */
			{ GICC_BASE,  16 },	/* its v2 CPU interface */
			{ PL031_BASE, 16 },	/* real-time clock */

			/* Sixty-four redistributor pairs: more processors than
			 * this kernel can hold, deliberately, so that a machine
			 * larger than MAX_CPUS can still be *walked* and
			 * reported rather than faulting while being counted. */
			{ GICR_BASE, 64 * 0x20000 / PAGE_SIZE },
		};

		for (unsigned i = 0; i < RK_ARRAY_LEN(fixed_devices); i++)
			if (!vm_map(DIRECT_MAP_BASE + fixed_devices[i].base,
				    fixed_devices[i].base,
				    PAGE_SIZE * fixed_devices[i].pages,
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

	vm_activate_this_cpu();

	/* First, and before anything can want to print. The map that was live a
	 * moment ago had the UART at its physical address; this one has it in
	 * the direct map, and between these two statements a single kprintf
	 * would fault on a device that no longer exists where the driver
	 * believes it is. */
	aarch64_device_offset = DIRECT_MAP_BASE;

	direct_map_live = true;

	/* Both roots were reached through the map we were handed. Re-point them
	 * through the direct map before anything walks them. */
	ttbr0_root = table_at(ttbr0_phys);
	ttbr1_root = table_at(ttbr1_phys);

	pmm_remap();
}

/* --- what the kernel has in the half a process is meant to own -------------
 *
 * The same report as x86_64's, and it is worth having on both even though this
 * architecture separates the halves in hardware. TTBR0 translates the low half
 * and TTBR1 the high one, so "give each process its own low half" is a register
 * write here rather than a page-table redesign -- but only if the low half
 * holds nothing the kernel still needs. What it actually holds is the question,
 * and this answers it by walking the live tables rather than by reading the
 * code that built them.
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
	/* Adjacent ranges join only when they are owned the same way: two
	 * neighbours with different owners are exactly what this is looking
	 * for and must not be merged away. */
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

/* Level 4 is the l0 table, level 1 the last. The access-permission field
 * carries both questions at once on this architecture; its low bit is the one
 * that says whether EL0 may reach the page at all. */
static void low_level(struct low_walk *w, u64 *table, unsigned level,
		      vaddr_t base)
{
	u64 span = 1ULL << (12 + 9 * (level - 1));
	unsigned i;

	for (i = 0; i < 512; i++) {
		u64 e = table[i];
		vaddr_t va = base + (vaddr_t)i * span;
		bool user;

		if ((e & 3) == DESC_INVALID)
			continue;

		user = ((e >> 6) & 1) != 0;

		if (level == 1 || (level < 4 && (e & 3) == DESC_BLOCK))
			low_add(w, va, span, user);
		else
			low_level(w, table_at(e & ADDR_MASK), level - 1, va);
	}
}

unsigned vm_user_half_report(void)
{
	struct low_walk w;

	kmemset(&w, 0, sizeof(w));

	kprintf("\nThe user half\n");
	low_level(&w, ttbr0_root, 4, 0);
	low_close(&w);

	if (!w.runs)
		kputs("  nothing is mapped in TTBR0\n");

	kprintf("  kernel-owned : %u of %u range%s\n",
		w.kernel_runs, w.runs, w.runs == 1 ? "" : "s");

	return w.kernel_runs;
}

/* --- an address space of its own -----------------------------------------
 *
 * This architecture already asked the question the x86_64 side had to be
 * taught: TTBR0 translates the low half and TTBR1 the high one, chosen by the
 * top bits of the address, in hardware. So a process address space here is a
 * TTBR0 root and nothing else -- there is no kernel half to copy into it,
 * because the kernel half is a different register that never changes.
 *
 * The whole of a process root is the program's, which is why the teardown walks
 * all 512 entries rather than the lower half only.
 */
paddr_t arch_as_new_root(void)
{
	u64 *root = alloc_table();

	if (!root)
		return 0;

	return virt_to_phys(root);
}

/* Frees the page tables a program's mappings caused to exist. Not the pages
 * they pointed at: this frees tables, and whoever allocated the memory frees
 * the memory. */
static void free_tables(u64 *table, unsigned level)
{
	unsigned i;

	for (i = 0; i < 512; i++) {
		u64 e = table[i];

		if ((e & 3) == DESC_INVALID)
			continue;

		/* A block is a leaf -- memory, not a table. */
		if (level < 4 && (e & 3) == DESC_BLOCK)
			continue;

		if (level > 1) {
			free_tables(table_at(e & ADDR_MASK), level - 1);
			pmm_free_page(e & ADDR_MASK);
			table_pages--;
		}
	}
}

void arch_as_free_root(paddr_t root)
{
	if (!root)
		return;

	free_tables(table_at(root), 4);

	pmm_free_page(root);
	table_pages--;
}

/* Makes an address space the one this processor translates the low half
 * through. Root zero means the kernel's own TTBR0, which holds nothing since
 * the secondary stacks stopped being identity mapped -- so a kernel thread
 * runs with an empty low half, and a stray pointer into it faults rather than
 * finding whatever the last program left.
 *
 * THE FLUSH IS HEAVIER THAN IT NEEDS TO BE, AND THAT IS WRITTEN DOWN RATHER
 * THAN HIDDEN. Nothing here uses ASIDs, so the processor cannot tell one
 * program's translations from another's and the whole of EL1's local set has
 * to go. ASIDs are the fix and they are a register-allocation problem with a
 * recycling policy attached -- worth doing when there is a measurement asking
 * for it, not before. Local rather than broadcast on purpose: another
 * processor's translations of *its* program are not made wrong by this one
 * changing programs.
 */
void arch_as_activate(paddr_t root)
{
	unsigned cpu = arch_cpu_id();
	u64 target = root ? (u64)root : (u64)ttbr0_phys;

	active_user_root[cpu] = root ? table_at(root) : NULL;

	__asm__ volatile(
		"msr ttbr0_el1, %0\n"
		"isb\n"
		"tlbi vmalle1\n"
		"dsb nsh\n"
		"isb\n"
		:
		: "r"(target)
		: "memory");
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

/* Nothing to report. `tlbi ... is` broadcasts to every processor in the inner
 * shareable domain in hardware, so there is no message to send, nothing to wait
 * for, and no way for one to go unanswered. */
void vm_print_shootdowns(void)
{
}

/* --- how recently a page was touched -------------------------------------
 *
 * ATTR_AF has been set on every mapping this kernel has ever made and never
 * read. Clearing it is how you ask "has this been touched since I last looked",
 * and on this architecture the answer arrives in a way it does not on x86_64.
 *
 * **Before ARMv8.1 there is no hardware update of the Access Flag at all.** A
 * valid descriptor with AF clear does not quietly get it set on access -- the
 * access *faults*, with an Access Flag fault, and software sets the bit and
 * returns. Cortex-A72, which is what the rig runs, is one of those cores.
 *
 * That is more accurate than a bit the processor sets when it happens to refill
 * a translation: a fault is exact, and it happens on the first touch rather than
 * at some point afterwards. It is also far more expensive -- one trap per page
 * per sweep -- which is why vm_page_age_is_cheap exists and why a caller is told
 * the answer rather than left to assume the two architectures cost the same.
 *
 * ARMv8.1 and later can do it in hardware, and the field that says so is read
 * below rather than guessed from the processor's name.
 */
static u64 *leaf_entry_any(vaddr_t va)
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

	/* A block descriptor carries the flag for a whole gigabyte or two
	 * megabytes. Readable, and a different measurement from the one this is
	 * for, so it is refused rather than answered at the wrong granularity.
	 */
	if ((l1[i1] & 3) == DESC_BLOCK)
		return 0;

	l2 = next_level(l1, i1, false);
	if (!l2)
		return 0;

	if ((l2[i2] & 3) == DESC_BLOCK)
		return 0;

	l3 = next_level(l2, i2, false);
	if (!l3)
		return 0;

	/* The slot, valid or not. A swapped page has a descriptor that
	 * is deliberately invalid, so a walk that stopped at invalid
	 * could never find one. */
	return &l3[i3];
}

static u64 *leaf_entry(vaddr_t va)
{
	u64 *entry = leaf_entry_any(va);

	return (entry && (*entry & 3) == DESC_PAGE) ? entry : 0;
}

bool vm_page_touched(vaddr_t va, bool clear)
{
	vaddr_t page = va & ~(vaddr_t)(PAGE_SIZE - 1);
	u64 *entry = leaf_entry(page);
	bool was;

	if (!entry)
		return false;

	was = (*entry & ATTR_AF) != 0;

	if (clear && was) {
		*entry &= ~ATTR_AF;
		invalidate_if_live(*entry | DESC_PAGE, page);
	}

	return was;
}

bool vm_page_age_is_cheap(void)
{
	u64 mmfr1;

	/* ID_AA64MMFR1_EL1.HAFDBS, bits 3:0. Zero means the processor does not
	 * update the flag at all and every sample costs a fault; anything else
	 * means it does it in hardware.
	 *
	 * Read rather than inferred from the processor's name, for the same
	 * reason checkpoint 3 reads what the chip offers rather than what the
	 * architecture allows: the two are not the same question, and the
	 * second one has been wrong here before. */
	__asm__ volatile("mrs %0, id_aa64mmfr1_el1" : "=r"(mmfr1));

	return (mmfr1 & 0xF) != 0;
}

/* Answers an Access Flag fault by setting the flag.
 *
 * Called from the abort path for fault status codes 0x08 to 0x0B, which are the
 * four levels of "this descriptor is valid and its Access Flag is clear". Before
 * page-age sampling existed nothing in this kernel ever cleared that flag, so
 * this fault could not happen and the abort decoder had no name for it -- an
 * unnamed abort is a panic, so arming the measurement without this would have
 * turned the first sampled page into a dead machine.
 *
 * Returns true if it handled it. The flag is set and the translation
 * invalidated; the faulting instruction is retried and succeeds.
 */
bool vm_fault_access_flag(vaddr_t va)
{
	vaddr_t page = va & ~(vaddr_t)(PAGE_SIZE - 1);
	u64 *entry = leaf_entry(page);

	if (!entry)
		return false;

	/* Already set means this was not an access-flag fault after all, and
	 * answering it would turn a real fault into a silent retry loop. */
	if (*entry & ATTR_AF)
		return false;

	*entry |= ATTR_AF;
	invalidate_if_live(*entry, page);

	access_flag_faults++;
	return true;
}

u64 vm_page_age_faults(void)
{
	return access_flag_faults;
}

/* --- a descriptor that names a swap slot instead of a page ----------------
 *
 * The same idea as x86_64 and the same reasoning: a descriptor whose low two
 * bits are 0b00 is invalid, the processor reads nothing else in it, and every
 * other bit is software's.
 *
 * Bit 2 is the marker, which is inside the field the architecture reserves for
 * software use in an invalid descriptor. The slot goes at bit 12 upward.
 */
#define DESC_SWAPPED   (1ULL << 2)
#define DESC_SLOT_SHIFT 12

bool vm_swap_out(vaddr_t va, u32 slot)
{
	vaddr_t page = va & ~(vaddr_t)(PAGE_SIZE - 1);
	u64 *entry = leaf_entry(page);
	u64 old;

	if (!entry || !slot)
		return false;

	old = *entry;

	if ((old & 3) != DESC_PAGE)
		return false;

	*entry = ((u64)slot << DESC_SLOT_SHIFT) | DESC_SWAPPED;

	/* invalidate_if_live is given the *old* descriptor, because what it
	 * decides from is whether there was a live translation to remove -- and
	 * the new one is deliberately not live. */
	invalidate_if_live(old, page);
	return true;
}

u32 vm_swap_slot(vaddr_t va)
{
	vaddr_t page = va & ~(vaddr_t)(PAGE_SIZE - 1);
	u64 *entry = leaf_entry_any(page);
	u64 value;

	if (!entry)
		return 0;

	value = *entry;

	/* A valid descriptor carrying the marker is one of the impossible
	 * states. Answered as "not swapped", which leaves the page alone. */
	if ((value & 3) != 0)
		return 0;

	if (!(value & DESC_SWAPPED))
		return 0;

	return (u32)(value >> DESC_SLOT_SHIFT);
}
