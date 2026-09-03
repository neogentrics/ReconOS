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
 */

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_avatar.h"
#include "recon_fs.h"
#include "recon_icon_gen.h"

#define ICON_SIZE 32

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

/* --- Drawing --- */

static void plot(color *px, int x, int y, color c) {
    if (x >= 0 && y >= 0 && x < ICON_SIZE && y < ICON_SIZE) {
        px[y * ICON_SIZE + x] = c;
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

/* A filled disc, for anything round. */
static void fill_disc(color *px, int cx, int cy, int radius, color c) {
    for (int y = cy - radius; y <= cy + radius; y++) {
        for (int x = cx - radius; x <= cx + radius; x++) {
            int dx = x - cx;
            int dy = y - cy;
            if (dx * dx + dy * dy <= radius * radius) {
                plot(px, x, y, c);
            }
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
    int w = ICON_SIZE, h = ICON_SIZE;
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

    /* ICONDIRENTRY */
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
    dib[4] = (unsigned char)w;
    dib[8] = (unsigned char)(h * 2);
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

static void draw_file(color *px) {
    draw_page(px, true);
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

static void draw_avatar_leaf(color *px) {
    avatar_disc(px, RGB(0x4E, 0x7C, 0x4E));
    for (int y = 0; y < 16; y++) {
        int w = (y < 8 ? y : 15 - y) + 2;
        fill_rect(px, 16 - w / 2, 8 + y, w, 1, RGB(0xB8, 0xE0, 0x90));
    }
    fill_rect(px, 15, 9, 1, 14, RGB(0x3A, 0x5E, 0x3A));
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

static void draw_avatar_gear(color *px) {
    avatar_disc(px, RGB(0x5E, 0x5E, 0x68));
    fill_disc(px, 16, 16, 9, RGB(0xC8, 0xC8, 0xD0));
    for (int i = 0; i < 4; i++) {
        fill_rect(px, 15, 4 + i * 8, 3, 4, RGB(0xC8, 0xC8, 0xD0));
        fill_rect(px, 4 + i * 8, 15, 4, 3, RGB(0xC8, 0xC8, 0xD0));
    }
    fill_disc(px, 16, 16, 4, RGB(0x5E, 0x5E, 0x68));
}

static void draw_avatar_moon(color *px) {
    avatar_disc(px, RGB(0x1E, 0x24, 0x3E));
    fill_disc(px, 15, 16, 10, RGB(0xF0, 0xEC, 0xD0));
    fill_disc(px, 21, 13, 9, RGB(0x1E, 0x24, 0x3E));
    plot(px, 8, 8, RGB(0xF0, 0xEC, 0xD0));
    plot(px, 24, 24, RGB(0xF0, 0xEC, 0xD0));
}

static void draw_avatar_flame(color *px) {
    avatar_disc(px, RGB(0x6E, 0x2A, 0x1E));
    for (int y = 0; y < 16; y++) {
        int w = (y < 5 ? y : (y < 11 ? 5 : 20 - y)) + 1;
        fill_rect(px, 16 - w, 8 + y, w * 2, 1, RGB(0xF0, 0xA0, 0x30));
    }
    for (int y = 0; y < 9; y++) {
        int w = (y < 3 ? y : (y < 6 ? 3 : 11 - y)) + 1;
        fill_rect(px, 16 - w / 2, 15 + y, w, 1, RGB(0xF8, 0xE8, 0x90));
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

static const struct generated_icon ICONS[] = {
    { "folder", draw_folder },
    { "file", draw_file },
    { "application", draw_application },
    { "terminal", draw_terminal },
    { "notepad", draw_notepad },
    { "calculator", draw_calculator },
    { "explorer", draw_explorer },
    { "taskmanager", draw_taskmanager },
    { "trash", draw_trash_empty },
    { "trash-full", draw_trash_full },
    { "shutdown", draw_shutdown },
    { "system", draw_system },

    /* The account pictures. Named with a prefix so the set can be listed by
     * looking for it, which is how the picker finds them without a second
     * table to keep in step. */
    { RECON_AVATAR_PREFIX "mountain", draw_avatar_mountain },
    { RECON_AVATAR_PREFIX "leaf", draw_avatar_leaf },
    { RECON_AVATAR_PREFIX "wave", draw_avatar_wave },
    { RECON_AVATAR_PREFIX "star", draw_avatar_star },
    { RECON_AVATAR_PREFIX "gear", draw_avatar_gear },
    { RECON_AVATAR_PREFIX "moon", draw_avatar_moon },
    { RECON_AVATAR_PREFIX "flame", draw_avatar_flame },
    { RECON_AVATAR_PREFIX "key", draw_avatar_key },
};

int recon_icons_write_defaults(bool overwrite) {
    color *px = malloc((size_t)ICON_SIZE * ICON_SIZE * sizeof(color));
    if (px == NULL) {
        return 0;
    }

    int written = 0;
    for (size_t i = 0; i < sizeof(ICONS) / sizeof(ICONS[0]); i++) {
        char path[RECON_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.ico",
            RECON_DIR_SYSTEM_ICONS, ICONS[i].name);

        /* A replaced icon stays replaced: the generated set is a default, not
         * something the system re-imposes on every start. */
        if (!overwrite && recon_fs_exists("/", path)) {
            continue;
        }

        memset(px, 0, (size_t)ICON_SIZE * ICON_SIZE * sizeof(color));
        ICONS[i].draw(px);
        if (write_ico(path, px)) {
            written++;
        }
    }

    free(px);
    return written;
}
