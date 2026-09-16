/*
 * Events from `/dev/input`, turned into keystrokes.
 *
 * --- What is worth checking here ---
 *
 * `recon_key.c` already holds the layout: that Shift and Caps Lock combine by
 * exclusive-or, that Caps Lock does not touch the digits, that Ctrl-C is the
 * letter C with Ctrl held. Forty-three checks and five mutations, in v0.4.51.
 * None of that is repeated.
 *
 * What is new here is everything about a **stream**: state that survives
 * between events, and the events a real keyboard only produces once something
 * has already gone wrong. Those are the hard ones to arrange on a machine and
 * the easy ones to write down in a table, which is the whole argument for this
 * file having no system call in it.
 *
 *   - **a release with no press.** The kernel's header says a reader can see
 *     one and must not be broken by it -- after a dropped event, or a program
 *     that started while a key was held. The failure it prevents is a machine
 *     that believes Shift is held for ever.
 *   - **a repeat is not a press.** Holding backspace should delete more than
 *     one character; holding a jump key should not jump twice. Both callers
 *     are right and only survive if the distinction reaches them.
 *   - **a malformed event is refused.** The two fields the kernel promises are
 *     zero are the canary for a byte layout written down in two files, and a
 *     layout disagreement does not crash -- it hands back plausible numbers.
 *   - **the two headers still agree.** Read as text, because a `_Static_assert`
 *     can hold this side's layout and cannot notice the kernel changing a
 *     number underneath it.
 *
 * Run with: ./build/recon_desktop_keyboard_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* By relative path, so the host's <stdio.h> is the one above. */
#include "../include/sys/input.h"

#include "keyboard.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
	g_checks++;
	if (!condition) {
		g_failures++;
		printf("  FAIL: %s\n", what);
	}
}

/* One event, written the way a driver would post it. */
static struct recon_input_event ev(unsigned code, enum recon_input_kind kind) {
	struct recon_input_event e;

	memset(&e, 0, sizeof(e));
	e.when = 1;
	e.code = (u16)code;
	e.kind = (u8)kind;
	return e;
}

/* Feed one event and say what came out, for the cases that expect a symbol. */
static recon_keysym typed(struct recon_keyboard *kb, unsigned code,
		enum recon_input_kind kind) {
	struct recon_input_event e = ev(code, kind);
	struct recon_keystroke stroke;

	if (!recon_keyboard_event(kb, &e, &stroke)) {
		return 0;
	}
	return stroke.symbol;
}

static void test_a_letter(void) {
	printf("a key goes down and a letter comes out\n");

	struct recon_keyboard kb = { 0 };

	check(typed(&kb, RECON_KEY_CODE_A, RECON_INPUT_PRESS) == RECON_KEY_a,
	      "position 4 pressed is a lowercase a");
	check(typed(&kb, RECON_KEY_CODE_A, RECON_INPUT_RELEASE) == 0,
	      "and releasing it is not a second one");
	check(kb.unknown_keys == 0, "nothing was unrecognised");
}

static void test_shift_is_held_across_events(void) {
	printf("shift is state, not an event\n");

	struct recon_keyboard kb = { 0 };

	/*
	 * The thing this file exists for. `recon_key_from_hid` is given the
	 * modifiers and does the right thing with them; knowing that Shift is
	 * *still down* three events later is what has to be kept here.
	 */
	check(typed(&kb, RECON_KEY_CODE_LEFTSHIFT, RECON_INPUT_PRESS) == 0,
	      "pressing shift types nothing");
	check((kb.modifiers & RECON_MOD_SHIFT) != 0, "but it is held");

	check(typed(&kb, RECON_KEY_CODE_A, RECON_INPUT_PRESS) == RECON_KEY_A,
	      "so the next key is a capital");
	check(typed(&kb, RECON_KEY_CODE_A, RECON_INPUT_RELEASE) == 0,
	      "releasing the letter types nothing");
	check(typed(&kb, RECON_KEY_CODE_A, RECON_INPUT_PRESS) == RECON_KEY_A,
	      "and the one after that is still a capital");

	check(typed(&kb, RECON_KEY_CODE_LEFTSHIFT, RECON_INPUT_RELEASE) == 0,
	      "releasing shift types nothing");
	check((kb.modifiers & RECON_MOD_SHIFT) == 0, "and it is no longer held");
	check(typed(&kb, RECON_KEY_CODE_A, RECON_INPUT_PRESS) == RECON_KEY_a,
	      "so the next key is lowercase again");
}

static void test_a_release_nobody_saw_a_press_for(void) {
	printf("a release with no press does not stick\n");

	/*
	 * The kernel's header names this and says a reader must not be broken
	 * by it: after a dropped event, or a program that started while
	 * somebody was already holding Shift, the release arrives alone.
	 *
	 * **The failure it prevents is a keyboard that types in capitals until
	 * the machine is restarted**, with nothing reporting a fault. Both
	 * sides derive modifier state from events rather than counting it for
	 * exactly this reason, and a counter here would go negative, wrap, and
	 * leave Shift set for ever.
	 */
	struct recon_keyboard kb = { 0 };

	check(typed(&kb, RECON_KEY_CODE_LEFTSHIFT, RECON_INPUT_RELEASE) == 0,
	      "a release nobody saw a press for types nothing");
	check(kb.modifiers == 0, "and leaves nothing held");
	check(typed(&kb, RECON_KEY_CODE_A, RECON_INPUT_PRESS) == RECON_KEY_a,
	      "so typing afterwards is lowercase, not stuck in capitals");

	/* And the other order: two releases in a row. */
	(void)typed(&kb, RECON_KEY_CODE_LEFTSHIFT, RECON_INPUT_PRESS);
	(void)typed(&kb, RECON_KEY_CODE_LEFTSHIFT, RECON_INPUT_RELEASE);
	(void)typed(&kb, RECON_KEY_CODE_LEFTSHIFT, RECON_INPUT_RELEASE);
	check(kb.modifiers == 0, "two releases in a row leave nothing held");
	check(typed(&kb, RECON_KEY_CODE_A, RECON_INPUT_PRESS) == RECON_KEY_a,
	      "and typing is still lowercase");
}

static void test_a_repeat_is_not_a_press(void) {
	printf("a repeat says it is one\n");

	struct recon_keyboard kb = { 0 };
	struct recon_input_event e;
	struct recon_keystroke stroke;

	e = ev(RECON_KEY_CODE_A, RECON_INPUT_PRESS);
	check(recon_keyboard_event(&kb, &e, &stroke), "the press produces one");
	check(!stroke.repeat, "and says it is not a repeat");

	e = ev(RECON_KEY_CODE_A, RECON_INPUT_REPEAT);
	check(recon_keyboard_event(&kb, &e, &stroke),
	      "a repeat produces a keystroke too");
	check(stroke.symbol == RECON_KEY_a, "with the same symbol");
	check(stroke.repeat, "and says it is a repeat");

	/*
	 * Both callers are right and neither can be served if this is folded
	 * away: a text field wants the repeat, a game does not. Collapsing
	 * them into one kind means every program that cares has to
	 * reconstruct it from timing, badly.
	 */
	e = ev(RECON_KEY_CODE_A, RECON_INPUT_REPEAT);
	e.value = 0;
	check(recon_keyboard_event(&kb, &e, &stroke) && stroke.repeat,
	      "and a second repeat is still a repeat, not a press");
}

static void test_caps_lock_is_a_latch(void) {
	printf("caps lock latches and types nothing itself\n");

	struct recon_keyboard kb = { 0 };

	check(typed(&kb, RECON_KEY_CODE_CAPSLOCK, RECON_INPUT_PRESS) == 0,
	      "pressing caps lock types nothing");
	check((kb.modifiers & RECON_MOD_CAPS) != 0, "and it is on");
	check(typed(&kb, RECON_KEY_CODE_CAPSLOCK, RECON_INPUT_RELEASE) == 0,
	      "releasing it types nothing");
	check((kb.modifiers & RECON_MOD_CAPS) != 0,
	      "and it stays on, because it is a latch and not a held key");

	check(typed(&kb, RECON_KEY_CODE_A, RECON_INPUT_PRESS) == RECON_KEY_A,
	      "so letters are capitals");

	/*
	 * Pressing it again turns it off, which is the case that the general
	 * rule -- "a modifier press produces no keystroke" -- cannot recognise
	 * by comparing state, because the state changes in the other
	 * direction. It must still type nothing.
	 */
	check(typed(&kb, RECON_KEY_CODE_CAPSLOCK, RECON_INPUT_PRESS) == 0,
	      "pressing it again types nothing either");
	check((kb.modifiers & RECON_MOD_CAPS) == 0, "and turns it off");
	check(typed(&kb, RECON_KEY_CODE_A, RECON_INPUT_PRESS) == RECON_KEY_a,
	      "so letters are lowercase again");
}

static void test_shift_and_caps_together(void) {
	printf("shift and caps lock combine by exclusive or\n");

	/*
	 * `recon_key.c` owns this rule and is tested on it. It is repeated
	 * here for one reason: it is the only rule whose *inputs* both come
	 * from the state this file keeps, so a fault in the state shows up as
	 * a fault in this rule and nowhere else.
	 */
	struct recon_keyboard kb = { 0 };

	(void)typed(&kb, RECON_KEY_CODE_CAPSLOCK, RECON_INPUT_PRESS);
	(void)typed(&kb, RECON_KEY_CODE_LEFTSHIFT, RECON_INPUT_PRESS);

	check(typed(&kb, RECON_KEY_CODE_A, RECON_INPUT_PRESS) == RECON_KEY_a,
	      "caps on and shift held gives lowercase, not a second capital");

	(void)typed(&kb, RECON_KEY_CODE_LEFTSHIFT, RECON_INPUT_RELEASE);
	check(typed(&kb, RECON_KEY_CODE_A, RECON_INPUT_PRESS) == RECON_KEY_A,
	      "and letting shift go gives a capital again");
}

static void test_the_modifiers_beside_a_symbol_are_the_ones_it_was_typed_under(void) {
	printf("a keystroke carries the modifiers it was typed under\n");

	struct recon_keyboard kb = { 0 };
	struct recon_input_event e;
	struct recon_keystroke stroke;

	(void)typed(&kb, RECON_KEY_CODE_LEFTCTRL, RECON_INPUT_PRESS);

	e = ev(RECON_KEY_CODE_A, RECON_INPUT_PRESS);
	check(recon_keyboard_event(&kb, &e, &stroke), "ctrl-a is a keystroke");

	/*
	 * Ctrl-C is the letter C with Ctrl held, and not a 0x03 -- which is
	 * what a text box gets if the symbol is mangled instead of the
	 * modifier being carried. The symbol and the modifiers have to
	 * describe one moment, which is why they travel together.
	 */
	check(stroke.symbol == RECON_KEY_a, "the symbol is the letter");
	check((stroke.modifiers & RECON_MOD_CTRL) != 0,
	      "and ctrl is reported beside it rather than folded into it");
}

static void test_a_malformed_event_is_refused(void) {
	printf("an event that cannot be right is refused\n");

	struct recon_keyboard kb = { 0 };
	struct recon_keystroke stroke;
	struct recon_input_event e;

	/*
	 * `reserved` is the canary for a byte layout written down in two
	 * files. A layout disagreement does not crash -- it hands back
	 * plausible numbers, and a `kind` read one byte off is a press
	 * arriving as a release.
	 */
	e = ev(RECON_KEY_CODE_A, RECON_INPUT_PRESS);
	e.reserved = 1;
	check(!recon_input_event_is_sane(&e), "a non-zero reserved is refused");
	check(!recon_keyboard_event(&kb, &e, &stroke), "and produces nothing");
	check(kb.modifiers == 0, "and does not move the modifiers");

	/* The kernel promises value is zero on anything that is not motion. */
	e = ev(RECON_KEY_CODE_LEFTSHIFT, RECON_INPUT_PRESS);
	e.value = 5;
	check(!recon_input_event_is_sane(&e), "a value on a press is refused");
	check(!recon_keyboard_event(&kb, &e, &stroke), "and produces nothing");
	check((kb.modifiers & RECON_MOD_SHIFT) == 0,
	      "and above all does not leave shift held");

	/* A kind the enum does not have. */
	e = ev(RECON_KEY_CODE_A, RECON_INPUT_PRESS);
	e.kind = 9;
	check(!recon_input_event_is_sane(&e), "an unknown kind is refused");

	check(!recon_keyboard_event(&kb, NULL, &stroke), "and so is no event");
	check(!recon_input_event_is_sane(NULL), "and so is no event at all");
}

static void test_a_mouse_is_not_a_keyboard(void) {
	printf("motion and buttons produce no letters\n");

	struct recon_keyboard kb = { 0 };
	struct recon_input_event e;
	struct recon_keystroke stroke;

	e = ev(RECON_REL_X, RECON_INPUT_MOTION);
	e.value = -12;
	check(recon_input_event_is_sane(&e), "motion carrying a value is fine");
	check(!recon_keyboard_event(&kb, &e, &stroke), "and types nothing");

	/*
	 * A button is a key that happens to be under your hand, and the kernel
	 * reports it with the same press and release kinds. It must not reach
	 * the layout: buttons are numbered above the keyboard page precisely
	 * so a keycode and a button cannot collide, and this is the line that
	 * relies on it.
	 */
	e = ev(RECON_BTN_LEFT, RECON_INPUT_PRESS);
	check(!recon_keyboard_event(&kb, &e, &stroke),
	      "a mouse button types nothing");
	check(kb.unknown_keys == 0,
	      "and is not counted as a key with no symbol, because it is not one");
}

static void test_a_key_with_no_symbol_is_counted(void) {
	printf("a key this layout has no symbol for is counted\n");

	struct recon_keyboard kb = { 0 };

	/*
	 * 200 is inside the keyboard page and is not a key the US layout
	 * names. It is delivered rather than refused -- the kernel says so --
	 * and what happens to it is worth a number: "some keys do nothing" is
	 * otherwise a complaint nobody can check, which is the same argument
	 * the kernel's queue makes for counting what it drops.
	 */
	check(typed(&kb, 200, RECON_INPUT_PRESS) == 0, "it types nothing");
	check(kb.unknown_keys == 1, "and is counted");
	check(typed(&kb, 200, RECON_INPUT_PRESS) == 0, "twice");
	check(kb.unknown_keys == 2, "and counted twice");
	check(kb.modifiers == 0, "and changes nothing about the modifiers");
}

static void test_a_sentence(void) {
	printf("a whole sentence, the way somebody would type it\n");

	/*
	 * Everything above tests one rule at a time. This is the shape a real
	 * stream has -- shift pressed and released around a letter, a space,
	 * a repeat -- because the faults this file can have are about what one
	 * event leaves behind for the next.
	 */
	struct recon_keyboard kb = { 0 };
	char out[64];
	size_t at = 0;

	static const struct { unsigned code; enum recon_input_kind kind; } TYPING[] = {
		{ RECON_KEY_CODE_LEFTSHIFT, RECON_INPUT_PRESS },
		{ RECON_KEY_CODE_A + 7,     RECON_INPUT_PRESS },   /* H */
		{ RECON_KEY_CODE_A + 7,     RECON_INPUT_RELEASE },
		{ RECON_KEY_CODE_LEFTSHIFT, RECON_INPUT_RELEASE },
		{ RECON_KEY_CODE_A + 8,     RECON_INPUT_PRESS },   /* i */
		{ RECON_KEY_CODE_A + 8,     RECON_INPUT_RELEASE },
		{ RECON_KEY_CODE_SPACE,     RECON_INPUT_PRESS },
		{ RECON_KEY_CODE_SPACE,     RECON_INPUT_RELEASE },
		{ RECON_KEY_CODE_A + 19,    RECON_INPUT_PRESS },   /* t */
		{ RECON_KEY_CODE_A + 19,    RECON_INPUT_REPEAT },
		{ RECON_KEY_CODE_A + 19,    RECON_INPUT_RELEASE },
	};

	for (size_t i = 0; i < sizeof(TYPING) / sizeof(TYPING[0]); i++) {
		struct recon_input_event e = ev(TYPING[i].code, TYPING[i].kind);
		struct recon_keystroke stroke;

		if (recon_keyboard_event(&kb, &e, &stroke) && at + 1 < sizeof(out)) {
			uint32_t c = recon_key_to_char(stroke.symbol);

			if (c != 0 && c < 128) {
				out[at++] = (char)c;
			}
		}
	}
	out[at] = '\0';

	check(strcmp(out, "Hi tt") == 0, "typed 'Hi tt'");
	check(kb.modifiers == 0, "and nothing is left held afterwards");
}

/*
 * --- The two headers, held to each other ---
 *
 * `sys/input.h` writes down a byte layout the kernel also writes down, and a
 * `_Static_assert` can hold this side's copy without noticing the kernel
 * changing a number underneath it. So the kernel's header is read as text and
 * the constants compared -- the same arrangement `test_init_layout.c` uses for
 * the directory list, and for the same reason: a real duplication is a thing
 * to test rather than a thing to be careful about.
 */
static bool kernel_defines(const char *name, long want, long *got) {
	FILE *f = fopen("kernel/include/recon/kernel/input.h", "r");
	char line[256];
	bool found = false;

	*got = -1;
	if (f == NULL) {
		return false;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		char seen[64];
		long value;

		if (sscanf(line, " #define %63s %ld", seen, &value) == 2 &&
		    strcmp(seen, name) == 0) {
			*got = value;
			found = (value == want);
			break;
		}
	}
	fclose(f);
	return found;
}

static void test_the_headers_still_agree(void) {
	printf("the kernel's numbers and this program's are the same\n");

	static const struct { const char *name; long ours; } SAME[] = {
		{ "KEY_A",          RECON_KEY_CODE_A },
		{ "KEY_Z",          RECON_KEY_CODE_Z },
		{ "KEY_1",          RECON_KEY_CODE_1 },
		{ "KEY_0",          RECON_KEY_CODE_0 },
		{ "KEY_ENTER",      RECON_KEY_CODE_ENTER },
		{ "KEY_ESCAPE",     RECON_KEY_CODE_ESCAPE },
		{ "KEY_BACKSPACE",  RECON_KEY_CODE_BACKSPACE },
		{ "KEY_TAB",        RECON_KEY_CODE_TAB },
		{ "KEY_SPACE",      RECON_KEY_CODE_SPACE },
		{ "KEY_CAPSLOCK",   RECON_KEY_CODE_CAPSLOCK },
		{ "KEY_LEFTCTRL",   RECON_KEY_CODE_LEFTCTRL },
		{ "KEY_LEFTSHIFT",  RECON_KEY_CODE_LEFTSHIFT },
		{ "KEY_LEFTALT",    RECON_KEY_CODE_LEFTALT },
		{ "KEY_LEFTMETA",   RECON_KEY_CODE_LEFTMETA },
		{ "KEY_RIGHTSHIFT", RECON_KEY_CODE_RIGHTSHIFT },
		{ "KEY_MAX",        RECON_KEY_CODE_MAX },
		{ "INPUT_QUEUE",    200 },
	};

	for (size_t i = 0; i < sizeof(SAME) / sizeof(SAME[0]); i++) {
		char what[160];
		long got = -1;
		bool agrees = kernel_defines(SAME[i].name, SAME[i].ours, &got);

		snprintf(what, sizeof(what),
			 "%s: the kernel says %ld, this program says %ld",
			 SAME[i].name, got, SAME[i].ours);
		check(agrees, what);
	}

	/*
	 * And that the kernel has not renamed the thing this is reading. A
	 * lookup that finds nothing and a lookup that finds a match are told
	 * apart by asking for a name that should not be there.
	 */
	long got = 0;
	check(!kernel_defines("KEY_NOT_A_REAL_NAME", 0, &got),
	      "a name the kernel does not define is not reported as agreeing");
	check(got == -1, "and comes back as not found rather than as zero");
}

int main(void) {
	printf("ReconOS desktop keyboard tests\n\n");

	test_a_letter();
	test_shift_is_held_across_events();
	test_a_release_nobody_saw_a_press_for();
	test_a_repeat_is_not_a_press();
	test_caps_lock_is_a_latch();
	test_shift_and_caps_together();
	test_the_modifiers_beside_a_symbol_are_the_ones_it_was_typed_under();
	test_a_malformed_event_is_refused();
	test_a_mouse_is_not_a_keyboard();
	test_a_key_with_no_symbol_is_counted();
	test_a_sentence();
	test_the_headers_still_agree();

	printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
