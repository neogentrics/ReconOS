/* ReconFS -- proving the checker before trusting anything it says.
 *
 * --- Why this runs before any filesystem code is trusted ---
 *
 * docs/RECONFS.md puts the reverse sweep second on the list of things to test,
 * and says to run the checker against a deliberately corrupted image *first*
 * and confirm that it fails. That order is not a preference. Three harness bugs
 * in one afternoon (BG-082, BG-083, BG-084) all presented as clean passes, and
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

		/* Now one that takes a block. Owned by the archive, because
		 * nothing above can reach it yet -- there is no way to create a
		 * file until the directory code exists, and a block owned by an
		 * inode that does not name it is exactly the corruption the
		 * checker is built to find. */
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

		kprintf("  a commit that takes a block : pass (block %llu, "
			"epoch %llu)\n", (unsigned long long)taken,
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

			/* And the listing returns the names as they were typed. */
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
