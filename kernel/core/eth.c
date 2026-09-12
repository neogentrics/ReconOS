/* Ethernet: fourteen bytes, and the decision of who gets the rest.
 *
 * The header is destination, source, type. That is all of it -- there is no
 * length, no checksum the software sees, and no sequencing. Ethernet's whole
 * job is to get a frame across one wire to one card, and everything about
 * reliability, ordering and reaching a machine that is not on this wire
 * belongs to the layers above.
 *
 * The trailing four-byte CRC is real but never visible here: the card checks
 * it and drops the frame if it is wrong, so software never sees a frame whose
 * CRC failed and never has to compute one. A stack that tries is computing a
 * checksum the hardware already stripped.
 */
#include <recon/kernel/net.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/console.h>

static u64 rx_unknown_type;
static u64 rx_not_for_us;
static u64 rx_runt;

bool eth_transmit(struct net_device *dev, const struct mac_addr *dst,
		  u16 ethertype, struct netbuf *b)
{
	u8 *h = netbuf_push(b, ETH_HDR_LEN);

	if (!h) {
		netbuf_free(b);
		return false;
	}

	kmemcpy(h, dst->b, MAC_LEN);
	kmemcpy(h + MAC_LEN, dev->mac.b, MAC_LEN);
	net_put16(h + 12, ethertype);

	return netdev_transmit(dev, b);
}

void eth_receive(struct net_device *dev, struct netbuf *b)
{
	struct mac_addr dst;
	u16 type;
	const u8 *h;

	if (b->len < ETH_HDR_LEN) {
		/* Shorter than a header. Counted rather than ignored: a card
		 * producing these says something is wrong with the card or the
		 * wire, and a silent drop makes that invisible. */
		rx_runt++;
		dev->rx_errors++;
		netbuf_free(b);
		return;
	}

	h = b->data;
	kmemcpy(dst.b, h, MAC_LEN);
	type = net_get16(h + 12);

	/* A card in normal operation filters this in hardware, and it is
	 * checked anyway -- because a card in promiscuous mode does not, and
	 * because the check costs six bytes of comparison against a frame we
	 * have already paid to receive. Broadcast is for us; so is multicast,
	 * which is any address with the low bit of the first byte set, and
	 * which ARP does not use but IPv6 neighbour discovery does. */
	if (!mac_equal(&dst, &dev->mac) &&
	    !mac_equal(&dst, &MAC_BROADCAST) &&
	    !(dst.b[0] & 1)) {
		rx_not_for_us++;
		netbuf_free(b);
		return;
	}

	netbuf_pull(b, ETH_HDR_LEN);

	switch (type) {
	case ETH_TYPE_ARP:
		arp_receive(dev, b);
		return;

	case ETH_TYPE_IPV4:
		ip_receive(dev, b);
		return;

	default:
		/* IPv6 lands here, and so does anything else on the wire. The
		 * blueprint lists IPv6 and it is not built; an unknown type is
		 * dropped and counted, which is what an honest "not yet" looks
		 * like from the inside. */
		rx_unknown_type++;
		netbuf_free(b);
		return;
	}
}

void eth_print_summary(void)
{
	if (rx_unknown_type || rx_not_for_us || rx_runt)
		kprintf("  ethernet     : %u unknown type, %u not addressed "
			"to us, %u too short\n",
			(unsigned)rx_unknown_type, (unsigned)rx_not_for_us,
			(unsigned)rx_runt);
	else
		kprintf("  ethernet     : nothing refused\n");
}
