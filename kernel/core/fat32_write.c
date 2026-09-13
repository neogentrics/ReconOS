/* Writing into somebody else's FAT32 volume.
 *
 * The reader in fat32.c can be wrong and cost you a confusing afternoon. This
 * can be wrong and cost you a machine that no longer boots, because the volume
 * it is for is the EFI System Partition and the file it writes is the thing
 * firmware runs. Everything here is shaped by that.
 *
 * --- The ordering is the design --------------------------------------------
 *
 * FAT has no transactions and no journal. There is no way to make two writes
 * happen together, so the only safety available is *which one goes first*. A
 * file is made real in this order and no other:
 *
 *   1. chain its clusters in both allocation tables;
 *   2. write its bytes into them;
 *   3. flush;
 *   4. write the directory entry that points at the chain;
 *   5. flush.
 *
 * Cut the power anywhere before (4) and the volume has clusters marked in use
 * that nothing refers to. That is a leak -- space lost until somebody runs a
 * repair tool -- and every file that existed still reads correctly.
 *
 * Do it the other way round, entry first, and a crash leaves a directory entry
 * pointing at clusters that were never filled. The file looks entirely valid
 * and contains whatever was there before. On an ESP that is a bootloader made
 * of somebody's deleted files, and **the firmware will run it.**
 *
 * --- Both tables, always ----------------------------------------------------
 *
 * The reader refuses a volume whose two allocation tables disagree. So a writer
 * that updated one would produce volumes its own reader rejects. That is a
 * property worth having on purpose: the two halves check each other instead of
 * sharing an assumption, and the test that proves it is the reader running
 * afterwards.
 */
#include <recon/kernel/fat32.h>

#include <recon/kernel/block.h>
#include <recon/kernel/console.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/kstring.h>

#include "fat32_internal.h"

/* --- The allocation table, written ----------------------------------------- */

static enum fat32_status entry_set(struct fat32 *fs, u32 cluster, u32 value)
{
	u32 per_sector = fs->bytes_per_sector / 4;
	u32 sector = cluster / per_sector;
	u32 within = (cluster % per_sector) * 4;
	u8 *buf;
	enum fat32_status st;
	u32 copy;

	if (!fat_cluster_ok(fs, cluster) || sector >= fs->fat_sectors)
		return FAT32_ERR_CORRUPT;

	buf = kzalloc(fs->bytes_per_sector);
	if (!buf)
		return FAT32_ERR_NOMEM;

	/* Read-modify-write, per copy. Reading the first and writing it to both
	 * would propagate a difference rather than preserving one -- and this
	 * volume has already been mounted, which is where a difference is
	 * refused, so by here they agree and each is its own source. */
	for (copy = 0; copy < fs->fat_count; copy++) {
		u64 at = fs->fat_start + (u64)copy * fs->fat_sectors + sector;

		st = fat_read_sectors(fs, at, 1, buf);
		if (st != FAT32_OK)
			goto out;

		/* The top four bits belong to the filesystem and are preserved:
		 * a writer that zeroes them is rewriting a field it does not
		 * own, on a volume it did not create. */
		fat_put32(buf + within,
			  (fat_le32(buf + within) & ~FAT32_CLUSTER_MASK) |
			  (value & FAT32_CLUSTER_MASK));

		st = fat_write_sectors(fs, at, 1, buf);
		if (st != FAT32_OK)
			goto out;
	}

	st = FAT32_OK;
out:
	kfree(buf);
	return st;
}

/* Counts free clusters a *sector* at a time, not a cluster at a time.
 *
 * The first version asked fat_entry_get for every cluster in turn, and that
 * call reads a sector from each allocation table and allocates a buffer to do
 * it. On an ordinary volume -- 129,022 clusters -- that is a quarter of a
 * million reads to answer one question, and it did not finish inside the test's
 * timeout. Same shape as the cost this project measured in its own filesystem
 * checker earlier the same day: an access pattern whose expense is invisible
 * against a file in memory and ruinous against real media.
 *
 * A sector holds 128 entries at the usual size, so this is that number times
 * cheaper. Both copies are still compared, because the reason for comparing
 * them does not go away when it becomes cheap.
 */
enum fat32_status fat32_free_clusters(struct fat32 *fs, u32 *out)
{
	u32 per_sector = fs->bytes_per_sector / 4;
	u32 last = fs->data_clusters + FAT32_CLUSTER_FIRST;
	u32 free = 0, sector;
	u8 *a, *b;
	enum fat32_status st = FAT32_OK;

	a = kzalloc(fs->bytes_per_sector);
	b = kzalloc(fs->bytes_per_sector);
	if (!a || !b) {
		kfree(a);
		kfree(b);
		return FAT32_ERR_NOMEM;
	}

	for (sector = 0; sector < fs->fat_sectors; sector++) {
		u32 first = sector * per_sector;
		u32 i;

		if (first >= last)
			break;

		st = fat_read_sectors(fs, fs->fat_start + sector, 1, a);
		if (st != FAT32_OK)
			goto out;

		if (fs->fat_count > 1) {
			st = fat_read_sectors(fs,
				fs->fat_start + fs->fat_sectors + sector, 1, b);
			if (st != FAT32_OK)
				goto out;

			for (i = 0; i < fs->bytes_per_sector; i++)
				if (a[i] != b[i]) {
					st = FAT32_ERR_FATS_DIFFER;
					goto out;
				}
		}

		for (i = 0; i < per_sector; i++) {
			u32 c = first + i;

			if (c < FAT32_CLUSTER_FIRST)
				continue;
			if (c >= last)
				break;

			if ((fat_le32(a + i * 4) & FAT32_CLUSTER_MASK) ==
			    FAT32_CLUSTER_FREE)
				free++;
		}
	}

	*out = free;
out:
	kfree(a);
	kfree(b);
	return st;
}

/* Finds `count` free clusters and chains them, returning the first.
 *
 * Nothing is written into them here. They are marked in use first so that a
 * crash between this and the data leaves clusters that are lost rather than
 * clusters that are shared -- and a lost cluster is a repair tool's problem
 * while a shared one is silent corruption of two files at once.
 */
static enum fat32_status alloc_chain(struct fat32 *fs, u32 count, u32 *first)
{
	u32 c = FAT32_CLUSTER_FIRST;
	u32 prev = 0, head = 0, made = 0;
	enum fat32_status st;

	if (!count) {
		*first = 0;
		return FAT32_OK;
	}

	while (made < count) {
		u32 v;

		if (!fat_cluster_ok(fs, c))
			return FAT32_ERR_TOO_LARGE;	/* the volume is full */

		st = fat_entry_get(fs, c, &v);
		if (st != FAT32_OK)
			return st;

		if (v != FAT32_CLUSTER_FREE) {
			c++;
			continue;
		}

		st = entry_set(fs, c, FAT32_CLUSTER_EOC | 0x7);
		if (st != FAT32_OK)
			return st;

		if (prev) {
			st = entry_set(fs, prev, c);
			if (st != FAT32_OK)
				return st;
		} else {
			head = c;
		}

		prev = c;
		made++;
		c++;
	}

	*first = head;
	return FAT32_OK;
}

static enum fat32_status free_chain(struct fat32 *fs, u32 first)
{
	u32 c = first, guard = 0;

	while (fat_cluster_ok(fs, c)) {
		u32 next;
		enum fat32_status st = fat_entry_get(fs, c, &next);

		if (st != FAT32_OK)
			return st;

		st = entry_set(fs, c, FAT32_CLUSTER_FREE);
		if (st != FAT32_OK)
			return st;

		if (++guard > fs->data_clusters)
			return FAT32_ERR_CORRUPT;

		c = next;
	}

	return FAT32_OK;
}

/* --- Names, written --------------------------------------------------------
 *
 * An 8.3 alias has to exist even for a file whose real name is long, because
 * that is the entry everything else hangs off -- the attributes, the size, the
 * first cluster, and the checksum the long-name fragments carry.
 *
 * It also has to be *unique in its directory*, so this appends `~1`, `~2` and
 * so on until nothing answers to it. A duplicate alias is not a cosmetic
 * problem: two entries with the same 8.3 name are two files a system reading
 * only aliases cannot tell apart, and it will happily delete one meaning the
 * other.
 */
static char alias_char(char c)
{
	if (c >= 'a' && c <= 'z')
		return (char)(c - 'a' + 'A');

	/* The characters 8.3 forbids, mapped to '_' rather than dropped: a name
	 * that loses characters can collide with a different name that lost
	 * different ones. */
	if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
		return c;
	if (c == '$' || c == '%' || c == '\'' || c == '-' || c == '_' ||
	    c == '@' || c == '~' || c == '`' || c == '!' || c == '(' ||
	    c == ')' || c == '{' || c == '}' || c == '^' || c == '#' ||
	    c == '&')
		return c;

	return '_';
}

static void base_and_ext(const char *name, char *base, unsigned *base_len,
			 char *ext, unsigned *ext_len)
{
	const char *dot = 0;
	const char *p;
	unsigned n = 0;

	for (p = name; *p; p++)
		if (*p == '.')
			dot = p;		/* the *last* dot */

	for (p = name; *p && (!dot || p < dot); p++)
		if (n < 8)
			base[n++] = alias_char(*p);
	*base_len = n;

	n = 0;
	if (dot)
		for (p = dot + 1; *p; p++)
			if (n < 3)
				ext[n++] = alias_char(*p);
	*ext_len = n;
}

/* Is `alias` already in use in this directory, ignoring `replacing`?
 *
 * The second argument is the whole point. Rewriting a file looks up its
 * name, finds the entry, and then has to pick an alias -- and the entry it
 * is about to replace is still there, so **the file collides with itself**
 * and gets a fresh alias every single time it is written. (KF-130)
 *
 * It cannot be fixed by deleting the old entry first. The old entry survives
 * deliberately until the new data is on the medium, because that is what
 * makes a crash mid-write leave one whole file instead of neither.
 */
/* FAT names compare without case. ASCII only, deliberately -- see the note
 * on the reader's fold(). */
static bool name_same(const char *a, const char *b)
{
	while (*a && *b) {
		char x = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
		char y = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;

		if (x != y)
			return false;
		a++;
		b++;
	}
	return *a == '\0' && *b == '\0';
}

static bool alias_taken(struct fat32 *fs, u32 dir, const char *alias,
			const char *replacing)
{
	struct fat32_entry e;
	unsigned i;

	for (i = 0; i < 4096; i++) {
		if (fat32_readdir(fs, dir, i, &e) != FAT32_OK)
			return false;

		/* The entry this write is replacing is not a collision with
		 * itself. Matched on the name being written rather than on a
		 * cluster number, because an empty file's first cluster is
		 * zero and every empty file would look like the same one. */
		if (replacing && (name_same(e.name, replacing) ||
				  name_same(e.alias, replacing)))
			continue;

		/* Compared case-insensitively against the *alias* only: two
		 * files may share a long name's spelling in different cases and
		 * still be one file, but two identical aliases are two entries
		 * nothing can tell apart. */
		{
			const char *a = e.alias, *b = alias;

			while (*a && *b) {
				char x = (*a >= 'a' && *a <= 'z') ?
					 (char)(*a - 32) : *a;
				char y = (*b >= 'a' && *b <= 'z') ?
					 (char)(*b - 32) : *b;

				if (x != y)
					break;
				a++;
				b++;
			}
			if (!*a && !*b)
				return true;
		}
	}

	return false;
}

static void pack11(const char *base, unsigned bl, const char *ext,
		   unsigned el, u8 *out11)
{
	unsigned i;

	for (i = 0; i < 11; i++)
		out11[i] = ' ';
	for (i = 0; i < bl && i < 8; i++)
		out11[i] = (u8)base[i];
	for (i = 0; i < el && i < 3; i++)
		out11[8 + i] = (u8)ext[i];
}

static void unpack11(const u8 *in11, char *out)
{
	unsigned i, o = 0;

	for (i = 0; i < 8 && in11[i] != ' '; i++)
		out[o++] = (char)in11[i];
	if (in11[8] != ' ') {
		out[o++] = '.';
		for (i = 8; i < 11 && in11[i] != ' '; i++)
			out[o++] = (char)in11[i];
	}
	out[o] = '\0';
}

static enum fat32_status make_alias(struct fat32 *fs, u32 dir, const char *name,
				    const char *replacing, u8 *out11)
{
	char base[8], ext[3], candidate[13];
	unsigned bl, el, n;

	base_and_ext(name, base, &bl, ext, &el);
	if (!bl)
		return FAT32_ERR_NAME;

	pack11(base, bl, ext, el, out11);
	unpack11(out11, candidate);
	if (!alias_taken(fs, dir, candidate, replacing))
		return FAT32_OK;

	for (n = 1; n < 1000; n++) {
		char tail[5];
		unsigned tl = 0, keep;
		unsigned v = n, digits = 0, d;

		for (d = v; d; d /= 10)
			digits++;
		if (!digits)
			digits = 1;

		tail[tl++] = '~';
		{
			char tmp[4];
			unsigned k = 0;

			v = n;
			do {
				tmp[k++] = (char)('0' + v % 10);
				v /= 10;
			} while (v);
			while (k)
				tail[tl++] = tmp[--k];
		}

		keep = bl;
		if (keep + tl > 8)
			keep = 8 - tl;

		{
			char merged[8];
			unsigned i;

			for (i = 0; i < keep; i++)
				merged[i] = base[i];
			for (i = 0; i < tl; i++)
				merged[keep + i] = tail[i];

			pack11(merged, keep + tl, ext, el, out11);
		}

		unpack11(out11, candidate);
		if (!alias_taken(fs, dir, candidate, replacing))
			return FAT32_OK;
	}

	/* A thousand files whose names collapse to the same eight characters is
	 * not a case worth guessing at. Refused rather than reusing one. */
	return FAT32_ERR_NAME;
}

/* --- Directory slots -------------------------------------------------------- */

/* How many 32-byte records a name needs: one for the entry, plus one per
 * thirteen characters of long name -- and none at all if the name *is* its own
 * alias, since then there is nothing a long name would add. */
static unsigned records_for(const char *name, const u8 *alias11)
{
	char alias[13];
	const char *a, *b;
	unsigned n = 0;

	unpack11(alias11, alias);

	for (a = alias, b = name; *a && *b; a++, b++)
		if (*a != *b)
			break;
	if (!*a && !*b)
		return 1;

	while (name[n])
		n++;

	return 1 + (n + 12) / 13;
}

/* Finds `need` consecutive free records in a directory, growing it if there is
 * no run that long. Returns the cluster and byte offset of the first. */
static enum fat32_status find_slots(struct fat32 *fs, u32 dir, unsigned need,
				    u32 *out_cluster, u32 *out_offset)
{
	u32 cluster = dir ? dir : fs->root_cluster;
	u32 run_cluster = 0, run_offset = 0;
	unsigned run = 0, guard = 0;
	u8 *buf = kzalloc(fs->bytes_per_cluster);
	enum fat32_status st = FAT32_OK;

	if (!buf)
		return FAT32_ERR_NOMEM;

	for (;;) {
		u32 off;

		if (!fat_cluster_ok(fs, cluster)) {
			st = FAT32_ERR_CORRUPT;
			break;
		}

		st = fat_read_sectors(fs, fat_cluster_to_sector(fs, cluster),
				      fs->sectors_per_cluster, buf);
		if (st != FAT32_OK)
			break;

		for (off = 0; off + FAT_DIR_SIZE <= fs->bytes_per_cluster;
		     off += FAT_DIR_SIZE) {
			u8 first = buf[off];

			if (first == FAT_ENTRY_FREE ||
			    first == FAT_ENTRY_DELETED) {
				if (!run) {
					run_cluster = cluster;
					run_offset = off;
				}
				if (++run == need) {
					*out_cluster = run_cluster;
					*out_offset = run_offset;
					kfree(buf);
					return FAT32_OK;
				}
			} else {
				run = 0;
			}
		}

		{
			u32 next;

			st = fat_entry_get(fs, cluster, &next);
			if (st != FAT32_OK)
				break;

			if (next >= FAT32_CLUSTER_EOC) {
				/* Out of room: one more cluster, zeroed, and
				 * chained on. Zeroed matters -- an unzeroed
				 * cluster of a directory is read as entries,
				 * and whatever used to be there becomes files.
				 */
				u32 grown;

				st = alloc_chain(fs, 1, &grown);
				if (st != FAT32_OK)
					break;

				kmemset(buf, 0, fs->bytes_per_cluster);
				st = fat_write_sectors(fs,
					fat_cluster_to_sector(fs, grown),
					fs->sectors_per_cluster, buf);
				if (st != FAT32_OK)
					break;

				st = entry_set(fs, cluster, grown);
				if (st != FAT32_OK)
					break;

				next = grown;
				run = 0;
			}

			if (++guard > fs->data_clusters) {
				st = FAT32_ERR_CORRUPT;
				break;
			}

			cluster = next;
		}
	}

	kfree(buf);
	return st;
}

/* --- Making the entry ------------------------------------------------------- */

static const struct { u8 off, count; } lfn_runs[] = {
	{ 1, 5 }, { 14, 6 }, { 28, 2 }
};

/* UTF-8 to UTF-16, refusing rather than approximating. A name that cannot be
 * encoded is a name this writer must not claim to have written. */
static bool utf8_to_utf16(const char *in, u16 *out, unsigned max, unsigned *n)
{
	unsigned o = 0;

	while (*in) {
		u32 c = (u8)*in++;
		unsigned extra;

		if (c < 0x80) {
			extra = 0;
		} else if ((c & 0xE0) == 0xC0) {
			c &= 0x1F;
			extra = 1;
		} else if ((c & 0xF0) == 0xE0) {
			c &= 0x0F;
			extra = 2;
		} else if ((c & 0xF8) == 0xF0) {
			c &= 0x07;
			extra = 3;
		} else {
			return false;
		}

		while (extra--) {
			if ((*in & 0xC0) != 0x80)
				return false;
			c = (c << 6) | (u32)(*in++ & 0x3F);
		}

		if (c >= 0x10000) {
			if (o + 2 > max)
				return false;
			c -= 0x10000;
			out[o++] = (u16)(0xD800 + (c >> 10));
			out[o++] = (u16)(0xDC00 + (c & 0x3FF));
		} else {
			if (o + 1 > max)
				return false;
			out[o++] = (u16)c;
		}
	}

	*n = o;
	return true;
}

static enum fat32_status write_entry(struct fat32 *fs, u32 cluster, u32 offset,
				     unsigned records, const char *name,
				     const u8 *alias11, u32 first_cluster,
				     u32 size, u8 attr)
{
	u8 *buf = kzalloc(fs->bytes_per_cluster);
	u16 units[260];
	unsigned n = 0, i;
	u8 sum = fat_alias_checksum(alias11);
	enum fat32_status st;

	if (!buf)
		return FAT32_ERR_NOMEM;

	if (records > 1 && !utf8_to_utf16(name, units, 255, &n)) {
		kfree(buf);
		return FAT32_ERR_NAME;
	}

	st = fat_read_sectors(fs, fat_cluster_to_sector(fs, cluster),
			      fs->sectors_per_cluster, buf);
	if (st != FAT32_OK)
		goto out;

	/* Fragments first, in reverse: the one holding the *start* of the name
	 * is written last on disk and carries bit 6. Getting this backwards
	 * produces a name that reads as its own reversal in thirteen-character
	 * chunks, which is the sort of thing that looks like an encoding bug. */
	for (i = 1; i < records; i++) {
		unsigned seq = records - i;		/* records-1 .. 1 */
		u8 *rec = buf + offset + (i - 1) * FAT_DIR_SIZE;
		unsigned base = (seq - 1) * 13;
		unsigned r, k = 0;

		kmemset(rec, 0, FAT_DIR_SIZE);
		rec[0] = (u8)(seq | (seq == records - 1 ? 0x40 : 0));
		rec[FAT_DIR_ATTR] = FAT_ATTR_LONG_NAME;
		rec[13] = sum;

		for (r = 0; r < 3; r++) {
			unsigned j;

			for (j = 0; j < lfn_runs[r].count; j++, k++) {
				u16 v;

				if (base + k < n)
					v = units[base + k];
				else if (base + k == n)
					v = 0x0000;
				else
					v = 0xFFFF;

				fat_put16(rec + lfn_runs[r].off + j * 2, v);
			}
		}
	}

	{
		u8 *rec = buf + offset + (records - 1) * FAT_DIR_SIZE;

		kmemset(rec, 0, FAT_DIR_SIZE);
		kmemcpy(rec + FAT_DIR_NAME, alias11, 11);
		rec[FAT_DIR_ATTR] = attr;
		fat_put16(rec + FAT_DIR_FST_CLUS_HI,
			  (u16)((first_cluster >> 16) & 0xFFFF));
		fat_put16(rec + FAT_DIR_FST_CLUS_LO,
			  (u16)(first_cluster & 0xFFFF));
		fat_put32(rec + FAT_DIR_FILE_SIZE, size);

		/* No timestamp. There is no wall clock this code can trust to
		 * be set -- the kernel reads one at boot and an installer may
		 * be running on a machine whose battery died in 2009 -- and a
		 * confidently wrong date is worse than an obviously absent one.
		 * Zero is what FAT uses for "not recorded". */
	}

	st = fat_write_sectors(fs, fat_cluster_to_sector(fs, cluster),
			       fs->sectors_per_cluster, buf);
out:
	kfree(buf);
	return st;
}

/* --- The public write ------------------------------------------------------- */

static enum fat32_status write_data(struct fat32 *fs, u32 first,
				    const void *data, u32 len)
{
	const u8 *src = data;
	u32 cluster = first, left = len, guard = 0;
	u8 *buf = kzalloc(fs->bytes_per_cluster);
	enum fat32_status st = FAT32_OK;

	if (!buf)
		return FAT32_ERR_NOMEM;

	while (left) {
		u32 take = left < fs->bytes_per_cluster ? left
						       : fs->bytes_per_cluster;

		if (!fat_cluster_ok(fs, cluster)) {
			st = FAT32_ERR_CORRUPT;
			break;
		}

		/* The tail of the last cluster is zeroed rather than left as
		 * whatever was there. Nothing reads past the file's length, but
		 * "nothing reads it" is a claim about every reader that will
		 * ever exist, and the bytes are somebody's deleted file. */
		kmemset(buf, 0, fs->bytes_per_cluster);
		kmemcpy(buf, src, take);

		st = fat_write_sectors(fs, fat_cluster_to_sector(fs, cluster),
				       fs->sectors_per_cluster, buf);
		if (st != FAT32_OK)
			break;

		src += take;
		left -= take;

		if (!left)
			break;

		st = fat_entry_get(fs, cluster, &cluster);
		if (st != FAT32_OK)
			break;

		if (++guard > fs->data_clusters) {
			st = FAT32_ERR_CORRUPT;
			break;
		}
	}

	kfree(buf);
	return st;
}

/* Marks a name's records deleted: the entry, and every long-name fragment in
 * front of it.
 *
 * The fragments matter. FAT deletes an entry by writing 0xE5 over the first
 * byte, and a reader gathers fragments as it goes and attaches them to the next
 * real entry it meets. Leave them behind and the *next* file in that directory
 * silently acquires the deleted file's name -- which is exactly the orphan case
 * the reader's checksum test defends against, arriving from our own writer.
 *
 * The bytes are not erased, only marked. That is what FAT means by deleting,
 * and pretending otherwise would be a promise this cannot keep on a medium
 * that remaps its own blocks.
 */
static enum fat32_status mark_deleted(struct fat32 *fs, u32 dir,
				      const char *name)
{
	struct fat32_entry want;
	u32 cluster = dir ? dir : fs->root_cluster;
	u8 *buf;
	enum fat32_status st;
	unsigned guard = 0;
	u32 run_start = 0;
	bool run_open = false;

	st = fat32_lookup(fs, dir, name, &want);
	if (st != FAT32_OK)
		return st;

	buf = kzalloc(fs->bytes_per_cluster);
	if (!buf)
		return FAT32_ERR_NOMEM;

	for (;;) {
		u32 off;
		bool dirty = false;

		if (!fat_cluster_ok(fs, cluster)) {
			st = FAT32_ERR_CORRUPT;
			break;
		}

		st = fat_read_sectors(fs, fat_cluster_to_sector(fs, cluster),
				      fs->sectors_per_cluster, buf);
		if (st != FAT32_OK)
			break;

		for (off = 0; off + FAT_DIR_SIZE <= fs->bytes_per_cluster;
		     off += FAT_DIR_SIZE) {
			u8 *rec = buf + off;

			if (rec[0] == FAT_ENTRY_FREE) {
				st = FAT32_ERR_NOT_FOUND;
				goto done;
			}

			if (rec[0] == FAT_ENTRY_DELETED) {
				run_open = false;
				continue;
			}

			if ((rec[FAT_DIR_ATTR] & FAT_ATTR_LONG_MASK) ==
			    FAT_ATTR_LONG_NAME) {
				if (!run_open) {
					run_open = true;
					run_start = off;
				}
				continue;
			}

			/* A real entry. Is it the one? */
			{
				u32 fc = (((u32)fat_le16(rec + FAT_DIR_FST_CLUS_HI))
					  << 16 |
					  fat_le16(rec + FAT_DIR_FST_CLUS_LO)) &
					 FAT32_CLUSTER_MASK;

				if (fc == want.first_cluster &&
				    fat_le32(rec + FAT_DIR_FILE_SIZE) ==
					    want.size) {
					u32 k = run_open ? run_start : off;

					while (k <= off) {
						buf[k] = FAT_ENTRY_DELETED;
						k += FAT_DIR_SIZE;
					}
					dirty = true;
					st = FAT32_OK;
					goto write_and_done;
				}
			}

			run_open = false;
		}

		if (dirty) {
			st = fat_write_sectors(fs,
				fat_cluster_to_sector(fs, cluster),
				fs->sectors_per_cluster, buf);
			if (st != FAT32_OK)
				break;
		}

		st = fat_entry_get(fs, cluster, &cluster);
		if (st != FAT32_OK)
			break;
		if (cluster >= FAT32_CLUSTER_EOC) {
			st = FAT32_ERR_NOT_FOUND;
			break;
		}
		if (++guard > fs->data_clusters) {
			st = FAT32_ERR_CORRUPT;
			break;
		}

		/* A run of fragments does not span clusters in any directory
		 * this writer creates, and a run that did would be somebody
		 * else's layout -- so it is dropped at the boundary rather than
		 * assumed to continue. */
		run_open = false;
	}

	goto done;

write_and_done:
	st = fat_write_sectors(fs, fat_cluster_to_sector(fs, cluster),
			       fs->sectors_per_cluster, buf);

done:
	kfree(buf);
	return st;
}

static enum fat32_status create(struct fat32 *fs, u32 dir, const char *name,
				const void *data, u32 len, u8 attr,
				u32 *out_cluster)
{
	struct fat32_entry existing;
	bool replacing = false;
	u32 old_chain = 0;
	u8 alias11[11];
	unsigned records;
	u32 clusters, first = 0, slot_cluster, slot_offset;
	enum fat32_status st;

	if (!name || !*name)
		return FAT32_ERR_NAME;

	if (fat32_lookup(fs, dir, name, &existing) == FAT32_OK) {
		if (existing.is_dir != ((attr & FAT_ATTR_DIRECTORY) != 0))
			return FAT32_ERR_NAME;
		replacing = true;
		old_chain = existing.first_cluster;
	}

		/* Replacing this very name, so it must not count against itself. */
	st = make_alias(fs, dir, name, replacing ? name : 0, alias11);
	if (st != FAT32_OK)
		return st;

	records = records_for(name, alias11);

	clusters = (len + fs->bytes_per_cluster - 1) / fs->bytes_per_cluster;
	if (attr & FAT_ATTR_DIRECTORY)
		clusters = 1;

	/* (1) and (2): the chain, then the bytes. Nothing refers to either yet,
	 * so a crash here loses space and no data. */
	st = alloc_chain(fs, clusters, &first);
	if (st != FAT32_OK)
		return st;

	if (attr & FAT_ATTR_DIRECTORY) {
		u8 *buf = kzalloc(fs->bytes_per_cluster);

		if (!buf) {
			free_chain(fs, first);
			return FAT32_ERR_NOMEM;
		}

		/* `.` and `..`, which every directory but the root must have.
		 * `..` pointing at the root is written as cluster 0, which is
		 * what the specification says and not what the root's actual
		 * cluster number is. */
		kmemset(buf, 0, fs->bytes_per_cluster);
		kmemset(buf, ' ', 11);
		buf[0] = '.';
		buf[FAT_DIR_ATTR] = FAT_ATTR_DIRECTORY;
		fat_put16(buf + FAT_DIR_FST_CLUS_HI, (u16)(first >> 16));
		fat_put16(buf + FAT_DIR_FST_CLUS_LO, (u16)(first & 0xFFFF));

		kmemset(buf + 32, ' ', 11);
		buf[32] = '.';
		buf[33] = '.';
		buf[32 + FAT_DIR_ATTR] = FAT_ATTR_DIRECTORY;
		{
			u32 parent = (dir && dir != fs->root_cluster) ? dir : 0;

			fat_put16(buf + 32 + FAT_DIR_FST_CLUS_HI,
				  (u16)(parent >> 16));
			fat_put16(buf + 32 + FAT_DIR_FST_CLUS_LO,
				  (u16)(parent & 0xFFFF));
		}

		st = fat_write_sectors(fs, fat_cluster_to_sector(fs, first),
				       fs->sectors_per_cluster, buf);
		kfree(buf);
	} else if (len) {
		st = write_data(fs, first, data, len);
	}

	if (st != FAT32_OK) {
		free_chain(fs, first);
		return st;
	}

	/* (3) flush, so the bytes are on the medium before anything points at
	 * them. Without this the ordering above is a statement about the order
	 * calls were made in, not about the order they reached the disk. */
	if (block_flush(fs->dev) != BLOCK_OK) {
		free_chain(fs, first);
		return FAT32_ERR_IO;
	}

	/* If a name is being replaced, its old record goes first so the
	 * directory never holds two entries answering to the same name. */
	if (replacing) {
		st = mark_deleted(fs, dir, name);
		if (st != FAT32_OK) {
			free_chain(fs, first);
			return st;
		}
	}

	st = find_slots(fs, dir, records, &slot_cluster, &slot_offset);
	if (st != FAT32_OK) {
		free_chain(fs, first);
		return st;
	}

	/* (4) */
	st = write_entry(fs, slot_cluster, slot_offset, records, name, alias11,
			 first, (attr & FAT_ATTR_DIRECTORY) ? 0 : len, attr);
	if (st != FAT32_OK) {
		free_chain(fs, first);
		return st;
	}

	/* (5) */
	if (block_flush(fs->dev) != BLOCK_OK)
		return FAT32_ERR_IO;

	/* Only now is the old content unreachable, so only now is it safe to
	 * give its space back. A crash before this leaves the old chain
	 * allocated and unreferenced -- a leak, not a loss. */
	if (replacing && old_chain)
		free_chain(fs, old_chain);

	if (out_cluster)
		*out_cluster = first;

	return block_flush(fs->dev) == BLOCK_OK ? FAT32_OK : FAT32_ERR_IO;
}

enum fat32_status fat32_write_named(struct fat32 *fs, u32 dir_cluster,
				    const char *name, const void *data,
				    u32 len)
{
	return create(fs, dir_cluster, name, data, len, FAT_ATTR_ARCHIVE, 0);
}

enum fat32_status fat32_mkdir(struct fat32 *fs, u32 dir_cluster,
			      const char *name, u32 *out_cluster)
{
	return create(fs, dir_cluster, name, 0, 0, FAT_ATTR_DIRECTORY,
		      out_cluster);
}

enum fat32_status fat32_mkpath(struct fat32 *fs, const char *path,
			       u32 *out_cluster)
{
	u32 dir = fs->root_cluster;
	char part[FAT_NAME_MAX];

	while (*path == '/' || *path == '\\')
		path++;

	while (*path) {
		unsigned n = 0;
		struct fat32_entry e;

		while (path[n] && path[n] != '/' && path[n] != '\\') {
			if (n + 1 >= sizeof(part))
				return FAT32_ERR_NAME;
			part[n] = path[n];
			n++;
		}
		part[n] = '\0';
		path += n;
		while (*path == '/' || *path == '\\')
			path++;

		if (fat32_lookup(fs, dir, part, &e) == FAT32_OK) {
			if (!e.is_dir)
				return FAT32_ERR_NAME;
			dir = e.first_cluster;
		} else {
			enum fat32_status st = fat32_mkdir(fs, dir, part, &dir);

			if (st != FAT32_OK)
				return st;
		}
	}

	if (out_cluster)
		*out_cluster = dir;

	return FAT32_OK;
}
