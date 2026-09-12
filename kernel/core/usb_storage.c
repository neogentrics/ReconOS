/* USB mass storage: SCSI in an envelope, on top of two bulk endpoints.
 *
 * Checkpoint 11b's second half. xhci.c got a device to the point where it
 * answers; this makes the answers mean something.
 *
 * The protocol is called Bulk-Only Transport and it is three transfers per
 * command: a 31-byte wrapper out, the data in whichever direction, a 13-byte
 * status in. There is no queue and no tagging beyond a number the device
 * echoes back, which is why this file is short and why it is synchronous --
 * the wire is synchronous, and pretending otherwise would only move the
 * waiting somewhere less visible.
 *
 * Inside the wrapper is a SCSI command, unchanged from the ones a disk on a
 * SAS cable would take. That is the whole trick of USB storage: it is not a
 * storage protocol, it is a way of carrying one. So this file speaks SCSI, and
 * the only USB-shaped things in it are the wrapper, the reset, and the fact
 * that a stall has to be cleared before anything else will work.
 *
 * What is deliberately not here: queueing, UAS (the newer transport, which is
 * SCSI over four endpoints and worth having later), and any attempt to hide a
 * removable device disappearing. A stick pulled mid-read returns an error.
 */
#include <recon/kernel/block.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/sched.h>
#include <recon/kernel/time.h>
#include <recon/kernel/types.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/xhci.h>

/* --- the envelope ---------------------------------------------------------
 *
 * Both wrappers are little-endian on the wire regardless of the host, and both
 * are built byte by byte below rather than by casting a struct over a buffer:
 * the layout is packed in a way C will not promise, and a compiler that adds
 * padding here produces a wrapper the device rejects with no explanation.
 */
#define CBW_SIGNATURE	0x43425355u	/* "USBC" */
#define CSW_SIGNATURE	0x53425355u	/* "USBS" */
#define CBW_LENGTH	31
#define CSW_LENGTH	13

#define CSW_PASSED	0
#define CSW_FAILED	1
#define CSW_PHASE_ERROR	2

/* SCSI operation codes. The six this kernel needs, out of a few hundred. */
#define SCSI_TEST_UNIT_READY	0x00
#define SCSI_REQUEST_SENSE	0x03
#define SCSI_INQUIRY		0x12
#define SCSI_READ_CAPACITY_10	0x25
#define SCSI_READ_10		0x28
#define SCSI_WRITE_10		0x2A
#define SCSI_SYNCHRONIZE_CACHE	0x35

/* Class-specific requests on the interface, not on the device. */
#define BOT_GET_MAX_LUN		0xFE
#define BOT_RESET		0xFF

/* Bytes moved in one request. The block layer splits to this, and it is a page
 * count rather than a byte count because the bounce buffer is pages: 64 KiB is
 * the largest run a single TRB may name anyway, so nothing is lost by
 * stopping there. */
#define BOUNCE_PAGES	16
#define BOUNCE_BYTES	(BOUNCE_PAGES * PAGE_SIZE)

struct usb_storage {
	struct xhci *x;
	struct usb_device *ud;

	u32 tag;		/* echoed back in the status wrapper */
	u8  lun;

	u32 block_size;
	u64 block_count;

	/* One physically contiguous run the controller can reach, because a
	 * caller's buffer is neither of those things by construction: the block
	 * interface takes any kernel pointer, and a run of pages that is
	 * contiguous in the kernel's address space need not be contiguous in
	 * the machine's. Copying costs a memcpy per request; not copying costs
	 * a read that silently returns another page's contents. */
	void   *bounce;
	paddr_t bounce_phys;

	struct block_device *bdev;
};

static struct usb_storage drives[4];
static unsigned drive_count;

/* --- SCSI over the envelope ------------------------------------------------ */

static void put_le32(u8 *p, u32 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
	p[2] = (u8)(v >> 16);
	p[3] = (u8)(v >> 24);
}

static u32 get_le32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
	       ((u32)p[3] << 24);
}

/* SCSI itself is big-endian, inside a wrapper that is not. Both appear in the
 * same buffer, a few bytes apart, which is exactly the kind of thing that gets
 * written once with the wrong helper and read back as a plausible number. */
static void put_be32(u8 *p, u32 v)
{
	p[0] = (u8)(v >> 24);
	p[1] = (u8)(v >> 16);
	p[2] = (u8)(v >> 8);
	p[3] = (u8)v;
}

static u32 get_be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) |
	       (u32)p[3];
}

/* One command: wrapper out, data, status in.
 *
 * `data` must be memory the controller can reach -- callers below use the
 * bounce buffer or the device's own page, never a caller's pointer. Returns
 * the status byte, or -1 if the exchange itself did not complete. */
static int command(struct usb_storage *s, const u8 *cdb, unsigned cdb_len,
		   bool in, void *data, u32 length)
{
	u8 *wrap = s->ud->buffer;
	u32 tag = ++s->tag;
	u32 moved = 0;
	unsigned i;

	if (cdb_len > 16)
		return -1;

	kmemset(wrap, 0, CBW_LENGTH);
	put_le32(wrap + 0, CBW_SIGNATURE);
	put_le32(wrap + 4, tag);
	put_le32(wrap + 8, length);
	wrap[12] = in ? 0x80 : 0x00;
	wrap[13] = s->lun;
	wrap[14] = (u8)cdb_len;
	for (i = 0; i < cdb_len; i++)
		wrap[15 + i] = cdb[i];

	if (!xhci_bulk_transfer(s->x, s->ud, false, wrap, CBW_LENGTH, &moved) ||
	    moved != CBW_LENGTH)
		return -1;

	if (length && !xhci_bulk_transfer(s->x, s->ud, in, data, length, &moved))
		return -1;

	/* The status wrapper goes in the device's page, not next to the command
	 * wrapper: the command wrapper is still the buffer a failed data phase
	 * might be writing into, and reusing it would make a torn read look
	 * like a bad signature. */
	kmemset(wrap + 64, 0, CSW_LENGTH);

	if (!xhci_bulk_transfer(s->x, s->ud, true, wrap + 64, CSW_LENGTH,
				&moved) ||
	    moved != CSW_LENGTH)
		return -1;

	if (get_le32(wrap + 64) != CSW_SIGNATURE)
		return -1;

	/* The tag is the only thing tying this status to this command. A device
	 * that answers with somebody else's tag has lost track of the exchange,
	 * and believing it would attribute one command's failure to another. */
	if (get_le32(wrap + 68) != tag)
		return -1;

	return wrap[76];
}

/* --- SCSI commands --------------------------------------------------------- */

static bool test_unit_ready(struct usb_storage *s)
{
	u8 cdb[6];

	kmemset(cdb, 0, sizeof(cdb));
	cdb[0] = SCSI_TEST_UNIT_READY;

	return command(s, cdb, sizeof(cdb), true, NULL, 0) == CSW_PASSED;
}

/* Asks why the last command failed, and throws the answer away.
 *
 * That is not laziness: a device that has failed a command holds sense data
 * until somebody reads it, and refuses everything after with the same failure
 * until they do. So the read has to happen even when nothing is done with what
 * it says. What it says is worth printing one day; not reading it at all is
 * what turns one recoverable error into a dead device. */
static void drain_sense(struct usb_storage *s)
{
	u8 cdb[6];
	u8 *sense = (u8 *)s->ud->buffer + 128;

	kmemset(cdb, 0, sizeof(cdb));
	cdb[0] = SCSI_REQUEST_SENSE;
	cdb[4] = 18;

	kmemset(sense, 0, 18);
	(void)command(s, cdb, sizeof(cdb), true, sense, 18);
}

/* Waits for a device that is not ready yet.
 *
 * A stick that has just been given an address is often still spinning up its
 * controller, and answers the first few commands with "not ready". Treating
 * the first refusal as a missing device is how a drive that works perfectly
 * fails to appear about a third of the time. */
static bool wait_until_ready(struct usb_storage *s)
{
	unsigned attempt;

	for (attempt = 0; attempt < 20; attempt++) {
		if (test_unit_ready(s))
			return true;

		drain_sense(s);

		{
			u64 until = time_monotonic_ns() + 50ull * 1000 * 1000;

			while (time_monotonic_ns() < until)
				sched_yield();
		}
	}

	return false;
}

static bool inquiry(struct usb_storage *s, char *vendor, char *product)
{
	u8 cdb[6];
	u8 *buf = (u8 *)s->ud->buffer + 128;
	unsigned i;

	kmemset(cdb, 0, sizeof(cdb));
	cdb[0] = SCSI_INQUIRY;
	cdb[4] = 36;

	kmemset(buf, 0, 36);

	if (command(s, cdb, sizeof(cdb), true, buf, 36) != CSW_PASSED)
		return false;

	/* Both fields are space-padded and not terminated. Trailing spaces are
	 * trimmed rather than kept, so that a name printed next to another
	 * name lines up. */
	for (i = 0; i < 8; i++)
		vendor[i] = (char)buf[8 + i];
	vendor[8] = '\0';
	for (i = 8; i > 0 && vendor[i - 1] == ' '; i--)
		vendor[i - 1] = '\0';

	for (i = 0; i < 16; i++)
		product[i] = (char)buf[16 + i];
	product[16] = '\0';
	for (i = 16; i > 0 && product[i - 1] == ' '; i--)
		product[i - 1] = '\0';

	return true;
}

static bool read_capacity(struct usb_storage *s)
{
	u8 cdb[10];
	u8 *buf = (u8 *)s->ud->buffer + 128;
	u32 last_lba, block_size;

	kmemset(cdb, 0, sizeof(cdb));
	cdb[0] = SCSI_READ_CAPACITY_10;

	kmemset(buf, 0, 8);

	if (command(s, cdb, sizeof(cdb), true, buf, 8) != CSW_PASSED)
		return false;

	last_lba   = get_be32(buf);
	block_size = get_be32(buf + 4);

	/* The *last* block's number, not how many there are. Reading it as a
	 * count loses the last block of every device, which is where a GPT
	 * keeps its backup header -- so the disk would look unpartitioned in
	 * exactly the case where it is not. */
	if (!block_size || (block_size & (block_size - 1)))
		return false;
	if (last_lba == 0xFFFFFFFFu)
		return false;		/* needs READ CAPACITY(16); not yet */

	s->block_size  = block_size;
	s->block_count = (u64)last_lba + 1;

	return true;
}

/* Whether the medium spins.
 *
 * BG-127: the block layer's `seek_is_free` defaults to false, and for a USB
 * flash drive that default is wrong -- it reports rotating for something with
 * no platter, which steers the allocator to work at keeping files contiguous
 * for no reason and, more seriously, is the same not-knowing that governs
 * discard. SCSI has an answer: vital product data page B1, Block Device
 * Characteristics, whose medium rotation rate is 1 for non-rotating media.
 *
 * A device that does not offer the page leaves the default alone. That is the
 * safe direction and it is a *measured* default rather than a guess: the
 * question was asked and the device declined to answer. */
static bool medium_is_solid_state(struct usb_storage *s)
{
	u8 cdb[6];
	u8 *buf = (u8 *)s->ud->buffer + 128;
	u16 rate;

	kmemset(cdb, 0, sizeof(cdb));
	cdb[0] = SCSI_INQUIRY;
	cdb[1] = 0x01;			/* enable vital product data */
	cdb[2] = 0xB1;			/* block device characteristics */
	cdb[4] = 64;

	kmemset(buf, 0, 64);

	if (command(s, cdb, sizeof(cdb), true, buf, 64) != CSW_PASSED) {
		drain_sense(s);
		return false;
	}

	if (buf[1] != 0xB1)
		return false;		/* answered with a different page */

	rate = (u16)((buf[4] << 8) | buf[5]);

	return rate == 1;
}

/* --- the block device ------------------------------------------------------ */

static enum block_status transfer(struct usb_storage *s, bool in, u64 lba,
				  u32 count, void *buf)
{
	u8 cdb[10];
	u32 bytes = count * s->block_size;
	int status;

	if (lba > 0xFFFFFFFFull)
		return BLOCK_ERR_RANGE;		/* needs READ(16) */
	if (bytes > BOUNCE_BYTES)
		return BLOCK_ERR_RANGE;

	kmemset(cdb, 0, sizeof(cdb));
	cdb[0] = in ? SCSI_READ_10 : SCSI_WRITE_10;
	put_be32(cdb + 2, (u32)lba);
	cdb[7] = (u8)(count >> 8);
	cdb[8] = (u8)count;

	if (!in)
		kmemcpy(s->bounce, buf, bytes);

	status = command(s, cdb, sizeof(cdb), in, s->bounce, bytes);

	if (status < 0)
		return BLOCK_ERR_TIMEOUT;
	if (status != CSW_PASSED) {
		drain_sense(s);
		return BLOCK_ERR_IO;
	}

	if (in)
		kmemcpy(buf, s->bounce, bytes);

	return BLOCK_OK;
}

static enum block_status usb_read(struct block_device *dev, u64 lba, u32 count,
				  void *buf)
{
	return transfer(dev->driver, true, lba, count, buf);
}

static enum block_status usb_write(struct block_device *dev, u64 lba, u32 count,
				   const void *buf)
{
	return transfer(dev->driver, false, lba, count, (void *)buf);
}

/* SYNCHRONIZE CACHE, and it is issued rather than assumed.
 *
 * A stick with a write cache reports a write complete before it has reached
 * flash, and the whole point of block_flush is to be able to say "and mean
 * it". A device that does not implement the command answers with a failure,
 * which is reported -- not swallowed as success, because a caller told a flush
 * succeeded when nothing reached the medium draws exactly the wrong conclusion
 * about what survives a power loss. */
static enum block_status usb_flush(struct block_device *dev)
{
	struct usb_storage *s = dev->driver;
	u8 cdb[10];
	int status;

	kmemset(cdb, 0, sizeof(cdb));
	cdb[0] = SCSI_SYNCHRONIZE_CACHE;	/* whole device: LBA 0, count 0 */

	status = command(s, cdb, sizeof(cdb), true, NULL, 0);

	if (status < 0)
		return BLOCK_ERR_TIMEOUT;
	if (status != CSW_PASSED) {
		drain_sense(s);
		return BLOCK_ERR_IO;
	}

	return BLOCK_OK;
}

static const struct block_ops usb_ops = {
	.read  = usb_read,
	.write = usb_write,
	.flush = usb_flush,
	.discard = NULL,	/* UNMAP exists in SCSI; nothing asks yet */
};

/* --- attaching ------------------------------------------------------------- */

bool usb_storage_attach(struct xhci *x, struct usb_device *ud)
{
	struct usb_storage *s;
	char vendor[9], product[17];
	char name[BLOCK_NAME_MAX];
	paddr_t bounce;

	/* Class 8 is mass storage, subclass 6 is "SCSI commands, unmodified",
	 * and protocol 0x50 is Bulk-Only Transport. All three, because the
	 * other combinations are different protocols wearing the same class:
	 * 0x62 is UAS, and the older subclasses carry command sets that are not
	 * SCSI and would be misread as it. */
	if (ud->usb_class != 0x08 || ud->usb_subclass != 0x06 ||
	    ud->usb_protocol != 0x50)
		return false;

	if (!ud->configured)
		return false;

	if (drive_count >= sizeof(drives) / sizeof(drives[0]))
		return false;

	bounce = pmm_alloc_pages(BOUNCE_PAGES);
	if (!bounce) {
		kputs("  usb-storage  : no memory for a transfer buffer\n");
		return false;
	}

	s = &drives[drive_count];
	kmemset(s, 0, sizeof(*s));
	s->x           = x;
	s->ud          = ud;
	s->bounce_phys = bounce;
	s->bounce      = phys_to_virt(bounce);

	/* How many logical units are behind this one interface. A stick has
	 * one; a card reader with four slots has four and reports three here.
	 * Only the first is used, but asking is not optional -- a device that
	 * is never asked answers the first command with a stall on some
	 * firmware, and the stall is what breaks everything after it. */
	{
		u8 *lun = (u8 *)ud->buffer + 192;

		*lun = 0;

		/* A device is allowed to stall this rather than answer, and a
		 * stall here means one unit -- not a failure. What is not
		 * allowed is skipping the question: some firmware waits for it
		 * before it will answer anything else. */
		if (xhci_control_transfer(x, ud, 0xA1, BOT_GET_MAX_LUN, 0,
					  ud->interface, lun, 1))
			s->lun = 0;	/* only the first unit, for now */
	}

	if (!wait_until_ready(s)) {
		kprintf("  usb-storage  : slot %u never became ready\n",
			ud->slot);
		goto fail;
	}

	if (!inquiry(s, vendor, product)) {
		kprintf("  usb-storage  : slot %u would not identify itself\n",
			ud->slot);
		goto fail;
	}

	if (!read_capacity(s)) {
		kprintf("  usb-storage  : %s %s would not report its size\n",
			vendor, product);
		goto fail;
	}

	kstrlcpy(name, "usb", sizeof(name));
	name[3] = (char)('0' + drive_count);
	name[4] = '\0';

	s->bdev = block_register(name, &usb_ops, s, s->block_size,
				 s->block_count);
	if (!s->bdev)
		goto fail;

	s->bdev->removable    = true;
	s->bdev->transfer_hint = BOUNCE_BYTES;
	s->bdev->max_blocks_per_request = BOUNCE_BYTES / s->block_size;

	/* Asked, not inferred from the bus. See medium_is_solid_state. */
	s->bdev->seek_is_free = medium_is_solid_state(s);

	/* SYNCHRONIZE CACHE is issued for real, so a flush that returns success
	 * means the device said so. Whether the device was telling the truth is
	 * not something any bus can establish. */
	s->bdev->flush_is_durable = true;

	kprintf("  %-12s : %s %s, %llu blocks of %u bytes%s\n",
		name, vendor, product,
		(unsigned long long)s->block_count, s->block_size,
		s->bdev->seek_is_free ? ", solid state" : "");

	drive_count++;
	return true;

fail:
	pmm_free_pages(bounce, BOUNCE_PAGES);
	return false;
}

void usb_storage_release(struct usb_device *ud)
{
	unsigned i;

	if (!ud)
		return;

	for (i = 0; i < 4; i++) {
		struct usb_storage *s = &drives[i];

		if (s->ud != ud)
			continue;

		/* The block layer retires the identity rather than freeing the
		 * slot, so anything still holding this disk by (id, generation)
		 * now fails to find it instead of finding whatever is plugged
		 * in next. */
		if (s->bdev)
			block_unregister(s->bdev);

		kmemset(s, 0, sizeof(*s));
	}
}

unsigned usb_storage_count(void)
{
	return drive_count;
}
