/* Walking the PCI bus, and placing registers nobody placed.
 *
 * The walk itself is the easy half: read every possible address, and the ones
 * that answer with an all-ones vendor identifier are not there. The half worth
 * reading is what happens when the firmware did not do its job -- or, more
 * precisely, when there was no firmware to do it.
 */
#include <recon/kernel/pci.h>

#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/vm.h>

static struct pci_device devices[PCI_MAX_DEVICES];
static unsigned device_count;

/* Where the next unassigned register block goes, and how much room is left.
 * Only used on a machine whose firmware assigned nothing. */
static u64 window_next, window_end;
static bool window_ready;
static unsigned bars_assigned;

u32 pci_read32(const struct pci_device *d, u8 offset)
{
	return arch_pci_config_read(d->bus, d->slot, d->func, offset);
}

u16 pci_read16(const struct pci_device *d, u8 offset)
{
	u32 v = arch_pci_config_read(d->bus, d->slot, d->func, offset & 0xFC);

	return (u16)(v >> ((offset & 2) * 8));
}

u8 pci_read8(const struct pci_device *d, u8 offset)
{
	u32 v = arch_pci_config_read(d->bus, d->slot, d->func, offset & 0xFC);

	return (u8)(v >> ((offset & 3) * 8));
}

void pci_write32(const struct pci_device *d, u8 offset, u32 value)
{
	arch_pci_config_write(d->bus, d->slot, d->func, offset, value);
}

void pci_write16(const struct pci_device *d, u8 offset, u16 value)
{
	u8 aligned = offset & 0xFC;
	unsigned shift = (offset & 2) * 8;
	u32 v = arch_pci_config_read(d->bus, d->slot, d->func, aligned);

	v &= ~(0xFFFFu << shift);
	v |= (u32)value << shift;
	arch_pci_config_write(d->bus, d->slot, d->func, aligned, v);
}

u8 pci_find_capability(const struct pci_device *d, u8 id, u8 from)
{
	u8 pos;
	unsigned guard = 0;

	if (!(pci_read16(d, PCI_STATUS) & PCI_STATUS_CAP_LIST))
		return 0;

	pos = from ? pci_read8(d, from + 1) : (pci_read8(d, PCI_CAPABILITIES) & 0xFC);

	/* A bounded walk. The list is a chain of offsets in device-controlled
	 * memory, and a device whose chain points at itself would otherwise
	 * hang the kernel during enumeration -- which is a real failure mode on
	 * hardware that is dying rather than absent. */
	while (pos >= 0x40 && guard++ < 48) {
		if (pci_read8(d, pos) == id)
			return pos;
		pos = pci_read8(d, pos + 1) & 0xFC;
	}

	return 0;
}

/* Sizing a base address register.
 *
 * There is no register that says how much space a device wants. The way to find
 * out is to write all ones and read back: the device holds the bits it does not
 * decode at zero, so the lowest bit still set is the size. It is a destructive
 * read, which is why the original value is put back afterwards -- and why this
 * must not be done to a device that is already running.
 */
static u64 size_bar(struct pci_device *d, unsigned i, u32 original, bool is_64)
{
	u8 off = (u8)(PCI_BAR0 + i * 4);
	u32 lo, hi = 0xFFFFFFFFu;
	u32 original_hi = 0;
	u64 mask;

	if (is_64)
		original_hi = pci_read32(d, (u8)(off + 4));

	pci_write32(d, off, 0xFFFFFFFFu);
	lo = pci_read32(d, off);

	if (is_64) {
		pci_write32(d, (u8)(off + 4), 0xFFFFFFFFu);
		hi = pci_read32(d, (u8)(off + 4));
	}

	/* Put BOTH halves back.
	 *
	 * The first version restored only the low one, and the consequence was
	 * not a wrong size -- the size came out right. It was that the device's
	 * base address was then read back as 0xFFFFFFFF_FE000000, a physical
	 * address with the top thirty-two bits set, which the direct map turned
	 * into a non-canonical pointer and the processor turned into a general
	 * protection fault three functions later.
	 *
	 * The same discipline the block self-test already had -- put back what
	 * you borrowed -- written there deliberately and forgotten here, on the
	 * only register in this file where borrowing is destructive. */
	pci_write32(d, off, original);
	if (is_64)
		pci_write32(d, (u8)(off + 4), original_hi);

	if (!is_64)
		hi = 0xFFFFFFFFu;

	mask = ((u64)hi << 32) | lo;

	/* The low bits are flags, not address. Four for memory, two for I/O. */
	mask &= (lo & 1) ? ~0x3ULL : ~0xFULL;

	if (mask == 0)
		return 0;

	/* The size is one more than the inverted mask, which is the standard
	 * trick and is worth stating: the device zeroes every bit below the
	 * region's size, so inverting gives size-1. */
	return (~mask) + 1;
}

/* Reads the six base address registers, sizes each one, and places any that the
 * firmware left empty. */
static void read_bars(struct pci_device *d)
{
	for (unsigned i = 0; i < 6; i++) {
		u8 off = (u8)(PCI_BAR0 + i * 4);
		u32 original = pci_read32(d, off);
		bool is_io = (original & 1) != 0;
		bool is_64 = !is_io && ((original >> 1) & 3) == 2;
		u64 size, addr;

		size = size_bar(d, i, original, is_64);
		if (!size)
			continue;	/* the register is not implemented */

		if (is_io) {
			d->bar[i]       = original & ~0x3u;
			d->bar_size[i]  = size;
			d->bar_is_io[i] = true;
			continue;
		}

		addr = original & ~0xFULL;
		if (is_64)
			addr |= (u64)pci_read32(d, (u8)(off + 4)) << 32;

		/* Zero means nobody placed it, which means there was no
		 * firmware. Place it now, aligned to its own size -- which the
		 * specification requires and which is also the only alignment
		 * the device's own decoder can express. */
		if (addr == 0 && window_ready) {
			u64 aligned = (window_next + size - 1) & ~(size - 1);

			if (aligned + size <= window_end) {
				pci_write32(d, off, (u32)aligned);
				if (is_64)
					pci_write32(d, (u8)(off + 4),
						    (u32)(aligned >> 32));
				addr = aligned;
				window_next = aligned + size;
				bars_assigned++;
			}
		}

		d->bar[i]       = addr;
		d->bar_size[i]  = size;
		d->bar_is_io[i] = false;

		if (is_64)
			i++;	/* the upper half is not a register of its own */
	}
}

static void examine(u8 bus, u8 slot, u8 func)
{
	u32 id = arch_pci_config_read(bus, slot, func, PCI_VENDOR_ID);
	struct pci_device *d;
	u16 command;

	/* All ones is what a bus returns when nothing answers. Zero is what
	 * some emulated buses return, and no real vendor identifier is
	 * either. */
	if ((id & 0xFFFF) == 0xFFFF || (id & 0xFFFF) == 0)
		return;

	if (device_count >= PCI_MAX_DEVICES)
		return;

	d = &devices[device_count];
	kmemset(d, 0, sizeof(*d));

	d->bus = bus;
	d->slot = slot;
	d->func = func;
	d->vendor = (u16)(id & 0xFFFF);
	d->device = (u16)(id >> 16);

	d->revision   = pci_read8(d, PCI_REVISION);
	d->prog_if    = pci_read8(d, PCI_PROG_IF);
	d->subclass   = pci_read8(d, PCI_SUBCLASS);
	d->class_code = pci_read8(d, PCI_CLASS);

	d->subsystem_vendor = pci_read16(d, PCI_SUBSYSTEM_VENDOR);
	d->subsystem_id     = pci_read16(d, PCI_SUBSYSTEM_ID);

	/* Only ordinary devices. A header type of 1 is a bridge and a 2 is a
	 * CardBus socket, and both have a different register layout in which
	 * the base address registers are somewhere else entirely -- reading
	 * them as if they were a device produces plausible nonsense. */
	if ((pci_read8(d, PCI_HEADER_TYPE) & 0x7F) != 0)
		return;

	/* Decoding off while the registers are being sized. A device that is
	 * decoding while all-ones is written to its base address register is a
	 * device that briefly claims most of the address space. */
	command = pci_read16(d, PCI_COMMAND);
	pci_write16(d, PCI_COMMAND, (u16)(command & ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY)));

	read_bars(d);

	/* And back on, plus bus mastering: a virtio or NVMe device reads its
	 * own descriptors out of main memory, and without this bit every one of
	 * those reads is silently dropped. The queue is set up, the doorbell is
	 * rung, and nothing ever happens. */
	pci_write16(d, PCI_COMMAND,
		    (u16)(command | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER));

	device_count++;
}

void pci_scan(void)
{
	u64 base, size;

	device_count = 0;
	bars_assigned = 0;
	window_ready = false;

	if (!arch_pci_available())
		return;

	if (arch_pci_mmio_window(&base, &size)) {
		window_next  = base;
		window_end   = base + size;
		window_ready = true;
	}

	/* Bus zero only, for now. A machine with more buses has bridges, and a
	 * bridge has to be configured before anything behind it answers --
	 * which is real work and is not needed by anything this kernel has met.
	 * Stated rather than silently assumed, because "no disk found" on a
	 * machine whose disk is behind a bridge is otherwise a mystery. */
	for (u8 slot = 0; slot < 32; slot++) {
		u32 id = arch_pci_config_read(0, slot, 0, PCI_VENDOR_ID);
		u8 header;

		if ((id & 0xFFFF) == 0xFFFF)
			continue;

		examine(0, slot, 0);

		/* Bit 7 of the header type says the device has more than one
		 * function. Without checking it, a scan of all eight functions
		 * on a single-function device reads the *same* function eight
		 * times -- because such a device ignores the function number --
		 * and enumerates one disk as eight. */
		header = (u8)(arch_pci_config_read(0, slot, 0, PCI_HEADER_TYPE) >> 16);
		if (!(header & 0x80))
			continue;

		for (u8 func = 1; func < 8; func++)
			examine(0, slot, func);
	}
}

unsigned pci_device_count(void)
{
	return device_count;
}

struct pci_device *pci_device_at(unsigned index)
{
	if (index >= device_count)
		return 0;
	return &devices[index];
}

/* Class codes worth naming. Not a complete list and not meant to be -- these
 * are the ones that will be looked for. */
static const char *class_name(u8 class_code, u8 subclass)
{
	switch (class_code) {
	case 0x01:
		switch (subclass) {
		case 0x01: return "IDE controller";
		case 0x06: return "SATA controller";
		case 0x08: return "NVMe controller";
		default:   return "storage controller";
		}
	case 0x02: return "network controller";
	case 0x03: return "display controller";
	case 0x06: return "bridge";
	case 0x0C: return "serial bus controller";
	default:   return "device";
	}
}

void pci_print_summary(void)
{
	if (!arch_pci_available())
		return;

	kprintf("  pci          : %u device%s on bus 0", device_count,
		device_count == 1 ? "" : "s");
	if (bars_assigned)
		kprintf(", %u register block%s placed by us", bars_assigned,
			bars_assigned == 1 ? "" : "s");
	kputs("\n");

	for (unsigned i = 0; i < device_count; i++) {
		const struct pci_device *d = &devices[i];

		kprintf("    %u:%u.%u  %x:%x  %s\n", d->bus, d->slot, d->func,
			d->vendor, d->device, class_name(d->class_code, d->subclass));
	}
}
