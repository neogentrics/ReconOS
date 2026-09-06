/* Reading a FAT32 volume this kernel did not write, and proving it.
 *
 * The whole point of a foreign filesystem is that somebody else made it, so a
 * test that formats a volume with our own code and reads it back proves
 * nothing: a writer and a reader built from the same misunderstanding agree
 * perfectly. This reads a volume built by `mkfs.vfat` and filled by `mcopy` --
 * neither of which shares a line with this kernel -- and checks against what
 * those tools were told to put there.
 *
 * Three things are checked that a weaker test would miss:
 *
 *   1. **File contents, byte for byte**, against a pattern the kernel
 *      recomputes rather than against a length. A reader that returns the right
 *      number of wrong bytes passes a length check.
 *   2. **The long name**, not the 8.3 alias. Our own ESP holds
 *      `kernel-x86_64.elf`, whose alias is `KERNEL~1.ELF` -- an installer that
 *      could only see aliases could not tell one kernel from another, and the
 *      alias is not even stable, since it depends on what else was created and
 *      in what order.
 *   3. **A file spanning several clusters**, because a file that fits in one
 *      never follows a chain, and following the chain is the part that goes
 *      wrong.
 *
 * Runs only when `fat32=<device>` names one on the command line. Reads only.
 */
#include <recon/kernel/fat32.h>

#include <recon/kernel/block.h>
#include <recon/kernel/boot.h>
#include <recon/kernel/console.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/kstring.h>

/* The pattern scripts/make-fat-fixture.sh writes into PATTERN.BIN, recomputed
 * here rather than stored, so the two cannot drift into agreement. */
static u8 pattern_byte(u32 i)
{
	return (u8)((i * 31u + 7u) & 0xFFu);
}

#define PATTERN_NAME  "pattern.bin"
#define PATTERN_SIZE  (140u * 1024u)	/* several clusters at any sane size */
#define LONG_NAME     "a-long-name-for-testing.txt"
#define LONG_CONTENT  "reconos"

static const char *wanted_device(void)
{
	static char name[BLOCK_NAME_MAX];
	const char *p = boot_info()->cmdline;
	static const char key[] = "fat32=";

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

static bool check_pattern(struct fat32 *fs)
{
	struct fat32_entry e;
	enum fat32_status st;
	u8 *buf;
	u32 got = 0, i;
	bool ok = true;

	st = fat32_walk(fs, PATTERN_NAME, &e);
	if (st != FAT32_OK) {
		kprintf("  a file across clusters : FAIL (%s: %s)\n",
			PATTERN_NAME, fat32_strerror(st));
		return false;
	}

	if (e.size != PATTERN_SIZE) {
		kprintf("  a file across clusters : FAIL (%u bytes, expected "
			"%u)\n", e.size, PATTERN_SIZE);
		return false;
	}

	/* One byte short on purpose first: a reader that truncates instead of
	 * refusing would hand back a short file with a success status, and the
	 * caller could not tell. */
	buf = kzalloc(PATTERN_SIZE);
	if (!buf) {
		kputs("  a file across clusters : FAIL (out of memory)\n");
		return false;
	}

	st = fat32_read_file(fs, &e, buf, PATTERN_SIZE - 1, &got);
	if (st != FAT32_ERR_TOO_LARGE) {
		kprintf("  a file across clusters : FAIL (a buffer one byte "
			"short was accepted: %s)\n", fat32_strerror(st));
		kfree(buf);
		return false;
	}

	kmemset(buf, 0, PATTERN_SIZE);
	st = fat32_read_file(fs, &e, buf, PATTERN_SIZE, &got);
	if (st != FAT32_OK || got != PATTERN_SIZE) {
		kprintf("  a file across clusters : FAIL (%s, %u of %u)\n",
			fat32_strerror(st), got, PATTERN_SIZE);
		kfree(buf);
		return false;
	}

	for (i = 0; i < PATTERN_SIZE; i++) {
		if (buf[i] != pattern_byte(i)) {
			kprintf("  a file across clusters : FAIL (byte %u is "
				"0x%02x, expected 0x%02x)\n",
				i, buf[i], pattern_byte(i));
			ok = false;
			break;
		}
	}

	kfree(buf);

	if (ok)
		kprintf("  a file across clusters : pass (%u bytes, every one "
			"of them, over %u clusters)\n",
			PATTERN_SIZE,
			(PATTERN_SIZE + fs->bytes_per_cluster - 1) /
				fs->bytes_per_cluster);
	return ok;
}

static bool check_long_name(struct fat32 *fs)
{
	struct fat32_entry e;
	enum fat32_status st;
	char buf[64];
	u32 got = 0;

	st = fat32_walk(fs, LONG_NAME, &e);
	if (st != FAT32_OK) {
		kprintf("  a name longer than 8.3 : FAIL (%s)\n",
			fat32_strerror(st));
		return false;
	}

	/* Found by its long name, so the fragments were gathered and their
	 * checksum matched the entry they belong to. */
	if (!kstrlen(e.alias) || kstrlen(e.name) <= kstrlen(e.alias)) {
		kprintf("  a name longer than 8.3 : FAIL (name '%s' alias "
			"'%s' -- the alias was returned as the name)\n",
			e.name, e.alias);
		return false;
	}

	st = fat32_read_file(fs, &e, buf, sizeof(buf), &got);
	if (st != FAT32_OK || got != kstrlen(LONG_CONTENT)) {
		kprintf("  a name longer than 8.3 : FAIL (contents: %s, %u "
			"bytes)\n", fat32_strerror(st), got);
		return false;
	}

	kprintf("  a name longer than 8.3 : pass ('%s', alias '%s')\n",
		e.name, e.alias);
	return true;
}

static bool check_directory(struct fat32 *fs)
{
	struct fat32_entry e;
	enum fat32_status st;
	unsigned i, found = 0;

	/* A path two deep, which is what an ESP is. */
	st = fat32_walk(fs, "/deeper/inside.txt", &e);
	if (st != FAT32_OK) {
		kprintf("  a path two deep        : FAIL (%s)\n",
			fat32_strerror(st));
		return false;
	}

	for (i = 0; i < 64; i++) {
		struct fat32_entry d;

		if (fat32_readdir(fs, 0, i, &d) != FAT32_OK)
			break;
		found++;
	}

	if (!found) {
		kputs("  a path two deep        : FAIL (the root listed "
		      "nothing)\n");
		return false;
	}

	kprintf("  a path two deep        : pass (%u entries in the root, "
		"and /deeper/inside.txt is %u bytes)\n", found, e.size);
	return true;
}

/* A name that is not there must be refused rather than answered with whatever
 * the walk happened to be looking at. */
static bool check_absent(struct fat32 *fs)
{
	struct fat32_entry e;
	enum fat32_status st = fat32_walk(fs, "/no-such-file.txt", &e);

	if (st != FAT32_ERR_NOT_FOUND) {
		kprintf("  a name that is absent  : FAIL (%s)\n",
			fat32_strerror(st));
		return false;
	}

	kputs("  a name that is absent  : pass (refused)\n");
	return true;
}

/* What is actually on this volume, whatever it is.
 *
 * A reader is far more useful when it can be pointed at a disk somebody
 * hands you and asked what is on it -- and that is also the honest way to
 * demonstrate it. The fixture proves correctness against content chosen to
 * be awkward; this proves it against content nobody arranged.
 */
static void list_dir(struct fat32 *fs, u32 cluster)
{
	struct fat32_entry e;
	unsigned i;

	for (i = 0; i < 32; i++) {
		if (fat32_readdir(fs, cluster, i, &e) != FAT32_OK)
			break;

		if (e.is_dir)
			kprintf("    %-30s <dir>\n", e.name);
		else
			kprintf("    %-30s %u bytes\n", e.name, e.size);
	}

	if (!i)
		kputs("    (empty)\n");
}

static void list_root(struct fat32 *fs)
{
	list_dir(fs, 0);
}

void fat32_run(void)
{
	const char *want = wanted_device();
	struct block_device *dev;
	struct fat32 fs;
	struct fat32_entry probe;
	enum fat32_status st;
	unsigned good = 0;

	if (!want)
		return;

	dev = find_by_name(want);
	if (!dev) {
		kprintf("\nfat32: no device called %s\n", want);
		return;
	}

	kprintf("\nfat32: reading %s, which this kernel did not write\n", want);

	st = fat32_mount(dev, &fs);
	if (st != FAT32_OK) {
		kprintf("  mount              : FAIL (%s)\n",
			fat32_strerror(st));
		return;
	}

	kprintf("  mount              : pass (%u-byte sectors, %u per "
		"cluster, %u data clusters, %u tables)\n",
		fs.bytes_per_sector, fs.sectors_per_cluster,
		fs.data_clusters, fs.fat_count);

	list_root(&fs);

	/* The fixture checks only mean anything on the fixture. Pointed at a
	 * real volume -- an actual EFI System Partition, say -- every one would
	 * fail for the honest reason that those files are not there, and a wall
	 * of FAILs about a volume that is fine teaches whoever reads the output
	 * to stop reading it. So the volume is asked whether it is the fixture,
	 * and a real one gets the listing above as its whole report. */
	if (fat32_walk(&fs, PATTERN_NAME, &probe) != FAT32_OK) {
		/* An ESP is the volume this reader exists for, so if this looks
		 * like one, walk into it. Two levels deep, on a volume built by
		 * somebody else, is the thing an installer has to do. */
		if (fat32_walk(&fs, "/EFI/BOOT", &probe) == FAT32_OK &&
		    probe.is_dir) {
			kputs("    EFI/BOOT holds:\n");
			list_dir(&fs, probe.first_cluster);
		}

		kputs("  not the test fixture, so the listing above is the whole "
		      "report\n");
		return;
	}

	good += check_directory(&fs) ? 1 : 0;
	good += check_long_name(&fs) ? 1 : 0;
	good += check_pattern(&fs) ? 1 : 0;
	good += check_absent(&fs) ? 1 : 0;

	kprintf("  %u of 4 checks passed on somebody else's filesystem\n", good);
}

/* --- Writing ---------------------------------------------------------------
 *
 * Everything written here is read back by `mdir` and `mcopy` afterwards, by
 * scripts/fat32-writes-foreign.sh. That is the point: our reader agreeing with
 * our writer would prove only that they share an opinion.
 *
 * Four files, each chosen for a thing that goes wrong:
 *
 *   BOOTX64.EFI   the real case -- an 8.3 name in a directory two deep, which
 *                 is exactly what an installer writes and where.
 *   long name     a name needing fragments, so the alias has to be generated
 *                 and the checksum has to tie them together.
 *   big.bin       larger than one cluster, so the chain has to be built and
 *                 followed. 140 KiB, the same pattern the read test uses.
 *   twice.txt     written, then written again with different contents, so the
 *                 replace path runs: old chain freed only after the new entry
 *                 is in place.
 */
static const char *write_device(void)
{
	static char name[BLOCK_NAME_MAX];
	const char *p = boot_info()->cmdline;
	static const char key[] = "fat32-write=";

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

#define WRITE_BIG_SIZE (140u * 1024u)

static bool wrote(const char *what, enum fat32_status st)
{
	if (st == FAT32_OK) {
		kprintf("  %-26s : wrote\n", what);
		return true;
	}

	kprintf("  %-26s : FAIL (%s)\n", what, fat32_strerror(st));
	return false;
}

void fat32_write_run(void)
{
	const char *want = write_device();
	struct block_device *dev;
	struct fat32 fs;
	enum fat32_status st;
	u32 boot_dir = 0, before = 0, after = 0;
	u8 *big;
	unsigned ok = 0;

	if (!want)
		return;

	dev = find_by_name(want);
	if (!dev) {
		kprintf("\nfat32: no device called %s\n", want);
		return;
	}

	kprintf("\nfat32: writing into %s, for somebody else's tools to read\n",
		want);

	st = fat32_mount(dev, &fs);
	if (st != FAT32_OK) {
		kprintf("  mount                      : FAIL (%s)\n",
			fat32_strerror(st));
		return;
	}

	if (fat32_free_clusters(&fs, &before) != FAT32_OK)
		before = 0;

	/* Two directories deep in one call, which is what \EFI\BOOT is. */
	st = fat32_mkpath(&fs, "/EFI/BOOT", &boot_dir);
	ok += wrote("EFI/BOOT, made two deep", st) ? 1 : 0;

	if (st == FAT32_OK)
		ok += wrote("BOOTX64.EFI in it",
			    fat32_write_named(&fs, boot_dir, "BOOTX64.EFI",
					      "not really a loader", 19)) ? 1 : 0;

	ok += wrote("a name needing fragments",
		    fat32_write_named(&fs, 0, "a-long-name-written-by-us.txt",
				      "reconos wrote this", 18)) ? 1 : 0;

	big = kzalloc(WRITE_BIG_SIZE);
	if (big) {
		u32 i;

		for (i = 0; i < WRITE_BIG_SIZE; i++)
			big[i] = pattern_byte(i);

		ok += wrote("140 KiB across clusters",
			    fat32_write_named(&fs, 0, "big.bin", big,
					      WRITE_BIG_SIZE)) ? 1 : 0;
		kfree(big);
	}

	/* Replace: the second write must leave exactly one file, with the second
	 * contents, and must not leak the first one's clusters. */
	st = fat32_write_named(&fs, 0, "twice.txt", "first", 5);
	if (st == FAT32_OK)
		st = fat32_write_named(&fs, 0, "twice.txt", "second-and-longer",
				       17);
	ok += wrote("a name written twice", st) ? 1 : 0;

	if (fat32_free_clusters(&fs, &after) == FAT32_OK && before)
		kprintf("  free clusters              : %u before, %u after "
			"(%u used)\n", before, after, before - after);

	kprintf("  %u of 5 writes reported success\n", ok);
}
