/*
 * The HID boot protocol, decoded.
 *
 * Moved out of `usb_hid.c` unchanged. See `hid_boot.h` for why: neither of
 * these functions ever looked at a bus, and being `static` in a USB file was
 * the only thing stopping a Bluetooth keyboard from using the decoder a USB
 * keyboard has been proving correct for weeks.
 *
 * The only deliberate change is the rollover: it used to increment a counter
 * that lived beside the USB driver's other statistics, and now it is reported
 * to the caller. A count kept in here would be the sum across every transport,
 * printed by whichever one happened to print it.
 */
#include <recon/kernel/hid_boot.h>
#include <recon/kernel/input.h>
#include <recon/kernel/kstring.h>

/* The first modifier's keycode. The eight bits of a boot keyboard's first byte
 * are left control, shift, alt, meta, then the same four on the right -- which
 * is exactly the order of HID keycodes 224 to 231, so the bit number is the
 * offset and no table is needed. */
#define MODIFIER_FIRST KEY_LEFTCTRL

static bool key_in(const u8 *report, u8 code)
{
	unsigned i;

	for (i = 0; i < HID_BOOT_KEYS; i++)
		if (report[2 + i] == code)
			return true;

	return false;
}

/* --- the keyboard ---------------------------------------------------------
 *
 * Eight bytes: modifiers, a reserved byte, then up to six keycodes. The
 * keycodes are HID usage ids, which are exactly what this kernel's input layer
 * uses -- so there is no translation here at all, which is the whole reason
 * those numbers were chosen.
 */
bool hid_boot_keyboard(struct hid_boot *state, const u8 *report)
{
	unsigned i;

	if (!state || !report)
		return false;

	/* Every keycode being 1 is ErrorRollOver: more keys are held than the
	 * device can report, so **this is not a list of keys**. Treating it as
	 * one would press the letter A six times. The report is dropped and
	 * the previous state left alone -- the keys really are still held, and
	 * the device will say so properly as soon as one is let go. */
	if (report[2] == 1 && report[3] == 1 && report[4] == 1)
		return false;

	/* The modifiers, which are a bitmap and not in the key list. */
	for (i = 0; i < 8; i++) {
		bool now  = (report[0] >> i) & 1u;
		bool was  = state->have_last
			  ? ((state->last[0] >> i) & 1u) : false;

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
	if (state->have_last) {
		for (i = 0; i < HID_BOOT_KEYS; i++) {
			u8 code = state->last[2 + i];

			if (code > 3 && !key_in(report, code))
				input_post(code, INPUT_RELEASE);
		}
	}

	/* Pressed: in the new report and not the old.
	 *
	 * Codes below 4 are not keys -- 0 is "no key here", 1 to 3 are error
	 * conditions -- and posting them would be a keypress nobody made. */
	for (i = 0; i < HID_BOOT_KEYS; i++) {
		u8 code = report[2 + i];

		if (code > 3 && !(state->have_last && key_in(state->last, code)))
			input_post(code, INPUT_PRESS);
	}

	kmemcpy(state->last, report, 8);
	state->have_last = true;
	return true;
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
void hid_boot_mouse(const u8 *report, u32 len)
{
	static const u16 button[3] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE };
	unsigned i;

	if (!report || len < 3)
		return;

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
