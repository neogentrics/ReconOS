/* The stack, assembled: what runs at boot, what it says, and what it proves.
 *
 * Each layer below is testable on its own and is tested on its own. This file
 * is the one that puts a packet through all of them, because a stack whose
 * layers each pass in isolation can still fail at every join -- and the joins
 * are where the byte order, the lengths and the checksum spans actually meet.
 */
#include <recon/kernel/net.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/console.h>
#include <recon/kernel/time.h>

bool icmp_reply_in_place_test(void);

/* Brings the cards up, and says plainly what came back.
 *
 * This is the first thing in the kernel that talks to a machine it did not
 * start, over a wire, and is answered. Every layer below can be proved against
 * itself; this cannot be, which is exactly why it **reports what happened**
 * rather than asserting what should have. A line here that says nothing
 * answered is a true line about a real network, not a failure.
 */
void net_bring_up(void)
{
	unsigned i;

	for (i = 0; i < netdev_count(); i++) {
		struct net_device *d = netdev_at(i);
		char a[20];

		if (!d || !d->up)
			continue;

		if (!dhcp_configure(d)) {
			kprintf("net: %s has no address; nothing offered one\n",
				d->name);
			continue;
		}

		kprintf("net: %s is %s", d->name, ipv4_format(d->ip, a));

		if (d->gateway)
			kprintf(", via %s", ipv4_format(d->gateway, a));

		kprintf("\n");

		/* And now the one claim in this stack that no self-test can
		 * make: something out there answered. */
		if (d->gateway) {
			u16 id = icmp_echo_send(d->gateway);

			if (id) {
				u64 until = time_monotonic_ns() + 500000000ull;
				u64 rtt = 0;

				while (time_monotonic_ns() < until) {
					netdev_service();
					ip_flush_pending();

					if (icmp_echo_seen(id, &rtt))
						break;
				}

				if (icmp_echo_seen(id, &rtt))
					kprintf("net: the gateway answered in "
						"%u us\n",
						(unsigned)(rtt / 1000));
				else
					kprintf("net: nothing answered the "
						"echo -- configured, and "
						"unproven\n");
			}
		}
	}
}

void net_print_summary(void)
{
	kputs("Network\n");

	netdev_print_summary();

	if (virtio_net_count())
		virtio_net_print_summary();

	dhcp_print_summary();
	netbuf_print_summary();
	eth_print_summary();
	arp_print_summary();
	ip_print_summary();
	icmp_print_summary();
	udp_print_summary();
	tcp_print_summary();
	socket_print_summary();
}

/* --- The test that crosses every layer ------------------------------------- */

/* A device that keeps the frame it was given, so the whole outbound path can
 * be walked and then read back byte by byte.
 *
 * This is what makes the stack testable on a machine with no network card --
 * which is every machine in the verification matrix. A test that prints "no
 * card on this machine" and counts as passing is the fault BG-187 was about,
 * and a stack whose only test needs hardware is exactly that fault waiting to
 * happen. */
static struct netbuf *captured;
static unsigned capture_count;

static bool capture_transmit(struct net_device *dev, struct netbuf *b)
{
	(void)dev;

	netbuf_free(captured);
	captured = b;
	capture_count++;
	return true;
}

static const struct net_device_ops capture_ops = {
	.transmit = capture_transmit,
	.enable_interrupts = NULL,
	.poll = NULL,
};

bool net_self_test(void)
{
	static const struct mac_addr our_mac = { { 0x02, 0xAB, 0, 0, 0, 1 } };
	static const struct mac_addr peer_mac = { { 0x02, 0xAB, 0, 0, 0, 2 } };
	struct net_device *dev;
	bool ok = true;

	net_init();

	if (!netbuf_self_test())
		ok = false;

	if (!netdev_self_test())
		ok = false;

	if (!arp_self_test())
		ok = false;

	if (!ip_self_test())
		ok = false;

	if (!udp_self_test())
		ok = false;

	if (!tcp_self_test())
		ok = false;

	if (!socket_self_test())
		ok = false;

	if (!icmp_reply_in_place_test())
		ok = false;

	/* --- And now one packet, all the way down and all the way up ------ */

	dev = netdev_register("cap0", &capture_ops, NULL, &our_mac);

	if (!dev) {
		kprintf("net: could not register the capture device\n");
		return false;
	}

	dev->ip = IPV4(192, 168, 7, 2);
	dev->netmask = IPV4(255, 255, 255, 0);
	dev->gateway = IPV4(192, 168, 7, 1);

	/* Teach ARP the peer's address, so that the send completes rather than
	 * being held waiting for a reply nothing will send. */
	{
		struct netbuf *reply = netbuf_alloc();

		if (reply) {
			u8 *a = netbuf_put(reply, 28);

			if (a) {
				net_put16(a + 0, 1);		/* ethernet */
				net_put16(a + 2, ETH_TYPE_IPV4);
				a[4] = MAC_LEN;
				a[5] = 4;
				net_put16(a + 6, 2);		/* reply */
				kmemcpy(a + 8, peer_mac.b, MAC_LEN);
				net_put32(a + 14, IPV4(192, 168, 7, 9));
				kmemcpy(a + 18, our_mac.b, MAC_LEN);
				net_put32(a + 24, dev->ip);

				arp_receive(dev, reply);
			} else {
				netbuf_free(reply);
			}
		}
	}

	capture_count = 0;

	if (!udp_send(IPV4(192, 168, 7, 9), 7777, 4444, "reconos", 7)) {
		kprintf("net: a datagram could not be sent\n");
		ok = false;
	}

	if (!capture_count) {
		kprintf("net: nothing reached the device\n");
		ok = false;
	} else if (captured) {
		const u8 *f = captured->data;
		u32 flen = captured->len;

		/* Ethernet: to the peer, from us, carrying IPv4. Checked on
		 * the bytes as they would go out, not on a structure -- the
		 * wire is bytes, and a struct with the wrong packing agrees
		 * with itself. */
		if (flen < ETH_HDR_LEN + IP_HDR_LEN + UDP_HDR_LEN + 7) {
			kprintf("net: the frame is %u bytes, too short\n",
				(unsigned)flen);
			ok = false;
		} else {
			if (kmemcmp(f, peer_mac.b, MAC_LEN) != 0) {
				kprintf("net: the frame is not addressed to "
					"the peer\n");
				ok = false;
			}

			if (kmemcmp(f + 6, our_mac.b, MAC_LEN) != 0) {
				kprintf("net: the frame is not from us\n");
				ok = false;
			}

			if (net_get16(f + 12) != ETH_TYPE_IPV4) {
				kprintf("net: the ethertype is %u\n",
					(unsigned)net_get16(f + 12));
				ok = false;
			}

			/* IPv4: version, protocol, addresses, and a checksum
			 * that a receiver would accept. */
			{
				const u8 *ip = f + ETH_HDR_LEN;

				if ((ip[0] >> 4) != 4) {
					kprintf("net: not an IPv4 packet\n");
					ok = false;
				}

				if (ip[9] != IP_PROTO_UDP) {
					kprintf("net: protocol %u, not UDP\n",
						(unsigned)ip[9]);
					ok = false;
				}

				if (net_get32(ip + 12) != dev->ip ||
				    net_get32(ip + 16) != IPV4(192, 168, 7, 9)) {
					kprintf("net: the addresses are wrong\n");
					ok = false;
				}

				if (net_checksum(ip, IP_HDR_LEN) != 0) {
					kprintf("net: the IP checksum would be "
						"rejected\n");
					ok = false;
				}

				/* UDP: ports, length, and the checksum with
				 * the pseudo-header -- which is the join most
				 * likely to be wrong, because it is the one
				 * that reaches across two layers. */
				{
					const u8 *u = ip + IP_HDR_LEN;
					u8 ph[12];
					u32 sum;

					if (net_get16(u + 0) != 4444 ||
					    net_get16(u + 2) != 7777) {
						kprintf("net: the ports are "
							"wrong\n");
						ok = false;
					}

					if (net_get16(u + 4) != UDP_HDR_LEN + 7) {
						kprintf("net: the UDP length "
							"is wrong\n");
						ok = false;
					}

					if (kmemcmp(u + UDP_HDR_LEN,
						    "reconos", 7) != 0) {
						kprintf("net: the payload did "
							"not survive\n");
						ok = false;
					}

					net_put32(ph + 0, dev->ip);
					net_put32(ph + 4,
						  IPV4(192, 168, 7, 9));
					ph[8] = 0;
					ph[9] = IP_PROTO_UDP;
					net_put16(ph + 10, UDP_HDR_LEN + 7);

					sum = net_checksum_partial(ph,
								   sizeof(ph), 0);
					sum = net_checksum_partial(u,
								   UDP_HDR_LEN + 7,
								   sum);

					if (net_checksum_finish(sum) != 0) {
						kprintf("net: the UDP checksum "
							"would be rejected\n");
						ok = false;
					}
				}
			}
		}
	}

	/* And back up: the same frame, fed to the receive path, must arrive at
	 * a socket bound to that port with its payload and sender intact.
	 *
	 * This is the assertion the whole file is for. Send and receive can
	 * each be self-consistently wrong -- a stack with the byte order
	 * reversed in both directions passes every test above. Feeding a frame
	 * this kernel built back through the path a card would use, and
	 * requiring it to come out of a socket, is what catches that. */
	if (captured) {
		struct socket *s = socket_create(SOCK_DGRAM);

		if (!s || !socket_bind(s, IPV4_ANY, 7777)) {
			kprintf("net: could not bind the receiving socket\n");
			ok = false;
		} else {
			struct netbuf *in = netbuf_alloc();

			if (in) {
				u8 *p = netbuf_put(in, captured->len);

				if (p) {
					char got[16];
					ipv4_addr from = 0;
					u16 port = 0;
					i64 n;

					kmemcpy(p, captured->data,
						captured->len);

					/* Addressed to us, so the Ethernet
					 * filter accepts it. */
					kmemcpy(p, our_mac.b, MAC_LEN);

					/* And aimed at our own address, so IP
					 * accepts it rather than dropping it
					 * as somebody else's. */
					net_put32(p + ETH_HDR_LEN + 16,
						  dev->ip);
					net_put16(p + ETH_HDR_LEN + 10, 0);
					net_put16(p + ETH_HDR_LEN + 10,
						  net_checksum(p + ETH_HDR_LEN,
							       IP_HDR_LEN));

					/* The UDP checksum now covers a
					 * destination address that changed, so
					 * it is zeroed -- which IPv4 permits
					 * and means "not computed". Recomputing
					 * it here would be this test checking
					 * its own arithmetic rather than the
					 * receive path. */
					net_put16(p + ETH_HDR_LEN + IP_HDR_LEN
						  + 6, 0);

					eth_receive(dev, in);

					kmemset(got, 0, sizeof(got));
					n = socket_recvfrom(s, got,
							    sizeof(got) - 1,
							    &from, &port);

					if (n != 7) {
						kprintf("net: the socket got "
							"%d bytes, expected 7\n",
							(int)n);
						ok = false;
					} else if (kmemcmp(got, "reconos",
							   7) != 0) {
						kprintf("net: the payload "
							"changed on the way "
							"up\n");
						ok = false;
					}

					if (port != 4444) {
						kprintf("net: the sender's "
							"port came back as "
							"%u\n", (unsigned)port);
						ok = false;
					}
				} else {
					netbuf_free(in);
				}
			}
		}

		if (s)
			socket_close(s);
	}

	netbuf_free(captured);
	captured = NULL;

	/* And the capture device goes away again, so the boot summary lists
	 * only cards that are really there. */
	netdev_forget_last();

	return ok;
}
