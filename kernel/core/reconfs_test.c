/* ReconFS -- proving the checker before trusting anything it says.
 *
 * --- Why this runs before any filesystem code is trusted ---
 *
 * docs/RECONFS.md puts the reverse sweep second on the list of things to test,
 * and says to run the checker against a deliberately corrupted image *first*
 * and confirm that it fails. That order is not a preference. Three harness bugs
 * in one afternoon (BG-114, BG-115, BG-116) all presented as clean passes, and
 * two of them were in the same script:
 *
 *     a test that has never been seen to fail is not a test yet
 *
 * So this formats a volume, checks it, and then breaks it on purpose in each of
 * the ways the format can be broken -- and every one of those must be caught.
 * A run where the checker passes a corrupted image is a failure of this test
 * even though nothing crashed.
 *
 * --- Why it is gated on the command line ---
 *
 * It formats. Formatting destroys whatever was there, and this kernel is meant
 * to be installable beside an existing operating system without harming it. A
 * self-test that runs by default and writes a filesystem over the first disk it
 * finds is the single most destructive thing this kernel could contain, so it
 * runs only when `reconfs=<device>` names a device explicitly, and it refuses
 * even then if the device carries a partition table.
 */
#include <recon/kernel/reconfs.h>

#include <recon/kernel/block.h>
#include <recon/kernel/boot.h>
#include <recon/kernel/console.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/kstring.h>

#define SUPER_CSUM	RK_OFFSETOF(struct reconfs_super, checksum)
#define INODE_CSUM	RK_OFFSETOF(struct reconfs_inode, checksum)

static const char *wanted_device(void)
{
	static char name[BLOCK_NAME_MAX];
	const char *p = boot_info()->cmdline;
	static const char key[] = "reconfs=";

	if (!p)
		return 0;

	while (*p) {
		const char *k = key;
		const char *q = p;

		while (*k && *q == *k) {
			q++;
			k++;
		}

		if (!*k) {
			size_t n = 0;

			while (q[n] && q[n] != ' ' && n + 1 < sizeof(name)) {
				name[n] = q[n];
				n++;
			}
			name[n] = '\0';
			return n ? name : 0;
		}

		while (*p && *p != ' ')
			p++;
		while (*p == ' ')
			p++;
	}

	return 0;
}

static struct block_device *find_by_name(const char *name)
{
	for (unsigned i = 0; i < block_device_count(); i++) {
		struct block_device *d = block_device_at(i);
		const char *a = d->name, *b = name;

		while (*a && *a == *b) {
			a++;
			b++;
		}
		if (*a == *b)
			return d;
	}
	return 0;
}

/* Each of these breaks the image in one specific way and returns what the
 * checker is expected to notice. The checker is not told which one ran. */
enum breakage {
	BREAK_ORPHAN_BLOCK,	/* allocated, reachable from nothing */
	BREAK_LOST_OWNER,	/* reachable from the root, marked free */
	BREAK_BAD_PARENT,	/* an inode disowned by its own directory */
	BREAK_SUPER_CHECKSUM,	/* a superblock that no longer adds up */
	BREAK_WRONG_OWNER,	/* allocated to one object, reachable from another */
	BREAK_COUNT
};

/* kprintf takes no field width, and growing it for one caller would be an
 * interface change made to tidy an output column. Padded here instead. */
static void put_padded(const char *s, unsigned width)
{
	unsigned n = 0;

	kputs(s);
	while (s[n])
		n++;
	while (n++ < width)
		kputc(' ');
}

static const char *breakage_name(enum breakage b)
{
	switch (b) {
	case BREAK_ORPHAN_BLOCK:   return "a block allocated to nobody";
	case BREAK_LOST_OWNER:     return "a reachable block marked free";
	case BREAK_BAD_PARENT:     return "an inode whose parent is wrong";
	case BREAK_SUPER_CHECKSUM: return "a superblock that does not add up";
	case BREAK_WRONG_OWNER:    return "a block owned by the wrong object";
	default:                   return "?";
	}
}

/* Returns false if the breakage could not be applied, which is not the same as
 * the checker failing to notice and must not be reported as a pass. */
static bool apply_breakage(struct reconfs *fs, enum breakage b)
{
	switch (b) {
	case BREAK_ORPHAN_BLOCK: {
		/* Claim a free block for an inode that does not own it. The
		 * reverse sweep sees an allocated block; the forward walk never
		 * reaches it. */
		u64 victim = fs->total_blocks - 1;

		return reconfs_set_owner(fs, victim, RECONFS_DOSSIER_ROOT)
		       == RECONFS_OK;
	}
	case BREAK_LOST_OWNER:
		/* The opposite direction: the root's own inode block, marked
		 * free. The forward walk reaches it; the sweep does not. */
		return reconfs_set_owner(fs, fs->root_inode, RECONFS_OWNER_VOID)
		       == RECONFS_OK;

	case BREAK_BAD_PARENT: {
		struct reconfs_inode *ino = kzalloc(fs->block_size);
		bool ok = false;

		if (!ino)
			return false;

		if (reconfs_read_block(fs, fs->root_inode, ino, INODE_CSUM)
		    == RECONFS_OK) {
			ino->parent = RECONFS_DOSSIER_ROOT;	/* its own parent */
			ok = reconfs_write_block(fs, fs->root_inode, ino,
						 INODE_CSUM) == RECONFS_OK;
		}
		kfree(ino);
		return ok;
	}
	case BREAK_SUPER_CHECKSUM: {
		/* Flip a byte in the live superblock without restamping the
		 * checksum. Nothing should mount this. */
		u8 *sb = kzalloc(fs->block_size);
		bool ok = false;

		if (!sb)
			return false;

		if (reconfs_read_block(fs, fs->live_super, sb, SUPER_CSUM)
		    == RECONFS_OK) {
			sb[64] ^= 0xFF;
			/* Written with RECONFS_NO_CSUM so the write does not
			 * helpfully repair the damage on the way out. */
			ok = reconfs_write_block(fs, fs->live_super, sb,
						 RECONFS_NO_CSUM) == RECONFS_OK;
		}
		kfree(sb);
		return ok;
	}
	case BREAK_WRONG_OWNER: {
		/* The root's own inode block, recorded in the table as
		 * belonging to some other object.
		 *
		 * This is the one comparison branch nothing else reaches: the
		 * forward walk finds the block and knows who owns it, the table
		 * says somebody else, and neither answer is "unclaimed". A
		 * checker that only tested the reachable/unreachable pair would
		 * pass an image where every block is accounted for and half of
		 * them are accounted to the wrong file. */
		return reconfs_set_owner(fs, fs->root_inode, 9999) == RECONFS_OK;
	}
	default:
		return false;
	}
}

/* What the checker said about the last fault it was shown, so it can be printed
 * under the fault's name rather than above it. */
static const char *last_reason;
static u64 last_reason_block;

/* Did the checker notice? For the superblock case the damage is caught at
 * mount, which is the correct place and still counts as noticing. */
static bool noticed(struct block_device *dev, enum breakage b)
{
	struct reconfs fs;
	struct reconfs_check_result r;
	enum reconfs_status st;
	bool saw;

	last_reason = 0;
	last_reason_block = 0;

	st = reconfs_mount(dev, &fs);

	if (b == BREAK_SUPER_CHECKSUM) {
		/* One superblock is damaged; the other is still valid, so the
		 * volume must still mount -- that is the whole point of there
		 * being two. What must be true is that the damaged one is not
		 * the one believed. */
		if (st != RECONFS_OK)
			return false;

		/* A format leaves both superblocks valid and equal, and the
		 * mount believes A. The damage was applied to whichever was
		 * believed, so a mount that still believes it did not notice. */
		saw = (fs.live_super != RECONFS_SUPER_A);
		reconfs_unmount(&fs);
		return saw;
	}

	if (st != RECONFS_OK)
		return true;		/* noticed, at mount */

	st = reconfs_check(&fs, &r);
	saw = (st != RECONFS_OK) || (r.disagreements != 0);

	/* Kept, not printed: it belongs under the line naming the fault, and
	 * this runs before that line exists. */
	last_reason = r.first_disagreement;
	last_reason_block = r.first_disagreement_block;

	reconfs_unmount(&fs);
	return saw;
}

static bool run_at(struct block_device *dev, u32 bs);

void reconfs_run(void)
{
	const char *want = wanted_device();
	struct block_device *dev;

	if (!want)
		return;

	dev = find_by_name(want);
	if (!dev) {
		kprintf("reconfs: no device called %s\n", want);
		return;
	}

	if (dev->read_only) {
		kprintf("reconfs: %s is read-only\n", want);
		return;
	}

	/* The one refusal that matters. A device carrying a partition table has
	 * somebody's operating system on it, and this test writes a filesystem
	 * over the front of whatever it is pointed at. There is no flag to
	 * override this. */
	if (dev->slice_count) {
		kprintf("reconfs: %s carries a partition table; refusing to format it\n",
			want);
		return;
	}

	/* Every block size the format allows, because the size is now read from
	 * the medium rather than compiled in -- and a path that has only ever
	 * run at 4096 has not been shown to work at 65536. The structures move:
	 * superblock B sits at a fixed byte offset, so its block *number*
	 * changes with the size, and so does everything after it. */
	{
		static const u32 sizes[] = { 4096, 16384, 65536 };
		unsigned k;
		unsigned good = 0;

		kprintf("\nreconfs: formatting %s and checking the checker\n", want);

		for (k = 0; k < RK_ARRAY_LEN(sizes); k++)
			if (run_at(dev, sizes[k]))
				good++;

		kprintf("  %u of %u block sizes behaved\n",
			good, (unsigned)RK_ARRAY_LEN(sizes));
	}
}

/* Create anything, anywhere: walk down, do the one-directory operation, copy the
 * chain back up, commit. This is the whole of what the path machinery is for,
 * and it is four lines because the operations underneath did not have to learn
 * about depth. */
static enum reconfs_status make_at(struct reconfs *fs, const char *path,
				   u32 type, u32 mode)
{
	struct reconfs_txn *txn;
	struct reconfs_path chain;
	char leaf[RECONFS_NAME_MAX + 1];
	u64 made = 0, moved = 0, root = 0;
	enum reconfs_status st;

	st = reconfs_walk_path(fs, path, &chain, leaf, sizeof(leaf));
	if (st != RECONFS_OK)
		return st;

	txn = reconfs_txn_begin(fs);
	if (!txn)
		return RECONFS_ERR_NOMEM;

	st = reconfs_create(txn, fs, chain.dirs[chain.count - 1], leaf, type,
			    mode, &made, &moved);
	if (st == RECONFS_OK)
		st = reconfs_rebuild_path(txn, fs, &chain, moved, &root);

	if (st != RECONFS_OK) {
		reconfs_txn_abort(txn);
		return st;
	}

	reconfs_txn_set_root(txn, root);
	return reconfs_txn_commit(txn);
}

static enum reconfs_status put_at(struct reconfs *fs, const char *path,
				  const void *data, u32 len)
{
	struct reconfs_txn *txn;
	struct reconfs_path chain;
	char leaf[RECONFS_NAME_MAX + 1];
	u64 moved = 0, root = 0;
	enum reconfs_status st;

	st = reconfs_walk_path(fs, path, &chain, leaf, sizeof(leaf));
	if (st != RECONFS_OK)
		return st;

	txn = reconfs_txn_begin(fs);
	if (!txn)
		return RECONFS_ERR_NOMEM;

	st = reconfs_write_named(txn, fs, chain.dirs[chain.count - 1], leaf,
				 data, len, &moved);
	if (st == RECONFS_OK)
		st = reconfs_rebuild_path(txn, fs, &chain, moved, &root);

	if (st != RECONFS_OK) {
		reconfs_txn_abort(txn);
		return st;
	}

	reconfs_txn_set_root(txn, root);
	return reconfs_txn_commit(txn);
}

static bool run_at(struct block_device *dev, u32 bs)
{
	struct reconfs fs;
	struct reconfs_check_result r;
	enum reconfs_status st;
	unsigned b;
	unsigned caught = 0;

	kprintf("\n  --- %u-byte blocks ---\n", bs);

	st = reconfs_format(dev, "ReconOS", bs);
	if (st != RECONFS_OK) {
		kprintf("  format             : FAIL (%s)\n", reconfs_strerror(st));
		return false;
	}

	st = reconfs_mount(dev, &fs);
	if (st != RECONFS_OK) {
		kprintf("  mount              : FAIL (%s)\n", reconfs_strerror(st));
		return false;
	}

	st = reconfs_check(&fs, &r);
	if (st != RECONFS_OK || r.disagreements) {
		kprintf("  a fresh volume     : FAIL (%s, %llu disagreements)\n",
			reconfs_strerror(st),
			(unsigned long long)r.disagreements);
		if (r.first_disagreement)
			kprintf("      %s at block %llu\n", r.first_disagreement,
				(unsigned long long)r.first_disagreement_block);
		reconfs_unmount(&fs);
		return false;
	}

	/* Both numbers, because they are not the same number, and printing one
	 * of them as "both ways" is the kind of small dishonesty that makes a
	 * summary worse than no summary. The forward walk sees what is
	 * reachable from the root; the reverse sweep also sees the metadata the
	 * filesystem stands on, which nothing above it points at. */
	kprintf("  a fresh volume     : pass (%llu reachable, %llu allocated, %llu inodes)\n",
		(unsigned long long)r.blocks_seen_forward,
		(unsigned long long)r.blocks_seen_reverse,
		(unsigned long long)r.inodes);

	/* --- The commit path -------------------------------------------------
	 *
	 * Two commits, on the volume that was just checked clean.
	 *
	 * The first changes nothing but the epoch, which is the case that has to
	 * work before any other one can: it proves the superblock alternates,
	 * that the epoch moves, and that a volume is still coherent afterwards.
	 * The second allocates a block, which is the case that exercises the
	 * owner table being copied on write -- the part with the chicken-and-egg
	 * problem, where taking a block to hold the new table changes the table.
	 *
	 * What is checked after each is not "it returned OK". It is that the
	 * volume still passes the whole checker, because a commit that half
	 * worked returns OK too. */
	{
		struct reconfs_txn *txn;
		u64 before = fs.super.epoch;
		u64 live_before = fs.live_super;
		u64 taken;

		txn = reconfs_txn_begin(&fs);
		if (!txn || reconfs_txn_commit(txn) != RECONFS_OK) {
			kputs("  an empty commit    : FAIL\n");
			reconfs_unmount(&fs);
			return false;
		}

		if (fs.super.epoch != before + 1 ||
		    fs.live_super == live_before) {
			kprintf("  an empty commit    : FAIL (epoch %llu -> "
				"%llu, superblock %llu -> %llu)\n",
				(unsigned long long)before,
				(unsigned long long)fs.super.epoch,
				(unsigned long long)live_before,
				(unsigned long long)fs.live_super);
			reconfs_unmount(&fs);
			return false;
		}

		st = reconfs_check(&fs, &r);
		if (st != RECONFS_OK || r.disagreements) {
			kprintf("  an empty commit    : FAIL (%llu "
				"disagreements after)\n",
				(unsigned long long)r.disagreements);
			reconfs_unmount(&fs);
			return false;
		}

		kprintf("  an empty commit    : pass (epoch %llu, superblock "
			"%llu)\n", (unsigned long long)fs.super.epoch,
			(unsigned long long)fs.live_super);

		/* Now one that touches the allocator, to exercise the part with
		 * the chicken-and-egg problem: taking a block to hold the new
		 * owner table changes the owner table.
		 *
		 * Taken and given back inside the same transaction, so the
		 * volume it commits has nothing new in it. An earlier version
		 * kept the block, owned by the archive and reachable from
		 * nothing -- which is a leak, and which passed only because the
		 * checker exempted archive-owned blocks from having to be
		 * reachable. Removing that exemption (BG-122) made this test
		 * fail, correctly, on the first run. */
		before = fs.super.epoch;
		txn = reconfs_txn_begin(&fs);
		if (!txn) {
			kputs("  a commit that allocates : FAIL (no transaction)\n");
			reconfs_unmount(&fs);
			return false;
		}

		taken = reconfs_txn_alloc(txn, RECONFS_OWNER_ARCHIVE);
		if (!taken || reconfs_txn_failed(txn)) {
			kputs("  a commit that allocates : FAIL (no block)\n");
			reconfs_txn_abort(txn);
			reconfs_unmount(&fs);
			return false;
		}

		reconfs_txn_free(txn, taken);

		if (reconfs_txn_commit(txn) != RECONFS_OK) {
			kputs("  a commit that allocates : FAIL (commit)\n");
			reconfs_unmount(&fs);
			return false;
		}

		st = reconfs_check(&fs, &r);
		if (st != RECONFS_OK || r.disagreements) {
			kprintf("  a commit that allocates : FAIL (%llu "
				"disagreements: %s)\n",
				(unsigned long long)r.disagreements,
				r.first_disagreement ? r.first_disagreement : "");
			reconfs_unmount(&fs);
			return false;
		}

		if (fs.super.epoch != before + 1) {
			kputs("  a commit that allocates : FAIL (epoch)\n");
			reconfs_unmount(&fs);
			return false;
		}

		kprintf("  a commit that takes a block : pass (block %llu "
			"taken and given back, epoch %llu)\n",
			(unsigned long long)taken,
			(unsigned long long)fs.super.epoch);

		/* A block released by a transaction must not be handed back out
		 * by that same transaction. It is unclaimed in the transaction's
		 * table and still reachable from the live superblock, so writing
		 * it would overwrite storage the current filesystem is using.
		 *
		 * Reaching that case takes some doing, and the first version of
		 * this test did not. It allocated a block, freed it, allocated
		 * again, and checked the two differed -- which passes whether or
		 * not the exclusion exists, because the allocator scans *forward*
		 * from a hint and had already moved past the freed block. It was
		 * run with the bug deliberately reintroduced and passed three
		 * times out of three.
		 *
		 * The allocator only revisits a released block after wrapping at
		 * the end of the volume. So: fill the volume, release one block,
		 * and ask for another. The wrap is then unavoidable.
		 *
		 * With the exclusion, that request must *fail* -- the only
		 * unclaimed block on the volume is one this transaction may not
		 * touch until its commit lands. A failure is the correct answer,
		 * and handing back the block instead is the corruption. */
		{
			struct reconfs_txn *t2 = reconfs_txn_begin(&fs);
			u64 last = 0, released, again;
			u64 n = 0;

			if (!t2) {
				kputs("  a freed block is not reused : FAIL (no txn)\n");
				reconfs_unmount(&fs);
				return false;
			}

			/* Fill it. Nothing is written -- allocation only moves
			 * numbers in the transaction's in-memory owner table. */
			for (;;) {
				u64 got = reconfs_txn_alloc(t2, RECONFS_OWNER_ARCHIVE);

				if (!got || reconfs_txn_failed(t2))
					break;
				last = got;
				n++;
			}

			if (!last || reconfs_txn_failed(t2)) {
				kprintf("  a freed block is not reused : FAIL "
					"(could not fill: %llu taken, txn %s)\n",
					(unsigned long long)n,
					reconfs_txn_failed(t2) ? "failed"
							       : "ok");
				reconfs_txn_abort(t2);
				reconfs_unmount(&fs);
				return false;
			}

			released = last;
			reconfs_txn_free(t2, released);

			again = reconfs_txn_alloc(t2, RECONFS_OWNER_ARCHIVE);


			if (again) {
				kprintf("  a freed block is not reused : FAIL "
					"(gave back %llu, released moments "
					"earlier)\n",
					(unsigned long long)again);
				reconfs_txn_abort(t2);
				reconfs_unmount(&fs);
				return false;
			}

			reconfs_txn_abort(t2);
			kprintf("  a freed block is not reused : pass (%llu blocks "
				"taken, %llu released, none given back)\n",
				(unsigned long long)n,
				(unsigned long long)released);
		}

		/* --- Names -------------------------------------------------
		 *
		 * Three files in the root, then look them up, list them, and
		 * check the volume still holds together. The last part is the
		 * one that matters: a create that returns OK and leaves the
		 * owner table disagreeing with the tree is a create that has
		 * corrupted the volume while reporting success. */
		{
			struct reconfs_txn *t3 = reconfs_txn_begin(&fs);
			static const char *names[] = {
				"Registry.dat", "Theme.ini", "ReadMe.txt"
			};
			u64 made[3] = { 0, 0, 0 };
			u64 dir = fs.root_inode;
			unsigned k;
			bool ok = true;

			if (!t3) {
				kputs("  names              : FAIL (no txn)\n");
				reconfs_unmount(&fs);
				return false;
			}

			for (k = 0; k < RK_ARRAY_LEN(names); k++) {
				u64 new_dir = 0;

				st = reconfs_create(t3, &fs, dir, names[k],
						    RECONFS_TYPE_FILE, 0600,
						    &made[k], &new_dir);
				if (st != RECONFS_OK) {
					kprintf("  names              : FAIL "
						"(create %s: %s)\n", names[k],
						reconfs_strerror(st));
					reconfs_txn_abort(t3);
					reconfs_unmount(&fs);
					return false;
				}
				dir = new_dir;
			}

			/* A name already taken must be refused, not duplicated. */
			{
				u64 a = 0, b = 0;

				if (reconfs_create(t3, &fs, dir, "theme.INI",
						   RECONFS_TYPE_FILE, 0600,
						   &a, &b) != RECONFS_ERR_EXISTS) {
					kputs("  names              : FAIL (a name "
					      "differing only in case was allowed "
					      "twice)\n");
					reconfs_txn_abort(t3);
					reconfs_unmount(&fs);
					return false;
				}
			}

			reconfs_txn_set_root(t3, dir);

			if (reconfs_txn_commit(t3) != RECONFS_OK) {
				kputs("  names              : FAIL (commit)\n");
				reconfs_unmount(&fs);
				return false;
			}

			/* Found by the name that was typed, and by one that differs
			 * only in case -- stored as given, compared without. */
			for (k = 0; k < RK_ARRAY_LEN(names); k++) {
				u64 found = 0;

				if (reconfs_lookup(&fs, fs.root_inode, names[k],
						   &found) != RECONFS_OK ||
				    found != made[k])
					ok = false;
			}

			{
				u64 found = 0;

				if (reconfs_lookup(&fs, fs.root_inode, "THEME.ini",
						   &found) != RECONFS_OK ||
				    found != made[1])
					ok = false;

				if (reconfs_lookup(&fs, fs.root_inode, "absent",
						   &found) != RECONFS_ERR_NOT_FOUND)
					ok = false;
			}

			/* And the listing returns the names as they were typed.
			 *
			 * Both ways: one entry at a time, which has no
			 * guarantee about concurrent modification and says so,
			 * and the whole directory in one call, which does. */
			{
				char got[RECONFS_NAME_MAX + 1];
				u64 child;
				u32 type;
				unsigned seen = 0;

				while (reconfs_readdir(&fs, fs.root_inode, seen, got,
						       sizeof(got), &child,
						       &type) == RECONFS_OK)
					seen++;

				if (seen != RK_ARRAY_LEN(names))
					ok = false;
			}

			{
				char all[256];
				u64 blocks[8];
				unsigned n = 0, i;

				if (reconfs_list(&fs, fs.root_inode, all,
						 sizeof(all), blocks, 8, &n)
				    != RECONFS_OK ||
				    n != RK_ARRAY_LEN(names))
					ok = false;

				/* The names come back exactly as they were
				 * typed, in order, back to back. */
				{
					const char *p = all;

					for (i = 0; i < n && ok; i++) {
						const char *want = names[i];
						size_t j = 0;

						while (p[j] && p[j] == want[j])
							j++;
						if (p[j] || want[j])
							ok = false;
						p += j + 1;
					}
				}

				/* A directory that does not fit is refused,
				 * not truncated. A caller handed the first two
				 * of three names, with a success status, has no
				 * way to know. */
				if (reconfs_list(&fs, fs.root_inode, all,
						 sizeof(all), blocks, 2, &n)
				    != RECONFS_ERR_TOO_LARGE)
					ok = false;
			}

			st = reconfs_check(&fs, &r);
			if (st != RECONFS_OK || r.disagreements) {
				kprintf("  names              : FAIL (%llu "
					"disagreements: %s)\n",
					(unsigned long long)r.disagreements,
					r.first_disagreement ? r.first_disagreement
							     : "");
				reconfs_unmount(&fs);
				return false;
			}

			if (!ok) {
				kputs("  names              : FAIL (lookup or "
				      "listing)\n");
				reconfs_unmount(&fs);
				return false;
			}

			kprintf("  three names        : pass (%llu blocks now "
				"reachable, %llu inodes)\n",
				(unsigned long long)r.blocks_seen_forward,
				(unsigned long long)r.inodes);
		}

		/* --- Contents ----------------------------------------------
		 *
		 * Written and read back at the sizes where the layout changes
		 * shape, because those are the only sizes where it can be wrong:
		 *
		 *   0                 nothing at all
		 *   1                 inline, one byte
		 *   inline_max        inline, exactly full
		 *   inline_max + 1    the first size that needs a block
		 *   2 blocks + 7      several blocks and a partial tail
		 *   13 blocks         one more than the direct pointers: indirect
		 *
		 * The last only at the smallest block size; at 64KiB it would be
		 * most of a megabyte, and what it exercises is the indirect
		 * pointer, which does not care how big a block is.
		 *
		 * The bytes are a position-dependent pattern, so a read that
		 * returns the right *length* of the wrong data fails. Reading back
		 * only the length is the mistake this shape is chosen to prevent:
		 * a file of zeroes has the right length too. */
		{
			u32 inline_max = reconfs_inline_max(fs.block_size);
			u32 sizes[6];
			unsigned count = 5, k;
			u8 *src, *back;
			u32 biggest;

			sizes[0] = 0;
			sizes[1] = 1;
			sizes[2] = inline_max;
			sizes[3] = inline_max + 1;
			sizes[4] = fs.block_size * 2 + 7;
			if (fs.block_size == 4096)
				sizes[count++] = fs.block_size * 13;

			biggest = sizes[count - 1];
			if (sizes[4] > biggest)
				biggest = sizes[4];

			src  = kzalloc(biggest ? biggest : 1);
			back = kzalloc(biggest ? biggest : 1);
			if (!src || !back) {
				kputs("  contents           : FAIL (memory)\n");
				kfree(src);
				kfree(back);
				reconfs_unmount(&fs);
				return false;
			}

			for (u32 q = 0; q < biggest; q++)
				src[q] = (u8)(q * 31u + (q >> 8) * 7u + 1u);

			for (k = 0; k < count; k++) {
				struct reconfs_txn *tw = reconfs_txn_begin(&fs);
				u64 dir = 0;
				u32 got = 0;
				u32 q;

				if (!tw) {
					kputs("  contents           : FAIL (txn)\n");
					goto contents_failed;
				}

				st = reconfs_write_named(tw, &fs, fs.root_inode,
							 "ReadMe.txt", src,
							 sizes[k], &dir);
				if (st != RECONFS_OK) {
					kprintf("  contents           : FAIL "
						"(write %u bytes: %s)\n",
						(unsigned)sizes[k],
						reconfs_strerror(st));
					reconfs_txn_abort(tw);
					goto contents_failed;
				}

				reconfs_txn_set_root(tw, dir);
				if (reconfs_txn_commit(tw) != RECONFS_OK) {
					kprintf("  contents           : FAIL "
						"(commit %u bytes)\n",
						(unsigned)sizes[k]);
					goto contents_failed;
				}

				kmemset(back, 0, biggest ? biggest : 1);
				st = reconfs_read_named(&fs, fs.root_inode,
							"ReadMe.txt", back,
							biggest ? biggest : 1, &got);
				if (st != RECONFS_OK || got != sizes[k]) {
					kprintf("  contents           : FAIL "
						"(read %u bytes, got %u: %s)\n",
						(unsigned)sizes[k], (unsigned)got,
						reconfs_strerror(st));
					goto contents_failed;
				}

				for (q = 0; q < sizes[k]; q++) {
					if (back[q] == src[q])
						continue;
					kprintf("  contents           : FAIL (%u bytes: "
						"byte %u is %u, should be %u)\n",
						(unsigned)sizes[k], (unsigned)q,
						(unsigned)back[q], (unsigned)src[q]);
					goto contents_failed;
				}

				st = reconfs_check(&fs, &r);
				if (st != RECONFS_OK || r.disagreements) {
					kprintf("  contents           : FAIL (%u bytes "
						"left %llu disagreements: %s)\n",
						(unsigned)sizes[k],
						(unsigned long long)r.disagreements,
						r.first_disagreement
							? r.first_disagreement : "");
					goto contents_failed;
				}
			}

			kprintf("  contents           : pass (%u sizes up to %u "
				"bytes, read back byte for byte)\n",
				count, (unsigned)sizes[count - 1]);
			kfree(src);
			kfree(back);
			goto contents_done;

contents_failed:
			kfree(src);
			kfree(back);
			reconfs_unmount(&fs);
			return false;

contents_done:
			;
		}

		/* --- Rename, which is what the registry is waiting on ------
		 *
		 * The pattern the desktop needs: write a temporary file, then
		 * rename it over the real one. What must be true afterwards is
		 * that the real name refers to the new object and the old object
		 * is gone -- and that the volume still holds together, because a
		 * rename that returns OK and leaves the owner table disagreeing
		 * with the tree has corrupted the volume while reporting success.
		 */
		{
			struct reconfs_txn *t4 = reconfs_txn_begin(&fs);
			u64 fresh = 0, dir = fs.root_inode, found = 0;
			u64 doomed = 0;

			if (!t4 ||
			    reconfs_lookup(&fs, dir, "Registry.dat",
					   &doomed) != RECONFS_OK) {
				kputs("  rename             : FAIL (setup)\n");
				if (t4)
					reconfs_txn_abort(t4);
				reconfs_unmount(&fs);
				return false;
			}

			st = reconfs_create(t4, &fs, dir, "Registry.tmp",
					    RECONFS_TYPE_FILE, 0600, &fresh, &dir);
			if (st != RECONFS_OK) {
				kprintf("  rename             : FAIL (create: %s)\n",
					reconfs_strerror(st));
				reconfs_txn_abort(t4);
				reconfs_unmount(&fs);
				return false;
			}

			st = reconfs_rename(t4, &fs, dir, "Registry.tmp",
					    "Registry.dat", &dir);
			if (st != RECONFS_OK) {
				kprintf("  rename             : FAIL (rename: %s)\n",
					reconfs_strerror(st));
				reconfs_txn_abort(t4);
				reconfs_unmount(&fs);
				return false;
			}

			reconfs_txn_set_root(t4, dir);

			if (reconfs_txn_commit(t4) != RECONFS_OK) {
				kputs("  rename             : FAIL (commit)\n");
				reconfs_unmount(&fs);
				return false;
			}

			/* The real name now refers to what was the temporary file. */
			if (reconfs_lookup(&fs, fs.root_inode, "Registry.dat",
					   &found) != RECONFS_OK ||
			    found != fresh || found == doomed) {
				kputs("  rename             : FAIL (the name does not "
				      "refer to the new object)\n");
				reconfs_unmount(&fs);
				return false;
			}

			/* And the temporary name is gone -- one name, not two. */
			if (reconfs_lookup(&fs, fs.root_inode, "Registry.tmp",
					   &found) != RECONFS_ERR_NOT_FOUND) {
				kputs("  rename             : FAIL (both names "
				      "exist)\n");
				reconfs_unmount(&fs);
				return false;
			}

			st = reconfs_check(&fs, &r);
			if (st != RECONFS_OK || r.disagreements) {
				kprintf("  rename             : FAIL (%llu "
					"disagreements: %s)\n",
					(unsigned long long)r.disagreements,
					r.first_disagreement ? r.first_disagreement
							     : "");
				reconfs_unmount(&fs);
				return false;
			}

			kprintf("  rename over a name : pass (%llu inodes, the "
				"replaced one released)\n",
				(unsigned long long)r.inodes);
		}

		/* --- Removing a name ---------------------------------------
		 *
		 * The check that matters is not that the name is gone. It is
		 * that **the space came back** — which is exactly what renaming
		 * over a file failed to do (BG-121), invisibly, because the test
		 * that covered it used empty files.
		 *
		 * So this writes a file large enough to need blocks of its own,
		 * records how many blocks the volume was using, removes it, and
		 * requires the count to return to where it started. */
		{
			struct reconfs_txn *tr;
			u64 before_blocks, after_blocks, dir = 0, found = 0;
			u32 payload = fs.block_size * 3 + 11;
			u8 *body = kzalloc(payload);

			if (!body) {
				kputs("  removing           : FAIL (memory)\n");
				reconfs_unmount(&fs);
				return false;
			}

			for (u32 q = 0; q < payload; q++)
				body[q] = (u8)(q ^ 0x5A);

			before_blocks = fs.super.blocks_used;

			/* Make it, fill it. */
			tr = reconfs_txn_begin(&fs);
			if (!tr ||
			    reconfs_create(tr, &fs, fs.root_inode, "Doomed.bin",
					   RECONFS_TYPE_FILE, 0600, &found,
					   &dir) != RECONFS_OK ||
			    reconfs_write_named(tr, &fs, dir, "Doomed.bin", body,
						payload, &dir) != RECONFS_OK) {
				kputs("  removing           : FAIL (setup)\n");
				if (tr)
					reconfs_txn_abort(tr);
				kfree(body);
				reconfs_unmount(&fs);
				return false;
			}
			reconfs_txn_set_root(tr, dir);
			if (reconfs_txn_commit(tr) != RECONFS_OK) {
				kputs("  removing           : FAIL (commit)\n");
				kfree(body);
				reconfs_unmount(&fs);
				return false;
			}
			kfree(body);

			if (fs.super.blocks_used <= before_blocks) {
				kputs("  removing           : FAIL (the file cost "
				      "no blocks, so this proves nothing)\n");
				reconfs_unmount(&fs);
				return false;
			}

			/* And take it away again. */
			tr = reconfs_txn_begin(&fs);
			if (!tr ||
			    reconfs_remove(tr, &fs, fs.root_inode, "doomed.BIN",
					   &dir) != RECONFS_OK) {
				kputs("  removing           : FAIL (remove)\n");
				if (tr)
					reconfs_txn_abort(tr);
				reconfs_unmount(&fs);
				return false;
			}
			reconfs_txn_set_root(tr, dir);
			if (reconfs_txn_commit(tr) != RECONFS_OK) {
				kputs("  removing           : FAIL (commit)\n");
				reconfs_unmount(&fs);
				return false;
			}

			after_blocks = fs.super.blocks_used;

			if (reconfs_lookup(&fs, fs.root_inode, "Doomed.bin",
					   &found) != RECONFS_ERR_NOT_FOUND) {
				kputs("  removing           : FAIL (the name is "
				      "still there)\n");
				reconfs_unmount(&fs);
				return false;
			}

			if (after_blocks != before_blocks) {
				kprintf("  removing           : FAIL (%llu blocks "
					"before, %llu after — the space did not "
					"come back)\n",
					(unsigned long long)before_blocks,
					(unsigned long long)after_blocks);
				reconfs_unmount(&fs);
				return false;
			}

			st = reconfs_check(&fs, &r);
			if (st != RECONFS_OK || r.disagreements) {
				kprintf("  removing           : FAIL (%llu "
					"disagreements: %s)\n",
					(unsigned long long)r.disagreements,
					r.first_disagreement ? r.first_disagreement
							     : "");
				reconfs_unmount(&fs);
				return false;
			}

			kprintf("  removing           : pass (%u bytes freed, "
				"back to %llu blocks)\n", (unsigned)payload,
				(unsigned long long)after_blocks);
		}

		/* --- The shape recon_fs actually needs ----------------------
		 *
		 * Three volumes' worth of directories, each with a recycle bin,
		 * and a file three levels down. This is the constraint the
		 * checkpoint was given — "it must hold recon_fs's shape" — and
		 * until the path machinery existed nothing below the root could
		 * be changed at all.
		 *
		 * What is being tested is not that mkdir works. It is that a
		 * change three levels down rewrites the chain to the root and
		 * leaves the volume agreeing with itself — which is the part
		 * copy-on-write makes non-obvious. */
		{
			static const char *dirs[] = {
				"/System", "/Programs", "/User",
				"/System/Recycled", "/Programs/Recycled",
				"/User/Recycled",
			};
			static const char *deep = "/User/Recycled/note.txt";
			u32 len = fs.block_size + 137;
			u8 *body, *back;
			u32 got = 0, q;
			unsigned k;
			u64 parent = 0, found = 0;

			for (k = 0; k < RK_ARRAY_LEN(dirs); k++) {
				st = make_at(&fs, dirs[k], RECONFS_TYPE_DIR, 0755);
				if (st != RECONFS_OK) {
					kprintf("  the desktop shape  : FAIL "
						"(%s: %s)\n", dirs[k],
						reconfs_strerror(st));
					reconfs_unmount(&fs);
					return false;
				}
			}

			body = kzalloc(len);
			back = kzalloc(len);
			if (!body || !back) {
				kfree(body); kfree(back);
				kputs("  the desktop shape  : FAIL (memory)\n");
				reconfs_unmount(&fs);
				return false;
			}
			for (q = 0; q < len; q++)
				body[q] = (u8)(q * 13u + 7u);

			st = make_at(&fs, deep, RECONFS_TYPE_FILE, 0600);
			if (st == RECONFS_OK)
				st = put_at(&fs, deep, body, len);

			if (st != RECONFS_OK) {
				kprintf("  the desktop shape  : FAIL (%s: %s)\n",
					deep, reconfs_strerror(st));
				kfree(body); kfree(back);
				reconfs_unmount(&fs);
				return false;
			}

			/* Read it back through the path, and compare the bytes —
			 * a file of the right length in the wrong place would
			 * otherwise look like success. */
			{
				struct reconfs_path chain;
				char leaf[RECONFS_NAME_MAX + 1];

				st = reconfs_walk_path(&fs, deep, &chain, leaf,
						       sizeof(leaf));
				if (st == RECONFS_OK) {
					parent = chain.dirs[chain.count - 1];
					st = reconfs_read_named(&fs, parent, leaf,
								back, len, &got);
				}
			}

			if (st != RECONFS_OK || got != len) {
				kprintf("  the desktop shape  : FAIL (read %u of %u: "
					"%s)\n", (unsigned)got, (unsigned)len,
					reconfs_strerror(st));
				kfree(body); kfree(back);
				reconfs_unmount(&fs);
				return false;
			}

			for (q = 0; q < len; q++) {
				if (back[q] == body[q])
					continue;
				kprintf("  the desktop shape  : FAIL (byte %u)\n",
					(unsigned)q);
				kfree(body); kfree(back);
				reconfs_unmount(&fs);
				return false;
			}

			kfree(body);
			kfree(back);

			/* A path through a file, and a path that is not there, are
			 * both refused rather than resolved to something nearby. */
			{
				struct reconfs_path chain;
				char leaf[RECONFS_NAME_MAX + 1];

				if (reconfs_walk_path(&fs, "/User/Recycled/note.txt/x",
						      &chain, leaf,
						      sizeof(leaf))
				    != RECONFS_ERR_NOT_DIR ||
				    reconfs_walk_path(&fs, "/Nowhere/at/all",
						      &chain, leaf,
						      sizeof(leaf))
				    != RECONFS_ERR_NOT_FOUND) {
					kputs("  the desktop shape  : FAIL (a bad "
					      "path resolved)\n");
					reconfs_unmount(&fs);
					return false;
				}
			}

			if (reconfs_lookup(&fs, fs.root_inode, "system",
					   &found) != RECONFS_OK) {
				kputs("  the desktop shape  : FAIL (the root lost a "
				      "name on the way back up)\n");
				reconfs_unmount(&fs);
				return false;
			}

			st = reconfs_check(&fs, &r);
			if (st != RECONFS_OK || r.disagreements) {
				kprintf("  the desktop shape  : FAIL (%llu "
					"disagreements: %s)\n",
					(unsigned long long)r.disagreements,
					r.first_disagreement ? r.first_disagreement
							     : "");
				reconfs_unmount(&fs);
				return false;
			}

			/* --- Across two directories ------------------------
			 *
			 * Four directories rewritten and two chains copied to
			 * the root, in one commit. What must be true after it
			 * is not only that the name moved: the file's contents
			 * must be intact, and its back-reference must name its
			 * new parent -- which the checker verifies, since a
			 * moved object whose parent still points at where it
			 * came from is exactly the disagreement it looks for.
			 */
			{
				struct reconfs_txn *tm = reconfs_txn_begin(&fs);
				struct reconfs_path where;
				char leaf2[RECONFS_NAME_MAX + 1];
				u64 root2 = 0;
				u32 back_len = fs.block_size + 137;
				u8 *check = kzalloc(back_len);
				u32 n = 0, z;

				if (!tm || !check) {
					kfree(check);
					if (tm)
						reconfs_txn_abort(tm);
					kputs("  a move across      : FAIL (setup)\n");
					reconfs_unmount(&fs);
					return false;
				}

				st = reconfs_move(tm, &fs, deep,
						  "/System/Recycled/note.txt",
						  &root2);
				if (st != RECONFS_OK) {
					kprintf("  a move across      : FAIL "
						"(%s)\n", reconfs_strerror(st));
					reconfs_txn_abort(tm);
					kfree(check);
					reconfs_unmount(&fs);
					return false;
				}

				reconfs_txn_set_root(tm, root2);
				if (reconfs_txn_commit(tm) != RECONFS_OK) {
					kputs("  a move across      : FAIL (commit)\n");
					kfree(check);
					reconfs_unmount(&fs);
					return false;
				}

				/* Gone from where it was. */
				if (reconfs_walk_path(&fs, deep, &where, leaf2,
						      sizeof(leaf2)) == RECONFS_OK &&
				    reconfs_lookup(&fs,
						   where.dirs[where.count - 1],
						   leaf2, &found)
				    != RECONFS_ERR_NOT_FOUND) {
					kputs("  a move across      : FAIL (still "
					      "at the old path)\n");
					kfree(check);
					reconfs_unmount(&fs);
					return false;
				}

				/* There, and unchanged. */
				st = reconfs_walk_path(&fs,
						       "/System/Recycled/note.txt",
						       &where, leaf2,
						       sizeof(leaf2));
				if (st == RECONFS_OK)
					st = reconfs_read_named(&fs,
							where.dirs[where.count - 1],
							leaf2, check, back_len,
							&n);

				if (st != RECONFS_OK || n != len) {
					kprintf("  a move across      : FAIL "
						"(read %u of %u: %s)\n",
						(unsigned)n, (unsigned)len,
						reconfs_strerror(st));
					kfree(check);
					reconfs_unmount(&fs);
					return false;
				}

				for (z = 0; z < len; z++) {
					if (check[z] == (u8)(z * 13u + 7u))
						continue;
					kprintf("  a move across      : FAIL "
						"(byte %u changed)\n",
						(unsigned)z);
					kfree(check);
					reconfs_unmount(&fs);
					return false;
				}

				kfree(check);

				/* And the volume still agrees with itself --
				 * which is where a stale back-reference would
				 * show up. */
				st = reconfs_check(&fs, &r);
				if (st != RECONFS_OK || r.disagreements) {
					kprintf("  a move across      : FAIL (%llu "
						"disagreements: %s)\n",
						(unsigned long long)r.disagreements,
						r.first_disagreement
							? r.first_disagreement : "");
					reconfs_unmount(&fs);
					return false;
				}

				kputs("  a move across      : pass (two chains "
				      "copied, contents and parent intact)\n");
			}

			kprintf("  the desktop shape  : pass (%llu inodes, a file "
				"three deep, %llu blocks both ways)\n",
				(unsigned long long)r.inodes,
				(unsigned long long)r.blocks_seen_forward);
		}

		/* And it must survive being read back from scratch. A commit
		 * that is only correct in the memory of the process that made
		 * it is not a commit. */
		/* Whatever the epoch is now, after everything above, is what a
		 * remount must find. Comparing against a number captured
		 * further up was how this read before, and it started failing
		 * the moment a test was inserted between the two -- an
		 * assertion about the state of the test rather than about the
		 * filesystem. */
		{
			u64 committed = fs.super.epoch;
			u64 named = 0;

			reconfs_unmount(&fs);

			if (reconfs_mount(dev, &fs) != RECONFS_OK) {
				kputs("  a commit that survives : FAIL "
				      "(remount)\n");
				return false;
			}

			if (fs.super.epoch != committed) {
				kprintf("  a commit that survives : FAIL "
					"(epoch %llu committed, %llu after "
					"remount)\n",
					(unsigned long long)committed,
					(unsigned long long)fs.super.epoch);
				reconfs_unmount(&fs);
				return false;
			}

			/* And the names survive with it. A commit that
			 * preserved the epoch and lost the directory would
			 * pass an epoch check and be useless. */
			if (reconfs_lookup(&fs, fs.root_inode, "Registry.dat",
					   &named) != RECONFS_OK) {
				kputs("  a commit that survives : FAIL (the "
				      "renamed file is not there)\n");
				reconfs_unmount(&fs);
				return false;
			}
		}

		st = reconfs_check(&fs, &r);
		if (st != RECONFS_OK || r.disagreements) {
			kputs("  a commit that survives : FAIL (check)\n");
			reconfs_unmount(&fs);
			return false;
		}

		kputs("  a commit that survives a remount : pass\n");
	}

	reconfs_unmount(&fs);

	/* Now break it, one way at a time, reformatting between each so the
	 * damage from one cannot be what the next one finds. */
	for (b = 0; b < BREAK_COUNT; b++) {
		bool applied, saw;

		if (reconfs_format(dev, "ReconOS", bs) != RECONFS_OK) {
			kprintf("  reformat           : FAIL\n");
			return false;
		}

		if (reconfs_mount(dev, &fs) != RECONFS_OK) {
			kprintf("  remount            : FAIL\n");
			return false;
		}

		applied = apply_breakage(&fs, (enum breakage)b);
		reconfs_unmount(&fs);

		if (!applied) {
			kputs("  ");
			put_padded(breakage_name((enum breakage)b), 34);
			kputs(": COULD NOT BREAK IT\n");
			continue;
		}

		saw = noticed(dev, (enum breakage)b);

		kputs("  ");
		put_padded(breakage_name((enum breakage)b), 34);
		kputs(saw ? ": caught\n" : ": MISSED\n");

		if (saw && last_reason)
			kprintf("      it said: %s (block %llu)\n",
				last_reason,
				(unsigned long long)last_reason_block);

		if (saw)
			caught++;
	}

	kprintf("  the checker caught %u of %u deliberate faults\n",
		caught, (unsigned)BREAK_COUNT);

	if (caught != BREAK_COUNT) {
		kputs("  a checker that misses a fault it was shown is not a checker\n");
		return false;
	}

	return true;
}
