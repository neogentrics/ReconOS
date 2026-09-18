/*
 * The desktop program, run.
 *
 * --- What changed to make this possible ---
 *
 * Until v0.4.56 this file could not exist. `main.c` reached the kernel through
 * `recon_screen` and `recon_map`, which are `static inline` in `<recon.h>` and
 * expand to a raw `syscall` instruction -- so there was no function for a shim
 * to replace, and **the one file that decides between a desktop and a black
 * screen was the only one in its directory that nothing could execute.**
 *
 * The calls are behind `machine.h` now. This supplies a machine made of
 * `malloc` and a table of keystrokes, and runs the real program against it.
 *
 * --- What is worth checking ---
 *
 * Not the drawing: `test_desktop_frame.c` holds that, and holds it against
 * properties a picture cannot show. What is here is everything `main.c`
 * decides, which is all of it about **what happens when something is wrong**:
 *
 *   - **eight ways to fail, and each says which.** An exit code is the only
 *     thing a program that cannot draw can still say, and a wrong one sends
 *     somebody looking at the wrong half of the machine.
 *   - **the frame is drawn before the keyboard is opened.** A machine with
 *     nothing plugged in should show a desktop and then say so; the other
 *     order gives a black screen for a fault that is not the desktop's.
 *   - **a short read is refused rather than parsed.** Half an event read as a
 *     whole one is a keypress that never happened.
 *   - **a machine that cannot describe itself still gets a desktop.** The one
 *     outcome nobody standing in front of a machine can diagnose is a black
 *     screen, and refusing to draw over a processor count would produce one.
 *
 * Run with: ./build/recon_desktop_main_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * By relative path, and recon.h first.
 *
 * `recon_machine.h` includes <recon.h> with angle brackets, which would need
 * `userland/include` on this file's path -- and that would hand it ReconOS's
 * <stdio.h>, which deliberately has no `printf`. Including recon.h first by
 * relative path defines its guard, so the angle-bracketed one inside
 * recon_machine.h is skipped before the preprocessor goes looking for it.
 */
#include "../include/recon.h"
#include "../include/recon_machine.h"

#include "recon_theme.h"
#include "recon_ui.h"

#include "../include/sys/input.h"

#include "machine.h"
#include "shell_frame.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
	g_checks++;
	if (!condition) {
		g_failures++;
		printf("  FAIL: %s\n", what);
	}
}

/* --- a machine made of malloc ------------------------------------------- */

#define FAKE_WIDTH   800
#define FAKE_HEIGHT  600
#define FAKE_SLACK   64
#define PAD_BYTE     0xAB

#define FB_FD        7
#define INPUT_FD     8

static struct {
	unsigned char *pixels;
	size_t pitch;

	/* What the fake answers, so a test can break one thing at a time. */
	long long screen_answer;     /* what `screen` returns */
	unsigned screen_width;
	unsigned screen_height;
	unsigned screen_pitch;
	bool fb_opens;
	bool input_opens;
	bool map_works;
	bool facts_work;
	bool machine_has_facts;      /* whether the vtable has a facts pointer */

	/* The keystrokes to deliver, and where we are in them. */
	const struct recon_input_event *events;
	size_t event_count;
	size_t event_at;

	/* Deliver a length that is not a whole number of events. */
	bool short_read;

	int reads;                   /* how many times `read` was called */
	int carry_on_left;           /* how many more loops before stopping */
	bool input_opened_after_draw;
} fake;

static long long fake_screen(struct recon_screen *into, unsigned long long size)
{
	if (fake.screen_answer < 0) {
		return fake.screen_answer;
	}
	memset(into, 0, (size_t)size);
	into->width = fake.screen_width;
	into->height = fake.screen_height;
	into->pitch = fake.screen_pitch;
	into->bytes = (u64)fake.screen_pitch * fake.screen_height;
	return fake.screen_answer;
}

/* How many pixels of the fake screen have been written at all. */
static int pixels_written(void)
{
	int n = 0;

	for (int y = 0; y < FAKE_HEIGHT; y++) {
		const uint32_t *row = (const uint32_t *)(fake.pixels +
			(size_t)y * fake.pitch);

		for (int x = 0; x < FAKE_WIDTH; x++) {
			if (row[x] != 0xABABABABu) {
				n++;
			}
		}
	}
	return n;
}

static long long fake_open(const char *path, unsigned long long flags)
{
	(void)flags;
	if (strcmp(path, "/dev/fb0") == 0) {
		return fake.fb_opens ? FB_FD : -1;
	}
	if (strcmp(path, "/dev/input") == 0) {
		/*
		 * The order check, and the reason this fake exists at all: a
		 * desktop that opens the keyboard before it draws shows nothing
		 * on a machine with nothing plugged in.
		 *
		 * Measured by looking at the framebuffer rather than by a flag
		 * set somewhere else. The first version of this used a flag set
		 * in `carry_on`, which is not called until the loop starts --
		 * so it read false however early the drawing happened, and
		 * failed a program that was doing the right thing. **A proxy
		 * for the thing is not the thing**; the pixels are.
		 */
		fake.input_opened_after_draw = (pixels_written() > 0);
		return fake.input_opens ? INPUT_FD : -1;
	}
	return -1;
}

static long long fake_map(int fd, unsigned long long length)
{
	(void)fd;
	(void)length;
	return fake.map_works ? (long long)(unsigned long)fake.pixels : -1;
}

static long long fake_read(int fd, void *into, unsigned long long length)
{
	(void)fd;
	fake.reads++;

	if (fake.event_at >= fake.event_count) {
		return 0;
	}

	size_t one = sizeof(struct recon_input_event);
	size_t room = (size_t)length / one;
	size_t left = fake.event_count - fake.event_at;
	size_t give = left < room ? left : room;

	memcpy(into, fake.events + fake.event_at, give * one);
	fake.event_at += give;

	/* A length that is not a whole number of events, which is what a
	 * layout disagreement would look like from here. */
	return fake.short_read ? (long long)(give * one - 1)
			       : (long long)(give * one);
}

static long long fake_facts(struct recon_machine *into,
			    unsigned long long size)
{
	if (!fake.facts_work) {
		return -1;
	}
	memset(into, 0, (size_t)size);
	into->size = (u32)size;
	into->processors_found = 8;
	into->processors_online = 4;
	into->memory_bytes = (u64)16 * 1024 * 1024 * 1024;
	snprintf(into->architecture, sizeof(into->architecture), "x86_64");
	return (long long)size;
}

static int fake_carry_on(void)
{
	/*
	 * The hook that makes this runnable. The real machine always says yes;
	 * this one counts down, and the count is what lets a test watch a loop
	 * that is designed never to end.
	 */
	return fake.carry_on_left-- > 0;
}

static struct recon_desktop_machine machine_of(void)
{
	struct recon_desktop_machine m;

	memset(&m, 0, sizeof(m));
	m.screen = fake_screen;
	m.open = fake_open;
	m.map = fake_map;
	m.read = fake_read;
	m.facts = fake.machine_has_facts ? fake_facts : NULL;
	m.carry_on = fake_carry_on;
	return m;
}

/* Everything working, before a test breaks one thing. */
static void fake_reset(void)
{
	free(fake.pixels);
	memset(&fake, 0, sizeof(fake));

	fake.pitch = (size_t)FAKE_WIDTH * 4u + FAKE_SLACK;
	fake.pixels = malloc(fake.pitch * FAKE_HEIGHT);
	memset(fake.pixels, PAD_BYTE, fake.pitch * FAKE_HEIGHT);

	fake.screen_answer = (long long)sizeof(struct recon_screen);
	fake.screen_width = FAKE_WIDTH;
	fake.screen_height = FAKE_HEIGHT;
	fake.screen_pitch = (unsigned)fake.pitch;
	fake.fb_opens = true;
	fake.input_opens = true;
	fake.map_works = true;
	fake.facts_work = true;
	fake.machine_has_facts = true;
	fake.carry_on_left = 4;
}

static int run(void)
{
	struct recon_desktop_machine m = machine_of();

	return recon_desktop_run(&m);
}

/* --- the tests ----------------------------------------------------------- */

static void test_it_runs(void)
{
	printf("the whole program runs and draws\n");

	fake_reset();
	check(run() == 0, "it runs to the end and reports no failure");
	check(pixels_written() == FAKE_WIDTH * FAKE_HEIGHT,
	      "and every pixel of the screen was written");
	check(fake.reads > 0, "and it read the keyboard");
}

static void test_every_way_it_can_fail(void)
{
	printf("each way of failing says which one it was\n");

	/*
	 * An exit code is the only thing a program that cannot draw can still
	 * say, and the kernel prints it. A wrong one sends somebody looking at
	 * the wrong half of the machine -- which on a system with no console is
	 * the difference between a fixed fault and an afternoon.
	 */
	fake_reset();
	fake.screen_answer = -1;
	check(run() == 20, "the screen would not describe itself: 20");

	fake_reset();
	fake.screen_answer = (long long)sizeof(struct recon_screen) - 1;
	check(run() == 21, "the description is a different size: 21");

	fake_reset();
	fake.screen_width = 0;
	check(run() == 22, "a screen with no width: 22");

	fake_reset();
	fake.screen_pitch = FAKE_WIDTH * 4u - 1;
	check(run() == 22, "and a pitch under a row: 22");

	fake_reset();
	fake.fb_opens = false;
	check(run() == 23, "the framebuffer will not open: 23");

	fake_reset();
	fake.map_works = false;
	check(run() == 24, "the mapping is refused: 24");

	check(recon_desktop_run(NULL) == 27, "and no machine at all: 27");

	/*
	 * 26, and it is the one to get right.
	 *
	 * The desktop opens its font with `fopen` down a hardcoded list in
	 * `src/recon_ui.c` whose first entry is `/System/Fonts/Sans.ttf`, and
	 * `RECONOS_FONT` overrides the lot. Pointing it at something that is
	 * not a font is the only way to make the search fail on a build machine
	 * that has DejaVu installed -- and it is the same failure a ReconOS
	 * volume with no font on it produces, reached from the other side.
	 *
	 * **This is the code I have told the kernel session to expect on the
	 * first boot**, because the medium writes its fonts to the ESP at
	 * `/reconos/fonts/` and nothing copies them onto the volume. A number I
	 * have put in somebody else's plan should be a number something runs.
	 */
	fake_reset();
	setenv("RECONOS_FONT", "/dev/null", 1);

	/*
	 * The cache emptied first, and that is the finding rather than a
	 * detail. `recon_font_system` keeps every size it has ever loaded for
	 * the life of the process -- so the first version of this check passed
	 * a bad font in and got back the one `test_it_runs` had already cached
	 * at size 14, and reported that the desktop drew fine with no font.
	 *
	 * On a real boot the process is new and the cache is empty, so the
	 * check has to start there to be about the same thing.
	 */
	recon_font_system_finish();
	check(run() == 26, "no font it can read: 26");
	unsetenv("RECONOS_FONT");
	recon_font_system_finish();

	/*
	 * --- 25 is not checked, and finding out why was worth the attempt ---
	 *
	 * `recon_panel_on_screen` returns NULL four ways. Three of them --
	 * a NULL pointer, a width or height of zero, a pitch under a row --
	 * are all caught *upstream* by the checks that produce 22 and 24, so
	 * the desktop can never reach the panel with any of them. **The fourth
	 * is `calloc` failing**, and that is the only way to 25.
	 *
	 * Asking for it means a screen too big to allocate a panel for, and a
	 * screen that big makes the sanitizers abort on the allocation rather
	 * than let it fail -- correctly: an allocation of four terabytes is a
	 * fault whatever the caller meant by it. Tuning the number until ASan
	 * tolerated it would make this a test about ASan's threshold.
	 *
	 * So 25 stays unexercised, and what is written down instead is that it
	 * is unreachable except under memory exhaustion. That is a fact about
	 * the code rather than a gap in this file, and it is the kind of thing
	 * somebody reading the exit table should know before spending a boot
	 * looking for it.
	 */

	/*
	 * A table with a hole in it. Every entry is called, so a NULL is a
	 * crash rather than a failure -- which is why it is checked once at
	 * the top rather than at each call site.
	 */
	struct recon_desktop_machine holed;

	fake_reset();
	holed = machine_of();
	holed.map = NULL;
	check(recon_desktop_run(&holed) == 27, "a table missing a call: 27");
}

static void test_a_machine_that_says_nothing_still_draws(void)
{
	printf("a machine that cannot describe itself still gets a desktop\n");

	/*
	 * The outcome this rules out is a black screen. Everything the screen
	 * needs has already succeeded by this point; the processor count is
	 * decoration, and a desktop that refuses to appear over decoration is
	 * the one failure nobody standing in front of the machine can tell
	 * from a kernel that started nothing.
	 */
	fake_reset();
	fake.facts_work = false;
	check(run() == 0, "it still runs");
	check(pixels_written() == FAKE_WIDTH * FAKE_HEIGHT, "and still draws");

	/* And with no facts call in the table at all, which is a different
	 * thing from one that fails. */
	fake_reset();
	fake.machine_has_facts = false;
	check(run() == 0, "with no way to ask at all, it still runs");
	check(pixels_written() == FAKE_WIDTH * FAKE_HEIGHT, "and still draws");
}

static void test_no_keyboard_still_draws(void)
{
	printf("no keyboard is reported on a screen that already exists\n");

	fake_reset();
	fake.input_opens = false;
	check(run() == 0, "it runs");
	check(pixels_written() == FAKE_WIDTH * FAKE_HEIGHT, "and draws");

	/*
	 * And the order, which is the point. The frame is on the screen before
	 * `/dev/input` is asked for, so a machine with nothing plugged in shows
	 * a desktop and then says so. Opening first would give a black screen
	 * for a fault that is not the desktop's.
	 */
	check(fake.input_opened_after_draw,
	      "and the keyboard was opened after the first frame, not before");
}

static void test_typing_reaches_the_screen(void)
{
	printf("a keystroke changes what is on the screen\n");

	static const struct recon_input_event NOTHING[1] = { { 0, 0, 0, 0, 0 } };
	struct recon_input_event typing[6];
	unsigned char *quiet;
	size_t bytes;

	/* First, a run with no keys at all, kept for comparison. */
	fake_reset();
	fake.events = NOTHING;
	fake.event_count = 0;
	(void)run();

	bytes = fake.pitch * FAKE_HEIGHT;
	quiet = malloc(bytes);
	memcpy(quiet, fake.pixels, bytes);

	/* Then the same run with somebody typing into it. */
	memset(typing, 0, sizeof(typing));
	typing[0].code = RECON_KEY_CODE_LEFTSHIFT;
	typing[0].kind = RECON_INPUT_PRESS;
	typing[1].code = RECON_KEY_CODE_A + 7;   /* H */
	typing[1].kind = RECON_INPUT_PRESS;
	typing[2].code = RECON_KEY_CODE_A + 7;
	typing[2].kind = RECON_INPUT_RELEASE;
	typing[3].code = RECON_KEY_CODE_LEFTSHIFT;
	typing[3].kind = RECON_INPUT_RELEASE;
	typing[4].code = RECON_KEY_CODE_A + 8;   /* i */
	typing[4].kind = RECON_INPUT_PRESS;
	typing[5].code = RECON_KEY_CODE_A + 8;
	typing[5].kind = RECON_INPUT_RELEASE;

	fake_reset();
	fake.events = typing;
	fake.event_count = sizeof(typing) / sizeof(typing[0]);
	check(run() == 0, "it runs");

	/*
	 * The screen differs. Which pixels changed is `test_desktop_frame.c`'s
	 * business; what this holds is that the path from a keycode to a
	 * screen is joined up at all -- and it is the only test in the tree
	 * that runs the whole of it.
	 */
	check(memcmp(quiet, fake.pixels, bytes) != 0,
	      "and typing changed what is on the screen");

	free(quiet);
}

static void test_a_short_read_is_refused(void)
{
	printf("a length that is not a whole number of events is refused\n");

	/*
	 * `/dev/input` reads whole events, so this cannot happen unless this
	 * program and the kernel disagree about how big one is -- which is the
	 * hazard `sys/input.h` is arranged around, and this is the only place
	 * it can be noticed while running.
	 *
	 * Half an event read as a whole one is a keypress that never happened,
	 * at a keycode nobody pressed. Dropped instead.
	 */
	struct recon_input_event typing[2];

	memset(typing, 0, sizeof(typing));
	typing[0].code = RECON_KEY_CODE_A;
	typing[0].kind = RECON_INPUT_PRESS;
	typing[1].code = RECON_KEY_CODE_A;
	typing[1].kind = RECON_INPUT_RELEASE;

	fake_reset();
	fake.events = typing;
	fake.event_count = 2;
	fake.short_read = true;

	check(run() == 0, "it keeps running rather than giving up");
	check(pixels_written() == FAKE_WIDTH * FAKE_HEIGHT,
	      "and the screen is still whole");
}

int main(void)
{
	printf("ReconOS desktop program tests\n\n");

	recon_theme_init();

	test_it_runs();
	test_every_way_it_can_fail();
	test_a_machine_that_says_nothing_still_draws();
	test_no_keyboard_still_draws();
	test_typing_reaches_the_screen();
	test_a_short_read_is_refused();

	free(fake.pixels);

	printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
