/* The recovery environment: what to do when the machine will not start.
 *
 * --- Where it lives, which decides everything else --------------------------
 *
 * **A recovery environment must not depend on the thing it repairs.** That one
 * rule settles most of the design:
 *
 *   - It cannot live on the ReconOS system volume, because a broken system
 *     volume is the most likely reason somebody is here.
 *   - It cannot need ReconFS to be mountable, for the same reason.
 *   - It has to be reachable by firmware alone, which means the EFI System
 *     Partition -- the one volume the machine can read before anything of ours
 *     has run.
 *
 * So recovery is **this same kernel**, booted from the ESP with `recovery` on
 * its command line, doing something else with itself. Not a second kernel: a
 * separate recovery build is a second thing to keep working, and the one time
 * it matters is the one time nobody has been testing it. The kernel that boots
 * you every day is the kernel that recovers you, and it is exercised every day.
 *
 * A separate recovery *partition* was the other option and is not needed yet.
 * Windows uses one because its recovery environment is a whole second operating
 * system; ours is a few thousand lines that already have to be on the ESP.
 *
 * --- What it does, and what it refuses to do --------------------------------
 *
 * It **looks**, and it says what it found. Nothing here writes.
 *
 * That is not a first-version compromise, it is the shape of the thing. A
 * person arrives here because a machine will not start, which means they do not
 * yet know why -- and a tool that begins by repairing is a tool that destroys
 * the evidence of what was wrong. Repair comes second, chosen explicitly, once
 * somebody has read what this says.
 */
#include <recon/kernel/block.h>
#include <recon/kernel/boot.h>
#include <recon/kernel/console.h>
#include <recon/kernel/fat32.h>
#include <recon/kernel/install.h>
#include <recon/kernel/reconfs.h>

static bool asked_for(const char *word)
{
	const char *p = boot_info()->cmdline;

	while (p && *p) {
		const char *k = word;
		const char *q = p;

		while (*k && *q == *k) {
			q++;
			k++;
		}
		if (!*k && (*q == '\0' || *q == ' '))
			return true;

		while (*p && *p != ' ')
			p++;
		while (*p == ' ')
			p++;
	}

	return false;
}

static void print_size(u64 blocks, u32 block_size)
{
	u64 mb = (blocks * block_size) / (1024 * 1024);

	if (mb >= 1024)
		kprintf("%llu.%llu GB", (unsigned long long)(mb / 1024),
			(unsigned long long)((mb % 1024) * 10 / 1024));
	else
		kprintf("%llu MB", (unsigned long long)mb);
}

/* One volume, and the most useful thing that can be said about it.
 *
 * The order matters: ReconFS first because that is ours and the checker can say
 * something precise, then FAT32, then "something else". A volume this kernel
 * does not recognise is reported as unrecognised rather than as empty --
 * somebody looking at a disk they think holds their data needs those to be
 * different sentences.
 */
static void look_at(struct block_device *d)
{
	struct reconfs fs;
	struct fat32 ffs;
	enum reconfs_status rst;

	kprintf("    %-12s ", d->name);
	print_size(d->block_count, d->block_size);

	rst = reconfs_mount(d, &fs);
	if (rst == RECONFS_OK) {
		struct reconfs_check_result r;

		kprintf("  ReconFS, %u-byte blocks\n", fs.block_size);

		/* The checker, which is the whole reason a person would run
		 * this. It reads only, and it derives the in-use set twice from
		 * disjoint fields -- so a volume it calls sound is sound by two
		 * accounts rather than one. */
		if (reconfs_check(&fs, &r) == RECONFS_OK && !r.disagreements) {
			kprintf("                 checked: sound "
				"(%llu blocks, %llu inodes)\n",
				(unsigned long long)r.blocks_seen_forward,
				(unsigned long long)r.inodes);
		} else {
			kprintf("                 checked: DAMAGED -- %s",
				r.first_disagreement ? r.first_disagreement
						     : "the two accounts differ");
			if (r.first_disagreement_block)
				kprintf(" (block %llu)",
					(unsigned long long)r.first_disagreement_block);
			kputs("\n");
		}

		reconfs_unmount(&fs);
		return;
	}

	if (fat32_mount(d, &ffs) == FAT32_OK) {
		struct fat32_entry e;
		bool bootable = fat32_walk(&ffs, "/EFI/BOOT", &e) == FAT32_OK &&
				e.is_dir;

		kprintf("  FAT32%s\n", bootable ? ", holds a bootloader" : "");
		return;
	}

	/* Said as "not recognised", never as "empty". Somebody looking at a
	 * disk they believe holds their data needs those to be different
	 * sentences, and this kernel understands two formats out of the many
	 * that exist. */
	kprintf("  not a filesystem this kernel recognises (%s)\n",
		reconfs_strerror(rst));
}

void recovery_run(void)
{
	unsigned i, disks = 0, volumes = 0;

	if (!asked_for("recovery"))
		return;

	kputs("\n=== ReconOS recovery ===\n\n");
	kputs("  Nothing here writes to anything. This is what the machine "
	      "looks like.\n\n");

	for (i = 0; i < block_device_count(); i++) {
		struct block_device *d = block_device_at(i);

		if (d->parent)
			continue;

		disks++;
		kprintf("  %s, ", d->name);
		print_size(d->block_count, d->block_size);
		kprintf("  %s", block_scheme_name((enum block_scheme)d->scheme));

		if (d->slice_count)
			kprintf(", %u partition%s\n", d->slice_count,
				d->slice_count == 1 ? "" : "s");
		else
			kputs(", no partitions\n");
	}

	if (!disks) {
		kputs("  No disks at all. The machine cannot see its own "
		      "storage, which is a hardware or driver problem rather "
		      "than a damaged system.\n");
		return;
	}

	kputs("\n  Volumes:\n");

	for (i = 0; i < block_device_count(); i++) {
		struct block_device *d = block_device_at(i);

		if (!d->parent)
			continue;

		volumes++;
		look_at(d);
	}

	if (!volumes)
		kputs("    none -- every disk here is unpartitioned\n");

	kputs("\n  Nothing was changed. Repair is a separate step, chosen "
	      "after reading the above.\n");
}
