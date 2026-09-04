/*
 * Setup and signing in. See include/recon_session.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */

#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "ReconOS.h"
#include "recon_access.h"
#include "recon_appwin.h"
#include "recon_avatar.h"
#include "recon_icon_gen.h"
#include "recon_fs.h"
#include "recon_icons.h"
#include "recon_modules.h"
#include "recon_error.h"
#include "recon_registry.h"
#include "recon_shell.h"
#include "recon_server.h"
#include "recon_theme.h"
#include "recon_ui.h"
#include "recon_users.h"
#include "recon_wallpaper.h"

/* --- Layout --- */

#define CARD_WIDTH 640
#define CARD_PADDING 28
#define TITLE_SIZE 30
#define ROW_HEIGHT 34
#define FIELD_HEIGHT 30
#define BUTTON_HEIGHT 32
#define BUTTON_WIDTH 130
#define GAP 14

/*
 * A band across the top of the card carrying the mark and the system's name.
 *
 * The card used to open straight onto body text on grey, which said nothing
 * about what the machine was. The first thing a person sees when they turn on
 * a computer should tell them whose computer it is.
 */
#define BANNER_HEIGHT 76
#define BANNER_LOGO 48

/* A row in the skin list carries a picture of the skin, not only its name. */
#define PREVIEW_WIDTH 84
#define PREVIEW_HEIGHT 26

/*
 * The login screen is about a person, so it is built around their picture:
 * one large one for whoever is being signed in, and small ones for the other
 * accounts to switch between. The password box is narrower than the card --
 * a field the full width of the window looks like somewhere to write an essay.
 */
#define AVATAR_SIZE 88
#define AVATAR_SMALL 40
#define LOGIN_FIELD_WIDTH 280

/* A tile on the account-picking screen: a face with a name and role under it. */
#define PICK_TILE 130
#define PICK_FACE 72

#define ACCOUNTS_VISIBLE 6
/*
 * An upper bound, not the number shown. Sizing the list to this rather than
 * to how many there are cut the last skin off the setup screen, so it could
 * not be chosen at all -- and the one that went missing was Reading, which is
 * the one somebody who needs it is looking for.
 */
#define OPTIONS_MAX 12

/* --- Hit ids --- */

#define HIT_PRIMARY 1
#define HIT_SECONDARY 2
#define HIT_BACK 3
#define HIT_NAME_FIELD 4
#define HIT_PASSWORD_FIELD 5
#define HIT_CONFIRM_FIELD 6
#define HIT_SHUTDOWN 7
#define HIT_RESTART 8
#define HIT_PICK_AGAIN 9
#define HIT_ACCOUNT_BASE 100
#define HIT_OPTION_BASE 200

/*
 * Where in the flow we are.
 *
 * The order matters and changed once. Reading and colour used to come after
 * the skin, and before that it was shown to everybody -- so a person with no
 * difficulty reading was asked to rule out six conditions before they were
 * allowed to use their computer. That is the wrong shape for a question that
 * most people answer "none of these" to.
 *
 * It is one question now: does anything need adjusting? Whoever says no never
 * sees the list. Whoever says yes is asked before the skin is chosen, because
 * the answer picks a skin, and being offered the choice afterwards would
 * quietly undo it.
 */
enum stage {
    /* The splash, on every start. */
    STAGE_BOOTING,
    /* Then, on the first run of a new version. */
    STAGE_UPDATING,
    /* Setup, in order. */
    STAGE_WELCOME,
    STAGE_ACCOUNT,
    STAGE_MACHINE,
    STAGE_HELP,
    STAGE_READING,
    STAGE_LOOK,
    STAGE_FINISHED,
    /*
     * The system has stopped. Full screen, like the splash, because nothing
     * behind it is usable any more and a card would suggest otherwise.
     */
    STAGE_STOPPED,
    /*
     * And the gentler version: the *last* run stopped, this one is fine, and
     * somebody should be told once before they carry on. A card rather than a
     * full screen -- the machine in front of them is working, and saying
     * otherwise would be a lie told in a large font.
     */
    STAGE_LAST_STOP,
    /* Not part of setup. */
    STAGE_LOGIN,
    /* Nothing showing; the desktop has it. */
    STAGE_DONE,
};

/* The one question that decides whether the reading and colour list is shown
 * at all. */
static const struct {
    const char *label;
    const char *detail;
} HELP_CHOICES[] = {
    { "No, everything is fine",
      "Go straight on. This can be changed later in the Control Panel." },
    { "Yes, show me the options",
      "Spacing, contrast, and colour vision." },
};

#define HELP_COUNT ((int)(sizeof(HELP_CHOICES) / sizeof(HELP_CHOICES[0])))

/* Where the machine's own name is kept, so the login screen and the Control
 * Panel can say whose machine this is. */
#define MACHINE_NAME_KEY "system/machine-name"
#define MACHINE_NAME_DEFAULT "recon-tower"

/*
 * --- Coming up on a new version ---
 *
 * The version the system last ran as. When it does not match the version
 * running now, this is the first start after an update, and the system says
 * so instead of quietly being different.
 *
 * It is shown on the way *up* rather than on the way down, because that is
 * when the work happens. Nothing is installed during a restart -- the new
 * build is already on disk -- and a progress bar before shutting down would
 * be an animation of nothing. What genuinely runs on the first start of a new
 * version is this: filling in icons, themes and modules the new version has
 * and the old one did not. Each step below does that work and reports what it
 * found, so the bar measures something real.
 */
#define INSTALLED_VERSION_KEY "system/installed-version"

#define UPDATE_STEP_MS 420

struct update_step {
    const char *label;
    /* Returns how many things it put in place, or -1 for "nothing to count". */
    int (*run)(void);
};

static int update_icons(void) {
    return recon_icons_write_defaults(false);
}

static int update_themes(void) {
    return recon_theme_write_defaults();
}

static int update_modules(void) {
    return recon_modules_install_shipped();
}

static int update_settings(void) {
    /* Nothing to migrate yet. The step is here because settings are the thing
     * most likely to need it, and a version that needs to move one should
     * have somewhere obvious to do it from. */
    return -1;
}

static const struct update_step UPDATE_STEPS[] = {
    { "Checking what changed", NULL },
    { "Icons", update_icons },
    { "Appearance", update_themes },
    { "Applications", update_modules },
    { "Settings", update_settings },
    { "Finishing", NULL },
};

#define UPDATE_STEP_COUNT \
    ((int)(sizeof(UPDATE_STEPS) / sizeof(UPDATE_STEPS[0])))

/* Defined with the rest of the update flow, further down; the timer that
 * drives it is created before that. */
static int update_tick(void *data);

/*
 * --- The splash ---
 *
 * The system used to appear without announcing itself: one frame of nothing,
 * then a login screen.
 *
 * What it reports is a count of what it brought up -- icons, skins,
 * wallpapers, accounts, modules -- rather than progress through work being
 * done. The startup work is finished before this screen can exist, and a bar
 * pretending to drive it would be a decoration in front of nothing. Counting
 * is honest about which it is: these are real numbers, read at the moment
 * each line appears, and if one of them says zero then something really is
 * missing.
 *
 * It is skippable, because a splash is the one part of a system that a
 * developer sees a hundred times a day and a user sees once.
 */
#define BOOT_TICK_MS 55        /* the animation, which wants to be smooth */
/*
 * Five ticks a line rather than three: about a second and three quarters
 * altogether.
 *
 * At three it went past faster than it could be read, which is the wrong
 * failure for a screen whose whole job is to say the machine is starting. It
 * is still short enough that somebody restarting for the fourth time in a row
 * is not waiting on it.
 */
#define BOOT_TICKS_PER_STEP 5

struct boot_step {
    const char *label;
    /* How many of the thing there are, or -1 when there is nothing to count. */
    int (*count)(void);
};

/* Counted from the folder rather than from the icon cache, which only holds
 * what has been asked for so far -- at this point in a start, almost nothing. */
static int boot_count_icons(void) {
    struct recon_dirent entries[128];
    int found = recon_fs_list("/", RECON_DIR_SYSTEM_ICONS, entries, 128);
    return found < 0 ? -1 : found;
}

static int boot_count_skins(void) {
    return recon_theme_count();
}

static int boot_count_wallpapers(void) {
    return recon_wallpaper_count();
}

static int boot_count_accounts(void) {
    return recon_users_count();
}

static int boot_count_modules(void) {
    return recon_modules_count();
}

static const struct boot_step BOOT_STEPS[] = {
    { "Filesystem", NULL },
    { "Settings", NULL },
    { "Icons", boot_count_icons },
    { "Appearance", boot_count_skins },
    { "Wallpapers", boot_count_wallpapers },
    { "Applications", boot_count_modules },
    { "Accounts", boot_count_accounts },
};

#define BOOT_STEP_COUNT ((int)(sizeof(BOOT_STEPS) / sizeof(BOOT_STEPS[0])))

static int boot_tick(void *data);


/*
 * M_PI is not in the C standard, and this file asks for POSIX rather than GNU
 * so math.h does not offer it. Written out rather than reached for through a
 * feature macro, which would change what else the file sees.
 */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Which field the keyboard is going to. */
enum focus {
    FOCUS_NAME,
    FOCUS_PASSWORD,
    FOCUS_CONFIRM,
};

struct recon_session {
    struct recon_server *server;
    struct recon_font *font;
    /*
     * A second, larger font for headings.
     *
     * The whole system draws in one size, which is fine for a taskbar and
     * wrong for the first screen anybody sees: with no difference between a
     * heading and a sentence, every screen read as a paragraph of grey text
     * and none of them said what they were asking. This is the one place that
     * matters enough to carry a font of its own.
     */
    struct recon_font *heading;
    struct recon_panel *panel;

    int width, height;
    enum stage stage;

    /* What stopped, and what it added. Held rather than pointed at, because
     * the thing that raised it may be gone by the time this is drawn. */
    char stop_code[16];
    char stop_summary[96];
    char stop_detail[512];
    char stop_advice[256];
    bool signed_in_flag;

    /* Setup and login share these; only one is showing at a time. */
    struct recon_edit name;
    struct recon_edit password;
    struct recon_edit confirm;
    struct recon_edit machine;
    enum focus focus;

    /* Whether the reading and colour list was asked for. */
    bool wants_help;

    /*
     * The account this screen is locked to, or empty.
     *
     * Locking and switching user are different acts. Somebody who locks their
     * machine is still using it -- their session is running behind this
     * screen with their windows open -- and the screen exists to keep other
     * people out of it, so it offers their account and no other. Switching
     * user ends the session first, and then there is nothing to protect and
     * every account is fair to offer.
     */
    char locked_to[RECON_USERS_NAME_MAX];

    /*
     * Whether the login screen is asking which account, rather than asking
     * for one account's password.
     *
     * Two screens, not one. Putting the accounts on the same screen as the
     * password box meant a stray click while typing changed who you were
     * signing in as, and it named the way in -- "Password" -- to anybody
     * looking at the machine before they had chosen anything. Choosing first
     * fixes both, and is where a PIN or a fingerprint would be offered
     * instead when there is one.
     *
     * A locked machine never asks: there is exactly one account it will
     * accept, and offering a choice of one is not a choice.
     */
    bool picking_account;

    /*
     * The splash: which line it has reached, what that line found, and how
     * far round the rings have turned.
     */
    int boot_step;
    int boot_ticks;
    unsigned boot_phase;
    char boot_detail[64];
    struct wl_event_source *boot_timer;

    /* The update screen: which step, what it found, and where it came from. */
    int update_step;
    char update_from[32];
    char update_detail[96];
    struct wl_event_source *update_timer;

    /* Which account the login screen has selected. */
    int account;
    int account_scroll;

    /* Which entry a list stage has highlighted. */
    int option;
    int hover;

    char message[192];
    bool message_is_warning;
};

static void set_message(struct recon_session *session, bool warning,
        const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static void set_message(struct recon_session *session, bool warning,
        const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(session->message, sizeof(session->message), fmt, args);
    va_end(args);
    session->message_is_warning = warning;
}

/* --- What each stage offers --- */

/*
 * The reading and vision options, which are the same registry settings the
 * `access` command and the skins use. Offered here rather than buried, because
 * somebody who needs them needs them from the first screen, not once they have
 * found a control panel.
 */
struct accessibility_option {
    const char *label;
    const char *detail;
    /* A skin to put on, or NULL. */
    const char *theme;
    /* Letter and line spacing to set, or -1 to leave alone. */
    int letter_spacing;
    int line_spacing;
};

static const struct accessibility_option ACCESSIBILITY[] = {
    { "None of these", "The usual appearance", NULL, 0, 0 },
    { "Easier to read", "Wider letters and lines, warmer background",
      "Reading", 2, 6 },
    { "High contrast", "Black on white, nothing carried by colour",
      "Contrast", 0, 0 },
    { "Red-green colour vision", "The most common kind",
      "Deuteran", -1, -1 },
    { "Red-green, darker reds", "Where dark reds read as black",
      "Protan", -1, -1 },
    { "Blue-yellow colour vision", "Rarer; blues and yellows are confused",
      "Tritan", -1, -1 },
};

#define ACCESSIBILITY_COUNT \
    ((int)(sizeof(ACCESSIBILITY) / sizeof(ACCESSIBILITY[0])))

/* --- Drawing --- */

static int card_height(struct recon_session *session) {
    int line = recon_font_line_height(session->font);
    if (line <= 0) {
        line = 18;
    }

    /* Every screen carries the band across the top. */
    int base = BANNER_HEIGHT + CARD_PADDING * 2;

    switch (session->stage) {
    case STAGE_BOOTING:
    case STAGE_STOPPED:
        /* No card: these are the whole screen and draw their own layout. */
        return session->height;
    case STAGE_LAST_STOP:
        return base + TITLE_SIZE + line * 7 + BUTTON_HEIGHT + GAP * 3;
    case STAGE_UPDATING:
        return base + TITLE_SIZE + line * 4 + 14 + GAP * 4;
    case STAGE_WELCOME:
        return base + TITLE_SIZE + line * 5 + BUTTON_HEIGHT + GAP * 2;
    case STAGE_FINISHED:
        return base + TITLE_SIZE + line * 3 + BUTTON_HEIGHT + GAP * 2;
    case STAGE_ACCOUNT:
        return base + TITLE_SIZE + line * 2 +
            (FIELD_HEIGHT + line + GAP) * 3 + BUTTON_HEIGHT + GAP * 2;
    case STAGE_MACHINE:
        return base + TITLE_SIZE + line * 2 + FIELD_HEIGHT + line * 3 +
            BUTTON_HEIGHT + GAP * 3;
    case STAGE_HELP:
        return base + TITLE_SIZE + line * 2 + ROW_HEIGHT * HELP_COUNT +
            BUTTON_HEIGHT + GAP * 3;
    case STAGE_LOOK: {
        int count = recon_theme_count();
        if (count > OPTIONS_MAX) {
            count = OPTIONS_MAX;
        }
        return base + TITLE_SIZE + line * 2 +
            ROW_HEIGHT * count + BUTTON_HEIGHT + GAP * 3;
    }
    case STAGE_READING:
        return base + TITLE_SIZE + line * 2 +
            ROW_HEIGHT * ACCESSIBILITY_COUNT + BUTTON_HEIGHT + GAP * 3;
    case STAGE_LOGIN:
    default: {
        if (session->picking_account) {
            int count = recon_users_count();
            if (count > ACCOUNTS_VISIBLE) {
                count = ACCOUNTS_VISIBLE;
            }
            int columns = (CARD_WIDTH - CARD_PADDING * 2) / PICK_TILE;
            if (columns < 1) {
                columns = 1;
            }
            int rows = (count + columns - 1) / columns;
            if (rows < 1) {
                rows = 1;
            }
            return base + TITLE_SIZE + rows * (PICK_TILE + line) +
                BUTTON_HEIGHT + GAP * 3;
        }

        bool locked = session->locked_to[0] != '\0';
        return base + AVATAR_SIZE + line * (locked ? 4 : 3) + FIELD_HEIGHT +
            BUTTON_HEIGHT + GAP * 4;
    }
    }
}

static void card_rect(struct recon_session *session, int *x, int *y,
        int *w, int *h) {
    *w = CARD_WIDTH;
    *h = card_height(session);
    if (*h > session->height - 40) {
        *h = session->height - 40;
    }
    *x = (session->width - *w) / 2;
    *y = (session->height - *h) / 2;
}

/* A button, returning the x to carry on from. */
static void draw_button(struct recon_session *session, struct recon_panel *p,
        int x, int y, int w, const char *label, uint32_t id, bool primary,
        bool enabled) {
    int ascent = recon_font_ascent(session->font);

    recon_color fill = primary ? THEME(ACCENT) : THEME(BUTTON);
    if (!enabled) {
        fill = THEME(BUTTON);
    } else if (id == (uint32_t)session->hover) {
        fill = primary ? THEME(ACCENT) : THEME(BUTTON_ACTIVE);
    }

    recon_fill_rect(p, x, y, w, BUTTON_HEIGHT, fill);
    recon_draw_button_edge(p, x, y, w, BUTTON_HEIGHT, false,
        THEME(DIALOG));

    recon_color ink = !enabled ? THEME(MENU_TEXT_DISABLED)
        : (primary ? THEME(ACCENT_TEXT) : THEME(MENU_TEXT));

    int text_w = recon_text_width(session->font, label);
    recon_draw_text(p, session->font, x + (w - text_w) / 2,
        y + (BUTTON_HEIGHT + ascent) / 2 - 2, w, label, ink);

    if (enabled) {
        recon_hit_add(p, x, y, w, BUTTON_HEIGHT, id);
    }
}

/* A labelled text field. Returns the y to carry on from. */
static int draw_field(struct recon_session *session, struct recon_panel *p,
        int x, int y, int w, const char *label, struct recon_edit *edit,
        uint32_t id, bool focused) {
    int ascent = recon_font_ascent(session->font);
    int line = recon_font_line_height(session->font);
    if (line <= 0) {
        line = 18;
    }

    recon_draw_text(p, session->font, x, y + ascent, w, label,
        THEME(MENU_TEXT_DISABLED));
    y += line + 2;

    recon_edit_draw(p, session->font, x, y, w, FIELD_HEIGHT, edit);

    /* The focused field is outlined, so it is clear where typing goes. */
    if (focused) {
        recon_stroke_rect(p, x - 1, y - 1, w + 2, FIELD_HEIGHT + 2, THEME(ACCENT));
    }

    recon_hit_add(p, x, y, w, FIELD_HEIGHT, id);
    return y + FIELD_HEIGHT + GAP;
}

/* One row of a list. */
/*
 * A row, with the text starting `indent` from the left edge.
 *
 * The indent is for the skin list, where a picture of the skin occupies the
 * left of the row. The highlight still covers the whole row, so the picture
 * is part of what is selected rather than something beside it.
 */
static void draw_row_at(struct recon_session *session, struct recon_panel *p,
        int x, int y, int w, int indent, const char *label, const char *detail,
        uint32_t id, bool selected) {
    int ascent = recon_font_ascent(session->font);
    bool hovered = (id == (uint32_t)session->hover);

    if (selected) {
        recon_fill_rect(p, x, y, w, ROW_HEIGHT, THEME(SELECTION));
    } else if (hovered) {
        recon_fill_rect(p, x, y, w, ROW_HEIGHT, THEME(MENU_HILITE));
    }

    recon_color ink = (selected || hovered)
        ? THEME(SELECTION_TEXT) : THEME(MENU_TEXT);
    recon_color faint = (selected || hovered)
        ? THEME(SELECTION_TEXT) : THEME(MENU_TEXT_DISABLED);

    int label_w = recon_text_width(session->font, label);
    recon_draw_text(p, session->font, x + indent,
        y + (ROW_HEIGHT + ascent) / 2 - 2, w - indent - 12, label, ink);

    if (detail != NULL && *detail != '\0') {
        recon_draw_text(p, session->font, x + indent + 12 + label_w,
            y + (ROW_HEIGHT + ascent) / 2 - 2,
            w - indent - 24 - label_w, detail, faint);
    }

    recon_hit_add(p, x, y, w, ROW_HEIGHT, id);
}

static void draw_row(struct recon_session *session, struct recon_panel *p,
        int x, int y, int w, const char *label, const char *detail,
        uint32_t id, bool selected) {
    draw_row_at(session, p, x, y, w, 12, label, detail, id, selected);
}

static void draw_title(struct recon_session *session, struct recon_panel *p,
        int x, int y, int w, const char *title, const char *subtitle) {
    struct recon_font *big = session->heading != NULL
        ? session->heading : session->font;
    int ascent = recon_font_ascent(session->font);

    recon_draw_text(p, big, x, y + recon_font_ascent(big) + 2, w, title,
        THEME(MENU_TEXT));

    if (subtitle != NULL && *subtitle != '\0') {
        recon_draw_text(p, session->font, x, y + TITLE_SIZE + ascent, w,
            subtitle, THEME(MENU_TEXT_DISABLED));
    }
}

/*
 * --- What to do with the machine, on the login screen ---
 *
 * Two small round buttons in the bottom right, where every system that has
 * them puts them. A full-width "Shut Down" button was the loudest thing on a
 * screen whose job is to let somebody in, and there was no way to restart at
 * all -- so a machine that needed restarting had to be shut down and started
 * again by hand.
 */
#define POWER_BUTTON 30

static void draw_power_glyph(struct recon_panel *p, int cx, int cy,
        bool restart, recon_color ink) {
    const int radius = 8;

    for (int dy = -radius - 1; dy <= radius + 1; dy++) {
        for (int dx = -radius - 1; dx <= radius + 1; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 > radius * radius || d2 < (radius - 2) * (radius - 2)) {
                continue;
            }
            /*
             * Power breaks the ring at the top, where its stem goes through.
             * Restart breaks it at the top right, where its arrowhead goes.
             */
            if (restart) {
                if (dx >= 0 && dy <= 0 && dy > -dx - 2) {
                    continue;
                }
            } else if (dy < 0 && dx >= -2 && dx <= 2) {
                continue;
            }
            recon_fill_rect(p, cx + dx, cy + dy, 1, 1, ink);
        }
    }

    if (restart) {
        for (int i = 0; i < 4; i++) {
            recon_fill_rect(p, cx + 3 + i, cy - radius - 1 + i, 1,
                (4 - i) * 2 - 1, ink);
        }
    } else {
        recon_fill_rect(p, cx - 1, cy - radius - 2, 2, radius, ink);
    }
}

static void draw_power_button(struct recon_session *session,
        struct recon_panel *p, int x, int y, bool restart, uint32_t id) {
    bool hovered = (id == (uint32_t)session->hover);

    if (hovered) {
        recon_fill_rect(p, x, y, POWER_BUTTON, POWER_BUTTON, THEME(BUTTON_ACTIVE));
    }
    recon_draw_button_edge(p, x, y, POWER_BUTTON, POWER_BUTTON, false,
        THEME(DIALOG));

    draw_power_glyph(p, x + POWER_BUTTON / 2, y + POWER_BUTTON / 2, restart,
        THEME(MENU_TEXT));

    recon_hit_add(p, x, y, POWER_BUTTON, POWER_BUTTON, id);
}

/*
 * The band across the top of the card: the mark, the system's name, and
 * where you are in the flow.
 *
 * Drawn in the readout colours rather than the dialog's, so it reads as part
 * of the machine rather than part of the form -- and so the mark, which has
 * its own bright palette, sits on something dark enough to hold it.
 */
static void draw_banner(struct recon_session *session, struct recon_panel *p,
        int x, int y, int w) {
    int ascent = recon_font_ascent(session->font);
    int line = recon_font_line_height(session->font);
    if (line <= 0) {
        line = 18;
    }

    /*
     * The dialog's title colour, not the readout's. The readout is what the
     * screen behind the card is filled with, so a band in it was invisible --
     * the mark appeared to be floating on the backdrop above a card that
     * started below it.
     */
    recon_fill_rect(p, x, y, w, BANNER_HEIGHT, THEME(DIALOG_TITLE));
    /* A rule in the accent along the bottom edge, so the band ends
     * deliberately rather than just stopping. */
    recon_fill_rect(p, x, y + BANNER_HEIGHT - 2, w, 2, THEME(ACCENT));

    int text_x = x + 20;
    int logo = BANNER_LOGO;
    if (recon_icon_draw(p, RECON_ICON_LOGO, x + 18,
            y + (BANNER_HEIGHT - 2 - logo) / 2, logo)) {
        text_x = x + 18 + logo + 18;
    }

    struct recon_font *big = session->heading != NULL
        ? session->heading : session->font;

    int block = recon_font_line_height(big) + line;
    int top = y + (BANNER_HEIGHT - 2 - block) / 2;

    recon_draw_text(p, big, text_x, top + recon_font_ascent(big),
        w - (text_x - x) - 20, RECONOS_FULL_NAME, THEME(DIALOG_TITLE_TEXT));
    top += recon_font_line_height(big) - line;

    /*
     * The step, on the right of the band. Welcome and the finish are not
     * numbered: they are the ends of the flow rather than steps through it.
     */
    static const char *const STEP_OF[] = {
        [STAGE_ACCOUNT] = "Step 1 of 4",
        [STAGE_MACHINE] = "Step 2 of 4",
        [STAGE_HELP]    = "Step 3 of 4",
        [STAGE_READING] = "Step 3 of 4",
        [STAGE_LOOK]    = "Step 4 of 4",
    };

    const char *step = NULL;
    if (session->stage < (int)(sizeof(STEP_OF) / sizeof(STEP_OF[0]))) {
        step = STEP_OF[session->stage];
    }

    recon_draw_text(p, session->font, text_x, top + line + ascent,
        w - (text_x - x) - 20, "Version " RECONOS_VERSION,
        THEME(DIALOG_TITLE_TEXT));

    if (step != NULL) {
        int step_w = recon_text_width(session->font, step);
        recon_draw_text(p, session->font, x + w - 20 - step_w,
            top + line + ascent, step_w + 4, step, THEME(DIALOG_TITLE_TEXT));
    }
}

/*
 * A picture of a skin: a window over a desktop with a taskbar, an inch across.
 *
 * A list of names told you nothing about what choosing one would do, and the
 * obvious answer -- screenshots -- would mean running the system to take them
 * and keeping them current afterwards. This is drawn from the palette itself,
 * so it cannot go out of date and a skin added tomorrow gets one for free.
 */
static void draw_theme_preview(struct recon_panel *p, int index,
        int x, int y) {
    int w = PREVIEW_WIDTH;
    int h = PREVIEW_HEIGHT;

#define SKIN(role) recon_theme_color_of(index, RECON_THEME_##role)

/*
 * Filled the way that skin would fill it, ramp and all.
 *
 * Not recon_fill_role, which asks the skin that is *in use* -- here the whole
 * point is to show a skin that is not. A preview drawn flat would make a skin
 * with gradients look identical to one without, which is most of what
 * separates several of them.
 */
#define SKIN_FILL(px, py, pw, ph, role)                                       \
    do {                                                                      \
        recon_color from, to;                                                 \
        if (recon_theme_gradient_of(index, RECON_THEME_##role, &from, &to)) { \
            recon_fill_gradient(p, (px), (py), (pw), (ph), from, to);         \
        } else {                                                              \
            recon_fill_rect(p, (px), (py), (pw), (ph), SKIN(role));           \
        }                                                                     \
    } while (0)

    /* The desktop behind it. */
    recon_fill_rect(p, x, y, w, h, SKIN(READOUT));

    /* A window: title bar, body, and a line of selected text in it. */
    int win_x = x + 6;
    int win_y = y + 4;
    int win_w = w - 20;
    int win_h = h - 12;

    SKIN_FILL(win_x, win_y, win_w, 6, TITLE_ACTIVE);
    recon_fill_rect(p, win_x, win_y + 6, win_w, win_h - 6, SKIN(SURFACE));
    SKIN_FILL(win_x + 3, win_y + 9, win_w - 6, 3, SELECTION);
    recon_fill_rect(p, win_x + 3, win_y + 14, win_w - 12, 2,
        SKIN(SURFACE_TEXT));

    /* The taskbar along the bottom, with the accent on it. */
    SKIN_FILL(x, y + h - 6, w, 6, BAR);
    recon_fill_rect(p, x + 2, y + h - 5, 10, 4, SKIN(ACCENT));

    recon_stroke_rect(p, x, y, w, h, SKIN(WINDOW_EDGE));

#undef SKIN
#undef SKIN_FILL
}

/*
 * --- The splash ---
 *
 * The mark, two rings turning around it, and a line for each part of the
 * system as it is counted.
 *
 * The rings are drawn as points rather than as an outline, because a circle
 * of single pixels at this radius reads as a thin ring and needs no
 * anti-aliasing to look deliberate. Each has a brighter arc that travels
 * round, the two at different speeds and in opposite directions, which is
 * what makes it shimmer rather than merely spin.
 */
#define SPLASH_LOGO 128
#define SPLASH_RING_GAP 26
#define SPLASH_ARC 70          /* degrees of the brighter part */

static void draw_ring(struct recon_panel *p, int cx, int cy, int radius,
        int thickness, unsigned phase, bool reverse, recon_color dim,
        recon_color bright) {
    if (radius <= 0) {
        return;
    }

    /*
     * Enough samples that neighbouring points touch.
     *
     * One per degree looked right on the inner ring and came out as a dotted
     * line on the outer one: a circle of radius 86 is 540 pixels around, so
     * 360 points leave gaps. Seven per unit of radius is a little over the
     * circumference either way.
     */
    int samples = radius * 7;
    if (samples < 360) {
        samples = 360;
    }

    for (int i = 0; i < samples; i++) {
        int degree = (i * 360) / samples;
        double radians = (i * 2.0 * M_PI) / samples;
        int x = cx + (int)(cos(radians) * radius);
        int y = cy + (int)(sin(radians) * radius);

        /* Where this point sits relative to the travelling arc. */
        int lead = reverse ? (int)(360 - phase % 360) : (int)(phase % 360);
        int offset = (degree - lead + 360) % 360;

        recon_color color = dim;
        if (offset < SPLASH_ARC) {
            /*
             * Faded along the arc rather than switched on, so the bright part
             * has a tail. A hard edge travelling round looked like a fault in
             * the drawing rather than like light.
             */
            int through = (offset * 255) / SPLASH_ARC;
            int weight = 255 - through;
            int r = (int)((dim >> 16) & 0xFF) +
                (((int)((bright >> 16) & 0xFF) - (int)((dim >> 16) & 0xFF)) * weight) / 255;
            int g = (int)((dim >> 8) & 0xFF) +
                (((int)((bright >> 8) & 0xFF) - (int)((dim >> 8) & 0xFF)) * weight) / 255;
            int b = (int)(dim & 0xFF) +
                (((int)(bright & 0xFF) - (int)(dim & 0xFF)) * weight) / 255;
            color = 0xFF000000u | ((unsigned)r << 16) | ((unsigned)g << 8) |
                (unsigned)b;
        }

        recon_fill_rect(p, x - thickness / 2, y - thickness / 2,
            thickness, thickness, color);
    }
}

/*
 * The system has stopped.
 *
 * Not a wall of hexadecimal. Somebody reading this is not going to debug it;
 * they are going to write down the code, restart, and look it up -- so the
 * code is the largest thing on the screen and everything else is there to
 * make it worth writing down.
 *
 * Drawn in fixed colours rather than the skin's. A skin is a file that can be
 * edited, and the one fault this screen must survive is the one where the
 * colours are the problem: a stop screen that renders black on black tells
 * nobody anything.
 */
static void draw_stopped(struct recon_session *session, struct recon_panel *p) {
    const recon_color BACKGROUND = RECON_RGB(0x0E, 0x1B, 0x33);
    const recon_color HEADING    = RECON_RGB(0xFF, 0xFF, 0xFF);
    const recon_color CODE       = RECON_RGB(0xFF, 0xC8, 0x50);
    const recon_color BODY       = RECON_RGB(0xC8, 0xD4, 0xE8);
    const recon_color QUIET      = RECON_RGB(0x84, 0x96, 0xB4);

    int line = recon_font_line_height(session->font);
    if (line <= 0) {
        line = 18;
    }
    int ascent = recon_font_ascent(session->font);

    struct recon_font *big = session->heading != NULL
        ? session->heading : session->font;

    recon_fill(p, BACKGROUND);

    /*
     * Left-aligned in a column, not centred. This is text to be read and
     * copied, and a centred paragraph with a ragged left edge is neither.
     */
    int x = session->width / 2 - 260;
    if (x < 40) {
        x = 40;
    }
    int w = session->width - x * 2;
    if (w < 200) {
        w = session->width - 80;
    }

    int y = session->height / 3 - 40;

    recon_icon_draw(p, RECON_ICON_LOGO, x, y, 48);

    recon_draw_text(p, big, x + 64, y + recon_font_ascent(big) + 6, w - 64,
        "ReconOS has stopped", HEADING);
    y += 64 + GAP;

    /* The code, as large as the heading, because it is the part that has to
     * survive being read out over a telephone. */
    recon_draw_text(p, big, x, y + recon_font_ascent(big), w,
        session->stop_code, CODE);
    y += TITLE_SIZE + 6;

    recon_draw_text(p, session->font, x, y + ascent, w,
        session->stop_summary, HEADING);
    y += line + GAP;

    /* The description, wrapped by hand: this screen cannot lean on anything
     * that might be part of what failed. */
    const char *at = session->stop_detail;
    while (*at != '\0' && y + line < session->height - line * 4) {
        char piece[128];
        size_t used = 0;
        size_t last_space = 0;

        while (at[used] != '\0' && used < sizeof(piece) - 1) {
            piece[used] = at[used];
            if (at[used] == ' ') {
                last_space = used;
            }
            used++;

            piece[used] = '\0';
            if (recon_text_width(session->font, piece) > w &&
                    last_space > 0) {
                used = last_space;
                break;
            }
        }
        piece[used] = '\0';

        recon_draw_text(p, session->font, x, y + ascent, w, piece, BODY);
        y += line;

        at += used;
        while (*at == ' ') {
            at++;
        }
    }

    if (session->stop_advice[0] != '\0') {
        y += GAP;
        recon_draw_text(p, session->font, x, y + ascent, w,
            session->stop_advice, QUIET);
        y += line;
    }

    /*
     * What to do now, at the bottom, where a person looks after they have
     * finished reading rather than before they have started.
     */
    recon_draw_text(p, session->font, x,
        session->height - line * 2 + ascent, w,
        "Press any key to shut down. The code and the time are in "
        "/System/Logs.", QUIET);
}

static void draw_splash(struct recon_session *session, struct recon_panel *p) {
    int line = recon_font_line_height(session->font);
    if (line <= 0) {
        line = 18;
    }
    int ascent = recon_font_ascent(session->font);

    struct recon_font *big = session->heading != NULL
        ? session->heading : session->font;

    int cx = session->width / 2;

    /*
     * The mark sits above the middle rather than in it, so the lines beneath
     * have somewhere to grow into without the whole thing shifting as they
     * arrive.
     */
    int cy = session->height / 2 - 70;

    draw_ring(p, cx, cy, SPLASH_LOGO / 2 + SPLASH_RING_GAP, 2,
        session->boot_phase * 5, false,
        THEME(MENU_TEXT_DISABLED), THEME(ACCENT));
    draw_ring(p, cx, cy, SPLASH_LOGO / 2 + SPLASH_RING_GAP + 12, 1,
        session->boot_phase * 3, true,
        THEME(MENU_TEXT_DISABLED), THEME(DIRECTORY));

    recon_icon_draw(p, RECON_ICON_LOGO, cx - SPLASH_LOGO / 2,
        cy - SPLASH_LOGO / 2, SPLASH_LOGO);

    int y = cy + SPLASH_LOGO / 2 + SPLASH_RING_GAP + 40;

    int name_w = recon_text_width(big, RECONOS_FULL_NAME);
    recon_draw_text(p, big, cx - name_w / 2, y + recon_font_ascent(big),
        session->width, RECONOS_FULL_NAME, THEME(READOUT_TEXT));
    y += recon_font_line_height(big) + 4;

    char version[64];
    snprintf(version, sizeof(version), "Version %s", RECONOS_VERSION);
    int version_w = recon_text_width(session->font, version);
    recon_draw_text(p, session->font, cx - version_w / 2, y + ascent,
        session->width, version, THEME(MENU_TEXT_DISABLED));
    y += line + 28;

    /* The bar, as wide as the mark and its rings, so the whole thing reads as
     * one object rather than as a logo with a bar under it. */
    int bar_w = SPLASH_LOGO + SPLASH_RING_GAP * 2 + 48;
    int bar_x = cx - bar_w / 2;
    int bar_h = 4;
    int done = session->boot_step;
    if (done > BOOT_STEP_COUNT) {
        done = BOOT_STEP_COUNT;
    }

    /*
     * The empty part in the same dim grey as the rings, not in a field
     * colour: a text field's border is meant to be seen against a window, and
     * on this backdrop it came out brighter than the filled part -- a bar
     * that looked emptier the fuller it got.
     */
    recon_fill_rect(p, bar_x, y, bar_w, bar_h, THEME(MENU_TEXT_DISABLED));
    recon_fill_rect(p, bar_x, y, bar_w * done / BOOT_STEP_COUNT, bar_h,
        THEME(ACCENT));
    y += bar_h + 16;

    /*
     * The line it has reached, and what that line counted. Only the current
     * one, because a splash that scrolls a log is a boot message screen, and
     * this is not that.
     */
    int index = session->boot_step;
    if (index >= BOOT_STEP_COUNT) {
        index = BOOT_STEP_COUNT - 1;
    }

    char what[128];
    if (session->boot_detail[0] != '\0') {
        snprintf(what, sizeof(what), "%s   %s", BOOT_STEPS[index].label,
            session->boot_detail);
    } else {
        snprintf(what, sizeof(what), "%s", BOOT_STEPS[index].label);
    }
    int what_w = recon_text_width(session->font, what);
    recon_draw_text(p, session->font, cx - what_w / 2, y + ascent,
        session->width, what, THEME(MENU_TEXT_DISABLED));
}

static void draw(struct recon_session *session) {
    struct recon_panel *p = session->panel;
    if (p == NULL || session->stage == STAGE_DONE) {
        return;
    }

    int ascent = recon_font_ascent(session->font);
    int line = recon_font_line_height(session->font);
    if (line <= 0) {
        line = 18;
    }

    /* The whole screen, so nothing of the desktop shows through. This is a
     * gate, not a window over something. */
    recon_fill(p, THEME(READOUT));
    recon_hit_clear(p);

    /*
     * The splash has no card and takes no input, so it is drawn here and
     * returns rather than being a case in the switch below -- everything
     * after this point is about laying out a card with questions in it.
     */
    if (session->stage == STAGE_STOPPED) {
        draw_stopped(session, p);
        recon_panel_commit(p);
        return;
    }

    if (session->stage == STAGE_BOOTING) {
        draw_splash(session, p);
        recon_panel_commit(p);
        return;
    }

    int cx, cy, cw, ch;
    card_rect(session, &cx, &cy, &cw, &ch);

    recon_fill_rect(p, cx, cy, cw, ch, THEME(DIALOG));
    recon_draw_bevel(p, cx, cy, cw, ch, false);
    recon_stroke_rect(p, cx, cy, cw, ch, THEME(MENU_BORDER));

    /* Every screen but the login carries the band; the login screen has its
     * own, drawn larger, because it is the one people see every day. */
    draw_banner(session, p, cx, cy, cw);

    int x = cx + CARD_PADDING;
    int y = cy + BANNER_HEIGHT + CARD_PADDING - 6;
    int w = cw - CARD_PADDING * 2;

    switch (session->stage) {
    case STAGE_LAST_STOP: {
        draw_title(session, p, x, y, w, "The last run did not finish", NULL);
        y += TITLE_SIZE + GAP;

        recon_draw_text(p, session->font, x, y + ascent, w,
            session->stop_code, THEME(WARNING));
        y += line + 2;

        recon_draw_text(p, session->font, x, y + ascent, w,
            session->stop_summary, THEME(MENU_TEXT));
        y += line + GAP;

        recon_draw_text(p, session->font, x, y + ascent, w,
            "This machine is running normally now. Nothing that had been "
            "saved is lost.", THEME(MENU_TEXT_DISABLED));
        y += line;
        recon_draw_text(p, session->font, x, y + ascent, w,
            "Help has what the code means; the log is in /System/Logs.",
            THEME(MENU_TEXT_DISABLED));
        y += line + GAP;

        draw_button(session, p, x + w - BUTTON_WIDTH, y, BUTTON_WIDTH,
            "Continue", HIT_PRIMARY, true, true);
        break;
    }

    case STAGE_UPDATING: {
        char title[96];
        snprintf(title, sizeof(title), "Updating to %s", RECONOS_VERSION);
        draw_title(session, p, x, y, w, title, NULL);
        y += TITLE_SIZE + GAP;

        char from[96];
        snprintf(from, sizeof(from), "This machine last ran %s.",
            session->update_from[0] != '\0' ? session->update_from
                                            : "an earlier version");
        recon_draw_text(p, session->font, x, y + ascent, w, from,
            THEME(MENU_TEXT_DISABLED));
        y += line + GAP;

        /* The bar, filled in proportion to the steps that have finished. */
        int bar_h = 14;
        int done = session->update_step;
        if (done > UPDATE_STEP_COUNT) {
            done = UPDATE_STEP_COUNT;
        }

        recon_fill_rect(p, x, y, w, bar_h, THEME(FIELD));
        recon_fill_rect(p, x, y, w * done / UPDATE_STEP_COUNT, bar_h,
            THEME(ACCENT));
        recon_stroke_rect(p, x, y, w, bar_h, THEME(FIELD_BORDER));
        y += bar_h + GAP;

        /* What it is doing, and what that step actually found. */
        int index = session->update_step;
        if (index >= UPDATE_STEP_COUNT) {
            index = UPDATE_STEP_COUNT - 1;
        }
        recon_draw_text(p, session->font, x, y + ascent, w,
            UPDATE_STEPS[index].label, THEME(MENU_TEXT));
        y += line + 2;

        recon_draw_text(p, session->font, x, y + ascent, w,
            session->update_detail, THEME(MENU_TEXT_DISABLED));
        y += line + GAP;

        recon_draw_text(p, session->font, x, y + ascent, w,
            "Do not turn the machine off.", THEME(MENU_TEXT_DISABLED));
        break;
    }

    case STAGE_WELCOME:
        draw_title(session, p, x, y, w, "Welcome", NULL);
        y += TITLE_SIZE + GAP;

        recon_draw_text(p, session->font, x, y + ascent, w,
            "This machine has not been set up yet.", THEME(MENU_TEXT));
        y += line + 4;
        recon_draw_text(p, session->font, x, y + ascent, w,
            "Four short questions and it is yours: who you are, what to call",
            THEME(MENU_TEXT_DISABLED));
        y += line;
        recon_draw_text(p, session->font, x, y + ascent, w,
            "the machine, whether anything needs adjusting, and how it should",
            THEME(MENU_TEXT_DISABLED));
        y += line;
        recon_draw_text(p, session->font, x, y + ascent, w,
            "look. Nothing here is permanent; all of it can be changed later.",
            THEME(MENU_TEXT_DISABLED));
        y += line + GAP;
        break;

    case STAGE_ACCOUNT:
        draw_title(session, p, x, y, w, "Who are you?",
            "This account will be able to administer the system.");
        y += TITLE_SIZE + line + GAP;

        y = draw_field(session, p, x, y, w, "Name", &session->name,
            HIT_NAME_FIELD, session->focus == FOCUS_NAME);
        y = draw_field(session, p, x, y, w, "Password (you may leave this empty)",
            &session->password, HIT_PASSWORD_FIELD,
            session->focus == FOCUS_PASSWORD);
        y = draw_field(session, p, x, y, w, "Password again",
            &session->confirm, HIT_CONFIRM_FIELD,
            session->focus == FOCUS_CONFIRM);
        break;

    case STAGE_MACHINE:
        draw_title(session, p, x, y, w, "What is this machine called?",
            "Its name, not yours. Somewhere to put it when there is more "
            "than one.");
        y += TITLE_SIZE + line + GAP;

        y = draw_field(session, p, x, y, w, "Name", &session->machine,
            HIT_NAME_FIELD, true);
        y += 4;

        recon_draw_text(p, session->font, x, y + ascent, w,
            "Networking is not built yet, so nothing else can see this name",
            THEME(MENU_TEXT_DISABLED));
        y += line;
        recon_draw_text(p, session->font, x, y + ascent, w,
            "yet. It is asked for now so it is already right when they can.",
            THEME(MENU_TEXT_DISABLED));
        y += line;
        break;

    case STAGE_HELP:
        draw_title(session, p, x, y, w,
            "Does anything need adjusting?",
            "Text size, spacing, contrast, or colour vision.");
        y += TITLE_SIZE + line + GAP;

        for (int i = 0; i < HELP_COUNT; i++) {
            draw_row(session, p, x, y, w, HELP_CHOICES[i].label,
                HELP_CHOICES[i].detail, HIT_OPTION_BASE + i,
                i == session->option);
            y += ROW_HEIGHT;
        }
        break;

    case STAGE_LOOK: {
        draw_title(session, p, x, y, w, "How should it look?",
            "Choose one to see it. This can be changed later.");
        y += TITLE_SIZE + line + GAP;

        int count = recon_theme_count();
        if (count > OPTIONS_MAX) {
            count = OPTIONS_MAX;
        }
        for (int i = 0; i < count; i++) {
            struct recon_theme_info info;
            if (!recon_theme_at(i, &info)) {
                continue;
            }
            /*
             * The row first, then the picture over it. The other way round
             * meant the selected row's highlight was painted across its own
             * preview -- the one row you most wanted to see was the one with
             * no picture on it.
             */
            draw_row_at(session, p, x, y, w, PREVIEW_WIDTH + 18,
                info.name, info.description, HIT_OPTION_BASE + i,
                i == session->option);
            draw_theme_preview(p, i, x + 8,
                y + (ROW_HEIGHT - PREVIEW_HEIGHT) / 2);
            y += ROW_HEIGHT;
        }
        break;
    }

    case STAGE_READING:
        draw_title(session, p, x, y, w, "Reading and colour",
            "Choose whichever fits. It sets a skin you can change afterwards.");
        y += TITLE_SIZE + line + GAP;

        for (int i = 0; i < ACCESSIBILITY_COUNT; i++) {
            draw_row(session, p, x, y, w, ACCESSIBILITY[i].label,
                ACCESSIBILITY[i].detail, HIT_OPTION_BASE + i,
                i == session->option);
            y += ROW_HEIGHT;
        }
        break;

    case STAGE_FINISHED: {
        const char *who = session->name.text;

        char greeting[128];
        recon_text_printf(greeting, sizeof(greeting), "%s%s.",
            *who != '\0' ? "The tower is yours, " : "The tower is yours",
            *who != '\0' ? who : "");

        draw_title(session, p, x, y, w, greeting, NULL);
        y += TITLE_SIZE + GAP;

        recon_draw_text(p, session->font, x, y + ascent, w,
            "Everything you chose is saved and can be changed again from the",
            THEME(MENU_TEXT));
        y += line;
        recon_draw_text(p, session->font, x, y + ascent, w,
            "Control Panel, which is in the Apps menu at the bottom left.",
            THEME(MENU_TEXT));
        y += line + GAP;
        break;
    }

    case STAGE_LOGIN: {
        /*
         * Asking which account. A grid of faces with a name under each,
         * and nothing about how any of them signs in -- that is the next
         * screen's question, and only about the one that was chosen.
         */
        if (session->picking_account) {
            draw_title(session, p, x, y, w, "Who is using this machine?", NULL);
            y += TITLE_SIZE + GAP;

            int count = recon_users_count();
            if (count > ACCOUNTS_VISIBLE) {
                count = ACCOUNTS_VISIBLE;
            }

            int step = PICK_TILE;
            int columns = w / step;
            if (columns < 1) {
                columns = 1;
            }
            if (columns > count) {
                columns = count;
            }
            int grid_w = columns * step;
            int gx = x + (w - grid_w) / 2;

            for (int i = 0; i < count; i++) {
                struct recon_user user;
                if (!recon_users_at(i, &user)) {
                    break;
                }

                int tx = gx + (i % columns) * step;
                int ty = y + (i / columns) * (PICK_TILE + line);
                uint32_t id = HIT_ACCOUNT_BASE + i;
                bool hovered = (id == (uint32_t)session->hover);

                if (hovered) {
                    recon_fill_rect(p, tx + 4, ty, step - 8,
                        PICK_FACE + line * 2 + 12, THEME(MENU_HILITE));
                }

                recon_avatar_draw(p,
                    session->heading != NULL ? session->heading : session->font,
                    user.name, tx + (step - PICK_FACE) / 2, ty + 4, PICK_FACE);

                int name_w = recon_text_width(session->font, user.name);
                recon_draw_text(p, session->font, tx + (step - name_w) / 2,
                    ty + PICK_FACE + 10 + ascent, step, user.name,
                    hovered ? THEME(MENU_HILITE_TEXT) : THEME(MENU_TEXT));

                const char *role = user.role == RECON_ROLE_ADMINISTRATOR
                    ? "Administrator" : "Limited";
                int role_w = recon_text_width(session->font, role);
                recon_draw_text(p, session->font, tx + (step - role_w) / 2,
                    ty + PICK_FACE + 10 + line + ascent, step, role,
                    hovered ? THEME(MENU_HILITE_TEXT)
                            : THEME(MENU_TEXT_DISABLED));

                recon_hit_add(p, tx + 4, ty, step - 8,
                    PICK_FACE + line * 2 + 12, id);
            }
            break;
        }

        /*
         * The name of the system used to be drawn here as well as in the band
         * above, so the login screen said "Recon Towers OS" twice and had
         * nothing to say about the person signing in. This is their screen:
         * their picture, their name, and a box for their password.
         */
        struct recon_user chosen_user;
        bool have_user = recon_users_at(session->account, &chosen_user);

        /*
         * A locked machine says so, above the face. There is a session
         * running behind this screen and that is worth knowing: it is the
         * difference between "sign in" and "you left this open".
         */
        if (session->locked_to[0] != '\0') {
            const char *locked = "Locked";
            int locked_w = recon_text_width(session->font, locked);
            recon_draw_text(p, session->font, x + (w - locked_w) / 2,
                y + ascent, w, locked, THEME(ACCENT));
            y += line + 6;
        }

        /* The heading font for the initial: at this size the body font
         * leaves a letter lost in the middle of the disc. */
        int face = AVATAR_SIZE;
        recon_avatar_draw(p,
            session->heading != NULL ? session->heading : session->font,
            have_user ? chosen_user.name : NULL,
            x + (w - face) / 2, y, face);
        y += face + 10;

        if (have_user) {
            struct recon_font *big = session->heading != NULL
                ? session->heading : session->font;
            int name_w = recon_text_width(big, chosen_user.name);
            recon_draw_text(p, big, x + (w - name_w) / 2,
                y + recon_font_ascent(big), w, chosen_user.name,
                THEME(MENU_TEXT));
            y += recon_font_line_height(big) + 2;

            /* Role, and -- when this screen is locked rather than signed out
             * of -- that the session behind it is still theirs. */
            bool locked = session->locked_to[0] != '\0';
            char caption[96];
            snprintf(caption, sizeof(caption), "%s%s",
                chosen_user.role == RECON_ROLE_ADMINISTRATOR
                    ? "Administrator" : "Limited",
                locked ? "   still signed in" : "");

            int caption_w = recon_text_width(session->font, caption);
            recon_draw_text(p, session->font, x + (w - caption_w) / 2,
                y + ascent, w, caption, THEME(MENU_TEXT_DISABLED));
            y += line + GAP;
        }

        /* The password field is only useful if the chosen account has one. */
        int field_w = LOGIN_FIELD_WIDTH;
        if (field_w > w) {
            field_w = w;
        }
        int field_x = x + (w - field_w) / 2;

        if (have_user && chosen_user.has_password) {
            y = draw_field(session, p, field_x, y, field_w, "Password",
                &session->password, HIT_PASSWORD_FIELD, true);
        } else {
            const char *note = "This account has no password.";
            int note_w = recon_text_width(session->font, note);
            recon_draw_text(p, session->font, x + (w - note_w) / 2, y + ascent,
                w, note, THEME(MENU_TEXT_DISABLED));
            y += line + GAP;
        }

        /*
         * The other accounts used to sit here as a strip of faces, under the
         * password box. A stray click while typing changed who you were
         * signing in as, which is a way to lose a password into the wrong
         * field. Going back to the list is a deliberate act now, and it is
         * not offered at all while locked -- a locked machine has somebody's
         * session running behind it, and there is only one account it will
         * accept.
         */
        break;
    }

    default:
        break;
    }

    /* The message, above the buttons, where the eye already is. */
    int by = cy + ch - CARD_PADDING - BUTTON_HEIGHT;
    if (session->message[0] != '\0') {
        recon_draw_text(p, session->font, x, by - 8, w, session->message,
            session->message_is_warning ? THEME(WARNING)
                                        : THEME(MENU_TEXT_DISABLED));
    }

    /* Buttons along the bottom. */
    switch (session->stage) {
    case STAGE_WELCOME:
        draw_button(session, p, x + w - BUTTON_WIDTH, by, BUTTON_WIDTH,
            "Begin", HIT_PRIMARY, true, true);
        break;

    case STAGE_ACCOUNT:
    case STAGE_MACHINE:
    case STAGE_HELP:
    case STAGE_LOOK:
    case STAGE_READING:
        draw_button(session, p, x, by, BUTTON_WIDTH, "Back", HIT_BACK,
            false, true);
        draw_button(session, p, x + w - BUTTON_WIDTH, by, BUTTON_WIDTH,
            "Continue", HIT_PRIMARY, true, true);
        break;

    case STAGE_FINISHED: {
        /* "Start" is what the button did, not what it means. This is the
         * moment the machine becomes theirs, and the button should say so. */
        const char *label = "Enter the Tower";
        int width = recon_text_width(session->font, label) + 48;
        draw_button(session, p, x + w - width, by, width, label,
            HIT_PRIMARY, true, true);
        break;
    }

    case STAGE_LOGIN: {
        /*
         * Restart and shut down, bottom right, as two small round buttons.
         * Restart was not offered at all, so a machine that needed one had to
         * be shut down and started again by hand.
         */
        int px = x + w - POWER_BUTTON;
        int py = by + (BUTTON_HEIGHT - POWER_BUTTON) / 2;

        draw_power_button(session, p, px, py, false, HIT_SHUTDOWN);
        draw_power_button(session, p, px - POWER_BUTTON - 8, py, true,
            HIT_RESTART);

        if (session->picking_account) {
            break;
        }

        /*
         * Going back to the list, bottom left. Not while locked: there is
         * nowhere to go back to, because there is one account this screen
         * will accept.
         */
        if (session->locked_to[0] == '\0' && recon_users_count() > 1) {
            draw_button(session, p, x, by, BUTTON_WIDTH + 20,
                "Back to accounts", HIT_PICK_AGAIN, false, true);
        }

        /* Sign In sits under the password box rather than in the corner, so
         * it is next to the thing it acts on. */
        draw_button(session, p, x + (w - BUTTON_WIDTH) / 2, by, BUTTON_WIDTH,
            "Sign In", HIT_PRIMARY, true, recon_users_count() > 0);
        break;
    }

    default:
        break;
    }

    recon_panel_commit(p);
}

void recon_session_refresh(struct recon_session *session) {
    if (session == NULL) {
        return;
    }
    draw(session);
    recon_damage_all(session->server);
}

/* --- Moving through it --- */

/* Put a skin on and remember it, so choosing one shows it immediately. */
static void preview_theme(struct recon_session *session, int index) {
    struct recon_theme_info info;
    if (recon_theme_at(index, &info)) {
        recon_theme_set(info.name);
        recon_session_refresh(session);
    }
}

static void apply_accessibility(struct recon_session *session, int index) {
    if (index < 0 || index >= ACCESSIBILITY_COUNT) {
        return;
    }
    const struct accessibility_option *option = &ACCESSIBILITY[index];

    if (option->theme != NULL) {
        recon_theme_set(option->theme);
    }
    if (option->letter_spacing >= 0) {
        recon_registry_set_int(RECON_REG_USER, RECON_ACCESS_LETTER_KEY,
            option->letter_spacing);
        recon_registry_set_int(RECON_REG_USER, RECON_ACCESS_LINE_KEY,
            option->line_spacing);
        recon_access_apply(session->font);
    }
    recon_session_refresh(session);
}

/* Create the first account, which is what setup exists to do. */
static bool finish_account(struct recon_session *session) {
    const char *name = session->name.text;
    while (*name == ' ') {
        name++;
    }

    if (*name == '\0') {
        set_message(session, true, "A name is needed.");
        session->focus = FOCUS_NAME;
        return false;
    }
    if (strcmp(session->password.text, session->confirm.text) != 0) {
        set_message(session, true, "The two passwords are not the same.");
        session->focus = FOCUS_CONFIRM;
        return false;
    }

    /* The first account administers the system: somebody has to be able to,
     * and there is nobody else yet. */
    if (!recon_users_create(name, session->password.text,
            RECON_ROLE_ADMINISTRATOR)) {
        set_message(session, true, "%s", recon_users_last_error());
        return false;
    }

    /* Signed in straight away. Asking for the password of the account you
     * just made would be asking a question nobody wants. */
    if (!recon_users_login(name, session->password.text)) {
        set_message(session, true, "%s", recon_users_last_error());
        return false;
    }

    /* The passwords go out of memory now rather than sitting in a panel
     * struct for the rest of the session. */
    recon_edit_end(&session->password);
    recon_edit_end(&session->confirm);
    return true;
}

static void go_to(struct recon_session *session, enum stage stage) {
    session->stage = stage;
    session->message[0] = '\0';
    session->hover = -1;

    if (stage == STAGE_LOOK) {
        /* Start on whatever is already on, so Continue without touching
         * anything changes nothing. */
        session->option = 0;
        int count = recon_theme_count();
        for (int i = 0; i < count; i++) {
            struct recon_theme_info info;
            if (recon_theme_at(i, &info) &&
                    strcmp(info.name, recon_theme_current()) == 0) {
                session->option = i;
                break;
            }
        }
    } else if (stage == STAGE_READING) {
        session->option = 0;
    } else if (stage == STAGE_HELP) {
        /* "No" first and selected: it is the answer most people give, and a
         * question whose default is the rare answer is a question that costs
         * everybody something. */
        session->option = 0;
    } else if (stage == STAGE_MACHINE) {
        recon_edit_begin(&session->machine,
            recon_registry_get(RECON_REG_SYSTEM, MACHINE_NAME_KEY,
                MACHINE_NAME_DEFAULT), false);
    }

    if (stage == STAGE_DONE) {
        recon_panel_set_enabled(session->panel, false);
    } else {
        recon_panel_set_enabled(session->panel, true);
        recon_panel_raise_to_top(session->panel);
    }

    recon_session_refresh(session);
}

/*
 * What comes after the card reporting a stop from last time -- which is what
 * would have come straight after the splash. Split out so the card can be
 * skipped when there is nothing to say without duplicating the rest.
 */
static void begin_after_last_stop(struct recon_session *session);

/* The primary button, or Enter. */
static void advance(struct recon_session *session) {
    switch (session->stage) {
    case STAGE_LAST_STOP:
        begin_after_last_stop(session);
        break;

    case STAGE_WELCOME:
        go_to(session, STAGE_ACCOUNT);
        break;

    case STAGE_ACCOUNT:
        if (finish_account(session)) {
            go_to(session, STAGE_MACHINE);
        } else {
            recon_session_refresh(session);
        }
        break;

    case STAGE_MACHINE: {
        /* Empty is allowed and means the default: a machine with no name is
         * still a machine, and refusing to continue over it would be a
         * gate in front of something nobody can see the effect of yet. */
        const char *wanted = session->machine.length > 0
            ? session->machine.text : MACHINE_NAME_DEFAULT;
        recon_registry_set(RECON_REG_SYSTEM, MACHINE_NAME_KEY, wanted);
        go_to(session, STAGE_HELP);
        break;
    }

    case STAGE_HELP:
        session->wants_help = (session->option == 1);
        go_to(session, session->wants_help ? STAGE_READING : STAGE_LOOK);
        break;

    case STAGE_READING:
        apply_accessibility(session, session->option);
        go_to(session, STAGE_LOOK);
        break;

    case STAGE_LOOK:
        go_to(session, STAGE_FINISHED);
        break;

    case STAGE_FINISHED:
        /* Setup does not run again. */
        recon_registry_set_bool(RECON_REG_SYSTEM, "setup/done", true);
        session->signed_in_flag = true;
        go_to(session, STAGE_DONE);
        break;

    case STAGE_LOGIN: {
        struct recon_user user;
        if (!recon_users_at(session->account, &user)) {
            set_message(session, true, "Choose an account.");
            recon_session_refresh(session);
            return;
        }

        /*
         * While choosing, Enter picks the account rather than trying to sign
         * in as it. Otherwise the keyboard would skip the screen the mouse
         * has to go through, and an empty password would be submitted for
         * whichever account happened to be selected.
         */
        if (session->picking_account) {
            session->picking_account = false;
            session->hover = -1;
            recon_session_refresh(session);
            return;
        }

        /*
         * The lock is enforced here as well as drawn.
         *
         * Hiding the other accounts stops somebody choosing one; it does not
         * stop the selection being somewhere else for a reason nobody
         * anticipated. A lock that is only a drawing is not a lock.
         */
        if (session->locked_to[0] != '\0' &&
                strcmp(session->locked_to, user.name) != 0) {
            set_message(session, true,
                "The machine is locked. Only %s can unlock it.",
                session->locked_to);
            recon_session_refresh(session);
            return;
        }

        if (!recon_users_login(user.name, session->password.text)) {
            /*
             * Deliberately vague. Saying "wrong password" confirms the account
             * exists, which is information a stranger at the keyboard has not
             * earned.
             */
            set_message(session, true, "That did not work. Try again.");
            recon_edit_begin(&session->password, "", false);
            session->password.masked = true;
            recon_session_refresh(session);
            return;
        }

        recon_edit_end(&session->password);
        session->locked_to[0] = '\0';
        session->signed_in_flag = true;
        go_to(session, STAGE_DONE);
        break;
    }

    default:
        break;
    }
}

static void go_back(struct recon_session *session) {
    switch (session->stage) {
    case STAGE_ACCOUNT:
        go_to(session, STAGE_WELCOME);
        break;
    case STAGE_MACHINE:
        /*
         * Not back to the account screen: the account has been created and
         * signed into by then, and going back would offer to create it again.
         * Setup is only reversible up to the point where it changes something.
         */
        set_message(session, false, "The account is made; carry on.");
        recon_session_refresh(session);
        break;
    case STAGE_HELP:
        go_to(session, STAGE_MACHINE);
        break;
    case STAGE_READING:
        go_to(session, STAGE_HELP);
        break;
    case STAGE_LOOK:
        /* Back to whichever screen was actually shown, not to the one that
         * would have been next in a list: somebody who skipped the reading
         * options should not be shown them on the way back. */
        go_to(session, session->wants_help ? STAGE_READING : STAGE_HELP);
        break;
    default:
        break;
    }
}

/* --- Lifecycle --- */

/* The screen that says the system has stopped. Registered with the error
 * layer as soon as there is a session to draw it on, and defined at the
 * bottom of this file with the rest of the stop handling. */
static void show_stop_screen(struct recon_server *server,
    const struct recon_error_info *info, const char *detail);

struct recon_session *recon_session_create(struct recon_server *server,
        struct recon_font *font, int width, int height) {
    struct recon_session *session = calloc(1, sizeof(*session));
    if (session == NULL) {
        return NULL;
    }

    session->server = server;
    session->font = font;
    session->width = width;
    session->height = height;
    session->stage = STAGE_DONE;
    session->hover = -1;

    session->panel = recon_panel_create(&server->scene->tree, width, height);
    if (session->panel == NULL) {
        free(session);
        return NULL;
    }
    recon_panel_set_position(session->panel, 0, 0);
    recon_panel_set_enabled(session->panel, false);

    /*
     * From here on there is something to draw a stop on. Before this point a
     * stop goes to stderr, which is the honest answer for a fault that
     * happens before the display exists.
     */
    recon_error_set_screen(server, show_stop_screen);

    /*
     * A larger font for headings. NULL is coped with everywhere it is used:
     * a heading in the body size is worse-looking and still readable, which
     * is the right trade for something that can fail to load.
     */
    session->heading = recon_font_load(getenv("RECONOS_FONT"),
        recon_font_line_height(font) + 10);

    struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
    session->update_timer = wl_event_loop_add_timer(loop, update_tick, session);
    session->boot_timer = wl_event_loop_add_timer(loop, boot_tick, session);

    return session;
}

void recon_session_destroy(struct recon_session *session) {
    if (session == NULL) {
        return;
    }
    if (session->boot_timer != NULL) {
        wl_event_source_remove(session->boot_timer);
    }
    if (session->update_timer != NULL) {
        wl_event_source_remove(session->update_timer);
    }
    recon_font_destroy(session->heading);
    recon_panel_destroy(session->panel);
    free(session);
}

/* What the splash hands off to. Defined below, with the login screen it leads
 * into. */
static void begin_after_splash(struct recon_session *session);


/* Where the login screen would have gone, once the update screen is done. */
static void after_update(struct recon_session *session) {
    recon_registry_set(RECON_REG_SYSTEM, INSTALLED_VERSION_KEY,
        RECONOS_VERSION);

    if (recon_users_count() == 0) {
        go_to(session, STAGE_WELCOME);
    } else {
        session->picking_account = true;
        go_to(session, STAGE_LOGIN);
    }
}

static int boot_tick(void *data) {
    struct recon_session *session = data;

    if (session->stage != STAGE_BOOTING) {
        return 0;
    }

    session->boot_phase++;

    /*
     * The rings turn on every tick and the lines advance on every third, so
     * the animation can be smooth without the text flickering past faster
     * than it can be read.
     */
    if (++session->boot_ticks >= BOOT_TICKS_PER_STEP) {
        session->boot_ticks = 0;
        session->boot_step++;

        if (session->boot_step >= BOOT_STEP_COUNT) {
            begin_after_splash(session);
            return 0;
        }

        int found = -1;
        if (BOOT_STEPS[session->boot_step].count != NULL) {
            found = BOOT_STEPS[session->boot_step].count();
        }
        if (found >= 0) {
            snprintf(session->boot_detail, sizeof(session->boot_detail),
                "%d", found);
        } else {
            session->boot_detail[0] = '\0';
        }
    }

    recon_session_refresh(session);
    wl_event_source_timer_update(session->boot_timer, BOOT_TICK_MS);
    return 0;
}

void recon_session_begin(struct recon_session *session) {
    if (session == NULL) {
        return;
    }

    /*
     * Straight past the splash when there is no timer to drive it, or when
     * whoever started the system asked to skip it.
     *
     * Skippable because a splash is the one part of a system a developer sees
     * a hundred times a day and a user sees once -- and because the automated
     * tests drive a running system over the control socket, where a second of
     * announcing itself is a second of every test.
     */
    const char *skip = getenv("RECONOS_NO_SPLASH");
    if (session->boot_timer == NULL || (skip != NULL && *skip != '\0')) {
        begin_after_splash(session);
        return;
    }

    session->boot_step = 0;
    session->boot_ticks = 0;
    session->boot_phase = 0;
    session->boot_detail[0] = '\0';
    go_to(session, STAGE_BOOTING);
    wl_event_source_timer_update(session->boot_timer, BOOT_TICK_MS);
}

static int update_tick(void *data) {
    struct recon_session *session = data;

    if (session->stage != STAGE_UPDATING) {
        return 0;
    }

    int index = session->update_step;
    if (index >= UPDATE_STEP_COUNT) {
        after_update(session);
        return 0;
    }

    /*
     * The work for this step, then the screen. Doing it here rather than all
     * at once before the screen appears is what makes the bar mean something:
     * it is behind the work, not a decoration in front of it.
     */
    int found = -1;
    if (UPDATE_STEPS[index].run != NULL) {
        found = UPDATE_STEPS[index].run();
    }

    if (found > 0) {
        snprintf(session->update_detail, sizeof(session->update_detail),
            "%d added.", found);
    } else if (found == 0) {
        snprintf(session->update_detail, sizeof(session->update_detail),
            "Already up to date.");
    } else {
        session->update_detail[0] = '\0';
    }

    session->update_step++;
    recon_session_refresh(session);

    wl_event_source_timer_update(session->update_timer, UPDATE_STEP_MS);
    return 0;
}

/*
 * What the splash hands off to: the update screen, setup, or the login
 * screen. This was recon_session_begin itself before there was a splash in
 * front of it.
 */
static void begin_after_splash(struct recon_session *session) {
    /*
     * Did the last run stop?
     *
     * Before the update screen and before the login, because it is the oldest
     * news on the machine and everything after it is about what happens next.
     * Taken rather than read, so it is said once.
     */
    char code[16];
    char detail[512];

    if (recon_error_take_last(code, sizeof(code), detail, sizeof(detail))) {
        recon_text_copy(session->stop_code, sizeof(session->stop_code), code);
        recon_text_copy(session->stop_detail, sizeof(session->stop_detail),
            detail);

        const struct recon_error_info *info = recon_error_find(code);
        recon_text_copy(session->stop_summary, sizeof(session->stop_summary),
            info != NULL ? info->summary : "The system stopped");

        go_to(session, STAGE_LAST_STOP);
        return;
    }

    begin_after_last_stop(session);
}

static void begin_after_last_stop(struct recon_session *session) {
    recon_edit_begin(&session->name, "", false);
    recon_edit_begin(&session->password, "", false);
    recon_edit_begin(&session->confirm, "", false);
    session->password.masked = true;
    session->confirm.masked = true;
    session->focus = FOCUS_NAME;
    session->account = 0;
    session->account_scroll = 0;

    /*
     * A machine that last ran a different version has just been updated, and
     * says so rather than quietly being different. Skipped on a system that
     * has never been set up: there is nothing to have updated *from*, and the
     * first thing anybody sees should be the welcome.
     */
    const char *ran = recon_registry_get(RECON_REG_SYSTEM,
        INSTALLED_VERSION_KEY, "");

    if (recon_users_count() > 0 && strcmp(ran, RECONOS_VERSION) != 0) {
        snprintf(session->update_from, sizeof(session->update_from), "%s", ran);
        session->update_step = 0;
        session->update_detail[0] = '\0';
        go_to(session, STAGE_UPDATING);

        if (session->update_timer != NULL) {
            wl_event_source_timer_update(session->update_timer,
                UPDATE_STEP_MS);
        } else {
            /* No timer means no way to advance it, so do not show a screen
             * that would never finish. */
            after_update(session);
        }
        return;
    }

    /* Nothing new. Record it anyway, so a system that predates this has a
     * version written down and does not announce an update it did not have. */
    recon_registry_set(RECON_REG_SYSTEM, INSTALLED_VERSION_KEY,
        RECONOS_VERSION);

    if (recon_users_count() == 0) {
        /* Never set up. Ask who is using it. */
        go_to(session, STAGE_WELCOME);
    } else {
        /*
         * Starting up asks which account before it asks for anything else,
         * even when there is only one. A single account with its password box
         * already showing tells whoever is standing at the machine how to get
         * in before they have chosen anything -- and once there is a PIN or a
         * fingerprint, which of those it is varies per account.
         */
        session->picking_account = true;
        go_to(session, STAGE_LOGIN);
    }
}

void recon_session_lock(struct recon_session *session) {
    if (session == NULL) {
        return;
    }

    recon_users_logout();
    recon_edit_begin(&session->password, "", false);
    session->password.masked = true;
    session->account = 0;
    session->account_scroll = 0;
    /* Nobody is signed in any more, so there is no session to protect and
     * every account is fair to offer -- starting by asking which. */
    session->locked_to[0] = '\0';
    session->picking_account = true;
    go_to(session, STAGE_LOGIN);
}

void recon_session_lock_screen(struct recon_session *session) {
    if (session == NULL) {
        return;
    }

    /*
     * The account stays signed in. That is the whole difference from signing
     * out, and it is what makes the "signed in" mark on the login screen mean
     * something: with only sign-out there was never an account still signed in
     * to mark, so the mark would have been a decoration rather than a fact.
     *
     * Signing back in as the same person returns to the same desktop, windows
     * and all -- the shell only clears those when the person changes.
     */
    const char *who = recon_users_current();

    recon_edit_begin(&session->password, "", false);
    session->password.masked = true;
    session->account_scroll = 0;

    /* Locked to whoever locked it: theirs is the only account this screen
     * will offer, and their password is the only one that opens it. There is
     * nothing to choose between, so it does not ask. */
    session->account = 0;
    session->locked_to[0] = '\0';
    session->picking_account = false;
    if (who != NULL) {
        snprintf(session->locked_to, sizeof(session->locked_to), "%s", who);

        int count = recon_users_count();
        for (int i = 0; i < count; i++) {
            struct recon_user user;
            if (recon_users_at(i, &user) && strcmp(user.name, who) == 0) {
                session->account = i;
                break;
            }
        }
    }

    go_to(session, STAGE_LOGIN);
}

bool recon_session_active(struct recon_session *session) {
    return session != NULL && session->stage != STAGE_DONE;
}

bool recon_session_take_signed_in(struct recon_session *session) {
    if (session == NULL || !session->signed_in_flag) {
        return false;
    }
    session->signed_in_flag = false;
    return true;
}

void recon_session_resize(struct recon_session *session, int width, int height) {
    if (session == NULL) {
        return;
    }
    session->width = width;
    session->height = height;
    recon_panel_resize(session->panel, width, height);
    recon_panel_set_position(session->panel, 0, 0);
    recon_session_refresh(session);
}

/* --- Input --- */

/* Where in the panel, and what is there. */
static uint32_t hit_at(struct recon_session *session, double lx, double ly) {
    if (lx < 0 || ly < 0 || lx >= session->width || ly >= session->height) {
        return RECON_HIT_NONE;
    }
    return recon_hit_test(session->panel, (int)lx, (int)ly);
}

bool recon_session_handle_motion(struct recon_session *session,
        double lx, double ly) {
    if (!recon_session_active(session)) {
        return false;
    }

    int hover = (int)hit_at(session, lx, ly);
    if (hover != session->hover) {
        session->hover = hover;
        recon_session_refresh(session);
    }
    return true;
}

bool recon_session_handle_click(struct recon_session *session,
        double lx, double ly, bool pressed) {
    if (!recon_session_active(session)) {
        return false;
    }
    if (!pressed) {
        return true;
    }

    uint32_t hit = hit_at(session, lx, ly);

    if (hit >= HIT_OPTION_BASE) {
        int index = (int)(hit - HIT_OPTION_BASE);
        session->option = index;

        /* Choosing shows the result rather than describing it. */
        if (session->stage == STAGE_LOOK) {
            preview_theme(session, index);
        } else if (session->stage == STAGE_READING) {
            apply_accessibility(session, index);
        }
        return true;
    }

    if (hit >= HIT_ACCOUNT_BASE) {
        int index = session->account_scroll + (int)(hit - HIT_ACCOUNT_BASE);
        if (index != session->account) {
            session->account = index;
            /* A password typed for one account is not an attempt at
             * another. */
            recon_edit_begin(&session->password, "", false);
            session->password.masked = true;
            session->message[0] = '\0';
        }
        /* Choosing an account is what takes you to its sign-in screen. */
        session->picking_account = false;
        session->hover = -1;
        recon_session_refresh(session);
        return true;
    }

    switch (hit) {
    case HIT_PRIMARY:
        advance(session);
        break;
    case HIT_BACK:
        go_back(session);
        break;
    case HIT_SHUTDOWN:
        recon_quit(session->server);
        break;
    case HIT_RESTART:
        recon_restart(session->server);
        break;
    case HIT_PICK_AGAIN:
        /* Back to the list. The typed password goes with it: it was an
         * attempt at one account and means nothing about another. */
        session->picking_account = true;
        recon_edit_begin(&session->password, "", false);
        session->password.masked = true;
        session->message[0] = '\0';
        session->hover = -1;
        recon_session_refresh(session);
        break;
    case HIT_NAME_FIELD:
        session->focus = FOCUS_NAME;
        recon_session_refresh(session);
        break;
    case HIT_PASSWORD_FIELD:
        session->focus = FOCUS_PASSWORD;
        recon_session_refresh(session);
        break;
    case HIT_CONFIRM_FIELD:
        session->focus = FOCUS_CONFIRM;
        recon_session_refresh(session);
        break;
    default:
        break;
    }
    return true;
}

/* The field the keyboard is currently going to. */
static struct recon_edit *focused_edit(struct recon_session *session) {
    if (session->stage == STAGE_LOGIN) {
        return &session->password;
    }
    if (session->stage == STAGE_MACHINE) {
        return &session->machine;
    }
    if (session->stage != STAGE_ACCOUNT) {
        return NULL;
    }
    switch (session->focus) {
    case FOCUS_NAME: return &session->name;
    case FOCUS_PASSWORD: return &session->password;
    default: return &session->confirm;
    }
}

bool recon_session_handle_key(struct recon_session *session,
        xkb_keysym_t sym, uint32_t modifiers) {
    if (!recon_session_active(session)) {
        return false;
    }

    /*
     * A stopped system takes one key and shuts down.
     *
     * Any key rather than a named one, because somebody at a stopped machine
     * should not have to find a particular key -- and because there is
     * nothing else this screen could mean by a keypress.
     */
    if (session->stage == STAGE_STOPPED) {
        recon_quit(session->server);
        return true;
    }

    /* Tab moves between fields, which is how a form is expected to work. */
    if (sym == XKB_KEY_Tab && session->stage == STAGE_ACCOUNT) {
        bool backwards = (modifiers & RECON_MOD_SHIFT) != 0;
        int next = (int)session->focus + (backwards ? -1 : 1);
        if (next < 0) {
            next = FOCUS_CONFIRM;
        }
        if (next > FOCUS_CONFIRM) {
            next = FOCUS_NAME;
        }
        session->focus = (enum focus)next;
        recon_session_refresh(session);
        return true;
    }

    /*
     * Escape on a sign-in screen goes back to the accounts, which is what
     * Escape means everywhere else: undo the choice that got you here. Not
     * while locked, where there is no choice to undo.
     */
    if (sym == XKB_KEY_Escape && session->stage == STAGE_LOGIN &&
            !session->picking_account && session->locked_to[0] == '\0' &&
            recon_users_count() > 1) {
        session->picking_account = true;
        recon_edit_begin(&session->password, "", false);
        session->password.masked = true;
        session->message[0] = '\0';
        session->hover = -1;
        recon_session_refresh(session);
        return true;
    }

    /* Up and Down walk a list, wherever there is one. */
    if (sym == XKB_KEY_Up || sym == XKB_KEY_Down) {
        int step = (sym == XKB_KEY_Down) ? 1 : -1;

        if (session->stage == STAGE_LOGIN) {
            /*
             * Only while choosing. Once an account is picked the arrows have
             * nothing to move between, and stepping off it would be both a
             * surprise and -- while locked -- a way round the lock.
             */
            int count = (session->picking_account &&
                    session->locked_to[0] == '\0')
                ? recon_users_count() : 0;
            int next = session->account + step;
            if (next >= 0 && next < count) {
                session->account = next;
                recon_edit_begin(&session->password, "", false);
                session->password.masked = true;
            }
        } else if (session->stage == STAGE_LOOK) {
            int next = session->option + step;
            if (next >= 0 && next < recon_theme_count()) {
                preview_theme(session, next);
                session->option = next;
            }
        } else if (session->stage == STAGE_READING) {
            int next = session->option + step;
            if (next >= 0 && next < ACCESSIBILITY_COUNT) {
                session->option = next;
                apply_accessibility(session, next);
            }
        } else if (session->stage == STAGE_HELP) {
            int next = session->option + step;
            if (next >= 0 && next < HELP_COUNT) {
                session->option = next;
            }
        }
        recon_session_refresh(session);
        return true;
    }

    struct recon_edit *edit = focused_edit(session);
    if (edit != NULL) {
        switch (recon_edit_key(edit, sym, modifiers)) {
        case RECON_EDIT_COMMIT:
            advance(session);
            return true;
        case RECON_EDIT_CHANGED:
            recon_session_refresh(session);
            return true;
        case RECON_EDIT_CANCEL:
        case RECON_EDIT_IGNORED:
            break;
        }
    }

    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        advance(session);
        return true;
    }

    /* Everything else is swallowed. Nothing behind this should see a key. */
    return true;
}

/*
 * Where an account's tile is on the login screen, so a test can choose one
 * the way a person does.
 *
 * The account grid was the one part of the gate nothing outside could reach:
 * every other screen is driven by keys, and picking an account is a click on
 * a tile whose position depends on how many accounts there are and how far
 * the grid has scrolled. Tests either guessed a coordinate or skipped it --
 * which is why lock, switch-user and what a limited account may do were all
 * verified by hand, and all three had bugs in them.
 *
 * False when the grid is not showing, or there is no such account, or it has
 * scrolled out of view. Coordinates are in screen space, ready for a click.
 */
bool recon_session_account_at(struct recon_session *session, const char *name,
        int *x, int *y) {
    if (session == NULL || name == NULL || session->panel == NULL) {
        return false;
    }
    if (session->stage != STAGE_LOGIN || !session->picking_account) {
        return false;
    }

    int count = recon_users_count();
    for (int i = 0; i < count; i++) {
        struct recon_user user;
        if (!recon_users_at(i, &user) || strcasecmp(user.name, name) != 0) {
            continue;
        }

        /* The tiles carry a hit region each, numbered from the first one
         * showing rather than from the first account, so this asks the panel
         * where that id ended up instead of repeating the layout. */
        uint32_t wanted = HIT_ACCOUNT_BASE + (uint32_t)(i - session->account_scroll);
        if (i < session->account_scroll) {
            return false;
        }

        int px = 0, py = 0;
        recon_panel_position(session->panel, &px, &py);

        for (size_t r = 0;; r++) {
            int rx, ry, rw, rh;
            uint32_t id;
            if (!recon_hit_region(session->panel, r, &rx, &ry, &rw, &rh, &id)) {
                return false;
            }
            if (id != wanted) {
                continue;
            }
            if (x != NULL) { *x = px + rx + rw / 2; }
            if (y != NULL) { *y = py + ry + rh / 2; }
            return true;
        }
    }
    return false;
}

void recon_session_describe(struct recon_session *session, char *out, size_t size) {
    if (session == NULL || out == NULL || size == 0) {
        return;
    }

    /*
     * Designated initialisers, so a stage added in the middle of the enum
     * cannot silently shift every name one along. Two were added and this
     * table was not, which is how "login" started reporting itself as null.
     */
    static const char *const STAGE_NAMES[] = {
        [STAGE_BOOTING]  = "booting",
        [STAGE_UPDATING] = "updating",
        [STAGE_WELCOME]  = "welcome",
        [STAGE_ACCOUNT]  = "account",
        [STAGE_MACHINE]  = "machine",
        [STAGE_HELP]     = "help",
        [STAGE_READING]  = "reading",
        [STAGE_LOOK]     = "look",
        [STAGE_FINISHED] = "finished",
        [STAGE_LOGIN]    = "login",
        [STAGE_DONE]     = "done",
    };

    snprintf(out, size,
        "session: %s%s\n"
        "  accounts: %d\n"
        "  selected: %d\n"
        "  signed in: %s\n"
        "  message: %s\n",
        STAGE_NAMES[session->stage],
        session->locked_to[0] != '\0' ? " (locked)"
            : (session->picking_account ? " (choosing an account)" : ""),
        recon_users_count(),
        session->stage == STAGE_LOGIN ? session->account : session->option,
        recon_users_current() != NULL ? recon_users_current() : "(nobody)",
        session->message);
}

/*
 * The system has stopped.
 *
 * The record is already on disk by the time this is called -- that happens
 * first, precisely because the screen is the part most likely to fail when
 * what failed is the drawing.
 *
 * Returns with the screen up and the session owning the machine, or does not
 * return at all when there is nothing to draw with. The second case is real:
 * "no usable font" is a stop, and it is a stop that happens before there is
 * any way to say so on screen. Then stderr is what is left, and it is better
 * than a blank display.
 */
static void show_stop_screen(struct recon_server *server,
        const struct recon_error_info *info, const char *detail) {
    struct recon_session *session =
        (server != NULL) ? recon_shell_session(server->shell) : NULL;

    if (session == NULL || session->panel == NULL || session->font == NULL) {
        fprintf(stderr, "\nReconOS has stopped.\n\n  %s  %s\n\n%s\n%s%s\n\n",
            info->code, info->summary, info->detail,
            (detail != NULL && detail[0] != '\0') ? "\n" : "",
            (detail != NULL) ? detail : "");
        exit(1);
    }

    recon_text_copy(session->stop_code, sizeof(session->stop_code),
        info->code);
    recon_text_copy(session->stop_summary, sizeof(session->stop_summary),
        info->summary);
    recon_text_copy(session->stop_detail, sizeof(session->stop_detail),
        info->detail);
    recon_text_copy(session->stop_advice, sizeof(session->stop_advice),
        (detail != NULL) ? detail : "");

    /*
     * The timers go first. A splash still ticking behind a stop screen would
     * walk the session on to the login while the stop is being read.
     */
    if (session->boot_timer != NULL) {
        wl_event_source_timer_update(session->boot_timer, 0);
    }
    if (session->update_timer != NULL) {
        wl_event_source_timer_update(session->update_timer, 0);
    }

    session->stage = STAGE_STOPPED;
    recon_panel_set_enabled(session->panel, true);
    recon_panel_raise_to_top(session->panel);
    recon_session_refresh(session);
}
