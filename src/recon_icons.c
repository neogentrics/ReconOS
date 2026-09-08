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

    /*
     * Every visible pixel is white, and the picture is entirely in the alpha.
     *
     * A whole family of icon sets is drawn this way -- a silhouette, with the
     * colour left to whoever shows it. Worth detecting rather than declaring,
     * because the alternative is a naming rule or a manifest, and both are
     * things somebody has to keep in step with the files. The pixels already
     * say what they are.
     */
    bool mask;

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

/*
 * Is this a silhouette -- white everywhere it is visible at all?
 *
 * Checked once, when the file is loaded, so showing it costs nothing. A single
 * coloured pixel is enough to say no: an icon that is *mostly* white is a
 * picture with white in it, and recolouring that would ruin it.
 *
 * The test is on pixels that are actually there. Anti-aliased edges carry the
 * shape's colour at a low alpha, and a fully transparent pixel's colour is
 * whatever happened to be in the file, which is frequently black and means
 * nothing.
 */
static bool looks_like_a_mask(const unsigned char *rgba, int width,
        int height) {
    if (rgba == NULL || width <= 0 || height <= 0) {
        return false;
    }

    bool seen = false;
    for (int i = 0; i < width * height; i++) {
        const unsigned char *px = rgba + (size_t)i * 4;
        if (px[3] < 8) {
            continue;              /* not really there */
        }
        if (px[0] < 0xF0 || px[1] < 0xF0 || px[2] < 0xF0) {
            return false;
        }
        seen = true;
    }
    return seen;
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

/*
 * The marker that means "the shared set, not the skin's".
 *
 * Carried in the cache key rather than passed alongside it, because the two
 * versions of one name are two different pictures and a cache keyed on the
 * name alone can hold only one of them. A real icon name never begins with
 * this, so it cannot collide.
 */
#define SHARED_ONLY '*'

const unsigned char *recon_icon_get(const char *name, int *width, int *height) {
    forget_if_the_skin_moved();
    if (name == NULL || *name == '\0') {
        return NULL;
    }

    /* Stripped for the lookup on disk; kept for the cache. */
    bool shared_only = (*name == SHARED_ONLY);
    const char *wanted = shared_only ? name + 1 : name;

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

    /*
     * --- Three places to look, in order ---
     *
     *   /System/Icons/<skin>/     the skin's own set, if it has one
     *   /System/Icons/Glossy/     the lit versions, if the skin asks for gloss
     *   /System/Icons/            the set every skin shares
     *
     * A skin brings its own icons by having a directory named after it, and
     * that is the whole of the rule -- no manifest, no metric, nothing to keep
     * in step. Drop a folder called Glass beside the icons and the Glass skin
     * uses what is in it.
     *
     * Each is a *fallback* rather than a switch. An icon that exists only in
     * the shared set still appears under a skin that has its own, because a
     * skin that silently loses icons looks like the icons are broken rather
     * than like the skin is incomplete.
     */
    const char *skin = recon_theme_current();

    for (int pass = 0; pass < 3 && entry->pixels == NULL; pass++) {
        if (shared_only && pass < 2) {
            continue;    /* asked for the shared set and nothing else */
        }
        if (pass == 0 && (skin == NULL || skin[0] == '\0')) {
            continue;
        }
        if (pass == 1 && !glossy) {
            continue;
        }

        for (int i = 0; EXTENSIONS[i] != NULL && entry->pixels == NULL; i++) {
            char path[RECON_PATH_MAX];
            if (pass == 0) {
                snprintf(path, sizeof(path), "%s/%s/%s.%s",
                    RECON_DIR_SYSTEM_ICONS, skin, wanted, EXTENSIONS[i]);
            } else if (pass == 1) {
                snprintf(path, sizeof(path), "%s/%s/%s.%s",
                    RECON_DIR_SYSTEM_ICONS, RECON_ICONS_GLOSSY, wanted,
                    EXTENSIONS[i]);
            } else {
                snprintf(path, sizeof(path), "%s/%s.%s",
                    RECON_DIR_SYSTEM_ICONS, wanted, EXTENSIONS[i]);
            }
            entry->pixels = try_load(path, &entry->width, &entry->height);
        }
    }

    /* A missing icon is remembered as missing, so a name that has no file is
     * not searched for again on every redraw. */
    if (entry->pixels == NULL) {
        return NULL;
    }

    entry->mask = looks_like_a_mask(entry->pixels, entry->width,
        entry->height);

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

/*
 * Is the icon by this name a silhouette waiting to be coloured?
 *
 * Loads it if it is not loaded, because the answer is in the file. False for a
 * name with no icon, which keeps a caller from having to ask twice.
 */
bool recon_icon_is_mask(const char *name) {
    int width = 0, height = 0;
    if (recon_icon_get(name, &width, &height) == NULL) {
        return false;
    }
    for (int i = 0; i < g_cache_count; i++) {
        if (strcasecmp(g_cache[i].name, name) == 0) {
            return g_cache[i].mask;
        }
    }
    return false;
}

/*
 * Below this, a picture is worse than a silhouette.
 *
 * The desktop rule is the other way round and both are true. A desktop icon is
 * drawn four times this size on a photograph nobody chose for it, and a shape
 * with no detail inside it comes out as a blob -- so there, a picture wins.
 *
 * A title bar is sixteen pixels of *coloured chrome*. A silhouette takes the
 * skin's own ink and reads on any of it; a picture keeps whatever colours it
 * was made with and takes its chances. Measured on the Photos window under
 * Metallic in garnet: the picture-frame icon is a blue frame on a dark red
 * bar, which is not an icon anybody can see. The same name's silhouette comes
 * out near-white, because that is what the title text is.
 */
#define ICON_SMALL 20

/*
 * The shape of an icon, in one colour.
 *
 * The shape lives entirely in the alpha channel, so colouring it is a matter
 * of replacing the colour and keeping the alpha -- which is why this can be
 * done at draw time rather than at load: the same file is one icon in a menu,
 * another on a toolbar and a third on a title bar, in three different
 * colours, without three copies of it.
 *
 * Scaled into a scratch buffer at the size wanted and recoloured there, so the
 * averaging that makes a small icon look like a small icon happens on the
 * alpha rather than on the colour -- the same reason recon_draw_image weights
 * by alpha in the first place.
 *
 * `spread` of one draws it four times, offset by a pixel each way: an outline
 * for a picture that is about to be drawn on top of it.
 */
static void draw_silhouette(struct recon_panel *panel,
        const unsigned char *pixels, int width, int height,
        int x, int y, int size, recon_color ink, int spread) {
    size_t count = (size_t)size * (size_t)size;
    unsigned char *tinted = malloc(count * 4);
    if (tinted == NULL) {
        return;
    }

    for (size_t i = 0; i < count; i++) {
        int col = (int)(i % (size_t)size);
        int row = (int)(i / (size_t)size);

        int sx = col * width / size;
        int sy = row * height / size;
        const unsigned char *src = pixels + ((size_t)sy * width + sx) * 4;

        tinted[i * 4 + 0] = (unsigned char)((ink >> 16) & 0xFF);
        tinted[i * 4 + 1] = (unsigned char)((ink >> 8) & 0xFF);
        tinted[i * 4 + 2] = (unsigned char)(ink & 0xFF);
        tinted[i * 4 + 3] = src[3];
    }

    if (spread <= 0) {
        recon_draw_image(panel, x, y, size, size, tinted, size, size);
    } else {
        const int AT[][2] = { {-1, 0}, {1, 0}, {0, -1}, {0, 1} };
        for (size_t i = 0; i < sizeof(AT) / sizeof(AT[0]); i++) {
            recon_draw_image(panel, x + AT[i][0] * spread,
                y + AT[i][1] * spread, size, size, tinted, size, size);
        }
    }
    free(tinted);
}

bool recon_icon_draw_in(struct recon_panel *panel, const char *name,
        int x, int y, int size, recon_color ink) {
    int width = 0, height = 0;
    const unsigned char *pixels = recon_icon_get(name, &width, &height);
    if (pixels == NULL) {
        return false;
    }

    if (!recon_icon_is_mask(name)) {
        /*
         * A picture, drawn as it was made. `ink` is what a silhouette would
         * have been coloured with and has nothing to say about a photograph
         * of a folder.
         *
         * Except at chrome size, where it has one thing to say: the picture
         * keeps whatever colours it was made with, and a title bar is a
         * *coloured* surface the picture has never heard of. Measured on the
         * Photos window under Metallic in garnet -- a blue picture-frame icon
         * on a dark red bar, dark on dark, which is not an icon anybody can
         * see.
         *
         * So it gets a one-pixel outline in the ink the title text is written
         * in. That colour is by definition one this skin knows reads on this
         * surface, and an outline says where the shape is without pretending
         * to know what colour the shape should be.
         */
        if (size > 0 && size <= ICON_SMALL) {
            draw_silhouette(panel, pixels, width, height, x, y, size, ink, 1);
        }
        recon_draw_image(panel, x, y, size, size, pixels, width, height);
        return true;
    }

    /*
     * A silhouette, in the colour asked for.
     *
     * The shape lives entirely in the alpha channel, so colouring it is a
     * matter of replacing the colour and keeping the alpha -- which is why
     * this can be done at draw time rather than at load: the same file is one
     * icon in a menu, another on a toolbar and a third on a title bar, in
     * three different colours, without three copies of it.
     *
     * Scaled into a scratch buffer at the size wanted, then recoloured, so the
     * averaging that makes a small icon look like a small icon happens on the
     * alpha rather than on the colour -- which is the same reason
     * recon_draw_image weights by alpha in the first place.
     */
    draw_silhouette(panel, pixels, width, height, x, y, size, ink, 0);
    return true;
}

bool recon_icon_draw_shared(struct recon_panel *panel, const char *name,
        int x, int y, int size) {
    if (name == NULL || *name == '\0') {
        return false;
    }

    char wanted[80];
    snprintf(wanted, sizeof(wanted), "%c%s", SHARED_ONLY, name);

    int width = 0, height = 0;
    const unsigned char *pixels = recon_icon_get(wanted, &width, &height);
    if (pixels == NULL) {
        return false;
    }
    recon_draw_image(panel, x, y, size, size, pixels, width, height);
    return true;
}

bool recon_icon_draw(struct recon_panel *panel, const char *name,
        int x, int y, int size) {
    /*
     * A silhouette with no colour named takes the surface's text colour,
     * which is the right guess for the majority of places one is drawn -- a
     * menu row, a list, a tile. Somewhere it is wrong, the caller says so:
     * recon_icon_draw_in is the same call with the colour spelled out, and a
     * title bar and a taskbar both want their own.
     */
    return recon_icon_draw_in(panel, name, x, y, size, THEME(SURFACE_TEXT));
}

void recon_icons_forget(void) {
    for (int i = 0; i < g_cache_count; i++) {
        free(g_cache[i].pixels);
        g_cache[i].pixels = NULL;
    }
    g_cache_count = 0;
    g_cache_bytes = 0;
}
