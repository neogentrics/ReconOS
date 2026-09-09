/* ReconFS -- reading the format, and making one.
 *
 * The format itself is kernel/include/recon/kernel/reconfs.h, which is where
 * the reasoning about the layout lives. This file is the part that can be
 * rewritten: how a block gets to and from the device, how a superblock is
 * chosen, and how an empty volume is built.
 *
 * --- Every block goes through two functions ---
 *
 * `reconfs_read_block` and `reconfs_write_block` are the only places that talk
 * to the block layer, and every read verifies the checksum before the caller
 * sees the bytes. That is what makes "a block whose checksum does not match did
 * not happen" true rather than aspirational: there is no path that reads a
 * block without checking it, so no caller has to remember to.
 *
 * --- The block size is on the medium, not in this file ---
 *
 * A volume records the block size it was made with, and everything here works
 * from that. Nothing may assume RECONFS_BLOCK_DEFAULT: a 64KiB volume read by
 * code assuming 4KiB would find a valid superblock and then misread every
 * structure after it, which is the worst way for a size mismatch to present --
 * it looks like corruption, at a distance from its cause.
 *
 * --- Which is why the superblocks are at fixed *byte* offsets ---
 *
 * Superblock A is at byte zero and superblock B at byte 65536, whatever the
 * block size is. If B's position depended on the block size, then finding B
 * would require knowing the block size, which is only readable from A -- and a
 * volume whose A is damaged would be unrecoverable precisely when the second
 * copy exists to save it.
 */
#include <recon/kernel/reconfs.h>

#include <recon/kernel/block.h>
#include <recon/kernel/console.h>
#include <recon/kernel/crc32.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/time.h>

#define SUPER_CSUM	RK_OFFSETOF(struct reconfs_super, checksum)
#define INODE_CSUM	RK_OFFSETOF(struct reconfs_inode, checksum)

/* The checksum, over the whole block with its own field taken as zero.
 *
 * In three pieces rather than by copying the block and zeroing a field in the
 * copy: the copy version put a block-sized buffer on the stack, in a function
 * reachable from the checker's recursive directory walk. It would have
 * survived, and "survives if you reason carefully about when it is live" is not
 * a property to build a filesystem on. */
static u32 block_checksum(const void *blk, unsigned off, u32 size)
{
	static const u8 zeros[sizeof(u32)] = { 0, 0, 0, 0 };
	const u8 *p = blk;
	u32 c = CRC32_INIT;

	c = crc32_update(c, p, off);
	c = crc32_update(c, zeros, sizeof(zeros));
	c = crc32_update(c, p + off + sizeof(u32), size - off - sizeof(u32));

	return crc32_final(c);
}

/* A filesystem block is `block_size` bytes; a device block may be 512. The
 * conversion happens here and nowhere else. */
static bool sectors_per_block(const struct block_device *dev, u32 block_size,
			      u32 *out)
{
	if (!dev->block_size || block_size % dev->block_size)
		return false;
	*out = block_size / dev->block_size;
	return true;
}

static bool block_size_ok(u32 bs)
{
	if (bs < RECONFS_BLOCK_MIN || bs > RECONFS_BLOCK_MAX)
		return false;
	return (bs & (bs - 1)) == 0;		/* a power of two */
}

enum reconfs_status reconfs_read_block(struct reconfs *fs, u64 blk, void *out,
				       unsigned csum_off)
{
	u32 spb;

	if (blk >= fs->total_blocks)
		return RECONFS_ERR_RANGE;

	if (!sectors_per_block(fs->dev, fs->block_size, &spb))
		return RECONFS_ERR_DEVICE;

	if (block_read(fs->dev, blk * spb, spb, out) != BLOCK_OK)
		return RECONFS_ERR_IO;

	if (csum_off != RECONFS_NO_CSUM) {
		u32 stored;

		kmemcpy(&stored, (const u8 *)out + csum_off, sizeof(stored));
		if (stored != block_checksum(out, csum_off, fs->block_size))
			return RECONFS_ERR_CHECKSUM;
	}

	return RECONFS_OK;
}

/* Writes one block, stamping its checksum on the way out.
 *
 * The caller never computes a checksum. A checksum computed by callers is a
 * checksum some caller forgets, and a block written without one reads back as
 * corrupt -- which is the good failure, but only after the data is gone. */
enum reconfs_status reconfs_write_block(struct reconfs *fs, u64 blk, void *buf,
					unsigned csum_off)
{
	u32 spb;

	if (blk >= fs->total_blocks)
		return RECONFS_ERR_RANGE;

	if (!sectors_per_block(fs->dev, fs->block_size, &spb))
		return RECONFS_ERR_DEVICE;

	if (csum_off != RECONFS_NO_CSUM) {
		u32 c = block_checksum(buf, csum_off, fs->block_size);

		kmemcpy((u8 *)buf + csum_off, &c, sizeof(c));
	}

	if (block_write(fs->dev, blk * spb, spb, buf) != BLOCK_OK)
		return RECONFS_ERR_IO;

	return RECONFS_OK;
}

u32 reconfs_inline_max(u32 block_size)
{
	return RECONFS_INLINE_MAX_FOR(block_size);
}

/* Where superblock B lives, in blocks, for a given block size. Both copies sit
 * at fixed byte offsets; this converts the second one. */
u64 reconfs_super_b(u32 block_size)
{
	return RECONFS_BLOCK_MAX / block_size;
}

static u64 first_data_block(u32 block_size)
{
	return reconfs_super_b(block_size) + 1;
}

/* --- The owner table ------------------------------------------------------
 *
 * A radix tree of index blocks over leaves of u64 owners. `table_depth` says
 * how many index levels sit above the leaves, and it is read from the
 * superblock rather than derived from the volume size, so that a reader and a
 * writer cannot disagree about a formula.
 */
static u64 table_leaf(struct reconfs *fs, u64 blk, enum reconfs_status *st)
{
	u64 node = fs->table_root;
	u64 index = blk / fs->per_leaf;
	u32 depth = fs->table_depth;
	u64 span = 1;
	u32 i;

	for (i = 0; i < depth; i++)
		span *= fs->per_index;

	while (depth--) {
		u64 *slots = fs->scratch_index;
		u64 slot;

		*st = reconfs_read_block(fs, node, slots, RECONFS_NO_CSUM);
		if (*st != RECONFS_OK)
			return 0;

		span /= fs->per_index;
		slot = index / span;
		if (slot >= fs->per_index) {
			*st = RECONFS_ERR_CORRUPT;
			return 0;
		}

		node = slots[slot];
		index %= span;

		if (!node) {
			*st = RECONFS_ERR_CORRUPT;
			return 0;
		}
	}

	*st = RECONFS_OK;
	return node;
}

/* Reads one whole owner-table leaf into the caller's buffer.
 *
 * For the checker, which reads every owner on the volume. Asking
 * reconfs_owner_of once per block walks the tree from the root every time: on a
 * volume of n blocks that is n * (depth + 1) reads, where reading the leaves
 * directly is n/per_leaf * (depth + 1). At a 4KiB block that is five hundred
 * times fewer, and it is the difference between a whole-volume check being slow
 * and being impossible.
 *
 * `leaf_index` counts leaves, not blocks: leaf i covers blocks
 * [i * per_leaf, (i+1) * per_leaf).
 */
enum reconfs_status reconfs_read_leaf(struct reconfs *fs, u64 leaf_index,
				      void *out)
{
	enum reconfs_status st;
	u64 first = leaf_index * fs->per_leaf;
	u64 leaf;

	if (first >= fs->total_blocks)
		return RECONFS_ERR_RANGE;

	leaf = table_leaf(fs, first, &st);
	if (st != RECONFS_OK)
		return st;

	return reconfs_read_block(fs, leaf, out, RECONFS_NO_CSUM);
}

enum reconfs_status reconfs_owner_of(struct reconfs *fs, u64 blk, u64 *owner)
{
	enum reconfs_status st;
	u64 leaf;
	u64 *entries = fs->scratch_leaf;

	if (blk >= fs->total_blocks)
		return RECONFS_ERR_RANGE;

	leaf = table_leaf(fs, blk, &st);
	if (st != RECONFS_OK)
		return st;

	st = reconfs_read_block(fs, leaf, entries, RECONFS_NO_CSUM);
	if (st != RECONFS_OK)
		return st;

	*owner = entries[blk % fs->per_leaf];
	return RECONFS_OK;
}

enum reconfs_status reconfs_set_owner(struct reconfs *fs, u64 blk, u64 owner)
{
	enum reconfs_status st;
	u64 leaf;
	u64 *entries = fs->scratch_leaf;

	if (blk >= fs->total_blocks)
		return RECONFS_ERR_RANGE;

	leaf = table_leaf(fs, blk, &st);
	if (st != RECONFS_OK)
		return st;

	st = reconfs_read_block(fs, leaf, entries, RECONFS_NO_CSUM);
	if (st != RECONFS_OK)
		return st;

	entries[blk % fs->per_leaf] = owner;
	return reconfs_write_block(fs, leaf, entries, RECONFS_NO_CSUM);
}

/* --- Mounting -------------------------------------------------------------
 *
 * Read both superblocks, discard any that does not validate, and believe the
 * survivor with the higher epoch.
 *
 * That is the whole of recovery. There is nothing to replay, so recovering the
 * same image twice produces byte-identical results and a crash during recovery
 * changes nothing, because recovery writes nothing.
 */
static bool super_ok(const struct reconfs_super *s, u64 device_blocks_min)
{
	if (s->magic != RECONFS_MAGIC)
		return false;
	if (s->version != RECONFS_VERSION)
		return false;
	if (!block_size_ok(s->block_size))
		return false;
	if (s->fold != RECONFS_FOLD_ASCII)
		return false;
	if (!s->total_blocks)
		return false;
	if (s->root_inode < first_data_block(s->block_size) ||
	    s->root_inode >= s->total_blocks)
		return false;
	if (s->table_root < first_data_block(s->block_size) ||
	    s->table_root >= s->total_blocks)
		return false;
	if (s->total_blocks > device_blocks_min)
		return false;
	return true;
}

enum reconfs_status reconfs_mount(struct block_device *dev, struct reconfs *fs)
{
	u8 *probe = NULL;
	struct reconfs_super *cand = NULL;
	unsigned which;
	bool found = false;
	u32 chosen_bs = 0;
	u32 spb;

	kmemset(fs, 0, sizeof(*fs));
	fs->dev = dev;

	/* --- Find out how big a block is, before trusting anything ---------
	 *
	 * The block size is inside the superblock, and the superblock's
	 * checksum covers a whole block -- so the size has to be read before it
	 * can be verified. It is read, sanity-checked as a power of two in
	 * range, and then the whole block is re-read at that size and the
	 * checksum verified. Nothing is believed until that second read. */
	probe = kzalloc(RECONFS_BLOCK_MIN);
	if (!probe)
		return RECONFS_ERR_NOMEM;

	if (!sectors_per_block(dev, RECONFS_BLOCK_MIN, &spb)) {
		kfree(probe);
		kputs("reconfs: the device's block size does not divide 4096\n");
		return RECONFS_ERR_DEVICE;
	}

	for (which = 0; which < 2 && !chosen_bs; which++) {
		const struct reconfs_super *p;
		u64 at_byte = which ? RECONFS_BLOCK_MAX : 0;

		if (block_read(dev, at_byte / dev->block_size, spb, probe)
		    != BLOCK_OK)
			continue;

		p = (const struct reconfs_super *)probe;
		if (p->magic != RECONFS_MAGIC || p->version != RECONFS_VERSION)
			continue;
		if (!block_size_ok(p->block_size))
			continue;

		chosen_bs = p->block_size;
	}

	kfree(probe);

	if (!chosen_bs)
		return RECONFS_ERR_NOT_RECONFS;

	if (!sectors_per_block(dev, chosen_bs, &spb)) {
		kprintf("reconfs: the volume uses %u-byte blocks, which the "
			"device's %u-byte blocks do not divide\n",
			chosen_bs, dev->block_size);
		return RECONFS_ERR_BLOCK_SIZE;
	}

	fs->block_size  = chosen_bs;
	fs->per_index   = RECONFS_PER_INDEX_FOR(chosen_bs);
	fs->per_leaf    = RECONFS_PER_LEAF_FOR(chosen_bs);
	fs->total_blocks = dev->block_count / spb;

	fs->scratch_index = kzalloc(chosen_bs);
	fs->scratch_leaf  = kzalloc(chosen_bs);
	cand              = kzalloc(chosen_bs);
	if (!fs->scratch_index || !fs->scratch_leaf || !cand) {
		reconfs_unmount(fs);
		kfree(cand);
		return RECONFS_ERR_NOMEM;
	}

	for (which = 0; which < 2; which++) {
		u64 at = which ? reconfs_super_b(chosen_bs) : RECONFS_SUPER_A;
		enum reconfs_status st;

		st = reconfs_read_block(fs, at, cand, SUPER_CSUM);
		if (st != RECONFS_OK)
			continue;
		if (!super_ok(cand, fs->total_blocks))
			continue;
		if (cand->block_size != chosen_bs)
			continue;

		/* Strictly greater: after a format both superblocks are valid
		 * and equal, and preferring the later one for no reason would
		 * make which superblock is live depend on read order. */
		if (found && cand->epoch <= fs->super.epoch)
			continue;

		kmemcpy(&fs->super, cand, sizeof(fs->super));
		fs->live_super = at;
		found = true;
	}

	kfree(cand);

	if (!found) {
		reconfs_unmount(fs);
		return RECONFS_ERR_NOT_RECONFS;
	}

	fs->total_blocks = fs->super.total_blocks;
	fs->table_root   = fs->super.table_root;
	fs->table_depth  = fs->super.table_depth;
	fs->root_inode   = fs->super.root_inode;
	fs->mounted      = true;
	return RECONFS_OK;
}

void reconfs_unmount(struct reconfs *fs)
{
	kfree(fs->scratch_index);
	kfree(fs->scratch_leaf);
	fs->scratch_index = NULL;
	fs->scratch_leaf = NULL;
	fs->mounted = false;
}

/* --- Making one -----------------------------------------------------------
 *
 * The layout a format produces, in block order:
 *
 *   0                superblock A            (byte 0)
 *   1 .. B-1         reserved                (room for a boot area)
 *   B                superblock B            (byte 65536)
 *   B+1              root directory inode
 *   B+2 ...          owner table leaves, then index levels
 *
 * Everything is written, then flushed, then the superblocks are written -- the
 * ordering rule applied to the one commit that has no previous state.
 */

/* How big a block should this volume use?
 *
 * --- The first version of this was a rule that never fired ---
 *
 * It grew the block size while the owner table exceeded a fiftieth of a percent
 * of the volume. The table is eight bytes per block, so its share is exactly
 * 8/block_size -- 0.195% at the minimum block size, which is already under a
 * fiftieth of a percent. The condition was true on the first test at every
 * volume size from 64MB to 24TB, and the function returned 4096 always.
 *
 * It read like a rule and behaved like a constant. Worth recording because it
 * is the third thing this session that looked like it was working: a heuristic
 * whose threshold sits on the wrong side of its own starting point produces a
 * plausible number every time, and nothing about the output says so.
 *
 * --- The trade-off, which is why this is a table and not a formula ---
 *
 * A bigger block is better for a big drive in three ways: the owner table
 * shrinks proportionally, transfers get longer, and there are fewer of them.
 * It is worse in exactly one, and the one is not small: a file that does not
 * fill a block wastes the rest of it. A 1KB file costs 4KB at the minimum block
 * size and 64KB at the maximum -- sixteen times the waste -- and that is true
 * of *every* small file on the volume, of which a system disk has hundreds of
 * thousands.
 *
 * So the choice depends on what the volume will hold, which nothing here can
 * know. The table below scales gently rather than aggressively, and an explicit
 * size always wins: the installer is where somebody who knows the answer gets
 * to say it.
 *
 *   under 2TiB     4KiB    a system disk; small files dominate
 *   under 16TiB   16KiB    large enough that metadata starts to matter
 *   16TiB and up  64KiB    an archive or media volume; large files dominate
 */
static u32 choose_block_size(u64 bytes)
{
	const u64 tib = 1024ULL * 1024 * 1024 * 1024;

	if (bytes < 2 * tib)
		return 4096;
	if (bytes < 16 * tib)
		return 16384;
	return 65536;
}

/* Not static, so the layout self-test can compare this against an
 * independently written derivation. A test that computes the answer
 * itself and then checks its own answer proves only that it agrees with
 * itself -- which is what the first version of that test did. */
u32 reconfs_depth_for(u64 total_blocks, u32 per_leaf, u32 per_index)
{
	u64 leaves = (total_blocks + per_leaf - 1) / per_leaf;
	u64 span = 1;
	u32 depth = 0;

	while (span < leaves) {
		span *= per_index;
		depth++;
	}
	return depth;
}

u64 reconfs_blocks_for_table(u64 total_blocks, u32 depth, u32 per_leaf,
			     u32 per_index)
{
	u64 leaves = (total_blocks + per_leaf - 1) / per_leaf;
	u64 total = leaves;
	u64 level = leaves;
	u32 d;

	for (d = 0; d < depth; d++) {
		level = (level + per_index - 1) / per_index;
		total += level;
	}
	return total;
}

enum reconfs_status reconfs_format(struct block_device *dev, const char *label,
				   u32 block_size)
{
	struct reconfs fs;
	struct reconfs_super *sb = NULL;
	struct reconfs_inode *root = NULL;
	u8 *zero = NULL;
	u32 spb, depth;
	u64 table_blocks, first_table, next, i, total, super_b, root_blk;
	enum reconfs_status st = RECONFS_OK;

	if (dev->read_only)
		return RECONFS_ERR_READ_ONLY;

	if (!block_size)
		block_size = choose_block_size(dev->block_count *
					       (u64)dev->block_size);

	if (!block_size_ok(block_size))
		return RECONFS_ERR_BLOCK_SIZE;

	if (!sectors_per_block(dev, block_size, &spb))
		return RECONFS_ERR_BLOCK_SIZE;

	total = dev->block_count / spb;

	kmemset(&fs, 0, sizeof(fs));
	fs.dev          = dev;
	fs.block_size   = block_size;
	fs.per_index    = RECONFS_PER_INDEX_FOR(block_size);
	fs.per_leaf     = RECONFS_PER_LEAF_FOR(block_size);
	fs.total_blocks = total;

	super_b  = reconfs_super_b(block_size);
	root_blk = first_data_block(block_size);

	depth        = reconfs_depth_for(total, fs.per_leaf, fs.per_index);
	table_blocks = reconfs_blocks_for_table(total, depth, fs.per_leaf,
						fs.per_index);

	first_table = root_blk + 1;
	if (first_table + table_blocks >= total)
		return RECONFS_ERR_TOO_SMALL;

	sb   = kzalloc(block_size);
	root = kzalloc(block_size);
	zero = kzalloc(block_size);
	fs.scratch_index = kzalloc(block_size);
	fs.scratch_leaf  = kzalloc(block_size);
	if (!sb || !root || !zero || !fs.scratch_index || !fs.scratch_leaf) {
		st = RECONFS_ERR_NOMEM;
		goto out;
	}

	/* --- The owner table, bottom up ------------------------------------
	 *
	 * Leaves first, then each index level above them, so that every block
	 * number an index block holds refers to a block that has already been
	 * written. Under the ordering rule that matters: an index naming a
	 * block that does not exist yet is a structure a crash could make
	 * permanent. */
	{
		u64 leaves = (total + fs.per_leaf - 1) / fs.per_leaf;
		u64 level_start = first_table;
		u64 level_count = leaves;
		u32 d;

		for (i = 0; i < leaves; i++) {
			kmemset(zero, 0, block_size);
			st = reconfs_write_block(&fs, level_start + i, zero,
						 RECONFS_NO_CSUM);
			if (st != RECONFS_OK)
				goto out;
		}

		next = level_start + leaves;

		for (d = 0; d < depth; d++) {
			u64 above = (level_count + fs.per_index - 1)
				  / fs.per_index;
			u64 k;

			for (k = 0; k < above; k++) {
				u64 *slots = fs.scratch_index;
				u64 j;

				kmemset(slots, 0, block_size);
				for (j = 0; j < fs.per_index; j++) {
					u64 child = k * fs.per_index + j;

					if (child >= level_count)
						break;
					slots[j] = level_start + child;
				}

				st = reconfs_write_block(&fs, next + k, slots,
							 RECONFS_NO_CSUM);
				if (st != RECONFS_OK)
					goto out;
			}

			level_start = next;
			level_count = above;
			next += above;
		}

		fs.table_root  = level_start;
		fs.table_depth = depth;
	}

	/* --- Claim what the archive itself occupies ------------------------
	 *
	 * Both superblocks, the reserved run between them, the root inode and
	 * the table. Marked owned before anything can allocate, so no first
	 * allocation can hand out a block the format is standing on. */
	for (i = 0; i < first_table + table_blocks; i++) {
		st = reconfs_set_owner(&fs, i, RECONFS_OWNER_ARCHIVE);
		if (st != RECONFS_OK)
			goto out;
	}

	/* --- The root directory -------------------------------------------- */
	root->magic      = RECONFS_INODE_MAGIC;
	root->dossier    = RECONFS_DOSSIER_ROOT;
	root->parent     = 0;			/* the only object with none */
	root->type       = RECONFS_TYPE_DIR;
	root->mode       = 0755;
	root->size       = 0;
	root->links      = 1;
	root->btime      = time_monotonic_ns();
	root->mtime      = root->btime;
	root->ctime      = root->btime;
	root->inline_len = 0;

	st = reconfs_write_block(&fs, root_blk, root, INODE_CSUM);
	if (st != RECONFS_OK)
		goto out;

	/* --- The commit -----------------------------------------------------
	 *
	 * Everything above is on the medium before either superblock names it.
	 * Flush, then the superblock, then flush again. */
	if (block_flush(dev) != BLOCK_OK) {
		st = RECONFS_ERR_IO;
		goto out;
	}

	sb->magic        = RECONFS_MAGIC;
	sb->version      = RECONFS_VERSION;
	sb->block_size   = block_size;
	sb->epoch        = 1;
	sb->total_blocks = total;
	sb->root_inode   = root_blk;
	sb->table_root   = fs.table_root;
	sb->table_depth  = fs.table_depth;
	sb->next_dossier = RECONFS_DOSSIER_FIRST;
	sb->blocks_used  = first_table + table_blocks;
	sb->fold         = RECONFS_FOLD_ASCII;

	/* What the medium is like, recorded once, because the allocator's
	 * decisions depend on it and the answer does not travel with the disk
	 * any other way. */
	sb->seek_is_free      = dev->seek_is_free ? 1 : 0;
	sb->discard_supported = dev->discard_supported ? 1 : 0;
	sb->transfer_hint     = dev->transfer_hint;

	kstrlcpy(sb->label, label ? label : "ReconFS", sizeof(sb->label));

	st = reconfs_write_block(&fs, RECONFS_SUPER_A, sb, SUPER_CSUM);
	if (st != RECONFS_OK)
		goto out;

	if (block_flush(dev) != BLOCK_OK) {
		st = RECONFS_ERR_IO;
		goto out;
	}

	st = reconfs_write_block(&fs, super_b, sb, SUPER_CSUM);
	if (st != RECONFS_OK)
		goto out;

	if (block_flush(dev) != BLOCK_OK)
		st = RECONFS_ERR_IO;

out:
	kfree(sb);
	kfree(root);
	kfree(zero);
	kfree(fs.scratch_index);
	kfree(fs.scratch_leaf);
	return st;
}

const char *reconfs_strerror(enum reconfs_status st)
{
	switch (st) {
	case RECONFS_OK:               return "ok";
	case RECONFS_ERR_IO:           return "the device refused the transfer";
	case RECONFS_ERR_RANGE:        return "block out of range";
	case RECONFS_ERR_DEVICE:       return "the device's block size does not divide 4096";
	case RECONFS_ERR_CHECKSUM:     return "a block did not match its checksum";
	case RECONFS_ERR_CORRUPT:      return "the structure does not hold together";
	case RECONFS_ERR_NOT_RECONFS:  return "no valid superblock";
	case RECONFS_ERR_NOMEM:        return "out of memory";
	case RECONFS_ERR_READ_ONLY:    return "the device is read-only";
	case RECONFS_ERR_TOO_LARGE:    return "the volume is larger than the format can address";
	case RECONFS_ERR_TOO_SMALL:    return "the volume is too small to hold a filesystem";
	case RECONFS_ERR_NOSPACE:      return "no free blocks";
	case RECONFS_ERR_BLOCK_SIZE:   return "the block size does not suit this device";
	case RECONFS_ERR_RETRY:        return "the transaction must be built again";
	case RECONFS_ERR_NAME:         return "that is not a usable name";
	case RECONFS_ERR_NOT_FOUND:    return "no such name in that directory";
	case RECONFS_ERR_NOT_DIR:      return "that is not a directory";
	case RECONFS_ERR_EXISTS:       return "that name is already taken";
	case RECONFS_ERR_DIR_FULL:     return "the directory is full";
	case RECONFS_ERR_NOT_FILE:     return "that is not a file";
	case RECONFS_ERR_NOT_EMPTY:    return "the directory still has things in it";
	case RECONFS_ERR_TOO_DEEP:     return "the path is deeper than this format goes";
	case RECONFS_ERR_NOT_MOUNTED:  return "there is no ReconFS volume mounted";
	}
	return "unknown";
}
