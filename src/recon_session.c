/*
 * Setup and signing in. See include/recon_session.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "ReconOS.h"
#include "recon_access.h"
#include "recon_appwin.h"
#include "recon_registry.h"
#include "recon_server.h"
#include "recon_theme.h"
#include "recon_ui.h"
#include "recon_users.h"

/* --- Layout --- */

#define CARD_WIDTH 460
#define CARD_PADDING 28
#define TITLE_SIZE 30
#define ROW_HEIGHT 34
#define FIELD_HEIGHT 30
#define BUTTON_HEIGHT 32
#define BUTTON_WIDTH 130
#define GAP 14

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
#define HIT_ACCOUNT_BASE 100
#define HIT_OPTION_BASE 200

/* Where in the flow we are. */
enum stage {
    /* Setup, in order. */
    STAGE_WELCOME,
    STAGE_ACCOUNT,
    STAGE_LOOK,
    STAGE_READING,
    STAGE_FINISHED,
    /* Not part of setup. */
    STAGE_LOGIN,
    /* Nothing showing; the desktop has it. */
    STAGE_DONE,
};

/* Which field the keyboard is going to. */
enum focus {
    FOCUS_NAME,
    FOCUS_PASSWORD,
    FOCUS_CONFIRM,
};

struct recon_session {
    struct recon_server *server;
    struct recon_font *font;
    struct recon_panel *panel;

    int width, height;
    enum stage stage;
    bool signed_in_flag;

    /* Setup and login share these; only one is showing at a time. */
    struct recon_edit name;
    struct recon_edit password;
    struct recon_edit confirm;
    enum focus focus;

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

    switch (session->stage) {
    case STAGE_WELCOME:
    case STAGE_FINISHED:
        return CARD_PADDING * 2 + TITLE_SIZE + line * 4 + BUTTON_HEIGHT + GAP * 2;
    case STAGE_ACCOUNT:
        return CARD_PADDING * 2 + TITLE_SIZE + line * 2 +
            (FIELD_HEIGHT + line + GAP) * 3 + BUTTON_HEIGHT + GAP * 2;
    case STAGE_LOOK: {
        int count = recon_theme_count();
        if (count > OPTIONS_MAX) {
            count = OPTIONS_MAX;
        }
        return CARD_PADDING * 2 + TITLE_SIZE + line * 2 +
            ROW_HEIGHT * count + BUTTON_HEIGHT + GAP * 3;
    }
    case STAGE_READING:
        return CARD_PADDING * 2 + TITLE_SIZE + line * 2 +
            ROW_HEIGHT * ACCESSIBILITY_COUNT + BUTTON_HEIGHT + GAP * 3;
    case STAGE_LOGIN:
    default:
        return CARD_PADDING * 2 + TITLE_SIZE + line +
            ROW_HEIGHT * ACCOUNTS_VISIBLE + FIELD_HEIGHT + line +
            BUTTON_HEIGHT + GAP * 4;
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
    recon_draw_bevel(p, x, y, w, BUTTON_HEIGHT, false);

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
static void draw_row(struct recon_session *session, struct recon_panel *p,
        int x, int y, int w, const char *label, const char *detail,
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
    recon_draw_text(p, session->font, x + 12, y + (ROW_HEIGHT + ascent) / 2 - 2,
        w - 24, label, ink);

    if (detail != NULL && *detail != '\0') {
        recon_draw_text(p, session->font, x + 24 + label_w,
            y + (ROW_HEIGHT + ascent) / 2 - 2,
            w - 36 - label_w, detail, faint);
    }

    recon_hit_add(p, x, y, w, ROW_HEIGHT, id);
}

static void draw_title(struct recon_session *session, struct recon_panel *p,
        int x, int y, int w, const char *title, const char *subtitle) {
    int ascent = recon_font_ascent(session->font);
    int line = recon_font_line_height(session->font);
    if (line <= 0) {
        line = 18;
    }

    recon_draw_text(p, session->font, x, y + ascent + 6, w, title,
        THEME(MENU_TEXT));

    if (subtitle != NULL && *subtitle != '\0') {
        recon_draw_text(p, session->font, x, y + TITLE_SIZE + ascent, w,
            subtitle, THEME(MENU_TEXT_DISABLED));
    }
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

    int cx, cy, cw, ch;
    card_rect(session, &cx, &cy, &cw, &ch);

    recon_fill_rect(p, cx, cy, cw, ch, THEME(DIALOG));
    recon_draw_bevel(p, cx, cy, cw, ch, false);
    recon_stroke_rect(p, cx, cy, cw, ch, THEME(MENU_BORDER));

    int x = cx + CARD_PADDING;
    int y = cy + CARD_PADDING;
    int w = cw - CARD_PADDING * 2;

    switch (session->stage) {
    case STAGE_WELCOME:
        draw_title(session, p, x, y, w, RECONOS_FULL_NAME,
            "Version " RECONOS_VERSION);
        y += TITLE_SIZE + line + GAP;

        recon_draw_text(p, session->font, x, y + ascent, w,
            "Nobody has used this system yet.", THEME(MENU_TEXT));
        y += line;
        recon_draw_text(p, session->font, x, y + ascent, w,
            "A few questions, and it is yours.", THEME(MENU_TEXT));
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
            draw_row(session, p, x, y, w, info.name, info.description,
                HIT_OPTION_BASE + i, i == session->option);
            y += ROW_HEIGHT;
        }
        break;
    }

    case STAGE_READING:
        draw_title(session, p, x, y, w, "Reading and colour",
            "For anyone who needs it. Skip if you do not.");
        y += TITLE_SIZE + line + GAP;

        for (int i = 0; i < ACCESSIBILITY_COUNT; i++) {
            draw_row(session, p, x, y, w, ACCESSIBILITY[i].label,
                ACCESSIBILITY[i].detail, HIT_OPTION_BASE + i,
                i == session->option);
            y += ROW_HEIGHT;
        }
        break;

    case STAGE_FINISHED:
        draw_title(session, p, x, y, w, "Ready",
            NULL);
        y += TITLE_SIZE + line + GAP;

        recon_draw_text(p, session->font, x, y + ascent, w,
            "Everything is set up.", THEME(MENU_TEXT));
        y += line;
        recon_draw_text(p, session->font, x, y + ascent, w,
            "Alt+Enter opens a terminal; the Apps button is bottom left.",
            THEME(MENU_TEXT_DISABLED));
        y += line + GAP;
        break;

    case STAGE_LOGIN: {
        draw_title(session, p, x, y, w, RECONOS_FULL_NAME, NULL);
        y += TITLE_SIZE + GAP;

        int count = recon_users_count();
        int shown = count < ACCOUNTS_VISIBLE ? count : ACCOUNTS_VISIBLE;

        for (int i = 0; i < shown; i++) {
            int index = session->account_scroll + i;
            struct recon_user user;
            if (!recon_users_at(index, &user)) {
                break;
            }
            draw_row(session, p, x, y, w, user.name,
                user.role == RECON_ROLE_ADMINISTRATOR ? "Administrator" : NULL,
                HIT_ACCOUNT_BASE + i, index == session->account);
            y += ROW_HEIGHT;
        }
        y += GAP;

        /* The password field is only useful if the chosen account has one. */
        struct recon_user chosen;
        bool needs_password = recon_users_at(session->account, &chosen) &&
            chosen.has_password;

        if (needs_password) {
            y = draw_field(session, p, x, y, w, "Password", &session->password,
                HIT_PASSWORD_FIELD, true);
        } else {
            recon_draw_text(p, session->font, x, y + ascent, w,
                "This account has no password.", THEME(MENU_TEXT_DISABLED));
            y += line + GAP;
        }
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
    case STAGE_LOOK:
    case STAGE_READING:
        draw_button(session, p, x, by, BUTTON_WIDTH, "Back", HIT_BACK,
            false, true);
        draw_button(session, p, x + w - BUTTON_WIDTH, by, BUTTON_WIDTH,
            "Continue", HIT_PRIMARY, true, true);
        break;

    case STAGE_FINISHED:
        draw_button(session, p, x + w - BUTTON_WIDTH, by, BUTTON_WIDTH,
            "Start", HIT_PRIMARY, true, true);
        break;

    case STAGE_LOGIN:
        draw_button(session, p, x, by, BUTTON_WIDTH, "Shut Down",
            HIT_SHUTDOWN, false, true);
        draw_button(session, p, x + w - BUTTON_WIDTH, by, BUTTON_WIDTH,
            "Sign In", HIT_PRIMARY, true, recon_users_count() > 0);
        break;

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
    }

    if (stage == STAGE_DONE) {
        recon_panel_set_enabled(session->panel, false);
    } else {
        recon_panel_set_enabled(session->panel, true);
        recon_panel_raise_to_top(session->panel);
    }

    recon_session_refresh(session);
}

/* The primary button, or Enter. */
static void advance(struct recon_session *session) {
    switch (session->stage) {
    case STAGE_WELCOME:
        go_to(session, STAGE_ACCOUNT);
        break;

    case STAGE_ACCOUNT:
        if (finish_account(session)) {
            go_to(session, STAGE_LOOK);
        } else {
            recon_session_refresh(session);
        }
        break;

    case STAGE_LOOK:
        go_to(session, STAGE_READING);
        break;

    case STAGE_READING:
        apply_accessibility(session, session->option);
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
    case STAGE_LOOK:
        /*
         * Not back to the account screen: the account has been created and
         * signed into by then, and going back would offer to create it again.
         * Setup is only reversible up to the point where it changes something.
         */
        set_message(session, false, "The account is made; carry on.");
        recon_session_refresh(session);
        break;
    case STAGE_READING:
        go_to(session, STAGE_LOOK);
        break;
    default:
        break;
    }
}

/* --- Lifecycle --- */

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
    return session;
}

void recon_session_destroy(struct recon_session *session) {
    if (session == NULL) {
        return;
    }
    recon_panel_destroy(session->panel);
    free(session);
}

void recon_session_begin(struct recon_session *session) {
    if (session == NULL) {
        return;
    }

    recon_edit_begin(&session->name, "", false);
    recon_edit_begin(&session->password, "", false);
    recon_edit_begin(&session->confirm, "", false);
    session->password.masked = true;
    session->confirm.masked = true;
    session->focus = FOCUS_NAME;
    session->account = 0;
    session->account_scroll = 0;

    if (recon_users_count() == 0) {
        /* Never set up. Ask who is using it. */
        go_to(session, STAGE_WELCOME);
    } else {
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

    /* Up and Down walk a list, wherever there is one. */
    if (sym == XKB_KEY_Up || sym == XKB_KEY_Down) {
        int step = (sym == XKB_KEY_Down) ? 1 : -1;

        if (session->stage == STAGE_LOGIN) {
            int count = recon_users_count();
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

void recon_session_describe(struct recon_session *session, char *out, size_t size) {
    if (session == NULL || out == NULL || size == 0) {
        return;
    }

    static const char *const STAGE_NAMES[] = {
        "welcome", "account", "look", "reading", "finished", "login", "done",
    };

    snprintf(out, size,
        "session: %s\n"
        "  accounts: %d\n"
        "  selected: %d\n"
        "  signed in: %s\n"
        "  message: %s\n",
        STAGE_NAMES[session->stage],
        recon_users_count(),
        session->stage == STAGE_LOGIN ? session->account : session->option,
        recon_users_current() != NULL ? recon_users_current() : "(nobody)",
        session->message);
}
