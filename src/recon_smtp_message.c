/*
 * What a message is, as distinct from how it gets there.
 *
 * Split from recon_smtp.c so it can be tested without a network stack behind
 * it. Everything here is a pure function of its arguments -- the rules about
 * what may appear in a header, and the shape of the bytes that go after DATA --
 * and those are exactly the rules worth checking, because getting them wrong
 * produces a message that sends perfectly and is not the message somebody
 * wrote.
 *
 * The conversation with a server lives next door and needs a server to test.
 */

#include <stdio.h>
#include <string.h>

#include "recon_smtp.h"

/* --- Checking a letter before it becomes one --- */

/*
 * Anything that would end a header line early.
 *
 * Carriage return and newline are the whole point. A subject containing one
 * lets whoever wrote it start a header of their own -- a second recipient, a
 * different sender, a whole second message -- and what arrives is not what was
 * shown on screen. Every other control character goes too: none of them belong
 * in a header and each is a way of confusing something downstream.
 */
static bool has_a_line_break(const char *text) {
    if (text == NULL) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (*p < 0x20 || *p == 0x7F) {
            return true;
        }
    }
    return false;
}

/*
 * An address, to the extent this checks one.
 *
 * Deliberately shallow: one at-sign, something either side of it, no spaces,
 * no line breaks. Anything more is a parser for RFC 5322, which is famously
 * larger than people expect and would reject working addresses to no purpose --
 * the server is the thing that decides whether an address exists, and it will
 * say so.
 *
 * What this is for is the difference between an address the server will reject
 * and an address that is an attack on the message being built around it.
 */
static bool looks_like_an_address(const char *text) {
    if (text == NULL || *text == '\0' || has_a_line_break(text)) {
        return false;
    }

    const char *at = strchr(text, '@');
    if (at == NULL || at == text || at[1] == '\0') {
        return false;
    }
    if (strchr(at + 1, '@') != NULL) {
        return false;
    }
    if (strchr(text, ' ') != NULL || strchr(text, '<') != NULL ||
            strchr(text, '>') != NULL || strchr(text, ',') != NULL) {
        return false;
    }
    return true;
}

bool recon_smtp_letter_ok(const struct recon_smtp_letter *letter,
        char *why, size_t why_size) {
    const char *problem = NULL;

    if (letter == NULL) {
        problem = "There is no message.";
    } else if (letter->to[0] == '\0') {
        problem = "There is nobody to send it to.";
    } else if (!looks_like_an_address(letter->to)) {
        problem = "That does not look like one address. One at a time, and "
            "no spaces or commas.";
    } else if (has_a_line_break(letter->subject)) {
        problem = "The subject has a line break in it, which cannot be sent.";
    }

    if (problem == NULL) {
        return true;
    }
    if (why != NULL && why_size > 0) {
        snprintf(why, why_size, "%s", problem);
    }
    return false;
}

/* --- Writing the message out --- */

size_t recon_smtp_compose(const struct recon_smtp_account *account,
        const struct recon_smtp_letter *letter, const char *now,
        char *out, size_t out_size) {
    if (account == NULL || letter == NULL || out == NULL || out_size == 0) {
        return 0;
    }
    out[0] = '\0';

    if (!recon_smtp_letter_ok(letter, NULL, 0)) {
        return 0;
    }
    if (has_a_line_break(account->from) || has_a_line_break(now)) {
        return 0;
    }

    size_t at = 0;

    /* Every write goes through this, so a buffer that fills up stops the whole
     * message rather than producing a shorter one that is still valid SMTP and
     * is missing the end of somebody's letter. */
    #define PUT(...) do { \
        int n = snprintf(out + at, out_size - at, __VA_ARGS__); \
        if (n < 0 || (size_t)n >= out_size - at) { \
            out[0] = '\0'; \
            return 0; \
        } \
        at += (size_t)n; \
    } while (0)

    PUT("From: %s\r\n", account->from);
    PUT("To: %s\r\n", letter->to);
    if (letter->subject[0] != '\0') {
        PUT("Subject: %s\r\n", letter->subject);
    }
    if (now != NULL && *now != '\0') {
        PUT("Date: %s\r\n", now);
    }

    /*
     * Said plainly rather than left out. A message with no content type is
     * guessed at by whatever opens it, and the guess is usually right and
     * occasionally turns a letter into markup.
     */
    PUT("MIME-Version: 1.0\r\n");
    PUT("Content-Type: text/plain; charset=utf-8\r\n");
    PUT("\r\n");

    /*
     * The body, with two rules the protocol will not forgive.
     *
     * Every line ends CRLF, whatever it ended with when it was typed. And a
     * line that begins with a dot gets a second one -- because a line that is
     * exactly "." is how the message ends, so a letter containing one would be
     * cut off there and the rest of it would arrive as commands. The server
     * takes one dot back off; that is the other half of the same rule and it
     * is why the doubling is invisible to everybody.
     */
    const char *body = letter->body != NULL ? letter->body : "";
    bool at_line_start = true;

    for (const char *p = body; *p != '\0'; p++) {
        if (at_line_start && *p == '.') {
            PUT("..");
            at_line_start = false;
            continue;
        }

        if (*p == '\r') {
            /* A CRLF already there is one ending, not two. */
            if (p[1] == '\n') {
                p++;
            }
            PUT("\r\n");
            at_line_start = true;
            continue;
        }
        if (*p == '\n') {
            PUT("\r\n");
            at_line_start = true;
            continue;
        }

        PUT("%c", *p);
        at_line_start = false;
    }

    /* The last line has to be ended before the dot that follows it, or the dot
     * lands on the end of somebody's sentence and is not a dot on its own. */
    if (!at_line_start) {
        PUT("\r\n");
    }

    #undef PUT
    return at;
}

