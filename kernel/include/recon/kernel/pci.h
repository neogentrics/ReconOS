/* PCI: the bus that has to be walked before this machine has any storage.
 *
 * Every disk controller a PC has ever had is on it -- NVMe, AHCI, the IDE
 * controllers before them, and the virtio devices a hypervisor offers. So this
 * is not a detour on the way to a disk driver; it is the road.
 *
 * --- What PCI actually is, in one paragraph ---
 *
 * A tree of buses, each carrying up to thirty-two devices, each of which may
 * present up to eight *functions*. Every function has 256 bytes of
 * configuration space at a fixed layout, and reading the first four bytes of it
 * says who made the device and what it is. There is no discovery protocol
 * beyond that: the kernel reads every possible address and the ones that answer
 * 0xFFFF are not there.
 *
 * --- The part that is not automatic ---
 *
 * A device's registers are not at a fixed address. Each function has up to six
 * *base address registers* saying where it would like its registers to appear,
 * and something has to decide where. On a machine with firmware, the firmware
 * already did -- SeaBIOS and OVMF both assign every BAR before handing over.
 * On a machine booted directly by a hypervisor there is no firmware at all, the
 * registers read back as zero, and the kernel has to size and place them
 * itself.
 *
 * Three boot paths out of four therefore work without that code, which is
 * exactly what makes it worth writing carefully rather than discovering later.
 */
#ifndef RECON_KERNEL_PCI_H
#define RECON_KERNEL_PCI_H

#include <recon/kernel/types.h>

/* Configuration space offsets, from the specification. */
#define PCI_VENDOR_ID        0x00
#define PCI_DEVICE_ID        0x02
#define PCI_COMMAND          0x04
#define PCI_STATUS           0x06
#define PCI_REVISION         0x08
#define PCI_PROG_IF          0x09
#define PCI_SUBCLASS         0x0A
#define PCI_CLASS            0x0B
#define PCI_HEADER_TYPE      0x0E
#define PCI_BAR0             0x10
#define PCI_SUBSYSTEM_VENDOR 0x2C
#define PCI_SUBSYSTEM_ID     0x2E
#define PCI_CAPABILITIES     0x34

/* Command register bits. Nothing works until the right ones are set: a device
 * whose memory decoding is off ignores every access to its registers and
 * returns all-ones, which reads exactly like a device that is not there. */
#define PCI_COMMAND_IO      0x0001
#define PCI_COMMAND_MEMORY  0x0002
#define PCI_COMMAND_MASTER  0x0004	/* may read and write main memory itself */

#define PCI_STATUS_CAP_LIST 0x0010

#define PCI_MAX_DEVICES 32

struct pci_device {
	u8 bus, slot, func;

	u16 vendor, device;
	u8 class_code, subclass, prog_if, revision;
	u16 subsystem_vendor, subsystem_id;

	/* Where each base address register ended up, and how big it is. Zero
	 * length means the register is unimplemented, which is the usual case
	 * for most of the six. */
	u64 bar[6];
	u64 bar_size[6];
	bool bar_is_io[6];
};

/* Reading and writing one function's configuration space. Sizes matter: some
 * registers must be written as a whole word or the device ignores the write. */
u32 pci_read32(const struct pci_device *d, u8 offset);
u16 pci_read16(const struct pci_device *d, u8 offset);
u8  pci_read8(const struct pci_device *d, u8 offset);
void pci_write32(const struct pci_device *d, u8 offset, u32 value);
void pci_write16(const struct pci_device *d, u8 offset, u16 value);

/* Walks the bus, sizes and places every base address register that needs it,
 * and fills the table. Safe to call on a machine with no PCI at all. */
void pci_scan(void);

unsigned pci_device_count(void);
struct pci_device *pci_device_at(unsigned index);

/* Finds the next capability of the given id at or after `from`, returning its
 * offset in configuration space, or zero.
 *
 * Capabilities are a linked list threaded through configuration space, and the
 * same id can appear more than once -- virtio publishes four vendor-specific
 * capabilities that differ only in a field inside them. So this takes a
 * starting point rather than returning "the" capability. */
u8 pci_find_capability(const struct pci_device *d, u8 id, u8 from);

void pci_print_summary(void);

/* --- What the architecture provides ---------------------------------------
 *
 * Configuration space is reached through two I/O ports on x86 and through a
 * memory window on everything else, and the address of that window is something
 * only the machine's own description knows. */

/* False on a machine with no PCI, and everything above becomes a no-op. */
bool arch_pci_available(void);

u32  arch_pci_config_read(u8 bus, u8 slot, u8 func, u8 offset);
void arch_pci_config_write(u8 bus, u8 slot, u8 func, u8 offset, u32 value);

/* Where unassigned memory-mapped registers may be placed. Only consulted on a
 * machine whose firmware left the base address registers empty, which means a
 * machine with no firmware. Returns false when the architecture has no window
 * to offer, in which case unassigned devices stay unusable and say so. */
bool arch_pci_mmio_window(u64 *base, u64 *size);

#endif /* RECON_KERNEL_PCI_H */
