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
#include "recon_icon_gen.h"
#include "recon_icons.h"
#include "recon_theme.h"
#include "stb_image.h"

/*
 * --- How much is kept, and what happens when it is full ---
 *
 * The cache used to hold 32 icons and, once full, *return NULL* for every
 * further name. Not evict, not reload each time -- refuse. Which meant the
 * thirty-third distinct icon a screen asked for simply did not appear, and no
 * error said so.
 *
 * It was invisible for as long as no single screen wanted more than a few
 * beyond what the desktop and taskbar had already loaded. The account picture
 * chooser wants one per picture; at eight pictures nothing showed the fault
 * and at twenty-four the picker drew six of them and stopped, which looks
 * exactly like sixteen pictures having failed to be written.
 *
 * Two bounds now, and the byte one is the one that matters. A count alone was
 * safe while every icon was 32 by 32 and four kilobytes; the generated set is
 * 128 by 128 now, sixteen times that, and an icon somebody drops in the folder
 * themselves has no size limit at all -- one photograph saved as a PNG could
 * be larger than every icon here put together. So the cache is bounded by what
 * it holds rather than by how many things it holds, and the count is a
 * secondary guard on the bookkeeping.
 *
 * Full means evict the least recently used, not refuse.
 */
#define CACHE_MAX 192
#define CACHE_BYTES_MAX (12u * 1024u * 1024u)
#define PREFERRED_SIZE 32

struct cached_icon {
    char name[64];
    unsigned char *pixels; /* RGBA, or NULL if there is no such icon */
    int width, height;
    bool looked_up;

    /* When this was last asked for. A plain counter rather than a clock: the
     * only question is which of two entries was wanted more recently, and a
     * clock would answer it with more machinery and no more accuracy. */
    unsigned long long used;
};

static struct cached_icon g_cache[CACHE_MAX];
static int g_cache_count;
static unsigned long long g_tick;
static size_t g_cache_bytes;

static size_t entry_bytes(const struct cached_icon *entry) {
    if (entry->pixels == NULL) {
        return 0;
    }
    return (size_t)entry->width * (size_t)entry->height * 4;
}

/*
 * Drop the least recently used entry that is actually holding pixels.
 *
 * Entries remembering that a name has *no* icon are kept: they cost nothing,
 * and they are what stops a missing name being searched for again on every
 * redraw. Returns false when there is nothing left worth dropping, which is
 * the caller's signal to stop trying.
 */
static bool evict_one(void) {
    int oldest = -1;
    for (int i = 0; i < g_cache_count; i++) {
        if (g_cache[i].pixels == NULL) {
            continue;
        }
        if (oldest < 0 || g_cache[i].used < g_cache[oldest].used) {
            oldest = i;
        }
    }
    if (oldest < 0) {
        return false;
    }

    g_cache_bytes -= entry_bytes(&g_cache[oldest]);
    free(g_cache[oldest].pixels);

    /*
     * The last entry moves into the hole rather than everything shifting
     * down. Nothing here depends on the order -- lookup is a scan and
     * recency is a number on the entry -- and a memmove of the whole table
     * on every eviction would be work done to preserve an order nobody reads.
     *
     * Safe because no caller holds a pointer into the cache across another
     * call: recon_icon_get's two callers use what they are handed
     * immediately, one to draw it and one to test it against NULL.
     */
    g_cache[oldest] = g_cache[g_cache_count - 1];
    g_cache_count--;
    return true;
}

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

/*
 * Throw the cache away if the skin has changed since it was filled.
 *
 * Which file a name resolves to now depends on the skin -- a glossy skin looks
 * in a different directory first -- so a cache filled under one skin is wrong
 * under the next. The generation counter is the theme's own and goes up for an
 * edit as well as a switch, which is what makes this correct for somebody
 * changing metric.icon-gloss on a skin they are already using.
 */
static void forget_if_the_skin_moved(void) {
    static unsigned seen;
    static bool ever;

    unsigned now = recon_theme_generation();
    if (ever && now == seen) {
        return;
    }
    ever = true;
    seen = now;
    recon_icons_forget();
}

const unsigned char *recon_icon_get(const char *name, int *width, int *height) {
    forget_if_the_skin_moved();
    if (name == NULL || *name == '\0') {
        return NULL;
    }

    for (int i = 0; i < g_cache_count; i++) {
        if (strcasecmp(g_cache[i].name, name) == 0) {
            g_cache[i].used = ++g_tick;
            if (g_cache[i].pixels == NULL) {
                return NULL;
            }
            /*
             * Written only where a caller asked for them.
             *
             * recon_appicon calls this with both as NULL -- it wants to know
             * whether an icon exists by that name and nothing else -- and
             * this wrote through them regardless. It survived because that
             * call reached a cached entry with pixels only when a name had
             * already been drawn, which is a condition, not a guarantee.
             */
            if (width != NULL) {
                *width = g_cache[i].width;
            }
            if (height != NULL) {
                *height = g_cache[i].height;
            }
            return g_cache[i].pixels;
        }
    }

    /* Room for one more, by count and by weight. Evicting until there is is
     * what makes a screen full of icons work rather than a screen full of
     * icons up to the thirty-second. */
    while (g_cache_count >= CACHE_MAX && evict_one()) {
        /* nothing */
    }
    if (g_cache_count >= CACHE_MAX) {
        return NULL;
    }

    struct cached_icon *entry = &g_cache[g_cache_count++];
    entry->used = ++g_tick;
    snprintf(entry->name, sizeof(entry->name), "%s", name);
    entry->pixels = NULL;
    entry->looked_up = true;

    /*
     * .ico first, since that is the format icons are usually supplied in, then
     * .png for anything produced by a tool that does not write icons.
     */
    static const char *const EXTENSIONS[] = { "ico", "png", NULL };

    /*
     * The glossy set first, when the skin asks for it -- and the flat one
     * underneath either way.
     *
     * A fallback rather than a switch, deliberately. An icon that only exists
     * in the flat set, because somebody added it or replaced it there, still
     * appears under a glossy skin. The alternative is a skin that silently
     * loses icons, which looks like the icons are broken rather than like the
     * skin is.
     */
    bool glossy = recon_theme_metric(RECON_METRIC_ICON_GLOSS) > 0;

    for (int pass = glossy ? 0 : 1; pass < 2 && entry->pixels == NULL; pass++) {
        for (int i = 0; EXTENSIONS[i] != NULL && entry->pixels == NULL; i++) {
            char path[RECON_PATH_MAX];
            if (pass == 0) {
                snprintf(path, sizeof(path), "%s/%s/%s.%s",
                    RECON_DIR_SYSTEM_ICONS, RECON_ICONS_GLOSSY, name,
                    EXTENSIONS[i]);
            } else {
                snprintf(path, sizeof(path), "%s/%s.%s",
                    RECON_DIR_SYSTEM_ICONS, name, EXTENSIONS[i]);
            }
            entry->pixels = try_load(path, &entry->width, &entry->height);
        }
    }

    /* A missing icon is remembered as missing, so a name that has no file is
     * not searched for again on every redraw. */
    if (entry->pixels == NULL) {
        return NULL;
    }

    g_cache_bytes += entry_bytes(entry);
    while (g_cache_bytes > CACHE_BYTES_MAX) {
        /*
         * Note what has just been loaded is the most recently used, so it is
         * the last thing evict_one would choose -- which is what stops a
         * single very large icon from throwing itself away and being reloaded
         * on the next frame forever.
         */
        if (!evict_one()) {
            break;
        }
    }

    if (width != NULL) {
        *width = entry->width;
    }
    if (height != NULL) {
        *height = entry->height;
    }
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
    g_cache_bytes = 0;
}
