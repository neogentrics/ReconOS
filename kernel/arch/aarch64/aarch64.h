/* Internal to arch/aarch64. Nothing above arch/ includes this. */
#ifndef RECON_ARCH_AARCH64_H
#define RECON_ARCH_AARCH64_H

#include <recon/kernel/types.h>

/* Where the kernel image is linked, and where boot.S puts the processor before
 * any C runs. The linker script holds the same number; they are two statements
 * of one fact and must not drift.
 *
 * Nothing on this architecture requires this particular address -- aarch64 code
 * is PC-relative and links anywhere. It is x86_64's -mcmodel=kernel that
 * requires the top two gigabytes, and one number in two architectures is easier
 * to hold than two. */
#define KERNEL_VMA 0xFFFFFFFF80000000ULL

/* What to add to a device's physical address to reach it.
 *
 * Zero while the kernel is running on the map boot.S built, which is an
 * identity map; DIRECT_MAP_BASE once vm_init has installed the real one, which
 * is not. It is a variable rather than a constant because both are true, at
 * different times, and the drivers should not have to know which time it is.
 *
 * Set exactly once, immediately after the tables change, and read by every
 * mmio access below. */
extern u64 aarch64_device_offset;

/* Reading and writing a hardware register. Volatile so the compiler does not
 * decide a write nobody reads is dead, and through the offset above so that
 * one assignment moves every driver from the identity map to the direct one. */
static inline void mmio_w32(u64 base, u64 off, u32 v)
{
	*(volatile u32 *)(base + aarch64_device_offset + off) = v;
}

static inline u32 mmio_r32(u64 base, u64 off)
{
	return *(volatile u32 *)(base + aarch64_device_offset + off);
}

/* Saved by boot.S from x0, where the firmware left it. */
extern u64 arch_dtb_pointer;

/* Set by reconboot_entry from x0. Zero when the firmware started us directly. */
extern u64 reconboot_handoff;

/* Where the first PL011 UART is on QEMUs virt machine and on a great many ARM
 * boards, because the address came from the same reference design. Hardcoded
 * until there is a device tree parser that can report why it failed; needed
 * here as well as in arch.c because the console has to be mapped before the
 * kernel switches to its own translation tables. */
#define PL011_BASE 0x09000000UL

/* The rest of the machine's fixed addresses, together in one place because the
 * page tables have to map every one of them before anything touches it.
 *
 * Learned by fault: the GIC was configured before it was mapped, and the kernel
 * took a translation fault writing to the CPU interface. Which is exactly what
 * the fault reporter from checkpoint 7 is for -- it said "data abort,
 * translation fault level 2, on a write to 0x08010004", and 0x08010004 is the
 * priority mask register. Two minutes, no guessing. */
#define GICD_BASE  0x08000000UL		/* interrupt controller, distributor */
#define GICC_BASE  0x08010000UL		/* interrupt controller, CPU interface */
#define PL031_BASE 0x09010000UL		/* real-time clock */

/* In time.c: the interrupt path, and what the console reports about the clock. */
void aarch64_irq(void);

/* The per-processor halves of the interrupt controller and the timer. Called by
 * the boot processor on itself and by each secondary on itself. */
void aarch64_gic_cpu_init(void);
void aarch64_timer_cpu_init(void);

/* This processor's own MPIDR-derived number, as opposed to the constant zero
 * arch_cpu_id() returned before there were other processors. */
unsigned arch_cpu_id_real(void);
void aarch64_time_print_source(void);

/* Reads the memory and the command line out of a flattened device tree.
 * Returns false if the blob is not one. */
bool fdt_parse(u64 dtb_phys);

#endif /* RECON_ARCH_AARCH64_H */
