/*
 * Do the accessibility skins actually work?
 *
 * Picking colours that look distinguishable to someone with ordinary colour
 * vision proves nothing about the people the skins are for. So each one is put
 * through a simulation of the deficiency it was made for, and the pairs that
 * must stay apart are measured afterwards.
 *
 * The simulation is the Vienot/Brettel dichromat projection, applied in linear
 * light, which is the standard approach. Distances are CIE76 dE in Lab, which
 * is crude by modern standards but close enough to separate "obviously
 * different" from "the same colour".
 *
 * This is a check on the palettes, not a claim of accessibility. A palette is
 * half of it; the other half is never encoding meaning in colour alone, which
 * no test can assert and which is why "Not responding" says so in words.
 *
 * Run with: ninja -C build && ./build/recon_access_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "recon_fs.h"
#include "recon_registry.h"
#include "recon_theme.h"

static int g_failures;
static int g_checks;

/* --- Colour maths --- */

struct rgb { double r, g, b; };

static double srgb_to_linear(double value) {
    return value <= 0.04045 ? value / 12.92
                            : pow((value + 0.055) / 1.055, 2.4);
}

static struct rgb unpack(recon_color c) {
    struct rgb out;
    out.r = srgb_to_linear(((c >> 16) & 0xFF) / 255.0);
    out.g = srgb_to_linear(((c >> 8) & 0xFF) / 255.0);
    out.b = srgb_to_linear((c & 0xFF) / 255.0);
    return out;
}

enum deficiency { VISION_NORMAL, VISION_PROTAN, VISION_DEUTAN, VISION_TRITAN };

/*
 * Project a colour onto what a dichromat can distinguish.
 *
 * These matrices operate on linear light, which is why the conversion above
 * matters -- applying them to gamma-encoded values, as a lot of code does,
 * gives noticeably wrong answers.
 */
static struct rgb simulate(struct rgb in, enum deficiency kind) {
    static const double PROTAN[9] = {
        0.152286, 1.052583, -0.204868,
        0.114503, 0.786281,  0.099216,
       -0.003882, -0.048116, 1.051998,
    };
    static const double DEUTAN[9] = {
        0.367322, 0.860646, -0.227968,
        0.280085, 0.672501,  0.047413,
       -0.011820, 0.042940,  0.968881,
    };
    static const double TRITAN[9] = {
        1.255528, -0.076749, -0.178779,
       -0.078411,  0.930809,  0.147602,
        0.004733,  0.691367,  0.303900,
    };

    const double *m;
    switch (kind) {
    case VISION_PROTAN: m = PROTAN; break;
    case VISION_DEUTAN: m = DEUTAN; break;
    case VISION_TRITAN: m = TRITAN; break;
    default: return in;
    }

    struct rgb out;
    out.r = m[0] * in.r + m[1] * in.g + m[2] * in.b;
    out.g = m[3] * in.r + m[4] * in.g + m[5] * in.b;
    out.b = m[6] * in.r + m[7] * in.g + m[8] * in.b;
    return out;
}

/* Lab, so distances mean roughly what a person would say. */
struct lab { double l, a, b; };

static double lab_f(double t) {
    return t > 0.008856 ? cbrt(t) : (7.787 * t) + (16.0 / 116.0);
}

static struct lab to_lab(struct rgb linear) {
    /* Clamp: a simulation can push a value slightly outside the gamut. */
    double r = linear.r < 0 ? 0 : (linear.r > 1 ? 1 : linear.r);
    double g = linear.g < 0 ? 0 : (linear.g > 1 ? 1 : linear.g);
    double b = linear.b < 0 ? 0 : (linear.b > 1 ? 1 : linear.b);

    double x = (0.4124 * r + 0.3576 * g + 0.1805 * b) / 0.95047;
    double y = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 1.00000;
    double z = (0.0193 * r + 0.1192 * g + 0.9505 * b) / 1.08883;

    struct lab out;
    out.l = 116.0 * lab_f(y) - 16.0;
    out.a = 500.0 * (lab_f(x) - lab_f(y));
    out.b = 200.0 * (lab_f(y) - lab_f(z));
    return out;
}

/* How different two colours look, to someone with the given vision. */
static double difference(recon_color a, recon_color b, enum deficiency kind) {
    struct lab la = to_lab(simulate(unpack(a), kind));
    struct lab lb = to_lab(simulate(unpack(b), kind));

    double dl = la.l - lb.l;
    double da = la.a - lb.a;
    double db = la.b - lb.b;
    return sqrt(dl * dl + da * da + db * db);
}

/* --- Checks --- */

/*
 * Thresholds. Around 2.3 is the point two colours stop looking identical;
 * these are far above that because the question is not "can a difference be
 * detected side by side" but "can these be told apart at a glance, in
 * different parts of the screen".
 */
#define APART 25.0     /* two things that mean different things */
#define READABLE 45.0  /* text against what it sits on */

static void check_pair(const char *skin, enum deficiency kind,
        const char *what, enum recon_theme_role a, enum recon_theme_role b,
        double threshold) {
    g_checks++;
    double d = difference(recon_theme_color(a), recon_theme_color(b), kind);
    if (d < threshold) {
        g_failures++;
        printf("  FAIL: %s: %s only differ by %.1f (wanted %.0f)\n",
            skin, what, d, threshold);
    }
}

static const char *vision_name(enum deficiency kind) {
    switch (kind) {
    case VISION_PROTAN: return "protanopia";
    case VISION_DEUTAN: return "deuteranopia";
    case VISION_TRITAN: return "tritanopia";
    default: return "ordinary vision";
    }
}

/*
 * The pairs that carry meaning.
 *
 * Each is somewhere the system says something with colour: a folder name
 * against an ordinary one, a warning against ordinary text, a selected row
 * against an unselected one.
 */
static void check_skin(const char *skin, enum deficiency kind, bool hueless) {
    if (!recon_theme_set(skin)) {
        g_checks++;
        g_failures++;
        printf("  FAIL: no skin called '%s'\n", skin);
        return;
    }

    printf("  %-10s under %s%s\n", skin, vision_name(kind),
        hueless ? " (nothing carried by hue, by design)" : "");

    /* Text has to be readable before anything else matters. */
    check_pair(skin, kind, "text on its surface",
        RECON_THEME_SURFACE_TEXT, RECON_THEME_SURFACE, READABLE);
    /*
     * Each label against the surface it is actually drawn on.
     *
     * This used to be one check pairing bar text with the bar itself, which
     * is a surface no label is ever drawn on: every label on the taskbar sits
     * on a button. The bar and the buttons are near-identical greys in most
     * skins, so the wrong pairing passed and looked right -- until a skin
     * arrived with a deep blue bar and light buttons, where white bar text
     * measured as excellent against the bar and was invisible on every button
     * on screen. Getting the pairs right then found the same fault in the
     * high-contrast skin, which is the one place it should have been
     * impossible.
     */
    check_pair(skin, kind, "the current window's name on its button",
        RECON_THEME_BAR_TEXT, RECON_THEME_BUTTON_ACTIVE, READABLE);
    check_pair(skin, kind, "a background window's name on its button",
        RECON_THEME_BAR_TEXT_DIM, RECON_THEME_BUTTON, READABLE);
    check_pair(skin, kind, "a button's own label on the button",
        RECON_THEME_BUTTON_TEXT, RECON_THEME_BUTTON, READABLE);
    check_pair(skin, kind, "menu text on the menu",
        RECON_THEME_MENU_TEXT, RECON_THEME_MENU, READABLE);
    check_pair(skin, kind, "selected text on the selection",
        RECON_THEME_SELECTION_TEXT, RECON_THEME_SELECTION, READABLE);
    check_pair(skin, kind, "title text on an active title bar",
        RECON_THEME_TITLE_TEXT, RECON_THEME_TITLE_ACTIVE, READABLE);
    /*
     * And on an inactive one, which was never checked. The same omission as
     * the taskbar labels: a pair that is drawn on screen and was not among
     * the pairs measured. A window you are not typing into still has to say
     * which window it is.
     */
    check_pair(skin, kind, "title text on an inactive title bar",
        RECON_THEME_TITLE_TEXT_INACTIVE, RECON_THEME_TITLE_INACTIVE, READABLE);

    /*
     * A skin that deliberately says nothing with hue is not held to the
     * checks below. The high-contrast skin is black on white throughout, so a
     * folder name and a file name are the same colour on purpose -- what
     * distinguishes them is the icon and the Type column, which are there
     * precisely so colour never has to carry it alone.
     *
     * That is the whole point rather than a gap: everything below is a check
     * that a *coloured* distinction survives, and this skin makes none.
     */
    if (hueless) {
        check_pair(skin, kind, "an active title bar against an inactive one",
            RECON_THEME_TITLE_ACTIVE, RECON_THEME_TITLE_INACTIVE, APART);
        check_pair(skin, kind, "a selected row against an unselected one",
            RECON_THEME_SELECTION, RECON_THEME_SURFACE, APART);
        return;
    }

    /* Then the places where the colour itself is the message. */
    check_pair(skin, kind, "a folder name against a file name",
        RECON_THEME_DIRECTORY, RECON_THEME_SURFACE_TEXT, APART);
    check_pair(skin, kind, "a warning against ordinary text",
        RECON_THEME_WARNING, RECON_THEME_SURFACE_TEXT, APART);
    check_pair(skin, kind, "a warning against a folder name",
        RECON_THEME_DIRECTORY, RECON_THEME_WARNING, APART);
    check_pair(skin, kind, "a selected row against an unselected one",
        RECON_THEME_SELECTION, RECON_THEME_SURFACE, APART);
    check_pair(skin, kind, "an active title bar against an inactive one",
        RECON_THEME_TITLE_ACTIVE, RECON_THEME_TITLE_INACTIVE, APART);

    /*
     * The defect that started this: an accent and a warning were the same
     * colour, so "highlighted" and "wrong" looked identical to everybody.
     */
    check_pair(skin, kind, "an accent against a warning",
        RECON_THEME_ACCENT, RECON_THEME_WARNING, APART);
}

int main(void) {
    char root[] = "/tmp/reconos-access-XXXXXX";
    if (mkdtemp(root) == NULL) {
        perror("mkdtemp");
        return 1;
    }

    printf("ReconOS accessibility checks, root %s\n\n", root);

    if (!recon_fs_init(root)) {
        printf("could not set up the test filesystem\n");
        return 1;
    }
    recon_registry_init();
    recon_theme_init();

    printf("each skin under the vision it was made for\n");
    check_skin("Deuteran", VISION_DEUTAN, false);
    check_skin("Protan", VISION_PROTAN, false);
    check_skin("Tritan", VISION_TRITAN, false);

    /*
     * Contrast carries everything in luminance, so it should hold up under
     * all three -- that is the whole idea of it, and the check that proves it
     * is not just another coloured skin.
     */
    printf("\nthe high-contrast skin, under every deficiency\n");
    check_skin("Contrast", VISION_DEUTAN, true);
    check_skin("Contrast", VISION_PROTAN, true);
    check_skin("Contrast", VISION_TRITAN, true);

    /*
     * And the ordinary skins with ordinary vision. These are not accessibility
     * skins and are not held to the same standard under simulation, but they
     * must at least be legible and internally distinct as designed.
     */
    printf("\nthe everyday skins, with ordinary vision\n");
    check_skin("Recon", VISION_NORMAL, false);
    check_skin("Classic", VISION_NORMAL, false);
    check_skin("Aqua", VISION_NORMAL, false);
    check_skin("Midnight", VISION_NORMAL, false);
    check_skin("Reading", VISION_NORMAL, false);

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
