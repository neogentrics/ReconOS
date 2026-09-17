/*
 * The desktop's first frame, drawn on the host.
 *
 * `userland/desktop/shell_frame.c` makes no system call, which is the whole
 * reason it is a separate file from `main.c`: the picture a machine will show
 * can be produced here in a millisecond instead of by writing a stick and
 * walking to a machine.
 *
 * --- What is actually worth checking about a picture ---
 *
 * Not that it looks right; a suite cannot see. What it can hold are the
 * properties that fail *silently* -- the ones that produce a frame somebody
 * would look at and call correct:
 *
 *   - **the padding at the end of every row is untouched.** On a screen where
 *     bytes per row happen to equal width times four, a program that confuses
 *     the two draws a perfect picture and shears on the first laptop. This is
 *     the same check `test_init_screen.c` makes, for the same reason, and it
 *     caught a real fault there.
 *   - **nothing is drawn outside the panel.** A frame that overruns by a row
 *     is invisible on the screen it was written against.
 *   - **the task bar is where the layout says it is**, because two answers to
 *     "how tall is the bar" is how a window ends up under it.
 *   - **it draws at sizes nobody tested it at.** 800x600 is what a machine
 *     falls back to when it cannot be asked, and it is the size at which a
 *     margin computed as a fraction goes negative.
 *
 * Run with: ./build/recon_desktop_frame_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_theme.h"
#include "recon_ui.h"

#include "shell_frame.h"

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
 * A screen with slack on the end of every row.
 *
 * The padding is filled with a value nothing draws, so anything that reaches
 * it can be seen. 0xAB rather than 0 because zero is what a cleared buffer
 * holds and what a black pixel holds, and a check that cannot tell those apart
 * is not a check.
 */
#define PAD_BYTE 0xAB

struct fake_screen {
    unsigned char *bytes;
    size_t pitch;
    int width;
    int height;
    size_t size;
};

static bool screen_make(struct fake_screen *s, int width, int height,
        int slack_bytes) {
    s->width = width;
    s->height = height;
    s->pitch = (size_t)width * 4u + (size_t)slack_bytes;
    s->size = s->pitch * (size_t)height;
    s->bytes = malloc(s->size);
    if (s->bytes == NULL) {
        return false;
    }
    memset(s->bytes, PAD_BYTE, s->size);
    return true;
}

/* Whether every byte past the last pixel of every row is still untouched. */
static bool padding_intact(const struct fake_screen *s) {
    size_t row_pixels = (size_t)s->width * 4u;

    for (int y = 0; y < s->height; y++) {
        const unsigned char *row = s->bytes + (size_t)y * s->pitch;

        for (size_t i = row_pixels; i < s->pitch; i++) {
            if (row[i] != PAD_BYTE) {
                return false;
            }
        }
    }
    return true;
}

/* How many pixels in the whole screen were written at all. */
static int pixels_drawn(const struct fake_screen *s) {
    int drawn = 0;

    for (int y = 0; y < s->height; y++) {
        const uint32_t *row = (const uint32_t *)(s->bytes +
            (size_t)y * s->pitch);

        for (int x = 0; x < s->width; x++) {
            if (row[x] != 0xABABABABu) {
                drawn++;
            }
        }
    }
    return drawn;
}

static uint32_t pixel_at(const struct fake_screen *s, int x, int y) {
    const uint32_t *row = (const uint32_t *)(s->bytes + (size_t)y * s->pitch);
    return row[x];
}

static const struct recon_shell_facts FACTS = {
    .version = "ReconOS 0.4.54",
    .machine = "x86_64, 8 processors, 15.6 GiB",
    .display = "1920 x 1080, 7680 bytes a row",
    .volume = "System volume mounted, fonts present",
    .note = "The desktop's own drawing layer, on its own kernel.",
};

/* Draw one frame into a fresh screen. The panel covers the whole thing. */
static bool draw_into(struct fake_screen *s, int width, int height,
        int slack, const struct recon_shell_facts *facts) {
    if (!screen_make(s, width, height, slack)) {
        return false;
    }

    struct recon_panel *panel = recon_panel_on_screen(s->bytes, s->pitch,
        s->width, s->height, 0, 0, s->width, s->height);

    if (panel == NULL) {
        free(s->bytes);
        s->bytes = NULL;
        return false;
    }

    recon_shell_first_frame(panel, width, height, facts);
    recon_panel_commit(panel);
    recon_panel_destroy(panel);
    return true;
}

static void test_it_draws_at_all(void) {
    printf("the frame fills the screen it was given\n");

    struct fake_screen s;
    check(draw_into(&s, 1920, 1080, 96, &FACTS), "the frame is drawn");
    if (s.bytes == NULL) {
        return;
    }

    /*
     * Every pixel, not most of them. A wallpaper that leaves a band at one
     * edge is a wallpaper somebody sees the previous contents of the
     * framebuffer through -- which on a machine that has just booted is the
     * boot loader's text, and looks like the desktop failed to start.
     */
    check(pixels_drawn(&s) == 1920 * 1080,
        "every pixel of the screen is written");
    check(padding_intact(&s),
        "and nothing was written into the padding at the end of a row");

    free(s.bytes);
}

static void test_the_padding_is_the_check_that_matters(void) {
    printf("a row's slack is untouched at several pitches\n");

    /*
     * Several, because `pitch == width * 4` is the one case where confusing
     * the two is invisible, and it is the case a desktop is developed
     * against. The others are what real adapters do.
     */
    static const int SLACK[] = { 0, 4, 64, 96, 512 };

    for (size_t i = 0; i < sizeof(SLACK) / sizeof(SLACK[0]); i++) {
        struct fake_screen s;
        char what[96];

        if (!draw_into(&s, 1024, 768, SLACK[i], &FACTS)) {
            check(false, "the frame is drawn");
            continue;
        }
        snprintf(what, sizeof(what),
            "%d bytes of slack a row: the slack is untouched", SLACK[i]);
        check(padding_intact(&s), what);

        snprintf(what, sizeof(what),
            "%d bytes of slack a row: every pixel still drawn", SLACK[i]);
        check(pixels_drawn(&s) == 1024 * 768, what);
        free(s.bytes);
    }
}

/* The lowest row holding any pixel of `want`, or -1. */
static int lowest_row_of(const struct fake_screen *s, recon_color want) {
    for (int y = s->height - 1; y >= 0; y--) {
        for (int x = 0; x < s->width; x++) {
            if (pixel_at(s, x, y) == want) {
                return y;
            }
        }
    }
    return -1;
}

/* The highest row holding any pixel of `want`, or -1. */
static int highest_row_of(const struct fake_screen *s, recon_color want) {
    for (int y = 0; y < s->height; y++) {
        for (int x = 0; x < s->width; x++) {
            if (pixel_at(s, x, y) == want) {
                return y;
            }
        }
    }
    return -1;
}

static void test_the_window_does_not_go_under_the_bar(void) {
    printf("the window stops above the task bar\n");

    /*
     * --- Why this is not measured against the bar's own pixels ---
     *
     * The first version of this looked for RECON_THEME_BAR and compared it
     * with the row above. In the default theme **BAR and WINDOW_FRAME are the
     * same colour** -- ffc2bfc8 both -- so the wallpaper and the bar are
     * indistinguishable and the check could not fail. Two of its assertions
     * passed whatever the code did.
     *
     * So it measures the thing the header says the function exists for
     * instead: *"two answers to how tall the bar is, is how a window ends up
     * with its bottom edge under it"*. The window's SURFACE is white and the
     * Start button's ACCENT is neither, so both are visible against the
     * background whatever a skin does to the bar.
     */
    struct fake_screen s;
    if (!draw_into(&s, 1920, 1080, 32, &FACTS)) {
        check(false, "the frame is drawn");
        return;
    }

    int bar = recon_shell_taskbar_height(1080);
    int window_bottom = lowest_row_of(&s, recon_theme_color(RECON_THEME_SURFACE));
    int start_top = highest_row_of(&s, recon_theme_color(RECON_THEME_ACCENT));

    check(window_bottom >= 0, "the window body is on the screen");
    check(window_bottom < 1080 - bar,
        "and its lowest row is above where the bar begins");

    check(start_top >= 0, "the Start button is drawn");
    check(start_top >= 1080 - bar,
        "and it is inside the bar rather than above it");
    check(start_top < 1080, "and on the screen");

    /*
     * And the two do not touch. A window whose bottom edge is the bar's top
     * edge looks correct in a screenshot and means the layout has no margin
     * left to give -- the next thing added to either overlaps.
     */
    check(start_top - window_bottom > 1,
        "there is space between the window and the bar");

    free(s.bytes);
}

/* Whether every pixel in rows [top, bottom) is exactly `want`. */
static bool rows_are_only(const struct fake_screen *s, recon_color want,
        int top, int bottom) {
    if (top < 0) {
        top = 0;
    }
    if (bottom > s->height) {
        bottom = s->height;
    }
    for (int y = top; y < bottom; y++) {
        for (int x = 0; x < s->width; x++) {
            if (pixel_at(s, x, y) != want) {
                return false;
            }
        }
    }
    return true;
}

/* Whether rows [top, bottom) hold anything that is not `background`. */
static bool rows_hold_something(const struct fake_screen *s,
        recon_color background, int top, int bottom) {
    return !rows_are_only(s, background, top, bottom);
}

static void test_text_is_inside_what_it_labels(void) {
    printf("a label is inside the bar it belongs to\n");

    /*
     * --- The check a photograph gave this suite ---
     *
     * `recon_draw_text` takes a **baseline**, and this frame was handing it a
     * top edge. Every line came out one ascent too high: the window's title
     * floated in the wallpaper above its own title bar, and "Start" sat above
     * the task bar. Forty checks passed, because not one of them asked where a
     * glyph was -- and the first rendering showed it in a second.
     *
     * --- And why it does not count ink ---
     *
     * The first version of this looked for pixels exactly equal to
     * TITLE_TEXT and ACCENT_TEXT outside their bars, and found them in the
     * body text. Not a bug: glyphs are anti-aliased, so dark text on white
     * produces every grey between the two, and those two inks are pale greys.
     * The colours are distinct; the **blends** are not, and a check written
     * against exact ink is really a check against a rasteriser's arithmetic.
     *
     * So it asks the opposite question, which no blend can confuse: the strip
     * above the window is **nothing but wallpaper**. Anything there escaped,
     * whatever colour it ended up.
     */
    struct fake_screen s;
    if (!draw_into(&s, 1600, 900, 64, &FACTS)) {
        check(false, "the frame is drawn");
        return;
    }

    recon_color paper = recon_theme_color(RECON_THEME_WINDOW_FRAME);
    recon_color title_bg = recon_theme_color(RECON_THEME_TITLE_ACTIVE);
    recon_color accent = recon_theme_color(RECON_THEME_ACCENT);

    /*
     * The window's top edge, not its title bar.
     *
     * Asking for the title-bar colour answers one row too low: the frame
     * draws a one-pixel edge over the title bar's first row, so the topmost
     * TITLE_ACTIVE pixel is the second row of the window. Measuring from the
     * edge is measuring the thing itself.
     */
    int window_top = highest_row_of(&s, recon_theme_color(RECON_THEME_WINDOW_EDGE));
    int bar = recon_shell_taskbar_height(900);
    int button_top = highest_row_of(&s, accent);

    check(window_top > 0, "the window's title bar is on the screen");
    check(rows_are_only(&s, paper, 0, window_top),
        "everything above the window is wallpaper and nothing else");
    check(rows_hold_something(&s, title_bg, window_top,
            window_top + bar),
        "and the title bar has something drawn in it");

    check(button_top >= 900 - bar,
        "the Start button is inside the task bar");

    /*
     * The strip between the window's bottom edge and the bar. The old fault
     * put "Start" here, and nothing else is ever drawn here, so any pixel
     * that is not wallpaper is something that got out.
     */
    int window_bottom = lowest_row_of(&s,
        recon_theme_color(RECON_THEME_SURFACE));
    check(window_bottom > 0 && window_bottom < 900 - bar,
        "the window ends above the bar");
    check(rows_are_only(&s, paper, window_bottom + 2, 900 - bar),
        "and the gap under it is wallpaper and nothing else");

    free(s.bytes);
}

static void test_small_screens(void) {
    printf("it draws at sizes nobody developed it at\n");

    /*
     * 800x600 is what a machine falls back to when it cannot be asked, and it
     * is where a margin computed as a fraction of the width gets close to
     * eating the window. 640x480 is below anything this expects and must
     * still produce a frame rather than nothing.
     */
    static const struct { int w, h; } SIZES[] = {
        { 640, 480 }, { 800, 600 }, { 1280, 800 }, { 2560, 1440 },
        { 3840, 2160 },
    };

    for (size_t i = 0; i < sizeof(SIZES) / sizeof(SIZES[0]); i++) {
        struct fake_screen s;
        char what[96];

        if (!draw_into(&s, SIZES[i].w, SIZES[i].h, 16, &FACTS)) {
            check(false, "the frame is drawn");
            continue;
        }

        snprintf(what, sizeof(what), "%dx%d: every pixel drawn",
            SIZES[i].w, SIZES[i].h);
        check(pixels_drawn(&s) == SIZES[i].w * SIZES[i].h, what);

        snprintf(what, sizeof(what), "%dx%d: the slack is untouched",
            SIZES[i].w, SIZES[i].h);
        check(padding_intact(&s), what);

        snprintf(what, sizeof(what),
            "%dx%d: the bar fits on the screen", SIZES[i].w, SIZES[i].h);
        check(recon_shell_taskbar_height(SIZES[i].h) < SIZES[i].h / 2, what);

        free(s.bytes);
    }
}

static void test_missing_facts_still_draw(void) {
    printf("a machine that answered nothing still gets a desktop\n");

    /*
     * The outcome this rules out is a black screen. A desktop that declines to
     * appear because it could not count the processors is the one failure
     * nobody standing in front of the machine can diagnose -- it looks exactly
     * like a kernel that did not start anything.
     */
    struct fake_screen s;
    if (!draw_into(&s, 1280, 800, 48, NULL)) {
        check(false, "a frame is drawn with no facts at all");
        return;
    }
    check(pixels_drawn(&s) == 1280 * 800,
        "every pixel is still written with no facts to show");
    check(padding_intact(&s), "and the slack is still untouched");
    free(s.bytes);

    /* And with some of them missing, which is the likelier case. */
    static const struct recon_shell_facts PARTIAL = {
        .version = "ReconOS 0.4.54",
        .machine = NULL,
        .display = "800 x 600, 3200 bytes a row",
        .volume = NULL,
        .note = NULL,
    };
    if (!draw_into(&s, 800, 600, 0, &PARTIAL)) {
        check(false, "a frame is drawn with some facts missing");
        return;
    }
    check(pixels_drawn(&s) == 800 * 600, "a partial answer still draws");
    free(s.bytes);
}

static void test_a_refused_screen_is_refused(void) {
    printf("a pitch that cannot be right is refused, not drawn\n");

    /*
     * A pitch smaller than a row would make every row overwrite part of the
     * previous one -- a sheared picture, which reads as a drawing fault rather
     * than as a caller that passed width where it meant bytes.
     *
     * The refusal is `recon_panel_on_screen`'s and is tested there too. It is
     * here because *this* is the program that passes a pitch it was handed by
     * a kernel, and `main.c` checks the same thing again before it ever gets
     * this far.
     */
    unsigned char buffer[1024 * 16];
    memset(buffer, PAD_BYTE, sizeof(buffer));

    check(recon_panel_on_screen(buffer, 100, 64, 32, 0, 0, 64, 32) == NULL,
        "a pitch under width * 4 gets no panel");
    check(recon_panel_on_screen(NULL, 4096, 64, 32, 0, 0, 64, 32) == NULL,
        "and neither does no screen at all");

    struct recon_panel *ok = recon_panel_on_screen(buffer, 64 * 4, 64, 32,
        0, 0, 64, 32);
    check(ok != NULL, "an honest pitch does");
    recon_panel_destroy(ok);
}

static void test_which_panel_is_on_top(void) {
    printf("the topmost panel at a point, when nothing knows better\n");

    /*
     * --- What this is, and why the default is not an approximation ---
     *
     * `recon_panel_topmost_of` asks which panel is genuinely drawn on top at a
     * point. Under a compositor a hook answers, because the scene graph knows
     * about windows the caller never passed in -- a client's.
     *
     * With no hook, it walks the caller's own list front to back. On a
     * framebuffer that is not a worse answer, it is **the** answer:
     * `src/recon_ui_fb.c` says what is in front of what is decided by the
     * order things are committed, by whoever is doing the committing. This
     * reads that order back.
     *
     * What it must not do is what a plain containment test would: a maximized
     * window contains every point on the screen and would claim clicks meant
     * for windows stacked above it.
     */
    unsigned char bytes[256 * 4 * 64];
    memset(bytes, 0, sizeof(bytes));

    /* A big one behind, a small one in front of part of it. */
    struct recon_panel *behind = recon_panel_on_screen(bytes, 256 * 4,
        256, 64, 0, 0, 200, 60);
    struct recon_panel *front = recon_panel_on_screen(bytes, 256 * 4,
        256, 64, 0, 0, 40, 20);

    check(behind != NULL && front != NULL, "two panels");
    recon_panel_set_position(behind, 0, 0);
    recon_panel_set_position(front, 10, 10);

    struct recon_panel *order[2] = { front, behind };

    check(recon_panel_topmost_of(order, 2, 20, 15) == 0,
        "where they overlap, the one in front wins");
    check(recon_panel_topmost_of(order, 2, 150, 50) == 1,
        "outside it, the one behind answers");
    check(recon_panel_topmost_of(order, 2, 400, 400) == -1,
        "and a point on neither is nobody's");

    /*
     * The order is the caller's, so reversing it reverses the answer. That is
     * the property the shell relies on: it keeps `app_order` front-most first
     * and hands that straight in.
     */
    struct recon_panel *reversed[2] = { behind, front };

    check(recon_panel_topmost_of(reversed, 2, 20, 15) == 0,
        "reversing the list puts the other one on top");

    /*
     * A hidden panel does not answer. A window that is not on the screen
     * cannot be clicked, and having it answer and making every caller check
     * afterwards is the same check written at every call site instead of once.
     */
    recon_panel_set_enabled(front, false);
    check(!recon_panel_is_enabled(front), "the front one is hidden");
    check(recon_panel_topmost_of(order, 2, 20, 15) == 1,
        "so the one behind it answers instead");

    recon_panel_set_enabled(front, true);
    check(recon_panel_is_enabled(front), "shown again");
    check(recon_panel_topmost_of(order, 2, 20, 15) == 0, "and it answers again");

    check(recon_panel_topmost_of(NULL, 2, 1, 1) == -1, "no list, nobody");
    check(recon_panel_topmost_of(order, 0, 1, 1) == -1, "no panels, nobody");

    recon_panel_destroy(front);
    recon_panel_destroy(behind);
}

int main(void) {
    printf("ReconOS desktop frame tests\n\n");

    recon_theme_init();

    test_it_draws_at_all();
    test_the_padding_is_the_check_that_matters();
    test_the_window_does_not_go_under_the_bar();
    test_text_is_inside_what_it_labels();
    test_small_screens();
    test_missing_facts_still_draw();
    test_a_refused_screen_is_refused();
    test_which_panel_is_on_top();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
