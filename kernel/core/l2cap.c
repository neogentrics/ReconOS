/* L2CAP framing, and putting a PDU back together from ACL fragments.
 *
 * The layer above HCI and below anything useful. See `l2cap.h` for what it is
 * for; this file is the bytes.
 *
 * --- What is here, and what is not ---
 *
 * Here: the basic frame, reassembly across fragments, the signalling commands
 * a HID connection needs, and the configuration options it negotiates. All of
 * it is a pure function of its input and all of it is exercised below.
 *
 * Not here: a connection state machine. Nothing can hold an L2CAP connection
 * until HCI works at all, and a state machine with no link to run on is
 * structure nobody can check -- the same reason `bluetooth.c` has no table of
 * controllers. It goes in when there is a link.
 *
 * --- The three places this is easy to get wrong ---
 *
 * **The length excludes the header.** Four bytes, not zero and not two. HCI's
 * event length excludes its two-byte header, so the rule sounds like the same
 * rule and the constant is different. Off by four gives a PDU missing exactly
 * its header, and a short mouse report is still a run of bytes that decodes.
 *
 * **Fragments from two links interleave.** Nothing in a fragment except the
 * handle says which conversation it belongs to, and the handle is in the ACL
 * header rather than in the L2CAP one -- so by the time these bytes arrive,
 * the only thing that keeps two devices apart is the caller passing the right
 * handle in. A reassembler that ignores it splices them and produces a PDU
 * that parses cleanly and means nothing.
 *
 * **A configuration option list can be walked forever.** Options are
 * type-length-value, the walk advances by the length, and an option declaring
 * a length of zero advances by two -- so the loop terminates only because the
 * header is counted too. Declaring a length that runs past the end is the
 * other half of it. Both are below, and both are tested, because the failure
 * is a kernel that stops rather than a value that is wrong.
 */
#include <recon/kernel/bt_bytes.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/l2cap.h>

/* --- little-endian helpers ------------------------------------------------
 *
 * Everything in L2CAP is little-endian on the wire, unlike the SCSI commands
 * inside `usb_storage.c`'s wrappers, which are big-endian inside a
 * little-endian envelope. Named rather than open-coded so that a big-endian
 * read in here would have to be written deliberately.
 */

/* --- the basic frame ------------------------------------------------------ */

bool l2cap_header_parse(const u8 *p, u32 len, struct l2cap_header *out)
{
	if (len < L2CAP_HEADER)
		return false;

	out->length = bt_get_le16(p);
	out->cid    = bt_get_le16(p + 2);

	return true;
}

u32 l2cap_header_build(u8 *out, u16 cid, u16 payload_length)
{
	bt_put_le16(out, payload_length);
	bt_put_le16(out + 2, cid);

	return L2CAP_HEADER;
}

/* --- reassembly ----------------------------------------------------------- */

void l2cap_reassembly_reset(struct l2cap_reassembly *r)
{
	r->have = 0;
	r->want = 0;
	r->handle = 0;
	r->active = false;
}

bool l2cap_acl_feed(struct l2cap_reassembly *r, u16 handle, u8 pb,
		    const u8 *payload, u32 len)
{
	bool starting;

	/* A finished PDU is cleared here rather than by the caller, for the
	 * reason `hci_event_feed` gives: a caller that forgot would build the
	 * next PDU on top of the last one. */
	if (r->active && r->want && r->have >= r->want)
		l2cap_reassembly_reset(r);

	if (!len)
		return false;

	/* Only one of the four boundary values continues something.
	 *
	 * 0x00 and 0x02 both begin a PDU and differ in whether the controller
	 * may flush it, which is a question about the link rather than about
	 * reassembly -- so both start one here. 0x03 is the LE encoding for a
	 * complete PDU in one fragment and has no meaning on a BR/EDR ACL
	 * link; it is treated as a start rather than guessed at, and the
	 * distinction is recorded here rather than lost, because a BR/EDR link
	 * producing one would mean something is wrong further down. */
	starting = (pb != ACL_PB_CONTINUATION);

	if (!starting) {
		if (!r->active) {
			/* Nothing to continue. This is ordinary right after a
			 * reset -- the first fragment seen may be the middle of
			 * a PDU that began before anybody was listening -- and
			 * it must be dropped rather than read as a header. */
			r->orphans++;
			return false;
		}

		if (handle != r->handle) {
			/* Another link's bytes. Appending them is the fault
			 * this whole structure exists to stop. */
			r->crossed++;
			return false;
		}
	} else {
		if (r->active && r->have < r->want) {
			/* A new PDU while the last one was unfinished. The old
			 * one is gone -- the controller does not resend -- and
			 * keeping its bytes would prefix this one. */
			r->restarts++;
		}

		l2cap_reassembly_reset(r);
		r->active = true;
		r->handle = handle;
	}

	/* Subtraction, not addition: `len` comes from the transport and
	 * `have + len` wraps for a large one, which turns this guard
	 * into a permission. `have` never exceeds the buffer, so
	 * `sizeof - have` is safe. */
	if (len > sizeof(r->buf) - r->have) {
		/* Past what this kernel said it could take. A peer told 672 in
		 * the Configure Request should never do this, so it is either
		 * a device that ignored the negotiation or a reassembly that
		 * has lost its place. Either way it must not be a write. */
		r->oversized++;
		l2cap_reassembly_reset(r);
		return false;
	}

	kmemcpy(r->buf + r->have, payload, len);
	r->have += len;

	/* The whole header before anything is read out of it.
	 *
	 * **Stricter than it strictly needs to be, deliberately.** The length
	 * is the first two bytes, so a two-byte fragment could in principle
	 * yield it; the byte that must not be read early is byte 1, and the
	 * hazard is a *one*-byte fragment, where byte 1 still holds whatever
	 * the last PDU left there. Waiting for all four costs nothing -- the
	 * CID is needed before the PDU is worth anything anyway -- and one
	 * rule is easier to keep true than two.
	 *
	 * Asserted on `want` below rather than on the return value, because
	 * `bluetooth.c` had this same check written the weaker way and it
	 * stayed green when the guard was deleted. */
	if (r->have < L2CAP_HEADER)
		return false;

	/* Plus the header, which the length does not count. */
	r->want = (u32)L2CAP_HEADER + bt_get_le16(r->buf);

	if (r->want > sizeof(r->buf)) {
		r->oversized++;
		l2cap_reassembly_reset(r);
		return false;
	}

	if (r->have < r->want)
		return false;

	if (r->have > r->want) {
		/* More bytes than the PDU declared. Unlike an HCI event, a
		 * second L2CAP PDU genuinely may share one ACL fragment -- but
		 * splitting here would hand the caller one PDU and silently
		 * drop the other, which is worse than refusing both. Counted
		 * as oversized and dropped, and this is the line to revisit
		 * when a device is seen doing it. */
		r->oversized++;
		l2cap_reassembly_reset(r);
		return false;
	}

	r->completed++;
	return true;
}

/* --- signalling ----------------------------------------------------------- */

bool l2cap_signal_parse(const u8 *p, u32 len, struct l2cap_signal *out)
{
	u16 dlen;

	if (len < L2CAP_SIG_HEADER)
		return false;

	dlen = bt_get_le16(p + 2);

	/* A command claiming more data than arrived is refused rather than
	 * truncated. Truncating it would hand a parser a short buffer that
	 * still has the right shape. */
	if ((u32)dlen + L2CAP_SIG_HEADER > len)
		return false;

	out->code   = p[0];
	out->id     = p[1];
	out->length = dlen;
	out->data   = p + L2CAP_SIG_HEADER;

	return true;
}

u32 l2cap_signal_build(u8 *out, u8 code, u8 id, const u8 *data, u16 dlen)
{
	out[0] = code;
	out[1] = id;
	bt_put_le16(out + 2, dlen);

	if (dlen && data)
		kmemcpy(out + L2CAP_SIG_HEADER, data, dlen);

	return (u32)L2CAP_SIG_HEADER + dlen;
}

u32 l2cap_connect_request_build(u8 *out, u8 id, u16 psm, u16 scid)
{
	u8 body[4];

	bt_put_le16(body, psm);
	bt_put_le16(body + 2, scid);

	return l2cap_signal_build(out, L2CAP_SIG_CONNECT_REQUEST, id, body,
				  sizeof(body));
}

bool l2cap_connect_response_parse(const u8 *data, u32 len, u16 *dcid,
				  u16 *scid, u16 *result, u16 *status)
{
	if (len < 8)
		return false;

	*dcid   = bt_get_le16(data);
	*scid   = bt_get_le16(data + 2);
	*result = bt_get_le16(data + 4);
	*status = bt_get_le16(data + 6);

	return true;
}

u32 l2cap_config_request_build(u8 *out, u8 id, u16 dcid, u16 mtu)
{
	u8 body[8];

	bt_put_le16(body, dcid);
	bt_put_le16(body + 2, 0);		/* continuation flags: no more to come */

	body[4] = L2CAP_CONF_OPT_MTU;	/* not a hint: this one must be obeyed */
	body[5] = 2;
	bt_put_le16(body + 6, mtu);

	return l2cap_signal_build(out, L2CAP_SIG_CONFIG_REQUEST, id, body,
				  sizeof(body));
}

bool l2cap_config_response_parse(const u8 *data, u32 len, u16 *scid,
				 u16 *flags, u16 *result)
{
	if (len < 6)
		return false;

	*scid   = bt_get_le16(data);
	*flags  = bt_get_le16(data + 2);
	*result = bt_get_le16(data + 4);

	return true;
}

bool l2cap_config_find_mtu(const u8 *opts, u32 len, u16 *mtu)
{
	u32 at = 0;

	/* Two bytes of header before any value, so an option needs at least
	 * that much left to exist at all. */
	while (at + 2 <= len) {
		u8 type = (u8)(opts[at] & (u8)~L2CAP_CONF_OPT_HINT);
		u8 olen = opts[at + 1];

		/* Declaring more than is left. Stopping rather than clamping:
		 * a clamped option is a value read from bytes that belong to
		 * something else. */
		if (at + 2u + olen > len)
			return false;

		if (type == L2CAP_CONF_OPT_MTU && olen == 2) {
			*mtu = bt_get_le16(opts + at + 2);
			return true;
		}

		/* **The header is counted, which is what ends this loop.** An
		 * option declaring a length of zero is legal-looking and
		 * advances `at` by two; advancing by `olen` alone would sit on
		 * it forever, and a kernel that stops is a worse answer than a
		 * number that is wrong. */
		at += 2u + olen;
	}

	return false;
}

/* --- a channel, and getting one open -------------------------------------- */

void l2cap_channel_init(struct l2cap_channel *c, u16 psm, u16 scid)
{
	kmemset(c, 0, sizeof(*c));
	c->state = L2CAP_CH_CLOSED;
	c->psm = psm;
	c->scid = scid;
}

u32 l2cap_channel_start(struct l2cap_channel *c, u8 *out, u8 ident)
{
	c->pending_ident = ident;
	c->state = L2CAP_CH_WAIT_CONNECT;

	return l2cap_connect_request_build(out, ident, c->psm, c->scid);
}

/* Both halves done, and only then. */
static void maybe_open(struct l2cap_channel *c)
{
	if (c->state == L2CAP_CH_WAIT_CONFIG &&
	    c->config_out_done && c->config_in_done)
		c->state = L2CAP_CH_OPEN;
}

u32 l2cap_channel_input(struct l2cap_channel *c, const u8 *sig, u32 len,
			u8 *out, u32 outmax)
{
	struct l2cap_signal s;
	u16 a, b, result;

	if (!l2cap_signal_parse(sig, len, &s))
		return 0;

	switch (s.code) {
	case L2CAP_SIG_CONNECT_RESPONSE: {
		u16 status;

		if (c->state != L2CAP_CH_WAIT_CONNECT)
			return 0;

		/* Somebody else's answer. The identifier is the only thing
		 * pairing a response with its request, and taking one that
		 * does not match means acting on a result for a channel this
		 * is not. */
		if (s.id != c->pending_ident) {
			c->wrong_ident++;
			return 0;
		}

		if (!l2cap_connect_response_parse(s.data, s.length, &a, &b,
						  &result, &status))
			return 0;

		/* `b` is the source CID echoed back -- this side's. A
		 * response naming a different one is about another channel. */
		if (b != c->scid) {
			c->wrong_channel++;
			return 0;
		}

		/* **Pending is not success and not refusal.** It means the
		 * peer is still deciding -- asking a user, or checking
		 * security -- and another response follows. A driver that
		 * treats it as refusal gives up on a device about to say yes;
		 * one that treats it as success configures a channel that
		 * does not exist yet. */
		if (result == L2CAP_CONN_PENDING)
			return 0;

		if (result != L2CAP_CONN_SUCCESS) {
			c->state = L2CAP_CH_REFUSED;
			c->refused_result = result;
			return 0;
		}

		c->dcid = a;
		c->state = L2CAP_CH_WAIT_CONFIG;

		/* Our half of configuration. A fresh identifier, because the
		 * one above has been answered. */
		c->pending_ident = (u8)(c->pending_ident + 1);

		if (outmax < 12)
			return 0;

		return l2cap_config_request_build(out, c->pending_ident,
						  c->dcid, L2CAP_MTU);
	}

	case L2CAP_SIG_CONFIG_RESPONSE:
		if (c->state != L2CAP_CH_WAIT_CONFIG)
			return 0;

		if (s.id != c->pending_ident) {
			c->wrong_ident++;
			return 0;
		}

		if (!l2cap_config_response_parse(s.data, s.length, &a, &b,
						 &result))
			return 0;

		if (a != c->scid) {
			c->wrong_channel++;
			return 0;
		}

		if (result != L2CAP_CONF_SUCCESS)
			return 0;

		c->config_out_done = true;
		maybe_open(c);
		return 0;

	case L2CAP_SIG_CONFIG_REQUEST: {
		u8 body[6];
		u16 mtu = 0;

		/* Their half. Answered whenever it arrives -- it may well
		 * arrive before this side's own Configure Response, and
		 * refusing it because the state is not what was expected
		 * deadlocks the pair. */
		if (c->state != L2CAP_CH_WAIT_CONFIG &&
		    c->state != L2CAP_CH_OPEN)
			return 0;

		if (s.length < 4)
			return 0;

		/* **The channel it names, which this one forgot to check.**
		 *
		 * A Configure Request carries the destination CID -- this
		 * side's -- and every other message here is matched on one.
		 * This case read it and never compared it, so a channel that
		 * happened to be open would answer a request addressed to a
		 * different one, and the channel it was meant for would sit
		 * waiting for a configuration that had already been answered
		 * by somebody else.
		 *
		 * Found by opening two channels on one link: the control
		 * channel, open first, swallowed the interrupt channel's
		 * Configure Request. Nothing in this file's own tests could
		 * see it, because they only ever have one channel. */
		if (bt_get_le16(s.data) != c->scid) {
			c->wrong_channel++;
			return 0;
		}

		/* Anything after the destination CID and the flags is the
		 * option list. An absent MTU option means the default rather
		 * than an error, so a false here is not a failure. */
		if (l2cap_config_find_mtu(s.data + 4, s.length - 4u, &mtu))
			c->peer_mtu = mtu;

		bt_put_le16(body, c->dcid);	/* their channel, from here */
		bt_put_le16(body + 2, 0);		/* no continuation */
		bt_put_le16(body + 4, L2CAP_CONF_SUCCESS);

		c->config_in_done = true;
		maybe_open(c);

		if (outmax < 10)
			return 0;

		return l2cap_signal_build(out, L2CAP_SIG_CONFIG_RESPONSE,
					  s.id, body, sizeof(body));
	}

	case L2CAP_SIG_DISCONNECT_REQUEST: {
		u8 body[4];

		if (s.length < 4)
			return 0;

		/* **The same omission as the Configure Request above, found
		 * by going looking for it.**
		 *
		 * A Disconnection Request names the channel to close --
		 * destination CID first, this side's. Without this check any
		 * channel would act on a request meant for another: it would
		 * tear itself down *and* answer on the other's behalf, so the
		 * channel actually being closed never gets its response and
		 * a working one is destroyed instead.
		 *
		 * That is worse than the configure case, which only stalled.
		 * It was found by listing every place a message is matched --
		 * five had a rule, two did not -- rather than by a test,
		 * because nothing here disconnects a second channel. */
		if (bt_get_le16(s.data) != c->scid) {
			c->wrong_channel++;
			return 0;
		}

		/* Echoed back as sent: destination then source, from the
		 * requester's point of view. */
		body[0] = s.data[0];
		body[1] = s.data[1];
		body[2] = s.data[2];
		body[3] = s.data[3];

		c->state = L2CAP_CH_CLOSED;
		c->config_out_done = false;
		c->config_in_done = false;

		if (outmax < 8)
			return 0;

		return l2cap_signal_build(out, L2CAP_SIG_DISCONNECT_RESPONSE,
					  s.id, body, sizeof(body));
	}

	default:
		return 0;
	}
}

/* --- the self-test --------------------------------------------------------
 *
 * Pure functions, synthetic bytes, no link. The cases are chosen the same way
 * as `bluetooth.c`'s: each one has a wrong answer that still looks like a
 * working stack.
 *
 *   - the length must exclude the header, because off by four is a PDU short
 *     by exactly its header and a short report still decodes;
 *   - a PDU split across fragments must complete **once**;
 *   - a fragment from another link must be refused, not appended, because
 *     appending produces a PDU that parses and means nothing;
 *   - a start fragment shorter than the header must not compute a length from
 *     bytes that have not arrived. This is asserted on `want` rather than on
 *     the return value, because `bluetooth.c` had the same check written the
 *     weaker way and it stayed green when the guard was deleted;
 *   - a configuration walk must terminate on a zero-length option, because
 *     the wrong answer there is a kernel that stops rather than a number;
 *   - the hint bit must be masked off a known option type, or a known option
 *     reads as an unknown one.
 */
static void plant(u8 *p, u32 n, u8 v)
{
	kmemset(p, v, n);
}

bool l2cap_self_test(void)
{
	struct l2cap_reassembly r;
	struct l2cap_header h;
	struct l2cap_signal sig;

	/* **`pdu` is written once, at the top, and read-only after that.**
	 *
	 * Eight blocks below feed it and none re-establishes it, so a block
	 * that writes it changes the fixture for every later one. That
	 * happened: a case added in the middle set a longer declared length,
	 * and the restart case two hundred lines down failed for a reason
	 * nothing in it mentioned.
	 *
	 * A block needing different bytes declares its own, as the oversized
	 * and overflow cases do. The rule is cheaper than re-establishing the
	 * fixture eight times, and it is written here so that breaking it is
	 * visible rather than discovered later. */
	u8 pdu[L2CAP_PDU_MAX];
	u8 out[64];
	u16 a, b, c, d;
	u32 n;
	bool ok = true;

	/* --- the length excludes the header ----------------------------- */
	n = l2cap_header_build(out, L2CAP_CID_SIGNALLING, 8);

	if (n != 4 || out[0] != 8 || out[1] != 0 || out[2] != 1 ||
	    out[3] != 0) {
		kprintf("  l2cap: a header for 8 bytes on channel 1 came out "
			"as %u bytes %02x %02x %02x %02x, expected 4 bytes "
			"08 00 01 00\n", n, out[0], out[1], out[2], out[3]);
		ok = false;
	}

	if (!l2cap_header_parse(out, 4, &h) || h.length != 8 ||
	    h.cid != L2CAP_CID_SIGNALLING) {
		kprintf("  l2cap: that header read back as length %u channel "
			"%u\n", h.length, h.cid);
		ok = false;
	}

	if (l2cap_header_parse(out, 3, &h)) {
		kputs("  l2cap: a three-byte header was accepted, so its "
		      "channel came from a byte that had not arrived\n");
		ok = false;
	}

	/* --- one fragment, one PDU -------------------------------------- */
	l2cap_reassembly_reset(&r);
	r.completed = r.orphans = r.crossed = r.restarts = r.oversized = 0;

	bt_put_le16(pdu, 4);			/* four bytes of payload */
	bt_put_le16(pdu + 2, L2CAP_CID_SIGNALLING);
	pdu[4] = L2CAP_SIG_CONNECT_REQUEST;
	pdu[5] = 1;
	bt_put_le16(pdu + 6, 0);

	if (!l2cap_acl_feed(&r, 0x0001, ACL_PB_START_FLUSHABLE, pdu, 8)) {
		kputs("  l2cap: a whole PDU in one fragment did not "
		      "complete\n");
		ok = false;
	} else if (r.want != 8) {
		kprintf("  l2cap: a PDU declaring 4 bytes of payload came to "
			"%u bytes, expected 8 -- the length excludes the "
			"four-byte header\n", r.want);
		ok = false;
	}

	/* --- the same PDU in three pieces -------------------------------- */
	{
		unsigned completions = 0;

		l2cap_reassembly_reset(&r);

		if (l2cap_acl_feed(&r, 0x0001, ACL_PB_START_FLUSHABLE, pdu, 3))
			completions++;
		if (l2cap_acl_feed(&r, 0x0001, ACL_PB_CONTINUATION, pdu + 3, 3))
			completions++;
		if (l2cap_acl_feed(&r, 0x0001, ACL_PB_CONTINUATION, pdu + 6, 2))
			completions++;

		if (completions != 1) {
			kprintf("  l2cap: a PDU across three fragments "
				"completed %u times, not once\n", completions);
			ok = false;
		}
	}

	/* --- a start fragment too short to carry the length --------------
	 *
	 * **One byte, not two.** The first version of this fed two, and two is
	 * the wrong number: the length is bytes 0 and 1, so after two bytes it
	 * has entirely arrived and the value computed from it is correct. That
	 * version went red when the guard was deleted, but for the wrong
	 * reason, and its message said bytes had not arrived when they had.
	 *
	 * With one byte, byte 1 is genuinely stale. The planted value is small
	 * on purpose: a large one makes `want` exceed the buffer, which the
	 * oversized check catches and resets -- leaving `want` at zero and the
	 * assertion below passing with the guard gone. A test that plants
	 * 0x7F7F here cannot fail.
	 */
	{
		l2cap_reassembly_reset(&r);

		/* Stale high byte of a length. 0x01 gives a `want` of 264 --
		 * inside the buffer, so nothing else rescues it. */
		plant(r.buf, L2CAP_HEADER, 0x00);
		r.buf[1] = 0x01;

		if (l2cap_acl_feed(&r, 0x0001, ACL_PB_START_FLUSHABLE, pdu, 1)) {
			kputs("  l2cap: one byte was reported as a whole "
			      "PDU\n");
			ok = false;
		}

		if (r.want != 0) {
			kprintf("  l2cap: after one byte the PDU length is "
				"%u, and byte 1 of the length had not arrived "
				"yet\n", r.want);
			ok = false;
		}

		/* And it still finishes when the rest turns up. */
		if (!l2cap_acl_feed(&r, 0x0001, ACL_PB_CONTINUATION, pdu + 1,
				    7)) {
			kputs("  l2cap: a PDU whose header was split across "
			      "fragments never completed\n");
			ok = false;
		}
	}

	/* --- another link's fragment must not be appended ---------------- */
	{
		l2cap_reassembly_reset(&r);
		r.crossed = 0;

		l2cap_acl_feed(&r, 0x0001, ACL_PB_START_FLUSHABLE, pdu, 4);

		if (l2cap_acl_feed(&r, 0x0002, ACL_PB_CONTINUATION, pdu + 4,
				   4) || r.crossed != 1) {
			kputs("  l2cap: a fragment from a different "
			      "connection was appended to this one -- the "
			      "result parses and belongs to neither device\n");
			ok = false;
		}

		/* The open PDU must survive that: the other link's fragment is
		 * not this link's problem. */
		if (!l2cap_acl_feed(&r, 0x0001, ACL_PB_CONTINUATION, pdu + 4,
				    4)) {
			kputs("  l2cap: a stray fragment from another link "
			      "destroyed the PDU being assembled\n");
			ok = false;
		}
	}

	/* --- an ACL fragment whose length wraps the bounds check ---------
	 *
	 * The same hazard as `hci_event_feed`'s, in the parallel function.
	 * Both were written the same way and both were wrong the same way,
	 * which is the argument for checking parallel implementations
	 * together rather than trusting that one was copied correctly.
	 */
	{
		l2cap_reassembly_reset(&r);
		r.oversized = 0;

		/* **The buffer has to be non-empty first, and the first
		 * version of this test forgot.**
		 *
		 * With `have` at zero, `0 + 0xFFFFFFFF` does not wrap -- it is
		 * 0xFFFFFFFF, which is larger than the buffer, so even the
		 * additive form refuses it correctly. Putting the broken
		 * check back left this green.
		 *
		 * The wrap needs the sum to pass 2^32. With four bytes
		 * already held, `4 + 0xFFFFFFFF` is 3, which is smaller than
		 * any buffer and sails through into a four-gigabyte copy.
		 */
		/* A local buffer, not the shared `pdu`.
		 *
		 * The first version wrote a longer declared length into
		 * `pdu`, which later blocks still feed and expect to
		 * complete -- the restart case reads it as an eight-byte PDU.
		 * Changing it here made that test fail, which is this test
		 * polluting its neighbours rather than a fault in the code.
		 * The same slip happened in `bluetooth.c` minutes earlier,
		 * with `ev`. */
		{
			u8 partial[4];

			bt_put_le16(partial, 64);	/* longer than we feed */
			bt_put_le16(partial + 2, L2CAP_CID_SIGNALLING);
			l2cap_acl_feed(&r, 0x0001, ACL_PB_START_FLUSHABLE,
				       partial, 4);
		}

		if (r.have != 4) {
			kprintf("  l2cap: the setup fragment left %u bytes, "
				"expected 4 -- without them the wrap below "
				"cannot happen and the test proves nothing\n",
				r.have);
			ok = false;
		}

		if (l2cap_acl_feed(&r, 0x0001, ACL_PB_CONTINUATION, pdu,
				   0xFFFFFFFFu) || r.oversized != 1) {
			kputs("  l2cap: a continuation claiming four "
			      "gigabytes was accepted on top of four bytes "
			      "already held; that sum wraps to 3\n");
			ok = false;
		}
	}

	/* --- a continuation with nothing to continue --------------------- */
	{
		l2cap_reassembly_reset(&r);
		r.orphans = 0;

		if (l2cap_acl_feed(&r, 0x0001, ACL_PB_CONTINUATION, pdu, 8) ||
		    r.orphans != 1) {
			kputs("  l2cap: a continuation fragment with no start "
			      "was read as a PDU -- its first two bytes are "
			      "payload, not a length\n");
			ok = false;
		}
	}

	/* --- a new PDU while the last was unfinished --------------------- */
	{
		l2cap_reassembly_reset(&r);
		r.restarts = 0;

		l2cap_acl_feed(&r, 0x0001, ACL_PB_START_FLUSHABLE, pdu, 4);

		if (!l2cap_acl_feed(&r, 0x0001, ACL_PB_START_FLUSHABLE, pdu,
				    8) || r.restarts != 1) {
			kputs("  l2cap: a PDU begun while another was "
			      "unfinished did not discard the first -- its "
			      "bytes would prefix this one\n");
			ok = false;
		}
	}

	/* --- a PDU larger than the MTU this kernel advertises ------------ */
	{
		u8 big[8];

		l2cap_reassembly_reset(&r);
		r.oversized = 0;

		/* Its own buffer. This wrote into the shared `pdu`, which
		 * every block above reads and none of them re-establishes --
		 * so it worked only by being last. A block added after it
		 * would have inherited a PDU declaring 673 bytes and failed
		 * for a reason nothing in it mentions. */
		bt_put_le16(big, L2CAP_MTU + 1);
		bt_put_le16(big + 2, L2CAP_CID_SIGNALLING);
		bt_put_le16(big + 4, 0);
		bt_put_le16(big + 6, 0);

		if (l2cap_acl_feed(&r, 0x0001, ACL_PB_START_FLUSHABLE, big,
				   8) || r.oversized != 1) {
			kprintf("  l2cap: a PDU declaring %u bytes was "
				"accepted against a %u-byte buffer\n",
				(unsigned)(L2CAP_MTU + 1),
				(unsigned)sizeof(r.buf));
			ok = false;
		}
	}

	/* --- signalling -------------------------------------------------- */
	n = l2cap_connect_request_build(out, 0x42, L2CAP_PSM_HID_INTERRUPT,
					0x0040);

	if (n != 8 || out[0] != L2CAP_SIG_CONNECT_REQUEST || out[1] != 0x42 ||
	    out[2] != 4 || out[4] != 0x13 || out[6] != 0x40) {
		kprintf("  l2cap: a Connection Request for the HID interrupt "
			"channel came out as %u bytes, code %02x id %02x psm "
			"%02x%02x\n", n, out[0], out[1], out[5], out[4]);
		ok = false;
	}

	if (!l2cap_signal_parse(out, n, &sig) ||
	    sig.code != L2CAP_SIG_CONNECT_REQUEST || sig.id != 0x42 ||
	    sig.length != 4) {
		kputs("  l2cap: a Connection Request did not read back as "
		      "one\n");
		ok = false;
	}

	/* A command claiming more than arrived. */
	if (l2cap_signal_parse(out, n - 1, &sig)) {
		kputs("  l2cap: a signalling command claiming more data than "
		      "arrived was accepted\n");
		ok = false;
	}

	/* --- a Connection Response --------------------------------------- */
	{
		u8 body[8];

		bt_put_le16(body, 0x0041);		/* their channel */
		bt_put_le16(body + 2, 0x0040);	/* ours */
		bt_put_le16(body + 4, L2CAP_CONN_SUCCESS);
		bt_put_le16(body + 6, 0);

		if (!l2cap_connect_response_parse(body, 8, &a, &b, &c, &d) ||
		    a != 0x0041 || b != 0x0040 || c != L2CAP_CONN_SUCCESS) {
			kprintf("  l2cap: a Connection Response read as dcid "
				"%04x scid %04x result %04x\n", a, b, c);
			ok = false;
		}

		if (l2cap_connect_response_parse(body, 7, &a, &b, &c, &d)) {
			kputs("  l2cap: a seven-byte Connection Response was "
			      "accepted\n");
			ok = false;
		}

		/* Pending is not a refusal, and the constants must differ or
		 * nothing downstream can tell them apart. */
		if (L2CAP_CONN_PENDING == L2CAP_CONN_SUCCESS ||
		    L2CAP_CONN_PENDING == L2CAP_CONN_REFUSED_PSM) {
			kputs("  l2cap: pending is indistinguishable from "
			      "success or refusal\n");
			ok = false;
		}
	}

	/* --- configuration options --------------------------------------- */
	{
		u16 mtu = 0;
		static const u8 with_mtu[4] = { L2CAP_CONF_OPT_MTU, 2,
						0xA0, 0x02 };	/* 672 */

		if (!l2cap_config_find_mtu(with_mtu, 4, &mtu) || mtu != 672) {
			kprintf("  l2cap: the MTU option read as %u, expected "
				"672\n", mtu);
			ok = false;
		}

		/* The same option marked as a hint. The type is the same
		 * option; only the top bit differs. */
		{
			u8 hinted[4];

			kmemcpy(hinted, with_mtu, 4);
			hinted[0] |= L2CAP_CONF_OPT_HINT;
			mtu = 0;

			if (!l2cap_config_find_mtu(hinted, 4, &mtu) ||
			    mtu != 672) {
				kputs("  l2cap: an MTU option marked as a "
				      "hint was not recognised -- the top bit "
				      "of the type is a hint flag, not part "
				      "of the type\n");
				ok = false;
			}
		}

		/* An option declaring zero length. The walk must move past it.
		 * Getting this wrong does not return a wrong number -- it
		 * never returns. */
		{
			static const u8 zero_len[6] = { L2CAP_CONF_OPT_QOS, 0,
							L2CAP_CONF_OPT_MTU, 2,
							0xA0, 0x02 };
			mtu = 0;

			if (!l2cap_config_find_mtu(zero_len, 6, &mtu) ||
			    mtu != 672) {
				kputs("  l2cap: an option declaring zero "
				      "length stopped the walk finding the "
				      "MTU after it\n");
				ok = false;
			}
		}

		/* An option declaring more than is left must not read past the
		 * end. */
		{
			static const u8 runs_over[4] = { L2CAP_CONF_OPT_MTU,
							 200, 0xA0, 0x02 };
			mtu = 0;

			if (l2cap_config_find_mtu(runs_over, 4, &mtu)) {
				kputs("  l2cap: an option declaring 200 bytes "
				      "inside a 4-byte list produced a "
				      "value\n");
				ok = false;
			}
		}

		/* No MTU option at all is not a failure to parse. */
		{
			static const u8 none[4] = { L2CAP_CONF_OPT_FLUSH_TIMEOUT,
						    2, 0xFF, 0xFF };
			mtu = 0;

			if (l2cap_config_find_mtu(none, 4, &mtu)) {
				kputs("  l2cap: an option list with no MTU in "
				      "it produced one\n");
				ok = false;
			}
		}
	}

	/* --- getting a channel open --------------------------------------
	 *
	 * The whole handshake, driven against synthetic replies, checking at
	 * each step for what must *not* happen as much as what must.
	 */
	{
		struct l2cap_channel ch;
		u8 tx[32], rx[32];
		u32 n;

		/* Build a Connection Response with a given identifier, source
		 * CID and result. Used several times below. */
		#define CONN_RSP(ident, dcid_, scid_, res)			\
			do {						\
				u8 body_[8];				\
				bt_put_le16(body_, (dcid_));		\
				bt_put_le16(body_ + 2, (scid_));		\
				bt_put_le16(body_ + 4, (res));		\
				bt_put_le16(body_ + 6, 0);			\
				n = l2cap_signal_build(rx,		\
					L2CAP_SIG_CONNECT_RESPONSE,	\
					(ident), body_, sizeof(body_));	\
			} while (0)

		l2cap_channel_init(&ch, L2CAP_PSM_HID_INTERRUPT, 0x0040);
		n = l2cap_channel_start(&ch, tx, 0x01);

		if (n != 8 || ch.state != L2CAP_CH_WAIT_CONNECT) {
			kputs("  l2cap: starting a channel did not produce a "
			      "Connection Request\n");
			ok = false;
		}

		/* Pending: not success, not refusal. Nothing may move. */
		CONN_RSP(0x01, 0x0041, 0x0040, L2CAP_CONN_PENDING);

		if (l2cap_channel_input(&ch, rx, n, tx, sizeof(tx)) ||
		    ch.state != L2CAP_CH_WAIT_CONNECT) {
			kputs("  l2cap: a pending Connection Response moved "
			      "the channel; pending means the peer is still "
			      "deciding and another response follows\n");
			ok = false;
		}

		/* An answer to a different request. */
		CONN_RSP(0x7F, 0x0041, 0x0040, L2CAP_CONN_SUCCESS);

		if (l2cap_channel_input(&ch, rx, n, tx, sizeof(tx)) ||
		    ch.state != L2CAP_CH_WAIT_CONNECT || ch.wrong_ident != 1) {
			kputs("  l2cap: a Connection Response carrying "
			      "somebody else's identifier was acted on -- the "
			      "identifier is the only thing pairing a "
			      "response with its request\n");
			ok = false;
		}

		/* An answer about a different channel. */
		CONN_RSP(0x01, 0x0041, 0x00FF, L2CAP_CONN_SUCCESS);

		if (l2cap_channel_input(&ch, rx, n, tx, sizeof(tx)) ||
		    ch.state != L2CAP_CH_WAIT_CONNECT ||
		    ch.wrong_channel != 1) {
			kputs("  l2cap: a Connection Response naming another "
			      "channel was acted on\n");
			ok = false;
		}

		/* The real one, which must draw a Configure Request. */
		CONN_RSP(0x01, 0x0041, 0x0040, L2CAP_CONN_SUCCESS);
		n = l2cap_channel_input(&ch, rx, n, tx, sizeof(tx));

		if (ch.state != L2CAP_CH_WAIT_CONFIG || ch.dcid != 0x0041) {
			kprintf("  l2cap: after success the channel is in "
				"state %u with dcid %04x, expected config and "
				"0041\n", (unsigned)ch.state, ch.dcid);
			ok = false;
		}

		if (n != 12 || tx[0] != L2CAP_SIG_CONFIG_REQUEST) {
			kprintf("  l2cap: the reply to a successful "
				"Connection Response was %u bytes of code "
				"%02x, expected a 12-byte Configure Request\n",
				n, tx[0]);
			ok = false;
		}

		/* **The trap.** Their Configure Response is one half. */
		{
			u8 body[6];

			bt_put_le16(body, 0x0040);
			bt_put_le16(body + 2, 0);
			bt_put_le16(body + 4, L2CAP_CONF_SUCCESS);
			n = l2cap_signal_build(rx, L2CAP_SIG_CONFIG_RESPONSE,
					       tx[1], body, sizeof(body));
		}

		l2cap_channel_input(&ch, rx, n, tx, sizeof(tx));

		if (!ch.config_out_done) {
			kputs("  l2cap: a successful Configure Response did "
			      "not finish this side's half\n");
			ok = false;
		}

		if (ch.state == L2CAP_CH_OPEN) {
			kputs("  l2cap: the channel opened on this side's "
			      "configuration alone -- the peer has not sent "
			      "its own Configure Request yet, and a channel "
			      "opened here usually works, which is the "
			      "problem\n");
			ok = false;
		}

		/* Their Configure Request. Answering it is the other half. */
		{
			u8 body[8];

			bt_put_le16(body, 0x0040);
			bt_put_le16(body + 2, 0);
			body[4] = L2CAP_CONF_OPT_MTU;
			body[5] = 2;
			bt_put_le16(body + 6, 512);
			n = l2cap_signal_build(rx, L2CAP_SIG_CONFIG_REQUEST,
					       0x20, body, sizeof(body));
		}

		n = l2cap_channel_input(&ch, rx, n, tx, sizeof(tx));

		if (n != 10 || tx[0] != L2CAP_SIG_CONFIG_RESPONSE ||
		    tx[1] != 0x20) {
			kprintf("  l2cap: their Configure Request drew %u "
				"bytes of code %02x id %02x, expected a "
				"10-byte Configure Response echoing id 20\n",
				n, tx[0], tx[1]);
			ok = false;
		}

		if (ch.peer_mtu != 512) {
			kprintf("  l2cap: the peer's MTU read as %u, expected "
				"512\n", ch.peer_mtu);
			ok = false;
		}

		if (ch.state != L2CAP_CH_OPEN) {
			kprintf("  l2cap: both directions are configured and "
				"the channel is in state %u, not open\n",
				(unsigned)ch.state);
			ok = false;
		}

		/* --- the other order, which is just as legal ---
		 *
		 * Their Configure Request may arrive before this side's
		 * Configure Response. Refusing to answer it until answered
		 * deadlocks the pair.
		 */
		l2cap_channel_init(&ch, L2CAP_PSM_HID_CONTROL, 0x0040);
		l2cap_channel_start(&ch, tx, 0x01);
		CONN_RSP(0x01, 0x0041, 0x0040, L2CAP_CONN_SUCCESS);
		l2cap_channel_input(&ch, rx, n, tx, sizeof(tx));

		{
			u8 body[4];

			bt_put_le16(body, 0x0040);
			bt_put_le16(body + 2, 0);
			n = l2cap_signal_build(rx, L2CAP_SIG_CONFIG_REQUEST,
					       0x21, body, sizeof(body));
		}

		if (!l2cap_channel_input(&ch, rx, n, tx, sizeof(tx))) {
			kputs("  l2cap: their Configure Request arriving "
			      "first was not answered, and both sides would "
			      "wait for each other\n");
			ok = false;
		}

		if (ch.state == L2CAP_CH_OPEN) {
			kputs("  l2cap: the channel opened on their "
			      "configuration alone\n");
			ok = false;
		}

		/* --- a Configure Request for a different channel ---------
		 *
		 * **Every test above this one has a single channel**, and a
		 * single channel cannot show a message being answered by the
		 * wrong one. This case was missing for exactly that reason,
		 * and the fault it covers was found by opening two channels
		 * on one link in `bt_stack.c` -- the control channel, open
		 * first, answered the interrupt channel's Configure Request
		 * and left it waiting for a configuration already consumed.
		 */
		{
			u8 body[4];
			u64 before;

			/* An open channel, so it is eligible to answer. */
			l2cap_channel_init(&ch, L2CAP_PSM_HID_CONTROL, 0x0040);
			l2cap_channel_start(&ch, tx, 0x01);
			CONN_RSP(0x01, 0x0041, 0x0040, L2CAP_CONN_SUCCESS);
			l2cap_channel_input(&ch, rx, n, tx, sizeof(tx));
			ch.config_out_done = true;
			ch.config_in_done = true;
			ch.state = L2CAP_CH_OPEN;
			before = ch.wrong_channel;

			/* Addressed to 0x0041, which is not this channel. */
			bt_put_le16(body, 0x0041);
			bt_put_le16(body + 2, 0);
			n = l2cap_signal_build(rx, L2CAP_SIG_CONFIG_REQUEST,
					       0x40, body, sizeof(body));

			if (l2cap_channel_input(&ch, rx, n, tx, sizeof(tx)) ||
			    ch.wrong_channel != before + 1) {
				kputs("  l2cap: a Configure Request naming "
				      "another channel was answered -- the "
				      "channel it was meant for then waits "
				      "for a configuration somebody else "
				      "consumed\n");
				ok = false;
			}

			/* And its own is still answered. */
			bt_put_le16(body, 0x0040);
			n = l2cap_signal_build(rx, L2CAP_SIG_CONFIG_REQUEST,
					       0x41, body, sizeof(body));

			if (!l2cap_channel_input(&ch, rx, n, tx,
						 sizeof(tx))) {
				kputs("  l2cap: the check on the destination "
				      "channel also refused this channel's "
				      "own Configure Request\n");
				ok = false;
			}
		}

		/* --- refusal --- */
		l2cap_channel_init(&ch, L2CAP_PSM_HID_CONTROL, 0x0040);
		l2cap_channel_start(&ch, tx, 0x05);
		CONN_RSP(0x05, 0x0000, 0x0040, L2CAP_CONN_REFUSED_PSM);

		if (l2cap_channel_input(&ch, rx, n, tx, sizeof(tx))) {
			kputs("  l2cap: a refused connection produced a "
			      "reply\n");
			ok = false;
		}

		if (ch.state != L2CAP_CH_REFUSED ||
		    ch.refused_result != L2CAP_CONN_REFUSED_PSM) {
			kprintf("  l2cap: a refusal left the channel in state "
				"%u with result %04x\n", (unsigned)ch.state,
				ch.refused_result);
			ok = false;
		}

		/* --- a disconnect closes it --- */
		l2cap_channel_init(&ch, L2CAP_PSM_HID_CONTROL, 0x0040);
		l2cap_channel_start(&ch, tx, 0x01);
		CONN_RSP(0x01, 0x0041, 0x0040, L2CAP_CONN_SUCCESS);
		l2cap_channel_input(&ch, rx, n, tx, sizeof(tx));

		{
			u8 body[4];

			bt_put_le16(body, 0x0040);
			bt_put_le16(body + 2, 0x0041);
			n = l2cap_signal_build(rx,
					       L2CAP_SIG_DISCONNECT_REQUEST,
					       0x30, body, sizeof(body));
		}

		/* **First, one for a different channel.** It must not close
		 * this one and must not be answered -- answering on another
		 * channel's behalf means the channel actually being closed
		 * never hears back, while this one is torn down instead. */
		{
			u8 other[4];
			u64 before = ch.wrong_channel;
			u32 m;

			bt_put_le16(other, 0x0099);	/* not this channel */
			bt_put_le16(other + 2, 0x0041);
			m = l2cap_signal_build(rx,
					       L2CAP_SIG_DISCONNECT_REQUEST,
					       0x2F, other, sizeof(other));

			if (l2cap_channel_input(&ch, rx, m, tx, sizeof(tx)) ||
			    ch.wrong_channel != before + 1) {
				kputs("  l2cap: a Disconnection Request for "
				      "another channel was answered\n");
				ok = false;
			}

			if (ch.state == L2CAP_CH_CLOSED) {
				kputs("  l2cap: a Disconnection Request for "
				      "another channel closed this one -- a "
				      "working channel torn down by a message "
				      "that was never about it\n");
				ok = false;
			}
		}

		/* Rebuilt, because the block above wrote its own request into
		 * the same buffer. The first version of this did not, and the
		 * legitimate request below was parsed out of the bytes the
		 * hostile one had left there -- which failed loudly rather
		 * than quietly, but only by luck. */
		{
			u8 body[4];

			bt_put_le16(body, 0x0040);
			bt_put_le16(body + 2, 0x0041);
			n = l2cap_signal_build(rx,
					       L2CAP_SIG_DISCONNECT_REQUEST,
					       0x30, body, sizeof(body));
		}

		n = l2cap_channel_input(&ch, rx, n, tx, sizeof(tx));

		if (n != 8 || tx[0] != L2CAP_SIG_DISCONNECT_RESPONSE ||
		    tx[1] != 0x30) {
			kprintf("  l2cap: a Disconnection Request drew %u "
				"bytes of code %02x, expected an 8-byte "
				"Disconnection Response\n", n, tx[0]);
			ok = false;
		}

		if (ch.state != L2CAP_CH_CLOSED) {
			kputs("  l2cap: a disconnected channel is not "
			      "closed\n");
			ok = false;
		}

		#undef CONN_RSP
	}

	/* --- a Configure Request carries the MTU we can actually take ----
	 *
	 * The option sits at out[8] and its layout is type, length, value --
	 * so the length is out[9], not out[8]. This check was written with
	 * those two the wrong way round and failed on its first run, which is
	 * the test doing its job on itself. The message below prints every
	 * byte of the option rather than only the MTU: the first version
	 * printed the MTU, which was correct, and said nothing about the byte
	 * that had actually failed.
	 */
	n = l2cap_config_request_build(out, 0x43, 0x0041, L2CAP_MTU);

	if (n != 12 || out[0] != L2CAP_SIG_CONFIG_REQUEST ||
	    out[8] != L2CAP_CONF_OPT_MTU || out[9] != 2 ||
	    bt_get_le16(out + 10) != L2CAP_MTU) {
		kprintf("  l2cap: a Configure Request came out as %u bytes, "
			"code %02x, option type %02x length %02x value %u -- "
			"expected 12 bytes, code %02x, type %02x length 02 "
			"value %u\n", n, out[0], out[8], out[9],
			bt_get_le16(out + 10),
			(unsigned)L2CAP_SIG_CONFIG_REQUEST,
			(unsigned)L2CAP_CONF_OPT_MTU, (unsigned)L2CAP_MTU);
		ok = false;
	}

	/* And that constant must actually be what the buffer holds, or the
	 * peer is told a number this kernel cannot honour. */
	if ((u32)L2CAP_MTU + L2CAP_HEADER != sizeof(r.buf)) {
		kprintf("  l2cap: the advertised MTU (%u) and the reassembly "
			"buffer (%u) disagree\n", (unsigned)L2CAP_MTU,
			(unsigned)sizeof(r.buf));
		ok = false;
	}

	/* --- the boundaries, which every case above stays clear of -------
	 *
	 * A mutation run turned each `<` in this file into `<=` and each `>`
	 * into `>=` and found that the exact sizes are never tried: the
	 * refusals are all tested with input well short of the limit and the
	 * acceptances with input well inside it. What that hides is a guard
	 * that has moved by one, which refuses a legal message rather than
	 * accepting an illegal one -- a device that works for everybody
	 * except when a PDU happens to end flush.
	 */

	/* A fragment carrying nothing. `l2cap_acl_feed`'s guard against one
	 * had no test, and reporting a complete PDU here hands the caller
	 * whatever the last one left in the buffer. */
	{
		struct l2cap_reassembly z;

		/* Zeroed, not merely reset: `l2cap_reassembly_reset` clears
		 * `have` and `want` and deliberately leaves the counters
		 * alone, because they are cumulative for the boot log. A
		 * local one starts as whatever was on the stack. */
		kmemset(&z, 0, sizeof(z));
		l2cap_reassembly_reset(&z);

		if (l2cap_acl_feed(&z, 0x000C, 2, pdu, 0)) {
			kputs("  l2cap: a zero-length fragment was reported "
			      "as a complete PDU\n");
			ok = false;
		}

		if (z.have) {
			kprintf("  l2cap: a zero-length fragment left %u "
				"bytes in the buffer\n", z.have);
			ok = false;
		}
	}

	/* A signalling command whose header is the whole of it: four bytes,
	 * declaring no data. `len < L2CAP_SIG_HEADER` as `<=` refuses every
	 * one of those, and the parser's job is the length rather than the
	 * meaning -- which command it is does not matter here, so this uses
	 * one of the codes this file already defines rather than naming a
	 * code nothing has checked. */
	{
		u8 bare[L2CAP_SIG_HEADER];

		bare[0] = L2CAP_SIG_DISCONNECT_RESPONSE;
		bare[1] = 0x07;
		bt_put_le16(bare + 2, 0);

		if (!l2cap_signal_parse(bare, sizeof(bare), &sig)) {
			kputs("  l2cap: a four-byte signalling command was "
			      "refused, though its header is the whole of "
			      "it\n");
			ok = false;
		} else if (sig.code != L2CAP_SIG_DISCONNECT_RESPONSE ||
			   sig.id != 7 || sig.length) {
			kprintf("  l2cap: a bare command read as code %02x "
				"id %u length %u, expected %02x, 7 and 0\n",
				sig.code, sig.id, sig.length,
				(unsigned)L2CAP_SIG_DISCONNECT_RESPONSE);
			ok = false;
		}
	}

	/* A PDU of exactly the advertised MTU, in one fragment that exactly
	 * fills the buffer. **This is the size this kernel told the peer it
	 * would accept**, so refusing it makes the Configure Request a lie,
	 * and it is the one size no other case here comes near. Both the
	 * fragment guard and the declared-length guard sit on it. */
	{
		static u8 full[L2CAP_PDU_MAX];
		struct l2cap_reassembly z;
		unsigned i;

		bt_put_le16(full, L2CAP_MTU);
		bt_put_le16(full + 2, 0x0041);

		for (i = L2CAP_HEADER; i < sizeof(full); i++)
			full[i] = (u8)i;

		kmemset(&z, 0, sizeof(z));
		l2cap_reassembly_reset(&z);

		if (!l2cap_acl_feed(&z, 0x000C, 2, full, sizeof(full))) {
			kprintf("  l2cap: a PDU of exactly the advertised "
				"%u-byte MTU was refused, though that is the "
				"number this kernel negotiates\n",
				(unsigned)L2CAP_MTU);
			ok = false;
		} else if (z.want != sizeof(full) || z.oversized) {
			kprintf("  l2cap: a maximum PDU came back wanting %u "
				"bytes with %u counted oversized, expected "
				"%u and 0\n", z.want, (unsigned)z.oversized,
				(unsigned)sizeof(full));
			ok = false;
		}

		/* And one byte more is not merely rejected -- it must be
		 * counted, or a device ignoring the negotiation is invisible
		 * in the boot log. */
		l2cap_reassembly_reset(&z);
		bt_put_le16(full, L2CAP_MTU + 1);

		if (l2cap_acl_feed(&z, 0x000C, 2, full, sizeof(full))) {
			kputs("  l2cap: a PDU declaring one byte more than "
			      "the MTU was accepted\n");
			ok = false;
		}

		if (!z.oversized) {
			kputs("  l2cap: an oversized PDU was refused and not "
			      "counted\n");
			ok = false;
		}
	}

	/* The restart counter, which is a diagnostic and therefore has to be
	 * right about *why* it fired. A first PDU is not a restart, and
	 * neither is one that follows a finished one -- only one that
	 * abandons bytes still in flight. */
	{
		struct l2cap_reassembly z;
		u8 small[8];

		bt_put_le16(small, 4);			/* four of payload */
		bt_put_le16(small + 2, 0x0041);
		small[4] = 0xA1;
		small[5] = 0x01;
		small[6] = 0x02;
		small[7] = 0x03;

		kmemset(&z, 0, sizeof(z));
		l2cap_reassembly_reset(&z);

		if (!l2cap_acl_feed(&z, 0x000C, 2, small, sizeof(small)) ||
		    z.restarts) {
			kprintf("  l2cap: the first PDU on a fresh "
				"reassembly counted %u restarts\n",
				(unsigned)z.restarts);
			ok = false;
		}

		/* A second, complete, following a complete one. Nothing was
		 * abandoned, so nothing was restarted. */
		if (!l2cap_acl_feed(&z, 0x000C, 2, small, sizeof(small)) ||
		    z.restarts) {
			kprintf("  l2cap: a PDU after a finished one counted "
				"%u restarts -- the count is measuring "
				"something other than lost bytes\n",
				(unsigned)z.restarts);
			ok = false;
		}

		/* And one that genuinely abandons a half-arrived PDU must
		 * count, or the check above is satisfied by a counter that
		 * never moves. */
		l2cap_reassembly_reset(&z);
		l2cap_acl_feed(&z, 0x000C, 2, small, 6);   /* half of it */
		l2cap_acl_feed(&z, 0x000C, 2, small, sizeof(small));

		if (z.restarts != 1) {
			kprintf("  l2cap: abandoning a half-arrived PDU "
				"counted %u restarts, expected 1\n",
				(unsigned)z.restarts);
			ok = false;
		}
	}

	return ok;
}
