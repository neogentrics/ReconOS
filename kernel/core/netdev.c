/* Network devices, and the line between an interrupt and the stack.
 *
 * A driver takes a frame off the hardware and calls `netdev_receive`. That may
 * be an interrupt handler, so what happens there is deliberately almost
 * nothing: the buffer goes on a queue, and the worker thread walks it up.
 *
 * The reason is not politeness about interrupt latency. It is that everything
 * above Ethernet takes locks -- the ARP cache, the socket table, a TCP
 * connection -- and an interrupt that lands on a processor already holding one
 * of those and then tries to take it deadlocks that processor. The queue is
 * the only structure the interrupt touches, it is taken with interrupts off,
 * and nothing above it is reachable from that context at all.
 *
 * The receive queue is **bounded**. A machine that is sent frames faster than
 * it can process them must drop them, and it must drop them *here*, where one
 * counter records it -- because the alternative is allocating buffers until
 * the page allocator is empty, at which point the machine stops doing anything
 * else either. A network that can take the machine down by talking to it is
 * not a network stack, and the bound is what makes the difference.
 */
#include <recon/kernel/net.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/console.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/work.h>

static struct net_device devices[NET_MAX_DEVICES];
static unsigned device_count;

static struct spinlock rx_lock;
static struct netbuf *rx_head;
static struct netbuf *rx_tail;
static unsigned rx_queued;

/* How many frames may wait. Sized so the queue costs at most a quarter of a
 * megabyte of pages at full stretch, which a machine that has a network card
 * can afford, and small enough that it is reached rather than theoretical. */
#define RX_QUEUE_MAX 64

static u64 rx_overflow;		/* frames dropped because the queue was full */
static u64 rx_serviced;
static bool initialised;

void netdev_init(void)
{
	if (initialised)
		return;

	spin_init(&rx_lock, "net-rx");
	initialised = true;
}

struct net_device *netdev_register(const char *name,
				   const struct net_device_ops *ops,
				   void *driver, const struct mac_addr *mac)
{
	struct net_device *d;

	netdev_init();

	if (device_count >= NET_MAX_DEVICES) {
		kprintf("net: no room for %s, %u already registered\n",
			name, device_count);
		return NULL;
	}

	d = &devices[device_count++];
	kmemset(d, 0, sizeof(*d));

	kstrlcpy(d->name, name, sizeof(d->name));
	d->ops = ops;
	d->driver = driver;
	d->mtu = NET_MTU;
	d->up = true;

	if (mac)
		d->mac = *mac;

	return d;
}

void netdev_forget_last(void)
{
	if (device_count)
		kmemset(&devices[--device_count], 0, sizeof(devices[0]));
}

unsigned netdev_count(void)
{
	return device_count;
}

struct net_device *netdev_at(unsigned i)
{
	return i < device_count ? &devices[i] : NULL;
}

struct net_device *netdev_by_name(const char *name)
{
	unsigned i;

	for (i = 0; i < device_count; i++) {
		const char *a = devices[i].name;
		const char *b = name;

		while (*a && *a == *b) {
			a++;
			b++;
		}

		if (!*a && !*b)
			return &devices[i];
	}

	return NULL;
}

struct net_device *netdev_route(ipv4_addr dst, ipv4_addr *next_hop)
{
	unsigned i;
	struct net_device *fallback = NULL;
	ipv4_addr fallback_hop = 0;

	/* A broadcast may leave a card that has **no address yet**, and this is
	 * the one case where that is true. DHCP has to send before it has been
	 * given anything to be the source of, so a router that requires an
	 * address first makes the protocol that assigns addresses impossible to
	 * run -- which is a circle that only shows up on a real network, since
	 * every test above configures its device by hand first. */
	if (dst == IPV4_BROADCAST) {
		for (i = 0; i < device_count; i++) {
			if (!devices[i].up)
				continue;

			if (next_hop)
				*next_hop = IPV4_BROADCAST;

			return &devices[i];
		}

		return NULL;
	}

	for (i = 0; i < device_count; i++) {
		struct net_device *d = &devices[i];

		if (!d->up || !d->ip)
			continue;

		/* On the same wire: the frame goes straight to the host. */
		if (d->netmask && (dst & d->netmask) == (d->ip & d->netmask)) {
			if (next_hop)
				*next_hop = dst;
			return d;
		}

		/* Otherwise it goes to a router, and the *frame* is addressed
		 * to the router even though the *packet* is addressed to the
		 * far end. Getting that wrong is the classic first-stack bug:
		 * ARP for an address on another continent, and wait for ever. */
		if (!fallback && d->gateway) {
			fallback = d;
			fallback_hop = d->gateway;
		}
	}

	if (fallback && next_hop)
		*next_hop = fallback_hop;

	return fallback;
}

/* --- Receive --------------------------------------------------------------- */

static void rx_work_fn(void *arg);
static struct work rx_work;
static bool rx_work_ready;

void netdev_receive(struct net_device *dev, struct netbuf *b)
{
	u64 flags;

	if (!b)
		return;

	b->dev = dev;
	b->next = NULL;

	flags = spin_lock_irq(&rx_lock);

	if (rx_queued >= RX_QUEUE_MAX) {
		/* Dropped here, and counted here. The frame is gone and the
		 * far end will notice or not; what must not happen is the
		 * machine trying to keep up by allocating. */
		rx_overflow++;
		dev->rx_dropped++;
		spin_unlock_irq(&rx_lock, flags);
		netbuf_free(b);
		return;
	}

	if (rx_tail)
		rx_tail->next = b;
	else
		rx_head = b;

	rx_tail = b;
	rx_queued++;

	dev->rx_packets++;
	dev->rx_bytes += b->len;

	spin_unlock_irq(&rx_lock, flags);

	/* Ask the worker thread to come and take them. Scheduling an already
	 * scheduled item is refused by `work_schedule` and that refusal is
	 * correct rather than a lost wake-up: the item has not run yet, so it
	 * will see this frame when it does. */
	if (rx_work_ready)
		work_schedule(&rx_work);
}

static struct netbuf *rx_take(void)
{
	struct netbuf *b;
	u64 flags = spin_lock_irq(&rx_lock);

	b = rx_head;

	if (b) {
		rx_head = b->next;
		if (!rx_head)
			rx_tail = NULL;
		rx_queued--;
		b->next = NULL;
	}

	spin_unlock_irq(&rx_lock, flags);
	return b;
}

void netdev_service(void)
{
	unsigned i;
	struct netbuf *b;

	/* Poll first. An interrupt-driven card usually has nothing here, and
	 * calling it anyway is what keeps a missed interrupt from being a
	 * machine that has silently stopped receiving. */
	for (i = 0; i < device_count; i++) {
		struct net_device *d = &devices[i];

		if (d->up && d->ops->poll)
			d->ops->poll(d);
	}

	while ((b = rx_take()) != NULL) {
		rx_serviced++;
		eth_receive(b->dev, b);
	}
}

static void rx_work_fn(void *arg)
{
	(void)arg;
	netdev_service();
}

bool netdev_transmit(struct net_device *dev, struct netbuf *b)
{
	if (!dev || !dev->up || !dev->ops->transmit) {
		if (dev)
			dev->tx_dropped++;
		netbuf_free(b);
		return false;
	}

	if (b->len > ETH_FRAME_MAX) {
		/* Refused rather than truncated, which is this project's rule
		 * everywhere else and is not different here: a frame cut to fit
		 * is a different frame, and the far end has no way to tell. */
		dev->tx_errors++;
		netbuf_free(b);
		return false;
	}

	dev->tx_packets++;
	dev->tx_bytes += b->len;

	if (!dev->ops->transmit(dev, b)) {
		dev->tx_packets--;
		dev->tx_bytes -= b->len;
		dev->tx_errors++;
		return false;
	}

	return true;
}

/* --- What it is doing ------------------------------------------------------ */

void netdev_print_summary(void)
{
	unsigned i;
	char mac[20], ip[20];

	if (!device_count) {
		kprintf("  devices      : none found\n");
		return;
	}

	for (i = 0; i < device_count; i++) {
		struct net_device *d = &devices[i];

		kprintf("  %s%s: %s", d->name,
			d->up ? " up  " : " down",
			mac_format(&d->mac, mac));

		if (d->ip)
			kprintf(" %s", ipv4_format(d->ip, ip));

		kprintf("\n");

		kprintf("    rx         : %u packets, %u bytes, %u dropped, "
			"%u errors\n",
			(unsigned)d->rx_packets, (unsigned)d->rx_bytes,
			(unsigned)d->rx_dropped, (unsigned)d->rx_errors);
		kprintf("    tx         : %u packets, %u bytes, %u dropped, "
			"%u errors\n",
			(unsigned)d->tx_packets, (unsigned)d->tx_bytes,
			(unsigned)d->tx_dropped, (unsigned)d->tx_errors);
	}

	kprintf("  queue        : %u waiting, %u serviced, %u overflowed\n",
		rx_queued, (unsigned)rx_serviced, (unsigned)rx_overflow);
}

void net_init(void)
{
	netdev_init();

	work_init(&rx_work, rx_work_fn, NULL);
	rx_work_ready = true;
}

/* --- The test -------------------------------------------------------------- */

/* A device that exists only here, so that the queue, the routing and the
 * counters can be exercised on a machine with no network card at all.
 *
 * This is the same reason the VFS test defines a `file_ops` the kernel has
 * never heard of: an interface with one implementation is not known to be an
 * interface. It is also what makes this testable on every boot rather than
 * only the boots that happen to have a NIC -- which is the difference between
 * a test that runs and a test that prints "no device on this machine" and
 * counts as passing. That distinction is what KF-187 was about.
 */
static struct netbuf *loop_last;
static unsigned loop_sent;

static bool loop_transmit(struct net_device *dev, struct netbuf *b)
{
	(void)dev;
	loop_sent++;

	netbuf_free(loop_last);
	loop_last = b;
	return true;
}

static const struct net_device_ops loop_ops = {
	.transmit = loop_transmit,
	.enable_interrupts = NULL,
	.poll = NULL,
};

bool netdev_self_test(void)
{
	static const struct mac_addr mac = { { 0x02, 0, 0, 0, 0, 1 } };
	struct net_device *d;
	struct net_device *routed;
	ipv4_addr hop = 0;
	bool ok = true;
	unsigned before = device_count;

	netdev_init();

	d = netdev_register("test0", &loop_ops, NULL, &mac);

	if (!d) {
		kprintf("netdev: could not register a test device\n");
		return false;
	}

	d->ip = IPV4(192, 168, 1, 10);
	d->netmask = IPV4(255, 255, 255, 0);
	d->gateway = IPV4(192, 168, 1, 1);

	/* An address on this wire goes straight to it. */
	routed = netdev_route(IPV4(192, 168, 1, 55), &hop);

	if (routed != d || hop != IPV4(192, 168, 1, 55)) {
		kprintf("net: a local address did not route locally\n");
		ok = false;
	}

	/* One that is not goes to the gateway -- and the *next hop* is the
	 * router, not the destination. A stack that returns the destination
	 * here ARPs for an address nothing on this wire holds. */
	routed = netdev_route(IPV4(8, 8, 8, 8), &hop);

	if (routed != d || hop != IPV4(192, 168, 1, 1)) {
		char buf[20];

		kprintf("net: a remote address routed to %s, not the gateway\n",
			ipv4_format(hop, buf));
		ok = false;
	}

	/* The queue takes frames and hands them back in order. */
	{
		struct netbuf *a = netbuf_alloc();
		struct netbuf *b = netbuf_alloc();

		if (a && b) {
			u8 *p = netbuf_put(a, 4);

			if (p)
				p[0] = 0xA1;

			p = netbuf_put(b, 4);
			if (p)
				p[0] = 0xB2;

			netdev_receive(d, a);
			netdev_receive(d, b);

			if (rx_queued != 2) {
				kprintf("net: %u frames queued, expected 2\n",
					rx_queued);
				ok = false;
			}

			{
				struct netbuf *first = rx_take();
				struct netbuf *second = rx_take();

				if (!first || first->data[0] != 0xA1)
					ok = false;
				if (!second || second->data[0] != 0xB2) {
					kprintf("net: the queue reordered\n");
					ok = false;
				}

				netbuf_free(first);
				netbuf_free(second);
			}
		} else {
			netbuf_free(a);
			netbuf_free(b);
		}
	}

	/* And it is bounded. This is the assertion that matters: without it a
	 * machine can be taken down by being talked to, and the failure is not
	 * visible until somebody does it. */
	{
		unsigned i;
		u64 dropped_before = rx_overflow;

		for (i = 0; i < RX_QUEUE_MAX + 8; i++) {
			struct netbuf *b = netbuf_alloc();

			if (!b)
				break;

			netbuf_put(b, 4);
			netdev_receive(d, b);
		}

		if (rx_queued > RX_QUEUE_MAX) {
			kprintf("net: queue grew to %u, past its bound of %u\n",
				rx_queued, RX_QUEUE_MAX);
			ok = false;
		}

		if (rx_overflow == dropped_before) {
			kprintf("net: the queue never reported an overflow\n");
			ok = false;
		}

		while (rx_queued) {
			struct netbuf *b = rx_take();

			netbuf_free(b);
		}
	}

	/* Transmit counts, and refuses something too big for a frame. */
	{
		struct netbuf *b = netbuf_alloc();
		u64 errors_before = d->tx_errors;

		if (b) {
			netbuf_put(b, 64);
			if (!netdev_transmit(d, b)) {
				kprintf("net: a 64-byte frame was refused\n");
				ok = false;
			}
		}

		b = netbuf_alloc();

		if (b) {
			/* Bigger than a frame may be, using the whole buffer. */
			netbuf_put(b, NET_BUF_CAPACITY - NET_HEADROOM);

			if (netdev_transmit(d, b)) {
				kprintf("net: an oversized frame was sent\n");
				ok = false;
			} else if (d->tx_errors == errors_before) {
				kprintf("net: an oversized frame was refused "
					"without being counted\n");
				ok = false;
			}
		}
	}

	netbuf_free(loop_last);
	loop_last = NULL;

	/* Give the slot back, so a machine that runs the tests twice does not
	 * fill the table with test devices. */
	while (device_count > before)
		netdev_forget_last();

	return ok;
}
