/* Sockets: the shape every program already knows, over the VFS it already has.
 *
 * The blueprint asks for the BSD socket API, and the reason to give it exactly
 * that shape rather than a better one is that it is the interface every
 * program on earth was written against. A stack with a cleaner API is a stack
 * nothing can be ported to.
 *
 * --- Why this is a file ---
 *
 * A socket is a descriptor, and `read`, `write` and `close` work on it without
 * knowing what it is. That is not a convenience: it is the test of whether the
 * VFS built in 1.6 is an interface or is ReconFS with the serial numbers filed
 * off. A socket has no path, no size, no position and no disk, so if
 * `file_ops` fits it, `file_ops` is real.
 *
 * `read` on a socket is `recv`, `write` is `send`, and `seek` is *null* rather
 * than an error-returning stub -- a stream has no position to move to, and the
 * VFS already answers "you cannot do that" once, above, for every file whose
 * operation is missing.
 *
 * --- What is deliberately absent ---
 *
 * No `select` or `poll`. Nothing in this kernel yet waits on more than one
 * descriptor at a time, and the multiplexing interface is the one that is
 * hardest to change once anything depends on it.
 *
 * No non-blocking mode. A read with nothing to read runs the stack and then
 * returns what it has, which for a kernel with one thread of network activity
 * is the same answer and much less machinery.
 */
#include <recon/kernel/net.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/console.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/time.h>
#include <recon/kernel/random.h>

#define SOCKET_MAX 16
#define DGRAM_QUEUE 4

/* Ports handed out to a socket that did not ask for one. The range above
 * 49152 is the one reserved for exactly this by IANA, so an automatic choice
 * cannot collide with a service somebody meant to run. */
#define EPHEMERAL_FIRST 49152
#define EPHEMERAL_COUNT 16384

struct dgram {
	struct netbuf *buf;
	ipv4_addr from;
	u16 port;
};

struct socket {
	bool used;
	int type;

	ipv4_addr local_ip, remote_ip;
	u16 local_port, remote_port;

	bool bound;
	bool listening;
	bool connected;

	/* The connection this socket is, for a stream socket. -1 when there is
	 * none -- which is every datagram socket, and a stream socket that has
	 * not connected or listened yet. */
	int conn;

	/* UDP: a small ring of received datagrams. Bounded, and a full ring
	 * drops the newest -- because dropping the oldest would mean a burst
	 * of traffic silently replacing data a program has not read yet. */
	struct dgram queue[DGRAM_QUEUE];
	unsigned q_head, q_len;
	u64 dropped;
};

static struct socket sockets[SOCKET_MAX];
static struct spinlock sock_lock;
static bool lock_ready;

static u64 created, closed, sends, receives;

static void ensure_lock(void)
{
	if (!lock_ready) {
		spin_init(&sock_lock, "socket");
		lock_ready = true;
	}
}

static u16 ephemeral_port(void)
{
	u16 v = 0;

	/* Random rather than sequential, for the same reason TCP's initial
	 * sequence number is: a predictable source port is one less thing an
	 * attacker has to guess to inject into a conversation. */
	if (!random_bytes(&v, sizeof(v)))
		v = (u16)(time_monotonic_ns() >> 8);

	return (u16)(EPHEMERAL_FIRST + (v % EPHEMERAL_COUNT));
}

static void udp_arrived(void *ctx, struct netbuf *b)
{
	struct socket *s = ctx;
	u64 flags;

	if (!s) {
		netbuf_free(b);
		return;
	}

	ensure_lock();
	flags = spin_lock_irq(&sock_lock);

	if (s->q_len >= DGRAM_QUEUE) {
		s->dropped++;
		spin_unlock_irq(&sock_lock, flags);
		netbuf_free(b);
		return;
	}

	{
		unsigned at = (s->q_head + s->q_len) % DGRAM_QUEUE;

		s->queue[at].buf = b;
		s->queue[at].from = b->src_ip;
		s->queue[at].port = b->src_port;
		s->q_len++;
	}

	spin_unlock_irq(&sock_lock, flags);
}

struct socket *socket_create(int type)
{
	unsigned i;
	u64 flags;
	struct socket *s = NULL;

	if (type != SOCK_STREAM && type != SOCK_DGRAM)
		return NULL;

	ensure_lock();
	flags = spin_lock_irq(&sock_lock);

	for (i = 0; i < SOCKET_MAX; i++) {
		if (!sockets[i].used) {
			s = &sockets[i];
			kmemset(s, 0, sizeof(*s));
			s->used = true;
			s->type = type;
			s->conn = -1;
			created++;
			break;
		}
	}

	spin_unlock_irq(&sock_lock, flags);
	return s;
}

bool socket_bind(struct socket *s, ipv4_addr addr, u16 port)
{
	if (!s || s->bound)
		return false;

	if (!port)
		port = ephemeral_port();

	if (s->type == SOCK_DGRAM) {
		if (!udp_bind_port(port, udp_arrived, s))
			return false;
	}

	s->local_ip = addr;
	s->local_port = port;
	s->bound = true;
	return true;
}

bool socket_listen(struct socket *s, unsigned backlog)
{
	(void)backlog;

	if (!s || s->type != SOCK_STREAM || !s->bound)
		return false;

	s->conn = tcp_open_listener(s->local_ip, s->local_port);

	if (s->conn < 0)
		return false;

	s->listening = true;
	return true;
}

struct socket *socket_accept(struct socket *s)
{
	int idx;
	struct socket *n;

	/* Returns null when nothing is waiting, rather than blocking -- for
	 * the same reason ARP does not block: the handshake completes when a
	 * frame is processed, and the thread that processes frames may be this
	 * one. A caller waits by doing something else and asking again. */
	if (!s || !s->listening)
		return NULL;

	netdev_service();
	ip_flush_pending();

	idx = tcp_accept_ready(s->conn);

	if (idx < 0)
		return NULL;

	n = socket_create(SOCK_STREAM);

	if (!n) {
		/* No socket to hand back, so the connection is closed rather
		 * than left established with nobody owning it -- a connection
		 * nothing can read from is one the far end waits on for ever. */
		tcp_shutdown(idx);
		return NULL;
	}

	n->conn = idx;
	n->connected = true;
	n->bound = true;
	n->local_port = s->local_port;
	return n;
}

bool socket_connect(struct socket *s, ipv4_addr addr, u16 port)
{
	if (!s)
		return false;

	if (!s->bound && !socket_bind(s, IPV4_ANY, 0))
		return false;

	s->remote_ip = addr;
	s->remote_port = port;

	if (s->type == SOCK_STREAM) {
		s->conn = tcp_open(s->local_ip, s->local_port, addr, port);

		if (s->conn < 0)
			return false;
	}

	s->connected = true;
	return true;
}

i64 socket_sendto(struct socket *s, const void *data, u32 len,
		  ipv4_addr addr, u16 port)
{
	if (!s || s->type != SOCK_DGRAM)
		return -1;

	if (!s->bound && !socket_bind(s, IPV4_ANY, 0))
		return -1;

	if (!udp_send(addr, port, s->local_port, data, len))
		return -1;

	sends++;
	return (i64)len;
}

i64 socket_send(struct socket *s, const void *data, u32 len)
{
	if (!s || !s->connected)
		return -1;

	if (s->type == SOCK_DGRAM)
		return socket_sendto(s, data, len, s->remote_ip,
				     s->remote_port);

	{
		i64 n = tcp_write(s->conn, data, len);

		if (n > 0)
			sends++;

		return n;
	}
}

i64 socket_recvfrom(struct socket *s, void *data, u32 len,
		    ipv4_addr *addr, u16 *port)
{
	struct netbuf *b = NULL;
	ipv4_addr from = 0;
	u16 fport = 0;
	u32 n;
	u64 flags;

	if (!s)
		return -1;

	/* Run the stack first. A caller asking for data is a caller that will
	 * otherwise sleep with frames sitting unprocessed behind it. */
	netdev_service();
	ip_flush_pending();
	tcp_tick();

	if (s->type == SOCK_STREAM) {
		i64 n = tcp_read(s->conn, data, len);

		if (n > 0) {
			receives++;

			if (addr)
				*addr = s->remote_ip;
			if (port)
				*port = s->remote_port;
		}

		return n;
	}

	ensure_lock();
	flags = spin_lock_irq(&sock_lock);

	if (s->q_len) {
		b = s->queue[s->q_head].buf;
		from = s->queue[s->q_head].from;
		fport = s->queue[s->q_head].port;
		s->queue[s->q_head].buf = NULL;
		s->q_head = (s->q_head + 1) % DGRAM_QUEUE;
		s->q_len--;
	}

	spin_unlock_irq(&sock_lock, flags);

	if (!b)
		return 0;

	n = b->len < len ? b->len : len;
	kmemcpy(data, b->data, n);

	if (addr)
		*addr = from;
	if (port)
		*port = fport;

	netbuf_free(b);
	receives++;
	return (i64)n;
}

i64 socket_recv(struct socket *s, void *data, u32 len)
{
	return socket_recvfrom(s, data, len, NULL, NULL);
}

void socket_close(struct socket *s)
{
	unsigned i;
	u64 flags;

	if (!s || !s->used)
		return;

	if (s->type == SOCK_DGRAM && s->bound)
		udp_unbind_port(s->local_port);

	if (s->type == SOCK_STREAM && s->conn >= 0)
		tcp_shutdown(s->conn);

	ensure_lock();
	flags = spin_lock_irq(&sock_lock);

	for (i = 0; i < DGRAM_QUEUE; i++) {
		if (s->queue[i].buf) {
			netbuf_free(s->queue[i].buf);
			s->queue[i].buf = NULL;
		}
	}

	kmemset(s, 0, sizeof(*s));
	s->conn = -1;
	closed++;

	spin_unlock_irq(&sock_lock, flags);
}

void socket_print_summary(void)
{
	unsigned i, live = 0;
	u64 dropped = 0;

	for (i = 0; i < SOCKET_MAX; i++) {
		if (sockets[i].used)
			live++;
		dropped += sockets[i].dropped;
	}

	kprintf("  sockets      : %u open, %u created, %u closed\n",
		live, (unsigned)created, (unsigned)closed);
	kprintf("               : %u sends, %u receives, %u datagrams "
		"dropped full\n",
		(unsigned)sends, (unsigned)receives, (unsigned)dropped);
}

bool socket_self_test(void)
{
	struct socket *a, *b;
	bool ok = true;

	a = socket_create(SOCK_DGRAM);

	if (!a) {
		kprintf("socket: could not create one\n");
		return false;
	}

	/* A bind with no port gets an ephemeral one, in the range reserved for
	 * that purpose -- not port 1, and not zero. */
	if (!socket_bind(a, IPV4_ANY, 0)) {
		kprintf("socket: bind with an automatic port failed\n");
		ok = false;
	} else if (a->local_port < EPHEMERAL_FIRST) {
		kprintf("socket: automatic port %u is below the ephemeral "
			"range\n", (unsigned)a->local_port);
		ok = false;
	}

	/* Two sockets cannot hold one port. */
	b = socket_create(SOCK_DGRAM);

	if (b) {
		if (socket_bind(b, IPV4_ANY, a->local_port)) {
			kprintf("socket: two sockets bound one port\n");
			ok = false;
		}

		socket_close(b);
	}

	/* A datagram delivered to the socket comes back out of recvfrom with
	 * the address it came from -- which is the whole difference between
	 * recv and recvfrom, and the field a server needs to reply. */
	{
		struct netbuf *d = netbuf_alloc();

		if (d) {
			u8 *p = netbuf_put(d, 5);

			if (p)
				kmemcpy(p, "hello", 5);

			d->src_ip = IPV4(10, 1, 2, 3);
			d->src_port = 1234;

			udp_arrived(a, d);

			{
				char got[8];
				ipv4_addr from = 0;
				u16 port = 0;
				i64 n;

				kmemset(got, 0, sizeof(got));
				n = socket_recvfrom(a, got, sizeof(got) - 1,
						    &from, &port);

				if (n != 5) {
					kprintf("socket: recvfrom returned %d, "
						"expected 5\n", (int)n);
					ok = false;
				} else if (kmemcmp(got, "hello", 5) != 0) {
					kprintf("socket: the payload changed\n");
					ok = false;
				}

				if (from != IPV4(10, 1, 2, 3) || port != 1234) {
					kprintf("socket: the sender's address "
						"did not survive\n");
					ok = false;
				}
			}
		}
	}

	/* The queue is bounded, and a full one drops the newest rather than
	 * overwriting something a program has not read. */
	{
		unsigned i;
		u64 before = a->dropped;

		for (i = 0; i < DGRAM_QUEUE + 2; i++) {
			struct netbuf *d = netbuf_alloc();

			if (!d)
				break;

			netbuf_put(d, 4);
			udp_arrived(a, d);
		}

		if (a->q_len > DGRAM_QUEUE) {
			kprintf("socket: the datagram queue overflowed\n");
			ok = false;
		}

		if (a->dropped == before) {
			kprintf("socket: a full queue dropped nothing and "
				"counted nothing\n");
			ok = false;
		}
	}

	/* An empty socket returns zero rather than blocking or failing. */
	socket_close(a);

	a = socket_create(SOCK_DGRAM);

	if (a) {
		char got[4];

		if (socket_recv(a, got, sizeof(got)) != 0) {
			kprintf("socket: an empty socket did not return 0\n");
			ok = false;
		}

		socket_close(a);
	}

	return ok;
}
