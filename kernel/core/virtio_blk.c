/* virtio-blk: the first thing this kernel can store something on.
 *
 * A request is three buffers in one descriptor chain, and that shape is not an
 * implementation choice -- it is the protocol:
 *
 *   a header the device reads   (what kind of request, and which sector)
 *   the data, one way or the other
 *   one status byte the device writes
 *
 * The header and the status must be separate descriptors from the data, because
 * the device writes one and reads the other and a descriptor carries a single
 * direction. Putting the status inside the same buffer as the data -- which is
 * the obvious thing to do with a small struct -- means asking the device to
 * write into a buffer it was told to read.
 *
 * --- Sectors are 512 bytes here, whatever the disk says ---
 *
 * `capacity` and the sector number in the header are counted in 512-byte units
 * unconditionally, even on a device that reports a 4096-byte physical block.
 * That is the specification, and it is the kind of detail that produces a
 * driver which works on every disk anyone tested it against and corrupts the
 * first 4K-native one. So this driver reports 512 upward and does no
 * arithmetic of its own.
 */
#include <recon/kernel/block.h>
#include <recon/kernel/virtio.h>

#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/time.h>
#include <recon/kernel/sched.h>

/* Request types. */
#define VIRTIO_BLK_T_IN     0	/* device -> memory */
#define VIRTIO_BLK_T_OUT    1	/* memory -> device */
#define VIRTIO_BLK_T_FLUSH  4

/* What the device writes into the status byte. */
#define VIRTIO_BLK_S_OK     0
#define VIRTIO_BLK_S_IOERR  1
#define VIRTIO_BLK_S_UNSUPP 2

/* Feature bits this driver cares about. */
#define VIRTIO_BLK_F_SIZE_MAX  1
#define VIRTIO_BLK_F_SEG_MAX   2
#define VIRTIO_BLK_F_RO        5
#define VIRTIO_BLK_F_BLK_SIZE  6
#define VIRTIO_BLK_F_FLUSH     9

#define VIRTIO_BLK_SECTOR 512

struct virtio_blk_req_header {
	u32 type;
	u32 reserved;
	u64 sector;
} RK_PACKED;

#define VIRTIO_BLK_MAX 4

struct virtio_blk {
	struct virtio_device dev;
	struct virtqueue q;

	/* One page, split between the header and the status byte. Allocated
	 * once rather than per request, because a request needs the *physical*
	 * address of both and taking that of a stack variable would hand the
	 * device a pointer into a kernel stack -- which happens to work on this
	 * kernel today and is exactly the kind of thing that stops working when
	 * stacks stop being in the direct map. */
	paddr_t scratch_phys;
	struct virtio_blk_req_header *header;
	volatile u8 *status;

	struct block_device *bdev;
	bool in_use;
};

static struct virtio_blk devices[VIRTIO_BLK_MAX];
static unsigned device_count;

/* Runs one request to completion.
 *
 * Polls rather than waits, because the scheduler has no way to block yet. The
 * loop yields, so the machine is not wedged while a disk thinks, but it does
 * burn this thread's slice. When there is a wait queue this function is where
 * it goes, and nothing above it changes. */
static enum block_status run(struct virtio_blk *b, u32 type, u64 sector,
			     paddr_t data, u32 data_len, bool device_writes)
{
	paddr_t bufs[3];
	u32 lens[3];
	bool wr[3];
	unsigned n = 0;
	u16 head;
	u64 deadline;

	b->header->type     = type;
	b->header->reserved = 0;
	b->header->sector   = sector;
	*b->status          = 0xFF;	/* not a value the device ever writes */

	bufs[n] = b->scratch_phys;
	lens[n] = sizeof(*b->header);
	wr[n]   = false;		/* the device reads the header */
	n++;

	if (data_len) {
		bufs[n] = data;
		lens[n] = data_len;
		wr[n]   = device_writes;
		n++;
	}

	bufs[n] = b->scratch_phys + sizeof(*b->header);
	lens[n] = 1;
	wr[n]   = true;			/* the device writes the status */
	n++;

	head = virtqueue_submit(&b->q, bufs, lens, wr, n);
	if (head == 0xFFFF)
		return BLOCK_ERR_BUSY;

	b->dev.t->notify(&b->dev, 0);

	/* Two seconds. Long enough that a slow emulated disk is not called
	 * broken, short enough that a wedged one does not stop the boot. A
	 * driver with no timeout at all turns one bad device into a machine
	 * that hangs with nothing on the screen. */
	deadline = time_monotonic_ns() + 2000000000ULL;

	for (;;) {
		u16 done;

		if (virtqueue_collect(&b->q, &done, 0)) {
			virtqueue_release(&b->q, done);
			break;
		}

		if (time_monotonic_ns() > deadline) {
			/* The descriptors are deliberately *not* released. The
			 * device still owns them, and handing them back to the
			 * free list would let a later request reuse memory the
			 * disk may still write into. Leaking three descriptors
			 * is the cheap, correct answer. */
			return BLOCK_ERR_TIMEOUT;
		}

		sched_yield();
	}

	switch (*b->status) {
	case VIRTIO_BLK_S_OK:     return BLOCK_OK;
	case VIRTIO_BLK_S_UNSUPP: return BLOCK_ERR_READ_ONLY;
	default:                  return BLOCK_ERR_IO;
	}
}

/* The buffer the caller handed us has to be somewhere the *device* can reach,
 * which means a physical address, which means it has to be in the direct map.
 * A stack address or anything else would translate to a physical page that has
 * nothing to do with the buffer. */
static bool reachable(const void *buf, paddr_t *phys_out)
{
	paddr_t p = virt_to_phys(buf);

	if (!p)
		return false;
	*phys_out = p;
	return true;
}

static enum block_status blk_read(struct block_device *dev, u64 lba, u32 count,
				  void *buf)
{
	struct virtio_blk *b = dev->driver;
	paddr_t phys;

	if (!reachable(buf, &phys))
		return BLOCK_ERR_ALIGN;

	return run(b, VIRTIO_BLK_T_IN, lba, phys, count * VIRTIO_BLK_SECTOR, true);
}

static enum block_status blk_write(struct block_device *dev, u64 lba, u32 count,
				   const void *buf)
{
	struct virtio_blk *b = dev->driver;
	paddr_t phys;

	if (!reachable(buf, &phys))
		return BLOCK_ERR_ALIGN;

	return run(b, VIRTIO_BLK_T_OUT, lba, phys, count * VIRTIO_BLK_SECTOR, false);
}

static enum block_status blk_flush(struct block_device *dev)
{
	struct virtio_blk *b = dev->driver;

	/* A device that did not offer the flush feature must not be sent a
	 * flush command. Success is returned because there is nothing else to
	 * return -- and dev->flush_is_durable is false, which is how a caller
	 * finds out that this success means less than it looks. */
	if (!(b->dev.features & (1ULL << VIRTIO_BLK_F_FLUSH)))
		return BLOCK_OK;

	return run(b, VIRTIO_BLK_T_FLUSH, 0, 0, 0, false);
}

static const struct block_ops virtio_blk_ops = {
	.read  = blk_read,
	.write = blk_write,
	.flush = blk_flush,
};

/* Takes a probed virtio device that turned out to be a disk, and makes it one.
 *
 * Returns false without complaint on a device that is not a block device: the
 * caller probes every slot it can find and most of them are something else or
 * nothing at all. */
bool virtio_blk_attach(const struct virtio_device *probed)
{
	struct virtio_blk *b;
	char name[BLOCK_NAME_MAX];
	u64 capacity = 0;
	u16 qsize;

	if (probed->device_id != 2)
		return false;	/* 2 is "block device"; 1 is network, 4 entropy */

	if (device_count >= VIRTIO_BLK_MAX)
		return false;

	b = &devices[device_count];
	kmemset(b, 0, sizeof(*b));
	b->dev = *probed;

	if (!virtio_begin(&b->dev, (1ULL << VIRTIO_BLK_F_FLUSH)
				 | (1ULL << VIRTIO_BLK_F_RO)))
		return false;

	/* Capacity, in 512-byte sectors, at offset 0 of configuration space.
	 * Read after feature negotiation because what configuration space
	 * *means* depends on what was negotiated. */
	b->dev.t->config_read(&b->dev, 0, &capacity, sizeof(capacity));

	if (!capacity) {
		kputs("virtio-blk: the device reports no capacity\n");
		virtio_give_up(&b->dev);
		return false;
	}

	qsize = b->dev.t->queue_size(&b->dev, 0);
	if (!qsize) {
		kputs("virtio-blk: the device has no request queue\n");
		virtio_give_up(&b->dev);
		return false;
	}

	/* Small on purpose. Every request in flight costs three descriptors,
	 * and nothing here issues more than one at a time; a bigger ring would
	 * be pages of memory reserved for a concurrency this driver does not
	 * have yet. Raise it when there is a reason. */
	if (qsize > 64)
		qsize = 64;

	if (!virtqueue_init(&b->q, qsize)) {
		kputs("virtio-blk: no memory for the request queue\n");
		virtio_give_up(&b->dev);
		return false;
	}

	if (!b->dev.t->setup_queue(&b->dev, 0, &b->q)) {
		kputs("virtio-blk: the device refused the queue\n");
		virtqueue_free(&b->q);
		virtio_give_up(&b->dev);
		return false;
	}

	b->scratch_phys = pmm_alloc_page();
	if (!b->scratch_phys) {
		kputs("virtio-blk: no memory for the request header\n");
		virtqueue_free(&b->q);
		virtio_give_up(&b->dev);
		return false;
	}

	b->header = phys_to_virt(b->scratch_phys);
	b->status = (volatile u8 *)b->header + sizeof(*b->header);

	/* The device may start using the queues the instant this is set, so it
	 * is the last thing that happens. */
	virtio_ready(&b->dev);

	/* "virtioN", numbered in the order they were found. Not a stable name
	 * across boots and not meant to be -- the identity that survives is the
	 * id and generation on struct block_device. */
	kstrlcpy(name, "virtio0", sizeof(name));
	name[6] = (char)('0' + device_count);

	b->bdev = block_register(name, &virtio_blk_ops, b,
				 VIRTIO_BLK_SECTOR, capacity);
	if (!b->bdev) {
		virtqueue_free(&b->q);
		pmm_free_page(b->scratch_phys);
		virtio_give_up(&b->dev);
		return false;
	}

	b->bdev->read_only = (b->dev.features & (1ULL << VIRTIO_BLK_F_RO)) != 0;

	/* Whether a flush here means anything.
	 *
	 * A device that did not offer VIRTIO_BLK_F_FLUSH must not be sent one,
	 * so blk_flush returns success having issued nothing. That used to be
	 * silent, with a comment asserting such a device "has nothing volatile
	 * to flush" -- which the specification does not say and the kernel never
	 * checked. Recorded now, so a filesystem can ask instead of assume. */
	b->bdev->flush_is_durable =
		(b->dev.features & (1ULL << VIRTIO_BLK_F_FLUSH)) != 0;

	/* One request at a time, and a request is three descriptors, so the
	 * useful limit is what one chain can carry rather than what the ring
	 * can hold. Left at zero -- no opinion -- because the block layer does
	 * not split yet and a limit it ignores would be a lie. */
	b->bdev->max_blocks_per_request = 0;

	/* What is underneath a virtio disk is whatever the host put there, and
	 * the device does not say. Left as "assume it seeks", which is the safe
	 * direction: treating an SSD as a disk costs a little allocator effort,
	 * while treating a disk as an SSD fragments it permanently.
	 *
	 * VIRTIO_BLK_F_DISCARD exists and is not negotiated here, so discard is
	 * reported as absent rather than as present-but-unimplemented. */
	b->bdev->seek_is_free      = false;
	b->bdev->discard_supported = false;

	b->in_use = true;
	device_count++;
	return true;
}

unsigned virtio_blk_count(void)
{
	return device_count;
}
