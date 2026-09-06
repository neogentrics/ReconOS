/*
 * Composing a message, and refusing to compose a dangerous one.
 *
 * The conversation with a server is not tested here -- that needs a server, and
 * a fake one written alongside the client tests that the two agree with each
 * other. What IS testable without one is the part with the rules in it: the
 * shape of the headers, the dot that has to be doubled, the line endings the
 * protocol will not forgive, and the newline in a subject that turns one
 * message into two.
 *
 * That last one is the oldest bug in mail software and it is the reason this
 * file exists.
 */

#include <stdio.h>
#include <string.h>

#include "recon_smtp.h"

static int checks = 0;
static int failures = 0;

static void check(bool ok, const char *what) {
    checks++;
    if (ok) {
        printf("  ok    %s\n", what);
    } else {
        failures++;
        printf("  FAIL  %s\n", what);
    }
}

static struct recon_smtp_account an_account(void) {
    struct recon_smtp_account a;
    memset(&a, 0, sizeof(a));
    snprintf(a.host, sizeof(a.host), "%s", "smtp.example.com");
    snprintf(a.user, sizeof(a.user), "%s", "joshua");
    snprintf(a.from, sizeof(a.from), "%s", "joshua@example.com");
    a.port = RECON_SMTP_TLS_PORT;
    return a;
}

static struct recon_smtp_letter a_letter(const char *to, const char *subject,
        const char *body) {
    struct recon_smtp_letter l;
    memset(&l, 0, sizeof(l));
    snprintf(l.to, sizeof(l.to), "%s", to);
    snprintf(l.subject, sizeof(l.subject), "%s", subject);
    l.body = body;
    return l;
}

/* --- What it will not send --- */

static void test_header_injection(void) {
    printf("A newline in a header\n");

    char why[192];

    /*
     * The attack, in the form it actually takes. A subject carrying a carriage
     * return lets whoever wrote it add headers of their own, and the message
     * that arrives has a recipient the sender never saw.
     */
    struct recon_smtp_letter bad = a_letter("someone@example.com",
        "Hello\r\nBcc: everybody@example.com", "text");
    check(!recon_smtp_letter_ok(&bad, why, sizeof(why)),
        "a subject with a carriage return in it is refused");

    struct recon_smtp_letter nl = a_letter("someone@example.com",
        "Hello\nBcc: everybody@example.com", "text");
    check(!recon_smtp_letter_ok(&nl, why, sizeof(why)),
        "and a plain newline too");

    struct recon_smtp_letter to = a_letter(
        "someone@example.com\r\nRCPT TO:<other@example.com>", "Hi", "text");
    check(!recon_smtp_letter_ok(&to, why, sizeof(why)),
        "and an address carrying a whole extra command");

    /*
     * Refused at composing as well, not only at checking. The two are separate
     * entry points and a caller could reach the second without the first --
     * which is exactly how a check that exists comes to not run.
     */
    struct recon_smtp_account account = an_account();
    char out[4096];
    check(recon_smtp_compose(&account, &bad, NULL, out, sizeof(out)) == 0,
        "and composing refuses it too, not just the check before it");

    /* And a sender's address, which comes from the settings rather than from
     * the message and is therefore the one nobody thinks to check. */
    struct recon_smtp_account poisoned = an_account();
    snprintf(poisoned.from, sizeof(poisoned.from), "%s",
        "me@example.com\r\nBcc: everybody@example.com");
    struct recon_smtp_letter fine = a_letter("someone@example.com", "Hi", "x");
    check(recon_smtp_compose(&poisoned, &fine, NULL, out, sizeof(out)) == 0,
        "a poisoned sender address in the settings is refused as well");
}

static void test_addresses(void) {
    printf("Addresses\n");

    char why[192];
    struct recon_smtp_letter l;

    l = a_letter("", "Hi", "x");
    check(!recon_smtp_letter_ok(&l, why, sizeof(why)), "no recipient");

    l = a_letter("nobody", "Hi", "x");
    check(!recon_smtp_letter_ok(&l, why, sizeof(why)), "no at sign");

    l = a_letter("@example.com", "Hi", "x");
    check(!recon_smtp_letter_ok(&l, why, sizeof(why)), "nothing before the at");

    l = a_letter("someone@", "Hi", "x");
    check(!recon_smtp_letter_ok(&l, why, sizeof(why)), "nothing after it");

    l = a_letter("a@b@c.com", "Hi", "x");
    check(!recon_smtp_letter_ok(&l, why, sizeof(why)), "two at signs");

    l = a_letter("one@a.com, two@b.com", "Hi", "x");
    check(!recon_smtp_letter_ok(&l, why, sizeof(why)),
        "two addresses, because this sends to one");

    l = a_letter("Joshua <j@example.com>", "Hi", "x");
    check(!recon_smtp_letter_ok(&l, why, sizeof(why)),
        "a display name, which this does not take yet");

    l = a_letter("someone@example.com", "Hi", "x");
    check(recon_smtp_letter_ok(&l, why, sizeof(why)),
        "and a plain one is fine");

    /* Deliberately shallow: the server decides whether an address exists, and
     * a stricter parser here would reject working addresses to no purpose. */
    l = a_letter("odd+tag@sub.domain.example.museum", "Hi", "x");
    check(recon_smtp_letter_ok(&l, why, sizeof(why)),
        "including an unusual but perfectly real one");
}

/* --- What it writes --- */

static void test_headers(void) {
    printf("The headers\n");

    struct recon_smtp_account account = an_account();
    struct recon_smtp_letter letter = a_letter("someone@example.com",
        "A subject", "One line.\n");

    char out[4096];
    size_t n = recon_smtp_compose(&account, &letter, "Sun, 6 Sep 2026 04:00:00 +0000",
        out, sizeof(out));

    check(n > 0, "a plain letter composes");
    check(strstr(out, "From: joshua@example.com\r\n") != NULL, "From is there");
    check(strstr(out, "To: someone@example.com\r\n") != NULL, "To is there");
    check(strstr(out, "Subject: A subject\r\n") != NULL, "Subject is there");
    check(strstr(out, "Date: Sun, 6 Sep 2026 04:00:00 +0000\r\n") != NULL,
        "and the date it was given");
    check(strstr(out, "Content-Type: text/plain; charset=utf-8\r\n") != NULL,
        "the content type is said rather than left to be guessed");
    check(strstr(out, "\r\n\r\nOne line.\r\n") != NULL,
        "and a blank line separates the headers from the body");

    /* No date rather than an invented one. */
    n = recon_smtp_compose(&account, &letter, NULL, out, sizeof(out));
    check(n > 0 && strstr(out, "Date:") == NULL,
        "with no date given, the header is left out rather than made up");

    /* An empty subject is left out rather than sent empty. */
    struct recon_smtp_letter quiet = a_letter("someone@example.com", "", "hi\n");
    recon_smtp_compose(&account, &quiet, NULL, out, sizeof(out));
    check(strstr(out, "Subject:") == NULL, "an empty subject is left out");
}

static void test_line_endings(void) {
    printf("Line endings\n");

    struct recon_smtp_account account = an_account();
    char out[4096];

    /* Whatever they were typed as, they go out CRLF. */
    struct recon_smtp_letter unix_ends = a_letter("a@b.com", "S", "one\ntwo\n");
    recon_smtp_compose(&account, &unix_ends, NULL, out, sizeof(out));
    check(strstr(out, "one\r\ntwo\r\n") != NULL, "bare newlines become CRLF");

    struct recon_smtp_letter crlf = a_letter("a@b.com", "S", "one\r\ntwo\r\n");
    recon_smtp_compose(&account, &crlf, NULL, out, sizeof(out));
    check(strstr(out, "one\r\ntwo\r\n") != NULL,
        "and CRLF stays CRLF rather than becoming CRCRLF");

    /* A body that does not end with a newline still has to, or the dot that
     * follows lands on the end of somebody's sentence. */
    struct recon_smtp_letter unterminated = a_letter("a@b.com", "S", "no ending");
    size_t n = recon_smtp_compose(&account, &unterminated, NULL, out,
        sizeof(out));
    check(n >= 2 && strcmp(out + n - 2, "\r\n") == 0,
        "a body with no final newline gets one");

    struct recon_smtp_letter empty = a_letter("a@b.com", "S", "");
    n = recon_smtp_compose(&account, &empty, NULL, out, sizeof(out));
    check(n > 0, "an empty body is allowed");
}

static void test_the_dot(void) {
    printf("The dot\n");

    struct recon_smtp_account account = an_account();
    char out[4096];

    /*
     * A line that is exactly a dot ends the message. So a letter containing
     * one has to have it doubled, or the message is cut off there and the rest
     * of somebody's letter arrives as SMTP commands.
     */
    struct recon_smtp_letter lone = a_letter("a@b.com", "S", "before\n.\nafter\n");
    recon_smtp_compose(&account, &lone, NULL, out, sizeof(out));
    check(strstr(out, "before\r\n..\r\nafter\r\n") != NULL,
        "a line that is only a dot is doubled");

    struct recon_smtp_letter leading = a_letter("a@b.com", "S",
        ".hidden\n..also\n");
    recon_smtp_compose(&account, &leading, NULL, out, sizeof(out));
    /*
     * ONE more, not two. The rule is that a leading dot is doubled, so
     * ".hidden" becomes "..hidden" -- and this assertion originally asked for
     * three, which is what "..hidden" would become if the rule were applied
     * twice. The code was right and the test was wrong.
     */
    check(strstr(out, "..hidden\r\n") != NULL,
        "a line starting with a dot gets exactly one more");
    check(strstr(out, "...also\r\n") != NULL,
        "and a line starting with two gets a third");

    /* A dot that is not at the start of a line is an ordinary full stop. */
    struct recon_smtp_letter sentence = a_letter("a@b.com", "S",
        "That is all. Really.\n");
    recon_smtp_compose(&account, &sentence, NULL, out, sizeof(out));
    check(strstr(out, "That is all. Really.\r\n") != NULL,
        "and a dot in the middle of a line is left alone");

    /* The very first line of the body counts as the start of a line. */
    struct recon_smtp_letter first = a_letter("a@b.com", "S", ".first\n");
    recon_smtp_compose(&account, &first, NULL, out, sizeof(out));
    check(strstr(out, "\r\n\r\n..first\r\n") != NULL,
        "including the first line of the body");
}

static void test_it_refuses_rather_than_truncates(void) {
    printf("A message too long for the buffer\n");

    struct recon_smtp_account account = an_account();
    struct recon_smtp_letter letter = a_letter("someone@example.com",
        "A subject", "a body long enough to matter\n");

    /* Small enough that the headers alone will not fit. */
    char tiny[32];
    check(recon_smtp_compose(&account, &letter, NULL, tiny, sizeof(tiny)) == 0,
        "it refuses");
    check(tiny[0] == '\0', "and leaves nothing behind rather than a short one");

    /*
     * The point of that: a truncated message is still valid SMTP. It would be
     * accepted, delivered, and arrive missing the end -- which is worse than
     * not being sent, because nobody is told.
     */
    char just_short[96];
    check(recon_smtp_compose(&account, &letter, NULL, just_short,
        sizeof(just_short)) == 0,
        "including when it is only the body that does not fit");
}

int main(void) {
    printf("SMTP\n\n");

    test_header_injection();
    test_addresses();
    test_headers();
    test_line_endings();
    test_the_dot();
    test_it_refuses_rather_than_truncates();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
