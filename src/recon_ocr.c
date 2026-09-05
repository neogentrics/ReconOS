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
    return recon_ocr_ink_detail(rgba, width, height, ink, NULL);
}

bool recon_ocr_ink_detail(const unsigned char *rgba, int width, int height,
        unsigned char *ink, struct recon_ocr_polarity *out) {
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

    if (out != NULL) {
        out->threshold = threshold;
        out->ink_is_dark = ink_is_dark;
    }
    return true;
}


/* --- Blocks --- */

/*
 * How wide a run of blank has to be before it separates two blocks.
 *
 * Proportional to the piece being cut, with a floor. That is the whole trick,
 * and a fixed number of pixels cannot do the job: the gap between two words at
 * forty point is wider than the gap between two paragraphs at eight, so any
 * constant either splits words somewhere or fails to split paragraphs
 * elsewhere.
 *
 * A twentieth means a cut has to be a real piece of the space available. On a
 * 1280-wide screen that is 64 pixels, which separates windows and leaves their
 * insides alone; inside a 400-wide block of text it is 20, which is wider than
 * any word space at any size that block could be holding. The rule tightens as
 * the recursion goes deeper, which is exactly the direction it needs to.
 */
static int cut_needed(int extent) {
    int needed = extent / 20;
    return needed < 6 ? 6 : needed;
}

/* How much ink is in this row, within these columns? */
static int row_ink(const unsigned char *ink, int width,
        const struct recon_ocr_region *at, int y) {
    const unsigned char *row = ink + (size_t)y * width;
    int seen = 0;
    for (int x = at->left; x < at->right; x++) {
        seen += row[x] != 0;
    }
    return seen;
}

static int column_ink(const unsigned char *ink, int width,
        const struct recon_ocr_region *at, int x) {
    int seen = 0;
    for (int y = at->top; y < at->bottom; y++) {
        seen += ink[(size_t)y * width + x] != 0;
    }
    return seen;
}

static bool row_has_ink(const unsigned char *ink, int width,
        const struct recon_ocr_region *at, int y) {
    return row_ink(ink, width, at, y) > 0;
}

static bool column_has_ink(const unsigned char *ink, int width,
        const struct recon_ocr_region *at, int x) {
    return column_ink(ink, width, at, x) > 0;
}

/*
 * A block big enough that a full-span line in it means a frame rather than a
 * letter.
 *
 * At the bottom of the recursion a block is one line of writing, and there a
 * tall letter genuinely is inked across almost the whole height -- an 'l' at
 * thirty point spans its line the way a border spans a window. Cutting on that
 * would split a line at every ascender.
 *
 * Forty-eight pixels is taller than any single line this engine can read and a
 * small fraction of any window, so the two cases do not meet.
 */
#define DIVIDER_MIN_EXTENT 48

/*
 * Nearly all of it, not all of it.
 *
 * A window border is interrupted where another window overlaps it, and a
 * printed rule loses a pixel to the paper. Demanding every row would find
 * neither.
 */
static bool spans(int inked, int extent) {
    return extent >= DIVIDER_MIN_EXTENT && inked * 20 >= extent * 19;
}

/*
 * Shrink a block to the ink actually in it.
 *
 * Done before every cut, so that the proportional threshold is measured
 * against the writing rather than against the margin around it. Without this a
 * block with a wide empty border keeps demanding wider cuts than its contents
 * ever contain, and stops splitting one level too early.
 */
static bool tighten(const unsigned char *ink, int width,
        struct recon_ocr_region *at) {
    /*
     * Blank margins and drawn borders, from every edge, until nothing moves.
     *
     * A border is not a margin but it is not content either, and at an edge it
     * is not a cut: a run at the end of a block has nothing on its far side to
     * separate from. Left attached, it is a column of ink on every row of the
     * block, and no line can ever be found inside it.
     *
     * Repeated rather than done once because each strip changes the extent the
     * next edge is judged against -- and because a border two pixels thick is
     * two passes.
     */
    bool moved = true;
    while (moved) {
        moved = false;

        if (at->top >= at->bottom || at->left >= at->right) {
            return false;
        }

        int across = at->right - at->left;
        int down = at->bottom - at->top;

        int ink_at = row_ink(ink, width, at, at->top);
        if (ink_at == 0 || spans(ink_at, across)) {
            at->top++;
            moved = true;
            continue;
        }
        ink_at = row_ink(ink, width, at, at->bottom - 1);
        if (ink_at == 0 || spans(ink_at, across)) {
            at->bottom--;
            moved = true;
            continue;
        }
        ink_at = column_ink(ink, width, at, at->left);
        if (ink_at == 0 || spans(ink_at, down)) {
            at->left++;
            moved = true;
            continue;
        }
        ink_at = column_ink(ink, width, at, at->right - 1);
        if (ink_at == 0 || spans(ink_at, down)) {
            at->right--;
            moved = true;
        }
    }
    return at->left < at->right && at->top < at->bottom;
}

/*
 * Split a block on the widest run of blank, one axis at a time.
 *
 * Rows first, then columns, then rows again on each piece -- which is what
 * takes a screen apart in the order a person would describe it: into windows,
 * then into the parts of a window, then into paragraphs.
 *
 * `depth` is a stop rather than a policy. Twelve levels is far more than any
 * real layout needs and far fewer than a picture of noise could demand.
 */
static void split_block(const unsigned char *ink, int width,
        struct recon_ocr_region at, bool try_rows, int depth,
        struct recon_ocr_region *out, int *count, int max) {
    if (*count >= max || depth <= 0) {
        return;
    }
    if (!tighten(ink, width, &at)) {
        return;                     /* nothing in it */
    }

    int extent = try_rows ? (at.bottom - at.top) : (at.right - at.left);
    int needed = cut_needed(extent);

    /*
     * Every gap wide enough, not just the widest.
     *
     * Cutting only at the largest would need a pass per piece and would take
     * a column of ten paragraphs apart in ten passes. Cutting at all of them
     * at once is one pass and the same answer.
     */
    int pieces = 0;
    int run = 0;
    int piece_start = try_rows ? at.top : at.left;
    int limit = try_rows ? at.bottom : at.right;

    int across = try_rows ? (at.right - at.left) : (at.bottom - at.top);
    bool run_has_divider = false;

    for (int i = piece_start; i <= limit; i++) {
        bool blank;
        bool divider = false;

        if (i == limit) {
            blank = true;           /* the end closes the last piece */
        } else {
            int inked = try_rows ? row_ink(ink, width, &at, i)
                                 : column_ink(ink, width, &at, i);
            blank = inked == 0;
            divider = !blank && spans(inked, across);
        }

        if (blank || divider) {
            run++;
            run_has_divider = run_has_divider || divider;
            continue;
        }

        /*
         * A run separates two pieces when it is wide enough to be a gap in the
         * layout, or when it contains a line drawn across the whole block. The
         * second is what gets inside a window: its border is one column of ink
         * on every row, so no run of blank will ever be wide enough there.
         */
        if ((run >= needed || run_has_divider) && i - run > piece_start) {
            struct recon_ocr_region piece = at;
            if (try_rows) {
                piece.top = piece_start;
                piece.bottom = i - run;
            } else {
                piece.left = piece_start;
                piece.right = i - run;
            }
            split_block(ink, width, piece, !try_rows, depth - 1, out, count,
                max);
            pieces++;
            piece_start = i;
        } else if (run > 0 && i - run == piece_start) {
            piece_start = i;        /* leading blank, not a cut */
        }
        run = 0;
        run_has_divider = false;
    }

    /* The tail, and the case where nothing was wide enough to cut at. */
    if (pieces > 0) {
        struct recon_ocr_region piece = at;
        if (try_rows) {
            piece.top = piece_start;
        } else {
            piece.left = piece_start;
        }
        if ((try_rows ? piece.bottom - piece.top : piece.right - piece.left)
                > 0) {
            split_block(ink, width, piece, !try_rows, depth - 1, out, count,
                max);
        }
        return;
    }

    /*
     * No cut on this axis. Try the other one before giving up -- a block of
     * text has no wide gaps between its lines and plenty between its columns,
     * or the reverse, and stopping at the first axis that fails would hand
     * back the whole thing.
     */
    if (try_rows) {
        split_block(ink, width, at, false, depth - 1, out, count, max);
        return;
    }

    if (*count < max) {
        out[(*count)++] = at;
    }
}

int recon_ocr_regions(const unsigned char *ink, int width, int height,
        struct recon_ocr_region *out, int max) {
    if (ink == NULL || out == NULL || width <= 0 || height <= 0 || max <= 0) {
        return 0;
    }

    struct recon_ocr_region whole = { 0, 0, width, height };
    int count = 0;
    split_block(ink, width, whole, true, 12, out, &count, max);
    return count;
}

/* --- Lines --- */

int recon_ocr_lines(const unsigned char *ink, int width, int height,
        struct recon_ocr_line *out, int max) {
    struct recon_ocr_region whole = { 0, 0, width, height };
    return recon_ocr_lines_in(ink, width, height, &whole, out, max);
}

int recon_ocr_lines_in(const unsigned char *ink, int width, int height,
        const struct recon_ocr_region *region, struct recon_ocr_line *out,
        int max) {
    if (ink == NULL || out == NULL || region == NULL || width <= 0 ||
            height <= 0 || max <= 0) {
        return 0;
    }
    if (region->left < 0 || region->top < 0 || region->right > width ||
            region->bottom > height || region->left >= region->right ||
            region->top >= region->bottom) {
        return 0;
    }

    int found = 0;
    int top = -1;

    for (int y = region->top; y <= region->bottom; y++) {
        /*
         * One row past the bottom, deliberately, and treated as empty. It
         * closes a line that runs to the last row of the image -- which is the
         * commonest single line there is, because it is what a tightly cropped
         * screenshot of one line of text looks like.
         */
        bool has_ink = false;
        if (y < region->bottom) {
            const unsigned char *row = ink + (size_t)y * width;
            for (int x = region->left; x < region->right && !has_ink; x++) {
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
                line->left = region->right;
                line->right = region->left;

                /* The ink's own extent, so a centred line is not measured from
                 * the block's edge. */
                for (int row_y = top; row_y < y; row_y++) {
                    const unsigned char *row = ink + (size_t)row_y * width;
                    for (int x = region->left; x < region->right; x++) {
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

/* --- Comparing two shapes --- */

int recon_ocr_mask_overlap(const unsigned char *a, int a_width, int a_height,
        const unsigned char *b, int b_width, int b_height, int dx, int dy) {
    if (a == NULL || b == NULL || a_width <= 0 || a_height <= 0 ||
            b_width <= 0 || b_height <= 0) {
        return 0;
    }

    long area_a = 0, area_b = 0, shared = 0;
    for (long i = 0; i < (long)a_width * a_height; i++) {
        area_a += a[i] != 0;
    }
    for (long i = 0; i < (long)b_width * b_height; i++) {
        area_b += b[i] != 0;
    }
    if (area_a == 0 || area_b == 0) {
        return 0;
    }

    /*
     * Where the two rectangles actually meet, in b's coordinates. When they do
     * not meet at all these bounds cross and the loop runs zero times, which
     * is the right answer arrived at without a special case.
     */
    int x0 = (-dx > 0) ? -dx : 0;
    int x1 = (b_width < a_width - dx) ? b_width : a_width - dx;
    int y0 = (-dy > 0) ? -dy : 0;
    int y1 = (b_height < a_height - dy) ? b_height : a_height - dy;

    for (int by = y0; by < y1; by++) {
        const unsigned char *brow = b + (long)by * b_width;
        const unsigned char *arow = a + (long)(by + dy) * a_width + dx;
        for (int bx = x0; bx < x1; bx++) {
            shared += (brow[bx] != 0) && (arow[bx] != 0);
        }
    }

    /*
     * Union by inclusion and exclusion rather than by walking a bounding box
     * around both. It is exact, it is two counts already taken, and it does
     * not need the two masks to be in a common frame.
     */
    long covered = area_a + area_b - shared;
    if (covered <= 0) {
        return 0;
    }
    return (int)((1000 * shared + covered / 2) / covered);
}

/* --- Marks --- */

static int compare_ints(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

/*
 * How wide a gap has to be before it separates two words.
 *
 * Measured from the line's own gaps rather than from its height. In rendered
 * text the gaps fall into two populations -- between letters and between words
 * -- and letter gaps are the large majority, so the median gap *is* a letter
 * gap and a word gap is a multiple of it.
 *
 * When the widest gap is not much bigger than the median, the line has no word
 * breaks: that is the right answer for "0123456789" and it is one that no fixed
 * threshold can give, because a threshold that admits a word break in one line
 * invents them in another.
 *
 * The height-based fallback is for a line with too few gaps to have a
 * distribution. Two marks give one gap, and one number is not a population.
 */
static int space_threshold(const int *gaps, int count, int line_height) {
    if (count < 3) {
        int fallback = line_height / 3;
        return fallback < 3 ? 3 : fallback;
    }

    int sorted[256];
    int n = count < 256 ? count : 256;
    memcpy(sorted, gaps, (size_t)n * sizeof(int));
    qsort(sorted, (size_t)n, sizeof(int), compare_ints);

    int median = sorted[n / 2];
    int widest = sorted[n - 1];

    if (median < 1) {
        median = 1;
    }

    /*
     * Twice the median is the line between "these letters are close together"
     * and "something deliberate happened here". Below it, treat the line as one
     * unbroken run: a line whose widest gap is barely wider than its typical
     * one is a line with no spaces in it.
     */
    /*
     * And it has to be wider by an absolute amount as well as a proportional
     * one. At nine-pixel text the gaps are one and two pixels: two is twice
     * one, so the proportional test alone calls every two-pixel gap a word
     * break, and a clock reads "9/5/2 0 2 6". One pixel of difference is not
     * evidence of anything at any size.
     */
    if (widest < median * 2 || widest - median < 2) {
        return widest + 1;          /* nothing reaches this */
    }

    /*
     * And a floor, because a distribution can be real and still be noise.
     *
     * At small sizes the gaps are one and two pixels, and the difference
     * between a letter gap and a word gap is smaller than the difference
     * between two letter gaps -- so the rule above finds a word break between
     * every character. A clock reading "9/5/2026" comes out as
     * "9 / 5 / 2 0 2 6".
     *
     * A quarter of the band's height is narrower than any word space and wider
     * than any gap between two letters. That is what the original threshold
     * measured, and it was right about the quantity and wrong to be the only
     * thing consulted.
     */
    int decided = (median + widest) / 2;
    int floor_needed = line_height / 4;

    return decided > floor_needed ? decided : floor_needed;
}

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
     * Two passes, because the width of a word break cannot be known until the
     * marks are known. The first pass finds them and records the gap before
     * each; the second decides which of those gaps were spaces.
     */
    int gaps[256];
    int gap_count = 0;

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

                /* Recorded now, judged later. The gap is kept in the flag's
                 * field until the second pass turns it into a decision. */
                mark->space_before = false;
                if (previous_end >= 0) {
                    int gap = start - previous_end;
                    if (gap_count < 256) {
                        gaps[gap_count++] = gap;
                    }
                }

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

    /* --- Which of those gaps were words --- */

    int threshold = space_threshold(gaps, gap_count,
        line->bottom - line->top);

    for (int i = 1; i < found; i++) {
        int gap = out[i].left - out[i - 1].right;
        out[i].space_before = gap >= threshold;
    }
    return found;
}
