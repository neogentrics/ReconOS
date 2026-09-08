/*
 * ReconOS UI layer implementation. See include/recon_ui.h.
 *
 * Pixels are ARGB8888, premultiplied-alpha ignored: the shell draws opaque
 * chrome, and text is blended against whatever is already in the buffer.
 */

#define _POSIX_C_SOURCE 200112L

#include <time.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <drm_fourcc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "recon_clip.h"
#include "recon_theme.h"
#include "recon_ui.h"

/* --- Font --- */

/*
 * Glyphs are rasterized once on first use and kept as 8-bit coverage masks.
 * A desktop draws the same few dozen characters over and over, so caching
 * them turns text drawing into a series of blends.
 */
#define GLYPH_FIRST 32  /* space */
#define GLYPH_LAST 126  /* tilde */
#define GLYPH_COUNT (GLYPH_LAST - GLYPH_FIRST + 1)

/*
 * And a small cache for everything else.
 *
 * Text was walked a byte at a time and glyphs were kept only for 32..126, so
 * anything outside that -- an accent, an em dash, a character from another
 * script -- was three or four bytes each of which drew nothing. Not a box, not
 * a question mark: nothing, so a sentence arrived with a hole in it where its
 * punctuation should be, which reads as a fault in the sentence rather than in
 * the font. The typeface had the glyphs the whole time.
 *
 * Direct-mapped rather than a list, because the alternative to a fixed size is
 * a cache that grows with whatever a person types, and a desktop drawing
 * mostly English will use a handful of these. A collision evicts, which costs
 * one rasterization and nothing else.
 */
#define GLYPH_EXTRA 128

struct recon_glyph {
    unsigned char *bitmap; /* coverage, width*height bytes; NULL until cached */
    int width, height;
    int bearing_x, bearing_y; /* offset from pen position to bitmap corner */
    int advance;              /* pen movement to the next character */
    bool cached;
};

struct recon_font {
    unsigned char *file_data;
    stbtt_fontinfo info;
    float scale;
    int ascent, descent, line_gap;
    int pixel_height;
    struct recon_glyph glyphs[GLYPH_COUNT];

    struct recon_glyph extra[GLYPH_EXTRA];
    /* Which character each slot currently holds. Zero means empty; zero is
     * not a character anybody draws. */
    uint32_t extra_for[GLYPH_EXTRA];
};

/* Searched in order when no font path is given. */
/*
 * Extra space between letters and lines. Global rather than per-font: it is a
 * property of the person reading, not of the typeface.
 */
static int g_letter_spacing;
static int g_line_spacing;

void recon_text_set_spacing(int letter, int line) {
    /* Clamped. Negative spacing would overlap glyphs into each other, and an
     * enormous value would push every label off its own button. */
    g_letter_spacing = letter < 0 ? 0 : (letter > 16 ? 16 : letter);
    g_line_spacing = line < 0 ? 0 : (line > 32 ? 32 : line);
}

int recon_text_letter_spacing(void) {
    return g_letter_spacing;
}

int recon_text_line_spacing(void) {
    return g_line_spacing;
}

static const char *const FONT_SEARCH_PATHS[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    NULL,
};

static unsigned char *read_whole_file(const char *path, size_t *size_out) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size <= 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    unsigned char *data = malloc((size_t)size);
    if (data == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);

    *size_out = (size_t)size;
    return data;
}

struct recon_font *recon_font_load(const char *path, int pixel_height) {
    if (pixel_height <= 0) {
        return NULL;
    }

    unsigned char *data = NULL;
    size_t size = 0;

    if (path != NULL) {
        data = read_whole_file(path, &size);
    } else {
        for (int i = 0; FONT_SEARCH_PATHS[i] != NULL && data == NULL; i++) {
            data = read_whole_file(FONT_SEARCH_PATHS[i], &size);
            if (data != NULL) {
                path = FONT_SEARCH_PATHS[i];
            }
        }
    }

    if (data == NULL) {
        wlr_log(WLR_ERROR, "ReconOS: no usable font found");
        return NULL;
    }

    struct recon_font *font = calloc(1, sizeof(*font));
    if (font == NULL) {
        free(data);
        return NULL;
    }
    font->file_data = data;

    if (!stbtt_InitFont(&font->info, data, stbtt_GetFontOffsetForIndex(data, 0))) {
        wlr_log(WLR_ERROR, "ReconOS: '%s' is not a usable font", path);
        free(data);
        free(font);
        return NULL;
    }

    font->pixel_height = pixel_height;
    font->scale = stbtt_ScaleForPixelHeight(&font->info, (float)pixel_height);
    stbtt_GetFontVMetrics(&font->info, &font->ascent, &font->descent, &font->line_gap);

    wlr_log(WLR_INFO, "ReconOS: font '%s' at %dpx", path, pixel_height);
    return font;
}

void recon_font_destroy(struct recon_font *font) {
    if (font == NULL) {
        return;
    }
    for (int i = 0; i < GLYPH_COUNT; i++) {
        free(font->glyphs[i].bitmap);
    }
    for (int i = 0; i < GLYPH_EXTRA; i++) {
        free(font->extra[i].bitmap);
    }
    free(font->file_data);
    free(font);
}

int recon_font_ascent(struct recon_font *font) {
    return font != NULL ? (int)(font->ascent * font->scale) : 0;
}

int recon_font_line_height(struct recon_font *font) {
    if (font == NULL) {
        return 0;
    }
    return (int)((font->ascent - font->descent + font->line_gap) * font->scale)
        + g_line_spacing;
}

/* Fill in one glyph from the typeface. */
static void rasterize(struct recon_font *font, struct recon_glyph *glyph,
        uint32_t c) {
    int advance, left_bearing;
    stbtt_GetCodepointHMetrics(&font->info, (int)c, &advance, &left_bearing);
    glyph->advance = (int)(advance * font->scale + 0.5f);

    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&font->info, (int)c, font->scale, font->scale,
        &x0, &y0, &x1, &y1);

    glyph->width = x1 - x0;
    glyph->height = y1 - y0;
    glyph->bearing_x = x0;
    glyph->bearing_y = y0;

    if (glyph->width > 0 && glyph->height > 0) {
        glyph->bitmap = malloc((size_t)glyph->width * glyph->height);
        if (glyph->bitmap != NULL) {
            stbtt_MakeCodepointBitmap(&font->info, glyph->bitmap,
                glyph->width, glyph->height, glyph->width,
                font->scale, font->scale, (int)c);
        }
    }

    glyph->cached = true;
}

/* Rasterize a character on first use; later calls reuse the cached mask. */
static struct recon_glyph *glyph_for(struct recon_font *font, uint32_t c) {
    if (font == NULL || c < GLYPH_FIRST) {
        return NULL;
    }

    if (c <= GLYPH_LAST) {
        struct recon_glyph *glyph = &font->glyphs[c - GLYPH_FIRST];
        if (!glyph->cached) {
            rasterize(font, glyph, c);
        }
        return glyph;
    }

    struct recon_glyph *glyph = &font->extra[c % GLYPH_EXTRA];
    if (glyph->cached && font->extra_for[c % GLYPH_EXTRA] == c) {
        return glyph;
    }

    /* A different character was here. Its mask goes; one rasterization is
     * the whole cost of a collision. */
    free(glyph->bitmap);
    memset(glyph, 0, sizeof(*glyph));
    font->extra_for[c % GLYPH_EXTRA] = c;

    rasterize(font, glyph, c);
    return glyph;
}

/*
 * The next character, and how many bytes it took.
 *
 * Enough UTF-8 to read what the system actually holds: the text files it
 * ships, names people type, and anything pasted in. A malformed sequence is
 * read as one byte so the walk always advances -- a decoder that can stall on
 * bad input is a decoder that hangs the desktop on a corrupt file name.
 */
static uint32_t next_codepoint(const unsigned char *p, int *length) {
    *length = 1;

    if (p[0] < 0x80) {
        return p[0];
    }

    int extra;
    uint32_t value;

    if ((p[0] & 0xE0) == 0xC0) {
        extra = 1;
        value = p[0] & 0x1Fu;
    } else if ((p[0] & 0xF0) == 0xE0) {
        extra = 2;
        value = p[0] & 0x0Fu;
    } else if ((p[0] & 0xF8) == 0xF0) {
        extra = 3;
        value = p[0] & 0x07u;
    } else {
        return p[0];   /* A continuation byte on its own, or worse. */
    }

    for (int i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            return p[0];   /* Cut short. Take the lead byte and move on. */
        }
        value = (value << 6) | (uint32_t)(p[i] & 0x3F);
    }

    *length = extra + 1;
    return value;
}

/*
 * The shared fonts, one per size asked for.
 *
 * Small and fixed: a desktop uses two or three sizes -- body text, headings,
 * and the splash -- and a system that has quietly loaded thirty typefaces has
 * a leak rather than a need.
 */
#define SYSTEM_FONTS_MAX 6

static struct {
    int pixel_height;
    struct recon_font *font;
} g_system_fonts[SYSTEM_FONTS_MAX];

struct recon_font *recon_font_system(int pixel_height) {
    for (int i = 0; i < SYSTEM_FONTS_MAX; i++) {
        if (g_system_fonts[i].font != NULL &&
                g_system_fonts[i].pixel_height == pixel_height) {
            return g_system_fonts[i].font;
        }
    }

    for (int i = 0; i < SYSTEM_FONTS_MAX; i++) {
        if (g_system_fonts[i].font != NULL) {
            continue;
        }

        struct recon_font *font = recon_font_load(getenv("RECONOS_FONT"),
            pixel_height);
        if (font == NULL) {
            return NULL;
        }
        g_system_fonts[i].pixel_height = pixel_height;
        g_system_fonts[i].font = font;
        return font;
    }

    /*
     * Out of slots. The nearest size already loaded, rather than NULL: text
     * slightly the wrong size is a cosmetic fault, and no text at all is not.
     *
     * Actually nearest, which it was not. This returned slot zero -- the
     * *first* size ever loaded, which is only the nearest by coincidence --
     * while the comment above it claimed otherwise. Nothing had reached this
     * path until Notepad gained a text size somebody can drive from 9 to 32,
     * at which point the seventh size would have come back as whatever the
     * splash screen happened to want.
     */
    struct recon_font *nearest = NULL;
    int best = 0;
    for (int i = 0; i < SYSTEM_FONTS_MAX; i++) {
        if (g_system_fonts[i].font == NULL) {
            continue;
        }
        int apart = g_system_fonts[i].pixel_height - pixel_height;
        if (apart < 0) {
            apart = -apart;
        }
        if (nearest == NULL || apart < best) {
            nearest = g_system_fonts[i].font;
            best = apart;
        }
    }

    wlr_log(WLR_ERROR, "ReconOS: no room for a %dpx font; using %dpx",
        pixel_height, pixel_height + best);
    return nearest;
}

/*
 * --- The other faces ---
 *
 * Two of them, each in its own small cache, and one function behind both.
 *
 * A terminal in a proportional face cannot line anything up, and half of what
 * this system prints is a table: `apps` puts a name, a version, a state and an
 * origin in four columns, and in DejaVu Sans those columns wander by a
 * character or two on every row. The interpreter is already writing columns
 * with `%-20s`; the font was throwing that work away.
 *
 * Bold arrived with the web viewer, where a heading that is only *larger* than
 * the text reads as text that is larger, rather than as a heading.
 *
 * Separate caches rather than one, because they are wanted at different sizes
 * for different reasons and a shared pool would have the terminal evicting the
 * taskbar's font. One *lookup* though: written twice, the second copy is the
 * one that misses a fix.
 */
static const char *const MONO_SEARCH_PATHS[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
    "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
    NULL,
};

static const char *const BOLD_SEARCH_PATHS[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    "/usr/share/fonts/truetype/ubuntu/Ubuntu-B.ttf",
    NULL,
};

#define FACE_FONTS_MAX 4

struct face_cache {
    const char *const *paths;
    /* An environment variable that overrides the search, so a machine with
     * neither face can be given one without rebuilding. */
    const char *override;
    /* Said once when there is no such face, rather than on every lookup. */
    const char *complaint;
    bool complained;
    struct {
        int pixel_height;
        struct recon_font *font;
    } slots[FACE_FONTS_MAX];
};

static struct face_cache g_mono = {
    .paths = MONO_SEARCH_PATHS,
    .override = "RECONOS_MONO_FONT",
    .complaint = "no fixed-width font found; columns will not line up",
};

static struct face_cache g_bold = {
    .paths = BOLD_SEARCH_PATHS,
    .override = "RECONOS_BOLD_FONT",
    .complaint = "no bold font found; headings will be large but not bold",
};

static struct recon_font *face_at(struct face_cache *cache, int pixel_height) {
    for (int i = 0; i < FACE_FONTS_MAX; i++) {
        if (cache->slots[i].font != NULL &&
                cache->slots[i].pixel_height == pixel_height) {
            return cache->slots[i].font;
        }
    }

    for (int i = 0; i < FACE_FONTS_MAX; i++) {
        if (cache->slots[i].font != NULL) {
            continue;
        }

        struct recon_font *font = NULL;
        const char *chosen = getenv(cache->override);
        if (chosen != NULL && *chosen != '\0') {
            font = recon_font_load(chosen, pixel_height);
        }
        for (int p = 0; font == NULL && cache->paths[p] != NULL; p++) {
            font = recon_font_load(cache->paths[p], pixel_height);
        }

        /*
         * No such face on this machine. The system font rather than nothing --
         * text in the wrong weight is a cosmetic fault and a blank rectangle is
         * not -- and said out loud once, because "why are my columns crooked"
         * should have an answer somewhere other than in somebody's head.
         */
        if (font == NULL) {
            if (!cache->complained) {
                cache->complained = true;
                wlr_log(WLR_INFO, "ReconOS: %s", cache->complaint);
            }
            return recon_font_system(pixel_height);
        }

        cache->slots[i].pixel_height = pixel_height;
        cache->slots[i].font = font;
        return font;
    }

    /* Out of slots: the nearest size loaded, for the reason given above the
     * system font's version of this. */
    struct recon_font *nearest = NULL;
    int best = 0;
    for (int i = 0; i < FACE_FONTS_MAX; i++) {
        if (cache->slots[i].font == NULL) {
            continue;
        }
        int apart = cache->slots[i].pixel_height - pixel_height;
        if (apart < 0) {
            apart = -apart;
        }
        if (nearest == NULL || apart < best) {
            nearest = cache->slots[i].font;
            best = apart;
        }
    }
    return nearest;
}

static void face_finish(struct face_cache *cache) {
    for (int i = 0; i < FACE_FONTS_MAX; i++) {
        if (cache->slots[i].font != NULL) {
            recon_font_destroy(cache->slots[i].font);
            cache->slots[i].font = NULL;
            cache->slots[i].pixel_height = 0;
        }
    }
    cache->complained = false;
}

struct recon_font *recon_font_monospace(int pixel_height) {
    return face_at(&g_mono, pixel_height);
}

struct recon_font *recon_font_bold(int pixel_height) {
    return face_at(&g_bold, pixel_height);
}

void recon_font_system_finish(void) {
    face_finish(&g_mono);
    face_finish(&g_bold);

    for (int i = 0; i < SYSTEM_FONTS_MAX; i++) {
        recon_font_destroy(g_system_fonts[i].font);
        g_system_fonts[i].font = NULL;
        g_system_fonts[i].pixel_height = 0;
    }
}

bool recon_font_reload(struct recon_font *font, const char *path,
        int pixel_height) {
    if (font == NULL) {
        return false;
    }

    struct recon_font *replacement = recon_font_load(path, pixel_height);
    if (replacement == NULL) {
        /* The old one is left alone. A desktop with no font is worse than one
         * with the font the reader was trying to change. */
        return false;
    }

    /* Everything the old one owned goes; then its contents are taken over, so
     * every window still pointing at this struct now draws with the new
     * typeface without knowing anything happened. */
    for (int i = 0; i < GLYPH_COUNT; i++) {
        free(font->glyphs[i].bitmap);
    }
    free(font->file_data);

    *font = *replacement;
    free(replacement);
    return true;
}

int recon_text_width(struct recon_font *font, const char *text) {
    if (font == NULL || text == NULL) {
        return 0;
    }

    int width = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ) {
        int length = 1;
        uint32_t c = next_codepoint(p, &length);
        p += length;

        struct recon_glyph *glyph = glyph_for(font, c);
        if (glyph != NULL) {
            width += glyph->advance + g_letter_spacing;
        }
    }

    /* The gap belongs between letters, not after the last one, or every
     * measurement is one gap too wide and text centres slightly off. */
    return width > 0 ? width - g_letter_spacing : 0;
}

/* --- Panel --- */

struct recon_hit_region {
    int x, y, w, h;
    uint32_t id;

    /*
     * Drawn, and explains itself, and cannot be pressed.
     *
     * A disabled control still has a region so that pointing at it can say
     * why it is unavailable. What it must not do is answer a click, and
     * before this the only way to arrange that was for every application to
     * remember its own guard next to its own switch statement -- which is
     * the arrangement that had "Half" shrinking a picture that was already
     * at its floor in one place and refusing in another.
     */
    bool inert;

    /* What this thing is, for the tooltip. Empty for most regions: a control
     * whose label already says it needs nothing said twice. */
    char tip[80];
};

#define MAX_HIT_REGIONS 64

struct recon_panel {
    struct wlr_scene_buffer *scene_buffer;
    int width, height;
    uint32_t *pixels; /* ARGB8888, width*height */

    struct recon_hit_region hits[MAX_HIT_REGIONS];
    size_t hit_count;

    /*
     * Which region the pointer is over, and which it went down on.
     *
     * Not cleared with the hit list. The regions are rebuilt on every repaint
     * and these are not: they describe the pointer, which does not stop being
     * where it is because the window redrew.
     */
    uint32_t hot;
    uint32_t held;
};

void recon_panel_set_hot(struct recon_panel *panel, uint32_t id) {
    if (panel != NULL) {
        panel->hot = id;
    }
}

uint32_t recon_panel_hot(const struct recon_panel *panel) {
    return panel != NULL ? panel->hot : RECON_HIT_NONE;
}

void recon_panel_set_held(struct recon_panel *panel, uint32_t id) {
    if (panel != NULL) {
        panel->held = id;
    }
}

uint32_t recon_panel_held(const struct recon_panel *panel) {
    return panel != NULL ? panel->held : RECON_HIT_NONE;
}

bool recon_panel_read(struct recon_panel *panel, int x, int y, int w, int h,
        uint32_t *out) {
    if (panel == NULL || out == NULL || w <= 0 || h <= 0) {
        return false;
    }
    if (x < 0 || y < 0 || x + w > panel->width || y + h > panel->height) {
        return false;
    }

    for (int row = 0; row < h; row++) {
        memcpy(out + (size_t)row * w,
            panel->pixels + (size_t)(y + row) * panel->width + x,
            (size_t)w * sizeof(uint32_t));
    }
    return true;
}

/* A wlr_buffer over the panel's pixels, handed to the scene graph. */
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

struct recon_panel *recon_panel_create(struct wlr_scene_tree *parent,
        int width, int height) {
    if (width <= 0 || height <= 0) {
        return NULL;
    }

    struct recon_panel *panel = calloc(1, sizeof(*panel));
    if (panel == NULL) {
        return NULL;
    }

    panel->width = width;
    panel->height = height;
    panel->pixels = calloc((size_t)width * height, sizeof(uint32_t));
    if (panel->pixels == NULL) {
        free(panel);
        return NULL;
    }

    /* Created with no buffer; commit installs one. */
    panel->scene_buffer = wlr_scene_buffer_create(parent, NULL);
    if (panel->scene_buffer == NULL) {
        free(panel->pixels);
        free(panel);
        return NULL;
    }

    return panel;
}

void recon_panel_destroy(struct recon_panel *panel) {
    if (panel == NULL) {
        return;
    }
    if (panel->scene_buffer != NULL) {
        wlr_scene_node_destroy(&panel->scene_buffer->node);
    }
    free(panel->pixels);
    free(panel);
}

bool recon_panel_resize(struct recon_panel *panel, int width, int height) {
    if (panel == NULL || width <= 0 || height <= 0) {
        return false;
    }
    if (panel->width == width && panel->height == height) {
        return true;
    }

    uint32_t *pixels = calloc((size_t)width * height, sizeof(uint32_t));
    if (pixels == NULL) {
        return false;
    }

    /* Safe to free now: committed buffers hold their own copies. */
    free(panel->pixels);
    panel->pixels = pixels;
    panel->width = width;
    panel->height = height;
    return true;
}

/*
 * Write a committed panel to a file when RECONOS_DEBUG_DUMP names a directory.
 *
 * This exists to answer one question that guesswork could not: whether pixels
 * leaving a panel are already wrong, or only become wrong further down. PPM
 * because it needs no encoder.
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
    fprintf(f, "P6\n%d %d\n255\n", panel->width, panel->height);
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

void recon_panel_commit(struct recon_panel *panel) {
    if (panel == NULL || panel->scene_buffer == NULL) {
        return;
    }

    struct panel_buffer *buf = calloc(1, sizeof(*buf));
    if (buf == NULL) {
        return;
    }

    size_t count = (size_t)panel->width * panel->height;
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
     * Converting at the copy rather than in the drawing code keeps one model
     * inside the program and one at the boundary, which is the only
     * arrangement where "what alpha means" has a single answer in each place.
     */
    for (size_t i = 0; i < count; i++) {
        uint32_t px = panel->pixels[i];
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
    buf->stride = (size_t)panel->width * 4;

    dump_panel(panel, buf->pixels);

    wlr_buffer_init(&buf->base, &panel_buffer_impl, panel->width, panel->height);

    /* The scene takes its own reference; drop ours so the buffer is released
     * when the scene is done with it. */
    wlr_scene_buffer_set_buffer(panel->scene_buffer, &buf->base);
    wlr_buffer_drop(&buf->base);

    wlr_scene_buffer_set_dest_size(panel->scene_buffer, panel->width, panel->height);
}

void recon_panel_set_position(struct recon_panel *panel, int x, int y) {
    if (panel != NULL && panel->scene_buffer != NULL) {
        wlr_scene_node_set_position(&panel->scene_buffer->node, x, y);
    }
}

void recon_panel_raise_to_top(struct recon_panel *panel) {
    if (panel != NULL && panel->scene_buffer != NULL) {
        wlr_scene_node_raise_to_top(&panel->scene_buffer->node);
    }
}

void recon_panel_set_enabled(struct recon_panel *panel, bool enabled) {
    if (panel != NULL && panel->scene_buffer != NULL) {
        wlr_scene_node_set_enabled(&panel->scene_buffer->node, enabled);
    }
}

void recon_panel_position(const struct recon_panel *panel, int *x, int *y) {
    if (panel == NULL || panel->scene_buffer == NULL) {
        return;
    }
    /* Read back from the scene node rather than keeping a copy on the panel.
     * The node is what actually decides where this is drawn, so a second
     * record of it could only ever be right or wrong, never authoritative. */
    if (x != NULL) {
        *x = panel->scene_buffer->node.x;
    }
    if (y != NULL) {
        *y = panel->scene_buffer->node.y;
    }
}

int recon_panel_width(const struct recon_panel *panel) {
    return panel != NULL ? panel->width : 0;
}

int recon_panel_height(const struct recon_panel *panel) {
    return panel != NULL ? panel->height : 0;
}

struct wlr_scene_node *recon_panel_node(struct recon_panel *panel) {
    if (panel == NULL || panel->scene_buffer == NULL) {
        return NULL;
    }
    return &panel->scene_buffer->node;
}

/* --- Drawing --- */

void recon_fill(struct recon_panel *panel, recon_color color) {
    if (panel == NULL) {
        return;
    }
    size_t count = (size_t)panel->width * panel->height;
    for (size_t i = 0; i < count; i++) {
        panel->pixels[i] = color;
    }
}

/* Clamp a rectangle to the panel. Returns false if nothing is left. */
static bool clip_rect(const struct recon_panel *panel, int *x, int *y,
        int *w, int *h) {
    if (*w <= 0 || *h <= 0) {
        return false;
    }
    if (*x < 0) {
        *w += *x;
        *x = 0;
    }
    if (*y < 0) {
        *h += *y;
        *y = 0;
    }
    if (*x + *w > panel->width) {
        *w = panel->width - *x;
    }
    if (*y + *h > panel->height) {
        *h = panel->height - *y;
    }
    return *w > 0 && *h > 0;
}

void recon_panel_fade(struct recon_panel *panel, int x, int y, int w, int h,
        uint8_t alpha) {
    if (alpha == 255) {
        return;                        /* opaque: the ordinary case */
    }
    if (panel == NULL || !clip_rect(panel, &x, &y, &w, &h)) {
        return;
    }

    for (int row = y; row < y + h; row++) {
        uint32_t *p = panel->pixels + (size_t)row * panel->width + x;
        for (int col = 0; col < w; col++) {
            p[col] = recon_color_fade(p[col], alpha);
        }
    }
}

void recon_fill_rect(struct recon_panel *panel, int x, int y, int w, int h,
        recon_color color) {
    if (panel == NULL || !clip_rect(panel, &x, &y, &w, &h)) {
        return;
    }
    for (int row = y; row < y + h; row++) {
        uint32_t *p = panel->pixels + (size_t)row * panel->width + x;
        for (int col = 0; col < w; col++) {
            p[col] = color;
        }
    }
}

/*
 * Source-over, in place.
 *
 * The destination is usually opaque and the arithmetic then reduces to a
 * straight mix, but it is not always: a Glass window's own frame is
 * translucent, and a dialog dimming a form drawn on one has to end up with an
 * alpha that is the two combined rather than the overlay's. Doing it properly
 * is a dozen lines and costs nothing outside the loop.
 */
void recon_blend_rect(struct recon_panel *panel, int x, int y, int w, int h,
        recon_color color) {
    unsigned sa = (color >> 24) & 0xFFu;

    /* Nothing to lay on, and the fully opaque case is a fill -- which is
     * faster and, more usefully, is exactly what a reader would expect. */
    if (sa == 0u) {
        return;
    }
    if (sa == 0xFFu) {
        recon_fill_rect(panel, x, y, w, h, color);
        return;
    }

    if (panel == NULL || !clip_rect(panel, &x, &y, &w, &h)) {
        return;
    }

    unsigned sr = (color >> 16) & 0xFFu;
    unsigned sg = (color >> 8) & 0xFFu;
    unsigned sb = color & 0xFFu;

    for (int row = y; row < y + h; row++) {
        uint32_t *p = panel->pixels + (size_t)row * panel->width + x;
        for (int col = 0; col < w; col++) {
            uint32_t d = p[col];
            unsigned da = (d >> 24) & 0xFFu;
            unsigned dr = (d >> 16) & 0xFFu;
            unsigned dg = (d >> 8) & 0xFFu;
            unsigned db = d & 0xFFu;

            /* How much of the destination survives. */
            unsigned kept = da * (255u - sa) / 255u;
            unsigned oa = sa + kept;
            if (oa == 0u) {
                p[col] = 0u;
                continue;
            }

            unsigned orr = (sr * sa + dr * kept) / oa;
            unsigned og = (sg * sa + dg * kept) / oa;
            unsigned ob = (sb * sa + db * kept) / oa;

            p[col] = (oa << 24) | (orr << 16) | (og << 8) | ob;
        }
    }
}

/*
 * One channel of the ramp, rounded rather than truncated.
 *
 * Truncating loses most of a step on a short gradient: a title bar is under
 * thirty rows, so an eight-unit difference between the two colours moves by
 * less than one unit per row and integer division floors nearly all of it
 * away, leaving a flat fill that was supposed to be a gradient.
 */
static unsigned ramp(unsigned from, unsigned to, int shift, int step, int of) {
    int a = (int)((from >> shift) & 0xFF);
    int b = (int)((to >> shift) & 0xFF);
    int value = a + ((b - a) * step * 2 + (of > 0 ? of : 1)) / ((of > 0 ? of : 1) * 2);
    if (value < 0) { value = 0; }
    if (value > 255) { value = 255; }
    return (unsigned)value;
}

/*
 * Fill the way the skin says a role should look.
 *
 * Here rather than in recon_theme.c, which is where it reads more naturally,
 * because the skin tests link recon_theme.c on its own -- deliberately, so
 * that measuring a palette does not need a compositor. A drawing call in
 * there left them with undefined references to the drawing module and to
 * wlroots behind it. Asking the theme a question is cheap in either
 * direction; drawing is what has the dependencies, so drawing is where this
 * belongs.
 */
/*
 * Mix `over` into `base` by `amount` out of 255.
 *
 * Used where a shape's edge falls partway across a pixel, so a curve reads as
 * a curve rather than as a staircase. Alpha is taken from the base, because
 * everything this is used on is already opaque and a corner that also went
 * transparent would show whatever is beneath the panel.
 */
static uint32_t blend_over(uint32_t base, uint32_t over, int amount) {
    if (amount <= 0) {
        return base;
    }
    if (amount >= 255) {
        return (base & 0xFF000000u) | (over & 0x00FFFFFFu);
    }

    unsigned out = base & 0xFF000000u;
    for (int shift = 0; shift <= 16; shift += 8) {
        int a = (int)((base >> shift) & 0xFF);
        int b = (int)((over >> shift) & 0xFF);
        int mixed = a + ((b - a) * amount) / 255;
        out |= (unsigned)mixed << shift;
    }
    return out;
}

void recon_fill_role(struct recon_panel *panel, int x, int y, int w, int h,
        enum recon_theme_role role) {
    recon_color from, to;
    if (recon_theme_gradient(role, &from, &to)) {
        recon_fill_gradient(panel, x, y, w, h, from, to);
        return;
    }
    recon_fill_rect(panel, x, y, w, h, recon_theme_color(role));
}

void recon_wash_role(struct recon_panel *panel, int x, int y, int w, int h,
        enum recon_theme_role role, uint8_t amount) {
    if (panel == NULL || amount == 0) {
        return;
    }

    recon_color from, to;
    bool graded = recon_theme_gradient(role, &from, &to);
    if (!graded) {
        from = to = recon_theme_color(role);
    }

    /*
     * The ramp is positioned against the rectangle asked for, the same way
     * recon_fill_gradient does it -- and that is worth naming as an
     * approximation rather than left to be discovered. A caller washing the
     * *inside* of a control passes a rectangle a few pixels shorter than the
     * one the fill used, so on a skin whose role carries a gradient the ramp
     * here is stretched across slightly less height than the one underneath.
     *
     * One skin of eleven puts a gradient on a taskbar button, over
     * twenty-eight rows, between two colours a few units apart. The error is
     * a fraction of a unit and the alternative is passing two rectangles to
     * every call. Named here so that a caller washing something tall and
     * steeply graded knows to pass the outer rectangle and clip instead.
     */
    int want_y = y;
    int want_h = h;
    if (!clip_rect(panel, &x, &y, &w, &h)) {
        return;
    }

    int last = want_h > 1 ? want_h - 1 : 1;
    for (int row = y; row < y + h; row++) {
        recon_color color;
        if (graded) {
            int step = row - want_y;
            color = 0xFF000000u |
                (ramp(from, to, 16, step, last) << 16) |
                (ramp(from, to, 8, step, last) << 8) |
                ramp(from, to, 0, step, last);
        } else {
            color = from;
        }

        uint32_t *p = panel->pixels + (size_t)row * panel->width + x;
        for (int col = 0; col < w; col++) {
            p[col] = blend_over(p[col], color, amount);
        }
    }
}


/*
 * Round all four corners of a rectangle already drawn into the panel.
 *
 * Unlike a window's top corners, this fills the corner back in with what is
 * behind it rather than clearing it to transparent: a button sits on a panel
 * that has already been painted, and punching a hole in it would show the
 * wallpaper through the middle of a title bar.
 */
static void round_corners(struct recon_panel *panel, int x, int y, int w,
        int h, int radius, recon_color behind, recon_color edge,
        bool edge_wanted) {
    if (panel == NULL || radius <= 0 || w <= 0 || h <= 0) {
        return;
    }
    if (radius * 2 > w || radius * 2 > h) {
        return;    /* Rounder than the shape; leave it square. */
    }

    const int SAMPLES = 4;

    /*
     * Sampled in integers, not in doubles.
     *
     * `(sx + 0.5) / SAMPLES` is a fraction, and multiplying the whole
     * comparison through by `2 * SAMPLES` clears it exactly: twice
     * `sx + 0.5` is `2 * sx + 1`, with nothing left over. So this is not a
     * fixed-point approximation of what was here before -- it is the same
     * test with the floating-point rounding taken out.
     *
     * Integers because this file is headed for `freestanding/`, where there
     * is no floating point at all: the kernel builds with
     * `-mgeneral-regs-only` on ARM, where a `double` is a compile error, and
     * with the FPU off on x86, where it is silent corruption instead.
     *
     * The squares are 64-bit. At a radius of 2048 the sum still fits in 32,
     * but "still fits" is a fact about today's window sizes rather than about
     * this code, and it costs nothing here to stop it being a question.
     */
    const int UNIT = SAMPLES * 2;
    const long long REACH = (long long)radius * UNIT;

    for (int dy = 0; dy < radius; dy++) {
        for (int dx = 0; dx < radius; dx++) {
            int inside = 0;

            for (int sy = 0; sy < SAMPLES; sy++) {
                for (int sx = 0; sx < SAMPLES; sx++) {
                    long long ox = (long long)(radius - dx) * UNIT
                        - (2 * sx + 1);
                    long long oy = (long long)(radius - dy) * UNIT
                        - (2 * sy + 1);
                    if (ox * ox + oy * oy <= REACH * REACH) {
                        inside++;
                    }
                }
            }

            if (inside == SAMPLES * SAMPLES && !edge_wanted) {
                continue;
            }

            /* How much of the corner belongs to what is behind it. */
            int coverage = 255 - (inside * 255) / (SAMPLES * SAMPLES);

            /*
             * How much of this pixel falls in the one-pixel ring just inside
             * the curve -- which is where an outline goes.
             *
             * Measured as the difference between two circles rather than
             * drawn as a second shape, so the outline is exactly as
             * anti-aliased as the edge it follows and cannot drift a pixel
             * away from it. Zero when no outline was asked for.
             */
            int ring = 0;
            if (edge_wanted) {
                int within = 0;
                long long inner = REACH - UNIT;   /* one pixel further in */
                if (inner < 0) {
                    inner = 0;
                }
                for (int sy = 0; sy < SAMPLES; sy++) {
                    for (int sx = 0; sx < SAMPLES; sx++) {
                        long long ox = (long long)(radius - dx) * UNIT
                            - (2 * sx + 1);
                        long long oy = (long long)(radius - dy) * UNIT
                            - (2 * sy + 1);
                        if (ox * ox + oy * oy <= inner * inner) {
                            within++;
                        }
                    }
                }
                ring = ((inside - within) * 255) / (SAMPLES * SAMPLES);
            }

            const int corners[4][2] = {
                { x + dx,             y + dy },
                { x + w - 1 - dx,     y + dy },
                { x + dx,             y + h - 1 - dy },
                { x + w - 1 - dx,     y + h - 1 - dy },
            };

            for (int i = 0; i < 4; i++) {
                int cx = corners[i][0];
                int cy = corners[i][1];
                if (cx < 0 || cy < 0 || cx >= panel->width ||
                        cy >= panel->height) {
                    continue;
                }

                uint32_t *pixel = panel->pixels + (size_t)cy * panel->width + cx;
                /* The outline first, then what is outside the shape over the
                 * top of it: the two coverages do not overlap, and doing them
                 * in this order means the cleared side always wins at the
                 * boundary rather than leaving a rim of outline outside the
                 * curve. */
                if (ring > 0) {
                    *pixel = blend_over(*pixel, edge, (unsigned char)ring);
                }
                *pixel = blend_over(*pixel, behind, coverage);
            }
        }
    }
}

void recon_round_rect(struct recon_panel *panel, int x, int y, int w, int h,
        int radius, recon_color behind) {
    round_corners(panel, x, y, w, h, radius, behind, 0, false);
}

/*
 * How much of one corner pixel lies inside a circle of this radius.
 *
 * The same sampling round_corners does, pulled out so a fill and a stroke can
 * ask the same question and get answers that agree to the sample. Two curves
 * computed separately are two curves that disagree by a pixel somewhere, and
 * the somewhere is always a corner.
 */
static int corner_coverage(int radius, int dx, int dy, int shrink) {
    const int SAMPLES = 4;
    const int UNIT = SAMPLES * 2;

    long long reach = (long long)radius * UNIT - (long long)shrink * UNIT;
    if (reach < 0) {
        reach = 0;
    }

    int inside = 0;
    for (int sy = 0; sy < SAMPLES; sy++) {
        for (int sx = 0; sx < SAMPLES; sx++) {
            long long ox = (long long)(radius - dx) * UNIT - (2 * sx + 1);
            long long oy = (long long)(radius - dy) * UNIT - (2 * sy + 1);
            if (ox * ox + oy * oy <= reach * reach) {
                inside++;
            }
        }
    }
    return (inside * 255) / (SAMPLES * SAMPLES);
}

/* Where the four corner boxes are, for a given offset into one of them. */
static void corner_points(int x, int y, int w, int h, int dx, int dy,
        int out[4][2]) {
    out[0][0] = x + dx;             out[0][1] = y + dy;
    out[1][0] = x + w - 1 - dx;     out[1][1] = y + dy;
    out[2][0] = x + dx;             out[2][1] = y + h - 1 - dy;
    out[3][0] = x + w - 1 - dx;     out[3][1] = y + h - 1 - dy;
}

/*
 * Composite an opaque colour over a pixel that may not be opaque itself.
 *
 * blend_over keeps the destination's alpha, which is right where it is used --
 * carving a corner replaces one opaque colour with another. It is wrong here,
 * and invisibly so: the desktop's panel is *transparent* wherever the
 * wallpaper shows through, so an anti-aliased edge blended onto it came out at
 * alpha zero and simply did not exist. The straight parts of the shape were
 * drawn with recon_fill_rect, which sets the pixel outright, so three
 * rectangles appeared and the four curved corners did not -- a selection box
 * with square bites out of it, which is what it had before rounding and looked
 * like the rounding had not been applied at all.
 *
 * So: source-over, alpha included. The colour arriving is opaque and `amount`
 * is how much of the pixel it covers, which makes `amount` the source alpha.
 */
static void blend_at(struct recon_panel *panel, int cx, int cy,
        recon_color color, int coverage) {
    if (coverage <= 0 || cx < 0 || cy < 0 ||
            cx >= panel->width || cy >= panel->height) {
        return;
    }
    if (coverage > 255) {
        coverage = 255;
    }

    uint32_t *pixel = panel->pixels + (size_t)cy * panel->width + cx;
    unsigned base_a = (*pixel >> 24) & 0xFFu;

    /*
     * The colour's own alpha multiplies the coverage.
     *
     * Several things drawn through here are translucent by design -- the
     * desktop's selection box is, so that the wallpaper reads through it --
     * and a corner composited at full strength came out as the *undimmed*
     * colour beside a body that was dimmed: a bright rim around a darker
     * shape. Coverage says how much of the pixel the shape covers and the
     * colour says how solid the shape is, and the pixel wants both.
     */
    unsigned col_a = (color >> 24) & 0xFFu;
    unsigned src_a = col_a * (unsigned)coverage / 255u;

    /* Opaque colour on an opaque surface is the common case and needs none of
     * the arithmetic below -- the colours mix and the alpha does not move. */
    if (base_a == 0xFFu && col_a == 0xFFu) {
        *pixel = blend_over(*pixel, color, coverage);
        return;
    }
    unsigned kept = base_a * (255u - src_a) / 255u;
    unsigned out_a = src_a + kept;
    if (out_a == 0) {
        *pixel = 0;
        return;
    }

    uint32_t out = out_a << 24;
    for (int shift = 0; shift <= 16; shift += 8) {
        unsigned over = (color >> shift) & 0xFFu;
        unsigned base = (*pixel >> shift) & 0xFFu;
        out |= ((over * src_a + base * kept) / out_a) << shift;
    }
    *pixel = out;
}

static int clamp_radius(int w, int h, int radius) {
    int most = (w < h ? w : h) / 2;
    if (radius > most) {
        radius = most;
    }
    return radius < 0 ? 0 : radius;
}

/*
 * A filled rounded rectangle, composited over what is already there.
 *
 * This is the shape drawn as a shape. What it replaces is: fill a square
 * rectangle, then paint the corners back out with a colour the caller
 * *believes* is behind them -- which is a guess, and every place the guess is
 * wrong shows as a wedge of the wrong colour in the corner. It is wrong
 * whenever the surface behind is a gradient rather than a flat fill, whenever
 * it is a wallpaper, and whenever the caller passes the colour of the panel it
 * is on rather than the colour of the thing it happens to be sitting on.
 *
 * Nothing has to be guessed here. The corner pixels are never overwritten, so
 * whatever was behind them stays behind them, at whatever coverage the curve
 * gives -- a gradient, a photograph or a flat fill, without this knowing which.
 */
void recon_fill_round_rect(struct recon_panel *panel, int x, int y, int w,
        int h, int radius, recon_color color) {
    if (panel == NULL || w <= 0 || h <= 0) {
        return;
    }
    radius = clamp_radius(w, h, radius);
    if (radius <= 0) {
        recon_fill_rect(panel, x, y, w, h, color);
        return;
    }

    /* Everything that is not a corner, as three plain rectangles. */
    recon_fill_rect(panel, x, y + radius, w, h - radius * 2, color);
    recon_fill_rect(panel, x + radius, y, w - radius * 2, radius, color);
    recon_fill_rect(panel, x + radius, y + h - radius, w - radius * 2, radius,
        color);

    for (int dy = 0; dy < radius; dy++) {
        for (int dx = 0; dx < radius; dx++) {
            int coverage = corner_coverage(radius, dx, dy, 0);
            if (coverage <= 0) {
                continue;
            }
            int points[4][2];
            corner_points(x, y, w, h, dx, dy, points);
            for (int i = 0; i < 4; i++) {
                blend_at(panel, points[i][0], points[i][1], color, coverage);
            }
        }
    }
}

/*
 * A filled rounded rectangle with its own outline, in one pass.
 *
 * Not a fill followed by a stroke. Those are two blends, and at a corner the
 * second lands on a pixel the first has already part-covered -- so the outline
 * comes out mixed with the *face* where it should be mixed with whatever is
 * behind the control. On a light button over a dark title bar that shows as a
 * pale wedge at each corner, which is precisely what it looked like: eight
 * light pixels per button, one small triangle in each corner.
 *
 * A corner pixel is three things at once and has to be worked out as three:
 * what is behind, outside the shape entirely; the outline, in the ring between
 * the curve and the curve one pixel in; and the face, inside that. Doing it in
 * one pass means the three shares add to exactly one pixel, and the outline
 * keeps its own colour at whatever share of the pixel it actually occupies.
 */
void recon_fill_round_rect_edged(struct recon_panel *panel, int x, int y,
        int w, int h, int radius, recon_color face, recon_color edge) {
    if (panel == NULL || w < 2 || h < 2) {
        return;
    }
    radius = clamp_radius(w, h, radius);
    if (radius <= 0) {
        recon_fill_rect(panel, x, y, w, h, face);
        recon_stroke_rect(panel, x, y, w, h, edge);
        return;
    }

    /* The straight runs: the face, then the outline along each edge. */
    recon_fill_rect(panel, x, y + radius, w, h - radius * 2, face);
    recon_fill_rect(panel, x + radius, y, w - radius * 2, radius, face);
    recon_fill_rect(panel, x + radius, y + h - radius, w - radius * 2, radius,
        face);

    int span_w = w - radius * 2;
    int span_h = h - radius * 2;
    if (span_w > 0) {
        recon_fill_rect(panel, x + radius, y, span_w, 1, edge);
        recon_fill_rect(panel, x + radius, y + h - 1, span_w, 1, edge);
    }
    if (span_h > 0) {
        recon_fill_rect(panel, x, y + radius, 1, span_h, edge);
        recon_fill_rect(panel, x + w - 1, y + radius, 1, span_h, edge);
    }

    for (int dy = 0; dy < radius; dy++) {
        for (int dx = 0; dx < radius; dx++) {
            int outer = corner_coverage(radius, dx, dy, 0);
            if (outer <= 0) {
                continue;               /* wholly outside; leave it alone */
            }
            int inner = corner_coverage(radius, dx, dy, 1);

            int points[4][2];
            corner_points(x, y, w, h, dx, dy, points);
            for (int i = 0; i < 4; i++) {
                /*
                 * The face first at its own share, then the outline over it
                 * at the ring's share of what is left. Two blends, but the
                 * second is scaled against the first rather than laid on top
                 * of it, so the outline is never diluted by the face and what
                 * is behind keeps exactly the share the curve leaves it.
                 */
                if (inner > 0) {
                    blend_at(panel, points[i][0], points[i][1], face, inner);
                }
                int ring = outer - inner;
                if (ring > 0) {
                    int share = inner < 255
                        ? (ring * 255) / (255 - inner) : 255;
                    if (share > 255) {
                        share = 255;
                    }
                    blend_at(panel, points[i][0], points[i][1], edge, share);
                }
            }
        }
    }
}

/*
 * A one-pixel outline around the same shape, composited the same way.
 *
 * The ring is the difference between the curve and the curve one pixel in, so
 * it is exactly as anti-aliased as the fill it edges and cannot drift away
 * from it by a pixel at any point around the corner.
 */
void recon_stroke_round_rect(struct recon_panel *panel, int x, int y, int w,
        int h, int radius, recon_color color) {
    if (panel == NULL || w < 2 || h < 2) {
        return;
    }
    radius = clamp_radius(w, h, radius);
    if (radius <= 0) {
        recon_stroke_rect(panel, x, y, w, h, color);
        return;
    }

    int span_w = w - radius * 2;
    int span_h = h - radius * 2;
    if (span_w > 0) {
        recon_fill_rect(panel, x + radius, y, span_w, 1, color);
        recon_fill_rect(panel, x + radius, y + h - 1, span_w, 1, color);
    }
    if (span_h > 0) {
        recon_fill_rect(panel, x, y + radius, 1, span_h, color);
        recon_fill_rect(panel, x + w - 1, y + radius, 1, span_h, color);
    }

    for (int dy = 0; dy < radius; dy++) {
        for (int dx = 0; dx < radius; dx++) {
            int ring = corner_coverage(radius, dx, dy, 0)
                - corner_coverage(radius, dx, dy, 1);
            if (ring <= 0) {
                continue;
            }
            int points[4][2];
            corner_points(x, y, w, h, dx, dy, points);
            for (int i = 0; i < 4; i++) {
                blend_at(panel, points[i][0], points[i][1], color, ring);
            }
        }
    }
}

/*
 * Round the corners and lay a one-pixel outline along the curve, in one pass.
 *
 * The outline has to be *composited* rather than painted, because a button is
 * not always drawn before its contents. The taskbar fills its buttons, draws
 * the window's icon and title into them, and only then asks for the edge --
 * for a good reason of its own, which is that a put-away window's contents are
 * washed afterwards and the edge has to survive that intact. An edge that
 * repainted the inside of the button to get a clean surface to carve would
 * erase the icon and the title, and did: two blank rounded rectangles on the
 * taskbar where two windows should have been.
 *
 * So nothing here fills. The straight runs are one-pixel lines and the corners
 * blend the outline over whatever is already there, at exactly the coverage
 * the curve gives them.
 */
void recon_round_rect_outline(struct recon_panel *panel, int x, int y, int w,
        int h, int radius, recon_color behind, recon_color edge) {
    if (panel == NULL || w < 2 || h < 2) {
        return;
    }
    if (radius <= 0) {
        recon_stroke_rect(panel, x, y, w, h, edge);
        return;
    }
    if (radius * 2 > w || radius * 2 > h) {
        radius = (w < h ? w : h) / 2;
    }

    /* The straight runs, between one corner and the next. */
    int span_w = w - radius * 2;
    int span_h = h - radius * 2;
    if (span_w > 0) {
        recon_fill_rect(panel, x + radius, y, span_w, 1, edge);
        recon_fill_rect(panel, x + radius, y + h - 1, span_w, 1, edge);
    }
    if (span_h > 0) {
        recon_fill_rect(panel, x, y + radius, 1, span_h, edge);
        recon_fill_rect(panel, x + w - 1, y + radius, 1, span_h, edge);
    }

    round_corners(panel, x, y, w, h, radius, behind, edge, true);
}

void recon_round_top_corners(struct recon_panel *panel, int radius,
        recon_color edge) {
    (void)edge;
    if (panel == NULL || radius <= 0) {
        return;
    }

    int width = recon_panel_width(panel);
    int height = recon_panel_height(panel);
    if (radius * 2 > width || radius > height) {
        return;    /* Rounder than the window is wide; leave it square. */
    }

    /*
     * How much of each pixel falls inside the curve, by sampling it in a grid.
     *
     * The first version simply cleared whole pixels, which gave a staircase --
     * and a staircase is worse than a square corner, because a rounded corner
     * is the one shape people read as smooth, so the steps look like a fault
     * in the drawing rather than a decision.
     *
     * Four by four is enough: seventeen levels of coverage across two or three
     * pixels of boundary, at a size nobody is looking at closely.
     */
    const int SAMPLES = 4;

    /*
     * Sampled in integers, not in doubles.
     *
     * `(sx + 0.5) / SAMPLES` is a fraction, and multiplying the whole
     * comparison through by `2 * SAMPLES` clears it exactly: twice
     * `sx + 0.5` is `2 * sx + 1`, with nothing left over. So this is not a
     * fixed-point approximation of what was here before -- it is the same
     * test with the floating-point rounding taken out.
     *
     * Integers because this file is headed for `freestanding/`, where there
     * is no floating point at all: the kernel builds with
     * `-mgeneral-regs-only` on ARM, where a `double` is a compile error, and
     * with the FPU off on x86, where it is silent corruption instead.
     *
     * The squares are 64-bit. At a radius of 2048 the sum still fits in 32,
     * but "still fits" is a fact about today's window sizes rather than about
     * this code, and it costs nothing here to stop it being a question.
     */
    const int UNIT = SAMPLES * 2;
    const long long REACH = (long long)radius * UNIT;

    for (int y = 0; y < radius; y++) {
        uint32_t *row = panel->pixels + (size_t)y * panel->width;

        for (int x = 0; x < radius; x++) {
            int inside = 0;

            for (int sy = 0; sy < SAMPLES; sy++) {
                for (int sx = 0; sx < SAMPLES; sx++) {
                    /* The corner's circle is centred at (radius, radius), so
                     * a point is in the frame when it is no further from that
                     * centre than the radius. */
                    long long dx = (long long)(radius - x) * UNIT
                        - (2 * sx + 1);
                    long long dy = (long long)(radius - y) * UNIT
                        - (2 * sy + 1);
                    if (dx * dx + dy * dy <= REACH * REACH) {
                        inside++;
                    }
                }
            }

            if (inside == SAMPLES * SAMPLES) {
                continue;    /* Fully inside; leave the pixel alone. */
            }

            /*
             * Scaled rather than replaced. The pixel already holds whatever
             * the frame drew there, and thinning its alpha is what makes the
             * boundary fade into the wallpaper instead of into a colour
             * chosen here -- which would be the wrong colour over any other
             * window.
             */
            int coverage = (inside * 255) / (SAMPLES * SAMPLES);

            uint32_t *left = &row[x];
            uint32_t *right = &row[width - 1 - x];

            unsigned alpha_l = ((*left >> 24) & 0xFF) * (unsigned)coverage / 255;
            unsigned alpha_r = ((*right >> 24) & 0xFF) * (unsigned)coverage / 255;

            *left = (*left & 0x00FFFFFFu) | (alpha_l << 24);
            *right = (*right & 0x00FFFFFFu) | (alpha_r << 24);
        }
    }
}

void recon_fill_gradient(struct recon_panel *panel, int x, int y, int w, int h,
        recon_color from, recon_color to) {
    if (panel == NULL) {
        return;
    }
    if (from == to) {
        recon_fill_rect(panel, x, y, w, h, from);
        return;
    }

    /*
     * The ramp is positioned against the rectangle that was asked for, not
     * against what survives clipping. A title bar hanging off the top of a
     * panel has to show the *bottom* of its gradient, and clip_rect moves y
     * and shortens h, so a row's place in the ramp has to be worked out from
     * the original before that happens.
     */
    int want_y = y;
    int want_h = h;
    if (!clip_rect(panel, &x, &y, &w, &h)) {
        return;
    }

    int last = want_h > 1 ? want_h - 1 : 1;
    for (int row = y; row < y + h; row++) {
        int step = row - want_y;
        recon_color color = 0xFF000000u |
            (ramp(from, to, 16, step, last) << 16) |
            (ramp(from, to, 8, step, last) << 8) |
            ramp(from, to, 0, step, last);

        uint32_t *p = panel->pixels + (size_t)row * panel->width + x;
        for (int col = 0; col < w; col++) {
            p[col] = color;
        }
    }
}

void recon_stroke_rect(struct recon_panel *panel, int x, int y, int w, int h,
        recon_color color) {
    if (panel == NULL || w <= 0 || h <= 0) {
        return;
    }
    recon_fill_rect(panel, x, y, w, 1, color);
    recon_fill_rect(panel, x, y + h - 1, w, 1, color);
    recon_fill_rect(panel, x, y, 1, h, color);
    recon_fill_rect(panel, x + w - 1, y, 1, h, color);
}

/*
 * The colour a bevel is made of: the surface's own, lit and shaded.
 *
 * It used to be a fixed EEEEEE and 555555, under a comment saying a skin would
 * supply them one day. Nothing ever did, so every button in the system was
 * edged in the same two greys whatever it was painted in -- and on a skin
 * whose buttons are lavender and whose panels are lavender, a grey line
 * between the two is the one thing on screen that belongs to neither. "They're
 * not even colour tone like they're supposed to" is exactly that.
 *
 * Nothing needs to supply them. A bevel is not a colour, it is *this surface
 * with light on it* and *this surface in shadow*, so it can be derived from
 * the surface and is then right on every skin including one made this
 * afternoon -- the same reasoning that decides hover in recon_widget.
 *
 * The shadow is moved further than the highlight. Most surfaces in a desktop
 * are light, and on a light surface there is far more room below than above:
 * lightening E8EBF5 by any amount lands somewhere indistinguishable from the
 * panel behind it, while darkening it produces a real edge. On a dark skin the
 * two swap roles by the same arithmetic, because both are proportions of what
 * they started from rather than fixed distances.
 */
static void bevel_colours(recon_color face, recon_color *light,
        recon_color *dark) {
    *light = recon_color_highlight(face);
    *dark = recon_color_mix(face, RECON_RGB(0x00, 0x00, 0x00), 120);
}

/* The pixel just outside a rectangle: what the control is sitting on. */
static recon_color colour_behind(const struct recon_panel *panel, int x, int y,
        int w, int h) {
    if (panel == NULL) {
        return RECON_RGB(0xC0, 0xC0, 0xC0);
    }
    const int TRY[][2] = {
        { x + w / 2, y - 1 },        /* above */
        { x - 1,     y + h / 2 },    /* left */
        { x + w / 2, y + h },        /* below */
        { x + w,     y + h / 2 },    /* right */
    };
    for (size_t i = 0; i < sizeof(TRY) / sizeof(TRY[0]); i++) {
        int sx = TRY[i][0], sy = TRY[i][1];
        if (sx >= 0 && sy >= 0 && sx < panel->width && sy < panel->height) {
            return panel->pixels[(size_t)sy * panel->width + sx];
        }
    }
    return RECON_RGB(0xC0, 0xC0, 0xC0);
}

/*
 * --- What an outline is for, and what follows from that ---
 *
 * An outline exists to separate a control from what is behind it. So how
 * strong it needs to be is not a property of the control: it is how far apart
 * the two already are.
 *
 * It used to be one thing -- the face mixed toward black -- and that is right
 * exactly when the two are close. On Glass the Calculator's keys are E8EBF5
 * on a panel of F0F2F8, eight levels apart, and without the outline the
 * button has no edge at all (BG-141). On Beacon the taskbar's buttons are
 * E0E6F2 on a bar of 2959C4 -- a hundred and forty-four levels apart, already
 * unmistakably separate -- and the same rule put a 777A81 ring round each
 * one. A neutral grey against a saturated blue does not read as an edge. It
 * reads as dirt, and it is what "that weird black line on the outer ring"
 * is a picture of.
 *
 * So: when face and background are close, the outline is the shaded tone and
 * does the whole job. As they separate, it slides toward a shadow of the
 * background -- present, the right hue, and no longer pretending to be an
 * edge the button does not need.
 */
static recon_color outline_colour(recon_color face, recon_color behind) {
    recon_color dark = recon_color_mix(face, RECON_RGB(0x00, 0x00, 0x00), 120);

    int gap = recon_color_luminance(face) - recon_color_luminance(behind);
    if (gap < 0) {
        gap = -gap;
    }

    /*
     * Fully the background's shadow once they are 128 apart -- half the range,
     * which is well beyond the point where a reader could mistake one for the
     * other.
     */
    int toward = gap * 2;
    if (toward > 255) {
        toward = 255;
    }

    recon_color shadow = recon_color_mix(behind, RECON_RGB(0x00, 0x00, 0x00),
        40);
    return recon_color_mix(dark, shadow, (uint8_t)toward);
}

/*
 * What colour the thing being bevelled is, read from the panel.
 *
 * Sampled rather than passed in. A bevel is drawn immediately after the fill
 * it belongs to in every one of the forty-odd places that draw one, so the
 * pixel just inside the corner *is* the surface -- and passing it would mean
 * changing forty call sites to say something all forty already knew.
 *
 * The corner rather than the middle, because the middle of a button is where
 * its label is. Falls back to the mid grey the fixed version used, for a
 * rectangle at the edge of the panel or one with no interior to read.
 */
static recon_color bevel_face(const struct recon_panel *panel, int x, int y,
        int w, int h) {
    if (panel == NULL || w < 3 || h < 3) {
        return RECON_RGB(0xC0, 0xC0, 0xC0);
    }
    int sx = x + 1;
    int sy = y + 1;
    if (sx < 0 || sy < 0 || sx >= panel->width || sy >= panel->height) {
        return RECON_RGB(0xC0, 0xC0, 0xC0);
    }
    return panel->pixels[(size_t)sy * panel->width + sx];
}

/*
 * The 95 bevel, in the surface's own colours rather than in grey.
 *
 * Light on the top and left, dark on the bottom and right. Right for a sunken
 * text field or a panel's own outline, which is what still calls this: those
 * are square, so both halves stay where they were drawn, and the two tones
 * against each other are the whole of the effect.
 *
 * It is *not* what a button wants -- see recon_draw_button_edge, which needs a
 * boundary as well as a lighting effect, and needs it to survive being
 * rounded.
 */
void recon_draw_bevel(struct recon_panel *panel, int x, int y, int w, int h,
        bool pressed) {
    recon_color light, dark;
    bevel_colours(bevel_face(panel, x, y, w, h), &light, &dark);

    recon_color top_left = pressed ? dark : light;
    recon_color bottom_right = pressed ? light : dark;

    recon_fill_rect(panel, x, y, w, 1, top_left);
    recon_fill_rect(panel, x, y, 1, h, top_left);
    recon_fill_rect(panel, x, y + h - 1, w, 1, bottom_right);
    recon_fill_rect(panel, x + w - 1, y, 1, h, bottom_right);
}

/*
 * The edge treatment every button in the system shares.
 *
 * The bevel is drawn first and the corners are then rounded off it, rather
 * than one or the other: the bevel is what says pressed from raised, and a
 * skin asking for round corners is not asking to give that up. Rounding
 * after leaves the straight edges carrying the light and the shadow, which is
 * where they were being read anyway.
 *
 * This exists because the rounding was written three times -- once in the
 * taskbar, once in the window frame, once in the notice after an update --
 * and every button that had not been rewritten stayed square. One skin
 * producing two shapes of button inside one window is not a theme, it is a
 * list of the files somebody remembered to change.
 */
/*
 * The radius a button of this size actually gets.
 *
 * The metric is one number and buttons are not one size. A close button is 16
 * pixels square and a toolbar button is 28 by 20; a radius that curves the
 * second nicely turns the first into a circle, and a radius that suits the
 * first is two pixels of diagonal on the second -- which is not a curve at
 * all. Two pixels of gradation is a *chamfer*, and a chamfer is exactly what
 * "the edges look like they have been cut off" describes. It was measured at
 * two before this existed.
 *
 * So the metric is what a skin asks for and this is what it can have: never
 * more than three tenths of the shorter side. Three tenths because a quarter
 * still reads as square on anything small and a third starts reading as a
 * lozenge on anything wide.
 *
 * The alternative -- a second metric for small buttons -- is two numbers that
 * have to be kept in a relationship, which is a relationship somebody will
 * eventually get wrong on a skin they are making at the time.
 */
/*
 * The cap lives here and only here.
 *
 * It was written twice -- once for the current skin and once in the Appearance
 * page, which draws a sample button for every *other* skin. Two copies of one
 * rule is one rule and one bug waiting: the copy is right until the original
 * changes.
 */
static int radius_capped(int radius, int w, int h) {
    if (radius <= 0) {
        return 0;
    }
    int shortest = w < h ? w : h;
    int most = shortest * 3 / 10;
    return radius < most ? radius : most;
}

int recon_button_radius(int w, int h) {
    return radius_capped(recon_theme_metric(RECON_METRIC_BUTTON_CORNER), w, h);
}

int recon_button_radius_of(int skin, int w, int h) {
    return radius_capped(recon_theme_metric_of(skin, RECON_METRIC_BUTTON_CORNER),
        w, h);
}

/*
 * Fill a button and edge it, as one shape.
 *
 * `behind` is gone from this path. It was the colour a caller believed was
 * underneath the corners, passed so the corners could be painted back out
 * after a square fill -- a guess, and a wedge of the wrong colour wherever the
 * guess was wrong. Over a gradient it was always wrong; over the wallpaper it
 * was always wrong; and a caller that passed its own panel's colour rather
 * than the colour of the strip the button actually sat on was wrong in a way
 * nobody could see until a skin changed one of the two.
 *
 * Drawing the shape asks nobody anything. The corner pixels are never
 * overwritten, so whatever is behind them stays there at whatever coverage the
 * curve gives.
 */
void recon_corners_keep(struct recon_panel *panel, int x, int y, int w, int h,
        int radius, struct recon_corners *out) {
    if (out == NULL) {
        return;
    }
    out->held = false;
    out->w = w;
    out->h = h;

    /* Never rounder than the shape, the same clamp the drawing uses, so a
     * caller cannot keep one curve and have another put back. */
    int most = (w < h ? w : h) / 2;
    out->radius = radius > most ? most : radius;

    if (panel == NULL || out->radius <= 0 ||
            out->radius > RECON_CORNER_MAX) {
        return;
    }

    int r = out->radius;
    for (int dy = 0; dy < r; dy++) {
        for (int dx = 0; dx < r; dx++) {
            int points[4][2];
            corner_points(x, y, w, h, dx, dy, points);
            for (int i = 0; i < 4; i++) {
                int cx = points[i][0];
                int cy = points[i][1];
                uint32_t value = 0;
                if (cx >= 0 && cy >= 0 && cx < panel->width &&
                        cy < panel->height) {
                    value = panel->pixels[(size_t)cy * panel->width + cx];
                }
                out->pixels[i][dy * r + dx] = value;
            }
        }
    }
    out->held = true;
}

void recon_corners_restore(struct recon_panel *panel, int x, int y,
        const struct recon_corners *keep) {
    if (panel == NULL || keep == NULL || !keep->held) {
        return;
    }

    int r = keep->radius;
    for (int dy = 0; dy < r; dy++) {
        for (int dx = 0; dx < r; dx++) {
            /*
             * How much of this pixel the rounded shape does *not* cover, which
             * is how much of what was here should come back. The same
             * coverage the shape is drawn with, so the two meet exactly rather
             * than leaving a seam a pixel wide.
             */
            int outside = 255 - corner_coverage(r, dx, dy, 0);
            if (outside <= 0) {
                continue;
            }

            int points[4][2];
            corner_points(x, y, keep->w, keep->h, dx, dy, points);
            for (int i = 0; i < 4; i++) {
                blend_at(panel, points[i][0], points[i][1],
                    keep->pixels[i][dy * r + dx] | 0xFF000000u, outside);
            }
        }
    }
}

/*
 * The edge alone: the outline and the highlight, filling nothing.
 *
 * Split out of recon_fill_button for the caller that has to do its own filling
 * and drawing first. Everything about how a button's edge looks lives in one
 * place either way -- this and recon_fill_button share it rather than each
 * having an opinion.
 */
void recon_edge_button(struct recon_panel *panel, int x, int y, int w, int h,
        bool pressed) {
    if (panel == NULL || w < 2 || h < 2) {
        return;
    }

    recon_color face = bevel_face(panel, x, y, w, h);
    recon_color light, dark;
    bevel_colours(face, &light, &dark);
    recon_color edge = outline_colour(face, colour_behind(panel, x, y, w, h));
    int radius = recon_button_radius(w, h);

    recon_stroke_round_rect(panel, x, y, w, h, radius, edge);

    if (w > radius * 2 + 4 && h > radius * 2 + 4) {
        recon_color inner = pressed ? dark : light;
        recon_fill_rect(panel, x + 1 + radius, y + 1,
            w - 2 - radius * 2, 1, inner);
        recon_fill_rect(panel, x + 1, y + 1 + radius,
            1, h - 2 - radius * 2, inner);
    }
}

void recon_fill_button(struct recon_panel *panel, int x, int y, int w, int h,
        bool pressed, recon_color face) {
    recon_fill_button_radius(panel, x, y, w, h, pressed, face,
        recon_button_radius(w, h));
}

void recon_fill_button_radius(struct recon_panel *panel, int x, int y, int w,
        int h, bool pressed, recon_color face, int radius) {
    if (panel == NULL || w < 2 || h < 2) {
        return;
    }

    recon_color light, dark;
    bevel_colours(face, &light, &dark);


    /*
     * --- The boundary ---
     *
     * A one-pixel outline in the shaded tone, all four sides, following the
     * corner.
     *
     * It used to be a 95 bevel and nothing else: light on the top and left,
     * dark on the bottom and right. That is a *lighting effect*, and it reads
     * as an edge only while the button sits between its own highlight and the
     * surface behind it -- true for as long as every skin was grey. On Glass
     * the Calculator's keys are E8EBF5 and the panel they sit on is F0F2F8,
     * eight levels apart, so the lit half landed lighter than the background
     * and the top and left of every key stopped existing. Two edges out of
     * four, which is what "they look incomplete" is a picture of.
     */
    recon_fill_round_rect_edged(panel, x, y, w, h, radius, face,
        outline_colour(face, colour_behind(panel, x, y, w, h)));

    /*
     * --- The lighting, kept out of the corners ---
     *
     * Inset by one from the boundary and by the radius at each end, so it
     * begins where the curve finishes. A highlight running the full width
     * would reach round into the corner and sit outside the curve, putting a
     * pale pixel where the outline should be.
     */
    if (w > radius * 2 + 4 && h > radius * 2 + 4) {
        recon_color inner = pressed ? dark : light;
        recon_fill_rect(panel, x + 1 + radius, y + 1,
            w - 2 - radius * 2, 1, inner);
        recon_fill_rect(panel, x + 1, y + 1 + radius,
            1, h - 2 - radius * 2, inner);
    }
}

/*
 * The older shape of the same thing, for callers that have already filled.
 *
 * The taskbar is the one that has to: it fills a button, draws a window's icon
 * and title into it, washes the lot when the window is put away, and only then
 * asks for the edge -- so the fill cannot be moved into the edge routine
 * without the wash losing its subject. It samples the face rather than being
 * told, and `behind` is what its corners are carved back to, with the guess
 * that implies.
 */
void recon_draw_button_edge(struct recon_panel *panel, int x, int y, int w,
        int h, bool pressed, recon_color behind) {
    if (panel == NULL || w < 2 || h < 2) {
        return;
    }

    recon_color face = bevel_face(panel, x, y, w, h);
    recon_color light, dark;
    bevel_colours(face, &light, &dark);
    int radius = recon_button_radius(w, h);

    recon_round_rect_outline(panel, x, y, w, h, radius, behind, dark);

    if (w > radius * 2 + 4 && h > radius * 2 + 4) {
        recon_color inner = pressed ? dark : light;
        recon_fill_rect(panel, x + 1 + radius, y + 1,
            w - 2 - radius * 2, 1, inner);
        recon_fill_rect(panel, x + 1, y + 1 + radius,
            1, h - 2 - radius * 2, inner);
    }
}

/* Blend a coverage value of `color` over one pixel. */
static void blend_pixel(uint32_t *dst, recon_color color, unsigned char coverage) {
    if (coverage == 0) {
        return;
    }
    if (coverage == 255) {
        *dst = color;
        return;
    }

    uint32_t src_r = (color >> 16) & 0xFF;
    uint32_t src_g = (color >> 8) & 0xFF;
    uint32_t src_b = color & 0xFF;

    uint32_t dst_r = (*dst >> 16) & 0xFF;
    uint32_t dst_g = (*dst >> 8) & 0xFF;
    uint32_t dst_b = *dst & 0xFF;

    uint32_t a = coverage;
    uint32_t inv = 255 - a;

    uint32_t r = (src_r * a + dst_r * inv) / 255;
    uint32_t g = (src_g * a + dst_g * inv) / 255;
    uint32_t b = (src_b * a + dst_b * inv) / 255;

    *dst = 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void draw_glyph(struct recon_panel *panel, struct recon_glyph *glyph,
        int pen_x, int baseline_y, recon_color color) {
    if (glyph->bitmap == NULL) {
        return;
    }

    int origin_x = pen_x + glyph->bearing_x;
    int origin_y = baseline_y + glyph->bearing_y;

    for (int row = 0; row < glyph->height; row++) {
        int py = origin_y + row;
        if (py < 0 || py >= panel->height) {
            continue;
        }
        const unsigned char *src = glyph->bitmap + (size_t)row * glyph->width;
        uint32_t *dst_row = panel->pixels + (size_t)py * panel->width;

        for (int col = 0; col < glyph->width; col++) {
            int px = origin_x + col;
            if (px < 0 || px >= panel->width) {
                continue;
            }
            blend_pixel(&dst_row[px], color, src[col]);
        }
    }
}

/*
 * One line at a time, greedily: take words until the next one would not fit.
 *
 * Greedy rather than balanced. Balancing lines looks better in a book and
 * needs the whole paragraph measured before anything is drawn; this is for
 * sentences under a heading, where the cost of getting it wrong is a slightly
 * short last line.
 */
int recon_draw_paragraph(struct recon_panel *panel, struct recon_font *font,
        int x, int y, int max_width, const char *text, recon_color color) {
    if (panel == NULL || font == NULL || text == NULL || max_width <= 0) {
        return 0;
    }

    int ascent = recon_font_ascent(font);
    int line_height = recon_font_line_height(font);
    int drawn = 0;

    char line[512];
    size_t used = 0;

    const char *word = text;
    while (*word != '\0') {
        /* One word, and the run of spaces after it. */
        const char *end = word;
        while (*end != '\0' && *end != ' ' && *end != '\n') {
            end++;
        }
        size_t length = (size_t)(end - word);

        /* Longer than the buffer is longer than any line could hold, so it
         * goes out on its own and recon_draw_text clips it. Silently dropping
         * it would lose text; this loses only the tail of one word. */
        if (length >= sizeof(line)) {
            length = sizeof(line) - 1;
        }

        char candidate[512];
        size_t at = 0;
        if (used > 0) {
            memcpy(candidate, line, used);
            at = used;
            candidate[at++] = ' ';
        }
        memcpy(candidate + at, word, length);
        candidate[at + length] = '\0';

        if (used > 0 && recon_text_width(font, candidate) > max_width) {
            /* It did not fit, so the line so far goes out and this word
             * starts the next one. */
            recon_draw_text(panel, font, x, y + drawn + ascent, max_width,
                line, color);
            drawn += line_height;
            memcpy(line, word, length);
            line[length] = '\0';
            used = length;
        } else {
            memcpy(line, candidate, at + length + 1);
            used = at + length;
        }

        word = end;
        while (*word == ' ' || *word == '\n') {
            word++;
        }
    }

    if (used > 0) {
        recon_draw_text(panel, font, x, y + drawn + ascent, max_width, line,
            color);
        drawn += line_height;
    }
    return drawn;
}

void recon_draw_text(struct recon_panel *panel, struct recon_font *font,
        int x, int y, int max_width, const char *text, recon_color color) {
    if (panel == NULL || font == NULL || text == NULL) {
        return;
    }

    /* If it doesn't fit, reserve room for an ellipsis and stop early. */
    bool truncating = false;
    int limit = max_width;
    if (max_width > 0 && recon_text_width(font, text) > max_width) {
        int ellipsis = recon_text_width(font, "...");
        limit = max_width - ellipsis;
        truncating = true;
        if (limit < 0) {
            limit = 0;
        }
    }

    int pen = x;
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ) {
        int length = 1;
        uint32_t c = next_codepoint(p, &length);
        p += length;

        struct recon_glyph *glyph = glyph_for(font, c);
        if (glyph == NULL) {
            continue;
        }
        if (max_width > 0 &&
                (pen - x) + glyph->advance + g_letter_spacing > limit) {
            break;
        }
        draw_glyph(panel, glyph, pen, y, color);
        pen += glyph->advance + g_letter_spacing;
    }

    if (truncating) {
        for (const char *e = "..."; *e != '\0'; e++) {
            struct recon_glyph *glyph = glyph_for(font, (uint32_t)*e);
            if (glyph == NULL) {
                continue;
            }
            draw_glyph(panel, glyph, pen, y, color);
            pen += glyph->advance + g_letter_spacing;
        }
    }
}

void recon_draw_image_clipped(struct recon_panel *panel, int x, int y,
        int w, int h, const unsigned char *rgba,
        int image_width, int image_height, int top, int bottom) {
    if (panel == NULL || rgba == NULL || w <= 0 || h <= 0 ||
            image_width <= 0 || image_height <= 0) {
        return;
    }

    /*
     * The band is a second clip inside the panel's own, for a caller drawing
     * into part of a panel rather than all of it.
     *
     * Text needed no such thing: a line is twenty-four pixels tall, so a
     * viewport that stops drawing lines whose top is past the bottom is
     * wrong by less than one line and nobody sees it. An image is three
     * hundred, and one whose top is ten pixels above the last visible row
     * drew the other two hundred and ninety straight over the status bar --
     * which is what the web viewer did, and what this exists for.
     */
    if (top < 0) {
        top = 0;
    }
    if (bottom > panel->height - 1) {
        bottom = panel->height - 1;
    }

    /*
     * Shrinking averages the source pixels that fall inside each destination
     * pixel; growing takes the nearest one.
     *
     * Taking the nearest when shrinking throws away most of the image and
     * keeps an arbitrary sample of what is left, which is why a 32-pixel icon
     * drawn at 22 looked speckled: whole features landed between samples and
     * simply vanished. Averaging is what makes a small icon look like a small
     * icon rather than a damaged one. Growing is left alone -- these are
     * pixel art, and blurring them upward would be worse than the steps.
     */
    bool shrinking = (image_width > w || image_height > h);

    for (int row = 0; row < h; row++) {
        int py = y + row;
        if (py < top || py > bottom) {
            continue;
        }

        int sy0 = row * image_height / h;
        int sy1 = shrinking ? (row + 1) * image_height / h : sy0 + 1;
        if (sy1 <= sy0) {
            sy1 = sy0 + 1;
        }

        uint32_t *dst_row = panel->pixels + (size_t)py * panel->width;

        for (int col = 0; col < w; col++) {
            int px = x + col;
            if (px < 0 || px >= panel->width) {
                continue;
            }

            int sx0 = col * image_width / w;
            int sx1 = shrinking ? (col + 1) * image_width / w : sx0 + 1;
            if (sx1 <= sx0) {
                sx1 = sx0 + 1;
            }

            /*
             * Colour weighted by alpha, so a transparent pixel contributes
             * nothing to the colour rather than dragging it towards whatever
             * happens to be stored in an invisible pixel -- which is usually
             * black, and is what puts a dark fringe around a scaled icon.
             */
            unsigned red = 0, green = 0, blue = 0, alpha = 0, count = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const unsigned char *src_row =
                    rgba + (size_t)sy * image_width * 4;
                for (int sx = sx0; sx < sx1; sx++) {
                    const unsigned char *src = src_row + (size_t)sx * 4;
                    unsigned a = src[3];
                    red += src[0] * a;
                    green += src[1] * a;
                    blue += src[2] * a;
                    alpha += a;
                    count++;
                }
            }

            if (count == 0 || alpha == 0) {
                continue;
            }

            /* blend_pixel takes one colour and a coverage, which is exactly
             * what a pixel and its alpha are. */
            blend_pixel(&dst_row[px],
                RECON_RGB(red / alpha, green / alpha, blue / alpha),
                (unsigned char)(alpha / count));
        }
    }
}

void recon_draw_image(struct recon_panel *panel, int x, int y, int w, int h,
        const unsigned char *rgba, int image_width, int image_height) {
    if (panel == NULL) {
        return;
    }
    recon_draw_image_clipped(panel, x, y, w, h, rgba,
        image_width, image_height, 0, panel->height - 1);
}

/* --- Click targets --- */

void recon_hit_clear(struct recon_panel *panel) {
    if (panel != NULL) {
        panel->hit_count = 0;
    }
}

bool recon_hit_add(struct recon_panel *panel, int x, int y, int w, int h,
        uint32_t id) {
    if (panel == NULL || panel->hit_count >= MAX_HIT_REGIONS) {
        return false;
    }
    panel->hits[panel->hit_count++] = (struct recon_hit_region){
        .x = x, .y = y, .w = w, .h = h, .id = id,
    };
    return true;
}

bool recon_hit_tip(struct recon_panel *panel, const char *text) {
    if (panel == NULL || panel->hit_count == 0 || text == NULL) {
        return false;
    }
    struct recon_hit_region *region = &panel->hits[panel->hit_count - 1];
    snprintf(region->tip, sizeof(region->tip), "%s", text);
    return true;
}

bool recon_hit_inert(struct recon_panel *panel) {
    if (panel == NULL || panel->hit_count == 0) {
        return false;
    }
    panel->hits[panel->hit_count - 1].inert = true;
    return true;
}

bool recon_hit_tip_at(struct recon_panel *panel, int x, int y,
        char *out, size_t size) {
    if (out == NULL || size == 0) {
        return false;
    }
    out[0] = '\0';
    if (panel == NULL) {
        return false;
    }

    /* Last added wins, the same rule the click uses. A tooltip that named a
     * region the click would not reach would be describing the wrong thing. */
    for (size_t i = panel->hit_count; i > 0; i--) {
        const struct recon_hit_region *r = &panel->hits[i - 1];
        if (x < r->x || x >= r->x + r->w || y < r->y || y >= r->y + r->h) {
            continue;
        }
        if (r->tip[0] == '\0') {
            return false;
        }
        snprintf(out, size, "%s", r->tip);
        return true;
    }
    return false;
}

bool recon_panel_tip_at(struct recon_panel *panel, double lx, double ly,
        char *out, size_t size) {
    if (out != NULL && size > 0) {
        out[0] = '\0';
    }
    if (panel == NULL) {
        return false;
    }

    int px = 0, py = 0;
    recon_panel_position(panel, &px, &py);

    int w = recon_panel_width(panel);
    int h = recon_panel_height(panel);

    int x = (int)lx - px;
    int y = (int)ly - py;
    if (x < 0 || y < 0 || x >= w || y >= h) {
        return false;
    }

    return recon_hit_tip_at(panel, x, y, out, size);
}

bool recon_panel_contains(const struct recon_panel *panel, double lx,
        double ly) {
    if (panel == NULL) {
        return false;
    }

    int px = 0, py = 0;
    recon_panel_position(panel, &px, &py);

    int x = (int)lx - px;
    int y = (int)ly - py;
    return x >= 0 && y >= 0 && x < recon_panel_width(panel) &&
        y < recon_panel_height(panel);
}

bool recon_hit_region(const struct recon_panel *panel, size_t index,
        int *x, int *y, int *w, int *h, uint32_t *id) {
    if (panel == NULL || index >= panel->hit_count) {
        return false;
    }
    const struct recon_hit_region *region = &panel->hits[index];
    if (x != NULL) { *x = region->x; }
    if (y != NULL) { *y = region->y; }
    if (w != NULL) { *w = region->w; }
    if (h != NULL) { *h = region->h; }
    if (id != NULL) { *id = region->id; }
    return true;
}

static uint32_t hit_test(struct recon_panel *panel, int x, int y,
        bool skip_inert) {
    if (panel == NULL) {
        return RECON_HIT_NONE;
    }
    /* Last added wins, matching draw order. */
    for (size_t i = panel->hit_count; i > 0; i--) {
        const struct recon_hit_region *r = &panel->hits[i - 1];
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) {
            /*
             * An inert region stops the search rather than being stepped
             * over. A disabled button covers whatever is behind it, and a
             * click that fell through to the panel underneath would be worse
             * than one that did nothing: the button is *there*, and it being
             * unavailable is not the same as it being absent.
             */
            if (skip_inert && r->inert) {
                return RECON_HIT_NONE;
            }
            return r->id;
        }
    }
    return RECON_HIT_NONE;
}

uint32_t recon_hit_test(struct recon_panel *panel, int x, int y) {
    return hit_test(panel, x, y, false);
}

uint32_t recon_hit_test_active(struct recon_panel *panel, int x, int y) {
    return hit_test(panel, x, y, true);
}

/* --- Double clicks --- */

static uint32_t g_last_click_id;
static uint64_t g_last_click_ms;

static uint64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

bool recon_click_is_double(uint32_t id) {
    uint64_t at = now_ms();
    bool paired = (id == g_last_click_id) && g_last_click_id != 0 &&
        at >= g_last_click_ms && (at - g_last_click_ms) <= RECON_DOUBLE_CLICK_MS;

    if (paired) {
        /*
         * Forgotten after a pair, so three clicks are a double and then a
         * single rather than two overlapping doubles.
         */
        g_last_click_id = 0;
        g_last_click_ms = 0;
        return true;
    }

    g_last_click_id = id;
    g_last_click_ms = at;
    return false;
}

void recon_click_forget(void) {
    g_last_click_id = 0;
    g_last_click_ms = 0;
}

/* --- Text entry --- */

#define EDIT_BG THEME(FIELD)
#define EDIT_TEXT THEME(FIELD_TEXT)
#define EDIT_CARET THEME(CARET)
#define EDIT_BORDER THEME(FIELD_BORDER)
#define EDIT_SELECTION THEME(FIELD_SELECTION)

void recon_edit_focus(struct recon_edit *edit) {
    if (edit == NULL) {
        return;
    }
    edit->active = true;
    edit->length = (int)strlen(edit->text);
    /* Selected whole, so the first key replaces a default rather than being
     * appended to it -- which is what recon_edit_begin was being used for, and
     * is the half of it that was worth keeping. */
    edit->caret = edit->length;
    edit->anchor = edit->length > 0 ? 0 : -1;
}

void recon_edit_begin(struct recon_edit *edit, const char *initial,
        bool select_stem) {
    if (edit == NULL) {
        return;
    }

    snprintf(edit->text, sizeof(edit->text), "%s", initial != NULL ? initial : "");
    edit->length = (int)strlen(edit->text);
    edit->caret = edit->length;
    edit->active = true;

    if (select_stem) {
        /* A leading dot is the whole name, not an extension, so ".config"
         * does not lose its identity to a rule about file types. */
        const char *dot = strrchr(edit->text, '.');
        if (dot != NULL && dot != edit->text) {
            edit->caret = (int)(dot - edit->text);
        }
    }

    /* Selected from the start, so the first keystroke replaces rather than
     * appends. Typing "Notes" over "New Folder" should leave "Notes", not
     * "New FolderNotes". */
    edit->anchor = (edit->caret > 0) ? 0 : -1;
}

void recon_edit_end(struct recon_edit *edit) {
    if (edit == NULL) {
        return;
    }
    edit->active = false;
    edit->text[0] = '\0';
    edit->length = 0;
    edit->caret = 0;
    edit->anchor = -1;
}

/* The selected span, low to high. False when nothing is selected. */
static bool edit_selection(const struct recon_edit *edit, int *from, int *to) {
    if (edit->anchor < 0 || edit->anchor == edit->caret) {
        return false;
    }
    *from = edit->anchor < edit->caret ? edit->anchor : edit->caret;
    *to = edit->anchor < edit->caret ? edit->caret : edit->anchor;
    return true;
}

/* Remove whatever is selected, leaving the caret where it was. Returns true
 * if anything went. */
static bool edit_delete_selection(struct recon_edit *edit) {
    int from, to;
    if (!edit_selection(edit, &from, &to)) {
        edit->anchor = -1;
        return false;
    }

    memmove(edit->text + from, edit->text + to,
        (size_t)(edit->length - to) + 1);
    edit->length -= (to - from);
    edit->caret = from;
    edit->anchor = -1;
    return true;
}

/*
 * Where the character containing or preceding `at` begins.
 *
 * The text is UTF-8, so a character is one to four bytes and the caret is a
 * byte offset. Stepping by one byte put the caret in the middle of a
 * character, and deleting one byte of a two-byte character left a broken
 * sequence in the field -- a name that had been typed correctly and could no
 * longer be read.
 *
 * Continuation bytes are 10xxxxxx. Walking back over them lands on the lead
 * byte, and the loop is bounded by the start of the buffer, so malformed text
 * costs a few wasted steps rather than a walk off the front.
 */
static int edit_step_back(const struct recon_edit *edit, int at) {
    if (at <= 0) {
        return 0;
    }
    at--;
    while (at > 0 && ((unsigned char)edit->text[at] & 0xC0) == 0x80) {
        at--;
    }
    return at;
}

/* And where the character at `at` ends. */
static int edit_step_forward(const struct recon_edit *edit, int at) {
    if (at >= edit->length) {
        return edit->length;
    }
    at++;
    while (at < edit->length &&
            ((unsigned char)edit->text[at] & 0xC0) == 0x80) {
        at++;
    }
    return at;
}

/* Insert a run of bytes -- one character's worth, or a whole paste. */
static void edit_insert_bytes(struct recon_edit *edit, const char *bytes,
        int count) {
    edit_delete_selection(edit);

    if (count <= 0 || edit->length + count >= RECON_EDIT_MAX) {
        return;
    }
    memmove(edit->text + edit->caret + count, edit->text + edit->caret,
        (size_t)(edit->length - edit->caret) + 1);
    memcpy(edit->text + edit->caret, bytes, (size_t)count);
    edit->caret += count;
    edit->length += count;
}

static void edit_insert(struct recon_edit *edit, char c) {
    edit_insert_bytes(edit, &c, 1);
}

/*
 * One character as UTF-8. Returns how many bytes it took, or 0 for something
 * that is not a character anybody types.
 */
static int encode_utf8(uint32_t c, char *out) {
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xC0 | (c >> 6));
        out[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | (c >> 12));
        out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    if (c <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (c >> 18));
        out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[3] = (char)(0x80 | (c & 0x3F));
        return 4;
    }
    return 0;
}

/* Remove the character starting at `index`, however many bytes that is. */
static void edit_delete_at(struct recon_edit *edit, int index) {
    if (index < 0 || index >= edit->length) {
        return;
    }
    int end = edit_step_forward(edit, index);
    int count = end - index;

    memmove(edit->text + index, edit->text + end,
        (size_t)(edit->length - end) + 1);
    edit->length -= count;
}

enum recon_edit_result recon_edit_key(struct recon_edit *edit,
        xkb_keysym_t sym, uint32_t modifiers) {
    if (edit == NULL || !edit->active) {
        return RECON_EDIT_IGNORED;
    }
    (void)modifiers;

    if ((modifiers & (1u << 2)) != 0) {  /* Ctrl */
        if (sym == XKB_KEY_a || sym == XKB_KEY_A) {
            edit->anchor = 0;
            edit->caret = edit->length;
            return RECON_EDIT_CHANGED;
        }

        /*
         * Cut, copy and paste, on the system's clipboard.
         *
         * Here rather than in each field's owner, because every field in
         * ReconOS is this editor -- renaming a file, typing a path, naming
         * one to save -- and a clipboard that works in some of them and not
         * others is worse than none, since which is which cannot be seen.
         */
        int from, to;
        if (sym == XKB_KEY_c || sym == XKB_KEY_C) {
            if (edit_selection(edit, &from, &to)) {
                recon_clip_set_text(edit->text + from, (size_t)(to - from));
            }
            return RECON_EDIT_IGNORED;
        }
        if (sym == XKB_KEY_x || sym == XKB_KEY_X) {
            if (edit_selection(edit, &from, &to)) {
                recon_clip_set_text(edit->text + from, (size_t)(to - from));
                edit_delete_selection(edit);
                return RECON_EDIT_CHANGED;
            }
            return RECON_EDIT_IGNORED;
        }
        if (sym == XKB_KEY_v || sym == XKB_KEY_V) {
            const char *held = recon_clip_text();
            if (*held == '\0') {
                return RECON_EDIT_IGNORED;
            }
            edit_delete_selection(edit);
            /*
             * One character at a time, through the same path typing takes, so
             * the field's own length limit applies. A newline is skipped:
             * this is a single line, and pasting a paragraph into a file name
             * should give the first line rather than a name with a line break
             * in it.
             */
            for (const char *c = held; *c != '\0'; c++) {
                if (*c == '\n' || *c == '\r') {
                    break;
                }
                edit_insert(edit, *c);
            }
            return RECON_EDIT_CHANGED;
        }

        /* Other control combinations are not text and must not be typed. */
        return RECON_EDIT_IGNORED;
    }

    switch (sym) {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        /* A multi-line field takes the newline instead of finishing. There is
         * no way to commit one from the keyboard, deliberately: a letter with
         * a blank line in it should not be sent by the key that made it. */
        if (edit->multiline) {
            edit_insert_bytes(edit, "\n", 1);
            return RECON_EDIT_CHANGED;
        }
        return RECON_EDIT_COMMIT;

    case XKB_KEY_Escape:
        return RECON_EDIT_CANCEL;

    case XKB_KEY_BackSpace:
        /* Backspace over a selection removes the selection, not the character
         * before it. */
        if (!edit_delete_selection(edit) && edit->caret > 0) {
            int start = edit_step_back(edit, edit->caret);
            edit_delete_at(edit, start);
            edit->caret = start;
        }
        return RECON_EDIT_CHANGED;

    case XKB_KEY_Delete:
        if (!edit_delete_selection(edit)) {
            edit_delete_at(edit, edit->caret);
        }
        return RECON_EDIT_CHANGED;

    case XKB_KEY_Left:
        /* Moving off a selection lands at its edge rather than stepping from
         * wherever the caret happened to be inside it. */
        if (edit->anchor >= 0) {
            int from, to;
            if (edit_selection(edit, &from, &to)) {
                edit->caret = from;
            }
            edit->anchor = -1;
        } else if (edit->caret > 0) {
            edit->caret = edit_step_back(edit, edit->caret);
        }
        return RECON_EDIT_CHANGED;

    case XKB_KEY_Right:
        if (edit->anchor >= 0) {
            int from, to;
            if (edit_selection(edit, &from, &to)) {
                edit->caret = to;
            }
            edit->anchor = -1;
        } else if (edit->caret < edit->length) {
            edit->caret = edit_step_forward(edit, edit->caret);
        }
        return RECON_EDIT_CHANGED;

    case XKB_KEY_Home:
        edit->caret = 0;
        edit->anchor = -1;
        return RECON_EDIT_CHANGED;

    case XKB_KEY_End:
        edit->caret = edit->length;
        edit->anchor = -1;
        return RECON_EDIT_CHANGED;

    default:
        break;
    }

    /*
     * Anything that produces a printable character is text. Control codes are
     * not: a stray Tab or newline inside a filename would be legal on disk and
     * impossible to see.
     *
     * Everything above the control range, not only ASCII. A keyboard laid out
     * for a language with accents in it sends those characters, and refusing
     * them meant a person could name a file only in English -- on a system
     * that will happily store the name and now draws it correctly.
     */
    uint32_t code = xkb_keysym_to_utf32(sym);
    if (code >= 0x20 && code != 0x7F) {
        char bytes[4];
        int count = encode_utf8(code, bytes);
        if (count > 0) {
            edit_insert_bytes(edit, bytes, count);
            return RECON_EDIT_CHANGED;
        }
    }

    return RECON_EDIT_IGNORED;
}

void recon_edit_draw(struct recon_panel *panel, struct recon_font *font,
        int x, int y, int w, int h, const struct recon_edit *edit) {
    if (panel == NULL || edit == NULL || w <= 0 || h <= 0) {
        return;
    }

    /*
     * A masked field is drawn from a string of dots of the same length, so
     * every measurement below -- the caret, the selection, the scrolling --
     * works on what is actually on screen rather than on the hidden text.
     * Measuring the real text and drawing dots would put the caret in the
     * wrong place for any character that is not the width of a dot.
     */
    struct recon_edit shown;
    if (edit->masked) {
        shown = *edit;
        int length = edit->length;
        if (length > RECON_EDIT_MAX - 1) {
            length = RECON_EDIT_MAX - 1;
        }
        for (int i = 0; i < length; i++) {
            shown.text[i] = '*';
        }
        shown.text[length] = '\0';
        edit = &shown;
    }

    recon_fill_rect(panel, x, y, w, h, EDIT_BG);
    recon_stroke_rect(panel, x, y, w, h, EDIT_BORDER);

    int pad = 3;
    int inner_w = w - pad * 2;
    if (inner_w <= 0 || font == NULL) {
        return;
    }

    /*
     * Scroll so the caret stays in view. Without this, typing a long name
     * silently continues past the right edge and the user is editing something
     * they cannot see.
     */
    char before[RECON_EDIT_MAX];
    int caret = edit->caret;
    if (caret > edit->length) {
        caret = edit->length;
    }
    memcpy(before, edit->text, (size_t)caret);
    before[caret] = '\0';

    int caret_x = recon_text_width(font, before);
    int offset = 0;
    if (caret_x > inner_w - 2) {
        offset = caret_x - (inner_w - 2);
    }

    int baseline = y + (h + recon_font_ascent(font)) / 2 - 1;

    /*
     * The selection, behind the text, so it is visible that typing will
     * replace rather than append.
     *
     * Only in the field being typed into, for the same reason as the caret
     * below: a highlight on a field that is not focused says the next key will
     * replace that text, and it will not. The Mail setup screen showed both
     * port numbers highlighted at once while the cursor was in Server.
     */
    int from, to;
    if (edit->active && edit_selection(edit, &from, &to)) {
        char head[RECON_EDIT_MAX];
        memcpy(head, edit->text, (size_t)from);
        head[from] = '\0';
        int from_x = recon_text_width(font, head);

        memcpy(head, edit->text, (size_t)to);
        head[to] = '\0';
        int to_x = recon_text_width(font, head);

        int sx = x + pad + from_x - offset;
        int sw = to_x - from_x;
        if (sx < x + pad) {
            sw -= (x + pad) - sx;
            sx = x + pad;
        }
        if (sw > 0) {
            if (sx + sw > x + w - pad) {
                sw = x + w - pad - sx;
            }
            recon_fill_rect(panel, sx, y + 2, sw, h - 4, EDIT_SELECTION);
        }
    }

    /* Drawn without truncation so the ellipsis logic does not fight the
     * scrolling; the panel clips whatever runs past its edge. */
    recon_draw_text(panel, font, x + pad - offset, baseline,
        inner_w + offset, edit->text, EDIT_TEXT);

    /*
     * The caret, only where the typing goes.
     *
     * It was drawn unconditionally, so every field on a form carried one at
     * once: the Mail setup screen showed a caret in Server, Username, Port,
     * Sending server, Sending port and Your address simultaneously, and none
     * of them meant anything. A caret is the answer to "where does the next
     * key land?", and six answers is no answer.
     *
     * Safe to gate on `active` because both ways of starting to edit --
     * recon_edit_begin and recon_edit_focus -- set it, so the windows with one
     * field each keep their caret without having been changed.
     */
    if (edit->active) {
        recon_fill_rect(panel, x + pad + caret_x - offset, y + 3, 1, h - 6,
            EDIT_CARET);
    }
}
