/* ext2 -- reading the filesystem Linux leaves on a disk, checking it, and
 * repairing what the checker names.
 *
 * --- Why this is here at all ---
 *
 * The installer's promise is that ReconOS goes onto a machine *beside* what is
 * already on it. Checkpoint 14 made that true for Windows by reading FAT32.
 * Linux is the other half, and its root filesystem is ext2, ext3 or ext4 --
 * which are one format with two sets of additions, and the part that matters
 * here is the part all three share.
 *
 * --- Three capabilities, and the boundary between them is the design ---
 *
 *   **Read** is always available. Mount, walk directories, read files.
 *
 *   **Check** is always available and writes nothing. It reports what is wrong
 *   without touching the disk, which is the only way to find out whether a
 *   repair is warranted before committing to one.
 *
 *   **Repair** is *not* available unless it is asked for by name, and it only
 *   fixes what the checker found. There is no speculative repair, no fixing on
 *   mount, and no "while we are here". A filesystem that repairs itself when
 *   opened is one that can turn a recoverable disk into an unrecoverable one
 *   without anybody deciding that was worth the risk.
 *
 * That split is the whole reason write exists here. A checker that can only
 * report is half a tool; a filesystem that writes whenever it likes is a
 * liability on a partition this kernel did not create.
 *
 * --- What is deliberately not supported ---
 *
 * **ext3's journal is not replayed, and a filesystem needing replay is refused
 * rather than read.** A journal exists precisely because the visible
 * filesystem may be stale, and reading it anyway means reading a state the
 * owning system already knows is wrong.
 *
 * **ext4 extents are refused by name.** An inode with the extents flag stores
 * its blocks in a tree this does not walk, and the block-pointer array it
 * would otherwise read holds a header rather than block numbers -- so reading
 * it produces confident nonsense rather than an error.
 *
 * **64-bit block numbers are refused.** They change the shape of the group
 * descriptor, and a descriptor read at the wrong size is a filesystem whose
 * every structure is somewhere else.
 */
#ifndef RECON_KERNEL_EXT2_H
#define RECON_KERNEL_EXT2_H

#include <recon/kernel/types.h>
#include <recon/kernel/block.h>

#define EXT2_SUPER_MAGIC	0xEF53
#define EXT2_ROOT_INODE		2
#define EXT2_NAME_MAX		255

/* Where the superblock always is, whatever the block size. 1024 bytes in,
 * because the first 1024 are left for a boot sector. */
#define EXT2_SUPER_OFFSET	1024

/* Feature bits this kernel has an opinion about. Anything set outside these is
 * reported and the filesystem is still read -- an unknown *compatible* feature
 * is by definition one a reader may ignore. */
#define EXT2_FEATURE_INCOMPAT_FILETYPE	0x0002
#define EXT2_FEATURE_INCOMPAT_RECOVER	0x0004	/* needs journal replay */
#define EXT2_FEATURE_INCOMPAT_64BIT	0x0080
#define EXT2_FEATURE_INCOMPAT_EXTENTS	0x0040

#define EXT2_INODE_FLAG_EXTENTS		0x00080000

/* File types, in the directory entry when FILETYPE is set. */
#define EXT2_FT_REG		1
#define EXT2_FT_DIR		2

enum ext2_status {
	EXT2_OK = 0,
	EXT2_ERR_NO_DEVICE,
	EXT2_ERR_IO,
	EXT2_ERR_NOT_EXT2,	/* the magic is not there */
	EXT2_ERR_UNSUPPORTED,	/* it is ext2 and this cannot read it safely */
	EXT2_ERR_NOT_FOUND,
	EXT2_ERR_NOT_DIR,
	EXT2_ERR_RANGE,
	EXT2_ERR_READ_ONLY,	/* a repair was attempted without asking */
	EXT2_ERR_CORRUPT,
};

const char *ext2_status_name(enum ext2_status s);

/* A mounted filesystem. Read-only unless a repair says otherwise, and the flag
 * is not a setting -- it is turned on for the duration of one repair and off
 * again afterwards. */
struct ext2 {
	struct block_device *dev;

	u32 block_size;
	u32 blocks_per_group;
	u32 inodes_per_group;
	u32 inode_size;
	u32 inode_count;
	u32 block_count;
	u32 first_data_block;
	u32 group_count;
	u32 incompat;

	bool filetype;		/* directory entries carry a type byte */
	bool mounted;
};

/* One file or directory, as this kernel cares about it. */
struct ext2_inode_info {
	u32 number;
	u16 mode;
	u32 size;
	u32 blocks[15];		/* twelve direct, then the three indirects */
	bool is_dir;
};

/* Mounts, and refuses anything it cannot read *correctly*. The reasons are
 * reported rather than folded into one failure, because "this is not ext2" and
 * "this is ext4 and I will not guess" send a caller to different places. */
enum ext2_status ext2_mount(struct block_device *dev, struct ext2 *fs);

enum ext2_status ext2_read_inode(struct ext2 *fs, u32 number,
				 struct ext2_inode_info *out);

/* Resolves a path from the root. */
enum ext2_status ext2_lookup(struct ext2 *fs, const char *path,
			     struct ext2_inode_info *out);

/* Reads from a file. Answers how many bytes were placed, which is short at the
 * end of the file and zero past it. */
i64 ext2_read(struct ext2 *fs, const struct ext2_inode_info *in, u64 offset,
	      void *out, u64 len);

/* Lists a directory: names back to back and NUL-terminated, the answer being
 * the size of the *whole* listing whether or not it fitted. The same shape
 * reconfs_list chose, and for the same reason. */
i64 ext2_list(struct ext2 *fs, const struct ext2_inode_info *dir, char *names,
	      u64 names_len, unsigned *count);

/* --- checking, which writes nothing --------------------------------------- */

struct ext2_damage {
	unsigned superblock_disagrees;	/* the copy in group 0 vs the primary */
	unsigned bad_block_bitmap;	/* a group's bitmap is outside the disk */
	unsigned bad_inode_bitmap;
	unsigned free_blocks_wrong;	/* the count does not match the bitmap */
	unsigned free_inodes_wrong;
	unsigned inode_past_end;	/* an inode points outside the volume */
	unsigned dir_entry_bad;		/* an entry runs past its block */

	/* The first thing found, in words, so a report says something rather
	 * than only counting. */
	char first[96];
};

enum ext2_status ext2_check(struct ext2 *fs, struct ext2_damage *out);

/* --- repairing, which does not happen unless it is asked for -------------- */

/* Fixes what `ext2_check` found and nothing else.
 *
 * `confirm` must be the exact word "repair". It is not a boolean because a
 * boolean is what a caller passes by accident -- this is the same argument the
 * block layer makes about claiming a disk with partitions on it.
 *
 * Answers how many things were changed. Zero with EXT2_OK means the filesystem
 * was already sound, which is a different result from a repair that failed. */
enum ext2_status ext2_repair(struct ext2 *fs, const char *confirm,
			     unsigned *fixed);

void ext2_print_summary(void);
bool ext2_self_test(void);

#endif
