/* Joining the layers: an ACL fragment in, a moved pointer out.
 *
 * See `bt_mouse.h` for the chain and for what is deliberately not here. This
 * file is the joins, which is the only part not already covered by somebody
 * else's tests.
 */
#include <recon/kernel/bt_hid.h>
#include <recon/kernel/bt_mouse.h>
#include <recon/kernel/console.h>
#include <recon/kernel/hid_report.h>
#include <recon/kernel/input.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/l2cap.h>

bool bt_mouse_configure(struct bt_mouse *m, const u8 *descriptor, u32 len,
			u16 acl_handle, u16 intr_cid)
{
	/* Static rather than automatic: the parse result carries a 32-field
	 * map, which is most of a kilobyte, and this can be called from a
	 * context whose stack has other plans. One mouse is configured at a
	 * time. */
	static struct hid_report_info info;

	kmemset(m, 0, sizeof(*m));
	l2cap_reassembly_reset(&m->rx);

	if (!hid_report_parse(descriptor, len, &info))
		return false;

	/* A withheld field map is not a map to build a layout from, and
	 * `hid_report_mouse_layout` refuses it -- but checking here as well
	 * means the reason can be seen at the point a device is turned away
	 * rather than inferred from a false. */
	if (!info.fields_usable)
		return false;

	if (!hid_report_mouse_layout(&info, &m->layout))
		return false;

	/* **The answer travelling from where it is known to where it is
	 * needed.** `bt_hid_input_report` takes this as an argument precisely
	 * so that it arrives from a descriptor rather than from a guess, and
	 * this line is the whole reason both were written that way. */
	m->uses_report_id = info.uses_report_id;

	m->acl_handle = acl_handle;
	m->intr_cid = intr_cid;
	m->ready = true;

	return true;
}

bool bt_mouse_acl(struct bt_mouse *m, u16 handle, u8 pb, const u8 *payload,
		  u32 len, struct hid_mouse_state *out)
{
	struct l2cap_header hdr;
	struct bt_hid_message msg;
	struct bt_hid_report rep;

	if (!m->ready)
		return false;

	/* Another link's fragment. `l2cap_acl_feed` would refuse it too, but
	 * it would also count it as a crossed fragment on this mouse's
	 * reassembly, which reads as a fault rather than as traffic that was
	 * never ours. */
	if (handle != m->acl_handle) {
		m->other_channel++;
		return false;
	}

	if (!l2cap_acl_feed(&m->rx, handle, pb, payload, len))
		return false;

	/* A whole PDU. Its first four bytes are the L2CAP header. */
	if (!l2cap_header_parse(m->rx.buf, m->rx.want, &hdr))
		return false;

	/* Signalling, or the control channel, or another device's stream on
	 * the same link. Not this mouse's reports, and not an error. */
	if (hdr.cid != m->intr_cid) {
		m->other_channel++;
		return false;
	}

	/* Past the header is the HIDP message. `hdr.length` rather than
	 * `want` because the length excludes the header and using the wrong
	 * one here is an off-by-four that still decodes. */
	if (!bt_hid_parse(m->rx.buf + L2CAP_HEADER, hdr.length, &msg)) {
		m->malformed++;
		return false;
	}

	if (!bt_hid_input_report(&msg, m->uses_report_id, &rep)) {
		/* A handshake, a control message, an output report echoed
		 * back. Ordinary, and not a report. */
		m->not_input++;
		return false;
	}

	if (!hid_mouse_decode(&m->layout, rep.data, rep.length, out)) {
		/* The device sent fewer bytes than its own descriptor
		 * promised. */
		m->malformed++;
		return false;
	}

	m->reports++;
	return true;
}

void bt_mouse_post(const struct bt_mouse *m, const struct hid_mouse_state *s)
{
	static const u16 button[3] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE };
	unsigned i;
	unsigned n = m->layout.buttons_count;

	if (n > 3)
		n = 3;

	/* Motion first, then buttons.
	 *
	 * A click is a position and a state, and a consumer reading the two
	 * events in order should see the pointer where it was clicked rather
	 * than where it was a report ago. Posting the button first puts the
	 * press at the old position for anything that reads events one at a
	 * time. */
	if (s->x)
		input_post_motion(REL_X, s->x);

	if (s->y)
		input_post_motion(REL_Y, s->y);

	if (s->wheel)
		input_post_motion(REL_WHEEL, s->wheel);

	for (i = 0; i < n; i++) {
		bool down = ((s->buttons >> i) & 1u) != 0;

		/* Asked of the input layer rather than remembered here, so
		 * that two mice cannot disagree about whether a button is
		 * down -- the same reasoning `usb_hid.c` gives. */
		if (down != input_key_held(button[i]))
			input_post(button[i], down ? INPUT_PRESS
						   : INPUT_RELEASE);
	}
}

/* --- the self-test --------------------------------------------------------
 *
 * The first test on this branch that runs the whole chain. Every layer below
 * is already covered on its own, so what is checked here is the joins:
 *
 *   - a descriptor's answer about report IDs actually reaches the HIDP layer;
 *   - the L2CAP length, which excludes its header, is the one handed on;
 *   - a CID that is not the interrupt channel is not decoded;
 *   - a fragment from another link is not decoded;
 *   - a report split across two ACL fragments still arrives once.
 *
 * The descriptor used is the real one from `hid_report.c` -- five buttons,
 * sixteen-bit axes -- so the values here are the ones a real mouse produces.
 */
bool bt_mouse_self_test(void)
{
	static struct bt_mouse m;
	struct hid_mouse_state s;
	bool ok = true;

	/* The same 87 bytes read off 3554:f54f. Kept here rather than shared
	 * so that neither file's test can be broken by the other's edit. */
	static const u8 real_mouse[] = {
		0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01,
		0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x05,
		0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01,
		0x81, 0x02, 0x95, 0x01, 0x75, 0x03, 0x81, 0x01,
		0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x00,
		0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02,
		0x81, 0x06, 0xC0, 0xA1, 0x00, 0x05, 0x01, 0x09,
		0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95,
		0x01, 0x81, 0x06, 0xC0, 0xA1, 0x00, 0x05, 0x0C,
		0x0A, 0x38, 0x02, 0x95, 0x01, 0x75, 0x08, 0x15,
		0x81, 0x25, 0x7F, 0x81, 0x06, 0xC0, 0xC0
	};

	/* One report on the wire: L2CAP header, HIDP byte, seven report
	 * bytes. Buttons 1 and 3, x = +300, y = -300, wheel -1.
	 *
	 *   08 00      length 8 -- the HIDP byte and seven report bytes,
	 *              *not* counting these four
	 *   41 00      CID 0x0041
	 *   a1         DATA | INPUT
	 *   05 2c 01 d4 fe ff 00
	 */
	static const u8 pdu[12] = {
		0x08, 0x00, 0x41, 0x00,
		0xA1,
		0x05, 0x2C, 0x01, 0xD4, 0xFE, 0xFF, 0x00
	};

	if (!bt_mouse_configure(&m, real_mouse, sizeof(real_mouse), 0x000C,
				0x0041)) {
		kputs("  btmouse: a real mouse's descriptor did not "
		      "configure\n");
		return false;
	}

	if (m.uses_report_id) {
		kputs("  btmouse: the descriptor says no report ids and the "
		      "mouse was configured for them\n");
		ok = false;
	}

	if (m.layout.report_bytes != 7 || m.layout.buttons_count != 5) {
		kprintf("  btmouse: the layout came through as %u bytes and "
			"%u buttons, expected 7 and 5\n",
			m.layout.report_bytes, m.layout.buttons_count);
		ok = false;
	}

	/* --- one whole report in one fragment --------------------------- */
	if (!bt_mouse_acl(&m, 0x000C, ACL_PB_START_FLUSHABLE, pdu,
			  sizeof(pdu), &s)) {
		kputs("  btmouse: a complete report in one ACL fragment did "
		      "not decode\n");
		ok = false;
	} else if (s.buttons != 0x05 || s.x != 300 || s.y != -300 ||
		   s.wheel != -1) {
		kprintf("  btmouse: decoded buttons %02x x=%d y=%d wheel=%d, "
			"expected 05, 300, -300, -1\n", s.buttons, s.x, s.y,
			s.wheel);
		ok = false;
	}

	/* --- the same report, split across two fragments ---------------- */
	{
		unsigned completions = 0;

		if (bt_mouse_acl(&m, 0x000C, ACL_PB_START_FLUSHABLE, pdu, 5,
				 &s))
			completions++;

		if (bt_mouse_acl(&m, 0x000C, ACL_PB_CONTINUATION, pdu + 5, 7,
				 &s))
			completions++;

		if (completions != 1) {
			kprintf("  btmouse: a report split across two "
				"fragments decoded %u times, not once\n",
				completions);
			ok = false;
		} else if (s.x != 300 || s.y != -300) {
			kprintf("  btmouse: reassembled across fragments it "
				"decoded x=%d y=%d, expected 300 and -300\n",
				s.x, s.y);
			ok = false;
		}
	}

	/* --- another link's fragment ------------------------------------ */
	{
		u64 before = m.reports;

		if (bt_mouse_acl(&m, 0x00FF, ACL_PB_START_FLUSHABLE, pdu,
				 sizeof(pdu), &s) || m.reports != before) {
			kputs("  btmouse: a fragment from another ACL link "
			      "was decoded as this mouse's\n");
			ok = false;
		}
	}

	/* --- the signalling channel, which shares the link -------------- */
	{
		u8 sig[12];
		u64 before = m.reports;

		kmemcpy(sig, pdu, sizeof(sig));
		sig[2] = 0x01;			/* CID 1: signalling */
		sig[3] = 0x00;

		if (bt_mouse_acl(&m, 0x000C, ACL_PB_START_FLUSHABLE, sig,
				 sizeof(sig), &s) || m.reports != before) {
			kputs("  btmouse: a PDU on the signalling channel was "
			      "decoded as a mouse report -- every channel on "
			      "a link arrives here\n");
			ok = false;
		}
	}

	/* --- a HIDP message that is not an input report ----------------- */
	{
		u8 hs[12];
		u64 before = m.reports;

		kmemcpy(hs, pdu, sizeof(hs));
		hs[4] = bt_hid_header(HIDP_TRANS_HANDSHAKE,
				      HIDP_HSHK_SUCCESSFUL);

		if (bt_mouse_acl(&m, 0x000C, ACL_PB_START_FLUSHABLE, hs,
				 sizeof(hs), &s) || m.reports != before) {
			kputs("  btmouse: a handshake was decoded as a mouse "
			      "report\n");
			ok = false;
		}

		if (!m.not_input) {
			kputs("  btmouse: a handshake was not counted as a "
			      "non-report\n");
			ok = false;
		}
	}

	/* --- a report shorter than the descriptor promised -------------- */
	{
		u8 short_pdu[9];
		u64 before = m.reports;

		/* Length 5: the HIDP byte and four report bytes, where the
		 * layout wants seven. */
		short_pdu[0] = 0x05;
		short_pdu[1] = 0x00;
		short_pdu[2] = 0x41;
		short_pdu[3] = 0x00;
		short_pdu[4] = 0xA1;
		short_pdu[5] = 0x05;
		short_pdu[6] = 0x2C;
		short_pdu[7] = 0x01;
		short_pdu[8] = 0xD4;

		if (bt_mouse_acl(&m, 0x000C, ACL_PB_START_FLUSHABLE,
				 short_pdu, sizeof(short_pdu), &s) ||
		    m.reports != before) {
			kputs("  btmouse: a report shorter than the layout "
			      "was decoded -- the axes would come from bytes "
			      "the device did not send\n");
			ok = false;
		}

		if (!m.malformed) {
			kputs("  btmouse: a short report was not counted as "
			      "malformed\n");
			ok = false;
		}
	}

	/* --- a device this cannot drive is refused ---------------------- */
	{
		static struct bt_mouse keyboard;
		static const u8 array_input[] = {
			0xA1, 0x01,
			0x75, 0x08,
			0x95, 0x06,
			0x81, 0x00,		/* Input (Data, Array) */
			0xC0
		};

		if (bt_mouse_configure(&keyboard, array_input,
				       sizeof(array_input), 0x000C, 0x0041)) {
			kputs("  btmouse: a descriptor whose field map is "
			      "withheld was configured as a mouse\n");
			ok = false;
		}
	}

	/* --- and the report-id answer actually travels ------------------
	 *
	 * The join this file exists for. A descriptor declaring a report ID
	 * must leave the mouse expecting one, and a report carrying the id
	 * byte must then decode to the same values as one without it does
	 * from a descriptor that declares none.
	 */
	{
		static struct bt_mouse with_id;
		static const u8 id_desc[] = {
			0x05, 0x01, 0x09, 0x02,
			0xA1, 0x01,
			0x85, 0x03,		/*   Report ID (3)        */
			0x05, 0x09,
			0x19, 0x01, 0x29, 0x03,
			0x95, 0x03, 0x75, 0x01,
			0x81, 0x02,		/*   3 buttons            */
			0x95, 0x01, 0x75, 0x05,
			0x81, 0x01,		/*   5 bits padding       */
			0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
			0x15, 0x81, 0x25, 0x7F,
			0x75, 0x08, 0x95, 0x02,
			0x81, 0x06,		/*   X and Y, 8 bits      */
			0xC0
		};

		if (!bt_mouse_configure(&with_id, id_desc, sizeof(id_desc),
					0x000C, 0x0041)) {
			kputs("  btmouse: a descriptor with a report id did "
			      "not configure\n");
			ok = false;
		} else {
			if (!with_id.uses_report_id) {
				kputs("  btmouse: a descriptor declaring a "
				      "report id configured a mouse that "
				      "does not expect one -- the answer did "
				      "not travel\n");
				ok = false;
			}

			/* Three buttons and five bits of padding make a byte,
			 * then two eight-bit axes: three bytes, measured
			 * after the report-id byte the HIDP layer strips. */
			if (with_id.layout.report_bytes != 3) {
				kprintf("  btmouse: the layout for a "
					"report-id device says %u bytes, "
					"expected 3 -- a zero here disables "
					"the short-report check entirely\n",
					with_id.layout.report_bytes);
				ok = false;
			}
		}
	}

	return ok;
}
