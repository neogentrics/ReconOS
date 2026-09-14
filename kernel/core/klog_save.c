/* Writing the boot report to the medium the machine booted from.
 *
 * --- why this exists ----------------------------------------------------
 *
 * A machine with no serial port says everything it has to say to a panel, once,
 * and then it is gone. The verification rig reads a wire; a laptop has no wire.
 * Every diagnosis on real hardware so far has come from photographing a screen
 * and reading it back -- which works, loses the part that scrolled, and costs a
 * round trip per question.
 *
 * `klog.c` has kept every character the kernel has printed since diagnostics
 * were built, and its own header names the gap this closes:
 *
 *     Not persistent. It is memory, so it goes when the power does -- reading
 *     it back after a reset needs somewhere on a disk to put it.
 *
 * There is somewhere. The machine booted from a USB stick, the kernel can read
 * that stick's partition table, and it can write FAT32 that other systems'
 * tools can read. So the report goes back onto the medium it came from, and
 * whoever pulls the stick out reads the whole thing instead of the last screen.
 *
 * --- what it will not do -------------------------------------------------
 *
 * **It will not write to a disk the machine did not boot from.** The target is
 * found by looking for the ReconOS medium's own shape -- a FAT32 volume with
 * `\reconos` on it and a loader beside it -- and nothing else is considered. A
 * diagnostic that picks a likely-looking partition is an operating system that
 * writes to somebody's data because it was curious.
 *
 * **It cannot report a hang.** This runs at the end of the boot, so a machine
 * that stops earlier leaves nothing behind. That is a real limit and the reason
 * the boot report is still printed to the screen as it goes: the file is for
 * the boots that finish, and the panel is for the ones that do not.
 *
 * **It says what it did on the console**, so a stick with no file on it and a
 * boot that claimed to write one are distinguishable.
 */
#include <recon/kernel/klog.h>
#include <recon/kernel/block.h>
#include <recon/kernel/fat32.h>
#include <recon/kernel/console.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/kstring.h>

/* The largest report this will carry off the machine.
 *
 * The ring is smaller than this, so in practice the whole of it goes. The cap
 * exists because the buffer is allocated in one piece and a boot that somehow
 * printed megabytes should lose its oldest lines rather than fail to allocate
 * and lose all of them. */
#define KLOG_SAVE_MAX (256u * 1024u)

/* Where it goes: the **root of the volume**, as a plain text file.
 *
 * Not tucked in beside the kernels. Somebody pulls this stick out of a machine
 * that would not talk to them and puts it in one that will, and the first thing
 * they should see is the file -- not a directory they have to be told about.
 * Plain text because every system opens one without being asked what it is.
 */
#define KLOG_NAME "RECONOS-BOOT.TXT"

/* Is this the volume ReconOS booted from?
 *
 * Asked by looking for what the medium is built with rather than by trusting a
 * device name or an index: `\reconos` holds the kernels and the loader stages,
 * and `make-medium.sh` is the only thing that puts it there. A FAT32 partition
 * belonging to Windows has an `\EFI` and no `\reconos`, so it is not a
 * candidate and is never opened for writing.
 */
static bool is_our_medium(struct fat32 *fs)
{
	struct fat32_entry e;

	/* **`fat32_walk`, not `fat32_mkpath`.**
	 *
	 * The first version of this asked mkpath, whose own documentation says
	 * it "makes every component of a path exist" -- so the *identification*
	 * would have created `\reconos` on the first writable FAT32 volume it
	 * looked at, and then recognised the directory it had just made as
	 * proof the volume was ours. On somebody's Windows EFI partition.
	 *
	 * A test that establishes the condition it is testing for is not a
	 * test, and this one contradicted the promise written at the top of
	 * this file. Walking reads and creates nothing. */
	if (fat32_walk(fs, "reconos", &e) != FAT32_OK)
		return false;

	return e.is_dir;
}

void klog_save_to_medium(void)
{
	char *buf;
	u32 held;
	unsigned i;

	held = klog_held();
	if (!held) {
		kputs("  boot log     : nothing recorded, so nothing written\n");
		return;
	}

	if (held > KLOG_SAVE_MAX)
		held = KLOG_SAVE_MAX;

	buf = kmalloc(held);
	if (!buf) {
		kputs("  boot log     : no memory to gather it, so it was not "
		      "written\n");
		return;
	}

	held = klog_read(buf, held);

	/* Every block device, in order, until one turns out to be ours.
	 *
	 * Not "the device we booted from", because the kernel is not told which
	 * that was -- the loader hands over a kernel, not a device handle. What
	 * it can do is recognise the medium by its contents, which is the same
	 * question asked in a way the machine can answer. */
	for (i = 0; i < BLOCK_MAX_DEVICES; i++) {
		struct block_device *dev = block_device_at(i);
		struct fat32 fs;
		enum fat32_status st;

		if (!dev || dev->read_only)
			continue;

		if (fat32_mount(dev, &fs) != FAT32_OK)
			continue;

		if (!is_our_medium(&fs))
			continue;

		st = fat32_write_named(&fs, fs.root_cluster, KLOG_NAME,
				       buf, held);
		if (st != FAT32_OK) {
			/* **The reason, not just the failure.** "It did not work"
			 * is precisely the message this whole file exists so that
			 * nobody has to guess at, and the filesystem already names
			 * every way it can refuse. */
			kprintf("  boot log     : %s is ours and the write failed: "
				"%s\n", dev->name, fat32_strerror(st));
			continue;
		}

		kprintf("  boot log     : %u bytes to %s:\\%s%s\n",
			(unsigned)held, dev->name, KLOG_NAME,
			klog_wrapped() ? " (oldest lines lost to the ring)" : "");
		kfree(buf);
		return;
	}

	/* Said plainly. A boot that could not save its report and did not
	 * mention it is a stick somebody pulls out expecting a file. */
	kputs("  boot log     : no ReconOS medium is writable here, so it "
	      "stayed in memory\n");
	kfree(buf);
}
