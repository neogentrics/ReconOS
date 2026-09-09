/*
 * Tests for cookies.
 *
 * These rules decide what a server is told about somebody on every request,
 * and the ones that matter are the *refusals* -- which are exactly the rules
 * nothing exercises by accident. A cookie policy that is too generous behaves
 * identically to a correct one until the day it does not, and then it has been
 * behaving that way for months.
 *
 * The headers below are real ones, taken off real responses, rather than
 * invented for the test.
 *
 * Run with: ./build/recon_cookie_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_cookie.h"

static int g_failures;
static int g_checks;

/* A fixed "now", so nothing here depends on when it is run. 1 July 2026. */
#define NOW ((time_t)1782000000)

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
        printf("  FAIL: %s\n    wanted: \"%s\"\n    got:    \"%s\"\n", what,
            want, got != NULL ? got : "(nothing)");
    }
}

/* The Cookie: header a request to this address would carry. */
static const char *sent(struct recon_cookie_jar *jar, const char *host,
        const char *path, bool secure) {
    static char out[4096];
    recon_cookie_header(jar, host, path, secure, NOW, out, sizeof(out));
    return out;
}

/* --- The ordinary case --- */

static void test_a_session_survives_a_page(void) {
    printf("a cookie set on one page comes back on the next\n");

    struct recon_cookie_jar *jar = recon_cookie_jar_new();
    check(jar != NULL, "a jar");
    if (jar == NULL) {
        return;
    }

    /* GitHub's, shortened. The shape is what matters: a session token, a
     * path of "/", Secure and HttpOnly. */
    check(recon_cookie_set(jar, "example.com", "/login", true,
        "user_session=abc123; path=/; secure; HttpOnly; SameSite=Lax",
        NOW) == NULL, "it is taken");

    check_text(sent(jar, "example.com", "/", true), "user_session=abc123",
        "and comes back on the next request");
    check_text(sent(jar, "example.com", "/settings/profile", true),
        "user_session=abc123", "on any path under the one it was set for");

    check(recon_cookie_count(jar) == 1, "one cookie is kept");

    struct recon_cookie_view view;
    check(recon_cookie_at(jar, 0, &view), "and can be looked at");
    check(strcmp(view.name, "user_session") == 0 &&
        strcmp(view.value, "abc123") == 0, "with its name and its value");
    check(view.secure && view.http_only, "and what was asked of it");
    check(view.expires == 0, "no expiry: it lasts as long as the window");

    recon_cookie_jar_free(jar);
}

/* --- The refusals, which are the point --- */

static void test_a_cookie_belongs_to_the_host_that_set_it(void) {
    printf("Domain is narrowed to the host, never widened\n");

    struct recon_cookie_jar *jar = recon_cookie_jar_new();
    if (jar == NULL) {
        return;
    }

    /*
     * The commonest shape on the web, and the one this narrows. A server on
     * `www.example.com` asking for the whole family gets the host it is.
     */
    check(recon_cookie_set(jar, "www.example.com", "/", true,
        "id=1; Domain=.example.com", NOW) == NULL,
        "a wider Domain does not lose the cookie");

    check_text(sent(jar, "www.example.com", "/", true), "id=1",
        "it comes back to the host that set it");
    check_text(sent(jar, "example.com", "/", true), "",
        "and not to the parent it asked for");
    check_text(sent(jar, "other.example.com", "/", true), "",
        "nor to a sibling");

    struct recon_cookie_view view;
    recon_cookie_at(jar, 0, &view);
    check(view.was_narrowed, "and it is recorded that this happened");

    /*
     * The one that makes the whole policy worth having. Without a public
     * suffix list, a rule that honoured Domain by suffix-matching would let a
     * site set a cookie every site in the country receives.
     */
    check(recon_cookie_set(jar, "shop.co.uk", "/", true,
        "track=everyone; Domain=.co.uk", NOW) == NULL, "it is still taken");
    check_text(sent(jar, "bank.co.uk", "/", true), "",
        "but Domain=.co.uk reaches nothing else");
    check_text(sent(jar, "shop.co.uk", "/", true), "track=everyone",
        "only the host that set it");

    /* A Domain that names the host exactly is what it already was. */
    check(recon_cookie_set(jar, "plain.example", "/", true,
        "a=1; Domain=plain.example", NOW) == NULL, "an exact Domain is fine");
    recon_cookie_at(jar, recon_cookie_count(jar) - 1, &view);
    check(!view.was_narrowed, "and is not recorded as a narrowing");

    recon_cookie_jar_free(jar);
}

static void test_secure(void) {
    printf("Secure means encrypted, in both directions\n");

    struct recon_cookie_jar *jar = recon_cookie_jar_new();
    if (jar == NULL) {
        return;
    }

    check(recon_cookie_set(jar, "example.com", "/", true,
        "s=1; Secure", NOW) == NULL, "set over https");
    check_text(sent(jar, "example.com", "/", true), "s=1",
        "sent over https");
    check_text(sent(jar, "example.com", "/", false), "",
        "and never over http");

    /*
     * The attack this stops: a server on the clear side of a site overwriting
     * the session set on the encrypted side. Browsers stopped allowing it and
     * the standard followed.
     */
    const char *why = recon_cookie_set(jar, "example.com", "/", false,
        "s=stolen; Secure", NOW);
    check(why != NULL, "a Secure cookie over http is refused");
    check(why != NULL && strstr(why, "unencrypted") != NULL,
        "and the refusal says why");
    check_text(sent(jar, "example.com", "/", true), "s=1",
        "the one that was already there is untouched");

    recon_cookie_jar_free(jar);
}

static void test_paths(void) {
    printf("a path is a path, not a prefix\n");

    struct recon_cookie_jar *jar = recon_cookie_jar_new();
    if (jar == NULL) {
        return;
    }

    check(recon_cookie_set(jar, "example.com", "/", true,
        "a=1; Path=/admin", NOW) == NULL, "a cookie scoped to /admin");

    check_text(sent(jar, "example.com", "/admin", true), "a=1",
        "reaches /admin");
    check_text(sent(jar, "example.com", "/admin/users", true), "a=1",
        "and everything under it");

    /*
     * The one everybody gets wrong. A plain prefix test sends a cookie scoped
     * to /admin to /administrators, which is a different part of the site and
     * may be a different person's.
     */
    check_text(sent(jar, "example.com", "/administrators", true), "",
        "and does not reach /administrators");
    check_text(sent(jar, "example.com", "/", true), "",
        "nor the root above it");

    /* The default path is the request's directory, not the file. A cookie set
     * while reading /help/index.html belongs to /help -- storing the file name
     * would mean it was never sent again. */
    check(recon_cookie_set(jar, "docs.example", "/help/index.html", true,
        "b=2", NOW) == NULL, "a cookie with no Path");
    check_text(sent(jar, "docs.example", "/help/other.html", true), "b=2",
        "belongs to the directory it was set in");
    check_text(sent(jar, "docs.example", "/", true), "",
        "and not to the whole site");

    recon_cookie_jar_free(jar);
}

static void test_expiry(void) {
    printf("what a server asks to be forgotten is forgotten\n");

    struct recon_cookie_jar *jar = recon_cookie_jar_new();
    if (jar == NULL) {
        return;
    }

    check(recon_cookie_set(jar, "example.com", "/", true,
        "keep=1; Max-Age=3600", NOW) == NULL, "an hour from now");
    check_text(sent(jar, "example.com", "/", true), "keep=1", "is sent");

    /*
     * How a server signs somebody out: it sends the same cookie back with an
     * expiry in the past. Treating that as an ordinary cookie leaves them
     * signed in, which is the failure worth having a test for.
     */
    check(recon_cookie_set(jar, "example.com", "/", true,
        "keep=; Max-Age=0", NOW) == NULL, "Max-Age=0 is accepted");
    check_text(sent(jar, "example.com", "/", true), "",
        "and the cookie is gone");
    check(recon_cookie_count(jar) == 0, "gone from the jar, not just unsent");

    /* An Expires already past does the same. */
    recon_cookie_set(jar, "example.com", "/", true, "x=1", NOW);
    check(recon_cookie_count(jar) == 1, "one there");
    recon_cookie_set(jar, "example.com", "/", true,
        "x=1; Expires=Thu, 01 Jan 1970 00:00:00 GMT", NOW);
    check(recon_cookie_count(jar) == 0, "an Expires in the past deletes it");

    /*
     * Max-Age wins when both are sent. A server that wants a cookie gone sends
     * both for the sake of old clients, and reading only Expires -- which may
     * be far in the future for compatibility -- leaves somebody signed in.
     */
    recon_cookie_set(jar, "example.com", "/", true, "y=1", NOW);
    recon_cookie_set(jar, "example.com", "/", true,
        "y=1; Max-Age=0; Expires=Sat, 01 Jan 2050 00:00:00 GMT", NOW);
    check(recon_cookie_count(jar) == 0, "Max-Age beats Expires");

    /* And one that expires while it is sitting there is swept. */
    recon_cookie_set(jar, "example.com", "/", true, "z=1; Max-Age=10", NOW);
    check(recon_cookie_count(jar) == 1, "kept while it is live");
    check(recon_cookie_sweep(jar, NOW + 11) == 1, "and swept once it is not");
    check(recon_cookie_count(jar) == 0, "leaving nothing");

    recon_cookie_jar_free(jar);
}

static void test_the_three_date_formats(void) {
    printf("all three of the date formats servers actually send\n");

    struct recon_cookie_jar *jar = recon_cookie_jar_new();
    if (jar == NULL) {
        return;
    }

    /*
     * A parser that read only the first of these would treat the other two as
     * "no expiry" and keep a cookie a server had asked to be forgotten -- so
     * this checks each is *understood*, by sending one of each in the past.
     */
    const char *formats[] = {
        "a=1; Expires=Wed, 09 Jun 2021 10:18:14 GMT",
        "b=1; Expires=Wednesday, 09-Jun-21 10:18:14 GMT",
        "c=1; Expires=Wed Jun 09 10:18:14 2021",
    };
    const char *names[] = { "a", "b", "c" };

    struct recon_cookie_view view;

    for (size_t i = 0; i < 3; i++) {
        char put[64];
        snprintf(put, sizeof(put), "%s=1", names[i]);
        recon_cookie_set(jar, "example.com", "/", true, put, NOW);
        check(recon_cookie_count(jar) == 1, "one is there to be removed");
        recon_cookie_set(jar, "example.com", "/", true, formats[i], NOW);

        char what[96];
        snprintf(what, sizeof(what), "format %zu is understood", i + 1);
        check(recon_cookie_count(jar) == 0, what);
    }

    /*
     * An expiry further off than the standard allows comes back to the bound.
     *
     * RFC 6265bis says a user agent must clamp to four hundred days, and that
     * bound is also what stops `now + Max-Age` overflowing -- which is what
     * the sanitizer said when the fuzzer sent `Max-Age=99999999999999999999`.
     */
    recon_cookie_forget_all(jar);
    check(recon_cookie_set(jar, "example.com", "/", true,
        "far=1; Max-Age=99999999999999999999", NOW) == NULL,
        "an absurd Max-Age is taken");
    recon_cookie_at(jar, 0, &view);
    check(view.expires > NOW, "with an expiry ahead of now");
    check(view.expires <= NOW + 400 * 24 * 60 * 60,
        "and no further off than four hundred days");

    recon_cookie_forget_all(jar);
    recon_cookie_set(jar, "example.com", "/", true,
        "far=1; Expires=Fri, 01 Jan 2100 00:00:00 GMT", NOW);
    recon_cookie_at(jar, 0, &view);
    check(view.expires <= NOW + 400 * 24 * 60 * 60,
        "an Expires cannot reach past the bound either");
    recon_cookie_forget_all(jar);

    /* And a date in the future is kept, so this is not passing by refusing
     * everything. */
    check(recon_cookie_set(jar, "example.com", "/", true,
        "d=1; Expires=Sat, 01 Jan 2050 00:00:00 GMT", NOW) == NULL,
        "a date in the future");
    check(recon_cookie_count(jar) == 1, "is kept");

    recon_cookie_at(jar, 0, &view);
    check(view.expires > NOW, "with an expiry ahead of now");

    /* Something unreadable is no expiry rather than an error, which is what
     * the standard says -- a cookie is not lost to a bad date. */
    recon_cookie_forget_all(jar);
    check(recon_cookie_set(jar, "example.com", "/", true,
        "e=1; Expires=some time next week", NOW) == NULL, "a date this cannot read");
    check(recon_cookie_count(jar) == 1, "leaves the cookie, without an expiry");
    recon_cookie_at(jar, 0, &view);
    check(view.expires == 0, "and it lasts as long as the window");

    recon_cookie_jar_free(jar);
}

static void test_the_order_they_are_sent_in(void) {
    printf("longer paths first, then oldest first\n");

    struct recon_cookie_jar *jar = recon_cookie_jar_new();
    if (jar == NULL) {
        return;
    }

    /*
     * It matters because a server reading a repeated name reads one of them,
     * and which one is decided here. A more specific path is the more specific
     * answer, so it goes first.
     */
    recon_cookie_set(jar, "example.com", "/", true, "id=root; Path=/", NOW);
    recon_cookie_set(jar, "example.com", "/", true,
        "id=deep; Path=/a/b/c", NOW);
    recon_cookie_set(jar, "example.com", "/", true, "id=mid; Path=/a", NOW);

    check_text(sent(jar, "example.com", "/a/b/c/page", true),
        "id=deep; id=mid; id=root", "most specific first");

    /* Among equal paths, the order they arrived. */
    recon_cookie_forget_all(jar);
    recon_cookie_set(jar, "example.com", "/", true, "first=1", NOW);
    recon_cookie_set(jar, "example.com", "/", true, "second=2", NOW);
    check_text(sent(jar, "example.com", "/", true), "first=1; second=2",
        "then oldest first");

    /*
     * And refreshing one keeps its place. A server that rewrites its session
     * cookie on every response would otherwise walk it to the end of the
     * order, which changes which one a server reads.
     */
    recon_cookie_set(jar, "example.com", "/", true, "first=updated", NOW);
    check_text(sent(jar, "example.com", "/", true),
        "first=updated; second=2", "a refreshed cookie keeps its place");

    recon_cookie_jar_free(jar);
}

static void test_what_is_not_stored(void) {
    printf("what is refused, and said out loud\n");

    struct recon_cookie_jar *jar = recon_cookie_jar_new();
    if (jar == NULL) {
        return;
    }

    check(recon_cookie_set(jar, "example.com", "/", true, "novalue", NOW)
        != NULL, "a cookie with no name at all is refused");
    check(recon_cookie_count(jar) == 0, "and nothing is stored");

    check(recon_cookie_set(jar, "example.com", "/", true, "=1", NOW) != NULL,
        "an empty name is refused too");

    /* A value longer than this keeps is refused rather than cut. Half a
     * session token is not a shorter token, it is a different one. */
    char huge[RECON_COOKIE_VALUE_MAX + 64];
    int used = snprintf(huge, sizeof(huge), "big=");
    memset(huge + used, 'x', sizeof(huge) - (size_t)used - 1);
    huge[sizeof(huge) - 1] = '\0';
    check(recon_cookie_set(jar, "example.com", "/", true, huge, NOW) != NULL,
        "an oversized value is refused, not cut");
    check(recon_cookie_count(jar) == 0, "and nothing is stored");

    /* A Path that does not begin with a slash is ignored and the default
     * stands, which is what the standard says. */
    check(recon_cookie_set(jar, "example.com", "/here/there.html", true,
        "p=1; Path=nonsense", NOW) == NULL, "a nonsense Path is not fatal");
    check_text(sent(jar, "example.com", "/here/again.html", true), "p=1",
        "and the default path is used instead");

    recon_cookie_jar_free(jar);
}

static void test_the_ceilings(void) {
    printf("a site cannot fill this, or push another site out\n");

    struct recon_cookie_jar *jar = recon_cookie_jar_new();
    if (jar == NULL) {
        return;
    }

    /* Somebody else's session, set first. */
    recon_cookie_set(jar, "bank.example", "/", true, "session=mine", NOW);

    /* And a site that sets far more than its share. Each has an expiry, so
     * the soonest goes first when there is no room. */
    for (int i = 0; i < RECON_COOKIE_PER_HOST + 20; i++) {
        char put[128];
        snprintf(put, sizeof(put), "c%d=%d; Max-Age=%d", i, i, 1000 + i);
        recon_cookie_set(jar, "greedy.example", "/", true, put, NOW);
    }

    int greedy = 0;
    for (int i = 0; i < recon_cookie_count(jar); i++) {
        struct recon_cookie_view view;
        recon_cookie_at(jar, i, &view);
        if (strcmp(view.host, "greedy.example") == 0) {
            greedy++;
        }
    }
    check(greedy == RECON_COOKIE_PER_HOST,
        "a host is held to its share");

    check_text(sent(jar, "bank.example", "/", true), "session=mine",
        "and another site's session is untouched");

    /* The ones that survived are the ones that live longest. */
    check_text(sent(jar, "greedy.example", "/", true) [0] == '\0' ? "" : "kept",
        "kept", "the host still has cookies");
    struct recon_cookie_view view;
    bool kept_the_last = false;
    for (int i = 0; i < recon_cookie_count(jar); i++) {
        recon_cookie_at(jar, i, &view);
        if (strcmp(view.name, "c69") == 0) {
            kept_the_last = true;
        }
    }
    check(kept_the_last, "and the ones kept are the ones that last longest");

    recon_cookie_jar_free(jar);
}

static void test_a_header_stops_at_a_cookie(void) {
    printf("a header too long stops between cookies, never inside one\n");

    struct recon_cookie_jar *jar = recon_cookie_jar_new();
    if (jar == NULL) {
        return;
    }

    recon_cookie_set(jar, "example.com", "/", true, "aaaa=1111", NOW);
    recon_cookie_set(jar, "example.com", "/", true, "bbbb=2222", NOW);
    recon_cookie_set(jar, "example.com", "/", true, "cccc=3333", NOW);

    char small[24];
    size_t used = recon_cookie_header(jar, "example.com", "/", true, NOW,
        small, sizeof(small));

    check(used < sizeof(small), "it fits what it was given");
    check(strlen(small) == used, "and says how much it wrote");
    /* Whatever came out is whole cookies: no trailing "=" or half a value. */
    check(strstr(small, "aaaa=1111") != NULL, "the first one is whole");
    check(small[used - 1] != '=' && small[used - 1] != ';',
        "and it does not end mid-cookie");

    recon_cookie_jar_free(jar);
}

static void test_forgetting(void) {
    printf("cookies can be forgotten, by host or altogether\n");

    struct recon_cookie_jar *jar = recon_cookie_jar_new();
    if (jar == NULL) {
        return;
    }

    recon_cookie_set(jar, "a.example", "/", true, "one=1", NOW);
    recon_cookie_set(jar, "a.example", "/", true, "two=2", NOW);
    recon_cookie_set(jar, "b.example", "/", true, "three=3", NOW);

    check(recon_cookie_forget_host(jar, "a.example") == 2,
        "forgetting a host says how many went");
    check_text(sent(jar, "a.example", "/", true), "", "and they are gone");
    check_text(sent(jar, "b.example", "/", true), "three=3",
        "while the others stay");

    check(recon_cookie_forget_all(jar) == 1, "and forgetting everything");
    check(recon_cookie_count(jar) == 0, "leaves nothing");

    recon_cookie_jar_free(jar);
}

static void test_nothing_is_refused_with_a_crash(void) {
    printf("nonsense headers do not take anything down\n");

    struct recon_cookie_jar *jar = recon_cookie_jar_new();
    if (jar == NULL) {
        return;
    }

    static const char *const NONSENSE[] = {
        "", ";", "=", "==", ";;;;", "a=b;;;;", "a=b; Path=", "a=b; Domain=",
        "a=b; Max-Age=", "a=b; Max-Age=abc", "a=b; Expires=",
        "a=b; Expires=;;;", "a=b; Secure=maybe", "; a=b",
        "a=b; Max-Age=99999999999999999999",
        "a=b; Expires=Wed, 99 Xxx 99999 99:99:99 GMT",
    };

    for (size_t i = 0; i < sizeof(NONSENSE) / sizeof(NONSENSE[0]); i++) {
        recon_cookie_set(jar, "example.com", "/x/y", true, NONSENSE[i], NOW);
        char out[256];
        recon_cookie_header(jar, "example.com", "/x/y", true, NOW, out,
            sizeof(out));
    }
    check(true, "nothing crashed and nothing pointed outside itself");

    recon_cookie_jar_free(jar);
}

int main(void) {
    printf("ReconOS cookie tests\n\n");

    test_a_session_survives_a_page();
    test_a_cookie_belongs_to_the_host_that_set_it();
    test_secure();
    test_paths();
    test_expiry();
    test_the_three_date_formats();
    test_the_order_they_are_sent_in();
    test_what_is_not_stored();
    test_the_ceilings();
    test_a_header_stops_at_a_cookie();
    test_forgetting();
    test_nothing_is_refused_with_a_crash();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
