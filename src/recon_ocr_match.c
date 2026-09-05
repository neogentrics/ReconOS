/*
 * Naming the marks. See include/recon_ocr_match.h.
 */

#define _POSIX_C_SOURCE 200809L

/*
 * Our own copy of the rasteriser's code, kept to this file.
 *
 * src/recon_ui.c already emits the implementation with external linkage, and a
 * second copy without STBTT_STATIC collides with it on about two hundred
 * symbols -- in the *main executable*, not in a test, so the first sign of it
 * would be a desktop that stopped linking for a reason unconnected to anything
 * anybody was working on.
 *
 * The alternative, including the header bare and borrowing recon_ui's copy,
 * looks tidier and is worse: the desktop would link and only a test target
 * would fail, with undefined references, which reads as a build-file mistake
 * rather than as a decision about who owns the implementation.
 */
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_ocr_match.h"

/* --- What the engine will consider --- */

/*
 * Printable ASCII, without the space.
 *
 * The space is not a candidate because it is not a mark: a gap between marks is
 * what a space is, and that was decided upstream by looking at the whole line's
 * gaps rather than at one shape. A candidate with no ink would also match every
 * empty comparison perfectly, which is the kind of thing that produces a
 * confident page of spaces.
 */
#define FIRST_CP 33
#define LAST_CP 126
#define CANDIDATES (LAST_CP - FIRST_CP + 1)

/*
 * Sizes kept rendered at once.
 *
 * Fitting one line tries about ten distinct pixel heights, so a cache of eight
 * would evict what it is about to want again. Sixteen is a few hundred
 * kilobytes and removes the question.
 */
#define SIZE_SLOTS 16

/* Where the three height probes are measured, once per font. Large enough that
 * rounding does not matter and small enough to cost nothing. */
#define PROBE_PIXELS 64

/*
 * The numbers that decide what gets named.
 *
 * Calibrated rather than derived, and held honest by the tests rather than by
 * argument: the round-trip reads at four sizes must stay exact and the refusal
 * at the smallest must keep firing. A constant that has to move to keep those
 * green moves with a note saying why.
 */
#define SCORE_FLOOR 600      /* nothing below this is named at all */
#define SCORE_GOOD 850       /* at or above, the fit contributes full marks */
#define MARGIN_FULL 60       /* this much clear of the runner-up is decisive */
#define TIE 8                /* a lead thinner than this is noise */
#define TWINS 985            /* two candidates this alike are one candidate */
#define FIT_FLOOR 500        /* a line fitting worse than this is not read */
#define FIT_FLOOR_LONE 700   /* ...or this, when there is no hint to lean on */
#define MIN_X_HEIGHT 5       /* below this, refuse rather than guess */
#define MAX_MARK 256         /* a mark larger than this is refused, not scaled */

#define MIN_PIXELS 6
#define MAX_PIXELS 96

#define MAX_LINES 256
#define MAX_MARKS 512

/*
 * Blocks in one picture.
 *
 * A screen taken apart into windows, their bars and their paragraphs comes to
 * a few dozen. Two hundred and fifty six is room for a page of a newspaper and
 * a stop against a picture of noise, which splits until the depth limit ends
 * it.
 */
#define MAX_REGIONS 256

/* --- Errors --- */

static char g_error[192];

static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void fail(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, args);
    va_end(args);
}

const char *recon_ocr_match_last_error(void) {
    return g_error;
}

/* --- The font --- */

struct candidate {
    uint32_t codepoint;
    int glyph;              /* the rasteriser's own index */
    unsigned char *mask;    /* w * h, 0 or 1, trimmed to its own ink */
    int w, h;
    int top;                /* baseline-relative top row, positive downward */
    int area;
};

struct sizeset {
    int pixel_height;       /* 0 when the slot is empty */
    int count;
    int x_height;
    int max_w, max_h;
    struct candidate items[CANDIDATES];

    bool twins_built;
    unsigned char twins[CANDIDATES][(CANDIDATES + 7) / 8];

    /* Whether this candidate has any twin at all. The pairwise table says
     * which; this says whether to bother asking, and is what the refusal
     * actually tests. */
    bool has_twin[CANDIDATES];

    unsigned long stamp;
};

struct recon_ocr_font {
    unsigned char *owned;           /* NULL when the bytes are borrowed */
    const unsigned char *bytes;
    stbtt_fontinfo info;

    /* Box heights at PROBE_PIXELS, for guessing a size from a mark height. */
    int probe_x, probe_cap, probe_ascender;

    /*
     * The coverage level at which a rendered glyph counts as ink.
     *
     * A page decides its own threshold, so a candidate has to be cut at the
     * mirror of it or the two shapes differ by a pixel of boundary all the way
     * round. 128 is the value for a page whose threshold came out at 127, and
     * is only a starting point.
     */
    int ink_cut;

    struct sizeset sizes[SIZE_SLOTS];
    unsigned long clock;
};

/* --- Opening --- */

static bool measure_probe(struct recon_ocr_font *font, int codepoint,
        int *out) {
    float scale = stbtt_ScaleForPixelHeight(&font->info, (float)PROBE_PIXELS);
    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&font->info, codepoint, scale, scale,
        &x0, &y0, &x1, &y1);
    *out = y1 - y0;
    return *out > 0;
}

static struct recon_ocr_font *start(const unsigned char *bytes, size_t size,
        unsigned char *owned) {
    struct recon_ocr_font *font = calloc(1, sizeof(*font));
    if (font == NULL) {
        free(owned);
        fail("Out of memory opening a typeface to read with.");
        return NULL;
    }
    font->owned = owned;
    font->bytes = bytes;
    font->ink_cut = 128;

    if (!stbtt_InitFont(&font->info, bytes,
            stbtt_GetFontOffsetForIndex(bytes, 0))) {
        free(owned);
        free(font);
        fail("That file is not a typeface this can read.");
        return NULL;
    }
    (void)size;

    /*
     * Three heights, measured once.
     *
     * 'x', 'H' and 'l' because all three have flat tops and flat bottoms, so
     * the inked box is exactly the height being measured. A round letter
     * overshoots its own line at both ends and would make every guess about a
     * size a little too large.
     */
    if (!measure_probe(font, 'x', &font->probe_x) ||
            !measure_probe(font, 'H', &font->probe_cap) ||
            !measure_probe(font, 'l', &font->probe_ascender)) {
        free(owned);
        free(font);
        fail("That typeface has no letters this can measure itself against.");
        return NULL;
    }
    return font;
}

struct recon_ocr_font *recon_ocr_font_open_memory(const unsigned char *data,
        size_t size) {
    g_error[0] = '\0';
    if (data == NULL || size < 4) {
        fail("There is no typeface here.");
        return NULL;
    }
    return start(data, size, NULL);
}

struct recon_ocr_font *recon_ocr_font_open(const char *host_path) {
    g_error[0] = '\0';
    if (host_path == NULL) {
        fail("No typeface was named.");
        return NULL;
    }

    FILE *f = fopen(host_path, "rb");
    if (f == NULL) {
        fail("Cannot open '%s'.", host_path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        fail("Cannot read '%s'.", host_path);
        return NULL;
    }
    long size = ftell(f);
    rewind(f);

    if (size <= 0) {
        fclose(f);
        fail("'%s' is empty.", host_path);
        return NULL;
    }

    unsigned char *data = malloc((size_t)size);
    if (data == NULL || fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        fail("Cannot read all of '%s'.", host_path);
        return NULL;
    }
    fclose(f);

    /* The bytes are kept, not copied out of: the rasteriser parses lazily
     * from this buffer for as long as the font is open. */
    return start(data, (size_t)size, data);
}

struct recon_ocr_font *recon_ocr_font_find(void) {
    /* The same places the drawing layer looks, in the same order, so that what
     * this reads with is what the desktop wrote with. */
    static const char *const PLACES[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
        NULL,
    };

    g_error[0] = '\0';

    const char *named = getenv("RECONOS_OCR_FONT");
    if (named == NULL || *named == '\0') {
        named = getenv("RECONOS_FONT");
    }
    if (named != NULL && *named != '\0') {
        struct recon_ocr_font *font = recon_ocr_font_open(named);
        if (font != NULL) {
            return font;
        }
    }

    for (int i = 0; PLACES[i] != NULL; i++) {
        struct recon_ocr_font *font = recon_ocr_font_open(PLACES[i]);
        if (font != NULL) {
            return font;
        }
    }

    fail("This machine has no typeface to read with.");
    return NULL;
}

static void free_sizeset(struct sizeset *set) {
    for (int i = 0; i < set->count; i++) {
        free(set->items[i].mask);
    }
    memset(set, 0, sizeof(*set));
}

void recon_ocr_font_close(struct recon_ocr_font *font) {
    if (font == NULL) {
        return;
    }
    for (int i = 0; i < SIZE_SLOTS; i++) {
        free_sizeset(&font->sizes[i]);
    }
    free(font->owned);
    free(font);
}

/* --- The candidates at one size --- */

/*
 * Trim a rasterised glyph to its own ink, folding what was dropped into the
 * offset from the baseline.
 *
 * The rasteriser's box is the *outline's* extent. After thresholding, a stroke
 * whose edge was faint can lose a row, and then the two sides of every
 * comparison are no longer both ink-tight -- which is the property that makes
 * the horizontal alignment exact and needs no search.
 */
static bool trim(unsigned char *mask, int *w, int *h, int *top) {
    int width = *w, height = *h;
    int first_row = height, last_row = -1;
    int first_col = width, last_col = -1;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            if (mask[y * width + x] == 0) {
                continue;
            }
            if (y < first_row) { first_row = y; }
            if (y > last_row) { last_row = y; }
            if (x < first_col) { first_col = x; }
            if (x > last_col) { last_col = x; }
        }
    }
    if (last_row < 0) {
        return false;               /* nothing survived the threshold */
    }

    int new_w = last_col - first_col + 1;
    int new_h = last_row - first_row + 1;

    for (int y = 0; y < new_h; y++) {
        memmove(mask + (size_t)y * new_w,
            mask + (size_t)(y + first_row) * width + first_col,
            (size_t)new_w);
    }

    *w = new_w;
    *h = new_h;
    *top += first_row;
    return true;
}

static bool fill_sizeset(struct recon_ocr_font *font, struct sizeset *set,
        int pixel_height) {
    memset(set, 0, sizeof(*set));
    float scale = stbtt_ScaleForPixelHeight(&font->info, (float)pixel_height);

    for (uint32_t cp = FIRST_CP; cp <= LAST_CP; cp++) {
        /*
         * Asked for by glyph index rather than by codepoint.
         *
         * A codepoint the typeface does not have rasterises as .notdef -- a
         * rectangle, silently. Three absent codepoints give three identical
         * rectangles, and a rectangular mark then matches all three
         * confidently and is named whichever came first.
         */
        int glyph = stbtt_FindGlyphIndex(&font->info, (int)cp);
        if (glyph == 0) {
            continue;
        }

        int x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBox(&font->info, glyph, scale, scale,
            &x0, &y0, &x1, &y1);

        int w = x1 - x0, h = y1 - y0;
        if (w <= 0 || h <= 0 || w > MAX_MARK || h > MAX_MARK) {
            continue;
        }

        unsigned char *coverage = malloc((size_t)w * h);
        if (coverage == NULL) {
            /*
             * Reported, not swallowed. The drawing layer's own rasteriser
             * marks a glyph cached even when this allocation failed, which
             * caches an empty shape for the life of the font; here that would
             * be a character that silently stops being a candidate.
             */
            free_sizeset(set);
            fail("Out of memory rendering the alphabet at %d pixels.",
                pixel_height);
            return false;
        }
        stbtt_MakeGlyphBitmap(&font->info, coverage, w, h, w, scale, scale,
            glyph);

        /*
         * Cut to ink or not at all, rather than compared as coverage.
         *
         * The mark side has already been thresholded and has no gradient left
         * in it. Comparing a hard 0-or-1 against a soft 0-to-255 charges every
         * shape for its own antialiasing, which penalises whatever has the
         * most edge -- thin strokes and small sizes, exactly where this is
         * already working hardest.
         */
        for (long i = 0; i < (long)w * h; i++) {
            coverage[i] = coverage[i] >= font->ink_cut ? 1 : 0;
        }

        int top = y0;
        if (!trim(coverage, &w, &h, &top)) {
            free(coverage);
            continue;
        }

        struct candidate *c = &set->items[set->count++];
        c->codepoint = cp;
        c->glyph = glyph;
        c->mask = coverage;
        c->w = w;
        c->h = h;
        c->top = top;

        c->area = 0;
        for (long i = 0; i < (long)w * h; i++) {
            c->area += coverage[i] != 0;
        }

        if (w > set->max_w) { set->max_w = w; }
        if (h > set->max_h) { set->max_h = h; }
    }

    if (set->count == 0) {
        fail("Nothing in this typeface renders at %d pixels.", pixel_height);
        return false;
    }

    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&font->info, 'x', scale, scale,
        &x0, &y0, &x1, &y1);
    set->x_height = y1 - y0;

    set->pixel_height = pixel_height;
    return true;
}

static struct sizeset *sizeset_for(struct recon_ocr_font *font,
        int pixel_height) {
    if (pixel_height < MIN_PIXELS || pixel_height > MAX_PIXELS) {
        return NULL;
    }

    font->clock++;

    for (int i = 0; i < SIZE_SLOTS; i++) {
        if (font->sizes[i].pixel_height == pixel_height) {
            font->sizes[i].stamp = font->clock;
            return &font->sizes[i];
        }
    }

    /* An empty slot, or the one used longest ago. */
    int pick = 0;
    for (int i = 0; i < SIZE_SLOTS; i++) {
        if (font->sizes[i].pixel_height == 0) {
            pick = i;
            break;
        }
        if (font->sizes[i].stamp < font->sizes[pick].stamp) {
            pick = i;
        }
    }

    free_sizeset(&font->sizes[pick]);
    if (!fill_sizeset(font, &font->sizes[pick], pixel_height)) {
        return NULL;
    }
    font->sizes[pick].stamp = font->clock;
    return &font->sizes[pick];
}

void recon_ocr_font_set_ink_cut(struct recon_ocr_font *font, int cut) {
    if (font == NULL) {
        return;
    }
    if (cut < 1) { cut = 1; }
    if (cut > 254) { cut = 254; }

    if (cut == font->ink_cut) {
        return;
    }
    font->ink_cut = cut;

    /* Every rendered candidate was cut at the old level, so every one of them
     * is now the wrong shape. Thrown away rather than kept and corrected:
     * there is nothing to correct, they have to be drawn again. */
    for (int i = 0; i < SIZE_SLOTS; i++) {
        free_sizeset(&font->sizes[i]);
    }
}

int recon_ocr_font_ink_cut(const struct recon_ocr_font *font) {
    return font != NULL ? font->ink_cut : 0;
}

/* --- Looking at one shape --- */

int recon_ocr_font_render(struct recon_ocr_font *font, int pixel_height,
        uint32_t codepoint, unsigned char *mask, int *width, int *height,
        int *top) {
    if (font == NULL) {
        return -1;
    }
    struct sizeset *set = sizeset_for(font, pixel_height);
    if (set == NULL) {
        return -1;
    }

    for (int i = 0; i < set->count; i++) {
        if (set->items[i].codepoint != codepoint) {
            continue;
        }
        struct candidate *c = &set->items[i];
        if (width != NULL) { *width = c->w; }
        if (height != NULL) { *height = c->h; }
        if (top != NULL) { *top = c->top; }

        int bytes = c->w * c->h;
        if (mask != NULL) {
            memcpy(mask, c->mask, (size_t)bytes);
        }
        return bytes;
    }
    return -1;
}

int recon_ocr_font_confusion(struct recon_ocr_font *font, int pixel_height,
        uint32_t a, uint32_t b) {
    if (font == NULL) {
        return 0;
    }
    struct sizeset *set = sizeset_for(font, pixel_height);
    if (set == NULL) {
        return 0;
    }

    struct candidate *ca = NULL, *cb = NULL;
    for (int i = 0; i < set->count; i++) {
        if (set->items[i].codepoint == a) { ca = &set->items[i]; }
        if (set->items[i].codepoint == b) { cb = &set->items[i]; }
    }
    if (ca == NULL || cb == NULL) {
        return 0;
    }

    /*
     * Lined up on their shared baseline, not on their tops.
     *
     * Each mask is stored in its own frame, so mask row 0 of a candidate is
     * image row baseline + top. Two candidates therefore differ vertically by
     * the difference of their tops, and comparing them top-to-top instead
     * would call an 'o' and an 'O' the same shape -- which is the exact
     * opposite of what this measurement is for.
     */
    int dy = cb->top - ca->top;

    int best = 0;
    for (int dx = -1; dx <= 1; dx++) {
        int score = recon_ocr_mask_overlap(ca->mask, ca->w, ca->h,
            cb->mask, cb->w, cb->h, dx, dy);
        if (score > best) {
            best = score;
        }
    }
    return best;
}

/*
 * Which candidates are the same shape at this size.
 *
 * Built once per size and kept with it. This is the part of the design that
 * would not exist in a matcher that could not draw: it turns "l and I are hard
 * to tell apart" from something everybody knows into a measured fact about
 * this typeface at this size -- and makes the refusal deterministic, so it
 * fires even when noise happens to open a wide gap between two shapes that are
 * actually identical.
 */
static void build_twins(struct sizeset *set) {
    if (set->twins_built) {
        return;
    }
    set->twins_built = true;

    for (int i = 0; i < set->count; i++) {
        for (int j = i + 1; j < set->count; j++) {
            struct candidate *a = &set->items[i];
            struct candidate *b = &set->items[j];
            int dy = b->top - a->top;

            int best = 0;
            for (int dx = -1; dx <= 1; dx++) {
                int score = recon_ocr_mask_overlap(a->mask, a->w, a->h,
                    b->mask, b->w, b->h, dx, dy);
                if (score > best) {
                    best = score;
                }
            }
            if (best >= TWINS) {
                set->twins[i][j / 8] |= (unsigned char)(1u << (j % 8));
                set->twins[j][i / 8] |= (unsigned char)(1u << (i % 8));
                set->has_twin[i] = true;
                set->has_twin[j] = true;
            }
        }
    }
}

static bool are_twins(const struct sizeset *set, int i, int j) {
    if (i < 0 || j < 0) {
        return false;
    }
    return (set->twins[i][j / 8] & (1u << (j % 8))) != 0;
}

/* --- Copying a mark out of the page --- */

/*
 * A mark's own pixels, tight, so it can be compared without walking the whole
 * image once per candidate.
 *
 * Returns the inked area, or -1 when the mark is too large to be a character.
 * That is the underlined-text case: an underline touches every letter, so the
 * splitting step hands back a whole line as one mark. Refusing it is right, and
 * refusing is not the same as scaling it down until it fits -- which would find
 * a letter in it.
 */
static int copy_mark(const unsigned char *ink, int width,
        const struct recon_ocr_mark *mark, unsigned char *out, int max) {
    int w = mark->right - mark->left;
    int h = mark->bottom - mark->top;

    if (w <= 0 || h <= 0 || w > MAX_MARK || h > MAX_MARK || w * h > max) {
        return -1;
    }

    int area = 0;
    for (int y = 0; y < h; y++) {
        const unsigned char *row = ink + (size_t)(mark->top + y) * width +
            mark->left;
        unsigned char *dst = out + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            dst[x] = row[x] != 0;
            area += dst[x];
        }
    }
    return area;
}

/* --- Naming one mark --- */

static int confidence_from(int best, int second) {
    int quality;
    if (best <= SCORE_FLOOR) {
        quality = 0;
    } else if (best >= SCORE_GOOD) {
        quality = 100;
    } else {
        quality = (best - SCORE_FLOOR) * 100 / (SCORE_GOOD - SCORE_FLOOR);
    }

    int gap = best - second;
    if (gap < 0) {
        gap = 0;
    }
    int separation = (gap >= MARGIN_FULL) ? 100 : gap * 100 / MARGIN_FULL;

    /*
     * The smaller of the two, not a blend.
     *
     * Each is disqualifying on its own: a mark that matches nothing has not
     * been read, and a mark that matches two things equally has not been read
     * either. An average of "fit 0.95, margin 2 points" reports a high number
     * for a shape the engine cannot tell from another shape, which is exactly
     * the way this kind of tool lies.
     */
    return quality < separation ? quality : separation;
}

static void match_one(struct sizeset *set, const struct recon_ocr_fit *fit,
        const struct recon_ocr_mark *mark, const unsigned char *bits,
        int a_w, int a_h, int area_a, struct recon_ocr_match *out) {
    memset(out, 0, sizeof(*out));

    int best = 0, best_i = -1;
    int second = 0, second_i = -1;

    for (int i = 0; i < set->count; i++) {
        struct candidate *c = &set->items[i];

        /*
         * Where this candidate would have to sit, computed rather than
         * searched. Both shapes are trimmed to their own ink and the baseline
         * is fixed for the whole line, so a candidate has exactly one place it
         * could be.
         *
         * These three comparisons separate o from O from 0, and a comma from
         * an apostrophe, before a single pixel is compared. They are the
         * payoff for anchoring on the line's baseline instead of normalising
         * every mark into a common box: normalising discards height and width,
         * which is all those pairs differ by.
         */
        int dy = (fit->baseline + c->top) - mark->top;
        int bottom = fit->baseline + c->top + c->h;

        if (dy < -2 || dy > 2) {
            continue;
        }
        if (bottom - mark->bottom > 2 || mark->bottom - bottom > 2) {
            continue;
        }
        if (c->w - a_w > 3 || a_w - c->w > 3) {
            continue;
        }

        /*
         * An exact ceiling on what this candidate could possibly score, from
         * the two areas alone: the shared part is at most the smaller area,
         * and the covered part is at least the larger. Cheap, and it can never
         * discard the eventual winner.
         */
        int small = area_a < c->area ? area_a : c->area;
        int large = area_a < c->area ? c->area : area_a;
        if (1000L * small <= (long)best * large) {
            continue;
        }

        int score = 0;
        for (int dx = -1; dx <= 1; dx++) {
            int s = recon_ocr_mask_overlap(bits, a_w, a_h, c->mask, c->w, c->h,
                dx, dy);
            if (s > score) {
                score = s;
            }
        }

        if (score > best) {
            second = best;
            second_i = best_i;
            best = score;
            best_i = i;
        } else if (score > second) {
            second = score;
            second_i = i;
        }
    }

    if (best_i < 0) {
        return;                 /* nothing could even be placed here */
    }

    out->score = best;
    out->runner_up_score = second;
    out->runner_up = second_i >= 0 ? set->items[second_i].codepoint : 0;

    if (best < SCORE_FLOOR) {
        return;                 /* nothing fitted */
    }
    if (second_i >= 0 && best - second <= TIE) {
        return;                 /* too close to call */
    }
    /*
     * The winner is a shape this face repeats at this size.
     *
     * Any twin, not just the runner-up. Comparing only the top two was the
     * weaker rule and it let an 'l' be named 'I': the two are identical in
     * DejaVu Sans at half the sizes this reads at, but when 'I' won and
     * something else placed second, nothing ever looked at 'l'.
     *
     * Deterministic where the margin test is not -- it fires even when a stray
     * pixel opens a wide gap between two shapes that are the same shape.
     */
    if (set->has_twin[best_i]) {
        return;
    }

    out->codepoint = set->items[best_i].codepoint;
    out->confidence = confidence_from(best, second);
}

/* --- Fitting a line --- */

/* Scratch for one mark. Static because it is large, single-threaded, and
 * capped: a mark bigger than MAX_MARK is refused before it gets here. */
static unsigned char g_bits[MAX_MARK * MAX_MARK];

/*
 * How well a whole line matches at a given baseline and size.
 *
 * Sampled rather than exhaustive, and sampled evenly rather than from the
 * front: the first eight marks of a line are often all lowercase, and a
 * baseline wrong by one row fits lowercase almost as well as the right one.
 * Spreading the sample across the line puts a capital or a descender into it,
 * which is what makes a wrong baseline score worse.
 */
static int score_fit(struct recon_ocr_font *font, const unsigned char *ink,
        int width, const struct recon_ocr_mark *marks, int count,
        int baseline, int pixel_height) {
    struct sizeset *set = sizeset_for(font, pixel_height);
    if (set == NULL) {
        return 0;
    }

    int step = (count + 7) / 8;
    if (step < 1) {
        step = 1;
    }

    long total = 0;
    int sampled = 0;

    for (int i = 0; i < count; i += step) {
        int w = marks[i].right - marks[i].left;
        int h = marks[i].bottom - marks[i].top;
        int area = copy_mark(ink, width, &marks[i], g_bits,
            (int)sizeof(g_bits));
        if (area <= 0) {
            continue;
        }

        int best = 0;
        for (int c = 0; c < set->count; c++) {
            struct candidate *cand = &set->items[c];
            int dy = (baseline + cand->top) - marks[i].top;
            if (dy < -2 || dy > 2) {
                continue;
            }
            int bottom = baseline + cand->top + cand->h;
            if (bottom - marks[i].bottom > 2 || marks[i].bottom - bottom > 2) {
                continue;
            }
            if (cand->w - w > 3 || w - cand->w > 3) {
                continue;
            }
            for (int dx = -1; dx <= 1; dx++) {
                int s = recon_ocr_mask_overlap(g_bits, w, h, cand->mask,
                    cand->w, cand->h, dx, dy);
                if (s > best) {
                    best = s;
                }
            }
        }
        total += best;
        sampled++;
    }

    return sampled > 0 ? (int)(total / sampled) : 0;
}

bool recon_ocr_fit_line(struct recon_ocr_font *font, const unsigned char *ink,
        int width, int height, const struct recon_ocr_mark *marks,
        int mark_count, const struct recon_ocr_fit *hint,
        struct recon_ocr_fit *out) {
    (void)height;

    if (font == NULL || ink == NULL || marks == NULL || out == NULL ||
            mark_count <= 0) {
        return false;
    }

    /*
     * Where the line most likely sits.
     *
     * The commonest bottom edge among the marks, because most letters in a
     * line stand on the baseline. Plus the row above each, because an unhinted
     * round letter -- o, e, c, s, O, 0 -- overshoots the line it stands on, so
     * its bottom edge is one row below the baseline. Without that, a line of
     * round letters fits a baseline one row too low and every candidate is
     * then rejected for sitting wrong.
     */
    int trials[16];
    int trial_count = 0;

    for (int pick = 0; pick < 3 && trial_count < 12; pick++) {
        int best_value = 0, best_seen = 0;

        for (int i = 0; i < mark_count; i++) {
            int value = marks[i].bottom;

            bool already = false;
            for (int t = 0; t < trial_count && !already; t++) {
                already = trials[t] == value || trials[t] == value - 1;
            }
            if (already) {
                continue;
            }

            int seen = 0;
            for (int j = 0; j < mark_count; j++) {
                seen += marks[j].bottom == value;
            }
            if (seen > best_seen) {
                best_seen = seen;
                best_value = value;
            }
        }
        if (best_seen == 0) {
            break;
        }
        trials[trial_count++] = best_value;
        trials[trial_count++] = best_value - 1;
    }
    if (trial_count == 0) {
        return false;
    }

    int best_quality = 0, best_baseline = trials[0], best_pixels = 0;

    /*
     * With the previous line's fit to lean on, try only around it. The lines
     * of a page are almost always the same size, so this is the difference
     * between a page costing what one line costs and costing what every line
     * costs.
     */
    if (hint != NULL && hint->pixel_height >= MIN_PIXELS) {
        for (int p = hint->pixel_height - 1; p <= hint->pixel_height + 1; p++) {
            for (int t = 0; t < trial_count; t++) {
                int q = score_fit(font, ink, width, marks, mark_count,
                    trials[t], p);
                if (q > best_quality) {
                    best_quality = q;
                    best_baseline = trials[t];
                    best_pixels = p;
                }
            }
        }
    }

    bool hint_held = hint != NULL && best_quality >= FIT_FLOOR &&
        best_quality >= hint->quality - 100;

    if (!hint_held) {
        best_quality = 0;
        best_pixels = 0;

        for (int t = 0; t < trial_count; t++) {
            int baseline = trials[t];

            /*
             * The commonest height among marks standing on this baseline.
             * Descenders are excluded by the bottom-edge test, because a 'y'
             * says nothing about how tall the letters are.
             */
            int tallest = 0, tallest_seen = 0;
            for (int i = 0; i < mark_count; i++) {
                int gap = marks[i].bottom - baseline;
                if (gap < -1 || gap > 1) {
                    continue;
                }
                int h = baseline - marks[i].top;
                if (h <= 0) {
                    continue;
                }
                int seen = 0;
                for (int j = 0; j < mark_count; j++) {
                    int other = marks[j].bottom - baseline;
                    if (other >= -1 && other <= 1 &&
                            baseline - marks[j].top == h) {
                        seen++;
                    }
                }
                if (seen > tallest_seen) {
                    tallest_seen = seen;
                    tallest = h;
                }
            }
            if (tallest <= 0) {
                continue;
            }

            /*
             * Three guesses at the size, because the commonest mark height in
             * a line is the x-height in "the quick brown", the cap height in
             * "HELLO", and the ascender height in "Hello". One guess is wrong
             * by nearly half on the other two.
             */
            const int probes[3] = {
                font->probe_x, font->probe_cap, font->probe_ascender,
            };
            for (int s = 0; s < 3; s++) {
                if (probes[s] <= 0) {
                    continue;
                }
                int p = (PROBE_PIXELS * tallest + probes[s] / 2) / probes[s];
                if (p < MIN_PIXELS || p > MAX_PIXELS) {
                    continue;
                }
                int q = score_fit(font, ink, width, marks, mark_count,
                    baseline, p);
                if (q > best_quality) {
                    best_quality = q;
                    best_baseline = baseline;
                    best_pixels = p;
                }
            }
        }

        if (best_pixels == 0) {
            return false;
        }

        /* Two refinements, one axis at a time: the size around the best, then
         * the baseline around that. */
        for (int p = best_pixels - 2; p <= best_pixels + 2; p++) {
            if (p < MIN_PIXELS || p > MAX_PIXELS || p == best_pixels) {
                continue;
            }
            int q = score_fit(font, ink, width, marks, mark_count,
                best_baseline, p);
            if (q > best_quality) {
                best_quality = q;
                best_pixels = p;
            }
        }
        for (int b = best_baseline - 1; b <= best_baseline + 1; b++) {
            if (b == best_baseline) {
                continue;
            }
            int q = score_fit(font, ink, width, marks, mark_count, b,
                best_pixels);
            if (q > best_quality) {
                best_quality = q;
                best_baseline = b;
            }
        }
    }

    if (best_pixels < MIN_PIXELS) {
        return false;
    }

    /*
     * A short line has too few marks to fit two numbers from, so it has to
     * clear a higher bar before its answer is believed. Two marks are not a
     * population, and a baseline fitted from them is a coincidence.
     */
    int needed = (hint == NULL && mark_count < 3) ? FIT_FLOOR_LONE : FIT_FLOOR;
    if (best_quality < needed) {
        return false;
    }

    struct sizeset *set = sizeset_for(font, best_pixels);
    if (set == NULL || set->x_height < MIN_X_HEIGHT) {
        return false;
    }

    out->pixel_height = best_pixels;
    out->baseline = best_baseline;
    out->x_height = set->x_height;
    out->quality = best_quality;
    return true;
}

int recon_ocr_match_marks(struct recon_ocr_font *font,
        const unsigned char *ink, int width, int height,
        const struct recon_ocr_fit *fit, const struct recon_ocr_mark *marks,
        int mark_count, struct recon_ocr_match *out) {
    (void)height;

    if (font == NULL || ink == NULL || fit == NULL || marks == NULL ||
            out == NULL || mark_count <= 0) {
        return -1;
    }

    struct sizeset *set = sizeset_for(font, fit->pixel_height);
    if (set == NULL) {
        return -1;
    }
    build_twins(set);

    for (int i = 0; i < mark_count; i++) {
        memset(&out[i], 0, sizeof(out[i]));

        int w = marks[i].right - marks[i].left;
        int h = marks[i].bottom - marks[i].top;

        /* Larger than any letter of this size could be: an underline, a rule,
         * or two lines that ran together. Refused rather than shrunk to fit. */
        if (w > set->max_w + 4 || h > set->max_h + 4) {
            continue;
        }

        int area = copy_mark(ink, width, &marks[i], g_bits,
            (int)sizeof(g_bits));
        if (area <= 0) {
            continue;
        }
        match_one(set, fit, &marks[i], g_bits, w, h, area, &out[i]);
    }
    return mark_count;
}

/* --- The whole picture --- */

/* U+FFFD, the replacement character, as UTF-8. Not a question mark: that is a
 * character somebody could have written, and using a real one to mean "I do not
 * know" is the same lie in miniature that this engine exists to avoid. */
static void append_unknown(char *text, size_t *at, size_t max) {
    if (*at + 3 >= max) {
        return;
    }
    text[(*at)++] = (char)0xEF;
    text[(*at)++] = (char)0xBF;
    text[(*at)++] = (char)0xBD;
}

bool recon_ocr_read(const unsigned char *rgba, int width, int height,
        struct recon_ocr_font *font, struct recon_ocr_result *out) {
    g_error[0] = '\0';

    if (rgba == NULL || font == NULL || out == NULL || width <= 0 ||
            height <= 0) {
        fail("There is nothing to read.");
        return false;
    }
    memset(out, 0, sizeof(*out));

    unsigned char *ink = malloc((size_t)width * height);
    if (ink == NULL) {
        fail("Out of memory reading a picture that size.");
        return false;
    }

    struct recon_ocr_polarity page;
    if (!recon_ocr_ink_detail(rgba, width, height, ink, &page)) {
        free(ink);
        fail("%s", recon_ocr_last_error());
        return false;
    }

    /*
     * Cut the candidates where the page was cut.
     *
     * A page pixel is ink when its value is at or below the threshold, and a
     * rasterised glyph draws coverage c as 255 - c, so the equivalent cut is
     * 255 - threshold. Light text on a dark page is the same argument the
     * other way round and the threshold is used directly.
     *
     * This is what makes "the system can generate the shape it is looking for"
     * true rather than nearly true. Without it the same letter comes out two
     * pixels wider from the page than from the rasteriser, and the score
     * measures the gap between two thresholds instead of the gap between two
     * letters.
     */
    recon_ocr_font_set_ink_cut(font,
        page.ink_is_dark ? 255 - page.threshold : page.threshold);

    struct recon_ocr_region *regions = calloc(MAX_REGIONS, sizeof(*regions));
    struct recon_ocr_line *lines = calloc(MAX_LINES, sizeof(*lines));
    struct recon_ocr_mark *marks = calloc(MAX_MARKS, sizeof(*marks));
    struct recon_ocr_match *matches = calloc(MAX_MARKS, sizeof(*matches));

    if (regions == NULL || lines == NULL || marks == NULL ||
            matches == NULL) {
        free(ink); free(regions); free(lines); free(marks); free(matches);
        fail("Out of memory.");
        return false;
    }

    /*
     * Blocks before lines.
     *
     * A picture that is only text comes back as one block and nothing changes.
     * A screenshot comes back as many, and that is what makes it readable at
     * all: looking for lines across a whole screen finds bands that span the
     * window borders, and every mark in one of those is a blob.
     */
    int region_count = recon_ocr_regions(ink, width, height, regions,
        MAX_REGIONS);
    if (region_count == MAX_REGIONS) {
        out->truncated = true;
    }
    if (region_count == 0) {
        free(ink); free(regions); free(lines); free(marks); free(matches);
        fail("There is nothing in this picture that looks like a line of "
            "text.");
        return false;
    }

    /* Generous: every mark can cost three bytes, plus a space before it and a
     * newline per line. */
    size_t room = (size_t)region_count * (MAX_MARKS * 4 + 2) + 1;
    char *text = malloc(room);
    if (text == NULL) {
        free(ink); free(regions); free(lines); free(marks); free(matches);
        fail("Out of memory.");
        return false;
    }

    size_t at = 0;
    long confidence_total = 0;
    int named = 0;

    struct recon_ocr_fit previous;
    bool have_previous = false;

    int line_total = 0;

    for (int region = 0; region < region_count; region++) {
        int line_count = recon_ocr_lines_in(ink, width, height,
            &regions[region], lines, MAX_LINES);
        if (line_count == MAX_LINES) {
            out->truncated = true;
        }

        /*
         * A fit is not carried between blocks.
         *
         * Within a block the lines are almost always one size, which is what
         * makes the hint worth having. Between blocks they are usually not --
         * a title bar, a menu and a paragraph are three sizes -- and a hint
         * from the wrong block costs more than it saves, because a fit that is
         * accepted on a stale hint is a whole block read at the wrong size.
         */
        have_previous = false;

        for (int i = 0; i < line_count; i++) {
            line_total++;

        int mark_count = recon_ocr_marks(ink, width, &lines[i], marks,
            MAX_MARKS);
        if (mark_count == MAX_MARKS) {
            out->truncated = true;
        }
        if (mark_count <= 0) {
            continue;
        }

        struct recon_ocr_fit fit;
        bool fitted = recon_ocr_fit_line(font, ink, width, height, marks,
            mark_count, have_previous ? &previous : NULL, &fit);

        if (fitted) {
            recon_ocr_match_marks(font, ink, width, height, &fit, marks,
                mark_count, matches);
            previous = fit;
            have_previous = true;
        } else {
            /* Not readable at any size. Every mark in it is unread, which is
             * the honest answer -- not a second attempt with the rules
             * loosened until something comes back. */
            memset(matches, 0, sizeof(*matches) * (size_t)mark_count);
        }

        for (int m = 0; m < mark_count; m++) {
            if (marks[m].space_before && at + 1 < room) {
                text[at++] = ' ';
            }
            if (matches[m].codepoint != 0) {
                if (at + 1 < room) {
                    text[at++] = (char)matches[m].codepoint;
                }
                confidence_total += matches[m].confidence;
                named++;
            } else {
                append_unknown(text, &at, room);
                out->unrecognised++;
            }
            out->characters++;
        }

        if (at + 1 < room) {
            text[at++] = '\n';
        }
        }
    }

    /* The last newline is a separator with nothing after it. */
    while (at > 0 && text[at - 1] == '\n') {
        at--;
    }
    text[at] = '\0';

    out->text = text;
    out->length = at;
    out->lines = line_total;

    /*
     * Averaged over what was named, with the refused excluded rather than
     * entered as zeros. The header already gives refusals their own channel
     * and says the ratio is the better signal precisely because an average
     * hides them; counting them twice would make both numbers misleading.
     */
    out->confidence = named > 0 ? (int)(confidence_total / named) : 0;

    free(ink);
    free(regions);
    free(lines);
    free(marks);
    free(matches);
    return true;
}

void recon_ocr_result_free(struct recon_ocr_result *result) {
    if (result == NULL) {
        return;
    }
    free(result->text);
    result->text = NULL;
    result->length = 0;
}

bool recon_ocr_result_is_a_reading(const struct recon_ocr_result *result) {
    if (result == NULL || result->characters == 0) {
        return false;
    }
    /* More than half refused is not a reading however well the rest scored. */
    if (result->unrecognised * 2 >= result->characters) {
        return false;
    }
    return result->confidence >= 60;
}
