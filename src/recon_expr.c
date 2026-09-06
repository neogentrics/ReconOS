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
 *   term       := unary (('*' | '/') unary)*
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

static double parse_primary(struct parser *p) {
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

        if (strcmp(name, "x") == 0) {
            return p->x;
        }
        if (strcmp(name, "pi") == 0) {
            return 3.14159265358979323846;
        }
        if (strcmp(name, "e") == 0) {
            return 2.71828182845904523536;
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
    struct parser p;
    memset(&p, 0, sizeof(p));
    p.at = text != NULL ? text : "";
    p.x = x;

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
