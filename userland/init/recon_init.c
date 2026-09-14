/*
 * The first program ReconOS runs that is a system rather than a self-test.
 *
 * Until now every program that had ever run in ring 3 on this kernel existed
 * to prove something about the kernel: `hello.S` proved the loader,
 * `paint.c` proved that C compiled against these headers runs at all and that
 * a framebuffer is written through its pitch. Both end by exiting with a code
 * the kernel checks.
 *
 * This one is different in the way that matters: **it is what somebody sees.**
 * It asks the machine what it is, asks the screen how it is arranged, asks the
 * volume what is on it, and draws a screen a person standing in front of a
 * computer that has just started can read. Then it stays up.
 *
 * --- What it is built out of ---
 *
 * Every string on that screen goes through `snprintf`, every comparison
 * through `strcmp`, every number out of text through `strtoul` -- which is to
 * say **it is the first real customer of `userland/libc/`**, and the first
 * proof that the library is a library rather than a suite of functions that
 * pass a suite of tests.
 *
 * It allocates nothing. There is no allocator on this kernel and that is the
 * first entry in `docs/KERNEL-WANTS.md`; everything here is a fixed buffer
 * sized at compile time, which is the same discipline `libc/stdio.c` follows
 * for the same reason.
 *
 * --- Where the drawing is ---
 *
 * Not here. `screen.c` takes a buffer and some facts and knows nothing about
 * system calls, so the host can render exactly this picture into memory and
 * the suite can check it. Finding out what the first screen ReconOS ever draws
 * looks like should not require a stick, a machine and a reboot.
 */

#include <recon.h>
#include <recon_machine.h>

#include <stdio.h>
#include <string.h>

#include "screen.h"

/*
 * Exit codes, distinct on purpose.
 *
 * A program that fails before it can draw cannot say why on the screen, so it
 * says why in the one number it can still return. The kernel prints it. This
 * is the same arrangement `paint.c` uses and the reason it can be debugged at
 * all on a machine with no console.
 */
#define WENT_WELL            0
#define NO_SCREEN_DESCRIBED  60
#define SCREEN_STRUCT_WRONG  61
#define FRAMEBUFFER_CLOSED   62
#define MAPPING_REFUSED      63
#define SCREEN_MAKES_NO_SENSE 64
#define MACHINE_UNREADABLE   65

/*
 * Say something on the serial line.
 *
 * The screen is what this program is for, and the screen is exactly what
 * cannot be read when the drawing is wrong. `SYS_WRITE` on descriptor 1
 * reaches the kernel's console, which reaches the serial port, which reaches
 * a file on the machine running the emulator -- so a program that has drawn a
 * rectangle in the wrong place can still say what numbers it drew it from.
 */
static void say(const char *text)
{
	recon_write(1, text, recon_strlen(text));
}

/* --- Formatting facts a person can read --- */

/*
 * Bytes as a person says them.
 *
 * Written out rather than reached for because there is no `%.1f` shortcut
 * that avoids it: the whole number and the tenth are computed separately in
 * integers, so a machine with no floating-point unit enabled in user mode --
 * which is a configuration this kernel allows -- prints the same string.
 */
static void say_bytes(char *into, size_t room, unsigned long long bytes)
{
	static const char *const UNIT[] = { "B", "KiB", "MiB", "GiB", "TiB" };
	unsigned long long whole = bytes;
	unsigned long long tenths = 0;
	int unit = 0;

	while (whole >= 1024ULL && unit < 4) {
		tenths = ((whole % 1024ULL) * 10ULL) / 1024ULL;
		whole /= 1024ULL;
		unit++;
	}

	if (unit == 0) {
		snprintf(into, room, "%llu %s", whole, UNIT[unit]);
	} else {
		snprintf(into, room, "%llu.%llu %s", whole, tenths,
			 UNIT[unit]);
	}
}

/* --- What the volume looks like --- */

/*
 * Ask the filesystem what is at the root and say so in one line.
 *
 * A first-boot screen that says nothing about storage is a screen that cannot
 * tell a machine which found its volume from one which did not, and that is
 * the single most useful thing it can report on a stranger's hardware.
 */
static void say_storage(char *into, size_t room)
{
	char listing[2048];
	i64 got;
	int entries = 0;
	size_t i;

	got = RECON_CALL4(SYS_LIST, "/", 1, listing, sizeof(listing) - 1);
	if (got < 0) {
		snprintf(into, room, "no volume this kernel can read");
		return;
	}
	if ((size_t)got >= sizeof(listing)) {
		got = (i64)(sizeof(listing) - 1);
	}
	listing[got] = '\0';

	/* The kernel answers with one name a line. Counting them is the whole
	 * of what this needs; showing them is the file manager's job and there
	 * is not one yet. */
	for (i = 0; i < (size_t)got; i++) {
		if (listing[i] == '\n') {
			entries++;
		}
	}
	if (entries == 0 && got > 0) {
		entries = 1;
	}

	snprintf(into, room, "%d %s at the root of the volume",
		 entries, entries == 1 ? "entry" : "entries");
}

int main(void)
{
	struct recon_screen screen;
	struct recon_machine machine;
	struct recon_canvas canvas;
	struct recon_first_boot facts;
	char kernel_line[96];
	char processors_line[64];
	char memory_line[96];
	char display_line[96];
	char storage_line[96];
	char total[32];
	char free_bytes[32];
	i64 answer;
	i64 fd;
	i64 mapped;

	/* --- The screen --- */

	answer = recon_screen(&screen, sizeof(screen));
	if (answer < 0) {
		return NO_SCREEN_DESCRIBED;
	}
	if ((u64)answer != sizeof(screen)) {
		/*
		 * The kernel's description is a different size from the one
		 * this was built against. **Refused rather than used**: the
		 * fields are laid out by agreement, and a disagreement about
		 * the size is a disagreement about the layout, which would
		 * draw a picture into whatever memory the wrong `pitch`
		 * pointed at.
		 */
		return SCREEN_STRUCT_WRONG;
	}
	if (screen.width == 0 || screen.height == 0 ||
	    screen.pitch < screen.width * 4u) {
		return SCREEN_MAKES_NO_SENSE;
	}

	fd = recon_open("/dev/fb0", 8, OPEN_WRITE | OPEN_READ, 0);
	if (fd < 0) {
		return FRAMEBUFFER_CLOSED;
	}

	mapped = recon_map((int)fd, screen.bytes);
	if (mapped < 0) {
		return MAPPING_REFUSED;
	}

	/* --- The machine --- */

	memset(&machine, 0, sizeof(machine));
	machine.size = (u32)sizeof(machine);
	answer = recon_machine_facts(&machine, sizeof(machine));
	if (answer < 0) {
		return MACHINE_UNREADABLE;
	}
	/*
	 * A kernel newer than this program answers with a *larger* structure
	 * and writes only what there was room for. That is a valid prefix and
	 * is used; it is the promise the size field exists to make.
	 */

	/* --- The facts, as strings --- */

	snprintf(kernel_line, sizeof(kernel_line), "kernel on %s, %u KiB pages",
		 machine.architecture[0] != '\0' ? machine.architecture
					         : "an unnamed architecture",
		 machine.page_size / 1024u);

	snprintf(processors_line, sizeof(processors_line),
		 "%u found, %u in use", machine.processors_found,
		 machine.processors_online);

	say_bytes(total, sizeof(total), machine.memory_bytes);
	say_bytes(free_bytes, sizeof(free_bytes), machine.memory_free_bytes);
	snprintf(memory_line, sizeof(memory_line), "%s, %s free",
		 total, free_bytes);

	snprintf(display_line, sizeof(display_line),
		 "%u x %u, %u bytes a row", screen.width, screen.height,
		 screen.pitch);

	say_storage(storage_line, sizeof(storage_line));

	memset(&facts, 0, sizeof(facts));
	facts.version = "ReconOS";
	facts.kernel = kernel_line;
	facts.processors = processors_line;
	facts.memory = memory_line;
	facts.cpu = machine.cpu_model[0] != '\0' ? machine.cpu_model
						 : machine.cpu_vendor;
	facts.display = display_line;
	facts.storage = storage_line;

	facts.notes[0] = "This machine is running its own kernel.";
	facts.notes[1] = "No Linux is underneath it.";
	facts.notes[2] = "";
	if (machine.entropy_bits == 0) {
		facts.notes[3] = "There is no hardware randomness here, so"
				 " keys would be refused.";
	} else {
		facts.notes[3] = "Randomness is available.";
	}

	/* --- Draw it --- */

	/*
	 * What it was given, before it draws anything with it.
	 *
	 * The first boot put the panel 420 pixels wide on a 2560-pixel screen
	 * and there were four plausible explanations. The program knows which
	 * one it is.
	 */
	{
		char line[160];

		snprintf(line, sizeof(line),
			 "  first screen: %u x %u, %u bytes a row,"
			 " %llu bytes mapped at %llx\n",
			 screen.width, screen.height, screen.pitch,
			 (unsigned long long)screen.bytes,
			 (unsigned long long)mapped);
		say(line);
	}

	canvas.pixels = (unsigned char *)(unsigned long)mapped;
	canvas.width = screen.width;
	canvas.height = screen.height;
	canvas.pitch = screen.pitch;

	recon_screen_draw(&canvas, &facts);

	/*
	 * And stay.
	 *
	 * Exiting would hand the screen back to whatever draws next, which on
	 * this kernel is the console -- so the first thing anybody saw would
	 * be replaced by a log before they had read it. Yielding rather than
	 * spinning, because a processor held at a hundred percent to show a
	 * static picture is a laptop with a fan on and an hour less battery.
	 *
	 * There is nothing to exit *to* yet. When there is -- a shell, a
	 * session, anything that can be started -- this returns instead, and
	 * that is the line that will change.
	 */
	for (;;) {
		recon_yield();
	}

	return WENT_WELL;
}
