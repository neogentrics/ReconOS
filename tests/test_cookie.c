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
#include "recon_fs.h"
#include "recon_keyring.h"
#include "recon_sealed.h"
#include "recon_users.h"

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

static void test_closing_the_window_ends_the_session(void) {
    printf("closing a window ends the session cookies and keeps the rest\n");

    /*
     * A cookie with no expiry lasts until the browser closes, which is not a
     * description but the definition -- so something has to end it, and until
     * v0.4.74 nothing did. Closing a built-in application *hides* its window
     * rather than destroying it, so the destructor that sealed the jar ran at
     * shutdown and never on a close. Found by setting one, closing the
     * window, opening it again, and watching it come back.
     */
    struct recon_cookie_jar *jar = recon_cookie_jar_new();

    if (jar == NULL) {
        return;
    }

    recon_cookie_set(jar, "a.example", "/", true, "stays=1; Max-Age=86400",
        NOW);
    recon_cookie_set(jar, "a.example", "/", true, "goes=2", NOW);
    recon_cookie_set(jar, "b.example", "/", true, "also-goes=3", NOW);

    check(recon_cookie_forget_session(jar) == 2,
        "the ones with no expiry go, and it says how many");
    check_text(sent(jar, "a.example", "/", true), "stays=1",
        "what a server asked to outlive the window is still here");
    check_text(sent(jar, "b.example", "/", true), "",
        "and a host whose only cookie was for the window has nothing left");

    /* Twice is not worse than once: a window closed while already closed is
     * not a state the shell can reach, but a function that only works the
     * first time is a trap for whoever calls it next. */
    check(recon_cookie_forget_session(jar) == 0, "again drops nothing");
    check(recon_cookie_count(jar) == 1, "and takes nothing with it");

    recon_cookie_jar_free(jar);
}

static void test_it_is_not_every_cookie(void) {
    printf("and ending a session is not the same as forgetting everything\n");

    /*
     * The failure this guards is the opposite one, and it is worse.
     *
     * Close and minimize arrive at the same `visibility(false)` callback, so
     * the obvious way to write this fix ends the session on a **minimize** --
     * and signing somebody out because they put the window down for a second
     * is a far worse bug than a session cookie that lived too long. The
     * browser uses `closed` instead, which fires from `recon_appwin_hide`
     * alone.
     *
     * That dispatch needs a window and a server, so the check on it is
     * `scripts/cookie-survives-shot.sh`, which minimizes a real browser and
     * photographs what comes back. What is left for here is the half that
     * this function owns: that it is a scalpel and not `forget_all`.
     */
    struct recon_cookie_jar *jar = recon_cookie_jar_new();

    if (jar == NULL) {
        return;
    }

    recon_cookie_set(jar, "a.example", "/", true, "one=1; Max-Age=86400",
        NOW);
    recon_cookie_set(jar, "a.example", "/", true, "two=2; Max-Age=86400",
        NOW);

    check(recon_cookie_forget_session(jar) == 0,
        "a jar with nothing session-only loses nothing");
    check(recon_cookie_count(jar) == 2, "all of it still there");

    check(recon_cookie_forget_session(NULL) == 0, "and no jar is not a crash");

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

/* --- Across a restart ---------------------------------------------------
 *
 * The jar used to be written nowhere, and refusal 4 at the top of
 * `include/recon_cookie.h` said so and said what it was waiting on. It is
 * written now, into a sealed file, and what matters is not that a cookie comes
 * back -- it is **which** ones do:
 *
 *   - one with an expiry, yes: that is what the server asked for
 *   - one without, never: "until the window closes" is what a session cookie
 *     *means*, and keeping one would be storing something a server asked not
 *     to be stored
 *   - one whose expiry passed while the browser was shut, never, and **not in
 *     the jar to be swept later** -- the gap between arriving and being swept
 *     is a gap in which it can be sent
 *
 * And the file itself has to be a sealed file rather than a list of somebody's
 * sessions: a check that the value is not sitting in it in plain text is the
 * one that would catch the sealing being quietly dropped, and every other
 * check here would stay green.
 */

#define REAL_SOON ((time_t)(NOW + 86400))

static void seal_ready(void) {
    recon_keyring_lock();
    check(recon_keyring_unlock("Tester", "correct horse"),
        "the keyring unlocks, so there is somewhere to keep a jar");
}

/* A jar with one of each kind of cookie in it. */
static struct recon_cookie_jar *a_jar_with_three(void) {
    struct recon_cookie_jar *jar = recon_cookie_jar_new();

    check(recon_cookie_set(jar, "example.com", "/", true,
        "session=abc; Path=/; Secure", NOW) == NULL, "a session cookie is set");
    check(recon_cookie_set(jar, "example.com", "/", true,
        "keep=xyz; Path=/; Max-Age=86400", NOW) == NULL,
        "and one with a day to run");
    check(recon_cookie_set(jar, "other.example", "/app", false,
        "pref=dark; Path=/app; Max-Age=86400", NOW) == NULL,
        "and one for another host");
    return jar;
}

static void test_only_what_outlives_the_window_is_kept(void) {
    printf("what survives a restart, and what does not\n");

    seal_ready();
    recon_cookie_jar_forget_saved();

    struct recon_cookie_jar *jar = a_jar_with_three();

    check(recon_cookie_count(jar) == 3, "three in the jar");
    check(recon_cookie_jar_save(jar), "it is saved");
    recon_cookie_jar_free(jar);

    /* A new window, later the same day. */
    jar = recon_cookie_jar_new();
    check(recon_cookie_jar_load(jar, NOW + 60) == 2, "two come back");

    char header[512];

    recon_cookie_header(jar, "example.com", "/", true, NOW + 60, header,
        sizeof(header));
    check(strstr(header, "keep=xyz") != NULL, "the one with an expiry is sent");
    check(strstr(header, "session=abc") == NULL,
        "and the session cookie is not, because that is what session means");

    recon_cookie_header(jar, "other.example", "/app", false, NOW + 60, header,
        sizeof(header));
    check_text(header, "pref=dark", "the other host's came back too");

    recon_cookie_jar_free(jar);
}

static void test_one_that_expired_while_it_was_shut(void) {
    printf("a cookie whose day passed while the browser was closed\n");

    seal_ready();
    recon_cookie_jar_forget_saved();

    struct recon_cookie_jar *jar = a_jar_with_three();

    check(recon_cookie_jar_save(jar), "saved");
    recon_cookie_jar_free(jar);

    jar = recon_cookie_jar_new();

    /*
     * Two days later. Not "loaded and then swept": the count is what is in the
     * jar, and it has to be zero -- a cookie waiting to be tidied up is a
     * cookie that can be sent before anything tidies.
     */
    check(recon_cookie_jar_load(jar, NOW + 2 * 86400) == 0,
        "nothing is restored");
    check(recon_cookie_count(jar) == 0, "and the jar is empty, not pending");
    recon_cookie_jar_free(jar);
}

static void test_an_emptied_jar_stays_emptied(void) {
    printf("forgetting them reaches the disk\n");

    seal_ready();

    struct recon_cookie_jar *jar = a_jar_with_three();

    check(recon_cookie_jar_save(jar), "saved");
    recon_cookie_forget_all(jar);
    check(recon_cookie_jar_save(jar), "and saved again, empty");
    recon_cookie_jar_free(jar);

    jar = recon_cookie_jar_new();
    check(recon_cookie_jar_load(jar, NOW + 60) == 0, "nothing comes back");
    recon_cookie_jar_free(jar);

    /*
     * And the other half: forgetting the saved file itself. Without it,
     * "clear cookies" empties the jar and leaves the file, so a crash or a
     * second window brings them back -- after somebody was told they were
     * gone.
     */
    jar = a_jar_with_three();
    check(recon_cookie_jar_save(jar), "saved once more");
    recon_cookie_jar_free(jar);

    check(recon_cookie_jar_forget_saved(), "the saved jar is forgotten");
    check(!recon_sealed_exists("browser/cookies"), "and the file is gone");

    jar = recon_cookie_jar_new();
    check(recon_cookie_jar_load(jar, NOW + 60) == 0, "so nothing comes back");
    recon_cookie_jar_free(jar);
}

static void test_the_file_is_not_a_list_of_sessions(void) {
    printf("what is on the disk\n");

    seal_ready();
    recon_cookie_jar_forget_saved();

    struct recon_cookie_jar *jar = a_jar_with_three();

    check(recon_cookie_jar_save(jar), "saved");
    recon_cookie_jar_free(jar);

    size_t size = 0;
    char *raw = recon_fs_read("/", "/Users/Tester/.sealed/browser/cookies",
        &size);

    check(raw != NULL, "there is a file");
    if (raw == NULL) {
        return;
    }

    /*
     * The check that would catch the sealing being dropped. Every other check
     * in this section passes against a version that writes the jar in plain
     * text, which is exactly the shape `tests/test_keyring.c` warns about.
     */
    bool found = false;

    for (size_t i = 0; i + 3 <= size; i++) {
        if (memcmp(raw + i, "xyz", 3) == 0) {
            found = true;
        }
    }
    check(!found, "and a cookie's value is nowhere in it");
    free(raw);
}

static void test_a_session_cookie_is_not_even_written_down(void) {
    printf("what a session cookie leaves on the disk\n");

    seal_ready();
    recon_cookie_jar_forget_saved();

    struct recon_cookie_jar *jar = a_jar_with_three();

    check(recon_cookie_jar_save(jar), "saved");
    recon_cookie_jar_free(jar);

    /*
     * Inside the seal, not outside it.
     *
     * `test_the_file_is_not_a_list_of_sessions` looks at the ciphertext and
     * would notice the sealing being dropped. This looks at what was sealed,
     * and notices something else: **a session cookie being written down at
     * all.**
     *
     * The mutation pass found this gap. Saving session cookies too changed no
     * check, because `recon_cookie_restore` refuses an expiry of zero and the
     * one that came back was still the right one. So the restore-side refusal
     * was standing in front of the save-side one and hiding whether it worked.
     *
     * It matters on its own. A server that said "until the window closes" has
     * asked for something, and a sealed file is still a file -- one that is
     * read back by this process, where every module runs.
     */
    size_t size = 0;
    char *plain = recon_sealed_read("browser/cookies", &size);

    check(plain != NULL, "the sealed jar opens");
    if (plain == NULL) {
        return;
    }

    check(strstr(plain, "keep") != NULL,
        "the cookie with an expiry is in it");
    check(strstr(plain, "session") == NULL,
        "and the session cookie's name is not");
    check(strstr(plain, "abc") == NULL, "nor its value");
    free(plain);
    recon_cookie_jar_forget_saved();
}

static void test_a_line_that_is_not_a_cookie_is_skipped(void) {
    printf("a saved line with a field missing\n");

    seal_ready();

    /*
     * Four fields where there should be six, and the missing ones are at the
     * end -- so a reader that ran out of tabs and carried on would use the
     * last field as the name *and* the value, and store a cookie called
     * `/onlypath` that no server ever set.
     *
     * The mutation pass found this too. Every forged line in the test above
     * happened to be caught by a refusal further down -- a path that is not a
     * path, a host that is empty -- so the skip itself was never the thing
     * under test. This line is built so that the downstream refusals would
     * *pass* it: the host is real and the path begins with a slash.
     */
    char forged[512];
    int n = snprintf(forged, sizeof(forged),
        "# ReconOS cookies\n"
        "%lld\t-\texample.com\t/onlypath\n"
        "%lld\t-\texample.com\t/\tgood\tvalue\n",
        (long long)REAL_SOON, (long long)REAL_SOON);

    check(n > 0 && recon_sealed_write("browser/cookies", forged, (size_t)n),
        "a short line is sealed into a jar");

    struct recon_cookie_jar *jar = recon_cookie_jar_new();

    check(recon_cookie_jar_load(jar, NOW) == 1, "one cookie is restored");

    char header[512];

    recon_cookie_header(jar, "example.com", "/onlypath", false, NOW, header,
        sizeof(header));
    check_text(header, "good=value",
        "and the short line became nothing, not a cookie named after a path");
    recon_cookie_jar_free(jar);
    recon_cookie_jar_forget_saved();
}

static void test_a_locked_machine_keeps_nothing(void) {
    printf("a locked keyring\n");

    seal_ready();
    recon_cookie_jar_forget_saved();

    struct recon_cookie_jar *jar = a_jar_with_three();

    check(recon_cookie_jar_save(jar), "saved while signed in");
    recon_cookie_jar_free(jar);

    recon_keyring_lock();

    jar = recon_cookie_jar_new();
    check(recon_cookie_jar_load(jar, NOW + 60) == 0,
        "and locked, nothing loads");

    struct recon_cookie_jar *other = a_jar_with_three();

    check(!recon_cookie_jar_save(other), "nor can anything be saved");
    recon_cookie_jar_free(other);
    recon_cookie_jar_free(jar);

    seal_ready();
    jar = recon_cookie_jar_new();
    check(recon_cookie_jar_load(jar, NOW + 60) == 2,
        "and signing in again brings them back");
    recon_cookie_jar_free(jar);
}

static void test_a_saved_file_cannot_smuggle_one_in(void) {
    printf("a saved jar somebody has written themselves\n");

    seal_ready();

    /*
     * Sealed by this account, so the cipher is satisfied -- which is the
     * interesting case. Somebody who can write the folder cannot forge a
     * sealed file, but anything that can call `recon_sealed_write` in this
     * process can, and every module can. So the *format* has to refuse what a
     * server would have been refused.
     */
    char forged[2048];
    int n = snprintf(forged, sizeof(forged),
        "# ReconOS cookies\n"
        /* No expiry: a session cookie has no business in a saved jar. */
        "0\t-\texample.com\t/\tsneak-session\tvalue\n"
        /* Already gone. */
        "%lld\t-\texample.com\t/\tsneak-expired\tvalue\n"
        /* No host. */
        "%lld\t-\t\t/\tsneak-nohost\tvalue\n"
        /* A path that is not one. */
        "%lld\t-\texample.com\tnot-a-path\tsneak-path\tvalue\n"
        /* Too few fields to be a cookie at all. */
        "%lld\t-\texample.com\n"
        /* And one that is fine, so the refusals are not simply "refuse". */
        "%lld\t-\texample.com\t/\tgood\tvalue\n",
        (long long)(NOW - 1), (long long)REAL_SOON, (long long)REAL_SOON,
        (long long)REAL_SOON, (long long)REAL_SOON);

    check(n > 0, "a forged jar is built");
    check(recon_sealed_write("browser/cookies", forged, (size_t)n),
        "and sealed under this account");

    struct recon_cookie_jar *jar = recon_cookie_jar_new();

    check(recon_cookie_jar_load(jar, NOW) == 1, "exactly one is restored");

    char header[512];

    recon_cookie_header(jar, "example.com", "/", false, NOW, header,
        sizeof(header));
    check_text(header, "good=value", "and it is the one that was well formed");
    recon_cookie_jar_free(jar);
    recon_cookie_jar_forget_saved();
}

static void test_a_control_character_is_refused(void) {
    printf("a cookie with a tab in it\n");

    struct recon_cookie_jar *jar = recon_cookie_jar_new();

    /*
     * The refusal the saved format depends on. A tab inside a value would be
     * a cookie that reads back as a different cookie, and nothing well formed
     * has one -- a `Set-Cookie` arrives as a line and RFC 6265 excludes
     * controls from a value outright.
     */
    check(recon_cookie_set(jar, "example.com", "/", false,
        "bad=one\ttwo", NOW) != NULL, "a tab in a value is refused");
    check(recon_cookie_set(jar, "example.com", "/", false,
        "ba\td=one", NOW) != NULL, "and in a name");
    check(recon_cookie_count(jar) == 0, "so neither is in the jar");

    check(recon_cookie_set(jar, "example.com", "/", false,
        "fine=one two", NOW) == NULL, "while a space is not a control");
    check(recon_cookie_count(jar) == 1, "and that one is kept");
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
    test_closing_the_window_ends_the_session();
    test_it_is_not_every_cookie();
    test_nothing_is_refused_with_a_crash();
    test_a_control_character_is_refused();

    /*
     * The rest need somewhere to keep a sealed file, which means a filesystem
     * and an account. Set up here rather than at the top so that everything
     * above stays a test of the policy alone, with nothing underneath it.
     */
    char root[] = "/tmp/recon-cookie-test-XXXXXX";

    if (mkdtemp(root) == NULL) {
        printf("could not make a temporary root\n");
        return 1;
    }
    setenv("RECONOS_ROOT", root, 1);

    if (!recon_fs_init(NULL)) {
        printf("could not start the filesystem: %s\n", recon_fs_last_error());
        return 1;
    }
    recon_fs_mkdir("/", "/Users");
    recon_users_init();

    if (!recon_users_create("Tester", "correct horse",
            RECON_ROLE_ADMINISTRATOR)) {
        printf("could not make the test account: %s\n",
            recon_users_last_error());
        return 1;
    }
    recon_fs_mkdir("/", "/Users/Tester");

    test_only_what_outlives_the_window_is_kept();
    test_one_that_expired_while_it_was_shut();
    test_an_emptied_jar_stays_emptied();
    test_the_file_is_not_a_list_of_sessions();
    test_a_session_cookie_is_not_even_written_down();
    test_a_line_that_is_not_a_cookie_is_skipped();
    test_a_locked_machine_keeps_nothing();
    test_a_saved_file_cannot_smuggle_one_in();

    recon_keyring_lock();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
