/*
 * ReconOS Notepad. See include/recon_notepad.h.
 *
 * Text is held as one buffer with newlines in it, and the cursor is an offset
 * into that buffer. Keeping a single source of truth means insertion and
 * deletion cannot leave the cursor pointing somewhere the text does not agree
 * with, which is the usual way simple editors go wrong.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_appwin.h"
#include "recon_notepad.h"
#include "recon_ui.h"

#define PADDING 6
#define STATUS_HEIGHT 22
#define LINE_SPACING 2
#define TAB_WIDTH 4

#define COLOR_BG RECON_RGB(0xFF, 0xFF, 0xFF)
#define COLOR_TEXT RECON_RGB(0x10, 0x10, 0x10)
#define COLOR_CURSOR RECON_RGB(0x8B, 0x1A, 0x1A)
#define COLOR_STATUS_BG RECON_RGB(0xC0, 0xC0, 0xC0)
#define COLOR_STATUS_TEXT RECON_RGB(0x30, 0x30, 0x30)

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
};

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

/* --- Drawing --- */

static void notepad_draw(void *user, struct recon_panel *panel,
        int x, int y, int w, int h) {
    struct recon_notepad *np = user;
    int line_height = recon_font_line_height(np->font) + LINE_SPACING;
    int ascent = recon_font_ascent(np->font);
    if (line_height <= 0) {
        line_height = 16;
    }

    int text_h = h - STATUS_HEIGHT;
    np->visible_lines = text_h > 0 ? text_h / line_height : 0;

    recon_fill_rect(panel, x, y, w, text_h, COLOR_BG);
    recon_draw_bevel(panel, x, y, w, text_h, true);

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

    /* Status line. */
    int sy = y + h - STATUS_HEIGHT;
    recon_fill_rect(panel, x, sy, w, STATUS_HEIGHT, COLOR_STATUS_BG);

    char status[128];
    snprintf(status, sizeof(status), "Line %d, Column %d   %zu characters%s",
        cursor_line + 1, column_number(np, np->cursor) + 1, np->length,
        np->modified ? "   (modified)" : "");
    recon_draw_text(panel, np->font, x + PADDING,
        sy + (STATUS_HEIGHT + ascent) / 2 - 2, w - PADDING * 2,
        status, COLOR_STATUS_TEXT);
}

/* --- Input --- */

static bool notepad_key(void *user, xkb_keysym_t sym, uint32_t modifiers) {
    struct recon_notepad *np = user;

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
        return true;
    }

    return false;
}

static void notepad_scroll(void *user, double delta) {
    struct recon_notepad *np = user;
    np->scroll_line += (delta > 0) ? 3 : -3;
    if (np->scroll_line < 0) {
        np->scroll_line = 0;
    }
}

static void notepad_destroy(void *user) {
    struct recon_notepad *np = user;
    free(np->text);
    free(np);
}

static const struct recon_appwin_impl NOTEPAD_IMPL = {
    .title = "Notepad",
    .default_width = 520,
    .default_height = 400,
    .min_width = 260,
    .min_height = 180,
    .draw = notepad_draw,
    .key = notepad_key,
    .scroll = notepad_scroll,
    .destroy = notepad_destroy,
};

struct recon_appwin *recon_notepad_create(struct recon_server *server,
        struct recon_font *font) {
    struct recon_notepad *np = calloc(1, sizeof(*np));
    if (np == NULL) {
        return NULL;
    }
    np->font = font;

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
