/*
 * Sending mail. See include/recon_smtp.h.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strncasecmp */

#include "ReconOS.h"
#include "recon_crypt.h"
#include "recon_net.h"
#include "recon_registry.h"
#include "recon_smtp.h"

#define SMTP_APPLICATION "Mail"

#define LINE_MAX 1024
#define BODY_MAX (256 * 1024)

/* Registry keys, under the same prefix the receiving account uses so an
 * account's two halves sit together. */
#define KEY_HOST "mail/send-host"
#define KEY_PORT "mail/send-port"
#define KEY_USER "mail/send-user"
#define KEY_FROM "mail/send-from"
#define KEY_SECURITY "mail/send-starttls"

static char g_error[256];

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, args);
    va_end(args);
}

const char *recon_smtp_last_error(void) {
    return g_error;
}

/* --- The account --- */

bool recon_smtp_account_get(struct recon_smtp_account *out) {
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    const char *host = recon_registry_get(RECON_REG_USER, KEY_HOST, "");
    if (host[0] == '\0') {
        return false;
    }

    snprintf(out->host, sizeof(out->host), "%s", host);
    snprintf(out->user, sizeof(out->user), "%s",
        recon_registry_get(RECON_REG_USER, KEY_USER, ""));
    snprintf(out->from, sizeof(out->from), "%s",
        recon_registry_get(RECON_REG_USER, KEY_FROM, ""));
    out->port = recon_registry_get_int(RECON_REG_USER, KEY_PORT,
        RECON_SMTP_TLS_PORT);
    out->security = recon_registry_get_bool(RECON_REG_USER, KEY_SECURITY,
        false) ? RECON_SMTP_STARTTLS : RECON_SMTP_TLS;
    return true;
}

bool recon_smtp_account_set(const struct recon_smtp_account *account) {
    if (account == NULL || account->host[0] == '\0') {
        set_error("a server is needed");
        return false;
    }

    recon_registry_set(RECON_REG_USER, KEY_HOST, account->host);
    recon_registry_set(RECON_REG_USER, KEY_USER, account->user);
    recon_registry_set(RECON_REG_USER, KEY_FROM, account->from);
    recon_registry_set_int(RECON_REG_USER, KEY_PORT,
        account->port > 0 ? account->port : RECON_SMTP_TLS_PORT);
    recon_registry_set_bool(RECON_REG_USER, KEY_SECURITY,
        account->security == RECON_SMTP_STARTTLS);
    return true;
}

bool recon_smtp_account_clear(void) {
    recon_registry_remove(RECON_REG_USER, KEY_HOST);
    recon_registry_remove(RECON_REG_USER, KEY_PORT);
    recon_registry_remove(RECON_REG_USER, KEY_USER);
    recon_registry_remove(RECON_REG_USER, KEY_FROM);
    recon_registry_remove(RECON_REG_USER, KEY_SECURITY);
    return true;
}

/* --- The conversation --- */

enum smtp_state {
    SMTP_GREETING,
    SMTP_EHLO,
    /* Sent STARTTLS, waiting for the server to agree before upgrading. */
    SMTP_STARTTLS,
    /* Upgraded, and saying hello again over the encrypted connection. */
    SMTP_EHLO_AGAIN,
    SMTP_AUTH,
    SMTP_AUTH_USER,
    SMTP_AUTH_PASSWORD,
    SMTP_FROM,
    SMTP_RCPT,
    SMTP_DATA,
    SMTP_BODY,
    SMTP_QUIT,
    SMTP_DONE,
};

struct recon_smtp_session {
    struct recon_smtp_account account;
    char password[256];
    char *message;

    /*
     * No copy of the letter is kept.
     *
     * There was one, assigned and never read. It became a trap the moment
     * attachments arrived: a letter borrows the bytes of its files from
     * whoever asked for the send, and that caller frees them as soon as this
     * function returns -- so a kept copy holds pointers that are already
     * dangling, and the next person to add a line reading from it would find
     * out the interesting way. What is needed past composing is the envelope
     * and the composed message, and both are below.
     */

    /*
     * Everybody the letter goes to, and how far through them this is.
     *
     * Worked out once, before anything connects, rather than re-split at each
     * RCPT: the list a server is given has to be the list that was checked,
     * and parsing the same text twice is two chances to get a different
     * answer.
     */
    struct recon_smtp_recipients recipients;
    int recipient_at;

    struct recon_net_stream *stream;
    struct recon_smtp_handlers handlers;
    void *user;

    enum smtp_state state;

    /*
     * Whether the server said it can do STARTTLS, heard from the EHLO list.
     *
     * Cleared before each EHLO, so what is remembered is what the most recent
     * one said. That matters after an upgrade: the first list was heard in the
     * clear from whoever happened to be speaking, and nothing it claimed is
     * allowed to count once the connection is encrypted.
     */
    bool offers_starttls;

    char line[LINE_MAX];
    size_t line_used;
    bool line_overflowed;

    /*
     * Stop parsing what is left of this read.
     *
     * Set the moment an upgrade begins, so bytes that arrived in the same
     * packet as the server's "ready to start TLS" are dropped instead of being
     * read as though they had come from inside the encrypted session.
     */
    bool discard_rest;

    /* Set while a handler runs, so a handler that closes the session does not
     * free the thing it is standing on. The same guard recon_net uses. */
    bool in_handler;
    bool close_wanted;
};

static void say(struct recon_smtp_session *s, const char *what) {
    if (s->handlers.progress == NULL) {
        return;
    }
    s->in_handler = true;
    s->handlers.progress(s->user, what);
    s->in_handler = false;
}

static void give_up(struct recon_smtp_session *s, const char *why) {
    if (s->state == SMTP_DONE) {
        return;
    }
    s->state = SMTP_DONE;
    if (s->handlers.failed != NULL) {
        s->in_handler = true;
        s->handlers.failed(s->user, why);
        s->in_handler = false;
    }
}

static bool send_line(struct recon_smtp_session *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static bool send_line(struct recon_smtp_session *s, const char *fmt, ...) {
    char line[LINE_MAX];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, sizeof(line) - 2, fmt, args);
    va_end(args);

    if (n < 0 || (size_t)n >= sizeof(line) - 2) {
        give_up(s, "A line to the server was too long to send.");
        return false;
    }

    line[n] = '\r';
    line[n + 1] = '\n';
    return recon_net_stream_send(s->stream, line, (size_t)n + 2);
}

/*
 * An SMTP reply, as far as this needs one.
 *
 * Three digits, then a space or a hyphen. A hyphen means more lines follow and
 * the one to act on is the last -- which is why EHLO's answer, a list of what
 * the server can do, arrives as a paragraph and only its final line means
 * "carry on".
 */
static bool reply_code(const char *line, int *code_out, bool *last_out) {
    if (strlen(line) < 4) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        if (line[i] < '0' || line[i] > '9') {
            return false;
        }
    }
    if (line[3] != ' ' && line[3] != '-') {
        return false;
    }

    *code_out = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
    *last_out = (line[3] == ' ');
    return true;
}

/* AUTH LOGIN sends the username and the password as separate base64 lines. */
static bool send_base64(struct recon_smtp_session *s, const char *text) {
    char encoded[512];
    if (recon_to_base64((const uint8_t *)text, strlen(text), encoded,
            sizeof(encoded)) == 0) {
        give_up(s, "That username or password is too long to send.");
        return false;
    }
    return send_line(s, "%s", encoded);
}

static void on_line(struct recon_smtp_session *s, char *line) {
    int code = 0;
    bool last = false;
    if (!reply_code(line, &code, &last)) {
        return;             /* not a reply; nothing here reads anything else */
    }

    /*
     * EHLO's answer is a list of what the server can do, one line each, and
     * STARTTLS is one of the things it can say. Read from the continuation
     * lines as they arrive rather than by asking afterwards, because by the
     * time the last line arrives the earlier ones are gone.
     */
    if ((s->state == SMTP_EHLO || s->state == SMTP_EHLO_AGAIN) &&
            code == 250 && strncasecmp(line + 4, "STARTTLS", 8) == 0) {
        s->offers_starttls = true;
    }

    if (!last) {
        return;             /* a continuation, and the last line is the answer */
    }

    switch (s->state) {
    case SMTP_GREETING:
        if (code != 220) {
            give_up(s, "The server did not greet us.");
            return;
        }
        say(s, "Saying hello");
        s->state = SMTP_EHLO;
        send_line(s, "EHLO reconos");
        return;

    case SMTP_EHLO:
        if (code != 250) {
            give_up(s, "The server would not accept a greeting.");
            return;
        }

        /*
         * On a connection that started plain, this is the only place an
         * upgrade can happen, and it is required.
         *
         * A server that does not offer STARTTLS ends the session. So does one
         * whose offer was removed by something in the middle -- which looks
         * exactly the same from here, and that is the point: a client that can
         * tell the difference is a client that could be talked out of
         * encrypting, and this one cannot be.
         */
        if (s->account.security == RECON_SMTP_STARTTLS) {
            if (!s->offers_starttls) {
                give_up(s, "That server did not offer to encrypt the "
                    "connection, so nothing was sent. Either it cannot, or "
                    "something between here and it removed the offer.");
                return;
            }
            say(s, "Encrypting");
            s->state = SMTP_STARTTLS;
            send_line(s, "STARTTLS");
            return;
        }

        say(s, "Signing in");
        s->state = SMTP_AUTH;
        send_line(s, "AUTH LOGIN");
        return;

    case SMTP_STARTTLS:
        if (code != 220) {
            give_up(s, "That server would not start encrypting, so nothing "
                "was sent.");
            return;
        }

        /*
         * Everything read so far is thrown away, and this is the line that
         * matters most in the file.
         *
         * A server -- or something pretending to be one -- can answer "ready
         * to start TLS" and put more commands in the same packet. Those bytes
         * arrived in the clear, before anything was proved, and treating them
         * as though they came from inside the encrypted session is how
         * STARTTLS has been broken in real mail clients. So the line buffer
         * goes, and feed() stops reading the rest of what it is holding.
         */
        s->line_used = 0;
        s->line_overflowed = false;
        s->discard_rest = true;

        if (!recon_net_stream_start_tls(s->stream, s->account.host)) {
            give_up(s, recon_net_last_error());
            return;
        }
        return;

    case SMTP_EHLO_AGAIN:
        if (code != 250) {
            give_up(s, "The server would not accept a greeting over the "
                "encrypted connection.");
            return;
        }
        say(s, "Signing in");
        s->state = SMTP_AUTH;
        send_line(s, "AUTH LOGIN");
        return;

    case SMTP_AUTH:
        if (code != 334) {
            give_up(s, "The server would not take a username and password "
                "this way.");
            return;
        }
        s->state = SMTP_AUTH_USER;
        send_base64(s, s->account.user);
        return;

    case SMTP_AUTH_USER:
        if (code != 334) {
            give_up(s, "The server did not ask for the password.");
            return;
        }
        s->state = SMTP_AUTH_PASSWORD;
        send_base64(s, s->password);
        /* Gone from here the moment it has been sent. It still exists in the
         * stream's buffer until that drains, which is not something this can
         * reach; what it can do is not keep a second copy. */
        recon_secure_erase(s->password, sizeof(s->password));
        return;

    case SMTP_AUTH_PASSWORD:
        if (code != 235) {
            give_up(s, "That username and password were not accepted.");
            return;
        }
        say(s, "Sending");
        s->state = SMTP_FROM;
        send_line(s, "MAIL FROM:<%s>", s->account.from);
        return;

    case SMTP_FROM:
        if (code != 250) {
            give_up(s, "The server would not accept the sender's address.");
            return;
        }
        s->state = SMTP_RCPT;
        s->recipient_at = 0;
        send_line(s, "RCPT TO:<%s>", s->recipients.address[0]);
        return;

    case SMTP_RCPT:
        if (code != 250 && code != 251) {
            /*
             * Named, because with several recipients "the server would not
             * accept that recipient" leaves somebody guessing which of six
             * addresses has the typo in it.
             *
             * And the whole send stops rather than skipping past. A letter
             * that reached four of five people and reported success is worse
             * than one that failed: nobody goes looking for the fifth.
             */
            char why[RECON_SMTP_ADDRESS_MAX + 64];
            snprintf(why, sizeof(why),
                "The server would not accept '%s' as a recipient.",
                s->recipients.address[s->recipient_at]);
            give_up(s, why);
            return;
        }

        s->recipient_at++;
        if (s->recipient_at < s->recipients.count) {
            /* One round trip each, which is what the protocol is: the envelope
             * is built a recipient at a time and each is accepted or refused
             * on its own. */
            send_line(s, "RCPT TO:<%s>",
                s->recipients.address[s->recipient_at]);
            return;
        }

        s->state = SMTP_DATA;
        send_line(s, "DATA");
        return;

    case SMTP_DATA:
        if (code != 354) {
            give_up(s, "The server would not take the message.");
            return;
        }
        s->state = SMTP_BODY;
        recon_net_stream_send(s->stream, s->message, strlen(s->message));
        send_line(s, ".");
        return;

    case SMTP_BODY:
        if (code != 250) {
            give_up(s, "The server did not accept the message.");
            return;
        }
        s->state = SMTP_QUIT;
        send_line(s, "QUIT");

        /*
         * Reported here rather than after QUIT is answered. The message is
         * with the server the moment it says 250 -- that is what the code
         * means -- and waiting for a polite goodbye would leave somebody
         * looking at a spinner after the thing they asked for had happened.
         */
        if (s->handlers.sent != NULL) {
            s->in_handler = true;
            s->handlers.sent(s->user);
            s->in_handler = false;
        }
        return;

    case SMTP_QUIT:
    case SMTP_DONE:
        s->state = SMTP_DONE;
        return;
    }
}

static void feed(struct recon_smtp_session *s, const char *bytes,
        size_t length) {
    for (size_t i = 0; i < length; i++) {
        /*
         * Set by the STARTTLS branch. Everything after the "ready" line in
         * this same read arrived in the clear and is dropped rather than
         * parsed -- see the note there.
         */
        if (s->discard_rest) {
            return;
        }
        char c = bytes[i];
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            if (s->line_overflowed) {
                s->line_overflowed = false;
                s->line_used = 0;
                continue;
            }
            s->line[s->line_used] = '\0';
            s->line_used = 0;
            on_line(s, s->line);
            if (s->state == SMTP_DONE) {
                return;
            }
            continue;
        }

        if (s->line_used + 1 >= sizeof(s->line)) {
            /* Dropped whole rather than in part: half a reply line parsed as a
             * reply is a reply that means something else. */
            s->line_overflowed = true;
            continue;
        }
        s->line[s->line_used++] = c;
    }
}

static void stream_opened(void *user, struct recon_net_stream *stream) {
    struct recon_smtp_session *s = user;
    (void)stream;
    say(s, "Connected");
}

static void stream_secured(void *user, struct recon_net_stream *stream) {
    struct recon_smtp_session *s = user;
    (void)stream;

    /*
     * Hello again, over the encrypted connection.
     *
     * Required by the standard and worth having anyway: everything the server
     * said the first time was said in the clear by whoever was speaking, so
     * none of it is allowed to count. The list is heard again from the party
     * whose certificate has now been checked.
     */
    s->discard_rest = false;
    s->offers_starttls = false;
    s->state = SMTP_EHLO_AGAIN;
    say(s, "Saying hello again, encrypted");
    send_line(s, "EHLO reconos");
}

static void stream_received(void *user, struct recon_net_stream *stream,
        const char *bytes, size_t length) {
    struct recon_smtp_session *s = user;
    (void)stream;
    feed(s, bytes, length);
}

static void stream_closed(void *user, struct recon_net_stream *stream,
        enum recon_net_result reason) {
    struct recon_smtp_session *s = user;
    (void)stream;
    s->stream = NULL;

    if (s->state == SMTP_QUIT || s->state == SMTP_DONE) {
        s->state = SMTP_DONE;
        return;
    }
    /* Named with the server, because "connection refused" on its own does not
     * say which of the two mail servers refused it. */
    char why[256];
    snprintf(why, sizeof(why), "%s: %s", s->account.host,
        recon_net_result_name(reason));
    give_up(s, why);
}

static const struct recon_net_stream_handlers STREAM_HANDLERS = {
    .opened = stream_opened,
    .secured = stream_secured,
    .received = stream_received,
    .closed = stream_closed,
};

struct recon_smtp_session *recon_smtp_send(
        const struct recon_smtp_account *account, const char *password,
        const struct recon_smtp_letter *letter,
        const struct recon_smtp_handlers *handlers, void *user) {
    if (account == NULL || account->host[0] == '\0') {
        set_error("there is no server to send through");
        return NULL;
    }
    if (account->from[0] == '\0') {
        set_error("there is no address to send from");
        return NULL;
    }
    if (password == NULL || *password == '\0') {
        set_error("a password is needed");
        return NULL;
    }

    char why[192];
    if (!recon_smtp_letter_ok(letter, why, sizeof(why))) {
        set_error("%s", why);
        return NULL;
    }

    struct recon_smtp_session *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        set_error("out of memory");
        return NULL;
    }

    s->account = *account;

    /*
     * The envelope, built here and not touched again.
     *
     * letter_ok above already split the same fields to decide whether they
     * were sendable, so this cannot fail -- but it is checked rather than
     * assumed, because "the earlier call already proved it" is exactly the
     * reasoning that stops being true when somebody adds a caller.
     */
    if (!recon_smtp_recipients_of(letter, &s->recipients, why, sizeof(why))) {
        set_error("%s", why);
        free(s);
        return NULL;
    }

    snprintf(s->password, sizeof(s->password), "%s", password);
    s->user = user;
    s->state = SMTP_GREETING;
    if (handlers != NULL) {
        s->handlers = *handlers;
    }

    /*
     * The whole message written out before anything is connected.
     *
     * So a letter that will not fit, or that turns out to be malformed, fails
     * while nothing is on the wire -- rather than halfway through DATA, which
     * leaves the server holding part of a message and no way to say so.
     */
    /*
     * Sized from what is actually being sent, rather than a fixed buffer.
     *
     * It was a flat 256 KB, which was generous for a letter and nowhere near
     * enough for one with a photograph attached. Base64 costs four bytes for
     * every three, plus a line ending every 57 -- so the allowance is the
     * files, half again, plus the text and room for headers. Over-allocating a
     * little is the cheap mistake here; the expensive one is a message that
     * composes to nothing and reports "too long to send" about a file the
     * limits above already agreed to.
     */
    size_t attached = 0;
    for (int i = 0; i < letter->attachment_count; i++) {
        attached += letter->attachment[i].size;
    }

    size_t room = BODY_MAX + attached + attached / 2;
    s->message = malloc(room);
    if (s->message == NULL) {
        set_error("out of memory");
        recon_secure_erase(s, sizeof(*s));
        free(s);
        return NULL;
    }
    if (recon_smtp_compose(account, letter, NULL, s->message, room) == 0) {
        set_error("that message is too long to send");
        free(s->message);
        recon_secure_erase(s, sizeof(*s));
        free(s);
        return NULL;
    }

    say(s, "Connecting");

    /*
     * Encrypted either way, and the difference is only when.
     *
     * A plain stream here is not a plain session: the state machine sends EHLO
     * and STARTTLS and nothing else until the upgrade has finished, and ends
     * the session if it does not. There is no path through this file that
     * sends a password or a letter over an unencrypted connection.
     */
    int port = account->port > 0 ? account->port
        : (account->security == RECON_SMTP_STARTTLS
            ? RECON_SMTP_STARTTLS_PORT : RECON_SMTP_TLS_PORT);

    s->stream = account->security == RECON_SMTP_STARTTLS
        ? recon_net_stream_open(SMTP_APPLICATION, account->host, port,
            &STREAM_HANDLERS, s)
        : recon_net_stream_open_tls(SMTP_APPLICATION, account->host, port,
            &STREAM_HANDLERS, s);
    if (s->stream == NULL) {
        set_error("%s", recon_net_last_error());
        free(s->message);
        recon_secure_erase(s, sizeof(*s));
        free(s);
        return NULL;
    }
    return s;
}

void recon_smtp_close(struct recon_smtp_session *session) {
    if (session == NULL) {
        return;
    }
    if (session->in_handler) {
        session->close_wanted = true;
        return;
    }

    if (session->stream != NULL) {
        recon_net_stream_close(session->stream);
        session->stream = NULL;
    }
    free(session->message);

    /* The password was in this memory. Erased rather than memset: see
     * recon_crypt.h. */
    recon_secure_erase(session, sizeof(*session));
    free(session);
}
