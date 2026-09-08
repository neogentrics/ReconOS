/*
 * Reading an address.
 *
 * There was no suite for this at all, which is how the fragment bug got in:
 * `<a href="#main">` parsed to an empty fragment, because the code that pulls
 * the fragment out searches the *path* -- and the path that branch writes is
 * the base's, whose own hash was stripped long before. So every link within a
 * page resolved to "no named place", and the viewer correctly did the thing
 * that means: went to the top of the page you were already at the top of.
 *
 * Every address a viewer handles comes off somebody else's page. This is the
 * boundary between their bytes and where the machine connects to, which is
 * exactly the kind of thing that wants tests.
 *
 * Run with: ./build/recon_url_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>

#include "recon_http.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
    }
}

/* Parse, and say what came out, so a failure names the address. */
static bool parse(const char *text, const struct recon_http_url *base,
        struct recon_http_url *out) {
    if (recon_http_parse_url(text, base, out)) {
        return true;
    }
    printf("  FAIL: '%s' did not parse: %s\n", text, recon_http_last_error());
    g_failures++;
    g_checks++;
    return false;
}

static void check_url(const struct recon_http_url *u, bool secure,
        const char *host, const char *path, const char *fragment,
        const char *what) {
    /* Room for every field at its own maximum. A diagnostic that truncates
     * is a diagnostic that hides the difference it was printed to show. */
    char said[RECON_HTTP_URL_MAX * 2 + 512];
    snprintf(said, sizeof(said), "%s://%s%s#%s",
        u->secure ? "https" : "http", u->host, u->path, u->fragment);

    bool ok = u->secure == secure &&
        strcmp(u->host, host) == 0 &&
        strcmp(u->path, path) == 0 &&
        strcmp(u->fragment, fragment) == 0;

    g_checks++;
    if (!ok) {
        g_failures++;
        printf("  FAIL: %s\n        wanted https=%d %s%s#%s\n"
               "        got    %s\n",
            what, secure, host, path, fragment, said);
    }
}

/* --- Tests --- */

static void test_a_whole_address(void) {
    printf("an address written in full\n");

    struct recon_http_url u;

    if (parse("https://example.com/a/b?q=1", NULL, &u)) {
        check_url(&u, true, "example.com", "/a/b?q=1", "",
            "scheme, host, path and query");
    }

    if (parse("http://example.com", NULL, &u)) {
        check_url(&u, false, "example.com", "/", "",
            "http is honoured when it is written");
    }

    /*
     * https for a bare host. Guessing http is guessing the one that costs
     * somebody their privacy.
     */
    if (parse("example.com", NULL, &u)) {
        check_url(&u, true, "example.com", "/", "",
            "a bare host is https");
    }

    if (parse("example.com:8080/x", NULL, &u)) {
        check(u.port == 8080, "a port is read");
    }
}

static void test_the_part_after_the_hash(void) {
    printf("the fragment, which is not sent to anybody\n");

    struct recon_http_url u;

    /*
     * It is kept out of the path deliberately: it is not part of what is
     * asked of the server, and sending it would be asking for a document that
     * does not exist.
     */
    if (parse("https://example.com/page#notes", NULL, &u)) {
        check_url(&u, true, "example.com", "/page", "notes",
            "an absolute address keeps its fragment out of the path");
    }

    if (parse("example.com#top", NULL, &u)) {
        check_url(&u, true, "example.com", "/", "top",
            "a fragment with no path at all");
    }

    if (parse("https://example.com/page#", NULL, &u)) {
        check_url(&u, true, "example.com", "/page", "",
            "a bare hash names no place");
    }
}

static void test_a_link_on_a_page(void) {
    printf("an address resolved against the page it was written on\n");

    struct recon_http_url base;
    if (!parse("https://example.com/docs/guide.html", NULL, &base)) {
        return;
    }

    struct recon_http_url u;

    if (parse("/other", &base, &u)) {
        check_url(&u, true, "example.com", "/other", "",
            "a rooted path replaces the whole path");
    }

    if (parse("next.html", &base, &u)) {
        check_url(&u, true, "example.com", "/docs/next.html", "",
            "a bare name is beside the page it was written on");
    }

    /*
     * The one this suite exists for. A link to a place on the same page keeps
     * the page and takes the name -- and the name comes from what was
     * written, not from the path, because the path here is the base's and its
     * hash was stripped when the base was parsed.
     */
    if (parse("#install", &base, &u)) {
        check_url(&u, true, "example.com", "/docs/guide.html", "install",
            "a link to a place on this page keeps the page and the name");
    }

    if (parse("other.html#part2", &base, &u)) {
        check_url(&u, true, "example.com", "/docs/other.html", "part2",
            "a link into the middle of another page carries both");
    }

    if (parse("https://elsewhere.org/x#y", &base, &u)) {
        check_url(&u, true, "elsewhere.org", "/x", "y",
            "an absolute link ignores the base entirely");
    }
}

static void test_two_addresses_differing_only_after_the_hash(void) {
    printf("the fragment is left out of the formatted address\n");

    /*
     * Which is what makes "is this the page I am already showing" work.
     * With the fragment in, every anchor on a page would look like a
     * different page and be fetched again -- losing the scroll position to
     * arrive at the same bytes.
     */
    struct recon_http_url a, b;
    if (!parse("https://example.com/p#one", NULL, &a) ||
            !parse("https://example.com/p#two", NULL, &b)) {
        return;
    }

    char first[512];
    char second[512];
    recon_http_format_url(&a, first, sizeof(first));
    recon_http_format_url(&b, second, sizeof(second));

    check(strcmp(first, second) == 0,
        "two places on one page format as the same address");
    check(strchr(first, '#') == NULL,
        "and the formatted address has no hash in it");
    check(strcmp(a.fragment, "one") == 0 && strcmp(b.fragment, "two") == 0,
        "while the places themselves are still distinct");
}

static void test_nothing_is_refused(void) {
    printf("addresses that are not addresses\n");

    struct recon_http_url u;

    check(!recon_http_parse_url(NULL, NULL, &u), "no text");
    check(!recon_http_parse_url("x", NULL, NULL), "nowhere to put it");
    check(!recon_http_parse_url("", NULL, &u), "an empty address");
    check(!recon_http_parse_url("   ", NULL, &u), "only spaces");

    /*
     * A scheme this cannot open is refused rather than guessed at. Treating
     * "javascript:..." as a host name would put it in the address bar as
     * though it were somewhere to go.
     */
    check(!recon_http_parse_url("ftp://example.com/x", NULL, &u),
        "a scheme this does not speak");
    check(!recon_http_parse_url("javascript:alert(1)", NULL, &u) ||
        u.host[0] != '\0',
        "javascript: is not treated as a place");

    /* A relative address with no base has nothing to be relative to. */
    struct recon_http_url v;
    bool got = recon_http_parse_url("#top", NULL, &v);
    check(!got || v.host[0] != '\0',
        "a bare fragment with no page it came from");
}

int main(void) {
    printf("ReconOS address tests\n\n");

    test_a_whole_address();
    test_the_part_after_the_hash();
    test_a_link_on_a_page();
    test_two_addresses_differing_only_after_the_hash();
    test_nothing_is_refused();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
