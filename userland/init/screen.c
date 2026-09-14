/*
 * Drawing the first screen ReconOS shows on a machine of its own.
 *
 * No system call, no kernel, no knowledge that either exists -- a buffer, a
 * description of how it is arranged, and some facts. That is what lets the
 * host render the same picture the machine will render, which is the only way
 * to find out what it looks like without a loop measured in reboots.
 *
 * The font is the one the kernel's console and the bootloader's menu already
 * use, for the reason its own header gives: so that a machine looks like one
 * machine from the first pixel drawn to the last.
 */

#include "screen.h"

#include <recon_font.h>

/* --- Pixels --- */

/*
 * Every write goes through `pitch`.
 *
 * Not `width * 4`. An adapter pads a row to whatever suits it, and on the
 * emulator the two happen to be equal -- which is exactly why a program that
 * used the wrong one would pass every test here and shear on a laptop. The
 * kernel's own framebuffer note makes the same point from the other side.
 */
static void put_pixel(const struct recon_canvas *canvas, unsigned x,
		      unsigned y, unsigned int colour)
{
	unsigned char *at;

	if (x >= canvas->width || y >= canvas->height) {
		return;
	}
	at = canvas->pixels + (size_t)y * canvas->pitch + (size_t)x * 4u;
	at[0] = (unsigned char)(colour & 0xFFu);		/* blue */
	at[1] = (unsigned char)((colour >> 8) & 0xFFu);		/* green */
	at[2] = (unsigned char)((colour >> 16) & 0xFFu);	/* red */
	at[3] = 0xFFu;
}

static void fill(const struct recon_canvas *canvas, unsigned x, unsigned y,
		 unsigned w, unsigned h, unsigned int colour)
{
	unsigned dx;
	unsigned dy;

	for (dy = 0; dy < h; dy++) {
		for (dx = 0; dx < w; dx++) {
			put_pixel(canvas, x + dx, y + dy, colour);
		}
	}
}

/* --- Text --- */

unsigned recon_screen_text_width(unsigned scale, const char *text)
{
	unsigned n = 0;

	while (text != NULL && text[n] != '\0') {
		n++;
	}
	return n * RECON_FONT_WIDTH * scale;
}

void recon_screen_text(const struct recon_canvas *canvas, unsigned x,
		       unsigned y, unsigned scale, unsigned int colour,
		       const char *text)
{
	unsigned cursor = x;

	/* The canvas as well as the text. The first version guarded one and
	 * not the other, and a null canvas walked straight into `put_pixel`,
	 * which reads its width. */
	if (canvas == NULL || canvas->pixels == NULL || text == NULL) {
		return;
	}

	for (; *text != '\0'; text++) {
		unsigned char c = (unsigned char)*text;
		const unsigned char *glyph;
		unsigned row;

		/*
		 * Anything outside the font is drawn as a space rather than
		 * skipped. A missing glyph that takes no room shifts the rest
		 * of the line and makes a table stop lining up, which reads
		 * as a layout fault rather than as a character this font does
		 * not have.
		 */
		if (c < RECON_FONT_FIRST || c > RECON_FONT_LAST) {
			cursor += RECON_FONT_WIDTH * scale;
			continue;
		}
		glyph = recon_font[c - RECON_FONT_FIRST];

		for (row = 0; row < RECON_FONT_HEIGHT; row++) {
			unsigned bit;

			for (bit = 0; bit < RECON_FONT_WIDTH; bit++) {
				/* Leftmost pixel is the high bit, which the
				 * font's own header says. */
				if ((glyph[row] & (0x80u >> bit)) == 0) {
					continue;
				}
				fill(canvas, cursor + bit * scale,
				     y + row * scale, scale, scale, colour);
			}
		}
		cursor += RECON_FONT_WIDTH * scale;
	}
}

/* --- The screen --- */

/*
 * Laid out in a column that scales with the display rather than at fixed
 * pixel positions.
 *
 * The machines this has to look right on are a 640x480 emulator and a 4K
 * laptop, which is a factor of nine in each direction. Fixed positions make
 * one of those unreadable and the other a postage stamp; deriving everything
 * from a text size chosen for the height means both get the same picture at
 * the same apparent size.
 */
static unsigned scale_for(unsigned height)
{
	/* Aim for about forty rows of text on any screen, and never go below
	 * one -- a scale of zero draws nothing at all, which would be a black
	 * screen reported as a success. */
	unsigned s = height / (RECON_FONT_HEIGHT * 40u);

	if (s < 1u) {
		s = 1u;
	}
	if (s > 6u) {
		s = 6u;
	}
	return s;
}

void recon_screen_draw(const struct recon_canvas *canvas,
		       const struct recon_first_boot *facts)
{
	unsigned scale;
	unsigned line;
	unsigned left;
	unsigned top;
	unsigned panel_w;
	unsigned panel_h;
	unsigned y;
	unsigned i;

	if (canvas == NULL || canvas->pixels == NULL || facts == NULL ||
	    canvas->width == 0 || canvas->height == 0) {
		return;
	}

	scale = scale_for(canvas->height);
	line = RECON_FONT_HEIGHT * scale + scale * 2u;

	fill(canvas, 0, 0, canvas->width, canvas->height,
	     RECON_INK_BACKGROUND);

	/*
	 * Too small to put anything on, so the background is the whole of what
	 * gets drawn -- and it is drawn, rather than the function returning
	 * before it.
	 *
	 * The arithmetic below is all unsigned, so a panel narrower than its
	 * own margins does not come out negative: it comes out at four
	 * billion, and the fill loop that follows runs until somebody turns
	 * the machine off. A screen that is merely blank is a far better
	 * failure than one that appears to have hung.
	 */
	if (canvas->width < line * 8u || canvas->height < line * 8u) {
		return;
	}

	/*
	 * A panel rather than text on the bare background, because the first
	 * thing this has to say is that something deliberate is running. A
	 * machine that boots to text on black is indistinguishable from a
	 * machine that has stopped.
	 *
	 * **Its height is measured from what goes on it**, not taken from the
	 * screen. The first version took three quarters of the display and the
	 * content filled the top third of that -- which every check in the
	 * suite passed, because every check was about whether things were
	 * drawn and none was about whether the box fitted them. Looking at the
	 * rendering is what said so.
	 */
	{
		unsigned rows = 0;

		rows += 2;			/* margin above the title */
		rows += 2;			/* the title, at twice the size */
		rows += 3;			/* air under it */
		rows += 1;			/* the kernel line */
		rows += 2;			/* and air under that */
		rows += 1;			/* the rule */

		for (i = 0; i < 5; i++) {
			const char *v[5];

			v[0] = facts->processors;
			v[1] = facts->memory;
			v[2] = facts->cpu;
			v[3] = facts->display;
			v[4] = facts->storage;
			if (v[i] != NULL) {
				rows++;
			}
		}

		rows += 1;			/* air before the notes */
		for (i = 0; i < 8 && facts->notes[i] != NULL; i++) {
			rows++;
		}
		rows += 2;			/* margin below */

		panel_h = rows * line;
	}

	panel_w = canvas->width - canvas->width / 6u;
	if (panel_h > canvas->height - canvas->height / 8u) {
		/* Taller than the screen can hold. Clamped rather than drawn
		 * off the bottom, so the lines that do fit are still centred
		 * and the ones that do not are simply absent -- which is
		 * visible, where a panel hanging off the edge is not. */
		panel_h = canvas->height - canvas->height / 8u;
	}
	left = (canvas->width - panel_w) / 2u;
	top = (canvas->height - panel_h) / 2u;

	fill(canvas, left, top, panel_w, panel_h, RECON_INK_PANEL);

	/* A rule along the top of the panel in the accent, which is the only
	 * place the accent is used at any size. One thing carries it. */
	fill(canvas, left, top, panel_w, scale * 2u, RECON_INK_ACCENT);

	y = top + line * 2u;
	recon_screen_text(canvas, left + line, y, scale * 2u, RECON_INK_TEXT,
			  facts->version != NULL ? facts->version : "ReconOS");
	y += line * 3u;

	recon_screen_text(canvas, left + line, y, scale, RECON_INK_DIM,
			  facts->kernel);
	y += line * 2u;

	fill(canvas, left + line, y, panel_w - line * 2u, scale,
	     RECON_INK_RULE);
	y += line;

	/*
	 * The facts, one to a line, in the order somebody standing in front of
	 * a machine that has just started wants them: what it is, what it is
	 * running on, and what it can see.
	 */
	{
		static const char *const LABEL[] = {
			"processors", "memory", "processor", "display",
			"storage",
		};
		const char *value[5];

		value[0] = facts->processors;
		value[1] = facts->memory;
		value[2] = facts->cpu;
		value[3] = facts->display;
		value[4] = facts->storage;

		for (i = 0; i < 5; i++) {
			if (value[i] == NULL) {
				continue;
			}
			recon_screen_text(canvas, left + line, y, scale,
					  RECON_INK_DIM, LABEL[i]);
			recon_screen_text(canvas,
					  left + line +
					  RECON_FONT_WIDTH * scale * 12u,
					  y, scale, RECON_INK_TEXT, value[i]);
			y += line;
		}
	}

	y += line;

	for (i = 0; i < 8; i++) {
		if (facts->notes[i] == NULL) {
			break;
		}
		recon_screen_text(canvas, left + line, y, scale,
				  RECON_INK_DIM, facts->notes[i]);
		y += line;
	}
}
