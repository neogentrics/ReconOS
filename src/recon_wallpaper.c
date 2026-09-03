/*
 * Wallpapers. See include/recon_wallpaper.h.
 *
 * The drawn ones are deliberately simple: a vertical wash of colour with a
 * scattering of stars and a horizon. Simple because a wallpaper is looked
 * past rather than at, and because anything with detail in it fights the
 * icons sitting on top. They are generated at the screen's size on first run
 * and written as PNG, so they are ordinary files somebody can replace.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_fs.h"
#include "recon_png.h"
#include "recon_registry.h"
#include "recon_theme.h"
#include "recon_wallpaper.h"

/* Big enough to look right on the screens ReconOS runs on, small enough that
 * generating four of them is not a noticeable pause on first start. */
#define WALLPAPER_WIDTH 1920
#define WALLPAPER_HEIGHT 1080

#define WALLPAPERS_MAX 32
#define NAME_MAX_LEN 96

static char g_names[WALLPAPERS_MAX][NAME_MAX_LEN];
static int g_count;
static char g_current[NAME_MAX_LEN];

/*
 * A repeatable scatter.
 *
 * The stars have to land in the same places every time the same wallpaper is
 * drawn, or regenerating one would produce a different picture under the same
 * name. A hash of the position does that without storing anything.
 */
static unsigned scatter(int x, int y, unsigned salt) {
    unsigned hash = 2166136261u ^ salt;
    hash = (hash ^ (unsigned)x) * 16777619u;
    hash = (hash ^ (unsigned)y) * 16777619u;
    hash ^= hash >> 13;
    return hash;
}

static unsigned mix(unsigned a, unsigned b, int numerator, int denominator) {
    if (denominator <= 0) {
        return a;
    }
    int red = (int)((a >> 16) & 0xFF) +
        (((int)((b >> 16) & 0xFF) - (int)((a >> 16) & 0xFF)) * numerator) / denominator;
    int green = (int)((a >> 8) & 0xFF) +
        (((int)((b >> 8) & 0xFF) - (int)((a >> 8) & 0xFF)) * numerator) / denominator;
    int blue = (int)(a & 0xFF) +
        (((int)(b & 0xFF) - (int)(a & 0xFF)) * numerator) / denominator;

    return 0xFF000000u | ((unsigned)red << 16) | ((unsigned)green << 8) |
        (unsigned)blue;
}

struct drawn_wallpaper {
    const char *name;
    unsigned top;
    unsigned bottom;
    unsigned glow;      /* the band just above the horizon */
    bool stars;
};

/*
 * Four, so every skin has something that does not fight it: two dark, one
 * light, one warm. Named for what they look like rather than for the skin
 * they suit, because a wallpaper somebody likes should not be filed under a
 * skin they have stopped using.
 */
static const struct drawn_wallpaper DRAWN[] = {
    { "Night Sky", 0xFF05070E, 0xFF101828, 0xFF1E3A5F, true },
    { "Deep Field", 0xFF0A0410, 0xFF1A0F26, 0xFF3A1F5C, true },
    { "Daybreak", 0xFFE8EEF6, 0xFFBFD0E4, 0xFFF0DCC0, false },
    { "Ember", 0xFF1A0E08, 0xFF2E1810, 0xFF8A4A20, false },
};

#define DRAWN_COUNT ((int)(sizeof(DRAWN) / sizeof(DRAWN[0])))

static bool draw_one(const struct drawn_wallpaper *style) {
    size_t pixel_count = (size_t)WALLPAPER_WIDTH * WALLPAPER_HEIGHT;
    unsigned *pixels = malloc(pixel_count * sizeof(*pixels));
    if (pixels == NULL) {
        return false;
    }

    /* The horizon sits low, so the busy part is under the windows rather than
     * behind them. */
    int horizon = (WALLPAPER_HEIGHT * 7) / 10;

    /* How far above the horizon the glow starts to show. */
    int glow_reach = horizon / 5;

    for (int y = 0; y < WALLPAPER_HEIGHT; y++) {
        unsigned row;
        if (y < horizon) {
            row = mix(style->top, style->bottom, y, horizon);

            /*
             * And the glow, rising into the last stretch before the horizon.
             *
             * Without this the two halves met at different colours and the
             * horizon was a hard purple band rather than light coming from
             * behind an edge -- the sky ended at one colour and the ground
             * began at another.
             */
            if (y > horizon - glow_reach) {
                int into = y - (horizon - glow_reach);
                row = mix(row, style->glow, into, glow_reach);
            }
        } else {
            /* Below it, the glow falling away into the ground. */
            int depth = y - horizon;
            int span = WALLPAPER_HEIGHT - horizon;
            row = mix(style->glow, style->bottom, depth, span);
        }

        for (int x = 0; x < WALLPAPER_WIDTH; x++) {
            unsigned pixel = row;

            /* A thin bright line exactly at the horizon. */
            if (y == horizon || y == horizon - 1) {
                pixel = mix(row, style->glow, 3, 4);
            }

            if (style->stars && y < horizon - 4) {
                unsigned noise = scatter(x, y, 0x5EED);
                /* About one pixel in twelve hundred, brighter the higher up:
                 * a sky that thins out towards the horizon looks like air. */
                unsigned threshold = 1200u + (unsigned)(y * 3);
                if (noise % threshold == 0) {
                    unsigned brightness = 140u + (noise >> 8) % 116u;
                    pixel = 0xFF000000u | (brightness << 16) |
                        (brightness << 8) | brightness;
                }
            }

            pixels[(size_t)y * WALLPAPER_WIDTH + x] = pixel;
        }
    }

    char path[RECON_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s.png", RECON_DIR_WALLPAPERS,
        style->name);

    bool ok = recon_png_write(path, pixels, WALLPAPER_WIDTH, WALLPAPER_HEIGHT,
        false);
    free(pixels);
    return ok;
}

int recon_wallpapers_write_defaults(void) {
    recon_fs_mkdir("/", RECON_DIR_WALLPAPERS);

    int written = 0;
    for (int i = 0; i < DRAWN_COUNT; i++) {
        char path[RECON_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.png", RECON_DIR_WALLPAPERS,
            DRAWN[i].name);

        /* One somebody replaced stays replaced. */
        if (recon_fs_exists("/", path)) {
            continue;
        }
        if (draw_one(&DRAWN[i])) {
            written++;
        }
    }
    return written;
}

/* --- Choosing --- */

static void scan(void) {
    g_count = 0;

    struct recon_dirent entries[64];
    int found = recon_fs_list("/", RECON_DIR_WALLPAPERS, entries, 64);
    if (found < 0) {
        return;
    }
    if (found > 64) {
        found = 64;
    }

    for (int i = 0; i < found && g_count < WALLPAPERS_MAX; i++) {
        if (entries[i].kind == RECON_FILE_DIRECTORY) {
            continue;
        }
        snprintf(g_names[g_count], NAME_MAX_LEN, "%s", entries[i].name);
        g_count++;
    }
}

int recon_wallpaper_count(void) {
    scan();
    return g_count;
}

bool recon_wallpaper_at(int index, char *name, size_t size) {
    scan();
    if (index < 0 || index >= g_count || name == NULL) {
        return false;
    }
    snprintf(name, size, "%s", g_names[index]);
    return true;
}

static bool exists(const char *name) {
    if (name == NULL || *name == '\0') {
        return false;
    }
    char path[RECON_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", RECON_DIR_WALLPAPERS, name);
    return recon_fs_exists("/", path);
}

const char *recon_wallpaper_current(void) {
    /* The account's own choice comes first. */
    const char *chosen = recon_registry_get(RECON_REG_USER,
        RECON_WALLPAPER_KEY, "");
    if (exists(chosen)) {
        snprintf(g_current, sizeof(g_current), "%s", chosen);
        return g_current;
    }

    /*
     * Then whatever the skin suggests. A skin names a wallpaper by file name,
     * so a skin someone wrote can ship one alongside itself.
     */
    const char *suggested = recon_theme_wallpaper();
    if (exists(suggested)) {
        snprintf(g_current, sizeof(g_current), "%s", suggested);
        return g_current;
    }

    /* Then anything at all, so a desktop is never blank because a setting
     * pointed at a file somebody deleted. */
    scan();
    if (g_count > 0) {
        snprintf(g_current, sizeof(g_current), "%s", g_names[0]);
        return g_current;
    }

    g_current[0] = '\0';
    return g_current;
}

bool recon_wallpaper_set(const char *name) {
    if (name == NULL || *name == '\0') {
        recon_registry_remove(RECON_REG_USER, RECON_WALLPAPER_KEY);
        return true;
    }
    if (!exists(name)) {
        return false;
    }
    return recon_registry_set(RECON_REG_USER, RECON_WALLPAPER_KEY, name);
}
