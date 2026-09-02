/*
 * ReconOS - a Wayland compositor built from scratch on wlroots.
 *
 * Phase 1 of the project: the desktop shell layer. See docs/ROADMAP.md.
 *
 * Controls:
 *   Alt + Q      quit the compositor
 *   Alt + Enter  launch a terminal client
 *   click (50,50)-(114,114)  power button -> quit
 */

#define _POSIX_C_SOURCE 200112L

/* stb_image is a single-header library; this TU provides the implementation. */
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

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
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

/* Where image assets live. CMake defines this; the env var overrides it so the
 * binary stays runnable when moved off the build machine. */
#ifndef RECONOS_ASSET_DIR
#define RECONOS_ASSET_DIR "assets"
#endif

/* Power button geometry. */
#define BUTTON_X 50
#define BUTTON_Y 50
#define BUTTON_SIZE 64

/* --- STRUCTS --- */

struct recon_server {
    struct wl_display *wl_display;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;

    struct wlr_scene *scene;
    /* Keeps scene outputs positioned in step with the output layout. */
    struct wlr_scene_output_layout *scene_layout;

    /* Desktop chrome. Exactly one of background_buffer/background_rect is set,
     * depending on whether the wallpaper image loaded. */
    struct wlr_scene_buffer *background_buffer;
    struct wlr_scene_rect *background_rect;
    struct wlr_scene_node *button_node;
    struct wlr_scene_node *cursor_node;

    struct wlr_xdg_shell *xdg_shell;
    struct wl_listener new_xdg_surface;

    struct wlr_seat *seat;
    struct wlr_output_layout *output_layout;
    struct wlr_cursor *cursor;

    const char *socket_name;

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
};

struct recon_output {
    struct wlr_output *wlr_output;
    struct recon_server *server;
    struct wl_listener frame;
    struct wl_listener destroy;

    /* Latches so a per-frame problem is reported once, not 60 times a second. */
    bool drew_once;
    bool warned_no_scene;
    bool warned_commit_failed;
};

struct recon_keyboard {
    struct wlr_keyboard *wlr_keyboard;
    struct recon_server *server;
    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
};

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
 * Decode an image asset into a wlr_buffer, or NULL if it can't be loaded.
 *
 * Pass a positive fit_w/fit_h to shrink an oversized image to those dimensions
 * at load time; pass 0 to keep it at its natural size.
 */
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

/* --- INPUT: POINTER --- */

static void server_cursor_button(struct wl_listener *listener, void *data) {
    struct recon_server *server = wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;

    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED && event->button == BTN_LEFT) {
        double x = server->cursor->x;
        double y = server->cursor->y;

        if (x >= BUTTON_X && x <= BUTTON_X + BUTTON_SIZE &&
                y >= BUTTON_Y && y <= BUTTON_Y + BUTTON_SIZE) {
            wlr_log(WLR_INFO, "ReconOS: power button clicked, shutting down");
            wl_display_terminate(server->wl_display);
            return;
        }
    }

    /* Anything not consumed by the shell goes to the focused client. */
    wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button,
        event->state);
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
    struct recon_server *server = wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;

    wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    wlr_scene_node_set_position(server->cursor_node, server->cursor->x, server->cursor->y);
}

static void server_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    struct recon_server *server = wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;

    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
    wlr_scene_node_set_position(server->cursor_node, server->cursor->x, server->cursor->y);
}

/* --- INPUT: KEYBOARD --- */

static void spawn_terminal(struct recon_server *server) {
    const char *term = getenv("RECONOS_TERMINAL");
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

static void server_keyboard_key(struct wl_listener *listener, void *data) {
    struct recon_keyboard *keyboard = wl_container_of(listener, keyboard, key);
    struct wlr_keyboard_key_event *event = data;

    /* libinput keycodes are offset by 8 from xkb's. */
    uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state, keycode, &syms);

    if (event->state != WL_KEYBOARD_KEY_STATE_PRESSED) {
        return;
    }

    uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
    for (int i = 0; i < nsyms; i++) {
        if ((modifiers & WLR_MODIFIER_ALT) && syms[i] == XKB_KEY_q) {
            wlr_log(WLR_INFO, "ReconOS: Alt+Q, shutting down");
            wl_display_terminate(keyboard->server->wl_display);
            return;
        }
        if ((modifiers & WLR_MODIFIER_ALT) && syms[i] == XKB_KEY_Return) {
            spawn_terminal(keyboard->server);
            return;
        }
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
static void setup_background(struct recon_server *server, int width, int height) {
    struct wlr_buffer *wallpaper = load_image("wallpaper.jpg", width, height);

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

static void server_new_xdg_surface(struct wl_listener *listener, void *data) {
    struct recon_server *server = wl_container_of(listener, server, new_xdg_surface);
    struct wlr_xdg_surface *xdg_surface = data;

    if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
        wlr_scene_xdg_surface_create(&server->scene->tree, xdg_surface);
        wlr_log(WLR_INFO, "ReconOS: new toplevel window");
    }
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
     * Widen whatever changed to cover the whole screen.
     *
     * Partial redraws assume the driver hands back a buffer still holding the
     * previous frame, so untouched regions can be left alone. Not all drivers
     * do -- hyperv_drm notably does not -- and the result is a screen of
     * correct strips separated by stale ones.
     *
     * Only existing damage is widened, never created. Marking the screen
     * damaged unconditionally would make every frame commit, and each commit
     * schedules the next frame, so the compositor would redraw forever at full
     * speed. Widening only real damage keeps an idle desktop at zero frames.
     *
     * Set RECONOS_PARTIAL_DAMAGE=1 on hardware known to preserve buffers.
     */
    if (!output->server->partial_damage &&
            pixman_region32_not_empty(&scene_output->damage_ring.current)) {
        wlr_damage_ring_add_whole(&scene_output->damage_ring);
    }

    if (!wlr_scene_output_commit(scene_output, NULL)) {
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
        /* First screen decides the wallpaper's size. */
        setup_background(server, width, height);
        server->background_ready = true;
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

    server.wl_display = wl_display_create();
    server.backend = wlr_backend_autocreate(server.wl_display, NULL);
    if (server.backend == NULL) {
        wlr_log(WLR_ERROR, "ReconOS: failed to create backend");
        return 1;
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
    wlr_subcompositor_create(server.wl_display);
    wlr_data_device_manager_create(server.wl_display);

    server.output_layout = wlr_output_layout_create();
    server.scene = wlr_scene_create();
    server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);
    if (server.scene_layout == NULL) {
        wlr_log(WLR_ERROR, "ReconOS: failed to attach output layout to scene");
        return 1;
    }

    /* The desktop background is created once the first output reports its size,
     * so the wallpaper can be scaled to fit exactly. See setup_background(). */

    /* Power button: icon if present, otherwise a plain red square so the
     * clickable region is always visible. */
    struct wlr_buffer *icon = load_image("power.png", BUTTON_SIZE, BUTTON_SIZE);
    if (icon != NULL) {
        struct wlr_scene_buffer *btn = wlr_scene_buffer_create(&server.scene->tree, icon);
        wlr_buffer_drop(icon);
        wlr_scene_buffer_set_dest_size(btn, BUTTON_SIZE, BUTTON_SIZE);
        server.button_node = &btn->node;
    } else {
        float color[4] = {0.8f, 0.1f, 0.1f, 1.0f};
        server.button_node =
            &wlr_scene_rect_create(&server.scene->tree, BUTTON_SIZE, BUTTON_SIZE, color)->node;
    }
    wlr_scene_node_set_position(server.button_node, BUTTON_X, BUTTON_Y);

    /* Cursor: a small square for now, until a real cursor theme is wired up. */
    float cursor_color[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    server.cursor_node = &wlr_scene_rect_create(&server.scene->tree, 10, 10, cursor_color)->node;

    server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 3);
    server.new_xdg_surface.notify = server_new_xdg_surface;
    wl_signal_add(&server.xdg_shell->events.new_surface, &server.new_xdg_surface);

    server.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

    server.cursor_motion.notify = server_cursor_motion;
    wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
    server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
    wl_signal_add(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute);
    server.cursor_button.notify = server_cursor_button;
    wl_signal_add(&server.cursor->events.button, &server.cursor_button);

    server.seat = wlr_seat_create(server.wl_display, "seat0");
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

    wlr_log(WLR_INFO, "ReconOS running on WAYLAND_DISPLAY=%s", server.socket_name);
    printf("ReconOS v0.7 - Alt+Enter for a terminal, Alt+Q to quit.\n");
    fflush(stdout);

    wl_display_run(server.wl_display);

    wl_display_destroy_clients(server.wl_display);
    wl_display_destroy(server.wl_display);
    return 0;
}
