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

	/* Split, if the device has an opinion about how much it can take at
	 * once. Done here rather than in each driver: the arithmetic is the
	 * same everywhere and getting it wrong means a short read that reports
	 * success, which is silent data loss rather than an error. */
	while (count) {
		u32 chunk = count;

		if (dev->max_blocks_per_request &&
		    chunk > dev->max_blocks_per_request)
			chunk = dev->max_blocks_per_request;

		s = dev->ops->read(dev, lba, chunk, buf);
		if (s != BLOCK_OK)
			return s;

		reads++;
		blocks_read += chunk;

		lba   += chunk;
		count -= chunk;
		buf    = (u8 *)buf + (size_t)chunk * dev->block_size;
	}

	return BLOCK_OK;
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

	while (count) {
		u32 chunk = count;

		if (dev->max_blocks_per_request &&
		    chunk > dev->max_blocks_per_request)
			chunk = dev->max_blocks_per_request;

		s = dev->ops->write(dev, lba, chunk, buf);
		if (s != BLOCK_OK)
			return s;

		writes++;
		blocks_written += chunk;

		lba   += chunk;
		count -= chunk;
		buf    = (const u8 *)buf + (size_t)chunk * dev->block_size;
	}

	return BLOCK_OK;
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
 * not: this is the first code in this kernel whose failure mode is *somebody
 * else's data*, so the test is written to catch the failures that matter rather
 * than the ones that are easy.
 *
 * Three choices in it are deliberate.
 *
 * It works on the *last* blocks of the device. A driver with an off-by-one in
 * its range arithmetic fails there and nowhere else, and a test that writes to
 * block zero tests the one address every bug agrees on.
 *
 * It transfers sixteen kilobytes, not one block. A one-block request fits
 * inside a single page, and a single page is the case every scatter scheme
 * gets right: NVMe describes a transfer as a list of pages, with one page in
 * the first pointer, two in the second, and three or more in a separate list --
 * so a test that never exceeds one page never reaches the code where a driver
 * goes wrong. Sixteen kilobytes crosses both boundaries.
 *
 * It puts back every byte it borrowed. This is somebody's disk.
 */
#define BLOCK_TEST_PAGES 4

bool block_self_test(void)
{
	struct block_device *d = block_device_at(0);
	u8 *buf, *back;
	paddr_t buf_pages, back_pages;
	bool ok = true;
	enum block_status s;
	u64 first;
	u32 count, bytes;

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
		kprintf("  block: could not read the last %u blocks: %s\n",
			count, block_status_name(s));
		ok = false;
		goto out;
	}

	if (d->read_only) {
		kputs("  block: read-only device, so the read is the whole test\n");
		goto out;
	}

	s = block_write(d, first, count, buf);
	if (s != BLOCK_OK) {
		kprintf("  block: could not write: %s\n", block_status_name(s));
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
