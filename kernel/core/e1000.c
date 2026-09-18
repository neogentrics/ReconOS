/* e1000: Intel's gigabit controller, and the driver that can be *run* here.
 *
 * `r8169.c` is the card this project actually owns -- two of them in the
 * server, bonded. This one is the card the verification rig can boot, because
 * QEMU emulates an 82540EM and emulates no Realtek gigabit part at all. So the
 * two files are written together on purpose and they are not redundant:
 *
 *   r8169  is real silicon that the rig cannot reach
 *   e1000  is emulated silicon that the rig reaches on every run
 *
 * A driver that cannot be run is a driver whose faults are opinions. Having
 * both means the claims this pair makes about the *interface* -- NW-001 to
 * NW-007 -- are made by something that boots, receives a DHCP lease and gets
 * an answer to a ping, rather than by something that only compiles.
 *
 * --- Where it differs from the Realtek, which is the point ---
 *
 * Two real network cards designed by different companies twenty years apart
 * disagree about almost everything, and each disagreement is a place the
 * interface above could have been shaped by one of them:
 *
 *   - **The ring position is a register.** Realtek publishes nothing and the
 *     driver walks its own way round; Intel has a head and a tail register per
 *     ring and the tail is how the card is told there is more. So "where am I
 *     up to" is driver state in one and hardware state in the other.
 *
 *   - **The card strips the CRC.** `RCTL_SECRC` makes the length exclude the
 *     four-byte frame check sequence. The Realtek cannot be asked to, so that
 *     driver subtracts four and this one must not -- and a subtraction copied
 *     from one to the other is four bytes off a frame with nothing to
 *     announce it.
 *
 *   - **The address comes from a serial EEPROM**, one sixteen-bit word at a
 *     time through a register that has to be polled, rather than being
 *     readable as six bytes.
 *
 * --- What this deliberately does not do ---
 *
 * No checksum offload, no segmentation offload, no interrupt moderation, no
 * multiple queues. Each of those is a real feature of the silicon and each
 * would be a thing the stack above cannot currently express -- and adding a
 * driver capability that nothing can ask for is how an interface acquires a
 * shape nobody chose.
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

#define INTEL_VENDOR	0x8086

/* The 8254x parts that share this register layout and this descriptor. The
 * 82540EM is what QEMU presents; the others are the same silicon family and
 * are listed because they are what turns up in real machines of that era.
 *
 * Not listed, deliberately: the 82574L that QEMU calls `e1000e`, and the
 * 82576 it calls `igb`. Both moved the interrupt registers and both want a
 * different initialisation, and claiming them here on the strength of the name
 * would produce a card that attaches and never receives -- the same refusal
 * `r8169.c` makes for the RTL8125, for the same reason. */
#define E1000_82540EM	0x100E
#define E1000_82545EM	0x100F
#define E1000_82540EP	0x1015
#define E1000_82541GI	0x1076
#define E1000_82547GI	0x1075
#define E1000_82544GC	0x1004

/* --- Registers ------------------------------------------------------------- */

#define R_CTRL		0x0000
#define R_STATUS	0x0008
#define R_EERD		0x0014
#define R_ICR		0x00C0	/* cause -- reading it clears it */
#define R_IMS		0x00D0	/* mask set */
#define R_IMC		0x00D8	/* mask clear */
#define R_RCTL		0x0100
#define R_TCTL		0x0400
#define R_TIPG		0x0410

#define R_RDBAL		0x2800
#define R_RDBAH		0x2804
#define R_RDLEN		0x2808
#define R_RDH		0x2810
#define R_RDT		0x2818

#define R_TDBAL		0x3800
#define R_TDBAH		0x3804
#define R_TDLEN		0x3808
#define R_TDH		0x3810
#define R_TDT		0x3818

#define R_MTA		0x5200	/* 128 words of multicast filter */
#define R_RAL0		0x5400
#define R_RAH0		0x5404

/* R_CTRL */
#define CTRL_FD		(1u << 0)
#define CTRL_ASDE	(1u << 5)	/* auto-detect speed and duplex */
#define CTRL_SLU	(1u << 6)	/* set link up */
#define CTRL_RST	(1u << 26)

/* R_STATUS */
#define STATUS_FD	(1u << 0)
#define STATUS_LU	(1u << 1)	/* link up */
#define STATUS_SPEED	(3u << 6)
#define STATUS_SPEED_SHIFT 6

/* R_EERD, in the layout the 8254x uses: the address goes in the high half of
 * the low word and the data comes back in the high word. */
#define EERD_START	(1u << 0)
#define EERD_DONE	(1u << 4)
#define EERD_ADDR_SHIFT	8
#define EERD_DATA_SHIFT	16

/* R_RCTL */
#define RCTL_EN		(1u << 1)
#define RCTL_UPE	(1u << 3)	/* every unicast -- not set */
#define RCTL_MPE	(1u << 4)	/* every multicast -- not set */
#define RCTL_LPE	(1u << 5)	/* frames over 1522 -- not set */
#define RCTL_BAM	(1u << 15)	/* broadcast */
#define RCTL_SZ_2048	(0u << 16)
#define RCTL_SECRC	(1u << 26)	/* strip the frame check sequence */

/* R_TCTL */
#define TCTL_EN		(1u << 1)
#define TCTL_PSP	(1u << 3)	/* pad frames under 64 bytes */
#define TCTL_CT_SHIFT	4
#define TCTL_COLD_SHIFT	12

/* Interrupt causes, which R_ICR reports and R_IMS asks for. */
#define INT_TXDW	(1u << 0)	/* a transmit descriptor came back */
#define INT_LSC		(1u << 2)	/* the link changed */
#define INT_RXDMT0	(1u << 4)	/* the receive ring is running low */
#define INT_RXO		(1u << 6)	/* the card ran out and dropped */
#define INT_RXT0	(1u << 7)	/* a frame arrived */

#define INT_WANTED	(INT_TXDW | INT_LSC | INT_RXDMT0 | INT_RXO | INT_RXT0)

/* --- Descriptors ----------------------------------------------------------- */

struct e1000_rx_desc {
	u64 addr;
	u16 length;
	u16 checksum;
	u8  status;
	u8  errors;
	u16 special;
};

struct e1000_tx_desc {
	u64 addr;
	u16 length;
	u8  cso;
	u8  cmd;
	u8  status;
	u8  css;
	u16 special;
};

_Static_assert(sizeof(struct e1000_rx_desc) == 16, "sixteen bytes");
_Static_assert(sizeof(struct e1000_tx_desc) == 16, "sixteen bytes");

#define RX_STATUS_DD	0x01	/* the card has finished with it */
#define RX_STATUS_EOP	0x02	/* and it is the end of a frame */

#define TX_CMD_EOP	0x01
#define TX_CMD_IFCS	0x02	/* have the card append the CRC */
#define TX_CMD_RS	0x08	/* and report when it is done */
#define TX_STATUS_DD	0x01

/* The ring length register is in **bytes** and must be a multiple of 128, so
 * the descriptor count must be a multiple of eight. Asserted rather than
 * remembered: a ring of 20 descriptors is 320 bytes, the card rounds the
 * register down to 256, and sixteen of the twenty are never looked at. */
#define RX_RING		32
#define TX_RING		32

_Static_assert((RX_RING * 16) % 128 == 0, "the receive ring must be a "
					  "multiple of 128 bytes");
_Static_assert((TX_RING * 16) % 128 == 0, "the transmit ring must be a "
					  "multiple of 128 bytes");

/* What RCTL_SZ_2048 means, and the card will write exactly this much. */
#define RX_BUF_SIZE	2048

_Static_assert(RX_BUF_SIZE >= ETH_FRAME_MAX, "a whole frame must fit");
_Static_assert(NET_HEADROOM + RX_BUF_SIZE <= NET_BUF_CAPACITY,
	       "the card would write past the end of the buffer");

#define RX_RING_OFFSET	0
#define TX_RING_OFFSET	2048

_Static_assert(RX_RING_OFFSET + RX_RING * 16 <= TX_RING_OFFSET, "rings meet");
_Static_assert(TX_RING_OFFSET + TX_RING * 16 <= 4096, "ring past the page");

#define E1000_MAX	4

struct e1000 {
	struct pci_device pci;
	volatile u8 *mmio;
	struct net_device *ndev;
	struct spinlock lock;

	paddr_t ring_page;
	struct e1000_rx_desc *rx_desc;
	struct e1000_tx_desc *tx_desc;

	struct netbuf *rx_buf[RX_RING];
	struct netbuf *tx_buf[TX_RING];

	unsigned rx_next;
	unsigned tx_head;
	unsigned tx_tail;

	bool interrupting;
	bool link_up, link_known;
	u16 device_id;
	bool mac_from_eeprom;

	u64 interrupts;
	u64 rx_frames, tx_frames;
	u64 rx_no_buffer, rx_errors, tx_ring_full;
	u64 link_changes;
};

static struct e1000 devices[E1000_MAX];
static unsigned device_count;

/* --- Reaching the registers ------------------------------------------------ */

static inline void wr(struct e1000 *e, u32 off, u32 v)
{
	*(volatile u32 *)(e->mmio + off) = v;
}

static inline u32 rr(struct e1000 *e, u32 off)
{
	return *(volatile u32 *)(e->mmio + off);
}

/* --- The address, a word at a time ----------------------------------------- */

/* Reads one sixteen-bit word out of the card's serial EEPROM.
 *
 * With a deadline rather than a spin count, for the reason `r8169.c` gives at
 * its reset: a loop count measures the processor and not the device, so the
 * same number is a millisecond on one machine and nothing at all on the next.
 *
 * False when the card never reported the read done -- which on some parts
 * means there is no EEPROM fitted, an ordinary thing rather than a fault, and
 * the caller falls back to the address register. */
static bool eeprom_word(struct e1000 *e, u8 word, u16 *out)
{
	u64 deadline;

	wr(e, R_EERD, ((u32)word << EERD_ADDR_SHIFT) | EERD_START);

	deadline = time_monotonic_ns() + 5000000ull;	/* 5 ms */

	while (time_monotonic_ns() < deadline) {
		u32 v = rr(e, R_EERD);

		if (v & EERD_DONE) {
			*out = (u16)(v >> EERD_DATA_SHIFT);
			return true;
		}
	}

	return false;
}

/* The hardware address, from the address register if the card has already
 * loaded one and from the EEPROM if it has not.
 *
 * That order rather than the other way round, and the reason is not
 * preference: the address register is what the card will actually *filter on*.
 * Reading the EEPROM first and believing it would mean a driver whose idea of
 * its own address differs from the card's, which is a machine that sends
 * correct ARP replies for an address it then refuses to receive. */
static bool read_mac(struct e1000 *e, struct mac_addr *out)
{
	u32 ral = rr(e, R_RAL0);
	u32 rah = rr(e, R_RAH0);
	unsigned i;

	/* Bit 31 of the high half is "this entry is valid". */
	if ((rah & (1u << 31)) && (ral || (rah & 0xFFFF))) {
		out->b[0] = (u8)(ral);
		out->b[1] = (u8)(ral >> 8);
		out->b[2] = (u8)(ral >> 16);
		out->b[3] = (u8)(ral >> 24);
		out->b[4] = (u8)(rah);
		out->b[5] = (u8)(rah >> 8);
		e->mac_from_eeprom = false;
		return true;
	}

	for (i = 0; i < 3; i++) {
		u16 w;

		if (!eeprom_word(e, (u8)i, &w))
			return false;

		out->b[i * 2] = (u8)(w & 0xFF);
		out->b[i * 2 + 1] = (u8)(w >> 8);
	}

	e->mac_from_eeprom = true;

	/* Written back, so the card filters on the address this driver is
	 * about to tell the rest of the kernel it has. */
	wr(e, R_RAL0, (u32)out->b[0] | ((u32)out->b[1] << 8) |
		      ((u32)out->b[2] << 16) | ((u32)out->b[3] << 24));
	wr(e, R_RAH0, (u32)out->b[4] | ((u32)out->b[5] << 8) | (1u << 31));

	return true;
}

/* --- The rings ------------------------------------------------------------- */

static void give_rx_to_card(struct e1000 *e, unsigned i)
{
	struct e1000_rx_desc *d = &e->rx_desc[i];
	struct netbuf *b = e->rx_buf[i];

	d->addr = (u64)(b->page + (paddr_t)(b->data - b->head));
	d->length = 0;
	d->checksum = 0;
	d->errors = 0;
	d->special = 0;

	/* The address must be in memory before the status byte is cleared,
	 * because a cleared status is what invites the card to use the
	 * address. */
	__atomic_thread_fence(__ATOMIC_RELEASE);
	d->status = 0;
}

static unsigned stock_rx(struct e1000 *e)
{
	unsigned i, stocked = 0;

	for (i = 0; i < RX_RING; i++) {
		if (!e->rx_buf[i]) {
			e->rx_buf[i] = netbuf_alloc();

			if (!e->rx_buf[i]) {
				e->rx_no_buffer++;
				continue;
			}

			give_rx_to_card(e, i);
		}

		stocked++;
	}

	return stocked;
}

static void collect_tx(struct e1000 *e)
{
	while (e->tx_tail != e->tx_head) {
		unsigned i = e->tx_tail % TX_RING;

		__atomic_thread_fence(__ATOMIC_ACQUIRE);

		if (!(e->tx_desc[i].status & TX_STATUS_DD))
			break;

		if (e->tx_buf[i]) {
			netbuf_free(e->tx_buf[i]);
			e->tx_buf[i] = NULL;
		}

		e->tx_tail++;
	}
}

/* How far the receive tail may be advanced: how many descriptors, counting
 * from `rx_next`, have a buffer without a gap -- capped at `RX_RING - 1`.
 *
 * The tail names the **last** descriptor the card may write, not one past it,
 * and everything from the card's head up to it is fair game. So it may only be
 * advanced across a run of descriptors that *all* hold a buffer. The first
 * version did not do that: it remembered the last index it had successfully
 * re-stocked, which under memory pressure can be a *later* index than one
 * whose allocation failed. Slot 1 fails, slot 2 succeeds, the tail goes to 2,
 * and the card is free to write descriptor 1 -- which still holds the physical
 * address of the buffer just handed up to IP. A card DMA-ing into a frame the
 * stack is parsing is not a dropped packet; it is somebody else's data
 * appearing in the middle of one. That is NW-006.
 *
 * The cap is not decoration: a tail equal to the head is how this card is told
 * there is nowhere to write, so the ring can never offer all of itself at once
 * without meaning the opposite of what it says.
 *
 * --- Why this is a function and not four lines inside the loop ---
 *
 * Because NW-006 needs `netbuf_alloc` to fail before it can happen at all, and
 * a self-test cannot make the page allocator run out on purpose without
 * wrecking the machine it is running in. Pulled out here, the rule can be
 * handed a ring with a hole punched in it and asked what it would do.
 *
 * **`e1000_poll` calls this**, which is the whole difference between
 * extracting a function and copying one: what the test exercises is what runs.
 * A test of a lifted copy would leave the original unproven, which is the
 * shape of fault this project keeps finding.
 */
static unsigned rx_available(const struct e1000 *e)
{
	unsigned n;

	for (n = 0; n < RX_RING - 1; n++)
		if (!e->rx_buf[(e->rx_next + n) % RX_RING])
			break;

	return n;
}

/* --- The cable ------------------------------------------------------------- */

static void update_link(struct e1000 *e)
{
	u32 status = rr(e, R_STATUS);
	bool up = (status & STATUS_LU) != 0;

	if (e->link_known && up == e->link_up)
		return;

	if (e->link_known)
		e->link_changes++;

	e->link_known = true;
	e->link_up = up;

	if (e->ndev)
		e->ndev->link = up;

	if (up) {
		unsigned code = (status & STATUS_SPEED) >> STATUS_SPEED_SHIFT;
		const char *speed = code == 0 ? "10" :
				    code == 1 ? "100" : "1000";

		kprintf("e1000: %s link up, %s Mb %s duplex\n",
			e->ndev ? e->ndev->name : "eth?", speed,
			(status & STATUS_FD) ? "full" : "half");
	} else {
		kprintf("e1000: %s link down\n",
			e->ndev ? e->ndev->name : "eth?");
	}
}

/* --- Receive --------------------------------------------------------------- */

static void e1000_poll(struct net_device *dev)
{
	struct e1000 *e = dev->driver;
	u64 flags;

	flags = spin_lock_irq(&e->lock);

	collect_tx(e);
	update_link(e);

	for (;;) {
		unsigned i = e->rx_next % RX_RING;
		struct e1000_rx_desc *d = &e->rx_desc[i];
		struct netbuf *b;
		u8 status, errors;
		u32 len;

		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		status = d->status;

		if (!(status & RX_STATUS_DD))
			break;		/* the card has not filled it */

		errors = d->errors;
		len = d->length;

		b = e->rx_buf[i];
		e->rx_buf[i] = NULL;
		e->rx_next++;

		if (!b)
			continue;

		/* `RCTL_SECRC` is set, so `length` is the frame **without** its
		 * four-byte check sequence and nothing is subtracted here.
		 *
		 * `r8169.c` subtracts four at the same point, because that card
		 * cannot be asked to strip it. The two are opposite and both
		 * are right, which is exactly why neither line may be copied to
		 * the other file: the difference is in the silicon and there is
		 * nothing in the frame to say which one you are holding.
		 *
		 * Copying the Realtek's subtraction to here was tried, on
		 * purpose, and the boot lost its DHCP lease -- so that
		 * direction is caught. The reverse, four bytes too long, was
		 * also tried and produced a lease and a ping reply that no
		 * instrument in this kernel can tell from a correct one. See
		 * NW-007: the direction a driver errs in is the silent one. */
		if (!(status & RX_STATUS_EOP) || errors || !len ||
		    len > ETH_FRAME_MAX) {
			e->rx_errors++;
			dev->rx_errors++;
			e->rx_buf[i] = b;
			give_rx_to_card(e, i);
			continue;
		}

		b->len = len;
		e->rx_frames++;

		spin_unlock_irq(&e->lock, flags);
		netdev_receive(dev, b);
		flags = spin_lock_irq(&e->lock);

		if (!e->rx_buf[i]) {
			e->rx_buf[i] = netbuf_alloc();

			if (e->rx_buf[i])
				give_rx_to_card(e, i);
			else
				e->rx_no_buffer++;
		}
	}

	stock_rx(e);

	{
		unsigned avail = rx_available(e);

		if (avail)
			wr(e, R_RDT, (e->rx_next + avail - 1) % RX_RING);
	}

	spin_unlock_irq(&e->lock, flags);
}

/* --- Transmit -------------------------------------------------------------- */

static bool e1000_transmit(struct net_device *dev, struct netbuf *b)
{
	struct e1000 *e = dev->driver;
	unsigned i;
	u64 flags;

	flags = spin_lock_irq(&e->lock);

	collect_tx(e);

	if (e->tx_head - e->tx_tail >= TX_RING) {
		e->tx_ring_full++;
		spin_unlock_irq(&e->lock, flags);
		netbuf_free(b);
		return false;
	}

	i = e->tx_head % TX_RING;

	e->tx_buf[i] = b;
	e->tx_desc[i].addr = (u64)(b->page + (paddr_t)(b->data - b->head));
	e->tx_desc[i].length = (u16)b->len;
	e->tx_desc[i].cso = 0;
	e->tx_desc[i].css = 0;
	e->tx_desc[i].special = 0;
	e->tx_desc[i].status = 0;

	/* One descriptor is the whole frame, the card appends the check
	 * sequence, and it reports when it is done -- without `TX_CMD_RS` the
	 * status byte is never written and `collect_tx` above would wait for a
	 * bit that cannot arrive, leaking every buffer it ever sent. */
	e->tx_desc[i].cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;

	e->tx_head++;
	e->tx_frames++;

	__atomic_thread_fence(__ATOMIC_RELEASE);

	/* The tail is one *past* the last descriptor to send -- the opposite
	 * of the receive tail three functions up, on the same card, in the
	 * same register block. That asymmetry is Intel's and not a mistake
	 * here; it is written down because a reader who has just read the
	 * receive path will assume otherwise. */
	wr(e, R_TDT, e->tx_head % TX_RING);

	spin_unlock_irq(&e->lock, flags);
	return true;
}

/* --- The interrupt --------------------------------------------------------- */

static void e1000_interrupt(void *arg)
{
	struct e1000 *e = arg;
	u32 cause = rr(e, R_ICR);	/* reading it is the acknowledgement */

	if (!cause)
		return;

	e->interrupts++;

	netdev_wake();
}

/* The device layer asking whether this card can interrupt, and arming it if
 * so. Called by `netdev_register`; see the same function in `r8169.c` and
 * NW-008 for why the vector is claimed before the card is registered
 * rather than after. */
static bool e1000_enable_interrupts(struct net_device *dev)
{
	struct e1000 *e = dev->driver;

	if (!e->interrupting)
		return false;

	(void)rr(e, R_ICR);
	wr(e, R_IMS, INT_WANTED);
	return true;
}

static const struct net_device_ops e1000_ops = {
	.transmit = e1000_transmit,
	.enable_interrupts = e1000_enable_interrupts,
	.poll = e1000_poll,
};

/* --- Setting it up --------------------------------------------------------- */

static bool reset_card(struct e1000 *e)
{
	u64 deadline;

	/* Every interrupt off before the reset and again after it. A card
	 * reset with its mask open can raise one on the way through, and there
	 * is no handler yet. */
	wr(e, R_IMC, 0xFFFFFFFFu);
	(void)rr(e, R_ICR);

	wr(e, R_CTRL, rr(e, R_CTRL) | CTRL_RST);

	deadline = time_monotonic_ns() + 10000000ull;	/* 10 ms */

	while (time_monotonic_ns() < deadline) {
		if (!(rr(e, R_CTRL) & CTRL_RST)) {
			wr(e, R_IMC, 0xFFFFFFFFu);
			(void)rr(e, R_ICR);
			return true;
		}
	}

	return false;
}

static void start_card(struct e1000 *e)
{
	unsigned i;

	/* The multicast filter, cleared. Whatever the card powered up with is
	 * not something this kernel chose, and a stale filter is a card that
	 * either drops groups it should take or takes traffic nobody asked
	 * for. */
	for (i = 0; i < 128; i++)
		wr(e, R_MTA + i * 4, 0);

	wr(e, R_RDBAL, (u32)((u64)(e->ring_page + RX_RING_OFFSET) & 0xFFFFFFFFu));
	wr(e, R_RDBAH, (u32)(((u64)(e->ring_page + RX_RING_OFFSET) >> 32)));
	wr(e, R_RDLEN, RX_RING * 16);
	wr(e, R_RDH, 0);

	/* The tail at the last stocked descriptor, so every one of them is
	 * available. Set before the receiver is enabled, because a receiver
	 * enabled with head equal to tail believes it has nowhere to write and
	 * drops the first frames that arrive. */
	wr(e, R_RDT, RX_RING - 1);

	wr(e, R_TDBAL, (u32)((u64)(e->ring_page + TX_RING_OFFSET) & 0xFFFFFFFFu));
	wr(e, R_TDBAH, (u32)(((u64)(e->ring_page + TX_RING_OFFSET) >> 32)));
	wr(e, R_TDLEN, TX_RING * 16);
	wr(e, R_TDH, 0);
	wr(e, R_TDT, 0);

	/* Ours and broadcast, the card strips the check sequence, and 2048
	 * bytes a buffer. **Not** promiscuous and not long-frame: the same
	 * refusal `r8169.c` makes, and for the same reason -- a stack that
	 * appears to work because it is seeing somebody else's traffic works
	 * until it meets a switch. */
	wr(e, R_RCTL, RCTL_EN | RCTL_BAM | RCTL_SZ_2048 | RCTL_SECRC);

	wr(e, R_TCTL, TCTL_EN | TCTL_PSP |
		      (0x0Fu << TCTL_CT_SHIFT) | (0x40u << TCTL_COLD_SHIFT));

	/* The inter-packet gap, which has no sensible default on this part:
	 * the reset value is zero and a card transmitting with no gap is a
	 * card the switch at the other end discards. */
	wr(e, R_TIPG, 10u | (8u << 10) | (6u << 20));

	/* And bring the link up and let the card work out speed and duplex for
	 * itself, which is the only thing to do when nobody has said what the
	 * far end is. */
	wr(e, R_CTRL, rr(e, R_CTRL) | CTRL_SLU | CTRL_ASDE);
}

bool e1000_attach(const struct pci_device *d)
{
	struct e1000 *e;
	struct mac_addr mac;
	char name[NET_NAME_MAX];
	unsigned stocked;

	if (!d || d->vendor != INTEL_VENDOR)
		return false;

	switch (d->device) {
	case E1000_82540EM:
	case E1000_82545EM:
	case E1000_82540EP:
	case E1000_82541GI:
	case E1000_82547GI:
	case E1000_82544GC:
		break;
	default:
		return false;
	}

	if (device_count >= E1000_MAX)
		return false;

	e = &devices[device_count];
	kmemset(e, 0, sizeof(*e));
	e->pci = *d;
	e->device_id = d->device;
	spin_init(&e->lock, "e1000");

	/* Base address register 0, and the whole of it -- unlike the Realtek,
	 * whose registers fit in 256 bytes, the ones this driver writes run to
	 * 0x5400 and beyond. */
	e->mmio = pci_map_bar(d, 0, 0, 0x6000);

	if (!e->mmio) {
		kprintf("e1000: the card at %u:%u.%u has no memory window\n",
			d->bus, d->slot, d->func);
		return false;
	}

	if (!reset_card(e)) {
		kprintf("e1000: the card at %u:%u.%u did not finish a reset "
			"in 10 ms; leaving it alone\n",
			d->bus, d->slot, d->func);
		return false;
	}

	if (!read_mac(e, &mac)) {
		kprintf("e1000: the card at %u:%u.%u would not say what its "
			"address is\n", d->bus, d->slot, d->func);
		return false;
	}

	if (mac_equal(&mac, &MAC_BROADCAST) || mac_equal(&mac, &MAC_ZERO)) {
		char text[20];

		kprintf("e1000: the card at %u:%u.%u reports %s as its "
			"address, which is not one; not registering it\n",
			d->bus, d->slot, d->func, mac_format(&mac, text));
		return false;
	}

	e->ring_page = pmm_alloc_page();

	if (!e->ring_page) {
		kputs("e1000: no memory for the descriptor rings\n");
		return false;
	}

	{
		u8 *base = phys_to_virt(e->ring_page);

		kmemset(base, 0, 4096);
		e->rx_desc = (struct e1000_rx_desc *)(base + RX_RING_OFFSET);
		e->tx_desc = (struct e1000_tx_desc *)(base + TX_RING_OFFSET);
	}

	stocked = stock_rx(e);

	if (!stocked) {
		kputs("e1000: no memory for a single receive buffer\n");
		pmm_free_pages(e->ring_page, 1);
		return false;
	}

	start_card(e);

	if (!netdev_name("eth", name, sizeof(name))) {
		kputs("e1000: no free device name\n");
		pmm_free_pages(e->ring_page, 1);
		return false;
	}

	/* Before the registration, for the reason in `r8169.c`. */
	e->interrupting = arch_pci_request_interrupt(&e->pci, 0,
						     e1000_interrupt, e,
						     "e1000");

	e->ndev = netdev_register(name, &e1000_ops, e, &mac);

	if (!e->ndev) {
		pmm_free_pages(e->ring_page, 1);
		return false;
	}

	update_link(e);

	device_count++;

	{
		char text[20];

		kprintf("e1000: %s at %s, 8254x %04X, %u receive buffers, "
			"address from %s, %s\n",
			name, mac_format(&mac, text), e->device_id, stocked,
			e->mac_from_eeprom ? "the eeprom" : "the card",
			e->interrupting ? "by interrupt" : "polled");
	}

	return true;
}

unsigned e1000_count(void)
{
	return device_count;
}

void e1000_print_summary(void)
{
	unsigned i;

	for (i = 0; i < device_count; i++) {
		struct e1000 *e = &devices[i];
		unsigned stocked = 0;
		unsigned j;

		for (j = 0; j < RX_RING; j++)
			if (e->rx_buf[j])
				stocked++;

		kprintf("  %s         : %u in, %u out, %u stocked, "
			"%u interrupt(s)\n",
			e->ndev ? e->ndev->name : "eth?",
			(unsigned)e->rx_frames, (unsigned)e->tx_frames,
			stocked, (unsigned)e->interrupts);

		kprintf("               : link %s, %u change(s), head %u "
			"tail %u of %u\n",
			e->link_up ? "up" : "down",
			(unsigned)e->link_changes,
			(unsigned)(rr(e, R_RDH) % RX_RING),
			(unsigned)(rr(e, R_RDT) % RX_RING), RX_RING);

		if (e->rx_errors || e->rx_no_buffer || e->tx_ring_full)
			kprintf("               : %u bad, %u no buffer, "
				"%u ring full\n",
				(unsigned)e->rx_errors,
				(unsigned)e->rx_no_buffer,
				(unsigned)e->tx_ring_full);

		if (!stocked)
			kprintf("               : the receive ring is empty -- "
				"this card can no longer receive\n");

		if (e->interrupting && !e->interrupts)
			kprintf("               : it has a vector and has "
				"never been interrupted -- everything here "
				"arrived by polling\n");

		/* The other half of that sentence, and the one this pair of
		 * drivers was written to be able to say. A card with no vector
		 * is not broken; it is polled, and NW-005 is why. */
		if (!e->interrupting)
			kprintf("               : no vector was available for "
				"it -- see NW-005; it is polled\n");
	}
}

/* --- The receive path, with the card played by this file ------------------- */

/* The other half of NW-007, and the mirror image of `r8169_self_test`.
 *
 * This driver *can* be booted -- QEMU emulates an 82540EM -- so unlike the
 * Realtek it is not untested silicon. What booting cannot do is catch the one
 * mistake it is most likely to make, and that was measured rather than
 * assumed: breaking the receive length four bytes **too long** produced a DHCP
 * lease and a ping reply indistinguishable from a correct run. Every layer
 * reads its own length field and ignores what trails it.
 *
 * --- The assertion that matters here is the opposite of the Realtek's ---
 *
 * `r8169_self_test` asserts that four bytes come **off**, because that card
 * counts the frame check sequence and cannot be told not to. This asserts that
 * **nothing** comes off, because `RCTL_SECRC` makes this card strip it before
 * the length is written.
 *
 * Both are right, they are opposites, and there is nothing in a frame to say
 * which card you are holding. So the failure worth guarding against is a
 * subtraction copied from one driver to the other by somebody tidying up --
 * which reads as consistency and is a four-byte error. This test is what makes
 * that copy fail loudly instead of silently.
 *
 * --- And one thing the Realtek's test has no equivalent of ---
 *
 * NW-006, the receive tail. That fault cannot exist on the Realtek: there a
 * descriptor is available if and only if its ownership bit is set, and that
 * bit is only ever set after an allocation succeeded, so availability *is*
 * having a buffer. Here they are two separate facts kept in step by hand, and
 * `rx_available` is the hand. It is asked directly, with a hole punched in the
 * ring, because the fault it prevents needs the page allocator to run out --
 * which a self-test must not arrange.
 *
 * --- What this cannot prove ---
 *
 * The same limit the Realtek's test states: that the real chip behaves the way
 * this file pretends. Register offsets, the reset sequence and the meaning of
 * every bit are proved by booting with the card, not here. The two halves are
 * complementary rather than overlapping -- booting proves the card is driven,
 * this proves the arithmetic is right.
 */

static struct e1000 test_card;

/* What the card leaves behind when it has filled a descriptor.
 *
 * `length` is the frame **without** its four-byte check sequence, because
 * `start_card` sets `RCTL_SECRC` and this is what that setting means. Writing
 * the fake this way round is the whole point: a driver that subtracted four
 * here would be measured against a card that had already done it. */
static void card_delivers(struct e1000 *e, unsigned i, u32 length,
			  u8 status, u8 errors)
{
	struct e1000_rx_desc *d = &e->rx_desc[i];
	struct netbuf *b = e->rx_buf[i];

	/* A frame addressed to this device with an ethertype nothing claims,
	 * so Ethernet accepts it as ours and drops it there rather than
	 * parsing it as a malformed IP packet further up. */
	if (b && length >= ETH_HDR_LEN) {
		kmemcpy(b->data, &e->ndev->mac, MAC_LEN);
		kmemset(b->data + MAC_LEN, 0x02, MAC_LEN);
		b->data[12] = 0x88;
		b->data[13] = 0xB5;
	}

	d->length = (u16)length;
	d->errors = errors;

	/* The status byte last and behind a barrier, because it is what says
	 * the rest of the descriptor is worth reading -- the same order the
	 * silicon has to use and the driver relies on. */
	__atomic_thread_fence(__ATOMIC_RELEASE);
	d->status = status;
}

/* One frame through the driver's own loop; out come the bytes and the frames.
 *
 * Both, not just the bytes, and the reason was learned the hard way on the
 * Realtek: a refused frame and a frame accepted with a length of zero both add
 * nothing to `rx_bytes`, so bytes alone cannot tell "correctly refused" from
 * "wrongly accepted, and empty". */
static void deliver_one(struct e1000 *e, struct net_device *dev, u32 length,
			u8 status, u8 errors, u64 *bytes, u64 *frames)
{
	u64 b0 = dev->rx_bytes;
	u64 f0 = dev->rx_packets;

	card_delivers(e, e->rx_next % RX_RING, length, status, errors);
	e1000_poll(dev);

	*bytes = dev->rx_bytes - b0;
	*frames = dev->rx_packets - f0;
}

/* --- NW-006: the tail, asked directly -------------------------------------- */

/* Punches a hole `at` descriptors ahead of where the driver is reading and
 * asks `rx_available` what it would offer the card.
 *
 * The buffer is taken out and put back rather than freed, so the ring is
 * exactly as it was afterwards and nothing leaks. */
static bool tail_stops_at_a_hole(struct e1000 *e, unsigned at, unsigned expect)
{
	unsigned slot = (e->rx_next + at) % RX_RING;
	struct netbuf *held = e->rx_buf[slot];
	unsigned got;

	e->rx_buf[slot] = NULL;
	got = rx_available(e);
	e->rx_buf[slot] = held;

	if (got == expect)
		return true;

	kprintf("  e1000: with no buffer %u descriptor(s) ahead, the tail "
		"would offer %u rather than %u -- the card would be given a "
		"descriptor holding a frame the stack is reading (NW-006)\n",
		at, got, expect);

	return false;
}

/* --- and the way out ------------------------------------------------------
 *
 * The same argument as the Realtek's transmit test: a far end that replies can
 * only report that *something* got through, and the things a driver gets wrong
 * on the way out are not the things a reply validates.
 *
 * One assertion here has no counterpart on the Realtek and is the reason this
 * function is worth its length. **`TX_CMD_RS`** is what asks the card to write
 * a status byte back when it is done. Without it the status byte is never
 * written, `collect_tx` waits for a bit that cannot arrive, and every buffer
 * ever transmitted is held for ever -- a page leaked per frame, on a path that
 * otherwise works perfectly. The far end sees every frame. The machine runs
 * out of memory hours later with nothing to point at.
 */
static bool transmit_builds_a_descriptor(struct e1000 *e, struct net_device *dev)
{
	struct netbuf *b = netbuf_alloc();
	unsigned i = e->tx_head % TX_RING;
	paddr_t expect;
	bool ok = true;

	if (!b) {
		kputs("  e1000: no buffer to transmit in the test\n");
		return false;
	}

	if (!netbuf_put(b, 100)) {
		kputs("  e1000: no room for a hundred bytes of payload\n");
		netbuf_free(b);
		return false;
	}

	expect = b->page + (paddr_t)(b->data - b->head);

	if (!netdev_transmit(dev, b)) {
		kputs("  e1000: the device refused a hundred-byte frame\n");
		return false;
	}

	/* `b` is the driver's from here. */

	if (e->tx_desc[i].length != 100) {
		kprintf("  e1000: a 100-byte frame was put on the wire as %u "
			"bytes\n", (unsigned)e->tx_desc[i].length);
		ok = false;
	}

	if (e->tx_desc[i].addr != (u64)expect) {
		kputs("  e1000: the descriptor points somewhere other than "
		      "the frame\n");
		ok = false;
	}

	if (!(e->tx_desc[i].cmd & TX_CMD_EOP)) {
		kputs("  e1000: the frame was not marked end-of-packet, so "
		      "the card waits for a continuation that never comes\n");
		ok = false;
	}

	if (!(e->tx_desc[i].cmd & TX_CMD_IFCS)) {
		kputs("  e1000: the card was not asked to append a check "
		      "sequence, so every frame goes out with a missing CRC "
		      "and the switch discards it\n");
		ok = false;
	}

	if (!(e->tx_desc[i].cmd & TX_CMD_RS)) {
		kputs("  e1000: the card was not asked to report completion -- "
		      "the status byte is never written, nothing is ever "
		      "reclaimed, and every frame sent leaks a page\n");
		ok = false;
	}

	if (e->tx_desc[i].status & TX_STATUS_DD) {
		kputs("  e1000: the descriptor was handed over already marked "
		      "done, so it would be reclaimed before it was sent\n");
		ok = false;
	}

	/* The transmit tail is **one past** the last descriptor to send --
	 * the opposite of the receive tail on the same card in the same
	 * register block. Asserted because a reader who has just read the
	 * receive path will assume otherwise, and because getting it wrong by
	 * one means either a frame that never goes or a descriptor sent
	 * twice. */
	if (rr(e, R_TDT) != e->tx_head % TX_RING) {
		kprintf("  e1000: the transmit tail reads %u and the ring is "
			"filled to %u -- the card was told the wrong amount "
			"to send\n",
			(unsigned)rr(e, R_TDT),
			(unsigned)(e->tx_head % TX_RING));
		ok = false;
	}

	if (!e->tx_buf[i]) {
		kputs("  e1000: the frame was released before the card said "
		      "it was done with it\n");
		ok = false;
	}

	/* The card finishes. */
	e->tx_desc[i].status = TX_STATUS_DD;
	__atomic_thread_fence(__ATOMIC_RELEASE);

	e1000_poll(dev);

	if (e->tx_buf[i]) {
		kputs("  e1000: the card finished with a frame and the buffer "
		      "was not given back -- every frame sent would leak a "
		      "page\n");
		ok = false;
	}

	if (e->tx_head != e->tx_tail) {
		kprintf("  e1000: the card finished and %u frame(s) are still "
			"counted as in flight\n", e->tx_head - e->tx_tail);
		ok = false;
	}

	return ok;
}

/* A full ring refuses, counts it, and frees what it refused. */
static bool a_full_ring_refuses(struct e1000 *e, struct net_device *dev)
{
	unsigned sent = 0;
	unsigned i;
	u64 refused_before = e->tx_ring_full;
	bool ok = true;

	for (i = 0; i < TX_RING; i++) {
		struct netbuf *b = netbuf_alloc();

		if (!b || !netbuf_put(b, 64)) {
			netbuf_free(b);
			break;
		}

		if (!e1000_transmit(dev, b))
			break;

		sent++;
	}

	if (sent != TX_RING) {
		kprintf("  e1000: the ring took %u frames before refusing, "
			"and it holds %u\n", sent, TX_RING);
		ok = false;
	}

	{
		struct netbuf *b = netbuf_alloc();

		if (b && netbuf_put(b, 64)) {
			if (e1000_transmit(dev, b)) {
				kputs("  e1000: a full ring accepted another "
				      "frame, overwriting one the card is "
				      "still reading\n");
				ok = false;
			}
		} else {
			netbuf_free(b);
		}
	}

	if (e->tx_ring_full == refused_before) {
		kputs("  e1000: a frame was dropped for a full ring and "
		      "nothing counted it\n");
		ok = false;
	}

	for (i = 0; i < TX_RING; i++)
		e->tx_desc[i].status = TX_STATUS_DD;

	__atomic_thread_fence(__ATOMIC_RELEASE);
	e1000_poll(dev);

	for (i = 0; i < TX_RING; i++) {
		if (e->tx_buf[i]) {
			kprintf("  e1000: descriptor %u was finished with and "
				"its buffer was not given back\n", i);
			ok = false;
			break;
		}
	}

	return ok;
}

bool e1000_self_test(void)
{
	struct e1000 *e = &test_card;
	struct net_device *dev;
	char name[NET_NAME_MAX];
	static const struct mac_addr mac = { { 0x02, 0x4E, 0x57, 0, 0, 0x20 } };
	const u8 done = RX_STATUS_DD | RX_STATUS_EOP;
	paddr_t regs;
	unsigned i;
	u64 got, frames;
	bool ok = true;

	kmemset(e, 0, sizeof(*e));
	spin_init(&e->lock, "e1000-test");

	/* Declared already known and down, so `update_link` finds nothing
	 * changed and stays quiet. There is no cable and no card. */
	e->link_known = true;
	e->link_up = false;

	regs = pmm_alloc_page();
	e->ring_page = pmm_alloc_page();

	if (!regs || !e->ring_page) {
		kputs("  e1000: no memory for the test's rings\n");

		if (regs)
			pmm_free_pages(regs, 1);
		if (e->ring_page)
			pmm_free_pages(e->ring_page, 1);

		return false;
	}

	e->mmio = phys_to_virt(regs);
	kmemset((void *)e->mmio, 0, 4096);

	{
		u8 *base = phys_to_virt(e->ring_page);

		kmemset(base, 0, 4096);
		e->rx_desc = (struct e1000_rx_desc *)(base + RX_RING_OFFSET);
		e->tx_desc = (struct e1000_tx_desc *)(base + TX_RING_OFFSET);
	}

	if (!stock_rx(e)) {
		kputs("  e1000: no memory for the test's buffers\n");
		pmm_free_pages(regs, 1);
		pmm_free_pages(e->ring_page, 1);
		return false;
	}

	if (!netdev_name("eth", name, sizeof(name))) {
		kputs("  e1000: no name for the test device\n");
		ok = false;
		goto out;
	}

	dev = netdev_register(name, &e1000_ops, e, &mac);

	if (!dev) {
		kputs("  e1000: the test device was refused\n");
		ok = false;
		goto out;
	}

	e->ndev = dev;

	/* Drained before anything is measured: the bounded-queue test
	 * deliberately overflows the receive queue, and a full queue makes
	 * `netdev_receive` drop without counting -- so every assertion below
	 * would read zero and blame this driver for the previous test's
	 * leftovers. */
	netdev_service();

	/* --- the four bytes that must NOT come off ------------------------- */

	deliver_one(e, dev, 64, done, 0, &got, &frames);

	if (got != 64 || frames != 1) {
		kprintf("  e1000: a 64-byte frame came up as %u byte(s) in %u "
			"frame(s) -- this card strips the check sequence "
			"itself and nothing here may subtract it again\n",
			(unsigned)got, (unsigned)frames);
		ok = false;
	}

	/* The largest frame the driver accepts, which on this card is the
	 * whole of `ETH_FRAME_MAX` rather than four less -- the boundary a
	 * copied subtraction would move. */
	deliver_one(e, dev, ETH_FRAME_MAX, done, 0, &got, &frames);

	if (got != ETH_FRAME_MAX || frames != 1) {
		kprintf("  e1000: the largest acceptable frame came up as %u "
			"rather than %u\n",
			(unsigned)got, (unsigned)ETH_FRAME_MAX);
		ok = false;
	}

	/* --- and the four kinds of frame that must be refused --------------- */

	deliver_one(e, dev, 64, RX_STATUS_DD, 0, &got, &frames);

	if (got || frames) {
		kputs("  e1000: a frame not marked end-of-packet was passed "
		      "up as though it were whole\n");
		ok = false;
	}

	deliver_one(e, dev, 64, done, 0x02, &got, &frames);

	if (got || frames) {
		kputs("  e1000: a frame the card marked bad was passed up\n");
		ok = false;
	}

	deliver_one(e, dev, 0, done, 0, &got, &frames);

	if (got || frames) {
		kprintf("  e1000: an empty frame was passed up -- %u frame(s), "
			"%u byte(s)\n", (unsigned)frames, (unsigned)got);
		ok = false;
	}

	deliver_one(e, dev, ETH_FRAME_MAX + 1, done, 0, &got, &frames);

	if (got || frames) {
		kprintf("  e1000: a frame of %u bytes was passed up, and this "
			"kernel does not accept jumbo frames\n",
			(unsigned)(ETH_FRAME_MAX + 1));
		ok = false;
	}

	/* --- NW-006, the tail ----------------------------------------------- */

	/* A full ring offers all but one descriptor. Not all of them: a tail
	 * equal to the head is how this card is told there is nowhere to
	 * write, so offering the whole ring says the opposite of what it
	 * means. */
	if (rx_available(e) != RX_RING - 1) {
		kprintf("  e1000: a fully stocked ring offers %u of %u "
			"descriptors; it must offer %u, because a tail equal "
			"to the head means empty\n",
			rx_available(e), RX_RING, RX_RING - 1);
		ok = false;
	}

	/* And it stops at the first descriptor with no buffer, wherever that
	 * is -- including immediately, which means offering nothing at all
	 * rather than offering the card a descriptor it must not have. */
	if (!tail_stops_at_a_hole(e, 3, 3))
		ok = false;

	if (!tail_stops_at_a_hole(e, 0, 0))
		ok = false;

	if (!tail_stops_at_a_hole(e, 1, 1))
		ok = false;

	/* --- and once round, so the wrap in the index arithmetic runs ------- */

	for (i = 0; i < RX_RING + 2; i++) {
		deliver_one(e, dev, 64, done, 0, &got, &frames);

		if (got != 64 || frames != 1) {
			kprintf("  e1000: frame %u of a lap came up as %u "
				"byte(s) -- the ring does not survive a "
				"wrap\n", i, (unsigned)got);
			ok = false;
			break;
		}

		/* Walked up as they go. The receive queue holds 64 and this
		 * lap alone makes 34, and a frame dropped for a full queue is
		 * counted in the same `overflowed` number the bounded-queue
		 * test uses to prove its bound -- which must not be borrowed
		 * for ordinary test traffic. */
		if ((i % 8) == 7)
			netdev_service();
	}

	/* --- and the way out ------------------------------------------------ */

	if (!transmit_builds_a_descriptor(e, dev))
		ok = false;

	if (!a_full_ring_refuses(e, dev))
		ok = false;

	netdev_service();
	netdev_forget_last();

out:
	for (i = 0; i < RX_RING; i++) {
		netbuf_free(e->rx_buf[i]);
		e->rx_buf[i] = NULL;
	}

	pmm_free_pages(regs, 1);
	pmm_free_pages(e->ring_page, 1);
	kmemset(e, 0, sizeof(*e));

	return ok;
}
