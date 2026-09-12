/* IPv4: twenty bytes that mean a packet can leave the wire it started on.
 *
 * Ethernet reaches one card on one cable. IP is the layer that says a packet
 * has a destination somewhere else entirely, and that some machine in between
 * will forward it. Everything in the header is in service of that: an address
 * that is not a hardware address, a hop count so a routing loop ends, and a
 * checksum recomputed at every hop because the hop count changes.
 *
 * --- What is deliberately not built ---
 *
 * **Fragmentation and reassembly.** A packet larger than the link can carry is
 * refused here rather than split. Reassembly is where a decade of security
 * holes lived -- overlapping fragments, the last fragment never arriving,
 * a table of half-packets that anybody on the wire can fill -- and nothing in
 * this kernel needs to send more than an MTU yet. A refusal is a bug report;
 * a broken reassembler is a vulnerability. Incoming fragments are dropped and
 * counted, so a machine that is being sent them says so.
 *
 * **Options.** A header longer than twenty bytes is read for its length, so
 * the payload is found correctly, and the options themselves are skipped.
 * Nothing is generated with options.
 */
#include <recon/kernel/net.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/console.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/time.h>

#define IP_VERSION_4 4
#define IP_DEFAULT_TTL 64

#define IP_FLAG_MORE_FRAGMENTS 0x2000
#define IP_FRAGMENT_OFFSET_MASK 0x1FFF

static u64 tx_packets, tx_no_route, tx_pending, tx_pending_dropped;
static u64 rx_packets, rx_bad_checksum, rx_bad_version, rx_short;
static u64 rx_fragments, rx_not_for_us, rx_unknown_proto;

/* Packets waiting on an ARP answer.
 *
 * Without this, the first packet to any address is always lost: the lookup
 * misses, a request goes out, and the packet that triggered it has nowhere to
 * go. Every conversation would begin with a dropped packet and a retransmit
 * timer, which works and is a second of latency nobody can explain.
 *
 * The queue is small and entries expire. A packet held for an address that
 * never answers is a packet that will never be delivered, and holding it for
 * longer only means holding more of them. */
#define PENDING_MAX 8
#define PENDING_TTL_NS (3ull * 1000000000ull)

struct pending {
	struct netbuf *buf;
	struct net_device *dev;
	ipv4_addr next_hop;
	u64 queued_ns;
	bool used;
};

static struct pending pending[PENDING_MAX];
static struct spinlock pending_lock;
static bool lock_ready;

static void ensure_lock(void)
{
	if (!lock_ready) {
		spin_init(&pending_lock, "ip-pending");
		lock_ready = true;
	}
}

static u16 next_id(void)
{
	static u16 id;

	/* Identifies the fragments of one packet. Nothing here fragments, so
	 * it only has to differ between packets in flight; a counter is enough
	 * and is what every stack uses. */
	return ++id;
}

static bool finish_send(struct net_device *dev, const struct mac_addr *dst,
			struct netbuf *b)
{
	return eth_transmit(dev, dst, ETH_TYPE_IPV4, b);
}

/* Builds the header onto a buffer whose `data` is the transport payload. */
static u8 *build_header(struct netbuf *b, struct net_device *dev,
			ipv4_addr dst, u8 protocol)
{
	u32 total = b->len + IP_HDR_LEN;
	u8 *h;

	if (total > NET_MTU)
		return NULL;

	h = netbuf_push(b, IP_HDR_LEN);

	if (!h)
		return NULL;

	h[0] = (IP_VERSION_4 << 4) | (IP_HDR_LEN / 4);
	h[1] = 0;				/* no differentiated services */
	net_put16(h + 2, (u16)total);
	net_put16(h + 4, next_id());
	net_put16(h + 6, 0);			/* not a fragment */
	h[8] = IP_DEFAULT_TTL;
	h[9] = protocol;
	net_put16(h + 10, 0);			/* checksum, computed below */
	net_put32(h + 12, dev->ip);
	net_put32(h + 16, dst);

	/* Over the header only. IPv4's checksum does not cover the payload --
	 * that is the transport's job, and it is why a router can decrement the
	 * hop count and fix up twenty bytes rather than re-summing the packet. */
	net_put16(h + 10, net_checksum(h, IP_HDR_LEN));

	return h;
}

bool ip_transmit(ipv4_addr dst, u8 protocol, struct netbuf *b)
{
	struct net_device *dev;
	ipv4_addr hop = 0;
	struct mac_addr mac;
	unsigned i;
	u64 flags;

	dev = netdev_route(dst, &hop);

	if (!dev) {
		tx_no_route++;
		netbuf_free(b);
		return false;
	}

	if (!build_header(b, dev, dst, protocol)) {
		/* Too big for one frame, and this kernel does not fragment. */
		netbuf_free(b);
		return false;
	}

	if (arp_lookup(dev, hop, &mac)) {
		tx_packets++;
		return finish_send(dev, &mac, b);
	}

	/* The address is not known yet and a request has just gone out. Hold
	 * the packet rather than dropping it. */
	ensure_lock();
	flags = spin_lock_irq(&pending_lock);

	for (i = 0; i < PENDING_MAX; i++) {
		if (!pending[i].used) {
			pending[i].used = true;
			pending[i].buf = b;
			pending[i].dev = dev;
			pending[i].next_hop = hop;
			pending[i].queued_ns = time_monotonic_ns();
			tx_pending++;
			spin_unlock_irq(&pending_lock, flags);
			return true;
		}
	}

	spin_unlock_irq(&pending_lock, flags);

	tx_pending_dropped++;
	netbuf_free(b);
	return false;
}

/* Called after anything that may have taught the ARP cache something. */
void ip_flush_pending(void)
{
	unsigned i;
	u64 now;

	ensure_lock();
	now = time_monotonic_ns();

	for (i = 0; i < PENDING_MAX; i++) {
		struct netbuf *b = NULL;
		struct net_device *dev = NULL;
		ipv4_addr hop = 0;
		struct mac_addr mac;
		u64 flags = spin_lock_irq(&pending_lock);

		if (pending[i].used) {
			if (now - pending[i].queued_ns > PENDING_TTL_NS) {
				b = pending[i].buf;
				pending[i].used = false;
				spin_unlock_irq(&pending_lock, flags);
				tx_pending_dropped++;
				netbuf_free(b);
				continue;
			}

			dev = pending[i].dev;
			hop = pending[i].next_hop;
		}

		spin_unlock_irq(&pending_lock, flags);

		if (!dev)
			continue;

		if (!arp_lookup(dev, hop, &mac))
			continue;

		/* Take it out under the lock before sending, so that two
		 * processors flushing at once cannot both send it. */
		flags = spin_lock_irq(&pending_lock);

		if (pending[i].used) {
			b = pending[i].buf;
			pending[i].used = false;
		} else {
			b = NULL;
		}

		spin_unlock_irq(&pending_lock, flags);

		if (b) {
			tx_packets++;
			finish_send(dev, &mac, b);
		}
	}
}

void ip_receive(struct net_device *dev, struct netbuf *b)
{
	const u8 *h = b->data;
	u32 hdr_len, total;
	u16 frag;
	u8 protocol;
	ipv4_addr src, dst;

	if (b->len < IP_HDR_LEN) {
		rx_short++;
		netbuf_free(b);
		return;
	}

	if ((h[0] >> 4) != IP_VERSION_4) {
		rx_bad_version++;
		netbuf_free(b);
		return;
	}

	hdr_len = (u32)(h[0] & 0x0F) * 4;

	if (hdr_len < IP_HDR_LEN || hdr_len > b->len) {
		rx_short++;
		netbuf_free(b);
		return;
	}

	/* The checksum is checked before anything in the header is believed.
	 * A correct header sums to zero including its own checksum field,
	 * which is why there is no need to zero it first. */
	if (net_checksum(h, hdr_len) != 0) {
		rx_bad_checksum++;
		netbuf_free(b);
		return;
	}

	total = net_get16(h + 2);

	if (total < hdr_len) {
		rx_short++;
		netbuf_free(b);
		return;
	}

	/* A frame can be padded to the minimum Ethernet length, so the buffer
	 * may be longer than the packet. Trusting the frame length instead of
	 * the header would hand the transport layer padding as payload. */
	if (total < b->len)
		b->len = total;

	frag = net_get16(h + 6);

	if ((frag & IP_FLAG_MORE_FRAGMENTS) ||
	    (frag & IP_FRAGMENT_OFFSET_MASK)) {
		rx_fragments++;
		netbuf_free(b);
		return;
	}

	protocol = h[9];
	src = net_get32(h + 12);
	dst = net_get32(h + 16);

	/* Addressed to this machine, to the broadcast address, or to this
	 * wire's broadcast. Anything else arrived because the card is in
	 * promiscuous mode or because somebody is routing badly; this kernel
	 * does not forward, so it is dropped. */
	if (dev->ip && dst != dev->ip && dst != IPV4_BROADCAST &&
	    !(dev->netmask && dst == (dev->ip | ~dev->netmask))) {
		rx_not_for_us++;
		netbuf_free(b);
		return;
	}

	netbuf_pull(b, hdr_len);

	b->src_ip = src;
	b->dst_ip = dst;
	b->protocol = protocol;

	rx_packets++;

	switch (protocol) {
	case IP_PROTO_ICMP:
		icmp_receive(dev, b);
		return;

	case IP_PROTO_UDP:
		udp_receive(dev, b);
		return;

	case IP_PROTO_TCP:
		tcp_receive(dev, b);
		return;

	default:
		rx_unknown_proto++;
		netbuf_free(b);
		return;
	}
}

void ip_print_summary(void)
{
	kprintf("  ipv4 tx      : %u sent, %u no route, %u held for arp, "
		"%u given up\n",
		(unsigned)tx_packets, (unsigned)tx_no_route,
		(unsigned)tx_pending, (unsigned)tx_pending_dropped);
	kprintf("  ipv4 rx      : %u taken, %u bad checksum, %u not v4, "
		"%u short\n",
		(unsigned)rx_packets, (unsigned)rx_bad_checksum,
		(unsigned)rx_bad_version, (unsigned)rx_short);
	kprintf("               : %u fragments refused, %u not for us, "
		"%u unknown protocol\n",
		(unsigned)rx_fragments, (unsigned)rx_not_for_us,
		(unsigned)rx_unknown_proto);
}

bool ip_self_test(void)
{
	struct netbuf *b;
	struct net_device dev;
	bool ok = true;
	u8 *h;

	kmemset(&dev, 0, sizeof(dev));
	dev.ip = IPV4(192, 168, 0, 2);
	dev.netmask = IPV4(255, 255, 255, 0);
	dev.mtu = NET_MTU;

	/* A header this kernel builds is one it accepts. That is a weaker
	 * claim than it looks -- a stack with the byte order wrong in both
	 * directions passes it -- so the checksum is also checked against the
	 * value an independent implementation produces, in netbuf's test. */
	b = netbuf_alloc();

	if (!b) {
		kprintf("ip: no memory for the test\n");
		return false;
	}

	netbuf_put(b, 40);
	h = build_header(b, &dev, IPV4(192, 168, 0, 9), IP_PROTO_UDP);

	if (!h) {
		kprintf("ip: could not build a header\n");
		netbuf_free(b);
		return false;
	}

	if (net_checksum(h, IP_HDR_LEN) != 0) {
		kprintf("ip: a header we built does not check out\n");
		ok = false;
	}

	if (net_get16(h + 2) != 60) {
		kprintf("ip: total length %u, expected 60\n",
			(unsigned)net_get16(h + 2));
		ok = false;
	}

	if (h[8] != IP_DEFAULT_TTL) {
		kprintf("ip: hop count is %u\n", (unsigned)h[8]);
		ok = false;
	}

	/* One bit flipped anywhere in the header must fail the checksum. This
	 * is the assertion that proves the checksum is being *checked* rather
	 * than computed and ignored. */
	{
		u8 copy[IP_HDR_LEN];
		unsigned bad = 0, i;

		for (i = 0; i < IP_HDR_LEN; i++) {
			kmemcpy(copy, h, IP_HDR_LEN);
			copy[i] ^= 0x01;

			if (net_checksum(copy, IP_HDR_LEN) != 0)
				bad++;
		}

		if (bad != IP_HDR_LEN) {
			kprintf("ip: %u of %u single-bit changes were caught\n",
				bad, IP_HDR_LEN);
			ok = false;
		}
	}

	netbuf_free(b);

	/* A packet larger than the link can carry is refused rather than
	 * fragmented. Silently sending a truncated one is the failure this
	 * guards, and it would look like packet loss at the far end. */
	b = netbuf_alloc();

	if (b) {
		netbuf_put(b, NET_MTU);	/* payload alone is already the MTU */

		if (build_header(b, &dev, IPV4(192, 168, 0, 9), IP_PROTO_UDP)) {
			kprintf("ip: an over-MTU packet was accepted\n");
			ok = false;
		}

		netbuf_free(b);
	}

	return ok;
}
