/* Finding out what a flush is actually worth, by cutting the power.
 *
 * Every crash-consistency scheme a filesystem can use rests on one property:
 * that a write which has been flushed is on the medium before a write issued
 * afterwards. Journalling rests on it. Copy-on-write rests on it. Careful write
 * ordering *is* it.
 *
 * That property is usually assumed. This file measures it.
 *
 * --- What it does ---
 *
 * Given `durability=<name>` on the command line, the kernel writes a numbered
 * marker to block N of that device, flushes, and repeats, counting upward,
 * for as long as it is allowed to run. Each marker carries its own number and a
 * checksum of itself.
 *
 * The harness then kills the machine at an arbitrary moment and reads the image
 * back. The property being checked is:
 *
 *     the markers present on the medium form an unbroken run from zero
 *
 * If marker 40 is on the disk and marker 12 is not, then a write that was
 * issued and flushed *later* reached the medium before one issued earlier. That
 * would mean this kernel cannot promise ordering, and every filesystem design
 * that assumes it is wrong in the same way.
 *
 * --- Why the marker is self-describing ---
 *
 * Each block holds its own number twice, at the front and at the end, with a
 * checksum between them. A block that reads back with two different numbers is
 * a block that was *torn* -- half of the old contents and half of the new --
 * which is the other thing a filesystem has to know about its medium, and the
 * one that decides whether a design may put two facts in one block and rely on
 * them agreeing.
 *
 * Nobody in the kernel calls this at boot. It runs only when asked for, on a
 * device named on the command line, because it writes to every block it
 * touches and there is no version of that which is safe to do by default.
 */
#include <recon/kernel/block.h>

#include <recon/kernel/console.h>
#include <recon/kernel/crc32.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/time.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/boot.h>

/* Distinctive enough that a block holding it was written by this and not left
 * over from something else. Four characters, so it survives being looked at in
 * a hex dump by a person. */
#define MARKER_MAGIC 0x4B52414Du	/* 'M','A','R','K' little-endian */

struct marker {
	u32 magic;
	u32 sequence;		/* which write this was */
	u32 crc;		/* over the bytes after it */
	u32 sequence_again;	/* the same number, at the far end of the block */
};

/* Finds `durability=<name>` and returns the name, or null. Written here rather
 * than in a general option parser because there is exactly one option and a
 * parser invented before its second caller gets its shape wrong. */
static const char *wanted_device(void)
{
	static char name[BLOCK_NAME_MAX];
	const char *p = boot_info()->cmdline;
	static const char key[] = "durability=";

	if (!p)
		return 0;

	while (*p) {
		const char *k = key;
		const char *q = p;

		while (*k && *q == *k) {
			q++;
			k++;
		}

		if (!*k) {
			size_t n = 0;

			while (q[n] && q[n] != ' ' && n + 1 < sizeof(name)) {
				name[n] = q[n];
				n++;
			}
			name[n] = '\0';
			return n ? name : 0;
		}

		while (*p && *p != ' ')
			p++;
		while (*p == ' ')
			p++;
	}

	return 0;
}

static struct block_device *find_by_name(const char *name)
{
	for (unsigned i = 0; i < block_device_count(); i++) {
		struct block_device *d = block_device_at(i);

		const char *a = d->name, *b = name;

		while (*a && *a == *b) {
			a++;
			b++;
		}

		if (*a == *b)
			return d;
	}

	return 0;
}

/* Writes markers until the machine is killed. Does not return. */
void durability_run(void)
{
	const char *name = wanted_device();
	struct block_device *dev;
	paddr_t page;
	u8 *block;
	struct marker *m;
	u32 sequence = 0;
	u64 limit;

	if (!name)
		return;

	dev = find_by_name(name);
	if (!dev) {
		kprintf("durability: no device called %s\n", name);
		return;
	}

	if (dev->read_only || dev->block_size < sizeof(*m) * 2) {
		kprintf("durability: %s cannot be used for this\n", name);
		return;
	}

	page = pmm_alloc_page();
	if (!page)
		return;

	block = phys_to_virt(page);

	/* One marker per block, from block zero upward, so that the harness can
	 * read the image sequentially and the sequence number and the block
	 * number are the same thing. */
	limit = dev->block_count;
	if (limit > 4096)
		limit = 4096;		/* enough to be killed in the middle of */

	kprintf("durability: writing markers to %s, %lu blocks, flushing each\n",
		name, limit);

	for (;;) {
		if (sequence >= limit) {
			kprintf("durability: wrote all %lu markers without being "
				"interrupted; the harness should kill sooner\n",
				limit);
			break;
		}

		kmemset(block, 0, dev->block_size);

		m = (struct marker *)block;
		m->magic          = MARKER_MAGIC;
		m->sequence       = sequence;
		m->sequence_again = sequence;
		m->crc            = crc32(&m->sequence_again,
					  sizeof(m->sequence_again));

		/* And the same number at the far end of the block, so a torn
		 * write shows as a disagreement between the two ends rather
		 * than as a block that merely looks odd. */
		{
			struct marker *tail = (struct marker *)
				(block + dev->block_size - sizeof(*m));

			*tail = *m;
		}

		if (block_write(dev, sequence, 1, block) != BLOCK_OK) {
			kprintf("durability: write %u failed\n", sequence);
			break;
		}

		/* THE POINT OF THE WHOLE FILE. If this does what it claims,
		 * marker N is on the medium before marker N+1 is issued. */
		if (block_flush(dev) != BLOCK_OK) {
			kprintf("durability: flush after %u failed\n", sequence);
			break;
		}

		sequence++;
	}

	pmm_free_page(page);
}
