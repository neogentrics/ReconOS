/*
 * <sys/input.h> -- somebody touching the machine.
 *
 * The events `/dev/input` hands to a program, as a program sees them. This
 * mirrors `kernel/include/recon/kernel/input.h`, and the mirroring is the
 * hazard the whole file is arranged around.
 *
 * --- Why the struct is duplicated, and how that is made safe ---
 *
 * The two headers cannot be one file: the kernel's includes the kernel's own
 * types and its VFS, neither of which exists in a program. So the layout is
 * written down twice, and **two copies of a byte layout that must agree is
 * exactly the kind of duplication that is wrong six months later and reports
 * nothing when it is.**
 *
 * A struct read with the wrong layout does not fail. It returns numbers. A
 * `code` read at the wrong offset is a different key, and a `kind` read at the
 * wrong offset is a press that looks like a release -- which is a keyboard that
 * types the wrong letters and holds Shift down for ever. There is no crash and
 * no error code anywhere in that.
 *
 * So: every offset and the total size are held by `_Static_assert` below, which
 * makes a disagreement a **compile error in this file**; and
 * `userland/tests/test_desktop_keyboard.c` reads the kernel's header as text
 * and requires the constants to match, which catches the other direction --
 * the kernel changing a number that this file then quietly disagrees with.
 * That is the arrangement `userland/init/layout.c` and `include/recon_fs.h`
 * already use for the directory list, and for the same reason.
 *
 * --- An event is a key, not a character ---
 *
 * The kernel's header argues this at length and it is worth restating here,
 * because this is the side that has to do something about it: the hardware
 * says *"the key in position 30 went down"*. What character that is depends on
 * the layout, the modifiers and the dead-key state, and **none of that is the
 * kernel's business**.
 *
 * `include/recon_key.h` is where the position becomes a meaning on this side.
 */

#ifndef RECON_SYS_INPUT_H
#define RECON_SYS_INPUT_H

/*
 * Quoted, so it finds the one beside it rather than the host's -- the same
 * exception `libc/posix.c` takes and for the same reason.
 *
 * `recon.h` and not `sys/types.h`, which is the sibling a file in this
 * directory would reach for by habit. Two reasons, and the second is the one
 * that decides it: the widths this needs (`u16`, `i32`) are spelled in
 * `recon.h` with the rest of the kernel's names for them, and nothing here
 * needs a POSIX type at all -- while `types.h` defines `off_t`, which collides
 * with the host's the moment anything includes both. A test that drives this
 * struct is exactly such a thing.
 */
#include "../recon.h"

/*
 * What kind of thing happened.
 *
 * `INPUT_REPEAT` is not folded into `INPUT_PRESS`, and the distinction is not
 * cosmetic: a text field wants repeats -- holding backspace should delete more
 * than one character -- and a game does not, because a repeat is not a new
 * jump. Collapsing them makes the first impossible to tell from the second,
 * and every program that cares has to reconstruct it from timing, badly.
 */
enum recon_input_kind {
	RECON_INPUT_RELEASE = 0,
	RECON_INPUT_PRESS   = 1,
	RECON_INPUT_REPEAT  = 2,
	RECON_INPUT_MOTION  = 3,
};

/*
 * One event, exactly as it arrives.
 *
 * Sixteen bytes with no padding, which is why the fields are in this order and
 * not in the order somebody would write them for reading: the u64 first, then
 * the i32, then the two-byte and the two one-byte fields. Rearranging them for
 * tidiness inserts padding and silently stops this matching the kernel.
 */
struct recon_input_event {
	u64 when;	/* monotonic nanoseconds, when the byte arrived */

	/*
	 * How far, for a motion event. Zero for a key or a button -- and the
	 * kernel checks that it is zero, so a program reading `value` on a
	 * press gets nothing rather than whatever was last in the field.
	 */
	i32 value;

	u16 code;	/* a keycode, a button, or an axis */
	u8  kind;	/* enum recon_input_kind */

	/*
	 * Zero, and checked to be zero on both sides, so that a field added
	 * here later cannot be mistaken for a value somebody set.
	 */
	u8  reserved;
};

/*
 * The layout, held rather than hoped for. A mismatch is a compile error here
 * instead of a keyboard that types the wrong letters somewhere else.
 */
_Static_assert(sizeof(struct recon_input_event) == 16,
	       "an input event is sixteen bytes; the kernel writes sixteen");
_Static_assert(__builtin_offsetof(struct recon_input_event, when) == 0,
	       "when is first");
_Static_assert(__builtin_offsetof(struct recon_input_event, value) == 8,
	       "value follows when");
_Static_assert(__builtin_offsetof(struct recon_input_event, code) == 12,
	       "code follows value");
_Static_assert(__builtin_offsetof(struct recon_input_event, kind) == 14,
	       "kind follows code");
_Static_assert(__builtin_offsetof(struct recon_input_event, reserved) == 15,
	       "reserved is last");

/*
 * Keycodes name positions, not letters, and the numbers are the USB HID usage
 * table's for the keyboard page.
 *
 * That is not a preference. USB HID is what every keyboard made this century
 * reports, so a USB driver hands these up with no translation at all -- and it
 * is the PS/2 driver, the older and stranger one, that does the converting.
 * Which is the right way round.
 *
 * Only the ones a driver can produce today are named. A keycode with no name
 * here is still delivered; it is a number nobody has got round to.
 */
#define RECON_KEY_CODE_A            4
#define RECON_KEY_CODE_Z            29
#define RECON_KEY_CODE_1            30
#define RECON_KEY_CODE_0            39
#define RECON_KEY_CODE_ENTER        40
#define RECON_KEY_CODE_ESCAPE       41
#define RECON_KEY_CODE_BACKSPACE    42
#define RECON_KEY_CODE_TAB          43
#define RECON_KEY_CODE_SPACE        44
#define RECON_KEY_CODE_MINUS        45
#define RECON_KEY_CODE_EQUAL        46
#define RECON_KEY_CODE_CAPSLOCK     57
#define RECON_KEY_CODE_F1           58
#define RECON_KEY_CODE_F12          69
#define RECON_KEY_CODE_RIGHT        79
#define RECON_KEY_CODE_LEFT         80
#define RECON_KEY_CODE_DOWN         81
#define RECON_KEY_CODE_UP           82
#define RECON_KEY_CODE_LEFTCTRL     224
#define RECON_KEY_CODE_LEFTSHIFT    225
#define RECON_KEY_CODE_LEFTALT      226
#define RECON_KEY_CODE_LEFTMETA     227
#define RECON_KEY_CODE_RIGHTCTRL    228
#define RECON_KEY_CODE_RIGHTSHIFT   229
#define RECON_KEY_CODE_RIGHTALT     230
#define RECON_KEY_CODE_RIGHTMETA    231
#define RECON_KEY_CODE_MAX          255

/*
 * Mouse buttons, above the keyboard page rather than inside it: they are a
 * different HID usage page, and folding them together would let a keycode and
 * a button collide as the tables grow.
 */
#define RECON_BTN_LEFT              0x110
#define RECON_BTN_RIGHT             0x111
#define RECON_BTN_MIDDLE            0x112

/*
 * Axes, for a motion event. Relative, always: a mouse reports movement and has
 * no idea where the pointer is -- whoever draws the pointer owns that, being
 * the only thing that knows where the edges of the screen are.
 *
 * **Y increases downward on every device**, which the drivers normalise: a
 * PS/2 mouse says positive is up and a USB HID mouse says positive is down,
 * and reporting each as it arrives would mean every reader had to know which
 * kind of mouse was attached.
 */
#define RECON_REL_X                 0x200
#define RECON_REL_Y                 0x201
#define RECON_REL_WHEEL             0x202

#endif /* RECON_SYS_INPUT_H */
