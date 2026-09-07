/*
 * Reading a file of numbers.
 *
 * Written for the Calculator's data mode, which plots two columns out of a
 * file somebody made elsewhere. Kept out of the Calculator for the same reason
 * recon_expr is: it touches no screen, it has nothing to do with a calculator,
 * and it can be tested by handing it text with a known answer rather than by
 * clicking anything.
 *
 * --- What it accepts ---
 *
 * Two numbers a line, separated by whitespace or a comma. Blank lines and
 * lines starting with `#` are skipped and are not counted as bad, because a
 * file with a heading on it is a file somebody made by hand and meant.
 *
 * --- What happens to a line that is not two numbers ---
 *
 * It is counted, and the caller is expected to say so. That is the whole of
 * this mode's design: a plot that silently drops the rows it could not read is
 * a picture of a different data set, and it looks exactly like a picture of
 * the right one. So does a plot that stops at the first bad line and shows
 * half. Neither refusing nor ignoring, then -- read what can be read and
 * report what could not.
 *
 * --- The decimal point ---
 *
 * The hard part, and the reason this file exists separately. `strtod` reads a
 * dot, always. Handed a file written where the convention is a comma, it does
 * not refuse and does not mangle anything: it returns a *different number*.
 * "3,14 2,71" reads as x = 3, comma skipped as a separator, y = 14 -- two
 * valid numbers, no bad line counted, a point plotted five hundred times too
 * high. A wrong picture indistinguishable from a right one.
 *
 * So the text is asked which convention it uses before any of it is read, and
 * the answer is reported back so the caller can say which way it was read.
 */

#ifndef RECON_DATA_H
#define RECON_DATA_H

#include <stdbool.h>

/* What reading a piece of text produced. */
struct recon_data_read {
    int count;              /* points stored */
    int bad;                /* lines that were not two numbers */
    int over;               /* lines that were two numbers, past the ceiling */
    bool comma_decimal;     /* the text writes decimal points with commas */
};

/*
 * Whether this text writes its decimal points with commas.
 *
 * A row votes for the comma only if it is two whitespace-separated tokens that
 * each hold exactly one comma and no dot. Any row holding a dot, or using a
 * comma to separate, votes against. **The comma wins only if it has votes and
 * nothing contradicts it**, so a file mixing the two is read the ordinary way
 * rather than guessed at.
 *
 * Rows of plain integers vote for neither, which is right: "3 4" means the
 * same thing under both conventions and should not tip the answer.
 */
bool recon_data_is_comma_decimal(const char *text);

/*
 * Read `text` into `xs` and `ys`, at most `max` points.
 *
 * **`text` is modified**: it is tokenised in place, and where the file writes
 * decimal commas they are turned into dots. The caller owns it and is expected
 * to have a copy it can afford to lose -- which the one caller does, having
 * just read the file into memory of its own.
 */
struct recon_data_read recon_data_parse(char *text, double *xs, double *ys,
    int max);

#endif
