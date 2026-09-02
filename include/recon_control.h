/*
 * Remote access to the ReconOS command interpreter.
 *
 * The same commands the terminal window sends, over a Unix socket, so a
 * running ReconOS can be inspected without a person at the screen -- useful
 * for development now, and the groundwork for remote administration later.
 *
 * Connect with any tool that can talk to a Unix socket:
 *
 *     socat - UNIX-CONNECT:/tmp/reconos.sock
 *     nc -U /tmp/reconos.sock
 *
 * The socket carries no authentication, so it is only as private as its file
 * permissions. That is adequate for a development machine and is not adequate
 * for anything reachable from a network; exposing it beyond the local machine
 * needs authentication first.
 */

#ifndef RECON_CONTROL_H
#define RECON_CONTROL_H

#define RECON_CONTROL_DEFAULT_PATH "/tmp/reconos.sock"

struct recon_server;
struct recon_control;

/*
 * Start listening. Pass NULL for the default path, which
 * RECONOS_CONTROL_SOCKET overrides. Returns NULL on failure; ReconOS runs
 * without it.
 */
struct recon_control *recon_control_create(struct recon_server *server,
    const char *socket_path);

void recon_control_destroy(struct recon_control *control);

const char *recon_control_path(struct recon_control *control);

#endif
