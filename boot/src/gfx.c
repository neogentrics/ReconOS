/* The boot menu, drawn.
 *
 * --- Why this tries and then gives up rather than deciding ------------------
 *
 * A graphical menu is not something a bootloader may *rely* on. The firmware
 * decides whether there is a framebuffer at all: AAVMF offers none, plenty of
 * server boards offer none, and a machine whose display has not been detected
 * offers none. So this is written as an attempt with a text menu behind it, and
 * the attempt is allowed to fail at every step:
 *
 *   - no framebuffer                       -> text
 *   - a pixel layout we cannot name        -> text
 *   - a pitch that cannot hold a row       -> text
 *   - a screen too small for the menu      -> text
 *
 * The last two are the ones worth being strict about. **Drawing into a
 * framebuffer we have misread is worse than not drawing at all**: the machine
 * shows garbage, or writes over device registers past the end of each row, and
 * either way the person in front of it has lost the one screen that was going
 * to tell them what is wrong. Refusing is the cheaper failure.
 *
 * The fallback is not a rarely-taken branch. Every aarch64 boot in the
 * verification matrix takes it, because AAVMF has no framebuffer -- so the text
 * path is exercised on half the runs and cannot quietly rot.
 *
 * --- What it draws ---------------------------------------------------------
 *
 * The same font the kernel's console uses, so a machine looks like one machine
 * from the first pixel to the last, rather than like two programs that happen
 * to run in sequence.
 */
#include "efi.h"
#include "boot_internal.h"

#include <recon_font.h>

#define SCALE   2
#define CELL_W  (RECON_FONT_WIDTH * SCALE)
#define CELL_H  (RECON_FONT_HEIGHT * SCALE)

/* Menu lines are spaced wider than a glyph is tall.
 *
 * At exactly CELL_H the rows touch: the highlight bar behind one entry runs
 * into the entry below it, and the descenders on g, p and y hang into the next
 * line. A console packs lines tightly on purpose because it wants the rows; a
 * menu of four things does not, and the space is what makes it readable as a
 * list rather than a paragraph. */
#define LINE_H  (CELL_H + 6)

/* Room for a title, a blank, up to MENU_MAX entries, a blank and a footer.
 * A screen that cannot hold that is a screen this does not use. */
#define NEEDED_ROWS (MENU_MAX + 6)
#define NEEDED_COLS 40

static struct {
	volatile UINT8 *pixels;
	UINT32 pitch, width, height, format;
	BOOLEAN ready;
} screen;

/* The channel order is a statement about *bytes in memory*, and this writes a
 * 32-bit word on a little-endian machine -- so the first byte is the low eight
 * bits, and BGRA means blue is at the bottom and red at bits 16..23.
 *
 * These two cases were the wrong way round, and the kernel's console had the
 * same mistake. It was invisible in both until something drew a colour that was
 * not grey: amber came out blue on the first menu that used it, and every grey
 * before that had looked perfectly correct, because a red-and-blue swap does
 * nothing to a colour whose red and blue are equal.
 */
static UINT32 pack(UINT8 r, UINT8 g, UINT8 b)
{
	if (screen.format == RECONBOOT_PIXEL_RGBA)
		return ((UINT32)b << 16) | ((UINT32)g << 8) | r;

	return ((UINT32)r << 16) | ((UINT32)g << 8) | b;	/* BGRA */
}

static void put_pixel(UINT32 x, UINT32 y, UINT32 colour)
{
	if (x >= screen.width || y >= screen.height)
		return;

	*(volatile UINT32 *)(screen.pixels + (UINT64)y * screen.pitch +
			     (UINT64)x * 4) = colour;
}

static void fill(UINT32 x0, UINT32 y0, UINT32 w, UINT32 h, UINT32 colour)
{
	UINT32 x, y;

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			put_pixel(x0 + x, y0 + y, colour);
}

/* One glyph, at whatever multiple of the font it is asked for.
 *
 * The scale is a parameter rather than the SCALE constant so the title can be
 * larger than the body. A header set at the same size as its own subtitle is
 * not a header, and this menu had the two sharing a single line until the
 * first machine with a different screen made that visible. */
static void draw_char_at(UINT32 x0, UINT32 y0, char c, UINT32 ink,
			 unsigned scale)
{
	const unsigned char *g;
	unsigned gy, gx, sy, sx;

	if ((unsigned char)c < RECON_FONT_FIRST ||
	    (unsigned char)c > RECON_FONT_LAST)
		return;

	g = recon_font[(unsigned char)c - RECON_FONT_FIRST];

	for (gy = 0; gy < RECON_FONT_HEIGHT; gy++)
		for (gx = 0; gx < RECON_FONT_WIDTH; gx++)
			if (g[gy] & (0x80u >> gx))
				for (sy = 0; sy < scale; sy++)
					for (sx = 0; sx < scale; sx++)
						put_pixel(x0 + gx * scale + sx,
							  y0 + gy * scale + sy,
							  ink);
}

static void draw_char(UINT32 x0, UINT32 y0, char c, UINT32 ink)
{
	draw_char_at(x0, y0, c, ink, SCALE);
}

static void draw_text(UINT32 col, UINT32 row, const char *s, UINT32 ink)
{
	UINT32 x = col * CELL_W;
	UINT32 y = row * LINE_H;

	while (*s) {
		draw_char(x, y, *s++, ink);
		x += CELL_W;
	}
}

/* Text positioned in pixels rather than in cells, because a larger scale does
 * not land on the cell grid the rest of the menu is laid out on. */
static void draw_big(UINT32 x, UINT32 y, const char *s, UINT32 ink,
		     unsigned scale)
{
	while (*s) {
		draw_char_at(x, y, *s++, ink, scale);
		x += RECON_FONT_WIDTH * scale;
	}
}

static void draw_dec(UINT32 col, UINT32 row, unsigned v, UINT32 ink)
{
	char buf[12];
	int i = 0;

	if (!v) {
		draw_char(col * CELL_W, row * LINE_H, '0', ink);
		return;
	}
	while (v && i < 11) {
		buf[i++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (i--) {
		draw_char(col * CELL_W, row * LINE_H, buf[i], ink);
		col++;
	}
}

/* Whether a screen can be drawn on at all. Every reason to say no is a reason
 * the text menu runs instead, and none of them is an error. */
BOOLEAN gfx_available(const struct reconboot_framebuffer *fb)
{
	if (!fb || !fb->base || !fb->width || !fb->height)
		return FALSE;

	if (fb->format != RECONBOOT_PIXEL_BGRA &&
	    fb->format != RECONBOOT_PIXEL_RGBA)
		return FALSE;

	/* A row that does not fit in its own pitch is a description we do not
	 * believe, and drawing into it writes past the end of every line. */
	if (fb->pitch < fb->width * 4)
		return FALSE;

	if (fb->width / CELL_W < NEEDED_COLS ||
	    fb->height / LINE_H < NEEDED_ROWS)
		return FALSE;

	screen.pixels = (volatile UINT8 *)(UINTN)fb->base;
	screen.pitch  = fb->pitch;
	screen.width  = fb->width;
	screen.height = fb->height;
	screen.format = fb->format;
	screen.ready  = TRUE;
	return TRUE;
}

/* One frame of the menu, with `selected` highlighted and `seconds` left on the
 * clock. **Drawn in full**, and that is now the exception rather than the rule.
 *
 * This comment used to argue that a full redraw was right *every* time --
 * because a partial one that gets its arithmetic wrong leaves the previous
 * frame's text underneath the new one, and nobody can tell which is which. The
 * reasoning is sound and it was applied too widely: the countdown called this
 * once a second to change one digit, and clearing an uncached framebuffer is
 * slow enough to see, so the menu blinked black on every tick (KF-245).
 *
 * `gfx_menu_countdown` repaints the one row that changes. It answers the
 * objection above rather than ignoring it: it does not compute a layout of its
 * own, it uses the one this function recorded, and it clears the whole width of
 * the content block so no digit of a longer number survives a shorter one. */
/* Where `gfx_menu_draw` last put the status line.
 *
 * Only that one row changes while the clock runs, and repainting the whole
 * screen to change a digit is what made the menu flash once a second on the
 * first machine anybody watched it on. (KF-245) */
static UINT32 status_row, status_left, status_x0, status_x1;
static BOOLEAN status_ready;

void gfx_menu_draw(unsigned count, const char *const *labels,
		   unsigned selected, unsigned seconds, BOOLEAN paused)
{
	UINT32 ink    = pack(0xD8, 0xDC, 0xE0);
	UINT32 dim    = pack(0x70, 0x78, 0x80);
	UINT32 accent = pack(0xE0, 0xA8, 0x40);
	UINT32 paper  = pack(0x0C, 0x0E, 0x10);
	UINT32 bar    = pack(0x1E, 0x24, 0x2A);
	UINT32 rule   = pack(0x2A, 0x32, 0x3A);
	UINT32 cols   = screen.width / CELL_W;

	/* The content is a fixed number of columns wide and centred, so the
	 * same layout lands on a 1280x800 panel and on the 800x600 one this was
	 * first photographed on. Everything below positions against WIDE rather
	 * than against the screen. */
	const UINT32 WIDE = 36;
	UINT32 left = cols > WIDE ? (cols - WIDE) / 2 : 0;
	UINT32 x0   = left * CELL_W;
	UINT32 x1   = (left + WIDE) * CELL_W;

	/* Header, subtitle, rule, the default, the entries, rule, two lines of
	 * footer -- counted rather than guessed, because this number is what
	 * centres the whole thing and a stale one pushes it off a short screen. */
	UINT32 rows  = count + 10;
	UINT32 first = screen.height / LINE_H > rows
		     ? (screen.height / LINE_H - rows) / 2 : 0;
	unsigned i;

	if (!screen.ready)
		return;

	fill(0, 0, screen.width, screen.height, paper);

	/* **The name on its own line, and larger than the line under it.** It
	 * shared one with the subtitle before, which meant the two were
	 * positioned by counting the characters in the first -- and a rename
	 * would have silently overlapped them. */
	draw_big(x0, first * LINE_H, "ReconOS", accent, SCALE * 2);
	draw_text(left, first + 2, "choose what to start", dim);

	/* Rules are rectangles, not rows of dashes: a character-drawn rule is
	 * as wide as the font happens to be and has to be counted out to fit. */
	fill(x0, (first + 3) * LINE_H, x1 - x0, 1, rule);

	/* **The default is a row, and it is the one on the bar.**
	 *
	 * It was not drawn at all before. ReconOS is what Enter starts and what
	 * the clock starts, and the screen instead put the highlight on the
	 * first *other* entry -- `> 1. ReconOS recovery` -- which is precisely
	 * what a person reads as "this is what will happen". It was not.
	 *
	 * Keyed `Enter` rather than numbered, because that is the key that
	 * picks it, and the numbers below are the ones '1' to '9' already map
	 * to. The screen now says what the keyboard does.
	 */
	{
		UINT32 row = first + 4;

		if (selected == 0) {
			fill(x0, row * LINE_H - 3, x1 - x0, LINE_H, bar);
			draw_text(left, row, ">", accent);
		}

		draw_text(left + 2, row, "Enter", selected == 0 ? accent : dim);
		draw_text(left + 9, row, "ReconOS",
			  selected == 0 ? ink : dim);
	}

	for (i = 0; i < count; i++) {
		UINT32 row = first + 5 + i;
		BOOLEAN on = (selected == i + 1);

		if (on) {
			fill(x0, row * LINE_H - 3, x1 - x0, LINE_H, bar);
			draw_text(left, row, ">", accent);
		}

		draw_dec(left + 2, row, i + 1, on ? accent : dim);
		draw_text(left + 9, row, labels[i], on ? ink : dim);
	}

	fill(x0, (first + count + 6) * LINE_H, x1 - x0, 1, rule);

	/* Where the status line ended up, so that the countdown can repaint
	 * that row alone without recomputing a layout it did not choose.
	 *
	 * Remembered rather than recalculated because the arithmetic above --
	 * `first`, `count + 7`, the centring -- is this function's, and a
	 * second copy of it elsewhere is a second thing to get wrong when the
	 * layout changes. (KF-245) */
	status_row  = first + count + 7;
	status_left = left;
	status_x0   = x0;
	status_x1   = x1;
	status_ready = TRUE;

	/* **The status line is drawn here, in this font, on this surface.**
	 * It used to be a `print` to the firmware's console -- which owns these
	 * same pixels, draws at its own size, and puts it wherever its cursor
	 * happened to be. On the first real machine this ran on, that landed
	 * through the middle of the title. */
	if (paused) {
		draw_text(left, first + count + 7, "waiting for you", ink);
		draw_text(left, first + count + 8,
			  "a number chooses, Enter starts ReconOS", dim);
	} else {
		draw_text(left, first + count + 7, "starting in", dim);
		draw_dec(left + 12, first + count + 7, seconds, accent);
		draw_text(left + 14, first + count + 7,
			  seconds == 1 ? "second" : "seconds", dim);

		draw_text(left, first + count + 8,
			  "a number chooses, any key waits", dim);
	}
}

/* Repaint the countdown, and nothing else.
 *
 * **The menu flashed once a second and this is why.** `gfx_menu_draw` opens
 * with `fill(0, 0, width, height, paper)` -- it clears the whole screen and
 * redraws every element -- and the countdown called it once a second to change
 * one digit. Over an uncached framebuffer that clear is slow enough to see, so
 * the panel blinked black between the wipe and the redraw, on every tick, on
 * the one screen a person is actually looking at while deciding what to boot.
 *
 * Joshua noticed it on the Gateway and asked whether it was normal. It was not.
 *
 * One row is cleared here rather than one screen. The bar is the width of the
 * content block, so a shorter number leaves no digits of the longer one behind
 * -- clearing only the glyphs that changed would turn `10` into `1` with a
 * stale `0` beside it.
 *
 * Falls back to a full draw if the layout is not known, which happens only if
 * this is called before anything has been drawn. Refusing would be a countdown
 * that silently stops counting. (KF-245)
 */
void gfx_menu_countdown(unsigned count, const char *const *labels,
			unsigned seconds)
{
	UINT32 ink    = pack(0xD8, 0xDC, 0xE0);
	UINT32 dim    = pack(0x70, 0x78, 0x80);
	UINT32 accent = pack(0xE0, 0xA8, 0x40);
	UINT32 paper  = pack(0x0C, 0x0E, 0x10);

	(void)ink;

	if (!screen.ready)
		return;

	if (!status_ready) {
		gfx_menu_draw(count, labels, 0, seconds, FALSE);
		return;
	}

	fill(status_x0, status_row * LINE_H - 3,
	     status_x1 - status_x0, LINE_H, paper);

	draw_text(status_left, status_row, "starting in", dim);
	draw_dec(status_left + 12, status_row, seconds, accent);
	draw_text(status_left + 14, status_row,
		  seconds == 1 ? "second" : "seconds", dim);
}

/* Says what happened, once, on the serial console -- which is where the
 * verification rig is listening and where a screenshot cannot reach. */
void gfx_report(BOOLEAN used)
{
	if (used)
		print("  menu         : drawn\n");
	else
		print("  menu         : text (no framebuffer this loader can "
		      "draw into)\n");
}
