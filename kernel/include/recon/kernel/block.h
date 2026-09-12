/* Block devices: storage the kernel can read and write.
 *
 * This is the narrowest of the four storage problems, and keeping it narrow is
 * the point. A block device answers exactly one question -- "give me these
 * sectors" and "put these sectors there" -- and knows nothing about partitions,
 * filesystems, files or names. Every one of those is a data format that lives
 * above this line, which is the same line THIRD_PARTY.md already draws for PNG
 * and TLS: the kernel owes sectors, and what the sectors mean is somebody
 * else's job.
 *
 * The line matters because conflating the four is how this work looks
 * impossible. A driver that knows what a partition table is has to be rewritten
 * when a new one appears; a driver that hands back sector 0 does not care.
 *
 * --- Addressing ---
 *
 * Everything is in *logical blocks*, numbered from zero, of `block_size` bytes.
 * Not "sectors", because that word means 512 bytes to some hardware and 4096 to
 * other hardware and the difference is exactly the kind of thing that is
 * assumed once and wrong forever. The device says how big its blocks are and
 * the caller multiplies.
 *
 * --- Synchronous, for now, and it says so ---
 *
 * block_read() and block_write() do not return until the transfer is done or
 * has failed. That is the right shape for everything this kernel does today --
 * the installer reads a partition table, the filesystem reads an inode -- and
 * it is the wrong shape for a disk with a queue depth of thirty-two.
 *
 * It is written this way because the alternative needs something that does not
 * exist yet: a thread that can *wait*. The scheduler has no blocking, so a
 * caller waiting on a disk yields in a loop, which burns a slice per poll.
 * When there is a wait queue, this interface does not change -- the loop inside
 * it does.
 */
#ifndef RECON_KERNEL_BLOCK_H
#define RECON_KERNEL_BLOCK_H

#include <recon/kernel/types.h>
#include <recon/kernel/wait.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/pmm.h>

#define BLOCK_NAME_MAX 24

/* Sixty-four, because a partition is a block device here and a machine with
 * four disks of eight partitions is ordinary. Eight was enough while a device
 * meant a whole disk and is not any more. */
#define BLOCK_MAX_DEVICES 64

/* Refused loudly at the cap, never truncated. A slice the kernel cannot see is
 * a partition it cannot protect from a whole-device write, so silently dropping
 * the sixteenth is the one failure that turns a safety check into a hazard. */
#define BLOCK_MAX_SLICES 16

/* Why a request failed. A status rather than a bool, for the same reason the
 * desktop-facing API takes one: "it did not work" is not a fault report, and
 * the difference between "there is no such block" and "the hardware said no"
 * is the difference between a bug and a dying disk. */
enum block_status {
	BLOCK_OK = 0,
	BLOCK_ERR_NO_DEVICE,	/* nothing is registered under that identity */
	BLOCK_ERR_RANGE,	/* past the end of the device, or wrapped */
	BLOCK_ERR_READ_ONLY,	/* the device refuses writes, and says so early */
	BLOCK_ERR_ALIGN,	/* the buffer is not where the hardware can reach */
	BLOCK_ERR_IO,		/* the hardware tried and failed */
	BLOCK_ERR_TIMEOUT,	/* the hardware did not answer */
	BLOCK_ERR_BUSY,		/* no room in the queue right now */
	BLOCK_ERR_UNSUPPORTED,	/* the device does not offer this operation --
				 * not a failure, and a caller must not treat it
				 * as one */
};

const char *block_status_name(enum block_status s);

/* What kind of table a device's first sectors turned out to hold.
 *
 * NONE and UNREADABLE are deliberately different values and print as different
 * lines. "There is no table here" and "there is a table here and it cannot be
 * believed" call for opposite responses -- the first disk is a blank one an
 * installer may use, and the second is somebody's data with a damaged header,
 * which an installer must not touch. Collapsing them is how a corrupt GPT gets
 * treated as an empty disk. */
enum block_scheme {
	BLOCK_SCHEME_NONE = 0,	/* looked, and there was no table */
	BLOCK_SCHEME_GPT,
	BLOCK_SCHEME_MBR,
	BLOCK_SCHEME_UNREADABLE,/* a table was there and did not check out */
};

const char *block_scheme_name(enum block_scheme s);

struct block_device;

/* What a driver provides. Three operations, and flush is not optional.
 *
 * A disk that reports a write complete when it is still in a volatile cache is
 * doing the normal thing, and a filesystem that expects power loss to be
 * survivable has to be able to say "and mean it". Leaving flush out would make
 * every ordering guarantee above this line a fiction, so it is here from the
 * first driver rather than added once something has already been lost. */
/* One thing somebody wants done to a disk.
 *
 * On the requester's own stack, because it lives exactly as long as the wait
 * for it -- and an allocation here would be a read that fails when memory is
 * short, which is when reads matter most. The same argument that put swap on a
 * partition.
 */
struct block_request {
	struct block_request *next;

	u64 lba;
	u32 count;
	void *buf;			/* const for a write; see submit */
	bool write;

	volatile bool done;
	volatile enum block_status status;
};

struct block_ops {
	enum block_status (*read)(struct block_device *dev, u64 lba,
				  u32 count, void *buf);
	enum block_status (*write)(struct block_device *dev, u64 lba,
				   u32 count, const void *buf);
	enum block_status (*flush)(struct block_device *dev);

	/* Optional. A null pointer means the device does not offer it, which is
	 * reported as BLOCK_ERR_UNSUPPORTED rather than as success -- a caller
	 * told a discard succeeded when nothing was issued would draw exactly
	 * the wrong conclusion about the drive's state, which is the mistake
	 * virtio-blk's flush used to make. */
	enum block_status (*discard)(struct block_device *dev, u64 lba,
				     u32 count);
};

struct block_device {
	char name[BLOCK_NAME_MAX];	/* "virtio0", "nvme0n1" -- for people */

	/* Identity that survives the device disappearing and coming back. The
	 * generation counter is the same idea as the desktop-facing API's:
	 * hardware appears and vanishes while software is looking at it, and an
	 * index alone silently becomes a different disk. */
	u32 id;
	u32 generation;

	u32 block_size;		/* bytes per logical block; a power of two */
	u64 block_count;	/* how many there are */

	bool removable;
	bool read_only;

	/* Whether block_flush on this device actually reaches the medium.
	 *
	 * It is not always true, and the case where it is false used to be
	 * invisible: virtio-blk returns success from flush without issuing
	 * anything when the device did not offer the flush feature, and no
	 * caller could tell that apart from a flush that happened.
	 *
	 * The comment that used to sit there said a device without the feature
	 * "has nothing volatile to flush, so success is the true answer". That
	 * is an assumption about the device, not something the kernel checked --
	 * the specification says only that the driver must not send a flush, not
	 * that the device has no cache. A durability promise resting on an
	 * assumption is not a durability promise.
	 *
	 * So the fact is recorded instead of assumed. flush still returns
	 * success, because there is nothing better to do; what changes is that
	 * anything building a crash-recoverable structure on top can ask, and
	 * refuse to promise what this device cannot deliver. */
	bool flush_is_durable;

	/* --- What kind of medium this is, so callers can stop guessing -------
	 *
	 * A filesystem's allocator has one decision that depends entirely on the
	 * medium: whether it may put a file's blocks wherever there is room, or
	 * must work to keep them together.
	 *
	 * On solid state a seek costs nothing and scattering is free. On a
	 * spinning disk a seek costs milliseconds -- five orders of magnitude
	 * more than the transfer -- and a file scattered across the platter reads
	 * at a fraction of the drive's sequential speed. Copy-on-write scatters
	 * by nature, so for ReconFS this is not a tuning detail: it is the
	 * difference between a filesystem that is pleasant on a hard disk and one
	 * that is unusable on it.
	 *
	 * `seek_is_free` defaults to false, which is the safe direction. Treating
	 * an SSD as a disk costs a little allocator effort and nothing else;
	 * treating a disk as an SSD fragments it and cannot be undone without
	 * rewriting the volume.
	 *
	 * **This is two states standing in for three, and the third is real.**
	 * Every driver that sets it today can actually answer -- NVMe because
	 * everything on it is flash, ATA because IDENTIFY word 217 says. USB
	 * mass storage is SCSI in a wrapper and has neither, so it cannot answer
	 * at all, and the default then reports *rotating* for an external SSD
	 * rather than reporting that it does not know. That costs nothing here,
	 * where the field only steers the allocator -- but the same not-knowing
	 * governs discard, and a drive never told about freed space wears out
	 * faster and slows down over months with every line of code correct.
	 *
	 * Measured, not supposed: Linux reports `rotational: 1` for a USB flash
	 * drive, for exactly this reason. Checkpoint 11b must read the SCSI Block
	 * Device Characteristics VPD page, where a medium rotation rate of 1
	 * means non-rotating, rather than inferring anything from the bus.
	 * (BG-127) */
	bool seek_is_free;

	/* The device wants to be told when a block stops being in use, so it can
	 * stop preserving its contents. On an SSD this is what keeps write
	 * amplification down as the drive fills; a drive never told about freed
	 * space eventually behaves as though it is full even when it is not. */
	bool discard_supported;

	/* Bytes the device would rather move in one request. Advisory, and zero
	 * means it did not say -- which is different from "it said zero", and is
	 * why this is not a defaulted value. */
	u32 transfer_hint;

	/* Held for the length of one request. See block.c: not a spinlock,
	 * because every driver polls with sched_yield() and a spinlock held
	 * across a yield is a deadlock. */
	volatile int busy;

	/* --- what is waiting, and who is serving it ---------------------
	 *
	 * Requests queue here rather than fighting over `busy`, and whoever
	 * arrives at an idle device becomes the one that drains them -- see
	 * block.c. There is no worker thread: a thread that is already waiting
	 * for a disk is the right thread to drive it.
	 */
	struct block_request *queue;	/* in the order the head will take them */
	struct wait_queue waiters;
	struct spinlock queue_lock;
	bool serving;

	u64 queued, merged, reordered;

	/* --- Slices ------------------------------------------------------
	 *
	 * A partition is a block device with a parent and an offset, in the
	 * same registry as the disk it lives on, addressed from its own zero.
	 * Not a separate kind of object: a filesystem should not care whether
	 * it was given a disk or a piece of one, and an encrypted volume
	 * should be able to be a child of a partition without anything above
	 * it changing.
	 *
	 * `block_count` on a slice is the *partition's* length. That one fact
	 * is what makes the bound free: the range check that already refuses a
	 * read past the end of a disk refuses a read past the end of a
	 * partition, in the same line, without knowing the difference. */
	u32 parent;		/* 0 for a whole device; else the parent's id */
	u32 parent_generation;
	u64 first_lba;		/* where this slice starts inside its parent */
	u8  slice_index;	/* 1-based, as the table numbers it; 0 if whole */
	u8  scheme;		/* enum block_scheme: the table this came from */
	unsigned slice_count;	/* how many children are registered under this */

	/* Somebody has said out loud that they intend to rewrite this whole
	 * device. Without it, a write to a disk that has partitions on it is
	 * refused -- see block_write. */
	bool claimed_raw;

	/* What the device can accept in one request. Zero means "no opinion",
	 * and the block layer will not split. */
	u32 max_blocks_per_request;

	const struct block_ops *ops;
	void *driver;		/* the driver's own state */

	bool present;
};

/* Called by a driver once it has a working device. Copies nothing that the
 * driver must keep alive except `ops` and `driver`, both of which the driver
 * owns for the life of the device. Returns null when there is no room. */
struct block_device *block_register(const char *name, const struct block_ops *ops,
				    void *driver, u32 block_size, u64 block_count);

/* Registers one partition of `parent` as a device of its own. Called by the
 * partition reader; drivers never call it. Returns null at the cap or on a
 * range that does not fit inside the parent. */
struct block_device *block_register_slice(struct block_device *parent,
					  u8 index, u64 first_lba, u64 count,
					  enum block_scheme scheme);

/* Says the device is gone.
 *
 * The structure is **not** freed and the slot is not reused. That is the whole
 * point of the generation counter beside the id: anything still holding this
 * device asks for it by identity, and an identity that has been retired answers
 * null rather than answering with whatever was plugged in afterwards. Freeing
 * the slot would make the second disk indistinguishable from the first.
 *
 * Every partition registered under it goes with it, because a slice of a disk
 * that is no longer there is not a smaller disk that still is. The buffer cache
 * drops what it held, because those blocks now describe a device that cannot be
 * asked to confirm them.
 *
 * After this, every operation on the device refuses -- which is not new code:
 * `present` has been checked at the top of read, write, flush, discard, resolve
 * and claim since they were written. This is the function that sets it. */
void block_unregister(struct block_device *dev);

/* By index, for enumeration, and by identity, for holding on to one. */
unsigned block_device_count(void);
struct block_device *block_device_at(unsigned index);
struct block_device *block_device_by_id(u32 id, u32 generation);

/* The transfers themselves. `buf` must be `count * block_size` bytes, and must
 * be memory the hardware can reach -- see BLOCK_ERR_ALIGN. */
enum block_status block_read(struct block_device *dev, u64 lba, u32 count, void *buf);
enum block_status block_write(struct block_device *dev, u64 lba, u32 count,
			      const void *buf);
enum block_status block_flush(struct block_device *dev);

/* Which disk, and which block of it, a request actually names.
 *
 * A partition and the disk it lives on are the same sectors under two
 * names, and anything holding on to blocks -- a cache, today -- has to be
 * able to tell that. Exported so there is one implementation of the
 * translation rather than two that can disagree.
 *
 * Checks the bound against `dev` before translating, so a caller cannot use
 * this to learn the absolute address of a block outside the slice it was
 * given. */
enum block_status block_resolve(struct block_device *dev, u64 lba, u32 count,
				u32 *root_id, u32 *root_generation,
				u64 *abs_lba);

/* Tells the device that a range of blocks no longer holds anything anyone
 * wants. Advisory in both directions: a device may ignore it, and a caller
 * must never rely on the blocks reading back as anything in particular
 * afterwards -- some devices return zeroes, some return the old contents, and
 * the specification permits both.
 *
 * Returns BLOCK_ERR_UNSUPPORTED on a device that does not offer it, which is
 * not a failure and callers should not treat it as one. */
enum block_status block_discard(struct block_device *dev, u64 lba, u32 count);

/* Whether a flush on this device reaches the medium, or merely succeeds.
 *
 * A filesystem that promises to survive power loss must ask, and must refuse
 * to make that promise where the answer is no. Asking is cheap; finding out
 * afterwards is somebody's data. */
bool block_flush_is_durable(const struct block_device *dev);

/* --- Rewriting a disk, which is the dangerous direction ---------------------
 *
 * A write to a device that has partitions on it is refused unless the caller
 * has claimed it. That is not paranoia about the caller; it is about *which
 * question gets asked*. "Am I allowed to destroy this disk" is a question with
 * one right moment to ask it -- once, out loud, at the top of an install -- and
 * today it is the ambient property of every block device, asked never.
 *
 * There is no flag to turn the refusal off, in keeping with the rule this
 * project already follows about safety checks. A caller that means it claims
 * the device; a caller that does not mean it gets an error it can read. */
enum block_status block_claim_raw(struct block_device *dev);
void block_release_raw(struct block_device *dev);

/* One extent of an intended layout, in the parent device's own blocks. */
struct block_extent {
	u64 first_lba;
	u64 count;
};

/* Checks a whole intended layout before any of it is written: nothing overlaps,
 * everything is inside the device, nothing lands on a slice the kernel is
 * using. Writes nothing and composes nothing -- the caller decides the layout,
 * and this is the arithmetic being checked by something that can see the disk.
 *
 * It exists because the range check on a single write cannot catch a plan that
 * is internally inconsistent: four writes that each land on the disk can still
 * describe two partitions that overlap, and the disk they overlap on has
 * somebody's Windows installation on it. */
enum block_status block_check_layout(const struct block_device *dev,
				     const struct block_extent *plan, unsigned n);

void block_init(void);
void block_print_summary(void);
bool block_self_test(void);

/* That the queue comes out in an order, and that a device which says it does
 * not need one is left alone. Runs against a device that is not there, so it
 * needs no disk and reports the same thing on every machine. */
bool block_queue_test(void);

void block_print_traffic(void);

/* What the partition reader prints, and what the fixture harness compares
 * against: the scheme, how many slices, and one line of geometry each. Kept
 * separate from block_print_summary so that a machine-readable claim and a
 * human-readable one do not have to be the same string. */
void block_print_tables(void);

/* --- What a driver's transport must provide --------------------------------
 *
 * Called by the block layer's own initialisation, in order, so that a driver
 * does not have to know when it is safe to look. Each is a no-op on an
 * architecture that has no such bus. */
void arch_storage_probe(void);

/* --- Drivers ---------------------------------------------------------------
 *
 * Each takes something the architecture found and registers a block device if
 * it recognises it. Declared here rather than in a header of their own because
 * the list of them is the interesting thing, and it is short. */
struct pci_device;

bool nvme_attach(const struct pci_device *d);
bool ahci_attach(const struct pci_device *d);

/* One line saying how the architecture went looking, printed above the devices
 * it found. Worth its own hook because "no devices" has two very different
 * causes -- nothing was plugged in, or nothing was looked at -- and a summary
 * that cannot tell them apart sends people to read the wrong file. */
void arch_storage_print(void);

#endif /* RECON_KERNEL_BLOCK_H */
