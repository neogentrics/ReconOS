/* virtio over PCI: the same device, reached through more indirection.
 *
 * A memory-mapped virtio device has its registers at a fixed offset from a
 * fixed base. A PCI one does not: it publishes a chain of capability structures
 * in configuration space, each saying *which base address register* and *what
 * offset within it* holds one part of the interface. There are four parts --
 * common configuration, the notification area, an interrupt status byte, and
 * the device-specific configuration space -- and they may be in the same
 * register block or in different ones.
 *
 * That is more work to find and exactly the same thing once found, which is why
 * this file is a transport under the same interface rather than a second
 * driver. The block driver above it does not know which bus it is on.
 *
 * --- The notification address is computed, not published ---
 *
 * The one piece of arithmetic worth naming. Each queue has its own doorbell,
 * and its address is
 *
 *     notify_base + queue_notify_off * notify_off_multiplier
 *
 * where the multiplier comes from the notification capability and
 * queue_notify_off comes from the common configuration *after selecting the
 * queue*. A multiplier of zero is legal and means every queue shares one
 * doorbell, which is a real configuration and not an error -- reading it as one
 * is a way to reject working hardware.
 */
#include <recon/kernel/virtio.h>
#include <recon/kernel/pci.h>

#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/vm.h>

/* Vendor-specific capability, and the four kinds virtio publishes under it. */
#define PCI_CAP_ID_VENDOR 0x09

#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG    3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4

/* Offsets within a virtio PCI capability, from its start in config space. */
#define VCAP_CFG_TYPE 3
#define VCAP_BAR      4
#define VCAP_OFFSET   8
#define VCAP_LENGTH   12
#define VCAP_NOTIFY_MULTIPLIER 16	/* notification capability only */

/* The common configuration block, which is the register set the handshake
 * happens through. Offsets from the start of the block. */
#define COMMON_DEVICE_FEATURE_SELECT 0x00
#define COMMON_DEVICE_FEATURE        0x04
#define COMMON_DRIVER_FEATURE_SELECT 0x08
#define COMMON_DRIVER_FEATURE        0x0C
#define COMMON_NUM_QUEUES            0x12
#define COMMON_DEVICE_STATUS         0x14
#define COMMON_QUEUE_SELECT          0x16
#define COMMON_QUEUE_SIZE            0x18
#define COMMON_QUEUE_ENABLE          0x1C
#define COMMON_QUEUE_NOTIFY_OFF      0x1E
#define COMMON_QUEUE_DESC            0x20
#define COMMON_QUEUE_DRIVER          0x28
#define COMMON_QUEUE_DEVICE          0x30

#define VIRTIO_PCI_MAX 4

struct virtio_pci {
	volatile u8 *common;
	volatile u8 *notify;
	volatile u8 *device_cfg;
	u32 notify_multiplier;
	bool in_use;
};

static struct virtio_pci slots[VIRTIO_PCI_MAX];
static unsigned slot_count;

static u8  r8(volatile u8 *p, u32 off)  { return *(volatile u8 *)(p + off); }
static u16 r16(volatile u8 *p, u32 off) { return *(volatile u16 *)(p + off); }
static u32 r32(volatile u8 *p, u32 off) { return *(volatile u32 *)(p + off); }

static void w8(volatile u8 *p, u32 off, u8 v)   { *(volatile u8 *)(p + off) = v; }
static void w16(volatile u8 *p, u32 off, u16 v) { *(volatile u16 *)(p + off) = v; }
static void w32(volatile u8 *p, u32 off, u32 v) { *(volatile u32 *)(p + off) = v; }

static void w64(volatile u8 *p, u32 off, u64 v)
{
	/* Two 32-bit writes, low half first. The specification requires exactly
	 * this: the register is defined as a pair, and a single 64-bit store to
	 * it is not guaranteed to be seen as one. */
	w32(p, off, (u32)v);
	w32(p, off + 4, (u32)(v >> 32));
}

static u32 pci_get_features(struct virtio_device *v, u32 select)
{
	struct virtio_pci *p = v->regs;

	w32(p->common, COMMON_DEVICE_FEATURE_SELECT, select);
	return r32(p->common, COMMON_DEVICE_FEATURE);
}

static void pci_set_features(struct virtio_device *v, u32 select, u32 value)
{
	struct virtio_pci *p = v->regs;

	w32(p->common, COMMON_DRIVER_FEATURE_SELECT, select);
	w32(p->common, COMMON_DRIVER_FEATURE, value);
}

static u8 pci_get_status(struct virtio_device *v)
{
	struct virtio_pci *p = v->regs;

	return r8(p->common, COMMON_DEVICE_STATUS);
}

static void pci_set_status(struct virtio_device *v, u8 status)
{
	struct virtio_pci *p = v->regs;

	w8(p->common, COMMON_DEVICE_STATUS, status);
}

static u16 pci_queue_size(struct virtio_device *v, u16 index)
{
	struct virtio_pci *p = v->regs;

	w16(p->common, COMMON_QUEUE_SELECT, index);
	return r16(p->common, COMMON_QUEUE_SIZE);
}

static bool pci_setup_queue(struct virtio_device *v, u16 index, struct virtqueue *q)
{
	struct virtio_pci *p = v->regs;

	w16(p->common, COMMON_QUEUE_SELECT, index);

	if (!r16(p->common, COMMON_QUEUE_SIZE))
		return false;	/* no such queue */

	w16(p->common, COMMON_QUEUE_SIZE, q->size);
	w64(p->common, COMMON_QUEUE_DESC,   q->desc_phys);
	w64(p->common, COMMON_QUEUE_DRIVER, q->avail_phys);
	w64(p->common, COMMON_QUEUE_DEVICE, q->used_phys);
	w16(p->common, COMMON_QUEUE_ENABLE, 1);
	return true;
}

static void pci_notify(struct virtio_device *v, u16 index)
{
	struct virtio_pci *p = v->regs;
	u16 off;

	/* The doorbell's address depends on the queue, and the queue has to be
	 * selected before its notification offset can be read. Doing this at
	 * setup time and caching it would be faster; doing it here keeps the
	 * transport stateless per queue, and this kernel rings one doorbell per
	 * disk request rather than per packet. */
	w16(p->common, COMMON_QUEUE_SELECT, index);
	off = r16(p->common, COMMON_QUEUE_NOTIFY_OFF);

	*(volatile u16 *)(p->notify + (u32)off * p->notify_multiplier) = index;
}

static void pci_config_read(struct virtio_device *v, u32 offset, void *dst, u32 len)
{
	struct virtio_pci *p = v->regs;
	u8 *out = dst;

	for (u32 i = 0; i < len; i++)
		out[i] = p->device_cfg[offset + i];
}

static const struct virtio_transport pci_transport = {
	.name         = "pci",
	.get_features = pci_get_features,
	.set_features = pci_set_features,
	.get_status   = pci_get_status,
	.set_status   = pci_set_status,
	.setup_queue  = pci_setup_queue,
	.queue_size   = pci_queue_size,
	.notify       = pci_notify,
	.config_read  = pci_config_read,
};

/* Maps a base address register so its contents can be reached, and returns a
 * pointer into it.
 *
 * The register block is at a physical address nothing has mapped: the direct
 * map covers the memory the firmware described, and a device's registers are
 * deliberately not memory. So it is mapped here, as device memory, in the
 * direct map -- the low half of the address space belongs to user programs
 * since checkpoint 10 and there is nothing down there to use. */
static volatile u8 *map_bar(const struct pci_device *d, u8 bar, u32 offset, u32 len)
{
	paddr_t base, page;
	u64 span;
	vaddr_t va;

	if (bar >= 6 || d->bar_is_io[bar] || !d->bar_size[bar] || !d->bar[bar])
		return 0;

	if ((u64)offset + len > d->bar_size[bar])
		return 0;

	base = (paddr_t)d->bar[bar];
	page = PAGE_ALIGN_DOWN(base);
	span = PAGE_ALIGN_UP((base - page) + d->bar_size[bar]);
	va   = (vaddr_t)(uintptr_t)phys_to_virt(page);

	if (!vm_lookup(va) &&
	    !vm_map(va, page, span, VM_READ | VM_WRITE | VM_DEVICE | VM_GLOBAL))
		return 0;

	return (volatile u8 *)phys_to_virt(base) + offset;
}

/* Looks at one PCI function and says whether a virtio device this kernel can
 * drive is there.
 *
 * --- Which identifier says what kind of device it is ---
 *
 * The vendor is always 0x1AF4. After that there are two conventions, and the
 * distinction is the whole of this comment:
 *
 *   0x1040 + type   a device that only speaks 1.0. The type is in the id.
 *   0x1000..0x103F  a *transitional* device: it can speak either protocol, and
 *                   the type is in the PCI subsystem id instead.
 *
 * A transitional device is what a hypervisor offers by default, because it has
 * to work with guests written before 1.0 existed. QEMU's virtio-blk-pci reports
 * 0x1001, which reads exactly like a legacy-only device -- and rejecting it on
 * that basis rejects hardware that would have worked perfectly.
 *
 * --- So the decision is made on capabilities, not on the identifier ---
 *
 * A device that publishes the 1.0 capability structures can be driven the 1.0
 * way, whatever number it reports. A device that does not, cannot. That is the
 * question this driver actually needs answered, so it is the question asked,
 * and it happens to be true of both conventions at once.
 */
bool virtio_pci_probe(const struct pci_device *d, struct virtio_device *out)
{
	struct virtio_pci *p;
	u8 cap;
	u32 device_type;

	if (d->vendor != 0x1AF4)
		return false;

	if (d->device >= 0x1040 && d->device <= 0x107F)
		device_type = (u32)(d->device - 0x1040);
	else if (d->device >= 0x1000 && d->device <= 0x103F)
		device_type = d->subsystem_id;
	else
		return false;

	if (slot_count >= VIRTIO_PCI_MAX)
		return false;

	p = &slots[slot_count];
	kmemset(p, 0, sizeof(*p));

	/* Walk the capability chain for the four parts. Each capability names a
	 * base address register and an offset in it, and the same id appears
	 * four times with a different kind field -- which is why the walk takes
	 * a starting point and continues rather than finding "the" one. */
	for (cap = pci_find_capability(d, PCI_CAP_ID_VENDOR, 0);
	     cap;
	     cap = pci_find_capability(d, PCI_CAP_ID_VENDOR, cap)) {
		u8 kind = pci_read8(d, (u8)(cap + VCAP_CFG_TYPE));
		u8 bar  = pci_read8(d, (u8)(cap + VCAP_BAR));
		u32 off = pci_read32(d, (u8)(cap + VCAP_OFFSET));
		u32 len = pci_read32(d, (u8)(cap + VCAP_LENGTH));

		switch (kind) {
		case VIRTIO_PCI_CAP_COMMON_CFG:
			p->common = map_bar(d, bar, off, len);
			break;
		case VIRTIO_PCI_CAP_NOTIFY_CFG:
			p->notify = map_bar(d, bar, off, len);
			p->notify_multiplier =
				pci_read32(d, (u8)(cap + VCAP_NOTIFY_MULTIPLIER));
			break;
		case VIRTIO_PCI_CAP_DEVICE_CFG:
			p->device_cfg = map_bar(d, bar, off, len);
			break;
		default:
			break;	/* the interrupt status byte, which nothing polls */
		}
	}

	if (!p->common || !p->notify) {
		/* Either a legacy-only device, or one whose registers could not
		 * be mapped. Both mean the same thing here and neither is worth
		 * two messages: this driver speaks 1.0 and this device does
		 * not. Said out loud, because a disk that is present and does
		 * nothing is worse than one that explains itself. */
		kprintf("virtio-pci: %x:%x publishes no 1.0 capabilities, so it "
			"speaks only the pre-1.0 protocol and is left alone\n",
			d->vendor, d->device);
		return false;
	}

	kmemset(out, 0, sizeof(*out));
	out->t         = &pci_transport;
	out->regs      = p;
	out->device_id = device_type;

	p->in_use = true;
	slot_count++;
	return true;
}
