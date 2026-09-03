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

#include "recon_appwin.h"
#include "recon_icons.h"
#include "recon_explorer.h"
#include "recon_fs.h"
#include "recon_icons.h"
#include "recon_ui.h"

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
 * Places worth reaching in one click. The user's own folders first, then the
 * system, which is the order they are wanted in.
 */
struct shortcut_entry {
    const char *label;
    const char *path;   /* NULL means the current user's folder of that name */
    const char *icon;
};

/* A sidebar entry whose path is worked out rather than fixed. */
#define SIDEBAR_TRASH "\x01trash"

static const struct shortcut_entry SIDEBAR[] = {
    { "Desktop", NULL, RECON_ICON_FOLDER },
    { "Documents", NULL, RECON_ICON_FOLDER },
    { "Downloads", NULL, RECON_ICON_FOLDER },
    { "Pictures", NULL, RECON_ICON_FOLDER },
    { "Music", NULL, RECON_ICON_FOLDER },
    { "Videos", NULL, RECON_ICON_FOLDER },
    { "This System", "/", RECON_ICON_EXPLORER },
    { "Apps", RECON_DIR_APPS, RECON_ICON_APP },
    { "Recycle Bin", SIDEBAR_TRASH, RECON_ICON_TRASH },
};

#define SIDEBAR_COUNT ((int)(sizeof(SIDEBAR) / sizeof(SIDEBAR[0])))

#define COLOR_BG RECON_RGB(0xC0, 0xC0, 0xC0)
#define COLOR_LIST_BG RECON_RGB(0xFF, 0xFF, 0xFF)
#define COLOR_HEADER RECON_RGB(0xD4, 0xD4, 0xD4)
#define COLOR_TEXT RECON_RGB(0x10, 0x10, 0x10)
#define COLOR_DIR RECON_RGB(0x1A, 0x3A, 0x8B)
#define COLOR_ROW_ALT RECON_RGB(0xF2, 0xF2, 0xF6)
#define COLOR_SELECTED RECON_RGB(0x30, 0x50, 0x90)
#define COLOR_SELECTED_TEXT RECON_RGB(0xFF, 0xFF, 0xFF)
#define COLOR_BUTTON RECON_RGB(0xC8, 0xC8, 0xC8)
#define COLOR_PATH_BG RECON_RGB(0xFF, 0xFF, 0xFF)
#define COLOR_STATUS RECON_RGB(0x30, 0x30, 0x30)
#define COLOR_WARNING RECON_RGB(0x8B, 0x1A, 0x1A)

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
#define HIT_SIDEBAR_BASE (RECON_APPWIN_HIT_USER + 20)
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
    snprintf(base, sizeof(base), "%s", leaf);
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
    if (strcmp(ex->cwd, "/") == 0) {
        snprintf(target, sizeof(target), "/%s", name);
    } else {
        snprintf(target, sizeof(target), "%s/%s", ex->cwd, name);
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
    } else {
        set_status(ex, false, "'%s' - %zu bytes", entry->name, entry->size);
    }
}

/* --- Drawing --- */

static int draw_button(struct recon_explorer *ex, struct recon_panel *p,
        int x, int y, const char *label, uint32_t id, bool warning) {
    int ascent = recon_font_ascent(ex->font);
    int width = recon_text_width(ex->font, label) + 22;

    recon_fill_rect(p, x, y, width, BUTTON_HEIGHT, COLOR_BUTTON);
    recon_draw_bevel(p, x, y, width, BUTTON_HEIGHT, false);
    recon_draw_text(p, ex->font, x + 11, y + (BUTTON_HEIGHT + ascent) / 2 - 2,
        width - 16, label, warning ? COLOR_WARNING : COLOR_TEXT);
    recon_hit_add(p, x, y, width, BUTTON_HEIGHT, id);

    return x + width + BUTTON_GAP;
}

/*
 * A navigation button. direction is -1 for back, 1 for forward, 0 for up; the
 * glyph is drawn as a triangle so it reads at a glance and needs no font.
 */
static int draw_arrow_button(struct recon_explorer *ex, struct recon_panel *p,
        int x, int y, int direction, uint32_t id, bool enabled) {
    int width = 30;

    recon_fill_rect(p, x, y, width, BUTTON_HEIGHT, COLOR_BUTTON);
    recon_draw_bevel(p, x, y, width, BUTTON_HEIGHT, false);

    recon_color ink = enabled ? COLOR_TEXT : RECON_RGB(0x98, 0x98, 0x98);
    int cx = x + width / 2;
    int cy = y + BUTTON_HEIGHT / 2;

    if (direction == 0) {
        /* Up: a triangle over a stem. */
        for (int i = 0; i < 5; i++) {
            recon_fill_rect(p, cx - i, cy - 4 + i, i * 2 + 1, 1, ink);
        }
        recon_fill_rect(p, cx - 1, cy + 1, 3, 5, ink);
    } else {
        for (int i = 0; i < 5; i++) {
            int dx = direction < 0 ? cx + 2 - i : cx - 2 + i;
            recon_fill_rect(p, dx, cy - i, 1, i * 2 + 1, ink);
        }
    }

    /* Only offer the click when it would do something. */
    if (enabled) {
        recon_hit_add(p, x, y, width, BUTTON_HEIGHT, id);
    }
    return x + width + 2;
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
    } else if (strcmp(SIDEBAR[index].path, SIDEBAR_TRASH) == 0) {
        snprintf(out, size, "%s", recon_fs_trash_dir());
    } else {
        snprintf(out, size, "%s", SIDEBAR[index].path);
    }
}

/* The sidebar: places worth reaching without walking the tree. */
static void draw_sidebar(struct recon_explorer *ex, struct recon_panel *p,
        int x, int y, int h) {
    int ascent = recon_font_ascent(ex->font);

    recon_fill_rect(p, x, y, SIDEBAR_WIDTH, h, COLOR_BG);
    recon_fill_rect(p, x + SIDEBAR_WIDTH - 1, y, 1, h, RECON_RGB(0x90, 0x90, 0x90));

    for (int i = 0; i < SIDEBAR_COUNT; i++) {
        int iy = y + 4 + i * 24;
        if (iy + 24 > y + h) {
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
    }
}

static void explorer_draw(void *user, struct recon_panel *p,
        int x, int y, int w, int h) {
    struct recon_explorer *ex = user;
    int ascent = recon_font_ascent(ex->font);

    recon_fill_rect(p, x, y, w, h, COLOR_BG);

    /*
     * Toolbar: where you have been on the left, then what you can do. The
     * arrows are drawn rather than lettered, since a direction reads faster
     * as a shape than as a word.
     */
    int bx = x + PADDING;
    int by = y + (TOOLBAR_HEIGHT - BUTTON_HEIGHT) / 2;

    bool can_back = ex->history_pos > 0;
    bool can_forward = ex->history_pos + 1 < ex->history_count;

    bx = draw_arrow_button(ex, p, bx, by, -1, HIT_BACK, can_back);
    bx = draw_arrow_button(ex, p, bx, by, 1, HIT_FORWARD, can_forward);
    bx = draw_arrow_button(ex, p, bx, by, 0, HIT_UP, strcmp(ex->cwd, "/") != 0);
    bx += 6;
    bx = draw_button(ex, p, bx, by, "Home", HIT_HOME, false);
    bx = draw_button(ex, p, bx, by, "Refresh", HIT_REFRESH, false);
    bx = draw_button(ex, p, bx, by, "New Folder", HIT_NEWFOLDER, false);
    bx = draw_button(ex, p, bx, by, "Rename", HIT_RENAME, false);

    /*
     * Inside the bin the actions are different, so the button says so. The
     * label is fixed for a given place rather than changing as a
     * confirmation: a button that grows between the first click and the
     * second moves out from under the pointer.
     */
    if (ex->in_trash) {
        bx = draw_button(ex, p, bx, by, "Restore", HIT_RESTORE, false);
        draw_button(ex, p, bx, by, "Empty Bin", HIT_EMPTY_BIN, true);
    } else {
        draw_button(ex, p, bx, by, "Delete", HIT_DELETE, false);
    }

    /* Path bar. */
    int py = y + TOOLBAR_HEIGHT;
    recon_fill_rect(p, x + PADDING, py, w - PADDING * 2, PATHBAR_HEIGHT, COLOR_PATH_BG);
    recon_draw_bevel(p, x + PADDING, py, w - PADDING * 2, PATHBAR_HEIGHT, true);
    recon_draw_text(p, ex->font, x + PADDING + 6,
        py + (PATHBAR_HEIGHT + ascent) / 2 - 2, w - PADDING * 2 - 12,
        ex->cwd, COLOR_TEXT);

    /* Sidebar down the left, listing to the right of it. */
    int body_y = py + PATHBAR_HEIGHT + 2;
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
                    is_dir ? "Folder" : "File", text);
            }
        } else {
            recon_draw_text(p, ex->font, lx + COL_TYPE, baseline, 80,
                is_dir ? "Folder" : "File", text);
        }

        if (!is_dir) {
            char size[32];
            if (entry->size >= 1024 * 1024) {
                snprintf(size, sizeof(size), "%.1f MB", entry->size / (1024.0 * 1024.0));
            } else if (entry->size >= 1024) {
                snprintf(size, sizeof(size), "%.1f KB", entry->size / 1024.0);
            } else {
                snprintf(size, sizeof(size), "%zu B", entry->size);
            }
            recon_draw_text(p, ex->font, lx + COL_SIZE, baseline, 80, size, text);
        }

        recon_hit_add(p, lx, ry, lw, ROW_HEIGHT, HIT_ROW_BASE + row);
    }

    if (ex->entry_count == 0) {
        recon_draw_text(p, ex->font, lx + COL_NAME, ly + ROW_HEIGHT,
            lw - COL_NAME, "This folder is empty", RECON_RGB(0x80, 0x80, 0x80));
    }

    /* Status. */
    int sy = y + h - STATUS_HEIGHT;
    recon_fill_rect(p, x, sy, w, STATUS_HEIGHT, COLOR_BG);
    recon_draw_text(p, ex->font, x + PADDING, sy + (STATUS_HEIGHT + ascent) / 2 - 2,
        w - PADDING * 2, ex->status,
        ex->status_is_warning ? COLOR_WARNING : COLOR_STATUS);
}

/* --- Input --- */

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
     * Clicking away from a rename applies it, the way a name typed into a
     * listing behaves everywhere else. Only Escape throws the typing away.
     */
    if (ex->renaming >= 0 &&
            hit_id != (uint32_t)(HIT_ROW_BASE + ex->renaming - ex->scroll)) {
        do_commit_rename(ex);
    }

    if (hit_id >= HIT_SIDEBAR_BASE && hit_id < HIT_ROW_BASE) {
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
        navigate(ex, "/");
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

        if (ex->selected == index &&
                ex->entries[index].kind == RECON_FILE_DIRECTORY) {
            /* A second click on an already-selected folder opens it, which is
             * how a double click behaves without needing to time one. */
            navigate(ex, ex->entries[index].name);
            return true;
        }

        ex->selected = index;
        const struct recon_dirent *entry = &ex->entries[index];
        if (entry->kind == RECON_FILE_DIRECTORY) {
            set_status(ex, false, "'%s' - click again to open", entry->name);
        } else {
            set_status(ex, false, "'%s' - %zu bytes", entry->name, entry->size);
        }
        return true;
    }

    return false;
}

static bool explorer_key(void *user, xkb_keysym_t sym, uint32_t modifiers) {
    struct recon_explorer *ex = user;

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
        recon_menu_add(menu, "Cut", EXCTX_CUT, !protectedd, false);
        recon_menu_add(menu, "Copy", EXCTX_COPY, true, false);
        recon_menu_add(menu, "Paste", EXCTX_PASTE, has_clip && is_dir, true);
        recon_menu_add(menu, "Rename", EXCTX_RENAME, !protectedd, false);
        recon_menu_add(menu, "Delete", EXCTX_DELETE, !protectedd, false);
        recon_menu_add(menu, "Delete Permanently", EXCTX_PURGE, !protectedd, true);
        /* Shown but unavailable: there is no properties view yet, and hiding
         * it would suggest there never will be. */
        recon_menu_add(menu, "Properties", EXCTX_PROPERTIES, false, false);
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
    case EXCTX_OPEN:        do_open_selected(ex); break;
    case EXCTX_RENAME:      do_begin_rename(ex); break;
    case EXCTX_DELETE:      do_delete(ex); break;
    case EXCTX_CUT:         do_clip(ex, true); break;
    case EXCTX_COPY:        do_clip(ex, false); break;
    case EXCTX_PASTE:       do_paste(ex); break;
    case EXCTX_NEW_FOLDER:  do_new_folder(ex); break;
    case EXCTX_NEW_FILE:    do_new_file(ex); break;
    case EXCTX_REFRESH:     reload(ex); break;
    case EXCTX_PROPERTIES:  break; /* Offered as disabled; never chosen. */
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
        "  status: %s\n",
        ex->cwd, ex->entry_count, ex->scroll, ex->rows_visible,
        ex->selected, selected, ex->renaming,
        stage, ex->question_target, ex->in_trash ? "yes" : "no", ex->status);
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
    snprintf(ex->cwd, sizeof(ex->cwd), "%s", recon_fs_user_dir(NULL));
    history_push(ex, ex->cwd);
    reload(ex);

    ex->win = recon_appwin_create(server, font, &EXPLORER_IMPL, ex);
    if (ex->win == NULL) {
        free(ex);
        return NULL;
    }
    return ex->win;
}
