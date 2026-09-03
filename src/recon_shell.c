/*
 * ReconOS desktop shell: taskbar and apps menu. See include/recon_shell.h.
 *
 * Both are panels the compositor draws into directly, so the whole shell costs
 * two textures regardless of how many buttons are on it, and nothing is
 * redrawn unless something actually changed.
 */

#define _POSIX_C_SOURCE 200112L

#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */

#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "recon_server.h"
#include "recon_shell.h"
#include "recon_appwin.h"
#include "recon_control_panel.h"
#include "recon_desktop.h"
#include "recon_explorer.h"
#include "recon_notepad.h"
#include "recon_terminal.h"
#include "recon_taskmgr.h"
#include "recon_fs.h"
#include "recon_icons.h"
#include "recon_avatar.h"
#include "recon_modules.h"
#include "ReconOS.h"
#include "recon_access.h"
#include "recon_registry.h"
#include "recon_session.h"
#include "recon_users.h"
#include "recon_theme.h"
#include "recon_ui.h"

/* --- Look --- */

#define TASKBAR_HEIGHT 34
#define TASKBAR_PADDING 3
#define BUTTON_HEIGHT (TASKBAR_HEIGHT - TASKBAR_PADDING * 2)
#define APPS_BUTTON_WIDTH 74
#define TASK_BUTTON_MAX_WIDTH 180
#define TASK_BUTTON_MIN_WIDTH 60
#define TEXT_INSET 8

/*
 * The Start menu: two columns, the way one has looked since 95.
 *
 * Applications on the left, places and settings on the right, who is signed in
 * across the top, and what to do with the machine along the bottom. The shape
 * carries meaning -- everything on the right is somewhere to go, everything at
 * the bottom ends your session -- so a menu of one long list would be shorter
 * and worse.
 */
/*
 * The apps menu. Wider and taller than it was: at 400px across with 28px rows
 * it read as a context menu that happened to be in the corner rather than as
 * the way into the system, and the labels had no room to breathe.
 */
#define MENU_ITEM_HEIGHT 32
#define MENU_LEFT_WIDTH 260
#define MENU_RIGHT_WIDTH 230
#define MENU_WIDTH (MENU_LEFT_WIDTH + MENU_RIGHT_WIDTH)
#define MENU_PADDING 4
#define MENU_HEADER_HEIGHT 52
#define MENU_FOOTER_HEIGHT 40
#define MENU_DIVIDER 1

#define FONT_HEIGHT 14

/* The Ctrl+Alt+Del box. */
#define SEC_WIDTH 300
#define SEC_TITLE_HEIGHT 24
#define SEC_ITEM_HEIGHT 30
#define SEC_PADDING 8
#define SEC_BUTTON_HEIGHT 26

/*
 * The one place the shell's colours are defined. A skin is a different set of
 * these, which is why nothing below uses a literal.
 */
#define COLOR_BAR THEME(BAR)
/*
 * The taskbar's ink and a menu's ink are different questions, even though
 * every skin shipped answers them the same. Sharing one name here would mean
 * a skin that wanted a dark bar with light menus could not have one.
 */
#define COLOR_TEXT THEME(BAR_TEXT)
#define COLOR_TEXT_DIM THEME(BAR_TEXT_DIM)
#define COLOR_MENU_TEXT THEME(MENU_TEXT)
#define COLOR_MENU_TEXT_DISABLED THEME(MENU_TEXT_DISABLED)
#define COLOR_BUTTON THEME(BUTTON)
#define COLOR_BUTTON_ACTIVE THEME(BUTTON_ACTIVE)
#define COLOR_MENU THEME(MENU)
#define COLOR_MENU_BORDER THEME(MENU_BORDER)
#define COLOR_MENU_SEPARATOR THEME(MENU_SEPARATOR)
#define COLOR_ACCENT THEME(ACCENT)
#define COLOR_DIALOG_TITLE THEME(DIALOG_TITLE)
#define COLOR_DIALOG_TITLE_TEXT THEME(DIALOG_TITLE_TEXT)
/* What the pointer is over, in a menu. */
#define COLOR_MENU_HILITE THEME(MENU_HILITE)
#define COLOR_MENU_HILITE_TEXT THEME(MENU_HILITE_TEXT)
/* Half-transparent black. Alpha is what makes the desktop show through. */
#define COLOR_DIM THEME(DIM)

/* Hit-region ids. Window buttons use TASK_BASE + index. */
#define HIT_APPS_BUTTON 1
#define HIT_TASK_BASE 100
#define HIT_MENU_BASE 200
/* The right column and the footer have their own ranges, so a click resolves
 * to the right column without any arithmetic about how many are in the other. */
#define HIT_PLACE_BASE 250
#define HIT_POWER_BASE 280
#define HIT_SEC_BASE 300
#define HIT_CONTEXT_BASE 400
#define HIT_DIALOG_BASE 500

/* What a context menu entry does. */
enum context_action {
    CTX_OPEN,
    CTX_DELETE,
    /* Deleting a folder with things in it is a bigger act than deleting a
     * file, so it is asked for separately rather than happening because the
     * first click landed on a directory. */
    CTX_DELETE_TREE,
    CTX_RENAME,
    CTX_CUT,
    CTX_COPY,
    CTX_PASTE,
    CTX_NEW_FOLDER,
    CTX_NEW_FILE,
    CTX_NEW_SHORTCUT,
    CTX_REFRESH,
    CTX_TASK_MANAGER,
    CTX_SHOW_DESKTOP,
    CTX_PURGE,
    CTX_EMPTY_BIN,
    CTX_RESTORE,
    CTX_MINIMIZE,
    CTX_MAXIMIZE,
    CTX_CLOSE,
    CTX_PROPERTIES,
};

/* The context menu. */
#define CONTEXT_ITEM_HEIGHT 24
#define CONTEXT_WIDTH 200
#define CONTEXT_PADDING 3
#define CONTEXT_ITEMS_MAX RECON_MENU_MAX
/* The gap a separator adds under an entry. Named because three places need to
 * agree about it, and a drawn menu whose entries are not where the hit test
 * says they are looks exactly like a menu whose entries do nothing. */
#define CONTEXT_SEPARATOR_HEIGHT 5

/* --- Apps menu contents --- */

/*
 * The Apps menu is built from the application registry rather than from a list
 * here, so an application contributed by a module appears in it without the
 * shell being told about that module. The built-in applications register
 * through the same call a module uses -- an extension path that only outsiders
 * take is an extension path nobody keeps working.
 *
 * Shut Down is not an application, so it is appended as an action after them.
 */
/*
 * What the right column offers. Places first, then the things that configure
 * the machine, which is the order somebody looks for them in.
 */
enum menu_place_kind {
    PLACE_FOLDER,     /* one of the user's own folders */
    PLACE_APP,        /* an application, by registered name */
    PLACE_SEPARATOR,
};

struct menu_place {
    const char *label;
    const char *icon;
    enum menu_place_kind kind;
    /* A folder name under the user's directory, or an application name. */
    const char *target;
};

/*
 * The same places the File Explorer's sidebar offers, in the same order.
 *
 * Three of the six were listed, which made the menu look like it had been
 * abandoned halfway: Desktop, Music and Videos exist, are folders in exactly
 * the same sense, and were simply missing. Two lists of places that disagree
 * about what the places are is worse than either list alone.
 */
static const struct menu_place MENU_PLACES[] = {
    { "Desktop", RECON_ICON_FOLDER, PLACE_FOLDER, "Desktop" },
    { "Documents", RECON_ICON_FOLDER, PLACE_FOLDER, "Documents" },
    { "Downloads", RECON_ICON_FOLDER, PLACE_FOLDER, "Downloads" },
    { "Pictures", RECON_ICON_FOLDER, PLACE_FOLDER, "Pictures" },
    { "Music", RECON_ICON_FOLDER, PLACE_FOLDER, "Music" },
    { "Videos", RECON_ICON_FOLDER, PLACE_FOLDER, "Videos" },
    { "", NULL, PLACE_SEPARATOR, NULL },
    /* The machine itself, above the thing that configures it. */
    { "Recon Core", RECON_ICON_EXPLORER, PLACE_FOLDER, "/" },
    { "", NULL, PLACE_SEPARATOR, NULL },
    /*
     * Only things the left column does not already have. The Task Manager was
     * here as well as there, which meant two entries with one name -- and
     * anything looking one up by name found whichever came first, so the
     * right-hand one could not be reached at all.
     */
    { "Control Panel", RECON_ICON_SYSTEM, PLACE_APP, "Control Panel" },
};

#define MENU_PLACE_COUNT \
    ((int)(sizeof(MENU_PLACES) / sizeof(MENU_PLACES[0])))

/*
 * What to do with the machine, along the bottom.
 *
 * Sleep and hibernate are deliberately absent. They need power management
 * ReconOS does not have -- as a hosted process it cannot suspend the machine
 * -- and a button that does not do what it says is worse than one that is not
 * there.
 */
enum menu_power {
    POWER_LOCK,
    POWER_SIGN_OUT,
    POWER_SWITCH_USER,
    POWER_RESTART,
    POWER_SHUT_DOWN,
};

static const struct {
    const char *label;
    enum menu_power action;
} MENU_POWER[] = {
    { "Lock", POWER_LOCK },
    { "Sign Out", POWER_SIGN_OUT },
    { "Switch User", POWER_SWITCH_USER },
    { "Restart", POWER_RESTART },
    { "Shut Down", POWER_SHUT_DOWN },
};

#define MENU_POWER_COUNT \
    ((int)(sizeof(MENU_POWER) / sizeof(MENU_POWER[0])))

struct menu_entry {
    char label[64];
    char icon[64];
    bool is_shutdown;
};

/* How many applications the left column shows. */
static int menu_entry_count(void) {
    int count = 0;
    int installed = recon_installed_app_count();
    for (int i = 0; i < installed; i++) {
        struct recon_installed_app app;
        if (recon_installed_app_at(i, &app) && app.in_menu) {
            count++;
        }
    }
    return count;
}

static bool menu_entry_at(int index, struct menu_entry *out) {
    int seen = 0;
    int installed = recon_installed_app_count();

    for (int i = 0; i < installed; i++) {
        struct recon_installed_app app;
        if (!recon_installed_app_at(i, &app) || !app.in_menu) {
            continue;
        }
        if (seen == index) {
            snprintf(out->label, sizeof(out->label), "%s", app.name);
            snprintf(out->icon, sizeof(out->icon), "%s", app.icon);
            out->is_shutdown = false;
            return true;
        }
        seen++;
    }
    return false;
}

/* What the Ctrl+Alt+Del box offers, in the order it offers it. */
enum sec_action {
    SEC_TASKMGR,
    SEC_SHUTDOWN,
    SEC_CANCEL,
};

static const char *const SEC_ITEMS[] = {
    "Watchtower",
    "Shut Down",
    "Cancel",
};

#define SEC_COUNT ((int)(sizeof(SEC_ITEMS) / sizeof(SEC_ITEMS[0])))


/* --- Shell --- */

struct recon_shell {
    struct recon_server *server;
    struct recon_font *font;

    /* Setup and the login screen, which sit over everything until somebody
     * is signed in. */
    struct recon_session *session;

    /*
     * Who was signed in last. Windows survive a sign-out so a half-written
     * note is still there when the same person comes back -- but they must
     * not survive a change of person, or the next account sees the last
     * one's work.
     */
    char last_user[64];

    struct recon_desktop *desktop;
    struct recon_panel *taskbar;
    struct recon_panel *menu;
    bool menu_open;
    /*
     * The entry the pointer is over, or -1. A menu that does not show what is
     * about to be chosen makes the user aim and hope; this is what turns a
     * list into something you can read before committing to it.
     */
    int menu_hover;
    int context_hover;
    int security_hover;

    /* Built-in windows. Listed on the taskbar beside client windows, and
     * offered input in front-to-back order. */
    struct recon_appwin *apps[8];
    int app_count;
    /*
     * Indices into apps[], front-most first. Input must be offered in the
     * order things are drawn, not the order they were created, or a window
     * underneath answers for the one on top of it.
     */
    int app_order[8];
    /*
     * Which built-in window holds focus, or -1 for none. Exactly one window
     * may hold it, which is a fact about all of them and so cannot be left to
     * each one separately: every window believing itself focused is how they
     * all drew active title bars and all claimed the keyboard.
     */
    int focused_app;

    /*
     * The right-click menu. One panel serves every use of it -- desktop,
     * explorer, taskbar -- because a context menu is the same thing wherever
     * it appears, differing only in what it offers.
     */
    struct recon_panel *context;
    bool context_open;
    struct {
        char label[48];
        uint32_t id;
        bool enabled;
        bool separator_after;
    } context_items[CONTEXT_ITEMS_MAX];
    int context_item_count;
    enum recon_context_kind context_kind;
    char context_target[RECON_PATH_MAX];

    /*
     * The question currently being asked, if any. One at a time: a second
     * question stacked on the first would leave the user answering them in an
     * order nobody chose.
     */
    /* What the shell itself asked, when the question is its own rather than
     * an application's. */
    enum {
        DESKTOP_ASK_NONE,
        DESKTOP_ASK_TRASH,
        DESKTOP_ASK_PURGE,
        DESKTOP_ASK_EMPTY_BIN,
    } desktop_question;
    char desktop_question_target[RECON_NAME_MAX];

    struct recon_panel *dialog;
    bool dialog_open;
    char dialog_title[64];
    char dialog_message[256];
    char dialog_buttons[RECON_DIALOG_BUTTONS_MAX][24];
    int dialog_button_count;
    int dialog_default;   /* what Enter chooses */
    int dialog_cancel;    /* what Escape chooses */
    int dialog_hover;
    recon_answer_fn dialog_answer;
    void *dialog_user;

    struct recon_panel *security;
    /* Dims the desktop behind the security box, so it is obvious that the
     * question wants answering before anything else happens. */
    struct recon_panel *dim;
    bool security_open;

    int screen_width, screen_height;

    /*
     * Windows in the order their buttons are drawn. The taskbar hit region
     * carries an index into this, so a click resolves to a window without
     * depending on focus order, which changes as soon as the click lands.
     */
    struct taskbar_button {
        struct recon_toplevel *toplevel; /* one of these is set */
        struct recon_appwin *appwin;
    } buttons[32];
    int button_count;
    /* Which button a taskbar context menu was opened on. */
    int context_button;
    /* Which built-in window a window context menu was opened on. */
    int context_app;
};

/* --- Asking the user something --- */

#define DIALOG_WIDTH 380
#define DIALOG_TITLE_HEIGHT 24
#define DIALOG_PADDING 14
#define DIALOG_BUTTON_HEIGHT 26
#define DIALOG_BUTTON_WIDTH 96
#define DIALOG_BUTTON_GAP 8
#define DIALOG_LINE_MAX 3

/* How many lines the message needs, wrapped to the dialog's width. */
static int dialog_wrap(struct recon_shell *shell, const char *message,
        char lines[DIALOG_LINE_MAX][128]) {
    int usable = DIALOG_WIDTH - DIALOG_PADDING * 2;
    int count = 0;

    const char *word = message;
    lines[0][0] = '\0';

    while (*word != '\0' && count < DIALOG_LINE_MAX) {
        const char *end = strchr(word, ' ');
        size_t length = (end != NULL) ? (size_t)(end - word) : strlen(word);

        char candidate[128];
        snprintf(candidate, sizeof(candidate), "%s%s%.*s",
            lines[count], lines[count][0] != '\0' ? " " : "",
            (int)length, word);

        if (recon_text_width(shell->font, candidate) > usable &&
                lines[count][0] != '\0') {
            count++;
            if (count >= DIALOG_LINE_MAX) {
                break;
            }
            snprintf(lines[count], sizeof(lines[count]), "%.*s", (int)length, word);
        } else {
            snprintf(lines[count], sizeof(lines[count]), "%s", candidate);
        }

        word += length;
        while (*word == ' ') {
            word++;
        }
    }

    return count + 1;
}

static int dialog_height(struct recon_shell *shell) {
    char lines[DIALOG_LINE_MAX][128];
    int count = dialog_wrap(shell, shell->dialog_message, lines);
    int line_height = recon_font_line_height(shell->font);
    if (line_height <= 0) {
        line_height = 18;
    }
    return DIALOG_TITLE_HEIGHT + DIALOG_PADDING * 2 + count * line_height +
        DIALOG_PADDING + DIALOG_BUTTON_HEIGHT + DIALOG_PADDING;
}

static void draw_dialog(struct recon_shell *shell) {
    struct recon_panel *p = shell->dialog;
    if (p == NULL) {
        return;
    }

    int width = recon_panel_width(p);
    int height = recon_panel_height(p);
    int ascent = recon_font_ascent(shell->font);
    int line_height = recon_font_line_height(shell->font);
    if (line_height <= 0) {
        line_height = 18;
    }

    recon_fill(p, COLOR_BAR);
    recon_hit_clear(p);

    recon_fill_rect(p, 0, 0, width, DIALOG_TITLE_HEIGHT, COLOR_DIALOG_TITLE);
    recon_draw_text(p, shell->font, DIALOG_PADDING,
        (DIALOG_TITLE_HEIGHT + ascent) / 2 - 1, width - DIALOG_PADDING * 2,
        shell->dialog_title, COLOR_DIALOG_TITLE_TEXT);

    char lines[DIALOG_LINE_MAX][128];
    int count = dialog_wrap(shell, shell->dialog_message, lines);
    for (int i = 0; i < count; i++) {
        recon_draw_text(p, shell->font, DIALOG_PADDING,
            DIALOG_TITLE_HEIGHT + DIALOG_PADDING + ascent + i * line_height,
            width - DIALOG_PADDING * 2, lines[i], COLOR_MENU_TEXT);
    }

    /* Buttons along the bottom right, in the order given, so the safe choice
     * can be placed where the eye lands last. */
    int by = height - DIALOG_PADDING - DIALOG_BUTTON_HEIGHT;
    int bx = width - DIALOG_PADDING -
        shell->dialog_button_count * DIALOG_BUTTON_WIDTH -
        (shell->dialog_button_count - 1) * DIALOG_BUTTON_GAP;

    for (int i = 0; i < shell->dialog_button_count; i++) {
        bool hovered = (i == shell->dialog_hover);

        recon_fill_rect(p, bx, by, DIALOG_BUTTON_WIDTH, DIALOG_BUTTON_HEIGHT,
            hovered ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON);
        recon_draw_bevel(p, bx, by, DIALOG_BUTTON_WIDTH, DIALOG_BUTTON_HEIGHT, false);

        /* The default is outlined, so Enter's meaning is visible. */
        if (i == shell->dialog_default) {
            recon_stroke_rect(p, bx + 2, by + 2, DIALOG_BUTTON_WIDTH - 4,
                DIALOG_BUTTON_HEIGHT - 4, COLOR_ACCENT);
        }

        int text_w = recon_text_width(shell->font, shell->dialog_buttons[i]);
        recon_draw_text(p, shell->font, bx + (DIALOG_BUTTON_WIDTH - text_w) / 2,
            by + (DIALOG_BUTTON_HEIGHT + ascent) / 2 - 2, DIALOG_BUTTON_WIDTH,
            shell->dialog_buttons[i], COLOR_MENU_TEXT);

        recon_hit_add(p, bx, by, DIALOG_BUTTON_WIDTH, DIALOG_BUTTON_HEIGHT,
            HIT_DIALOG_BASE + i);

        bx += DIALOG_BUTTON_WIDTH + DIALOG_BUTTON_GAP;
    }

    recon_draw_bevel(p, 0, 0, width, height, false);
    recon_stroke_rect(p, 0, 0, width, height, COLOR_MENU_BORDER);
    recon_panel_commit(p);
}

bool recon_shell_dialog_open(struct recon_shell *shell) {
    return shell != NULL && shell->dialog_open;
}

/* Take the question down and hand the answer back. */
static void dialog_finish(struct recon_shell *shell, int choice) {
    if (!shell->dialog_open) {
        return;
    }

    recon_answer_fn answer = shell->dialog_answer;
    void *user = shell->dialog_user;

    /* Cleared before the callback runs: the answer may well ask another
     * question, and it should not be refused because this one is still up. */
    shell->dialog_open = false;
    shell->dialog_answer = NULL;
    shell->dialog_user = NULL;
    recon_panel_set_enabled(shell->dialog, false);
    if (shell->dim != NULL && !shell->security_open) {
        recon_panel_set_enabled(shell->dim, false);
    }
    recon_damage_all(shell->server);

    if (answer != NULL) {
        answer(user, choice);
    }
}

void recon_shell_cancel_dialog(struct recon_shell *shell, void *user) {
    if (shell == NULL || !shell->dialog_open || shell->dialog_user != user) {
        return;
    }
    /* Dropped without answering: whatever asked is going away, so calling back
     * into it would be a use after free. */
    shell->dialog_open = false;
    shell->dialog_answer = NULL;
    shell->dialog_user = NULL;
    recon_panel_set_enabled(shell->dialog, false);
    if (shell->dim != NULL && !shell->security_open) {
        recon_panel_set_enabled(shell->dim, false);
    }
    recon_damage_all(shell->server);
}

void recon_shell_ask(struct recon_shell *shell, const char *title,
        const char *message, const char *const *buttons, int button_count,
        recon_answer_fn answer, void *user) {
    if (shell == NULL || shell->dialog == NULL || buttons == NULL ||
            button_count < 1) {
        return;
    }
    if (button_count > RECON_DIALOG_BUTTONS_MAX) {
        button_count = RECON_DIALOG_BUTTONS_MAX;
    }

    /* A question already up is answered as dismissed rather than dropped, so
     * whatever asked it is not left waiting forever. */
    if (shell->dialog_open) {
        dialog_finish(shell, -1);
    }

    snprintf(shell->dialog_title, sizeof(shell->dialog_title), "%s",
        title != NULL ? title : "ReconOS");
    snprintf(shell->dialog_message, sizeof(shell->dialog_message), "%s",
        message != NULL ? message : "");

    for (int i = 0; i < button_count; i++) {
        snprintf(shell->dialog_buttons[i], sizeof(shell->dialog_buttons[i]),
            "%s", buttons[i]);
    }
    shell->dialog_button_count = button_count;

    /*
     * The last button is the default and the way out. Callers put the safe
     * answer last, so Enter and Escape both decline rather than confirm --
     * a dialog that destroys something when you hit Return by reflex is worse
     * than no dialog.
     */
    shell->dialog_default = button_count - 1;
    shell->dialog_cancel = button_count - 1;
    shell->dialog_hover = -1;
    shell->dialog_answer = answer;
    shell->dialog_user = user;
    shell->dialog_open = true;

    int height = dialog_height(shell);
    recon_panel_resize(shell->dialog, DIALOG_WIDTH, height);
    recon_panel_set_position(shell->dialog,
        (shell->screen_width - DIALOG_WIDTH) / 2,
        (shell->screen_height - height) / 2);

    if (shell->dim != NULL) {
        recon_panel_set_enabled(shell->dim, true);
        recon_panel_raise_to_top(shell->dim);
    }

    draw_dialog(shell);
    recon_panel_set_enabled(shell->dialog, true);
    recon_panel_raise_to_top(shell->dialog);
    recon_damage_all(shell->server);
}

static int menu_height(void) {
    /* As tall as whichever column needs more, plus the header and footer.
     * Sizing to the left alone would clip the right when few applications are
     * installed, which is exactly the state a new system is in. */
    int left = menu_entry_count() * MENU_ITEM_HEIGHT;
    int right = MENU_PLACE_COUNT * MENU_ITEM_HEIGHT;
    int body = left > right ? left : right;

    return MENU_HEADER_HEIGHT + body + MENU_PADDING * 2 + MENU_FOOTER_HEIGHT;
}

static int security_height(void) {
    return SEC_TITLE_HEIGHT + SEC_PADDING * 3 +
        SEC_COUNT * (SEC_BUTTON_HEIGHT + SEC_PADDING) + 20;
}

/*
 * Give focus to one built-in window, taking it from every other.
 *
 * Pass -1 when focus goes elsewhere -- to a client window, or nowhere.
 */
static void set_focused_app(struct recon_shell *shell, int index) {
    shell->focused_app = index;
    for (int i = 0; i < shell->app_count; i++) {
        recon_appwin_set_focused(shell->apps[i], i == index);
    }
}

/* --- Context menu --- */

/* Defined with the other input helpers, further down. */
static bool point_in_panel(struct recon_panel *panel, double lx, double ly,
    int *px, int *py);
static void draw_menu(struct recon_shell *shell);
static void toggle_menu(struct recon_shell *shell);
static void draw_security(struct recon_shell *shell);

/*
 * Where entry `index` starts, inside the menu panel.
 *
 * The one place this arithmetic lives. Drawing, hit-testing and looking the
 * entry up from outside all go through it, so they cannot drift apart.
 */
static int context_entry_y(struct recon_shell *shell, int index) {
    int y = CONTEXT_PADDING;
    for (int i = 0; i < index && i < shell->context_item_count; i++) {
        y += CONTEXT_ITEM_HEIGHT;
        if (shell->context_items[i].separator_after) {
            y += CONTEXT_SEPARATOR_HEIGHT;
        }
    }
    return y;
}

static int context_height(struct recon_shell *shell) {
    if (shell->context_item_count == 0) {
        return CONTEXT_PADDING * 2;
    }
    int last = shell->context_item_count - 1;
    return context_entry_y(shell, last) + CONTEXT_ITEM_HEIGHT +
        (shell->context_items[last].separator_after ? CONTEXT_SEPARATOR_HEIGHT : 0) +
        CONTEXT_PADDING;
}

bool recon_shell_context_entry_at(struct recon_shell *shell, const char *label,
        int *x, int *y) {
    if (shell == NULL || !shell->context_open || shell->context == NULL ||
            label == NULL) {
        return false;
    }

    for (int i = 0; i < shell->context_item_count; i++) {
        if (strcasecmp(shell->context_items[i].label, label) != 0) {
            continue;
        }

        int px = 0, py = 0;
        recon_panel_position(shell->context, &px, &py);
        if (x != NULL) {
            *x = px + CONTEXT_WIDTH / 2;
        }
        if (y != NULL) {
            *y = py + context_entry_y(shell, i) + CONTEXT_ITEM_HEIGHT / 2;
        }
        return true;
    }
    return false;
}

struct recon_appwin *recon_shell_focused_app(struct recon_shell *shell) {
    if (shell == NULL || shell->focused_app < 0 ||
            shell->focused_app >= shell->app_count) {
        return NULL;
    }
    return shell->apps[shell->focused_app];
}

bool recon_shell_dialog_button_at(struct recon_shell *shell, const char *label,
        int *x, int *y) {
    if (shell == NULL || !shell->dialog_open || shell->dialog == NULL ||
            label == NULL) {
        return false;
    }

    for (int i = 0; i < shell->dialog_button_count; i++) {
        if (strcasecmp(shell->dialog_buttons[i], label) != 0) {
            continue;
        }

        /* Asked of the panel rather than recomputed, so this cannot drift
         * from where the button was actually drawn. */
        int rx, ry, rw, rh;
        uint32_t id;
        for (size_t r = 0; recon_hit_region(shell->dialog, r, &rx, &ry, &rw, &rh, &id); r++) {
            if (id != (uint32_t)(HIT_DIALOG_BASE + i)) {
                continue;
            }
            int px = 0, py = 0;
            recon_panel_position(shell->dialog, &px, &py);
            if (x != NULL) { *x = px + rx + rw / 2; }
            if (y != NULL) { *y = py + ry + rh / 2; }
            return true;
        }
        return false;
    }
    return false;
}

/*
 * Find a Start menu entry by what it says.
 *
 * Asked of the panel's own regions rather than recomputed, so this cannot
 * drift from where the entry was actually drawn -- which is the failure that
 * makes a menu look like it does nothing.
 */
bool recon_shell_menu_entry_at(struct recon_shell *shell, const char *label,
        int *x, int *y) {
    if (shell == NULL || !shell->menu_open || shell->menu == NULL ||
            label == NULL) {
        return false;
    }

    /* Work out which id carries that label, then ask the panel where it is. */
    uint32_t wanted = 0;
    bool found = false;

    int apps = menu_entry_count();
    for (int i = 0; i < apps && !found; i++) {
        struct menu_entry entry;
        if (menu_entry_at(i, &entry) && strcasecmp(entry.label, label) == 0) {
            wanted = HIT_MENU_BASE + i;
            found = true;
        }
    }
    for (int i = 0; i < MENU_PLACE_COUNT && !found; i++) {
        if (MENU_PLACES[i].kind != PLACE_SEPARATOR &&
                strcasecmp(MENU_PLACES[i].label, label) == 0) {
            wanted = HIT_PLACE_BASE + i;
            found = true;
        }
    }
    for (int i = 0; i < MENU_POWER_COUNT && !found; i++) {
        if (strcasecmp(MENU_POWER[i].label, label) == 0) {
            wanted = HIT_POWER_BASE + i;
            found = true;
        }
    }
    if (!found) {
        return false;
    }

    int px = 0, py = 0;
    recon_panel_position(shell->menu, &px, &py);

    for (size_t r = 0;; r++) {
        int rx, ry, rw, rh;
        uint32_t id;
        if (!recon_hit_region(shell->menu, r, &rx, &ry, &rw, &rh, &id)) {
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

void recon_shell_describe(struct recon_shell *shell, char *out, size_t size) {
    if (shell == NULL || out == NULL || size == 0) {
        return;
    }

    size_t used = 0;
    #define EMIT(...) do { \
        if (used < size) { \
            int w = snprintf(out + used, size - used, __VA_ARGS__); \
            if (w > 0) { used += (size_t)w; } \
        } \
    } while (0)

    EMIT("focus: ");
    if (shell->focused_app >= 0 && shell->focused_app < shell->app_count) {
        EMIT("%s\n", recon_appwin_title(shell->apps[shell->focused_app]));
    } else {
        EMIT("(none)\n");
    }

    EMIT("desktop renaming: %s\n",
        recon_desktop_is_renaming(shell->desktop) ? "yes" : "no");
    EMIT("clipboard: %s\n", recon_fs_clip_empty() ? "(empty)" : "holds something");

    EMIT("windows:\n");
    for (int i = 0; i < shell->app_count; i++) {
        struct recon_appwin *win = shell->apps[i];
        if (!recon_appwin_is_open(win)) {
            continue;
        }
        int wx = 0, wy = 0, ww = 0, wh = 0, cx = 0, cy = 0;
        recon_appwin_geometry(win, &wx, &wy, &ww, &wh);
        recon_appwin_content_origin(win, &cx, &cy);
        EMIT("  %-22s %-9s%s frame %d,%d %dx%d content-origin %d,%d\n",
            recon_appwin_title(win),
            recon_appwin_is_minimized(win) ? "minimized" : "open",
            recon_appwin_is_focused(win) ? " focused" : "",
            wx, wy, ww, wh, cx, cy);
    }

    EMIT("apps menu: %s\n", shell->menu_open ? "open" : "closed");

    if (shell->menu_open) {
        /* Everything in it, with where to click, so a test does not have to
         * work out the layout for itself. */
        int apps = menu_entry_count();
        for (int i = 0; i < apps; i++) {
            struct menu_entry entry;
            int mx = 0, my = 0;
            if (menu_entry_at(i, &entry) &&
                    recon_shell_menu_entry_at(shell, entry.label, &mx, &my)) {
                EMIT("  app    %-18s click at %d,%d\n", entry.label, mx, my);
            }
        }
        for (int i = 0; i < MENU_PLACE_COUNT; i++) {
            int mx = 0, my = 0;
            if (MENU_PLACES[i].kind != PLACE_SEPARATOR &&
                    recon_shell_menu_entry_at(shell, MENU_PLACES[i].label,
                        &mx, &my)) {
                EMIT("  place  %-18s click at %d,%d\n",
                    MENU_PLACES[i].label, mx, my);
            }
        }
        for (int i = 0; i < MENU_POWER_COUNT; i++) {
            int mx = 0, my = 0;
            if (recon_shell_menu_entry_at(shell, MENU_POWER[i].label,
                    &mx, &my)) {
                EMIT("  power  %-18s click at %d,%d\n",
                    MENU_POWER[i].label, mx, my);
            }
        }
    }

    if (!shell->dialog_open) {
        EMIT("dialog: closed\n");
    } else {
        EMIT("dialog: \"%s\" -- %s\n", shell->dialog_title, shell->dialog_message);
        for (int i = 0; i < shell->dialog_button_count; i++) {
            int bx = 0, by = 0;
            bool found = recon_shell_dialog_button_at(shell,
                shell->dialog_buttons[i], &bx, &by);
            EMIT("  [%d] %-12s%s click at %d,%d\n", i, shell->dialog_buttons[i],
                i == shell->dialog_default ? " (default)" : "         ",
                found ? bx : -1, found ? by : -1);
        }
    }

    if (!shell->context_open) {
        EMIT("context menu: closed\n");
    } else {
        int px = 0, py = 0;
        recon_panel_position(shell->context, &px, &py);
        EMIT("context menu: open kind=%d at %d,%d target='%s'\n",
            (int)shell->context_kind, px, py, shell->context_target);
        for (int i = 0; i < shell->context_item_count; i++) {
            EMIT("  [%d] %-22s %-8s click at %d,%d\n", i,
                shell->context_items[i].label,
                shell->context_items[i].enabled ? "enabled" : "disabled",
                px + CONTEXT_WIDTH / 2,
                py + context_entry_y(shell, i) + CONTEXT_ITEM_HEIGHT / 2);
        }
    }

    #undef EMIT
}

static void draw_context(struct recon_shell *shell) {
    struct recon_panel *p = shell->context;
    if (p == NULL) {
        return;
    }

    int width = recon_panel_width(p);
    int height = recon_panel_height(p);
    int ascent = recon_font_ascent(shell->font);

    recon_fill(p, COLOR_MENU);
    recon_hit_clear(p);

    for (int i = 0; i < shell->context_item_count; i++) {
        int y = context_entry_y(shell, i);

        /* Only entries that can be chosen highlight: showing a disabled one
         * as selectable would promise something the click will not do. */
        bool hovered = (i == shell->context_hover) && shell->context_items[i].enabled;

        if (hovered) {
            recon_fill_rect(p, CONTEXT_PADDING, y, width - CONTEXT_PADDING * 2,
                CONTEXT_ITEM_HEIGHT, COLOR_MENU_HILITE);
        }

        recon_draw_text(p, shell->font, 14, y + (CONTEXT_ITEM_HEIGHT + ascent) / 2 - 2,
            width - 24, shell->context_items[i].label,
            !shell->context_items[i].enabled ? COLOR_MENU_TEXT_DISABLED :
            hovered ? COLOR_MENU_HILITE_TEXT : COLOR_MENU_TEXT);

        /* Disabled entries are shown rather than hidden, so the menu keeps the
         * same shape and what is unavailable is visible. */
        if (shell->context_items[i].enabled) {
            recon_hit_add(p, CONTEXT_PADDING, y, width - CONTEXT_PADDING * 2,
                CONTEXT_ITEM_HEIGHT, HIT_CONTEXT_BASE + i);
        }

        if (shell->context_items[i].separator_after) {
            recon_fill_rect(p, 6, y + CONTEXT_ITEM_HEIGHT + 2, width - 12, 1,
                RECON_RGB(0x90, 0x90, 0x90));
        }
    }

    recon_draw_bevel(p, 0, 0, width, height, false);
    recon_stroke_rect(p, 0, 0, width, height, COLOR_MENU_BORDER);
    recon_panel_commit(p);
}

/*
 * Add an entry whose id means whatever the caller decided.
 *
 * Application menus carry the application's own ids, so the id cannot be an
 * enum here; context_activate looks at the menu's kind to know how to read it.
 */
static void context_add_id(struct recon_shell *shell, const char *label,
        uint32_t id, bool enabled, bool separator) {
    if (shell->context_item_count >= CONTEXT_ITEMS_MAX) {
        return;
    }
    int i = shell->context_item_count++;
    snprintf(shell->context_items[i].label, sizeof(shell->context_items[i].label),
        "%s", label);
    shell->context_items[i].id = id;
    shell->context_items[i].enabled = enabled;
    shell->context_items[i].separator_after = separator;
}

static void context_add(struct recon_shell *shell, const char *label,
        enum context_action action, bool enabled, bool separator) {
    if (shell->context_item_count >= CONTEXT_ITEMS_MAX) {
        return;
    }
    int i = shell->context_item_count++;
    snprintf(shell->context_items[i].label, sizeof(shell->context_items[i].label),
        "%s", label);
    shell->context_items[i].id = (uint32_t)action;
    shell->context_items[i].enabled = enabled;
    shell->context_items[i].separator_after = separator;
}

/* Show the menu at a point, kept on screen if it would run off an edge. */
static void context_show(struct recon_shell *shell, double lx, double ly) {
    if (shell->context == NULL || shell->context_item_count == 0) {
        return;
    }

    int height = context_height(shell);
    recon_panel_resize(shell->context, CONTEXT_WIDTH, height);

    int x = (int)lx;
    int y = (int)ly;
    if (x + CONTEXT_WIDTH > shell->screen_width) {
        x = shell->screen_width - CONTEXT_WIDTH;
    }
    if (y + height > shell->screen_height - TASKBAR_HEIGHT) {
        /* Above the pointer rather than below, so it neither runs off the
         * bottom nor hides behind the taskbar. */
        y -= height;
    }
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }

    recon_panel_set_position(shell->context, x, y);
    draw_context(shell);
    recon_panel_set_enabled(shell->context, true);
    recon_panel_raise_to_top(shell->context);
    shell->context_hover = -1;
    shell->context_open = true;
    recon_damage_all(shell->server);
}

void recon_shell_close_context(struct recon_shell *shell) {
    if (shell == NULL || !shell->context_open) {
        return;
    }
    shell->context_open = false;
    recon_panel_set_enabled(shell->context, false);
    recon_damage_all(shell->server);
}

/* Move a window to the front of the input order. */
static void raise_app_order(struct recon_shell *shell, int index) {
    int position = -1;
    for (int i = 0; i < shell->app_count; i++) {
        if (shell->app_order[i] == index) {
            position = i;
            break;
        }
    }
    if (position <= 0) {
        return;
    }
    for (int i = position; i > 0; i--) {
        shell->app_order[i] = shell->app_order[i - 1];
    }
    shell->app_order[0] = index;
}

/*
 * The topmost scene node at this point.
 *
 * The scene graph is the only thing that knows what is actually drawn on top.
 * Asking whether a point falls inside a window is not the same question: a
 * maximized window contains every point on screen, so it would claim clicks
 * meant for windows stacked above it.
 */
static struct wlr_scene_node *topmost_node(struct recon_shell *shell,
        double lx, double ly) {
    double sx, sy;
    return wlr_scene_node_at(&shell->server->scene->tree.node, lx, ly, &sx, &sy);
}

/* The built-in window that owns a node, if any. */
static int appwin_index_for_node(struct recon_shell *shell,
        struct wlr_scene_node *node) {
    if (node == NULL) {
        return -1;
    }
    for (int i = 0; i < shell->app_count; i++) {
        if (recon_appwin_node(shell->apps[i]) == node) {
            return i;
        }
    }
    return -1;
}

/* --- Drawing --- */

/*
 * One taskbar button. A minimized window is drawn recessed and its label
 * dimmed, so the bar shows at a glance what is on screen and what is put away.
 */
static void draw_task_button(struct recon_shell *shell, struct recon_panel *bar,
        int x, int w, int baseline, const char *title, const char *icon,
        bool active, bool minimized) {
    recon_color fill = active ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
    if (minimized) {
        fill = COLOR_BAR;
    }

    recon_fill_rect(bar, x, TASKBAR_PADDING, w, BUTTON_HEIGHT, fill);
    recon_draw_bevel(bar, x, TASKBAR_PADDING, w, BUTTON_HEIGHT, active || minimized);

    /* The icon comes first, so a bar of buttons can be read at a glance
     * without depending on the titles fitting. */
    int text_x = x + TEXT_INSET;
    int icon_size = BUTTON_HEIGHT - 10;
    if (icon != NULL &&
            recon_icon_draw(bar, icon, x + 5, TASKBAR_PADDING + 5, icon_size)) {
        text_x = x + 5 + icon_size + 5;
    }

    recon_draw_text(bar, shell->font, text_x, baseline,
        w - (text_x - x) - TEXT_INSET, title,
        active ? COLOR_TEXT : COLOR_TEXT_DIM);
}

static void draw_taskbar(struct recon_shell *shell) {
    struct recon_panel *bar = shell->taskbar;
    if (bar == NULL) {
        return;
    }

    int width = recon_panel_width(bar);
    int baseline = TASKBAR_PADDING + (BUTTON_HEIGHT + recon_font_ascent(shell->font)) / 2 - 2;

    recon_fill(bar, COLOR_BAR);
    /* A highlight along the top edge lifts the bar off the wallpaper. */
    recon_fill_rect(bar, 0, 0, width, 1, RECON_RGB(0xE8, 0xE8, 0xE8));

    recon_hit_clear(bar);

    /* Apps button. */
    recon_fill_rect(bar, TASKBAR_PADDING, TASKBAR_PADDING,
        APPS_BUTTON_WIDTH, BUTTON_HEIGHT,
        shell->menu_open ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON);
    recon_draw_bevel(bar, TASKBAR_PADDING, TASKBAR_PADDING,
        APPS_BUTTON_WIDTH, BUTTON_HEIGHT, shell->menu_open);

    /* A mark so the button reads as the system menu rather than a window. */
    int mark_size = BUTTON_HEIGHT - 10;
    if (!recon_icon_draw(bar, "system", TASKBAR_PADDING + 6, TASKBAR_PADDING + 5,
            mark_size)) {
        recon_fill_rect(bar, TASKBAR_PADDING + TEXT_INSET, TASKBAR_PADDING + 9,
            10, 10, COLOR_ACCENT);
    }
    recon_draw_text(bar, shell->font, TASKBAR_PADDING + 6 + mark_size + 6, baseline,
        APPS_BUTTON_WIDTH - mark_size - 18, "Apps", COLOR_TEXT);
    recon_hit_add(bar, TASKBAR_PADDING, TASKBAR_PADDING,
        APPS_BUTTON_WIDTH, BUTTON_HEIGHT, HIT_APPS_BUTTON);

    /* One button per window, client or built-in, sharing the remaining
     * width. Both kinds appear here, because from the user's side there is no
     * difference between them. */
    shell->button_count = 0;
    int x = TASKBAR_PADDING * 2 + APPS_BUTTON_WIDTH;
    int available = width - x - TASKBAR_PADDING;

    int window_count = wl_list_length(&shell->server->toplevels);
    for (int i = 0; i < shell->app_count; i++) {
        if (recon_appwin_is_open(shell->apps[i])) {
            window_count++;
        }
    }

    if (window_count <= 0 || available < TASK_BUTTON_MIN_WIDTH) {
        recon_panel_commit(bar);
        return;
    }

    int button_width = (available / window_count) - TASKBAR_PADDING;
    if (button_width > TASK_BUTTON_MAX_WIDTH) {
        button_width = TASK_BUTTON_MAX_WIDTH;
    }
    if (button_width < TASK_BUTTON_MIN_WIDTH) {
        button_width = TASK_BUTTON_MIN_WIDTH;
    }

    int max_buttons = (int)(sizeof(shell->buttons) / sizeof(shell->buttons[0]));

    /* Built-in windows first, so their position does not shift as client
     * windows come and go. */
    for (int i = 0; i < shell->app_count; i++) {
        struct recon_appwin *win = shell->apps[i];
        if (!recon_appwin_is_open(win) || shell->button_count >= max_buttons) {
            continue;
        }
        if (x + button_width > width - TASKBAR_PADDING) {
            break;
        }

        bool active = recon_appwin_is_focused(win);
        draw_task_button(shell, bar, x, button_width, baseline,
            recon_appwin_title(win), recon_appwin_icon(win),
            active, recon_appwin_is_minimized(win));

        recon_hit_add(bar, x, TASKBAR_PADDING, button_width, BUTTON_HEIGHT,
            HIT_TASK_BASE + shell->button_count);
        shell->buttons[shell->button_count].toplevel = NULL;
        shell->buttons[shell->button_count].appwin = win;
        shell->button_count++;
        x += button_width + TASKBAR_PADDING;
    }

    struct recon_toplevel *toplevel;
    wl_list_for_each(toplevel, &shell->server->toplevels, link) {
        if (shell->button_count >= max_buttons) {
            break;
        }
        if (x + button_width > width - TASKBAR_PADDING) {
            break;
        }

        /* A client window has no icon of its own, so it gets the generic
         * application one rather than nothing. */
        draw_task_button(shell, bar, x, button_width, baseline,
            recon_toplevel_title(toplevel), RECON_ICON_APP,
            recon_toplevel_is_focused(toplevel),
            recon_toplevel_is_minimized(toplevel));

        recon_hit_add(bar, x, TASKBAR_PADDING, button_width, BUTTON_HEIGHT,
            HIT_TASK_BASE + shell->button_count);
        shell->buttons[shell->button_count].toplevel = toplevel;
        shell->buttons[shell->button_count].appwin = NULL;
        shell->button_count++;
        x += button_width + TASKBAR_PADDING;
    }

    recon_panel_commit(bar);
}

static void draw_menu(struct recon_shell *shell) {
    struct recon_panel *menu = shell->menu;
    if (menu == NULL) {
        return;
    }

    int width = recon_panel_width(menu);
    int height = recon_panel_height(menu);

    recon_fill(menu, COLOR_MENU);
    recon_stroke_rect(menu, 0, 0, width, height, COLOR_MENU_BORDER);
    recon_draw_bevel(menu, 1, 1, width - 2, height - 2, false);

    recon_hit_clear(menu);

    int ascent = recon_font_ascent(shell->font);
    int icon_size = MENU_ITEM_HEIGHT - 8;

    /*
     * The header: who is signed in. Across the top of both columns, because it
     * is a fact about the whole menu rather than about either side.
     */
    recon_fill_rect(menu, 1, 1, width - 2, MENU_HEADER_HEIGHT, COLOR_DIALOG_TITLE);

    /*
     * The person's own picture, not the system's mark. The mark is already on
     * the Apps button an inch below, so the header was showing it twice and
     * saying nothing about whose menu this is. This is the one place the
     * account is named on the desktop; it should look like them.
     */
    const char *who = recon_users_current();
    int face = MENU_HEADER_HEIGHT - 16;
    recon_avatar_draw(menu, shell->font, who, 10, 8, face);
    int header_x = 10 + face + 12;

    recon_draw_text(menu, shell->font, header_x,
        (MENU_HEADER_HEIGHT + ascent) / 2 - 1, width - header_x - 12,
        who != NULL ? who : RECONOS_FULL_NAME, COLOR_DIALOG_TITLE_TEXT);

    int body_y = MENU_HEADER_HEIGHT + MENU_PADDING;
    int body_bottom = height - MENU_FOOTER_HEIGHT - MENU_PADDING;

    /* The divider between the columns, which is what makes them read as two
     * lists rather than one wide one. */
    recon_fill_rect(menu, MENU_LEFT_WIDTH, body_y, MENU_DIVIDER,
        body_bottom - body_y, COLOR_MENU_SEPARATOR);

    /* --- Left: the applications --- */
    int count = menu_entry_count();
    for (int i = 0; i < count; i++) {
        struct menu_entry entry;
        if (!menu_entry_at(i, &entry)) {
            break;
        }

        int y = body_y + i * MENU_ITEM_HEIGHT;
        if (y + MENU_ITEM_HEIGHT > body_bottom) {
            break;
        }
        int baseline = y + (MENU_ITEM_HEIGHT + ascent) / 2 - 2;
        bool hovered = (shell->menu_hover == HIT_MENU_BASE + i);

        if (hovered) {
            recon_fill_rect(menu, MENU_PADDING, y,
                MENU_LEFT_WIDTH - MENU_PADDING * 2, MENU_ITEM_HEIGHT,
                COLOR_MENU_HILITE);
        }

        int label_x = MENU_PADDING + TEXT_INSET;
        if (recon_icon_draw(menu, entry.icon, MENU_PADDING + 6, y + 4, icon_size)) {
            label_x = MENU_PADDING + 6 + icon_size + 8;
        }
        recon_draw_text(menu, shell->font, label_x, baseline,
            MENU_LEFT_WIDTH - label_x - MENU_PADDING, entry.label,
            hovered ? COLOR_MENU_HILITE_TEXT : COLOR_MENU_TEXT);
        recon_hit_add(menu, MENU_PADDING, y,
            MENU_LEFT_WIDTH - MENU_PADDING * 2, MENU_ITEM_HEIGHT,
            HIT_MENU_BASE + i);
    }

    /* --- Right: places, then the things that configure the machine --- */
    int right_x = MENU_LEFT_WIDTH + MENU_DIVIDER + MENU_PADDING;
    int right_w = width - right_x - MENU_PADDING;

    for (int i = 0; i < MENU_PLACE_COUNT; i++) {
        int y = body_y + i * MENU_ITEM_HEIGHT;
        if (y + MENU_ITEM_HEIGHT > body_bottom) {
            break;
        }

        if (MENU_PLACES[i].kind == PLACE_SEPARATOR) {
            recon_fill_rect(menu, right_x + 6, y + MENU_ITEM_HEIGHT / 2,
                right_w - 12, 1, COLOR_MENU_SEPARATOR);
            continue;
        }

        int baseline = y + (MENU_ITEM_HEIGHT + ascent) / 2 - 2;
        bool hovered = (shell->menu_hover == HIT_PLACE_BASE + i);

        if (hovered) {
            recon_fill_rect(menu, right_x, y, right_w, MENU_ITEM_HEIGHT,
                COLOR_MENU_HILITE);
        }

        int label_x = right_x + TEXT_INSET;
        if (MENU_PLACES[i].icon != NULL &&
                recon_icon_draw(menu, MENU_PLACES[i].icon, right_x + 4,
                    y + 4, icon_size)) {
            label_x = right_x + 4 + icon_size + 8;
        }
        recon_draw_text(menu, shell->font, label_x, baseline,
            right_x + right_w - label_x, MENU_PLACES[i].label,
            hovered ? COLOR_MENU_HILITE_TEXT : COLOR_MENU_TEXT);
        recon_hit_add(menu, right_x, y, right_w, MENU_ITEM_HEIGHT,
            HIT_PLACE_BASE + i);
    }

    /* --- Footer: what to do with the machine --- */
    int fy = height - MENU_FOOTER_HEIGHT;
    recon_fill_rect(menu, 1, fy, width - 2, MENU_FOOTER_HEIGHT - 1, COLOR_BAR);
    recon_fill_rect(menu, 1, fy, width - 2, 1, COLOR_MENU_SEPARATOR);

    int button_w = (width - 2 - MENU_PADDING * 2) / MENU_POWER_COUNT;
    for (int i = 0; i < MENU_POWER_COUNT; i++) {
        int bx = MENU_PADDING + i * button_w;
        int by = fy + 5;
        int bh = MENU_FOOTER_HEIGHT - 11;
        bool hovered = (shell->menu_hover == HIT_POWER_BASE + i);

        if (hovered) {
            recon_fill_rect(menu, bx, by, button_w - 2, bh, COLOR_MENU_HILITE);
        }

        int text_w = recon_text_width(shell->font, MENU_POWER[i].label);
        recon_draw_text(menu, shell->font,
            bx + (button_w - 2 - text_w) / 2, by + (bh + ascent) / 2 - 2,
            button_w - 4, MENU_POWER[i].label,
            hovered ? COLOR_MENU_HILITE_TEXT : COLOR_MENU_TEXT);

        recon_hit_add(menu, bx, by, button_w - 2, bh, HIT_POWER_BASE + i);
    }

    recon_panel_commit(menu);
}

static void draw_security(struct recon_shell *shell) {
    struct recon_panel *p = shell->security;
    if (p == NULL) {
        return;
    }

    int width = recon_panel_width(p);
    int height = recon_panel_height(p);
    int ascent = recon_font_ascent(shell->font);

    recon_fill(p, COLOR_MENU);
    recon_hit_clear(p);

    recon_fill_rect(p, 0, 0, width, SEC_TITLE_HEIGHT, COLOR_DIALOG_TITLE);
    recon_draw_text(p, shell->font, SEC_PADDING,
        (SEC_TITLE_HEIGHT + ascent) / 2 - 1, width - SEC_PADDING * 2,
        "ReconOS Security", COLOR_DIALOG_TITLE_TEXT);

    recon_draw_text(p, shell->font, SEC_PADDING,
        SEC_TITLE_HEIGHT + SEC_PADDING + ascent,
        width - SEC_PADDING * 2, "What would you like to do?", COLOR_MENU_TEXT);

    int y = SEC_TITLE_HEIGHT + SEC_PADDING * 2 + 20;
    for (int i = 0; i < SEC_COUNT; i++) {
        int bw = width - SEC_PADDING * 2;
        bool hovered = (i == shell->security_hover);

        recon_fill_rect(p, SEC_PADDING, y, bw, SEC_BUTTON_HEIGHT,
            hovered ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON);
        recon_draw_bevel(p, SEC_PADDING, y, bw, SEC_BUTTON_HEIGHT, false);
        recon_draw_text(p, shell->font, SEC_PADDING + 12,
            y + (SEC_BUTTON_HEIGHT + ascent) / 2 - 2, bw - 24,
            SEC_ITEMS[i], COLOR_MENU_TEXT);
        recon_hit_add(p, SEC_PADDING, y, bw, SEC_BUTTON_HEIGHT, HIT_SEC_BASE + i);
        y += SEC_BUTTON_HEIGHT + SEC_PADDING;
    }

    recon_draw_bevel(p, 0, 0, width, height, false);
    recon_stroke_rect(p, 0, 0, width, height, COLOR_MENU_BORDER);
    recon_panel_commit(p);
}

/* --- Layout --- */

static void layout(struct recon_shell *shell) {
    if (shell->taskbar != NULL) {
        recon_panel_resize(shell->taskbar, shell->screen_width, TASKBAR_HEIGHT);
        recon_panel_set_position(shell->taskbar, 0,
            shell->screen_height - TASKBAR_HEIGHT);
    }
    if (shell->menu != NULL) {
        recon_panel_set_position(shell->menu, TASKBAR_PADDING,
            shell->screen_height - TASKBAR_HEIGHT - menu_height());
    }
    if (shell->dim != NULL) {
        recon_panel_resize(shell->dim, shell->screen_width, shell->screen_height);
        recon_panel_set_position(shell->dim, 0, 0);
        recon_fill(shell->dim, COLOR_DIM);
        recon_panel_commit(shell->dim);
    }
    if (shell->security != NULL) {
        /* Centred: it is a question that interrupts, not a corner notice. */
        recon_panel_set_position(shell->security,
            (shell->screen_width - SEC_WIDTH) / 2,
            (shell->screen_height - security_height()) / 2);
    }
    for (int i = 0; i < shell->app_count; i++) {
        recon_appwin_screen_changed(shell->apps[i], shell->screen_width,
            shell->screen_height, TASKBAR_HEIGHT);
    }
    recon_desktop_resize(shell->desktop, shell->screen_width,
        shell->screen_height - TASKBAR_HEIGHT);
    recon_session_resize(shell->session, shell->screen_width,
        shell->screen_height);
}

/* --- Lifecycle --- */

struct recon_shell *recon_shell_create(struct recon_server *server,
        int screen_width, int screen_height) {
    struct recon_shell *shell = calloc(1, sizeof(*shell));
    if (shell == NULL) {
        return NULL;
    }

    shell->server = server;
    shell->focused_app = -1;
    shell->screen_width = screen_width;
    shell->screen_height = screen_height;

    /* No font means no labels, but the taskbar still works as buttons. */
    shell->font = recon_font_load(getenv("RECONOS_FONT"), FONT_HEIGHT);

    shell->taskbar = recon_panel_create(&server->scene->tree,
        screen_width, TASKBAR_HEIGHT);
    if (shell->taskbar == NULL) {
        wlr_log(WLR_ERROR, "ReconOS: could not create the taskbar");
        recon_font_destroy(shell->font);
        free(shell);
        return NULL;
    }

    shell->menu = recon_panel_create(&server->scene->tree, MENU_WIDTH, menu_height());
    if (shell->menu != NULL) {
        recon_panel_set_enabled(shell->menu, false);
        draw_menu(shell);
    }

    shell->dim = recon_panel_create(&server->scene->tree,
        screen_width, screen_height);
    if (shell->dim != NULL) {
        recon_panel_set_enabled(shell->dim, false);
    }

    shell->dialog = recon_panel_create(&server->scene->tree, DIALOG_WIDTH, 200);
    if (shell->dialog != NULL) {
        recon_panel_set_enabled(shell->dialog, false);
    }
    shell->dialog_hover = -1;

    shell->context = recon_panel_create(&server->scene->tree,
        CONTEXT_WIDTH, CONTEXT_ITEM_HEIGHT * CONTEXT_ITEMS_MAX);
    if (shell->context != NULL) {
        recon_panel_set_enabled(shell->context, false);
    }

    shell->security = recon_panel_create(&server->scene->tree,
        SEC_WIDTH, security_height());
    if (shell->security != NULL) {
        recon_panel_set_enabled(shell->security, false);
        draw_security(shell);
    }

    /*
     * The built-in applications are registered, not constructed. Their windows
     * are built the first time somebody opens one, so an application nobody
     * touches costs an entry in a list rather than a window's worth of pixels.
     *
     * They go through recon_register_builtin_app, which is recon_register_app
     * with the contributing module left empty -- the same registry a module
     * adds to, so the path a module takes is the path the system takes.
     */
    static const struct recon_app_registration BUILTIN_APPS[] = {
        { "File Explorer", RECON_ICON_EXPLORER, recon_explorer_create, true },
        { "Terminal", RECON_ICON_TERMINAL, recon_terminal_create, true },
        { "Notepad", RECON_ICON_NOTEPAD, recon_notepad_create, true },
        { "Watchtower", RECON_ICON_TASKMGR, recon_taskmgr_create, true },
        /*
         * Not in the applications column: it is reached from the right of the
         * Start menu, where the things that configure the machine live. Listing
         * it in both would put the same name in two places, and anything
         * looking one up by name would find whichever came first.
         */
        { "Control Panel", RECON_ICON_SYSTEM, recon_control_panel_create, false },
    };

    for (size_t i = 0; i < sizeof(BUILTIN_APPS) / sizeof(BUILTIN_APPS[0]); i++) {
        if (!recon_register_builtin_app(&BUILTIN_APPS[i])) {
            wlr_log(WLR_ERROR, "ReconOS: could not register '%s'",
                BUILTIN_APPS[i].name);
        }
    }

    for (int i = 0; i < shell->app_count; i++) {
        recon_appwin_screen_changed(shell->apps[i], screen_width, screen_height,
            TASKBAR_HEIGHT);
    }

    /* Shortcuts for the native applications, written on first run only, so
     * removing one stays removed. */
    static const struct { const char *file; const char *target; } DEFAULTS[] = {
        { "File Explorer.app", "File Explorer" },
        { "Terminal.app", "Terminal" },
        { "Notepad.app", "Notepad" },
    };
    /*
     * Whether this account's desktop has been set up is a fact about how the
     * system is being used, which is what the registry is for. It was a
     * hidden marker file, and a marker file is a setting with no name, no
     * type and nowhere to look for it -- which is how the one that was
     * written ended up somewhere nothing checked.
     *
     * The old file is still honoured, so an account set up before this
     * change does not have its deleted shortcuts handed back.
     */
    char marker[RECON_PATH_MAX];
    snprintf(marker, sizeof(marker), "%s/.desktop-set-up", recon_fs_user_dir(NULL));

    bool already_set_up =
        recon_registry_get_bool(RECON_REG_USER, "desktop/set-up", false) ||
        recon_fs_exists("/", marker);

    if (!already_set_up) {
        for (size_t i = 0; i < sizeof(DEFAULTS) / sizeof(DEFAULTS[0]); i++) {
            char path[RECON_PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", recon_fs_user_dir("Desktop"),
                DEFAULTS[i].file);
            char body[RECON_NAME_MAX + 2];
            int length = snprintf(body, sizeof(body), "%s\n", DEFAULTS[i].target);
            recon_fs_write("/", path, body, (size_t)length);
        }
        recon_registry_set_bool(RECON_REG_USER, "desktop/set-up", true);
    }

    shell->context_button = -1;
    shell->context_app = -1;
    /* Not an id, so nothing matches it. */
    shell->menu_hover = -1;
    shell->context_hover = -1;
    shell->security_hover = -1;
    shell->desktop = recon_desktop_create(server, shell->font,
        screen_width, screen_height - TASKBAR_HEIGHT);

    /*
     * Last, so it is above everything the shell has just made. It stays
     * hidden until asked to show itself.
     */
    shell->session = recon_session_create(server, shell->font,
        screen_width, screen_height);

    layout(shell);
    draw_taskbar(shell);

    /* Debug aid: open and maximize a window at startup, so the state that
     * shows the rendering fault can be reached without a person clicking. */
    const char *autostart = getenv("RECONOS_DEBUG_AUTOSTART");
    if (autostart != NULL && strcmp(autostart, "taskmgr-max") == 0) {
        recon_shell_open_named(shell, "Watchtower");
        struct recon_appwin *win = recon_installed_app_existing("Watchtower");
        if (win != NULL) {
            recon_appwin_set_maximized(win, true);
        }
    }

    wlr_log(WLR_INFO, "ReconOS: shell up, taskbar %dx%d",
        screen_width, TASKBAR_HEIGHT);
    return shell;
}

void recon_shell_destroy(struct recon_shell *shell) {
    if (shell == NULL) {
        return;
    }
    for (int i = 0; i < shell->app_count; i++) {
        recon_appwin_destroy(shell->apps[i]);
    }
    recon_desktop_destroy(shell->desktop);
    recon_session_destroy(shell->session);
    recon_panel_destroy(shell->dialog);
    recon_panel_destroy(shell->context);
    recon_panel_destroy(shell->security);
    recon_panel_destroy(shell->dim);
    recon_panel_destroy(shell->menu);
    recon_panel_destroy(shell->taskbar);
    recon_font_destroy(shell->font);
    free(shell);
}

void recon_shell_resize(struct recon_shell *shell, int screen_width, int screen_height) {
    if (shell == NULL) {
        return;
    }
    shell->screen_width = screen_width;
    shell->screen_height = screen_height;
    layout(shell);
    draw_taskbar(shell);
}

void recon_shell_refresh(struct recon_shell *shell) {
    if (shell != NULL) {
        recon_desktop_reload(shell->desktop);
        draw_taskbar(shell);
        recon_damage_all(shell->server);
    }
}

int recon_shell_reserved_bottom(struct recon_shell *shell) {
    return shell != NULL ? TASKBAR_HEIGHT : 0;
}

void recon_shell_raise(struct recon_shell *shell) {
    if (shell == NULL) {
        return;
    }
    /* The desktop stays at the bottom, above only the wallpaper. */
    recon_desktop_lower(shell->desktop, shell->server->background_buffer != NULL
        ? &shell->server->background_buffer->node
        : (shell->server->background_rect != NULL
            ? &shell->server->background_rect->node : NULL));
    recon_panel_raise_to_top(shell->taskbar);
    if (shell->menu_open) {
        recon_panel_raise_to_top(shell->menu);
    }
    /* The task manager and the security box sit above everything, including
     * the taskbar: they are how you regain control when something else has
     * taken over the screen. */
    /*
     * Built-in windows are deliberately not raised here. They are ordinary
     * windows and should stack by use like any other; raising them whenever
     * the shell was raised pinned them above everything, so a client window
     * could never be brought in front of the task manager.
     */
    if (shell->security_open) {
        recon_panel_raise_to_top(shell->dim);
        recon_panel_raise_to_top(shell->security);
    }
}

void recon_shell_open_taskmgr(struct recon_shell *shell) {
    if (shell == NULL) {
        return;
    }
    recon_shell_close_menu(shell);
    /* No longer a special case: it is an application like the others, and
     * opening it by name is what makes it possible for it to become a module
     * later without this having to change. */
    recon_shell_open_named(shell, "Watchtower");
}

void recon_shell_restyle(struct recon_shell *shell) {
    if (shell == NULL) {
        return;
    }

    /* Everything the shell draws itself. */
    draw_taskbar(shell);
    draw_menu(shell);
    draw_context(shell);
    draw_security(shell);
    draw_dialog(shell);

    if (shell->dim != NULL) {
        recon_fill(shell->dim, COLOR_DIM);
        recon_panel_commit(shell->dim);
    }

    /* Then every window, including the ones nobody is looking at: a window
     * that is minimized now will be restored later, and should not come back
     * wearing the old skin. */
    for (int i = 0; i < shell->app_count; i++) {
        recon_appwin_refresh(shell->apps[i]);
    }

    recon_desktop_reload(shell->desktop);
    recon_damage_all(shell->server);
}

/*
 * Show or hide everything the desktop is made of.
 *
 * Called when the login screen goes up or comes down. The windows are left
 * alone -- signing out is not shutting down, and a half-written note should
 * still be there afterwards.
 */
static void set_desktop_visible(struct recon_shell *shell, bool visible) {
    if (shell->taskbar != NULL) {
        recon_panel_set_enabled(shell->taskbar, visible);
    }
    if (!visible) {
        recon_shell_close_menu(shell);
        recon_shell_close_context(shell);
    }

    struct wlr_scene_node *desktop = recon_desktop_node(shell->desktop);
    if (desktop != NULL) {
        wlr_scene_node_set_enabled(desktop, visible);
    }

    for (int i = 0; i < shell->app_count; i++) {
        struct wlr_scene_node *node = recon_appwin_node(shell->apps[i]);
        if (node == NULL) {
            continue;
        }
        /* A window that was minimized stays hidden when the desktop comes
         * back: signing in should not open things nobody opened. */
        bool showing = visible && recon_appwin_is_open(shell->apps[i]) &&
            !recon_appwin_is_minimized(shell->apps[i]);
        wlr_scene_node_set_enabled(node, showing);
    }

    recon_damage_all(shell->server);
}

/*
 * Everything that depends on *which* account signed in.
 *
 * The skin, the reading settings and the desktop folder all belong to a
 * person, so they are read now rather than at startup -- at startup there is
 * nobody to read them for.
 */
static void adopt_signed_in_user(struct recon_shell *shell) {
    const char *who = recon_users_current();

    /*
     * A different person means a clean desktop. Leaving the windows would
     * show one account the other's open documents, which is the one thing a
     * login screen exists to prevent.
     *
     * Destroyed rather than hidden. Hiding them looked equivalent and was
     * not: an application's window is built once and handed out again on
     * every open, so the next person clicking File Explorer was given the
     * previous person's -- hidden, then shown again, still sitting in their
     * folder. A window carries the state of whoever was using it, so the way
     * to stop it carrying that across a login is to end it.
     */
    if (who != NULL && shell->last_user[0] != '\0' &&
            strcmp(shell->last_user, who) != 0) {
        for (int i = 0; i < shell->app_count; i++) {
            recon_appwin_destroy(shell->apps[i]);
            shell->apps[i] = NULL;
        }
        shell->app_count = 0;
        /* The registry hands these out; it must not keep handing out ones
         * that no longer exist. */
        recon_installed_apps_forget_windows();
        set_focused_app(shell, -1);
        recon_fs_clip_clear();
    }
    snprintf(shell->last_user, sizeof(shell->last_user), "%s",
        who != NULL ? who : "");

    /*
     * Before anything reads a user setting. The skin, the spacing and the
     * folder the explorer opens at all live in this account's hive, and until
     * it is re-read they are still the last account's.
     */
    recon_registry_reload_user();

    recon_theme_init();
    recon_access_apply(shell->font);

    set_desktop_visible(shell, true);
    recon_desktop_reload(shell->desktop);
    recon_shell_restyle(shell);
}

void recon_shell_begin_session(struct recon_shell *shell) {
    if (shell == NULL || shell->session == NULL) {
        return;
    }
    /* Hidden first, so the desktop is not visible for a frame behind the
     * thing meant to be covering it. */
    set_desktop_visible(shell, false);
    recon_session_begin(shell->session);
}

void recon_shell_sign_out(struct recon_shell *shell) {
    if (shell == NULL || shell->session == NULL) {
        return;
    }
    set_focused_app(shell, -1);
    set_desktop_visible(shell, false);
    recon_session_lock(shell->session);
}

void recon_shell_lock(struct recon_shell *shell) {
    if (shell == NULL || shell->session == NULL) {
        return;
    }
    /* The windows are not touched. Locking is not signing out: coming back
     * should be the same desktop, in the same state, where it was left. */
    set_focused_app(shell, -1);
    set_desktop_visible(shell, false);
    recon_session_lock_screen(shell->session);
}

bool recon_shell_session_active(struct recon_shell *shell) {
    return shell != NULL && recon_session_active(shell->session);
}

void recon_shell_describe_session(struct recon_shell *shell,
        char *out, size_t size) {
    if (shell == NULL || out == NULL || size == 0) {
        return;
    }
    if (!recon_session_active(shell->session)) {
        const char *who = recon_users_current();
        snprintf(out, size,
            "session: done, the desktop has the screen\n  signed in: %s\n",
            who != NULL ? who : "(nobody)");
        return;
    }
    recon_session_describe(shell->session, out, size);
}

struct recon_font *recon_shell_font(struct recon_shell *shell) {
    return shell != NULL ? shell->font : NULL;
}

int recon_shell_app_count(struct recon_shell *shell) {
    return shell != NULL ? shell->app_count : 0;
}

struct recon_appwin *recon_shell_app_at(struct recon_shell *shell, int index) {
    if (shell == NULL || index < 0 || index >= shell->app_count) {
        return NULL;
    }
    return shell->apps[index];
}

int recon_shell_app_index(struct recon_shell *shell, const char *title) {
    if (shell == NULL || title == NULL) {
        return -1;
    }

    /*
     * By registry name first, then by window title. The two differ once an
     * application renames its window -- the notepad showing the file it is
     * editing -- and the registry name is the stable one.
     */
    struct recon_appwin *win = recon_installed_app_existing(title);
    if (win != NULL) {
        for (int i = 0; i < shell->app_count; i++) {
            if (shell->apps[i] == win) {
                return i;
            }
        }
    }

    for (int i = 0; i < shell->app_count; i++) {
        if (strcmp(recon_appwin_title(shell->apps[i]), title) == 0) {
            return i;
        }
    }
    return -1;
}

/*
 * Where a window sits in the shell's list, adding it if it is new.
 *
 * The shell's array is the windows that exist, which is a different list from
 * the applications that could exist. A window joins it the first time its
 * application is opened.
 */
static int adopt_window(struct recon_shell *shell, struct recon_appwin *win) {
    if (win == NULL) {
        return -1;
    }
    for (int i = 0; i < shell->app_count; i++) {
        if (shell->apps[i] == win) {
            return i;
        }
    }
    if (shell->app_count >= (int)(sizeof(shell->apps) / sizeof(shell->apps[0]))) {
        wlr_log(WLR_ERROR, "ReconOS: no room for another window");
        return -1;
    }

    int index = shell->app_count;
    shell->app_order[index] = index;
    shell->apps[index] = win;
    shell->app_count++;

    /* A window that has just appeared needs placing for the current screen;
     * it was not there when the last resize went round. */
    recon_appwin_screen_changed(win, shell->screen_width, shell->screen_height,
        TASKBAR_HEIGHT);
    return index;
}

void recon_shell_open_named(struct recon_shell *shell, const char *title) {
    if (shell == NULL || title == NULL) {
        return;
    }

    /* Built on demand if this is the first time. */
    struct recon_appwin *win = recon_installed_app_window(title);
    if (win == NULL) {
        wlr_log(WLR_ERROR, "ReconOS: cannot open '%s': %s", title,
            recon_modules_last_error());
        return;
    }

    int index = adopt_window(shell, win);
    if (index >= 0) {
        recon_shell_open_app(shell, index);
    }
}

const char *recon_shell_icon_for_app(struct recon_shell *shell, const char *title) {
    (void)shell;
    if (title == NULL) {
        return NULL;
    }

    /*
     * Asked of the registry rather than of the open windows. Applications are
     * built the first time they are opened, so on a fresh desktop none of them
     * exists yet -- and a shortcut whose icon depends on the application
     * already running is a shortcut that looks broken until you use it.
     */
    const char *name = recon_installed_app_resolve(title);
    if (name == NULL) {
        return NULL;
    }

    int count = recon_installed_app_count();
    for (int i = 0; i < count; i++) {
        struct recon_installed_app app;
        if (recon_installed_app_at(i, &app) && strcmp(app.name, name) == 0) {
            return app.icon[0] != '\0' ? recon_installed_app_icon(name) : NULL;
        }
    }
    return NULL;
}

void recon_shell_open_app(struct recon_shell *shell, int index) {
    if (shell == NULL || index < 0 || index >= shell->app_count) {
        return;
    }
    recon_shell_close_menu(shell);
    raise_app_order(shell, index);
    set_focused_app(shell, index);
    recon_appwin_show(shell->apps[index]);
    recon_appwin_focus(shell->apps[index]);
    recon_shell_refresh(shell);
}

/* Open whatever a desktop item points at. */
static void perform_desktop_action(struct recon_shell *shell,
    const struct recon_desktop_action *action);

static void open_desktop_item(struct recon_shell *shell, const char *name) {
    struct recon_desktop_action action;
    if (!recon_desktop_action_for(shell->desktop, name, &action)) {
        return;
    }

    perform_desktop_action(shell, &action);
}

/*
 * Carry out what opening a desktop item asked for.
 *
 * One place, because there are two ways to open one -- clicking it and
 * choosing Open from its menu -- and they were drifting apart. Both used to
 * search the open windows for a title match, which stopped working the moment
 * applications began being built on demand: on a fresh desktop there are no
 * windows to search, so every shortcut silently did nothing.
 */
static void perform_desktop_action(struct recon_shell *shell,
        const struct recon_desktop_action *action) {
    if (action->kind == RECON_DESKTOP_ACTION_OPEN_APP) {
        recon_shell_open_named(shell, action->target);
        return;
    }

    if (action->kind == RECON_DESKTOP_ACTION_OPEN_PATH) {
        /*
         * Open the explorer *at* the folder. Opening it and leaving it
         * wherever it was looks exactly like the folder failing to open,
         * which is what it looked like.
         */
        recon_shell_open_named(shell, "File Explorer");
        recon_explorer_open_at(recon_installed_app_existing("File Explorer"),
            action->target);
    }
}

/* The answer to a question the shell asked about a desktop item. */
static void desktop_answered(void *user, int choice) {
    struct recon_shell *shell = user;

    int asked = shell->desktop_question;
    char name[RECON_NAME_MAX];
    snprintf(name, sizeof(name), "%s", shell->desktop_question_target);
    shell->desktop_question = DESKTOP_ASK_NONE;

    /* Button 0 goes ahead; anything else, Escape included, declines. */
    if (choice != 0) {
        return;
    }

    switch (asked) {
    case DESKTOP_ASK_TRASH:
        recon_desktop_delete(shell->desktop, name);
        break;
    case DESKTOP_ASK_PURGE:
        recon_desktop_purge(shell->desktop, name);
        break;
    case DESKTOP_ASK_EMPTY_BIN:
        recon_fs_trash_empty();
        recon_desktop_reload(shell->desktop);
        break;
    default:
        break;
    }

    recon_shell_refresh(shell);
}

static void ask_desktop(struct recon_shell *shell, int question, const char *name) {
    shell->desktop_question = question;
    snprintf(shell->desktop_question_target,
        sizeof(shell->desktop_question_target), "%s", name != NULL ? name : "");

    char message[320];
    const char *title;
    const char *go_ahead;

    if (question == DESKTOP_ASK_EMPTY_BIN) {
        int count = recon_fs_trash_count();
        if (count <= 0) {
            shell->desktop_question = DESKTOP_ASK_NONE;
            return;
        }
        title = "Empty Recycle Bin";
        go_ahead = "Empty";
        snprintf(message, sizeof(message),
            "Permanently delete %d item%s in the Recycle Bin? "
            "This cannot be undone.", count, count == 1 ? "" : "s");
    } else if (question == DESKTOP_ASK_PURGE) {
        title = "Delete Permanently";
        go_ahead = "Delete";
        snprintf(message, sizeof(message),
            "Permanently delete '%s'? This cannot be undone.", name);
    } else {
        title = "Delete";
        go_ahead = "Move";
        snprintf(message, sizeof(message), "Move '%s' to the Recycle Bin?", name);
    }

    /* Cancel last: it is what Enter and Escape both choose. */
    const char *buttons[2] = { go_ahead, "Cancel" };
    recon_shell_ask(shell, title, message, buttons, 2, desktop_answered, shell);
}

/* Carry out what a context menu entry asked for. */
static void context_activate(struct recon_shell *shell, uint32_t id) {
    /*
     * An application's entries are its own; the shell drew the menu but has no
     * idea what the choices mean, so it hands the id straight back.
     */
    if (shell->context_kind == RECON_CONTEXT_APP) {
        if (shell->context_app >= 0 && shell->context_app < shell->app_count) {
            recon_appwin_context_action(shell->apps[shell->context_app], id);
        }
        recon_shell_refresh(shell);
        return;
    }

    enum context_action action = (enum context_action)id;

    switch (shell->context_kind) {
    case RECON_CONTEXT_TASKBAR_WINDOW: {
        if (shell->context_button < 0 || shell->context_button >= shell->button_count) {
            return;
        }
        struct taskbar_button *button = &shell->buttons[shell->context_button];

        if (button->appwin != NULL) {
            switch (action) {
            case CTX_RESTORE:
                recon_appwin_restore(button->appwin);
                break;
            case CTX_MINIMIZE:
                recon_appwin_minimize(button->appwin);
                break;
            case CTX_MAXIMIZE:
                recon_appwin_set_maximized(button->appwin,
                    !recon_appwin_is_maximized(button->appwin));
                break;
            case CTX_CLOSE:
                recon_appwin_hide(button->appwin);
                break;
            default:
                break;
            }
        } else if (button->toplevel != NULL) {
            switch (action) {
            case CTX_RESTORE:
                recon_toplevel_restore(button->toplevel);
                break;
            case CTX_MINIMIZE:
                recon_toplevel_minimize(button->toplevel);
                break;
            case CTX_MAXIMIZE:
                recon_toplevel_toggle_maximized(button->toplevel);
                break;
            case CTX_CLOSE:
                recon_toplevel_close(button->toplevel);
                break;
            default:
                break;
            }
        }
        break;
    }

    case RECON_CONTEXT_WINDOW: {
        if (shell->context_app < 0 || shell->context_app >= shell->app_count) {
            return;
        }
        struct recon_appwin *win = shell->apps[shell->context_app];
        switch (action) {
        case CTX_RESTORE:
            recon_appwin_set_maximized(win, false);
            break;
        case CTX_MINIMIZE:
            recon_appwin_minimize(win);
            break;
        case CTX_MAXIMIZE:
            recon_appwin_set_maximized(win, true);
            break;
        case CTX_CLOSE:
            recon_appwin_hide(win);
            break;
        default:
            break;
        }
        break;
    }

    case RECON_CONTEXT_DESKTOP_ITEM:
        switch (action) {
        case CTX_OPEN:
            open_desktop_item(shell, shell->context_target);
            break;

        case CTX_RENAME:
            if (recon_desktop_is_trash_item(shell->context_target)) {
                break; /* The bin is part of the system, not a file. */
            }
            /* The desktop takes the keyboard while a name is being typed, so
             * nothing else may hold focus. */
            set_focused_app(shell, -1);
            recon_desktop_begin_rename(shell->desktop, shell->context_target);
            break;

        case CTX_CUT:
            if (!recon_desktop_is_trash_item(shell->context_target)) {
                recon_desktop_clip(shell->desktop, shell->context_target, true);
            }
            break;
        case CTX_COPY:
            if (!recon_desktop_is_trash_item(shell->context_target)) {
                recon_desktop_clip(shell->desktop, shell->context_target, false);
            }
            break;

        case CTX_DELETE:
        case CTX_DELETE_TREE:
            ask_desktop(shell, DESKTOP_ASK_TRASH, shell->context_target);
            break;

        case CTX_PURGE:
            ask_desktop(shell, DESKTOP_ASK_PURGE, shell->context_target);
            break;

        case CTX_EMPTY_BIN:
            ask_desktop(shell, DESKTOP_ASK_EMPTY_BIN, NULL);
            break;

        default:
            break;
        }
        break;

    case RECON_CONTEXT_TASKBAR:
        if (action == CTX_TASK_MANAGER) {
            recon_shell_open_named(shell, "Watchtower");
        } else if (action == CTX_SHOW_DESKTOP) {
            /*
             * Minimize everything rather than hiding it: the windows are
             * still open and still on the taskbar, which is what makes this
             * reversible by clicking one of them.
             */
            for (int i = 0; i < shell->app_count; i++) {
                if (recon_appwin_is_open(shell->apps[i]) &&
                        !recon_appwin_is_minimized(shell->apps[i])) {
                    recon_appwin_minimize(shell->apps[i]);
                }
            }
            struct recon_toplevel *toplevel;
            wl_list_for_each(toplevel, &shell->server->toplevels, link) {
                if (!recon_toplevel_is_minimized(toplevel)) {
                    recon_toplevel_minimize(toplevel);
                }
            }
            set_focused_app(shell, -1);
        } else if (action == CTX_REFRESH) {
            recon_desktop_reload(shell->desktop);
        }
        break;

    case RECON_CONTEXT_APP:
        break; /* Handled above. */

    case RECON_CONTEXT_DESKTOP:
        if (action == CTX_NEW_FOLDER) {
            set_focused_app(shell, -1);
            recon_desktop_new_folder(shell->desktop);
        } else if (action == CTX_NEW_FILE) {
            set_focused_app(shell, -1);
            recon_desktop_new_file(shell->desktop);
        } else if (action == CTX_NEW_SHORTCUT) {
            set_focused_app(shell, -1);
            recon_desktop_new_shortcut(shell->desktop);
        } else if (action == CTX_PASTE) {
            recon_desktop_paste(shell->desktop);
        } else if (action == CTX_REFRESH) {
            recon_desktop_reload(shell->desktop);
        }
        break;
    }

    recon_shell_refresh(shell);
}

bool recon_shell_handle_right_click(struct recon_shell *shell, double lx, double ly) {
    if (shell == NULL) {
        return false;
    }

    /* No menus while a question is up: it must be answered first. */
    if (shell->dialog_open) {
        return true;
    }

    /* Nothing behind the login screen has a menu worth opening. */
    if (recon_session_active(shell->session)) {
        return true;
    }

    recon_shell_close_menu(shell);
    recon_shell_close_context(shell);
    shell->context_item_count = 0;
    shell->context_target[0] = 0;

    int px, py;

    /* A window button on the taskbar. */
    if (shell->taskbar != NULL && point_in_panel(shell->taskbar, lx, ly, &px, &py)) {
        uint32_t hit = recon_hit_test(shell->taskbar, px, py);
        int index = (hit >= HIT_TASK_BASE && hit < HIT_MENU_BASE)
            ? (int)(hit - HIT_TASK_BASE) : -1;

        /*
         * Empty taskbar has its own menu rather than no menu. Right-clicking
         * the bar and getting nothing reads as broken, and the things worth
         * offering there -- the task manager, clearing the screen -- have
         * nowhere else obvious to live.
         */
        if (index < 0 || index >= shell->button_count) {
            shell->context_kind = RECON_CONTEXT_TASKBAR;
            context_add(shell, "Watchtower", CTX_TASK_MANAGER, true, false);
            context_add(shell, "Show Desktop", CTX_SHOW_DESKTOP, true, true);
            context_add(shell, "Refresh", CTX_REFRESH, true, false);
            context_show(shell, lx, ly);
            return true;
        }

        shell->context_kind = RECON_CONTEXT_TASKBAR_WINDOW;
        shell->context_button = index;

        struct taskbar_button *button = &shell->buttons[index];
        bool builtin = (button->appwin != NULL);
        bool minimized = builtin
            ? recon_appwin_is_minimized(button->appwin)
            : recon_toplevel_is_minimized(button->toplevel);

        context_add(shell, "Restore", CTX_RESTORE, minimized, false);
        context_add(shell, "Minimize", CTX_MINIMIZE, !minimized, false);
        context_add(shell, "Maximize", CTX_MAXIMIZE, true, true);
        context_add(shell, "Close", CTX_CLOSE, true, false);
        context_show(shell, lx, ly);
        return true;
    }

    struct wlr_scene_node *node = topmost_node(shell, lx, ly);

    /*
     * A window under the pointer offers what can be done to the window. Right
     * click should answer everywhere rather than only in the two places that
     * happen to have something interesting to say.
     */
    int app_index = appwin_index_for_node(shell, node);
    if (app_index >= 0) {
        struct recon_appwin *win = shell->apps[app_index];

        /*
         * The application gets first refusal. Right-clicking a file should
         * offer things to do with the file; only when the application has
         * nothing to say there -- empty space, its own background -- does the
         * window's own menu make sense.
         */
        struct recon_menu_spec spec;
        if (recon_appwin_context_at(win, lx, ly, &spec)) {
            /*
             * Focus follows the menu. Choosing "Rename" opens a text box, and
             * a text box in an unfocused window would sit there taking no
             * keys -- the feature would look broken rather than unfocused.
             */
            set_focused_app(shell, app_index);
            shell->context_kind = RECON_CONTEXT_APP;
            shell->context_app = app_index;
            for (int i = 0; i < spec.count; i++) {
                context_add_id(shell, spec.items[i].label, spec.items[i].id,
                    spec.items[i].enabled, spec.items[i].separator_after);
            }
            context_show(shell, lx, ly);
            return true;
        }

        shell->context_kind = RECON_CONTEXT_WINDOW;
        shell->context_app = app_index;

        context_add(shell, "Restore", CTX_RESTORE,
            recon_appwin_is_maximized(win), false);
        context_add(shell, "Minimize", CTX_MINIMIZE, true, false);
        context_add(shell, "Maximize", CTX_MAXIMIZE,
            !recon_appwin_is_maximized(win), true);
        context_add(shell, "Close", CTX_CLOSE, true, false);
        context_show(shell, lx, ly);
        return true;
    }

    /* The desktop, on an icon or on empty space. */
    if (node == recon_desktop_node(shell->desktop)) {
        const char *name = recon_desktop_item_at(shell->desktop, lx, ly);
        if (name != NULL) {
            shell->context_kind = RECON_CONTEXT_DESKTOP_ITEM;
            snprintf(shell->context_target, sizeof(shell->context_target), "%s", name);

            /*
             * Emptying a folder is asked for by name, so the menu says what it
             * is about to remove rather than offering a bare "Delete" that
             * turns out to take a tree with it.
             */
            char path[RECON_PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s",
                recon_fs_user_dir("Desktop"), name);

            struct recon_dirent info;
            bool is_dir = recon_fs_stat("/", path, &info) &&
                info.kind == RECON_FILE_DIRECTORY;
            bool has_contents = is_dir &&
                recon_fs_list("/", path, NULL, 0) > 0;

            /*
             * The recycle bin is not a file, so most of what can be done to a
             * desktop icon makes no sense for it. It gets its own menu rather
             * than a menu of things that would fail.
             */
            if (recon_desktop_is_trash_item(name)) {
                int count = recon_fs_trash_count();
                context_add(shell, "Open", CTX_OPEN, true, true);
                context_add(shell, "Empty Recycle Bin", CTX_EMPTY_BIN,
                    count > 0, true);
                context_add(shell, "Properties", CTX_PROPERTIES, false, false);
                context_show(shell, lx, ly);
                return true;
            }

            context_add(shell, "Open", CTX_OPEN, true, true);
            context_add(shell, "Cut", CTX_CUT, true, false);
            context_add(shell, "Copy", CTX_COPY, true, true);
            context_add(shell, "Rename", CTX_RENAME, true, false);
            context_add(shell, "Delete", CTX_DELETE, true, false);
            context_add(shell, "Delete Permanently", CTX_PURGE, true, true);
            (void)has_contents;
            /* Shown but unavailable: there is no properties view yet, and
             * hiding it would suggest there never will be. */
            context_add(shell, "Properties", CTX_PROPERTIES, false, false);
        } else {
            shell->context_kind = RECON_CONTEXT_DESKTOP;
            context_add(shell, "New Folder", CTX_NEW_FOLDER, true, false);
            context_add(shell, "New File", CTX_NEW_FILE, true, false);
            context_add(shell, "New Shortcut", CTX_NEW_SHORTCUT, true, true);
            context_add(shell, "Paste", CTX_PASTE, !recon_fs_clip_empty(), true);
            context_add(shell, "Empty Recycle Bin", CTX_EMPTY_BIN,
                recon_fs_trash_count() > 0, true);
            context_add(shell, "Refresh", CTX_REFRESH, true, false);
        }
        context_show(shell, lx, ly);
        return true;
    }

    return false;
}

bool recon_shell_handle_key(struct recon_shell *shell, uint32_t sym,
        uint32_t modifiers) {
    if (shell == NULL) {
        return false;
    }

    if (recon_session_active(shell->session)) {
        bool handled = recon_session_handle_key(shell->session, sym, modifiers);
        if (recon_session_take_signed_in(shell->session)) {
            adopt_signed_in_user(shell);
        }
        return handled;
    }

    /* A question owns the keyboard while it is up. */
    if (shell->dialog_open) {
        if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
            dialog_finish(shell, shell->dialog_default);
        } else if (sym == XKB_KEY_Escape) {
            dialog_finish(shell, shell->dialog_cancel);
        } else if (sym == XKB_KEY_Left || sym == XKB_KEY_Right ||
                sym == XKB_KEY_Tab) {
            int step = (sym == XKB_KEY_Left) ? -1 : 1;
            shell->dialog_default =
                (shell->dialog_default + step + shell->dialog_button_count) %
                shell->dialog_button_count;
            draw_dialog(shell);
            recon_damage_all(shell->server);
        }
        return true;
    }

    /*
     * A name being typed on the desktop takes the keyboard. It only exists
     * because the user clicked the desktop, so nothing else is expecting these
     * keys -- and without this, Enter and Escape would never reach the box and
     * it could not be closed.
     */
    if (recon_desktop_is_renaming(shell->desktop) &&
            recon_desktop_handle_key(shell->desktop, sym, modifiers)) {
        return true;
    }

    if (shell->focused_app < 0) {
        return false;
    }
    /* Only the focused window, so a calculator sitting open in the background
     * cannot swallow digits meant for the notepad. */
    return recon_appwin_handle_key(shell->apps[shell->focused_app], sym, modifiers);
}

void recon_shell_clear_app_focus(struct recon_shell *shell) {
    if (shell != NULL) {
        set_focused_app(shell, -1);
    }
}

bool recon_shell_security_open(struct recon_shell *shell) {
    return shell != NULL && shell->security_open;
}

void recon_shell_toggle_security(struct recon_shell *shell) {
    if (shell == NULL || shell->security == NULL) {
        return;
    }
    shell->security_open = !shell->security_open;
    recon_panel_set_enabled(shell->dim, shell->security_open);
    recon_panel_set_enabled(shell->security, shell->security_open);
    if (shell->security_open) {
        recon_shell_close_menu(shell);
        /* Dim first, then the box on top of it. */
        recon_panel_raise_to_top(shell->dim);
        recon_panel_raise_to_top(shell->security);
    }
    recon_damage_all(shell->server);
}

/*
 * Follow the pointer across whichever menu is open, redrawing only when the
 * highlighted entry actually changes -- moving within one entry should not
 * cost a repaint.
 */
static void update_hover(struct recon_shell *shell, double lx, double ly) {
    int px, py;

    if (shell->dialog_open) {
        int hover = -1;
        if (point_in_panel(shell->dialog, lx, ly, &px, &py)) {
            uint32_t hit = recon_hit_test(shell->dialog, px, py);
            if (hit >= HIT_DIALOG_BASE) {
                hover = (int)(hit - HIT_DIALOG_BASE);
            }
        }
        if (hover != shell->dialog_hover) {
            shell->dialog_hover = hover;
            draw_dialog(shell);
            recon_damage_all(shell->server);
        }
        return;
    }

    int menu = -1;
    int context = -1;
    int security = -1;

    if (shell->context_open && shell->context != NULL &&
            point_in_panel(shell->context, lx, ly, &px, &py)) {
        uint32_t hit = recon_hit_test(shell->context, px, py);
        if (hit >= HIT_CONTEXT_BASE) {
            context = (int)(hit - HIT_CONTEXT_BASE);
        }
    } else if (shell->menu_open && shell->menu != NULL &&
            point_in_panel(shell->menu, lx, ly, &px, &py)) {
        uint32_t hit = recon_hit_test(shell->menu, px, py);
        if (hit >= HIT_MENU_BASE) {
            /* The raw id, not an index. The menu has three ranges in it now --
             * applications, places, and what to do with the machine -- and an
             * index relative to one of them cannot say which. */
            menu = (int)hit;
        }
    } else if (shell->security_open && shell->security != NULL &&
            point_in_panel(shell->security, lx, ly, &px, &py)) {
        uint32_t hit = recon_hit_test(shell->security, px, py);
        if (hit >= HIT_SEC_BASE) {
            security = (int)(hit - HIT_SEC_BASE);
        }
    }

    if (context != shell->context_hover) {
        shell->context_hover = context;
        if (shell->context_open) {
            draw_context(shell);
            recon_damage_all(shell->server);
        }
    }
    if (menu != shell->menu_hover) {
        shell->menu_hover = menu;
        if (shell->menu_open) {
            draw_menu(shell);
            recon_damage_all(shell->server);
        }
    }
    if (security != shell->security_hover) {
        shell->security_hover = security;
        if (shell->security_open) {
            draw_security(shell);
            recon_damage_all(shell->server);
        }
    }
}

void recon_shell_handle_motion(struct recon_shell *shell, double lx, double ly) {
    if (shell == NULL) {
        return;
    }
    if (recon_session_handle_motion(shell->session, lx, ly)) {
        return;
    }
    update_hover(shell, lx, ly);
    for (int i = 0; i < shell->app_count; i++) {
        recon_appwin_handle_motion(shell->apps[i], lx, ly);
    }
}

const char *recon_shell_cursor_at(struct recon_shell *shell, double lx, double ly) {
    if (shell == NULL) {
        return NULL;
    }
    /* Ask in stacking order, so an edge hidden behind another window does not
     * claim the cursor. */
    for (int i = 0; i < shell->app_count; i++) {
        const char *name =
            recon_appwin_cursor_at(shell->apps[shell->app_order[i]], lx, ly);
        if (name != NULL) {
            return name;
        }
    }
    return NULL;
}

bool recon_shell_handle_scroll(struct recon_shell *shell, double lx, double ly,
        double delta) {
    if (shell == NULL) {
        return false;
    }
    for (int i = 0; i < shell->app_count; i++) {
        if (recon_appwin_handle_scroll(shell->apps[shell->app_order[i]], lx, ly, delta)) {
            return true;
        }
    }
    return false;
}

void recon_shell_open_menu(struct recon_shell *shell) {
    if (shell == NULL || shell->menu_open) {
        return;
    }
    toggle_menu(shell);
    /*
     * Drawn as well as enabled. A panel's clickable regions are registered
     * while it is drawn, so a menu that has been switched on but not yet
     * painted has none -- which looks from outside like a menu with nothing
     * in it. Every other route to opening it goes through a redraw; this one
     * has to say so.
     */
    draw_menu(shell);
}

void recon_shell_close_menu(struct recon_shell *shell) {
    if (shell == NULL || !shell->menu_open) {
        return;
    }
    shell->menu_open = false;
    recon_panel_set_enabled(shell->menu, false);
    draw_taskbar(shell);
    recon_damage_all(shell->server);
}

static void toggle_menu(struct recon_shell *shell) {
    if (shell->menu == NULL) {
        return;
    }
    shell->menu_open = !shell->menu_open;
    recon_panel_set_enabled(shell->menu, shell->menu_open);
    if (shell->menu_open) {
        recon_panel_raise_to_top(shell->menu);
    }
    draw_taskbar(shell);
    recon_damage_all(shell->server);
}

/* --- Input --- */

/* Convert layout coordinates to panel-local, returning false if outside. */
static bool point_in_panel(struct recon_panel *panel, double lx, double ly,
        int *px, int *py) {
    struct wlr_scene_node *node = recon_panel_node(panel);
    if (node == NULL || !node->enabled) {
        return false;
    }

    int local_x = (int)lx - node->x;
    int local_y = (int)ly - node->y;
    if (local_x < 0 || local_y < 0 ||
            local_x >= recon_panel_width(panel) ||
            local_y >= recon_panel_height(panel)) {
        return false;
    }

    *px = local_x;
    *py = local_y;
    return true;
}

bool recon_shell_contains_point(struct recon_shell *shell, double lx, double ly) {
    if (shell == NULL) {
        return false;
    }
    int px, py;
    if (shell->menu_open && shell->menu != NULL &&
            point_in_panel(shell->menu, lx, ly, &px, &py)) {
        return true;
    }
    if (appwin_index_for_node(shell, topmost_node(shell, lx, ly)) >= 0) {
        return true;
    }
    if (shell->security_open && shell->security != NULL &&
            point_in_panel(shell->security, lx, ly, &px, &py)) {
        return true;
    }
    if (shell->menu_open && shell->menu != NULL &&
            point_in_panel(shell->menu, lx, ly, &px, &py)) {
        return true;
    }
    return shell->taskbar != NULL && point_in_panel(shell->taskbar, lx, ly, &px, &py);
}

bool recon_shell_handle_click(struct recon_shell *shell, double lx, double ly,
        bool pressed) {
    if (shell == NULL) {
        return false;
    }

    /*
     * Setup and the login screen come before everything, a question
     * included: there is no desktop behind them to have asked one.
     */
    if (recon_session_active(shell->session)) {
        bool handled = recon_session_handle_click(shell->session, lx, ly, pressed);
        if (recon_session_take_signed_in(shell->session)) {
            adopt_signed_in_user(shell);
        }
        return handled;
    }

    int px, py;

    /*
     * A question takes every click until it is answered, including the ones
     * that miss it. That is what makes it a question rather than a notice:
     * the thing being asked about must not change underneath the answer.
     */
    if (shell->dialog_open) {
        if (!pressed) {
            return true;
        }
        if (point_in_panel(shell->dialog, lx, ly, &px, &py)) {
            uint32_t hit = recon_hit_test(shell->dialog, px, py);
            if (hit >= HIT_DIALOG_BASE) {
                int choice = (int)(hit - HIT_DIALOG_BASE);
                if (choice >= 0 && choice < shell->dialog_button_count) {
                    dialog_finish(shell, choice);
                }
            }
        }
        /* Clicking outside does nothing at all -- not even dismiss. A
         * destructive question should be answered deliberately. */
        return true;
    }

    /* An open context menu takes the next click wherever it lands: either it
     * chose something, or it dismissed the menu. */
    if (shell->context_open) {
        if (!pressed) {
            return true;
        }
        if (point_in_panel(shell->context, lx, ly, &px, &py)) {
            uint32_t hit = recon_hit_test(shell->context, px, py);
            recon_shell_close_context(shell);
            if (hit >= HIT_CONTEXT_BASE) {
                int index = (int)(hit - HIT_CONTEXT_BASE);
                if (index >= 0 && index < shell->context_item_count) {
                    context_activate(shell, shell->context_items[index].id);
                }
            }
        } else {
            recon_shell_close_context(shell);
        }
        return true;
    }

    /* The security box is modal in spirit: while it is up it takes the click,
     * wherever the click landed. */
    if (shell->security_open && shell->security != NULL) {
        if (!pressed) {
            return true;
        }
        if (point_in_panel(shell->security, lx, ly, &px, &py)) {
            uint32_t hit = recon_hit_test(shell->security, px, py);
            if (hit >= HIT_SEC_BASE) {
                int index = (int)(hit - HIT_SEC_BASE);
                recon_shell_toggle_security(shell); /* closes it */
                if (index == SEC_TASKMGR) {
                    recon_shell_open_taskmgr(shell);
                } else if (index == SEC_SHUTDOWN) {
                    recon_quit(shell->server);
                }
                return true;
            }
        } else {
            /* Clicking outside dismisses it, like Cancel. */
            recon_shell_toggle_security(shell);
        }
        return true;
    }

    /*
     * Order here must match what is drawn on top of what. The apps menu is
     * raised above every window, so it has to be offered the click before
     * them: a maximized window covers the same pixels, and checking windows
     * first let it swallow clicks meant for the menu.
     */
    /* The menu sits above the bar, so it gets first refusal. */
    if (shell->menu_open && shell->menu != NULL &&
            point_in_panel(shell->menu, lx, ly, &px, &py)) {
        if (!pressed) {
            return true;
        }
        uint32_t hit = recon_hit_test(shell->menu, px, py);
        if (hit >= HIT_MENU_BASE) {
            /* The right column: a place to go, or something that
             * configures the machine. */
            if (hit >= HIT_POWER_BASE) {
                int which = (int)(hit - HIT_POWER_BASE);
                recon_shell_close_menu(shell);

                if (which >= 0 && which < MENU_POWER_COUNT) {
                    switch (MENU_POWER[which].action) {
                    case POWER_LOCK:
                        recon_shell_lock(shell);
                        break;
                    case POWER_SIGN_OUT:
                    case POWER_SWITCH_USER:
                        /*
                         * The same thing, honestly. With one session there is
                         * nothing to switch *between* -- signing out and
                         * signing in as somebody else is the whole of it, and
                         * the windows are closed when the person changes.
                         *
                         * Lock is the one that differs: it leaves the account
                         * signed in and its windows open.
                         */
                        recon_shell_sign_out(shell);
                        break;
                    case POWER_RESTART:
                        recon_restart(shell->server);
                        break;
                    case POWER_SHUT_DOWN:
                        recon_quit(shell->server);
                        break;
                    }
                }
                return true;
            }

            if (hit >= HIT_PLACE_BASE) {
                int which = (int)(hit - HIT_PLACE_BASE);
                recon_shell_close_menu(shell);

                if (which < 0 || which >= MENU_PLACE_COUNT) {
                    return true;
                }

                if (MENU_PLACES[which].kind == PLACE_APP) {
                    recon_shell_open_named(shell, MENU_PLACES[which].target);
                } else if (MENU_PLACES[which].kind == PLACE_FOLDER) {
                    /* A place opens where it is, not wherever the explorer
                     * happened to be left. A target beginning with a slash is
                     * an absolute path -- the root of the filesystem is a
                     * place too, and it is not inside anybody's folder. */
                    const char *target = MENU_PLACES[which].target;
                    const char *path = (target[0] == '/')
                        ? target : recon_fs_user_dir(target);

                    recon_shell_open_named(shell, "File Explorer");
                    recon_explorer_open_at(
                        recon_installed_app_existing("File Explorer"), path);
                }
                return true;
            }

            int index = (int)(hit - HIT_MENU_BASE);
            struct menu_entry entry;
            if (index >= 0 && menu_entry_at(index, &entry)) {
                /* Close the menu first: quitting never returns here. */
                recon_shell_close_menu(shell);
                if (entry.is_shutdown) {
                    recon_quit(shell->server);
                } else {
                    recon_shell_open_named(shell, entry.label);
                }
                return true;
            }
        }
        recon_shell_close_menu(shell);
        return true;
    }

    /*
     * The click did not land in the menu, so the menu is finished.
     *
     * Closed here rather than further down, where the old dismissal sat: by
     * then a click on a window had already been handled and returned, so
     * clicking a window left the menu standing over it until something else
     * was clicked. The Apps button is the exception -- it toggles the menu,
     * and closing it here first would make the toggle reopen it, so it could
     * never be closed by the button that opened it.
     */
    if (shell->menu_open && pressed) {
        bool on_apps_button = shell->taskbar != NULL &&
            point_in_panel(shell->taskbar, lx, ly, &px, &py) &&
            recon_hit_test(shell->taskbar, px, py) == HIT_APPS_BUTTON;
        if (!on_apps_button) {
            recon_shell_close_menu(shell);
            /*
             * Not consumed. Clicking a window while the menu is open both
             * dismisses the menu and raises the window, which is one click
             * for one intention rather than a click spent on dismissal.
             */
        }
    }

    /*
     * Then whichever built-in window the scene graph says is actually on top
     * here -- not merely one whose rectangle covers the point.
     */
    struct wlr_scene_node *node = topmost_node(shell, lx, ly);
    int index = appwin_index_for_node(shell, node);
    if (index >= 0) {
        if (recon_appwin_handle_click(shell->apps[index], lx, ly, pressed)) {
            if (pressed) {
                raise_app_order(shell, index);
                set_focused_app(shell, index);
            }
            recon_shell_refresh(shell);
            return true;
        }
    }

    /* A release still has to reach a window mid-drag, whose pointer may have
     * left it. */
    if (!pressed) {
        for (int i = 0; i < shell->app_count; i++) {
            if (recon_appwin_handle_click(shell->apps[shell->app_order[i]],
                    lx, ly, false)) {
                return true;
            }
        }
    }

    if (shell->taskbar != NULL && point_in_panel(shell->taskbar, lx, ly, &px, &py)) {
        if (!pressed) {
            return true;
        }
        uint32_t hit = recon_hit_test(shell->taskbar, px, py);
        wlr_log(WLR_DEBUG, "ReconOS: taskbar click at %d,%d -> hit %u (%d buttons)",
            px, py, hit, shell->button_count);

        if (hit == HIT_APPS_BUTTON) {
            toggle_menu(shell);
        } else if (hit >= HIT_TASK_BASE && hit < HIT_MENU_BASE) {
            int index = (int)(hit - HIT_TASK_BASE);
            if (index >= 0 && index < shell->button_count) {
                struct taskbar_button *button = &shell->buttons[index];

                /*
                 * Clicking the focused window's button puts it away; clicking
                 * any other brings it forward. That is the behaviour people
                 * already expect from a taskbar.
                 */
                if (button->appwin != NULL) {
                    int app_index = -1;
                    for (int a = 0; a < shell->app_count; a++) {
                        if (shell->apps[a] == button->appwin) {
                            app_index = a;
                            break;
                        }
                    }

                    if (recon_appwin_is_minimized(button->appwin)) {
                        recon_appwin_restore(button->appwin);
                        recon_appwin_focus(button->appwin);
                        set_focused_app(shell, app_index);
                        raise_app_order(shell, app_index);
                    } else if (recon_appwin_is_focused(button->appwin)) {
                        recon_appwin_minimize(button->appwin);
                        set_focused_app(shell, -1);
                    } else {
                        recon_appwin_focus(button->appwin);
                        set_focused_app(shell, app_index);
                        raise_app_order(shell, app_index);
                    }
                } else if (button->toplevel != NULL) {
                    if (recon_toplevel_is_minimized(button->toplevel)) {
                        recon_toplevel_restore(button->toplevel);
                    } else if (recon_toplevel_is_focused(button->toplevel)) {
                        recon_toplevel_minimize(button->toplevel);
                    } else {
                        recon_focus_toplevel(button->toplevel);
                    }
                }
            }
            recon_shell_close_menu(shell);
            recon_shell_refresh(shell);
        } else {
            recon_shell_close_menu(shell);
        }
        return true;
    }

    /*
     * The desktop answers last. It is the backdrop, so it only sees clicks
     * nothing in front of it wanted -- and only when the scene agrees nothing
     * is drawn over it there.
     */
    if (node == recon_desktop_node(shell->desktop)) {
        struct recon_desktop_action action;
        if (recon_desktop_handle_click(shell->desktop, lx, ly, pressed, &action)) {
            perform_desktop_action(shell, &action);
            return true;
        }
    }

    return false;
}
