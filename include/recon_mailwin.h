/*
 * The Mail window.
 *
 * A list of what is in the inbox and a pane to read one in. The protocol is
 * recon_mail.h's; this is the part somebody looks at.
 *
 * Three things it shows in turn, depending on what it has: the account form
 * when none is set up, a password prompt when there is an account and no
 * password, and the mail when it has both. They are one window rather than
 * three, because they are one task with three prerequisites.
 */

#ifndef RECON_MAILWIN_H
#define RECON_MAILWIN_H

struct recon_server;
struct recon_font;
struct recon_appwin;

struct recon_appwin *recon_mailwin_create(struct recon_server *server,
    struct recon_font *font);

#endif /* RECON_MAILWIN_H */
