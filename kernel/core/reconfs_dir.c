/* ReconFS -- names.
 *
 * A directory is a byte stream of entries, held inline in its inode while it
 * fits and in the inode's direct blocks once it does not. Every operation that
 * changes one builds the whole new stream, allocates fresh blocks for it, and
 * hands the caller a new directory inode -- which is what copy-on-write means
 * and is why there is no "insert an entry in place" anywhere in this file.
 *
 * --- Entries never straddle a block ---
 *
 * A block with no room for the next entry is padded to its end. That wastes a
 * little, and it buys the property that any single directory block can be read
 * and understood on its own: the checker's reverse sweep needs that, and so
 * will any future repair tool that finds a directory block whose siblings are
 * gone.
 *
 * A padded tail is written as a zero `rec_len`, which is why zero is not a
 * legal entry length and why every reader stops at one.
 *
 * --- Case ---
 *
 * Names are stored exactly as they were given and compared without case, which
 * is what a person means by a name. The fold is ASCII only: a byte above 127 is
 * compared exactly, so two names that differ only in the case of a non-ASCII
 * letter are two different names. That is a limitation which shows up as "two
 * files that look similar both exist", never as data loss, and it is recorded
 * in the superblock as `fold` so a future table cannot be applied silently to
 * volumes written without one.
 */
#include <recon/kernel/reconfs.h>

#include <recon/kernel/console.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/time.h>

#define INODE_CSUM	RK_OFFSETOF(struct reconfs_inode, checksum)

static u8 fold(u8 c)
{
	return (c >= 'A' && c <= 'Z') ? (u8)(c - 'A' + 'a') : c;
}

/* Compares a stored name, which is not terminated, against a C string. */
static bool name_eq(const char *stored, u8 stored_len, const char *want)
{
	size_t i;

	for (i = 0; i < stored_len; i++) {
		if (!want[i])
			return false;
		if (fold((u8)stored[i]) != fold((u8)want[i]))
			return false;
	}
	return want[stored_len] == '\0';
}

static u16 entry_size(u8 name_len)
{
	u32 n = (u32)RECONFS_DIRENT_MIN + name_len;

	return (u16)((n + 7u) & ~7u);		/* a multiple of eight */
}

/* --- Reading a directory's entries into one contiguous buffer -------------
 *
 * Blocks are joined end to end with their padding removed, so everything above
 * this works on one stream and only this file knows the entries were ever split.
 */
struct entries {
	u8 *buf;
	u32 len;
	u32 cap;
};

static void entries_free(struct entries *e)
{
	kfree(e->buf);
	e->buf = NULL;
}

static bool entries_append(struct entries *e, const void *src, u32 n)
{
	if (e->len + n > e->cap)
		return false;
	kmemcpy(e->buf + e->len, src, n);
	e->len += n;
	return true;
}

/* Copies the entries out of one block, stopping at the padding. */
static bool take_block(struct entries *e, const u8 *blk, u32 len)
{
	u32 off = 0;

	while (off + RECONFS_DIRENT_MIN <= len) {
		const struct reconfs_dirent *de =
			(const struct reconfs_dirent *)(blk + off);

		if (!de->rec_len)
			break;			/* padding to the block's end */

		if (de->rec_len < RECONFS_DIRENT_MIN ||
		    off + de->rec_len > len ||
		    (u32)de->name_len + RECONFS_DIRENT_MIN > de->rec_len)
			return false;

		if (!entries_append(e, de, de->rec_len))
			return false;

		off += de->rec_len;
	}

	return true;
}

/* How much a directory's entries could possibly come to: every direct block
 * full, or the inline area. Allocated once rather than grown, because growing
 * inside a transaction is a second way for a commit to fail. */
static u32 entries_capacity(const struct reconfs *fs)
{
	return (u32)(RECONFS_DIRECT * fs->block_size);
}

static enum reconfs_status read_entries(struct reconfs *fs,
					const struct reconfs_inode *dir,
					struct entries *e)
{
	enum reconfs_status st = RECONFS_OK;
	u8 *blk;
	unsigned i;

	kmemset(e, 0, sizeof(*e));
	e->cap = entries_capacity(fs);
	e->buf = kzalloc(e->cap);
	if (!e->buf)
		return RECONFS_ERR_NOMEM;

	if (dir->inline_len) {
		if (!take_block(e, dir->data, dir->inline_len))
			st = RECONFS_ERR_CORRUPT;
		return st;
	}

	blk = kzalloc(fs->block_size);
	if (!blk) {
		entries_free(e);
		return RECONFS_ERR_NOMEM;
	}

	for (i = 0; i < RECONFS_DIRECT; i++) {
		if (!dir->direct[i])
			continue;

		st = reconfs_read_block(fs, dir->direct[i], blk,
					RECONFS_NO_CSUM);
		if (st != RECONFS_OK)
			break;

		if (!take_block(e, blk, fs->block_size)) {
			st = RECONFS_ERR_CORRUPT;
			break;
		}
	}

	kfree(blk);
	if (st != RECONFS_OK)
		entries_free(e);
	return st;
}

/* --- Looking a name up ---------------------------------------------------- */

enum reconfs_status reconfs_lookup(struct reconfs *fs, u64 dir_block,
				   const char *name, u64 *out_block)
{
	struct reconfs_inode *dir;
	struct entries e;
	enum reconfs_status st;
	u32 off = 0;

	if (!name || !*name)
		return RECONFS_ERR_NAME;

	dir = kzalloc(fs->block_size);
	if (!dir)
		return RECONFS_ERR_NOMEM;

	st = reconfs_read_block(fs, dir_block, dir, INODE_CSUM);
	if (st != RECONFS_OK) {
		kfree(dir);
		return st;
	}

	if (dir->magic != RECONFS_INODE_MAGIC ||
	    dir->type != RECONFS_TYPE_DIR) {
		kfree(dir);
		return RECONFS_ERR_NOT_DIR;
	}

	st = read_entries(fs, dir, &e);
	kfree(dir);
	if (st != RECONFS_OK)
		return st;

	while (off + RECONFS_DIRENT_MIN <= e.len) {
		const struct reconfs_dirent *de =
			(const struct reconfs_dirent *)(e.buf + off);

		if (!de->rec_len)
			break;

		if (de->inode && name_eq(de->name, de->name_len, name)) {
			*out_block = de->inode;
			entries_free(&e);
			return RECONFS_OK;
		}

		off += de->rec_len;
	}

	entries_free(&e);
	return RECONFS_ERR_NOT_FOUND;
}

/* --- Writing a directory back out ----------------------------------------
 *
 * Builds a fresh inode for the directory, with the entry stream either inline
 * or spread over newly allocated direct blocks. Every block it touches comes
 * from the transaction, so none of them is reachable from the live superblock.
 */
static enum reconfs_status write_dir(struct reconfs_txn *txn,
				     struct reconfs *fs,
				     const struct reconfs_inode *old,
				     const struct entries *e,
				     u64 *out_block)
{
	struct reconfs_inode *ino;
	enum reconfs_status st = RECONFS_OK;
	u64 blk;
	u32 off = 0;
	unsigned used = 0;
	u8 *page = NULL;

	blk = reconfs_txn_alloc(txn, old->parent ? old->parent
						 : RECONFS_OWNER_ARCHIVE);
	if (!blk)
		return RECONFS_ERR_NOSPACE;

	ino = kzalloc(fs->block_size);
	if (!ino)
		return RECONFS_ERR_NOMEM;

	kmemcpy(ino, old, sizeof(*ino));
	kmemset(ino->direct, 0, sizeof(ino->direct));
	ino->indirect = 0;
	ino->double_indirect = 0;
	ino->triple_indirect = 0;
	ino->inline_len = 0;
	ino->size = e->len;
	ino->mtime = time_monotonic_ns();
	ino->ctime = ino->mtime;

	if (e->len <= reconfs_inline_max(fs->block_size)) {
		kmemcpy(ino->data, e->buf, e->len);
		ino->inline_len = e->len;
		goto write;
	}

	/* Out of line. Entries are packed into blocks without straddling one,
	 * so a block that cannot fit the next entry is left padded. */
	page = kzalloc(fs->block_size);
	if (!page) {
		st = RECONFS_ERR_NOMEM;
		goto out;
	}

	while (off < e->len) {
		u32 fill = 0;

		if (used == RECONFS_DIRECT) {
			/* Refused, not truncated. A directory that silently
			 * dropped its last entries would lose files, which is
			 * the one outcome this project's rules forbid outright.
			 * The indirect block that would raise this limit is in
			 * the format already and is not written yet. */
			st = RECONFS_ERR_DIR_FULL;
			goto out;
		}

		kmemset(page, 0, fs->block_size);

		while (off < e->len) {
			const struct reconfs_dirent *de =
				(const struct reconfs_dirent *)(e->buf + off);

			if (fill + de->rec_len > fs->block_size)
				break;

			kmemcpy(page + fill, de, de->rec_len);
			fill += de->rec_len;
			off  += de->rec_len;
		}

		if (!fill) {
			st = RECONFS_ERR_CORRUPT;
			goto out;
		}

		ino->direct[used] = reconfs_txn_alloc(txn, ino->dossier);
		if (!ino->direct[used]) {
			st = RECONFS_ERR_NOSPACE;
			goto out;
		}

		st = reconfs_write_block(fs, ino->direct[used], page,
					 RECONFS_NO_CSUM);
		if (st != RECONFS_OK)
			goto out;

		used++;
	}

write:
	st = reconfs_write_block(fs, blk, ino, INODE_CSUM);
	if (st == RECONFS_OK)
		*out_block = blk;

out:
	kfree(page);
	kfree(ino);
	return st;
}

/* --- Creating ------------------------------------------------------------- */

enum reconfs_status reconfs_create(struct reconfs_txn *txn, struct reconfs *fs,
				   u64 dir_block, const char *name, u32 type,
				   u32 mode, u64 *out_inode, u64 *out_dir)
{
	struct reconfs_inode *dir = NULL, *ino = NULL;
	struct reconfs_dirent *de;
	struct entries e;
	enum reconfs_status st;
	size_t name_len;
	u16 need;
	u64 child = 0;

	if (!name || !*name)
		return RECONFS_ERR_NAME;

	name_len = kstrlen(name);
	if (name_len > RECONFS_NAME_MAX)
		return RECONFS_ERR_NAME;

	/* A name may not contain the separator, and "." and ".." are the
	 * directory's own business rather than entries anyone may create. */
	{
		size_t i;

		for (i = 0; i < name_len; i++)
			if (name[i] == '/')
				return RECONFS_ERR_NAME;

		if (name_eq(".", 1, name) || name_eq("..", 2, name))
			return RECONFS_ERR_NAME;
	}

	if (reconfs_lookup(fs, dir_block, name, &child) == RECONFS_OK)
		return RECONFS_ERR_EXISTS;

	dir = kzalloc(fs->block_size);
	ino = kzalloc(fs->block_size);
	if (!dir || !ino) {
		st = RECONFS_ERR_NOMEM;
		goto out;
	}

	st = reconfs_read_block(fs, dir_block, dir, INODE_CSUM);
	if (st != RECONFS_OK)
		goto out;

	if (dir->type != RECONFS_TYPE_DIR) {
		st = RECONFS_ERR_NOT_DIR;
		goto out;
	}

	st = read_entries(fs, dir, &e);
	if (st != RECONFS_OK)
		goto out;

	need = entry_size((u8)name_len);
	if (e.len + need > e.cap) {
		entries_free(&e);
		st = RECONFS_ERR_DIR_FULL;
		goto out;
	}

	/* --- The new inode ---------------------------------------------------
	 *
	 * Its mode is written here, in the same block as everything else about
	 * it, and that block becomes reachable in the single superblock write
	 * that commits the whole change. There is no image, crashed or
	 * otherwise, in which this file is reachable with a mode that was
	 * applied afterwards -- which is the constraint this design was asked
	 * for, and it falls out rather than being arranged. */
	ino->magic      = RECONFS_INODE_MAGIC;
	ino->dossier    = reconfs_txn_new_dossier(txn);
	ino->parent     = dir->dossier;
	ino->type       = type;
	ino->mode       = mode;
	ino->size       = 0;
	ino->links      = 1;
	ino->btime      = time_monotonic_ns();
	ino->mtime      = ino->btime;
	ino->ctime      = ino->btime;
	ino->inline_len = 0;

	child = reconfs_txn_alloc(txn, dir->dossier);
	if (!child) {
		entries_free(&e);
		st = RECONFS_ERR_NOSPACE;
		goto out;
	}

	st = reconfs_write_block(fs, child, ino, INODE_CSUM);
	if (st != RECONFS_OK) {
		entries_free(&e);
		goto out;
	}

	/* --- The entry, appended to the directory's stream ------------------ */
	de = (struct reconfs_dirent *)(e.buf + e.len);
	kmemset(de, 0, need);
	de->inode    = child;
	de->rec_len  = need;
	de->name_len = (u8)name_len;
	de->type     = (u8)type;
	kmemcpy(de->name, name, name_len);
	e.len += need;

	st = write_dir(txn, fs, dir, &e, out_dir);
	entries_free(&e);

	if (st == RECONFS_OK)
		*out_inode = child;

out:
	kfree(dir);
	kfree(ino);
	return st;
}

/* --- Renaming -------------------------------------------------------------
 *
 * The operation this whole design was asked for, and the reason it is
 * copy-on-write rather than journalled.
 *
 * A rename is *one commit*. The new entry stream -- with the old name gone, any
 * existing target gone, and the new name pointing at the same object -- is built
 * in blocks nothing reachable can see, and becomes the truth in the single
 * superblock write at the end of the transaction.
 *
 * So the set of post-crash outcomes has exactly two members: the rename
 * happened, or it did not. There is no image in which both names exist, neither
 * exists, or the new name points at a half-written file. That is what lets the
 * caller above -- the registry, today -- write a temporary file and rename over
 * the real one, and know that a machine losing power mid-save leaves the old
 * settings rather than an empty file.
 *
 * --- Renaming over something ---
 *
 * Permitted, and the replaced object's inode is released in the same
 * transaction. It is released, not overwritten: the block goes back to the
 * unclaimed state in the *new* owner table, so the old superblock still reaches
 * it until the commit lands, and a crash before that leaves the old contents
 * entirely intact.
 *
 * --- What is refused ---
 *
 * Renaming between two different directories. It needs the moved object's
 * parent rewritten and both directories rewritten and the path from each to the
 * root copied, and the path-copy helper that would do that does not exist yet.
 * Refused with a status of its own rather than half-performed.
 */
enum reconfs_status reconfs_rename(struct reconfs_txn *txn, struct reconfs *fs,
				   u64 dir_block, const char *from,
				   const char *to, u64 *out_dir)
{
	struct reconfs_inode *dir = NULL;
	struct entries old_e, new_e;
	enum reconfs_status st;
	size_t to_len;
	u16 need;
	u64 moving = 0, replaced = 0;
	u32 off;
	bool have_old = false;

	kmemset(&old_e, 0, sizeof(old_e));
	kmemset(&new_e, 0, sizeof(new_e));

	if (!from || !*from || !to || !*to)
		return RECONFS_ERR_NAME;

	to_len = kstrlen(to);
	if (to_len > RECONFS_NAME_MAX)
		return RECONFS_ERR_NAME;

	{
		size_t i;

		for (i = 0; i < to_len; i++)
			if (to[i] == '/')
				return RECONFS_ERR_NAME;
	}

	dir = kzalloc(fs->block_size);
	if (!dir)
		return RECONFS_ERR_NOMEM;

	st = reconfs_read_block(fs, dir_block, dir, INODE_CSUM);
	if (st != RECONFS_OK)
		goto out;

	if (dir->type != RECONFS_TYPE_DIR) {
		st = RECONFS_ERR_NOT_DIR;
		goto out;
	}

	st = read_entries(fs, dir, &old_e);
	if (st != RECONFS_OK)
		goto out;

	/* Find what is moving, and what it will land on. Both in one pass over
	 * the stream, because a second pass could see a different stream if
	 * anything above ever makes these concurrent. */
	off = 0;
	while (off + RECONFS_DIRENT_MIN <= old_e.len) {
		const struct reconfs_dirent *de =
			(const struct reconfs_dirent *)(old_e.buf + off);

		if (!de->rec_len)
			break;

		if (de->inode) {
			if (name_eq(de->name, de->name_len, from)) {
				moving = de->inode;
				have_old = true;
			} else if (name_eq(de->name, de->name_len, to)) {
				replaced = de->inode;
			}
		}

		off += de->rec_len;
	}

	if (!have_old) {
		st = RECONFS_ERR_NOT_FOUND;
		goto out;
	}

	/* Renaming a name onto itself, in any case spelling, is a request to
	 * change how the name is *spelled* -- which is a real thing to want on a
	 * case-preserving filesystem, and is handled by the rebuild below rather
	 * than short-circuited. */
	if (replaced == moving)
		replaced = 0;

	new_e.cap = entries_capacity(fs);
	new_e.buf = kzalloc(new_e.cap);
	if (!new_e.buf) {
		st = RECONFS_ERR_NOMEM;
		goto out;
	}

	/* Everything except the two names being disturbed. */
	off = 0;
	while (off + RECONFS_DIRENT_MIN <= old_e.len) {
		const struct reconfs_dirent *de =
			(const struct reconfs_dirent *)(old_e.buf + off);

		if (!de->rec_len)
			break;

		if (de->inode &&
		    !name_eq(de->name, de->name_len, from) &&
		    !name_eq(de->name, de->name_len, to)) {
			if (!entries_append(&new_e, de, de->rec_len)) {
				st = RECONFS_ERR_DIR_FULL;
				goto out;
			}
		}

		off += de->rec_len;
	}

	need = entry_size((u8)to_len);
	if (new_e.len + need > new_e.cap) {
		st = RECONFS_ERR_DIR_FULL;
		goto out;
	}

	{
		struct reconfs_dirent *de =
			(struct reconfs_dirent *)(new_e.buf + new_e.len);

		kmemset(de, 0, need);
		de->inode    = moving;
		de->rec_len  = need;
		de->name_len = (u8)to_len;
		/* The type is carried over from the entry that is moving, not
		 * re-derived: the inode is not being rewritten, so anything
		 * this recomputed could only disagree with it. */
		de->type     = RECONFS_TYPE_FILE;
		kmemcpy(de->name, to, to_len);
		new_e.len += need;
	}

	/* Carry the moving entry's real type across. Read from the old stream
	 * rather than assumed, because a directory renamed as a file would be a
	 * listing that lies about what it is showing. */
	off = 0;
	while (off + RECONFS_DIRENT_MIN <= old_e.len) {
		const struct reconfs_dirent *de =
			(const struct reconfs_dirent *)(old_e.buf + off);

		if (!de->rec_len)
			break;
		if (de->inode == moving) {
			struct reconfs_dirent *added =
				(struct reconfs_dirent *)(new_e.buf +
							  new_e.len - need);

			added->type = de->type;
			break;
		}
		off += de->rec_len;
	}

	st = write_dir(txn, fs, dir, &new_e, out_dir);
	if (st != RECONFS_OK)
		goto out;

	/* The object the new name used to refer to. Released only now, after
	 * the new directory is built -- and released rather than written over,
	 * so the live superblock still reaches it until the commit. */
	if (replaced)
		reconfs_txn_free(txn, replaced);

out:
	entries_free(&old_e);
	entries_free(&new_e);
	kfree(dir);
	return st;
}

/* --- Listing --------------------------------------------------------------
 *
 * Index-based, and the guarantee is stated rather than left to be discovered:
 * **a listing is a snapshot of one epoch.** Every entry returned for a given
 * index comes from the directory as it was when the listing started, because
 * copy-on-write means the old directory blocks stay intact and reachable until
 * the transaction that replaced them commits.
 *
 * What a caller does *not* get is a listing that reflects changes made while it
 * is iterating. That is the honest guarantee, and stating it is the whole point:
 * the design records that an unstated guarantee is the worst of the three
 * options.
 */
enum reconfs_status reconfs_readdir(struct reconfs *fs, u64 dir_block,
				    unsigned index, char *name, size_t len,
				    u64 *child, u32 *type)
{
	struct reconfs_inode *dir;
	struct entries e;
	enum reconfs_status st;
	u32 off = 0;
	unsigned n = 0;

	dir = kzalloc(fs->block_size);
	if (!dir)
		return RECONFS_ERR_NOMEM;

	st = reconfs_read_block(fs, dir_block, dir, INODE_CSUM);
	if (st != RECONFS_OK) {
		kfree(dir);
		return st;
	}

	if (dir->type != RECONFS_TYPE_DIR) {
		kfree(dir);
		return RECONFS_ERR_NOT_DIR;
	}

	st = read_entries(fs, dir, &e);
	kfree(dir);
	if (st != RECONFS_OK)
		return st;

	while (off + RECONFS_DIRENT_MIN <= e.len) {
		const struct reconfs_dirent *de =
			(const struct reconfs_dirent *)(e.buf + off);

		if (!de->rec_len)
			break;

		if (de->inode) {
			if (n == index) {
				size_t copy = de->name_len;

				if (copy >= len)
					copy = len ? len - 1 : 0;
				kmemcpy(name, de->name, copy);
				name[copy] = '\0';
				*child = de->inode;
				*type  = de->type;
				entries_free(&e);
				return RECONFS_OK;
			}
			n++;
		}

		off += de->rec_len;
	}

	entries_free(&e);
	return RECONFS_ERR_NOT_FOUND;
}
