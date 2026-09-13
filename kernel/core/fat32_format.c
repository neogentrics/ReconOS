/* Making a FAT32 filesystem, rather than writing into one somebody else made.
 *
 * The installer needs this for one reason: a machine with no EFI System
 * Partition needs one, and an ESP is FAT32 by specification. Nothing else in
 * ReconOS wants a FAT volume -- our own filesystem is ReconFS -- so this is
 * deliberately the smallest formatter that produces something firmware and
 * everybody else's tools will accept, and not a general mkfs.
 *
 * --- What makes a volume FAT32, and what does not ---------------------------
 *
 * There is no flag. A volume is FAT32 because it has **more than 65,524 data
 * clusters**, and for no other reason: the string at offset 82 saying "FAT32"
 * is a comment that Microsoft's own specification forbids anybody to trust, and
 * a volume that says FAT32 while having 60,000 clusters is a FAT16 volume that
 * every correct reader will treat as FAT16.
 *
 * So the cluster size is not chosen for tidiness. It is chosen so that the
 * count lands on the right side of that boundary, and then checked, because
 * getting it wrong produces a volume that mounts perfectly on the tool that
 * made it and is unreadable everywhere else.
 *
 * --- The chicken and egg in the FAT's own size ------------------------------
 *
 * How many sectors the allocation table needs depends on how many clusters
 * there are; how many clusters there are depends on how much room is left after
 * the table. The specification resolves this with an approximation that
 * over-estimates slightly, and everybody uses it, so this does too -- being
 * generous here wastes a few sectors and being clever here produces a table one
 * entry short of the volume it describes.
 */
#include <recon/kernel/fat32.h>

#include <recon/kernel/block.h>
#include <recon/kernel/console.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/kstring.h>

#include "fat32_internal.h"

#define RESERVED_SECTORS	32	/* what every FAT32 formatter uses */
#define BACKUP_BOOT_SECTOR	6	/* where the specification puts the copy */
#define FSINFO_SECTOR		1

/* 256 KiB at a time. Large enough that the call count stops mattering,
 * small enough that the allocation is not itself a reason to fail. */
#define ZERO_CHUNK_BYTES	(256u * 1024u)

/* Cluster size by volume size, which is Microsoft's own table. The point of
 * following it rather than inventing one is that these are the values every
 * other implementation has been tested against for thirty years. */
static u32 clusters_for(u64 sectors)
{
	if (sectors <= 532480ull)	/* up to 260 MB */
		return 1;
	if (sectors <= 16777216ull)	/* up to 8 GB */
		return 8;
	if (sectors <= 33554432ull)	/* up to 16 GB */
		return 16;
	if (sectors <= 67108864ull)	/* up to 32 GB */
		return 32;
	return 64;
}

/* The specification's approximation for the table's size. Deliberately not
 * improved: it over-estimates by a sector or two, and the exact version has a
 * boundary case that yields a table one entry short of the volume. */
static u32 fat_size_for(u64 total, u32 per_cluster, u32 bytes_per_sector)
{
	u64 usable = total - RESERVED_SECTORS;
	u64 denom = ((u64)(bytes_per_sector / 4) * per_cluster) + 2;

	return (u32)((usable + denom - 1) / denom);
}

enum fat32_status fat32_format(struct block_device *dev, const char *label)
{
	u64 total = dev->block_count;
	u32 bps = dev->block_size;
	u32 spc = clusters_for(total);
	u32 fat_sectors;
	u64 data_sectors, clusters;
	u8 *buf, *zeros;
	u32 zero_sectors;
	enum fat32_status st = FAT32_ERR_IO;
	unsigned i;

	if (bps < 512 || (bps & (bps - 1)))
		return FAT32_ERR_UNSUPPORTED;

	/* Grow the cluster until the table fits and the count is above the
	 * boundary. Two ways to fail, and they are different failures: too few
	 * clusters means this cannot be a FAT32 volume at all, and no cluster
	 * size fixes a volume that is simply too small. */
	for (;;) {
		fat_sectors = fat_size_for(total, spc, bps);
		data_sectors = total - RESERVED_SECTORS - (u64)fat_sectors * 2;
		clusters = data_sectors / spc;

		if (clusters > FAT16_MAX_CLUSTERS)
			break;

		if (spc >= 64)
			return FAT32_ERR_TOO_SMALL_FOR_FAT32;

		/* A *smaller* cluster gives more of them, which is the
		 * direction that reaches the boundary. */
		if (spc == 1)
			return FAT32_ERR_TOO_SMALL_FOR_FAT32;
		spc /= 2;
	}

	if (clusters + FAT32_CLUSTER_FIRST > 0x0FFFFFF6ull)
		return FAT32_ERR_TOO_LARGE;

	buf = kzalloc(bps);
	if (!buf)
		return FAT32_ERR_NOMEM;

	/* A chunk to clear with, rather than a sector.
	 *
	 * The first version wrote the allocation table one sector at a time --
	 * on a 16 GB volume that is 65,536 write calls to lay down 32 MB of
	 * zeroes, and it took long enough to look like a hang. That is the
	 * *third* time this shape has been written in this project in one day:
	 * a loop over items in a structure that is stored per block, whose cost
	 * is invisible against a file in memory and ruinous on real media.
	 *
	 * A small failure to allocate is not fatal here -- the sector-at-a-time
	 * path still works, and being slow is better than refusing to install. */
	zeros = kzalloc(ZERO_CHUNK_BYTES);
	zero_sectors = zeros ? ZERO_CHUNK_BYTES / bps : 1;

	/* --- the boot sector ------------------------------------------------ */

	kmemset(buf, 0, bps);

	/* A jump instruction, because some firmware checks for one before it
	 * will believe this is a boot sector at all. It jumps over the BPB to
	 * a byte that is not code, which is correct: nothing boots from this
	 * sector, the EFI loader is a file. */
	buf[0] = 0xEB;
	buf[1] = 0x58;
	buf[2] = 0x90;
	kmemcpy(buf + 3, "RECONOS ", 8);		/* OEM name */

	fat_put16(buf + FAT_BPB_BYTS_PER_SEC, (u16)bps);
	buf[FAT_BPB_SEC_PER_CLUS] = (u8)spc;
	fat_put16(buf + FAT_BPB_RSVD_SEC_CNT, RESERVED_SECTORS);
	buf[FAT_BPB_NUM_FATS] = 2;
	fat_put16(buf + FAT_BPB_ROOT_ENT_CNT, 0);	/* zero on FAT32 */
	fat_put16(buf + FAT_BPB_TOT_SEC16, 0);
	buf[FAT_BPB_MEDIA] = 0xF8;			/* "fixed disk" */
	fat_put16(buf + FAT_BPB_FAT_SZ16, 0);
	fat_put16(buf + 24, 63);			/* sectors per track */
	fat_put16(buf + 26, 255);			/* heads */
	fat_put32(buf + 28, 0);				/* hidden sectors */
	fat_put32(buf + FAT_BPB_TOT_SEC32,
		  total > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)total);
	fat_put32(buf + FAT_BPB_FAT_SZ32, fat_sectors);
	fat_put16(buf + FAT_BPB_EXT_FLAGS, 0);		/* both FATs live */
	fat_put16(buf + FAT_BPB_FS_VER, 0);
	fat_put32(buf + FAT_BPB_ROOT_CLUS, FAT32_CLUSTER_FIRST);
	fat_put16(buf + FAT_BPB_FS_INFO, FSINFO_SECTOR);
	fat_put16(buf + 50, BACKUP_BOOT_SECTOR);

	buf[64] = 0x80;					/* drive number */
	buf[66] = 0x29;					/* extended signature */
	fat_put32(buf + 67, 0x5245434Fu);		/* volume serial */

	for (i = 0; i < 11; i++)
		buf[71 + i] = (u8)(label && label[i] ? label[i] : ' ');

	/* The string the specification says not to trust. Written anyway,
	 * because everything writes it and a tool that shows it to a person
	 * should show the right thing -- it is a label, not a fact. */
	kmemcpy(buf + 82, "FAT32   ", 8);

	buf[510] = 0x55;
	buf[511] = 0xAA;

	if (block_write(dev, 0, 1, buf) != BLOCK_OK)
		goto out;
	if (block_write(dev, BACKUP_BOOT_SECTOR, 1, buf) != BLOCK_OK)
		goto out;

	/* --- FSInfo -------------------------------------------------------- */

	kmemset(buf, 0, bps);
	fat_put32(buf + 0, 0x41615252);			/* lead signature */
	fat_put32(buf + 484, 0x61417272);		/* struct signature */

	/* The free count and the next-free hint are *hints*, and the
	 * specification permits them to be wrong. Written as "unknown" rather
	 * than computed: a hint that is confidently wrong is worse than one
	 * that admits it, and every reader is required to cope with this. */
	fat_put32(buf + 488, 0xFFFFFFFFu);
	fat_put32(buf + 492, 0xFFFFFFFFu);
	fat_put32(buf + 508, 0xAA550000u);

	if (block_write(dev, FSINFO_SECTOR, 1, buf) != BLOCK_OK)
		goto out;
	if (block_write(dev, BACKUP_BOOT_SECTOR + FSINFO_SECTOR, 1, buf) !=
	    BLOCK_OK)
		goto out;

	/* --- the allocation tables ------------------------------------------ */

	kmemset(buf, 0, bps);

	for (i = 0; i < 2; i++) {
		u64 at = RESERVED_SECTORS + (u64)i * fat_sectors;
		u32 s;

		/* Entry 0 is the media byte with the rest set; entry 1 is the
		 * end-of-chain marker with the two housekeeping bits. Entry 2
		 * is the root directory, one cluster, ending immediately. */
		kmemset(buf, 0, bps);
		fat_put32(buf + 0, 0x0FFFFFF8u);
		fat_put32(buf + 4, 0xFFFFFFFFu);
		fat_put32(buf + 8, 0x0FFFFFFFu);

		if (block_write(dev, at, 1, buf) != BLOCK_OK)
			goto out;

		for (s = 1; s < fat_sectors; ) {
			u32 n = fat_sectors - s;

			if (n > zero_sectors)
				n = zero_sectors;

			if (block_write(dev, at + s, n,
					zeros ? zeros : buf) != BLOCK_OK)
				goto out;

			s += n;
		}
	}

	/* --- the root directory --------------------------------------------- */

	kmemset(buf, 0, bps);

	{
		u64 root = RESERVED_SECTORS + (u64)fat_sectors * 2;
		u32 s;

		/* Zeroed, every sector of it. An unzeroed directory cluster is
		 * read as entries, and whatever was on this partition before
		 * becomes files -- with names and sizes, pointing at clusters
		 * the table says are free. */
		for (s = 0; s < spc; ) {
			u32 n = spc - s;

			if (n > zero_sectors)
				n = zero_sectors;

			if (block_write(dev, root + s, n,
					zeros ? zeros : buf) != BLOCK_OK)
				goto out;

			s += n;
		}
	}

	st = block_flush(dev) == BLOCK_OK ? FAT32_OK : FAT32_ERR_IO;
out:
	kfree(zeros);
	kfree(buf);
	return st;
}
