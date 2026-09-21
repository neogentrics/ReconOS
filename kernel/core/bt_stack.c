/* Running the whole sequence.
 *
 * See `bt_stack.h` for the order and for why no descriptor is fetched. This
 * file is the sequencing, which is the part the individual state machines
 * cannot check about themselves.
 */
#include <recon/kernel/bt_hid.h>
#include <recon/kernel/bt_stack.h>
#include <recon/kernel/console.h>
#include <recon/kernel/hid_report.h>
#include <recon/kernel/kstring.h>

static void fail(struct bt_stack *s, const char *why)
{
	/* First reason wins, as in `hid_report.c`: the later ones are usually
	 * consequences, and a message naming the third thing that went wrong
	 * sends the reader to the wrong place. */
	if (s->state != BT_ST_FAILED) {
		s->state = BT_ST_FAILED;
		s->failure = why;
	}
}

/* Defined below, beside the poll that leads to it. */
static void protocol_set(struct bt_stack *s);

void bt_stack_init(struct bt_stack *s)
{
	kmemset(s, 0, sizeof(*s));

	s->state = BT_ST_IDLE;
	bt_link_init(&s->link);
	bt_pairing_init(&s->pairing);
	s->next_ident = 1;
}

u32 bt_stack_start(struct bt_stack *s, const u8 *addr, u8 *out, u32 outmax)
{
	if (outmax < 8)
		return 0;

	if (addr) {
		bt_link_target(&s->link, addr);

		/* **The pairing window opens here and nowhere else.**
		 *
		 * Pairing is invited for exactly the device this sequence was
		 * asked to find. Starting without an address inquires for
		 * anything, and in that case no window is opened at all --
		 * there is no named device to consent on behalf of. */
		bt_pairing_allow(&s->pairing, addr);
	}

	s->state = BT_ST_RESETTING;
	s->commands_sent++;

	return hci_command_build(out, HCI_OP_RESET, 0, 0);
}

/* The control channel first, then the interrupt one. */
static u32 open_channel(struct bt_stack *s, struct l2cap_channel *ch, u16 psm,
			u16 scid, u8 *out, u32 outmax)
{
	if (outmax < 8)
		return 0;

	l2cap_channel_init(ch, psm, scid);

	return l2cap_channel_start(ch, out, s->next_ident++);
}

u32 bt_stack_event(struct bt_stack *s, const u8 *ev, u32 len, u8 *out,
		   u32 outmax)
{
	u32 n;

	if (len < HCI_EVENT_HEADER)
		return 0;

	/* Pairing first, and unconditionally.
	 *
	 * Its events arrive whenever the controller decides, not when this
	 * sequence expects them -- a link key request can land between the
	 * connection completing and the first channel opening. Routing them
	 * by state would drop the ones that arrive at an inconvenient moment,
	 * and a dropped pairing request is a controller left waiting. */
	n = bt_pairing_event(&s->pairing, ev, len, out, outmax);

	if (n) {
		s->commands_sent++;
		return n;
	}

	if (s->pairing.state == BT_PAIR_FAILED)
		fail(s, "pairing was refused by the device");

	/* Command Complete for the reset that started everything. */
	if (ev[0] == HCI_EV_COMMAND_COMPLETE && s->state == BT_ST_RESETTING) {
		u16 opcode;
		u8 status;

		if (hci_event_command_result(ev, len, &opcode, &status) &&
		    opcode == HCI_OP_RESET) {
			if (status) {
				fail(s, "the controller refused to reset");
				return 0;
			}

			/* **Eight bytes, and this path was not checking.**
			 *
			 * The builders take no size -- the caller guarantees
			 * the room -- and every other call site here does
			 * guard. This one did not, found by listing them
			 * rather than by a test, and it is the worst kind of
			 * omission to leave to a test: nothing fails, a few
			 * bytes past the end are overwritten, and what breaks
			 * is whatever was living there. */
			if (outmax < 8)
				return 0;

			s->state = BT_ST_INQUIRING;
			s->link.state = BT_LINK_INQUIRING;
			s->commands_sent++;

			return hci_inquiry_build(out, 8, 0);
		}

		return 0;
	}

	/* Everything else about the link belongs to the link layer. */
	n = bt_link_event(&s->link, ev, len, out, outmax);

	if (n) {
		s->commands_sent++;

		if (s->link.state == BT_LINK_CONNECTING)
			s->state = BT_ST_CONNECTING;

		return n;
	}

	if (s->link.state == BT_LINK_FAILED) {
		fail(s, "the link could not be brought up");
		return 0;
	}

	/* **The link went away, and nothing above had noticed.**
	 *
	 * `bt_link_event` handles a Disconnection Complete by putting the
	 * link back to idle and forgetting its handle. Nothing here looked
	 * for that, so the sequence stayed at RUNNING with a mouse still
	 * marked ready on a handle the controller had reclaimed.
	 *
	 * That is not merely stale. **Handles are reused.** The next
	 * connection can be given the same number, and the mouse -- still
	 * configured with the previous device's layout and channel -- would
	 * decode that device's data as though it were the old one's. A
	 * pointer moving from somebody else's traffic.
	 *
	 * Found by asking what happens the *second* time each state machine
	 * runs; every test here had only ever run one. */
	if (s->state > BT_ST_CONNECTING && s->state != BT_ST_FAILED &&
	    s->link.state != BT_LINK_UP) {
		u8 peer[BT_ADDR_LEN];
		bool had_peer = s->link.have_peer;
		struct bt_pairing keep = s->pairing;

		kmemcpy(peer, s->link.peer, BT_ADDR_LEN);

		/* Everything the link carried is gone with it: both channels
		 * and the mouse. Torn down rather than reset in place, so a
		 * field added later cannot be forgotten here. */
		bt_stack_init(s);

		/* The pairing survives, because the link key does. Losing it
		 * would make a known device pair again on every
		 * disconnection, which is the opposite of what pairing is
		 * for. */
		s->pairing = keep;
		s->pairing.window_open = false;

		if (had_peer)
			bt_link_target(&s->link, peer);

		return 0;
	}

	if (s->link.state == BT_LINK_NEEDS_PAIRING &&
	    !s->pairing.window_open) {
		fail(s, "the device wants pairing and none was invited");
		return 0;
	}

	/* The link came up. Open the control channel -- **control before
	 * interrupt**, because the protocol is set on control and a device
	 * may not answer reports requests before it is configured. */
	if (s->link.state == BT_LINK_UP && s->state == BT_ST_CONNECTING) {
		s->state = BT_ST_OPENING_CONTROL;
		s->commands_sent++;

		return open_channel(s, &s->control, L2CAP_PSM_HID_CONTROL,
				    BT_CID_CONTROL, out, outmax);
	}

	return 0;
}

/* An L2CAP PDU, wrapped for the link. */
static u32 wrap_pdu(u8 *out, u32 outmax, u16 cid, const u8 *payload, u32 len)
{
	if (outmax < L2CAP_HEADER + len)
		return 0;

	l2cap_header_build(out, cid, (u16)len);
	kmemcpy(out + L2CAP_HEADER, payload, len);

	return L2CAP_HEADER + len;
}

u32 bt_stack_acl(struct bt_stack *s, u16 handle, u8 pb, const u8 *payload,
		 u32 len, u8 *out, u32 outmax, bool *moved,
		 struct hid_mouse_state *state)
{
	struct l2cap_header hdr;
	struct l2cap_reassembly *rx = &s->mouse.rx;
	u8 reply[64];
	u32 n;

	*moved = false;

	if (handle != s->link.handle)
		return 0;

	/* **Fed exactly once, here, and nowhere else.**
	 *
	 * One buffer serves every channel on this link -- signalling, the
	 * control channel and the reports -- because they are one stream of
	 * fragments and two reassemblies would disagree about which fragment
	 * is in flight.
	 *
	 * The first version let `bt_mouse_acl` feed it as well, and then the
	 * guard below was written `state != RUNNING && !feed(...)`. Once
	 * running, that condition short-circuits before the feed and the
	 * early return never happens -- so an *incomplete* PDU fell straight
	 * through to the parsing below, which read `want` bytes out of a
	 * buffer holding fewer, and acted on whatever the last PDU had left
	 * there.
	 *
	 * There is one feed now, and nothing past it runs until it says the
	 * PDU is whole. `bt_mouse_pdu` takes the finished buffer. */
	if (!l2cap_acl_feed(rx, handle, pb, payload, len))
		return 0;

	if (!l2cap_header_parse(rx->buf, rx->want, &hdr))
		return 0;

	/* Reports, once there is a mouse to decode them. */
	if (s->state == BT_ST_RUNNING) {
		if (bt_mouse_pdu(&s->mouse, rx->buf, rx->want, state)) {
			*moved = true;
			s->reports++;
			return 0;
		}
	}

	/* The control channel, carrying the answer to SET_PROTOCOL. */
	if (hdr.cid == BT_CID_CONTROL && s->state == BT_ST_SETTING_PROTOCOL) {
		struct bt_hid_message msg;

		if (!bt_hid_parse(rx->buf + L2CAP_HEADER, hdr.length, &msg))
			return 0;

		if (msg.transaction != HIDP_TRANS_HANDSHAKE)
			return 0;

		if (bt_hid_handshake_ok(msg.parameter)) {
			protocol_set(s);
			return 0;
		}

		/* **Retryable is neither**, and this is where that
		 * distinction earns itself: NOT_READY means ask again, and
		 * treating it as a refusal abandons a device that was about
		 * to work. */
		if (bt_hid_handshake_retryable(msg.parameter))
			return 0;

		/* A refusal. The device does not do boot protocol, which is
		 * allowed -- and means it needs its report descriptor, which
		 * needs SDP, which is not written. Reported rather than
		 * retried for ever. */
		fail(s, "the device refused boot protocol and SDP is not "
			"implemented");
		return 0;
	}

	if (hdr.cid != L2CAP_CID_SIGNALLING)
		return 0;

	/* Signalling, for whichever channel it names. Offered to both,
	 * because a response carries this side's CID and each channel
	 * refuses what is not its own -- which is the matching rule those
	 * channels were given, doing its job here. */
	n = l2cap_channel_input(&s->control, rx->buf + L2CAP_HEADER,
				hdr.length, reply, sizeof(reply));

	if (!n)
		n = l2cap_channel_input(&s->interrupt,
					rx->buf + L2CAP_HEADER, hdr.length,
					reply, sizeof(reply));

	/* A reply, and nothing else. **Whatever the sequence owes next is
	 * `bt_stack_poll`'s**, because this can return one thing and there
	 * are two -- see the note on that function. */
	if (n)
		return wrap_pdu(out, outmax, L2CAP_CID_SIGNALLING, reply, n);

	return 0;
}

u32 bt_stack_poll(struct bt_stack *s, u8 *out, u32 outmax)
{
	u8 reply[64];
	u32 n;

	/* The control channel finished. Open the interrupt one. */
	if (s->state == BT_ST_OPENING_CONTROL &&
	    s->control.state == L2CAP_CH_OPEN) {
		s->state = BT_ST_OPENING_INTERRUPT;

		n = open_channel(s, &s->interrupt, L2CAP_PSM_HID_INTERRUPT,
				 BT_CID_INTERRUPT, reply, sizeof(reply));

		if (n)
			return wrap_pdu(out, outmax, L2CAP_CID_SIGNALLING,
					reply, n);
	}

	/* Both channels up. Ask for boot protocol on the **control** one --
	 * sending it on the interrupt channel is the mistake the fixed
	 * channel order exists to make visible. */
	if (s->state == BT_ST_OPENING_INTERRUPT &&
	    s->interrupt.state == L2CAP_CH_OPEN) {
		u8 sp[1];

		s->state = BT_ST_SETTING_PROTOCOL;

		sp[0] = bt_hid_header(HIDP_TRANS_SET_PROTOCOL,
				      HIDP_PROTO_BOOT);

		return wrap_pdu(out, outmax, s->control.dcid, sp, 1);
	}

	return 0;
}

/* The device accepted boot protocol. Configure the mouse from the fixed
 * layout and start reading reports. */
static void protocol_set(struct bt_stack *s)
{
	struct hid_mouse_layout boot;

	hid_mouse_boot_layout(&boot, true);

	kmemset(&s->mouse, 0, sizeof(s->mouse));
	l2cap_reassembly_reset(&s->mouse.rx);
	s->mouse.layout = boot;
	s->mouse.uses_report_id = false;
	s->mouse.acl_handle = s->link.handle;
	s->mouse.intr_cid = BT_CID_INTERRUPT;
	s->mouse.ready = true;

	s->state = BT_ST_RUNNING;
}

/* --- the self-test --------------------------------------------------------
 *
 * The whole sequence, driven by handing it the events a controller would
 * send. What is checked is the **order** and the handoffs, since every
 * individual machine is already covered:
 *
 *   - reset before inquiry, inquiry before connection;
 *   - control channel before interrupt;
 *   - SET_PROTOCOL on the control channel, not the interrupt one;
 *   - the pairing window opens only for the named device;
 *   - a device wanting pairing when none was invited fails with a reason
 *     rather than stalling.
 */
static void put_conn_complete(u8 *ev, const u8 *addr, u8 status, u16 handle)
{
	kmemset(ev, 0, 13);
	ev[0] = HCI_EV_CONN_COMPLETE;
	ev[1] = 11;
	ev[2] = status;
	ev[3] = (u8)(handle & 0xFF);
	ev[4] = (u8)(handle >> 8);
	kmemcpy(ev + 5, addr, BT_ADDR_LEN);
	ev[11] = HCI_LINK_TYPE_ACL;
}

bool bt_stack_self_test(void)
{
	static struct bt_stack s;
	struct hid_mouse_state ms;
	u8 out[64];
	u8 ev[16];
	u32 n;
	bool moved;
	bool ok = true;

	static const u8 addr[BT_ADDR_LEN] = {
		0x55, 0x44, 0x33, 0x22, 0x11, 0x00
	};

	/* --- reset comes first ------------------------------------------ */
	bt_stack_init(&s);
	n = bt_stack_start(&s, addr, out, sizeof(out));

	if (n != 3 || out[0] != 0x03 || out[1] != 0x0C) {
		kprintf("  btstack: starting drew %u bytes of opcode "
			"%02x%02x, expected a 3-byte HCI_Reset\n", n, out[1],
			out[0]);
		ok = false;
	}

	/* The window is open, and for this device only. */
	if (!s.pairing.window_open) {
		kputs("  btstack: starting with a named device did not "
		      "invite pairing for it\n");
		ok = false;
	}

	/* --- reset answered, inquiry follows ----------------------------- */
	ev[0] = HCI_EV_COMMAND_COMPLETE;
	ev[1] = 4;
	ev[2] = 1;
	ev[3] = 0x03;
	ev[4] = 0x0C;
	ev[5] = 0x00;

	n = bt_stack_event(&s, ev, 6, out, sizeof(out));

	if (n != 8 || out[0] != 0x01 || out[1] != 0x04) {
		kprintf("  btstack: a completed reset drew %u bytes of "
			"opcode %02x%02x, expected an Inquiry\n", n, out[1],
			out[0]);
		ok = false;
	}

	/* --- inquiry finishes, connection follows ------------------------ */
	{
		u8 done[2];

		done[0] = HCI_EV_INQUIRY_COMPLETE;
		done[1] = 0;

		n = bt_stack_event(&s, done, 2, out, sizeof(out));

		if (n != 16 || out[0] != 0x05 || out[1] != 0x04) {
			kprintf("  btstack: inquiry finishing drew %u bytes "
				"of opcode %02x%02x, expected a Create "
				"Connection\n", n, out[1], out[0]);
			ok = false;
		}
	}

	/* --- the link comes up, the control channel opens ---------------- */
	put_conn_complete(ev, addr, 0x00, 0x000C);
	n = bt_stack_event(&s, ev, 13, out, sizeof(out));

	if (s.link.handle != 0x000C) {
		kprintf("  btstack: the link handle is %04x, expected 000c\n",
			s.link.handle);
		ok = false;
	}

	if (n != 8 || out[0] != L2CAP_SIG_CONNECT_REQUEST) {
		kprintf("  btstack: the link coming up drew %u bytes of code "
			"%02x, expected an L2CAP Connection Request\n", n,
			out[0]);
		ok = false;
	}

	/* **Control first.** PSM 0x0011, not 0x0013. */
	if (out[4] != 0x11) {
		kprintf("  btstack: the first channel asked for PSM %02x, "
			"expected 11 -- control is opened before interrupt "
			"because the protocol is set on it\n", out[4]);
		ok = false;
	}

	/* --- the control channel completes, the interrupt one opens ------
	 *
	 * Both halves of the configuration, since a channel is not open on
	 * one alone -- which `l2cap.c` already asserts, and which this
	 * relies on.
	 */
	{
		u8 pdu[32];
		u8 body[8];
		u32 blen;

		/* Connection Response: their CID 0x0050, ours 0x0040. */
		bt_put_le16(body, 0x0050);
		bt_put_le16(body + 2, BT_CID_CONTROL);
		bt_put_le16(body + 4, L2CAP_CONN_SUCCESS);
		bt_put_le16(body + 6, 0);
		blen = l2cap_signal_build(pdu + L2CAP_HEADER,
					  L2CAP_SIG_CONNECT_RESPONSE, 1,
					  body, sizeof(body));
		l2cap_header_build(pdu, L2CAP_CID_SIGNALLING, (u16)blen);

		n = bt_stack_acl(&s, 0x000C, ACL_PB_START_FLUSHABLE, pdu,
				 L2CAP_HEADER + blen, out, sizeof(out),
				 &moved, &ms);

		if (!n || out[4] != L2CAP_SIG_CONFIG_REQUEST) {
			kprintf("  btstack: a control Connection Response "
				"drew %u bytes of code %02x, expected a "
				"Configure Request\n", n,
				n > 4 ? out[4] : 0);
			ok = false;
		}

		/* Their Configure Response, then their Configure Request. */
		bt_put_le16(body, BT_CID_CONTROL);
		bt_put_le16(body + 2, 0);
		bt_put_le16(body + 4, L2CAP_CONF_SUCCESS);
		blen = l2cap_signal_build(pdu + L2CAP_HEADER,
					  L2CAP_SIG_CONFIG_RESPONSE, 2,
					  body, 6);
		l2cap_header_build(pdu, L2CAP_CID_SIGNALLING, (u16)blen);
		bt_stack_acl(&s, 0x000C, ACL_PB_START_FLUSHABLE, pdu,
			     L2CAP_HEADER + blen, out, sizeof(out), &moved,
			     &ms);

		bt_put_le16(body, BT_CID_CONTROL);
		bt_put_le16(body + 2, 0);
		blen = l2cap_signal_build(pdu + L2CAP_HEADER,
					  L2CAP_SIG_CONFIG_REQUEST, 0x30,
					  body, 4);
		l2cap_header_build(pdu, L2CAP_CID_SIGNALLING, (u16)blen);

		n = bt_stack_acl(&s, 0x000C, ACL_PB_START_FLUSHABLE, pdu,
				 L2CAP_HEADER + blen, out, sizeof(out),
				 &moved, &ms);

		if (s.control.state != L2CAP_CH_OPEN) {
			kprintf("  btstack: the control channel is in state "
				"%u after both configurations\n",
				(unsigned)s.control.state);
			ok = false;
		}

		/* That reply is the Configure Response; the interrupt channel
		 * opens on the step after. */
		if (!n || out[4] != L2CAP_SIG_CONFIG_RESPONSE) {
			kputs("  btstack: their Configure Request on the "
			      "control channel was not answered\n");
			ok = false;
		}

		/* **The next step comes from `poll`, not from more input.**
		 *
		 * This is the distinction the first version of this file did
		 * not make: it answered their Configure Request and returned,
		 * and the check that opens the next channel sat after that
		 * return. The sequence stopped with the control channel open
		 * and nothing following it, which is what this test caught.
		 */
		n = bt_stack_poll(&s, out, sizeof(out));

		if (s.state != BT_ST_OPENING_INTERRUPT) {
			kprintf("  btstack: after control opened the state "
				"is %u, expected the interrupt channel to be "
				"opening\n", (unsigned)s.state);
			ok = false;
		}

		if (!n || out[4] != L2CAP_SIG_CONNECT_REQUEST) {
			kprintf("  btstack: polling after the control channel "
				"opened drew %u bytes of code %02x, expected "
				"a Connection Request for the interrupt "
				"channel\n", n, n > 4 ? out[4] : 0);
			ok = false;
		} else if (out[8] != 0x13) {
			kprintf("  btstack: the second channel asked for PSM "
				"%02x, expected 13\n", out[8]);
			ok = false;
		}

		/* Bring the interrupt channel up the same way. */
		bt_put_le16(body, 0x0051);
		bt_put_le16(body + 2, BT_CID_INTERRUPT);
		bt_put_le16(body + 4, L2CAP_CONN_SUCCESS);
		bt_put_le16(body + 6, 0);
		blen = l2cap_signal_build(pdu + L2CAP_HEADER,
					  L2CAP_SIG_CONNECT_RESPONSE,
					  s.interrupt.pending_ident, body, 8);
		l2cap_header_build(pdu, L2CAP_CID_SIGNALLING, (u16)blen);
		bt_stack_acl(&s, 0x000C, ACL_PB_START_FLUSHABLE, pdu,
			     L2CAP_HEADER + blen, out, sizeof(out), &moved,
			     &ms);

		bt_put_le16(body, BT_CID_INTERRUPT);
		bt_put_le16(body + 2, 0);
		bt_put_le16(body + 4, L2CAP_CONF_SUCCESS);
		blen = l2cap_signal_build(pdu + L2CAP_HEADER,
					  L2CAP_SIG_CONFIG_RESPONSE,
					  s.interrupt.pending_ident, body, 6);
		l2cap_header_build(pdu, L2CAP_CID_SIGNALLING, (u16)blen);
		bt_stack_acl(&s, 0x000C, ACL_PB_START_FLUSHABLE, pdu,
			     L2CAP_HEADER + blen, out, sizeof(out), &moved,
			     &ms);

		bt_put_le16(body, BT_CID_INTERRUPT);
		bt_put_le16(body + 2, 0);
		blen = l2cap_signal_build(pdu + L2CAP_HEADER,
					  L2CAP_SIG_CONFIG_REQUEST, 0x31,
					  body, 4);
		l2cap_header_build(pdu, L2CAP_CID_SIGNALLING, (u16)blen);
		bt_stack_acl(&s, 0x000C, ACL_PB_START_FLUSHABLE, pdu,
			     L2CAP_HEADER + blen, out, sizeof(out), &moved,
			     &ms);

		if (s.interrupt.state != L2CAP_CH_OPEN) {
			kprintf("  btstack: the interrupt channel is in "
				"state %u after both configurations\n",
				(unsigned)s.interrupt.state);
			ok = false;
		}

		/* --- and now SET_PROTOCOL, on the control channel --- */
		n = bt_stack_poll(&s, out, sizeof(out));

		if (n != L2CAP_HEADER + 1) {
			kprintf("  btstack: polling with both channels open "
				"drew %u bytes, expected a 5-byte "
				"SET_PROTOCOL\n", n);
			ok = false;
		} else {
			u16 cid = (u16)(out[2] | ((u16)out[3] << 8));

			if (cid != s.control.dcid) {
				kprintf("  btstack: SET_PROTOCOL went to "
					"channel %04x, expected the control "
					"channel %04x -- sending it on the "
					"interrupt one is why the order is "
					"fixed\n", cid, s.control.dcid);
				ok = false;
			}

			if (out[4] != 0x70) {
				kprintf("  btstack: SET_PROTOCOL carried %02x, "
					"expected 70 -- transaction 7, boot "
					"protocol 0\n", out[4]);
				ok = false;
			}
		}

		/* --- a device that will not do boot protocol -------------
		 *
		 * **This block was missing**, and its absence let the whole
		 * handshake check be deleted without a single assertion
		 * noticing: the test only ever sent a success, so code that
		 * accepted unconditionally agreed with it. A device refusing
		 * boot mode is allowed -- it needs its descriptor, which
		 * needs SDP, which is not written -- and must be reported
		 * rather than driven.
		 */
		{
			static struct bt_stack refuser;
			u8 hs[1];
			u32 hlen;

			/* Same position in the sequence, reached by copying
			 * the state rather than replaying it. */
			refuser = s;

			hs[0] = bt_hid_header(HIDP_TRANS_HANDSHAKE,
					HIDP_HSHK_ERR_UNSUPPORTED_REQUEST);
			hlen = wrap_pdu(pdu, sizeof(pdu), BT_CID_CONTROL, hs,
					1);
			bt_stack_acl(&refuser, 0x000C, ACL_PB_START_FLUSHABLE,
				     pdu, hlen, out, sizeof(out), &moved, &ms);

			if (refuser.state == BT_ST_RUNNING) {
				kputs("  btstack: a device that refused boot "
				      "protocol was driven anyway, and its "
				      "reports would be read against a "
				      "layout it never agreed to\n");
				ok = false;
			}

			if (refuser.state != BT_ST_FAILED ||
			    !refuser.failure) {
				kprintf("  btstack: a refused boot protocol "
					"left state %u with no reason "
					"attached\n", (unsigned)refuser.state);
				ok = false;
			}

			/* And NOT_READY is neither: ask again, do not give
			 * up. */
			refuser = s;
			hs[0] = bt_hid_header(HIDP_TRANS_HANDSHAKE,
					      HIDP_HSHK_NOT_READY);
			hlen = wrap_pdu(pdu, sizeof(pdu), BT_CID_CONTROL, hs,
					1);
			bt_stack_acl(&refuser, 0x000C, ACL_PB_START_FLUSHABLE,
				     pdu, hlen, out, sizeof(out), &moved, &ms);

			if (refuser.state != BT_ST_SETTING_PROTOCOL) {
				kprintf("  btstack: NOT_READY moved the "
					"sequence to state %u -- it means ask "
					"again, and giving up abandons a "
					"device that was about to work\n",
					(unsigned)refuser.state);
				ok = false;
			}
		}

		/* --- the device accepts, and reports start --- */
		{
			u8 hs[1];

			hs[0] = bt_hid_header(HIDP_TRANS_HANDSHAKE,
					      HIDP_HSHK_SUCCESSFUL);
			blen = wrap_pdu(pdu, sizeof(pdu), BT_CID_CONTROL, hs,
					1);
			bt_stack_acl(&s, 0x000C, ACL_PB_START_FLUSHABLE, pdu,
				     blen, out, sizeof(out), &moved, &ms);

			if (s.state != BT_ST_RUNNING) {
				kprintf("  btstack: after the device accepted "
					"boot protocol the state is %u, "
					"expected running\n",
					(unsigned)s.state);
				ok = false;
			}
		}

		/* --- an incomplete PDU must not be acted on --------------
		 *
		 * Running, with one buffer serving every channel. A
		 * signalling PDU arriving in pieces must do nothing until the
		 * last piece lands -- the first version parsed the partial
		 * buffer, because the feed that would have said "not yet" was
		 * skipped once the state reached running.
		 *
		 * **Choosing a PDU that makes it visible took a second
		 * attempt.** A short one does not: with six of eight bytes,
		 * the two length bytes of the signalling header are stale
		 * rubbish, `l2cap_signal_parse` refuses it, and the fault
		 * hides behind an accident.
		 *
		 * A *Configure Request* is sixteen bytes, and eight of them
		 * carry a complete and self-consistent signalling header. The
		 * partial parse then succeeds and reads its channel id out of
		 * bytes that have not arrived -- which are the previous PDU's,
		 * still in the buffer. Sending the same request twice makes
		 * those bytes the right channel id, so a stack reading early
		 * answers a request it has not finished receiving.
		 */
		{
			u8 cfg[4];
			u32 clen, whole;
			u64 before_wrong = s.control.wrong_channel;

			bt_put_le16(cfg, BT_CID_CONTROL);
			bt_put_le16(cfg + 2, 0);
			clen = l2cap_signal_build(pdu + L2CAP_HEADER,
						  L2CAP_SIG_CONFIG_REQUEST,
						  0x60, cfg, sizeof(cfg));
			l2cap_header_build(pdu, L2CAP_CID_SIGNALLING,
					   (u16)clen);
			whole = L2CAP_HEADER + clen;

			/* Once in full, so the buffer holds these bytes. */
			bt_stack_acl(&s, 0x000C, ACL_PB_START_FLUSHABLE, pdu,
				     whole, out, sizeof(out), &moved, &ms);

			/* Now the same thing, half of it. */
			n = bt_stack_acl(&s, 0x000C, ACL_PB_START_FLUSHABLE,
					 pdu, whole / 2, out, sizeof(out),
					 &moved, &ms);

			if (n) {
				kprintf("  btstack: a half-arrived Configure "
					"Request drew a %u-byte reply -- its "
					"channel id came from the previous "
					"PDU's bytes, which are still in the "
					"buffer\n", n);
				ok = false;
			}

			if (s.control.wrong_channel != before_wrong) {
				kputs("  btstack: a half-arrived PDU was "
				      "handed to a channel, which judged it "
				      "on bytes that had not arrived\n");
				ok = false;
			}

			/* And the rest of it completes normally. */
			n = bt_stack_acl(&s, 0x000C, ACL_PB_CONTINUATION,
					 pdu + whole / 2, whole - whole / 2,
					 out, sizeof(out), &moved, &ms);

			if (!n || out[4] != L2CAP_SIG_CONFIG_RESPONSE) {
				kprintf("  btstack: the completed Configure "
					"Request drew %u bytes of code %02x, "
					"expected a Configure Response\n", n,
					n > 4 ? out[4] : 0);
				ok = false;
			}
		}

		/* --- a report, all the way to a movement --- */
		{
			u8 rep[5];

			rep[0] = bt_hid_header(HIDP_TRANS_DATA,
					       HIDP_REPORT_INPUT);
			rep[1] = 0x01;		/* left button */
			rep[2] = 0x05;		/* x = +5 */
			rep[3] = 0xFB;		/* y = -5 */
			rep[4] = 0x00;		/* wheel */

			blen = wrap_pdu(pdu, sizeof(pdu), BT_CID_INTERRUPT,
					rep, 5);
			bt_stack_acl(&s, 0x000C, ACL_PB_START_FLUSHABLE, pdu,
				     blen, out, sizeof(out), &moved, &ms);

			if (!moved) {
				kputs("  btstack: a report on the interrupt "
				      "channel did not become a movement\n");
				ok = false;
			} else if (ms.x != 5 || ms.y != -5 ||
				   ms.buttons != 1) {
				kprintf("  btstack: the report decoded as "
					"buttons %02x x=%d y=%d, expected 01, "
					"5, -5\n", ms.buttons, ms.x, ms.y);
				ok = false;
			}

			if (s.reports != 1) {
				kprintf("  btstack: %llu reports counted, "
					"expected 1\n",
					(unsigned long long)s.reports);
				ok = false;
			}
		}
	}

	/* --- a buffer too small for what would be written ----------------
	 *
	 * The command builders take no size; the caller promises the room.
	 * That promise is only worth what the call sites keep, and one of
	 * them here was not keeping it.
	 *
	 * Checked with a canary rather than by inspection: a byte past the
	 * end of a deliberately short buffer must still hold what was put
	 * there. An overrun of a few bytes into somebody else's memory is
	 * the kind of fault that does not fail here at all -- it fails
	 * somewhere unrelated, later.
	 */
	{
		static struct bt_stack tight;
		u8 small[9];		/* 8 usable, 1 canary */
		u32 m;

		bt_stack_init(&tight);
		bt_stack_start(&tight, addr, out, sizeof(out));

		ev[0] = HCI_EV_COMMAND_COMPLETE;
		ev[1] = 4;
		ev[2] = 1;
		ev[3] = 0x03;
		ev[4] = 0x0C;
		ev[5] = 0x00;

		/* The inquiry this draws is eight bytes; offer seven. */
		kmemset(small, 0xC7, sizeof(small));
		m = bt_stack_event(&tight, ev, 6, small, 7);

		if (m) {
			kprintf("  btstack: a command was written into a "
				"seven-byte buffer that needs eight (%u "
				"bytes)\n", m);
			ok = false;
		}

		if (small[8] != 0xC7 || small[7] != 0xC7) {
			kputs("  btstack: the bytes past a short buffer were "
			      "overwritten -- nothing fails here, it fails "
			      "later somewhere unrelated\n");
			ok = false;
		}

		/* And with enough room it still works, so the guard is not
		 * simply refusing everything. */
		m = bt_stack_event(&tight, ev, 6, small, 8);

		if (m != 8) {
			kprintf("  btstack: with eight bytes offered the "
				"inquiry came to %u\n", m);
			ok = false;
		}
	}

	/* --- the link goes away, and everything on it must go too --------
	 *
	 * Every other test here runs the sequence once. This runs it to
	 * RUNNING and then takes the link away, which is the case a
	 * once-through test cannot reach.
	 *
	 * What makes it matter rather than merely untidy: **handles are
	 * reused.** A mouse left ready on a reclaimed handle decodes the
	 * next connection's data with the previous device's layout.
	 */
	{
		u8 disc[6];
		u16 old_handle = s.link.handle;

		if (s.state != BT_ST_RUNNING || !s.mouse.ready) {
			kputs("  btstack: the sequence was not running "
			      "before the disconnection test\n");
			ok = false;
		}

		disc[0] = HCI_EV_DISCONN_COMPLETE;
		disc[1] = 4;
		disc[2] = 0x00;			/* the disconnect succeeded */
		disc[3] = (u8)(old_handle & 0xFF);
		disc[4] = (u8)(old_handle >> 8);
		disc[5] = 0x13;			/* remote user ended it */

		bt_stack_event(&s, disc, sizeof(disc), out, sizeof(out));

		if (s.mouse.ready) {
			kputs("  btstack: the mouse is still ready after the "
			      "link went away, and handles are reused -- the "
			      "next device on that handle would be decoded "
			      "with this one's layout\n");
			ok = false;
		}

		if (s.state == BT_ST_RUNNING) {
			kputs("  btstack: the sequence is still running with "
			      "no link under it\n");
			ok = false;
		}

		if (s.control.state == L2CAP_CH_OPEN ||
		    s.interrupt.state == L2CAP_CH_OPEN) {
			kputs("  btstack: a channel survived the link it was "
			      "carried on\n");
			ok = false;
		}

		/* A report on the old handle must now do nothing. */
		{
			u8 rep[5];
			u8 wire[32];
			u32 blen;

			rep[0] = bt_hid_header(HIDP_TRANS_DATA,
					       HIDP_REPORT_INPUT);
			rep[1] = 0x01;
			rep[2] = 0x7F;
			rep[3] = 0x7F;
			rep[4] = 0x00;
			blen = wrap_pdu(wire, sizeof(wire), BT_CID_INTERRUPT,
					rep, 5);

			bt_stack_acl(&s, old_handle, ACL_PB_START_FLUSHABLE,
				     wire, blen, out, sizeof(out), &moved,
				     &ms);

			if (moved) {
				kputs("  btstack: traffic on the old handle "
				      "still moved the pointer\n");
				ok = false;
			}
		}

		/* And the pairing survived, because the link key did --
		 * losing it would make a known device pair again every time
		 * it disconnects. */
		if (!s.pairing.have_target) {
			kputs("  btstack: the device being paired with was "
			      "forgotten when the link dropped\n");
			ok = false;
		}
	}

	/* --- a device that wants pairing nobody invited ------------------ */
	{
		static struct bt_stack bare;
		u8 keyreq[8];

		bt_stack_init(&bare);

		/* Started with no address: inquire for anything, and invite
		 * nothing. */
		bt_stack_start(&bare, 0, out, sizeof(out));

		if (bare.pairing.window_open) {
			kputs("  btstack: starting without a named device "
			      "opened a pairing window anyway, so anything "
			      "that asked would be let in\n");
			ok = false;
		}

		keyreq[0] = HCI_EV_LINK_KEY_REQ;
		keyreq[1] = BT_ADDR_LEN;
		kmemcpy(keyreq + 2, addr, BT_ADDR_LEN);

		n = bt_stack_event(&bare, keyreq, 8, out, sizeof(out));

		/* A negative reply, which is what starts pairing -- and then
		 * the IO capability request that follows is refused, because
		 * no window is open. */
		if (!n) {
			kputs("  btstack: a link key request drew no answer, "
			      "and the controller waits\n");
			ok = false;
		}

		{
			u8 iocap[8];

			iocap[0] = HCI_EV_IO_CAP_REQUEST;
			iocap[1] = BT_ADDR_LEN;
			kmemcpy(iocap + 2, addr, BT_ADDR_LEN);

			n = bt_stack_event(&bare, iocap, 8, out, sizeof(out));

			if (!n ||
			    out[0] != (u8)(HCI_OP_IO_CAP_NEG_REPLY & 0xFF)) {
				kputs("  btstack: an uninvited pairing got "
				      "past the stack, which opens no window "
				      "when it is not given a device\n");
				ok = false;
			}
		}
	}

	/* --- and the boot layout is the one the parser agrees with ------- */
	{
		struct hid_mouse_layout boot;
		static const u8 rep[4] = { 0x01, 0x05, 0xFB, 0x00 };
		struct hid_mouse_state d;

		hid_mouse_boot_layout(&boot, true);

		if (!hid_mouse_decode(&boot, rep, 4, &d) || d.x != 5 ||
		    d.y != -5 || d.buttons != 1) {
			kprintf("  btstack: a boot report decoded as buttons "
				"%02x x=%d y=%d, expected 01, 5, -5\n",
				d.buttons, d.x, d.y);
			ok = false;
		}
	}

	return ok;
}
