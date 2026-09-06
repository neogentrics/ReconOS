/*
 * Sending mail.
 *
 * Its own file rather than a second half of recon_mail.c, because receiving and
 * sending share a transport and nothing else: different protocol, different
 * settings, different failure modes, and a reader that has to hold both at once
 * to follow either.
 *
 * --- What this refuses to do ---
 *
 * It will not send a message over a connection that is not encrypted. Both
 * usual ways of getting one are here: TLS from the first byte on port 465, and
 * STARTTLS on 587. There is no third way and no setting that produces one.
 *
 * --- STARTTLS, and why it is the dangerous one ---
 *
 * It begins in the clear and asks to be upgraded, which puts three things in
 * the hands of whatever is in the middle. All three are refused here.
 *
 * A middle can delete STARTTLS from the server's list of what it supports, and
 * a client that then carries on has sent everything in the open because one
 * line was removed. So the upgrade is REQUIRED: no STARTTLS in the offer means
 * the session ends and nothing is sent. There is no setting to relax that,
 * which is the same rule the rest of this system follows about safety checks.
 *
 * A middle can answer "ready to start TLS" and put more commands in the same
 * packet. Everything read before the handshake is plaintext that arrived before
 * anything was proved, and a client that keeps it treats an attacker's
 * commands as though they came from inside the encrypted session. That is how
 * STARTTLS has been broken in real mail clients more than once, so everything
 * buffered is thrown away at the moment the upgrade starts.
 *
 * And nothing learned before the upgrade is carried across it. EHLO is sent
 * again afterwards -- which the standard requires anyway -- so the list of what
 * the server supports, including whether it takes a password at all, is one
 * heard over the encrypted connection rather than one heard from whoever was
 * speaking first.
 *
 * Nothing is written before the connection is encrypted except EHLO and
 * STARTTLS themselves. Not the username, not the password, not the letter.
 *
 * --- What it does not do yet ---
 *
 * One recipient list, plain text bodies, no attachments, no HTML, no
 * addressing beyond To and a subject. Enough to send a letter, and everything
 * absent is absent by name rather than by discovery.
 */

#ifndef RECON_SMTP_H
#define RECON_SMTP_H

#include <stdbool.h>
#include <stddef.h>

/*
 * How the connection becomes encrypted.
 *
 * Both end up in the same place and one of them is riskier to reach, which is
 * why it is a choice rather than something guessed from the port: guessing
 * would mean a typed port number silently deciding how carefully the
 * connection is made.
 */
enum recon_smtp_security {
    /* TLS from the first byte. Nothing is ever spoken in the clear. */
    RECON_SMTP_TLS,
    /* Plain, then an upgrade that is required to succeed. */
    RECON_SMTP_STARTTLS,
};

/* The usual ports. Editable in the account, because somebody's server is on
 * another one -- but always encrypted, whatever the number. */
#define RECON_SMTP_TLS_PORT 465
#define RECON_SMTP_STARTTLS_PORT 587

struct recon_smtp_account {
    char host[192];
    char user[128];
    /* The address messages are sent as, which is not always the username. */
    char from[192];
    int port;
    enum recon_smtp_security security;
};

/* Read and write the sending account. False when there is none. */
bool recon_smtp_account_get(struct recon_smtp_account *out);
bool recon_smtp_account_set(const struct recon_smtp_account *account);
bool recon_smtp_account_clear(void);

/*
 * A message, before it is a message.
 *
 * `to` is one address. `body` is plain text with whatever line endings; this
 * writes the ones the protocol wants.
 */
struct recon_smtp_letter {
    char to[192];
    char subject[256];
    const char *body;
};

/*
 * Whether a letter can be sent as written.
 *
 * Checked before anything is connected, and separately from sending, so the
 * window can say what is wrong while somebody is still typing.
 *
 * What it is really looking for is a newline in a header. A subject or an
 * address carrying a carriage return lets whoever wrote it add headers of their
 * own -- a second recipient, a different sender -- and the message that arrives
 * is not the message that was shown. That is the oldest bug in mail software
 * and it is one line to prevent, so it is prevented here rather than at the
 * point of writing the headers, where a later caller could miss it.
 *
 * `why` is filled with a sentence when the answer is false.
 */
bool recon_smtp_letter_ok(const struct recon_smtp_letter *letter,
    char *why, size_t why_size);

struct recon_smtp_session;

struct recon_smtp_handlers {
    /* Something changed that is worth showing: connecting, greeting, sending. */
    void (*progress)(void *user, const char *what);
    /* The letter is with the server. */
    void (*sent)(void *user);
    /* It is not, and this says why. The session is finished either way. */
    void (*failed)(void *user, const char *why);
};

/*
 * Send one letter, and close.
 *
 * The password is copied and erased when the session ends. It is not stored
 * anywhere, for the reasons written out at length in recon_mail.h -- the same
 * argument, the same missing keyring.
 *
 * NULL when the letter is malformed or there is no account, with
 * recon_smtp_last_error saying which.
 */
struct recon_smtp_session *recon_smtp_send(
    const struct recon_smtp_account *account, const char *password,
    const struct recon_smtp_letter *letter,
    const struct recon_smtp_handlers *handlers, void *user);

void recon_smtp_close(struct recon_smtp_session *session);

const char *recon_smtp_last_error(void);

/*
 * Write a letter out as the bytes that go after DATA.
 *
 * Exposed because it is the part with the rules in it -- header shape, the dot
 * that has to be doubled, the line endings the protocol insists on -- and those
 * are worth testing without a server to send to.
 *
 * Returns the length written, or 0 if it does not fit. `now` is the date line;
 * passing NULL leaves the header out rather than inventing a time.
 */
size_t recon_smtp_compose(const struct recon_smtp_account *account,
    const struct recon_smtp_letter *letter, const char *now,
    char *out, size_t out_size);

#endif /* RECON_SMTP_H */
