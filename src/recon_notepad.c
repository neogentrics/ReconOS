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
#include "recon_ui.h"

#define PADDING 6
#define MENUBAR_HEIGHT 22
#define STATUS_HEIGHT 22
#define LINE_SPACING 2
#define TAB_WIDTH 4

#define MENU_WIDTH 150
#define MENU_ITEM_HEIGHT 22

#define COLOR_BG RECON_RGB(0xFF, 0xFF, 0xFF)
#define COLOR_TEXT RECON_RGB(0x10, 0x10, 0x10)
#define COLOR_CURSOR RECON_RGB(0x8B, 0x1A, 0x1A)
#define COLOR_STATUS_BG RECON_RGB(0xC0, 0xC0, 0xC0)
#define COLOR_STATUS_TEXT RECON_RGB(0x30, 0x30, 0x30)
#define COLOR_MENUBAR RECON_RGB(0xC0, 0xC0, 0xC0)
#define COLOR_MENU RECON_RGB(0xC8, 0xC8, 0xC8)
#define COLOR_MENU_BORDER RECON_RGB(0x30, 0x30, 0x30)
#define COLOR_MENU_HILITE RECON_RGB(0x30, 0x50, 0x90)
#define COLOR_MENU_HILITE_TEXT RECON_RGB(0xFF, 0xFF, 0xFF)
#define COLOR_WARNING RECON_RGB(0x8B, 0x1A, 0x1A)

#define HIT_FILE (RECON_APPWIN_HIT_USER + 1)
#define HIT_TEXT (RECON_APPWIN_HIT_USER + 2)
#define HIT_MENU_BASE (RECON_APPWIN_HIT_USER + 10)

/* The File menu, in the order it is shown. */
enum file_command {
    FILE_NEW,
    FILE_OPEN,
    FILE_SAVE,
    FILE_SAVE_AS,
    FILE_CLOSE,
};

static const struct {
    const char *label;
    enum file_command command;
    bool separator_after;
} FILE_MENU[] = {
    { "New",     FILE_NEW,     false },
    { "Open...", FILE_OPEN,    true  },
    { "Save",    FILE_SAVE,    false },
    { "Save As...", FILE_SAVE_AS, true },
    { "Close",   FILE_CLOSE,   false },
};

#define FILE_MENU_COUNT ((int)(sizeof(FILE_MENU) / sizeof(FILE_MENU[0])))

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

    bool menu_open;
    int menu_hover;

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

static void insert_char(struct recon_notepad *np, char c) {
    if (!ensure_capacity(np, np->length + 1)) {
        return;
    }
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
    np->menu_open = false;

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

/* The menu bar, and the File menu when it is down. */
static void draw_menubar(struct recon_notepad *np, struct recon_panel *panel,
        int x, int y, int w) {
    int ascent = recon_font_ascent(np->font);

    recon_fill_rect(panel, x, y, w, MENUBAR_HEIGHT, COLOR_MENUBAR);
    recon_fill_rect(panel, x, y + MENUBAR_HEIGHT - 1, w, 1,
        RECON_RGB(0x90, 0x90, 0x90));

    int file_w = recon_text_width(np->font, "File") + 20;
    if (np->menu_open) {
        recon_fill_rect(panel, x + 2, y + 1, file_w, MENUBAR_HEIGHT - 2,
            COLOR_MENU_HILITE);
    }
    recon_draw_text(panel, np->font, x + 12, y + (MENUBAR_HEIGHT + ascent) / 2 - 2,
        file_w, "File",
        np->menu_open ? COLOR_MENU_HILITE_TEXT : COLOR_TEXT);
    recon_hit_add(panel, x + 2, y + 1, file_w, MENUBAR_HEIGHT - 2, HIT_FILE);
}

static void draw_file_menu(struct recon_notepad *np, struct recon_panel *panel,
        int x, int y) {
    int ascent = recon_font_ascent(np->font);
    int height = FILE_MENU_COUNT * MENU_ITEM_HEIGHT + 6;
    int mx = x + 2;
    int my = y + MENUBAR_HEIGHT;

    recon_fill_rect(panel, mx, my, MENU_WIDTH, height, COLOR_MENU);
    recon_stroke_rect(panel, mx, my, MENU_WIDTH, height, COLOR_MENU_BORDER);

    for (int i = 0; i < FILE_MENU_COUNT; i++) {
        int iy = my + 3 + i * MENU_ITEM_HEIGHT;
        bool hovered = (i == np->menu_hover);

        if (hovered) {
            recon_fill_rect(panel, mx + 2, iy, MENU_WIDTH - 4, MENU_ITEM_HEIGHT,
                COLOR_MENU_HILITE);
        }

        recon_draw_text(panel, np->font, mx + 10,
            iy + (MENU_ITEM_HEIGHT + ascent) / 2 - 2, MENU_WIDTH - 20,
            FILE_MENU[i].label,
            hovered ? COLOR_MENU_HILITE_TEXT : COLOR_TEXT);

        if (FILE_MENU[i].separator_after) {
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
    if (np->menu_open) {
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

    if (hit_id >= HIT_MENU_BASE && np->menu_open) {
        int index = (int)(hit_id - HIT_MENU_BASE);
        if (index >= 0 && index < FILE_MENU_COUNT) {
            run_command(np, FILE_MENU[index].command);
        }
        return true;
    }

    if (hit_id == HIT_FILE) {
        np->menu_open = !np->menu_open;
        np->menu_hover = -1;
        return true;
    }

    /* A click anywhere else closes the menu rather than choosing from it,
     * which is what clicking away from an open menu should do. */
    if (np->menu_open) {
        np->menu_open = false;
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

    if (!np->menu_open) {
        np->menu_hover = -1;
        return;
    }

    int hover = -1;
    if (hit_id >= HIT_MENU_BASE) {
        int index = (int)(hit_id - HIT_MENU_BASE);
        if (index >= 0 && index < FILE_MENU_COUNT) {
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
        default:
            break;
        }
        /* Other control combinations are not text and must not be typed into
         * the buffer. */
        return true;
    }

    if (np->menu_open && sym == XKB_KEY_Escape) {
        np->menu_open = false;
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

struct recon_appwin *recon_notepad_create(struct recon_server *server,
        struct recon_font *font) {
    struct recon_notepad *np = calloc(1, sizeof(*np));
    if (np == NULL) {
        return NULL;
    }
    np->font = font;
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
