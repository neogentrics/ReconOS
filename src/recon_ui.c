/*
 * ReconOS UI layer implementation. See include/recon_ui.h.
 *
 * Pixels are ARGB8888, premultiplied-alpha ignored: the shell draws opaque
 * chrome, and text is blended against whatever is already in the buffer.
 */

#define _POSIX_C_SOURCE 200112L

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
};

#define MAX_HIT_REGIONS 64

struct recon_panel {
    struct wlr_scene_buffer *scene_buffer;
    int width, height;
    uint32_t *pixels; /* ARGB8888, width*height */

    struct recon_hit_region hits[MAX_HIT_REGIONS];
    size_t hit_count;
};

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
    memcpy(buf->pixels, panel->pixels, count * sizeof(uint32_t));
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
void recon_fill_role(struct recon_panel *panel, int x, int y, int w, int h,
        enum recon_theme_role role) {
    recon_color from, to;
    if (recon_theme_gradient(role, &from, &to)) {
        recon_fill_gradient(panel, x, y, w, h, from, to);
        return;
    }
    recon_fill_rect(panel, x, y, w, h, recon_theme_color(role));
}

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

/*
 * Round all four corners of a rectangle already drawn into the panel.
 *
 * Unlike a window's top corners, this fills the corner back in with what is
 * behind it rather than clearing it to transparent: a button sits on a panel
 * that has already been painted, and punching a hole in it would show the
 * wallpaper through the middle of a title bar.
 */
void recon_round_rect(struct recon_panel *panel, int x, int y, int w, int h,
        int radius, recon_color behind) {
    if (panel == NULL || radius <= 0 || w <= 0 || h <= 0) {
        return;
    }
    if (radius * 2 > w || radius * 2 > h) {
        return;    /* Rounder than the shape; leave it square. */
    }

    const int SAMPLES = 4;

    for (int dy = 0; dy < radius; dy++) {
        for (int dx = 0; dx < radius; dx++) {
            int inside = 0;

            for (int sy = 0; sy < SAMPLES; sy++) {
                for (int sx = 0; sx < SAMPLES; sx++) {
                    double px = dx + (sx + 0.5) / SAMPLES;
                    double py = dy + (sy + 0.5) / SAMPLES;
                    double ox = radius - px;
                    double oy = radius - py;
                    if (ox * ox + oy * oy <= (double)radius * radius) {
                        inside++;
                    }
                }
            }

            if (inside == SAMPLES * SAMPLES) {
                continue;
            }

            /* How much of the corner belongs to what is behind it. */
            int coverage = 255 - (inside * 255) / (SAMPLES * SAMPLES);

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
                *pixel = blend_over(*pixel, behind, coverage);
            }
        }
    }
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

    for (int y = 0; y < radius; y++) {
        uint32_t *row = panel->pixels + (size_t)y * panel->width;

        for (int x = 0; x < radius; x++) {
            int inside = 0;

            for (int sy = 0; sy < SAMPLES; sy++) {
                for (int sx = 0; sx < SAMPLES; sx++) {
                    /* The corner's circle is centred at (radius, radius), so
                     * a point is in the frame when it is no further from that
                     * centre than the radius. */
                    double px = x + (sx + 0.5) / SAMPLES;
                    double py = y + (sy + 0.5) / SAMPLES;
                    double dx = radius - px;
                    double dy = radius - py;
                    if (dx * dx + dy * dy <= (double)radius * radius) {
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

void recon_draw_bevel(struct recon_panel *panel, int x, int y, int w, int h,
        bool pressed) {
    /* Fixed highlight and shadow for now; a skin would supply these. */
    recon_color light = RECON_RGB(0xEE, 0xEE, 0xEE);
    recon_color dark = RECON_RGB(0x55, 0x55, 0x55);

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
void recon_draw_button_edge(struct recon_panel *panel, int x, int y, int w,
        int h, bool pressed, recon_color behind) {
    recon_draw_bevel(panel, x, y, w, h, pressed);

    int radius = recon_theme_metric(RECON_METRIC_BUTTON_CORNER);
    if (radius > 0) {
        recon_round_rect(panel, x, y, w, h, radius, behind);
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

void recon_draw_image(struct recon_panel *panel, int x, int y, int w, int h,
        const unsigned char *rgba, int image_width, int image_height) {
    if (panel == NULL || rgba == NULL || w <= 0 || h <= 0 ||
            image_width <= 0 || image_height <= 0) {
        return;
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
        if (py < 0 || py >= panel->height) {
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

uint32_t recon_hit_test(struct recon_panel *panel, int x, int y) {
    if (panel == NULL) {
        return RECON_HIT_NONE;
    }
    /* Last added wins, matching draw order. */
    for (size_t i = panel->hit_count; i > 0; i--) {
        const struct recon_hit_region *r = &panel->hits[i - 1];
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) {
            return r->id;
        }
    }
    return RECON_HIT_NONE;
}

/* --- Text entry --- */

#define EDIT_BG THEME(FIELD)
#define EDIT_TEXT THEME(FIELD_TEXT)
#define EDIT_CARET THEME(CARET)
#define EDIT_BORDER THEME(FIELD_BORDER)
#define EDIT_SELECTION THEME(FIELD_SELECTION)

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

    /* The selection, behind the text, so it is visible that typing will
     * replace rather than append. */
    int from, to;
    if (edit_selection(edit, &from, &to)) {
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

    recon_fill_rect(panel, x + pad + caret_x - offset, y + 3, 1, h - 6, EDIT_CARET);
}
