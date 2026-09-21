/* The HID boot protocol, decoded — and nothing about how the bytes arrived.
 *
 * These two decoders lived in `usb_hid.c` and were `static` there, which meant
 * a second transport carrying identical reports could not call either. The
 * Bluetooth session found that while writing `bt_hid.c` and stopped rather
 * than duplicating them, which was the right call twice over: two decoders
 * drift, and the copy would have arrived carrying none of the checks the
 * original has.
 *
 * **Neither had anything to do with USB.** The layouts are the *boot
 * protocol's* — eight bytes for a keyboard, three or four for a mouse — and a
 * Bluetooth device in boot mode sends exactly those bytes. Even the comment
 * about HID reporting positive as down, and the PS/2 driver flipping it, is a
 * statement about HID rather than about a bus.
 *
 * Same shape as `display_ops` before a second backend existed, and as
 * `struct usb_device`'s single IN endpoint (KF-248): **one implementation
 * cannot tell its requirements from its accidents.** A second caller is what
 * makes the seam visible, so the seam is cut here rather than argued about.
 */
#ifndef RECON_KERNEL_HID_BOOT_H
#define RECON_KERNEL_HID_BOOT_H

#include <recon/kernel/types.h>

/* Six, because that is what a boot keyboard reports. A seventh key held is not
 * reported as a seventh key; the device sends a rollover instead. */
#define HID_BOOT_KEYS 6

/* What a keyboard decoder has to remember between reports.
 *
 * A boot report is a *state* — which keys are down now — and events are the
 * difference between two of them. So the previous report is not an
 * optimisation, it is the only thing that turns one into presses and releases.
 *
 * A mouse needs no equivalent: its report is already a delta, and its buttons
 * are asked of the input layer rather than remembered here, so that two mice
 * cannot disagree about whether a button is down.
 */
struct hid_boot {
	u8 last[8];
	bool have_last;
};

/* Decode one keyboard report, posting presses and releases.
 *
 * **Returns false when the report was a rollover** and nothing was posted.
 * The caller counts those rather than this file, so that two transports keep
 * their own tallies and the drop is visible where the device is — a count
 * hidden in here would be the sum of every bus, reported as one bus's.
 */
bool hid_boot_keyboard(struct hid_boot *state, const u8 *report);

/* Decode one mouse report of `len` bytes: buttons, then x and y as signed
 * bytes, and a wheel if there are four. */
void hid_boot_mouse(const u8 *report, u32 len);

#endif /* RECON_KERNEL_HID_BOOT_H */
