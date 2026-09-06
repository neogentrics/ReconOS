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

    /* This used to be refused, and the check used to read "two addresses,
     * because this sends to one". It sends to several now. */
    l = a_letter("one@a.com, two@b.com", "Hi", "x");
    check(recon_smtp_letter_ok(&l, why, sizeof(why)),
        "a comma-separated list, now that there can be more than one");

    l = a_letter("one@a.com,,two@b.com,", "Hi", "x");
    check(recon_smtp_letter_ok(&l, why, sizeof(why)),
        "and stray commas, which are a typing artefact and not a statement");

    l = a_letter("one@a.com, nobody", "Hi", "x");
    check(!recon_smtp_letter_ok(&l, why, sizeof(why)),
        "but one bad address in a list still stops the whole letter");
    check(strstr(why, "nobody") != NULL,
        "and the reason names which one, out of however many");

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

/* --- Who it goes to, and who is told --- */

/*
 * The one thing about Bcc that has to be right.
 *
 * Every address goes in the envelope; only To and Cc go in the message. Get
 * that backwards and the letter still sends, still arrives, and quietly hands
 * every recipient the list of people who were meant to be hidden -- which is
 * the failure that has embarrassed real mail software repeatedly and is not
 * visible from the sender's side at all.
 */
static void test_blind_copies(void) {
    printf("Cc and Bcc\n");

    char why[192];
    struct recon_smtp_letter l;
    struct recon_smtp_recipients everyone;

    /* --- The envelope has all three --- */

    l = a_letter("to@a.com", "Hi", "x");
    snprintf(l.cc, sizeof(l.cc), "%s", "cc1@b.com, cc2@b.com");
    snprintf(l.bcc, sizeof(l.bcc), "%s", "secret@c.com");

    check(recon_smtp_recipients_of(&l, &everyone, why, sizeof(why)),
        "a letter with all three fields splits");
    check(everyone.count == 4, "and everybody named is in the envelope");
    check(strcmp(everyone.address[0], "to@a.com") == 0, "To first");
    check(strcmp(everyone.address[1], "cc1@b.com") == 0, "then Cc");
    check(strcmp(everyone.address[2], "cc2@b.com") == 0, "in order");
    check(strcmp(everyone.address[3], "secret@c.com") == 0, "then Bcc");

    /* --- The message has two of them --- */

    struct recon_smtp_account account = an_account();
    char out[8192];
    size_t n = recon_smtp_compose(&account, &l, NULL, out, sizeof(out));

    check(n > 0, "and it composes");
    check(strstr(out, "To: to@a.com\r\n") != NULL, "To is written");
    check(strstr(out, "Cc: cc1@b.com, cc2@b.com\r\n") != NULL,
        "Cc is written, with the list joined the same way every time");
    check(strstr(out, "secret@c.com") == NULL,
        "AND THE BCC ADDRESS APPEARS NOWHERE IN THE MESSAGE");
    check(strstr(out, "Bcc") == NULL && strstr(out, "bcc") == NULL,
        "not even the header name, which would say one was used");

    /* --- Each field can carry the letter on its own --- */

    struct recon_smtp_letter cc_only;
    memset(&cc_only, 0, sizeof(cc_only));
    snprintf(cc_only.cc, sizeof(cc_only.cc), "%s", "only@b.com");
    cc_only.body = "x";
    check(recon_smtp_letter_ok(&cc_only, why, sizeof(why)),
        "a letter with only a Cc is a real letter");

    struct recon_smtp_letter bcc_only;
    memset(&bcc_only, 0, sizeof(bcc_only));
    snprintf(bcc_only.bcc, sizeof(bcc_only.bcc), "%s", "only@c.com");
    bcc_only.body = "x";
    check(recon_smtp_letter_ok(&bcc_only, why, sizeof(why)),
        "and so is one with only a Bcc");

    n = recon_smtp_compose(&account, &bcc_only, NULL, out, sizeof(out));
    check(n > 0, "which composes");
    check(strstr(out, "To:") == NULL,
        "with no To header, because there is honestly nobody to put in it");
    check(strstr(out, "only@c.com") == NULL,
        "and still nothing naming the recipient");

    /* --- Nobody at all is still refused --- */

    struct recon_smtp_letter nobody;
    memset(&nobody, 0, sizeof(nobody));
    nobody.body = "x";
    check(!recon_smtp_letter_ok(&nobody, why, sizeof(why)),
        "three empty fields is nobody, and that is still an error");

    /* --- The limit is a limit --- */

    struct recon_smtp_letter crowd;
    memset(&crowd, 0, sizeof(crowd));
    crowd.body = "x";
    size_t at = 0;
    for (int i = 0; i < RECON_SMTP_RECIPIENTS_MAX + 1; i++) {
        int w = snprintf(crowd.to + at, sizeof(crowd.to) - at, "%sp%d@e.com",
            i > 0 ? "," : "", i);
        if (w <= 0) {
            break;
        }
        at += (size_t)w;
    }
    check(!recon_smtp_letter_ok(&crowd, why, sizeof(why)),
        "one more than the limit is refused rather than quietly trimmed");
}

/* --- Attachments --- */

static void attach(struct recon_smtp_letter *l, const char *name,
        const char *content) {
    int i = l->attachment_count++;
    snprintf(l->attachment[i].name, sizeof(l->attachment[i].name), "%s", name);
    l->attachment[i].bytes = (const unsigned char *)content;
    l->attachment[i].size = strlen(content);
}

static void test_attachments(void) {
    printf("Attachments\n");

    struct recon_smtp_account account = an_account();
    char out[65536];
    char why[256];

    /* --- Nothing attached is the message it always was --- */

    struct recon_smtp_letter plain = a_letter("to@a.com", "Hi", "Text.\n");
    check(recon_smtp_compose(&account, &plain, NULL, out, sizeof(out)) > 0,
        "a letter with nothing attached composes");
    check(strstr(out, "Content-Type: text/plain; charset=utf-8\r\n") != NULL,
        "as one plain part, exactly as before");
    check(strstr(out, "multipart") == NULL && strstr(out, "boundary") == NULL,
        "with no boundary and no multipart wrapper anywhere in it");

    /* --- One attached file --- */

    struct recon_smtp_letter l = a_letter("to@a.com", "Hi", "See attached.\n");
    attach(&l, "notes.txt", "hello");

    check(recon_smtp_letter_ok(&l, why, sizeof(why)),
        "a letter with a file is sendable");
    size_t n = recon_smtp_compose(&account, &l, NULL, out, sizeof(out));
    check(n > 0, "and composes");

    check(strstr(out, "Content-Type: multipart/mixed; boundary=\"") != NULL,
        "the top-level type says multipart and names the boundary");
    check(strstr(out, "Content-Type: text/plain; charset=utf-8\r\n") != NULL,
        "the text is still a plain part");
    check(strstr(out, "See attached.\r\n") != NULL, "and it is still there");
    check(strstr(out, "Content-Type: text/plain; name=\"notes.txt\"") != NULL,
        "the file's type is worked out from its name");
    check(strstr(out, "Content-Transfer-Encoding: base64\r\n") != NULL,
        "it travels as base64");
    check(strstr(out,
        "Content-Disposition: attachment; filename=\"notes.txt\"") != NULL,
        "and is offered as an attachment rather than shown inline");
    check(strstr(out, "aGVsbG8=\r\n") != NULL,
        "with the content encoded -- 'hello' is 'aGVsbG8='");

    /* The closing boundary, which is what says the last part was the last. */
    const char *b = strstr(out, "boundary=\"");
    check(b != NULL, "the boundary is findable");
    if (b != NULL) {
        char mark[80];
        snprintf(mark, sizeof(mark), "%s", b + strlen("boundary=\""));
        char *quote = strchr(mark, '"');
        if (quote != NULL) {
            *quote = '\0';
        }
        char closing[96];
        snprintf(closing, sizeof(closing), "--%s--\r\n", mark);
        check(strstr(out, closing) != NULL,
            "and the message ends with the closing boundary");
    }

    /* --- The type is honest when it does not know --- */

    struct recon_smtp_letter odd = a_letter("to@a.com", "Hi", "x");
    attach(&odd, "thing.qqq", "data");
    recon_smtp_compose(&account, &odd, NULL, out, sizeof(out));
    check(strstr(out, "application/octet-stream") != NULL,
        "an unknown extension is octet-stream rather than a guess");

    struct recon_smtp_letter shouty = a_letter("to@a.com", "Hi", "x");
    attach(&shouty, "PHOTO.PNG", "data");
    recon_smtp_compose(&account, &shouty, NULL, out, sizeof(out));
    check(strstr(out, "image/png") != NULL,
        "and the extension is matched whatever case it is written in");

    /*
     * --- The boundary is checked, not assumed ---
     *
     * The dangerous case: a body that already contains the boundary. Left
     * alone, the message splits there and everything after it is read as
     * another part -- which somebody who can put text in a body could do on
     * purpose.
     */
    struct recon_smtp_letter attack = a_letter("to@a.com", "Hi",
        "innocent text\r\n"
        "--=_ReconOS_0_=\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "a part I added myself\r\n");
    attach(&attack, "x.txt", "y");

    n = recon_smtp_compose(&account, &attack, NULL, out, sizeof(out));
    check(n > 0, "a body containing the first candidate boundary still sends");

    const char *decl = strstr(out, "boundary=\"");
    check(decl != NULL, "and declares a boundary");
    if (decl != NULL) {
        char used[80];
        snprintf(used, sizeof(used), "%s", decl + strlen("boundary=\""));
        char *quote = strchr(used, '"');
        if (quote != NULL) {
            *quote = '\0';
        }
        check(strcmp(used, "=_ReconOS_0_=") != 0,
            "which is NOT the one the body already contained");
        check(strstr(attack.body, used) == NULL,
            "and does not appear in the body at all");
    }

    /* --- A filename is a header value, and can be attacked like one --- */

    struct recon_smtp_letter injected = a_letter("to@a.com", "Hi", "x");
    attach(&injected, "ok.txt\r\nBcc: someone@evil.com", "y");
    check(!recon_smtp_letter_ok(&injected, why, sizeof(why)),
        "a filename with a line break is refused");

    struct recon_smtp_letter quoted = a_letter("to@a.com", "Hi", "x");
    attach(&quoted, "a\".txt", "y");
    check(!recon_smtp_letter_ok(&quoted, why, sizeof(why)),
        "and so is one with a quote, which would end the header value early");

    /* --- Sizes --- */

    struct recon_smtp_letter empty_file = a_letter("to@a.com", "Hi", "x");
    attach(&empty_file, "nothing.txt", "");
    check(recon_smtp_letter_ok(&empty_file, why, sizeof(why)),
        "an empty file is a real file");
    check(recon_smtp_compose(&account, &empty_file, NULL, out, sizeof(out)) > 0,
        "and composes, as a part with a name and no content");

    struct recon_smtp_letter too_many = a_letter("to@a.com", "Hi", "x");
    too_many.attachment_count = RECON_SMTP_ATTACHMENTS_MAX + 1;
    check(!recon_smtp_letter_ok(&too_many, why, sizeof(why)),
        "more files than the limit is refused");

    struct recon_smtp_letter huge = a_letter("to@a.com", "Hi", "x");
    huge.attachment_count = 1;
    snprintf(huge.attachment[0].name, sizeof(huge.attachment[0].name), "big.bin");
    huge.attachment[0].bytes = (const unsigned char *)"x";
    huge.attachment[0].size = RECON_SMTP_ATTACHED_BYTES_MAX + 1;
    check(!recon_smtp_letter_ok(&huge, why, sizeof(why)),
        "and so is more than the total size, with the number said out loud");

    /* --- Base64 lines are wrapped --- */

    static char big[4096];
    memset(big, 'A', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    struct recon_smtp_letter wrapped = a_letter("to@a.com", "Hi", "x");
    attach(&wrapped, "big.txt", big);
    check(recon_smtp_compose(&account, &wrapped, NULL, out, sizeof(out)) > 0,
        "a file large enough to need wrapping composes");

    int longest = 0;
    int run = 0;
    for (const char *p = out; *p != '\0'; p++) {
        if (*p == '\n') {
            run = 0;
        } else if (*p != '\r') {
            run++;
            if (run > longest) {
                longest = run;
            }
        }
    }
    check(longest <= 998,
        "and no line in the whole message reaches SMTP's limit of 998");
    check(longest <= 100,
        "in fact none is much over the conventional 76");
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
    test_blind_copies();
    test_attachments();
    test_headers();
    test_line_endings();
    test_the_dot();
    test_it_refuses_rather_than_truncates();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
