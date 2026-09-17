/*
 * The windows the shell does not own.
 *
 * --- Why this exists ---
 *
 * `scripts/port-blockers.sh` prints what stops each source that cannot build
 * with no Linux under it, beside how many lines it is holding, and its advice
 * is to look for *a small marker count beside a large line count*. Its run at
 * v0.4.61 had one row that could not be read any other way:
 *
 *     recon_shell.c   7231 lines   0 markers   wayland-server-core.h
 *
 * Seven thousand lines, the largest file in the desktop, and **zero mentions
 * of wayland anywhere in it** -- held by a single `#include "recon_server.h"`.
 * `recon_cmd.c` is the same shape at 3,626 lines, and `recon_apps.c` at 375.
 *
 * What crosses that include is not an accident, though, which is why this is
 * a header rather than a deletion: the shell calls fifteen functions out of
 * the compositor and names two of its types. **It reaches into neither.** All
 * fifty-five calls are calls; there is not one field access. Thirty-two of
 * them -- `recon_damage_all`, `recon_quit`, `recon_restart`,
 * `recon_background_luminance_at` -- are already in
 * `include/recon_server_facts.h`, which v0.4.46 created for exactly this. The
 * rest are these, and they are a different subject: not facts about the
 * server, but **a window belonging to somebody else**.
 *
 * --- What a client window is, and why the shell can only ask ---
 *
 * A `recon_toplevel` is a window a program other than ReconOS put on the
 * screen. The shell draws the taskbar button for it, decides which desktop it
 * is on and whether that desktop is showing, and can minimize, restore,
 * maximize, focus or close it -- and it cannot draw a single pixel of the
 * window itself, because those pixels are the client's.
 *
 * That asymmetry is the whole reason this type stays opaque. A built-in window
 * (`recon_appwin`) is the shell's to take apart. A client's is not, and a
 * header that let the shell see the fields would be inviting it to do
 * something it must not.
 *
 * --- Taking a copy, rather than walking the list ---
 *
 * `recon_clients()` fills an array instead of offering an iterator, and that
 * is not a matter of taste. The compositor keeps its windows in a list ordered
 * front to back, and **several of the things a caller does inside one of these
 * loops move a window within it**: minimizing sends it to the back and gives
 * the focus to the next one up, restoring brings one to the front.
 *
 * A walk that reads each window's successor *after* the body has run will
 * follow whatever the body did to the ordering. That is not hypothetical --
 * see `docs/BUGS.md` BG-210, which is the bug this interface was written to
 * remove rather than to document. A copy taken before anything happens cannot
 * have that fault, whatever the body does.
 *
 * It costs one pointer per open window on the stack, which for a screen full
 * of windows is a few hundred bytes.
 */

#ifndef RECON_CLIENTS_H
#define RECON_CLIENTS_H

#include <stdbool.h>

struct recon_server;

/*
 * A window belonging to a program that is not ReconOS.
 *
 * Opaque on purpose: what is inside it is the compositor's, and everything the
 * shell is allowed to do to one is below.
 */
struct recon_toplevel;

/*
 * As many client windows as fit, front to back, and how many were put there.
 *
 * **A copy, taken now.** The pointers stay good for as long as the windows do,
 * which is why a caller must not hold them across anything that could let a
 * client disconnect -- within one loop, which is what every caller does, there
 * is nothing that can.
 *
 * `max` is a cap on the array and not on the answer: when there are more
 * windows than fit, the first `max` are copied and `max` is returned, so a
 * caller that has to be sure it saw everything compares the two. The one
 * caller that legitimately does not care is the taskbar, which stops at the
 * edge of the screen well before any plausible array runs out.
 */
int recon_clients(struct recon_server *server, struct recon_toplevel **out,
    int max);

/*
 * How many there are, without copying any.
 *
 * For a caller that only wants the number -- and for sizing a check that the
 * array was big enough.
 */
int recon_client_count(struct recon_server *server);

/*
 * A reasonable size for that array.
 *
 * Not a limit the system enforces anywhere: the compositor's list has no
 * maximum, and a caller that must not miss a window checks the return against
 * `recon_client_count`. This is the number above which somebody has more
 * windows open than a screen can show, and it exists so that call sites do not
 * each invent one.
 */
#define RECON_CLIENTS_MAX 64

/* --- What the shell may do to one --------------------------------------- */

/*
 * Give it the keyboard, and bring it to the front.
 *
 * **Moves the window within the compositor's ordering**, which is why
 * `recon_clients` hands out a copy.
 */
void recon_focus_toplevel(struct recon_toplevel *toplevel);

/*
 * Hidden but still open, so the taskbar keeps listing it and can bring it
 * back. A window with nowhere to return from would simply be lost.
 *
 * **`recon_toplevel_minimize` moves the window to the back and focuses the
 * next one that is still visible**, so it changes the ordering twice.
 * `recon_toplevel_restore` brings one to the front. Both are the reason the
 * interface above copies.
 */
void recon_toplevel_minimize(struct recon_toplevel *toplevel);
void recon_toplevel_restore(struct recon_toplevel *toplevel);
bool recon_toplevel_is_minimized(struct recon_toplevel *toplevel);

void recon_toplevel_toggle_maximized(struct recon_toplevel *toplevel);

/*
 * Ask it to close.
 *
 * Asked, not done: a client may have something unsaved and is entitled to put
 * a question up instead. The window goes away when the client lets it, which
 * is why nothing here returns whether it worked.
 */
void recon_toplevel_close(struct recon_toplevel *toplevel);

/*
 * A client window's desktop, kept the same way a built-in window's is.
 *
 * Its surface is not the shell's to draw, but its place in the scene is the
 * shell's to switch off, which is all a desktop needs.
 */
void recon_toplevel_set_desktop(struct recon_toplevel *toplevel, int desktop);
int recon_toplevel_desktop(struct recon_toplevel *toplevel);
void recon_toplevel_set_desktop_showing(struct recon_toplevel *toplevel,
    bool showing);

const char *recon_toplevel_title(struct recon_toplevel *toplevel);

/*
 * What the client calls itself: "org.gnome.Calculator", "foot".
 *
 * Empty rather than NULL when a client has not said, so a caller can pass it
 * straight on without asking. It is what `recon_appicon` turns into a picture,
 * and it is the only thing a Wayland client offers that identifies the
 * *program* rather than the window -- a title changes with the document.
 */
const char *recon_toplevel_app_id(struct recon_toplevel *toplevel);

bool recon_toplevel_is_focused(struct recon_toplevel *toplevel);

/* The process behind a client window, or 0 if it cannot be determined. */
int recon_toplevel_pid(struct recon_toplevel *toplevel);

#endif /* RECON_CLIENTS_H */
