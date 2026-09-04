/*
 * Remote access to the ReconOS command interpreter.
 *
 * The same commands the terminal window sends, from somewhere else, so a
 * running ReconOS can be worked on without a person at the screen.
 *
 * --- Two ways in, and they are not equivalent ---
 *
 * **A Unix socket**, always. It carries no authentication of its own, so it is
 * created readable and writable by its owner alone -- the filesystem is the
 * authentication, which is the right amount for something that cannot leave
 * the machine.
 *
 *     nc -U /tmp/reconos.sock
 *     socat - UNIX-CONNECT:/tmp/reconos.sock
 *
 * That socket can be carried across a network by SSH, which is the *secure*
 * way to reach ReconOS from elsewhere and needs nothing from ReconOS at all:
 *
 *     ssh -L /tmp/recon-there.sock:/tmp/reconos.sock user@machine
 *     nc -U /tmp/recon-there.sock
 *
 * SSH does the encryption and the identity, both of which it is much better at
 * than anything written here would be.
 *
 * **A network port**, off by default. When it is on, ReconOS listens on TCP
 * 7420 and asks each connection for a key before it will take a command. This
 * exists because it is what people expect a system to be able to do, and
 * because the SSH route needs an account on the host underneath -- which a
 * machine ReconOS owns will not have.
 *
 * It is honest about what it is: **the key crosses the network in the clear.**
 * There is no TLS yet. On a trusted network that is a reasonable trade; across
 * anything else it is not, and the forwarded socket above is the answer. The
 * `remote` command says so every time it is turned on, and the help says so
 * where somebody reading about it will see it.
 *
 * --- The firewall decides ---
 *
 * The network listener asks the firewall before it opens the port, and asks
 * again for each connection that arrives. Turning remote access on is not
 * enough: the rule has to allow it. That is the point of having a firewall
 * rather than a setting -- one place says what may be reached, and everything
 * that opens a port goes through it.
 */

#ifndef RECON_CONTROL_H
#define RECON_CONTROL_H

#include <stdbool.h>
#include <stddef.h>

#define RECON_CONTROL_DEFAULT_PATH "/tmp/reconos.sock"

/*
 * Where the key's hash lives, and what settings remember.
 *
 * The hash, not the key: a file holding the secret is a secret at rest, and
 * losing a key that can be regenerated in one command costs nothing. The key
 * is shown once, when it is made.
 */
#define RECON_REMOTE_KEY_FILE "/System/Config/remote.key"
#define RECON_REMOTE_ON_KEY "remote/enabled"
#define RECON_REMOTE_PORT_KEY "remote/port"

struct recon_server;
struct recon_control;

/*
 * Start listening on the Unix socket. Pass NULL for the default path, which
 * RECONOS_CONTROL_SOCKET overrides. Returns NULL on failure; ReconOS runs
 * without it.
 */
struct recon_control *recon_control_create(struct recon_server *server,
    const char *socket_path);

void recon_control_destroy(struct recon_control *control);

const char *recon_control_path(struct recon_control *control);

/* --- The network port --- */

/*
 * Open the port, if the firewall allows it and there is a key to check
 * against.
 *
 * False with `why_out` saying which of those it was. Refusing to listen
 * because the firewall says no is the firewall working, and the message says
 * which rule to turn on rather than only that something said no.
 */
bool recon_control_listen_network(struct recon_control *control, int port,
    char *why_out, size_t why_size);

void recon_control_stop_network(struct recon_control *control);

bool recon_control_network_listening(struct recon_control *control);
int recon_control_network_port(struct recon_control *control);

/* --- The key --- */

/* Whether a key has been set at all. Without one the port will not open: a
 * listener that accepts anything is not remote access, it is an invitation. */
bool recon_control_has_key(void);

/*
 * Make a new key, forget the old one, and write the new one into `out`.
 *
 * Shown once and never again, because only its hash is kept. Regenerating is
 * one command, which is the trade being made: a key that can be read back is
 * a key sitting in a file.
 */
bool recon_control_new_key(char *out, size_t size);

/* Does this match? Constant time, so how long it took says nothing about how
 * much of the guess was right. */
bool recon_control_key_matches(const char *offered);

#endif
