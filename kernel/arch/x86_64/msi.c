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
 * What was not here, until 11 September, was a driver that uses it -- and the
 * driver that arrived does not use *this*. Every device this kernel enumerates
 * offers MSI-X and not MSI, so `x86_msi_enable` below still has no caller, and
 * the code a driver actually reaches is `x86_msix_enable` further down.
 *
 * That is worth saying plainly rather than quietly deleting. This file was
 * written against the wrong capability, was tested, passed, and would have gone
 * on passing forever: a driver wired to the call above would have compiled,
 * run, returned false and changed nothing. It is kept because the encoding it
 * composes is shared with MSI-X and its self-test is what proves that encoding
 * is right -- but as a *path a device takes* it is unexercised, and an
 * unexercised path is one that does not work.
 */
#include "x86_64.h"

#include <recon/kernel/console.h>
#include <recon/kernel/pci.h>
#include <recon/kernel/irq.h>
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

/* --- MSI-X, which is the one devices actually offer ------------------------
 *
 * MSI puts the address and the data in configuration space, which is why it
 * allows one message per device and at most 32 vectors that have to be
 * consecutive. MSI-X puts them in a *table in the device's memory*, which
 * removes both limits: up to 2048 messages, each with its own address and its
 * own vector, so a device with eight queues can aim eight different interrupts
 * at eight different processors.
 *
 * It is also, empirically, what is actually there. The virtio disk this kernel
 * boots from publishes capability 0x11 and not 0x05 -- so a driver wired to
 * `x86_msi_enable` above would have compiled, run, returned false and changed
 * nothing. That is the shape of bug this project keeps finding: not a wrong
 * answer, an answer to a question nothing asked.
 *
 * Each entry is sixteen bytes: address, address-high, data, and a control word
 * whose bit 0 masks it.
 */
#define MSIX_CONTROL		0x02
#define MSIX_TABLE		0x04

#define MSIX_CONTROL_SIZE	0x07FFu	/* entries, minus one */
#define MSIX_CONTROL_MASK	(1u << 14)	/* mask every entry at once */
#define MSIX_CONTROL_ENABLE	(1u << 15)

#define MSIX_ENTRY_BYTES	16
#define MSIX_ENTRY_ADDR_LOW	0x00
#define MSIX_ENTRY_ADDR_HIGH	0x04
#define MSIX_ENTRY_DATA		0x08
#define MSIX_ENTRY_CONTROL	0x0C
#define MSIX_ENTRY_MASKED	(1u << 0)

static void table_write(volatile u8 *t, u32 off, u32 v)
{
	*(volatile u32 *)(t + off) = v;
}

/* Points one entry of a device's MSI-X table at a vector on a processor.
 *
 * False when the device has no MSI-X capability, when it has fewer entries than
 * the one asked for, or when the table's own memory could not be reached. None
 * of those is an error here: each means this device interrupts some other way
 * and its driver should expect it there.
 */
bool x86_msix_enable(const struct pci_device *d, unsigned entry, u8 vector,
		     u32 destination)
{
	u8 cap = pci_find_capability(d, PCI_CAP_MSIX, 0);
	volatile u8 *slot;
	u64 address;
	u32 data, where;
	u16 control;

	if (!cap)
		return false;

	control = pci_read16(d, (u8)(cap + MSIX_CONTROL));

	/* The field holds the count minus one, so a device with a single
	 * message reports zero. Reading it as the count is an off-by-one that
	 * writes one entry past the end of the table. */
	if (entry > (unsigned)(control & MSIX_CONTROL_SIZE))
		return false;

	/* Where the table is: the low three bits name a base address register
	 * and the rest is a byte offset into it. Masking the wrong way round
	 * gives an offset three bits too large, in a register that is usually
	 * the right one anyway -- so it would work on a device whose table
	 * starts at zero and corrupt one whose table does not. */
	where = pci_read32(d, (u8)(cap + MSIX_TABLE));

	slot = pci_map_bar(d, (u8)(where & 0x7), (where & ~0x7u)
			   + (u32)entry * MSIX_ENTRY_BYTES, MSIX_ENTRY_BYTES);

	if (!slot)
		return false;

	x86_msi_compose(vector, destination, &address, &data);

	/* Masked while it is written, unmasked last.
	 *
	 * The same hazard `x86_msi_enable` disables the device for, and the
	 * reason it is per-entry here: an interrupt raised between the address
	 * being written and the data being written carries half of each
	 * configuration, which is a vector nothing has a handler for. The
	 * unmask is a separate store *after* the other three precisely so that
	 * there is no instant at which a live entry is half-written. */
	table_write(slot, MSIX_ENTRY_CONTROL, MSIX_ENTRY_MASKED);
	table_write(slot, MSIX_ENTRY_ADDR_LOW, (u32)address);
	table_write(slot, MSIX_ENTRY_ADDR_HIGH, (u32)(address >> 32));
	table_write(slot, MSIX_ENTRY_DATA, data);
	table_write(slot, MSIX_ENTRY_CONTROL, 0);

	/* And the legacy wire is switched off. A device using MSI-X must not
	 * also assert INTx, and one that does is an interrupt arriving on a
	 * shared line whose handler list does not include this driver.
	 *
	 * Done here as well as wherever the driver did it, because this is the
	 * function that makes the statement true: after this returns the device
	 * signals by message, and a caller that had left the line alone would
	 * be asserting both. */
	pci_write16(d, PCI_COMMAND,
		    (u16)(pci_read16(d, PCI_COMMAND) | PCI_COMMAND_INTX_DISABLE));

	/* Function mask cleared in the same write that enables: leaving it set
	 * is a device that is configured, enabled, and silent. */
	control = (u16)((control | MSIX_CONTROL_ENABLE) & ~MSIX_CONTROL_MASK);
	pci_write16(d, (u8)(cap + MSIX_CONTROL), control);

	return true;
}

/* --- what a driver actually calls -----------------------------------------
 *
 * Arch-neutral, because a driver that asks for an interrupt should not have to
 * know which machine it is on: `virtio_blk_attach` is the same file on both
 * architectures and one of them has no PCI at all.
 *
 * The vector is claimed here rather than by the caller, because the caller has
 * nothing useful to do with the number. What it wants is "my handler runs when
 * this device speaks", and the number is an implementation detail of how that
 * is arranged on this machine.
 */
bool arch_pci_request_interrupt(const struct pci_device *d, unsigned entry,
				void (*fn)(void *), void *arg, const char *name)
{
	u8 vector;

	if (!d || !fn)
		return false;

	/* No local APIC is nothing to decode the write, so the message would
	 * land in memory and stay there. */
	if (!x86_apic_present())
		return false;

	/* Plain MSI is deliberately not tried.
	 *
	 * It is implemented above and it is tested, but no device this kernel
	 * has ever enumerated offers MSI without also offering MSI-X -- so a
	 * fallback to it here would be a branch that never runs, and a branch
	 * that never runs is one that does not work. It goes in beside the
	 * first device that needs it. */
	if (!pci_find_capability(d, PCI_CAP_MSIX, 0))
		return false;

	vector = irq_claim_vector(fn, arg, name);

	if (!vector)
		return false;

	if (!x86_msix_enable(d, entry, vector, x86_apic_id())) {
		/* Given back rather than leaked. Sixteen is not many, and a
		 * driver that probes several devices and fails on each would
		 * otherwise exhaust them before reaching the one that works. */
		irq_release_vector(vector);
		return false;
	}

	return true;
}

/* How many devices can raise an interrupt without a wire, and how many of them
 * do it the way that is actually offered.
 *
 * This used to count capability 0x05 alone and report the total as "devices
 * that can signal by memory write" -- which left out every device with MSI-X
 * and no MSI, and on this machine that is both disks: the only two devices that
 * actually do it. The sentence was broader than the count, and the count was
 * the one people believed.
 */
unsigned x86_msi_capable_devices(unsigned *msix_out)
{
	unsigned i, n = 0, msix = 0;

	for (i = 0; i < pci_device_count(); i++) {
		struct pci_device *d = pci_device_at(i);
		bool has_msix;

		if (!d)
			continue;

		has_msix = pci_find_capability(d, PCI_CAP_MSIX, 0) != 0;

		if (has_msix)
			msix++;

		/* Either mechanism counts. A device offering both is one
		 * device, not two -- which is why this is an or and not two
		 * additions. */
		if (has_msix || pci_find_capability(d, PCI_CAP_MSI, 0))
			n++;
	}

	if (msix_out)
		*msix_out = msix;

	return n;
}

void arch_irq_print_device_summary(void)
{
	unsigned msix = 0;
	unsigned n = x86_msi_capable_devices(&msix);

	kprintf("  signalling   : %u device(s) can raise an interrupt by "
		"writing to memory, %u of them by MSI-X\n", n, msix);

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
