/*
 * Built-in application windows. See include/recon_appwin.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "ReconOS.h"
#include "recon_appwin.h"
#include "recon_icons.h"
#include "recon_server.h"
#include "recon_registry.h"
#include "recon_shell.h"
#include "recon_theme.h"
#include "recon_ui.h"

/* --- Frame metrics --- */

/*
 * The frame's shape, asked of the skin rather than fixed.
 *
 * Macros so that every place already written in terms of these keeps working
 * unchanged, and so a skin change takes effect on the next redraw without
 * anything having to be told. They are function calls now rather than
 * constants; these are drawing paths, not inner loops, and the alternative is
 * threading a measurement through eighteen call sites.
 */
#define TITLE_HEIGHT recon_theme_metric(RECON_METRIC_TITLE_HEIGHT)
#define BORDER recon_theme_metric(RECON_METRIC_BORDER)
#define BUTTON_SIZE recon_theme_metric(RECON_METRIC_BUTTON_SIZE)
#define CORNER recon_theme_metric(RECON_METRIC_CORNER)
#define BUTTON_GAP 3
#define BUTTON_TOP 4
#define TITLE_INSET 8

/* How close to an edge counts as grabbing it. Wider than the border, because
 * a 3px target is not one a person can reliably hit. */
#define RESIZE_MARGIN 6
#define MIN_WIDTH 160
#define MIN_HEIGHT 120

/* --- Frame colours --- */

#define COLOR_FRAME THEME(WINDOW_FRAME)
#define COLOR_TITLE_ACTIVE THEME(TITLE_ACTIVE)
#define COLOR_TITLE_INACTIVE THEME(TITLE_INACTIVE)
#define COLOR_TITLE_TEXT THEME(TITLE_TEXT)
#define COLOR_TITLE_TEXT_INACTIVE THEME(TITLE_TEXT_INACTIVE)
#define COLOR_BUTTON THEME(WINDOW_BUTTON)
#define COLOR_GLYPH THEME(WINDOW_BUTTON_GLYPH)
#define COLOR_EDGE THEME(WINDOW_EDGE)

/* Framework hit ids, kept below RECON_APPWIN_HIT_USER. */
#define HIT_TITLEBAR 1
#define HIT_MINIMIZE 2
#define HIT_MAXIMIZE 3
#define HIT_CLOSE 4

struct recon_appwin {
    struct recon_server *server;
    struct recon_font *font;
    const struct recon_appwin_impl *impl;
    void *user;

    struct recon_panel *panel;

    bool open;
    bool minimized;
    bool maximized;

    /*
     * Which desktop this window belongs to, and whether that desktop is the
     * one showing.
     *
     * Kept apart from `minimized` on purpose. Both end with the window off
     * the screen, and they mean different things to everything else: a
     * minimized window is still listed on the taskbar of the desktop it is
     * on, and one sitting on another desktop is not listed here at all.
     * Folding them together would have put every window from every desktop on
     * every taskbar, marked as put away.
     */
    int desktop;
    bool desktop_hidden;
    bool focused;

    int x, y;
    int width, height;

    /* Where to return to when unmaximized. */
    int restore_x, restore_y, restore_w, restore_h;

    bool dragging;
    double drag_offset_x, drag_offset_y;
    /* Where the window was when the drag began. A snap restores to this
     * rather than to wherever the drag ended, which is in the corner. */
    int drag_from_x, drag_from_y, drag_from_w, drag_from_h;

    /* Edge drag in progress. Zero when not resizing. */
    uint32_t resize_edges;
    double resize_start_x, resize_start_y;
    int resize_start_left, resize_start_top;
    int resize_start_width, resize_start_height;

    int screen_w, screen_h, reserved_bottom;

    /* Set by the application to override impl->title; empty means it has not
     * asked for anything but its own name. */
    char title[96];

    /* Which page of the help is about what this window is showing, from the
     * implementation and then from whatever the application says as it moves
     * between its own views. */
    char help_topic[64];

    /*
     * Whether the remembered position has been applied yet. Done on first
     * show rather than at construction, because a window does not know how
     * big the screen is until the shell has told it, and a position cannot be
     * checked as reachable without that.
     */
    bool geometry_restored;
};

static void save_geometry(struct recon_appwin *win);
static void restore_geometry(struct recon_appwin *win);

/* --- Drawing --- */

/*
 * The three window buttons, right to left: close, maximize, minimize.
 * Glyphs are drawn as rectangles rather than text so they do not depend on a
 * font having loaded.
 */
static void draw_buttons(struct recon_appwin *win) {
    struct recon_panel *p = win->panel;
    int right = win->width - TITLE_INSET;

    struct {
        uint32_t id;
        int cx;
    } buttons[3];

    for (int i = 0; i < 3; i++) {
        buttons[i].cx = right - (i + 1) * BUTTON_SIZE - i * BUTTON_GAP;
    }
    buttons[0].id = HIT_CLOSE;
    buttons[1].id = HIT_MAXIMIZE;
    buttons[2].id = HIT_MINIMIZE;

    for (int i = 0; i < 3; i++) {
        int bx = buttons[i].cx;
        int by = BUTTON_TOP;

        recon_fill_rect(p, bx, by, BUTTON_SIZE, BUTTON_SIZE, COLOR_BUTTON);
        recon_draw_bevel(p, bx, by, BUTTON_SIZE, BUTTON_SIZE, false);
        recon_hit_add(p, bx, by, BUTTON_SIZE, BUTTON_SIZE, buttons[i].id);

        switch (buttons[i].id) {
        case HIT_MINIMIZE:
            /* A bar along the bottom. */
            recon_fill_rect(p, bx + 4, by + BUTTON_SIZE - 6, 8, 2, COLOR_GLYPH);
            break;
        case HIT_MAXIMIZE:
            /* An outlined box, doubled when already maximized to suggest
             * restoring to a smaller size. */
            if (win->maximized) {
                recon_stroke_rect(p, bx + 3, by + 5, 8, 7, COLOR_GLYPH);
                recon_stroke_rect(p, bx + 5, by + 3, 8, 7, COLOR_GLYPH);
            } else {
                recon_stroke_rect(p, bx + 3, by + 3, 10, 10, COLOR_GLYPH);
                recon_fill_rect(p, bx + 3, by + 3, 10, 2, COLOR_GLYPH);
            }
            break;
        case HIT_CLOSE:
            /* A cross, drawn as two diagonals a pixel at a time. */
            for (int k = 0; k < 8; k++) {
                recon_fill_rect(p, bx + 4 + k, by + 4 + k, 2, 1, COLOR_GLYPH);
                recon_fill_rect(p, bx + 4 + k, by + 11 - k, 2, 1, COLOR_GLYPH);
            }
            break;
        default:
            break;
        }

        /*
         * The corners last, so the glyph and the bevel are rounded off with
         * the button rather than sticking out of it.
         *
         * Filled back to the title bar's colour, not cleared: this is a
         * button drawn on top of a bar that is already painted, and a hole
         * here would show the wallpaper through the middle of the frame.
         */
        int radius = recon_theme_metric(RECON_METRIC_BUTTON_CORNER);
        if (radius > 0) {
            recon_round_rect(p, bx, by, BUTTON_SIZE, BUTTON_SIZE, radius,
                win->focused ? THEME(TITLE_ACTIVE) : THEME(TITLE_INACTIVE));
        }
    }
}

static void draw_frame(struct recon_appwin *win) {
    struct recon_panel *p = win->panel;
    int ascent = recon_font_ascent(win->font);

    recon_fill(p, COLOR_FRAME);

    recon_fill_role(p, 0, 0, win->width, TITLE_HEIGHT,
        win->focused ? RECON_THEME_TITLE_ACTIVE : RECON_THEME_TITLE_INACTIVE);

    /* An icon at the left, so a window says what it is before it is read. */
    int text_x = TITLE_INSET;
    const char *icon = win->impl->icon != NULL ? win->impl->icon : RECON_ICON_APP;
    int icon_size = TITLE_HEIGHT - 8;
    if (recon_icon_draw(p, icon, TITLE_INSET, 4, icon_size)) {
        text_x = TITLE_INSET + icon_size + 6;
    }

    /* The title bar is draggable except where the buttons are. */
    int buttons_width = 3 * BUTTON_SIZE + 2 * BUTTON_GAP + TITLE_INSET * 2;
    /*
     * The colour that goes with the bar underneath it.
     *
     * This always used the active colour. title.text-inactive existed as a
     * role, was answered by all ten skins, and was read by nothing -- so an
     * unfocused window's name was drawn in the colour meant for a focused
     * one. On skins where the two bars are near-identical greys that looked
     * fine, which is why it survived; on Beacon it is white on a light blue
     * bar, and the window's own name had vanished from its title.
     */
    recon_draw_text(p, win->font, text_x, (TITLE_HEIGHT + ascent) / 2 - 1,
        win->width - buttons_width - text_x, win->impl->title,
        win->focused ? COLOR_TITLE_TEXT : COLOR_TITLE_TEXT_INACTIVE);
    recon_hit_add(p, 0, 0, win->width - buttons_width + TITLE_INSET, TITLE_HEIGHT,
        HIT_TITLEBAR);

    draw_buttons(win);

    recon_draw_bevel(p, 0, 0, win->width, win->height, false);
    recon_stroke_rect(p, 0, 0, win->width, win->height, COLOR_EDGE);

    /* Last, because anything drawn into a corner afterwards puts the square
     * back. The contents are drawn below the title bar and inside the
     * border, so they never reach up here. */
    recon_round_top_corners(p, CORNER, COLOR_EDGE);
}

void recon_appwin_refresh(struct recon_appwin *win) {
    if (win == NULL || !win->open || win->minimized) {
        return;
    }

    recon_hit_clear(win->panel);
    draw_frame(win);

    if (win->impl->draw != NULL) {
        win->impl->draw(win->user, win->panel,
            BORDER, TITLE_HEIGHT,
            win->width - BORDER * 2,
            win->height - TITLE_HEIGHT - BORDER);
    }

    recon_panel_commit(win->panel);
    recon_damage_all(win->server);
}

/* --- Geometry --- */

static void apply_geometry(struct recon_appwin *win) {
    recon_panel_resize(win->panel, win->width, win->height);
    recon_panel_set_position(win->panel, win->x, win->y);
}

void recon_appwin_screen_changed(struct recon_appwin *win, int screen_w, int screen_h,
        int reserved_bottom) {
    if (win == NULL) {
        return;
    }

    win->screen_w = screen_w;
    win->screen_h = screen_h;
    win->reserved_bottom = reserved_bottom;

    if (win->maximized) {
        win->width = screen_w;
        win->height = screen_h - reserved_bottom;
        win->x = 0;
        win->y = 0;
        apply_geometry(win);
        recon_appwin_refresh(win);
        return;
    }

    /* Keep an unmaximized window on screen if the screen shrank. */
    if (win->x + win->width > screen_w) {
        win->x = screen_w - win->width;
    }
    if (win->y + win->height > screen_h - reserved_bottom) {
        win->y = screen_h - reserved_bottom - win->height;
    }
    if (win->x < 0) {
        win->x = 0;
    }
    if (win->y < 0) {
        win->y = 0;
    }
    apply_geometry(win);
}

void recon_appwin_set_maximized(struct recon_appwin *win, bool maximized) {
    if (win == NULL || win->maximized == maximized) {
        return;
    }

    if (maximized) {
        win->restore_x = win->x;
        win->restore_y = win->y;
        win->restore_w = win->width;
        win->restore_h = win->height;

        win->x = 0;
        win->y = 0;
        win->width = win->screen_w;
        win->height = win->screen_h - win->reserved_bottom;
    } else {
        win->x = win->restore_x;
        win->y = win->restore_y;
        win->width = win->restore_w;
        win->height = win->restore_h;
    }

    win->maximized = maximized;
    apply_geometry(win);
    recon_appwin_refresh(win);

    /* Maximizing is a decision worth remembering, the same as a position. */
    if (win->geometry_restored) {
        save_geometry(win);
    }
}

/* --- Lifecycle --- */

struct recon_appwin *recon_appwin_create(struct recon_server *server,
        struct recon_font *font, const struct recon_appwin_impl *impl, void *user) {
    if (server == NULL || impl == NULL) {
        return NULL;
    }

    struct recon_appwin *win = calloc(1, sizeof(*win));
    if (win == NULL) {
        return NULL;
    }

    win->server = server;
    win->font = font;
    win->impl = impl;
    win->user = user;
    recon_text_copy(win->help_topic, sizeof(win->help_topic),
        impl->help != NULL ? impl->help : "");

    win->width = impl->default_width > 0 ? impl->default_width : 400;
    win->height = impl->default_height > 0 ? impl->default_height : 300;
    win->restore_w = win->width;
    win->restore_h = win->height;

    win->panel = recon_panel_create(&server->scene->tree, win->width, win->height);
    if (win->panel == NULL) {
        free(win);
        return NULL;
    }
    recon_panel_set_enabled(win->panel, false);

    return win;
}

void recon_appwin_destroy(struct recon_appwin *win) {
    if (win == NULL) {
        return;
    }
    if (win->impl->destroy != NULL) {
        win->impl->destroy(win->user);
    }
    recon_panel_destroy(win->panel);
    free(win);
}

/* Centre in the space not taken by the taskbar. */
static void center(struct recon_appwin *win) {
    int usable_h = win->screen_h - win->reserved_bottom;
    win->x = (win->screen_w - win->width) / 2;
    win->y = (usable_h - win->height) / 2;
    if (win->x < 0) {
        win->x = 0;
    }
    if (win->y < 0) {
        win->y = 0;
    }
}

/*
 * On screen when it is open, not put away, and on the desktop being shown.
 *
 * The three reasons a window can be invisible used to be spelled out at each
 * of the five places that enabled or disabled the panel, which is how a
 * fourth reason ends up being applied at four of them.
 */
static void apply_visibility(struct recon_appwin *win) {
    if (win == NULL || win->panel == NULL) {
        return;
    }
    recon_panel_set_enabled(win->panel,
        win->open && !win->minimized && !win->desktop_hidden);
}

void recon_appwin_set_desktop(struct recon_appwin *win, int desktop) {
    if (win == NULL) {
        return;
    }
    win->desktop = desktop;
}

int recon_appwin_desktop(struct recon_appwin *win) {
    return win != NULL ? win->desktop : 0;
}

void recon_appwin_set_desktop_showing(struct recon_appwin *win, bool showing) {
    if (win == NULL || win->desktop_hidden == !showing) {
        return;
    }
    win->desktop_hidden = !showing;

    if (win->desktop_hidden) {
        /* A window that vanishes because the desktop changed must not keep
         * the keyboard, or typing would go to something nobody can see. */
        win->focused = false;
        win->dragging = false;
        win->resize_edges = 0;
    }
    apply_visibility(win);
    recon_appwin_refresh(win);
}

void recon_appwin_show(struct recon_appwin *win) {
    if (win != NULL && !win->geometry_restored) {
        /* Set first: restoring may maximize the window, which saves, and
         * there is no reason for that to come back through here. */
        win->geometry_restored = true;
        restore_geometry(win);
    }

    if (win == NULL) {
        return;
    }

    if (!win->open) {
        win->open = true;
        if (win->x == 0 && win->y == 0) {
            center(win);
        }
        apply_geometry(win);
        if (win->impl->visibility != NULL) {
            win->impl->visibility(win->user, true);
        }
    }

    win->minimized = false;
    apply_visibility(win);
    recon_panel_raise_to_top(win->panel);
    recon_appwin_refresh(win);
}

void recon_appwin_hide(struct recon_appwin *win) {
    if (win == NULL || !win->open) {
        return;
    }

    win->open = false;
    win->minimized = false;
    win->dragging = false;
    win->focused = false;
    apply_visibility(win);

    if (win->impl->visibility != NULL) {
        win->impl->visibility(win->user, false);
    }
    recon_damage_all(win->server);
}

void recon_appwin_toggle(struct recon_appwin *win) {
    if (win == NULL) {
        return;
    }
    if (win->open && !win->minimized) {
        recon_appwin_hide(win);
    } else {
        recon_appwin_show(win);
    }
}

/*
 * Minimizing hides the window but keeps it open, so the taskbar still lists it
 * and can bring it back. A window with nowhere to return from would be lost.
 */
void recon_appwin_minimize(struct recon_appwin *win) {
    if (win == NULL || !win->open || win->minimized) {
        return;
    }

    win->minimized = true;
    win->dragging = false;
    win->focused = false;
    apply_visibility(win);

    if (win->impl->visibility != NULL) {
        win->impl->visibility(win->user, false);
    }
    recon_damage_all(win->server);
}

void recon_appwin_restore(struct recon_appwin *win) {
    if (win == NULL || !win->open || !win->minimized) {
        return;
    }

    win->minimized = false;
    apply_visibility(win);
    recon_panel_raise_to_top(win->panel);

    if (win->impl->visibility != NULL) {
        win->impl->visibility(win->user, true);
    }
    recon_appwin_refresh(win);
}

bool recon_appwin_is_open(struct recon_appwin *win) {
    return win != NULL && win->open;
}

bool recon_appwin_is_minimized(struct recon_appwin *win) {
    return win != NULL && win->minimized;
}

bool recon_appwin_is_maximized(struct recon_appwin *win) {
    return win != NULL && win->maximized;
}

void recon_appwin_ask(struct recon_appwin *win, const char *title,
        const char *message, const char *const *buttons, int button_count,
        void (*answer)(void *user, int choice)) {
    if (win == NULL || win->server == NULL) {
        return;
    }
    /* The application's own `user` pointer goes back to it, so it does not
     * have to carry a second identity through the question. */
    recon_shell_ask(win->server->shell, title, message, buttons, button_count,
        answer, win->user);
}

void recon_appwin_describe(struct recon_appwin *win, char *out, size_t size) {
    if (win == NULL || out == NULL || size == 0) {
        return;
    }
    out[0] = '\0';
    if (win->impl != NULL && win->impl->describe != NULL) {
        win->impl->describe(win->user, out, size);
    }
}

bool recon_appwin_hit_centre(struct recon_appwin *win, uint32_t id,
        int *x, int *y) {
    if (win == NULL || win->panel == NULL) {
        return false;
    }
    for (size_t i = 0;; i++) {
        int rx, ry, rw, rh;
        uint32_t rid;
        if (!recon_hit_region(win->panel, i, &rx, &ry, &rw, &rh, &rid)) {
            return false;
        }
        if (rid != id) {
            continue;
        }
        /* Panel coordinates are relative to the window's top-left. */
        if (x != NULL) { *x = win->x + rx + rw / 2; }
        if (y != NULL) { *y = win->y + ry + rh / 2; }
        return true;
    }
}

void recon_appwin_describe_hits(struct recon_appwin *win, char *out, size_t size) {
    if (win == NULL || out == NULL || size == 0) {
        return;
    }

    size_t used = 0;
    for (size_t i = 0; used < size; i++) {
        int rx, ry, rw, rh;
        uint32_t rid;
        if (!recon_hit_region(win->panel, i, &rx, &ry, &rw, &rh, &rid)) {
            break;
        }
        int written = snprintf(out + used, size - used,
            "  id %-6u %dx%d at screen %d,%d  centre %d,%d\n",
            rid, rw, rh, win->x + rx, win->y + ry,
            win->x + rx + rw / 2, win->y + ry + rh / 2);
        if (written < 0) {
            break;
        }
        used += (size_t)written;
    }
}

void recon_appwin_geometry(struct recon_appwin *win, int *x, int *y,
        int *w, int *h) {
    if (win == NULL) {
        return;
    }
    if (x != NULL) { *x = win->x; }
    if (y != NULL) { *y = win->y; }
    if (w != NULL) { *w = win->width; }
    if (h != NULL) { *h = win->height; }
}

size_t recon_appwin_memory_kb(struct recon_appwin *win) {
    if (win == NULL || win->panel == NULL) {
        return 0;
    }
    /* Four bytes a pixel, which is what the panel allocates. */
    size_t bytes = (size_t)recon_panel_width(win->panel) *
        (size_t)recon_panel_height(win->panel) * 4;
    return bytes / 1024;
}

void recon_appwin_content_origin(struct recon_appwin *win, int *x, int *y) {
    if (win == NULL) {
        return;
    }
    if (x != NULL) { *x = win->x + BORDER; }
    if (y != NULL) { *y = win->y + TITLE_HEIGHT; }
}

struct recon_server *recon_appwin_server(struct recon_appwin *win) {
    return win != NULL ? win->server : NULL;
}

void *recon_appwin_user(struct recon_appwin *win) {
    return win != NULL ? win->user : NULL;
}

/*
 * Where a window's geometry is remembered.
 *
 * Keyed on the application's own name rather than its window title: the
 * notepad's title becomes the file it is editing, and a window that remembers
 * its position under one name and looks for it under another remembers
 * nothing. Spaces become dashes because a key is a path, not a sentence.
 */
static void geometry_key(struct recon_appwin *win, const char *field,
        char *out, size_t size) {
    char name[64];
    size_t used = 0;
    for (const char *c = win->impl->title;
            *c != '\0' && used < sizeof(name) - 1; c++) {
        name[used++] = (*c == ' ') ? '-' : *c;
    }
    name[used] = '\0';

    snprintf(out, size, "windows/%s/%s", name, field);
}

/* Remember where this window is, so it opens there next time. */
static void save_geometry(struct recon_appwin *win) {
    if (win == NULL || win->impl == NULL || win->impl->title == NULL) {
        return;
    }

    char key[RECON_REGISTRY_KEY_MAX];

    /*
     * The restore geometry, not the current one. A maximized window's actual
     * size is the screen, and remembering that would mean unmaximizing it next
     * time gave you a window exactly the size of the screen.
     */
    int x = win->maximized ? win->restore_x : win->x;
    int y = win->maximized ? win->restore_y : win->y;
    int w = win->maximized ? win->restore_w : win->width;
    int h = win->maximized ? win->restore_h : win->height;

    geometry_key(win, "x", key, sizeof(key));
    recon_registry_set_int(RECON_REG_USER, key, x);
    geometry_key(win, "y", key, sizeof(key));
    recon_registry_set_int(RECON_REG_USER, key, y);
    geometry_key(win, "width", key, sizeof(key));
    recon_registry_set_int(RECON_REG_USER, key, w);
    geometry_key(win, "height", key, sizeof(key));
    recon_registry_set_int(RECON_REG_USER, key, h);
    geometry_key(win, "maximized", key, sizeof(key));
    recon_registry_set_bool(RECON_REG_USER, key, win->maximized);
}

/* Put it back where it was, if it has been here before. */
static void restore_geometry(struct recon_appwin *win) {
    if (win == NULL || win->impl == NULL || win->impl->title == NULL) {
        return;
    }

    char key[RECON_REGISTRY_KEY_MAX];

    geometry_key(win, "width", key, sizeof(key));
    int w = recon_registry_get_int(RECON_REG_USER, key, win->width);
    geometry_key(win, "height", key, sizeof(key));
    int h = recon_registry_get_int(RECON_REG_USER, key, win->height);

    /* Never smaller than the application says it can work at: a remembered
     * size from a different build could be unusable. */
    if (w >= win->impl->min_width && h >= win->impl->min_height) {
        win->width = w;
        win->height = h;
    }

    geometry_key(win, "x", key, sizeof(key));
    int x = recon_registry_get_int(RECON_REG_USER, key, win->x);
    geometry_key(win, "y", key, sizeof(key));
    int y = recon_registry_get_int(RECON_REG_USER, key, win->y);

    /*
     * Only if it still lands on screen. A position remembered from a larger
     * monitor would otherwise put the window somewhere it cannot be reached,
     * and a window you cannot reach is worse than one in the wrong place.
     */
    if (x > -(win->width - 80) && y >= 0 &&
            win->screen_w > 0 && x < win->screen_w - 80 &&
            win->screen_h > 0 && y < win->screen_h - TITLE_HEIGHT) {
        win->x = x;
        win->y = y;
    }

    geometry_key(win, "maximized", key, sizeof(key));
    if (recon_registry_get_bool(RECON_REG_USER, key, false)) {
        recon_appwin_set_maximized(win, true);
    }
}

void recon_appwin_set_help_topic(struct recon_appwin *win,
        const char *topic) {
    if (win == NULL) {
        return;
    }
    recon_text_copy(win->help_topic, sizeof(win->help_topic),
        topic != NULL ? topic : "");
}

const char *recon_appwin_help_topic(struct recon_appwin *win) {
    return (win != NULL) ? win->help_topic : "";
}

const char *recon_appwin_title(struct recon_appwin *win) {
    if (win == NULL) {
        return "";
    }
    /* An application that has set one wins: the notepad naming the file it is
     * editing is more use on a taskbar than five buttons all saying
     * "Notepad". */
    return win->title[0] != '\0' ? win->title : win->impl->title;
}

void recon_appwin_set_title(struct recon_appwin *win, const char *title) {
    if (win == NULL) {
        return;
    }
    if (title == NULL || *title == '\0') {
        win->title[0] = '\0';  /* Back to the application's own name. */
    } else {
        snprintf(win->title, sizeof(win->title), "%s", title);
    }
    recon_appwin_refresh(win);
}

const char *recon_appwin_icon(struct recon_appwin *win) {
    if (win == NULL) {
        return RECON_ICON_APP;
    }
    return win->impl->icon != NULL ? win->impl->icon : RECON_ICON_APP;
}

bool recon_appwin_is_focused(struct recon_appwin *win) {
    return win != NULL && win->focused && win->open && !win->minimized;
}

void recon_appwin_focus(struct recon_appwin *win) {
    if (win == NULL || !win->open) {
        return;
    }
    if (win->minimized) {
        recon_appwin_restore(win);
    }
    recon_panel_raise_to_top(win->panel);
    recon_appwin_refresh(win);
}

void recon_appwin_set_focused(struct recon_appwin *win, bool focused) {
    if (win == NULL || win->focused == focused) {
        return;
    }
    win->focused = focused;

    /*
     * The window is told, so it can put away anything that only made sense
     * while it had the keyboard. A menu left standing open on a window that
     * is no longer in front is a menu belonging to nothing.
     */
    if (win->impl->focus_changed != NULL) {
        win->impl->focus_changed(win->user, focused);
    }

    /* The title bar shows focus, so it has to be repainted either way. */
    recon_appwin_refresh(win);
}

struct wlr_scene_node *recon_appwin_node(struct recon_appwin *win) {
    return win != NULL ? recon_panel_node(win->panel) : NULL;
}

void recon_appwin_raise(struct recon_appwin *win) {
    if (win != NULL && win->open && !win->minimized) {
        recon_panel_raise_to_top(win->panel);
    }
}

/* --- Resizing --- */

/* Edge flags, matching the sense of wlroots' own. */
#define EDGE_TOP 1u
#define EDGE_BOTTOM 2u
#define EDGE_LEFT 4u
#define EDGE_RIGHT 8u

/* Which edges, if any, a window-local point is close enough to grab. */
static uint32_t edges_at(struct recon_appwin *win, int px, int py) {
    uint32_t edges = 0;

    if (px < RESIZE_MARGIN) {
        edges |= EDGE_LEFT;
    } else if (px >= win->width - RESIZE_MARGIN) {
        edges |= EDGE_RIGHT;
    }

    if (py < RESIZE_MARGIN) {
        edges |= EDGE_TOP;
    } else if (py >= win->height - RESIZE_MARGIN) {
        edges |= EDGE_BOTTOM;
    }

    return edges;
}

/* The cursor that tells the user which way an edge will move. */
static const char *cursor_for_edges(uint32_t edges) {
    switch (edges) {
    case EDGE_TOP: return "n-resize";
    case EDGE_BOTTOM: return "s-resize";
    case EDGE_LEFT: return "w-resize";
    case EDGE_RIGHT: return "e-resize";
    case EDGE_TOP | EDGE_LEFT: return "nw-resize";
    case EDGE_TOP | EDGE_RIGHT: return "ne-resize";
    case EDGE_BOTTOM | EDGE_LEFT: return "sw-resize";
    case EDGE_BOTTOM | EDGE_RIGHT: return "se-resize";
    default: return NULL;
    }
}

static int min_width_of(struct recon_appwin *win) {
    return win->impl->min_width > 0 ? win->impl->min_width : MIN_WIDTH;
}

static int min_height_of(struct recon_appwin *win) {
    return win->impl->min_height > 0 ? win->impl->min_height : MIN_HEIGHT;
}

/*
 * Move the grabbed edges to follow the pointer.
 *
 * Edges the user is not dragging stay where they are, which is why this works
 * from the geometry at the start of the drag rather than the current one:
 * accumulating deltas would let rounding walk the opposite edge across the
 * screen.
 */
static void process_resize(struct recon_appwin *win, double lx, double ly) {
    double dx = lx - win->resize_start_x;
    double dy = ly - win->resize_start_y;

    int left = win->resize_start_left;
    int top = win->resize_start_top;
    int width = win->resize_start_width;
    int height = win->resize_start_height;

    int min_w = min_width_of(win);
    int min_h = min_height_of(win);

    if (win->resize_edges & EDGE_LEFT) {
        int new_left = left + (int)dx;
        int new_width = width - (int)dx;
        if (new_width < min_w) {
            new_left -= min_w - new_width;
            new_width = min_w;
        }
        left = new_left;
        width = new_width;
    } else if (win->resize_edges & EDGE_RIGHT) {
        width = width + (int)dx;
        if (width < min_w) {
            width = min_w;
        }
    }

    if (win->resize_edges & EDGE_TOP) {
        int new_top = top + (int)dy;
        int new_height = height - (int)dy;
        if (new_height < min_h) {
            new_top -= min_h - new_height;
            new_height = min_h;
        }
        /* The title bar must stay reachable. */
        if (new_top < 0) {
            new_height += new_top;
            new_top = 0;
        }
        top = new_top;
        height = new_height;
    } else if (win->resize_edges & EDGE_BOTTOM) {
        height = height + (int)dy;
        if (height < min_h) {
            height = min_h;
        }
    }

    win->x = left;
    win->y = top;
    win->width = width;
    win->height = height;

    apply_geometry(win);
    recon_appwin_refresh(win);
}

/* --- Input --- */

static bool to_local(struct recon_appwin *win, double lx, double ly,
        int *px, int *py) {
    int local_x = (int)lx - win->x;
    int local_y = (int)ly - win->y;
    if (local_x < 0 || local_y < 0 ||
            local_x >= win->width || local_y >= win->height) {
        return false;
    }
    *px = local_x;
    *py = local_y;
    return true;
}

/*
 * The cursor to show at this point, or NULL to leave it alone. Lets the shell
 * indicate that an edge can be dragged before the user tries it.
 */
const char *recon_appwin_cursor_at(struct recon_appwin *win, double lx, double ly) {
    if (win == NULL || !win->open || win->minimized) {
        return NULL;
    }
    if (win->resize_edges != 0) {
        return cursor_for_edges(win->resize_edges);
    }
    if (win->maximized) {
        return NULL;
    }

    int px, py;
    if (!to_local(win, lx, ly, &px, &py)) {
        return NULL;
    }

    const char *edge = cursor_for_edges(edges_at(win, px, py));
    if (edge != NULL) {
        return edge;
    }

    /* Then whatever the window itself wants over the region under the
     * pointer. The edges win, because a resize edge is part of the frame and
     * the frame is not the application's to override. */
    if (win->impl->cursor != NULL && win->panel != NULL) {
        uint32_t hit = recon_hit_test(win->panel,
            px - BORDER, py - TITLE_HEIGHT);
        return win->impl->cursor(win->user, hit);
    }
    return NULL;
}

bool recon_appwin_contains_point(struct recon_appwin *win, double lx, double ly) {
    int px, py;
    return win != NULL && win->open && !win->minimized &&
        to_local(win, lx, ly, &px, &py);
}

/*
 * --- Snapping ---
 *
 * Drag a window until the pointer touches an edge of the screen and let go:
 * left or right fills that half, the top fills the whole screen.
 *
 * The pointer's position decides it, not the window's. A window dragged by
 * the middle of a wide title bar has its own left edge far from the screen's
 * while the hand is right against it, and the gesture people make is "put the
 * mouse where I want the window to go".
 */
#define SNAP_MARGIN 8

enum snap_edge {
    SNAP_NONE,
    SNAP_LEFT,
    SNAP_RIGHT,
    SNAP_TOP,
};

static enum snap_edge snap_for_pointer(struct recon_appwin *win,
        double lx, double ly) {
    if (win->screen_w <= 0 || win->screen_h <= 0) {
        return SNAP_NONE;
    }

    /*
     * The top is checked first. A pointer in a screen's top-left corner is
     * touching both edges, and maximizing is the less surprising of the two
     * -- a window that filled the left half when the hand was at the very top
     * would look like the gesture had been misread.
     */
    if (ly <= SNAP_MARGIN) {
        return SNAP_TOP;
    }
    if (lx <= SNAP_MARGIN) {
        return SNAP_LEFT;
    }
    if (lx >= win->screen_w - 1 - SNAP_MARGIN) {
        return SNAP_RIGHT;
    }
    return SNAP_NONE;
}

static void snap_window(struct recon_appwin *win, enum snap_edge edge) {
    if (edge == SNAP_NONE) {
        return;
    }

    if (edge == SNAP_TOP) {
        recon_appwin_set_maximized(win, true);
        return;
    }

    /*
     * Where it was before the drag, not where the drag left it, so letting go
     * at an edge and then unsnapping puts the window back where it started
     * rather than in the corner it was dragged to.
     */
    win->restore_x = win->drag_from_x;
    win->restore_y = win->drag_from_y;
    win->restore_w = win->drag_from_w;
    win->restore_h = win->drag_from_h;

    int half = win->screen_w / 2;
    win->x = (edge == SNAP_LEFT) ? 0 : win->screen_w - half;
    win->y = 0;
    win->width = half;
    win->height = win->screen_h - win->reserved_bottom;

    /*
     * Snapped counts as maximized for the restore button, which is what makes
     * it undoable: the button already means "put this back where it was", and
     * a second way of saying the same thing would be a second thing to get
     * wrong.
     */
    win->maximized = true;

    apply_geometry(win);
    recon_appwin_refresh(win);
}

bool recon_appwin_handle_click(struct recon_appwin *win, double lx, double ly,
        bool pressed) {
    if (win == NULL || !win->open || win->minimized) {
        return false;
    }

    if (!pressed) {
        bool was_dragging = win->dragging || win->resize_edges != 0;
        bool was_moving = win->dragging;
        win->dragging = false;
        win->resize_edges = 0;

        /* Let go at an edge of the screen and the window takes that edge.
         * Only for a move: a resize that happens to end at an edge is
         * somebody sizing a window against it, not asking for a half. */
        if (was_moving) {
            snap_window(win, snap_for_pointer(win, lx, ly));
        }

        /* Once it has stopped moving, not while it moves: saving on every
         * pixel of a drag would write the file hundreds of times to record
         * one decision. */
        if (was_dragging) {
            save_geometry(win);
        }
        if (win->impl->click != NULL) {
            int px, py;
            if (to_local(win, lx, ly, &px, &py)) {
                win->impl->click(win->user, 0, px - BORDER, py - TITLE_HEIGHT, false);
            }
        }
        return was_dragging || recon_appwin_contains_point(win, lx, ly);
    }

    int px, py;
    if (!to_local(win, lx, ly, &px, &py)) {
        return false;
    }

    recon_appwin_focus(win);

    /* An edge grab beats anything drawn underneath it. A maximized window has
     * no edges to drag: there is nowhere for them to go. */
    if (!win->maximized) {
        uint32_t edges = edges_at(win, px, py);
        if (edges != 0) {
            win->resize_edges = edges;
            win->resize_start_x = lx;
            win->resize_start_y = ly;
            win->resize_start_left = win->x;
            win->resize_start_top = win->y;
            win->resize_start_width = win->width;
            win->resize_start_height = win->height;
            return true;
        }
    }

    uint32_t hit = recon_hit_test(win->panel, px, py);

    switch (hit) {
    case HIT_CLOSE:
        recon_appwin_hide(win);
        return true;
    case HIT_MINIMIZE:
        recon_appwin_minimize(win);
        return true;
    case HIT_MAXIMIZE:
        recon_appwin_set_maximized(win, !win->maximized);
        return true;
    case HIT_TITLEBAR:
        /* A maximized window has nowhere to be dragged to. */
        if (!win->maximized) {
            win->dragging = true;
            win->drag_from_x = win->x;
            win->drag_from_y = win->y;
            win->drag_from_w = win->width;
            win->drag_from_h = win->height;
            win->drag_offset_x = lx - win->x;
            win->drag_offset_y = ly - win->y;
        }
        return true;
    default:
        break;
    }

    if (hit >= RECON_APPWIN_HIT_USER && win->impl->click != NULL) {
        if (win->impl->click(win->user, hit, px - BORDER, py - TITLE_HEIGHT, true)) {
            recon_appwin_refresh(win);
        }
        return true;
    }

    /* Anywhere else inside the window still belongs to it. */
    return true;
}

void recon_appwin_handle_motion(struct recon_appwin *win, double lx, double ly) {
    if (win == NULL || !win->open || win->minimized) {
        return;
    }

    if (win->resize_edges != 0) {
        process_resize(win, lx, ly);
        return;
    }

    /*
     * Tell the application where the pointer is, so it can highlight what is
     * under it. Sent whether or not anything is being dragged: an application
     * that tracks hover needs to know when the pointer leaves as much as when
     * it arrives.
     */
    if (win->impl->motion != NULL && !win->dragging && win->resize_edges == 0) {
        int px = (int)lx - win->x;
        int py = (int)ly - win->y;
        uint32_t hit = recon_appwin_contains_point(win, lx, ly)
            ? recon_hit_test(win->panel, px, py) : RECON_HIT_NONE;

        win->impl->motion(win->user, hit, px - BORDER, py - TITLE_HEIGHT);
    }

    if (!win->dragging) {
        return;
    }

    win->x = (int)(lx - win->drag_offset_x);
    win->y = (int)(ly - win->drag_offset_y);

    /* Keep the title bar reachable: a window dragged fully off screen could
     * not be dragged back. */
    if (win->y < 0) {
        win->y = 0;
    }
    if (win->x < -(win->width - 80)) {
        win->x = -(win->width - 80);
    }
    if (win->x > win->screen_w - 80) {
        win->x = win->screen_w - 80;
    }
    if (win->y > win->screen_h - win->reserved_bottom - TITLE_HEIGHT) {
        win->y = win->screen_h - win->reserved_bottom - TITLE_HEIGHT;
    }

    recon_panel_set_position(win->panel, win->x, win->y);
    recon_damage_all(win->server);
}

bool recon_appwin_handle_scroll(struct recon_appwin *win, double lx, double ly,
        double delta) {
    if (!recon_appwin_contains_point(win, lx, ly)) {
        return false;
    }
    if (win->impl->scroll != NULL) {
        win->impl->scroll(win->user, delta);
        recon_appwin_refresh(win);
    }
    return true;
}

bool recon_appwin_handle_key(struct recon_appwin *win, xkb_keysym_t sym,
        uint32_t modifiers) {
    if (win == NULL || !win->open || win->minimized || !win->focused) {
        return false;
    }
    if (win->impl->key == NULL) {
        return false;
    }
    if (win->impl->key(win->user, sym, modifiers)) {
        recon_appwin_refresh(win);
        return true;
    }
    return false;
}

/* --- Context menus --- */

void recon_menu_add(struct recon_menu_spec *menu, const char *label,
        uint32_t id, bool enabled, bool separator_after) {
    if (menu == NULL || menu->count >= RECON_MENU_MAX) {
        return;
    }
    struct recon_menu_entry *entry = &menu->items[menu->count++];
    snprintf(entry->label, sizeof(entry->label), "%s", label);
    entry->id = id;
    entry->enabled = enabled;
    entry->separator_after = separator_after;
}

bool recon_appwin_context_at(struct recon_appwin *win, double lx, double ly,
        struct recon_menu_spec *menu) {
    if (win == NULL || menu == NULL || win->impl == NULL ||
            win->impl->context == NULL) {
        return false;
    }
    if (!win->open || win->minimized ||
            !recon_appwin_contains_point(win, lx, ly)) {
        return false;
    }

    int px = (int)lx - win->x;
    int py = (int)ly - win->y;

    /* The title bar and the frame belong to the shell, which offers the
     * window's own actions there. */
    if (py < TITLE_HEIGHT) {
        return false;
    }

    uint32_t hit = recon_hit_test(win->panel, px, py);

    /* Content coordinates, so an application never has to know where its frame
     * ends -- the same ones its click handler is given. */
    menu->count = 0;
    return win->impl->context(win->user, hit, px - BORDER, py - TITLE_HEIGHT,
        menu) && menu->count > 0;
}

void recon_appwin_context_action(struct recon_appwin *win, uint32_t id) {
    if (win == NULL || win->impl == NULL || win->impl->context_action == NULL) {
        return;
    }
    win->impl->context_action(win->user, id);
    recon_appwin_refresh(win);
}
