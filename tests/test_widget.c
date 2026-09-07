/*
 * What state a control is in, and what colour that makes it.
 *
 * The widget layer's whole promise is that a button behaves the same
 * everywhere, and the part of that promise which is *arithmetic* rather than
 * pixels lives in recon_widget_state.c. It is four rules and they are the kind
 * that look obviously right and are obviously right in only three of the four
 * cases:
 *
 *   - the pointer over a control makes it hot
 *   - holding it makes it active
 *   - holding it and then sliding off makes it neither -- not still active,
 *     which is what a naive "held == id" gives you, and not hot either
 *   - while anything is held, nothing else lights up
 *
 * The third is the one worth a test. It is invisible in the code (`held == id`
 * reads as correct), it is the difference between a button that can be
 * cancelled and one that cannot, and finding it by hand means pressing a
 * close button, dragging off it, letting go, and noticing that the window
 * closed -- which is a bad way to find out.
 *
 * The colours are checked for the property that actually matters and not for
 * particular values: hover and press have to be *different from each other and
 * from rest*, on every skin the system ships. A test asserting that hover on
 * Recon is B1B6C0 would fail the next time anybody adjusts a palette, and
 * would be measuring the palette rather than the rule.
 *
 * Run with: cmake --build build && ./build/recon_widget_tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_fs.h"
#include "recon_theme.h"
#include "recon_widget.h"

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

#define BUTTON  1001u
#define OTHER   1002u

static void test_nothing_happening(void) {
    printf("\nAt rest\n");

    check(recon_widget_state_from(RECON_HIT_NONE, RECON_HIT_NONE, BUTTON,
            false) == RECON_WIDGET_NORMAL,
        "a control nobody is pointing at is normal");

    check(recon_widget_state_from(OTHER, RECON_HIT_NONE, BUTTON, false)
            == RECON_WIDGET_NORMAL,
        "the pointer on something else leaves this one alone");
}

static void test_hover(void) {
    printf("\nUnder the pointer\n");

    check(recon_widget_state_from(BUTTON, RECON_HIT_NONE, BUTTON, false)
            == RECON_WIDGET_HOT,
        "the pointer over it makes it hot");

    check(recon_widget_state_from(BUTTON, RECON_HIT_NONE, BUTTON, true)
            == RECON_WIDGET_DISABLED,
        "a disabled control does not light up under the pointer");

    /*
     * Disabled beats everything, including being held.
     *
     * Not a hypothetical: a control can be disabled by the very click being
     * held on it -- press Half on a picture one step above the floor and it
     * is at the floor by the time the frame is drawn. Sunk-and-unavailable is
     * a state with nothing to say.
     */
    check(recon_widget_state_from(BUTTON, BUTTON, BUTTON, true)
            == RECON_WIDGET_DISABLED,
        "disabled wins over held");
}

static void test_press(void) {
    printf("\nHeld down\n");

    check(recon_widget_state_from(BUTTON, BUTTON, BUTTON, false)
            == RECON_WIDGET_ACTIVE,
        "held with the pointer still on it is active");

    /*
     * The rule this file exists for.
     *
     * `held == id` is true here and the answer is still not ACTIVE. Every
     * desktop lets somebody press a button, think better of it, slide off and
     * let go -- and a button that stayed sunk while the pointer wandered away
     * would be promising the opposite of what is about to happen.
     */
    check(recon_widget_state_from(RECON_HIT_NONE, BUTTON, BUTTON, false)
            == RECON_WIDGET_NORMAL,
        "held, but the pointer slid off: not active and not hot");

    check(recon_widget_state_from(OTHER, BUTTON, BUTTON, false)
            == RECON_WIDGET_NORMAL,
        "held, with the pointer now over something else: not active");

    check(recon_widget_state_from(BUTTON, OTHER, BUTTON, false)
            == RECON_WIDGET_NORMAL,
        "while another control is held, this one does not light up");
}

static void test_nothing_is_a_control(void) {
    printf("\nThe absence of a control\n");

    /*
     * RECON_HIT_NONE is zero, which is also what an uninitialised field is,
     * so "no control" must never come out as a state. Otherwise every gap
     * between two buttons highlights the moment the pointer is over nothing.
     */
    check(recon_widget_state_from(RECON_HIT_NONE, RECON_HIT_NONE,
            RECON_HIT_NONE, false) == RECON_WIDGET_NORMAL,
        "nothing under the pointer is not a hot nothing");

    check(recon_widget_state_from(RECON_HIT_NONE, RECON_HIT_NONE,
            RECON_HIT_NONE, true) == RECON_WIDGET_DISABLED,
        "an explicitly disabled nothing still reports disabled");
}

/*
 * Hover and press have to be visible, on every skin.
 *
 * Derived from the selection colour rather than named as a role, which is what
 * makes a skin somebody wrote this afternoon get a working highlight -- and
 * also what makes this worth checking, because a skin whose selection colour
 * happens to equal its button colour would derive a highlight identical to
 * rest and nobody would notice until they used it.
 */
static void test_the_shades_differ(void) {
    printf("\nThe three shades, on every skin\n");

    int skins = recon_theme_count();
    check(skins > 0, "there are skins to check");

    int flat_hover = 0;
    int flat_press = 0;
    int no_ladder = 0;
    int looked_at = 0;

    for (int i = 0; i < skins; i++) {
        struct recon_theme_info info;
        if (!recon_theme_at(i, &info) || !recon_theme_set(info.name)) {
            continue;
        }
        const char *name = info.name;
        looked_at++;

        recon_color rest = THEME(BUTTON);
        recon_color hot = recon_widget_surface(rest, RECON_WIDGET_HOT);
        recon_color held = recon_widget_surface(rest, RECON_WIDGET_ACTIVE);

        if (hot == rest) {
            printf("        %s: hover is the same as rest\n", name);
            flat_hover++;
        }
        if (held == hot) {
            printf("        %s: press is the same as hover\n", name);
            flat_press++;
        }

        /*
         * And they have to go the same way. Hover a step toward the selection
         * colour and press two steps means press is always further from rest
         * than hover is -- which is what makes the two read as one idea at
         * two strengths rather than as two unrelated colours.
         */
        int d_hot = abs(recon_color_luminance(hot)
            - recon_color_luminance(rest));
        int d_held = abs(recon_color_luminance(held)
            - recon_color_luminance(rest));
        if (d_held < d_hot) {
            printf("        %s: press is nearer rest than hover is\n", name);
            no_ladder++;
        }
    }

    /*
     * How many were actually reached.
     *
     * The three counters below are all zero when the loop never runs, so
     * without this a skin list that failed to load would report three passes
     * for having checked nothing. A test that cannot tell "all correct" from
     * "none examined" is worse than no test, because it is believed.
     */
    check(looked_at == skins, "every skin was reachable and was checked");

    check(flat_hover == 0, "every skin's hover differs from its rest");
    check(flat_press == 0, "every skin's press differs from its hover");
    check(no_ladder == 0, "press is always the further of the two");

    check(recon_widget_surface(THEME(BUTTON), RECON_WIDGET_NORMAL)
            == THEME(BUTTON),
        "normal is the colour it was given, untouched");
    check(recon_widget_surface(THEME(BUTTON), RECON_WIDGET_DISABLED)
            == THEME(BUTTON),
        "disabled does not tint the surface -- the label carries it");
}

int main(void) {
    printf("Widgets\n");

    /* Its own filesystem: reading a skin touches the registry. */
    char root[] = "/tmp/recon-widget-test-XXXXXX";
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

    test_nothing_happening();
    test_hover();
    test_press();
    test_nothing_is_a_control();
    test_the_shades_differ();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
