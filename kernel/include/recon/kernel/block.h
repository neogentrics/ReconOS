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
#include <recon/kernel/pmm.h>

#define BLOCK_NAME_MAX 24
#define BLOCK_MAX_DEVICES 8

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
};

const char *block_status_name(enum block_status s);

struct block_device;

/* What a driver provides. Three operations, and flush is not optional.
 *
 * A disk that reports a write complete when it is still in a volatile cache is
 * doing the normal thing, and a filesystem that expects power loss to be
 * survivable has to be able to say "and mean it". Leaving flush out would make
 * every ordering guarantee above this line a fiction, so it is here from the
 * first driver rather than added once something has already been lost. */
struct block_ops {
	enum block_status (*read)(struct block_device *dev, u64 lba,
				  u32 count, void *buf);
	enum block_status (*write)(struct block_device *dev, u64 lba,
				   u32 count, const void *buf);
	enum block_status (*flush)(struct block_device *dev);
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

void block_init(void);
void block_print_summary(void);
bool block_self_test(void);

/* --- What a driver's transport must provide --------------------------------
 *
 * Called by the block layer's own initialisation, in order, so that a driver
 * does not have to know when it is safe to look. Each is a no-op on an
 * architecture that has no such bus. */
void arch_storage_probe(void);

/* One line saying how the architecture went looking, printed above the devices
 * it found. Worth its own hook because "no devices" has two very different
 * causes -- nothing was plugged in, or nothing was looked at -- and a summary
 * that cannot tell them apart sends people to read the wrong file. */
void arch_storage_print(void);

#endif /* RECON_KERNEL_BLOCK_H */
