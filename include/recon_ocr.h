/*
 * Reading text out of a picture.
 *
 * --- Why this is worth having here, more than elsewhere ---
 *
 * ReconOS has no word processor and cannot yet run anybody else's. So a
 * screenshot of a document, a photograph of a page, a diagram with a label in
 * it -- all of them are currently text that cannot be got at. Elsewhere this is
 * a convenience. Here it is the difference between a picture being something
 * you look at and something you can work with.
 *
 * --- Why this is not a research project ---
 *
 * General optical character recognition is: photographs, perspective,
 * handwriting, unknown typefaces, degraded scans. That is a field, not a file.
 *
 * But most of what somebody actually wants read is not that. It is a screenshot
 * of a window, a saved page, a scan of an ordinary printed document -- text
 * that was *rendered* rather than written, in a face not far from one the
 * system already has, on a flat background, the right way up.
 *
 * And ReconOS is in an unusually good position for that, because it draws its
 * own text. It has a font rasterizer and it knows what its own glyphs look like
 * at any size. So it can produce the shapes it is trying to recognise, which
 * turns the hard half of the problem -- "what letter is this" -- into a
 * comparison against shapes it can generate on demand.
 *
 * That is the whole idea, and it is also the whole limit. What this can read is
 * text whose shapes are close to shapes the system can draw. Everything else,
 * it should decline to guess at rather than return confident nonsense, because
 * a wrong transcription that looks right is worse than no transcription: one is
 * a tool that failed and the other is a tool that lied.
 *
 * --- The stages ---
 *
 *   1. Ink and paper       decide which pixels are text and which are not
 *   2. Lines               find the horizontal bands the text sits in
 *   3. Glyphs              split each band into separate marks
 *   4. Matching            compare each mark against rendered candidates
 *   5. Spaces and breaks   turn a list of letters back into words and lines
 *
 * The first three are arithmetic on a bitmap and need no font at all, which is
 * why they are testable on their own and are tested that way.
 */

#ifndef RECON_OCR_H
#define RECON_OCR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * What came out, and how much of it to believe.
 *
 * `confidence` is 0 to 100 and is not decoration. This engine can be handed a
 * photograph of a street sign and will find *something* -- marks that are the
 * right size and roughly the right shape -- and every character of it may be
 * wrong. The number is how well the marks actually matched the shapes they were
 * compared against, and anything under about 60 should be shown as a guess
 * rather than as a reading.
 */
struct recon_ocr_result {
    char *text;             /* owned; NUL-terminated */
    size_t length;

    int confidence;         /* 0-100 */
    int lines;
    int characters;

    /*
     * Marks that were found and matched nothing well enough to name.
     *
     * Reported rather than silently dropped, because the ratio of these to
     * `characters` is the honest signal that a picture is outside what this can
     * read -- more so than the confidence, which averages.
     */
    int unrecognised;
};

/*
 * --- What is here, and what is not ---
 *
 * The first three stages are written and tested: ink, lines, marks. They are
 * the half that is arithmetic on a bitmap, they need no font, and they are
 * exposed below because being testable without a display is worth more than
 * being tidy.
 *
 * The matching is not written. So there is deliberately no `recon_ocr_read`
 * yet: a whole-picture entry point that found marks and could name none of them
 * would be a function that works and returns nothing, which is a worse thing to
 * ship than a header that says which half is finished.
 *
 * `struct recon_ocr_result` is here because it is the shape the answer will
 * take, and because `confidence` and `unrecognised` are the part of the design
 * worth settling first rather than last. A reader that cannot say "I am
 * guessing" is a reader that lies on every picture it was not built for.
 */

const char *recon_ocr_last_error(void);

/* --- The stages, exposed because they are testable and worth testing --- */

/*
 * Ink and paper.
 *
 * Fills `ink` with one byte per pixel: non-zero where the pixel is text. The
 * caller owns it and it is width * height bytes.
 *
 * The threshold is chosen from the image rather than fixed, because a fixed one
 * is right for exactly the brightness it was picked at. It also decides which
 * way round the page is: white text on black is as common as black on white in
 * a screenshot, and getting that backwards finds the gaps instead of the
 * letters -- which produces marks, and lines, and a confident answer made
 * entirely of holes.
 */
bool recon_ocr_ink(const unsigned char *rgba, int width, int height,
    unsigned char *ink);

/* A horizontal band that text sits in. */
struct recon_ocr_line {
    int top, bottom;        /* rows, inclusive of top and exclusive of bottom */
    int left, right;        /* the ink's extent within those rows */
};

/*
 * Find the bands. Returns how many were found, up to `max`.
 *
 * By where the ink is, row by row: a row with ink is inside a line and a row
 * without is between two. Simple, and it is what makes the whole approach
 * depend on the text being the right way up -- which is a limit worth having
 * stated rather than discovered.
 */
int recon_ocr_lines(const unsigned char *ink, int width, int height,
    struct recon_ocr_line *out, int max);

/* One mark: a connected run of ink within a line. */
struct recon_ocr_mark {
    int left, right;        /* columns, right exclusive */
    int top, bottom;        /* trimmed to this mark's own ink */
    bool space_before;      /* a gap wide enough to be a word break */
};

/*
 * Split a line into marks. Returns how many, up to `max`.
 *
 * Columns rather than connected components, which is a real simplification and
 * a real limit: it splits touching letters wrongly and joins overlapping ones.
 * Rendered text at ordinary sizes rarely does either, and the cases where it
 * does -- italics, tight kerning, ligatures -- are exactly the cases this is
 * honest about not reading well.
 */
int recon_ocr_marks(const unsigned char *ink, int width,
    const struct recon_ocr_line *line, struct recon_ocr_mark *out, int max);

#endif /* RECON_OCR_H */
