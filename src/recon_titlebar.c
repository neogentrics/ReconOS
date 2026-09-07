/*
 * Where the things on a title bar go. See include/recon_titlebar.h.
 */

#include <string.h>

#include "recon_theme.h"
#include "recon_titlebar.h"

void recon_titlebar_layout(int width, int inset, int gap, bool with_icon,
        struct recon_titlebar_layout *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    int height = recon_theme_metric(RECON_METRIC_TITLE_HEIGHT);
    int size = recon_theme_metric(RECON_METRIC_BUTTON_SIZE);
    int top = (height - size) / 2;
    if (top < 0) {
        top = 0;
    }

    /*
     * The close bit, put back.
     *
     * The metric's range already refuses zero, so a skin file cannot ask for
     * no buttons -- but a range is a promise about what may be *written*, and
     * this is a promise about what is *drawn*. They are different promises and
     * only the second one is a guarantee: a value can reach this function from
     * somewhere that never went through the file parser.
     *
     * A window nobody can close by mouse is not a look somebody chose. It is
     * a window that has to be closed with Alt+Q, by somebody who knows that,
     * on a machine whose skin came from the internet.
     */
    int wanted = recon_theme_metric(RECON_METRIC_BUTTONS) | RECON_BUTTON_CLOSE;
    bool on_left = recon_theme_metric(RECON_METRIC_BUTTONS_LEFT) != 0;

    static const int BIT[RECON_TITLEBAR_BUTTON_COUNT] = {
        RECON_BUTTON_CLOSE,
        RECON_BUTTON_MAXIMIZE,
        RECON_BUTTON_MINIMIZE,
    };

    /*
     * Placed from the outside in, so close is in the corner on either side and
     * a missing maximize closes the gap rather than leaving one.
     *
     * `slot` counts buttons that exist rather than buttons considered, which
     * is the whole of what makes a skin with two buttons look deliberate
     * instead of like a skin with three and a hole in it.
     */
    int slot = 0;
    for (int i = 0; i < RECON_TITLEBAR_BUTTON_COUNT; i++) {
        if ((wanted & BIT[i]) == 0) {
            continue;
        }

        int offset = slot * (size + gap);
        out->button[i].x = on_left
            ? inset + offset
            : width - inset - size - offset;
        out->button[i].y = top;
        out->button[i].w = size;
        out->button[i].h = size;
        slot++;
    }

    /* How much of the bar the buttons and their margins take. Measured from
     * `slot` rather than from three, so a skin with fewer of them gives the
     * title the room back. */
    int taken = (slot > 0) ? inset + slot * size + (slot - 1) * gap : 0;

    int icon_size = height - 8;
    if (icon_size < 8) {
        icon_size = 8;
    }

    if (on_left) {
        /*
         * Buttons on the left, then the icon, then the title.
         *
         * The icon does not swap to the right with them. It belongs to the
         * title -- it says what this window is, the way the words beside it
         * do -- and a layout that separated the two would put a picture at one
         * end of the bar and its caption at the other.
         */
        int at = taken + inset;
        if (with_icon) {
            out->icon.x = at;
            out->icon.y = 4;
            out->icon.w = icon_size;
            out->icon.h = icon_size;
            at += icon_size + 6;
        }
        out->text_x = at;
        out->text_width = width - inset - at;

        out->drag.x = taken;
        out->drag.w = width - taken;
    } else {
        int at = inset;
        if (with_icon) {
            out->icon.x = at;
            out->icon.y = 4;
            out->icon.w = icon_size;
            out->icon.h = icon_size;
            at += icon_size + 6;
        }
        out->text_x = at;
        out->text_width = width - taken - at;

        out->drag.x = 0;
        out->drag.w = width - taken;
    }

    if (out->text_width < 0) {
        out->text_width = 0;
    }
    if (out->drag.w < 0) {
        out->drag.w = 0;
    }
    out->drag.y = 0;
    out->drag.h = height;
}
