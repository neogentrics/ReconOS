/*
 * The ReconOS desktop shell: taskbar and apps menu.
 *
 * Drawn by the compositor itself rather than by a separate bar process. The
 * shell is part of the system, not an application that happens to run on it,
 * so there is no second process to schedule, no protocol between them, and
 * nothing to keep running when the desktop is idle.
 */

#ifndef RECON_SHELL_H
#define RECON_SHELL_H

#include <stdbool.h>

struct recon_server;
struct recon_shell;

/*
 * Build the shell for a screen of the given size. Returns NULL if it could
 * not be created; the compositor still runs, just without a taskbar.
 */
struct recon_shell *recon_shell_create(struct recon_server *server,
    int screen_width, int screen_height);

void recon_shell_destroy(struct recon_shell *shell);

/* Re-lay-out for a new screen size. */
void recon_shell_resize(struct recon_shell *shell, int screen_width, int screen_height);

/* Redraw the taskbar. Call when the window list or focus changes. */
void recon_shell_refresh(struct recon_shell *shell);

/*
 * Offer a click at layout coordinates to the shell.
 *
 * Returns true if the shell consumed it, in which case the compositor should
 * not pass it to a window.
 */
bool recon_shell_handle_click(struct recon_shell *shell, double lx, double ly,
    bool pressed);

/* Height reserved at the bottom of the screen, so windows can avoid it. */
int recon_shell_reserved_bottom(struct recon_shell *shell);

/* True if the pointer is over shell chrome rather than the desktop. */
bool recon_shell_contains_point(struct recon_shell *shell, double lx, double ly);

/* Keep the shell above application windows. */
void recon_shell_raise(struct recon_shell *shell);

/*
 * The Ctrl+Alt+Del dialog: a small box asking what the user wants to do.
 *
 * Kept because it is the gesture people already know for "something is wrong",
 * and it should reach the task manager without going through a desktop that
 * may be the thing misbehaving.
 */
void recon_shell_toggle_security(struct recon_shell *shell);
bool recon_shell_security_open(struct recon_shell *shell);

/* Open the task manager directly. */
void recon_shell_open_taskmgr(struct recon_shell *shell);

/*
 * The built-in windows, for anything that needs to enumerate open windows --
 * the task manager's Applications view, for one.
 */
int recon_shell_app_count(struct recon_shell *shell);
struct recon_appwin *recon_shell_app_at(struct recon_shell *shell, int index);

/*
 * The icon an application uses, found by its title. Lets a shortcut show the
 * icon of what it points at rather than deriving a name from the title, which
 * only works while the two happen to match.
 */
const char *recon_shell_icon_for_app(struct recon_shell *shell, const char *title);

/* Open a built-in window by its registration index. */
void recon_shell_open_app(struct recon_shell *shell, int index);

/* Take focus away from every built-in window, when it goes elsewhere. */
void recon_shell_clear_app_focus(struct recon_shell *shell);

/* Offer a key to whichever built-in window has focus. */
bool recon_shell_handle_key(struct recon_shell *shell, uint32_t sym,
    uint32_t modifiers);

/* The cursor the shell wants shown at this point, or NULL for the default. */
const char *recon_shell_cursor_at(struct recon_shell *shell, double lx, double ly);

/* Report pointer motion, so a window drag or resize can follow it. */
void recon_shell_handle_motion(struct recon_shell *shell, double lx, double ly);

/* Offer a scroll to the shell. Returns true if consumed. */
bool recon_shell_handle_scroll(struct recon_shell *shell, double lx, double ly,
    double delta);

/*
 * What a right click landed on, which decides what the menu offers.
 */
enum recon_context_kind {
    RECON_CONTEXT_DESKTOP,
    RECON_CONTEXT_DESKTOP_ITEM,
    RECON_CONTEXT_TASKBAR_WINDOW,
    RECON_CONTEXT_WINDOW,
    /* A menu the application under the pointer asked for. Its entry ids mean
     * whatever that application decided they mean. */
    RECON_CONTEXT_APP,
};

/* Offer a right click to the shell. Returns true if it opened a menu. */
bool recon_shell_handle_right_click(struct recon_shell *shell, double lx, double ly);

void recon_shell_close_context(struct recon_shell *shell);

/* Close the apps menu if it is open. */
void recon_shell_close_menu(struct recon_shell *shell);

#endif
