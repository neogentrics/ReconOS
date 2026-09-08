/*
 * The HTML parser against somebody else's adversarial markup.
 *
 * Every other test in this project was written by whoever wrote the code it
 * tests, which is the failure BG-162 is a record of: the fault was diagnosed
 * from reading the source, a test was written for the markup that diagnosis
 * assumed, it passed, and the page still rendered wrong. A test written
 * against markup you assumed proves nothing.
 *
 * These inputs come from the html5lib tree-construction suite (MIT, extracted
 * from mozilla-central's copy) -- fifteen hundred pieces of markup written by
 * people whose purpose was to break HTML parsers. Their *expected output* is a
 * DOM tree and this parser deliberately builds no tree, so the answers cannot
 * be compared. What can be checked is everything that must hold whatever the
 * answer is, and those are the interesting properties anyway:
 *
 *   it returns, it does not crash, and it does not run away
 *   every run points inside the document's own text
 *   every block's runs exist
 *   every link index a run names exists
 *   a stylesheet in the mix changes none of the above
 *
 * A parser that turns somebody else's file into what a window draws is exactly
 * the thing that wants a corpus it did not choose.
 *
 * Run with: ./build/recon_html_corpus_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_css.h"
#include "recon_html.h"

static int g_failures;
static int g_checks;

/*
 * Every byte of every run, added up.
 *
 * The sum is worthless; reading the bytes is the entire point. A compiler may
 * delete a loop whose result nothing uses, and deleting that loop would
 * delete the check -- the value of walking every run is that a pointer
 * outside the document's buffer gets touched and faults.
 */
static unsigned long g_touched;

/*
 * Failures are reported once per kind rather than once per case.
 *
 * Fifteen hundred cases through a broken invariant is fifteen hundred
 * identical lines, and the one useful fact -- which case first showed it --
 * scrolls off the top.
 */
static void check_case(bool condition, const char *what, int which,
        const char *markup, size_t length) {
    g_checks++;
    if (condition) {
        return;
    }
    g_failures++;
    if (g_failures > 12) {
        return;
    }

    /* The markup, printable, short, and with the invisible made visible --
     * these cases are largely about bytes nobody can see. */
    char shown[160];
    size_t used = 0;
    for (size_t i = 0; i < length && used < sizeof(shown) - 5; i++) {
        unsigned char c = (unsigned char)markup[i];
        if (c == '\0') {
            memcpy(shown + used, "\\0", 2);
            used += 2;
        } else if (c == '\n') {
            memcpy(shown + used, "\\n", 2);
            used += 2;
        } else if (c < 0x20 || c == 0x7F) {
            used += (size_t)snprintf(shown + used, sizeof(shown) - used,
                "\\x%02X", c);
        } else {
            shown[used++] = (char)c;
        }
    }
    shown[used] = '\0';
    printf("  FAIL: %s\n        case %d: %s\n", what, which, shown);
}

/* --- The corpus --- */

struct corpus {
    char *bytes;
    size_t length;
};

static struct corpus load(const char *path) {
    struct corpus c = { NULL, 0 };
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return c;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return c;
    }
    long size = ftell(f);
    if (size <= 0) {
        fclose(f);
        return c;
    }
    rewind(f);

    c.bytes = malloc((size_t)size + 1);
    if (c.bytes == NULL) {
        fclose(f);
        return c;
    }
    c.length = fread(c.bytes, 1, (size_t)size, f);
    c.bytes[c.length] = '\0';
    fclose(f);
    return c;
}

/*
 * One case: a decimal length, a newline, then that many bytes.
 *
 * Length-prefixed rather than delimited because thirty-six of these contain
 * NUL bytes and several contain every kind of line ending -- there is no
 * delimiter that survives the corpus, which is rather the point of it.
 */
static bool next_case(const struct corpus *c, size_t *at,
        const char **markup, size_t *length) {
    while (*at < c->length) {
        /* Comment lines at the head of the file. */
        if (c->bytes[*at] == '#') {
            while (*at < c->length && c->bytes[*at] != '\n') {
                (*at)++;
            }
            (*at)++;
            continue;
        }
        if (c->bytes[*at] == '\n') {
            (*at)++;
            continue;
        }
        break;
    }
    if (*at >= c->length) {
        return false;
    }

    char *end = NULL;
    long n = strtol(c->bytes + *at, &end, 10);
    if (end == NULL || *end != '\n' || n < 0) {
        return false;
    }
    size_t start = (size_t)(end - c->bytes) + 1;
    if (start + (size_t)n > c->length) {
        return false;
    }

    *markup = c->bytes + start;
    *length = (size_t)n;
    *at = start + (size_t)n + 1;
    return true;
}

/* --- What must hold, whatever the answer is --- */

static void check_document(const struct recon_html_document *d, int which,
        const char *markup, size_t length) {
    int blocks = recon_html_block_count(d);
    int runs = recon_html_run_count(d);
    int links = recon_html_link_count(d);

    check_case(blocks >= 0 && runs >= 0 && links >= 0,
        "the counts are not negative", which, markup, length);

    for (int i = 0; i < blocks; i++) {
        const struct recon_html_block_entry *b = recon_html_block_at(d, i);
        if (b == NULL) {
            check_case(false, "a block within the count is missing",
                which, markup, length);
            break;
        }

        /*
         * A block names a span of the run table. Both ends inside it: a
         * first_run past the end is a read of whatever follows the array, and
         * it is exactly the shape of fault that survives ordinary testing
         * because the memory after an array is usually readable.
         */
        check_case(b->first_run >= 0 && b->run_count >= 0 &&
            b->first_run + b->run_count <= runs,
            "a block's runs are inside the run table", which, markup, length);

        check_case(b->source < 0 || b->source < links,
            "a block's image index names a link that exists",
            which, markup, length);

        check_case(b->level >= 0 && b->level <= 32,
            "a block's level is sane", which, markup, length);
    }

    for (int i = 0; i < runs; i++) {
        const struct recon_html_run *r = recon_html_run_at(d, i);
        if (r == NULL) {
            check_case(false, "a run within the count is missing",
                which, markup, length);
            break;
        }

        /*
         * Runs point into one buffer the document owns, and there is no public
         * way to ask where that buffer is -- nor should there be. So instead
         * every run's bytes are read, which is exactly what the drawing code
         * does: a run pointing outside the buffer either faults here or is
         * caught by a sanitiser, and both of those are this test failing
         * rather than a page quietly drawing somebody else's memory.
         */
        check_case(r->text != NULL || r->length == 0,
            "a run with length has text", which, markup, length);

        unsigned long sum = 0;
        for (size_t j = 0; r->text != NULL && j < r->length; j++) {
            sum += (unsigned char)r->text[j];
        }
        g_touched += sum;

        check_case(r->link < links, "a run's link index exists",
            which, markup, length);
    }

    for (int i = 0; i < links; i++) {
        check_case(recon_html_link_at(d, i) != NULL,
            "a link within the count is there", which, markup, length);
    }
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "tests/data/html5lib-inputs.txt";

    printf("ReconOS HTML parser against the html5lib corpus\n\n");

    struct corpus c = load(path);
    if (c.bytes == NULL) {
        printf("  FAIL: cannot read %s\n", path);
        printf("\n0 checks, 1 failure\n");
        return 1;
    }

    size_t at = 0;
    const char *markup = NULL;
    size_t length = 0;
    int which = 0;
    int with_blocks = 0;

    while (next_case(&c, &at, &markup, &length)) {
        which++;

        /* --- Plain --- */
        struct recon_html_document *d = recon_html_parse(markup, length);
        check_case(d != NULL, "it returns a document", which, markup, length);
        if (d != NULL) {
            check_document(d, which, markup, length);
            if (recon_html_block_count(d) > 0) {
                with_blocks++;
            }
            recon_html_free(d);
        }

        /* --- And with a stylesheet in the mix --- */
        struct recon_css_sheet *sheet = recon_css_new();
        d = recon_html_parse_styled(markup, length, sheet);
        check_case(d != NULL, "styled, it returns a document too",
            which, markup, length);
        if (d != NULL) {
            check_document(d, which, markup, length);
            recon_html_free(d);
        }
        recon_css_free(sheet);
    }

    free(c.bytes);

    /*
     * --- How much came out ---
     *
     * A corpus that produced nothing at all would pass every invariant above
     * while proving the parser does not work, so the yield is itself a check.
     *
     * The number is a regression guard and not a claim about the
     * specification, which is worth being precise about. Only 787 of these
     * 1556 cases contain any text outside a tag -- measured, not guessed --
     * and of those, some put their text inside `<script>`, `<title>` or a
     * closed `<details>`, where producing no block is the right answer. So
     * "how many should produce blocks" has no principled value; what it has
     * is a value it produces today, and a drop below it means something
     * stopped working.
     */
    printf("  %d cases, %d of them produced blocks, %lu bytes read back\n",
        which, with_blocks, g_touched);
    g_checks++;
    if (which < 1000) {
        g_failures++;
        printf("  FAIL: only %d cases read -- the corpus is short\n", which);
    }
    g_checks++;
    if (with_blocks < 600) {
        g_failures++;
        printf("  FAIL: only %d cases produced blocks, and %d did before -- "
            "something stopped reading markup it used to read\n",
            with_blocks, 600);
    }

    if (g_failures > 12) {
        printf("  ... and %d more failures\n", g_failures - 12);
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
