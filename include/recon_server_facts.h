/*
 * The things something asks the server without including the compositor.
 *
 * It began as "the few things an *application* asks", and that is still most
 * of what is here -- but the command interpreter now asks two of these, and a
 * header whose opening sentence has stopped describing it is the thing this
 * project keeps finding in other people's code.
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

struct recon_control;
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

/*
 * Something for a window to draw on.
 *
 * A window asking for a panel is a window asking the compositor which *layer*
 * to put it in -- and the layer is a wlroots idea that a window has no other
 * reason to know about. It reached through `server->layer_windows` to say it,
 * which is what kept `src/recon_appwin.c` -- sixteen hundred lines about
 * frames, titles, buttons and edges -- off a compiler with no Linux under it.
 *
 * NULL when there is nothing to make one on, which the caller has to treat as
 * a window that cannot open. That is the honest answer rather than a panel
 * that draws nowhere: a window with a panel nothing presents looks open,
 * accepts clicks, and shows nothing.
 */
struct recon_panel *recon_server_window_panel(struct recon_server *server,
    int width, int height);

/*
 * Say that something on screen changed and the next frame must repaint all of
 * it.
 *
 * Declared here as well as in `recon_server.h`, which is deliberate rather
 * than sloppy: it is one function with one definition, and what changes is
 * which header a caller has to include to reach it. A window saying "I
 * changed" should not have to include a compositor to say so.
 *
 * Why repaint everything: drivers that do not preserve buffer contents between
 * frames cannot be given a partial repaint -- whatever is not redrawn shows
 * stale pixels. So the compositor repaints fully whenever it is told something
 * changed, and not at all otherwise.
 */
void recon_damage_all(struct recon_server *server);

/*
 * Something for the system's own screens to draw on -- the login screen, the
 * shutdown screen.
 *
 * The sibling of `recon_server_window_panel`, and separate because the *layer*
 * differs: a login screen is above every window and a window is not. Which
 * layer is a compositor idea, which is exactly why neither caller should be
 * reaching for it.
 */
struct recon_panel *recon_server_system_panel(struct recon_server *server,
    int width, int height);

/*
 * And one for the shell's own chrome -- the task bar, the menu, the tips.
 *
 * A third layer rather than a flag, because the layers are what the compositor
 * stacks by and the shell has three kinds of thing in three places: behind
 * windows, among them, and above them all. Which layer is a compositor idea,
 * which is why the caller should be naming the *kind* instead.
 */
struct recon_panel *recon_server_chrome_panel(struct recon_server *server,
    int width, int height);

/*
 * And one for what is behind everything: the wallpaper, and the desktop's own
 * icons drawn over it.
 *
 * The fourth and last layer a shell puts things in. They are four because the
 * compositor stacks by layer and there are four heights of thing -- behind,
 * among the windows, above them, and the chrome above that -- and naming the
 * *kind* is what keeps a caller from having to know which is which.
 */
struct recon_panel *recon_server_background_panel(struct recon_server *server,
    int width, int height);

/*
 * How light or dark the wallpaper is at a point, 0 to 255.
 *
 * For anything drawn straight onto the desktop, which has to stay readable
 * over a picture the system did not choose -- an icon label, mostly.
 *
 * Answers 128, neither light nor dark, when there is no wallpaper to ask
 * about, so a caller gets a usable answer rather than a special case. On a
 * machine with no compositor that is every point, and a label drawn against
 * it is as readable as one drawn against a real average.
 */
int recon_background_luminance_at(struct recon_server *server, int x, int y);

/*
 * The loop to wait on.
 *
 * `include/recon_loop.h` is what a program does with it, and this is where one
 * comes from when there is a compositor underneath. It was
 * `wl_display_get_event_loop(server->wl_display)` at four call sites, which is
 * two compositor ideas deep for something whose only use is "in 250
 * milliseconds, call me".
 *
 * That reach is what kept six files -- eight thousand lines of session, player,
 * photos, task manager, clock and audio -- off a compiler with no Linux under
 * them.
 */
struct recon_loop *recon_server_loop(struct recon_server *server);

/*
 * Stop, and start again.
 *
 * Declared here as well as in `recon_server.h`, like `recon_damage_all`: one
 * function with one definition, and what changes is which header a caller
 * needs. The login screen offering Shut Down is not a compositor thing; it
 * only looks like one because `main.c` is where the compositor happens to be.
 *
 * `recon_restart` ends the process with a status the thing that started it
 * knows means "again" -- `scripts/run.sh` and the systemd unit both know it.
 * A compositor that tore itself down and rebuilt in place would be a great
 * deal of machinery for something its parent does trivially.
 */
void recon_quit(struct recon_server *server);
void recon_restart(struct recon_server *server);

/*
 * Run a program.
 *
 * Asked for by the Start menu, by the command interpreter's `spawn`, and by
 * anything that opens a file with something other than a built-in window. It
 * forks and execs; nothing here waits for it or learns whether it worked,
 * because the thing that starts a program is not the thing that watches it.
 */
void recon_spawn(struct recon_server *server, const char *command);

/*
 * The control layer, or NULL when ReconOS is running without one.
 *
 * NULL is ordinary rather than exceptional: the socket is optional, and a
 * desktop running without one works in every respect except being driven from
 * outside.
 */
struct recon_control *recon_server_control(struct recon_server *server);

#endif /* RECON_SERVER_FACTS_H */
