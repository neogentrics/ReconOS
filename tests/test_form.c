/*
 * Tests for what a form sends.
 *
 * These rules decide what leaves the machine, and every one of them is the
 * kind that is wrong in one case out of eight without anything looking wrong:
 * a request the server rejects for a reason nobody can see, or -- worse -- one
 * it accepts with an answer missing.
 *
 * The forms below are copied off real pages rather than invented. That is the
 * lesson of BG-162, where a test was written against markup that had been
 * assumed, passed, and proved nothing.
 *
 * Run with: ./build/recon_form_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_form.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
    }
}

static void check_text(const char *got, const char *want, const char *what) {
    g_checks++;
    if (got == NULL || strcmp(got, want) != 0) {
        g_failures++;
        printf("  FAIL: %s\n    wanted: %s\n    got:    %s\n", what, want,
            got != NULL ? got : "(nothing)");
    }
}

/* Every control at whatever the page says it starts out holding. */
static struct recon_form_value *defaults_of(
        const struct recon_html_document *d) {
    int count = recon_html_field_count(d);
    if (count <= 0) {
        return NULL;
    }
    struct recon_form_value *values = calloc((size_t)count, sizeof(*values));
    if (values == NULL) {
        return NULL;
    }
    for (int i = 0; i < count; i++) {
        const struct recon_html_field *f = recon_html_field_at(d, i);
        values[i].text = f->value;
        values[i].on = f->on;
        values[i].chosen = 0;
        for (int o = 0; o < f->option_count; o++) {
            const struct recon_html_option *opt =
                recon_html_option_at(d, f->first_option + o);
            if (opt != NULL && opt->selected) {
                values[i].chosen = o;
                break;
            }
        }
    }
    return values;
}

/* --- The encoding --- */

static void test_the_encoding(void) {
    printf("a value is encoded the way a form encodes it\n");

    char out[256];

    recon_form_encode("pizza", out, sizeof(out));
    check_text(out, "pizza", "letters are themselves");

    recon_form_encode("deep pan", out, sizeof(out));
    check_text(out, "deep+pan",
        "a space is a plus, which is this encoding's one oddity");

    recon_form_encode("a&b=c", out, sizeof(out));
    check_text(out, "a%26b%3Dc",
        "the two characters that would otherwise be structure");

    recon_form_encode("-_.~", out, sizeof(out));
    check_text(out, "-_.~", "the unreserved punctuation is left alone");

    /*
     * The four that used to break a search: a slash, a plus somebody typed,
     * a percent sign, and a quote. A literal "+" that arrived unencoded would
     * come back as a space, so "C++" would be searched for as "C  ".
     */
    recon_form_encode("C++", out, sizeof(out));
    check_text(out, "C%2B%2B", "a typed plus is not a space");

    recon_form_encode("100%", out, sizeof(out));
    check_text(out, "100%25", "a percent sign is escaped, or it eats two bytes");

    recon_form_encode("a/b", out, sizeof(out));
    check_text(out, "a%2Fb", "a slash inside a value is not a path");

    /* Not ASCII. UTF-8 goes byte by byte, which is what a server expects. */
    recon_form_encode("caf\xC3\xA9", out, sizeof(out));
    check_text(out, "caf%C3%A9", "UTF-8 is encoded a byte at a time");

    /*
     * And it refuses rather than truncating. Half an encoded value is a
     * different value, and a search for half of what somebody typed looks
     * like the search working.
     */
    char tiny[4];
    check(recon_form_encode("abcdef", tiny, sizeof(tiny)) == 0,
        "a value that will not fit is refused, not cut");
    check(tiny[0] == '\0', "and nothing is left behind in the buffer");
}

/* --- What each kind of control contributes --- */

/*
 * The HTML5 standard's own example form, which httpbin serves at /forms/post.
 * Not written here.
 */
static void test_the_standards_example_form(void) {
    printf("the standard's example form, sent\n");

    const char *html =
        "<form method=\"post\" action=\"/post\">"
        "<p><label>Customer name: <input name=\"custname\"></label></p>"
        "<p><label><input type=radio name=size value=\"small\"> Small</label>"
        "<label><input type=radio name=size value=\"medium\" checked> Medium"
        "</label>"
        "<label><input type=radio name=size value=\"large\"> Large</label></p>"
        "<p><label><input type=checkbox name=topping value=\"bacon\"> Bacon"
        "</label>"
        "<label><input type=checkbox name=topping value=\"cheese\" checked>"
        " Cheese</label></p>"
        "<p><textarea name=\"comments\"></textarea></p>"
        "<p><button name=\"act\" value=\"order\">Submit order</button></p>"
        "</form>";

    struct recon_html_document *d = recon_html_parse(html, strlen(html));
    check(d != NULL, "it parses");
    if (d == NULL) {
        return;
    }
    struct recon_form_value *values = defaults_of(d);
    check(values != NULL, "and has controls");
    if (values == NULL) {
        recon_html_free(d);
        return;
    }
    int count = recon_html_field_count(d);

    /* The button is the last control, and it is the one pressed. */
    int button = count - 1;
    int sent = 0;
    bool secret = false;
    char *body = recon_form_body(d, 0, button, values, count, 4096, &sent,
        &secret);
    check(body != NULL, "it builds a body");

    if (body != NULL) {
        /*
         * Every rule at once. The unchecked box is *absent* rather than
         * empty, the checked one carries its own value, only one radio
         * appears, and the button that was pressed is the only one sent.
         */
        check_text(body,
            "custname=&size=medium&topping=cheese&comments=&act=order",
            "the defaults, in document order");
        check(sent == 5, "five answers");
        check(!secret, "and none of them a password");
        free(body);
    }

    /* Now with somebody's answers in it. */
    values[0].text = "Ada Lovelace";
    values[1].on = false;                        /* small */
    values[2].on = false;                        /* medium */
    values[3].on = true;                         /* large */
    values[4].on = true;                         /* bacon */
    values[6].text = "Leave it at the door";     /* comments */

    body = recon_form_body(d, 0, button, values, count, 4096, &sent, NULL);
    check(body != NULL, "it builds a body from what was typed");
    if (body != NULL) {
        check_text(body,
            "custname=Ada+Lovelace&size=large&topping=bacon&topping=cheese"
            "&comments=Leave+it+at+the+door&act=order",
            "two toppings under one name, in the order the page has them");
        free(body);
    }

    /*
     * Enter pressed in a text box rather than a button clicked. The form goes
     * and the button's own name does not, because no button was pressed --
     * which is what a browser does and what a server distinguishing Save from
     * Delete depends on.
     */
    body = recon_form_body(d, 0, -1, values, count, 4096, &sent, NULL);
    if (body != NULL) {
        check(strstr(body, "act=order") == NULL,
            "no button pressed means no button sent");
        free(body);
    }

    free(values);
    recon_html_free(d);
}

/*
 * Wikipedia's search form. The hidden field is the whole test: without it the
 * request goes somewhere Wikipedia does not serve.
 */
static void test_a_real_search_form(void) {
    printf("a search form sends its hidden field too\n");

    const char *html =
        "<form action=\"/w/index.php\" id=\"searchform\">"
        "<input type=\"search\" name=\"search\" placeholder=\"Search "
        "Wikipedia\">"
        "<input type=\"hidden\" name=\"title\" value=\"Special:Search\">"
        "<input type=\"submit\" value=\"Search\">"
        "</form>";

    struct recon_html_document *d = recon_html_parse(html, strlen(html));
    check(d != NULL, "it parses");
    if (d == NULL) {
        return;
    }
    struct recon_form_value *values = defaults_of(d);
    if (values == NULL) {
        recon_html_free(d);
        return;
    }
    int count = recon_html_field_count(d);
    values[0].text = "pixman";

    char *body = recon_form_body(d, 0, -1, values, count, 4096, NULL, NULL);
    check_text(body, "search=pixman&title=Special%3ASearch",
        "the typed word and the hidden field, the colon escaped");
    free(body);

    /*
     * The submit button carries no name, so pressing it adds nothing. A
     * viewer that sent `=Search` because the button had a value would put an
     * unnamed parameter into every search.
     */
    body = recon_form_body(d, 0, 2, values, count, 4096, NULL, NULL);
    check_text(body, "search=pixman&title=Special%3ASearch",
        "a button with no name sends nothing even when it is pressed");
    free(body);

    /* And into the address. */
    struct recon_http_url where;
    check(recon_http_parse_url("https://en.wikipedia.org/w/index.php", NULL,
        &where), "the action resolves");
    body = recon_form_body(d, 0, -1, values, count, 4096, NULL, NULL);
    check(recon_form_get_address(&where, body), "and takes the answers");
    check_text(where.path, "/w/index.php?search=pixman&title=Special%3ASearch",
        "which is the address a search goes to");
    free(body);

    free(values);
    recon_html_free(d);
}

/* --- The rules that are easy to get wrong --- */

static void test_what_is_left_out(void) {
    printf("what a form does not send\n");

    const char *html =
        "<form action=\"/s\">"
        "<input name=\"kept\" value=\"1\">"
        "<input value=\"no name at all\">"
        "<input name=\"off\" disabled value=\"2\">"
        "<input type=checkbox name=\"box\" value=\"3\">"
        "<input type=reset name=\"clear\" value=\"Clear\">"
        "<button type=\"button\" name=\"more\" value=\"4\">Show more</button>"
        "<input type=hidden name=\"token\" value=\"abc\">"
        "</form>";

    struct recon_html_document *d = recon_html_parse(html, strlen(html));
    check(d != NULL, "it parses");
    if (d == NULL) {
        return;
    }
    struct recon_form_value *values = defaults_of(d);
    if (values == NULL) {
        recon_html_free(d);
        return;
    }
    int count = recon_html_field_count(d);

    char *body = recon_form_body(d, 0, -1, values, count, 4096, NULL, NULL);
    check_text(body, "kept=1&token=abc",
        "no name, disabled, unchecked, reset and script-only are all absent");
    free(body);

    /* And a checkbox appears the moment it is ticked. */
    values[3].on = true;
    body = recon_form_body(d, 0, -1, values, count, 4096, NULL, NULL);
    check_text(body, "kept=1&box=3&token=abc",
        "a ticked box appears where the page put it");
    free(body);

    free(values);
    recon_html_free(d);
}

static void test_a_box_with_no_value_of_its_own(void) {
    printf("a checkbox with no value sends \"on\"\n");

    /* Which is the standard, and is what a server checking for presence and
     * a server checking for "on" both expect. */
    const char *html =
        "<form action=\"/s\"><input type=checkbox name=\"agree\" checked>"
        "</form>";

    struct recon_html_document *d = recon_html_parse(html, strlen(html));
    if (d == NULL) {
        check(false, "it parses");
        return;
    }
    struct recon_form_value *values = defaults_of(d);
    if (values != NULL) {
        char *body = recon_form_body(d, 0, -1, values,
            recon_html_field_count(d), 4096, NULL, NULL);
        check_text(body, "agree=on", "\"on\", not empty");
        free(body);
        free(values);
    }
    recon_html_free(d);
}

static void test_a_menu_sends_its_value_not_its_words(void) {
    printf("a menu sends the value, not what is shown\n");

    const char *html =
        "<form action=\"/find\"><select name=\"where\">"
        "<option value=\"door\">The front door</option>"
        "<option value=\"desk\" selected>The reception desk</option>"
        "<option>The car park</option>"
        "</select></form>";

    struct recon_html_document *d = recon_html_parse(html, strlen(html));
    if (d == NULL) {
        check(false, "it parses");
        return;
    }
    struct recon_form_value *values = defaults_of(d);
    if (values != NULL) {
        char *body = recon_form_body(d, 0, -1, values,
            recon_html_field_count(d), 4096, NULL, NULL);
        check_text(body, "where=desk",
            "the one marked selected, by its value");
        free(body);

        values[0].chosen = 2;
        body = recon_form_body(d, 0, -1, values,
            recon_html_field_count(d), 4096, NULL, NULL);
        check_text(body, "where=The+car+park",
            "an option with no value of its own sends its own words");
        free(body);
        free(values);
    }
    recon_html_free(d);
}

static void test_a_password_is_flagged(void) {
    printf("a form carrying a password says so\n");

    /* Not so it can be treated differently in the request -- it is not -- but
     * so the question asked before a POST can name it. */
    const char *html =
        "<form method=post action=\"/in\">"
        "<input name=\"user\"><input type=password name=\"pw\">"
        "</form>";

    struct recon_html_document *d = recon_html_parse(html, strlen(html));
    if (d == NULL) {
        check(false, "it parses");
        return;
    }
    struct recon_form_value *values = defaults_of(d);
    if (values != NULL) {
        int count = 0;
        bool secret = false;
        values[0].text = "ada";
        values[1].text = "hunter2";
        char *body = recon_form_body(d, 0, -1, values,
            recon_html_field_count(d), 4096, &count, &secret);
        check_text(body, "user=ada&pw=hunter2", "both are sent");
        check(count == 2 && secret,
            "and it is known that one of them is a password");
        free(body);
        free(values);
    }
    recon_html_free(d);
}

/*
 * Two forms on one page. Sending one must not sweep up the other's controls.
 */
static void test_two_forms_do_not_mix(void) {
    printf("one form's controls stay in one form\n");

    const char *html =
        "<form action=\"/a\"><input name=\"one\" value=\"1\"></form>"
        "<form action=\"/b\"><input name=\"two\" value=\"2\"></form>"
        "<input name=\"loose\" value=\"3\">";

    struct recon_html_document *d = recon_html_parse(html, strlen(html));
    if (d == NULL) {
        check(false, "it parses");
        return;
    }
    struct recon_form_value *values = defaults_of(d);
    if (values != NULL) {
        int count = recon_html_field_count(d);
        char *a = recon_form_body(d, 0, -1, values, count, 4096, NULL, NULL);
        char *b = recon_form_body(d, 1, -1, values, count, 4096, NULL, NULL);
        check_text(a, "one=1", "the first form sends its own");
        check_text(b, "two=2", "and so does the second");
        free(a);
        free(b);
        free(values);
    }
    recon_html_free(d);
}

/*
 * The address a GET makes, including the case the standard is explicit about
 * and almost nothing implements: the action's own query is replaced.
 */
static void test_the_address_a_get_makes(void) {
    printf("a GET's answers replace the action's query\n");

    struct recon_http_url where;
    check(recon_http_parse_url("https://example.com/s?lang=en", NULL, &where),
        "an action with a query of its own parses");
    check(recon_form_get_address(&where, "q=pizza"), "and takes the answers");
    check_text(where.path, "/s?q=pizza",
        "the form's fields replace it, which is what the standard says");

    check(recon_http_parse_url("https://example.com", NULL, &where),
        "an address with no path parses");
    check(recon_form_get_address(&where, "q=pizza"), "and takes the answers");
    check_text(where.path, "/?q=pizza", "and asks the root");

    /* A fragment does not survive a submission: the answer is a new document
     * and not a place on the form. */
    check(recon_http_parse_url("https://example.com/s#top", NULL, &where),
        "an action with a fragment parses");
    recon_form_get_address(&where, "q=x");
    check(where.fragment[0] == '\0', "the fragment does not come along");

    /* And an answer too long to fit is refused rather than cut. */
    char huge[RECON_HTTP_URL_MAX + 64];
    memset(huge, 'x', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    check(recon_http_parse_url("https://example.com/s", NULL, &where),
        "a plain action parses");
    check(!recon_form_get_address(&where, huge),
        "an address that would be too long is refused");
    check_text(where.path, "/s", "and the address is left as it was");
}

/*
 * A body that will not fit is no body at all.
 *
 * The one that matters most here. A request arriving with its last two
 * answers missing is worse than one not arriving, because the server accepts
 * it -- so this refuses, and the viewer above says so.
 */
static void test_a_body_too_long_is_refused(void) {
    printf("a form bigger than the request is refused, not cut\n");

    const char *html =
        "<form action=\"/s\">"
        "<input name=\"a\" value=\"1111111111\">"
        "<input name=\"b\" value=\"2222222222\">"
        "<input name=\"c\" value=\"3333333333\">"
        "</form>";

    struct recon_html_document *d = recon_html_parse(html, strlen(html));
    if (d == NULL) {
        check(false, "it parses");
        return;
    }
    struct recon_form_value *values = defaults_of(d);
    if (values != NULL) {
        int count = recon_html_field_count(d);

        char *whole = recon_form_body(d, 0, -1, values, count, 4096, NULL,
            NULL);
        check_text(whole, "a=1111111111&b=2222222222&c=3333333333",
            "all three fit in a large enough request");
        free(whole);

        check(recon_form_body(d, 0, -1, values, count, 20, NULL, NULL) == NULL,
            "and in a small one, none of them are sent");
        free(values);
    }
    recon_html_free(d);
}

int main(void) {
    printf("ReconOS form tests\n\n");

    test_the_encoding();
    test_the_standards_example_form();
    test_a_real_search_form();
    test_what_is_left_out();
    test_a_box_with_no_value_of_its_own();
    test_a_menu_sends_its_value_not_its_words();
    test_a_password_is_flagged();
    test_two_forms_do_not_mix();
    test_the_address_a_get_makes();
    test_a_body_too_long_is_refused();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
