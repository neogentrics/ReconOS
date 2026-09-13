/* virtio-net: the first card this kernel can actually talk to.
 *
 * The blueprint names four NICs -- e1000, e1000e, RTL8139/8169 and this one.
 * This is first for the same reason virtio-blk was the first disk: it is the
 * device designed by somebody who knew it would be emulated, so there is a
 * ring of descriptors and no vendor errata, and the transport work is already
 * done and shared with the disk driver. A real card is a second driver behind
 * the same `net_device_ops`, not a second stack.
 *
 * --- Two queues, and which way each one points ---
 *
 * Queue 0 is receive and queue 1 is transmit, and the asymmetry is the thing
 * to get right: on the **receive** queue the driver hands the device empty
 * buffers and the device writes into them, so every descriptor is marked
 * device-writable. On the **transmit** queue the driver hands over full
 * buffers the device only reads. A receive buffer posted without the write
 * flag is a buffer the device will not fill, and the symptom is a card that
 * appears to receive nothing at all.
 *
 * The receive queue must be kept **stocked**. A device with no free buffer
 * drops the frame; it does not wait. So a buffer is posted again as soon as
 * the frame in it has been handed upward, and the count of buffers in the ring
 * is printed, because a ring that has quietly emptied looks exactly like a
 * network with no traffic on it.
 *
 * --- The twelve bytes in front of every frame ---
 *
 * virtio-net puts its own header ahead of the Ethernet frame, in both
 * directions. It carries checksum-offload and segmentation-offload fields,
 * none of which this driver asks for -- but the header is present whether or
 * not its fields are used, and a driver that forgets it is reading the frame
 * twelve bytes early. That is a header that parses as plausible nonsense
 * rather than failing cleanly, which is why the length is a named constant
 * rather than a number written at each site.
 */
#include <recon/kernel/net.h>
#include <recon/kernel/virtio.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/console.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/vm.h>

#define VIRTIO_NET_F_MAC    5
#define VIRTIO_NET_F_STATUS 16

/* With VIRTIO_F_VERSION_1 negotiated the header carries `num_buffers`, which
 * makes it twelve bytes rather than the legacy ten. */
#define VIRTIO_NET_HDR_LEN 12

#define RX_RING 32
#define TX_RING 32

/* The largest queue this driver will accept, and therefore the largest
 * descriptor index it can be handed back. */
#define VIRTIO_NET_QUEUE_MAX 64

#define VIRTIO_NET_MAX 2

struct rx_slot {
	struct netbuf *buf;
	paddr_t hdr_phys;
	u8 *hdr;
	bool posted;
};

struct virtio_net {
	struct virtio_device dev;
	struct net_device *ndev;

	struct virtqueue rx, tx;
	struct spinlock lock;

	/* One page holds the little headers for every slot in both rings:
	 * twelve bytes each, so sixty-four of them is 768 bytes. One
	 * allocation rather than one per slot. */
	paddr_t hdr_page;
	u8 *hdr_base;

	struct rx_slot rx_slots[RX_RING];

	/* Which slot a descriptor head belongs to, and which buffer a transmit
	 * head is carrying. Indexed by the descriptor index the queue returns,
	 * which is why they are sized by the queue rather than by the ring:
	 * a descriptor index is not a slot index, and treating one as the
	 * other makes two frames in flight share an entry. */
	u16 rx_head_slot[VIRTIO_NET_QUEUE_MAX];
	struct netbuf *tx_bufs[VIRTIO_NET_QUEUE_MAX];

	/* Which header slot a transmit descriptor borrowed, so it can be
	 * released when the device is finished with it. */
	u16 tx_head_hdr[VIRTIO_NET_QUEUE_MAX];
	bool tx_hdr_busy[TX_RING];

	unsigned posted;
	u64 rx_frames, tx_frames, rx_no_buffer, tx_ring_full;
};

static struct virtio_net devices[VIRTIO_NET_MAX];
static unsigned device_count;

/* Puts one empty buffer on the receive ring. Held with the lock. */
static bool post_one(struct virtio_net *n, unsigned slot)
{
	struct rx_slot *s = &n->rx_slots[slot];
	paddr_t bufs[2];
	u32 lens[2];
	bool write[2];
	u16 head;

	if (s->posted)
		return true;

	if (!s->buf) {
		s->buf = netbuf_alloc();

		if (!s->buf) {
			n->rx_no_buffer++;
			return false;
		}
	}

	/* The device writes the virtio header into one descriptor and the
	 * frame into the next. Splitting them means the frame lands where the
	 * stack wants it -- at `data` -- with no copy to strip the header. */
	bufs[0] = s->hdr_phys;
	lens[0] = VIRTIO_NET_HDR_LEN;
	write[0] = true;

	bufs[1] = s->buf->page + (paddr_t)(s->buf->data - s->buf->head);
	lens[1] = ETH_FRAME_MAX;
	write[1] = true;

	head = virtqueue_submit(&n->rx, bufs, lens, write, 2);

	if (head == 0xFFFF)
		return false;

	s->posted = true;
	n->posted++;

	/* The head identifies the slot when the device hands it back. Kept
	 * here rather than in the descriptor: `next` chains the head to the
	 * frame's descriptor and is the device's to read. */
	if (head < VIRTIO_NET_QUEUE_MAX)
		n->rx_head_slot[head] = (u16)slot;

	n->dev.t->notify(&n->dev, 0);
	return true;
}

static unsigned slot_of(struct virtio_net *n, u16 head)
{
	if (head >= VIRTIO_NET_QUEUE_MAX)
		return 0;

	return (unsigned)n->rx_head_slot[head] % RX_RING;
}

static void collect_tx(struct virtio_net *n)
{
	u16 head;
	u32 len;

	while (virtqueue_collect(&n->tx, &head, &len)) {
		if (head < VIRTIO_NET_QUEUE_MAX) {
			if (n->tx_bufs[head]) {
				netbuf_free(n->tx_bufs[head]);
				n->tx_bufs[head] = NULL;
			}

			n->tx_hdr_busy[n->tx_head_hdr[head] % TX_RING] = false;
		}

		virtqueue_release(&n->tx, head);
	}
}

static void net_poll(struct net_device *dev)
{
	struct virtio_net *n = dev->driver;
	u16 head;
	u32 len;
	u64 flags;

	flags = spin_lock_irq(&n->lock);

	collect_tx(n);

	while (virtqueue_collect(&n->rx, &head, &len)) {
		unsigned slot = slot_of(n, head);
		struct rx_slot *s = &n->rx_slots[slot];
		struct netbuf *b = s->buf;

		virtqueue_release(&n->rx, head);

		s->posted = false;
		n->posted--;

		if (!b)
			continue;

		s->buf = NULL;

		/* `len` counts the virtio header as well as the frame, because
		 * the device wrote both. Subtracting it is what turns a device
		 * byte count into a frame length -- and forgetting to is a
		 * frame twelve bytes too long, which Ethernet does not notice
		 * and IP rejects as a bad checksum. */
		if (len <= VIRTIO_NET_HDR_LEN) {
			netbuf_free(b);
			post_one(n, slot);
			continue;
		}

		b->len = len - VIRTIO_NET_HDR_LEN;

		if (b->len > ETH_FRAME_MAX)
			b->len = ETH_FRAME_MAX;

		n->rx_frames++;

		spin_unlock_irq(&n->lock, flags);
		netdev_receive(dev, b);
		flags = spin_lock_irq(&n->lock);

		post_one(n, slot);
	}

	/* Anything that could not be re-posted for want of memory is tried
	 * again here, so a moment of pressure does not leave the ring
	 * permanently short. */
	{
		unsigned i;

		for (i = 0; i < RX_RING; i++)
			if (!n->rx_slots[i].posted)
				post_one(n, i);
	}

	spin_unlock_irq(&n->lock, flags);
}

static bool net_transmit(struct net_device *dev, struct netbuf *b)
{
	struct virtio_net *n = dev->driver;
	paddr_t bufs[2];
	u32 lens[2];
	bool write[2];
	u16 head;
	unsigned slot;
	u64 flags;
	u8 *hdr;

	flags = spin_lock_irq(&n->lock);

	collect_tx(n);

	/* A free header slot. Full means the device has not finished with what
	 * is already out, and the frame is dropped rather than queued -- a
	 * transmit queue in front of the device's own transmit queue is two
	 * queues with one purpose. */
	for (slot = 0; slot < TX_RING; slot++)
		if (!n->tx_hdr_busy[slot])
			break;

	if (slot == TX_RING) {
		n->tx_ring_full++;
		spin_unlock_irq(&n->lock, flags);
		netbuf_free(b);
		return false;
	}

	hdr = n->hdr_base + (RX_RING + slot) * VIRTIO_NET_HDR_LEN;
	kmemset(hdr, 0, VIRTIO_NET_HDR_LEN);

	bufs[0] = n->hdr_page +
		  (paddr_t)((RX_RING + slot) * VIRTIO_NET_HDR_LEN);
	lens[0] = VIRTIO_NET_HDR_LEN;
	write[0] = false;

	bufs[1] = b->page + (paddr_t)(b->data - b->head);
	lens[1] = b->len;
	write[1] = false;

	head = virtqueue_submit(&n->tx, bufs, lens, write, 2);

	if (head == 0xFFFF) {
		n->tx_ring_full++;
		spin_unlock_irq(&n->lock, flags);
		netbuf_free(b);
		return false;
	}

	if (head < VIRTIO_NET_QUEUE_MAX)
		n->tx_bufs[head] = b;

	n->tx_hdr_busy[slot] = true;
	n->tx_head_hdr[head < VIRTIO_NET_QUEUE_MAX ? head : 0] = (u16)slot;
	n->tx_frames++;

	n->dev.t->notify(&n->dev, 1);
	spin_unlock_irq(&n->lock, flags);

	return true;
}

static const struct net_device_ops virtio_net_ops = {
	.transmit = net_transmit,
	.enable_interrupts = NULL,
	.poll = net_poll,
};

bool virtio_net_attach(const struct virtio_device *probed)
{
	struct virtio_net *n;
	struct mac_addr mac;
	char name[NET_NAME_MAX];
	u16 qsize;
	unsigned i;

	if (probed->device_id != 1)
		return false;	/* 1 is network; 2 is block, 4 entropy */

	if (device_count >= VIRTIO_NET_MAX)
		return false;

	n = &devices[device_count];
	kmemset(n, 0, sizeof(*n));
	n->dev = *probed;
	spin_init(&n->lock, "virtio-net");

	if (!virtio_begin(&n->dev, (1ULL << VIRTIO_NET_F_MAC)
				 | (1ULL << VIRTIO_NET_F_STATUS)))
		return false;

	/* The hardware address, at offset 0 of configuration space. Read after
	 * negotiation, because whether it is there at all depends on whether
	 * VIRTIO_NET_F_MAC was agreed. */
	if (n->dev.features & (1ULL << VIRTIO_NET_F_MAC)) {
		n->dev.t->config_read(&n->dev, 0, mac.b, MAC_LEN);
	} else {
		/* No address offered. One is invented with the locally
		 * administered bit set, which is what that bit is for -- an
		 * address nobody else on the wire will have assigned. */
		mac.b[0] = 0x02;
		mac.b[1] = 0x00;
		mac.b[2] = 0x00;
		mac.b[3] = 0x00;
		mac.b[4] = 0x00;
		mac.b[5] = (u8)(device_count + 1);
	}

	qsize = n->dev.t->queue_size(&n->dev, 0);

	if (!qsize) {
		kputs("virtio-net: the device has no receive queue\n");
		return false;
	}

	if (qsize > RX_RING * 2)
		qsize = RX_RING * 2;

	if (!virtqueue_init(&n->rx, qsize) ||
	    !virtqueue_init(&n->tx, qsize)) {
		kputs("virtio-net: no memory for the queues\n");
		return false;
	}

	if (!n->dev.t->setup_queue(&n->dev, 0, &n->rx) ||
	    !n->dev.t->setup_queue(&n->dev, 1, &n->tx)) {
		kputs("virtio-net: the device refused a queue\n");
		virtqueue_free(&n->rx);
		virtqueue_free(&n->tx);
		return false;
	}

	n->hdr_page = pmm_alloc_page();

	if (!n->hdr_page) {
		kputs("virtio-net: no memory for the headers\n");
		virtqueue_free(&n->rx);
		virtqueue_free(&n->tx);
		return false;
	}

	n->hdr_base = phys_to_virt(n->hdr_page);
	kmemset(n->hdr_base, 0, 4096);

	for (i = 0; i < RX_RING; i++) {
		n->rx_slots[i].hdr = n->hdr_base + i * VIRTIO_NET_HDR_LEN;
		n->rx_slots[i].hdr_phys = n->hdr_page +
					  (paddr_t)(i * VIRTIO_NET_HDR_LEN);
	}

	/* DRIVER_OK before the ring is stocked would be telling the device it
	 * may start receiving into buffers that do not exist yet. */
	n->dev.t->set_status(&n->dev,
			     (u8)(n->dev.t->get_status(&n->dev) |
				  VIRTIO_STATUS_DRIVER_OK));

	for (i = 0; i < RX_RING; i++)
		post_one(n, i);

	kstrlcpy(name, "eth", sizeof(name));
	name[3] = (char)('0' + device_count);
	name[4] = 0;

	n->ndev = netdev_register(name, &virtio_net_ops, n, &mac);

	if (!n->ndev) {
		virtqueue_free(&n->rx);
		virtqueue_free(&n->tx);
		pmm_free_pages(n->hdr_page, 1);
		return false;
	}

	n->ndev->link = true;
	device_count++;

	{
		char text[20];

		kprintf("virtio-net: %s at %s, %u receive buffers posted\n",
			name, mac_format(&mac, text), n->posted);
	}

	return true;
}

unsigned virtio_net_count(void)
{
	return device_count;
}

void virtio_net_print_summary(void)
{
	unsigned i;

	for (i = 0; i < device_count; i++) {
		struct virtio_net *n = &devices[i];

		kprintf("  %s         : %u in, %u out, %u posted",
			n->ndev ? n->ndev->name : "eth?",
			(unsigned)n->rx_frames, (unsigned)n->tx_frames,
			n->posted);

		if (n->rx_no_buffer || n->tx_ring_full)
			kprintf(", %u no buffer, %u ring full",
				(unsigned)n->rx_no_buffer,
				(unsigned)n->tx_ring_full);

		kprintf("\n");

		/* A ring that has quietly emptied looks exactly like a quiet
		 * network, so it is said outright rather than left to be
		 * inferred from a number. */
		if (!n->posted)
			kprintf("               : the receive ring is empty -- "
				"this card can no longer receive\n");
	}
}
