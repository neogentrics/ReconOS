/* Reading FAT32 volumes this kernel did not write.
 *
 * See fat32.h for why this exists and what it refuses. The short version: the
 * UEFI System Partition is FAT32 by specification, that is where our bootloader
 * has to go, and an installer that cannot put a file there cannot make a
 * machine boot.
 *
 * Every multi-byte field on disk is little-endian regardless of the machine, so
 * every one of them goes through the readers below rather than through a cast.
 * A struct with a designed layout would be a promise the compiler makes; this
 * is a promise somebody else's formatter made twenty years ago.
 */
#include <recon/kernel/block.h>
#include <recon/kernel/console.h>
#include <recon/kernel/fat32.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/kstring.h>

#include "fat32_internal.h"

u16 fat_le16(const u8 *p)
{
	return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

u32 fat_le32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
	       ((u32)p[3] << 24);
}

const char *fat32_strerror(enum fat32_status s)
{
	switch (s) {
	case FAT32_OK:              return "ok";
	case FAT32_ERR_IO:          return "the device could not be read";
	case FAT32_ERR_NOT_FAT:     return "this is not a FAT volume";
	case FAT32_ERR_WRONG_TYPE:  return "this is FAT12 or FAT16, not FAT32";
	case FAT32_ERR_FATS_DIFFER: return "the two allocation tables disagree";
	case FAT32_ERR_NOT_FOUND:   return "no such name";
	case FAT32_ERR_NAME:        return "the name is empty or too long";
	case FAT32_ERR_TOO_LARGE:   return "the file is larger than the buffer";
	case FAT32_ERR_CORRUPT:     return "a cluster chain that cannot be true";
	case FAT32_ERR_NOMEM:       return "out of memory";
	case FAT32_ERR_UNSUPPORTED: return "a layout this reader does not handle";
	case FAT32_ERR_TOO_SMALL_FOR_FAT32:
		return "too small to be FAT32 at any cluster size";
	}
	return "unknown";
}

/* --- Reading, in the volume's units ---------------------------------------
 *
 * The volume's sector size and the device's need not be the same number. Both
 * are read and neither is assumed; a volume whose sectors are not a whole
 * number of device blocks is refused rather than approximated, because the
 * arithmetic that would "handle" it is the arithmetic that silently reads the
 * wrong place.
 */
enum fat32_status fat_read_sectors(struct fat32 *fs, u64 vol_sector,
				   u32 count, void *buf)
{
	u64 byte = vol_sector * fs->bytes_per_sector;
	u32 per = fs->bytes_per_sector / fs->dev_sector_size;

	if (block_read(fs->dev, byte / fs->dev_sector_size, count * per,
		       buf) != BLOCK_OK)
		return FAT32_ERR_IO;

	return FAT32_OK;
}

u64 fat_cluster_to_sector(const struct fat32 *fs, u32 cluster)
{
	return fs->data_start +
	       (u64)(cluster - FAT32_CLUSTER_FIRST) * fs->sectors_per_cluster;
}

/* --- The allocation table --------------------------------------------------
 *
 * FAT32 volumes carry two copies of the table, and the whole reason for the
 * second is that the first can be wrong. Reading only one throws that away.
 *
 * They are compared **per sector, as the sector is read**, rather than in full
 * at mount. Comparing 8 MB of table to open a volume is a cost paid on every
 * mount for a fault almost no volume has -- and, as this project learned
 * measuring a checker against real media rather than against a file, a check
 * whose cost is invisible on an image can be minutes on a USB stick.
 *
 * Per-sector costs one extra read per *sector of table*, not per cluster, so a
 * file whose chain lives in one sector -- which is any file under 128 MB at
 * this cluster size -- costs exactly one. A disagreement is refused rather than
 * resolved: which copy is right is not knowable from here, and picking the
 * first is a guess wearing a uniform.
 */
static enum fat32_status fat_sector(struct fat32 *fs, u32 index, u8 *primary,
				    u8 *shadow)
{
	enum fat32_status st;
	u32 i;

	st = fat_read_sectors(fs, fs->fat_start + index, 1, primary);
	if (st != FAT32_OK)
		return st;

	if (fs->fat_count < 2)
		return FAT32_OK;

	st = fat_read_sectors(fs, fs->fat_start + fs->fat_sectors + index, 1,
			  shadow);
	if (st != FAT32_OK)
		return st;

	for (i = 0; i < fs->bytes_per_sector; i++)
		if (primary[i] != shadow[i])
			return FAT32_ERR_FATS_DIFFER;

	return FAT32_OK;
}

enum fat32_status fat32_next_cluster(struct fat32 *fs, u32 cluster, u32 *next)
{
	u32 per_sector = fs->bytes_per_sector / 4;
	u32 sector = cluster / per_sector;
	u32 within = (cluster % per_sector) * 4;
	u8 *a, *b;
	enum fat32_status st;

	if (cluster < FAT32_CLUSTER_FIRST ||
	    cluster >= fs->data_clusters + FAT32_CLUSTER_FIRST)
		return FAT32_ERR_CORRUPT;

	if (sector >= fs->fat_sectors)
		return FAT32_ERR_CORRUPT;

	a = kzalloc(fs->bytes_per_sector);
	b = kzalloc(fs->bytes_per_sector);
	if (!a || !b) {
		kfree(a);
		kfree(b);
		return FAT32_ERR_NOMEM;
	}

	st = fat_sector(fs, sector, a, b);
	if (st == FAT32_OK) {
		/* The top four bits belong to the filesystem, not to the
		 * cluster number, and a reader that keeps them compares a
		 * chain terminator against the wrong constant. */
		*next = fat_le32(a + within) & FAT32_CLUSTER_MASK;
	}

	kfree(a);
	kfree(b);
	return st;
}

void fat_put16(u8 *p, u16 v)
{
	p[0] = (u8)(v & 0xFF);
	p[1] = (u8)(v >> 8);
}

void fat_put32(u8 *p, u32 v)
{
	p[0] = (u8)(v & 0xFF);
	p[1] = (u8)((v >> 8) & 0xFF);
	p[2] = (u8)((v >> 16) & 0xFF);
	p[3] = (u8)(v >> 24);
}

enum fat32_status fat_write_sectors(struct fat32 *fs, u64 vol_sector, u32 count,
				    const void *buf)
{
	u64 byte = vol_sector * fs->bytes_per_sector;
	u32 per = fs->bytes_per_sector / fs->dev_sector_size;

	if (block_write(fs->dev, byte / fs->dev_sector_size, count * per,
			buf) != BLOCK_OK)
		return FAT32_ERR_IO;

	return FAT32_OK;
}

bool fat_cluster_ok(const struct fat32 *fs, u32 cluster)
{
	return cluster >= FAT32_CLUSTER_FIRST &&
	       cluster < fs->data_clusters + FAT32_CLUSTER_FIRST;
}

enum fat32_status fat_entry_get(struct fat32 *fs, u32 cluster, u32 *value)
{
	return fat32_next_cluster(fs, cluster, value);
}

/* --- Mounting --------------------------------------------------------------- */

enum fat32_status fat32_mount(struct block_device *dev, struct fat32 *fs)
{
	u8 *sector;
	enum fat32_status st = FAT32_OK;
	u32 root_entries, fat_sz16, tot16, fat_sz, tot_sec, root_dir_sectors;
	u32 data_sectors;

	kmemset(fs, 0, sizeof(*fs));
	fs->dev = dev;
	fs->dev_sector_size = dev->block_size;

	sector = kzalloc(fs->dev_sector_size);
	if (!sector)
		return FAT32_ERR_NOMEM;

	if (block_read(dev, 0, 1, sector) != BLOCK_OK) {
		kfree(sector);
		return FAT32_ERR_IO;
	}

	if (fat_le16(sector + FAT_BOOT_SIG) != 0xAA55) {
		kfree(sector);
		return FAT32_ERR_NOT_FAT;
	}

	fs->bytes_per_sector    = fat_le16(sector + FAT_BPB_BYTS_PER_SEC);
	fs->sectors_per_cluster = sector[FAT_BPB_SEC_PER_CLUS];
	fs->fat_count           = sector[FAT_BPB_NUM_FATS];
	fs->fat_start           = fat_le16(sector + FAT_BPB_RSVD_SEC_CNT);

	root_entries = fat_le16(sector + FAT_BPB_ROOT_ENT_CNT);
	fat_sz16     = fat_le16(sector + FAT_BPB_FAT_SZ16);
	tot16        = fat_le16(sector + FAT_BPB_TOT_SEC16);

	fat_sz  = fat_sz16 ? fat_sz16 : fat_le32(sector + FAT_BPB_FAT_SZ32);
	tot_sec = tot16 ? tot16 : fat_le32(sector + FAT_BPB_TOT_SEC32);

	/* Every one of these being sane is what separates a FAT volume from a
	 * sector that happens to end in 0xAA55 -- which every bootable MBR
	 * does, so this is not a hypothetical collision. */
	if (fs->bytes_per_sector < 512 || fs->bytes_per_sector > 4096 ||
	    (fs->bytes_per_sector & (fs->bytes_per_sector - 1)) != 0 ||
	    fs->sectors_per_cluster == 0 ||
	    (fs->sectors_per_cluster & (fs->sectors_per_cluster - 1)) != 0 ||
	    fs->fat_count == 0 || fs->fat_count > 2 ||
	    fs->fat_start == 0 || fat_sz == 0 || tot_sec == 0) {
		kfree(sector);
		return FAT32_ERR_NOT_FAT;
	}

	if (fs->bytes_per_sector < fs->dev_sector_size ||
	    fs->bytes_per_sector % fs->dev_sector_size != 0) {
		kfree(sector);
		return FAT32_ERR_UNSUPPORTED;
	}

	fs->fat_sectors      = fat_sz;
	fs->bytes_per_cluster = fs->bytes_per_sector * fs->sectors_per_cluster;

	/* On FAT32 this is zero and the root is a cluster chain like any other
	 * directory. The term is kept because the arithmetic below is the
	 * specification's, and changing its shape to suit one case is how the
	 * other case stops being computed. */
	root_dir_sectors = ((root_entries * 32) + (fs->bytes_per_sector - 1)) /
			   fs->bytes_per_sector;

	fs->data_start = fs->fat_start + (u64)fs->fat_count * fat_sz +
			 root_dir_sectors;

	if (fs->data_start >= tot_sec) {
		kfree(sector);
		return FAT32_ERR_NOT_FAT;
	}

	/* The volume says how long it is, and the device knows how long it is.
	 * Believe the device.
	 *
	 * Without this a boot sector claiming more sectors than exist mounts
	 * perfectly and then fails one read at a time, each reported as an I/O
	 * error -- so the machine says "the disk is broken" about a disk that is
	 * fine, and the actual fault, a boot sector that does not describe this
	 * device, is never named. This check exists because the damage test for
	 * it was written first and had nothing to catch it. */
	if ((u64)tot_sec * fs->bytes_per_sector >
	    (u64)dev->block_count * fs->dev_sector_size) {
		kfree(sector);
		return FAT32_ERR_NOT_FAT;
	}

	data_sectors     = tot_sec - (u32)fs->data_start;
	fs->data_clusters = data_sectors / fs->sectors_per_cluster;

	/* **The type, derived rather than believed.**
	 *
	 * There is a string at offset 82 reading "FAT32   ", and Microsoft's
	 * own specification says in as many words that it must not be used to
	 * determine the type. Formatters write whatever they like there. The
	 * count of data clusters is the only definition, and these boundaries
	 * are exact: 4084 and 65524, not 4096 and 65536. Off by one here reads
	 * the wrong width of entry from the right offset, which yields a
	 * plausible wrong answer rather than an error. */
	if (fs->data_clusters <= FAT16_MAX_CLUSTERS) {
		kfree(sector);
		return FAT32_ERR_WRONG_TYPE;
	}

	fs->root_cluster = fat_le32(sector + FAT_BPB_ROOT_CLUS) & FAT32_CLUSTER_MASK;
	if (fs->root_cluster < FAT32_CLUSTER_FIRST ||
	    fs->root_cluster >= fs->data_clusters + FAT32_CLUSTER_FIRST)
		st = FAT32_ERR_NOT_FAT;

	kfree(sector);
	return st;
}

/* --- Names -----------------------------------------------------------------
 *
 * A long name is stored *backwards*, in up to twenty 32-byte fragments that
 * precede the 8.3 entry they belong to, each holding thirteen UTF-16 units in
 * three discontiguous runs. The last fragment to appear on disk is the first
 * part of the name, and carries bit 6 of its sequence number set.
 *
 * Every fragment also carries a one-byte checksum **of the 8.3 name it belongs
 * to**. That is the only thing tying the two together: fragments are not
 * self-identifying, and a directory whose entries were partly rewritten by a
 * system that did not understand long names leaves orphans behind. Checking it
 * is what stops this reader gluing somebody else's name onto a file.
 */
u8 fat_alias_checksum(const u8 *name11)
{
	u8 sum = 0;
	unsigned i;

	for (i = 0; i < 11; i++)
		sum = (u8)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + name11[i]);

	return sum;
}

/* The three runs of UTF-16 units inside one long-name fragment. */
static const struct { u8 off, count; } lfn_runs[] = {
	{ 1, 5 }, { 14, 6 }, { 28, 2 }
};

/* UTF-16 to UTF-8. Surrogate pairs are combined; an unpaired surrogate is
 * refused rather than emitted, because a name that cannot be encoded is a name
 * this reader should not claim to have read. */
static bool utf16_to_utf8(const u16 *in, unsigned n, char *out, unsigned max)
{
	unsigned i, o = 0;

	for (i = 0; i < n; i++) {
		u32 c = in[i];

		if (c >= 0xD800 && c <= 0xDBFF) {
			u32 lo;

			if (i + 1 >= n)
				return false;
			lo = in[i + 1];
			if (lo < 0xDC00 || lo > 0xDFFF)
				return false;
			c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
			i++;
		} else if (c >= 0xDC00 && c <= 0xDFFF) {
			return false;
		}

		if (c < 0x80) {
			if (o + 1 >= max)
				return false;
			out[o++] = (char)c;
		} else if (c < 0x800) {
			if (o + 2 >= max)
				return false;
			out[o++] = (char)(0xC0 | (c >> 6));
			out[o++] = (char)(0x80 | (c & 0x3F));
		} else if (c < 0x10000) {
			if (o + 3 >= max)
				return false;
			out[o++] = (char)(0xE0 | (c >> 12));
			out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
			out[o++] = (char)(0x80 | (c & 0x3F));
		} else {
			if (o + 4 >= max)
				return false;
			out[o++] = (char)(0xF0 | (c >> 18));
			out[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
			out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
			out[o++] = (char)(0x80 | (c & 0x3F));
		}
	}

	out[o] = '\0';
	return true;
}

/* "KERNEL~1ELF" as stored becomes "KERNEL~1.ELF". The NTRes byte carries two
 * hints from Windows NT saying the base or extension was really lower case;
 * honoured, because a reader that ignores them turns `kernel.elf` into
 * `KERNEL.ELF` and then fails to match it against a path a person typed. */
static void alias_of(const u8 *e, char *out)
{
	unsigned i, o = 0;
	bool lower_base = (e[FAT_DIR_NTRES] & 0x08) != 0;
	bool lower_ext  = (e[FAT_DIR_NTRES] & 0x10) != 0;

	for (i = 0; i < 8 && e[i] != ' '; i++) {
		char c = (char)e[i];

		/* 0x05 in the first byte means the name really begins 0xE5,
		 * which on disk marks a deleted entry. */
		if (i == 0 && e[0] == 0x05)
			c = (char)0xE5;
		if (lower_base && c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
		out[o++] = c;
	}

	if (e[8] != ' ') {
		out[o++] = '.';
		for (i = 8; i < 11 && e[i] != ' '; i++) {
			char c = (char)e[i];

			if (lower_ext && c >= 'A' && c <= 'Z')
				c = (char)(c - 'A' + 'a');
			out[o++] = c;
		}
	}

	out[o] = '\0';
}

static char fold(char c)
{
	return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/* FAT is case-insensitive, and this folds ASCII only.
 *
 * Said rather than left implicit: the full rule is a Unicode case-folding table
 * that depends on the volume's code page, and a kernel that pretends to
 * implement it while only folding ASCII would be wrong in a way nobody could
 * see until a name in another script failed to match. Folding ASCII is what
 * every path the installer and the loader use consists of. */
static bool name_eq(const char *a, const char *b)
{
	while (*a && *b) {
		if (fold(*a) != fold(*b))
			return false;
		a++;
		b++;
	}
	return *a == '\0' && *b == '\0';
}

/* --- Walking a directory ---------------------------------------------------- */

struct dir_walk {
	struct fat32 *fs;
	u32 cluster;
	u8 *buf;		/* one cluster */
	u32 offset;		/* byte within the cluster */
	bool loaded;
	u32 guard;
};

static enum fat32_status walk_begin(struct fat32 *fs, u32 cluster,
				    struct dir_walk *w)
{
	kmemset(w, 0, sizeof(*w));
	w->fs = fs;
	w->cluster = cluster ? cluster : fs->root_cluster;
	w->buf = kzalloc(fs->bytes_per_cluster);
	return w->buf ? FAT32_OK : FAT32_ERR_NOMEM;
}

static void walk_end(struct dir_walk *w)
{
	kfree(w->buf);
	w->buf = NULL;
}

/* The next 32-byte record, following the chain across clusters.
 *
 * The guard is not decoration. A cluster chain is a linked list written by
 * software this kernel has never met, and a chain that points back at itself
 * makes this loop run for ever with no output -- which is the least
 * informative failure a kernel can have, and one this project has already
 * shipped once. Bounded by the number of clusters the volume actually has, so
 * the bound is a property of the volume rather than a number somebody guessed.
 */
static enum fat32_status walk_next(struct dir_walk *w, const u8 **rec)
{
	struct fat32 *fs = w->fs;
	enum fat32_status st;

	for (;;) {
		if (!w->loaded) {
			if (w->cluster < FAT32_CLUSTER_FIRST ||
			    w->cluster >= fs->data_clusters + FAT32_CLUSTER_FIRST)
				return FAT32_ERR_CORRUPT;

			st = fat_read_sectors(fs, fat_cluster_to_sector(fs, w->cluster),
					  fs->sectors_per_cluster, w->buf);
			if (st != FAT32_OK)
				return st;

			w->loaded = true;
			w->offset = 0;
		}

		if (w->offset + FAT_DIR_SIZE <= fs->bytes_per_cluster) {
			*rec = w->buf + w->offset;
			w->offset += FAT_DIR_SIZE;
			return FAT32_OK;
		}

		st = fat32_next_cluster(fs, w->cluster, &w->cluster);
		if (st != FAT32_OK)
			return st;

		if (w->cluster >= FAT32_CLUSTER_EOC)
			return FAT32_ERR_NOT_FOUND;

		if (++w->guard > fs->data_clusters)
			return FAT32_ERR_CORRUPT;

		w->loaded = false;
	}
}

/* One entry a person would see: long-name fragments gathered, deleted records
 * and the volume label skipped. */
static enum fat32_status next_entry(struct dir_walk *w, struct fat32_entry *out)
{
	u16 units[260];
	unsigned have = 0;
	u8 want_sum = 0;
	bool has_long = false;
	const u8 *rec;
	enum fat32_status st;

	for (;;) {
		st = walk_next(w, &rec);
		if (st != FAT32_OK)
			return st;

		if (rec[0] == FAT_ENTRY_FREE)
			return FAT32_ERR_NOT_FOUND;	/* and every one after */

		if (rec[0] == FAT_ENTRY_DELETED) {
			has_long = false;
			have = 0;
			continue;
		}

		if ((rec[FAT_DIR_ATTR] & FAT_ATTR_LONG_MASK) ==
		    FAT_ATTR_LONG_NAME) {
			unsigned seq = rec[0] & 0x1F;
			unsigned base, r, k;

			if (seq == 0 || seq > 20) {
				has_long = false;
				have = 0;
				continue;
			}

			if (rec[0] & 0x40) {		/* last, so first */
				has_long = true;
				have = seq * 13;
				want_sum = rec[13];
				if (have > 260)
					have = 260;
				for (k = 0; k < 260; k++)
					units[k] = 0;
			} else if (!has_long || rec[13] != want_sum) {
				has_long = false;
				have = 0;
				continue;
			}

			base = (seq - 1) * 13;
			k = 0;
			for (r = 0; r < 3; r++) {
				unsigned i;

				for (i = 0; i < lfn_runs[r].count; i++, k++)
					if (base + k < 260)
						units[base + k] =
							fat_le16(rec + lfn_runs[r].off + i * 2);
			}
			continue;
		}

		if (rec[FAT_DIR_ATTR] & FAT_ATTR_VOLUME_ID) {
			has_long = false;
			have = 0;
			continue;
		}

		/* A real entry. */
		kmemset(out, 0, sizeof(*out));
		alias_of(rec, out->alias);
		out->attr = rec[FAT_DIR_ATTR];
		out->is_dir = (out->attr & FAT_ATTR_DIRECTORY) != 0;
		out->size = fat_le32(rec + FAT_DIR_FILE_SIZE);
		out->first_cluster =
			(((u32)fat_le16(rec + FAT_DIR_FST_CLUS_HI)) << 16 |
			 fat_le16(rec + FAT_DIR_FST_CLUS_LO)) & FAT32_CLUSTER_MASK;

		/* The checksum is what ties fragments to this entry. Without
		 * it, orphaned fragments left by a system that did not
		 * understand long names get glued onto the next real file. */
		if (has_long && fat_alias_checksum(rec) == want_sum) {
			unsigned n = 0;

			while (n < have && units[n] != 0 && units[n] != 0xFFFF)
				n++;
			if (!utf16_to_utf8(units, n, out->name, FAT_NAME_MAX))
				kstrlcpy(out->name, out->alias,
					 sizeof(out->name));
		} else {
			kstrlcpy(out->name, out->alias, sizeof(out->name));
		}

		return FAT32_OK;
	}
}

enum fat32_status fat32_readdir(struct fat32 *fs, u32 dir_cluster, u32 index,
				struct fat32_entry *out)
{
	struct dir_walk w;
	enum fat32_status st;
	u32 seen = 0;

	st = walk_begin(fs, dir_cluster, &w);
	if (st != FAT32_OK)
		return st;

	for (;;) {
		st = next_entry(&w, out);
		if (st != FAT32_OK)
			break;
		if (seen++ == index)
			break;
	}

	walk_end(&w);
	return st;
}

enum fat32_status fat32_lookup(struct fat32 *fs, u32 dir_cluster,
			       const char *name, struct fat32_entry *out)
{
	struct dir_walk w;
	enum fat32_status st;

	if (!name || !*name)
		return FAT32_ERR_NAME;

	st = walk_begin(fs, dir_cluster, &w);
	if (st != FAT32_OK)
		return st;

	for (;;) {
		st = next_entry(&w, out);
		if (st != FAT32_OK)
			break;
		if (name_eq(out->name, name) || name_eq(out->alias, name))
			break;
	}

	walk_end(&w);
	return st;
}

enum fat32_status fat32_walk(struct fat32 *fs, const char *path,
			     struct fat32_entry *out)
{
	u32 dir = fs->root_cluster;
	char part[FAT_NAME_MAX];
	enum fat32_status st = FAT32_ERR_NAME;

	while (*path == '/')
		path++;

	if (!*path)
		return FAT32_ERR_NAME;

	while (*path) {
		unsigned n = 0;

		while (path[n] && path[n] != '/') {
			if (n + 1 >= sizeof(part))
				return FAT32_ERR_NAME;
			part[n] = path[n];
			n++;
		}
		part[n] = '\0';
		path += n;
		while (*path == '/')
			path++;

		st = fat32_lookup(fs, dir, part, out);
		if (st != FAT32_OK)
			return st;

		if (*path) {
			if (!out->is_dir)
				return FAT32_ERR_NOT_FOUND;
			dir = out->first_cluster;
		}
	}

	return st;
}

enum fat32_status fat32_read_file(struct fat32 *fs,
				  const struct fat32_entry *entry,
				  void *out, u32 max, u32 *got)
{
	u8 *dst = out;
	u32 left = entry->size;
	u32 cluster = entry->first_cluster;
	u32 guard = 0;
	u8 *buf;
	enum fat32_status st = FAT32_OK;

	if (got)
		*got = 0;

	if (entry->is_dir)
		return FAT32_ERR_NOT_FOUND;

	/* Refused, not truncated. A caller handed the first half of a file with
	 * a success status has no way to know. */
	if (entry->size > max)
		return FAT32_ERR_TOO_LARGE;

	if (entry->size == 0)
		return FAT32_OK;

	buf = kzalloc(fs->bytes_per_cluster);
	if (!buf)
		return FAT32_ERR_NOMEM;

	while (left) {
		u32 take = left < fs->bytes_per_cluster ? left
						       : fs->bytes_per_cluster;

		if (cluster < FAT32_CLUSTER_FIRST ||
		    cluster >= fs->data_clusters + FAT32_CLUSTER_FIRST) {
			st = FAT32_ERR_CORRUPT;
			break;
		}

		st = fat_read_sectors(fs, fat_cluster_to_sector(fs, cluster),
				  fs->sectors_per_cluster, buf);
		if (st != FAT32_OK)
			break;

		kmemcpy(dst, buf, take);
		dst += take;
		left -= take;

		if (!left)
			break;

		st = fat32_next_cluster(fs, cluster, &cluster);
		if (st != FAT32_OK)
			break;

		/* A file whose size says more than its chain provides is
		 * corrupt, and saying so beats returning what was there. */
		if (cluster >= FAT32_CLUSTER_EOC) {
			st = FAT32_ERR_CORRUPT;
			break;
		}

		if (++guard > fs->data_clusters) {
			st = FAT32_ERR_CORRUPT;
			break;
		}
	}

	kfree(buf);

	if (st == FAT32_OK && got)
		*got = entry->size;

	return st;
}
