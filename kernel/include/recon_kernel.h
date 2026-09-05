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
 * them. Partition tables are read by the caller, not here: GPT and MBR are
 * data formats, and a kernel that parses them has taken on a parser it did not
 * need to. What the kernel owes is sectors.
 */

struct recon_k_block {
	uint64_t id;			/* also a device id, above */
	char name[RECON_K_NAME_MAX];

	uint64_t sector_count;
	uint32_t sector_size;		/* 512 or 4096, and it matters which */

	bool removable;
	bool read_only;
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
