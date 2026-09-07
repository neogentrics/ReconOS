/*
 * What state a control is in, and what colour that makes its surface.
 *
 * Split from recon_widget.c because it is the half with no compositor in it.
 * The rules below are the ones worth proving -- "held but the pointer wandered
 * off is not pressed", "nothing else lights up while a press is in progress" --
 * and proving them by photographing a window would mean a screen, a skin, a
 * pointer and a great deal of patience for four lines of arithmetic.
 *
 * The same reason recon_titlebar.c is its own file: a rule that can be checked
 * without pixels should be.
 */

#include <stdbool.h>
#include <stdint.h>

#include "recon_theme.h"
#include "recon_ui.h"
#include "recon_widget.h"

/*
 * How far hover and press move a surface toward the selection colour.
 *
 * Derived rather than named as a role, so a skin somebody made this afternoon
 * has a hover the moment it has a selection colour -- which every skin does,
 * because a skin that could not show a selected file would be unusable long
 * before anyone noticed its buttons did not light up.
 *
 * 40 and 90 of 255. At 20 the highlight was there and had to be looked for,
 * which is the worst of both: the cost of the feature without the difference
 * it was meant to make -- the same lesson the button corner radius learnt at
 * three pixels.
 */
#define HOVER_MIX 40
#define PRESS_MIX 90

recon_color recon_widget_surface(recon_color base,
        enum recon_widget_state state) {
    switch (state) {
    case RECON_WIDGET_HOT:
        return recon_color_mix(base, THEME(SELECTION), HOVER_MIX);
    case RECON_WIDGET_ACTIVE:
        return recon_color_mix(base, THEME(SELECTION), PRESS_MIX);
    case RECON_WIDGET_NORMAL:
    case RECON_WIDGET_DISABLED:
    default:
        return base;
    }
}

enum recon_widget_state recon_widget_state_from(uint32_t hot, uint32_t held,
        uint32_t id, bool disabled) {
    if (disabled) {
        return RECON_WIDGET_DISABLED;
    }
    if (id == RECON_HIT_NONE) {
        return RECON_WIDGET_NORMAL;
    }

    /*
     * Held *and* hovered is what counts as pressed, not held alone.
     *
     * Dragging off a button and letting go there does not press it -- every
     * desktop agrees on that, and a button that stayed sunk while the pointer
     * wandered away would be telling the person the opposite of what is about
     * to happen.
     */
    if (held == id) {
        return hot == id ? RECON_WIDGET_ACTIVE : RECON_WIDGET_NORMAL;
    }

    /*
     * Nothing else is hot while a press is in progress.
     *
     * Otherwise every button the pointer crossed on its way somewhere would
     * light up behind the one being held, which reads as the press having
     * escaped rather than as the pointer having moved.
     */
    if (held != RECON_HIT_NONE) {
        return RECON_WIDGET_NORMAL;
    }

    return hot == id ? RECON_WIDGET_HOT : RECON_WIDGET_NORMAL;
}
