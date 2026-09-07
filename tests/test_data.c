/*
 * Reading a file of numbers, and the one mistake that produces a wrong picture
 * instead of an error.
 *
 * Every other failure in this reader announces itself: a line that is not two
 * numbers is counted and shown, a file that will not open says so. The decimal
 * comma does not. Handed "3,14 2,71" a dot-only reader returns two perfectly
 * valid numbers -- 3 and 14 -- counts no bad lines, and plots a point five
 * hundred times too high.
 *
 * So the first test here names that wrong answer explicitly. If the sniff is
 * ever removed, this suite does not merely fail; it says which point came back
 * and what it should have been.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "recon_data.h"

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

#define POINTS 64

/* The text has to be writable: recon_data_parse tokenises it in place. */
struct reading {
    struct recon_data_read got;
    double x[POINTS];
    double y[POINTS];
};

static void read_text(struct reading *r, const char *text) {
    static char buffer[8192];
    snprintf(buffer, sizeof(buffer), "%s", text);
    memset(r, 0, sizeof(*r));
    r->got = recon_data_parse(buffer, r->x, r->y, POINTS);
}

static bool close_to(double got, double want) {
    return fabs(got - want) <= 1e-12 * (1.0 + fabs(want));
}

/* ------------------------------------------------------------------ */

static void test_the_decimal_comma(void) {
    printf("The decimal comma\n");

    struct reading r;
    read_text(&r, "3,14 2,71\n1,5 -0,5\n");

    check(r.got.comma_decimal,
        "a file of two comma-bearing tokens a line is read as decimal commas");
    check(r.got.count == 2, "both lines are points");
    check(r.got.bad == 0, "and neither is counted bad");

    /*
     * The whole reason this suite exists. A dot-only reader gives (3, 14) --
     * valid, uncounted, and wrong.
     */
    bool right = r.got.count == 2 && close_to(r.x[0], 3.14) &&
        close_to(r.y[0], 2.71);
    if (!right && r.got.count > 0) {
        printf("        first point came back as (%.17g, %.17g)\n",
            r.x[0], r.y[0]);
        printf("        a dot-only reader gives (3, 14); it should be "
            "(3.14, 2.71)\n");
    }
    check(right, "3,14 2,71 IS THE POINT (3.14, 2.71) -- not (3, 14)");

    check(r.got.count == 2 && close_to(r.x[1], 1.5) && close_to(r.y[1], -0.5),
        "and a negative one survives the same treatment");
}

static void test_the_ordinary_way(void) {
    printf("The ordinary way\n");

    struct reading r;
    read_text(&r, "3.14 2.71\n1.5 -0.5\n");

    check(!r.got.comma_decimal, "dots are read as dots");
    check(r.got.count == 2 && close_to(r.x[0], 3.14) && close_to(r.y[0], 2.71),
        "and mean what they say");

    read_text(&r, "1, 2\n3, 4\n");
    check(!r.got.comma_decimal,
        "a comma used to separate is not a decimal point");
    check(r.got.count == 2 && close_to(r.x[0], 1) && close_to(r.y[0], 2),
        "and the two numbers either side of it are the point");

    read_text(&r, "3 4\n5 6\n");
    check(!r.got.comma_decimal,
        "plain integers vote for neither, so the ordinary reading stands");
    check(r.got.count == 2 && close_to(r.x[1], 5) && close_to(r.y[1], 6),
        "and they read the same either way, which is why they may not vote");
}

static void test_what_it_refuses_to_guess(void) {
    printf("What it will not guess at\n");

    struct reading r;

    /*
     * One row each way. The comma row alone would win; the dot row alone would
     * win. Together the honest answer is the ordinary one, because reading the
     * dot row as decimal commas would be wrong in the other direction.
     */
    read_text(&r, "3,14 2,71\n1.5 2.5\n");
    check(!r.got.comma_decimal,
        "a file mixing the two conventions is read the ordinary way");

    read_text(&r, "1,234.5 2.5\n");
    check(!r.got.comma_decimal,
        "a thousands comma beside a decimal dot does not win it for the comma");

    read_text(&r, "1,2,3 4\n");
    check(!r.got.comma_decimal,
        "and neither does a token with two commas in it");

    check(!recon_data_is_comma_decimal(""),
        "an empty file has no convention, so it gets the ordinary one");
    check(!recon_data_is_comma_decimal(NULL),
        "and neither does nothing at all");
}

static void test_lines_that_are_not_points(void) {
    printf("Lines that are not points\n");

    struct reading r;
    read_text(&r,
        "# x and y\n"
        "\n"
        "1 2\n"
        "   \n"
        "not a number\n"
        "3 4\n"
        "5\n");

    check(r.got.count == 2, "two of the seven lines were points");
    check(r.got.bad == 2,
        "two were not two numbers -- the words, and the lone 5");
    check(r.got.comma_decimal == false, "and none of it voted for a comma");

    /* A heading and a blank line are not mistakes. If they were counted the
     * warning would fire on a file that is completely fine. */
    read_text(&r, "# heading\n\n1 2\n");
    check(r.got.bad == 0,
        "a heading and a blank line are not counted against the file");
}

static void test_the_ceiling(void) {
    printf("The ceiling\n");

    static char many[8192];
    int used = 0;
    for (int i = 0; i < POINTS + 5; i++) {
        used += snprintf(many + used, sizeof(many) - (size_t)used,
            "%d %d\n", i, i * 2);
    }

    struct reading r;
    memset(&r, 0, sizeof(r));
    r.got = recon_data_parse(many, r.x, r.y, POINTS);

    check(r.got.count == POINTS, "it stops at the ceiling");
    check(r.got.over == 5, "and counts what would not fit rather than dropping it");
    check(r.got.bad == 0, "a line past the ceiling is not a bad line");
    check(close_to(r.x[POINTS - 1], POINTS - 1),
        "and the last one that fitted is the right one");
}

static void test_nothing_at_all(void) {
    printf("Nothing at all\n");

    struct recon_data_read got = recon_data_parse(NULL, NULL, NULL, 0);
    check(got.count == 0 && got.bad == 0 && got.over == 0,
        "handed nothing, it reads nothing and does not reach for it");

    struct reading r;
    read_text(&r, "");
    check(r.got.count == 0 && r.got.bad == 0,
        "and an empty file is empty rather than broken");
}

int main(void) {
    printf("Reading numbers out of a file\n\n");

    test_the_decimal_comma();
    test_the_ordinary_way();
    test_what_it_refuses_to_guess();
    test_lines_that_are_not_points();
    test_the_ceiling();
    test_nothing_at_all();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
