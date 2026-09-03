/*
 * What happens before the desktop: first-run setup, and signing in.
 *
 * A system with no accounts has never been set up, so it asks who is using it.
 * A system with accounts and nobody signed in asks which of them you are.
 * Only after that is there a desktop.
 *
 * Kept apart from the shell because it is a different thing: the shell is what
 * you use once you are in, and this is the gate. It draws over everything,
 * takes every click and key while it is up, and hands control over exactly
 * once.
 */

#ifndef RECON_SESSION_H
#define RECON_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#include <xkbcommon/xkbcommon.h>

struct recon_server;
struct recon_font;
struct recon_session;

struct recon_session *recon_session_create(struct recon_server *server,
    struct recon_font *font, int width, int height);
void recon_session_destroy(struct recon_session *session);

/*
 * Decide what to show. Setup when there are no accounts, the login screen when
 * there are. Call once the rest of the system is up.
 */
void recon_session_begin(struct recon_session *session);

/* Back to the login screen, for signing out. The account is logged out. */
void recon_session_lock(struct recon_session *session);

/*
 * The login screen over a session that is still running.
 *
 * The account stays signed in and its windows stay open; signing back in as
 * the same person returns to the same desktop. Signing in as somebody else
 * from here is a switch, and the shell clears the previous desktop then.
 */
void recon_session_lock_screen(struct recon_session *session);

/* True while setup or login is showing, which is when it owns the screen. */
bool recon_session_active(struct recon_session *session);

/*
 * True exactly once after somebody signs in, and false afterwards.
 *
 * The shell asks this after handling input, so it can do the work that only
 * makes sense for a particular account -- read their desktop, put on their
 * skin -- without the session having to know any of that exists.
 */
bool recon_session_take_signed_in(struct recon_session *session);

void recon_session_resize(struct recon_session *session, int width, int height);
void recon_session_refresh(struct recon_session *session);

bool recon_session_handle_click(struct recon_session *session,
    double lx, double ly, bool pressed);
bool recon_session_handle_motion(struct recon_session *session,
    double lx, double ly);
bool recon_session_handle_key(struct recon_session *session,
    xkb_keysym_t sym, uint32_t modifiers);

/* What is on screen, for diagnosis from the control socket. */
void recon_session_describe(struct recon_session *session, char *out, size_t size);

#endif
