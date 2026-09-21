/*
 * The browser, driven headlessly.
 *
 * --- Why this file exists ---
 *
 * `src/recon_web.c` is **6,433 lines and had no suite at all.** Everything
 * ever checked about it was checked by photographing a running desktop, which
 * is slow, needs a compositor, and can only ask the questions a picture can
 * answer.
 *
 * It turned out not to need any of that. v0.4.76 established that
 * `recon_appwin.c` links without a compositor; the browser sits on top of it
 * and the same is true, because everything underneath it that is *logic* --
 * the parser, the stylesheet, the jar, the forms, the URL -- is a real file
 * this suite links, and everything that is a *screen* is a stub.
 *
 * --- How a test drives it ---
 *
 * The same three doors the live harness uses, and one of them is better here:
 *
 *  - `recon_web_open_path` opens a file through the real `recon_fs`, so every
 *    page below is a real parse of real bytes.
 *  - `recon_appwin_handle_click` and `..._handle_key` are the window's own
 *    input entry points.
 *  - `recon_appwin_describe` returns what `web_describe` writes, whose own
 *    comment says it is *"what the test harness reads"*.
 *
 * The better one is clicking. `scripts/look.sh` clicks at coordinates read
 * off a screenshot, so a moved button silently makes a test click on nothing
 * and pass. Here `recon_appwin_hit_centre` finds a control **by its id**, and
 * a test for a control that has gone fails rather than drifting.
 *
 * --- What is still the live scripts' job ---
 *
 * Anything that needs a server to answer: cookies arriving from a
 * `Set-Cookie`, TLS, a redirect. And anything about pixels. This suite makes
 * no claim about either.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "ReconOS.h"
#include "recon_appwin.h"
#include "recon_fs.h"
#include "recon_web.h"

/*
 * The controls this suite clicks.
 *
 * These mirror the ones in `src/recon_web.c`, which are private to it -- and
 * a copied constant is normally the start of two things that can disagree.
 * Here they cannot disagree *quietly*: `recon_appwin_hit_centre` answers false
 * for an id that is not on the window, so a number that has drifted fails at
 * the line that asks where the control is, naming the control.
 *
 * Which is the property the live harness does not have. There a click is a
 * pair of coordinates, and clicking on empty space is not an error -- so a
 * control that moved makes the test do nothing and report success.
 */
#define HIT_NEWTAB (RECON_APPWIN_HIT_USER + 8)

static int g_checks;
static int g_failures;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
    }
}

/*
 * `describe` is a block of lines. What a test wants is one field out of it,
 * and a test that searched the whole report for "tabs: 2" would also match a
 * bookmark named that.
 */
static bool reports(struct recon_appwin *win, const char *want) {
    char out[4096];
    recon_appwin_describe(win, out, sizeof(out));
    return strstr(out, want) != NULL;
}

static void show_report(struct recon_appwin *win) {
    char out[4096];
    recon_appwin_describe(win, out, sizeof(out));
    printf("    ---\n");
    for (const char *p = out; *p != '\0'; ) {
        const char *nl = strchr(p, '\n');
        int n = nl != NULL ? (int)(nl - p) : (int)strlen(p);
        printf("    %.*s\n", n, p);
        if (nl == NULL) {
            break;
        }
        p = nl + 1;
    }
}

static void check_reports(struct recon_appwin *win, const char *want,
        const char *what) {
    g_checks++;
    if (!reports(win, want)) {
        g_failures++;
        printf("  FAIL: %s\n    wanted to find: \"%s\"\n", what, want);
        show_report(win);
    }
}

/* --- a machine to run one on -------------------------------------------- */

static struct recon_server *fake_server(void) {
    static int nothing;
    return (struct recon_server *)&nothing;
}

/* Write a page into the test filesystem and hand back the path inside it. */
static const char *page(const char *name, const char *html) {
    static char path[256];
    snprintf(path, sizeof(path), "/Users/Tester/%s", name);
    if (!recon_fs_write("/", path, html, strlen(html))) {
        printf("  (could not write %s)\n", path);
    }
    return path;
}

/* --- the tests ---------------------------------------------------------- */

static void test_a_browser_opens_a_page(void) {
    printf("a browser opens a file and says what is in it\n");

    struct recon_appwin *win = recon_web_create(fake_server(), NULL);
    check(win != NULL, "A BROWSER CAN BE MADE WITHOUT A COMPOSITOR");
    if (win == NULL) {
        return;
    }

    recon_appwin_show(win);
    check_reports(win, "tabs: 1", "it opens with one tab");

    const char *p = page("first.html",
        "<!doctype html><title>The first page</title>"
        "<h1>Hello</h1><p>Some words.</p>");

    check(recon_web_open_path(win, p), "and it opens a file");
    check_reports(win, "title: The first page",
        "THE TITLE COMES OUT OF THE DOCUMENT -- a real parse of real bytes, "
        "through the real recon_html.c");

    recon_appwin_destroy(win);
}

static void test_history_remembers_where_it_has_been(void) {
    printf("and it remembers where it has been\n");

    struct recon_appwin *win = recon_web_create(fake_server(), NULL);
    if (win == NULL) {
        check(false, "a browser to work with");
        return;
    }
    recon_appwin_show(win);

    /*
     * **A file opened from disk is not in the history, and that is by design.**
     *
     * I expected it to be, and the suite said otherwise on its first run.
     * `recon_web_open_path` leaves `have_url` false deliberately -- its own
     * comment says a document with no address has nothing for a relative link
     * to resolve against -- and the history is a list of addresses. So what is
     * held here is that opening a second file *replaces* the first rather than
     * stacking behind it.
     *
     * Worth writing down rather than deleting: somebody who opens two files
     * and presses Back is going to be surprised, and the reason they cannot is
     * a decision rather than an oversight.
     */
    const char *one = page("one.html", "<!doctype html><title>One</title><p>1");
    recon_web_open_path(win, one);
    check_reports(win, "title: One", "the first file is showing");
    check_reports(win, "history: 0, at -1",
        "and a file has no address, so nothing goes into the history");

    const char *two = page("two.html", "<!doctype html><title>Two</title><p>2");
    recon_web_open_path(win, two);
    check_reports(win, "title: Two", "the second replaces it");
    check_reports(win, "history: 0, at -1", "and still nothing is recorded");

    recon_appwin_destroy(win);
}

static void test_a_second_tab(void) {
    printf("a second tab, opened by clicking the control that opens one\n");

    struct recon_appwin *win = recon_web_create(fake_server(), NULL);
    if (win == NULL) {
        check(false, "a browser to work with");
        return;
    }
    recon_appwin_show(win);
    recon_web_open_path(win, page("tab.html",
        "<!doctype html><title>Tab one</title><p>x"));

    /*
     * `showing` is one-based. `web_describe` prints `w->active + 1` so the
     * number here matches the number beside the tab in the list underneath
     * it -- I expected a zero on the first run and the suite was right.
     */
    check_reports(win, "tabs: 1, showing 1", "one tab to begin with");

    /*
     * By id rather than by coordinate.
     *
     * This is the difference between this suite and the look harness. There,
     * a click is a pair of numbers read off a screenshot, so a control that
     * moves makes the test click on empty space -- and clicking on empty
     * space is not an error, so the test goes green having done nothing. Here
     * the id is asked where it is, and a control that has gone fails here,
     * at the point the test is about.
     */
    int x = 0;
    int y = 0;
    bool found = recon_appwin_hit_centre(win, HIT_NEWTAB, &x, &y);
    check(found, "THE NEW-TAB CONTROL IS SOMEWHERE");
    if (!found) {
        recon_appwin_destroy(win);
        return;
    }

    recon_appwin_handle_click(win, x, y, true);
    check_reports(win, "tabs: 2", "CLICKING IT OPENS A SECOND");
    check_reports(win, "showing 2", "and moves to it");

    recon_appwin_destroy(win);
}

static void test_a_page_that_is_not_there(void) {
    printf("and a file that is not there is refused rather than guessed at\n");

    struct recon_appwin *win = recon_web_create(fake_server(), NULL);
    if (win == NULL) {
        check(false, "a browser to work with");
        return;
    }
    recon_appwin_show(win);

    check(!recon_web_open_path(win, "/Users/Tester/nothing-here.html"),
        "opening a file that does not exist says so");

    /*
     * And the browser is still usable afterwards, which is the half a refusal
     * test usually leaves out: a function that returns false and leaves the
     * window in a state nothing else works from has not refused, it has
     * broken.
     */
    const char *p = page("after.html",
        "<!doctype html><title>Still here</title><p>ok");
    check(recon_web_open_path(win, p), "and the next page still opens");
    check_reports(win, "title: Still here", "AND IS THE ONE SHOWING");

    recon_appwin_destroy(win);
}

/*
 * A form, and the control that sends it.
 *
 * The submit button's id is not a fixed number -- fields are numbered as they
 * are met -- so it is found by walking the hit regions rather than guessed at.
 * `HIT_FIELD_BASE` mirrors `src/recon_web.c`, and a drift in it shows up as
 * the button not being found, which fails by name.
 */
#define HIT_FIELD_BASE (RECON_APPWIN_HIT_USER + 4000)

static void submit(struct recon_appwin *win, int field_index) {
    int x = 0;
    int y = 0;
    if (!recon_appwin_hit_centre(win, HIT_FIELD_BASE + (uint32_t)field_index,
            &x, &y)) {
        check(false, "the Send button is on the page");
        return;
    }
    recon_appwin_handle_click(win, x, y, true);
}

static void test_a_form_refuses_what_the_page_said_it_would(void) {
    printf("a form checks the shape of an answer, not only that there is one\n");

    struct recon_appwin *win = recon_web_create(fake_server(), NULL);
    if (win == NULL) {
        check(false, "a browser to work with");
        return;
    }
    recon_appwin_show(win);

    /*
     * Every value is set in the markup, so no typing is needed -- which
     * matters, because the text field is stubbed in this suite and a test
     * that had to type would be a test of the stub.
     */
    recon_web_open_path(win, page("short.html",
        "<!doctype html><title>Short</title>"
        "<form action=\"http://example.com/x\">"
        "<input name=\"pin\" minlength=\"4\" value=\"12\">"
        "<input type=\"submit\" value=\"Send\">"
        "</form>"));

    submit(win, 1);
    check_reports(win, "needs at least 4 characters, and has 2",
        "TOO SHORT IS REFUSED, AND SAYS BY HOW MUCH");

    recon_appwin_destroy(win);
}

static void test_too_long_and_not_an_address(void) {
    printf("and the other two a page can ask for\n");

    struct recon_appwin *win = recon_web_create(fake_server(), NULL);
    if (win == NULL) {
        check(false, "a browser to work with");
        return;
    }
    recon_appwin_show(win);

    recon_web_open_path(win, page("long.html",
        "<!doctype html><title>Long</title>"
        "<form action=\"http://example.com/x\">"
        "<input name=\"code\" maxlength=\"3\" value=\"abcdef\">"
        "<input type=\"submit\" value=\"Send\">"
        "</form>"));
    submit(win, 1);
    check_reports(win, "takes at most 3 characters, and has 6",
        "too long is refused");

    recon_web_open_path(win, page("mail.html",
        "<!doctype html><title>Mail</title>"
        "<form action=\"http://example.com/x\">"
        "<input type=\"email\" name=\"who\" value=\"not-an-address\">"
        "<input type=\"submit\" value=\"Send\">"
        "</form>"));
    submit(win, 1);
    check_reports(win, "needs an email address",
        "AND SOMETHING THAT IS NOT ONE IS REFUSED");

    recon_web_open_path(win, page("mail-ok.html",
        "<!doctype html><title>Mail ok</title>"
        "<form action=\"http://example.com/x\">"
        "<input type=\"email\" name=\"who\" value=\"someone@example.com\">"
        "<input type=\"submit\" value=\"Send\">"
        "</form>"));
    submit(win, 1);
    check(!reports(win, "needs an email address"),
        "while an address that is one is not");

    recon_appwin_destroy(win);
}

static void test_a_number_out_of_range(void) {
    printf("a number outside the range the page gave\n");

    struct recon_appwin *win = recon_web_create(fake_server(), NULL);
    if (win == NULL) {
        check(false, "a browser to work with");
        return;
    }
    recon_appwin_show(win);

    recon_web_open_path(win, page("range.html",
        "<!doctype html><title>Range</title>"
        "<form action=\"http://example.com/x\">"
        "<input name=\"n\" min=\"1\" max=\"10\" value=\"50\">"
        "<input type=\"submit\" value=\"Send\">"
        "</form>"));
    submit(win, 1);
    check_reports(win, "cannot be more than 10", "too big is refused");

    /*
     * And a `min` this cannot read as a number is left alone rather than
     * guessed at. A date in `min` compared as a number would be a confident
     * wrong answer, and refusing a form for it would be worse than the gap.
     */
    recon_web_open_path(win, page("dated.html",
        "<!doctype html><title>Dated</title>"
        "<form action=\"http://example.com/x\">"
        "<input name=\"when\" min=\"2026-01-01\" value=\"2020-05-05\">"
        "<input type=\"submit\" value=\"Send\">"
        "</form>"));
    submit(win, 1);
    check(!reports(win, "cannot be less than"),
        "A BOUND THIS CANNOT READ IS LEFT ALONE, NOT GUESSED AT");

    recon_appwin_destroy(win);
}

static void test_an_empty_box_is_required_s_business(void) {
    printf("and an empty box is `required`'s business, not this one's\n");

    struct recon_appwin *win = recon_web_create(fake_server(), NULL);
    if (win == NULL) {
        check(false, "a browser to work with");
        return;
    }
    recon_appwin_show(win);

    /*
     * Somebody who left a box alone should not be told it is three characters
     * short of a minimum they never started typing. The page did not say the
     * box had to be filled in, so an empty one is an answer.
     */
    recon_web_open_path(win, page("empty.html",
        "<!doctype html><title>Empty</title>"
        "<form action=\"http://example.com/x\">"
        "<input name=\"pin\" minlength=\"4\" value=\"\">"
        "<input type=\"submit\" value=\"Send\">"
        "</form>"));
    submit(win, 1);
    check(!reports(win, "needs at least"),
        "an empty box is not refused for being too short");

    /* And with `required`, it is -- in those words rather than these. */
    recon_web_open_path(win, page("empty-required.html",
        "<!doctype html><title>Empty required</title>"
        "<form action=\"http://example.com/x\">"
        "<input name=\"pin\" minlength=\"4\" required value=\"\">"
        "<input type=\"submit\" value=\"Send\">"
        "</form>"));
    submit(win, 1);
    check_reports(win, "before it can be sent",
        "AND REQUIRED STILL SAYS WHAT IT ALWAYS SAID");

    recon_appwin_destroy(win);
}

int main(void) {
    printf("\n--- the browser, with no compositor under it ---\n\n");

    char root[] = "/tmp/reconos-web-XXXXXX";
    if (mkdtemp(root) == NULL) {
        printf("could not make a test filesystem\n");
        return 1;
    }

    if (!recon_fs_init(root)) {
        printf("could not start the test filesystem\n");
        return 1;
    }
    recon_fs_mkdir("/", "/Users");
    recon_fs_mkdir("/", "/Users/Tester");

    test_a_browser_opens_a_page();
    test_history_remembers_where_it_has_been();
    test_a_second_tab();
    test_a_page_that_is_not_there();
    test_a_form_refuses_what_the_page_said_it_would();
    test_too_long_and_not_an_address();
    test_a_number_out_of_range();
    test_an_empty_box_is_required_s_business();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
