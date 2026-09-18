/* Answering a controller's pairing questions.
 *
 * See `bt_pair.h` for what pairing a mouse can and cannot promise, and for
 * where the numbers were checked. This file is the answers.
 *
 * --- One rule runs through all of it ---
 *
 * **Every request is answered, and the answer depends on two things: is the
 * window open, and is this the device it was opened for.** Anything else gets
 * a negative reply.
 *
 * Negative rather than nothing, because a controller that asked and heard
 * nothing waits -- and a host that stays silent turns a refusal into a hang,
 * which is harder to diagnose than either. It is the same reasoning behind
 * `BT_LINK_NEEDS_PAIRING` a layer down.
 */
#include <recon/kernel/bluetooth.h>
#include <recon/kernel/bt_pair.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>

static void put_le16(u8 *p, u16 v)
{
	p[0] = (u8)(v & 0xFFu);
	p[1] = (u8)(v >> 8);
}

static bool same_addr(const u8 *a, const u8 *b)
{
	unsigned i;

	for (i = 0; i < BT_ADDR_LEN; i++)
		if (a[i] != b[i])
			return false;

	return true;
}

/* May this device be answered at all?
 *
 * Both halves matter and they fail differently. A closed window means nobody
 * asked for a pairing; a wrong address means somebody else answered the one
 * that was asked for. Either way the reply is negative. */
static bool may_answer(struct bt_pairing *p, const u8 *addr)
{
	if (!p->window_open || !p->have_target)
		return false;

	return same_addr(addr, p->target);
}

/* A command whose only parameter is an address. Four of the replies below are
 * exactly that shape. */
static u32 addr_command(u8 *out, u16 opcode, const u8 *addr)
{
	return hci_command_build(out, opcode, addr, BT_ADDR_LEN);
}

void bt_pairing_init(struct bt_pairing *p)
{
	kmemset(p, 0, sizeof(*p));

	p->state = BT_PAIR_IDLE;

	/* Honest about what this machine can do *during pairing*. There is no
	 * way to put a six-digit number in front of somebody and take a yes
	 * at the point a mouse is being paired at boot, so claiming a display
	 * would be a claim the association model then relies on. */
	p->io_capability = BT_IO_NO_INPUT_OUTPUT;
	p->auth_requirements = BT_AUTH_NO_MITM_GENERAL;

	/* The legacy default. Four ASCII zeroes, not four zero bytes -- the
	 * PIN is characters, and a device told 0x00000000 rejects it. */
	p->pin[0] = '0';
	p->pin[1] = '0';
	p->pin[2] = '0';
	p->pin[3] = '0';
	p->pin_len = 4;
}

void bt_pairing_allow(struct bt_pairing *p, const u8 *addr)
{
	kmemcpy(p->target, addr, BT_ADDR_LEN);
	p->have_target = true;
	p->window_open = true;
	p->state = BT_PAIR_RUNNING;
}

void bt_pairing_close(struct bt_pairing *p)
{
	p->window_open = false;
}

bool bt_pairing_set_pin(struct bt_pairing *p, const u8 *pin, u8 len)
{
	/* The bound lives here so that callers do not each have to know it.
	 * A PIN longer than the field holds is a configuration mistake, and
	 * refusing says so at the point it is made rather than at the point
	 * a device asks. */
	if (!len || len > BT_PIN_MAX)
		return false;

	kmemset(p->pin, 0, sizeof(p->pin));
	kmemcpy(p->pin, pin, len);
	p->pin_len = len;

	return true;
}

void bt_pairing_remember(struct bt_pairing *p, const u8 *addr, const u8 *key,
			 u8 key_type)
{
	kmemcpy(p->key_addr, addr, BT_ADDR_LEN);
	kmemcpy(p->key, key, BT_LINK_KEY_LEN);
	p->key_type = key_type;
	p->have_key = true;
}

u32 hci_auth_requested_build(u8 *out, u16 handle)
{
	u8 body[2];

	put_le16(body, (u16)(handle & 0x0FFFu));

	return hci_command_build(out, HCI_OP_AUTH_REQUESTED, body,
				 sizeof(body));
}

u32 hci_set_conn_encrypt_build(u8 *out, u16 handle, bool on)
{
	u8 body[3];

	put_le16(body, (u16)(handle & 0x0FFFu));
	body[2] = on ? 1u : 0u;

	return hci_command_build(out, HCI_OP_SET_CONN_ENCRYPT, body,
				 sizeof(body));
}

u32 bt_pairing_event(struct bt_pairing *p, const u8 *ev, u32 len, u8 *out,
		     u32 outmax)
{
	const u8 *addr = ev + 2;

	if (len < HCI_EVENT_HEADER)
		return 0;

	switch (ev[0]) {
	case HCI_EV_LINK_KEY_REQ:
		if (len < 2 + BT_ADDR_LEN || outmax < 3 + 22)
			return 0;

		/* A key for this device means it has paired before and does
		 * not need to again. This path is what makes pairing happen
		 * once rather than every boot.
		 *
		 * Not gated on the window: handing back a key already held
		 * for a device that is asking for it reveals nothing that
		 * device does not have, and refusing would force a needless
		 * re-pair. The gate is on *making* keys, below. */
		if (p->have_key && same_addr(addr, p->key_addr)) {
			u8 body[BT_ADDR_LEN + BT_LINK_KEY_LEN];

			kmemcpy(body, addr, BT_ADDR_LEN);
			kmemcpy(body + BT_ADDR_LEN, p->key, BT_LINK_KEY_LEN);

			return hci_command_build(out, HCI_OP_LINK_KEY_REPLY,
						 body, sizeof(body));
		}

		/* No key. The negative reply is what *starts* pairing, so it
		 * is the right answer whether or not a window is open --
		 * pairing then stops at the next request if it is not. */
		return addr_command(out, HCI_OP_LINK_KEY_NEG_REPLY, addr);

	case HCI_EV_IO_CAP_REQUEST: {
		u8 body[9];

		if (len < 2 + BT_ADDR_LEN || outmax < 3 + 9)
			return 0;

		if (!may_answer(p, addr)) {
			p->refused++;
			return addr_command(out, HCI_OP_IO_CAP_NEG_REPLY,
					    addr);
		}

		kmemcpy(body, addr, BT_ADDR_LEN);
		body[6] = p->io_capability;
		body[7] = BT_OOB_NOT_PRESENT;
		body[8] = p->auth_requirements;

		return hci_command_build(out, HCI_OP_IO_CAP_REPLY, body,
					 sizeof(body));
	}

	case HCI_EV_USER_CONFIRM_REQ:
		if (len < 2 + BT_ADDR_LEN + 4 || outmax < 3 + BT_ADDR_LEN)
			return 0;

		/* The number the peer would have shown, had either end a
		 * screen. Kept rather than used. */
		p->last_passkey = (u32)addr[6] | ((u32)addr[7] << 8) |
				  ((u32)addr[8] << 16) | ((u32)addr[9] << 24);

		if (!may_answer(p, addr)) {
			p->refused++;
			return addr_command(out, HCI_OP_USER_CONFIRM_NEG,
					    addr);
		}

		/* **This is the accept, and it is Just Works.**
		 *
		 * With a mouse at the other end there is nothing to compare
		 * the number against, so confirming it authenticates nothing.
		 * What makes that acceptable is not this line -- it is that
		 * `may_answer` has already established somebody deliberately
		 * asked to pair with this exact device. */
		return addr_command(out, HCI_OP_USER_CONFIRM_REPLY, addr);

	case HCI_EV_PIN_CODE_REQ: {
		u8 body[BT_ADDR_LEN + 1 + BT_PIN_MAX];

		if (len < 2 + BT_ADDR_LEN || outmax < 3 + (u32)sizeof(body))
			return 0;

		if (!may_answer(p, addr)) {
			p->refused++;
			return addr_command(out, HCI_OP_PIN_CODE_NEG_REPLY,
					    addr);
		}

		/* **A length nothing had validated, feeding a fixed copy.**
		 *
		 * `pin_len` is a public field. `bt_pairing_init` sets it to
		 * four, and `bt_pairing_set_pin` now bounds it -- but the
		 * struct is in a header and the field can be written
		 * directly, and this copy would take whatever it said into a
		 * 23-byte buffer on the stack.
		 *
		 * Found by listing every `kmemcpy` with a variable length and
		 * asking what bounds it. Every other one is bounded by a
		 * check a few lines above; this one was bounded by a
		 * convention.
		 *
		 * Refused rather than clamped: a clamped PIN is a *different*
		 * PIN, and pairing then fails for a reason nobody can see. A
		 * negative reply is the same answer this file gives every
		 * other request it will not serve. */
		if (p->pin_len > BT_PIN_MAX) {
			p->refused++;
			return addr_command(out, HCI_OP_PIN_CODE_NEG_REPLY,
					    addr);
		}

		/* The whole sixteen bytes go out regardless of the length;
		 * the length field says how many of them count. Zeroed first
		 * so the unused tail is not whatever was on the stack. */
		kmemset(body, 0, sizeof(body));
		kmemcpy(body, addr, BT_ADDR_LEN);
		body[BT_ADDR_LEN] = p->pin_len;
		kmemcpy(body + BT_ADDR_LEN + 1, p->pin, p->pin_len);

		return hci_command_build(out, HCI_OP_PIN_CODE_REPLY, body,
					 sizeof(body));
	}

	case HCI_EV_LINK_KEY_NOTIFY:
		/* bdaddr, key, key_type -- 23 bytes of parameters. */
		if (len < 2 + BT_ADDR_LEN + BT_LINK_KEY_LEN + 1)
			return 0;

		bt_pairing_remember(p, addr, addr + BT_ADDR_LEN,
				    addr[BT_ADDR_LEN + BT_LINK_KEY_LEN]);
		return 0;

	case HCI_EV_SIMPLE_PAIR_COMPLETE:
		/* status first, then the address. */
		if (len < 2 + 1 + BT_ADDR_LEN)
			return 0;

		if (ev[2]) {
			p->state = BT_PAIR_FAILED;
			p->failure = ev[2];
		} else {
			p->state = BT_PAIR_DONE;
		}

		/* Whatever the outcome, the window shuts. Leaving it open
		 * after a pairing finishes is how a machine ends up pairing
		 * with the next thing that asks. */
		p->window_open = false;
		return 0;

	case HCI_EV_AUTH_COMPLETE:
		if (len < 2 + 3)
			return 0;

		if (ev[2]) {
			p->state = BT_PAIR_FAILED;
			p->failure = ev[2];
		}

		return 0;

	default:
		return 0;
	}
}

/* --- the self-test --------------------------------------------------------
 *
 * The cases are the gate and the shapes:
 *
 *   - a request outside the window is refused, and refused *actively*;
 *   - a request naming another device is refused even inside one;
 *   - the positive and negative replies are different commands, or the gate
 *     decides nothing;
 *   - a stored key is handed back so a known device does not pair twice;
 *   - the PIN goes out as characters with a length, not as bytes;
 *   - the window shuts when pairing finishes.
 */
bool bt_pairing_self_test(void)
{
	struct bt_pairing p;
	u8 out[64];
	u32 n;
	bool ok = true;

	static const u8 addr[BT_ADDR_LEN] = {
		0x55, 0x44, 0x33, 0x22, 0x11, 0x00
	};
	static const u8 other[BT_ADDR_LEN] = {
		0x99, 0x44, 0x33, 0x22, 0x11, 0x00
	};

	/* An IO Capability Request from `addr`. */
	u8 iocap[8];

	iocap[0] = HCI_EV_IO_CAP_REQUEST;
	iocap[1] = BT_ADDR_LEN;
	kmemcpy(iocap + 2, addr, BT_ADDR_LEN);

	/* --- the replies must be different commands -------------------- */
	if (HCI_OP_IO_CAP_REPLY == HCI_OP_IO_CAP_NEG_REPLY ||
	    HCI_OP_USER_CONFIRM_REPLY == HCI_OP_USER_CONFIRM_NEG ||
	    HCI_OP_LINK_KEY_REPLY == HCI_OP_LINK_KEY_NEG_REPLY ||
	    HCI_OP_PIN_CODE_REPLY == HCI_OP_PIN_CODE_NEG_REPLY) {
		kputs("  btpair: an accept and a refusal build the same "
		      "command, so the gate decides nothing\n");
		ok = false;
	}

	/* --- closed by default ------------------------------------------ */
	bt_pairing_init(&p);

	if (p.window_open) {
		kputs("  btpair: pairing is open before anybody asked for "
		      "it\n");
		ok = false;
	}

	n = bt_pairing_event(&p, iocap, sizeof(iocap), out, sizeof(out));

	if (!n) {
		kputs("  btpair: an unasked-for pairing request drew no "
		      "answer at all; the controller waits and so does "
		      "everything above it\n");
		ok = false;
	} else if (out[0] != (u8)(HCI_OP_IO_CAP_NEG_REPLY & 0xFF) ||
		   out[1] != (u8)(HCI_OP_IO_CAP_NEG_REPLY >> 8)) {
		kprintf("  btpair: an unasked-for pairing request drew "
			"opcode %02x%02x, expected the negative reply "
			"%04x\n", out[1], out[0],
			(unsigned)HCI_OP_IO_CAP_NEG_REPLY);
		ok = false;
	}

	if (p.refused != 1) {
		kputs("  btpair: a refused request was not counted\n");
		ok = false;
	}

	/* --- open, but for somebody else -------------------------------- */
	bt_pairing_allow(&p, other);
	p.refused = 0;

	n = bt_pairing_event(&p, iocap, sizeof(iocap), out, sizeof(out));

	if (!n || out[0] != (u8)(HCI_OP_IO_CAP_NEG_REPLY & 0xFF) ||
	    p.refused != 1) {
		kputs("  btpair: a device that is not the one being paired "
		      "with was answered, inside somebody else's window\n");
		ok = false;
	}

	/* --- open, and for this device ---------------------------------- */
	bt_pairing_allow(&p, addr);

	n = bt_pairing_event(&p, iocap, sizeof(iocap), out, sizeof(out));

	if (n != 3 + 9) {
		kprintf("  btpair: the IO capability reply is %u bytes, "
			"expected 12\n", n);
		ok = false;
	} else if (out[0] != (u8)(HCI_OP_IO_CAP_REPLY & 0xFF) ||
		   out[1] != (u8)(HCI_OP_IO_CAP_REPLY >> 8)) {
		kprintf("  btpair: a permitted request drew opcode %02x%02x, "
			"expected %04x\n", out[1], out[0],
			(unsigned)HCI_OP_IO_CAP_REPLY);
		ok = false;
	} else if (out[3] != 0x55 || out[9] != BT_IO_NO_INPUT_OUTPUT ||
		   out[10] != BT_OOB_NOT_PRESENT) {
		kprintf("  btpair: the reply carried address byte %02x, "
			"capability %u, oob %u\n", out[3], out[9], out[10]);
		ok = false;
	}

	/* --- user confirmation, which is the accept --------------------- */
	{
		u8 uc[12];

		uc[0] = HCI_EV_USER_CONFIRM_REQ;
		uc[1] = BT_ADDR_LEN + 4;
		kmemcpy(uc + 2, addr, BT_ADDR_LEN);
		uc[8] = 0x40;			/* passkey 123456 */
		uc[9] = 0xE2;
		uc[10] = 0x01;
		uc[11] = 0x00;

		bt_pairing_allow(&p, addr);
		n = bt_pairing_event(&p, uc, sizeof(uc), out, sizeof(out));

		if (n != 3 + BT_ADDR_LEN ||
		    out[0] != (u8)(HCI_OP_USER_CONFIRM_REPLY & 0xFF)) {
			kputs("  btpair: a permitted user confirmation was "
			      "not accepted\n");
			ok = false;
		}

		if (p.last_passkey != 123456) {
			kprintf("  btpair: the passkey read as %u, expected "
				"123456 -- it is a 32-bit value after the "
				"address, least significant byte first\n",
				p.last_passkey);
			ok = false;
		}

		/* And the same request with the window shut. */
		bt_pairing_close(&p);
		p.refused = 0;

		n = bt_pairing_event(&p, uc, sizeof(uc), out, sizeof(out));

		if (!n || out[0] != (u8)(HCI_OP_USER_CONFIRM_NEG & 0xFF) ||
		    p.refused != 1) {
			kputs("  btpair: a user confirmation was accepted "
			      "with the pairing window shut\n");
			ok = false;
		}
	}

	/* --- a stored key means not pairing again ----------------------- */
	{
		u8 lkr[8];
		u8 key[BT_LINK_KEY_LEN];
		unsigned i;

		for (i = 0; i < BT_LINK_KEY_LEN; i++)
			key[i] = (u8)(0xA0 + i);

		lkr[0] = HCI_EV_LINK_KEY_REQ;
		lkr[1] = BT_ADDR_LEN;
		kmemcpy(lkr + 2, addr, BT_ADDR_LEN);

		bt_pairing_init(&p);

		/* With no key, the negative reply -- which is what starts
		 * pairing, and is correct even with the window shut. */
		n = bt_pairing_event(&p, lkr, sizeof(lkr), out, sizeof(out));

		if (!n || out[0] != (u8)(HCI_OP_LINK_KEY_NEG_REPLY & 0xFF)) {
			kputs("  btpair: a link key request for an unknown "
			      "device did not draw the negative reply that "
			      "starts pairing\n");
			ok = false;
		}

		bt_pairing_remember(&p, addr, key, 5);

		n = bt_pairing_event(&p, lkr, sizeof(lkr), out, sizeof(out));

		if (n != 3 + BT_ADDR_LEN + BT_LINK_KEY_LEN) {
			kprintf("  btpair: the link key reply is %u bytes, "
				"expected 25\n", n);
			ok = false;
		} else if (out[0] != (u8)(HCI_OP_LINK_KEY_REPLY & 0xFF) ||
			   out[9] != 0xA0 || out[24] != 0xAF) {
			kprintf("  btpair: the key came back starting %02x "
				"ending %02x, expected a0 and af\n", out[9],
				out[24]);
			ok = false;
		}

		/* A key held for one device is not a key for another. */
		kmemcpy(lkr + 2, other, BT_ADDR_LEN);
		n = bt_pairing_event(&p, lkr, sizeof(lkr), out, sizeof(out));

		if (!n || out[0] != (u8)(HCI_OP_LINK_KEY_NEG_REPLY & 0xFF)) {
			kputs("  btpair: another device's link key request "
			      "was answered with this device's key\n");
			ok = false;
		}
	}

	/* --- the key arrives, and is stored ----------------------------- */
	{
		u8 notify[2 + BT_ADDR_LEN + BT_LINK_KEY_LEN + 1];
		unsigned i;

		notify[0] = HCI_EV_LINK_KEY_NOTIFY;
		notify[1] = BT_ADDR_LEN + BT_LINK_KEY_LEN + 1;
		kmemcpy(notify + 2, addr, BT_ADDR_LEN);

		for (i = 0; i < BT_LINK_KEY_LEN; i++)
			notify[2 + BT_ADDR_LEN + i] = (u8)(0x10 + i);

		notify[2 + BT_ADDR_LEN + BT_LINK_KEY_LEN] = 4;

		bt_pairing_init(&p);
		bt_pairing_event(&p, notify, sizeof(notify), out,
				 sizeof(out));

		if (!p.have_key || p.key[0] != 0x10 ||
		    p.key[BT_LINK_KEY_LEN - 1] != 0x1F || p.key_type != 4) {
			kputs("  btpair: the notified link key was not "
			      "stored as sent\n");
			ok = false;
		}

		if (!same_addr(p.key_addr, addr)) {
			kputs("  btpair: the key was stored against the "
			      "wrong address\n");
			ok = false;
		}
	}

	/* --- the PIN is characters, and the window shuts at the end ----- */
	{
		u8 pinreq[8];
		u8 done[9];

		pinreq[0] = HCI_EV_PIN_CODE_REQ;
		pinreq[1] = BT_ADDR_LEN;
		kmemcpy(pinreq + 2, addr, BT_ADDR_LEN);

		bt_pairing_init(&p);
		bt_pairing_allow(&p, addr);

		n = bt_pairing_event(&p, pinreq, sizeof(pinreq), out,
				     sizeof(out));

		if (n != 3 + BT_ADDR_LEN + 1 + BT_PIN_MAX) {
			kprintf("  btpair: the PIN reply is %u bytes, "
				"expected 26\n", n);
			ok = false;
		} else if (out[9] != 4 || out[10] != '0' || out[13] != '0') {
			kprintf("  btpair: the PIN went out length %u "
				"starting %02x, expected 4 and 30 -- it is "
				"the character '0', not the byte 0\n",
				out[9], out[10]);
			ok = false;
		}

		/* Simple Pairing Complete shuts the window. */
		done[0] = HCI_EV_SIMPLE_PAIR_COMPLETE;
		done[1] = 1 + BT_ADDR_LEN;
		done[2] = 0x00;
		kmemcpy(done + 3, addr, BT_ADDR_LEN);

		bt_pairing_event(&p, done, sizeof(done), out, sizeof(out));

		if (p.state != BT_PAIR_DONE) {
			kprintf("  btpair: pairing completed into state %u\n",
				(unsigned)p.state);
			ok = false;
		}

		if (p.window_open) {
			kputs("  btpair: the window stayed open after "
			      "pairing finished, so the next device to ask "
			      "would be let in too\n");
			ok = false;
		}

		/* A failing completion is a failure, not a success. */
		bt_pairing_allow(&p, addr);
		done[2] = 0x05;			/* authentication failure */
		bt_pairing_event(&p, done, sizeof(done), out, sizeof(out));

		if (p.state != BT_PAIR_FAILED || p.failure != 0x05) {
			kputs("  btpair: a failed Simple Pairing Complete was "
			      "read as success\n");
			ok = false;
		}
	}

	/* --- a PIN length nothing had bounded ---------------------------
	 *
	 * `pin_len` fed a copy into a 23-byte buffer and was bounded only by
	 * the convention that only `bt_pairing_init` wrote it. The field is
	 * public, so the convention was the whole guarantee.
	 *
	 * Both halves are checked: the setter refuses an impossible length,
	 * and the copy refuses one that got through anyway.
	 */
	{
		static const u8 four[4] = { '1', '2', '3', '4' };
		u8 pinreq[8];
		u8 big[BT_PIN_MAX + 8];
		unsigned i;

		for (i = 0; i < sizeof(big); i++)
			big[i] = 'X';

		bt_pairing_init(&p);

		if (!bt_pairing_set_pin(&p, four, 4) || p.pin_len != 4 ||
		    p.pin[0] != '1' || p.pin[3] != '4') {
			kputs("  btpair: a four-character PIN was not "
			      "accepted\n");
			ok = false;
		}

		if (bt_pairing_set_pin(&p, big, BT_PIN_MAX + 1)) {
			kputs("  btpair: a PIN longer than the field holds "
			      "was accepted by the setter\n");
			ok = false;
		}

		if (bt_pairing_set_pin(&p, four, 0)) {
			kputs("  btpair: an empty PIN was accepted\n");
			ok = false;
		}

		/* The setter refused, so the length must be the old one. */
		if (p.pin_len != 4) {
			kprintf("  btpair: a refused PIN still changed the "
				"length to %u\n", p.pin_len);
			ok = false;
		}

		/* And the copy itself, against a field written directly --
		 * which the header now says not to do, and which the struct
		 * being public still allows. The reply must be refused and
		 * nothing past the reply's end may be touched. */
		pinreq[0] = HCI_EV_PIN_CODE_REQ;
		pinreq[1] = BT_ADDR_LEN;
		kmemcpy(pinreq + 2, addr, BT_ADDR_LEN);

		bt_pairing_allow(&p, addr);
		p.pin_len = BT_PIN_MAX + 40;	/* as a stray write would */

		kmemset(out, 0xD3, sizeof(out));
		n = bt_pairing_event(&p, pinreq, sizeof(pinreq), out,
				     sizeof(out));

		if (n != 3 + BT_ADDR_LEN ||
		    out[0] != (u8)(HCI_OP_PIN_CODE_NEG_REPLY & 0xFF)) {
			kprintf("  btpair: an impossible PIN length drew %u "
				"bytes of opcode %02x, expected a negative "
				"reply -- the alternative is copying that "
				"many bytes into a 23-byte buffer\n", n,
				out[0]);
			ok = false;
		}

		for (i = n; i < sizeof(out); i++) {
			if (out[i] != 0xD3) {
				kprintf("  btpair: byte %u past the reply was "
					"overwritten\n", i);
				ok = false;
				break;
			}
		}
	}

	/* --- authenticate and encrypt ----------------------------------- */
	n = hci_auth_requested_build(out, 0xF00C);

	if (n != 5 || out[3] != 0x0C || out[4] != 0x00) {
		kprintf("  btpair: Authentication Requested carried handle "
			"%02x%02x, expected 000c -- twelve bits\n", out[4],
			out[3]);
		ok = false;
	}

	n = hci_set_conn_encrypt_build(out, 0x000C, true);

	if (n != 6 || out[5] != 1) {
		kputs("  btpair: Set Connection Encryption did not ask for "
		      "encryption on\n");
		ok = false;
	}

	return ok;
}
