/* AHCI: the SATA controller, for every machine built between the IDE era and
 * the NVMe one.
 *
 * That is roughly 2005 to 2020, which is most of the machines this kernel will
 * actually be installed on for years. NVMe covers what is new and virtio covers
 * what is virtual; this covers what people already own.
 *
 * --- It is a mailbox, not a queue ---
 *
 * NVMe and virtio both give the device a ring and an index. AHCI gives it a
 * *table of thirty-two slots* and a bitmask register: setting bit N of the
 * command-issue register means "slot N is yours now", and the controller clears
 * the bit when it is finished. There is no ordering and no head or tail --
 * which is simpler to drive and is why this driver is shorter than the NVMe one
 * despite the hardware being older.
 *
 * Each slot points at a *command table*, and a command table holds:
 *
 *   a Frame Information Structure -- an ATA command in the shape SATA sends it
 *   an optional ATAPI packet, unused here
 *   a scatter list, of address and length
 *
 * --- The two things that make a first AHCI driver hang ---
 *
 * The engine must be stopped before its pointers are changed, and stopping it
 * is two bits and two waits rather than one: clear ST and wait for CR, then
 * clear FRE and wait for FR. Clearing both at once and waiting for neither is
 * the natural thing to write, and it leaves the controller reading a command
 * list that has moved.
 *
 * And a port with nothing attached still exists, still has registers, and still
 * answers. Whether a disk is there is in the SATA status register's detection
 * field, and a driver that skips that check waits forever for a command it gave
 * to nobody.
 */
#include <recon/kernel/block.h>
#include <recon/kernel/pci.h>

#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/time.h>
#include <recon/kernel/sched.h>

/* Host bus adapter registers, offsets from the fifth base address register. */
#define AHCI_CAP  0x00
#define AHCI_GHC  0x04
#define AHCI_IS   0x08
#define AHCI_PI   0x0C		/* which ports are implemented */
#define AHCI_VS   0x10

#define AHCI_GHC_HR (1u << 0)	/* reset the whole controller */
#define AHCI_GHC_AE (1u << 31)	/* speak AHCI rather than pretending to be IDE */

/* Per-port registers, from 0x100 plus 0x80 per port. */
#define AHCI_PORT_BASE 0x100
#define AHCI_PORT_SIZE 0x80

#define PORT_CLB  0x00		/* command list base; 64-bit */
#define PORT_FB   0x08		/* received-FIS base; 64-bit */
#define PORT_IS   0x10
#define PORT_IE   0x14
#define PORT_CMD  0x18
#define PORT_TFD  0x20		/* the device's task file: status and error */
#define PORT_SIG  0x24		/* what kind of thing is attached */
#define PORT_SSTS 0x28		/* SATA link status */
#define PORT_SERR 0x30
#define PORT_CI   0x38		/* command issue: one bit per slot */

#define PORT_CMD_ST  (1u << 0)	/* start processing the command list */
#define PORT_CMD_FRE (1u << 4)	/* start receiving FISes */
#define PORT_CMD_FR  (1u << 14)	/* ...and it has */
#define PORT_CMD_CR  (1u << 15)	/* ...and it is */

#define TFD_ERR (1u << 0)
#define TFD_DRQ (1u << 3)
#define TFD_BSY (1u << 7)

#define SIG_SATA_DISK 0x101	/* a plain SATA drive; other values are not one */

/* ATA commands. The EXT forms take a 48-bit block number; the older ones take
 * 28 bits, which stops at 128 gigabytes. Only the EXT forms are used, because a
 * driver that silently truncates a block number on a large disk writes to the
 * wrong place rather than failing. */
#define ATA_READ_DMA_EXT   0x25
#define ATA_WRITE_DMA_EXT  0x35
#define ATA_FLUSH_EXT      0xEA
#define ATA_IDENTIFY       0xEC
#define ATA_DSM            0x06	/* DATA SET MANAGEMENT; feature 1 is TRIM */

#define AHCI_MAX 2
#define AHCI_PRD_MAX 120	/* what fits in the page beside everything else */

struct ahci_cmd_header {
	u16 flags;		/* FIS length in dwords, and the write bit */
	u16 prdtl;		/* how many scatter entries */
	u32 prdbc;		/* bytes the controller actually moved */
	u64 ctba;		/* the command table */
	u32 reserved[4];
} RK_PACKED;

struct ahci_prd {
	u64 dba;
	u32 reserved;
	u32 dbc;		/* byte count minus one, in the low 22 bits */
} RK_PACKED;

/* A register FIS travelling host to device: an ATA command, in the shape SATA
 * puts it on the wire. */
struct fis_h2d {
	u8 type;		/* 0x27 */
	u8 flags;		/* bit 7 says this is a command, not a control */
	u8 command;
	u8 feature_low;
	u8 lba0, lba1, lba2, device;
	u8 lba3, lba4, lba5, feature_high;
	u8 count_low, count_high, icc, control;
	u8 reserved[4];
} RK_PACKED;

struct ahci {
	volatile u8 *regs;
	unsigned port;

	/* One page holds all three structures a port needs, at the alignments
	 * the controller insists on: the command list on a 1KB boundary, the
	 * received-FIS area on 256 bytes, the command table on 128. Laying them
	 * out inside one page satisfies all three and costs one page. */
	paddr_t page_phys;
	u8 *page;

	volatile struct ahci_cmd_header *cmd_list;	/* at 0 */
	volatile u8 *fis;				/* at 1024 */
	volatile u8 *cmd_table;				/* at 2048 */
	paddr_t cmd_table_phys;

	u32 block_size;
	bool seek_is_free;
	bool discard_supported;
	u16 rpm;		/* zero when the drive did not say */
	u16 dsm_blocks;	/* range blocks per DSM command */
	u64 block_count;

	struct block_device *bdev;
};

static struct ahci controllers[AHCI_MAX];
static unsigned controller_count;

static u32 hba_r(struct ahci *a, u32 off)
{
	return *(volatile u32 *)(a->regs + off);
}

static void hba_w(struct ahci *a, u32 off, u32 v)
{
	*(volatile u32 *)(a->regs + off) = v;
}

static u32 port_off(struct ahci *a, u32 reg)
{
	return AHCI_PORT_BASE + a->port * AHCI_PORT_SIZE + reg;
}

static u32 port_r(struct ahci *a, u32 reg)
{
	return hba_r(a, port_off(a, reg));
}

static void port_w(struct ahci *a, u32 reg, u32 v)
{
	hba_w(a, port_off(a, reg), v);
}

static void port_w64(struct ahci *a, u32 reg, u64 v)
{
	port_w(a, reg, (u32)v);
	port_w(a, reg + 4, (u32)(v >> 32));
}

/* Stops the port's two engines, in the order the specification requires and
 * waiting for each. Both waits matter: the controller keeps reading the command
 * list until CR clears, and keeps writing the received-FIS area until FR does,
 * so moving either pointer early points live hardware at freed memory. */
static bool port_stop(struct ahci *a)
{
	u64 deadline;

	port_w(a, PORT_CMD, port_r(a, PORT_CMD) & ~PORT_CMD_ST);

	deadline = time_monotonic_ns() + 1000000000ULL;
	while (port_r(a, PORT_CMD) & PORT_CMD_CR) {
		if (time_monotonic_ns() > deadline)
			return false;
		sched_yield();
	}

	port_w(a, PORT_CMD, port_r(a, PORT_CMD) & ~PORT_CMD_FRE);

	deadline = time_monotonic_ns() + 1000000000ULL;
	while (port_r(a, PORT_CMD) & PORT_CMD_FR) {
		if (time_monotonic_ns() > deadline)
			return false;
		sched_yield();
	}

	return true;
}

static void port_start(struct ahci *a)
{
	/* Receiving first. A port started before it will accept a FIS has
	 * nowhere to put the answer to the first command. */
	port_w(a, PORT_CMD, port_r(a, PORT_CMD) | PORT_CMD_FRE);
	port_w(a, PORT_CMD, port_r(a, PORT_CMD) | PORT_CMD_ST);
}

/* Waits for the device to stop being busy. */
static bool wait_not_busy(struct ahci *a, u64 timeout_ns)
{
	u64 deadline = time_monotonic_ns() + timeout_ns;

	while (port_r(a, PORT_TFD) & (TFD_BSY | TFD_DRQ)) {
		if (time_monotonic_ns() > deadline)
			return false;
		sched_yield();
	}

	return true;
}

/* Builds one command in slot zero and runs it to completion.
 *
 * Slot zero every time, because nothing here issues two at once. Thirty-two
 * slots are what makes AHCI fast under a real workload, and a driver whose
 * caller waits for each request would use them to hold one command and
 * thirty-one empty ones. */
static bool run(struct ahci *a, u8 command, u64 lba, u32 sectors,
		paddr_t buf, u32 bytes, bool write)
{
	volatile struct fis_h2d *fis;
	volatile struct ahci_prd *prd;
	u32 entries = 0;
	u64 deadline;
	paddr_t p = buf;
	u32 left = bytes;

	if (!wait_not_busy(a, 1000000000ULL))
		return false;

	fis = (volatile struct fis_h2d *)a->cmd_table;
	kmemset((void *)fis, 0, sizeof(*fis));

	fis->type    = 0x27;
	fis->flags   = 0x80;		/* this is a command */
	fis->command = command;
	fis->device  = 0x40;		/* LBA mode, which is not the default */

	fis->lba0 = (u8)lba;
	fis->lba1 = (u8)(lba >> 8);
	fis->lba2 = (u8)(lba >> 16);
	fis->lba3 = (u8)(lba >> 24);
	fis->lba4 = (u8)(lba >> 32);
	fis->lba5 = (u8)(lba >> 40);

	fis->count_low  = (u8)sectors;
	fis->count_high = (u8)(sectors >> 8);

	/* The scatter list. Each entry carries a byte count that must be even
	 * and is stored as one less than the real length -- so a zero-length
	 * entry is impossible to express and a length of one is illegal. */
	prd = (volatile struct ahci_prd *)(a->cmd_table + 0x80);

	while (left) {
		u32 chunk = left;
		u32 to_page_end = (u32)(PAGE_SIZE - (p & (PAGE_SIZE - 1)));

		if (chunk > to_page_end)
			chunk = to_page_end;

		if (entries >= AHCI_PRD_MAX)
			return false;

		prd[entries].dba       = p;
		prd[entries].reserved  = 0;
		prd[entries].dbc       = chunk - 1;
		entries++;

		p    += chunk;
		left -= chunk;
	}

	a->cmd_list[0].flags = (u16)((sizeof(struct fis_h2d) / 4)
				     | (write ? (1u << 6) : 0));
	a->cmd_list[0].prdtl = (u16)entries;
	a->cmd_list[0].prdbc = 0;
	a->cmd_list[0].ctba  = a->cmd_table_phys;

	/* Everything the controller is about to read must be visible before the
	 * register write that tells it to look -- the same requirement as a
	 * virtqueue's available index, and the same one line. */
	__atomic_thread_fence(__ATOMIC_RELEASE);

	port_w(a, PORT_IS, port_r(a, PORT_IS));	/* clear stale status, write-one */
	port_w(a, PORT_CI, 1u);

	deadline = time_monotonic_ns() + 10000000000ULL;

	while (port_r(a, PORT_CI) & 1u) {
		if (port_r(a, PORT_TFD) & TFD_ERR)
			return false;

		if (time_monotonic_ns() > deadline)
			return false;

		sched_yield();
	}

	__atomic_thread_fence(__ATOMIC_ACQUIRE);

	return (port_r(a, PORT_TFD) & TFD_ERR) == 0;
}

static enum block_status ahci_read(struct block_device *dev, u64 lba, u32 count,
				   void *buf)
{
	struct ahci *a = dev->driver;
	paddr_t phys = virt_to_phys(buf);

	if (!phys)
		return BLOCK_ERR_ALIGN;

	if (!run(a, ATA_READ_DMA_EXT, lba, count, phys, count * a->block_size,
		 false))
		return BLOCK_ERR_IO;

	return BLOCK_OK;
}

static enum block_status ahci_write(struct block_device *dev, u64 lba, u32 count,
				    const void *buf)
{
	struct ahci *a = dev->driver;
	paddr_t phys = virt_to_phys(buf);

	if (!phys)
		return BLOCK_ERR_ALIGN;

	if (!run(a, ATA_WRITE_DMA_EXT, lba, count, phys, count * a->block_size,
		 true))
		return BLOCK_ERR_IO;

	return BLOCK_OK;
}

static enum block_status ahci_flush(struct block_device *dev)
{
	struct ahci *a = dev->driver;

	if (!run(a, ATA_FLUSH_EXT, 0, 0, 0, 0, false))
		return BLOCK_ERR_IO;

	return BLOCK_OK;
}

static const struct block_ops ahci_ops = {
	.read  = ahci_read,
	.write = ahci_write,
	.flush = ahci_flush,
};

/* Reads the drive's own description and pulls out the two numbers that matter.
 *
 * IDENTIFY returns 256 sixteen-bit words, and their meaning is forty years of
 * accumulated history. Only three fields are read here, and each one is read
 * the careful way rather than the usual way:
 *
 *   the 48-bit capacity, because the 28-bit one stops at 128 gigabytes
 *   whether the logical sector is larger than 512 bytes, which 4K-native
 *     drives report and which a driver that assumes 512 corrupts
 */
static bool identify(struct ahci *a, paddr_t scratch_phys, const u16 *words)
{
	u64 sectors = 0;
	u32 sector_size = 512;

	if (!run(a, ATA_IDENTIFY, 0, 0, scratch_phys, 512, false))
		return false;

	/* Word 83 bit 10 says the 48-bit commands are supported, and words 100
	 * to 103 hold the capacity in that form. */
	if (words[83] & (1u << 10)) {
		for (unsigned i = 0; i < 4; i++)
			sectors |= (u64)words[100 + i] << (i * 16);
	} else {
		sectors = (u64)words[60] | ((u64)words[61] << 16);
	}

	if (!sectors)
		return false;

	/* Word 106 is valid when its top two bits are 01. Bit 12 then says the
	 * logical sector is longer than 512 bytes, and words 117 and 118 hold
	 * how long in *words*. */
	if ((words[106] & 0xC000u) == 0x4000u && (words[106] & (1u << 12))) {
		u32 in_words = (u32)words[117] | ((u32)words[118] << 16);

		if (in_words >= 256 && in_words <= 32768)
			sector_size = in_words * 2;
	}

	/* Word 217 is the nominal media rotation rate, and it is the only place
	 * a drive says outright what it is made of:
	 *
	 *      0       not reported
	 *      1       non-rotating -- solid state
	 *   0x0401..0xFFFE   rotations per minute
	 *
	 * Anything else is a drive that did not answer, and the answer is then
	 * "assume it spins". That is the safe direction: treating an SSD as a
	 * disk costs a little allocator effort, while treating a disk as an SSD
	 * fragments it in a way that cannot be undone without rewriting the
	 * volume.
	 *
	 * Read here rather than guessed from the bus, because SATA carries both
	 * kinds and has for fifteen years. */
	a->seek_is_free = (words[217] == 1);
	a->rpm = (words[217] >= 0x0401 && words[217] <= 0xFFFE) ? words[217] : 0;

	/* Word 169 bit 0: the drive supports DATA SET MANAGEMENT, whose only
	 * defined function is TRIM. */
	a->discard_supported = (words[169] & 1u) != 0;

	/* Word 105 is the largest number of 512-byte blocks of range
	 * descriptors one DSM command may carry. Each descriptor covers up to
	 * 65535 sectors, and there are 64 per block. Zero means the drive did
	 * not say, and one block is the floor the specification guarantees. */
	a->dsm_blocks = words[105] ? words[105] : 1;

	a->block_count = sectors;
	a->block_size  = sector_size;
	return true;
}

/* Takes a PCI function that says it is an AHCI controller and makes its first
 * attached disk a block device. */
bool ahci_attach(const struct pci_device *d)
{
	struct ahci *a;
	char name[BLOCK_NAME_MAX];
	paddr_t scratch;
	u32 ports;
	bool found = false;

	/* Class 1 subclass 6 is "serial ATA"; programming interface 1 is the
	 * AHCI register set. A controller in the same subclass with a different
	 * interface is in legacy IDE mode and is a different driver. */
	if (d->class_code != 0x01 || d->subclass != 0x06 || d->prog_if != 0x01)
		return false;

	if (controller_count >= AHCI_MAX)
		return false;

	a = &controllers[controller_count];
	kmemset(a, 0, sizeof(*a));

	{
		paddr_t base = (paddr_t)d->bar[5];
		paddr_t first;
		u64 span;
		vaddr_t va;

		/* The fifth register, not the first. AHCI is the one controller
		 * here whose registers are not in BAR0 -- the earlier ones are
		 * the legacy IDE ports it can also pretend to be. */
		if (!base || d->bar_is_io[5] || !d->bar_size[5])
			return false;

		first = PAGE_ALIGN_DOWN(base);
		span  = PAGE_ALIGN_UP((base - first) + d->bar_size[5]);
		va    = (vaddr_t)(uintptr_t)phys_to_virt(first);

		if (!vm_lookup(va) &&
		    !vm_map(va, first, span,
			    VM_READ | VM_WRITE | VM_DEVICE | VM_GLOBAL)) {
			kputs("ahci: could not map the controller's registers\n");
			return false;
		}

		a->regs = phys_to_virt(base);
	}

	/* Say we are speaking AHCI. Without this the controller may still be
	 * emulating an IDE interface, and every register below means something
	 * else. */
	hba_w(a, AHCI_GHC, hba_r(a, AHCI_GHC) | AHCI_GHC_AE);

	a->page_phys = pmm_alloc_page();
	if (!a->page_phys) {
		kputs("ahci: no memory for the command list\n");
		return false;
	}
	a->page = phys_to_virt(a->page_phys);

	a->cmd_list       = (volatile struct ahci_cmd_header *)a->page;
	a->fis            = a->page + 1024;
	a->cmd_table      = a->page + 2048;
	a->cmd_table_phys = a->page_phys + 2048;

	scratch = pmm_alloc_page();
	if (!scratch) {
		pmm_free_page(a->page_phys);
		kputs("ahci: no memory to identify the drive with\n");
		return false;
	}

	/* Which ports exist is a bitmask, and a port that exists may still have
	 * nothing plugged into it. Both have to be checked, and the second is
	 * the one that hangs a driver that skips it. */
	ports = hba_r(a, AHCI_PI);

	for (unsigned p = 0; p < 32 && !found; p++) {
		u32 det, sig;

		if (!(ports & (1u << p)))
			continue;

		a->port = p;

		/* Three means a device is present and the link is up. One means
		 * present but not communicating, which is a cable problem on
		 * real hardware and never happens under emulation. */
		det = port_r(a, PORT_SSTS) & 0xF;
		if (det != 3)
			continue;

		if (!port_stop(a)) {
			kprintf("ahci: port %u would not stop\n", p);
			continue;
		}

		kmemset(a->page, 0, PAGE_SIZE);

		port_w64(a, PORT_CLB, a->page_phys);
		port_w64(a, PORT_FB,  a->page_phys + 1024);

		port_w(a, PORT_SERR, port_r(a, PORT_SERR));	/* write-one-to-clear */
		port_w(a, PORT_IS,   port_r(a, PORT_IS));
		port_w(a, PORT_IE,   0);	/* nothing services interrupts yet */

		port_start(a);

		/* The signature is read *here*, not before the port was started,
		 * and that ordering cost a boot.
		 *
		 * The register does not hold a constant describing the socket;
		 * it holds the first four bytes the attached device sent, which
		 * it sends in a FIS -- so it means nothing until the port is
		 * receiving FISes. Before that it reads as all ones.
		 *
		 * On three of this kernel's four boot paths the firmware had
		 * already started every port, so the signature was valid early
		 * and reading it early worked. On the one path with no firmware
		 * it was all ones, and a disk that was plainly attached was
		 * skipped as "not a disk". Correct on the machines it was tried
		 * on, which is its own way for a bug to hide. */
		if (!wait_not_busy(a, 1000000000ULL)) {
			port_stop(a);
			continue;
		}

		sig = port_r(a, PORT_SIG);
		if (sig != SIG_SATA_DISK) {
			/* An optical drive, an enclosure, or a port whose device
			 * never announced itself. None of them is a disk. */
			port_stop(a);
			continue;
		}

		if (identify(a, scratch, phys_to_virt(scratch))) {
			found = true;
			break;
		}

		port_stop(a);
	}

	pmm_free_page(scratch);

	if (!found) {
		pmm_free_page(a->page_phys);
		return false;
	}

	kstrlcpy(name, "sata0", sizeof(name));
	name[4] = (char)('0' + controller_count);

	a->bdev = block_register(name, &ahci_ops, a, a->block_size, a->block_count);
	if (!a->bdev) {
		port_stop(a);
		pmm_free_page(a->page_phys);
		return false;
	}

	/* What the drive said about itself, passed up rather than re-derived.
	 * A filesystem asking "is a seek free here" must get the drive's answer,
	 * not a guess made from the driver's name. */
	a->bdev->seek_is_free      = a->seek_is_free;
	/* The drive's answer AND whether this driver can act on it. Reporting
	 * the drive's answer alone would tell a filesystem that discard works
	 * here when block_discard would refuse it -- the same shape of lie as
	 * a flush that returns success having issued nothing. */
	a->bdev->discard_supported = a->discard_supported &&
				     ahci_ops.discard != NULL;

	if (a->seek_is_free)
		kprintf("%s: solid state\n", name);
	else if (a->rpm)
		kprintf("%s: %u rpm\n", name, (unsigned)a->rpm);
	else
		kprintf("%s: the drive did not say whether it spins; "
			"assuming it does\n", name);

	/* One scatter entry per page, and a fixed number of them. Stated so the
	 * block layer splits rather than the driver failing a request a caller
	 * had every right to make. */
	/* FLUSH CACHE EXT is issued and waited for. */
	a->bdev->flush_is_durable = true;

	a->bdev->max_blocks_per_request =
		(u32)(AHCI_PRD_MAX * PAGE_SIZE / a->block_size);

	controller_count++;
	return true;
}
