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
#include "recon_data.h"
#include "recon_filedlg.h"
#include "recon_expr.h"
#include "recon_calc_modes.h"
#include "recon_clock.h"
#include "recon_module.h"
#include "recon_fs.h"
#include "recon_png.h"
#include "recon_registry.h"
#include "recon_users.h"
#include "recon_theme.h"
#include "recon_ui.h"

#define DISPLAY_HEIGHT 44
/* Room either side of a tab's name, and between one tab and the next. */
#define TAB_PADDING 12
#define TAB_GAP 4

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

/*
 * The graph's own colours, taken from the readout roles.
 *
 * A graph is a dark panel with lines on it, which is what a readout is -- so it
 * takes the same roles rather than a fifth set for a skin to have to answer.
 * The grid is the readout's own text at a fraction of its strength, so it stays
 * behind the curve on every palette instead of being a grey that happens to
 * work on most of them.
 */
#define COLOR_AXIS THEME(READOUT_TEXT)
#define COLOR_GRID recon_color_mix(THEME(READOUT), THEME(READOUT_TEXT), 60)
#define COLOR_CURVE THEME(READOUT_ACCENT)
#define COLOR_WARNING THEME(WARNING)

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
    /* Date, Convert and Graph are not keypads. */
    [CALC_DATE]       = { 0, 0, NULL },
    [CALC_CONVERT]    = { 0, 0, NULL },
    [CALC_GRAPH]      = { 0, 0, NULL },
};

#define GRAPH_SPAN 10.0

/*
 * And a polar one, which is a quarter of it.
 *
 * The two modes have genuinely different natural scales. Almost every polar
 * curve anybody types has a radius of one or two -- sin(3t) is a rose that
 * fits inside the unit circle, 1+cos(t) reaches two -- so drawing it in a
 * window twenty units wide gives a shape the size of a thumbnail, which reads
 * as the mode not working rather than as the view being wrong.
 */
#define GRAPH_SPAN_POLAR 2.5

/*
 * How many curves at once.
 *
 * Three. Each takes a row above the picture, and a picture with six rows over
 * it is a form with a graph attached.
 */
#define GRAPH_CURVES 3

/*
 * Where the three expressions are kept between sessions.
 *
 * In the user's own settings rather than a file, because they are a setting:
 * three short strings that belong to whoever typed them. Closing the window
 * lost them, which made the mode something to be re-typed into rather than
 * something to come back to -- and an expression somebody spent a minute
 * getting right is exactly the thing worth keeping.
 */
#define GRAPH_KEY_PREFIX "calculator/graph-"
/* Which shape of question the fields are answering. Remembered with them,
 * because an expression means a different curve under a different kind and
 * coming back to the wrong one would look like the expressions being wrong. */
#define GRAPH_KIND_KEY "calculator/graph-kind"

/*
 * How far round a polar curve is drawn, and how finely.
 *
 * Four turns rather than one, because a spiral is a polar curve and r = theta
 * over a single turn is a comma. Curves that close after one turn are simply
 * drawn over themselves three more times, which costs nothing and is invisible.
 *
 * The step is in radians and is what makes a rose look like a rose rather than
 * a polygon. Fixed rather than derived from the zoom: the alternative is a
 * curve that gets coarser as you zoom in, which is exactly backwards.
 */
#define POLAR_TURNS 4
#define POLAR_STEPS 2400

/*
 * How far the parameter runs in parametric mode, and how finely.
 *
 * Minus two pi to two pi. There is no natural range for a parameter the way
 * there is for an angle, so this is a choice rather than a consequence, and it
 * is the one that makes the two commonest things work: (cos t, sin t) closes
 * its circle, and (t, t^2) is a parabola with both arms rather than half of
 * one. Symmetric for that second reason -- a range starting at zero draws
 * every even function as a hook.
 *
 * A range somebody can set is the obvious next thing and is two more fields
 * above an already crowded form; it is written down in docs/ROADMAP.md rather
 * than guessed at here.
 */
#define PARAM_FROM (-6.283185307179586)
#define PARAM_TO 6.283185307179586
#define PARAM_STEPS 2400

/* Where the second half of a parametric pair is written down. */
#define GRAPH_Y_KEY_PREFIX "calculator/graph-y-"

/*
 * How many points one data file may put on the plane.
 *
 * A ceiling rather than an allocation, and one that says when it is reached:
 * a file with more rows is drawn up to here and the line under the plane says
 * how many were left. Silently drawing the first four thousand of nine
 * thousand would be a picture of a different data set, which is the same fault
 * as silently dropping a row that will not parse.
 *
 * Four thousand because the plane is a few hundred pixels across and points
 * beyond that are drawing on each other; a file that needs more needs a
 * different kind of chart.
 */
#define DATA_POINTS_MAX 4000

/*
 * How much one notch of the wheel changes the view.
 *
 * The same factor as the Zoom in and Zoom out buttons, so the two agree and
 * one notch out undoes one notch in exactly. A wheel that zoomed by a
 * different amount from the buttons would make the two ways of doing it
 * disagree about where you are.
 */
#define GRAPH_ZOOM_STEP 1.25

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

    /*
     * Graph mode: the expression, and how much of the plane is on screen.
     *
     * The window is kept as a half-width and a half-height about the origin,
     * so zooming is one multiply and the centre never drifts. Storing the four
     * edges instead means zoom has to work out a centre first, and rounding
     * moves it a little every time somebody presses the button.
     */
    /*
     * The curves, and which field is being typed into.
     *
     * Three rather than one because a grapher exists to compare -- "is x^2
     * above or below 2^x" is the question, and answering it by typing one,
     * looking, typing the other and remembering is not answering it. Three
     * rather than more because each needs a row above the picture, and a
     * picture with six rows over it is a form with a graph attached.
     */
    struct recon_edit formula[GRAPH_CURVES];
    int formula_focused;

    /*
     * What the fields mean.
     *
     * The same three fields under both, because the question changes and the
     * number of curves worth comparing does not. A polar field holds r as a
     * function of the angle; an ordinary one holds y as a function of x.
     */
    enum graph_kind {
        GRAPH_XY,
        GRAPH_POLAR,
        GRAPH_PARAM,
        GRAPH_DATA,
    } graph_kind;

    /*
     * The other half of a parametric curve.
     *
     * A second array rather than doubling the first, because only one mode has
     * two fields per curve and the three above are the same three fields under
     * all of them: switching modes keeps what was typed, and it can only do
     * that if `formula[1]` means the same slot whichever question is being
     * asked.
     *
     * Laid out beside its partner rather than under it, so parametric has
     * three rows like the others rather than six. Two narrower fields on a row
     * is a form; six rows over a small plane is a form with a graph attached.
     */
    struct recon_edit formula_y[GRAPH_CURVES];

    /*
     * A file's worth of points, and what reading it came to.
     *
     * Held rather than re-read every frame: the plane redraws on every pointer
     * move while somebody is dragging it, and reading a file per frame would
     * make dragging a graph of a large file feel like dragging through mud.
     * `loaded` is the path these points came from, so a changed field is
     * noticed without anything having to announce it.
     *
     * `bad` and `over` are the two honest answers this mode owes: rows that
     * were not numbers, and rows there was no room for. A plot that quietly
     * dropped either would be a picture of a data set nobody has.
     */
    struct data_set {
        char loaded[RECON_PATH_MAX];
        double x[DATA_POINTS_MAX];
        double y[DATA_POINTS_MAX];
        int count;
        int bad;
        int over;
        bool read_failed;
        /* Which character the file wrote its decimal points with. Recorded
         * rather than assumed, and said out loud, because reading a file the
         * other way round is silently wrong rather than visibly wrong. */
        bool comma_decimal;
    } data[GRAPH_CURVES];

    double span_x, span_y;

    /*
     * Where the middle of the picture is, in the plane.
     *
     * It was always the origin, which makes a grapher that can only ever look
     * at one place: every interesting part of log(x) is to the right of it,
     * and zooming in to see the shape of something near x=10 moves it off the
     * screen.
     */
    double centre_x, centre_y;

    /*
     * Dragging the plane about.
     *
     * The centre at the moment the drag began is remembered rather than
     * updated as it goes, so the point under the pointer stays under the
     * pointer -- accumulating small steps instead drifts, and a picture that
     * slides out from under the finger holding it feels broken in a way that
     * is hard to name.
     */
    bool panning;
    int pan_from_x, pan_from_y;
    double pan_centre_x, pan_centre_y;

    /* Pixels per unit, worked out while drawing and needed while dragging. */
    double per_unit;

    /*
     * Where the pointer is, in units rather than pixels.
     *
     * Kept by the motion handler because the wheel has no coordinates of its
     * own -- a scroll callback is given a direction and nothing else -- and
     * zooming about the middle instead of about the pointer is the difference
     * between a grapher somebody explores with and one they fight.
     */
    double pointer_x, pointer_y;

    /* The middle of the plane, in content coordinates, so motion can turn a
     * pixel into a value without re-deriving the layout. */
    int plane_mid_x, plane_mid_y;

    /*
     * The plane's rectangle, in the window's own coordinates.
     *
     * Recorded while drawing, because that is the only place it is worked out
     * and Save has to copy exactly those pixels. Deriving it a second time
     * would be a second copy of the layout arithmetic, and two copies of one
     * rule is how a saved picture comes to have somebody else's buttons along
     * the top of it.
     */
    int plane_x, plane_y, plane_w, plane_h;

    /* What the last Save did, said under the buttons. */
    char note[160];

    /*
     * A Save asked for and not yet done.
     *
     * Done during the next draw rather than at the click, because saving means
     * reading the panel's pixels and a click does not have the panel. It also
     * means the picture is of the frame the person was looking at when they
     * pressed the button, which is what they meant.
     */
    bool want_save;

    /*
     * Choosing a data file rather than typing its path.
     *
     * Drawn inside this window, like every other file dialog in the system,
     * and it takes all input while it is up.
     */
    struct recon_filedlg picker;

    /*
     * Which of the three path fields the picker is filling.
     *
     * Held rather than derived, because by the time a path comes back the
     * click that started it is long gone -- and filling the focused field
     * instead would mean a click on the third row's button writing into the
     * first row whenever the caret happened to be there.
     */
    int picking_for;

    /*
     * The window, kept so that panning can ask for a redraw.
     *
     * A click is redrawn by the shell because the shell knows a click may have
     * changed something. Pointer motion is not -- most motion changes nothing,
     * and redrawing every window on every pixel of movement would be a
     * compositor that never idles. So a handler that DOES change something on
     * motion has to say so, and the first version of this did not: the plane
     * moved, the numbers moved, and the picture went on showing where it used
     * to be. It looked exactly like the drag not being delivered at all, and
     * an afternoon could go into checking the input path.
     */
    struct recon_appwin *win;

};

/* Where one curve's expression is written down. */
static void graph_key(int i, char *out, size_t size) {
    snprintf(out, size, "%s%d", GRAPH_KEY_PREFIX, i);
}

/*
 * Kept as each is typed rather than when the window closes.
 *
 * A window that saves on close loses everything if it is never closed
 * politely, and the whole point of remembering these is that somebody spent a
 * minute getting one right. Three short registry writes per keystroke is
 * nothing.
 */
static void remember_graph(struct recon_calc *calc) {
    for (int i = 0; i < GRAPH_CURVES; i++) {
        char key[64];
        graph_key(i, key, sizeof(key));
        recon_registry_set(RECON_REG_USER, key, calc->formula[i].text);

        snprintf(key, sizeof(key), "%s%d", GRAPH_Y_KEY_PREFIX, i);
        recon_registry_set(RECON_REG_USER, key, calc->formula_y[i].text);
    }

    const char *kind = "xy";
    if (calc->graph_kind == GRAPH_POLAR) {
        kind = "polar";
    } else if (calc->graph_kind == GRAPH_PARAM) {
        kind = "parametric";
    } else if (calc->graph_kind == GRAPH_DATA) {
        kind = "data";
    }
    recon_registry_set(RECON_REG_USER, GRAPH_KIND_KEY, kind);
}

/*
 * What the one variable may be called: `x`, and `t`.
 *
 * Both, in both modes, rather than one per mode. recon_expr always accepts
 * `x`, so naming the other spelling `t` here means every expression works
 * under either question -- and that is what makes switching modes teach
 * something: the same three lines stay in the fields and mean a wave and then
 * a circle.
 *
 * The alternative was tried first and photographed: with the variable called
 * `x` in the ordinary mode, switching back from polar left three fields
 * reading "'t' is not something this knows", which is true, useless, and
 * reads as the mode having broken the expressions.
 *
 * The labels and the hint still name the idiomatic one for each mode. Which
 * spelling somebody uses is not a thing this needs an opinion about; which
 * one it *suggests* is.
 */
static const char *graph_variable(void) {
    return "t";
}

/* How many fields this mode has: three, or three pairs. */
static int graph_field_count(const struct recon_calc *calc) {
    return calc->graph_kind == GRAPH_PARAM ? GRAPH_CURVES * 2 : GRAPH_CURVES;
}

/*
 * One field by number, counting the three main ones first.
 *
 * The numbering is what the hit ids and the focus both use, and keeping it in
 * one function is what stops the click handler and the keyboard disagreeing
 * about which box the caret is in -- which is the kind of fault that looks
 * like typing into the wrong field at random.
 */
static struct recon_edit *graph_field(struct recon_calc *calc, int i) {
    if (i < 0) {
        i = 0;
    }
    if (i < GRAPH_CURVES) {
        return &calc->formula[i];
    }
    if (i < GRAPH_CURVES * 2) {
        return &calc->formula_y[i - GRAPH_CURVES];
    }
    return &calc->formula[0];
}

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
#define HIT_GRAPH_IN (RECON_APPWIN_HIT_USER + 902)
#define HIT_GRAPH_OUT (RECON_APPWIN_HIT_USER + 903)
#define HIT_GRAPH_HOME (RECON_APPWIN_HIT_USER + 904)
#define HIT_GRAPH_SAVE (RECON_APPWIN_HIT_USER + 906)
/* The plane itself, which is dragged to move about in it. */
#define HIT_GRAPH_PLANE (RECON_APPWIN_HIT_USER + 905)
/* One per curve. */
#define HIT_GRAPH_XY (RECON_APPWIN_HIT_USER + 907)
#define HIT_GRAPH_POLAR (RECON_APPWIN_HIT_USER + 908)
#define HIT_GRAPH_PARAM (RECON_APPWIN_HIT_USER + 909)
#define HIT_GRAPH_DATA (RECON_APPWIN_HIT_USER + 907 + 100)
#define HIT_GRAPH_FIELD_BASE (RECON_APPWIN_HIT_USER + 910)
/* One per data-mode path field, in the space the y field uses in parametric
 * mode -- which is empty in data mode, so the button costs no room. */
#define HIT_GRAPH_PICK_BASE (RECON_APPWIN_HIT_USER + 920)

/*
 * The highest id this window can produce.
 *
 * Kept as its own name so that adding an id above it is visibly a change to
 * this line as well, and checked against the file dialog's range: the picker
 * is drawn inside this window and claims ids of its own. Without the check the
 * two overlapped, and clicking the file list would have worked the tab bar
 * behind it.
 */
#define HIT_CALC_TOP HIT_GRAPH_DATA
RECON_FILEDLG_IDS_BELOW(HIT_CALC_TOP);

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
/*
 * The mode tabs, each as wide as its own name.
 *
 * They used to be equal slices of the width, which was fine for five and broke
 * on the sixth: "Programmer" does not fit in a sixth of a narrow window, so it
 * was drawn as "Progra..." while "Date" sat in a box twice the size it needed.
 * A row of buttons where the long ones are cut off and the short ones are
 * padded is a row that reads as neither.
 *
 * And they wrap rather than shrink. A window narrow enough that six do not fit
 * gets two rows of six readable tabs instead of one row of six unreadable
 * ones -- the same thing the Convert page already does with its categories,
 * for the same reason.
 */
static int draw_tabs(struct recon_calc *calc, struct recon_panel *panel,
        int x, int y, int w) {
    int ascent = recon_font_ascent(calc->font);
    int line = recon_font_line_height(calc->font);

    int tx = x;
    int ty = y;

    for (int i = 0; i < CALC_MODE_COUNT; i++) {
        int tab_w = recon_text_width(calc->font, CALC_MODE_NAMES[i]) +
            TAB_PADDING * 2;

        if (tx + tab_w > x + w && tx > x) {
            tx = x;
            ty += TAB_HEIGHT + TAB_GAP;
        }

        bool on = calc->mode == (enum calc_mode)i;

        recon_fill_rect(panel, tx, ty, tab_w, TAB_HEIGHT,
            on ? COLOR_KEY_ACCENT : COLOR_KEY);
        /*
         * The button edge rather than a bare bevel, so these round with the
         * skin like every other button in the system -- and so they read as
         * six separate things rather than one strip with lines in it.
         */
        recon_draw_button_edge(panel, tx, ty, tab_w, TAB_HEIGHT, on,
            COLOR_BG);

        recon_draw_text(panel, calc->font, tx + TAB_PADDING,
            ty + (TAB_HEIGHT - line) / 2 + ascent, tab_w - TAB_PADDING,
            CALC_MODE_NAMES[i], on ? COLOR_ACCENT_TEXT : COLOR_KEY_TEXT);

        recon_hit_add(panel, tx, ty, tab_w, TAB_HEIGHT,
            HIT_TAB_BASE + (uint32_t)i);

        tx += tab_w + TAB_GAP;
    }

    return ty + TAB_HEIGHT + PAD_PADDING;
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

/* --- Graph --- */

/*
 * How many pixels a curve may jump before it is treated as two curves.
 *
 * A steep function moves a long way between one column and the next, and so
 * does a function that has an asymptote between them -- and the difference
 * cannot be seen from two points. What can be said is that no honest curve
 * crosses the whole window in one pixel of x, so a jump larger than the window
 * is a break rather than a slope. Getting this wrong in the other direction
 * draws tan(x) as a row of vertical walls.
 */
#define GRAPH_BREAK_FACTOR 2

/*
 * One polar curve: r as a function of the angle.
 *
 * Its own function rather than a branch inside the loop below, because it is
 * stepping something different. An ordinary curve has one point per column of
 * pixels and cannot double back; a polar one has a point per step of angle,
 * several of which may land in the same column and many of which land in none
 * at all, and it crosses itself as a matter of course.
 *
 * What it keeps from the ordinary one is the honesty about gaps. An angle
 * where the expression has no value breaks the stroke, so tan-shaped polar
 * curves are not joined across the place where they run off to infinity -- the
 * same lie, in a different coordinate system.
 *
 * Points are joined with straight segments rather than plotted as dots: at
 * four turns and this step the gaps are sub-pixel near the origin and several
 * pixels at the outside, and a curve made of dots is a dotted curve.
 */
/*
 * One point at a time along a path, joined where joining is honest.
 *
 * Shared by the polar and parametric curves, because they are the same drawing
 * problem: a path walked by a parameter, several of whose steps land in one
 * column of pixels and many of which land in none, and which crosses itself as
 * a matter of course. Only the arithmetic that turns a step into a point
 * differs, and that is the caller's.
 *
 * `had` is what keeps a break a break: a step where the expression has no value
 * starts a new stroke rather than being joined across the gap.
 */
struct path_pen {
    bool had;
    int last_x, last_y;
};

static void path_to(struct path_pen *pen, struct recon_panel *panel,
        double px, double py, recon_color ink, int x, int y, int w, int h) {
    /* Far outside the box in either direction: nothing to draw and nothing to
     * join to, so the stroke breaks rather than being dragged across the
     * window by a value of ten thousand. */
    if (!isfinite(px) || !isfinite(py) ||
            px < x - w || px > x + w * 2 ||
            py < y - h || py > y + h * 2) {
        pen->had = false;
        return;
    }

    int cx = (int)px;
    int cy = (int)py;

    if (pen->had) {
        /*
         * Walked along whichever axis the segment covers more of, with
         * neither privileged -- a path may be travelling in any direction,
         * which is exactly what a column-at-a-time loop cannot express.
         */
        int dx = cx - pen->last_x;
        int dy = cy - pen->last_y;
        int steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy)
            ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
        for (int s = 1; s < steps; s++) {
            int ix = pen->last_x + dx * s / steps;
            int iy = pen->last_y + dy * s / steps;
            if (ix >= x + 1 && ix < x + w - 1 &&
                    iy >= y + 1 && iy < y + h - 1) {
                recon_fill_rect(panel, ix, iy, 1, 1, ink);
            }
        }
    }

    if (cx >= x + 1 && cx < x + w - 1 && cy >= y + 1 && cy < y + h - 1) {
        recon_fill_rect(panel, cx, cy, 1, 1, ink);
    }
    pen->last_x = cx;
    pen->last_y = cy;
    pen->had = true;
}

/*
 * One parametric curve: x and y each a function of the same parameter.
 *
 * Both halves are asked at every step and either may have no value there --
 * so a step is drawn only when both answer, which is the honest reading: a
 * point needs two coordinates and half of one is not a place.
 */
static void draw_param_curve(struct recon_panel *panel, const char *fx,
        const char *fy, recon_color ink, int zero_x, int zero_y,
        double per_unit_x, double per_unit_y, int x, int y, int w, int h) {
    struct path_pen pen = { false, 0, 0 };
    double step = (PARAM_TO - PARAM_FROM) / PARAM_STEPS;

    for (int i = 0; i <= PARAM_STEPS; i++) {
        double t = PARAM_FROM + i * step;

        double vx = 0.0;
        double vy = 0.0;
        if (recon_expr_eval_named(fx, graph_variable(), t, &vx, NULL, 0)
                != RECON_EXPR_OK ||
            recon_expr_eval_named(fy, graph_variable(), t, &vy, NULL, 0)
                != RECON_EXPR_OK) {
            pen.had = false;
            continue;
        }

        path_to(&pen, panel, zero_x + vx * per_unit_x,
            zero_y - vy * per_unit_y, ink, x, y, w, h);
    }
}

/*
 * Read a file of numbers into one curve's points.
 *
 * The format is the one somebody would write without being told: two numbers
 * a line, separated by whitespace or a comma. Blank lines and lines starting
 * with `#` are skipped and are not counted as bad, because a file with a
 * heading on it is a file somebody made by hand and meant.
 *
 * --- What happens to a line that is not two numbers ---
 *
 * It is counted, and the count is shown. That is the whole of this mode's
 * design and the reason it took a decision rather than an afternoon: a plot
 * that silently drops the rows it could not read is a picture of a different
 * data set, and it looks exactly like a picture of the right one. So does a
 * plot that stops at the first bad line and shows half.
 *
 * Neither refusing nor ignoring, then: draw what was read and say what was
 * not. The same answer the rest of this system gives about ceilings.
 */
static bool load_data(struct data_set *set, const char *path) {
    if (strcmp(set->loaded, path) == 0) {
        return false;   /* Already the file in the field. */
    }

    snprintf(set->loaded, sizeof(set->loaded), "%s", path);
    set->count = 0;
    set->bad = 0;
    set->over = 0;
    set->read_failed = false;
    set->comma_decimal = false;

    if (path[0] == '\0') {
        return true;
    }

    size_t size = 0;
    char *text = recon_fs_read("/", path, &size);
    if (text == NULL) {
        set->read_failed = true;
        return true;
    }

    struct recon_data_read got =
        recon_data_parse(text, set->x, set->y, DATA_POINTS_MAX);

    set->count = got.count;
    set->bad = got.bad;
    set->over = got.over;
    set->comma_decimal = got.comma_decimal;

    free(text);
    return true;
}

/*
 * Put the view where the data is.
 *
 * A plot of readings is not a plot of a function: an expression can be looked
 * at anywhere and still be an expression, while a file of measurements is a
 * particular set of numbers in particular units. Somebody plotting readings
 * between 0 and 1000 into a window ten units wide sees nothing at all, and
 * what they conclude is that the mode does not work.
 *
 * So the view is fitted when a file is read, and only then -- panning and
 * zooming afterwards are somebody's own and are not undone on the next frame.
 *
 * A margin of a tenth, so the outermost points are not drawn on the edge of
 * the plane where half a mark is outside it. A set with no spread -- every
 * reading the same, or one reading -- gets a unit either side rather than a
 * span of zero, which would divide by nothing.
 */
static void fit_to_data(struct recon_calc *calc) {
    double low_x = 0, high_x = 0, low_y = 0, high_y = 0;
    bool any = false;

    for (int c = 0; c < GRAPH_CURVES; c++) {
        const struct data_set *set = &calc->data[c];
        for (int i = 0; i < set->count; i++) {
            if (!any) {
                low_x = high_x = set->x[i];
                low_y = high_y = set->y[i];
                any = true;
                continue;
            }
            if (set->x[i] < low_x) { low_x = set->x[i]; }
            if (set->x[i] > high_x) { high_x = set->x[i]; }
            if (set->y[i] < low_y) { low_y = set->y[i]; }
            if (set->y[i] > high_y) { high_y = set->y[i]; }
        }
    }

    if (!any) {
        return;   /* Nothing read, so nothing to look at. */
    }

    calc->centre_x = (low_x + high_x) / 2.0;
    calc->centre_y = (low_y + high_y) / 2.0;

    double wide = (high_x - low_x) / 2.0;
    double tall = (high_y - low_y) / 2.0;
    double span = (wide > tall) ? wide : tall;
    if (!(span > 0.0)) {
        span = 1.0;
    }
    calc->span_x = span * 1.1;
    calc->span_y = calc->span_x;
}

/*
 * One data set, drawn as points with a line through them.
 *
 * Both, because either alone lies about a different thing: points alone hide
 * how few there are between two far apart, and a line alone makes measurements
 * look like a function. A mark at every reading and a line joining them says
 * what was measured and in what order.
 *
 * Not sorted by x. The order in the file is the order they are joined in,
 * because a data plot is often a path -- a position over time, a hysteresis
 * loop -- and sorting one would draw something that never happened.
 */
static void draw_data_set(struct recon_panel *panel, const struct data_set *set,
        recon_color ink, int zero_x, int zero_y, double per_unit_x,
        double per_unit_y, int x, int y, int w, int h) {
    struct path_pen pen = { false, 0, 0 };

    for (int i = 0; i < set->count; i++) {
        double px = zero_x + set->x[i] * per_unit_x;
        double py = zero_y - set->y[i] * per_unit_y;
        path_to(&pen, panel, px, py, ink, x, y, w, h);

        /* And a mark, so a single reading is visible and so the readings can
         * be told from the line between them. */
        int cx = (int)px;
        int cy = (int)py;
        if (cx >= x + 2 && cx < x + w - 2 && cy >= y + 2 && cy < y + h - 2) {
            recon_fill_rect(panel, cx - 1, cy - 1, 3, 3, ink);
        }
    }
}

static void draw_polar_curve(struct recon_calc *calc, struct recon_panel *panel,
        const char *text, recon_color ink, int zero_x, int zero_y,
        double per_unit_x, double per_unit_y, int x, int y, int w, int h) {
    struct path_pen pen = { false, 0, 0 };
    double step = (POLAR_TURNS * 2.0 * 3.14159265358979323846) / POLAR_STEPS;

    for (int i = 0; i <= POLAR_STEPS; i++) {
        double angle = i * step;

        double r = 0.0;
        if (recon_expr_eval_named(text, graph_variable(), angle, &r, NULL, 0)
                != RECON_EXPR_OK) {
            pen.had = false;
            continue;
        }

        /*
         * A negative r is drawn opposite the angle, which is what the
         * convention says and what makes r = cos(2t) a four-petal rose rather
         * than a two-petal one. cos and sin do this on their own -- the sign
         * carries through the multiplication -- so there is nothing to write
         * here except the note that it was not forgotten.
         */
        path_to(&pen, panel, zero_x + r * cos(angle) * per_unit_x,
            zero_y - r * sin(angle) * per_unit_y, ink, x, y, w, h);
    }
    (void)calc;
}

/* How much of the plane is on screen at the start, either side of zero. */

/*
 * Write the plane out as a picture.
 *
 * The pixels the window last drew, cropped to the plane, rather than a second
 * drawing of the same thing into a buffer. A second drawing is a second copy
 * of every rule about where a curve goes, and the two would drift -- the saved
 * picture would stop being a picture of what was on screen, which is the one
 * thing it has to be.
 *
 * Into Pictures, under a name that says what it is and when, because a grapher
 * that saved over graph.png would lose the one somebody kept.
 */
static void save_graph(struct recon_calc *calc, struct recon_panel *panel) {
    if (calc->plane_w <= 0 || calc->plane_h <= 0) {
        snprintf(calc->note, sizeof(calc->note),
            "There is nothing drawn to save yet.");
        return;
    }

    size_t count = (size_t)calc->plane_w * (size_t)calc->plane_h;
    uint32_t *pixels = malloc(count * sizeof(*pixels));
    if (pixels == NULL) {
        snprintf(calc->note, sizeof(calc->note),
            "There was not enough memory to save it.");
        return;
    }

    if (!recon_panel_read(panel, calc->plane_x, calc->plane_y,
            calc->plane_w, calc->plane_h, pixels)) {
        free(pixels);
        snprintf(calc->note, sizeof(calc->note),
            "The graph could not be read off the screen.");
        return;
    }

    struct recon_clock_time now;
    recon_clock_now(&now);

    char name[128];
    snprintf(name, sizeof(name), "Graph %04d-%02d-%02d %02d%02d%02d.png",
        now.year, now.month, now.day, now.hour, now.minute, now.second);

    char path[RECON_PATH_MAX];
    if (!recon_fs_join(path, sizeof(path), "/Users", name)) {
        free(pixels);
        snprintf(calc->note, sizeof(calc->note), "That path is too long.");
        return;
    }

    /* The signed-in person's own Pictures folder, which is where every other
     * picture this system writes goes. */
    const char *user = recon_users_current();
    char folder[RECON_PATH_MAX];
    if (user == NULL ||
            snprintf(folder, sizeof(folder), "/Users/%s/Pictures", user) >=
                (int)sizeof(folder) ||
            !recon_fs_join(path, sizeof(path), folder, name)) {
        free(pixels);
        snprintf(calc->note, sizeof(calc->note),
            "There is nowhere to save it to.");
        return;
    }

    /*
     * Opaque. The plane is drawn on the readout's colour and has no
     * transparency in it, and a PNG with an alpha channel full of 255 is a
     * third larger for nothing.
     */
    bool ok = recon_png_write(path, pixels, calc->plane_w, calc->plane_h,
        false);
    free(pixels);

    if (ok) {
        snprintf(calc->note, sizeof(calc->note), "Saved as %s in Pictures.",
            name);
    } else {
        snprintf(calc->note, sizeof(calc->note), "%s",
            recon_png_last_error());
    }
}

/*
 * The colour of each curve.
 *
 * The first is the skin's own readout accent, which every skin already defines
 * to be legible against the readout it sits on. The other two are that colour
 * with its hue rotated a third of the way round and two thirds -- by shuffling
 * the channels, which for a saturated colour is exactly that rotation.
 *
 * Rotating rather than picking three fixed colours keeps the skin's character
 * and keeps the *lightness*, which is the half of a colour that decides
 * whether it can be seen at all: recon_color_tint uses the shuffled colour as
 * a hue reference and puts the accent's own lightness back.
 *
 * On a skin whose readout accent is grey there is no hue to rotate, so all
 * three come out the same. That is why each field has a swatch beside it: the
 * picture says which curve is where, and the swatch says which field drew it,
 * and only one of those can fail.
 */
static recon_color curve_color(int i) {
    recon_color accent = THEME(READOUT_ACCENT);
    if (i <= 0) {
        return accent;
    }

    uint32_t r = (accent >> 16) & 0xFF;
    uint32_t g = (accent >> 8) & 0xFF;
    uint32_t b = accent & 0xFF;

    recon_color shuffled = (i == 1)
        ? RECON_RGB(b, r, g)
        : RECON_RGB(g, b, r);

    return recon_color_tint(accent, shuffled, 255);
}

static void draw_graph_mode(struct recon_calc *calc, struct recon_panel *panel,
        int x, int y, int w, int bottom) {
    int ascent = recon_font_ascent(calc->font);
    int line = recon_font_line_height(calc->font);

    /*
     * The expressions, above the picture of them.
     *
     * A swatch in each curve's colour rather than a number, because what
     * somebody wants to know looking at three lines is which field drew the
     * one they are pointing at -- and a legend that says "1, 2, 3" makes them
     * count the lines to find out.
     */
    /*
     * Which question the fields are answering, chosen before them.
     *
     * Two buttons rather than one that toggles, so the mode can be seen
     * without being changed -- a single button reading "Polar" says neither
     * what is showing nor what pressing it does.
     */
    static const struct { const char *label; uint32_t hit; int kind; } KINDS[] = {
        { "y of x", HIT_GRAPH_XY, GRAPH_XY },
        { "Polar", HIT_GRAPH_POLAR, GRAPH_POLAR },
        { "Parametric", HIT_GRAPH_PARAM, GRAPH_PARAM },
        { "Data", HIT_GRAPH_DATA, GRAPH_DATA },
    };
    int kx = x;
    for (size_t i = 0; i < sizeof(KINDS) / sizeof(KINDS[0]); i++) {
        bool on = (calc->graph_kind == (enum graph_kind)KINDS[i].kind);
        int bw = recon_text_width(calc->font, KINDS[i].label) + 16;
        recon_fill_rect(panel, kx, y, bw, line + 4,
            on ? COLOR_KEY_ACCENT : COLOR_KEY);
        recon_draw_bevel(panel, kx, y, bw, line + 4, on);
        recon_draw_text(panel, calc->font, kx + 8, y + ascent + 2, bw,
            KINDS[i].label, on ? COLOR_ACCENT_TEXT : COLOR_KEY_TEXT);
        recon_hit_add(panel, kx, y, bw, line + 4, KINDS[i].hit);
        kx += bw + 6;
    }
    y += line + 8;

    /*
     * "r =" and "y =" are the same width in this font and would not be in
     * another, so the wider of the two decides -- otherwise the fields shift
     * sideways when the mode changes, which reads as the layout breaking.
     */
    const char *field_label = "y =";
    if (calc->graph_kind == GRAPH_POLAR) {
        field_label = "r =";
    } else if (calc->graph_kind == GRAPH_PARAM) {
        field_label = "x =";
    } else if (calc->graph_kind == GRAPH_DATA) {
        field_label = "file";
    }

    /* The widest of them decides, so the fields do not shift sideways when
     * the mode changes -- which reads as the layout breaking rather than as
     * the label being a different word. */
    static const char *LABELS[] = { "y =", "r =", "x =", "file" };
    int label_w = 0;
    for (size_t i = 0; i < sizeof(LABELS) / sizeof(LABELS[0]); i++) {
        int other = recon_text_width(calc->font, LABELS[i]);
        if (other > label_w) {
            label_w = other;
        }
    }
    label_w += 6;

    /*
     * Parametric puts its pair on one row rather than under each other, so
     * every mode has three rows and the plane keeps its height. Two narrower
     * fields is a form; six rows over a small plane is a form with a graph
     * attached.
     */
    bool paired = (calc->graph_kind == GRAPH_PARAM);

    /*
     * Data mode ends each row with a button that opens a file dialog, and the
     * field has to be told about it.
     *
     * The first version of this drew the button after the field without
     * narrowing it, on the assumption that the space the paired modes use for
     * their second field was going spare. It is not: an unpaired field takes
     * the whole row. The button was drawn off the right-hand edge and clipped,
     * which looked exactly like the feature not being there -- and a
     * photograph of the mode is what said so, not the code.
     */
    const char *CHOOSE = "Choose";
    bool choosing = (calc->graph_kind == GRAPH_DATA);
    int choose_w = choosing ? recon_text_width(calc->font, CHOOSE) + 16 : 0;

    int room = w - label_w - 16;
    int field_w = paired
        ? (room - label_w - 8) / 2
        : room - (choosing ? choose_w + 8 : 0);

    for (int i = 0; i < GRAPH_CURVES; i++) {
        recon_fill_rect(panel, x, y + 2, 10, line - 2, curve_color(i));

        int at = x + 16;
        recon_draw_text(panel, calc->font, at, y + ascent, label_w,
            field_label, COLOR_KEY_TEXT);
        at += label_w;
        recon_edit_draw(panel, calc->font, at, y - 2, field_w, line + 6,
            &calc->formula[i]);
        recon_hit_add(panel, at, y - 2, field_w, line + 6,
            HIT_GRAPH_FIELD_BASE + (uint32_t)i);
        at += field_w + 8;

        /*
         * The button costs the field some width rather than costing the form a
         * row -- which is the objection the roadmap raised against putting a
         * picker here, and the answer to it. A path is long, but it scrolls
         * inside the field; a fourth row would come off the plane's height,
         * and the plane is the thing the mode is for.
         */
        if (choosing) {
            recon_fill_rect(panel, at, y - 2, choose_w, line + 6, COLOR_KEY);
            recon_draw_bevel(panel, at, y - 2, choose_w, line + 6, false);
            recon_draw_text(panel, calc->font, at + 8, y + ascent, choose_w,
                CHOOSE, COLOR_KEY_TEXT);
            recon_hit_add(panel, at, y - 2, choose_w, line + 6,
                HIT_GRAPH_PICK_BASE + (uint32_t)i);
        }

        if (paired) {
            recon_draw_text(panel, calc->font, at, y + ascent, label_w, "y =",
                COLOR_KEY_TEXT);
            at += label_w;
            recon_edit_draw(panel, calc->font, at, y - 2, field_w, line + 6,
                &calc->formula_y[i]);
            recon_hit_add(panel, at, y - 2, field_w, line + 6,
                HIT_GRAPH_FIELD_BASE + (uint32_t)(GRAPH_CURVES + i));
        }

        y += line + 6;
    }
    y += 4;

    /* Zoom, and a way back to where it started. */
    int bx = x;
    static const struct { const char *label; uint32_t hit; } BUTTONS[] = {
        { "Zoom in", HIT_GRAPH_IN },
        { "Zoom out", HIT_GRAPH_OUT },
        { "Reset", HIT_GRAPH_HOME },
        { "Save", HIT_GRAPH_SAVE },
    };
    for (size_t i = 0; i < sizeof(BUTTONS) / sizeof(BUTTONS[0]); i++) {
        int bw = recon_text_width(calc->font, BUTTONS[i].label) + 16;
        recon_fill_rect(panel, bx, y, bw, line + 6, COLOR_KEY);
        recon_draw_bevel(panel, bx, y, bw, line + 6, false);
        recon_draw_text(panel, calc->font, bx + 8, y + ascent + 3, bw,
            BUTTONS[i].label, COLOR_KEY_TEXT);
        recon_hit_add(panel, bx, y, bw, line + 6, BUTTONS[i].hit);
        bx += bw + 6;
    }

    char range[64];
    snprintf(range, sizeof(range), "x %+.2f to %+.2f",
        calc->centre_x - calc->span_x, calc->centre_x + calc->span_x);
    /* What Save did, if it has done anything, in place of the range -- the
     * range is always true and the note is news. */
    const char *under = calc->note[0] != '\0' ? calc->note : range;
    recon_draw_text(panel, calc->font, bx + 6, y + ascent + 3, w - (bx - x),
        under, COLOR_KEY_TEXT);
    y += line + 12;

    int h = bottom - y;
    if (h < 40 || w < 40) {
        return;
    }

    /* The plane it is drawn on. */
    recon_fill_rect(panel, x, y, w, h, COLOR_DISPLAY);
    recon_draw_bevel(panel, x, y, w, h, true);

    int mid_x = x + w / 2;
    int mid_y = y + h / 2;

    /*
     * Gridlines at whole numbers, and only while they are far enough apart to
     * be lines rather than a fill. Zoomed out far enough, a line per unit is a
     * solid rectangle that says nothing.
     */
    /*
     * Square pixels: one unit across is one unit down.
     *
     * The height on screen follows from the width and the shape of the box
     * rather than being a second number somebody sets. Letting the two differ
     * means a circle is drawn as an ellipse and a slope of one is not at
     * forty-five degrees -- which is a quieter lie than drawing a wall at an
     * asymptote and the same kind: a picture that is not of the function.
     *
     * The cost is that sin(x) looks flat at this scale, because it is: an
     * amplitude of one inside a window ten units tall. Zooming fixes it, and
     * a grapher that silently stretched it would be answering a question
     * nobody asked.
     */
    double per_unit_x = (w / 2.0) / calc->span_x;
    double per_unit_y = per_unit_x;
    calc->span_y = (h / 2.0) / per_unit_y;
    calc->per_unit = per_unit_x;
    calc->plane_mid_x = mid_x;
    calc->plane_mid_y = mid_y;
    calc->plane_x = x;
    calc->plane_y = y;
    calc->plane_w = w;
    calc->plane_h = h;

    /*
     * The plane takes drags, and is registered before the curves are drawn
     * over it -- nothing else claims this rectangle, so it does not matter
     * where in the drawing it goes, and here is where the geometry is known.
     */
    recon_hit_add(panel, x, y, w, h, HIT_GRAPH_PLANE);

    /*
     * Where zero is on screen, which is the middle only when the view has not
     * been moved. Everything below measures from these rather than from the
     * middle of the box, which is the whole of what panning is.
     */
    int zero_x = mid_x - (int)(calc->centre_x * per_unit_x);
    int zero_y = mid_y + (int)(calc->centre_y * per_unit_y);

    /*
     * Drawn outward from zero and clipped to the box, rather than counted
     * from the middle. Once the view can be moved, zero is not the middle --
     * and a grid counted from the middle would slide half a line at a time as
     * the plane moved under it, which looks like the grid being wrong rather
     * than like the view moving.
     */
    if (per_unit_x >= 8.0) {
        for (int i = 1; i < 4000; i++) {
            int dx = (int)(i * per_unit_x);
            bool any = false;
            if (zero_x + dx > x && zero_x + dx < x + w - 1) {
                recon_fill_rect(panel, zero_x + dx, y + 1, 1, h - 2,
                    COLOR_GRID);
                any = true;
            }
            if (zero_x - dx > x && zero_x - dx < x + w - 1) {
                recon_fill_rect(panel, zero_x - dx, y + 1, 1, h - 2,
                    COLOR_GRID);
                any = true;
            }
            if (!any && zero_x - dx <= x && zero_x + dx >= x + w - 1) {
                break;
            }
        }
    }
    if (per_unit_y >= 8.0) {
        for (int i = 1; i < 4000; i++) {
            int dy = (int)(i * per_unit_y);
            bool any = false;
            if (zero_y + dy > y && zero_y + dy < y + h - 1) {
                recon_fill_rect(panel, x + 1, zero_y + dy, w - 2, 1,
                    COLOR_GRID);
                any = true;
            }
            if (zero_y - dy > y && zero_y - dy < y + h - 1) {
                recon_fill_rect(panel, x + 1, zero_y - dy, w - 2, 1,
                    COLOR_GRID);
                any = true;
            }
            if (!any && zero_y - dy <= y && zero_y + dy >= y + h - 1) {
                break;
            }
        }
    }

    /* The axes, brighter than the grid, because they are where zero is --
     * drawn only when zero is somewhere on screen to draw. */
    if (zero_y > y && zero_y < y + h - 1) {
        recon_fill_rect(panel, x + 1, zero_y, w - 2, 1, COLOR_AXIS);
    }
    if (zero_x > x && zero_x < x + w - 1) {
        recon_fill_rect(panel, zero_x, y + 1, 1, h - 2, COLOR_AXIS);
    }

    /*
     * Nothing typed anywhere, so say what to type.
     *
     * Asked of all three rather than the first, because somebody who has
     * cleared the first field and left the other two should see their curves
     * and not an instruction.
     */
    bool anything = false;
    for (int i = 0; i < graph_field_count(calc); i++) {
        if (graph_field(calc, i)->text[0] != '\0') {
            anything = true;
        }
    }
    if (!anything) {
        /*
         * `sin(3t)` again, because it works again. The star was here for one
         * commit, while the grammar could not read the form everybody writes.
         */
        recon_draw_text(panel, calc->font, x + 8, y + h - 10 - ascent / 2,
            w - 16, calc->graph_kind == GRAPH_POLAR
                ? "Type a distance from the origin, in the angle t. "
                  "For example: sin(3t), 1+cos(t), t"
                : calc->graph_kind == GRAPH_PARAM
                ? "Type x and y, both in t. For example: cos(t) and sin(t), "
                  "or t and t^2"
                : calc->graph_kind == GRAPH_DATA
                ? "Type the path of a file with two numbers on each line. "
                  "For example: /Users/you/Documents/readings.txt"
                : "Type an expression in x. For example: sin(x), 2x^2-1, 1/x",
            COLOR_KEY_TEXT);
        return;
    }

    /*
     * The first thing wrong, named, and only the first.
     *
     * Three complaints stacked on top of each other in a box this size is a
     * picture nobody can see. The one reported is the first field with
     * something wrong in it, and its swatch says which.
     */
    int complaining = -1;
    char why[128];
    why[0] = '\0';
    for (int i = 0; i < graph_field_count(calc) && complaining < 0; i++) {
        struct recon_edit *field = graph_field(calc, i);
        /* A path is not an expression, and running it through the grammar
         * would complain about a slash. What is wrong with a file is said
         * below, where the file has been read. */
        if (calc->graph_kind == GRAPH_DATA) {
            break;
        }
        if (field->text[0] != '\0' &&
                !recon_expr_valid_named(field->text, graph_variable(), why,
                    sizeof(why))) {
            /* The swatch names a curve, not a field, so a complaint about the
             * y of the second pair points at the second colour. */
            complaining = i % GRAPH_CURVES;
        }
    }

    for (int c = 0; c < GRAPH_CURVES; c++) {
        if (calc->formula[c].text[0] == '\0' || c == complaining) {
            continue;
        }
        /*
         * A data file's field holds a path, not an expression, and a path is
         * not one -- so this check has to come after the mode is known rather
         * than before it. It did not, and every data file was skipped here
         * and then reported as having no numbers in it, which is what a file
         * that was never opened looks like from the other end.
         */
        if (calc->graph_kind != GRAPH_DATA &&
                !recon_expr_valid_named(calc->formula[c].text,
                    graph_variable(), NULL, 0)) {
            continue;
        }

        recon_color ink = curve_color(c);

        /*
         * A polar curve is walked round rather than across.
         *
         * The difference is not the drawing, it is what is being stepped: an
         * ordinary curve has one point per column of pixels, and a polar one
         * has a point per step of angle, several of which may land in the same
         * column and many of which land in none. So it is its own loop rather
         * than a flag inside the one below -- and it joins its points with the
         * same rule, since a polar curve has asymptotes too.
         */
        if (calc->graph_kind == GRAPH_DATA) {
            if (load_data(&calc->data[c], calc->formula[c].text)) {
                /*
                 * A file changed under this frame, so the view it is being
                 * drawn into is the old one. Fitted now and drawn properly on
                 * the next frame, which is one frame of the wrong scale after
                 * typing a path -- unnoticeable, and the alternative is
                 * working out the geometry twice in one draw.
                 */
                fit_to_data(calc);
                recon_appwin_refresh(calc->win);
            }
            draw_data_set(panel, &calc->data[c], ink, zero_x, zero_y,
                per_unit_x, per_unit_y, x, y, w, h);
            continue;
        }

        if (calc->graph_kind == GRAPH_POLAR) {
            draw_polar_curve(calc, panel, calc->formula[c].text, ink,
                zero_x, zero_y, per_unit_x, per_unit_y, x, y, w, h);
            continue;
        }

        if (calc->graph_kind == GRAPH_PARAM) {
            /* Both halves, or nothing: half a pair is not a curve, and
             * drawing the x against a y of zero would be a line somebody did
             * not ask for that looks like an answer. */
            if (calc->formula_y[c].text[0] == '\0' ||
                    !recon_expr_valid_named(calc->formula_y[c].text,
                        graph_variable(), NULL, 0)) {
                continue;
            }
            draw_param_curve(panel, calc->formula[c].text,
                calc->formula_y[c].text, ink, zero_x, zero_y, per_unit_x,
                per_unit_y, x, y, w, h);
            continue;
        }

        /*
         * One column of pixels at a time, joined to the last where joining
         * them is honest.
         *
         * `had_last` is what makes an asymptote look like an asymptote: a
         * column where the function has no value breaks the line, so the next
         * point starts a new stroke rather than being joined across the gap.
         * Without it 1/x is drawn with a vertical line down the y axis, which
         * is not part of the function and is the commonest way a grapher lies.
         */
        bool had_last = false;
        int last_y = 0;

        for (int col = 0; col < w - 2; col++) {
            /* Measured from where zero is rather than from the middle of the
             * box, which is what makes the curve move with the grid. */
            double vx = ((x + 1 + col) - zero_x) / per_unit_x;

            double vy = 0.0;
            if (recon_expr_eval_named(calc->formula[c].text,
                    graph_variable(), vx, &vy, NULL, 0) != RECON_EXPR_OK) {
                had_last = false;
                continue;
            }

            double py = zero_y - vy * per_unit_y;

            /* Off the top or bottom by a long way. Clamped rather than
             * skipped, so a curve leaving the window is drawn up to the edge
             * instead of stopping short of it. */
            if (py < y - h) {
                py = y - h;
            }
            if (py > y + h * 2) {
                py = y + h * 2;
            }
            int cy = (int)py;

            if (had_last) {
                int from = last_y < cy ? last_y : cy;
                int to = last_y < cy ? cy : last_y;

                /*
                 * A jump wider than the window is a break, not a slope. See
                 * the note on GRAPH_BREAK_FACTOR: this is what stops tan(x)
                 * being drawn as a row of walls.
                 */
                if (to - from > h * GRAPH_BREAK_FACTOR) {
                    had_last = false;
                } else {
                    for (int py2 = from; py2 <= to; py2++) {
                        if (py2 >= y + 1 && py2 < y + h - 1) {
                            recon_fill_rect(panel, x + 1 + col, py2, 1, 1,
                                ink);
                        }
                    }
                }
            }

            if (cy >= y + 1 && cy < y + h - 1) {
                recon_fill_rect(panel, x + 1 + col, cy, 1, 1, ink);
            }
            last_y = cy;
            had_last = true;
        }
    }

    if (complaining >= 0) {
        recon_fill_rect(panel, x + 8, y + h - 14 - ascent / 2, 10, line - 2,
            curve_color(complaining));
        recon_draw_text(panel, calc->font, x + 24,
            y + h - 10 - ascent / 2, w - 32, why, COLOR_WARNING);
    }

    /*
     * What reading the files came to, said out loud.
     *
     * This is the whole reason the data mode is a mode rather than an
     * afternoon. A plot that quietly drops the rows it could not read is a
     * picture of a data set nobody has, and it looks exactly like a picture of
     * the right one -- so the count of what did not go in is drawn next to the
     * thing that did.
     *
     * The first file with something to say is the one reported, the same rule
     * the expression complaint above follows, and its swatch says which.
     */
    if (calc->graph_kind == GRAPH_DATA) {
        for (int c = 0; c < GRAPH_CURVES; c++) {
            const struct data_set *set = &calc->data[c];
            if (calc->formula[c].text[0] == '\0') {
                continue;
            }

            /* Room for the whole path plus the sentence around it. The
             * path is what somebody typed and is what they would fix, so it
             * is named in full rather than by its last part. */
            char said[RECON_PATH_MAX + 96];
            if (set->read_failed) {
                snprintf(said, sizeof(said), "%s could not be read.",
                    calc->formula[c].text);
            } else if (set->bad > 0 && set->over > 0) {
                snprintf(said, sizeof(said),
                    "%d points. %d line%s were not two numbers, and %d more "
                    "would not fit.", set->count, set->bad,
                    set->bad == 1 ? "" : "s", set->over);
            } else if (set->bad > 0) {
                snprintf(said, sizeof(said),
                    "%d points. %d line%s %s not two numbers and %s left out.",
                    set->count, set->bad, set->bad == 1 ? "" : "s",
                    set->bad == 1 ? "was" : "were",
                    set->bad == 1 ? "was" : "were");
            } else if (set->over > 0) {
                snprintf(said, sizeof(said),
                    "%d points, which is all there is room for. %d more are "
                    "in the file.", set->count, set->over);
            } else if (set->comma_decimal && set->count > 0) {
                /* Every line went in, but not the way `strtod` would have
                 * read them. An interpretation nobody can see is a guess,
                 * however well founded. */
                snprintf(said, sizeof(said),
                    "%d points, read with a comma as the decimal point.",
                    set->count);
            } else if (set->count > 0) {
                continue;   /* Nothing to say: every line went in. */
            } else {
                snprintf(said, sizeof(said), "%s has no numbers in it.",
                    calc->formula[c].text);
            }

            recon_fill_rect(panel, x + 8, y + h - 14 - ascent / 2, 10,
                line - 2, curve_color(c));
            recon_draw_text(panel, calc->font, x + 24,
                y + h - 10 - ascent / 2, w - 32, said, COLOR_WARNING);
            break;
        }
    }

    /*
     * A Save, at the very end, once the plane holds everything it is going to.
     *
     * The note it leaves is drawn on the *next* frame -- this one has already
     * drawn where the note goes. That is a frame of delay on a message about a
     * file that has just been written, which nobody will see as a delay, and
     * it is the price of the picture being of what was on screen rather than
     * of a redraw arranged for the camera.
     */
    if (calc->want_save) {
        calc->want_save = false;
        save_graph(calc, panel);
        recon_appwin_refresh(calc->win);
    }
}

static void calc_draw(void *user, struct recon_panel *panel,
        int x, int y, int w, int h) {
    struct recon_calc *calc = user;
    int ascent = recon_font_ascent(calc->font);
    int line = recon_font_line_height(calc->font);

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
    if (calc->mode == CALC_GRAPH) {
        draw_graph_mode(calc, panel, dx, cy, dw, y + h - PAD_PADDING);

        /*
         * Over the whole content area rather than beside the plane: while it
         * is up it is the only thing that can be used, and covering what is
         * behind it is what makes that obvious rather than something to find
         * out by clicking.
         */
        if (recon_filedlg_is_open(&calc->picker)) {
            recon_filedlg_draw(&calc->picker, panel, calc->font, x, y, w, h);
        }
        return;
    }

    const struct calc_layout *layout = &LAYOUTS[calc->mode];
    if (layout->keys == NULL || layout->rows == 0) {
        return;
    }

    int pad_h = h - (cy - y) - PAD_PADDING;
    if (pad_h < layout->rows) {
        pad_h = layout->rows;
    }

    /*
     * Each edge computed from the whole span rather than from a single key
     * width multiplied out.
     *
     * A width of (dw - gaps) / cols throws away the remainder, and six columns
     * of it threw away up to five pixels -- so the keypad stopped short of the
     * right margin by a different amount in every mode, and the rightmost
     * column was visibly narrower than the rest. Dividing the span at each
     * boundary instead spreads that remainder one pixel at a time and lands
     * the last key exactly on the edge.
     */
    int span_w = dw + KEY_GAP;
    int span_h = pad_h + KEY_GAP;

    for (int row = 0; row < layout->rows; row++) {
        int ky = cy + span_h * row / layout->rows;
        int key_h = cy + span_h * (row + 1) / layout->rows - KEY_GAP - ky;
        if (key_h < 1) {
            key_h = 1;
        }

        for (int col = 0; col < layout->cols; col++) {
            const struct calc_key *key =
                &layout->keys[row * layout->cols + col];
            if (key->kind == KEY_NONE || key->label[0] == '\0') {
                continue;
            }

            int kx = dx + span_w * col / layout->cols;
            int key_w = dx + span_w * (col + 1) / layout->cols
                - KEY_GAP - kx;

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

            recon_fill_rect(panel, kx, ky, key_w, key_h, fill);
            /*
             * The button edge, not a bare bevel.
             *
             * These are buttons, so they round with the skin like every other
             * button in the system. Drawn with a flat bevel they were forty
             * rectangles sharing an edge -- which is what "they just kind of
             * exist" describes: nothing said where one key stopped and the
             * next began except a four-pixel gap.
             */
            recon_draw_button_edge(panel, kx, ky, key_w, key_h, false,
                COLOR_BG);

            /*
             * Centred, and clipped at the button's own right edge.
             *
             * The width passed used to be the key's full width measured from
             * the centred start, which runs past the button by however far the
             * label was indented -- so a label too wide for its key spilled
             * over the one beside it instead of being cut at the edge.
             */
            int label_w = recon_text_width(calc->font, key->label);
            int label_x = kx + (key_w - label_w) / 2;
            if (label_x < kx + 2) {
                label_x = kx + 2;
            }
            /*
             * The baseline centred by the line's own height.
             *
             * It was (key_h + ascent) / 2 - 2, which centres correctly only
             * when the descent happens to be four pixels. Every other font
             * size sat the label low in its key by half the difference, and
             * "the text is messed up" is what that looks like when the keys
             * beside each other are tall enough to notice.
             */
            recon_draw_text(panel, calc->font, label_x,
                ky + (key_h - line) / 2 + ascent,
                kx + key_w - 2 - label_x, key->label, text);

            /* Hit ids encode the position, so drawing and input cannot drift
             * out of step. */
            recon_hit_add(panel, kx, ky, key_w, key_h,
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

/*
 * Put the chosen path in the field it was chosen for.
 *
 * Into the field rather than straight into the reader, because the field is
 * where a path lives in this mode -- somebody can then see it, edit it, and
 * keep it, and it is written to the settings with the other two. A picker that
 * loaded a file without showing which one would leave the mode with a plot and
 * no way to say where it came from.
 */
static void data_file_chosen(struct recon_calc *calc) {
    const char *path = recon_filedlg_path(&calc->picker);
    if (path == NULL || *path == '\0') {
        return;
    }
    if (calc->picking_for < 0 || calc->picking_for >= GRAPH_CURVES) {
        return;
    }

    recon_edit_begin(&calc->formula[calc->picking_for], path, false);
    remember_graph(calc);
}

static bool calc_click(void *user, uint32_t hit_id, int cx, int cy, bool pressed) {
    struct recon_calc *calc = user;
    (void)cx;
    (void)cy;

    /*
     * A release ends a drag wherever it lands.
     *
     * Before the `!pressed` refusal below, because the pointer is usually not
     * over the plane by the time the button comes up -- a drag that ends off
     * the edge of the picture would otherwise leave the plane still following
     * the mouse with nothing held down.
     */
    if (!pressed && calc->panning) {
        calc->panning = false;
        return true;
    }

    /*
     * The picker takes every click while it is up, including the ones that
     * miss it -- the graph behind is not usable until it is answered. Before
     * the refusal below because its own ids are above this window's, which is
     * what RECON_FILEDLG_IDS_BELOW exists to keep true.
     */
    if (pressed && recon_filedlg_is_open(&calc->picker)) {
        if (recon_filedlg_click(&calc->picker, hit_id) ==
                RECON_FILEDLG_ACCEPTED) {
            data_file_chosen(calc);
        }
        recon_appwin_refresh(calc->win);
        return true;
    }

    if (!pressed || hit_id < RECON_APPWIN_HIT_USER) {
        return false;
    }

    /*
     * The graph's controls first. HIT_DATE_FIELD below is matched with an
     * open-ended >= and these are numbered above it, so anything checked after
     * it would be read as a date field.
     */
    /*
     * A curve's field. Bounded and checked before the switch, for the reason
     * the switch itself is checked before HIT_DATE_FIELD: these are numbered
     * above it and it is matched with an open-ended `>=`.
     */
    if (hit_id >= HIT_GRAPH_FIELD_BASE &&
            hit_id < HIT_GRAPH_FIELD_BASE + GRAPH_CURVES * 2) {
        int i = (int)(hit_id - HIT_GRAPH_FIELD_BASE);
        /* Every field, not the three of this mode: a caret left on a
         * parametric y after switching back would be drawn on a box that is
         * not there. */
        for (int j = 0; j < GRAPH_CURVES * 2; j++) {
            graph_field(calc, j)->active = (j == i);
        }
        calc->formula_focused = i;
        recon_edit_focus(graph_field(calc, i));
        return true;
    }

    if (hit_id >= HIT_GRAPH_PICK_BASE &&
            hit_id < HIT_GRAPH_PICK_BASE + GRAPH_CURVES) {
        calc->picking_for = (int)(hit_id - HIT_GRAPH_PICK_BASE);

        /* Starting where the field points, so a second choice opens beside
         * the first rather than back at the top every time. */
        const char *from = calc->formula[calc->picking_for].text;
        recon_filedlg_open(&calc->picker, RECON_FILEDLG_OPEN,
            "Choose a data file", from[0] != '\0' ? from : NULL, NULL);
        return true;
    }

    switch (hit_id) {
    case HIT_GRAPH_PLANE:
        /*
         * A press on the plane begins a drag. The centre at this moment is
         * remembered rather than updated as the pointer moves, so the point
         * under the pointer stays under it.
         */
        calc->panning = true;
        calc->pan_from_x = cx;
        calc->pan_from_y = cy;
        calc->pan_centre_x = calc->centre_x;
        calc->pan_centre_y = calc->centre_y;
        return true;
    case HIT_GRAPH_IN:
        /* Halved rather than stepped by a fixed amount, so zooming in and out
         * the same number of times comes back to where it started. */
        calc->span_x /= 2.0;
        calc->span_y /= 2.0;
        return true;
    case HIT_GRAPH_OUT:
        calc->span_x *= 2.0;
        calc->span_y *= 2.0;
        return true;
    case HIT_GRAPH_XY:
    case HIT_GRAPH_POLAR:
    case HIT_GRAPH_PARAM:
    case HIT_GRAPH_DATA: {
        enum graph_kind wanted = GRAPH_XY;
        if (hit_id == HIT_GRAPH_POLAR) {
            wanted = GRAPH_POLAR;
        } else if (hit_id == HIT_GRAPH_PARAM) {
            wanted = GRAPH_PARAM;
        } else if (hit_id == HIT_GRAPH_DATA) {
            wanted = GRAPH_DATA;
        }
        if (calc->graph_kind == wanted) {
            return true;
        }
        calc->graph_kind = wanted;

        /*
         * The expressions are kept across the change; the view is not.
         *
         * Opposite decisions, for the same reason. sin(x) is a wave and
         * sin(t) is a circle, and seeing the same three lines mean something
         * else is the quickest way to understand what the mode is -- so the
         * text stays.
         *
         * The scale cannot stay. Twenty units wide is right for y = x^2 and
         * draws every ordinary polar curve as a thumbnail; two and a half is
         * right for a rose and shows almost nothing of a parabola. Carrying
         * the old view across would make the mode look broken in whichever
         * direction you switched.
         */
        calc->span_x = (wanted == GRAPH_XY) ? GRAPH_SPAN : GRAPH_SPAN_POLAR;
        calc->span_y = calc->span_x;
        calc->centre_x = 0.0;
        calc->centre_y = 0.0;

        /* A caret on a field this mode does not show would be drawn on
         * nothing. Back to the first, which every mode has. */
        if (calc->formula_focused >= graph_field_count(calc)) {
            graph_field(calc, calc->formula_focused)->active = false;
            calc->formula_focused = 0;
        }

        calc->note[0] = '\0';
        remember_graph(calc);
        return true;
    }

    case HIT_GRAPH_SAVE:
        /* Handled in the draw, where there is a panel to read from. A click
         * has no panel of its own, and reading the one from the last frame
         * would save the graph as it was before whatever this click changed. */
        calc->want_save = true;
        return true;
    case HIT_GRAPH_HOME:
        /* Back to where it started, which is the span AND the place -- a
         * Reset that left the view somewhere in the third quadrant would be
         * resetting half of what somebody had changed. */
        if (calc->graph_kind == GRAPH_DATA) {
            /* Back to the data, which is what "the starting view" means when
             * the plane is showing a file rather than a function. */
            fit_to_data(calc);
            return true;
        }
        /* Back to this mode's own scale, not to the other one's. */
        calc->span_x = (calc->graph_kind == GRAPH_POLAR)
            ? GRAPH_SPAN_POLAR : GRAPH_SPAN;
        calc->span_y = calc->span_x;
        calc->centre_x = 0.0;
        calc->centre_y = 0.0;
        return true;
    default:
        break;
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

    /*
     * The picker first, and it takes everything -- including the keys it does
     * not use. Typing into the expression behind a dialog that is waiting for
     * an answer is how a filename ends up halfway through somebody's formula.
     */
    if (recon_filedlg_is_open(&calc->picker)) {
        if (recon_filedlg_key(&calc->picker, sym, modifiers) ==
                RECON_FILEDLG_ACCEPTED) {
            data_file_chosen(calc);
        }
        recon_appwin_refresh(calc->win);
        return true;
    }

    /*
     * Graph mode types an expression rather than arithmetic, so every key goes
     * to the field. That includes the digits and the operators, which
     * everywhere else in this application are buttons -- and here they are
     * characters in "x^2-2".
     */
    if (calc->mode == CALC_GRAPH) {
        /*
         * Tab moves between the three curves, which is what Tab does on every
         * other form in the system. Without it the second and third fields can
         * only be reached with the mouse, which makes them feel like an extra
         * rather than like part of the same thing.
         */
        if (sym == XKB_KEY_Tab) {
            graph_field(calc, calc->formula_focused)->active = false;
            calc->formula_focused =
                (calc->formula_focused + 1) % graph_field_count(calc);
            recon_edit_focus(graph_field(calc, calc->formula_focused));
            return true;
        }

        switch (recon_edit_key(graph_field(calc, calc->formula_focused), sym,
                modifiers)) {
        case RECON_EDIT_CHANGED:
        case RECON_EDIT_COMMIT:
        case RECON_EDIT_CANCEL:
            remember_graph(calc);
            /* The note is about a picture of the old expression. */
            calc->note[0] = '\0';
            return true;
        case RECON_EDIT_IGNORED:
            return false;
        }
        return false;
    }

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

/*
 * Moving the plane while the button is held.
 *
 * Measured from where the drag began rather than from the last position, so
 * the point that was under the pointer stays under it: accumulating small
 * steps drifts, and a picture that slides out from under the finger holding it
 * feels broken in a way that is hard to name.
 *
 * Dragging right moves the view LEFT -- the plane comes with the pointer, the
 * way a map does. The opposite is technically also a choice and is the one
 * nobody expects.
 */
static void calc_motion(void *user, uint32_t hit_id, int cx, int cy) {
    struct recon_calc *calc = user;

    /* Remembered whether or not anything is being dragged, because the wheel
     * needs it and the wheel arrives with no coordinates. */
    if (hit_id == HIT_GRAPH_PLANE && calc->per_unit > 0.0) {
        calc->pointer_x = calc->centre_x +
            (cx - calc->plane_mid_x) / calc->per_unit;
        calc->pointer_y = calc->centre_y -
            (cy - calc->plane_mid_y) / calc->per_unit;
    }

    if (!calc->panning || calc->per_unit <= 0.0) {
        return;
    }
    calc->centre_x = calc->pan_centre_x -
        (cx - calc->pan_from_x) / calc->per_unit;
    calc->centre_y = calc->pan_centre_y +
        (cy - calc->pan_from_y) / calc->per_unit;

    /* And ask to be drawn. See the note on `win`. */
    recon_appwin_refresh(calc->win);
}

/* The plane says it can be dragged, because a thing that can be dragged and
 * looks exactly like a thing that cannot is a thing nobody drags. */
static const char *calc_cursor(void *user, uint32_t hit_id) {
    (void)user;
    return (hit_id == HIT_GRAPH_PLANE) ? "grab" : NULL;
}

/*
 * The wheel zooms, about the point under the pointer.
 *
 * About the pointer rather than about the middle, because that is what makes
 * zooming a way of looking closer at *something*: zooming about the middle
 * means finding a feature, dragging it to the centre, zooming, and finding it
 * has moved again. The arithmetic is one line -- keep the value under the
 * pointer where it is -- and it is the difference between a grapher somebody
 * explores with and one they fight.
 */
static void calc_scroll(void *user, double delta) {
    struct recon_calc *calc = user;

    if (calc->mode != CALC_GRAPH || calc->per_unit <= 0.0) {
        return;
    }

    /*
     * Wheel away from you zooms in.
     *
     * `delta > 0` is a scroll DOWN -- that is what it means everywhere else in
     * this system, where a positive delta moves a list further down. So the
     * test reads inverted and is not: down is out, up is in, which is what a
     * map does and what everybody expects. Written the obvious way round first
     * and the zoom went the wrong way, which is why the direction is stated
     * here rather than left to be inferred from the sign.
     */
    double before = calc->span_x;
    if (delta < 0) {
        calc->span_x /= GRAPH_ZOOM_STEP;
    } else {
        calc->span_x *= GRAPH_ZOOM_STEP;
    }

    /*
     * A floor and a ceiling, because both ends stop meaning anything. Zoomed
     * in past this the arithmetic runs out of precision and the curve goes
     * jagged; zoomed out past it every function is a horizontal line.
     */
    if (calc->span_x < 1e-6) {
        calc->span_x = 1e-6;
    }
    if (calc->span_x > 1e9) {
        calc->span_x = 1e9;
    }

    /* Keep the value under the pointer where it is. `pointer_x` and
     * `pointer_y` are in units, worked out by the motion handler. */
    double scale = calc->span_x / before;
    calc->centre_x = calc->pointer_x + (calc->centre_x - calc->pointer_x) * scale;
    calc->centre_y = calc->pointer_y + (calc->centre_y - calc->pointer_y) * scale;

    recon_appwin_refresh(calc->win);
}

static void calc_destroy(void *user) {
    free(user);
}

static const struct recon_appwin_impl CALC_IMPL = {
    .title = "Calculator",
    /*
     * F1 goes to the page about this, like every other window. A module's
     * application is not a lesser one: it names a topic in the system's help
     * the same way a built-in does, and the shell says so in the log if the
     * page is not there.
     */
    .help = "The Calculator",
    .icon = RECON_ICON_CALCULATOR,
    /*
     * Wide enough for five mode names and a six-column scientific keypad.
     * It was 260, which fitted the four-function keypad exactly and truncated
     * every tab label the moment there were tabs.
     *
     * Then it was 430, which was BELOW the 520 minimum below -- so the window
     * opened at a size it would refuse to be resized to, six tabs wrapped onto
     * a second row on a window nobody had touched yet, and the keys were as
     * cramped as the wrapping made them. This is the pair to check together:
     * a default under the minimum is a window that opens wrong and is only
     * right once somebody drags it.
     *
     * The height leaves the tallest keypad -- programmer mode's nine rows --
     * keys that are wider than they are tall rather than the other way round.
     */
    .default_width = 560,
    .default_height = 520,
    /*
     * Wide enough for six mode tabs on one row.
     *
     * Three hundred and eighty fitted five. The sixth pushed "Programmer" off
     * the end, and a window whose minimum size cannot show its own controls is
     * a minimum that was measured against an older version of itself.
     */
    .min_width = 520,
    .min_height = 320,
    .draw = calc_draw,
    .click = calc_click,
    .key = calc_key_press,
    .motion = calc_motion,
    .cursor = calc_cursor,
    .scroll = calc_scroll,
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

    /*
     * Ten units either way to begin with, which puts sin(x) through more than
     * three periods and x^2 through its interesting part without anybody
     * having to zoom before they see anything.
     */
    calc->span_x = GRAPH_SPAN;
    calc->span_y = GRAPH_SPAN;
    /*
     * What was typed last time, or an example.
     *
     * The example only when there is nothing remembered at all -- somebody who
     * deliberately cleared all three fields and closed the window should not
     * be handed sin(x) back on the way in, which would look like the clearing
     * not having worked.
     */
    bool remembered = false;
    for (int i = 0; i < GRAPH_CURVES; i++) {
        char key[64];
        graph_key(i, key, sizeof(key));
        const char *kept = recon_registry_get(RECON_REG_USER, key, "");

        recon_edit_begin(&calc->formula[i], kept, false);
        calc->formula[i].active = false;
        if (kept[0] != '\0') {
            remembered = true;
        }

        char y_key[64];
        snprintf(y_key, sizeof(y_key), "%s%d", GRAPH_Y_KEY_PREFIX, i);
        const char *kept_y = recon_registry_get(RECON_REG_USER, y_key, "");
        recon_edit_begin(&calc->formula_y[i], kept_y, false);
        calc->formula_y[i].active = false;
        if (kept_y[0] != '\0') {
            remembered = true;
        }
    }

    if (!remembered &&
            !recon_registry_has(RECON_REG_USER, GRAPH_KEY_PREFIX "0")) {
        recon_edit_begin(&calc->formula[0], "sin(x)", false);
        calc->formula[0].active = false;
    }
    const char *kept_kind = recon_registry_get(RECON_REG_USER,
        GRAPH_KIND_KEY, "xy");
    calc->graph_kind = GRAPH_XY;
    if (strcmp(kept_kind, "polar") == 0) {
        calc->graph_kind = GRAPH_POLAR;
    } else if (strcmp(kept_kind, "parametric") == 0) {
        calc->graph_kind = GRAPH_PARAM;
    } else if (strcmp(kept_kind, "data") == 0) {
        calc->graph_kind = GRAPH_DATA;
    }

    calc->formula_focused = 0;
    calc->unit_to = 1;

    /* Both dates start at today, so the difference starts at zero and moves
     * from somewhere real rather than from the first of January 1970. */
    struct recon_clock_time now;
    recon_clock_now(&now);
    calc->from_year = calc->to_year = now.year;
    calc->from_month = calc->to_month = now.month;
    calc->from_day = calc->to_day = now.day;

    clear_all(calc);

    calc->win = recon_appwin_create(server, font, &CALC_IMPL, calc);
    if (calc->win == NULL) {
        free(calc);
        return NULL;
    }
    return calc->win;
}

/* --- The module --- */

/*
 * The calculator is an application ReconOS loads rather than one it contains.
 *
 * It is here as the first thing to go through the module path, and it is a
 * good first thing: entirely self-contained, useful, and nothing else depends
 * on it. If it fails to load the user loses a calculator, not a desktop.
 */
/*
 * One version, named once.
 *
 * The module carries a version and so does the application inside it, and the
 * two are the same thing said twice -- which is the shape where they drift,
 * because bumping one and forgetting the other costs nothing at the time and
 * shows up later as a module that says 2.0 registering an applet that says
 * 1.0.
 *
 * 2.0.0: five modes and the converters. 1.0 was the four-function keypad.
 */
#define CALCULATOR_VERSION "2.0.0"

static bool calculator_load(void) {
    static const struct recon_app_registration APP = {
        .name = "Calculator",
        .icon = RECON_ICON_CALCULATOR,
        .create = recon_calc_create,
        .in_menu = true,
        .version = CALCULATOR_VERSION,
    };
    return recon_register_app(&APP);
}

static void calculator_unload(void) {
    recon_unregister_app("Calculator");
}

RECON_MODULE(
    .name = "Calculator",
    .version = CALCULATOR_VERSION,
    .description = "Arithmetic by mouse or keyboard, in five modes",
    .load = calculator_load,
    .unload = calculator_unload,
);
