/* The memory-mapped virtio transport, and the handshake every transport shares.
 *
 * On a machine described by a device tree there is no bus to enumerate: the
 * firmware lists a set of fixed addresses, each of which is either a virtio
 * device or a slot with nothing plugged into it. Reading the first register
 * says which. That is the whole of discovery, and it is why this is the
 * shortest path to a working driver on aarch64 -- there is no PCI bus to bring
 * up first.
 *
 * The register block is portable in the strict sense: offsets from a base, with
 * the base supplied by whoever found the device. Nothing in this file knows an
 * address.
 *
 * --- Version 1 and version 2 are different protocols ---
 *
 * The legacy register layout (version 1) hands the device a *page number* for
 * one contiguous ring area and lets it work the rest out. The modern one
 * (version 2) is given three separate physical addresses and a queue is
 * enabled explicitly. Only version 2 is implemented, and a version 1 device is
 * refused rather than half-supported: the legacy layout also changes the
 * endianness rules for configuration space, and a driver that quietly handles
 * both is a driver with two untested paths.
 */
#include <recon/kernel/virtio.h>

#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/vm.h>

/* The register block. All 32 bits wide, all little-endian, all offsets from
 * the device's base. */
#define VIRTIO_MMIO_MAGIC              0x000	/* "virt" */
#define VIRTIO_MMIO_VERSION            0x004
#define VIRTIO_MMIO_DEVICE_ID          0x008
#define VIRTIO_MMIO_VENDOR_ID          0x00C
#define VIRTIO_MMIO_DEVICE_FEATURES    0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES    0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_QUEUE_SEL          0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX      0x034
#define VIRTIO_MMIO_QUEUE_NUM          0x038
#define VIRTIO_MMIO_QUEUE_READY        0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY       0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS   0x060
#define VIRTIO_MMIO_INTERRUPT_ACK      0x064
#define VIRTIO_MMIO_STATUS             0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW     0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH    0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW    0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH   0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW     0x0A0
#define VIRTIO_MMIO_QUEUE_USED_HIGH    0x0A4
#define VIRTIO_MMIO_CONFIG             0x100

/* The four letters "virt", read out of a little-endian 32-bit register.
 *
 * Written as characters rather than as the number they add up to, which is
 * both clearer and the honest form: it is a string that happens to be read
 * through a word-wide register, not a magic constant. */
#define VIRTIO_MMIO_MAGIC_VALUE \
	((u32)'v' | ((u32)'i' << 8) | ((u32)'r' << 16) | ((u32)'t' << 24))

static u32 mmio_r(void *base, u32 off)
{
	return *(volatile u32 *)((u8 *)base + off);
}

static void mmio_w(void *base, u32 off, u32 v)
{
	*(volatile u32 *)((u8 *)base + off) = v;
}

static u32 mmio_get_features(struct virtio_device *v, u32 select)
{
	mmio_w(v->regs, VIRTIO_MMIO_DEVICE_FEATURES_SEL, select);
	return mmio_r(v->regs, VIRTIO_MMIO_DEVICE_FEATURES);
}

static void mmio_set_features(struct virtio_device *v, u32 select, u32 value)
{
	mmio_w(v->regs, VIRTIO_MMIO_DRIVER_FEATURES_SEL, select);
	mmio_w(v->regs, VIRTIO_MMIO_DRIVER_FEATURES, value);
}

static u8 mmio_get_status(struct virtio_device *v)
{
	return (u8)mmio_r(v->regs, VIRTIO_MMIO_STATUS);
}

static void mmio_set_status(struct virtio_device *v, u8 status)
{
	mmio_w(v->regs, VIRTIO_MMIO_STATUS, status);
}

static u16 mmio_queue_size(struct virtio_device *v, u16 index)
{
	mmio_w(v->regs, VIRTIO_MMIO_QUEUE_SEL, index);
	return (u16)mmio_r(v->regs, VIRTIO_MMIO_QUEUE_NUM_MAX);
}

static bool mmio_setup_queue(struct virtio_device *v, u16 index, struct virtqueue *q)
{
	mmio_w(v->regs, VIRTIO_MMIO_QUEUE_SEL, index);

	if (mmio_r(v->regs, VIRTIO_MMIO_QUEUE_READY))
		return false;	/* already live; setting it up twice is a bug */

	mmio_w(v->regs, VIRTIO_MMIO_QUEUE_NUM, q->size);

	/* Sixty-four-bit addresses through thirty-two-bit registers, low half
	 * first. The device latches nothing until QUEUE_READY, so the two
	 * halves being written separately is safe by construction. */
	mmio_w(v->regs, VIRTIO_MMIO_QUEUE_DESC_LOW,   (u32)q->desc_phys);
	mmio_w(v->regs, VIRTIO_MMIO_QUEUE_DESC_HIGH,  (u32)(q->desc_phys >> 32));
	mmio_w(v->regs, VIRTIO_MMIO_QUEUE_AVAIL_LOW,  (u32)q->avail_phys);
	mmio_w(v->regs, VIRTIO_MMIO_QUEUE_AVAIL_HIGH, (u32)(q->avail_phys >> 32));
	mmio_w(v->regs, VIRTIO_MMIO_QUEUE_USED_LOW,   (u32)q->used_phys);
	mmio_w(v->regs, VIRTIO_MMIO_QUEUE_USED_HIGH,  (u32)(q->used_phys >> 32));

	mmio_w(v->regs, VIRTIO_MMIO_QUEUE_READY, 1);
	return true;
}

static void mmio_notify(struct virtio_device *v, u16 index)
{
	mmio_w(v->regs, VIRTIO_MMIO_QUEUE_NOTIFY, index);
}

/* Configuration space, byte by byte.
 *
 * Byte-wise rather than as words because the fields are not all word-aligned
 * and their meaning depends on which features were negotiated. Reading a struct
 * over the top would be faster and would break the first time a device reports
 * a layout this kernel was not compiled against. */
static void mmio_config_read(struct virtio_device *v, u32 offset, void *dst, u32 len)
{
	volatile u8 *src = (volatile u8 *)v->regs + VIRTIO_MMIO_CONFIG + offset;
	u8 *out = dst;

	for (u32 i = 0; i < len; i++)
		out[i] = src[i];
}

static const struct virtio_transport mmio_transport = {
	.name         = "mmio",
	.get_features = mmio_get_features,
	.set_features = mmio_set_features,
	.get_status   = mmio_get_status,
	.set_status   = mmio_set_status,
	.setup_queue  = mmio_setup_queue,
	.queue_size   = mmio_queue_size,
	.notify       = mmio_notify,
	.config_read  = mmio_config_read,
};

/* Looks at one address and says what, if anything, is there.
 *
 * `regs` must already be mapped as device memory. Returns false for an empty
 * slot, which is the common case: QEMU's virt machine lays out thirty-two of
 * these and populates them from the last one backwards, so most of them are
 * empty and finding that out is the normal outcome rather than an error. */
bool virtio_mmio_probe(void *regs, struct virtio_device *out)
{
	u32 magic, version, device_id;

	magic = mmio_r(regs, VIRTIO_MMIO_MAGIC);
	if (magic != VIRTIO_MMIO_MAGIC_VALUE)
		return false;

	device_id = mmio_r(regs, VIRTIO_MMIO_DEVICE_ID);
	if (device_id == 0)
		return false;	/* a real slot with nothing plugged into it */

	version = mmio_r(regs, VIRTIO_MMIO_VERSION);
	if (version != 2) {
		kprintf("virtio-mmio: a version %u device is present and is not "
			"supported; only the 1.0 register layout is implemented\n",
			version);
		return false;
	}

	kmemset(out, 0, sizeof(*out));
	out->t         = &mmio_transport;
	out->regs      = regs;
	out->device_id = device_id;
	return true;
}

/* --- The handshake, which is the same for every transport ------------------
 *
 * The order below is the protocol, not a convention. A device is entitled to
 * ignore anything written out of order, and to set FAILED if the driver asks
 * for features it does not have. Skipping a step does not produce an error --
 * it produces a device that never answers.
 */
bool virtio_begin(struct virtio_device *v, u64 wanted)
{
	u64 offered, agreed;
	u8 status;

	/* Reset. Zero means "forget everything", and doing it first means a
	 * device left configured by firmware -- which is exactly what a UEFI
	 * boot leaves behind -- starts from the same place as one that was
	 * never touched. */
	v->t->set_status(v, 0);

	v->t->set_status(v, VIRTIO_STATUS_ACKNOWLEDGE);
	v->t->set_status(v, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

	offered = (u64)v->t->get_features(v, 0)
		| ((u64)v->t->get_features(v, 1) << 32);

	/* VERSION_1 is not optional here. A device that does not offer it is
	 * speaking the pre-1.0 draft, in which configuration space is in the
	 * guest's byte order and the queue layout differs -- a different
	 * protocol wearing the same name. */
	if (!(offered & (1ULL << VIRTIO_F_VERSION_1))) {
		kputs("virtio: the device does not offer the 1.0 feature bit\n");
		virtio_give_up(v);
		return false;
	}

	agreed = offered & (wanted | (1ULL << VIRTIO_F_VERSION_1));

	v->t->set_features(v, 0, (u32)agreed);
	v->t->set_features(v, 1, (u32)(agreed >> 32));

	v->t->set_status(v, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER
			  | VIRTIO_STATUS_FEATURES_OK);

	/* Read it back. This is the device's only chance to disagree, and a
	 * driver that does not read it proceeds against a device that has
	 * already refused. */
	status = v->t->get_status(v);
	if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
		kputs("virtio: the device refused the features it was offered\n");
		virtio_give_up(v);
		return false;
	}

	v->features = agreed;
	return true;
}

void virtio_ready(struct virtio_device *v)
{
	v->t->set_status(v, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER
			  | VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
}

void virtio_give_up(struct virtio_device *v)
{
	v->t->set_status(v, VIRTIO_STATUS_FAILED);
}
