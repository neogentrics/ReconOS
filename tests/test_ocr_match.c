/*
 * Tests for naming the marks.
 *
 * Two halves, and the split matters. The metric is arithmetic with a known
 * answer and is checked against masks written out by hand -- those run
 * anywhere. Everything else needs a typeface, and a machine without one skips
 * those rather than failing: there is no skip convention in this tree and a
 * nonzero exit is read as a failure, so an absent font would look like a
 * broken engine.
 *
 * The round trip is the real test. Render a string with the same rasteriser the
 * engine matches against, read it back, and require it *exactly*. That is the
 * target case stated as an assertion: a screenshot of ReconOS's own text is
 * text this system drew, and if the engine cannot read back what it just wrote
 * then the premise the whole design rests on is false.
 *
 * Run with: cmake --build build && ./build/recon_ocr_match_tests
 */

/*
 * The test draws its own samples, so it needs its own copy of the rasteriser.
 *
 * Static, like the matcher's. Two private copies in two translation units is
 * exactly what STBTT_STATIC is for, and it is the only arrangement where
 * neither this file nor src/recon_ui.c can collide with the other -- the
 * alternative, one shared external copy, is what makes the main executable
 * fail to link the moment a second file wants the rasteriser.
 */
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_ocr.h"
#include "recon_ocr_match.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
    }
}

/* --- The metric, on masks with an answer worked out by hand --- */

static void test_overlap(void) {
    printf("Overlap\n");

    unsigned char full[4] = { 1, 1, 1, 1 };          /* 2x2, all ink */
    unsigned char dot[1] = { 1 };
    unsigned char nine[9] = { 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    unsigned char empty[4] = { 0, 0, 0, 0 };

    /* Identical and aligned: everything shared, nothing else covered. */
    check(recon_ocr_mask_overlap(full, 2, 2, full, 2, 2, 0, 0) == 1000,
        "the same mask on itself is 1000");

    /*
     * Slid one column across. Two of four pixels line up; the covered area is
     * 4 + 4 - 2 = 6. 1000 * 2 / 6, rounded, is 333.
     */
    check(recon_ocr_mask_overlap(full, 2, 2, full, 2, 2, 1, 0) == 333,
        "slid one column: 333");

    /* Slid clear: nothing shared. */
    check(recon_ocr_mask_overlap(full, 2, 2, full, 2, 2, 2, 0) == 0,
        "slid clear: 0");

    /*
     * A single pixel inside a three-by-three block. One shared, 9 + 1 - 1 = 9
     * covered, so 111.
     *
     * This is the whole reason the metric divides by the union: by any measure
     * that divides by the smaller shape, a dot inside a block scores a perfect
     * 1000, because the dot is entirely inside. That is what would make a '0'
     * and an 'O' the same letter.
     */
    check(recon_ocr_mask_overlap(nine, 3, 3, dot, 1, 1, 1, 1) == 111,
        "a dot inside a block: 111, not 1000");

    check(recon_ocr_mask_overlap(full, 2, 2, empty, 2, 2, 0, 0) == 0,
        "an empty mask matches nothing");
    check(recon_ocr_mask_overlap(NULL, 2, 2, full, 2, 2, 0, 0) == 0,
        "no mask is refused");
    check(recon_ocr_mask_overlap(full, 0, 2, full, 2, 2, 0, 0) == 0,
        "no width is refused");
}

/* --- Drawing a line of text to read back --- */

#define PAGE_W 640
#define PAGE_H 80

struct page {
    unsigned char rgba[PAGE_W * PAGE_H * 4];
};

/*
 * Draw a string the way the desktop draws one.
 *
 * Deliberately the same shape as src/recon_ui.c's text path: whole-pixel
 * advances, no hinting, no subpixel positioning, the glyph placed at the
 * baseline plus its own bearing. A test that drew text some other way would be
 * testing whether the engine can read that other way.
 */
static bool draw_text(struct page *p, stbtt_fontinfo *info, int pixel_height,
        const char *text, int x, int baseline) {
    for (int i = 0; i < PAGE_W * PAGE_H; i++) {
        p->rgba[i * 4 + 0] = 255;
        p->rgba[i * 4 + 1] = 255;
        p->rgba[i * 4 + 2] = 255;
        p->rgba[i * 4 + 3] = 255;
    }

    float scale = stbtt_ScaleForPixelHeight(info, (float)pixel_height);
    int pen = x;

    for (const char *c = text; *c != '\0'; c++) {
        int advance, bearing;
        stbtt_GetCodepointHMetrics(info, *c, &advance, &bearing);

        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(info, *c, scale, scale, &x0, &y0, &x1, &y1);

        int w = x1 - x0, h = y1 - y0;
        if (w > 0 && h > 0) {
            unsigned char *bits = malloc((size_t)w * h);
            if (bits == NULL) {
                return false;
            }
            stbtt_MakeCodepointBitmap(info, bits, w, h, w, scale, scale, *c);

            for (int row = 0; row < h; row++) {
                int py = baseline + y0 + row;
                if (py < 0 || py >= PAGE_H) {
                    continue;
                }
                for (int col = 0; col < w; col++) {
                    int px = pen + x0 + col;
                    if (px < 0 || px >= PAGE_W) {
                        continue;
                    }
                    int v = 255 - bits[row * w + col];
                    int at = (py * PAGE_W + px) * 4;
                    p->rgba[at + 0] = (unsigned char)v;
                    p->rgba[at + 1] = (unsigned char)v;
                    p->rgba[at + 2] = (unsigned char)v;
                }
            }
            free(bits);
        }
        pen += (int)(advance * scale + 0.5f);
    }
    return true;
}

/* --- The round trip --- */

/*
 * No 'l', no 'I', no '1'.
 *
 * Those are the shapes this design refuses at small sizes, on purpose, and a
 * first test whose pass depends on a face-specific difference between an
 * ascender and a capital would be testing the typeface rather than the code.
 */
#define SAMPLE "Hamburgefonstiv 0234"

/*
 * Everything the engine named, with the refusals taken out.
 *
 * U+FFFD is three bytes; anything else here is one, because the candidate set
 * is printable ASCII.
 */
static void without_refusals(const char *text, char *out, size_t max) {
    size_t at = 0;
    for (const unsigned char *c = (const unsigned char *)text;
            *c != '\0' && at + 1 < max; c++) {
        if (c[0] == 0xEF && c[1] == 0xBF && c[2] == 0xBD) {
            c += 2;
            continue;
        }
        out[at++] = (char)*c;
    }
    out[at] = '\0';
}

/* Is every character of `part` present in `whole`, in order? */
static bool is_subsequence(const char *part, const char *whole) {
    const char *w = whole;
    for (const char *p = part; *p != '\0'; p++) {
        while (*w != '\0' && *w != *p) {
            w++;
        }
        if (*w == '\0') {
            return false;
        }
        w++;
    }
    return true;
}

static void round_trip(struct recon_ocr_font *font, stbtt_fontinfo *info,
        int pixel_height, bool expect_a_reading, bool expect_exact) {
    static struct page page;

    if (!draw_text(&page, info, pixel_height, SAMPLE, 12, PAGE_H / 2)) {
        check(false, "could not draw the sample");
        return;
    }

    struct recon_ocr_result result;
    if (!recon_ocr_read(page.rgba, PAGE_W, PAGE_H, font, &result)) {
        char what[128];
        snprintf(what, sizeof(what), "%dpx: reading began", pixel_height);
        check(!expect_a_reading, what);
        return;
    }

    char what[192];

    if (expect_a_reading) {
        /*
         * The claim, at every size: nothing the engine named was wrong.
         *
         * Column splitting joins letters whose ink overlaps -- at 24px 'rg'
         * and 'fo' arrive as single blobs -- and the engine scores those in
         * the two hundreds and refuses them. That is the design working. What
         * must never happen is a blob being named a letter.
         */
        char named[256];
        without_refusals(result.text, named, sizeof(named));

        snprintf(what, sizeof(what),
            "%dpx: nothing invented (read \"%s\")", pixel_height,
            result.text);
        check(is_subsequence(named, SAMPLE), what);

        snprintf(what, sizeof(what), "%dpx: most of it read (%d of %d "
            "refused)", pixel_height, result.unrecognised, result.characters);
        check(result.unrecognised * 4 < result.characters, what);

        snprintf(what, sizeof(what), "%dpx: confident (%d)",
            pixel_height, result.confidence);
        check(result.confidence >= 80, what);

        snprintf(what, sizeof(what), "%dpx: counts as a reading",
            pixel_height);
        check(recon_ocr_result_is_a_reading(&result), what);

        if (expect_exact) {
            /*
             * And at one size, where the marks come out clean, the whole
             * thing exactly. This is the premise the design rests on stated
             * as an assertion: the system rendered this text, so the system
             * can read it back.
             */
            snprintf(what, sizeof(what),
                "%dpx: reads back exactly (got \"%s\")", pixel_height,
                result.text);
            check(strcmp(result.text, SAMPLE) == 0, what);

            snprintf(what, sizeof(what), "%dpx: and refuses nothing",
                pixel_height);
            check(result.unrecognised == 0, what);
        }
    } else {
        /*
         * A test that the refusal fires is worth more than a test that the
         * reading succeeds. Anything can return an answer.
         */
        snprintf(what, sizeof(what),
            "%dpx: too small to read, and says so (confidence %d, %d of %d "
            "refused)", pixel_height, result.confidence, result.unrecognised,
            result.characters);
        check(!recon_ocr_result_is_a_reading(&result), what);
    }

    recon_ocr_result_free(&result);
}

/* --- What the face can and cannot tell apart --- */

static void test_confusion(struct recon_ocr_font *font) {
    printf("Telling shapes apart\n");

    check(recon_ocr_font_confusion(font, 20, 'x', 'x') == 1000,
        "a shape is identical to itself");

    /*
     * These two pairs are the design's central claim. Both are near-identical
     * outlines separated only by size and by where they sit, which is exactly
     * what a matcher that normalised every mark into a common box would throw
     * away before comparing.
     */
    int o_vs_cap = recon_ocr_font_confusion(font, 20, 'o', 'O');
    char what[128];
    snprintf(what, sizeof(what), "'o' and 'O' are different shapes (%d)",
        o_vs_cap);
    check(o_vs_cap < 985, what);

    int comma_vs_quote = recon_ocr_font_confusion(font, 20, ',', '\'');
    snprintf(what, sizeof(what),
        "a comma and an apostrophe are different shapes (%d)", comma_vs_quote);
    check(comma_vs_quote < 985, what);

    /*
     * Printed rather than asserted. Whether a face can tell an 'l' from an 'I'
     * at nine pixels is a property of that face, and asserting it would be
     * asserting whichever typeface this machine happens to have.
     */
    printf("    for information: 'l' against 'I' at 9px is %d, at 20px is %d\n",
        recon_ocr_font_confusion(font, 9, 'l', 'I'),
        recon_ocr_font_confusion(font, 20, 'l', 'I'));
}

/*
 * The sign of the baseline offset.
 *
 * A comma hangs below the line and an apostrophe sits above it, so the comma's
 * top row is the *lower* number. Get this backwards and every score stays
 * plausible while every comma reads as an apostrophe -- which is why it is
 * checked directly rather than left to be inferred from a transcription.
 */
static void test_baseline_sign(struct recon_ocr_font *font) {
    printf("Which way is down\n");

    int w = 0, h = 0, comma_top = 0, quote_top = 0;

    check(recon_ocr_font_render(font, 24, ',', NULL, &w, &h, &comma_top) > 0,
        "a comma renders");
    check(recon_ocr_font_render(font, 24, '\'', NULL, &w, &h, &quote_top) > 0,
        "an apostrophe renders");

    char what[128];
    snprintf(what, sizeof(what),
        "the comma sits lower than the apostrophe (%d against %d)",
        comma_top, quote_top);
    check(comma_top > quote_top, what);

    check(quote_top < 0, "and the apostrophe is above the baseline");
}

int main(void) {
    printf("OCR matching tests\n\n");

    test_overlap();

    struct recon_ocr_font *font = recon_ocr_font_find();
    if (font == NULL) {
        printf("\n%s\n", recon_ocr_match_last_error());
        printf("Skipping everything that needs one.\n");
        printf("\n%d checks, %d failures\n", g_checks, g_failures);
        return (g_failures == 0) ? 0 : 1;
    }

    /* The same file, for drawing the samples: the test has to render with the
     * same rasteriser the engine matches against, or it is testing whether one
     * rasteriser can read another. */
    const char *path = getenv("RECONOS_OCR_FONT");
    if (path == NULL || *path == '\0') {
        path = getenv("RECONOS_FONT");
    }

    static const char *const PLACES[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
        NULL,
    };

    unsigned char *data = NULL;
    for (int i = -1; PLACES[i + 1] != NULL || i < 0; i++) {
        const char *try = (i < 0) ? path : PLACES[i];
        if (try == NULL || *try == '\0') {
            continue;
        }
        FILE *f = fopen(try, "rb");
        if (f == NULL) {
            continue;
        }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        rewind(f);
        data = malloc((size_t)size);
        if (data != NULL && fread(data, 1, (size_t)size, f) != (size_t)size) {
            free(data);
            data = NULL;
        }
        fclose(f);
        if (data != NULL) {
            break;
        }
    }

    stbtt_fontinfo info;
    if (data == NULL ||
            !stbtt_InitFont(&info, data, stbtt_GetFontOffsetForIndex(data, 0))) {
        printf("\nfound a font to read with but not one to draw with; "
            "skipping the round trip\n");
        recon_ocr_font_close(font);
        printf("\n%d checks, %d failures\n", g_checks, g_failures);
        return (g_failures == 0) ? 0 : 1;
    }

    test_confusion(font);
    test_baseline_sign(font);

    printf("Reading back what was just written\n");
    round_trip(font, &info, 12, true, false);
    round_trip(font, &info, 16, true, false);
    round_trip(font, &info, 20, true, true);   /* clean marks: exact */
    round_trip(font, &info, 24, true, false);

    /* And the other direction: at five pixels there is not enough of a letter
     * left to be sure of, and saying so is the whole point. A test that the
     * refusal fires is worth more than a test that the reading succeeds --
     * anything can return an answer. */
    round_trip(font, &info, 5, false, false);

    recon_ocr_font_close(font);
    free(data);

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
