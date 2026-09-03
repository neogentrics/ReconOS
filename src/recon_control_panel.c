/*
 * The Control Panel. See include/recon_control_panel.h.
 *
 * Four pages down the left, whichever is chosen on the right. Everything it
 * changes is a setting something else already reads -- the skin the shell
 * draws with, the spacing the text layer uses, the accounts the login screen
 * offers -- so nothing here has state of its own to fall out of step.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ReconOS.h"
#include "recon_access.h"
#include "recon_appwin.h"
#include "recon_control_panel.h"
#include "recon_fs.h"
#include "recon_icons.h"
#include "recon_modules.h"
#include "recon_procinfo.h"
#include "recon_registry.h"
#include "recon_server.h"
#include "recon_shell.h"
#include "recon_theme.h"
#include "recon_ui.h"
#include "recon_users.h"

#define SIDEBAR_WIDTH 150
#define ROW_HEIGHT 26
#define HEADER_HEIGHT 34
#define PADDING 12
#define BUTTON_HEIGHT 24
#define FIELD_HEIGHT 26
#define STATUS_HEIGHT 24

#define COLOR_BG THEME(WINDOW_FRAME)
#define COLOR_PANEL THEME(SURFACE)
#define COLOR_TEXT THEME(SURFACE_TEXT)
#define COLOR_DIM THEME(SURFACE_TEXT_DIM)
#define COLOR_SELECTED THEME(SELECTION)
#define COLOR_SELECTED_TEXT THEME(SELECTION_TEXT)
#define COLOR_BUTTON THEME(BUTTON)
#define COLOR_BUTTON_ACTIVE THEME(BUTTON_ACTIVE)
#define COLOR_SEPARATOR THEME(MENU_SEPARATOR)
#define COLOR_WARNING THEME(WARNING)
#define COLOR_ROW_ALT THEME(SURFACE_ALT)

/* Hit ids. */
#define HIT_PAGE_BASE (RECON_APPWIN_HIT_USER + 10)
#define HIT_ROW_BASE (RECON_APPWIN_HIT_USER + 100)
#define HIT_ACTION_BASE (RECON_APPWIN_HIT_USER + 200)
#define HIT_FIELD_BASE (RECON_APPWIN_HIT_USER + 300)

/* What the buttons on each page do. */
enum action {
    ACTION_NONE,
    /* Accounts */
    ACTION_ADD_USER,
    ACTION_REMOVE_USER,
    ACTION_TOGGLE_ROLE,
    ACTION_SET_PASSWORD,
    /* Reading */
    ACTION_SPACING_LESS,
    ACTION_SPACING_MORE,
    ACTION_LINES_LESS,
    ACTION_LINES_MORE,
    ACTION_SIZE_LESS,
    ACTION_SIZE_MORE,
    ACTION_RESET_READING,
};

enum page {
    PAGE_ACCOUNTS,
    PAGE_APPEARANCE,
    PAGE_READING,
    PAGE_SYSTEM,
    PAGE_COUNT,
};

static const struct {
    const char *label;
    const char *icon;
} PAGES[PAGE_COUNT] = {
    { "Accounts", RECON_ICON_APP },
    { "Appearance", RECON_ICON_SYSTEM },
    { "Reading", RECON_ICON_NOTEPAD },
    { "System", RECON_ICON_TASKMGR },
};

/* What a pending question is about, since the answer arrives later. */
enum question {
    QUESTION_NONE,
    QUESTION_REMOVE_USER,
};

struct control_panel {
    struct recon_server *server;
    struct recon_font *font;
    struct recon_appwin *win;

    enum page page;
    int selected;

    /*
     * Adding an account, or changing a password. Both need a name and a
     * secret, so they share the fields and differ in what is done with them.
     */
    bool editing;
    bool editing_password_only;
    struct recon_edit name;
    struct recon_edit password;
    bool password_focused;

    enum question question;
    char question_target[RECON_USERS_NAME_MAX];

    char status[192];
    bool status_is_warning;

    /* Where the list was drawn, so a right click can tell a row from the
     * space under the last one. */
    int list_x, list_y, list_w, list_h;
};

static void set_status(struct control_panel *cp, bool warning, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void set_status(struct control_panel *cp, bool warning, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(cp->status, sizeof(cp->status), fmt, args);
    va_end(args);
    cp->status_is_warning = warning;
}

/* --- Small drawing helpers --- */

static int draw_button(struct control_panel *cp, struct recon_panel *p,
        int x, int y, const char *label, uint32_t id, bool enabled) {
    int ascent = recon_font_ascent(cp->font);
    int width = recon_text_width(cp->font, label) + 22;

    recon_fill_rect(p, x, y, width, BUTTON_HEIGHT,
        enabled ? COLOR_BUTTON : COLOR_BG);
    recon_draw_bevel(p, x, y, width, BUTTON_HEIGHT, false);
    recon_draw_text(p, cp->font, x + 11, y + (BUTTON_HEIGHT + ascent) / 2 - 2,
        width - 16, label, enabled ? COLOR_TEXT : COLOR_DIM);

    /* A disabled button is drawn but not registered, so it cannot be pressed
     * and cannot report a failure the user could not have avoided. */
    if (enabled) {
        recon_hit_add(p, x, y, width, BUTTON_HEIGHT, id);
    }
    return x + width + 6;
}

static int draw_heading(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, const char *title, const char *detail) {
    int ascent = recon_font_ascent(cp->font);
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    recon_draw_text(p, cp->font, x, y + ascent, w, title, COLOR_TEXT);
    y += line;

    if (detail != NULL) {
        recon_draw_text(p, cp->font, x, y + ascent, w, detail, COLOR_DIM);
        y += line;
    }
    return y + 6;
}

/* A row of a list, with an optional second column. */
static void draw_row(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int index, const char *label, const char *detail,
        bool selected) {
    int ascent = recon_font_ascent(cp->font);

    if (selected) {
        recon_fill_rect(p, x, y, w, ROW_HEIGHT, COLOR_SELECTED);
    } else if (index % 2 == 1) {
        recon_fill_rect(p, x, y, w, ROW_HEIGHT, COLOR_ROW_ALT);
    }

    recon_color ink = selected ? COLOR_SELECTED_TEXT : COLOR_TEXT;
    recon_color faint = selected ? COLOR_SELECTED_TEXT : COLOR_DIM;

    recon_draw_text(p, cp->font, x + 10, y + (ROW_HEIGHT + ascent) / 2 - 2,
        w / 2, label, ink);
    if (detail != NULL) {
        recon_draw_text(p, cp->font, x + w / 2, y + (ROW_HEIGHT + ascent) / 2 - 2,
            w / 2 - 10, detail, faint);
    }

    recon_hit_add(p, x, y, w, ROW_HEIGHT, HIT_ROW_BASE + index);
}

/* --- The pages --- */

static void draw_accounts(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    bool admin = recon_users_may_administer();

    y = draw_heading(cp, p, x, y, w, "Accounts",
        admin ? "Who can use this system."
              : "Only an administrator can change these.");

    int count = recon_users_count();
    int rows = (h - (y - 0) - BUTTON_HEIGHT - PADDING * 2) / ROW_HEIGHT;
    if (rows < 1) {
        rows = 1;
    }

    cp->list_x = x;
    cp->list_y = y;
    cp->list_w = w;
    cp->list_h = rows * ROW_HEIGHT;

    recon_fill_rect(p, x, y, w, cp->list_h, COLOR_PANEL);

    for (int i = 0; i < count && i < rows; i++) {
        struct recon_user user;
        if (!recon_users_at(i, &user)) {
            break;
        }

        char detail[96];
        snprintf(detail, sizeof(detail), "%s%s%s",
            user.role == RECON_ROLE_ADMINISTRATOR ? "Administrator" : "Limited",
            user.has_password ? "" : "   no password",
            (recon_users_current() != NULL &&
             strcmp(recon_users_current(), user.name) == 0)
                ? "   signed in" : "");

        draw_row(cp, p, x, y + i * ROW_HEIGHT, w, i, user.name, detail,
            i == cp->selected);
    }

    y += cp->list_h + PADDING;

    /* Adding an account, or changing a password, happens here rather than in
     * a separate window: it is two fields and a button. */
    if (cp->editing) {
        int ascent = recon_font_ascent(cp->font);
        int field_w = (w - 12) / 2;

        if (!cp->editing_password_only) {
            recon_edit_draw(p, cp->font, x, y, field_w, FIELD_HEIGHT, &cp->name);
            if (!cp->password_focused) {
                recon_stroke_rect(p, x - 1, y - 1, field_w + 2, FIELD_HEIGHT + 2,
                    THEME(ACCENT));
            }
            recon_hit_add(p, x, y, field_w, FIELD_HEIGHT, HIT_FIELD_BASE);
        } else {
            recon_draw_text(p, cp->font, x, y + (FIELD_HEIGHT + ascent) / 2 - 2,
                field_w, cp->question_target, COLOR_TEXT);
        }

        recon_edit_draw(p, cp->font, x + field_w + 12, y, field_w,
            FIELD_HEIGHT, &cp->password);
        if (cp->password_focused) {
            recon_stroke_rect(p, x + field_w + 11, y - 1, field_w + 2,
                FIELD_HEIGHT + 2, THEME(ACCENT));
        }
        recon_hit_add(p, x + field_w + 12, y, field_w, FIELD_HEIGHT,
            HIT_FIELD_BASE + 1);

        y += FIELD_HEIGHT + 8;

        int bx = x;
        bx = draw_button(cp, p, bx, y,
            cp->editing_password_only ? "Set Password" : "Create",
            HIT_ACTION_BASE + (cp->editing_password_only
                ? ACTION_SET_PASSWORD : ACTION_ADD_USER), true);
        draw_button(cp, p, bx, y, "Cancel", HIT_ACTION_BASE + ACTION_NONE, true);
        return;
    }

    struct recon_user chosen;
    bool have = recon_users_at(cp->selected, &chosen);

    int bx = x;
    bx = draw_button(cp, p, bx, y, "Add Account",
        HIT_ACTION_BASE + ACTION_ADD_USER, admin);
    bx = draw_button(cp, p, bx, y, "Password",
        HIT_ACTION_BASE + ACTION_SET_PASSWORD, admin && have);
    bx = draw_button(cp, p, bx, y,
        (have && chosen.role == RECON_ROLE_ADMINISTRATOR)
            ? "Make Limited" : "Make Administrator",
        HIT_ACTION_BASE + ACTION_TOGGLE_ROLE, admin && have);
    draw_button(cp, p, bx, y, "Remove",
        HIT_ACTION_BASE + ACTION_REMOVE_USER, admin && have);
}

static void draw_appearance(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    y = draw_heading(cp, p, x, y, w, "Appearance",
        "Choose one to see it. Yours alone; other accounts keep theirs.");

    int count = recon_theme_count();
    int rows = (h - y - PADDING) / ROW_HEIGHT;
    if (rows > count) {
        rows = count;
    }

    cp->list_x = x;
    cp->list_y = y;
    cp->list_w = w;
    cp->list_h = rows * ROW_HEIGHT;

    recon_fill_rect(p, x, y, w, cp->list_h, COLOR_PANEL);

    const char *current = recon_theme_current();
    for (int i = 0; i < rows; i++) {
        struct recon_theme_info info;
        if (!recon_theme_at(i, &info)) {
            break;
        }
        draw_row(cp, p, x, y + i * ROW_HEIGHT, w, i, info.name,
            info.description, strcmp(info.name, current) == 0);
    }
}

static void draw_reading(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    (void)h;
    int ascent = recon_font_ascent(cp->font);
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    y = draw_heading(cp, p, x, y, w, "Reading",
        "How text is spaced and sized. Not a colour; see Appearance for that.");

    struct {
        const char *label;
        int value;
        enum action less;
        enum action more;
        const char *detail;
    } rows[] = {
        { "Space between letters", recon_text_letter_spacing(),
          ACTION_SPACING_LESS, ACTION_SPACING_MORE,
          "The best-supported adjustment for a dyslexic reader." },
        { "Space between lines", recon_text_line_spacing(),
          ACTION_LINES_LESS, ACTION_LINES_MORE, NULL },
        { "Text size",
          recon_registry_get_int(RECON_REG_USER, RECON_ACCESS_FONT_SIZE_KEY,
              RECON_ACCESS_FONT_SIZE_DEFAULT),
          ACTION_SIZE_LESS, ACTION_SIZE_MORE,
          "The chrome is laid out in fixed pixels, so a large size crowds it." },
    };

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        recon_draw_text(p, cp->font, x, y + ascent, w - 140, rows[i].label,
            COLOR_TEXT);

        char value[16];
        snprintf(value, sizeof(value), "%d", rows[i].value);

        int bx = x + w - 120;
        bx = draw_button(cp, p, bx, y - 4, "-",
            HIT_ACTION_BASE + rows[i].less, true);
        recon_draw_text(p, cp->font, bx + 6, y + ascent, 30, value, COLOR_TEXT);
        draw_button(cp, p, bx + 34, y - 4, "+",
            HIT_ACTION_BASE + rows[i].more, true);

        y += line + 6;
        if (rows[i].detail != NULL) {
            recon_draw_text(p, cp->font, x, y + ascent, w, rows[i].detail,
                COLOR_DIM);
            y += line;
        }
        y += 10;
    }

    const char *font = recon_registry_get(RECON_REG_USER,
        RECON_ACCESS_FONT_KEY, "");
    char summary[256];
    snprintf(summary, sizeof(summary), "Font: %s",
        *font != '\0' ? font : "the system's");
    recon_draw_text(p, cp->font, x, y + ascent, w, summary, COLOR_DIM);
    y += line + 4;

    recon_draw_text(p, cp->font, x, y + ascent, w,
        "Change it with 'access font <path>' in the terminal.", COLOR_DIM);
    y += line + 10;

    draw_button(cp, p, x, y, "Back to the defaults",
        HIT_ACTION_BASE + ACTION_RESET_READING, true);
}

static void draw_system(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    (void)h;
    int ascent = recon_font_ascent(cp->font);
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    y = draw_heading(cp, p, x, y, w, RECONOS_FULL_NAME,
        "Version " RECONOS_VERSION);

    struct recon_proc_snapshot *snapshot = recon_proc_snapshot_create();
    size_t total_mb = 0, used_mb = 0;
    if (snapshot != NULL && recon_proc_snapshot_refresh(snapshot)) {
        total_mb = recon_proc_total_memory_kb(snapshot) / 1024;
        used_mb = recon_proc_used_memory_kb(snapshot) / 1024;
    }
    recon_proc_snapshot_destroy(snapshot);

    char lines[8][160];
    int count = 0;

    snprintf(lines[count++], sizeof(lines[0]), "Filesystem      %s",
        recon_fs_host_root());
    snprintf(lines[count++], sizeof(lines[0]), "Signed in as    %s (%s)",
        recon_users_current() != NULL ? recon_users_current() : "nobody",
        recon_users_current_is_admin() ? "administrator" : "limited");
    snprintf(lines[count++], sizeof(lines[0]), "Accounts        %d",
        recon_users_count());
    snprintf(lines[count++], sizeof(lines[0]), "Memory          %zu of %zu MB",
        used_mb, total_mb);
    snprintf(lines[count++], sizeof(lines[0]), "Skin            %s",
        recon_theme_current());
    snprintf(lines[count++], sizeof(lines[0]), "Applications    %d installed",
        recon_installed_app_count());
    snprintf(lines[count++], sizeof(lines[0]), "Modules         %d loaded",
        recon_modules_count());

    for (int i = 0; i < count; i++) {
        recon_draw_text(p, cp->font, x, y + ascent, w, lines[i], COLOR_TEXT);
        y += line + 2;
    }

    y += 8;
    recon_draw_text(p, cp->font, x, y + ascent, w,
        "Accounts are enforced by ReconOS, inside ReconOS. A program running",
        COLOR_DIM);
    y += line;
    recon_draw_text(p, cp->font, x, y + ascent, w,
        "on the host underneath is not subject to them.", COLOR_DIM);
}

/* --- The window --- */

static void panel_draw(void *user, struct recon_panel *p,
        int x, int y, int w, int h) {
    struct control_panel *cp = user;
    int ascent = recon_font_ascent(cp->font);

    recon_fill_rect(p, x, y, w, h, COLOR_BG);

    /* The pages, down the left. */
    recon_fill_rect(p, x, y, SIDEBAR_WIDTH, h, COLOR_BG);
    recon_fill_rect(p, x + SIDEBAR_WIDTH - 1, y, 1, h, COLOR_SEPARATOR);

    for (int i = 0; i < PAGE_COUNT; i++) {
        int py = y + PADDING + i * (ROW_HEIGHT + 2);
        bool selected = (i == (int)cp->page);

        if (selected) {
            recon_fill_rect(p, x + 4, py, SIDEBAR_WIDTH - 10, ROW_HEIGHT,
                COLOR_SELECTED);
        }

        int label_x = x + 12;
        if (recon_icon_draw(p, PAGES[i].icon, x + 8, py + 4, ROW_HEIGHT - 8)) {
            label_x = x + 8 + (ROW_HEIGHT - 8) + 8;
        }
        recon_draw_text(p, cp->font, label_x, py + (ROW_HEIGHT + ascent) / 2 - 2,
            SIDEBAR_WIDTH - (label_x - x) - 8, PAGES[i].label,
            selected ? COLOR_SELECTED_TEXT : COLOR_TEXT);

        recon_hit_add(p, x + 4, py, SIDEBAR_WIDTH - 10, ROW_HEIGHT,
            HIT_PAGE_BASE + i);
    }

    /* Whichever page is chosen, to the right of it. */
    int cx = x + SIDEBAR_WIDTH + PADDING;
    int cy = y + PADDING;
    int cw = w - SIDEBAR_WIDTH - PADDING * 2;
    int chh = h - PADDING * 2 - STATUS_HEIGHT;

    switch (cp->page) {
    case PAGE_ACCOUNTS:   draw_accounts(cp, p, cx, cy, cw, chh); break;
    case PAGE_APPEARANCE: draw_appearance(cp, p, cx, cy, cw, chh); break;
    case PAGE_READING:    draw_reading(cp, p, cx, cy, cw, chh); break;
    default:              draw_system(cp, p, cx, cy, cw, chh); break;
    }

    /* Status, along the bottom. */
    int sy = y + h - STATUS_HEIGHT;
    recon_fill_rect(p, x, sy, w, STATUS_HEIGHT, COLOR_BG);
    recon_draw_text(p, cp->font, x + PADDING, sy + (STATUS_HEIGHT + ascent) / 2 - 2,
        w - PADDING * 2, cp->status,
        cp->status_is_warning ? COLOR_WARNING : COLOR_DIM);
}

/* --- Doing things --- */

static void begin_editing(struct control_panel *cp, bool password_only) {
    cp->editing = true;
    cp->editing_password_only = password_only;
    cp->password_focused = password_only;

    recon_edit_begin(&cp->name, "", false);
    recon_edit_begin(&cp->password, "", false);
    cp->password.masked = true;

    if (password_only) {
        struct recon_user user;
        if (recon_users_at(cp->selected, &user)) {
            snprintf(cp->question_target, sizeof(cp->question_target),
                "%s", user.name);
        }
        set_status(cp, false, "A new password for '%s'. Empty removes it.",
            cp->question_target);
    } else {
        set_status(cp, false, "A name, and a password if you want one.");
    }
}

static void stop_editing(struct control_panel *cp) {
    cp->editing = false;
    /* The typed password goes out of memory rather than sitting in the
     * window's state for the rest of the session. */
    recon_edit_end(&cp->name);
    recon_edit_end(&cp->password);
}

/* The answer to "remove this account?". */
static void answered(void *user, int choice) {
    struct control_panel *cp = user;

    char name[RECON_USERS_NAME_MAX];
    snprintf(name, sizeof(name), "%s", cp->question_target);
    cp->question = QUESTION_NONE;

    if (choice != 0) {
        set_status(cp, false, "Nothing was changed.");
    } else if (!recon_users_remove(name)) {
        set_status(cp, true, "%s", recon_users_last_error());
    } else {
        cp->selected = 0;
        set_status(cp, false, "Removed '%s'. Its files are still there.", name);
    }
    recon_appwin_refresh(cp->win);
}

static void do_action(struct control_panel *cp, enum action action) {
    struct recon_user chosen;
    bool have = recon_users_at(cp->selected, &chosen);

    switch (action) {
    case ACTION_NONE:
        stop_editing(cp);
        set_status(cp, false, "");
        break;

    case ACTION_ADD_USER:
        if (!cp->editing) {
            begin_editing(cp, false);
            break;
        }
        if (!recon_users_create(cp->name.text, cp->password.text,
                RECON_ROLE_LIMITED)) {
            set_status(cp, true, "%s", recon_users_last_error());
            break;
        }
        set_status(cp, false, "Created '%s' as a limited account.",
            cp->name.text);
        stop_editing(cp);
        break;

    case ACTION_SET_PASSWORD:
        if (!cp->editing) {
            if (have) {
                begin_editing(cp, true);
            }
            break;
        }
        if (!recon_users_set_password(cp->question_target,
                cp->password.length > 0 ? cp->password.text : NULL)) {
            set_status(cp, true, "%s", recon_users_last_error());
            break;
        }
        set_status(cp, false, "Password %s for '%s'.",
            cp->password.length > 0 ? "changed" : "removed",
            cp->question_target);
        stop_editing(cp);
        break;

    case ACTION_TOGGLE_ROLE: {
        if (!have) {
            break;
        }
        enum recon_user_role role =
            chosen.role == RECON_ROLE_ADMINISTRATOR
                ? RECON_ROLE_LIMITED : RECON_ROLE_ADMINISTRATOR;

        if (!recon_users_set_role(chosen.name, role)) {
            set_status(cp, true, "%s", recon_users_last_error());
            break;
        }
        set_status(cp, false, "'%s' is now %s.", chosen.name,
            role == RECON_ROLE_ADMINISTRATOR ? "an administrator" : "limited");
        break;
    }

    case ACTION_REMOVE_USER: {
        if (!have) {
            break;
        }
        cp->question = QUESTION_REMOVE_USER;
        snprintf(cp->question_target, sizeof(cp->question_target), "%s",
            chosen.name);

        char message[256];
        snprintf(message, sizeof(message),
            "Remove the account '%s'? Its files are kept.", chosen.name);

        /* Cancel last: it is what Enter and Escape both choose. */
        const char *buttons[2] = { "Remove", "Cancel" };
        recon_appwin_ask(cp->win, "Remove Account", message, buttons, 2,
            answered);
        break;
    }

    case ACTION_SPACING_LESS:
    case ACTION_SPACING_MORE: {
        int value = recon_text_letter_spacing() +
            (action == ACTION_SPACING_MORE ? 1 : -1);
        recon_registry_set_int(RECON_REG_USER, RECON_ACCESS_LETTER_KEY, value);
        recon_access_apply(cp->font);
        recon_shell_restyle(cp->server->shell);
        break;
    }

    case ACTION_LINES_LESS:
    case ACTION_LINES_MORE: {
        int value = recon_text_line_spacing() +
            (action == ACTION_LINES_MORE ? 2 : -2);
        recon_registry_set_int(RECON_REG_USER, RECON_ACCESS_LINE_KEY, value);
        recon_access_apply(cp->font);
        recon_shell_restyle(cp->server->shell);
        break;
    }

    case ACTION_SIZE_LESS:
    case ACTION_SIZE_MORE: {
        int value = recon_registry_get_int(RECON_REG_USER,
            RECON_ACCESS_FONT_SIZE_KEY, RECON_ACCESS_FONT_SIZE_DEFAULT) +
            (action == ACTION_SIZE_MORE ? 1 : -1);
        recon_registry_set_int(RECON_REG_USER, RECON_ACCESS_FONT_SIZE_KEY, value);
        recon_access_apply(cp->font);
        recon_shell_restyle(cp->server->shell);
        break;
    }

    case ACTION_RESET_READING:
        recon_registry_remove(RECON_REG_USER, RECON_ACCESS_LETTER_KEY);
        recon_registry_remove(RECON_REG_USER, RECON_ACCESS_LINE_KEY);
        recon_registry_remove(RECON_REG_USER, RECON_ACCESS_FONT_SIZE_KEY);
        recon_access_apply(cp->font);
        recon_shell_restyle(cp->server->shell);
        set_status(cp, false, "Back to the defaults.");
        break;
    }
}

static bool panel_click(void *user, uint32_t hit_id, int cx, int cy,
        bool pressed) {
    struct control_panel *cp = user;
    (void)cx;
    (void)cy;

    if (!pressed) {
        return false;
    }

    if (hit_id >= HIT_FIELD_BASE) {
        cp->password_focused = (hit_id > HIT_FIELD_BASE);
        return true;
    }
    if (hit_id >= HIT_ACTION_BASE) {
        do_action(cp, (enum action)(hit_id - HIT_ACTION_BASE));
        return true;
    }
    if (hit_id >= HIT_ROW_BASE) {
        int index = (int)(hit_id - HIT_ROW_BASE);

        if (cp->page == PAGE_APPEARANCE) {
            /* Choosing a skin shows it rather than describing it. */
            struct recon_theme_info info;
            if (recon_theme_at(index, &info) && recon_theme_set(info.name)) {
                recon_shell_restyle(cp->server->shell);
                set_status(cp, false, "Skin is now '%s'.", info.name);
            }
            return true;
        }

        cp->selected = index;
        return true;
    }
    if (hit_id >= HIT_PAGE_BASE) {
        int page = (int)(hit_id - HIT_PAGE_BASE);
        if (page >= 0 && page < PAGE_COUNT) {
            cp->page = (enum page)page;
            cp->selected = 0;
            stop_editing(cp);
            set_status(cp, false, "");
        }
        return true;
    }
    return true;
}

static bool panel_key(void *user, xkb_keysym_t sym, uint32_t modifiers) {
    struct control_panel *cp = user;

    if (cp->editing) {
        if (sym == XKB_KEY_Tab && !cp->editing_password_only) {
            cp->password_focused = !cp->password_focused;
            return true;
        }

        struct recon_edit *edit = cp->password_focused
            ? &cp->password : &cp->name;

        switch (recon_edit_key(edit, sym, modifiers)) {
        case RECON_EDIT_COMMIT:
            do_action(cp, cp->editing_password_only
                ? ACTION_SET_PASSWORD : ACTION_ADD_USER);
            return true;
        case RECON_EDIT_CANCEL:
            stop_editing(cp);
            set_status(cp, false, "");
            return true;
        case RECON_EDIT_CHANGED:
        case RECON_EDIT_IGNORED:
            return true;
        }
        return true;
    }

    switch (sym) {
    case XKB_KEY_Up:
        if (cp->selected > 0) {
            cp->selected--;
        }
        return true;
    case XKB_KEY_Down:
        cp->selected++;
        return true;
    default:
        return false;
    }
}

static void panel_describe(void *user, char *out, size_t size) {
    struct control_panel *cp = user;

    snprintf(out, size,
        "  page: %s\n"
        "  selected: %d\n"
        "  editing: %s\n"
        "  status: %s\n",
        PAGES[cp->page].label, cp->selected,
        cp->editing ? (cp->editing_password_only ? "password" : "new account")
                    : "no",
        cp->status);
}

static void panel_destroy(void *user) {
    free(user);
}

static const struct recon_appwin_impl CONTROL_PANEL_IMPL = {
    .title = "Control Panel",
    .icon = RECON_ICON_SYSTEM,
    .default_width = 660,
    .default_height = 440,
    .min_width = 520,
    .min_height = 320,
    .draw = panel_draw,
    .click = panel_click,
    .key = panel_key,
    .describe = panel_describe,
    .destroy = panel_destroy,
};

struct recon_appwin *recon_control_panel_create(struct recon_server *server,
        struct recon_font *font) {
    struct control_panel *cp = calloc(1, sizeof(*cp));
    if (cp == NULL) {
        return NULL;
    }

    cp->server = server;
    cp->font = font;
    cp->page = PAGE_ACCOUNTS;

    cp->win = recon_appwin_create(server, font, &CONTROL_PANEL_IMPL, cp);
    if (cp->win == NULL) {
        free(cp);
        return NULL;
    }
    return cp->win;
}
