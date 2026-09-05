/* Virtual memory: the kernel's own page tables.
 *
 * Until now the kernel has run on somebody else's map -- the trampoline's
 * identity map on the Multiboot2 and PVH paths, and the firmware's on the UEFI
 * one. Both are borrowed, both map more than they should, and neither can be
 * changed. This is where the kernel stops borrowing.
 *
 * --- What it buys, in the order the project cares about ---
 *
 * LARGE PAGES. Mapping a gigabyte with 4KB pages costs 262,144 page table
 * entries and a translation lookaside buffer that thrashes every time the
 * working set moves. With 1GB pages it costs one entry and never misses. This
 * is the single largest lever on the claim that a 4GB machine should not need
 * 16GB to feel responsive, and checkpoint 3 already asked the CPU whether it
 * has them.
 *
 * A DIRECT MAP. Every byte of physical memory, visible at a fixed offset, so
 * that "give me a page and let me write to it" stops requiring a mapping call.
 * Without it every allocation needs a page table edit, which is slow, and a
 * TLB shootdown, which on more than one CPU is much slower.
 *
 * PAGES THAT CANNOT BE EXECUTED. The firmware's map is executable everywhere.
 * Ours is not: data is not executable and code is not writable, on any CPU
 * that has the bit -- which checkpoint 3 also asked about.
 *
 * --- What is deliberately not here yet ---
 *
 * Address spaces. There is one map, the kernel's. Per-process address spaces
 * arrive with processes, at checkpoint 10, and the interface below is shaped to
 * grow a `struct addrspace *` first argument rather than to be replaced.
 */
#ifndef RECON_KERNEL_VM_H
#define RECON_KERNEL_VM_H

#include <recon/kernel/types.h>
#include <recon/kernel/pmm.h>

/* What a mapping may be used for. Absence is meaningful: a mapping with
 * neither VM_WRITE nor VM_EXEC is read-only data, and that is the default
 * rather than a special case. */
#define VM_READ   (1u << 0)
#define VM_WRITE  (1u << 1)
#define VM_EXEC   (1u << 2)

/* Memory-mapped hardware rather than RAM. On x86 this disables caching; on
 * aarch64 it selects a Device memory type, which also forbids the speculative
 * reads and write merging that a UART would be destroyed by. Getting this
 * wrong on ARM does not produce slow hardware, it produces hardware that
 * behaves randomly. */
#define VM_DEVICE (1u << 3)

/* The mapping is the same in every address space, so the CPU may keep it
 * across an address space switch. Every kernel mapping is global. */
#define VM_GLOBAL (1u << 4)

/* Reachable from user mode.
 *
 * Its *absence* is the load-bearing part. Every kernel mapping omits it, and
 * that omission is what makes the kernel unreachable from a user program --
 * not a check in any code, but a bit the processor consults on every access. */
#define VM_USER   (1u << 5)

/* --- The direct map -----------------------------------------------------
 *
 * All of physical memory at a fixed virtual offset. The offset is per
 * architecture because the address spaces are different shapes, but the
 * property is the same everywhere: physical address p is readable at
 * phys_to_virt(p), always, with no mapping call. */

void   *phys_to_virt(paddr_t phys);
paddr_t virt_to_phys(const void *virt);

/* --- Building and changing the map -------------------------------------- */

/* Builds the kernel's own page tables from the boot memory map and switches to
 * them. Needs the physical allocator, so it runs after pmm_init(). */
void vm_init(void);

/* Turns *this* processor's MMU on, using the tables vm_init() built. Called by
 * the boot processor from vm_init(), and by every secondary on itself: the
 * tables are shared, the registers pointing at them are not. */
void vm_activate_this_cpu(void);

/* Maps `size` bytes. Both addresses must be page-aligned and size a multiple of
 * a page. Uses the largest page the alignment and length allow, which is what
 * makes the direct map cost tens of entries rather than hundreds of thousands.
 * Returns false if it ran out of memory for page tables. */
bool vm_map(vaddr_t va, paddr_t pa, u64 size, unsigned flags);

/* The physical address a virtual one currently resolves to, or 0 for
 * unmapped. Reads the tables rather than the CPU, so it answers for the map
 * being built as well as the one in use. */
paddr_t vm_lookup(vaddr_t va);

void vm_print_summary(void);

/* Allocates, maps and returns a page of the given size, in one call, because
 * every caller that wants memory wants it mapped. Returns 0 on failure. */
bool vm_self_test(void);

#endif /* RECON_KERNEL_VM_H */
