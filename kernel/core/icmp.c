/* ICMP: the protocol that answers "is anything there".
 *
 * ICMP is not a transport. Nothing is carried over it; it exists so that the
 * network can say something about itself -- a host is unreachable, a packet
 * outlived its hop count, or, in the only case anybody uses by hand, that a
 * machine is there and answering.
 *
 * Echo is built and the error messages are not. That is deliberate rather than
 * partial: sending "destination unreachable" correctly means knowing which of
 * six reasons applies and quoting the packet that caused it, and nothing in
 * this kernel yet makes a decision based on receiving one. A reply this kernel
 * cannot act on is a reply it should not be generating either.
 */
#include <recon/kernel/net.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/console.h>
#include <recon/kernel/time.h>

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

#define ICMP_HDR_LEN 8

/* Outstanding echoes. Small, because this is a diagnostic rather than a
 * transport, and bounded for the same reason everything else here is. */
#define ECHO_MAX 8

struct echo {
	u16 id;
	u16 seq;
	ipv4_addr dst;
	u64 sent_ns;
	u64 replied_ns;
	bool used;
	bool answered;
};

static struct echo echoes[ECHO_MAX];
static u16 next_id = 1;

static u64 requests_sent, replies_sent, replies_received;
static u64 rx_short, rx_bad_checksum, rx_unknown_type;

u16 icmp_echo_send(ipv4_addr dst)
{
	struct netbuf *b;
	struct echo *e = NULL;
	unsigned i;
	u8 *h;
	u16 id;

	for (i = 0; i < ECHO_MAX; i++) {
		if (!echoes[i].used) {
			e = &echoes[i];
			break;
		}
	}

	/* Nothing free: reuse the oldest rather than refusing, because an echo
	 * nobody ever answered would otherwise hold its slot for ever and the
	 * diagnostic would stop working after eight unreachable addresses. */
	if (!e) {
		u64 oldest = ~0ull;

		for (i = 0; i < ECHO_MAX; i++) {
			if (echoes[i].sent_ns < oldest) {
				oldest = echoes[i].sent_ns;
				e = &echoes[i];
			}
		}
	}

	b = netbuf_alloc();

	if (!b)
		return 0;

	id = next_id++;

	if (!next_id)
		next_id = 1;	/* zero means "could not send" */

	h = netbuf_put(b, ICMP_HDR_LEN + 32);

	if (!h) {
		netbuf_free(b);
		return 0;
	}

	h[0] = ICMP_ECHO_REQUEST;
	h[1] = 0;
	net_put16(h + 2, 0);		/* checksum, below */
	net_put16(h + 4, id);
	net_put16(h + 6, 1);		/* sequence */

	/* A recognisable payload, so that a reply carrying something else is
	 * visible as a reply carrying something else. */
	for (i = 0; i < 32; i++)
		h[ICMP_HDR_LEN + i] = (u8)('a' + (i % 26));

	/* ICMP's checksum covers the header *and* the payload, unlike IPv4's,
	 * which covers only its own header. Getting that span wrong produces a
	 * packet every receiver silently drops. */
	net_put16(h + 2, net_checksum(h, ICMP_HDR_LEN + 32));

	e->id = id;
	e->seq = 1;
	e->dst = dst;
	e->sent_ns = time_monotonic_ns();
	e->replied_ns = 0;
	e->used = true;
	e->answered = false;

	requests_sent++;

	if (!ip_transmit(dst, IP_PROTO_ICMP, b)) {
		e->used = false;
		return 0;
	}

	return id;
}

bool icmp_echo_seen(u16 id, u64 *rtt_ns)
{
	unsigned i;

	for (i = 0; i < ECHO_MAX; i++) {
		if (echoes[i].used && echoes[i].id == id) {
			if (!echoes[i].answered)
				return false;

			if (rtt_ns)
				*rtt_ns = echoes[i].replied_ns -
					  echoes[i].sent_ns;
			return true;
		}
	}

	return false;
}

void icmp_receive(struct net_device *dev, struct netbuf *b)
{
	u8 *h = b->data;
	u8 type;
	u16 id;
	unsigned i;

	if (b->len < ICMP_HDR_LEN) {
		rx_short++;
		netbuf_free(b);
		return;
	}

	/* Checked before anything is believed, and over the whole message. */
	if (net_checksum(h, b->len) != 0) {
		rx_bad_checksum++;
		netbuf_free(b);
		return;
	}

	type = h[0];
	id = net_get16(h + 4);

	if (type == ICMP_ECHO_REPLY) {
		replies_received++;

		for (i = 0; i < ECHO_MAX; i++) {
			if (echoes[i].used && echoes[i].id == id) {
				echoes[i].answered = true;
				echoes[i].replied_ns = time_monotonic_ns();
				break;
			}
		}

		netbuf_free(b);
		return;
	}

	if (type == ICMP_ECHO_REQUEST) {
		/* Answered by turning the request into a reply in place: the
		 * payload must come back unchanged, and copying it into a new
		 * buffer to send it back unchanged is work with no purpose.
		 *
		 * Only the type byte changes, and the checksum is adjusted
		 * rather than recomputed -- the type went from 8 to 0, so the
		 * sum lost 0x0800, and adding it back to the complemented
		 * value is the whole correction. Recomputing over the payload
		 * would also be correct and would be slower for no reason. */
		u16 sum = net_get16(h + 2);
		u32 adjusted = (u32)(~sum & 0xFFFF);

		h[0] = ICMP_ECHO_REPLY;

		if (adjusted < 0x0800)
			adjusted += 0xFFFF;	/* borrow */

		adjusted -= 0x0800;
		net_put16(h + 2, (u16)(~adjusted & 0xFFFF));

		replies_sent++;

		/* Back to whoever asked. The source of the request becomes the
		 * destination of the reply; using the device's own notion of a
		 * route is what makes this work when the asker is elsewhere. */
		{
			ipv4_addr to = b->src_ip;

			(void)dev;
			ip_transmit(to, IP_PROTO_ICMP, b);
		}
		return;
	}

	rx_unknown_type++;
	netbuf_free(b);
}

void icmp_print_summary(void)
{
	kprintf("  icmp         : %u asked, %u answered, %u replies in\n",
		(unsigned)requests_sent, (unsigned)replies_sent,
		(unsigned)replies_received);

	if (rx_short || rx_bad_checksum || rx_unknown_type)
		kprintf("               : %u short, %u bad checksum, "
			"%u unhandled type\n",
			(unsigned)rx_short, (unsigned)rx_bad_checksum,
			(unsigned)rx_unknown_type);
}

/* Exercised from net_self_test, which has a device to send on. */
bool icmp_reply_in_place_test(void)
{
	u8 msg[ICMP_HDR_LEN + 8];
	u16 original, adjusted;
	unsigned i;

	kmemset(msg, 0, sizeof(msg));
	msg[0] = ICMP_ECHO_REQUEST;
	net_put16(msg + 4, 0x1234);
	net_put16(msg + 6, 7);

	for (i = 0; i < 8; i++)
		msg[ICMP_HDR_LEN + i] = (u8)(0x40 + i);

	net_put16(msg + 2, net_checksum(msg, sizeof(msg)));
	original = net_get16(msg + 2);

	/* Apply the in-place adjustment the receive path uses. */
	{
		u32 sum = (u32)(~original & 0xFFFF);

		msg[0] = ICMP_ECHO_REPLY;

		if (sum < 0x0800)
			sum += 0xFFFF;

		sum -= 0x0800;
		net_put16(msg + 2, (u16)(~sum & 0xFFFF));
	}

	adjusted = net_get16(msg + 2);

	/* The claim: the adjusted checksum equals what a full recomputation
	 * would produce. An adjustment that is merely self-consistent passes a
	 * test that checks it against itself, so it is checked against the
	 * function that does the work the long way. */
	{
		u8 again[ICMP_HDR_LEN + 8];

		kmemcpy(again, msg, sizeof(again));
		net_put16(again + 2, 0);

		if (net_checksum(again, sizeof(again)) != adjusted) {
			kprintf("icmp: the adjusted checksum %u differs from "
				"a full recomputation\n", (unsigned)adjusted);
			return false;
		}
	}

	/* And the finished message checks out, which is what a receiver does. */
	if (net_checksum(msg, sizeof(msg)) != 0) {
		kprintf("icmp: the adjusted reply does not check out\n");
		return false;
	}

	return true;
}
