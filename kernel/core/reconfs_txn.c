/* ReconFS -- the commit.
 *
 * This is where the ordering rule stops being a sentence and becomes something
 * a reviewer can check line by line:
 *
 *     Nothing reachable from a live superblock is ever overwritten, and a
 *     change becomes real when -- and only when -- a superblock naming it is
 *     written after a flush that has already put everything it names on the
 *     medium.
 *
 * Every write in this file is one of exactly two kinds, and there is no third:
 *
 *   1. a write to a block that `txn_alloc` handed out during this transaction,
 *      which by construction no live superblock can reach, because the live
 *      superblock's owner table says that block is free;
 *   2. the superblock write in `reconfs_commit`, which happens after a flush
 *      and is followed by another.
 *
 * If a future change adds a write that is neither, the rule is broken and the
 * filesystem stops being crash-safe. That is the one thing to look for when
 * reading this file.
 *
 * --- The chicken and the egg ---
 *
 * The owner table records which blocks are allocated, and the owner table is
 * itself made of blocks, which are allocated. Copying a table leaf on write
 * needs a new block, and taking that new block changes a table leaf.
 *
 * So allocation happens entirely in memory, against images of the leaves this
 * transaction has touched, and nothing is written until every block the
 * transaction will need has been decided. Phases, in order:
 *
 *   plan    allocate every block: the new copies of the dirty leaves, and the
 *           new copies of every index block on a path to one
 *   leaves  write the leaf images to the blocks planned for them
 *   index   write the index blocks, bottom up, so no index block is ever
 *           written naming a block that does not exist yet
 *   commit  flush, superblock, flush
 *
 * Writing an index block before the leaf it names would leave, after a crash,
 * a structure pointing at a block that was never written -- which the new
 * superblock would then make permanent.
 */
#include <recon/kernel/reconfs.h>

#include <recon/kernel/block.h>
#include <recon/kernel/console.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/time.h>

#define SUPER_CSUM	RK_OFFSETOF(struct reconfs_super, checksum)

/* How many table leaves one transaction may touch, and how many index blocks
 * may be rewritten because of them.
 *
 * Bounded on purpose. A transaction that needs more is *refused*, not grown:
 * an allocator that quietly takes more memory under load is how a filesystem
 * fails at the moment it is most needed. Sixteen leaves is 16,384 blocks of
 * allocation change in one commit, which is far past anything a single file
 * operation does. */
#define TXN_LEAVES	16
#define TXN_INDEX	64
#define TXN_FREED	256

struct txn_leaf {
	u64 index;		/* which leaf: block number / RECONFS_PER_LEAF */
	u64 old_block;
	u64 new_block;		/* 0 until planned */
	u64 *data;		/* the leaf's contents, in memory */
	bool dirty;
};

struct txn_map {
	u64 old_block;
	u64 new_block;
};

struct reconfs_txn {
	struct reconfs *fs;

	struct txn_leaf leaf[TXN_LEAVES];
	unsigned leaves;

	struct txn_map index[TXN_INDEX];
	unsigned indexes;

	u64 hint;		/* where the last search stopped */

	/* Blocks this transaction released.
	 *
	 * Kept for two reasons, and the second one is a correctness rule rather
	 * than an optimisation:
	 *
	 *   1. the drive is told about them once the commit lands;
	 *   2. **this transaction must not allocate them.**
	 *
	 * See reconfs_txn_alloc for why (2) matters. The list is bounded, and
	 * overflowing it fails the transaction rather than silently dropping
	 * the exclusion -- an allocator that cannot prove a block is safe to
	 * hand out must not hand it out. */
	u64 freed[TXN_FREED];
	unsigned freed_count;
	u64 new_table_root;
	u64 new_root_inode;
	u64 next_dossier;
	u64 blocks_used;

	bool failed;
};

/* --- Leaf images ---------------------------------------------------------- */

static u64 leaf_index_of(const struct reconfs *fs, u64 blk)
{
	return blk / fs->per_leaf;
}

/* Walk the live tree to find where a leaf currently lives. Reads only; the
 * transaction's own changes are held in the leaf images, not on the medium. */
static u64 live_leaf_block(struct reconfs *fs, u64 leaf_index,
			   enum reconfs_status *st)
{
	u64 node = fs->table_root;
	u32 depth = fs->table_depth;
	u64 span = 1;
	u64 idx = leaf_index;
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
		slot = idx / span;
		if (slot >= fs->per_index) {
			*st = RECONFS_ERR_CORRUPT;
			return 0;
		}
		node = slots[slot];
		idx %= span;

		if (!node) {
			*st = RECONFS_ERR_CORRUPT;
			return 0;
		}
	}

	*st = RECONFS_OK;
	return node;
}

static struct txn_leaf *leaf_for(struct reconfs_txn *txn, u64 blk)
{
	u64 want = leaf_index_of(txn->fs, blk);
	enum reconfs_status st;
	struct txn_leaf *l;
	unsigned i;

	for (i = 0; i < txn->leaves; i++)
		if (txn->leaf[i].index == want)
			return &txn->leaf[i];

	if (txn->leaves == TXN_LEAVES) {
		txn->failed = true;
		return NULL;
	}

	l = &txn->leaf[txn->leaves];
	l->index = want;
	l->new_block = 0;
	l->dirty = false;
	l->data = kzalloc(txn->fs->block_size);
	if (!l->data) {
		txn->failed = true;
		return NULL;
	}

	l->old_block = live_leaf_block(txn->fs, want, &st);
	if (st != RECONFS_OK) {
		kfree(l->data);
		l->data = NULL;
		txn->failed = true;
		return NULL;
	}

	st = reconfs_read_block(txn->fs, l->old_block, l->data, RECONFS_NO_CSUM);
	if (st != RECONFS_OK) {
		kfree(l->data);
		l->data = NULL;
		txn->failed = true;
		return NULL;
	}

	txn->leaves++;
	return l;
}

static u64 txn_owner_get(struct reconfs_txn *txn, u64 blk)
{
	struct txn_leaf *l = leaf_for(txn, blk);

	if (!l)
		return RECONFS_OWNER_ARCHIVE;	/* never hand out a block we
						 * could not read the state of */
	return l->data[blk % txn->fs->per_leaf];
}

static void txn_owner_set(struct reconfs_txn *txn, u64 blk, u64 owner)
{
	struct txn_leaf *l = leaf_for(txn, blk);

	if (!l)
		return;

	l->data[blk % txn->fs->per_leaf] = owner;
	l->dirty = true;
}

/* --- Allocation ----------------------------------------------------------- */

/* Defined below, beside reconfs_txn_free, which is where the list it consults
 * is filled in. */
static bool was_freed_here(const struct reconfs_txn *txn, u64 blk);

/* Hands out a block that the live superblock's owner table says is free.
 *
 * That is the whole safety argument for every write in this file except the
 * superblock: a block the live table calls free is a block no live superblock
 * can reach, so writing it cannot damage the filesystem that exists.
 *
 * --- The exception, which broke that argument ---
 *
 * A block *this transaction* released is marked unclaimed in the owner table
 * images immediately, because the next transaction must be able to use it. But
 * the live superblock still reaches it, and will keep reaching it until the new
 * superblock is durable.
 *
 * So a transaction that freed a block and then allocated the same block would
 * write into storage the current filesystem still points at -- and a crash
 * before the commit would leave the old superblock, still the truth at that
 * moment, naming a block this transaction had already overwritten. Silent
 * corruption, in the one place the whole design exists to prevent it.
 *
 * It was reachable: the allocator scans forward from a hint and wraps at the end
 * of the volume, and after wrapping it would walk straight back over everything
 * this transaction had just released.
 *
 * Hence the exclusion below. A freed block becomes available to the *next*
 * transaction, never to this one. */
u64 reconfs_txn_alloc(struct reconfs_txn *txn, u64 owner)
{
	u64 scanned = 0;
	u64 blk = txn->hint;

	if (txn->failed)
		return 0;

	while (scanned < txn->fs->total_blocks) {
		if (blk >= txn->fs->total_blocks)
			blk = reconfs_super_b(txn->fs->block_size) + 1;

		if (txn_owner_get(txn, blk) == RECONFS_OWNER_VOID &&
		    !was_freed_here(txn, blk)) {
			if (txn->failed)
				return 0;
			txn_owner_set(txn, blk, owner);
			txn->hint = blk + 1;
			txn->blocks_used++;
			return blk;
		}

		if (txn->failed)
			return 0;

		blk++;
		scanned++;
	}

	/* Nothing available. Zero is returned and the transaction is *not*
	 * marked failed, because a full volume is not a broken transaction: the
	 * caller may well have another way to proceed, and every caller already
	 * has to check the return value anyway.
	 *
	 * This used to poison the transaction, which made "the volume is full"
	 * indistinguishable from "something went wrong inside the allocator" --
	 * and made it impossible to write a test that fills a volume on purpose,
	 * which is exactly what the exclusion above needs to be tested. */
	return 0;
}

void reconfs_txn_free(struct reconfs_txn *txn, u64 blk)
{
	if (txn->failed || !blk)
		return;

	txn_owner_set(txn, blk, RECONFS_OWNER_VOID);
	if (txn->blocks_used)
		txn->blocks_used--;

	if (txn->freed_count == TXN_FREED) {
		/* The exclusion list is what keeps this transaction from
		 * writing into a block the live superblock still reaches.
		 * Without room for another entry there is no way to promise
		 * that, so the transaction fails instead. */
		txn->failed = true;
		return;
	}

	txn->freed[txn->freed_count++] = blk;
}

/* Did this transaction already release this block? */
static bool was_freed_here(const struct reconfs_txn *txn, u64 blk)
{
	unsigned i;

	for (i = 0; i < txn->freed_count; i++)
		if (txn->freed[i] == blk)
			return true;
	return false;
}

/* --- Beginning and abandoning --------------------------------------------- */

struct reconfs_txn *reconfs_txn_begin(struct reconfs *fs)
{
	struct reconfs_txn *txn;

	if (!fs->mounted)
		return NULL;

	txn = kzalloc(sizeof(*txn));
	if (!txn)
		return NULL;

	txn->fs = fs;
	txn->hint = reconfs_super_b(fs->block_size) + 1;
	txn->new_root_inode = fs->root_inode;
	txn->next_dossier = fs->super.next_dossier;
	txn->blocks_used = fs->super.blocks_used;
	return txn;
}

/* Throws the transaction away.
 *
 * Nothing has to be undone. Every block this transaction wrote is one the live
 * superblock still calls free, and the live superblock is untouched -- so
 * abandoning is simply forgetting, which is the property copy-on-write is for.
 * The blocks that were written are reused by the next transaction. */
void reconfs_txn_abort(struct reconfs_txn *txn)
{
	unsigned i;

	if (!txn)
		return;

	for (i = 0; i < txn->leaves; i++)
		kfree(txn->leaf[i].data);
	kfree(txn);
}

u64 reconfs_txn_new_dossier(struct reconfs_txn *txn)
{
	return txn->next_dossier++;
}

void reconfs_txn_set_root(struct reconfs_txn *txn, u64 inode)
{
	txn->new_root_inode = inode;
}

bool reconfs_txn_failed(const struct reconfs_txn *txn)
{
	return txn->failed;
}

/* --- Planning the table ---------------------------------------------------
 *
 * Every dirty leaf needs a new home, and taking that home dirties a leaf -- so
 * this loops until no dirty leaf is left without one. It terminates because
 * each pass allocates at most TXN_LEAVES blocks and a leaf, once planned, is
 * never planned again however often its contents change afterwards.
 */
static enum reconfs_status plan_leaves(struct reconfs_txn *txn, bool *did_any)
{
	bool again = true;

	while (again) {
		unsigned i;

		again = false;

		for (i = 0; i < txn->leaves; i++) {
			struct txn_leaf *l = &txn->leaf[i];

			if (!l->dirty || l->new_block)
				continue;

			l->new_block = reconfs_txn_alloc(txn, RECONFS_OWNER_ARCHIVE);
			if (!l->new_block || txn->failed)
				return RECONFS_ERR_NOSPACE;

			if (did_any)
				*did_any = true;

			/* The old copy becomes free in the *new* table, which
			 * is what makes the space reusable next time and not
			 * this time -- a block freed in this transaction must
			 * not be handed out by it, or a crash could leave one
			 * block claimed by two things. */
			reconfs_txn_free(txn, l->old_block);

			again = true;	/* those two calls may have dirtied
					 * another leaf */
		}
	}

	return RECONFS_OK;
}

static u64 mapped(struct reconfs_txn *txn, u64 old)
{
	unsigned i;

	for (i = 0; i < txn->indexes; i++)
		if (txn->index[i].old_block == old)
			return txn->index[i].new_block;
	return 0;
}

static u64 planned_leaf(struct reconfs_txn *txn, u64 old)
{
	unsigned i;

	for (i = 0; i < txn->leaves; i++)
		if (txn->leaf[i].old_block == old && txn->leaf[i].new_block)
			return txn->leaf[i].new_block;
	return 0;
}

/* Walks down to every dirty leaf, allocating a new block for each index node on
 * the way. Returns the new block for `node`, or 0 if nothing below it changed.
 *
 * Allocation only. Nothing is written here, because writing an index block
 * before the blocks it names would be the one thing this design forbids. */
static u64 plan_index(struct reconfs_txn *txn, u64 node, u32 depth,
		      u64 span, u64 base, enum reconfs_status *st)
{
	u64 *slots;
	u64 changed = 0;
	unsigned i;

	if (depth == 0)
		return planned_leaf(txn, node);

	slots = kzalloc(txn->fs->block_size);
	if (!slots) {
		*st = RECONFS_ERR_NOMEM;
		return 0;
	}

	*st = reconfs_read_block(txn->fs, node, slots, RECONFS_NO_CSUM);
	if (*st != RECONFS_OK) {
		kfree(slots);
		return 0;
	}

	span /= txn->fs->per_index;

	for (i = 0; i < txn->fs->per_index; i++) {
		if (!slots[i])
			continue;
		if (plan_index(txn, slots[i], depth - 1, span,
			       base + (u64)i * span, st))
			changed = 1;
		if (*st != RECONFS_OK) {
			kfree(slots);
			return 0;
		}
	}

	kfree(slots);

	if (!changed)
		return 0;

	if (txn->indexes == TXN_INDEX) {
		*st = RECONFS_ERR_NOSPACE;
		return 0;
	}

	txn->index[txn->indexes].old_block = node;
	txn->index[txn->indexes].new_block =
		reconfs_txn_alloc(txn, RECONFS_OWNER_ARCHIVE);

	if (!txn->index[txn->indexes].new_block || txn->failed) {
		*st = RECONFS_ERR_NOSPACE;
		return 0;
	}

	reconfs_txn_free(txn, node);
	txn->indexes++;

	return txn->index[txn->indexes - 1].new_block;
}

/* Writes the index blocks, deepest first, each naming blocks already written. */
static enum reconfs_status emit_index(struct reconfs_txn *txn, u64 node,
				      u32 depth, u64 span)
{
	u64 *slots;
	u64 dest;
	enum reconfs_status st;
	unsigned i;

	if (depth == 0)
		return RECONFS_OK;

	dest = mapped(txn, node);
	if (!dest)
		return RECONFS_OK;		/* nothing below changed */

	slots = kzalloc(txn->fs->block_size);
	if (!slots)
		return RECONFS_ERR_NOMEM;

	st = reconfs_read_block(txn->fs, node, slots, RECONFS_NO_CSUM);
	if (st != RECONFS_OK) {
		kfree(slots);
		return st;
	}

	span /= txn->fs->per_index;

	for (i = 0; i < txn->fs->per_index; i++) {
		u64 child = slots[i];
		u64 moved;

		if (!child)
			continue;

		st = emit_index(txn, child, depth - 1, span);
		if (st != RECONFS_OK) {
			kfree(slots);
			return st;
		}

		moved = (depth - 1 == 0) ? planned_leaf(txn, child)
					 : mapped(txn, child);
		if (moved)
			slots[i] = moved;
	}

	st = reconfs_write_block(txn->fs, dest, slots, RECONFS_NO_CSUM);
	kfree(slots);
	return st;
}

/* --- The commit ----------------------------------------------------------- */

/* Tells the drive about the blocks this transaction released.
 *
 * --- Why this runs after the commit and not before ---
 *
 * A block freed by this transaction is still reachable from the *live*
 * superblock until the new one is on the medium. Discarding it earlier and then
 * crashing would leave the old superblock -- which is still the truth at that
 * moment -- pointing at data the drive has been told to forget. On a drive that
 * returns zeroes for discarded blocks, that is silent data loss on a filesystem
 * whose whole design exists to prevent exactly that.
 *
 * So the order is: write, flush, superblock, flush, *then* discard. The blocks
 * are unreachable only once the new superblock is durable, and that is the
 * first moment this is safe.
 *
 * --- Why nothing here can fail the commit ---
 *
 * The commit has already happened. Discard is advisory in both directions: the
 * drive may ignore it, and a filesystem that reported failure because the drive
 * would not listen has misunderstood which of them is in charge. Errors are
 * dropped on purpose.
 */
static void discard_freed(struct reconfs_txn *txn)
{
	struct reconfs *fs = txn->fs;
	u32 spb;
	unsigned i;

	if (!fs->dev->discard_supported)
		return;

	spb = fs->block_size / fs->dev->block_size;

	for (i = 0; i < txn->freed_count; i++)
		(void)block_discard(fs->dev, txn->freed[i] * spb, spb);
}

enum reconfs_status reconfs_txn_commit(struct reconfs_txn *txn)
{
	struct reconfs *fs = txn->fs;
	struct reconfs_super *sb = NULL;
	enum reconfs_status st;
	u64 new_root;
	u64 span = 1;
	u64 target;
	unsigned i;

	if (txn->failed) {
		reconfs_txn_abort(txn);
		return RECONFS_ERR_NOSPACE;
	}

	/* --- plan ------------------------------------------------------- */
	st = plan_leaves(txn, NULL);
	if (st != RECONFS_OK)
		goto fail;

	for (i = 0; i < fs->table_depth; i++)
		span *= fs->per_index;

	new_root = plan_index(txn, fs->table_root, fs->table_depth, span, 0, &st);
	if (st != RECONFS_OK)
		goto fail;

	/* A transaction that changed no allocation at all still commits: the
	 * generation moves, which is how a no-op commit is distinguishable from
	 * a crash that lost one. */
	if (!new_root)
		new_root = fs->table_root;

	/* Planning the index allocated blocks, which dirtied leaves, which may
	 * have dirtied a leaf that had no new home yet.
	 *
	 * If that happened, this transaction cannot be committed. The index was
	 * already planned around where the leaves were *before* this pass, so a
	 * leaf that moves now is one the rewritten index does not point at: the
	 * new copy would be unreachable and the old copy, already released,
	 * would still be named. Silent corruption, and it would look like a
	 * successful commit.
	 *
	 * So the transaction is refused instead. It is rare -- it needs the
	 * index's own allocation to cross into a leaf nothing had touched -- and
	 * a refused transaction is retried harmlessly, because nothing it wrote
	 * was ever reachable. Refusing rather than corrupting is this project's
	 * standing rule, and this is the case it was written for. */
	{
		bool moved = false;

		st = plan_leaves(txn, &moved);
		if (st != RECONFS_OK)
			goto fail;

		if (moved) {
			st = RECONFS_ERR_RETRY;
			goto fail;
		}
	}

	for (i = 0; i < txn->leaves; i++) {
		if (txn->leaf[i].dirty && !txn->leaf[i].new_block) {
			st = RECONFS_ERR_NOSPACE;
			goto fail;
		}
	}

	/* --- write the leaves ------------------------------------------- */
	for (i = 0; i < txn->leaves; i++) {
		struct txn_leaf *l = &txn->leaf[i];

		if (!l->dirty)
			continue;

		st = reconfs_write_block(fs, l->new_block, l->data,
					 RECONFS_NO_CSUM);
		if (st != RECONFS_OK)
			goto fail;
	}

	/* --- then the index, which names them --------------------------- */
	st = emit_index(txn, fs->table_root, fs->table_depth, span);
	if (st != RECONFS_OK)
		goto fail;

	/* --- the ordering point ------------------------------------------
	 *
	 * Everything the next superblock will name is on the medium before the
	 * superblock naming it is written. This flush is the whole guarantee;
	 * without it the two writes are unordered and a crash between them can
	 * leave a superblock pointing at blocks that were never written. */
	if (block_flush(fs->dev) != BLOCK_OK) {
		st = RECONFS_ERR_IO;
		goto fail;
	}

	/* --- the superblock ----------------------------------------------
	 *
	 * Written to whichever of the two is *not* live, so a crash during this
	 * write leaves the other one intact and the volume never has a moment
	 * with no valid superblock on it. */
	sb = kzalloc(fs->block_size);
	if (!sb) {
		st = RECONFS_ERR_NOMEM;
		goto fail;
	}

	kmemcpy(sb, &fs->super, sizeof(fs->super));
	sb->epoch++;
	sb->table_root     = new_root;
	sb->root_inode     = txn->new_root_inode;
	sb->next_dossier = txn->next_dossier;
	sb->blocks_used    = txn->blocks_used;

	target = (fs->live_super == RECONFS_SUPER_A) ? RECONFS_SUPER_B
						    : RECONFS_SUPER_A;

	st = reconfs_write_block(fs, target, sb, SUPER_CSUM);
	if (st != RECONFS_OK)
		goto fail;

	/* And again, so that "committed" means the superblock is on the medium
	 * rather than in a cache -- a caller told a change is durable must not
	 * have been told that early. */
	if (block_flush(fs->dev) != BLOCK_OK) {
		st = RECONFS_ERR_IO;
		goto fail;
	}

	/* The change is real. Now, and not before. */
	kmemcpy(&fs->super, sb, sizeof(fs->super));
	fs->live_super  = target;
	fs->table_root  = new_root;
	fs->root_inode  = txn->new_root_inode;

	/* And only now are the released blocks genuinely unreachable. */
	discard_freed(txn);

	kfree(sb);
	reconfs_txn_abort(txn);
	return RECONFS_OK;

fail:
	kfree(sb);
	reconfs_txn_abort(txn);
	return st;
}
