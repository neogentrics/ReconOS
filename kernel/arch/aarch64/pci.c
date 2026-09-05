/* PCI configuration space on aarch64: a window, not a pair of ports.
 *
 * This architecture has no I/O ports, so configuration space is *memory*: a
 * window in which every function's 256 bytes appear at a computed offset. The
 * mechanism is simpler than x86's -- no address register to write first, no
 * chance of two processors interleaving an address and a data cycle -- and the
 * hard part moves to finding where the window is.
 *
 * Two firmwares, two answers. A machine booted through UEFI publishes the
 * address in an ACPI table called MCFG. A machine booted with a device tree
 * puts it in the host bridge node's `reg`. Both are read here, ACPI first,
 * because the UEFI path is the one that has nothing else.
 *
 * The address arithmetic is fixed by the specification and is the whole of it:
 *
 *     window + (bus << 20) + (device << 15) + (function << 12) + offset
 *
 * Twelve bits per function, three bits of function, five of device, eight of
 * bus -- which is why a full window is 256 megabytes and why only the buses
 * actually used are mapped.
 */
#include "aarch64.h"

#include <recon/kernel/pci.h>
#include <recon/kernel/acpi.h>
#include <recon/kernel/boot.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/console.h>

static volatile u8 *ecam;
static u8 first_bus, last_bus;
static bool ready;

/* Filled by the device tree walk below, because a callback cannot return. */
static u64 dt_ecam_base, dt_ecam_size;

static void note_host_bridge(u64 base, u64 size)
{
	/* The first `reg` entry of a PCI host bridge node is its configuration
	 * window. Later entries describe other things; taking the first is
	 * correct and taking the largest would not be. */
	if (!dt_ecam_base) {
		dt_ecam_base = base;
		dt_ecam_size = size;
	}
}

/* Called once, before the first configuration access. */
static void find_window(void)
{
	u64 base = 0, size = 0;
	paddr_t page;
	vaddr_t va;

	if (ready)
		return;

	{
		u64 acpi_base;
		u8 lo, hi;

		if (acpi_pci_ecam(&acpi_base, &lo, &hi)) {
			base = acpi_base;
			first_bus = lo;
			last_bus  = hi;
		}
	}

	if (!base && boot_info()->dtb) {
		fdt_each_compatible(boot_info()->dtb, "pci-host-ecam-generic",
				    note_host_bridge);
		base = dt_ecam_base;
		size = dt_ecam_size;
		first_bus = 0;
		last_bus  = size ? (u8)((size >> 20) - 1) : 0;
	}

	if (!base)
		return;

	/* One bus is a megabyte, and one bus is all that is walked. Mapping the
	 * whole window would be 256MB of page tables to describe a region whose
	 * far end has never answered anything. */
	size = 1024 * 1024;

	page = PAGE_ALIGN_DOWN((paddr_t)base);
	va   = (vaddr_t)(uintptr_t)phys_to_virt(page);

	if (!vm_lookup(va) &&
	    !vm_map(va, page, PAGE_ALIGN_UP(size),
		    VM_READ | VM_WRITE | VM_DEVICE | VM_GLOBAL)) {
		kputs("pci: could not map the configuration window\n");
		return;
	}

	ecam = phys_to_virt((paddr_t)base);
	ready = true;
}

bool arch_pci_available(void)
{
	find_window();
	return ready;
}

static volatile u32 *slot_at(u8 bus, u8 slot, u8 func, u8 offset)
{
	u64 off = ((u64)(bus - first_bus) << 20)
		| ((u64)slot << 15)
		| ((u64)func << 12)
		| (offset & 0xFCu);

	return (volatile u32 *)(ecam + off);
}

u32 arch_pci_config_read(u8 bus, u8 slot, u8 func, u8 offset)
{
	if (!ready || bus < first_bus || bus > last_bus)
		return 0xFFFFFFFFu;	/* which is what "nothing there" looks like */

	return *slot_at(bus, slot, func, offset);
}

void arch_pci_config_write(u8 bus, u8 slot, u8 func, u8 offset, u32 value)
{
	if (!ready || bus < first_bus || bus > last_bus)
		return;

	*slot_at(bus, slot, func, offset) = value;
}

/* Where to put registers nobody placed.
 *
 * This function said "nowhere" and gave a reason, and the reason was wrong.
 * It argued that every machine reached through this file was booted by
 * something that assigns base address registers -- true of UEFI, false of a
 * hypervisor starting the kernel directly with a device tree, which is one of
 * this kernel's boot paths and has no firmware at all. An NVMe controller on
 * that path came up with every register unassigned, and the kernel could not
 * reach a disk that was plainly there.
 *
 * The answer is in the device tree. Unlike x86_64 there is no architectural
 * hole in the address space to fall back on -- an ARM machine may put RAM
 * anywhere -- so guessing is not available, and the host bridge node says which
 * window it forwards. That is a better answer than the guess: it is what the
 * machine says about itself rather than what its architecture usually does.
 */
bool arch_pci_mmio_window(u64 *base, u64 *size)
{
	if (!boot_info()->dtb)
		return false;

	return fdt_pci_window(boot_info()->dtb, base, size);
}
