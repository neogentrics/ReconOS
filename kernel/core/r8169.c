/* r8169: the Realtek gigabit family, and the first card this kernel drives
 * that nobody designed to be emulated.
 *
 * The blueprint names four network cards -- e1000, e1000e, RTL8139/8169 and
 * virtio-net. virtio-net was written first and for good reasons, and this is
 * the second. The reason for *this* one is arithmetic rather than preference:
 * it is what is actually in the machines this project runs on. Two RTL8168s
 * bonded in the server, an RTL8125 in the desktop, a Realtek in the firewall.
 *
 * --- What "the same family" does and does not mean ---
 *
 * The blueprint's phrase "RTL8139/8169" reads like one driver covering two
 * chips, and it is worth saying plainly that it is not. The RTL8139 receives
 * into **one circular buffer** and the driver chases a pointer around it; the
 * RTL8169 and everything after it receive into a **ring of descriptors**, each
 * naming its own buffer. Those are not two settings of one design. They share
 * a vendor and some register offsets and nothing about how a frame arrives.
 * A driver for one is not most of a driver for the other, and writing this as
 * though it were would have produced something that compiled.
 *
 * What *is* one family, and what this drives, is the C+ descriptor generation:
 * RTL8169, RTL8168/8111, RTL8161, RTL8101/8102. The RTL8125 is declined by
 * name below rather than accepted on the strength of the resemblance, and the
 * comment there says exactly which registers moved.
 *
 * --- The two rings ---
 *
 * Each direction is a ring of sixteen-byte descriptors. A descriptor holds an
 * ownership bit, a length, some flags and a 64-bit physical address, and the
 * ownership bit is the whole protocol: the driver sets OWN and the card may
 * take it, the card clears OWN and the driver may have it back. There is no
 * index register to read and no doorbell to acknowledge -- the rings are
 * walked, and where the driver is up to is the driver's own business.
 *
 * The one thing the hardware insists on is **EOR on the last descriptor**, the
 * bit that says "wrap here". A ring built without it is a card reading
 * descriptors off the end of the ring into whatever follows it in memory, and
 * the symptom is not a card that stops: it is a card that transmits plausible
 * garbage. It is set once, at setup, and never cleared -- which is why every
 * place that rewrites a descriptor below rebuilds the flag word from scratch
 * rather than or-ing into it, and re-applies EOR from the index.
 *
 * --- The four bytes that are not payload ---
 *
 * A received descriptor's length **includes the Ethernet frame check
 * sequence**: the four-byte CRC that the wire carries and that nothing above
 * the card wants. A driver that forgets is handing four trailing bytes of
 * checksum to IP as though they were data.
 *
 * That failure does not announce itself, and the asymmetry was **measured
 * rather than reasoned about** -- on the Intel, by breaking its length on
 * purpose in each direction and booting:
 *
 *   four bytes too short   no DHCP lease, no ping reply -- caught at once
 *   four bytes too long    a lease and a ping reply, exactly as if correct
 *
 * Too short is caught because UDP's checksum covers a length taken from its
 * own header, so a truncated datagram fails it and DHCP's reply is discarded.
 * Too long is silent because every layer reads its own length field and
 * ignores whatever trails it: Ethernet has no length at all, IP has its own,
 * UDP has its own. Four bytes of somebody's CRC sit past the end of the
 * payload and nothing ever looks there.
 *
 * **Forgetting this subtraction is the too-long case.** So the mistake this
 * driver is most likely to make is the one a booting machine cannot show you,
 * which is why it is a paragraph here rather than a line of code with a number
 * in it. It is the same shape as `virtio_net.c`'s twelve-byte header, and the
 * two comments are deliberately written to look alike.
 *
 * --- Where this runs ---
 *
 * The descriptors are host memory the card reads, so their fields are in the
 * card's byte order, which is little-endian -- PCI is little-endian and both
 * architectures this kernel targets are too. That is an assumption and it is
 * written here rather than relied on quietly: on a big-endian host every
 * `opts1` below needs a swap, and the place to notice is this paragraph rather
 * than a packet capture.
 */
#include <recon/kernel/net.h>
#include <recon/kernel/pci.h>
#include <recon/kernel/arch.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/console.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/time.h>

/* --- Who this is ---------------------------------------------------------- */

#define REALTEK_VENDOR	0x10EC

#define RTL_8169	0x8169
#define RTL_8168	0x8168	/* and 8111, which is the same silicon */
#define RTL_8161	0x8161
#define RTL_8136	0x8136	/* 8101E/8102E, fast ethernet, same rings */
#define RTL_8125	0x8125	/* 2.5 Gb -- declined, see r8169_attach */

/* --- Registers, at offsets from the memory window -------------------------- */

#define R_IDR0		0x00	/* the hardware address, six bytes */
#define R_MAR0		0x08	/* the multicast filter, eight bytes */
#define R_TNPDS		0x20	/* transmit ring, 64-bit physical */
#define R_CR		0x37	/* command */
#define R_TPPOLL	0x38	/* "look at the transmit ring now" */
#define R_IMR		0x3C	/* which interrupts are wanted, 16-bit */
#define R_ISR		0x3E	/* which have happened, 16-bit */
#define R_TCR		0x40	/* transmit configuration, 32-bit */
#define R_RCR		0x44	/* receive configuration, 32-bit */
#define R_CFG9346	0x50	/* the lock over the configuration registers */
#define R_PHYSTATUS	0x6C	/* what the cable is doing */
#define R_RMS		0xDA	/* largest frame to accept, 16-bit */
#define R_CPLUSCMD	0xE0	/* the C+ mode controls, 16-bit */
#define R_RDSAR		0xE4	/* receive ring, 64-bit physical */
#define R_MTPS		0xEC	/* largest frame to send, in 128-byte units */

/* R_CR */
#define CR_RST		0x10
#define CR_RX_ENABLE	0x08
#define CR_TX_ENABLE	0x04

/* R_TPPOLL */
#define TPPOLL_NPQ	0x40	/* the normal-priority queue */

/* R_CFG9346. The configuration registers ignore writes unless this is first
 * set to the unlock value, and a driver that forgets sees its writes accepted
 * and discarded -- which is indistinguishable from a register that reads back
 * what you wrote. */
#define CFG9346_LOCK	0x00
#define CFG9346_UNLOCK	0xC0

/* R_IMR and R_ISR share a layout. */
#define INT_ROK		0x0001	/* a frame arrived */
#define INT_RER		0x0002
#define INT_TOK		0x0004	/* a frame went */
#define INT_TER		0x0008
#define INT_RX_OVERFLOW	0x0010
#define INT_LINK_CHG	0x0020
#define INT_RX_FIFO_OVER 0x0040
#define INT_TX_NO_DESC	0x0080
#define INT_SYS_ERR	0x8000

#define INT_WANTED	(INT_ROK | INT_RER | INT_TOK | INT_TER | \
			 INT_RX_OVERFLOW | INT_LINK_CHG | INT_RX_FIFO_OVER | \
			 INT_SYS_ERR)

/* R_RCR: which frames to take, and how to fetch them. */
#define RCR_ACCEPT_ALL_PHYS	0x01	/* promiscuous -- deliberately not set */
#define RCR_ACCEPT_MY_PHYS	0x02
#define RCR_ACCEPT_MULTICAST	0x04
#define RCR_ACCEPT_BROADCAST	0x08
#define RCR_DMA_SHIFT		8
#define RCR_FIFO_SHIFT		13

/* R_TCR */
#define TCR_DMA_SHIFT		8
#define TCR_IFG_SHIFT		24
#define TCR_IFG_NORMAL		3

/* The "no limit" setting for a burst-length or threshold field, which on this
 * family is all-ones in a three-bit field for every one of them. One name,
 * because three copies of the number 7 with different names is three chances
 * to change one of them. */
#define BURST_UNLIMITED		7

/* R_CPLUSCMD */
#define CPLUS_RX_VLAN		0x0040
#define CPLUS_RX_CHECKSUM	0x0020

/* R_PHYSTATUS */
#define PHY_LINK_OK		0x02
#define PHY_FULL_DUPLEX		0x01
#define PHY_10M			0x04
#define PHY_100M		0x08
#define PHY_1000M		0x10

/* --- The descriptor -------------------------------------------------------- */

#define DESC_OWN	0x80000000u	/* the card's, not ours */
#define DESC_EOR	0x40000000u	/* the ring wraps after this one */
#define DESC_FS		0x20000000u	/* first fragment of a frame */
#define DESC_LS		0x10000000u	/* last fragment of a frame */
#define DESC_RX_ERROR	0x00200000u
#define DESC_LEN_MASK	0x00003FFFu

struct rtl_desc {
	u32 opts1;
	u32 opts2;
	u64 addr;
};

/* Sixteen bytes, and the card reads it as sixteen bytes. Checked at compile
 * time rather than trusted, because a padded descriptor is a ring the card
 * walks with the wrong stride and every entry after the first is garbage --
 * and nothing about that failure points at a structure layout. */
_Static_assert(sizeof(struct rtl_desc) == 16, "a descriptor is sixteen bytes");

#define RX_RING		32
#define TX_RING		32

/* What the card may write into one receive buffer.
 *
 * A multiple of eight, which the descriptor requires, and at least a whole
 * Ethernet frame. `netbuf` hands out a page with `NET_HEADROOM` in front of
 * the data, so the bytes actually available from `data` are well over three
 * thousand -- this fits with room to spare, and the assertion below is what
 * says so rather than the arithmetic being redone by a reader. */
#define RX_BUF_SIZE	1536

_Static_assert(RX_BUF_SIZE >= ETH_FRAME_MAX, "a whole frame must fit");
_Static_assert((RX_BUF_SIZE % 8) == 0, "the descriptor wants a multiple of 8");
_Static_assert(NET_HEADROOM + RX_BUF_SIZE <= NET_BUF_CAPACITY,
	       "the card would write past the end of the buffer");

/* Both rings in one page. Each must be 256-byte aligned, which a page start
 * and a 2048-byte offset both are, and 32 descriptors is 512 bytes, so the two
 * cannot reach each other. */
#define RX_RING_OFFSET	0
#define TX_RING_OFFSET	2048

_Static_assert(RX_RING_OFFSET + RX_RING * 16 <= TX_RING_OFFSET,
	       "the receive ring runs into the transmit ring");
_Static_assert(TX_RING_OFFSET + TX_RING * 16 <= 4096,
	       "the transmit ring runs off the end of the page");

#define R8169_MAX	4

struct r8169 {
	struct pci_device pci;
	volatile u8 *mmio;
	struct net_device *ndev;
	struct spinlock lock;

	paddr_t ring_page;
	struct rtl_desc *rx_desc;
	struct rtl_desc *tx_desc;

	struct netbuf *rx_buf[RX_RING];
	struct netbuf *tx_buf[TX_RING];

	/* Where each ring has been walked up to. The card keeps its own
	 * position and does not publish it, so these are the only record --
	 * which is why they are the first thing printed when a ring stalls. */
	unsigned rx_next;
	unsigned tx_head;	/* the next descriptor to fill */
	unsigned tx_tail;	/* the oldest the card has not returned */

	bool interrupting;
	u16 device_id;
	u8 revision;

	u64 interrupts;
	u64 rx_frames, tx_frames;
	u64 rx_no_buffer, rx_errors, tx_ring_full;
	u64 link_changes;
	bool link_up;
	bool link_known;
};

static struct r8169 devices[R8169_MAX];
static unsigned device_count;

/* --- Reaching the registers ------------------------------------------------ */

static inline void w8(struct r8169 *r, u32 off, u8 v)
{
	*(volatile u8 *)(r->mmio + off) = v;
}

static inline void w16(struct r8169 *r, u32 off, u16 v)
{
	*(volatile u16 *)(r->mmio + off) = v;
}

static inline void w32(struct r8169 *r, u32 off, u32 v)
{
	*(volatile u32 *)(r->mmio + off) = v;
}

static inline u8 rd8(struct r8169 *r, u32 off)
{
	return *(volatile u8 *)(r->mmio + off);
}

static inline u16 rd16(struct r8169 *r, u32 off)
{
	return *(volatile u16 *)(r->mmio + off);
}

/* A 64-bit ring address, written as two 32-bit halves, low first.
 *
 * Not one 64-bit store. The low half is what arms the register on this family
 * and a single quadword write is not guaranteed to reach the device as one
 * transaction in the order the two halves need -- so the card can latch a base
 * address whose upper half is still whatever was there before, and then read
 * its descriptors from an address that exists and is not the ring. Two stores,
 * low then high, is what the documentation describes and what every working
 * driver does. */
static void write_ring_address(struct r8169 *r, u32 off, paddr_t phys)
{
	w32(r, off, (u32)((u64)phys & 0xFFFFFFFFu));
	w32(r, off + 4, (u32)(((u64)phys >> 32) & 0xFFFFFFFFu));
}

/* --- Bringing the card to a known state ------------------------------------ */

/* The soft reset, with a deadline rather than a spin count.
 *
 * A count of loops measures the processor, not the card: the same number is a
 * millisecond on one machine and a microsecond on the next, so a reset that
 * "times out after 1000 tries" has no defined duration at all and will start
 * failing on faster hardware years after it was written. This waits a stated
 * length of real time.
 *
 * Returns false when the bit never cleared, which is a card that is not
 * answering, and must be reported rather than pressed on from -- every
 * register written after a failed reset is written into an unknown state. */
static bool reset_card(struct r8169 *r)
{
	u64 deadline;

	w8(r, R_CR, CR_RST);

	/* Ten milliseconds. The specification says the bit clears in under a
	 * millisecond; the margin is for a card behind a bridge that is busy,
	 * not for a card that is broken. */
	deadline = time_monotonic_ns() + 10000000ull;

	while (time_monotonic_ns() < deadline) {
		if (!(rd8(r, R_CR) & CR_RST))
			return true;
	}

	return false;
}

/* --- The rings ------------------------------------------------------------- */

/* Rebuilds one receive descriptor so the card may fill it again.
 *
 * The flag word is written whole rather than or-ed into, and EOR is re-applied
 * from the index every single time. The card *clears* the flags it consumed,
 * so a descriptor handed back with `opts1 |= DESC_OWN` keeps whatever the card
 * left behind -- and on the last descriptor of the ring that means EOR is
 * cleared exactly once, on the first wrap, after which the card walks off the
 * end of the ring. A ring that works for thirty-two frames and then corrupts
 * memory is a worse failure than one that never worked. */
static void give_rx_to_card(struct r8169 *r, unsigned i)
{
	struct rtl_desc *d = &r->rx_desc[i];
	u32 flags = DESC_OWN | (u32)RX_BUF_SIZE;

	if (i == RX_RING - 1)
		flags |= DESC_EOR;

	d->addr = (u64)(r->rx_buf[i]->page +
			(paddr_t)(r->rx_buf[i]->data - r->rx_buf[i]->head));
	d->opts2 = 0;

	/* The address must be visible to the card before the ownership bit
	 * that invites it to read the address. Two stores with nothing between
	 * them is a compiler and a processor both free to reorder, and the
	 * reordering is a card DMA-ing into whatever the previous buffer
	 * was. */
	__atomic_thread_fence(__ATOMIC_RELEASE);
	d->opts1 = flags;
}

/* Fills any receive slot that has no buffer. Held with the lock.
 *
 * Returns how many slots are stocked, because a ring that has quietly gone
 * empty looks exactly like a network with nothing on it -- the same reasoning
 * `virtio_net.c` gives for printing its posted count, and the same failure. */
static unsigned stock_rx(struct r8169 *r)
{
	unsigned i, stocked = 0;

	for (i = 0; i < RX_RING; i++) {
		if (!r->rx_buf[i]) {
			r->rx_buf[i] = netbuf_alloc();

			if (!r->rx_buf[i]) {
				r->rx_no_buffer++;
				continue;
			}

			give_rx_to_card(r, i);
		}

		stocked++;
	}

	return stocked;
}

/* Takes back every transmit descriptor the card has finished with. Held. */
static void collect_tx(struct r8169 *r)
{
	while (r->tx_tail != r->tx_head) {
		unsigned i = r->tx_tail % TX_RING;

		__atomic_thread_fence(__ATOMIC_ACQUIRE);

		if (r->tx_desc[i].opts1 & DESC_OWN)
			break;	/* still the card's */

		if (r->tx_buf[i]) {
			netbuf_free(r->tx_buf[i]);
			r->tx_buf[i] = NULL;
		}

		r->tx_tail++;
	}
}

/* --- What the cable is doing ----------------------------------------------- */

/* Reads the link, and says so when it changes.
 *
 * `net_device.link` is a field this kernel has carried since the stack was
 * written and that nothing has ever set to anything but a constant -- see
 * NW-003. It is a real, changing fact about a real card, and the only way it
 * becomes one is a driver that reads it from the silicon. */
static void update_link(struct r8169 *r)
{
	bool up = (rd8(r, R_PHYSTATUS) & PHY_LINK_OK) != 0;

	/* `link_known` rather than comparing against the zeroed field.
	 *
	 * Without it a card that comes up with no cable in it says nothing at
	 * all, because "down" is what the zeroed structure already claimed --
	 * so the one case where somebody needs to be told is the one case that
	 * stays silent. The first read always reports, whichever way it
	 * went. */
	if (r->link_known && up == r->link_up)
		return;

	if (r->link_known)
		r->link_changes++;

	r->link_known = true;
	r->link_up = up;

	if (r->ndev)
		r->ndev->link = up;

	if (up) {
		u8 s = rd8(r, R_PHYSTATUS);
		const char *speed = s & PHY_1000M ? "1000" :
				    s & PHY_100M  ? "100"  :
				    s & PHY_10M   ? "10"   : "?";

		kprintf("r8169: %s link up, %s Mb %s duplex\n",
			r->ndev ? r->ndev->name : "eth?", speed,
			(s & PHY_FULL_DUPLEX) ? "full" : "half");
	} else {
		kprintf("r8169: %s link down\n",
			r->ndev ? r->ndev->name : "eth?");
	}
}

/* --- Receive --------------------------------------------------------------- */

static void r8169_poll(struct net_device *dev)
{
	struct r8169 *r = dev->driver;
	u64 flags;

	flags = spin_lock_irq(&r->lock);

	collect_tx(r);
	update_link(r);

	for (;;) {
		unsigned i = r->rx_next % RX_RING;
		struct rtl_desc *d = &r->rx_desc[i];
		struct netbuf *b;
		u32 status;
		u32 len;

		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		status = d->opts1;

		if (status & DESC_OWN)
			break;		/* the card has not filled it */

		b = r->rx_buf[i];
		r->rx_buf[i] = NULL;
		r->rx_next++;

		if (!b)
			continue;

		/* A frame the card marked bad, or one split across two
		 * descriptors. The second is refused rather than reassembled:
		 * the receive buffer is larger than the largest frame this
		 * kernel accepts, so a multi-descriptor receive means the card
		 * is doing something this driver did not ask for, and guessing
		 * at what would be guessing at somebody's data. */
		if ((status & DESC_RX_ERROR) ||
		    (status & (DESC_FS | DESC_LS)) != (DESC_FS | DESC_LS)) {
			r->rx_errors++;
			dev->rx_errors++;
			r->rx_buf[i] = b;
			give_rx_to_card(r, i);
			continue;
		}

		len = status & DESC_LEN_MASK;

		/* The four bytes of frame check sequence, which the card
		 * counted and nothing above here wants. See the head of this
		 * file: getting this wrong is invisible at Ethernet and at IP
		 * and shows up as a wrong payload boundary much later. */
		if (len <= 4) {
			r->rx_errors++;
			dev->rx_errors++;
			r->rx_buf[i] = b;
			give_rx_to_card(r, i);
			continue;
		}

		len -= 4;

		if (len > ETH_FRAME_MAX) {
			/* Refused, not clipped. A frame cut to fit is a
			 * different frame and the far end cannot tell. */
			r->rx_errors++;
			dev->rx_errors++;
			r->rx_buf[i] = b;
			give_rx_to_card(r, i);
			continue;
		}

		b->len = len;
		r->rx_frames++;

		/* Handed up outside the lock: everything above Ethernet takes
		 * locks of its own, and this one is also taken by the
		 * interrupt. */
		spin_unlock_irq(&r->lock, flags);
		netdev_receive(dev, b);
		flags = spin_lock_irq(&r->lock);

		/* A fresh buffer for the slot just emptied. If memory is gone
		 * the slot stays empty and `stock_rx` below tries again, so a
		 * moment of pressure does not leave the ring permanently
		 * short. */
		if (!r->rx_buf[i]) {
			r->rx_buf[i] = netbuf_alloc();

			if (r->rx_buf[i])
				give_rx_to_card(r, i);
			else
				r->rx_no_buffer++;
		}
	}

	stock_rx(r);

	spin_unlock_irq(&r->lock, flags);
}

/* --- Transmit -------------------------------------------------------------- */

static bool r8169_transmit(struct net_device *dev, struct netbuf *b)
{
	struct r8169 *r = dev->driver;
	unsigned i;
	u32 opts;
	u64 flags;

	flags = spin_lock_irq(&r->lock);

	collect_tx(r);

	/* Full means the card has not finished with what is already out. The
	 * frame is dropped rather than queued behind the ring, for the reason
	 * `virtio_net.c` gives: a transmit queue in front of the device's own
	 * transmit queue is two queues with one purpose. */
	if (r->tx_head - r->tx_tail >= TX_RING) {
		r->tx_ring_full++;
		spin_unlock_irq(&r->lock, flags);
		netbuf_free(b);
		return false;
	}

	i = r->tx_head % TX_RING;

	r->tx_buf[i] = b;
	r->tx_desc[i].addr = (u64)(b->page + (paddr_t)(b->data - b->head));
	r->tx_desc[i].opts2 = 0;

	/* One descriptor carries the whole frame, so it is both the first and
	 * the last fragment. EOR from the index, for the reason in
	 * `give_rx_to_card`. */
	opts = DESC_OWN | DESC_FS | DESC_LS | (b->len & DESC_LEN_MASK);

	if (i == TX_RING - 1)
		opts |= DESC_EOR;

	__atomic_thread_fence(__ATOMIC_RELEASE);
	r->tx_desc[i].opts1 = opts;

	r->tx_head++;
	r->tx_frames++;

	/* The descriptor has to be in memory before the card is told to go and
	 * read it. */
	__atomic_thread_fence(__ATOMIC_RELEASE);
	w8(r, R_TPPOLL, TPPOLL_NPQ);

	spin_unlock_irq(&r->lock, flags);
	return true;
}

/* --- The interrupt --------------------------------------------------------- */

/* Almost nothing, and the rule is `irq.h`'s rather than this driver's: a
 * handler may not allocate and may not take a lock held outside interrupt
 * context. So it acknowledges -- which the card *will* lose if nobody does,
 * because the status bits latch and a card with unacknowledged status raises
 * no further interrupt -- and asks for the worker thread.
 *
 * `netdev_wake` is the thing that did not exist. Before it, the only way to
 * reach the worker was `netdev_receive`, which needs a buffer, which needs an
 * allocation, which a handler may not do. That is NW-001. */
static void r8169_interrupt(void *arg)
{
	struct r8169 *r = arg;
	u16 status = rd16(r, R_ISR);

	if (!status)
		return;		/* not ours, on a shared line */

	/* Acknowledged by writing the bits back. Done before the work is
	 * scheduled, not after: a frame arriving in between must set the bit
	 * again and raise another interrupt, and acknowledging afterwards
	 * would clear that one too. */
	w16(r, R_ISR, status);

	r->interrupts++;

	netdev_wake();
}

static const struct net_device_ops r8169_ops = {
	.transmit = r8169_transmit,
	.enable_interrupts = NULL,
	.poll = r8169_poll,
};

/* --- Setting it up --------------------------------------------------------- */

static void start_card(struct r8169 *r)
{
	u16 cplus;

	/* The configuration registers are read-only until unlocked, and a
	 * write that lands while they are locked is accepted and dropped. */
	w8(r, R_CFG9346, CFG9346_UNLOCK);

	/* C+ mode: no hardware VLAN tag removal and no receive checksum
	 * offload. Both are refused rather than taken, and the reason is the
	 * same for both: this kernel computes and checks its own checksums,
	 * and a card that silently rewrites a frame on the way in makes the
	 * one test that proves the checksum code untestable. Read, masked and
	 * written back rather than assigned, because the other bits in this
	 * register are set by the card's own initialisation. */
	cplus = rd16(r, R_CPLUSCMD);
	cplus &= (u16)~(CPLUS_RX_VLAN | CPLUS_RX_CHECKSUM);
	w16(r, R_CPLUSCMD, cplus);

	w16(r, R_RMS, RX_BUF_SIZE);
	w8(r, R_MTPS, 0x3B);

	write_ring_address(r, R_RDSAR, r->ring_page + RX_RING_OFFSET);
	write_ring_address(r, R_TNPDS, r->ring_page + TX_RING_OFFSET);

	/* Receive and transmit on **before** the two configuration registers
	 * below, which is the order this family documents and the reverse of
	 * what reads naturally. A card configured while disabled latches some
	 * of these on the enable edge and not others, and the result is a card
	 * that receives its own broadcasts and nothing else. */
	w8(r, R_CR, CR_RX_ENABLE | CR_TX_ENABLE);

	w32(r, R_TCR, ((u32)BURST_UNLIMITED << TCR_DMA_SHIFT) |
		      ((u32)TCR_IFG_NORMAL << TCR_IFG_SHIFT));

	/* Ours, broadcast and multicast. **Not** promiscuous: a card that
	 * accepts every frame on the wire makes the address filter untestable,
	 * and a stack that appears to work only because it is seeing traffic
	 * addressed to somebody else is the kind of thing that survives until
	 * it reaches a switch. */
	w32(r, R_RCR, RCR_ACCEPT_MY_PHYS | RCR_ACCEPT_BROADCAST |
		      RCR_ACCEPT_MULTICAST |
		      ((u32)BURST_UNLIMITED << RCR_DMA_SHIFT) |
		      ((u32)BURST_UNLIMITED << RCR_FIFO_SHIFT));

	/* Every multicast group, which is what the stack expects until it has
	 * a filter to ask for. Said here rather than left at whatever reset
	 * produced. */
	w32(r, R_MAR0, 0xFFFFFFFFu);
	w32(r, R_MAR0 + 4, 0xFFFFFFFFu);

	w8(r, R_CFG9346, CFG9346_LOCK);

	/* Anything latched from before the reset is acknowledged, and the mask
	 * is left **shut**.
	 *
	 * Opening it here would be a card raising interrupts before anything
	 * has been registered to take them, and on a legacy line that is not
	 * merely untidy: the card asserts the wire and holds it until somebody
	 * acknowledges the status register, nobody does, and the machine takes
	 * the same interrupt for ever. `irq.h` names that failure exactly --
	 * "a line nothing claims that keeps firing is a device nobody turned
	 * off, and that is a live-lock the machine cannot escape".
	 *
	 * `arm_interrupts` below opens it, and only once there is a handler. */
	w16(r, R_ISR, 0xFFFF);
	w16(r, R_IMR, 0);
}

/* Opens the interrupt mask, and only when there is somewhere for one to go.
 *
 * A card that could not be given a vector is left with the mask shut and is
 * polled. That is a real difference in behaviour and not a tidy-up: masked, it
 * never asserts anything, so a machine with no message-signalled interrupts
 * runs this card correctly instead of hanging on a line nobody claimed. */
static void arm_interrupts(struct r8169 *r)
{
	if (!r->interrupting)
		return;

	w16(r, R_ISR, 0xFFFF);
	w16(r, R_IMR, INT_WANTED);
}

bool r8169_attach(const struct pci_device *d)
{
	struct r8169 *r;
	struct mac_addr mac;
	char name[NET_NAME_MAX];
	unsigned i;
	unsigned stocked;

	if (!d || d->vendor != REALTEK_VENDOR)
		return false;

	switch (d->device) {
	case RTL_8169:
	case RTL_8168:
	case RTL_8161:
	case RTL_8136:
		break;

	case RTL_8125:
		/* Declined by name, and this is a decision rather than an
		 * omission.
		 *
		 * The RTL8125 keeps the sixteen-byte descriptor and moves the
		 * things this driver would write: the interrupt mask and
		 * status are thirty-two bits at 0x38 and 0x3C rather than
		 * sixteen at 0x3C and 0x3E, so every access above would write
		 * the mask into the poll register and read status out of the
		 * mask. It also wants its own reset sequence.
		 *
		 * Accepting it on the strength of the family name would
		 * produce a driver that attaches, prints a hardware address,
		 * and never receives a frame -- which is worse than refusing,
		 * because the boot summary would say a card was found. There
		 * is an RTL8125 in the desktop and it is not tested here, so
		 * it is not claimed here. */
		kprintf("r8169: RTL8125 at %u:%u.%u is not this driver's -- "
			"its interrupt registers moved and it is untested\n",
			d->bus, d->slot, d->func);
		return false;

	default:
		return false;
	}

	if (device_count >= R8169_MAX)
		return false;

	r = &devices[device_count];
	kmemset(r, 0, sizeof(*r));
	r->pci = *d;
	r->device_id = d->device;
	r->revision = d->revision;
	spin_init(&r->lock, "r8169");

	/* The registers are offered twice: base address register 0 is an I/O
	 * port range and 2 is a memory window onto the same registers. The
	 * memory one is taken, because `pci_map_bar` serves memory and refuses
	 * I/O, and because I/O ports are an x86 instruction that `core/` may
	 * not contain. A card that offers only the I/O range is declined and
	 * says so rather than being driven through a window that is not
	 * there. */
	r->mmio = pci_map_bar(d, 2, 0, 0x100);

	if (!r->mmio) {
		kprintf("r8169: the card at %u:%u.%u has no memory window "
			"(bar2 %s)\n", d->bus, d->slot, d->func,
			d->bar_is_io[2] ? "is I/O ports" : "is unassigned");
		return false;
	}

	if (!reset_card(r)) {
		kprintf("r8169: the card at %u:%u.%u did not finish a reset "
			"in 10 ms; leaving it alone\n",
			d->bus, d->slot, d->func);
		return false;
	}

	/* The hardware address, read after the reset because the reset is what
	 * reloads it from the card's own storage. */
	for (i = 0; i < MAC_LEN; i++)
		mac.b[i] = rd8(r, R_IDR0 + i);

	/* A card that answers every register as ones is a card that is not
	 * decoding -- which reads exactly like a card that is there. Refused,
	 * because the alternative is registering a device with a broadcast
	 * address that would then answer ARP for everybody. */
	if (mac_equal(&mac, &MAC_BROADCAST) || mac_equal(&mac, &MAC_ZERO)) {
		char text[20];

		kprintf("r8169: the card at %u:%u.%u reports %s as its "
			"address, which is not one; not registering it\n",
			d->bus, d->slot, d->func, mac_format(&mac, text));
		return false;
	}

	r->ring_page = pmm_alloc_page();

	if (!r->ring_page) {
		kputs("r8169: no memory for the descriptor rings\n");
		return false;
	}

	{
		u8 *base = phys_to_virt(r->ring_page);

		kmemset(base, 0, 4096);
		r->rx_desc = (struct rtl_desc *)(base + RX_RING_OFFSET);
		r->tx_desc = (struct rtl_desc *)(base + TX_RING_OFFSET);
	}

	/* EOR on the last transmit descriptor, once, before the card is
	 * started. The receive ring gets its EOR from `give_rx_to_card`, which
	 * re-applies it on every hand-back; the transmit ring's last entry is
	 * rebuilt on every send and gets it there. This is the state the card
	 * reads before either has happened. */
	r->tx_desc[TX_RING - 1].opts1 = DESC_EOR;

	stocked = stock_rx(r);

	if (!stocked) {
		kputs("r8169: no memory for a single receive buffer\n");
		pmm_free_pages(r->ring_page, 1);
		return false;
	}

	start_card(r);

	if (!netdev_name("eth", name, sizeof(name))) {
		kputs("r8169: no free device name\n");
		pmm_free_pages(r->ring_page, 1);
		return false;
	}

	r->ndev = netdev_register(name, &r8169_ops, r, &mac);

	if (!r->ndev) {
		pmm_free_pages(r->ring_page, 1);
		return false;
	}

	/* Read from the silicon rather than assumed. A card that is up and a
	 * card with a cable in it are two different facts, and this kernel has
	 * only ever had a field for the second that nothing wrote -- NW-003. */
	update_link(r);

	/* And an interrupt, if this machine can deliver one. False is not a
	 * failure: it means the card signals the old way or the machine has no
	 * message-signalled interrupts, and the polling path above is what
	 * happens then. Which one is in force is printed, because a driver
	 * quietly polling when it believes it is interrupt-driven is the
	 * failure this whole file was written to find. */
	r->interrupting = arch_pci_request_interrupt(&r->pci, 0,
						     r8169_interrupt, r,
						     "r8169");

	arm_interrupts(r);

	device_count++;

	{
		char text[20];

		kprintf("r8169: %s at %s, RTL%04X rev %u, %u receive buffers, "
			"%s\n",
			name, mac_format(&mac, text), r->device_id,
			r->revision, stocked,
			r->interrupting ? "by interrupt" : "polled");
	}

	return true;
}

unsigned r8169_count(void)
{
	return device_count;
}

void r8169_print_summary(void)
{
	unsigned i;

	for (i = 0; i < device_count; i++) {
		struct r8169 *r = &devices[i];
		unsigned stocked = 0;
		unsigned j;

		for (j = 0; j < RX_RING; j++)
			if (r->rx_buf[j])
				stocked++;

		kprintf("  %s         : %u in, %u out, %u stocked, "
			"%u interrupt(s)",
			r->ndev ? r->ndev->name : "eth?",
			(unsigned)r->rx_frames, (unsigned)r->tx_frames,
			stocked, (unsigned)r->interrupts);

		if (r->rx_errors || r->rx_no_buffer || r->tx_ring_full)
			kprintf(", %u bad, %u no buffer, %u ring full",
				(unsigned)r->rx_errors,
				(unsigned)r->rx_no_buffer,
				(unsigned)r->tx_ring_full);

		kprintf("\n");

		kprintf("               : link %s, %u change(s), "
			"transmit ring %u of %u in flight\n",
			r->link_up ? "up" : "down",
			(unsigned)r->link_changes,
			r->tx_head - r->tx_tail, TX_RING);

		/* Said outright rather than left to be inferred, for the
		 * reason virtio-net says it: an empty ring and a quiet network
		 * produce the same frame count. */
		if (!stocked)
			kprintf("               : the receive ring is empty -- "
				"this card can no longer receive\n");

		/* And the one a reader would otherwise have to work out. A
		 * card that asked for an interrupt and has never had one is
		 * being carried entirely by the polling path, which works and
		 * is not what it says on the line above. */
		if (r->interrupting && !r->interrupts)
			kprintf("               : it has a vector and has "
				"never been interrupted -- everything here "
				"arrived by polling\n");
	}
}

/* --- The receive path, with the card played by this file ------------------- */

/* NW-007 says the frame length is the likeliest single fault in either driver,
 * and that booting with the card covers only half of it. For this driver there
 * is no booting with the card at all: the rig emulates no Realtek gigabit
 * part, so without what follows, `r8169_poll` has never executed a single
 * instruction.
 *
 * --- What is real here and what is not ---
 *
 * **The code under test is the real receive loop.** Nothing is lifted out into
 * a testable copy, which is the trap this project keeps finding: a test that
 * proves a copy while the original goes on being the thing that runs.
 *
 * What is faked is the far side. A page of ordinary memory stands in for the
 * register window, and `card_delivers` below writes the descriptor fields the
 * silicon would have written. The driver cannot tell the difference, because a
 * network card *is* a thing that writes descriptors and raises interrupts --
 * and this file writes descriptors.
 *
 * --- What it therefore cannot prove, stated so it is not assumed ---
 *
 * That the real chip behaves the way this file pretends it does. Every
 * register offset, the reset sequence, the meaning of every bit in `R_RCR`,
 * whether `R_TPPOLL` actually starts a transmission -- none of that is touched
 * here and none of it can be. Those are still checked by running on silicon
 * and nowhere else.
 *
 * What it does prove is the arithmetic and the ring bookkeeping: that four
 * bytes of frame check sequence come off, that a frame too short to contain
 * one is refused rather than underflowed, that a frame the card marked bad
 * does not reach the stack, and that the end-of-ring bit survives a lap.
 *
 * --- One thing this test does that shows up in the boot summary ---
 *
 * It really does receive frames, so the Ethernet counters really do move. The
 * frames are addressed to the test device and carry an ethertype nothing
 * handles, so they are dropped one layer up and counted there. That is a true
 * line about work that happened, not noise -- and hiding it would mean a
 * summary that under-reports on purpose, which is the fault half this
 * register is about.
 */

/* Kept apart from `devices[]` so a test never consumes a slot a real card
 * would have had, and so `r8169_count()` does not report it. */
static struct r8169 test_card;

/* What the card leaves behind when it has filled a descriptor.
 *
 * Two things here are deliberately harsher than the hardware, because a test
 * that assumes the gentler behaviour cannot fail when the driver depends on
 * it:
 *
 *   - **The length includes the four-byte frame check sequence.** That is the
 *     fact the whole file exists to pin down.
 *
 *   - **The end-of-ring bit is cleared.** Real Realtek silicon preserves it in
 *     the write-back -- Linux's driver reads it back out of the descriptor and
 *     re-applies what it found. This driver instead derives it from the index
 *     on every hand-back, which does not depend on the card preserving
 *     anything. Simulating a card that clears it is what makes that difference
 *     testable: a driver that or-ed `DESC_OWN` into whatever was there would
 *     lose the bit on the first wrap and walk off the end of the ring, and
 *     with a gentler fake it would pass.
 */
static void card_delivers(struct r8169 *r, unsigned i, u32 frame_len, u32 extra)
{
	struct netbuf *b = r->rx_buf[i];

	/* A frame addressed to this device with an ethertype nothing claims,
	 * so it is accepted by Ethernet as ours and dropped there rather than
	 * being parsed as a malformed IP packet further up. */
	if (b) {
		kmemcpy(b->data, &r->ndev->mac, MAC_LEN);
		kmemset(b->data + MAC_LEN, 0x02, MAC_LEN);
		b->data[12] = 0x88;	/* reserved for experiments */
		b->data[13] = 0xB5;
	}

	__atomic_thread_fence(__ATOMIC_RELEASE);

	/* No DESC_EOR, whatever the index -- see above. */
	r->rx_desc[i].opts1 = (frame_len + 4) | DESC_FS | DESC_LS | extra;
}

/* Hands one frame to the driver through its own loop and says what came out
 * the other side: how many bytes, and how many frames.
 *
 * `netdev_receive` adds `b->len` to `rx_bytes` and one to `rx_packets`, which
 * is where the driver's answer becomes readable without reaching into a buffer
 * that has already been given away. Using counters the driver does not know
 * are being watched is the point: the numbers are produced by the ordinary
 * path.
 *
 * **Both, not just the bytes.** Measuring bytes alone made one assertion below
 * unable to fail: with the short-length guard removed, a four-byte descriptor
 * -- a frame check sequence and nothing else -- becomes a frame of length
 * zero, which is passed up the stack and adds *nothing* to `rx_bytes`. The
 * test saw no bytes, which is what it saw when the frame was correctly
 * refused, and reported a pass. Found by removing the guard on purpose and
 * watching the test stay green. The frame count is what tells "refused" from
 * "accepted, and empty". */
static void deliver_one(struct r8169 *r, struct net_device *dev,
			u32 frame_len, u32 extra, u64 *bytes, u64 *frames)
{
	u64 b0 = dev->rx_bytes;
	u64 f0 = dev->rx_packets;

	card_delivers(r, r->rx_next % RX_RING, frame_len, extra);
	r8169_poll(dev);

	*bytes = dev->rx_bytes - b0;
	*frames = dev->rx_packets - f0;
}

bool r8169_self_test(void)
{
	struct r8169 *r = &test_card;
	struct net_device *dev;
	char name[NET_NAME_MAX];
	static const struct mac_addr mac = { { 0x02, 0x4E, 0x57, 0, 0, 0x10 } };
	paddr_t regs;
	unsigned i;
	u64 got, frames;
	bool ok = true;

	kmemset(r, 0, sizeof(*r));
	spin_init(&r->lock, "r8169-test");

	/* Declared already known and down, so `update_link` finds nothing
	 * changed and stays quiet. A test that prints a line about a cable on
	 * every boot makes the summary worse, and there is no cable. */
	r->link_known = true;
	r->link_up = false;

	regs = pmm_alloc_page();
	r->ring_page = pmm_alloc_page();

	if (!regs || !r->ring_page) {
		kputs("  r8169: no memory for the test's rings\n");

		if (regs)
			pmm_free_pages(regs, 1);
		if (r->ring_page)
			pmm_free_pages(r->ring_page, 1);

		return false;
	}

	/* A page of ordinary memory where the registers would be. Every `w8`
	 * and `rd8` in the driver lands here and changes nothing in the
	 * machine -- which is also why this test can run on a board that has
	 * no PCI at all. */
	r->mmio = phys_to_virt(regs);
	kmemset((void *)r->mmio, 0, 4096);

	{
		u8 *base = phys_to_virt(r->ring_page);

		kmemset(base, 0, 4096);
		r->rx_desc = (struct rtl_desc *)(base + RX_RING_OFFSET);
		r->tx_desc = (struct rtl_desc *)(base + TX_RING_OFFSET);
	}

	r->tx_desc[TX_RING - 1].opts1 = DESC_EOR;

	if (!stock_rx(r)) {
		kputs("  r8169: no memory for the test's buffers\n");
		pmm_free_pages(regs, 1);
		pmm_free_pages(r->ring_page, 1);
		return false;
	}

	if (!netdev_name("eth", name, sizeof(name))) {
		kputs("  r8169: no name for the test device\n");
		ok = false;
		goto out;
	}

	dev = netdev_register(name, &r8169_ops, r, &mac);

	if (!dev) {
		kputs("  r8169: the test device was refused\n");
		ok = false;
		goto out;
	}

	r->ndev = dev;

	/* The receive queue is drained before anything is measured.
	 *
	 * `netdev_self_test` deliberately overflows that queue, and a full
	 * queue makes `netdev_receive` drop the frame **without** adding to
	 * `rx_bytes` -- so without this line every assertion below would read
	 * zero bytes and blame the driver's arithmetic for the previous test's
	 * leftovers. */
	netdev_service();

	/* --- the four bytes ------------------------------------------------ */

	deliver_one(r, dev, 64, 0, &got, &frames);

	if (got != 64 || frames != 1) {
		kprintf("  r8169: a 64-byte frame delivered as 68 came up as "
			"%u bytes -- the frame check sequence is not coming "
			"off correctly\n", (unsigned)got);
		ok = false;
	}

	/* And again at the largest frame the driver accepts, because the
	 * length is also what the `> ETH_FRAME_MAX` refusal is measured
	 * against -- and a driver that subtracted after the comparison rather
	 * than before would refuse this one. */
	deliver_one(r, dev, ETH_FRAME_MAX - 4, 0, &got, &frames);

	if (got != ETH_FRAME_MAX - 4 || frames != 1) {
		kprintf("  r8169: the largest acceptable frame came up as %u "
			"bytes rather than %u\n",
			(unsigned)got, (unsigned)(ETH_FRAME_MAX - 4));
		ok = false;
	}

	/* --- a frame the card marked bad ------------------------------------ */

	deliver_one(r, dev, 64, DESC_RX_ERROR, &got, &frames);

	if (got != 0 || frames != 0) {
		kputs("  r8169: a frame the card marked bad was passed up\n");
		ok = false;
	}

	/* --- a length too short to hold a check sequence ---------------------
	 *
	 * Two bytes. Subtracting four underflows an unsigned length to about
	 * four billion, which is why the driver tests before it subtracts
	 * rather than after -- and why this case is worth its own assertion
	 * rather than being assumed to fall out of the one above. */

	deliver_one(r, dev, 0, 0, &got, &frames);	/* delivered as 4 */

	if (got != 0 || frames != 0) {
		kprintf("  r8169: a frame with nothing but a check sequence "
			"was passed up -- %u frame(s), %u byte(s)\n",
			(unsigned)frames, (unsigned)got);
		ok = false;
	}

	/* --- a fragment of a larger frame ------------------------------------
	 *
	 * Neither first nor last. The driver refuses these rather than
	 * reassembling, because its receive buffer is larger than any frame it
	 * accepts -- so a split frame means the card is doing something it was
	 * not asked to, and guessing at what would be guessing at somebody's
	 * data. */

	card_delivers(r, r->rx_next % RX_RING, 64, 0);
	r->rx_desc[r->rx_next % RX_RING].opts1 &= ~(u32)DESC_LS;
	{
		u64 before = dev->rx_packets;

		r8169_poll(dev);

		if (dev->rx_packets != before) {
			kputs("  r8169: half of a split frame was passed up "
			      "as though it were whole\n");
			ok = false;
		}
	}

	/* --- all the way round, and the bit that says where the ring ends ---
	 *
	 * One full lap plus two. The failure this catches survives exactly one
	 * lap, so one lap is the shortest run that can catch it. */

	for (i = 0; i < RX_RING + 2; i++) {
		deliver_one(r, dev, 64, 0, &got, &frames);

		/* Walked up as they go rather than left to pile up. The
		 * receive queue holds 64 and this lap alone produces 34, so
		 * without draining it the queue would be most of the way full
		 * when the test ends -- and `netdev_receive` counts a frame
		 * dropped for a full queue in the same `overflowed` number the
		 * bounded-queue test uses to prove its bound. Borrowing that
		 * counter for ordinary test traffic would make the one number
		 * that says the bound works say something else as well. */
		if ((i % 8) == 7)
			netdev_service();
	}

	if (!(r->rx_desc[RX_RING - 1].opts1 & DESC_EOR)) {
		kputs("  r8169: the end-of-ring bit was lost on the first "
		      "wrap -- the card would read descriptors past the end "
		      "of the ring\n");
		ok = false;
	}

	for (i = 0; i < RX_RING - 1; i++) {
		if (r->rx_desc[i].opts1 & DESC_EOR) {
			kprintf("  r8169: descriptor %u claims to end the "
				"ring, and it does not\n", i);
			ok = false;
			break;
		}
	}

	/* The frames this test produced are walked up and dropped now rather
	 * than left on the queue for whatever runs next. */
	netdev_service();

	netdev_forget_last();

out:
	for (i = 0; i < RX_RING; i++) {
		netbuf_free(r->rx_buf[i]);
		r->rx_buf[i] = NULL;
	}

	pmm_free_pages(regs, 1);
	pmm_free_pages(r->ring_page, 1);
	kmemset(r, 0, sizeof(*r));

	return ok;
}
