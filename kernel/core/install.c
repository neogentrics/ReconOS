/* Deciding what an install would do, without doing any of it.
 *
 * See install.h for why this is a separate half. The short version: an
 * installer runs once, on somebody else's machine, with their data on it, and
 * the expensive part of testing one is trying it. Everything here is reachable
 * by a test that risks nothing.
 */
#include <recon/kernel/install.h>

#include <recon/kernel/block.h>
#include <recon/kernel/console.h>
#include <recon/kernel/boot.h>
#include <recon/kernel/fat32.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/kstring.h>

/* The GPT header, at LBA 1, by offset. Read here rather than in the kernel's
 * partition reader for the reason install.h gives: this is the caller, and the
 * caller is where type GUIDs belong. */
#define GPT_SIG			0	/* 8 bytes, "EFI PART" */
#define GPT_FIRST_USABLE	40	/* u64 */
#define GPT_LAST_USABLE		48	/* u64 */
#define GPT_ENTRY_LBA		72	/* u64 */
#define GPT_ENTRY_COUNT		80	/* u32 */
#define GPT_ENTRY_SIZE		84	/* u32 */

#define GPT_ENT_TYPE		0	/* 16 bytes */
#define GPT_ENT_FIRST		32	/* u64 */
#define GPT_ENT_LAST		40	/* u64 */

/* C12A7328-F81F-11D2-BA4B-00A0C93EC93B, as it is laid out on disk: the first
 * three fields little-endian, the last two as written. */
static const u8 esp_type[16] = {
	0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
	0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

static u32 le32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
	       ((u32)p[3] << 24);
}

static u64 le64(const u8 *p)
{
	return (u64)le32(p) | ((u64)le32(p + 4) << 32);
}

const char *install_verdict_name(enum install_verdict v)
{
	switch (v) {
	case INSTALL_OK:
		return "a plan exists";
	case INSTALL_NO_TABLE_READABLE:
		return "this disk has a partition table that cannot be read, "
		       "and a disk whose layout is unknown is the one disk "
		       "never to write to";
	case INSTALL_NOT_GPT:
		return "this disk uses an MBR partition table, which this "
		       "installer does not write yet";
	case INSTALL_NO_ROOM_SYSTEM:
		return "there is not enough unpartitioned space for ReconOS";
	case INSTALL_NO_ROOM_PROGRAMS:
		return "there is room for ReconOS but not for a separate "
		       "partition to install programs into";
	case INSTALL_NO_ROOM_ESP:
		return "there is no EFI System Partition and not enough "
		       "unpartitioned space to make one";
	case INSTALL_ESP_TOO_FULL:
		return "the EFI System Partition on this disk has no room "
		       "left for a bootloader";
	case INSTALL_TOO_MANY_SLICES:
		return "this disk has more partitions than the kernel tracks";
	case INSTALL_IO:
		return "the disk could not be read";
	}
	return "unknown";
}

/* --- Free space -------------------------------------------------------------
 *
 * Computed from the *table*, not from the slices the kernel registered. Those
 * are the same set today, and relying on that would be relying on the kernel
 * having understood every entry -- including any it declined to register. An
 * entry this code did not understand is still occupied space, and space that
 * looks free because something failed to parse is exactly the space an
 * installer must not use.
 */
struct gap {
	u64 first;
	u64 count;
};

#define MAX_ENTRIES	128

struct survey {
	u64 first_usable;
	u64 last_usable;

	struct gap gaps[MAX_ENTRIES + 1];
	unsigned gap_count;

	unsigned used;			/* entries in use */
	u64 used_blocks;

	bool  esp_found;
	u8    esp_index;
	u64   esp_first;
	u64   esp_count;
};

static bool same_type(const u8 *a, const u8 *b)
{
	unsigned i;

	for (i = 0; i < 16; i++)
		if (a[i] != b[i])
			return false;
	return true;
}

static bool type_is_zero(const u8 *t)
{
	unsigned i;

	for (i = 0; i < 16; i++)
		if (t[i])
			return false;
	return true;
}

static enum install_verdict survey_gpt(struct block_device *disk,
				       struct survey *s)
{
	u8 *sector = kzalloc(disk->block_size);
	u8 *entries = 0;
	u32 count, size, per_sector, sectors;
	u64 entry_lba;
	unsigned i;
	u64 cursor;
	enum install_verdict v = INSTALL_IO;

	if (!sector)
		return INSTALL_IO;

	kmemset(s, 0, sizeof(*s));

	if (block_read(disk, 1, 1, sector) != BLOCK_OK)
		goto out;

	if (sector[0] != 'E' || sector[1] != 'F' || sector[2] != 'I' ||
	    sector[3] != ' ' || sector[4] != 'P' || sector[5] != 'A' ||
	    sector[6] != 'R' || sector[7] != 'T') {
		v = INSTALL_NOT_GPT;
		goto out;
	}

	s->first_usable = le64(sector + GPT_FIRST_USABLE);
	s->last_usable  = le64(sector + GPT_LAST_USABLE);
	entry_lba       = le64(sector + GPT_ENTRY_LBA);
	count           = le32(sector + GPT_ENTRY_COUNT);
	size            = le32(sector + GPT_ENTRY_SIZE);

	/* Bounded before anything is believed. These numbers were chosen by
	 * whatever last wrote this disk. */
	if (!count || count > MAX_ENTRIES || size < 128 ||
	    size > disk->block_size ||
	    s->first_usable >= s->last_usable ||
	    s->last_usable >= disk->block_count) {
		v = INSTALL_NO_TABLE_READABLE;
		goto out;
	}

	per_sector = disk->block_size / size;
	sectors = (count + per_sector - 1) / per_sector;

	entries = kzalloc((size_t)sectors * disk->block_size);
	if (!entries) {
		v = INSTALL_IO;
		goto out;
	}

	if (block_read(disk, entry_lba, sectors, entries) != BLOCK_OK)
		goto out;

	/* Entries are not required to be in order on disk, and a gap list built
	 * from an unsorted set is nonsense. Collected first, then sorted by
	 * where they start -- an insertion sort, because 128 is 128. */
	{
		struct gap used[MAX_ENTRIES];
		unsigned n = 0;

		for (i = 0; i < count; i++) {
			const u8 *e = entries + (size_t)i * size;
			u64 first, last;

			if (type_is_zero(e + GPT_ENT_TYPE))
				continue;

			first = le64(e + GPT_ENT_FIRST);
			last  = le64(e + GPT_ENT_LAST);

			if (first > last || first < s->first_usable ||
			    last > s->last_usable) {
				v = INSTALL_NO_TABLE_READABLE;
				goto out;
			}

			if (same_type(e + GPT_ENT_TYPE, esp_type) &&
			    !s->esp_found) {
				s->esp_found = true;
				s->esp_index = (u8)(i + 1);
				s->esp_first = first;
				s->esp_count = last - first + 1;
			}

			used[n].first = first;
			used[n].count = last - first + 1;
			n++;
			s->used_blocks += last - first + 1;
		}

		s->used = n;

		for (i = 1; i < n; i++) {
			struct gap key = used[i];
			unsigned j = i;

			while (j && used[j - 1].first > key.first) {
				used[j] = used[j - 1];
				j--;
			}
			used[j] = key;
		}

		/* Overlapping entries mean the table is not describing a disk
		 * anybody could have used. Refused rather than reconciled. */
		for (i = 1; i < n; i++)
			if (used[i].first < used[i - 1].first + used[i - 1].count) {
				v = INSTALL_NO_TABLE_READABLE;
				goto out;
			}

		cursor = s->first_usable;
		for (i = 0; i < n; i++) {
			if (used[i].first > cursor) {
				s->gaps[s->gap_count].first = cursor;
				s->gaps[s->gap_count].count =
					used[i].first - cursor;
				s->gap_count++;
			}
			cursor = used[i].first + used[i].count;
		}
		if (cursor <= s->last_usable) {
			s->gaps[s->gap_count].first = cursor;
			s->gaps[s->gap_count].count =
				s->last_usable - cursor + 1;
			s->gap_count++;
		}
	}

	v = INSTALL_OK;
out:
	kfree(entries);
	kfree(sector);
	return v;
}

/* Rounds a gap's start up to alignment and returns what is left of it.
 *
 * Every partitioning tool in use aligns to a megabyte, and a partition that is
 * not aligned costs a read-modify-write on every write to a drive whose real
 * block is larger than its reported one -- which is every drive sold since
 * about 2011. Aligning is not tidiness. */
static bool aligned_fit(const struct gap *g, u64 need, struct block_extent *out)
{
	u64 first = g->first;
	u64 end = g->first + g->count;	/* exclusive */
	u64 pad = first % INSTALL_ALIGN_BLOCKS;

	if (pad)
		first += INSTALL_ALIGN_BLOCKS - pad;

	if (first >= end || end - first < need)
		return false;

	out->first_lba = first;
	out->count = need;
	return true;
}

/* How much room is left inside an ESP that is already there.
 *
 * Its *size* is the wrong question: a 100 MB ESP that Windows has filled with
 * three firmware updates has no room for a bootloader, and one that looks small
 * may be nearly empty. So it is mounted and asked. */
static bool esp_has_room(struct block_device *disk, const struct survey *s,
			 u64 *free_blocks)
{
	struct block_device *slice = 0;
	struct fat32 fs;
	u32 free_clusters = 0;
	unsigned i;

	for (i = 0; i < block_device_count(); i++) {
		struct block_device *d = block_device_at(i);

		if (d->parent == disk->id && d->first_lba == s->esp_first) {
			slice = d;
			break;
		}
	}

	if (!slice)
		return false;

	if (fat32_mount(slice, &fs) != FAT32_OK)
		return false;

	if (fat32_free_clusters(&fs, &free_clusters) != FAT32_OK)
		return false;

	*free_blocks = (u64)free_clusters *
		       (fs.bytes_per_cluster / disk->block_size);

	return *free_blocks >= INSTALL_ESP_MIN_FREE;
}

/* Divides a run of blocks between the system and its programs.
 *
 * An operating system is roughly a fixed size; a library of installed software
 * is not, and grows with the disk it is on. So the system takes a quarter,
 * never less than its floor and never more than 64 GiB, and programs take
 * everything else. On a small disk that gives the system what it needs; on a
 * large one it stops a terabyte of ReconOS volume sitting empty beside a
 * cramped programs volume.
 */
static void split_rest(u64 first, u64 count, struct install_plan *out)
{
	u64 sys = count / INSTALL_SYSTEM_SHARE;

	if (sys < INSTALL_SYSTEM_MIN)
		sys = INSTALL_SYSTEM_MIN;
	if (sys > INSTALL_SYSTEM_CAP)
		sys = INSTALL_SYSTEM_CAP;
	if (sys > count - INSTALL_PROGRAMS_MIN)
		sys = count - INSTALL_PROGRAMS_MIN;

	sys -= sys % INSTALL_ALIGN_BLOCKS;

	out->system.first_lba = first;
	out->system.count = sys;

	out->programs.first_lba = first + sys;
	out->programs.count = count - sys;
	out->programs.count -= out->programs.count % INSTALL_ALIGN_BLOCKS;
}

enum install_verdict install_plan(struct block_device *disk,
				  struct install_plan *out)
{
	struct survey s;
	enum install_verdict v;
	unsigned i;

	kmemset(out, 0, sizeof(*out));
	out->disk = disk;

	if (disk->parent) {
		/* A slice is not a disk. Planning against one would produce a
		 * layout expressed in the wrong coordinates, which is the shape
		 * of mistake that lands writes in a neighbour. */
		out->verdict = INSTALL_NO_TABLE_READABLE;
		return out->verdict;
	}

	if (disk->scheme == BLOCK_SCHEME_UNREADABLE) {
		out->verdict = INSTALL_NO_TABLE_READABLE;
		return out->verdict;
	}

	if (disk->scheme == BLOCK_SCHEME_NONE) {
		/* A blank disk: one table, and both partitions in it. The whole
		 * device is ours because there is demonstrably nothing to
		 * lose -- and "demonstrably" is the partition reader having
		 * looked and found no table, not this code assuming. */
		u64 first = INSTALL_ALIGN_BLOCKS;
		u64 usable;

		out->fresh_table = true;

		/* 33 blocks at each end for the two copies of the table, and
		 * one aligned block at the front for the protective MBR and the
		 * primary copy. */
		if (disk->block_count < 34 + first) {
			out->verdict = INSTALL_NO_ROOM_ESP;
			return out->verdict;
		}

		usable = disk->block_count - 33 - first;

		if (usable < INSTALL_ESP_CREATE + INSTALL_SYSTEM_MIN +
			     INSTALL_PROGRAMS_MIN) {
			out->verdict = usable < INSTALL_ESP_CREATE
					     ? INSTALL_NO_ROOM_ESP
					     : INSTALL_NO_ROOM_PROGRAMS;
			return out->verdict;
		}

		out->esp.first_lba = first;
		out->esp.count = INSTALL_ESP_CREATE;

		split_rest(first + INSTALL_ESP_CREATE,
			   usable - INSTALL_ESP_CREATE, out);

		out->verdict = INSTALL_OK;
		return out->verdict;
	}

	v = survey_gpt(disk, &s);
	if (v != INSTALL_OK) {
		out->verdict = v;
		return v;
	}

	out->untouched_slices = s.used;
	out->untouched_blocks = s.used_blocks;

	/* An existing ESP is reused if it has room. Making a second one on a
	 * machine that has one is how a dual-boot install stops booting:
	 * which of two ESPs the firmware picks is not this code's decision. */
	if (s.esp_found) {
		u64 free_blocks = 0;

		if (esp_has_room(disk, &s, &free_blocks)) {
			out->reuse_esp = true;
			out->reuse_esp_index = s.esp_index;
			out->reuse_esp_free_blocks = free_blocks;
		} else {
			out->verdict = INSTALL_ESP_TOO_FULL;
			return out->verdict;
		}
	}

	/* The system partition goes in the largest gap that fits, and the ESP
	 * -- if one has to be made -- in the first gap that will take it. */
	{
		struct block_extent best = { 0, 0 };
		bool have_esp = out->reuse_esp;

		if (!have_esp) {
			for (i = 0; i < s.gap_count; i++)
				if (aligned_fit(&s.gaps[i], INSTALL_ESP_CREATE,
						&out->esp)) {
					have_esp = true;
					/* Consume it, so the system partition
					 * does not plan to use the same
					 * blocks. */
					s.gaps[i].count -=
						(out->esp.first_lba +
						 out->esp.count) -
						s.gaps[i].first;
					s.gaps[i].first = out->esp.first_lba +
							  out->esp.count;
					break;
				}

			if (!have_esp) {
				out->verdict = INSTALL_NO_ROOM_ESP;
				return out->verdict;
			}
		}

		for (i = 0; i < s.gap_count; i++) {
			struct block_extent got;

			/* Both volumes come out of one gap, so the gap has to
			 * hold both. Splitting them across two gaps is
			 * possible and deliberately not done: it would put the
			 * system and its programs in whatever two holes
			 * happened to exist, which is a layout nobody chose
			 * and nobody could predict from the disk. */
			if (!aligned_fit(&s.gaps[i],
					 INSTALL_SYSTEM_MIN +
					 INSTALL_PROGRAMS_MIN, &got))
				continue;

			got.count = s.gaps[i].first + s.gaps[i].count -
				    got.first_lba;
			got.count -= got.count % INSTALL_ALIGN_BLOCKS;

			if (got.count > best.count)
				best = got;
		}

		if (!best.count) {
			out->verdict = INSTALL_NO_ROOM_SYSTEM;
			return out->verdict;
		}

		split_rest(best.first_lba, best.count, out);
	}

	/* Checked by the layer that can see the disk, not by the arithmetic
	 * that produced it. Four writes that each land on the disk can still
	 * describe two partitions that overlap. */
	{
		struct block_extent plan[3];
		unsigned n = 0;

		if (!out->reuse_esp)
			plan[n++] = out->esp;
		plan[n++] = out->system;
		plan[n++] = out->programs;

		if (block_check_layout(disk, plan, n) != BLOCK_OK) {
			out->verdict = INSTALL_NO_ROOM_SYSTEM;
			return out->verdict;
		}
	}

	out->verdict = INSTALL_OK;
	return out->verdict;
}

static void print_size(u64 blocks, u32 block_size)
{
	u64 bytes = blocks * block_size;

	if (bytes >= (1024ull * 1024 * 1024))
		kprintf("%llu.%llu GB",
			(unsigned long long)(bytes / (1024ull * 1024 * 1024)),
			(unsigned long long)((bytes / (1024ull * 1024 * 107)) % 10));
	else
		kprintf("%llu MB", (unsigned long long)(bytes / (1024 * 1024)));
}

void install_print_plan(const struct install_plan *p)
{
	struct block_device *d = p->disk;

	kprintf("  %s: ", d->name);

	if (p->verdict != INSTALL_OK) {
		kprintf("no\n      %s\n", install_verdict_name(p->verdict));
		return;
	}

	kputs("yes\n");

	if (p->fresh_table)
		kputs("      the disk is blank, so a new partition table\n");
	else
		kprintf("      %u existing partition%s left exactly as "
			"%s are, holding ", p->untouched_slices,
			p->untouched_slices == 1 ? "" : "s",
			p->untouched_slices == 1 ? "it" : "they");

	if (!p->fresh_table) {
		print_size(p->untouched_blocks, d->block_size);
		kputs("\n");
	}

	if (p->reuse_esp) {
		kprintf("      the EFI partition already there (number %u) is "
			"reused, with ", p->reuse_esp_index);
		print_size(p->reuse_esp_free_blocks, d->block_size);
		kputs(" free\n");
	} else {
		kputs("      a new EFI partition of ");
		print_size(p->esp.count, d->block_size);
		kprintf(" at block %lu\n", p->esp.first_lba);
	}

	kputs("      a ReconOS system partition of ");
	print_size(p->system.count, d->block_size);
	kprintf(" at block %lu\n", p->system.first_lba);

	kputs("      a separate partition for programs of ");
	print_size(p->programs.count, d->block_size);
	kprintf(" at block %lu\n", p->programs.first_lba);
}

void install_plan_run(void)
{
	const char *p = boot_info()->cmdline;
	bool wanted = false;
	unsigned i;

	while (p && *p) {
		const char *k = "install-plan";
		const char *q = p;

		while (*k && *q == *k) {
			q++;
			k++;
		}
		if (!*k && (*q == '\0' || *q == ' ')) {
			wanted = true;
			break;
		}

		while (*p && *p != ' ')
			p++;
		while (*p == ' ')
			p++;
	}

	if (!wanted)
		return;

	kputs("\ninstaller: what installing would do, on every disk here\n");

	for (i = 0; i < block_device_count(); i++) {
		struct block_device *d = block_device_at(i);
		struct install_plan plan;

		if (d->parent)
			continue;

		install_plan(d, &plan);
		install_print_plan(&plan);
	}

	kputs("      (nothing was written)\n");
}
