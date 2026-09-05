/* The kernel heap: allocation finer than a page.
 *
 * The physical allocator hands out 4KB at a time, which is the right unit for
 * page tables and the wrong one for a forty-byte structure. This sits on top of
 * it and hands out bytes.
 *
 * --- The design, and why ---
 *
 * Small allocations come from *slabs*: one page, carved into objects of a
 * single size, with a header at the front of the page and a free list threaded
 * through the free objects themselves. Two consequences worth having:
 *
 *   NO PER-OBJECT HEADER. A sixteen-byte allocation costs sixteen bytes. The
 *   usual alternative -- a size word before every object -- would cost fifty
 *   percent on the smallest class, and this project's whole argument is about
 *   not spending memory it does not have to.
 *
 *   FREEING NEEDS NO SEARCH. The slab a pointer belongs to is the page it sits
 *   in, so kfree() finds it by masking off the low twelve bits.
 *
 * Large allocations -- bigger than the largest size class -- come straight from
 * the page allocator, and are page-aligned as a result. That is what tells the
 * two apart at kfree() time, unambiguously and without reading anything that
 * might not be a header: a slab object can never be page-aligned, because the
 * slab header occupies the start of the page.
 *
 * --- What is deliberately not here ---
 *
 * No realloc, because nothing wants one yet and a realloc that has to be
 * written before its first caller gets the semantics wrong. No locking: there
 * is one CPU running kernel code until checkpoint 9, and a lock invented before
 * the concurrency it guards is a lock in the wrong place.
 */
#ifndef RECON_KERNEL_HEAP_H
#define RECON_KERNEL_HEAP_H

#include <recon/kernel/types.h>

/* Every allocation is at least this aligned, which is enough for any type the
 * kernel has and for the atomics it will eventually want. */
#define HEAP_ALIGN 16

void heap_init(void);

/* Returns 0 when there is no memory. Never returns 0 for a zero-byte request
 * by accident: a zero-byte request returns 0 deliberately, because there is no
 * sensible thing to hand back and silently allocating one byte hides the bug in
 * whatever computed the size. */
void *kmalloc(size_t size);

/* The same, zeroed. Separate rather than a flag, because a caller that forgets
 * to zero gets uninitialised memory and a caller that forgets a flag gets a
 * compile error. */
void *kzalloc(size_t size);

void kfree(void *ptr);

void heap_print_summary(void);
bool heap_self_test(void);

#endif /* RECON_KERNEL_HEAP_H */
