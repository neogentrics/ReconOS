/* ReconFS -- the checker, which is two derivations that must agree.
 *
 * --- Why this exists at all ---
 *
 * Nobody else implements ReconFS. For partition tables the reader could be
 * checked against disks written by sgdisk and sfdisk, which share no code with
 * this and no misunderstanding with it. Here there is no second implementation,
 * and testing a reader against its own writer is the failure mode
 * scripts/make-partition-fixtures.sh opens by naming: a writer and a reader
 * built from the same misunderstanding agree perfectly.
 *
 * So the independence is built into the format instead. Two derivations of one
 * fact -- which object owns which block -- from disjoint fields:
 *
 *   forward   from the root, follow directory entries and block pointers down,
 *             reading pointers and nothing else
 *   reverse   read the owner table, and nothing else
 *
 * Neither reads a field the other reads. If they agree, that agreement is
 * evidence. If they had shared a field they would agree by construction, which
 * is worth nothing.
 *
 * --- The rule this file obeys ---
 *
 * The reverse sweep calls nothing the reader uses to resolve a path. It reads
 * owner-table leaves and nothing else. A sweep that quietly used the reader's
 * lookup would be the reader checking itself with extra steps.
 */
#include <recon/kernel/reconfs.h>

#include <recon/kernel/block.h>
#include <recon/kernel/console.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/kstring.h>

#define INODE_CSUM	RK_OFFSETOF(struct reconfs_inode, checksum)

/* The forward walk keeps one dossier number per block on the volume, so the two
 * derivations can be compared entry by entry rather than as two sets.
 *
 * That costs eight bytes a block, which is what caps the volume this can check
 * in one pass. A larger volume needs the check done in segments -- reading the
 * table once per segment and walking the tree once per segment -- which is a
 * real piece of work and is not written. The cap is enforced rather than
 * exceeded, because a checker that quietly examines part of a volume and
 * reports no disagreements is the failure this whole file exists to avoid. */
#define CHECK_MAX_BLOCKS	(1ULL << 22)	/* 16GiB at a 4KiB block */

struct walk {
	struct reconfs *fs;
	u64 *by;		/* by[block] = the dossier the tree says owns it */
	struct reconfs_check_result *r;
	u32 depth;
};

static void disagree(struct reconfs_check_result *r, const char *why, u64 blk)
{
	if (!r->disagreements) {
		r->first_disagreement = why;
		r->first_disagreement_block = blk;
	}
	r->disagreements++;
}

/* Records that `owner` reaches `blk`. A block reached twice is a corruption,
 * not a shortcut to skip. */
static void claim(struct walk *w, u64 blk, u64 owner, const char *what)
{
	if (blk < reconfs_super_b(w->fs->block_size) ||
	    blk >= w->fs->total_blocks) {
		disagree(w->r, "a pointer left the volume", blk);
		return;
	}

	if (w->by[blk]) {
		disagree(w->r, what, blk);
		return;
	}

	w->by[blk] = owner;
	w->r->blocks_seen_forward++;
}

static enum reconfs_status walk_file(struct walk *w,
				     const struct reconfs_inode *ino)
{
	unsigned i;

	for (i = 0; i < RECONFS_DIRECT; i++)
		if (ino->direct[i])
			claim(w, ino->direct[i], ino->dossier,
			      "a data block is reachable twice");

	if (ino->indirect) {
		u64 *slots = kzalloc(w->fs->block_size);
		enum reconfs_status st;
		unsigned j;

		if (!slots)
			return RECONFS_ERR_NOMEM;

		claim(w, ino->indirect, ino->dossier,
		      "an indirect block is reachable twice");

		st = reconfs_read_block(w->fs, ino->indirect, slots,
					RECONFS_NO_CSUM);
		if (st == RECONFS_OK) {
			for (j = 0; j < w->fs->per_index; j++)
				if (slots[j])
					claim(w, slots[j], ino->dossier,
					      "a data block is reachable twice");
		}
		kfree(slots);
		if (st != RECONFS_OK)
			return st;
	}

	/* Part of the format, not written by this version. A file that used one
	 * is reported rather than skipped: a checker that silently ignores a
	 * field will agree with a writer that silently corrupts it. */
	if (ino->double_indirect || ino->triple_indirect)
		disagree(w->r, "a file uses indirection this version cannot write", 0);

	return RECONFS_OK;
}

static enum reconfs_status walk_inode(struct walk *w, u64 blk,
				      u64 expect_parent, u64 owner);

static enum reconfs_status walk_dir_block(struct walk *w, u64 dir_blk,
					  u64 dir_dossier, const u8 *buf,
					  u32 len)
{
	u32 off = 0;

	while (off + RECONFS_DIRENT_MIN <= len) {
		const struct reconfs_dirent *de =
			(const struct reconfs_dirent *)(buf + off);
		enum reconfs_status st;

		if (!de->rec_len)
			break;			/* padding to the block end */

		if (de->rec_len < RECONFS_DIRENT_MIN ||
		    off + de->rec_len > len ||
		    (u32)de->name_len + RECONFS_DIRENT_MIN > de->rec_len) {
			disagree(w->r, "a directory entry does not fit its block",
				 dir_blk);
			return RECONFS_ERR_CORRUPT;
		}

		if (de->inode) {
			st = walk_inode(w, de->inode, dir_dossier, dir_dossier);
			if (st != RECONFS_OK)
				return st;
		}

		off += de->rec_len;
	}

	return RECONFS_OK;
}

/* `owner` is the dossier that should be recorded against this inode's own
 * block -- its parent's, or the archive for the root. */
static enum reconfs_status walk_inode(struct walk *w, u64 blk,
				      u64 expect_parent, u64 owner)
{
	struct reconfs_inode *ino;
	enum reconfs_status st;

	/* Depth, not a visited set: a cycle shows up as a block reached twice,
	 * which `claim` already reports. This is only here so a cycle cannot run
	 * the stack out before that happens. */
	if (w->depth > 64) {
		disagree(w->r, "the directory tree is deeper than 64", blk);
		return RECONFS_ERR_CORRUPT;
	}

	claim(w, blk, owner, "an inode is reachable by two paths");

	ino = kzalloc(w->fs->block_size);
	if (!ino)
		return RECONFS_ERR_NOMEM;

	st = reconfs_read_block(w->fs, blk, ino, INODE_CSUM);
	if (st != RECONFS_OK) {
		disagree(w->r, "an inode did not read", blk);
		kfree(ino);
		return st;
	}

	if (ino->magic != RECONFS_INODE_MAGIC) {
		disagree(w->r, "a directory entry points at something that is "
			       "not an inode", blk);
		kfree(ino);
		return RECONFS_ERR_CORRUPT;
	}

	if (!ino->dossier) {
		disagree(w->r, "an inode has no dossier number", blk);
		kfree(ino);
		return RECONFS_ERR_CORRUPT;
	}

	/* The forward walk knows who the parent is because it came from there.
	 * The inode also says. They must match -- the one place the two
	 * derivations are allowed to meet, and it is a comparison rather than a
	 * read: nothing downstream depends on `parent` being right. */
	if (ino->parent != expect_parent)
		disagree(w->r, "an inode's parent is not the directory that "
			       "names it", blk);

	w->r->inodes++;

	if (ino->type == RECONFS_TYPE_DIR) {
		unsigned i;

		w->depth++;
		if (ino->inline_len) {
			st = walk_dir_block(w, blk, ino->dossier, ino->data,
					    ino->inline_len);
		} else {
			u8 *db = kzalloc(w->fs->block_size);

			if (!db) {
				st = RECONFS_ERR_NOMEM;
			} else {
				for (i = 0; i < RECONFS_DIRECT; i++) {
					if (!ino->direct[i])
						continue;
					claim(w, ino->direct[i], ino->dossier,
					      "a directory block is reachable twice");
					st = reconfs_read_block(w->fs,
								ino->direct[i],
								db,
								RECONFS_NO_CSUM);
					if (st != RECONFS_OK)
						break;
					st = walk_dir_block(w, blk,
							    ino->dossier, db,
							    w->fs->block_size);
					if (st != RECONFS_OK)
						break;
				}
				kfree(db);
			}
		}
		w->depth--;
	} else if (ino->type == RECONFS_TYPE_FILE) {
		if (!ino->inline_len)
			st = walk_file(w, ino);
	} else {
		disagree(w->r, "an inode has a type this format does not define",
			 blk);
		st = RECONFS_ERR_CORRUPT;
	}

	kfree(ino);
	return st;
}

/* Claims the blocks the owner table itself occupies: its index blocks and its
 * leaves, from the root down.
 *
 * This is the forward walk's business, not the sweep's -- it reads the table's
 * *structure*, following pointers, which is what the forward walk does. The
 * sweep still reads only the table's contents. The two stay disjoint.
 */
static enum reconfs_status claim_table(struct walk *w, u64 node, u32 depth)
{
	u64 *slots;
	enum reconfs_status st;
	u32 i;

	if (node >= w->fs->total_blocks) {
		disagree(w->r, "the owner table leaves the volume", node);
		return RECONFS_ERR_CORRUPT;
	}

	if (w->by[node]) {
		disagree(w->r, "an owner table block is reachable twice", node);
		return RECONFS_ERR_CORRUPT;
	}
	w->by[node] = RECONFS_OWNER_ARCHIVE;
	w->r->blocks_seen_forward++;

	if (depth == 0)
		return RECONFS_OK;

	slots = kzalloc(w->fs->block_size);
	if (!slots)
		return RECONFS_ERR_NOMEM;

	st = reconfs_read_block(w->fs, node, slots, RECONFS_NO_CSUM);
	if (st == RECONFS_OK) {
		for (i = 0; i < w->fs->per_index; i++) {
			if (!slots[i])
				continue;
			st = claim_table(w, slots[i], depth - 1);
			if (st != RECONFS_OK)
				break;
		}
	}

	kfree(slots);
	return st;
}

/* --- The reverse sweep, and the comparison --------------------------------
 *
 * Reads the owner table, a whole leaf at a time, and compares each entry with
 * what the forward walk recorded. Nothing here follows a directory entry, reads
 * an inode, or resolves a name.
 */
static enum reconfs_status sweep_and_compare(struct reconfs *fs,
					     const u64 *by,
					     struct reconfs_check_result *r)
{
	u64 *leaf = kzalloc(fs->block_size);
	u64 b;

	if (!leaf)
		return RECONFS_ERR_NOMEM;

	for (b = 0; b < fs->total_blocks; b++) {
		u64 actual, expected;

		/* A whole leaf at a time. Asking for one owner at a time walks
		 * the tree from the root on every block, which on a volume of
		 * any size is the difference between a check that is slow and
		 * one that never finishes. */
		if (b % fs->per_leaf == 0) {
			enum reconfs_status st;

			st = reconfs_read_leaf(fs, b / fs->per_leaf, leaf);
			if (st != RECONFS_OK) {
				kfree(leaf);
				return st;
			}
		}

		actual = leaf[b % fs->per_leaf];
		expected = by[b];

		if (actual != RECONFS_OWNER_VOID)
			r->blocks_seen_reverse++;

		if (actual == expected)
			continue;

		if (actual == RECONFS_OWNER_VOID)
			disagree(r, "reachable from the root but not allocated", b);
		else if (!expected)
			disagree(r, "allocated but not reachable from the root", b);
		else
			disagree(r, "allocated to one object and reachable from "
				    "another", b);
	}

	kfree(leaf);
	return RECONFS_OK;
}

enum reconfs_status reconfs_check(struct reconfs *fs,
				  struct reconfs_check_result *out)
{
	struct walk w;
	enum reconfs_status st;

	kmemset(out, 0, sizeof(*out));
	kmemset(&w, 0, sizeof(w));

	if (!fs->mounted)
		return RECONFS_ERR_NOT_RECONFS;

	if (fs->total_blocks > CHECK_MAX_BLOCKS) {
		kputs("reconfs: volume too large to check in one pass with the "
		      "memory available\n");
		return RECONFS_ERR_NOMEM;
	}

	w.fs = fs;
	w.r  = out;
	w.by = kzalloc((size_t)(fs->total_blocks * sizeof(u64)));
	if (!w.by)
		return RECONFS_ERR_NOMEM;

	/* --- The archive's own blocks, accounted for like everything else ---
	 *
	 * The comparison used to *exempt* every block owned by the archive from
	 * needing to be reachable, on the grounds that the superblocks and the
	 * owner table are owned by nothing above them. That is true of those
	 * blocks and of nothing else -- and the exemption applied to any block
	 * whose owner happened to be the archive.
	 *
	 * Which included every stale copy of the root directory. Each commit
	 * writes a new root inode, and nothing released the old one (KF-122):
	 * a leaked block on every create, rename, write and remove, owned by
	 * the archive because the root has no parent, and therefore skipped by
	 * the one check that would have named it.
	 *
	 * So the archive's blocks are claimed here, explicitly, and the
	 * exemption is gone. Every allocated block on the volume must be
	 * reachable from something.
	 */
	{
		u64 b, first = reconfs_super_b(fs->block_size) + 1;

		/* Both superblocks and the run reserved between them. */
		for (b = 0; b < first; b++) {
			if (b >= fs->total_blocks)
				break;
			w.by[b] = RECONFS_OWNER_ARCHIVE;
			out->blocks_seen_forward++;
		}

		st = claim_table(&w, fs->table_root, fs->table_depth);
		if (st != RECONFS_OK) {
			kfree(w.by);
			return st;
		}
	}

	/* The root's own block is archive-owned: it has no parent, and that is
	 * what "no parent" looks like in the table. */
	st = walk_inode(&w, fs->root_inode, 0, RECONFS_OWNER_ARCHIVE);
	if (st == RECONFS_OK)
		st = sweep_and_compare(fs, w.by, out);

	kfree(w.by);
	return st;
}
