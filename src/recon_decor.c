/*
 * Client window decorations. See include/recon_decor.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>

#include "ReconOS.h"
#include "recon_decor.h"
#include "recon_icons.h"
#include "recon_server.h"
#include "recon_theme.h"
#include "recon_ui.h"

/*
 * The same numbers a built-in window's frame uses, read from the skin the same
 * way. A client window that looked *nearly* like a ReconOS window would be
 * worse than one that plainly does not: near-misses read as a fault rather
 * than as a difference.
 */
#define TITLE_HEIGHT recon_theme_metric(RECON_METRIC_TITLE_HEIGHT)
#define BUTTON_SIZE recon_theme_metric(RECON_METRIC_BUTTON_SIZE)
#define CORNER recon_theme_metric(RECON_METRIC_CORNER)
#define TITLE_INSET 6
#define BUTTON_GAP 2

#define COLOR_TITLE_TEXT THEME(TITLE_TEXT)
#define COLOR_TITLE_TEXT_INACTIVE THEME(TITLE_TEXT_INACTIVE)
#define COLOR_BUTTON THEME(WINDOW_BUTTON)
#define COLOR_GLYPH THEME(WINDOW_BUTTON_GLYPH)
#define COLOR_EDGE THEME(WINDOW_EDGE)

enum hit {
    HIT_NONE,
    HIT_TITLEBAR,
    HIT_MINIMIZE,
    HIT_MAXIMIZE,
    HIT_CLOSE,
};

struct recon_decor {
    struct recon_server *server;
    struct recon_font *font;
    struct recon_toplevel *toplevel;

    struct recon_panel *panel;

    /*
     * What the bar was last drawn for. Redrawing on every commit would mean
     * repainting a title bar sixty times a second because a terminal is
     * printing, so the drawing happens when one of these has moved.
     */
    int at_x, at_y;
    int width;
    bool focused;
    bool maximized;
    char title[128];

    /* Dragging, in the decoration's own coordinates: where in the bar the
     * pointer took hold, so the window follows without jumping. */
    bool dragging;
    double grab_x, grab_y;
};

/* --- Where the window is --- */

/*
 * The window's box on screen, as the client draws it.
 *
 * The scene node says where the surface tree was put; the surface's own
 * geometry says which part of it is the window rather than its shadow. With
 * server-side decorations there should be no shadow, but a client is not
 * obliged to agree and the arithmetic is the same either way.
 */
static void window_box(struct recon_decor *decor, int *x, int *y, int *w,
        int *h) {
    struct wlr_scene_tree *tree = decor->toplevel->scene_tree;
    struct wlr_xdg_toplevel *xdg = decor->toplevel->xdg_toplevel;

    *x = 0; *y = 0; *w = 0; *h = 0;
    if (tree == NULL || xdg == NULL || xdg->base == NULL) {
        return;
    }

    struct wlr_box geometry;
    wlr_xdg_surface_get_geometry(xdg->base, &geometry);

    *x = tree->node.x;
    *y = tree->node.y;
    *w = geometry.width;
    *h = geometry.height;
}

/* --- Drawing --- */

static void draw_buttons(struct recon_decor *decor) {
    struct recon_panel *p = decor->panel;
    int title_height = TITLE_HEIGHT;
    int button = BUTTON_SIZE;
    int top = (title_height - button) / 2;
    int right = decor->width - TITLE_INSET;

    const enum hit ids[3] = { HIT_CLOSE, HIT_MAXIMIZE, HIT_MINIMIZE };

    for (int i = 0; i < 3; i++) {
        int bx = right - (i + 1) * button - i * BUTTON_GAP;
        int by = top;

        recon_fill_rect(p, bx, by, button, button, COLOR_BUTTON);
        recon_draw_button_edge(p, bx, by, button, button, false,
            decor->focused ? THEME(TITLE_ACTIVE) : THEME(TITLE_INACTIVE));
        recon_hit_add(p, bx, by, button, button, (uint32_t)ids[i]);

        switch (ids[i]) {
        case HIT_MINIMIZE:
            recon_fill_rect(p, bx + 4, by + button - 6, button - 8, 2,
                COLOR_GLYPH);
            break;
        case HIT_MAXIMIZE:
            if (decor->maximized) {
                recon_stroke_rect(p, bx + 3, by + 5, button - 8, button - 9,
                    COLOR_GLYPH);
                recon_stroke_rect(p, bx + 5, by + 3, button - 8, button - 9,
                    COLOR_GLYPH);
            } else {
                recon_stroke_rect(p, bx + 3, by + 3, button - 6, button - 6,
                    COLOR_GLYPH);
                recon_fill_rect(p, bx + 3, by + 3, button - 6, 2, COLOR_GLYPH);
            }
            break;
        case HIT_CLOSE:
            for (int k = 0; k < button - 8; k++) {
                recon_fill_rect(p, bx + 4 + k, by + 4 + k, 2, 1, COLOR_GLYPH);
                recon_fill_rect(p, bx + 4 + k, by + button - 5 - k, 2, 1,
                    COLOR_GLYPH);
            }
            break;
        default:
            break;
        }
    }
}

static void draw(struct recon_decor *decor) {
    struct recon_panel *p = decor->panel;
    if (p == NULL) {
        return;
    }

    int ascent = recon_font_ascent(decor->font);
    int title_height = TITLE_HEIGHT;

    recon_hit_clear(p);
    recon_fill_role(p, 0, 0, decor->width, title_height,
        decor->focused ? RECON_THEME_TITLE_ACTIVE : RECON_THEME_TITLE_INACTIVE);

    /*
     * A generic application icon, because a Wayland client does not hand its
     * compositor a picture. The app_id could be looked up against the icon
     * folder later; guessing at one from a reverse-DNS string would be
     * wrong more often than right.
     */
    int text_x = TITLE_INSET;
    int icon_size = title_height - 8;
    if (recon_icon_draw(p, RECON_ICON_APP, TITLE_INSET, 4, icon_size)) {
        text_x = TITLE_INSET + icon_size + 6;
    }

    int buttons_width = 3 * BUTTON_SIZE + 2 * BUTTON_GAP + TITLE_INSET * 2;

    recon_draw_text(p, decor->font, text_x, (title_height + ascent) / 2 - 1,
        decor->width - buttons_width - text_x, decor->title,
        decor->focused ? COLOR_TITLE_TEXT : COLOR_TITLE_TEXT_INACTIVE);
    recon_hit_add(p, 0, 0, decor->width - buttons_width + TITLE_INSET,
        title_height, HIT_TITLEBAR);

    draw_buttons(decor);

    recon_stroke_rect(p, 0, 0, decor->width, title_height, COLOR_EDGE);

    /*
     * The top corners, rounded to whatever the skin asks for and cleared
     * rather than filled: there is nothing behind a title bar's top corner
     * but the desktop, which is exactly what should show through.
     */
    int radius = CORNER;
    if (radius > 0) {
        recon_round_top_corners(p, radius, COLOR_EDGE);
    }

    recon_panel_commit(p);
}

/* --- Following the window --- */

void recon_decor_update(struct recon_decor *decor) {
    if (decor == NULL || decor->panel == NULL) {
        return;
    }

    int x, y, w, h;
    window_box(decor, &x, &y, &w, &h);
    if (w <= 0) {
        return;
    }

    int title_height = TITLE_HEIGHT;
    bool focused = recon_toplevel_is_focused(decor->toplevel);
    bool maximized = decor->toplevel->maximized;

    const char *title = recon_toplevel_title(decor->toplevel);
    if (title == NULL) {
        title = "";
    }

    bool moved = (x != decor->at_x || y != decor->at_y);
    bool resized = (w != decor->width);
    bool restyled = (focused != decor->focused ||
        maximized != decor->maximized ||
        strcmp(title, decor->title) != 0);

    if (!moved && !resized && !restyled &&
            recon_panel_height(decor->panel) == title_height) {
        return;    /* Nothing to say. A terminal printing must not repaint. */
    }

    if (resized || recon_panel_height(decor->panel) != title_height) {
        if (!recon_panel_resize(decor->panel, w, title_height)) {
            return;
        }
    }

    decor->at_x = x;
    decor->at_y = y;
    decor->width = w;
    decor->focused = focused;
    decor->maximized = maximized;
    recon_text_copy(decor->title, sizeof(decor->title), title);

    /* Directly above the window, which is where the client's own bar was. */
    recon_panel_set_position(decor->panel, x, y - title_height);
    draw(decor);
}

void recon_decor_set_visible(struct recon_decor *decor, bool visible) {
    if (decor != NULL && decor->panel != NULL) {
        recon_panel_set_enabled(decor->panel, visible);
    }
}

/* --- Input --- */

static bool point_in(struct recon_decor *decor, double lx, double ly,
        int *px, int *py) {
    int x, y;
    recon_panel_position(decor->panel, &x, &y);

    int w = recon_panel_width(decor->panel);
    int h = recon_panel_height(decor->panel);

    if (lx < x || ly < y || lx >= x + w || ly >= y + h) {
        return false;
    }
    *px = (int)(lx - x);
    *py = (int)(ly - y);
    return true;
}

bool recon_decor_click(struct recon_decor *decor, double lx, double ly,
        bool pressed) {
    if (decor == NULL || decor->panel == NULL) {
        return false;
    }

    if (!pressed) {
        bool was = decor->dragging;
        decor->dragging = false;
        return was;
    }

    int px, py;
    if (!point_in(decor, lx, ly, &px, &py)) {
        return false;
    }

    /* Clicking any part of the frame brings the window forward first, the
     * way clicking the window itself does. */
    recon_focus_toplevel(decor->toplevel);

    switch ((enum hit)recon_hit_test(decor->panel, px, py)) {
    case HIT_CLOSE:
        recon_toplevel_close(decor->toplevel);
        return true;
    case HIT_MAXIMIZE:
        recon_toplevel_toggle_maximized(decor->toplevel);
        recon_decor_update(decor);
        return true;
    case HIT_MINIMIZE:
        recon_toplevel_minimize(decor->toplevel);
        return true;
    case HIT_TITLEBAR:
        decor->dragging = true;
        decor->grab_x = lx - decor->at_x;
        decor->grab_y = ly - (decor->at_y - TITLE_HEIGHT);
        return true;
    default:
        /* The frame took the click even where nothing is: a gap between two
         * buttons is still the frame, and letting it fall through would send
         * it to whatever is behind the window. */
        return true;
    }
}

void recon_decor_motion(struct recon_decor *decor, double lx, double ly) {
    if (decor == NULL || !decor->dragging) {
        return;
    }

    int title_height = TITLE_HEIGHT;
    int x = (int)(lx - decor->grab_x);
    int y = (int)(ly - decor->grab_y) + title_height;

    /* The window moves; the bar follows on the update below, which is the
     * one place the two are kept in step. */
    wlr_scene_node_set_position(&decor->toplevel->scene_tree->node, x, y);
    recon_decor_update(decor);
}

bool recon_decor_dragging(struct recon_decor *decor) {
    return decor != NULL && decor->dragging;
}

int recon_decor_reserved_top(struct recon_decor *decor) {
    return (decor != NULL) ? TITLE_HEIGHT : 0;
}

/* --- Life --- */

struct recon_decor *recon_decor_create(struct recon_server *server,
        struct recon_font *font, struct recon_toplevel *toplevel) {
    if (server == NULL || toplevel == NULL) {
        return NULL;
    }

    struct recon_decor *decor = calloc(1, sizeof(*decor));
    if (decor == NULL) {
        return NULL;
    }

    decor->server = server;
    decor->font = font;
    decor->toplevel = toplevel;

    /*
     * In the same tree as the windows rather than in the shell's, so it sits
     * with the window it belongs to instead of above every window there is.
     * A title bar that floated over other people's windows would be a worse
     * fault than the one this is fixing.
     */
    decor->panel = recon_panel_create(&server->scene->tree, 1,
        TITLE_HEIGHT);
    if (decor->panel == NULL) {
        free(decor);
        return NULL;
    }

    recon_decor_update(decor);
    return decor;
}

void recon_decor_destroy(struct recon_decor *decor) {
    if (decor == NULL) {
        return;
    }
    recon_panel_destroy(decor->panel);
    free(decor);
}
