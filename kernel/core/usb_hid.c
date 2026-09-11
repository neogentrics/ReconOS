/* Keyboards and mice on USB, in the boot protocol.
 *
 * This is what gives aarch64 any input at all: the 8042 is an ISA chip from
 * 1984 and ARM machines have neither the bus nor the chip, so until now that
 * architecture had a keyboard driver it could never use.
 *
 * --- The boot protocol, and why it is not a shortcut ---
 *
 * A HID device describes its own reports in a *report descriptor*: a small
 * stack language saying which bits mean which usages. Parsing it properly is a
 * real piece of work, and a kernel that gets it wrong reads somebody's mouse as
 * a joystick.
 *
 * The boot protocol exists precisely so that something small does not have to.
 * A device that reports subclass 1 promises a fixed report layout -- eight
 * bytes for a keyboard, three or four for a mouse -- and `SET_PROTOCOL(0)`
 * asks for it. It exists so a BIOS can run a keyboard before an operating
 * system loads, which is exactly this kernel's position.
 *
 * What it costs is the extra keys: a boot keyboard reports six at a time and
 * has no media keys, and a boot mouse has three buttons. Both are written down
 * rather than discovered later, and the way past them is a report-descriptor
 * parser, which is its own piece of work and not a variation on this one.
 *
 * --- A report is a state, not an event ---
 *
 * This is the difference from PS/2 and it is the whole of the decoding. A
 * PS/2 keyboard sends *make* and *break* codes: the events themselves. A USB
 * keyboard sends **which keys are held right now**, over and over.
 *
 * So the driver keeps the previous report and compares. A keycode in the new
 * one and not the old is a press; in the old and not the new is a release; in
 * both is nothing at all -- which is what stops a held key generating a press
 * on every poll.
 *
 * One consequence worth stating: **there are no repeats from USB.** The
 * typematic repeat a PS/2 keyboard generates in hardware does not exist here,
 * because the device just keeps saying the key is still down. Whatever wants
 * repeats has to make them from the press and the clock, and that is a policy
 * -- how long before the first, how fast after -- which belongs with whoever
 * knows what the person is typing into.
 *
 * --- Polling, and what it is waiting for ---
 *
 * One thread. It asks each device for its next report and blocks until one
 * arrives, which for an interrupt endpoint is what the controller is for: the
 * transfer sits queued and completes when the device has something to say.
 * The wait yields, so a machine with a keyboard nobody is touching costs
 * nothing.
 */
#include <recon/kernel/xhci.h>
#include <recon/kernel/input.h>
#include <recon/kernel/sched.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/timer.h>

#define HID_CLASS            3
#define HID_SUBCLASS_BOOT    1
#define HID_PROTOCOL_KEYBOARD 1
#define HID_PROTOCOL_MOUSE    2

#define HID_SET_IDLE     0x0A
#define HID_SET_PROTOCOL 0x0B
#define HID_BOOT_PROTOCOL 0

/* The first modifier's keycode. The eight bits of a boot keyboard's first byte
 * are left control, shift, alt, meta, then the same four on the right -- which
 * is exactly the order of HID keycodes 224 to 231, so the bit number is the
 * offset and no table is needed. */
#define MODIFIER_FIRST KEY_LEFTCTRL

/* Six, because that is what a boot keyboard reports. A seventh key held is not
 * reported as a seventh key; the device sends a rollover instead, which is
 * handled below. */
#define BOOT_KEYS 6

/* How many devices this driver will take. A machine with more keyboards than
 * this plugged in is not a machine anybody is holding. */
#define HID_MAX 4

struct hid {
	struct xhci *x;
	struct usb_device *ud;
	bool is_mouse;

	/* The previous report, which is what makes a state into events. */
	u8 last[8];
	bool have_last;

	u64 reports;
};

static struct hid devices[HID_MAX];
static unsigned device_count;
static struct thread *poller;
static u64 rollovers, short_reports, failed_transfers;

/* How many bytes this device's report is.
 *
 * The boot protocol fixes it: eight for a keyboard, four for a mouse with a
 * wheel and three without. Asked of the endpoint rather than assumed, and
 * capped at what the buffer below holds. */
static u32 report_size(const struct hid *h)
{
	u32 n = h->ud->in_packet;

	if (n > 8)
		n = 8;

	return n ? n : (h->is_mouse ? 4u : 8u);
}

/* --- the keyboard ---------------------------------------------------------
 *
 * Eight bytes: modifiers, a reserved byte, then up to six keycodes. The
 * keycodes are HID usage ids, which are exactly what this kernel's input layer
 * uses -- so there is no translation here at all, which is the whole reason
 * those numbers were chosen.
 */
static bool key_in(const u8 *report, u8 code)
{
	unsigned i;

	for (i = 0; i < BOOT_KEYS; i++)
		if (report[2 + i] == code)
			return true;

	return false;
}

static void decode_keyboard(struct hid *h, const u8 *report)
{
	unsigned i;

	/* Every keycode being 1 is ErrorRollOver: more keys are held than the
	 * device can report, so **this is not a list of keys**. Treating it as
	 * one would press the letter A six times. The report is dropped and
	 * the previous state left alone -- the keys really are still held, and
	 * the device will say so properly as soon as one is let go. */
	if (report[2] == 1 && report[3] == 1 && report[4] == 1) {
		rollovers++;
		return;
	}

	/* The modifiers, which are a bitmap and not in the key list. */
	for (i = 0; i < 8; i++) {
		bool now  = (report[0] >> i) & 1u;
		bool was  = h->have_last ? ((h->last[0] >> i) & 1u) : false;

		if (now != was)
			input_post((u16)(MODIFIER_FIRST + i),
				   now ? INPUT_PRESS : INPUT_RELEASE);
	}

	/* Released: in the old report and not the new one.
	 *
	 * Done before the presses so that a key let go in the same report as
	 * another is pressed arrives in that order. It is the order the person
	 * did it in more often than not, and a reader building a chord from
	 * these should see the release first. */
	if (h->have_last) {
		for (i = 0; i < BOOT_KEYS; i++) {
			u8 code = h->last[2 + i];

			if (code > 3 && !key_in(report, code))
				input_post(code, INPUT_RELEASE);
		}
	}

	/* Pressed: in the new report and not the old.
	 *
	 * Codes below 4 are not keys -- 0 is "no key here", 1 to 3 are error
	 * conditions -- and posting them would be a keypress nobody made. */
	for (i = 0; i < BOOT_KEYS; i++) {
		u8 code = report[2 + i];

		if (code > 3 && !(h->have_last && key_in(h->last, code)))
			input_post(code, INPUT_PRESS);
	}

	kmemcpy(h->last, report, 8);
	h->have_last = true;
}

/* --- the mouse ------------------------------------------------------------
 *
 * Three bytes, or four with a wheel: buttons, then x and y as signed bytes.
 *
 * **Y is not negated here**, and that is the difference from the PS/2 mouse.
 * HID reports positive as *down*, which is the direction the input layer uses,
 * so this driver passes it through and the older one flips it. Both arrive
 * meaning the same thing, which is the point of normalising in the driver.
 */
static void decode_mouse(struct hid *h, const u8 *report, u32 len)
{
	static const u16 button[3] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE };
	unsigned i;

	/* Signed bytes, and cast through i8 rather than subtracted: a HID mouse
	 * reports eight bits, unlike PS/2's nine, so the sign is in the byte
	 * itself. */
	input_post_motion(REL_X, (i32)(i8)report[1]);
	input_post_motion(REL_Y, (i32)(i8)report[2]);

	if (len >= 4)
		input_post_motion(REL_WHEEL, (i32)(i8)report[3]);

	for (i = 0; i < 3; i++) {
		bool down = (report[0] >> i) & 1u;

		/* Asked of the input layer rather than remembered here, so that
		 * two mice cannot disagree about whether a button is down. */
		if (down != input_key_held(button[i]))
			input_post(button[i], down ? INPUT_PRESS
						   : INPUT_RELEASE);
	}
}

/* --- the thread ----------------------------------------------------------- */

static void poll(void *arg)
{
	unsigned i;

	(void)arg;

	/* One transfer outstanding on every device, all at once.
	 *
	 * This is the difference between a keyboard that works and one that
	 * looks like it does. Asking each device in turn and waiting for its
	 * answer means a keypress waits behind however long the mouse takes to
	 * be moved -- which, for a mouse nobody is touching, is for ever. An
	 * interrupt endpoint is *meant* to be left outstanding; the controller
	 * completes it when the device has something to say. */
	for (i = 0; i < device_count; i++) {
		struct hid *h = &devices[i];

		kmemset(h->ud->buffer, 0, 8);
		xhci_transfer_queue(h->x, h->ud, h->ud->buffer, report_size(h));
	}

	for (;;) {
		bool any = false;

		for (i = 0; i < device_count; i++) {
			struct hid *h = &devices[i];
			u8 *buf = h->ud->buffer;
			u32 want = report_size(h);
			u32 residue = 0;
			u32 got;
			bool ok = false;

			if (!xhci_transfer_poll(h->x, h->ud, &residue, &ok))
				continue;

			any = true;
			got = (residue <= want) ? want - residue : 0;

			if (!ok) {
				failed_transfers++;
			} else if (got < (h->is_mouse ? 3u : 8u)) {
				/* Shorter than the protocol promises is not a
				 * report. Counted rather than padded: padding
				 * with zeroes reads as every key released. */
				short_reports++;
			} else {
				h->reports++;

				if (h->is_mouse)
					decode_mouse(h, buf, got);
				else
					decode_keyboard(h, buf);
			}

			/* Straight back on, before anything else is looked at.
			 * A device with no transfer outstanding is a device
			 * that will never report again, and nothing anywhere
			 * would say so. */
			kmemset(buf, 0, 8);
			xhci_transfer_queue(h->x, h->ud, buf, want);
		}

		/* Nothing had anything to say. Sleeping rather than yielding,
		 * because yielding here is a thread asking the controller as
		 * fast as the processor can and calling it idle. */
		if (!any)
			timer_sleep_ns(4000000ull);	/* 4 ms */
	}
}

/* --- attaching ------------------------------------------------------------ */

bool usb_hid_attach(struct xhci *x, struct usb_device *ud)
{
	struct hid *h;

	if (ud->usb_class != HID_CLASS)
		return false;

	/* Only the boot subclass. A device that does not offer it describes
	 * itself in a report descriptor and this driver cannot read one --
	 * saying so beats guessing at a layout. */
	if (ud->usb_subclass != HID_SUBCLASS_BOOT) {
		kputs("  usbhid       : a HID device that does not offer the "
		      "boot protocol; it needs a report descriptor parser\n");
		return false;
	}

	if (ud->usb_protocol != HID_PROTOCOL_KEYBOARD &&
	    ud->usb_protocol != HID_PROTOCOL_MOUSE)
		return false;

	if (device_count >= HID_MAX)
		return false;

	if (!ud->configured || !ud->in_ep || !ud->in_is_interrupt) {
		kputs("  usbhid       : a keyboard or mouse with no interrupt "
		      "endpoint the controller would take\n");
		return false;
	}

	/* Into the boot protocol. A device powers up in the report protocol,
	 * where the layout is whatever its descriptor says -- so without this
	 * the eight bytes read below are eight bytes of something else. */
	if (!xhci_control_transfer(x, ud, 0x21, HID_SET_PROTOCOL,
				   HID_BOOT_PROTOCOL, ud->interface, 0, 0)) {
		kputs("  usbhid       : the device would not take the boot "
		      "protocol\n");
		return false;
	}

	/* Report only when something changes. The default is to repeat the
	 * current state on a timer, which for a keyboard nobody is touching is
	 * a transfer every few milliseconds saying nothing. Not fatal if the
	 * device refuses -- some do -- because the decoding above produces no
	 * events from an unchanged report anyway. */
	(void)xhci_control_transfer(x, ud, 0x21, HID_SET_IDLE, 0,
				    ud->interface, 0, 0);

	h = &devices[device_count++];
	h->x = x;
	h->ud = ud;
	h->is_mouse = ud->usb_protocol == HID_PROTOCOL_MOUSE;
	h->have_last = false;
	h->reports = 0;

	kprintf("  usbhid       : a %s on interrupt endpoint %02x\n",
		h->is_mouse ? "mouse" : "keyboard", ud->in_ep);

	/* One thread for all of them, started with the first. A thread each
	 * would be a thread per device blocking on its own transfer, and the
	 * controller only does one at a time anyway. */
	if (!poller)
		poller = thread_create("usbhid", poll, 0);

	return true;
}

unsigned usb_hid_count(void)
{
	return device_count;
}

void usb_hid_print_summary(void)
{
	unsigned i;

	if (!device_count)
		return;

	for (i = 0; i < device_count; i++) {
		/* The count, once there is one. This prints during boot, before
		 * anybody could have touched the machine, and a bare zero reads
		 * as a device that does not work. */
		if (devices[i].reports)
			kprintf("  usbhid%u      : %s, %llu report(s)\n", i,
				devices[i].is_mouse ? "mouse" : "keyboard",
				(unsigned long long)devices[i].reports);
		else
			kprintf("  usbhid%u      : %s, nothing said yet\n", i,
				devices[i].is_mouse ? "mouse" : "keyboard");
	}

	/* Both are ordinary and neither is silent. A rollover means somebody
	 * held more keys than the protocol carries; a short report means a
	 * device said less than it promised, which is worth seeing because the
	 * alternative reading -- every key released -- is what padding would
	 * have produced. */
	if (rollovers)
		kprintf("  rollovers    : %llu report(s) with too many keys "
			"held to name them\n", (unsigned long long)rollovers);

	if (short_reports)
		kprintf("  short        : %llu report(s) shorter than the "
			"protocol promises\n",
			(unsigned long long)short_reports);
}

/* --- the self-test --------------------------------------------------------
 *
 * No hardware is touched, and none needs to be. The part of this driver with
 * somewhere to go wrong is the *decoding* -- turning a report that says which
 * keys are held into the events of keys going down and coming up -- and that is
 * a pure function of two reports. Synthetic ones exercise it exactly.
 *
 * What is deliberately **not** covered: anything involving the controller. A
 * test that needed a USB keyboard attached would report a failure on every
 * machine that has none, which is most of them and all of the rig. The
 * enumeration, the endpoint configuration and the transfers are exercised by
 * the machine having a device or not, and say so either way in the summary.
 *
 * The checks below are chosen for having a wrong answer that still looks like a
 * working keyboard:
 *
 *   - a held key must produce **one** press, not one per report. A USB keyboard
 *     repeats its report; a driver that posted a press each time would type
 *     hundreds of characters from one keystroke;
 *   - a rollover must produce **nothing**. Six ones is "too many keys to name
 *     them", and read as a key list it is the letter A six times;
 *   - a release must arrive **before** the press in the same report, because a
 *     reader building a chord from these needs the order the person did it in.
 */
static bool feed(struct hid *h, u8 mods, u8 k0, u8 k1)
{
	u8 report[8];

	kmemset(report, 0, sizeof(report));
	report[0] = mods;
	report[2] = k0;
	report[3] = k1;

	decode_keyboard(h, report);
	return true;
}

static unsigned drain(void)
{
	struct input_event e;
	unsigned n = 0;

	while (input_take(&e))
		n++;

	return n;
}

bool usb_hid_self_test(void)
{
	struct hid h;
	struct input_event e;
	bool ok = true;

	kmemset(&h, 0, sizeof(h));
	h.is_mouse = false;
	h.have_last = false;

	drain();

	/* --- a key goes down ------------------------------------------- */
	feed(&h, 0, KEY_A, 0);

	if (!input_take(&e)) {
		kputs("  usbhid: a key in a report produced no event\n");
		return false;
	}

	if (e.code != KEY_A || e.kind != INPUT_PRESS) {
		kprintf("  usbhid: expected a press of %u, got code %u "
			"kind %u\n", (unsigned)KEY_A, e.code, e.kind);
		ok = false;
	}

	/* --- and stays down, silently ----------------------------------- */
	feed(&h, 0, KEY_A, 0);

	if (drain()) {
		kputs("  usbhid: a key still held produced another event -- a "
		      "USB keyboard repeats its report, so one keystroke would "
		      "type for as long as it is held\n");
		ok = false;
	}

	/* --- rollover says nothing -------------------------------------- */
	{
		u8 roll[8];

		kmemset(roll, 0, sizeof(roll));
		roll[2] = roll[3] = roll[4] = 1;
		roll[5] = roll[6] = roll[7] = 1;

		decode_keyboard(&h, roll);

		if (drain()) {
			kputs("  usbhid: a rollover report produced events -- "
			      "six ones means too many keys to name, and read "
			      "as a key list it is the letter A six times\n");
			ok = false;
		}

		/* And it did not disturb what was held: the keys really are
		 * still down, and the device will say so properly as soon as
		 * one is let go. */
		if (!h.have_last || h.last[2] != KEY_A) {
			kputs("  usbhid: a rollover overwrote the state, so "
			      "every key would appear to be released\n");
			ok = false;
		}
	}

	/* --- released --------------------------------------------------- */
	feed(&h, 0, 0, 0);

	if (!input_take(&e) || e.code != KEY_A || e.kind != INPUT_RELEASE) {
		kputs("  usbhid: letting a key go did not release it\n");
		ok = false;
	}

	drain();

	/* --- a modifier is a bit, not a keycode ------------------------- */
	feed(&h, 1u << 1, 0, 0);		/* left shift */

	if (!input_take(&e) || e.code != KEY_LEFTSHIFT ||
	    e.kind != INPUT_PRESS) {
		kprintf("  usbhid: bit 1 of the modifier byte is not left "
			"shift (%u)\n", (unsigned)KEY_LEFTSHIFT);
		ok = false;
	}

	feed(&h, 0, 0, 0);
	drain();

	/* --- one let go and one pressed, in that order ------------------ */
	feed(&h, 0, KEY_A, 0);
	drain();

	feed(&h, 0, KEY_TAB, 0);

	{
		struct input_event first, second;

		if (!input_take(&first) || !input_take(&second)) {
			kputs("  usbhid: swapping one key for another did not "
			      "produce two events\n");
			ok = false;
		} else if (first.kind != INPUT_RELEASE || first.code != KEY_A ||
			   second.kind != INPUT_PRESS ||
			   second.code != KEY_TAB) {
			kputs("  usbhid: a key let go and a key pressed in one "
			      "report did not arrive released-first, so a "
			      "reader cannot tell which order they happened "
			      "in\n");
			ok = false;
		}
	}

	feed(&h, 0, 0, 0);
	drain();

	/* --- the codes that are not keys -------------------------------- */
	feed(&h, 0, 2, 3);		/* POSTFail and ErrorUndefined */

	if (drain()) {
		kputs("  usbhid: an error code in the key list was posted as a "
		      "keypress nobody made\n");
		ok = false;
	}

	drain();
	return ok;
}
