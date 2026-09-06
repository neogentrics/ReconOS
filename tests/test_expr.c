/*
 * The expression evaluator, against answers worked out by hand.
 *
 * Most of these are arithmetic anybody can check, which is the point: a test
 * whose expected values came from running the code proves the code agrees with
 * itself. Where the answer is not obvious -- precedence, associativity, what
 * happens at an asymptote -- the reason is written down beside it.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "recon_expr.h"

static int checks = 0;
static int failures = 0;

static void check(bool ok, const char *what) {
    checks++;
    if (ok) {
        printf("  ok    %s\n", what);
    } else {
        failures++;
        printf("  FAIL  %s\n", what);
    }
}

/* Compared with a tolerance, because these are doubles and 0.1 + 0.2 is not
 * 0.3 in any language. */
static bool comes_to(const char *text, double x, double want) {
    double got = 0.0;
    if (recon_expr_eval(text, x, &got, NULL, 0) != RECON_EXPR_OK) {
        printf("        \"%s\" at x=%g did not evaluate\n", text, x);
        return false;
    }
    if (fabs(got - want) > 1e-9 * (1.0 + fabs(want))) {
        printf("        \"%s\" at x=%g came to %.17g, wanted %.17g\n",
            text, x, got, want);
        return false;
    }
    return true;
}

static void test_arithmetic(void) {
    printf("Arithmetic\n");

    check(comes_to("1+1", 0, 2), "one and one");
    check(comes_to("7-9", 0, -2), "and taking away more than there is");
    check(comes_to("6*7", 0, 42), "multiplying");
    check(comes_to("1/8", 0, 0.125), "and dividing");
    check(comes_to("2.5*4", 0, 10), "a decimal point");
    check(comes_to(".5+.5", 0, 1), "a number that starts with one");
    check(comes_to("1e3", 0, 1000), "and one written in exponent form");
    check(comes_to("  3  +  4  ", 0, 7), "spaces anywhere");
}

static void test_precedence(void) {
    printf("What binds tighter than what\n");

    check(comes_to("2+3*4", 0, 14), "times before plus");
    check(comes_to("(2+3)*4", 0, 20), "and brackets before either");
    check(comes_to("2*3^2", 0, 18), "power before times");

    /*
     * The two that are usually wrong, and they are wrong in opposite
     * directions, so getting both right is evidence the rule is the real one
     * rather than a special case.
     */
    check(comes_to("-2^2", 0, -4),
        "a minus outside a power applies after it: -(2^2), not (-2)^2");
    check(comes_to("2^3^2", 0, 512),
        "and powers group to the right: 2^(3^2), not (2^3)^2");

    check(comes_to("2^-1", 0, 0.5), "a negative exponent needs no brackets");
    check(comes_to("--5", 0, 5), "two minuses");
    check(comes_to("10-2-3", 0, 5), "subtraction groups to the left");
    check(comes_to("100/10/2", 0, 5), "and so does division");
}

static void test_x_and_constants(void) {
    printf("The variable and the constants\n");

    check(comes_to("x", 3, 3), "x is what it was given");
    check(comes_to("x*x", 5, 25), "twice over");
    check(comes_to("2*x+1", 4, 9), "and in an expression");
    check(comes_to("x^2-4", -3, 5), "with a negative x");
    check(comes_to("pi", 0, 3.14159265358979323846), "pi");
    check(comes_to("e", 0, 2.71828182845904523536), "e");
    check(comes_to("PI", 0, 3.14159265358979323846), "and case does not matter");
}

static void test_functions(void) {
    printf("Functions\n");

    check(comes_to("sin(0)", 0, 0), "sin of nothing");
    check(comes_to("cos(0)", 0, 1), "cos of nothing");
    check(comes_to("sin(pi/2)", 0, 1), "sin of a right angle, in radians");
    check(comes_to("sqrt(16)", 0, 4), "a square root");
    check(comes_to("abs(0-7)", 0, 7), "an absolute value");
    check(comes_to("ln(e)", 0, 1), "a natural logarithm");
    check(comes_to("log(1000)", 0, 3), "and a base-ten one");
    check(comes_to("exp(0)", 0, 1), "e to the nothing");
    check(comes_to("floor(2.7)", 0, 2), "rounding down");
    check(comes_to("ceil(2.1)", 0, 3), "and up");
    check(comes_to("sqrt(x^2)", -4, 4), "a function of x");
    check(comes_to("sin(cos(0))", 0, sin(1.0)), "one inside another");
}

/* --- The part that matters for drawing a curve --- */

static void test_undefined_is_not_an_error(void) {
    printf("Where a curve has no point\n");

    char why[128];
    double out = 0.0;

    /*
     * These are the whole reason there are three results rather than two. The
     * expression is fine; it simply has no value at this x. A grapher told
     * "error" gives up on the curve, and one told a very large number draws a
     * vertical line where an asymptote is and calls it part of the function.
     */
    check(recon_expr_eval("1/x", 0, &out, why, sizeof(why))
        == RECON_EXPR_UNDEFINED, "one over x, at zero");
    check(recon_expr_eval("1/x", 2, &out, why, sizeof(why))
        == RECON_EXPR_OK && fabs(out - 0.5) < 1e-12,
        "and the same expression is perfectly fine at two");

    check(recon_expr_eval("sqrt(x)", -1, &out, why, sizeof(why))
        == RECON_EXPR_UNDEFINED, "the root of a negative");
    check(recon_expr_eval("ln(x)", 0, &out, why, sizeof(why))
        == RECON_EXPR_UNDEFINED, "the logarithm of nothing");
    check(recon_expr_eval("ln(x)", -5, &out, why, sizeof(why))
        == RECON_EXPR_UNDEFINED, "and of a negative");
    check(recon_expr_eval("asin(x)", 2, &out, why, sizeof(why))
        == RECON_EXPR_UNDEFINED, "an arcsine outside its domain");
    check(recon_expr_eval("x/(x-1)", 1, &out, why, sizeof(why))
        == RECON_EXPR_UNDEFINED, "a denominator that reaches zero");

    /* An overflow is the same kind of nothing: a number too large to be one. */
    check(recon_expr_eval("10^10000", 0, &out, why, sizeof(why))
        == RECON_EXPR_UNDEFINED, "and a result too large to exist");
}

static void test_what_is_refused(void) {
    printf("What is not an expression\n");

    char why[128];

    check(!recon_expr_valid("", why, sizeof(why)), "nothing at all");
    check(!recon_expr_valid("2+", why, sizeof(why)), "an operator with no right side");
    check(!recon_expr_valid("(2+3", why, sizeof(why)), "a bracket left open");
    check(!recon_expr_valid("2+3)", why, sizeof(why)), "a bracket never opened");
    check(!recon_expr_valid("wibble(2)", why, sizeof(why)), "a function nobody has");
    check(!recon_expr_valid("y+1", why, sizeof(why)),
        "a variable that is not x");
    check(!recon_expr_valid("2 3", why, sizeof(why)),
        "two numbers with nothing between them");
    check(!recon_expr_valid("sin", why, sizeof(why)),
        "a function with no argument");

    /* And the reason is a sentence rather than a code. */
    recon_expr_valid("(1+2", why, sizeof(why));
    check(strstr(why, "closed") != NULL, "and it says what was wrong");

    /*
     * Undefined still counts as valid, because it is: the caller asked whether
     * this is an expression, and it is one -- it merely has no value at the x
     * that was tried.
     */
    check(recon_expr_valid("1/(x-1)", why, sizeof(why)),
        "an expression undefined at the x it was tried with is still valid");
}

int main(void) {
    printf("Expressions\n\n");

    test_arithmetic();
    test_precedence();
    test_x_and_constants();
    test_functions();
    test_undefined_is_not_an_error();
    test_what_is_refused();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
