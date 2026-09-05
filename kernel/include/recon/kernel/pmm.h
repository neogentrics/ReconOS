/* The physical page allocator.
 *
 * The first thing in the kernel that hands out memory, and therefore the first
 * thing anything else can be built on. It owns whole pages of physical memory
 * and nothing finer; a heap that hands out fourteen bytes is a later, separate
 * problem that will be built on top of this one.
 *
 * It is a bitmap, one bit per page, and that is a deliberate choice rather
 * than a placeholder. A free list is faster to allocate from but cannot answer
 * "is this particular page free" without walking, cannot hand out contiguous
 * runs without a second structure, and stores its links *inside* the free
 * pages -- which means a bug that writes through a stale pointer corrupts the
 * allocator itself and is discovered somewhere else entirely. The bitmap costs
 * one page of overhead per 128MB of RAM and every question about it can be
 * answered by looking.
 */
#ifndef RECON_KERNEL_PMM_H
#define RECON_KERNEL_PMM_H

#include <recon/kernel/types.h>

/* Both architectures the kernel currently supports use 4KB pages, and both
 * support larger ones. When the first architecture arrives that cannot, or the
 * first time a larger granule is wanted, this moves into arch.h and becomes a
 * question asked rather than a constant known. */
#define PAGE_SIZE 4096UL

#define PAGE_ALIGN_DOWN(x) ((x) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_UP(x)   PAGE_ALIGN_DOWN((x) + PAGE_SIZE - 1)

/* Reads the memory map that arch_early_init() built and takes ownership of
 * everything in it marked usable. Must be called exactly once. */
void pmm_init(void);

/* One page. Returns 0 when there is none -- and 0 is a safe sentinel because
 * the page at physical zero is never handed out, deliberately, so that a null
 * return and a null dereference cannot be confused for a valid page. */
paddr_t pmm_alloc_page(void);

/* `count` pages, contiguous. Contiguity matters for anything a device will
 * read by physical address, which cannot follow a page table. */
paddr_t pmm_alloc_pages(size_t count);

void pmm_free_page(paddr_t page);

/* Re-reaches the bitmap through the direct map. Called by vm_init() the moment
 * the kernel is running on its own page tables, and never otherwise. */
void pmm_remap(void);
void pmm_free_pages(paddr_t page, size_t count);

size_t pmm_total_pages(void);
size_t pmm_free_page_count(void);

void pmm_print_summary(void);

/* Allocates and frees in a pattern with known answers, and reports whether the
 * allocator agreed. Called at boot: an allocator that is wrong is worth
 * finding out about immediately rather than three checkpoints later, and
 * there is no test harness that can run a kernel yet. */
bool pmm_self_test(void);

#endif /* RECON_KERNEL_PMM_H */
