/*
 * Naming the marks: the half of reading text that needs a font.
 *
 * --- The idea, and the honest version of it ---
 *
 * ReconOS draws its own text, so it can draw the shapes it is trying to
 * recognise. That is the premise recon_ocr.h states, and it is worth being
 * precise about how far it goes.
 *
 * On the target case it goes all the way: a screenshot of a ReconOS window is
 * text this system rasterised, at a whole pixel size, at an integer advance,
 * with no hinting and no subpixel positioning. Rendering the same character at
 * the same size reproduces it almost exactly, and the residue is one row of
 * antialiasing either side of a threshold.
 *
 * Off that case it does not. Somebody else's rasteriser hints differently,
 * positions on fractions of a pixel, and may have been scaled after the fact by
 * a viewer fitting a picture to a window. Then this is ordinary template
 * matching with a good starting guess -- better than nothing and not the same
 * claim.
 *
 * So the design is built to *know which case it is in*. Every mark gets a score
 * against the shape it was named as and against the runner-up, and the engine
 * declines when either the fit is poor or the two are too close. That is why
 * confidence is the minimum of the two rather than a blend: a mark that matches
 * nothing is unread, and a mark that matches two things equally is also unread,
 * and averaging those with anything hides exactly the case worth reporting.
 *
 * --- Why this does not use struct recon_font ---
 *
 * That one belongs to the drawing layer, and recon_font_reload can change the
 * typeface underneath a holder at any moment with no notification and no
 * version counter. A matcher whose candidate shapes silently changed face
 * halfway down a page would keep scoring and keep answering. This opens its own
 * copy and holds it.
 *
 * It also keeps the matcher out of recon_ui.c, which cannot be linked without
 * a compositor -- so the whole engine stays testable on a machine with no
 * display.
 */

#ifndef RECON_OCR_MATCH_H
#define RECON_OCR_MATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "recon_ocr.h"

struct recon_ocr_font;

/*
 * Open a typeface to read with.
 *
 * `host_path` is a host path for fopen, not a path inside the ReconOS
 * filesystem. A caller holding a ReconOS path resolves it first.
 */
struct recon_ocr_font *recon_ocr_font_open(const char *host_path);

/* Borrows the bytes: they must outlive the font, because the rasteriser parses
 * out of them lazily rather than copying what it needs. */
struct recon_ocr_font *recon_ocr_font_open_memory(const unsigned char *data,
    size_t size);

/*
 * The first typeface this machine has, tried in the same order the drawing
 * layer tries them, with RECONOS_OCR_FONT and RECONOS_FONT ahead of the list.
 *
 * NULL on a machine with no font at all, which is a condition to handle rather
 * than a bug -- the same position recon_font_load takes.
 */
struct recon_ocr_font *recon_ocr_font_find(void);

void recon_ocr_font_close(struct recon_ocr_font *font);

const char *recon_ocr_match_last_error(void);

/*
 * Where to cut a rasterised glyph so it matches the page it is being compared
 * against: the coverage level, 0 to 255, at or above which a pixel is ink.
 *
 * recon_ocr_read works this out from the page and sets it, so most callers
 * never touch this. It is here because getting it wrong is invisible in the
 * output and obvious in the shapes: the same letter came out 9x10 from a page
 * and 7x9 from the rasteriser, and the scores said 850 where they should have
 * said a thousand.
 *
 * Changing it discards every rendered candidate, because they were all cut at
 * the old level.
 */
void recon_ocr_font_set_ink_cut(struct recon_ocr_font *font, int cut);
int recon_ocr_font_ink_cut(const struct recon_ocr_font *font);

/*
 * How alike two characters are in this face at this size, 0 to 1000.
 *
 * The engine's own answer to "can this typeface tell l from I at nine pixels".
 * At or above RECON_OCR_TWINS they are one shape here, and no mark will be
 * named either of them.
 *
 * Public because it is the design's central claim in a form a test can assert
 * with no picture involved: it turns "these two letters are hard" from folklore
 * into a measurement of this face at this size.
 */
int recon_ocr_font_confusion(struct recon_ocr_font *font, int pixel_height,
    uint32_t a, uint32_t b);

/*
 * One character's shape, as the matcher sees it.
 *
 * `*top` is the row offset from the baseline, positive downward -- so it is
 * negative for nearly everything, and positive only for the parts of a comma
 * or a semicolon that hang below. Getting that sign backwards produces
 * plausible scores everywhere and reads every comma as an apostrophe, so it is
 * exposed to be looked at rather than reasoned about.
 *
 * Returns the bytes needed, or -1. Pass mask = NULL to ask for the size.
 */
int recon_ocr_font_render(struct recon_ocr_font *font, int pixel_height,
    uint32_t codepoint, unsigned char *mask, int *width, int *height,
    int *top);

/*
 * Where a line's text sits, and how big it is.
 *
 * Both are properties of the line rather than of any mark in it, because no
 * single mark contains either: an 'o' does not know where the baseline is and a
 * 'T' does not know the x-height. So they are fitted from all the marks at
 * once, by trying sizes and keeping the one that fits best.
 */
struct recon_ocr_fit {
    int pixel_height;   /* what the rasteriser was asked for */
    int baseline;       /* image row: the first row *below* a flat-bottomed
                         * glyph, so mark.bottom == baseline for an 'n' */
    int x_height;       /* this font's 'x' at that size, in pixels */
    int quality;        /* 0-1000: how well the fit actually matched */
};

/*
 * Fit one line.
 *
 * `hint` is the previous line's accepted fit, or NULL. Passing it turns a
 * thirty-five trial search into a nine-trial one, which is what makes a page of
 * text cost about what one line costs -- the lines of a page are almost always
 * the same size.
 *
 * False means the line could not be fitted at any size: too small to read, or
 * not text in this face. The honest response is a line of unrecognised marks
 * rather than a second attempt with the rules loosened.
 */
bool recon_ocr_fit_line(struct recon_ocr_font *font,
    const unsigned char *ink, int width, int height,
    const struct recon_ocr_mark *marks, int mark_count,
    const struct recon_ocr_fit *hint, struct recon_ocr_fit *out);

/*
 * What one mark was read as, and what it nearly was.
 *
 * The runner-up is carried because the gap between the two is most of what
 * confidence means. A shape that scored 0.95 against 'e' and 0.94 against 'c'
 * has not been read, however good 0.95 sounds.
 */
struct recon_ocr_match {
    uint32_t codepoint;         /* 0 when nothing was good enough */
    uint32_t runner_up;         /* 0 when nothing else survived */
    int score;                  /* 0-1000 */
    int runner_up_score;        /* 0-1000 */
    int confidence;             /* 0-100 */
};

/*
 * Name every mark, at a size and baseline already fitted.
 *
 * Never rescales or reseats an individual mark. The fit is the fit: letting
 * each mark find its own best size is what turns a reader into a thing that
 * finds a letter in any shape it is given.
 *
 * Returns how many matches were written, or -1.
 */
int recon_ocr_match_marks(struct recon_ocr_font *font,
    const unsigned char *ink, int width, int height,
    const struct recon_ocr_fit *fit,
    const struct recon_ocr_mark *marks, int mark_count,
    struct recon_ocr_match *out);

/*
 * All five stages, on a whole picture.
 *
 * False only when it could not begin -- no picture, no font, no memory, or
 * nothing in the image that looks like a line of text. Everything else comes
 * back as a result whose own numbers say how much of it to believe, because a
 * partly-read page is a real thing and reporting it as failure loses the part
 * that was read.
 *
 * A mark that could not be named appears in the text as U+FFFD and is counted
 * in `unrecognised`. Not '?', which is a character that could have been read --
 * emitting a real character to mean "I do not know" is the same lie in
 * miniature that this engine exists to avoid.
 */
bool recon_ocr_read(const unsigned char *rgba, int width, int height,
    struct recon_ocr_font *font, struct recon_ocr_result *out);

void recon_ocr_result_free(struct recon_ocr_result *result);

/*
 * Whether that was a reading or a guess.
 *
 * Confidence of at least 60 and fewer than half the marks refused -- the two
 * thresholds recon_ocr.h names, in one place rather than in every caller.
 */
bool recon_ocr_result_is_a_reading(const struct recon_ocr_result *result);

#endif /* RECON_OCR_MATCH_H */
