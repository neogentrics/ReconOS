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

/* Close the apps menu if it is open. */
void recon_shell_close_menu(struct recon_shell *shell);

#endif
