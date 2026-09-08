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

int recon_color_luminance(recon_color color) {
    int r = (int)((color >> 16) & 0xFF);
    int g = (int)((color >> 8) & 0xFF);
    int b = (int)(color & 0xFF);

    /* 0.299, 0.587, 0.114, in 1024ths. The classic weights, and integer so
     * this can run anywhere the drawing code runs. */
    return (306 * r + 601 * g + 117 * b) >> 10;
}

recon_color recon_color_tint(recon_color base, recon_color tint, int strength) {
    return recon_color_tint_to(base, tint, strength,
        recon_color_luminance(base));
}

recon_color recon_color_tint_to(recon_color base, recon_color tint,
        int strength, int want) {
    if (strength <= 0) {
        return base;
    }
    if (strength > 255) {
        strength = 255;
    }
    if (want < 0) { want = 0; }
    if (want > 255) { want = 255; }

    int have = recon_color_luminance(tint);

    int tr = (int)((tint >> 16) & 0xFF);
    int tg = (int)((tint >> 8) & 0xFF);
    int tb = (int)(tint & 0xFF);

    /*
     * The tint, moved to the base's lightness.
     *
     * Below the tint's own lightness this scales towards black; above it, it
     * scales towards white. Two ranges rather than one multiply, because a
     * multiply cannot make a colour lighter than the tint without pushing a
     * channel past 255 -- and clamping there is what turns a light surface
     * into a saturated one.
     */
    if (want <= have && have > 0) {
        tr = tr * want / have;
        tg = tg * want / have;
        tb = tb * want / have;
    } else if (have < 255) {
        int room = 255 - have;
        int over = want - have;
        tr += (255 - tr) * over / room;
        tg += (255 - tg) * over / room;
        tb += (255 - tb) * over / room;
    }

    int br = (int)((base >> 16) & 0xFF);
    int bg = (int)((base >> 8) & 0xFF);
    int bb = (int)(base & 0xFF);

    int r = br + (tr - br) * strength / 255;
    int g = bg + (tg - bg) * strength / 255;
    int b = bb + (tb - bb) * strength / 255;

    if (r < 0) { r = 0; } if (r > 255) { r = 255; }
    if (g < 0) { g = 0; } if (g > 255) { g = 255; }
    if (b < 0) { b = 0; } if (b > 255) { b = 255; }

    /* The alpha is the base's. A tint changes what colour something is, not
     * whether it is there. */
    return (base & 0xFF000000u) | ((recon_color)r << 16) |
        ((recon_color)g << 8) | (recon_color)b;
}

recon_color recon_color_mix(recon_color from, recon_color to,
        uint8_t amount) {
    if (amount == 0) {
        return from;
    }

    uint32_t out = from & 0xFF000000u;
    for (int shift = 0; shift <= 16; shift += 8) {
        int a = (int)((from >> shift) & 0xFF);
        int b = (int)((to >> shift) & 0xFF);
        out |= (uint32_t)(a + ((b - a) * (int)amount) / 255) << shift;
    }
    return out;
}

/*
 * A surface with light falling on it.
 *
 * Not "part of the way to white", which is what this was and which is a
 * different amount of change depending on where it starts. Mixing 110 of 255
 * toward white lifts E8EBF5 by nine levels -- a soft sheen on a light button --
 * and lifts 3A3E46 by *eighty-five*, which on Midnight put a pale grey bar
 * across the top of every dark button. The fraction is constant and the effect
 * is not.
 *
 * A light source adds roughly the same amount wherever it lands, so this adds:
 * a proportion of the headroom, capped at a step. On something already light
 * the proportion wins and the lift stays gentle; on something dark the cap
 * wins and the lift stays gentle. Both come out looking like the same lamp.
 */
recon_color recon_color_highlight(recon_color base) {
    const int MOST = 26;

    uint32_t out = base & 0xFF000000u;
    for (int shift = 0; shift <= 16; shift += 8) {
        int c = (int)((base >> shift) & 0xFF);
        int room = ((255 - c) * 110) / 255;
        if (room > MOST) {
            room = MOST;
        }
        out |= (uint32_t)(c + room) << shift;
    }
    return out;
}

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

/*
 * Enough of a step that text does not sit *on* its background. Sixty is where
 * the shipped skins land for their dimmed text once the bad ones are fixed,
 * and the taskbar test holds them there.
 */
#define READABLE_GAP 60

recon_color recon_color_readable_on(recon_color surface, recon_color preferred,
        recon_color light_ink, recon_color dark_ink) {
    int on = recon_color_luminance(surface);
    int want = recon_color_luminance(preferred);

    int gap = want > on ? want - on : on - want;
    if (gap >= READABLE_GAP) {
        return preferred;
    }

    /* Whichever of the skin's two inks is further from the surface. */
    int lit = recon_color_luminance(light_ink);
    int dim = recon_color_luminance(dark_ink);
    int from_light = lit > on ? lit - on : on - lit;
    int from_dark = dim > on ? dim - on : on - dim;
    return from_light >= from_dark ? light_ink : dark_ink;
}
