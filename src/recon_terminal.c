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
#include <strings.h>

#include "ReconOS.h"
#include "recon_appwin.h"
#include "recon_icons.h"
#include "recon_clip.h"
#include "recon_cmd.h"
#include "recon_registry.h"
#include "recon_terminal.h"
#include "recon_theme.h"
#include "recon_ui.h"

#define SCROLLBACK_LINES 400
#define LINE_MAX 512
#define INPUT_MAX 512
#define HISTORY_MAX 32
#define PADDING 6
#define LINE_SPACING 2

/*
 * --- Colour schemes ---
 *
 * A terminal is a place people look at for a long time, and which colours it
 * uses is a preference rather than a matter of taste anybody should have
 * imposed on them.
 *
 * "Recon" takes its colours from the skin, which is what this always did and
 * is why it is still the default: changing the system's appearance should
 * change the terminal with it. The rest are fixed sets, so choosing one is
 * choosing to opt *out* of the skin for this one window.
 *
 * These are presets. Presets cannot be deleted -- a list of choices somebody
 * can empty is a list that eventually has nothing in it -- and a custom scheme,
 * when there is a way to make one, can be.
 */
struct term_scheme {
    const char *name;
    const char *detail;
    /* True for the one that follows the skin; the colours below are ignored. */
    bool from_theme;
    uint32_t bg, text, prompt, input, error, notice, caret;
};

static const struct term_scheme SCHEMES[] = {
    { "Recon", "Follows the system skin", true, 0, 0, 0, 0, 0, 0, 0 },

    /*
     * PowerShell's console, which is a specific and well-known blue: the
     * classic host remaps DarkMagenta to #012456 and draws on it in near
     * white. Both of those are copied exactly, because they are the part
     * people mean when they say they like how it looks.
     *
     * The error colour is not copied. PowerShell 5's is Red on that blue,
     * which is close to unreadable and is the single most complained-about
     * thing about the console; PowerShell 7 changed it for the same reason.
     * A red that can be read on this background serves the person better than
     * one that is faithful to a mistake.
     */
    { "PowerShell", "Dark blue and near white", false,
      RECON_RGB(0x01, 0x24, 0x56), RECON_RGB(0xEE, 0xED, 0xF0),
      RECON_RGB(0x61, 0xD6, 0xD6), RECON_RGB(0xFF, 0xFF, 0xFF),
      RECON_RGB(0xF2, 0x6D, 0x6D), RECON_RGB(0xF9, 0xF1, 0xA5),
      RECON_RGB(0xEE, 0xED, 0xF0) },

    { "Green Screen", "Phosphor on black", false,
      RECON_RGB(0x00, 0x00, 0x00), RECON_RGB(0x33, 0xEE, 0x33),
      RECON_RGB(0x00, 0xBB, 0x00), RECON_RGB(0x77, 0xFF, 0x77),
      RECON_RGB(0xFF, 0x66, 0x55), RECON_RGB(0xEE, 0xEE, 0x66),
      RECON_RGB(0x33, 0xEE, 0x33) },

    { "Paper", "Dark on light, for a bright room", false,
      RECON_RGB(0xFA, 0xFA, 0xF7), RECON_RGB(0x1A, 0x1A, 0x1A),
      RECON_RGB(0x0B, 0x53, 0x94), RECON_RGB(0x00, 0x00, 0x00),
      RECON_RGB(0xB0, 0x00, 0x20), RECON_RGB(0x7A, 0x5C, 0x00),
      RECON_RGB(0x1A, 0x1A, 0x1A) },
};

#define SCHEME_COUNT ((int)(sizeof(SCHEMES) / sizeof(SCHEMES[0])))

/* Where the choice is kept. A registry key cannot contain a space; the value
 * can, which is why the name is the value and not the key. */
#define SCHEME_KEY "terminal/scheme"

/*
 * What a line of scrollback is, so it can be drawn in the colour that says so.
 *
 * The terminal knows two of these for free -- it typed the echo itself, and it
 * wrote the banner -- and the interpreter says which of the rest are failures.
 */
enum term_line {
    TERM_LINE_PLAIN,
    TERM_LINE_ECHO,
    TERM_LINE_ERROR,
    TERM_LINE_NOTICE,
};

struct recon_terminal {
    struct recon_font *font;
    struct recon_appwin *win;
    struct recon_cmd_session *session;

    /* Which scheme, and the colours resolved from it. Resolved at draw time
     * rather than kept, so a skin change reaches the "Recon" scheme without
     * anything having to tell the terminal about it. */
    int scheme;

    /* Ring of past output. Old lines are overwritten rather than shifted, so
     * a long session does not get slower as it goes. */
    char lines[SCROLLBACK_LINES][LINE_MAX];
    unsigned char kinds[SCROLLBACK_LINES];
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

/* --- The scheme in use --- */

/* The colours to draw with, resolved now rather than remembered. */
struct term_colours {
    uint32_t bg, text, prompt, input, error, notice, caret;
};

static void colours_of(const struct recon_terminal *term,
        struct term_colours *out) {
    const struct term_scheme *scheme = &SCHEMES[
        (term->scheme >= 0 && term->scheme < SCHEME_COUNT) ? term->scheme : 0];

    if (!scheme->from_theme) {
        out->bg = scheme->bg;
        out->text = scheme->text;
        out->prompt = scheme->prompt;
        out->input = scheme->input;
        out->error = scheme->error;
        out->notice = scheme->notice;
        out->caret = scheme->caret;
        return;
    }

    /*
     * Asked of the theme every time, because a skin can change while this
     * window is open and a colour copied at construction would be the old
     * skin's until the terminal was closed and opened again.
     */
    out->bg = recon_theme_color(RECON_THEME_READOUT);
    out->text = recon_theme_color(RECON_THEME_READOUT_TEXT);
    out->prompt = recon_theme_color(RECON_THEME_READOUT_ACCENT);
    out->input = recon_theme_color(RECON_THEME_READOUT_INPUT);
    out->caret = RECON_RGB(0x8B, 0x1A, 0x1A);

    /*
     * The skin has no role for "this went wrong in a terminal", and inventing
     * one would mean every skin ever written suddenly has a hole in it. A red
     * and a yellow that read on any of the readout backgrounds, chosen once
     * here rather than fifty times in the skins.
     */
    out->error = RECON_RGB(0xE0, 0x5A, 0x50);
    out->notice = RECON_RGB(0xD8, 0xB0, 0x40);
}

static int scheme_named(const char *name) {
    for (int i = 0; i < SCHEME_COUNT; i++) {
        if (strcasecmp(SCHEMES[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* --- Scrollback --- */

/* What the interpreter says the line at this offset is. */
static enum term_line cmd_kind(struct recon_terminal *term, size_t offset) {
    switch (recon_cmd_line_kind(term->session, offset)) {
    case RECON_CMD_LINE_ERROR:  return TERM_LINE_ERROR;
    case RECON_CMD_LINE_NOTICE: return TERM_LINE_NOTICE;
    case RECON_CMD_LINE_PLAIN:  break;
    }
    return TERM_LINE_PLAIN;
}

static void push_kind(struct recon_terminal *term, const char *text,
        enum term_line kind) {
    int index;
    if (term->line_count < SCROLLBACK_LINES) {
        index = (term->line_start + term->line_count) % SCROLLBACK_LINES;
        term->line_count++;
    } else {
        index = term->line_start;
        term->line_start = (term->line_start + 1) % SCROLLBACK_LINES;
    }
    snprintf(term->lines[index], LINE_MAX, "%s", text);
    term->kinds[index] = (unsigned char)kind;
}

static void push_line(struct recon_terminal *term, const char *text) {
    push_kind(term, text, TERM_LINE_PLAIN);
}

static const char *line_at(struct recon_terminal *term, int index) {
    if (index < 0 || index >= term->line_count) {
        return "";
    }
    return term->lines[(term->line_start + index) % SCROLLBACK_LINES];
}

static enum term_line kind_at(struct recon_terminal *term, int index) {
    if (index < 0 || index >= term->line_count) {
        return TERM_LINE_PLAIN;
    }
    return (enum term_line)
        term->kinds[(term->line_start + index) % SCROLLBACK_LINES];
}

/*
 * Split interpreter output on newlines and add each line separately, asking
 * the interpreter what each one is.
 *
 * The offset of the line's first byte is what the interpreter keys its answer
 * on, so it is tracked here rather than counted lines -- the two of us do not
 * have to agree about what happens to a line long enough to be split, and this
 * is a terminal, so some of them are.
 */
static void push_output(struct recon_terminal *term, const char *text) {
    char buffer[LINE_MAX];
    size_t used = 0;

    /*
     * Where the interpreter's current line began. It advances past a newline
     * and *not* past an overflow: a line too long for the buffer is shown as
     * two rows, but it is still one line of output and both rows are the same
     * kind. Advancing it there would look up an offset in the middle of the
     * text, find no mark, and draw the second half of a failure in the colour
     * of ordinary output.
     */
    size_t line_begins = 0;

    for (const char *p = text; *p != '\0'; p++) {
        if (*p == '\n' || used >= sizeof(buffer) - 1) {
            buffer[used] = '\0';
            push_kind(term, buffer, cmd_kind(term, line_begins));
            used = 0;

            if (*p == '\n') {
                line_begins = (size_t)(p - text) + 1;
            } else {
                /* The character that overflowed still belongs to the next
                 * row rather than being dropped. */
                buffer[used++] = *p;
            }
            continue;
        }
        buffer[used++] = *p;
    }

    if (used > 0) {
        buffer[used] = '\0';
        push_kind(term, buffer, cmd_kind(term, line_begins));
    }
}

/* Show the newest output. */
static void scroll_to_end(struct recon_terminal *term) {
    term->scroll = term->line_count - term->visible_lines;
    if (term->scroll < 0) {
        term->scroll = 0;
    }
}

/* --- The terminal's own commands --- */

/*
 * Handled here rather than in the interpreter, and returning whether it was.
 *
 * `scheme` changes how this window is drawn. The interpreter is reachable over
 * a socket, where there is no window and no colours, so a command that only
 * means something to one of its two front ends does not belong in it.
 */
static bool local_command(struct recon_terminal *term, const char *line) {
    while (*line == ' ') {
        line++;
    }
    if (strncasecmp(line, "scheme", 6) != 0 ||
            (line[6] != '\0' && line[6] != ' ')) {
        return false;
    }

    const char *rest = line + 6;
    while (*rest == ' ') {
        rest++;
    }

    if (*rest == '\0') {
        char row[LINE_MAX];
        push_kind(term, "Colour schemes:", TERM_LINE_NOTICE);
        for (int i = 0; i < SCHEME_COUNT; i++) {
            snprintf(row, sizeof(row), "  %c %-14s %s",
                i == term->scheme ? '*' : ' ', SCHEMES[i].name,
                SCHEMES[i].detail);
            push_line(term, row);
        }
        push_line(term, "");
        push_line(term, "'scheme <name>' changes it. These are the ones that "
            "ship, and cannot be removed.");
        return true;
    }

    int found = scheme_named(rest);
    if (found < 0) {
        char said[LINE_MAX];
        snprintf(said, sizeof(said),
            "There is no scheme called '%.64s'. 'scheme' lists them.", rest);
        push_kind(term, said, TERM_LINE_ERROR);
        return true;
    }

    term->scheme = found;
    recon_registry_set(RECON_REG_USER, SCHEME_KEY, SCHEMES[found].name);

    char said[LINE_MAX];
    snprintf(said, sizeof(said), "Scheme is now %s.", SCHEMES[found].name);
    push_kind(term, said, TERM_LINE_NOTICE);
    return true;
}

/* --- Running a line --- */

static void submit(struct recon_terminal *term) {
    char prompt[LINE_MAX + RECON_PATH_MAX];
    snprintf(prompt, sizeof(prompt), "%s> %s",
        recon_cmd_cwd(term->session), term->input);
    push_kind(term, prompt, TERM_LINE_ECHO);

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

        /*
         * The terminal's own commands first. `scheme` changes how this window
         * looks, which is not a thing about the system and so not a thing the
         * interpreter should carry -- a socket connection has no colours to
         * set.
         */
        if (!local_command(term, term->input)) {
            const char *output = recon_cmd_run(term->session, term->input);
            if (output != NULL && *output != '\0') {
                push_output(term, output);
            }
        }

        /*
         * A blank line before the next prompt, which is the one piece of
         * PowerShell's layout worth taking: it separates one command and its
         * answer from the next, and a screen of commands run back to back is
         * otherwise a wall.
         */
        push_line(term, "");

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

    struct term_colours colour;
    colours_of(term, &colour);

    recon_fill_rect(panel, x, y, w, h, colour.bg);
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
        uint32_t ink = colour.text;
        switch (kind_at(term, index)) {
        case TERM_LINE_ECHO:   ink = colour.prompt; break;
        case TERM_LINE_ERROR:  ink = colour.error; break;
        case TERM_LINE_NOTICE: ink = colour.notice; break;
        case TERM_LINE_PLAIN:  break;
        }

        recon_draw_text(panel, term->font, x + PADDING,
            y + PADDING + row * line_height + ascent, w - PADDING * 2,
            line_at(term, index), ink);
    }

    /* The prompt and the line being typed. */
    int input_y = y + PADDING + term->visible_lines * line_height;
    int baseline = input_y + ascent;

    /*
     * The path and the arrow are drawn separately so the arrow can stay in the
     * text colour. In one colour the whole thing reads as decoration; with the
     * arrow left plain, the eye finds where the path stops and where typing
     * starts, which is the thing a prompt is for.
     */
    char where[RECON_PATH_MAX];
    snprintf(where, sizeof(where), "%s", recon_cmd_cwd(term->session));
    int where_w = recon_text_width(term->font, where);
    int arrow_w = recon_text_width(term->font, "> ");
    int prompt_w = where_w + arrow_w;

    recon_draw_text(panel, term->font, x + PADDING, baseline,
        w - PADDING * 2, where, colour.prompt);
    recon_draw_text(panel, term->font, x + PADDING + where_w, baseline,
        w - PADDING * 2 - where_w, "> ", colour.text);
    recon_draw_text(panel, term->font, x + PADDING + prompt_w, baseline,
        w - PADDING * 2 - prompt_w, term->input, colour.input);

    /* Caret, placed by measuring the text before it. */
    char saved = term->input[term->cursor];
    term->input[term->cursor] = '\0';
    int caret_x = x + PADDING + prompt_w + recon_text_width(term->font, term->input);
    term->input[term->cursor] = saved;

    recon_fill_rect(panel, caret_x, input_y, 2, line_height - LINE_SPACING,
        colour.caret);
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

    if ((modifiers & RECON_MOD_CTRL) != 0) {
        /*
         * Paste, one character at a time through the same path typing takes,
         * so the input line's own limit applies.
         *
         * A newline ends it rather than submitting the line. Pasting several
         * commands and having them all run is how somebody loses a directory
         * to a paste they meant to read first -- the first line goes in, and
         * pressing Return is still a decision.
         */
        if (sym == XKB_KEY_v || sym == XKB_KEY_V) {
            const char *held = recon_clip_text();
            for (const char *c = held; *c != '\0'; c++) {
                if (*c == '\n' || *c == '\r') {
                    break;
                }
                insert_char(term, *c);
            }
            return true;
        }

        /* Other control combinations are not text and must not be typed. */
        return true;
    }

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
    .help = "The Terminal",
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

    /*
     * A fixed-width face, not the system one it was handed.
     *
     * The interpreter writes its tables with `%-20s` and every one of them was
     * being thrown away: in a proportional font the columns wander by a
     * character or two on each row, so `apps` and `ls` and `firewall` all came
     * out as a heap of words. The window frame keeps the font it was given --
     * a title bar is not a terminal -- so only the contents change.
     */
    int size = recon_font_line_height(font);
    struct recon_font *mono = recon_font_monospace(size > 0 ? size : 14);
    term->font = mono != NULL ? mono : font;
    term->history_pos = HISTORY_MAX;

    /* The remembered scheme, or the one that follows the skin. An unknown name
     * -- a scheme removed since, a hand-edited registry -- falls back rather
     * than leaving the window with no colours at all. */
    const char *want = recon_registry_get(RECON_REG_USER, SCHEME_KEY, "");
    int chosen = (want != NULL && *want != '\0') ? scheme_named(want) : -1;
    term->scheme = chosen >= 0 ? chosen : 0;

    term->session = recon_cmd_session_create(server);
    if (term->session == NULL) {
        free(term);
        return NULL;
    }

    push_kind(term, RECONOS_NAME " " RECONOS_VERSION, TERM_LINE_NOTICE);
    push_line(term, "Type 'help' for a list of commands, or 'scheme' to "
        "change the colours.");
    push_line(term, "");

    term->win = recon_appwin_create(server, font, &TERMINAL_IMPL, term);
    if (term->win == NULL) {
        recon_cmd_session_destroy(term->session);
        free(term);
        return NULL;
    }
    return term->win;
}
