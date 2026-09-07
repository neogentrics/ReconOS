#include "recon_data.h"

#include <stdlib.h>
#include <string.h>

bool recon_data_is_comma_decimal(const char *text) {
    if (text == NULL) {
        return false;
    }

    int for_comma = 0;
    int against = 0;

    const char *line = text;
    while (*line != '\0') {
        const char *newline = strchr(line, '\n');
        const char *stop = (newline != NULL) ? newline : line + strlen(line);

        const char *p = line;
        while (p < stop && (*p == ' ' || *p == '\t' || *p == '\r')) {
            p++;
        }

        if (p < stop && *p != '#') {
            int tokens = 0;
            int with_one_comma = 0;
            int with_a_dot = 0;
            bool comma_separates = false;

            while (p < stop) {
                int commas = 0;
                int dots = 0;
                while (p < stop && *p != ' ' && *p != '\t' && *p != '\r') {
                    if (*p == ',') {
                        commas++;
                    }
                    if (*p == '.') {
                        dots++;
                    }
                    p++;
                }
                tokens++;

                /*
                 * Two commas in one token is "1,234,5" -- whatever that is, it
                 * is not one number written with a decimal comma.
                 */
                if (commas > 1) {
                    comma_separates = true;
                }
                if (commas == 1) {
                    with_one_comma++;
                }
                if (dots > 0) {
                    with_a_dot++;
                }

                while (p < stop && (*p == ' ' || *p == '\t' || *p == '\r')) {
                    p++;
                }
            }

            if (tokens == 2 && with_one_comma == 2 && with_a_dot == 0 &&
                    !comma_separates) {
                for_comma++;
            } else if (with_a_dot > 0 || comma_separates || tokens != 2) {
                against++;
            }
            /*
             * Everything else -- two integer tokens, a single token, a row of
             * "1, 2" -- votes for neither. Silence is the right answer from a
             * row that reads the same under both conventions.
             */
        }

        if (newline == NULL) {
            break;
        }
        line = newline + 1;
    }

    return for_comma > 0 && against == 0;
}

struct recon_data_read recon_data_parse(char *text, double *xs, double *ys,
        int max) {
    struct recon_data_read got = { 0, 0, 0, false };

    if (text == NULL || xs == NULL || ys == NULL || max <= 0) {
        return got;
    }

    got.comma_decimal = recon_data_is_comma_decimal(text);

    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save);
            line != NULL;
            line = strtok_r(NULL, "\n", &save)) {

        while (*line == ' ' || *line == '\t' || *line == '\r') {
            line++;
        }
        if (*line == '\0' || *line == '#') {
            continue;
        }

        /*
         * Decimal commas become dots before strtod sees them. In place, and
         * safely: a file using the comma that way cannot also be using it to
         * separate, because recon_data_is_comma_decimal votes against any row
         * that does. So there is no comma left on this line meaning anything
         * else.
         */
        if (got.comma_decimal) {
            for (char *c = line; *c != '\0'; c++) {
                if (*c == ',') {
                    *c = '.';
                }
            }
        }

        char *end = NULL;
        double x = strtod(line, &end);
        if (end == line) {
            got.bad++;
            continue;
        }
        while (*end == ' ' || *end == '\t' || *end == ',') {
            end++;
        }
        char *second = end;
        double y = strtod(second, &end);
        if (end == second) {
            got.bad++;
            continue;
        }

        if (got.count >= max) {
            got.over++;
            continue;
        }
        xs[got.count] = x;
        ys[got.count] = y;
        got.count++;
    }

    return got;
}
