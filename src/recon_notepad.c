/*
 * ReconOS Notepad. See include/recon_notepad.h.
 *
 * Text is held as one buffer with newlines in it, and the cursor is an offset
 * into that buffer. Keeping a single source of truth means insertion and
 * deletion cannot leave the cursor pointing somewhere the text does not agree
 * with, which is the usual way simple editors go wrong.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_appwin.h"
#include "recon_filedlg.h"
#include "recon_fs.h"
#include "recon_icons.h"
#include "recon_notepad.h"
#include "recon_theme.h"
#include "recon_ui.h"

#define PADDING 6
#define MENUBAR_HEIGHT 22
#define STATUS_HEIGHT 22
#define LINE_SPACING 2
#define TAB_WIDTH 4

#define MENU_WIDTH 150
#define MENU_ITEM_HEIGHT 22

#define COLOR_BG THEME(SURFACE)
#define COLOR_TEXT THEME(SURFACE_TEXT)
#define COLOR_CURSOR THEME(CARET)
#define COLOR_STATUS_BG THEME(WINDOW_FRAME)
#define COLOR_STATUS_TEXT THEME(SURFACE_TEXT_DIM)
#define COLOR_MENUBAR THEME(MENU)
#define COLOR_MENU THEME(MENU)
#define COLOR_MENU_BORDER THEME(MENU_BORDER)
#define COLOR_MENU_HILITE THEME(MENU_HILITE)
#define COLOR_MENU_HILITE_TEXT THEME(MENU_HILITE_TEXT)
/* A menu entry's shortcut, which is a reminder rather than another thing to
 * read -- so it uses the colour a menu gives to something unavailable. */
#define COLOR_MENU_DIM THEME(MENU_TEXT_DISABLED)
#define COLOR_WARNING THEME(WARNING)

#define HIT_TEXT (RECON_APPWIN_HIT_USER + 2)
/* The entries inside whichever menu is down. */
#define HIT_MENU_BASE (RECON_APPWIN_HIT_USER + 10)
/*
 * The names along the bar, one id each.
 *
 * Its own range with room in it. This was a single HIT_FILE next to HIT_TEXT,
 * and adding a second menu made its id the text area's -- so clicking Edit
 * put the caret in the document and the menu never opened. A range with a gap
 * after it is what stops the next menu doing the same thing.
 */
#define HIT_MENUBAR_BASE (RECON_APPWIN_HIT_USER + 40)

/* The File menu, in the order it is shown. */
enum file_command {
    FILE_NEW,
    FILE_OPEN,
    FILE_SAVE,
    FILE_SAVE_AS,
    FILE_CLOSE,
    EDIT_UNDO,
    EDIT_REDO,
};

struct menu_item {
    const char *label;
    /* What the keyboard does instead, shown beside the entry. A shortcut
     * nobody can see is a shortcut only the person who wrote it knows. */
    const char *shortcut;
    enum file_command command;
    bool separator_after;
};

static const struct menu_item FILE_MENU[] = {
    { "New",        "Ctrl+N", FILE_NEW,     false },
    { "Open...",    "Ctrl+O", FILE_OPEN,    true  },
    { "Save",       "Ctrl+S", FILE_SAVE,    false },
    { "Save As...", "",       FILE_SAVE_AS, true  },
    { "Close",      "",       FILE_CLOSE,   false },
};

static const struct menu_item EDIT_MENU[] = {
    { "Undo", "Ctrl+Z", EDIT_UNDO, false },
    { "Redo", "Ctrl+Y", EDIT_REDO, false },
};

/*
 * The menu bar, in the order it is drawn.
 *
 * A table rather than two of everything, because the second menu is where a
 * menu bar written for one starts drawing the first one's items under the
 * second one's name.
 */
static const struct {
    const char *name;
    const struct menu_item *items;
    int count;
} MENUS[] = {
    { "File", FILE_MENU, (int)(sizeof(FILE_MENU) / sizeof(FILE_MENU[0])) },
    { "Edit", EDIT_MENU, (int)(sizeof(EDIT_MENU) / sizeof(EDIT_MENU[0])) },
};

#define MENU_COUNT ((int)(sizeof(MENUS) / sizeof(MENUS[0])))

/*
 * What the file dialog was opened for. The dialog itself does not know or care
 * -- it answers with a path -- so the notepad remembers what it asked.
 */
enum pending_action {
    PENDING_NONE,
    PENDING_OPEN,
    PENDING_SAVE,
    /* Saving because the window is being closed: once written, close it. */
    PENDING_SAVE_THEN_CLOSE,
};

/* --- Undo --- */

/* The editor's own state, defined below: the history is described here
 * because the struct holds it, and operates on the struct in turn. */
struct recon_notepad;

/* Defined with the drawing, below; the history needs it to keep the cursor
 * on screen after stepping back. */
static void scroll_to_cursor(struct recon_notepad *np);
static void set_message(struct recon_notepad *np, bool warning,
    const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static bool ensure_capacity(struct recon_notepad *np, size_t needed);


/*
 * Every change, and how to put it back.
 *
 * A text editor that cannot undo is a text editor that loses work, and this
 * one now opens files by being clicked -- so more text passes through it than
 * when the only way in was to type it.
 *
 * An edit records what changed and where, not a copy of the whole document.
 * Snapshots are simpler and would mean holding a hundred copies of a file to
 * be able to step back a hundred keystrokes.
 */
#define UNDO_MAX 256

struct edit {
    size_t at;       /* where in the text it happened */
    char *text;      /* what was put in, or taken out */
    size_t length;
    bool insert;     /* true when this edit added the text */
    size_t cursor_before;
};

struct recon_notepad {
    struct recon_font *font;
    struct recon_appwin *win;

    char *text;
    size_t length;
    size_t capacity;
    size_t cursor; /* byte offset into text */

    int scroll_line;
    int visible_lines;
    bool modified;

    /* Where this text came from, and where Save writes. Empty means it has
     * never been saved, which is what makes Save fall through to Save As. */
    char path[RECON_PATH_MAX];

    /* Which menu is showing, or -1. A bool was enough for one menu. */
    int menu_open;
    int menu_hover;

    /*
     * What was done, and where in that history we are.
     *
     * `undo_at` is not the same as `undo_count`: undoing walks it back
     * without discarding what it walked past, which is what makes redo
     * possible. Typing something new is what throws the rest away.
     */
    struct edit undo[UNDO_MAX];
    int undo_count;
    int undo_at;
    /* True while undo or redo is changing the text, so the change they make
     * is not recorded as another thing to undo. */
    bool replaying;

    struct recon_filedlg dialog;
    enum pending_action pending;

    char message[160];
    bool message_is_warning;
};

/* --- Files --- */

static void set_message(struct recon_notepad *np, bool warning, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void set_message(struct recon_notepad *np, bool warning, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(np->message, sizeof(np->message), fmt, args);
    va_end(args);
    np->message_is_warning = warning;
}

/* The name shown in the title bar and on the taskbar. */
static void update_title(struct recon_notepad *np) {
    if (np->win == NULL) {
        return;
    }

    if (np->path[0] == '\0') {
        recon_appwin_set_title(np->win, np->modified ? "Untitled * - Notepad"
                                                     : "Notepad");
        return;
    }

    const char *leaf = strrchr(np->path, '/');
    leaf = (leaf != NULL && leaf[1] != '\0') ? leaf + 1 : np->path;

    /* Trimmed deliberately rather than left to snprintf: a title bar has
     * room for a name, not for a path, and a long one should lose its middle
     * rather than its " - Notepad". */
    char shown[64];
    snprintf(shown, sizeof(shown), "%s", leaf);

    char title[96];
    snprintf(title, sizeof(title), "%s%s - Notepad", shown,
        np->modified ? " *" : "");
    recon_appwin_set_title(np->win, title);
}

/* --- Buffer --- */

static bool ensure_capacity(struct recon_notepad *np, size_t needed) {
    if (needed + 1 <= np->capacity) {
        return true;
    }
    size_t capacity = np->capacity ? np->capacity : 256;
    while (capacity < needed + 1) {
        capacity *= 2;
    }
    char *grown = realloc(np->text, capacity);
    if (grown == NULL) {
        return false;
    }
    np->text = grown;
    np->capacity = capacity;
    return true;
}

/* Whitespace, for deciding where one typed word ends and the next starts. */
static bool is_space(char c) {
    return c == ' ' || c == '\t';
}

static void edit_free(struct edit *edit) {
    free(edit->text);
    edit->text = NULL;
    edit->length = 0;
}

/* Drop everything ahead of where we are: once something new is typed, the
 * future that was undone is not reachable any more, and keeping it would let
 * redo paste it back into a document it no longer fits. */
static void drop_redo(struct recon_notepad *np) {
    for (int i = np->undo_count - 1; i >= np->undo_at; i--) {
        edit_free(&np->undo[i]);
    }
    np->undo_count = np->undo_at;
}

static struct edit *push_edit(struct recon_notepad *np) {
    drop_redo(np);

    if (np->undo_count == UNDO_MAX) {
        /* The oldest goes. A bounded history is the price of not growing
         * without limit while somebody holds a key down. */
        edit_free(&np->undo[0]);
        memmove(&np->undo[0], &np->undo[1],
            sizeof(np->undo[0]) * (UNDO_MAX - 1));
        np->undo_count--;
    }

    struct edit *edit = &np->undo[np->undo_count];
    memset(edit, 0, sizeof(*edit));
    np->undo_count++;
    np->undo_at = np->undo_count;
    return edit;
}

/* Add to the edit at the top of the stack, if this change continues it. */
static bool extend_edit(struct recon_notepad *np, size_t at, char c,
        bool insert) {
    if (np->undo_at == 0 || np->undo_at != np->undo_count) {
        return false;
    }

    struct edit *edit = &np->undo[np->undo_count - 1];
    if (edit->insert != insert) {
        return false;
    }

    /*
     * Where one edit ends and the next begins.
     *
     * Undo works in units of what somebody would call "a thing I did", and
     * the unit people think in is a word. Grouping only at line breaks made
     * typing a sentence a single undo -- one press and the whole line was
     * gone, with no way back to the word.
     *
     * So: a line break is always a boundary, and a word ends when the run of
     * spaces after it does. That keeps the trailing space with the word it
     * follows, so undoing gives back "hello " and then nothing, rather than
     * "hello", then " ", then nothing.
     */
    char last = '\0';
    if (edit->length > 0) {
        /* Deleting prepends, so index zero is the most recent character. */
        last = insert ? edit->text[edit->length - 1] : edit->text[0];
    }

    if (c == '\n' || last == '\n') {
        return false;
    }
    if (is_space(last) && !is_space(c)) {
        return false;
    }

    if (insert) {
        /* Typing carries on from where the last character went. */
        if (at != edit->at + edit->length) {
            return false;
        }
    } else {
        /* Backspace walks backwards, so each removal is just before the last.
         * The text is prepended and the edit's start moves with it. */
        if (at + 1 != edit->at) {
            return false;
        }
    }

    char *grown = realloc(edit->text, edit->length + 1);
    if (grown == NULL) {
        return false;
    }
    edit->text = grown;

    if (insert) {
        edit->text[edit->length] = c;
    } else {
        memmove(edit->text + 1, edit->text, edit->length);
        edit->text[0] = c;
        edit->at = at;
    }
    edit->length++;
    return true;
}

/* Remember one character going in or out. */
static void record(struct recon_notepad *np, size_t at, char c, bool insert,
        size_t cursor_before) {
    if (np->replaying) {
        return;    /* Undo and redo are not themselves edits. */
    }

    if (extend_edit(np, at, c, insert)) {
        return;
    }

    struct edit *edit = push_edit(np);
    edit->text = malloc(1);
    if (edit->text == NULL) {
        np->undo_count--;
        np->undo_at = np->undo_count;
        return;
    }
    edit->text[0] = c;
    edit->length = 1;
    edit->at = at;
    edit->insert = insert;
    edit->cursor_before = cursor_before;
}

/* Put text back, or take it out again, without recording it as a new edit. */
static void apply_edit(struct recon_notepad *np, const struct edit *edit,
        bool as_insert) {
    np->replaying = true;

    if (as_insert) {
        if (ensure_capacity(np, np->length + edit->length)) {
            memmove(np->text + edit->at + edit->length, np->text + edit->at,
                np->length - edit->at + 1);
            memcpy(np->text + edit->at, edit->text, edit->length);
            np->length += edit->length;
            np->cursor = edit->at + edit->length;
        }
    } else {
        if (edit->at + edit->length <= np->length) {
            memmove(np->text + edit->at, np->text + edit->at + edit->length,
                np->length - edit->at - edit->length + 1);
            np->length -= edit->length;
            np->cursor = edit->at;
        }
    }

    np->replaying = false;
    np->modified = true;
}

static void do_undo(struct recon_notepad *np) {
    if (np->undo_at == 0) {
        set_message(np, false, "Nothing to undo.");
        return;
    }

    np->undo_at--;
    const struct edit *edit = &np->undo[np->undo_at];

    /* The reverse of what it did. */
    apply_edit(np, edit, !edit->insert);
    if (edit->insert) {
        np->cursor = edit->at;
    }
    scroll_to_cursor(np);
    np->message[0] = '\0';
}

static void do_redo(struct recon_notepad *np) {
    if (np->undo_at >= np->undo_count) {
        set_message(np, false, "Nothing to redo.");
        return;
    }

    const struct edit *edit = &np->undo[np->undo_at];
    np->undo_at++;

    apply_edit(np, edit, edit->insert);
    scroll_to_cursor(np);
    np->message[0] = '\0';
}

/* Everything forgotten, for when the document is replaced rather than edited. */
static void forget_history(struct recon_notepad *np) {
    for (int i = 0; i < np->undo_count; i++) {
        edit_free(&np->undo[i]);
    }
    np->undo_count = 0;
    np->undo_at = 0;
}

static void insert_char(struct recon_notepad *np, char c) {
    if (!ensure_capacity(np, np->length + 1)) {
        return;
    }
    record(np, np->cursor, c, true, np->cursor);

    memmove(np->text + np->cursor + 1, np->text + np->cursor,
        np->length - np->cursor + 1);
    np->text[np->cursor] = c;
    np->length++;
    np->cursor++;
    np->modified = true;
}

static void delete_before_cursor(struct recon_notepad *np) {
    if (np->cursor == 0) {
        return;
    }
    record(np, np->cursor - 1, np->text[np->cursor - 1], false, np->cursor);

    memmove(np->text + np->cursor - 1, np->text + np->cursor,
        np->length - np->cursor + 1);
    np->cursor--;
    np->length--;
    np->modified = true;
}

static void delete_at_cursor(struct recon_notepad *np) {
    if (np->cursor >= np->length) {
        return;
    }
    record(np, np->cursor, np->text[np->cursor], false, np->cursor);

    memmove(np->text + np->cursor, np->text + np->cursor + 1,
        np->length - np->cursor);
    np->length--;
    np->modified = true;
}

/* --- Cursor navigation --- */

/* Offset of the start of the line containing `pos`. */
static size_t line_start(struct recon_notepad *np, size_t pos) {
    while (pos > 0 && np->text[pos - 1] != '\n') {
        pos--;
    }
    return pos;
}

/* Offset of the newline ending this line, or the end of the text. */
static size_t line_end(struct recon_notepad *np, size_t pos) {
    while (pos < np->length && np->text[pos] != '\n') {
        pos++;
    }
    return pos;
}

static int line_number(struct recon_notepad *np, size_t pos) {
    int line = 0;
    for (size_t i = 0; i < pos && i < np->length; i++) {
        if (np->text[i] == '\n') {
            line++;
        }
    }
    return line;
}

static int column_number(struct recon_notepad *np, size_t pos) {
    return (int)(pos - line_start(np, pos));
}

static void move_up(struct recon_notepad *np) {
    size_t start = line_start(np, np->cursor);
    if (start == 0) {
        return;
    }
    int column = (int)(np->cursor - start);

    size_t previous_start = line_start(np, start - 1);
    size_t previous_end = start - 1;
    size_t target = previous_start + (size_t)column;
    np->cursor = target < previous_end ? target : previous_end;
}

static void move_down(struct recon_notepad *np) {
    size_t end = line_end(np, np->cursor);
    if (end >= np->length) {
        return;
    }
    int column = column_number(np, np->cursor);

    size_t next_start = end + 1;
    size_t next_end = line_end(np, next_start);
    size_t target = next_start + (size_t)column;
    np->cursor = target < next_end ? target : next_end;
}

/* Keep the cursor's line on screen after it moves. */
static void scroll_to_cursor(struct recon_notepad *np) {
    int line = line_number(np, np->cursor);

    if (line < np->scroll_line) {
        np->scroll_line = line;
    } else if (np->visible_lines > 0 && line >= np->scroll_line + np->visible_lines) {
        np->scroll_line = line - np->visible_lines + 1;
    }
    if (np->scroll_line < 0) {
        np->scroll_line = 0;
    }
}

static void set_text(struct recon_notepad *np, const char *text, size_t length) {
    if (!ensure_capacity(np, length)) {
        return;
    }
    memcpy(np->text, text, length);
    np->text[length] = '\0';
    np->length = length;
    np->cursor = 0;
    np->scroll_line = 0;

    /*
     * A new document has no history worth keeping.
     *
     * Undoing past the moment a file was opened would put the previous
     * document's characters back into this one a few at a time -- which is
     * not "the change before this one" by any reading, and is how an editor
     * quietly corrupts a file somebody trusted it with.
     */
    forget_history(np);
}

static void do_new(struct recon_notepad *np) {
    set_text(np, "", 0);
    np->path[0] = '\0';
    np->modified = false;
    set_message(np, false, "New document");
    update_title(np);
}

static bool load_from(struct recon_notepad *np, const char *path) {
    size_t size = 0;
    char *data = recon_fs_read("/", path, &size);
    if (data == NULL) {
        set_message(np, true, "%s", recon_fs_last_error());
        return false;
    }

    set_text(np, data, size);
    free(data);

    snprintf(np->path, sizeof(np->path), "%s", path);
    np->modified = false;
    set_message(np, false, "Opened '%s'", path);
    update_title(np);
    return true;
}

static bool save_to(struct recon_notepad *np, const char *path) {
    if (!recon_fs_write("/", path, np->text, np->length)) {
        set_message(np, true, "%s", recon_fs_last_error());
        return false;
    }

    snprintf(np->path, sizeof(np->path), "%s", path);
    np->modified = false;
    set_message(np, false, "Saved to '%s'", path);
    update_title(np);
    return true;
}

/* The folder a dialog should start in: where this file already lives, or the
 * user's documents when it has no home yet. */
static void start_folder(struct recon_notepad *np, char *out, size_t size) {
    if (np->path[0] == '\0') {
        snprintf(out, size, "%s", recon_fs_user_dir("Documents"));
        return;
    }

    snprintf(out, size, "%s", np->path);
    char *slash = strrchr(out, '/');
    if (slash == out) {
        out[1] = '\0';   /* A file directly in the root. */
    } else if (slash != NULL) {
        *slash = '\0';
    } else {
        snprintf(out, size, "%s", recon_fs_user_dir("Documents"));
    }
}

/* The name to offer when saving: what it is called now, or a new default. */
static void suggested_name(struct recon_notepad *np, char *out, size_t size) {
    if (np->path[0] == '\0') {
        snprintf(out, size, "Untitled.txt");
        return;
    }
    const char *leaf = strrchr(np->path, '/');
    snprintf(out, size, "%s",
        (leaf != NULL && leaf[1] != '\0') ? leaf + 1 : np->path);
}

static void ask_save_as(struct recon_notepad *np, enum pending_action pending) {
    char folder[RECON_PATH_MAX];
    char name[RECON_NAME_MAX];
    start_folder(np, folder, sizeof(folder));
    suggested_name(np, name, sizeof(name));

    np->pending = pending;
    recon_filedlg_open(&np->dialog, RECON_FILEDLG_SAVE, "Save As", folder, name);
}

static void do_save(struct recon_notepad *np, enum pending_action pending) {
    /* A file that has never been saved has nowhere to go, so Save has to ask
     * where -- it becomes Save As rather than failing. */
    if (np->path[0] == '\0') {
        ask_save_as(np, pending);
        return;
    }

    if (save_to(np, np->path) && pending == PENDING_SAVE_THEN_CLOSE) {
        recon_appwin_hide(np->win);
    }
}

static void run_command(struct recon_notepad *np, enum file_command command) {
    np->menu_open = -1;

    switch (command) {
    case FILE_NEW:
        do_new(np);
        break;

    case FILE_OPEN: {
        char folder[RECON_PATH_MAX];
        start_folder(np, folder, sizeof(folder));
        np->pending = PENDING_OPEN;
        recon_filedlg_open(&np->dialog, RECON_FILEDLG_OPEN, "Open", folder, NULL);
        break;
    }

    case FILE_SAVE:
        do_save(np, PENDING_SAVE);
        break;

    case FILE_SAVE_AS:
        ask_save_as(np, PENDING_SAVE);
        break;

    case EDIT_UNDO:
        do_undo(np);
        break;

    case EDIT_REDO:
        do_redo(np);
        break;

    case FILE_CLOSE:
        /*
         * Unsaved work is not thrown away on a click. Closing asks where to
         * put it first, and only then closes.
         */
        if (np->modified) {
            do_save(np, PENDING_SAVE_THEN_CLOSE);
        } else {
            recon_appwin_hide(np->win);
        }
        break;
    }
}

/* What the dialog answered. */
static void finish_dialog(struct recon_notepad *np) {
    const char *path = recon_filedlg_path(&np->dialog);
    enum pending_action pending = np->pending;
    np->pending = PENDING_NONE;

    switch (pending) {
    case PENDING_OPEN:
        load_from(np, path);
        break;
    case PENDING_SAVE:
        save_to(np, path);
        break;
    case PENDING_SAVE_THEN_CLOSE:
        if (save_to(np, path)) {
            recon_appwin_hide(np->win);
        }
        break;
    case PENDING_NONE:
        break;
    }
}

/* --- Drawing --- */

/* Where a menu's name starts on the bar, so its dropdown opens under it. */
static int menu_left(struct recon_notepad *np, int x, int which) {
    int at = x + 2;
    for (int i = 0; i < which && i < MENU_COUNT; i++) {
        at += recon_text_width(np->font, MENUS[i].name) + 20;
    }
    return at;
}

/* The menu bar. Every menu's name, and which of them is down. */
static void draw_menubar(struct recon_notepad *np, struct recon_panel *panel,
        int x, int y, int w) {
    int ascent = recon_font_ascent(np->font);

    recon_fill_rect(panel, x, y, w, MENUBAR_HEIGHT, COLOR_MENUBAR);
    recon_fill_rect(panel, x, y + MENUBAR_HEIGHT - 1, w, 1,
        RECON_RGB(0x90, 0x90, 0x90));

    for (int i = 0; i < MENU_COUNT; i++) {
        int width = recon_text_width(np->font, MENUS[i].name) + 20;
        int at = menu_left(np, x, i);
        bool open = (np->menu_open == i);

        if (open) {
            recon_fill_rect(panel, at, y + 1, width, MENUBAR_HEIGHT - 2,
                COLOR_MENU_HILITE);
        }
        recon_draw_text(panel, np->font, at + 10,
            y + (MENUBAR_HEIGHT + ascent) / 2 - 2, width, MENUS[i].name,
            open ? COLOR_MENU_HILITE_TEXT : COLOR_TEXT);

        /* One id per menu, counting from the first, so a click resolves to a
         * menu without the bar and the handler each doing the arithmetic. */
        recon_hit_add(panel, at, y + 1, width, MENUBAR_HEIGHT - 2,
            HIT_MENUBAR_BASE + i);
    }
}

static void draw_file_menu(struct recon_notepad *np, struct recon_panel *panel,
        int x, int y) {
    if (np->menu_open < 0 || np->menu_open >= MENU_COUNT) {
        return;
    }

    const struct menu_item *items = MENUS[np->menu_open].items;
    int count = MENUS[np->menu_open].count;

    int ascent = recon_font_ascent(np->font);
    int height = count * MENU_ITEM_HEIGHT + 6;
    int mx = menu_left(np, x, np->menu_open);
    int my = y + MENUBAR_HEIGHT;

    recon_fill_rect(panel, mx, my, MENU_WIDTH, height, COLOR_MENU);
    recon_stroke_rect(panel, mx, my, MENU_WIDTH, height, COLOR_MENU_BORDER);

    for (int i = 0; i < count; i++) {
        int iy = my + 3 + i * MENU_ITEM_HEIGHT;
        bool hovered = (i == np->menu_hover);

        if (hovered) {
            recon_fill_rect(panel, mx + 2, iy, MENU_WIDTH - 4, MENU_ITEM_HEIGHT,
                COLOR_MENU_HILITE);
        }

        int baseline = iy + (MENU_ITEM_HEIGHT + ascent) / 2 - 2;
        recon_draw_text(panel, np->font, mx + 10, baseline, MENU_WIDTH - 20,
            items[i].label, hovered ? COLOR_MENU_HILITE_TEXT : COLOR_TEXT);

        /* The shortcut, right-aligned and dimmer: it is a reminder, not
         * another thing to read. */
        if (items[i].shortcut != NULL && items[i].shortcut[0] != '\0') {
            int sw = recon_text_width(np->font, items[i].shortcut);
            recon_draw_text(panel, np->font, mx + MENU_WIDTH - 10 - sw,
                baseline, sw, items[i].shortcut,
                hovered ? COLOR_MENU_HILITE_TEXT : COLOR_MENU_DIM);
        }

        if (items[i].separator_after) {
            recon_fill_rect(panel, mx + 4, iy + MENU_ITEM_HEIGHT - 1,
                MENU_WIDTH - 8, 1, RECON_RGB(0x90, 0x90, 0x90));
        }

        recon_hit_add(panel, mx + 2, iy, MENU_WIDTH - 4, MENU_ITEM_HEIGHT,
            HIT_MENU_BASE + i);
    }
}

static void notepad_draw(void *user, struct recon_panel *panel,
        int x, int y, int w, int h) {
    struct recon_notepad *np = user;
    int line_height = recon_font_line_height(np->font) + LINE_SPACING;
    int ascent = recon_font_ascent(np->font);
    if (line_height <= 0) {
        line_height = 16;
    }

    draw_menubar(np, panel, x, y, w);

    int content_y = y + MENUBAR_HEIGHT;
    int text_h = h - MENUBAR_HEIGHT - STATUS_HEIGHT;
    np->visible_lines = text_h > 0 ? text_h / line_height : 0;

    recon_fill_rect(panel, x, content_y, w, text_h, COLOR_BG);
    recon_draw_bevel(panel, x, content_y, w, text_h, true);
    recon_hit_add(panel, x, content_y, w, text_h, HIT_TEXT);

    /* Everything below draws relative to the text area, not the window. */
    y = content_y;

    /* Walk the buffer a line at a time, drawing the visible window of it. */
    size_t pos = 0;
    int line = 0;
    int cursor_line = line_number(np, np->cursor);

    while (pos <= np->length && line < np->scroll_line + np->visible_lines) {
        size_t end = line_end(np, pos);

        if (line >= np->scroll_line) {
            int row = line - np->scroll_line;
            int ly = y + PADDING + row * line_height;
            int baseline = ly + ascent;

            if (end > pos) {
                /* recon_draw_text needs a terminated string; borrow the byte
                 * after the line and put it back. */
                char saved = np->text[end];
                np->text[end] = '\0';
                recon_draw_text(panel, np->font, x + PADDING, baseline,
                    w - PADDING * 2, np->text + pos, COLOR_TEXT);
                np->text[end] = saved;
            }

            if (line == cursor_line) {
                /* Measure the text before the cursor to place the caret. */
                size_t start = line_start(np, np->cursor);
                char saved = np->text[np->cursor];
                np->text[np->cursor] = '\0';
                int caret_x = x + PADDING +
                    recon_text_width(np->font, np->text + start);
                np->text[np->cursor] = saved;

                recon_fill_rect(panel, caret_x, ly, 2, line_height - LINE_SPACING,
                    COLOR_CURSOR);
            }
        }

        if (end >= np->length) {
            break;
        }
        pos = end + 1;
        line++;
    }

    /* Status line. It sits below the text area, which now starts under the
     * menu bar, so it is measured from there rather than from the window. */
    int sy = y + text_h;
    recon_fill_rect(panel, x, sy, w, STATUS_HEIGHT, COLOR_STATUS_BG);

    char status[256];
    if (np->message[0] != '\0') {
        snprintf(status, sizeof(status), "%s", np->message);
    } else {
        snprintf(status, sizeof(status), "Line %d, Column %d   %zu characters%s",
            cursor_line + 1, column_number(np, np->cursor) + 1, np->length,
            np->modified ? "   (modified)" : "");
    }
    recon_draw_text(panel, np->font, x + PADDING,
        sy + (STATUS_HEIGHT + ascent) / 2 - 2, w - PADDING * 2,
        status, np->message[0] != '\0' && np->message_is_warning
            ? COLOR_WARNING : COLOR_STATUS_TEXT);

    /* The menu draws over the text, so it goes last. */
    if (np->menu_open >= 0) {
        draw_file_menu(np, panel, x, y - MENUBAR_HEIGHT);
    }

    /*
     * The dialog covers the whole content area, including the menu bar: while
     * it is up it is the only thing that can be used, and drawing it over
     * everything is what makes that obvious.
     */
    if (recon_filedlg_is_open(&np->dialog)) {
        recon_filedlg_draw(&np->dialog, panel, np->font,
            x, y - MENUBAR_HEIGHT, w, h);
    }
}

/* --- Input --- */

static bool notepad_click(void *user, uint32_t hit_id, int cx, int cy,
        bool pressed) {
    struct recon_notepad *np = user;
    (void)cx;
    (void)cy;

    if (!pressed) {
        return false;
    }

    /* The dialog takes every click while it is up, including the ones that
     * miss it: the window behind is not usable until it is answered. */
    if (recon_filedlg_is_open(&np->dialog)) {
        switch (recon_filedlg_click(&np->dialog, hit_id)) {
        case RECON_FILEDLG_ACCEPTED:
            finish_dialog(np);
            break;
        case RECON_FILEDLG_CANCELLED:
            np->pending = PENDING_NONE;
            break;
        default:
            break;
        }
        return true;
    }

    /*
     * A name on the bar. Checked before the entries below it, because the
     * bar's ids sit above the entries' and would otherwise be read as an
     * entry index far past the end of whichever menu is open.
     */
    if (hit_id >= HIT_MENUBAR_BASE &&
            hit_id < HIT_MENUBAR_BASE + (uint32_t)MENU_COUNT) {
        int which = (int)(hit_id - HIT_MENUBAR_BASE);
        /* Clicking the open menu's own name closes it, which is what makes
         * the name a toggle rather than only a way in. */
        np->menu_open = (np->menu_open == which) ? -1 : which;
        np->menu_hover = -1;
        return true;
    }

    if (hit_id >= HIT_MENU_BASE && np->menu_open >= 0) {
        int index = (int)(hit_id - HIT_MENU_BASE);
        if (index >= 0 && index < MENUS[np->menu_open].count) {
            run_command(np, MENUS[np->menu_open].items[index].command);
        }
        return true;
    }

    /* A click anywhere else closes the menu rather than choosing from it,
     * which is what clicking away from an open menu should do. */
    if (np->menu_open >= 0) {
        np->menu_open = -1;
        return true;
    }

    return true;
}

/*
 * Follow the pointer across the open menu. Redrawn only when the highlighted
 * entry changes, so moving within one entry costs nothing.
 */
static void notepad_motion(void *user, uint32_t hit_id, int cx, int cy) {
    struct recon_notepad *np = user;
    (void)cx;
    (void)cy;

    if (np->menu_open < 0) {
        np->menu_hover = -1;
        return;
    }

    int hover = -1;
    if (hit_id >= HIT_MENU_BASE) {
        int index = (int)(hit_id - HIT_MENU_BASE);
        if (index >= 0 && index < MENUS[np->menu_open].count) {
            hover = index;
        }
    }

    if (hover != np->menu_hover) {
        np->menu_hover = hover;
        recon_appwin_refresh(np->win);
    }
}

static bool notepad_key(void *user, xkb_keysym_t sym, uint32_t modifiers) {
    struct recon_notepad *np = user;

    if (recon_filedlg_is_open(&np->dialog)) {
        switch (recon_filedlg_key(&np->dialog, sym, modifiers)) {
        case RECON_FILEDLG_ACCEPTED:
            finish_dialog(np);
            break;
        case RECON_FILEDLG_CANCELLED:
            np->pending = PENDING_NONE;
            break;
        default:
            break;
        }
        return true;
    }

    bool ctrl = (modifiers & RECON_MOD_CTRL) != 0;

    if (ctrl) {
        switch (sym) {
        case XKB_KEY_n:
        case XKB_KEY_N:
            run_command(np, FILE_NEW);
            return true;
        case XKB_KEY_o:
        case XKB_KEY_O:
            run_command(np, FILE_OPEN);
            return true;
        case XKB_KEY_s:
        case XKB_KEY_S:
            /* Shift+Ctrl+S is Save As, which is why the shift bit is read
             * rather than ignored. */
            run_command(np, (modifiers & RECON_MOD_SHIFT)
                ? FILE_SAVE_AS : FILE_SAVE);
            return true;
        case XKB_KEY_z:
        case XKB_KEY_Z:
            /* Shift+Ctrl+Z redoes, which is the other spelling of Ctrl+Y and
             * the one people who came from a Mac reach for. */
            if (modifiers & RECON_MOD_SHIFT) {
                do_redo(np);
            } else {
                do_undo(np);
            }
            return true;
        case XKB_KEY_y:
        case XKB_KEY_Y:
            do_redo(np);
            return true;
        default:
            break;
        }
        /* Other control combinations are not text and must not be typed into
         * the buffer. */
        return true;
    }

    if (np->menu_open >= 0 && sym == XKB_KEY_Escape) {
        np->menu_open = -1;
        return true;
    }

    /* Typing clears whatever the last operation had to say; the status line
     * goes back to reporting where the cursor is. */
    np->message[0] = '\0';

    switch (sym) {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        insert_char(np, '\n');
        scroll_to_cursor(np);
        return true;

    case XKB_KEY_BackSpace:
        delete_before_cursor(np);
        scroll_to_cursor(np);
        return true;

    case XKB_KEY_Delete:
    case XKB_KEY_KP_Delete:
        delete_at_cursor(np);
        return true;

    case XKB_KEY_Tab:
        for (int i = 0; i < TAB_WIDTH; i++) {
            insert_char(np, ' ');
        }
        return true;

    case XKB_KEY_Left:
        if (np->cursor > 0) {
            np->cursor--;
        }
        scroll_to_cursor(np);
        return true;

    case XKB_KEY_Right:
        if (np->cursor < np->length) {
            np->cursor++;
        }
        scroll_to_cursor(np);
        return true;

    case XKB_KEY_Up:
        move_up(np);
        scroll_to_cursor(np);
        return true;

    case XKB_KEY_Down:
        move_down(np);
        scroll_to_cursor(np);
        return true;

    case XKB_KEY_Home:
        np->cursor = line_start(np, np->cursor);
        return true;

    case XKB_KEY_End:
        np->cursor = line_end(np, np->cursor);
        return true;

    default:
        break;
    }

    /* Anything that maps to a printable character gets inserted. Going
     * through the keymap rather than the raw keysym is what makes shifted
     * characters and other layouts work. */
    uint32_t codepoint = xkb_keysym_to_utf32(sym);
    if (codepoint >= 32 && codepoint < 127) {
        insert_char(np, (char)codepoint);
        scroll_to_cursor(np);
        update_title(np);
        return true;
    }

    return false;
}

static void notepad_scroll(void *user, double delta) {
    struct recon_notepad *np = user;

    if (recon_filedlg_is_open(&np->dialog)) {
        recon_filedlg_scroll(&np->dialog, delta);
        return;
    }

    np->scroll_line += (delta > 0) ? 3 : -3;
    if (np->scroll_line < 0) {
        np->scroll_line = 0;
    }
}

/*
 * Right-clicking offers the File menu's contents, so the commands are reachable
 * without going up to the bar. Save is offered as unavailable rather than
 * hidden when there is nothing to save, which says why it does nothing.
 */
static bool notepad_context(void *user, uint32_t hit_id, int cx, int cy,
        struct recon_menu_spec *menu) {
    struct recon_notepad *np = user;
    (void)hit_id;
    (void)cx;
    (void)cy;

    if (recon_filedlg_is_open(&np->dialog)) {
        return false;
    }

    np->menu_open = false;

    recon_menu_add(menu, "New", FILE_NEW, true, false);
    recon_menu_add(menu, "Open...", FILE_OPEN, true, true);
    recon_menu_add(menu, "Save", FILE_SAVE, np->modified || np->path[0] == '\0',
        false);
    recon_menu_add(menu, "Save As...", FILE_SAVE_AS, true, false);
    return true;
}

static void notepad_context_action(void *user, uint32_t id) {
    run_command(user, (enum file_command)id);
}

static void notepad_destroy(void *user) {
    struct recon_notepad *np = user;
    forget_history(np);
    free(np->text);
    free(np);
}

static const struct recon_appwin_impl NOTEPAD_IMPL = {
    .title = "Notepad",
    .icon = RECON_ICON_NOTEPAD,
    .default_width = 520,
    .default_height = 400,
    .min_width = 260,
    .min_height = 180,
    .draw = notepad_draw,
    .click = notepad_click,
    .key = notepad_key,
    .motion = notepad_motion,
    .scroll = notepad_scroll,
    .context = notepad_context,
    .context_action = notepad_context_action,
    .destroy = notepad_destroy,
};

bool recon_notepad_open_path(struct recon_appwin *win, const char *path) {
    if (win == NULL || path == NULL) {
        return false;
    }

    struct recon_notepad *np = recon_appwin_user(win);
    if (np == NULL) {
        return false;
    }

    /*
     * Work in progress wins.
     *
     * Opening from the File menu asks what to do about unsaved text, because
     * the person asking is looking at it. This is somebody double-clicking a
     * file somewhere else on the screen, quite possibly having forgotten this
     * window is open at all -- and silently replacing what they were writing
     * is the one outcome that cannot be undone.
     */
    if (np->modified) {
        return false;
    }

    return load_from(np, path);
}

struct recon_appwin *recon_notepad_create(struct recon_server *server,
        struct recon_font *font) {
    struct recon_notepad *np = calloc(1, sizeof(*np));
    if (np == NULL) {
        return NULL;
    }
    np->font = font;
    /*
     * No menu is down. Zero would mean the File menu, so a new window came up
     * with its own File menu open and the first click on the bar closed it
     * rather than opening anything -- which is precisely the bug the file
     * explorer had, for precisely this reason.
     */
    np->menu_open = -1;
    np->menu_hover = -1;

    if (!ensure_capacity(np, 0)) {
        free(np);
        return NULL;
    }
    np->text[0] = '\0';

    np->win = recon_appwin_create(server, font, &NOTEPAD_IMPL, np);
    if (np->win == NULL) {
        free(np->text);
        free(np);
        return NULL;
    }
    return np->win;
}
