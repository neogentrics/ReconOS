/* What the two card drivers can be asked on a machine with neither card.
 *
 * `r8169.c` and `e1000.c` both talk to registers that are not in the
 * verification rig -- one because QEMU emulates no Realtek gigabit part at
 * all, the other because the emulated Intel is only present when the rig is
 * told to add one. So the drivers themselves are proved by booting with the
 * card attached, and that is a different run from this one.
 *
 * What is proved *here* is the part of them that is not silicon: the two
 * pieces of the device interface that had to be built before a second driver
 * could exist at all, and that nothing before now exercised. Both are in
 * `netdev.c`, both are used by both drivers, and both are the kind of thing
 * that is invisible until there are two of something:
 *
 *   NW-001  a driver's interrupt had no way to ask for the receive queue to
 *           be drained, so an interrupt-driven card could not be collected
 *           from at all
 *   NW-002  every driver named its own cards from zero, so the second driver
 *           on a machine produces a second `eth0`
 *
 * --- Why these are asserted and the descriptor arithmetic is not ---
 *
 * The obvious other thing to check is the length arithmetic, which is the
 * difference between the two drivers that would be easiest to get wrong: the
 * Realtek's receive length includes the four-byte frame check sequence and the
 * Intel's does not, because the Intel is told to strip it.
 *
 * It is not asserted *in this file*. Each driver asserts it in its own, beside
 * the loop it is about, driving that loop against a page of memory standing in
 * for the register window -- `r8169_self_test` and `e1000_self_test`. They are
 * there rather than here because the alternative was lifting the arithmetic
 * out into something this file could call, and then the thing under test would
 * be the lifted copy while the loop went on being the thing that runs. A test
 * that proves a copy of the code is the shape of fault this project keeps
 * finding.
 *
 * **And the two assert opposite things, which is why neither could be
 * skipped.** The Realtek's length includes the four-byte frame check sequence
 * and the Intel's does not, because the Intel is told to strip it. So one test
 * insists four bytes come off and the other insists nothing does. Both are
 * right; there is nothing in a frame to say which card you are holding; and
 * the failure worth guarding against is somebody tidying one line into the
 * other file, which reads as consistency and is a four-byte error.
 *
 * **Why booting was not enough**, measured rather than assumed: breaking the
 * Intel's receive length on purpose, in each direction --
 *
 *   four bytes too short   no DHCP lease and no ping reply
 *   four bytes too long    a lease and a ping reply, indistinguishable
 *                          from correct
 *
 * Too short fails UDP's checksum and the DHCP reply is thrown away. Too long
 * is invisible: every layer reads its own length and ignores the tail. And
 * **too long is the direction a driver errs in**, because it is what
 * forgetting the subtraction looks like -- so booting catches the mistake
 * nobody makes and misses the one they do.
 *
 * Thirteen faults were introduced across the two tests and all thirteen are
 * caught. **Twelve of those thirteen kernels still took a DHCP lease and
 * answered a ping**, which is the measurement that says the tests reach ground
 * a running machine cannot. NW-007 is closed on that basis; what remains
 * uncovered is the *transmit* length, and it is named in that entry rather
 * than implied here.
 */
#include <recon/kernel/net.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/work.h>

/* A device that exists to be counted, in the manner `net.c`'s capture device
 * already establishes: registered, measured, and taken straight back out so
 * that it never appears in a boot summary beside a real card. */
static unsigned polls;

static void test_poll(struct net_device *dev)
{
	(void)dev;
	polls++;
}

static bool test_transmit(struct net_device *dev, struct netbuf *b)
{
	(void)dev;
	netbuf_free(b);
	return true;
}

static const struct net_device_ops test_ops = {
	.transmit = test_transmit,
	.enable_interrupts = NULL,
	.poll = test_poll,
};

/* --- NW-002: one name per card, issued by the thing that can see them all -- */

static bool names_are_unique(void)
{
	static const struct mac_addr mac = { { 0x02, 0x4E, 0x57, 0, 0, 1 } };
	struct net_device *d;
	char first[NET_NAME_MAX];
	char second[NET_NAME_MAX];
	bool ok = true;

	if (!netdev_name("eth", first, sizeof(first))) {
		kputs("  nic: no name was available for the first device\n");
		return false;
	}

	d = netdev_register(first, &test_ops, NULL, &mac);

	if (!d) {
		kprintf("  nic: %s was refused, and nothing holds it\n", first);
		return false;
	}

	/* The second ask must not return the first answer. This is the whole
	 * of NW-002: a driver counting its own cards would hand out the same
	 * string here, because from inside a driver there is one card. */
	if (!netdev_name("eth", second, sizeof(second))) {
		kputs("  nic: no second name was available\n");
		ok = false;
	} else if (kstrlen(first) == kstrlen(second) &&
		   kmemcmp(first, second, kstrlen(first)) == 0) {
		kprintf("  nic: asked twice for a name and got %s both "
			"times\n", first);
		ok = false;
	}

	/* And the other half: a name already in use is refused rather than
	 * quietly duplicated. Without this the machine ends up with two
	 * devices answering to one name and a summary that prints it twice. */
	netdev_note_expected_refusal();

	if (netdev_register(first, &test_ops, NULL, &mac)) {
		kprintf("  nic: %s was registered twice\n", first);

		/* Two were made, so two are taken back. */
		netdev_forget_last();
		ok = false;
	}

	netdev_forget_last();

	/* And the name is free again once the device is gone -- otherwise the
	 * allocator leaks a name per test on every boot. */
	if (netdev_by_name(first)) {
		kprintf("  nic: %s still answers after being forgotten\n",
			first);
		ok = false;
	}

	return ok;
}

/* --- NW-001: a driver can ask for the receive queue to be drained ---------- */

static bool wake_reaches_the_worker(void)
{
	static const struct mac_addr mac = { { 0x02, 0x4E, 0x57, 0, 0, 2 } };
	struct net_device *d;
	char name[NET_NAME_MAX];
	unsigned before;
	bool ok = true;

	if (!netdev_name("eth", name, sizeof(name)))
		return false;

	d = netdev_register(name, &test_ops, NULL, &mac);

	if (!d) {
		kputs("  nic: could not register a device to be woken\n");
		return false;
	}

	/* Everything already pending is run off first, so that what is measured
	 * below is what *this* call caused rather than what the boot happened
	 * to leave queued.
	 *
	 * This line is here because the test without it passed with
	 * `netdev_wake` gutted -- measured, by gutting it, not reasoned about.
	 * `work_drain` waits for everything queued before the call, and at this
	 * point in the boot the receive drain is usually already queued from
	 * something else; so the drain ran, `netdev_service` polled, the count
	 * moved, and the test reported that the wake worked while the wake did
	 * nothing at all. A check that cannot fail looks exactly like one that
	 * passes. */
	if (!work_drain()) {
		kputs("  nic: the work queue could not be drained\n");
		netdev_forget_last();
		return false;
	}

	/* And the control, which is the other half of making that true.
	 *
	 * If a drain polls devices with nothing having asked it to, then
	 * something else is feeding the queue and this test cannot attribute
	 * the poll below to the wake. That is reported rather than passed
	 * over: a test that cannot tell what caused what should say so, not
	 * return the answer it hoped for. */
	before = polls;

	if (!work_drain()) {
		kputs("  nic: the work queue could not be drained twice\n");
		netdev_forget_last();
		return false;
	}

	if (polls != before) {
		kputs("  nic: the receive drain is being scheduled by "
		      "something other than this test, so the wake below "
		      "cannot be isolated\n");
		netdev_forget_last();
		return false;
	}

	/* The claim, now isolated: this call, and nothing else, causes the
	 * worker thread to run `netdev_service`, which polls every device. */
	before = polls;
	netdev_wake();

	if (!work_drain()) {
		kputs("  nic: the work queue could not be drained\n");
		ok = false;
	} else if (polls == before) {
		kputs("  nic: netdev_wake did not reach the worker -- an "
		      "interrupt-driven card cannot be collected from\n");
		ok = false;
	}

	netdev_forget_last();
	return ok;
}

/* --- NW-008: the hook that nothing called ---------------------------------
 *
 * `enable_interrupts` was declared in `net_device_ops`, documented in a
 * paragraph explaining when a driver should implement it, and **called by
 * nothing**. There was no `ops->enable_interrupts` anywhere in the kernel. All
 * three implementations were null, so a missing call site and a correctly
 * skipped optional hook looked identical from every angle except grepping for
 * the call — which is what found it.
 *
 * It is called by `netdev_register` now, and this is what says so. The device
 * below is the only implementation in the tree that returns **true**, which
 * matters: both real drivers return false on a machine with no vector to give
 * them, so on the verification rig a test that accepted false as proof would be
 * proving nothing.
 */
static unsigned enables;
static bool enable_answer;
static struct net_device *enabled_with;
static bool enabled_while_registered;

static bool test_enable_interrupts(struct net_device *dev)
{
	enables++;
	enabled_with = dev;

	/* Asked **here**, inside the call, because that is the only moment the
	 * question means anything. The first version of this test asked after
	 * `netdev_register` had returned, where the device is always in the
	 * table -- an assertion that could not fail, written into a file whose
	 * subject is assertions that cannot fail. */
	enabled_while_registered = netdev_by_name(dev->name) == dev;

	return enable_answer;
}

static const struct net_device_ops enabling_ops = {
	.transmit = test_transmit,
	.enable_interrupts = test_enable_interrupts,
	.poll = test_poll,
};

static bool the_enable_hook_is_called(void)
{
	static const struct mac_addr mac = { { 0x02, 0x4E, 0x57, 0, 0, 4 } };
	struct net_device *d;
	char name[NET_NAME_MAX];
	unsigned before = enables;
	bool ok = true;

	if (!netdev_name("eth", name, sizeof(name)))
		return false;

	enable_answer = true;
	enabled_with = NULL;
	enabled_while_registered = false;

	d = netdev_register(name, &enabling_ops, NULL, &mac);

	if (!d) {
		kputs("  nic: could not register a device to enable\n");
		return false;
	}

	if (enables == before) {
		kputs("  nic: enable_interrupts was never called -- a driver "
		      "that implements it would sit there never being asked\n");
		ok = false;
	}

	/* The device it was handed, not merely that something was called. A
	 * hook invoked with the wrong device is a driver arming a card that is
	 * not its own. */
	if (enabled_with != d) {
		kputs("  nic: enable_interrupts was called with a different "
		      "device than the one being registered\n");
		ok = false;
	}

	/* And it must be called **after** the device is in the table. The whole
	 * reason the hook exists rather than each driver arming its own mask is
	 * that an interrupt arriving the instant the mask opens asks the worker
	 * thread to poll every registered device -- so the card had better be
	 * on that list already. */
	if (!enabled_while_registered) {
		kputs("  nic: enable_interrupts ran before the device was in "
		      "the table, so an interrupt could arrive for a card the "
		      "stack cannot find\n");
		ok = false;
	}

	netdev_forget_last();
	return ok;
}

/* --- NW-003: a card with no cable is not a route ---------------------------
 *
 * `net_device.link` was carried for as long as this stack has existed and read
 * by nothing. It could not be read usefully, either: the only driver that set
 * it set it to a constant `true`, which is the honest value for a card with no
 * cable to have an opinion about. **An interface with one implementation cannot
 * disagree with itself**, and this field is the smallest possible example of
 * that -- one boolean, written once, meaning nothing.
 *
 * It means something now: `netdev_route` skips a device whose cable is out.
 * What that is worth is a machine with two cards sending out of the live one
 * instead of the dead one, which is the whole reason the server has two.
 */
static bool a_dead_cable_is_not_a_route(void)
{
	static const struct mac_addr mac = { { 0x02, 0x4E, 0x57, 0, 0, 3 } };
	struct net_device *d;
	char name[NET_NAME_MAX];
	ipv4_addr hop = 0;
	bool ok = true;

	if (!netdev_name("eth", name, sizeof(name)))
		return false;

	d = netdev_register(name, &test_ops, NULL, &mac);

	if (!d) {
		kputs("  nic: could not register a device to unplug\n");
		return false;
	}

	/* On its own subnet, so routing has every reason to choose it. */
	d->ip = IPV4(10, 90, 0, 2);
	d->netmask = IPV4(255, 255, 255, 0);

	if (netdev_route(IPV4(10, 90, 0, 9), &hop) != d) {
		kputs("  nic: a device on the destination's own subnet was not "
		      "chosen, so this test cannot say anything about the "
		      "cable\n");
		ok = false;
	}

	/* And now the cable comes out. Nothing else changes -- the device is
	 * still up, still has an address, still owns the subnet. */
	d->link = false;

	if (netdev_route(IPV4(10, 90, 0, 9), &hop) == d) {
		kputs("  nic: a card with no cable was still chosen to send "
		      "through -- every frame would be counted as sent and "
		      "dropped on the floor\n");
		ok = false;
	}

	/* A broadcast too, which takes its own path through the router and had
	 * its own copy of the check. DHCP is the caller that matters: asking for
	 * an address down a cable that is not there is how a machine with two
	 * cards ends up with none.
	 *
	 * **Every other device is put down for the length of this.** The
	 * broadcast path returns the *first* device that qualifies, and this one
	 * registered last -- so on a machine with a real card the answer is that
	 * card whatever this test's device does, and the assertion passes
	 * without ever consulting the thing it is about.
	 *
	 * That is not a hypothetical. The first version of this did not do it,
	 * and the broadcast half of the check was removed on purpose and stayed
	 * green. A check that cannot fail looks exactly like one that passes,
	 * and it looked that way sitting directly underneath a comment saying
	 * so. */
	{
		bool was_up[NET_MAX_DEVICES];
		unsigned i, n = netdev_count();

		for (i = 0; i < n; i++) {
			struct net_device *o = netdev_at(i);

			was_up[i] = o->up;
			if (o != d)
				o->up = false;
		}

		if (netdev_route(IPV4_BROADCAST, &hop)) {
			kputs("  nic: a broadcast was routed out of a card "
			      "with no cable, and it was the only card\n");
			ok = false;
		}

		/* And the control: with the cable in, this device *is* the
		 * answer. Without this the assertion above would also pass on a
		 * machine where broadcasts route nowhere at all. */
		d->link = true;

		if (netdev_route(IPV4_BROADCAST, &hop) != d) {
			kputs("  nic: the only card on the machine, with a "
			      "cable in it, was not chosen for a broadcast\n");
			ok = false;
		}

		d->link = false;

		for (i = 0; i < n; i++)
			netdev_at(i)->up = was_up[i];
	}

	d->link = true;

	if (netdev_route(IPV4(10, 90, 0, 9), &hop) != d) {
		kputs("  nic: plugging the cable back in did not make the "
		      "card routable again\n");
		ok = false;
	}

	netdev_forget_last();
	return ok;
}

/* NW-010: the address this machine gives out is one somebody can reach.
 *
 * `logport.c` walked the device table itself, asking `up && ip` and not
 * `link`, because there was no `netdev_primary` to call -- its comment says
 * so. Routing asks all three. So on a machine with a card that is up and
 * addressed with the cable out, the two disagreed, and the one that picks the
 * address to advertise was the one that did not care about the wire.
 *
 * **What that is worth saying honestly: the divergence was latent, not live.**
 * A cableless card does not normally hold an address, because NW-003 made
 * `net_bring_up` skip DHCP when there is no cable -- so `ip` was doing
 * `link`'s job by coincidence, in a different file. The ways it stops being a
 * coincidence are a static address, and a cable pulled after the lease: both
 * drivers re-read the cable on every poll, so `link` goes false under a live
 * address and nothing else changes. This test builds the second case directly
 * rather than waiting for it. */
static bool the_primary_device_has_a_cable(void)
{
	static const struct mac_addr mac = { { 0x02, 0x4E, 0x57, 0, 0, 4 } };
	struct net_device *d;
	char name[NET_NAME_MAX];
	bool was_up[NET_MAX_DEVICES];
	unsigned i, n;
	bool ok = true;

	if (!netdev_name("eth", name, sizeof(name)))
		return false;

	d = netdev_register(name, &test_ops, NULL, &mac);

	if (!d) {
		kputs("  nic: could not register a device to unplug\n");
		return false;
	}

	d->ip = IPV4(10, 90, 1, 2);
	d->netmask = IPV4(255, 255, 255, 0);

	/* **Every other device goes down for the length of this.**
	 * `netdev_primary` returns the *first* device that qualifies and this
	 * one registered last, so on a machine with a real card the answer is
	 * that card whatever this device does -- and the assertion would pass
	 * without ever consulting the thing it is about. That is the same trap
	 * the broadcast half of `a_dead_cable_is_not_a_route` fell into, which
	 * is how it was seen here before it was written. */
	n = netdev_count();

	for (i = 0; i < n; i++) {
		struct net_device *o = netdev_at(i);

		was_up[i] = o->up;
		if (o != d)
			o->up = false;
	}

	/* The control first, and it is not optional: without it, an assertion
	 * that a cableless card is not chosen also passes on a machine where
	 * nothing is ever chosen. */
	if (netdev_primary() != d) {
		kputs("  nic: a card that is up, addressed and plugged in was "
		      "not offered as this machine's address, so this test "
		      "cannot say anything about the cable\n");
		ok = false;
	}

	/* And now the cable comes out. The address does not go with it: that is
	 * the whole case, and it is what a lease outliving a cable looks like. */
	d->link = false;

	if (netdev_primary() == d) {
		kputs("  nic: a card with no cable was offered as this "
		      "machine's address -- the log port would listen on an "
		      "address nothing can reach, which is the one case it "
		      "exists for\n");
		ok = false;
	}

	d->link = true;

	if (netdev_primary() != d) {
		kputs("  nic: plugging the cable back in did not make the card "
		      "addressable again\n");
		ok = false;
	}

	for (i = 0; i < n; i++)
		netdev_at(i)->up = was_up[i];

	netdev_forget_last();
	return ok;
}

bool nic_self_test(void)
{
	bool ok = true;

	if (!names_are_unique())
		ok = false;

	if (!wake_reaches_the_worker())
		ok = false;

	if (!a_dead_cable_is_not_a_route())
		ok = false;

	if (!the_enable_hook_is_called())
		ok = false;

	if (!the_primary_device_has_a_cable())
		ok = false;

	/* And the Realtek's receive loop, which has no other way to be run at
	 * all: the rig emulates no card it recognises. */
	if (!r8169_self_test())
		ok = false;

	/* And the Intel's, which asserts the opposite arithmetic. */
	if (!e1000_self_test())
		ok = false;

	return ok;
}
