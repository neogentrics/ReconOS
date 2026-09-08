/*
 * Tests for reading CSS.
 *
 * The failure worth catching here is a rule that fires when it should not.
 * Everything this reads can hide text -- `display: none` most obviously, but a
 * colour is as good as a hide when it matches the background -- so a selector
 * matched too eagerly is worse than one never matched at all. Half of what is
 * below is checking that things do *not* match.
 *
 * Run with: ./build/recon_css_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>

#include "recon_css.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
    }
}

/* What a sheet says about one element with the given ancestry. */
static struct recon_css_style ask(struct recon_css_sheet *sheet,
        const struct recon_css_element *stack, int depth) {
    struct recon_css_style style;
    memset(&style, 0, sizeof(style));
    recon_css_match(sheet, stack, depth, &style);
    return style;
}

static struct recon_css_sheet *sheet_of(const char *text) {
    struct recon_css_sheet *sheet = recon_css_new();
    if (sheet != NULL) {
        recon_css_add(sheet, text, strlen(text));
    }
    return sheet;
}

/* --- Tests --- */

static void test_colours(void) {
    printf("colours, in the notations pages use\n");

    unsigned c = 0;
    check(recon_css_colour("#abc", &c) && c == 0xAABBCC, "#rgb doubles");
    check(recon_css_colour("#A1B2C3", &c) && c == 0xA1B2C3, "#rrggbb");
    check(recon_css_colour("rgb(1, 2, 3)", &c) && c == 0x010203, "rgb()");
    check(recon_css_colour("rgba(255,0,0,0.5)", &c) && c == 0xFF0000,
        "rgba, with the alpha this has nowhere to put");
    check(recon_css_colour("white", &c) && c == 0xFFFFFF, "a name");
    check(recon_css_colour("  navy", &c) && c == 0x000080, "and leading space");

    /*
     * These are not colours, and answering with one would paint text in a
     * value nobody wrote.
     */
    check(!recon_css_colour("transparent", &c), "transparent is not a colour");
    check(!recon_css_colour("inherit", &c), "nor is inherit");
    check(!recon_css_colour("#12", &c), "nor is a short hex");
    check(!recon_css_colour("chartreuse", &c),
        "a name this does not know is unset, not a guess");
}

static void test_declarations(void) {
    printf("what a declaration block says\n");

    struct recon_css_style s;
    memset(&s, 0, sizeof(s));
    const char *text = "color: red; font-weight: bold; display: none";
    recon_css_inline(text, strlen(text), &s);

    check(s.has_colour && s.colour == 0xFF0000, "the colour is read");
    check(s.weight == 700, "and the weight");
    check(s.display == RECON_CSS_NONE, "and the hiding");

    /* One broken line does not lose the rest: a stylesheet with a typo in it
     * is normal, and the properties around it are still worth having. */
    memset(&s, 0, sizeof(s));
    text = "color red; font-weight: bold";
    recon_css_inline(text, strlen(text), &s);
    check(!s.has_colour, "a declaration with no colon is skipped");
    check(s.weight == 700, "and the one after it still applies");

    /* !important is read off and thrown away rather than left in the value,
     * where it would stop the colour parsing at all. */
    memset(&s, 0, sizeof(s));
    text = "color: #00ff00 !important";
    recon_css_inline(text, strlen(text), &s);
    check(s.has_colour && s.colour == 0x00FF00, "!important does not eat the value");

    /* visibility: hidden is a hide, because a reader with no layout has no
     * space to leave -- and "shown" would put back what somebody hid. */
    memset(&s, 0, sizeof(s));
    text = "visibility: hidden";
    recon_css_inline(text, strlen(text), &s);
    check(s.display == RECON_CSS_NONE, "visibility: hidden hides");
}

static void test_font_size(void) {
    printf("font-size, as a percentage of whatever this would be\n");

    struct recon_css_style s;
    const struct { const char *text; int want; } CASES[] = {
        { "font-size: 200%", 200 },
        { "font-size: 2em", 200 },
        { "font-size: 32px", 200 },
        { "font-size: 8px", 50 },
        { "font-size: large", 120 },
    };
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        memset(&s, 0, sizeof(s));
        recon_css_inline(CASES[i].text, strlen(CASES[i].text), &s);
        char what[96];
        snprintf(what, sizeof(what), "%s is %d%% (got %d)", CASES[i].text,
            CASES[i].want, s.size_percent);
        check(s.size_percent == CASES[i].want, what);
    }

    /* A size nobody could read, and one that would fill the window. Clamped
     * rather than refused: the rest of the rule is fine. */
    memset(&s, 0, sizeof(s));
    recon_css_inline("font-size: 1px", 14, &s);
    check(s.size_percent >= 40, "an unreadable size is clamped up");
    memset(&s, 0, sizeof(s));
    recon_css_inline("font-size: 900px", 16, &s);
    check(s.size_percent <= 400, "and an enormous one down");
}

static void test_selectors_that_match(void) {
    printf("selectors this can honour\n");

    struct recon_css_sheet *sheet = sheet_of(
        "p { color: red }"
        ".hidden { display: none }"
        "#main { color: blue }"
        "p.lead { font-weight: bold }"
        "div.body p { font-style: italic }");
    check(sheet != NULL, "the sheet parses");
    if (sheet == NULL) {
        return;
    }
    check(recon_css_rule_count(sheet) == 5, "five rules");

    struct recon_css_element p = { "p", NULL, NULL };
    struct recon_css_style s = ask(sheet, &p, 1);
    check(s.has_colour && s.colour == 0xFF0000, "a type selector matches");
    check(s.display == 0, "and says nothing about hiding");

    struct recon_css_element hidden = { "span", NULL, "hidden" };
    s = ask(sheet, &hidden, 1);
    check(s.display == RECON_CSS_NONE, "a class selector matches");

    struct recon_css_element main_div = { "div", "main", NULL };
    s = ask(sheet, &main_div, 1);
    check(s.has_colour && s.colour == 0x0000FF, "an id selector matches");

    struct recon_css_element lead = { "p", NULL, "lead intro" };
    s = ask(sheet, &lead, 1);
    check(s.weight == 700, "a compound matches when both halves do");
    check(s.has_colour && s.colour == 0xFF0000,
        "and the type rule still applies underneath it");

    /* A descendant, which is the one combinator a flat reader can honour. */
    struct recon_css_element stack[3] = {
        { "body", NULL, NULL },
        { "div", NULL, "body" },
        { "p", NULL, NULL },
    };
    s = ask(sheet, stack, 3);
    check(s.italic_set && s.italic, "a descendant selector matches");

    /* The same paragraph outside that div does not get it. */
    struct recon_css_element elsewhere[2] = {
        { "body", NULL, NULL },
        { "p", NULL, NULL },
    };
    s = ask(sheet, elsewhere, 2);
    check(!s.italic, "and does not match outside the ancestor it names");

    recon_css_free(sheet);
}

static void test_selectors_that_must_not_match(void) {
    printf("selectors this cannot honour are kept out, not half-matched\n");

    /*
     * Every one of these would hide something if it were read as its bare
     * type or class. `a:hover` is not `a`; `input[type=text]` is not `input`.
     */
    struct recon_css_sheet *sheet = sheet_of(
        "a:hover { display: none }"
        "input[type=hidden] { display: none }"
        "div > p { display: none }"
        "h1 + p { display: none }"
        "li ~ li { display: none }"
        "::before { display: none }");
    check(sheet != NULL, "it parses");
    if (sheet == NULL) {
        return;
    }
    check(recon_css_rule_count(sheet) == 0,
        "and contributes no rules at all");

    const struct recon_css_element ELEMENTS[] = {
        { "a", NULL, NULL },
        { "input", NULL, NULL },
        { "p", NULL, NULL },
        { "li", NULL, NULL },
        { "h1", NULL, NULL },
    };
    for (size_t i = 0; i < sizeof(ELEMENTS) / sizeof(ELEMENTS[0]); i++) {
        struct recon_css_style s = ask(sheet, &ELEMENTS[i], 1);
        char what[96];
        snprintf(what, sizeof(what), "<%s> is not hidden by any of them",
            ELEMENTS[i].tag);
        check(s.display != RECON_CSS_NONE, what);
    }

    recon_css_free(sheet);
}

static void test_the_class_list(void) {
    printf("a class matches a name in the list, not a piece of one\n");

    struct recon_css_sheet *sheet = sheet_of(".nav { display: none }");
    if (sheet == NULL) {
        return;
    }

    struct recon_css_element yes = { "div", NULL, "header nav wide" };
    check(ask(sheet, &yes, 1).display == RECON_CSS_NONE,
        "a name in the middle of the list matches");

    /*
     * The failure this guards against is a substring test: "navigation"
     * contains "nav", and a page whose article is in class "navigation" would
     * disappear.
     */
    struct recon_css_element no = { "div", NULL, "navigation" };
    check(ask(sheet, &no, 1).display != RECON_CSS_NONE,
        "a longer name that contains it does not");

    struct recon_css_element also_no = { "div", NULL, "subnav" };
    check(ask(sheet, &also_no, 1).display != RECON_CSS_NONE,
        "nor does one that ends with it");

    recon_css_free(sheet);
}

static void test_order_and_specificity(void) {
    printf("which rule wins\n");

    struct recon_css_sheet *sheet = sheet_of(
        "p { color: red }"
        "p { color: green }");
    if (sheet != NULL) {
        struct recon_css_element p = { "p", NULL, NULL };
        check(ask(sheet, &p, 1).colour == 0x008000,
            "the later of two equal rules");
        recon_css_free(sheet);
    }

    /* A class beats a type however they are ordered, which is the half of the
     * cascade that is not file order. */
    sheet = sheet_of(
        ".loud { color: red }"
        "p { color: green }");
    if (sheet != NULL) {
        struct recon_css_element p = { "p", NULL, "loud" };
        check(ask(sheet, &p, 1).colour == 0xFF0000,
            "a class beats a type written after it");
        recon_css_free(sheet);
    }

    /* And an inline style beats the sheet, which is what writing one means. */
    sheet = sheet_of("p { color: red }");
    if (sheet != NULL) {
        struct recon_css_element p = { "p", NULL, NULL };
        struct recon_css_style s = ask(sheet, &p, 1);
        recon_css_inline("color: blue", 11, &s);
        check(s.colour == 0x0000FF, "an inline style beats the sheet");
        recon_css_free(sheet);
    }
}

static void test_at_rules_and_comments(void) {
    printf("comments, and blocks this skips whole\n");

    struct recon_css_sheet *sheet = sheet_of(
        "/* a comment { p { display: none } } */"
        "@media print { p { display: none } }"
        "@import url(other.css);"
        "p { color: red }");
    if (sheet == NULL) {
        return;
    }

    struct recon_css_element p = { "p", NULL, NULL };
    struct recon_css_style s = ask(sheet, &p, 1);
    check(s.display != RECON_CSS_NONE,
        "nothing inside a comment or an @media applies");
    check(s.has_colour && s.colour == 0xFF0000,
        "and the rule after them still does");
    check(recon_css_rule_count(sheet) == 1, "one rule out of all that");

    recon_css_free(sheet);
}

static void test_nothing_is_refused(void) {
    printf("there is no such thing as CSS this refuses\n");

    const char *nonsense[] = {
        "", "{", "}", "{}", "p", "p {", "p { color", "p { color: }",
        "/*", "@media", "@", ",,,", "p,, {color:red}", "#{}", ".{}",
    };
    for (size_t i = 0; i < sizeof(nonsense) / sizeof(nonsense[0]); i++) {
        struct recon_css_sheet *sheet = sheet_of(nonsense[i]);
        char what[96];
        snprintf(what, sizeof(what), "'%s' is read without falling over",
            nonsense[i]);
        check(sheet != NULL, what);
        recon_css_free(sheet);
    }
}

int main(void) {
    printf("ReconOS CSS tests\n\n");

    test_colours();
    test_declarations();
    test_font_size();
    test_selectors_that_match();
    test_selectors_that_must_not_match();
    test_the_class_list();
    test_order_and_specificity();
    test_at_rules_and_comments();
    test_nothing_is_refused();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
