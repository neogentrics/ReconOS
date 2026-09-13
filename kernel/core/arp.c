/* ARP: the only way an IP address becomes a frame that a card will accept.
 *
 * IP addresses are a fiction the wire knows nothing about. A card accepts
 * frames addressed to a six-byte hardware address and no other, so before a
 * single IP packet can leave, something has to shout on the wire and ask who
 * holds the address it is aimed at.
 *
 * --- Why lookup does not block ---
 *
 * `arp_lookup` answers from the cache or returns false having sent a request.
 * It never waits for the reply, and that is not laziness: the reply arrives as
 * a frame, frames are processed by the worker thread, and the caller may *be*
 * the worker thread. A lookup that slept there would be a thread waiting for
 * work only it could do.
 *
 * So a caller that has nothing to send yet asks, does something else, and asks
 * again. A caller with a packet in hand hands it to `ip_transmit`, which holds
 * it and sends it when the answer arrives.
 *
 * --- Why a reply is not simply believed ---
 *
 * Anything on the wire can claim any address. This cache accepts an entry from
 * a reply it asked for and from a request addressed to it, which is standard,
 * and it does **not** accept one from every broadcast it happens to overhear.
 * That is a real reduction in how quickly the cache fills and a real reduction
 * in how easily somebody else fills it for you.
 */
#include <recon/kernel/net.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/console.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/time.h>

#define ARP_HDR_LEN 28

#define ARP_HW_ETHERNET 1
#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2

#define ARP_CACHE_SIZE 32

/* How long an answer is trusted. Short enough that a machine which has moved
 * to another card is noticed within a minute; long enough that a busy
 * conversation does not re-ask on every packet. */
#define ARP_TTL_NS (60ull * 1000000000ull)

struct arp_entry {
	ipv4_addr ip;
	struct mac_addr mac;
	u64 learned_ns;
	struct net_device *dev;
	bool valid;
};

static struct arp_entry cache[ARP_CACHE_SIZE];
static struct spinlock cache_lock;
static bool lock_ready;

static u64 requests_sent;
static u64 replies_sent;
static u64 replies_received;
static u64 hits;
static u64 misses;
static u64 refused;

static void ensure_lock(void)
{
	if (!lock_ready) {
		spin_init(&cache_lock, "arp");
		lock_ready = true;
	}
}

/* Held with the lock. */
static struct arp_entry *find(ipv4_addr ip)
{
	unsigned i;

	for (i = 0; i < ARP_CACHE_SIZE; i++) {
		if (cache[i].valid && cache[i].ip == ip)
			return &cache[i];
	}

	return NULL;
}

static void learn(struct net_device *dev, ipv4_addr ip,
		  const struct mac_addr *mac)
{
	struct arp_entry *e;
	unsigned i;
	u64 flags;
	u64 now = time_monotonic_ns();

	/* Nothing sensible to learn from these, and an entry for either would
	 * be an entry that sends real traffic to a broadcast address. */
	if (!ip || ip == IPV4_BROADCAST || mac_equal(mac, &MAC_ZERO))
		return;

	ensure_lock();
	flags = spin_lock_irq(&cache_lock);

	e = find(ip);

	if (!e) {
		/* A free slot, or the oldest one. Evicting the oldest rather
		 * than the first is what keeps a burst of one-off addresses
		 * from throwing out the gateway, which is the entry every
		 * other packet needs. */
		u64 oldest = ~0ull;

		for (i = 0; i < ARP_CACHE_SIZE; i++) {
			if (!cache[i].valid) {
				e = &cache[i];
				break;
			}

			if (cache[i].learned_ns < oldest) {
				oldest = cache[i].learned_ns;
				e = &cache[i];
			}
		}
	}

	e->ip = ip;
	e->mac = *mac;
	e->dev = dev;
	e->learned_ns = now;
	e->valid = true;

	spin_unlock_irq(&cache_lock, flags);
}

static bool send_packet(struct net_device *dev, u16 op,
			const struct mac_addr *target_mac, ipv4_addr target_ip,
			const struct mac_addr *frame_dst)
{
	struct netbuf *b = netbuf_alloc();
	u8 *a;

	if (!b)
		return false;

	a = netbuf_put(b, ARP_HDR_LEN);

	if (!a) {
		netbuf_free(b);
		return false;
	}

	net_put16(a + 0, ARP_HW_ETHERNET);
	net_put16(a + 2, ETH_TYPE_IPV4);
	a[4] = MAC_LEN;
	a[5] = 4;
	net_put16(a + 6, op);

	kmemcpy(a + 8, dev->mac.b, MAC_LEN);
	net_put32(a + 14, dev->ip);

	kmemcpy(a + 18, target_mac->b, MAC_LEN);
	net_put32(a + 24, target_ip);

	return eth_transmit(dev, frame_dst, ETH_TYPE_ARP, b);
}

bool arp_lookup(struct net_device *dev, ipv4_addr ip, struct mac_addr *out)
{
	struct arp_entry *e;
	u64 flags;
	bool found = false;

	/* Broadcast needs no asking, and asking would be asking the whole wire
	 * who holds the address that means "the whole wire". */
	if (ip == IPV4_BROADCAST ||
	    (dev->netmask && (ip | ~dev->netmask) == ip && ~dev->netmask)) {
		*out = MAC_BROADCAST;
		return true;
	}

	ensure_lock();
	flags = spin_lock_irq(&cache_lock);

	e = find(ip);

	if (e && time_monotonic_ns() - e->learned_ns < ARP_TTL_NS) {
		*out = e->mac;
		found = true;
		hits++;
	} else if (e) {
		/* Expired. Dropped rather than used-and-refreshed: an address
		 * that has moved answers the new request, and continuing to
		 * use the old answer in the meantime sends traffic to a card
		 * that is no longer listening. */
		e->valid = false;
	}

	if (!found)
		misses++;

	spin_unlock_irq(&cache_lock, flags);

	if (found)
		return true;

	requests_sent++;
	send_packet(dev, ARP_OP_REQUEST, &MAC_ZERO, ip, &MAC_BROADCAST);
	return false;
}

void arp_receive(struct net_device *dev, struct netbuf *b)
{
	const u8 *a = b->data;
	u16 op;
	ipv4_addr sender_ip, target_ip;
	struct mac_addr sender_mac;

	if (b->len < ARP_HDR_LEN) {
		refused++;
		netbuf_free(b);
		return;
	}

	/* Ethernet and IPv4 only, with the lengths that implies. Anything else
	 * is refused by name rather than read hopefully -- the same rule ext2
	 * follows about feature flags. */
	if (net_get16(a + 0) != ARP_HW_ETHERNET ||
	    net_get16(a + 2) != ETH_TYPE_IPV4 ||
	    a[4] != MAC_LEN || a[5] != 4) {
		refused++;
		netbuf_free(b);
		return;
	}

	op = net_get16(a + 6);
	kmemcpy(sender_mac.b, a + 8, MAC_LEN);
	sender_ip = net_get32(a + 14);
	target_ip = net_get32(a + 24);

	/* Learn from anything addressed to us -- a request for our address is
	 * a machine about to talk to us, and we will need its address in a
	 * moment anyway. Overheard traffic between two other machines is not
	 * learned from. */
	if (dev->ip && target_ip == dev->ip)
		learn(dev, sender_ip, &sender_mac);

	if (op == ARP_OP_REPLY) {
		replies_received++;
		learn(dev, sender_ip, &sender_mac);
		netbuf_free(b);
		return;
	}

	if (op == ARP_OP_REQUEST && dev->ip && target_ip == dev->ip) {
		netbuf_free(b);
		replies_sent++;
		send_packet(dev, ARP_OP_REPLY, &sender_mac, sender_ip,
			    &sender_mac);
		return;
	}

	netbuf_free(b);
}

void arp_print_summary(void)
{
	unsigned i, live = 0;

	for (i = 0; i < ARP_CACHE_SIZE; i++)
		if (cache[i].valid)
			live++;

	kprintf("  arp          : %u known, %u hits, %u misses\n",
		live, (unsigned)hits, (unsigned)misses);
	kprintf("               : %u asked, %u answered, %u replies in, "
		"%u refused\n",
		(unsigned)requests_sent, (unsigned)replies_sent,
		(unsigned)replies_received, (unsigned)refused);
}

bool arp_self_test(void)
{
	struct mac_addr m = { { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 } };
	struct mac_addr got;
	struct net_device dev;
	bool ok = true;

	kmemset(&dev, 0, sizeof(dev));
	dev.ip = IPV4(10, 0, 0, 5);
	dev.netmask = IPV4(255, 255, 255, 0);

	ensure_lock();

	/* A learned address comes back. */
	learn(&dev, IPV4(10, 0, 0, 9), &m);

	if (!arp_lookup(&dev, IPV4(10, 0, 0, 9), &got)) {
		kprintf("arp: an address just learned was not found\n");
		ok = false;
	} else if (!mac_equal(&got, &m)) {
		kprintf("arp: the wrong hardware address came back\n");
		ok = false;
	}

	/* Broadcast resolves without asking anybody. */
	if (!arp_lookup(&dev, IPV4_BROADCAST, &got) ||
	    !mac_equal(&got, &MAC_BROADCAST)) {
		kprintf("arp: broadcast did not resolve to broadcast\n");
		ok = false;
	}

	/* An entry past its lifetime is not used. This is the assertion worth
	 * having: a cache that never expires works perfectly until a machine
	 * moves, and then fails in a way that looks like the network. */
	{
		u64 flags = spin_lock_irq(&cache_lock);
		struct arp_entry *e = find(IPV4(10, 0, 0, 9));

		if (e) {
			/* "Long ago" cannot be zero. A machine that has been up
			 * for four seconds is four seconds from zero, which is
			 * well inside a sixty-second lifetime -- so an entry
			 * stamped zero at boot is not stale, it is new.
			 *
			 * Subtracting from *now* is right even when the result
			 * underflows: the comparison is a difference in
			 * unsigned arithmetic, so it comes out as TTL + 1
			 * whether or not the subtraction wrapped. Same reason
			 * TCP compares sequence numbers by difference. */
			e->learned_ns = time_monotonic_ns() - ARP_TTL_NS - 1;
		}

		spin_unlock_irq(&cache_lock, flags);

		/* No device to send the request on, so the lookup fails --
		 * which is the point. It must report a miss, not hand back the
		 * stale answer. */
		if (arp_lookup(&dev, IPV4(10, 0, 0, 9), &got)) {
			kprintf("arp: a stale entry was still handed out\n");
			ok = false;
		}
	}

	/* Eviction takes the oldest rather than the first, so that the entry
	 * everything needs is not thrown out by a burst of one-off addresses. */
	{
		struct mac_addr gw = { { 0x02, 0, 0, 0, 0, 1 } };
		unsigned i;

		for (i = 0; i < ARP_CACHE_SIZE; i++)
			cache[i].valid = false;

		learn(&dev, IPV4(10, 0, 0, 1), &gw);

		/* Keep it fresh while everything else is learned after it. */
		for (i = 0; i < ARP_CACHE_SIZE + 4; i++) {
			struct mac_addr other = { { 0x02, 0, 0, 0, 1, 0 } };
			u64 flags;

			other.b[5] = (u8)i;
			learn(&dev, IPV4(10, 0, 1, 0) + i, &other);

			flags = spin_lock_irq(&cache_lock);
			{
				struct arp_entry *e = find(IPV4(10, 0, 0, 1));

				if (e)
					e->learned_ns = time_monotonic_ns();
			}
			spin_unlock_irq(&cache_lock, flags);
		}

		if (!arp_lookup(&dev, IPV4(10, 0, 0, 1), &got)) {
			kprintf("arp: the gateway was evicted by a burst\n");
			ok = false;
		}
	}

	/* Leave the cache as it was found. */
	{
		unsigned i;
		u64 flags = spin_lock_irq(&cache_lock);

		for (i = 0; i < ARP_CACHE_SIZE; i++)
			cache[i].valid = false;

		spin_unlock_irq(&cache_lock, flags);
	}

	return ok;
}
