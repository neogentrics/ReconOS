/* The network, from the wire up.
 *
 * Everything above a network card is layered, and the layering is not an
 * organising idea somebody imposed afterwards -- it is forced by the fact that
 * each layer is *read by a different machine*. Ethernet is read by the switch
 * in the wall, IP by every router between here and the far end, and TCP by
 * nothing until the far end itself. A packet is therefore an onion, and the
 * only two operations that matter are putting a layer on and taking one off.
 *
 * --- Why a packet buffer is not just a byte array ---
 *
 * A program hands down payload. TCP prepends its header, IP prepends its own,
 * Ethernet prepends a third. Done with a plain array, each of those is a copy
 * of everything below it -- three copies of every byte the machine sends.
 *
 * So a buffer is allocated with **empty space in front of the data**, and the
 * layers grow backwards into it. `netbuf_push` moves the start pointer back
 * and hands you the room; `netbuf_pull` steps it forwards past a header on the
 * way up. Nothing is copied in either direction, and that single decision is
 * most of what separates a stack from a demonstration.
 *
 * This is the same idea as Linux's `sk_buff`, which the blueprint names. The
 * name is not borrowed because the thing is not the same thing: this holds one
 * contiguous run of bytes and cannot be a chain of fragments, which is a real
 * limitation and is written down in `netbuf_alloc` rather than discovered.
 *
 * --- Byte order, and why it is not optional ---
 *
 * Every integer on the wire is big-endian, and both machines this kernel runs
 * on are little-endian. There is no "usually works" here: a port number sent
 * the wrong way round is a different port, and a length sent the wrong way
 * round is a packet the other end drops without telling anybody.
 *
 * The conversions are written as plain shifts rather than as a byte swap
 * instruction, because `core/` may not know what machine it is on -- and
 * because a compiler turns this back into the single instruction anyway.
 *
 * --- What runs where ---
 *
 * A driver's interrupt hands a buffer to `netdev_receive` and returns. Nothing
 * above Ethernet runs in that context: a TCP state machine executed inside an
 * interrupt handler holds off every other device on the machine while it
 * thinks, and one that then takes a lock a thread already holds deadlocks the
 * processor it is on. Received frames are queued and the worker thread walks
 * them up, which is what `work.h` exists for.
 */
#ifndef RECON_KERNEL_NET_H
#define RECON_KERNEL_NET_H

#include <recon/kernel/types.h>
#include <recon/kernel/compiler.h>
#include <recon/kernel/pmm.h>

/* --- Byte order ---------------------------------------------------------- */

static inline u16 net_htons(u16 v)
{
	return (u16)((v << 8) | (v >> 8));
}

static inline u32 net_htonl(u32 v)
{
	return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
	       ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}

/* Reading the other way is the same operation, and saying so is clearer than
 * two identical functions that could drift apart. */
#define net_ntohs(v) net_htons(v)
#define net_ntohl(v) net_htonl(v)

/* Reads from a header that may not be aligned. An IP header inside an Ethernet
 * frame starts at offset 14, so every 32-bit field in it is on a 2-byte
 * boundary. x86_64 does not care; aarch64 does, for some access widths, and
 * "it worked on the machine I wrote it on" is the failure this avoids. */
static inline u16 net_get16(const void *p)
{
	const u8 *b = p;

	return (u16)(((u16)b[0] << 8) | b[1]);
}

static inline u32 net_get32(const void *p)
{
	const u8 *b = p;

	return ((u32)b[0] << 24) | ((u32)b[1] << 16) |
	       ((u32)b[2] << 8)  | b[3];
}

static inline void net_put16(void *p, u16 v)
{
	u8 *b = p;

	b[0] = (u8)(v >> 8);
	b[1] = (u8)v;
}

static inline void net_put32(void *p, u32 v)
{
	u8 *b = p;

	b[0] = (u8)(v >> 24);
	b[1] = (u8)(v >> 16);
	b[2] = (u8)(v >> 8);
	b[3] = (u8)v;
}

/* --- Addresses ----------------------------------------------------------- */

#define MAC_LEN 6

struct mac_addr {
	u8 b[MAC_LEN];
};

/* An IPv4 address is held in **host order** everywhere inside this kernel, and
 * converted exactly at the wire. Holding it in network order internally means
 * every comparison, every mask and every print has to remember which way round
 * it is, and one of them eventually does not. */
typedef u32 ipv4_addr;

#define IPV4(a, b, c, d) \
	(((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | (u32)(d))

#define IPV4_ANY       IPV4(0, 0, 0, 0)
#define IPV4_BROADCAST IPV4(255, 255, 255, 255)

extern const struct mac_addr MAC_BROADCAST;
extern const struct mac_addr MAC_ZERO;

bool mac_equal(const struct mac_addr *a, const struct mac_addr *b);

/* Into a caller's buffer, which must hold at least 18 and 16 bytes. Returns
 * the buffer, so these can be used inline in a print. */
char *mac_format(const struct mac_addr *m, char *out);
char *ipv4_format(ipv4_addr a, char *out);

/* --- Packet buffers ------------------------------------------------------ */

/* Room reserved in front of the payload, in bytes.
 *
 * The worst case a header stack needs is Ethernet (14) + IPv4 with options
 * (60) + TCP with options (60) = 134. Rounded to 192 so that the payload
 * starts 64-byte aligned, which costs nothing and keeps a header off the end
 * of a cache line. */
#define NET_HEADROOM 192

/* One buffer is one page. A standard Ethernet frame is 1518 bytes and a page
 * is 4096, so the headroom and the frame both fit with room to spare, and a
 * buffer is one allocation that can never fail halfway. Jumbo frames do not
 * fit and are not supported; that is a real limit and is asserted rather than
 * silently truncated. */
#define NET_BUF_PAGE_SIZE 4096
#define NET_BUF_CAPACITY  (NET_BUF_PAGE_SIZE - NET_HEADROOM - 64)
#define NET_MTU           1500
#define ETH_FRAME_MAX     1518

struct net_device;

struct netbuf {
	u8 *head;		/* the allocation, where headroom begins */
	u8 *data;		/* the current layer's first byte */
	u32 len;		/* bytes from `data` */
	u32 capacity;		/* bytes from `head` */

	struct netbuf *next;	/* queue linkage; owned by whatever queue holds it */

	struct net_device *dev;	/* where it arrived, or where it is going */
	paddr_t page;		/* so it can be given back */

	/* Filled in on the way up, by the layer that understood them. A layer
	 * that did not run leaves its field zero, which is why `protocol` is
	 * checked rather than assumed. */
	ipv4_addr src_ip, dst_ip;
	u16 src_port, dst_port;
	u8  protocol;
};

/* A buffer with `NET_HEADROOM` bytes already reserved: `data` starts at the
 * end of the headroom and `len` is zero, so the first `netbuf_push` has room.
 * Null when memory is gone, which a driver must treat as "drop this frame"
 * rather than as a reason to fail. */
struct netbuf *netbuf_alloc(void);
void netbuf_free(struct netbuf *b);

/* Makes room for a header of `n` bytes in front of the data and returns it.
 * Null if the headroom is exhausted -- which cannot happen for any header
 * stack this kernel builds, and is checked anyway, because the alternative is
 * writing backwards out of the allocation. */
u8 *netbuf_push(struct netbuf *b, u32 n);

/* Steps past a header of `n` bytes on the way up and returns the new `data`.
 * Null when the buffer is shorter than the header it claims to carry, which is
 * the ordinary shape of a malformed or truncated packet and must not be a
 * panic: anything on the wire can say anything. */
u8 *netbuf_pull(struct netbuf *b, u32 n);

/* Appends `n` bytes of payload and returns where to write them. Null when the
 * buffer cannot hold it. */
u8 *netbuf_put(struct netbuf *b, u32 n);

/* How many buffers exist, and how many were refused for want of memory. A
 * drop counter nothing prints is a drop counter nobody acts on -- see BG-186. */
void netbuf_print_summary(void);
bool netbuf_self_test(void);

/* --- The one-line checksum every layer above Ethernet uses ---------------- */

/* The Internet checksum: the one's-complement sum of 16-bit words, complemented.
 *
 * Used by IPv4, ICMP, UDP and TCP, with different spans and a different
 * pseudo-header, and written once because four copies of a fold-the-carries
 * loop is four chances to fold them differently. */
u16 net_checksum(const void *data, u32 len);

/* The same sum, continued across more than one region -- which TCP and UDP
 * need, because their checksum covers a pseudo-header that is not next to the
 * payload in memory. Feed `net_checksum_partial` each region, then finish. */
u32 net_checksum_partial(const void *data, u32 len, u32 sum);
u16 net_checksum_finish(u32 sum);

/* --- Network devices ------------------------------------------------------ */

struct net_device_ops {
	/* Hands one frame to the hardware. The buffer is the driver's to free
	 * once the device is done with it, however that is discovered. */
	bool (*transmit)(struct net_device *dev, struct netbuf *b);

	/* Optional: asks the device to raise an interrupt on receive. A driver
	 * with no way to do it leaves this null and is polled instead, which is
	 * the same bargain virtio's transports already make. */
	bool (*enable_interrupts)(struct net_device *dev);

	/* Called from the worker thread to collect anything the device has
	 * taken in. A polled driver does its work here; an interrupt-driven one
	 * still gets called and usually finds nothing, which is correct rather
	 * than wasteful -- an interrupt that was missed is a machine that stops
	 * receiving for ever. */
	void (*poll)(struct net_device *dev);
};

#define NET_NAME_MAX 16
#define NET_MAX_DEVICES 8

struct net_device {
	char name[NET_NAME_MAX];

	struct mac_addr mac;
	ipv4_addr ip;		/* zero until configured */
	ipv4_addr netmask;
	ipv4_addr gateway;

	u32 mtu;
	bool up;
	bool link;		/* what the device says about the cable */

	const struct net_device_ops *ops;
	void *driver;

	/* Counters. Every one of these is printed by `netdev_print_summary`,
	 * deliberately: a counter nothing prints is a number that can go to
	 * zero without anybody noticing, which is exactly what BG-186 was. */
	u64 rx_packets, rx_bytes, rx_dropped, rx_errors;
	u64 tx_packets, tx_bytes, tx_dropped, tx_errors;
};

/* Called by a driver once it has a working card. Returns null at the cap. */
struct net_device *netdev_register(const char *name,
				   const struct net_device_ops *ops,
				   void *driver, const struct mac_addr *mac);

unsigned netdev_count(void);
struct net_device *netdev_at(unsigned i);
struct net_device *netdev_by_name(const char *name);

/* The device an address should leave by: the one whose subnet contains it, or
 * the one with a gateway if none does. Null when there is no route, which is
 * an ordinary answer on a machine with no network. */
struct net_device *netdev_route(ipv4_addr dst, ipv4_addr *next_hop);

/* A driver hands a received frame here, from any context including an
 * interrupt. The buffer is queued and the worker thread takes it from there;
 * ownership passes, and the driver must not touch it again. */
void netdev_receive(struct net_device *dev, struct netbuf *b);

/* Sends one frame that already carries its Ethernet header. */
bool netdev_transmit(struct net_device *dev, struct netbuf *b);

/* Runs every device's poll and drains the receive queue. Called by the worker
 * thread, and directly by anything that is waiting for a reply and would
 * otherwise sleep with a full queue behind it. */
void netdev_service(void);

void netdev_init(void);
void netdev_print_summary(void);
bool netdev_self_test(void);

/* Takes back the most recently registered device.
 *
 * This exists for the self-tests, which register a device that exists only to
 * be measured, and it is narrow on purpose: it removes the *last* one, so it
 * can only undo a registration that has just happened. A card left behind by a
 * test is listed in the boot summary beside the real ones, and nothing on that
 * line says it is not a card -- which would make the summary a worse instrument
 * than no summary at all. */
void netdev_forget_last(void);

/* --- Ethernet ------------------------------------------------------------- */

#define ETH_HDR_LEN 14

#define ETH_TYPE_IPV4 0x0800
#define ETH_TYPE_ARP  0x0806
#define ETH_TYPE_IPV6 0x86DD

/* Prepends an Ethernet header and hands the frame to the device. */
bool eth_transmit(struct net_device *dev, const struct mac_addr *dst,
		  u16 ethertype, struct netbuf *b);

/* One received frame, with its Ethernet header still on. Consumes the buffer. */
void eth_receive(struct net_device *dev, struct netbuf *b);

/* --- ARP ------------------------------------------------------------------ */

/* Asks the wire who holds `ip`, or answers from the cache.
 *
 * Returns true and fills `out` on a hit. On a miss it sends a request and
 * returns false -- it does **not** block, because the caller may be the worker
 * thread that would have to run to process the reply. A caller that needs the
 * answer waits and asks again. */
bool arp_lookup(struct net_device *dev, ipv4_addr ip, struct mac_addr *out);

void arp_receive(struct net_device *dev, struct netbuf *b);
void arp_print_summary(void);
bool arp_self_test(void);

/* --- IPv4 ----------------------------------------------------------------- */

#define IP_HDR_LEN 20

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

/* Prepends an IPv4 header, resolves the next hop, and sends. The buffer's
 * `data` must point at the payload -- the transport header included, since to
 * IP that is payload. */
bool ip_transmit(ipv4_addr dst, u8 protocol, struct netbuf *b);

void ip_receive(struct net_device *dev, struct netbuf *b);
void ip_print_summary(void);
bool ip_self_test(void);

/* --- ICMP ----------------------------------------------------------------- */

void icmp_receive(struct net_device *dev, struct netbuf *b);

/* Sends an echo request and returns its identifier, or zero if it could not be
 * sent. `icmp_echo_seen` says whether a reply with that identifier has come
 * back -- split apart for the reason `recon_net`'s reach test was split on the
 * desktop: a machine that is not there takes the whole timeout to say so, and
 * blocking a caller for that long is a poor way to report it. */
u16 icmp_echo_send(ipv4_addr dst);
bool icmp_echo_seen(u16 id, u64 *rtt_ns);
void icmp_print_summary(void);

void eth_print_summary(void);

/* Retries anything held waiting for an ARP answer. Called after the stack has
 * processed frames, since one of them may have been the answer. */
void ip_flush_pending(void);

/* --- UDP ------------------------------------------------------------------ */

#define UDP_HDR_LEN 8

void udp_receive(struct net_device *dev, struct netbuf *b);
void udp_print_summary(void);
bool udp_self_test(void);

/* One port, one listener. A port already taken is refused rather than shared,
 * because two sockets quietly sharing one means one of them stops receiving
 * and nothing says which. */
bool udp_bind_port(u16 port, void (*fn)(void *ctx, struct netbuf *b), void *ctx);
void udp_unbind_port(u16 port);
bool udp_send(ipv4_addr dst, u16 dport, u16 sport, const void *data, u32 len);

/* --- TCP ------------------------------------------------------------------ */

#define TCP_HDR_LEN 20

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10
#define TCP_URG 0x20

/* The states, in the order a connection passes through them. Named as the
 * specification names them, because every diagram, packet trace and article
 * about TCP uses these words and inventing new ones would make all of that
 * material harder to apply to this code. */
enum tcp_state {
	TCP_CLOSED = 0,
	TCP_LISTEN,
	TCP_SYN_SENT,
	TCP_SYN_RECEIVED,
	TCP_ESTABLISHED,
	TCP_FIN_WAIT_1,
	TCP_FIN_WAIT_2,
	TCP_CLOSE_WAIT,
	TCP_CLOSING,
	TCP_LAST_ACK,
	TCP_TIME_WAIT,
};

const char *tcp_state_name(enum tcp_state s);

void tcp_receive(struct net_device *dev, struct netbuf *b);
void tcp_print_summary(void);
bool tcp_self_test(void);

/* Retransmission, and the expiry of TIME_WAIT. Driven from the same place the
 * receive queue is drained, so a machine receiving nothing still makes
 * progress on what it has sent. */
void tcp_tick(void);

/* What a socket asks of a connection. A connection is named by **index**
 * rather than by pointer: a socket holding a pointer across a close would be
 * holding freed memory the moment the slot was reused, and an index is checked
 * on every call. Negative means there was no room, or no entropy for an
 * initial sequence number -- which is refused rather than worked around. */
int tcp_open(ipv4_addr local, u16 lport, ipv4_addr remote, u16 rport);
int tcp_open_listener(ipv4_addr local, u16 port);
int tcp_accept_ready(int listener);
enum tcp_state tcp_state_of(int idx);
i64 tcp_write(int idx, const void *data, u32 len);
i64 tcp_read(int idx, void *data, u32 len);
void tcp_shutdown(int idx);

/* --- Drivers -------------------------------------------------------------- */

struct virtio_device;

/* Takes a probed virtio device and makes it a network card, if that is what it
 * is. False, quietly, for anything else. */
bool virtio_net_attach(const struct virtio_device *probed);
unsigned virtio_net_count(void);
void virtio_net_print_summary(void);

/* --- Sockets --------------------------------------------------------------- */

/* The BSD shape, which is the one every program already knows. Kept behind the
 * VFS's `file_ops` so that a socket is a descriptor like any other and `read`,
 * `write` and `close` need no branch about what they are talking to. */

#define SOCK_STREAM 1		/* TCP */
#define SOCK_DGRAM  2		/* UDP */

struct socket;

struct socket *socket_create(int type);
bool socket_bind(struct socket *s, ipv4_addr addr, u16 port);
bool socket_listen(struct socket *s, unsigned backlog);
struct socket *socket_accept(struct socket *s);
bool socket_connect(struct socket *s, ipv4_addr addr, u16 port);
i64 socket_send(struct socket *s, const void *data, u32 len);
i64 socket_recv(struct socket *s, void *data, u32 len);
i64 socket_sendto(struct socket *s, const void *data, u32 len,
		  ipv4_addr addr, u16 port);
i64 socket_recvfrom(struct socket *s, void *data, u32 len,
		    ipv4_addr *addr, u16 *port);
void socket_close(struct socket *s);

void socket_print_summary(void);
bool socket_self_test(void);

/* --- Configuration --------------------------------------------------------- */

/* Asks the wire for an address. Blocks for up to about two and a half seconds
 * while it does -- the one place in this stack that waits, because a machine
 * that has not got an address yet has nothing else to get on with.
 *
 * False when nothing answered, which is the ordinary outcome on a network with
 * no server and must not be a failure: a machine with a card and no address is
 * a machine that still boots. */
bool dhcp_configure(struct net_device *dev);
void dhcp_print_summary(void);

/* Brings up every card that was found and says on the console what happened.
 * Called once at boot. */
void net_bring_up(void);

/* --- The stack as a whole -------------------------------------------------- */

void net_init(void);
void net_print_summary(void);
bool net_self_test(void);

#endif /* RECON_KERNEL_NET_H */
