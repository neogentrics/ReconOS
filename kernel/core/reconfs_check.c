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
 * So the independence is built into the format instead. Every allocated block
 * has an owner recorded in a table that the forward walk never reads, and every
 * inode has a `parent` field that the forward walk never reads either. That
 * makes two derivations of one set:
 *
 *   forward   from the root, follow directory entries and block pointers down
 *   reverse   from the owner table, take every allocated block and walk up
 *
 * They use disjoint fields. If they agree, that agreement is evidence. If they
 * had shared a field they would agree by construction, which is worth nothing.
 *
 * --- The rule this file obeys ---
 *
 * The reverse sweep calls nothing the reader uses to resolve a path. It reads
 * blocks and it reads the owner table, and that is all. A sweep that quietly
 * used the reader's lookup would be the reader checking itself with extra
 * steps.
 */
#include <recon/kernel/reconfs.h>

#include <recon/kernel/block.h>
#include <recon/kernel/console.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/kstring.h>

#define INODE_CSUM	RK_OFFSETOF(struct reconfs_inode, checksum)

/* A block's worth of bits per volume. For the volumes this runs on today that
 * is a few hundred bytes; the ceiling is stated rather than discovered. */
#define CHECK_MAX_BLOCKS	(1ULL << 26)	/* 64M blocks = 256GiB */

struct seen {
	u8 *bits;
	u64 count;
};

static bool seen_init(struct seen *s, u64 blocks)
{
	s->bits = kzalloc((size_t)((blocks + 7) / 8));
	s->count = blocks;
	return s->bits != NULL;
}

static void seen_free(struct seen *s)
{
	kfree(s->bits);
	s->bits = NULL;
}

static bool seen_test(const struct seen *s, u64 b)
{
	return b < s->count && (s->bits[b / 8] & (u8)(1u << (b % 8))) != 0;
}

/* Returns true if the bit was already set -- which for the forward walk means
 * one block is reachable by two paths, and that is a corruption, not a
 * shortcut to skip. */
static bool seen_set(struct seen *s, u64 b)
{
	if (b >= s->count)
		return false;
	if (seen_test(s, b))
		return true;
	s->bits[b / 8] |= (u8)(1u << (b % 8));
	return false;
}

struct walk {
	struct reconfs *fs;
	struct seen forward;
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

/* Claim a block for the forward walk. */
static void claim(struct walk *w, u64 blk, const char *what)
{
	if (blk < reconfs_super_b(w->fs->block_size) || blk >= w->fs->total_blocks) {
		disagree(w->r, "a pointer left the volume", blk);
		return;
	}
	if (seen_set(&w->forward, blk))
		disagree(w->r, what, blk);
	else
		w->r->blocks_seen_forward++;
}

/* Walk one file's block pointers, claiming each.
 *
 * The two deeper levels of indirection are part of the format and are not
 * written yet; a file that uses one is reported rather than skipped, because a
 * checker that silently ignores a field is a checker that will agree with a
 * writer that silently corrupts it. */
static enum reconfs_status walk_file(struct walk *w,
				     const struct reconfs_inode *ino)
{
	unsigned i;

	for (i = 0; i < RECONFS_DIRECT; i++)
		if (ino->direct[i])
			claim(w, ino->direct[i], "a data block has two owners");

	if (ino->indirect) {
		u64 *slots = kzalloc(w->fs->block_size);
		enum reconfs_status st;
		unsigned j;

		if (!slots)
			return RECONFS_ERR_NOMEM;

		claim(w, ino->indirect, "an indirect block has two owners");
		st = reconfs_read_block(w->fs, ino->indirect, slots,
					RECONFS_NO_CSUM);
		if (st == RECONFS_OK) {
			for (j = 0; j < w->fs->per_index; j++)
				if (slots[j])
					claim(w, slots[j],
					      "a data block has two owners");
		}
		kfree(slots);
		if (st != RECONFS_OK)
			return st;
	}

	if (ino->double_indirect || ino->triple_indirect)
		disagree(w->r, "a file uses indirection this version cannot write", 0);

	return RECONFS_OK;
}

static enum reconfs_status walk_inode(struct walk *w, u64 blk, u64 expect_parent);

/* Walk a directory's entries. */
static enum reconfs_status walk_dir_block(struct walk *w, u64 dir_blk,
					  const u8 *buf, u32 len)
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
			st = walk_inode(w, de->inode, dir_blk);
			if (st != RECONFS_OK)
				return st;
		}

		off += de->rec_len;
	}

	return RECONFS_OK;
}

static enum reconfs_status walk_inode(struct walk *w, u64 blk, u64 expect_parent)
{
	struct reconfs_inode *ino;
	enum reconfs_status st;

	/* Depth, not a visited set, because a cycle in the directory tree shows
	 * up as a block claimed twice -- which `claim` already reports. This is
	 * only here so a cycle cannot run the stack out before that happens. */
	if (w->depth > 64) {
		disagree(w->r, "the directory tree is deeper than 64", blk);
		return RECONFS_ERR_CORRUPT;
	}

	claim(w, blk, "an inode is reachable by two paths");

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
		disagree(w->r, "a directory entry points at something that is not an inode", blk);
		kfree(ino);
		return RECONFS_ERR_CORRUPT;
	}

	/* The forward walk knows who the parent is because it came from there.
	 * The inode also says. They must match -- this is the one place the two
	 * derivations are allowed to meet, and it is a comparison rather than a
	 * read: nothing downstream depends on `parent` being right. */
	if (ino->parent != expect_parent)
		disagree(w->r, "an inode's parent is not the directory that names it", blk);

	w->r->inodes++;

	if (ino->type == RECONFS_TYPE_DIR) {
		unsigned i;

		w->depth++;
		if (ino->inline_len) {
			st = walk_dir_block(w, blk, ino->data, ino->inline_len);
		} else {
			u8 *db = kzalloc(w->fs->block_size);

			if (!db) {
				st = RECONFS_ERR_NOMEM;
			} else {
				for (i = 0; i < RECONFS_DIRECT; i++) {
					if (!ino->direct[i])
						continue;
					claim(w, ino->direct[i],
					      "a directory block has two owners");
					st = reconfs_read_block(w->fs,
								ino->direct[i],
								db,
								RECONFS_NO_CSUM);
					if (st != RECONFS_OK)
						break;
					st = walk_dir_block(w, blk, db,
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
		disagree(w->r, "an inode has a type this format does not define", blk);
		st = RECONFS_ERR_CORRUPT;
	}

	kfree(ino);
	return st;
}

/* --- The reverse sweep ---------------------------------------------------
 *
 * Every block the owner table says is allocated, taken on its own terms: read
 * its owner, and walk up until the root is reached or the chain fails. Nothing
 * here follows a directory entry, and nothing here resolves a name.
 */
static enum reconfs_status sweep(struct reconfs *fs, struct seen *rev,
				 struct reconfs_check_result *r)
{
	struct reconfs_inode *ino = kzalloc(fs->block_size);
	u64 *leaf;
	u64 b;

	if (!ino)
		return RECONFS_ERR_NOMEM;

	leaf = kzalloc(fs->block_size);
	if (!leaf) {
		kfree(ino);
		return RECONFS_ERR_NOMEM;
	}

	for (b = 0; b < fs->total_blocks; b++) {
		u64 owner;
		enum reconfs_status st;
		u64 up;
		unsigned hops;

		/* A whole leaf at a time. Asking for one owner at a time walks
		 * the tree from the root on every block, which on a volume of
		 * any size is the difference between a check that is slow and
		 * one that never finishes. */
		if (b % fs->per_leaf == 0) {
			st = reconfs_read_leaf(fs, b / fs->per_leaf, leaf);
			if (st != RECONFS_OK) {
				kfree(leaf);
				kfree(ino);
				return st;
			}
		}

		owner = leaf[b % fs->per_leaf];

		if (owner == RECONFS_OWNER_VOID)
			continue;

		seen_set(rev, b);
		r->blocks_seen_reverse++;

		/* The filesystem's own metadata is owned by nothing above it,
		 * and the forward walk never reaches it -- superblocks, the
		 * owner table, the blocks the format stands on. Counted, and
		 * not required to climb. */
		if (owner == RECONFS_OWNER_ARCHIVE)
			continue;

		/* Climb. Each step must land on a real inode, and the chain
		 * must end at the root within a bounded number of hops. */
		up = owner;
		for (hops = 0; hops <= 64; hops++) {
			/* Only the root has no parent, and the root is reached
			 * by the test below rather than by falling off the top.
			 * Arriving at zero therefore means an inode that is not
			 * the root claims to have no parent, and treating that
			 * as "climbed all the way home" -- which the first
			 * version of this loop did -- accepts exactly the
			 * corruption the sweep exists to find. */
			if (up == 0) {
				disagree(r, "an owner chain ends at an inode with no parent", b);
				break;
			}

			if (up < reconfs_super_b(fs->block_size) || up >= fs->total_blocks) {
				disagree(r, "an owner points outside the volume", b);
				break;
			}

			st = reconfs_read_block(fs, up, ino, INODE_CSUM);
			if (st != RECONFS_OK) {
				disagree(r, "an owner points at a block that does not read", b);
				break;
			}

			if (ino->magic != RECONFS_INODE_MAGIC) {
				disagree(r, "an owner points at something that is not an inode", b);
				break;
			}

			if (up == fs->root_inode)
				break;		/* home */

			up = ino->parent;
		}

		if (hops > 64)
			disagree(r, "an owner chain does not reach the root", b);
	}

	kfree(leaf);
	kfree(ino);
	return RECONFS_OK;
}

enum reconfs_status reconfs_check(struct reconfs *fs,
				  struct reconfs_check_result *out)
{
	struct walk w;
	struct seen rev;
	enum reconfs_status st;
	u64 b;

	kmemset(out, 0, sizeof(*out));
	kmemset(&w, 0, sizeof(w));

	if (!fs->mounted)
		return RECONFS_ERR_NOT_RECONFS;

	if (fs->total_blocks > CHECK_MAX_BLOCKS) {
		/* Refused rather than run on a subset. A checker that quietly
		 * examines part of a volume and reports "no disagreements" is
		 * the failure this whole file exists to avoid. */
		kputs("reconfs: volume too large to check with the memory available\n");
		return RECONFS_ERR_NOMEM;
	}

	w.fs = fs;
	w.r = out;

	if (!seen_init(&w.forward, fs->total_blocks))
		return RECONFS_ERR_NOMEM;
	if (!seen_init(&rev, fs->total_blocks)) {
		seen_free(&w.forward);
		return RECONFS_ERR_NOMEM;
	}

	st = walk_inode(&w, fs->root_inode, 0);
	if (st == RECONFS_OK)
		st = sweep(fs, &rev, out);

	/* The comparison. Metadata the format stands on is reachable only from
	 * the reverse side by design, so it is excluded by its owner rather
	 * than by position -- a rule about *what a block is* rather than about
	 * where it happens to sit. */
	if (st == RECONFS_OK) {
		for (b = 0; b < fs->total_blocks; b++) {
			bool f = seen_test(&w.forward, b);
			bool rv = seen_test(&rev, b);
			u64 owner;

			if (f == rv)
				continue;

			if (reconfs_owner_of(fs, b, &owner) == RECONFS_OK &&
			    owner == RECONFS_OWNER_ARCHIVE && rv && !f)
				continue;

			disagree(out, f ? "reachable from the root but not allocated"
				       : "allocated but not reachable from the root", b);
		}
	}

	seen_free(&w.forward);
	seen_free(&rev);
	return st;
}
