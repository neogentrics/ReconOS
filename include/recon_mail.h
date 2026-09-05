/*
 * Reading mail.
 *
 * IMAP and POP3, both over TLS, both through recon_net's encrypted streams --
 * so the certificate of whatever answers is checked before a password goes
 * anywhere near it. There is no option for either protocol in the clear. Those
 * ports exist and the passwords they carry are readable by anything between
 * here and there, and offering a switch for it would mean somebody eventually
 * flicks it.
 *
 * --- Two protocols, and why both ---
 *
 * They answer different questions and it is worth knowing which one you want.
 *
 * POP3 collects. It downloads what is waiting and, classically, deletes it
 * from the server. One machine, one copy, and the mail is yours. It is a
 * hundred lines of protocol and it has not changed since 1996.
 *
 * IMAP reads. The mail stays on the server and this is a view onto it, so the
 * same account read from two machines shows the same thing. That is what
 * almost everybody wants now, and it is why every large provider offers it.
 *
 * ReconOS never deletes on the server, on either protocol. `DELE` is not sent.
 * A young mail client with a bug that deletes somebody's mail is a young mail
 * client nobody uses twice, and there is no undo on the far end.
 *
 * --- What this does not do ---
 *
 * It does not send. SMTP is a separate protocol, a separate port, and a
 * separate set of ways to lose somebody's message halfway; it is not here yet
 * and this file does not pretend otherwise.
 *
 * It does not store the password. See below -- that decision is the one thing
 * in this header worth reading twice.
 */

#ifndef RECON_MAIL_H
#define RECON_MAIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- The account --- */

enum recon_mail_protocol {
    RECON_MAIL_IMAP,
    RECON_MAIL_POP3,
};

/* The usual encrypted ports. Offered as defaults so nobody has to look them
 * up, and editable because somebody's server is on neither. */
#define RECON_MAIL_IMAP_PORT 993
#define RECON_MAIL_POP3_PORT 995

struct recon_mail_account {
    char name[64];        /* what to call it in the window */
    char host[192];
    char user[128];
    int port;
    enum recon_mail_protocol protocol;
};

/*
 * --- The password ---
 *
 * It is not stored. It is asked for when a connection is made and kept in
 * memory until the window closes, and then it is gone.
 *
 * That is a real inconvenience and it is the honest position for now. The
 * alternatives were considered and each is worse:
 *
 *   In the registry as text.  Then anything that can read a file can read
 *                             somebody's mail password, and several things
 *                             can. This is what "saved passwords" means in a
 *                             system with no keyring, and it is why the phrase
 *                             should make people nervous.
 *
 *   Obfuscated.               Worse than plain, because it looks like
 *                             protection. Anybody who can read the file can
 *                             read the code that unscrambles it.
 *
 *   Encrypted properly.       Right, and it needs a key that exists only while
 *                             somebody is signed in -- a keyring, derived from
 *                             the account password at sign-in and held in
 *                             memory. That is a subsystem, not a field, and
 *                             building it badly would be the worst of the
 *                             three. It is written down as work to do.
 *
 * So: asked each time, held nowhere. When there is a keyring, this is the
 * function whose contract changes and nothing else.
 */

/* Read and write the account. False when there is none configured yet. */
bool recon_mail_account_get(struct recon_mail_account *out);
bool recon_mail_account_set(const struct recon_mail_account *account);
bool recon_mail_account_clear(void);

/* --- A message, as far as this reads one --- */

/* Headers are shown in a list; a subject longer than this is truncated for
 * the list and is not truncated in the message itself. */
#define RECON_MAIL_SUBJECT_MAX 256
#define RECON_MAIL_FROM_MAX 192
#define RECON_MAIL_DATE_MAX 64

struct recon_mail_message {
    /* The server's own number for it, which is what a fetch asks by. */
    int number;
    char from[RECON_MAIL_FROM_MAX];
    char subject[RECON_MAIL_SUBJECT_MAX];
    char date[RECON_MAIL_DATE_MAX];
    size_t size;
    /* NULL until the body has been fetched. Owned here. */
    char *body;
};

/* --- A session --- */

struct recon_mail_session;

/*
 * What a session tells its owner. All optional.
 *
 * `changed` fires whenever the list or a message has been added to, so a
 * window can redraw without polling. `finished` fires once when the session
 * has nothing left to do, successfully or not; `error` is empty when it
 * succeeded.
 */
struct recon_mail_handlers {
    void (*changed)(void *user);
    void (*finished)(void *user, bool ok, const char *error);
};

/*
 * Connect, sign in, and fetch the headers of the newest messages.
 *
 * Returns immediately: nothing has happened when this returns, and the
 * handlers say what did. NULL when it could not be started at all, with
 * recon_mail_last_error saying why.
 *
 * `limit` caps how many message headers are fetched, newest first. A mailbox
 * with forty thousand messages in it is a real thing and fetching all of them
 * to show the first screenful is how a mail client earns a reputation.
 */
struct recon_mail_session *recon_mail_open(
    const struct recon_mail_account *account, const char *password, int limit,
    const struct recon_mail_handlers *handlers, void *user);

/* Fetch one message's body. The `changed` handler fires when it arrives. */
bool recon_mail_fetch_body(struct recon_mail_session *session, int index);

void recon_mail_close(struct recon_mail_session *session);

/* What has arrived so far. The pointer is the session's and is valid until
 * the session is closed. */
int recon_mail_count(struct recon_mail_session *session);
const struct recon_mail_message *recon_mail_at(
    struct recon_mail_session *session, int index);

/* What the session is doing right now, in words, for a status line. */
const char *recon_mail_status(struct recon_mail_session *session);

const char *recon_mail_last_error(void);

#endif /* RECON_MAIL_H */
