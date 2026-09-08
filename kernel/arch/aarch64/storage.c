/* Finding storage on a machine described by a device tree.
 *
 * There is no bus to enumerate here. The firmware left a blob saying where
 * every device's registers are, and the kernel's job is to read it, map what it
 * finds, and ask each address what it is. That is a shorter road than PCI by a
 * long way, which is why aarch64 is where this kernel's first disk driver was
 * made to work.
 *
 * QEMU's `virt` machine lays out thirty-two virtio slots and fills them from
 * the *last* one backwards, so most of what this finds is empty. An empty slot
 * is not an error and is not reported; a slot with something in it that is not
 * a disk is not an error either.
 */
#include "aarch64.h"

#include <recon/kernel/block.h>
#include <recon/kernel/virtio.h>
#include <recon/kernel/pci.h>
#include <recon/kernel/xhci.h>

/* In core/virtio_pci.c. */
bool virtio_pci_probe(const struct pci_device *d, struct virtio_device *out);
#include <recon/kernel/boot.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/console.h>

static unsigned slots_seen, devices_found;

/* Called once per virtio-mmio node in the tree. */
static void probe_slot(u64 base, u64 size)
{
	struct virtio_device v;
	void *regs;

	slots_seen++;

	if (!size)
		return;

	/* Mapped by *page*, not by slot. A virtio-mmio slot is 512 bytes and
	 * they are laid out end to end, so eight of them share one page --
	 * asking for a mapping at the slot's own address is an unaligned
	 * request, which is a panic, and it took one to notice.
	 *
	 * So the page containing the slot is mapped, and the registers are
	 * found at their offset within it. The page is skipped when it is
	 * already there, which it will be for seven slots out of eight.
	 *
	 * Device memory, not normal memory. A register block read as cacheable
	 * would let the processor merge writes and speculate reads, and a
	 * doorbell written twice or not at all is a device that does nothing
	 * while looking perfectly configured.
	 *
	 * In the direct map rather than at its physical address, because since
	 * checkpoint 10 the low half of the address space belongs to user
	 * programs and there is nothing down there to map it into. */
	{
		paddr_t page = PAGE_ALIGN_DOWN((paddr_t)base);
		u64 span = PAGE_ALIGN_UP((base - page) + size);
		vaddr_t va = (vaddr_t)(uintptr_t)phys_to_virt(page);

		if (!vm_lookup(va) &&
		    !vm_map(va, page, span,
			    VM_READ | VM_WRITE | VM_DEVICE | VM_GLOBAL)) {
			kprintf("storage: could not map a virtio slot at %p\n",
				(void *)(uintptr_t)base);
			return;
		}
	}

	regs = phys_to_virt((paddr_t)base);

	if (!virtio_mmio_probe(regs, &v))
		return;

	if (virtio_blk_attach(&v))
		devices_found++;
}

void arch_storage_probe(void)
{
	const struct boot_info *info = boot_info();

	slots_seen = devices_found = 0;

	if (info->dtb)
		fdt_each_compatible(info->dtb, "virtio,mmio", probe_slot);

	/* And the bus, which is where the devices are when the firmware
	 * described this machine with ACPI instead of a tree. Both are walked
	 * rather than one or the other: a machine can have devices on each, and
	 * an empty walk costs nothing. */
	pci_scan();

	for (unsigned i = 0; i < pci_device_count(); i++) {
		const struct pci_device *d = pci_device_at(i);
		struct virtio_device v;

		/* The USB controller first: it is not storage itself, and
		 * claiming it here keeps the storage probes below from
		 * having to know it exists. */
		if (xhci_attach(d))
			continue;

		if (nvme_attach(d))
			continue;

		if (ahci_attach(d))
			continue;

		if (!virtio_pci_probe(d, &v))
			continue;

		if (virtio_blk_attach(&v))
			devices_found++;
	}
}

void arch_storage_print(void)
{
	if (slots_seen)
		kprintf("  looked at    : %u memory-mapped virtio slots\n",
			slots_seen);

	pci_print_summary();
}
