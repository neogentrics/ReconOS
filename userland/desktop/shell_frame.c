/*
 * The first frame of the ReconOS desktop. See shell_frame.h.
 *
 * Nothing here makes a system call. It takes a panel and draws into it with
 * the same calls `src/recon_web.c` and the Control Panel draw with, which is
 * the whole argument for this file existing: when this frame appears on a
 * machine running the ReconOS kernel, what appeared is the desktop's own
 * drawing layer rather than a picture that resembles it.
 */

#include <stddef.h>

#include "recon_theme.h"
#include "recon_ui.h"

#include "shell_frame.h"

/*
 * The task bar's height, as a fraction rather than a number of pixels.
 *
 * A fixed 32 is right on the 1920x1080 panel this was written against and
 * wrong in both directions elsewhere: invisible on a 4K display, and half the
 * screen on the 800x600 a machine falls back to when it cannot be asked. The
 * bounds are what keeps it usable at both ends rather than merely proportional.
 */
int recon_shell_taskbar_height(int screen_height) {
    int height = screen_height / 24;

    if (height < 24) {
        height = 24;
    }
    if (height > 64) {
        height = 64;
    }
    return height;
}

/* A text size that suits the screen, by the same argument as the bar. */
static int body_size_for(int screen_height) {
    int size = screen_height / 62;

    if (size < 11) {
        size = 11;
    }
    if (size > 22) {
        size = 22;
    }
    return size;
}

/*
 * One line of the readout, or nothing at all.
 *
 * `*y` is a **baseline**, not a top edge -- which is what `recon_draw_text`
 * takes, and what every position in this file had wrong until v0.4.54: each
 * line came out one ascent too high, so the title floated above its own title
 * bar. Forty checks did not notice, because none of them asks where a glyph
 * is. One photograph did, immediately.
 *
 * A NULL line draws nothing and **does not advance `*y`**, so a machine that
 * could not answer one question shows the others closed up rather than with a
 * gap where the answer would have been. A gap reads as a drawing fault; a
 * shorter list reads as a shorter list.
 */
static void line(struct recon_panel *panel, struct recon_font *font,
        int x, int *y, int max_width, const char *text, recon_color ink) {
    if (text == NULL || text[0] == '\0') {
        return;
    }
    recon_draw_text(panel, font, x, *y, max_width, text, ink);
    *y += recon_font_line_height(font);
}

void recon_shell_first_frame(struct recon_panel *panel, int width, int height,
        const struct recon_shell_facts *facts) {
    static const struct recon_shell_facts NOTHING_KNOWN = { 0 };

    if (panel == NULL || width <= 0 || height <= 0) {
        return;
    }
    if (facts == NULL) {
        /*
         * Drawn anyway, with no facts on it. A desktop that refuses to appear
         * because it could not describe the machine is a black screen, and a
         * black screen is the one outcome nobody can diagnose.
         */
        facts = &NOTHING_KNOWN;
    }

    int body = body_size_for(height);
    struct recon_font *font = recon_font_system(body);
    struct recon_font *bold = recon_font_bold(body * 3 / 2);
    int bar = recon_shell_taskbar_height(height);

    /* --- The wallpaper --- */

    /*
     * A flat fill, and it is deliberate rather than pending.
     *
     * `recon_wallpaper.h` can produce the drawn ones, and reaching for it
     * here would make the first frame a machine ever draws depend on a file
     * being on the volume and being readable. This depends on the theme
     * alone, which is compiled in with defaults -- so the frame appears on a
     * machine whose volume is empty, and that is the machine somebody most
     * needs to see a frame on.
     */
    recon_fill_rect(panel, 0, 0, width, height, THEME(WINDOW_FRAME));

    /* --- The window --- */

    int margin = width / 12;
    int win_w = width - margin * 2;
    int win_h = height - bar - margin * 2;

    if (win_w < 160) {
        win_w = width > 160 ? 160 : width;
    }
    if (win_h < 120) {
        win_h = 120;
    }

    int win_x = (width - win_w) / 2;
    int win_y = (height - bar - win_h) / 2;
    int title_h = bar;

    recon_fill_rect(panel, win_x, win_y, win_w, title_h, THEME(TITLE_ACTIVE));
    recon_fill_rect(panel, win_x, win_y + title_h, win_w, win_h - title_h,
        THEME(SURFACE));

    /*
     * One pixel of edge all the way round.
     *
     * Four fills rather than a rounded rectangle, because a rounded corner on
     * a framebuffer needs something to blend against and there is nothing
     * under this -- `src/recon_ui_fb.c` says the same thing at more length.
     * A square window that is drawn is worth more than a round one that is
     * not.
     */
    recon_color edge = THEME(WINDOW_EDGE);
    recon_fill_rect(panel, win_x, win_y, win_w, 1, edge);
    recon_fill_rect(panel, win_x, win_y + win_h - 1, win_w, 1, edge);
    recon_fill_rect(panel, win_x, win_y, 1, win_h, edge);
    recon_fill_rect(panel, win_x + win_w - 1, win_y, 1, win_h, edge);

    /* --- The title --- */

    int pad = body;

    /*
     * The baseline, not the top. Centring the *box* and then handing that
     * number to a call that wants a baseline is the fault this comment
     * exists to stop coming back: it puts every glyph one ascent too high,
     * which looks like a layout that is merely a bit off until you notice the
     * text is outside the bar it belongs to.
     */
    int title_baseline = win_y + (title_h - recon_font_line_height(bold)) / 2
        + recon_font_ascent(bold);

    recon_draw_text(panel, bold, win_x + pad, title_baseline, win_w - pad * 2,
        facts->version != NULL ? facts->version : "ReconOS",
        THEME(TITLE_TEXT));

    /* --- What the machine is --- */

    int y = win_y + title_h + pad + recon_font_ascent(font);
    int text_width = win_w - pad * 2;

    line(panel, font, win_x + pad, &y, text_width, facts->machine,
        THEME(SURFACE_TEXT));
    line(panel, font, win_x + pad, &y, text_width, facts->display,
        THEME(SURFACE_TEXT));
    line(panel, font, win_x + pad, &y, text_width, facts->volume,
        THEME(SURFACE_TEXT));

    if (facts->note != NULL && facts->note[0] != '\0') {
        y += recon_font_line_height(font) / 2;
        line(panel, font, win_x + pad, &y, text_width, facts->note,
            THEME(SURFACE_TEXT_DIM));
    }

    /*
     * What somebody has typed, in a field of its own.
     *
     * A field rather than another line, because it is the one thing here that
     * is not a fact printed at startup: it changes while somebody watches, and
     * a changing thing that looks like the fixed things around it reads as a
     * fact that keeps being wrong.
     *
     * Drawn even when empty, so a machine where nothing arrives shows an empty
     * field rather than nothing at all -- which is the difference between "no
     * keyboard" and "this frame never got that far".
     */
    if (facts->typed != NULL) {
        int field_h = recon_font_line_height(font) * 2;

        y += recon_font_line_height(font);
        recon_fill_rect(panel, win_x + pad, y, text_width, field_h,
            THEME(FIELD));
        recon_fill_rect(panel, win_x + pad, y, text_width, 1,
            THEME(FIELD_BORDER));
        recon_fill_rect(panel, win_x + pad, y + field_h - 1, text_width, 1,
            THEME(FIELD_BORDER));

        recon_draw_text(panel, font, win_x + pad * 2,
            y + (field_h - recon_font_line_height(font)) / 2
                + recon_font_ascent(font),
            text_width - pad * 2, facts->typed, THEME(FIELD_TEXT));
    }

    /* --- The task bar --- */

    recon_fill_rect(panel, 0, height - bar, width, bar, THEME(BAR));

    /*
     * A Start button, drawn rather than registered.
     *
     * Nothing can be clicked on this frame: there is no pointer on a ReconOS
     * machine yet and no loop to deliver one to. It is here because a task bar
     * with nothing on it looks like a task bar that failed to draw, and the
     * difference between "not implemented" and "broken" is the one thing a
     * first frame has to get across.
     */
    int start_w = bar * 3;
    int inset = bar / 6;
    int button_y = height - bar + inset;
    int button_h = bar - inset * 2;

    recon_fill_rect(panel, inset, button_y, start_w, button_h, THEME(ACCENT));

    /*
     * Centred on the *button*, not on the bar behind it.
     *
     * The button is inset, so centring on the bar puts the label in the
     * button's top half -- close enough to look deliberate and wrong enough
     * to look unfinished. Found by looking at a rendering, which is the
     * second thing in this frame that forty checks passed over.
     */
    recon_draw_text(panel, font, inset + inset,
        button_y + (button_h - recon_font_line_height(font)) / 2
            + recon_font_ascent(font),
        start_w - inset * 2, "Start", THEME(ACCENT_TEXT));
}
