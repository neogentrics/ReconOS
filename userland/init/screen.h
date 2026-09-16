/*
 * The first-boot screen, as a function of facts rather than of a machine.
 *
 * Split out from `recon_init.c` for one reason: **so it can be drawn without
 * a kernel.** Everything here takes a buffer and a description and writes
 * pixels; nothing here makes a system call or knows one exists. So the host
 * can render exactly what the machine will render, into memory, and the suite
 * can check the pixels and write out a picture somebody can look at.
 *
 * That matters more than it sounds. The alternative is finding out what the
 * first screen ReconOS ever draws looks like by putting a stick in a machine
 * and turning it on, which is a loop measured in minutes and produces a
 * photograph rather than a number. BG-174 is the record of what a photograph
 * catches that a number does not; this is the other half, and both are wanted.
 */

#ifndef RECON_INIT_SCREEN_H
#define RECON_INIT_SCREEN_H

#include <stddef.h>

/*
 * Where the pixels are and how they are arranged.
 *
 * **`pitch` is the field that cannot be derived**: bytes per row is not
 * `width * 4` on real hardware, because adapters pad a row to whatever suits
 * them. Everything below draws through it. The kernel's own note says the same
 * thing on the other side of the boundary, and `kernel/user/paint.c` exists
 * largely to prove it.
 */
struct recon_canvas {
	unsigned char *pixels;
	unsigned int width;
	unsigned int height;
	unsigned int pitch;
};

/*
 * What the screen has to say. Every field is a string because every field is
 * going to be drawn, and formatting a number is the caller's job -- it has
 * `snprintf` and this has no business knowing whether memory is counted in
 * bytes or gibibytes.
 */
struct recon_first_boot {
	const char *version;		/* "ReconOS 0.4.24" */
	const char *kernel;		/* "kernel 0.2.27, x86_64" */
	const char *processors;		/* "8 processors, 4 in use" */
	const char *memory;		/* "16.0 GiB, 15.2 GiB free" */
	const char *cpu;		/* the model string */
	const char *display;		/* "1920 x 1080, 7680 bytes a row" */
	const char *storage;		/* what the volume looks like */

	/* Up to eight lines of whatever the system wants to say about itself.
	 * A null entry ends the list early. */
	const char *notes[8];
};

/* Colours, as 0x00RRGGBB. The framebuffer is 32 bits a pixel with the top
 * byte unused, which is what every adapter this will meet reports. */
#define RECON_INK_BACKGROUND 0x00141821u
#define RECON_INK_PANEL      0x001C2130u
#define RECON_INK_TEXT       0x00D8DCE6u
#define RECON_INK_DIM        0x00737C90u
#define RECON_INK_ACCENT     0x004FB3A5u
#define RECON_INK_RULE       0x002A3145u

void recon_screen_draw(const struct recon_canvas *canvas,
		       const struct recon_first_boot *facts);

/* Exposed so the suite can check a single glyph rather than only a whole
 * screen, and so a caller can measure before it draws. */
/*
 * Draw text, stopping before `right`.
 *
 * `right` is the first column that must not be drawn on; zero means the whole
 * canvas. A line that does not fit is cut with a `>` in the last cell rather
 * than running on, because `put_pixel` clips to the canvas and not to the box
 * -- so before this had a bound, a long line left the panel, crossed the gap
 * and ran off the side of the screen. It took a photograph of a real boot to
 * notice, which is exactly what a bound is for.
 */
void recon_screen_text(const struct recon_canvas *canvas, unsigned x,
		       unsigned y, unsigned scale, unsigned int colour,
		       const char *text, unsigned right);
unsigned recon_screen_text_width(unsigned scale, const char *text);

#endif /* RECON_INIT_SCREEN_H */
