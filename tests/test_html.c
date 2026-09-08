/*
 * Tests for reading HTML.
 *
 * There was no suite for this at all, which is how BG-160 got in: `<title>` is
 * not only the document's -- SVG uses it for the accessible name of a drawing
 * -- and the parser turned collection back on at every one, so wikipedia.org
 * put "Wikipedia Close" on the title bar. A parser that turns somebody else's
 * file into what a window draws is exactly the kind of thing that wants tests,
 * and the check for that fault is the last one here.
 *
 * Run with: ./build/recon_html_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_css.h"
#include "recon_html.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
    }
}

/* The text of one block, joined, for comparing against what was written. */
static void block_text(const struct recon_html_document *d, int index,
        char *out, size_t size) {
    out[0] = '\0';
    const struct recon_html_block_entry *b = recon_html_block_at(d, index);
    if (b == NULL) {
        return;
    }
    size_t used = 0;
    for (int i = 0; i < b->run_count; i++) {
        const struct recon_html_run *run = recon_html_run_at(d, b->first_run + i);
        if (run == NULL) {
            continue;
        }
        size_t take = run->length;
        if (used + take >= size) {
            take = size - used - 1;
        }
        memcpy(out + used, run->text, take);
        used += take;
        out[used] = '\0';
    }
}

/* The first block of a given kind, or -1. */
static int first_of(const struct recon_html_document *d,
        enum recon_html_block kind) {
    for (int i = 0; i < recon_html_block_count(d); i++) {
        const struct recon_html_block_entry *b = recon_html_block_at(d, i);
        if (b != NULL && b->kind == kind) {
            return i;
        }
    }
    return -1;
}

/* --- Tests --- */

static void test_the_shape_of_a_page(void) {
    printf("a page becomes blocks and runs\n");

    const char *html =
        "<html><head><title>A Page</title></head><body>"
        "<h1>Heading</h1>"
        "<p>Some <b>bold</b> words.</p>"
        "<ul><li>one</li><li>two</li></ul>"
        "<hr>"
        "</body></html>";

    struct recon_html_document *d = recon_html_parse(html, strlen(html));
    check(d != NULL, "a page parses");
    if (d == NULL) {
        return;
    }

    check(strcmp(recon_html_title(d), "A Page") == 0, "the title is read");

    int at = first_of(d, RECON_HTML_HEADING);
    check(at >= 0, "there is a heading");
    if (at >= 0) {
        char text[128];
        block_text(d, at, text, sizeof(text));
        check(strcmp(text, "Heading") == 0, "and it says what was written");
        check(recon_html_block_at(d, at)->level == 1, "at level one");
    }

    /*
     * The bold word is a run of its own inside the paragraph, and the spaces
     * either side of it survive. That is the whole reason runs exist, and the
     * failure it guards against reads as "SomeboldWords".
     */
    at = first_of(d, RECON_HTML_PARAGRAPH);
    check(at >= 0, "there is a paragraph");
    if (at >= 0) {
        char text[128];
        block_text(d, at, text, sizeof(text));
        check(strcmp(text, "Some bold words.") == 0,
            "the spaces around a styled word survive it");
    }

    at = first_of(d, RECON_HTML_LIST_ITEM);
    check(at >= 0, "a list becomes items");

    check(first_of(d, RECON_HTML_RULE) >= 0, "and a rule is a block with no runs");

    recon_html_free(d);
}

static void test_an_image_keeps_both_halves(void) {
    printf("an image is a block with an address and its alt text\n");

    const char *html =
        "<p>before</p>"
        "<img src=\"/cat.png\" alt=\"A cat\">"
        "<p>after</p>";

    struct recon_html_document *d = recon_html_parse(html, strlen(html));
    check(d != NULL, "it parses");
    if (d == NULL) {
        return;
    }

    int at = first_of(d, RECON_HTML_IMAGE);
    check(at >= 0, "the image is a block of its own");
    if (at >= 0) {
        const struct recon_html_block_entry *b = recon_html_block_at(d, at);
        check(b->source >= 0, "carrying where the picture is");
        check(b->source >= 0 &&
            strcmp(recon_html_link_at(d, b->source), "/cat.png") == 0,
            "which is the src, unresolved -- resolving it is the viewer's");

        /*
         * The alt text is still there. That is what makes this degrade to
         * exactly what it did before: a picture that never arrives leaves a
         * block whose runs are the alt text, which is what alt text is for.
         */
        char text[128];
        block_text(d, at, text, sizeof(text));
        check(strcmp(text, "A cat") == 0, "and the alt text as its runs");
    }

    /* Splitting a paragraph is the cost of a picture having a height. What
     * must not happen is either half going missing. */
    check(recon_html_block_count(d) >= 3,
        "the paragraphs either side of it survive");

    recon_html_free(d);
}

static void test_an_image_with_only_one_half(void) {
    printf("an image with only a source, or only alt text\n");

    const char *only_src = "<img src=\"/x.png\">";
    struct recon_html_document *d =
        recon_html_parse(only_src, strlen(only_src));
    if (d != NULL) {
        int at = first_of(d, RECON_HTML_IMAGE);
        check(at >= 0, "a picture with no alt text is still a picture");
        if (at >= 0) {
            check(recon_html_block_at(d, at)->source >= 0,
                "and still knows where it is");
        }
        recon_html_free(d);
    }

    const char *only_alt = "<img alt=\"described\">";
    d = recon_html_parse(only_alt, strlen(only_alt));
    if (d != NULL) {
        int at = first_of(d, RECON_HTML_IMAGE);
        check(at >= 0, "alt text with no picture is still a block");
        if (at >= 0) {
            check(recon_html_block_at(d, at)->source < 0,
                "with nothing to fetch");
        }
        recon_html_free(d);
    }

    /* Neither: nothing to say and nothing to draw. */
    const char *neither = "<p>a</p><img><p>b</p>";
    d = recon_html_parse(neither, strlen(neither));
    if (d != NULL) {
        check(first_of(d, RECON_HTML_IMAGE) < 0,
            "an img with no src and no alt is not a block at all");
        recon_html_free(d);
    }
}

static void test_only_the_first_title_counts(void) {
    printf("the document's title, and not every title in the page\n");

    /*
     * BG-160, written down as a page. SVG's `<title>` is the accessible name
     * of a drawing, and a page may have any number of them -- wikipedia.org
     * does. Collecting them all put "Wikipedia Close" on the title bar.
     */
    const char *html =
        "<html><head><title>Wikipedia</title></head><body>"
        "<svg><title>Close</title><path d=\"M0 0\"/></svg>"
        "<p>text</p></body></html>";

    struct recon_html_document *d = recon_html_parse(html, strlen(html));
    check(d != NULL, "it parses");
    if (d == NULL) {
        return;
    }
    check(strcmp(recon_html_title(d), "Wikipedia") == 0,
        "the head's title wins and the drawing's is ignored");
    recon_html_free(d);
}

static void test_a_stylesheet_hides_things(void) {
    printf("what a stylesheet does to a page\n");

    /*
     * The one that matters. Most of what makes a real page unreadable in a
     * structural reader is not layout -- it is the parts that were never
     * meant to be seen at once.
     */
    const char *html =
        "<style>.chrome { display: none } p { color: #112233 }</style>"
        "<div class=\"chrome\"><p>navigation</p><p>more navigation</p></div>"
        "<p>the article</p>";

    struct recon_css_sheet *sheet = recon_css_new();
    struct recon_html_document *d =
        recon_html_parse_styled(html, strlen(html), sheet);
    check(d != NULL, "it parses");
    if (d == NULL) {
        recon_css_free(sheet);
        return;
    }

    check(recon_css_rule_count(sheet) == 2,
        "the page's own <style> reached the sheet");

    bool saw_article = false;
    bool saw_navigation = false;
    for (int i = 0; i < recon_html_block_count(d); i++) {
        char text[128];
        block_text(d, i, text, sizeof(text));
        if (strstr(text, "article") != NULL) {
            saw_article = true;
        }
        if (strstr(text, "navigation") != NULL) {
            saw_navigation = true;
        }
    }
    check(saw_article, "the article is there");
    check(!saw_navigation, "and everything inside the hidden div is not");

    /* The colour reached the run, which is what a page written for a light
     * background needs in order not to be black on black. */
    int at = first_of(d, RECON_HTML_PARAGRAPH);
    if (at >= 0) {
        const struct recon_html_block_entry *b = recon_html_block_at(d, at);
        const struct recon_html_run *r = recon_html_run_at(d, b->first_run);
        check(r != NULL && r->has_colour && r->colour == 0x112233,
            "and the colour is on the run");
    }

    recon_html_free(d);
    recon_css_free(sheet);
}

static void test_hiding_nests_and_ends(void) {
    printf("a hide ends with the element that started it\n");

    /*
     * The failure this guards against is a hide that never ends: one flag
     * instead of a depth, and the first closing tag inside the hidden element
     * turns everything back on -- or the outermost one never does, and the
     * rest of the page disappears.
     */
    const char *html =
        "<style>.gone { display: none }</style>"
        "<div class=\"gone\"><div><p>inside</p></div></div>"
        "<p>after</p>";

    struct recon_css_sheet *sheet = recon_css_new();
    struct recon_html_document *d =
        recon_html_parse_styled(html, strlen(html), sheet);
    if (d == NULL) {
        recon_css_free(sheet);
        return;
    }

    bool inside = false, after = false;
    for (int i = 0; i < recon_html_block_count(d); i++) {
        char text[128];
        block_text(d, i, text, sizeof(text));
        if (strstr(text, "inside") != NULL) { inside = true; }
        if (strstr(text, "after") != NULL) { after = true; }
    }
    check(!inside, "a nested element inside a hidden one stays hidden");
    check(after, "and the page comes back when the hidden element closes");

    recon_html_free(d);
    recon_css_free(sheet);
}

static void test_a_void_element_does_not_swallow_the_page(void) {
    printf("a void element is asked about and not pushed\n");

    /*
     * <br>, <img>, <meta> have no closing tag. A stack that pushed them would
     * never pop, so a hidden <img> would hide everything after it and a plain
     * <br> would put the rest of the page one level too deep.
     */
    const char *html =
        "<style>.gone { display: none }</style>"
        "<p>before</p><img class=\"gone\" src=\"/x.png\" alt=\"hidden\">"
        "<br><p>after</p>";

    struct recon_css_sheet *sheet = recon_css_new();
    struct recon_html_document *d =
        recon_html_parse_styled(html, strlen(html), sheet);
    if (d == NULL) {
        recon_css_free(sheet);
        return;
    }

    bool before = false, after = false;
    for (int i = 0; i < recon_html_block_count(d); i++) {
        char text[128];
        block_text(d, i, text, sizeof(text));
        if (strstr(text, "before") != NULL) { before = true; }
        if (strstr(text, "after") != NULL) { after = true; }
    }
    check(before && after, "the page either side of them survives");
    check(first_of(d, RECON_HTML_IMAGE) < 0, "and the hidden picture is gone");

    recon_html_free(d);
    recon_css_free(sheet);
}

static void test_an_inline_style(void) {
    printf("a style attribute, which beats the sheet\n");

    const char *html =
        "<style>p { color: red }</style>"
        "<p style=\"color: #00ff00; font-weight: bold\">text</p>";

    struct recon_css_sheet *sheet = recon_css_new();
    struct recon_html_document *d =
        recon_html_parse_styled(html, strlen(html), sheet);
    if (d == NULL) {
        recon_css_free(sheet);
        return;
    }

    int at = first_of(d, RECON_HTML_PARAGRAPH);
    check(at >= 0, "the paragraph is there");
    if (at >= 0) {
        const struct recon_html_block_entry *b = recon_html_block_at(d, at);
        const struct recon_html_run *r = recon_html_run_at(d, b->first_run);
        check(r != NULL && r->has_colour && r->colour == 0x00FF00,
            "the attribute's colour wins over the sheet's");
        check(r != NULL && (r->style & RECON_HTML_BOLD) != 0,
            "and its weight is on the run");
    }

    recon_html_free(d);
    recon_css_free(sheet);
}

static void test_style_is_inherited(void) {
    printf("a colour set on an ancestor reaches the text inside it\n");

    /*
     * Without inheritance, `body { color: #333 }` -- which is how nearly
     * every page sets its text colour -- would colour nothing at all, because
     * no text is a direct child of body.
     */
    const char *html =
        "<style>body { color: #334455 }</style>"
        "<body><div><p>deep</p></div></body>";

    struct recon_css_sheet *sheet = recon_css_new();
    struct recon_html_document *d =
        recon_html_parse_styled(html, strlen(html), sheet);
    if (d == NULL) {
        recon_css_free(sheet);
        return;
    }

    int at = first_of(d, RECON_HTML_PARAGRAPH);
    if (at >= 0) {
        const struct recon_html_block_entry *b = recon_html_block_at(d, at);
        const struct recon_html_run *r = recon_html_run_at(d, b->first_run);
        check(r != NULL && r->has_colour && r->colour == 0x334455,
            "the body's colour reaches a paragraph two levels down");
    }

    recon_html_free(d);
    recon_css_free(sheet);
}

static void test_the_sheets_a_page_asks_for(void) {
    printf("the stylesheets a page names, for the viewer to fetch\n");

    const char *html =
        "<link rel=\"stylesheet\" href=\"/one.css\">"
        "<link rel=\"icon\" href=\"/favicon.ico\">"
        "<link rel=\"Stylesheet\" href=\"two.css\">"
        "<p>text</p>";

    struct recon_html_document *d = recon_html_parse(html, strlen(html));
    if (d == NULL) {
        return;
    }
    check(recon_html_stylesheet_count(d) == 2, "two stylesheets, not three");
    if (recon_html_stylesheet_count(d) == 2) {
        check(strcmp(recon_html_stylesheet_at(d, 0), "/one.css") == 0,
            "in the order the page named them");
        check(strcmp(recon_html_stylesheet_at(d, 1), "two.css") == 0,
            "and unresolved, which is the viewer's job");
    }
    recon_html_free(d);
}

static void test_the_encoding_is_looked_at_not_believed(void) {
    printf("bytes that are not UTF-8 are read as Windows-1252\n");

    /* Already UTF-8: nothing to do, and saying so is how the caller avoids a
     * copy of every page it ever loads. */
    const char *plain = "plain ASCII";
    check(recon_html_to_utf8(plain, strlen(plain), NULL, NULL) == NULL,
        "ASCII is left alone");

    const char *utf8 = "caf\xC3\xA9";                       /* café */
    check(recon_html_to_utf8(utf8, strlen(utf8), NULL, NULL) == NULL,
        "and so is real UTF-8");

    /*
     * The same word in Latin-1. As UTF-8 it is invalid -- 0xE9 is the start
     * of a three-byte sequence and there are no continuation bytes after it
     * -- so it is read as Windows-1252 and comes out as café.
     */
    const char *latin1 = "caf\xE9";
    size_t out_length = 0;
    char *fixed = recon_html_to_utf8(latin1, strlen(latin1), NULL,
        &out_length);
    check(fixed != NULL, "Latin-1 is converted");
    if (fixed != NULL) {
        check(strcmp(fixed, "caf\xC3\xA9") == 0,
            "and comes out as the same word in UTF-8");
        free(fixed);
    }

    /*
     * The bytes Windows-1252 puts where Latin-1 has controls. A page written
     * in a word processor is full of them, and read as Latin-1 they are
     * control characters and vanish.
     */
    const char *smart = "\x93quoted\x94 \x97 dash";
    fixed = recon_html_to_utf8(smart, strlen(smart), NULL, &out_length);
    check(fixed != NULL, "the 0x80-0x9F range is converted too");
    if (fixed != NULL) {
        check(strstr(fixed, "\xE2\x80\x9C") != NULL,
            "a curly open quote becomes U+201C");
        check(strstr(fixed, "\xE2\x80\x94") != NULL,
            "and an em dash becomes U+2014");
        free(fixed);
    }

    /*
     * A page that says Latin-1 and is really UTF-8 is common enough that
     * believing the label would break pages that work today. The bytes win.
     */
    fixed = recon_html_to_utf8(utf8, strlen(utf8), "iso-8859-1", NULL);
    check(fixed == NULL, "a wrong label does not override valid UTF-8");

    /* Overlong forms and surrogates decode "fine" and are not valid. Reading
     * them as UTF-8 is how one byte sequence means two different things. */
    const char *overlong = "\xC0\xAF";
    check(recon_html_to_utf8(overlong, 2, NULL, NULL) != NULL,
        "an overlong form is not treated as UTF-8");
    const char *surrogate = "\xED\xA0\x80";
    check(recon_html_to_utf8(surrogate, 3, NULL, NULL) != NULL,
        "nor is a surrogate");
    const char *truncated = "abc\xC3";
    check(recon_html_to_utf8(truncated, 4, NULL, NULL) != NULL,
        "nor is a sequence cut off at the end");
}

static void test_nothing_is_refused(void) {
    printf("there is no such thing as HTML this refuses\n");

    const char *nonsense[] = {
        "", "<", "<<<>>>", "<p", "<p>unclosed",
        "<img src=", "<img src=\"", "<title>", "</title>",
        "&", "&amp", "&#;", "&#xZZ;",
    };

    for (size_t i = 0; i < sizeof(nonsense) / sizeof(nonsense[0]); i++) {
        struct recon_html_document *d =
            recon_html_parse(nonsense[i], strlen(nonsense[i]));
        char what[96];
        snprintf(what, sizeof(what), "'%s' parses to something", nonsense[i]);
        check(d != NULL, what);
        recon_html_free(d);
    }
}

int main(void) {
    printf("ReconOS HTML tests\n\n");

    test_the_shape_of_a_page();
    test_an_image_keeps_both_halves();
    test_an_image_with_only_one_half();
    test_only_the_first_title_counts();
    test_a_stylesheet_hides_things();
    test_hiding_nests_and_ends();
    test_a_void_element_does_not_swallow_the_page();
    test_an_inline_style();
    test_style_is_inherited();
    test_the_sheets_a_page_asks_for();
    test_the_encoding_is_looked_at_not_believed();
    test_nothing_is_refused();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
