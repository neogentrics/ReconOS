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
 * **And it allocates**, which it could not do until v0.4.33. What this
 * paragraph used to say is worth keeping: *"It allocates nothing. There is no
 * allocator on this kernel and that is the first entry in
 * `docs/KERNEL-WANTS.md`."* The entry was `SYS_MAP` with no file behind it,
 * and the kernel answers it now.
 *
 * Everything this program needs is still a fixed buffer sized at compile time,
 * which is the same discipline `libc/stdio.c` follows for the same reason. The
 * allocating is deliberate rather than needed: it is the only thing that runs
 * the whole path -- `malloc` to `mem_recon.c` to `SYS_MAP` to a region to a
 * page fault to a page -- on the machine, and it says what happened on the
 * screen.
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
#include <stdlib.h>
#include <string.h>

#include "layout.h"
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
#define PRESENT_REFUSED      66

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


/* --- Memory this program asked the machine for --------------------------- */

/*
 * How many blocks, and why a number rather than one allocation.
 *
 * One `malloc` that succeeds proves the kernel handed over a range. It says
 * nothing about the allocator laid out inside that range -- and the allocator
 * is the part with boundary tags, a footer in the next block's header, and
 * sixteen free lists, all of which were written against a host that answers
 * every request and none of which has ever run on a range this kernel gave.
 */
#define HEAP_BLOCKS 64

/* Big enough that `malloc` gives it a region of its own rather than carving it
 * out of a shared one -- which is the path that asks the kernel a *second*
 * time, and so the only one that can tell a working call from a working first
 * call. */
#define HEAP_BIG (512u * 1024u)

/*
 * Use the heap hard enough to be worth reporting, and say what happened.
 *
 * The middle pass is the one that matters. Freeing every other block and
 * allocating into the holes is what makes the free lists and the coalescing do
 * work; without it this is a bump allocator walking upwards through memory
 * nothing has touched, which is the one arrangement that cannot go wrong.
 *
 * Every block carries a pattern derived from where it is, so a block handed
 * out twice, a coalesce that swallowed a neighbour still in use, and a write
 * that went one block over are all the same observation: the bytes that came
 * back are not the bytes that went in.
 */
static void exercise_the_heap(char *into, size_t room)
{
	unsigned char *block[HEAP_BLOCKS];
	size_t size[HEAP_BLOCKS];
	unsigned char *big;
	unsigned i;
	size_t j;
	unsigned changed = 0;
	unsigned faults;
	size_t held, still_live;

	for (i = 0; i < HEAP_BLOCKS; i++) {
		size[i] = 24 + (i * 137u) % 900u;
		block[i] = (unsigned char *)malloc(size[i]);

		if (block[i] == NULL) {
			snprintf(into, room,
				 "refused after %u blocks -- this kernel has"
				 " no memory to hand a program", i);
			return;
		}

		for (j = 0; j < size[i]; j++)
			block[i][j] = (unsigned char)(i + j);
	}

	/* Half of them back, and the same half taken again. */
	for (i = 0; i < HEAP_BLOCKS; i += 2) {
		free(block[i]);
		block[i] = NULL;
	}

	for (i = 0; i < HEAP_BLOCKS; i += 2) {
		size[i] = 40 + (i * 53u) % 700u;
		block[i] = (unsigned char *)malloc(size[i]);

		if (block[i] == NULL) {
			snprintf(into, room,
				 "refused re-using freed memory at block %u",
				 i);
			return;
		}

		for (j = 0; j < size[i]; j++)
			block[i][j] = (unsigned char)(i + j);
	}

	big = (unsigned char *)malloc(HEAP_BIG);
	if (big == NULL) {
		snprintf(into, room,
			 "small blocks work and %u KiB was refused",
			 HEAP_BIG / 1024u);
		return;
	}
	memset(big, 0xA5, HEAP_BIG);

	/* Read back *after* the large one, so a second range that landed on top
	 * of the first is seen here rather than not at all. */
	for (i = 0; i < HEAP_BLOCKS; i++) {
		for (j = 0; j < size[i]; j++) {
			if (block[i][j] != (unsigned char)(i + j)) {
				changed++;
				break;
			}
		}
	}

	for (j = 0; j < HEAP_BIG; j++) {
		if (big[j] != 0xA5) {
			changed++;
			break;
		}
	}

	held = recon_malloc_held();

	free(big);
	for (i = 0; i < HEAP_BLOCKS; i++)
		free(block[i]);

	still_live = recon_malloc_live();
	faults = recon_malloc_audit();

	if (changed != 0) {
		snprintf(into, room,
			 "%u of %u blocks came back holding different bytes",
			 changed, HEAP_BLOCKS + 1);
	} else if (faults != 0) {
		snprintf(into, room,
			 "%u blocks survived and the heap does not hold"
			 " together (%u faults)", HEAP_BLOCKS + 1, faults);
	} else if (still_live != 0) {
		snprintf(into, room,
			 "%llu bytes still counted as in use after everything"
			 " was freed",
			 (unsigned long long)still_live);
	} else {
		snprintf(into, room,
			 "%u blocks and %u KiB written, read back and freed;"
			 " %llu KiB from the kernel",
			 HEAP_BLOCKS, HEAP_BIG / 1024u,
			 (unsigned long long)(held / 1024u));
	}
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

/* --- Laying the volume out --- */

/*
 * The one line of this that touches the kernel.
 *
 * `layout.c` decides *what* gets made and in what order and knows nothing
 * about system calls; this hands it the call. That split is what lets the
 * whole arrangement be checked on the host in a millisecond instead of by
 * installing onto a disk.
 */
static long make_one_directory(const char *path, unsigned long mode)
{
	return (long)recon_mkdir(path, recon_strlen(path), (u64)mode);
}

/*
 * Build the layout if it is not there, and say what happened in one line.
 *
 * Runs on **every** boot, not only the first. A second boot finds everything
 * already there and does nothing, which is the requirement rather than a
 * nicety: a first boot that lost power half way has to be able to finish, and
 * that is the same code path.
 */
static void lay_out_the_volume(char *into, size_t room)
{
	struct recon_layout_report report;
	int wanted = recon_layout_count();
	int present;

	present = recon_layout_build(make_one_directory, SYS_EEXIST, &report);

	if (report.refused != 0) {
		/*
		 * Named rather than counted. "1 refused" sends somebody
		 * looking; "/Users refused (-12)" tells them where and why,
		 * and this is a screen on a machine with no other way to ask.
		 */
		snprintf(into, room, "%d of %d directories -- %s refused (%ld)",
			 present, wanted, report.first_refused,
			 report.first_reason);
		return;
	}

	if (report.made == 0) {
		snprintf(into, room, "%d directories, all already there",
			 present);
		return;
	}
	if (report.already == 0) {
		snprintf(into, room, "%d directories, laid out just now",
			 present);
		return;
	}
	/* A mixture means a previous boot did not finish, which is worth
	 * saying rather than smoothing over. */
	snprintf(into, room, "%d directories -- %d were missing and are not now",
		 present, report.made);
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
	if (got == SYS_ENODEV) {
		snprintf(into, room, "no volume this kernel can read");
		return;
	}
	if (got < 0) {
		/* The number, not a conclusion drawn from it. This line read
		 * "no volume this kernel can read" on a machine that had just
		 * created ten directories on the volume -- true of one refusal
		 * and printed for every one, which is how a screen comes to
		 * contradict itself and give nobody anywhere to start. */
		snprintf(into, room, "a volume, but it would not be listed (%ld)",
			 (long)got);
		return;
	}
	/* Whole or nothing. The kernel writes a listing only if all of it
	 * fits, and answers with the size either way -- so a number bigger
	 * than what was offered means the buffer holds nothing, and counting
	 * it would be counting whatever was on the stack. */
	if ((size_t)got > sizeof(listing) - 1) {
		snprintf(into, room, "%ld bytes of names, more than this asked"
			 " for", (long)got);
		return;
	}
	listing[got] = '\0';

	/* The names come back NUL-terminated and back to back, which is what
	 * user.h says and not what this counted: it looked for newlines, found
	 * none, and reported the single entry its fallback invented. A volume
	 * with twelve names at its root said "1 entry".
	 *
	 * Showing them is the file manager's job and there is not one yet. */
	for (i = 0; i < (size_t)got; i++) {
		if (listing[i] == '\0') {
			entries++;
		}
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
	char layout_line[120];
	char heap_line[120];
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

	/* Before the listing, because the listing is what proves it worked. */
	lay_out_the_volume(layout_line, sizeof(layout_line));

	/* After the framebuffer is mapped, on purpose. The heap is handed the
	 * addresses after the screen's, so anything that got the two runs
	 * confused would be drawing into its own allocations. */
	exercise_the_heap(heap_line, sizeof(heap_line));
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

	facts.notes[0] = layout_line;
	facts.notes[1] = heap_line;
	facts.notes[2] = "This machine is running its own kernel.";
	facts.notes[3] = "No Linux is underneath it.";
	facts.notes[4] = "";
	if (machine.entropy_bits == 0) {
		facts.notes[5] = "There is no hardware randomness here, so"
				 " keys would be refused.";
	} else {
		facts.notes[5] = "Randomness is available.";
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

		snprintf(line, sizeof(line), "  the volume: %s\n",
			 layout_line);
		say(line);

		snprintf(line, sizeof(line), "  storage: %s\n",
			 storage_line);
		say(line);

		snprintf(line, sizeof(line), "  the heap: %s\n", heap_line);
		say(line);
	}

	canvas.pixels = (unsigned char *)(unsigned long)mapped;
	canvas.width = screen.width;
	canvas.height = screen.height;
	canvas.pitch = screen.pitch;

	recon_screen_draw(&canvas, &facts);

	/*
	 * And ask for it to be shown.
	 *
	 * Drawing is not showing. On a display whose framebuffer is scanned out
	 * continuously -- the EFI framebuffer, the Bochs adapter, real Intel and
	 * AMD silicon -- the two are the same thing and this call returns having
	 * done nothing. On virtio-gpu the pixels are memory the host cannot see
	 * until it is told, so without this the first screen is a black
	 * rectangle on a machine reporting every self-test passing. That is
	 * GX-003, and this program is the one place it would have mattered most:
	 * it is what somebody standing in front of the machine actually reads.
	 *
	 * Unconditional, and there is deliberately no way to ask which sort of
	 * display this is -- a program that branched on it would be wrong on one
	 * of them.
	 *
	 * The whole screen, spelled out, because there is no zero-means-
	 * everything: an uninitialised width is zero, and that is the one value
	 * which must not mean "do the most expensive thing".
	 */
	if (recon_present((int)fd, 0, 0, screen.width, screen.height) != SYS_OK) {
		return PRESENT_REFUSED;
	}

	/*
	 * And say so on the serial line, which is the only place this can be
	 * said -- the screen now belongs to this program and the console has
	 * stopped drawing on it.
	 *
	 * It is the marker the verification rig waits for before photographing
	 * the panel. "the heap:" above is printed *before* the drawing, so a rig
	 * keyed on that one screendumps a machine that has not drawn yet and
	 * reports a blank screen as a fault in the display.
	 */
	say("the first screen is up\n");

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
