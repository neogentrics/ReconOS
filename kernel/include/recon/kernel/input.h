/* Somebody touching the machine.
 *
 * Every screen this kernel has ever drawn has been read and not touched. There
 * is no keyboard, no mouse, nothing that turns a keypress into anything -- which
 * the audit names as the single thing with nothing above it and nothing blocking
 * it, and the checkpoint board has carried as open since the list was written.
 *
 * --- An event is a key, not a character ---
 *
 * This is the decision the whole file turns on, and getting it the other way
 * round is how a kernel ends up owning keyboard layouts.
 *
 * The hardware says "the key in position 30 went down". What character that is
 * depends on the layout, the modifiers, the dead-key state, and in some
 * languages on the two keys before it. **All of that is data, and none of it is
 * the kernel's.** A kernel that hands up characters has to contain a layout
 * table, and then a second one, and then a way to choose -- and a program that
 * wants the *position* (a game reading WASD, which is ZQSD on a French
 * keyboard) can never get it back, because the information was thrown away
 * below.
 *
 * So an event carries a **keycode**: a number naming a physical position, the
 * same number for that position on every keyboard, independent of what is
 * printed on the key. Turning that into text is a job for whatever knows which
 * layout the person chose, and that is not down here.
 *
 * --- Press, release, and the third one ---
 *
 * A key that is held down repeats, because the keyboard's own typematic circuit
 * sends the make code again. Those repeats are reported as `INPUT_REPEAT` rather
 * than as fresh presses.
 *
 * The distinction is not cosmetic. A text field wants repeats -- holding
 * backspace should delete more than one character. A game does not: a repeat is
 * not a new jump. Collapsing them into "pressed" makes the first impossible to
 * tell from the second, and every program that cares has to reconstruct it from
 * timing, badly.
 *
 * --- The queue, and what happens when nobody is reading ---
 *
 * A fixed ring. If it fills, **the oldest event is dropped and the loss is
 * counted**, because the alternative -- refusing new events -- means the most
 * recent thing the person did is the thing that gets lost, and a machine that
 * ignores you *because* you are typing is worse than one that forgets what you
 * typed a while ago.
 *
 * Either way something is wrong: the queue holds several seconds of the fastest
 * human typing, so filling it means nothing is reading at all. The count is
 * printed rather than hidden, since "input feels like it is dropping keys" is
 * otherwise an unfalsifiable complaint.
 *
 * --- A release with no press ---
 *
 * A reader can see one, and must not be broken by it. After a dropped event, or
 * a program that started while a key was already held, the release arrives for a
 * press nobody saw. So modifier state is *derived* from events rather than
 * counted with them: a release of a key that was not held is a no-op, never a
 * decrement. A counter here goes negative, wraps, and leaves the machine
 * believing shift is held for ever.
 *
 * --- Where the work happens ---
 *
 * The interrupt handler does one thing: takes the byte the hardware will lose if
 * nobody takes it. The 8042 holds exactly one, and the next keypress overwrites
 * it. Everything after that -- the scancode state machine, the queue, waking a
 * reader -- happens in the deferred worker, which is what deferred work was
 * built for and the first thing to use it in anger.
 */
#ifndef RECON_KERNEL_INPUT_H
#define RECON_KERNEL_INPUT_H

#include <recon/kernel/types.h>
#include <recon/kernel/vfs.h>

/* What kind of thing happened. Deliberately small: this is not an extensible
 * protocol, it is the set of things the machine can currently tell. */
enum input_kind {
	INPUT_RELEASE = 0,
	INPUT_PRESS   = 1,
	INPUT_REPEAT  = 2,

	/* Something moved. `code` names an axis and `value` says how far, as a
	 * signed count in whatever unit the device works in -- which for a
	 * mouse is a "mickey" and is not a pixel, because how far a pixel is
	 * depends on a screen this layer has never seen. */
	INPUT_MOTION  = 3,
};

/* Keycodes name positions, not letters.
 *
 * The numbers follow the USB HID usage table for the keyboard page, which is not
 * arbitrary and not a preference: USB HID is what every keyboard made this
 * century actually reports, so a USB driver will hand these up with no
 * translation at all. It is the PS/2 driver -- the older, stranger one -- that
 * does the converting, which is the right way round.
 *
 * Only the ones the PS/2 driver can produce today are named. A keycode with no
 * name here is still delivered; it is a number this file has not got round to.
 */
#define KEY_A            4
#define KEY_Z            29
#define KEY_1            30
#define KEY_0            39
#define KEY_ENTER        40
#define KEY_ESCAPE       41
#define KEY_BACKSPACE    42
#define KEY_TAB          43
#define KEY_SPACE        44
#define KEY_MINUS        45
#define KEY_EQUAL        46
#define KEY_CAPSLOCK     57
#define KEY_F1           58
#define KEY_F12          69
#define KEY_RIGHT        79
#define KEY_LEFT         80
#define KEY_DOWN         81
#define KEY_UP           82
#define KEY_LEFTCTRL     224
#define KEY_LEFTSHIFT    225
#define KEY_LEFTALT      226
#define KEY_LEFTMETA     227
#define KEY_RIGHTCTRL    228
#define KEY_RIGHTSHIFT   229
#define KEY_RIGHTALT     230
#define KEY_RIGHTMETA    231

#define KEY_MAX          255

/* Mouse buttons.
 *
 * Above the keyboard page rather than inside it, because they are a different
 * HID usage page and folding them together would mean a keycode and a button
 * could collide as the tables grow. The numbers follow the same convention
 * Linux uses, for the same reason the keycodes follow HID: somebody porting a
 * program should not have to learn a third numbering.
 *
 * They are *held* like keys are, and reported with the same press and release
 * kinds -- a button is a key that happens to be under your hand. */
#define BTN_LEFT         0x110
#define BTN_RIGHT        0x111
#define BTN_MIDDLE       0x112

/* The largest code whose held-state is tracked. Axes are above it and are not
 * held: an axis has no up or down, only a distance since last time. */
#define INPUT_HELD_MAX   BTN_MIDDLE

/* Axes, for INPUT_MOTION.
 *
 * Relative, always. X and Y are *deltas* and not positions, because a mouse
 * reports movement and has no idea where the pointer is -- whoever draws the
 * pointer owns that, and it is the only thing that knows where the edges of
 * the screen are.
 *
 * **Y increases downward**, on every device, and that is a decision rather
 * than a passthrough: a PS/2 mouse says positive is up and a USB HID mouse
 * says positive is down. Reporting each as it arrives would mean every reader
 * had to know which kind of mouse was attached, which is the one thing this
 * layer exists to prevent. The drivers normalise; nothing above them asks. */
#define REL_X            0x200
#define REL_Y            0x201
#define REL_WHEEL        0x202

struct input_event {
	u64 when;	/* monotonic nanoseconds, taken when the byte arrived */

	/* How far, for a motion event. Zero for a key or a button, and checked
	 * to be zero -- a caller that reads `value` on a press should get
	 * nothing rather than whatever was left in the field. */
	i32 value;

	u16 code;	/* a keycode, a button, or an axis */
	u8  kind;	/* enum input_kind */
	u8  reserved;	/* zero, and checked to be zero, so that adding a field
			 * later cannot be mistaken for a value somebody set */
};

/* How many events are held. Two hundred is several seconds of the fastest human
 * typing -- so a full queue means nothing is reading, not that somebody typed
 * quickly. A visible number, because the only interesting thing about it is
 * whether it was ever reached. */
#define INPUT_QUEUE 200

/* Called by a driver, from the deferred worker rather than from the interrupt.
 *
 * `code` is a keycode and not a scancode: the translation belongs in the driver,
 * because the whole point of this line is that nothing above it knows what kind
 * of keyboard is attached. */
void input_post(u16 code, enum input_kind kind);

/* Something moved, by `delta`, along `axis`.
 *
 * **Consecutive motion on the same axis is added to the event already
 * waiting**, rather than appended as a second one. That is safe here and would
 * not be for anything else on this page: a relative distance is a number you
 * may sum, and a keypress is not. A mouse dragged across a desk produces
 * hundreds of packets a second, and without this the queue fills with them and
 * throws away the keystrokes sitting behind.
 *
 * A delta of zero is dropped rather than queued. Some mice report one on every
 * poll whether or not the hand moved. */
void input_post_motion(u16 axis, i32 delta);

/* Takes the oldest event. False when there is none -- this does not wait, and a
 * caller that wants to wait uses the descriptor below, where waiting is the
 * file's business rather than every caller's. */
bool input_take(struct input_event *out);

/* Whether a key is held, derived from the events rather than counted alongside
 * them. See the note above about a release with no press. */
bool input_key_held(u16 code);

/* The operations behind /dev/input, named in devfs's table beside the others.
 * Exported rather than reached through a function, because devfs holds a table
 * of pointers and a function would be the only entry that was different. */
extern const struct file_ops input_file_ops;

/* The file behind /dev/input. Reads whole events; a partial read would hand back
 * half of one and leave the reader unable to say which half. */
struct file *input_open(unsigned flags, i64 *error);

void input_init(void);
void input_print_summary(void);
bool input_self_test(void);

/* --- what an architecture provides ----------------------------------------
 *
 * A no-op where there is no such hardware, which is every aarch64 machine: the
 * 8042 is a chip from 1984 that lives on the ISA bus, and ARM machines have
 * neither. Input there comes from USB HID, which is checkpoint 11b's stack plus
 * a driver, and is a separate piece of work rather than a variation on this one.
 */
void arch_input_probe(void);
void arch_input_print(void);

#endif /* RECON_KERNEL_INPUT_H */
