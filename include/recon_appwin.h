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
#include <stddef.h>
#include <stdint.h>

#include <xkbcommon/xkbcommon.h>

struct recon_server;
struct recon_font;
struct recon_panel;
struct recon_appwin;

/*
 * Keyboard modifiers, as the compositor reports them.
 *
 * Named here so an application can test for Ctrl without including wlroots
 * headers for one constant, or -- worse -- writing the bit as a number and
 * hoping. These match wlr_keyboard_modifier, which is what actually arrives.
 */
#define RECON_MOD_SHIFT (1u << 0)
#define RECON_MOD_CAPS  (1u << 1)
#define RECON_MOD_CTRL  (1u << 2)
#define RECON_MOD_ALT   (1u << 3)
#define RECON_MOD_LOGO  (1u << 6)

/* Hit-region ids at or above this belong to the application. */
#define RECON_APPWIN_HIT_USER 1000

/*
 * A menu an application offers for what is under the pointer.
 *
 * Right-clicking a file should offer things to do with the file, not with the
 * window that happens to be showing it. The shell owns the drawing and the
 * clicking -- a context menu is the same object wherever it is raised -- and
 * the application only says what should be in it.
 */

#define RECON_MENU_MAX 16

struct recon_menu_entry {
    char label[48];
    uint32_t id;
    bool enabled;
    bool separator_after;
};

struct recon_menu_spec {
    struct recon_menu_entry items[RECON_MENU_MAX];
    int count;
};

/* Append an entry. Silently ignored once full, because a menu losing its last
 * item is better than an application failing to open one. */
void recon_menu_add(struct recon_menu_spec *menu, const char *label,
    uint32_t id, bool enabled, bool separator_after);

/*
 * What an application must provide. Only `draw` is required; the rest may be
 * NULL for a window that shows something without accepting input.
 */
struct recon_appwin_impl {
    const char *title;
    /* Icon name, looked up in /System/Icons. NULL falls back to a generic
     * application icon, so a window without one still looks like a window. */
    const char *icon;
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

    /*
     * The pointer moved over the window. `hit_id` is RECON_HIT_NONE when it is
     * over nothing the application registered, or has left the window
     * entirely -- which is how a highlight knows to switch off.
     */
    void (*motion)(void *user, uint32_t hit_id, int cx, int cy);

    /*
     * The cursor to show over one of this window's own regions, or NULL to
     * leave it alone.
     *
     * The frame already changes the cursor on its resize edges. Without this,
     * anything draggable *inside* a window had no way to say so -- a column
     * boundary in the task manager could be dragged and looked exactly like
     * a column boundary that could not.
     */
    const char *(*cursor)(void *user, uint32_t hit_id);

    /*
     * The window gained or lost the keyboard.
     *
     * For putting away anything that only made sense while it was in front:
     * an open menu, a half-typed thing. A menu standing open on a window
     * behind another window belongs to nothing.
     */
    void (*focus_changed)(void *user, bool focused);

    void (*scroll)(void *user, double delta);

    /*
     * Offer a context menu for the point given, in content coordinates.
     *
     * Return true having filled `menu`, or false to let the shell offer the
     * window's own Restore/Minimize/Maximize/Close instead -- which is the
     * right answer for a right-click on a window's empty background.
     */
    bool (*context)(void *user, uint32_t hit_id, int cx, int cy,
        struct recon_menu_spec *menu);

    /* A choice made from the menu the application offered. */
    void (*context_action)(void *user, uint32_t id);

    /* Shown or hidden, so an application can start and stop doing work. */
    void (*visibility)(void *user, bool visible);

    /*
     * Report the application's own state in a few lines.
     *
     * Optional, and only for diagnosis: when a button "does nothing", the
     * question is what the application believed at the time, and nothing
     * outside it can answer that.
     */
    void (*describe)(void *user, char *out, size_t size);

    /* Free whatever `user` points at. */
    void (*destroy)(void *user);

    /*
     * The help topic about this application, by the title it has in the help
     * index. Optional; without one, F1 opens the help at its first page.
     *
     * Here rather than worked out by the shell from a window's title, because
     * the two are not the same thing and should not be made to look like it:
     * "Watchtower" is a window, "Programs" is a page, and an application can
     * be about more than one page as it moves between its own views.
     */
    const char *help;
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
/* --- Desktops --- */

/*
 * Which desktop a window belongs to.
 *
 * The shell decides what the numbers mean; a window only remembers which one
 * it was put on. Separate from being minimized, because the two look the same
 * on screen and mean different things to the taskbar: a minimized window is
 * listed on its own desktop's bar, and one on another desktop is not listed
 * there at all.
 */
void recon_appwin_set_desktop(struct recon_appwin *win, int desktop);
int recon_appwin_desktop(struct recon_appwin *win);

/* Tell a window whether the desktop it is on is the one being shown. A window
 * that goes away this way gives up the keyboard: typing into something nobody
 * can see is worse than typing nowhere. */
void recon_appwin_set_desktop_showing(struct recon_appwin *win, bool showing);

bool recon_appwin_is_open(struct recon_appwin *win);
bool recon_appwin_is_minimized(struct recon_appwin *win);
bool recon_appwin_is_maximized(struct recon_appwin *win);

const char *recon_appwin_title(struct recon_appwin *win);

/*
 * Which page of the help is about what this window is currently showing.
 *
 * An application with pages of its own -- the Control Panel -- moves this as
 * it moves between them, so F1 answers the question actually on screen rather
 * than the one the application is called after.
 */
void recon_appwin_set_help_topic(struct recon_appwin *win, const char *topic);
const char *recon_appwin_help_topic(struct recon_appwin *win);

/* The application's own state pointer, so an application's public functions
 * can be given the window and find themselves from it. */
void *recon_appwin_user(struct recon_appwin *win);

/* The server this window belongs to, and through it the shell. An application
 * that needs the shell has a window; making it carry a second pointer to the
 * same place is a second thing to keep in step. */
struct recon_server *recon_appwin_server(struct recon_appwin *win);

/*
 * The middle of the region with this id, in screen coordinates. False when the
 * window has no such region.
 *
 * Lets a test click "the Delete button" by its id rather than by working out
 * where the toolbar laid it out. The click that follows is a real one through
 * the real hit test; only the measuring is skipped.
 */
bool recon_appwin_hit_centre(struct recon_appwin *win, uint32_t id, int *x, int *y);

/*
 * Ask the user a question, without the application needing to know the shell
 * exists. The answer comes back on the callback with the index of the button
 * chosen, or -1 if it was dismissed.
 *
 * Buttons read left to right; put the safe answer last, because that is what
 * Enter and Escape both choose.
 */
void recon_appwin_ask(struct recon_appwin *win, const char *title,
    const char *message, const char *const *buttons, int button_count,
    void (*answer)(void *user, int choice));

/* Whatever the application has to say about its own state. */
void recon_appwin_describe(struct recon_appwin *win, char *out, size_t size);

/* List the window's clickable regions, for diagnosis. */
void recon_appwin_describe_hits(struct recon_appwin *win, char *out, size_t size);

/* Where the window is and how big, so it can be found from outside without
 * a second copy of the geometry. */
void recon_appwin_geometry(struct recon_appwin *win, int *x, int *y, int *w, int *h);

/*
 * What this window costs in memory, in KB.
 *
 * Its pixels, which is nearly all of it: a window holds one buffer of its own
 * size, and everything else it keeps is small beside that. Built-in
 * applications share ReconOS's process, so there is no per-application figure
 * to read from the system -- but "this window's buffer" is a real number,
 * attributable to exactly one application, and it changes when the window is
 * resized, which is the truthful thing to report.
 */
size_t recon_appwin_memory_kb(struct recon_appwin *win);

/* The top-left of the content area, inside the frame -- what an application's
 * own coordinates are relative to. */
void recon_appwin_content_origin(struct recon_appwin *win, int *x, int *y);

/*
 * Show something other than the application's name -- the file being edited,
 * say. Pass NULL or "" to go back to impl->title. The taskbar and the title
 * bar both read through recon_appwin_title, so they stay in step.
 */
void recon_appwin_set_title(struct recon_appwin *win, const char *title);
const char *recon_appwin_icon(struct recon_appwin *win);

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

/*
 * Ask the application what it offers at this point. False means it has nothing
 * to say there, and the shell should offer window actions instead.
 */
bool recon_appwin_context_at(struct recon_appwin *win, double lx, double ly,
    struct recon_menu_spec *menu);

/* Tell the application which of its own entries was chosen. */
void recon_appwin_context_action(struct recon_appwin *win, uint32_t id);

#endif
