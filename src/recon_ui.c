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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

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
};

/* Searched in order when no font path is given. */
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
    return (int)((font->ascent - font->descent + font->line_gap) * font->scale);
}

/* Rasterize a character on first use; later calls reuse the cached mask. */
static struct recon_glyph *glyph_for(struct recon_font *font, unsigned char c) {
    if (font == NULL || c < GLYPH_FIRST || c > GLYPH_LAST) {
        return NULL;
    }

    struct recon_glyph *glyph = &font->glyphs[c - GLYPH_FIRST];
    if (glyph->cached) {
        return glyph;
    }

    int advance, left_bearing;
    stbtt_GetCodepointHMetrics(&font->info, c, &advance, &left_bearing);
    glyph->advance = (int)(advance * font->scale + 0.5f);

    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&font->info, c, font->scale, font->scale,
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
                font->scale, font->scale, c);
        }
    }

    glyph->cached = true;
    return glyph;
}

int recon_text_width(struct recon_font *font, const char *text) {
    if (font == NULL || text == NULL) {
        return 0;
    }

    int width = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        struct recon_glyph *glyph = glyph_for(font, *p);
        if (glyph != NULL) {
            width += glyph->advance;
        }
    }
    return width;
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
struct panel_buffer {
    struct wlr_buffer base;
    struct recon_panel *panel;
};

static void panel_buffer_destroy(struct wlr_buffer *wlr_buffer) {
    struct panel_buffer *buf = wl_container_of(wlr_buffer, buf, base);
    free(buf);
}

static bool panel_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
        uint32_t flags, void **data, uint32_t *format, size_t *stride) {
    struct panel_buffer *buf = wl_container_of(wlr_buffer, buf, base);
    *data = buf->panel->pixels;
    *format = DRM_FORMAT_ARGB8888;
    *stride = (size_t)buf->panel->width * 4;
    return true;
}

static void panel_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
    /* Pixels stay mapped for the panel's lifetime. */
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

    free(panel->pixels);
    panel->pixels = pixels;
    panel->width = width;
    panel->height = height;
    return true;
}

void recon_panel_commit(struct recon_panel *panel) {
    if (panel == NULL || panel->scene_buffer == NULL) {
        return;
    }

    struct panel_buffer *buf = calloc(1, sizeof(*buf));
    if (buf == NULL) {
        return;
    }
    buf->panel = panel;
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
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        struct recon_glyph *glyph = glyph_for(font, *p);
        if (glyph == NULL) {
            continue;
        }
        if (max_width > 0 && (pen - x) + glyph->advance > limit) {
            break;
        }
        draw_glyph(panel, glyph, pen, y, color);
        pen += glyph->advance;
    }

    if (truncating) {
        for (const char *e = "..."; *e != '\0'; e++) {
            struct recon_glyph *glyph = glyph_for(font, (unsigned char)*e);
            if (glyph == NULL) {
                continue;
            }
            draw_glyph(panel, glyph, pen, y, color);
            pen += glyph->advance;
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
