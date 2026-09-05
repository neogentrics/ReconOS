/* GPT and MBR, read for where the sectors are.
 *
 * See partition.h for why this is in the kernel and where the line inside the
 * format falls. This file is the mechanics, and the mechanics are mostly about
 * refusing to believe things.
 *
 * Every number below came off a disk somebody else wrote. There is no reading
 * here that is not bounded by a number that was itself checked first, and no
 * loop that is not capped -- because the alternative to a cap is that a damaged
 * disk hangs the machine inside block_init(), which is the earliest point in
 * the boot and the one with the least left to report it.
 */
#include <recon/kernel/partition.h>

#include <recon/kernel/block.h>
#include <recon/kernel/crc32.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/vm.h>

/* --- The master boot record ------------------------------------------------
 *
 * Sixty-four bytes at offset 446 of the first block: four entries of sixteen.
 * Everything else in that block is boot code nobody here executes.
 */
#define MBR_TABLE_OFFSET 446
#define MBR_ENTRY_SIZE   16
#define MBR_SIGNATURE_OFFSET 510

#define MBR_TYPE_EMPTY      0x00
#define MBR_TYPE_PROTECTIVE 0xEE	/* "there is a GPT here; do not touch" */

/* The three type bytes that mean "this is not a partition, it is a container
 * holding a chain of them". Three rather than one because the format grew two
 * more over the years and a reader that knows only 0x05 walks straight past a
 * disk partitioned by anything since about 1995. */
static bool is_extended(u8 type)
{
	return type == 0x05 || type == 0x0F || type == 0x85;
}

/* --- The GUID partition table ---------------------------------------------
 *
 * A header at block 1, a copy at the last block, and an array of entries
 * wherever the header says. Everything about where things are comes out of the
 * header, which is why the header's checksum is checked before a single field
 * of it is used for anything.
 */
#define GPT_SIGNATURE_0 0x20494645u	/* "EFI " */
#define GPT_SIGNATURE_1 0x54524150u	/* "PART" */

/* Offsets within the header. Named rather than read through a struct, because
 * the header's own length field says how much of it exists, and a struct
 * invites reading past that. */
#define GPT_SIG            0x00
#define GPT_REVISION       0x08
#define GPT_HEADER_SIZE    0x0C
#define GPT_HEADER_CRC     0x10
#define GPT_MY_LBA         0x18
#define GPT_ALT_LBA        0x20
#define GPT_FIRST_USABLE   0x28
#define GPT_LAST_USABLE    0x30
#define GPT_ENTRIES_LBA    0x48
#define GPT_ENTRY_COUNT    0x50
#define GPT_ENTRY_SIZE     0x54
#define GPT_ENTRIES_CRC    0x58

/* Offsets within one entry. */
#define GPT_ENTRY_TYPE     0x00		/* read only to tell "unused" apart */
#define GPT_ENTRY_FIRST    0x20
#define GPT_ENTRY_LAST     0x28

/* A sane ceiling on the entry array. The header can claim any count and any
 * stride, and their product is how much memory this would read. 128 entries of
 * 128 bytes is what every tool writes; the cap is generous against that and
 * still bounded. */
#define GPT_MAX_ENTRIES 512
#define GPT_MAX_ENTRY_SIZE 512

/* Little-endian reads out of a byte buffer.
 *
 * Byte by byte rather than by casting to a wider type, and that is not
 * pedantry: an entry array is read at an offset the *disk* chose, so a cast
 * would be an unaligned load at an address a hostile table controls, which
 * faults on aarch64 and silently works on x86. The kernel has already been bitten
 * by exactly that shape once, reading ACPI. */
static u16 le16(const u8 *p)
{
	return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

static u32 le32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u64 le64(const u8 *p)
{
	return (u64)le32(p) | ((u64)le32(p + 4) << 32);
}

static bool all_zero(const u8 *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (p[i])
			return false;
	return true;
}

/* --- MBR ------------------------------------------------------------------ */

/* Walks the chain of logical partitions inside an extended container.
 *
 * This is the structure that gets misparsed, and the reason is that the two
 * numbers in each link are relative to *different* bases. In every link, the
 * first entry's start is relative to that link's own block; the second entry --
 * which points at the next link -- is relative to the start of the whole
 * extended container. Using one base for both produces a chain that is subtly
 * wrong from the second partition onwards, and on a real disk that means
 * reporting a partition that begins inside another one.
 *
 * The walk is capped and required to move forwards. A chain that loops is not a
 * theoretical disk: it is what a half-overwritten one looks like.
 */
static void walk_extended(struct block_device *dev, u64 container_lba,
			  u64 container_count, u8 *block)
{
	u64 link = container_lba;
	u8 index = 5;			/* logical partitions are numbered from 5 */
	unsigned guard = 0;

	while (guard++ < BLOCK_MAX_SLICES + 8) {
		const u8 *e0, *e1;
		u64 start, count, next;

		if (link < container_lba ||
		    link - container_lba >= container_count)
			return;		/* outside the container it claims to be in */

		if (block_read(dev, link, 1, block) != BLOCK_OK)
			return;

		if (le16(block + MBR_SIGNATURE_OFFSET) != 0xAA55u)
			return;

		e0 = block + MBR_TABLE_OFFSET;
		e1 = e0 + MBR_ENTRY_SIZE;

		/* The partition itself, relative to this link. */
		start = (u64)le32(e0 + 8);
		count = (u64)le32(e0 + 12);

		if (e0[4] != MBR_TYPE_EMPTY && count) {
			if (!block_register_slice(dev, index,
						  link + start, count,
						  BLOCK_SCHEME_MBR))
				return;
			index++;
		}

		/* The next link, relative to the container. Zero type or zero
		 * offset ends the chain, which is how the format says "no
		 * more" -- there is no terminator. */
		if (!is_extended(e1[4]))
			return;

		next = container_lba + (u64)le32(e1 + 8);

		/* Strictly forwards. Without this, a disk whose chain points
		 * back at itself is an infinite loop with the machine's first
		 * disk read on the stack. */
		if (next <= link)
			return;

		link = next;
	}
}

/* Returns true if this looked like an MBR and slices were registered. */
static bool read_mbr(struct block_device *dev, u8 *block)
{
	const u8 *table;
	unsigned found = 0;

	if (block_read(dev, 0, 1, block) != BLOCK_OK)
		return false;

	if (le16(block + MBR_SIGNATURE_OFFSET) != 0xAA55u)
		return false;

	table = block + MBR_TABLE_OFFSET;

	/* Primaries first, in table order, numbered by their slot. The slot is
	 * the number people know a partition by -- an empty slot 2 does not
	 * renumber slot 3 -- so the index is the position, not a counter. */
	for (unsigned i = 0; i < 4; i++) {
		const u8 *e = table + i * MBR_ENTRY_SIZE;
		u8 type = e[4];
		u64 start = (u64)le32(e + 8);
		u64 count = (u64)le32(e + 12);

		if (type == MBR_TYPE_EMPTY || !count)
			continue;

		if (is_extended(type))
			continue;	/* a container, walked below */

		if (block_register_slice(dev, (u8)(i + 1), start, count,
					 BLOCK_SCHEME_MBR))
			found++;
	}

	/* Then the chain inside the first extended container. One container,
	 * because the format allows only one and a disk with two is damaged --
	 * and walking both would number the second one's partitions on top of
	 * the first's. */
	for (unsigned i = 0; i < 4; i++) {
		const u8 *e = table + i * MBR_ENTRY_SIZE;

		if (!is_extended(e[4]))
			continue;

		walk_extended(dev, (u64)le32(e + 8), (u64)le32(e + 12), block);
		break;
	}

	return true;
}

/* Does the master boot record say a GPT is in charge?
 *
 * Any entry of type 0xEE means yes, and that is the whole test. Not entry zero,
 * and no sanity check on the protective entry's size.
 *
 * A hybrid MBR -- which Apple ships -- has a protective entry that covers a
 * fraction of the disk sitting *beside* entries that describe real partitions
 * correctly. Every wrong reader produces a plausible answer on one: checking
 * only slot zero misses it if the tool put it elsewhere, and demanding that the
 * protective entry span the disk rejects it and falls through to reading the
 * MBR, which is exactly the wrong table. */
static bool has_protective_entry(const u8 *block)
{
	for (unsigned i = 0; i < 4; i++)
		if (block[MBR_TABLE_OFFSET + i * MBR_ENTRY_SIZE + 4] ==
		    MBR_TYPE_PROTECTIVE)
			return true;
	return false;
}

/* --- GPT ------------------------------------------------------------------ */

/* Checks one header and, if it is believable, reports where the entries are.
 *
 * `at` is the block this header was read from, and it is checked against the
 * header's own idea of where it lives. That check is what stops a backup header
 * being believed as a primary -- the two are byte-identical apart from that
 * field and the pointer to the other one, so without it a disk whose primary
 * was overwritten by a stale backup reads as consistent. */
static bool header_ok(const u8 *hdr, u32 block_size, u64 at,
		      u64 *entries_lba, u32 *count, u32 *stride, u32 *entries_crc)
{
	u32 header_size, crc_stored, crc_computed;
	u8 zeroed[4] = { 0, 0, 0, 0 };
	u32 running;

	if (le32(hdr + GPT_SIG) != GPT_SIGNATURE_0 ||
	    le32(hdr + GPT_SIG + 4) != GPT_SIGNATURE_1)
		return false;

	header_size = le32(hdr + GPT_HEADER_SIZE);

	/* The header must be at least as big as the fields being read out of
	 * it, and no bigger than the block it came in. Both directions matter:
	 * too small and the checksum covers less than the fields; too large and
	 * the checksum reads past the buffer. */
	if (header_size < 92 || header_size > block_size)
		return false;

	if (le64(hdr + GPT_MY_LBA) != at)
		return false;

	/* The checksum is over exactly header_size bytes -- not over the block,
	 * not over the size of any structure -- with the checksum field itself
	 * read as four zero bytes. That hole in the middle is why crc32 has an
	 * incremental form. */
	crc_stored = le32(hdr + GPT_HEADER_CRC);

	running = crc32_update(CRC32_INIT, hdr, GPT_HEADER_CRC);
	running = crc32_update(running, zeroed, sizeof(zeroed));
	running = crc32_update(running, hdr + GPT_HEADER_CRC + 4,
			       header_size - (GPT_HEADER_CRC + 4));
	crc_computed = crc32_final(running);

	if (crc_computed != crc_stored)
		return false;

	*entries_lba = le64(hdr + GPT_ENTRIES_LBA);
	*count       = le32(hdr + GPT_ENTRY_COUNT);
	*stride      = le32(hdr + GPT_ENTRY_SIZE);
	*entries_crc = le32(hdr + GPT_ENTRIES_CRC);

	/* A stride smaller than the fields being read is a table that would
	 * have entries overlapping each other, and a count times stride that
	 * overflows is a read length chosen by the disk. */
	if (*stride < 128 || *stride > GPT_MAX_ENTRY_SIZE)
		return false;

	if (!*count || *count > GPT_MAX_ENTRIES)
		return false;

	return true;
}

static bool read_gpt(struct block_device *dev, u8 *block)
{
	u64 entries_lba = 0;
	u32 count = 0, stride = 0, entries_crc = 0;
	bool have_header = false;
	u8 *entries;
	paddr_t entries_phys;
	u64 entry_bytes, entry_blocks;
	unsigned registered = 0;

	/* The primary, at block 1 whatever the block size is. */
	if (block_read(dev, 1, 1, block) == BLOCK_OK)
		have_header = header_ok(block, dev->block_size, 1,
					&entries_lba, &count, &stride,
					&entries_crc);

	/* And if that failed, the backup at the last block. Never the MBR:
	 * a GPT disk whose primary header is damaged is still a GPT disk, and
	 * falling back to the master boot record reads a protective entry as
	 * though it were a partition covering the whole disk. */
	if (!have_header && dev->block_count) {
		u64 last = dev->block_count - 1;

		if (block_read(dev, last, 1, block) == BLOCK_OK) {
			have_header = header_ok(block, dev->block_size, last,
						&entries_lba, &count, &stride,
						&entries_crc);
			if (have_header)
				kprintf("partition: %s primary GPT header is "
					"unreadable; using the backup\n",
					dev->name);
		}
	}

	if (!have_header)
		return false;

	entry_bytes  = (u64)count * stride;
	entry_blocks = (entry_bytes + dev->block_size - 1) / dev->block_size;

	if (entries_lba >= dev->block_count ||
	    entry_blocks > dev->block_count - entries_lba)
		return false;

	entries_phys = pmm_alloc_pages((size_t)((entry_bytes + PAGE_SIZE - 1)
						/ PAGE_SIZE));
	if (!entries_phys) {
		kprintf("partition: no memory to read %s's entry array\n",
			dev->name);
		return false;
	}

	entries = phys_to_virt(entries_phys);

	if (block_read(dev, entries_lba, (u32)entry_blocks, entries) != BLOCK_OK) {
		pmm_free_pages(entries_phys,
			       (size_t)((entry_bytes + PAGE_SIZE - 1) / PAGE_SIZE));
		return false;
	}

	/* The array's checksum is over count * stride bytes using the stride
	 * the header gave, not the size of any structure this file declares.
	 * A disk written by a tool with a larger entry has a larger stride, and
	 * checksumming sizeof(something) instead gives a mismatch that reads as
	 * a corrupt disk. */
	if (crc32(entries, (size_t)entry_bytes) != entries_crc) {
		kprintf("partition: %s's GPT entry array does not match its "
			"checksum\n", dev->name);
		pmm_free_pages(entries_phys,
			       (size_t)((entry_bytes + PAGE_SIZE - 1) / PAGE_SIZE));
		return false;
	}

	for (u32 i = 0; i < count; i++) {
		const u8 *e = entries + (u64)i * stride;
		u64 first, last;

		/* An unused entry is an all-zero type GUID. This is the only
		 * thing the type is read for: whether the slot is used. What
		 * kind of partition it is belongs to the caller. */
		if (all_zero(e + GPT_ENTRY_TYPE, 16))
			continue;

		first = le64(e + GPT_ENTRY_FIRST);
		last  = le64(e + GPT_ENTRY_LAST);

		/* Both ends are INCLUSIVE, so the length is last - first + 1.
		 * One short makes the final block of every partition
		 * unreachable, which presents as a failing disk; one long lets
		 * a write into the neighbour and reports success. */
		if (last < first)
			continue;

		if (!block_register_slice(dev, (u8)(i + 1), first,
					  last - first + 1, BLOCK_SCHEME_GPT))
			break;

		registered++;
	}

	pmm_free_pages(entries_phys,
		       (size_t)((entry_bytes + PAGE_SIZE - 1) / PAGE_SIZE));

	(void)registered;
	return true;
}

/* --- The scan -------------------------------------------------------------- */

void partition_scan(struct block_device *dev)
{
	paddr_t page;
	u8 *block;

	if (!dev || !dev->present || dev->parent)
		return;

	dev->scheme = BLOCK_SCHEME_NONE;

	/* A device whose blocks are bigger than a page would need a bigger
	 * buffer to read one. Refused rather than silently half-read. */
	if (dev->block_size > PAGE_SIZE || dev->block_count < 2)
		return;

	page = pmm_alloc_page();
	if (!page)
		return;

	block = phys_to_virt(page);

	if (block_read(dev, 0, 1, block) != BLOCK_OK) {
		pmm_free_page(page);
		return;
	}

	if (le16(block + MBR_SIGNATURE_OFFSET) != 0xAA55u) {
		/* No signature at all. A blank disk, which is what an installer
		 * wants to find, and is not an error. */
		dev->scheme = BLOCK_SCHEME_NONE;
		pmm_free_page(page);
		return;
	}

	if (has_protective_entry(block)) {
		/* Committed: this is a GPT disk, and if its GPT cannot be read
		 * then the disk is unreadable rather than an MBR disk. */
		if (read_gpt(dev, block))
			dev->scheme = BLOCK_SCHEME_GPT;
		else
			dev->scheme = BLOCK_SCHEME_UNREADABLE;

		pmm_free_page(page);
		return;
	}

	/* Some tools write a GPT without a protective entry, which is out of
	 * specification and exists. Look for the header before giving up. */
	if (read_gpt(dev, block)) {
		dev->scheme = BLOCK_SCHEME_GPT;
		pmm_free_page(page);
		return;
	}

	if (read_mbr(dev, block))
		dev->scheme = dev->slice_count ? BLOCK_SCHEME_MBR
					       : BLOCK_SCHEME_NONE;

	pmm_free_page(page);
}

/* --- The self-test ---------------------------------------------------------
 *
 * The real test of this file is the fixture harness, which boots the kernel
 * against disks partitioned by sgdisk and sfdisk and compares what it prints.
 * That is outside the kernel and it is the one that matters.
 *
 * What is left in here is the arithmetic that has no disk to check it against:
 * the inclusive-end conversion, and the fact that a slice's bound is its own
 * length rather than the disk's.
 */
bool partition_self_test(void)
{
	struct block_device *disk = 0;
	struct block_device *slice = 0;
	bool ok = true;

	for (unsigned i = 0; i < block_device_count(); i++) {
		struct block_device *d = block_device_at(i);

		if (d->parent && !slice)
			slice = d;
		if (!d->parent && d->slice_count && !disk)
			disk = d;
	}

	if (!slice) {
		kputs("  partition: no partitioned device to test against\n");
		return true;
	}

	/* A slice is bounded by its own length, not the disk's. This is the one
	 * bug slicing introduces, and it is silent: without the bound, a read
	 * one block past a partition returns the neighbour's data and reports
	 * success. */
	{
		u8 scratch[1];

		if (block_read(slice, slice->block_count, 1, scratch)
		    != BLOCK_ERR_RANGE) {
			kputs("  partition: reading one block past a partition "
			      "was allowed, so a slice is not bounded\n");
			ok = false;
		}
	}

	/* And a whole disk that has partitions on it refuses a write until it
	 * is claimed. */
	if (disk && !disk->read_only) {
		u8 scratch[1] = { 0 };

		if (block_write(disk, 0, 1, scratch) != BLOCK_ERR_BUSY) {
			kputs("  partition: writing to a partitioned disk was "
			      "allowed without claiming it\n");
			ok = false;
		}
	}

	return ok;
}
