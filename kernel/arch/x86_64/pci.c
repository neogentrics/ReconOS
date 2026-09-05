/* Reaching PCI configuration space on x86_64, and finding somewhere to put
 * registers the firmware did not place.
 *
 * The access mechanism is two I/O ports and predates almost everything else in
 * this kernel: write a bus/device/function/offset into 0xCF8, read or write the
 * data at 0xCFC. It has been the same since 1992, it works before paging, and
 * it needs no discovery of its own -- which is why it is still how a kernel
 * finds out what is in the machine.
 */
#include "x86_64.h"

#include <recon/kernel/pci.h>
#include <recon/kernel/boot.h>
#include <recon/kernel/console.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static inline void outl(u16 port, u32 value)
{
	__asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline u32 inl(u16 port)
{
	u32 value;

	__asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

/* Bit 31 is "enable"; without it the data port reads whatever was there
 * before, which on a quiet bus is the previous device's answer. */
static u32 address_of(u8 bus, u8 slot, u8 func, u8 offset)
{
	return 0x80000000u
	     | ((u32)bus  << 16)
	     | ((u32)slot << 11)
	     | ((u32)func << 8)
	     | (offset & 0xFCu);	/* the low two bits are never part of it */
}

bool arch_pci_available(void)
{
	/* Every x86_64 machine has it. There is a probe -- write an address,
	 * read it back, and see whether bit 31 stuck -- and it exists for
	 * machines from before 1995, which this kernel will not meet. */
	return true;
}

u32 arch_pci_config_read(u8 bus, u8 slot, u8 func, u8 offset)
{
	outl(PCI_CONFIG_ADDRESS, address_of(bus, slot, func, offset));
	return inl(PCI_CONFIG_DATA);
}

void arch_pci_config_write(u8 bus, u8 slot, u8 func, u8 offset, u32 value)
{
	outl(PCI_CONFIG_ADDRESS, address_of(bus, slot, func, offset));
	outl(PCI_CONFIG_DATA, value);
}

/* Where to put registers nobody placed.
 *
 * On this architecture there is a hole in the physical address space between
 * the top of low memory and the fixed hardware at the very top -- the local
 * APIC at 0xFEE00000, the I/O APIC below it, the firmware ROM above. Every PC
 * has it, and every firmware places PCI registers in it.
 *
 * Three gigabytes is where it starts, because that is where this architecture's
 * memory controllers stop putting RAM: a machine with more than 3GB has the
 * remainder mapped *above* 4GB precisely so that this hole stays a hole. Ending
 * at 0xF0000000 leaves the fixed hardware alone with room to spare.
 *
 * Checked against the memory map rather than asserted, because a machine that
 * does report RAM here would otherwise have it quietly overwritten by a device,
 * which is the worst failure in this file by a distance.
 */
bool arch_pci_mmio_window(u64 *base, u64 *size)
{
	const struct boot_info *info = boot_info();
	const u64 start = 0xC0000000ULL;
	const u64 end   = 0xF0000000ULL;

	for (unsigned i = 0; i < info->region_count; i++) {
		const struct mem_region *r = &info->regions[i];

		if (r->kind != MEM_USABLE)
			continue;

		if (r->base < end && r->base + r->size > start) {
			kprintf("pci: the window at %p is not free -- this "
				"machine reports usable memory there, so "
				"unplaced devices stay unusable\n",
				(void *)(uintptr_t)start);
			return false;
		}
	}

	*base = start;
	*size = end - start;
	return true;
}
