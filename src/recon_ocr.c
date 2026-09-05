/*
 * Reading text out of a picture. See include/recon_ocr.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_ocr.h"

static char g_error[192];

static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void fail(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, args);
    va_end(args);
}

const char *recon_ocr_last_error(void) {
    return g_error;
}

/* --- Ink and paper --- */

/*
 * How light a pixel is, weighted the way an eye weights it.
 *
 * The same weights recon_color_luminance uses, and repeated here rather than
 * shared because that one takes a packed colour and this takes bytes out of an
 * array -- and the alternative to two short functions is one that packs a
 * colour so the other can unpack it, four million times a page.
 */
static inline int brightness(const unsigned char *px) {
    return (306 * px[0] + 601 * px[1] + 117 * px[2]) >> 10;
}

/*
 * The threshold that best separates two groups of pixels, by Otsu's method.
 *
 * The idea is small: try every threshold, and keep the one where the two groups
 * it makes are each as tightly clustered as possible. It is chosen from the
 * image rather than fixed because a fixed threshold is correct for exactly the
 * brightness it was picked at -- a scan is not a screenshot, and neither is a
 * photograph of a screen.
 *
 * Written out rather than borrowed. It is a histogram and a running sum, which
 * is squarely the kind of arithmetic this project writes for itself.
 */
static int otsu(const int *histogram, long total) {
    long sum = 0;
    for (int i = 0; i < 256; i++) {
        sum += (long)i * histogram[i];
    }

    long below_weight = 0;
    long below_sum = 0;
    double best = -1.0;
    int threshold = 128;

    for (int t = 0; t < 256; t++) {
        below_weight += histogram[t];
        if (below_weight == 0) {
            continue;
        }
        long above_weight = total - below_weight;
        if (above_weight == 0) {
            break;
        }

        below_sum += (long)t * histogram[t];

        double below_mean = (double)below_sum / (double)below_weight;
        double above_mean = (double)(sum - below_sum) / (double)above_weight;
        double gap = below_mean - above_mean;

        /* The variance *between* the two groups. Maximising it is the same as
         * minimising the variance within them, and is one line rather than
         * two passes. */
        double score = (double)below_weight * (double)above_weight * gap * gap;
        if (score > best) {
            best = score;
            threshold = t;
        }
    }
    return threshold;
}

bool recon_ocr_ink(const unsigned char *rgba, int width, int height,
        unsigned char *ink) {
    if (rgba == NULL || ink == NULL || width <= 0 || height <= 0) {
        fail("There is no picture to read.");
        return false;
    }

    int histogram[256];
    memset(histogram, 0, sizeof(histogram));

    long count = (long)width * height;
    for (long i = 0; i < count; i++) {
        histogram[brightness(rgba + i * 4)]++;
    }

    int threshold = otsu(histogram, count);

    /*
     * Which side is the ink.
     *
     * Text is the minority of a page: a printed line covers well under half the
     * pixels of the band it sits in, and far less of a whole page. So whichever
     * side of the threshold holds fewer pixels is the writing.
     *
     * This matters more than it looks. White text on black is as ordinary as
     * black on white in a screenshot, and getting it backwards does not fail --
     * it finds the *gaps* between letters, which are marks, in rows, of
     * plausible size. The result is a confident answer made entirely of holes.
     */
    long darker = 0;
    for (int i = 0; i <= threshold; i++) {
        darker += histogram[i];
    }
    bool ink_is_dark = darker <= count - darker;

    for (long i = 0; i < count; i++) {
        int v = brightness(rgba + i * 4);
        bool is_ink = ink_is_dark ? (v <= threshold) : (v > threshold);

        /*
         * A transparent pixel is not ink whichever way round the page is.
         * Without this an icon on a transparent background reads as a page of
         * solid text, because whatever is in the unused channels wins the
         * brightness test.
         */
        if (rgba[i * 4 + 3] < 128) {
            is_ink = false;
        }
        ink[i] = is_ink ? 1 : 0;
    }
    return true;
}

/* --- Lines --- */

int recon_ocr_lines(const unsigned char *ink, int width, int height,
        struct recon_ocr_line *out, int max) {
    if (ink == NULL || out == NULL || width <= 0 || height <= 0 || max <= 0) {
        return 0;
    }

    int found = 0;
    int top = -1;

    for (int y = 0; y <= height; y++) {
        /*
         * One row past the bottom, deliberately, and treated as empty. It
         * closes a line that runs to the last row of the image -- which is the
         * commonest single line there is, because it is what a tightly cropped
         * screenshot of one line of text looks like.
         */
        bool has_ink = false;
        if (y < height) {
            const unsigned char *row = ink + (size_t)y * width;
            for (int x = 0; x < width && !has_ink; x++) {
                has_ink = row[x] != 0;
            }
        }

        if (has_ink && top < 0) {
            top = y;
        } else if (!has_ink && top >= 0) {
            if (found < max) {
                struct recon_ocr_line *line = &out[found];
                line->top = top;
                line->bottom = y;
                line->left = width;
                line->right = 0;

                /* The ink's own extent, so a centred line is not measured from
                 * the page edge. */
                for (int row_y = top; row_y < y; row_y++) {
                    const unsigned char *row = ink + (size_t)row_y * width;
                    for (int x = 0; x < width; x++) {
                        if (row[x] == 0) {
                            continue;
                        }
                        if (x < line->left) { line->left = x; }
                        if (x + 1 > line->right) { line->right = x + 1; }
                    }
                }
                found++;
            }
            top = -1;
        }
    }
    return found;
}

/* --- Marks --- */

int recon_ocr_marks(const unsigned char *ink, int width,
        const struct recon_ocr_line *line, struct recon_ocr_mark *out,
        int max) {
    if (ink == NULL || line == NULL || out == NULL || max <= 0) {
        return 0;
    }
    if (line->right <= line->left || line->bottom <= line->top) {
        return 0;
    }

    int found = 0;
    int start = -1;
    int previous_end = -1;

    /*
     * The width a gap has to reach before it is a space rather than the
     * ordinary distance between two letters.
     *
     * Taken from the line's own height rather than fixed, because the only
     * thing here that scales with the text is the text. A quarter of the
     * height is comfortably wider than inter-letter spacing at every size this
     * can read and comfortably narrower than a word space.
     */
    int line_height = line->bottom - line->top;
    int space_gap = line_height / 4;
    if (space_gap < 2) {
        space_gap = 2;
    }

    for (int x = line->left; x <= line->right; x++) {
        bool has_ink = false;
        if (x < line->right) {
            for (int y = line->top; y < line->bottom && !has_ink; y++) {
                has_ink = ink[(size_t)y * width + x] != 0;
            }
        }

        if (has_ink && start < 0) {
            start = x;
        } else if (!has_ink && start >= 0) {
            if (found < max) {
                struct recon_ocr_mark *mark = &out[found];
                mark->left = start;
                mark->right = x;
                mark->space_before = previous_end >= 0 &&
                    (start - previous_end) >= space_gap;

                /*
                 * Trimmed to this mark's own rows, not the line's.
                 *
                 * Where a mark sits within the line is most of what tells a
                 * comma from an apostrophe and an 'o' from a '0' -- they are
                 * near-identical shapes at different heights. Keeping the
                 * line's own top and bottom would throw that away before
                 * anything had a chance to use it.
                 */
                mark->top = line->bottom;
                mark->bottom = line->top;
                for (int y = line->top; y < line->bottom; y++) {
                    for (int mx = start; mx < x; mx++) {
                        if (ink[(size_t)y * width + mx] == 0) {
                            continue;
                        }
                        if (y < mark->top) { mark->top = y; }
                        if (y + 1 > mark->bottom) { mark->bottom = y + 1; }
                    }
                }
                found++;
            }
            previous_end = x;
            start = -1;
        }
    }
    return found;
}
