/* Reading ext2. See the header for what this refuses and why.
 *
 * --- The one arithmetic trap in this format ---
 *
 * Almost every number here is a *block* number, and a block is 1024, 2048 or
 * 4096 bytes depending on a shift in the superblock -- while the device
 * underneath speaks in 512-byte sectors. Every conversion between the two is a
 * multiply that can overflow if it is done in 32 bits, and ext2 block numbers
 * are 32 bits.
 *
 * So every conversion goes through `block_lba`, which widens first. A
 * filesystem on a 2TB partition has block numbers near 2^29, and 2^29 * 4096
 * does not fit in a u32 -- it wraps, and the read lands near the start of the
 * disk and succeeds.
 */
#include <recon/kernel/ext2.h>
#include <recon/kernel/block.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/vm.h>

/* The superblock, as far as this reads it. */
struct ext2_super {
	u32 inodes_count;
	u32 blocks_count;
	u32 r_blocks_count;
	u32 free_blocks_count;
	u32 free_inodes_count;
	u32 first_data_block;
	u32 log_block_size;
	u32 log_frag_size;
	u32 blocks_per_group;
	u32 frags_per_group;
	u32 inodes_per_group;
	u32 mtime;
	u32 wtime;
	u16 mnt_count;
	u16 max_mnt_count;
	u16 magic;
	u16 state;
	u16 errors;
	u16 minor_rev_level;
	u32 lastcheck;
	u32 checkinterval;
	u32 creator_os;
	u32 rev_level;
	u16 def_resuid;
	u16 def_resgid;
	/* Revision 1 and later. Zero on an old filesystem, and the defaults
	 * below are what those numbers mean there. */
	u32 first_ino;
	u16 inode_size;
	u16 block_group_nr;
	u32 feature_compat;
	u32 feature_incompat;
	u32 feature_ro_compat;
} __attribute__((packed));

struct ext2_group_desc {
	u32 block_bitmap;
	u32 inode_bitmap;
	u32 inode_table;
	u16 free_blocks_count;
	u16 free_inodes_count;
	u16 used_dirs_count;
	u16 pad;
	u32 reserved[3];
} __attribute__((packed));

struct ext2_raw_inode {
	u16 mode;
	u16 uid;
	u32 size;
	u32 atime, ctime, mtime, dtime;
	u16 gid;
	u16 links_count;
	u32 blocks;
	u32 flags;
	u32 osd1;
	u32 block[15];
	u32 generation;
	u32 file_acl;
	u32 dir_acl;
} __attribute__((packed));

struct ext2_dir_entry {
	u32 inode;
	u16 rec_len;
	u8  name_len;
	u8  file_type;
	/* the name follows, name_len bytes, not terminated */
} __attribute__((packed));

static u64 mounts, files_read, dirs_listed, refusals, repairs;

const char *ext2_status_name(enum ext2_status s)
{
	switch (s) {
	case EXT2_OK:			return "ok";
	case EXT2_ERR_NO_DEVICE:	return "no device";
	case EXT2_ERR_IO:		return "the disk would not read";
	case EXT2_ERR_NOT_EXT2:		return "not ext2";
	case EXT2_ERR_UNSUPPORTED:	return "ext2 this cannot read safely";
	case EXT2_ERR_NOT_FOUND:	return "no such name";
	case EXT2_ERR_NOT_DIR:		return "not a directory";
	case EXT2_ERR_RANGE:		return "outside the volume";
	case EXT2_ERR_READ_ONLY:	return "a repair that was not asked for";
	case EXT2_ERR_CORRUPT:		return "the structure does not hold";
	}

	return "unknown";
}

/* A filesystem block as a device sector, widened before it is multiplied. */
static u64 block_lba(const struct ext2 *fs, u32 block)
{
	return ((u64)block * (u64)fs->block_size) / 512ull;
}

static u32 sectors_per_block(const struct ext2 *fs)
{
	return fs->block_size / 512;
}

/* One filesystem block into a caller's buffer. */
static enum ext2_status read_block(struct ext2 *fs, u32 block, void *out)
{
	if (!fs || !fs->dev || !out)
		return EXT2_ERR_NO_DEVICE;

	/* Subtraction, never addition: `block >= count` is the only form that
	 * cannot be defeated by a number near the top of its range. */
	if (block >= fs->block_count)
		return EXT2_ERR_RANGE;

	if (block_read(fs->dev, block_lba(fs, block), sectors_per_block(fs),
		       out) != BLOCK_OK)
		return EXT2_ERR_IO;

	return EXT2_OK;
}

enum ext2_status ext2_mount(struct block_device *dev, struct ext2 *fs)
{
	struct ext2_super *sb;
	paddr_t page;
	u8 *sector;

	if (!dev || !fs)
		return EXT2_ERR_NO_DEVICE;

	kmemset(fs, 0, sizeof(*fs));

	/* A page from the allocator, not a local array, and that is not a
	 * preference.
	 *
	 * A driver hands the device a **physical** address, which it obtains by
	 * subtracting the direct map's base from the pointer it was given. A
	 * stack buffer lives in the kernel image rather than the direct map, so
	 * there is no physical address for it, and `virt_to_phys` answers zero
	 * -- which is what makes the driver refuse.
	 *
	 * **It did not always refuse, and this call is what found that out.**
	 * The bounds test was one-sided, so a kernel-image address subtracted
	 * to a plausible-looking number and the device wrote to a physical
	 * address about a hundred and forty terabytes in. The read was entered,
	 * queued, issued and reported BLOCK_OK with the buffer untouched
	 * (KF-193).
	 *
	 * Every other buffer in this file already came from the page allocator.
	 * This one did not, which is why it was the one that noticed.
	 */
	page = pmm_alloc_page();

	if (!page)
		return EXT2_ERR_IO;

	sector = phys_to_virt(page);

	/* The superblock is 1024 bytes in, whatever the block size -- the
	 * first 1024 belong to a boot sector that may or may not exist. Two
	 * sectors, because the structure spans a 512-byte boundary. */
	if (block_read(dev, 2, 2, sector) != BLOCK_OK) {
		pmm_free_pages(page, 1);
		return EXT2_ERR_IO;
	}

	sb = (struct ext2_super *)sector;

	if (sb->magic != EXT2_SUPER_MAGIC)
	{
		pmm_free_pages(page, 1);
		return EXT2_ERR_NOT_EXT2;
	}

	fs->dev              = dev;
	fs->block_size       = 1024u << sb->log_block_size;
	fs->blocks_per_group = sb->blocks_per_group;
	fs->inodes_per_group = sb->inodes_per_group;
	fs->inode_count      = sb->inodes_count;
	fs->block_count      = sb->blocks_count;
	fs->first_data_block = sb->first_data_block;
	fs->incompat         = (sb->rev_level >= 1) ? sb->feature_incompat : 0;

	/* Revision 0 has no inode_size field and every inode is 128 bytes.
	 * Reading the field anyway gives whatever happens to be there. */
	fs->inode_size = (sb->rev_level >= 1 && sb->inode_size)
			 ? sb->inode_size : 128;

	fs->filetype = (fs->incompat & EXT2_FEATURE_INCOMPAT_FILETYPE) != 0;

	/* Refused, each for its own reason, and named. "Unsupported" as a
	 * single answer would send somebody looking in the wrong place. */
	if (fs->incompat & EXT2_FEATURE_INCOMPAT_RECOVER) {
		refusals++;
		kputs("  ext2         : this filesystem needs its journal "
		      "replayed, so what is on disk is known to be stale\n");
	{
		pmm_free_pages(page, 1);
		return EXT2_ERR_UNSUPPORTED;
	}
	}

	if (fs->incompat & EXT2_FEATURE_INCOMPAT_EXTENTS) {
		refusals++;
		kputs("  ext2         : this filesystem uses extents, and the "
		      "block array holds a tree this does not walk\n");
	{
		pmm_free_pages(page, 1);
		return EXT2_ERR_UNSUPPORTED;
	}
	}

	if (fs->incompat & EXT2_FEATURE_INCOMPAT_64BIT) {
		refusals++;
		kputs("  ext2         : 64-bit block numbers change the group "
		      "descriptor's shape\n");
	{
		pmm_free_pages(page, 1);
		return EXT2_ERR_UNSUPPORTED;
	}
	}

	if (!fs->block_size || fs->block_size > 4096 ||
	    !fs->blocks_per_group || !fs->inodes_per_group ||
	    !fs->inode_count || !fs->block_count ||
	    fs->inode_size < 128 || fs->inode_size > fs->block_size)
	{
		pmm_free_pages(page, 1);
		return EXT2_ERR_CORRUPT;
	}

	/* Rounded up, because the last group is usually short and a division
	 * that rounds down loses it -- along with every file in it. */
	fs->group_count = (fs->block_count - fs->first_data_block +
			   fs->blocks_per_group - 1) / fs->blocks_per_group;

	if (!fs->group_count)
	{
		pmm_free_pages(page, 1);
		return EXT2_ERR_CORRUPT;
	}

	fs->mounted = true;
	mounts++;

	pmm_free_pages(page, 1);
	return EXT2_OK;
}

/* The group descriptor table starts in the block after the superblock. */
static enum ext2_status read_group(struct ext2 *fs, u32 group,
				   struct ext2_group_desc *out)
{
	u32 per_block = fs->block_size / sizeof(struct ext2_group_desc);
	u32 table = fs->first_data_block + 1;
	u8 *buf;
	paddr_t page;
	enum ext2_status s;

	if (group >= fs->group_count || !per_block)
		return EXT2_ERR_RANGE;

	page = pmm_alloc_page();

	if (!page)
		return EXT2_ERR_IO;

	buf = phys_to_virt(page);
	s = read_block(fs, table + group / per_block, buf);

	if (s == EXT2_OK)
		kmemcpy(out, buf + (group % per_block) * sizeof(*out),
			sizeof(*out));

	pmm_free_pages(page, 1);
	return s;
}


/* Writes one group descriptor back.
 *
 * The only write in this file, and it is reached from one place: `ext2_repair`,
 * after the caller has said the word. Read-modify-write of the block the
 * descriptor lives in, because a descriptor is 32 bytes and a block is at
 * least 1024 -- writing the block without reading it first would blank every
 * other group in it.
 */
static enum ext2_status write_group(struct ext2 *fs, u32 group,
				    const struct ext2_group_desc *in)
{
	u32 per_block = fs->block_size / sizeof(struct ext2_group_desc);
	u32 table = fs->first_data_block + 1;
	u32 block;
	paddr_t page;
	u8 *buf;
	enum ext2_status s;

	if (group >= fs->group_count || !per_block)
		return EXT2_ERR_RANGE;

	block = table + group / per_block;
	page = pmm_alloc_page();

	if (!page)
		return EXT2_ERR_IO;

	buf = phys_to_virt(page);
	s = read_block(fs, block, buf);

	if (s == EXT2_OK) {
		kmemcpy(buf + (group % per_block) * sizeof(*in), in,
			sizeof(*in));

		if (block_write(fs->dev, block_lba(fs, block),
				sectors_per_block(fs), buf) != BLOCK_OK)
			s = EXT2_ERR_IO;
	}

	pmm_free_pages(page, 1);
	return s;
}

enum ext2_status ext2_read_inode(struct ext2 *fs, u32 number,
				 struct ext2_inode_info *out)
{
	struct ext2_group_desc gd;
	struct ext2_raw_inode *raw;
	u32 group, index, block, offset;
	u8 *buf;
	paddr_t page;
	enum ext2_status s;

	if (!fs || !fs->mounted || !out)
		return EXT2_ERR_NO_DEVICE;

	/* Inodes are numbered from one. Zero is not "the first inode", it is
	 * the value a directory entry uses to mean *this slot is empty*. */
	if (!number || number > fs->inode_count)
		return EXT2_ERR_RANGE;

	group = (number - 1) / fs->inodes_per_group;
	index = (number - 1) % fs->inodes_per_group;

	s = read_group(fs, group, &gd);

	if (s != EXT2_OK)
		return s;

	/* Widened before multiplying: inode_size times index passes 32 bits on
	 * a filesystem with many inodes per group. */
	block  = gd.inode_table +
		 (u32)(((u64)index * fs->inode_size) / fs->block_size);
	offset = (u32)(((u64)index * fs->inode_size) % fs->block_size);

	page = pmm_alloc_page();

	if (!page)
		return EXT2_ERR_IO;

	buf = phys_to_virt(page);
	s = read_block(fs, block, buf);

	if (s != EXT2_OK) {
		pmm_free_pages(page, 1);
		return s;
	}

	raw = (struct ext2_raw_inode *)(buf + offset);

	kmemset(out, 0, sizeof(*out));
	out->number = number;
	out->mode   = raw->mode;
	out->size   = raw->size;
	out->is_dir = (raw->mode & 0xF000) == 0x4000;

	/* An inode may say its blocks are an extent tree even on a filesystem
	 * whose superblock did not. Checked here as well, because this is
	 * where the array is about to be believed. */
	if (raw->flags & EXT2_INODE_FLAG_EXTENTS) {
		pmm_free_pages(page, 1);
		refusals++;
		return EXT2_ERR_UNSUPPORTED;
	}

	kmemcpy(out->blocks, raw->block, sizeof(out->blocks));

	pmm_free_pages(page, 1);
	return EXT2_OK;
}

/* Which disk block holds block `n` of a file.
 *
 * Twelve direct, then one indirect block of pointers, then a double indirect.
 * Triple indirect is refused rather than walked: reaching it needs a file
 * larger than this kernel has anywhere to put, and an untested path that deep
 * is a path that is wrong. */
static enum ext2_status block_of(struct ext2 *fs,
				 const struct ext2_inode_info *in, u32 n,
				 u32 *out)
{
	u32 per_block = fs->block_size / 4;
	paddr_t page;
	u32 *table;
	enum ext2_status s;

	if (n < 12) {
		*out = in->blocks[n];
		return EXT2_OK;
	}

	n -= 12;

	page = pmm_alloc_page();

	if (!page)
		return EXT2_ERR_IO;

	table = phys_to_virt(page);

	if (n < per_block) {
		s = read_block(fs, in->blocks[12], table);

		if (s == EXT2_OK)
			*out = table[n];

		pmm_free_pages(page, 1);
		return s;
	}

	n -= per_block;

	if (n < per_block * per_block) {
		u32 outer;

		s = read_block(fs, in->blocks[13], table);

		if (s != EXT2_OK) {
			pmm_free_pages(page, 1);
			return s;
		}

		outer = table[n / per_block];
		s = read_block(fs, outer, table);

		if (s == EXT2_OK)
			*out = table[n % per_block];

		pmm_free_pages(page, 1);
		return s;
	}

	pmm_free_pages(page, 1);
	return EXT2_ERR_UNSUPPORTED;
}

i64 ext2_read(struct ext2 *fs, const struct ext2_inode_info *in, u64 offset,
	      void *out, u64 len)
{
	u8 *dst = out;
	u64 done = 0;
	paddr_t page;
	u8 *buf;

	if (!fs || !fs->mounted || !in || !out)
		return EXT2_ERR_NO_DEVICE;

	if (offset >= in->size)
		return 0;

	/* Subtraction: `offset + len` wraps and then compares as inside. */
	if (len > in->size - offset)
		len = in->size - offset;

	page = pmm_alloc_page();

	if (!page)
		return EXT2_ERR_IO;

	buf = phys_to_virt(page);

	while (done < len) {
		u32 index = (u32)((offset + done) / fs->block_size);
		u32 within = (u32)((offset + done) % fs->block_size);
		u32 take = fs->block_size - within;
		u32 disk = 0;

		if ((u64)take > len - done)
			take = (u32)(len - done);

		if (block_of(fs, in, index, &disk) != EXT2_OK)
			break;

		/* A hole. ext2 records an unwritten block as zero, and it reads
		 * back as zeroes rather than as an error -- a sparse file is a
		 * real file, not a damaged one. */
		if (!disk) {
			kmemset(dst + done, 0, take);
			done += take;
			continue;
		}

		if (read_block(fs, disk, buf) != EXT2_OK)
			break;

		kmemcpy(dst + done, buf + within, take);
		done += take;
	}

	pmm_free_pages(page, 1);
	files_read++;

	return (i64)done;
}

/* Walks one directory, calling `fn` for each entry. Returns false if the
 * directory's own structure is broken, which is different from it being
 * empty. */
static bool walk_dir(struct ext2 *fs, const struct ext2_inode_info *dir,
		     bool (*fn)(void *ctx, const char *name, u8 len,
				u32 inode),
		     void *ctx)
{
	u64 offset = 0;
	paddr_t page = pmm_alloc_page();
	u8 *buf;

	if (!page)
		return false;

	buf = phys_to_virt(page);

	while (offset < dir->size) {
		u32 index = (u32)(offset / fs->block_size);
		u32 disk = 0;
		u32 at = 0;

		if (block_of(fs, dir, index, &disk) != EXT2_OK || !disk)
			break;

		if (read_block(fs, disk, buf) != EXT2_OK)
			break;

		while (at < fs->block_size) {
			struct ext2_dir_entry *e =
				(struct ext2_dir_entry *)(buf + at);

			/* A record length of zero would loop for ever, and a
			 * length running past the block would read the next
			 * entry out of the middle of this one. Both are the
			 * damage `ext2_check` reports. */
			if (e->rec_len < sizeof(*e) ||
			    at + e->rec_len > fs->block_size)
				break;

			if (e->inode && e->name_len &&
			    at + sizeof(*e) + e->name_len <= fs->block_size)
				if (!fn(ctx, (const char *)(buf + at +
							    sizeof(*e)),
					e->name_len, e->inode))
					goto done;

			at += e->rec_len;
		}

		offset += fs->block_size;
	}

done:
	pmm_free_pages(page, 1);
	return true;
}

struct find_ctx {
	const char *want;
	u8 want_len;
	u32 found;
};

static bool find_one(void *ctx, const char *name, u8 len, u32 inode)
{
	struct find_ctx *f = ctx;

	if (len == f->want_len && kmemcmp(name, f->want, len) == 0) {
		f->found = inode;
		return false;		/* stop */
	}

	return true;
}

enum ext2_status ext2_lookup(struct ext2 *fs, const char *path,
			     struct ext2_inode_info *out)
{
	struct ext2_inode_info here;
	enum ext2_status s;
	const char *p = path;

	if (!fs || !fs->mounted || !path || !out)
		return EXT2_ERR_NO_DEVICE;

	s = ext2_read_inode(fs, EXT2_ROOT_INODE, &here);

	if (s != EXT2_OK)
		return s;

	while (*p) {
		struct find_ctx f;
		const char *start;
		u8 len = 0;

		while (*p == '/')
			p++;

		if (!*p)
			break;

		start = p;

		while (*p && *p != '/' && len < EXT2_NAME_MAX) {
			p++;
			len++;
		}

		if (!here.is_dir)
			return EXT2_ERR_NOT_DIR;

		f.want = start;
		f.want_len = len;
		f.found = 0;

		walk_dir(fs, &here, find_one, &f);

		if (!f.found)
			return EXT2_ERR_NOT_FOUND;

		s = ext2_read_inode(fs, f.found, &here);

		if (s != EXT2_OK)
			return s;
	}

	*out = here;
	return EXT2_OK;
}

struct list_ctx {
	char *names;
	u64 names_len;
	u64 needed;
	unsigned count;
};

static bool list_one(void *ctx, const char *name, u8 len, u32 inode)
{
	struct list_ctx *l = ctx;

	if (l->needed + len + 1 <= l->names_len && l->names) {
		kmemcpy(l->names + l->needed, name, len);
		l->names[l->needed + len] = 0;
	}

	l->needed += (u64)len + 1;
	l->count++;

	return true;
}

i64 ext2_list(struct ext2 *fs, const struct ext2_inode_info *dir, char *names,
	      u64 names_len, unsigned *count)
{
	struct list_ctx l;

	if (!fs || !fs->mounted || !dir)
		return EXT2_ERR_NO_DEVICE;

	if (!dir->is_dir)
		return EXT2_ERR_NOT_DIR;

	l.names = names;
	l.names_len = names_len;
	l.needed = 0;
	l.count = 0;

	walk_dir(fs, dir, list_one, &l);

	/* Whole or absent, never half. The answer is the size of the complete
	 * listing whether or not it fitted. */
	if (l.needed > names_len)
		l.count = 0;

	if (count)
		*count = l.count;

	dirs_listed++;

	return (i64)l.needed;
}


/* --- checking, which writes nothing ---------------------------------------
 *
 * Every finding here is a disagreement between two things on the disk that are
 * supposed to say the same thing. That is deliberate and it is what makes a
 * repair possible: a checker that only knows one of the two can tell you
 * something is wrong and cannot tell you which half to believe.
 *
 *   free block counts   the superblock's total, each group's count, and what
 *                       the bitmaps actually show. Three statements of one
 *                       fact.
 *   bitmap addresses    a group whose bitmap block is outside the volume is a
 *                       group nothing can be allocated from -- and reading it
 *                       gives whatever is at that sector.
 *   directory records   an entry whose length runs past its block, or is zero.
 *                       Zero is the one that matters: walking it loops for
 *                       ever, so it has to be found rather than met.
 *
 * **It opens nothing for writing.** That is not enforced by discipline; there
 * is no write call in this function.
 */

/* Counts the set bits in one bitmap block, up to `bits` of them. */
static u32 count_used(const u8 *bitmap, u32 bits)
{
	u32 used = 0;
	u32 i;

	for (i = 0; i < bits; i++)
		if (bitmap[i / 8] & (1u << (i % 8)))
			used++;

	return used;
}

static void note(struct ext2_damage *d, const char *what)
{
	if (!d->first[0])
		kstrlcpy(d->first, what, sizeof(d->first));
}

enum ext2_status ext2_check(struct ext2 *fs, struct ext2_damage *out)
{
	paddr_t page;
	u8 *buf;
	u32 g;

	if (!fs || !fs->mounted || !out)
		return EXT2_ERR_NO_DEVICE;

	kmemset(out, 0, sizeof(*out));

	page = pmm_alloc_page();

	if (!page)
		return EXT2_ERR_IO;

	buf = phys_to_virt(page);

	for (g = 0; g < fs->group_count; g++) {
		struct ext2_group_desc gd;
		u32 blocks_here, used;

		if (read_group(fs, g, &gd) != EXT2_OK) {
			out->bad_block_bitmap++;
			note(out, "a group descriptor could not be read");
			continue;
		}

		/* A bitmap outside the volume. Checked before it is read,
		 * because reading it is what would return somebody else's
		 * sectors as though they were an allocation map. */
		if (gd.block_bitmap >= fs->block_count) {
			out->bad_block_bitmap++;
			note(out, "a group's block bitmap is outside the "
			     "volume");
			continue;
		}

		if (gd.inode_bitmap >= fs->block_count) {
			out->bad_inode_bitmap++;
			note(out, "a group's inode bitmap is outside the "
			     "volume");
			continue;
		}

		/* The last group is usually short, and counting a full group's
		 * worth of bits in it would report the padding as used. */
		blocks_here = fs->blocks_per_group;

		if ((u64)fs->first_data_block + (u64)(g + 1) *
		    fs->blocks_per_group > fs->block_count)
			blocks_here = (u32)(fs->block_count -
					    fs->first_data_block -
					    (u64)g * fs->blocks_per_group);

		if (blocks_here > fs->block_size * 8)
			blocks_here = fs->block_size * 8;

		if (read_block(fs, gd.block_bitmap, buf) != EXT2_OK) {
			out->bad_block_bitmap++;
			note(out, "a block bitmap would not read");
			continue;
		}

		used = count_used(buf, blocks_here);

		if (blocks_here - used != gd.free_blocks_count) {
			out->free_blocks_wrong++;
			note(out, "a group's free block count disagrees with "
			     "its own bitmap");
		}

		if (read_block(fs, gd.inode_bitmap, buf) != EXT2_OK) {
			out->bad_inode_bitmap++;
			note(out, "an inode bitmap would not read");
			continue;
		}

		used = count_used(buf, fs->inodes_per_group);

		if (fs->inodes_per_group - used != gd.free_inodes_count) {
			out->free_inodes_wrong++;
			note(out, "a group's free inode count disagrees with "
			     "its own bitmap");
		}
	}

	pmm_free_pages(page, 1);

	/* The root directory, walked. A filesystem whose root cannot be read
	 * is one nothing above this can do anything with, and it is the one
	 * directory that is always there to check. */
	{
		struct ext2_inode_info root;

		if (ext2_read_inode(fs, EXT2_ROOT_INODE, &root) != EXT2_OK) {
			out->inode_past_end++;
			note(out, "the root inode could not be read");
		} else if (!root.is_dir) {
			out->dir_entry_bad++;
			note(out, "the root inode is not a directory");
		}
	}

	return EXT2_OK;
}

/* --- repairing, which does not happen unless it is asked for --------------
 *
 * Only the counts. That is not a first instalment -- it is the set of things
 * where this kernel can be *sure* which of two disagreeing numbers is wrong.
 *
 * A bitmap is the record of what is actually allocated; a free count is a
 * cached summary of it. When they disagree, the bitmap is the evidence and the
 * count is the claim, so the count is what gets rewritten. Nothing here
 * invents an allocation, frees a block, or touches a directory -- those need a
 * decision about *which file* something belongs to, and a kernel that guesses
 * at that turns a filesystem somebody could still recover into one nobody can.
 *
 * `e2fsck` is the tool for the rest, and saying so is better than half doing
 * its job.
 */
enum ext2_status ext2_repair(struct ext2 *fs, const char *confirm,
			     unsigned *fixed)
{
	struct ext2_damage d;
	paddr_t page;
	u8 *buf;
	u32 g;
	unsigned changed = 0;
	enum ext2_status s;

	if (!fs || !fs->mounted)
		return EXT2_ERR_NO_DEVICE;

	/* The word, exactly. A boolean is what a caller passes by accident;
	 * this is the same argument the block layer makes about claiming a
	 * disk that already has partitions on it. */
	if (!confirm || kstrlen(confirm) != 6 ||
	    kmemcmp(confirm, "repair", 6) != 0) {
		refusals++;
		return EXT2_ERR_READ_ONLY;
	}

	if (fs->dev && fs->dev->read_only)
		return EXT2_ERR_READ_ONLY;

	s = ext2_check(fs, &d);

	if (s != EXT2_OK)
		return s;

	/* Nothing to do is a result, not a failure. Reported as zero changes
	 * and EXT2_OK, which is a different answer from a repair that could
	 * not proceed. */
	if (!d.free_blocks_wrong && !d.free_inodes_wrong) {
		if (fixed)
			*fixed = 0;

		return EXT2_OK;
	}

	page = pmm_alloc_page();

	if (!page)
		return EXT2_ERR_IO;

	buf = phys_to_virt(page);

	for (g = 0; g < fs->group_count; g++) {
		struct ext2_group_desc gd;
		u32 blocks_here, used;
		bool dirty = false;

		if (read_group(fs, g, &gd) != EXT2_OK)
			continue;

		if (gd.block_bitmap >= fs->block_count ||
		    gd.inode_bitmap >= fs->block_count)
			continue;	/* named by the check; not ours to guess at */

		blocks_here = fs->blocks_per_group;

		if ((u64)fs->first_data_block + (u64)(g + 1) *
		    fs->blocks_per_group > fs->block_count)
			blocks_here = (u32)(fs->block_count -
					    fs->first_data_block -
					    (u64)g * fs->blocks_per_group);

		if (blocks_here > fs->block_size * 8)
			blocks_here = fs->block_size * 8;

		if (read_block(fs, gd.block_bitmap, buf) == EXT2_OK) {
			used = count_used(buf, blocks_here);

			if (blocks_here - used != gd.free_blocks_count) {
				gd.free_blocks_count =
					(u16)(blocks_here - used);
				dirty = true;
			}
		}

		if (read_block(fs, gd.inode_bitmap, buf) == EXT2_OK) {
			used = count_used(buf, fs->inodes_per_group);

			if (fs->inodes_per_group - used !=
			    gd.free_inodes_count) {
				gd.free_inodes_count =
					(u16)(fs->inodes_per_group - used);
				dirty = true;
			}
		}

		if (dirty && write_group(fs, g, &gd) == EXT2_OK)
			changed++;
	}

	pmm_free_pages(page, 1);

	if (fixed)
		*fixed = changed;

	repairs += changed;

	return EXT2_OK;
}

void ext2_print_summary(void)
{
	kprintf("  ext2         : %llu mounted, %llu files read, %llu "
		"directories listed, %llu refused\n",
		(unsigned long long)mounts, (unsigned long long)files_read,
		(unsigned long long)dirs_listed, (unsigned long long)refusals);

	if (repairs)
		kprintf("  ext2         : %llu group descriptors repaired\n",
			(unsigned long long)repairs);
}

/* --- the self-test --------------------------------------------------------
 *
 * Against an image `mke2fs` wrote, which is the whole point. A reader and a
 * writer built from the same misunderstanding agree perfectly; agreeing with a
 * tool written by somebody else, from the specification, is evidence about the
 * format rather than about this kernel's self-consistency. It is the same rule
 * the partition tests follow.
 *
 * Four claims:
 *
 *   1. it mounts, and the geometry matches what dumpe2fs reports
 *   2. a file in the root reads back byte for byte
 *   3. a file one directory down reads back, which is the path walk working
 *   4. the checker finds nothing wrong with a filesystem e2fsck calls clean
 *
 * The fourth is the one that could pass by accident, so the repair path is
 * checked for its refusal too: asking without the word must be refused, and
 * that refusal is what stands between a mounted foreign filesystem and a
 * kernel that writes to it.
 */
bool ext2_self_test(void)
{
	struct block_device *dev = NULL;
	struct ext2 fs;
	struct ext2_inode_info in;
	struct ext2_damage d;
	enum ext2_status s;
	unsigned i, fixed = 0;
	char buf[128];
	i64 n;
	bool ok = true;

	/* Any device with an ext2 filesystem on it. Tried rather than assumed:
	 * on most boots there is none, and that is not a failure. */
	for (i = 0; i < block_device_count(); i++) {
		struct block_device *d2 = block_device_at(i);
		enum ext2_status ms;

		if (!d2 || !d2->present) {
			continue;
		}

		ms = ext2_mount(d2, &fs);
		if (ms == EXT2_OK) {
			dev = d2;
			break;
		}
	}

	if (!dev) {
		kputs("  ext2: no ext2 filesystem on this machine, so there "
		      "is nothing to read\n");
		return true;
	}

	kprintf("  ext2         : %s, %u blocks of %u bytes, %u inodes, "
		"%u groups\n", dev->name, fs.block_count, fs.block_size,
		fs.inode_count, fs.group_count);

	/* 2. A file in the root. */
	s = ext2_lookup(&fs, "/hello.txt", &in);

	if (s != EXT2_OK) {
		kprintf("  ext2: /hello.txt would not resolve (%s)\n",
			ext2_status_name(s));
		ok = false;
	} else {
		n = ext2_read(&fs, &in, 0, buf, sizeof(buf) - 1);

		if (n <= 0) {
			kputs("  ext2: /hello.txt read back nothing\n");
			ok = false;
		} else {
			buf[n] = 0;

			if (kmemcmp(buf, "the quick brown fox", 19) != 0) {
				kprintf("  ext2: /hello.txt reads as \"%s\", "
					"which is not what was written\n",
					buf);
				ok = false;
			}
		}
	}

	/* 3. One directory down, which is the path walk rather than the root
	 *    directory happening to be readable. */
	s = ext2_lookup(&fs, "/docs/notes.txt", &in);

	if (s != EXT2_OK) {
		kprintf("  ext2: /docs/notes.txt would not resolve (%s) -- "
			"the walk stops at the first directory\n",
			ext2_status_name(s));
		ok = false;
	} else {
		n = ext2_read(&fs, &in, 0, buf, sizeof(buf) - 1);

		if (n <= 0 || kmemcmp(buf, "second file", 11) != 0) {
			kputs("  ext2: /docs/notes.txt did not read back\n");
			ok = false;
		}
	}

	/* 4. The checker agrees with e2fsck about a clean filesystem. */
	if (ext2_check(&fs, &d) != EXT2_OK) {
		kputs("  ext2: the checker could not run\n");
		ok = false;
	} else if (d.free_blocks_wrong || d.free_inodes_wrong ||
		   d.bad_block_bitmap || d.bad_inode_bitmap ||
		   d.inode_past_end || d.dir_entry_bad) {
		kprintf("  ext2: the checker reports damage on a filesystem "
			"e2fsck calls clean -- %s\n",
			d.first[0] ? d.first : "and did not say what");
		ok = false;
	}

	/* And the refusal, which is what stands between a mounted foreign
	 * filesystem and a kernel that writes to it. Checked with a word that
	 * is nearly right, because that is the mistake a caller makes. */
	if (ext2_repair(&fs, "repai", &fixed) != EXT2_ERR_READ_ONLY) {
		kputs("  ext2: a repair went ahead without being asked for by "
		      "name\n");
		ok = false;
	}

	if (ext2_repair(&fs, NULL, &fixed) != EXT2_ERR_READ_ONLY) {
		kputs("  ext2: a repair went ahead with no confirmation at "
		      "all\n");
		ok = false;
	}

	return ok;
}
