/* Writing the BIOS boot path onto a disk.
 *
 * The UEFI half of an install is files on a filesystem: firmware knows how to
 * read FAT32, and putting `BOOTX64.EFI` in the right directory is the whole
 * job. A BIOS knows nothing. It reads the first sector of the disk and jumps
 * to it, so the BIOS half of an install is **two raw writes to specific
 * blocks** -- and one of them is the sector that also holds the partition
 * table.
 *
 * That makes this the most dangerous code in the installer, and it is written
 * to be read with that in mind:
 *
 *   - the first sector is read, modified in place, and written back. Only
 *     bytes 0 to 439 change. The partition table at 446 and the signature at
 *     510 are the disk's own and are never touched, because overwriting them
 *     turns a dual-boot machine into an empty one.
 *
 *   - stage 2 goes into a **partition**, not into the gap after the MBR. A
 *     partition entry is how a disk says "occupied"; unallocated space is
 *     space the next tool along is entitled to take.
 *
 *   - the block number of stage 2 is patched into stage 1, and the offsets to
 *     patch are read out of stage 1 itself rather than hard-coded here. Stage 1
 *     carries a small descriptor saying where its two numbers live, because an
 *     offset written down in this file would be correct until somebody edited
 *     stage1.S -- and then this would write a sector count over something else,
 *     silently, on a stranger's disk.
 */
#include <recon/kernel/block.h>
#include <recon/kernel/console.h>
#include <recon/kernel/fat32.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/install.h>
#include <recon/kernel/kstring.h>

/* Where stage 1 says its patchable numbers are: a magic word and two offsets,
 * at a fixed place near the end of the 440 bytes. See boot/bios/stage1.S. */
#define PATCH_INFO_OFF	432
#define PATCH_MAGIC	0x3153		/* "S1" */

#define MBR_CODE_BYTES	440
#define STAGE2_MAX	(64u * 1024u)

static u16 rd16(const u8 *p)
{
	return (u16)(p[0] | (p[1] << 8));
}

static void wr16(u8 *p, u16 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
}

static void wr64(u8 *p, u64 v)
{
	unsigned i;

	for (i = 0; i < 8; i++)
		p[i] = (u8)(v >> (i * 8));
}

/* The registered slice covering exactly this extent, or none.
 *
 * Matched on first block and length rather than on index: the table this
 * install just wrote put entries in whatever slots were free, so an index from
 * before the write is a number that no longer means what it did.
 *
 * Unlike the installer's other lookup this one does not register the slice if
 * it is missing. A partition that was written to the table but never appeared
 * as a device means something went wrong between the two, and inventing it here
 * would paper over that with a write to a device nobody had checked.
 */
static struct block_device *slice_matching(struct block_device *disk,
					   const struct block_extent *e)
{
	unsigned i;

	for (i = 0; i < block_device_count(); i++) {
		struct block_device *d = block_device_at(i);

		if (d->parent == disk->id && d->first_lba == e->first_lba &&
		    d->block_count == e->count)
			return d;
	}

	return 0;
}

/* Reads one file off the medium. Absent is reported separately from broken,
 * because "this medium has no BIOS loader on it" and "the medium is damaged"
 * call for different things from whoever is standing there. */
static enum fat32_status read_one(struct fat32 *fs, const char *path,
				  u8 *buf, u32 max, u32 *got)
{
	struct fat32_entry e;
	enum fat32_status st;

	st = fat32_walk(fs, path, &e);
	if (st != FAT32_OK)
		return st;

	if (e.is_dir)
		return FAT32_ERR_NOT_FOUND;

	return fat32_read_file(fs, &e, buf, max, got);
}

enum install_verdict install_write_bios_boot(struct block_device *source_esp,
					     struct block_device *disk,
					     const struct install_plan *plan)
{
	struct fat32 src;
	u8 *stage1 = 0, *stage2 = 0, *first = 0;
	u32 s1_len = 0, s2_len = 0;
	u32 count_off, lba_off, sectors;
	enum install_verdict v = INSTALL_OK;
	enum fat32_status st;

	if (!plan->bios_boot.count) {
		/* No partition for it. Not a failure: a machine whose firmware
		 * is UEFI has no use for this, and there was nowhere to put it
		 * without moving something that was already there. */
		kputs("  BIOS loader        : not installed (no room for a "
		      "BIOS boot partition)\n");
		return INSTALL_OK;
	}

	if (fat32_mount(source_esp, &src) != FAT32_OK) {
		kputs("  BIOS loader        : FAILED (the medium's EFI "
		      "partition could not be read)\n");
		return INSTALL_IO;
	}

	stage1 = kmalloc(MBR_CODE_BYTES);
	stage2 = kmalloc(STAGE2_MAX);
	first  = kmalloc(disk->block_size);

	if (!stage1 || !stage2 || !first) {
		v = INSTALL_IO;
		goto out;
	}

	st = read_one(&src, "/reconos/stage1.bin", stage1, MBR_CODE_BYTES,
		      &s1_len);
	if (st == FAT32_ERR_NOT_FOUND) {
		/* The medium carries no BIOS loader. Said plainly and not
		 * treated as an error: a UEFI-only medium is a legitimate
		 * thing to have made. */
		kputs("  BIOS loader        : not on this medium\n");
		goto out;
	}
	if (st != FAT32_OK || s1_len != MBR_CODE_BYTES) {
		kprintf("  BIOS loader        : FAILED (stage 1 is %u bytes, "
			"not %u)\n", s1_len, MBR_CODE_BYTES);
		v = INSTALL_IO;
		goto out;
	}

	st = read_one(&src, "/reconos/stage2.bin", stage2, STAGE2_MAX, &s2_len);
	if (st != FAT32_OK || !s2_len) {
		kputs("  BIOS loader        : FAILED (stage 1 is there and "
		      "stage 2 is not)\n");
		v = INSTALL_IO;
		goto out;
	}

	sectors = (s2_len + disk->block_size - 1) / disk->block_size;

	if (sectors > plan->bios_boot.count) {
		kprintf("  BIOS loader        : FAILED (stage 2 needs %u "
			"blocks, the partition is %llu)\n",
			sectors, (unsigned long long)plan->bios_boot.count);
		v = INSTALL_IO;
		goto out;
	}

	/* A single BIOS extended read fetches at most 127 sectors on machines
	 * that can be relied on. Checked here, where there is a person to tell,
	 * rather than discovered as a machine that stops. */
	if (sectors > 127) {
		kprintf("  BIOS loader        : FAILED (stage 2 is %u blocks; "
			"one BIOS read fetches 127)\n", sectors);
		v = INSTALL_IO;
		goto out;
	}

	/* --- patch stage 1 with where stage 2 went ------------------------ */

	if (rd16(stage1 + PATCH_INFO_OFF) != PATCH_MAGIC) {
		/* Without the descriptor there is no safe place to write the
		 * block number, and writing it at a guessed offset would
		 * corrupt whatever is really there. */
		kputs("  BIOS loader        : FAILED (stage 1 does not say "
		      "where its numbers are)\n");
		v = INSTALL_IO;
		goto out;
	}

	count_off = rd16(stage1 + PATCH_INFO_OFF + 2);
	lba_off   = rd16(stage1 + PATCH_INFO_OFF + 4);

	if (count_off + 2 > MBR_CODE_BYTES || lba_off + 8 > MBR_CODE_BYTES) {
		kputs("  BIOS loader        : FAILED (stage 1's offsets point "
		      "outside itself)\n");
		v = INSTALL_IO;
		goto out;
	}

	wr16(stage1 + count_off, (u16)sectors);
	wr64(stage1 + lba_off, plan->bios_boot.first_lba);

	/* --- stage 2 first, then the sector that points at it ------------- */
	//
	// This order matters on a machine that loses power in the middle. A
	// first sector that points at a stage 2 which is not there yet is a
	// disk that does not boot; stage 2 sitting in a partition nothing
	// points at is a disk that boots exactly as it did before.

	/* Through the partition, not through the disk.
	 *
	 * The block layer refuses a write to a whole device that has slices on
	 * it unless the caller has claimed it raw, and that refusal is right:
	 * a write through the parent that is off by a partition lands in the
	 * middle of somebody's filesystem and reports success. The BIOS boot
	 * partition is a partition -- so it is written as one, bounded by its
	 * own extent, where an arithmetic mistake cannot reach a neighbour.
	 */
	{
		struct block_device *part = slice_matching(disk,
							   &plan->bios_boot);

		if (!part) {
			kputs("  BIOS loader        : FAILED (the BIOS boot "
			      "partition is not registered)\n");
			v = INSTALL_IO;
			goto out;
		}

		if (block_write(part, 0, sectors, stage2) != BLOCK_OK) {
			kputs("  BIOS loader        : FAILED (writing stage "
			      "2)\n");
			v = INSTALL_IO;
			goto out;
		}
	}

	/* The first sector is the one place this genuinely needs the whole
	 * device, because the boot code and the partition table share it. So
	 * the disk is claimed, out loud and briefly, which is exactly the
	 * question the block layer exists to make somebody ask. */
	if (block_claim_raw(disk) != BLOCK_OK) {
		kputs("  BIOS loader        : FAILED (the disk is busy)\n");
		v = INSTALL_IO;
		goto out;
	}

	if (block_read(disk, 0, 1, first) != BLOCK_OK) {
		block_release_raw(disk);
		kputs("  BIOS loader        : FAILED (reading the first "
		      "sector)\n");
		v = INSTALL_IO;
		goto out;
	}

	/* **Only the boot code.** The partition table at 446 and the 0xAA55 at
	 * 510 belong to the disk, and this install is a guest on it. */
	kmemcpy(first, stage1, MBR_CODE_BYTES);

	if (block_write(disk, 0, 1, first) != BLOCK_OK) {
		block_release_raw(disk);
		kputs("  BIOS loader        : FAILED (writing the first "
		      "sector)\n");
		v = INSTALL_IO;
		goto out;
	}

	block_release_raw(disk);

	kprintf("  BIOS loader        : stage 1 in the first sector, stage 2 "
		"at block %llu (%u blocks)\n",
		(unsigned long long)plan->bios_boot.first_lba, sectors);

out:
	kfree(stage1);
	kfree(stage2);
	kfree(first);
	return v;
}
