/*
 * Parsing and evaluating an expression. See include/recon_expr.h.
 *
 * A recursive-descent parser that evaluates as it goes, rather than building a
 * tree and walking it. For expressions of this size the tree would be an
 * allocation per node to hold something used once -- and the grapher calls this
 * a few hundred times a redraw, so the version that allocates nothing is the
 * one worth having.
 *
 * The grammar, lowest binding first:
 *
 *   expression := term (('+' | '-') term)*
 *   term       := unary (('*' | '/' | nothing at all) unary)*
 *   unary      := ('-' | '+') unary | power
 *   power      := primary ('^' unary)?
 *   primary    := number | 'x' | name '(' expression ')' | '(' expression ')'
 *
 * `^` binds tighter than a minus on its left and looser than one on its right,
 * so -2^2 is -(2^2) and 2^-1 is a half. That is what every calculator and every
 * mathematician means by it, and getting it the other way round is the usual
 * mistake -- which is why it is spelled out here and checked in the tests.
 *
 * The shape that produces it is unary sitting ABOVE power rather than inside
 * it, and power taking a unary on its right. The first version of this file had
 * them the other way round, with this same paragraph above it describing the
 * behaviour it did not have: -2^2 came to 4.
 *
 * --- Multiplication nobody wrote ---
 *
 * `3t` is 3*t, `2pi` is 2*pi, `2(x+1)` and `(x+1)(x-1)` and `3sin(x)` all mean
 * what they look like. Everybody writes them that way and the grapher's own
 * hint had to say "sin(3*t)" rather than "sin(3t)", which is teaching somebody
 * the one form that does not work.
 *
 * The rule is one line: **a value has just finished and the next character
 * begins another one.** A number, the variable, a constant or a closing bracket
 * ends a value; a digit, a letter or an opening bracket begins one. Nothing
 * else multiplies, so `2 3` is still two numbers with nothing between them and
 * is still refused.
 *
 * What it does *not* do is join two names. `xt` is one name, is not something
 * this knows, and says so -- there is exactly one variable, so `xt` was never
 * going to be x times t, and reading it that way would make every misspelled
 * function name silently become a product.
 *
 * The ambiguity worth naming is `2e3`. Scientific notation and "two times e
 * cubed" are the same six characters, and this settles it the way everybody
 * expects without a rule of its own: the number reader is greedy about
 * exponents that are *valid*, so `2e3` is read whole as 2000, and `2e` is read
 * as 2 with an `e` left over -- which the line above then multiplies. Both
 * answers are the ones somebody typing them meant.
 */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>   /* strtod */
#include <string.h>

#include "recon_expr.h"

struct parser {
    const char *at;
    double x;
    /* What the variable is called here, lowercased, or empty for just `x`.
     * Sixteen because that is what the name buffer in parse_primary holds,
     * and a name longer than it could match would never be read. */
    char name[16];

    /*
     * True when the last thing read was a complete value.
     *
     * Which is the whole of how `3t` is told from `3 * t` and from `2 3`: a
     * value having just ended, and a value beginning next, is a multiplication
     * nobody wrote down. Set in parse_primary, where a value is what is being
     * read, and cleared by every operator that takes one.
     */
    bool ended_a_value;

    /* Set the moment something is wrong, and never cleared -- the first
     * complaint is the useful one, and carrying on to find a second means
     * describing wreckage caused by the first. */
    bool bad;
    bool undefined;
    char why[128];
};

static void fail(struct parser *p, const char *what) {
    if (!p->bad) {
        p->bad = true;
        snprintf(p->why, sizeof(p->why), "%s", what);
    }
}

static void skip_spaces(struct parser *p) {
    while (*p->at == ' ' || *p->at == '\t') {
        p->at++;
    }
}

static bool take(struct parser *p, char c) {
    skip_spaces(p);
    if (*p->at == c) {
        p->at++;
        return true;
    }
    return false;
}

static double parse_expression(struct parser *p);

/*
 * A result that is not a number is undefined here rather than wrong.
 *
 * Every function below funnels through this, so there is one place that
 * decides what an infinity means -- and it means the curve has no point at this
 * x, which is what lets a grapher leave a gap instead of drawing a wall.
 */
static double checked(struct parser *p, double v) {
    if (!isfinite(v)) {
        p->undefined = true;
        return 0.0;
    }
    return v;
}

static double call_function(struct parser *p, const char *name, double arg) {
    struct entry {
        const char *name;
        double (*fn)(double);
    };
    static const struct entry FUNCTIONS[] = {
        { "sin", sin }, { "cos", cos }, { "tan", tan },
        { "asin", asin }, { "acos", acos }, { "atan", atan },
        { "sqrt", sqrt }, { "cbrt", cbrt },
        { "ln", log }, { "log", log10 }, { "exp", exp },
        { "abs", fabs }, { "floor", floor }, { "ceil", ceil },
        { "round", round },
    };

    for (size_t i = 0; i < sizeof(FUNCTIONS) / sizeof(FUNCTIONS[0]); i++) {
        if (strcmp(FUNCTIONS[i].name, name) == 0) {
            /*
             * The domain is not checked before calling. Every one of these
             * returns NaN outside its domain, and `checked` turns that into
             * "no value here" -- which is the same answer a hand-written check
             * would give, arrived at without a second table of domains to
             * disagree with the library's.
             */
            return checked(p, FUNCTIONS[i].fn(arg));
        }
    }

    char message[128];
    snprintf(message, sizeof(message), "There is no function called '%s'.",
        name);
    fail(p, message);
    return 0.0;
}

static double parse_primary_value(struct parser *p);

/*
 * A value, and a note that one was just read.
 *
 * The note is what parse_term uses to tell `3t` from `3 * t` and from an
 * operator with nothing after it. Kept here, wrapping the reading, rather than
 * set at each of the five places a value can be read -- which is five places
 * to forget it, and forgetting it in one of them would make implicit
 * multiplication work everywhere except after a function call.
 */
static double parse_primary(struct parser *p) {
    p->ended_a_value = false;
    double v = parse_primary_value(p);
    p->ended_a_value = !p->bad;
    return v;
}

static double parse_primary_value(struct parser *p) {
    skip_spaces(p);

    if (take(p, '(')) {
        double v = parse_expression(p);
        if (!take(p, ')')) {
            fail(p, "A bracket was opened and not closed.");
        }
        return v;
    }

    if (isdigit((unsigned char)*p->at) || *p->at == '.') {
        char *end = NULL;
        double v = strtod(p->at, &end);
        if (end == p->at) {
            fail(p, "That is not a number.");
            return 0.0;
        }
        p->at = end;
        return v;
    }

    if (isalpha((unsigned char)*p->at)) {
        char name[16];
        size_t n = 0;
        while (isalpha((unsigned char)*p->at) && n < sizeof(name) - 1) {
            name[n++] = (char)tolower((unsigned char)*p->at);
            p->at++;
        }
        name[n] = '\0';

        /*
         * `x` is always the variable. So is whatever the caller called it --
         * but only after the constants have had their turn.
         *
         * The order matters, and it is the header's promise: a caller who
         * renames the variable to `e` gets the constant, because `e` silently
         * ceasing to mean 2.718 inside every expression in that field is a far
         * worse surprise than a rename that did not take.
         *
         * Written the other way round first, and the test for it failed. That
         * is how the code and the header came to agree -- not by reading
         * either of them.
         *
         * `x` stays accepted whatever the variable is called: there is only
         * one variable, so sin(3x) typed into a field labelled r is
         * unambiguous, and refusing it would be pedantry.
         */
        if (strcmp(name, "x") == 0) {
            return p->x;
        }
        if (strcmp(name, "pi") == 0) {
            return 3.14159265358979323846;
        }
        if (strcmp(name, "e") == 0) {
            return 2.71828182845904523536;
        }
        if (p->name[0] != '\0' && strcmp(name, p->name) == 0) {
            return p->x;
        }

        if (!take(p, '(')) {
            char message[128];
            snprintf(message, sizeof(message),
                "'%s' is not something this knows.", name);
            fail(p, message);
            return 0.0;
        }
        double arg = parse_expression(p);
        if (!take(p, ')')) {
            fail(p, "A function's bracket was opened and not closed.");
            return 0.0;
        }
        return call_function(p, name, arg);
    }

    if (*p->at == '\0') {
        fail(p, "The expression stops before it says anything.");
    } else {
        char message[128];
        snprintf(message, sizeof(message), "'%c' does not belong here.",
            *p->at);
        fail(p, message);
    }
    return 0.0;
}

static double parse_power(struct parser *p);

static double parse_unary(struct parser *p) {
    skip_spaces(p);
    if (take(p, '-')) {
        return -parse_unary(p);
    }
    if (take(p, '+')) {
        return parse_unary(p);
    }
    return parse_power(p);
}

static double parse_power(struct parser *p) {
    double base = parse_primary(p);
    skip_spaces(p);
    if (take(p, '^')) {
        /*
         * A unary on the right, which does two things at once.
         *
         * It makes the operator right-associative -- 2^3^2 is 2^(3^2), the way
         * it is written on paper, rather than 64 -- because the right-hand
         * side comes back through here. And it lets 2^-1 be a half without
         * brackets, because a minus is allowed to start it.
         */
        double exponent = parse_unary(p);
        return checked(p, pow(base, exponent));
    }
    return base;
}

/*
 * Does a value begin here?
 *
 * Deliberately not "is this not an operator": `)` and the end of the text are
 * neither, and treating them as the start of a value would multiply by
 * whatever came next across a bracket.
 */
static bool a_value_begins(char c) {
    /*
     * A letter or an opening bracket. **Not a digit.**
     *
     * `2x` and `2(x+1)` are things people write. `2 3` is a typo -- nobody
     * writes six that way -- and it was refused before this and stays refused,
     * with the message it already had. Letting implicit multiplication swallow
     * it would turn a mistake somebody can see into an answer they cannot
     * question.
     */
    return isalpha((unsigned char)c) || c == '(';
}

static double parse_term(struct parser *p) {
    double left = parse_unary(p);

    for (;;) {
        skip_spaces(p);
        if (take(p, '*')) {
            left = checked(p, left * parse_unary(p));
        } else if (take(p, '/')) {
            double right = parse_unary(p);
            if (right == 0.0) {
                /*
                 * Not an error. The expression is fine and has no value at
                 * this x, which is exactly what an asymptote is -- so this
                 * notes it and carries on parsing.
                 *
                 * The first version returned here, after consuming the rest
                 * with a recursive call. That call started by asking for an
                 * operand, found the end of the text, and reported a parse
                 * error -- so "1/x" at zero came back as a broken expression
                 * rather than an undefined one, which is the exact distinction
                 * this file exists to make.
                 */
                p->undefined = true;
                left = 0.0;
            } else {
                left = checked(p, left / right);
            }
        } else if (p->ended_a_value && a_value_begins(*p->at)) {
            /*
             * Nothing between two values means multiply.
             *
             * `ended_a_value` is set by parse_primary and is what keeps this
             * from firing after an operator: in `2 * 3` the `*` is taken
             * above, and in `2 +` there is no value to the left of the space.
             * Without it this would read `sin` in `2 * sin(x)` as a second
             * factor and multiply twice.
             */
            left = checked(p, left * parse_unary(p));
        } else {
            return left;
        }
        if (p->bad) {
            return 0.0;
        }
    }
}

static double parse_expression(struct parser *p) {
    double left = parse_term(p);

    for (;;) {
        skip_spaces(p);
        if (take(p, '+')) {
            left = checked(p, left + parse_term(p));
        } else if (take(p, '-')) {
            left = checked(p, left - parse_term(p));
        } else {
            return left;
        }
        if (p->bad) {
            return 0.0;
        }
    }
}

enum recon_expr_result recon_expr_eval(const char *text, double x,
        double *out, char *why, size_t why_size) {
    return recon_expr_eval_named(text, NULL, x, out, why, why_size);
}

bool recon_expr_valid_named(const char *text, const char *name, char *why,
        size_t why_size) {
    return recon_expr_eval_named(text, name, 1.0, NULL, why, why_size)
        != RECON_EXPR_BAD;
}

enum recon_expr_result recon_expr_eval_named(const char *text,
        const char *name, double x, double *out, char *why,
        size_t why_size) {
    struct parser p;
    memset(&p, 0, sizeof(p));
    p.at = text != NULL ? text : "";
    p.x = x;

    /* Lowercased once here rather than at every use, because parse_primary
     * lowercases what it reads and comparing the two any other way would make
     * `T` and `t` different variables. */
    if (name != NULL) {
        size_t i = 0;
        for (; name[i] != '\0' && i + 1 < sizeof(p.name); i++) {
            p.name[i] = (char)tolower((unsigned char)name[i]);
        }
        p.name[i] = '\0';
    }

    if (*p.at == '\0') {
        if (why != NULL && why_size > 0) {
            snprintf(why, why_size, "There is nothing to work out.");
        }
        return RECON_EXPR_BAD;
    }

    double value = parse_expression(&p);

    /* Anything left over means the expression ended and the text did not,
     * which is a mistake rather than something to ignore -- "2 + 3 )" should
     * not quietly come to five. */
    if (!p.bad) {
        skip_spaces(&p);
        if (*p.at != '\0') {
            char message[128];
            snprintf(message, sizeof(message),
                "There is '%s' left over at the end.", p.at);
            fail(&p, message);
        }
    }

    if (p.bad) {
        if (why != NULL && why_size > 0) {
            snprintf(why, why_size, "%s", p.why);
        }
        return RECON_EXPR_BAD;
    }

    /*
     * Undefined is checked after parsing rather than during, so an expression
     * that is undefined somewhere inside it still reports as an expression.
     * The caller asked what it comes to at this x; the answer is "nothing",
     * and it is not a complaint about the text.
     */
    if (p.undefined || !isfinite(value)) {
        return RECON_EXPR_UNDEFINED;
    }

    if (out != NULL) {
        *out = value;
    }
    return RECON_EXPR_OK;
}

bool recon_expr_valid(const char *text, char *why, size_t why_size) {
    return recon_expr_eval(text, 1.0, NULL, why, why_size) != RECON_EXPR_BAD;
}
