/*
 * How a panel reaches a screen, on Linux.
 *
 * This is the last inch of `src/recon_ui.c`, and the only part of it that
 * cannot leave here. Everything above -- three thousand lines of widgets,
 * text, themed fills and rounded corners -- writes into a `uint32_t *` and
 * knows nothing about what shows it. This file takes that array, hands it to
 * a wlroots scene graph, and moves, raises and hides the node that holds it.
 *
 * **It is a file to delete, not an idea to rethink.** The board's `Borrowed`
 * section says that of every wlroots dependency and it is most literally true
 * of this one: on ReconOS the same pixels go to the framebuffer device, and
 * what changes is this file and nothing else.
 *
 * The code here was moved out of recon_ui.c unchanged in v0.4.44. The
 * comments explaining *why* each piece is the way it is came with it, because
 * they are about wlroots and this is now where wlroots lives.
 */

#define _POSIX_C_SOURCE 200112L

#include <drm_fourcc.h>
#include <stdio.h>
#include <stdlib.h>

#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "recon_error.h"
#include "recon_ui.h"
#include "recon_ui_internal.h"

/*
 * A committed buffer owns its pixels outright rather than pointing back at the
 * panel's.
 *
 * Sharing them looks tempting and is wrong twice over. The compositor may
 * still be reading a previously committed buffer while the next frame is being
 * drawn, so a shared block gets overwritten mid-read; and resizing the panel
 * frees that block while those buffers still reference it. Both show up as
 * torn or black rectangles, the second far more violently, because it is a use
 * after free.
 *
 * The cost is a copy per commit, which is nothing next to how rarely a panel
 * commits: only when its contents actually change.
 */
struct panel_buffer {
    struct wlr_buffer base;
    uint32_t *pixels; /* owned by this buffer */
    size_t stride;
};

static void panel_buffer_destroy(struct wlr_buffer *wlr_buffer) {
    struct panel_buffer *buf = wl_container_of(wlr_buffer, buf, base);
    free(buf->pixels);
    free(buf);
}

static bool panel_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
        uint32_t flags, void **data, uint32_t *format, size_t *stride) {
    struct panel_buffer *buf = wl_container_of(wlr_buffer, buf, base);
    *data = buf->pixels;
    *format = DRM_FORMAT_ARGB8888;
    *stride = buf->stride;
    return true;
}

static void panel_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
    /* The buffer owns these pixels; nothing else writes to them. */
}

static const struct wlr_buffer_impl panel_buffer_impl = {
    .destroy = panel_buffer_destroy,
    .begin_data_ptr_access = panel_buffer_begin_data_ptr_access,
    .end_data_ptr_access = panel_buffer_end_data_ptr_access,
};

/*
 * Write a committed panel to a file when RECONOS_DEBUG_DUMP names a directory.
 *
 * This exists to answer one question that guesswork could not: whether pixels
 * leaving a panel are already wrong, or only become wrong further down. PPM
 * because it needs no encoder.
 *
 * It lives on this side of the seam because it reads an environment variable
 * and writes a host file, neither of which a ReconOS program has -- and
 * because the question it answers is about what leaves for the compositor.
 */
static void dump_panel(struct recon_panel *panel, const uint32_t *pixels) {
    const char *dir = getenv("RECONOS_DEBUG_DUMP");
    if (dir == NULL || *dir == '\0') {
        return;
    }

    static unsigned counter;
    char path[512];
    snprintf(path, sizeof(path), "%s/panel-%04u-%dx%d.ppm",
        dir, counter++, panel->width, panel->height);

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", panel->width,
        panel->height);
    for (size_t i = 0; i < (size_t)panel->width * panel->height; i++) {
        unsigned char rgb[3] = {
            (unsigned char)((pixels[i] >> 16) & 0xFF),
            (unsigned char)((pixels[i] >> 8) & 0xFF),
            (unsigned char)(pixels[i] & 0xFF),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/* --- The presentation --- */

static void wlr_commit(struct recon_panel *panel, void *state) {
    struct wlr_scene_buffer *scene_buffer = state;

    if (scene_buffer == NULL) {
        return;
    }

    int width = panel->width;
    int height = panel->height;
    const uint32_t *src = panel->pixels;
    if (src == NULL || width <= 0 || height <= 0) {
        return;
    }

    struct panel_buffer *buf = calloc(1, sizeof(*buf));
    if (buf == NULL) {
        return;
    }

    size_t count = (size_t)width * height;
    buf->pixels = malloc(count * sizeof(uint32_t));
    if (buf->pixels == NULL) {
        free(buf);
        return;
    }

    /*
     * Straight alpha becomes premultiplied here, and nowhere else.
     *
     * Everything that draws into a panel works in straight alpha, which is
     * what the arithmetic in recon_color_fade and recon_round_top_corners
     * assumes and what makes them composable. wlroots renders this buffer
     * with WLR_RENDER_BLEND_MODE_PREMULTIPLIED, which is a different
     * agreement: it takes the colour as already scaled by its own alpha and
     * adds it to what is behind.
     *
     * Handed straight alpha it therefore *adds the full colour* of every
     * transparent pixel. A rounded corner -- alpha 0, RGB still holding the
     * frame's edge colour -- came out as the edge colour at full strength,
     * which is why every window looked square with a notch cut in it. Glass
     * was wrong the same way and looked merely washed out rather than broken.
     *
     * Converting at the boundary rather than in the drawing code keeps one
     * model inside the program and one at the edge, which is the only
     * arrangement where "what alpha means" has a single answer in each place.
     * That sentence is also the reason this conversion moved to this file
     * rather than staying with the drawing: *premultiplied* is what wlroots
     * wants, not what a screen wants.
     */
    for (size_t i = 0; i < count; i++) {
        uint32_t px = src[i];
        uint32_t a = (px >> 24) & 0xFFu;

        if (a == 0xFFu) {
            buf->pixels[i] = px;            /* the ordinary case */
            continue;
        }
        if (a == 0u) {
            buf->pixels[i] = 0u;            /* nothing there at all */
            continue;
        }

        uint32_t r = (((px >> 16) & 0xFFu) * a + 127u) / 255u;
        uint32_t g = (((px >> 8) & 0xFFu) * a + 127u) / 255u;
        uint32_t b = ((px & 0xFFu) * a + 127u) / 255u;
        buf->pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
    buf->stride = (size_t)width * 4;

    dump_panel(panel, buf->pixels);

    wlr_buffer_init(&buf->base, &panel_buffer_impl, width, height);

    /* The scene takes its own reference; drop ours so the buffer is released
     * when the scene is done with it. */
    wlr_scene_buffer_set_buffer(scene_buffer, &buf->base);
    wlr_buffer_drop(&buf->base);

    wlr_scene_buffer_set_dest_size(scene_buffer, width, height);
}

static void wlr_set_position(void *state, int x, int y) {
    struct wlr_scene_buffer *scene_buffer = state;
    if (scene_buffer != NULL) {
        wlr_scene_node_set_position(&scene_buffer->node, x, y);
    }
}

static void wlr_position(void *state, int *x, int *y) {
    struct wlr_scene_buffer *scene_buffer = state;
    if (scene_buffer == NULL) {
        return;
    }
    /* Read back from the scene node rather than keeping a copy on the panel.
     * The node is what actually decides where this is drawn, so a second
     * record of it could only ever be right or wrong, never authoritative. */
    if (x != NULL) {
        *x = scene_buffer->node.x;
    }
    if (y != NULL) {
        *y = scene_buffer->node.y;
    }
}

static void wlr_raise_to_top(void *state) {
    struct wlr_scene_buffer *scene_buffer = state;
    if (scene_buffer != NULL) {
        wlr_scene_node_raise_to_top(&scene_buffer->node);
    }
}

static void wlr_lower_to_bottom(void *state) {
    struct wlr_scene_buffer *scene_buffer = state;
    if (scene_buffer != NULL) {
        wlr_scene_node_lower_to_bottom(&scene_buffer->node);
    }
}

static void wlr_set_enabled(void *state, bool enabled) {
    struct wlr_scene_buffer *scene_buffer = state;
    if (scene_buffer != NULL) {
        wlr_scene_node_set_enabled(&scene_buffer->node, enabled);
    }
}

static void wlr_destroy(void *state) {
    struct wlr_scene_buffer *scene_buffer = state;
    if (scene_buffer != NULL) {
        wlr_scene_node_destroy(&scene_buffer->node);
    }
}

static const struct recon_panel_present WLR_PRESENT = {
    .commit = wlr_commit,
    .set_position = wlr_set_position,
    .position = wlr_position,
    .raise_to_top = wlr_raise_to_top,
    .lower_to_bottom = wlr_lower_to_bottom,
    .set_enabled = wlr_set_enabled,
    .destroy = wlr_destroy,
};

/* --- Making one --- */

struct recon_panel *recon_panel_create(struct wlr_scene_tree *parent,
        int width, int height) {
    if (width <= 0 || height <= 0) {
        return NULL;
    }

    /* Created with no buffer; commit installs one. */
    struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_create(parent,
        NULL);
    if (scene_buffer == NULL) {
        recon_error_raise(NULL, RECON_ERR_D002,
            "the scene refused a buffer for it");
        return NULL;
    }

    struct recon_panel *panel = recon_panel_wrap(width, height, &WLR_PRESENT,
        scene_buffer);
    if (panel == NULL) {
        /* recon_panel_wrap has already raised D-002 for the pixels. */
        wlr_scene_node_destroy(&scene_buffer->node);
        return NULL;
    }

    return panel;
}

struct wlr_scene_node *recon_panel_node(struct recon_panel *panel) {
    if (panel == NULL) {
        return NULL;
    }
    struct wlr_scene_buffer *scene_buffer = panel->present_state;
    if (scene_buffer == NULL) {
        return NULL;
    }
    return &scene_buffer->node;
}

/* --- Saying something --- */

/*
 * What recon_ui used to do with `wlr_log` directly, from inside the font code.
 *
 * Installed by whoever starts the compositor, so the messages still reach the
 * same place -- and the drawing half no longer needs a wayland header to
 * complain that a font is missing.
 */
static void wlr_log_hook(bool error, const char *message) {
    wlr_log(error ? WLR_ERROR : WLR_INFO, "%s", message);
}

void recon_ui_log_to_wlroots(void) {
    recon_ui_set_log(wlr_log_hook);
}

/*
 * --- Which window is genuinely on top ---
 *
 * The scene graph is the only thing that knows, and the reason it is worth
 * asking rather than checking rectangles is that **a maximized window contains
 * every point on the screen**. Testing containment would have it claim clicks
 * meant for windows stacked above it.
 *
 * And it knows something the caller cannot: the scene holds windows the shell
 * never passed in -- a client's toplevel. So this can answer **-1 for a point
 * one of the given panels does contain**, which is not a failure to find one.
 * It is the correct answer, and dropping it would send a click meant for a
 * client to a built-in window underneath it.
 *
 * That is the whole reason this is a hook rather than the default. Where there
 * is no compositor there are no clients, and the caller's own order is the
 * answer -- see `recon_panel_topmost_of`.
 */
static struct wlr_scene *g_scene;

static int wlr_hit_test(struct recon_panel *const *panels, int count,
        double lx, double ly) {
    if (g_scene == NULL) {
        return -1;
    }

    double sx, sy;
    struct wlr_scene_node *on_top =
        wlr_scene_node_at(&g_scene->tree.node, lx, ly, &sx, &sy);

    if (on_top == NULL) {
        return -1;
    }

    for (int i = 0; i < count; i++) {
        if (panels[i] != NULL && recon_panel_node(panels[i]) == on_top) {
            return i;
        }
    }

    /*
     * Something is on top here and it is not one of these. A client window,
     * the cursor, a layer this shell does not own -- the caller gets -1 and
     * should let it have the click.
     */
    return -1;
}

void recon_ui_hit_test_to_wlroots(struct wlr_scene *scene) {
    g_scene = scene;
    recon_ui_set_hit_test(scene != NULL ? wlr_hit_test : NULL);
}
