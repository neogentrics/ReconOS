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
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ReconOS.h"
#include "recon_appwin.h"
#include "recon_access.h"
#include "recon_fonts.h"
#include "recon_registry.h"
#include "recon_filedlg.h"
#include "recon_clip.h"
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
/* What marks selected text. The field selection rather than the list
 * one: this is text being edited, not a row being pointed at. */
#define COLOR_SELECTION THEME(FIELD_SELECTION)
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
/* The find bar's field. Its own id, in the gap before the menu entries. */
#define HIT_FIND (RECON_APPWIN_HIT_USER + 3)
#define HIT_REPLACEMENT (RECON_APPWIN_HIT_USER + 4)
#define HIT_REPLACE_ONE (RECON_APPWIN_HIT_USER + 5)
#define HIT_REPLACE_ALL (RECON_APPWIN_HIT_USER + 6)
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
    EDIT_CUT,
    EDIT_COPY,
    EDIT_PASTE,
    EDIT_SELECT_ALL,
    EDIT_FIND,
    EDIT_FIND_NEXT,
    EDIT_REPLACE,

    VIEW_WRAP,
    VIEW_BIGGER,
    VIEW_SMALLER,
    VIEW_NEXT_FONT,
    VIEW_SYSTEM_FONT,
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
    { "Undo",       "Ctrl+Z", EDIT_UNDO,   false },
    { "Redo",       "Ctrl+Y", EDIT_REDO,   true  },
    { "Cut",        "Ctrl+X", EDIT_CUT,    false },
    { "Copy",       "Ctrl+C", EDIT_COPY,   false },
    { "Paste",      "Ctrl+V", EDIT_PASTE,  true  },
    { "Select All", "Ctrl+A", EDIT_SELECT_ALL, true  },
    { "Find...",    "Ctrl+F", EDIT_FIND,       false },
    { "Find Next",  "F3",     EDIT_FIND_NEXT,  false },
    { "Replace...", "Ctrl+H", EDIT_REPLACE,    false },
};

/*
 * The menu bar, in the order it is drawn.
 *
 * A table rather than two of everything, because the second menu is where a
 * menu bar written for one starts drawing the first one's items under the
 * second one's name.
 */
/*
 * View: how the text is shown rather than what it says.
 *
 * Word wrap first because it is the one people look for, and because it is
 * the only entry here that changes what the document appears to be rather
 * than only how big it is.
 */
static const struct menu_item VIEW_MENU[] = {
    { "Word Wrap",      "",  VIEW_WRAP,        true  },
    { "Larger Text",    "",  VIEW_BIGGER,      false },
    { "Smaller Text",   "",  VIEW_SMALLER,     true  },
    { "Next Font",      "",  VIEW_NEXT_FONT,   false },
    { "The System's Font", "", VIEW_SYSTEM_FONT, false },
};

static const struct {
    const char *name;
    const struct menu_item *items;
    int count;
} MENUS[] = {
    { "File", FILE_MENU, (int)(sizeof(FILE_MENU) / sizeof(FILE_MENU[0])) },
    { "Edit", EDIT_MENU, (int)(sizeof(EDIT_MENU) / sizeof(EDIT_MENU[0])) },
    { "View", VIEW_MENU, (int)(sizeof(VIEW_MENU) / sizeof(VIEW_MENU[0])) },
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

    /*
     * The other end of a selection, or NO_ANCHOR. Zero is a real place in the
     * document, so "nothing selected" cannot be zero.
     */
    size_t anchor;

    int scroll_line;
    int visible_lines;
    bool modified;

    /*
     * Wrapping, and how wide a row may be before it wraps.
     *
     * The width is set while drawing, because only the draw knows how wide
     * the text area is. Everything that asks about rows reads it, so a
     * question asked before the first draw gets a width of zero and is
     * answered as though wrapping were off -- which is the right answer,
     * because nothing has been laid out yet.
     */
    bool wrap;
    int wrap_width;

    /*
     * The typeface this window draws with, when it is not the system's.
     *
     * Owned here and freed with the window. `size` is applied either way:
     * recon_font_system caches by size, so changing it costs nothing the
     * second time a size is used.
     */
    struct recon_font *own_font;
    char font_name[96];
    int size;

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

    /* The find bar, and what is in it. */
    bool finding;
    struct recon_edit find;

    /*
     * And what to put in its place, when the bar is open for replacing.
     *
     * One bar rather than two, because Find and Replace are the same act with
     * one more field: everything about locating the text is identical, and a
     * separate bar would be the same code twice with a second place for the
     * two to disagree about what "next" means.
     */
    bool replacing;
    struct recon_edit replacement;
    /* Which of the two fields the keyboard is in. */
    bool replacement_focused;

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
    recon_text_copy(shown, sizeof(shown), leaf);

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

/* --- Selection --- */

/*
 * A selection is an anchor and the cursor, with the text between them.
 *
 * Which of the two comes first is not fixed: dragging backwards from a word
 * puts the anchor after the cursor, and everything below sorts them rather
 * than assuming. Anchoring at SIZE_MAX means nothing is selected, because
 * zero is a real place in the document and "no selection" is not.
 */
#define NO_ANCHOR ((size_t)-1)

static bool has_selection(const struct recon_notepad *np) {
    return np->anchor != NO_ANCHOR && np->anchor != np->cursor;
}

static void selection_range(const struct recon_notepad *np,
        size_t *from, size_t *to) {
    if (!has_selection(np)) {
        *from = np->cursor;
        *to = np->cursor;
        return;
    }
    *from = np->anchor < np->cursor ? np->anchor : np->cursor;
    *to = np->anchor < np->cursor ? np->cursor : np->anchor;
}

static void clear_selection(struct recon_notepad *np) {
    np->anchor = NO_ANCHOR;
}

/*
 * Start or continue a selection, depending on whether Shift is held.
 *
 * Called before every cursor movement. Moving without Shift drops whatever
 * was selected, which is what makes an arrow key mean "go here" rather than
 * "extend to here".
 */
static void before_move(struct recon_notepad *np, bool extending) {
    if (!extending) {
        clear_selection(np);
        return;
    }
    if (np->anchor == NO_ANCHOR) {
        np->anchor = np->cursor;
    }
}

/* Record a whole run as one edit, rather than one per character. */
static void record_run(struct recon_notepad *np, size_t at, const char *text,
        size_t length, bool insert) {
    if (np->replaying || length == 0) {
        return;
    }

    struct edit *edit = push_edit(np);
    edit->text = malloc(length);
    if (edit->text == NULL) {
        np->undo_count--;
        np->undo_at = np->undo_count;
        return;
    }
    memcpy(edit->text, text, length);
    edit->length = length;
    edit->at = at;
    edit->insert = insert;
    edit->cursor_before = np->cursor;
}

/*
 * Take out everything between two points, as one undoable act.
 *
 * Not a loop over delete_before_cursor: that would record a character at a
 * time and group them by word, so undoing a deleted paragraph would give it
 * back a word at a time -- which is not what deleting it was.
 */
static void delete_range(struct recon_notepad *np, size_t from, size_t to) {
    if (to <= from || to > np->length) {
        return;
    }

    record_run(np, from, np->text + from, to - from, false);

    memmove(np->text + from, np->text + to, np->length - to + 1);
    np->length -= (to - from);
    np->cursor = from;
    np->modified = true;
    clear_selection(np);
}

/* Whatever is selected, gone. True when there was something. */
static bool delete_selection(struct recon_notepad *np) {
    if (!has_selection(np)) {
        return false;
    }
    size_t from, to;
    selection_range(np, &from, &to);
    delete_range(np, from, to);
    return true;
}

static void insert_run(struct recon_notepad *np, const char *text,
        size_t length) {
    if (length == 0 || !ensure_capacity(np, np->length + length)) {
        return;
    }

    record_run(np, np->cursor, text, length, true);

    memmove(np->text + np->cursor + length, np->text + np->cursor,
        np->length - np->cursor + 1);
    memcpy(np->text + np->cursor, text, length);
    np->length += length;
    np->cursor += length;
    np->modified = true;
}

/* --- Find --- */

/*
 * A bar above the status line, with the search in it.
 *
 * A bar rather than a dialog. A find dialog covers the thing being searched,
 * so people drag it out of the way and then cannot see the field they are
 * typing into -- and this window is small enough that a dialog would cover
 * most of it.
 */
#define FIND_HEIGHT 24

/*
 * Case-insensitive, always.
 *
 * A case-sensitive search is a real thing to want, and it is a second control
 * on a bar that does not have room for one yet. Insensitive is the better
 * default of the two: somebody looking for "error" in a log wants ERROR as
 * well, and somebody who did not want it can see at a glance that the match
 * is the wrong case and press again.
 */
static bool matches_at(const struct recon_notepad *np, size_t at,
        const char *needle, size_t needle_len) {
    if (at + needle_len > np->length) {
        return false;
    }
    for (size_t i = 0; i < needle_len; i++) {
        if (tolower((unsigned char)np->text[at + i]) !=
                tolower((unsigned char)needle[i])) {
            return false;
        }
    }
    return true;
}

/*
 * The next match after `from`, wrapping once.
 *
 * Wrapping matters more than it sounds: without it, a search that starts
 * halfway down a file reports nothing for a word that is only above the
 * cursor, which reads as "that word is not here" and is wrong.
 */
static bool find_from(struct recon_notepad *np, size_t from, size_t *found) {
    const char *needle = np->find.text;
    size_t needle_len = strlen(needle);

    if (needle_len == 0 || needle_len > np->length) {
        return false;
    }

    size_t limit = np->length - needle_len;
    for (size_t at = from; at <= limit; at++) {
        if (matches_at(np, at, needle, needle_len)) {
            *found = at;
            return true;
        }
    }
    for (size_t at = 0; at < from && at <= limit; at++) {
        if (matches_at(np, at, needle, needle_len)) {
            *found = at;
            return true;
        }
    }
    return false;
}

static void do_find_next(struct recon_notepad *np) {
    if (np->find.text[0] == '\0') {
        set_message(np, false, "Type something to look for.");
        return;
    }

    /*
     * Starting one past the selection, not at the cursor.
     *
     * A match is left selected, so searching again from the cursor would find
     * the same one for ever. One past its start is what makes the second
     * press mean "the next one".
     */
    size_t from = np->cursor;
    if (has_selection(np)) {
        size_t a, b;
        selection_range(np, &a, &b);
        from = a + 1;
    }
    if (from > np->length) {
        from = 0;
    }

    size_t at;
    if (!find_from(np, from, &at)) {
        set_message(np, true, "'%s' is not in this document.", np->find.text);
        return;
    }

    np->anchor = at;
    np->cursor = at + strlen(np->find.text);
    scroll_to_cursor(np);
    np->message[0] = '\0';
}

/*
 * Replace what is selected, if it is what was searched for, then find the
 * next one.
 *
 * The order matters. Replacing first and searching second is what makes
 * pressing the button repeatedly walk the document: each press deals with the
 * match in front of you and puts the next one there. Searching first would
 * leave the last match unreplaced and require one more press than there are
 * matches, which nobody would count correctly.
 */
static void do_replace_one(struct recon_notepad *np) {
    const char *needle = np->find.text;
    if (needle[0] == '\0') {
        set_message(np, false, "Type something to look for.");
        return;
    }

    size_t needle_len = strlen(needle);
    bool replaced = false;

    if (has_selection(np)) {
        size_t a, b;
        selection_range(np, &a, &b);
        if (b - a == needle_len && matches_at(np, a, needle, needle_len)) {
            delete_range(np, a, b);
            insert_run(np, np->replacement.text,
                strlen(np->replacement.text));
            replaced = true;
        }
    }

    do_find_next(np);

    if (replaced) {
        set_message(np, false, "Replaced one.");
    }
}

/*
 * Every match, from the top.
 *
 * From the top rather than from the cursor, because "all" means all and a
 * count that depends on where the cursor happened to be is not a count of
 * anything. One pass forward, stepping over what was just written: replacing
 * "a" with "aa" otherwise finds its own output and never stops.
 */
static void do_replace_all(struct recon_notepad *np) {
    const char *needle = np->find.text;
    size_t needle_len = strlen(needle);

    if (needle_len == 0) {
        set_message(np, false, "Type something to look for.");
        return;
    }

    size_t replacement_len = strlen(np->replacement.text);
    int count = 0;

    size_t at = 0;
    while (at + needle_len <= np->length) {
        if (!matches_at(np, at, needle, needle_len)) {
            at++;
            continue;
        }

        np->anchor = NO_ANCHOR;
        np->cursor = at;
        delete_range(np, at, at + needle_len);
        np->cursor = at;
        insert_run(np, np->replacement.text, replacement_len);

        at += replacement_len;
        count++;
    }

    np->anchor = NO_ANCHOR;
    scroll_to_cursor(np);

    if (count == 0) {
        set_message(np, true, "'%s' is not in this document.", needle);
    } else {
        set_message(np, false, "Replaced %d.", count);
    }
}

static void open_find(struct recon_notepad *np) {
    np->finding = true;

    /*
     * Whatever is selected becomes what to look for, if it is one line of it.
     * Selecting a word and pressing Ctrl+F to search for that word is the
     * gesture people already make.
     */
    char initial[128];
    initial[0] = '\0';

    if (has_selection(np)) {
        size_t a, b;
        selection_range(np, &a, &b);
        size_t length = b - a;
        if (length > 0 && length < sizeof(initial) &&
                memchr(np->text + a, '\n', length) == NULL) {
            memcpy(initial, np->text + a, length);
            initial[length] = '\0';
        }
    }

    recon_edit_begin(&np->find, initial, false);
    recon_edit_begin(&np->replacement, "", false);
    np->replacement_focused = false;
    np->message[0] = '\0';
}

/* The same bar with the second field showing. */
static void open_replace(struct recon_notepad *np) {
    bool was_open = np->finding;

    open_find(np);
    np->replacing = true;

    /*
     * The keyboard starts in the field that is new. Somebody who pressed
     * Ctrl+H with the bar already open and something to find in it wants to
     * type the replacement, not retype the search.
     */
    np->replacement_focused = was_open && np->find.text[0] != '\0';
}

static void close_find(struct recon_notepad *np) {
    np->finding = false;
    np->replacing = false;
    np->replacement_focused = false;
    recon_edit_end(&np->find);
    recon_edit_end(&np->replacement);
    np->message[0] = '\0';
}

/* --- Cut, copy, paste --- */

static void do_copy(struct recon_notepad *np, bool cut) {
    if (!has_selection(np)) {
        set_message(np, false, "Nothing is selected.");
        return;
    }

    size_t from, to;
    selection_range(np, &from, &to);
    if (!recon_clip_set_text(np->text + from, to - from)) {
        set_message(np, true, "There was not room to copy that.");
        return;
    }

    if (cut) {
        delete_range(np, from, to);
        scroll_to_cursor(np);
        set_message(np, false, "Cut %zu characters.", to - from);
    } else {
        set_message(np, false, "Copied %zu characters.", to - from);
    }
}

static void do_paste(struct recon_notepad *np) {
    if (recon_clip_empty()) {
        set_message(np, false, "The clipboard is empty.");
        return;
    }

    /* Pasting over a selection replaces it, which is what the selection being
     * there means. */
    delete_selection(np);
    insert_run(np, recon_clip_text(), recon_clip_length());
    scroll_to_cursor(np);
    np->message[0] = '\0';
}

static void do_select_all(struct recon_notepad *np) {
    if (np->length == 0) {
        return;
    }
    np->anchor = 0;
    np->cursor = np->length;
    scroll_to_cursor(np);
    np->message[0] = '\0';
}

static void insert_char(struct recon_notepad *np, char c) {
    /* Typing over a selection replaces it, which is what having selected it
     * meant. Done first so the character lands where the selection was. */
    delete_selection(np);

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
    if (delete_selection(np)) {
        return;    /* Backspace with something selected removes that. */
    }
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
    if (delete_selection(np)) {
        return;
    }
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

/* How wide the text from `from` to `to` draws. */
static int width_between(struct recon_notepad *np, size_t from, size_t to) {
    if (to <= from) {
        return 0;
    }
    /* recon_text_width needs a terminated string; borrow the byte after the
     * run and put it back, the way the drawing code does. */
    char saved = np->text[to];
    np->text[to] = '\0';
    int width = recon_text_width(np->font, np->text + from);
    np->text[to] = saved;
    return width;
}

/*
 * Where the display row starting at `start` ends.
 *
 * With wrapping off this is the end of the line, and everything above behaves
 * as it always did. With wrapping on it is the last point that fits, moved
 * back to a space if there is one -- breaking mid-word is what a terminal
 * does, and a text editor that did it would look broken rather than terse.
 *
 * A single word longer than the whole width has nowhere to break, so it
 * breaks where it runs out. That is the one case where mid-word is correct:
 * the alternative is a row that overflows the window.
 */
static size_t row_end(struct recon_notepad *np, size_t start) {
    size_t hard = line_end(np, start);

    if (!np->wrap || np->wrap_width <= 0) {
        return hard;
    }
    if (width_between(np, start, hard) <= np->wrap_width) {
        return hard;
    }

    /* The longest run that fits. Walked forward a byte at a time: the text is
     * UTF-8 and a continuation byte measures as nothing on its own, so
     * stepping by bytes lands on the same answer as stepping by characters
     * and needs no decoder here. */
    size_t fits = start;
    for (size_t at = start + 1; at <= hard; at++) {
        if (width_between(np, start, at) > np->wrap_width) {
            break;
        }
        fits = at;
    }

    if (fits <= start) {
        /* Not even one character fits, which means the window is narrower
         * than a letter. Take one anyway; a row that never advances is a
         * draw loop that never ends. */
        return start + 1 <= hard ? start + 1 : hard;
    }

    /* Back up to the last space, so words stay whole. */
    for (size_t at = fits; at > start; at--) {
        if (np->text[at - 1] == ' ' || np->text[at - 1] == '\t') {
            return at;
        }
    }

    return fits;   /* One long word; break where it runs out. */
}

/* The start of the display row containing `pos`. */
static size_t row_start(struct recon_notepad *np, size_t pos) {
    size_t at = line_start(np, pos);
    if (!np->wrap || np->wrap_width <= 0) {
        return at;
    }

    while (at < pos) {
        size_t end = row_end(np, at);
        if (end >= pos || end <= at) {
            return at;
        }
        at = end;
    }
    return at;
}

/* How many display rows come before `pos`. */
static int row_number(struct recon_notepad *np, size_t pos) {
    int rows = 0;
    size_t at = 0;
    while (at < pos) {
        size_t end = row_end(np, at);
        if (end >= pos) {
            break;
        }
        /* Past the newline, or past the wrap point. */
        at = (end < np->length && np->text[end] == '\n') ? end + 1 : end;
        if (at == 0) {
            break;
        }
        rows++;
    }
    return rows;
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

/*
 * Up and down move by what is on the screen, not by what is in the file.
 *
 * With wrapping on, a paragraph is one line and several rows, and an arrow
 * that jumped the whole paragraph would be an arrow that skipped most of the
 * document. What somebody means by "up" is the row above the one they can
 * see.
 */
static void move_up(struct recon_notepad *np) {
    size_t start = row_start(np, np->cursor);
    if (start == 0) {
        return;
    }
    size_t column = np->cursor - start;

    /* The row before this one: found by walking from the start of the row
     * above the previous character, which is inside it by construction. */
    size_t previous = row_start(np, start - 1);
    size_t previous_end = row_end(np, previous);

    size_t target = previous + column;
    np->cursor = target < previous_end ? target : previous_end;
}

static void move_down(struct recon_notepad *np) {
    size_t start = row_start(np, np->cursor);
    size_t end = row_end(np, start);
    if (end >= np->length) {
        return;
    }
    size_t column = np->cursor - start;

    size_t next = (np->text[end] == '\n') ? end + 1 : end;
    size_t next_end = row_end(np, next);

    size_t target = next + column;
    np->cursor = target < next_end ? target : next_end;
}

/* Keep the cursor's line on screen after it moves. */
static void scroll_to_cursor(struct recon_notepad *np) {
    int line = row_number(np, np->cursor);

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

static bool apply_font(struct recon_notepad *np, char *why, size_t why_size);
static void next_font(struct recon_notepad *np, char *why, size_t why_size);

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

    case EDIT_CUT:
        do_copy(np, true);
        break;

    case EDIT_COPY:
        do_copy(np, false);
        break;

    case EDIT_PASTE:
        do_paste(np);
        break;

    case EDIT_SELECT_ALL:
        do_select_all(np);
        break;

    case EDIT_FIND:
        open_find(np);
        break;

    case EDIT_FIND_NEXT:
        do_find_next(np);
        break;

    case EDIT_REPLACE:
        open_replace(np);
        break;

    case VIEW_WRAP:
        np->wrap = !np->wrap;
        /*
         * The cursor's row number changes when wrapping does, so where the
         * view is scrolled to has to be worked out again. Without this,
         * turning wrap on in a long document leaves the window looking at a
         * part of it the cursor is no longer in.
         */
        scroll_to_cursor(np);
        set_message(np, false, np->wrap
            ? "Word wrap is on. Long lines fold at the window's edge."
            : "Word wrap is off. Long lines run past the edge.");
        break;

    case VIEW_BIGGER:
    case VIEW_SMALLER: {
        int was = np->size;
        np->size += (command == VIEW_BIGGER) ? 1 : -1;

        char why[160];
        if (!apply_font(np, why, sizeof(why))) {
            np->size = was;
            apply_font(np, NULL, 0);
            set_message(np, true, "%s", why);
            break;
        }

        if (np->size == was) {
            set_message(np, false, command == VIEW_BIGGER
                ? "That is as large as it goes."
                : "That is as small as it goes.");
            break;
        }

        /* A different size is a different number of rows, and a different
         * place for every wrap point. */
        scroll_to_cursor(np);
        set_message(np, false, "Text size %d.", np->size);
        break;
    }

    case VIEW_NEXT_FONT: {
        char why[160];
        why[0] = '\0';
        next_font(np, why, sizeof(why));

        if (why[0] != '\0') {
            set_message(np, true, "%s", why);
            break;
        }
        scroll_to_cursor(np);
        set_message(np, false, "Drawing with %s.",
            np->font_name[0] != '\0' ? np->font_name : "the system's font");
        break;
    }

    case VIEW_SYSTEM_FONT: {
        np->font_name[0] = '\0';
        char why[160];
        if (!apply_font(np, why, sizeof(why))) {
            set_message(np, true, "%s", why);
            break;
        }
        scroll_to_cursor(np);
        set_message(np, false, "Back to the system's font.");
        break;
    }

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

/* --- How it is shown --- */

#define NOTEPAD_SIZE_MIN 9
#define NOTEPAD_SIZE_MAX 32

/*
 * Point the window at the typeface it should be drawing with.
 *
 * One place, called after any change, so the two ways of choosing -- a size
 * and a family -- cannot get out of step. A family that will not load leaves
 * the previous one in place and says so, rather than leaving the window with
 * nothing to draw text with.
 */
static bool apply_font(struct recon_notepad *np, char *why, size_t why_size) {
    if (why != NULL && why_size > 0) {
        why[0] = '\0';
    }

    if (np->size < NOTEPAD_SIZE_MIN) {
        np->size = NOTEPAD_SIZE_MIN;
    }
    if (np->size > NOTEPAD_SIZE_MAX) {
        np->size = NOTEPAD_SIZE_MAX;
    }

    if (np->font_name[0] == '\0') {
        /* The system's, at the chosen size. recon_font_system caches by size,
         * so this is free the second time a size is used and the result is
         * not ours to free. */
        struct recon_font *shared = recon_font_system(np->size);
        if (shared == NULL) {
            recon_text_copy(why, why_size, "The system font could not be "
                "loaded at that size.");
            return false;
        }
        if (np->own_font != NULL) {
            recon_font_destroy(np->own_font);
            np->own_font = NULL;
        }
        np->font = shared;
        return true;
    }

    char path[RECON_PATH_MAX];
    char host[RECON_PATH_MAX];
    char canonical[RECON_PATH_MAX];
    if (!recon_fonts_path(np->font_name, path, sizeof(path)) ||
            !recon_fs_resolve("/", path, host, sizeof(host), canonical,
                sizeof(canonical))) {
        recon_text_copy(why, why_size, "That font could not be found.");
        return false;
    }

    struct recon_font *loaded = recon_font_load(host, np->size);
    if (loaded == NULL) {
        recon_text_copy(why, why_size, "That font could not be read.");
        return false;
    }

    if (np->own_font != NULL) {
        recon_font_destroy(np->own_font);
    }
    np->own_font = loaded;
    np->font = loaded;
    return true;
}

/*
 * The next installed font after the current one, wrapping round to the
 * system's own.
 *
 * A menu that cycles rather than a list to pick from: Notepad has a menu bar
 * and no room for a chooser, the Control Panel already has the list, and
 * cycling is two clicks to see what a document looks like in something else.
 */
static void next_font(struct recon_notepad *np, char *why, size_t why_size) {
    int count = recon_fonts_count();
    if (count == 0) {
        recon_text_copy(why, why_size, "No fonts are installed. Display "
            "Settings can add one.");
        return;
    }

    int at = -1;   /* -1 means the system's own, which comes before the rest. */
    for (int i = 0; i < count; i++) {
        char name[96];
        if (recon_fonts_at(i, name, sizeof(name)) &&
                strcmp(name, np->font_name) == 0) {
            at = i;
            break;
        }
    }

    if (at + 1 >= count) {
        np->font_name[0] = '\0';   /* Round the loop, back to the system's. */
    } else {
        char name[96];
        if (recon_fonts_at(at + 1, name, sizeof(name))) {
            recon_text_copy(np->font_name, sizeof(np->font_name), name);
        }
    }

    apply_font(np, why, why_size);
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
    /* The find bar takes its height from the text, not from the status line:
     * the status line is where the result is reported, so covering it would
     * hide the answer to the search. */
    int find_h = np->finding ? FIND_HEIGHT : 0;
    int text_h = h - MENUBAR_HEIGHT - STATUS_HEIGHT - find_h;
    np->visible_lines = text_h > 0 ? text_h / line_height : 0;

    /*
     * Set before anything asks a question about rows, because every one of
     * those questions is answered against this number. The text area's
     * padding comes off both sides, and one character's slack keeps the last
     * letter of a full row clear of the bevel.
     */
    np->wrap_width = w - PADDING * 2 - 4;

    recon_fill_rect(panel, x, content_y, w, text_h, COLOR_BG);
    recon_draw_bevel(panel, x, content_y, w, text_h, true);
    recon_hit_add(panel, x, content_y, w, text_h, HIT_TEXT);

    /* Everything below draws relative to the text area, not the window. */
    y = content_y;

    /*
     * Walk the buffer a display row at a time.
     *
     * A row is a line when wrapping is off, and part of one when it is on.
     * Everything below is written against rows, so the two cases are one path
     * rather than two that have to be kept in step.
     */
    size_t pos = 0;
    int line = 0;
    int cursor_line = row_number(np, np->cursor);

    size_t sel_from, sel_to;
    selection_range(np, &sel_from, &sel_to);

    while (pos <= np->length && line < np->scroll_line + np->visible_lines) {
        size_t end = row_end(np, pos);

        if (line >= np->scroll_line) {
            int row = line - np->scroll_line;
            int ly = y + PADDING + row * line_height;
            int baseline = ly + ascent;

            /*
             * The selection, under the text rather than over it.
             *
             * Drawn per line and clipped to that line, because a selection
             * spanning three lines is three rectangles and not one -- and the
             * middle one has to run to the edge of the text, not to the edge
             * of the window, or an empty line in the middle of a selection
             * looks like the selection stopped there.
             */
            if (sel_to > sel_from) {
                size_t line_from = pos > sel_from ? pos : sel_from;
                size_t line_to = end < sel_to ? end : sel_to;

                if (line_to >= line_from &&
                        line_from <= end && line_to >= pos) {
                    char saved_a = np->text[line_from];
                    np->text[line_from] = '\0';
                    int from_x = recon_text_width(np->font, np->text + pos);
                    np->text[line_from] = saved_a;

                    char saved_b = np->text[line_to];
                    np->text[line_to] = '\0';
                    int to_x = recon_text_width(np->font, np->text + pos);
                    np->text[line_to] = saved_b;

                    /*
                     * A line whose break is inside the selection shows a
                     * little past its last character, so the newline itself
                     * reads as part of what was taken.
                     */
                    if (sel_to > end) {
                        to_x += 4;
                    }

                    if (to_x > from_x) {
                        recon_fill_rect(panel, x + PADDING + from_x, ly,
                            to_x - from_x, line_height - LINE_SPACING,
                            COLOR_SELECTION);
                    }
                }
            }

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
                /* Measured from the start of the row the cursor is on. On a
                 * wrapped line that is not the start of the line. */
                size_t start = row_start(np, np->cursor);
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

        /*
         * Past the newline, or straight on from the wrap point.
         *
         * A wrapped row ends *at* a character rather than at a separator, so
         * skipping one here would eat a letter from the start of every
         * continuation row.
         */
        pos = (np->text[end] == '\n') ? end + 1 : end;
        line++;
    }

    /* Status line. It sits below the text area, which now starts under the
     * menu bar, so it is measured from there rather than from the window. */
    int sy = y + text_h;
    if (np->finding) {
        int fy = sy - FIND_HEIGHT;
        recon_fill_rect(panel, x, fy, w, FIND_HEIGHT, COLOR_MENUBAR);
        recon_fill_rect(panel, x, fy, w, 1, RECON_RGB(0x90, 0x90, 0x90));

        const char *label = "Find:";
        int label_w = recon_text_width(np->font, label);
        recon_draw_text(panel, np->font, x + PADDING,
            fy + (FIND_HEIGHT + ascent) / 2 - 2, label_w, label, COLOR_TEXT);

        int field_x = x + PADDING + label_w + 8;

        /*
         * Narrower when there is a second field and two buttons to fit
         * beside it. A find bar that pushed its own buttons off the end of
         * the window would be a bar with nothing to press.
         */
        int field_w = np->replacing ? 130 : 260;
        int room = w - (field_x - x) - PADDING;
        if (field_w > room) {
            field_w = room;
        }

        recon_edit_draw(panel, np->font, field_x, fy + 2, field_w,
            FIND_HEIGHT - 4, &np->find);
        recon_hit_add(panel, field_x, fy + 2, field_w, FIND_HEIGHT - 4,
            HIT_FIND);

        if (np->replacing) {
            const char *with = "With:";
            int with_w = recon_text_width(np->font, with);
            int wx = field_x + field_w + 10;

            recon_draw_text(panel, np->font, wx,
                fy + (FIND_HEIGHT + ascent) / 2 - 2, with_w, with, COLOR_TEXT);

            int rx = wx + with_w + 8;
            recon_edit_draw(panel, np->font, rx, fy + 2, field_w,
                FIND_HEIGHT - 4, &np->replacement);
            recon_hit_add(panel, rx, fy + 2, field_w, FIND_HEIGHT - 4,
                HIT_REPLACEMENT);

            /*
             * Which field the keyboard is in, said with an outline. Two text
             * boxes and no way to tell which one is typing into is a bar that
             * eats what you type.
             */
            int focused_x = np->replacement_focused ? rx : field_x;
            recon_stroke_rect(panel, focused_x - 1, fy + 1, field_w + 2,
                FIND_HEIGHT - 2, THEME(ACCENT));

            int bx = rx + field_w + 10;
            const struct { const char *label; uint32_t id; } BUTTONS[] = {
                { "Replace", HIT_REPLACE_ONE },
                { "All", HIT_REPLACE_ALL },
            };

            for (size_t i = 0; i < sizeof(BUTTONS) / sizeof(BUTTONS[0]); i++) {
                int bw = recon_text_width(np->font, BUTTONS[i].label) + 16;
                if (bx + bw > x + w - PADDING) {
                    break;
                }

                recon_fill_role(panel, bx, fy + 2, bw, FIND_HEIGHT - 4,
                    RECON_THEME_BUTTON);
                recon_draw_button_edge(panel, bx, fy + 2, bw, FIND_HEIGHT - 4,
                    false, COLOR_MENUBAR);
                recon_draw_text(panel, np->font, bx + 8,
                    fy + (FIND_HEIGHT + ascent) / 2 - 2, bw - 12,
                    BUTTONS[i].label, THEME(BUTTON_TEXT));
                recon_hit_add(panel, bx, fy + 2, bw, FIND_HEIGHT - 4,
                    BUTTONS[i].id);

                bx += bw + 6;
            }
        }
    }

    recon_fill_rect(panel, x, sy, w, STATUS_HEIGHT, COLOR_STATUS_BG);

    char status[256];
    if (np->message[0] != '\0') {
        snprintf(status, sizeof(status), "%s", np->message);
    } else {
        /*
         * The position in the *file*, not on the screen.
         *
         * `cursor_line` above is a display row, which is what the drawing
         * loop needs. Somebody reading "Line 4" means the fourth line of the
         * document, and with wrapping on those are different numbers -- a
         * status line mixing one with a column counted the other way is
         * two facts that cannot both be about the same place.
         */
        snprintf(status, sizeof(status), "Line %d, Column %d   %zu characters%s",
            line_number(np, np->cursor) + 1,
            column_number(np, np->cursor) + 1, np->length,
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

    /* The find bar's own controls. Clicking a field is also how the keyboard
     * moves between the two, so a click on one is a click away from the
     * other. */
    if (hit_id == HIT_FIND) {
        np->replacement_focused = false;
        return true;
    }
    if (hit_id == HIT_REPLACEMENT) {
        np->replacement_focused = true;
        return true;
    }
    if (hit_id == HIT_REPLACE_ONE) {
        do_replace_one(np);
        return true;
    }
    if (hit_id == HIT_REPLACE_ALL) {
        do_replace_all(np);
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
    bool shift = (modifiers & RECON_MOD_SHIFT) != 0;

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
        case XKB_KEY_a:
        case XKB_KEY_A:
            do_select_all(np);
            return true;
        case XKB_KEY_c:
        case XKB_KEY_C:
            do_copy(np, false);
            return true;
        case XKB_KEY_x:
        case XKB_KEY_X:
            do_copy(np, true);
            return true;
        case XKB_KEY_v:
        case XKB_KEY_V:
            do_paste(np);
            return true;
        case XKB_KEY_h:
        case XKB_KEY_H:
            open_replace(np);
            return true;
        case XKB_KEY_f:
        case XKB_KEY_F:
            open_find(np);
            return true;
        default:
            break;
        }
        /* Other control combinations are not text and must not be typed into
         * the buffer. */
        return true;
    }

    /*
     * The find bar has the keyboard while it is up, so typing goes into it
     * rather than into the document behind it. Enter looks for the next
     * match and leaves the bar open, because searching again is the usual
     * next thing; Escape puts it away.
     */
    if (np->finding) {
        if (sym == XKB_KEY_F3) {
            do_find_next(np);
            return true;
        }

        /* Tab moves between the two fields, which is the gesture a form of
         * any kind teaches. */
        if (np->replacing && sym == XKB_KEY_Tab) {
            np->replacement_focused = !np->replacement_focused;
            return true;
        }

        struct recon_edit *edit = (np->replacing && np->replacement_focused)
            ? &np->replacement : &np->find;

        switch (recon_edit_key(edit, sym, modifiers)) {
        case RECON_EDIT_COMMIT:
            /*
             * Enter means the thing the bar is open for: find the next one,
             * or replace this one and find the next. Somebody who opened the
             * replace bar and pressed Enter meant to replace something.
             */
            if (np->replacing) {
                do_replace_one(np);
            } else {
                do_find_next(np);
            }
            return true;
        case RECON_EDIT_CANCEL:
            close_find(np);
            return true;
        case RECON_EDIT_CHANGED:
        case RECON_EDIT_IGNORED:
            return true;
        }
        return true;
    }

    if (sym == XKB_KEY_F3) {
        do_find_next(np);
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

    /*
     * Moving with Shift held extends the selection; moving without it drops
     * whatever was selected. before_move is called on every one of these, so
     * there is no arrow key that quietly forgets to.
     */
    case XKB_KEY_Left:
        before_move(np, shift);
        if (np->cursor > 0) {
            np->cursor--;
        }
        scroll_to_cursor(np);
        return true;

    case XKB_KEY_Right:
        before_move(np, shift);
        if (np->cursor < np->length) {
            np->cursor++;
        }
        scroll_to_cursor(np);
        return true;

    case XKB_KEY_Up:
        before_move(np, shift);
        move_up(np);
        scroll_to_cursor(np);
        return true;

    case XKB_KEY_Down:
        before_move(np, shift);
        move_down(np);
        scroll_to_cursor(np);
        return true;

    case XKB_KEY_Home:
        before_move(np, shift);
        np->cursor = line_start(np, np->cursor);
        return true;

    case XKB_KEY_End:
        before_move(np, shift);
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
    /* Only the one this window loaded for itself. The system's font is shared
     * and cached by size, and freeing it would take it from every other
     * window drawing at that size. */
    if (np->own_font != NULL) {
        recon_font_destroy(np->own_font);
    }
    free(np->text);
    free(np);
}

static const struct recon_appwin_impl NOTEPAD_IMPL = {
    .title = "Notepad",
    .help = "Writing",
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
    np->anchor = NO_ANCHOR;

    /*
     * Wrapping on, which is the opposite of what Notepad has historically
     * done and is the right default anyway: a line that runs off the edge is
     * a line somebody has to scroll sideways to read, and sideways scrolling
     * to read a sentence is the worst way to read a sentence.
     */
    np->wrap = true;

    /* The size the window came up with, so Larger and Smaller move from
     * wherever the reader's settings put it rather than from a constant. */
    np->size = recon_registry_get_int(RECON_REG_USER,
        RECON_ACCESS_FONT_SIZE_KEY, RECON_ACCESS_FONT_SIZE_DEFAULT);

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
