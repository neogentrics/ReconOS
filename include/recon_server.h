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

struct recon_server {
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

    /* Position and size to restore to when unmaximized. */
    bool maximized;
    struct wlr_box restore_geometry;

    /*
     * Hidden but still open, so the taskbar keeps listing it and can bring it
     * back. A window with nowhere to return from would simply be lost.
     */
    bool minimized;
};

/* Implemented in main.c, used by the shell. */
void recon_focus_toplevel(struct recon_toplevel *toplevel);
void recon_spawn(struct recon_server *server, const char *command);
void recon_quit(struct recon_server *server);

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

/* Window state for client windows. Built-in windows have their own. */
void recon_toplevel_minimize(struct recon_toplevel *toplevel);
void recon_toplevel_restore(struct recon_toplevel *toplevel);
void recon_toplevel_toggle_maximized(struct recon_toplevel *toplevel);
bool recon_toplevel_is_minimized(struct recon_toplevel *toplevel);
const char *recon_toplevel_title(struct recon_toplevel *toplevel);
bool recon_toplevel_is_focused(struct recon_toplevel *toplevel);
/* The process behind a client window, or 0 if it cannot be determined. */
int recon_toplevel_pid(struct recon_toplevel *toplevel);

#endif
