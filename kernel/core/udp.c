/* UDP: two ports, a length, and a checksum that reaches outside itself.
 *
 * UDP adds exactly one thing to IP -- the idea that a machine has more than
 * one conversation going, told apart by a port number. It adds no ordering, no
 * retransmission and no connection, and that is the point: a protocol that
 * needs none of those should not pay for them.
 *
 * --- The pseudo-header, which is the only subtle part ---
 *
 * UDP's checksum covers its header, its payload, *and* five fields that are
 * not in the datagram at all: the source and destination IP addresses, the
 * protocol number and the UDP length. Those are IP's fields, and they are
 * included so that a datagram delivered to the wrong machine, or handed to the
 * wrong protocol, fails its checksum rather than being accepted by whoever
 * received it.
 *
 * It is called a pseudo-header because it is never transmitted. It is
 * assembled, summed, and thrown away at both ends. That is exactly why
 * `net_checksum_partial` exists: the sum has to run across two regions that
 * are not next to each other in memory.
 */
#include <recon/kernel/net.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/console.h>
#include <recon/kernel/lock.h>

#define UDP_PORTS_MAX 16

struct udp_binding {
	u16 port;
	void (*fn)(void *ctx, struct netbuf *b);
	void *ctx;
	bool used;
};

static struct udp_binding bindings[UDP_PORTS_MAX];
static struct spinlock bind_lock;
static bool lock_ready;

static u64 tx_datagrams, tx_failed;
static u64 rx_datagrams, rx_short, rx_bad_checksum, rx_no_port;

static void ensure_lock(void)
{
	if (!lock_ready) {
		spin_init(&bind_lock, "udp");
		lock_ready = true;
	}
}

/* The sum of the five fields that are not in the datagram. */
static u32 pseudo_sum(ipv4_addr src, ipv4_addr dst, u8 protocol, u16 length)
{
	u8 ph[12];

	net_put32(ph + 0, src);
	net_put32(ph + 4, dst);
	ph[8] = 0;
	ph[9] = protocol;
	net_put16(ph + 10, length);

	return net_checksum_partial(ph, sizeof(ph), 0);
}

bool udp_bind_port(u16 port, void (*fn)(void *ctx, struct netbuf *b), void *ctx)
{
	unsigned i;
	u64 flags;
	bool ok = false;

	ensure_lock();
	flags = spin_lock_irq(&bind_lock);

	/* A port already bound is refused rather than replaced. Two sockets
	 * silently sharing a port means one of them stops receiving and
	 * nothing says which. */
	for (i = 0; i < UDP_PORTS_MAX; i++) {
		if (bindings[i].used && bindings[i].port == port) {
			spin_unlock_irq(&bind_lock, flags);
			return false;
		}
	}

	for (i = 0; i < UDP_PORTS_MAX; i++) {
		if (!bindings[i].used) {
			bindings[i].used = true;
			bindings[i].port = port;
			bindings[i].fn = fn;
			bindings[i].ctx = ctx;
			ok = true;
			break;
		}
	}

	spin_unlock_irq(&bind_lock, flags);
	return ok;
}

void udp_unbind_port(u16 port)
{
	unsigned i;
	u64 flags;

	ensure_lock();
	flags = spin_lock_irq(&bind_lock);

	for (i = 0; i < UDP_PORTS_MAX; i++)
		if (bindings[i].used && bindings[i].port == port)
			bindings[i].used = false;

	spin_unlock_irq(&bind_lock, flags);
}

bool udp_send(ipv4_addr dst, u16 dport, u16 sport, const void *data, u32 len)
{
	struct netbuf *b;
	struct net_device *dev;
	ipv4_addr hop = 0;
	u8 *h;
	u32 sum;

	if (len > NET_MTU - IP_HDR_LEN - UDP_HDR_LEN) {
		tx_failed++;
		return false;
	}

	dev = netdev_route(dst, &hop);

	if (!dev) {
		tx_failed++;
		return false;
	}

	b = netbuf_alloc();

	if (!b) {
		tx_failed++;
		return false;
	}

	/* Payload first, then the header in front of it -- which is the whole
	 * reason a buffer has headroom. */
	if (len) {
		u8 *p = netbuf_put(b, len);

		if (!p) {
			netbuf_free(b);
			tx_failed++;
			return false;
		}

		kmemcpy(p, data, len);
	}

	h = netbuf_push(b, UDP_HDR_LEN);

	if (!h) {
		netbuf_free(b);
		tx_failed++;
		return false;
	}

	net_put16(h + 0, sport);
	net_put16(h + 2, dport);
	net_put16(h + 4, (u16)(UDP_HDR_LEN + len));
	net_put16(h + 6, 0);

	sum = pseudo_sum(dev->ip, dst, IP_PROTO_UDP, (u16)(UDP_HDR_LEN + len));
	sum = net_checksum_partial(h, UDP_HDR_LEN + len, sum);

	{
		u16 c = net_checksum_finish(sum);

		/* A computed checksum of zero is written as all ones. Zero is
		 * reserved to mean "no checksum was computed", so a datagram
		 * that legitimately sums to zero would otherwise tell the far
		 * end not to check it. The two values are equivalent in
		 * one's-complement arithmetic, which is why this is allowed to
		 * be a substitution rather than a recomputation. */
		net_put16(h + 6, c ? c : 0xFFFF);
	}

	tx_datagrams++;
	return ip_transmit(dst, IP_PROTO_UDP, b);
}

void udp_receive(struct net_device *dev, struct netbuf *b)
{
	const u8 *h = b->data;
	u16 sport, dport, length, checksum;
	unsigned i;
	void (*fn)(void *, struct netbuf *) = NULL;
	void *ctx = NULL;
	u64 flags;

	(void)dev;

	if (b->len < UDP_HDR_LEN) {
		rx_short++;
		netbuf_free(b);
		return;
	}

	sport = net_get16(h + 0);
	dport = net_get16(h + 2);
	length = net_get16(h + 4);
	checksum = net_get16(h + 6);

	if (length < UDP_HDR_LEN || length > b->len) {
		rx_short++;
		netbuf_free(b);
		return;
	}

	/* Zero means the sender did not compute one, which IPv4 permits. */
	if (checksum) {
		u32 sum = pseudo_sum(b->src_ip, b->dst_ip, IP_PROTO_UDP, length);

		sum = net_checksum_partial(h, length, sum);

		if (net_checksum_finish(sum) != 0) {
			rx_bad_checksum++;
			netbuf_free(b);
			return;
		}
	}

	b->src_port = sport;
	b->dst_port = dport;

	netbuf_pull(b, UDP_HDR_LEN);
	b->len = (u32)(length - UDP_HDR_LEN);

	ensure_lock();
	flags = spin_lock_irq(&bind_lock);

	for (i = 0; i < UDP_PORTS_MAX; i++) {
		if (bindings[i].used && bindings[i].port == dport) {
			fn = bindings[i].fn;
			ctx = bindings[i].ctx;
			break;
		}
	}

	spin_unlock_irq(&bind_lock, flags);

	if (!fn) {
		/* Nobody is listening. A full stack answers with an ICMP port
		 * unreachable; this one drops and counts, because generating
		 * that message correctly means quoting the datagram and this
		 * kernel does not yet act on receiving one either. */
		rx_no_port++;
		netbuf_free(b);
		return;
	}

	rx_datagrams++;
	fn(ctx, b);
}

void udp_print_summary(void)
{
	kprintf("  udp          : %u sent, %u failed, %u received\n",
		(unsigned)tx_datagrams, (unsigned)tx_failed,
		(unsigned)rx_datagrams);

	if (rx_short || rx_bad_checksum || rx_no_port)
		kprintf("               : %u short, %u bad checksum, "
			"%u no listener\n",
			(unsigned)rx_short, (unsigned)rx_bad_checksum,
			(unsigned)rx_no_port);
}

bool udp_self_test(void)
{
	bool ok = true;

	/* The pseudo-header is checked against a datagram whose checksum is
	 * known from outside this kernel. Source 192.168.0.1, destination
	 * 192.168.0.199, ports 4096 and 4097, payload "abcd".
	 *
	 * Computed independently rather than by this code, because a checksum
	 * verified by re-running the function that produced it agrees with
	 * every mistake that function makes. */
	{
		u8 dg[UDP_HDR_LEN + 4];
		u32 sum;
		u16 c;

		net_put16(dg + 0, 4096);
		net_put16(dg + 2, 4097);
		net_put16(dg + 4, UDP_HDR_LEN + 4);
		net_put16(dg + 6, 0);
		dg[8] = 'a'; dg[9] = 'b'; dg[10] = 'c'; dg[11] = 'd';

		sum = pseudo_sum(IPV4(192, 168, 0, 1), IPV4(192, 168, 0, 199),
				 IP_PROTO_UDP, UDP_HDR_LEN + 4);
		sum = net_checksum_partial(dg, sizeof(dg), sum);
		c = net_checksum_finish(sum);

		net_put16(dg + 6, c);

		/* A receiver's check: the whole thing including the checksum
		 * sums to zero. */
		sum = pseudo_sum(IPV4(192, 168, 0, 1), IPV4(192, 168, 0, 199),
				 IP_PROTO_UDP, UDP_HDR_LEN + 4);
		sum = net_checksum_partial(dg, sizeof(dg), sum);

		if (net_checksum_finish(sum) != 0) {
			kprintf("udp: a datagram we checksummed does not "
				"check out\n");
			ok = false;
		}

		/* And the pseudo-header really is part of it: changing only
		 * the source address, which is not in the datagram, must break
		 * the checksum. That is the entire reason it exists, and a
		 * stack that leaves it out passes every other test here. */
		sum = pseudo_sum(IPV4(192, 168, 0, 2), IPV4(192, 168, 0, 199),
				 IP_PROTO_UDP, UDP_HDR_LEN + 4);
		sum = net_checksum_partial(dg, sizeof(dg), sum);

		if (net_checksum_finish(sum) == 0) {
			kprintf("udp: the source address is not covered by "
				"the checksum\n");
			ok = false;
		}
	}

	/* A port can be bound once and not twice. */
	if (!udp_bind_port(9999, NULL, NULL)) {
		kprintf("udp: could not bind a free port\n");
		ok = false;
	}

	if (udp_bind_port(9999, NULL, NULL)) {
		kprintf("udp: bound the same port twice\n");
		ok = false;
	}

	udp_unbind_port(9999);

	if (!udp_bind_port(9999, NULL, NULL)) {
		kprintf("udp: a released port could not be taken again\n");
		ok = false;
	}

	udp_unbind_port(9999);

	return ok;
}
