/* See rootfs.h for what this is for and what it deliberately does not do. */
#include <recon/kernel/rootfs.h>
#include <recon/kernel/vfs.h>
#include <recon/kernel/user.h>

#include <recon/kernel/block.h>
#include <recon/kernel/console.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/kstring.h>

static struct reconfs the_root;
static bool mounted;

/* Which device it came from, for the summary. A machine with several ReconFS
 * volumes takes the first, and a person needs to be able to see which. */
static char from_device[BLOCK_NAME_MAX];

struct reconfs *rootfs(void)
{
	return mounted ? &the_root : NULL;
}

void rootfs_init(void)
{
	unsigned i;

	for (i = 0; i < block_device_count(); i++) {
		struct block_device *d = block_device_at(i);

		if (!d || !d->present)
			continue;

		/* Whole disks are skipped in favour of their slices. A ReconFS
		 * volume lives in a partition; a whole disk that mounts is
		 * either an unpartitioned volume, which is fine, or a disk
		 * whose first blocks happen to look like one. Trying slices
		 * first means the ordinary case is never decided by luck. */
		if (d->slice_count)
			continue;

		if (reconfs_mount(d, &the_root) != RECONFS_OK)
			continue;

		kstrlcpy(from_device, d->name, sizeof(from_device));
		mounted = true;
		return;
	}
}

enum reconfs_status rootfs_list(const char *path, char *names, u64 names_len,
				u64 *needed, unsigned *count)
{
	struct reconfs *fs = rootfs();
	struct reconfs_path chain;
	char leaf[RECONFS_NAME_MAX + 1];
	enum reconfs_status st;
	u64 dir, child = 0;
	unsigned n = 0;
	u64 *blocks;
	char *scratch;

	if (needed)
		*needed = 0;
	if (count)
		*count = 0;

	if (!fs)
		return RECONFS_ERR_NOT_MOUNTED;

	st = reconfs_walk_path(fs, path, &chain, leaf, sizeof(leaf));
	if (st != RECONFS_OK)
		return st;

	/* The root is the one path with no leaf: its parent chain ends at it.
	 * Every other path names something inside the directory the chain ends
	 * at, and that something has to be looked up before it can be read. */
	if (!leaf[0]) {
		dir = chain.count ? chain.dirs[chain.count - 1]
				  : fs->root_inode;
	} else {
		if (chain.count == 0)
			return RECONFS_ERR_NAME;

		st = reconfs_lookup(fs, chain.dirs[chain.count - 1], leaf,
				    &child);
		if (st != RECONFS_OK)
			return st;
		if (!child)
			return RECONFS_ERR_NOT_FOUND;

		dir = child;
	}

	/* Read into our own buffers and copy out only if it all fits.
	 * `reconfs_list` refuses a directory larger than the buffers it is
	 * given, so asking it with the caller's size would turn "your buffer is
	 * small" into "this directory cannot be read" -- and the caller could
	 * not tell those apart, which is the whole reason it wants a size back.
	 */
	scratch = kzalloc(RECONFS_LIST_BYTES);
	blocks  = kzalloc(RECONFS_LIST_MAX * sizeof(*blocks));

	if (!scratch || !blocks) {
		kfree(scratch);
		kfree(blocks);
		return RECONFS_ERR_NOMEM;
	}

	st = reconfs_list(fs, dir, scratch, RECONFS_LIST_BYTES,
			  blocks, RECONFS_LIST_MAX, &n);

	if (st == RECONFS_OK) {
		u64 used = 0;
		unsigned i;

		for (i = 0; i < n; i++)
			used += kstrlen(scratch + used) + 1;

		if (needed)
			*needed = used;

		if (names && used <= names_len) {
			kmemcpy(names, scratch, used);

			if (count)
				*count = n;
		}
	}

	kfree(scratch);
	kfree(blocks);

	return st;
}

void rootfs_print_summary(void)
{
	kprintf("\nFilesystem\n");

	if (!mounted) {
		/* Not a failure, and it says which of the two it is. A machine
		 * with no ReconFS volume is ordinary -- every disk in the
		 * verification rig is blank, and a machine booted from
		 * installation media has not been installed onto yet. */
		kputs("  root         : none found; file calls will say so\n");
		return;
	}

	kprintf("  root         : %s, %u-byte blocks\n",
		from_device, the_root.block_size);
}

/* --- creating ------------------------------------------------------------- */

enum reconfs_status rootfs_replace_file(const char *path, const void *data,
					u32 len)
{
	struct reconfs *fs = rootfs();
	struct reconfs_txn *txn;
	struct reconfs_path chain;
	char leaf[RECONFS_NAME_MAX + 1];
	enum reconfs_status st;
	u64 dir = 0, new_root = 0, existing = 0;

	if (!fs)
		return RECONFS_ERR_NOT_MOUNTED;

	if (!path || !*path)
		return RECONFS_ERR_NAME;

	if (len && !data)
		return RECONFS_ERR_NAME;

	st = reconfs_walk_path(fs, path, &chain, leaf, sizeof(leaf));
	if (st != RECONFS_OK)
		return st;

	if (chain.count == 0)
		return RECONFS_ERR_NAME;

	/* It has to be there. A replace that created what it could not find
	 * would be a create with a different name, and the difference between
	 * them is the only reason both exist. */
	if (reconfs_lookup(fs, chain.dirs[chain.count - 1], leaf,
			   &existing) != RECONFS_OK || !existing)
		return RECONFS_ERR_NOT_FOUND;

	txn = reconfs_txn_begin(fs);
	if (!txn)
		return RECONFS_ERR_NOMEM;

	/* One transaction, so there is no instant at which the file holds half
	 * of each version. The old contents are what anybody reading sees until
	 * the commit, and the new ones the moment after it -- which is the
	 * property a power cut in the middle of this depends on. */
	st = reconfs_write_named(txn, fs, chain.dirs[chain.count - 1], leaf,
				 data, len, &dir);
	if (st != RECONFS_OK)
		goto abort;

	st = reconfs_rebuild_path(txn, fs, &chain, dir, &new_root);
	if (st != RECONFS_OK)
		goto abort;

	reconfs_txn_set_root(txn, new_root);
	return reconfs_txn_commit(txn);

abort:
	reconfs_txn_abort(txn);
	return st;
}

enum reconfs_status rootfs_create_file(const char *path, u32 mode,
				       const void *data, u32 len)
{
	struct reconfs *fs = rootfs();
	struct reconfs_txn *txn;
	struct reconfs_path chain;
	char leaf[RECONFS_NAME_MAX + 1];
	enum reconfs_status st;
	u64 made = 0, dir = 0, new_root = 0;
	u64 existing = 0;

	if (!fs)
		return RECONFS_ERR_NOT_MOUNTED;

	if (!path || !*path)
		return RECONFS_ERR_NAME;

	if (len && !data)
		return RECONFS_ERR_NAME;

	/* Walked before the transaction opens, to find the parent and to answer
	 * "does this already exist" while the tree is still the committed one. */
	st = reconfs_walk_path(fs, path, &chain, leaf, sizeof(leaf));
	if (st != RECONFS_OK)
		return st;

	if (chain.count == 0)
		return RECONFS_ERR_NAME;

	/* Refused rather than replaced. See the header: a create that silently
	 * overwrites is how running a key-generation routine a second time
	 * destroys the key that was working. */
	if (reconfs_lookup(fs, chain.dirs[chain.count - 1], leaf,
			   &existing) == RECONFS_OK && existing)
		return RECONFS_ERR_EXISTS;

	txn = reconfs_txn_begin(fs);
	if (!txn)
		return RECONFS_ERR_NOMEM;

	/* Create, then fill, then rebuild the path to the root -- all inside
	 * one transaction. This is the whole point of the file: the mode is set
	 * by the create, the contents by the write, and *neither is visible*
	 * until the commit at the bottom. There is no ordering between them a
	 * power cut can catch, because there is no intermediate state to catch.
	 */
	st = reconfs_create(txn, fs, chain.dirs[chain.count - 1], leaf,
			    RECONFS_TYPE_FILE, mode, &made, &dir);
	if (st != RECONFS_OK)
		goto abort;

	if (len) {
		st = reconfs_write_named(txn, fs, dir, leaf, data, len, &dir);
		if (st != RECONFS_OK)
			goto abort;
	}

	/* The leaf's parent moved, so every directory above it has to be
	 * rewritten up to the root. Copy-on-write's cost, and the reason this
	 * is one call rather than a loop each caller writes for itself. */
	st = reconfs_rebuild_path(txn, fs, &chain, dir, &new_root);
	if (st != RECONFS_OK)
		goto abort;

	reconfs_txn_set_root(txn, new_root);
	return reconfs_txn_commit(txn);

abort:
	reconfs_txn_abort(txn);
	return st;
}

/* The inode's own fields, read once.
 *
 * rootfs_read_file already loads the inode when a caller wants the mode, so
 * this is the same walk with the other two fields taken out of it as well --
 * not a second way of finding a file, which would be a second way of being
 * wrong about where one is. */
enum reconfs_status rootfs_owner_of(const char *path, u32 *mode, u32 *uid,
				    u32 *gid, u64 *dossier)
{
	struct reconfs *fs = rootfs();
	struct reconfs_path chain;
	char leaf[RECONFS_NAME_MAX + 1];
	struct reconfs_inode *inode;
	enum reconfs_status st;
	u64 dir, child = 0;

	if (!fs)
		return RECONFS_ERR_NOT_MOUNTED;

	st = reconfs_walk_path(fs, path, &chain, leaf, sizeof(leaf));
	if (st != RECONFS_OK)
		return st;

	if (chain.count == 0)
		return RECONFS_ERR_NAME;

	dir = chain.dirs[chain.count - 1];

	st = reconfs_lookup(fs, dir, leaf, &child);
	if (st != RECONFS_OK)
		return st;
	if (!child)
		return RECONFS_ERR_NOT_FOUND;

	inode = kzalloc(fs->block_size);
	if (!inode)
		return RECONFS_ERR_NOMEM;

	st = reconfs_read_block(fs, child, inode,
				RK_OFFSETOF(struct reconfs_inode, checksum));

	if (st == RECONFS_OK) {
		/* Each guarded, because this header follows the same rule the
		 * one above it does and BG-161 is what happens when it does
		 * not: a promise that a pointer may be null, kept for one of
		 * them and not the other. */
		if (mode)
			*mode = inode->mode;
		if (uid)
			*uid = inode->uid;
		if (gid)
			*gid = inode->gid;
		if (dossier)
			*dossier = inode->dossier;
	}

	kfree(inode);
	return st;
}

enum reconfs_status rootfs_read_file(const char *path, void *out, u32 max,
				     u32 *got, u32 *mode)
{
	struct reconfs *fs = rootfs();
	struct reconfs_path chain;
	char leaf[RECONFS_NAME_MAX + 1];
	enum reconfs_status st;
	u64 dir, child = 0;

	if (!fs)
		return RECONFS_ERR_NOT_MOUNTED;

	st = reconfs_walk_path(fs, path, &chain, leaf, sizeof(leaf));
	if (st != RECONFS_OK)
		return st;

	if (chain.count == 0)
		return RECONFS_ERR_NAME;

	dir = chain.dirs[chain.count - 1];

	if (mode) {
		struct reconfs_inode *inode;

		st = reconfs_lookup(fs, dir, leaf, &child);
		if (st != RECONFS_OK)
			return st;
		if (!child)
			return RECONFS_ERR_NOT_FOUND;

		inode = kzalloc(fs->block_size);
		if (!inode)
			return RECONFS_ERR_NOMEM;

		st = reconfs_read_block(fs, child, inode, RK_OFFSETOF(struct reconfs_inode, checksum));
		if (st == RECONFS_OK)
			*mode = inode->mode;

		kfree(inode);

		if (st != RECONFS_OK)
			return st;
	}

	if (!out)
		return RECONFS_OK;

	/* A caller that does not want the count gets a place to put it anyway.
	 *
	 * This header has promised since it was written that either pointer may
	 * be null, and `mode` above honours that. `got` did not: it was handed
	 * straight to reconfs_read_named, which writes through it before it does
	 * anything else. The first caller to believe the header wrote to address
	 * zero in kernel mode. (BG-161) */
	{
		u32 ignored;

		return reconfs_read_named(fs, dir, leaf, out, max,
					  got ? got : &ignored);
	}
}

/* --- the self-test --------------------------------------------------------
 *
 * What is checked is the property the desktop asked for, which is not "a mode
 * can be stored". It is that the mode a file is created with is the mode it
 * has, from the first instant the file is visible -- so the read-back is of the
 * *mode*, through a fresh lookup, and not of a value this file remembered.
 */
bool rootfs_self_test(void)
{
	static const char content[] = "a secret, written once";
	struct reconfs *fs = rootfs();
	char back[64];
	u32 got = 0, mode = 0;
	enum reconfs_status st;
	bool ok = true;

	if (!fs) {
		/* A machine with no volume is a machine this must still boot
		 * on, and saying so is better than reporting a pass for a test
		 * that did not run. */
		kputs("  rootfs: no filesystem on this machine to test against\n");
		return true;
	}

	st = rootfs_create_file("/mode-test", 0600, content, sizeof(content));
	if (st != RECONFS_OK) {
		kprintf("  rootfs: could not create the test file (%d)\n",
			(int)st);
		return false;
	}

	/* Created a second time. It must refuse: a create that overwrites is
	 * the failure this would have if it were written the obvious way. */
	if (rootfs_create_file("/mode-test", 0600, content,
			       sizeof(content)) == RECONFS_OK) {
		kputs("  rootfs: creating the same name twice was allowed\n");
		ok = false;
	}

	kmemset(back, 0, sizeof(back));
	st = rootfs_read_file("/mode-test", back, sizeof(back), &got, &mode);
	if (st != RECONFS_OK) {
		kprintf("  rootfs: could not read it back (%d)\n", (int)st);
		return false;
	}

	if (mode != 0600) {
		kprintf("  rootfs: created with mode 0%o and read back as "
			"0%o\n", 0600, mode);
		ok = false;
	}

	if (got != sizeof(content) ||
	    kmemcmp(back, content, sizeof(content)) != 0) {
		kputs("  rootfs: the contents did not survive the round "
		      "trip\n");
		ok = false;
	}

	/* And a mode that is not the default, so that a test passing because
	 * everything happens to be 0600 is caught. */
	st = rootfs_create_file("/mode-test-2", 0644, content,
				sizeof(content));
	if (st == RECONFS_OK) {
		mode = 0;
		if (rootfs_read_file("/mode-test-2", NULL, 0, NULL,
				     &mode) == RECONFS_OK && mode != 0644) {
			kprintf("  rootfs: asked for 0644 and got 0%o\n", mode);
			ok = false;
		}
	} else {
		kprintf("  rootfs: could not create the second test file "
			"(%d)\n", (int)st);
		ok = false;
	}

	return ok;
}

/* Runs the test, having first looked again for a volume.
 *
 * Looking again is not tidiness. rootfs_init() runs early, before anything has
 * had a chance to format a disk, so on a machine that arrives with no volume
 * the answer at boot is "none" and stays that way -- including on every machine
 * in the verification rig. A second look after the filesystem battery has run
 * is what makes this test something other than a sentence about not running.
 */
void rootfs_run(void)
{
	/* Mounted again unconditionally, not only when nothing is mounted.
	 *
	 * The filesystem battery that runs just before this one *formats* its
	 * device. A volume mounted at boot and then reformatted underneath
	 * leaves this file holding a superblock that no longer describes the
	 * disk -- and the first read through it fails a checksum, which reads
	 * as a corrupt volume rather than as a stale mount.
	 *
	 * Found exactly that way: the test passed on a fresh disk and failed
	 * with a checksum error on a disk that already had a volume, which is
	 * the same run in a different order. */
	mounted = false;
	rootfs_init();

	if (!rootfs()) {
		kputs("  files carry a mode : no volume on this machine\n");
		return;
	}

	kprintf("  files carry a mode : %s\n",
		rootfs_self_test() ? "pass" : "FAIL");
	kprintf("  files by descriptor : %s\n",
		vfs_file_self_test() ? "pass" : "FAIL");
	kprintf("  a program from a volume : %s\n",
		user_exec_path_test() ? "pass" : "FAIL");
	kprintf("  a file, mapped      : %s\n",
		vfs_mmap_self_test() ? "pass" : "FAIL");
}
