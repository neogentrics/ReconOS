/*
 * The ReconOS icon set. See include/recon_icons.h.
 *
 * Icons are files in /System/Icons, loaded on first use and kept. They are
 * looked up by name, so putting a differently drawn file there changes what
 * the system shows without a line of code changing -- which is what makes the
 * icons replaceable rather than compiled in.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "recon_fs.h"
#include "recon_ico.h"
#include "recon_icons.h"
#include "stb_image.h"

#define CACHE_MAX 32
#define PREFERRED_SIZE 32

struct cached_icon {
    char name[64];
    unsigned char *pixels; /* RGBA, or NULL if there is no such icon */
    int width, height;
    bool looked_up;
};

static struct cached_icon g_cache[CACHE_MAX];
static int g_cache_count;

/* Try one file, returning pixels or NULL. */
static unsigned char *try_load(const char *path, int *width, int *height) {
    size_t size = 0;
    char *data = recon_fs_read("/", path, &size);
    if (data == NULL || size == 0) {
        free(data);
        return NULL;
    }

    unsigned char *pixels = NULL;
    size_t length = strlen(path);

    if (length > 4 && strcasecmp(path + length - 4, ".ico") == 0) {
        pixels = recon_ico_decode((const unsigned char *)data, size,
            PREFERRED_SIZE, width, height);
    } else {
        int channels;
        pixels = stbi_load_from_memory((const unsigned char *)data, (int)size,
            width, height, &channels, 4);
    }

    free(data);
    return pixels;
}

const unsigned char *recon_icon_get(const char *name, int *width, int *height) {
    if (name == NULL || *name == '\0') {
        return NULL;
    }

    for (int i = 0; i < g_cache_count; i++) {
        if (strcasecmp(g_cache[i].name, name) == 0) {
            if (g_cache[i].pixels == NULL) {
                return NULL;
            }
            *width = g_cache[i].width;
            *height = g_cache[i].height;
            return g_cache[i].pixels;
        }
    }

    if (g_cache_count >= CACHE_MAX) {
        return NULL;
    }

    struct cached_icon *entry = &g_cache[g_cache_count++];
    snprintf(entry->name, sizeof(entry->name), "%s", name);
    entry->pixels = NULL;
    entry->looked_up = true;

    /*
     * .ico first, since that is the format icons are usually supplied in, then
     * .png for anything produced by a tool that does not write icons.
     */
    static const char *const EXTENSIONS[] = { "ico", "png", NULL };
    for (int i = 0; EXTENSIONS[i] != NULL && entry->pixels == NULL; i++) {
        char path[RECON_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.%s",
            RECON_DIR_SYSTEM_ICONS, name, EXTENSIONS[i]);
        entry->pixels = try_load(path, &entry->width, &entry->height);
    }

    /* A missing icon is remembered as missing, so a name that has no file is
     * not searched for again on every redraw. */
    if (entry->pixels == NULL) {
        return NULL;
    }

    *width = entry->width;
    *height = entry->height;
    return entry->pixels;
}

bool recon_icon_draw(struct recon_panel *panel, const char *name,
        int x, int y, int size) {
    int width = 0, height = 0;
    const unsigned char *pixels = recon_icon_get(name, &width, &height);
    if (pixels == NULL) {
        return false;
    }
    recon_draw_image(panel, x, y, size, size, pixels, width, height);
    return true;
}

void recon_icons_forget(void) {
    for (int i = 0; i < g_cache_count; i++) {
        free(g_cache[i].pixels);
        g_cache[i].pixels = NULL;
    }
    g_cache_count = 0;
}
