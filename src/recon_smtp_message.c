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

/* --- One field of addresses into many addresses --- */

static void say_why(char *why, size_t why_size, const char *what) {
    if (why != NULL && why_size > 0) {
        snprintf(why, why_size, "%s", what);
    }
}

/*
 * Add every address in one comma-separated field to the list.
 *
 * Surrounding space is trimmed, because somebody typing a list puts a space
 * after each comma and a field that rejected that would be a field nobody can
 * use. An empty entry -- a trailing comma, or two in a row -- is skipped
 * rather than refused, for the same reason: it is a typing artefact and not a
 * statement about who the letter is for.
 *
 * What is *not* forgiven is an address that does not look like one. That is
 * the difference between tidying up input and guessing at intent.
 */
static bool add_field(const char *field, struct recon_smtp_recipients *out,
        char *why, size_t why_size) {
    if (field == NULL || *field == '\0') {
        return true;
    }
    if (has_a_line_break(field)) {
        say_why(why, why_size,
            "An address has a line break in it, which cannot be sent.");
        return false;
    }

    const char *p = field;
    while (*p != '\0') {
        const char *comma = strchr(p, ',');
        const char *end = comma != NULL ? comma : p + strlen(p);

        const char *start = p;
        while (start < end && (*start == ' ' || *start == '\t')) {
            start++;
        }
        const char *stop = end;
        while (stop > start && (stop[-1] == ' ' || stop[-1] == '\t')) {
            stop--;
        }

        size_t length = (size_t)(stop - start);
        if (length > 0) {
            if (out->count >= RECON_SMTP_RECIPIENTS_MAX) {
                say_why(why, why_size,
                    "That is more people than one letter can be addressed to.");
                return false;
            }
            if (length >= RECON_SMTP_ADDRESS_MAX) {
                say_why(why, why_size, "One of those addresses is too long.");
                return false;
            }

            char one[RECON_SMTP_ADDRESS_MAX];
            memcpy(one, start, length);
            one[length] = '\0';

            if (!looks_like_an_address(one)) {
                char message[RECON_SMTP_ADDRESS_MAX + 64];
                snprintf(message, sizeof(message),
                    "'%s' does not look like an address.", one);
                say_why(why, why_size, message);
                return false;
            }

            memcpy(out->address[out->count], one, length + 1);
            out->count++;
        }

        if (comma == NULL) {
            break;
        }
        p = comma + 1;
    }
    return true;
}

bool recon_smtp_recipients_of(const struct recon_smtp_letter *letter,
        struct recon_smtp_recipients *out, char *why, size_t why_size) {
    if (letter == NULL || out == NULL) {
        say_why(why, why_size, "There is no message.");
        return false;
    }

    out->count = 0;

    /*
     * To, then Cc, then Bcc. The order is the one a person would read them in
     * and the server does not care -- but a stable order means a failing send
     * fails at the same address every time, which is the difference between a
     * bug that can be reproduced and one that cannot.
     */
    if (!add_field(letter->to, out, why, why_size) ||
            !add_field(letter->cc, out, why, why_size) ||
            !add_field(letter->bcc, out, why, why_size)) {
        return false;
    }

    if (out->count == 0) {
        say_why(why, why_size, "There is nobody to send it to.");
        return false;
    }
    return true;
}

bool recon_smtp_letter_ok(const struct recon_smtp_letter *letter,
        char *why, size_t why_size) {
    if (letter == NULL) {
        say_why(why, why_size, "There is no message.");
        return false;
    }

    /*
     * A letter with somebody only in Cc or only in Bcc is a real letter and is
     * sent. What is refused is a letter with nobody anywhere, which
     * recipients_of already answers -- so this asks that rather than checking
     * the To field on its own and calling an empty To an error.
     */
    struct recon_smtp_recipients everyone;
    if (!recon_smtp_recipients_of(letter, &everyone, why, why_size)) {
        return false;
    }

    if (has_a_line_break(letter->subject)) {
        say_why(why, why_size,
            "The subject has a line break in it, which cannot be sent.");
        return false;
    }
    return true;
}

/* --- Writing the message out --- */

/*
 * One address header, or nothing when the field is empty.
 *
 * The addresses are re-joined from what was parsed rather than copied through
 * as typed, so what goes out is the list this program understood. If those two
 * ever disagree, the one that reaches the server should be the one that was
 * checked.
 *
 * Nothing here knows which header it is writing beyond the name it is handed,
 * which is why there is exactly one of these and Bcc simply never calls it.
 */
static bool write_address_header(const char *field, const char *name,
        char *out, size_t out_size, size_t *at) {
    if (field == NULL || *field == '\0') {
        return true;
    }

    struct recon_smtp_letter one_field;
    memset(&one_field, 0, sizeof(one_field));
    snprintf(one_field.to, sizeof(one_field.to), "%s", field);

    struct recon_smtp_recipients people;
    if (!recon_smtp_recipients_of(&one_field, &people, NULL, 0)) {
        return false;
    }

    int written = snprintf(out + *at, out_size - *at, "%s: ", name);
    if (written < 0 || (size_t)written >= out_size - *at) {
        return false;
    }
    *at += (size_t)written;

    for (int i = 0; i < people.count; i++) {
        written = snprintf(out + *at, out_size - *at, "%s%s",
            i > 0 ? ", " : "", people.address[i]);
        if (written < 0 || (size_t)written >= out_size - *at) {
            return false;
        }
        *at += (size_t)written;
    }

    written = snprintf(out + *at, out_size - *at, "\r\n");
    if (written < 0 || (size_t)written >= out_size - *at) {
        return false;
    }
    *at += (size_t)written;
    return true;
}

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

    /*
     * To and Cc, tidied to what was actually parsed rather than echoed as
     * typed -- so a trailing comma or a double space does not travel.
     *
     * And Bcc is not here. There is no branch below that writes it, no flag
     * that turns it on, and nothing further down this function that touches
     * `letter->bcc` at all: the addresses in it reach the server as RCPT TO
     * lines in recon_smtp.c and reach the message nowhere. That is the whole
     * meaning of a blind copy, and it is a guarantee worth having as a thing
     * this function *cannot* do rather than as a thing it remembers not to.
     */
    if (!write_address_header(letter->to, "To", out, out_size, &at)) {
        out[0] = '\0';
        return 0;
    }
    if (!write_address_header(letter->cc, "Cc", out, out_size, &at)) {
        out[0] = '\0';
        return 0;
    }

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

