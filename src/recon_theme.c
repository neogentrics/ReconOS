/*
 * Skins. See include/recon_theme.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "recon_fs.h"
#include "recon_registry.h"
#include "recon_theme.h"

#define THEMES_MAX 16

struct theme {
    struct recon_theme_info info;
    recon_color colors[RECON_THEME_ROLE_COUNT];

    /*
     * The far end of a gradient, per role, and whether there is one.
     *
     * A separate flag rather than a sentinel colour, because every colour is
     * a legal answer -- including one equal to the first, which a skin might
     * reasonably write while tuning a ramp down to nothing.
     */
    recon_color gradient[RECON_THEME_ROLE_COUNT];
    bool has_gradient[RECON_THEME_ROLE_COUNT];

    bool used;
};

/*
 * A gradient a built-in skin declares, as a sparse list.
 *
 * Sparse because a handful of surfaces want one out of forty-eight roles, and
 * writing the other forty-two as "no gradient" in every skin would bury the
 * few that matter.
 */
struct gradient_spec {
    enum recon_theme_role role;
    recon_color to;
};

static struct theme g_themes[THEMES_MAX];
static int g_count;
static int g_current = -1;
static unsigned g_generation;
static char g_error[256];

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, args);
    va_end(args);
}

const char *recon_theme_last_error(void) {
    return g_error[0] != '\0' ? g_error : "no error";
}

/* --- Role names --- */

/*
 * The name of every role, in enum order.
 *
 * Dotted rather than underscored, so a theme file reads as groups: everything
 * under "title." is the title bar. The compile-time check below is what stops
 * this drifting out of step with the enum, which would silently map every role
 * after the gap to the wrong colour.
 */
static const char *const ROLE_NAMES[] = {
    "window.frame",
    "window.edge",
    "title.active",
    "title.inactive",
    "title.text",
    "title.text-inactive",
    "window.button",
    "window.button-glyph",

    "bar",
    "bar.text",
    "bar.text-dim",
    "button",
    "button.active",
    "button.text",

    "menu",
    "menu.border",
    "menu.text",
    "menu.text-disabled",
    "menu.hilite",
    "menu.hilite-text",
    "menu.separator",

    "dialog",
    "dialog.title",
    "dialog.title-text",
    "dim",

    "surface",
    "surface.alt",
    "surface.text",
    "surface.text-dim",
    "surface.header",
    "selection",
    "selection.text",

    "field",
    "field.border",
    "field.text",
    "field.selection",
    "caret",

    "readout",
    "readout.text",
    "readout.accent",
    "readout.input",

    "desktop.label",
    "desktop.label-shadow",
    "desktop.selection",

    "accent",
    "accent.text",
    "warning",
    "directory",
};

_Static_assert(
    sizeof(ROLE_NAMES) / sizeof(ROLE_NAMES[0]) == RECON_THEME_ROLE_COUNT,
    "every role needs a name, in the same order as the enum");

const char *recon_theme_role_name(enum recon_theme_role role) {
    if (role < 0 || role >= RECON_THEME_ROLE_COUNT) {
        return NULL;
    }
    return ROLE_NAMES[role];
}

static int role_from_name(const char *name) {
    for (int i = 0; i < RECON_THEME_ROLE_COUNT; i++) {
        if (strcasecmp(ROLE_NAMES[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

/* --- The skins that ship --- */

/*
 * Written out in role order, one line each, so a missing entry is a visible
 * gap rather than a silent shift. Designated initialisers would survive
 * reordering, but they would also let a role be forgotten entirely, and a
 * forgotten role draws as nothing.
 */
#define RGB(r, g, b) RECON_RGB(0x##r, 0x##g, 0x##b)
#define RGBA(r, g, b, a) RECON_RGBA(0x##r, 0x##g, 0x##b, 0x##a)

/* The native look: grey chrome, deep navy titles, oxblood accent. */
static const recon_color THEME_RECON[] = {
    RGB(C0,C0,C0), RGB(30,30,30), RGB(20,2A,44), RGB(6A,6A,72),
    RGB(F0,F0,F0), RGB(D0,D0,D4), RGB(C8,C8,C8), RGB(10,10,10),

    RGB(C0,C0,C0), RGB(10,10,10), RGB(40,40,40), RGB(C8,C8,C8),
    RGB(A8,A8,B4),
    RGB(10,10,10),

    RGB(C8,C8,C8), RGB(30,30,30), RGB(10,10,10), RGB(40,40,40),
    RGB(30,50,90), RGB(FF,FF,FF), RGB(90,90,90),

    RGB(C0,C0,C0), RGB(20,2A,44), RGB(F0,F0,F0), RGBA(00,00,00,99),

    RGB(FF,FF,FF), RGB(F2,F2,F6), RGB(10,10,10), RGB(30,30,30),
    RGB(D4,D4,D4), RGB(30,50,90), RGB(FF,FF,FF),

    RGB(FF,FF,FF), RGB(30,50,90), RGB(10,10,10), RGB(B0,C8,F0),
    RGB(10,10,10),

    RGB(0C,10,18), RGB(C8,D4,C8), RGB(7C,C8,7C), RGB(F0,F0,E8),

    RGB(F0,F0,F0), RGBA(00,00,00,C0), RGBA(30,50,90,A0),

    /* Accent and warning were the same oxblood, which made "this is
     * highlighted" and "this is a problem" the same colour for everybody, not
     * only for someone who cannot separate reds. The warning is brighter and
     * hotter now, and differs in luminance as well as hue. */
    RGB(8B,1A,1A), RGB(F4,F4,F4), RGB(D0,42,1B), RGB(1A,3A,8B),
};

/* The 95-era look: brighter grey, the familiar navy, no softening. */
static const recon_color THEME_CLASSIC[] = {
    RGB(C0,C0,C0), RGB(00,00,00), RGB(00,00,80), RGB(80,80,80),
    RGB(FF,FF,FF), RGB(C0,C0,C0), RGB(C0,C0,C0), RGB(00,00,00),

    RGB(C0,C0,C0), RGB(00,00,00), RGB(3A,3A,3A), RGB(C0,C0,C0),
    RGB(A0,A0,A0),
    RGB(00,00,00),

    RGB(C0,C0,C0), RGB(00,00,00), RGB(00,00,00), RGB(80,80,80),
    RGB(00,00,80), RGB(FF,FF,FF), RGB(80,80,80),

    RGB(C0,C0,C0), RGB(00,00,80), RGB(FF,FF,FF), RGBA(00,00,00,80),

    RGB(FF,FF,FF), RGB(FF,FF,FF), RGB(00,00,00), RGB(40,40,40),
    RGB(C0,C0,C0), RGB(00,00,80), RGB(FF,FF,FF),

    RGB(FF,FF,FF), RGB(80,80,80), RGB(00,00,00), RGB(00,00,80),
    RGB(00,00,00),

    RGB(00,00,00), RGB(C0,C0,C0), RGB(00,FF,00), RGB(FF,FF,FF),

    RGB(00,00,00), RGBA(FF,FF,FF,C8), RGBA(00,00,80,A0),

    RGB(00,00,80), RGB(FF,FF,FF), RGB(80,00,00), RGB(00,00,80),
};

/* Light and quiet: pale chrome, blue selection, thin edges. */
static const recon_color THEME_AQUA[] = {
    /* The active and inactive title bars were seven units apart, which is
     * to say indistinguishable. A subtle look is not worth not knowing which
     * window has the keyboard. */
    RGB(EC,EC,EE), RGB(B8,B8,BC), RGB(8F,B4,DC), RGB(F2,F2,F4),
    RGB(1C,1C,20), RGB(90,90,96), RGB(E4,E4,E8), RGB(40,40,46),

    RGB(E8,E8,EC), RGB(1C,1C,20), RGB(56,56,5E), RGB(E4,E4,E8),
    RGB(D0,D4,DC),
    RGB(1C,1C,20),

    RGB(F4,F4,F6), RGB(C0,C0,C6), RGB(1C,1C,20), RGB(A0,A0,A6),
    RGB(2A,6C,E0), RGB(FF,FF,FF), RGB(D8,D8,DC),

    RGB(F4,F4,F6), RGB(DC,DC,E0), RGB(1C,1C,20), RGBA(00,00,00,55),

    RGB(FF,FF,FF), RGB(F7,F7,F9), RGB(1C,1C,20), RGB(70,70,78),
    RGB(EE,EE,F0), RGB(2A,6C,E0), RGB(FF,FF,FF),

    RGB(FF,FF,FF), RGB(2A,6C,E0), RGB(1C,1C,20), RGB(C4,DA,FA),
    RGB(1C,1C,20),

    RGB(1E,1E,22), RGB(E0,E0,E4), RGB(64,C8,96), RGB(FF,FF,FF),

    RGB(1C,1C,20), RGBA(FF,FF,FF,C8), RGBA(2A,6C,E0,90),

    RGB(2A,6C,E0), RGB(FF,FF,FF), RGB(C0,3A,2A), RGB(2A,6C,E0),
};

/* Dark and flat, the way a modern Linux desktop tends to look. */
static const recon_color THEME_MIDNIGHT[] = {
    /* Active and inactive were three units apart -- a flat look taken far
     * enough to stop conveying anything. */
    RGB(2E,32,38), RGB(1A,1C,20), RGB(2F,4F,7A), RGB(2A,2E,34),
    RGB(E8,E8,EC), RGB(90,94,9C), RGB(3A,3E,46), RGB(E0,E0,E4),

    RGB(24,28,2E), RGB(E0,E0,E4), RGB(B8,BC,C4), RGB(3A,3E,46),
    RGB(4E,54,5E),
    RGB(DC,DC,E0),

    RGB(32,36,3E), RGB(16,18,1C), RGB(E4,E4,E8), RGB(78,7C,84),
    RGB(3E,7A,C8), RGB(FF,FF,FF), RGB(4A,4E,56),

    RGB(2E,32,38), RGB(24,28,2E), RGB(E8,E8,EC), RGBA(00,00,00,A0),

    RGB(1E,20,24), RGB(24,26,2A), RGB(DC,DC,E0), RGB(94,98,A0),
    RGB(2A,2E,34), RGB(3E,7A,C8), RGB(FF,FF,FF),

    RGB(18,1A,1E), RGB(3E,7A,C8), RGB(E4,E4,E8), RGB(2A,4A,78),
    RGB(E4,E4,E8),

    RGB(12,14,18), RGB(C8,D4,C8), RGB(7C,C8,7C), RGB(F0,F0,E8),

    RGB(F0,F0,F2), RGBA(00,00,00,C0), RGBA(3E,7A,C8,A0),

    /* Accent was a burnt orange and warning a salmon: nine units apart, the
     * same defect the default skin had. The accent is blue now, which also
     * puts it in the same family as this skin's selection. */
    RGB(5A,9C,E0), RGB(10,14,18), RGB(E0,64,50), RGB(6E,A8,E8),
};

/*
 * --- Skins for colour vision deficiency ---
 *
 * "Colour blind" is not one thing, and a single mode for it would be wrong:
 * protan and tritan need opposite decisions. There are four here because
 * there are four problems.
 *
 * The chrome stays neutral grey in all of them, because grey is unambiguous
 * for everyone. What changes is the handful of places where a colour carries
 * meaning -- selection, folder names, warnings, the accent.
 *
 * The semantic colours are taken from the Okabe-Ito colour-universal set
 * (Okabe & Ito, 2008), which was chosen for exactly this: hues that stay
 * separable across the common deficiencies. They are verified rather than
 * trusted -- tests/test_theme.c simulates each deficiency and checks the
 * pairs that must stay apart really do.
 *
 * A palette is only half of it. The other half is not encoding meaning in
 * colour alone, which is why "Not responding" says so in words and a folder
 * has an icon and a Type column as well as a colour.
 */

/* Red-green, the common kind. Blue and orange are the reliable axis. */
static const recon_color THEME_DEUTERAN[] = {
    RGB(C8,C8,C8), RGB(30,30,30), RGB(00,54,8A), RGB(7A,7A,7A),
    RGB(FF,FF,FF), RGB(DC,DC,DC), RGB(D0,D0,D0), RGB(10,10,10),

    RGB(C8,C8,C8), RGB(10,10,10), RGB(55,55,55), RGB(D0,D0,D0),
    RGB(A8,B4,C4),
    RGB(10,10,10),

    RGB(D4,D4,D4), RGB(30,30,30), RGB(10,10,10), RGB(6E,6E,6E),
    RGB(00,72,B2), RGB(FF,FF,FF), RGB(90,90,90),

    RGB(C8,C8,C8), RGB(00,54,8A), RGB(FF,FF,FF), RGBA(00,00,00,99),

    RGB(FF,FF,FF), RGB(F0,F0,F0), RGB(10,10,10), RGB(4A,4A,4A),
    RGB(DC,DC,DC), RGB(00,72,B2), RGB(FF,FF,FF),

    RGB(FF,FF,FF), RGB(00,72,B2), RGB(10,10,10), RGB(A8,D0,F0),
    RGB(10,10,10),

    RGB(10,14,18), RGB(E0,E0,E0), RGB(56,B4,E9), RGB(FF,FF,FF),

    RGB(FF,FF,FF), RGBA(00,00,00,C0), RGBA(00,72,B2,A0),

    RGB(E6,9F,00), RGB(10,10,10), RGB(9A,3A,00), RGB(00,72,B2),
};

/*
 * Protan is red-green too, but red also looks darker, so a dark red reads as
 * near-black rather than as a colour at all. Everything that has to be seen
 * is lighter here than in the deuteran set.
 */
static const recon_color THEME_PROTAN[] = {
    RGB(C8,C8,C8), RGB(30,30,30), RGB(0A,66,99), RGB(7A,7A,7A),
    RGB(FF,FF,FF), RGB(DC,DC,DC), RGB(D0,D0,D0), RGB(10,10,10),

    RGB(C8,C8,C8), RGB(10,10,10), RGB(55,55,55), RGB(D0,D0,D0),
    RGB(A8,C0,CC),
    RGB(10,10,10),

    RGB(D4,D4,D4), RGB(30,30,30), RGB(10,10,10), RGB(6E,6E,6E),
    RGB(1B,8F,D6), RGB(FF,FF,FF), RGB(90,90,90),

    RGB(C8,C8,C8), RGB(0A,66,99), RGB(FF,FF,FF), RGBA(00,00,00,99),

    /*
     * A lighter blue than the deuteran set's, all the way through. That skin
     * says everything meant to be seen is lighter here, because protan makes
     * dark colours read darker still -- and then it used the identical blue,
     * so the two skins were indistinguishable and the promise was empty.
     */
    RGB(FF,FF,FF), RGB(F0,F0,F0), RGB(10,10,10), RGB(4A,4A,4A),
    RGB(DC,DC,DC), RGB(1B,8F,D6), RGB(FF,FF,FF),

    RGB(FF,FF,FF), RGB(1B,8F,D6), RGB(10,10,10), RGB(B8,DC,F4),
    RGB(10,10,10),

    RGB(10,14,18), RGB(E0,E0,E0), RGB(56,B4,E9), RGB(FF,FF,FF),

    RGB(FF,FF,FF), RGBA(00,00,00,C0), RGBA(1B,8F,D6,A0),

    RGB(F0,B4,20), RGB(10,10,10), RGB(B8,5C,00), RGB(1B,8F,D6),
};

/*
 * Tritan confuses blue with green and yellow with pink, so the blue-orange
 * axis the other two rely on is exactly the wrong choice. Red, green and
 * magenta stay separable.
 */
static const recon_color THEME_TRITAN[] = {
    RGB(C8,C8,C8), RGB(30,30,30), RGB(7A,10,30), RGB(7A,7A,7A),
    RGB(FF,FF,FF), RGB(DC,DC,DC), RGB(D0,D0,D0), RGB(10,10,10),

    RGB(C8,C8,C8), RGB(10,10,10), RGB(55,55,55), RGB(D0,D0,D0),
    RGB(C4,A8,B4),
    RGB(10,10,10),

    RGB(D4,D4,D4), RGB(30,30,30), RGB(10,10,10), RGB(6E,6E,6E),
    RGB(8A,0F,3C), RGB(FF,FF,FF), RGB(90,90,90),

    RGB(C8,C8,C8), RGB(7A,10,30), RGB(FF,FF,FF), RGBA(00,00,00,99),

    RGB(FF,FF,FF), RGB(F0,F0,F0), RGB(10,10,10), RGB(4A,4A,4A),
    RGB(DC,DC,DC), RGB(8A,0F,3C), RGB(FF,FF,FF),

    RGB(FF,FF,FF), RGB(8A,0F,3C), RGB(10,10,10), RGB(F0,B8,CC),
    RGB(10,10,10),

    RGB(10,14,14), RGB(E0,E0,E0), RGB(00,C0,60), RGB(FF,FF,FF),

    RGB(FF,FF,FF), RGBA(00,00,00,C0), RGBA(8A,0F,3C,A0),

    RGB(B0,00,6A), RGB(FF,FF,FF), RGB(D0,20,20), RGB(00,73,3C),
};

/*
 * Maximum contrast: no mid tones, nothing conveyed by hue at all.
 *
 * For low vision, and for achromatopsia, where only luminance carries any
 * information. Everything that must be told apart is told apart by black
 * against white.
 */
static const recon_color THEME_CONTRAST[] = {
    RGB(FF,FF,FF), RGB(00,00,00), RGB(00,00,00), RGB(FF,FF,FF),
    RGB(FF,FF,FF), RGB(00,00,00), RGB(FF,FF,FF), RGB(00,00,00),

    RGB(00,00,00), RGB(FF,FF,FF), RGB(3C,3C,3C), RGB(FF,FF,FF),
    RGB(00,00,00),
    RGB(00,00,00),

    RGB(FF,FF,FF), RGB(00,00,00), RGB(00,00,00), RGB(70,70,70),
    RGB(00,00,00), RGB(FF,FF,FF), RGB(00,00,00),

    RGB(FF,FF,FF), RGB(00,00,00), RGB(FF,FF,FF), RGBA(00,00,00,C8),

    RGB(FF,FF,FF), RGB(FF,FF,FF), RGB(00,00,00), RGB(40,40,40),
    RGB(00,00,00), RGB(00,00,00), RGB(FF,FF,FF),

    RGB(FF,FF,FF), RGB(00,00,00), RGB(00,00,00), RGB(B0,B0,B0),
    RGB(00,00,00),

    RGB(00,00,00), RGB(FF,FF,FF), RGB(FF,FF,FF), RGB(FF,FF,FF),

    RGB(00,00,00), RGBA(FF,FF,FF,FF), RGBA(00,00,00,C0),

    RGB(00,00,00), RGB(FF,FF,FF), RGB(00,00,00), RGB(00,00,00),
};

/*
 * Softened for reading.
 *
 * Pure black on pure white is the highest contrast available and, for a good
 * number of dyslexic readers, uncomfortable with it -- text appears to shimmer
 * or swim. A warm off-white with dark grey rather than black is the usual
 * remedy, and costs nothing to anyone who does not need it.
 *
 * Meant to be worn together with `access reading`, which does the spacing.
 * Colour and spacing are separate settings because they help separately.
 */
/*
 * Bright blue chrome and a green accent: the look people remember fondly
 * from the early 2000s, in our own colours.
 *
 * Not a copy of one. That era's look was built on gradients and rounded
 * corners, and ReconOS draws flat fills with square edges -- so this is the
 * palette and the *feeling*, not the shapes. Reproducing it exactly would
 * mean lifting somebody's design as well as needing chrome geometry the skin
 * system does not have.
 *
 * The point of it is comfort. A system that feels familiar is one people are
 * willing to try, and there is no reason the familiar option has to be the
 * default to be worth having.
 */
static const recon_color THEME_BEACON[] = {
    RGB(E8,EA,F0), RGB(6E,84,B4), RGB(2A,5B,C8), RGB(8C,A4,C8),
    RGB(FF,FF,FF), RGB(E4,EA,F4), RGB(3A,6E,D8), RGB(FF,FF,FF),

    /* bar.text and bar.text-dim are drawn on a taskbar button, and this
     * skin's buttons are light -- so they are dark here, despite the bar
     * behind them being deep blue. White was chosen for the bar and left
     * every button label white on near-white. */
    RGB(2A,5B,C8), RGB(16,20,32), RGB(74,86,A8), RGB(E0,E6,F2),
    RGB(C0,D0,EC),
    RGB(16,20,32),

    RGB(F4,F6,FA), RGB(6E,84,B4), RGB(16,20,32), RGB(96,A0,B4),
    RGB(31,68,D5), RGB(FF,FF,FF), RGB(C8,D4,E8),

    RGB(EC,EE,F4), RGB(2A,5B,C8), RGB(FF,FF,FF), RGBA(00,00,20,88),

    RGB(FF,FF,FF), RGB(F2,F5,FB), RGB(16,20,32), RGB(5A,66,78),
    RGB(DC,E4,F2), RGB(31,68,D5), RGB(FF,FF,FF),

    RGB(FF,FF,FF), RGB(7A,90,BC), RGB(16,20,32), RGB(BC,D2,F4),
    RGB(16,20,32),

    RGB(10,18,28), RGB(D8,E4,F4), RGB(6E,C8,6E), RGB(FF,FF,FF),

    /* Dark on a light wallpaper. A white label with a dark shadow reads on a
     * night sky and disappears on a daytime one, which is what this skin
     * pairs with. */
    RGB(16,20,32), RGBA(FF,FF,FF,C8), RGBA(31,68,D5,A0),

    /* Green, the way that era's start button was, and a hot orange for
     * warnings so the two are apart in hue and in luminance. */
    RGB(3E,8E,3E), RGB(FF,FF,FF), RGB(C8,4A,10), RGB(1E,4E,A8),
};

static const recon_color THEME_READING[] = {
    RGB(E4,DF,D4), RGB(58,52,46), RGB(4A,5A,6E), RGB(9A,94,88),
    RGB(FB,F7,EE), RGB(E0,DA,CE), RGB(EA,E5,DA), RGB(3A,36,30),

    RGB(E4,DF,D4), RGB(3A,36,30), RGB(6E,68,5E), RGB(EA,E5,DA),
    RGB(C8,C2,B4),
    RGB(3A,36,30),

    RGB(EF,EA,DF), RGB(58,52,46), RGB(3A,36,30), RGB(8A,84,78),
    RGB(4A,5A,6E), RGB(FB,F7,EE), RGB(C0,BA,AE),

    RGB(E4,DF,D4), RGB(4A,5A,6E), RGB(FB,F7,EE), RGBA(2A,26,20,88),

    RGB(FA,F4,E6), RGB(F4,EE,E0), RGB(3A,36,30), RGB(6E,68,5E),
    RGB(EA,E5,DA), RGB(4A,5A,6E), RGB(FB,F7,EE),

    RGB(FA,F4,E6), RGB(4A,5A,6E), RGB(3A,36,30), RGB(CC,D6,E4),
    RGB(3A,36,30),

    RGB(2A,28,24), RGB(E8,E2,D6), RGB(9C,C0,8C), RGB(FB,F7,EE),

    RGB(FB,F7,EE), RGBA(00,00,00,C0), RGBA(4A,5A,6E,A0),

    RGB(8A,5A,2A), RGB(FB,F7,EE), RGB(A8,3A,20), RGB(3A,5A,7A),
};

/*
 * Every skin answers every role.
 *
 * These arrays are sized by what is in them rather than declared as
 * [RECON_THEME_ROLE_COUNT], which is what makes this checkable: a table
 * declared at full length and initialised with one value short does not fail
 * to compile, it silently zero-fills, and a role that came out as transparent
 * black would look like a missing feature rather than a missing colour. Sized
 * by their contents, a short table is a compile error naming the skin.
 */
#define CHECK_SKIN(table) _Static_assert(     sizeof(table) / sizeof((table)[0]) == RECON_THEME_ROLE_COUNT,     #table " does not answer every role")

CHECK_SKIN(THEME_RECON);
CHECK_SKIN(THEME_CLASSIC);
CHECK_SKIN(THEME_AQUA);
CHECK_SKIN(THEME_MIDNIGHT);
CHECK_SKIN(THEME_BEACON);
CHECK_SKIN(THEME_DEUTERAN);
CHECK_SKIN(THEME_PROTAN);
CHECK_SKIN(THEME_TRITAN);
CHECK_SKIN(THEME_CONTRAST);
CHECK_SKIN(THEME_READING);

#undef CHECK_SKIN

/* --- Gradients --- */

/*
 * Beacon is the skin this was built for. Every ramp goes light at the top to
 * deeper at the bottom, which is the whole trick: it reads as a lit surface
 * with the light above it, and that single cue is most of what separates the
 * early 2000s from the flat fills of the decade before.
 *
 * Kept shallow on purpose. A steep ramp on a title bar is a stripe, and text
 * sitting on it then has two very different backgrounds to be legible against.
 */
static const struct gradient_spec GRAD_BEACON[] = {
    { RECON_THEME_TITLE_ACTIVE,   RGB(1B,42,9E) },
    { RECON_THEME_TITLE_INACTIVE, RGB(74,8C,B4) },
    { RECON_THEME_BAR,            RGB(1B,42,9E) },
    { RECON_THEME_DIALOG_TITLE,   RGB(1B,42,9E) },

    { RECON_THEME_BUTTON,         RGB(C6,D2,E8) },
    { RECON_THEME_BUTTON_ACTIVE,  RGB(D6,E2,F6) },

    { RECON_THEME_ACCENT,         RGB(2A,6E,2A) },
    { RECON_THEME_SELECTION,      RGB(24,52,B4) },
    { RECON_THEME_MENU_HILITE,    RGB(24,52,B4) },

    { RECON_THEME_ROLE_COUNT, 0 },
};

/*
 * The native look gets one too, shallower still. Recon is meant to read as
 * its own thing rather than as a period piece, so this is barely a ramp --
 * enough that chrome is not perfectly flat, not enough to be a style.
 */
static const struct gradient_spec GRAD_RECON[] = {
    { RECON_THEME_TITLE_ACTIVE,   RGB(16,1E,34) },
    { RECON_THEME_BAR,            RGB(A8,A8,B0) },
    { RECON_THEME_DIALOG_TITLE,   RGB(16,1E,34) },
    { RECON_THEME_ROLE_COUNT, 0 },
};

/*
 * Aqua's is the one that earns it most after Beacon: a light skin has almost
 * no contrast between its own surfaces, so a shallow ramp is what stops the
 * title bar and the window below it reading as one shape.
 */
static const struct gradient_spec GRAD_AQUA[] = {
    { RECON_THEME_TITLE_ACTIVE,   RGB(96,B4,D8) },
    { RECON_THEME_TITLE_INACTIVE, RGB(CE,D6,DE) },
    { RECON_THEME_BAR,            RGB(D4,DA,E2) },
    { RECON_THEME_DIALOG_TITLE,   RGB(96,B4,D8) },
    { RECON_THEME_ROLE_COUNT, 0 },
};

/*
 * Classic, Midnight, Reading, Contrast and the three colour-vision skins get
 * none, each for its own reason.
 *
 * Classic is the 95 era, and 95 was flat -- a gradient there would be the
 * wrong decade. Contrast cannot have one at all: it exists so that nothing
 * depends on a shade, and a ramp behind text is a range of contrast ratios
 * where the skin promises one. Midnight and Reading are deliberately quiet.
 * And the dichromat skins are tested as pairs of flat colours a measured
 * distance apart; a ramp would put one end of a pair a different distance
 * from its partner than the other end, which is a promise the test could no
 * longer check.
 */

#undef RGB
#undef RGBA

/*
 * Each skin names the wallpaper it goes with. A default rather than a rule:
 * choosing a skin puts its wallpaper on, and a wallpaper chosen afterwards
 * stays until the skin changes again. A skin that owned the background would
 * mean somebody who liked one picture could not keep it.
 */
static const struct {
    const char *name;
    const char *description;
    const recon_color *colors;
    const char *wallpaper;
    /* NULL for a skin that fills flat, which is most of them. */
    const struct gradient_spec *gradients;
} BUILT_IN[] = {
    { "Recon", "The native look: grey chrome, navy titles, oxblood accent",
      THEME_RECON, "Night Sky.png", GRAD_RECON },
    { "Classic", "Squared-off and high contrast, the 95 era", THEME_CLASSIC,
      "Daybreak.png", NULL },
    { "Aqua", "Light and quiet, thin edges, blue selection", THEME_AQUA,
      "Daybreak.png", GRAD_AQUA },
    { "Midnight", "Dark and flat", THEME_MIDNIGHT, "Deep Field.png", NULL },
    { "Beacon", "Bright blue chrome and a green accent, early 2000s",
      THEME_BEACON, "Daybreak.png", GRAD_BEACON },
    { "Deuteran", "Red-green safe: blue and orange carry meaning",
      THEME_DEUTERAN, "Night Sky.png", NULL },
    { "Protan", "Red-green safe, avoiding dark reds that read as black",
      THEME_PROTAN, "Night Sky.png", NULL },
    { "Tritan", "Blue-yellow safe: red, green and magenta carry meaning",
      THEME_TRITAN, "Deep Field.png", NULL },
    { "Contrast", "Black on white throughout; nothing depends on hue",
      THEME_CONTRAST, "Daybreak.png", NULL },
    { "Reading", "Warm off-white and softened contrast, easier to read on",
      THEME_READING, "Ember.png", NULL },
};

#define BUILT_IN_COUNT ((int)(sizeof(BUILT_IN) / sizeof(BUILT_IN[0])))

/* --- Reading and writing --- */

/* RRGGBB or AARRGGBB. Six digits mean fully opaque, which is what a person
 * writing a colour by hand almost always intends. */
static bool parse_color(const char *text, recon_color *out) {
    while (*text == '#' || *text == ' ') {
        text++;
    }

    size_t length = 0;
    while (isxdigit((unsigned char)text[length])) {
        length++;
    }
    if ((length != 6 && length != 8) || text[length] != '\0') {
        return false;
    }

    char *end = NULL;
    unsigned long value = strtoul(text, &end, 16);
    if (end == NULL || *end != '\0') {
        return false;
    }

    *out = (length == 6) ? (recon_color)(0xFF000000u | value)
                         : (recon_color)value;
    return true;
}

static struct theme *find_theme(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    for (int i = 0; i < THEMES_MAX; i++) {
        if (g_themes[i].used && strcasecmp(g_themes[i].info.name, name) == 0) {
            return &g_themes[i];
        }
    }
    return NULL;
}

static struct theme *new_theme(const char *name) {
    if (find_theme(name) != NULL) {
        return NULL; /* First one wins; a file cannot shadow a built-in. */
    }
    for (int i = 0; i < THEMES_MAX; i++) {
        if (!g_themes[i].used) {
            memset(&g_themes[i], 0, sizeof(g_themes[i]));
            g_themes[i].used = true;
            snprintf(g_themes[i].info.name, sizeof(g_themes[i].info.name),
                "%s", name);
            g_count++;
            return &g_themes[i];
        }
    }
    return NULL;
}

/*
 * Read one theme file.
 *
 * A skin starts as a copy of the default rather than as nothing, so a file
 * that only wants to change the accent colour says only that. Anything it does
 * not mention keeps a sensible answer instead of drawing as a hole.
 */
static bool load_theme_file(const char *path, const char *fallback_name) {
    size_t size = 0;
    char *text = recon_fs_read("/", path, &size);
    if (text == NULL) {
        set_error("cannot read '%s'", path);
        return false;
    }

    char name[48];
    snprintf(name, sizeof(name), "%s", fallback_name);
    char description[96] = "";

    recon_color colors[RECON_THEME_ROLE_COUNT];
    memcpy(colors, THEME_RECON, sizeof(colors));

    /*
     * Gradients start empty rather than inherited. The default skin's ramps
     * are chosen for its own colours, and carrying them into a skin that
     * replaced those colours would give it a ramp running to a shade of a
     * palette it does not use.
     */
    recon_color gradient[RECON_THEME_ROLE_COUNT] = {0};
    bool has_gradient[RECON_THEME_ROLE_COUNT] = {0};

    char *saveptr = NULL;
    for (char *line = strtok_r(text, "\n", &saveptr);
            line != NULL;
            line = strtok_r(NULL, "\n", &saveptr)) {

        while (*line == ' ' || *line == '\t') {
            line++;
        }
        if (*line == '\0' || *line == '#') {
            continue;
        }

        char *equals = strchr(line, '=');
        if (equals == NULL) {
            continue;
        }
        *equals = '\0';

        char *key_end = equals;
        while (key_end > line && (key_end[-1] == ' ' || key_end[-1] == '\t')) {
            *--key_end = '\0';
        }
        char *value = equals + 1;
        while (*value == ' ' || *value == '\t') {
            value++;
        }
        char *value_end = value + strlen(value);
        while (value_end > value &&
                (value_end[-1] == ' ' || value_end[-1] == '\t' ||
                 value_end[-1] == '\r')) {
            *--value_end = '\0';
        }

        if (strcasecmp(line, "name") == 0) {
            snprintf(name, sizeof(name), "%s", value);
            continue;
        }
        if (strcasecmp(line, "description") == 0) {
            snprintf(description, sizeof(description), "%s", value);
            continue;
        }

        /*
         * A key ending in ".to" is the far end of that role's gradient rather
         * than a role of its own. Checked before the role lookup, because
         * "title.active.to" is not a role name and would otherwise be skipped
         * as unknown.
         */
        size_t key_len = strlen(line);
        bool is_gradient = key_len > 3 &&
            strcasecmp(line + key_len - 3, ".to") == 0;
        if (is_gradient) {
            line[key_len - 3] = ' ';
        }

        int role = role_from_name(line);
        if (role < 0) {
            continue; /* An unknown role is skipped, not guessed at. */
        }

        recon_color parsed;
        if (!parse_color(value, &parsed)) {
            continue;
        }
        if (is_gradient) {
            gradient[role] = parsed;
            has_gradient[role] = true;
        } else {
            colors[role] = parsed;
        }
    }

    free(text);

    struct theme *theme = new_theme(name);
    if (theme == NULL) {
        return false;
    }
    snprintf(theme->info.description, sizeof(theme->info.description),
        "%s", description);
    theme->info.built_in = false;
    memcpy(theme->colors, colors, sizeof(colors));
    memcpy(theme->gradient, gradient, sizeof(gradient));
    memcpy(theme->has_gradient, has_gradient, sizeof(has_gradient));
    return true;
}

int recon_theme_write_defaults(void) {
    int written = 0;

    for (int i = 0; i < BUILT_IN_COUNT; i++) {
        char path[RECON_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s%s", RECON_DIR_THEMES,
            BUILT_IN[i].name, RECON_THEME_EXT);

        /* Left alone if it is there. A file the user has edited is theirs. */
        if (recon_fs_exists("/", path)) {
            continue;
        }

        /* Room for every role at full width, plus the header. */
        size_t capacity = RECON_THEME_ROLE_COUNT * 64 + 512;
        char *text = malloc(capacity);
        if (text == NULL) {
            break;
        }

        size_t used = (size_t)snprintf(text, capacity,
            "# ReconOS theme.\n"
            "#\n"
            "# Colours are RRGGBB, or AARRGGBB where transparency matters.\n"
            "# Anything left out keeps the default, so a theme that only\n"
            "# changes the accent colour need only say that.\n"
            "\n"
            "name = %s\n"
            "description = %s\n"
            "\n",
            BUILT_IN[i].name, BUILT_IN[i].description);

        for (int role = 0; role < RECON_THEME_ROLE_COUNT && used < capacity;
                role++) {
            recon_color c = BUILT_IN[i].colors[role];
            int n;

            /* Six digits when it is opaque, which is most of them, and eight
             * only where the alpha is doing something. */
            if ((c >> 24) == 0xFF) {
                n = snprintf(text + used, capacity - used, "%-22s = %06X\n",
                    ROLE_NAMES[role], c & 0xFFFFFFu);
            } else {
                n = snprintf(text + used, capacity - used, "%-22s = %08X\n",
                    ROLE_NAMES[role], c);
            }
            if (n < 0) {
                break;
            }
            used += (size_t)n;
        }

        if (recon_fs_write("/", path, text, used)) {
            written++;
        }
        free(text);
    }

    return written;
}

/* --- Starting up --- */

void recon_theme_init(void) {
    memset(g_themes, 0, sizeof(g_themes));
    g_count = 0;
    g_current = -1;

    /* The compiled-in ones first, so a broken or missing /System/Themes never
     * leaves the system with no colours at all. */
    for (int i = 0; i < BUILT_IN_COUNT; i++) {
        struct theme *theme = new_theme(BUILT_IN[i].name);
        if (theme == NULL) {
            continue;
        }
        snprintf(theme->info.description, sizeof(theme->info.description),
            "%s", BUILT_IN[i].description);
        snprintf(theme->info.wallpaper, sizeof(theme->info.wallpaper),
            "%s", BUILT_IN[i].wallpaper);
        theme->info.built_in = true;
        memcpy(theme->colors, BUILT_IN[i].colors, sizeof(theme->colors));

        for (const struct gradient_spec *g = BUILT_IN[i].gradients;
                g != NULL && g->role != RECON_THEME_ROLE_COUNT; g++) {
            if (g->role < 0 || g->role >= RECON_THEME_ROLE_COUNT) {
                continue;
            }
            theme->gradient[g->role] = g->to;
            theme->has_gradient[g->role] = true;
        }
    }

    /* Then anything on disk, which may add to them but cannot replace one. */
    struct recon_dirent entries[32];
    int count = recon_fs_list("/", RECON_DIR_THEMES, entries, 32);
    for (int i = 0; i < count && i < 32; i++) {
        size_t length = strlen(entries[i].name);
        size_t suffix = strlen(RECON_THEME_EXT);
        if (entries[i].kind == RECON_FILE_DIRECTORY || length <= suffix ||
                strcasecmp(entries[i].name + length - suffix,
                    RECON_THEME_EXT) != 0) {
            continue;
        }

        char path[RECON_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", RECON_DIR_THEMES, entries[i].name);

        char fallback[48];
        snprintf(fallback, sizeof(fallback), "%.*s",
            (int)(length - suffix), entries[i].name);

        load_theme_file(path, fallback);
    }

    /*
     * The user's choice, then the machine's, then the default. A skin named in
     * the registry that no longer exists falls back rather than leaving the
     * system with none.
     */
    const char *wanted = recon_registry_get(RECON_REG_USER, RECON_THEME_KEY,
        recon_registry_get(RECON_REG_SYSTEM, RECON_THEME_KEY,
            RECON_THEME_DEFAULT));

    struct theme *chosen = find_theme(wanted);
    if (chosen == NULL) {
        chosen = find_theme(RECON_THEME_DEFAULT);
    }

    for (int i = 0; i < THEMES_MAX; i++) {
        if (&g_themes[i] == chosen) {
            g_current = i;
            break;
        }
    }
    g_generation++;
}

void recon_theme_finish(void) {
    memset(g_themes, 0, sizeof(g_themes));
    g_count = 0;
    g_current = -1;
}

/* --- Asking --- */

recon_color recon_theme_color(enum recon_theme_role role) {
    if (role < 0 || role >= RECON_THEME_ROLE_COUNT) {
        /* Magenta, on purpose. A colour nobody chose should be obvious on
         * screen rather than blending in as a shadow. */
        return RECON_RGB(0xFF, 0x00, 0xFF);
    }
    if (g_current < 0 || !g_themes[g_current].used) {
        return THEME_RECON[role];
    }
    return g_themes[g_current].colors[role];
}

bool recon_theme_gradient(enum recon_theme_role role, recon_color *from,
        recon_color *to) {
    if (role < 0 || role >= RECON_THEME_ROLE_COUNT) {
        return false;
    }
    if (g_current < 0 || !g_themes[g_current].used) {
        return false;    /* The fallback skin is the flat one. */
    }
    if (!g_themes[g_current].has_gradient[role]) {
        return false;
    }
    if (from != NULL) { *from = g_themes[g_current].colors[role]; }
    if (to != NULL) { *to = g_themes[g_current].gradient[role]; }
    return true;
}

const char *recon_theme_wallpaper(void) {
    if (g_current < 0 || !g_themes[g_current].used) {
        return "";
    }
    return g_themes[g_current].info.wallpaper;
}

const char *recon_theme_current(void) {
    if (g_current < 0 || !g_themes[g_current].used) {
        return RECON_THEME_DEFAULT;
    }
    return g_themes[g_current].info.name;
}

unsigned recon_theme_generation(void) {
    return g_generation;
}

int recon_theme_count(void) {
    return g_count;
}

bool recon_theme_at(int index, struct recon_theme_info *out) {
    int seen = 0;
    for (int i = 0; i < THEMES_MAX; i++) {
        if (!g_themes[i].used) {
            continue;
        }
        if (seen == index) {
            if (out != NULL) {
                *out = g_themes[i].info;
            }
            return true;
        }
        seen++;
    }
    return false;
}

recon_color recon_theme_color_of(int index, enum recon_theme_role role) {
    if (role < 0 || role >= RECON_THEME_ROLE_COUNT) {
        return RECON_RGB(0xFF, 0x00, 0xFF);
    }

    int seen = 0;
    for (int i = 0; i < THEMES_MAX; i++) {
        if (!g_themes[i].used) {
            continue;
        }
        if (seen == index) {
            return g_themes[i].colors[role];
        }
        seen++;
    }
    return RECON_RGB(0xFF, 0x00, 0xFF);
}

bool recon_theme_gradient_of(int index, enum recon_theme_role role,
        recon_color *from, recon_color *to) {
    if (role < 0 || role >= RECON_THEME_ROLE_COUNT) {
        return false;
    }

    int seen = 0;
    for (int i = 0; i < THEMES_MAX; i++) {
        if (!g_themes[i].used) {
            continue;
        }
        if (seen == index) {
            if (!g_themes[i].has_gradient[role]) {
                return false;
            }
            if (from != NULL) { *from = g_themes[i].colors[role]; }
            if (to != NULL) { *to = g_themes[i].gradient[role]; }
            return true;
        }
        seen++;
    }
    return false;
}

bool recon_theme_set(const char *name) {
    struct theme *chosen = find_theme(name);
    if (chosen == NULL) {
        set_error("there is no skin called '%s'", name != NULL ? name : "");
        return false;
    }

    for (int i = 0; i < THEMES_MAX; i++) {
        if (&g_themes[i] == chosen) {
            g_current = i;
            break;
        }
    }

    /* Remembered per account: which skin someone likes is theirs, not the
     * machine's. */
    recon_registry_set(RECON_REG_USER, RECON_THEME_KEY, chosen->info.name);
    g_generation++;
    return true;
}
