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
#include <recon/kernel/pmm.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/time.h>
#include <recon/kernel/sched.h>
#include <recon/kernel/wait.h>

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

	/* Whether this device was given a message vector, and how many times it
	 * has used it. Volatile because the only writer is an interrupt
	 * handler and every reader is a thread -- without it the compiler is
	 * entitled to hoist the read out of the loop that waits for it, which
	 * is a hang that appears only with optimisation on. */
	bool interrupting;
	volatile unsigned arrivals;

	/* What a waiting thread waits on, and the lock that closes the gap
	 * between deciding to wait and waiting. The handler must take this lock
	 * to wake, which is what makes the wakeup impossible to lose. */
	struct spinlock lock;
	struct wait_queue waiters;
	struct wait_deadline deadline;
};

static struct virtio_blk devices[VIRTIO_BLK_MAX];
static unsigned device_count;

/* How every pause in this driver was actually spent.
 *
 * Without these the change that made this driver wait is unfalsifiable: if no
 * thread could ever block, `pause_for_completion` would fall back to yielding
 * every time, the driver would behave exactly as it did before, and every test
 * would still pass. A count is the difference between a driver that waits and
 * a driver that was *edited* to wait. */
static u64 pauses_slept;
static u64 pauses_yielded;
static u64 pauses_unblockable;
static u64 pauses_timed_out;

/* The device saying it has finished something.
 *
 * Deliberately does not touch the queue. The thread that submitted the request
 * is in run() below with the ring in its hands, and a second party collecting
 * from it would need a lock this driver does not have yet -- so this counts the
 * arrival and returns.
 *
 * That is a smaller thing than it sounds, and it is the right first step.
 * Counting is enough to answer the only question an interrupt path has at this
 * stage: *does it arrive at all*. Every other question -- whether waiting beats
 * polling, whether a completion can be collected from here -- is asked of a
 * mechanism that is known to work, rather than of one that is merely written.
 * The alternative is to build all of it and then debug the whole stack at once
 * with no idea which layer is silent.
 */
static void blk_interrupt(void *arg)
{
	struct virtio_blk *b = arg;
	u64 flags = spin_lock_irq(&b->lock);

	b->arrivals++;

	/* All of them rather than one. Several threads may be waiting on this
	 * device, each for a different request, and this interrupt does not say
	 * whose completion arrived -- it says *a* completion did. Waking one
	 * would be choosing a thread at random and telling it to look, while
	 * the thread whose request actually finished stays asleep.
	 *
	 * Still nothing here touches the queue. Collecting from interrupt
	 * context would mean this handler and every waiting thread contending
	 * for the ring, and the thread has to look anyway when it wakes -- a
	 * woken thread is one that has been told to look again, not one whose
	 * condition is known true. */
	wait_wake_all(&b->waiters);

	spin_unlock_irq(&b->lock, flags);
}

/* Collects one completion, sleeping until the device says there is one.
 *
 * The collect and the sleep are under **one** acquisition of the lock, and
 * that is not tidiness -- it is the whole correctness argument. Written as two
 * functions, with the caller collecting under the lock, releasing it, and this
 * one taking it again to sleep, there is a window between them: a completion
 * landing there runs the handler against a queue nobody is on yet, the wake
 * goes nowhere, and the thread sleeps out its full deadline for a request that
 * had already finished.
 *
 * That is not hypothetical. It is what this driver did on its first run, and
 * the counter below is what caught it -- one sleep in two ending on the clock
 * rather than on the device, on a machine where every request succeeded and
 * every test passed.
 *
 * The two-second limit on a request is *not* enforced here, deliberately. It
 * stays where it has always been, a comparison against the monotonic clock in
 * the caller's loop. This only stops a sleep lasting for ever, so that the
 * caller gets to look at that clock again -- which means the timeout this
 * driver has always had is unchanged, and a device that goes silent still
 * fails after two seconds whether or not it had an interrupt to offer.
 */
static bool collect_or_wait(struct virtio_blk *b, u16 *head)
{
	u64 flags;
	bool collected, armed = false, slept = false, timed_out = false;

	/* Armed before the lock is taken, because arming files a timer whose
	 * callback wants that same lock. */
	if (b->interrupting)
		armed = wait_deadline_arm(&b->deadline, &b->waiters, &b->lock,
					  50000000ull);	/* 50 ms */

	flags = spin_lock_irq(&b->lock);

	collected = virtqueue_collect(&b->q, head, 0);

	if (collected)
		virtqueue_release(&b->q, *head);
	else if (armed && !wait_deadline_passed(&b->deadline))
		slept = wait_sleep(&b->waiters, &b->lock, flags);

	/* Read under the lock, and before the disarm below. Disarming retires
	 * the arming -- which is what makes a late callback harmless, and also
	 * makes this question unanswerable, because there is no longer a
	 * current arming for the answer to be about. Asked afterwards it reads
	 * false for ever, which is how this counter spent its first run:
	 * unable to fire, on a control built to make it fire. */
	if (slept)
		timed_out = wait_deadline_passed(&b->deadline);

	spin_unlock_irq(&b->lock, flags);

	if (armed)
		wait_deadline_disarm(&b->deadline);

	if (collected)
		return true;

	if (slept) {
		pauses_slept++;

		if (timed_out)
			pauses_timed_out++;
	} else {
		/* Nothing to sleep on, or nothing that could sleep -- the idle
		 * thread, or no thread at all, which is the ordinary case early
		 * in boot when the disk is read before there is a scheduler to
		 * hand the processor to. Yielding is what this driver did
		 * before any of this existed, and it still works. */
		if (armed)
			pauses_unblockable++;
		else
			pauses_yielded++;

		sched_yield();
	}

	return false;
}

/* Runs one request to completion.
 *
 * It waits now. The comment here used to read "polls rather than waits, because
 * the scheduler has no way to block yet ... when there is a wait queue this
 * function is where it goes, and nothing above it changes" -- and that turned
 * out to be exactly right about *where*, and wrong about *nothing*: the loop
 * below had to collect and sleep under one acquisition of the lock, which the
 * polling version had no reason to care about.
 *
 * The two-second limit is still enforced here against the monotonic clock, and
 * deliberately not by the deadline the sleep uses. A device that goes silent
 * fails after two seconds whether or not it had an interrupt to offer. */
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

		if (collect_or_wait(b, &done))
			break;

		if (time_monotonic_ns() > deadline) {
			/* The descriptors are deliberately *not* released. The
			 * device still owns them, and handing them back to the
			 * free list would let a later request reuse memory the
			 * disk may still write into. Leaking three descriptors
			 * is the cheap, correct answer. */
			return BLOCK_ERR_TIMEOUT;
		}
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
	spin_init(&b->lock, "virtio-blk");

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

	/* Ask for completions to be signalled rather than looked for.
	 *
	 * Before DRIVER_OK, because after it the device may already be using
	 * the queue -- and a device that completes a request while its vector
	 * is half-configured raises an interrupt into a table entry nothing
	 * owns.
	 *
	 * False is not a failure and is not reported as one. It means this
	 * machine, this transport or this device has no way to signal, and the
	 * polling below is what happens then -- which is what happened before
	 * this call existed. */
	if (b->dev.t->request_interrupt)
		b->interrupting = b->dev.t->request_interrupt(
			&b->dev, 0, blk_interrupt, b, "virtio-blk");

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

void virtio_blk_print_summary(void)
{
	unsigned i, interrupting = 0;

	if (!device_count)
		return;

	for (i = 0; i < device_count; i++)
		if (devices[i].interrupting)
			interrupting++;

	kprintf("  virtio-blk   : %u disk(s), %u told when a request "
		"finishes\n", device_count, interrupting);

	/* Said plainly, including the case where it never happened. A driver
	 * that waits on paper and yields in practice reads identically in every
	 * other line of this boot. */
	kprintf("  pauses       : %llu slept, %llu yielded",
		(unsigned long long)pauses_slept,
		(unsigned long long)pauses_yielded);

	if (pauses_timed_out)
		kprintf(", %llu of the sleeps ending on the clock rather than "
			"on the device",
			(unsigned long long)pauses_timed_out);

	if (pauses_unblockable)
		kprintf(", %llu with no thread that could block",
			(unsigned long long)pauses_unblockable);

	kprintf("\n");

	if (pauses_slept && pauses_timed_out == pauses_slept)
		kputs("  and every one of those woke on its own deadline, so "
		      "nothing was ever woken by the device\n");

	if (interrupting && !pauses_slept)
		kputs("  and not one of them was waited for, so every "
		      "completion beat the thread to it\n");
}

/* --- that the interrupt actually arrives ----------------------------------
 *
 * The whole of the difficulty with an interrupt path is that a silent one looks
 * exactly like a working one as long as something else is also polling. This
 * driver still polls, on purpose -- so if the vector were misconfigured, the
 * table entry written to the wrong offset, or the device never told which
 * message to use, every disk operation in this kernel would keep working and
 * nothing would say a word.
 *
 * So the test does a real read and asserts the counter moved. Not "an interrupt
 * can be delivered", which msi_self_test already shows by writing one by hand:
 * that *this device*, told about *this vector*, raises it when it finishes work
 * it was actually given.
 */
bool virtio_blk_self_test(void)
{
	unsigned i;
	bool found = false;

	for (i = 0; i < device_count; i++) {
		struct virtio_blk *b = &devices[i];
		paddr_t scratch;
		unsigned before;
		enum block_status st;

		if (!b->in_use || !b->interrupting)
			continue;

		found = true;

		/* A page, because the request needs a physical address and the
		 * device writes a whole sector into it. */
		scratch = pmm_alloc_page();

		if (!scratch) {
			kputs("  virtio-blk: no page to read a sector "
			      "into\n");
			return false;
		}

		before = b->arrivals;

		st = run(b, VIRTIO_BLK_T_IN, 0, scratch, VIRTIO_BLK_SECTOR,
			 true);

		pmm_free_page(scratch);

		if (st != BLOCK_OK) {
			kprintf("  virtio-blk: reading sector 0 of virtio%u "
				"failed (%d)\n", i, (int)st);
			return false;
		}

		/* The read completed, so the device certainly finished the
		 * request. If the counter did not move, the completion was
		 * seen by the polling loop and the interrupt was not raised --
		 * which is the failure this test exists for, and the one that
		 * is otherwise invisible. */
		if (b->arrivals == before) {
			kprintf("  virtio-blk: virtio%u was given a message "
				"vector and finished a request without "
				"raising it\n", i);
			return false;
		}
	}

	if (!found) {
		/* Nothing asked for an interrupt: no PCI, no MSI-X, or no disk.
		 * Said out loud rather than passing quietly, because a test
		 * that reports success without having run is the thing this
		 * project has been bitten by and does not do. */
		kputs("  virtio-blk: no device here signals by message, so "
		      "there was nothing to check\n");
	}

	return true;
}
