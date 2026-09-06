/* ReconFS -- the workload a power cut is aimed at.
 *
 * --- What this is for ---
 *
 * docs/RECONFS.md puts one test first: *cut the power inside a rename and
 * assert the target is old-complete or new-complete.* This is the half that
 * runs inside the machine. The half that judges what survived is
 * scripts/reconfs-check.py, which is a second implementation of the format,
 * written from the header rather than from this code -- because a reader and a
 * writer built from the same misunderstanding agree perfectly.
 *
 * --- Why two commits and not one ---
 *
 * The pattern being modelled is what the desktop's registry writer needs to do:
 *
 *     write everything into a temporary name
 *     rename the temporary name over the real one
 *
 * Those are two separate operations for the caller, so they are two separate
 * commits here. Doing both in one transaction would be easier and would test
 * something weaker -- a crash could then never land *between* them, which is
 * exactly the interval the guarantee is about.
 *
 * So after a cut there are two legitimate images: one where `settings` is the
 * old object and a leftover `settings.tmp` may exist, and one where `settings`
 * is the new object. What must never exist is an image where `settings` is
 * missing, resolves to two things, or resolves to something that is not a
 * complete inode.
 *
 * --- Why it never stops on its own ---
 *
 * It loops until it is killed. A workload that finishes has chosen the moment
 * the crash cannot happen after, and the moment that matters is the one nobody
 * chose.
 */
#include <recon/kernel/reconfs.h>

#include <recon/kernel/block.h>
#include <recon/kernel/boot.h>
#include <recon/kernel/console.h>
#include <recon/kernel/crc32.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/kstring.h>

#define TARGET	"settings"
#define TEMP	"settings.tmp"

static const char *wanted_device(void)
{
	static char name[BLOCK_NAME_MAX];
	const char *p = boot_info()->cmdline;
	static const char key[] = "reconfs-crash=";

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

/* --- What the file actually contains -------------------------------------
 *
 * An empty file is always complete, so a crash test on empty files asserts
 * nothing about contents at all -- it can only say the *name* resolved. The
 * payload below is what makes "old-complete or new-complete" mean something.
 *
 * It is self-describing, in the same shape as the durability markers:
 *
 *     magic, round, length, checksum, then the bytes, then round again
 *
 * The round number appears at both ends. A file assembled from the head of one
 * version and the tail of another has two different numbers in it, which is
 * visible without knowing which version was expected -- and that is the point,
 * because after a power cut nobody knows which version to expect.
 *
 * The checksum covers everything before it and everything after, so a file that
 * is short, long, or altered anywhere fails it.
 */
#define PAYLOAD_MAGIC	0x52464350u	/* "RFCP" */

/* Deliberately not a round number of blocks: a payload that ended exactly on a
 * block boundary would never exercise the partial tail, which is where a
 * length is most likely to be got wrong. */
#define PAYLOAD_BYTES	9001

static u32 fill_payload(u8 *buf, u32 round)
{
	u32 crc, i;

	kmemset(buf, 0, PAYLOAD_BYTES);

	for (i = 20; i < PAYLOAD_BYTES - 4; i++)
		buf[i] = (u8)(round * 31u + i * 7u + 1u);

	kmemcpy(buf + 0,  &(u32){ PAYLOAD_MAGIC },  4);
	kmemcpy(buf + 4,  &round,                   4);
	kmemcpy(buf + 8,  &(u32){ PAYLOAD_BYTES },  4);
	kmemcpy(buf + 16, &round,                   4);
	kmemcpy(buf + PAYLOAD_BYTES - 4, &round,    4);

	/* Over everything except the four bytes holding it. */
	crc = crc32_update(CRC32_INIT, buf, 12);
	crc = crc32_update(crc, buf + 16, PAYLOAD_BYTES - 16);
	crc = crc32_final(crc);
	kmemcpy(buf + 12, &crc, 4);

	return crc;
}

/* One transaction: make `TEMP`, with its contents. */
static enum reconfs_status make_temp(struct reconfs *fs, u8 *payload, u32 round)
{
	struct reconfs_txn *txn = reconfs_txn_begin(fs);
	u64 made = 0, dir = 0;
	enum reconfs_status st;

	if (!txn)
		return RECONFS_ERR_NOMEM;

	st = reconfs_create(txn, fs, fs->root_inode, TEMP,
			    RECONFS_TYPE_FILE, 0600, &made, &dir);
	if (st != RECONFS_OK) {
		reconfs_txn_abort(txn);
		return st;
	}

	fill_payload(payload, round);

	st = reconfs_write_named(txn, fs, dir, TEMP, payload, PAYLOAD_BYTES,
				 &dir);
	if (st != RECONFS_OK) {
		reconfs_txn_abort(txn);
		return st;
	}

	reconfs_txn_set_root(txn, dir);
	return reconfs_txn_commit(txn);
}

/* The next transaction: put it where TARGET was. */
static enum reconfs_status swap_in(struct reconfs *fs)
{
	struct reconfs_txn *txn = reconfs_txn_begin(fs);
	u64 dir = 0;
	enum reconfs_status st;

	if (!txn)
		return RECONFS_ERR_NOMEM;

	st = reconfs_rename(txn, fs, fs->root_inode, TEMP, TARGET, &dir);
	if (st != RECONFS_OK) {
		reconfs_txn_abort(txn);
		return st;
	}

	reconfs_txn_set_root(txn, dir);
	return reconfs_txn_commit(txn);
}

void reconfs_crash_run(void)
{
	const char *want = wanted_device();
	struct block_device *dev;
	struct reconfs fs;
	enum reconfs_status st;
	u64 rounds = 0;
	u64 limit = 0;
	u8 *payload;

	if (!want)
		return;

	dev = find_by_name(want);
	if (!dev) {
		kprintf("reconfs-crash: no device called %s\n", want);
		return;
	}

	/* Formats. The same refusal as the other self-test, for the same
	 * reason: a device carrying a partition table has somebody's operating
	 * system on it. */
	if (dev->slice_count) {
		kprintf("reconfs-crash: %s carries a partition table; refusing\n",
			want);
		return;
	}

	st = reconfs_format(dev, "CrashTest", 4096);
	if (st != RECONFS_OK) {
		kprintf("reconfs-crash: format: %s\n", reconfs_strerror(st));
		return;
	}

	st = reconfs_mount(dev, &fs);
	if (st != RECONFS_OK) {
		kprintf("reconfs-crash: mount: %s\n", reconfs_strerror(st));
		return;
	}

	/* The first TARGET, so there is always something to rename over. */
	{
		struct reconfs_txn *txn = reconfs_txn_begin(&fs);
		u64 made = 0, dir = 0;

		if (!txn ||
		    reconfs_create(txn, &fs, fs.root_inode, TARGET,
				   RECONFS_TYPE_FILE, 0600, &made,
				   &dir) != RECONFS_OK) {
			kputs("reconfs-crash: could not make the first target\n");
			if (txn)
				reconfs_txn_abort(txn);
			reconfs_unmount(&fs);
			return;
		}

		{
			u8 *first = kzalloc(PAYLOAD_BYTES);

			if (!first) {
				reconfs_txn_abort(txn);
				reconfs_unmount(&fs);
				return;
			}

			fill_payload(first, 0);
			if (reconfs_write_named(txn, &fs, dir, TARGET, first,
						PAYLOAD_BYTES,
						&dir) != RECONFS_OK) {
				kfree(first);
				reconfs_txn_abort(txn);
				reconfs_unmount(&fs);
				return;
			}
			kfree(first);
		}

		reconfs_txn_set_root(txn, dir);
		if (reconfs_txn_commit(txn) != RECONFS_OK) {
			kputs("reconfs-crash: could not commit the first target\n");
			reconfs_unmount(&fs);
			return;
		}
	}

	kprintf("reconfs-crash: replacing %s on %s, over and over\n",
		TARGET, want);

	/* A limit, only so the same workload can be run to a known number of
	 * replacements while something is being diagnosed. Zero, the default,
	 * means never stop -- a workload that finishes has chosen the moment the
	 * crash cannot happen after. */
	{
		const char *p = boot_info()->cmdline;
		static const char key[] = "reconfs-rounds=";

		while (p && *p) {
			const char *k = key, *q = p;

			while (*k && *q == *k) { q++; k++; }
			if (!*k) {
				limit = 0;
				while (*q >= '0' && *q <= '9')
					limit = limit * 10 + (u64)(*q++ - '0');
				break;
			}
			while (*p && *p != ' ') p++;
			while (*p == ' ') p++;
		}
	}

	payload = kzalloc(PAYLOAD_BYTES);
	if (!payload) {
		kputs("reconfs-crash: no memory for the payload\n");
		reconfs_unmount(&fs);
		return;
	}

	for (;;) {
		if (limit && rounds >= limit) {
			kprintf("reconfs-crash: stopping at %llu replacements, "
				"epoch %llu, root %llu\n",
				(unsigned long long)rounds,
				(unsigned long long)fs.super.epoch,
				(unsigned long long)fs.root_inode);
			break;
		}

		st = make_temp(&fs, payload, (u32)rounds + 1);
		if (st == RECONFS_ERR_RETRY)
			continue;
		if (st != RECONFS_OK) {
			kprintf("reconfs-crash: create: %s\n",
				reconfs_strerror(st));
			break;
		}

		st = swap_in(&fs);
		if (st == RECONFS_ERR_RETRY)
			continue;
		if (st != RECONFS_OK) {
			kprintf("reconfs-crash: rename: %s\n",
				reconfs_strerror(st));
			break;
		}

		rounds++;
		if ((rounds % 64) == 0)
			kprintf("reconfs-crash: %llu replacements\n",
				(unsigned long long)rounds);
	}

	kfree(payload);
	reconfs_unmount(&fs);
}
