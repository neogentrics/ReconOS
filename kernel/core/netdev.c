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
#include <recon/kernel/suspend.h>
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

/* Held by the self-test, and only for the length of one flood.
 *
 * The bounded-queue assertion needs the queue to actually fill, and
 * `netdev_receive` schedules the drain on its way out. Given a spare
 * processor the drain keeps up, the queue never reaches its bound, and the
 * test then reports that the bound is broken -- when what really happened is
 * that it could not build its own precondition. It failed at eight processors
 * and passed at four, on identical code. That is KF-201, and it is the same
 * shape as KF-197: a check whose setup the rig does not guarantee.
 *
 * Read under `rx_lock`, so any take beginning after this is set sees it. A
 * take already inside the lock removes at most one frame, and the flood runs
 * eight frames past the bound. */
static bool rx_drain_held;

static void rx_hold_drain(bool held)
{
	u64 flags = spin_lock_irq(&rx_lock);

	rx_drain_held = held;
	spin_unlock_irq(&rx_lock, flags);
}
static u64 rx_serviced;

/* How many times a driver said it had something, whether or not a drain was
 * already queued. Printed, because a card whose interrupt never arrives and a
 * card that is simply quiet produce identical frame counts -- and this is the
 * only number that tells the two apart. */
static u64 rx_wakes;

/* How many registered devices said they could raise an interrupt, and how many
 * said they could not. Both printed: a machine where every card is polled is a
 * machine with a whole class of driver path never exercised, and that is worth
 * seeing rather than inferring from the absence of a line. */
static unsigned interrupt_capable;
static unsigned interrupt_refused;

/* How many registrations were refused for a name already in use, and how many
 * of those the self-test asked for on purpose. Both, for the reason the
 * interrupt layer keeps both: a number that is never zero stops being read,
 * and a suppressed message that is never counted stops being a fact. */
static u64 name_refusals;
static u64 suppressed_refusals;
static unsigned expected_name_refusals;

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

	/* Refused rather than accepted as a second device with the same name.
	 *
	 * This used to copy the string and ask nothing, which was correct for
	 * as long as there was one driver -- a driver numbering its own cards
	 * from zero is numbering every card on the machine. The second driver
	 * makes that false, and the failure is quiet: two `eth0`s in the
	 * device table, `netdev_by_name` answering with whichever registered
	 * first, and a summary that prints one name twice with two different
	 * sets of counters under it. NW-002.
	 *
	 * Refusing is the right half of the fix and `netdev_name` is the
	 * other: a driver that asks for a name it cannot be given should be
	 * told, not quietly given a name that already means something else. */
	if (netdev_by_name(name)) {
		name_refusals++;

		/* The self-test registers a duplicate on purpose -- that is
		 * the assertion -- so without this every boot prints a line
		 * that reads like a collision and is not one. A message that
		 * is always there is a message a reader stops seeing, and the
		 * one that mattered would go past with it. Same bargain as
		 * `irq_note_expected_refusals`, and deliberately the same
		 * shape so there is one idiom for this and not two. */
		if (expected_name_refusals) {
			expected_name_refusals--;
			suppressed_refusals++;
			return NULL;
		}

		kprintf("net: %s is already registered; refused\n", name);
		return NULL;
	}

	d = &devices[device_count++];
	kmemset(d, 0, sizeof(*d));

	kstrlcpy(d->name, name, sizeof(d->name));
	d->ops = ops;
	d->driver = driver;
	d->mtu = NET_MTU;
	d->up = true;

	/* Connected until a driver says otherwise.
	 *
	 * `link` is now consulted by `netdev_route`, so the default decides what
	 * happens to every device whose driver never sets it -- and three of
	 * those exist in this tree, all of them test devices with no cable to
	 * have an opinion about. Defaulting to false would make them unroutable
	 * and take the whole stack self-test down with them.
	 *
	 * True is also the honest default for a real driver that cannot read a
	 * PHY: "I do not know" and "there is no cable" are different answers, and
	 * routing round a card that is actually fine is worse than trying and
	 * failing. A driver that *can* tell overwrites this at attach and on
	 * every change. */
	d->link = true;

	if (mac)
		d->mac = *mac;

	/* Declared to the suspend layer with no ops, which is the honest
	 * state: this holds hardware state and nothing here can bring it
	 * back yet. Recorded rather than remembered, so the list of what
	 * would not survive a suspend is generated from what is actually in
	 * the machine. */
	suspend_declare(d->name, 0);

	/* And now the card may interrupt, if it can.
	 *
	 * This call is NW-008. `enable_interrupts` has been declared in
	 * `net_device_ops` with a paragraph of comment explaining when a driver
	 * should implement it, and **nothing in the kernel called it** -- there
	 * was no `ops->enable_interrupts` anywhere. All three implementations
	 * were null, so a missing call site and a correctly-skipped optional
	 * hook looked identical from every angle except grepping for the call.
	 *
	 * **Here rather than at the end of a driver's attach**, and the ordering
	 * is the reason the hook is worth having at all: the device is in the
	 * table before anything is allowed to interrupt about it. A driver that
	 * armed its own mask first can take an interrupt for a device the stack
	 * has not been told about, and the handler's first act is to ask the
	 * worker thread to go and poll every registered device -- which is a
	 * list this one is not on yet.
	 *
	 * False is not a failure. It means the card cannot, and the polling path
	 * carries it; `netdev_service` calls every device's `poll` whether or not
	 * it interrupts, for exactly this reason. */
	if (d->ops && d->ops->enable_interrupts) {
		if (d->ops->enable_interrupts(d))
			interrupt_capable++;
		else
			interrupt_refused++;
	}

	return d;
}

void netdev_note_expected_refusal(void)
{
	expected_name_refusals++;
}

bool netdev_name(const char *prefix, char *out, unsigned len)
{
	unsigned n;
	unsigned plen = 0;

	if (!prefix || !out || len < 3)
		return false;

	while (prefix[plen])
		plen++;

	/* Room for the prefix, one digit and the terminator. Checked rather
	 * than assumed: kstrlcpy would truncate the prefix and then every
	 * candidate would collide with the one before it, and the loop below
	 * would report that every index is taken on a machine with one card. */
	if (plen + 2 > len)
		return false;

	for (n = 0; n < NET_MAX_DEVICES; n++) {
		kstrlcpy(out, prefix, len);
		out[plen] = (char)('0' + n);
		out[plen + 1] = 0;

		if (!netdev_by_name(out))
			return true;
	}

	out[0] = 0;
	return false;
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

/* The two questions the stack asks about a device, each with a name, because
 * they are not the same question and the difference is load-bearing.
 *
 * `has_cable` is what a **broadcast** needs: up, and a wire in it. It
 * deliberately does not ask for an address, because DHCP has to send before it
 * has been given one -- see the comment in `netdev_route`.
 *
 * `is_addressable` is what everything else needs: that, and an address to be
 * the source of.
 *
 * **These exist as functions because the second one had been written twice.**
 * `netdev_route` asked `up && link && ip`; `logport.c` asked `up && ip` and
 * left out the cable, having reached for a `netdev_primary` that did not exist
 * and walked the table itself instead. The two answers differ on exactly the
 * field NW-003 had just given a meaning to -- which is NW-010, and the reason
 * the fix is one predicate rather than two that agree today. */
static bool has_cable(const struct net_device *d)
{
	return d && d->up && d->link;
}

static bool is_addressable(const struct net_device *d)
{
	return has_cable(d) && d->ip;
}

/* The device to give out as this machine's address, when the question has no
 * destination to route toward.
 *
 * **Added because a caller reached for it, not because an interface wanted
 * rounding out.** `logport.c` needs one address to print and to listen on, and
 * has no destination to route toward, so `netdev_route` cannot answer it. The
 * comment there says plainly that it looked for this and found nothing.
 *
 * First addressable rather than first: a machine with two cards where only one
 * has come up should report the one somebody can reach. That is `logport`'s
 * own reasoning and it is right; it just could not enforce it, because the
 * cable is the half of "come up" its copy of the test left out. */
struct net_device *netdev_primary(void)
{
	unsigned i;

	for (i = 0; i < device_count; i++)
		if (is_addressable(&devices[i]))
			return &devices[i];

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
			if (!has_cable(&devices[i]))
				continue;

			if (next_hop)
				*next_hop = IPV4_BROADCAST;

			return &devices[i];
		}

		return NULL;
	}

	for (i = 0; i < device_count; i++) {
		struct net_device *d = &devices[i];

		/* `link` as well as `up`, and they are different questions.
		 *
		 * `up` is whether the kernel is willing to use the card; `link`
		 * is whether there is a cable in it. A card that is up with
		 * nothing plugged in will accept a frame, count it as sent, and
		 * drop it on the floor -- so routing to it is how a machine
		 * with two cards sends everything out of the dead one.
		 *
		 * This field existed and nothing read it, which is NW-003. It
		 * was written once by virtio-net as a constant `true`, which is
		 * the honest value for a card that has no cable to have an
		 * opinion about -- and that is exactly why the gap was
		 * invisible: with one driver, the field could not disagree with
		 * anything. */
		if (!is_addressable(d))
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

	/* The self-test holds this while it fills the queue on purpose. */
	if (rx_drain_held) {
		spin_unlock_irq(&rx_lock, flags);
		return NULL;
	}

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

void netdev_wake(void)
{
	/* Counted before the refusal, not after.
	 *
	 * The number worth having is "how many times did a card say it had
	 * something", and `work_schedule` returns false for a drain already
	 * queued -- which is the common case under load and is not a failure.
	 * Counting only the successes would make a busy card look like a
	 * silent one, which is the exact reading this counter exists to
	 * prevent: a machine whose interrupt never fires must not look like a
	 * machine whose interrupt fires constantly. */
	rx_wakes++;

	if (rx_work_ready)
		work_schedule(&rx_work);
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

	kprintf("  queue        : %u waiting, %u serviced, %u overflowed, "
		"%u woken by a card\n",
		rx_queued, (unsigned)rx_serviced, (unsigned)rx_overflow,
		(unsigned)rx_wakes);

	/* Said only when there were any, and said differently depending on
	 * whether any were real. A refusal the test asked for is not news; one
	 * it did not is two cards claiming one name, which is NW-002 coming
	 * back. */
	if (interrupt_capable || interrupt_refused)
		kprintf("  interrupts   : %u card(s) can raise one, %u cannot "
			"and are polled\n",
			interrupt_capable, interrupt_refused);

	if (name_refusals) {
		kprintf("  names        : %u refused as already taken",
			(unsigned)name_refusals);

		/* The two are counted separately rather than one being
		 * inferred from the other. "Every refusal was expected" and
		 * "the expected budget is spent" are different statements, and
		 * they stop agreeing the moment a real collision happens after
		 * the test has run -- which is exactly the case this line
		 * exists to report. */
		if (suppressed_refusals == name_refusals)
			kputs(" -- every one the self-test proving a "
			      "duplicate is refused\n");
		else
			kprintf(", %u of them a real collision\n",
				(unsigned)(name_refusals -
					   suppressed_refusals));
	}
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
		unsigned pushed = 0;

		/* Nothing may drain while this is being filled. See KF-201:
		 * without it this asserted an overflow the drain had already
		 * made impossible, and said the bound was broken. */
		rx_hold_drain(true);

		for (i = 0; i < RX_QUEUE_MAX + 8; i++) {
			struct netbuf *b = netbuf_alloc();

			if (!b)
				break;

			netbuf_put(b, 4);
			netdev_receive(d, b);
			pushed++;
		}

		/* Say so rather than fail as though the queue misbehaved. A
		 * flood that was never built has tested nothing, and that is a
		 * different sentence from "the bound does not hold". */
		if (pushed <= RX_QUEUE_MAX) {
			kprintf("net: only %u of %u buffers -- the queue was "
				"never filled, so the drop path was not "
				"reached\n", pushed, RX_QUEUE_MAX + 8);
			ok = false;
		}

		if (rx_queued > RX_QUEUE_MAX) {
			kprintf("net: queue grew to %u, past its bound of %u\n",
				rx_queued, RX_QUEUE_MAX);
			ok = false;
		}

		if (rx_overflow == dropped_before) {
			kprintf("net: %u frames past a bound of %u and not one "
				"was dropped\n", pushed, RX_QUEUE_MAX);
			ok = false;
		}

		/* Before the drain below, which goes through rx_take. */
		rx_hold_drain(false);

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
