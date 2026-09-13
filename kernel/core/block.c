/* The block layer: a registry, a range check, and nothing else.
 *
 * Everything interesting about storage is either below this file, in a driver
 * that knows one piece of hardware, or above it, in a data format that knows
 * one on-disk layout. What is left in the middle is small on purpose. A block
 * layer that grew a cache, a scheduler and a partition parser would be three
 * things wearing one name, and each of them would be harder to test than it
 * needs to be.
 *
 * What it does keep is the checks that every driver would otherwise write
 * again and get subtly differently: does this request run off the end, does the
 * length wrap, is the device read-only. Those belong once, here, because a
 * driver that gets the overflow check wrong writes to the wrong sector and says
 * it succeeded.
 */
#include <recon/kernel/block.h>
#include <recon/kernel/wait.h>
#include <recon/kernel/identity.h>
#include <recon/kernel/bcache.h>

#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/partition.h>
#include <recon/kernel/sched.h>

static struct block_device devices[BLOCK_MAX_DEVICES];
static unsigned device_count;

/* Never reused, so an identity that outlives its device fails to resolve rather
 * than resolving to somebody else's disk. */
static u32 next_id = 1;

static u64 reads, writes, flushes, blocks_read, blocks_written;

const char *block_status_name(enum block_status s)
{
	switch (s) {
	case BLOCK_OK:              return "ok";
	case BLOCK_ERR_NO_DEVICE:   return "no such device";
	case BLOCK_ERR_RANGE:       return "past the end of the device";
	case BLOCK_ERR_READ_ONLY:   return "the device is read-only";
	case BLOCK_ERR_ALIGN:       return "the buffer is not reachable by the hardware";
	case BLOCK_ERR_IO:          return "the hardware reported a failure";
	case BLOCK_ERR_TIMEOUT:     return "the hardware did not answer";
	case BLOCK_ERR_BUSY:        return "the queue is full, or the disk is not claimed";
	case BLOCK_ERR_UNSUPPORTED: return "the device does not offer that operation";
	default:                    return "unrecognised status";
	}
}

const char *block_scheme_name(enum block_scheme s)
{
	switch (s) {
	case BLOCK_SCHEME_NONE:       return "no partition table";
	case BLOCK_SCHEME_GPT:        return "gpt";
	case BLOCK_SCHEME_MBR:        return "mbr";
	case BLOCK_SCHEME_UNREADABLE: return "a table that could not be believed";
	default:                      return "unrecognised";
	}
}

struct block_device *block_register(const char *name, const struct block_ops *ops,
				    void *driver, u32 block_size, u64 block_count)
{
	struct block_device *d;

	if (device_count >= BLOCK_MAX_DEVICES) {
		kprintf("block: no room for %s; %u devices is the limit\n",
			name, (unsigned)BLOCK_MAX_DEVICES);
		return 0;
	}

	/* A block size that is not a power of two turns every division in every
	 * caller into something slow and every mask into something wrong.
	 * Refused here rather than worked around there. */
	if (!block_size || (block_size & (block_size - 1))) {
		kprintf("block: %s reports a block size of %u, which is not a "
			"power of two\n", name, block_size);
		return 0;
	}

	d = &devices[device_count++];
	kmemset(d, 0, sizeof(*d));

	kstrlcpy(d->name, name, sizeof(d->name));
	d->id          = next_id++;
	d->generation  = 1;
	d->block_size  = block_size;
	d->block_count = block_count;
	d->ops         = ops;
	d->driver      = driver;
	d->present     = true;

	return d;
}

void block_unregister(struct block_device *dev)
{
	unsigned i;

	if (!dev || !dev->present)
		return;

	/* Partitions first. A slice whose parent has gone is not a smaller disk
	 * that is still there, and leaving one present would leave a filesystem
	 * holding a device that answers reads from a disk nobody can reach. */
	for (i = 0; i < device_count; i++) {
		struct block_device *slice = &devices[i];

		if (slice == dev || !slice->present)
			continue;

		if (slice->parent == dev->id &&
		    slice->parent_generation == dev->generation) {
			bcache_invalidate_device(slice);
			slice->present = false;
			slice->generation++;
		}
	}

	bcache_invalidate_device(dev);

	dev->present = false;
	dev->slice_count = 0;

	/* The identity is retired rather than reused. Anything still holding
	 * this device by (id, generation) now fails to find it, instead of
	 * finding whatever is plugged in next. */
	dev->generation++;

	kprintf("block: %s is gone\n", dev->name);
}

struct block_device *block_register_slice(struct block_device *parent,
					  u8 index, u64 first_lba, u64 count,
					  enum block_scheme scheme)
{
	struct block_device *d;
	char name[BLOCK_NAME_MAX];
	size_t n;

	if (!parent || !count)
		return 0;

	if (parent->slice_count >= BLOCK_MAX_SLICES) {
		kprintf("block: %s has more than %u partitions; the rest are "
			"not registered, so nothing will protect them\n",
			parent->name, (unsigned)BLOCK_MAX_SLICES);
		return 0;
	}

	/* Inside the parent, checked the same way a request is: the sum is
	 * never formed, because a table on a hostile or damaged disk can name a
	 * partition whose start plus length wraps. */
	if (first_lba >= parent->block_count ||
	    count > parent->block_count - first_lba) {
		kprintf("block: %s partition %u runs past the end of the "
			"device; ignored\n", parent->name, index);
		return 0;
	}

	/* "nvme0n1" + "p1". The convention the drivers already set by naming
	 * whole devices after what they are. */
	n = kstrlcpy(name, parent->name, sizeof(name));
	if (n + 3 < sizeof(name)) {
		name[n] = 'p';
		name[n + 1] = (char)('0' + (index % 10));
		name[n + 2] = '\0';
		if (index >= 10) {
			name[n + 1] = (char)('0' + (index / 10));
			name[n + 2] = (char)('0' + (index % 10));
			name[n + 3] = '\0';
		}
	}

	d = block_register(name, parent->ops, parent->driver,
			   parent->block_size, count);
	if (!d)
		return 0;

	d->parent            = parent->id;
	d->parent_generation = parent->generation;
	d->first_lba         = first_lba;
	d->slice_index       = index;
	d->scheme            = (u8)scheme;
	d->removable         = parent->removable;
	d->read_only         = parent->read_only;
	d->max_blocks_per_request = parent->max_blocks_per_request;

	parent->slice_count++;
	return d;
}

/* How many devices are **there**, which is not how many slots are in use.
 *
 * A retired device keeps its slot so that its identity can go on being refused,
 * and every loop in this kernel that walks the table was written before a slot
 * could be occupied and absent at once. Rather than teach seventeen callers a
 * new rule, enumeration simply cannot produce one. */
unsigned block_device_count(void)
{
	unsigned i, n = 0;

	for (i = 0; i < device_count; i++)
		if (devices[i].present)
			n++;

	return n;
}

struct block_device *block_device_at(unsigned index)
{
	unsigned i;

	/* The index counts devices that are there, not slots. A caller looping
	 * to `block_device_count()` therefore sees every live device exactly
	 * once and no retired one at all -- which is the property that lets the
	 * loops written before retirement existed go on being correct. */
	for (i = 0; i < device_count; i++) {
		if (!devices[i].present)
			continue;

		if (!index)
			return &devices[i];

		index--;
	}

	return 0;
}

struct block_device *block_device_by_id(u32 id, u32 generation)
{
	for (unsigned i = 0; i < device_count; i++)
		if (devices[i].id == id && devices[i].generation == generation &&
		    devices[i].present)
			return &devices[i];
	return 0;
}

/* The check every driver would otherwise write for itself.
 *
 * The overflow half is the half that matters. `lba + count > block_count` is
 * the obvious form and it is wrong: a caller that passes an lba near the top
 * and a large count produces a sum that wraps below block_count, the check
 * passes, and the driver writes wherever the truncated address landed. */
static enum block_status check_range(const struct block_device *dev, u64 lba, u32 count)
{
	if (!dev || !dev->present)
		return BLOCK_ERR_NO_DEVICE;

	if (count == 0)
		return BLOCK_OK;

	if (lba >= dev->block_count)
		return BLOCK_ERR_RANGE;

	if ((u64)count > dev->block_count - lba)
		return BLOCK_ERR_RANGE;

	return BLOCK_OK;
}

/* --- One request at a time, per device --------------------------------------
 *
 * There was no locking anywhere in the storage path, and the reason it worked
 * is that exactly one thread ever called it. Nothing enforced that and nothing
 * said it.
 *
 * Every driver holds one command slot and one scratch buffer for the request it
 * is running, and every driver polls for completion with sched_yield(). The
 * scheduler is preemptive and runs on every processor. So the day a second
 * thread calls block_read -- which is the day a filesystem exists -- two
 * requests share one command slot, and the failure is silent: two reads return
 * each other's data.
 *
 * That is the same shape as several bugs already in this project's register:
 * correct until a second caller arrives, and armed by a change somewhere else.
 * A filesystem is that second caller, so this is fixed before it is written
 * rather than after.
 *
 * It is NOT a spinlock. A spinlock cannot be held across sched_yield(), and the
 * driver being protected yields on every poll -- so a spinlock here deadlocks
 * on one processor and wastes a core on several. What this needs is a lock a
 * waiter can sleep on, and the kernel has no sleeping primitive yet, so a
 * waiter yields. When there is a wait queue this loop is what changes; the
 * shape of the lock is not.
 */
static void device_acquire(struct block_device *dev)
{
	while (__atomic_test_and_set(&dev->busy, __ATOMIC_ACQUIRE))
		sched_yield();
}

static void device_release(struct block_device *dev)
{
	__atomic_clear(&dev->busy, __ATOMIC_RELEASE);
}

/* --- the scheduler ---------------------------------------------------------
 *
 * Requests used to fight over `busy`: whoever won the race went first, and the
 * head moved wherever the winner happened to want. That is not an ordering, it
 * is the absence of one, and on anything with a seek it is the difference
 * between a sweep and a scramble.
 *
 * --- No worker thread, and that is the design rather than a shortcut ---
 *
 * A thread that wants a disk is already going to wait for one, so it is the
 * right thread to drive it. Whoever arrives at an idle device becomes the
 * **servicer**: it takes requests off the queue in order and issues them,
 * including ones that arrive while it is working, and wakes each owner as it
 * finishes. Everybody else inserts and sleeps.
 *
 * That matters most where it is least visible. With one caller -- which is
 * every read during early boot, before there is a scheduler to sleep on -- the
 * caller finds the device idle, services its own request, and the whole
 * mechanism costs one list insertion. There is no thread to have started and
 * nothing to fall back to.
 *
 * --- What the order is ---
 *
 * Sorted by block, one way. A disk's head is a physical thing and the cost of
 * a request is mostly the distance from the last one, so a sweep beats
 * arrival order. On a device that says `seek_is_free` the sort is skipped:
 * ordering costs a little and buys nothing, and pretending otherwise would be
 * a scheduler that is sure it is helping.
 */
static void queue_insert(struct block_device *dev, struct block_request *r)
{
	struct block_request **at = &dev->queue;

	if (!dev->seek_is_free)
		while (*at && (*at)->lba <= r->lba)
			at = &(*at)->next;
	else
		while (*at)
			at = &(*at)->next;

	if (*at)
		dev->reordered++;

	r->next = *at;
	*at = r;
	dev->queued++;
}

/* Issues one request, splitting it if the device has an opinion about how much
 * it will take at once. */
static enum block_status issue(struct block_device *dev,
			       struct block_request *r)
{
	enum block_status s = BLOCK_OK;
	u64 lba = r->lba;
	u32 count = r->count;
	u8 *buf = r->buf;

	/* Still taken, even though the queue already admits one servicer at a
	 * time. `busy` is not this function's lock -- block_flush takes it too,
	 * and a flush that overlaps a transfer is asking the driver to commit
	 * something it is in the middle of writing. Held across the split
	 * rather than per chunk, because a half-issued request is not a state
	 * anybody else should be allowed to see. */
	device_acquire(dev);

	while (count) {
		u32 chunk = count;

		if (dev->max_blocks_per_request &&
		    chunk > dev->max_blocks_per_request)
			chunk = dev->max_blocks_per_request;

		s = r->write ? dev->ops->write(dev, lba, chunk, buf)
			     : dev->ops->read(dev, lba, chunk, buf);

		if (s != BLOCK_OK) {
			device_release(dev);
			return s;
		}

		/* Counted per chunk rather than per request, which is what the
		 * old bodies did and is the more honest number: a request the
		 * device made the kernel deliver in four pieces cost four
		 * transfers, and a statistic that says one is hiding the very
		 * thing max_blocks_per_request exists to describe. */
		if (r->write) {
			writes++;
			blocks_written += chunk;
		} else {
			reads++;
			blocks_read += chunk;
		}

		lba += chunk;
		buf += (u64)chunk * dev->block_size;
		count -= chunk;
	}

	device_release(dev);
	return s;
}

/* Queues one request and does not return until it is done.
 *
 * The request is on the caller's stack and must stay there until `done` is
 * set, which is why nothing here returns early on a failed wait.
 */
static enum block_status submit(struct block_device *dev,
				struct block_request *r)
{
	u64 flags;
	bool mine;

	r->next = NULL;
	r->done = false;
	r->status = BLOCK_OK;

	flags = spin_lock_irq(&dev->queue_lock);
	queue_insert(dev, r);
	/* Nobody is driving the device, so this thread is. */
	mine = !dev->serving;

	if (mine)
		dev->serving = true;

	spin_unlock_irq(&dev->queue_lock, flags);

	if (!mine) {
		/* Somebody else is working through the queue and will get to
		 * this one. Waited for under the same lock the servicer wakes
		 * under, so a completion between the test and the sleep cannot
		 * be lost. */
		flags = spin_lock_irq(&dev->queue_lock);

		while (!r->done)
			if (!wait_sleep(&dev->waiters, &dev->queue_lock, flags))
				break;		/* nothing here can sleep */

		spin_unlock_irq(&dev->queue_lock, flags);

		/* Could not sleep -- early boot, or an idle thread. Spinning
		 * is all that is left, and the servicer is another thread that
		 * is running. */
		while (!r->done)
			sched_yield();

		return r->status;
	}

	for (;;) {
		struct block_request *next;
		enum block_status s;

		flags = spin_lock_irq(&dev->queue_lock);
		next = dev->queue;

		if (!next) {
			/* Nothing left. Stop serving *under the lock*, so a
			 * request arriving at this instant either gets on the
			 * queue before this and is taken, or finds the device
			 * idle and drives itself. There is no gap where a
			 * request is queued and nobody is coming. */
			dev->serving = false;
			spin_unlock_irq(&dev->queue_lock, flags);
			break;
		}

		dev->queue = next->next;
		spin_unlock_irq(&dev->queue_lock, flags);

		s = issue(dev, next);
		flags = spin_lock_irq(&dev->queue_lock);
		next->status = s;
		next->done = true;
		wait_wake_all(&dev->waiters);
		spin_unlock_irq(&dev->queue_lock, flags);
	}

	return r->status;
}

/* Translates a request on a slice into one on the disk underneath it.
 *
 * Walked to the root rather than assuming one level, so that a volume nested
 * inside a partition -- an encrypted one, eventually -- costs nothing here.
 *
 * Called only AFTER check_range has run against the device the caller named.
 * That order is the whole safety property: the bound is checked against the
 * slice's own length, in the caller's own coordinates, before the address is
 * translated into somebody else's. Translating first and checking after would
 * check the wrong number. */
static struct block_device *to_root(struct block_device *dev, u64 *lba)
{
	unsigned guard = 0;

	while (dev->parent && guard++ < BLOCK_MAX_DEVICES) {
		struct block_device *p =
			block_device_by_id(dev->parent, dev->parent_generation);

		if (!p)
			return 0;	/* the disk went away underneath us */

		*lba += dev->first_lba;
		dev = p;
	}

	return dev;
}

/* The identity a caller's request actually names: which disk, and which
 * block of it. See bcache.h -- a cache keyed on anything else holds two
 * entries for one sector whenever a partition is involved.
 *
 * The bound is checked first, against the device the caller named, in that
 * device's own coordinates. That is the same order block_read uses and the
 * order is the safety property: a slice's length is the limit, and checking
 * after translation would check the whole disk's. */
enum block_status block_resolve(struct block_device *dev, u64 lba, u32 count,
				u32 *root_id, u32 *root_generation,
				u64 *abs_lba)
{
	enum block_status s;
	struct block_device *root;

	if (!dev || !root_id || !root_generation || !abs_lba)
		return BLOCK_ERR_NO_DEVICE;

	s = check_range(dev, lba, count);
	if (s != BLOCK_OK)
		return s;

	root = to_root(dev, &lba);
	if (!root)
		return BLOCK_ERR_NO_DEVICE;

	*root_id = root->id;
	*root_generation = root->generation;
	*abs_lba = lba;
	return BLOCK_OK;
}

enum block_status block_read(struct block_device *dev, u64 lba, u32 count, void *buf)
{
	enum block_status s = check_range(dev, lba, count);
	struct block_request r;

	if (s != BLOCK_OK || count == 0)
		return s;

	if (!buf)
		return BLOCK_ERR_ALIGN;

	dev = to_root(dev, &lba);
	if (!dev)
		return BLOCK_ERR_NO_DEVICE;

	/* Queued rather than issued. The splitting the old body did here has
	 * moved into `issue`, which is where the request finally reaches the
	 * driver -- the arithmetic is the same and there is now one place that
	 * does it for reads and writes both. */
	r.lba = lba;
	r.count = count;
	r.buf = buf;
	r.write = false;

	return submit(dev, &r);
}

enum block_status block_write(struct block_device *dev, u64 lba, u32 count,
			      const void *buf)
{
	enum block_status s = check_range(dev, lba, count);
	struct block_request r;

	if (s != BLOCK_OK || count == 0)
		return s;

	if (!buf)
		return BLOCK_ERR_ALIGN;

	/* Answered here rather than by the hardware. A device that refuses a
	 * write may or may not say why, and "read-only" is a fact the kernel
	 * already knows -- so it is a clear error rather than a mystery. */
	if (dev->read_only)
		return BLOCK_ERR_READ_ONLY;

	if (!dev->ops->write)
		return BLOCK_ERR_READ_ONLY;

	/* A write to a whole disk that has partitions on it, by somebody who
	 * has not said they mean to rewrite the disk. Refused.
	 *
	 * The partitions are right there in the registry, addressable by name,
	 * with their own bounds. Writing through the parent instead is either a
	 * partitioner -- which claims the device first, once, deliberately --
	 * or a mistake that lands in the middle of somebody's filesystem and
	 * reports success. */
	if (dev->slice_count && !dev->claimed_raw)
		return BLOCK_ERR_BUSY;

	/* Anything cached for these blocks stops being true here.
	 *
	 * Done from inside the block layer rather than left to callers,
	 * because the callers on this path are the installer and the
	 * partition writer -- code that uses the raw interface precisely
	 * because it is not filesystem traffic, and that should not have to
	 * know a cache exists for the cache to be correct.
	 *
	 * Before the transfer, not after: a write that fails halfway leaves
	 * the disk holding neither the old contents nor the new, and a cache
	 * that guessed either would be confidently wrong.
	 *
	 * In the caller's own coordinates, and before to_root, because that is
	 * how the cache is keyed -- the partition-and-disk aliasing the cache
	 * already handles is handled by telling it what the caller said, not
	 * what the caller meant. Dropping this line is what a queue rewrite did
	 * without noticing, and the only thing that noticed was a test. */
	bcache_invalidate(dev, lba, count);

	dev = to_root(dev, &lba);
	if (!dev)
		return BLOCK_ERR_NO_DEVICE;

	/* The cast is the one place const is given up, and it is given up
	 * because a request carries one pointer for both directions. `issue`
	 * hands it to the driver's write, which takes a const pointer again --
	 * so nothing between here and the device writes through it. */
	r.lba = lba;
	r.count = count;
	r.buf = (void *)(uintptr_t)buf;
	r.write = true;

	return submit(dev, &r);
}

/* Tells the device a range of blocks holds nothing anyone wants.
 *
 * --- Why this is here at all ---
 *
 * A solid-state drive cannot overwrite a block in place: it erases a much
 * larger region and rewrites it. To do that it has to preserve everything else
 * in that region -- including blocks whose contents no one will ever read
 * again, because nothing ever told it so. A drive that is never told about
 * freed space gradually behaves as though it is full even when the filesystem
 * says it is half empty, and its write speed falls with it.
 *
 * Copy-on-write makes this sharper than it is for most filesystems: every write
 * frees the block it replaced, so a ReconFS volume generates freed space
 * continuously rather than only when files are deleted.
 *
 * --- Why an unsupported discard is not success ---
 *
 * A device with no discard operation returns BLOCK_ERR_UNSUPPORTED, not
 * BLOCK_OK. Returning success would tell a caller the drive now knows about the
 * freed space when it does not -- which is the same shape as the virtio-blk
 * flush bug this kernel already had once, and the reason `flush_is_durable`
 * exists. Once is a mistake; twice is a pattern nobody looked for.
 *
 * It is still not an error for the *caller*: discard is advisory, and a
 * filesystem that refuses to free a block because the drive would not listen
 * has misunderstood which of them is in charge.
 */
enum block_status block_discard(struct block_device *dev, u64 lba, u32 count)
{
	if (!dev || !dev->present)
		return BLOCK_ERR_NO_DEVICE;

	if (dev->read_only)
		return BLOCK_ERR_READ_ONLY;

	if (!count)
		return BLOCK_OK;

	/* The same range check as a write, and for the same reason: a discard
	 * that runs off the end of a slice would be telling the device to
	 * forget somebody else's data. */
	if (lba >= dev->block_count)
		return BLOCK_ERR_RANGE;
	if ((u64)count > dev->block_count - lba)
		return BLOCK_ERR_RANGE;

	/* A discarded block may read back as zeroes or as what it held,
	 * and the specification permits both -- so whatever is cached for
	 * it is no longer known to be anything. */
	bcache_invalidate(dev, lba, count);

	{
		struct block_device *root = to_root(dev, &lba);

		if (!root->ops->discard)
			return BLOCK_ERR_UNSUPPORTED;

		return root->ops->discard(root, lba, count);
	}
}

enum block_status block_flush(struct block_device *dev)
{
	if (!dev || !dev->present)
		return BLOCK_ERR_NO_DEVICE;

	/* A device with no flush operation. Success is reported, because there
	 * is nothing better to return and an error would make every caller
	 * invent a policy. What the caller can do instead is ask
	 * block_flush_is_durable() and decline to promise what this device
	 * cannot deliver. */
	if (!dev->ops->flush)
		return BLOCK_OK;

	/* A flush is device-wide: there is no range-scoped or file-scoped flush
	 * on this block layer and there cannot be one, so flushing through a
	 * partition commits everything pending on the whole controller. Anything
	 * above must state its durability guarantee in those terms rather than
	 * promising that one file is safe and another is not. */
	{
		u64 ignored = 0;
		struct block_device *root = to_root(dev, &ignored);
		enum block_status s;

		if (!root)
			return BLOCK_ERR_NO_DEVICE;

		device_acquire(root);
		flushes++;
		s = root->ops->flush(root);
		device_release(root);
		return s;
	}
}

bool block_flush_is_durable(const struct block_device *dev)
{
	if (!dev || !dev->present)
		return false;

	/* Answered by the disk underneath, because that is where the cache is
	 * and where the flush goes. A slice inherits the answer rather than
	 * having one of its own. */
	while (dev->parent) {
		const struct block_device *p =
			block_device_by_id(dev->parent, dev->parent_generation);

		if (!p)
			return false;
		dev = p;
	}

	return dev->flush_is_durable;
}

/* --- Claiming a whole disk -------------------------------------------------
 *
 * Nothing here is clever. What it buys is that the question gets asked at a
 * moment somebody chose, rather than being the permanent ambient state of every
 * device in the machine.
 */
enum block_status block_claim_raw(struct block_device *dev)
{
	if (!dev || !dev->present)
		return BLOCK_ERR_NO_DEVICE;

	/* **Who is declaring it.**
	 *
	 * This function's own comment calls a claim a declaration of intent to
	 * destroy a disk, asked once, out loud, at the top of an install. Until
	 * capabilities existed there was nothing to ask -- every caller was the
	 * kernel and the question had one answer.
	 *
	 * Now an installer can hold this power while it writes a partition
	 * table and drop it the moment it is done, and every bug in everything
	 * it does afterwards is a bug that cannot reach a disk. */
	if (!capable(CAP_RAW_DISK))
		return BLOCK_ERR_READ_ONLY;

	/* A slice is already a bounded view; claiming one would mean nothing,
	 * and allowing it would let a caller believe it had claimed the disk. */
	if (dev->parent)
		return BLOCK_ERR_RANGE;

	if (dev->read_only)
		return BLOCK_ERR_READ_ONLY;

	if (dev->claimed_raw)
		return BLOCK_ERR_BUSY;

	/* The caller has just said it intends to rewrite this whole disk.
	 * Every block cached from it is a statement about a disk that is
	 * about to stop existing. */
	bcache_invalidate_device(dev);

	dev->claimed_raw = true;
	return BLOCK_OK;
}

void block_release_raw(struct block_device *dev)
{
	if (dev)
		dev->claimed_raw = false;
}

/* --- Checking a layout before any of it is written -------------------------
 *
 * The per-request range check cannot catch a plan that is internally
 * inconsistent. Four writes that each land inside the disk can still describe
 * two partitions that overlap, and every one of those writes succeeds, and the
 * disk they overlap on has somebody's installation on it.
 *
 * So the whole plan is checked as a plan, once, by something that can see the
 * device. It writes nothing and it composes nothing: what the layout should be
 * is the caller's decision, and this is the arithmetic being checked.
 */
enum block_status block_check_layout(const struct block_device *dev,
				     const struct block_extent *plan, unsigned n)
{
	if (!dev || !dev->present)
		return BLOCK_ERR_NO_DEVICE;

	if (dev->parent)
		return BLOCK_ERR_RANGE;	/* a layout goes on a disk, not a slice */

	if (!plan && n)
		return BLOCK_ERR_ALIGN;

	for (unsigned i = 0; i < n; i++) {
		u64 a_first = plan[i].first_lba;
		u64 a_count = plan[i].count;

		if (!a_count)
			return BLOCK_ERR_RANGE;

		/* Inside the device, and never by forming the sum -- a plan
		 * that came from arithmetic above the kernel is exactly the
		 * thing that can hand down a length which wraps. */
		if (a_first >= dev->block_count ||
		    a_count > dev->block_count - a_first)
			return BLOCK_ERR_RANGE;

		for (unsigned j = i + 1; j < n; j++) {
			u64 b_first = plan[j].first_lba;
			u64 b_count = plan[j].count;

			/* Two half-open ranges overlap unless one ends at or
			 * before the other begins. Written as two comparisons
			 * on the ends rather than as a sum, for the same
			 * reason. */
			if (!(a_first >= b_first && a_first - b_first >= b_count) &&
			    !(b_first >= a_first && b_first - a_first >= a_count))
				return BLOCK_ERR_RANGE;
		}
	}

	/* What is deliberately not checked yet: whether an extent lands on a
	 * volume the kernel has mounted, or on the one it booted from. Both
	 * need a filesystem to exist before they mean anything, and both are
	 * part of checkpoint 13. Named here so that the gap is a known one
	 * rather than an assumption -- a caller reading this today gets
	 * geometry checked and nothing else, and should be told so. */

	return BLOCK_OK;
}

/* --- What the partition reader found ---------------------------------------
 *
 * Printed in its own shape, separately from the human summary, because the
 * fixture harness compares this against what sgdisk and sfdisk say is on the
 * same disk. A format that has to be both readable and parseable ends up being
 * neither, and the one that gets quietly reformatted is the one under test.
 *
 * Geometry only: which slice, where it starts, where it ends. Not names, not
 * type codes. Those are what the table *means*, and meaning is the caller's.
 */
void block_print_tables(void)
{
	for (unsigned i = 0; i < device_count; i++) {
		const struct block_device *d = &devices[i];

		if (d->parent)
			continue;	/* slices are listed under their disk */

		kprintf("table %s %s %u\n", d->name,
			block_scheme_name((enum block_scheme)d->scheme),
			d->slice_count);

		for (unsigned j = 0; j < device_count; j++) {
			const struct block_device *sl = &devices[j];

			if (sl->parent != d->id)
				continue;

			kprintf("slice %s %u %lu %lu\n", d->name,
				sl->slice_index, sl->first_lba,
				sl->first_lba + sl->block_count - 1);
		}
	}
}

void block_init(void)
{
	device_count = 0;
	reads = writes = flushes = blocks_read = blocks_written = 0;

	/* The architecture goes looking. What it finds -- a PCI bus, a handful
	 * of memory-mapped virtio slots, nothing at all -- is its business, and
	 * every device it finds arrives back here through block_register(). */
	arch_storage_probe();

	/* Then read what is written on each of them. Iterated over a snapshot of
	 * the count taken first, because reading a table registers slices and
	 * would otherwise walk into the partitions it is creating and try to
	 * find partitions inside those. */
	{
		unsigned whole = device_count;

		for (unsigned i = 0; i < whole; i++)
			partition_scan(&devices[i]);
	}
}

static void print_size(u64 bytes)
{
	static const char *const unit[] = { "B", "KB", "MB", "GB", "TB" };
	unsigned u = 0;
	u64 whole = bytes, frac = 0;

	while (whole >= 1024 && u < 4) {
		frac = ((whole % 1024) * 10) / 1024;
		whole /= 1024;
		u++;
	}

	kprintf("%lu.%lu %s", whole, frac, unit[u]);
}

void block_print_summary(void)
{
	kprintf("\nStorage\n");

	arch_storage_print();

	if (!device_count) {
		kputs("  devices      : none found\n");
		return;
	}

	for (unsigned i = 0; i < device_count; i++) {
		const struct block_device *d = &devices[i];

		/* Padded by hand: kprintf has no field widths, and adding them
		 * for one caller is a worse trade than four lines here. */
		kprintf("  %s", d->name);
		for (size_t pad = kstrlen(d->name); pad < 12; pad++)
			kputc(' ');
		kputs(" : ");
		print_size(d->block_count * (u64)d->block_size);
		kprintf(", %lu blocks of %u bytes%s%s%s\n",
			d->block_count, d->block_size,
			d->read_only ? ", read-only" : "",
			d->removable ? ", removable" : "",
			(!d->parent && !d->flush_is_durable)
				? ", FLUSH DOES NOT REACH THE MEDIUM" : "");
	}
}

/* --- The self-test ---------------------------------------------------------
 *
 * Writes a pattern, reads it back, and checks it. Which sounds trivial and is
 * not: this is the first code in this kernel whose failure mode is *somebody
 * else's data*, so the test is written to catch the failures that matter rather
 * than the ones that are easy.
 *
 * Four choices in it are deliberate.
 *
 * It works on the *last* blocks of whatever it is given. A driver with an
 * off-by-one in its range arithmetic fails there and nowhere else, and a test
 * that writes to block zero tests the one address every bug agrees on.
 *
 * It transfers sixteen kilobytes, not one block. A one-block request fits
 * inside a single page, and a single page is the case every scatter scheme
 * gets right: NVMe describes a transfer as a list of pages, with one page in
 * the first pointer, two in the second, and three or more in a separate list --
 * so a test that never exceeds one page never reaches the code where a driver
 * goes wrong.
 *
 * It puts back every byte it borrowed. This is somebody's disk.
 *
 * And it chooses what to write to, which it did not used to.
 *
 * --- Why choosing matters, and how this test was caught by the kernel ---
 *
 * It used to write to block_device_at(0) unconditionally. The moment partition
 * reading landed, device zero became a *partitioned disk*, and the last sixteen
 * kilobytes of a GPT disk are its backup header and entry array. The test was
 * about to destroy the table it had just read, on every run.
 *
 * It did not, because the rule added in the same checkpoint refused the write:
 * a disk with partitions on it will not take one until somebody claims it. So
 * the first thing that safety check ever caught was this file. That is worth
 * recording rather than quietly fixing -- a check whose first catch is the test
 * suite is a check that was needed.
 *
 * The choice below is not an off-switch for that rule, which this project does
 * not allow. It is the test finding a surface it is entitled to write to: a
 * disk with no table on it. What it can do depends on what the disk is, the
 * same way the read-only case already worked.
 *
 * --- And it used to accept a partition, which was worse (KF-140) ---
 *
 * The second preference used to be "failing that, a single partition -- bounded,
 * and exercises the slice arithmetic". That reads as the careful choice and is
 * the opposite of one, because of *when* it fires: only when no disk is blank,
 * which is to say only on a machine whose disks are all partitioned. That is
 * not a description of the test rig. It is a description of somebody's computer.
 *
 * So the branch that wrote to a stranger's partition was the branch that could
 * only ever run on a stranger's machine, and it never ran in the rig at all --
 * every disk the rig builds is blank, so the first preference always won and
 * this one sat unexercised through five checkpoints.
 *
 * It was found by booting the real install medium: with USB storage working the
 * kernel could finally see the stick it had booted from, the stick is
 * partitioned, and the self-test picked the BIOS boot partition and wrote to
 * it. It only failed because that run was deliberately read-only.
 *
 * A partition belongs to whoever's data is in it. There is no version of this
 * test worth a write to that.
 */
#define BLOCK_TEST_PAGES 4

/* Where it is safe and meaningful to write.
 *
 * Preference order, and each step is a fallback rather than a nicety:
 *
 *   an unpartitioned disk   -- the whole surface, which is what the rig builds
 *                              and what an installer meets on a new machine
 *   anything at all         -- read-only test, better than no test
 *
 * There is no third option, and the missing one is the point: see KF-140 above.
 * A partitioned disk and every slice of it are refused for writing on a machine
 * this kernel does not own, which is every machine except the rig's.
 */
static struct block_device *pick_test_device(bool *writable)
{
	struct block_device *whole_blank = 0, *anything = 0;

	for (unsigned i = 0; i < device_count; i++) {
		struct block_device *d = &devices[i];

		if (!d->present)
			continue;

		if (!anything)
			anything = d;

		if (d->read_only)
			continue;

		/* A disk whose table could not be *read* is not a blank disk.
		 *
		 * It has no slices, because none could be worked out -- which
		 * is exactly what a blank disk looks like from here, and the
		 * difference is somebody's data. The kernel already tells them
		 * apart: `install.c` refuses to install onto UNREADABLE for the
		 * same reason. This is the second caller, and it was not asking.
		 *
		 * KF-197. Found by working out what would happen if a real USB
		 * stick were attached at boot rather than by anything failing. */
		if (d->scheme == BLOCK_SCHEME_UNREADABLE)
			continue;

		/* A whole device with no table and no slices. Both conditions,
		 * not one: `!parent` alone would accept a partitioned disk, and
		 * writing to the end of one lands on its backup GPT. */
		if (!d->parent && !d->slice_count && !whole_blank)
			whole_blank = d;
	}

	if (whole_blank) {
		*writable = true;
		return whole_blank;
	}

	*writable = false;
	return anything;
}

/* The one bug slicing introduces, and it is silent.
 *
 * A slice whose length is one block too long lets a write run into whatever
 * comes after it -- the next partition, or a GPT's backup structures -- and
 * report success. Nothing above the block layer can see it. So: write to the
 * last block of a slice, then read the block immediately after it *through the
 * parent* and check it did not move.
 *
 * Reading through the parent is the part that makes this a real test. Reading
 * through the slice would be asking the same arithmetic whether it agrees with
 * itself.
 */
static bool neighbour_untouched(struct block_device *slice, u8 *scratch, u8 *keep)
{
	struct block_device *parent;
	u64 after;

	if (!slice->parent)
		return true;

	parent = block_device_by_id(slice->parent, slice->parent_generation);
	if (!parent)
		return true;

	after = slice->first_lba + slice->block_count;
	if (after >= parent->block_count)
		return true;	/* the slice ends at the end of the disk */

	if (block_read(parent, after, 1, keep) != BLOCK_OK)
		return true;	/* cannot check; not a failure of the bound */

	for (u32 i = 0; i < slice->block_size; i++)
		scratch[i] = (u8)(i * 31 + 17);

	if (block_write(slice, slice->block_count - 1, 1, scratch) != BLOCK_OK)
		return true;

	if (block_read(parent, after, 1, scratch) != BLOCK_OK)
		return true;

	for (u32 i = 0; i < parent->block_size; i++) {
		if (scratch[i] != keep[i]) {
			kputs("  block: writing the last block of a partition "
			      "changed the block after it, so a slice is one "
			      "block too long\n");
			return false;
		}
	}

	return true;
}

/* --- that the queue is actually in an order ---------------------------------
 *
 * Against a device that is not there, and deliberately.
 *
 * The obvious test -- several threads asking for descending blocks and an
 * assertion that the driver saw them ascending -- cannot be written honestly,
 * because it only holds when the threads happen to overlap. With one caller the
 * queue never holds more than one request: the first arrival finds the device
 * idle, services itself, and leaves. That is the design working, and a test
 * that depended on losing that race would pass or fail by timing and tell
 * nobody anything either way.
 *
 * So the ordering is tested where the ordering lives. queue_insert is a pure
 * function of a list and a request; give it the worst arrival order there is --
 * strictly descending -- and the queue it produces is either sorted or it is
 * not. No threads, no timing, and it fails the same way on every machine.
 *
 * The control is the half that makes it a measurement. A device that says
 * seek_is_free must come out in arrival order, and if it does not, then the
 * sort is not being chosen -- it is just happening, and the flag that is
 * supposed to turn it off does nothing.
 */
static bool queue_is_sorted(struct block_device *dev, bool expect_sorted)
{
	/* Descending, so that every single insertion has to walk past nothing
	 * and land at the head. Ascending arrival would produce a sorted queue
	 * even from an implementation that only ever appends. */
	static struct block_request r[6];
	const u64 arrival[6] = { 500, 90, 4000, 12, 70, 1 };
	struct block_request *p;
	u64 last = 0;
	unsigned seen = 0;

	dev->queue = NULL;
	dev->queued = 0;
	dev->reordered = 0;

	for (unsigned i = 0; i < 6; i++) {
		r[i].lba = arrival[i];
		r[i].count = 1;
		r[i].next = NULL;
		queue_insert(dev, &r[i]);
	}

	for (p = dev->queue; p; p = p->next) {
		if (seen && expect_sorted && p->lba < last)
			return false;

		/* The control's assertion, and it is the stronger one: arrival
		 * order is a single sequence, so this cannot be satisfied by
		 * accident the way "sorted" sometimes can. */
		if (!expect_sorted && p->lba != arrival[seen])
			return false;

		last = p->lba;
		seen++;
	}

	if (seen != 6 || dev->queued != 6)
		return false;

	/* An insertion that landed in front of something already queued. Zero
	 * here with six descending arrivals would mean the list came out sorted
	 * without anything ever being put in order, which is not a thing that
	 * can happen -- so this catches a queue that was sorted by the test
	 * rather than by the code. */
	if (expect_sorted && !dev->reordered)
		return false;

	return true;
}

bool block_queue_test(void)
{
	struct block_device dev;
	bool ok = true;

	kmemset(&dev, 0, sizeof(dev));

	dev.seek_is_free = false;
	if (!queue_is_sorted(&dev, true)) {
		kputs("  block: requests came off the queue in the order they "
		      "arrived, not the order the head wants them\n");
		ok = false;
	}

	dev.seek_is_free = true;
	if (!queue_is_sorted(&dev, false)) {
		kputs("  block: a device that says seeking is free was sorted "
		      "anyway, so the sort is not a decision\n");
		ok = false;
	}

	return ok;
}

/* Read by nothing until today, which is the whole reason this is here.
 *
 * The counters have been incremented since the block layer was written and
 * printed nowhere, so the one thing that could have caught an I/O path quietly
 * losing them -- a rewrite that replaced two function bodies wholesale, say --
 * was a number nobody could see. They were in fact lost exactly that way, and
 * what noticed was a cache test failing for an unrelated reason.
 *
 * Printed late, beside the cache, because printed early it describes a machine
 * that has not done any work yet.
 */
void block_print_traffic(void)
{
	kprintf("\nBlock traffic\n");
	kprintf("  transfers    : %llu read, %llu written, %llu flushed\n",
		(unsigned long long)reads, (unsigned long long)writes,
		(unsigned long long)flushes);
	kprintf("  blocks       : %llu read, %llu written\n",
		(unsigned long long)blocks_read,
		(unsigned long long)blocks_written);

	for (unsigned i = 0; i < device_count; i++) {
		const struct block_device *d = &devices[i];

		if (!d->queued)
			continue;

		/* Per device, because the interesting number is the share that
		 * had to be put in front of something -- a queue that never
		 * reorders is one that is never deep, and that is a fact about
		 * this machine's traffic rather than about the scheduler. */
		kprintf("  %s", d->name);
		for (size_t pad = kstrlen(d->name); pad < 12; pad++)
			kputc(' ');
		kprintf(" : %llu queued, %llu put in order%s\n",
			(unsigned long long)d->queued,
			(unsigned long long)d->reordered,
			d->seek_is_free ? " (seeking is free, so none)" : "");
	}
}

/* A device that exists only here, so that retiring one can be exercised on a
 * machine with no disk at all -- which is most of the verification matrix.
 *
 * Every operation below refuses on a device that is not present, and that
 * refusal has been in the code since those functions were written. What did not
 * exist was any way to reach it. This is the test that the way now exists. */
static enum block_status ghost_read(struct block_device *dev, u64 lba,
				    u32 count, void *buf)
{
	(void)dev; (void)lba; (void)count; (void)buf;
	return BLOCK_OK;
}

static const struct block_ops ghost_ops = {
	.read = ghost_read,
};

static bool unregister_self_test(void)
{
	struct block_device *d;
	struct block_device *slice;
	u32 id, generation;
	u8 scratch[512];
	bool ok = true;

	d = block_register("ghost", &ghost_ops, 0, 512, 2048);

	if (!d) {
		kputs("  block: no room to register the retirement test\n");
		return true;
	}

	id = d->id;
	generation = d->generation;

	slice = block_register_slice(d, 1, 0, 1024, BLOCK_SCHEME_MBR);

	if (!block_device_by_id(id, generation)) {
		kputs("block: a device just registered cannot be found by "
		      "identity\n");
		ok = false;
	}

	block_unregister(d);

	/* The claim that matters: the identity is *retired*, not recycled.
	 * Something still holding this disk must fail to find it rather than
	 * find whatever is plugged in next. */
	if (block_device_by_id(id, generation)) {
		kputs("block: a retired device is still findable by its old "
		      "identity\n");
		ok = false;
	}

	/* And every operation refuses. Checked through the public entry point
	 * rather than by reading the flag, because the flag being false is not
	 * the claim -- the claim is that callers are stopped. */
	if (block_read(d, 0, 1, scratch) == BLOCK_OK) {
		kputs("block: a retired device still served a read\n");
		ok = false;
	}

	/* A partition of a disk that is gone is not a smaller disk that is
	 * still there. This is the assertion a naive implementation fails: the
	 * parent is marked absent and the slice is left behind, holding a
	 * filesystem that reads from a device nobody can reach. */
	if (slice && slice->present) {
		kputs("block: a partition outlived the disk it was on\n");
		ok = false;
	}

	/* Retiring twice is not an error and must not double-count. */
	block_unregister(d);

	return ok;
}

bool block_self_test(void)
{
	if (!unregister_self_test())
		return false;

	struct block_device *d;
	u8 *buf, *back;
	paddr_t buf_pages, back_pages;
	bool ok = true, writable;
	enum block_status s;
	u64 first;
	u32 count, bytes;

	d = pick_test_device(&writable);

	if (!d) {
		/* Not a failure. A machine with no disk attached is a machine
		 * this kernel should still boot on, and reporting "pass" for a
		 * test that did not run would be worse than saying so. */
		kputs("  block: no device to test against\n");
		return true;
	}

	bytes = BLOCK_TEST_PAGES * (u32)PAGE_SIZE;

	if (d->block_size > bytes) {
		kputs("  block: a block bigger than the test buffer\n");
		return false;
	}

	count = bytes / d->block_size;
	if ((u64)count > d->block_count)
		count = (u32)d->block_count;
	bytes = count * d->block_size;

	buf_pages  = pmm_alloc_pages(BLOCK_TEST_PAGES);
	back_pages = pmm_alloc_pages(BLOCK_TEST_PAGES);
	if (!buf_pages || !back_pages) {
		kputs("  block: no memory to test with\n");
		if (buf_pages)
			pmm_free_pages(buf_pages, BLOCK_TEST_PAGES);
		if (back_pages)
			pmm_free_pages(back_pages, BLOCK_TEST_PAGES);
		return false;
	}

	buf   = phys_to_virt(buf_pages);
	back  = phys_to_virt(back_pages);
	first = d->block_count - count;

	/* A pattern that fails loudly. All zeroes or all ones would read back
	 * correctly from a driver that transferred nothing at all, because the
	 * page allocator hands out cleared pages. Every byte differs from its
	 * neighbour, and the sequence does not repeat within a page, so a
	 * transfer that got the right bytes in the wrong order is caught too. */
	for (u32 i = 0; i < bytes; i++)
		buf[i] = (u8)(i * 7 + 3);

	s = block_read(d, first, count, back);
	if (s != BLOCK_OK) {
		kprintf("  block: could not read the last %u blocks of %s: %s\n",
			count, d->name, block_status_name(s));
		ok = false;
		goto out;
	}

	if (!writable) {
		/* Framed the way this file already frames a read-only disk:
		 * a fact about the device changing what the test can do, not a
		 * switch that turns a check off. */
		kprintf("  block: %s cannot be written to, so the read is the "
			"whole test\n", d->name);
		goto out;
	}

	s = block_write(d, first, count, buf);
	if (s != BLOCK_OK) {
		kprintf("  block: could not write to %s: %s\n", d->name,
			block_status_name(s));
		ok = false;
		goto out;
	}

	/* Between the write and the read, so that a driver which quietly hands
	 * back the caller's own buffer is caught rather than congratulated. */
	kmemset(buf, 0, bytes);

	s = block_read(d, first, count, buf);
	if (s != BLOCK_OK) {
		kprintf("  block: could not read back: %s\n", block_status_name(s));
		ok = false;
		goto out;
	}

	for (u32 i = 0; i < bytes; i++) {
		if (buf[i] != (u8)(i * 7 + 3)) {
			kprintf("  block: byte %u of %u read back as %u, not "
				"%u\n", i, bytes, buf[i],
				(unsigned)(u8)(i * 7 + 3));
			ok = false;
			break;
		}
	}

	if (!neighbour_untouched(d, buf, back + d->block_size))
		ok = false;

	/* Put it back, and only then report. A test that leaves the disk
	 * modified is a test that can only be run once. */
	if (block_write(d, first, count, back) != BLOCK_OK) {
		kputs("  block: could not restore what it borrowed\n");
		ok = false;
	}

	if (block_flush(d) != BLOCK_OK) {
		kputs("  block: flush failed, so nothing written can be relied "
		      "on\n");
		ok = false;
	}

	/* The range check, which is the part of this file that protects
	 * everything above it. One past the end must fail, and so must a count
	 * that wraps -- the second is the one a plausible implementation gets
	 * wrong. */
	if (block_read(d, d->block_count, 1, buf) != BLOCK_ERR_RANGE) {
		kputs("  block: reading one block past the end was allowed\n");
		ok = false;
	}

	/* Every block there could be. Asked from the last block, so that a
	 * check written as `lba + count > block_count` overflows and passes --
	 * which is what separates a real range check from one that looks like
	 * one. */
	if (block_read(d, d->block_count - 1, (u32)-1, buf) != BLOCK_ERR_RANGE) {
		kputs("  block: a length that wraps past the end was allowed\n");
		ok = false;
	}

out:
	pmm_free_pages(buf_pages, BLOCK_TEST_PAGES);
	pmm_free_pages(back_pages, BLOCK_TEST_PAGES);
	return ok;
}
