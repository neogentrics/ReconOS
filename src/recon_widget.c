/*
 * The widget layer.
 *
 * See include/recon_widget.h for what this is for and why it carries its own
 * version. This file is the drawing: everything an application used to write
 * out longhand for every button it had, written once.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "recon_icons.h"
#include "recon_theme.h"
#include "recon_ui.h"
#include "recon_widget.h"

/* --- Interaction state --- */

bool recon_widget_hover(struct recon_panel *panel, uint32_t id) {
    if (panel == NULL || recon_panel_hot(panel) == id) {
        return false;
    }
    recon_panel_set_hot(panel, id);
    return true;
}

bool recon_widget_press(struct recon_panel *panel, uint32_t id) {
    if (panel == NULL || recon_panel_held(panel) == id) {
        return false;
    }
    recon_panel_set_held(panel, id);
    return true;
}

bool recon_widget_release(struct recon_panel *panel) {
    if (panel == NULL || recon_panel_held(panel) == RECON_HIT_NONE) {
        return false;
    }
    recon_panel_set_held(panel, RECON_HIT_NONE);
    return true;
}

uint32_t recon_widget_hot_id(const struct recon_panel *panel) {
    return recon_panel_hot(panel);
}

uint32_t recon_widget_held_id(const struct recon_panel *panel) {
    return recon_panel_held(panel);
}

enum recon_widget_state recon_widget_state_of(const struct recon_panel *panel,
        uint32_t id, bool disabled) {
    if (panel == NULL) {
        return disabled ? RECON_WIDGET_DISABLED : RECON_WIDGET_NORMAL;
    }
    return recon_widget_state_from(recon_panel_hot(panel),
        recon_panel_held(panel), id, disabled);
}

/* --- Highlights --- */

void recon_widget_highlight(struct recon_panel *panel, int x, int y, int w,
        int h, recon_color color) {
    recon_fill_round_rect(panel, x, y, w, h,
        recon_button_radius(w, h), color);
}

void recon_widget_highlight_role(struct recon_panel *panel, int x, int y,
        int w, int h, enum recon_theme_role role) {
    /*
     * Through recon_fill_role when the skin grades this role, because a
     * gradient is a per-row colour and cannot be reduced to one. Graded
     * highlights stay square, which is the honest trade: a ramp drawn into a
     * rounded shape needs the ramp and the shape computed together, and no
     * skin currently grades a selection.
     */
    if (recon_theme_gradient(role, NULL, NULL)) {
        recon_fill_role(panel, x, y, w, h, role);
        return;
    }
    recon_widget_highlight(panel, x, y, w, h, recon_theme_color(role));
}

/* --- Buttons --- */

/* What a look asks for at rest, before the state is applied. */
static recon_color look_fill(enum recon_widget_look look, bool checked) {
    switch (look) {
    case RECON_WIDGET_ACCENT:
        return THEME(ACCENT);
    case RECON_WIDGET_TAB:
        return checked ? THEME(SURFACE) : THEME(BUTTON);
    case RECON_WIDGET_DANGER:
    case RECON_WIDGET_PLAIN:
    default:
        return checked ? THEME(BUTTON_ACTIVE) : THEME(BUTTON);
    }
}

static recon_color look_text(enum recon_widget_look look, bool checked) {
    switch (look) {
    case RECON_WIDGET_ACCENT:
        return THEME(ACCENT_TEXT);
    case RECON_WIDGET_TAB:
        return checked ? THEME(SURFACE_TEXT) : THEME(BUTTON_TEXT);
    case RECON_WIDGET_DANGER:
    case RECON_WIDGET_PLAIN:
    default:
        return THEME(BUTTON_TEXT);
    }
}

enum recon_widget_state recon_widget_button(struct recon_panel *panel,
        const struct recon_widget_button *b) {
    if (panel == NULL || b == NULL || b->w <= 0 || b->h <= 0) {
        return RECON_WIDGET_NORMAL;
    }

    enum recon_widget_state state =
        recon_widget_state_of(panel, b->id, b->disabled);

    recon_color fill = b->fill != RECON_WIDGET_SKIN
        ? b->fill : look_fill(b->look, b->checked);
    recon_color text = b->text != RECON_WIDGET_SKIN
        ? b->text : look_text(b->look, b->checked);

    /*
     * Danger goes warning-coloured under the pointer rather than at rest.
     *
     * A close button that is red all the time is a close button somebody
     * stops seeing. Red on approach is the whole signal: it arrives exactly
     * when it is useful and at no other time.
     */
    if (b->look == RECON_WIDGET_DANGER
            && (state == RECON_WIDGET_HOT || state == RECON_WIDGET_ACTIVE)) {
        fill = state == RECON_WIDGET_ACTIVE
            ? recon_color_mix(THEME(WARNING), 0xFF000000u, 40)
            : THEME(WARNING);
        text = THEME(ACCENT_TEXT);
    } else {
        fill = recon_widget_surface(fill, state);
    }

    if (state == RECON_WIDGET_DISABLED) {
        text = THEME(DIM);
    }

    /*
     * Filled and edged as one rounded shape, sunk while it is held.
     *
     * `behind` is no longer read. It was what the corners were painted back
     * out to after a square fill, and a wedge of the wrong colour appeared
     * wherever the caller's belief about what was underneath did not match
     * what was actually underneath -- over any gradient, always. The field is
     * kept on the struct so that the handful of callers still setting it
     * compile, and it is ignored.
     */
    recon_fill_button(panel, b->x, b->y, b->w, b->h,
        state == RECON_WIDGET_ACTIVE, fill);

    /*
     * The label, centred, and clipped at the button's own right edge.
     *
     * The width passed is measured from where the text starts to where the
     * button ends. Passing the button's full width from an indented start --
     * which is what several call sites did before this existed -- lets a long
     * label run past its own button and over the next one.
     */
    if (b->label != NULL && b->font != NULL) {
        int line = recon_font_line_height(b->font);
        int ascent = recon_font_ascent(b->font);
        int label_w = recon_text_width(b->font, b->label);

        int tx = b->x + (b->w - label_w) / 2;
        if (tx < b->x + 3) {
            tx = b->x + 3;
        }
        int ty = b->y + (b->h - line) / 2 + ascent;

        /* A pressed button's label moves with it, or the button sinks and
         * its writing stays behind. */
        if (state == RECON_WIDGET_ACTIVE) {
            tx += 1;
            ty += 1;
        }

        recon_draw_text(panel, b->font, tx, ty, b->x + b->w - 3 - tx,
            b->label, text);
    }

    if (b->id != RECON_HIT_NONE) {
        recon_hit_add(panel, b->x, b->y, b->w, b->h, b->id);
        if (b->tip != NULL && b->tip[0] != '\0') {
            recon_hit_tip(panel, b->tip);
        }
        /*
         * A disabled button keeps its region and its tooltip and stops
         * answering. Set here so that saying `disabled` is the whole of
         * saying it -- an application that had to remember a second guard in
         * its own click handler would be one where the two can disagree, and
         * eventually they always do.
         */
        if (b->disabled) {
            recon_hit_inert(panel);
        }
    }

    return state;
}

/* --- Caption buttons --- */

static const char *caption_icon(enum recon_widget_caption glyph) {
    switch (glyph) {
    case RECON_WIDGET_CAPTION_MINIMIZE: return RECON_ICON_WINDOW_MINIMIZE;
    case RECON_WIDGET_CAPTION_MAXIMIZE: return RECON_ICON_WINDOW_MAXIMIZE;
    case RECON_WIDGET_CAPTION_RESTORE:  return RECON_ICON_WINDOW_RESTORE;
    case RECON_WIDGET_CAPTION_CLOSE:
    default:                            return RECON_ICON_WINDOW_CLOSE;
    }
}

/*
 * The three shapes, drawn rather than written.
 *
 * These have to be there before a font has loaded and before any file has been
 * read, which is why they are rectangles and not glyphs from a typeface. A
 * skin that wants different ones puts a picture at the icon name and it is
 * used instead.
 */
static void caption_glyph(struct recon_panel *p, int x, int y, int size,
        enum recon_widget_caption glyph, recon_color ink) {
    switch (glyph) {
    case RECON_WIDGET_CAPTION_MINIMIZE:
        recon_fill_rect(p, x + 4, y + size - 6, size - 8, 2, ink);
        break;
    case RECON_WIDGET_CAPTION_MAXIMIZE:
        recon_stroke_rect(p, x + 3, y + 3, size - 6, size - 6, ink);
        recon_fill_rect(p, x + 3, y + 3, size - 6, 2, ink);
        break;
    case RECON_WIDGET_CAPTION_RESTORE:
        recon_stroke_rect(p, x + 3, y + 5, size - 8, size - 9, ink);
        recon_stroke_rect(p, x + 5, y + 3, size - 8, size - 9, ink);
        break;
    case RECON_WIDGET_CAPTION_CLOSE:
    default:
        for (int k = 0; k < size - 8; k++) {
            recon_fill_rect(p, x + 4 + k, y + 4 + k, 2, 1, ink);
            recon_fill_rect(p, x + 4 + k, y + size - 5 - k, 2, 1, ink);
        }
        break;
    }
}

enum recon_widget_state recon_widget_caption_button(struct recon_panel *panel,
        int x, int y, int size, uint32_t id, enum recon_widget_caption glyph,
        recon_color behind, const char *tip) {
    if (panel == NULL || size <= 0) {
        return RECON_WIDGET_NORMAL;
    }

    enum recon_widget_state state = recon_widget_state_of(panel, id, false);

    recon_color fill = THEME(WINDOW_BUTTON);
    recon_color ink = THEME(WINDOW_BUTTON_GLYPH);

    /*
     * Close is the one that goes red, and only under the pointer.
     *
     * Somebody who has learnt on another desktop that the red one loses their
     * work has learnt it here. That is not imitation for its own sake -- it is
     * the single piece of window chrome where being conventional is worth more
     * than being distinctive, because the cost of being wrong about it is a
     * closed window.
     */
    if (glyph == RECON_WIDGET_CAPTION_CLOSE
            && (state == RECON_WIDGET_HOT || state == RECON_WIDGET_ACTIVE)) {
        fill = state == RECON_WIDGET_ACTIVE
            ? recon_color_mix(THEME(WARNING), 0xFF000000u, 50)
            : THEME(WARNING);
        ink = THEME(ACCENT_TEXT);
    } else {
        fill = recon_widget_surface(fill, state);
    }

    /*
     * Drawn as a rounded shape, so the corners are never painted over and
     * there is nothing to guess about what is behind them.
     *
     * A title bar is the worst place for the old approach. Several skins put a
     * *gradient* on it, so the colour a pixel of bar actually is depends on
     * which row it is in -- and a corner carved back to the bar's single named
     * colour was therefore wrong on every row but one, which is the pale wedge
     * that showed at the corner of every close button.
     */
    recon_fill_button(panel, x, y, size, size,
        state == RECON_WIDGET_ACTIVE, fill);

    recon_hit_add(panel, x, y, size, size, id);
    if (tip != NULL && tip[0] != '\0') {
        recon_hit_tip(panel, tip);
    }

    /*
     * A picture where the skin supplied one, and the drawn shape otherwise.
     *
     * Almost every system takes the second path, which is right: these are the
     * controls that have to work before anything has been loaded.
     */
    int nudge = state == RECON_WIDGET_ACTIVE ? 1 : 0;
    if (!recon_icon_draw(panel, caption_icon(glyph), x + 2 + nudge,
            y + 2 + nudge, size - 4)) {
        caption_glyph(panel, x + nudge, y + nudge, size, glyph, ink);
    }

    (void)behind;
    return state;
}
