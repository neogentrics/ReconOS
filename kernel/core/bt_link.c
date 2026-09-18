/* Finding a device and connecting to it.
 *
 * See `bt_link.h` for the traps and for where the numbers were checked. This
 * file is the commands and the state.
 */
#include <recon/kernel/bt_link.h>
#include <recon/kernel/bluetooth.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>

static void put_le16(u8 *p, u16 v)
{
	p[0] = (u8)(v & 0xFFu);
	p[1] = (u8)(v >> 8);
}

static u16 get_le16(const u8 *p)
{
	return (u16)(p[0] | ((u16)p[1] << 8));
}

/* --- commands -------------------------------------------------------------
 *
 * Every multi-byte field goes out least significant byte first, including the
 * address and the access code, both of which are conventionally written the
 * other way round.
 */
u32 hci_inquiry_build(u8 *out, u8 length, u8 num_rsp)
{
	u8 body[5];

	/* Three bytes, little-endian: 0x9E8B33 becomes 33 8B 9E. */
	body[0] = (u8)(BT_GIAC & 0xFFu);
	body[1] = (u8)((BT_GIAC >> 8) & 0xFFu);
	body[2] = (u8)((BT_GIAC >> 16) & 0xFFu);
	body[3] = length;
	body[4] = num_rsp;

	return hci_command_build(out, HCI_OP_INQUIRY, body, sizeof(body));
}

u32 hci_create_connection_build(u8 *out, const u8 *addr, u16 pkt_type,
				u8 pscan_rep_mode, u16 clock_offset,
				bool allow_role_switch)
{
	u8 body[13];

	/* The address is already in wire order here -- an inquiry result hands
	 * it over that way, so copying it straight through is right and
	 * reversing it "back" would be the mistake. */
	kmemcpy(body, addr, BT_ADDR_LEN);

	put_le16(body + 6, pkt_type);
	body[8] = pscan_rep_mode;
	body[9] = 0;			/* reserved; was pscan_mode */
	put_le16(body + 10, clock_offset);
	body[12] = allow_role_switch ? 1u : 0u;

	return hci_command_build(out, HCI_OP_CREATE_CONN, body, sizeof(body));
}

u32 hci_disconnect_build(u8 *out, u16 handle, u8 reason)
{
	u8 body[3];

	put_le16(body, (u16)(handle & 0x0FFFu));
	body[2] = reason;

	return hci_command_build(out, HCI_OP_DISCONNECT, body, sizeof(body));
}

/* --- events --------------------------------------------------------------- */

bool hci_inquiry_result_parse(const u8 *ev, u32 len, unsigned n,
			      struct bt_inquiry_result *out)
{
	const u8 *p;
	u8 count;

	/* code, plen, then a count, then that many fourteen-byte entries. */
	if (len < 3 || ev[0] != HCI_EV_INQUIRY_RESULT)
		return false;

	count = ev[2];

	if (n >= count)
		return false;

	/* Refused rather than read short: an entry assembled from the end of
	 * the event and whatever follows it is an address. */
	if (3u + (u32)(count) * 14u > len)
		return false;

	p = ev + 3 + n * 14;

	kmemcpy(out->addr, p, BT_ADDR_LEN);
	out->pscan_rep_mode = p[6];
	/* p[7] is pscan_period_mode, p[8] pscan_mode -- neither is used. */
	out->device_class = (u32)p[9] | ((u32)p[10] << 8) | ((u32)p[11] << 16);
	out->clock_offset = get_le16(p + 12);

	return true;
}

/* --- the state machine ----------------------------------------------------- */

void bt_link_init(struct bt_link *l)
{
	kmemset(l, 0, sizeof(*l));
	l->state = BT_LINK_IDLE;
}

void bt_link_target(struct bt_link *l, const u8 *addr)
{
	kmemcpy(l->peer, addr, BT_ADDR_LEN);
	l->have_peer = true;
}

static bool same_address(const u8 *a, const u8 *b)
{
	unsigned i;

	for (i = 0; i < BT_ADDR_LEN; i++)
		if (a[i] != b[i])
			return false;

	return true;
}

u32 bt_link_event(struct bt_link *l, const u8 *ev, u32 len, u8 *out,
		  u32 outmax)
{
	if (len < HCI_EVENT_HEADER)
		return 0;

	switch (ev[0]) {
	case HCI_EV_COMMAND_STATUS: {
		u16 opcode;
		u8 status;

		if (!hci_event_command_result(ev, len, &opcode, &status))
			return 0;

		/* **This is where a link gets reported before it exists.**
		 *
		 * A Command Status for Create Connection means the controller
		 * accepted the request, not that the device answered. The
		 * state stays CONNECTING and the real answer arrives later as
		 * a Connection Complete.
		 *
		 * A *failing* status is different: the request was refused
		 * outright and no Connection Complete will follow, so waiting
		 * for one waits for ever. */
		if (opcode == HCI_OP_CREATE_CONN) {
			if (status) {
				l->state = BT_LINK_FAILED;
				l->failure = status;
			}

			return 0;
		}

		if (opcode == HCI_OP_INQUIRY && status) {
			l->state = BT_LINK_FAILED;
			l->failure = status;
		}

		return 0;
	}

	case HCI_EV_INQUIRY_RESULT: {
		struct bt_inquiry_result r;

		if (hci_inquiry_result_parse(ev, len, 0, &r)) {
			l->results++;

			/* Remembered so that Create Connection can carry them.
			 * A connection built with the wrong page scan
			 * repetition mode takes far longer to complete, which
			 * looks like a device that is not answering. */
			if (!l->have_peer) {
				kmemcpy(l->peer, r.addr, BT_ADDR_LEN);
				l->have_peer = true;
			}

			if (same_address(r.addr, l->peer)) {
				l->peer_pscan_rep_mode = r.pscan_rep_mode;
				l->peer_clock_offset = r.clock_offset;
			}
		}

		return 0;
	}

	case HCI_EV_INQUIRY_COMPLETE:
		if (l->state != BT_LINK_INQUIRING)
			return 0;

		if (!l->have_peer) {
			l->state = BT_LINK_IDLE;
			return 0;
		}

		l->state = BT_LINK_CONNECTING;

		if (outmax < 3 + 13)
			return 0;

		return hci_create_connection_build(out, l->peer,
						   HCI_PKT_TYPE_DEFAULT,
						   l->peer_pscan_rep_mode,
						   l->peer_clock_offset,
						   true);

	case HCI_EV_CONN_COMPLETE: {
		/* status, handle, bdaddr, link_type, encr_mode. */
		if (len < 2 + 11)
			return 0;

		if (l->state != BT_LINK_CONNECTING)
			return 0;

		/* **Status before handle, and that order is the point.** A
		 * failed Connection Complete still carries a handle field,
		 * and it means nothing. Reading it first gives a handle the
		 * controller never issued. */
		if (ev[2]) {
			l->state = BT_LINK_FAILED;
			l->failure = ev[2];
			return 0;
		}

		/* The address this is about. A Connection Complete for
		 * another device is not this link's answer -- the same rule
		 * as the opcode match and the L2CAP identifier. */
		if (l->have_peer && !same_address(ev + 5, l->peer)) {
			l->wrong_address++;
			return 0;
		}

		/* Not an ACL link. A SCO connection completing here is a
		 * voice channel, and driving L2CAP over it would put PDUs
		 * somewhere they cannot go. */
		if (ev[11] != HCI_LINK_TYPE_ACL)
			return 0;

		/* Twelve bits. The event carries sixteen and the top four
		 * are not part of the handle. */
		l->handle = (u16)(get_le16(ev + 3) & 0x0FFFu);
		l->state = BT_LINK_UP;

		return 0;
	}

	case HCI_EV_DISCONN_COMPLETE:
		if (len < 2 + 4)
			return 0;

		if (ev[2])			/* the disconnect itself failed */
			return 0;

		if (l->state == BT_LINK_UP &&
		    (get_le16(ev + 3) & 0x0FFFu) == l->handle) {
			l->state = BT_LINK_IDLE;
			l->handle = 0;
		}

		return 0;

	case HCI_EV_PIN_CODE_REQ:
	case HCI_EV_LINK_KEY_REQ:
		/* Pairing, which is not implemented.
		 *
		 * Reported rather than ignored: a controller that asked for a
		 * key and got no reply waits, and so does everything above.
		 * A state saying *this device wants pairing* is a diagnosis;
		 * silence is a hang. */
		l->state = BT_LINK_NEEDS_PAIRING;
		return 0;

	default:
		return 0;
	}
}

/* --- the self-test --------------------------------------------------------
 *
 * The cases are the two byte orders, the early answer, and the matching rule:
 *
 *   - the access code and the address go out least significant byte first,
 *     and both are conventionally written the other way;
 *   - a Command Status for Create Connection is **not** a connection;
 *   - a failing Command Status is, because no Connection Complete follows;
 *   - a Connection Complete carries its status before its handle;
 *   - one for another address is not this link's;
 *   - the handle is twelve bits of the sixteen carried.
 */
bool bt_link_self_test(void)
{
	struct bt_link l;
	u8 out[32];
	u32 n;
	bool ok = true;

	/* As printed: 00:11:22:33:44:55. On the wire, backwards. */
	static const u8 addr[BT_ADDR_LEN] = {
		0x55, 0x44, 0x33, 0x22, 0x11, 0x00
	};

	/* --- the access code, little-endian ------------------------------ */
	n = hci_inquiry_build(out, 8, 0);

	if (n != 8 || out[0] != 0x01 || out[1] != 0x04) {
		kprintf("  btlink: Inquiry came out as %u bytes opcode "
			"%02x%02x, expected 8 and 0401\n", n, out[1], out[0]);
		ok = false;
	}

	if (out[3] != 0x33 || out[4] != 0x8B || out[5] != 0x9E) {
		kprintf("  btlink: the inquiry access code went out as "
			"%02x %02x %02x, expected 33 8b 9e -- 0x9E8B33 "
			"least significant byte first\n", out[3], out[4],
			out[5]);
		ok = false;
	}

	/* --- the address, also little-endian ----------------------------- */
	n = hci_create_connection_build(out, addr, HCI_PKT_TYPE_DEFAULT, 1,
					0, true);

	if (n != 16 || out[0] != 0x05 || out[1] != 0x04) {
		kprintf("  btlink: Create Connection came out as %u bytes "
			"opcode %02x%02x, expected 16 and 0405\n", n, out[1],
			out[0]);
		ok = false;
	}

	if (out[3] != 0x55 || out[8] != 0x00) {
		kprintf("  btlink: the address went out starting %02x and "
			"ending %02x, expected 55 and 00 -- a BD_ADDR is "
			"written 00:11:22:33:44:55 and sent backwards\n",
			out[3], out[8]);
		ok = false;
	}

	if (out[9] != 0x18 || out[10] != 0xCC) {
		kprintf("  btlink: the packet types went out as %02x %02x, "
			"expected 18 cc\n", out[9], out[10]);
		ok = false;
	}

	/* --- a Command Status is not a connection ------------------------ */
	{
		u8 ev[6];

		bt_link_init(&l);
		bt_link_target(&l, addr);
		l.state = BT_LINK_CONNECTING;

		ev[0] = HCI_EV_COMMAND_STATUS;
		ev[1] = 4;
		ev[2] = 0x00;			/* status: accepted */
		ev[3] = 1;			/* credits */
		ev[4] = 0x05;			/* opcode: Create Connection */
		ev[5] = 0x04;

		bt_link_event(&l, ev, sizeof(ev), out, sizeof(out));

		if (l.state != BT_LINK_CONNECTING) {
			kprintf("  btlink: a Command Status for Create "
				"Connection moved the link to state %u -- it "
				"means the controller accepted the request, "
				"not that the device answered\n",
				(unsigned)l.state);
			ok = false;
		}

		/* A failing one is different: nothing follows it. */
		ev[2] = 0x0C;			/* command disallowed */
		bt_link_event(&l, ev, sizeof(ev), out, sizeof(out));

		if (l.state != BT_LINK_FAILED || l.failure != 0x0C) {
			kputs("  btlink: a failing Command Status left the "
			      "link waiting for a Connection Complete that "
			      "will never come\n");
			ok = false;
		}
	}

	/* --- Connection Complete: status first --------------------------- */
	{
		u8 ev[13];

		kmemset(ev, 0, sizeof(ev));
		ev[0] = HCI_EV_CONN_COMPLETE;
		ev[1] = 11;
		ev[2] = 0x04;			/* page timeout */
		ev[3] = 0x0C;			/* a handle, meaning nothing */
		ev[4] = 0x00;
		kmemcpy(ev + 5, addr, BT_ADDR_LEN);
		ev[11] = HCI_LINK_TYPE_ACL;

		bt_link_init(&l);
		bt_link_target(&l, addr);
		l.state = BT_LINK_CONNECTING;

		bt_link_event(&l, ev, sizeof(ev), out, sizeof(out));

		if (l.state != BT_LINK_FAILED || l.handle != 0) {
			kprintf("  btlink: a failed Connection Complete left "
				"state %u handle %04x -- the handle field is "
				"present and means nothing when the status is "
				"non-zero\n", (unsigned)l.state, l.handle);
			ok = false;
		}

		/* Now a successful one, but for somebody else. */
		bt_link_init(&l);
		bt_link_target(&l, addr);
		l.state = BT_LINK_CONNECTING;

		ev[2] = 0x00;
		ev[5] = 0x99;			/* a different address */

		bt_link_event(&l, ev, sizeof(ev), out, sizeof(out));

		if (l.state == BT_LINK_UP || l.wrong_address != 1) {
			kputs("  btlink: a Connection Complete for another "
			      "device brought this link up\n");
			ok = false;
		}

		/* And the real one. The handle has rubbish in its top four
		 * bits, which are not part of it. */
		kmemcpy(ev + 5, addr, BT_ADDR_LEN);
		ev[3] = 0x0C;
		ev[4] = 0xF0;			/* top nibble set */

		bt_link_event(&l, ev, sizeof(ev), out, sizeof(out));

		if (l.state != BT_LINK_UP) {
			kprintf("  btlink: a good Connection Complete left "
				"state %u\n", (unsigned)l.state);
			ok = false;
		}

		if (l.handle != 0x000C) {
			kprintf("  btlink: the handle came out %04x, expected "
				"000c -- it is twelve bits of the sixteen "
				"carried\n", l.handle);
			ok = false;
		}

		/* A SCO link completing must not be taken for the ACL one. */
		bt_link_init(&l);
		bt_link_target(&l, addr);
		l.state = BT_LINK_CONNECTING;
		ev[11] = 0x00;			/* SCO */

		bt_link_event(&l, ev, sizeof(ev), out, sizeof(out));

		if (l.state == BT_LINK_UP) {
			kputs("  btlink: a SCO connection was taken for the "
			      "ACL link, and L2CAP cannot run on it\n");
			ok = false;
		}
	}

	/* --- inquiry results, and what they feed -------------------------- */
	{
		u8 ev[3 + 14];

		kmemset(ev, 0, sizeof(ev));
		ev[0] = HCI_EV_INQUIRY_RESULT;
		ev[1] = 1 + 14;
		ev[2] = 1;			/* one response */
		kmemcpy(ev + 3, addr, BT_ADDR_LEN);
		ev[3 + 6] = 0x01;		/* pscan_rep_mode R1 */
		ev[3 + 9] = 0x80;		/* class, low byte */
		ev[3 + 10] = 0x05;
		ev[3 + 12] = 0x34;		/* clock offset */
		ev[3 + 13] = 0x12;

		{
			struct bt_inquiry_result r;

			if (!hci_inquiry_result_parse(ev, sizeof(ev), 0, &r)) {
				kputs("  btlink: an inquiry result with one "
				      "entry did not parse\n");
				ok = false;
			} else {
				if (r.addr[0] != 0x55 || r.addr[5] != 0x00) {
					kputs("  btlink: the inquiry result's "
					      "address came back reversed\n");
					ok = false;
				}

				if (r.device_class != 0x000580) {
					kprintf("  btlink: class of device "
						"%06x, expected 000580\n",
						r.device_class);
					ok = false;
				}

				if (r.clock_offset != 0x1234) {
					kprintf("  btlink: clock offset %04x, "
						"expected 1234\n",
						r.clock_offset);
					ok = false;
				}
			}

			/* A second entry that is not there. */
			if (hci_inquiry_result_parse(ev, sizeof(ev), 1, &r)) {
				kputs("  btlink: a second inquiry entry was "
				      "read out of an event carrying one\n");
				ok = false;
			}

			/* An event claiming more entries than it holds. */
			ev[2] = 4;

			if (hci_inquiry_result_parse(ev, sizeof(ev), 0, &r)) {
				kputs("  btlink: an inquiry result claiming "
				      "four entries in room for one was "
				      "read\n");
				ok = false;
			}

			ev[2] = 1;
		}

		/* Inquiry Complete must draw a Create Connection carrying
		 * what the result reported. */
		bt_link_init(&l);
		l.state = BT_LINK_INQUIRING;
		bt_link_event(&l, ev, sizeof(ev), out, sizeof(out));

		{
			u8 done[2];

			done[0] = HCI_EV_INQUIRY_COMPLETE;
			done[1] = 0;

			n = bt_link_event(&l, done, sizeof(done), out,
					  sizeof(out));
		}

		if (n != 16 || out[0] != 0x05 || out[1] != 0x04) {
			kprintf("  btlink: Inquiry Complete drew %u bytes of "
				"opcode %02x%02x, expected a 16-byte Create "
				"Connection\n", n, out[1], out[0]);
			ok = false;
		} else if (out[11] != 0x01) {
			kprintf("  btlink: the connection carried page scan "
				"repetition mode %u, expected the 1 the "
				"inquiry reported -- a wrong one makes the "
				"device look slow to answer\n", out[11]);
			ok = false;
		}

		if (l.state != BT_LINK_CONNECTING) {
			kputs("  btlink: the link did not move to connecting "
			      "after inquiry finished\n");
			ok = false;
		}
	}

	/* --- pairing is reported, not waited on --------------------------- */
	{
		u8 ev[8];

		kmemset(ev, 0, sizeof(ev));
		ev[0] = HCI_EV_LINK_KEY_REQ;
		ev[1] = 6;
		kmemcpy(ev + 2, addr, BT_ADDR_LEN);

		bt_link_init(&l);
		l.state = BT_LINK_CONNECTING;
		bt_link_event(&l, ev, sizeof(ev), out, sizeof(out));

		if (l.state != BT_LINK_NEEDS_PAIRING) {
			kputs("  btlink: a link key request was ignored; the "
			      "controller waits for a reply and so does "
			      "everything above it\n");
			ok = false;
		}
	}

	return ok;
}
