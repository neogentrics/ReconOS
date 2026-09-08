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

static void draw_char(UINT32 x0, UINT32 y0, char c, UINT32 ink)
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
				for (sy = 0; sy < SCALE; sy++)
					for (sx = 0; sx < SCALE; sx++)
						put_pixel(x0 + gx * SCALE + sx,
							  y0 + gy * SCALE + sy,
							  ink);
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
 * clock. Drawn in full each time rather than patched, because a partial redraw
 * that gets its arithmetic wrong leaves the previous frame's text underneath
 * the new one, and nobody looking at the result can tell which is which. */
void gfx_menu_draw(unsigned count, const char *const *labels,
		   unsigned selected, unsigned seconds)
{
	UINT32 ink    = pack(0xD8, 0xDC, 0xE0);
	UINT32 dim    = pack(0x70, 0x78, 0x80);
	UINT32 accent = pack(0xE0, 0xA8, 0x40);
	UINT32 paper  = pack(0x0C, 0x0E, 0x10);
	UINT32 bar    = pack(0x1E, 0x24, 0x2A);
	UINT32 cols   = screen.width / CELL_W;
	UINT32 first  = (screen.height / LINE_H - (count + 6)) / 2;
	UINT32 left   = (cols - 34) / 2;
	unsigned i;

	if (!screen.ready)
		return;

	fill(0, 0, screen.width, screen.height, paper);

	draw_text(left, first, "ReconOS", accent);
	draw_text(left + 8, first, "-- choose what to start", dim);

	for (i = 0; i < count; i++) {
		UINT32 row = first + 2 + i;

		if (i == selected) {
			/* The whole line, so the eye finds it without reading
			 * anything. */
			fill((left - 2) * CELL_W, row * LINE_H - 3,
			     36 * CELL_W, LINE_H, bar);
			draw_text(left - 1, row, ">", accent);
		}

		draw_dec(left + 1, row, i + 1, dim);
		draw_text(left + 2, row, ".", dim);
		draw_text(left + 4, row, labels[i],
			  i == selected ? ink : dim);
	}

	draw_text(left, first + count + 3, "starting in", dim);
	draw_dec(left + 12, first + count + 3, seconds, accent);
	draw_text(left + 14, first + count + 3,
		  seconds == 1 ? "second" : "seconds", dim);

	draw_text(left, first + count + 4,
		  "a number chooses, any key waits", dim);
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
