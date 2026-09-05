/*
 * Tests for the part of reading text that is arithmetic on a bitmap.
 *
 * Deciding which pixels are ink, finding the bands text sits in, and splitting
 * a band into marks are all done before any font is involved -- which is what
 * makes them testable without one, and is why they are a separate file from the
 * matching.
 *
 * The images here are built by hand, one rectangle at a time, so that what the
 * answer should be is known rather than inspected. An OCR test that runs on a
 * photograph can only be checked by reading its output, which means the test
 * passes when the output looks plausible.
 *
 * Run with: cmake --build build && ./build/recon_ocr_tests
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_ocr.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
    }
}

/* --- A page to draw on --- */

#define PAGE_W 200
#define PAGE_H 100

struct page {
    unsigned char rgba[PAGE_W * PAGE_H * 4];
    unsigned char ink[PAGE_W * PAGE_H];
};

static void paper(struct page *p, int shade) {
    for (int i = 0; i < PAGE_W * PAGE_H; i++) {
        p->rgba[i * 4 + 0] = (unsigned char)shade;
        p->rgba[i * 4 + 1] = (unsigned char)shade;
        p->rgba[i * 4 + 2] = (unsigned char)shade;
        p->rgba[i * 4 + 3] = 255;
    }
}

static void blot(struct page *p, int x, int y, int w, int h, int shade) {
    for (int row = y; row < y + h && row < PAGE_H; row++) {
        for (int col = x; col < x + w && col < PAGE_W; col++) {
            int i = row * PAGE_W + col;
            p->rgba[i * 4 + 0] = (unsigned char)shade;
            p->rgba[i * 4 + 1] = (unsigned char)shade;
            p->rgba[i * 4 + 2] = (unsigned char)shade;
        }
    }
}

/* --- Ink and paper --- */

static void test_ink(void) {
    printf("Ink and paper\n");

    struct page p;

    /* Dark marks on a light page: the ordinary case. */
    paper(&p, 240);
    blot(&p, 10, 10, 20, 20, 30);
    check(recon_ocr_ink(p.rgba, PAGE_W, PAGE_H, p.ink), "a light page reads");
    check(p.ink[15 * PAGE_W + 15] != 0, "the mark is ink");
    check(p.ink[50 * PAGE_W + 100] == 0, "the paper is not");

    /*
     * And the same page inverted, which is the case that decides whether this
     * works on a screenshot at all. Light text on a dark background is as
     * ordinary as the other way round, and reading it backwards does not fail:
     * it finds the gaps between the letters, which are marks, in rows, of
     * plausible size, and produces a confident answer made of holes.
     */
    paper(&p, 20);
    blot(&p, 10, 10, 20, 20, 230);
    check(recon_ocr_ink(p.rgba, PAGE_W, PAGE_H, p.ink), "a dark page reads");
    check(p.ink[15 * PAGE_W + 15] != 0, "the light mark is the ink");
    check(p.ink[50 * PAGE_W + 100] == 0, "the dark page is not");

    /*
     * A transparent image. Without the alpha check this reads as a page of
     * solid text, because whatever is in the unused colour channels wins the
     * brightness comparison -- which is what an icon looks like.
     */
    paper(&p, 240);
    for (int i = 0; i < PAGE_W * PAGE_H; i++) {
        p.rgba[i * 4 + 3] = 0;
    }
    blot(&p, 10, 10, 20, 20, 30);
    for (int row = 10; row < 30; row++) {
        for (int col = 10; col < 30; col++) {
            p.rgba[(row * PAGE_W + col) * 4 + 3] = 255;
        }
    }
    check(recon_ocr_ink(p.rgba, PAGE_W, PAGE_H, p.ink), "a cut-out reads");
    check(p.ink[15 * PAGE_W + 15] != 0, "what is there is ink");
    check(p.ink[50 * PAGE_W + 100] == 0, "what is transparent is not");

    check(!recon_ocr_ink(NULL, PAGE_W, PAGE_H, p.ink), "no picture is refused");
    check(!recon_ocr_ink(p.rgba, 0, PAGE_H, p.ink), "no width is refused");
}

/* --- Lines --- */

static void test_lines(void) {
    printf("Lines\n");

    struct page p;
    struct recon_ocr_line lines[8];

    /* Three bands with clear gaps. */
    paper(&p, 240);
    blot(&p, 20, 10, 100, 8, 20);
    blot(&p, 20, 30, 140, 8, 20);
    blot(&p, 20, 50, 60, 8, 20);
    recon_ocr_ink(p.rgba, PAGE_W, PAGE_H, p.ink);

    int found = recon_ocr_lines(p.ink, PAGE_W, PAGE_H, lines, 8);
    check(found == 3, "three bands are three lines");

    if (found == 3) {
        check(lines[0].top == 10 && lines[0].bottom == 18,
            "the first band's rows");
        check(lines[1].top == 30 && lines[1].bottom == 38,
            "the second band's rows");

        /* The ink's own extent, so a short line is short. */
        check(lines[2].left == 20 && lines[2].right == 80,
            "a line is as wide as its ink, not as the page");
    }

    /*
     * A band that runs to the very last row. That is what a tightly cropped
     * screenshot of one line looks like, and a loop that only closes a band
     * when it meets an empty row never closes this one.
     */
    paper(&p, 240);
    blot(&p, 10, PAGE_H - 6, 50, 6, 20);
    recon_ocr_ink(p.rgba, PAGE_W, PAGE_H, p.ink);
    found = recon_ocr_lines(p.ink, PAGE_W, PAGE_H, lines, 8);
    check(found == 1, "a band against the bottom edge is still a line");
    if (found == 1) {
        check(lines[0].bottom == PAGE_H, "and it reaches the last row");
    }

    /* A blank page has no lines, which is an answer rather than a failure. */
    paper(&p, 240);
    recon_ocr_ink(p.rgba, PAGE_W, PAGE_H, p.ink);
    check(recon_ocr_lines(p.ink, PAGE_W, PAGE_H, lines, 8) == 0,
        "a blank page has no lines");

    /* More bands than there is room for: the first few, not a crash. */
    paper(&p, 240);
    for (int i = 0; i < 12; i++) {
        blot(&p, 10, i * 8, 40, 4, 20);
    }
    recon_ocr_ink(p.rgba, PAGE_W, PAGE_H, p.ink);
    check(recon_ocr_lines(p.ink, PAGE_W, PAGE_H, lines, 4) == 4,
        "more lines than room gives as many as fit");
}

/* --- Marks --- */

static void test_marks(void) {
    printf("Marks\n");

    struct page p;
    struct recon_ocr_line lines[4];
    struct recon_ocr_mark marks[16];

    /*
     * Four marks in two groups: narrow gaps within a group, a wide one
     * between. That is a line of two words, and the gap that separates them is
     * the only thing distinguishing it from a line of one long one.
     */
    paper(&p, 240);
    blot(&p, 10, 20, 6, 12, 20);
    blot(&p, 18, 20, 6, 12, 20);
    blot(&p, 50, 20, 6, 12, 20);
    blot(&p, 58, 20, 6, 12, 20);
    recon_ocr_ink(p.rgba, PAGE_W, PAGE_H, p.ink);

    int found = recon_ocr_lines(p.ink, PAGE_W, PAGE_H, lines, 4);
    check(found == 1, "one band");
    if (found != 1) {
        return;
    }

    int count = recon_ocr_marks(p.ink, PAGE_W, &lines[0], marks, 16);
    check(count == 4, "four marks");

    if (count == 4) {
        check(marks[0].left == 10 && marks[0].right == 16, "the first mark");
        check(!marks[0].space_before, "nothing before the first");
        check(!marks[1].space_before, "a narrow gap is not a space");
        check(marks[2].space_before, "a wide gap is");
        check(!marks[3].space_before, "and the one after it is not");
    }

    /*
     * Marks at different heights within the same band. Where a mark sits is
     * most of what separates a comma from an apostrophe and an 'o' from a '0',
     * so a mark that reported the whole line's height would have thrown that
     * away before anything could use it.
     */
    paper(&p, 240);
    blot(&p, 10, 20, 6, 20, 20);      /* tall, full height */
    blot(&p, 30, 34, 6, 6, 20);       /* short, sitting low */
    recon_ocr_ink(p.rgba, PAGE_W, PAGE_H, p.ink);
    recon_ocr_lines(p.ink, PAGE_W, PAGE_H, lines, 4);
    count = recon_ocr_marks(p.ink, PAGE_W, &lines[0], marks, 16);

    check(count == 2, "two marks at different heights");
    if (count == 2) {
        check(marks[0].top == 20 && marks[0].bottom == 40,
            "the tall mark keeps its own rows");
        check(marks[1].top == 34 && marks[1].bottom == 40,
            "and the low one keeps its own, not the line's");
    }
}

/* --- Word breaks --- */

static void test_spaces(void) {
    printf("Word breaks\n");

    struct page p;
    struct recon_ocr_line lines[4];
    struct recon_ocr_mark marks[32];

    /*
     * Ten marks, evenly spaced, in a short band. This is "0123456789", and the
     * right answer is that it contains no spaces.
     *
     * The old rule -- a quarter of the band's height -- could not give that
     * answer. Its threshold was a number of pixels, and every gap in this line
     * is the same number of pixels, so it either called all of them spaces or
     * none, depending on a band height that has nothing to do with the gaps.
     */
    paper(&p, 240);
    for (int i = 0; i < 10; i++) {
        blot(&p, 10 + i * 9, 20, 6, 11, 20);
    }
    recon_ocr_ink(p.rgba, PAGE_W, PAGE_H, p.ink);
    recon_ocr_lines(p.ink, PAGE_W, PAGE_H, lines, 4);

    int count = recon_ocr_marks(p.ink, PAGE_W, &lines[0], marks, 32);
    check(count == 10, "ten evenly spaced marks");

    int spaces = 0;
    for (int i = 0; i < count; i++) {
        spaces += marks[i].space_before ? 1 : 0;
    }
    check(spaces == 0, "evenly spaced marks are one word");

    /*
     * The same ten marks with two of the gaps widened. Now there are word
     * breaks, and there are exactly two of them -- the point being that the
     * threshold came from this line's own gaps rather than from its height,
     * which has not changed between the two halves of this test.
     */
    paper(&p, 240);
    int x = 10;
    for (int i = 0; i < 10; i++) {
        blot(&p, x, 20, 6, 11, 20);
        x += 9;
        if (i == 2 || i == 6) {
            x += 8;              /* a word ends here */
        }
    }
    recon_ocr_ink(p.rgba, PAGE_W, PAGE_H, p.ink);
    recon_ocr_lines(p.ink, PAGE_W, PAGE_H, lines, 4);
    count = recon_ocr_marks(p.ink, PAGE_W, &lines[0], marks, 32);

    check(count == 10, "still ten marks");

    spaces = 0;
    for (int i = 0; i < count; i++) {
        spaces += marks[i].space_before ? 1 : 0;
    }
    check(spaces == 2, "two wide gaps are two word breaks");
    if (count == 10) {
        check(marks[3].space_before && marks[7].space_before,
            "and they are the two that were widened");
    }

    /*
     * Two marks is one gap, and one number is not a population. The fallback
     * has to decide something, and the something is that an ordinary gap
     * between two letters is not a space.
     */
    paper(&p, 240);
    blot(&p, 10, 20, 6, 12, 20);
    blot(&p, 18, 20, 6, 12, 20);
    recon_ocr_ink(p.rgba, PAGE_W, PAGE_H, p.ink);
    recon_ocr_lines(p.ink, PAGE_W, PAGE_H, lines, 4);
    count = recon_ocr_marks(p.ink, PAGE_W, &lines[0], marks, 32);
    check(count == 2 && !marks[1].space_before,
        "two marks close together are one word");
}

/* --- Blocks --- */

/* Three short bars, the shape of a line of writing. */
static void write_a_line(struct page *p, int x, int y, int height) {
    for (int i = 0; i < 3; i++) {
        blot(p, x + i * 9, y, 6, height, 20);
    }
}

/* Every line found in every block. However the picture was divided, this is
 * the number that has to survive the division. */
static int lines_in_all(struct page *p, const struct recon_ocr_region *regions,
        int count) {
    struct recon_ocr_line lines[16];
    int total = 0;
    for (int i = 0; i < count; i++) {
        total += recon_ocr_lines_in(p->ink, PAGE_W, PAGE_H, &regions[i],
            lines, 16);
    }
    return total;
}

static void test_regions(void) {
    printf("Blocks\n");

    struct page p;
    struct recon_ocr_region regions[32];

    /* A picture that is only writing is one block, and nothing changes for it.
     * That is the case every earlier test in this file is about. */
    paper(&p, 240);
    write_a_line(&p, 20, 20, 10);
    write_a_line(&p, 20, 36, 10);
    recon_ocr_ink(p.rgba, PAGE_W, PAGE_H, p.ink);

    int count = recon_ocr_regions(p.ink, PAGE_W, PAGE_H, regions, 32);
    check(count >= 1, "a picture of only writing has blocks in it");
    check(lines_in_all(&p, regions, count) == 2,
        "and both lines survive being divided up");

    /* Two lots of writing far apart across the page. */
    paper(&p, 240);
    write_a_line(&p, 10, 20, 10);
    write_a_line(&p, 130, 20, 10);
    recon_ocr_ink(p.rgba, PAGE_W, PAGE_H, p.ink);
    count = recon_ocr_regions(p.ink, PAGE_W, PAGE_H, regions, 32);
    check(count == 2, "writing either side of a wide gap is two blocks");

    /*
     * The case this stage was built for.
     *
     * Writing inside a drawn box. The box's sides put ink on every row it
     * spans, so no row is blank and the line finder -- looking at the whole
     * picture -- finds one band covering the box and everything in it.
     *
     * This is a screenshot in miniature: a window is a box with writing in it.
     */
    /*
     * Taller than 48 rows on purpose. Below that a full-height line is not
     * treated as a border, because at the bottom of the recursion a block is a
     * single line of writing and a tall letter spans it -- so the box in this
     * test has to be the size a real window is, not the size that fits neatly
     * into a test page.
     */
    paper(&p, 240);
    blot(&p, 20, 8, 120, 1, 20);        /* top */
    blot(&p, 20, 90, 120, 1, 20);       /* bottom */
    blot(&p, 20, 8, 1, 83, 20);         /* left */
    blot(&p, 139, 8, 1, 83, 20);        /* right */
    write_a_line(&p, 30, 20, 10);
    write_a_line(&p, 30, 60, 10);
    recon_ocr_ink(p.rgba, PAGE_W, PAGE_H, p.ink);

    struct recon_ocr_line lines[16];
    int whole = recon_ocr_lines(p.ink, PAGE_W, PAGE_H, lines, 16);
    check(whole == 1, "a boxed page is one band to the line finder");

    count = recon_ocr_regions(p.ink, PAGE_W, PAGE_H, regions, 32);
    check(count >= 1, "but the block finder gets inside the box");

    /* And within the block that holds the writing, both lines are found. */
    int best = 0;
    for (int i = 0; i < count; i++) {
        int n = recon_ocr_lines_in(p.ink, PAGE_W, PAGE_H, &regions[i], lines,
            16);
        if (n > best) {
            best = n;
        }
    }
    check(lines_in_all(&p, regions, count) == 2,
        "and both lines inside the box are found");
    check(best >= 1, "with the writing in a block of its own");

    /* A blank picture has no blocks, which is an answer rather than a fault. */
    paper(&p, 240);
    recon_ocr_ink(p.rgba, PAGE_W, PAGE_H, p.ink);
    check(recon_ocr_regions(p.ink, PAGE_W, PAGE_H, regions, 32) == 0,
        "a blank picture has no blocks");

    check(recon_ocr_regions(NULL, PAGE_W, PAGE_H, regions, 32) == 0,
        "no picture is refused");
}

int main(void) {
    printf("OCR tests\n\n");

    test_ink();
    test_lines();
    test_marks();
    test_spaces();
    test_regions();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
