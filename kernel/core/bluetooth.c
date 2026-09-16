/* Bluetooth over USB: the HCI transport, and what it currently cannot do.
 *
 * Bluetooth reaches this kernel as a USB device reporting class 224.1.1, and
 * everything above the radio is HCI: commands down, events up, ACL data both
 * ways. USB carries the four on four different endpoints, and that count is
 * the whole problem this file currently reports rather than solves.
 *
 * --- Why this driver refuses every device it recognises ---
 *
 * An HCI interface has three endpoints and needs all three at once:
 *
 *     control       commands down, as class requests on the interface
 *     interrupt in  events up
 *     bulk in       ACL data up
 *     bulk out      ACL data down
 *
 * `struct usb_device` has room for **one** IN endpoint -- one `in_ep`, one
 * `in_packet`, one `in` ring, one `in_is_interrupt` flag. Two of the three are
 * IN. There is nowhere to put the second.
 *
 * It is worse than a missing slot, because the enumeration does not notice.
 * `read_interface()` in `xhci.c` guards its interrupt-IN branch with
 * `!ud->in_ep` and leaves its bulk-IN branch unguarded, so:
 *
 *   - interrupt endpoint first in the descriptor: it is taken, then the bulk
 *     one overwrites it;
 *   - bulk endpoint first: it is taken, and the guard then rejects the
 *     interrupt one.
 *
 * **Either order ends with the bulk endpoint and no event endpoint**, so this
 * does not depend on how a particular adapter orders its descriptors. The
 * device still configures, `ud->configured` is still set, and the enumeration
 * prints a perfectly ordinary bulk device. A driver that pressed on would send
 * HCI_Reset as a control transfer, get a success back from the controller, and
 * then wait for a Command Complete on an endpoint nobody is listening to.
 *
 * That guard is not a bug in the sense of being careless. Its comment says
 * exactly what it is for -- a card reader that offers both kinds of IN
 * endpoint should still be read as the storage device it is -- and for that
 * device it chooses correctly. It is one rule serving two classes that want
 * opposite answers, which is a shape rather than a slip.
 *
 * So `bt_hci_attach()` below **diagnoses and declines**. It prints which
 * endpoint the enumeration actually handed it, and says what that means. That
 * makes the paragraph above a thing the next boot either confirms or refutes,
 * on two different adapters, instead of a thing this comment asserts.
 *
 * --- What is deliberately not here ---
 *
 * **No command is sent.** The framing to send one is below and tested, but the
 * path that would put it on the wire is not written, because there is no
 * endpoint to read the answer on and untestable code in a tree is code that
 * quietly stops being right. It goes in when the interface can hold a second
 * IN endpoint.
 *
 * **No firmware loading.** Both adapters need a blob before they answer
 * anything, and the kernel has no way to get a file from the volume into a
 * driver at init. That is a missing mechanism, not a missing driver.
 *
 * **No SCO.** The isochronous interface a controller offers for voice is
 * interface 1, and nothing on the path to a mouse or a file transfer touches
 * it.
 *
 * --- What is here, and is finished ---
 *
 * The part of HCI with somewhere to go wrong that does not involve hardware:
 * framing a command, reassembling an event across transport packets, and
 * finding a command's answer inside an event. Those are pure functions of
 * their input, they are exercised by `bt_hci_self_test()` below, and they are
 * the part that will not change when the transport does.
 */
#include <recon/kernel/bluetooth.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/xhci.h>

/* Four counters and no table.
 *
 * A table of attached controllers was written here first and then removed: it
 * held a slot, a ring and an endpoint for every device claimed, and this driver
 * claims none. Structure for code that does not exist yet is structure nobody
 * can check, and the compiler is happy either way -- the fields were assigned,
 * so nothing warned. It goes back in when there is a controller to keep.
 *
 * What is worth counting is what happened to the devices that were offered.
 * `seen` and the two refusals are different facts, and a zero in one of them
 * means something different from a zero in another. */
static unsigned seen;
static unsigned declined_no_event_endpoint;
static unsigned declined_unconfigured;
static unsigned unexpected_event_endpoint;

/* --- framing --------------------------------------------------------------
 *
 * Built byte by byte rather than by casting a struct over a buffer. The layout
 * is packed in a way C will not promise, and a compiler that inserts padding
 * here produces a command the controller rejects with no explanation -- which
 * is the same reasoning `usb_storage.c` gives for its wrappers, and it is the
 * same hazard.
 *
 * Little-endian on the wire regardless of the host, which is why the opcode is
 * split by hand instead of copied.
 */
u32 hci_command_build(u8 *out, u16 opcode, const u8 *params, u8 plen)
{
	out[0] = (u8)(opcode & 0xFFu);
	out[1] = (u8)(opcode >> 8);
	out[2] = plen;

	if (plen && params)
		kmemcpy(out + HCI_COMMAND_HEADER, params, plen);

	return (u32)HCI_COMMAND_HEADER + plen;
}

/* --- reassembly ----------------------------------------------------------- */

void hci_reassembly_reset(struct hci_event_reassembly *r)
{
	r->have = 0;
	r->want = 0;
}

bool hci_event_feed(struct hci_event_reassembly *r, const u8 *pkt, u32 len)
{
	/* A completed event is left in the buffer for the caller to read and
	 * cleared here, on the next feed, rather than by the caller.
	 *
	 * Deliberately not the caller's job: a caller that forgot would append
	 * the next event onto the last one and then see neither of them, since
	 * the length check below would never line up again. Making it
	 * impossible to forget costs one comparison. */
	if (r->want && r->have >= r->want)
		hci_reassembly_reset(r);

	if (!len)
		return false;

	/* More than the buffer holds. Cannot happen from a controller that
	 * follows the specification -- 255 is the largest parameter length the
	 * one-byte field can express -- so this is about what a broken or
	 * unplugged device sends, and it must not be a memory write. */
	if (r->have + len > sizeof(r->buf)) {
		r->overruns++;
		hci_reassembly_reset(r);
		return false;
	}

	kmemcpy(r->buf + r->have, pkt, len);
	r->have += len;

	/* The declared length lives in the second byte, so until two bytes have
	 * arrived there is nothing to compare against. Reading `buf[1]` before
	 * this check is a read of whatever the last event left there. */
	if (r->have < HCI_EVENT_HEADER)
		return false;

	r->want = (u32)HCI_EVENT_HEADER + r->buf[1];

	if (r->have < r->want)
		return false;

	/* Past the end of what the event said it was. Each event gets its own
	 * transfer, so this is not two events sharing one packet -- it is a
	 * packet that disagrees with its own header, and guessing where to
	 * split it would invent a boundary. */
	if (r->have > r->want) {
		r->overruns++;
		hci_reassembly_reset(r);
		return false;
	}

	r->completed++;
	return true;
}

/* --- reading an answer out of an event ------------------------------------ */

bool hci_event_command_result(const u8 *ev, u32 len, u16 *opcode, u8 *status)
{
	if (len < HCI_EVENT_HEADER)
		return false;

	if (ev[0] == HCI_EV_COMMAND_COMPLETE) {
		/* code, plen, credits, opcode low, opcode high, status */
		if (len < 6)
			return false;

		*opcode = (u16)(ev[3] | ((u16)ev[4] << 8));
		*status = ev[5];
		return true;
	}

	if (ev[0] == HCI_EV_COMMAND_STATUS) {
		/* code, plen, status, credits, opcode low, opcode high.
		 *
		 * The status comes *before* the opcode here and *after* it
		 * above. Reading one with the other's offsets yields an opcode
		 * built from a status byte and a credit count, which is a
		 * number rather than an error. */
		if (len < 6)
			return false;

		*status = ev[2];
		*opcode = (u16)(ev[4] | ((u16)ev[5] << 8));
		return true;
	}

	return false;
}

bool hci_acl_parse(const u8 *hdr, u32 len, struct hci_acl_header *out)
{
	if (len < HCI_ACL_HEADER)
		return false;

	/* Twelve bits, not sixteen. The two flags sit in the top nibble of the
	 * second byte, so an unmasked read returns a handle with the flags
	 * folded into it -- a wrong connection, not an obviously wrong one. */
	out->handle = (u16)((hdr[0] | ((u16)hdr[1] << 8)) & 0x0FFFu);
	out->pb     = (u8)((hdr[1] >> 4) & 0x03u);
	out->bc     = (u8)((hdr[1] >> 6) & 0x03u);
	out->length = (u16)(hdr[2] | ((u16)hdr[3] << 8));

	return true;
}

/* --- attaching ------------------------------------------------------------ */

bool bt_hci_attach(struct xhci *x, struct usb_device *ud)
{
	/* Taken and not used. This driver reads the device the enumeration has
	 * already filled in and never issues a transfer, so it needs no
	 * controller -- the parameter stays because the attach chain calls
	 * every class driver the same way, and a different signature here would
	 * be a special case to remember. */
	(void)x;

	if (ud->usb_class != BT_USB_CLASS ||
	    ud->usb_subclass != BT_USB_SUBCLASS ||
	    ud->usb_protocol != BT_USB_PROTOCOL)
		return false;

	seen++;

	kprintf("  bluetooth    : %04x:%04x, a Bluetooth controller on "
		"interface %u\n", ud->vendor, ud->product, ud->interface);

	if (!ud->configured) {
		declined_unconfigured++;
		kputs("  bluetooth    : not configured, so the only endpoint "
		      "is the control one -- commands could go out and no "
		      "event could come back\n");
		return false;
	}

	/* The measurement this driver exists to take today. Printed before any
	 * judgement about it, so the number is in the log even if the reading
	 * of it below turns out to be wrong. */
	kprintf("  bluetooth    : the enumeration gave it in %02x (%s, %u "
		"byte), out %02x (%u byte)\n", ud->in_ep,
		ud->in_is_interrupt ? "interrupt" : "bulk", ud->in_packet,
		ud->out_ep, ud->out_packet);

	if (!ud->in_is_interrupt) {
		declined_no_event_endpoint++;

		/* Said in full rather than as a code, because this line is the
		 * whole diagnosis and the next person to read it will be
		 * reading a boot log, not this file. */
		kputs("  bluetooth    : that is the ACL endpoint, not the "
		      "event one. An HCI interface offers both, and "
		      "read_interface() keeps only one IN endpoint -- its bulk "
		      "branch is unguarded and overwrites whatever the "
		      "interrupt branch took, whichever order they appear in\n");
		kputs("  bluetooth    : declining. Sending a command from here "
		      "would succeed and its answer would arrive on an "
		      "endpoint nothing is reading\n");
		return false;
	}

	/* The other branch, and it would be a surprise.
	 *
	 * The reading of `read_interface()` above says this cannot happen for a
	 * Bluetooth controller: the bulk branch is unguarded, so it wins
	 * whichever order the endpoints appear in. If this line ever prints,
	 * that reading is wrong -- and a boot log saying so is worth more than
	 * the comment at the top of this file being right, so it says so loudly
	 * rather than quietly succeeding. */
	unexpected_event_endpoint++;

	kputs("  bluetooth    : an interrupt IN endpoint survived "
	      "enumeration, which contradicts this driver's reading of "
	      "read_interface(). Worth recording: the reading is wrong, or "
	      "this adapter is shaped differently from the two measured\n");
	kputs("  bluetooth    : still declining to talk -- ACL data needs the "
	      "bulk IN endpoint that was dropped to keep this one\n");

	return false;
}

unsigned bt_hci_count(void)
{
	return seen;
}

void bt_hci_print_summary(void)
{
	/* Nothing at all when no Bluetooth device was seen. A machine with no
	 * adapter should not grow a line saying so. */
	if (!seen)
		return;

	kprintf("  bluetooth    : %u controller(s) seen\n", seen);

	if (declined_no_event_endpoint)
		kprintf("  bluetooth    : %u declined for having no event "
			"endpoint after enumeration\n",
			declined_no_event_endpoint);

	if (declined_unconfigured)
		kprintf("  bluetooth    : %u declined for not configuring\n",
			declined_unconfigured);

	/* Printed with emphasis because it is the reading above being wrong,
	 * which is the one outcome here nobody has planned for. */
	if (unexpected_event_endpoint)
		kprintf("  bluetooth    : **%u kept an interrupt endpoint** -- "
			"this driver's reading of read_interface() does not "
			"hold for that adapter\n", unexpected_event_endpoint);
}

/* --- the self-test --------------------------------------------------------
 *
 * No hardware, and none needed: everything checked here is a pure function of
 * bytes. What is deliberately not covered is anything involving the
 * controller, for the reason `usb_hid.c` gives -- a test needing an adapter
 * attached reports a failure on every machine that has none, which is most of
 * them and all of the rig.
 *
 * Each check below is chosen for having a wrong answer that still looks like a
 * working driver:
 *
 *   - an opcode must split into the right two fields. 0x0C03 is HCI_Reset and
 *     is checked against the literal, because a macro that builds the wrong
 *     number builds it consistently and every test that used the macro on both
 *     sides would agree with itself;
 *   - an event arriving in pieces must produce **one** event, not three;
 *   - an event whose total length is an exact multiple of the packet size must
 *     still complete. This is the check that a short-packet-terminated reader
 *     fails, and it fails only on that one length;
 *   - two events in a row must not concatenate;
 *   - Command Complete and Command Status must not be read with each other's
 *     offsets, which yields a plausible wrong opcode rather than an error;
 *   - an ACL handle must be masked to twelve bits, because the unmasked read
 *     is a valid-looking handle for a different connection.
 */
static bool feed_all(struct hci_event_reassembly *r, const u8 *ev, u32 total,
		     u32 chunk, unsigned *completions)
{
	u32 at = 0;

	*completions = 0;

	while (at < total) {
		u32 n = (total - at < chunk) ? total - at : chunk;

		if (hci_event_feed(r, ev + at, n))
			(*completions)++;

		at += n;
	}

	return true;
}

bool bt_hci_self_test(void)
{
	struct hci_event_reassembly r;
	struct hci_acl_header acl;
	u8 out[HCI_COMMAND_MAX];
	u8 ev[HCI_EVENT_MAX];
	u16 opcode;
	u8 status;
	unsigned completions;
	u32 n;
	bool ok = true;

	/* --- an opcode is two fields ------------------------------------ */
	if (HCI_OP_RESET != 0x0C03) {
		kprintf("  bluetooth: HCI_Reset built as %04x, not 0c03 -- the "
			"opcode split is wrong\n", (unsigned)HCI_OP_RESET);
		ok = false;
	}

	if (HCI_OP_READ_BD_ADDR != 0x1009) {
		kprintf("  bluetooth: Read_BD_ADDR built as %04x, not 1009\n",
			(unsigned)HCI_OP_READ_BD_ADDR);
		ok = false;
	}

	/* --- a command with no parameters ------------------------------- */
	kmemset(out, 0xAA, sizeof(out));
	n = hci_command_build(out, HCI_OP_RESET, 0, 0);

	if (n != 3 || out[0] != 0x03 || out[1] != 0x0C || out[2] != 0) {
		kprintf("  bluetooth: HCI_Reset framed as %u bytes %02x %02x "
			"%02x, expected 3 bytes 03 0c 00 -- the opcode is "
			"little-endian on the wire\n", n, out[0], out[1],
			out[2]);
		ok = false;
	}

	/* --- and one with them ------------------------------------------ */
	{
		static const u8 params[3] = { 0x11, 0x22, 0x33 };

		n = hci_command_build(out, HCI_OP_RESET, params, 3);

		if (n != 6 || out[2] != 3 || out[3] != 0x11 || out[5] != 0x33) {
			kputs("  bluetooth: a command's parameters did not "
			      "land after its header\n");
			ok = false;
		}
	}

	/* --- one packet, one event -------------------------------------- */
	hci_reassembly_reset(&r);
	r.completed = r.overruns = 0;

	ev[0] = HCI_EV_COMMAND_COMPLETE;
	ev[1] = 4;
	ev[2] = 1;			/* credits */
	ev[3] = 0x03;			/* opcode low */
	ev[4] = 0x0C;			/* opcode high */
	ev[5] = 0x00;			/* status */

	if (!hci_event_feed(&r, ev, 6)) {
		kputs("  bluetooth: a complete event in one packet was not "
		      "reported complete\n");
		ok = false;
	}

	/* --- the same event in three pieces ----------------------------- */
	hci_reassembly_reset(&r);
	feed_all(&r, ev, 6, 2, &completions);

	if (completions != 1) {
		kprintf("  bluetooth: an event split across packets completed "
			"%u times, not once -- a reader that reports every "
			"packet would answer a command three times\n",
			completions);
		ok = false;
	} else if (r.want != 6 || r.buf[3] != 0x03 || r.buf[4] != 0x0C) {
		kputs("  bluetooth: an event reassembled from pieces did not "
		      "come back with its bytes in order\n");
		ok = false;
	}

	/* --- a header split down the middle -----------------------------
	 *
	 * **This check was rewritten after it failed to fail.** The first
	 * version fed one byte and asserted only that no event was reported,
	 * and it passed with the `have < HCI_EVENT_HEADER` guard deleted --
	 * because the length comparison below the guard returns false for a
	 * one-byte buffer anyway. So the guard was covered by nothing, and a
	 * later reader deleting it as redundant would have seen green.
	 *
	 * What the guard actually prevents is `r->want` being computed from
	 * `buf[1]` before `buf[1]` is part of this event, so that is what is
	 * asserted: a stale byte is planted where the length will go, and
	 * `want` must still be zero after one byte has arrived.
	 */
	{
		hci_reassembly_reset(&r);

		/* What a previous event left behind. Any non-zero value does;
		 * 200 is chosen because it is large enough that a `want`
		 * computed from it could not be mistaken for a real one. */
		r.buf[1] = 200;

		if (hci_event_feed(&r, ev, 1)) {
			kputs("  bluetooth: one byte was reported as a "
			      "complete event -- the length had not arrived "
			      "yet\n");
			ok = false;
		}

		if (r.want != 0) {
			kprintf("  bluetooth: after one byte the expected "
				"length is %u, from a byte belonging to the "
				"last event rather than this one\n", r.want);
			ok = false;
		}

		if (!hci_event_feed(&r, ev + 1, 5)) {
			kputs("  bluetooth: an event whose header was split "
			      "across two packets never completed\n");
			ok = false;
		}
	}

	/* --- exactly one packet's worth, which is the interesting one ---
	 *
	 * Sixteen bytes total: a fourteen-byte parameter block. There is no
	 * short packet to end it, so a reader keyed on short packets waits for
	 * one that never arrives. Keyed on the declared length, it completes.
	 */
	{
		unsigned i;

		kmemset(ev, 0, sizeof(ev));
		ev[0] = HCI_EV_COMMAND_COMPLETE;
		ev[1] = 14;

		for (i = 0; i < 14; i++)
			ev[2 + i] = (u8)(0x40 + i);

		hci_reassembly_reset(&r);
		feed_all(&r, ev, 16, 16, &completions);

		if (completions != 1) {
			kputs("  bluetooth: an event exactly one packet long "
			      "did not complete -- nothing shorter than the "
			      "packet size ever arrives to end it, so this is "
			      "the length a short-packet reader hangs on\n");
			ok = false;
		}
	}

	/* --- the longest event there is, in sixteen-byte packets --------- */
	{
		kmemset(ev, 0x5A, sizeof(ev));
		ev[0] = 0x3E;
		ev[1] = 255;

		hci_reassembly_reset(&r);
		feed_all(&r, ev, HCI_EVENT_MAX, 16, &completions);

		if (completions != 1 || r.want != HCI_EVENT_MAX) {
			kprintf("  bluetooth: a 257-byte event across 17 "
				"packets completed %u time(s) at %u bytes\n",
				completions, r.want);
			ok = false;
		}
	}

	/* --- two in a row must not run together -------------------------- */
	{
		u8 first[6], second[6];

		first[0] = HCI_EV_COMMAND_COMPLETE;
		first[1] = 4;
		first[2] = 1;
		first[3] = 0x03;
		first[4] = 0x0C;
		first[5] = 0x00;

		second[0] = HCI_EV_COMMAND_COMPLETE;
		second[1] = 4;
		second[2] = 1;
		second[3] = 0x09;	/* Read_BD_ADDR */
		second[4] = 0x10;
		second[5] = 0x00;

		hci_reassembly_reset(&r);

		if (!hci_event_feed(&r, first, 6)) {
			kputs("  bluetooth: the first of two events did not "
			      "complete\n");
			ok = false;
		}

		if (!hci_event_feed(&r, second, 6)) {
			kputs("  bluetooth: the second of two events did not "
			      "complete -- the first was still in the buffer, "
			      "so neither would ever line up again\n");
			ok = false;
		} else if (r.buf[3] != 0x09 || r.buf[4] != 0x10) {
			kputs("  bluetooth: the second event came back with "
			      "the first one's opcode\n");
			ok = false;
		}
	}

	/* --- a packet that disagrees with its own header ------------------ */
	{
		u8 lying[8];

		kmemset(lying, 0, sizeof(lying));
		lying[0] = HCI_EV_COMMAND_COMPLETE;
		lying[1] = 2;			/* says four bytes total */

		hci_reassembly_reset(&r);
		r.overruns = 0;

		if (hci_event_feed(&r, lying, 8) || r.overruns != 1) {
			kputs("  bluetooth: a packet carrying more than its "
			      "header declared was accepted -- the extra bytes "
			      "have no boundary anybody can infer\n");
			ok = false;
		}
	}

	/* --- Command Complete and Command Status ------------------------- */
	{
		u8 complete[6], stat[6];

		complete[0] = HCI_EV_COMMAND_COMPLETE;
		complete[1] = 4;
		complete[2] = 1;		/* credits */
		complete[3] = 0x03;		/* opcode */
		complete[4] = 0x0C;
		complete[5] = 0x00;		/* status */

		opcode = 0;
		status = 0xFF;

		if (!hci_event_command_result(complete, 6, &opcode, &status) ||
		    opcode != HCI_OP_RESET || status != 0) {
			kprintf("  bluetooth: Command Complete read as opcode "
				"%04x status %u, expected 0c03 and 0\n",
				opcode, status);
			ok = false;
		}

		stat[0] = HCI_EV_COMMAND_STATUS;
		stat[1] = 4;
		stat[2] = 0x00;			/* status comes FIRST here */
		stat[3] = 1;			/* credits */
		stat[4] = 0x03;			/* opcode */
		stat[5] = 0x0C;

		opcode = 0;
		status = 0xFF;

		if (!hci_event_command_result(stat, 6, &opcode, &status) ||
		    opcode != HCI_OP_RESET || status != 0) {
			kprintf("  bluetooth: Command Status read as opcode "
				"%04x status %u, expected 0c03 and 0 -- its "
				"status and opcode are the other way round "
				"from Command Complete\n", opcode, status);
			ok = false;
		}

		/* And the two layouts must actually differ, or the check above
		 * proves nothing. Reading the Command Status bytes with the
		 * Command Complete offsets gives opcode 0x0301 from the credit
		 * and opcode-low bytes -- a real-looking opcode. */
		if ((u16)(stat[3] | ((u16)stat[4] << 8)) == HCI_OP_RESET) {
			kputs("  bluetooth: the two event layouts agree in "
			      "this test, so it cannot tell them apart\n");
			ok = false;
		}

		/* Anything else answers no command. */
		if (hci_event_command_result(ev, 6, &opcode, &status)) {
			kputs("  bluetooth: an event that answers no command "
			      "was read as answering one\n");
			ok = false;
		}

		/* Truncated is not a result either. */
		if (hci_event_command_result(complete, 4, &opcode, &status)) {
			kputs("  bluetooth: a truncated Command Complete gave "
			      "an opcode from bytes that had not arrived\n");
			ok = false;
		}
	}

	/* --- an ACL handle is twelve bits -------------------------------- */
	{
		/* Handle 0x0EFF, packet boundary 2, broadcast 0, 6 bytes. */
		static const u8 hdr[4] = { 0xFF, 0x2E, 0x06, 0x00 };

		if (!hci_acl_parse(hdr, 4, &acl)) {
			kputs("  bluetooth: a four-byte ACL header was "
			      "refused\n");
			ok = false;
		} else {
			if (acl.handle != 0x0EFF) {
				kprintf("  bluetooth: ACL handle read as %04x, "
					"expected 0eff -- the flags are in the "
					"top nibble and must be masked off\n",
					acl.handle);
				ok = false;
			}

			if (acl.pb != 2 || acl.bc != 0) {
				kprintf("  bluetooth: ACL flags read as pb %u "
					"bc %u, expected 2 and 0\n",
					acl.pb, acl.bc);
				ok = false;
			}

			if (acl.length != 6) {
				kprintf("  bluetooth: ACL length read as %u, "
					"expected 6\n", acl.length);
				ok = false;
			}
		}

		/* Three bytes is not a header. */
		if (hci_acl_parse(hdr, 3, &acl)) {
			kputs("  bluetooth: a three-byte ACL header was "
			      "accepted, so its length came from a byte that "
			      "had not arrived\n");
			ok = false;
		}
	}

	return ok;
}
