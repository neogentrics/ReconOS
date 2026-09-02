/*
 * Built-in application windows. See include/recon_appwin.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "recon_appwin.h"
#include "recon_server.h"
#include "recon_shell.h"
#include "recon_ui.h"

/* --- Frame metrics --- */

#define TITLE_HEIGHT 24
#define BORDER 3
#define BUTTON_SIZE 16
#define BUTTON_GAP 3
#define BUTTON_TOP 4
#define TITLE_INSET 8

/* --- Frame colours --- */

#define COLOR_FRAME RECON_RGB(0xC0, 0xC0, 0xC0)
#define COLOR_TITLE_ACTIVE RECON_RGB(0x20, 0x2A, 0x44)
#define COLOR_TITLE_INACTIVE RECON_RGB(0x6A, 0x6A, 0x72)
#define COLOR_TITLE_TEXT RECON_RGB(0xF0, 0xF0, 0xF0)
#define COLOR_BUTTON RECON_RGB(0xC8, 0xC8, 0xC8)
#define COLOR_GLYPH RECON_RGB(0x10, 0x10, 0x10)
#define COLOR_EDGE RECON_RGB(0x30, 0x30, 0x30)

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
    bool focused;

    int x, y;
    int width, height;

    /* Where to return to when unmaximized. */
    int restore_x, restore_y, restore_w, restore_h;

    bool dragging;
    double drag_offset_x, drag_offset_y;

    int screen_w, screen_h, reserved_bottom;
};

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
    }
}

static void draw_frame(struct recon_appwin *win) {
    struct recon_panel *p = win->panel;
    int ascent = recon_font_ascent(win->font);

    recon_fill(p, COLOR_FRAME);

    recon_fill_rect(p, 0, 0, win->width, TITLE_HEIGHT,
        win->focused ? COLOR_TITLE_ACTIVE : COLOR_TITLE_INACTIVE);

    /* The title bar is draggable except where the buttons are. */
    int buttons_width = 3 * BUTTON_SIZE + 2 * BUTTON_GAP + TITLE_INSET * 2;
    recon_draw_text(p, win->font, TITLE_INSET, (TITLE_HEIGHT + ascent) / 2 - 1,
        win->width - buttons_width, win->impl->title, COLOR_TITLE_TEXT);
    recon_hit_add(p, 0, 0, win->width - buttons_width + TITLE_INSET, TITLE_HEIGHT,
        HIT_TITLEBAR);

    draw_buttons(win);

    recon_draw_bevel(p, 0, 0, win->width, win->height, false);
    recon_stroke_rect(p, 0, 0, win->width, win->height, COLOR_EDGE);
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

void recon_appwin_show(struct recon_appwin *win) {
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
    recon_panel_set_enabled(win->panel, true);
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
    recon_panel_set_enabled(win->panel, false);

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
    recon_panel_set_enabled(win->panel, false);

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
    recon_panel_set_enabled(win->panel, true);
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

const char *recon_appwin_title(struct recon_appwin *win) {
    return win != NULL ? win->impl->title : "";
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
    win->focused = true;
    recon_panel_raise_to_top(win->panel);
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

bool recon_appwin_contains_point(struct recon_appwin *win, double lx, double ly) {
    int px, py;
    return win != NULL && win->open && !win->minimized &&
        to_local(win, lx, ly, &px, &py);
}

bool recon_appwin_handle_click(struct recon_appwin *win, double lx, double ly,
        bool pressed) {
    if (win == NULL || !win->open || win->minimized) {
        return false;
    }

    if (!pressed) {
        bool was_dragging = win->dragging;
        win->dragging = false;
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
    if (win == NULL || !win->dragging) {
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
