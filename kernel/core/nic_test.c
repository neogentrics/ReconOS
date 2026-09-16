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
 * It is not asserted here and that is deliberate rather than an oversight. To
 * test it without a card, the arithmetic would have to be lifted out of the
 * receive loop into a function this file could call -- and then the thing
 * under test would be the lifted function rather than the loop, while the loop
 * went on being the thing that runs. A test that proves a copy of the code is
 * the shape of fault this project keeps finding.
 *
 * So it is checked by booting with the card. **And that only half works**,
 * which is the part worth writing down, because it was measured rather than
 * assumed. Breaking the Intel's receive length on purpose, in each direction,
 * and booting each one:
 *
 *   four bytes too short   no DHCP lease and no ping reply
 *   four bytes too long    a lease and a ping reply, indistinguishable
 *                          from correct
 *
 * Too short fails UDP's checksum and the DHCP reply is thrown away. Too long
 * is invisible: every layer reads its own length and ignores the tail.
 *
 * And **too long is the direction a driver actually errs in**, because it is
 * what forgetting the subtraction looks like. So the boot test catches the
 * mistake nobody makes and misses the one they do. That is a real hole with no
 * check in front of it, recorded here rather than left implied -- NW-007.
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

bool nic_self_test(void)
{
	bool ok = true;

	if (!names_are_unique())
		ok = false;

	if (!wake_reaches_the_worker())
		ok = false;

	return ok;
}
