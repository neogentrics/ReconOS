/* Finding storage on x86_64: walk the bus, and ask everything on it.
 *
 * Unlike a machine described by a device tree, nothing here lists what is
 * present. The bus has to be enumerated -- read every possible address and see
 * which ones answer -- and on the boot path with no firmware at all the kernel
 * also has to decide where each device's registers appear.
 *
 * Both of those live in core/pci.c, because neither is specific to this
 * architecture. What *is* specific is how configuration space is reached, and
 * that is two I/O ports, in pci.c beside this file.
 *
 * Only virtio is attached so far. NVMe and AHCI are what matter on real
 * hardware and are the obvious next drivers; the bus walk that finds them is
 * already here, which was most of the work.
 */
#include "x86_64.h"

#include <recon/kernel/block.h>
#include <recon/kernel/pci.h>
#include <recon/kernel/virtio.h>
#include <recon/kernel/console.h>

/* In core/virtio_pci.c. */
bool virtio_pci_probe(const struct pci_device *d, struct virtio_device *out);

void arch_storage_probe(void)
{
	pci_scan();

	for (unsigned i = 0; i < pci_device_count(); i++) {
		const struct pci_device *d = pci_device_at(i);
		struct virtio_device v;

		if (!virtio_pci_probe(d, &v))
			continue;

		virtio_blk_attach(&v);
	}
}

void arch_storage_print(void)
{
	pci_print_summary();
}
