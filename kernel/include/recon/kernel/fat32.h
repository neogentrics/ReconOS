/* FAT32 -- somebody else's filesystem, read as carefully as our own.
 *
 * This is the first foreign format the kernel understands, and it is required
 * rather than optional: the UEFI System Partition is FAT32 by specification,
 * and that is where our own bootloader has to be written. An installer that
 * cannot write a file into a FAT32 volume cannot make a machine boot.
 *
 * --- Why this is a different job from ReconFS -------------------------------
 *
 * ReconFS is ours. Its on-disk format is frozen by us, every volume that exists
 * was written by this code, and when the checker disagrees with the tree one of
 * the two is our bug.
 *
 * FAT32 volumes were written by somebody else -- Windows, macOS, a camera, a
 * firmware update tool from 2004 -- and they disagree with each other about the
 * parts of the specification that are ambiguous, which is most of the
 * interesting parts. The rule here is therefore the opposite of the rule there:
 *
 *   **Believe nothing that can be derived, and derive everything that can be.**
 *
 * The two places that matters most:
 *
 *   1. The type. A volume does not say whether it is FAT12, FAT16 or FAT32 in
 *      any field you may trust. There is a `BS_FilSysType` string that reads
 *      "FAT32   ", and Microsoft's own specification says in as many words that
 *      it is *not* to be used to determine the type -- it is a comment, and
 *      formatters write whatever they like in it. The type is the **count of
 *      data clusters** and nothing else. Under 4085 is FAT12, under 65525 is
 *      FAT16, and at or above that FAT32. Those boundaries are exact and
 *      off-by-one there means reading the wrong width of FAT entry from the
 *      right place, which produces a plausible, wrong answer rather than an
 *      error.
 *
 *   2. The sector size. `BPB_BytsPerSec` is 512 on almost everything and is not
 *      512 on some 4Kn media, and it does not have to equal the block device's
 *      sector size. Both are read; neither is assumed.
 *
 * --- What this refuses -------------------------------------------------------
 *
 * A volume that is FAT12 or FAT16 is *refused*, not read as FAT32. A volume
 * whose two FATs disagree is refused rather than silently preferring the first.
 * A file larger than the caller's buffer is refused rather than truncated,
 * which is this project's standing rule: a caller handed half a file with a
 * success status cannot tell.
 *
 * --- Names -------------------------------------------------------------------
 *
 * Long names are read, not just the 8.3 aliases. That is not a nicety. Our own
 * ESP holds `kernel-x86_64.elf`, whose alias is `KERNEL~1.ELF`, and an
 * installer that could only see aliases would be unable to tell one kernel from
 * another on a volume it had written itself. The alias is also *not* stable --
 * it depends on what else is in the directory and in what order it was created.
 */
#ifndef RECON_KERNEL_FAT32_H
#define RECON_KERNEL_FAT32_H

#include <recon/kernel/block.h>
#include <recon/kernel/types.h>

/* Offsets into the boot sector, by name, because a struct with a designed
 * layout is a promise the compiler makes and this is a promise somebody else's
 * formatter made. Every one is little-endian regardless of the machine. */
#define FAT_BPB_BYTS_PER_SEC   11	/* u16 */
#define FAT_BPB_SEC_PER_CLUS   13	/* u8  */
#define FAT_BPB_RSVD_SEC_CNT   14	/* u16 */
#define FAT_BPB_NUM_FATS       16	/* u8  */
#define FAT_BPB_ROOT_ENT_CNT   17	/* u16 -- zero on FAT32 */
#define FAT_BPB_TOT_SEC16      19	/* u16 */
#define FAT_BPB_MEDIA          21	/* u8  */
#define FAT_BPB_FAT_SZ16       22	/* u16 -- zero on FAT32 */
#define FAT_BPB_TOT_SEC32      32	/* u32 */
#define FAT_BPB_FAT_SZ32       36	/* u32 */
#define FAT_BPB_EXT_FLAGS      40	/* u16 */
#define FAT_BPB_FS_VER         42	/* u16 */
#define FAT_BPB_ROOT_CLUS      44	/* u32 */
#define FAT_BPB_FS_INFO        48	/* u16 */
#define FAT_BOOT_SIG           510	/* u16, 0xAA55 */

/* A directory entry is 32 bytes, and these are the offsets within it. */
#define FAT_DIR_NAME           0	/* 11 bytes, 8.3, space padded */
#define FAT_DIR_ATTR           11	/* u8  */
#define FAT_DIR_NTRES          12	/* u8  -- lower-case hints */
#define FAT_DIR_FST_CLUS_HI    20	/* u16 */
#define FAT_DIR_WRT_TIME       22	/* u16 */
#define FAT_DIR_WRT_DATE       24	/* u16 */
#define FAT_DIR_FST_CLUS_LO    26	/* u16 */
#define FAT_DIR_FILE_SIZE      28	/* u32 */
#define FAT_DIR_SIZE           32

#define FAT_ATTR_READ_ONLY     0x01
#define FAT_ATTR_HIDDEN        0x02
#define FAT_ATTR_SYSTEM        0x04
#define FAT_ATTR_VOLUME_ID     0x08
#define FAT_ATTR_DIRECTORY     0x10
#define FAT_ATTR_ARCHIVE       0x20

/* The four attribute bits that mark a long-name fragment rather than an entry.
 * Deliberately the same value as READ_ONLY|HIDDEN|SYSTEM|VOLUME_ID: it was
 * chosen so that systems predating long names would skip these entries as
 * volume labels they did not understand. */
#define FAT_ATTR_LONG_NAME     0x0F
#define FAT_ATTR_LONG_MASK     0x3F

#define FAT_ENTRY_FREE         0x00	/* this entry and every one after it */
#define FAT_ENTRY_DELETED      0xE5

/* Cluster values with meanings rather than positions. */
#define FAT32_CLUSTER_MASK     0x0FFFFFFFu	/* the top four bits are reserved */
#define FAT32_CLUSTER_FREE     0x00000000u
#define FAT32_CLUSTER_BAD      0x0FFFFFF7u
#define FAT32_CLUSTER_EOC      0x0FFFFFF8u	/* >= this ends a chain */
#define FAT32_CLUSTER_FIRST    2		/* 0 and 1 are not data */

/* The exact boundaries that decide the type. Not approximate, and not the
 * string in the boot sector. */
#define FAT12_MAX_CLUSTERS     4084
#define FAT16_MAX_CLUSTERS     65524

/* A long name may be 255 characters. Stored here as UTF-8, which is longer in
 * bytes than in characters -- three bytes per character covers the Basic
 * Multilingual Plane, and a surrogate pair becomes four bytes for two units. */
#define FAT_NAME_MAX           768

enum fat32_status {
	FAT32_OK = 0,
	FAT32_ERR_IO,
	FAT32_ERR_NOT_FAT,	/* no boot signature, or a nonsense BPB */
	FAT32_ERR_WRONG_TYPE,	/* it is FAT12 or FAT16, and this reads FAT32 */
	FAT32_ERR_FATS_DIFFER,	/* the copies disagree; which is right is unknown */
	FAT32_ERR_NOT_FOUND,
	FAT32_ERR_NAME,
	FAT32_ERR_TOO_LARGE,	/* the caller's buffer is smaller than the file */
	FAT32_ERR_CORRUPT,	/* a chain that loops, or leaves the volume */
	FAT32_ERR_NOMEM,
	FAT32_ERR_UNSUPPORTED,
};

const char *fat32_strerror(enum fat32_status s);

struct fat32 {
	struct block_device *dev;

	u32 bytes_per_sector;	/* the volume's, which need not be the device's */
	u32 sectors_per_cluster;
	u32 bytes_per_cluster;

	u64 fat_start;		/* in volume sectors */
	u32 fat_sectors;	/* per copy */
	u32 fat_count;

	u64 data_start;		/* in volume sectors */
	u32 data_clusters;	/* the number the type is decided by */

	u32 root_cluster;

	/* The device's sector size, kept because every read has to be expressed
	 * in the device's units and the volume speaks its own. */
	u32 dev_sector_size;
};

/* One directory entry, after long-name fragments have been gathered. */
struct fat32_entry {
	char name[FAT_NAME_MAX];	/* the long name if there is one */
	char alias[13];			/* the 8.3 name, always */
	u32 first_cluster;
	u32 size;
	u8  attr;
	bool is_dir;
};

enum fat32_status fat32_mount(struct block_device *dev, struct fat32 *fs);

/* Reads one entry of a directory by index, skipping the machinery: long-name
 * fragments, deleted entries and the volume label. `index` counts *entries a
 * person would see*, not 32-byte records.
 *
 * Returns FAT32_ERR_NOT_FOUND once the directory is exhausted, which is how a
 * caller knows to stop rather than by being told a count first. */
enum fat32_status fat32_readdir(struct fat32 *fs, u32 dir_cluster, u32 index,
				struct fat32_entry *out);

/* Finds one name in one directory. Case-insensitive, because FAT is, and
 * matching either the long name or the 8.3 alias -- a caller that only knows
 * `KERNEL~1.ELF` should still find the file. */
enum fat32_status fat32_lookup(struct fat32 *fs, u32 dir_cluster,
			       const char *name, struct fat32_entry *out);

/* Walks a `/`-separated path from the root. */
enum fat32_status fat32_walk(struct fat32 *fs, const char *path,
			     struct fat32_entry *out);

/* Reads a whole file. A file larger than `max` is refused rather than partly
 * returned, for the same reason ReconFS refuses one: a caller handed half a
 * file with a success status has no way to know. */
enum fat32_status fat32_read_file(struct fat32 *fs,
				  const struct fat32_entry *entry,
				  void *out, u32 max, u32 *got);

/* Follows a cluster chain one link at a time. Exposed because the self-test
 * checks chains directly, and because a caller streaming a large file should
 * not have to hold it all. */
enum fat32_status fat32_next_cluster(struct fat32 *fs, u32 cluster, u32 *next);

/* --- Writing ---------------------------------------------------------------
 *
 * This is the half that touches somebody else's disk, and the ESP it writes to
 * is usually the one the machine boots from. Two rules shape all of it.
 *
 * **FAT has no atomicity, so the ordering is the whole design.** A file is made
 * real in this order, and no other:
 *
 *   1. find free clusters and chain them in *both* allocation tables;
 *   2. write the file's bytes into them;
 *   3. flush;
 *   4. write the directory entry that points at the chain;
 *   5. flush.
 *
 * A crash before (4) leaves clusters marked in use that nothing refers to. That
 * is a *leak* -- space lost until a repair tool reclaims it -- and it is not
 * corruption: every file that existed still reads correctly.
 *
 * The opposite order is the one that destroys data. A directory entry written
 * first points at clusters that have not been filled yet, so a crash leaves a
 * file that looks entirely valid and contains whatever was there before. On an
 * ESP that is a bootloader made of somebody else's deleted files, and the
 * firmware will run it.
 *
 * **Both tables are written, always.** The reader refuses a volume whose copies
 * disagree, so a writer that updated one would produce volumes its own reader
 * rejects -- which is a useful property to have on purpose: the two halves keep
 * each other honest rather than sharing an assumption.
 *
 * The FSInfo sector's free-count is a *hint* the specification permits to be
 * wrong. It is set to "unknown" rather than recomputed, because a hint that is
 * confidently wrong is worse than one that admits it, and recomputing it means
 * reading the whole table to save somebody else a search.
 */

/* Creates or replaces a file in one directory, contents and all.
 *
 * Whole-file only, like ReconFS: the callers that matter -- an installer
 * writing a bootloader, a recovery environment restoring one -- have the bytes
 * in memory already, and a partial-write interface would need a file handle,
 * a seek position and a truncate, none of which anything here wants yet.
 *
 * Replacing an existing name frees its old chain *after* the new entry is in
 * place, so a crash in the middle leaves one of the two files whole rather than
 * neither.
 */
enum fat32_status fat32_write_named(struct fat32 *fs, u32 dir_cluster,
				    const char *name, const void *data,
				    u32 len);

/* Creates a directory, and the `.` and `..` entries inside it that FAT expects
 * every directory except the root to have. */
enum fat32_status fat32_mkdir(struct fat32 *fs, u32 dir_cluster,
			      const char *name, u32 *out_cluster);

/* Makes every component of a `/`-separated path exist, and returns the cluster
 * of the last one. `\EFI\BOOT` is two directories, and an installer should not
 * have to make them one at a time. */
enum fat32_status fat32_mkpath(struct fat32 *fs, const char *path,
			       u32 *out_cluster);

/* How many clusters are free. Counted, not read from the FSInfo hint, because
 * the hint is allowed to be wrong and a caller asking this wants the answer. */
enum fat32_status fat32_free_clusters(struct fat32 *fs, u32 *out);

/* Mounts whatever FAT32 volumes the block layer can see and reads from them,
 * comparing against content this kernel did not write. Runs when
 * `fat32=<device>` names one on the command line. Reads only; writes nothing. */
void fat32_run(void);

/* Writes a handful of files for somebody else's tools to read back, and
 * reports what it managed. Runs when `fat32-write=<device>` names one,
 * because it destroys what is there. */
void fat32_write_run(void);

#endif /* RECON_KERNEL_FAT32_H */
