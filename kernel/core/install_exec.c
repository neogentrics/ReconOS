/* Doing exactly what a plan says, and nothing worked out along the way.
 *
 * The split matters more than it looks. Every decision -- which disk, which
 * gap, whether an existing EFI partition can be reused, how the space divides
 * -- was made in install.c against a disk that was only read, and is testable
 * without risking anybody's data. This half takes those numbers and writes
 * them.
 *
 * **Nothing here recomputes a decision.** If this file ever needs to work
 * something out, that working-out belongs in the planner where it can be tested
 * cheaply, and the fact that it was needed here means the plan was incomplete.
 *
 * --- The order, and what each step leaves behind ----------------------------
 *
 *   1. the partition table          existing entries preserved byte for byte
 *   2. register the new partitions  so the steps below have somewhere to write
 *   3. the EFI filesystem           only when we created the partition
 *   4. ReconFS on system            }  the two ReconOS volumes, in the order
 *   5. ReconFS on programs          }  a person would expect to lose the less
 *
 * Steps 3 to 5 each write only inside a partition this install created or was
 * given. Nothing in this file addresses the whole disk after step 1, which is
 * the property that keeps a bug here inside our own partitions.
 */
#include <recon/kernel/install.h>

#include <recon/kernel/block.h>
#include <recon/kernel/boot.h>
#include <recon/kernel/console.h>
#include <recon/kernel/fat32.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/reconfs.h>

/* Finds the slice covering an extent, registering it if the table was only just
 * written and the block layer has not seen it. */
static struct block_device *slice_for(struct block_device *disk, u8 index,
				      const struct block_extent *e)
{
	unsigned i;

	for (i = 0; i < block_device_count(); i++) {
		struct block_device *d = block_device_at(i);

		if (d->parent == disk->id && d->first_lba == e->first_lba &&
		    d->block_count == e->count)
			return d;
	}

	return block_register_slice(disk, index, e->first_lba, e->count,
				    BLOCK_SCHEME_GPT);
}

/* The EFI partition already on this disk, which the plan chose to reuse.
 *
 * Found by looking for the one holding \\EFI rather than by index, because a
 * table this install just rewrote has entries in slots that were free, and an
 * index from before the write is a number that no longer means what it did.
 */
static struct block_device *esp_existing(struct block_device *disk)
{
	unsigned i;

	for (i = 0; i < block_device_count(); i++) {
		struct block_device *d = block_device_at(i);
		struct fat32 fs;
		struct fat32_entry e;

		if (d->parent != disk->id)
			continue;
		if (fat32_mount(d, &fs) != FAT32_OK)
			continue;
		if (fat32_walk(&fs, "/EFI", &e) == FAT32_OK && e.is_dir)
			return d;
	}

	return 0;
}

enum install_verdict install_execute(struct block_device *disk,
				     const struct install_plan *plan)
{
	struct block_device *system, *programs;
	enum install_verdict v;
	enum fat32_status fst;
	enum reconfs_status rst;

	if (plan->verdict != INSTALL_OK)
		return plan->verdict;

	/* Re-planned and compared, rather than trusted.
	 *
	 * A plan is a set of numbers that were true when the disk was read, and
	 * between then and here a person had time to read it and agree -- time
	 * in which a USB disk can be pulled out and a different one pushed in,
	 * under the same name. Re-reading and requiring the same answer costs a
	 * few milliseconds and turns "the disk changed underneath us" from a
	 * silent wrong-disk write into a refusal. */
	{
		struct install_plan now;

		install_plan(disk, &now);

		if (now.verdict != INSTALL_OK ||
		    now.fresh_table != plan->fresh_table ||
		    now.reuse_esp != plan->reuse_esp ||
		    now.system.first_lba != plan->system.first_lba ||
		    now.system.count != plan->system.count ||
		    now.programs.first_lba != plan->programs.first_lba ||
		    now.programs.count != plan->programs.count ||
		    now.esp.first_lba != plan->esp.first_lba ||
		    now.esp.count != plan->esp.count) {
			kputs("  the disk is not what it was when this was "
			      "planned; nothing written\n");
			return INSTALL_NO_TABLE_READABLE;
		}
	}

	v = install_write_gpt(disk, plan);
	if (v != INSTALL_OK)
		return v;

	kputs("  partition table    : written\n");

	system = slice_for(disk, 2, &plan->system);
	programs = slice_for(disk, 3, &plan->programs);
	if (!system || !programs)
		return INSTALL_TOO_MANY_SLICES;

	/* The BIOS boot partition becomes a device too, even though nothing
	 * mounts it: it is written through, and the block layer refuses a write
	 * to a whole disk that has slices unless the caller has claimed it.
	 * Writing stage 2 through the parent instead would be a write bounded
	 * by the disk rather than by the partition -- which is the exact shape
	 * of mistake that lands in a neighbour's filesystem. */
	if (plan->bios_boot.count &&
	    !slice_for(disk, 4, &plan->bios_boot))
		return INSTALL_TOO_MANY_SLICES;

	if (!plan->reuse_esp) {
		struct block_device *esp = slice_for(disk, 1, &plan->esp);

		if (!esp)
			return INSTALL_TOO_MANY_SLICES;

		fst = fat32_format(esp, "RECONOS");
		if (fst != FAT32_OK) {
			kprintf("  EFI filesystem     : FAILED (%s)\n",
				fat32_strerror(fst));
			return INSTALL_IO;
		}
		kputs("  EFI filesystem     : FAT32, made\n");
	} else {
		kputs("  EFI filesystem     : the one already there, kept\n");
	}

	rst = reconfs_format(system, "ReconOS System", 0);
	if (rst != RECONFS_OK) {
		kprintf("  system volume      : FAILED (%s)\n",
			reconfs_strerror(rst));
		return INSTALL_IO;
	}
	kputs("  system volume      : ReconFS, made\n");

	rst = reconfs_format(programs, "ReconOS Programs", 0);
	if (rst != RECONFS_OK) {
		kprintf("  programs volume    : FAILED (%s)\n",
			reconfs_strerror(rst));
		return INSTALL_IO;
	}
	kputs("  programs volume    : ReconFS, made\n");

	/* And the step that makes the machine boot. Last, because everything
	 * before it is preparation that a half-finished install leaves merely
	 * unused -- while a bootloader is the one file whose absence firmware
	 * notices and whose corruption firmware runs. */
	{
		struct block_device *medium = install_find_medium(disk);
		struct block_device *target = plan->reuse_esp
					    ? esp_existing(disk)
					    : slice_for(disk, 1, &plan->esp);

		if (!medium) {
			kputs("  bootloader         : no install medium "
			      "found; the disk is prepared but will not "
			      "boot\n");
			return INSTALL_OK;
		}

		if (!target)
			return INSTALL_TOO_MANY_SLICES;

		v = install_copy_boot(medium, target);
		if (v != INSTALL_OK)
			return v;

		/* And the BIOS path, last of all.
		 *
		 * Last because it is the only step that writes to the first
		 * sector of the disk -- the one that also holds the partition
		 * table. Everything reversible happens before the one thing
		 * that changes how the machine starts. */
		v = install_write_bios_boot(medium, disk, plan);
		if (v != INSTALL_OK)
			return v;
	}

	return INSTALL_OK;
}

/* Plans and then executes, on the device named on the command line.
 *
 * Guarded by a device name rather than a bare flag, and the name has to be a
 * whole disk, because the one mistake this must never make is being pointed at
 * something by accident. `install-plan` on its own still only ever reads. */
void install_execute_run(void)
{
	const char *p = boot_info()->cmdline;
	static char want[BLOCK_NAME_MAX];
	unsigned i;

	while (p && *p) {
		const char *k = "install-onto=";
		const char *q = p;

		while (*k && *q == *k) {
			q++;
			k++;
		}
		if (!*k) {
			size_t n = 0;

			while (q[n] && q[n] != ' ' && n + 1 < sizeof(want)) {
				want[n] = q[n];
				n++;
			}
			want[n] = '\0';
			break;
		}

		while (*p && *p != ' ')
			p++;
		while (*p == ' ')
			p++;
	}

	if (!want[0])
		return;

	for (i = 0; i < block_device_count(); i++) {
		struct block_device *d = block_device_at(i);
		const char *a = d->name, *b = want;
		struct install_plan plan;
		enum install_verdict v;

		while (*a && *a == *b) {
			a++;
			b++;
		}
		if (*a != *b)
			continue;

		kprintf("\ninstaller: installing onto %s\n", d->name);

		install_plan(d, &plan);
		install_print_plan(&plan);

		if (plan.verdict != INSTALL_OK)
			return;

		v = install_execute(d, &plan);
		kprintf("  %s\n", v == INSTALL_OK
				? "installed: table, EFI filesystem and two "
				  "ReconOS volumes"
				: install_verdict_name(v));
		return;
	}

	kprintf("\ninstaller: no disk called %s\n", want);
}
