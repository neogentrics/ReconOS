#include <recon/kernel/pmm.h>
#include <recon/kernel/boot.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/panic.h>

/* One bit per page: set means in use, clear means free. Starting from "all
 * used" and clearing what the firmware called usable is the safe direction --
 * memory nobody told us about stays untouchable, which is the correct
 * treatment of an unknown. */
static u8    *bitmap;
static size_t bitmap_bytes;
static size_t total_pages;
static size_t free_pages;

/* The address the bitmap's first bit describes.
 *
 * Not zero, and this matters. On x86 RAM starts near zero and the distinction
 * is invisible, but on ARM it commonly starts at 1GB, and on server parts at
 * 4GB or higher. A bitmap indexed from physical zero would spend its first
 * quarter-million bits describing addresses that are not memory -- reporting
 * them as "used", which is both wasteful and a lie, since nothing is using
 * them and nothing could. */
static paddr_t base_paddr;

static inline size_t page_index(paddr_t addr)
{
	return (size_t)((addr - base_paddr) / PAGE_SIZE);
}

static inline paddr_t page_addr(size_t index)
{
	return base_paddr + (paddr_t)index * PAGE_SIZE;
}

/* Where the last search stopped. Without it, allocating n pages costs a scan
 * from zero every time and boot gets quadratic in the number of allocations. */
static size_t search_hint;

static inline bool bit_test(size_t page)
{
	return (bitmap[page / 8] >> (page % 8)) & 1u;
}

static inline void bit_set(size_t page)
{
	bitmap[page / 8] |= (u8)(1u << (page % 8));
}

static inline void bit_clear(size_t page)
{
	bitmap[page / 8] &= (u8)~(1u << (page % 8));
}

static void mark_used(size_t page, size_t count)
{
	for (size_t i = 0; i < count && page + i < total_pages; i++)
		if (!bit_test(page + i)) {
			bit_set(page + i);
			free_pages--;
		}
}

static void mark_free(size_t page, size_t count)
{
	for (size_t i = 0; i < count && page + i < total_pages; i++)
		if (bit_test(page + i)) {
			bit_clear(page + i);
			free_pages++;
		}
}

/* The bitmap has to live somewhere, and it has to be found before there is an
 * allocator to find it with. So it is placed by hand, in the first usable
 * region large enough to hold it, and then marked used by the very allocator
 * it makes possible. */
static paddr_t place_bitmap(size_t bytes)
{
	const struct boot_info *info = boot_info();

	for (unsigned i = 0; i < info->region_count; i++) {
		const struct mem_region *r = &info->regions[i];
		paddr_t base;
		u64 end;

		if (r->kind != MEM_USABLE)
			continue;

		base = PAGE_ALIGN_UP(r->base);
		end  = r->base + r->size;

		/* The page at physical zero is never used, so a region starting
		 * there must give up its first page before it can be measured. */
		if (base == 0)
			base = PAGE_SIZE;

		if (end > base && end - base >= bytes)
			return base;
	}

	return 0;
}

void pmm_init(void)
{
	const struct boot_info *info = boot_info();
	u64 lowest = ~0ULL;
	u64 highest = 0;
	paddr_t bitmap_phys;

	/* The bitmap spans the lowest to the highest *usable* address, and
	 * neither bound comes from the full map. Machines commonly report
	 * reserved ranges far above their RAM -- a memory-mapped device at
	 * sixty-something gigabytes would otherwise demand a bitmap of tens of
	 * megabytes to describe memory that will never be allocated. */
	for (unsigned i = 0; i < info->region_count; i++)
		if (info->regions[i].kind == MEM_USABLE) {
			u64 start = info->regions[i].base;
			u64 end = start + info->regions[i].size;

			if (start < lowest)
				lowest = start;
			if (end > highest)
				highest = end;
		}

	if (highest == 0)
		panic("pmm: the memory map contains no usable memory");

	base_paddr   = (paddr_t)PAGE_ALIGN_DOWN(lowest);
	total_pages  = (size_t)((PAGE_ALIGN_UP(highest) - base_paddr) / PAGE_SIZE);
	bitmap_bytes = (total_pages + 7) / 8;

	bitmap_phys = place_bitmap(bitmap_bytes);
	if (!bitmap_phys)
		panic("pmm: no usable region large enough to hold the page bitmap");

	/* Identity, for now. When the kernel moves to the higher half this
	 * becomes a translation, and it is the only place here that has to
	 * change. */
	bitmap = (u8 *)(uintptr_t)bitmap_phys;

	kmemset(bitmap, 0xFF, bitmap_bytes);
	free_pages = 0;

	/* Now give back what the firmware said we may have. Partial pages at
	 * either end are dropped rather than rounded outward: rounding a usable
	 * range up would claim a page that overlaps something else's. */
	for (unsigned i = 0; i < info->region_count; i++) {
		const struct mem_region *r = &info->regions[i];
		u64 start, end;

		if (r->kind != MEM_USABLE)
			continue;

		start = PAGE_ALIGN_UP(r->base);
		end   = PAGE_ALIGN_DOWN(r->base + r->size);

		if (end > start)
			mark_free(page_index(start),
				  (size_t)((end - start) / PAGE_SIZE));
	}

	/* Two things the allocator must never hand out. The bitmap, because it
	 * is standing on it. And the page at physical zero, so that a null
	 * return from pmm_alloc_page() and a null pointer bug cannot be
	 * confused -- which only arises where memory starts at zero at all. */
	mark_used(page_index(bitmap_phys),
		  (size_t)(PAGE_ALIGN_UP(bitmap_bytes) / PAGE_SIZE));
	if (base_paddr == 0)
		mark_used(0, 1);

	search_hint = 0;
}

paddr_t pmm_alloc_pages(size_t count)
{
	size_t run = 0;
	size_t scanned = 0;
	size_t i = search_hint;

	if (count == 0 || count > total_pages)
		return 0;

	/* One pass over every page, starting where the last search finished and
	 * wrapping once. Bounded by construction: a full circuit means there is
	 * no run of this length and the answer is genuinely no. */
	while (scanned < total_pages) {
		if (bit_test(i)) {
			run = 0;
		} else {
			run++;
			if (run == count) {
				size_t start = i + 1 - count;

				mark_used(start, count);
				search_hint = (i + 1) % total_pages;
				return page_addr(start);
			}
		}

		i++;
		scanned++;

		if (i >= total_pages) {
			i = 0;
			/* A run cannot straddle the wrap: page total_pages-1 and
			 * page 0 are not adjacent in physical memory. */
			run = 0;
		}
	}

	return 0;
}

paddr_t pmm_alloc_page(void)
{
	return pmm_alloc_pages(1);
}

void pmm_free_pages(paddr_t page, size_t count)
{
	size_t index;

	if (page == 0 || (page % PAGE_SIZE) != 0)
		panic("pmm: asked to free something that is not a page address");

	/* Below the base is as wrong as beyond the end, and the subtraction in
	 * page_index() would wrap into an enormous index that passes the upper
	 * bound check. Caught here rather than corrupting a distant bit. */
	if (page < base_paddr)
		panic("pmm: asked to free memory below the start of the bitmap");

	index = page_index(page);

	if (index + count > total_pages)
		panic("pmm: asked to free memory past the end of the bitmap");

	mark_free(index, count);
}

void pmm_free_page(paddr_t page)
{
	pmm_free_pages(page, 1);
}

size_t pmm_total_pages(void)     { return total_pages; }
size_t pmm_free_page_count(void) { return free_pages; }

void pmm_print_summary(void)
{
	size_t used = total_pages - free_pages;

	kprintf("\nPhysical memory\n");
	kprintf("  page size    : %lu bytes\n", (u64)PAGE_SIZE);
	kprintf("  pages        : %lu total, %lu free, %lu used\n",
		(u64)total_pages, (u64)free_pages, (u64)used);
	kprintf("  free         : %lu MB\n",
		(u64)(free_pages * PAGE_SIZE) / (1024 * 1024));
	kprintf("  describes    : %p upward, %lu MB\n",
		(void *)(uintptr_t)base_paddr,
		(u64)(total_pages * PAGE_SIZE) / (1024 * 1024));
	kprintf("  bitmap       : %p, %lu bytes\n",
		(void *)bitmap, (u64)bitmap_bytes);
}

bool pmm_self_test(void)
{
	size_t before = free_pages;
	paddr_t a, b, run;
	bool ok = true;

	/* Distinct pages, and both actually free beforehand. */
	a = pmm_alloc_page();
	b = pmm_alloc_page();
	if (!a || !b || a == b) {
		kputs("  pmm: two allocations were not two distinct pages\n");
		ok = false;
	}

	/* A contiguous run really is contiguous, and really is marked used. */
	run = pmm_alloc_pages(4);
	if (!run) {
		kputs("  pmm: could not allocate four contiguous pages\n");
		ok = false;
	} else {
		for (size_t i = 0; i < 4; i++)
			if (!bit_test(page_index(run) + i)) {
				kputs("  pmm: a page inside a fresh run was not marked used\n");
				ok = false;
				break;
			}
	}

	if (free_pages != before - 6) {
		kputs("  pmm: the free count did not fall by the six pages taken\n");
		ok = false;
	}

	if (run)
		pmm_free_pages(run, 4);
	if (b)
		pmm_free_page(b);
	if (a)
		pmm_free_page(a);

	/* The point of the whole test: everything taken came back. A leak here
	 * is one that would otherwise be found as a machine that slows down
	 * over hours. */
	if (free_pages != before) {
		kprintf("  pmm: %lu pages did not come back after being freed\n",
			(u64)(before - free_pages));
		ok = false;
	}

	return ok;
}
