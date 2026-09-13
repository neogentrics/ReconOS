/* Putting the bootloader and kernel onto the disk being installed to.
 *
 * Everything before this makes a disk *ready*: a table, an EFI filesystem, two
 * ReconOS volumes. None of it makes the machine boot. This is the step that
 * does, and it is a file copy between two FAT32 volumes -- the medium ReconOS
 * was booted from, and the EFI System Partition it is being installed onto.
 *
 * --- Where the files come from, and why not from the kernel image -----------
 *
 * The obvious idea is to carry the loader and the kernel inside the installer,
 * as data compiled in. It is wrong for a reason that only shows later: the
 * installer would then write *the bootloader it was built with*, and an install
 * medium is the one thing people keep for years. A stick made today would still
 * be writing today's loader in 2029.
 *
 * So they are copied from the medium in front of us. What gets installed is
 * whatever that medium carries, which is the same thing that just booted --
 * and if it booted, it works on this machine, which is a stronger guarantee
 * than any version check.
 *
 * --- What it refuses --------------------------------------------------------
 *
 * A file that is on the source and cannot be written to the target is a failed
 * install, not a warning. Half a bootloader on an EFI partition is worse than
 * none: with none, firmware moves on to the next entry and the machine still
 * starts.
 */
#include <recon/kernel/install.h>

#include <recon/kernel/block.h>
#include <recon/kernel/console.h>
#include <recon/kernel/fat32.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/kstring.h>

/* What to copy, expressed as rules rather than a list of names.
 *
 * The first version listed the files: BOOTX64.EFI, BOOTAA64.EFI,
 * kernel-x86_64.elf, kernel-aarch64.elf. `make check-portable` refused it, and
 * was right to -- but the real objection is not that the names are
 * machine-specific. It is that **a list of names silently stops being
 * complete.** The day this project gains a third architecture, an installer
 * holding that list would prepare a disk with no kernel on it for the machine
 * it was running on, and report success.
 *
 * So: everything in \EFI\BOOT that is a loader, and everything in \reconos
 * that is a kernel. A rule covers the architecture that does not exist yet.
 *
 * --- What is deliberately not copied ----------------------------------------
 *
 * Not the whole directory. An install medium carries things that are not part
 * of an install -- a firmware update somebody left on it, the NvVars file OVMF
 * writes into any ESP it boots from -- and copying wholesale puts them on
 * somebody's system partition.
 *
 * And **not the command line**, which is the important one. The medium's
 * \reconos\cmdline is what told this kernel to install; copying it to the
 * target would produce a machine that tries to install ReconOS every time it
 * boots, onto itself, forever. The file that makes an installer run is the one
 * file an installed system must not have.
 */
static bool ends_with_efi(const char *name)
{
	size_t n = kstrlen(name);
	const char *e;

	if (n < 4)
		return false;

	e = name + n - 4;
	return e[0] == '.' &&
	       (e[1] == 'E' || e[1] == 'e') &&
	       (e[2] == 'F' || e[2] == 'f') &&
	       (e[3] == 'I' || e[3] == 'i');
}

static bool looks_like_kernel(const char *name)
{
	static const char want[] = "kernel";
	unsigned i;

	for (i = 0; want[i]; i++) {
		char c = name[i];

		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
		if (c != want[i])
			return false;
	}

	/* "cmdline" does not begin with "kernel", so it is excluded by this
	 * rule already -- but the reason it must be is worth more than the
	 * accident that it is. See above. */
	return true;
}

/* One file, source to target, whole. Whole-file because that is what both
 * filesystems offer and what a bootloader is: there is no case here for
 * streaming, and a partial copy is exactly the outcome to avoid. */
static enum fat32_status copy_one(struct fat32 *src, struct fat32 *dst,
				  const char *dir, const char *name,
				  bool *copied)
{
	char path[FAT_NAME_MAX];
	struct fat32_entry e;
	enum fat32_status st;
	u32 dir_cluster = 0, got = 0;
	u8 *buf;
	unsigned n = 0;

	*copied = false;

	while (dir[n] && n + 1 < sizeof(path)) {
		path[n] = dir[n];
		n++;
	}
	path[n++] = '/';
	{
		unsigned k = 0;

		while (name[k] && n + 1 < sizeof(path))
			path[n++] = name[k++];
	}
	path[n] = '\0';

	st = fat32_walk(src, path, &e);
	if (st != FAT32_OK)
		return FAT32_ERR_NOT_FOUND;	/* absent is not a failure */

	if (e.is_dir || !e.size)
		return FAT32_ERR_NOT_FOUND;

	buf = kzalloc(e.size);
	if (!buf)
		return FAT32_ERR_NOMEM;

	st = fat32_read_file(src, &e, buf, e.size, &got);
	if (st != FAT32_OK || got != e.size) {
		kfree(buf);
		return st == FAT32_OK ? FAT32_ERR_IO : st;
	}

	st = fat32_mkpath(dst, dir, &dir_cluster);
	if (st != FAT32_OK) {
		kfree(buf);
		return st;
	}

	st = fat32_write_named(dst, dir_cluster, name, buf, e.size);
	kfree(buf);

	if (st == FAT32_OK)
		*copied = true;

	return st;
}

/* Copies every entry of one directory that a rule accepts. */
static enum install_verdict copy_matching(struct fat32 *src, struct fat32 *dst,
					  const char *dir,
					  bool (*wanted)(const char *),
					  unsigned *count)
{
	struct fat32_entry d;
	u32 src_dir = 0;
	unsigned i;

	if (fat32_walk(src, dir, &d) != FAT32_OK || !d.is_dir)
		return INSTALL_OK;		/* absent is not a failure */

	src_dir = d.first_cluster;

	for (i = 0; i < 64; i++) {
		struct fat32_entry e;
		bool did = false;
		enum fat32_status st;

		if (fat32_readdir(src, src_dir, i, &e) != FAT32_OK)
			break;

		if (e.is_dir || !wanted(e.name))
			continue;

		st = copy_one(src, dst, dir, e.name, &did);
		if (st != FAT32_OK && st != FAT32_ERR_NOT_FOUND) {
			kprintf("  bootloader         : FAILED (%s: %s)\n",
				e.name, fat32_strerror(st));
			return INSTALL_IO;
		}

		if (did)
			(*count)++;
	}

	return INSTALL_OK;
}

enum install_verdict install_copy_boot(struct block_device *source_esp,
				       struct block_device *target_esp)
{
	struct fat32 src, dst;
	unsigned loaders = 0, kernels = 0;
	enum install_verdict v;

	if (fat32_mount(source_esp, &src) != FAT32_OK) {
		kputs("  bootloader         : FAILED (the medium's EFI "
		      "partition could not be read)\n");
		return INSTALL_IO;
	}

	if (fat32_mount(target_esp, &dst) != FAT32_OK) {
		kputs("  bootloader         : FAILED (the target's EFI "
		      "partition could not be read)\n");
		return INSTALL_IO;
	}

	v = copy_matching(&src, &dst, "/EFI/BOOT", ends_with_efi, &loaders);
	if (v != INSTALL_OK)
		return v;

	v = copy_matching(&src, &dst, "/reconos", looks_like_kernel, &kernels);
	if (v != INSTALL_OK)
		return v;

	/* A disk with a kernel and no loader will not boot, and neither will
	 * one with a loader and no kernel. Either alone is a failed install,
	 * said so rather than reported as a count of files copied. */
	if (!loaders || !kernels) {
		kprintf("  bootloader         : FAILED (%s on the install "
			"medium)\n",
			loaders ? "loaders but no kernel"
				: "a kernel but no loader");
		return INSTALL_IO;
	}

	kprintf("  bootloader         : %u loader%s and %u kernel%s copied "
		"from the medium\n",
		loaders, loaders == 1 ? "" : "s",
		kernels, kernels == 1 ? "" : "s");
	return INSTALL_OK;
}

/* The EFI System Partition of the medium this kernel was booted from.
 *
 * Found by looking rather than by being told: the partition that holds
 * \EFI\BOOT and is not the one being installed to. Being told would be a flag
 * somebody has to get right, and getting it wrong means installing a bootloader
 * from a disk that is about to be overwritten.
 */
struct block_device *install_find_medium(struct block_device *exclude_disk)
{
	unsigned i;

	for (i = 0; i < block_device_count(); i++) {
		struct block_device *d = block_device_at(i);
		struct fat32 fs;
		struct fat32_entry e;

		if (!d->parent || (exclude_disk && d->parent == exclude_disk->id))
			continue;

		if (fat32_mount(d, &fs) != FAT32_OK)
			continue;

		if (fat32_walk(&fs, "/EFI/BOOT", &e) == FAT32_OK && e.is_dir)
			return d;
	}

	return 0;
}
