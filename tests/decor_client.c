/*
 * A Wayland client, for testing what ReconOS does with somebody else's window.
 *
 * ReconOS has hosted client windows since v0.1.0 and there has never been a
 * client written against it, so everything on that path was reasoned about and
 * nothing was watched. Server-side decorations made that untenable: the whole
 * feature is "the client stops drawing its title bar and we draw it instead",
 * and the only clients to hand -- weston's own demos -- do not use the
 * protocol at all and draw their frames regardless.
 *
 * So: the smallest client that does. It opens a window of a fixed size, fills
 * it with a flat colour so it is obvious on screen, asks for server-side
 * decorations, and does nothing else. It is not a test of ReconOS's drawing;
 * it is the thing ReconOS draws around.
 *
 *   decor_client [--client-side] [--width N] [--height N] [--title T]
 *
 * `--client-side` asks for the other mode, which is how the refusal path gets
 * exercised: a client that draws its own frame must not be given a second one.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-client-protocol.h"

struct client {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct zxdg_decoration_manager_v1 *decoration_manager;

    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct zxdg_toplevel_decoration_v1 *decoration;

    int width, height;
    const char *title;
    bool client_side;

    bool configured;
    bool closed;

    /* What the compositor said about who draws the frame, so the test can
     * assert on it rather than on a screenshot. */
    uint32_t decoration_mode;
};

/* --- The buffer --- */

static int make_shm_file(size_t size) {
    char name[] = "/reconos-decor-XXXXXX";
    /* mkstemp on a name in /dev/shm, by hand: shm_open wants a unique name and
     * there is no need for a directory entry to survive this function. */
    for (int attempt = 0; attempt < 100; attempt++) {
        for (int i = (int)strlen(name) - 6; i < (int)strlen(name); i++) {
            name[i] = 'a' + (char)(rand() % 26);
        }
        int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            shm_unlink(name);
            if (ftruncate(fd, (off_t)size) < 0) {
                close(fd);
                return -1;
            }
            return fd;
        }
        if (errno != EEXIST) {
            return -1;
        }
    }
    return -1;
}

static struct wl_buffer *make_buffer(struct client *c) {
    int stride = c->width * 4;
    size_t size = (size_t)stride * (size_t)c->height;

    int fd = make_shm_file(size);
    if (fd < 0) {
        return NULL;
    }

    uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
        fd, 0);
    if (pixels == MAP_FAILED) {
        close(fd);
        return NULL;
    }

    /*
     * A flat colour with a lighter band down the left, so which part of what
     * is on screen belongs to the client is not a matter of opinion.
     */
    for (int y = 0; y < c->height; y++) {
        for (int x = 0; x < c->width; x++) {
            pixels[y * c->width + x] = (x < 12) ? 0xFF6699CC : 0xFF203040;
        }
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(c->shm, fd, (int32_t)size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, c->width,
        c->height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);

    munmap(pixels, size);
    close(fd);
    return buffer;
}

/* --- Protocol --- */

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base,
        uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener WM_BASE_LISTENER = {
    .ping = wm_base_ping,
};

static void registry_global(void *data, struct wl_registry *registry,
        uint32_t name, const char *interface, uint32_t version) {
    struct client *c = data;
    (void)version;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        c->compositor = wl_registry_bind(registry, name,
            &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        c->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        c->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(c->wm_base, &WM_BASE_LISTENER, c);
    } else if (strcmp(interface,
            zxdg_decoration_manager_v1_interface.name) == 0) {
        c->decoration_manager = wl_registry_bind(registry, name,
            &zxdg_decoration_manager_v1_interface, 1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
        uint32_t name) {
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener REGISTRY_LISTENER = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void xdg_surface_configure(void *data, struct xdg_surface *surface,
        uint32_t serial) {
    struct client *c = data;
    xdg_surface_ack_configure(surface, serial);

    struct wl_buffer *buffer = make_buffer(c);
    if (buffer != NULL) {
        wl_surface_attach(c->surface, buffer, 0, 0);
        wl_surface_damage_buffer(c->surface, 0, 0, c->width, c->height);
        wl_surface_commit(c->surface);
    }
    c->configured = true;
}

static const struct xdg_surface_listener XDG_SURFACE_LISTENER = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
        int32_t width, int32_t height, struct wl_array *states) {
    struct client *c = data;
    (void)toplevel; (void)states;

    /* Zero means "you decide", which is what an unmaximized window gets. */
    if (width > 0 && height > 0) {
        c->width = width;
        c->height = height;
    }
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    struct client *c = data;
    (void)toplevel;
    c->closed = true;
}

static const struct xdg_toplevel_listener TOPLEVEL_LISTENER = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

static void decoration_configure(void *data,
        struct zxdg_toplevel_decoration_v1 *decoration, uint32_t mode) {
    struct client *c = data;
    (void)decoration;

    c->decoration_mode = mode;
    fprintf(stdout, "decoration: %s\n",
        mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
            ? "server-side" : "client-side");
    fflush(stdout);
}

static const struct zxdg_toplevel_decoration_v1_listener DECORATION_LISTENER = {
    .configure = decoration_configure,
};

/* --- Running --- */

int main(int argc, char **argv) {
    struct client c = {
        .width = 480,
        .height = 320,
        .title = "Decorated by ReconOS",
    };

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--client-side") == 0) {
            c.client_side = true;
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            c.width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            c.height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
            c.title = argv[++i];
        }
    }

    c.display = wl_display_connect(NULL);
    if (c.display == NULL) {
        fprintf(stderr, "decor_client: no Wayland display\n");
        return 1;
    }

    c.registry = wl_display_get_registry(c.display);
    wl_registry_add_listener(c.registry, &REGISTRY_LISTENER, &c);
    wl_display_roundtrip(c.display);

    if (c.compositor == NULL || c.shm == NULL || c.wm_base == NULL) {
        fprintf(stderr, "decor_client: the compositor is missing something "
            "this needs\n");
        return 1;
    }

    c.surface = wl_compositor_create_surface(c.compositor);
    c.xdg_surface = xdg_wm_base_get_xdg_surface(c.wm_base, c.surface);
    xdg_surface_add_listener(c.xdg_surface, &XDG_SURFACE_LISTENER, &c);

    c.toplevel = xdg_surface_get_toplevel(c.xdg_surface);
    xdg_toplevel_add_listener(c.toplevel, &TOPLEVEL_LISTENER, &c);
    xdg_toplevel_set_title(c.toplevel, c.title);
    xdg_toplevel_set_app_id(c.toplevel, "reconos.decor_client");

    /*
     * The whole point. Asked for before the first commit, so the compositor's
     * answer arrives with the first configure and the window is never drawn
     * under the wrong assumption.
     */
    if (c.decoration_manager != NULL) {
        c.decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
            c.decoration_manager, c.toplevel);
        zxdg_toplevel_decoration_v1_add_listener(c.decoration,
            &DECORATION_LISTENER, &c);
        zxdg_toplevel_decoration_v1_set_mode(c.decoration, c.client_side
            ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
            : ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    } else {
        fprintf(stdout, "decoration: not offered\n");
        fflush(stdout);
    }

    wl_surface_commit(c.surface);

    while (!c.closed && wl_display_dispatch(c.display) != -1) {
        /* Until the compositor closes it, or the connection ends. */
    }

    wl_display_disconnect(c.display);
    return 0;
}
