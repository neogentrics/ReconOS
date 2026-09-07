/* Adding partitions to somebody's disk without disturbing the ones there.
 *
 * This is the first code in the project that writes a partition table, and a
 * partition table is the one structure on a disk where being wrong does not
 * lose a file, it loses every file. So the rule it is built on is narrower than
 * "write it correctly":
 *
 *   **Every entry that was already there is preserved byte for byte, and this
 *   code never regenerates one it did not create.**
 *
 * Not "written back identically" -- *not rewritten at all*. The entry array is
 * read, the empty slots this install claims are filled in, and the array goes
 * back. Windows' entry keeps its type GUID, its unique GUID, its name and its
 * attribute bits exactly as they were, including any bit this code has never
 * heard of. Regenerating an entry "identically" from fields we understand is
 * how a disk stops mounting: the fields we do not understand are the ones that
 * would go missing, and they are missing precisely because we did not know to
 * look.
 *
 * --- The order, and what a power cut leaves ---------------------------------
 *
 * There are two copies of the table. They are written primary first:
 *
 *   1. primary entry array
 *   2. primary header        3. flush
 *   4. backup entry array
 *   5. backup header         6. flush
 *
 * Cut the power between (1) and (2) and the primary header's stored checksum no
 * longer matches the entry array it describes, so every tool that reads this
 * disk rejects the primary and uses the backup -- which is the *old* table,
 * still valid, describing the disk exactly as it was. **The failure mode is the
 * disk reverting.**
 *
 * Between (2) and (5) the primary is the new table and the backup is stale.
 * Tools prefer the primary and report a repairable disagreement; `sgdisk -v`
 * says so and fixes it in one command.
 *
 * There is no ordering that makes this atomic, because the format has no way to
 * express that. There is only an ordering whose failures are recoverable, and
 * this is it.
 */
#include <recon/kernel/install.h>

#include <recon/kernel/block.h>
#include <recon/kernel/console.h>
#include <recon/kernel/crc32.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/time.h>

#define GPT_HDR_SIG		0
#define GPT_HDR_REVISION	8
#define GPT_HDR_SIZE		12
#define GPT_HDR_CRC		16
#define GPT_HDR_MY_LBA		24
#define GPT_HDR_ALT_LBA		32
#define GPT_HDR_FIRST_USABLE	40
#define GPT_HDR_LAST_USABLE	48
#define GPT_HDR_DISK_GUID	56
#define GPT_HDR_ENTRY_LBA	72
#define GPT_HDR_ENTRY_COUNT	80
#define GPT_HDR_ENTRY_SIZE	84
#define GPT_HDR_ENTRY_CRC	88

#define GPT_ENT_TYPE		0
#define GPT_ENT_GUID		16
#define GPT_ENT_FIRST		32
#define GPT_ENT_LAST		40
#define GPT_ENT_ATTR		48
#define GPT_ENT_NAME		56	/* 36 UTF-16 units */

static const u8 type_esp[16] = {
	0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
	0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

/* ReconOS's own partition type.
 *
 * A type GUID is a vendor-assigned 16-byte identifier and nothing more -- it is
 * not required to be an RFC 4122 UUID, and plenty of the well-known ones are
 * not. So this one is chosen to be *legible in a hex dump*, because the moment
 * somebody is staring at `xxd` output wondering whose partition this is, a
 * recognisable name is worth more than a random number:
 *
 *   52 65 63 6F 6E 4F 53 00 53 79 73 74 65 6D 00 01   ReconOS.System..
 */
static const u8 type_reconos[16] = {
	'R', 'e', 'c', 'o', 'n', 'O', 'S', 0x00,
	'S', 'y', 's', 't', 'e', 'm', 0x00, 0x01
};

static void put32(u8 *p, u32 v)
{
	p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

static void put64(u8 *p, u64 v)
{
	put32(p, (u32)v);
	put32(p + 4, (u32)(v >> 32));
}

static u32 get32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u64 get64(const u8 *p)
{
	return (u64)get32(p) | ((u64)get32(p + 4) << 32);
}

/* --- Identifiers -----------------------------------------------------------
 *
 * Every partition needs a unique GUID, and so does the disk. **Unique, not
 * unguessable.** These are identifiers, not secrets: nothing is protected by
 * one being hard to predict, and a comment implying otherwise would be a
 * security claim this code does not support.
 *
 * So the requirement is that two disks, or two partitions, do not collide, and
 * that comes from mixing things that differ between them: the disk's own name
 * and size, the monotonic clock, and a counter that moves within a single run.
 *
 * **There is no hardware randomness here, and that is stated rather than
 * implied.** cpu.h records whether the processor has a source, for an entropy
 * pool this kernel has not built yet; when it exists, folding it in is one line
 * and this comment should stop saying otherwise. Until then, two machines
 * booting the same image and installing at the same monotonic instant onto
 * identically-named disks of identical size would collide -- which is a real if
 * unlikely case, and the honest one to write down rather than to describe as
 * random.
 */
static u64 mix(u64 x)
{
	x ^= x >> 33;
	x *= 0xFF51AFD7ED558CCDull;
	x ^= x >> 33;
	x *= 0xC4CEB9FE1A85EC53ull;
	x ^= x >> 33;
	return x;
}

static void make_guid(const struct block_device *disk, u8 *out)
{
	static u64 counter;
	u64 a, b;
	const char *n = disk->name;
	u64 name_hash = 0xCBF29CE484222325ull;

	while (*n) {
		name_hash ^= (u8)*n++;
		name_hash *= 0x100000001B3ull;
	}

	a = mix(name_hash ^ (disk->block_count * 0x9E3779B97F4A7C15ull));
	b = mix(time_monotonic_ns() ^ (++counter * 0xD6E8FEB86659FD93ull));

	put64(out, a);
	put64(out + 8, b);

	/* Version 4 and the RFC 4122 variant, so anything that parses these as
	 * UUIDs sees a well-formed one rather than something it may decide to
	 * "correct". */
	out[7] = (u8)((out[7] & 0x0F) | 0x40);
	out[8] = (u8)((out[8] & 0x3F) | 0x80);
}

/* --- The protective MBR ----------------------------------------------------
 *
 * One entry of type 0xEE spanning the disk, which is what stops a tool that
 * only understands MBR from seeing an unpartitioned disk and offering to help.
 *
 * The stick this was first tested on had none at all -- LBA 0 was 512 zero
 * bytes with a perfectly valid GPT behind it, and `sgdisk` called that fine. So
 * the reader tolerates its absence. The writer still writes one, because
 * tolerating somebody else's omission and repeating it are different things.
 */
static void protective_mbr(u8 *sector, u64 blocks)
{
	u64 span = blocks - 1;

	kmemset(sector, 0, 512);

	sector[446 + 4] = 0xEE;			/* type */
	put32(sector + 446 + 8, 1);		/* first LBA */
	put32(sector + 446 + 12, span > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)span);

	/* The CHS fields, which nothing has used since the 1990s and which some
	 * firmware still refuses a disk without. The values are the
	 * conventional "as large as CHS can say". */
	sector[446 + 1] = 0x00;
	sector[446 + 2] = 0x02;
	sector[446 + 3] = 0x00;
	sector[446 + 5] = 0xFF;
	sector[446 + 6] = 0xFF;
	sector[446 + 7] = 0xFF;

	sector[510] = 0x55;
	sector[511] = 0xAA;
}

struct table {
	u32 entry_count;
	u32 entry_size;
	u64 first_usable;
	u64 last_usable;
	u64 entry_lba;
	u8  disk_guid[16];
	u8 *entries;		/* entry_count * entry_size */
	size_t entries_bytes;
};

static void free_table(struct table *t)
{
	kfree(t->entries);
	t->entries = 0;
}

/* Reads the table that is there, or invents an empty one for a blank disk. */
static enum install_verdict load_table(struct block_device *disk, bool fresh,
				       struct table *t)
{
	u8 *sector;
	enum install_verdict v = INSTALL_IO;

	kmemset(t, 0, sizeof(*t));

	t->entry_count = 128;
	t->entry_size = 128;
	t->entry_lba = 2;

	if (fresh) {
		t->entries_bytes = (size_t)t->entry_count * t->entry_size;
		t->entries = kzalloc(t->entries_bytes);
		if (!t->entries)
			return INSTALL_IO;

		/* 32 sectors of entries, so the first usable block is 34, and
		 * the same at the far end for the backup. */
		t->first_usable = 34;
		t->last_usable = disk->block_count - 34;
		make_guid(disk, t->disk_guid);
		return INSTALL_OK;
	}

	sector = kzalloc(disk->block_size);
	if (!sector)
		return INSTALL_IO;

	if (block_read(disk, 1, 1, sector) != BLOCK_OK)
		goto out;

	t->first_usable = get64(sector + GPT_HDR_FIRST_USABLE);
	t->last_usable  = get64(sector + GPT_HDR_LAST_USABLE);
	t->entry_lba    = get64(sector + GPT_HDR_ENTRY_LBA);
	t->entry_count  = get32(sector + GPT_HDR_ENTRY_COUNT);
	t->entry_size   = get32(sector + GPT_HDR_ENTRY_SIZE);
	kmemcpy(t->disk_guid, sector + GPT_HDR_DISK_GUID, 16);

	if (!t->entry_count || t->entry_count > 128 || t->entry_size < 128 ||
	    t->entry_size > disk->block_size) {
		v = INSTALL_NO_TABLE_READABLE;
		goto out;
	}

	t->entries_bytes = (size_t)t->entry_count * t->entry_size;
	t->entries = kzalloc(t->entries_bytes);
	if (!t->entries)
		goto out;

	{
		u32 per = disk->block_size / t->entry_size;
		u32 sectors = (t->entry_count + per - 1) / per;

		if (block_read(disk, t->entry_lba, sectors, t->entries) !=
		    BLOCK_OK) {
			free_table(t);
			goto out;
		}
	}

	v = INSTALL_OK;
out:
	kfree(sector);
	if (v != INSTALL_OK)
		free_table(t);
	return v;
}

/* The first entry slot with a zero type, which is what "unused" means. */
static u8 *free_slot(struct table *t, unsigned *index)
{
	u32 i, j;

	for (i = 0; i < t->entry_count; i++) {
		u8 *e = t->entries + (size_t)i * t->entry_size;
		bool empty = true;

		for (j = 0; j < 16; j++)
			if (e[j]) {
				empty = false;
				break;
			}

		if (empty) {
			*index = i + 1;
			return e;
		}
	}

	return 0;
}

static void fill_entry(struct block_device *disk, u8 *e, const u8 *type,
		       u64 first, u64 count, const char *name)
{
	unsigned i;

	kmemset(e, 0, 128);
	kmemcpy(e + GPT_ENT_TYPE, type, 16);
	make_guid(disk, e + GPT_ENT_GUID);
	put64(e + GPT_ENT_FIRST, first);
	put64(e + GPT_ENT_LAST, first + count - 1);

	/* The name is UTF-16, and ASCII is written one byte per unit. Bounded
	 * at 35 so the 36th unit stays the terminator. */
	for (i = 0; i < 35 && name[i]; i++)
		e[GPT_ENT_NAME + i * 2] = (u8)name[i];
}

static void build_header(u8 *sector, const struct table *t, u32 block_size,
			 u64 my_lba, u64 alt_lba, u64 entry_lba,
			 u32 entries_crc)
{
	kmemset(sector, 0, block_size);

	kmemcpy(sector + GPT_HDR_SIG, "EFI PART", 8);
	put32(sector + GPT_HDR_REVISION, 0x00010000);
	put32(sector + GPT_HDR_SIZE, 92);
	put64(sector + GPT_HDR_MY_LBA, my_lba);
	put64(sector + GPT_HDR_ALT_LBA, alt_lba);
	put64(sector + GPT_HDR_FIRST_USABLE, t->first_usable);
	put64(sector + GPT_HDR_LAST_USABLE, t->last_usable);
	kmemcpy(sector + GPT_HDR_DISK_GUID, t->disk_guid, 16);
	put64(sector + GPT_HDR_ENTRY_LBA, entry_lba);
	put32(sector + GPT_HDR_ENTRY_COUNT, t->entry_count);
	put32(sector + GPT_HDR_ENTRY_SIZE, t->entry_size);
	put32(sector + GPT_HDR_ENTRY_CRC, entries_crc);

	/* The header's own checksum is computed with its checksum field zero,
	 * over exactly the declared header size and not the whole sector --
	 * both of which are easy to get wrong and produce a table that every
	 * tool rejects for a reason none of them explains. */
	put32(sector + GPT_HDR_CRC, 0);
	put32(sector + GPT_HDR_CRC, crc32(sector, 92));
}

enum install_verdict install_write_gpt(struct block_device *disk,
				       const struct install_plan *plan)
{
	struct table t;
	enum install_verdict v;
	u8 *sector = 0;
	u32 per, sectors, entries_crc;
	u64 backup_entry_lba;

	if (plan->verdict != INSTALL_OK)
		return plan->verdict;

	if (block_claim_raw(disk) != BLOCK_OK)
		return INSTALL_IO;

	v = load_table(disk, plan->fresh_table, &t);
	if (v != INSTALL_OK)
		goto release;

	sector = kzalloc(disk->block_size);
	if (!sector) {
		v = INSTALL_IO;
		goto out;
	}

	/* The entries this install adds go into free slots. Nothing already in
	 * the array is read, rewritten or even looked at beyond finding out
	 * that it is occupied. */
	{
		unsigned index;
		u8 *e;

		if (!plan->reuse_esp) {
			e = free_slot(&t, &index);
			if (!e) {
				v = INSTALL_TOO_MANY_SLICES;
				goto out;
			}
			fill_entry(disk, e, type_esp, plan->esp.first_lba,
				   plan->esp.count, "ReconOS Boot");
		}

		e = free_slot(&t, &index);
		if (!e) {
			v = INSTALL_TOO_MANY_SLICES;
			goto out;
		}
		fill_entry(disk, e, type_reconos, plan->system.first_lba,
			   plan->system.count, "ReconOS System");

		/* And the programs volume, which this used to leave out.
		 *
		 * The plan had three partitions and the table got two, so the
		 * executor went on to format a volume that no entry pointed at:
		 * a ReconFS filesystem sitting in space the table calls free,
		 * waiting for the next thing that allocates to write over it.
		 * Caught by the test printing how many partitions the table
		 * ended up with rather than only whether sgdisk liked it. */
		e = free_slot(&t, &index);
		if (!e) {
			v = INSTALL_TOO_MANY_SLICES;
			goto out;
		}
		fill_entry(disk, e, type_reconos, plan->programs.first_lba,
			   plan->programs.count, "ReconOS Programs");
	}

	entries_crc = crc32(t.entries, t.entries_bytes);

	per = disk->block_size / t.entry_size;
	sectors = (t.entry_count + per - 1) / per;

	/* The backup array sits immediately below the backup header, which is
	 * the last block of the device. */
	backup_entry_lba = disk->block_count - 1 - sectors;

	if (plan->fresh_table) {
		protective_mbr(sector, disk->block_count);
		if (block_write(disk, 0, 1, sector) != BLOCK_OK) {
			v = INSTALL_IO;
			goto out;
		}
	}

	/* (1) primary entries, (2) primary header. A cut between them leaves a
	 * primary whose checksum does not match, so every tool falls back to
	 * the backup -- the old table, still describing the disk as it was. */
	if (block_write(disk, t.entry_lba, sectors, t.entries) != BLOCK_OK) {
		v = INSTALL_IO;
		goto out;
	}

	build_header(sector, &t, disk->block_size, 1, disk->block_count - 1,
		     t.entry_lba, entries_crc);
	if (block_write(disk, 1, 1, sector) != BLOCK_OK) {
		v = INSTALL_IO;
		goto out;
	}

	if (block_flush(disk) != BLOCK_OK) {
		v = INSTALL_IO;
		goto out;
	}

	/* (4) backup entries, (5) backup header. */
	if (block_write(disk, backup_entry_lba, sectors, t.entries) != BLOCK_OK) {
		v = INSTALL_IO;
		goto out;
	}

	build_header(sector, &t, disk->block_size, disk->block_count - 1, 1,
		     backup_entry_lba, entries_crc);
	if (block_write(disk, disk->block_count - 1, 1, sector) != BLOCK_OK) {
		v = INSTALL_IO;
		goto out;
	}

	v = block_flush(disk) == BLOCK_OK ? INSTALL_OK : INSTALL_IO;

out:
	kfree(sector);
	free_table(&t);
release:
	block_release_raw(disk);
	return v;
}
