/*
 * Where the things on a title bar go.
 *
 * The other file scripts/coverage.sh found at zero. It is small -- 165 lines --
 * and it is the arithmetic behind every window in the system, including the
 * one rule this project states most loudly: **close is always there, and
 * always outermost.**
 *
 * That rule had been checked by writing a skin that asks for no buttons and
 * photographing the close button it got anyway. That is a real check and it is
 * a slow one, it needs a display, and it proves the rule for one skin. This
 * proves it for every combination of side and button set there is, in a
 * millisecond, without a screen.
 *
 * What is worth checking here is geometry that a person cannot see is wrong
 * until it is: buttons that overlap by a pixel, a title that runs under them,
 * a drag region that covers the close button. Each of those looks like a
 * different bug when it happens -- "the close button does not work" is what a
 * drag region overlapping it feels like -- and none of them is visible in the
 * code.
 *
 * Run with: cmake --build build && ./build/recon_titlebar_tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_fs.h"
#include "recon_theme.h"
#include "recon_titlebar.h"

/*
 * A skin of this test's own, because the metrics belong to a skin.
 *
 * The ones ReconOS ships are refused for writing -- a preset that can be
 * edited is a preset somebody can break with no way back -- so the test copies
 * one and edits the copy, which is exactly what the Control Panel makes a
 * person do before it will let them change a colour.
 */
#define TEST_SKIN "Titlebar Test"

/* Both metrics at once, since no test here wants one without the other. */
static bool set_buttons(int wanted, bool on_the_left) {
    return recon_theme_set_metric(TEST_SKIN, RECON_METRIC_BUTTONS, true,
            wanted) &&
        recon_theme_set_metric(TEST_SKIN, RECON_METRIC_BUTTONS_LEFT, true,
            on_the_left ? 1 : 0);
}

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL  %s\n", what);
    } else {
        printf("  ok    %s\n", what);
    }
}

/* Do two rectangles share a pixel? Nothing on a title bar should. */
static bool overlaps(const struct recon_titlebar_rect *a,
        const struct recon_titlebar_rect *b) {
    if (a->w <= 0 || b->w <= 0) {
        return false;
    }
    return a->x < b->x + b->w && b->x < a->x + a->w;
}

static bool shown(const struct recon_titlebar_layout *layout, int which) {
    return layout->button[which].w > 0;
}

static void test_close_is_always_there(void) {
    printf("Close\n");

    /*
     * Every combination the skin can ask for, including zero.
     *
     * The metric is a bit field, so there are eight; asking for all eight and
     * checking the same thing about each is what makes this a proof rather
     * than an example. The photographed test covered one of the eight.
     */
    int missing_close = 0;
    int not_outermost = 0;

    for (int wanted = 0; wanted < 8; wanted++) {
        for (int side = 0; side < 2; side++) {
            if (!set_buttons(wanted, side != 0)) {
                check(false, "the test skin would not take a metric");
                return;
            }

            struct recon_titlebar_layout layout;
            recon_titlebar_layout(400, 8, 3, true, &layout);

            if (!shown(&layout, RECON_TITLEBAR_CLOSE)) {
                missing_close++;
                continue;
            }

            /*
             * Outermost means nearest the edge the buttons are on, which is a
             * different comparison per side -- and getting that backwards is
             * exactly the mistake this checks for, since it looks right from
             * whichever side the person who wrote it was thinking about.
             */
            for (int b = 0; b < RECON_TITLEBAR_BUTTON_COUNT; b++) {
                if (b == RECON_TITLEBAR_CLOSE || !shown(&layout, b)) {
                    continue;
                }
                bool ok = side
                    ? layout.button[RECON_TITLEBAR_CLOSE].x <= layout.button[b].x
                    : layout.button[RECON_TITLEBAR_CLOSE].x >= layout.button[b].x;
                if (!ok) {
                    not_outermost++;
                }
            }
        }
    }

    check(missing_close == 0,
        "CLOSE IS DRAWN FOR EVERY SET OF BUTTONS A SKIN CAN ASK FOR, "
        "including none");
    check(not_outermost == 0,
        "and is outermost in all of them, on either side");
}

static void test_nothing_overlaps(void) {
    printf("Nothing sits on anything else\n");

    int button_overlaps = 0;
    int over_the_title = 0;
    int drag_over_a_button = 0;

    /* A range of widths, because the arithmetic that goes wrong here goes
     * wrong at one end: a bar too narrow for its own buttons. */
    static const int WIDTHS[] = { 60, 120, 240, 400, 1280 };

    for (size_t wi = 0; wi < sizeof(WIDTHS) / sizeof(WIDTHS[0]); wi++) {
        for (int wanted = 0; wanted < 8; wanted++) {
            for (int side = 0; side < 2; side++) {
                if (!set_buttons(wanted, side != 0)) {
                    check(false, "the test skin would not take a metric");
                    return;
                }

                struct recon_titlebar_layout layout;
                recon_titlebar_layout(WIDTHS[wi], 8, 3, true, &layout);

                for (int a = 0; a < RECON_TITLEBAR_BUTTON_COUNT; a++) {
                    for (int b = a + 1; b < RECON_TITLEBAR_BUTTON_COUNT; b++) {
                        if (overlaps(&layout.button[a], &layout.button[b])) {
                            button_overlaps++;
                        }
                    }
                    if (layout.text_width > 0 && shown(&layout, a)) {
                        struct recon_titlebar_rect text = {
                            layout.text_x, 0, layout.text_width, 0
                        };
                        if (overlaps(&text, &layout.button[a])) {
                            over_the_title++;
                        }
                    }
                    if (shown(&layout, a) &&
                            overlaps(&layout.drag, &layout.button[a])) {
                        drag_over_a_button++;
                    }
                }
            }
        }
    }

    check(button_overlaps == 0, "no two buttons share a pixel");
    check(over_the_title == 0, "the title never runs under one");
    check(drag_over_a_button == 0,
        "AND THE DRAG REGION DOES NOT COVER ONE -- which would read as the "
        "button not working rather than as a layout fault");
}

static void test_a_bar_with_no_room(void) {
    printf("A bar too narrow to hold what is on it\n");

    set_buttons(7, false);

    /*
     * Zero and one pixel wide.
     *
     * A window cannot be either, and this is called while one is being resized
     * by a person holding a mouse button -- so it is called with whatever
     * number the pointer is over, including numbers that are not a window.
     * What it must not do is hand back a negative width for something to draw.
     */
    for (int width = 0; width <= 40; width += 5) {
        struct recon_titlebar_layout layout;
        recon_titlebar_layout(width, 8, 3, true, &layout);

        bool sane = layout.text_width >= 0 && layout.drag.w >= 0 &&
            layout.icon.w >= 0;
        for (int b = 0; b < RECON_TITLEBAR_BUTTON_COUNT && sane; b++) {
            if (layout.button[b].w < 0 || layout.button[b].h < 0) {
                sane = false;
            }
        }
        if (!sane) {
            char what[96];
            snprintf(what, sizeof(what),
                "a bar %d pixels wide gives nothing a negative size", width);
            check(false, what);
            return;
        }
    }
    check(true, "no width from zero to forty produces a negative size");
}

static void test_the_icon(void) {
    printf("The icon, and the room it takes\n");

    set_buttons(7, false);

    struct recon_titlebar_layout with;
    struct recon_titlebar_layout without;
    recon_titlebar_layout(400, 8, 3, true, &with);
    recon_titlebar_layout(400, 8, 3, false, &without);

    check(with.icon.w > 0, "a bar with an icon has somewhere to put it");
    check(without.icon.w == 0, "one without does not");
    check(without.text_width > with.text_width,
        "AND THE TITLE GETS THE ROOM BACK, rather than a hole where a "
        "picture would have been");
    check(without.text_x <= with.text_x, "starting further left");
}

static void test_both_sides_are_mirrors(void) {
    printf("Left and right\n");

    struct recon_titlebar_layout right;
    struct recon_titlebar_layout left;

    set_buttons(7, false);
    recon_titlebar_layout(400, 8, 3, true, &right);
    set_buttons(7, true);
    recon_titlebar_layout(400, 8, 3, true, &left);

    check(right.button[RECON_TITLEBAR_CLOSE].x >
        left.button[RECON_TITLEBAR_CLOSE].x,
        "the buttons are on opposite ends");
    check(right.text_x < left.text_x,
        "and the title moves out of their way");
    check(right.text_width == left.text_width,
        "with the same room either way, because the same things are on the "
        "bar");
}

int main(void) {
    printf("Title bars\n\n");

    /* Its own filesystem, so the skin this writes is not somebody's. */
    char root[] = "/tmp/recon-titlebar-test-XXXXXX";
    if (mkdtemp(root) == NULL) {
        printf("could not make a temporary root\n");
        return 1;
    }
    if (!recon_fs_init(root)) {
        printf("could not start the filesystem: %s\n", recon_fs_last_error());
        return 1;
    }

    recon_theme_write_defaults();
    recon_theme_init();

    /* Copied from whatever is on, then put on, because the metrics that are
     * read are the current skin's. */
    if (!recon_theme_copy(recon_theme_current(), TEST_SKIN,
            "For the title bar tests") ||
            !recon_theme_set(TEST_SKIN)) {
        printf("could not make a skin to edit: %s\n",
            recon_theme_last_error());
        return 1;
    }

    test_close_is_always_there();
    test_nothing_overlaps();
    test_a_bar_with_no_room();
    test_the_icon();
    test_both_sides_are_mirrors();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
