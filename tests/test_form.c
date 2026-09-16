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
static void test_a_file_field_is_not_a_text_box(void) {
    printf("a file field is its own kind\n");

    /*
     * **It used to be a text box**, because `type=file` fell through the
     * dispatch to the default -- and a text box is exactly the wrong answer,
     * because it is one somebody can type into. A page with a file field
     * drawn as a text box invites a person to type a filename, press Send,
     * and have the server receive a word where it expected a document.
     *
     * A photograph of a real page is what showed it: two file fields
     * indistinguishable from the name field beside them.
     */
    const char *html =
        "<form method=post action=/sent enctype=\"multipart/form-data\">"
        "<input type=text name=who value=Joshua>"
        "<input type=file name=picture>"
        "<input type=submit value=Send>"
        "</form>";

    struct recon_html_document *d = recon_html_parse(html, strlen(html));
    check(d != NULL, "it parses");
    if (d == NULL) {
        return;
    }

    check(recon_html_field_count(d) == 3, "three controls");

    const struct recon_html_field *text = recon_html_field_at(d, 0);
    const struct recon_html_field *file = recon_html_field_at(d, 1);
    check(text != NULL && text->kind == RECON_HTML_FIELD_TEXT,
        "the first is a text box");
    check(file != NULL && file->kind == RECON_HTML_FIELD_FILE,
        "and the second is a file field, not another text box");

    /* And it says what it is, because a file input carries no words of its
     * own -- the standard forbids a page setting its value. */
    if (file != NULL) {
        check(file->label[0] != '\0',
            "and it has words, which the page did not give it");
    }

    recon_html_free(d);
}

static void test_which_forms_ask_for_multipart(void) {
    printf("enctype, and only the one value that means it\n");

    /*
     * Only `multipart/form-data` counts. `enctype` also takes `text/plain`,
     * which almost nothing uses and which a url-encoded body is not, and
     * anything unreadable is the page being wrong.
     *
     * Neither is treated as multipart, and the reason is a cost: a form
     * marked this way **is refused rather than sent**, so reading the mark
     * too generously would stop forms working that work today.
     */
    const char *html =
        "<form id=a method=post enctype=\"multipart/form-data\">"
        "<input type=submit></form>"
        "<form id=b method=post enctype=\"text/plain\">"
        "<input type=submit></form>"
        "<form id=c method=post enctype=\"banana\">"
        "<input type=submit></form>"
        "<form id=d method=post><input type=submit></form>";

    struct recon_html_document *d = recon_html_parse(html, strlen(html));
    check(d != NULL, "it parses");
    if (d == NULL) {
        return;
    }

    check(recon_html_form_count(d) == 4, "four forms");
    if (recon_html_form_count(d) == 4) {
        check(recon_html_form_at(d, 0)->wants_files,
            "multipart/form-data asks for files");
        check(!recon_html_form_at(d, 1)->wants_files,
            "text/plain does not");
        check(!recon_html_form_at(d, 2)->wants_files,
            "nor does a value that is not an enctype at all");
        check(!recon_html_form_at(d, 3)->wants_files,
            "nor does a form with no enctype");
    }

    recon_html_free(d);
}

static void test_a_file_field_in_an_ordinary_form(void) {
    printf("what a file field sends when the form is not multipart\n");

    /*
     * **The name, with an empty value** -- and that is not a stand-in, it is
     * the right answer.
     *
     * The standard has a form that does not ask for multipart send a file
     * field's *name* rather than its content, and a browser with no file
     * chosen sends nothing after the equals sign. This viewer has no file
     * chosen either, so the request it makes and the one a browser makes are
     * the same bytes.
     *
     * A form that *does* ask for multipart never reaches this code: the
     * viewer refuses it, because a url-encoded body is not a degraded
     * multipart one, it is an unintelligible one.
     */
    const char *html =
        "<form method=post action=/sent>"
        "<input type=file name=stray>"
        "<input type=text name=notes value=ordinary>"
        "<input type=submit value=Send>"
        "</form>";

    struct recon_html_document *d = recon_html_parse(html, strlen(html));
    check(d != NULL, "it parses");
    if (d == NULL) {
        return;
    }
    struct recon_form_value *values = defaults_of(d);
    if (values == NULL) {
        check(false, "it has controls");
        recon_html_free(d);
        return;
    }

    int count = recon_html_field_count(d);
    int sent = 0;
    char *body = recon_form_body(d, 0, count - 1, values, count, 4096, &sent,
        NULL);
    check(body != NULL, "it builds a body");

    if (body != NULL) {
        /* The exact bytes a browser sends for this form with nothing
         * attached, which is what the echo server confirmed. */
        check_text(body, "stray=&notes=ordinary",
            "the file field sends its name and nothing after it");
        free(body);
    }

    free(values);
    recon_html_free(d);
}

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

/*
 * What a form sends when it attaches a file.
 *
 * A url-encoded body is text and a wrong one looks wrong. A multipart body is
 * a small protocol with a delimiter in it, and a wrong one **parses** -- the
 * server reads it, accepts it, and stores something other than what was sent.
 * So these hold the parts of it that fail quietly.
 */

/* A page copied off a real upload form: a caption, a file, and a button. */
static const char UPLOAD[] =
    "<form action=\"/upload\" method=\"post\" enctype=\"multipart/form-data\">"
    "<input name=\"caption\" value=\"a cat\">"
    "<input type=\"file\" name=\"photo\">"
    "<input type=\"submit\" name=\"go\" value=\"Upload\">"
    "</form>";

/* Whether `body` contains `needle` over `length` bytes rather than as text. */
static bool holds(const char *body, size_t length, const char *needle) {
    size_t n = strlen(needle);
    if (body == NULL || n == 0 || length < n) {
        return false;
    }
    for (size_t i = 0; i + n <= length; i++) {
        if (memcmp(body + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

static void test_a_file_part_carries_the_bytes(void) {
    printf("a file part carries the bytes, the name, and the type\n");

    struct recon_html_document *d = recon_html_parse(UPLOAD, sizeof(UPLOAD) - 1);
    int count = recon_html_field_count(d);
    struct recon_form_value *values = defaults_of(d);

    /* A PNG's signature, which has a zero byte in it -- the reason this call
     * exists rather than the string-returning one. */
    static const char PNG[] = "\x89PNG\r\n\x1a\n" "\0\0\0\rIHDR";
    values[1].file_name = "cat.png";
    values[1].file_bytes = PNG;
    values[1].file_length = sizeof(PNG) - 1;

    char boundary[RECON_FORM_BOUNDARY_MAX];
    size_t length = 0;
    int sent = 0;
    char *body = recon_form_body_multipart(d, 0, 2, values, count, 4096,
        boundary, sizeof(boundary), &length, &sent, NULL);

    check(body != NULL, "the body is built");
    if (body != NULL) {
        check(sent == 3, "the caption, the file and the button pressed");
        check(holds(body, length,
                "Content-Disposition: form-data; name=\"caption\"\r\n\r\n"
                "a cat\r\n"),
            "a text part is its name, a blank line, and its value");
        check(holds(body, length,
                "Content-Disposition: form-data; name=\"photo\"; "
                "filename=\"cat.png\"\r\nContent-Type: image/png\r\n\r\n"),
            "a file part names the file and says what kind it is");
        check(holds(body, length, PNG),
            "and the file's own bytes are in it, zero byte and all");

        /*
         * The one that would be missed by reading the body as a string: it has
         * a NUL in the middle, so `strlen` stops at the signature and reports
         * a body about a fifth of its real size. Every part after the photo
         * would be silently absent from anything measuring it that way.
         */
        check(strlen(body) < length,
            "the length is not the string length -- a file contains zeroes");
        check(length > 200, "and the real length counts every part");
        free(body);
    }

    free(values);
    recon_html_free(d);
}

static void test_the_last_line_closes_it(void) {
    printf("the body ends with the closing boundary\n");

    struct recon_html_document *d = recon_html_parse(UPLOAD, sizeof(UPLOAD) - 1);
    int count = recon_html_field_count(d);
    struct recon_form_value *values = defaults_of(d);

    char boundary[RECON_FORM_BOUNDARY_MAX];
    size_t length = 0;
    char *body = recon_form_body_multipart(d, 0, -1, values, count, 4096,
        boundary, sizeof(boundary), &length, NULL, NULL);

    check(body != NULL, "the body is built");
    if (body != NULL) {
        char tail[RECON_FORM_BOUNDARY_MAX + 8];
        snprintf(tail, sizeof(tail), "--%s--\r\n", boundary);
        size_t n = strlen(tail);
        check(length >= n && memcmp(body + length - n, tail, n) == 0,
            "the last line is the boundary with two dashes after it");

        /*
         * Not decoration. A body whose final delimiter is missing or is the
         * ordinary one is a body the server reads as still arriving, and it
         * either waits for the rest or throws the last part away. Either way
         * the answer somebody typed does not get stored, and nothing about the
         * request looks wrong.
         */
        snprintf(tail, sizeof(tail), "--%s\r\n", boundary);
        check(!holds(body + length - strlen(tail), strlen(tail), tail),
            "and it is not the ordinary delimiter, which means 'more coming'");
        free(body);
    }

    free(values);
    recon_html_free(d);
}

static void test_the_boundary_is_not_in_the_content(void) {
    printf("a boundary that occurs in the file is not used\n");

    struct recon_html_document *d = recon_html_parse(UPLOAD, sizeof(UPLOAD) - 1);
    int count = recon_html_field_count(d);
    struct recon_form_value *values = defaults_of(d);

    /*
     * Somebody uploads a file that contains the boundary this would otherwise
     * have chosen. That is not a far-fetched file: a saved HTTP request, a
     * bug report with a request pasted into it, or a second upload captured
     * from this very program.
     *
     * If the boundary were drawn at random and not checked, this body would
     * split at that point: the server would read the file as ending early and
     * read whatever follows in the file as further form fields. Fields the
     * person filling in the form never saw and never agreed to send.
     */
    static const char ATTACK[] =
        "harmless text\r\n"
        "----ReconOSForm0\r\n"
        "Content-Disposition: form-data; name=\"role\"\r\n"
        "\r\n"
        "administrator\r\n";
    values[1].file_name = "notes.txt";
    values[1].file_bytes = ATTACK;
    values[1].file_length = sizeof(ATTACK) - 1;

    char boundary[RECON_FORM_BOUNDARY_MAX];
    size_t length = 0;
    char *body = recon_form_body_multipart(d, 0, -1, values, count, 4096,
        boundary, sizeof(boundary), &length, NULL, NULL);

    check(body != NULL, "the body is still built");
    if (body != NULL) {
        check(strcmp(boundary, "----ReconOSForm0") != 0,
            "the boundary in the file is not the boundary chosen");
        check(!holds(ATTACK, sizeof(ATTACK) - 1, boundary),
            "and the one chosen appears nowhere in the file");
        check(holds(body, length, ATTACK),
            "the file is still sent whole, not edited to make room");
        free(body);
    }

    free(values);
    recon_html_free(d);
}

static void test_a_quote_in_a_name_cannot_add_an_attribute(void) {
    printf("a quote in a filename does not write a second attribute\n");

    struct recon_html_document *d = recon_html_parse(UPLOAD, sizeof(UPLOAD) - 1);
    int count = recon_html_field_count(d);
    struct recon_form_value *values = defaults_of(d);

    /*
     * A filename is a name somebody chose, or -- through a rename -- one a
     * page talked them into. Unescaped, this one closes the filename and
     * opens a `name` of its own, and the server reads one part as two fields.
     */
    values[1].file_name = "a\";name=\"role\";x=\"b.txt";
    values[1].file_bytes = "x";
    values[1].file_length = 1;

    char boundary[RECON_FORM_BOUNDARY_MAX];
    size_t length = 0;
    char *body = recon_form_body_multipart(d, 0, -1, values, count, 4096,
        boundary, sizeof(boundary), &length, NULL, NULL);

    check(body != NULL, "the body is built");
    if (body != NULL) {
        check(holds(body, length,
                "name=\"photo\"; filename=\"a%22;name=%22role%22;x=%22b.txt\""),
            "the quotes are escaped and the header has one filename in it");
        check(!holds(body, length, "name=\"role\""),
            "so no second field appears out of the filename");
        free(body);
    }

    free(values);
    recon_html_free(d);
}

static void test_a_newline_in_a_name_cannot_add_a_header(void) {
    printf("a newline in a field name does not write a second header\n");

    /* The name comes from the page here, not from the person: a page that
     * wants to slip a header past the viewer writes it into `name`. */
    static const char SNEAKY[] =
        "<form method=\"post\" enctype=\"multipart/form-data\">"
        "<input name=\"a&#13;&#10;X-Secret: yes\" value=\"1\">"
        "</form>";
    struct recon_html_document *d = recon_html_parse(SNEAKY, sizeof(SNEAKY) - 1);
    int count = recon_html_field_count(d);
    struct recon_form_value *values = defaults_of(d);

    char boundary[RECON_FORM_BOUNDARY_MAX];
    size_t length = 0;
    char *body = recon_form_body_multipart(d, 0, -1, values, count, 4096,
        boundary, sizeof(boundary), &length, NULL, NULL);

    check(body != NULL, "the body is built");
    if (body != NULL) {
        check(!holds(body, length, "\r\nX-Secret: yes"),
            "the line break is escaped, so no header appears");
        check(holds(body, length, "%0D%0AX-Secret: yes"),
            "and the name is sent as the odd name it is, not dropped");
        free(body);
    }

    free(values);
    recon_html_free(d);
}

static void test_no_file_chosen_still_sends_the_field(void) {
    printf("a file field with nothing chosen is still a part\n");

    struct recon_html_document *d = recon_html_parse(UPLOAD, sizeof(UPLOAD) - 1);
    int count = recon_html_field_count(d);
    struct recon_form_value *values = defaults_of(d);

    char boundary[RECON_FORM_BOUNDARY_MAX];
    size_t length = 0;
    int sent = 0;
    char *body = recon_form_body_multipart(d, 0, -1, values, count, 4096,
        boundary, sizeof(boundary), &length, &sent, NULL);

    check(body != NULL, "the body is built");
    if (body != NULL) {
        /*
         * An empty part rather than no part, which is what a browser sends.
         * The server's question is "did this form have a picture field", and
         * the answer is yes and it was left empty -- a form that omits the
         * field entirely is one a server can read as an older version of
         * itself.
         */
        check(sent == 2, "the caption and the empty file field");
        check(holds(body, length,
                "name=\"photo\"; filename=\"\"\r\n"
                "Content-Type: application/octet-stream\r\n\r\n\r\n"),
            "an empty filename, the honest type, and nothing between");
        free(body);
    }

    free(values);
    recon_html_free(d);
}

static void test_the_same_rules_decide_what_is_sent(void) {
    printf("multipart leaves out exactly what url-encoding leaves out\n");

    /*
     * Two encodings of one form must agree about *which* controls contribute.
     * They cannot agree by being written twice, so `recon_form_field_sends`
     * owns the question and both callers ask it -- and this is the check that
     * they still do.
     */
    static const char MIXED[] =
        "<form method=\"post\" enctype=\"multipart/form-data\">"
        "<input name=\"kept\" value=\"1\">"
        "<input name=\"off\" value=\"2\" disabled>"
        "<input type=\"checkbox\" name=\"unticked\" value=\"3\">"
        "<input type=\"checkbox\" name=\"ticked\" value=\"4\" checked>"
        "<input value=\"5\">"
        "<input type=\"reset\" name=\"clear\" value=\"6\">"
        "<input type=\"submit\" name=\"save\" value=\"Save\">"
        "<input type=\"submit\" name=\"drop\" value=\"Delete\">"
        "</form>";
    struct recon_html_document *d = recon_html_parse(MIXED, sizeof(MIXED) - 1);
    int count = recon_html_field_count(d);
    struct recon_form_value *values = defaults_of(d);

    char boundary[RECON_FORM_BOUNDARY_MAX];
    size_t length = 0;
    int multi = 0, plain = 0;
    char *body = recon_form_body_multipart(d, 0, 6, values, count, 4096,
        boundary, sizeof(boundary), &length, &multi, NULL);
    char *other = recon_form_body(d, 0, 6, values, count, 4096, &plain, NULL);

    check(body != NULL && other != NULL, "both bodies are built");
    if (body != NULL && other != NULL) {
        check(multi == plain && multi == 3,
            "both send three: the text box, the ticked box, and Save");
        check(holds(body, length, "name=\"kept\"") &&
                holds(body, length, "name=\"ticked\"") &&
                holds(body, length, "name=\"save\""),
            "and they are the same three");
        check(!holds(body, length, "name=\"off\"") &&
                !holds(body, length, "name=\"unticked\"") &&
                !holds(body, length, "name=\"clear\"") &&
                !holds(body, length, "name=\"drop\""),
            "disabled, unticked, reset and the other button stay out");
    }
    free(body);
    free(other);
    free(values);
    recon_html_free(d);
}

static void test_a_body_too_long_is_refused_not_cut(void) {
    printf("a file that will not fit is refused, not trimmed\n");

    struct recon_html_document *d = recon_html_parse(UPLOAD, sizeof(UPLOAD) - 1);
    int count = recon_html_field_count(d);
    struct recon_form_value *values = defaults_of(d);

    /*
     * Three thousand bytes and a terminator, because `holds` takes its needle
     * as a C string and would otherwise read off the end of this looking for
     * one. Found by the sanitizers, in the test rather than in the code --
     * which is the failure a test suite cannot report on itself.
     */
    static char BIG[3001];
    memset(BIG, 'z', sizeof(BIG) - 1);
    values[1].file_name = "big.txt";
    values[1].file_bytes = BIG;
    values[1].file_length = sizeof(BIG) - 1;

    char boundary[RECON_FORM_BOUNDARY_MAX];
    size_t length = 99;
    char *body = recon_form_body_multipart(d, 0, -1, values, count, 512,
        boundary, sizeof(boundary), &length, NULL, NULL);

    /*
     * The refusal that matters. Half a file uploaded is a file the server
     * stores, names, and shows back -- a corrupt photo, a truncated document,
     * a spreadsheet missing its last rows -- with nothing anywhere reporting
     * a failure. Better that the upload does not happen.
     */
    check(body == NULL, "nothing is sent");
    check(length == 0, "and the length is cleared rather than left stale");

    /* And with room, the same call sends all three thousand bytes. */
    char *whole = recon_form_body_multipart(d, 0, -1, values, count, 8192,
        boundary, sizeof(boundary), &length, NULL, NULL);
    check(whole != NULL && holds(whole, length, BIG),
        "with room, the whole file goes");
    free(whole);

    free(values);
    recon_html_free(d);
}

static void test_a_password_is_still_flagged(void) {
    printf("a password in a multipart form is still flagged\n");

    static const char LOGIN[] =
        "<form method=\"post\" enctype=\"multipart/form-data\">"
        "<input name=\"user\" value=\"jt\">"
        "<input type=\"password\" name=\"pass\" value=\"hunter2\">"
        "</form>";
    struct recon_html_document *d = recon_html_parse(LOGIN, sizeof(LOGIN) - 1);
    int count = recon_html_field_count(d);
    struct recon_form_value *values = defaults_of(d);

    char boundary[RECON_FORM_BOUNDARY_MAX];
    size_t length = 0;
    bool secret = false;
    char *body = recon_form_body_multipart(d, 0, -1, values, count, 4096,
        boundary, sizeof(boundary), &length, NULL, &secret);

    /* Whatever warns somebody that a form is about to send a password over a
     * plain connection asks this flag. A form that changed its encoding must
     * not thereby lose the warning. */
    check(body != NULL && secret, "the flag says a secret is in it");
    free(body);
    free(values);
    recon_html_free(d);
}

static void test_a_buffer_too_small_for_a_boundary(void) {
    printf("a caller's boundary buffer is checked, not trusted\n");

    struct recon_html_document *d = recon_html_parse(UPLOAD, sizeof(UPLOAD) - 1);
    int count = recon_html_field_count(d);
    struct recon_form_value *values = defaults_of(d);

    char small[8];
    size_t length = 0;
    char *body = recon_form_body_multipart(d, 0, -1, values, count, 4096,
        small, sizeof(small), &length, NULL, NULL);

    /* The header says what size to pass and this is what happens when it is
     * not passed. A boundary written into a buffer that cannot hold it is a
     * header naming one delimiter while the body uses another, and a server
     * reads that as an empty form. */
    check(body == NULL, "refused rather than a boundary nobody can match");
    free(values);
    recon_html_free(d);
}

static void test_the_boundary_is_not_in_the_filename(void) {
    printf("a boundary that occurs in the file's name is not used\n");

    struct recon_html_document *d = recon_html_parse(UPLOAD, sizeof(UPLOAD) - 1);
    int count = recon_html_field_count(d);
    struct recon_form_value *values = defaults_of(d);

    /*
     * The same attack through an easier door. Arranging the *contents* of an
     * upload takes a file; arranging its *name* takes a page that suggests
     * one, or a rename somebody was talked into. The name goes into a header
     * that sits between two delimiters, so a name carrying the delimiter ends
     * the part from inside the header.
     */
    values[1].file_name = "photo----ReconOSForm0.png";
    values[1].file_bytes = "x";
    values[1].file_length = 1;

    char boundary[RECON_FORM_BOUNDARY_MAX];
    size_t length = 0;
    char *body = recon_form_body_multipart(d, 0, -1, values, count, 4096,
        boundary, sizeof(boundary), &length, NULL, NULL);

    check(body != NULL, "the body is still built");
    if (body != NULL) {
        check(strstr(values[1].file_name, boundary) == NULL,
            "the boundary chosen does not occur in the name");
        check(holds(body, length, "filename=\"photo----ReconOSForm0.png\""),
            "and the name is still sent as it is, not altered to make room");
        free(body);
    }

    free(values);
    recon_html_free(d);
}

static void test_a_boundary_buffer_below_the_stated_size(void) {
    printf("a boundary buffer smaller than the header asks for is refused\n");

    struct recon_html_document *d = recon_html_parse(UPLOAD, sizeof(UPLOAD) - 1);
    int count = recon_html_field_count(d);
    struct recon_form_value *values = defaults_of(d);

    /*
     * Twenty bytes. Big enough to hold every boundary this builds -- so it
     * would work, today, by luck -- and smaller than the size the header tells
     * callers to pass. The refusal is the contract being enforced rather than
     * advertised: a caller who sized the buffer by looking at one boundary
     * finds out now, and not on the day a boundary gets longer.
     */
    char narrow[20];
    size_t length = 99;
    char *body = recon_form_body_multipart(d, 0, -1, values, count, 4096,
        narrow, sizeof(narrow), &length, NULL, NULL);

    check(body == NULL, "refused, though the string would have fitted");
    check(length == 0, "and the length is cleared");

    free(values);
    recon_html_free(d);
}

static void test_an_exact_fit_leaves_room_for_the_terminator(void) {
    printf("a buffer of exactly the body's length is one byte short\n");

    struct recon_html_document *d = recon_html_parse(UPLOAD, sizeof(UPLOAD) - 1);
    int count = recon_html_field_count(d);
    struct recon_form_value *values = defaults_of(d);
    values[1].file_name = "a.txt";
    values[1].file_bytes = "hello";
    values[1].file_length = 5;

    char boundary[RECON_FORM_BOUNDARY_MAX];
    size_t length = 0;
    char *body = recon_form_body_multipart(d, 0, -1, values, count, 4096,
        boundary, sizeof(boundary), &length, NULL, NULL);
    check(body != NULL && length > 0, "the body is built, and reports a length");
    free(body);

    /*
     * The body is kept NUL-terminated on top of its real length, so it needs
     * `length + 1` bytes and not `length`. Asking for exactly `length` must
     * fail.
     *
     * This is the check that catches an off-by-one in the fit test, and it is
     * the only shape that can: everywhere else in this suite there is slack in
     * the buffer, and a fit test that is one byte too generous is correct
     * everywhere there is slack. What it does on an exact fit is write the
     * terminator one past the end of the allocation -- a heap overflow that
     * produces no wrong output at all, and so is invisible to every other
     * check here.
     */
    char *snug = recon_form_body_multipart(d, 0, -1, values, count, length,
        boundary, sizeof(boundary), NULL, NULL, NULL);
    check(snug == NULL, "a buffer of exactly the body's length is refused");
    free(snug);

    size_t again = 0;
    char *room = recon_form_body_multipart(d, 0, -1, values, count, length + 1,
        boundary, sizeof(boundary), &again, NULL, NULL);
    check(room != NULL && again == length, "one more byte, and it fits exactly");
    free(room);

    free(values);
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
    test_a_file_field_is_not_a_text_box();
    test_which_forms_ask_for_multipart();
    test_a_file_field_in_an_ordinary_form();
    test_the_address_a_get_makes();
    test_a_body_too_long_is_refused();

    test_a_file_part_carries_the_bytes();
    test_the_last_line_closes_it();
    test_the_boundary_is_not_in_the_content();
    test_a_quote_in_a_name_cannot_add_an_attribute();
    test_a_newline_in_a_name_cannot_add_a_header();
    test_no_file_chosen_still_sends_the_field();
    test_the_same_rules_decide_what_is_sent();
    test_a_body_too_long_is_refused_not_cut();
    test_a_password_is_still_flagged();
    test_a_buffer_too_small_for_a_boundary();
    test_the_boundary_is_not_in_the_filename();
    test_a_boundary_buffer_below_the_stated_size();
    test_an_exact_fit_leaves_room_for_the_terminator();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
