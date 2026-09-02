/*
 * The ReconOS command interpreter.
 *
 * ReconOS's own commands, acting on ReconOS: its filesystem, its windows, its
 * state. Not a Unix shell -- nothing here reaches the host system, and nothing
 * here runs host programs. Addressing the system directly is what separates an
 * operating system from a desktop, and this is where that happens.
 *
 * The interpreter is deliberately separate from any window. It takes a line of
 * text and returns text, which means the terminal window is only one way to
 * reach it: a socket is another, so a running ReconOS can be inspected from
 * outside without a person at the screen.
 */

#ifndef RECON_CMD_H
#define RECON_CMD_H

#include <stdbool.h>
#include <stddef.h>

#include "recon_fs.h"

struct recon_server;
struct recon_cmd_session;

/*
 * A session holds what persists between commands -- the working directory, and
 * whether the session has been asked to end. Each connection gets its own, so
 * the terminal window and a remote connection do not disturb one another.
 */
struct recon_cmd_session *recon_cmd_session_create(struct recon_server *server);
void recon_cmd_session_destroy(struct recon_cmd_session *session);

/* The working directory, for a prompt. */
const char *recon_cmd_cwd(struct recon_cmd_session *session);

/* Set when a command asks the session to end. */
bool recon_cmd_should_exit(struct recon_cmd_session *session);

/*
 * Run one command line and return its output as text.
 *
 * The returned string is owned by the session and is valid until the next
 * call. An empty result means the command produced no output, which is not an
 * error.
 */
const char *recon_cmd_run(struct recon_cmd_session *session, const char *line);

/* Completion for the leading word of a line: the names of known commands. */
int recon_cmd_names(const char *const **names_out);

#endif
