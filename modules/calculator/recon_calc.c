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
#include "recon_calc_modes.h"
#include "recon_clock.h"
#include "recon_module.h"
#include "recon_theme.h"
#include "recon_ui.h"

#define DISPLAY_HEIGHT 44
#define PAD_PADDING 6
#define KEY_GAP 4
#define COLS 4
#define ROWS 5

#define DIGITS_MAX 16

#define COLOR_BG THEME(WINDOW_FRAME)
#define COLOR_DISPLAY THEME(READOUT)
#define COLOR_DISPLAY_TEXT THEME(READOUT_TEXT)
#define COLOR_KEY THEME(BUTTON)
#define COLOR_KEY_OP THEME(BUTTON_ACTIVE)
#define COLOR_KEY_ACCENT THEME(ACCENT)
#define COLOR_KEY_TEXT THEME(SURFACE_TEXT)
#define COLOR_ACCENT_TEXT THEME(ACCENT_TEXT)

enum key_kind {
    KEY_NONE,        /* a hole in the grid */
    KEY_DIGIT,
    KEY_OP,
    KEY_EQUALS,
    KEY_CLEAR,
    KEY_BACKSPACE,
    KEY_SIGN,
    KEY_DOT,

    /* One number in, one number out: sin, log, the reciprocal. `value`
     * says which. */
    KEY_FUNC,
    /* A number that is simply itself: pi, e. */
    KEY_CONST,
    /* Programmer mode. A base to read and write in, and the bitwise
     * operations, which are operators but on integers. */
    KEY_BASE,
    KEY_BITOP,
};

/*
 * Which function a KEY_FUNC key is.
 *
 * A letter rather than an index so the table below reads as what it does.
 * Trigonometry is in degrees, because a calculator with buttons is being used
 * by somebody who thinks in degrees; radians are what the Convert mode's
 * Angle family is for.
 */
enum calc_func {
    FUNC_SIN = 's', FUNC_COS = 'c', FUNC_TAN = 't',
    FUNC_ASIN = 'S', FUNC_ACOS = 'C', FUNC_ATAN = 'T',
    FUNC_LOG = 'l', FUNC_LN = 'n', FUNC_SQRT = 'r',
    FUNC_SQUARE = 'q', FUNC_RECIPROCAL = 'i', FUNC_FACTORIAL = '!',
    FUNC_EXP = 'e', FUNC_TEN_POW = 'X', FUNC_PERCENT = '%',
};

struct calc_key {
    const char *label;
    enum key_kind kind;
    char value; /* digit character, or operator symbol */
};

/*
 * A mode's keypad: however many rows and columns it wants.
 *
 * Each mode has its own grid rather than one grid with keys switched on and
 * off, because a scientific keypad is not a standard one with extras bolted
 * to the side -- the digits move, and a layout that kept them still would
 * waste half the window on a mode nobody is in.
 */
struct calc_layout {
    int rows, cols;
    const struct calc_key *keys;
};

/*
 * Laid out as a keypad rather than in rows of related functions, because the
 * digits should sit where a number pad puts them.
 */
static const struct calc_key STANDARD_KEYS[] = {
    {"C", KEY_CLEAR, 0},   {"+/-", KEY_SIGN, 0}, {"<-", KEY_BACKSPACE, 0}, {"/", KEY_OP, '/'},
    {"7", KEY_DIGIT, '7'}, {"8", KEY_DIGIT, '8'}, {"9", KEY_DIGIT, '9'},   {"*", KEY_OP, '*'},
    {"4", KEY_DIGIT, '4'}, {"5", KEY_DIGIT, '5'}, {"6", KEY_DIGIT, '6'},   {"-", KEY_OP, '-'},
    {"1", KEY_DIGIT, '1'}, {"2", KEY_DIGIT, '2'}, {"3", KEY_DIGIT, '3'},   {"+", KEY_OP, '+'},
    {"0", KEY_DIGIT, '0'}, {".", KEY_DOT, 0},     {"%", KEY_FUNC, FUNC_PERCENT}, {"=", KEY_EQUALS, 0},
};

/*
 * Six columns, because the functions want their own two and the digits keep
 * the four they had. Inverse trigonometry is on its own keys rather than
 * behind a shift, which would be a mode inside a mode.
 */
static const struct calc_key SCIENTIFIC_KEYS[] = {
    {"sin", KEY_FUNC, FUNC_SIN}, {"cos", KEY_FUNC, FUNC_COS}, {"tan", KEY_FUNC, FUNC_TAN},
        {"C", KEY_CLEAR, 0},   {"<-", KEY_BACKSPACE, 0}, {"/", KEY_OP, '/'},
    {"asin", KEY_FUNC, FUNC_ASIN}, {"acos", KEY_FUNC, FUNC_ACOS}, {"atan", KEY_FUNC, FUNC_ATAN},
        {"7", KEY_DIGIT, '7'}, {"8", KEY_DIGIT, '8'}, {"9", KEY_DIGIT, '9'},
    {"log", KEY_FUNC, FUNC_LOG}, {"ln", KEY_FUNC, FUNC_LN}, {"10^x", KEY_FUNC, FUNC_TEN_POW},
        {"4", KEY_DIGIT, '4'}, {"5", KEY_DIGIT, '5'}, {"6", KEY_DIGIT, '6'},
    {"sqrt", KEY_FUNC, FUNC_SQRT}, {"x^2", KEY_FUNC, FUNC_SQUARE}, {"x^y", KEY_OP, '^'},
        {"1", KEY_DIGIT, '1'}, {"2", KEY_DIGIT, '2'}, {"3", KEY_DIGIT, '3'},
    {"1/x", KEY_FUNC, FUNC_RECIPROCAL}, {"n!", KEY_FUNC, FUNC_FACTORIAL}, {"e^x", KEY_FUNC, FUNC_EXP},
        {"0", KEY_DIGIT, '0'}, {".", KEY_DOT, 0}, {"*", KEY_OP, '*'},
    {"pi", KEY_CONST, 'p'}, {"e", KEY_CONST, 'e'}, {"+/-", KEY_SIGN, 0},
        {"-", KEY_OP, '-'}, {"+", KEY_OP, '+'}, {"=", KEY_EQUALS, 0},
};

/*
 * Programmer. A to F are always present rather than greyed outside
 * hexadecimal: a key that is sometimes a key is harder to learn than one that
 * simply refuses a digit the current base has no room for, and the display
 * says which base it is in.
 */
static const struct calc_key PROGRAMMER_KEYS[] = {
    {"HEX", KEY_BASE, 16}, {"DEC", KEY_BASE, 10}, {"OCT", KEY_BASE, 8}, {"BIN", KEY_BASE, 2},
    {"AND", KEY_BITOP, '&'}, {"OR", KEY_BITOP, '|'}, {"XOR", KEY_BITOP, '^'}, {"NOT", KEY_BITOP, '~'},
    {"<<", KEY_BITOP, '<'}, {">>", KEY_BITOP, '>'}, {"C", KEY_CLEAR, 0}, {"<-", KEY_BACKSPACE, 0},
    {"A", KEY_DIGIT, 'A'}, {"B", KEY_DIGIT, 'B'}, {"C", KEY_DIGIT, 'C'}, {"D", KEY_DIGIT, 'D'},
    {"E", KEY_DIGIT, 'E'}, {"F", KEY_DIGIT, 'F'}, {"/", KEY_OP, '/'}, {"*", KEY_OP, '*'},
    {"7", KEY_DIGIT, '7'}, {"8", KEY_DIGIT, '8'}, {"9", KEY_DIGIT, '9'}, {"-", KEY_OP, '-'},
    {"4", KEY_DIGIT, '4'}, {"5", KEY_DIGIT, '5'}, {"6", KEY_DIGIT, '6'}, {"+", KEY_OP, '+'},
    {"1", KEY_DIGIT, '1'}, {"2", KEY_DIGIT, '2'}, {"3", KEY_DIGIT, '3'}, {"=", KEY_EQUALS, 0},
    {"0", KEY_DIGIT, '0'}, {"", KEY_NONE, 0}, {"", KEY_NONE, 0}, {"", KEY_NONE, 0},
};

static const struct calc_layout LAYOUTS[CALC_MODE_COUNT] = {
    [CALC_STANDARD]   = { 5, 4, STANDARD_KEYS },
    [CALC_SCIENTIFIC] = { 6, 6, SCIENTIFIC_KEYS },
    [CALC_PROGRAMMER] = { 9, 4, PROGRAMMER_KEYS },
    /* Date and Convert are not keypads. */
    [CALC_DATE]       = { 0, 0, NULL },
    [CALC_CONVERT]    = { 0, 0, NULL },
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

    /* Which mode is showing. */
    enum calc_mode mode;

    /*
     * Programmer mode works in whole numbers, not doubles.
     *
     * A calculator that shows you a bit pattern has to be exact about every
     * one of those bits, and a double is exact only to 53 of them -- so a
     * 64-bit mask would come back subtly wrong at the top end, which is the
     * end somebody using this mode cares about.
     */
    long long whole;
    long long whole_accumulator;
    char whole_op;
    int base;

    /* Date mode: two dates and which of them is being typed into. */
    int from_year, from_month, from_day;
    int to_year, to_month, to_day;
    int date_field;          /* 0 = the first date, 1 = the second */

    /* Convert mode: which family, and which two units within it. */
    int category;
    int unit_from, unit_to;
    int convert_scroll;

    /* Remembered so the layout and the hit regions agree. */
    int key_x[8], key_y[10], key_w, key_h;
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

/*
 * Everything a KEY_FUNC key does.
 *
 * Each one that can be asked an impossible question says so rather than
 * showing a special floating-point value: "nan" on a calculator's display is
 * the calculator failing to answer *and* failing to say it failed.
 */
static void apply_function(struct recon_calc *calc, char which) {
    double v = current_value(calc);
    double result = 0.0;

    /* Degrees, because somebody pressing a button marked "sin" is thinking in
     * degrees. Radians are what the Convert mode's Angle family is for. */
    const double TO_RADIANS = 3.14159265358979323846 / 180.0;
    const double TO_DEGREES = 180.0 / 3.14159265358979323846;

    switch (which) {
    case FUNC_SIN:  result = sin(v * TO_RADIANS); break;
    case FUNC_COS:  result = cos(v * TO_RADIANS); break;
    case FUNC_TAN:
        /* Tangent of 90 is not a large number, it is undefined, and the
         * floating-point answer is merely large enough to look like one. */
        if (fmod(fabs(v) - 90.0, 180.0) == 0.0) {
            calc->error = true;
            snprintf(calc->entry, sizeof(calc->entry), "No tangent there");
            return;
        }
        result = tan(v * TO_RADIANS);
        break;

    case FUNC_ASIN:
    case FUNC_ACOS:
        if (v < -1.0 || v > 1.0) {
            calc->error = true;
            snprintf(calc->entry, sizeof(calc->entry),
                "Only between -1 and 1");
            return;
        }
        result = (which == FUNC_ASIN ? asin(v) : acos(v)) * TO_DEGREES;
        break;
    case FUNC_ATAN: result = atan(v) * TO_DEGREES; break;

    case FUNC_LOG:
    case FUNC_LN:
        if (v <= 0.0) {
            calc->error = true;
            snprintf(calc->entry, sizeof(calc->entry),
                "Only above zero");
            return;
        }
        result = (which == FUNC_LOG) ? log10(v) : log(v);
        break;

    case FUNC_SQRT:
        if (v < 0.0) {
            calc->error = true;
            snprintf(calc->entry, sizeof(calc->entry),
                "No square root of a negative");
            return;
        }
        result = sqrt(v);
        break;

    case FUNC_SQUARE: result = v * v; break;
    case FUNC_EXP: result = exp(v); break;
    case FUNC_TEN_POW: result = pow(10.0, v); break;

    case FUNC_RECIPROCAL:
        if (v == 0.0) {
            calc->error = true;
            snprintf(calc->entry, sizeof(calc->entry),
                "Cannot divide by zero");
            return;
        }
        result = 1.0 / v;
        break;

    case FUNC_FACTORIAL: {
        if (v < 0.0 || v != (double)(long long)v) {
            calc->error = true;
            snprintf(calc->entry, sizeof(calc->entry),
                "Whole numbers from zero up");
            return;
        }
        if (v > 170.0) {
            /* 171! is larger than a double holds, and the answer would come
             * back as infinity dressed as a number. */
            calc->error = true;
            snprintf(calc->entry, sizeof(calc->entry), "Too large to hold");
            return;
        }
        result = 1.0;
        for (long long i = 2; i <= (long long)v; i++) {
            result *= (double)i;
        }
        break;
    }

    case FUNC_PERCENT:
        /*
         * A percentage *of the running total*, which is what somebody means
         * by "200 + 10 %". Taken on its own it is simply a hundredth.
         */
        result = (calc->pending_op != 0)
            ? calc->accumulator * v / 100.0 : v / 100.0;
        break;

    default:
        return;
    }

    show_value(calc, result);
    calc->entering = false;
}

/* --- Programmer --- */

/* The entry, read in the base showing. */
static long long whole_value(struct recon_calc *calc) {
    return strtoll(calc->entry, NULL, calc->base > 0 ? calc->base : 10);
}

/* Write a whole number out in the base showing. */
static void show_whole(struct recon_calc *calc, long long value) {
    calc->whole = value;

    if (calc->base == 10) {
        snprintf(calc->entry, sizeof(calc->entry), "%lld", value);
        return;
    }
    if (calc->base == 16) {
        snprintf(calc->entry, sizeof(calc->entry), "%llX",
            (unsigned long long)value);
        return;
    }
    if (calc->base == 8) {
        snprintf(calc->entry, sizeof(calc->entry), "%llo",
            (unsigned long long)value);
        return;
    }

    /* Binary, which no printf writes. Built backwards and reversed, with a
     * lone zero for zero because an empty display is not a number. */
    unsigned long long bits = (unsigned long long)value;
    char out[72];
    int at = 0;
    if (bits == 0) {
        out[at++] = '0';
    }
    while (bits != 0 && at < (int)sizeof(out) - 1) {
        out[at++] = (char)('0' + (bits & 1));
        bits >>= 1;
    }
    for (int i = 0; i < at; i++) {
        calc->entry[i] = out[at - 1 - i];
    }
    calc->entry[at] = '\0';
}

static void apply_whole_pending(struct recon_calc *calc) {
    long long rhs = whole_value(calc);
    long long acc = calc->whole_accumulator;

    switch (calc->whole_op) {
    case '+': acc += rhs; break;
    case '-': acc -= rhs; break;
    case '*': acc *= rhs; break;
    case '/':
        if (rhs == 0) {
            calc->error = true;
            calc->whole_op = 0;
            snprintf(calc->entry, sizeof(calc->entry),
                "Cannot divide by zero");
            return;
        }
        acc /= rhs;
        break;
    case '&': acc &= rhs; break;
    case '|': acc |= rhs; break;
    case '^': acc ^= rhs; break;
    case '<':
    case '>':
        /*
         * A shift of 64 or more is undefined in C, and what a processor
         * actually does with it varies -- x86 masks the count to six bits, so
         * "shift by 64" silently becomes "shift by 0". Refused rather than
         * left to the hardware's opinion.
         */
        if (rhs < 0 || rhs > 63) {
            calc->error = true;
            calc->whole_op = 0;
            snprintf(calc->entry, sizeof(calc->entry), "Shift 0 to 63");
            return;
        }
        acc = (calc->whole_op == '<') ? (long long)((unsigned long long)acc << rhs)
                                      : (long long)((unsigned long long)acc >> rhs);
        break;
    default:
        acc = rhs;
        break;
    }

    calc->whole_accumulator = acc;
    show_whole(calc, acc);
}

static void clear_all(struct recon_calc *calc) {
    snprintf(calc->entry, sizeof(calc->entry), "0");
    calc->accumulator = 0.0;
    calc->pending_op = 0;
    calc->entering = false;
    calc->error = false;
    calc->whole = 0;
    calc->whole_accumulator = 0;
    calc->whole_op = 0;
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

#define TAB_HEIGHT 24
#define HIT_TAB_BASE (RECON_APPWIN_HIT_USER + 500)
#define HIT_CATEGORY_BASE (RECON_APPWIN_HIT_USER + 600)
#define HIT_FROM_BASE (RECON_APPWIN_HIT_USER + 700)
#define HIT_TO_BASE (RECON_APPWIN_HIT_USER + 800)
#define HIT_DATE_FIELD (RECON_APPWIN_HIT_USER + 900)

/* One of `n` values converted from `from` to `to` within a category. */
static double convert(const struct calc_category *cat, int from, int to,
        double value) {
    if (cat->has_offset) {
        /*
         * Temperature, which is the one family where a factor is not enough.
         * Everything goes through Celsius rather than through a table of
         * pairs: three units is six conversions, and six is where people
         * start getting one of them backwards.
         */
        double celsius = value;
        if (from == 1) {
            celsius = (value - 32.0) * 5.0 / 9.0;
        } else if (from == 2) {
            celsius = value - 273.15;
        }

        if (to == 1) {
            return celsius * 9.0 / 5.0 + 32.0;
        }
        if (to == 2) {
            return celsius + 273.15;
        }
        return celsius;
    }

    if (cat->units[to].factor == 0.0) {
        return 0.0;
    }
    return value * cat->units[from].factor / cat->units[to].factor;
}

/* The tabs across the top. Returns the y to carry on drawing from. */
static int draw_tabs(struct recon_calc *calc, struct recon_panel *panel,
        int x, int y, int w) {
    int ascent = recon_font_ascent(calc->font);
    int tab_w = w / CALC_MODE_COUNT;

    for (int i = 0; i < CALC_MODE_COUNT; i++) {
        int tx = x + i * tab_w;
        bool on = calc->mode == (enum calc_mode)i;

        recon_fill_rect(panel, tx, y, tab_w - 1, TAB_HEIGHT,
            on ? COLOR_KEY_ACCENT : COLOR_KEY);
        recon_draw_bevel(panel, tx, y, tab_w - 1, TAB_HEIGHT, on);

        int label_w = recon_text_width(calc->font, CALC_MODE_NAMES[i]);
        recon_draw_text(panel, calc->font, tx + (tab_w - label_w) / 2,
            y + (TAB_HEIGHT + ascent) / 2 - 2, tab_w - 4, CALC_MODE_NAMES[i],
            on ? COLOR_ACCENT_TEXT : COLOR_KEY_TEXT);

        recon_hit_add(panel, tx, y, tab_w - 1, TAB_HEIGHT,
            HIT_TAB_BASE + (uint32_t)i);
    }

    return y + TAB_HEIGHT + PAD_PADDING;
}

/* The readout. In programmer mode it carries the same number in every base,
 * because seeing them together is the entire point of the mode. */
static int draw_display(struct recon_calc *calc, struct recon_panel *panel,
        int x, int y, int w) {
    int ascent = recon_font_ascent(calc->font);
    int line = recon_font_line_height(calc->font);
    int height = DISPLAY_HEIGHT;

    if (calc->mode == CALC_PROGRAMMER) {
        height = DISPLAY_HEIGHT + line * 3 + 6;
    }

    recon_fill_rect(panel, x, y, w, height, COLOR_DISPLAY);
    recon_draw_bevel(panel, x, y, w, height, true);

    int text_w = recon_text_width(calc->font, calc->entry);
    int text_x = x + w - 10 - text_w;
    if (text_x < x + 6) {
        text_x = x + 6;
    }
    recon_draw_text(panel, calc->font, text_x,
        y + (DISPLAY_HEIGHT + ascent) / 2 - 2, w - 12, calc->entry,
        COLOR_DISPLAY_TEXT);

    char pending = calc->mode == CALC_PROGRAMMER ? calc->whole_op
        : calc->pending_op;
    if (pending != 0) {
        char op[2] = { pending, '\0' };
        recon_draw_text(panel, calc->font, x + 8,
            y + (DISPLAY_HEIGHT + ascent) / 2 - 2, 20, op,
            COLOR_DISPLAY_TEXT);
    }

    if (calc->mode == CALC_PROGRAMMER) {
        long long v = calc->error ? 0 : whole_value(calc);
        int by = y + DISPLAY_HEIGHT;

        static const struct { const char *name; int base; } SHOW[] = {
            { "HEX", 16 }, { "DEC", 10 }, { "OCT", 8 },
        };

        for (int i = 0; i < 3; i++) {
            char out[80];
            if (SHOW[i].base == 16) {
                snprintf(out, sizeof(out), "%llX", (unsigned long long)v);
            } else if (SHOW[i].base == 10) {
                snprintf(out, sizeof(out), "%lld", v);
            } else {
                snprintf(out, sizeof(out), "%llo", (unsigned long long)v);
            }

            /*
             * Every label in the readout's own ink. The base showing is
             * marked by the strip beside it rather than by colour: these sit
             * on the dark readout, and the surface's text colour -- which is
             * for the light chrome -- came out dark on dark and left two of
             * the three rows unlabelled.
             */
            if (calc->base == SHOW[i].base) {
                recon_fill_rect(panel, x + 3, by + 1, 3, line - 2,
                    COLOR_KEY_ACCENT);
            }
            recon_draw_text(panel, calc->font, x + 10, by + ascent + 1, 40,
                SHOW[i].name, COLOR_DISPLAY_TEXT);
            int ow = recon_text_width(calc->font, out);
            recon_draw_text(panel, calc->font, x + w - 10 - ow,
                by + ascent + 1, w - 60, out, COLOR_DISPLAY_TEXT);
            by += line;
        }
    }

    return y + height + PAD_PADDING;
}

/* --- Date --- */

static void draw_date_mode(struct recon_calc *calc, struct recon_panel *panel,
        int x, int y, int w, int bottom) {
    int ascent = recon_font_ascent(calc->font);
    int line = recon_font_line_height(calc->font);

    recon_draw_text(panel, calc->font, x, y + ascent, w,
        "How far apart two dates are.", COLOR_KEY_TEXT);
    y += line + 8;

    struct {
        const char *label;
        int year, month, day;
    } rows[2] = {
        { "From", calc->from_year, calc->from_month, calc->from_day },
        { "To",   calc->to_year,   calc->to_month,   calc->to_day },
    };

    for (int i = 0; i < 2; i++) {
        bool on = calc->date_field == i;

        char shown[64];
        snprintf(shown, sizeof(shown), "%04d-%02d-%02d", rows[i].year,
            rows[i].month, rows[i].day);

        recon_draw_text(panel, calc->font, x, y + ascent, 50, rows[i].label,
            COLOR_KEY_TEXT);
        recon_fill_rect(panel, x + 54, y - 2, w - 54, line + 6,
            on ? COLOR_KEY_ACCENT : COLOR_DISPLAY);
        recon_draw_bevel(panel, x + 54, y - 2, w - 54, line + 6, true);
        recon_draw_text(panel, calc->font, x + 62, y + ascent, w - 70, shown,
            on ? COLOR_ACCENT_TEXT : COLOR_DISPLAY_TEXT);

        recon_hit_add(panel, x + 54, y - 2, w - 54, line + 6,
            HIT_DATE_FIELD + (uint32_t)i);
        y += line + 12;
    }

    y += 4;

    /*
     * The difference, both ways round, because "how many days until" and
     * "how many days since" are the same subtraction and somebody has one of
     * them in mind.
     */
    int64_t from = recon_clock_epoch_of(calc->from_year, calc->from_month,
        calc->from_day);
    int64_t to = recon_clock_epoch_of(calc->to_year, calc->to_month,
        calc->to_day);
    long long days = (long long)((to - from) / 86400);

    char answer[128];
    snprintf(answer, sizeof(answer), "%lld day%s", days < 0 ? -days : days,
        (days == 1 || days == -1) ? "" : "s");
    recon_draw_text(panel, calc->font, x, y + ascent, w, answer,
        COLOR_KEY_TEXT);
    y += line + 2;

    long long weeks = (days < 0 ? -days : days) / 7;
    long long spare = (days < 0 ? -days : days) % 7;
    snprintf(answer, sizeof(answer), "%lld week%s and %lld day%s", weeks,
        weeks == 1 ? "" : "s", spare, spare == 1 ? "" : "s");
    recon_draw_text(panel, calc->font, x, y + ascent, w, answer,
        COLOR_KEY_TEXT);
    y += line + 8;

    if (y + line <= bottom) {
        recon_draw_text(panel, calc->font, x, y + ascent, w,
            "Click a date, then type digits. Arrows step a day.",
            COLOR_KEY_TEXT);
    }
}

/* --- Convert --- */

static void draw_convert_mode(struct recon_calc *calc,
        struct recon_panel *panel, int x, int y, int w, int bottom) {
    int ascent = recon_font_ascent(calc->font);
    int line = recon_font_line_height(calc->font);

    const struct calc_category *cat = &CALC_CATEGORIES[calc->category];

    /* The families, across the top and wrapped. */
    int cx = x;
    int cy = y;
    for (int i = 0; i < CALC_CATEGORY_COUNT; i++) {
        int cw = recon_text_width(calc->font, CALC_CATEGORIES[i].name) + 14;
        if (cx + cw > x + w) {
            cx = x;
            cy += line + 6;
        }

        bool on = i == calc->category;
        recon_fill_rect(panel, cx, cy, cw - 2, line + 4,
            on ? COLOR_KEY_ACCENT : COLOR_KEY);
        recon_draw_bevel(panel, cx, cy, cw - 2, line + 4, on);
        recon_draw_text(panel, calc->font, cx + 6, cy + ascent + 2, cw - 8,
            CALC_CATEGORIES[i].name,
            on ? COLOR_ACCENT_TEXT : COLOR_KEY_TEXT);
        recon_hit_add(panel, cx, cy, cw - 2, line + 4,
            HIT_CATEGORY_BASE + (uint32_t)i);
        cx += cw;
    }
    y = cy + line + 12;

    /* The two unit lists, side by side, because converting is a question
     * about a pair and a pair should be visible at once. */
    int half = (w - 10) / 2;
    int list_top = y;
    int rows = (bottom - y - line * 3) / (line + 2);
    if (rows < 1) {
        rows = 1;
    }
    if (rows > cat->count) {
        rows = cat->count;
    }

    for (int side = 0; side < 2; side++) {
        int sx = x + side * (half + 10);
        int chosen = side == 0 ? calc->unit_from : calc->unit_to;

        recon_draw_text(panel, calc->font, sx, list_top + ascent, half,
            side == 0 ? "From" : "To", COLOR_KEY_TEXT);

        int ry = list_top + line + 2;
        for (int i = 0; i < rows; i++) {
            int at = i;
            if (at >= cat->count) {
                break;
            }
            bool on = at == chosen;
            recon_fill_rect(panel, sx, ry, half, line + 2,
                on ? COLOR_KEY_ACCENT : COLOR_KEY);
            recon_draw_text(panel, calc->font, sx + 5, ry + ascent + 1,
                half - 8, cat->units[at].name,
                on ? COLOR_ACCENT_TEXT : COLOR_KEY_TEXT);
            recon_hit_add(panel, sx, ry, half, line + 2,
                (side == 0 ? HIT_FROM_BASE : HIT_TO_BASE) + (uint32_t)at);
            ry += line + 2;
        }
    }

    /* The answer, under both lists. */
    int ay = list_top + line + 2 + rows * (line + 2) + 8;
    double value = current_value(calc);
    double out = convert(cat, calc->unit_from, calc->unit_to, value);

    char answer[160];
    snprintf(answer, sizeof(answer), "%.10g %s  =  %.10g %s", value,
        cat->units[calc->unit_from].name, out,
        cat->units[calc->unit_to].name);
    recon_draw_text(panel, calc->font, x, ay + ascent, w, answer,
        COLOR_KEY_TEXT);
    ay += line + 2;

    if (ay + line <= bottom) {
        recon_draw_text(panel, calc->font, x, ay + ascent, w,
            "Type a number. Currency is absent on purpose -- a rate is a "
            "fact about today.", COLOR_KEY_TEXT);
    }
}

static void calc_draw(void *user, struct recon_panel *panel,
        int x, int y, int w, int h) {
    struct recon_calc *calc = user;
    int ascent = recon_font_ascent(calc->font);

    recon_fill_rect(panel, x, y, w, h, COLOR_BG);

    int dx = x + PAD_PADDING;
    int dw = w - PAD_PADDING * 2;
    int cy = draw_tabs(calc, panel, dx, y + PAD_PADDING, dw);

    cy = draw_display(calc, panel, dx, cy, dw);

    if (calc->mode == CALC_DATE) {
        draw_date_mode(calc, panel, dx, cy, dw, y + h - PAD_PADDING);
        return;
    }
    if (calc->mode == CALC_CONVERT) {
        draw_convert_mode(calc, panel, dx, cy, dw, y + h - PAD_PADDING);
        return;
    }

    const struct calc_layout *layout = &LAYOUTS[calc->mode];
    if (layout->keys == NULL || layout->rows == 0) {
        return;
    }

    int pad_h = h - (cy - y) - PAD_PADDING;
    calc->key_w = (dw - (layout->cols - 1) * KEY_GAP) / layout->cols;
    calc->key_h = (pad_h - (layout->rows - 1) * KEY_GAP) / layout->rows;
    if (calc->key_h < 1) {
        calc->key_h = 1;
    }

    for (int row = 0; row < layout->rows; row++) {
        int ky = cy + row * (calc->key_h + KEY_GAP);

        for (int col = 0; col < layout->cols; col++) {
            const struct calc_key *key =
                &layout->keys[row * layout->cols + col];
            if (key->kind == KEY_NONE || key->label[0] == '\0') {
                continue;
            }

            int kx = dx + col * (calc->key_w + KEY_GAP);

            recon_color fill = COLOR_KEY;
            recon_color text = COLOR_KEY_TEXT;
            if (key->kind == KEY_EQUALS) {
                fill = COLOR_KEY_ACCENT;
                text = COLOR_ACCENT_TEXT;
            } else if (key->kind == KEY_BASE &&
                    calc->base == (int)key->value) {
                /* The base showing is lit, because it is a state and not an
                 * action -- pressing it again does nothing. */
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
                RECON_APPWIN_HIT_USER +
                (uint32_t)(row * layout->cols + col));
        }
    }
}

/* --- Input --- */

/* Whether a digit exists in the base showing. */
static bool digit_fits(struct recon_calc *calc, char digit) {
    int value;
    if (digit >= '0' && digit <= '9') {
        value = digit - '0';
    } else if (digit >= 'A' && digit <= 'F') {
        value = 10 + (digit - 'A');
    } else {
        return false;
    }
    return value < calc->base;
}

static void press_key(struct recon_calc *calc, const struct calc_key *key) {
    bool programmer = calc->mode == CALC_PROGRAMMER;

    switch (key->kind) {
    case KEY_NONE:
        break;

    case KEY_DIGIT:
        if (key->value == 0) {
            break;
        }
        /*
         * In programmer mode a digit has to exist in the base showing. B is
         * not a number in decimal, and accepting it would produce an entry
         * that reads back as something else entirely.
         */
        if (programmer && !digit_fits(calc, key->value)) {
            break;
        }
        append_digit(calc, key->value);
        break;

    case KEY_DOT:
        /* No fractions in programmer mode: it is about bit patterns, and
         * half a bit is not one. */
        if (!programmer) {
            append_dot(calc);
        }
        break;

    case KEY_OP:
        if (programmer) {
            if (calc->whole_op != 0 && calc->entering) {
                apply_whole_pending(calc);
            } else {
                calc->whole_accumulator = whole_value(calc);
            }
            calc->whole_op = key->value;
            calc->entering = false;
        } else {
            set_operator(calc, key->value);
        }
        break;

    case KEY_BITOP:
        if (!programmer) {
            break;
        }
        if (key->value == '~') {
            /* One number in, one out -- it is a function wearing an
             * operator's coat, so it acts at once rather than waiting for a
             * right-hand side. */
            show_whole(calc, ~whole_value(calc));
            calc->entering = false;
            break;
        }
        if (calc->whole_op != 0 && calc->entering) {
            apply_whole_pending(calc);
        } else {
            calc->whole_accumulator = whole_value(calc);
        }
        calc->whole_op = key->value;
        calc->entering = false;
        break;

    case KEY_BASE: {
        /* The number stays; only the way it is written changes. */
        long long held = calc->error ? 0 : whole_value(calc);
        calc->base = key->value;
        calc->error = false;
        show_whole(calc, held);
        calc->entering = false;
        break;
    }

    case KEY_FUNC:
        if (!programmer) {
            apply_function(calc, key->value);
        }
        break;

    case KEY_CONST:
        if (programmer) {
            break;
        }
        show_value(calc, key->value == 'p' ? 3.14159265358979323846
                                           : 2.71828182845904523536);
        calc->entering = false;
        break;

    case KEY_EQUALS:
        if (programmer) {
            if (calc->whole_op != 0) {
                apply_whole_pending(calc);
                calc->whole_op = 0;
                calc->entering = false;
            }
        } else {
            equals(calc);
        }
        break;

    case KEY_CLEAR:
        clear_all(calc);
        if (programmer) {
            show_whole(calc, 0);
        }
        break;

    case KEY_BACKSPACE:
        backspace(calc);
        break;

    case KEY_SIGN:
        if (programmer) {
            show_whole(calc, -whole_value(calc));
        } else {
            toggle_sign(calc);
        }
        break;
    }
}

/* Move to a mode, leaving the entry in a state that mode can read. */
static void set_mode(struct recon_calc *calc, enum calc_mode mode) {
    if (mode == calc->mode) {
        return;
    }

    /*
     * The number does not survive the move.
     *
     * It could be made to -- a decimal is a whole number often enough -- but
     * "often enough" is the problem: 2.5 arriving in programmer mode as 2 is
     * a wrong answer that looks like a right one. Clearing says plainly that
     * this is a different calculation.
     */
    calc->mode = mode;
    clear_all(calc);

    if (mode == CALC_PROGRAMMER) {
        show_whole(calc, 0);
    }
}

static void date_step(struct recon_calc *calc, int days) {
    int *y = calc->date_field == 0 ? &calc->from_year : &calc->to_year;
    int *m = calc->date_field == 0 ? &calc->from_month : &calc->to_month;
    int *d = calc->date_field == 0 ? &calc->from_day : &calc->to_day;

    /* Through the epoch and back, so stepping off the end of a month lands
     * on the first of the next one rather than on the 32nd. */
    int64_t at = recon_clock_epoch_of(*y, *m, *d) + (int64_t)days * 86400;

    struct recon_clock_time t;
    recon_clock_break_up(at + 12 * 3600, &t);
    *y = t.year;
    *m = t.month;
    *d = t.day;
}

static bool calc_click(void *user, uint32_t hit_id, int cx, int cy, bool pressed) {
    struct recon_calc *calc = user;
    (void)cx;
    (void)cy;

    if (!pressed || hit_id < RECON_APPWIN_HIT_USER) {
        return false;
    }

    if (hit_id >= HIT_DATE_FIELD) {
        calc->date_field = (int)(hit_id - HIT_DATE_FIELD);
        return true;
    }
    if (hit_id >= HIT_TO_BASE) {
        calc->unit_to = (int)(hit_id - HIT_TO_BASE);
        return true;
    }
    if (hit_id >= HIT_FROM_BASE) {
        calc->unit_from = (int)(hit_id - HIT_FROM_BASE);
        return true;
    }
    if (hit_id >= HIT_CATEGORY_BASE) {
        int at = (int)(hit_id - HIT_CATEGORY_BASE);
        if (at >= 0 && at < CALC_CATEGORY_COUNT) {
            calc->category = at;
            /* A different family has different units, and the old indices
             * would name whichever happened to sit in those positions. */
            calc->unit_from = 0;
            calc->unit_to = CALC_CATEGORIES[at].count > 1 ? 1 : 0;
        }
        return true;
    }
    if (hit_id >= HIT_TAB_BASE) {
        int at = (int)(hit_id - HIT_TAB_BASE);
        if (at >= 0 && at < CALC_MODE_COUNT) {
            set_mode(calc, (enum calc_mode)at);
        }
        return true;
    }

    const struct calc_layout *layout = &LAYOUTS[calc->mode];
    if (layout->keys == NULL) {
        return false;
    }

    int index = (int)(hit_id - RECON_APPWIN_HIT_USER);
    if (index < 0 || index >= layout->rows * layout->cols) {
        return false;
    }

    press_key(calc, &layout->keys[index]);
    return true;
}

/*
 * Keyboard input, including the number pad, so the calculator can be driven
 * without touching the mouse.
 */
static bool calc_key_press(void *user, xkb_keysym_t sym, uint32_t modifiers) {
    struct recon_calc *calc = user;

    /* Date mode steps a day at a time rather than typing arithmetic. */
    if (calc->mode == CALC_DATE) {
        switch (sym) {
        case XKB_KEY_Left:  date_step(calc, -1); return true;
        case XKB_KEY_Right: date_step(calc, 1); return true;
        case XKB_KEY_Down:  date_step(calc, -7); return true;
        case XKB_KEY_Up:    date_step(calc, 7); return true;
        case XKB_KEY_Tab:
            calc->date_field = calc->date_field == 0 ? 1 : 0;
            return true;
        default:
            return false;
        }
    }

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
    /*
     * Wide enough for five mode names and a six-column scientific keypad.
     * It was 260, which fitted the four-function keypad exactly and truncated
     * every tab label the moment there were tabs.
     */
    .default_width = 430,
    .default_height = 440,
    .min_width = 380,
    .min_height = 320,
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
    calc->base = 10;
    calc->unit_to = 1;

    /* Both dates start at today, so the difference starts at zero and moves
     * from somewhere real rather than from the first of January 1970. */
    struct recon_clock_time now;
    recon_clock_now(&now);
    calc->from_year = calc->to_year = now.year;
    calc->from_month = calc->to_month = now.month;
    calc->from_day = calc->to_day = now.day;

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
