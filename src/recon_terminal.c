/*
 * The ReconOS terminal window.
 *
 * A view onto the command interpreter, not an emulator. There is no pty here
 * and no escape sequences to decode: a line is typed, the interpreter answers
 * with text, and the text is shown. Everything it can do, it can do because
 * ReconOS can do it, which is the point of it existing.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_appwin.h"
#include "recon_icons.h"
#include "recon_cmd.h"
#include "recon_terminal.h"
#include "recon_ui.h"

#define SCROLLBACK_LINES 400
#define LINE_MAX 512
#define INPUT_MAX 512
#define HISTORY_MAX 32
#define PADDING 6
#define LINE_SPACING 2

#define COLOR_BG RECON_RGB(0x0C, 0x10, 0x18)
#define COLOR_TEXT RECON_RGB(0xC8, 0xD4, 0xC8)
#define COLOR_PROMPT RECON_RGB(0x7C, 0xC8, 0x7C)
#define COLOR_INPUT RECON_RGB(0xF0, 0xF0, 0xE8)
#define COLOR_CURSOR RECON_RGB(0x8B, 0x1A, 0x1A)

struct recon_terminal {
    struct recon_font *font;
    struct recon_appwin *win;
    struct recon_cmd_session *session;

    /* Ring of past output. Old lines are overwritten rather than shifted, so
     * a long session does not get slower as it goes. */
    char lines[SCROLLBACK_LINES][LINE_MAX];
    int line_count;
    int line_start;

    char input[INPUT_MAX];
    size_t input_length;
    size_t cursor;

    char history[HISTORY_MAX][INPUT_MAX];
    int history_count;
    int history_pos; /* HISTORY_MAX means "at the new line" */

    int scroll;
    int visible_lines;
};

/* --- Scrollback --- */

static void push_line(struct recon_terminal *term, const char *text) {
    int index;
    if (term->line_count < SCROLLBACK_LINES) {
        index = (term->line_start + term->line_count) % SCROLLBACK_LINES;
        term->line_count++;
    } else {
        index = term->line_start;
        term->line_start = (term->line_start + 1) % SCROLLBACK_LINES;
    }
    snprintf(term->lines[index], LINE_MAX, "%s", text);
}

static const char *line_at(struct recon_terminal *term, int index) {
    if (index < 0 || index >= term->line_count) {
        return "";
    }
    return term->lines[(term->line_start + index) % SCROLLBACK_LINES];
}

/* Split interpreter output on newlines and add each line separately. */
static void push_output(struct recon_terminal *term, const char *text) {
    char buffer[LINE_MAX];
    size_t used = 0;

    for (const char *p = text; *p != '\0'; p++) {
        if (*p == '\n' || used >= sizeof(buffer) - 1) {
            buffer[used] = '\0';
            push_line(term, buffer);
            used = 0;
            if (*p != '\n') {
                /* The character that overflowed still belongs to the next
                 * line rather than being dropped. */
                buffer[used++] = *p;
            }
            continue;
        }
        buffer[used++] = *p;
    }

    if (used > 0) {
        buffer[used] = '\0';
        push_line(term, buffer);
    }
}

/* Show the newest output. */
static void scroll_to_end(struct recon_terminal *term) {
    term->scroll = term->line_count - term->visible_lines;
    if (term->scroll < 0) {
        term->scroll = 0;
    }
}

/* --- Running a line --- */

static void submit(struct recon_terminal *term) {
    char prompt[LINE_MAX + RECON_PATH_MAX];
    snprintf(prompt, sizeof(prompt), "%s> %s",
        recon_cmd_cwd(term->session), term->input);
    push_line(term, prompt);

    if (term->input_length > 0) {
        /* Remember it, unless it repeats the last one. */
        if (term->history_count == 0 ||
                strcmp(term->history[term->history_count - 1], term->input) != 0) {
            if (term->history_count == HISTORY_MAX) {
                memmove(term->history[0], term->history[1],
                    sizeof(term->history[0]) * (HISTORY_MAX - 1));
                term->history_count--;
            }
            snprintf(term->history[term->history_count++], INPUT_MAX, "%s", term->input);
        }

        const char *output = recon_cmd_run(term->session, term->input);
        if (output != NULL && *output != '\0') {
            push_output(term, output);
        }

        if (recon_cmd_should_exit(term->session)) {
            recon_appwin_hide(term->win);
        }
    }

    term->input[0] = '\0';
    term->input_length = 0;
    term->cursor = 0;
    term->history_pos = HISTORY_MAX;
    scroll_to_end(term);
}

static void recall_history(struct recon_terminal *term, int direction) {
    if (term->history_count == 0) {
        return;
    }

    int pos = term->history_pos;
    if (pos > term->history_count) {
        pos = term->history_count;
    }
    pos += direction;

    if (pos < 0) {
        pos = 0;
    }
    if (pos >= term->history_count) {
        /* Past the newest is the line being typed, which is empty. */
        term->history_pos = HISTORY_MAX;
        term->input[0] = '\0';
        term->input_length = 0;
        term->cursor = 0;
        return;
    }

    term->history_pos = pos;
    snprintf(term->input, INPUT_MAX, "%s", term->history[pos]);
    term->input_length = strlen(term->input);
    term->cursor = term->input_length;
}

/* --- Drawing --- */

static void terminal_draw(void *user, struct recon_panel *panel,
        int x, int y, int w, int h) {
    struct recon_terminal *term = user;
    int line_height = recon_font_line_height(term->font) + LINE_SPACING;
    int ascent = recon_font_ascent(term->font);
    if (line_height <= 0) {
        line_height = 16;
    }

    recon_fill_rect(panel, x, y, w, h, COLOR_BG);
    recon_draw_bevel(panel, x, y, w, h, true);

    /* One line is reserved at the bottom for what is being typed. */
    term->visible_lines = (h - PADDING * 2) / line_height - 1;
    if (term->visible_lines < 1) {
        term->visible_lines = 1;
    }

    for (int row = 0; row < term->visible_lines; row++) {
        int index = term->scroll + row;
        if (index >= term->line_count) {
            break;
        }
        recon_draw_text(panel, term->font, x + PADDING,
            y + PADDING + row * line_height + ascent, w - PADDING * 2,
            line_at(term, index), COLOR_TEXT);
    }

    /* The prompt and the line being typed. */
    int input_y = y + PADDING + term->visible_lines * line_height;
    int baseline = input_y + ascent;

    char prompt[RECON_PATH_MAX + 4];
    snprintf(prompt, sizeof(prompt), "%s> ", recon_cmd_cwd(term->session));
    int prompt_w = recon_text_width(term->font, prompt);

    recon_draw_text(panel, term->font, x + PADDING, baseline,
        w - PADDING * 2, prompt, COLOR_PROMPT);
    recon_draw_text(panel, term->font, x + PADDING + prompt_w, baseline,
        w - PADDING * 2 - prompt_w, term->input, COLOR_INPUT);

    /* Caret, placed by measuring the text before it. */
    char saved = term->input[term->cursor];
    term->input[term->cursor] = '\0';
    int caret_x = x + PADDING + prompt_w + recon_text_width(term->font, term->input);
    term->input[term->cursor] = saved;

    recon_fill_rect(panel, caret_x, input_y, 2, line_height - LINE_SPACING,
        COLOR_CURSOR);
}

/* --- Input --- */

static void insert_char(struct recon_terminal *term, char c) {
    if (term->input_length + 1 >= INPUT_MAX) {
        return;
    }
    memmove(term->input + term->cursor + 1, term->input + term->cursor,
        term->input_length - term->cursor + 1);
    term->input[term->cursor] = c;
    term->input_length++;
    term->cursor++;
}

static bool terminal_key(void *user, xkb_keysym_t sym, uint32_t modifiers) {
    struct recon_terminal *term = user;

    switch (sym) {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        submit(term);
        return true;

    case XKB_KEY_BackSpace:
        if (term->cursor > 0) {
            memmove(term->input + term->cursor - 1, term->input + term->cursor,
                term->input_length - term->cursor + 1);
            term->cursor--;
            term->input_length--;
        }
        return true;

    case XKB_KEY_Delete:
        if (term->cursor < term->input_length) {
            memmove(term->input + term->cursor, term->input + term->cursor + 1,
                term->input_length - term->cursor);
            term->input_length--;
        }
        return true;

    case XKB_KEY_Left:
        if (term->cursor > 0) {
            term->cursor--;
        }
        return true;

    case XKB_KEY_Right:
        if (term->cursor < term->input_length) {
            term->cursor++;
        }
        return true;

    case XKB_KEY_Up:
        recall_history(term, -1);
        return true;

    case XKB_KEY_Down:
        recall_history(term, 1);
        return true;

    case XKB_KEY_Home:
        term->cursor = 0;
        return true;

    case XKB_KEY_End:
        term->cursor = term->input_length;
        return true;

    case XKB_KEY_Page_Up:
        term->scroll -= term->visible_lines;
        if (term->scroll < 0) {
            term->scroll = 0;
        }
        return true;

    case XKB_KEY_Page_Down:
        term->scroll += term->visible_lines;
        scroll_to_end(term);
        return true;

    default:
        break;
    }

    uint32_t codepoint = xkb_keysym_to_utf32(sym);
    if (codepoint >= 32 && codepoint < 127) {
        insert_char(term, (char)codepoint);
        return true;
    }
    return false;
}

static void terminal_scroll(void *user, double delta) {
    struct recon_terminal *term = user;
    term->scroll += (delta > 0) ? 3 : -3;

    int max = term->line_count - term->visible_lines;
    if (max < 0) {
        max = 0;
    }
    if (term->scroll > max) {
        term->scroll = max;
    }
    if (term->scroll < 0) {
        term->scroll = 0;
    }
}

static void terminal_destroy(void *user) {
    struct recon_terminal *term = user;
    recon_cmd_session_destroy(term->session);
    free(term);
}

static const struct recon_appwin_impl TERMINAL_IMPL = {
    .title = "ReconOS Terminal",
    .icon = RECON_ICON_TERMINAL,
    .default_width = 640,
    .default_height = 400,
    .min_width = 320,
    .min_height = 200,
    .draw = terminal_draw,
    .key = terminal_key,
    .scroll = terminal_scroll,
    .destroy = terminal_destroy,
};

struct recon_appwin *recon_terminal_create(struct recon_server *server,
        struct recon_font *font) {
    struct recon_terminal *term = calloc(1, sizeof(*term));
    if (term == NULL) {
        return NULL;
    }

    term->font = font;
    term->history_pos = HISTORY_MAX;

    term->session = recon_cmd_session_create(server);
    if (term->session == NULL) {
        free(term);
        return NULL;
    }

    push_line(term, "ReconOS 0.0.7");
    push_line(term, "Type 'help' for a list of commands.");
    push_line(term, "");

    term->win = recon_appwin_create(server, font, &TERMINAL_IMPL, term);
    if (term->win == NULL) {
        recon_cmd_session_destroy(term->session);
        free(term);
        return NULL;
    }
    return term->win;
}
