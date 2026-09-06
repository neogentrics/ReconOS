/*
 * Working out what an expression comes to.
 *
 * Written for the Calculator's graphing mode, which needs to evaluate the same
 * expression a few hundred times with a different x each time. Kept out of the
 * Calculator because it has nothing to do with a calculator: it is a parser and
 * an evaluator, it touches no screen, and it can be tested by asking it
 * questions with known answers rather than by clicking anything.
 *
 * --- What it accepts ---
 *
 *   numbers        12, 3.5, .5, 1e-3
 *   the variable   x
 *   constants      pi, e
 *   arithmetic     + - * / ^, and unary minus
 *   grouping       ( )
 *   functions      sin cos tan asin acos atan sqrt cbrt ln log exp
 *                  abs floor ceil round
 *
 * Angles are radians, because every function above takes them that way in C
 * and a converter that silently used degrees would disagree with the Scientific
 * mode sitting next to it.
 *
 * --- What it does about answers that do not exist ---
 *
 * This is the part that matters for drawing a curve, so it is deliberate rather
 * than incidental.
 *
 * Dividing by zero, the square root of a negative, the logarithm of zero: these
 * are not errors in the expression. The expression is fine and simply has no
 * value at that x, which is a different thing and has to be reported
 * differently -- a grapher that treats them as errors gives up on the whole
 * curve because of one point, and a grapher that treats them as very large
 * numbers draws a vertical line where an asymptote is and calls it part of the
 * function.
 *
 * So there are three outcomes rather than two, and RECON_EXPR_UNDEFINED is the
 * one worth knowing about.
 */

#ifndef RECON_EXPR_H
#define RECON_EXPR_H

#include <stdbool.h>
#include <stddef.h>

enum recon_expr_result {
    /* It worked. */
    RECON_EXPR_OK,
    /* The expression is not one -- a stray bracket, an unknown name, an
     * operator with nothing to work on. `why` says which, and it will say the
     * same thing for every x, so a caller can stop asking. */
    RECON_EXPR_BAD,
    /* The expression is fine and has no value at this x. Ask again at another
     * x and it may well have one. */
    RECON_EXPR_UNDEFINED,
};

/*
 * Evaluate `text` with `x` bound, into `out`.
 *
 * `why` is filled with a sentence on RECON_EXPR_BAD, and left alone otherwise.
 * Both may be NULL.
 */
enum recon_expr_result recon_expr_eval(const char *text, double x,
    double *out, char *why, size_t why_size);

/*
 * Whether the expression parses at all, without caring what it comes to.
 *
 * For a caller that wants to say "that is not an expression" while somebody is
 * still typing, rather than after drawing nothing. Uses x = 1, which is a value
 * almost nothing is undefined at -- and an expression that parses is judged
 * here on parsing, so an undefined answer still counts as valid.
 */
bool recon_expr_valid(const char *text, char *why, size_t why_size);

#endif /* RECON_EXPR_H */
