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
#include "recon_appicon.h"
#include "recon_icons.h"
#include "recon_server.h"
#include "recon_theme.h"
#include "recon_titlebar.h"
#include "recon_ui.h"
#include "recon_widget.h"

/*
 * The same numbers a built-in window's frame uses, read from the skin the same
 * way. A client window that looked *nearly* like a ReconOS window would be
 * worse than one that plainly does not: near-misses read as a fault rather
 * than as a difference.
 */
#define TITLE_HEIGHT recon_theme_metric(RECON_METRIC_TITLE_HEIGHT)
#define BUTTON_SIZE recon_theme_metric(RECON_METRIC_BUTTON_SIZE)
#define CORNER recon_theme_metric(RECON_METRIC_CORNER)
/*
 * The same as recon_appwin's, which they were not.
 *
 * This file used 6 and 2 where the built-in frame used 8 and 3, so a client
 * window's buttons sat two pixels from where a ReconOS window's did. Nobody
 * wrote down a reason and the note at the top of this file argues against it:
 * "a client window that looked *nearly* like a ReconOS window would be worse
 * than one that plainly does not."
 */
#define TITLE_INSET 8
#define BUTTON_GAP 3

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

static void draw(struct recon_decor *decor);

static void draw_buttons(struct recon_decor *decor,
        const struct recon_titlebar_layout *bar) {
    static const enum hit IDS[RECON_TITLEBAR_BUTTON_COUNT] = {
        HIT_CLOSE, HIT_MAXIMIZE, HIT_MINIMIZE,
    };

    recon_color behind = decor->focused
        ? THEME(TITLE_ACTIVE) : THEME(TITLE_INACTIVE);

    for (int i = 0; i < RECON_TITLEBAR_BUTTON_COUNT; i++) {
        /* Left out by the skin. */
        if (bar->button[i].w == 0) {
            continue;
        }

        enum recon_widget_caption glyph;
        const char *tip;

        switch (IDS[i]) {
        case HIT_CLOSE:
            glyph = RECON_WIDGET_CAPTION_CLOSE;
            tip = "Close";
            break;
        case HIT_MAXIMIZE:
            glyph = decor->maximized ? RECON_WIDGET_CAPTION_RESTORE
                                     : RECON_WIDGET_CAPTION_MAXIMIZE;
            tip = decor->maximized ? "Restore" : "Maximize";
            break;
        default:
            glyph = RECON_WIDGET_CAPTION_MINIMIZE;
            tip = "Minimize";
            break;
        }

        recon_widget_caption_button(decor->panel, bar->button[i].x,
            bar->button[i].y, BUTTON_SIZE, (uint32_t)IDS[i], glyph, behind,
            tip);
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
     * The client's own icon where there is one, and the generic application
     * icon where there is not.
     *
     * A Wayland client hands its compositor an app_id and never a picture, so
     * every client window used to wear the same generic icon -- honest, and
     * also a taskbar where six different programs look identical.
     * recon_appicon has the three rules and the reason none of them is a
     * guess; the short version is that every answer has to be confirmed
     * against a file that exists, so the worst case is the icon that was
     * already being drawn.
     */
    const char *app_id = (decor->toplevel != NULL &&
        decor->toplevel->xdg_toplevel != NULL)
        ? decor->toplevel->xdg_toplevel->app_id : NULL;

    /* Where everything goes, from the same function the built-in frame asks --
     * which is what makes a skin that moves the buttons move them here too. */
    struct recon_titlebar_layout bar;
    recon_titlebar_layout(decor->width, TITLE_INSET, BUTTON_GAP, true, &bar);

    recon_icon_draw(p, recon_appicon_for(app_id), bar.icon.x, bar.icon.y,
        bar.icon.w);

    recon_draw_text(p, decor->font, bar.text_x, (title_height + ascent) / 2 - 1,
        bar.text_width, decor->title,
        decor->focused ? COLOR_TITLE_TEXT : COLOR_TITLE_TEXT_INACTIVE);
    recon_hit_add(p, bar.drag.x, bar.drag.y, bar.drag.w, bar.drag.h,
        HIT_TITLEBAR);

    draw_buttons(decor, &bar);

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

        /*
         * The button acts here, and only if the pointer is still on the one
         * it went down on -- the same rule the built-in frame follows, so a
         * press begun on Close and slid off it is cancelled on both kinds of
         * window rather than on one of them.
         *
         * `held` is read before it is cleared and the redraw happens before
         * the action, because closing the toplevel takes the panel that held
         * id lives on with it.
         */
        uint32_t held = recon_widget_held_id(decor->panel);
        recon_widget_release(decor->panel);

        int rx, ry;
        bool on = point_in(decor, lx, ly, &rx, &ry)
            && recon_hit_test_active(decor->panel, rx, ry) == held;
        draw(decor);

        if (on) {
            switch ((enum hit)held) {
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
            default:
                break;
            }
        }
        return was;
    }

    int px, py;
    if (!point_in(decor, lx, ly, &px, &py)) {
        return false;
    }

    /* Clicking any part of the frame brings the window forward first, the
     * way clicking the window itself does. */
    recon_focus_toplevel(decor->toplevel);

    uint32_t hit = recon_hit_test_active(decor->panel, px, py);
    if (recon_widget_press(decor->panel, hit)) {
        draw(decor);
    }

    switch ((enum hit)hit) {
    /* The three wait for the release. Acting on the press meant the window
     * was gone before the frame showing the button sunk was ever drawn. */
    case HIT_CLOSE:
    case HIT_MAXIMIZE:
    case HIT_MINIMIZE:
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
    if (decor == NULL) {
        return;
    }

    /*
     * Highlight first, and whether or not this frame is being dragged.
     *
     * The early return below used to be the first thing in the function, so a
     * frame that was not being dragged never saw the pointer at all -- which
     * was fine while nothing on it reacted, and is why nothing on it reacted.
     */
    if (!decor->dragging && decor->panel != NULL) {
        int px, py;
        uint32_t over = point_in(decor, lx, ly, &px, &py)
            ? recon_hit_test_active(decor->panel, px, py) : RECON_HIT_NONE;
        if (recon_widget_hover(decor->panel, over)) {
            draw(decor);
        }
    }

    if (!decor->dragging) {
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
