/*
 * Tests for skins.
 *
 * The failure worth catching here is silent: a role that no skin answers, or a
 * name table that has drifted out of step with the enum, does not crash. It
 * just makes one part of the desktop the wrong colour forever, and nobody
 * connects that to the commit that caused it.
 *
 * Run with: ninja -C build && ./build/recon_theme_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <unistd.h>

#include "recon_fs.h"
#include "recon_registry.h"
#include "recon_theme.h"
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

/* --- Tests --- */

static void test_roles_are_all_answered(void) {
    printf("every role has a name and a colour\n");

    for (int i = 0; i < RECON_THEME_ROLE_COUNT; i++) {
        const char *name = recon_theme_role_name((enum recon_theme_role)i);
        if (name == NULL || *name == '\0') {
            g_checks++;
            g_failures++;
            printf("  FAIL: role %d has no name\n", i);
            continue;
        }
        g_checks++;
    }

    /* Names must be unique, or a theme file setting one of them would be
     * setting whichever the lookup happened to find first. */
    for (int i = 0; i < RECON_THEME_ROLE_COUNT; i++) {
        for (int j = i + 1; j < RECON_THEME_ROLE_COUNT; j++) {
            if (strcmp(recon_theme_role_name(i), recon_theme_role_name(j)) == 0) {
                g_checks++;
                g_failures++;
                printf("  FAIL: roles %d and %d share the name '%s'\n",
                    i, j, recon_theme_role_name(i));
            }
        }
    }
    g_checks++;

    check(recon_theme_role_name(RECON_THEME_ROLE_COUNT) == NULL,
        "a role past the end has no name");
}

static void test_no_role_is_unset(void) {
    printf("no skin leaves a role blank\n");

    int count = recon_theme_count();
    check(count >= 4, "the four shipped skins are all there");

    for (int t = 0; t < count; t++) {
        struct recon_theme_info info;
        if (!recon_theme_at(t, &info)) {
            continue;
        }
        check(recon_theme_set(info.name), info.name);

        /*
         * Fully transparent means nothing was written there. A role left at
         * zero draws as nothing at all, which is the failure this whole test
         * exists to catch -- it looks like a missing feature, not a missing
         * colour.
         */
        for (int i = 0; i < RECON_THEME_ROLE_COUNT; i++) {
            recon_color c = recon_theme_color((enum recon_theme_role)i);
            if ((c >> 24) == 0) {
                g_checks++;
                g_failures++;
                printf("  FAIL: '%s' leaves '%s' fully transparent\n",
                    info.name, recon_theme_role_name(i));
            }
        }
        g_checks++;
    }
}

static void test_out_of_range_is_loud(void) {
    printf("an unknown role is obvious rather than invisible\n");

    recon_color bad = recon_theme_color((enum recon_theme_role)-1);
    check(bad == RECON_RGB(0xFF, 0x00, 0xFF),
        "a role below the range gives magenta, not black");

    bad = recon_theme_color(RECON_THEME_ROLE_COUNT);
    check(bad == RECON_RGB(0xFF, 0x00, 0xFF),
        "a role past the end gives magenta, not black");
}

static void test_switching(void) {
    printf("switching\n");

    check(recon_theme_set("Recon"), "put on the default");
    recon_color recon_bar = recon_theme_color(RECON_THEME_BAR);

    check(recon_theme_set("Midnight"), "put on a dark one");
    recon_color midnight_bar = recon_theme_color(RECON_THEME_BAR);

    /* The point of the whole system: asking the same question gives a
     * different answer. */
    check(recon_bar != midnight_bar, "the same role gives a different colour");
    check(strcmp(recon_theme_current(), "Midnight") == 0,
        "the current skin is the one that was chosen");

    check(!recon_theme_set("NoSuchSkin"), "an unknown skin is refused");
    check(strcmp(recon_theme_current(), "Midnight") == 0,
        "and nothing changed when it was");

    unsigned before = recon_theme_generation();
    check(recon_theme_set("Recon"), "switch back");
    check(recon_theme_generation() != before,
        "the generation moves, so a cache can tell it is stale");
}

static void test_remembered(void) {
    printf("the choice is remembered\n");

    check(recon_theme_set("Aqua"), "choose one");
    check(strcmp(recon_registry_get(RECON_REG_USER, RECON_THEME_KEY, ""),
        "Aqua") == 0, "it was written to the registry");

    /* Exactly what a restart does. */
    recon_theme_finish();
    recon_theme_init();

    check(strcmp(recon_theme_current(), "Aqua") == 0,
        "and it comes back after a restart");
}

/*
 * A skin can say what shape a window frame is, within limits.
 *
 * The limits are the point. A skin is a text file somebody edits, and the
 * failure mode of an unchecked number here is not an ugly window -- it is a
 * title bar taller than the screen, or a border of zero with no edge left to
 * grab, and either way a window that cannot be closed by pointing at it.
 */
/*
 * Saying a measurement out loud, and reading one back.
 *
 * Three of the ten are questions rather than amounts and one is a bit set, so
 * "the value" is not always a number somebody would recognise. `buttons` is
 * the sharp case: the file holds 7, and a row of a Control Panel list reading
 * "7" tells nobody which buttons that is.
 *
 * The half that matters most is what is REFUSED. `set_metric` clamps, which is
 * right for a file being read and wrong as the only answer a person gets --
 * somebody who types 4000 should be told the range, not silently given 48 and
 * left thinking it worked.
 */
/*
 * A colour and its ramp, written down and read back.
 *
 * The property the whole design rests on is that these are the SAME text. The
 * list shows "E8E8EC to D4DAE2"; the field is filled with it; what somebody
 * types back is the whole value rather than an edit to part of it. That is
 * what lets one colour mean flat without a second control to say so -- and it
 * is only true if the two functions agree, which is what most of this checks.
 */
static void test_a_colour_and_its_ramp(void) {
    printf("a colour and its ramp, written down and read back\n");

    char said[64];

    recon_theme_ramp_text(0xFF1C1C20u, false, 0, said, sizeof(said));
    check(strcmp(said, "1C1C20") == 0, "a flat colour is six digits");

    recon_theme_ramp_text(0xFFE8E8ECu, true, 0xFFD4DAE2u, said, sizeof(said));
    check(strcmp(said, "E8E8EC to D4DAE2") == 0,
        "and a ramp is the word the list already uses");

    /* Eight digits only when the alpha says something, because FF on the
     * front of every colour makes the common case harder to read. */
    recon_theme_ramp_text(0x80112233u, false, 0, said, sizeof(said));
    check(strcmp(said, "80112233") == 0,
        "an alpha that is not FF is written out");

    /* --- and back --- */

    recon_color from = 0;
    recon_color to = 0;
    bool ramped = true;

    check(recon_theme_ramp_parse("1C1C20", &from, &ramped, &to) &&
        from == 0xFF1C1C20u && !ramped,
        "ONE COLOUR MEANS FLAT -- the field holds the whole value, so what "
        "somebody types replaces the whole value");

    check(recon_theme_ramp_parse("E8E8EC to D4DAE2", &from, &ramped, &to) &&
        from == 0xFFE8E8ECu && ramped && to == 0xFFD4DAE2u,
        "and two colours are a ramp");

    check(recon_theme_ramp_parse("  E8E8EC   to   D4DAE2  ", &from, &ramped,
        &to) && ramped && to == 0xFFD4DAE2u, "spaces anywhere");
    check(recon_theme_ramp_parse("#E8E8EC to #D4DAE2", &from, &ramped, &to) &&
        ramped, "and a hash on either, the way a file may write it");
    check(recon_theme_ramp_parse("e8e8ec TO d4dae2", &from, &ramped, &to) &&
        from == 0xFFE8E8ECu && ramped, "in either case");

    /*
     * The round trip, which is the property the field depends on: what the
     * list shows, typed back, is what the list showed.
     */
    recon_theme_ramp_text(0xFF2A5BC8u, true, 0x804A8BE8u, said, sizeof(said));
    check(recon_theme_ramp_parse(said, &from, &ramped, &to) &&
        from == 0xFF2A5BC8u && ramped && to == 0x804A8BE8u,
        "AND WHAT IT WRITES, IT READS -- including an alpha on the far end");

    /* --- what it will not take --- */

    check(!recon_theme_ramp_parse("E8E8EC to", &from, &ramped, &to),
        "a ramp with nothing to ramp to is refused");
    check(!recon_theme_ramp_parse("to D4DAE2", &from, &ramped, &to),
        "and one with nothing to ramp from");
    check(!recon_theme_ramp_parse("E8E8EC to D4DAE2 extra", &from, &ramped,
        &to), "trailing rubbish is not part of a colour");
    check(!recon_theme_ramp_parse("E8E8EC D4DAE2", &from, &ramped, &to),
        "two colours with no word between them is not a ramp");
    check(!recon_theme_ramp_parse("E8E8EC toD4DAE2", &from, &ramped, &to),
        "and neither is one with no space after the word");
    check(!recon_theme_ramp_parse("E8E8EC tot D4DAE2", &from, &ramped, &to),
        "'tot' is not 'to'");
    check(!recon_theme_ramp_parse("", &from, &ramped, &to),
        "nothing is not a colour");
    check(!recon_theme_ramp_parse("ABC", &from, &ramped, &to),
        "three digits is not a colour, however common that is elsewhere");
    check(!recon_theme_ramp_parse("E8E8ECD", &from, &ramped, &to),
        "and neither is seven");
    check(!recon_theme_ramp_parse(NULL, &from, &ramped, &to),
        "nor nothing at all");

    /*
     * The laxity this replaced. The old parse stopped at the first space and
     * returned what it had, so the junk was silently dropped and somebody who
     * mistyped got a colour they did not ask for with no sign of it.
     */
    recon_color one = 0;
    check(!recon_theme_colour_parse("AABBCC junk", &one),
        "'AABBCC junk' IS REFUSED -- it used to be accepted as AABBCC");
    check(recon_theme_colour_parse("  AABBCC  ", &one) && one == 0xFFAABBCCu,
        "though spaces around it are still fine");
}

static void test_saying_a_measurement(void) {
    printf("saying a measurement, and reading one back\n");

    char said[64];

    recon_theme_metric_text(RECON_METRIC_TITLE_HEIGHT, 24, said, sizeof(said));
    check(strcmp(said, "24") == 0, "a length is its number");

    recon_theme_metric_text(RECON_METRIC_ICON_GLOSS, 1, said, sizeof(said));
    check(strcmp(said, "yes") == 0, "a question is yes");
    recon_theme_metric_text(RECON_METRIC_TINTABLE, 0, said, sizeof(said));
    check(strcmp(said, "no") == 0, "or no");

    recon_theme_metric_text(RECON_METRIC_BUTTONS, 7, said, sizeof(said));
    check(strcmp(said, "close maximize minimize") == 0,
        "AND SEVEN IS THREE BUTTONS BY NAME -- a list reading '7' tells "
        "nobody which buttons that is");

    recon_theme_metric_text(RECON_METRIC_BUTTONS, RECON_BUTTON_CLOSE,
        said, sizeof(said));
    check(strcmp(said, "close") == 0, "and one bit is one word");

    recon_theme_metric_text(RECON_METRIC_BUTTONS,
        RECON_BUTTON_CLOSE | RECON_BUTTON_MINIMIZE, said, sizeof(said));
    check(strcmp(said, "close minimize") == 0,
        "and a gap in the middle does not leave a double space");

    /* --- and back --- */

    int got = -1;
    check(recon_theme_metric_parse(RECON_METRIC_TITLE_HEIGHT, "30", &got) &&
        got == 30, "a number reads as itself");
    check(recon_theme_metric_parse(RECON_METRIC_TITLE_HEIGHT, "  30  ", &got) &&
        got == 30, "with spaces around it");

    check(recon_theme_metric_parse(RECON_METRIC_ICON_GLOSS, "yes", &got) &&
        got == 1, "yes is one");
    check(recon_theme_metric_parse(RECON_METRIC_ICON_GLOSS, "NO", &got) &&
        got == 0, "NO in capitals is zero");
    check(recon_theme_metric_parse(RECON_METRIC_TINTABLE, "1", &got) &&
        got == 1, "and the number still works, because the file holds one");

    check(recon_theme_metric_parse(RECON_METRIC_BUTTONS,
        "close maximize minimize", &got) && got == 7,
        "three names are seven");
    check(recon_theme_metric_parse(RECON_METRIC_BUTTONS,
        "minimize close", &got) &&
        got == (RECON_BUTTON_CLOSE | RECON_BUTTON_MINIMIZE),
        "in any order");
    check(recon_theme_metric_parse(RECON_METRIC_BUTTONS, "Close, Maximize",
        &got) && got == (RECON_BUTTON_CLOSE | RECON_BUTTON_MAXIMIZE),
        "with commas and capitals");
    check(recon_theme_metric_parse(RECON_METRIC_BUTTONS, "7", &got) &&
        got == 7, "and the number, for somebody who has read the file");

    /* --- what it will not take --- */

    check(!recon_theme_metric_parse(RECON_METRIC_TITLE_HEIGHT, "4000", &got),
        "A NUMBER PAST THE RANGE IS REFUSED, NOT CLAMPED -- somebody who "
        "types it can be told, unlike a file");
    check(!recon_theme_metric_parse(RECON_METRIC_TITLE_HEIGHT, "2", &got),
        "and one below it");
    check(!recon_theme_metric_parse(RECON_METRIC_TITLE_HEIGHT, "24px", &got),
        "trailing rubbish is not a number");
    check(!recon_theme_metric_parse(RECON_METRIC_TITLE_HEIGHT, "", &got),
        "and neither is nothing");
    check(!recon_theme_metric_parse(RECON_METRIC_ICON_GLOSS, "maybe", &got),
        "a question takes yes or no, not maybe");
    check(!recon_theme_metric_parse(RECON_METRIC_BUTTONS, "close sideways",
        &got), "a button nobody has is refused rather than ignored");
    check(!recon_theme_metric_parse(RECON_METRIC_BUTTONS, "0", &got),
        "and no buttons at all is refused, not turned into a close button");
    check(!recon_theme_metric_parse(RECON_METRIC_TITLE_HEIGHT, "30", NULL),
        "handed nowhere to put the answer, it declines");

    /* --- the range, and whether the skin said anything --- */

    int least = -1;
    int most = -1;
    int fallback = -1;
    recon_theme_metric_range(RECON_METRIC_TITLE_HEIGHT, &least, &most,
        &fallback);
    check(least == 18 && most == 48 && fallback == 24,
        "the range is askable, so somebody can be told before they type");

    check(recon_theme_set("Classic"), "a skin that keeps the default shape");
    check(!recon_theme_metric_is_set(RECON_METRIC_TITLE_HEIGHT),
        "a skin saying nothing about a measurement is not the same as one "
        "asking for the default");
    check(recon_theme_set("Beacon"), "a skin that asks for its own shape");
    check(recon_theme_metric_is_set(RECON_METRIC_TITLE_HEIGHT),
        "and one that asks says so");
}

static void test_metrics(void) {
    printf("frame shapes\n");

    check(recon_theme_set("Classic"), "a skin that keeps the default shape");
    check(recon_theme_metric(RECON_METRIC_TITLE_HEIGHT) == 24,
        "a skin with no opinion gets the default height");
    check(recon_theme_metric(RECON_METRIC_CORNER) == 0,
        "and square corners");

    check(recon_theme_set("Beacon"), "a skin that asks for its own shape");
    check(recon_theme_metric(RECON_METRIC_TITLE_HEIGHT) == 30,
        "the height it asked for");
    check(recon_theme_metric(RECON_METRIC_CORNER) == 7,
        "the corner it asked for");

    /* Written out and read back: a file is how anybody else would set these. */
    const char *shaped =
        "name = Shaped\n"
        "description = Nothing but a shape\n"
        "metric.title-height = 40\n"
        "metric.corner = 5\n";
    check(recon_fs_write("/", RECON_DIR_THEMES "/Shaped" RECON_THEME_EXT,
        shaped, strlen(shaped)), "write a skin that only sets a shape");

    /* Numbers no window could survive. */
    const char *absurd =
        "name = Absurd\n"
        "description = Numbers nobody should be able to ask for\n"
        "metric.title-height = 4000\n"
        "metric.border = 0\n"
        "metric.button-size = -20\n";
    check(recon_fs_write("/", RECON_DIR_THEMES "/Absurd" RECON_THEME_EXT,
        absurd, strlen(absurd)), "write a skin with impossible numbers");

    recon_theme_finish();
    recon_theme_init();

    check(recon_theme_set("Shaped"), "the shape-only skin loaded");
    check(recon_theme_metric(RECON_METRIC_TITLE_HEIGHT) == 40,
        "a file can set a height");
    check(recon_theme_metric(RECON_METRIC_CORNER) == 5,
        "and a corner");
    check(recon_theme_metric(RECON_METRIC_BORDER) == 3,
        "what it did not set stays the default");
    check(recon_theme_color(RECON_THEME_BAR) ==
        recon_theme_color_of(0, RECON_THEME_BAR),
        "and its colours are the default's, since it named none");

    check(recon_theme_set("Absurd"), "the impossible skin loaded too");
    check(recon_theme_metric(RECON_METRIC_TITLE_HEIGHT) == 48,
        "a title bar of 4000 is clamped to something a window can hold");
    check(recon_theme_metric(RECON_METRIC_BORDER) == 1,
        "a border of zero is clamped to something there is to grab");
    check(recon_theme_metric(RECON_METRIC_BUTTON_SIZE) == 10,
        "a negative button is clamped to one that can be clicked");
}

static void test_files(void) {
    printf("skins from files\n");

    check(recon_theme_write_defaults() == 0,
        "the shipped skins are already written, so nothing is rewritten");

    /*
     * A partial file: it says one thing and inherits the rest. This is what
     * makes "theme only the taskbar" possible without listing every role.
     */
    const char *partial =
        "name = JustTheBar\n"
        "description = Only the taskbar differs\n"
        "bar = 123456\n";

    check(recon_fs_write("/", RECON_DIR_THEMES "/JustTheBar" RECON_THEME_EXT,
        partial, strlen(partial)), "write a skin that sets one role");

    recon_theme_finish();
    recon_theme_init();

    check(recon_theme_set("JustTheBar"), "it loaded and can be chosen");
    check(recon_theme_color(RECON_THEME_BAR) == RECON_RGB(0x12, 0x34, 0x56),
        "the role it set is its own");
    check(recon_theme_color(RECON_THEME_ACCENT) ==
        RECON_RGB(0x8B, 0x1A, 0x1A),
        "and everything it did not mention is inherited, not blank");

    /* Six digits mean opaque; a person writing a colour by hand means opaque
     * unless they say otherwise. */
    check((recon_theme_color(RECON_THEME_BAR) >> 24) == 0xFF,
        "a six-digit colour is fully opaque");

    /* A file cannot quietly replace something that ships with the system. */
    const char *impostor = "name = Recon\nbar = 000000\n";
    check(recon_fs_write("/", RECON_DIR_THEMES "/Impostor" RECON_THEME_EXT,
        impostor, strlen(impostor)), "write a file claiming a built-in name");

    recon_theme_finish();
    recon_theme_init();
    check(recon_theme_set("Recon"), "the built-in is still there");
    check(recon_theme_color(RECON_THEME_BAR) != RECON_RGB(0, 0, 0),
        "and was not replaced by the file");
}

static void test_damaged_file(void) {
    printf("a skin file that is not quite right\n");

    const char *damaged =
        "name = Patchy\n"
        "this line has no equals\n"
        "not-a-role = FFFFFF\n"
        "accent = not a colour\n"
        "warning = 00FF00\n";

    check(recon_fs_write("/", RECON_DIR_THEMES "/Patchy" RECON_THEME_EXT,
        damaged, strlen(damaged)), "write it");

    recon_theme_finish();
    recon_theme_init();

    check(recon_theme_set("Patchy"), "it still loaded");
    check(recon_theme_color(RECON_THEME_WARNING) == RECON_RGB(0x00, 0xFF, 0x00),
        "the good line after the bad ones was read");
    check(recon_theme_color(RECON_THEME_ACCENT) == RECON_RGB(0x8B, 0x1A, 0x1A),
        "the unparseable colour was left at the default, not zeroed");
}

/* --- The see-through frame --- */

static void fades_to(recon_color in, uint8_t alpha, recon_color want,
        const char *what) {
    recon_color got = recon_color_fade(in, alpha);
    char label[160];
    snprintf(label, sizeof(label), "%s: %08X at %u is %08X (got %08X)",
        what, in, (unsigned)alpha, want, got);
    check(got == want, label);
}

static void test_glass(void) {
    printf("Glass\n");

    /*
     * Premultiplied, which is the whole of what this has to get right.
     *
     * Wayland's ARGB8888 has the colour channels already scaled by the alpha,
     * so half-opacity white is half-grey, not white. A version that changed
     * only the alpha byte would give chrome that is see-through *and* too
     * bright -- which reads as a deliberate glow rather than as a fault, and
     * is the reason this is pinned by a number rather than by looking at it.
     */
    fades_to(0xFFFFFFFFu, 128, 0x80808080u, "white at half");
    fades_to(0xFF000000u, 128, 0x80000000u, "black at half stays black");
    fades_to(0xFFFFFFFFu, 0, 0x00000000u, "nothing at zero");

    /* Opaque is a no-op, so a caller may apply it without asking first. */
    fades_to(0xFF3366CCu, 255, 0xFF3366CCu, "full opacity changes nothing");
    fades_to(0x00000000u, 255, 0x00000000u, "already clear, and left alone");

    /*
     * Applied to what is there rather than replacing it, so nesting halves
     * twice. A version that set the alpha instead would pass every check above
     * and fail only where two translucent things overlap.
     */
    recon_color once = recon_color_fade(0xFFFFFFFFu, 128);
    recon_color twice = recon_color_fade(once, 128);
    check((twice >> 24) < (once >> 24), "fading twice is more transparent");

    /*
     * The colours never exceed the alpha. That is the invariant premultiplied
     * really means, and a violation of it is what a compositor renders as a
     * bright halo around otherwise correct chrome.
     */
    bool valid = true;
    for (int a = 0; a <= 255; a += 5) {
        recon_color got = recon_color_fade(0xFFFFFFFFu, (uint8_t)a);
        uint32_t alpha = (got >> 24) & 0xFF;
        for (int shift = 0; shift <= 16; shift += 8) {
            if (((got >> shift) & 0xFF) > alpha) {
                valid = false;
            }
        }
    }
    check(valid, "no channel ever exceeds the alpha, at any opacity");

    /*
     * And the metric, which is what a skin actually sets. The floor is 140
     * rather than 0 because a skin that can make a title unreadable is a skin
     * somebody installs once -- so a file asking for 40 gets 140, not 40.
     */
    check(recon_theme_metric(RECON_METRIC_CHROME_OPACITY) >= 140,
        "chrome never fades past what can still be read");
}

/* --- Tints --- */

static void test_tint(void) {
    printf("Tints\n");

    const recon_color AMBER = RECON_RGB(0xC8, 0x90, 0x3A);

    /* Nothing at zero, so a caller need not ask whether a tint is set. */
    check(recon_color_tint(0xFF3366CCu, AMBER, 0) == 0xFF3366CCu,
        "strength zero changes nothing");

    /*
     * Lightness is kept. This is the property the whole design rests on: a
     * palette's *structure* is in its lightness -- which surfaces sit above
     * which, which text reads against which -- and only its appearance is in
     * its hue. A tint that moved lightness would change whether the skin
     * works, not what it looks like.
     *
     * Checked across the range rather than at one value, because the two sides
     * of the calculation are different code: below the tint's own lightness it
     * scales towards black, above it towards white, and an error in either one
     * is invisible from the other side of the branch.
     */
    bool kept = true;
    int worst = 0;
    for (int v = 0; v <= 255; v += 5) {
        recon_color grey = RECON_RGB(v, v, v);
        recon_color got = recon_color_tint(grey, AMBER, 255);

        int before = recon_color_luminance(grey);
        int after = recon_color_luminance(got);
        int off = (after > before) ? after - before : before - after;

        if (off > worst) {
            worst = off;
        }
        if (off > 6) {
            kept = false;
        }
    }
    char label[128];
    snprintf(label, sizeof(label),
        "lightness survives the tint at every level (worst %d)", worst);
    check(kept, label);

    /* And the hue actually arrives. A mid grey tinted amber must come out
     * warmer than it went in, or the whole thing is an expensive no-op. */
    recon_color warm = recon_color_tint(RECON_RGB(0x80, 0x80, 0x80),
        AMBER, 200);
    check(((warm >> 16) & 0xFF) > (warm & 0xFF) + 20,
        "a grey tinted amber comes out warm");

    recon_color cool = recon_color_tint(RECON_RGB(0x80, 0x80, 0x80),
        RECON_RGB(0x4A, 0x86, 0xC8), 200);
    check((cool & 0xFF) > ((cool >> 16) & 0xFF) + 20,
        "a grey tinted blue comes out cool");

    /*
     * Black and white have no room to take a hue and must not be given one.
     * They are the ends of every palette's range, and a tint that lifted black
     * off zero would raise the floor of every skin it touched.
     */
    check((recon_color_tint(RECON_RGB(0, 0, 0), AMBER, 255) & 0xFFFFFFu) == 0,
        "black stays black");
    check((recon_color_tint(RECON_RGB(255, 255, 255), AMBER, 255) & 0xFFFFFFu)
        == 0xFFFFFFu, "white stays white");

    /* A tint changes what colour a thing is, not whether it is there. */
    check((recon_color_tint(0x80336699u, AMBER, 255) >> 24) == 0x80u,
        "the alpha is left alone");

    /*
     * And the rule that matters most, which is not about colour at all: a skin
     * whose palette was chosen for colour vision must not accept a tint. Glass
     * is the only skin that does.
     */
    check(recon_theme_set("Deuteran"), "Deuteran can be put on");
    check(!recon_tint_available(), "a colour-vision skin takes no tint");
    check(!recon_tint_set("Amber"), "and refuses one when asked");

    check(recon_theme_set("Glass"), "Glass can be put on");
    check(recon_tint_available(), "Glass takes a tint");
    check(recon_tint_set("Amber"), "and accepts one");
    check(strcasecmp(recon_tint_current(), "Amber") == 0, "which is remembered");

    check(!recon_tint_set("Chartreuse"), "a tint nobody defined is refused");
    check(strcasecmp(recon_tint_current(), "Amber") == 0,
        "and the one in use is left alone");

    check(recon_tint_set("none"), "'none' takes it off");
    check(recon_tint_current()[0] == '\0', "and then there is none");
}

int main(void) {
    char root[] = "/tmp/reconos-theme-XXXXXX";
    if (mkdtemp(root) == NULL) {
        perror("mkdtemp");
        return 1;
    }

    printf("ReconOS skin tests, root %s\n\n", root);

    if (!recon_fs_init(root)) {
        printf("could not set up the test filesystem: %s\n", recon_fs_last_error());
        return 1;
    }
    recon_registry_init();
    recon_theme_write_defaults();
    recon_theme_init();

    test_roles_are_all_answered();
    test_no_role_is_unset();
    test_out_of_range_is_loud();
    test_switching();
    test_remembered();
    test_files();
    test_metrics();
    test_saying_a_measurement();
    test_a_colour_and_its_ramp();
    test_glass();
    test_tint();
    test_damaged_file();

    recon_theme_finish();
    recon_registry_finish();
    recon_fs_finish();

    char command[512];
    snprintf(command, sizeof(command), "rm -rf '%s'", root);
    if (system(command) != 0) {
        printf("\nnote: could not remove %s\n", root);
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
