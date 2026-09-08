/*
 * The default icon set, drawn by ReconOS and written to /System/Icons.
 *
 * Generated rather than shipped, for two reasons. It means the system has a
 * complete set of icons the moment it first runs, with nothing to install and
 * nothing borrowed. And because they are written out as ordinary files, every
 * one of them can be replaced by dropping a different file over it -- the
 * generated set is a starting point, not a fixed part of the build.
 *
 * They are drawn in the chunky, high-contrast idiom of the era ReconOS is
 * styled after: flat colour, hard edges, a light source at the top left.
 *
 * --- And a second set, with a gloss on it ---
 *
 * Every icon is also written a second time with a curved-glass treatment, into
 * a subdirectory, and a skin says which set it wants. Two sets rather than one
 * treatment applied to everything: the flat idiom is what Classic and Recon are
 * *for*, and 95 did not gleam.
 *
 * Two sets rather than glossing on the way to the screen, as well. These are
 * files, and the reason they are files is that any one of them can be replaced
 * by dropping a different image over it. A gloss applied at draw time would be
 * applied to the replacement too, which is the one thing a replaced icon exists
 * to avoid.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_avatar.h"
#include "recon_registry.h"
#include "recon_fs.h"
#include "recon_icon_gen.h"

/*
 * --- Two sizes, and the difference between them is the whole point ---
 *
 * ICON_SIZE is the *coordinate space* every drawing function below works in.
 * It has always been 32 and stays 32, so every rectangle and disc in this file
 * keeps the numbers it was written with.
 *
 * ICON_PIXELS is what actually gets written to disk, four times larger.
 *
 * The reason is what happens at the other end. recon_draw_image averages when
 * it shrinks an image and takes the nearest pixel when it grows one -- and
 * growing is right for pixel art, where blurring upward is worse than the
 * steps. But the login screen draws an account picture at around seventy
 * pixels, and a 32-pixel source grown to seventy means every pixel of the
 * drawing becomes a 2.25-pixel block. That is exactly the "pixelated image
 * made in paint" it was described as, and no amount of redrawing at 32 would
 * have fixed it, because the fault is not in the drawing.
 *
 * At 128 every size ReconOS asks for -- 16 in a menu, 22 on a task button, 32
 * on the desktop, 72 on the login screen -- is a *shrink*, and the averaging
 * path that already exists handles all of them. The upscale is never reached.
 *
 * It also makes the round things round. A disc drawn at 128 and averaged down
 * to 32 has a soft edge; the same disc drawn at 32 has a staircase, and a
 * staircase on a circle is the single clearest signal that something was made
 * pixel by pixel.
 *
 * Costs sixteen times the memory of one icon -- 64KB, held one at a time --
 * and runs once, when the filesystem is built.
 */
#define ICON_SIZE 32
#define ICON_SCALE 4
#define ICON_PIXELS (ICON_SIZE * ICON_SCALE)

/* Packed 0xAARRGGBB, matching the UI layer's colours. */
typedef uint32_t color;

#define RGB(r, g, b) ((color)(0xFF000000u | ((r) << 16) | ((g) << 8) | (b)))
#define CLEAR 0u

#define C_FOLDER RGB(0xE8, 0xC8, 0x50)
#define C_FOLDER_DARK RGB(0xB8, 0x98, 0x28)
#define C_FOLDER_LIGHT RGB(0xF8, 0xE0, 0x90)
#define C_PAPER RGB(0xFC, 0xFC, 0xFC)
#define C_PAPER_EDGE RGB(0xA0, 0xA0, 0xA0)
#define C_INK RGB(0x50, 0x50, 0x60)
#define C_OUTLINE RGB(0x20, 0x20, 0x28)
#define C_SCREEN RGB(0x10, 0x18, 0x14)
#define C_PHOSPHOR RGB(0x70, 0xE0, 0x70)
#define C_METAL RGB(0xC0, 0xC0, 0xC8)
#define C_METAL_DARK RGB(0x80, 0x80, 0x90)
#define C_ACCENT RGB(0x8B, 0x1A, 0x1A)
#define C_BLUE RGB(0x28, 0x48, 0x98)
#define C_BLUE_LIGHT RGB(0x58, 0x80, 0xD0)
#define C_KEY RGB(0xE0, 0xE0, 0xE0)
/* A key that has to read against a grey panel, which C_KEY does not. */
#define C_BRASS RGB(0xE8, 0xC0, 0x50)

/* --- The gloss --- */

/*
 * Move a colour towards white or black by a fraction in 256ths.
 *
 * Towards, not to: a highlight that reaches white loses the colour underneath
 * it, and an icon whose top half is a white smear is not a shiny icon, it is a
 * damaged one.
 */
static color shade(color c, int towards_white, int towards_black) {
    int r = (int)((c >> 16) & 0xFF);
    int g = (int)((c >> 8) & 0xFF);
    int b = (int)(c & 0xFF);

    r += ((255 - r) * towards_white) >> 8;
    g += ((255 - g) * towards_white) >> 8;
    b += ((255 - b) * towards_white) >> 8;

    r -= (r * towards_black) >> 8;
    g -= (g * towards_black) >> 8;
    b -= (b * towards_black) >> 8;

    return (c & 0xFF000000u) | ((color)r << 16) | ((color)g << 8) | (color)b;
}

/*
 * A curved, lit surface over whatever has already been drawn.
 *
 * Three things, and each is doing a different job:
 *
 *   - a vertical ramp, lighter at the top and darker at the bottom, which is
 *     what makes a flat shape read as curved rather than as tilted;
 *   - a specular highlight, the bottom arc of a large ellipse centred well
 *     above the icon, so it crosses the top third the way a reflection on a
 *     convex surface does;
 *   - a single lighter row along the very top of the shape, which is the rim
 *     of the glass catching the light.
 *
 * Applied only where something has been drawn. Following the icon's own alpha
 * rather than a rectangle is the whole difference between a glossy icon and an
 * icon with a glossy box behind it.
 */
static void gloss(color *px) {
    for (int y = 0; y < ICON_PIXELS; y++) {
        for (int x = 0; x < ICON_PIXELS; x++) {
            color c = px[y * ICON_PIXELS + x];
            if ((c >> 24) == 0) {
                continue;              /* nothing here to light */
            }

            /*
             * The ramp. Strongest at the very top and the very bottom, nothing
             * across the middle, so the two halves meet without a seam.
             */
            int light = 0, dark = 0;
            if (y < ICON_PIXELS / 2) {
                light = 56 * (ICON_PIXELS / 2 - y) / (ICON_PIXELS / 2);
            } else {
                dark = 40 * (y - ICON_PIXELS / 2) / (ICON_PIXELS / 2);
            }

            /*
             * The highlight, as an ellipse centred eleven rows above the icon.
             * Only its lower arc reaches the pixels, which is why the centre is
             * off the top: a highlight centred *on* the icon is a circle in the
             * middle of it, and reads as a hole rather than as a reflection.
             *
             * Integer throughout, scaled by 1024, because this runs at build-
             * of-the-filesystem time on a machine that may have no floating
             * point worth relying on -- and because the shape is not sensitive
             * enough to need any more precision than this.
             */
            /* The ellipse in real pixels: every constant here was measured
             * against a 32-wide icon, so each is scaled with the canvas
             * rather than retuned -- the shape is the same shape. */
            int dx = (x - ICON_PIXELS / 2) * 1024 / (19 * ICON_SCALE);
            int dy = (y + 11 * ICON_SCALE) * 1024 / (21 * ICON_SCALE);
            int inside = 1024 * 1024 - (dx * dx + dy * dy);
            if (inside > 0) {
                /* Falls off towards the arc rather than stopping at it, so the
                 * edge of the highlight is an edge of light and not a line. */
                light += 58 * inside / (1024 * 1024);
            }

            /* The rim: the top edge of this column, as thick as one
             * coordinate-space pixel. A one-real-pixel rim at this size is a
             * hairline that averages away to nothing, which is the same as
             * not having drawn it. */
            bool rim = (y < ICON_SCALE)
                || ((px[(y - ICON_SCALE) * ICON_PIXELS + x] >> 24) == 0);
            if (rim) {
                light += 46;
            }

            if (light > 200) {
                light = 200;
            }
            px[y * ICON_PIXELS + x] = shade(c, light, dark);
        }
    }
}

/* --- Drawing --- */

/*
 * One coordinate-space pixel: a block of ICON_SCALE by ICON_SCALE real ones.
 *
 * Keeping the coarse coordinate space is what lets every drawing function in
 * this file stay exactly as it was written. A rectangle at 32 is a rectangle
 * at 128 -- its edges are axis-aligned, so nothing is gained by describing it
 * more finely. What *is* gained at 128 happens in fill_disc below, where the
 * edge is not axis-aligned and the extra resolution is the difference between
 * a circle and a staircase.
 */
static void plot(color *px, int x, int y, color c) {
    if (x < 0 || y < 0 || x >= ICON_SIZE || y >= ICON_SIZE) {
        return;
    }
    for (int row = 0; row < ICON_SCALE; row++) {
        for (int col = 0; col < ICON_SCALE; col++) {
            px[(y * ICON_SCALE + row) * ICON_PIXELS + x * ICON_SCALE + col] = c;
        }
    }
}

/* The same, addressing a real pixel rather than a block. For the few things
 * that want the finer grid -- which means the curves. */
static void plot_fine(color *px, int x, int y, color c) {
    if (x >= 0 && y >= 0 && x < ICON_PIXELS && y < ICON_PIXELS) {
        px[y * ICON_PIXELS + x] = c;
    }
}

static void fill_rect(color *px, int x, int y, int w, int h, color c) {
    for (int row = y; row < y + h; row++) {
        for (int col = x; col < x + w; col++) {
            plot(px, col, row, c);
        }
    }
}

static void stroke_rect(color *px, int x, int y, int w, int h, color c) {
    for (int col = x; col < x + w; col++) {
        plot(px, col, y, c);
        plot(px, col, y + h - 1, c);
    }
    for (int row = y; row < y + h; row++) {
        plot(px, x, row, c);
        plot(px, x + w - 1, row, c);
    }
}

/*
 * A filled disc, for anything round -- drawn on the fine grid.
 *
 * This is where the four times pays for itself. A disc tested block by block
 * has a staircase four real pixels tall on every step, which is the thing that
 * makes a drawn icon look drawn. Tested pixel by pixel it has a staircase one
 * real pixel tall, and one pixel of stair at 128 averages away to nothing by
 * the time anything asks for it at 32 or at 72.
 *
 * The arguments stay in the coarse space, so every existing caller is
 * unchanged and every existing icon comes out where it was.
 */
static void fill_disc(color *px, int cx, int cy, int radius, color c) {
    int fcx = cx * ICON_SCALE + ICON_SCALE / 2;
    int fcy = cy * ICON_SCALE + ICON_SCALE / 2;
    int fr = radius * ICON_SCALE;

    for (int y = fcy - fr; y <= fcy + fr; y++) {
        for (int x = fcx - fr; x <= fcx + fr; x++) {
            int dx = x - fcx;
            int dy = y - fcy;
            if (dx * dx + dy * dy <= fr * fr) {
                plot_fine(px, x, y, c);
            }
        }
    }
}

/*
 * The overlap of two discs: a pointed oval, which is the shape of a leaf and
 * of nothing else that is easy to draw with rectangles.
 *
 * On the fine grid, like fill_disc, because both of its edges are curved and
 * both of its ends come to a point -- and a point drawn in blocks is not a
 * point, it is a corner.
 */
static void fill_lens(color *px, int ax, int ay, int bx, int by, int radius,
        color c) {
    int fax = ax * ICON_SCALE + ICON_SCALE / 2;
    int fay = ay * ICON_SCALE + ICON_SCALE / 2;
    int fbx = bx * ICON_SCALE + ICON_SCALE / 2;
    int fby = by * ICON_SCALE + ICON_SCALE / 2;
    int fr = radius * ICON_SCALE;

    for (int y = 0; y < ICON_PIXELS; y++) {
        for (int x = 0; x < ICON_PIXELS; x++) {
            int dax = x - fax, day = y - fay;
            int dbx = x - fbx, dby = y - fby;
            if (dax * dax + day * day <= fr * fr &&
                    dbx * dbx + dby * dby <= fr * fr) {
                plot_fine(px, x, y, c);
            }
        }
    }
}

/*
 * An arc: the part of a ring between two angles, given as a cone rather than
 * in degrees.
 *
 * `spread` is how far to either side of straight up the arc reaches, measured
 * as a fraction of the radius -- so an arc drawn at several radii keeps the
 * same *shape*, which is what makes a stack of them read as a signal spreading
 * out rather than as three unrelated curves. The first attempt at this walked
 * x and solved for y, which flattens badly near the ends and produced
 * something that looked like a palm tree.
 */
static void stroke_arc(color *px, int cx, int cy, int radius, int thickness,
        int spread_num, int spread_den, color c) {
    int fcx = cx * ICON_SCALE + ICON_SCALE / 2;
    int fcy = cy * ICON_SCALE + ICON_SCALE / 2;
    int fr = radius * ICON_SCALE;
    int ft = thickness * ICON_SCALE;

    long long inner = (long long)(fr - ft) * (fr - ft);
    long long outer = (long long)fr * fr;

    for (int y = fcy - fr; y <= fcy; y++) {
        for (int x = fcx - fr; x <= fcx + fr; x++) {
            int dx = x - fcx;
            int dy = y - fcy;             /* negative: above the centre */
            long long d = (long long)dx * dx + (long long)dy * dy;
            if (d > outer || d < inner) {
                continue;
            }
            /* Inside the cone when the sideways distance is small enough
             * relative to how far up we are. */
            if (abs(dx) * spread_den > -dy * spread_num) {
                continue;
            }
            plot_fine(px, x, y, c);
        }
    }
}

/* --- The icons --- */

static void draw_folder(color *px) {
    /* Tab, then body, with a lit top edge and a shaded bottom. */
    fill_rect(px, 3, 7, 11, 3, C_FOLDER_DARK);
    fill_rect(px, 3, 10, 26, 15, C_FOLDER);
    fill_rect(px, 3, 10, 26, 2, C_FOLDER_LIGHT);
    fill_rect(px, 3, 23, 26, 2, C_FOLDER_DARK);
    stroke_rect(px, 3, 7, 11, 4, C_OUTLINE);
    stroke_rect(px, 3, 10, 26, 15, C_OUTLINE);
}

static void draw_page(color *px, bool with_lines) {
    fill_rect(px, 7, 4, 18, 24, C_PAPER);
    stroke_rect(px, 7, 4, 18, 24, C_PAPER_EDGE);

    /* A folded corner, drawn as a stepped triangle. */
    for (int i = 0; i < 6; i++) {
        for (int j = 0; j < 6 - i; j++) {
            plot(px, 24 - j, 4 + i, CLEAR);
        }
        plot(px, 24 - (6 - i) + 1, 4 + i, C_PAPER_EDGE);
    }
    fill_rect(px, 19, 4, 6, 1, C_PAPER_EDGE);

    if (with_lines) {
        for (int i = 0; i < 5; i++) {
            fill_rect(px, 10, 13 + i * 3, 12, 1, C_INK);
        }
    }
}

static void draw_terminal(color *px) {
    fill_rect(px, 2, 5, 28, 22, C_METAL);
    fill_rect(px, 2, 5, 28, 1, RGB(0xF0, 0xF0, 0xF8));
    fill_rect(px, 2, 26, 28, 1, C_METAL_DARK);
    stroke_rect(px, 2, 5, 28, 22, C_OUTLINE);

    fill_rect(px, 5, 8, 22, 16, C_SCREEN);
    stroke_rect(px, 5, 8, 22, 16, C_OUTLINE);

    /* A prompt and a cursor. */
    fill_rect(px, 8, 12, 2, 2, C_PHOSPHOR);
    fill_rect(px, 10, 14, 2, 2, C_PHOSPHOR);
    fill_rect(px, 8, 16, 2, 2, C_PHOSPHOR);
    fill_rect(px, 14, 12, 8, 2, C_PHOSPHOR);
    fill_rect(px, 14, 16, 5, 2, C_PHOSPHOR);
}

static void draw_notepad(color *px) {
    draw_page(px, true);
    /* A spiral binding down the left edge. */
    fill_rect(px, 7, 4, 3, 24, RGB(0xE0, 0xE0, 0xE8));
    for (int i = 0; i < 6; i++) {
        fill_rect(px, 7, 7 + i * 4, 3, 2, C_METAL_DARK);
    }
    stroke_rect(px, 7, 4, 18, 24, C_PAPER_EDGE);
}

static void draw_calculator(color *px) {
    fill_rect(px, 6, 3, 20, 26, C_METAL);
    fill_rect(px, 6, 3, 20, 1, RGB(0xF0, 0xF0, 0xF8));
    fill_rect(px, 6, 28, 20, 1, C_METAL_DARK);
    stroke_rect(px, 6, 3, 20, 26, C_OUTLINE);

    /* Display. */
    fill_rect(px, 9, 6, 14, 5, C_SCREEN);
    fill_rect(px, 18, 8, 3, 1, C_PHOSPHOR);
    stroke_rect(px, 9, 6, 14, 5, C_OUTLINE);

    /* Keypad: three columns of grey, one of accent down the right. */
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            int x = 9 + col * 4;
            int y = 13 + row * 4;
            fill_rect(px, x, y, 3, 3, col == 3 ? C_ACCENT : C_KEY);
        }
    }
}

static void draw_explorer(color *px) {
    /* A folder with a window over it: browsing, rather than storing. */
    fill_rect(px, 2, 9, 11, 3, C_FOLDER_DARK);
    fill_rect(px, 2, 12, 24, 14, C_FOLDER);
    fill_rect(px, 2, 12, 24, 1, C_FOLDER_LIGHT);
    stroke_rect(px, 2, 9, 11, 4, C_OUTLINE);
    stroke_rect(px, 2, 12, 24, 14, C_OUTLINE);

    fill_rect(px, 13, 3, 17, 14, C_PAPER);
    fill_rect(px, 13, 3, 17, 4, C_BLUE);
    fill_rect(px, 13, 3, 17, 1, C_BLUE_LIGHT);
    stroke_rect(px, 13, 3, 17, 14, C_OUTLINE);
    for (int i = 0; i < 3; i++) {
        fill_rect(px, 16, 9 + i * 3, 11, 1, C_INK);
    }
}

static void draw_taskmanager(color *px) {
    fill_rect(px, 3, 4, 26, 24, C_PAPER);
    fill_rect(px, 3, 4, 26, 4, C_BLUE);
    fill_rect(px, 3, 4, 26, 1, C_BLUE_LIGHT);
    stroke_rect(px, 3, 4, 26, 24, C_OUTLINE);

    /* Bars, because what it shows is how much of something is being used. */
    static const int heights[] = { 6, 11, 8, 14, 5 };
    for (int i = 0; i < 5; i++) {
        int x = 6 + i * 4;
        int h = heights[i];
        fill_rect(px, x, 25 - h, 3, h, i == 3 ? C_ACCENT : C_BLUE);
    }
    fill_rect(px, 5, 25, 22, 1, C_OUTLINE);
}

static void draw_application(color *px) {
    /* A generic window, for anything without an icon of its own. */
    fill_rect(px, 4, 5, 24, 22, C_METAL);
    fill_rect(px, 4, 5, 24, 5, C_BLUE);
    fill_rect(px, 4, 5, 24, 1, C_BLUE_LIGHT);
    stroke_rect(px, 4, 5, 24, 22, C_OUTLINE);
    fill_rect(px, 24, 6, 3, 3, C_METAL);
    for (int i = 0; i < 3; i++) {
        fill_rect(px, 8, 14 + i * 4, 16, 2, C_METAL_DARK);
    }
}

/* The system mark on the Apps button: four panes, a window seen small. */
static void draw_system(color *px) {
    fill_rect(px, 4, 4, 24, 24, C_ACCENT);
    fill_rect(px, 4, 4, 24, 2, RGB(0xC0, 0x40, 0x40));
    fill_rect(px, 4, 26, 24, 2, RGB(0x60, 0x10, 0x10));
    fill_rect(px, 15, 6, 2, 20, RGB(0xF0, 0xE0, 0xE0));
    fill_rect(px, 6, 15, 20, 2, RGB(0xF0, 0xE0, 0xE0));
    stroke_rect(px, 4, 4, 24, 24, C_OUTLINE);
}

static void draw_shutdown(color *px) {
    /* The universal power mark: a ring broken at the top by a bar. */
    int cx = 16, cy = 17, r = 9;
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            int d2 = x * x + y * y;
            if (d2 <= r * r && d2 >= (r - 3) * (r - 3)) {
                if (y < -3 && x > -4 && x < 4) {
                    continue; /* the gap the bar sits in */
                }
                plot(px, cx + x, cy + y, C_ACCENT);
            }
        }
    }
    fill_rect(px, cx - 1, 5, 3, 10, C_ACCENT);
}

/* --- Writing --- */

/*
 * Write RGBA pixels as a 32-bit ICO.
 *
 * Two details of the format matter: rows are stored bottom to top, and the
 * declared height is doubled because a transparency mask follows the image.
 * The mask is written even though 32-bit images carry their own alpha, since
 * readers expect it to be there.
 */
static bool write_ico(const char *recon_path, const color *px) {
    int w = ICON_PIXELS, h = ICON_PIXELS;
    size_t mask_stride = (((size_t)w + 31) / 32) * 4;
    size_t image_bytes = (size_t)w * h * 4;
    size_t mask_bytes = mask_stride * (size_t)h;
    size_t dib_bytes = 40 + image_bytes + mask_bytes;
    size_t total = 6 + 16 + dib_bytes;

    unsigned char *out = calloc(1, total);
    if (out == NULL) {
        return false;
    }
    unsigned char *p = out;

    /* ICONDIR */
    p[2] = 1; /* type: icon */
    p[4] = 1; /* one image */
    p += 6;

    /*
     * ICONDIRENTRY.
     *
     * Width and height are single bytes here, and zero means 256 -- which is
     * why 128 is the largest useful canvas short of taking the 256 special
     * case. Both fit.
     */
    p[0] = (unsigned char)w;
    p[1] = (unsigned char)h;
    p[4] = 1;  /* planes */
    p[6] = 32; /* bits per pixel */
    p[8] = (unsigned char)(dib_bytes & 0xFF);
    p[9] = (unsigned char)((dib_bytes >> 8) & 0xFF);
    p[10] = (unsigned char)((dib_bytes >> 16) & 0xFF);
    p[12] = 22; /* offset to the image */
    p += 16;

    /* BITMAPINFOHEADER */
    unsigned char *dib = p;
    dib[0] = 40;

    /*
     * Written as full little-endian 32-bit fields rather than as low bytes.
     *
     * They were low bytes, which was correct for exactly as long as the canvas
     * was 32: the doubled height an ICO header carries is 256 at a 128-pixel
     * icon, and 256 in one byte is zero. A height of zero is not a small
     * mistake in a bitmap header -- it is an image every reader declines.
     */
    dib[4] = (unsigned char)(w & 0xFF);
    dib[5] = (unsigned char)((w >> 8) & 0xFF);
    dib[8] = (unsigned char)((h * 2) & 0xFF);
    dib[9] = (unsigned char)(((h * 2) >> 8) & 0xFF);
    dib[12] = 1;
    dib[14] = 32;
    p += 40;

    /* Pixels, bottom to top, as BGRA. */
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            color c = px[y * w + x];
            *p++ = (unsigned char)(c & 0xFF);
            *p++ = (unsigned char)((c >> 8) & 0xFF);
            *p++ = (unsigned char)((c >> 16) & 0xFF);
            *p++ = (unsigned char)((c >> 24) & 0xFF);
        }
    }

    /* The mask: a set bit means transparent. */
    for (int y = h - 1; y >= 0; y--) {
        unsigned char *row = p;
        for (int x = 0; x < w; x++) {
            if ((px[y * w + x] >> 24) == 0) {
                row[x / 8] |= (unsigned char)(0x80 >> (x % 8));
            }
        }
        p += mask_stride;
    }

    bool ok = recon_fs_write("/", recon_path, (const char *)out, total);
    free(out);
    return ok;
}

/* --- Entry point --- */

struct generated_icon {
    const char *name;
    void (*draw)(color *px);
};

/* --- A file, and then what kind of file --- */

/*
 * The accent every one of these carries.
 *
 * The same red the drawn icon set uses, so a generated icon and a drawn one
 * sit beside each other in a list without one of them looking imported.
 */
#define C_MARK RGB(0xC4, 0x1D, 0x21)
#define C_MARK_DARK RGB(0x10, 0x28, 0x6B)

static void draw_file(color *px) {
    draw_page(px, true);
}

/*
 * A level meter: three bars, the tallest in red.
 *
 * Not a musical note. The same icon has to serve a spoken recording, and a
 * note claims the file is music -- which is the sort of small lie an icon set
 * tells for years before anybody notices it is wrong about half its files.
 */
static void draw_file_sound(color *px) {
    draw_page(px, false);

    static const int HEIGHT[] = { 6, 12, 8 };
    for (int i = 0; i < 3; i++) {
        int x = 11 + i * 4;
        int h = HEIGHT[i];
        fill_rect(px, x, 22 - h, 3, h, i == 1 ? C_MARK : C_MARK_DARK);
    }
}

/*
 * Film perforations and a play triangle.
 *
 * The perforations do the work at small sizes: a triangle alone is a play
 * button, and a play button on a page could be sound just as easily. Sprocket
 * holes down an edge have meant film for a hundred years.
 */
static void draw_file_video(color *px) {
    draw_page(px, false);

    for (int i = 0; i < 4; i++) {
        fill_rect(px, 9, 10 + i * 4, 2, 2, C_MARK_DARK);
    }

    /* A triangle pointing right, a row at a time. */
    for (int row = 0; row < 11; row++) {
        int from_middle = row - 5;
        if (from_middle < 0) {
            from_middle = -from_middle;
        }
        int span = 8 - (from_middle * 8) / 5;
        if (span > 0) {
            fill_rect(px, 14, 11 + row, span, 1, C_MARK);
        }
    }
}

/* A horizon with a sun -- the same shape as the Photos icon, so a picture file
 * and the thing that opens it look like they know about each other. */
static void draw_file_image(color *px) {
    draw_page(px, false);

    fill_rect(px, 10, 12, 12, 10, RGB(0xD8, 0xE2, 0xF2));
    stroke_rect(px, 10, 12, 12, 10, C_MARK_DARK);

    /* The sun, as a small stepped disc. */
    fill_rect(px, 12, 14, 3, 3, C_MARK);
    fill_rect(px, 13, 13, 1, 5, C_MARK);

    /* And a ridge along the bottom of the frame. */
    for (int i = 0; i < 5; i++) {
        fill_rect(px, 13 + i, 21 - i, 1, i + 1, C_MARK_DARK);
        fill_rect(px, 21 - i, 21 - i, 1, i + 1, C_MARK_DARK);
    }
}

/* Angle brackets, which is what markup looks like to anybody who has seen
 * any. */
static void draw_file_web(color *px) {
    draw_page(px, false);

    for (int i = 0; i < 4; i++) {
        fill_rect(px, 12 - i, 16 - i, 2, 2, C_MARK_DARK);
        fill_rect(px, 12 - i, 16 + i, 2, 2, C_MARK_DARK);
        fill_rect(px, 19 + i, 16 - i, 2, 2, C_MARK);
        fill_rect(px, 19 + i, 16 + i, 2, 2, C_MARK);
    }
}

/* A brace, for the structured-text formats. */
static void draw_file_data(color *px) {
    draw_page(px, false);

    fill_rect(px, 13, 12, 2, 2, C_MARK_DARK);
    fill_rect(px, 12, 14, 2, 8, C_MARK_DARK);
    fill_rect(px, 13, 22, 2, 2, C_MARK_DARK);
    fill_rect(px, 10, 17, 2, 2, C_MARK_DARK);

    fill_rect(px, 18, 12, 2, 2, C_MARK);
    fill_rect(px, 19, 14, 2, 8, C_MARK);
    fill_rect(px, 18, 22, 2, 2, C_MARK);
    fill_rect(px, 21, 17, 2, 2, C_MARK);
}

/* A letter A. The only mark that says "font" at sixteen pixels. */
static void draw_file_font(color *px) {
    draw_page(px, false);

    for (int row = 0; row < 12; row++) {
        int spread = row / 2;
        fill_rect(px, 15 - spread, 11 + row, 2, 1, C_MARK_DARK);
        fill_rect(px, 16 + spread, 11 + row, 2, 1, C_MARK_DARK);
    }
    fill_rect(px, 13, 18, 7, 2, C_MARK);
}

/* A box with a band round it. */
static void draw_file_archive(color *px) {
    draw_page(px, false);

    fill_rect(px, 10, 13, 12, 9, RGB(0xD8, 0xC0, 0x88));
    stroke_rect(px, 10, 13, 12, 9, C_MARK_DARK);
    fill_rect(px, 15, 13, 2, 9, C_MARK);
}

/* --- The recycle bin --- */

#define C_BIN RGB(0x9A, 0xA4, 0xB0)
#define C_BIN_DARK RGB(0x6E, 0x78, 0x86)
#define C_BIN_LID RGB(0xB8, 0xC0, 0xCA)
#define C_BIN_EDGE RGB(0x2A, 0x2E, 0x36)
#define C_BIN_FULL RGB(0xD8, 0xC0, 0x60)

/*
 * A tapered bin with a lid, at 32x32.
 *
 * Two versions rather than one: an empty bin and a full one should be
 * distinguishable at a glance, which is most of the reason for having it on
 * the desktop at all.
 */
static void draw_bin(color *px, bool full) {
    const int lid_y = 8;
    const int body_top = 11;
    const int body_bottom = 28;

    if (full) {
        /* Something poking out from under the lid. */
        fill_rect(px, 10, 4, 5, 4, C_BIN_FULL);
        fill_rect(px, 16, 3, 4, 5, C_BIN_FULL);
        fill_rect(px, 13, 2, 4, 3, C_BIN_FULL);
    }

    /* Handle, then the lid across the top. */
    fill_rect(px, 13, lid_y - 2, 6, 2, C_BIN_LID);
    fill_rect(px, 4, lid_y, 24, 3, C_BIN_LID);
    stroke_rect(px, 4, lid_y, 24, 3, C_BIN_EDGE);

    /*
     * The body narrows towards the base a row at a time. A plain rectangle
     * reads as a box rather than a bin.
     */
    const int height = body_bottom - body_top;
    for (int row = 0; row < height; row++) {
        int inset = 6 + (row * 3) / height;
        int width = ICON_SIZE - inset * 2;
        if (width <= 2) {
            break;
        }

        /* Three vertical ribs, the way a moulded bin has. */
        color shade = C_BIN;
        fill_rect(px, inset, body_top + row, width, 1, shade);
        fill_rect(px, inset + width / 4, body_top + row, 1, 1, C_BIN_DARK);
        fill_rect(px, inset + width / 2, body_top + row, 1, 1, C_BIN_DARK);
        fill_rect(px, inset + (width * 3) / 4, body_top + row, 1, 1, C_BIN_DARK);

        /* Edges follow the taper. */
        plot(px, inset, body_top + row, C_BIN_EDGE);
        plot(px, inset + width - 1, body_top + row, C_BIN_EDGE);
    }

    /* The base. */
    fill_rect(px, 9, body_bottom - 1, 14, 1, C_BIN_EDGE);
}

static void draw_trash_empty(color *px) {
    draw_bin(px, false);
}

static void draw_trash_full(color *px) {
    draw_bin(px, true);
}

/*
 * --- Account pictures ---
 *
 * The set somebody picks from when they are asked what they should look like.
 *
 * Drawn, for the same reason the icons are: a system that generates its own
 * has a complete set the moment it first runs, with nothing shipped, nothing
 * borrowed and nobody's licence to honour. They are ordinary files in
 * /System/Icons like everything else, so a person who would rather use a
 * photograph drops one over the top and it stays.
 *
 * Each is a plain shape on a coloured disc. Simple on purpose: at the size a
 * login screen shows them, a detailed picture is a smudge, and these have to
 * be told apart at a glance by somebody choosing between them.
 */
static void avatar_disc(color *px, color background) {
    fill_disc(px, 16, 16, 15, background);
}

static void draw_avatar_mountain(color *px) {
    avatar_disc(px, RGB(0x3E, 0x6E, 0x8E));
    /* Two peaks and a snow line. */
    for (int i = 0; i < 9; i++) {
        fill_rect(px, 11 - i, 12 + i, i * 2 + 1, 1, RGB(0xE8, 0xE8, 0xF0));
    }
    for (int i = 0; i < 7; i++) {
        fill_rect(px, 21 - i, 14 + i, i * 2 + 1, 1, RGB(0xC0, 0xC8, 0xD8));
    }
}

/*
 * A leaf, and it used to be a vertical smear.
 *
 * The old one tapered symmetrically from the middle, which is the shape of a
 * grain of rice. A leaf is the *overlap of two circles* -- pointed at both
 * ends, curved along both sides -- and it has to be tilted, because a leaf
 * standing straight up reads as a flame or an eye. The midrib and a stem
 * settle which end is which.
 */
static void draw_avatar_leaf(color *px) {
    avatar_disc(px, RGB(0x28, 0x3E, 0x2A));

    fill_lens(px, 6, 22, 22, 6, 15, RGB(0x7E, 0xC0, 0x58));

    /* The midrib, along the long axis, and veins off it. */
    for (int i = 0; i < 18; i++) {
        plot(px, 7 + i, 24 - i, RGB(0x3E, 0x74, 0x38));
        if (i % 4 == 2) {
            plot(px, 7 + i + 1, 24 - i - 2, RGB(0x3E, 0x74, 0x38));
            plot(px, 7 + i + 2, 24 - i - 3, RGB(0x3E, 0x74, 0x38));
            plot(px, 7 + i - 2, 24 - i + 1, RGB(0x3E, 0x74, 0x38));
            plot(px, 7 + i - 3, 24 - i + 2, RGB(0x3E, 0x74, 0x38));
        }
    }

    /* The stem, past the lower point. */
    for (int i = 0; i < 4; i++) {
        plot(px, 6 - i, 25 + i, RGB(0x5A, 0x46, 0x2E));
    }
}

static void draw_avatar_wave(color *px) {
    avatar_disc(px, RGB(0x2E, 0x5E, 0x8E));
    for (int band = 0; band < 3; band++) {
        int y = 12 + band * 5;
        for (int x = 5; x < 27; x++) {
            int lift = ((x + band * 3) / 3) % 2;
            plot(px, x, y + lift, RGB(0xA8, 0xD8, 0xF0));
            plot(px, x, y + lift + 1, RGB(0x88, 0xC0, 0xE8));
        }
    }
}

static void draw_avatar_star(color *px) {
    avatar_disc(px, RGB(0x2A, 0x2A, 0x4E));
    /* A four-point star: two tapering bars crossed. */
    for (int i = 0; i < 12; i++) {
        int w = (i < 6 ? i : 11 - i) / 2 + 1;
        fill_rect(px, 16 - w / 2, 10 + i, w + 1, 1, RGB(0xF0, 0xE0, 0x90));
    }
    for (int i = 0; i < 12; i++) {
        int h = (i < 6 ? i : 11 - i) / 2 + 1;
        fill_rect(px, 10 + i, 16 - h / 2, 1, h + 1, RGB(0xF0, 0xE0, 0x90));
    }
}

/*
 * A gear, and it used to read as a crosshair.
 *
 * Four stubby teeth on a disc is a reticle: the eye takes short marks at the
 * cardinal points as sights, not as gearing. Eight teeth is what makes it
 * machinery -- at four the gaps are larger than the teeth and the shape has
 * no rim left to be a rim.
 */
static void draw_avatar_gear(color *px) {
    avatar_disc(px, RGB(0x3E, 0x42, 0x4E));

    /* Eight teeth: four square-on and four on the diagonals. */
    for (int i = 0; i < 2; i++) {
        int off = i * 20;
        fill_rect(px, 13, 3 + off, 6, 8, RGB(0xC8, 0xCC, 0xD4));
        fill_rect(px, 3 + off, 13, 8, 6, RGB(0xC8, 0xCC, 0xD4));
    }
    for (int i = 0; i < 4; i++) {
        int cx = (i % 2 == 0) ? 8 : 24;
        int cy = (i < 2) ? 8 : 24;
        fill_disc(px, cx, cy, 4, RGB(0xC8, 0xCC, 0xD4));
    }

    fill_disc(px, 16, 16, 11, RGB(0xC8, 0xCC, 0xD4));
    fill_disc(px, 16, 16, 9, RGB(0xA0, 0xA6, 0xB0));
    fill_disc(px, 16, 16, 5, RGB(0x3E, 0x42, 0x4E));
}

static void draw_avatar_moon(color *px) {
    avatar_disc(px, RGB(0x1E, 0x24, 0x3E));
    fill_disc(px, 15, 16, 10, RGB(0xF0, 0xEC, 0xD0));
    fill_disc(px, 21, 13, 9, RGB(0x1E, 0x24, 0x3E));
    plot(px, 8, 8, RGB(0xF0, 0xEC, 0xD0));
    plot(px, 24, 24, RGB(0xF0, 0xEC, 0xD0));
}

/*
 * A campfire, and it used to be a flame that read as one anyway.
 *
 * "It looks odd" was the note, and the reason is that a bare tapering blob is
 * not a picture of anything -- there is nothing in it to say what it is, so
 * whoever looks at it supplies the nearest thing, and the nearest thing to an
 * orange tapering blob on a dark red disc is a campfire. Given that everybody
 * was going to read it as a campfire, it should be one: two crossed logs give
 * the flame something to be *on*, which is the whole difference between a
 * shape and a picture.
 */
static void draw_avatar_campfire(color *px) {
    avatar_disc(px, RGB(0x22, 0x1A, 0x2E));

    /* The flame first, so the logs sit in front of it. Three tongues, the
     * outer two shorter, because a single symmetrical one reads as a leaf. */
    for (int y = 0; y < 15; y++) {
        int w = (y < 5 ? y : (y < 10 ? 5 : 19 - y)) + 1;
        fill_rect(px, 16 - w, 7 + y, w * 2, 1, RGB(0xE0, 0x62, 0x20));
    }
    for (int y = 0; y < 10; y++) {
        int w = (y < 3 ? y : (y < 7 ? 3 : 12 - y)) + 1;
        fill_rect(px, 16 - w, 12 + y, w * 2, 1, RGB(0xF6, 0xA8, 0x30));
    }
    for (int y = 0; y < 6; y++) {
        int w = (y < 2 ? y : (y < 4 ? 2 : 7 - y)) + 1;
        fill_rect(px, 16 - w / 2, 16 + y, w, 1, RGB(0xFA, 0xE8, 0xA0));
    }

    /* Two logs, crossed, with a lit end each. The angle is what makes them
     * logs rather than a bench. */
    for (int i = 0; i < 15; i++) {
        int y = 24 + i / 6;
        fill_rect(px, 8 + i, y, 1, 3, RGB(0x6A, 0x46, 0x2C));
        fill_rect(px, 8 + i, y, 1, 1, RGB(0x8C, 0x60, 0x3C));
    }
    for (int i = 0; i < 15; i++) {
        int y = 26 - i / 6;
        fill_rect(px, 8 + i, y, 1, 3, RGB(0x5A, 0x3A, 0x24));
        fill_rect(px, 8 + i, y, 1, 1, RGB(0x7C, 0x52, 0x32));
    }

    /* Embers where the logs meet the flame. */
    plot(px, 13, 24, RGB(0xF0, 0x80, 0x28));
    plot(px, 18, 25, RGB(0xF0, 0x80, 0x28));
}

/*
 * --- Sixteen more ---
 *
 * Asked for as a set rather than one at a time, and that is the right way to
 * ask: eight pictures is not a choice, it is a shortage that everybody works
 * around by keeping whichever one they were given. Twenty-four is enough that
 * two accounts on the same machine rarely collide.
 *
 * Same rules as the first eight. Drawn rather than shipped, so the set is
 * complete the moment ReconOS first runs with nothing borrowed and nobody's
 * licence to honour. Each is a plain shape on a coloured disc, and each has
 * to be *identifiable* at the size a login screen shows it -- which rules out
 * anything whose meaning lives in fine detail. The dark discs and the light
 * ones alternate roughly, so a grid of them does not read as one texture.
 */

/* A tower with a lit lamp. The one the system is named for. */
/*
 * A lighthouse. The one the system is named for, so it is worth getting right.
 *
 * The first attempt was fourteen rows tall on a thirty-two pixel canvas with
 * four stripes across it, and read as a spool of thread: too squat to be a
 * tower, and striped often enough that the stripes became the subject. Taller,
 * narrower, two bands, and the light drawn as a wedge going out to each side
 * rather than as two floating bars -- a beam has to be attached to the lamp or
 * it is just weather.
 */
static void draw_avatar_lighthouse(color *px) {
    avatar_disc(px, RGB(0x16, 0x26, 0x3C));

    /* The beam: a wedge each side, widening away from the lamp. */
    for (int i = 0; i < 10; i++) {
        int spread = 1 + i / 2;
        fill_rect(px, 11 - i, 10 - spread / 2, 1, spread + 1,
            RGB(0x2E, 0x4A, 0x6C));
        fill_rect(px, 20 + i, 10 - spread / 2, 1, spread + 1,
            RGB(0x2E, 0x4A, 0x6C));
    }

    /* The tower: tall, with a gentle taper. */
    for (int y = 0; y < 17; y++) {
        int w = 5 + y / 4;
        color band = (y >= 4 && y < 8) || (y >= 12 && y < 16)
            ? RGB(0xC4, 0x3A, 0x2E) : RGB(0xEC, 0xEC, 0xF0);
        fill_rect(px, 16 - w / 2, 12 + y, w, 1, band);
    }

    /* The lamp room, and the gallery it stands on. */
    fill_rect(px, 13, 7, 6, 5, RGB(0xF8, 0xE0, 0x70));
    fill_rect(px, 12, 11, 8, 1, RGB(0x8A, 0x8A, 0x94));
    fill_rect(px, 12, 6, 8, 1, RGB(0x8A, 0x8A, 0x94));
    plot(px, 16, 4, RGB(0x8A, 0x8A, 0x94));
    plot(px, 16, 5, RGB(0x8A, 0x8A, 0x94));

    /* The rock it stands on. */
    fill_rect(px, 8, 29, 16, 2, RGB(0x3E, 0x46, 0x50));
    fill_rect(px, 10, 27, 12, 2, RGB(0x4A, 0x52, 0x5E));
}

/* The project's own mark: a body and two panels. */
static void draw_avatar_satellite(color *px) {
    avatar_disc(px, RGB(0x14, 0x18, 0x2C));
    fill_rect(px, 14, 12, 5, 9, RGB(0xD0, 0xD4, 0xDC));
    fill_rect(px, 5, 14, 8, 5, RGB(0x50, 0x84, 0xC8));
    fill_rect(px, 20, 14, 8, 5, RGB(0x50, 0x84, 0xC8));
    fill_rect(px, 9, 14, 1, 5, RGB(0x28, 0x50, 0x88));
    fill_rect(px, 24, 14, 1, 5, RGB(0x28, 0x50, 0x88));
    fill_rect(px, 15, 21, 3, 5, RGB(0x9A, 0x9A, 0xA4));
    fill_disc(px, 16, 27, 3, RGB(0xE8, 0xE8, 0xF0));
    fill_disc(px, 16, 27, 1, RGB(0x14, 0x18, 0x2C));
}

/* A ringed planet: the ring is what stops it being a dot. */
static void draw_avatar_planet(color *px) {
    avatar_disc(px, RGB(0x1A, 0x14, 0x2E));
    fill_disc(px, 16, 15, 8, RGB(0xE0, 0x9A, 0x50));
    fill_disc(px, 13, 12, 3, RGB(0xF0, 0xBC, 0x78));
    for (int x = 2; x < 30; x++) {
        int dy = (x - 16) / 6;
        plot(px, x, 21 - dy, RGB(0xC8, 0xB8, 0xE8));
        plot(px, x, 22 - dy, RGB(0xA0, 0x8C, 0xC8));
    }
    fill_disc(px, 16, 15, 7, RGB(0xE0, 0x9A, 0x50));
    fill_disc(px, 13, 12, 3, RGB(0xF0, 0xBC, 0x78));
}

/* Rays, so it is a sun and not a circle. */
static void draw_avatar_sun(color *px) {
    avatar_disc(px, RGB(0x2A, 0x4E, 0x74));
    for (int i = 0; i < 4; i++) {
        fill_rect(px, 15, 3 + i * 9, 2, 4, RGB(0xF8, 0xD8, 0x60));
        fill_rect(px, 3 + i * 9, 15, 4, 2, RGB(0xF8, 0xD8, 0x60));
    }
    fill_disc(px, 16, 16, 8, RGB(0xF8, 0xC8, 0x40));
    fill_disc(px, 14, 14, 4, RGB(0xFC, 0xE8, 0x90));
}

/* Three overlapping discs and a flat base. */
static void draw_avatar_cloud(color *px) {
    avatar_disc(px, RGB(0x5A, 0x86, 0xB4));
    fill_disc(px, 12, 17, 6, RGB(0xF0, 0xF4, 0xF8));
    fill_disc(px, 19, 15, 7, RGB(0xF0, 0xF4, 0xF8));
    fill_disc(px, 23, 19, 5, RGB(0xF0, 0xF4, 0xF8));
    fill_rect(px, 7, 19, 19, 5, RGB(0xF0, 0xF4, 0xF8));
    fill_rect(px, 7, 23, 19, 1, RGB(0xC8, 0xD4, 0xE0));
}

/* A bolt: two wedges offset, which is what gives it the kink. */
static void draw_avatar_bolt(color *px) {
    avatar_disc(px, RGB(0x30, 0x2A, 0x50));
    for (int y = 0; y < 10; y++) {
        fill_rect(px, 17 - y / 2, 6 + y, 6, 1, RGB(0xF8, 0xE0, 0x50));
    }
    fill_rect(px, 10, 16, 12, 2, RGB(0xF8, 0xE0, 0x50));
    for (int y = 0; y < 10; y++) {
        fill_rect(px, 15 - y / 2, 17 + y, 5, 1, RGB(0xF0, 0xC8, 0x38));
    }
}

/* A drop: a disc with a point on top. */
static void draw_avatar_drop(color *px) {
    avatar_disc(px, RGB(0x1E, 0x4A, 0x60));
    for (int y = 0; y < 11; y++) {
        int w = y / 2 + 1;
        fill_rect(px, 16 - w / 2, 6 + y, w + 1, 1, RGB(0x70, 0xC8, 0xE8));
    }
    fill_disc(px, 16, 20, 7, RGB(0x70, 0xC8, 0xE8));
    fill_disc(px, 13, 18, 2, RGB(0xC8, 0xEC, 0xF8));
}

/* A pine: three tiers, because one triangle is a traffic cone. */
static void draw_avatar_pine(color *px) {
    avatar_disc(px, RGB(0x24, 0x3A, 0x2E));
    fill_rect(px, 15, 24, 3, 5, RGB(0x6A, 0x46, 0x2C));
    for (int tier = 0; tier < 3; tier++) {
        int top = 6 + tier * 6;
        for (int y = 0; y < 9; y++) {
            int w = 3 + y + tier * 2;
            fill_rect(px, 16 - w / 2, top + y, w, 1, RGB(0x4E, 0x8C, 0x50));
        }
    }
}

/* A shield: straight shoulders, tapering to a point. */
static void draw_avatar_shield(color *px) {
    avatar_disc(px, RGB(0x3A, 0x2A, 0x24));
    for (int y = 0; y < 20; y++) {
        int w = y < 11 ? 18 : 18 - (y - 11) * 2;
        if (w < 2) {
            break;
        }
        fill_rect(px, 16 - w / 2, 7 + y, w, 1, RGB(0xC8, 0xB0, 0x60));
    }
    for (int y = 0; y < 14; y++) {
        int w = y < 8 ? 10 : 10 - (y - 8) * 2;
        if (w < 2) {
            break;
        }
        fill_rect(px, 16 - w / 2, 10 + y, w, 1, RGB(0x8C, 0x2E, 0x2E));
    }
}

/* A book, open, with a gutter down the middle. */
static void draw_avatar_book(color *px) {
    avatar_disc(px, RGB(0x2E, 0x2A, 0x40));
    fill_rect(px, 5, 10, 22, 15, RGB(0xF0, 0xEC, 0xE0));
    fill_rect(px, 5, 10, 22, 1, RGB(0xC8, 0xC4, 0xB8));
    fill_rect(px, 15, 9, 2, 17, RGB(0x8C, 0x4E, 0x2E));
    for (int i = 0; i < 4; i++) {
        fill_rect(px, 8, 14 + i * 3, 6, 1, RGB(0xB0, 0xAC, 0xA0));
        fill_rect(px, 18, 14 + i * 3, 6, 1, RGB(0xB0, 0xAC, 0xA0));
    }
}

/* A mug with a handle and something hot in it. */
static void draw_avatar_mug(color *px) {
    avatar_disc(px, RGB(0x40, 0x38, 0x30));
    fill_rect(px, 8, 13, 14, 13, RGB(0xE8, 0xE8, 0xEC));
    fill_rect(px, 8, 13, 14, 2, RGB(0x8C, 0x5A, 0x38));
    fill_rect(px, 22, 16, 4, 2, RGB(0xE8, 0xE8, 0xEC));
    fill_rect(px, 25, 16, 2, 7, RGB(0xE8, 0xE8, 0xEC));
    fill_rect(px, 22, 22, 4, 2, RGB(0xE8, 0xE8, 0xEC));
    for (int i = 0; i < 6; i++) {
        plot(px, 12 + (i % 2), 6 + i, RGB(0xB0, 0xB8, 0xC0));
        plot(px, 18 - (i % 2), 6 + i, RGB(0xB0, 0xB8, 0xC0));
    }
}

/* A quaver: a head, a stem and a flag. */
static void draw_avatar_note(color *px) {
    avatar_disc(px, RGB(0x28, 0x22, 0x3E));
    fill_disc(px, 12, 22, 5, RGB(0xE8, 0xD8, 0xF0));
    fill_rect(px, 15, 8, 3, 15, RGB(0xE8, 0xD8, 0xF0));
    for (int i = 0; i < 7; i++) {
        fill_rect(px, 18, 8 + i, 4 - i / 3, 1, RGB(0xE8, 0xD8, 0xF0));
    }
}

/* A rocket: a body, a nose, two fins and a flame. */
static void draw_avatar_rocket(color *px) {
    avatar_disc(px, RGB(0x18, 0x22, 0x3A));
    for (int y = 0; y < 6; y++) {
        int w = y + 1;
        fill_rect(px, 16 - w / 2, 5 + y, w, 1, RGB(0xD8, 0xDC, 0xE4));
    }
    fill_rect(px, 13, 11, 6, 11, RGB(0xE8, 0xEC, 0xF0));
    fill_disc(px, 16, 15, 2, RGB(0x50, 0x90, 0xC8));
    fill_rect(px, 9, 17, 4, 6, RGB(0xC4, 0x3A, 0x2E));
    fill_rect(px, 19, 17, 4, 6, RGB(0xC4, 0x3A, 0x2E));
    for (int y = 0; y < 5; y++) {
        int w = 5 - y;
        fill_rect(px, 16 - w / 2, 22 + y, w, 1, RGB(0xF8, 0xC0, 0x40));
    }
}

/* A cut gem: a table on top and facets below. */
static void draw_avatar_gem(color *px) {
    avatar_disc(px, RGB(0x24, 0x2E, 0x3E));
    fill_rect(px, 9, 11, 14, 4, RGB(0x88, 0xE0, 0xD8));
    fill_rect(px, 11, 9, 10, 2, RGB(0xB8, 0xF0, 0xE8));
    for (int y = 0; y < 12; y++) {
        int w = 14 - y - y / 2;
        if (w < 1) {
            break;
        }
        fill_rect(px, 16 - w / 2, 15 + y, w, 1, RGB(0x50, 0xB0, 0xB8));
    }
    fill_rect(px, 15, 15, 2, 9, RGB(0x88, 0xE0, 0xD8));
}

/* A compass rose: a needle on a dial. */
static void draw_avatar_compass(color *px) {
    avatar_disc(px, RGB(0x2E, 0x38, 0x2A));
    fill_disc(px, 16, 16, 12, RGB(0xE4, 0xE0, 0xD0));
    fill_disc(px, 16, 16, 10, RGB(0xF4, 0xF0, 0xE4));
    for (int y = 0; y < 9; y++) {
        int w = y / 2 + 1;
        fill_rect(px, 16 - w / 2, 7 + y, w, 1, RGB(0xC4, 0x3A, 0x2E));
    }
    for (int y = 0; y < 9; y++) {
        int w = 5 - y / 2;
        fill_rect(px, 16 - w / 2, 16 + y, w, 1, RGB(0x50, 0x58, 0x64));
    }
    fill_disc(px, 16, 16, 2, RGB(0x2E, 0x38, 0x2A));
}

/* A beacon: a mast throwing three arcs. */
/*
 * A beacon: a mast throwing three arcs.
 *
 * The arcs were computed by walking x and solving for y, which is fine near
 * the top of a circle and wrong at the sides -- so they flattened out into
 * fronds and the whole thing read as a palm tree. stroke_arc walks the ring
 * itself and cuts it with a cone, which keeps the same shape at every radius.
 */
static void draw_avatar_signal(color *px) {
    avatar_disc(px, RGB(0x1A, 0x26, 0x2E));

    for (int ring = 0; ring < 3; ring++) {
        stroke_arc(px, 16, 14, 6 + ring * 4, 1, 3, 5,
            RGB(0x58, 0xC8, 0xA0));
    }

    /* The mast, and feet, so the arcs have something to come from. */
    fill_rect(px, 15, 13, 2, 14, RGB(0xD8, 0xDC, 0xE4));
    for (int i = 0; i < 5; i++) {
        plot(px, 15 - i, 27 - i / 2, RGB(0xD8, 0xDC, 0xE4));
        plot(px, 16 + i, 27 - i / 2, RGB(0xD8, 0xDC, 0xE4));
    }
    fill_disc(px, 16, 12, 2, RGB(0xF8, 0xE0, 0x60));
}

/*
 * A tent, which is what the winding path should have been.
 *
 * A path bending down a 32-pixel disc is four pixels wide and mostly
 * ambiguous, and with a round marker at the top it read as a match. A tent is
 * a silhouette -- one triangle, one dark opening -- and it goes with the
 * campfire, which is the other thing anybody keeps out here.
 */
static void draw_avatar_tent(color *px) {
    avatar_disc(px, RGB(0x1E, 0x2E, 0x2A));

    for (int y = 0; y < 16; y++) {
        int w = y + 2;
        fill_rect(px, 16 - w, 11 + y, w * 2, 1, RGB(0xC8, 0x8A, 0x40));
        /* A lit face and a shaded one, so it has a side. */
        fill_rect(px, 16, 11 + y, w, 1, RGB(0xA8, 0x70, 0x32));
    }

    /* The opening: a narrow triangle up the middle. */
    for (int y = 0; y < 12; y++) {
        int w = y / 2 + 1;
        fill_rect(px, 16 - w / 2, 15 + y, w + 1, 1, RGB(0x22, 0x1C, 0x18));
    }

    /* The ridge and the guy line. */
    fill_rect(px, 15, 9, 2, 3, RGB(0x8A, 0x5E, 0x2C));
    for (int i = 0; i < 5; i++) {
        plot(px, 17 + i, 10 + i, RGB(0x6E, 0x7A, 0x62));
    }
    fill_rect(px, 4, 27, 24, 1, RGB(0x36, 0x46, 0x3A));
}

/* Two curved strokes: a person, without a face to get wrong. */
static void draw_avatar_person(color *px) {
    avatar_disc(px, RGB(0x46, 0x50, 0x64));
    fill_disc(px, 16, 12, 6, RGB(0xE8, 0xEC, 0xF0));
    for (int y = 0; y < 9; y++) {
        int w = 10 + y;
        fill_rect(px, 16 - w / 2, 21 + y, w, 1, RGB(0xE8, 0xEC, 0xF0));
    }
}

static void draw_avatar_key(color *px) {
    avatar_disc(px, RGB(0x4E, 0x3E, 0x6E));
    fill_disc(px, 12, 13, 6, RGB(0xE8, 0xD0, 0x80));
    fill_disc(px, 12, 13, 3, RGB(0x4E, 0x3E, 0x6E));
    fill_rect(px, 14, 17, 3, 10, RGB(0xE8, 0xD0, 0x80));
    fill_rect(px, 17, 21, 4, 2, RGB(0xE8, 0xD0, 0x80));
    fill_rect(px, 17, 25, 4, 2, RGB(0xE8, 0xD0, 0x80));
}

/*
 * A key on a ring, for the page that lists kept passwords.
 *
 * A key rather than a padlock: a padlock says "locked", which is a state, and
 * this page is about the things being held rather than about whether they are
 * reachable right now. The ring is what makes it a keyring and not one key --
 * the page lists however many there are.
 */
static void draw_keyring(color *px) {
    /* The ring the keys hang from, drawn as a disc with the middle taken out
     * rather than as a circle of plotted points -- the same trick every other
     * ring in this file uses, and it antialiases the same way (not at all,
     * deliberately: these are read at 16 pixels). */
    fill_disc(px, 11, 13, 8, C_OUTLINE);
    fill_disc(px, 11, 13, 6, C_METAL);
    fill_disc(px, 11, 13, 4, 0);

    /*
     * The key: a bow, a shaft, and two teeth on one side. Two rather than one
     * because a single tooth reads as an arrow at this size.
     *
     * Brass on a dark outline rather than the near-white C_KEY, which is what
     * this was first drawn in -- and which vanished into the Control Panel's
     * grey. Every icon on that page is dark-edged; one that is not looks
     * disabled.
     */
    fill_disc(px, 21, 12, 6, C_OUTLINE);
    fill_disc(px, 21, 12, 4, C_BRASS);
    fill_disc(px, 21, 12, 2, 0);
    fill_rect(px, 19, 15, 5, 13, C_OUTLINE);
    fill_rect(px, 20, 16, 3, 11, C_BRASS);
    fill_rect(px, 23, 19, 5, 3, C_OUTLINE);
    fill_rect(px, 23, 19, 4, 2, C_BRASS);
    fill_rect(px, 23, 24, 5, 3, C_OUTLINE);
    fill_rect(px, 23, 24, 4, 2, C_BRASS);
}

static const struct generated_icon ICONS[] = {
    { "folder", draw_folder },
    { "file", draw_file },

    /*
     * One per kind of file, all built on the same sheet.
     *
     * A single icon for every file says nothing, and saying what a thing is
     * before its name is read is most of what an icon is for. Which extension
     * gets which is recon_props_icon's business, not this file's -- this only
     * knows how to draw them.
     */
    { "file-sound", draw_file_sound },
    { "file-video", draw_file_video },
    { "file-image", draw_file_image },
    { "file-web", draw_file_web },
    { "file-data", draw_file_data },
    { "file-font", draw_file_font },
    { "file-archive", draw_file_archive },
    { "application", draw_application },
    { "terminal", draw_terminal },

    /*
     * The same two drawings again, under the names the Control Panel now asks
     * by. Not a rename: "application" and "terminal" are still what an
     * application and a terminal are called, and these are what two *pages*
     * are called. A skin's own set can answer one without answering the
     * other, which is the point of them being separate names at all.
     */
    { "accounts", draw_application },
    { "troubleshoot", draw_terminal },
    { "display", draw_notepad },
    { "notepad", draw_notepad },
    { "calculator", draw_calculator },
    { "explorer", draw_explorer },
    { "taskmanager", draw_taskmanager },
    { "trash", draw_trash_empty },
    { "trash-full", draw_trash_full },
    { "shutdown", draw_shutdown },
    { "system", draw_system },
    { "keyring", draw_keyring },

    /* The account pictures. Named with a prefix so the set can be listed by
     * looking for it, which is how the picker finds them without a second
     * table to keep in step. */
    { RECON_AVATAR_PREFIX "mountain", draw_avatar_mountain },
    { RECON_AVATAR_PREFIX "leaf", draw_avatar_leaf },
    { RECON_AVATAR_PREFIX "wave", draw_avatar_wave },
    { RECON_AVATAR_PREFIX "star", draw_avatar_star },
    { RECON_AVATAR_PREFIX "gear", draw_avatar_gear },
    { RECON_AVATAR_PREFIX "moon", draw_avatar_moon },
    { RECON_AVATAR_PREFIX "campfire", draw_avatar_campfire },
    { RECON_AVATAR_PREFIX "key", draw_avatar_key },

    /* The sixteen added in v0.4.0. Appended rather than sorted in, so an
     * account that already chose one of the first eight keeps the picture it
     * chose -- the choice is stored by name, but the picker shows them in
     * this order and moving them would shuffle the grid under somebody who
     * had learnt where theirs sits. */
    { RECON_AVATAR_PREFIX "lighthouse", draw_avatar_lighthouse },
    { RECON_AVATAR_PREFIX "satellite", draw_avatar_satellite },
    { RECON_AVATAR_PREFIX "planet", draw_avatar_planet },
    { RECON_AVATAR_PREFIX "sun", draw_avatar_sun },
    { RECON_AVATAR_PREFIX "cloud", draw_avatar_cloud },
    { RECON_AVATAR_PREFIX "bolt", draw_avatar_bolt },
    { RECON_AVATAR_PREFIX "drop", draw_avatar_drop },
    { RECON_AVATAR_PREFIX "pine", draw_avatar_pine },
    { RECON_AVATAR_PREFIX "shield", draw_avatar_shield },
    { RECON_AVATAR_PREFIX "book", draw_avatar_book },
    { RECON_AVATAR_PREFIX "mug", draw_avatar_mug },
    { RECON_AVATAR_PREFIX "note", draw_avatar_note },
    { RECON_AVATAR_PREFIX "rocket", draw_avatar_rocket },
    { RECON_AVATAR_PREFIX "gem", draw_avatar_gem },
    { RECON_AVATAR_PREFIX "compass", draw_avatar_compass },
    { RECON_AVATAR_PREFIX "signal", draw_avatar_signal },
    { RECON_AVATAR_PREFIX "tent", draw_avatar_tent },
    { RECON_AVATAR_PREFIX "person", draw_avatar_person },
};

/*
 * --- Which drawing of the icons a system has ---
 *
 * Raised whenever the generated set changes in a way somebody would see.
 *
 * The rule up to now was "write an icon if it is not already there", and it
 * has a promise inside it worth keeping: an icon somebody replaced stays
 * replaced, so the generated set is a starting point rather than something
 * reimposed on every start. What it also meant, and nobody intended, is that
 * an icon *improved* here never reaches a machine that has run ReconOS once.
 * The set was not a default. It was a one-time imprint.
 *
 * So: remember which generation wrote each file, and a fingerprint of what it
 * wrote. On a later start, an icon is rewritten only when the generation has
 * moved on *and* the file on disk is still byte-for-byte what this program put
 * there. Anything else -- a replaced icon, a hand-edited one -- is left alone
 * and stops being tracked, which is the original promise stated precisely
 * instead of approximately.
 *
 *   1  the set as it stood through v0.3.x, drawn at 32 by 32
 *   2  v0.4.0: drawn at 128 and downsampled by whatever shows it; the flame
 *      became a campfire; eighteen more account pictures; gear, leaf,
 *      lighthouse and the beacon redrawn after looking at them
 *   3  v0.4.0: `accounts`, `troubleshoot` and `display`, for three Control
 *      Panel pages that had been borrowing -- two of them the same one, which
 *      is why Display Settings and Registry wore the same picture
 */
#define ICONS_GENERATION 3

#define ICONS_GENERATION_KEY "icons/generation"

/*
 * CRC-32, so "is this still the file we wrote" can be asked of a whole icon
 * without keeping a copy of it.
 *
 * A hash rather than a size: two drawings of the same icon are the same
 * length, and length alone would call a replaced icon unchanged. Not a
 * cryptographic one -- nobody is trying to forge an icon, and the question is
 * only whether it drifted.
 */
static unsigned long crc32_of(const unsigned char *data, size_t length) {
    static unsigned long table[256];
    static bool ready;

    if (!ready) {
        for (unsigned long i = 0; i < 256; i++) {
            unsigned long c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        ready = true;
    }

    unsigned long crc = 0xFFFFFFFFUL;
    for (size_t i = 0; i < length; i++) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFUL;
}

static void stamp_key(const char *path, char *out, size_t size) {
    snprintf(out, size, "icons/stamp/%s", path);
}

/* What is on disk right now, or 0 for a file that is not there. */
static unsigned long crc_of_file(const char *path) {
    size_t size = 0;
    char *data = recon_fs_read("/", path, &size);
    if (data == NULL) {
        return 0;
    }
    unsigned long crc = crc32_of((const unsigned char *)data, size);
    free(data);
    return crc;
}

/*
 * Should this file be written?
 *
 * Missing: yes. Present and still exactly what a previous generation of this
 * program wrote, with the generation since moved on: yes, and that is the
 * whole point of the machinery. Present and different from what we recorded:
 * no, somebody owns it now.
 */
static bool should_write(const char *path, bool overwrite, bool moved_on) {
    if (overwrite || !recon_fs_exists("/", path)) {
        return true;
    }
    if (!moved_on) {
        return false;
    }

    char key[RECON_REGISTRY_KEY_MAX];
    stamp_key(path, key, sizeof(key));

    const char *recorded = recon_registry_get(RECON_REG_SYSTEM, key, "");
    if (recorded[0] == '\0') {
        /* Written before stamps existed. Its bytes cannot be vouched for, so
         * it is treated as somebody's -- the safe direction, and it costs one
         * generation of staleness on a system upgrading from before v0.4.0. */
        return false;
    }
    return strtoul(recorded, NULL, 16) == crc_of_file(path);
}

static void record_stamp(const char *path) {
    char key[RECON_REGISTRY_KEY_MAX];
    char value[16];
    stamp_key(path, key, sizeof(key));
    snprintf(value, sizeof(value), "%08lX", crc_of_file(path));
    recon_registry_set(RECON_REG_SYSTEM, key, value);
}

int recon_icons_write_defaults(bool overwrite) {
    color *px = malloc((size_t)ICON_PIXELS * ICON_PIXELS * sizeof(color));
    if (px == NULL) {
        return 0;
    }

    /* Where the glossy set goes. Created here rather than assumed, because
     * this runs on a filesystem that may have been made a moment ago. */
    char glossy_dir[RECON_PATH_MAX];
    snprintf(glossy_dir, sizeof(glossy_dir), "%s/%s",
        RECON_DIR_SYSTEM_ICONS, RECON_ICONS_GLOSSY);
    recon_fs_mkdir("/", glossy_dir);

    /* Has the drawing changed since this system was last written to? */
    bool moved_on = recon_registry_get_int(RECON_REG_SYSTEM,
        ICONS_GENERATION_KEY, 0) < ICONS_GENERATION;

    int written = 0;
    for (size_t i = 0; i < sizeof(ICONS) / sizeof(ICONS[0]); i++) {
        char path[RECON_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.ico",
            RECON_DIR_SYSTEM_ICONS, ICONS[i].name);

        /* A replaced icon stays replaced: the generated set is a default, not
         * something the system re-imposes on every start. An unreplaced one
         * is brought up to date, which is the other half of being a default
         * rather than an imprint. */
        if (should_write(path, overwrite, moved_on)) {
            memset(px, 0, (size_t)ICON_PIXELS * ICON_PIXELS * sizeof(color));
            ICONS[i].draw(px);
            if (write_ico(path, px)) {
                record_stamp(path);
                written++;
            }
        }

        /*
         * And the same icon again, lit.
         *
         * Drawn from scratch rather than glossed from the copy above: that copy
         * may be one somebody replaced, and putting a highlight on a picture
         * whose whole point was to not be ours is worse than not offering the
         * glossy version of it at all.
         */
        snprintf(path, sizeof(path), "%s/%s/%s.ico",
            RECON_DIR_SYSTEM_ICONS, RECON_ICONS_GLOSSY, ICONS[i].name);
        if (!should_write(path, overwrite, moved_on)) {
            continue;
        }
        memset(px, 0, (size_t)ICON_PIXELS * ICON_PIXELS * sizeof(color));
        ICONS[i].draw(px);
        gloss(px);
        if (write_ico(path, px)) {
            record_stamp(path);
            written++;
        }
    }

    /* Clear away the files for pictures this program has since renamed, so
     * the picker offers the new name and not both. */
    recon_avatar_retire_old_files();

    /*
     * Recorded last, so a run that is interrupted part way through tries
     * again next time rather than deciding it is finished.
     */
    if (moved_on) {
        recon_registry_set_int(RECON_REG_SYSTEM, ICONS_GENERATION_KEY,
            ICONS_GENERATION);
    }

    free(px);
    return written;
}
