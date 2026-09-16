/*
 * The few things an application asks the server.
 *
 * --- why this file exists -------------------------------------------------
 *
 * `include/recon_server.h` is the compositor: it embeds a wlroots scene, a
 * seat, an output layout, a cursor manager, an xdg shell and a dozen
 * `wl_listener`s. Including it means including wayland, and **five of the
 * desktop's applications were including it for one field.**
 *
 * The File Explorer, Help and the Control Panel all want the same thing --
 * `server->shell`, so they can ask the shell to open a file or to put a window
 * up. The Control Panel also wants to know how big the screen is. None of them
 * has any business knowing what a `wl_listener` is.
 *
 * So they ask, rather than reach in. The implementation is the one file that
 * includes both this and `recon_server.h`, which is where the wayland stops.
 *
 * --- the third time this shape has come up -------------------------------
 *
 * `recon_ui.h` included xkbcommon for one typedef and held twenty-four files
 * (v0.4.42). `recon_ui.c` had twenty-eight lines of wlroots holding three
 * thousand lines of drawing (v0.4.44). This is the same thing again, smaller:
 * **a header that describes a big thing, included by files that want a small
 * one.**
 *
 * The pattern worth remembering is that none of the three was a library gap or
 * a design problem. Each was an include, and each was found by offering files
 * to a compiler rather than by reading them.
 */

#ifndef RECON_SERVER_FACTS_H
#define RECON_SERVER_FACTS_H

struct recon_server;
struct recon_shell;

/*
 * The shell: the taskbar, the Start menu, and the windows that are open.
 *
 * NULL for a NULL server, so a caller that was handed nothing gets nothing
 * rather than a fault -- these are called from application code that is often
 * running before the desktop is fully up.
 */
struct recon_shell *recon_server_shell(struct recon_server *server);

/*
 * How big the screen is, in pixels.
 *
 * Zero when there is no server or no screen yet. A caller sizing a window to
 * the screen has to handle that anyway: the Control Panel opens at a size
 * derived from this and the first thing it must not do is open at zero.
 */
int recon_server_screen_width(const struct recon_server *server);
int recon_server_screen_height(const struct recon_server *server);

/*
 * Put the chosen wallpaper on, replacing whatever is there.
 *
 * Called when the choice changes, so it takes effect without a restart -- by
 * the Control Panel's Appearance page and by the File Explorer's "set as
 * wallpaper". Both of those are applications, which is why the declaration is
 * here rather than in recon_server.h: it is something an application asks for,
 * not something the compositor keeps to itself.
 */
void recon_background_reload(struct recon_server *server);

#endif /* RECON_SERVER_FACTS_H */
