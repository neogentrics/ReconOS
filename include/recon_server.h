/*
 * Shared compositor state.
 *
 * The server and window types live here rather than in main.c so the shell --
 * taskbar, menus, window frames -- can be built in its own translation unit
 * against them.
 */

#ifndef RECON_SERVER_H
#define RECON_SERVER_H

#include <stdbool.h>
#include <stdint.h>

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>

struct recon_shell;

/*
 * What the pointer is currently doing. In PASSTHROUGH the pointer belongs to
 * whatever is under it; the other two mean the compositor has grabbed it to
 * drag or resize a window.
 */
enum recon_cursor_mode {
    RECON_CURSOR_PASSTHROUGH,
    RECON_CURSOR_MOVE,
    RECON_CURSOR_RESIZE,
};

struct recon_toplevel;
struct recon_decor;
struct wlr_xdg_decoration_manager_v1;

struct recon_server {
    /* Set when a restart was asked for, so the exit status can say so. */
    bool restarting;

    struct wl_display *wl_display;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;

    struct wlr_scene *scene;
    /* Keeps scene outputs positioned in step with the output layout. */
    struct wlr_scene_output_layout *scene_layout;

    /* Desktop background. Exactly one of these is set, depending on whether
     * the wallpaper image loaded. */
    struct wlr_scene_buffer *background_buffer;
    struct wlr_scene_rect *background_rect;
    /* The size the background was built for, so it can be rebuilt when the
     * wallpaper changes without waiting for a screen to say how big it is. */
    int screen_width;
    int screen_height;

    /*
     * The pointer is drawn by wlroots on its own layer, not as a scene node.
     * A scene node would sit under the pointer and above everything else, so
     * every hit test would find the cursor instead of the window beneath it,
     * and no client would ever receive a mouse event.
     */
    struct wlr_xcursor_manager *cursor_mgr;
    struct wl_listener request_set_cursor;

    /* The desktop shell: taskbar and menus. */
    struct recon_shell *shell;

    struct wlr_xdg_shell *xdg_shell;
    struct wl_listener new_xdg_surface;

    /* Server-side decorations: ReconOS draws every client's title bar. */
    struct wlr_xdg_decoration_manager_v1 *xdg_decoration;
    struct wl_listener new_decoration;

    /* Open windows, most recently focused first. */
    struct wl_list toplevels;
    /* Where the next window will be placed, so they don't stack exactly. */
    int next_window_x, next_window_y;

    /* State for an in-progress window drag or resize. */
    enum recon_cursor_mode cursor_mode;
    struct recon_toplevel *grabbed;
    double grab_x, grab_y;
    struct wlr_box grab_geometry;
    uint32_t resize_edges;

    struct wlr_seat *seat;
    struct wlr_output_layout *output_layout;
    struct wlr_cursor *cursor;

    const char *socket_name;

    /* Screens currently connected. */
    struct wl_list outputs;

    /* Trust the driver to preserve buffer contents between frames, and redraw
     * only what changed. Off by default; see output_frame(). */
    bool partial_damage;
    /* The wallpaper is sized to the first output, so it is set up lazily. */
    bool background_ready;

    struct wl_listener new_output;
    struct wl_listener new_input;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;
};

/* An application window. */
struct recon_toplevel {
    struct wl_list link; /* recon_server.toplevels */
    struct recon_server *server;
    struct wlr_xdg_toplevel *xdg_toplevel;
    struct wlr_scene_tree *scene_tree;

    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener destroy;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    struct wl_listener request_maximize;
    struct wl_listener request_fullscreen;
    struct wl_listener request_minimize;
    struct wl_listener set_title;
    struct wl_listener commit;

    /*
     * Which desktop this window is on, and whether that desktop is showing.
     *
     * The same pair a built-in window keeps, for the same reason: minimized
     * and elsewhere both end with the window not drawn and mean different
     * things to the taskbar.
     */
    int desktop;
    bool desktop_hidden;

    /* Position and size to restore to when unmaximized. */
    bool maximized;
    struct wlr_box restore_geometry;

    /*
     * Hidden but still open, so the taskbar keeps listing it and can bring it
     * back. A window with nowhere to return from would simply be lost.
     */
    bool minimized;

    /*
     * The ReconOS title bar drawn above this window. NULL for a client that
     * draws its own.
     */
    struct recon_decor *decor;

    /*
     * Whether this client asked ReconOS who draws its title bar.
     *
     * Only clients that use xdg-decoration are decorated. A client that never
     * asks -- weston-terminal is one -- is drawing its own frame and cannot be
     * told to stop, and giving it a second title bar above the one it drew is
     * worse than the mismatch this feature exists to fix. That was not a
     * guess: it is what the screen looked like the first time this worked.
     */
    bool wants_server_decoration;
};

/* Implemented in main.c, used by the shell. */
void recon_focus_toplevel(struct recon_toplevel *toplevel);
void recon_spawn(struct recon_server *server, const char *command);
void recon_quit(struct recon_server *server);

/*
 * Stop, and ask whatever started ReconOS to start it again.
 *
 * Done by exiting with a status the launcher recognises rather than by
 * re-running ourselves in place: the compositor holds the display, the input
 * devices and a Wayland socket, and handing all of that to a fresh copy of
 * itself is a great deal of machinery for something the thing that started us
 * can do trivially.
 *
 * scripts/run.sh and the systemd unit both know the status.
 */
void recon_restart(struct recon_server *server);

/* The status to exit with when a restart was asked for. */
#define RECON_EXIT_RESTART 42

/*
 * Report that something on screen changed and the next frame must repaint
 * everything.
 *
 * Drivers that do not preserve buffer contents between frames cannot be given
 * a partial repaint: whatever is not redrawn shows stale pixels. Rather than
 * repainting forever, the compositor repaints fully whenever it is told
 * something changed, and not at all otherwise.
 */
void recon_damage_all(struct recon_server *server);

/* Put the chosen wallpaper on, replacing whatever is there. Called when the
 * choice changes, so it takes effect without a restart. */
void recon_background_reload(struct recon_server *server);

/* --- Synthetic input --- */

/*
 * Feed the system input as though it came from a mouse or keyboard.
 *
 * This exists because a desktop cannot be tested by reasoning about it. Every
 * "the button does nothing" report needs a way to press that button and watch
 * what happens, and asking a person to click while somebody else reads the
 * code is not that. These go through exactly the same entry points as real
 * input -- not a shortcut around them -- so what they exercise is what a user
 * exercises.
 *
 * Reached from the control socket via the `ui` command.
 */
void recon_inject_pointer(struct recon_server *server, int x, int y);
void recon_inject_button(struct recon_server *server, uint32_t button, bool pressed);
void recon_inject_key(struct recon_server *server, uint32_t sym, uint32_t modifiers);

/* Where the pointer is, so a test can check what it is about to click. */
void recon_pointer_position(struct recon_server *server, int *x, int *y);

/* Window state for client windows. Built-in windows have their own. */
void recon_toplevel_minimize(struct recon_toplevel *toplevel);
void recon_toplevel_restore(struct recon_toplevel *toplevel);
void recon_toplevel_toggle_maximized(struct recon_toplevel *toplevel);
void recon_toplevel_close(struct recon_toplevel *toplevel);
bool recon_toplevel_is_minimized(struct recon_toplevel *toplevel);

/* A client window's desktop, kept the same way a built-in window's is. Its
 * surface is not the shell's to draw, but its place in the scene is the
 * shell's to switch off, which is all a desktop needs. */
void recon_toplevel_set_desktop(struct recon_toplevel *toplevel, int desktop);
int recon_toplevel_desktop(struct recon_toplevel *toplevel);
void recon_toplevel_set_desktop_showing(struct recon_toplevel *toplevel,
    bool showing);
const char *recon_toplevel_title(struct recon_toplevel *toplevel);
bool recon_toplevel_is_focused(struct recon_toplevel *toplevel);
/* The process behind a client window, or 0 if it cannot be determined. */
int recon_toplevel_pid(struct recon_toplevel *toplevel);

#endif
