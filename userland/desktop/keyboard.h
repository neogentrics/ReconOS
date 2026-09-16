/*
 * Events from `/dev/input`, turned into keystrokes.
 *
 * --- The split, again ---
 *
 * Nothing here opens anything or makes a system call. It is handed events and
 * gives back keystrokes, so the whole of it can be driven from a test on the
 * host with a table of events written by hand -- including the ones a real
 * keyboard produces only when something has already gone wrong, which are the
 * ones worth testing and the hardest to arrange on a machine.
 *
 * `main.c` is what opens the device and reads the bytes.
 *
 * --- What it is actually for ---
 *
 * The kernel delivers **positions**: "the key in position 4 went down". The
 * desktop wants **meanings**: "the person typed `a`". Between those two sits a
 * layout, and on Linux that is xkbcommon -- which does not exist on a ReconOS
 * machine. `include/recon_key.h` is ReconOS's own answer and was written and
 * tested in v0.4.51 with no caller at all. This is the caller.
 *
 * What this adds on top of `recon_key_from_hid` is the part that is about a
 * *stream* rather than a single event: which modifiers are held, and what a
 * repeat means.
 */

#ifndef RECON_DESKTOP_KEYBOARD_H
#define RECON_DESKTOP_KEYBOARD_H

#include <stdbool.h>

#include "recon_key.h"

struct recon_input_event;

/*
 * What a keyboard is holding down.
 *
 * Kept by this file rather than asked of the kernel, because the kernel's own
 * header says the same thing about its side: modifier state is **derived from
 * events**, never counted alongside them. A release of a key that was not held
 * is a no-op and never a decrement -- a counter there goes negative, wraps,
 * and leaves the machine believing Shift is held for ever.
 *
 * Zero-initialised is a valid starting state, and it is the honest one: a
 * program that starts while somebody is holding Shift does not know that, and
 * will find out on the release.
 */
struct recon_keyboard {
	unsigned modifiers;	/* RECON_MOD_* */

	/*
	 * How many events have been seen that named a key this layout has no
	 * symbol for. Not an error -- the kernel delivers every keycode,
	 * including ones nobody has named yet -- but a number worth having,
	 * because "some keys do nothing" is otherwise an unfalsifiable
	 * complaint. The same argument the kernel's queue makes for counting
	 * what it drops.
	 */
	unsigned unknown_keys;
};

/*
 * One keystroke: what the person typed.
 */
struct recon_keystroke {
	recon_keysym symbol;	/* 0 when the event produced no symbol */
	unsigned modifiers;	/* what was held *at the time*, not afterwards */

	/*
	 * True when the keyboard's own typematic circuit sent this again,
	 * rather than somebody pressing the key a second time.
	 *
	 * A text field wants these -- holding backspace should delete more
	 * than one character. A game does not: a repeat is not a new jump. The
	 * caller decides, which it can only do if the distinction survives to
	 * here.
	 */
	bool repeat;
};

/*
 * Feed one event in.
 *
 * Returns true when it produced a keystroke, and then `out` holds it. False
 * for everything that is not somebody typing: a release, a modifier going
 * down, a mouse moving, a key with no symbol in this layout, and a malformed
 * event.
 *
 * **A modifier press produces no keystroke and is not nothing**: it changes
 * what the next one means. That is why this returns a bool rather than a
 * symbol -- zero would be ambiguous between "nothing happened" and "something
 * happened that has no symbol", and those need different handling by anything
 * that is counting.
 */
bool recon_keyboard_event(struct recon_keyboard *keyboard,
	const struct recon_input_event *event, struct recon_keystroke *out);

/*
 * Whether an event is one this program can believe.
 *
 * The kernel promises `reserved` is zero and that `value` is zero on anything
 * that is not motion. **Checked rather than trusted**, because the thing on
 * the other side of that promise is a byte layout written down twice -- and
 * the failure mode of a layout disagreement is not a crash, it is events that
 * look plausible and mean something else. A `kind` read one byte off is a
 * press that arrives as a release.
 *
 * A short read is the other case: `/dev/input` reads whole events, so a length
 * that is not a multiple of one is a reader that has lost its place.
 */
bool recon_input_event_is_sane(const struct recon_input_event *event);

#endif /* RECON_DESKTOP_KEYBOARD_H */
