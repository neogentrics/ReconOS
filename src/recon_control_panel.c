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
#include "recon_avatar.h"
#include "recon_control_panel.h"
#include "recon_fs.h"
#include "recon_icons.h"
#include "recon_modules.h"
#include "recon_net.h"
#include "recon_procinfo.h"
#include "recon_registry.h"
#include "recon_server.h"
#include "recon_shell.h"
#include "recon_theme.h"
#include "recon_ui.h"
#include "recon_users.h"
#include "recon_wallpaper.h"

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
#define HIT_PENDING_BASE (RECON_APPWIN_HIT_USER + 400)
#define HIT_AVATAR_BASE (RECON_APPWIN_HIT_USER + 500)
#define HIT_WALLPAPER_BASE (RECON_APPWIN_HIT_USER + 600)

/* What the buttons on each page do. */
enum action {
    ACTION_NONE,
    /* Accounts */
    ACTION_ADD_USER,
    ACTION_REMOVE_USER,
    ACTION_TOGGLE_ROLE,
    ACTION_SET_PASSWORD,
    ACTION_NEXT_AVATAR,
    ACTION_CHOOSE_AVATAR,
    /* Programs and modules */
    ACTION_INSTALL_PROGRAM,
    ACTION_REMOVE_PROGRAM,
    ACTION_CONFIRM_INSTALL,
    ACTION_LOAD_MODULE,
    ACTION_UNLOAD_MODULE,
    /* Registry */
    ACTION_UNLOCK_REGISTRY,
    ACTION_LOCK_REGISTRY,
    ACTION_REGISTRY_HIVE,
    ACTION_REGISTRY_EDIT,
    ACTION_REGISTRY_ADD,
    ACTION_REGISTRY_SAVE,
    ACTION_REGISTRY_CANCEL,
    ACTION_REGISTRY_REMOVE,
    /* Network */
    ACTION_TEST_NETWORK,
    ACTION_REFRESH_NETWORK,
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

    PAGE_PROGRAMS,
    PAGE_MODULES,

    PAGE_NETWORK,
    PAGE_POWER,
    PAGE_STORAGE,
    PAGE_MULTITASKING,
    PAGE_UPDATE,

    PAGE_TROUBLESHOOT,
    PAGE_RECOVERY,
    PAGE_REGISTRY,

    PAGE_ABOUT,
    PAGE_COUNT,
};

static const struct {
    const char *label;
    const char *icon;
    bool starts_group;   /* Draw a dividing rule above this one. */
} PAGES[PAGE_COUNT] = {
    { "Accounts", RECON_ICON_APP, false },
    { "Appearance", RECON_ICON_SYSTEM, false },
    { "Reading", RECON_ICON_NOTEPAD, false },

    { "Programs", RECON_ICON_APP, true },
    { "Modules", RECON_ICON_SYSTEM, false },

    { "Network", RECON_ICON_SYSTEM, true },
    { "Power", RECON_ICON_SHUTDOWN, false },
    { "Storage", RECON_ICON_EXPLORER, false },
    { "Multitasking", RECON_ICON_TASKMGR, false },
    { "Update", RECON_ICON_SYSTEM, false },

    { "Troubleshoot", RECON_ICON_TERMINAL, true },
    { "Recovery", RECON_ICON_SYSTEM, false },
    { "Registry", RECON_ICON_NOTEPAD, false },

    { "About", RECON_ICON_TASKMGR, true },
};

/*
 * --- Settings that are not built yet ---
 *
 * These pages are here on purpose, empty of function and honest about it.
 *
 * The argument for leaving them out was that a button which does nothing is
 * worse than no button. The argument against, which won, is that a gap is
 * worse still: nobody can see a gap, so nobody remembers it was meant to be
 * filled, and the shape of the system stops being visible to the person
 * building it. A control that says plainly what it will do and why it cannot
 * yet is a note to ourselves that is also useful to a reader.
 *
 * Every one of these says what is missing rather than "coming soon". The
 * missing thing is usually the kernel: a hosted process cannot suspend a
 * machine, partition a disk, or reinstall itself.
 */
struct pending_item {
    const char *label;
    const char *detail;   /* What it is for. */
    const char *blocked;  /* What has to exist first. */
};

static const struct pending_item POWER_ITEMS[] = {
    { "Sleep", "Stop everything and hold it in memory.",
      "Suspending needs a kernel underneath. ReconOS is a process on one." },
    { "Hibernate", "Write memory to disk and switch off.",
      "Needs somewhere on disk to write an image to, and a boot path that "
      "reads it back." },
    { "Screen blanks after", "Turn the display off when nothing is happening.",
      "Needs an idle timer and control of the display's power state." },
    { "Power mode", "Trade speed against battery.",
      "Nothing to trade yet: no governor, and no battery to read." },
};

static const struct pending_item STORAGE_ITEMS[] = {
    { "Volumes", "The disks and partitions the system can see.",
      "ReconOS has one filesystem, hosted in a folder. There is no volume "
      "layer to list." },
    { "Where new files go", "Which volume documents and programs are put on.",
      "Needs more than one volume for the choice to mean anything." },
    { "Clean up", "Find what is taking room and offer to remove it.",
      "Needs the Recycle Bin and temporary files accounted for by size." },
    { "Format and partition", "Prepare a disk for use.",
      "Writing partition tables is a kernel's job, and would be dangerous "
      "from here." },
};

static const struct pending_item MULTITASKING_ITEMS[] = {
    { "Title bar buttons", "Which buttons a window's title bar carries.",
      "Real, but not settable: they are drawn from a fixed set." },
};

static const struct pending_item UPDATE_ITEMS[] = {
    { "Check for updates", "Ask whether a newer ReconOS exists.",
      "Nothing to ask. There is no update server and no signed package "
      "format to trust an answer from." },
    { "What changed", "The history of what each version did.",
      "The history exists in the repository; nothing brings it into the "
      "system." },
    { "Install automatically", "Apply updates without being asked.",
      "Would need updates first, and a way to restart into them." },
};

static const struct pending_item TROUBLESHOOT_ITEMS[] = {
    { "A program stopped answering", "Find it and close it safely.",
      "Partly real already: the Task Manager finds these and can end them. "
      "This page would be the guided version." },
    { "Something looks wrong on screen", "Redraw everything from scratch.",
      "Needs a way to force a full repaint and to reload the theme and "
      "icons together." },
    { "Report a problem", "Collect what happened into one file.",
      "Needs a log the system keeps rather than one the terminal prints." },
};

static const struct pending_item RECOVERY_ITEMS[] = {
    { "Restore points", "Save the system's state, and go back to it.",
      "Needs a copy of /System and the registry taken on a schedule, and "
      "somewhere to keep it." },
    { "Back up", "Copy what matters somewhere else.",
      "Needs a second volume to copy to. There is one filesystem." },
    { "Advanced startup", "Start into a screen for repairing the system.",
      "Needs a boot path of our own. ReconOS is started by whatever is "
      "underneath it." },
    { "Reset this system", "Put it back the way it was installed.",
      "Needs a copy of the original to put back, kept somewhere a reset "
      "cannot reach." },
    { "Reinstall", "Lay the system down again, keeping accounts and files.",
      "Needs an installer that runs from inside a running system." },
};

/* One table, so a page and its items cannot be wired up separately and drift.
 * A page with no items is one that does something real. */
static const struct {
    const char *lede;
    const struct pending_item *items;
    int count;
} PENDING[PAGE_COUNT] = {
    [PAGE_POWER] = { "What the machine does when it is left alone.",
        POWER_ITEMS, (int)(sizeof(POWER_ITEMS) / sizeof(POWER_ITEMS[0])) },
    [PAGE_STORAGE] = { "Where things are kept, and how much room is left.",
        STORAGE_ITEMS, (int)(sizeof(STORAGE_ITEMS) / sizeof(STORAGE_ITEMS[0])) },
    [PAGE_MULTITASKING] = { "How windows and desktops behave together.",
        MULTITASKING_ITEMS,
        (int)(sizeof(MULTITASKING_ITEMS) / sizeof(MULTITASKING_ITEMS[0])) },
    [PAGE_UPDATE] = { "Keeping ReconOS current.",
        UPDATE_ITEMS, (int)(sizeof(UPDATE_ITEMS) / sizeof(UPDATE_ITEMS[0])) },
    [PAGE_TROUBLESHOOT] = { "When something is not working.",
        TROUBLESHOOT_ITEMS,
        (int)(sizeof(TROUBLESHOOT_ITEMS) / sizeof(TROUBLESHOOT_ITEMS[0])) },
    [PAGE_RECOVERY] = { "Getting back to a system that worked.",
        RECOVERY_ITEMS,
        (int)(sizeof(RECOVERY_ITEMS) / sizeof(RECOVERY_ITEMS[0])) },
};

/* What a pending question is about, since the answer arrives later. */
enum question {
    QUESTION_NONE,
    QUESTION_REMOVE_USER,
    QUESTION_REMOVE_PROGRAM,
    QUESTION_REMOVE_KEY,
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
    /*
     * Long enough for the longest thing it carries, which is a registry key.
     * It used to be an account name's length and silently truncated module
     * names, which the compiler had been pointing out for a while.
     */
    char question_target[RECON_REGISTRY_KEY_MAX];

    /*
     * The registry page is locked until an administrator says who they are.
     *
     * Not because the values are secret -- they are a text file anyone with
     * the disk can read -- but because this is the one page where a careless
     * change breaks the system quietly, and asking for the password makes it
     * a deliberate act rather than a click made while looking for something
     * else. The unlock does not outlive the window.
     */
    /* Choosing a picture: the set is laid out over the account list until one
     * is picked. A grid you look at, rather than a button you click until the
     * right one comes round. */
    bool picking_avatar;

    /* Typing the path of something to install. */
    bool installing;

    bool registry_unlocked;
    struct recon_edit unlock;
    int registry_hive;    /* 0 system, 1 user */
    int registry_scroll;

    /*
     * Changing a setting, or adding one.
     *
     * The key being changed is copied out rather than read from the list on
     * the way past: adding or removing anything renumbers the hive, so an
     * index captured when the field opened would not still mean the same key
     * when Save is pressed.
     */
    bool registry_editing;
    bool registry_adding;
    char registry_key[RECON_REGISTRY_KEY_MAX];
    struct recon_edit reg_key;
    struct recon_edit reg_value;
    bool reg_key_focused;

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

/*
 * Nothing to say.
 *
 * Its own function rather than set_status(cp, false, "") because set_status
 * carries a printf format attribute, and an empty format is a warning at
 * every call site -- six of them, which is enough noise to hide a real one.
 */
static void clear_status(struct control_panel *cp) {
    cp->status[0] = '\0';
    cp->status_is_warning = false;
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

/*
 * A row of a list, with an optional second column, and the text starting
 * `indent` from the left edge so something can be drawn in front of it.
 */
static void draw_row_at(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int indent, int index, const char *label,
        const char *detail, bool selected) {
    int ascent = recon_font_ascent(cp->font);

    if (selected) {
        recon_fill_rect(p, x, y, w, ROW_HEIGHT, COLOR_SELECTED);
    } else if (index % 2 == 1) {
        recon_fill_rect(p, x, y, w, ROW_HEIGHT, COLOR_ROW_ALT);
    }

    recon_color ink = selected ? COLOR_SELECTED_TEXT : COLOR_TEXT;
    recon_color faint = selected ? COLOR_SELECTED_TEXT : COLOR_DIM;

    recon_draw_text(p, cp->font, x + indent, y + (ROW_HEIGHT + ascent) / 2 - 2,
        w / 2, label, ink);
    if (detail != NULL) {
        recon_draw_text(p, cp->font, x + w / 2, y + (ROW_HEIGHT + ascent) / 2 - 2,
            w / 2 - 10, detail, faint);
    }

    recon_hit_add(p, x, y, w, ROW_HEIGHT, HIT_ROW_BASE + index);
}

static void draw_row(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int index, const char *label, const char *detail,
        bool selected) {
    draw_row_at(cp, p, x, y, w, 10, index, label, detail, selected);
}

/* --- The pages --- */

/*
 * The pictures, laid out as a grid to choose from.
 *
 * The first tile is "no picture", which puts the account back to a coloured
 * disc with its initial. That has to be reachable or a choice made once could
 * never be undone.
 */
#define AVATAR_TILE 56
#define AVATAR_FACE 40

static void draw_avatar_picker(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    int ascent = recon_font_ascent(cp->font);

    /* Sized for what goes in it. question_target now holds a registry key,
     * which is longer than an account name, and this heading was the one
     * place that silently shortened it. */
    char title[RECON_REGISTRY_KEY_MAX + 32];
    snprintf(title, sizeof(title), "A picture for %s", cp->question_target);
    y = draw_heading(cp, p, x, y, w, title,
        "Or drop a file called avatar-something into /System/Icons.");

    int columns = w / AVATAR_TILE;
    if (columns < 1) {
        columns = 1;
    }

    int total = recon_avatar_count() + 1;   /* the first is "none" */
    for (int i = 0; i < total; i++) {
        int tx = x + (i % columns) * AVATAR_TILE;
        int ty = y + (i / columns) * AVATAR_TILE;
        if (ty + AVATAR_TILE > y + h) {
            break;
        }

        char name[64] = "";
        if (i > 0 && !recon_avatar_at(i - 1, name, sizeof(name))) {
            continue;
        }

        const char *current = recon_avatar_of(cp->question_target);
        bool chosen = strcmp(current, name) == 0;
        if (chosen) {
            recon_fill_rect(p, tx, ty, AVATAR_TILE - 4, AVATAR_TILE - 4,
                COLOR_SELECTED);
        }

        int fx = tx + (AVATAR_TILE - 4 - AVATAR_FACE) / 2;
        int fy = ty + 4;

        if (i == 0) {
            /* The account's own initial, which is what "none" means. */
            recon_avatar_draw(p, cp->font, cp->question_target, fx, fy,
                AVATAR_FACE);
            recon_draw_text(p, cp->font, tx + 8, fy + AVATAR_FACE + ascent + 2,
                AVATAR_TILE - 12, "None",
                chosen ? COLOR_SELECTED_TEXT : COLOR_DIM);
        } else if (!recon_icon_draw(p, name, fx, fy, AVATAR_FACE)) {
            continue;
        }

        recon_hit_add(p, tx, ty, AVATAR_TILE - 4, AVATAR_TILE - 4,
            HIT_AVATAR_BASE + i);
    }

    int rows = (total + columns - 1) / columns;
    draw_button(cp, p, x, y + rows * AVATAR_TILE + PADDING, "Done",
        HIT_ACTION_BASE + ACTION_CHOOSE_AVATAR, true);
}

static void draw_accounts(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    bool admin = recon_users_may_administer();

    if (cp->picking_avatar) {
        draw_avatar_picker(cp, p, x, y, w, h);
        return;
    }

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

        int ry = y + i * ROW_HEIGHT;

        /* The row first, then the picture over it, so the highlight does not
         * paint across the face of the account you have selected. */
        draw_row_at(cp, p, x, ry, w, ROW_HEIGHT + 6, i, user.name, detail,
            i == cp->selected);
        recon_avatar_draw(p, cp->font, user.name, x + 4, ry + 2,
            ROW_HEIGHT - 4);
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
    /*
     * Changing a picture is not administration: it is somebody deciding what
     * they look like. So an account may change its own without being an
     * administrator, which is the one thing on this page that is true of.
     */
    bool own = have && recon_users_current() != NULL &&
        strcmp(recon_users_current(), chosen.name) == 0;
    bx = draw_button(cp, p, bx, y, "Picture",
        HIT_ACTION_BASE + ACTION_NEXT_AVATAR, have && (admin || own));
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
    int ascent = recon_font_ascent(cp->font);

    for (int i = 0; i < rows; i++) {
        struct recon_theme_info info;
        if (!recon_theme_at(i, &info)) {
            break;
        }

        int ry = y + i * ROW_HEIGHT;
        bool selected = strcmp(info.name, current) == 0;

        /*
         * Each row is drawn in the colours of the skin it offers, not in the
         * ones currently on screen. A list of skins painted entirely in the
         * present skin shows nothing about any of the others -- and since
         * several of these share a selection blue, half of them looked
         * identical. Now the row is a sample of what choosing it does.
         */
        recon_color row_bg = recon_theme_color_of(i, RECON_THEME_SELECTION);
        recon_color row_ink = recon_theme_color_of(i, RECON_THEME_SELECTION_TEXT);

        if (selected) {
            recon_fill_rect(p, x, ry, w, ROW_HEIGHT, row_bg);
        } else {
            /* Unselected rows keep the surface, with a swatch of the skin's
             * own selection colour down the left edge: the whole list in nine
             * different backgrounds would be a mess to read. */
            if (i % 2 == 1) {
                recon_fill_rect(p, x, ry, w, ROW_HEIGHT, COLOR_ROW_ALT);
            }
            recon_fill_rect(p, x, ry + 3, 5, ROW_HEIGHT - 6, row_bg);
        }

        recon_color ink = selected ? row_ink : COLOR_TEXT;
        recon_color faint = selected ? row_ink : COLOR_DIM;

        recon_draw_text(p, cp->font, x + 14, ry + (ROW_HEIGHT + ascent) / 2 - 2,
            w / 2, info.name, ink);
        recon_draw_text(p, cp->font, x + w / 2, ry + (ROW_HEIGHT + ascent) / 2 - 2,
            w / 2 - 10, info.description, faint);

        recon_hit_add(p, x, ry, w, ROW_HEIGHT, HIT_ROW_BASE + i);
    }

    /*
     * And the wallpaper, under the skins, because it is the other half of how
     * the desktop looks. Choosing a skin puts its wallpaper on; choosing a
     * wallpaper here keeps it until the skin changes again.
     */
    y += cp->list_h + PADDING;

    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    recon_draw_text(p, cp->font, x, y + ascent, w, "Wallpaper", COLOR_TEXT);
    y += line + 4;

    int papers = recon_wallpaper_count();
    int room = (h - y - PADDING) / ROW_HEIGHT;
    if (room < 0) {
        room = 0;
    }

    const char *showing = recon_wallpaper_current();

    for (int i = 0; i < papers && i < room; i++) {
        char name[96];
        if (!recon_wallpaper_at(i, name, sizeof(name))) {
            break;
        }

        int ry = y + i * ROW_HEIGHT;
        bool chosen = strcmp(name, showing) == 0;

        if (chosen) {
            recon_fill_rect(p, x, ry, w, ROW_HEIGHT, COLOR_SELECTED);
        } else if (i % 2 == 1) {
            recon_fill_rect(p, x, ry, w, ROW_HEIGHT, COLOR_ROW_ALT);
        }

        recon_draw_text(p, cp->font, x + 14, ry + (ROW_HEIGHT + ascent) / 2 - 2,
            w - 28, name, chosen ? COLOR_SELECTED_TEXT : COLOR_TEXT);

        recon_hit_add(p, x, ry, w, ROW_HEIGHT, HIT_WALLPAPER_BASE + i);
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

/*
 * A page of settings that do not work yet.
 *
 * Each one says what it is for, and clicking it says what has to exist first.
 * The tag on the right is deliberately plain: this is a note about the state
 * of the system, not a feature being advertised.
 */
static void draw_pending_page(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h, enum page page) {
    (void)h;
    int ascent = recon_font_ascent(cp->font);
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    y = draw_heading(cp, p, x, y, w, PAGES[page].label, PENDING[page].lede);

    const char *tag = "Not built yet";
    int tag_w = recon_text_width(cp->font, tag);

    for (int i = 0; i < PENDING[page].count; i++) {
        const struct pending_item *item = &PENDING[page].items[i];
        int row_h = line * 2 + 8;

        if (i % 2 == 1) {
            recon_fill_rect(p, x, y, w, row_h, COLOR_ROW_ALT);
        }

        recon_draw_text(p, cp->font, x + 8, y + 4 + ascent,
            w - tag_w - 30, item->label, COLOR_TEXT);
        recon_draw_text(p, cp->font, x + 8, y + 4 + line + ascent,
            w - 20, item->detail, COLOR_DIM);

        recon_draw_text(p, cp->font, x + w - tag_w - 10, y + 4 + ascent,
            tag_w + 4, tag, COLOR_DIM);

        recon_hit_add(p, x, y, w, row_h, HIT_PENDING_BASE + i);
        y += row_h + 2;
    }
}

/* What is installed, and what could be done about it. */
static void draw_programs(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    y = draw_heading(cp, p, x, y, w, "Programs",
        "What is installed. Applications come from modules in /Apps.");

    int count = recon_installed_app_count();
    int rows = (h - y - BUTTON_HEIGHT - PADDING * 3) / ROW_HEIGHT;
    if (rows < 1) {
        rows = 1;
    }
    if (rows > count) {
        rows = count;
    }

    cp->list_x = x;
    cp->list_y = y;
    cp->list_w = w;
    cp->list_h = rows * ROW_HEIGHT;
    recon_fill_rect(p, x, y, w, cp->list_h, COLOR_PANEL);

    for (int i = 0; i < rows; i++) {
        struct recon_installed_app app;
        if (!recon_installed_app_at(i, &app)) {
            break;
        }
        draw_row(cp, p, x, y + i * ROW_HEIGHT, w, i, app.name,
            app.module[0] != '\0' ? app.module : "built into ReconOS",
            i == cp->selected);
    }

    y += cp->list_h + PADDING;

    bool admin = recon_users_may_administer();

    /*
     * Installing is typing a path. A module runs inside ReconOS with
     * everything ReconOS can do, so it is an administrator's decision --
     * closer to installing a driver than to saving a file.
     */
    if (cp->installing) {
        recon_edit_draw(p, cp->font, x, y, w - 4, FIELD_HEIGHT, &cp->name);
        recon_hit_add(p, x, y, w - 4, FIELD_HEIGHT, HIT_FIELD_BASE);
        y += FIELD_HEIGHT + 8;

        int bx = draw_button(cp, p, x, y, "Install",
            HIT_ACTION_BASE + ACTION_CONFIRM_INSTALL, true);
        draw_button(cp, p, bx, y, "Cancel",
            HIT_ACTION_BASE + ACTION_NONE, true);
        return;
    }

    struct recon_installed_app chosen;
    bool have = recon_installed_app_at(cp->selected, &chosen);
    bool removable = have && chosen.module[0] != '\0';

    int bx = draw_button(cp, p, x, y, "Install a Program",
        HIT_ACTION_BASE + ACTION_INSTALL_PROGRAM, admin);
    draw_button(cp, p, bx, y, "Remove",
        HIT_ACTION_BASE + ACTION_REMOVE_PROGRAM, admin && removable);

    y += BUTTON_HEIGHT + PADDING;

    int ascent = recon_font_ascent(cp->font);
    recon_draw_text(p, cp->font, x, y + ascent, w,
        have && !removable
            ? "That one is part of ReconOS, so it cannot be removed."
            : "Programs are .rex files. Installing copies one into /Apps.",
        COLOR_DIM);
}

/* Modules: the ones that loaded, and the ones that would not. */
static void draw_modules(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    y = draw_heading(cp, p, x, y, w, "Modules",
        "Code loaded into the running system: .rts for the system, .rex for "
        "an application.");

    int count = recon_modules_count();
    int rows = (h - y - BUTTON_HEIGHT - PADDING * 3) / ROW_HEIGHT;
    if (rows < 1) {
        rows = 1;
    }
    if (rows > count) {
        rows = count;
    }

    cp->list_x = x;
    cp->list_y = y;
    cp->list_w = w;
    cp->list_h = rows * ROW_HEIGHT;
    recon_fill_rect(p, x, y, w, cp->list_h, COLOR_PANEL);

    if (count == 0) {
        int ascent = recon_font_ascent(cp->font);
        recon_draw_text(p, cp->font, x + 10, y + (ROW_HEIGHT + ascent) / 2 - 2,
            w - 20, "Nothing is loaded.", COLOR_DIM);
    }

    for (int i = 0; i < rows; i++) {
        struct recon_module_state module;
        if (!recon_modules_at(i, &module)) {
            break;
        }

        /* A module that refused to load says why, in the place where a
         * working one says what it is. A silent failure is the thing a
         * module list exists to prevent. */
        /* Long enough for the longer of the two things it shows: the reason
         * a module refused to load, which is a sentence rather than a
         * version number. */
        char detail[RECON_MODULES_PROBLEM_MAX + 32];
        snprintf(detail, sizeof(detail), "%s   %s",
            module.is_app ? "application" : "system",
            module.loaded ? module.version : module.problem);

        draw_row(cp, p, x, y + i * ROW_HEIGHT, w, i, module.name, detail,
            i == cp->selected);
    }

    y += cp->list_h + PADDING;

    struct recon_module_state chosen;
    bool have = recon_modules_at(cp->selected, &chosen);
    bool admin = recon_users_may_administer();

    int bx = draw_button(cp, p, x, y, "Load from /Apps",
        HIT_ACTION_BASE + ACTION_LOAD_MODULE, admin);
    draw_button(cp, p, bx, y, "Unload",
        HIT_ACTION_BASE + ACTION_UNLOAD_MODULE,
        admin && have && chosen.loaded);
}

/*
 * The network, as ReconOS can see it.
 *
 * Everything here is read from the host, and the page says so at the bottom
 * rather than letting a list of addresses imply that ReconOS is doing the
 * networking. It is not; see include/recon_net.h.
 */
static void draw_network(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    int ascent = recon_font_ascent(cp->font);
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    /* Read again every time the page is drawn: an address that was true when
     * the window opened is not a fact, it is a memory. */
    recon_net_refresh();

    bool online = recon_net_online();
    y = draw_heading(cp, p, x, y, w, "Network",
        online ? "This machine has a way out."
               : "This machine has no way out at the moment.");

    /* The machine's own name, which is ReconOS's rather than the host's. */
    char summary[192];
    snprintf(summary, sizeof(summary), "Called          %s",
        recon_net_machine_name());
    recon_draw_text(p, cp->font, x, y + ascent, w, summary, COLOR_TEXT);
    y += line + 6;

    /* Interfaces. Loopback last and dimmed: it is real, and it is never the
     * answer to "am I connected". */
    int count = recon_net_interface_count();
    int rows = (h - y - BUTTON_HEIGHT - PADDING * 3) / ROW_HEIGHT;
    if (rows < 1) {
        rows = 1;
    }

    cp->list_x = x;
    cp->list_y = y;
    cp->list_w = w;
    cp->list_h = (rows < count ? rows : count) * ROW_HEIGHT;
    if (cp->list_h > 0) {
        recon_fill_rect(p, x, y, w, cp->list_h, COLOR_PANEL);
    }

    int drawn = 0;
    for (int pass = 0; pass < 2 && drawn < rows; pass++) {
        for (int i = 0; i < count && drawn < rows; i++) {
            struct recon_net_interface interface;
            if (!recon_net_interface_at(i, &interface)) {
                continue;
            }
            /* Real interfaces first, loopback second. */
            if (interface.loopback != (pass == 1)) {
                continue;
            }

            char detail[128];
            snprintf(detail, sizeof(detail), "%s%s%s",
                interface.address[0] != '\0' ? interface.address
                                             : "no address",
                interface.up ? "" : "   down",
                interface.loopback ? "   this machine only"
                    : (interface.wireless ? "   wireless" : ""));

            draw_row(cp, p, x, y + drawn * ROW_HEIGHT, w, drawn,
                interface.name, detail, false);
            drawn++;
        }
    }

    if (count == 0) {
        recon_draw_text(p, cp->font, x + 10, y + (ROW_HEIGHT + ascent) / 2 - 2,
            w - 20, "No interfaces at all.", COLOR_DIM);
    }

    y += cp->list_h + PADDING;

    const char *gateway = recon_net_gateway();
    snprintf(summary, sizeof(summary), "Gateway         %s",
        gateway[0] != '\0' ? gateway : "none");
    recon_draw_text(p, cp->font, x, y + ascent, w, summary, COLOR_TEXT);
    y += line + 2;

    int servers = recon_net_nameserver_count();
    for (int i = 0; i < servers; i++) {
        snprintf(summary, sizeof(summary), "%-15s %s",
            i == 0 ? "Resolver" : "", recon_net_nameserver_at(i));
        recon_draw_text(p, cp->font, x, y + ascent, w, summary, COLOR_TEXT);
        y += line + 2;
    }
    if (servers == 0) {
        recon_draw_text(p, cp->font, x, y + ascent, w,
            "Resolver        none configured", COLOR_TEXT);
        y += line + 2;
    }

    /* The last test, if one has been run. */
    char host[128];
    enum recon_net_result result;
    int elapsed = 0;
    if (recon_net_last_probe(host, sizeof(host), &result, &elapsed)) {
        if (result == RECON_NET_OK) {
            snprintf(summary, sizeof(summary), "Last test       %s answered "
                "in %d ms", host, elapsed);
        } else {
            snprintf(summary, sizeof(summary), "Last test       %s: %s",
                host, recon_net_result_name(result));
        }
        recon_draw_text(p, cp->font, x, y + ascent, w, summary,
            result == RECON_NET_OK ? COLOR_TEXT : COLOR_WARNING);
        y += line + 2;
    }

    if (recon_net_probe_count() > 0) {
        recon_draw_text(p, cp->font, x, y + ascent, w,
            "Testing...", COLOR_DIM);
        y += line + 2;
    }

    y += PADDING;
    int bx = draw_button(cp, p, x, y, "Test the connection",
        HIT_ACTION_BASE + ACTION_TEST_NETWORK, true);
    draw_button(cp, p, bx, y, "Read again",
        HIT_ACTION_BASE + ACTION_REFRESH_NETWORK, true);
    y += BUTTON_HEIGHT + PADDING;

    recon_draw_text(p, cp->font, x, y + ascent, w,
        "ReconOS has no network stack of its own. This is the host's,",
        COLOR_DIM);
    y += line;
    recon_draw_text(p, cp->font, x, y + ascent, w,
        "reported through ReconOS. Its own comes with its own kernel.",
        COLOR_DIM);
}

/*
 * The registry, behind a password.
 *
 * Read-only for now: seeing what the system remembers is most of the value,
 * and an editor here would be the fastest way yet built to make a system
 * unbootable. The terminal's 'reg' command still changes things for anyone
 * who means to.
 */
static void draw_registry(struct control_panel *cp, struct recon_panel *p,
        int x, int y, int w, int h) {
    int ascent = recon_font_ascent(cp->font);
    int line = recon_font_line_height(cp->font);
    if (line <= 0) {
        line = 18;
    }

    y = draw_heading(cp, p, x, y, w, "Registry",
        "Everything the system remembers between runs.");

    if (!cp->registry_unlocked) {
        recon_draw_text(p, cp->font, x, y + ascent, w,
            "An administrator's password is needed to look at this.",
            COLOR_TEXT);
        y += line + 4;
        recon_draw_text(p, cp->font, x, y + ascent, w,
            "Not because it is secret -- it is a text file on disk -- but "
            "because a", COLOR_DIM);
        y += line;
        recon_draw_text(p, cp->font, x, y + ascent, w,
            "careless change here breaks the system quietly.", COLOR_DIM);
        y += line + PADDING;

        const char *who = recon_users_current();
        char label[128];
        snprintf(label, sizeof(label), "Password for %s",
            who != NULL ? who : "the administrator");
        recon_draw_text(p, cp->font, x, y + ascent, w, label, COLOR_DIM);
        y += line + 2;

        recon_edit_draw(p, cp->font, x, y, 240, FIELD_HEIGHT, &cp->unlock);
        recon_hit_add(p, x, y, 240, FIELD_HEIGHT, HIT_FIELD_BASE + 1);
        y += FIELD_HEIGHT + 8;

        draw_button(cp, p, x, y, "Unlock",
            HIT_ACTION_BASE + ACTION_UNLOCK_REGISTRY,
            recon_users_may_administer());
        return;
    }

    /* Which hive, and how many keys are in it. */
    int hive = cp->registry_hive;
    enum recon_registry_scope scope =
        hive == 0 ? RECON_REG_SYSTEM : RECON_REG_USER;

    int bx = draw_button(cp, p, x, y,
        hive == 0 ? "Showing: the machine's" : "Showing: this account's",
        HIT_ACTION_BASE + ACTION_REGISTRY_HIVE, true);
    draw_button(cp, p, bx, y, "Lock",
        HIT_ACTION_BASE + ACTION_LOCK_REGISTRY, true);
    y += BUTTON_HEIGHT + PADDING;

    int count = recon_registry_count(scope, "");

    /*
     * Room kept below the list for the buttons, and for the fields when one
     * of them is open. Taken off before the rows are counted rather than
     * after, so the list never draws over its own controls -- which it would
     * do on a short window, and the controls are the half that changes
     * things.
     */
    int reserved = BUTTON_HEIGHT + PADDING * 2;
    if (cp->registry_editing || cp->registry_adding) {
        reserved += FIELD_HEIGHT + 8;
    }
    if (cp->registry_adding) {
        reserved += FIELD_HEIGHT + 8;
    }

    int rows = (h - y - reserved) / ROW_HEIGHT;
    if (rows < 1) {
        rows = 1;
    }

    cp->list_x = x;
    cp->list_y = y;
    cp->list_w = w;
    cp->list_h = rows * ROW_HEIGHT;
    recon_fill_rect(p, x, y, w, cp->list_h, COLOR_PANEL);

    if (cp->selected >= count) {
        cp->selected = count - 1;
    }
    if (cp->selected < 0) {
        cp->selected = 0;
    }

    /* Follow the selection, so a key chosen and then scrolled past does not
     * leave the buttons acting on something nobody can see. */
    if (cp->selected < cp->registry_scroll) {
        cp->registry_scroll = cp->selected;
    }
    if (cp->selected >= cp->registry_scroll + rows) {
        cp->registry_scroll = cp->selected - rows + 1;
    }
    if (cp->registry_scroll > count - rows) {
        cp->registry_scroll = count - rows;
    }
    if (cp->registry_scroll < 0) {
        cp->registry_scroll = 0;
    }

    for (int i = 0; i < rows && cp->registry_scroll + i < count; i++) {
        const char *key = NULL;
        const char *value = NULL;
        if (!recon_registry_at(scope, "", cp->registry_scroll + i,
                &key, &value)) {
            break;
        }

        int ry = y + i * ROW_HEIGHT;
        bool chosen = (cp->registry_scroll + i == cp->selected);

        if (chosen) {
            recon_fill_role(p, x, ry, w, ROW_HEIGHT, RECON_THEME_SELECTION);
        } else if (i % 2 == 1) {
            recon_fill_rect(p, x, ry, w, ROW_HEIGHT, COLOR_ROW_ALT);
        }

        recon_draw_text(p, cp->font, x + 10, ry + (ROW_HEIGHT + ascent) / 2 - 2,
            w / 2, key != NULL ? key : "",
            chosen ? THEME(SELECTION_TEXT) : COLOR_TEXT);
        recon_draw_text(p, cp->font, x + w / 2, ry + (ROW_HEIGHT + ascent) / 2 - 2,
            w / 2 - 10, value != NULL ? value : "",
            chosen ? THEME(SELECTION_TEXT) : COLOR_DIM);

        recon_hit_add(p, x, ry, w, ROW_HEIGHT, HIT_ROW_BASE + i);
    }

    if (count == 0) {
        recon_draw_text(p, cp->font, x + 10, y + (ROW_HEIGHT + ascent) / 2 - 2,
            w - 20, "This hive is empty.", COLOR_DIM);
    }

    y += cp->list_h + PADDING;

    /*
     * Adding takes two fields, changing takes one.
     *
     * The key is not editable when changing an existing setting: a key that
     * can be typed over is a rename, and a rename here is a delete and an add
     * that look like one act -- which is how somebody ends up with the old
     * key still in the file and no idea it is there.
     */
    if (cp->registry_adding) {
        recon_draw_text(p, cp->font, x, y + ascent, w,
            "Key, then value. Keys are paths and cannot contain spaces.",
            COLOR_DIM);
        y += line;

        recon_edit_draw(p, cp->font, x, y, w - 4, FIELD_HEIGHT, &cp->reg_key);
        recon_hit_add(p, x, y, w - 4, FIELD_HEIGHT, HIT_FIELD_BASE + 2);
        y += FIELD_HEIGHT + 8;

        recon_edit_draw(p, cp->font, x, y, w - 4, FIELD_HEIGHT, &cp->reg_value);
        recon_hit_add(p, x, y, w - 4, FIELD_HEIGHT, HIT_FIELD_BASE + 3);
        y += FIELD_HEIGHT + 8;

        int abx = draw_button(cp, p, x, y, "Add",
            HIT_ACTION_BASE + ACTION_REGISTRY_SAVE, true);
        draw_button(cp, p, abx, y, "Cancel",
            HIT_ACTION_BASE + ACTION_REGISTRY_CANCEL, true);
        return;
    }

    if (cp->registry_editing) {
        char label[RECON_REGISTRY_KEY_MAX + 32];
        snprintf(label, sizeof(label), "New value for %s", cp->registry_key);
        recon_draw_text(p, cp->font, x, y + ascent, w, label, COLOR_DIM);
        y += line;

        recon_edit_draw(p, cp->font, x, y, w - 4, FIELD_HEIGHT, &cp->reg_value);
        recon_hit_add(p, x, y, w - 4, FIELD_HEIGHT, HIT_FIELD_BASE + 3);
        y += FIELD_HEIGHT + 8;

        int ebx = draw_button(cp, p, x, y, "Save",
            HIT_ACTION_BASE + ACTION_REGISTRY_SAVE, true);
        draw_button(cp, p, ebx, y, "Cancel",
            HIT_ACTION_BASE + ACTION_REGISTRY_CANCEL, true);
        return;
    }

    bool have = (count > 0 && cp->selected >= 0 && cp->selected < count);

    int cbx = draw_button(cp, p, x, y, "Change",
        HIT_ACTION_BASE + ACTION_REGISTRY_EDIT, have);
    cbx = draw_button(cp, p, cbx, y, "Add",
        HIT_ACTION_BASE + ACTION_REGISTRY_ADD, true);
    draw_button(cp, p, cbx, y, "Remove",
        HIT_ACTION_BASE + ACTION_REGISTRY_REMOVE, have);
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

    int py = y + PADDING;
    for (int i = 0; i < PAGE_COUNT; i++) {
        bool selected = (i == (int)cp->page);

        /* A rule between groups: thirteen pages in one undivided column is a
         * list nobody can find anything in. */
        if (PAGES[i].starts_group && i > 0) {
            recon_fill_rect(p, x + 10, py + 4, SIDEBAR_WIDTH - 24, 1,
                COLOR_SEPARATOR);
            py += 9;
        }

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
        py += ROW_HEIGHT + 2;
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
    case PAGE_PROGRAMS:   draw_programs(cp, p, cx, cy, cw, chh); break;
    case PAGE_MODULES:    draw_modules(cp, p, cx, cy, cw, chh); break;
    case PAGE_NETWORK:    draw_network(cp, p, cx, cy, cw, chh); break;
    case PAGE_REGISTRY:   draw_registry(cp, p, cx, cy, cw, chh); break;
    case PAGE_ABOUT:      draw_system(cp, p, cx, cy, cw, chh); break;
    default:
        draw_pending_page(cp, p, cx, cy, cw, chh, cp->page);
        break;
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

    char name[RECON_REGISTRY_KEY_MAX];
    snprintf(name, sizeof(name), "%s", cp->question_target);

    enum question asked = cp->question;
    cp->question = QUESTION_NONE;

    /*
     * The last button is always Cancel, and dismissing gives -1. Removing an
     * account offers two ways of saying yes, so "anything but the first" is
     * no longer the same as "no".
     */
    int button_count = (asked == QUESTION_REMOVE_USER) ? 3 : 2;
    if (choice < 0 || choice >= button_count - 1) {
        set_status(cp, false, "Nothing was changed.");
        recon_appwin_refresh(cp->win);
        return;
    }

    if (asked == QUESTION_REMOVE_KEY) {
        enum recon_registry_scope scope =
            cp->registry_hive == 0 ? RECON_REG_SYSTEM : RECON_REG_USER;

        if (!recon_registry_remove(scope, name)) {
            set_status(cp, true, "%s", recon_registry_last_error());
        } else {
            set_status(cp, false, "Removed '%s'.", name);
        }
        recon_appwin_refresh(cp->win);
        return;
    }

    if (asked == QUESTION_REMOVE_PROGRAM) {
        if (!recon_modules_uninstall(name)) {
            set_status(cp, true, "%s", recon_modules_last_error());
        } else {
            cp->selected = 0;
            /* The Start menu listed it; it has to stop. */
            recon_shell_restyle(cp->server->shell);
            set_status(cp, false, "Removed '%s'.", name);
        }
        recon_appwin_refresh(cp->win);
        return;
    }

    /* Button 1 was "Delete Files"; button 0 was "Keep Files". */
    bool delete_files = (choice == 1);

    if (!recon_users_remove(name, delete_files)) {
        set_status(cp, true, "%s", recon_users_last_error());
    } else {
        cp->selected = 0;
        set_status(cp, false, delete_files
            ? "Removed '%s' and its files."
            : "Removed '%s'. Its files are still there.", name);
    }
    recon_appwin_refresh(cp->win);
}

static void do_action(struct control_panel *cp, enum action action) {
    struct recon_user chosen;
    bool have = recon_users_at(cp->selected, &chosen);

    switch (action) {
    case ACTION_NONE:
        stop_editing(cp);
        cp->installing = false;
        clear_status(cp);
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
            "Remove the account '%s'?\n"
            "Its folder can be kept or deleted. Deleting cannot be undone.",
            chosen.name);

        /*
         * Three answers, because there are three. Closing an account and
         * destroying somebody's documents are separate decisions, and a
         * dialog offering only "Remove" has already made the second one on
         * their behalf.
         *
         * Cancel last, as everywhere: it is what Enter and Escape choose.
         */
        const char *buttons[3] = { "Keep Files", "Delete Files", "Cancel" };
        recon_appwin_ask(cp->win, "Remove Account", message, buttons, 3,
            answered);
        break;
    }

    case ACTION_NEXT_AVATAR:
        /*
         * Show the pictures rather than cycling through them.
         *
         * A button that steps to the next one asks somebody to click it
         * repeatedly and watch a thumbnail change, which is a way of choosing
         * that shows you one option at a time and never the set. Choosing
         * from a picture is the whole point of having pictures.
         */
        if (have) {
            cp->picking_avatar = true;
            snprintf(cp->question_target, sizeof(cp->question_target), "%s",
                chosen.name);
            set_status(cp, false, "Choose a picture for %s.", chosen.name);
        }
        break;

    case ACTION_CHOOSE_AVATAR:
        cp->picking_avatar = false;
        clear_status(cp);
        break;

    case ACTION_INSTALL_PROGRAM:
        /*
         * A path to type, rather than a file dialog. The dialog draws itself
         * inside the window that asked for it and this page already has a
         * list in that space; a field is the smaller change and does the same
         * job. A browse button is worth having and is not this.
         */
        cp->installing = true;
        recon_edit_begin(&cp->name, recon_fs_user_dir("Downloads"), false);
        set_status(cp, false,
            "The path to a %s or %s, then Enter.",
            RECON_APP_EXT, RECON_MODULE_EXT);
        break;

    case ACTION_CONFIRM_INSTALL:
        if (!recon_modules_install(cp->name.text)) {
            set_status(cp, true, "%s", recon_modules_last_error());
            break;
        }
        cp->installing = false;
        recon_edit_end(&cp->name);
        /* The Start menu lists applications, so it has to hear about a new
         * one arriving. */
        recon_shell_restyle(cp->server->shell);
        set_status(cp, false, "Installed, and loaded.");
        break;

    case ACTION_REMOVE_PROGRAM: {
        /*
         * Only something that came from a module can be removed. The rest are
         * compiled into ReconOS, and "remove the File Explorer" is a request
         * to delete part of the system rather than a program.
         */
        struct recon_installed_app app;
        if (!recon_installed_app_at(cp->selected, &app)) {
            set_status(cp, true, "Choose a program first.");
            break;
        }
        if (app.module[0] == '\0') {
            set_status(cp, true,
                "'%s' is part of ReconOS, not something installed.", app.name);
            break;
        }

        cp->question = QUESTION_REMOVE_PROGRAM;
        snprintf(cp->question_target, sizeof(cp->question_target), "%s",
            app.module);

        char message[256];
        snprintf(message, sizeof(message),
            "Remove '%s'? Its file is deleted, and it will not come back on "
            "the next start.", app.name);

        const char *buttons[2] = { "Remove", "Cancel" };
        recon_appwin_ask(cp->win, "Remove Program", message, buttons, 2,
            answered);
        break;
    }

    case ACTION_LOAD_MODULE: {
        /* Real. Everything in /Apps that is not loaded already. */
        int before = recon_modules_count();
        int loaded = recon_modules_load_all();
        int now = recon_modules_count();

        if (loaded > 0 || now != before) {
            set_status(cp, false, "%d module%s loaded; %d known.",
                loaded, loaded == 1 ? "" : "s", now);
        } else {
            set_status(cp, false, "Nothing new in /Apps.");
        }
        break;
    }

    case ACTION_UNLOAD_MODULE: {
        struct recon_module_state module;
        if (!recon_modules_at(cp->selected, &module)) {
            set_status(cp, true, "Choose a module first.");
            break;
        }
        if (!module.loaded) {
            set_status(cp, true, "'%s' is not loaded.", module.name);
            break;
        }
        if (!recon_modules_unload(module.name)) {
            set_status(cp, true, "%s", recon_modules_last_error());
            break;
        }
        set_status(cp, false, "Unloaded '%s'.", module.name);
        break;
    }

    case ACTION_UNLOCK_REGISTRY: {
        const char *who = recon_users_current();
        if (who == NULL || !recon_users_may_administer()) {
            set_status(cp, true, "Only an administrator can open this.");
            break;
        }
        if (!recon_users_check(who, cp->unlock.text)) {
            /* The password goes out of memory whether it worked or not. */
            recon_edit_begin(&cp->unlock, "", false);
            cp->unlock.masked = true;
            set_status(cp, true, "That is not the password.");
            break;
        }
        recon_edit_end(&cp->unlock);
        cp->registry_unlocked = true;
        cp->registry_scroll = 0;
        set_status(cp, false, "Unlocked. This does not outlive the window.");
        break;
    }

    case ACTION_LOCK_REGISTRY:
        cp->registry_unlocked = false;
        cp->registry_editing = false;
        cp->registry_adding = false;
        recon_edit_end(&cp->reg_key);
        recon_edit_end(&cp->reg_value);
        recon_edit_begin(&cp->unlock, "", false);
        cp->unlock.masked = true;
        set_status(cp, false, "Locked again.");
        break;

    case ACTION_REGISTRY_EDIT: {
        enum recon_registry_scope scope =
            cp->registry_hive == 0 ? RECON_REG_SYSTEM : RECON_REG_USER;

        const char *key = NULL;
        const char *value = NULL;
        if (!recon_registry_at(scope, "", cp->selected, &key, &value)) {
            set_status(cp, true, "Choose a setting first.");
            break;
        }

        /* Copied out, because adding or removing anything renumbers the hive
         * and the index would then point at a different key. */
        snprintf(cp->registry_key, sizeof(cp->registry_key), "%s", key);
        cp->registry_editing = true;
        cp->registry_adding = false;
        recon_edit_begin(&cp->reg_value, value != NULL ? value : "", false);
        set_status(cp, false, "Changing '%s'.", cp->registry_key);
        break;
    }

    case ACTION_REGISTRY_ADD:
        cp->registry_adding = true;
        cp->registry_editing = false;
        cp->reg_key_focused = true;
        recon_edit_begin(&cp->reg_key, "", false);
        recon_edit_begin(&cp->reg_value, "", false);
        set_status(cp, false, "A key, then what it should say.");
        break;

    case ACTION_REGISTRY_CANCEL:
        cp->registry_editing = false;
        cp->registry_adding = false;
        recon_edit_end(&cp->reg_key);
        recon_edit_end(&cp->reg_value);
        set_status(cp, false, "Nothing was changed.");
        break;

    case ACTION_REGISTRY_SAVE: {
        enum recon_registry_scope scope =
            cp->registry_hive == 0 ? RECON_REG_SYSTEM : RECON_REG_USER;

        /*
         * Copied out, not pointed at. The edit fields are closed further
         * down before the status line is written, and recon_edit_end
         * clears the text -- so a pointer into the field left the status
         * saying Saved '' by the time anybody read it.
         */
        const char *typed = cp->registry_adding
            ? cp->reg_key.text : cp->registry_key;

        if (cp->registry_adding && typed[0] == '\0') {
            set_status(cp, true, "A setting needs a key.");
            break;
        }

        /*
         * Refused rather than shortened. The edit field holds more than a
         * key can, and quietly storing the first 127 characters would put
         * a setting in the registry under a name nobody typed -- which
         * then looks like the one that was typed simply did not save.
         */
        size_t typed_len = strlen(typed);
        if (typed_len >= RECON_REGISTRY_KEY_MAX) {
            set_status(cp, true,
                "That key is too long: %d characters at most.",
                RECON_REGISTRY_KEY_MAX - 1);
            break;
        }

        /* Copied by the length just checked, so the bound is the one
         * the refusal above enforces rather than a second guess at
         * it. */
        char key[RECON_REGISTRY_KEY_MAX];
        memcpy(key, typed, typed_len);
        key[typed_len] = '\0';

        if (!recon_registry_set(scope, key, cp->reg_value.text)) {
            /* The registry says why -- a space in the key, a value too long,
             * a full hive -- and its sentence is better than a general one. */
            set_status(cp, true, "%s", recon_registry_last_error());
            break;
        }

        cp->registry_editing = false;
        cp->registry_adding = false;
        recon_edit_end(&cp->reg_key);
        recon_edit_end(&cp->reg_value);

        /*
         * Everything drawn, because a setting here is a setting something is
         * already using: the skin, the spacing, where a window opens. Writing
         * one and leaving the screen as it was would make the registry look
         * like it does not do anything.
         */
        recon_theme_init();
        recon_access_apply(recon_shell_font(cp->server->shell));
        recon_shell_restyle(cp->server->shell);

        set_status(cp, false, "Saved '%s'.", key);
        break;
    }

    case ACTION_REGISTRY_REMOVE: {
        enum recon_registry_scope scope =
            cp->registry_hive == 0 ? RECON_REG_SYSTEM : RECON_REG_USER;

        const char *key = NULL;
        const char *value = NULL;
        if (!recon_registry_at(scope, "", cp->selected, &key, &value)) {
            set_status(cp, true, "Choose a setting first.");
            break;
        }

        cp->question = QUESTION_REMOVE_KEY;
        snprintf(cp->question_target, sizeof(cp->question_target), "%s", key);

        char message[RECON_REGISTRY_KEY_MAX + 160];
        snprintf(message, sizeof(message),
            "Remove '%s'? Whatever reads it goes back to its default, which "
            "may not be what is on screen now.", key);

        const char *buttons[2] = { "Remove", "Cancel" };
        recon_appwin_ask(cp->win, "Remove Setting", message, buttons, 2,
            answered);
        break;
    }

    case ACTION_REGISTRY_HIVE:
        cp->registry_hive = cp->registry_hive == 0 ? 1 : 0;
        cp->registry_scroll = 0;
        cp->selected = 0;
        /* A field open on the other hive's key would save into this one. */
        cp->registry_editing = false;
        cp->registry_adding = false;
        recon_edit_end(&cp->reg_key);
        recon_edit_end(&cp->reg_value);
        break;

    case ACTION_TEST_NETWORK: {
        /*
         * Asks whether something out there answers, and does not wait for the
         * answer. A dead network takes the whole timeout to say so, and a
         * Control Panel frozen for three seconds because somebody pressed a
         * button is worse than the answer is useful. The result lands where
         * the page reads it, and the page redraws when it next does.
         */
        if (!recon_net_online()) {
            set_status(cp, true,
                "Nothing is configured to test with -- no gateway.");
            break;
        }
        if (!recon_net_probe("example.com", 80, 3000, NULL, NULL)) {
            set_status(cp, true, "%s", recon_net_last_error());
            break;
        }
        set_status(cp, false,
            "Asking example.com. The answer appears above.");
        break;
    }

    case ACTION_REFRESH_NETWORK:
        recon_net_refresh();
        set_status(cp, false, "Read the network again.");
        break;

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

    /* A wallpaper chosen from the Appearance page. */
    if (hit_id >= HIT_WALLPAPER_BASE) {
        int index = (int)(hit_id - HIT_WALLPAPER_BASE);
        char name[96];
        if (recon_wallpaper_at(index, name, sizeof(name)) &&
                recon_wallpaper_set(name)) {
            recon_background_reload(cp->server);
            set_status(cp, false, "Wallpaper is now '%s'.", name);
        }
        return true;
    }

    /* A picture chosen from the grid. */
    if (hit_id >= HIT_AVATAR_BASE) {
        int index = (int)(hit_id - HIT_AVATAR_BASE);

        char wanted[64] = "";
        if (index > 0 && !recon_avatar_at(index - 1, wanted, sizeof(wanted))) {
            return true;
        }

        if (!recon_avatar_set(cp->question_target, wanted)) {
            set_status(cp, true, "Could not change the picture.");
            return true;
        }
        cp->picking_avatar = false;
        /* The Start menu and the login screen show it too. */
        recon_shell_restyle(cp->server->shell);
        set_status(cp, false, "%s's picture is set.", cp->question_target);
        return true;
    }

    /* A row on a page of things that are not built: say what is missing. */
    if (hit_id >= HIT_PENDING_BASE) {
        int index = (int)(hit_id - HIT_PENDING_BASE);
        if (index >= 0 && index < PENDING[cp->page].count) {
            set_status(cp, false, "%s",
                PENDING[cp->page].items[index].blocked);
        }
        return true;
    }

    if (hit_id >= HIT_FIELD_BASE) {
        /* The registry's two fields are numbered above the account page's,
         * so a click lands on the one that was drawn there. */
        if (hit_id == HIT_FIELD_BASE + 2) {
            cp->reg_key_focused = true;
            return true;
        }
        if (hit_id == HIT_FIELD_BASE + 3) {
            cp->reg_key_focused = false;
            return true;
        }
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

        if (cp->page == PAGE_REGISTRY) {
            /* The rows are numbered from the first one showing, and the
             * selection is an index into the whole hive. */
            cp->selected = cp->registry_scroll + index;
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
            clear_status(cp);

            /* Leaving the registry page locks it again. Coming back to a page
             * that was left unlocked is how an unlock becomes permanent by
             * accident. */
            cp->installing = false;

            if (page != PAGE_REGISTRY && cp->registry_unlocked) {
                cp->registry_unlocked = false;
                recon_edit_begin(&cp->unlock, "", false);
                cp->unlock.masked = true;
            }
        }
        return true;
    }
    return true;
}

static bool panel_key(void *user, xkb_keysym_t sym, uint32_t modifiers) {
    struct control_panel *cp = user;

    /* Typing the path of something to install. */
    if (cp->installing) {
        switch (recon_edit_key(&cp->name, sym, modifiers)) {
        case RECON_EDIT_COMMIT:
            do_action(cp, ACTION_CONFIRM_INSTALL);
            return true;
        case RECON_EDIT_CANCEL:
            cp->installing = false;
            recon_edit_end(&cp->name);
            clear_status(cp);
            return true;
        case RECON_EDIT_CHANGED:
        case RECON_EDIT_IGNORED:
            return true;
        }
        return true;
    }

    /*
     * Changing or adding a setting. Tab moves between the two fields while
     * adding; there is only one to type in when changing, because the key of
     * an existing setting is not editable.
     */
    if (cp->page == PAGE_REGISTRY &&
            (cp->registry_editing || cp->registry_adding)) {
        if (cp->registry_adding && sym == XKB_KEY_Tab) {
            cp->reg_key_focused = !cp->reg_key_focused;
            return true;
        }

        struct recon_edit *edit = (cp->registry_adding && cp->reg_key_focused)
            ? &cp->reg_key : &cp->reg_value;

        switch (recon_edit_key(edit, sym, modifiers)) {
        case RECON_EDIT_COMMIT:
            do_action(cp, ACTION_REGISTRY_SAVE);
            return true;
        case RECON_EDIT_CANCEL:
            do_action(cp, ACTION_REGISTRY_CANCEL);
            return true;
        case RECON_EDIT_CHANGED:
        case RECON_EDIT_IGNORED:
            return true;
        }
        return true;
    }

    /* The registry's password field has the keyboard while it is showing. */
    if (cp->page == PAGE_REGISTRY && !cp->registry_unlocked) {
        switch (recon_edit_key(&cp->unlock, sym, modifiers)) {
        case RECON_EDIT_COMMIT:
            do_action(cp, ACTION_UNLOCK_REGISTRY);
            return true;
        case RECON_EDIT_CANCEL:
            recon_edit_begin(&cp->unlock, "", false);
            cp->unlock.masked = true;
            clear_status(cp);
            return true;
        case RECON_EDIT_CHANGED:
        case RECON_EDIT_IGNORED:
            return true;
        }
        return true;
    }

    if (cp->page == PAGE_REGISTRY && cp->registry_unlocked) {
        if (sym == XKB_KEY_Up) {
            cp->registry_scroll--;
            return true;
        }
        if (sym == XKB_KEY_Down) {
            cp->registry_scroll++;
            return true;
        }
    }

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
            clear_status(cp);
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
        "  pages: %d\n"
        "  selected: %d\n"
        "  editing: %s\n"
        "  registry: %s, hive %s, scroll %d\n"
        "  status: %s\n",
        PAGES[cp->page].label, PAGE_COUNT, cp->selected,
        cp->editing ? (cp->editing_password_only ? "password" : "new account")
                    : "no",
        cp->registry_unlocked ? "unlocked" : "locked",
        cp->registry_hive == 0 ? "system" : "user", cp->registry_scroll,
        cp->status);
}

static void panel_destroy(void *user) {
    free(user);
}

static const struct recon_appwin_impl CONTROL_PANEL_IMPL = {
    .title = "Control Panel",
    .icon = RECON_ICON_SYSTEM,
    /* Tall enough for the whole page list without scrolling it: a settings
     * window whose own list of settings does not fit is a poor advertisement
     * for the settings. */
    .default_width = 720,
    .default_height = 560,
    .min_width = 560,
    .min_height = 420,
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

    recon_edit_begin(&cp->unlock, "", false);
    cp->unlock.masked = true;

    cp->win = recon_appwin_create(server, font, &CONTROL_PANEL_IMPL, cp);
    if (cp->win == NULL) {
        free(cp);
        return NULL;
    }
    return cp->win;
}
