/*
 * What the kernel offers the system above it.
 *
 * --- Status ---
 *
 * Every function here is a stub. They compile, they link, and they all say
 * "not implemented" -- deliberately, and the kernel version does not move for
 * this file, because the version says what works and stubs work at nothing.
 *
 * They exist so the desktop can be written against a settled interface instead
 * of waiting, and so that the shape of each call is argued about now, while
 * changing it costs a diff rather than a migration. An interface agreed before
 * either side has code that assumes the answer is the cheapest interface there
 * will ever be.
 *
 * --- Rules that hold for everything below ---
 *
 * NOTHING HERE ALLOCATES. The caller owns every buffer and says how large it
 * is. A kernel that allocates on behalf of a caller has to decide what to do
 * when it cannot, in the middle of an operation the caller has already begun.
 *
 * EVERY CALL RETURNS A STATUS, never a bool. "Failed" is not an answer a
 * settings page can render: no such device, a device that cannot do it, and a
 * device that refused are three different sentences to a person, and
 * collapsing them into false throws away the only thing that made the message
 * useful. recon_display.h already reached this conclusion independently.
 *
 * ENUMERATION IS BY IDENTITY, NOT POSITION. Every enumerable thing has an id
 * that stays with it, and every listing carries a generation number that
 * changes when the set changes. Indices are for walking a list; ids are for
 * acting on what you found. Hardware appears and disappears while software is
 * looking at it, and an index-addressed "set the mode on display 1" applied
 * after a monitor was unplugged does something visible and wrong.
 *
 * NO CALLBACKS INTO THE KERNEL. Anything asynchronous reports by leaving a
 * result to be collected, the way recon_net does with reachability, because a
 * callback is a promise about which stack you are on.
 *
 * --- Error codes ---
 *
 * The kernel claims area letter 'N' in recon_errors.def (A-H and J-M were
 * taken, I and O are never used). The statuses below are the kernel's own
 * narrow vocabulary; mapping them to VT-N### codes happens at the boundary,
 * where there is a person to show them to.
 */

#ifndef RECON_KERNEL_H
#define RECON_KERNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- Status ------------------------------------------------------------- */

typedef enum {
	RECON_K_OK = 0,

	RECON_K_ENOSYS,		/* not implemented yet -- every stub returns this */
	RECON_K_ENODEV,		/* no such device, or it went away */
	RECON_K_ENOTSUP,	/* the device exists and cannot do that */
	RECON_K_EINVAL,		/* the request does not make sense */
	RECON_K_EPERM,		/* the caller may not */
	RECON_K_EBUSY,		/* in use, try later */
	RECON_K_EIO,		/* the hardware failed */
	RECON_K_ERANGE,		/* the caller's buffer is too small */
	RECON_K_ESTALE,		/* the generation moved: enumerate again */
} recon_status;

const char *recon_kernel_status_text(recon_status s);

/* True when the kernel underneath is ReconOS's own rather than a stub over
 * Linux. The one call that is honest today, and the one the About page should
 * ask before claiming anything. */
bool recon_kernel_present(void);

/* --- Devices ------------------------------------------------------------
 *
 * Unblocks Device Manager. What a driver *is* -- the .rts module ABI -- is not
 * settled here: that gets shown before anything loads one, and its version
 * field is read before any other field, so a mismatched module is rejected
 * without its layout ever being interpreted. Its version is its own, distinct
 * from RECON_MODULE_ABI, so the existing module gate keeps meaning what it
 * means.
 */

#define RECON_K_NAME_MAX 64

typedef enum {
	RECON_K_DEV_OTHER = 0,
	RECON_K_DEV_STORAGE,
	RECON_K_DEV_DISPLAY,
	RECON_K_DEV_INPUT,
	RECON_K_DEV_NETWORK,
	RECON_K_DEV_BUS,	/* a thing other devices hang off */
	RECON_K_DEV_SYSTEM,	/* timers, interrupt controllers, firmware */
} recon_k_device_class;

struct recon_k_device {
	uint64_t id;			/* stable while the device is present */
	uint64_t parent;		/* 0 when it hangs off nothing */

	char name[RECON_K_NAME_MAX];	/* what it calls itself */
	char driver[RECON_K_NAME_MAX];	/* the module bound to it; empty if none */

	recon_k_device_class kind;
	bool bound;			/* a driver is attached and working */
	bool present;			/* still physically there */
};

/* Changes whenever a device appears or disappears. Read it before a listing
 * and again after; if it moved, the listing was torn and should be retaken. */
recon_status recon_kernel_device_generation(uint64_t *out);

recon_status recon_kernel_device_count(size_t *out);
recon_status recon_kernel_device_at(size_t index, struct recon_k_device *out);
recon_status recon_kernel_device_by_id(uint64_t id, struct recon_k_device *out);

/* --- Block devices ------------------------------------------------------
 *
 * Unblocks real partitions and formatting, and disk encryption underneath
 * them.
 *
 * Partition tables are read here, and only for their geometry.
 *
 * The rule this replaces -- that GPT and MBR are data formats and a kernel that
 * parses them has taken on a parser it did not need to -- was written before
 * there was a block layer, and the repository had already argued with it. The
 * kernel parses a device tree because it has to find its own memory, and ACPI
 * tables because it has to find its own bus. Where the root filesystem begins
 * is that same kind of question: one that must be answered before anything
 * exists above the kernel to answer it. So the rule is narrowed rather than
 * kept -- the kernel parses what it must to answer a question it has to answer
 * first, and nothing else.
 *
 * The line inside the format is drawn where the format stops changing. The
 * kernel reads the fixed-width integers that say *where sectors are*: the
 * first and last block of an entry, how many entries and how far apart, where
 * the two copies of the header live. Every disk already written froze those and
 * they cannot move. It does not read type identifiers, partition names or
 * attribute bits, because those are specified to grow, and a kernel you have to
 * ship again to recognise a new partition type is the wrong shape.
 *
 * The test for any field a later change wants to add: can somebody change what
 * this field *means* without changing the format? If yes, it belongs to the
 * caller. Apple's partition map is entirely the caller's and stays there, which
 * is what makes this a seam rather than a rebrand.
 *
 * What the kernel buys with it is a bound it can enforce. A partition appears
 * below as a block device of its own, with its own sector count and a parent,
 * and a caller holding one cannot address a sector outside it -- refused by the
 * same check that already refuses a read past the end of a disk. A bound
 * computed above the kernel and passed down is arithmetic the kernel cannot
 * verify, on the one call where getting it wrong writes into somebody else's
 * filesystem and reports success.
 *
 * What the caller owns is everything the table means, and every byte of one
 * being written. The kernel composes no partition table. It reads one for its
 * geometry, it will check a layout it is shown before that layout is written,
 * and it refuses a write to a disk that has partitions on it until the caller
 * has said out loud that it means to rewrite the disk. It will not choose a
 * layout, and there is no flag to turn the refusal off.
 *
 * The cost, said plainly: this is a parser running with kernel privilege over
 * numbers chosen by whoever last wrote the disk, which on the machines this
 * exists for is Windows. So it allocates nothing on the parse path it can
 * avoid, it never writes, every field is bounded before the field after it is
 * read, every loop is capped, and it is tested against disks partitioned by
 * tools that share no code with it.
 */

struct recon_k_block {
	uint64_t id;			/* also a device id, above */
	char name[RECON_K_NAME_MAX];

	uint64_t sector_count;
	uint32_t sector_size;		/* 512 or 4096, and it matters which */

	bool removable;
	bool read_only;

	/* Zero for a whole disk; otherwise the id of the disk this is a
	 * partition of. A partition is a device in this same list, and
	 * `sector_count` is the partition's length rather than the disk's. */
	uint64_t parent;

	/* Where it begins inside its parent. Informational: sector numbers
	 * passed to the calls below are relative to the id being addressed, so
	 * a caller holding a partition cannot reach outside it and does not
	 * have to add this itself. A partition editor that wants the whole disk
	 * addresses the parent, which is still in the list. */
	uint64_t first_sector;

	uint8_t slice_index;		/* as the table numbers it; 0 if whole */
	uint8_t scheme;			/* which kind of table it came from */
};

/* What kind of table was found on a disk. NONE and UNREADABLE are different
 * answers on purpose: a disk with no table is one an installer may use, and a
 * disk whose table cannot be believed is somebody's data with a damaged header,
 * which an installer must not touch. */
enum recon_k_scheme {
	RECON_K_SCHEME_NONE = 0,
	RECON_K_SCHEME_GPT,
	RECON_K_SCHEME_MBR,
	RECON_K_SCHEME_UNREADABLE,
};

recon_status recon_kernel_block_count(size_t *out);
recon_status recon_kernel_block_at(size_t index, struct recon_k_block *out);

/* Whole sectors only, and `buf` is at least count * sector_size. Partial
 * sector access is a convenience that hides which sectors were actually
 * touched, and the layer that wants bytes can do the arithmetic where it can
 * also see the file it is doing it for. */
recon_status recon_kernel_block_read(uint64_t id, uint64_t first_sector,
				     size_t count, void *buf, size_t buf_size);
recon_status recon_kernel_block_write(uint64_t id, uint64_t first_sector,
				      size_t count, const void *buf,
				      size_t buf_size);

/* Returns once the device says the data is on the medium, not once it has been
 * accepted. The distinction is the whole of what makes a filesystem
 * recoverable after power loss. */
recon_status recon_kernel_block_flush(uint64_t id);

/* --- Rewriting a disk ---------------------------------------------------
 *
 * A write to a disk that has partitions on it is refused until it is claimed.
 * Not because the caller is not trusted, but because "am I allowed to destroy
 * this disk" is a question with one right moment to ask it -- once, out loud,
 * at the top of an install -- and without this it is the ambient property of
 * every block device, asked never.
 *
 * Returns RECON_K_EBUSY if somebody already holds it.
 */
recon_status recon_kernel_block_claim_raw(uint64_t id);
recon_status recon_kernel_block_release_raw(uint64_t id);

/* Checks a whole intended layout before any of it is written: nothing overlaps,
 * everything is inside the disk, nothing lands where the kernel is working.
 *
 * It writes nothing and composes nothing -- what the layout should be is the
 * caller's decision. It exists because the check on a single write cannot catch
 * a plan that is internally inconsistent: four writes that each land on the
 * disk can still describe two partitions that overlap, and the disk they
 * overlap on has somebody's Windows installation on it.
 */
struct recon_k_extent {
	uint64_t first_sector;
	uint64_t sector_count;
};

recon_status recon_kernel_block_check_layout(uint64_t id,
					     const struct recon_k_extent *plan,
					     size_t count);

/* Reads the table again after it has been rewritten. Every id from before is
 * stale afterwards; that is what the generation counter above is for. */
recon_status recon_kernel_block_rescan(uint64_t id);

/* --- Time ---------------------------------------------------------------
 *
 * Two clocks, because they answer different questions and confusing them is a
 * classic fault. Monotonic never goes backwards and is what durations are
 * measured with. Wall clock is what a person reads, and it jumps -- at boot,
 * when a time source is believed, when somebody changes it.
 */

recon_status recon_kernel_time_monotonic_ns(uint64_t *out);
recon_status recon_kernel_time_wall_ns(uint64_t *out);	/* since 1970-01-01 UTC */

/* --- Randomness ---------------------------------------------------------
 *
 * A kernel service and not a library, because nothing above the kernel can
 * measure how good the machine's entropy is -- and a machine generating its
 * first long-lived private key is exactly where a weak source produces
 * quietly guessable keys that nothing ever notices.
 *
 * There is no non-blocking variant on purpose. A caller that would accept
 * "not enough entropy, here is some anyway" does not need this call.
 */

recon_status recon_kernel_random(void *buf, size_t len);

/* Whether the pool has been seeded from a real source yet. The honest answer
 * to "is it safe to generate a key now", and the reason it is a separate
 * question is that the answer is no for a while after boot. */
recon_status recon_kernel_random_ready(bool *out);

/* --- Power --------------------------------------------------------------
 *
 * Unblocks the Power page offering only the states the machine actually has,
 * rather than offering four and failing at two.
 */

typedef enum {
	RECON_K_POWER_SHUTDOWN = 0,
	RECON_K_POWER_RESTART,
	RECON_K_POWER_SUSPEND,		/* to RAM */
	RECON_K_POWER_HIBERNATE,	/* to disk */
} recon_k_power_state;

recon_status recon_kernel_power_supported(recon_k_power_state state,
					  bool *out);

/* Does not return when it succeeds. A status coming back is always a failure
 * to enter the state, which is why there is no success path to write. */
recon_status recon_kernel_power_enter(recon_k_power_state state);

/* --- Displays -----------------------------------------------------------
 *
 * Nothing here. The interface already exists as `recon_display_*` in
 * include/recon_display.h, owned by the desktop, and the kernel implements
 * that rather than inventing a second one for the same question. Two
 * interfaces for one thing is how a seam becomes a translation layer.
 *
 * See docs/KERNEL.md for the four changes worth making to it while it has one
 * implementation instead of two.
 */

/* --- Processes ----------------------------------------------------------
 *
 * Nothing here, and deliberately not stubbed.
 *
 * Applications are loaded with dlopen into the compositor's own address space,
 * so there is no boundary to write in advance: the boundary is an address
 * space and there is not one yet. Stubbing this would be pretending otherwise,
 * and the pretence would be discovered by whoever wrote code against it.
 *
 * What replaces it is settled at checkpoint 9, together with what the module
 * ABI becomes, and before either half has written code that assumes an answer.
 */

#endif /* RECON_KERNEL_H */
