/* HID over Bluetooth: one header byte, and the report behind it.
 *
 * See `bt_hid.h` for what the layer is. This file is the bytes, and it is
 * short because HIDP is short -- the protocol's whole framing is one byte, and
 * the difficulty is entirely in what that byte does not tell you.
 *
 * --- What is here ---
 *
 * The transaction header, in both directions, and pulling an input report out
 * of a message. Pure functions of bytes, exercised below.
 *
 * --- What is not, and why each one is somebody's decision rather than mine ---
 *
 * **No report-descriptor parser here** -- it is `core/hid_report.c`, written
 * as its own piece of work exactly as `usb_hid.c` said it would have to be,
 * and it names no transport because a descriptor is the same bytes on either
 * one. It answers `uses_report_id` below, which is the argument this file
 * refuses to guess at. What it does not answer yet is which bits are the
 * buttons and which byte is X; that is its second pass.
 *
 * **No SDP.** Service Discovery is how a device's PSMs and its report
 * descriptor are found in the first place, and it is another protocol on
 * another L2CAP channel. A separate layer, not a corner of this one.
 *
 * **No report decoding.** This is the interesting omission, because the code
 * already exists and cannot be reached.
 *
 * `usb_hid.c` decodes boot-protocol reports in `decode_mouse` and
 * `decode_keyboard`. Neither has anything to do with USB: they take a report
 * and the previous report, compare them, and post input events. The layouts
 * they decode are the **HID boot protocol's**, which is exactly what a
 * Bluetooth device in boot mode sends -- the same eight bytes for a keyboard
 * and the same three or four for a mouse.
 *
 * Both are `static`. So a second transport carrying identical reports cannot
 * call either, and the choice is to duplicate a tested decoder or to change a
 * file this session does not own. Neither is done here: it is reported instead,
 * because it is the same shape as the endpoint fault -- **one implementation
 * that cannot tell which of its parts were about USB and which were about
 * HID** -- and the fix belongs with whoever owns `usb_hid.c`.
 *
 * Duplicating it would be the worse answer twice over: two decoders drift, and
 * the one copied here would arrive with none of the 11,506 checks the original
 * carries.
 */
#include <recon/kernel/bt_hid.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>

u8 bt_hid_header(u8 transaction, u8 parameter)
{
	/* Transaction on top. Masked rather than trusted: a caller passing a
	 * whole byte where a nibble belongs would otherwise shift its high
	 * bits out of the byte entirely and produce a header that is wrong
	 * without being obviously wrong. */
	return (u8)(((transaction & 0x0Fu) << 4) | (parameter & 0x0Fu));
}

bool bt_hid_parse(const u8 *pdu, u32 len, struct bt_hid_message *out)
{
	if (!len)
		return false;

	out->transaction = (u8)(pdu[0] >> 4);
	out->parameter   = (u8)(pdu[0] & 0x0Fu);

	/* A message may be header and nothing else -- a handshake is exactly
	 * that -- so an empty payload is a result rather than a failure. The
	 * pointer is still set, because a caller that checks `length` and then
	 * reads `payload` should not find a stale one from the last message. */
	out->payload = pdu + 1;
	out->length  = len - 1;

	return true;
}

bool bt_hid_input_report(const struct bt_hid_message *m, bool uses_report_id,
			 struct bt_hid_report *out)
{
	if (m->transaction != HIDP_TRANS_DATA ||
	    m->parameter != HIDP_REPORT_INPUT)
		return false;

	if (!m->length)
		return false;

	if (uses_report_id) {
		/* The id costs a byte, so a message carrying only the id and
		 * no report is not a report. */
		if (m->length < 2)
			return false;

		out->have_id = true;
		out->id      = m->payload[0];
		out->data    = m->payload + 1;
		out->length  = m->length - 1;
	} else {
		out->have_id = false;
		out->id      = 0;
		out->data    = m->payload;
		out->length  = m->length;
	}

	return true;
}

bool bt_hid_handshake_ok(u8 parameter)
{
	return parameter == HIDP_HSHK_SUCCESSFUL;
}

bool bt_hid_handshake_retryable(u8 parameter)
{
	/* Only one value means "not now". Everything else that is not success
	 * is a refusal, and treating a refusal as retryable is a driver that
	 * asks a device the same impossible question for ever. */
	return parameter == HIDP_HSHK_NOT_READY;
}

/* --- the self-test --------------------------------------------------------
 *
 * The cases are chosen the same way as the two layers below: each has a wrong
 * answer that still looks like a working driver.
 *
 *   - the nibbles must not swap. 0xA1 reversed is 0x1A, which is not garbage
 *     -- it is HID_CONTROL with parameter 10, a different valid message;
 *   - the well-known header is checked against the **literal** 0xA1, not
 *     against the macro that builds it, because a macro that shifts wrongly
 *     shifts wrongly on both sides of a comparison and agrees with itself;
 *   - a report read with the report-id assumption inverted must come back
 *     **different**, because that is the failure this layer refuses to guess
 *     at, and a test where both readings agree would prove the guess safe;
 *   - a handshake of NOT_READY is neither success nor refusal;
 *   - a message with nothing after its header is not a report.
 */
bool bt_hid_self_test(void)
{
	struct bt_hid_message m;
	struct bt_hid_report rep;
	u8 pdu[8];
	bool ok = true;

	/* --- the header, against the number it has to be ----------------- */
	if (bt_hid_header(HIDP_TRANS_DATA, HIDP_REPORT_INPUT) != 0xA1) {
		kprintf("  bthid: a DATA/INPUT header came out as %02x, not "
			"a1 -- the transaction is the top nibble\n",
			bt_hid_header(HIDP_TRANS_DATA, HIDP_REPORT_INPUT));
		ok = false;
	}

	if (HIDP_INPUT_DATA_HEADER != 0xA1) {
		kprintf("  bthid: the header constant is %02x, not a1\n",
			(unsigned)HIDP_INPUT_DATA_HEADER);
		ok = false;
	}

	/* Swapped, which is the mistake this is here to catch. 0x1A must not
	 * read back as a DATA/INPUT message. */
	{
		u8 swapped = bt_hid_header(HIDP_REPORT_INPUT, HIDP_TRANS_DATA);

		if (swapped == 0xA1) {
			kputs("  bthid: the nibbles are interchangeable, so "
			      "nothing here can tell a data report from a "
			      "control message\n");
			ok = false;
		}

		if (!bt_hid_parse(&swapped, 1, &m)) {
			kputs("  bthid: a one-byte message was refused\n");
			ok = false;
		} else if (m.transaction == HIDP_TRANS_DATA) {
			kprintf("  bthid: %02x read as a DATA transaction; it "
				"is transaction %x parameter %x\n", swapped,
				m.transaction, m.parameter);
			ok = false;
		}
	}

	/* --- a mouse report, the ordinary way ---------------------------- */
	pdu[0] = HIDP_INPUT_DATA_HEADER;
	pdu[1] = 0x01;			/* buttons: left down */
	/* x, and it is even on purpose. The buttons byte is odd, so shifting
	 * by one flips the left button's bit and the two readings disagree.
	 * The first version of this used 0x05 -- also odd -- which made the
	 * readings agree about the button and hid the shift in the very field
	 * the test uses to show it. The check at the end of that block caught
	 * it on the first boot, which is the only reason it is 0x04 now. */
	pdu[2] = 0x04;
	pdu[3] = 0xFB;			/* y, -5 */
	pdu[4] = 0x00;			/* wheel */

	if (!bt_hid_parse(pdu, 5, &m)) {
		kputs("  bthid: a five-byte report was refused\n");
		ok = false;
	} else if (m.transaction != HIDP_TRANS_DATA ||
		   m.parameter != HIDP_REPORT_INPUT || m.length != 4) {
		kprintf("  bthid: a mouse report parsed as transaction %x "
			"parameter %x with %u bytes, expected a %x/%x with "
			"4\n", m.transaction, m.parameter, m.length,
			HIDP_TRANS_DATA, HIDP_REPORT_INPUT);
		ok = false;
	}

	if (!bt_hid_input_report(&m, false, &rep)) {
		kputs("  bthid: a DATA/INPUT message yielded no report\n");
		ok = false;
	} else if (rep.have_id || rep.length != 4 || rep.data[0] != 0x01 ||
		   rep.data[1] != 0x04) {
		kprintf("  bthid: the report came back %u bytes starting "
			"%02x %02x, expected 4 starting 01 04\n", rep.length,
			rep.data[0], rep.data[1]);
		ok = false;
	}

	/* --- and the same bytes read the other way ----------------------
	 *
	 * This is the one that matters. If both readings agreed, the
	 * assumption this layer refuses to make would be safe to make, and
	 * the argument could go away.
	 */
	{
		struct bt_hid_report with_id;

		if (!bt_hid_input_report(&m, true, &with_id)) {
			kputs("  bthid: the same message yielded no report "
			      "when read as carrying a report id\n");
			ok = false;
		} else {
			if (!with_id.have_id || with_id.id != 0x01) {
				kprintf("  bthid: read with an id, the id is "
					"%02x, expected 01 -- the first byte "
					"after the header\n", with_id.id);
				ok = false;
			}

			if (with_id.length != 3 || with_id.data[0] != 0x04) {
				kprintf("  bthid: read with an id, the report "
					"is %u bytes starting %02x, expected "
					"3 starting 04\n", with_id.length,
					with_id.data[0]);
				ok = false;
			}

			/* The two readings must disagree, or this whole
			 * argument is theoretical. */
			if (with_id.length == rep.length &&
			    with_id.data == rep.data) {
				kputs("  bthid: reading a report with and "
				      "without a report id gave the same "
				      "answer, so guessing would be safe and "
				      "this layer need not ask\n");
				ok = false;
			}

			/* Named concretely: what a mouse would do. Buttons are
			 * byte 0 of a boot report, so the two readings put a
			 * different byte there -- 01 against 04, a left button
			 * that is down against one that is not. */
			if ((rep.data[0] & 1u) == (with_id.data[0] & 1u)) {
				kputs("  bthid: both readings agree about the "
				      "left button, so this test cannot show "
				      "the shift it is about\n");
				ok = false;
			}
		}
	}

	/* --- a message with nothing after its header --------------------- */
	{
		u8 bare = HIDP_INPUT_DATA_HEADER;

		if (!bt_hid_parse(&bare, 1, &m) || m.length != 0) {
			kputs("  bthid: a header-only message did not parse "
			      "as one with an empty payload\n");
			ok = false;
		}

		if (bt_hid_input_report(&m, false, &rep)) {
			kputs("  bthid: a message with no report after its "
			      "header produced one\n");
			ok = false;
		}

		/* And with an id assumed, one byte is the id and still no
		 * report. */
		pdu[0] = HIDP_INPUT_DATA_HEADER;
		pdu[1] = 0x02;

		if (!bt_hid_parse(pdu, 2, &m)) {
			kputs("  bthid: a two-byte message was refused\n");
			ok = false;
		} else if (bt_hid_input_report(&m, true, &rep)) {
			kputs("  bthid: a message carrying only a report id "
			      "produced a report with no bytes in it\n");
			ok = false;
		}
	}

	/* --- nothing at all ---------------------------------------------- */
	if (bt_hid_parse(pdu, 0, &m)) {
		kputs("  bthid: an empty payload parsed as a message, so its "
		      "transaction came from a byte that is not there\n");
		ok = false;
	}

	/* --- a message that is not input data ---------------------------- */
	{
		u8 output[3];

		output[0] = bt_hid_header(HIDP_TRANS_DATA, HIDP_REPORT_OUTPUT);
		output[1] = 0xFF;
		output[2] = 0xFF;

		if (!bt_hid_parse(output, 3, &m)) {
			kputs("  bthid: an output report was refused\n");
			ok = false;
		} else if (bt_hid_input_report(&m, false, &rep)) {
			kputs("  bthid: an OUTPUT report was read as an input "
			      "one -- they differ only in the bottom nibble\n");
			ok = false;
		}
	}

	/* --- handshakes --------------------------------------------------- */
	if (!bt_hid_handshake_ok(HIDP_HSHK_SUCCESSFUL)) {
		kputs("  bthid: a successful handshake did not read as "
		      "success\n");
		ok = false;
	}

	if (bt_hid_handshake_ok(HIDP_HSHK_NOT_READY) ||
	    bt_hid_handshake_ok(HIDP_HSHK_ERR_FATAL)) {
		kputs("  bthid: a handshake that is not success read as "
		      "success\n");
		ok = false;
	}

	if (!bt_hid_handshake_retryable(HIDP_HSHK_NOT_READY)) {
		kputs("  bthid: NOT_READY did not read as retryable -- it "
		      "means ask again, and a driver reading it as a refusal "
		      "gives up on a device that was about to work\n");
		ok = false;
	}

	if (bt_hid_handshake_retryable(HIDP_HSHK_ERR_UNSUPPORTED_REQUEST) ||
	    bt_hid_handshake_retryable(HIDP_HSHK_ERR_FATAL) ||
	    bt_hid_handshake_retryable(HIDP_HSHK_SUCCESSFUL)) {
		kputs("  bthid: a handshake that is not NOT_READY read as "
		      "retryable, so a refusal would be asked again for "
		      "ever\n");
		ok = false;
	}

	/* --- boot and report protocol are different requests -------------- */
	if (bt_hid_header(HIDP_TRANS_SET_PROTOCOL, HIDP_PROTO_BOOT) ==
	    bt_hid_header(HIDP_TRANS_SET_PROTOCOL, HIDP_PROTO_REPORT)) {
		kputs("  bthid: asking for boot protocol and asking for "
		      "report protocol build the same byte\n");
		ok = false;
	}

	if (bt_hid_header(HIDP_TRANS_SET_PROTOCOL, HIDP_PROTO_BOOT) != 0x70) {
		kprintf("  bthid: SET_PROTOCOL(boot) came out as %02x, not "
			"70\n", bt_hid_header(HIDP_TRANS_SET_PROTOCOL,
					      HIDP_PROTO_BOOT));
		ok = false;
	}

	return ok;
}
