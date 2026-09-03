/*
 * Screen capture. See include/recon_capture.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <drm_fourcc.h>
#include <wlr/types/wlr_buffer.h>

#include "recon_capture.h"
#include "recon_fs.h"
#include "recon_png.h"
#include "recon_server.h"

static struct recon_server *g_server;
static bool g_pending;
static char g_wanted[RECON_PATH_MAX];

static bool g_have_last;
static char g_last_path[RECON_PATH_MAX];
static bool g_last_ok;

static char g_error[256];

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, args);
    va_end(args);
}

const char *recon_capture_last_error(void) {
    return g_error[0] != '\0' ? g_error : "no error";
}

bool recon_capture_pending(void) {
    return g_pending;
}

bool recon_capture_last(char *path, size_t size, bool *ok) {
    if (!g_have_last) {
        return false;
    }
    if (path != NULL && size > 0) {
        snprintf(path, size, "%s", g_last_path);
    }
    if (ok != NULL) {
        *ok = g_last_ok;
    }
    return true;
}

/*
 * A name nothing else has.
 *
 * Dated and numbered rather than only dated: two captures in the same second
 * are ordinary, and a second one that silently replaced the first would be a
 * screenshot tool that loses screenshots.
 */
static void choose_path(char *out, size_t size) {
    time_t now = time(NULL);
    struct tm parts;
    localtime_r(&now, &parts);

    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H%M%S", &parts);

    const char *pictures = recon_fs_user_dir("Pictures");

    for (int attempt = 0; attempt < 100; attempt++) {
        if (attempt == 0) {
            snprintf(out, size, "%s/Screen %s.png", pictures, stamp);
        } else {
            snprintf(out, size, "%s/Screen %s (%d).png", pictures, stamp,
                attempt + 1);
        }
        if (!recon_fs_exists("/", out)) {
            return;
        }
    }
}

bool recon_capture_request(struct recon_server *server, const char *path) {
    if (g_pending) {
        set_error("a capture is already waiting for a frame");
        return false;
    }

    g_server = server;

    if (path != NULL && *path != '\0') {
        snprintf(g_wanted, sizeof(g_wanted), "%s", path);
    } else {
        choose_path(g_wanted, sizeof(g_wanted));
    }

    g_pending = true;

    /*
     * A frame has to happen for there to be anything to read, and an idle
     * desktop is not drawing any. Asking for one is the whole mechanism.
     */
    recon_damage_all(server);
    return true;
}

void recon_capture_take(struct wlr_buffer *buffer) {
    if (!g_pending || buffer == NULL) {
        return;
    }
    g_pending = false;
    g_have_last = true;
    g_last_ok = false;
    snprintf(g_last_path, sizeof(g_last_path), "%s", g_wanted);

    /*
     * The frame's pixels, straight out of the buffer it was drawn into.
     *
     * This only works when the buffer is one the CPU can read, which is the
     * case with the pixman renderer -- the one ReconOS chooses on a machine
     * with no GPU, which is every machine it has run on so far. On a buffer
     * that lives on a GPU this fails cleanly and says so rather than saving
     * a picture of nothing.
     */
    void *data = NULL;
    uint32_t format = 0;
    size_t stride = 0;

    if (!wlr_buffer_begin_data_ptr_access(buffer,
            WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
        set_error("this frame is not in memory the system can read");
        return;
    }

    int width = buffer->width;
    int height = buffer->height;

    /*
     * Both formats are four bytes with the same layout; they differ only in
     * whether the fourth byte means anything. A screenshot has nothing behind
     * it, so it is written without alpha either way.
     */
    if (format != DRM_FORMAT_XRGB8888 && format != DRM_FORMAT_ARGB8888) {
        wlr_buffer_end_data_ptr_access(buffer);
        set_error("unexpected pixel format 0x%08x", format);
        return;
    }

    unsigned int *pixels = malloc((size_t)width * height * sizeof(*pixels));
    if (pixels == NULL) {
        wlr_buffer_end_data_ptr_access(buffer);
        set_error("out of memory");
        return;
    }

    /* Copied row by row rather than in one block: a buffer's stride is
     * whatever the allocator felt like and is often wider than the image. */
    for (int y = 0; y < height; y++) {
        const unsigned char *row = (const unsigned char *)data + y * stride;
        memcpy(pixels + (size_t)y * width, row,
            (size_t)width * sizeof(*pixels));
    }

    wlr_buffer_end_data_ptr_access(buffer);

    /*
     * The account's own folder has to exist. It does for anybody signed in,
     * and does not before anybody has signed in -- which is a real moment,
     * because the setup screen is worth capturing too.
     */
    g_last_ok = recon_png_write(g_last_path, pixels, width, height, false);
    if (!g_last_ok) {
        set_error("%s", recon_png_last_error());
    }

    free(pixels);
}
