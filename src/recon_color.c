/*
 * Colour arithmetic that needs nothing.
 *
 * Split out of recon_ui.c for a reason worth stating: everything else in that
 * file needs a compositor, a font library and a pixel buffer, and this needs
 * none of them. Leaving it there would mean the one piece of arithmetic in the
 * drawing layer that can be silently wrong is also the one piece that cannot be
 * checked without a display.
 *
 * Premultiplication is exactly that kind of arithmetic. Getting it wrong
 * produces chrome that is see-through *and* too bright, which looks like a
 * deliberate glow rather than a fault -- so it would be found, if ever, by
 * somebody wondering why the glass looks lit from inside.
 */

#include "recon_ui.h"

recon_color recon_color_fade(recon_color color, uint8_t alpha) {
    if (alpha == 255) {
        return color;
    }

    uint32_t a = alpha;

    /*
     * Scaled by what is already there rather than replaced, so applying this
     * twice halves the opacity twice instead of quietly setting it -- which is
     * what a reader expects of something called "fade", and is the behaviour
     * that survives somebody nesting the calls later.
     *
     * Rounded on the alpha and truncated on the colours, deliberately: alpha
     * decides whether a thing is visible at all and a lost unit there
     * accumulates over repeated application, while a lost unit of blue is
     * below what any screen can show.
     */
    uint32_t was = (color >> 24) & 0xFFu;
    uint32_t now = (was * a + 127) / 255;

    uint32_t r = ((color >> 16) & 0xFFu) * a / 255;
    uint32_t g = ((color >> 8) & 0xFFu) * a / 255;
    uint32_t b = (color & 0xFFu) * a / 255;

    return (now << 24) | (r << 16) | (g << 8) | b;
}
