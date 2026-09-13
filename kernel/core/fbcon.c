/* The console you can see without a serial cable.
 *
 * Until this existed, everything the kernel said went to a serial port. That is
 * fine for the verification rig, which is holding the other end of the cable,
 * and it is useless on a machine somebody has just installed ReconOS onto: the
 * screen stops at the bootloader's last line and the recovery report, the boot
 * log and the fault reporter are all somewhere nobody is looking.
 *
 * --- What it draws into ------------------------------------------------------
 *
 * The framebuffer the loader obtained from the firmware and passed through the
 * handoff, mapped by vm_init() as uncached device memory in the direct map --
 * uncached because a write that sits in a cache line is a pixel that does not
 * appear.
 *
 * Where there is no framebuffer this does nothing at all, and that is a normal
 * answer rather than a failure: AAVMF provides none, so half the boot paths in
 * the verification matrix take this branch on every run. **The absence is the
 * tested case**, which is the only reason to trust it will hold on a machine
 * that has no display either.
 *
 * --- Scrolling, and why it redraws rather than copies -------------------------
 *
 * A framebuffer is memory across a bus, and reading it back is far slower than
 * writing to it. Scrolling by copying rows means reading the whole screen; this
 * keeps a shadow copy of the characters in ordinary RAM and redraws from that,
 * so a scroll is writes only. The shadow costs four kilobytes and buys back
 * every read.
 */
#include <recon/kernel/boot.h>
#include <recon/kernel/console.h>
#include <recon/kernel/fbcon.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/vm.h>

#include <recon_font.h>

/* **The glyph is scaled to the panel, not to a constant.**
 *
 * This was a fixed doubling, which is right on the 1280x800 the UEFI paths
 * come up at and wrong at both ends of the range the kernel now has to cope
 * with. An 8x8 glyph doubled is sixteen pixels tall: on a 4320-line 8K panel
 * that is a fortieth of the screen height and unreadable from a desk, and the
 * character grid would be 480x270 -- most of it clamped away and left dark. On
 * a small 480-line panel the same doubling leaves thirty rows.
 *
 * So the scale is chosen from the height to keep the console at roughly the
 * same *apparent* size on any screen. 800 pixels still comes out at exactly
 * two, which is what keeps every path that already had a framebuffer looking
 * precisely as it did.
 */
#define TARGET_ROWS	50
#define SCALE_MAX	8

/* **The area the console will draw into, however large the screen is.**
 *
 * A scroll redraws every cell, so its cost is the console's area in pixels and
 * nothing else. At 1280x800 that is a million writes and unnoticeable. At
 * 7680x4320 it is thirty-three million, through uncached device memory, per
 * scrolled line -- which is seconds, and which broke a timing assertion in a
 * subsystem that had nothing to do with the screen (KF-203).
 *
 * So the console occupies a window and leaves the rest dark. Roughly 1080p:
 * enough for eighty columns of readable text on any panel, and a bounded cost
 * on all of them. The glass beyond it is what a compositor is for. */
#define CONSOLE_MAX_W	1920u
#define CONSOLE_MAX_H	1200u

/* The shadow, and therefore the largest grid this will use. A display bigger
 * than this is not refused -- it is used, at this many characters, with the
 * rest of the glass left dark. Refusing to print anything because the screen is
 * large would be the wrong way round.
 *
 * These are far less likely to bite now the glyph scales with the panel: 8K at
 * scale 8 is a 120x67 grid rather than the 480x270 a fixed doubling would have
 * asked for. The margin above that is for shapes rather than sizes -- an
 * ultra-wide is short and very wide, a phone panel held in portrait is the
 * other way round, and neither is unusual enough to clip. */
#define MAX_COLS 240
#define MAX_ROWS 80

static struct {
	volatile u8 *pixels;
	u32 pitch;
	u32 width;
	u32 height;
	enum fb_format format;

	unsigned draw_w, draw_h;	/* the console's window, not the panel */
	unsigned scale;			/* pixels per glyph pixel */
	unsigned cell_w, cell_h;	/* a character cell, in screen pixels */

	unsigned cols, rows;
	unsigned col, row;

	char shadow[MAX_ROWS][MAX_COLS];
	bool ready;

	/* The screen as it was handed over, kept whole.
	 *
	 * Everything above is the console's own view of it -- a window, a grid, a
	 * scale. This is the panel, and it is what anything that is not the
	 * console needs: /dev/fb0 offers the whole screen, not the eighty columns
	 * of text drawn in the corner of it. */
	struct framebuffer screen;
} fb;

/* How far to magnify the font on a screen this tall.
 *
 * Integer, because a glyph drawn at a fractional scale is a glyph with uneven
 * strokes -- and this font is eight pixels of bitmap, which has nothing to
 * spare. At least one, so a very small panel still gets text rather than
 * nothing. */
static unsigned scale_for(u32 height)
{
	unsigned s = height / (RECON_FONT_HEIGHT * TARGET_ROWS);

	if (s < 1)
		s = 1;
	if (s > SCALE_MAX)
		s = SCALE_MAX;

	return s;
}

/* Colours as they are written, not as they are named.
 *
 * Both layouts are four bytes with the same three channels in a different
 * order, so one value per format is enough. A format this kernel does not
 * understand never reaches here: the loader reports it as none, and none means
 * this console does not start. */
/* The channel order is a statement about *bytes in memory*, and this writes a
 * 32-bit word on a little-endian machine -- so the first byte is the low eight
 * bits, and BGRA means blue is at the bottom and red at bits 16..23.
 *
 * These two cases were the wrong way round here and in the loader, and it was
 * invisible in both: this console draws grey on near-black, and a swap of red
 * and blue does nothing to a colour whose red and blue are equal. It surfaced
 * on the first menu that drew amber, which came out blue.
 */
static u32 pack(u8 r, u8 g, u8 b)
{
	if (fb.format == FB_FORMAT_RGBA)
		return ((u32)b << 16) | ((u32)g << 8) | r;

	return ((u32)r << 16) | ((u32)g << 8) | b;	/* BGRA */
}

static void put_pixel(u32 x, u32 y, u32 colour)
{
	volatile u32 *p;

	if (x >= fb.width || y >= fb.height)
		return;

	p = (volatile u32 *)(fb.pixels + (u64)y * fb.pitch + (u64)x * 4);
	*p = colour;
}

static void fill(u32 x0, u32 y0, u32 w, u32 h, u32 colour)
{
	u32 x, y;

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			put_pixel(x0 + x, y0 + y, colour);
}

/* One character, at a character cell. Anything outside the font draws as a
 * hollow box: a visible question, rather than a blank that reads as though
 * nothing was printed. */
static void draw_glyph(unsigned col, unsigned row, char c)
{
	u32 x0 = col * fb.cell_w;
	u32 y0 = row * fb.cell_h;
	u32 ink = pack(0xD0, 0xD4, 0xD8);
	u32 paper = pack(0x0C, 0x0E, 0x10);
	const unsigned char *g;
	unsigned gy, gx, sy, sx;

	fill(x0, y0, fb.cell_w, fb.cell_h, paper);

	if ((unsigned char)c < RECON_FONT_FIRST ||
	    (unsigned char)c > RECON_FONT_LAST) {
		if (c == ' ' || c == 0)
			return;
		/* The box. */
		fill(x0 + 2, y0 + 2, fb.cell_w - 4, 2, ink);
		fill(x0 + 2, y0 + fb.cell_h - 4, fb.cell_w - 4, 2, ink);
		fill(x0 + 2, y0 + 2, 2, fb.cell_h - 4, ink);
		fill(x0 + fb.cell_w - 4, y0 + 2, 2, fb.cell_h - 4, ink);
		return;
	}

	g = recon_font[(unsigned char)c - RECON_FONT_FIRST];

	for (gy = 0; gy < RECON_FONT_HEIGHT; gy++)
		for (gx = 0; gx < RECON_FONT_WIDTH; gx++)
			if (g[gy] & (0x80u >> gx))
				for (sy = 0; sy < fb.scale; sy++)
					for (sx = 0; sx < fb.scale; sx++)
						put_pixel(x0 + gx * fb.scale + sx,
							  y0 + gy * fb.scale + sy,
							  ink);
}

static void redraw(void)
{
	unsigned r, c;

	for (r = 0; r < fb.rows; r++)
		for (c = 0; c < fb.cols; c++)
			draw_glyph(c, r, fb.shadow[r][c]);
}

static void scroll(void)
{
	unsigned r, c;

	for (r = 1; r < fb.rows; r++)
		for (c = 0; c < fb.cols; c++)
			fb.shadow[r - 1][c] = fb.shadow[r][c];

	for (c = 0; c < fb.cols; c++)
		fb.shadow[fb.rows - 1][c] = ' ';

	redraw();
	fb.row = fb.rows - 1;
}

/* Point the console at a framebuffer and start drawing into it.
 *
 * Two callers, and they arrive at different times on purpose. `fbcon_init`
 * passes what the handoff carried, before there is a PCI bus to ask about
 * anything. The display driver passes a mode it set itself, after the bus walk,
 * on a machine where firmware left no framebuffer at all -- which is every PVH
 * and direct-kernel path in the verification matrix.
 *
 * Returns false where the description is one this console will not draw into,
 * leaving whatever was there before untouched: a refused framebuffer must not
 * cost the caller the screen it already had.
 */
bool fbcon_adopt(const struct framebuffer *given)
{
	unsigned r, c;

	/* No framebuffer, or one whose layout the loader could not name. Either
	 * way this console does not start, and the serial port carries
	 * everything as it did before. */
	if (!given || !given->width || !given->height || !given->base ||
	    given->format == FB_FORMAT_NONE)
		return false;

	/* A pitch that cannot hold a row is a framebuffer description this
	 * kernel does not believe. Drawing into it would write past the end of
	 * every line, which on a device mapping is somebody else's registers. */
	if (given->pitch < given->width * 4)
		return false;

	{
		/* Scaled and counted against the *window*, not the panel. A
		 * bigger screen past this point gains resolution for whatever
		 * draws next; it does not gain console. */
		unsigned draw_w = given->width  < CONSOLE_MAX_W
				  ? given->width  : CONSOLE_MAX_W;
		unsigned draw_h = given->height < CONSOLE_MAX_H
				  ? given->height : CONSOLE_MAX_H;
		unsigned scale = scale_for(draw_h);
		unsigned cols = draw_w / (RECON_FONT_WIDTH * scale);
		unsigned rows = draw_h / (RECON_FONT_HEIGHT * scale);

		/* Checked before anything is changed, so a framebuffer too
		 * small to hold one character does not leave the console
		 * pointing at it with nothing drawable. */
		if (!cols || !rows)
			return false;

		fb.scale  = scale;
		fb.cell_w = RECON_FONT_WIDTH * scale;
		fb.cell_h = RECON_FONT_HEIGHT * scale;
		fb.draw_w = draw_w;
		fb.draw_h = draw_h;
	}

	fb.ready = false;

	fb.pixels = (volatile u8 *)phys_to_virt(given->base);
	fb.pitch  = given->pitch;
	fb.width  = given->width;
	fb.height = given->height;
	fb.format = given->format;

	fb.screen = *given;
	fb.cols = fb.draw_w / fb.cell_w;
	fb.rows = fb.draw_h / fb.cell_h;

	if (fb.cols > MAX_COLS)
		fb.cols = MAX_COLS;
	if (fb.rows > MAX_ROWS)
		fb.rows = MAX_ROWS;

	for (r = 0; r < fb.rows; r++)
		for (c = 0; c < fb.cols; c++)
			fb.shadow[r][c] = ' ';

	fb.col = 0;
	fb.row = 0;
	fb.ready = true;

	/* Only the window is painted. Clearing thirty-three million pixels to
	 * show eighty columns of text is most of the cost this bound exists to
	 * remove, and it would be paid on every mode change. */
	fill(0, 0, fb.draw_w, fb.draw_h, pack(0x0C, 0x0E, 0x10));
	return true;
}

void fbcon_init(void)
{
	fb.ready = false;
	fbcon_adopt(&boot_info()->fb);
}

void fbcon_putc(char c)
{
	if (!fb.ready)
		return;

	if (c == '\r') {
		fb.col = 0;
		return;
	}

	if (c == '\n') {
		fb.col = 0;
		if (++fb.row >= fb.rows)
			scroll();
		return;
	}

	if (c == '\t') {
		unsigned n = 8 - (fb.col % 8);

		while (n--)
			fbcon_putc(' ');
		return;
	}

	if (fb.col >= fb.cols) {
		fb.col = 0;
		if (++fb.row >= fb.rows)
			scroll();
	}

	fb.shadow[fb.row][fb.col] = c;
	draw_glyph(fb.col, fb.row, c);
	fb.col++;
}

const struct framebuffer *fbcon_framebuffer(void)
{
	/* `ready` and not `screen.base`: the description is only meaningful once
	 * this console has checked it, and it checks several things a caller
	 * would otherwise have to check again -- a pitch too small for a row, a
	 * format nobody named. Handing out a description that failed those is
	 * handing out a framebuffer nothing should draw into. */
	return fb.ready ? &fb.screen : 0;
}

bool fbcon_active(void)
{
	return fb.ready;
}

void fbcon_describe(void)
{
	if (!fb.ready) {
		kputs("  screen       : none -- serial only\n");
		return;
	}

	kprintf("  screen       : %ux%u, %u columns by %u rows\n",
		fb.width, fb.height, fb.cols, fb.rows);

	/* Said, rather than left to be worked out from a grid that does not
	 * cover the panel. A dark screen with text in one corner looks like a
	 * fault unless somebody says it is not. */
	if (fb.draw_w < fb.width || fb.draw_h < fb.height)
		kprintf("               : the console uses %ux%u of that; a "
			"scroll redraws its area and nothing else\n",
			fb.draw_w, fb.draw_h);
}
