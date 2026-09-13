/* DHCP: how a card stops being a card and becomes a machine on a network.
 *
 * Everything below this file works without an address. Nothing *above* it does.
 * A stack with no address can build a frame, checksum it and hand it to a card,
 * and every one of those steps can be correct while the machine is unreachable
 * and unable to reach anything -- which is why this is here rather than left as
 * a thing somebody types in.
 *
 * --- The four messages ---
 *
 * DISCOVER, OFFER, REQUEST, ACK. The shape looks redundant -- the server has
 * already chosen an address by the time it offers one -- and it is not: **more
 * than one server may answer**. The client picks one offer and names it in the
 * REQUEST, which is how the servers it did not pick learn to release what they
 * had set aside. A two-message exchange works perfectly on a network with one
 * server and leaks addresses on a network with two.
 *
 * --- Why this is broadcast, and the chicken-and-egg it solves ---
 *
 * The client has no address, so it cannot be the source of a normal packet and
 * cannot be the destination of a normal reply. So DISCOVER goes from 0.0.0.0 to
 * 255.255.255.255 with the hardware broadcast address, and the server answers
 * the same way. The *hardware* address in the payload is what lets each client
 * recognise its own reply, which is why it is carried in the message body and
 * not merely in the frame.
 *
 * The transaction identifier does the same job a second time, and is random for
 * the same reason TCP's initial sequence number is: it is the thing an attacker
 * would have to guess to answer on a real server's behalf.
 *
 * --- What is deliberately absent ---
 *
 * **No lease renewal.** An address is taken and kept for as long as the machine
 * runs. A lease that expires while the machine is up would need a timer and a
 * re-request, and nothing here runs long enough yet for it to matter. Written
 * down because the failure -- a machine that silently loses its address after
 * an hour -- is one nobody would look for here.
 *
 * **No DECLINE, no RELEASE.** Both are politeness to the server rather than
 * correctness for the client.
 */
#include <recon/kernel/net.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/console.h>
#include <recon/kernel/time.h>
#include <recon/kernel/random.h>

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

#define DHCP_OP_REQUEST 1
#define DHCP_OP_REPLY   2

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6

#define OPT_SUBNET_MASK   1
#define OPT_ROUTER        3
#define OPT_DNS           6
#define OPT_REQUESTED_IP  50
#define OPT_LEASE_TIME    51
#define OPT_MESSAGE_TYPE  53
#define OPT_SERVER_ID     54
#define OPT_PARAM_REQUEST 55
#define OPT_END           255

/* The fixed part of the message, before the options. */
#define DHCP_FIXED_LEN 240

/* The four bytes that say the options are DHCP's rather than the older BOOTP
 * vendor field. A message without them is BOOTP and is ignored. */
static const u8 MAGIC[4] = { 99, 130, 83, 99 };

struct dhcp_result {
	bool have_offer;
	bool have_ack;
	ipv4_addr offered;
	ipv4_addr server;
	ipv4_addr netmask;
	ipv4_addr router;
	ipv4_addr dns;
	u32 lease_seconds;
};

static struct dhcp_result result;
static u32 transaction;
static struct net_device *configuring;

static u64 sent, received, ignored;

/* Walks the option list. Returns the value and its length, or null.
 *
 * Bounds-checked at every step, because this is parsing a message from a
 * machine that has not authenticated itself, over broadcast, before this
 * machine has an address. It is the most exposed parser in the kernel. */
static const u8 *find_option(const u8 *msg, u32 len, u8 want, u8 *out_len)
{
	u32 i = DHCP_FIXED_LEN;

	if (len < DHCP_FIXED_LEN)
		return NULL;

	while (i < len) {
		u8 code = msg[i];
		u8 olen;

		if (code == OPT_END)
			return NULL;

		if (code == 0) {	/* pad */
			i++;
			continue;
		}

		if (i + 1 >= len)
			return NULL;

		olen = msg[i + 1];

		if (i + 2 + olen > len)
			return NULL;

		if (code == want) {
			if (out_len)
				*out_len = olen;
			return msg + i + 2;
		}

		i += 2 + olen;
	}

	return NULL;
}

static u32 build(u8 *msg, struct net_device *dev, u8 type,
		 ipv4_addr requested, ipv4_addr server)
{
	u32 at;

	kmemset(msg, 0, DHCP_FIXED_LEN);

	msg[0] = DHCP_OP_REQUEST;
	msg[1] = 1;			/* ethernet */
	msg[2] = MAC_LEN;
	msg[3] = 0;			/* hops */
	net_put32(msg + 4, transaction);
	net_put16(msg + 10, 0x8000);	/* ask for the reply by broadcast */

	kmemcpy(msg + 28, dev->mac.b, MAC_LEN);
	kmemcpy(msg + 236, MAGIC, 4);

	at = DHCP_FIXED_LEN;

	msg[at++] = OPT_MESSAGE_TYPE;
	msg[at++] = 1;
	msg[at++] = type;

	if (requested) {
		msg[at++] = OPT_REQUESTED_IP;
		msg[at++] = 4;
		net_put32(msg + at, requested);
		at += 4;
	}

	if (server) {
		msg[at++] = OPT_SERVER_ID;
		msg[at++] = 4;
		net_put32(msg + at, server);
		at += 4;
	}

	/* What this machine wants told. Asking is not demanding: a server may
	 * answer with none of them, which is why every one is checked for
	 * before it is used. */
	msg[at++] = OPT_PARAM_REQUEST;
	msg[at++] = 3;
	msg[at++] = OPT_SUBNET_MASK;
	msg[at++] = OPT_ROUTER;
	msg[at++] = OPT_DNS;

	msg[at++] = OPT_END;

	return at;
}

static void arrived(void *ctx, struct netbuf *b)
{
	const u8 *msg = b->data;
	const u8 *opt;
	u8 olen = 0;
	u8 type;

	(void)ctx;

	if (b->len < DHCP_FIXED_LEN + 4) {
		ignored++;
		netbuf_free(b);
		return;
	}

	/* Ours, and DHCP rather than BOOTP, and a reply rather than an echo of
	 * our own broadcast -- which a machine does receive, because it sent it
	 * to an address that includes itself. */
	if (msg[0] != DHCP_OP_REPLY ||
	    net_get32(msg + 4) != transaction ||
	    kmemcmp(msg + 236, MAGIC, 4) != 0) {
		ignored++;
		netbuf_free(b);
		return;
	}

	if (configuring &&
	    kmemcmp(msg + 28, configuring->mac.b, MAC_LEN) != 0) {
		/* Somebody else's reply, on a broadcast everybody hears. */
		ignored++;
		netbuf_free(b);
		return;
	}

	opt = find_option(msg, b->len, OPT_MESSAGE_TYPE, &olen);

	if (!opt || olen != 1) {
		ignored++;
		netbuf_free(b);
		return;
	}

	type = opt[0];
	received++;

	if (type == DHCP_OFFER) {
		result.offered = net_get32(msg + 16);	/* yiaddr */
		result.have_offer = true;

		opt = find_option(msg, b->len, OPT_SERVER_ID, &olen);

		if (opt && olen == 4)
			result.server = net_get32(opt);
	} else if (type == DHCP_ACK) {
		result.offered = net_get32(msg + 16);
		result.have_ack = true;

		opt = find_option(msg, b->len, OPT_SUBNET_MASK, &olen);
		if (opt && olen == 4)
			result.netmask = net_get32(opt);

		opt = find_option(msg, b->len, OPT_ROUTER, &olen);
		if (opt && olen >= 4)
			result.router = net_get32(opt);

		opt = find_option(msg, b->len, OPT_DNS, &olen);
		if (opt && olen >= 4)
			result.dns = net_get32(opt);

		opt = find_option(msg, b->len, OPT_LEASE_TIME, &olen);
		if (opt && olen == 4)
			result.lease_seconds = net_get32(opt);
	}

	netbuf_free(b);
}

/* Runs the stack for up to `ms`, or until `done` becomes true. */
static bool wait_for(const bool *done, u32 ms)
{
	u64 until = time_monotonic_ns() + (u64)ms * 1000000ull;

	while (time_monotonic_ns() < until) {
		netdev_service();
		ip_flush_pending();

		if (*done)
			return true;
	}

	return *done;
}

bool dhcp_configure(struct net_device *dev)
{
	u8 msg[400];
	u32 len;
	unsigned attempt;

	if (!dev)
		return false;

	kmemset(&result, 0, sizeof(result));
	configuring = dev;

	if (!random_bytes(&transaction, sizeof(transaction)))
		transaction = (u32)time_monotonic_ns();

	if (!udp_bind_port(DHCP_CLIENT_PORT, arrived, NULL))
		return false;

	/* The address has to be zero while this runs. A machine that answers
	 * with an address it has not been given yet is answering for somebody
	 * else's, and `ip_transmit` would use it as the source. */
	dev->ip = IPV4_ANY;
	dev->netmask = IPV4_ANY;

	for (attempt = 0; attempt < 3 && !result.have_offer; attempt++) {
		len = build(msg, dev, DHCP_DISCOVER, 0, 0);
		sent++;

		if (!udp_send(IPV4_BROADCAST, DHCP_SERVER_PORT,
			      DHCP_CLIENT_PORT, msg, len))
			break;

		wait_for(&result.have_offer, 400);
	}

	if (!result.have_offer) {
		udp_unbind_port(DHCP_CLIENT_PORT);
		configuring = NULL;
		return false;
	}

	for (attempt = 0; attempt < 3 && !result.have_ack; attempt++) {
		len = build(msg, dev, DHCP_REQUEST, result.offered,
			    result.server);
		sent++;

		if (!udp_send(IPV4_BROADCAST, DHCP_SERVER_PORT,
			      DHCP_CLIENT_PORT, msg, len))
			break;

		wait_for(&result.have_ack, 400);
	}

	udp_unbind_port(DHCP_CLIENT_PORT);
	configuring = NULL;

	if (!result.have_ack)
		return false;

	dev->ip = result.offered;
	dev->netmask = result.netmask ? result.netmask
				      : IPV4(255, 255, 255, 0);
	dev->gateway = result.router;

	return true;
}

void dhcp_print_summary(void)
{
	char a[20], b[20];

	if (!sent) {
		kprintf("  dhcp         : not attempted -- no card to configure\n");
		return;
	}

	kprintf("  dhcp         : %u sent, %u answers, %u not for us\n",
		(unsigned)sent, (unsigned)received, (unsigned)ignored);

	if (result.have_ack) {
		kprintf("               : %s, mask %s",
			ipv4_format(result.offered, a),
			ipv4_format(result.netmask, b));

		if (result.router)
			kprintf(", via %s", ipv4_format(result.router, a));

		kprintf("\n");

		if (result.lease_seconds)
			kprintf("               : lease %u seconds, and it is "
				"**not renewed** -- nothing here runs long "
				"enough yet\n",
				(unsigned)result.lease_seconds);
	} else if (result.have_offer) {
		kprintf("               : offered %s and the request went "
			"unanswered\n", ipv4_format(result.offered, a));
	} else {
		kprintf("               : nothing answered\n");
	}
}
