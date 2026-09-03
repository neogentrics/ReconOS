/*
 * ReconOS - a Wayland compositor built from scratch on wlroots.
 *
 * Phase 1 of the project: the desktop shell layer. See docs/ROADMAP.md.
 *
 * Controls:
 *   Alt + Q      quit the compositor
 *   Alt + Enter  launch a terminal client
 *   Alt + Tab    cycle windows
 *   Alt + T      open the task manager
 *   Alt + C      close the focused window
 *   Ctrl + Alt + Del   the security box: task manager, or shut down
 */

#define _POSIX_C_SOURCE 200112L

/* stb_image is a single-header library; this TU provides the implementation. */
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <dirent.h>
#include <drm_fourcc.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include "ReconOS.h"
#include "recon_control.h"
#include "recon_fs.h"
#include "recon_icon_gen.h"
#include "recon_icons.h"
#include "recon_server.h"
#include "recon_apps.h"
#include "recon_modules.h"
#include "recon_capture.h"
#include "recon_wallpaper.h"
#include "recon_net.h"
#include "recon_registry.h"
#include "recon_access.h"
#include "recon_theme.h"
#include "recon_session.h"
#include "recon_shell.h"
#include "recon_users.h"

/* Where image assets live. CMake defines this; the env var overrides it so the
 * binary stays runnable when moved off the build machine. */
#ifndef RECONOS_ASSET_DIR
#define RECONOS_ASSET_DIR "assets"
#endif

/*
 * Cursor appearance. Both are overridable at runtime with
 * RECONOS_CURSOR_THEME and RECONOS_CURSOR_SIZE; a NULL theme means whatever
 * the system provides. Set here so a working cursor needs no configuration.
 */
#ifndef RECONOS_DEFAULT_CURSOR_THEME
#define RECONOS_DEFAULT_CURSOR_THEME NULL
#endif
#ifndef RECONOS_DEFAULT_CURSOR_SIZE
#define RECONOS_DEFAULT_CURSOR_SIZE 24
#endif

/* --- STRUCTS --- */

/* A keyboard. Owned by main.c; the shell has no use for it. */
struct recon_keyboard {
    struct wlr_keyboard *wlr_keyboard;
    struct recon_server *server;
    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
};

/* A screen. Owned by main.c; the shell has no use for it. */
struct recon_output {
    struct wl_list link; /* recon_server.outputs */
    struct wlr_output *wlr_output;
    struct recon_server *server;
    struct wl_listener frame;
    struct wl_listener destroy;

    /* Set when something changed and the next frame must repaint in full. */
    bool needs_full_redraw;

    /* Latches so a per-frame problem is reported once, not 60 times a second. */
    bool drew_once;
    bool warned_no_scene;
    bool warned_commit_failed;
};

/*
 * Mark every screen as needing a complete repaint, and ask for a frame.
 *
 * Called from anywhere that changes what should be on screen. Nothing calls it
 * from inside the frame handler, which is what keeps an idle desktop idle: no
 * change means no frame, and no frame means no work.
 */
void recon_damage_all(struct recon_server *server) {
    struct recon_output *output;
    wl_list_for_each(output, &server->outputs, link) {
        output->needs_full_redraw = true;
        wlr_output_schedule_frame(output->wlr_output);
    }
}

/*
 * Whether the machine offers a GPU render node.
 *
 * Its absence means all rendering is done on the CPU, which changes which
 * renderer is appropriate.
 */
static bool has_render_node(void) {
    DIR *dir = opendir("/dev/dri");
    if (dir == NULL) {
        return false;
    }

    bool found = false;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "renderD", 7) == 0) {
            found = true;
            break;
        }
    }
    closedir(dir);
    return found;
}

/* --- ASSET LOADING --- */

/*
 * A wlr_buffer backed by raw pixels from stb_image. wlroots' scene graph
 * consumes buffers rather than textures, so decoded image data is wrapped in a
 * buffer that hands out its data pointer on demand.
 */
struct recon_image_buffer {
    struct wlr_buffer base;
    unsigned char *data;
    /* Pixels straight from stb must go back to stb; rescaled pixels are ours. */
    bool data_from_stb;
    uint32_t format;
    size_t stride;
};

static void image_buffer_destroy(struct wlr_buffer *wlr_buffer) {
    struct recon_image_buffer *buf = wl_container_of(wlr_buffer, buf, base);
    if (buf->data_from_stb) {
        stbi_image_free(buf->data);
    } else {
        free(buf->data);
    }
    free(buf);
}

static bool image_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
        uint32_t flags, void **data, uint32_t *format, size_t *stride) {
    struct recon_image_buffer *buf = wl_container_of(wlr_buffer, buf, base);
    *data = buf->data;
    *format = buf->format;
    *stride = buf->stride;
    return true;
}

static void image_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
    /* Pixels stay mapped for the lifetime of the buffer. */
}

static const struct wlr_buffer_impl image_buffer_impl = {
    .destroy = image_buffer_destroy,
    .begin_data_ptr_access = image_buffer_begin_data_ptr_access,
    .end_data_ptr_access = image_buffer_end_data_ptr_access,
};

/*
 * Shrink RGBA pixels by averaging each destination pixel over the source
 * pixels it covers. Downscaling only -- enlarging would need interpolation.
 *
 * Doing this once at load means the compositor never rescales the image again:
 * the buffer ends up the size it will be drawn at, so compositing is a copy
 * rather than a resample. On a machine without a GPU that is the difference
 * between a cheap frame and an expensive one.
 */
static unsigned char *downscale_rgba(const unsigned char *src, int src_w, int src_h,
        int dst_w, int dst_h) {
    unsigned char *dst = malloc((size_t)dst_w * dst_h * 4);
    if (dst == NULL) {
        return NULL;
    }

    for (int y = 0; y < dst_h; y++) {
        int sy0 = (int)((int64_t)y * src_h / dst_h);
        int sy1 = (int)((int64_t)(y + 1) * src_h / dst_h);
        if (sy1 <= sy0) {
            sy1 = sy0 + 1;
        }

        for (int x = 0; x < dst_w; x++) {
            int sx0 = (int)((int64_t)x * src_w / dst_w);
            int sx1 = (int)((int64_t)(x + 1) * src_w / dst_w);
            if (sx1 <= sx0) {
                sx1 = sx0 + 1;
            }

            uint32_t r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const unsigned char *px = src + ((size_t)sy * src_w + sx0) * 4;
                for (int sx = sx0; sx < sx1; sx++) {
                    r += px[0];
                    g += px[1];
                    b += px[2];
                    a += px[3];
                    px += 4;
                    n++;
                }
            }

            unsigned char *out = dst + ((size_t)y * dst_w + x) * 4;
            out[0] = (unsigned char)(r / n);
            out[1] = (unsigned char)(g / n);
            out[2] = (unsigned char)(b / n);
            out[3] = (unsigned char)(a / n);
        }
    }

    return dst;
}

/* Resolve an asset name against the asset directory. Caller frees. */
static char *asset_path(const char *name) {
    const char *dir = getenv("RECONOS_ASSETS");
    if (dir == NULL || *dir == '\0') {
        dir = RECONOS_ASSET_DIR;
    }
    size_t len = strlen(dir) + 1 + strlen(name) + 1;
    char *path = malloc(len);
    if (path == NULL) {
        return NULL;
    }
    snprintf(path, len, "%s/%s", dir, name);
    return path;
}

/*
 * Copy an image out of the asset directory and into the icon set, if it is
 * not already there.
 *
 * Everything else in /System/Icons is generated. This one is a file somebody
 * drew, and it goes through the same door as the rest so that it is asked for
 * by name, cached the same way, and replaceable by dropping another file over
 * it. Doing nothing when it is already there is what makes a replacement
 * stay replaced.
 */
static void install_asset_icon(const char *asset, const char *icon_name) {
    char destination[RECON_PATH_MAX];
    snprintf(destination, sizeof(destination), "%s/%s.png",
        RECON_DIR_SYSTEM_ICONS, icon_name);
    if (recon_fs_exists("/", destination)) {
        return;
    }

    char *path = asset_path(asset);
    if (path == NULL) {
        return;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        wlr_log(WLR_INFO, "ReconOS: no '%s' to install", path);
        free(path);
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    if (size > 0 && size < 4 * 1024 * 1024) {
        char *bytes = malloc((size_t)size);
        if (bytes != NULL && fread(bytes, 1, (size_t)size, f) == (size_t)size) {
            if (recon_fs_write("/", destination, bytes, (size_t)size)) {
                wlr_log(WLR_INFO, "ReconOS: installed %s", destination);
            }
        }
        free(bytes);
    }

    fclose(f);
    free(path);
}

/* The bundled photograph, copied in as one wallpaper among the drawn ones. */
static void install_asset_wallpaper(const char *asset, const char *name) {
    char destination[RECON_PATH_MAX];
    snprintf(destination, sizeof(destination), "%s/%s",
        RECON_DIR_WALLPAPERS, name);
    if (recon_fs_exists("/", destination)) {
        return;
    }

    char *path = asset_path(asset);
    if (path == NULL) {
        return;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        free(path);
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    /* Larger than an icon, so a larger ceiling -- but still one, because an
     * asset directory is not a place to read arbitrary amounts from. */
    if (size > 0 && size < 32 * 1024 * 1024) {
        char *bytes = malloc((size_t)size);
        if (bytes != NULL && fread(bytes, 1, (size_t)size, f) == (size_t)size) {
            if (recon_fs_write("/", destination, bytes, (size_t)size)) {
                wlr_log(WLR_INFO, "ReconOS: installed %s", destination);
            }
        }
        free(bytes);
    }

    fclose(f);
    free(path);
}

/*
 * Decode an image asset into a wlr_buffer, or NULL if it can't be loaded.
 *
 * Pass a positive fit_w/fit_h to shrink an oversized image to those dimensions
 * at load time; pass 0 to keep it at its natural size.
 */
/*
 * Wrap decoded pixels in a wlr_buffer the scene graph can hold.
 *
 * Shared by the two loaders -- one reads from the asset directory, the other
 * from the ReconOS filesystem -- because the wrapping is identical and having
 * it twice is how the second copy ends up with a different pixel format.
 */
static struct wlr_buffer *image_buffer_create(unsigned char *data, int width,
        int height, bool from_stb) {
    struct recon_image_buffer *buf = calloc(1, sizeof(*buf));
    if (buf == NULL) {
        if (from_stb) {
            stbi_image_free(data);
        } else {
            free(data);
        }
        return NULL;
    }

    buf->data = data;
    buf->data_from_stb = from_stb;
    /* stb gives us bytes in R,G,B,A order, which is DRM's ABGR8888. */
    buf->format = DRM_FORMAT_ABGR8888;
    buf->stride = (size_t)width * 4;
    wlr_buffer_init(&buf->base, &image_buffer_impl, width, height);
    return &buf->base;
}

static struct wlr_buffer *load_image(const char *name, int fit_w, int fit_h) {
    char *path = asset_path(name);
    if (path == NULL) {
        return NULL;
    }

    int width, height, channels;
    unsigned char *data = stbi_load(path, &width, &height, &channels, 4);
    if (data == NULL) {
        wlr_log(WLR_ERROR, "ReconOS: could not load image '%s'", path);
        free(path);
        return NULL;
    }
    wlr_log(WLR_INFO, "ReconOS: loaded '%s' (%dx%d)", path, width, height);

    bool from_stb = true;
    if (fit_w > 0 && fit_h > 0 && (width > fit_w || height > fit_h)) {
        unsigned char *scaled = downscale_rgba(data, width, height, fit_w, fit_h);
        if (scaled != NULL) {
            wlr_log(WLR_INFO, "ReconOS: scaled '%s' to %dx%d", name, fit_w, fit_h);
            stbi_image_free(data);
            data = scaled;
            width = fit_w;
            height = fit_h;
            from_stb = false;
        } else {
            wlr_log(WLR_ERROR, "ReconOS: could not scale '%s', using full size", name);
        }
    }
    free(path);
    return image_buffer_create(data, width, height, from_stb);
}

/* --- WINDOW MANAGEMENT --- */

/* Keep the shell's own chrome above application windows. */
static void raise_chrome(struct recon_server *server) {
    recon_shell_raise(server->shell);
}

/*
 * Give a window keyboard focus and raise it.
 *
 * Wayland clients only draw themselves as focused, and only accept typing,
 * once the compositor tells them they have the keyboard. Nothing here happens
 * automatically.
 */
void recon_focus_toplevel(struct recon_toplevel *toplevel) {
    if (toplevel == NULL) {
        return;
    }
    /* Focusing a minimized window brings it back; there is no sense in
     * focusing something invisible. */
    if (toplevel->minimized) {
        recon_toplevel_restore(toplevel);
        return;
    }

    struct recon_server *server = toplevel->server;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
    struct wlr_surface *focused = seat->keyboard_state.focused_surface;

    if (focused == surface) {
        return;
    }

    /* Tell the previously focused window it is no longer active, so it can
     * redraw its title bar accordingly. */
    if (focused != NULL) {
        struct wlr_xdg_toplevel *prev = wlr_xdg_toplevel_try_from_wlr_surface(focused);
        if (prev != NULL) {
            wlr_xdg_toplevel_set_activated(prev, false);
        }
    }

    wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
    raise_chrome(server);

    /* Move to the front of the list; alt-tab and refocus-on-close use this
     * order to decide what is "most recent". */
    wl_list_remove(&toplevel->link);
    wl_list_insert(&server->toplevels, &toplevel->link);

    wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);

    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    if (keyboard != NULL) {
        wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes,
            keyboard->num_keycodes, &keyboard->modifiers);
    }

    /* Focus is one thing, held by one window. A client window taking it means
     * every built-in window loses it. */
    recon_shell_clear_app_focus(server->shell);
    recon_shell_refresh(server->shell);
    recon_damage_all(server);
}

/*
 * On screen when it is neither put away nor on another desktop.
 *
 * A client's surface is not the shell's to draw, but its place in the scene
 * is the shell's to switch off -- which is all a desktop needs.
 */
static void toplevel_apply_visibility(struct recon_toplevel *toplevel) {
    if (toplevel == NULL || toplevel->scene_tree == NULL) {
        return;
    }
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node,
        !toplevel->minimized && !toplevel->desktop_hidden);
}

bool recon_toplevel_is_minimized(struct recon_toplevel *toplevel) {
    return toplevel != NULL && toplevel->minimized;
}

void recon_toplevel_set_desktop(struct recon_toplevel *toplevel, int desktop) {
    if (toplevel != NULL) {
        toplevel->desktop = desktop;
    }
}

int recon_toplevel_desktop(struct recon_toplevel *toplevel) {
    return toplevel != NULL ? toplevel->desktop : 0;
}

void recon_toplevel_set_desktop_showing(struct recon_toplevel *toplevel,
        bool showing) {
    if (toplevel == NULL || toplevel->desktop_hidden == !showing) {
        return;
    }
    toplevel->desktop_hidden = !showing;

    if (toplevel->desktop_hidden && toplevel->server->grabbed == toplevel) {
        /* A grab pointing at a window that is no longer on screen would keep
         * moving it from the other desktop. */
        toplevel->server->grabbed = NULL;
        toplevel->server->cursor_mode = RECON_CURSOR_PASSTHROUGH;
    }
    toplevel_apply_visibility(toplevel);
}

const char *recon_toplevel_title(struct recon_toplevel *toplevel) {
    if (toplevel == NULL) {
        return "";
    }
    const char *title = toplevel->xdg_toplevel->title;
    return title != NULL ? title : "Untitled";
}

/* Wayland tells us which process is on the other end of a client connection. */
int recon_toplevel_pid(struct recon_toplevel *toplevel) {
    if (toplevel == NULL) {
        return 0;
    }
    struct wl_client *client = wl_resource_get_client(toplevel->xdg_toplevel->resource);
    if (client == NULL) {
        return 0;
    }
    pid_t pid = 0;
    wl_client_get_credentials(client, &pid, NULL, NULL);
    return (int)pid;
}

bool recon_toplevel_is_focused(struct recon_toplevel *toplevel) {
    if (toplevel == NULL || toplevel->minimized) {
        return false;
    }
    struct wl_list *first = toplevel->server->toplevels.next;
    return first == &toplevel->link;
}

/*
 * Minimizing hides the window without closing it. The taskbar keeps listing
 * it, which is the only way back.
 */
void recon_toplevel_minimize(struct recon_toplevel *toplevel) {
    if (toplevel == NULL || toplevel->minimized) {
        return;
    }

    struct recon_server *server = toplevel->server;
    toplevel->minimized = true;
    toplevel_apply_visibility(toplevel);
    wlr_log(WLR_INFO, "ReconOS: minimized '%s'", recon_toplevel_title(toplevel));

    /* Don't leave a grab pointing at a window that is no longer visible. */
    if (server->grabbed == toplevel) {
        server->grabbed = NULL;
        server->cursor_mode = RECON_CURSOR_PASSTHROUGH;
    }

    /* Move it to the back so focus falls to something still on screen. */
    wl_list_remove(&toplevel->link);
    wl_list_insert(server->toplevels.prev, &toplevel->link);

    struct recon_toplevel *next;
    wl_list_for_each(next, &server->toplevels, link) {
        if (!next->minimized) {
            recon_focus_toplevel(next);
            break;
        }
    }

    recon_shell_refresh(server->shell);
    recon_damage_all(server);
}

/* Ask a client window to close. The program decides how to comply. */
void recon_toplevel_close(struct recon_toplevel *toplevel) {
    if (toplevel != NULL) {
        wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
    }
}

void recon_toplevel_restore(struct recon_toplevel *toplevel) {
    if (toplevel == NULL || !toplevel->minimized) {
        return;
    }
    toplevel->minimized = false;
    toplevel_apply_visibility(toplevel);
    wlr_log(WLR_INFO, "ReconOS: restored '%s'", recon_toplevel_title(toplevel));
    recon_focus_toplevel(toplevel);
    recon_damage_all(toplevel->server);
}

/* The window under the given layout coordinates, if any. */
static struct recon_toplevel *toplevel_at(struct recon_server *server,
        double lx, double ly, struct wlr_surface **surface, double *sx, double *sy) {
    struct wlr_scene_node *node =
        wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
    if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
        return NULL;
    }

    struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *scene_surface =
        wlr_scene_surface_try_from_buffer(scene_buffer);
    if (scene_surface == NULL) {
        return NULL;
    }
    *surface = scene_surface->surface;

    /* Surfaces sit inside the window's scene tree, possibly nested. Walk up
     * until we reach the tree we tagged with its toplevel. */
    struct wlr_scene_tree *tree = node->parent;
    while (tree != NULL && tree->node.data == NULL) {
        tree = tree->node.parent;
    }
    return tree != NULL ? tree->node.data : NULL;
}

/* Take over the pointer to drag or resize a window. */
static void begin_interactive(struct recon_toplevel *toplevel,
        enum recon_cursor_mode mode, uint32_t edges) {
    struct recon_server *server = toplevel->server;

    /* Ignore requests from a window that isn't the one being pointed at. */
    if (toplevel->xdg_toplevel->base->surface !=
            wlr_surface_get_root_surface(server->seat->pointer_state.focused_surface)) {
        return;
    }

    server->grabbed = toplevel;
    server->cursor_mode = mode;

    struct wlr_box geometry;
    wlr_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &geometry);

    if (mode == RECON_CURSOR_MOVE) {
        /* Remember where in the window the pointer grabbed it. */
        server->grab_x = server->cursor->x - toplevel->scene_tree->node.x;
        server->grab_y = server->cursor->y - toplevel->scene_tree->node.y;
    } else {
        server->grab_x = server->cursor->x;
        server->grab_y = server->cursor->y;
        server->grab_geometry = geometry;
        server->grab_geometry.x += toplevel->scene_tree->node.x;
        server->grab_geometry.y += toplevel->scene_tree->node.y;
        server->resize_edges = edges;
    }
}

/* The area of the screen windows may use, excluding shell chrome. */
static bool output_box_for(struct recon_toplevel *toplevel, struct wlr_box *box) {
    struct recon_server *server = toplevel->server;
    struct wlr_output *output = wlr_output_layout_output_at(server->output_layout,
        toplevel->scene_tree->node.x, toplevel->scene_tree->node.y);

    if (output == NULL) {
        /* Off-screen, or no window position yet: fall back to the first screen. */
        if (wl_list_empty(&server->output_layout->outputs)) {
            return false;
        }
        struct wlr_output_layout_output *first =
            wl_container_of(server->output_layout->outputs.next, first, link);
        output = first->output;
    }

    wlr_output_layout_get_box(server->output_layout, output, box);

    /* Leave the taskbar visible: maximizing should fill the desktop, not
     * cover the shell. */
    box->height -= recon_shell_reserved_bottom(server->shell);

    return !wlr_box_empty(box);
}

/* Fill the screen, remembering where the window was so it can be restored. */
static void set_maximized(struct recon_toplevel *toplevel, bool maximized) {
    if (toplevel->maximized == maximized) {
        return;
    }

    if (maximized) {
        struct wlr_box screen;
        if (!output_box_for(toplevel, &screen)) {
            return;
        }

        struct wlr_box geometry;
        wlr_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &geometry);
        toplevel->restore_geometry = (struct wlr_box){
            .x = toplevel->scene_tree->node.x,
            .y = toplevel->scene_tree->node.y,
            .width = geometry.width,
            .height = geometry.height,
        };

        wlr_scene_node_set_position(&toplevel->scene_tree->node, screen.x, screen.y);
        wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, screen.width, screen.height);
    } else {
        wlr_scene_node_set_position(&toplevel->scene_tree->node,
            toplevel->restore_geometry.x, toplevel->restore_geometry.y);
        wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
            toplevel->restore_geometry.width, toplevel->restore_geometry.height);
    }

    toplevel->maximized = maximized;
    wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, maximized);
}

void recon_toplevel_toggle_maximized(struct recon_toplevel *toplevel) {
    if (toplevel != NULL) {
        set_maximized(toplevel, !toplevel->maximized);
        wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
    }
}

static void toplevel_request_maximize(struct wl_listener *listener, void *data) {
    struct recon_toplevel *toplevel = wl_container_of(listener, toplevel, request_maximize);

    if (toplevel->xdg_toplevel->base->initialized) {
        set_maximized(toplevel, toplevel->xdg_toplevel->requested.maximized);
        /* xdg-shell requires a configure in reply to a state request, whether
         * or not the state actually changed. */
        wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
    }
}

/* Fullscreen is treated as maximize for now: no chrome exists to hide yet. */
static void toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
    struct recon_toplevel *toplevel =
        wl_container_of(listener, toplevel, request_fullscreen);

    if (toplevel->xdg_toplevel->base->initialized) {
        bool wants = toplevel->xdg_toplevel->requested.fullscreen;
        set_maximized(toplevel, wants);
        wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, wants);
        wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
    }
}

/*
 * Minimize is acknowledged but not acted on: a hidden window needs a taskbar
 * to bring it back, and there isn't one yet. Replying is still required.
 */
static void toplevel_request_minimize(struct wl_listener *listener, void *data) {
    struct recon_toplevel *toplevel =
        wl_container_of(listener, toplevel, request_minimize);

    if (toplevel->xdg_toplevel->base->initialized) {
        wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
    }
}

static void toplevel_request_move(struct wl_listener *listener, void *data) {
    struct recon_toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
    begin_interactive(toplevel, RECON_CURSOR_MOVE, 0);
}

static void toplevel_request_resize(struct wl_listener *listener, void *data) {
    struct recon_toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
    struct wlr_xdg_toplevel_resize_event *event = data;
    begin_interactive(toplevel, RECON_CURSOR_RESIZE, event->edges);
}

/* --- INPUT: POINTER --- */

/* Drag the grabbed window to follow the pointer. */
static void process_move(struct recon_server *server) {
    struct recon_toplevel *toplevel = server->grabbed;
    wlr_scene_node_set_position(&toplevel->scene_tree->node,
        server->cursor->x - server->grab_x,
        server->cursor->y - server->grab_y);
    recon_damage_all(server);
}

/*
 * Resize the grabbed window.
 *
 * The compositor decides the new geometry, but only the client can actually
 * resize itself, so this asks for a size and repositions the window to keep
 * the edges the user is not dragging where they were.
 */
static void process_resize(struct recon_server *server) {
    struct recon_toplevel *toplevel = server->grabbed;
    double dx = server->cursor->x - server->grab_x;
    double dy = server->cursor->y - server->grab_y;

    int left = server->grab_geometry.x;
    int right = server->grab_geometry.x + server->grab_geometry.width;
    int top = server->grab_geometry.y;
    int bottom = server->grab_geometry.y + server->grab_geometry.height;

    if (server->resize_edges & WLR_EDGE_TOP) {
        top += dy;
        if (top >= bottom) {
            top = bottom - 1;
        }
    } else if (server->resize_edges & WLR_EDGE_BOTTOM) {
        bottom += dy;
        if (bottom <= top) {
            bottom = top + 1;
        }
    }
    if (server->resize_edges & WLR_EDGE_LEFT) {
        left += dx;
        if (left >= right) {
            left = right - 1;
        }
    } else if (server->resize_edges & WLR_EDGE_RIGHT) {
        right += dx;
        if (right <= left) {
            right = left + 1;
        }
    }

    struct wlr_box geometry;
    wlr_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &geometry);
    wlr_scene_node_set_position(&toplevel->scene_tree->node,
        left - geometry.x, top - geometry.y);
    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, right - left, bottom - top);
    recon_damage_all(server);
}

/*
 * Point the pointer at whatever is under it.
 *
 * Wayland clients receive no pointer events until the compositor tells them
 * the pointer has entered their surface, so this has to run on every motion.
 */
static void process_cursor_motion(struct recon_server *server, uint32_t time) {
    if (server->cursor_mode == RECON_CURSOR_MOVE) {
        process_move(server);
        return;
    }
    if (server->cursor_mode == RECON_CURSOR_RESIZE) {
        process_resize(server);
        return;
    }

    /* Over the taskbar or a menu: the shell owns the pointer, not a client. */
    if (recon_shell_contains_point(server->shell, server->cursor->x, server->cursor->y)) {
        const char *shape = recon_shell_cursor_at(server->shell,
            server->cursor->x, server->cursor->y);
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
            shape != NULL ? shape : "default");
        wlr_seat_pointer_clear_focus(server->seat);
        return;
    }

    double sx, sy;
    struct wlr_surface *surface = NULL;
    toplevel_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);

    if (surface != NULL) {
        wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(server->seat, time, sx, sy);
    } else {
        /* Over the desktop: no client should think it still has the pointer,
         * and the shell's own cursor image applies again. */
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
        wlr_seat_pointer_clear_focus(server->seat);
    }
}

/*
 * A client asking to change the cursor image, e.g. a terminal switching to an
 * I-beam over its text area. Honour it only for the client actually holding
 * the pointer, so a background window cannot hijack the cursor.
 */
static void seat_request_set_cursor(struct wl_listener *listener, void *data) {
    struct recon_server *server = wl_container_of(listener, server, request_set_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *event = data;

    if (server->seat->pointer_state.focused_client == event->seat_client) {
        wlr_cursor_set_surface(server->cursor, event->surface,
            event->hotspot_x, event->hotspot_y);
    }
}

/*
 * What a button press means, independent of where the event came from.
 *
 * Split out so injected input takes the identical path to real input. A test
 * harness that goes around the thing it is testing proves nothing.
 *
 * Returns true when the shell consumed the event, so a real event knows
 * whether the seat still needs telling.
 */
static bool dispatch_button(struct recon_server *server, uint32_t button,
        bool pressed) {
    if (!pressed) {
        /* Any drag or resize ends when the button comes up. */
        server->cursor_mode = RECON_CURSOR_PASSTHROUGH;
        server->grabbed = NULL;
        recon_shell_handle_click(server->shell, server->cursor->x, server->cursor->y,
            false);
        return false;
    }

    if (button == BTN_RIGHT) {
        /* Right click opens a context menu where there is one to open. */
        return recon_shell_handle_right_click(server->shell,
            server->cursor->x, server->cursor->y);
    }

    if (button == BTN_LEFT) {
        double x = server->cursor->x;
        double y = server->cursor->y;

        /*
         * The shell sits above windows, so it sees clicks first.
         *
         * The context menu is deliberately NOT closed here. Closing it first
         * cleared `context_open` before the shell could look at it, so the
         * branch that turns a click into a menu choice never ran and every
         * context menu entry silently did nothing. The shell closes the menu
         * itself, after working out what was chosen.
         */
        if (recon_shell_handle_click(server->shell, x, y, true)) {
            return true;
        }

        /* Clicking a window focuses it. */
        double sx, sy;
        struct wlr_surface *surface = NULL;
        struct recon_toplevel *toplevel =
            toplevel_at(server, x, y, &surface, &sx, &sy);
        if (toplevel != NULL) {
            recon_focus_toplevel(toplevel);
        }
    }

    return false;
}

void recon_inject_pointer(struct recon_server *server, int x, int y) {
    if (server == NULL || server->cursor == NULL) {
        return;
    }
    wlr_cursor_warp_closest(server->cursor, NULL, (double)x, (double)y);
    recon_shell_handle_motion(server->shell, server->cursor->x, server->cursor->y);
    recon_damage_all(server);
}

void recon_inject_button(struct recon_server *server, uint32_t button, bool pressed) {
    if (server == NULL) {
        return;
    }
    dispatch_button(server, button, pressed);
    recon_damage_all(server);
}

/* Defined below with the rest of the input handling; the injection path
 * needs it above, so that a key sent from outside takes the same route a key
 * from a keyboard does. */
static bool handle_shortcut(struct recon_server *server, uint32_t modifiers,
    xkb_keysym_t sym);

void recon_inject_key(struct recon_server *server, uint32_t sym, uint32_t modifiers) {
    if (server == NULL) {
        return;
    }

    /*
     * The same order a real key takes: the system's shortcuts first, then the
     * focused window.
     *
     * This went straight to the shell, which meant no system shortcut could
     * be driven from outside at all -- not Alt+Tab, not Alt+Q, not
     * Ctrl+Alt+Delete, not Print Screen. So none of them were ever tested,
     * and Alt+Tab had quietly stopped working: it cycled the compositor's
     * *client* windows, and once ReconOS drew its own there were usually no
     * clients to cycle. A shortcut nothing can press is a shortcut nothing
     * notices the loss of.
     */
    if (!handle_shortcut(server, modifiers, sym)) {
        recon_shell_handle_key(server->shell, sym, modifiers);
    }
    recon_damage_all(server);
}

void recon_pointer_position(struct recon_server *server, int *x, int *y) {
    if (server == NULL || server->cursor == NULL) {
        return;
    }
    if (x != NULL) {
        *x = (int)server->cursor->x;
    }
    if (y != NULL) {
        *y = (int)server->cursor->y;
    }
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
    struct recon_server *server = wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;

    if (dispatch_button(server, event->button,
            event->state != WL_POINTER_BUTTON_STATE_RELEASED)) {
        return;
    }

    wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button,
        event->state);
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
    struct recon_server *server = wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;

    wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    recon_shell_handle_motion(server->shell, server->cursor->x, server->cursor->y);
    process_cursor_motion(server, event->time_msec);
    recon_damage_all(server);
}

static void server_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    struct recon_server *server = wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;

    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
    recon_shell_handle_motion(server->shell, server->cursor->x, server->cursor->y);
    process_cursor_motion(server, event->time_msec);
    recon_damage_all(server);
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
    struct recon_server *server = wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *event = data;

    if (recon_shell_handle_scroll(server->shell, server->cursor->x, server->cursor->y,
            event->delta)) {
        return;
    }

    wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation,
        event->delta, event->delta_discrete, event->source);
}

/*
 * Pointer events are sent in batches, and clients wait for the frame event
 * before acting on them.
 */
static void server_cursor_frame(struct wl_listener *listener, void *data) {
    struct recon_server *server = wl_container_of(listener, server, cursor_frame);
    wlr_seat_pointer_notify_frame(server->seat);
}

/* --- INPUT: KEYBOARD --- */

/* Shut the compositor down. */
void recon_quit(struct recon_server *server) {
    wlr_log(WLR_INFO, "ReconOS: shutting down");
    wl_display_terminate(server->wl_display);
}

void recon_restart(struct recon_server *server) {
    wlr_log(WLR_INFO, "ReconOS: restarting");
    server->restarting = true;
    wl_display_terminate(server->wl_display);
}

/* Launch a client. A NULL command means the configured terminal. */
void recon_spawn(struct recon_server *server, const char *command) {
    const char *term = command;
    if (term == NULL || *term == '\0') {
        term = getenv("RECONOS_TERMINAL");
    }
    if (term == NULL || *term == '\0') {
        term = "weston-terminal";
    }

    wlr_log(WLR_INFO, "ReconOS: launching '%s' on %s", term, server->socket_name);

    pid_t pid = fork();
    if (pid < 0) {
        wlr_log(WLR_ERROR, "ReconOS: fork failed, cannot launch '%s'", term);
        return;
    }

    if (pid == 0) {
        /* Point the child at our compositor socket. XDG_RUNTIME_DIR is
         * inherited, so it resolves the socket in the same place we created it. */
        setenv("WAYLAND_DISPLAY", server->socket_name, 1);
        execlp(term, term, (char *)NULL);
        /* Only reached if exec failed. */
        fprintf(stderr, "ReconOS: failed to exec '%s'\n", term);
        _exit(127);
    }
}

/* Focus the window after the currently focused one, wrapping around. */
static void cycle_focus(struct recon_server *server) {
    if (wl_list_length(&server->toplevels) < 2) {
        return;
    }

    /* Walk forward to the next window that is actually on screen. */
    struct recon_toplevel *current =
        wl_container_of(server->toplevels.next, current, link);
    struct wl_list *node = current->link.next;

    while (node != &current->link) {
        if (node == &server->toplevels) {
            node = node->next; /* skip the list head */
            continue;
        }
        struct recon_toplevel *candidate = wl_container_of(node, candidate, link);
        if (!candidate->minimized) {
            recon_focus_toplevel(candidate);
            return;
        }
        node = node->next;
    }
}

/* Ask the focused window to close. The client decides how to comply. */
static void close_focused(struct recon_server *server) {
    if (wl_list_empty(&server->toplevels)) {
        return;
    }
    struct recon_toplevel *focused =
        wl_container_of(server->toplevels.next, focused, link);
    wlr_xdg_toplevel_send_close(focused->xdg_toplevel);
}

/*
 * Compositor-level shortcuts. Returns true if the key was consumed and should
 * not reach the focused window.
 */
static bool handle_shortcut(struct recon_server *server, uint32_t modifiers,
        xkb_keysym_t sym) {
    /* Ctrl+Alt+Delete, kept because it is the gesture people already reach for
     * when something has gone wrong. */
    if ((modifiers & WLR_MODIFIER_CTRL) && (modifiers & WLR_MODIFIER_ALT) &&
            (sym == XKB_KEY_Delete || sym == XKB_KEY_BackSpace)) {
        recon_shell_toggle_security(server->shell);
        return true;
    }

    /*
     * Print Screen, with no modifier, because that is the key's whole job and
     * every system that has one treats it that way. Handled before the Alt
     * check for the same reason.
     */
    if (sym == XKB_KEY_Print) {
        if (recon_capture_request(server, NULL)) {
            wlr_log(WLR_INFO, "ReconOS: capturing the screen");
        } else {
            wlr_log(WLR_INFO, "ReconOS: %s", recon_capture_last_error());
        }
        return true;
    }

    if (!(modifiers & WLR_MODIFIER_ALT)) {
        return false;
    }

    switch (sym) {
    case XKB_KEY_q:
    case XKB_KEY_Q:
        wlr_log(WLR_INFO, "ReconOS: Alt+Q, shutting down");
        wl_display_terminate(server->wl_display);
        return true;
    case XKB_KEY_n:
    case XKB_KEY_N:
        recon_shell_open_app(server->shell, 2);
        return true;
    case XKB_KEY_Tab:
    case XKB_KEY_ISO_Left_Tab:
        /*
         * Over what the taskbar lists, not over the compositor's client
         * windows. cycle_focus walked the latter, which was right when the
         * only windows were clients and became a shortcut that did nothing
         * once ReconOS drew its own -- a desktop with a Notepad and a
         * Terminal on it has no clients in that list at all.
         */
        recon_shell_cycle_windows(server->shell);
        return true;
    case XKB_KEY_t:
    case XKB_KEY_T:
        recon_shell_open_taskmgr(server->shell);
        return true;

    /*
     * Alt+1..4 goes to a desktop; adding Shift takes the current window
     * along. A number key each is what makes four desktops learnable, and it
     * is the same number the pager on the taskbar shows.
     */
    case XKB_KEY_1: case XKB_KEY_2: case XKB_KEY_3: case XKB_KEY_4:
        recon_shell_set_desktop(server->shell, (int)(sym - XKB_KEY_1));
        return true;
    case XKB_KEY_exclam:   /* Shift+1 and friends, which is what the */
    case XKB_KEY_at:       /* keyboard actually sends. */
    case XKB_KEY_numbersign:
    case XKB_KEY_dollar: {
        static const xkb_keysym_t SHIFTED[] = {
            XKB_KEY_exclam, XKB_KEY_at, XKB_KEY_numbersign, XKB_KEY_dollar,
        };
        for (int i = 0; i < (int)(sizeof(SHIFTED) / sizeof(SHIFTED[0])); i++) {
            if (sym == SHIFTED[i]) {
                recon_shell_move_to_desktop(server->shell, i);
                break;
            }
        }
        return true;
    }
    case XKB_KEY_Return:
        /* The ReconOS terminal, in the place the old one used to be. */
        recon_shell_open_app(server->shell, 3);
        return true;
    case XKB_KEY_c:
    case XKB_KEY_C:
        close_focused(server);
        return true;
    default:
        return false;
    }
}

static void server_keyboard_key(struct wl_listener *listener, void *data) {
    struct recon_keyboard *keyboard = wl_container_of(listener, keyboard, key);
    struct recon_server *server = keyboard->server;
    struct wlr_keyboard_key_event *event = data;

    /* libinput keycodes are offset by 8 from xkb's. */
    uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state, keycode, &syms);

    bool handled = false;
    uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);

    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (int i = 0; i < nsyms; i++) {
            if (handle_shortcut(server, modifiers, syms[i])) {
                handled = true;
                break;
            }
        }
        /* A focused built-in window gets the keys the shell did not claim,
         * which is how the calculator can be driven from the number pad. */
        if (!handled) {
            for (int i = 0; i < nsyms; i++) {
                if (recon_shell_handle_key(server->shell, syms[i], modifiers)) {
                    handled = true;
                    break;
                }
            }
        }
    }

    /* Everything the shell didn't claim belongs to the focused window. Without
     * this, typing goes nowhere and applications appear frozen. */
    if (!handled) {
        wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
        wlr_seat_keyboard_notify_key(server->seat, event->time_msec,
            event->keycode, event->state);
    }
}

static void server_keyboard_modifiers(struct wl_listener *listener, void *data) {
    struct recon_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
    wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
        &keyboard->wlr_keyboard->modifiers);
}

static void server_new_keyboard(struct recon_server *server,
        struct wlr_input_device *device) {
    struct recon_keyboard *keyboard = calloc(1, sizeof(*keyboard));
    if (keyboard == NULL) {
        return;
    }
    keyboard->server = server;
    keyboard->wlr_keyboard = wlr_keyboard_from_input_device(device);

    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(keyboard->wlr_keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);

    keyboard->key.notify = server_keyboard_key;
    wl_signal_add(&keyboard->wlr_keyboard->events.key, &keyboard->key);
    keyboard->modifiers.notify = server_keyboard_modifiers;
    wl_signal_add(&keyboard->wlr_keyboard->events.modifiers, &keyboard->modifiers);

    wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
}

static void server_new_input(struct wl_listener *listener, void *data) {
    struct recon_server *server = wl_container_of(listener, server, new_input);
    struct wlr_input_device *device = data;

    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        server_new_keyboard(server, device);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        wlr_cursor_attach_input_device(server->cursor, device);
        wlr_log(WLR_INFO, "ReconOS: pointer attached");
        break;
    default:
        break;
    }

    /* Advertise what we have so clients know which input to expect. */
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(server->seat, caps);
}

/* --- DESKTOP BACKGROUND --- */

/*
 * Build the desktop background at the size it will actually be displayed.
 *
 * This runs once the first output is up, because only then is the screen size
 * known. Nodes are added to the top of the scene, so the background is lowered
 * beneath the chrome that main() already created.
 */
/*
 * Decode an image out of the ReconOS filesystem.
 *
 * Wallpapers live at /System/Wallpapers rather than in the asset directory,
 * because they are files the user owns: replaceable, addable, and visible in
 * the File Explorer like anything else. The asset directory is only where the
 * ones that ship come *from*.
 */
static struct wlr_buffer *load_reconos_image(const char *reconos_path,
        int fit_w, int fit_h) {
    size_t size = 0;
    char *bytes = recon_fs_read("/", reconos_path, &size);
    if (bytes == NULL) {
        return NULL;
    }

    int width, height, channels;
    unsigned char *data = stbi_load_from_memory((const unsigned char *)bytes,
        (int)size, &width, &height, &channels, 4);
    free(bytes);

    if (data == NULL) {
        wlr_log(WLR_ERROR, "ReconOS: could not decode '%s'", reconos_path);
        return NULL;
    }

    bool from_stb = true;
    if (fit_w > 0 && fit_h > 0 && (width > fit_w || height > fit_h)) {
        unsigned char *scaled = downscale_rgba(data, width, height, fit_w, fit_h);
        if (scaled != NULL) {
            stbi_image_free(data);
            data = scaled;
            width = fit_w;
            height = fit_h;
            from_stb = false;
        }
    }

    struct wlr_buffer *buffer = image_buffer_create(data, width, height,
        from_stb);
    if (buffer == NULL) {
        if (from_stb) {
            stbi_image_free(data);
        } else {
            free(data);
        }
    }
    return buffer;
}

static void setup_background(struct recon_server *server, int width, int height);

/*
 * Put the wallpaper on, replacing whatever was there.
 *
 * Called at startup and again whenever the choice changes, which is what
 * makes changing it feel like a setting rather than something that needs a
 * restart. The old node goes first: two backgrounds is one too many, and the
 * one underneath would never be seen but would still be drawn.
 */
void recon_background_reload(struct recon_server *server) {
    if (server == NULL || !server->background_ready) {
        return;
    }

    int width = server->screen_width;
    int height = server->screen_height;
    if (width <= 0 || height <= 0) {
        return;
    }

    if (server->background_buffer != NULL) {
        wlr_scene_node_destroy(&server->background_buffer->node);
        server->background_buffer = NULL;
    }
    if (server->background_rect != NULL) {
        wlr_scene_node_destroy(&server->background_rect->node);
        server->background_rect = NULL;
    }

    setup_background(server, width, height);
    recon_damage_all(server);
}

static void setup_background(struct recon_server *server, int width, int height) {
    server->screen_width = width;
    server->screen_height = height;

    /*
     * Whichever wallpaper is chosen, from the filesystem. Falls back to the
     * bundled asset only when there is nothing there at all -- which is the
     * moment before the defaults have been written.
     */
    struct wlr_buffer *wallpaper = NULL;

    const char *chosen = recon_wallpaper_current();
    if (chosen != NULL && *chosen != '\0') {
        char path[RECON_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", RECON_DIR_WALLPAPERS, chosen);
        wallpaper = load_reconos_image(path, width, height);
    }
    if (wallpaper == NULL) {
        wallpaper = load_image("wallpaper.jpg", width, height);
    }

    struct wlr_scene_node *node;
    if (wallpaper != NULL) {
        server->background_buffer = wlr_scene_buffer_create(&server->scene->tree, wallpaper);
        /* The scene holds its own reference now. */
        wlr_buffer_drop(wallpaper);
        if (server->background_buffer == NULL) {
            return;
        }
        wlr_scene_buffer_set_dest_size(server->background_buffer, width, height);
        node = &server->background_buffer->node;
    } else {
        float color[4] = {0.1f, 0.1f, 0.3f, 1.0f};
        server->background_rect =
            wlr_scene_rect_create(&server->scene->tree, width, height, color);
        if (server->background_rect == NULL) {
            return;
        }
        node = &server->background_rect->node;
    }

    wlr_scene_node_set_position(node, 0, 0);
    wlr_scene_node_lower_to_bottom(node);
}

/* --- SURFACES --- */

/* Step the cascade, wrapping before windows march off the screen. */
#define CASCADE_STEP 32
#define CASCADE_LIMIT 320

static void place_window(struct recon_toplevel *toplevel) {
    struct recon_server *server = toplevel->server;

    int x = CASCADE_STEP + server->next_window_x;
    int y = CASCADE_STEP + server->next_window_y;
    wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y);

    server->next_window_x += CASCADE_STEP;
    server->next_window_y += CASCADE_STEP;
    if (server->next_window_x > CASCADE_LIMIT) {
        server->next_window_x = 0;
        server->next_window_y = 0;
    }
}

static void toplevel_map(struct wl_listener *listener, void *data) {
    struct recon_toplevel *toplevel = wl_container_of(listener, toplevel, map);

    wl_list_insert(&toplevel->server->toplevels, &toplevel->link);

    /*
     * On the desktop somebody is looking at, not on the first one.
     *
     * The field starts at zero, so without this a client launched from
     * desktop three would arrive on desktop one -- which is to say, it would
     * appear not to have started at all.
     */
    recon_toplevel_set_desktop(toplevel,
        recon_shell_desktop(toplevel->server->shell));
    recon_toplevel_set_desktop_showing(toplevel, true);

    place_window(toplevel);
    recon_focus_toplevel(toplevel);

    const char *title = toplevel->xdg_toplevel->title;
    wlr_log(WLR_INFO, "ReconOS: window mapped: %s", title != NULL ? title : "(untitled)");
}

static void toplevel_commit(struct wl_listener *listener, void *data) {
    struct recon_toplevel *toplevel = wl_container_of(listener, toplevel, commit);
    recon_damage_all(toplevel->server);
}

/* The taskbar shows window titles, so it follows title changes. */
static void toplevel_set_title(struct wl_listener *listener, void *data) {
    struct recon_toplevel *toplevel = wl_container_of(listener, toplevel, set_title);
    recon_shell_refresh(toplevel->server->shell);
}

static void toplevel_unmap(struct wl_listener *listener, void *data) {
    struct recon_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
    struct recon_server *server = toplevel->server;

    /* Don't leave a grab pointing at a window that is going away. */
    if (server->grabbed == toplevel) {
        server->grabbed = NULL;
        server->cursor_mode = RECON_CURSOR_PASSTHROUGH;
    }

    wl_list_remove(&toplevel->link);

    /* Hand focus to whatever was most recently used before this one. */
    if (!wl_list_empty(&server->toplevels)) {
        struct recon_toplevel *next =
            wl_container_of(server->toplevels.next, next, link);
        recon_focus_toplevel(next);
    } else {
        recon_shell_refresh(server->shell);
    }
    recon_damage_all(server);
}

static void toplevel_destroy(struct wl_listener *listener, void *data) {
    struct recon_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);

    /* Out of the application table before the memory goes, or the next
     * refresh would read a freed toplevel to get its title. */
    recon_apps_remove_client(toplevel);

    wl_list_remove(&toplevel->map.link);
    wl_list_remove(&toplevel->unmap.link);
    wl_list_remove(&toplevel->destroy.link);
    wl_list_remove(&toplevel->request_move.link);
    wl_list_remove(&toplevel->request_resize.link);
    wl_list_remove(&toplevel->request_maximize.link);
    wl_list_remove(&toplevel->request_fullscreen.link);
    wl_list_remove(&toplevel->request_minimize.link);
    wl_list_remove(&toplevel->set_title.link);
    wl_list_remove(&toplevel->commit.link);
    free(toplevel);
}

static void server_new_xdg_surface(struct wl_listener *listener, void *data) {
    struct recon_server *server = wl_container_of(listener, server, new_xdg_surface);
    struct wlr_xdg_surface *xdg_surface = data;

    /* Popups (menus, tooltips) position themselves relative to their parent, so
     * they just need to be placed in the parent's part of the scene graph. */
    if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
        struct wlr_xdg_surface *parent =
            wlr_xdg_surface_try_from_wlr_surface(xdg_surface->popup->parent);
        if (parent != NULL && parent->data != NULL) {
            struct wlr_scene_tree *parent_tree = parent->data;
            xdg_surface->data = wlr_scene_xdg_surface_create(parent_tree, xdg_surface);
        }
        return;
    }

    if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
        return;
    }

    struct recon_toplevel *toplevel = calloc(1, sizeof(*toplevel));
    if (toplevel == NULL) {
        return;
    }

    toplevel->server = server;
    toplevel->xdg_toplevel = xdg_surface->toplevel;
    toplevel->scene_tree = wlr_scene_xdg_surface_create(&server->scene->tree, xdg_surface);

    /* Tag the tree so a surface found under the pointer can be traced back to
     * its window; see toplevel_at(). */
    toplevel->scene_tree->node.data = toplevel;
    xdg_surface->data = toplevel->scene_tree;

    toplevel->map.notify = toplevel_map;
    wl_signal_add(&xdg_surface->surface->events.map, &toplevel->map);
    toplevel->unmap.notify = toplevel_unmap;
    wl_signal_add(&xdg_surface->surface->events.unmap, &toplevel->unmap);
    toplevel->destroy.notify = toplevel_destroy;
    wl_signal_add(&xdg_surface->events.destroy, &toplevel->destroy);

    toplevel->request_move.notify = toplevel_request_move;
    wl_signal_add(&xdg_surface->toplevel->events.request_move, &toplevel->request_move);
    toplevel->request_resize.notify = toplevel_request_resize;
    wl_signal_add(&xdg_surface->toplevel->events.request_resize, &toplevel->request_resize);
    toplevel->request_maximize.notify = toplevel_request_maximize;
    wl_signal_add(&xdg_surface->toplevel->events.request_maximize, &toplevel->request_maximize);
    toplevel->request_fullscreen.notify = toplevel_request_fullscreen;
    wl_signal_add(&xdg_surface->toplevel->events.request_fullscreen,
        &toplevel->request_fullscreen);
    toplevel->request_minimize.notify = toplevel_request_minimize;
    wl_signal_add(&xdg_surface->toplevel->events.request_minimize, &toplevel->request_minimize);
    toplevel->set_title.notify = toplevel_set_title;
    wl_signal_add(&xdg_surface->toplevel->events.set_title, &toplevel->set_title);
    toplevel->commit.notify = toplevel_commit;
    wl_signal_add(&xdg_surface->surface->events.commit, &toplevel->commit);

    wlr_log(WLR_INFO, "ReconOS: new toplevel window");
}

/* --- OUTPUT --- */

static void output_frame(struct wl_listener *listener, void *data) {
    struct recon_output *output = wl_container_of(listener, output, frame);
    struct wlr_scene_output *scene_output =
        wlr_scene_get_scene_output(output->server->scene, output->wlr_output);

    if (scene_output == NULL) {
        if (!output->warned_no_scene) {
            wlr_log(WLR_ERROR, "ReconOS: no scene output for '%s', nothing will draw",
                output->wlr_output->name);
            output->warned_no_scene = true;
        }
        return;
    }

    /*
     * Repaint the whole screen if anything changed since the last frame.
     *
     * Partial redraws assume the driver hands back a buffer still holding the
     * previous frame, so untouched regions can be left alone. Not all drivers
     * do -- hyperv_drm notably does not -- and the result is stale fragments
     * of whatever used to be there.
     *
     * The flag is what keeps this from becoming a permanent full-speed
     * redraw. Marking the screen damaged on every frame would make every
     * frame commit, and every commit schedules another frame. Marking it only
     * when something actually changed repaints correctly and then stops.
     *
     * Set RECONOS_PARTIAL_DAMAGE=1 on hardware known to preserve buffers.
     */
    if (!output->server->partial_damage && output->needs_full_redraw) {
        wlr_damage_ring_add_whole(&scene_output->damage_ring);
        output->needs_full_redraw = false;
    }

    /*
     * Capturing takes the long way round, and only when something asked.
     *
     * wlr_scene_output_commit renders and commits in one step, which leaves
     * no moment at which the finished frame exists and can be read -- the
     * pixels go to the display and the buffer comes back to be drawn over.
     * Building the state separately renders into a buffer we are holding,
     * which is the frame about to be shown, and *then* commits it.
     *
     * The ordinary path is untouched. This is the frame loop, it took four
     * wrong theories to get right once already, and a screenshot is not worth
     * risking it on every frame that nobody asked to photograph.
     */
    bool committed;
    if (recon_capture_pending()) {
        struct wlr_output_state state;
        wlr_output_state_init(&state);

        committed = wlr_scene_output_build_state(scene_output, &state, NULL);
        if (committed) {
            if ((state.committed & WLR_OUTPUT_STATE_BUFFER) != 0) {
                recon_capture_take(state.buffer);
            }
            committed = wlr_output_commit_state(output->wlr_output, &state);
        }
        wlr_output_state_finish(&state);
    } else {
        committed = wlr_scene_output_commit(scene_output, NULL);
    }

    if (!committed) {
        if (!output->warned_commit_failed) {
            wlr_log(WLR_ERROR, "ReconOS: scene commit failed on '%s'",
                output->wlr_output->name);
            output->warned_commit_failed = true;
        }
    } else if (!output->drew_once) {
        wlr_log(WLR_INFO, "ReconOS: first frame drawn on '%s'", output->wlr_output->name);
        output->drew_once = true;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_destroy(struct wl_listener *listener, void *data) {
    struct recon_output *output = wl_container_of(listener, output, destroy);
    wl_list_remove(&output->link);
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->destroy.link);
    free(output);
}

static void server_new_output(struct wl_listener *listener, void *data) {
    struct recon_server *server = wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = data;

    if (!wlr_output_init_render(wlr_output, server->allocator, server->renderer)) {
        wlr_log(WLR_ERROR, "ReconOS: failed to init renderer for output");
        return;
    }

    struct recon_output *output = calloc(1, sizeof(*output));
    if (output == NULL) {
        return;
    }
    output->wlr_output = wlr_output;
    output->server = server;
    output->needs_full_redraw = true;
    wl_list_insert(&server->outputs, &output->link);

    output->frame.notify = output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);
    output->destroy.notify = output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    /* Turn the output on at its preferred mode. */
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);

    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode != NULL) {
        wlr_output_state_set_mode(&state, mode);
    }

    if (!wlr_output_commit_state(wlr_output, &state)) {
        wlr_log(WLR_ERROR, "ReconOS: failed to enable output '%s'", wlr_output->name);
    }
    wlr_output_state_finish(&state);

    /* Give the scene graph a viewport onto this output, then tie that viewport
     * to the output's place in the layout. Attaching the layout to the scene
     * only keeps positions in sync -- it does not create the viewport, and
     * without one the scene has nowhere to draw. */
    struct wlr_scene_output *scene_output =
        wlr_scene_output_create(server->scene, wlr_output);
    struct wlr_output_layout_output *layout_output =
        wlr_output_layout_add_auto(server->output_layout, wlr_output);

    if (scene_output == NULL || layout_output == NULL) {
        wlr_log(WLR_ERROR, "ReconOS: could not attach output '%s' to the scene",
            wlr_output->name);
        return;
    }
    wlr_scene_output_layout_add_output(server->scene_layout, layout_output, scene_output);

    int width, height;
    wlr_output_effective_resolution(wlr_output, &width, &height);

    if (!server->background_ready) {
        /* First screen decides the wallpaper's size, and carries the shell. */
        setup_background(server, width, height);
        server->background_ready = true;

        /* The application table needs the shell to enumerate built-ins. */
        server->shell = recon_shell_create(server, width, height);
        recon_apps_init(server);

        /*
         * Networking, which means reading what the host already has. Before
         * the shell needs it and before any module can ask -- and after the
         * registry, since it wants the machine name from there.
         */
        recon_net_init(wl_display_get_event_loop(server->wl_display));
        wlr_log(WLR_INFO, "ReconOS: network %s, %d interface%s, gateway %s",
            recon_net_online() ? "up" : "down",
            recon_net_interface_count(),
            recon_net_interface_count() == 1 ? "" : "s",
            recon_net_gateway()[0] != '\0' ? recon_net_gateway() : "(none)");

        /* The reader's settings, once there is a font to apply them to. */
        recon_access_apply(recon_shell_font(server->shell));

        /*
         * Modules load after the shell, because most of them register
         * applications and there has to be somewhere to register them.
         */
        recon_modules_init(server, recon_shell_font(server->shell));

        /* What this build shipped with, if the filesystem does not have it
         * yet. A new system would otherwise come up with no applications at
         * all, since they live in modules now. */
        recon_modules_install_shipped();

        /*
         * Accounts before the gate, so it knows whether to ask who you are or
         * to set the system up.
         */
        recon_users_init();

        int loaded = recon_modules_load_all();
        if (loaded > 0) {
            wlr_log(WLR_INFO, "ReconOS: %d module%s loaded",
                loaded, loaded == 1 ? "" : "s");
        }

        /* Last: setup or the login screen, over everything else. */
        recon_shell_begin_session(server->shell);
        raise_chrome(server);
    } else if (server->background_buffer != NULL) {
        wlr_scene_buffer_set_dest_size(server->background_buffer, width, height);
    } else if (server->background_rect != NULL) {
        wlr_scene_rect_set_size(server->background_rect, width, height);
    }

    /* Kick off the render loop. Without this nothing requests a first frame,
     * so output_frame never fires and the screen stays blank. */
    wlr_output_schedule_frame(wlr_output);

    wlr_log(WLR_INFO, "ReconOS: output '%s' online at %dx%d",
        wlr_output->name, width, height);
}

/* --- MAIN --- */

int main(int argc, char **argv) {
    wlr_log_init(WLR_DEBUG, NULL);

    /* Reap spawned clients automatically so they don't linger as zombies. */
    signal(SIGCHLD, SIG_IGN);

    struct recon_server server = {0};

    const char *partial = getenv("RECONOS_PARTIAL_DAMAGE");
    server.partial_damage = (partial != NULL && *partial == '1');
    if (server.partial_damage) {
        wlr_log(WLR_INFO, "ReconOS: partial damage enabled, expecting the driver "
            "to preserve buffers between frames");
    }

    /* The filesystem comes up before anything that might want to read from
     * it, and creates its layout on first run. */
    if (!recon_fs_init(NULL)) {
        wlr_log(WLR_ERROR, "ReconOS: cannot open the filesystem: %s",
            recon_fs_last_error());
        return 1;
    }
    wlr_log(WLR_INFO, "ReconOS: filesystem rooted at %s", recon_fs_host_root());

    /* Settings come up next, since almost everything after this may want to
     * know what was chosen last time. */
    recon_registry_init();

    /* Skins come next: they read a setting to know which one is wanted, and
     * everything that draws needs colours before it draws anything. */
    recon_theme_write_defaults();
    recon_theme_init();

    /* Draw the default icons if they are not already there. Replaced ones are
     * left alone, so the generated set is a starting point rather than
     * something reimposed on every start. */
    int icons = recon_icons_write_defaults(false);
    if (icons > 0) {
        wlr_log(WLR_INFO, "ReconOS: wrote %d default icons", icons);
    }

    /*
     * The Recon Towers mark, copied into the icon set on first run.
     *
     * Copied rather than drawn, because unlike the rest of the icons this one
     * is artwork somebody made, and unlike the wallpaper it belongs to the
     * system rather than to a screen. Copied rather than read from the asset
     * directory every time, so it lives inside the ReconOS filesystem, is
     * asked for by name like every other icon, and can be replaced by dropping
     * a different file over it.
     */
    install_asset_icon("logo.png", RECON_ICON_LOGO);

    /*
     * Wallpapers, drawn the way the icons are. A system with nothing
     * installed should still look like something, and the photograph that
     * ships is one option among them rather than the only one.
     */
    int papers = recon_wallpapers_write_defaults();
    if (papers > 0) {
        wlr_log(WLR_INFO, "ReconOS: drew %d wallpapers", papers);
    }
    install_asset_wallpaper("wallpaper.jpg", "Earth.jpg");

    wl_list_init(&server.toplevels);
    wl_list_init(&server.outputs);
    server.cursor_mode = RECON_CURSOR_PASSTHROUGH;

    server.wl_display = wl_display_create();
    server.backend = wlr_backend_autocreate(server.wl_display, NULL);
    if (server.backend == NULL) {
        wlr_log(WLR_ERROR, "ReconOS: failed to create backend");
        return 1;
    }

    /*
     * With no GPU, the OpenGL path renders through a software implementation
     * and then copies the finished frame back out for the display to scan.
     * That copy corrupts frames on some drivers -- it is what caused black
     * rectangles to flicker across the screen here. Pixman rasterizes straight
     * into the display's buffer, so the copy does not happen at all, and on a
     * machine without a GPU it is the better choice regardless.
     *
     * An explicit WLR_RENDERER still wins; this only supplies a default.
     */
    if (getenv("WLR_RENDERER") == NULL && !has_render_node()) {
        wlr_log(WLR_INFO, "ReconOS: no GPU render node, using the pixman renderer");
        setenv("WLR_RENDERER", "pixman", 1);
    }

    server.renderer = wlr_renderer_autocreate(server.backend);
    wlr_renderer_init_wl_display(server.renderer, server.wl_display);
    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);

    /* Core globals every Wayland client expects to find:
     *   wl_compositor    - lets clients create surfaces
     *   wl_subcompositor - lets clients nest surfaces
     *   wl_data_device   - clipboard and drag-and-drop
     * Without these a client cannot draw anything and will abort on startup. */
    wlr_compositor_create(server.wl_display, 5, server.renderer);
    /*
     * Screen capture. A desktop should be able to photograph itself, and it
     * is the only way to see what was actually composited rather than what
     * was meant to be.
     */
    wlr_screencopy_manager_v1_create(server.wl_display);
    wlr_subcompositor_create(server.wl_display);
    wlr_data_device_manager_create(server.wl_display);

    server.output_layout = wlr_output_layout_create();
    /* Advertises screen geometry to clients. Screenshot tools need it to know
     * what area to ask for. */
    wlr_xdg_output_manager_v1_create(server.wl_display, server.output_layout);
    server.scene = wlr_scene_create();
    server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);
    if (server.scene_layout == NULL) {
        wlr_log(WLR_ERROR, "ReconOS: failed to attach output layout to scene");
        return 1;
    }

    /* The desktop background is created once the first output reports its size,
     * so the wallpaper can be scaled to fit exactly. See setup_background(). */

    /* xdg-shell is how clients create ordinary application windows. Without
     * this global they cannot open one at all and abort on startup. */
    server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 3);
    server.new_xdg_surface.notify = server_new_xdg_surface;
    wl_signal_add(&server.xdg_shell->events.new_surface, &server.new_xdg_surface);

    server.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

    /* Load a cursor theme and show an arrow. Drawn on the pointer's own layer,
     * so moving the mouse costs nothing and never blocks hit testing.
     * An unset theme means whatever the system provides. */
    const char *cursor_theme = getenv("RECONOS_CURSOR_THEME");
    if (cursor_theme == NULL || *cursor_theme == '\0') {
        cursor_theme = RECONOS_DEFAULT_CURSOR_THEME;
    }
    int cursor_size = RECONOS_DEFAULT_CURSOR_SIZE;
    const char *size_str = getenv("RECONOS_CURSOR_SIZE");
    if (size_str != NULL) {
        int parsed = atoi(size_str);
        if (parsed > 0) {
            cursor_size = parsed;
        }
    }
    wlr_log(WLR_INFO, "ReconOS: cursor theme '%s' at size %d",
        cursor_theme != NULL ? cursor_theme : "(system default)", cursor_size);

    server.cursor_mgr = wlr_xcursor_manager_create(cursor_theme, cursor_size);
    if (server.cursor_mgr == NULL || !wlr_xcursor_manager_load(server.cursor_mgr, 1)) {
        wlr_log(WLR_ERROR, "ReconOS: could not load a cursor theme");
    } else {
        wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, "default");
    }

    server.cursor_motion.notify = server_cursor_motion;
    wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
    server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
    wl_signal_add(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute);
    server.cursor_button.notify = server_cursor_button;
    wl_signal_add(&server.cursor->events.button, &server.cursor_button);
    server.cursor_axis.notify = server_cursor_axis;
    wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
    server.cursor_frame.notify = server_cursor_frame;
    wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

    server.seat = wlr_seat_create(server.wl_display, "seat0");
    server.request_set_cursor.notify = seat_request_set_cursor;
    wl_signal_add(&server.seat->events.request_set_cursor, &server.request_set_cursor);
    server.new_input.notify = server_new_input;
    wl_signal_add(&server.backend->events.new_input, &server.new_input);
    server.new_output.notify = server_new_output;
    wl_signal_add(&server.backend->events.new_output, &server.new_output);

    server.socket_name = wl_display_add_socket_auto(server.wl_display);
    if (server.socket_name == NULL) {
        wlr_log(WLR_ERROR, "ReconOS: failed to create Wayland socket");
        wl_display_destroy(server.wl_display);
        return 1;
    }

    if (!wlr_backend_start(server.backend)) {
        wlr_log(WLR_ERROR, "ReconOS: failed to start backend");
        wl_display_destroy(server.wl_display);
        return 1;
    }

    /* Remote access to the command interpreter. Optional: without it ReconOS
     * runs, just without being reachable from outside. */
    struct recon_control *control = recon_control_create(&server, NULL);

    wlr_log(WLR_INFO, "ReconOS running on WAYLAND_DISPLAY=%s", server.socket_name);
    printf(RECONOS_NAME " v" RECONOS_VERSION " - Alt+Enter for a terminal, "
        "Ctrl+Alt+Del for the task manager, Alt+Q to quit.\n");
    fflush(stdout);

    wl_display_run(server.wl_display);

    recon_control_destroy(control);
    recon_shell_destroy(server.shell);
    recon_users_finish();
    recon_net_finish();
    recon_theme_finish();
    recon_registry_finish();
    recon_fs_finish();
    wl_display_destroy_clients(server.wl_display);
    wl_display_destroy(server.wl_display);

    /*
     * The status is how a restart is asked for. Whatever started ReconOS
     * already knows how to start it; re-running ourselves in place would mean
     * handing the display, the input devices and a Wayland socket to a fresh
     * copy of this process, which is a great deal of machinery for something
     * the launcher does trivially.
     */
    return server.restarting ? RECON_EXIT_RESTART : 0;
}
