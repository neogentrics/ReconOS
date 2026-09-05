/*
 * ReconOS File Explorer.
 *
 * A native application: it ships with the system and is part of it, rather
 * than something installed into it. It browses the ReconOS filesystem only --
 * there is no path it can be given that reaches the host, because there is no
 * such path.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ReconOS.h"
#include "recon_appwin.h"
#include "recon_icons.h"
#include "recon_explorer.h"
#include "recon_fonts.h"
#include "recon_fs.h"
#include "recon_props.h"
#include "recon_server.h"
#include "recon_shell.h"
#include "recon_icons.h"
#include "recon_registry.h"
#include "recon_theme.h"
#include "recon_ui.h"
#include "recon_wallpaper.h"

#define MENUBAR_HEIGHT 20
#define MENU_TITLE_PAD 10
#define TOOLBAR_HEIGHT 30
#define PATHBAR_HEIGHT 24
#define HEADER_HEIGHT 22
#define ROW_HEIGHT 18
#define STATUS_HEIGHT 24
#define PADDING 6
#define BUTTON_HEIGHT 22
#define BUTTON_GAP 4

#define SIDEBAR_WIDTH 132
#define COL_NAME 8
#define COL_TYPE 240
#define COL_SIZE 330

#define ENTRIES_MAX 512
#define HISTORY_MAX 64

/*
 * Places worth reaching in one click, in two groups: the folders that belong
 * to the person using the machine, then the machine itself. The rule for what
 * earns a place here is that it is somewhere you keep things.
 *
 * The Recycle Bin used to be listed and is not any more -- it is on the
 * desktop, which is where a bin belongs, and a second copy of it in the
 * sidebar only made the list longer. Applications are likewise reached from
 * the Start menu rather than by browsing to the folder they happen to live
 * in; /Apps is still there for anyone who wants to look at it.
 *
 * /Users was here briefly and should not have been. This list is places you
 * keep things; the folder holding everybody's account is administration, and
 * administration is the Control Panel's job. An administrator who wants to
 * look at it can walk there from Recon Core, which is what Recon Core is for.
 */
struct shortcut_entry {
    const char *label;
    const char *path;   /* NULL means the current user's folder of that name */
    const char *icon;
    bool starts_group;  /* Draw a dividing rule above this one. */
    bool admin_only;
};

static const struct shortcut_entry SIDEBAR[] = {
    { "Desktop", NULL, RECON_ICON_FOLDER, false, false },
    { "Documents", NULL, RECON_ICON_FOLDER, false, false },
    { "Downloads", NULL, RECON_ICON_FOLDER, false, false },
    { "Pictures", NULL, RECON_ICON_FOLDER, false, false },
    { "Music", NULL, RECON_ICON_FOLDER, false, false },
    { "Videos", NULL, RECON_ICON_FOLDER, false, false },

    /*
     * "This System" was a poor name for it -- everything on screen is this
     * system. This is the root of the filesystem, which in ReconOS is the
     * Recon Core.
     */
    { "Recon Core", "/", RECON_ICON_EXPLORER, true, false },
};

#define SIDEBAR_COUNT ((int)(sizeof(SIDEBAR) / sizeof(SIDEBAR[0])))

/* Height of one sidebar row, and of the gap a group divider occupies. */
#define SIDEBAR_ROW 24
#define SIDEBAR_GROUP_GAP 9

/*
 * The address bar's drop-down: everywhere you might reasonably want to go
 * from here. The known places first, then what is inside the folder being
 * looked at -- the same two things Windows offers from its address bar, for
 * the same reason, which is that "up and along" is a slow way to travel.
 */
#define PLACES_MAX 32
#define PLACE_ROW 20

struct place {
    char label[RECON_NAME_MAX];
    char path[RECON_PATH_MAX];
    /* A heading above this entry, or NULL. The list has two halves that mean
     * different things -- where you can jump to, and what is inside the
     * folder you are looking at -- and a bare rule between them left the
     * reader to work out which half was which. */
    const char *heading;
    /* An entry with no path is a note, not a destination: it says something
     * about the section it is in and cannot be clicked. */
    bool is_note;
};

#define COLOR_BG THEME(WINDOW_FRAME)
#define COLOR_LIST_BG THEME(SURFACE)
#define COLOR_HEADER THEME(SURFACE_HEADER)
#define COLOR_TEXT THEME(SURFACE_TEXT)
#define COLOR_BUTTON_TEXT THEME(BUTTON_TEXT)
#define COLOR_DIR THEME(DIRECTORY)
#define COLOR_ROW_ALT THEME(SURFACE_ALT)
#define COLOR_SELECTED THEME(SELECTION)
#define COLOR_SELECTED_TEXT THEME(SELECTION_TEXT)
#define COLOR_BUTTON THEME(BUTTON)
#define COLOR_PATH_BG THEME(FIELD)
#define COLOR_STATUS THEME(SURFACE_TEXT_DIM)
#define COLOR_WARNING THEME(WARNING)

#define HIT_BACK (RECON_APPWIN_HIT_USER + 1)
#define HIT_FORWARD (RECON_APPWIN_HIT_USER + 2)
#define HIT_UP (RECON_APPWIN_HIT_USER + 3)
#define HIT_HOME (RECON_APPWIN_HIT_USER + 4)
#define HIT_NEWFOLDER (RECON_APPWIN_HIT_USER + 5)
#define HIT_DELETE (RECON_APPWIN_HIT_USER + 6)
#define HIT_REFRESH (RECON_APPWIN_HIT_USER + 7)
#define HIT_RENAME (RECON_APPWIN_HIT_USER + 8)
#define HIT_RESTORE (RECON_APPWIN_HIT_USER + 9)
#define HIT_EMPTY_BIN (RECON_APPWIN_HIT_USER + 10)
#define HIT_PATH (RECON_APPWIN_HIT_USER + 11)
#define HIT_PATH_DROP (RECON_APPWIN_HIT_USER + 12)
#define HIT_MENU_BASE (RECON_APPWIN_HIT_USER + 14)
#define HIT_SIDEBAR_BASE (RECON_APPWIN_HIT_USER + 20)
#define HIT_PLACE_BASE (RECON_APPWIN_HIT_USER + 40)
#define HIT_MENU_ITEM_BASE (RECON_APPWIN_HIT_USER + 80)
#define HIT_ROW_BASE (RECON_APPWIN_HIT_USER + 100)

/*
 * What the explorer's own context menu entries mean. These ids travel through
 * the shell, which draws the menu without knowing what any of it does, and come
 * back here unchanged.
 */
enum explorer_context {
    EXCTX_OPEN = 1,
    EXCTX_RENAME,
    EXCTX_DELETE,
    EXCTX_CUT,
    EXCTX_COPY,
    EXCTX_PASTE,
    EXCTX_NEW_FOLDER,
    EXCTX_NEW_FILE,
    EXCTX_REFRESH,
    EXCTX_PROPERTIES,
    EXCTX_PURGE,        /* delete permanently, skipping the bin */
    EXCTX_RESTORE,      /* put back where it came from */
    EXCTX_EMPTY_BIN,
    EXCTX_SET_WALLPAPER,
    EXCTX_INSTALL_FONT,
};

/*
 * How far a delete has been confirmed.
 *
 * Emptying a folder is a bigger act than deleting a file, so it is asked for
 * separately rather than happening because the first confirmation happened to
 * land on a directory.
 */
/*
 * What a pending question was about.
 *
 * Deleting used to be confirmed by relabelling the Delete button to "Confirm
 * Delete". That changed the button's width, so the second click could miss it
 * entirely, and it put a question somewhere nobody reads. It is a dialog now.
 */
enum pending_question {
    QUESTION_NONE,
    QUESTION_TRASH,      /* move to the recycle bin */
    QUESTION_PURGE,      /* delete permanently */
    QUESTION_EMPTY_BIN,
};

struct recon_explorer {
    struct recon_font *font;
    struct recon_appwin *win;

    char cwd[RECON_PATH_MAX];
    struct recon_dirent entries[ENTRIES_MAX];
    int entry_count;

    int selected;
    int scroll;
    int rows_visible;

    /*
     * Where the user has been, as a trail with a position in it. Going back
     * and then somewhere new truncates the trail from that point, which is
     * what makes Forward mean "the way I came" rather than a stale branch.
     */
    char history[HISTORY_MAX][RECON_PATH_MAX];
    int history_count;
    int history_pos;

    /*
     * Deleting is confirmed by asking again rather than by a dialog. The
     * armed state is cleared by anything else the user does, so a stray second
     * click cannot delete something they have since moved on from.
     */
    enum pending_question question;
    char question_target[RECON_NAME_MAX];

    /* True while looking inside the recycle bin, which changes what the
     * actions mean: things there are restored or purged, not deleted again. */
    bool in_trash;

    /*
     * Renaming happens in place: the row turns into a text box. A dialog for
     * one short string would be more machinery and less obvious about what is
     * being renamed.
     */
    int renaming;   /* Index being renamed, or -1. */
    struct recon_edit rename_edit;

    /*
     * The address bar is a place you can type as well as read. Clicking it
     * turns it into a text box holding the current path, selected, so typing
     * replaces it and Enter goes there.
     */
    bool typing_path;
    struct recon_edit path_edit;

    /* The drop-down at the right of the address bar, and what is in it while
     * it is open. Filled when it opens rather than every frame: the folder
     * cannot change underneath a list somebody is reading. */
    bool places_open;
    struct place places[PLACES_MAX];
    int place_count;
    /* Which entry the pointer is over, or -1. A list that does not say which
     * row a click will take is a list you have to aim at twice. */
    int place_hover;

    /* Which menu on the menu bar is open, and which of its entries the
     * pointer is over. -1 for none. */
    int menu_open;
    int menu_item_hover;

    /*
     * What the pointer is over, in words. The toolbar is icons now, and an
     * icon nobody recognises is worse than a word -- so the status bar says
     * what the thing under the pointer does.
     */
    char hint[64];

    /* Where the listing was drawn, relative to the content area, so a right
     * click can tell a row from the empty space below the last one. */
    int list_x, list_y, list_w, list_h;

    char status[192];
    bool status_is_warning;
};

/* --- Navigation --- */

static void set_status(struct recon_explorer *ex, bool warning, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void set_status(struct recon_explorer *ex, bool warning, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(ex->status, sizeof(ex->status), fmt, args);
    va_end(args);
    ex->status_is_warning = warning;
}

/* Nothing to say. Its own function because set_status carries a printf format
 * attribute, and an empty format is a warning at every call site. */
static void clear_status(struct recon_explorer *ex) {
    ex->status[0] = '\0';
    ex->status_is_warning = false;
}

static void reload(struct recon_explorer *ex) {
    ex->entry_count = recon_fs_list(ex->cwd, ".", ex->entries, ENTRIES_MAX);
    if (ex->entry_count < 0) {
        ex->entry_count = 0;
        set_status(ex, true, "%s", recon_fs_last_error());
        return;
    }
    if (ex->entry_count > ENTRIES_MAX) {
        ex->entry_count = ENTRIES_MAX;
    }

    /*
     * Drop the system's own bookkeeping from the listing.
     *
     * A person's home folder was showing them .Trash and their registry
     * hive -- the Recycle Bin, which is already on the desktop and is not a
     * folder anyone should be walking into by hand, and a file that exists so
     * the desktop can remember their skin. Neither is theirs to manage, and
     * both being in the way of the folders that are theirs made the folder
     * look like a workspace they did not tidy.
     *
     * Hidden by the leading dot, the way every system that has this problem
     * solves it, rather than by a list of names that would go stale. The
     * terminal still shows them: 'dir' is where you go to see what is
     * actually there.
     */
    int kept = 0;
    for (int i = 0; i < ex->entry_count; i++) {
        if (ex->entries[i].name[0] == '.') {
            continue;
        }
        if (kept != i) {
            ex->entries[kept] = ex->entries[i];
        }
        kept++;
    }
    ex->entry_count = kept;

    int dirs = 0;
    size_t bytes = 0;
    for (int i = 0; i < ex->entry_count; i++) {
        if (ex->entries[i].kind == RECON_FILE_DIRECTORY) {
            dirs++;
        } else {
            bytes += ex->entries[i].size;
        }
    }

    set_status(ex, false, "%d item%s   %d folder%s   %zu bytes",
        ex->entry_count, ex->entry_count == 1 ? "" : "s",
        dirs, dirs == 1 ? "" : "s", bytes);
}

/* Record a place in the trail, dropping anything ahead of it. */
static void history_push(struct recon_explorer *ex, const char *path) {
    if (ex->history_count > 0 &&
            strcmp(ex->history[ex->history_pos], path) == 0) {
        return;
    }

    ex->history_pos = ex->history_count > 0 ? ex->history_pos + 1 : 0;

    if (ex->history_pos >= HISTORY_MAX) {
        /* Drop the oldest rather than refusing to move. */
        memmove(ex->history[0], ex->history[1],
            sizeof(ex->history[0]) * (HISTORY_MAX - 1));
        ex->history_pos = HISTORY_MAX - 1;
    }

    snprintf(ex->history[ex->history_pos], RECON_PATH_MAX, "%s", path);
    ex->history_count = ex->history_pos + 1;
}

static void navigate_to(struct recon_explorer *ex, const char *path, bool record);

static void navigate(struct recon_explorer *ex, const char *path) {
    navigate_to(ex, path, true);
}

static void navigate_to(struct recon_explorer *ex, const char *path, bool record) {
    char host[RECON_PATH_MAX];
    char canonical[RECON_PATH_MAX];

    if (!recon_fs_resolve(ex->cwd, path, host, sizeof(host),
            canonical, sizeof(canonical))) {
        set_status(ex, true, "%s", recon_fs_last_error());
        return;
    }

    struct recon_dirent info;
    if (!recon_fs_stat(ex->cwd, path, &info) || info.kind != RECON_FILE_DIRECTORY) {
        set_status(ex, true, "'%s' is not a folder", canonical);
        return;
    }

    snprintf(ex->cwd, sizeof(ex->cwd), "%s", canonical);

    /*
     * Remembered, so the explorer opens where it was left rather than at the
     * top every time. Written on arrival rather than on close, because a
     * desktop that crashes should still remember where you were.
     */
    recon_registry_set(RECON_REG_USER, "apps/explorer/last-folder", canonical);

    ex->selected = -1;
    ex->scroll = 0;
    /* Leaving the folder abandons anything half-started in it. */
    ex->question = QUESTION_NONE;
    ex->question_target[0] = '\0';
    ex->in_trash = recon_fs_is_trash("/", canonical);
    ex->renaming = -1;
    recon_edit_end(&ex->rename_edit);
    if (record) {
        history_push(ex, canonical);
    }
    reload(ex);
}

static void go_back(struct recon_explorer *ex) {
    if (ex->history_pos <= 0) {
        set_status(ex, false, "Nowhere to go back to");
        return;
    }
    ex->history_pos--;
    navigate_to(ex, ex->history[ex->history_pos], false);
}

static void go_forward(struct recon_explorer *ex) {
    if (ex->history_pos + 1 >= ex->history_count) {
        set_status(ex, false, "Nowhere to go forward to");
        return;
    }
    ex->history_pos++;
    navigate_to(ex, ex->history[ex->history_pos], false);
}

/* --- Actions --- */

/* Put the selection back on a named entry after the listing has changed, so
 * renaming or pasting leaves the thing you acted on still selected. */
static void select_by_name(struct recon_explorer *ex, const char *name) {
    ex->selected = -1;
    for (int i = 0; i < ex->entry_count; i++) {
        if (strcmp(ex->entries[i].name, name) == 0) {
            ex->selected = i;
            if (ex->rows_visible > 0 && i >= ex->scroll + ex->rows_visible) {
                ex->scroll = i - ex->rows_visible + 1;
            }
            if (i < ex->scroll) {
                ex->scroll = i;
            }
            return;
        }
    }
}

static void cancel_delete(struct recon_explorer *ex) {
    ex->question = QUESTION_NONE;
    ex->question_target[0] = '\0';
}

static void cancel_rename(struct recon_explorer *ex) {
    if (ex->renaming >= 0) {
        ex->renaming = -1;
        recon_edit_end(&ex->rename_edit);
    }
}

static const struct recon_dirent *selection(struct recon_explorer *ex) {
    if (ex->selected < 0 || ex->selected >= ex->entry_count) {
        return NULL;
    }
    return &ex->entries[ex->selected];
}

static void do_new_folder(struct recon_explorer *ex) {
    char name[RECON_NAME_MAX];
    if (!recon_fs_unique_name(ex->cwd, ex->cwd, "New Folder", "",
            name, sizeof(name))) {
        set_status(ex, true, "%s", recon_fs_last_error());
        return;
    }

    if (!recon_fs_mkdir(ex->cwd, name)) {
        set_status(ex, true, "%s", recon_fs_last_error());
        return;
    }

    reload(ex);
    select_by_name(ex, name);

    /*
     * Open the name for editing straight away. A folder called "New Folder 1"
     * that you then have to find a way to rename is a folder the system named,
     * not one you did.
     */
    ex->renaming = ex->selected;
    recon_edit_begin(&ex->rename_edit, name, false);
    set_status(ex, false, "Type a name, then Enter. Escape keeps '%s'.", name);
}

static void do_new_file(struct recon_explorer *ex) {
    char name[RECON_NAME_MAX];
    if (!recon_fs_unique_name(ex->cwd, ex->cwd, "New File", ".txt",
            name, sizeof(name))) {
        set_status(ex, true, "%s", recon_fs_last_error());
        return;
    }

    if (!recon_fs_write(ex->cwd, name, "", 0)) {
        set_status(ex, true, "%s", recon_fs_last_error());
        return;
    }

    reload(ex);
    select_by_name(ex, name);
    ex->renaming = ex->selected;
    /* The caret sits before ".txt", so typing replaces the name and keeps the
     * extension. */
    recon_edit_begin(&ex->rename_edit, name, true);
    set_status(ex, false, "Type a name, then Enter. Escape keeps '%s'.", name);
}

static void do_begin_rename(struct recon_explorer *ex) {
    const struct recon_dirent *entry = selection(ex);
    if (entry == NULL) {
        set_status(ex, true, "Select something to rename first");
        return;
    }
    if (recon_fs_is_protected(ex->cwd, entry->name)) {
        set_status(ex, true, "'%s' is part of the system and is protected",
            entry->name);
        return;
    }

    cancel_delete(ex);
    ex->renaming = ex->selected;
    recon_edit_begin(&ex->rename_edit, entry->name,
        entry->kind != RECON_FILE_DIRECTORY);
    set_status(ex, false, "Renaming '%s' - Enter to apply, Escape to cancel",
        entry->name);
}

static void do_commit_rename(struct recon_explorer *ex) {
    if (ex->renaming < 0 || ex->renaming >= ex->entry_count) {
        cancel_rename(ex);
        return;
    }

    char from[RECON_NAME_MAX];
    char to[RECON_NAME_MAX];
    snprintf(from, sizeof(from), "%s", ex->entries[ex->renaming].name);
    snprintf(to, sizeof(to), "%s", ex->rename_edit.text);

    cancel_rename(ex);

    /* Trim the spaces a name picks up from typing; a file called "notes " is
     * almost never what was meant and is invisible in a listing. */
    char *end = to + strlen(to);
    while (end > to && end[-1] == ' ') {
        *--end = '\0';
    }
    const char *start = to;
    while (*start == ' ') {
        start++;
    }

    if (*start == '\0') {
        set_status(ex, true, "A name cannot be empty");
        return;
    }
    if (strchr(start, '/') != NULL) {
        set_status(ex, true, "A name cannot contain '/'");
        return;
    }
    if (strcmp(start, from) == 0) {
        set_status(ex, false, "'%s' unchanged", from);
        return;
    }

    if (!recon_fs_rename(ex->cwd, from, start)) {
        set_status(ex, true, "%s", recon_fs_last_error());
        return;
    }

    char renamed[RECON_NAME_MAX];
    snprintf(renamed, sizeof(renamed), "%s", start);
    reload(ex);
    select_by_name(ex, renamed);
    set_status(ex, false, "Renamed to '%s'", renamed);
}

/* --- Deleting --- */

/*
 * The answer to whatever was asked.
 *
 * Called by the shell with the explorer's own pointer, so it does not need to
 * carry an identity through the question.
 */
static void explorer_answer(void *user, int choice) {
    struct recon_explorer *ex = user;

    enum pending_question asked = ex->question;
    char name[RECON_NAME_MAX];
    snprintf(name, sizeof(name), "%s", ex->question_target);
    cancel_delete(ex);

    /* Button 0 goes ahead; anything else, including Escape, declines. */
    if (choice != 0) {
        set_status(ex, false, "Nothing was changed");
        recon_appwin_refresh(ex->win);
        return;
    }

    switch (asked) {
    case QUESTION_TRASH:
        if (!recon_fs_trash(ex->cwd, name)) {
            set_status(ex, true, "%s", recon_fs_last_error());
        } else {
            ex->selected = -1;
            reload(ex);
            set_status(ex, false, "Moved '%s' to the Recycle Bin", name);
        }
        break;

    case QUESTION_PURGE:
        if (ex->in_trash ? !recon_fs_trash_purge(name)
                         : !(recon_fs_remove(ex->cwd, name) ||
                             recon_fs_remove_tree(ex->cwd, name))) {
            set_status(ex, true, "%s", recon_fs_last_error());
        } else {
            ex->selected = -1;
            reload(ex);
            set_status(ex, false, "Deleted '%s' permanently", name);
        }
        break;

    case QUESTION_EMPTY_BIN:
        recon_fs_trash_empty();
        ex->selected = -1;
        reload(ex);
        set_status(ex, false, "The Recycle Bin is empty");
        break;

    case QUESTION_NONE:
        break;
    }

    recon_appwin_refresh(ex->win);
}

/* Ask about something, remembering what was asked so the answer means
 * something when it arrives. */
static void ask_about(struct recon_explorer *ex, enum pending_question question,
        const char *name, const char *title, const char *message,
        const char *go_ahead) {
    ex->question = question;
    snprintf(ex->question_target, sizeof(ex->question_target), "%s",
        name != NULL ? name : "");

    /* Cancel last, because that is what Enter and Escape both choose. A
     * dialog that deletes when you hit Return by reflex is worse than none. */
    const char *buttons[2] = { go_ahead, "Cancel" };
    recon_appwin_ask(ex->win, title, message, buttons, 2, explorer_answer);
}

/*
 * What the selected thing is, in a box.
 *
 * The description comes from recon_props, which the desktop's identical menu
 * entry also uses -- two answers to "how big is this and when did it change"
 * is two places for them to disagree.
 */
static void do_properties(struct recon_explorer *ex) {
    const struct recon_dirent *entry = selection(ex);
    if (entry == NULL) {
        set_status(ex, true, "Select something first");
        return;
    }

    char text[512];
    /* Either way: when there is nothing there the reason goes in the text,
     * and a box saying why is more use than no box. */
    recon_props_describe(ex->cwd, entry->name, text, sizeof(text));

    const char *close[1] = { "Close" };
    recon_appwin_ask(ex->win, "Properties", text, close, 1, NULL);
}

static void do_delete(struct recon_explorer *ex) {
    const struct recon_dirent *entry = selection(ex);
    if (entry == NULL) {
        set_status(ex, true, "Select something to delete first");
        return;
    }

    char message[320];

    /* Inside the bin there is nowhere further to send things, so the only
     * delete available is the permanent one. */
    if (ex->in_trash) {
        snprintf(message, sizeof(message),
            "Permanently delete '%s'? This cannot be undone.", entry->name);
        ask_about(ex, QUESTION_PURGE, entry->name, "Delete Permanently",
            message, "Delete");
        return;
    }

    if (recon_fs_is_protected(ex->cwd, entry->name)) {
        set_status(ex, true, "'%s' is part of the system and is protected",
            entry->name);
        return;
    }

    snprintf(message, sizeof(message), "Move '%s' to the Recycle Bin?",
        entry->name);
    ask_about(ex, QUESTION_TRASH, entry->name, "Delete", message, "Move");
}

/* Skipping the bin, for when the user means it. */
static void do_purge(struct recon_explorer *ex) {
    const struct recon_dirent *entry = selection(ex);
    if (entry == NULL) {
        set_status(ex, true, "Select something first");
        return;
    }
    if (recon_fs_is_protected(ex->cwd, entry->name)) {
        set_status(ex, true, "'%s' is part of the system and is protected",
            entry->name);
        return;
    }

    char message[320];
    snprintf(message, sizeof(message),
        "Permanently delete '%s'? This cannot be undone.", entry->name);
    ask_about(ex, QUESTION_PURGE, entry->name, "Delete Permanently",
        message, "Delete");
}

static void do_restore(struct recon_explorer *ex) {
    const struct recon_dirent *entry = selection(ex);
    if (entry == NULL || !ex->in_trash) {
        set_status(ex, true, "Select something in the Recycle Bin first");
        return;
    }

    char name[RECON_NAME_MAX];
    snprintf(name, sizeof(name), "%s", entry->name);

    if (!recon_fs_trash_restore(name)) {
        set_status(ex, true, "%s", recon_fs_last_error());
        return;
    }

    char origin[RECON_PATH_MAX] = "where it came from";
    ex->selected = -1;
    reload(ex);
    set_status(ex, false, "Restored '%s' to %s", name, origin);
}

static void do_empty_bin(struct recon_explorer *ex) {
    int count = recon_fs_trash_count();
    if (count <= 0) {
        set_status(ex, false, "The Recycle Bin is already empty");
        return;
    }

    char message[320];
    snprintf(message, sizeof(message),
        "Permanently delete %d item%s in the Recycle Bin? This cannot be undone.",
        count, count == 1 ? "" : "s");
    ask_about(ex, QUESTION_EMPTY_BIN, NULL, "Empty Recycle Bin",
        message, "Empty");
}

/* Remember what to move or copy. Held as an absolute path, so navigating
 * somewhere else between the copy and the paste does not lose it. */
static void do_clip(struct recon_explorer *ex, bool cut) {
    const struct recon_dirent *entry = selection(ex);
    if (entry == NULL) {
        set_status(ex, true, "Select something first");
        return;
    }
    if (cut && recon_fs_is_protected(ex->cwd, entry->name)) {
        set_status(ex, true, "'%s' is part of the system and is protected",
            entry->name);
        return;
    }

    char host[RECON_PATH_MAX];
    char canonical[RECON_PATH_MAX];
    if (!recon_fs_resolve(ex->cwd, entry->name, host, sizeof(host),
            canonical, sizeof(canonical))) {
        set_status(ex, true, "%s", recon_fs_last_error());
        return;
    }

    recon_fs_clip_set(canonical, cut);
    set_status(ex, false, "%s '%s'", cut ? "Cut" : "Copied", entry->name);
}

static void do_paste(struct recon_explorer *ex) {
    char source[RECON_PATH_MAX];
    bool cut = false;

    if (!recon_fs_clip_get(source, sizeof(source), &cut)) {
        set_status(ex, true, "Nothing to paste");
        return;
    }
    if (!recon_fs_exists("/", source)) {
        /* The source went away between the copy and the paste. Say so and let
         * go of it, rather than leaving a clipboard that will fail forever. */
        recon_fs_clip_clear();
        set_status(ex, true, "'%s' is no longer there", source);
        return;
    }

    const char *leaf = strrchr(source, '/');
    leaf = (leaf != NULL && leaf[1] != '\0') ? leaf + 1 : source;

    /* Split the name so a second copy of "notes.txt" becomes "notes 2.txt"
     * rather than "notes.txt 2". */
    char base[RECON_NAME_MAX];
    char extension[RECON_NAME_MAX] = "";
    recon_text_copy(base, sizeof(base), leaf);
    char *dot = strrchr(base, '.');
    if (dot != NULL && dot != base) {
        snprintf(extension, sizeof(extension), "%s", dot);
        *dot = '\0';
    }

    char name[RECON_NAME_MAX];
    if (!recon_fs_unique_name(ex->cwd, ex->cwd, base, extension,
            name, sizeof(name))) {
        set_status(ex, true, "%s", recon_fs_last_error());
        return;
    }

    char target[RECON_PATH_MAX];
    if (!recon_fs_join(target, sizeof(target), ex->cwd, name)) {
        set_status(ex, true, "That would make a path too long to store.");
        return;
    }

    bool ok = cut ? recon_fs_rename("/", source, target)
                  : recon_fs_copy("/", source, target);
    if (!ok) {
        set_status(ex, true, "%s", recon_fs_last_error());
        return;
    }

    /* A cut is spent once pasted; a copy can be pasted again. */
    if (cut) {
        recon_fs_clip_clear();
    }

    reload(ex);
    select_by_name(ex, name);
    set_status(ex, false, "%s '%s'", cut ? "Moved" : "Copied", name);
}

static void do_open_selected(struct recon_explorer *ex) {
    const struct recon_dirent *entry = selection(ex);
    if (entry == NULL) {
        return;
    }
    if (entry->kind == RECON_FILE_DIRECTORY) {
        navigate(ex, entry->name);
        return;
    }

    /*
     * Open it, if anything opens it. This used to put the file's size in the
     * status bar, which is what a file manager says when it has nothing to
     * offer -- and it had nothing to offer, because nothing in ReconOS opened
     * a file by being clicked.
     */
    char path[RECON_PATH_MAX];
    if (!recon_fs_join(path, sizeof(path), ex->cwd, entry->name)) {
        set_status(ex, true, "That path is too long to open.");
        return;
    }

    struct recon_server *server = recon_appwin_server(ex->win);
    if (server != NULL && recon_shell_open_file(server->shell, path)) {
        return;
    }

    char size[32];
    recon_props_size(entry->size, size, sizeof(size));
    set_status(ex, false, "Nothing here opens a %s. '%s' - %s",
        recon_props_kind(entry, entry->name), entry->name, size);
}

/* --- Drawing --- */

static int draw_button(struct recon_explorer *ex, struct recon_panel *p,
        int x, int y, const char *label, uint32_t id, bool warning) {
    int ascent = recon_font_ascent(ex->font);
    int width = recon_text_width(ex->font, label) + 22;

    recon_fill_rect(p, x, y, width, BUTTON_HEIGHT, COLOR_BUTTON);
    recon_draw_button_edge(p, x, y, width, BUTTON_HEIGHT, false,
        COLOR_BG);
    recon_draw_text(p, ex->font, x + 11, y + (BUTTON_HEIGHT + ascent) / 2 - 2,
        width - 16, label, warning ? COLOR_WARNING : COLOR_BUTTON_TEXT);
    recon_hit_add(p, x, y, width, BUTTON_HEIGHT, id);

    return x + width + BUTTON_GAP;
}

/*
 * --- Toolbar glyphs ---
 *
 * Drawn from filled rectangles rather than loaded from files. Two reasons:
 * an icon file that is missing would leave a blank button, and these are
 * small enough that a hand-placed shape is sharper than a scaled bitmap.
 *
 * They are icons and not words because the toolbar was running out of room
 * and because a shape is read faster than a label -- but an unrecognised
 * icon is worse than a word, so every one of them also names itself in the
 * status bar when the pointer is over it.
 */
enum glyph {
    GLYPH_BACK,
    GLYPH_FORWARD,
    GLYPH_UP,
    GLYPH_REFRESH,
    GLYPH_HOME,
    GLYPH_NEW_FOLDER,
    GLYPH_RENAME,
    GLYPH_DELETE,
    GLYPH_RESTORE,
};

/*
 * A triangle pointing left (-1), right (1) or up (0), centred on (cx, cy).
 *
 * The point of the triangle is the narrow end, so the column of height 1 goes
 * in the direction of travel. This was the other way round, which meant Back
 * had a right-pointing arrow on it and Forward a left-pointing one -- the two
 * buttons said the opposite of what they did.
 */
static void draw_triangle(struct recon_panel *p, int cx, int cy,
        int direction, recon_color ink) {
    for (int i = 0; i < 5; i++) {
        if (direction == 0) {
            recon_fill_rect(p, cx - i, cy - 4 + i, i * 2 + 1, 1, ink);
        } else {
            int dx = direction < 0 ? cx - 2 + i : cx + 2 - i;
            recon_fill_rect(p, dx, cy - i, 1, i * 2 + 1, ink);
        }
    }
}

static void draw_glyph(struct recon_panel *p, enum glyph glyph,
        int cx, int cy, recon_color ink) {
    switch (glyph) {
    case GLYPH_BACK:
        draw_triangle(p, cx, cy, -1, ink);
        break;

    case GLYPH_FORWARD:
        draw_triangle(p, cx, cy, 1, ink);
        break;

    case GLYPH_UP:
        draw_triangle(p, cx, cy - 2, 0, ink);
        recon_fill_rect(p, cx - 1, cy + 1, 3, 5, ink);
        break;

    case GLYPH_REFRESH: {
        /*
         * A ring with a bite out of the top right, and an arrowhead at the
         * open end: a circle that is going somewhere.
         *
         * Drawn as an arc rather than as a dozen hand-placed rectangles. The
         * hand-placed version was assembled by eye and looked, accurately, as
         * though it had been assembled by eye.
         */
        const int radius = 6;
        for (int y = -radius - 1; y <= radius + 1; y++) {
            for (int x = -radius - 1; x <= radius + 1; x++) {
                int d2 = x * x + y * y;
                /* A band two pixels thick, which is the ring itself. */
                if (d2 > radius * radius || d2 < (radius - 2) * (radius - 2)) {
                    continue;
                }
                /* The gap: the top-right eighth or so, where the arrow is. */
                if (x >= 0 && y <= 0 && y > -x - 2) {
                    continue;
                }
                recon_fill_rect(p, cx + x, cy + y, 1, 1, ink);
            }
        }
        /* The arrowhead, pointing clockwise into the gap. */
        for (int i = 0; i < 4; i++) {
            recon_fill_rect(p, cx + 2 + i, cy - radius - 1 + i, 1,
                (4 - i) * 2 - 1, ink);
        }
        break;
    }

    case GLYPH_HOME:
        /* A roof over a box. */
        for (int i = 0; i < 6; i++) {
            recon_fill_rect(p, cx - i, cy - 5 + i, i * 2 + 1, 1, ink);
        }
        recon_fill_rect(p, cx - 4, cy + 1, 9, 1, ink);
        recon_fill_rect(p, cx - 4, cy + 1, 1, 5, ink);
        recon_fill_rect(p, cx + 4, cy + 1, 1, 5, ink);
        recon_fill_rect(p, cx - 4, cy + 5, 9, 1, ink);
        /* A door, so it reads as a house rather than as a tent on a box. */
        recon_fill_rect(p, cx - 1, cy + 2, 3, 4, ink);
        break;

    case GLYPH_NEW_FOLDER:
        /* A folder with a tab, and a plus in the corner. */
        recon_fill_rect(p, cx - 7, cy - 4, 5, 1, ink);
        recon_fill_rect(p, cx - 7, cy - 3, 11, 1, ink);
        recon_fill_rect(p, cx - 7, cy - 3, 1, 8, ink);
        recon_fill_rect(p, cx + 3, cy - 3, 1, 8, ink);
        recon_fill_rect(p, cx - 7, cy + 4, 11, 1, ink);
        recon_fill_rect(p, cx + 4, cy + 1, 5, 1, ink);
        recon_fill_rect(p, cx + 6, cy - 1, 1, 5, ink);
        break;

    case GLYPH_RENAME:
        /* A pencil on a line: the line is what is being written on. */
        for (int i = 0; i < 6; i++) {
            recon_fill_rect(p, cx - 4 + i, cy + 1 - i, 2, 2, ink);
        }
        recon_fill_rect(p, cx - 6, cy + 2, 3, 3, ink);   /* the tip */
        recon_fill_rect(p, cx - 7, cy + 6, 14, 1, ink);  /* the line */
        break;

    case GLYPH_DELETE:
        /* A bin: lid, handle, body, two ribs. */
        recon_fill_rect(p, cx - 2, cy - 6, 5, 1, ink);
        recon_fill_rect(p, cx - 6, cy - 5, 13, 2, ink);
        recon_fill_rect(p, cx - 5, cy - 2, 1, 8, ink);
        recon_fill_rect(p, cx + 5, cy - 2, 1, 8, ink);
        recon_fill_rect(p, cx - 5, cy + 6, 11, 1, ink);
        recon_fill_rect(p, cx - 2, cy - 1, 1, 6, ink);
        recon_fill_rect(p, cx + 2, cy - 1, 1, 6, ink);
        break;

    case GLYPH_RESTORE:
        /* The same bin with something coming back out of it. */
        recon_fill_rect(p, cx - 6, cy - 1, 13, 2, ink);
        recon_fill_rect(p, cx - 5, cy + 2, 1, 5, ink);
        recon_fill_rect(p, cx + 5, cy + 2, 1, 5, ink);
        recon_fill_rect(p, cx - 5, cy + 7, 11, 1, ink);
        draw_triangle(p, cx, cy - 4, 0, ink);
        break;
    }
}

/*
 * A toolbar button: one glyph, no label. `enabled` decides both the ink and
 * whether the button is offered to a click at all -- a button that cannot do
 * anything should not report a failure the user could not have avoided.
 */
static int draw_tool(struct recon_explorer *ex, struct recon_panel *p,
        int x, int y, enum glyph glyph, uint32_t id, bool enabled) {
    const int width = 28;
    (void)ex;

    recon_fill_rect(p, x, y, width, BUTTON_HEIGHT, COLOR_BUTTON);
    recon_draw_button_edge(p, x, y, width, BUTTON_HEIGHT, false,
        COLOR_BG);

    draw_glyph(p, glyph, x + width / 2, y + BUTTON_HEIGHT / 2,
        enabled ? COLOR_TEXT : THEME(MENU_TEXT_DISABLED));

    if (enabled) {
        recon_hit_add(p, x, y, width, BUTTON_HEIGHT, id);
    }
    return x + width + 2;
}

/* A vertical rule between groups of toolbar buttons, so "where I have been"
 * and "what I can do here" do not read as one undifferentiated row. */
static int draw_tool_divider(struct recon_panel *p, int x, int y) {
    recon_fill_rect(p, x + 3, y + 3, 1, BUTTON_HEIGHT - 6,
        THEME(MENU_SEPARATOR));
    return x + 8;
}

/* Where a sidebar entry points. Worked out in one place so the drawing and
 * the clicking cannot disagree about it. */
static void sidebar_path(int index, char *out, size_t size) {
    if (index < 0 || index >= SIDEBAR_COUNT) {
        snprintf(out, size, "/");
        return;
    }
    if (SIDEBAR[index].path == NULL) {
        snprintf(out, size, "%s", recon_fs_user_dir(SIDEBAR[index].label));
    } else {
        snprintf(out, size, "%s", SIDEBAR[index].path);
    }
}

/*
 * --- The menu bar ---
 *
 * The row of words above everything, which every application with a File
 * menu has and this one did not. It carries what belongs in a menu rather
 * than on a toolbar: the things you do occasionally, and the things whose
 * names are worth reading.
 */
static const char *const MENU_FILE_ITEMS[] = {
    "New Folder", "New File", "Rename", "Delete", "Close",
};

static const char *const MENU_EDIT_ITEMS[] = {
    "Cut", "Copy", "Paste",
};

static const char *const MENU_VIEW_ITEMS[] = {
    "Refresh", "Home", "Up One Folder",
};

static const struct {
    const char *label;
    const char *const *items;
    int count;
} MENUS[] = {
    { "File", MENU_FILE_ITEMS,
      (int)(sizeof(MENU_FILE_ITEMS) / sizeof(MENU_FILE_ITEMS[0])) },
    { "Edit", MENU_EDIT_ITEMS,
      (int)(sizeof(MENU_EDIT_ITEMS) / sizeof(MENU_EDIT_ITEMS[0])) },
    { "View", MENU_VIEW_ITEMS,
      (int)(sizeof(MENU_VIEW_ITEMS) / sizeof(MENU_VIEW_ITEMS[0])) },
};

#define MENU_COUNT ((int)(sizeof(MENUS) / sizeof(MENUS[0])))

/* Where a menu's title sits, so drawing it and clicking it agree. */
static int menu_title_x(struct recon_explorer *ex, int index) {
    int mx = PADDING;
    for (int i = 0; i < index && i < MENU_COUNT; i++) {
        mx += recon_text_width(ex->font, MENUS[i].label) + MENU_TITLE_PAD * 2;
    }
    return mx;
}

static void draw_menubar(struct recon_explorer *ex, struct recon_panel *p,
        int x, int y, int w) {
    int ascent = recon_font_ascent(ex->font);

    recon_fill_rect(p, x, y, w, MENUBAR_HEIGHT, THEME(MENU));

    for (int i = 0; i < MENU_COUNT; i++) {
        int mx = x + menu_title_x(ex, i);
        int mw = recon_text_width(ex->font, MENUS[i].label) + MENU_TITLE_PAD * 2;
        bool open = (ex->menu_open == i);

        if (open) {
            recon_fill_rect(p, mx, y, mw, MENUBAR_HEIGHT, THEME(MENU_HILITE));
        }
        recon_draw_text(p, ex->font, mx + MENU_TITLE_PAD,
            y + (MENUBAR_HEIGHT + ascent) / 2 - 2, mw, MENUS[i].label,
            open ? THEME(MENU_HILITE_TEXT) : THEME(MENU_TEXT));

        recon_hit_add(p, mx, y, mw, MENUBAR_HEIGHT, HIT_MENU_BASE + i);
    }
}

/* Drawn last, so it covers what is beneath it. */
static void draw_menu_dropdown(struct recon_explorer *ex, struct recon_panel *p,
        int x, int y) {
    if (ex->menu_open < 0 || ex->menu_open >= MENU_COUNT) {
        return;
    }

    int ascent = recon_font_ascent(ex->font);
    int count = MENUS[ex->menu_open].count;

    int widest = 0;
    for (int i = 0; i < count; i++) {
        int iw = recon_text_width(ex->font, MENUS[ex->menu_open].items[i]);
        if (iw > widest) {
            widest = iw;
        }
    }

    int dw = widest + 40;
    int dh = count * PLACE_ROW + 6;
    int dx = x + menu_title_x(ex, ex->menu_open);
    int dy = y + MENUBAR_HEIGHT;

    recon_fill_rect(p, dx, dy, dw, dh, THEME(MENU));
    recon_stroke_rect(p, dx, dy, dw, dh, THEME(MENU_BORDER));

    for (int i = 0; i < count; i++) {
        int iy = dy + 3 + i * PLACE_ROW;
        bool hovered = (ex->menu_item_hover == i);

        if (hovered) {
            recon_fill_rect(p, dx + 1, iy, dw - 2, PLACE_ROW,
                THEME(SELECTION));
        }
        recon_draw_text(p, ex->font, dx + 14,
            iy + (PLACE_ROW + ascent) / 2 - 2, dw - 20,
            MENUS[ex->menu_open].items[i],
            hovered ? THEME(SELECTION_TEXT) : THEME(MENU_TEXT));

        recon_hit_add(p, dx, iy, dw, PLACE_ROW, HIT_MENU_ITEM_BASE + i);
    }
}

/* Whether a sidebar entry is offered to whoever is signed in. */
static bool sidebar_visible(int index) {
    if (index < 0 || index >= SIDEBAR_COUNT) {
        return false;
    }
    return !SIDEBAR[index].admin_only || recon_fs_user_is_administrator();
}

/* The sidebar: places worth reaching without walking the tree, in groups. */
static void draw_sidebar(struct recon_explorer *ex, struct recon_panel *p,
        int x, int y, int h) {
    int ascent = recon_font_ascent(ex->font);

    recon_fill_rect(p, x, y, SIDEBAR_WIDTH, h, COLOR_BG);
    recon_fill_rect(p, x + SIDEBAR_WIDTH - 1, y, 1, h, RECON_RGB(0x90, 0x90, 0x90));

    int iy = y + 4;
    for (int i = 0; i < SIDEBAR_COUNT; i++) {
        if (!sidebar_visible(i)) {
            continue;
        }

        /* A rule between groups. Drawn before the row it belongs to rather
         * than after the previous one, so a hidden entry cannot leave a
         * divider hanging under nothing. */
        if (SIDEBAR[i].starts_group && iy > y + 4) {
            recon_fill_rect(p, x + 8, iy + SIDEBAR_GROUP_GAP / 2,
                SIDEBAR_WIDTH - 20, 1, THEME(MENU_SEPARATOR));
            iy += SIDEBAR_GROUP_GAP;
        }

        if (iy + SIDEBAR_ROW > y + h) {
            break;
        }

        char path[RECON_PATH_MAX];
        sidebar_path(i, path, sizeof(path));

        /* The place being looked at is marked, so the sidebar says where you
         * are as well as where you can go. */
        bool current = (strcmp(ex->cwd, path) == 0);
        if (current) {
            recon_fill_rect(p, x + 2, iy, SIDEBAR_WIDTH - 6, 22, COLOR_SELECTED);
        }

        int label_x = x + 8;
        if (recon_icon_draw(p, SIDEBAR[i].icon, x + 6, iy + 3, 16)) {
            label_x = x + 6 + 16 + 6;
        }
        recon_draw_text(p, ex->font, label_x, iy + (22 + ascent) / 2 - 2,
            SIDEBAR_WIDTH - (label_x - x) - 6, SIDEBAR[i].label,
            current ? COLOR_SELECTED_TEXT : COLOR_TEXT);

        recon_hit_add(p, x + 2, iy, SIDEBAR_WIDTH - 6, 22, HIT_SIDEBAR_BASE + i);
        iy += SIDEBAR_ROW;
    }
}

/* --- The address bar's drop-down --- */

/*
 * Fill the list: the known places, then the folders inside the one being
 * looked at. Built when the list opens rather than while it is drawn, so what
 * is on screen cannot change between reading it and clicking it.
 */
static void build_places(struct recon_explorer *ex) {
    ex->place_count = 0;

    for (int i = 0; i < SIDEBAR_COUNT && ex->place_count < PLACES_MAX; i++) {
        if (!sidebar_visible(i)) {
            continue;
        }
        struct place *place = &ex->places[ex->place_count++];
        snprintf(place->label, sizeof(place->label), "%s", SIDEBAR[i].label);
        sidebar_path(i, place->path, sizeof(place->path));
        place->heading = (ex->place_count == 1) ? "Go to" : NULL;
        place->is_note = false;
    }

    /*
     * The Recycle Bin. Not in the sidebar, because it is already on the
     * desktop and a bin is not somewhere you keep things -- but it is
     * somewhere you go, and this list is everywhere you can go from here.
     */
    if (ex->place_count < PLACES_MAX) {
        struct place *bin = &ex->places[ex->place_count++];
        snprintf(bin->label, sizeof(bin->label), "Recycle Bin");
        snprintf(bin->path, sizeof(bin->path), "%s", recon_fs_trash_dir());
        bin->heading = NULL;
        bin->is_note = false;
    }

    /* Then what is in this folder, which is the half that makes the
     * drop-down worth having: it is a way down as well as a way across. */
    int here = 0;
    int folders = 0;
    for (int i = 0; i < ex->entry_count && ex->place_count < PLACES_MAX; i++) {
        if (ex->entries[i].kind != RECON_FILE_DIRECTORY) {
            continue;
        }
        folders++;

        /*
         * Joined rather than pasted. A path built by hand and cut short is a
         * path to somewhere else, and this one is compared against the known
         * places and then navigated to -- so a deep folder with a long name
         * would have put a wrong destination in the drop-down.
         */
        char path[RECON_PATH_MAX];
        if (!recon_fs_join(path, sizeof(path), ex->cwd,
                ex->entries[i].name)) {
            continue;
        }

        /*
         * Not if it is already up there among the known places.
         *
         * Standing in your own folder, the six folders inside it are exactly
         * the six the list already offers, so the drop-down showed each of
         * them twice -- and the second copy was in the section meant to show
         * you what was *here*, telling you nothing you had not just read.
         */
        bool already = false;
        for (int j = 0; j < ex->place_count && !already; j++) {
            already = strcmp(ex->places[j].path, path) == 0;
        }
        if (already) {
            continue;
        }

        struct place *place = &ex->places[ex->place_count++];
        snprintf(place->label, sizeof(place->label), "%s", ex->entries[i].name);
        snprintf(place->path, sizeof(place->path), "%s", path);
        place->heading = (here == 0) ? "In this folder" : NULL;
        place->is_note = false;
        here++;
    }

    /*
     * Say so when there is nothing here, rather than ending the list.
     *
     * A folder with no folders in it produced no second section at all, so
     * the drop-down looked like a list of shortcuts that had forgotten to
     * mention where you were standing. An empty section that says it is empty
     * answers the question; a missing one leaves it open.
     */
    if (here == 0 && ex->place_count < PLACES_MAX) {
        struct place *note = &ex->places[ex->place_count++];
        /*
         * Two different nothings. A folder with no folders in it, and a
         * folder whose folders are all listed above already -- which is what
         * your own home folder looks like. Saying "no folders inside this
         * one" while standing in a folder with six of them is simply false.
         */
        snprintf(note->label, sizeof(note->label), "%s",
            folders > 0 ? "All of them are listed above"
                        : "No folders inside this one");
        note->path[0] = '\0';
        note->heading = "In this folder";
        note->is_note = true;
    }
}

/*
 * How wide the open list is: as wide as its widest entry needs, and no wider.
 *
 * It used to be the width of the address bar, which on a wide window meant a
 * panel of mostly empty grey with a column of short words down one edge. A
 * menu should be the size of what is in it.
 */
static int places_width(struct recon_explorer *ex, int limit) {
    int widest = 0;
    for (int i = 0; i < ex->place_count; i++) {
        int w = recon_text_width(ex->font, ex->places[i].label);
        if (w > widest) {
            widest = w;
        }
        if (ex->places[i].heading != NULL) {
            w = recon_text_width(ex->font, ex->places[i].heading);
            if (w > widest) {
                widest = w;
            }
        }
    }

    int width = widest + 44;
    if (width < 180) {
        width = 180;   /* Narrower than this and it reads as a tooltip. */
    }
    if (width > limit) {
        width = limit;
    }
    return width;
}

/* How tall the open list is, headings and rules included. */
static int places_height(struct recon_explorer *ex) {
    int height = 4;
    for (int i = 0; i < ex->place_count; i++) {
        if (ex->places[i].heading != NULL) {
            height += PLACE_ROW + (i > 0 ? 6 : 0);
        }
        height += PLACE_ROW;
    }
    return height + 4;
}

static void draw_places(struct recon_explorer *ex, struct recon_panel *p,
        int x, int y, int w, int limit) {
    int ascent = recon_font_ascent(ex->font);
    int height = places_height(ex);
    if (height > limit) {
        height = limit;
    }

    /* Sized to its contents, and hung from the right-hand end of the address
     * bar where the button that opened it is. */
    int full = w;
    w = places_width(ex, full);
    x += full - w;

    recon_fill_rect(p, x, y, w, height, THEME(MENU));
    recon_stroke_rect(p, x, y, w, height, THEME(MENU_BORDER));

    int iy = y + 4;
    for (int i = 0; i < ex->place_count; i++) {
        /* A heading, and above all but the first a rule to part the sections
         * properly. */
        if (ex->places[i].heading != NULL) {
            if (i > 0) {
                recon_fill_rect(p, x + 6, iy + 2, w - 12, 1,
                    THEME(MENU_SEPARATOR));
                iy += 6;
            }
            if (iy + PLACE_ROW > y + height) {
                break;
            }
            recon_draw_text(p, ex->font, x + 10,
                iy + (PLACE_ROW + ascent) / 2 - 2, w - 20,
                ex->places[i].heading, THEME(MENU_TEXT_DISABLED));
            iy += PLACE_ROW;
        }

        if (iy + PLACE_ROW > y + height) {
            break;
        }

        /* A note describes the section it sits in; it is not somewhere to
         * go, so it is neither highlighted nor offered to a click. */
        if (ex->places[i].is_note) {
            recon_draw_text(p, ex->font, x + 22,
                iy + (PLACE_ROW + ascent) / 2 - 2, w - 32,
                ex->places[i].label, THEME(MENU_TEXT_DISABLED));
            iy += PLACE_ROW;
            continue;
        }

        /* Marked when it is where you already are, and when the pointer is
         * over it. Without the second, the list showed its entries and gave
         * no sign which one a click would take. */
        bool current = strcmp(ex->places[i].path, ex->cwd) == 0;
        bool hovered = (ex->place_hover == i);
        if (current || hovered) {
            recon_fill_rect(p, x + 1, iy, w - 2, PLACE_ROW,
                hovered ? THEME(SELECTION) : THEME(MENU_HILITE));
        }
        recon_draw_text(p, ex->font, x + 22, iy + (PLACE_ROW + ascent) / 2 - 2,
            w - 32, ex->places[i].label,
            hovered ? THEME(SELECTION_TEXT)
                    : (current ? THEME(MENU_HILITE_TEXT) : THEME(MENU_TEXT)));

        recon_hit_add(p, x, iy, w, PLACE_ROW, HIT_PLACE_BASE + i);
        iy += PLACE_ROW;
    }
}

static void close_places(struct recon_explorer *ex) {
    ex->places_open = false;
    ex->place_count = 0;
    ex->place_hover = -1;
}

/* --- The address bar --- */

static void begin_typing_path(struct recon_explorer *ex) {
    close_places(ex);
    ex->typing_path = true;
    /* The whole path arrives selected, so typing replaces it -- the usual
     * reason to click an address bar is to go somewhere else entirely. */
    recon_edit_begin(&ex->path_edit, ex->cwd, false);
    set_status(ex, false, "Type a path, then Enter. Escape to leave it.");
}

static void stop_typing_path(struct recon_explorer *ex) {
    ex->typing_path = false;
    recon_edit_end(&ex->path_edit);
}

static void explorer_draw(void *user, struct recon_panel *p,
        int x, int y, int w, int h) {
    struct recon_explorer *ex = user;
    int ascent = recon_font_ascent(ex->font);

    recon_fill_rect(p, x, y, w, h, COLOR_BG);

    /* A menu bar, above everything, the way Notepad has one. */
    draw_menubar(ex, p, x, y, w);

    /*
     * One bar: where you have been, then where you are, then what you can do
     * here.
     *
     * These were two rows -- a row of buttons and, under it, an address bar
     * running the whole width of the window. The address bar did not need
     * that width, and the second row bought nothing but height. Putting the
     * navigation to the left of the address and the actions to its right
     * makes the row read left to right as one sentence, and gives the listing
     * back the space.
     */
    int by = y + MENUBAR_HEIGHT + (TOOLBAR_HEIGHT - BUTTON_HEIGHT) / 2;
    int bx = x + PADDING;

    bool can_back = ex->history_pos > 0;
    bool can_forward = ex->history_pos + 1 < ex->history_count;

    /* Where you have been, and where you started. */
    bx = draw_tool(ex, p, bx, by, GLYPH_BACK, HIT_BACK, can_back);
    bx = draw_tool(ex, p, bx, by, GLYPH_FORWARD, HIT_FORWARD, can_forward);
    bx = draw_tool(ex, p, bx, by, GLYPH_UP, HIT_UP, strcmp(ex->cwd, "/") != 0);
    bx = draw_tool(ex, p, bx, by, GLYPH_REFRESH, HIT_REFRESH, true);
    bx = draw_tool(ex, p, bx, by, GLYPH_HOME, HIT_HOME, true);
    bx = draw_tool_divider(p, bx, by);

    /*
     * What you can do here, laid out from the right. Inside the bin these
     * mean different things, so different ones are offered: things in a bin
     * are restored or emptied, not renamed and deleted again.
     */
    int actions_right = x + w - PADDING;
    int actions_left;

    if (ex->in_trash) {
        int empty_w = recon_text_width(ex->font, "Empty Bin") + 22;
        actions_left = actions_right - empty_w - 2 - 28;
        int ax = draw_tool(ex, p, actions_left, by, GLYPH_RESTORE,
            HIT_RESTORE, true);
        draw_button(ex, p, ax, by, "Empty Bin", HIT_EMPTY_BIN, true);
    } else {
        actions_left = actions_right - 28 * 3;
        int ax = draw_tool(ex, p, actions_left, by, GLYPH_NEW_FOLDER,
            HIT_NEWFOLDER, true);
        ax = draw_tool(ex, p, ax, by, GLYPH_RENAME, HIT_RENAME, true);
        draw_tool(ex, p, ax, by, GLYPH_DELETE, HIT_DELETE, true);
    }
    draw_tool_divider(p, actions_left - 8, by);

    /*
     * The address bar, in the middle of that row. It reads as a label and
     * works as a field: clicking it lets you type a path, and the button at
     * its right end drops down everywhere you might want to go from here.
     */
    int py = by + (BUTTON_HEIGHT - PATHBAR_HEIGHT) / 2;
    int path_x = bx + 4;
    int path_w = (actions_left - 12) - path_x;
    int drop_w = 20;

    /* A window narrow enough that the buttons meet in the middle gets a
     * field of nothing rather than one drawn backwards. */
    if (path_w < drop_w + 40) {
        path_w = drop_w + 40;
    }

    if (ex->typing_path) {
        recon_edit_draw(p, ex->font, path_x, py, path_w - drop_w,
            PATHBAR_HEIGHT, &ex->path_edit);
    } else {
        recon_fill_rect(p, path_x, py, path_w - drop_w, PATHBAR_HEIGHT,
            COLOR_PATH_BG);
        recon_draw_bevel(p, path_x, py, path_w - drop_w, PATHBAR_HEIGHT, true);
        recon_draw_text(p, ex->font, path_x + 6,
            py + (PATHBAR_HEIGHT + ascent) / 2 - 2, path_w - drop_w - 12,
            ex->cwd, COLOR_TEXT);
    }
    recon_hit_add(p, path_x, py, path_w - drop_w, PATHBAR_HEIGHT, HIT_PATH);

    int drop_x = path_x + path_w - drop_w;
    recon_fill_rect(p, drop_x, py, drop_w, PATHBAR_HEIGHT,
        ex->places_open ? THEME(BUTTON_ACTIVE) : COLOR_BUTTON);
    recon_draw_bevel(p, drop_x, py, drop_w, PATHBAR_HEIGHT, ex->places_open);
    /* A chevron: the same shape every drop-down in the system uses. */
    for (int i = 0; i < 4; i++) {
        recon_fill_rect(p, drop_x + drop_w / 2 - 3 + i,
            py + PATHBAR_HEIGHT / 2 - 2 + i, 1, 1, COLOR_TEXT);
        recon_fill_rect(p, drop_x + drop_w / 2 + 3 - i,
            py + PATHBAR_HEIGHT / 2 - 2 + i, 1, 1, COLOR_TEXT);
    }
    recon_hit_add(p, drop_x, py, drop_w, PATHBAR_HEIGHT, HIT_PATH_DROP);

    /* Sidebar down the left, listing to the right of it. */
    int body_y = y + MENUBAR_HEIGHT + TOOLBAR_HEIGHT + 2;
    int body_h = h - (body_y - y) - STATUS_HEIGHT;
    draw_sidebar(ex, p, x, body_y, body_h);

    int lx = x + SIDEBAR_WIDTH;
    int lw = w - SIDEBAR_WIDTH;

    /* Column headings. */
    int hy = body_y;
    recon_fill_rect(p, lx, hy, lw, HEADER_HEIGHT, COLOR_HEADER);
    recon_fill_rect(p, lx, hy + HEADER_HEIGHT - 1, lw, 1, RECON_RGB(0x80, 0x80, 0x80));

    int hbase = hy + (HEADER_HEIGHT + ascent) / 2 - 2;
    recon_draw_text(p, ex->font, lx + COL_NAME, hbase, 200, "Name", COLOR_TEXT);
    recon_draw_text(p, ex->font, lx + COL_TYPE, hbase, 80, "Type", COLOR_TEXT);
    recon_draw_text(p, ex->font, lx + COL_SIZE, hbase, 80, "Size", COLOR_TEXT);

    /* Listing. */
    int ly = hy + HEADER_HEIGHT;
    int list_h = body_h - HEADER_HEIGHT;
    ex->rows_visible = list_h > 0 ? list_h / ROW_HEIGHT : 0;

    recon_fill_rect(p, lx, ly, lw, list_h, COLOR_LIST_BG);

    /* Kept so a right click can tell a row from the empty space under the last
     * one, which offer different things. */
    ex->list_x = lx - x;
    ex->list_y = ly - y;
    ex->list_w = lw;
    ex->list_h = list_h > 0 ? list_h : 0;

    for (int row = 0; row < ex->rows_visible; row++) {
        int index = ex->scroll + row;
        if (index >= ex->entry_count) {
            break;
        }

        const struct recon_dirent *entry = &ex->entries[index];
        int ry = ly + row * ROW_HEIGHT;
        int baseline = ry + (ROW_HEIGHT + ascent) / 2 - 2;
        bool selected = (index == ex->selected);
        bool is_dir = (entry->kind == RECON_FILE_DIRECTORY);

        if (selected) {
            recon_fill_rect(p, lx, ry, lw, ROW_HEIGHT, COLOR_SELECTED);
        } else if (row % 2 == 1) {
            recon_fill_rect(p, lx, ry, lw, ROW_HEIGHT, COLOR_ROW_ALT);
        }

        recon_color text = selected ? COLOR_SELECTED_TEXT
            : (is_dir ? COLOR_DIR : COLOR_TEXT);

        /* A small icon per row, so folders and files are told apart before
         * the type column is read. */
        int name_x = lx + COL_NAME;
        if (recon_icon_draw(p, is_dir ? RECON_ICON_FOLDER : RECON_ICON_FILE,
                lx + 4, ry + 2, ROW_HEIGHT - 4)) {
            name_x = lx + 4 + (ROW_HEIGHT - 4) + 6;
        }

        if (index == ex->renaming && ex->rename_edit.active) {
            /*
             * The row becomes the text box. Editing the name where the name
             * already is leaves no doubt about what is being renamed.
             */
            recon_edit_draw(p, ex->font, name_x - 2, ry,
                lx + COL_TYPE - name_x - 6, ROW_HEIGHT, &ex->rename_edit);
        } else {
            recon_draw_text(p, ex->font, name_x, baseline,
                lx + COL_TYPE - name_x - 10, entry->name, text);
        }

        /* In the bin, the Type column is worth less than knowing where the
         * thing came from, so it says that instead. */
        if (ex->in_trash) {
            char origin[RECON_PATH_MAX];
            if (recon_fs_trash_origin(entry->name, origin, sizeof(origin))) {
                recon_draw_text(p, ex->font, lx + COL_TYPE, baseline,
                    lw - COL_TYPE - 8, origin, text);
            } else {
                recon_draw_text(p, ex->font, lx + COL_TYPE, baseline, 120,
                    recon_props_kind(entry, entry->name), text);
            }
        } else {
            /* The same name the properties box gives it. This said "File"
             * for everything that was not a folder, so a listing called
             * something a File while its own properties called it a Text
             * file. */
            recon_draw_text(p, ex->font, lx + COL_TYPE, baseline, 90,
                recon_props_kind(entry, entry->name), text);
        }

        if (!is_dir) {
            /* The same phrasing the properties box uses. This had its own
             * rounding, so a file could say "2.4 KB" in the listing and
             * "2441 bytes" in its properties -- which reads as two different
             * files rather than as two ways of saying one number. */
            char size[32];
            recon_props_size(entry->size, size, sizeof(size));
            recon_draw_text(p, ex->font, lx + COL_SIZE, baseline, 80, size, text);
        }

        recon_hit_add(p, lx, ry, lw, ROW_HEIGHT, HIT_ROW_BASE + row);
    }

    if (ex->entry_count == 0) {
        recon_draw_text(p, ex->font, lx + COL_NAME, ly + ROW_HEIGHT,
            lw - COL_NAME, "This folder is empty", RECON_RGB(0x80, 0x80, 0x80));
    }

    /*
     * Status, or -- while the pointer is over a toolbar icon -- what that
     * icon does. The hint wins because it answers a question the user is
     * asking right now.
     */
    int sy = y + h - STATUS_HEIGHT;
    bool hinting = ex->hint[0] != '\0';
    recon_fill_rect(p, x, sy, w, STATUS_HEIGHT, COLOR_BG);
    recon_draw_text(p, ex->font, x + PADDING, sy + (STATUS_HEIGHT + ascent) / 2 - 2,
        w - PADDING * 2, hinting ? ex->hint : ex->status,
        (!hinting && ex->status_is_warning) ? COLOR_WARNING : COLOR_STATUS);

    /*
     * The drop-down last of all, so it is drawn over the listing and, since
     * the topmost hit region wins, clicked in preference to it.
     */
    if (ex->places_open) {
        draw_places(ex, p, path_x, py + PATHBAR_HEIGHT, path_w,
            (sy - (py + PATHBAR_HEIGHT)) - 2);
    }
    draw_menu_dropdown(ex, p, x, y);
}

/* --- Input --- */

/*
 * What a menu entry does.
 *
 * Every one of these is something the toolbar or a shortcut can already do.
 * The menu is a second way to reach them with their names written out, which
 * is what a menu bar is for -- not a second implementation.
 */
static void do_menu_item(struct recon_explorer *ex, int menu, int item) {
    if (menu == 0) {           /* File */
        switch (item) {
        case 0: do_new_folder(ex); break;
        case 1: do_new_file(ex); break;
        case 2: do_begin_rename(ex); break;
        case 3: do_delete(ex); break;
        case 4: recon_appwin_hide(ex->win); break;
        default: break;
        }
        return;
    }

    if (menu == 1) {           /* Edit */
        switch (item) {
        case 0: do_clip(ex, true); break;
        case 1: do_clip(ex, false); break;
        case 2: do_paste(ex); break;
        default: break;
        }
        return;
    }

    switch (item) {            /* View */
    case 0: reload(ex); break;
    case 1: navigate(ex, recon_fs_user_dir(NULL)); break;
    case 2: navigate(ex, ".."); break;
    default: break;
    }
}

static bool explorer_click(void *user, uint32_t hit_id, int cx, int cy, bool pressed) {
    struct recon_explorer *ex = user;
    if (!pressed) {
        return false;
    }

    /* Anything other than pressing Delete again cancels a pending delete. */
    if (hit_id != HIT_DELETE) {
        cancel_delete(ex);
    }

    /*
     * An open menu takes the next click wherever it lands: it either chose an
     * entry or dismissed the menu. Answered first, because it is drawn over
     * everything and a click that looks like it landed on the listing landed
     * on the menu.
     */
    if (ex->menu_open >= 0) {
        int which = ex->menu_open;

        if (hit_id >= HIT_MENU_ITEM_BASE && hit_id < HIT_ROW_BASE) {
            int index = (int)(hit_id - HIT_MENU_ITEM_BASE);
            ex->menu_open = -1;
            ex->menu_item_hover = -1;
            do_menu_item(ex, which, index);
            return true;
        }

        ex->menu_open = -1;
        ex->menu_item_hover = -1;

        /* Clicking the title that opened it closes it and nothing more. */
        if (hit_id >= HIT_MENU_BASE && hit_id < HIT_SIDEBAR_BASE) {
            return true;
        }
    }

    if (hit_id >= HIT_MENU_BASE && hit_id < HIT_SIDEBAR_BASE) {
        int index = (int)(hit_id - HIT_MENU_BASE);
        if (index >= 0 && index < MENU_COUNT) {
            ex->menu_open = index;
            ex->menu_item_hover = -1;
            close_places(ex);
        }
        return true;
    }

    /*
     * An open drop-down takes the next click wherever it lands: it either
     * chose a place or dismissed the list. Handled before everything else so
     * a click that lands on the listing underneath dismisses rather than
     * selects -- a list that is dismissed *and* acted through is a list that
     * does something you did not ask for.
     */
    if (ex->places_open) {
        if (hit_id >= HIT_PLACE_BASE && hit_id < HIT_ROW_BASE) {
            int index = (int)(hit_id - HIT_PLACE_BASE);
            char path[RECON_PATH_MAX];
            bool valid = index >= 0 && index < ex->place_count;
            if (valid) {
                snprintf(path, sizeof(path), "%s", ex->places[index].path);
            }
            close_places(ex);
            if (valid) {
                navigate(ex, path);
            }
            return true;
        }
        close_places(ex);
        if (hit_id != HIT_PATH_DROP) {
            return true;
        }
        /* Clicking the button that opened it closes it, and nothing more. */
        return true;
    }

    /* Typing a path is abandoned by clicking anywhere but the field itself. */
    if (ex->typing_path && hit_id != HIT_PATH) {
        stop_typing_path(ex);
        clear_status(ex);
    }

    /*
     * Clicking away from a rename applies it, the way a name typed into a
     * listing behaves everywhere else. Only Escape throws the typing away.
     */
    if (ex->renaming >= 0 &&
            hit_id != (uint32_t)(HIT_ROW_BASE + ex->renaming - ex->scroll)) {
        do_commit_rename(ex);
    }

    /* Bounded at HIT_PLACE_BASE, not at HIT_ROW_BASE: the drop-down's ids sit
     * between the two, and a range that swallowed them would have turned
     * every place in the list into a sidebar entry. */
    if (hit_id >= HIT_SIDEBAR_BASE && hit_id < HIT_PLACE_BASE) {
        int index = (int)(hit_id - HIT_SIDEBAR_BASE);
        if (index >= 0 && index < SIDEBAR_COUNT) {
            char path[RECON_PATH_MAX];
            sidebar_path(index, path, sizeof(path));
            navigate(ex, path);
        }
        return true;
    }

    switch (hit_id) {
    case HIT_BACK:
        go_back(ex);
        return true;
    case HIT_FORWARD:
        go_forward(ex);
        return true;
    case HIT_UP:
        navigate(ex, "..");
        return true;
    case HIT_HOME:
        /* Home is the signed-in account's own folder. It used to be the root
         * of the filesystem, which is where a system starts rather than where
         * a person does. */
        navigate(ex, recon_fs_user_dir(NULL));
        return true;
    case HIT_PATH:
        if (!ex->typing_path) {
            begin_typing_path(ex);
        }
        return true;
    case HIT_PATH_DROP:
        build_places(ex);
        ex->places_open = ex->place_count > 0;
        return true;
    case HIT_REFRESH:
        reload(ex);
        return true;
    case HIT_NEWFOLDER:
        do_new_folder(ex);
        return true;
    case HIT_RENAME:
        do_begin_rename(ex);
        return true;
    case HIT_DELETE:
        do_delete(ex);
        return true;
    case HIT_RESTORE:
        do_restore(ex);
        return true;
    case HIT_EMPTY_BIN:
        do_empty_bin(ex);
        return true;
    default:
        break;
    }

    if (hit_id >= HIT_ROW_BASE) {
        int index = ex->scroll + (int)(hit_id - HIT_ROW_BASE);
        if (index < 0 || index >= ex->entry_count) {
            return true;
        }

        if (ex->selected == index) {
            /*
             * A second click on an already-selected row opens it, which is
             * how a double click behaves without needing to time one.
             *
             * This tested for a folder, so a second click on a file only
             * re-selected it -- which was consistent with there being nothing
             * that opened a file, and stopped being consistent the moment
             * there was.
             */
            do_open_selected(ex);
            return true;
        }

        ex->selected = index;
        const struct recon_dirent *entry = &ex->entries[index];
        if (entry->kind == RECON_FILE_DIRECTORY) {
            set_status(ex, false, "'%s' - click again to open", entry->name);
        } else {
            char size[32];
            recon_props_size(entry->size, size, sizeof(size));
            set_status(ex, false, "'%s' - %s", entry->name, size);
        }
        return true;
    }

    return false;
}

static bool explorer_key(void *user, xkb_keysym_t sym, uint32_t modifiers) {
    struct recon_explorer *ex = user;

    /* An open drop-down is dismissed by any key rather than navigated with
     * the arrows: the arrows already mean something in the listing behind it,
     * and two meanings for one key is how a keyboard stops being predictable. */
    if (ex->menu_open >= 0) {
        ex->menu_open = -1;
        ex->menu_item_hover = -1;
        if (sym == XKB_KEY_Escape) {
            return true;
        }
    }

    if (ex->places_open) {
        close_places(ex);
        if (sym == XKB_KEY_Escape) {
            return true;
        }
    }

    /* The address bar has the keyboard while it is being typed into. */
    if (ex->typing_path && ex->path_edit.active) {
        switch (recon_edit_key(&ex->path_edit, sym, modifiers)) {
        case RECON_EDIT_COMMIT: {
            char wanted[RECON_PATH_MAX];
            snprintf(wanted, sizeof(wanted), "%s", ex->path_edit.text);
            stop_typing_path(ex);
            navigate(ex, wanted);
            return true;
        }
        case RECON_EDIT_CANCEL:
            stop_typing_path(ex);
            clear_status(ex);
            return true;
        case RECON_EDIT_CHANGED:
        case RECON_EDIT_IGNORED:
            return true;
        }
    }

    /* While a name is being typed, the keyboard belongs to the editor: Up and
     * Delete mean things inside a word, not things to do to files. */
    if (ex->renaming >= 0 && ex->rename_edit.active) {
        switch (recon_edit_key(&ex->rename_edit, sym, modifiers)) {
        case RECON_EDIT_COMMIT:
            do_commit_rename(ex);
            return true;
        case RECON_EDIT_CANCEL:
            cancel_rename(ex);
            set_status(ex, false, "Rename cancelled");
            return true;
        case RECON_EDIT_CHANGED:
            return true;
        case RECON_EDIT_IGNORED:
            return true; /* Swallowed: nothing else should act on it. */
        }
    }

    bool ctrl = (modifiers & RECON_MOD_CTRL) != 0;

    if (ctrl) {
        switch (sym) {
        case XKB_KEY_x:
        case XKB_KEY_X:
            do_clip(ex, true);
            return true;
        case XKB_KEY_c:
        case XKB_KEY_C:
            do_clip(ex, false);
            return true;
        case XKB_KEY_v:
        case XKB_KEY_V:
            do_paste(ex);
            return true;
        default:
            break;
        }
    }

    switch (sym) {
    case XKB_KEY_F2:
        do_begin_rename(ex);
        return true;

    case XKB_KEY_Delete:
        /* Shift means skip the bin, the way it does everywhere else. */
        if (modifiers & RECON_MOD_SHIFT) {
            do_purge(ex);
        } else {
            do_delete(ex);
        }
        return true;

    case XKB_KEY_Up:
        if (ex->selected > 0) {
            ex->selected--;
        }
        if (ex->selected < ex->scroll) {
            ex->scroll = ex->selected;
        }
        return true;

    case XKB_KEY_Down:
        if (ex->selected + 1 < ex->entry_count) {
            ex->selected++;
        }
        if (ex->rows_visible > 0 && ex->selected >= ex->scroll + ex->rows_visible) {
            ex->scroll = ex->selected - ex->rows_visible + 1;
        }
        return true;

    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        if (ex->selected >= 0 && ex->selected < ex->entry_count &&
                ex->entries[ex->selected].kind == RECON_FILE_DIRECTORY) {
            navigate(ex, ex->entries[ex->selected].name);
        }
        return true;

    case XKB_KEY_BackSpace:
        navigate(ex, "..");
        return true;

    case XKB_KEY_Left:
        go_back(ex);
        return true;

    case XKB_KEY_Right:
        go_forward(ex);
        return true;

    case XKB_KEY_F5:
        reload(ex);
        return true;

    case XKB_KEY_Escape:
        cancel_delete(ex);
        ex->selected = -1;
        return true;

    default:
        return false;
    }
}

/*
 * Say what the thing under the pointer does.
 *
 * The toolbar is icons, and an icon is only faster than a word once you know
 * what it means. This is where that is learned: hover it, and the status bar
 * at the bottom says so in words.
 */
static void explorer_motion(void *user, uint32_t hit_id, int cx, int cy) {
    struct recon_explorer *ex = user;
    (void)cx;
    (void)cy;

    /* Same for an open menu: it covers what is underneath, so what is
     * underneath is not what the pointer is on. */
    if (ex->menu_open >= 0) {
        int hover = -1;
        if (hit_id >= HIT_MENU_ITEM_BASE && hit_id < HIT_ROW_BASE) {
            hover = (int)(hit_id - HIT_MENU_ITEM_BASE);
        }
        if (hover != ex->menu_item_hover) {
            ex->menu_item_hover = hover;
            recon_appwin_refresh(ex->win);
        }
        return;
    }

    /* An open list tracks what the pointer is over, and nothing else does
     * while it is up: it covers what is underneath, so what is underneath is
     * not what the pointer is on. */
    if (ex->places_open) {
        int hover = -1;
        if (hit_id >= HIT_PLACE_BASE && hit_id < HIT_ROW_BASE) {
            hover = (int)(hit_id - HIT_PLACE_BASE);
        }
        if (hover != ex->place_hover) {
            ex->place_hover = hover;
            recon_appwin_refresh(ex->win);
        }
        return;
    }

    const char *hint = NULL;
    switch (hit_id) {
    case HIT_BACK:      hint = "Back"; break;
    case HIT_FORWARD:   hint = "Forward"; break;
    case HIT_UP:        hint = "Up one folder"; break;
    case HIT_REFRESH:   hint = "Refresh this folder"; break;
    case HIT_HOME:      hint = "Your own folder"; break;
    case HIT_NEWFOLDER: hint = "New folder"; break;
    case HIT_RENAME:    hint = "Rename what is selected"; break;
    case HIT_DELETE:    hint = "Move to the Recycle Bin"; break;
    case HIT_RESTORE:   hint = "Put back where it came from"; break;
    case HIT_PATH:      hint = "Click to type a path"; break;
    case HIT_PATH_DROP: hint = "Go to another folder"; break;
    default:            break;
    }

    /* Only redrawn when it actually changed, so moving the pointer across a
     * button does not repaint the window on every step. */
    const char *now = hint != NULL ? hint : "";
    if (strcmp(ex->hint, now) != 0) {
        snprintf(ex->hint, sizeof(ex->hint), "%s", now);
        recon_appwin_refresh(ex->win);
    }
}

/*
 * The window stopped being in front, so anything that was only up because it
 * was in front goes away.
 *
 * Clicking elsewhere *inside* the window already closed the drop-down;
 * clicking outside it did not, because the window never saw that click. A
 * list left hanging over a window nobody is using belongs to nothing.
 */
static void explorer_focus_changed(void *user, bool focused) {
    struct recon_explorer *ex = user;
    if (focused) {
        return;
    }
    if (ex->places_open || ex->typing_path || ex->menu_open >= 0) {
        close_places(ex);
        stop_typing_path(ex);
        ex->menu_open = -1;
        ex->menu_item_hover = -1;
        recon_appwin_refresh(ex->win);
    }
}

static void explorer_scroll(void *user, double delta) {
    struct recon_explorer *ex = user;
    int max = ex->entry_count - ex->rows_visible;
    if (max < 0) {
        max = 0;
    }

    ex->scroll += (delta > 0) ? 3 : -3;
    if (ex->scroll > max) {
        ex->scroll = max;
    }
    if (ex->scroll < 0) {
        ex->scroll = 0;
    }
}

/*
 * What the explorer offers at a point.
 *
 * A right click on a file should be about the file. Returning false hands the
 * question back to the shell, which offers the window's own actions -- the
 * right answer for the toolbar or the path bar, where there is nothing of the
 * explorer's to do.
 */
/* Whether a name ends in something ReconOS can draw as a background. */
static bool looks_like_a_picture(const char *name) {
    static const char *const KINDS[] = { ".png", ".jpg", ".jpeg", ".bmp",
        NULL };

    size_t length = strlen(name);
    for (int i = 0; KINDS[i] != NULL; i++) {
        size_t kind = strlen(KINDS[i]);
        if (length > kind &&
                strcasecmp(name + length - kind, KINDS[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* And one ReconOS can install as a typeface. Beside its neighbour above,
 * because the two answer the same shape of question about a file name. */
static bool looks_like_a_font(const char *name) {
    static const char *const KINDS[] = { ".ttf", ".otf", ".ttc", NULL };

    size_t length = strlen(name);
    for (int i = 0; KINDS[i] != NULL; i++) {
        size_t kind = strlen(KINDS[i]);
        if (length > kind &&
                strcasecmp(name + length - kind, KINDS[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool explorer_context(void *user, uint32_t hit_id, int cx, int cy,
        struct recon_menu_spec *menu) {
    struct recon_explorer *ex = user;

    bool has_clip = !recon_fs_clip_empty();

    if (hit_id >= HIT_ROW_BASE) {
        int index = ex->scroll + (int)(hit_id - HIT_ROW_BASE);
        if (index < 0 || index >= ex->entry_count) {
            return false;
        }

        /* Right-clicking selects, so the menu acts on what it appeared over
         * rather than on whatever was selected beforehand. */
        cancel_rename(ex);
        cancel_delete(ex);
        ex->selected = index;

        const struct recon_dirent *entry = &ex->entries[index];
        bool is_dir = (entry->kind == RECON_FILE_DIRECTORY);
        bool protectedd = recon_fs_is_protected(ex->cwd, entry->name);

        /* In the bin the choices are different: things there are put back
         * or destroyed, not deleted again into the place they already are. */
        if (ex->in_trash) {
            recon_menu_add(menu, "Restore", EXCTX_RESTORE, true, true);
            recon_menu_add(menu, "Delete Permanently", EXCTX_PURGE, true, true);
            recon_menu_add(menu, "Empty Recycle Bin", EXCTX_EMPTY_BIN, true, false);
            return true;
        }

        recon_menu_add(menu, is_dir ? "Open" : "Select", EXCTX_OPEN, true, true);

        /*
         * A picture can become the background from wherever it is. Offered
         * only for something that looks like one: a menu entry that appears
         * on every file and fails on most of them teaches people to ignore
         * it.
         */
        if (!is_dir && looks_like_a_picture(entry->name)) {
            recon_menu_add(menu, "Set as Desktop Background",
                EXCTX_SET_WALLPAPER, true, true);
        }

        /* And a font, for the same reason and on the same terms. */
        if (!is_dir && looks_like_a_font(entry->name)) {
            recon_menu_add(menu, "Install Font", EXCTX_INSTALL_FONT, true,
                true);
        }

        recon_menu_add(menu, "Cut", EXCTX_CUT, !protectedd, false);
        recon_menu_add(menu, "Copy", EXCTX_COPY, true, false);
        recon_menu_add(menu, "Paste", EXCTX_PASTE, has_clip && is_dir, true);
        recon_menu_add(menu, "Rename", EXCTX_RENAME, !protectedd, false);
        recon_menu_add(menu, "Delete", EXCTX_DELETE, !protectedd, false);
        recon_menu_add(menu, "Delete Permanently", EXCTX_PURGE, !protectedd, true);
        recon_menu_add(menu, "Properties", EXCTX_PROPERTIES, true, false);
        return true;
    }

    if (hit_id >= HIT_SIDEBAR_BASE && hit_id < HIT_ROW_BASE) {
        return false; /* A place to go, not a thing to act on. */
    }

    /* Empty space in the listing: what can be made here. */
    if (cx >= ex->list_x && cx < ex->list_x + ex->list_w &&
            cy >= ex->list_y && cy < ex->list_y + ex->list_h) {
        cancel_rename(ex);
        cancel_delete(ex);
        ex->selected = -1;

        if (ex->in_trash) {
            recon_menu_add(menu, "Empty Recycle Bin", EXCTX_EMPTY_BIN,
                recon_fs_trash_count() > 0, true);
            recon_menu_add(menu, "Refresh", EXCTX_REFRESH, true, false);
            return true;
        }

        recon_menu_add(menu, "New Folder", EXCTX_NEW_FOLDER, true, false);
        recon_menu_add(menu, "New File", EXCTX_NEW_FILE, true, true);
        recon_menu_add(menu, "Paste", EXCTX_PASTE, has_clip, true);
        recon_menu_add(menu, "Refresh", EXCTX_REFRESH, true, false);
        return true;
    }

    return false;
}

static void explorer_context_action(void *user, uint32_t id) {
    struct recon_explorer *ex = user;

    switch ((enum explorer_context)id) {
    case EXCTX_SET_WALLPAPER: {
        if (ex->selected < 0 || ex->selected >= ex->entry_count) {
            break;
        }

        char name[RECON_NAME_MAX];
        if (!recon_wallpaper_add(ex->cwd, ex->entries[ex->selected].name,
                name, sizeof(name))) {
            set_status(ex, true, "%s cannot be used as a background.",
                ex->entries[ex->selected].name);
            break;
        }

        /*
         * Put on straight away. Somebody who asked for a picture to be the
         * background did not ask for it to be added to a list they would
         * then have to go and find it in.
         */
        recon_wallpaper_set(name);
        recon_background_reload(recon_appwin_server(ex->win));
        set_status(ex, false, "'%s' is the desktop background.", name);
        break;
    }

    case EXCTX_INSTALL_FONT: {
        if (ex->selected < 0 || ex->selected >= ex->entry_count) {
            break;
        }

        char name[RECON_NAME_MAX];
        if (!recon_fonts_add(ex->cwd, ex->entries[ex->selected].name,
                name, sizeof(name))) {
            set_status(ex, true, "%s", recon_fonts_last_error());
            break;
        }

        /*
         * Installed, not switched to. A wallpaper somebody right-clicked is
         * the one they want on the screen; a font is a thing they want the
         * system to have, and changing every letter on the desktop out from
         * under them without being asked is a different act.
         */
        set_status(ex, false, "'%s' is installed. Display Settings can draw "
            "with it.", name);
        break;
    }

    case EXCTX_OPEN:        do_open_selected(ex); break;
    case EXCTX_RENAME:      do_begin_rename(ex); break;
    case EXCTX_DELETE:      do_delete(ex); break;
    case EXCTX_CUT:         do_clip(ex, true); break;
    case EXCTX_COPY:        do_clip(ex, false); break;
    case EXCTX_PASTE:       do_paste(ex); break;
    case EXCTX_NEW_FOLDER:  do_new_folder(ex); break;
    case EXCTX_NEW_FILE:    do_new_file(ex); break;
    case EXCTX_REFRESH:     reload(ex); break;
    case EXCTX_PROPERTIES:  do_properties(ex); break;
    case EXCTX_PURGE:       do_purge(ex); break;
    case EXCTX_RESTORE:     do_restore(ex); break;
    case EXCTX_EMPTY_BIN:   do_empty_bin(ex); break;
    }
}

/* What the explorer currently believes, for when a button appears to do
 * nothing and the question is what it thought it was acting on. */
static void explorer_describe(void *user, char *out, size_t size) {
    struct recon_explorer *ex = user;

    const char *stage =
        ex->question == QUESTION_TRASH ? "asking: move to bin" :
        ex->question == QUESTION_PURGE ? "asking: delete permanently" :
        ex->question == QUESTION_EMPTY_BIN ? "asking: empty bin" : "idle";

    const char *selected = "(none)";
    if (ex->selected >= 0 && ex->selected < ex->entry_count) {
        selected = ex->entries[ex->selected].name;
    }

    snprintf(out, size,
        "  cwd: %s\n"
        "  entries: %d  scroll: %d  rows visible: %d\n"
        "  selected: [%d] %s\n"
        "  renaming: %d\n"
        "  question: %s target '%s'\n"
        "  in recycle bin: %s\n"
        "  typing path: %s\n"
        "  places open: %s (%d)\n"
        "  status: %s\n",
        ex->cwd, ex->entry_count, ex->scroll, ex->rows_visible,
        ex->selected, selected, ex->renaming,
        stage, ex->question_target, ex->in_trash ? "yes" : "no",
        ex->typing_path ? ex->path_edit.text : "no",
        ex->places_open ? "yes" : "no", ex->place_count, ex->status);
}

/* The listing may have changed while the window was closed. */
static void explorer_visibility(void *user, bool visible) {
    struct recon_explorer *ex = user;
    if (visible) {
        reload(ex);
    } else {
        /* Half-finished state should not be waiting when the window comes
         * back: a delete confirmed before it was hidden is not confirmed
         * still. */
        cancel_rename(ex);
        cancel_delete(ex);
    }
}

static void explorer_destroy(void *user) {
    free(user);
}

static const struct recon_appwin_impl EXPLORER_IMPL = {
    .title = "File Explorer",
    .help = "Files",
    .icon = RECON_ICON_EXPLORER,
    .default_width = 720,
    .default_height = 460,
    /* Wide enough for the whole toolbar; narrower and the buttons would be
     * there but unreachable. */
    .min_width = 560,
    .min_height = 260,
    .draw = explorer_draw,
    .click = explorer_click,
    .key = explorer_key,
    .motion = explorer_motion,
    .focus_changed = explorer_focus_changed,
    .scroll = explorer_scroll,
    .context = explorer_context,
    .context_action = explorer_context_action,
    .describe = explorer_describe,
    .visibility = explorer_visibility,
    .destroy = explorer_destroy,
};

void recon_explorer_open_at(struct recon_appwin *win, const char *path) {
    if (win == NULL || path == NULL || *path == '\0') {
        return;
    }
    /* Confirmed rather than assumed: handing this another application's
     * window would otherwise reinterpret its state as an explorer's. */
    if (strcmp(recon_appwin_title(win), EXPLORER_IMPL.title) != 0) {
        return;
    }

    struct recon_explorer *ex = recon_appwin_user(win);
    if (ex == NULL) {
        return;
    }

    navigate(ex, path);
    recon_appwin_refresh(win);
}

struct recon_appwin *recon_explorer_create(struct recon_server *server,
        struct recon_font *font) {
    struct recon_explorer *ex = calloc(1, sizeof(*ex));
    if (ex == NULL) {
        return NULL;
    }

    ex->font = font;
    ex->selected = -1;
    ex->renaming = -1;
    ex->place_hover = -1;
    /* Not zero. Zero is the File menu, so a calloc'd window came up believing
     * File was already open -- and the first click on File closed it instead
     * of opening it, which looked exactly like the menu bar doing nothing. */
    ex->menu_open = -1;
    ex->menu_item_hover = -1;

    /*
     * Where it was last, if that is still a folder. A remembered path can
     * have been deleted since, and opening onto an error is a worse greeting
     * than opening at home.
     */
    const char *remembered = recon_registry_get(RECON_REG_USER,
        "apps/explorer/last-folder", NULL);

    struct recon_dirent info;
    if (remembered != NULL && recon_fs_stat("/", remembered, &info) &&
            info.kind == RECON_FILE_DIRECTORY) {
        snprintf(ex->cwd, sizeof(ex->cwd), "%s", remembered);
    } else {
        snprintf(ex->cwd, sizeof(ex->cwd), "%s", recon_fs_user_dir(NULL));
    }
    history_push(ex, ex->cwd);
    reload(ex);

    ex->win = recon_appwin_create(server, font, &EXPLORER_IMPL, ex);
    if (ex->win == NULL) {
        free(ex);
        return NULL;
    }
    return ex->win;
}
