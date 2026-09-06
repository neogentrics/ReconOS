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

/* Defined with the rest of the contents handling, further down. Declared here
 * because the operations above it -- add, remove, rename -- all release what
 * they displace, and every one of them needs it. */
static void release_contents(struct reconfs_txn *txn, struct reconfs *fs,
			     const struct reconfs_inode *ino);

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
				     struct reconfs *fs, u64 old_block,
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
	if (st != RECONFS_OK)
		goto out;

	*out_block = blk;

	/* The copy this one replaces, and the blocks its entries were in.
	 *
	 * Every operation that changes a directory writes a new inode for it,
	 * because that is what copy-on-write means. Nothing released the old
	 * one, so *every* create, rename, write and remove leaked a block --
	 * one per directory rewrite, forever, on a filesystem whose whole
	 * pattern of use is rewriting directories.
	 *
	 * It was invisible to the checker, which asks whether the owner table
	 * and the tree agree about what is allocated: a leaked block is
	 * allocated and unreachable, which is exactly what it reports. What it
	 * took was a test that asked a different question -- write a file,
	 * remove it, and require the free-block count to return to where it
	 * started.
	 *
	 * Released after the new copy is written, so the live superblock still
	 * reaches the old one until the commit lands. */
	if (old_block) {
		unsigned k;

		if (!old->inline_len)
			for (k = 0; k < RECONFS_DIRECT; k++)
				if (old->direct[k])
					reconfs_txn_free(txn, old->direct[k]);

		reconfs_txn_free(txn, old_block);
	}

out:
	kfree(page);
	kfree(ino);
	return st;
}

/* Puts a name into a directory, pointing at an inode that already exists.
 *
 * If the name is taken, what it named is released -- which is what makes both
 * "rename over" and "move over" one operation rather than a delete followed by
 * a create, with an instant in between where the name refers to nothing.
 *
 * Shared by create and by move so the two cannot drift apart about what an
 * entry looks like.
 */
static enum reconfs_status add_entry(struct reconfs_txn *txn, struct reconfs *fs,
				     u64 dir_block, const char *name,
				     u64 inode, u8 type, u64 *out_dir)
{
	struct reconfs_inode *dir = NULL, *gone = NULL;
	struct entries old_e, new_e;
	struct reconfs_dirent *de;
	enum reconfs_status st;
	size_t name_len = kstrlen(name);
	u64 replaced = 0;
	u16 need;
	u32 off;

	kmemset(&old_e, 0, sizeof(old_e));
	kmemset(&new_e, 0, sizeof(new_e));

	if (!name_len || name_len > RECONFS_NAME_MAX)
		return RECONFS_ERR_NAME;

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

	new_e.cap = entries_capacity(fs);
	new_e.buf = kzalloc(new_e.cap);
	if (!new_e.buf) {
		st = RECONFS_ERR_NOMEM;
		goto out;
	}

	off = 0;
	while (off + RECONFS_DIRENT_MIN <= old_e.len) {
		const struct reconfs_dirent *e =
			(const struct reconfs_dirent *)(old_e.buf + off);

		if (!e->rec_len)
			break;

		if (e->inode && name_eq(e->name, e->name_len, name)) {
			replaced = e->inode;
		} else if (e->inode) {
			if (!entries_append(&new_e, e, e->rec_len)) {
				st = RECONFS_ERR_DIR_FULL;
				goto out;
			}
		}

		off += e->rec_len;
	}

	need = entry_size((u8)name_len);
	if (new_e.len + need > new_e.cap) {
		st = RECONFS_ERR_DIR_FULL;
		goto out;
	}

	de = (struct reconfs_dirent *)(new_e.buf + new_e.len);
	kmemset(de, 0, need);
	de->inode    = inode;
	de->rec_len  = need;
	de->name_len = (u8)name_len;
	de->type     = type;
	kmemcpy(de->name, name, name_len);
	new_e.len += need;

	st = write_dir(txn, fs, dir_block, dir, &new_e, out_dir);
	if (st != RECONFS_OK)
		goto out;

	if (replaced && replaced != inode) {
		gone = kzalloc(fs->block_size);
		if (!gone) {
			st = RECONFS_ERR_NOMEM;
			goto out;
		}

		if (reconfs_read_block(fs, replaced, gone, INODE_CSUM)
		    == RECONFS_OK)
			release_contents(txn, fs, gone);

		reconfs_txn_free(txn, replaced);
	}

out:
	entries_free(&old_e);
	entries_free(&new_e);
	kfree(dir);
	kfree(gone);
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

	st = write_dir(txn, fs, dir_block, dir, &e, out_dir);
	entries_free(&e);

	if (st == RECONFS_OK)
		*out_inode = child;

out:
	kfree(dir);
	kfree(ino);
	return st;
}

/* --- A file's contents ----------------------------------------------------
 *
 * Whole-file writes only. `reconfs_write_named` replaces everything a file
 * holds; there is no way to change part of one.
 *
 * That is not a simplification to be tidied up later -- it is what the caller
 * above actually does. A registry, a theme file, a package receipt: each is
 * built in memory and written out entire. An offset-based write would be an
 * interface invented before its first caller, which this project has a rule
 * about, and under copy-on-write it is barely cheaper anyway: changing one byte
 * still rewrites the block holding it and the path from it to the root.
 *
 * --- Where the bytes live ---
 *
 *   up to RECONFS_INLINE_MAX   inside the inode, costing no extra block
 *   up to 12 blocks            the direct pointers
 *   up to per_index more       one indirect block
 *
 * At a 4KiB block that is about two megabytes; at 64KiB, about half a gigabyte.
 * Beyond it the file is *refused*, not truncated -- the deeper indirection is in
 * the format and is not written, and a write that silently kept part of what it
 * was given would be the one outcome this project's rules forbid outright.
 */
static enum reconfs_status write_contents(struct reconfs_txn *txn,
					  struct reconfs *fs,
					  const struct reconfs_inode *old,
					  const u8 *data, u32 len,
					  u64 *out_block)
{
	struct reconfs_inode *ino;
	enum reconfs_status st = RECONFS_OK;
	u64 blk;
	u8 *page = NULL;
	u64 *index = NULL;
	u32 off = 0;
	unsigned used = 0, in_index = 0;

	blk = reconfs_txn_alloc(txn, old->parent);
	if (!blk)
		return RECONFS_ERR_NOSPACE;

	ino = kzalloc(fs->block_size);
	if (!ino)
		return RECONFS_ERR_NOMEM;

	kmemcpy(ino, old, sizeof(*ino));
	kmemset(ino->direct, 0, sizeof(ino->direct));
	ino->indirect = 0;
	ino->inline_len = 0;
	ino->size = len;
	ino->mtime = time_monotonic_ns();
	ino->ctime = ino->mtime;

	if (len <= reconfs_inline_max(fs->block_size)) {
		if (len)
			kmemcpy(ino->data, data, len);
		ino->inline_len = len;
		goto write;
	}

	page = kzalloc(fs->block_size);
	if (!page) {
		st = RECONFS_ERR_NOMEM;
		goto out;
	}

	while (off < len) {
		u32 take = len - off;
		u64 where;

		if (take > fs->block_size)
			take = fs->block_size;

		/* Zeroed first, so the tail of the last block carries nothing
		 * from whatever previously occupied that block. A block handed
		 * out by the allocator holds somebody's old contents, and
		 * writing only `take` bytes into it would publish the rest. */
		kmemset(page, 0, fs->block_size);
		kmemcpy(page, data + off, take);

		where = reconfs_txn_alloc(txn, ino->dossier);
		if (!where) {
			st = RECONFS_ERR_NOSPACE;
			goto out;
		}

		st = reconfs_write_block(fs, where, page, RECONFS_NO_CSUM);
		if (st != RECONFS_OK)
			goto out;

		if (used < RECONFS_DIRECT) {
			ino->direct[used++] = where;
		} else {
			if (!index) {
				index = kzalloc(fs->block_size);
				if (!index) {
					st = RECONFS_ERR_NOMEM;
					goto out;
				}
			}
			if (in_index == fs->per_index) {
				st = RECONFS_ERR_TOO_LARGE;
				goto out;
			}
			index[in_index++] = where;
		}

		off += take;
	}

	if (index) {
		ino->indirect = reconfs_txn_alloc(txn, ino->dossier);
		if (!ino->indirect) {
			st = RECONFS_ERR_NOSPACE;
			goto out;
		}

		/* After the blocks it names, never before: an index naming a
		 * block that does not exist yet is a structure a crash could
		 * make permanent. */
		st = reconfs_write_block(fs, ino->indirect, index,
					 RECONFS_NO_CSUM);
		if (st != RECONFS_OK)
			goto out;
	}

write:
	st = reconfs_write_block(fs, blk, ino, INODE_CSUM);
	if (st == RECONFS_OK)
		*out_block = blk;

out:
	kfree(index);
	kfree(page);
	kfree(ino);
	return st;
}

/* Releases every block a file's contents occupied, so the next transaction can
 * use them. Not this one -- see reconfs_txn_alloc. */
static void release_contents(struct reconfs_txn *txn, struct reconfs *fs,
			     const struct reconfs_inode *ino)
{
	unsigned i;

	if (ino->inline_len)
		return;

	for (i = 0; i < RECONFS_DIRECT; i++)
		if (ino->direct[i])
			reconfs_txn_free(txn, ino->direct[i]);

	if (ino->indirect) {
		u64 *slots = kzalloc(fs->block_size);

		if (slots) {
			if (reconfs_read_block(fs, ino->indirect, slots,
					       RECONFS_NO_CSUM) == RECONFS_OK) {
				u32 j;

				for (j = 0; j < fs->per_index; j++)
					if (slots[j])
						reconfs_txn_free(txn, slots[j]);
			}
			kfree(slots);
		}

		reconfs_txn_free(txn, ino->indirect);
	}
}

enum reconfs_status reconfs_write_named(struct reconfs_txn *txn,
					struct reconfs *fs, u64 dir_block,
					const char *name, const void *data,
					u32 len, u64 *out_dir)
{
	struct reconfs_inode *dir = NULL, *old = NULL;
	struct entries e;
	enum reconfs_status st;
	u64 child = 0, fresh = 0;
	u32 off;

	kmemset(&e, 0, sizeof(e));

	st = reconfs_lookup(fs, dir_block, name, &child);
	if (st != RECONFS_OK)
		return st;

	dir = kzalloc(fs->block_size);
	old = kzalloc(fs->block_size);
	if (!dir || !old) {
		st = RECONFS_ERR_NOMEM;
		goto out;
	}

	st = reconfs_read_block(fs, dir_block, dir, INODE_CSUM);
	if (st != RECONFS_OK)
		goto out;

	st = reconfs_read_block(fs, child, old, INODE_CSUM);
	if (st != RECONFS_OK)
		goto out;

	if (old->type != RECONFS_TYPE_FILE) {
		st = RECONFS_ERR_NOT_FILE;
		goto out;
	}

	st = write_contents(txn, fs, old, data, len, &fresh);
	if (st != RECONFS_OK)
		goto out;

	/* The old copy's blocks, and the old inode itself. Released after the
	 * new one is written, and reachable from the live superblock until the
	 * commit lands. */
	release_contents(txn, fs, old);
	reconfs_txn_free(txn, child);

	/* The directory's entry has to name the new inode, because the inode
	 * moved -- which is what copy-on-write means and why a write to a file
	 * rewrites the directory above it. */
	st = read_entries(fs, dir, &e);
	if (st != RECONFS_OK)
		goto out;

	off = 0;
	while (off + RECONFS_DIRENT_MIN <= e.len) {
		struct reconfs_dirent *de =
			(struct reconfs_dirent *)(e.buf + off);

		if (!de->rec_len)
			break;
		if (de->inode == child) {
			de->inode = fresh;
			break;
		}
		off += de->rec_len;
	}

	st = write_dir(txn, fs, dir_block, dir, &e, out_dir);

out:
	entries_free(&e);
	kfree(dir);
	kfree(old);
	return st;
}

enum reconfs_status reconfs_read_named(struct reconfs *fs, u64 dir_block,
				       const char *name, void *out, u32 max,
				       u32 *got)
{
	struct reconfs_inode *ino;
	enum reconfs_status st;
	u64 child = 0;
	u8 *dst = out;
	u32 want, off = 0;

	*got = 0;

	st = reconfs_lookup(fs, dir_block, name, &child);
	if (st != RECONFS_OK)
		return st;

	ino = kzalloc(fs->block_size);
	if (!ino)
		return RECONFS_ERR_NOMEM;

	st = reconfs_read_block(fs, child, ino, INODE_CSUM);
	if (st != RECONFS_OK)
		goto out;

	if (ino->type != RECONFS_TYPE_FILE) {
		st = RECONFS_ERR_NOT_FILE;
		goto out;
	}

	/* Refused rather than truncated. A caller handed the first half of a
	 * file, with a success status, has no way to know. */
	if (ino->size > max) {
		st = RECONFS_ERR_TOO_LARGE;
		goto out;
	}

	want = (u32)ino->size;

	if (ino->inline_len) {
		kmemcpy(dst, ino->data, want);
		*got = want;
		goto out;
	}

	{
		u8 *page = kzalloc(fs->block_size);
		u64 *slots = NULL;
		unsigned n = 0;

		if (!page) {
			st = RECONFS_ERR_NOMEM;
			goto out;
		}

		while (off < want) {
			u64 where = 0;
			u32 take = want - off;

			if (take > fs->block_size)
				take = fs->block_size;

			if (n < RECONFS_DIRECT) {
				where = ino->direct[n];
			} else {
				if (!slots) {
					slots = kzalloc(fs->block_size);
					if (!slots) {
						st = RECONFS_ERR_NOMEM;
						break;
					}
					st = reconfs_read_block(fs,
								ino->indirect,
								slots,
								RECONFS_NO_CSUM);
					if (st != RECONFS_OK)
						break;
				}
				if (n - RECONFS_DIRECT >= fs->per_index) {
					st = RECONFS_ERR_CORRUPT;
					break;
				}
				where = slots[n - RECONFS_DIRECT];
			}

			if (!where) {
				st = RECONFS_ERR_CORRUPT;
				break;
			}

			st = reconfs_read_block(fs, where, page,
						RECONFS_NO_CSUM);
			if (st != RECONFS_OK)
				break;

			kmemcpy(dst + off, page, take);
			off += take;
			n++;
		}

		kfree(slots);
		kfree(page);

		if (st == RECONFS_OK)
			*got = want;
	}

out:
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
 * --- Renaming between two directories ---
 *
 * Not here. `reconfs_move` does it, and hands the same-directory case back to
 * this function so the two cannot disagree about what a rename means.
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
	u8 moving_type = RECONFS_TYPE_FILE;
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
				/* Carried from the entry being moved rather
				 * than recomputed: the inode is not being
				 * rewritten, so anything derived here could
				 * only disagree with it -- and a directory
				 * listed as a file is a listing that lies about
				 * what it is showing. */
				moving_type = de->type;
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
		de->type     = moving_type;
		kmemcpy(de->name, to, to_len);
		new_e.len += need;
	}

	st = write_dir(txn, fs, dir_block, dir, &new_e, out_dir);
	if (st != RECONFS_OK)
		goto out;

	/* The object the new name used to refer to.
	 *
	 * Its contents as well as its inode. Freeing the inode alone leaks
	 * every block the replaced file occupied -- which is invisible while
	 * the files being renamed over are empty, and was: the self-test
	 * renamed over a file with nothing in it, and found nothing wrong.
	 * The crash workload writing a real payload is what made the leaked
	 * blocks exist, and the checker named them immediately.
	 *
	 * Released only now, after the new directory is built, and released
	 * rather than written over -- so the live superblock still reaches the
	 * old contents until the commit lands. */
	if (replaced) {
		struct reconfs_inode *gone = kzalloc(fs->block_size);

		if (!gone) {
			st = RECONFS_ERR_NOMEM;
			goto out;
		}

		if (reconfs_read_block(fs, replaced, gone, INODE_CSUM)
		    == RECONFS_OK)
			release_contents(txn, fs, gone);

		kfree(gone);
		reconfs_txn_free(txn, replaced);
	}

out:
	entries_free(&old_e);
	entries_free(&new_e);
	kfree(dir);
	return st;
}

/* --- Paths ----------------------------------------------------------------
 *
 * Everything above works on one directory, identified by the block its inode
 * lives in. That is enough for a flat volume and not enough for the shape
 * ReconFS has to hold: System, Programs and User, each with a recycle bin.
 *
 * The obstacle is copy-on-write itself. Changing anything in `/System/Recycled`
 * writes a new inode for that directory, which changes its block -- so
 * `/System` must be rewritten to name the new block, which changes *its* block,
 * so the root must be rewritten too. The chain has to be copied from the change
 * up to the root, and the superblock names the new root.
 *
 * That is not overhead to be avoided. It is the mechanism: the whole path is
 * built out of blocks nothing reachable can see, and one superblock write makes
 * every one of them real at the same instant. A crash anywhere in the middle
 * leaves the entire old tree, including the directory being changed.
 *
 * --- Two halves, on purpose ---
 *
 * `reconfs_walk_path` finds the chain of directories from the root down, and
 * `reconfs_rebuild_path` copies it back up once the deepest one has moved.
 * Between them the caller does whatever it came to do, using the existing
 * single-directory operations unchanged.
 *
 * Splitting it that way means there is exactly one implementation of "create a
 * name", not one per depth -- and the path machinery can be read on its own,
 * without a create or a rename tangled through it.
 */

/* Replaces one child's block number in a directory, writing a new directory. */
static enum reconfs_status replace_child(struct reconfs_txn *txn,
					 struct reconfs *fs, u64 dir_block,
					 u64 old_child, u64 new_child,
					 u64 *out_dir)
{
	struct reconfs_inode *dir;
	struct entries e;
	enum reconfs_status st;
	u32 off = 0;
	bool found = false;

	kmemset(&e, 0, sizeof(e));

	dir = kzalloc(fs->block_size);
	if (!dir)
		return RECONFS_ERR_NOMEM;

	st = reconfs_read_block(fs, dir_block, dir, INODE_CSUM);
	if (st != RECONFS_OK)
		goto out;

	st = read_entries(fs, dir, &e);
	if (st != RECONFS_OK)
		goto out;

	while (off + RECONFS_DIRENT_MIN <= e.len) {
		struct reconfs_dirent *de =
			(struct reconfs_dirent *)(e.buf + off);

		if (!de->rec_len)
			break;
		if (de->inode == old_child) {
			de->inode = new_child;
			found = true;
			break;
		}
		off += de->rec_len;
	}

	if (!found) {
		/* The chain said this directory contains that one. It does not.
		 * Reported rather than papered over: a path that cannot be
		 * rebuilt is a tree that has already gone wrong. */
		st = RECONFS_ERR_CORRUPT;
		goto out;
	}

	st = write_dir(txn, fs, dir_block, dir, &e, out_dir);

out:
	entries_free(&e);
	kfree(dir);
	return st;
}

/* Splits a path into the chain of directories leading to its last component.
 *
 * "/System/Recycled/note.txt" gives dirs = { root, System, Recycled } and a
 * leaf of "note.txt". A trailing slash, a repeated slash and a missing leading
 * slash are all accepted; "." and ".." are not, because they are the
 * directory's own business rather than names anyone may write.
 */
enum reconfs_status reconfs_walk_path_from(struct reconfs *fs, u64 root,
					   const char *path,
					   struct reconfs_path *chain,
					   char *leaf, size_t leaf_len)
{
	const char *p = path;
	enum reconfs_status st;

	if (!path || !chain || !leaf || !leaf_len)
		return RECONFS_ERR_NAME;

	chain->count = 0;
	chain->dirs[chain->count++] = root;
	leaf[0] = '\0';

	while (*p == '/')
		p++;

	while (*p) {
		char name[RECONFS_NAME_MAX + 1];
		size_t n = 0;
		u64 next = 0;

		while (p[n] && p[n] != '/') {
			if (n >= RECONFS_NAME_MAX)
				return RECONFS_ERR_NAME;
			name[n] = p[n];
			n++;
		}
		name[n] = '\0';

		if (!n)
			return RECONFS_ERR_NAME;

		if (name_eq(".", 1, name) || name_eq("..", 2, name))
			return RECONFS_ERR_NAME;

		p += n;
		while (*p == '/')
			p++;

		if (!*p) {
			/* The last component. It is the leaf, whether or not it
			 * exists and whether or not it is a directory -- the
			 * caller is the one who knows which it wants. */
			if (n + 1 > leaf_len)
				return RECONFS_ERR_NAME;
			kmemcpy(leaf, name, n + 1);
			return RECONFS_OK;
		}

		/* An interior component, which must be a directory that is
		 * already there. */
		st = reconfs_lookup(fs, chain->dirs[chain->count - 1], name,
				    &next);
		if (st != RECONFS_OK)
			return st;

		if (chain->count == RECONFS_PATH_MAX)
			return RECONFS_ERR_TOO_DEEP;

		{
			struct reconfs_inode *probe = kzalloc(fs->block_size);

			if (!probe)
				return RECONFS_ERR_NOMEM;

			st = reconfs_read_block(fs, next, probe, INODE_CSUM);
			if (st == RECONFS_OK && probe->type != RECONFS_TYPE_DIR)
				st = RECONFS_ERR_NOT_DIR;
			kfree(probe);

			if (st != RECONFS_OK)
				return st;
		}

		chain->dirs[chain->count++] = next;
	}

	/* The path named no leaf at all -- "/" or "///". */
	return RECONFS_ERR_NAME;
}

enum reconfs_status reconfs_walk_path(struct reconfs *fs, const char *path,
				      struct reconfs_path *chain, char *leaf,
				      size_t leaf_len)
{
	return reconfs_walk_path_from(fs, fs->root_inode, path, chain, leaf,
				      leaf_len);
}

/* Copies the chain back up, from the directory that changed to the root.
 *
 * `moved` is the new block of `chain->dirs[chain->count - 1]`. Every ancestor
 * is rewritten to name its child's new block, deepest first, so that each new
 * directory is written before the one that will point at it -- an entry naming
 * a block that does not exist yet is a structure a crash could make permanent.
 */
enum reconfs_status reconfs_rebuild_path(struct reconfs_txn *txn,
					 struct reconfs *fs,
					 const struct reconfs_path *chain,
					 u64 moved, u64 *new_root)
{
	u64 child_was, child_now;
	unsigned i;

	if (!chain->count)
		return RECONFS_ERR_CORRUPT;

	child_was = chain->dirs[chain->count - 1];
	child_now = moved;

	for (i = chain->count - 1; i > 0; i--) {
		u64 parent_now = 0;
		enum reconfs_status st;

		st = replace_child(txn, fs, chain->dirs[i - 1], child_was,
				   child_now, &parent_now);
		if (st != RECONFS_OK)
			return st;

		child_was = chain->dirs[i - 1];
		child_now = parent_now;
	}

	*new_root = child_now;
	return RECONFS_OK;
}

/* --- Removing a name ------------------------------------------------------
 *
 * One commit, like everything else: the directory is rebuilt without the entry,
 * and the object it named is released -- its contents and its inode -- in the
 * same transaction.
 *
 * "Released" and not "erased". The blocks go back to the unclaimed state in the
 * *new* owner table, so the live superblock still reaches the old contents until
 * the commit lands, and a crash before that leaves the file entirely intact.
 * That is the two-outcome rule again: the name is there, or it is not.
 *
 * --- What this does not do ---
 *
 * The bytes are not overwritten. A block released here keeps whatever it held
 * until something else is written into it, so deleting a file does not destroy
 * what was in it -- and anything that needs it destroyed has to say so.
 *
 * That is worth stating plainly rather than leaving to be discovered, because
 * "delete" is a word people reasonably expect to mean the other thing. The
 * desktop already has a recycle bin, which is a different promise again; when
 * something needs an erase that is really an erase, it will need a discard that
 * the device honours and a rule about what to do on the devices that do not.
 *
 * --- A directory is refused unless it is empty ---
 *
 * Removing a directory that still has entries would strand everything under it:
 * reachable from nothing, still marked allocated, and the checker would say so
 * on the next run. Refused rather than recursed, because a recursive delete is a
 * decision for the layer that knows whether the user meant it.
 */
/* Takes a name out of a directory.
 *
 * `release` says whether what it named goes with it. Moving an object to
 * another directory takes the name out and keeps the object -- and passing
 * `false` here is the difference between a move and a deletion followed by a
 * broken reference, so the parameter exists rather than a second copy of this
 * function that somebody would forget to keep in step.
 */
static enum reconfs_status remove_entry(struct reconfs_txn *txn,
					struct reconfs *fs, u64 dir_block,
					const char *name, bool release,
					u64 *out_dir, u64 *removed)
{
	struct reconfs_inode *dir = NULL, *gone = NULL;
	struct entries old_e, new_e;
	enum reconfs_status st;
	u64 target = 0;
	u32 off;
	bool found = false;

	kmemset(&old_e, 0, sizeof(old_e));
	kmemset(&new_e, 0, sizeof(new_e));

	if (!name || !*name)
		return RECONFS_ERR_NAME;

	dir = kzalloc(fs->block_size);
	gone = kzalloc(fs->block_size);
	if (!dir || !gone) {
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

	st = read_entries(fs, dir, &old_e);
	if (st != RECONFS_OK)
		goto out;

	new_e.cap = entries_capacity(fs);
	new_e.buf = kzalloc(new_e.cap);
	if (!new_e.buf) {
		st = RECONFS_ERR_NOMEM;
		goto out;
	}

	off = 0;
	while (off + RECONFS_DIRENT_MIN <= old_e.len) {
		const struct reconfs_dirent *de =
			(const struct reconfs_dirent *)(old_e.buf + off);

		if (!de->rec_len)
			break;

		if (de->inode && name_eq(de->name, de->name_len, name)) {
			target = de->inode;
			found = true;
		} else if (de->inode) {
			if (!entries_append(&new_e, de, de->rec_len)) {
				st = RECONFS_ERR_DIR_FULL;
				goto out;
			}
		}

		off += de->rec_len;
	}

	if (!found) {
		st = RECONFS_ERR_NOT_FOUND;
		goto out;
	}

	st = reconfs_read_block(fs, target, gone, INODE_CSUM);
	if (st != RECONFS_OK)
		goto out;

	if (gone->type == RECONFS_TYPE_DIR) {
		struct entries inside;

		st = read_entries(fs, gone, &inside);
		if (st != RECONFS_OK)
			goto out;

		{
			bool empty = true;
			u32 k = 0;

			while (k + RECONFS_DIRENT_MIN <= inside.len) {
				const struct reconfs_dirent *de =
					(const struct reconfs_dirent *)
					(inside.buf + k);

				if (!de->rec_len)
					break;
				if (de->inode) {
					empty = false;
					break;
				}
				k += de->rec_len;
			}

			entries_free(&inside);

			if (!empty) {
				st = RECONFS_ERR_NOT_EMPTY;
				goto out;
			}
		}
	}

	st = write_dir(txn, fs, dir_block, dir, &new_e, out_dir);
	if (st != RECONFS_OK)
		goto out;

	/* After the new directory exists, so the object is unreachable from the
	 * next superblock before it is unreachable from anything. */
	if (release) {
		release_contents(txn, fs, gone);
		reconfs_txn_free(txn, target);
	}

	if (removed)
		*removed = target;

out:
	entries_free(&old_e);
	entries_free(&new_e);
	kfree(dir);
	kfree(gone);
	return st;
}

enum reconfs_status reconfs_remove(struct reconfs_txn *txn, struct reconfs *fs,
				   u64 dir_block, const char *name,
				   u64 *out_dir)
{
	return remove_entry(txn, fs, dir_block, name, true, out_dir, NULL);
}

/* --- Moving between directories -------------------------------------------
 *
 * Renaming within one directory is one rewrite. Renaming across two is four,
 * and the awkwardness is not the count -- it is that both directories sit on
 * chains that have to be copied to the root, and the two chains share a prefix.
 * Rebuilding them independently would produce two different roots.
 *
 * So it is done in sequence, against a tree that is consistent at every step:
 *
 *   1. walk to the source, take the name out, rebuild that chain      -> root A
 *   2. walk to the destination *in the tree rooted at A*
 *   3. rewrite the moved object with its new parent
 *   4. put the name in, rebuild that chain                            -> root B
 *
 * Step 2 is why `reconfs_walk_path_from` takes a root: after step 1 the
 * committed root is still the old one, and walking from it would find the
 * destination's *previous* block and rebuild a chain that undoes step 1.
 *
 * All four rewrites are still one transaction, so the superblock write at the
 * end makes the whole move real at once. There is no image in which the name
 * exists in both places or in neither.
 */
enum reconfs_status reconfs_move(struct reconfs_txn *txn, struct reconfs *fs,
				 const char *from, const char *to,
				 u64 *new_root)
{
	struct reconfs_path chain;
	char leaf_from[RECONFS_NAME_MAX + 1], leaf_to[RECONFS_NAME_MAX + 1];
	struct reconfs_inode *ino = NULL;
	enum reconfs_status st;
	u64 moving = 0, moved = 0, dir_now = 0, root = 0, to_dossier = 0;
	u32 type;

	st = reconfs_walk_path(fs, from, &chain, leaf_from, sizeof(leaf_from));
	if (st != RECONFS_OK)
		return st;

	st = reconfs_lookup(fs, chain.dirs[chain.count - 1], leaf_from, &moving);
	if (st != RECONFS_OK)
		return st;

	/* Within one directory this is the cheaper operation, and the two must
	 * not disagree about what a rename means. Handed over rather than
	 * reimplemented. */
	{
		struct reconfs_path dest;

		st = reconfs_walk_path(fs, to, &dest, leaf_to, sizeof(leaf_to));
		if (st != RECONFS_OK)
			return st;

		if (dest.dirs[dest.count - 1] == chain.dirs[chain.count - 1]) {
			st = reconfs_rename(txn, fs,
					    chain.dirs[chain.count - 1],
					    leaf_from, leaf_to, &dir_now);
			if (st != RECONFS_OK)
				return st;
			return reconfs_rebuild_path(txn, fs, &chain, dir_now,
						    new_root);
		}
	}

	ino = kzalloc(fs->block_size);
	if (!ino)
		return RECONFS_ERR_NOMEM;

	st = reconfs_read_block(fs, moving, ino, INODE_CSUM);
	if (st != RECONFS_OK)
		goto out;

	type = ino->type;

	/* 1. Out of the source, keeping the object. */
	st = remove_entry(txn, fs, chain.dirs[chain.count - 1], leaf_from,
			  false, &dir_now, NULL);
	if (st != RECONFS_OK)
		goto out;

	st = reconfs_rebuild_path(txn, fs, &chain, dir_now, &root);
	if (st != RECONFS_OK)
		goto out;

	/* 2. The destination, in the tree that now exists. */
	st = reconfs_walk_path_from(fs, root, to, &chain, leaf_to,
				    sizeof(leaf_to));
	if (st != RECONFS_OK)
		goto out;

	/* 3. The object's back-reference is its parent's dossier, which has
	 *    changed. The dossier is stable, so this is the *only* thing that
	 *    has to be rewritten -- had the back-reference been the parent's
	 *    block, every descendant would need rewriting too. */
	{
		struct reconfs_inode *parent = kzalloc(fs->block_size);

		if (!parent) {
			st = RECONFS_ERR_NOMEM;
			goto out;
		}

		st = reconfs_read_block(fs, chain.dirs[chain.count - 1], parent,
					INODE_CSUM);
		if (st == RECONFS_OK)
			to_dossier = parent->dossier;
		kfree(parent);

		if (st != RECONFS_OK)
			goto out;
	}

	ino->parent = to_dossier;
	ino->ctime  = time_monotonic_ns();

	moved = reconfs_txn_alloc(txn, to_dossier);
	if (!moved) {
		st = RECONFS_ERR_NOSPACE;
		goto out;
	}

	st = reconfs_write_block(fs, moved, ino, INODE_CSUM);
	if (st != RECONFS_OK)
		goto out;

	reconfs_txn_free(txn, moving);

	/* 4. Into the destination, replacing whatever was there. */
	st = add_entry(txn, fs, chain.dirs[chain.count - 1], leaf_to, moved,
		       (u8)type, &dir_now);
	if (st != RECONFS_OK)
		goto out;

	st = reconfs_rebuild_path(txn, fs, &chain, dir_now, new_root);

out:
	kfree(ino);
	return st;
}

/* --- Listing --------------------------------------------------------------
 *
 * The checkpoint's seventh constraint says a listing must come with a stated
 * guarantee about concurrent modification, or a stated absence of one, because
 * an unstated guarantee is the worst of the three.
 *
 * --- reconfs_readdir has no guarantee, and this is the statement of that ---
 *
 * It takes a directory block and an index and returns one entry. Between two
 * calls the directory can be changed, and the block the caller is holding can
 * be *reused*: copy-on-write frees the old copy when the change commits, and a
 * later transaction may allocate that block for something else entirely. A
 * caller iterating across such a change can see an entry twice, miss one, or
 * read a block that is no longer a directory at all.
 *
 * An earlier version of this comment claimed the opposite -- "a listing is a
 * snapshot of one epoch" -- which is the exact failure the constraint was
 * written against, made worse by being written down confidently. It is
 * withdrawn.
 *
 * --- reconfs_list does have one ---
 *
 * It reads the whole directory in a single call, from a directory that cannot
 * change while it is being read, because nothing else is running inside that
 * call. A listing that has no part-way cannot observe a change part-way
 * through.
 *
 * The cost is that the caller supplies a buffer big enough for the whole
 * directory and is told so if it is not, rather than being handed a prefix. A
 * caller given the first half of a directory with a success status has no way
 * to know.
 */

/* Every name in a directory, in one call.
 *
 * `names` receives NUL-terminated names back to back; `blocks` receives the
 * matching inode blocks. `count` is set to how many there were. A directory
 * with more entries than the buffers hold is refused, not truncated.
 */
enum reconfs_status reconfs_list(struct reconfs *fs, u64 dir_block,
				 char *names, size_t names_len,
				 u64 *blocks, unsigned max, unsigned *count)
{
	struct reconfs_inode *dir;
	struct entries e;
	enum reconfs_status st;
	size_t used = 0;
	u32 off = 0;
	unsigned n = 0;

	*count = 0;

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
			if (n == max || used + de->name_len + 1 > names_len) {
				entries_free(&e);
				return RECONFS_ERR_TOO_LARGE;
			}

			kmemcpy(names + used, de->name, de->name_len);
			used += de->name_len;
			names[used++] = '\0';
			blocks[n++] = de->inode;
		}

		off += de->rec_len;
	}

	entries_free(&e);
	*count = n;
	return RECONFS_OK;
}
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
