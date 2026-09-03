/*
 * ReconOS Calculator. See include/recon_calc.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_appwin.h"
#include "recon_icons.h"
#include "recon_calc.h"
#include "recon_module.h"
#include "recon_ui.h"

#define DISPLAY_HEIGHT 44
#define PAD_PADDING 6
#define KEY_GAP 4
#define COLS 4
#define ROWS 5

#define DIGITS_MAX 16

#define COLOR_BG RECON_RGB(0xC0, 0xC0, 0xC0)
#define COLOR_DISPLAY RECON_RGB(0x18, 0x1C, 0x24)
#define COLOR_DISPLAY_TEXT RECON_RGB(0x9C, 0xE8, 0x9C)
#define COLOR_KEY RECON_RGB(0xD0, 0xD0, 0xD0)
#define COLOR_KEY_OP RECON_RGB(0xB8, 0xBC, 0xC8)
#define COLOR_KEY_ACCENT RECON_RGB(0x8B, 0x1A, 0x1A)
#define COLOR_KEY_TEXT RECON_RGB(0x10, 0x10, 0x10)
#define COLOR_ACCENT_TEXT RECON_RGB(0xF4, 0xF4, 0xF4)

enum key_kind {
    KEY_DIGIT,
    KEY_OP,
    KEY_EQUALS,
    KEY_CLEAR,
    KEY_BACKSPACE,
    KEY_SIGN,
    KEY_DOT,
};

struct calc_key {
    const char *label;
    enum key_kind kind;
    char value; /* digit character, or operator symbol */
};

/*
 * Laid out as a keypad rather than in rows of related functions, because the
 * digits should sit where a number pad puts them.
 */
static const struct calc_key KEYS[ROWS][COLS] = {
    {{"C", KEY_CLEAR, 0},   {"+/-", KEY_SIGN, 0}, {"<-", KEY_BACKSPACE, 0}, {"/", KEY_OP, '/'}},
    {{"7", KEY_DIGIT, '7'}, {"8", KEY_DIGIT, '8'}, {"9", KEY_DIGIT, '9'},   {"*", KEY_OP, '*'}},
    {{"4", KEY_DIGIT, '4'}, {"5", KEY_DIGIT, '5'}, {"6", KEY_DIGIT, '6'},   {"-", KEY_OP, '-'}},
    {{"1", KEY_DIGIT, '1'}, {"2", KEY_DIGIT, '2'}, {"3", KEY_DIGIT, '3'},   {"+", KEY_OP, '+'}},
    {{"0", KEY_DIGIT, '0'}, {".", KEY_DOT, 0},     {"", KEY_DIGIT, 0},      {"=", KEY_EQUALS, 0}},
};

struct recon_calc {
    struct recon_font *font;

    /* Holds a typed number, a result, or an error message, so it is sized
     * for the longest of those rather than for DIGITS_MAX. */
    char entry[48];
    double accumulator;         /* the running result */
    char pending_op;            /* operator waiting for its right-hand side */
    bool entering;              /* true while the entry is the user's, not a result */
    bool error;

    /* Remembered so the layout and the hit regions agree. */
    int key_x[COLS], key_y[ROWS], key_w, key_h;
};

/* --- Arithmetic --- */

static double current_value(struct recon_calc *calc) {
    return atof(calc->entry);
}

static void show_value(struct recon_calc *calc, double value) {
    if (isnan(value) || isinf(value)) {
        snprintf(calc->entry, sizeof(calc->entry), "0");
        calc->error = true;
        return;
    }

    /* Print without a trailing ".000000" when the result is whole. */
    if (value == (long long)value && fabs(value) < 1e15) {
        snprintf(calc->entry, sizeof(calc->entry), "%lld", (long long)value);
    } else {
        snprintf(calc->entry, sizeof(calc->entry), "%.10g", value);
    }
}

static void apply_pending(struct recon_calc *calc) {
    double rhs = current_value(calc);

    switch (calc->pending_op) {
    case '+':
        calc->accumulator += rhs;
        break;
    case '-':
        calc->accumulator -= rhs;
        break;
    case '*':
        calc->accumulator *= rhs;
        break;
    case '/':
        if (rhs == 0.0) {
            /* Say so rather than showing infinity. */
            calc->error = true;
            calc->pending_op = 0;
            snprintf(calc->entry, sizeof(calc->entry), "Cannot divide by zero");
            return;
        }
        calc->accumulator /= rhs;
        break;
    default:
        calc->accumulator = rhs;
        break;
    }

    show_value(calc, calc->accumulator);
}

static void clear_all(struct recon_calc *calc) {
    snprintf(calc->entry, sizeof(calc->entry), "0");
    calc->accumulator = 0.0;
    calc->pending_op = 0;
    calc->entering = false;
    calc->error = false;
}

static void append_digit(struct recon_calc *calc, char digit) {
    if (calc->error) {
        clear_all(calc);
    }

    if (!calc->entering) {
        /* Starting a fresh number replaces whatever result was showing. */
        calc->entry[0] = '\0';
        calc->entering = true;
    }

    size_t len = strlen(calc->entry);
    if (len >= DIGITS_MAX) {
        return;
    }
    if (len == 1 && calc->entry[0] == '0' && digit != '.') {
        len = 0; /* no leading zeros */
    }

    calc->entry[len] = digit;
    calc->entry[len + 1] = '\0';
}

static void append_dot(struct recon_calc *calc) {
    if (calc->error) {
        clear_all(calc);
    }
    if (!calc->entering) {
        snprintf(calc->entry, sizeof(calc->entry), "0");
        calc->entering = true;
    }
    if (strchr(calc->entry, '.') != NULL) {
        return; /* only one decimal point */
    }
    size_t len = strlen(calc->entry);
    if (len < DIGITS_MAX) {
        calc->entry[len] = '.';
        calc->entry[len + 1] = '\0';
    }
}

static void backspace(struct recon_calc *calc) {
    if (calc->error) {
        clear_all(calc);
        return;
    }
    size_t len = strlen(calc->entry);
    if (len > 1) {
        calc->entry[len - 1] = '\0';
    } else {
        snprintf(calc->entry, sizeof(calc->entry), "0");
        calc->entering = false;
    }
}

static void toggle_sign(struct recon_calc *calc) {
    if (calc->error) {
        return;
    }
    if (calc->entry[0] == '-') {
        memmove(calc->entry, calc->entry + 1, strlen(calc->entry));
    } else if (strcmp(calc->entry, "0") != 0) {
        size_t len = strlen(calc->entry);
        if (len < DIGITS_MAX) {
            memmove(calc->entry + 1, calc->entry, len + 1);
            calc->entry[0] = '-';
        }
    }
}

static void set_operator(struct recon_calc *calc, char op) {
    if (calc->error) {
        return;
    }

    if (calc->pending_op != 0 && calc->entering) {
        /* Chaining: 2 + 3 + shows 5 before taking the next operand. */
        apply_pending(calc);
        if (calc->error) {
            return;
        }
    } else {
        calc->accumulator = current_value(calc);
    }

    calc->pending_op = op;
    calc->entering = false;
}

static void equals(struct recon_calc *calc) {
    if (calc->error || calc->pending_op == 0) {
        return;
    }
    apply_pending(calc);
    calc->pending_op = 0;
    calc->entering = false;
}

/* --- Drawing --- */

static void calc_draw(void *user, struct recon_panel *panel,
        int x, int y, int w, int h) {
    struct recon_calc *calc = user;
    int ascent = recon_font_ascent(calc->font);

    recon_fill_rect(panel, x, y, w, h, COLOR_BG);

    /* Display: right-aligned, as a calculator reads. */
    int dx = x + PAD_PADDING;
    int dy = y + PAD_PADDING;
    int dw = w - PAD_PADDING * 2;
    recon_fill_rect(panel, dx, dy, dw, DISPLAY_HEIGHT, COLOR_DISPLAY);
    recon_draw_bevel(panel, dx, dy, dw, DISPLAY_HEIGHT, true);

    int text_w = recon_text_width(calc->font, calc->entry);
    int text_x = dx + dw - 10 - text_w;
    if (text_x < dx + 6) {
        text_x = dx + 6;
    }
    recon_draw_text(panel, calc->font, text_x,
        dy + (DISPLAY_HEIGHT + ascent) / 2 - 2, dw - 12,
        calc->entry, COLOR_DISPLAY_TEXT);

    /* A small marker showing an operation is waiting. */
    if (calc->pending_op != 0) {
        char op[2] = { calc->pending_op, '\0' };
        recon_draw_text(panel, calc->font, dx + 8,
            dy + (DISPLAY_HEIGHT + ascent) / 2 - 2, 20, op, COLOR_DISPLAY_TEXT);
    }

    /* Keypad. */
    int pad_top = dy + DISPLAY_HEIGHT + PAD_PADDING;
    int pad_h = h - (pad_top - y) - PAD_PADDING;
    calc->key_w = (dw - (COLS - 1) * KEY_GAP) / COLS;
    calc->key_h = (pad_h - (ROWS - 1) * KEY_GAP) / ROWS;

    for (int row = 0; row < ROWS; row++) {
        calc->key_y[row] = pad_top + row * (calc->key_h + KEY_GAP);

        for (int col = 0; col < COLS; col++) {
            const struct calc_key *key = &KEYS[row][col];
            calc->key_x[col] = dx + col * (calc->key_w + KEY_GAP);

            if (key->label[0] == '\0') {
                continue; /* the gap beside 0 */
            }

            int kx = calc->key_x[col];
            int ky = calc->key_y[row];

            recon_color fill = COLOR_KEY;
            recon_color text = COLOR_KEY_TEXT;
            if (key->kind == KEY_EQUALS) {
                fill = COLOR_KEY_ACCENT;
                text = COLOR_ACCENT_TEXT;
            } else if (key->kind != KEY_DIGIT && key->kind != KEY_DOT) {
                fill = COLOR_KEY_OP;
            }

            recon_fill_rect(panel, kx, ky, calc->key_w, calc->key_h, fill);
            recon_draw_bevel(panel, kx, ky, calc->key_w, calc->key_h, false);

            int label_w = recon_text_width(calc->font, key->label);
            recon_draw_text(panel, calc->font,
                kx + (calc->key_w - label_w) / 2,
                ky + (calc->key_h + ascent) / 2 - 2,
                calc->key_w - 4, key->label, text);

            /* Hit ids encode the position, so drawing and input cannot drift
             * out of step. */
            recon_hit_add(panel, kx, ky, calc->key_w, calc->key_h,
                RECON_APPWIN_HIT_USER + row * COLS + col);
        }
    }
}

/* --- Input --- */

static void press_key(struct recon_calc *calc, const struct calc_key *key) {
    switch (key->kind) {
    case KEY_DIGIT:
        if (key->value != 0) {
            append_digit(calc, key->value);
        }
        break;
    case KEY_DOT:
        append_dot(calc);
        break;
    case KEY_OP:
        set_operator(calc, key->value);
        break;
    case KEY_EQUALS:
        equals(calc);
        break;
    case KEY_CLEAR:
        clear_all(calc);
        break;
    case KEY_BACKSPACE:
        backspace(calc);
        break;
    case KEY_SIGN:
        toggle_sign(calc);
        break;
    }
}

static bool calc_click(void *user, uint32_t hit_id, int cx, int cy, bool pressed) {
    struct recon_calc *calc = user;
    if (!pressed || hit_id < RECON_APPWIN_HIT_USER) {
        return false;
    }

    int index = (int)(hit_id - RECON_APPWIN_HIT_USER);
    int row = index / COLS;
    int col = index % COLS;
    if (row < 0 || row >= ROWS || col < 0 || col >= COLS) {
        return false;
    }

    press_key(calc, &KEYS[row][col]);
    return true;
}

/*
 * Keyboard input, including the number pad, so the calculator can be driven
 * without touching the mouse.
 */
static bool calc_key_press(void *user, xkb_keysym_t sym, uint32_t modifiers) {
    struct recon_calc *calc = user;

    /* Number pad digits arrive as their own keysyms. */
    if (sym >= XKB_KEY_KP_0 && sym <= XKB_KEY_KP_9) {
        append_digit(calc, (char)('0' + (sym - XKB_KEY_KP_0)));
        return true;
    }
    if (sym >= XKB_KEY_0 && sym <= XKB_KEY_9) {
        append_digit(calc, (char)('0' + (sym - XKB_KEY_0)));
        return true;
    }

    switch (sym) {
    case XKB_KEY_period:
    case XKB_KEY_KP_Decimal:
    case XKB_KEY_comma:
        append_dot(calc);
        return true;

    case XKB_KEY_plus:
    case XKB_KEY_KP_Add:
        set_operator(calc, '+');
        return true;
    case XKB_KEY_minus:
    case XKB_KEY_KP_Subtract:
        set_operator(calc, '-');
        return true;
    case XKB_KEY_asterisk:
    case XKB_KEY_KP_Multiply:
        set_operator(calc, '*');
        return true;
    case XKB_KEY_slash:
    case XKB_KEY_KP_Divide:
        set_operator(calc, '/');
        return true;

    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
    case XKB_KEY_equal:
        equals(calc);
        return true;

    case XKB_KEY_BackSpace:
        backspace(calc);
        return true;

    case XKB_KEY_Escape:
    case XKB_KEY_Delete:
    case XKB_KEY_KP_Delete:
        clear_all(calc);
        return true;

    default:
        return false;
    }
}

static void calc_destroy(void *user) {
    free(user);
}

static const struct recon_appwin_impl CALC_IMPL = {
    .title = "Calculator",
    .icon = RECON_ICON_CALCULATOR,
    .default_width = 260,
    .default_height = 330,
    .min_width = 200,
    .min_height = 260,
    .draw = calc_draw,
    .click = calc_click,
    .key = calc_key_press,
    .destroy = calc_destroy,
};

struct recon_appwin *recon_calc_create(struct recon_server *server,
        struct recon_font *font) {
    struct recon_calc *calc = calloc(1, sizeof(*calc));
    if (calc == NULL) {
        return NULL;
    }
    calc->font = font;
    clear_all(calc);

    struct recon_appwin *win = recon_appwin_create(server, font, &CALC_IMPL, calc);
    if (win == NULL) {
        free(calc);
        return NULL;
    }
    return win;
}

/* --- The module --- */

/*
 * The calculator is an application ReconOS loads rather than one it contains.
 *
 * It is here as the first thing to go through the module path, and it is a
 * good first thing: entirely self-contained, useful, and nothing else depends
 * on it. If it fails to load the user loses a calculator, not a desktop.
 */
static bool calculator_load(void) {
    static const struct recon_app_registration APP = {
        .name = "Calculator",
        .icon = RECON_ICON_CALCULATOR,
        .create = recon_calc_create,
        .in_menu = true,
    };
    return recon_register_app(&APP);
}

static void calculator_unload(void) {
    recon_unregister_app("Calculator");
}

RECON_MODULE(
    .name = "Calculator",
    .version = "1.0",
    .description = "Arithmetic by mouse or keyboard",
    .load = calculator_load,
    .unload = calculator_unload,
);
