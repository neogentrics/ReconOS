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

/* Drawn at double size: an 8x8 glyph in a 16x16 cell, so a 1280x800 screen is
 * 80 columns by 50 rows -- the shape of a text console. */
#define SCALE	2
#define CELL_W	(RECON_FONT_WIDTH * SCALE)
#define CELL_H	(RECON_FONT_HEIGHT * SCALE)

/* The shadow, and therefore the largest screen this will use. A display bigger
 * than this is not refused -- it is used, at this many characters, with the
 * rest of the glass left dark. Refusing to print anything because the screen is
 * large would be the wrong way round. */
#define MAX_COLS 200
#define MAX_ROWS 64

static struct {
	volatile u8 *pixels;
	u32 pitch;
	u32 width;
	u32 height;
	enum fb_format format;

	unsigned cols, rows;
	unsigned col, row;

	char shadow[MAX_ROWS][MAX_COLS];
	bool ready;
} fb;

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
	u32 x0 = col * CELL_W;
	u32 y0 = row * CELL_H;
	u32 ink = pack(0xD0, 0xD4, 0xD8);
	u32 paper = pack(0x0C, 0x0E, 0x10);
	const unsigned char *g;
	unsigned gy, gx, sy, sx;

	fill(x0, y0, CELL_W, CELL_H, paper);

	if ((unsigned char)c < RECON_FONT_FIRST ||
	    (unsigned char)c > RECON_FONT_LAST) {
		if (c == ' ' || c == 0)
			return;
		/* The box. */
		fill(x0 + 2, y0 + 2, CELL_W - 4, 2, ink);
		fill(x0 + 2, y0 + CELL_H - 4, CELL_W - 4, 2, ink);
		fill(x0 + 2, y0 + 2, 2, CELL_H - 4, ink);
		fill(x0 + CELL_W - 4, y0 + 2, 2, CELL_H - 4, ink);
		return;
	}

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

void fbcon_init(void)
{
	const struct boot_info *info = boot_info();
	unsigned r, c;

	fb.ready = false;

	/* No framebuffer, or one whose layout the loader could not name. Either
	 * way this console does not start, and the serial port carries
	 * everything as it did before. */
	if (!info->fb.width || !info->fb.height || !info->fb.base ||
	    info->fb.format == FB_FORMAT_NONE)
		return;

	/* A pitch that cannot hold a row is a framebuffer description this
	 * kernel does not believe. Drawing into it would write past the end of
	 * every line, which on a device mapping is somebody else's registers. */
	if (info->fb.pitch < info->fb.width * 4)
		return;

	fb.pixels = (volatile u8 *)phys_to_virt(info->fb.base);
	fb.pitch  = info->fb.pitch;
	fb.width  = info->fb.width;
	fb.height = info->fb.height;
	fb.format = info->fb.format;

	fb.cols = fb.width / CELL_W;
	fb.rows = fb.height / CELL_H;

	if (fb.cols > MAX_COLS)
		fb.cols = MAX_COLS;
	if (fb.rows > MAX_ROWS)
		fb.rows = MAX_ROWS;

	if (!fb.cols || !fb.rows)
		return;

	for (r = 0; r < fb.rows; r++)
		for (c = 0; c < fb.cols; c++)
			fb.shadow[r][c] = ' ';

	fb.col = 0;
	fb.row = 0;
	fb.ready = true;

	fill(0, 0, fb.width, fb.height, pack(0x0C, 0x0E, 0x10));
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
}
