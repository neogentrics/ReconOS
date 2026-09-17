/*
 * How a panel reaches a screen when there is no compositor.
 *
 * The other implementation of `struct recon_panel_present`, and the reason
 * that seam exists. `recon_ui_wlr.c` hands a panel's pixels to a scene graph
 * on Linux; this one stores them straight into a framebuffer, which is what a
 * program on the ReconOS kernel does:
 *
 *     open("/dev/fb0") -> SYS_SCREEN (shape) -> SYS_MAP (address) -> stores
 *
 * After the map the kernel is not in the path at all. Drawing is stores to
 * memory, and so is this.
 *
 * --- what this deliberately is not --------------------------------------
 *
 * **There is no z-order here, and there cannot be.** A scene graph knows what
 * is in front of what and redraws the parts that become visible; a direct
 * store knows only where the pixels went. `raise_to_top` therefore does
 * nothing, and says so rather than pretending -- a function that quietly fails
 * to raise a window is worse than one that was never offered, because the
 * caller stops looking for the reason.
 *
 * The same is true of hiding: `set_enabled(false)` stops the panel drawing,
 * and what was underneath does not come back, because nothing here remembers
 * what was underneath. Whatever owns the screen repaints.
 *
 * That is not a limitation to fix here. It is the difference between a
 * compositor and a program with a screen, and the desktop-on-ReconOS question
 * is which of those the shell becomes -- board row `addr-space`, and not this
 * file's business.
 *
 * --- and it takes no system calls ---------------------------------------
 *
 * The caller passes the address and the shape it already asked the kernel
 * for. So this file has no `SYS_MAP` in it, does not know what a file
 * descriptor is, and runs exactly the same on a host buffer -- which is what
 * `tests/test_ui_fb.c` drives it with. **The half that can be wrong is
 * arithmetic on pixels, and arithmetic on pixels does not need a machine.**
 */

#include <stdlib.h>
#include <string.h>

#include "recon_ui.h"
#include "recon_ui_internal.h"

struct screen_present {
    uint32_t *pixels;   /* the framebuffer itself; not ours to free */
    size_t pitch;       /* BYTES per row, which is not width * 4 */
    int width, height;  /* of the screen */

    int x, y;           /* where this panel sits on it */
    bool enabled;
};

/*
 * One pixel of straight-alpha source over one pixel of destination.
 *
 * Straight alpha, because that is what everything above the seam works in and
 * what makes `recon_color_fade` composable. The conversion the wlroots side
 * does -- to premultiplied -- is for wlroots' blend mode and has no meaning
 * here: this is the blend, so it does the arithmetic once and writes the
 * answer.
 */
static uint32_t over(uint32_t src, uint32_t dst) {
    uint32_t a = (src >> 24) & 0xFFu;

    /*
     * **Both of these are shortcuts, not guards**, and the difference is worth
     * stating because the first version of this comment got it wrong.
     *
     * The general formula below is already exact at both ends of the range:
     * with `a` 0 it returns the destination byte for byte, and with `a` 255 it
     * returns the source, for every one of the 256 values -- the `+127`
     * rounding is what makes that true rather than nearly true. They are here
     * because the two ends are almost all of the pixels on a screen, not
     * because the arithmetic needs help.
     *
     * A mutation that deleted the `a == 0` return changed no output at all,
     * which is what said so. The claim that it was preventing the
     * square-cornered-window fault was carried over from the wlroots side,
     * where the same fault is real -- but there it comes from *premultiplying*
     * and then adding, and this does not premultiply.
     */
    if (a == 0xFFu) {
        return src | 0xFF000000u;
    }
    if (a == 0u) {
        return dst;
    }

    uint32_t sr = (src >> 16) & 0xFFu;
    uint32_t sg = (src >> 8) & 0xFFu;
    uint32_t sb = src & 0xFFu;

    uint32_t dr = (dst >> 16) & 0xFFu;
    uint32_t dg = (dst >> 8) & 0xFFu;
    uint32_t db = dst & 0xFFu;

    /*
     * Rounded rather than truncated: +127 before the divide.
     *
     * Truncating loses about half a level per blend, which is invisible once
     * and visible as banding on a gradient drawn by repeated fades -- which
     * is exactly how the glass skins are built.
     */
    uint32_t r = (sr * a + dr * (255u - a) + 127u) / 255u;
    uint32_t g = (sg * a + dg * (255u - a) + 127u) / 255u;
    uint32_t b = (sb * a + db * (255u - a) + 127u) / 255u;

    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void screen_commit(struct recon_panel *panel, void *state) {
    struct screen_present *s = state;

    if (s == NULL || !s->enabled || panel->pixels == NULL) {
        return;
    }

    for (int row = 0; row < panel->height; row++) {
        int sy = s->y + row;
        if (sy < 0 || sy >= s->height) {
            continue;   /* off the top or the bottom */
        }

        /*
         * The row is addressed through `pitch`, not `width * 4`.
         *
         * An adapter pads a row to whatever suits it, and bytes per row is
         * the one fact a program cannot recover by looking at the pixels --
         * which is the whole reason SYS_SCREEN exists. Where the two happen
         * to be equal, and they are on most emulators, a program that
         * confuses them draws a perfect picture and shears on the first real
         * laptop.
         */
        uint32_t *dst_row = (uint32_t *)((unsigned char *)s->pixels +
            (size_t)sy * s->pitch);
        const uint32_t *src_row = panel->pixels + (size_t)row * panel->width;

        for (int col = 0; col < panel->width; col++) {
            int sx = s->x + col;
            if (sx < 0 || sx >= s->width) {
                continue;   /* off the left or the right */
            }
            dst_row[sx] = over(src_row[col], dst_row[sx]);
        }
    }
}

static void screen_set_position(void *state, int x, int y) {
    struct screen_present *s = state;
    if (s != NULL) {
        s->x = x;
        s->y = y;
    }
}

static void screen_position(void *state, int *x, int *y) {
    struct screen_present *s = state;
    if (s == NULL) {
        return;
    }
    if (x != NULL) {
        *x = s->x;
    }
    if (y != NULL) {
        *y = s->y;
    }
}

static void screen_raise_to_top(void *state) {
    /*
     * Nothing, and deliberately nothing.
     *
     * There is no z-order to raise within: this writes pixels at an address.
     * What is in front of what is decided by the order things are committed,
     * by whoever is doing the committing.
     */
    (void)state;
}

static void screen_set_enabled(void *state, bool enabled) {
    struct screen_present *s = state;
    if (s != NULL) {
        /* Stops it drawing. What was underneath does not come back --
         * nothing here remembers it. See the note at the top. */
        s->enabled = enabled;
    }
}

static void screen_destroy(void *state) {
    /* The framebuffer is the caller's; only the little record of where this
     * panel sits on it belongs to us. */
    free(state);
}

/*
 * Nothing, and deliberately nothing, for the same reason as raising: there is
 * no z-order here to move within. A panel that asked to go to the back has
 * asked the wrong thing to do it -- whoever commits decides.
 */
static void screen_lower_to_bottom(void *state) {
    (void)state;
}

static const struct recon_panel_present SCREEN_PRESENT = {
    .commit = screen_commit,
    .set_position = screen_set_position,
    .position = screen_position,
    .raise_to_top = screen_raise_to_top,
    .lower_to_bottom = screen_lower_to_bottom,
    .set_enabled = screen_set_enabled,
    .destroy = screen_destroy,
};

struct recon_panel *recon_panel_on_screen(void *screen, size_t pitch,
        int screen_width, int screen_height,
        int x, int y, int width, int height) {
    if (screen == NULL || width <= 0 || height <= 0 ||
            screen_width <= 0 || screen_height <= 0) {
        return NULL;
    }

    /*
     * A pitch smaller than a row is refused rather than clamped.
     *
     * Every row would be written partly over the previous one, which draws a
     * sheared picture that looks like a drawing fault rather than a caller
     * that passed width where it meant bytes. Refusing says which.
     */
    if (pitch < (size_t)screen_width * 4u) {
        return NULL;
    }

    struct screen_present *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }

    s->pixels = screen;
    s->pitch = pitch;
    s->width = screen_width;
    s->height = screen_height;
    s->x = x;
    s->y = y;
    s->enabled = true;

    struct recon_panel *panel = recon_panel_wrap(width, height,
        &SCREEN_PRESENT, s);
    if (panel == NULL) {
        free(s);
        return NULL;
    }

    return panel;
}
