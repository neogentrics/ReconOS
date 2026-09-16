/*
 * A panel drawn straight onto a screen, with no compositor under it.
 *
 * `src/recon_ui_fb.c` is what a program on the ReconOS kernel uses: the
 * kernel hands back an address and a shape, and after that drawing is stores
 * to memory. It takes no system calls -- the caller passes the address and the
 * shape it already asked for -- so **the same code runs against a plain buffer
 * here**, and the half that can be wrong is arithmetic on pixels.
 *
 * It is also the second implementation of `struct recon_panel_present`, which
 * is the argument that the seam is in the right place: if a second one needs
 * the drawing half to change, the seam was drawn around one implementation.
 *
 * --- what is worth testing here -----------------------------------------
 *
 * Three things, and one of them is the reason SYS_SCREEN exists:
 *
 *   **The pitch.** Bytes per row is not `width * 4` on real hardware, and it
 *   is the one fact a program cannot recover from the pixels. Every canvas in
 *   this file is therefore *wider in bytes than it is in pixels*, with the
 *   padding filled with a value nothing should ever write -- because where the
 *   two are equal, and they are on most emulators, a program that confuses
 *   them draws a perfect picture and shears on the first real laptop.
 *
 *   **The edges.** A panel partly off the screen must draw the part that is on
 *   and touch nothing else.
 *
 *   **The blend.** Straight alpha over what is already there.
 *
 * Run with: cmake --build build && ./build/recon_ui_fb_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_ui.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
    }
}

/*
 * A screen with padding on the end of every row.
 *
 * `PAD` pixels of it, filled with a value nothing may write. The suite checks
 * it afterwards, which is the one fault a look at the picture cannot catch.
 */
#define SCREEN_W 40
#define SCREEN_H 20
#define PAD 7
#define PITCH ((size_t)(SCREEN_W + PAD) * 4u)
#define GUARD 0xDEADBEEFu

struct screen {
    uint32_t *memory;
};

static void screen_make(struct screen *s, uint32_t fill) {
    s->memory = malloc(PITCH * SCREEN_H);
    for (int y = 0; y < SCREEN_H; y++) {
        uint32_t *row = (uint32_t *)((unsigned char *)s->memory +
            (size_t)y * PITCH);
        for (int x = 0; x < SCREEN_W; x++) {
            row[x] = fill;
        }
        for (int x = SCREEN_W; x < SCREEN_W + PAD; x++) {
            row[x] = GUARD;
        }
    }
}

static uint32_t at(const struct screen *s, int x, int y) {
    const uint32_t *row = (const uint32_t *)((const unsigned char *)s->memory +
        (size_t)y * PITCH);
    return row[x];
}

static bool padding_untouched(const struct screen *s) {
    for (int y = 0; y < SCREEN_H; y++) {
        for (int x = SCREEN_W; x < SCREEN_W + PAD; x++) {
            if (at(s, x, y) != GUARD) {
                printf("        padding written at %d,%d\n", x, y);
                return false;
            }
        }
    }
    return true;
}

static void screen_free(struct screen *s) {
    free(s->memory);
    s->memory = NULL;
}

/* --- the tests --- */

static void test_it_lands_where_it_is_told(void) {
    printf("a panel is drawn at its position, through the pitch\n");

    struct screen s;
    screen_make(&s, 0xFF000000u);

    struct recon_panel *panel = recon_panel_on_screen(s.memory, PITCH,
        SCREEN_W, SCREEN_H, 5, 3, 10, 4);
    check(panel != NULL, "a panel can be put on a screen");
    if (panel == NULL) {
        screen_free(&s);
        return;
    }

    recon_fill(panel, 0xFFFF0000u);         /* opaque red */
    recon_panel_commit(panel);

    check(at(&s, 5, 3) == 0xFFFF0000u, "its top-left corner is at 5,3");
    check(at(&s, 14, 6) == 0xFFFF0000u, "and its bottom-right at 14,6");
    check(at(&s, 4, 3) == 0xFF000000u, "the pixel to its left is untouched");
    check(at(&s, 15, 3) == 0xFF000000u, "and the one to its right");
    check(at(&s, 5, 2) == 0xFF000000u, "and the one above it");
    check(at(&s, 5, 7) == 0xFF000000u, "and the one below it");

    /*
     * **The padding.** This is the check that catches a row addressed as
     * `width * 4` instead of through the pitch -- on a screen where the two
     * are equal such a program is perfect, and here it walks into the next
     * row's padding.
     */
    check(padding_untouched(&s),
        "and nothing was written into the padding at the end of a row");

    recon_panel_destroy(panel);
    screen_free(&s);
}

static void test_the_edges(void) {
    printf("a panel hanging off the screen draws the part that is on it\n");

    struct screen s;
    screen_make(&s, 0xFF000000u);

    /* Off the top-left corner: only its bottom-right quarter is on screen. */
    struct recon_panel *panel = recon_panel_on_screen(s.memory, PITCH,
        SCREEN_W, SCREEN_H, -3, -2, 6, 5);
    check(panel != NULL, "a panel can hang off the corner");
    if (panel != NULL) {
        recon_fill(panel, 0xFF00FF00u);
        recon_panel_commit(panel);

        check(at(&s, 0, 0) == 0xFF00FF00u, "0,0 is drawn");
        check(at(&s, 2, 2) == 0xFF00FF00u, "and so is 2,2, its last pixel");
        check(at(&s, 3, 0) == 0xFF000000u, "3,0 is past its right edge");
        check(at(&s, 0, 3) == 0xFF000000u, "and 0,3 is past its bottom");
        recon_panel_destroy(panel);
    }

    /* And off the far corner. Nothing may be written past the screen. */
    struct recon_panel *far = recon_panel_on_screen(s.memory, PITCH,
        SCREEN_W, SCREEN_H, SCREEN_W - 2, SCREEN_H - 2, 8, 8);
    check(far != NULL, "and off the far corner");
    if (far != NULL) {
        recon_fill(far, 0xFF0000FFu);
        recon_panel_commit(far);

        check(at(&s, SCREEN_W - 1, SCREEN_H - 1) == 0xFF0000FFu,
            "the last pixel of the screen is drawn");
        check(padding_untouched(&s),
            "and a panel running off the right edge does not spill into the "
            "padding");
        recon_panel_destroy(far);
    }

    /* Entirely off the screen: nothing at all. */
    struct screen clean;
    screen_make(&clean, 0xFF123456u);
    struct recon_panel *away = recon_panel_on_screen(clean.memory, PITCH,
        SCREEN_W, SCREEN_H, 100, 100, 4, 4);
    if (away != NULL) {
        recon_fill(away, 0xFFFFFFFFu);
        recon_panel_commit(away);
        bool all_still = true;
        for (int y = 0; y < SCREEN_H && all_still; y++) {
            for (int x = 0; x < SCREEN_W; x++) {
                if (at(&clean, x, y) != 0xFF123456u) {
                    all_still = false;
                    break;
                }
            }
        }
        check(all_still, "a panel entirely off the screen writes nothing");
        recon_panel_destroy(away);
    }
    screen_free(&clean);

    screen_free(&s);
}

static void test_the_blend(void) {
    printf("straight alpha over what is already there\n");

    struct screen s;
    screen_make(&s, 0xFF000000u);   /* black */

    struct recon_panel *panel = recon_panel_on_screen(s.memory, PITCH,
        SCREEN_W, SCREEN_H, 0, 0, 4, 4);
    if (panel == NULL) {
        check(false, "a panel was made");
        screen_free(&s);
        return;
    }

    /*
     * **An exact answer, on a case chosen to be exact about.**
     *
     * The first version blended half-white over black and accepted anything
     * from 126 to 130. That tolerance was mine, not the code's, and it was
     * wide enough to swallow two real faults: swapping the source and
     * destination weights gives 127 there, and dropping the `+127` rounding
     * gives 128, so a mutation run reported both as *not caught*. A symmetric
     * input over a black background cannot tell those apart -- it has no
     * asymmetry to lose.
     *
     * So: a source that is not grey, over a destination that is not black, at
     * an alpha that divides unevenly, and one exact value. Worked out from the
     * same arithmetic the implementation uses, which is a weaker check than
     * comparing against a second implementation -- but the property it is
     * really holding is *that this does not change*, and every mutation that
     * alters the blend now moves it.
     */
    for (int y = 0; y < SCREEN_H; y++) {
        for (int x = 0; x < SCREEN_W; x++) {
            uint32_t *row = (uint32_t *)((unsigned char *)s.memory +
                (size_t)y * PITCH);
            row[x] = 0xFF206450u;
        }
    }

    recon_fill(panel, 0x60C0C840u);         /* alpha 96, over 0xFF206450 */
    recon_panel_commit(panel);

    uint32_t got = at(&s, 0, 0);
    check(got == 0xFF5C8A4Au, "the blend gives exactly one answer");
    if (got != 0xFF5C8A4Au) {
        printf("        wanted 0xFF5C8A4A, got 0x%08X\n", got);
    }
    check((got >> 24) == 0xFFu, "and the screen stays opaque");

    /*
     * Nothing at all where alpha is zero -- **including the colour**.
     *
     * A rounded corner is alpha 0 with the frame's colour still in the other
     * three bytes, and on the wlroots side a blend that adds it anyway draws
     * the corner square. That fault is real there and comes from
     * premultiplying; **it cannot happen here**, because this blends directly
     * and the formula is exact at alpha 0.
     *
     * Kept anyway, and the reason is worth writing down: deleting the
     * `a == 0` shortcut from the implementation changes no output, which a
     * mutation run is what proved. The check holds the *property* rather than
     * the shortcut -- a future blend written a different way would be caught
     * by it, and that is what it is for.
     */
    recon_fill(panel, 0x00FF0000u);     /* fully transparent red */
    recon_panel_commit(panel);
    check(at(&s, 0, 0) == got,
        "a fully transparent pixel changes nothing, colour included");

    /* And opaque wins outright. */
    recon_fill(panel, 0xFF00FF00u);
    recon_panel_commit(panel);
    check(at(&s, 0, 0) == 0xFF00FF00u, "an opaque pixel replaces what is there");

    recon_panel_destroy(panel);
    screen_free(&s);
}

static void test_moving_and_hiding(void) {
    printf("where it sits, and whether it draws at all\n");

    struct screen s;
    screen_make(&s, 0xFF000000u);

    struct recon_panel *panel = recon_panel_on_screen(s.memory, PITCH,
        SCREEN_W, SCREEN_H, 1, 1, 3, 3);
    if (panel == NULL) {
        check(false, "a panel was made");
        screen_free(&s);
        return;
    }

    int x = -1, y = -1;
    recon_panel_position(panel, &x, &y);
    check(x == 1 && y == 1, "it remembers where it was put");

    recon_panel_set_position(panel, 10, 10);
    recon_panel_position(panel, &x, &y);
    check(x == 10 && y == 10, "and where it was moved to");

    recon_fill(panel, 0xFFFFFFFFu);
    recon_panel_commit(panel);
    check(at(&s, 10, 10) == 0xFFFFFFFFu, "and it draws at the new place");
    check(at(&s, 1, 1) == 0xFF000000u, "and not at the old one");

    /*
     * Hidden stops it drawing, and what was underneath does not come back --
     * nothing here remembers what was underneath. That is the difference
     * between a compositor and a program with a screen, and it is written
     * down rather than discovered.
     */
    recon_panel_set_enabled(panel, false);
    recon_fill(panel, 0xFFFF00FFu);
    recon_panel_commit(panel);
    check(at(&s, 10, 10) == 0xFFFFFFFFu,
        "a hidden panel does not draw, and does not restore either");

    recon_panel_set_enabled(panel, true);
    recon_panel_commit(panel);
    check(at(&s, 10, 10) == 0xFFFF00FFu, "and it draws again when shown");

    recon_panel_destroy(panel);
    screen_free(&s);
}

static void test_what_it_refuses(void) {
    printf("what it will not accept\n");

    uint32_t buffer[64];

    check(recon_panel_on_screen(NULL, PITCH, 8, 8, 0, 0, 4, 4) == NULL,
        "no screen, no panel");
    check(recon_panel_on_screen(buffer, PITCH, 8, 8, 0, 0, 0, 4) == NULL,
        "a panel no pixels wide is not a panel");
    check(recon_panel_on_screen(buffer, PITCH, 0, 8, 0, 0, 4, 4) == NULL,
        "nor is a screen with no width");

    /*
     * **A pitch smaller than a row is refused, not clamped.**
     *
     * Every row would be written partly over the previous one, which draws a
     * sheared picture that reads as a drawing fault rather than as a caller
     * who passed a width where bytes were wanted. Refusing says which.
     */
    check(recon_panel_on_screen(buffer, 8u * 4u - 1u, 8, 8, 0, 0, 4, 4) == NULL,
        "and a pitch that cannot hold a row is refused rather than clamped");

    /*
     * The one call here that succeeds, and it has to be let go of.
     *
     * The first version dropped it -- three allocations, 6,808 bytes, and no
     * visible symptom at all. The sanitizer pass in check.sh is what said so,
     * which is the whole reason every suite is built twice.
     */
    struct recon_panel *fine = recon_panel_on_screen(buffer, 8u * 4u, 8, 8,
        0, 0, 4, 4);
    check(fine != NULL, "while a pitch exactly one row wide is fine");
    recon_panel_destroy(fine);
}

int main(void) {
    printf("ReconOS screen-panel tests\n\n");

    test_it_lands_where_it_is_told();
    test_the_edges();
    test_the_blend();
    test_moving_and_hiding();
    test_what_it_refuses();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
