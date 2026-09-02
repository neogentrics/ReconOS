/*
 * Built-in application windows.
 *
 * A window ReconOS draws itself, for programs that are part of the system
 * rather than clients connecting to it. The frame -- border, title bar,
 * minimize, maximize and close -- belongs to the framework, so an application
 * supplies only its contents and gets window behaviour for free. Nothing an
 * app does can leave its buttons half-wired, because it does not implement
 * them.
 *
 * Client windows are separate (they draw their own decorations, for now), but
 * both kinds appear on the taskbar and both minimize, maximize and restore the
 * same way.
 */

#ifndef RECON_APPWIN_H
#define RECON_APPWIN_H

#include <stdbool.h>
#include <stdint.h>

#include <xkbcommon/xkbcommon.h>

struct recon_server;
struct recon_font;
struct recon_panel;
struct recon_appwin;

/* Hit-region ids at or above this belong to the application. */
#define RECON_APPWIN_HIT_USER 1000

/*
 * What an application must provide. Only `draw` is required; the rest may be
 * NULL for a window that shows something without accepting input.
 */
struct recon_appwin_impl {
    const char *title;
    int default_width, default_height;
    int min_width, min_height;

    /*
     * Paint the content area. The rectangle given excludes the frame, and
     * hit regions added here should use ids at or above
     * RECON_APPWIN_HIT_USER.
     */
    void (*draw)(void *user, struct recon_panel *panel, int x, int y, int w, int h);

    /* A click on a region the application registered. */
    bool (*click)(void *user, uint32_t hit_id, int cx, int cy, bool pressed);

    /* A key press while this window has focus. */
    bool (*key)(void *user, xkb_keysym_t sym, uint32_t modifiers);

    void (*scroll)(void *user, double delta);

    /* Shown or hidden, so an application can start and stop doing work. */
    void (*visibility)(void *user, bool visible);

    /* Free whatever `user` points at. */
    void (*destroy)(void *user);
};

struct recon_appwin *recon_appwin_create(struct recon_server *server,
    struct recon_font *font, const struct recon_appwin_impl *impl, void *user);
void recon_appwin_destroy(struct recon_appwin *win);

/* --- Window state --- */

void recon_appwin_show(struct recon_appwin *win);
void recon_appwin_hide(struct recon_appwin *win);
void recon_appwin_toggle(struct recon_appwin *win);

void recon_appwin_minimize(struct recon_appwin *win);
void recon_appwin_restore(struct recon_appwin *win);
void recon_appwin_set_maximized(struct recon_appwin *win, bool maximized);

/* Open at all, whether or not currently minimized. */
bool recon_appwin_is_open(struct recon_appwin *win);
bool recon_appwin_is_minimized(struct recon_appwin *win);
bool recon_appwin_is_maximized(struct recon_appwin *win);

const char *recon_appwin_title(struct recon_appwin *win);

/*
 * Raise to the front. Focus itself is not decided here: exactly one window may
 * hold it, which is a question about all of them, so the shell sets it.
 */
void recon_appwin_focus(struct recon_appwin *win);
bool recon_appwin_is_focused(struct recon_appwin *win);
void recon_appwin_set_focused(struct recon_appwin *win, bool focused);

void recon_appwin_raise(struct recon_appwin *win);

/*
 * The scene node this window draws into.
 *
 * Lets the shell ask the scene graph which window is genuinely on top at a
 * point, rather than assuming. Testing only whether a point falls inside a
 * window is not enough: a maximized window contains every point on screen and
 * would claim clicks meant for windows stacked above it.
 */
struct wlr_scene_node *recon_appwin_node(struct recon_appwin *win);

/*
 * The cursor to show at this point, or NULL to leave it alone. Lets a resize
 * edge announce itself before the user tries to drag it.
 */
const char *recon_appwin_cursor_at(struct recon_appwin *win, double lx, double ly);

/* Repaint. Call after changing anything the window displays. */
void recon_appwin_refresh(struct recon_appwin *win);

/* Re-place for a new screen size; a maximized window is resized to fit. */
void recon_appwin_screen_changed(struct recon_appwin *win, int screen_w, int screen_h,
    int reserved_bottom);

/* --- Input --- */

bool recon_appwin_contains_point(struct recon_appwin *win, double lx, double ly);
bool recon_appwin_handle_click(struct recon_appwin *win, double lx, double ly,
    bool pressed);
void recon_appwin_handle_motion(struct recon_appwin *win, double lx, double ly);
bool recon_appwin_handle_scroll(struct recon_appwin *win, double lx, double ly,
    double delta);
bool recon_appwin_handle_key(struct recon_appwin *win, xkb_keysym_t sym,
    uint32_t modifiers);

#endif
