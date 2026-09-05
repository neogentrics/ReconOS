/*
 * Reading mail. See include/recon_mail.h.
 *
 * Both protocols are line-oriented text over a stream, so the shape is the
 * same for each: a line buffer that reassembles whatever the network hands
 * over into lines, and a state machine that decides what to say next when one
 * arrives. Only the states and the words differ.
 *
 * Nothing here blocks. The stream is asynchronous and so is this: a step
 * happens when a line arrives, and between lines the desktop is doing whatever
 * it was doing.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "recon_mail.h"
#include "recon_net.h"
#include "recon_registry.h"

/*
 * How many messages a session holds headers for.
 *
 * A cap rather than a growing list, because the alternative is a mailbox with
 * forty thousand messages deciding how much memory the desktop uses. The
 * newest are the ones fetched; see fetch_range below.
 */
#define MESSAGES_MAX 200

/* A line of a mail protocol. IMAP's literals can be longer, and a line longer
 * than this is truncated rather than being allowed to run off the end of the
 * buffer -- what is lost is the tail of a header, not the connection. */
#define LINE_MAX 4096

/* A body. Bigger than a line and still bounded: a message with a photograph
 * attached is megabytes, and this reads mail rather than downloading files. */
#define BODY_MAX (256 * 1024)

/* The name the network permission and the firewall know this by. */
#define MAIL_APPLICATION "Mail"

/* Where the account lives. Keys cannot contain spaces; values can. */
#define KEY_NAME "mail/name"
#define KEY_HOST "mail/host"
#define KEY_USER "mail/user"
#define KEY_PORT "mail/port"
#define KEY_PROTOCOL "mail/protocol"

static char g_error[256];

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, args);
    va_end(args);
}

const char *recon_mail_last_error(void) {
    return g_error[0] != '\0' ? g_error : "no error";
}

/* --- The account --- */

bool recon_mail_account_get(struct recon_mail_account *out) {
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    const char *host = recon_registry_get(RECON_REG_USER, KEY_HOST, "");
    if (host == NULL || *host == '\0') {
        return false;
    }

    snprintf(out->host, sizeof(out->host), "%s", host);
    snprintf(out->user, sizeof(out->user), "%s",
        recon_registry_get(RECON_REG_USER, KEY_USER, ""));
    snprintf(out->name, sizeof(out->name), "%s",
        recon_registry_get(RECON_REG_USER, KEY_NAME, "Mail"));

    const char *protocol = recon_registry_get(RECON_REG_USER, KEY_PROTOCOL,
        "imap");
    out->protocol = (protocol != NULL && strcasecmp(protocol, "pop3") == 0)
        ? RECON_MAIL_POP3 : RECON_MAIL_IMAP;

    out->port = atoi(recon_registry_get(RECON_REG_USER, KEY_PORT, "0"));
    if (out->port <= 0 || out->port > 65535) {
        out->port = out->protocol == RECON_MAIL_POP3
            ? RECON_MAIL_POP3_PORT : RECON_MAIL_IMAP_PORT;
    }
    return true;
}

bool recon_mail_account_set(const struct recon_mail_account *account) {
    if (account == NULL || account->host[0] == '\0') {
        set_error("an account needs a server to connect to");
        return false;
    }

    char port[16];
    snprintf(port, sizeof(port), "%d", account->port);

    return recon_registry_set(RECON_REG_USER, KEY_HOST, account->host) &&
        recon_registry_set(RECON_REG_USER, KEY_USER, account->user) &&
        recon_registry_set(RECON_REG_USER, KEY_NAME,
            account->name[0] != '\0' ? account->name : "Mail") &&
        recon_registry_set(RECON_REG_USER, KEY_PORT, port) &&
        recon_registry_set(RECON_REG_USER, KEY_PROTOCOL,
            account->protocol == RECON_MAIL_POP3 ? "pop3" : "imap");
}

bool recon_mail_account_clear(void) {
    /* The host is what account_get tests, so clearing it is what makes there
     * be no account. The rest is tidied for the same reason a deleted file's
     * contents are: leaving somebody's username behind is leaving something
     * about them behind. */
    recon_registry_remove(RECON_REG_USER, KEY_USER);
    recon_registry_remove(RECON_REG_USER, KEY_NAME);
    recon_registry_remove(RECON_REG_USER, KEY_PORT);
    recon_registry_remove(RECON_REG_USER, KEY_PROTOCOL);
    return recon_registry_remove(RECON_REG_USER, KEY_HOST);
}

/* --- The session --- */

/*
 * Where a conversation has got to.
 *
 * The two protocols share this enum rather than having one each, because they
 * are the same conversation in different words: say hello, sign in, ask what
 * is there, ask for some of it, leave.
 */
enum mail_state {
    MAIL_GREETING,
    MAIL_LOGIN,          /* POP3 sends USER here, IMAP sends LOGIN */
    MAIL_PASSWORD,       /* POP3 only; IMAP sends both at once */
    MAIL_SELECT,         /* IMAP SELECT INBOX; POP3 STAT */
    MAIL_HEADERS,
    MAIL_BODY,
    MAIL_IDLE,           /* signed in, nothing outstanding */
    MAIL_DONE,
    MAIL_FAILED,
};

struct recon_mail_session {
    struct recon_mail_account account;
    char password[256];
    int limit;

    struct recon_net_stream *stream;
    struct recon_mail_handlers handlers;
    void *user;

    enum mail_state state;
    char status[128];

    /* Reassembling lines out of whatever the network hands over. */
    char line[LINE_MAX];
    size_t line_used;
    bool line_overflowed;

    /* IMAP tags its commands and matches the answer by tag. POP3 does not, and
     * leaves this at zero. */
    int tag;

    struct recon_mail_message messages[MESSAGES_MAX];
    int count;

    /* Which message the current fetch is filling in, or -1. */
    int filling;
    char *body;
    size_t body_used;

    /* POP3's STAT answer, and how far down the list the headers have got. */
    int total;
    int asking;

    /* Set while a handler runs, so a handler that closes the session does not
     * free the thing it is standing on. The same guard recon_net uses. */
    bool in_handler;
    bool close_wanted;
};

const char *recon_mail_status(struct recon_mail_session *session) {
    return session != NULL ? session->status : "";
}

int recon_mail_count(struct recon_mail_session *session) {
    return session != NULL ? session->count : 0;
}

const struct recon_mail_message *recon_mail_at(
        struct recon_mail_session *session, int index) {
    if (session == NULL || index < 0 || index >= session->count) {
        return NULL;
    }
    return &session->messages[index];
}

static void say(struct recon_mail_session *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void say(struct recon_mail_session *s, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(s->status, sizeof(s->status), fmt, args);
    va_end(args);
}

static void changed(struct recon_mail_session *s) {
    if (s->handlers.changed != NULL) {
        s->in_handler = true;
        s->handlers.changed(s->user);
        s->in_handler = false;
    }
}

static void finished(struct recon_mail_session *s, bool ok, const char *why) {
    if (s->handlers.finished != NULL) {
        s->in_handler = true;
        s->handlers.finished(s->user, ok, why);
        s->in_handler = false;
    }
}

static void fail(struct recon_mail_session *s, const char *why) {
    if (s->state == MAIL_FAILED) {
        return;
    }
    s->state = MAIL_FAILED;
    say(s, "%s", why);
    set_error("%s", why);
    finished(s, false, why);
}

/* Send one command line, with the ending both protocols require. */
static bool send_line(struct recon_mail_session *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static bool send_line(struct recon_mail_session *s, const char *fmt, ...) {
    char line[LINE_MAX];
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(line, sizeof(line) - 3, fmt, args);
    va_end(args);

    if (written < 0) {
        fail(s, "could not put the command together");
        return false;
    }
    /* CRLF, not a newline. Both protocols say so, and a server that is strict
     * about it -- several are -- answers a bare newline with a syntax error
     * that looks like a rejected password. */
    strcat(line, "\r\n");

    if (!recon_net_stream_send_text(s->stream, line)) {
        fail(s, recon_net_last_error());
        return false;
    }
    return true;
}

/* --- Reading headers out of what arrives --- */

/*
 * Copy a header's value if this line is that header.
 *
 * Case-insensitive because the specification says header names are, and
 * servers vary: "Subject", "subject" and "SUBJECT" all turn up.
 */
static bool header_is(const char *line, const char *name, char *out,
        size_t size) {
    size_t length = strlen(name);
    if (strncasecmp(line, name, length) != 0 || line[length] != ':') {
        return false;
    }

    const char *value = line + length + 1;
    while (*value == ' ' || *value == '\t') {
        value++;
    }
    snprintf(out, size, "%s", value);
    return true;
}

/* Strip a trailing CR, which is on every line of both protocols. */
static void trim(char *line) {
    size_t length = strlen(line);
    while (length > 0 && (line[length - 1] == '\r' || line[length - 1] == '\n')) {
        line[--length] = '\0';
    }
}

static struct recon_mail_message *current(struct recon_mail_session *s) {
    if (s->filling < 0 || s->filling >= s->count) {
        return NULL;
    }
    return &s->messages[s->filling];
}

/* --- POP3 --- */

static bool pop3_ok(const char *line) {
    return strncmp(line, "+OK", 3) == 0;
}

/*
 * How far down the list to ask.
 *
 * The newest messages are the highest numbers in POP3 and the highest sequence
 * numbers in IMAP, so both count backwards from the total. A mailbox with
 * forty thousand messages is a real thing, and fetching all of them to fill
 * one screen is how a mail client earns its reputation.
 */
static int fetch_from(int total, int limit) {
    int from = total - limit + 1;
    return from < 1 ? 1 : from;
}

static void pop3_line(struct recon_mail_session *s, char *line) {
    switch (s->state) {
    case MAIL_GREETING:
        if (!pop3_ok(line)) {
            fail(s, "the server refused the connection");
            return;
        }
        say(s, "Signing in as %s", s->account.user);
        s->state = MAIL_LOGIN;
        send_line(s, "USER %s", s->account.user);
        return;

    case MAIL_LOGIN:
        if (!pop3_ok(line)) {
            fail(s, "the server did not accept that username");
            return;
        }
        s->state = MAIL_PASSWORD;
        send_line(s, "PASS %s", s->password);
        return;

    case MAIL_PASSWORD:
        if (!pop3_ok(line)) {
            /*
             * The server's own words are not repeated here. They are often
             * "-ERR authentication failed" with a URL, and sometimes they
             * contain the username. What somebody needs to know is which of
             * the two things they typed was wrong.
             */
            fail(s, "the server did not accept that username and password");
            return;
        }
        say(s, "Asking what is there");
        s->state = MAIL_SELECT;
        send_line(s, "STAT");
        return;

    case MAIL_SELECT: {
        if (!pop3_ok(line)) {
            fail(s, "the server would not say what is in the mailbox");
            return;
        }
        /* "+OK 12 34567" -- how many, and how many bytes in total. */
        int total = 0;
        sscanf(line, "+OK %d", &total);
        s->total = total;

        if (total == 0) {
            say(s, "There is no mail");
            s->state = MAIL_DONE;
            finished(s, true, "");
            return;
        }

        s->asking = total;   /* newest first */
        s->state = MAIL_HEADERS;
        say(s, "Reading %d of %d", total - fetch_from(total, s->limit) + 1,
            total);

        /* TOP asks for the headers and none of the body, which is the whole
         * reason a list can be shown before anything is downloaded. */
        send_line(s, "TOP %d 0", s->asking);
        return;
    }

    case MAIL_HEADERS: {
        struct recon_mail_message *message = current(s);

        if (message == NULL) {
            /* The first line of a TOP answer: +OK, or an error for a message
             * that has been deleted since STAT. */
            if (!pop3_ok(line)) {
                /* Skip it and carry on; one unreadable message is not a
                 * reason to show none of the others. */
                s->asking--;
            } else if (s->count < MESSAGES_MAX) {
                message = &s->messages[s->count];
                memset(message, 0, sizeof(*message));
                message->number = s->asking;
                snprintf(message->subject, sizeof(message->subject),
                    "(no subject)");
                s->filling = s->count;
                s->count++;
                return;
            }
        } else if (strcmp(line, ".") == 0) {
            /* End of this message's headers. */
            s->filling = -1;
            s->asking--;
            changed(s);
        } else {
            header_is(line, "From", message->from, sizeof(message->from)) ||
            header_is(line, "Subject", message->subject,
                sizeof(message->subject)) ||
            header_is(line, "Date", message->date, sizeof(message->date));
            return;
        }

        /* On to the next, or done. */
        if (s->asking < fetch_from(s->total, s->limit) ||
                s->count >= MESSAGES_MAX) {
            say(s, "%d message%s", s->count, s->count == 1 ? "" : "s");
            s->state = MAIL_IDLE;
            finished(s, true, "");
            return;
        }
        send_line(s, "TOP %d 0", s->asking);
        return;
    }

    case MAIL_BODY: {
        struct recon_mail_message *message = current(s);
        if (message == NULL) {
            s->state = MAIL_IDLE;
            return;
        }

        if (s->body == NULL) {
            if (!pop3_ok(line)) {
                s->filling = -1;
                s->state = MAIL_IDLE;
                say(s, "That message could not be read");
                changed(s);
                return;
            }
            s->body = calloc(1, BODY_MAX);
            s->body_used = 0;
            if (s->body == NULL) {
                fail(s, "out of memory");
            }
            return;
        }

        if (strcmp(line, ".") == 0) {
            message->body = s->body;
            s->body = NULL;
            s->filling = -1;
            s->state = MAIL_IDLE;
            say(s, "%d message%s", s->count, s->count == 1 ? "" : "s");
            changed(s);
            return;
        }

        /*
         * A line that begins with a dot had one added by the server, because
         * a bare dot is how the message ends. Taking it off again is the other
         * half of that rule, and forgetting it puts a stray dot in front of
         * every line somebody quoted with one.
         */
        const char *text = (line[0] == '.') ? line + 1 : line;
        size_t length = strlen(text);
        if (s->body_used + length + 2 < BODY_MAX) {
            memcpy(s->body + s->body_used, text, length);
            s->body_used += length;
            s->body[s->body_used++] = '\n';
            s->body[s->body_used] = '\0';
        }
        return;
    }

    case MAIL_IDLE:
    case MAIL_DONE:
    case MAIL_FAILED:
        return;
    }
}

/* --- IMAP --- */

/* Whether this line is the tagged answer to command `tag`, and whether it
 * said OK. */
static bool imap_answer(const char *line, int tag, bool *ok_out) {
    char expect[16];
    snprintf(expect, sizeof(expect), "a%d ", tag);

    size_t length = strlen(expect);
    if (strncmp(line, expect, length) != 0) {
        return false;
    }
    *ok_out = strncmp(line + length, "OK", 2) == 0;
    return true;
}

static void imap_line(struct recon_mail_session *s, char *line) {
    bool ok = false;

    switch (s->state) {
    case MAIL_GREETING:
        if (strncmp(line, "* OK", 4) != 0) {
            fail(s, "the server refused the connection");
            return;
        }
        say(s, "Signing in as %s", s->account.user);
        s->state = MAIL_LOGIN;
        s->tag++;
        /*
         * The username and password in quotes. A password with a space in it
         * is otherwise two arguments, which the server reads as a wrong
         * password -- and the person retyping it carefully has no way to find
         * that out.
         */
        send_line(s, "a%d LOGIN \"%s\" \"%s\"", s->tag, s->account.user,
            s->password);
        return;

    case MAIL_LOGIN:
        if (!imap_answer(line, s->tag, &ok)) {
            return;   /* untagged chatter before the answer */
        }
        if (!ok) {
            fail(s, "the server did not accept that username and password");
            return;
        }
        say(s, "Opening the inbox");
        s->state = MAIL_SELECT;
        s->tag++;
        send_line(s, "a%d SELECT INBOX", s->tag);
        return;

    case MAIL_SELECT: {
        /* "* 42 EXISTS" says how many are in there, and arrives before the
         * tagged answer. */
        int total = 0;
        if (sscanf(line, "* %d EXISTS", &total) == 1) {
            s->total = total;
            return;
        }
        if (!imap_answer(line, s->tag, &ok)) {
            return;
        }
        if (!ok) {
            fail(s, "the server would not open the inbox");
            return;
        }
        if (s->total == 0) {
            say(s, "There is no mail");
            s->state = MAIL_DONE;
            finished(s, true, "");
            return;
        }

        s->state = MAIL_HEADERS;
        s->tag++;
        int from = fetch_from(s->total, s->limit);
        say(s, "Reading %d of %d", s->total - from + 1, s->total);

        /*
         * BODY.PEEK rather than BODY, which is the difference between reading
         * somebody's mail and marking it read. A list that silently marks
         * forty messages as seen is a list that has damaged the mailbox it was
         * only supposed to look at.
         */
        send_line(s, "a%d FETCH %d:%d (BODY.PEEK[HEADER.FIELDS "
            "(FROM SUBJECT DATE)])", s->tag, from, s->total);
        return;
    }

    case MAIL_HEADERS: {
        if (imap_answer(line, s->tag, &ok)) {
            s->filling = -1;
            if (!ok) {
                fail(s, "the server would not send the message list");
                return;
            }
            say(s, "%d message%s", s->count, s->count == 1 ? "" : "s");
            s->state = MAIL_IDLE;
            changed(s);
            finished(s, true, "");
            return;
        }

        /* "* 42 FETCH (BODY[HEADER.FIELDS ...] {123}" starts one. */
        int number = 0;
        if (sscanf(line, "* %d FETCH", &number) == 1) {
            if (s->count < MESSAGES_MAX) {
                struct recon_mail_message *message = &s->messages[s->count];
                memset(message, 0, sizeof(*message));
                message->number = number;
                snprintf(message->subject, sizeof(message->subject),
                    "(no subject)");
                s->filling = s->count;
                s->count++;
            }
            return;
        }

        struct recon_mail_message *message = current(s);
        if (message != NULL && line[0] != ')' && line[0] != '\0') {
            header_is(line, "From", message->from, sizeof(message->from)) ||
            header_is(line, "Subject", message->subject,
                sizeof(message->subject)) ||
            header_is(line, "Date", message->date, sizeof(message->date));
        }
        return;
    }

    case MAIL_BODY: {
        struct recon_mail_message *message = current(s);

        if (imap_answer(line, s->tag, &ok)) {
            if (message != NULL && s->body != NULL) {
                message->body = s->body;
                s->body = NULL;
            }
            free(s->body);
            s->body = NULL;
            s->filling = -1;
            s->state = MAIL_IDLE;
            say(s, "%d message%s", s->count, s->count == 1 ? "" : "s");
            changed(s);
            return;
        }

        if (s->body == NULL) {
            /* The line announcing the fetch; the body follows it. */
            if (strncmp(line, "* ", 2) == 0) {
                s->body = calloc(1, BODY_MAX);
                s->body_used = 0;
                if (s->body == NULL) {
                    fail(s, "out of memory");
                }
            }
            return;
        }

        size_t length = strlen(line);
        if (s->body_used + length + 2 < BODY_MAX) {
            memcpy(s->body + s->body_used, line, length);
            s->body_used += length;
            s->body[s->body_used++] = '\n';
            s->body[s->body_used] = '\0';
        }
        return;
    }

    case MAIL_PASSWORD:
    case MAIL_IDLE:
    case MAIL_DONE:
    case MAIL_FAILED:
        return;
    }
}

/* --- The stream --- */

static void on_line(struct recon_mail_session *s, char *line) {
    trim(line);
    if (s->account.protocol == RECON_MAIL_POP3) {
        pop3_line(s, line);
    } else {
        imap_line(s, line);
    }
}

static void on_received(void *user, struct recon_net_stream *stream,
        const char *bytes, size_t length) {
    struct recon_mail_session *s = user;
    (void)stream;

    for (size_t i = 0; i < length; i++) {
        char c = bytes[i];

        if (c == '\n') {
            s->line[s->line_used] = '\0';
            if (!s->line_overflowed) {
                on_line(s, s->line);
            }
            s->line_used = 0;
            s->line_overflowed = false;
            if (s->state == MAIL_FAILED || s->close_wanted) {
                return;
            }
            continue;
        }

        if (s->line_used + 1 >= sizeof(s->line)) {
            /*
             * A line longer than the buffer. The rest of it is dropped and the
             * line is not acted on, rather than being cut short and treated as
             * though it were whole -- half a header that reads as a complete
             * one is worse than a header that did not arrive.
             */
            s->line_overflowed = true;
            continue;
        }
        s->line[s->line_used++] = c;
    }
}

static void on_opened(void *user, struct recon_net_stream *stream) {
    struct recon_mail_session *s = user;
    (void)stream;
    say(s, "Connected to %s", s->account.host);
    /* Nothing is sent here. Both protocols speak first, and the greeting is
     * what moves the state machine on. */
}

static void on_closed(void *user, struct recon_net_stream *stream,
        enum recon_net_result reason) {
    struct recon_mail_session *s = user;
    (void)stream;

    s->stream = NULL;

    if (s->state == MAIL_DONE || s->state == MAIL_IDLE ||
            s->state == MAIL_FAILED) {
        return;
    }

    if (reason == RECON_NET_UNTRUSTED) {
        /*
         * Passed through rather than summarised. recon_net_last_error says
         * whether the clock is wrong, a root is missing, or something is
         * answering in the server's place, and that difference is the whole
         * reason the connection was checked.
         */
        fail(s, recon_net_last_error());
        return;
    }

    char why[256];
    snprintf(why, sizeof(why), "the connection to %s ended: %s",
        s->account.host, recon_net_result_name(reason));
    fail(s, why);
}

static const struct recon_net_stream_handlers STREAM_HANDLERS = {
    .opened = on_opened,
    .received = on_received,
    .closed = on_closed,
};

struct recon_mail_session *recon_mail_open(
        const struct recon_mail_account *account, const char *password,
        int limit, const struct recon_mail_handlers *handlers, void *user) {
    if (account == NULL || account->host[0] == '\0') {
        set_error("there is no account to connect with");
        return NULL;
    }
    if (password == NULL || *password == '\0') {
        set_error("a password is needed");
        return NULL;
    }

    struct recon_mail_session *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        set_error("out of memory");
        return NULL;
    }

    s->account = *account;
    snprintf(s->password, sizeof(s->password), "%s", password);
    s->limit = (limit > 0 && limit < MESSAGES_MAX) ? limit : MESSAGES_MAX;
    s->user = user;
    s->filling = -1;
    s->state = MAIL_GREETING;
    if (handlers != NULL) {
        s->handlers = *handlers;
    }
    say(s, "Connecting to %s", account->host);

    /*
     * Encrypted, with no plain alternative anywhere in this file. A mail
     * password in the clear is readable by everything between here and the
     * server, and a switch for it is a switch somebody eventually flicks.
     */
    s->stream = recon_net_stream_open_tls(MAIL_APPLICATION, account->host,
        account->port, &STREAM_HANDLERS, s);
    if (s->stream == NULL) {
        set_error("%s", recon_net_last_error());
        /* The password was in this memory a moment ago. */
        memset(s, 0, sizeof(*s));
        free(s);
        return NULL;
    }
    return s;
}

bool recon_mail_fetch_body(struct recon_mail_session *session, int index) {
    if (session == NULL || index < 0 || index >= session->count) {
        set_error("there is no such message");
        return false;
    }
    if (session->state != MAIL_IDLE) {
        set_error("still busy with the last request");
        return false;
    }
    if (session->messages[index].body != NULL) {
        return true;   /* already have it */
    }

    session->filling = index;
    session->state = MAIL_BODY;
    free(session->body);
    session->body = NULL;
    say(session, "Reading message %d", session->messages[index].number);

    if (session->account.protocol == RECON_MAIL_POP3) {
        return send_line(session, "RETR %d", session->messages[index].number);
    }

    session->tag++;
    /* PEEK again, for the same reason as the header fetch: opening a message
     * to read it is the user's decision to mark it seen, and this is not
     * that -- until there is a way to say so deliberately, nothing here
     * changes the mailbox. */
    return send_line(session, "a%d FETCH %d (BODY.PEEK[TEXT])",
        session->tag, session->messages[index].number);
}

void recon_mail_close(struct recon_mail_session *session) {
    if (session == NULL) {
        return;
    }
    if (session->in_handler) {
        session->close_wanted = true;
        return;
    }

    if (session->stream != NULL) {
        /* Said properly. A server told LOGOUT closes tidily; one whose client
         * vanishes holds the connection open until it times out. */
        if (session->state != MAIL_FAILED) {
            if (session->account.protocol == RECON_MAIL_POP3) {
                send_line(session, "QUIT");
            } else {
                session->tag++;
                send_line(session, "a%d LOGOUT", session->tag);
            }
        }
        recon_net_stream_close(session->stream);
    }

    for (int i = 0; i < session->count; i++) {
        free(session->messages[i].body);
    }
    free(session->body);

    /* The password lived here. Cleared rather than merely freed, because freed
     * memory keeps whatever was in it until something else uses it. */
    memset(session, 0, sizeof(*session));
    free(session);
}
