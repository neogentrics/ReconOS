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
    /*
     * The results are freed. `recon_html_to_utf8` hands back a buffer the
     * caller owns, and three of these were dropped -- thirty bytes, found by
     * the sanitizer under scripts/check.sh.
     *
     * Thirty bytes in a test matter for one reason: a suite that leaks is a
     * suite that cannot be used to find a leak, because its own noise is
     * indistinguishable from the signal it exists to show.
     */
    const char *overlong = "\xC0\xAF";
    char *decoded = recon_html_to_utf8(overlong, 2, NULL, NULL);
    check(decoded != NULL, "an overlong form is not treated as UTF-8");
    free(decoded);

    const char *surrogate = "\xED\xA0\x80";
    decoded = recon_html_to_utf8(surrogate, 3, NULL, NULL);
    check(decoded != NULL, "nor is a surrogate");
    free(decoded);

    const char *truncated = "abc\xC3";
    decoded = recon_html_to_utf8(truncated, 4, NULL, NULL);
    check(decoded != NULL, "nor is a sequence cut off at the end");
    free(decoded);
}

/* Does any block on the page contain this text? */
static bool page_says(const struct recon_html_document *d, const char *want) {
    for (int i = 0; i < recon_html_block_count(d); i++) {
        char text[256];
        block_text(d, i, text, sizeof(text));
        if (strstr(text, want) != NULL) {
            return true;
        }
    }
    return false;
}

static void test_what_a_browser_hides_without_being_told(void) {
    printf("the things a browser hides that no stylesheet mentions\n");

    /*
     * None of this is CSS -- it is the behaviour of the elements themselves,
     * and a reader that only reads stylesheets shows all of it. Measured on
     * recontowers.com, whose accessibility panel is a closed <details> and
     * appeared in full: twenty lines of settings nobody had opened.
     */
    const char *html =
        "<p>visible</p>"
        "<template><p>a template</p></template>"
        "<dialog><p>a closed dialog</p></dialog>"
        "<dialog open><p>an open dialog</p></dialog>"
        "<div hidden><p>marked hidden</p></div>"
        "<p>after</p>";

    struct recon_html_document *d = recon_html_parse(html, strlen(html));
    check(d != NULL, "it parses");
    if (d == NULL) {
        return;
    }

    check(page_says(d, "visible"), "ordinary text is there");
    check(page_says(d, "after"), "and the page after it all survives");
    check(!page_says(d, "a template"), "a template is never rendered");
    check(!page_says(d, "a closed dialog"), "a closed dialog is not shown");
    check(page_says(d, "an open dialog"), "an open one is");
    check(!page_says(d, "marked hidden"), "and the hidden attribute hides");

    recon_html_free(d);
}

static void test_a_closed_details_shows_its_summary(void) {
    printf("a closed <details> shows the line you click and nothing else\n");

    const char *html =
        "<details><summary>Accessibility settings</summary>"
        "<p>text size</p><p>line spacing</p></details>"
        "<p>after</p>";

    struct recon_html_document *d = recon_html_parse(html, strlen(html));
    if (d == NULL) {
        return;
    }
    check(page_says(d, "Accessibility settings"),
        "the summary is shown even though its parent is not");
    check(!page_says(d, "text size"), "and the contents are not");
    check(page_says(d, "after"),
        "and the hide ends with the details, not with the summary");
    recon_html_free(d);

    /* Opened, it is an ordinary element again. */
    const char *open =
        "<details open><summary>Settings</summary><p>text size</p></details>";
    d = recon_html_parse(open, strlen(open));
    if (d != NULL) {
        check(page_says(d, "text size"), "an open one shows everything");
        recon_html_free(d);
    }
}

static void test_a_span_a_stylesheet_made_a_block(void) {
    printf("display: block on a span starts a line\n");

    /*
     * The difference between a <span> and a <div> is one property, and pages
     * set it constantly. Measured on recontowers.com: a card of five spans
     * laid out with flex came out as one run-on underlined sentence.
     */
    const char *html =
        "<style>.stack span { display: block }</style>"
        "<div class=\"stack\"><span>first</span><span>second</span></div>";

    struct recon_css_sheet *sheet = recon_css_new();
    struct recon_html_document *d =
        recon_html_parse_styled(html, strlen(html), sheet);
    if (d == NULL) {
        recon_css_free(sheet);
        return;
    }

    /* Two lines, not one: neither block holds both words. */
    bool joined = false;
    for (int i = 0; i < recon_html_block_count(d); i++) {
        char text[128];
        block_text(d, i, text, sizeof(text));
        if (strstr(text, "first") != NULL && strstr(text, "second") != NULL) {
            joined = true;
        }
    }
    check(page_says(d, "first") && page_says(d, "second"),
        "both spans are there");
    check(!joined, "and they are not run together into one line");

    recon_html_free(d);
    recon_css_free(sheet);

    /*
     * The other way round: a <p> told it is a block is being told what it
     * already was, and acting on it would leave an empty paragraph between
     * every two real ones.
     */
    const char *plain =
        "<style>p { display: block }</style><p>one</p><p>two</p>";
    sheet = recon_css_new();
    d = recon_html_parse_styled(plain, strlen(plain), sheet);
    if (d != NULL) {
        check(recon_html_block_count(d) == 2,
            "a block told it is a block does not gain an empty one");
        recon_html_free(d);
    }
    recon_css_free(sheet);
}

static void test_a_block_inside_a_heading_is_still_the_heading(void) {
    printf("a span made a block inside an h1 does not become body text\n");

    /*
     * The line break has to reopen the block it interrupted, not a fresh
     * paragraph. Measured on gaming.recontowers.com, whose masthead is
     * "Games built to<br>mean something." -- the break put the second half in
     * a paragraph and the page rendered one huge line followed by one small
     * one, in the middle of a sentence.
     */
    const char *html =
        "<style>h1 span { display: block }</style>"
        "<h1>Games built to <span>mean something.</span></h1>";

    struct recon_css_sheet *sheet = recon_css_new();
    struct recon_html_document *d =
        recon_html_parse_styled(html, strlen(html), sheet);
    if (d == NULL) {
        recon_css_free(sheet);
        return;
    }

    /* Every block that has any of the heading's words is the heading. */
    int seen = 0;
    for (int i = 0; i < recon_html_block_count(d); i++) {
        char text[128];
        block_text(d, i, text, sizeof(text));
        if (strstr(text, "Games built") == NULL &&
                strstr(text, "mean something") == NULL) {
            continue;
        }
        seen++;
        const struct recon_html_block_entry *b = recon_html_block_at(d, i);
        check(b != NULL && b->kind == RECON_HTML_HEADING,
            "the half is a heading, not a paragraph");
        check(b != NULL && b->level == 1, "and it is still an h1");
    }
    check(seen == 2, "and the heading did break into two lines");

    recon_html_free(d);
    recon_css_free(sheet);

    /*
     * And the way the page actually writes it, which is the older and far
     * commoner path: a plain `<br>`. No stylesheet involved at all, so every
     * heading, list item and quote with a line break in it was affected --
     * not just the ones a stylesheet had opinions about.
     */
    const char *with_br = "<h1>Games built to<br>mean something.</h1>";
    d = recon_html_parse(with_br, strlen(with_br));
    if (d == NULL) {
        return;
    }
    seen = 0;
    for (int i = 0; i < recon_html_block_count(d); i++) {
        char text[128];
        block_text(d, i, text, sizeof(text));
        if (strstr(text, "Games built") == NULL &&
                strstr(text, "mean something") == NULL) {
            continue;
        }
        seen++;
        const struct recon_html_block_entry *b = recon_html_block_at(d, i);
        check(b != NULL && b->kind == RECON_HTML_HEADING,
            "a <br> ends the line without ending the heading");
        check(b != NULL && b->level == 1, "and it is still an h1");
    }
    check(seen == 2, "and the <br> did break the line");
    recon_html_free(d);

    /* A `<br>` between paragraphs is still what it always was. */
    const char *plain = "<p>one<br>two</p>";
    d = recon_html_parse(plain, strlen(plain));
    if (d != NULL) {
        check(recon_html_block_count(d) == 2, "two lines out of one <p>");
        const struct recon_html_block_entry *b = recon_html_block_at(d, 1);
        check(b != NULL && b->kind == RECON_HTML_PARAGRAPH,
            "and both halves are paragraphs");
        recon_html_free(d);
    }
}

static void test_the_paper_a_page_paints_for_itself(void) {
    printf("a page that sets its own background says so\n");

    /*
     * The one part of "what this page looks like" that can be honoured
     * without any layout at all -- and most of the difference between a site
     * and a transcript of one. Measured on gaming.recontowers.com, which is
     * near-black and came out white.
     */
    unsigned colour = 0;

    /* Nothing said: the paper belongs to whoever is reading. */
    const char *plain = "<p>hello</p>";
    struct recon_html_document *d = recon_html_parse(plain, strlen(plain));
    check(d != NULL && !recon_html_page_background(d, &colour),
        "a page with no opinion has none");
    recon_html_free(d);

    /* Said on body. */
    const char *dark =
        "<style>body { background: #08090c }</style><p>hello</p>";
    struct recon_css_sheet *sheet = recon_css_new();
    d = recon_html_parse_styled(dark, strlen(dark), sheet);
    check(d != NULL && recon_html_page_background(d, &colour),
        "a page that paints its own is heard");
    check(colour == 0x08090c, "in the colour it asked for");
    recon_html_free(d);
    recon_css_free(sheet);

    /* And through a custom property, which is how a page really writes it. */
    const char *via_var =
        "<style>:root { --bg: #112233 } body { background: var(--bg) }</style>"
        "<p>hello</p>";
    sheet = recon_css_new();
    d = recon_html_parse_styled(via_var, strlen(via_var), sheet);
    check(d != NULL && recon_html_page_background(d, &colour) &&
        colour == 0x112233, "including when it is written as var()");
    recon_html_free(d);
    recon_css_free(sheet);

    /*
     * A background on a `<div>` is a box, and there are no boxes here. Taking
     * it as the page's would paint one card's colour behind the whole site.
     */
    const char *inner =
        "<style>div { background: #ff0000 }</style><div><p>hi</p></div>";
    sheet = recon_css_new();
    d = recon_html_parse_styled(inner, strlen(inner), sheet);
    check(d != NULL && !recon_html_page_background(d, &colour),
        "a background on something inside the page is not the page's");
    recon_html_free(d);
    recon_css_free(sheet);
}

static void test_the_named_places_on_a_page(void) {
    printf("an id is a place a link can arrive at\n");

    /*
     * "#install" is how half the web links within itself, and a viewer that
     * cannot find the place reports a dead link on a page where nothing is
     * wrong. Measured on gaming.recontowers.com, whose first link is
     * `<a href="#main">Skip to content</a>` -- the one link on the page whose
     * entire purpose is to be followed by somebody who cannot use a mouse.
     */
    const char *html =
        "<p>before</p>"
        "<main id=\"main\"><h1>The heading</h1></main>"
        "<div id=\"notes\"><p>notes</p></div>";

    struct recon_html_document *d = recon_html_parse(html, strlen(html));
    check(d != NULL, "it parses");
    if (d == NULL) {
        return;
    }

    check(recon_html_anchor_count(d) == 2, "both ids are recorded");

    int main_at = recon_html_anchor_block(d, "main");
    int notes_at = recon_html_anchor_block(d, "notes");
    check(main_at >= 0, "the first is findable");
    check(notes_at >= 0, "and so is the second");
    check(main_at < notes_at, "and they are in the order they appear");

    /*
     * The block an id names is the one its content lands in, so arriving
     * there puts the heading at the top rather than the paragraph above it.
     */
    if (main_at >= 0) {
        char text[128];
        block_text(d, main_at, text, sizeof(text));
        check(strstr(text, "The heading") != NULL,
            "and it lands on the block its content is in");
    }

    check(recon_html_anchor_block(d, "nothing") < 0,
        "a name the page does not have is not found");
    check(recon_html_anchor_block(d, "Main") < 0,
        "and the match is case-sensitive, as the specification says");
    check(recon_html_anchor_block(d, "") < 0, "an empty name finds nothing");
    check(recon_html_anchor_block(NULL, "main") < 0, "so does no document");

    recon_html_free(d);
}

static void test_an_id_inside_something_hidden(void) {
    printf("a place inside something hidden is not a place\n");

    /*
     * Following a link into a closed `<details>` would scroll to a block that
     * was never made, which lands somewhere arbitrary. The id is not recorded
     * at all, so the link reports honestly that the page has no such place.
     */
    const char *html =
        "<p>visible</p>"
        "<template><div id=\"inside\">held</div></template>";

    struct recon_html_document *d = recon_html_parse(html, strlen(html));
    if (d == NULL) {
        return;
    }
    check(recon_html_anchor_block(d, "inside") < 0,
        "an id inside a template names nothing");
    recon_html_free(d);
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
    test_what_a_browser_hides_without_being_told();
    test_a_closed_details_shows_its_summary();
    test_a_span_a_stylesheet_made_a_block();
    test_a_block_inside_a_heading_is_still_the_heading();
    test_the_paper_a_page_paints_for_itself();
    test_the_named_places_on_a_page();
    test_an_id_inside_something_hidden();
    test_nothing_is_refused();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
