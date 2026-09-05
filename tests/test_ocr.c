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

int main(void) {
    printf("OCR tests\n\n");

    test_ink();
    test_lines();
    test_marks();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
