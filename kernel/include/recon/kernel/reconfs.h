/* ReconFS -- the on-disk format.
 *
 * This header is the format. Everything else about ReconFS can be rewritten;
 * this cannot, because the moment somebody's data is in it the layout is frozen
 * and every mistake here has to be carried forever or migrated with a tool that
 * must itself be perfect.
 *
 * The design and its reasoning are in docs/RECONFS.md. This file records the
 * decisions that ended up as bytes, and why each field is the width it is.
 *
 * --- The one rule ---
 *
 * Nothing reachable from a live superblock is ever overwritten, and a change
 * becomes real when -- and only when -- a superblock naming it is written after
 * a flush that has already put everything it names on the medium.
 *
 * Every write is therefore one of two things: a write to a block no live
 * superblock can reach, or a superblock write. There is no third kind, and a
 * reviewer can check that claim against the code rather than take it on trust.
 *
 * --- Why the block is 4096 bytes and always carries a checksum ---
 *
 * The medium promises that one *device* block is written whole, and even that
 * is convention rather than something this kernel has verified -- QEMU never
 * tore one, but QEMU cannot tear one (issue #273). A device block may be 512
 * bytes, so a 4096-byte filesystem block can be eight of them, which makes
 * every filesystem block a multi-block structure by the definition in the
 * design.
 *
 * Rather than have two cases, there is one: **every block carries a checksum
 * over itself**, and a block whose checksum does not match did not happen. That
 * covers tearing, a partial write whose transferred count `block_write` does
 * not report, and a device that returns stale bytes, with a single mechanism.
 *
 * --- Why an owner table rather than a bitmap ---
 *
 * The design needs a back-reference on every allocated object, so that walking
 * *down* from the root and sweeping *up* from every allocated object are two
 * independent derivations of the same set. It also needs to know which blocks
 * are free.
 *
 * Those are the same table. A block is unclaimed exactly when its owner is
 * RECONFS_OWNER_VOID, and otherwise the owner *is* the back-reference. One
 * structure, eight bytes per block, and the reverse sweep gets what it needs
 * without a header inside data blocks -- which would have made file data no
 * longer block-aligned, for the benefit of a checker.
 *
 * --- The owner is a dossier number, not a block number ---
 *
 * It was a block number, which is the obvious choice and is wrong here, for a
 * reason that only appears once directories have entries in them.
 *
 * Copy-on-write moves an object's block every time the object changes. If a
 * child recorded its parent's *block*, then changing a directory -- adding one
 * file to it -- would move that directory's inode, and every child would then
 * hold a stale back-reference. Fixing them means rewriting every child, which
 * moves every child, which invalidates the back-references of *their* children.
 *
 * Creating one file in a directory of a thousand would rewrite a thousand and
 * one inodes, and a deep tree would rewrite far more than that. Not slow:
 * unusable.
 *
 * A dossier number is allocated once and never reused, so it survives the
 * object moving. Nothing has to be rewritten when a directory changes except
 * the directory itself and the path from it to the root.
 *
 * --- What that costs the checker, and what it keeps ---
 *
 * The reverse sweep can no longer *climb*: a dossier does not say where its
 * inode lives, and finding out would need an index the forward walk builds --
 * which is precisely the dependency that would make the two derivations one.
 *
 * So the sweep does not climb. It builds a map of *which dossier claims which
 * blocks*, reading nothing but the owner table, and the forward walk builds the
 * same map from directory entries and block pointers, reading nothing but
 * pointers. Disjoint fields, and the two maps must be identical.
 *
 * That is the same evidence as before. The climb was the smaller half: four of
 * the five faults the checker is tested against were caught by the comparison,
 * not by the climb.
 *
 * --- Why an inode is a whole block, and its number is its address ---
 *
 * An inode occupies one block, and the inode's number *is* that block's number.
 * That is wasteful and it is deliberate:
 *
 *   - allocating an inode is allocating a block, so create-with-mode is not a
 *     special case: the mode is in the same block as everything else about the
 *     file, and that block becomes reachable in the single write that commits
 *     it. There is no image, crashed or otherwise, in which a file is reachable
 *     with a mode that was written afterwards.
 *   - the owner table entry for an inode block is its parent directory, which
 *     is exactly the "a file to its directory" half of the reverse sweep.
 *   - there is no inode allocator to get wrong, and no second free-space
 *     structure that can disagree with the first.
 *
 * The cost is a block per file. Small files get most of it back by living
 * inside their inode (RECONFS_INLINE_MAX below), which is also the common case
 * for the registry and theme files this has to hold.
 *
 * --- What changes on every write, and what does not ---
 *
 * Copy-on-write means the path from a changed object up to the root is copied,
 * so an inode's *block number changes* whenever the file is written. Anything
 * that needs a name for a file that survives a write must use `object_id`,
 * which is allocated once from a counter in the superblock and never reused.
 *
 * Stated here because "the inode number is stable" is the assumption every
 * filesystem trains people to make, and here it is false.
 */
#ifndef RECON_KERNEL_RECONFS_H
#define RECON_KERNEL_RECONFS_H

#include <recon/kernel/types.h>
#include <recon/kernel/compiler.h>

/* "ReconFS1" read as big-endian ASCII, stored little-endian like everything
 * else. A version bump changes the trailing digit, so a wrong-version image is
 * rejected by the magic rather than by a version check somebody forgot. */
#define RECONFS_MAGIC		0x3153466E6F636552ULL
#define RECONFS_INODE_MAGIC	0x316F6E496E636552ULL	/* "ReconIno1" (9 -> 8) */
#define RECONFS_VERSION		1

/* --- Block size: chosen when the volume is made, not compiled in -----------
 *
 * A big drive wants a big block. The owner table is eight bytes per block
 * however large the block is, so the table's share of the volume is 8/blocksize
 * and nothing else:
 *
 *     4KiB blocks   0.195% of the volume    a 24TB drive spends 47GB on it
 *    16KiB blocks   0.049%                                      12GB
 *    64KiB blocks   0.012%                                       3GB
 *
 * Bigger blocks also mean bigger sequential transfers, which is what both a
 * spinning disk and an SSD are fastest at, and fewer of them -- an SSD's
 * controller does less work per megabyte written.
 *
 * The cost is the tail of every file: a 100-byte file in a 64KiB block wastes
 * 64KiB. Small files live inside their inode instead (RECONFS_INLINE_MAX),
 * which *grows* with the block size, so a larger block makes the inline case
 * cover more files rather than fewer.
 *
 * The value in use is on the medium, in the superblock. Nothing in the code
 * may assume RECONFS_BLOCK_DEFAULT. */
#define RECONFS_BLOCK_MIN	4096
#define RECONFS_BLOCK_MAX	65536
#define RECONFS_BLOCK_DEFAULT	4096

/* Structures are laid out against the smallest block, so that a structure that
 * fits at all fits at every size. */
#define RECONFS_BLOCK		RECONFS_BLOCK_MIN

/* Superblocks live at fixed, known blocks, because recovery must be able to
 * find them without already knowing anything. Two of them, alternating: a
 * commit writes whichever is *not* live, so a crash during the superblock write
 * itself leaves the other one intact and the format never has a moment with no
 * valid superblock on it. */
/* Superblock A is at byte zero, which is block zero at every block size. The
 * second copy is at byte RECONFS_BLOCK_MAX, whose *block number* depends on the
 * block size -- so it has no constant, only reconfs_super_b().
 *
 * There was a `RECONFS_SUPER_B 1` here, from when the two were adjacent blocks.
 * It went on compiling and went on meaning block 1 after the layout changed
 * under it, and every commit wrote its superblock into dead space. A constant
 * whose meaning has moved is worse than no constant, so there is no constant. */
#define RECONFS_SUPER_A		0

/* Owner table sentinels.
 *
 * Zero means unclaimed, and block zero is a superblock which is never owned by
 * an inode, so zero is available as a sentinel without ambiguity. One means the
 * archive's own metadata -- the superblocks and the owner table itself -- which
 * is owned by nothing above it and must still not read as free. */
#define RECONFS_OWNER_VOID	0ULL
#define RECONFS_OWNER_ARCHIVE	1ULL

/* The root directory's dossier. Fixed, because the root has to be identifiable
 * before anything has been read, and because dossier 0 and 1 are taken by the
 * sentinels above -- an object numbered 1 would be indistinguishable from a
 * block the archive owns. Everything else is numbered from
 * RECONFS_DOSSIER_FIRST upward and never reused. */
#define RECONFS_DOSSIER_ROOT	2ULL
#define RECONFS_DOSSIER_FIRST	3ULL

/* --- Why an owner is 64 bits, and what that cost buys --------------------
 *
 * It was 32. A 32-bit owner is a 32-bit block number, which caps the volume at
 * 2^32 blocks: 16TiB at the smallest block size. That was written down as a
 * documented limit, which is not the same as it being an acceptable one --
 * 20TB and 24TB drives are on sale now, and a filesystem that refuses the disk
 * somebody just bought is not a filesystem with a documented limitation, it is
 * a filesystem that does not work.
 *
 * At 64 bits there is no ceiling worth stating: 2^64 blocks of 4096 bytes is
 * sixty-four zettabytes, and the practical limit becomes the drivers -- NVMe
 * carries a full 64-bit LBA, and AHCI is capped by ATA's own LBA48 at 128PiB.
 *
 * The cost is the owner table, which is the price of the reverse sweep being an
 * independent derivation. Eight bytes per block is 0.2% of the volume at a 4KiB
 * block, and it is why the block size below is chosen at format time rather
 * than fixed: at 64KiB blocks the same table is 0.012%, and the arithmetic is
 * the same in both cases.
 */

/* Blocks per volume is a u64 and nothing here narrows it. Kept as a name so
 * that a future check has something to refer to rather than a literal. */
#define RECONFS_MAX_BLOCKS	0xFFFFFFFFFFFFFFFFULL

#define RECONFS_NAME_MAX	255
#define RECONFS_LABEL_MAX	64

/* File types. Deliberately few. Symlinks, devices and FIFOs are absent because
 * nothing above this can create one yet, and a type byte that no code writes is
 * a promise the format has not earned. */
#define RECONFS_TYPE_FILE	1
#define RECONFS_TYPE_DIR	2

/* Direct block pointers in an inode. Twelve is enough for 48KiB without an
 * indirect block, which covers every file the desktop currently writes. */
#define RECONFS_DIRECT		12

/* Block numbers per index block, for both the owner table's tree and a file's
 * indirect blocks -- and, now that an owner is also 64 bits, for an owner table
 * leaf as well. One expression, used by all three, because they are the same
 * shape and three constants that must stay equal are two bugs waiting for one
 * of them to be changed.
 *
 * Taken from the volume's block size, never from the compiled-in default. */
#define RECONFS_PER_INDEX_FOR(bs)  ((bs) / (unsigned)sizeof(u64))
#define RECONFS_PER_LEAF_FOR(bs)   ((bs) / (unsigned)sizeof(u64))

/*
 * The superblock.
 *
 * Written last in every commit, after a flush that has already put everything
 * it names on the medium. Its `generation` is what makes recovery a pure
 * function of the image: read both, discard any whose checksum fails, and
 * believe the survivor with the higher generation. Nothing is replayed, so
 * recovering the same image twice produces byte-identical results and a crash
 * during recovery changes nothing, because recovery writes nothing.
 */
struct reconfs_super {
	u64 magic;
	u32 version;
	u32 block_size;		/* RECONFS_BLOCK; a different value is refused */

	/* The commit counter, and the only thing that decides which of the two
	 * superblocks is the truth. Monotonic, never reset. 64 bits so that one
	 * commit per microsecond for half a million years does not wrap -- the
	 * alternative is a wrap rule, and a wrap rule is a branch that runs once
	 * in the lifetime of a filesystem and has never been tested.
	 *
	 * Called an epoch rather than a generation for two reasons: `struct
	 * block_device` already has a `generation` meaning something else
	 * entirely, and the archive this format is named for numbers its history
	 * in epochs. */
	u64 epoch;

	u64 total_blocks;
	u64 root_inode;		/* block number of the root directory's inode */

	/* The owner table, as a radix tree rather than a contiguous run.
	 *
	 * A run would have been simpler to read and impossible to commit: under
	 * copy-on-write, changing one entry in a contiguous table means copying
	 * the whole table, and on a 16TiB volume the table is 16GiB. One commit
	 * per file write, each rewriting 16GiB, is not slow -- it is a design
	 * that cannot be used at all.
	 *
	 * As a tree, a commit copies only the leaves it touched and the index
	 * blocks above them: at most `table_depth + 1` blocks per changed leaf.
	 *
	 *   depth 0   table_root is a leaf              4MiB of volume
	 *   depth 1   512 leaves                        2GiB
	 *   depth 2   512 x 512 leaves                  1TiB
	 *   depth 3   512 x 512 x 512 leaves            512TiB
	 *
	 * Depth is stored rather than derived from `total_blocks`, so a reader
	 * never has to agree with the writer about a calculation. Two
	 * implementations of one formula is two chances to be wrong; a number
	 * on the medium is one fact. */
	u64 table_root;
	u32 table_depth;
	u32 reserved_table;

	/* Allocated once per object, never reused, so a name for a file can
	 * outlive the file's block number changing under it.
	 *
	 * Called a dossier number after the archive's own rule for them, which
	 * is the same rule and is worth borrowing exactly: *never renumber a
	 * record once it is locked -- that is how a retcon sneaks in by
	 * accident.* Here the accident is two files that were never the same
	 * file sharing an identity because a number was reused. */
	u64 next_dossier;

	u64 blocks_used;

	u8  uuid[16];
	char label[RECONFS_LABEL_MAX];

	/* Case folding is part of the format, not a mount option: an index
	 * built for one cannot answer the other, so a volume written by a
	 * case-sensitive implementation and read by a case-insensitive one
	 * would find files it should not and miss files it should.
	 *
	 * One value is defined -- ASCII folding -- and a volume claiming any
	 * other is refused rather than mounted with the wrong comparison. */
	u32 fold;

	/* --- What the medium underneath is like -----------------------------
	 *
	 * Recorded at format time, because the allocator's decisions depend on
	 * it and a volume can be moved to a machine that reports differently --
	 * or to no machine at all, when a checker reads the image as a file.
	 *
	 * `seek_is_free` is the one that matters: on an SSD it is true and
	 * scattering a file across the volume costs nothing, so the allocator
	 * may take the first free block anywhere. On a spinning disk it is
	 * false, a seek costs milliseconds, and the allocator must try to keep a
	 * file's blocks near each other and near its inode.
	 *
	 * This is the *only* place that difference is recorded. Copy-on-write
	 * fragments by nature, so the difference between the two media is not a
	 * tuning detail here -- it is the difference between a filesystem that
	 * is pleasant on a hard disk and one that is unusable on it. */
	u8  seek_is_free;	/* solid state: any block is as good as any */
	u8  discard_supported;	/* the device wants to be told about freed space */
	u16 reserved_media;
	u32 transfer_hint;	/* bytes the device prefers to move at once */

	u32 reserved[10];

	/* CRC-32 over the whole block with this field taken as zero.
	 *
	 * The same reflected polynomial as the GPT reader, and deliberately not
	 * the Castagnoli CRC that SSE4.2's `crc32` instruction computes -- see
	 * crc32.h, where that trap is written down. */
	u32 checksum;
} RK_PACKED;

/*
 * An inode. One per block; the block's number is the inode's number.
 */
struct reconfs_inode {
	u64 magic;
	u64 dossier;		/* stable across rewrites; never reused */

	/* The back-reference, and half of what makes the checker independent:
	 * the *dossier* of the directory whose entry names this inode. Zero for
	 * the root, which is the only object with no parent.
	 *
	 * A dossier rather than a block, for the reason given at the top of this
	 * file: a block number would go stale every time the parent changed, and
	 * fixing it would rewrite the whole subtree.
	 *
	 * A forward walk never reads this field. That is the point -- if the
	 * checker's two derivations shared a field, they would agree by
	 * construction rather than by the image being correct. */
	u64 parent;

	u32 type;		/* RECONFS_TYPE_* */
	u32 mode;		/* permission bits, written with the inode */
	u32 uid;
	u32 gid;

	u64 size;		/* bytes, for a file; bytes of entries, for a
				 * directory */

	u64 btime;		/* created */
	u64 mtime;		/* contents last changed */
	u64 ctime;		/* inode last changed */

	u32 links;		/* always 1 today; no hard links exist */
	u32 flags;

	/* Where the contents are.
	 *
	 * `inline_len` non-zero means the contents are in this block, after the
	 * header, and `direct`/`indirect` are unused. A file switches out of
	 * inline form by being rewritten, which under copy-on-write is what
	 * every write does anyway. */
	u32 inline_len;
	u32 reserved0;
	u64 direct[RECONFS_DIRECT];

	/* Indirection, to three levels.
	 *
	 *   direct              48KiB
	 *   indirect            2MiB
	 *   double_indirect     1GiB
	 *   triple_indirect     512GiB
	 *
	 * The first version of this header had `direct` and `indirect` only,
	 * which caps a file at 2MiB. That is enough for everything the desktop
	 * writes today and it would have been the wrong thing to freeze: a
	 * field cannot be added to an on-disk inode later without a migration,
	 * and "no file may exceed two megabytes" is not a limitation anyone
	 * would accept discovering afterwards.
	 *
	 * The deeper two are read and written by the format but not yet by the
	 * allocator: a file that would need one is *refused*, with an error, not
	 * silently truncated. Refusing rather than truncating is this project's
	 * standing rule, and it is the difference between a limitation and a
	 * data-loss bug. */
	u64 indirect;
	u64 double_indirect;
	u64 triple_indirect;

	u64 reserved1[2];

	u32 checksum;		/* as the superblock's, over the whole block */
	u32 reserved2;

	/* The contents, when they fit. */
	u8 data[];
} RK_PACKED;

/* How much of a small file, or how many directory entries, live inside the
 * inode itself. It grows with the block size, so a larger block makes the
 * inline case cover *more* files rather than fewer. */
#define RECONFS_INLINE_MAX_FOR(bs) ((bs) - (unsigned)sizeof(struct reconfs_inode))

/*
 * A directory entry, in a directory's data blocks.
 *
 * Entries never straddle a block. A block with no room for the next entry is
 * padded, which wastes a little and buys the property that any single directory
 * block can be read and understood on its own -- which is what the reverse
 * sweep needs, and what a torn-block check can be written against.
 */
struct reconfs_dirent {
	u64 inode;		/* block number of the child's inode */
	u16 rec_len;		/* to the next entry; a multiple of 8 */
	u8  name_len;
	u8  type;		/* RECONFS_TYPE_*, so a listing needs no inode
				 * read -- and is checked against the inode when
				 * one is read, because a duplicated fact that
				 * is never compared is just a second place to
				 * be wrong */
	u32 reserved;
	char name[];		/* name_len bytes, not terminated */
} RK_PACKED;

#define RECONFS_DIRENT_MIN	sizeof(struct reconfs_dirent)

/* Case folding, named so the superblock can say which one it means. */
#define RECONFS_FOLD_ASCII	1

/* =========================================================================
 * Everything above this line is the format, and is frozen once anyone's data
 * is in it. Everything below is how this kernel happens to work with it, and
 * can be rewritten freely.
 * ========================================================================= */

struct block_device;

enum reconfs_status {
	RECONFS_OK = 0,
	RECONFS_ERR_IO,
	RECONFS_ERR_RANGE,
	RECONFS_ERR_DEVICE,
	RECONFS_ERR_CHECKSUM,
	RECONFS_ERR_CORRUPT,
	RECONFS_ERR_NOT_RECONFS,
	RECONFS_ERR_NOMEM,
	RECONFS_ERR_READ_ONLY,
	RECONFS_ERR_TOO_LARGE,
	RECONFS_ERR_TOO_SMALL,
	RECONFS_ERR_NOSPACE,
	RECONFS_ERR_BLOCK_SIZE,
	RECONFS_ERR_RETRY,	/* nothing is wrong; try the whole thing again */
	RECONFS_ERR_NAME,	/* the name is empty, too long, or contains a slash */
	RECONFS_ERR_NOT_FOUND,
	RECONFS_ERR_NOT_DIR,
	RECONFS_ERR_EXISTS,
	RECONFS_ERR_DIR_FULL,	/* refused rather than truncated */
	RECONFS_ERR_NOT_FILE,
	RECONFS_ERR_NOT_EMPTY,
};

/* Passed to the block routines for a block kind that carries no checksum of
 * its own -- an owner-table leaf or index block, whose integrity comes from
 * the superblock generation that named it. Not zero, because zero is a
 * perfectly good offset and a sentinel that collides with a real value is a
 * bug that waits for the field to move. */
#define RECONFS_NO_CSUM		0xFFFFFFFFu

struct reconfs {
	struct block_device *dev;

	struct reconfs_super super;
	u64 live_super;		/* which of the two is believed */

	/* Taken from the superblock at mount, never from the compiled-in
	 * default. A volume made with 64KiB blocks read by code that assumed
	 * 4KiB would find a valid superblock and then misread everything after
	 * it, which is the worst way for a size mismatch to present. */
	u32 block_size;
	u32 per_index;		/* block numbers per index block */
	u32 per_leaf;		/* owners per table leaf */

	u64 total_blocks;
	u64 table_root;
	u32 table_depth;
	u64 root_inode;

	/* One block each, allocated at mount. The owner-table walk needs a
	 * place to put an index block and a leaf, and doing that on the stack
	 * would put eight kilobytes on a kernel stack that does not have it. */
	void *scratch_index;
	void *scratch_leaf;

	bool mounted;
};

enum reconfs_status reconfs_mount(struct block_device *dev, struct reconfs *fs);
void reconfs_unmount(struct reconfs *fs);

/* `block_size` of zero means "choose one for this device": the largest size
 * that keeps the owner table under a fiftieth of a percent of the volume, which
 * on anything smaller than a terabyte is the minimum anyway. */
enum reconfs_status reconfs_format(struct block_device *dev, const char *label,
				   u32 block_size);

enum reconfs_status reconfs_read_block(struct reconfs *fs, u64 blk, void *out,
				       unsigned csum_off);
enum reconfs_status reconfs_write_block(struct reconfs *fs, u64 blk, void *buf,
					unsigned csum_off);

/* Where superblock B lives, in blocks, for a given block size. Both copies sit
 * at fixed byte offsets so that either can be found without knowing the other.
 * The first block a file may use is one past it. */
u64 reconfs_super_b(u32 block_size);

/* The owner table's shape for a volume of this size. Exposed so the layout
 * self-test can check them against a derivation written from the
 * definition rather than from this code. */
u32 reconfs_depth_for(u64 total_blocks, u32 per_leaf, u32 per_index);
u64 reconfs_blocks_for_table(u64 total_blocks, u32 depth, u32 per_leaf,
			     u32 per_index);

enum reconfs_status reconfs_read_leaf(struct reconfs *fs, u64 leaf_index,
				      void *out);
enum reconfs_status reconfs_owner_of(struct reconfs *fs, u64 blk, u64 *owner);
enum reconfs_status reconfs_set_owner(struct reconfs *fs, u64 blk, u64 owner);

const char *reconfs_strerror(enum reconfs_status st);

/* --- The commit -----------------------------------------------------------
 *
 * A change is built entirely out of blocks the live superblock calls unclaimed,
 * and becomes real in the single superblock write at the end of
 * `reconfs_txn_commit`. Abandoning a transaction needs no undo: nothing it
 * wrote was ever reachable.
 */
struct reconfs_txn;

struct reconfs_txn *reconfs_txn_begin(struct reconfs *fs);
u64  reconfs_txn_alloc(struct reconfs_txn *txn, u64 owner);
void reconfs_txn_free(struct reconfs_txn *txn, u64 blk);
u64  reconfs_txn_new_dossier(struct reconfs_txn *txn);
void reconfs_txn_set_root(struct reconfs_txn *txn, u64 inode);
bool reconfs_txn_failed(const struct reconfs_txn *txn);
enum reconfs_status reconfs_txn_commit(struct reconfs_txn *txn);
void reconfs_txn_abort(struct reconfs_txn *txn);

/* The checker. Two derivations of the same set, compared. */
struct reconfs_check_result {
	u64 blocks_seen_forward;
	u64 blocks_seen_reverse;
	u64 inodes;
	u64 disagreements;
	const char *first_disagreement;
	u64 first_disagreement_block;
};

enum reconfs_status reconfs_check(struct reconfs *fs,
				  struct reconfs_check_result *out);

/* Formats a device and proves the checker catches deliberate damage.
 *
 * Runs only when `reconfs=<device>` names one on the command line, because it
 * destroys what is there. Refuses a device carrying a partition table, and
 * there is no flag to override that. */
void reconfs_run(void);

/* Replaces one file by rename, over and over, until the machine is killed.
 * Runs only when `reconfs-crash=<device>` names one; scripts/crash-test.sh
 * cuts the power into it and scripts/reconfs-check.py judges what survived. */
void reconfs_crash_run(void);

/* The layout arithmetic, at sizes no disk in this build environment can reach.
 * Writes nothing and touches no device, so it runs on every boot. */
bool reconfs_layout_self_test(void);

/* --- Names ----------------------------------------------------------------
 *
 * Lookup is case-insensitive and storage is case-preserving, which is what a
 * person means by a name. The fold is ASCII only; see reconfs_dir.c.
 */
u32 reconfs_inline_max(u32 block_size);

enum reconfs_status reconfs_lookup(struct reconfs *fs, u64 dir_block,
				   const char *name, u64 *out_block);

/* Creates an object in `dir_block` and returns both the new object's block and
 * the *new* block of the directory, which moved because it changed. The caller
 * is responsible for making the new directory reachable -- for the root, with
 * reconfs_txn_set_root. */
enum reconfs_status reconfs_create(struct reconfs_txn *txn, struct reconfs *fs,
				   u64 dir_block, const char *name, u32 type,
				   u32 mode, u64 *out_inode, u64 *out_dir);

/* Replaces everything a file holds. Whole-file only: the callers above build
 * their contents in memory and write them out entire, and under copy-on-write a
 * partial write is barely cheaper anyway.
 *
 * The file's inode moves, so the directory is rewritten too and its new block
 * comes back in `out_dir`. A file too large for one indirect block is refused,
 * not truncated. */
enum reconfs_status reconfs_write_named(struct reconfs_txn *txn,
					struct reconfs *fs, u64 dir_block,
					const char *name, const void *data,
					u32 len, u64 *out_dir);

/* Reads a whole file. A file larger than `max` is refused rather than partly
 * returned -- a caller handed half a file with a success status cannot tell. */
enum reconfs_status reconfs_read_named(struct reconfs *fs, u64 dir_block,
				       const char *name, void *out, u32 max,
				       u32 *got);

/* Removes a name, and releases what it named, in a single commit.
 *
 * The blocks are released, not erased: they keep whatever they held until
 * something else is written into them. Anything needing an erase that is really
 * an erase has to say so.
 *
 * A directory that still has entries is refused rather than recursed -- a
 * recursive delete is a decision for the layer that knows whether the user
 * meant it. */
enum reconfs_status reconfs_remove(struct reconfs_txn *txn, struct reconfs *fs,
				   u64 dir_block, const char *name,
				   u64 *out_dir);

/* Renames `from` to `to` within one directory, in a single commit.
 *
 * The post-crash outcome set has exactly two members: the rename happened, or
 * it did not. Renaming over an existing name is permitted and releases what was
 * there. Renaming between two directories is refused -- the path-copy that would
 * make it one commit does not exist yet.
 *
 * Returns the directory's new block; the caller makes it reachable. */
enum reconfs_status reconfs_rename(struct reconfs_txn *txn, struct reconfs *fs,
				   u64 dir_block, const char *from,
				   const char *to, u64 *out_dir);

/* A listing is a snapshot of one epoch: entries come from the directory as it
 * was when the listing started, and changes made while iterating are not seen.
 * Stated rather than left to be discovered. */
enum reconfs_status reconfs_readdir(struct reconfs *fs, u64 dir_block,
				    unsigned index, char *name, size_t len,
				    u64 *child, u32 *type);

#endif /* RECON_KERNEL_RECONFS_H */
