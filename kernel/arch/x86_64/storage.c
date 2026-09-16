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
 * What is attached here: the USB controller, NVMe, AHCI, and virtio --
 * which is a disk or a network card depending on what its configuration space
 * says it is. Each attach answers for its own device and declines quietly
 * otherwise, so this is a list rather than a decision tree.
 *
 * Missing, and it matters: **legacy IDE**. A machine that boots over BIOS
 * commonly presents its disk that way, and the driver cannot live in `core/`
 * with the other three, because compatibility-mode IDE is at fixed I/O ports
 * and `in` and `out` are x86 instructions. It belongs in this directory, and
 * it is KF-192.
 */
#include "x86_64.h"

#include <recon/kernel/block.h>
#include <recon/kernel/pci.h>
#include <recon/kernel/xhci.h>
#include <recon/kernel/virtio.h>
#include <recon/kernel/net.h>
#include <recon/kernel/console.h>

/* In core/virtio_pci.c. */
bool virtio_pci_probe(const struct pci_device *d, struct virtio_device *out);

void arch_storage_probe(void)
{
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

		if (sdhci_attach(d))
			continue;

		if (ahci_attach(d))
			continue;

		/* Network cards, in a file named for storage.
		 *
		 * That is not a tidy place for them and the name is the least
		 * of it: this loop is the whole of device binding on this
		 * architecture, there is one of it per architecture, and a
		 * driver in portable `core/` cannot be reached without editing
		 * an `arch/` file. NW-004 records it. Adding the line here
		 * rather than inventing a driver registry alongside it is
		 * deliberate -- one more entry on a list is cheap and honest,
		 * and a registry built to hold three drivers would be an
		 * interface designed before anything measured what it needs. */
		if (r8169_attach(d))
			continue;

		if (e1000_attach(d))
			continue;

		if (!virtio_pci_probe(d, &v))
			continue;

		/* A probed virtio device is a disk, a card, or something
		 * else entirely, and which one is a number in its
		 * configuration space. Each attach answers for its own
		 * device id and declines quietly otherwise, so the order
		 * here carries no meaning and adding a third costs a
		 * line. */
		if (virtio_blk_attach(&v))
			continue;

		virtio_net_attach(&v);
	}
}

void arch_storage_print(void)
{
	pci_print_summary();
}
