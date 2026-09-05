/* Finding storage on x86_64 -- not yet.
 *
 * Reported rather than pretended, for the same reason arch_smp_start does it on
 * this architecture: a kernel that says it found a disk and did not is worse
 * than one that says it found none.
 *
 * What it needs, so the size of it is on the record rather than in somebody's
 * head:
 *
 *   PCI CONFIGURATION SPACE. Two I/O ports, an address and a data register, and
 *   a scan of bus/device/function looking for vendor identifiers that are not
 *   0xFFFF. Short, and the same scan finds NVMe, AHCI and virtio alike.
 *
 *   BASE ADDRESS REGISTERS THAT NOBODY ASSIGNED. On the two UEFI paths and
 *   under SeaBIOS, firmware has already placed every device's registers
 *   somewhere. On the PVH path there is no firmware at all, so the BARs read
 *   back as zero and the kernel has to size each one and place it itself. That
 *   is the part that is easy to skip and then spend an afternoon on, because
 *   three of the four boot paths work without it.
 *
 *   THE VIRTIO PCI CAPABILITY LIST. A modern virtio device does not put its
 *   registers at a fixed offset; it publishes a chain of capability structures
 *   in configuration space saying which BAR and which offset holds the common
 *   configuration, the notification area and the device-specific space. More
 *   indirection than the memory-mapped transport, and the same registers at the
 *   end of it.
 *
 * None of it is hard, and all of it is needed anyway: NVMe and AHCI are both
 * PCI devices, and this architecture cannot reach a real disk without it. The
 * device tree path on aarch64 needed none of it, which is why the block layer
 * and the virtio driver were proved there first -- with those working, this is
 * a transport rather than a leap.
 */
#include "x86_64.h"

#include <recon/kernel/block.h>
#include <recon/kernel/console.h>

void arch_storage_probe(void)
{
	/* Nothing found, and nothing claimed. */
}

void arch_storage_print(void)
{
	kputs("  looked at    : nothing; this architecture needs PCI first\n");
}
