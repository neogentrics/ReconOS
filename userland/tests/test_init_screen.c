/*
 * The first screen ReconOS draws, rendered on the host and checked.
 *
 * `screen.c` makes no system call and knows nothing about a kernel, so the
 * picture a machine will show can be produced here, in memory, in a
 * millisecond. The alternative is writing a stick, walking to a machine and
 * turning it on -- a loop measured in minutes that produces a photograph
 * rather than a number.
 *
 * Both are wanted, and this is the half that can fail loudly. BG-174 is the
 * record of what a photograph catches that a number does not; what a number
 * catches that a photograph does not is **pitch**: on a screen where bytes per
 * row happen to equal width times four, a program that confuses the two draws
 * a perfect picture. Every canvas below has padding on purpose, and the suite
 * checks that not one byte of it was touched.
 *
 * It also writes the rendering out as a PNM, so somebody can look.
 *
 * Run with: ./build/recon_init_screen_tests
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../init/screen.h"

static int g_failures;
static long g_checks;

static void check(int condition, const char *what)
{
	g_checks++;
	if (!condition) {
		g_failures++;
		printf("  FAIL: %s\n", what);
	}
}

/* --- A canvas with padding, because padding is the point --- */

#define GUARD 0xA5u

struct sheet {
	unsigned char *memory;
	size_t bytes;
	struct recon_canvas canvas;
	unsigned padding;
};

/*
 * `padding` bytes of slack on the end of every row, filled with a byte no
 * drawing should ever write. A program that computes its row stride as
 * `width * 4` lands in that slack on every row but the first, and the check
 * at the end sees it.
 */
static void sheet_make(struct sheet *s, unsigned width, unsigned height,
		       unsigned padding)
{
	s->padding = padding;
	s->canvas.width = width;
	s->canvas.height = height;
	s->canvas.pitch = width * 4u + padding;
	s->bytes = (size_t)s->canvas.pitch * height;
	s->memory = malloc(s->bytes);
	if (s->memory == NULL) {
		printf("  FAIL: out of memory for a %ux%u sheet\n",
		       width, height);
		exit(1);
	}
	memset(s->memory, GUARD, s->bytes);
	s->canvas.pixels = s->memory;
}

/*
 * Clear the pixels and **leave the padding alone**.
 *
 * The guard byte is what catches a program using width times four as its
 * stride, so it has to survive; the pixel area has to start at a known value
 * or a question like "did this glyph draw anything" has no answer. The first
 * version of this suite cleared neither and asked that question anyway, so
 * every pixel read as lit before a glyph was drawn.
 */
static void sheet_clear(struct sheet *s)
{
	unsigned y;

	for (y = 0; y < s->canvas.height; y++) {
		memset(s->memory + (size_t)y * s->canvas.pitch, 0,
		       s->canvas.width * 4u);
	}
}

static void sheet_check_padding(struct sheet *s, const char *what)
{
	unsigned y;
	unsigned i;

	if (s->padding == 0) {
		return;
	}
	for (y = 0; y < s->canvas.height; y++) {
		const unsigned char *slack = s->memory +
			(size_t)y * s->canvas.pitch + s->canvas.width * 4u;

		for (i = 0; i < s->padding; i++) {
			g_checks++;
			if (slack[i] != GUARD) {
				g_failures++;
				printf("  FAIL: %s wrote into the padding at"
				       " row %u, byte %u -- it is using width"
				       " times four as a stride\n",
				       what, y, i);
				return;		/* one report is enough */
			}
		}
	}
}

static unsigned int pixel_at(const struct recon_canvas *c, unsigned x,
			     unsigned y)
{
	const unsigned char *at = c->pixels + (size_t)y * c->pitch +
		(size_t)x * 4u;

	return ((unsigned int)at[2] << 16) | ((unsigned int)at[1] << 8) |
		(unsigned int)at[0];
}

static void sheet_free(struct sheet *s)
{
	free(s->memory);
	s->memory = NULL;
}

/* --- The facts a screen is drawn from --- */

static void fill_facts(struct recon_first_boot *facts)
{
	memset(facts, 0, sizeof(*facts));
	facts->version = "ReconOS";
	facts->kernel = "kernel on x86_64, 4 KiB pages";
	facts->processors = "8 found, 4 in use";
	facts->memory = "15.6 GiB, 15.2 GiB free";
	facts->cpu = "AMD Ryzen 7 5800X 8-Core Processor";
	facts->display = "1920 x 1080, 7680 bytes a row";
	facts->storage = "3 entries at the root of the volume";
	facts->notes[0] = "This machine is running its own kernel.";
	facts->notes[1] = "No Linux is underneath it.";
}

/* --- The tests --- */

static void test_text(void)
{
	struct sheet s;
	unsigned lit;
	unsigned x;
	unsigned y;

	printf("text, one glyph at a time\n");

	sheet_make(&s, 64, 32, 24);
	sheet_clear(&s);

	/* A space marks nothing, which is what makes it a space. */
	recon_screen_text(&s.canvas, 0, 0, 1, RECON_INK_TEXT, "   ");
	lit = 0;
	for (y = 0; y < 32; y++) {
		for (x = 0; x < 64; x++) {
			if (pixel_at(&s.canvas, x, y) != 0) {
				lit++;
			}
		}
	}
	check(lit == 0, "a space draws nothing");

	/* A full stop marks something, and only in the bottom half. */
	sheet_clear(&s);
	recon_screen_text(&s.canvas, 0, 0, 1, RECON_INK_TEXT, ".");
	lit = 0;
	for (y = 0; y < 4; y++) {
		for (x = 0; x < 8; x++) {
			if (pixel_at(&s.canvas, x, y) != 0) {
				lit++;
			}
		}
	}
	check(lit == 0, "a full stop has nothing in its top half");
	lit = 0;
	for (y = 4; y < 8; y++) {
		for (x = 0; x < 8; x++) {
			if (pixel_at(&s.canvas, x, y) != 0) {
				lit++;
			}
		}
	}
	check(lit > 0, "and something in its bottom half");

	/*
	 * A character the font does not have still takes its place.
	 *
	 * If it did not, everything after it on the line would shift left and
	 * a table would stop lining up -- which reads as a layout fault rather
	 * than as a character this font has never had.
	 */
	check(recon_screen_text_width(1, "ab") ==
	      recon_screen_text_width(1, "a\x01"),
	      "a character the font lacks still takes its width");

	check(recon_screen_text_width(2, "abc") ==
	      recon_screen_text_width(1, "abc") * 2,
	      "twice the scale is twice the width");

	sheet_check_padding(&s, "text");
	sheet_free(&s);
}

static void test_the_screen_at_every_size(void)
{
	static const struct { unsigned w; unsigned h; } SIZES[] = {
		{ 640, 480 },		/* the emulator's default */
		{ 800, 600 },
		{ 1024, 768 },
		{ 1280, 800 },		/* what the font's header sizes for */
		{ 1366, 768 },		/* the commonest laptop there has ever been */
		{ 1920, 1080 },
		{ 2560, 1440 },
		{ 3840, 2160 },		/* and a 4K panel */
		{ 320, 200 },		/* absurdly small, on purpose */
	};
	struct recon_first_boot facts;
	size_t i;

	printf("the whole screen, at every size a machine might have\n");
	fill_facts(&facts);

	for (i = 0; i < sizeof(SIZES) / sizeof(SIZES[0]); i++) {
		struct sheet s;
		unsigned lit = 0;
		unsigned x;
		unsigned y;
		char what[64];

		/* A different padding each time, so a stride fault cannot
		 * hide behind one that happens to be zero. */
		sheet_make(&s, SIZES[i].w, SIZES[i].h,
			   (unsigned)(i % 4) * 16u + 8u);

		recon_screen_draw(&s.canvas, &facts);

		snprintf(what, sizeof(what), "the screen at %ux%u",
			 SIZES[i].w, SIZES[i].h);
		sheet_check_padding(&s, what);

		/*
		 * Something was drawn everywhere it should be, and the
		 * background is not the panel -- which is the cheapest way to
		 * catch a layout that has collapsed to nothing.
		 */
		check(pixel_at(&s.canvas, 0, 0) == RECON_INK_BACKGROUND,
		      "the corner is the background");
		check(pixel_at(&s.canvas, SIZES[i].w / 2, SIZES[i].h / 2) !=
		      RECON_INK_BACKGROUND,
		      "and the middle is not");

		/* The accent rule exists and is exactly one thing. */
		for (y = 0; y < SIZES[i].h; y++) {
			for (x = 0; x < SIZES[i].w; x++) {
				if (pixel_at(&s.canvas, x, y) ==
				    RECON_INK_ACCENT) {
					lit++;
				}
			}
		}
		check(lit > 0, "the accent rule is drawn");

		/* And text reached the panel: some pixels are the text ink. */
		lit = 0;
		for (y = 0; y < SIZES[i].h; y++) {
			for (x = 0; x < SIZES[i].w; x++) {
				if (pixel_at(&s.canvas, x, y) ==
				    RECON_INK_TEXT) {
					lit++;
				}
			}
		}
		check(lit > 0, "and there is text on it");

		sheet_free(&s);
	}
}

/*
 * The screen must refuse rather than fault on a canvas that makes no sense.
 *
 * This runs on somebody's machine on the first boot, where the framebuffer
 * description comes from firmware -- and firmware says surprising things. A
 * program that faults here shows nothing at all and cannot say why.
 */
static void test_what_it_refuses(void)
{
	struct recon_first_boot facts;
	struct recon_canvas canvas;
	unsigned char one[64];

	printf("the descriptions it refuses rather than faults on\n");
	fill_facts(&facts);

	memset(&canvas, 0, sizeof(canvas));
	recon_screen_draw(&canvas, &facts);
	check(1, "a canvas with no pixels is refused");

	canvas.pixels = one;
	canvas.width = 0;
	canvas.height = 0;
	canvas.pitch = 0;
	recon_screen_draw(&canvas, &facts);
	check(1, "and a canvas with no size");

	canvas.width = 4;
	canvas.height = 4;
	canvas.pitch = 16;
	memset(one, GUARD, sizeof(one));
	recon_screen_draw(&canvas, &facts);
	check(1, "and a canvas far too small for anything on it");

	recon_screen_draw(NULL, &facts);
	recon_screen_text(NULL, 0, 0, 1, 0, "x");
	check(1, "and no canvas at all");
}

/* --- And write it out, so somebody can look --- */

static void write_picture(void)
{
	struct recon_first_boot facts;
	struct sheet s;
	FILE *f;
	unsigned x;
	unsigned y;

	fill_facts(&facts);
	sheet_make(&s, 1280, 800, 64);
	recon_screen_draw(&s.canvas, &facts);

	f = fopen("recon-first-boot.pnm", "wb");
	if (f == NULL) {
		printf("  (could not write the picture)\n");
		sheet_free(&s);
		return;
	}
	fprintf(f, "P6\n%u %u\n255\n", s.canvas.width, s.canvas.height);
	for (y = 0; y < s.canvas.height; y++) {
		for (x = 0; x < s.canvas.width; x++) {
			unsigned int c = pixel_at(&s.canvas, x, y);
			unsigned char rgb[3];

			rgb[0] = (unsigned char)((c >> 16) & 0xFF);
			rgb[1] = (unsigned char)((c >> 8) & 0xFF);
			rgb[2] = (unsigned char)(c & 0xFF);
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
	printf("wrote recon-first-boot.pnm -- 1280x800, what a machine will"
	       " show\n");
	sheet_free(&s);
}

int main(void)
{
	printf("ReconOS: the first screen, drawn without a kernel\n\n");

	test_text();
	test_the_screen_at_every_size();
	test_what_it_refuses();
	write_picture();

	printf("\n%ld checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
