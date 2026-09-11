/* virtio: a queue of buffers, and two rings that pass ownership of them.
 *
 * virtio is what a device looks like when the person designing it knows it is
 * being emulated. There is no register-poking protocol, no vendor quirks, no
 * fifteen-year-old errata -- there is a shared ring of descriptors, and the two
 * ends take turns owning entries in it. Every virtio device speaks it: disks,
 * networks, consoles, random number generators. Learning it once buys all of
 * them, which is most of why the first driver this kernel gets is a virtio one.
 *
 * --- The three pieces ---
 *
 * A **descriptor table**: an array of (physical address, length, flags, next).
 * A buffer handed to the device is one descriptor, or a chain of them linked by
 * `next`. Flags say whether the device may write to it.
 *
 * An **available ring**: the driver's end. The driver fills descriptors, then
 * publishes the head of the chain here and bumps an index.
 *
 * A **used ring**: the device's end. When the device is finished it publishes
 * the same head here, with how many bytes it wrote, and bumps its own index.
 *
 * That is the whole protocol. The driver owns a descriptor from the moment it
 * allocates it until it appears in the used ring, and not one instruction
 * longer.
 *
 * --- The part that is actually hard ---
 *
 * These structures are shared with something running concurrently. The
 * ordering is not advisory:
 *
 *   - every descriptor must be *visible* before the available index that
 *     points at it, or the device reads a descriptor that is still being
 *     written;
 *   - the used index must be read before the used ring entry it refers to, and
 *     the entry read after, or the driver reads an entry the device has not
 *     finished writing.
 *
 * Both are one barrier each, and both failures look like corrupted data rather
 * than like a missing barrier, which is why they are named here rather than
 * left to be rediscovered.
 */
#ifndef RECON_KERNEL_VIRTIO_H
#define RECON_KERNEL_VIRTIO_H

#include <recon/kernel/types.h>
#include <recon/kernel/compiler.h>
#include <recon/kernel/pmm.h>

/* Feature bits every transport shares. */
#define VIRTIO_F_VERSION_1  32	/* speaks the 1.0 specification, not the draft */

/* Descriptor flags. */
#define VRING_DESC_F_NEXT   1	/* this descriptor chains to `next` */
#define VRING_DESC_F_WRITE  2	/* the *device* writes here; otherwise it reads */

struct vring_desc {
	u64 addr;		/* physical, always */
	u32 len;
	u16 flags;
	u16 next;
} RK_PACKED;

struct vring_avail {
	u16 flags;
	u16 idx;
	u16 ring[];		/* queue_size entries */
} RK_PACKED;

struct vring_used_elem {
	u32 id;			/* the head descriptor of the chain */
	u32 len;		/* bytes the device wrote */
} RK_PACKED;

struct vring_used {
	u16 flags;
	u16 idx;
	struct vring_used_elem ring[];
} RK_PACKED;

/* One queue, and everything needed to work it.
 *
 * The three rings live in pages the driver allocated, and the device is told
 * their *physical* addresses. Nothing here holds a virtual address the device
 * could ever see -- a device that was handed one would read whatever physical
 * memory happens to sit at that number, which on this kernel is a plausible
 * page rather than a fault. */
struct virtqueue {
	u16 size;		/* entries; a power of two */

	struct vring_desc  *desc;
	struct vring_avail *avail;
	struct vring_used  *used;

	paddr_t desc_phys, avail_phys, used_phys;

	/* Free descriptors, threaded through `next` in the table itself. There
	 * is nowhere cheaper to keep the list, and the entries are free by
	 * definition while they are on it. */
	u16 free_head;
	u16 free_count;

	/* The last used index this driver has consumed. The device's own index
	 * runs ahead of it and wraps at 65536, not at queue size -- so the
	 * comparison is on the raw counters and the *indexing* is masked. Doing
	 * it the other way round works until the first wrap and then stops. */
	u16 last_used;

	paddr_t backing;	/* the pages, so they can be given back */
	size_t backing_pages;
};

/* Allocates the rings and threads the free list. `size` must be a power of two
 * and is normally what the device reports as its maximum. */
bool virtqueue_init(struct virtqueue *q, u16 size);
void virtqueue_free(struct virtqueue *q);

/* Hands a chain of buffers to the device and publishes it.
 *
 * `bufs` are physical addresses and lengths; `write` says, per buffer, whether
 * the device writes to it. Returns the head descriptor index, or 0xFFFF when
 * there are not enough free descriptors.
 *
 * Publishing is included rather than left to the caller: the barrier between
 * filling the descriptors and bumping the index is the one thing that must not
 * be forgotten, so there is no interface in which it can be. */
u16 virtqueue_submit(struct virtqueue *q, const paddr_t *bufs, const u32 *lens,
		     const bool *write, unsigned count);

/* Collects one completed chain, or returns false if the device has not
 * finished anything new. `len_out` receives the byte count the device
 * reported, and may be null. */
bool virtqueue_collect(struct virtqueue *q, u16 *head_out, u32 *len_out);

/* Returns the descriptors of a chain to the free list. Call after collecting. */
void virtqueue_release(struct virtqueue *q, u16 head);

/* Whether the device has been told there is something to look at. Split from
 * submit because a transport rings the doorbell in its own way. */
u16 virtqueue_avail_index(const struct virtqueue *q);

/* --- Transports ------------------------------------------------------------
 *
 * The queue above is the same everywhere. How a device is *found*, and how its
 * registers are reached, is not: on a machine with a device tree the devices
 * are memory-mapped at addresses the firmware lists, and on a PC they are
 * behind PCI configuration space. Both end up pointing the same hardware at the
 * same rings, so the difference is confined to this table.
 *
 * Every function here talks to one device's registers and nothing else. None of
 * them may allocate, and none of them knows what kind of device it is talking
 * to -- a network card and a disk configure identically and differ only in
 * what their configuration space means.
 */

struct virtio_device;

struct virtio_transport {
	const char *name;

	/* Feature negotiation is 64 bits wide, read and written in halves,
	 * because the register that carries it is 32. `select` is 0 for the low
	 * half and 1 for the high. */
	u32  (*get_features)(struct virtio_device *v, u32 select);
	void (*set_features)(struct virtio_device *v, u32 select, u32 value);

	/* The status byte is the handshake: the driver writes bits as it gets
	 * further, and the device sets FAILED if it disagrees. */
	u8   (*get_status)(struct virtio_device *v);
	void (*set_status)(struct virtio_device *v, u8 status);

	/* Tells the device where one queue's three rings are. */
	bool (*setup_queue)(struct virtio_device *v, u16 index, struct virtqueue *q);

	/* How many entries the device will accept in that queue, or zero if
	 * there is no such queue. */
	u16  (*queue_size)(struct virtio_device *v, u16 index);

	/* "There is something in the available ring." */
	void (*notify)(struct virtio_device *v, u16 index);

	/* Asks for this queue's completions to arrive as an interrupt.
	 *
	 * Optional, and null on a transport that has no way to do it -- which
	 * is how the memory-mapped transport says so without every driver
	 * needing to know there are two. A driver that gets null, or false,
	 * keeps polling; a driver that stops polling on the strength of a
	 * pointer it never checked stops for ever.
	 *
	 * `name` is what the vector is called in the interrupt summary, so
	 * that a line nobody is taking can be traced back to who asked. */
	bool (*request_interrupt)(struct virtio_device *v, u16 index,
				  void (*fn)(void *), void *arg,
				  const char *name);

	/* Device-specific configuration space, which means something different
	 * for every device type. Read as bytes rather than as a struct, because
	 * the layout depends on which features were negotiated. */
	void (*config_read)(struct virtio_device *v, u32 offset, void *dst, u32 len);
};

struct virtio_device {
	const struct virtio_transport *t;
	void *regs;		/* the transport's own handle on the registers */
	u32 device_id;		/* 1 network, 2 block, 4 entropy, ... */
	u64 features;		/* what both ends agreed to */
};

/* Status bits, in the order a driver sets them. The order is the protocol:
 * a device may refuse at any step by setting FAILED, and a driver that skips
 * a step is talking to hardware that has not agreed to listen. */
#define VIRTIO_STATUS_ACKNOWLEDGE  1
#define VIRTIO_STATUS_DRIVER       2
#define VIRTIO_STATUS_DRIVER_OK    4
#define VIRTIO_STATUS_FEATURES_OK  8
#define VIRTIO_STATUS_FAILED       128

/* Looks at one already-mapped register block and says whether a virtio device
 * is there. False for an empty slot, which is the ordinary outcome: a machine
 * that lays out thirty-two of these populates a handful. */
bool virtio_mmio_probe(void *regs, struct virtio_device *out);

/* Takes a probed device and makes it a block device, if that is what it is.
 * False, quietly, for anything else -- a caller probes everything it can find
 * and most of what it finds is not a disk. */
bool virtio_blk_attach(const struct virtio_device *probed);
unsigned virtio_blk_count(void);

/* That a disk given a message vector actually raises it. True, with a line
 * saying so, when no device here has one. */
bool virtio_blk_self_test(void);

/* What the disks are, and how their waiting actually went. */
void virtio_blk_print_summary(void);

/* Walks the handshake as far as FEATURES_OK, negotiating `wanted` on top of
 * VIRTIO_F_VERSION_1. Leaves DRIVER_OK unset: the driver still has to set its
 * queues up, and a device told the driver is ready before its queues exist is
 * entitled to start using them. */
bool virtio_begin(struct virtio_device *v, u64 wanted);

/* The last step, once the queues are live. */
void virtio_ready(struct virtio_device *v);

/* Marks the device failed, so that it stops rather than half-works. Called on
 * any path that gives up part-way through. */
void virtio_give_up(struct virtio_device *v);

#endif /* RECON_KERNEL_VIRTIO_H */
