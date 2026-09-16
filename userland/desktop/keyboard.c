/*
 * Events from `/dev/input`, turned into keystrokes. See keyboard.h.
 *
 * No system call in this file. It is handed events and gives back keystrokes,
 * which is what lets the whole of it be driven from a test with a table of
 * events written by hand -- including the malformed ones, which are the ones
 * worth testing and the hardest to arrange on a real machine.
 */

#include <stddef.h>

/*
 * By relative path, so it is found without putting `userland/include` on
 * anybody's include path. That matters for the suite that drives this file:
 * a test compiled against this library's <stdio.h> loses the host's `printf`,
 * and the rule it is really obeying is the older one -- a test that includes
 * ReconOS's headers is a test comparing ReconOS against itself.
 */
#include "../include/sys/input.h"

#include "keyboard.h"

bool recon_input_event_is_sane(const struct recon_input_event *event) {
	if (event == NULL) {
		return false;
	}

	/*
	 * Both of these are promises the kernel's header makes, and both are
	 * checked rather than trusted -- because what is on the other side of
	 * the promise is a byte layout written down in two files, and a layout
	 * disagreement does not crash. It hands back numbers.
	 *
	 * `reserved` is the sharper of the two: it is the one field whose only
	 * job is to be zero, so anything else in it means the bytes are not
	 * laid out the way this program believes. A `kind` read one byte off
	 * is a press arriving as a release, which is a keyboard that holds
	 * Shift down for ever and never reports anything wrong.
	 */
	if (event->reserved != 0) {
		return false;
	}
	if (event->kind != RECON_INPUT_MOTION && event->value != 0) {
		return false;
	}
	if (event->kind > RECON_INPUT_MOTION) {
		return false;
	}
	return true;
}

/* Whether a code names a key on the keyboard page at all. */
static bool is_a_key(unsigned code) {
	/*
	 * Buttons and axes live above the keyboard page deliberately, so that
	 * a keycode and a button cannot collide as the tables grow. This is
	 * the line that relies on that.
	 */
	return code <= RECON_KEY_CODE_MAX;
}

bool recon_keyboard_event(struct recon_keyboard *keyboard,
		const struct recon_input_event *event,
		struct recon_keystroke *out) {
	if (keyboard == NULL || out == NULL ||
			!recon_input_event_is_sane(event)) {
		return false;
	}

	out->symbol = 0;
	out->modifiers = keyboard->modifiers;
	out->repeat = false;

	/* A mouse is not a keyboard. Nothing here has anywhere to put it yet. */
	if (event->kind == RECON_INPUT_MOTION || !is_a_key(event->code)) {
		return false;
	}

	bool pressed = (event->kind == RECON_INPUT_PRESS ||
			event->kind == RECON_INPUT_REPEAT);

	/*
	 * The modifiers move first, and on releases as well as presses.
	 *
	 * `recon_key_modifiers_after` is where the rules live -- that Caps
	 * Lock is a latch rather than a held key, and that a release clears a
	 * modifier whether or not a press was seen. That second one is the
	 * kernel's rule too, and it has to be honoured here as well: after a
	 * dropped event, or a program that started while a key was held, the
	 * release arrives for a press nobody saw.
	 */
	unsigned before = keyboard->modifiers;

	keyboard->modifiers = recon_key_modifiers_after(keyboard->modifiers,
		event->code, pressed);

	if (!pressed) {
		return false;
	}

	/*
	 * A modifier going down is not a keystroke, and saying so by comparing
	 * the state before and after is better than a list of modifier keycodes
	 * here: a list is a second copy of something `recon_key.c` already
	 * knows, and the two would disagree the day a key is added to one of
	 * them.
	 *
	 * **It covers Caps Lock too**, including the case that looks like it
	 * needs naming -- pressing Caps Lock when it is already on, which turns
	 * it off. This file used to carry a branch for exactly that, with a
	 * comment saying the comparison could not see it. The comment was
	 * wrong: `recon_key_modifiers_after` toggles the bit on every press, so
	 * the state differs from `before` in *both* directions and this line
	 * catches it either way. Deleting that branch changed no output, which
	 * is how it was found.
	 */
	if (keyboard->modifiers != before) {
		return false;
	}

	/*
	 * The modifiers the key was typed under. `recon_key_from_hid` is given
	 * the same ones, so the symbol a caller receives and the modifiers
	 * beside it describe one moment rather than two.
	 *
	 * Equal to `before` here, provably -- the line above returns unless
	 * they are. Written as the live state because that is what it *means*,
	 * and said here because a mutation swapping the two is an equivalent
	 * mutant: no check can distinguish them, and one that appeared to would
	 * be a check that cannot fail.
	 */
	out->modifiers = keyboard->modifiers;
	out->repeat = (event->kind == RECON_INPUT_REPEAT);
	out->symbol = recon_key_from_hid(event->code, keyboard->modifiers);

	if (out->symbol == 0) {
		/*
		 * A key this layout has no symbol for. Counted rather than
		 * ignored, for the reason the kernel counts the events it
		 * drops: "some keys do nothing" is otherwise a complaint
		 * nobody can check.
		 */
		keyboard->unknown_keys++;
		return false;
	}
	return true;
}
