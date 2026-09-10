/* Message-signalled interrupts: a device that raises an interrupt by writing to
 * memory.
 *
 * Everything about interrupts up to here has been wires. A device pulls a line,
 * the line reaches a controller, the controller tells a processor. That works
 * and it does not scale: there are as many lines as the board was built with,
 * several devices share each one, and a shared line means every driver on it is
 * asked "was that yours?" on every interrupt.
 *
 * MSI removes the wire. The device is told an address and a value at start-up,
 * and to raise an interrupt it performs an ordinary memory write of that value
 * to that address. Nothing about it is special except where it is pointed: the
 * range at 0xFEE00000 is claimed by the local APICs, and a write landing there
 * is decoded as "deliver this vector to this processor".
 *
 * Which means the address and the data *are* the routing. There is no table in
 * a chip to program and no line to share: a device with sixteen queues asks for
 * sixteen vectors, each one aimed at whichever processor should handle that
 * queue, and no two of them ever have to ask each other whose interrupt it was.
 *
 * --- What is here, and what is honestly not ---
 *
 * The message encoding, and programming it into a PCI device's MSI capability.
 * That is the mechanism, and it is tested -- see msi_self_test, which composes
 * a message and *performs the write itself*, because the write a device does is
 * an ordinary memory write and a processor can do the same one. If the vector
 * arrives, the encoding is right.
 *
 * What is not here is a driver that uses it. Every storage driver in this
 * kernel polls, which is why none of them has ever needed an interrupt at all.
 * Turning MSI on for a device whose driver does not expect completions to
 * arrive asynchronously would be worse than leaving it off. So this is the
 * half that was missing, ready for the half that comes with the first driver
 * that wants it.
 */
#include "x86_64.h"

#include <recon/kernel/console.h>
#include <recon/kernel/pci.h>
#include <recon/kernel/time.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/arch.h>

/* The local APIC's window. A write here is an interrupt; the destination
 * processor goes in bits 19:12 and the low bits select how it is interpreted --
 * physical destination and no redirection hint, which is the plain "this
 * processor" case. */
#define MSI_ADDRESS_BASE	0xFEE00000ull
#define MSI_REDIRECT_HINT	(1u << 3)
#define MSI_LOGICAL		(1u << 2)

/* The data word is the same shape as the low half of an interrupt command:
 * vector in the low byte, delivery mode above it, and a trigger-mode pair that
 * for an edge-triggered message is left at zero. */
#define MSI_DELIVERY_FIXED	(0u << 8)

#define PCI_CAP_MSI		0x05
#define PCI_CAP_MSIX		0x11

/* Offsets within an MSI capability, from its first byte. */
#define MSI_CONTROL		0x02
#define MSI_ADDRESS_LOW		0x04
#define MSI_ADDRESS_HIGH	0x08	/* only if the 64-bit bit is set */
#define MSI_DATA_32		0x08
#define MSI_DATA_64		0x0C

#define MSI_CONTROL_ENABLE	(1u << 0)
#define MSI_CONTROL_64BIT	(1u << 7)

void x86_msi_compose(u8 vector, u32 destination, u64 *address, u32 *data)
{
	/* Physical destination: one named processor, not a set. The same choice
	 * the I/O APIC's redirection entries make, and for the same reason --
	 * logical destination addresses a group whose membership nothing in
	 * this kernel maintains yet. */
	*address = MSI_ADDRESS_BASE | ((u64)(destination & 0xFF) << 12);
	*data = (u32)vector | MSI_DELIVERY_FIXED;
}

/* Points one PCI device's MSI capability at a vector on a processor.
 *
 * Returns false if the device has no MSI capability, which is not an error: it
 * means that device raises interrupts the old way and its driver should expect
 * them there.
 */
bool x86_msi_enable(const struct pci_device *d, u8 vector, u32 destination)
{
	u8 cap = pci_find_capability(d, PCI_CAP_MSI, 0);
	u64 address;
	u32 data;
	u16 control;

	if (!cap)
		return false;

	x86_msi_compose(vector, destination, &address, &data);

	control = pci_read16(d, (u8)(cap + MSI_CONTROL));

	/* Disabled while it is reprogrammed. A device that raises an interrupt
	 * between the address being written and the data being written sends a
	 * message built half from the old configuration and half from the new
	 * one -- which is a vector nobody has a handler for. */
	pci_write16(d, (u8)(cap + MSI_CONTROL),
		    (u16)(control & ~MSI_CONTROL_ENABLE));

	pci_write32(d, (u8)(cap + MSI_ADDRESS_LOW), (u32)address);

	if (control & MSI_CONTROL_64BIT) {
		pci_write32(d, (u8)(cap + MSI_ADDRESS_HIGH),
			    (u32)(address >> 32));
		pci_write16(d, (u8)(cap + MSI_DATA_64), (u16)data);
	} else {
		/* A device that can only be given 32 bits of address. The
		 * APIC's window is below four gigabytes, so there is nothing
		 * to refuse here -- but the two layouts put the data word in
		 * different places, and using the wrong one writes the vector
		 * into the top half of the address. */
		pci_write16(d, (u8)(cap + MSI_DATA_32), (u16)data);
	}

	/* The number of vectors the device may use is left at one. Asking for
	 * more is how a multi-queue device spreads itself across processors,
	 * and it requires an allocator for consecutive vectors that this kernel
	 * has no caller for yet. */
	control = (u16)((control & ~0x0070) | MSI_CONTROL_ENABLE);
	pci_write16(d, (u8)(cap + MSI_CONTROL), control);

	return true;
}

unsigned x86_msi_capable_devices(bool *any_msix)
{
	unsigned i, n = 0;

	if (any_msix)
		*any_msix = false;

	for (i = 0; i < pci_device_count(); i++) {
		struct pci_device *d = pci_device_at(i);

		if (!d)
			continue;

		if (pci_find_capability(d, PCI_CAP_MSI, 0))
			n++;

		if (any_msix && pci_find_capability(d, PCI_CAP_MSIX, 0))
			*any_msix = true;
	}

	return n;
}

void x86_msi_print_summary(void)
{
	bool msix = false;
	unsigned n = x86_msi_capable_devices(&msix);

	kprintf("  MSI          : %u device(s) can signal by memory write%s; "
		"no driver asks for it yet\n", n,
		msix ? ", and at least one has MSI-X" : "");
}

/* --- the self-test --------------------------------------------------------
 *
 * The whole of an MSI is a memory write, so the test performs one.
 *
 * That is not a simulation of what a device does; it is the same operation. The
 * local APIC decodes a write to its window without caring which agent on the
 * bus made it, so a message composed here and written here is delivered exactly
 * as a device's would be. If the vector arrives, then the address encoding, the
 * destination field and the data word are all correct -- which is every part of
 * this file that could be wrong without a device attached.
 *
 * What it does not test is a device: whether a particular card's capability
 * structure was programmed correctly, and whether its driver copes with an
 * interrupt arriving. That waits for the first driver that wants one, and this
 * says so rather than implying otherwise by passing.
 */
volatile unsigned x86_msi_test_arrivals;

bool x86_msi_self_test(void)
{
	u64 address;
	u32 data;
	u64 deadline;
	unsigned before;

	if (!x86_apic_present()) {
		/* Nothing decodes the window. Not a failure of this code. */
		return true;
	}

	x86_msi_compose(VECTOR_MSI, x86_apic_id(), &address, &data);

	/* The address the encoding produced, checked before it is used, because
	 * a wrong one is a write into whatever happens to be mapped there. The
	 * destination is this processor, so bits 19:12 must hold its
	 * identifier and nothing else may have moved. */
	if ((address & ~0x000FF000ull) != MSI_ADDRESS_BASE ||
	    ((address >> 12) & 0xFF) != (x86_apic_id() & 0xFF)) {
		kprintf("  msi: the message address came out as 0x%lx\n",
			address);
		return false;
	}

	if (data != VECTOR_MSI) {
		kprintf("  msi: the message data came out as 0x%x rather than "
			"the vector\n", data);
		return false;
	}

	before = x86_msi_test_arrivals;

	/* Written through the direct map, as a single 32-bit store. The APIC
	 * window is device memory and a partial or merged write is not a
	 * message. */
	*(volatile u32 *)phys_to_virt((paddr_t)address) = data;

	deadline = time_monotonic_ns() + 500000000ULL;

	while (x86_msi_test_arrivals == before &&
	       time_monotonic_ns() < deadline)
		arch_cpu_relax();

	if (x86_msi_test_arrivals == before) {
		kputs("  msi: a message was written to the local APIC's window "
		      "and no interrupt arrived\n");
		return false;
	}

	return true;
}
