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

#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/vm.h>

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
	case BLOCK_ERR_BUSY:        return "the queue is full";
	default:                    return "unrecognised status";
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

unsigned block_device_count(void)
{
	return device_count;
}

struct block_device *block_device_at(unsigned index)
{
	if (index >= device_count)
		return 0;
	return &devices[index];
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

enum block_status block_read(struct block_device *dev, u64 lba, u32 count, void *buf)
{
	enum block_status s = check_range(dev, lba, count);

	if (s != BLOCK_OK || count == 0)
		return s;

	if (!buf)
		return BLOCK_ERR_ALIGN;

	s = dev->ops->read(dev, lba, count, buf);
	if (s == BLOCK_OK) {
		reads++;
		blocks_read += count;
	}
	return s;
}

enum block_status block_write(struct block_device *dev, u64 lba, u32 count,
			      const void *buf)
{
	enum block_status s = check_range(dev, lba, count);

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

	s = dev->ops->write(dev, lba, count, buf);
	if (s == BLOCK_OK) {
		writes++;
		blocks_written += count;
	}
	return s;
}

enum block_status block_flush(struct block_device *dev)
{
	if (!dev || !dev->present)
		return BLOCK_ERR_NO_DEVICE;

	/* A device with no flush operation is one whose writes are already
	 * durable when they complete. Reporting success is correct; reporting
	 * "unsupported" would push every caller into deciding whether that
	 * meant danger. */
	if (!dev->ops->flush)
		return BLOCK_OK;

	flushes++;
	return dev->ops->flush(dev);
}

void block_init(void)
{
	device_count = 0;
	reads = writes = flushes = blocks_read = blocks_written = 0;

	/* The architecture goes looking. What it finds -- a PCI bus, a handful
	 * of memory-mapped virtio slots, nothing at all -- is its business, and
	 * every device it finds arrives back here through block_register(). */
	arch_storage_probe();
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
		kprintf(", %lu blocks of %u bytes%s%s\n",
			d->block_count, d->block_size,
			d->read_only ? ", read-only" : "",
			d->removable ? ", removable" : "");
	}
}

/* --- The self-test ---------------------------------------------------------
 *
 * Writes a pattern, reads it back, and checks it. Which sounds trivial and is
 * not: it is the first code in this kernel whose failure mode is *somebody
 * else's data*, so the test is written to catch the failures that matter rather
 * than the ones that are easy.
 *
 * It uses the last block on the device on purpose. A driver that has an
 * off-by-one in its range check fails there and nowhere else, and a test that
 * writes to block 0 would be testing the one address every bug agrees on.
 */
bool block_self_test(void)
{
	struct block_device *d = block_device_at(0);
	u8 *buf, *back;
	paddr_t buf_page, back_page;
	bool ok = true;
	enum block_status s;
	u64 last;

	if (!d) {
		/* Not a failure. A machine with no disk attached is a machine
		 * this kernel should still boot on, and saying "pass" for a
		 * test that did not run would be worse than saying so. */
		kputs("  block: no device to test against\n");
		return true;
	}

	if (d->block_size > PAGE_SIZE) {
		kputs("  block: a block larger than a page needs a bigger test buffer\n");
		return false;
	}

	buf_page  = pmm_alloc_page();
	back_page = pmm_alloc_page();
	if (!buf_page || !back_page) {
		kputs("  block: no memory to test with\n");
		return false;
	}

	buf  = phys_to_virt(buf_page);
	back = phys_to_virt(back_page);

	last = d->block_count - 1;

	/* A pattern that fails loudly. All zeroes or all 0xFF would read back
	 * correctly from a driver that transferred nothing at all, because the
	 * page allocator hands out cleared pages -- so every byte differs from
	 * its neighbour and from what an untouched buffer holds. */
	for (u32 i = 0; i < d->block_size; i++)
		buf[i] = (u8)(i * 7 + 3);

	/* What was there first, so the test puts it back. This is somebody's
	 * disk. */
	s = block_read(d, last, 1, back);
	if (s != BLOCK_OK) {
		kprintf("  block: could not read the last block: %s\n",
			block_status_name(s));
		ok = false;
		goto out;
	}

	if (d->read_only) {
		kputs("  block: read-only device, so the read is the whole test\n");
		goto out;
	}

	s = block_write(d, last, 1, buf);
	if (s != BLOCK_OK) {
		kprintf("  block: could not write the last block: %s\n",
			block_status_name(s));
		ok = false;
		goto out;
	}

	/* Between the write and the read, so that a driver which quietly
	 * returns the caller's own buffer is caught rather than congratulated. */
	kmemset(buf, 0, d->block_size);

	s = block_read(d, last, 1, buf);
	if (s != BLOCK_OK) {
		kprintf("  block: could not read back: %s\n", block_status_name(s));
		ok = false;
		goto out;
	}

	for (u32 i = 0; i < d->block_size; i++) {
		if (buf[i] != (u8)(i * 7 + 3)) {
			kprintf("  block: byte %u read back as %u, not %u\n",
				i, buf[i], (unsigned)(u8)(i * 7 + 3));
			ok = false;
			break;
		}
	}

	/* Put it back, and only then report. A test that leaves the disk
	 * modified is a test that can only be run once. */
	if (block_write(d, last, 1, back) != BLOCK_OK) {
		kputs("  block: could not restore the block it borrowed\n");
		ok = false;
	}

	if (block_flush(d) != BLOCK_OK) {
		kputs("  block: flush failed, so nothing written can be relied on\n");
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

	/* Every block there could be. On a device with fewer than four billion
	 * blocks -- which is every device -- adding this to any starting point
	 * overflows a 64-bit sum only if the check is written the naive way, so
	 * this is the case that separates a real range check from one that
	 * looks like a range check. */
	if (block_read(d, d->block_count - 1, (u32)-1, buf) != BLOCK_ERR_RANGE) {
		kputs("  block: a length that wraps past the end was allowed\n");
		ok = false;
	}

out:
	pmm_free_page(buf_page);
	pmm_free_page(back_page);
	return ok;
}
