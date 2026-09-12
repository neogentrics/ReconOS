/* TCP: the state machine, and the three things it is actually for.
 *
 * Everything below this file can lose a packet, duplicate it, reorder it, or
 * deliver it to the wrong conversation, and none of those are faults -- they
 * are what a network does. TCP is the layer that turns that into a stream of
 * bytes that arrive once, in order, or not at all.
 *
 * It does it with three mechanisms and nothing else:
 *
 *   - **Sequence numbers.** Every byte has one. That is what makes a duplicate
 *     recognisable and a gap visible.
 *   - **Acknowledgements.** The receiver says which byte it expects next, so
 *     the sender knows what arrived without the receiver listing it.
 *   - **Retransmission on a timer.** Nothing else recovers a lost segment,
 *     because nothing else knows one was lost.
 *
 * The state machine exists because opening and closing a connection are
 * themselves conversations that can lose packets. The names below are the
 * specification's names, so that any diagram or packet trace of TCP applies to
 * this code directly.
 *
 * --- What is simplified, deliberately, and what it costs ---
 *
 * **Out-of-order segments are dropped rather than queued.** A segment that
 * arrives after a gap is discarded, and the sender retransmits the whole run
 * from the gap onward. This is legal TCP -- the receiver is never obliged to
 * hold what it cannot use -- and it costs throughput on a link that reorders
 * or drops. What it buys is that there is no reassembly queue, and a
 * reassembly queue is where a large share of every stack's bugs and security
 * holes have lived: overlapping segments, a gap that is never filled, and a
 * table of half-streams that anybody on the wire can fill.
 *
 * **No window scaling, no selective acknowledgement, no timestamps.** All
 * three are options negotiated at open, all three matter for throughput on a
 * fast or long link, and none of them change whether the stream is correct.
 *
 * **The retransmission timeout is fixed rather than measured.** A real stack
 * estimates the round trip time and adapts. This one waits a fixed interval,
 * which is too long on a local wire and too short across a continent. It is
 * written down here rather than discovered later.
 *
 * --- The one part that is not a simplification ---
 *
 * **The initial sequence number is random.** It would be simpler to start at
 * zero or at a counter, and either would work perfectly against a cooperative
 * peer. It would also mean anybody who can guess the number can inject data
 * into a connection they cannot see. This is the one place in this file where
 * the easy version is a vulnerability rather than a slowdown, which is why it
 * takes its number from the entropy pool and refuses to run without one.
 */
#include <recon/kernel/net.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/console.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/time.h>
#include <recon/kernel/random.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/vm.h>

#define TCP_MAX_CONNECTIONS 16
#define TCP_BUFFER_SIZE 4096		/* one page each way */
#define TCP_RTO_NS (500ull * 1000000ull)	/* half a second */
#define TCP_MAX_RETRIES 6
#define TCP_TIME_WAIT_NS (2ull * 1000000000ull)

struct tcp_conn {
	bool used;

	enum tcp_state state;

	ipv4_addr local_ip, remote_ip;
	u16 local_port, remote_port;

	/* Send side. `snd_una` is the oldest byte sent and not acknowledged;
	 * `snd_nxt` is the next one to send. The difference is what is in
	 * flight and what a retransmission has to repeat. */
	u32 snd_una, snd_nxt;
	u32 snd_wnd;			/* what the far end will accept */

	/* Receive side. `rcv_nxt` is the sequence number this end expects, and
	 * is exactly what goes in every acknowledgement it sends. */
	u32 rcv_nxt;

	/* Buffers. Circular, and the free space in the receive buffer is what
	 * is advertised as the window -- a stack that advertises a constant
	 * overflows its own buffer under load and blames the network. */
	u8 *tx_buf, *rx_buf;
	paddr_t tx_page, rx_page;
	u32 tx_head, tx_len;
	u32 rx_head, rx_len;

	u64 retransmit_at_ns;
	unsigned retries;
	u64 time_wait_until_ns;

	bool listening;
	bool peer_closed;
	bool reset;

	/* A connection accepted on a listening socket waits here. */
	int accept_of;		/* index of the listener, or -1 */
	bool accept_pending;
};

static struct tcp_conn conns[TCP_MAX_CONNECTIONS];
static struct spinlock tcp_lock;
static bool lock_ready;

static u64 segments_sent, segments_received, retransmits;
static u64 rx_short, rx_bad_checksum, rx_no_connection, rx_out_of_order;
static u64 connections_opened, connections_reset;
static u64 receives_hint;

const char *tcp_state_name(enum tcp_state s)
{
	switch (s) {
	case TCP_CLOSED:       return "closed";
	case TCP_LISTEN:       return "listen";
	case TCP_SYN_SENT:     return "syn-sent";
	case TCP_SYN_RECEIVED: return "syn-received";
	case TCP_ESTABLISHED:  return "established";
	case TCP_FIN_WAIT_1:   return "fin-wait-1";
	case TCP_FIN_WAIT_2:   return "fin-wait-2";
	case TCP_CLOSE_WAIT:   return "close-wait";
	case TCP_CLOSING:      return "closing";
	case TCP_LAST_ACK:     return "last-ack";
	case TCP_TIME_WAIT:    return "time-wait";
	}

	return "?";
}

static void ensure_lock(void)
{
	if (!lock_ready) {
		spin_init(&tcp_lock, "tcp");
		lock_ready = true;
	}
}

/* Sequence numbers wrap at 2^32, so they are compared by *difference* rather
 * than by magnitude. `a < b` is wrong the moment the counter wraps, and the
 * failure is a connection that stalls after four gigabytes -- which is why
 * this is one function rather than a comparison written out at each site. */
static bool seq_lt(u32 a, u32 b)
{
	return (i32)(a - b) < 0;
}

static bool seq_le(u32 a, u32 b)
{
	return (i32)(a - b) <= 0;
}

static u32 pseudo_sum_tcp(ipv4_addr src, ipv4_addr dst, u16 length)
{
	u8 ph[12];

	net_put32(ph + 0, src);
	net_put32(ph + 4, dst);
	ph[8] = 0;
	ph[9] = IP_PROTO_TCP;
	net_put16(ph + 10, length);

	return net_checksum_partial(ph, sizeof(ph), 0);
}

static u32 rx_free(const struct tcp_conn *c)
{
	return TCP_BUFFER_SIZE - c->rx_len;
}

/* Builds and sends one segment. `payload` may be null for a pure ACK. */
static bool send_segment(struct tcp_conn *c, u8 flags, u32 seq,
			 const u8 *payload, u32 len)
{
	struct netbuf *b = netbuf_alloc();
	u8 *h;
	u32 sum;

	if (!b)
		return false;

	if (len) {
		u8 *p = netbuf_put(b, len);

		if (!p) {
			netbuf_free(b);
			return false;
		}

		kmemcpy(p, payload, len);
	}

	h = netbuf_push(b, TCP_HDR_LEN);

	if (!h) {
		netbuf_free(b);
		return false;
	}

	net_put16(h + 0, c->local_port);
	net_put16(h + 2, c->remote_port);
	net_put32(h + 4, seq);
	net_put32(h + 8, c->rcv_nxt);
	h[12] = (TCP_HDR_LEN / 4) << 4;		/* data offset, no options */
	h[13] = flags;
	net_put16(h + 14, (u16)rx_free(c));	/* the real free space */
	net_put16(h + 16, 0);			/* checksum, below */
	net_put16(h + 18, 0);			/* urgent pointer, unused */

	sum = pseudo_sum_tcp(c->local_ip, c->remote_ip,
			     (u16)(TCP_HDR_LEN + len));
	sum = net_checksum_partial(h, TCP_HDR_LEN + len, sum);
	net_put16(h + 16, net_checksum_finish(sum));

	segments_sent++;
	return ip_transmit(c->remote_ip, IP_PROTO_TCP, b);
}

/* A reset for a segment that belongs to no connection. Built without a
 * connection structure, because the whole point is that there isn't one. */
static void send_reset(ipv4_addr src, ipv4_addr dst, u16 sport, u16 dport,
		       u32 seq, u32 ack, bool have_ack)
{
	struct netbuf *b = netbuf_alloc();
	u8 *h;
	u32 sum;

	if (!b)
		return;

	h = netbuf_put(b, TCP_HDR_LEN);

	if (!h) {
		netbuf_free(b);
		return;
	}

	kmemset(h, 0, TCP_HDR_LEN);
	net_put16(h + 0, dport);
	net_put16(h + 2, sport);

	/* A reset answering a segment that carried an acknowledgement uses
	 * that number as its own sequence, so the far end accepts it. One
	 * answering a segment without one acknowledges instead. Getting this
	 * backwards produces a reset the peer ignores, and a connection that
	 * hangs rather than failing. */
	if (have_ack) {
		net_put32(h + 4, ack);
		h[13] = TCP_RST;
	} else {
		net_put32(h + 4, 0);
		net_put32(h + 8, seq);
		h[13] = TCP_RST | TCP_ACK;
	}

	h[12] = (TCP_HDR_LEN / 4) << 4;

	sum = pseudo_sum_tcp(dst, src, TCP_HDR_LEN);
	sum = net_checksum_partial(h, TCP_HDR_LEN, sum);
	net_put16(h + 16, net_checksum_finish(sum));

	segments_sent++;
	ip_transmit(src, IP_PROTO_TCP, b);
}

static struct tcp_conn *alloc_conn(void)
{
	unsigned i;

	for (i = 0; i < TCP_MAX_CONNECTIONS; i++) {
		if (!conns[i].used) {
			struct tcp_conn *c = &conns[i];

			kmemset(c, 0, sizeof(*c));

			c->tx_page = pmm_alloc_page();
			c->rx_page = pmm_alloc_page();

			if (!c->tx_page || !c->rx_page) {
				if (c->tx_page)
					pmm_free_pages(c->tx_page, 1);
				if (c->rx_page)
					pmm_free_pages(c->rx_page, 1);
				kmemset(c, 0, sizeof(*c));
				return NULL;
			}

			c->tx_buf = phys_to_virt(c->tx_page);
			c->rx_buf = phys_to_virt(c->rx_page);
			c->used = true;
			c->accept_of = -1;
			c->state = TCP_CLOSED;
			return c;
		}
	}

	return NULL;
}

static void free_conn(struct tcp_conn *c)
{
	if (!c->used)
		return;

	if (c->tx_page)
		pmm_free_pages(c->tx_page, 1);
	if (c->rx_page)
		pmm_free_pages(c->rx_page, 1);

	kmemset(c, 0, sizeof(*c));
	c->accept_of = -1;
}

/* The initial sequence number. Random, and this refuses rather than falling
 * back to a counter -- see the note at the top of this file. */
static bool initial_sequence(u32 *out)
{
	u32 v;

	if (!random_bytes(&v, sizeof(v)))
		return false;

	*out = v;
	return true;
}

static struct tcp_conn *find_conn(ipv4_addr local, u16 lport,
				  ipv4_addr remote, u16 rport)
{
	unsigned i;
	struct tcp_conn *listener = NULL;

	for (i = 0; i < TCP_MAX_CONNECTIONS; i++) {
		struct tcp_conn *c = &conns[i];

		if (!c->used)
			continue;

		if (c->local_port != lport)
			continue;

		/* An exact match on all four is the connection. This is looked
		 * for before the listener, because a listening socket and an
		 * established connection share a port by design and answering
		 * from the listener would restart a live conversation. */
		if (c->state != TCP_LISTEN &&
		    c->remote_ip == remote && c->remote_port == rport &&
		    (!c->local_ip || c->local_ip == local))
			return c;

		if (c->state == TCP_LISTEN)
			listener = c;
	}

	return listener;
}

/* --- Receiving -------------------------------------------------------------- */

static void deliver(struct tcp_conn *c, const u8 *data, u32 len)
{
	u32 i;

	for (i = 0; i < len && c->rx_len < TCP_BUFFER_SIZE; i++) {
		u32 at = (c->rx_head + c->rx_len) % TCP_BUFFER_SIZE;

		c->rx_buf[at] = data[i];
		c->rx_len++;
	}

	/* Anything that did not fit is not acknowledged, so the far end sends
	 * it again when the window opens. Acknowledging bytes that were
	 * dropped is how a stream silently loses data. */
	c->rcv_nxt += i;
}

void tcp_receive(struct net_device *dev, struct netbuf *b)
{
	const u8 *h = b->data;
	u16 sport, dport;
	u32 seq, ack, hdr_len, payload_len;
	u8 flags;
	struct tcp_conn *c;
	u64 lock_flags;

	(void)dev;

	if (b->len < TCP_HDR_LEN) {
		rx_short++;
		netbuf_free(b);
		return;
	}

	{
		u32 sum = pseudo_sum_tcp(b->src_ip, b->dst_ip, (u16)b->len);

		sum = net_checksum_partial(h, b->len, sum);

		if (net_checksum_finish(sum) != 0) {
			rx_bad_checksum++;
			netbuf_free(b);
			return;
		}
	}

	sport = net_get16(h + 0);
	dport = net_get16(h + 2);
	seq = net_get32(h + 4);
	ack = net_get32(h + 8);
	hdr_len = (u32)(h[12] >> 4) * 4;
	flags = h[13];

	if (hdr_len < TCP_HDR_LEN || hdr_len > b->len) {
		rx_short++;
		netbuf_free(b);
		return;
	}

	payload_len = b->len - hdr_len;
	segments_received++;

	ensure_lock();
	lock_flags = spin_lock_irq(&tcp_lock);

	c = find_conn(b->dst_ip, dport, b->src_ip, sport);

	if (!c) {
		spin_unlock_irq(&tcp_lock, lock_flags);
		rx_no_connection++;

		/* Never answer a reset with a reset -- two machines that both
		 * do it trade resets for ever. */
		if (!(flags & TCP_RST))
			send_reset(b->src_ip, b->dst_ip, sport, dport,
				   seq + payload_len + ((flags & TCP_SYN) ? 1 : 0),
				   ack, (flags & TCP_ACK) != 0);

		netbuf_free(b);
		return;
	}

	if (flags & TCP_RST) {
		connections_reset++;
		c->reset = true;
		c->state = TCP_CLOSED;
		spin_unlock_irq(&tcp_lock, lock_flags);
		netbuf_free(b);
		return;
	}

	switch (c->state) {
	case TCP_LISTEN:
		if (flags & TCP_SYN) {
			/* A new connection gets its own structure; the
			 * listener stays listening. A stack that turns the
			 * listener itself into the connection accepts exactly
			 * one client and then appears to hang. */
			struct tcp_conn *n = alloc_conn();
			u32 iss;

			if (!n || !initial_sequence(&iss)) {
				if (n)
					free_conn(n);
				spin_unlock_irq(&tcp_lock, lock_flags);
				netbuf_free(b);
				return;
			}

			n->local_ip = b->dst_ip;
			n->local_port = dport;
			n->remote_ip = b->src_ip;
			n->remote_port = sport;
			n->rcv_nxt = seq + 1;
			n->snd_una = iss;
			n->snd_nxt = iss + 1;
			n->snd_wnd = net_get16(h + 14);
			n->state = TCP_SYN_RECEIVED;
			n->accept_of = (int)(c - conns);
			n->retransmit_at_ns = time_monotonic_ns() + TCP_RTO_NS;

			send_segment(n, TCP_SYN | TCP_ACK, iss, NULL, 0);
		}
		break;

	case TCP_SYN_SENT:
		if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) &&
		    ack == c->snd_nxt) {
			c->rcv_nxt = seq + 1;
			c->snd_una = ack;
			c->snd_wnd = net_get16(h + 14);
			c->state = TCP_ESTABLISHED;
			c->retries = 0;
			connections_opened++;
			send_segment(c, TCP_ACK, c->snd_nxt, NULL, 0);
		}
		break;

	case TCP_SYN_RECEIVED:
		if ((flags & TCP_ACK) && ack == c->snd_nxt) {
			c->snd_una = ack;
			c->state = TCP_ESTABLISHED;
			c->accept_pending = true;
			c->retries = 0;
			connections_opened++;
		}
		break;

	case TCP_ESTABLISHED:
	case TCP_FIN_WAIT_1:
	case TCP_FIN_WAIT_2:
		if (flags & TCP_ACK) {
			if (seq_le(c->snd_una, ack) && seq_le(ack, c->snd_nxt)) {
				u32 acked = ack - c->snd_una;

				/* Bytes the far end has confirmed leave the
				 * send buffer, and only then. */
				if (acked <= c->tx_len) {
					c->tx_head = (c->tx_head + acked) %
						     TCP_BUFFER_SIZE;
					c->tx_len -= acked;
				}

				c->snd_una = ack;
				c->retries = 0;
			}

			c->snd_wnd = net_get16(h + 14);
		}

		if (payload_len) {
			if (seq == c->rcv_nxt) {
				deliver(c, h + hdr_len, payload_len);
				send_segment(c, TCP_ACK, c->snd_nxt, NULL, 0);
			} else {
				/* Out of order. Dropped, and the duplicate
				 * acknowledgement tells the far end where the
				 * gap starts -- which is what makes this
				 * recoverable rather than merely lossy. */
				rx_out_of_order++;
				send_segment(c, TCP_ACK, c->snd_nxt, NULL, 0);
			}
		}

		if ((flags & TCP_FIN) && seq == c->rcv_nxt) {
			c->rcv_nxt++;
			c->peer_closed = true;
			send_segment(c, TCP_ACK, c->snd_nxt, NULL, 0);

			if (c->state == TCP_ESTABLISHED)
				c->state = TCP_CLOSE_WAIT;
			else
				c->state = TCP_TIME_WAIT;

			c->time_wait_until_ns = time_monotonic_ns() +
						TCP_TIME_WAIT_NS;
		} else if (c->state == TCP_FIN_WAIT_1 && (flags & TCP_ACK) &&
			   ack == c->snd_nxt) {
			c->state = TCP_FIN_WAIT_2;
		}
		break;

	case TCP_LAST_ACK:
		if ((flags & TCP_ACK) && ack == c->snd_nxt) {
			free_conn(c);
			spin_unlock_irq(&tcp_lock, lock_flags);
			netbuf_free(b);
			return;
		}
		break;

	case TCP_CLOSE_WAIT:
	case TCP_CLOSING:
	case TCP_TIME_WAIT:
	case TCP_CLOSED:
		break;
	}

	spin_unlock_irq(&tcp_lock, lock_flags);
	netbuf_free(b);
}

/* --- Time ------------------------------------------------------------------ */

/* Retransmission, and the expiry of TIME_WAIT. Called from the same place the
 * receive queue is drained, so that a machine which is not receiving anything
 * still makes progress on what it has sent. */
void tcp_tick(void)
{
	unsigned i;
	u64 now = time_monotonic_ns();
	u64 flags;

	ensure_lock();
	flags = spin_lock_irq(&tcp_lock);

	for (i = 0; i < TCP_MAX_CONNECTIONS; i++) {
		struct tcp_conn *c = &conns[i];

		if (!c->used)
			continue;

		if (c->state == TCP_TIME_WAIT &&
		    now >= c->time_wait_until_ns) {
			/* The wait exists so that a late duplicate of the last
			 * segment is answered by a connection that still
			 * exists, rather than by a reset to whoever has since
			 * taken the port. */
			free_conn(c);
			continue;
		}

		if (c->tx_len && c->retransmit_at_ns &&
		    now >= c->retransmit_at_ns) {
			if (c->retries >= TCP_MAX_RETRIES) {
				c->state = TCP_CLOSED;
				c->reset = true;
				continue;
			}

			/* Everything from the oldest unacknowledged byte, not
			 * just the last segment: the receiver drops out of
			 * order, so resending the tail alone would be dropped
			 * again every time. */
			{
				u8 chunk[512];
				u32 n = c->tx_len > sizeof(chunk) ?
					(u32)sizeof(chunk) : c->tx_len;
				u32 k;

				for (k = 0; k < n; k++)
					chunk[k] = c->tx_buf[(c->tx_head + k) %
							     TCP_BUFFER_SIZE];

				retransmits++;
				c->retries++;
				send_segment(c, TCP_ACK | TCP_PSH, c->snd_una,
					     chunk, n);

				/* Back off, so a link that is down is not
				 * hammered at a fixed rate by every
				 * connection on the machine at once. */
				c->retransmit_at_ns = now +
					(TCP_RTO_NS << (c->retries < 5 ?
							c->retries : 5));
			}
		}
	}

	spin_unlock_irq(&tcp_lock, flags);
}

void tcp_print_summary(void)
{
	unsigned i, live = 0;

	for (i = 0; i < TCP_MAX_CONNECTIONS; i++)
		if (conns[i].used)
			live++;

	kprintf("  tcp          : %u connections, %u opened, %u reset\n",
		live, (unsigned)connections_opened,
		(unsigned)connections_reset);
	kprintf("               : %u reads served\n",
		(unsigned)receives_hint);
	kprintf("               : %u segments out (%u retransmitted), "
		"%u in\n",
		(unsigned)segments_sent, (unsigned)retransmits,
		(unsigned)segments_received);

	if (rx_short || rx_bad_checksum || rx_no_connection || rx_out_of_order)
		kprintf("               : %u short, %u bad checksum, "
			"%u no connection, %u out of order\n",
			(unsigned)rx_short, (unsigned)rx_bad_checksum,
			(unsigned)rx_no_connection, (unsigned)rx_out_of_order);
}

/* --- What a socket asks of a connection ------------------------------------ */

/* A connection is named by index, not by pointer. A socket that held a pointer
 * across a close would be holding freed memory the moment the slot was reused;
 * an index is checked against `used` on every call and answers for whatever is
 * in the slot now. */

int tcp_open(ipv4_addr local, u16 lport, ipv4_addr remote, u16 rport)
{
	struct tcp_conn *c;
	u32 iss;
	u64 flags;
	int idx = -1;

	ensure_lock();
	flags = spin_lock_irq(&tcp_lock);

	c = alloc_conn();

	if (!c) {
		spin_unlock_irq(&tcp_lock, flags);
		return -1;
	}

	if (!initial_sequence(&iss)) {
		/* No entropy. Refused rather than falling back to a counter --
		 * a predictable initial sequence number is the one shortcut in
		 * this file that is a vulnerability rather than a slowdown. */
		free_conn(c);
		spin_unlock_irq(&tcp_lock, flags);
		return -1;
	}

	c->local_ip = local;
	c->local_port = lport;
	c->remote_ip = remote;
	c->remote_port = rport;
	c->snd_una = iss;
	c->snd_nxt = iss + 1;		/* SYN takes one sequence number */
	c->state = TCP_SYN_SENT;
	c->retransmit_at_ns = time_monotonic_ns() + TCP_RTO_NS;

	idx = (int)(c - conns);

	send_segment(c, TCP_SYN, iss, NULL, 0);

	spin_unlock_irq(&tcp_lock, flags);
	return idx;
}

int tcp_open_listener(ipv4_addr local, u16 port)
{
	struct tcp_conn *c;
	u64 flags;
	int idx;

	ensure_lock();
	flags = spin_lock_irq(&tcp_lock);

	c = alloc_conn();

	if (!c) {
		spin_unlock_irq(&tcp_lock, flags);
		return -1;
	}

	c->local_ip = local;
	c->local_port = port;
	c->state = TCP_LISTEN;
	c->listening = true;
	idx = (int)(c - conns);

	spin_unlock_irq(&tcp_lock, flags);
	return idx;
}

/* The next connection that finished its handshake on this listener, or -1. */
int tcp_accept_ready(int listener)
{
	unsigned i;
	u64 flags;
	int idx = -1;

	if (listener < 0 || listener >= TCP_MAX_CONNECTIONS)
		return -1;

	ensure_lock();
	flags = spin_lock_irq(&tcp_lock);

	for (i = 0; i < TCP_MAX_CONNECTIONS; i++) {
		if (conns[i].used && conns[i].accept_pending &&
		    conns[i].accept_of == listener) {
			conns[i].accept_pending = false;
			idx = (int)i;
			break;
		}
	}

	spin_unlock_irq(&tcp_lock, flags);
	return idx;
}

enum tcp_state tcp_state_of(int idx)
{
	if (idx < 0 || idx >= TCP_MAX_CONNECTIONS || !conns[idx].used)
		return TCP_CLOSED;

	return conns[idx].state;
}

i64 tcp_write(int idx, const void *data, u32 len)
{
	struct tcp_conn *c;
	const u8 *p = data;
	u32 n = 0;
	u64 flags;

	if (idx < 0 || idx >= TCP_MAX_CONNECTIONS)
		return -1;

	ensure_lock();
	flags = spin_lock_irq(&tcp_lock);

	c = &conns[idx];

	if (!c->used || c->state != TCP_ESTABLISHED) {
		spin_unlock_irq(&tcp_lock, flags);
		return -1;
	}

	/* Into the send buffer first, and only then onto the wire. The buffer
	 * is what a retransmission reads from -- data sent straight to the
	 * device and not kept is data that cannot be sent again, which is the
	 * one thing TCP exists to be able to do. */
	while (n < len && c->tx_len < TCP_BUFFER_SIZE) {
		u32 at = (c->tx_head + c->tx_len) % TCP_BUFFER_SIZE;

		c->tx_buf[at] = p[n];
		c->tx_len++;
		n++;
	}

	if (n) {
		u8 chunk[512];
		u32 send_now = n > sizeof(chunk) ? (u32)sizeof(chunk) : n;
		u32 k;

		/* Never more than the far end says it can take. A sender that
		 * ignores the advertised window overruns the receiver's buffer
		 * and the loss looks like a bad network. */
		if (c->snd_wnd && send_now > c->snd_wnd)
			send_now = c->snd_wnd;

		for (k = 0; k < send_now; k++)
			chunk[k] = c->tx_buf[(c->tx_head +
					      (c->tx_len - n) + k) %
					     TCP_BUFFER_SIZE];

		if (send_now) {
			send_segment(c, TCP_ACK | TCP_PSH, c->snd_nxt,
				     chunk, send_now);
			c->snd_nxt += send_now;
			c->retransmit_at_ns = time_monotonic_ns() + TCP_RTO_NS;
			c->retries = 0;
		}
	}

	spin_unlock_irq(&tcp_lock, flags);
	return (i64)n;
}

i64 tcp_read(int idx, void *data, u32 len)
{
	struct tcp_conn *c;
	u8 *p = data;
	u32 n = 0;
	u64 flags;

	if (idx < 0 || idx >= TCP_MAX_CONNECTIONS)
		return -1;

	ensure_lock();
	flags = spin_lock_irq(&tcp_lock);

	c = &conns[idx];

	if (!c->used) {
		spin_unlock_irq(&tcp_lock, flags);
		return -1;
	}

	while (n < len && c->rx_len) {
		p[n] = c->rx_buf[c->rx_head];
		c->rx_head = (c->rx_head + 1) % TCP_BUFFER_SIZE;
		c->rx_len--;
		n++;
	}

	/* Nothing left and the far end has finished: that is the end of the
	 * stream, which is what zero means. A connection merely quiet also
	 * returns zero, and the difference is `peer_closed` -- which is why a
	 * caller that needs to tell them apart asks the state. */
	spin_unlock_irq(&tcp_lock, flags);

	if (n)
		receives_hint++;

	return (i64)n;
}

void tcp_shutdown(int idx)
{
	struct tcp_conn *c;
	u64 flags;

	if (idx < 0 || idx >= TCP_MAX_CONNECTIONS)
		return;

	ensure_lock();
	flags = spin_lock_irq(&tcp_lock);

	c = &conns[idx];

	if (!c->used) {
		spin_unlock_irq(&tcp_lock, flags);
		return;
	}

	switch (c->state) {
	case TCP_ESTABLISHED:
		send_segment(c, TCP_FIN | TCP_ACK, c->snd_nxt, NULL, 0);
		c->snd_nxt++;
		c->state = TCP_FIN_WAIT_1;
		break;

	case TCP_CLOSE_WAIT:
		/* The far end closed first; this is the other half. */
		send_segment(c, TCP_FIN | TCP_ACK, c->snd_nxt, NULL, 0);
		c->snd_nxt++;
		c->state = TCP_LAST_ACK;
		break;

	case TCP_LISTEN:
	case TCP_SYN_SENT:
		free_conn(c);
		break;

	default:
		break;
	}

	spin_unlock_irq(&tcp_lock, flags);
}

/* --- The test --------------------------------------------------------------- */

bool tcp_self_test(void)
{
	bool ok = true;

	/* Sequence comparison must survive the wrap at 2^32. This is the
	 * assertion worth having: a stack that compares with `<` works
	 * perfectly for four gigabytes and then stalls, on a connection that
	 * has been up long enough that nobody is watching. */
	if (!seq_lt(0xFFFFFFF0u, 0x00000010u)) {
		kprintf("tcp: a sequence comparison across the wrap is wrong\n");
		ok = false;
	}

	if (seq_lt(0x00000010u, 0xFFFFFFF0u)) {
		kprintf("tcp: a sequence comparison is wrong the other way\n");
		ok = false;
	}

	if (!seq_le(100, 100) || seq_lt(100, 100)) {
		kprintf("tcp: equal sequence numbers compare wrongly\n");
		ok = false;
	}

	/* The initial sequence number is random and is not a counter. Two
	 * connections opened in succession must not differ by something
	 * predictable -- the whole reason this is not a counter. */
	{
		u32 a = 0, b2 = 0;

		if (!initial_sequence(&a) || !initial_sequence(&b2)) {
			kprintf("tcp: no entropy for an initial sequence "
				"number\n");
			ok = false;
		} else if (a == b2 || b2 - a == 1) {
			kprintf("tcp: initial sequence numbers look like a "
				"counter\n");
			ok = false;
		}
	}

	/* A connection's receive buffer accepts what fits and no more, and the
	 * advertised window follows the free space. A constant window is what
	 * makes a stack overflow its own buffer under load. */
	{
		struct tcp_conn *c;
		u64 flags;
		static const u8 data[64] = { 0 };

		ensure_lock();
		flags = spin_lock_irq(&tcp_lock);
		c = alloc_conn();
		spin_unlock_irq(&tcp_lock, flags);

		if (!c) {
			kprintf("tcp: no room for a test connection\n");
			return false;
		}

		if (rx_free(c) != TCP_BUFFER_SIZE) {
			kprintf("tcp: an empty buffer does not advertise its "
				"whole size\n");
			ok = false;
		}

		deliver(c, data, sizeof(data));

		if (c->rx_len != sizeof(data)) {
			kprintf("tcp: %u bytes delivered, expected 64\n",
				(unsigned)c->rx_len);
			ok = false;
		}

		if (rx_free(c) != TCP_BUFFER_SIZE - sizeof(data)) {
			kprintf("tcp: the window did not shrink with the "
				"buffer\n");
			ok = false;
		}

		/* Fill it past the end. What does not fit must not be
		 * acknowledged, or the far end believes bytes arrived that
		 * were thrown away. */
		{
			u32 before = c->rcv_nxt;
			u8 big[512];
			unsigned n = 0;

			kmemset(big, 0x5A, sizeof(big));

			while (rx_free(c) && n < 32) {
				deliver(c, big, sizeof(big));
				n++;
			}

			if (c->rx_len > TCP_BUFFER_SIZE) {
				kprintf("tcp: the receive buffer overflowed\n");
				ok = false;
			}

			if (c->rcv_nxt - before > TCP_BUFFER_SIZE) {
				kprintf("tcp: acknowledged more bytes than the "
					"buffer can hold\n");
				ok = false;
			}
		}

		flags = spin_lock_irq(&tcp_lock);
		free_conn(c);
		spin_unlock_irq(&tcp_lock, flags);
	}

	/* Every state has a name. A summary that prints "?" for a state a
	 * connection can actually be in is a diagnostic that fails exactly
	 * when it is needed. */
	{
		int s;

		for (s = TCP_CLOSED; s <= TCP_TIME_WAIT; s++) {
			const char *n = tcp_state_name((enum tcp_state)s);

			if (!n || n[0] == '?') {
				kprintf("tcp: state %d has no name\n", s);
				ok = false;
			}
		}
	}

	return ok;
}
