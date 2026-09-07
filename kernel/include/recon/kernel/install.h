/* The installer, and the half of it that decides before anything is written.
 *
 * Everything before this checkpoint was a component with a self-test: it works
 * or it does not, on a machine that exists to be experimented on. **An
 * installer is a sequence that runs once, on a stranger's machine, with their
 * data on it.** There is no second attempt and no undo, and the thing it is
 * most likely to destroy is the operating system somebody is currently using.
 *
 * So it is built in two halves that can be trusted separately:
 *
 *   1. **The plan.** Reads the disk, works out what it *would* do, and returns
 *      it. Writes nothing. Every decision, every refusal and every number in
 *      this file is reachable by a test that risks no data at all, and that is
 *      the point -- the hard part of an installer is deciding, and the hard
 *      part of testing one is that trying it is expensive.
 *
 *   2. The execution, which does exactly what the plan says and nothing it
 *      worked out for itself.
 *
 * A plan is also the thing a person should be shown before they agree. "This
 * will create two partitions in the 40 GB of free space and leave your three
 * existing ones alone" is a sentence somebody can check. "Installing..." is not.
 *
 * --- Where the type GUIDs are read, and why here ----------------------------
 *
 * <recon/kernel/partition.h> deliberately reads geometry and nothing else --
 * where sectors are, which every written disk froze -- because type GUIDs and
 * attributes are specified to *grow*, and a kernel you must ship again to
 * recognise a new partition type is the wrong shape. Its own rule says those
 * belong to the caller.
 *
 * The installer is that caller. It reads the GPT itself for the one type it
 * has to recognise -- the EFI System Partition -- which keeps the seam where
 * that file put it rather than widening the kernel's parser to suit one user.
 */
#ifndef RECON_KERNEL_INSTALL_H
#define RECON_KERNEL_INSTALL_H

#include <recon/kernel/block.h>
#include <recon/kernel/types.h>

/* Sizes, in blocks of 512, gathered here because every one of them is a
 * judgement somebody may want to argue with and none should be buried in a
 * condition halfway down a function. */
#define INSTALL_ALIGN_BLOCKS	2048u		/* 1 MiB, what every tool aligns to */
#define INSTALL_ESP_CREATE	(256u * 2048u)	/* 256 MiB when we make one */
#define INSTALL_ESP_MIN_FREE	(16u * 2048u)	/* 16 MiB, to reuse somebody else's */

/* --- Three partitions, and why applications get one of their own -----------
 *
 * The EFI System Partition holds the bootloader and the kernel. It is FAT32
 * because firmware can read nothing else, and the kernel lives there for a
 * reason worth stating: if it lived on the ReconOS volume, the *bootloader*
 * would have to understand ReconFS in order to load the thing that understands
 * ReconFS. Keeping both on the ESP stops that dependency from existing.
 *
 * Then the system, and then **applications, separately**.
 *
 * That last split is not tidiness. ReconOS is meant to run other systems'
 * programs -- old Windows software, Linux binaries -- through compatibility
 * layers, and a compatibility layer running somebody's twenty-year-old
 * installer is the least trustworthy code the machine will ever execute. Giving
 * it a different volume makes "a bad application cannot break the operating
 * system" a property of the layout rather than a promise made by code that
 * could have a bug in it.
 *
 * It also gives each subsystem somewhere to live: DOS-era, Win32, NT, Linux,
 * and ReconOS's own, each with its own tree, on a volume that can be wiped
 * without reinstalling anything.
 */
#define INSTALL_SYSTEM_MIN	(2048u * 2048u)	/* 2 GiB, the floor for ReconOS */
#define INSTALL_PROGRAMS_MIN	(1024u * 2048u)	/* 1 GiB, the floor for programs */

/* How the space left over is divided. The system does not grow with the disk
 * the way installed software does -- an operating system is roughly a fixed
 * size and a library of applications is not -- so the system takes a quarter,
 * bounded at both ends, and programs take the rest. */
#define INSTALL_SYSTEM_SHARE	4u		/* a quarter of what is available */
#define INSTALL_SYSTEM_CAP	(65536u * 2048u)	/* 64 GiB is plenty for an OS */

enum install_verdict {
	INSTALL_OK = 0,

	/* Each refusal is its own value, and they print differently, because
	 * "there is no room" and "I could not read your partition table" call
	 * for opposite responses from the person reading it. */
	INSTALL_NO_TABLE_READABLE,	/* a table is there and cannot be believed */
	INSTALL_NOT_GPT,		/* MBR: possible, deliberately not yet */
	INSTALL_NO_ROOM_SYSTEM,
	INSTALL_NO_ROOM_PROGRAMS,
	INSTALL_NO_ROOM_ESP,
	INSTALL_ESP_TOO_FULL,		/* somebody else's ESP, with no space left */
	INSTALL_TOO_MANY_SLICES,
	INSTALL_IO,
};

const char *install_verdict_name(enum install_verdict v);

struct install_plan {
	struct block_device *disk;

	enum install_verdict verdict;

	/* True when the disk had no table at all and one will be created. A
	 * disk with a table is never re-tabled: every existing partition is
	 * left exactly where it is, and ReconOS goes in the space between. */
	bool fresh_table;

	/* An EFI System Partition that is already there and has room. Reused
	 * rather than duplicated -- a second ESP on a machine that has one is
	 * how a dual-boot install stops booting, because which one firmware
	 * picks is not something this code gets to decide. */
	bool reuse_esp;
	u8   reuse_esp_index;
	u64  reuse_esp_free_blocks;

	struct block_extent esp;	/* created when !reuse_esp */
	struct block_extent system;
	struct block_extent programs;

	/* What is being left alone, which is the number a person actually wants
	 * to see before agreeing to any of this. */
	unsigned untouched_slices;
	u64      untouched_blocks;
};

/* Works out what installing on `disk` would do. Reads; writes nothing.
 *
 * `disk` must be a whole device, not a slice. The plan's verdict says whether
 * it can be done, and every field below it is only meaningful when the verdict
 * is INSTALL_OK.
 */
enum install_verdict install_plan(struct block_device *disk,
				  struct install_plan *out);

/* Adds this plan's partitions to the disk's GPT.
 *
 * **Every entry already in the table is preserved byte for byte** -- not
 * rewritten identically, not regenerated from the fields this code
 * understands, but left alone. The fields it does not understand are exactly
 * the ones that would go missing, and they would go missing precisely because
 * it did not know to look for them.
 *
 * Claims the device raw for the duration, so a disk something else is using
 * is refused rather than shared. */
enum install_verdict install_write_gpt(struct block_device *disk,
				       const struct install_plan *plan);

/* Prints a plan the way a person should see it before agreeing: what will be
 * created, what will be reused, and what will be left alone. */
void install_print_plan(const struct install_plan *plan);

/* Does exactly what a plan says: writes the table, makes the EFI filesystem
 * when one had to be created, and lays ReconFS on the two ReconOS volumes.
 *
 * Re-plans first and refuses if the answer changed. A plan is a set of
 * numbers that were true when the disk was read, and between then and here a
 * person had time to agree to it -- time in which a removable disk can be
 * pulled out and a different one pushed in under the same name. */
enum install_verdict install_execute(struct block_device *disk,
				     const struct install_plan *plan);

/* Plans and executes against the disk named by `install-onto=<device>`. */
void install_execute_run(void);

/* Copies the bootloader and kernels from the medium ReconOS booted from onto
 * the EFI partition it is installing to. What gets installed is whatever that
 * medium carries -- which is the same thing that just booted, and if it
 * booted, it works on this machine.
 *
 * A loader with no kernel, or a kernel with no loader, is a failed install
 * rather than a count of files copied. */
enum install_verdict install_copy_boot(struct block_device *source_esp,
				       struct block_device *target_esp);

/* The EFI partition of the medium this kernel was booted from, found by
 * looking for one holding \EFI\BOOT that is not on the disk being installed
 * to. Found rather than named: a flag somebody has to get right, gotten
 * wrong, means copying a bootloader off a disk that is about to be
 * overwritten. */
struct block_device *install_find_medium(struct block_device *exclude_disk);

/* Plans an install against every disk the block layer can see and prints the
 * result. Reads only; there is no execution behind it yet. Runs when
 * `install-plan` appears on the command line. */
void install_plan_run(void);

/* The recovery environment: reports what the machine looks like when it will
 * not start, and writes nothing. Runs when `recovery` is on the command line.
 *
 * It is this same kernel rather than a second one, booted from the EFI
 * partition -- a recovery environment must not depend on the thing it
 * repairs, and a separate recovery build is a second thing to keep working
 * whose one moment of use is the moment nobody has been testing it. */
void recovery_run(void);

#endif /* RECON_KERNEL_INSTALL_H */
