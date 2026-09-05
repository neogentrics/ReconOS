/*
 * Encryption for the network port.
 *
 * ReconOS can be reached from another machine two ways. One is an SSH
 * forwarding of the Unix socket, which needs nothing from ReconOS and is
 * still the better answer wherever there is an SSH to lean on. The other is
 * the network listener on TCP 7420 -- which exists because a machine ReconOS
 * owns will not have an SSH, or an account on a host underneath, or anything
 * else to borrow identity from.
 *
 * Until this file existed that listener asked for a key and took it in the
 * clear. Every place that offered it said so. This is what takes those
 * warnings down.
 *
 * --- What it is not ---
 *
 * Not an invention. TLS is a specification with a wire format that other
 * people's clients have to understand, so it is one of the two places where
 * `THIRD_PARTY.md`'s line applies -- "libraries may parse formats and talk to
 * hardware" -- and mbedTLS does the parsing and the primitives. Everything
 * above that, including the decision about identity below, is here.
 *
 * The intent to write ReconOS's own cryptography one day is real and is
 * recorded in recon_crypt.h. That will be a thing to study and break. It will
 * not be what stands between somebody's machine and the network.
 *
 * --- Identity, which is the part that gets done wrong ---
 *
 * There is no certificate authority and there is no expiry theatre.
 *
 * A machine that owns itself has no upstream to ask for an identity, so it
 * asserts its own: a self-signed certificate made the first time remote
 * access is turned on. Its fingerprint is shown where somebody enabling
 * remote access will see it -- Control Panel and the `remote` command -- and
 * the client pins it on first connect and shouts if it ever changes.
 *
 * That is the SSH model, and it is honest in a way a certificate chain would
 * not be here. A chain answers "did somebody vouch for this name"; nobody has
 * vouched for this machine and nobody is going to. Pinning answers "is this
 * the same machine I talked to before", which is the question actually being
 * asked.
 *
 * --- What this does not fix ---
 *
 * The port still ships closed, and the firewall still decides whether it may
 * open at all. Encryption removes one reason remote access is off by default.
 * It does not make opening a port to the world a default.
 */

#ifndef RECON_TLS_H
#define RECON_TLS_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Where the machine's own certificate and its private key live.
 *
 * Named apart from RECON_REMOTE_KEY_FILE, which holds the hash of the shared
 * secret and is a different key entirely. Two files called remote.key would
 * be a mistake waiting to be made by somebody in a hurry.
 */
#define RECON_TLS_CERT_FILE "/System/Config/remote-tls.crt"
#define RECON_TLS_KEY_FILE "/System/Config/remote-tls.key"

/* Long enough for "AA:BB:..." over a SHA-256, plus its terminator. */
#define RECON_TLS_FINGERPRINT_MAX 100

struct recon_tls_conn;

/*
 * Load the certificate, making one if there is none.
 *
 * Called when the network port is about to open rather than at startup: a
 * machine that never turns remote access on should never generate a key it
 * has no use for. Generating takes a moment, which is why it happens when
 * somebody has asked for something rather than while they are waiting for a
 * desktop.
 *
 * False with `why_out` saying what went wrong.
 */
bool recon_tls_init(char *why_out, size_t why_size);

void recon_tls_finish(void);

/* Whether a certificate is loaded and connections can be taken. */
bool recon_tls_ready(void);

/*
 * The certificate's fingerprint, as SHA-256 over its DER bytes, in the
 * usual colon-separated uppercase hex.
 *
 * This is the string a person compares. It is what makes the difference
 * between "encrypted to somebody" and "encrypted to this machine", and every
 * place that turns remote access on shows it for exactly that reason.
 */
bool recon_tls_fingerprint(char *out, size_t size);

/*
 * Take a connection that has already been accepted.
 *
 * `fd` is handed over on success and stays the caller's on failure, so a
 * refused handshake does not leak a descriptor and does not double-close one.
 * The handshake runs here, so this blocks for as long as one takes -- which
 * is why the caller sets a receive timeout on the socket first. A client that
 * connects and then says nothing must not be able to stop the desktop.
 */
struct recon_tls_conn *recon_tls_accept(int fd);

/*
 * "Nothing right now, ask again when the socket says so."
 *
 * Distinct from an error, and distinct from zero. On a blocking socket it
 * never comes back; on a non-blocking one it is the ordinary state of a
 * connection with no data waiting, and a caller that treated it as failure
 * would close a perfectly good connection every time it was quiet.
 */
#define RECON_TLS_AGAIN (-2)

/*
 * Read and write, with the same shape as read(2) and write(2): the number of
 * bytes moved, 0 for a closed connection, negative for an error --
 * RECON_TLS_AGAIN for the not-an-error above.
 */
int recon_tls_read(struct recon_tls_conn *conn, void *out, size_t size);
int recon_tls_write(struct recon_tls_conn *conn, const void *data, size_t size);

/*
 * --- Going out, which is the opposite problem ---
 *
 * Everything above is about somebody connecting *to* this machine, where the
 * identity question is "is this the same machine as last time" and pinning a
 * self-signed certificate is the honest answer.
 *
 * Connecting out is the other question, and it has the other answer. When
 * ReconOS talks to somebody's mail server, "is this really imap.example.com"
 * is exactly the question a certificate authority exists for, and somebody
 * *has* vouched for that name. So this verifies: a chain to a trusted root,
 * and the hostname checked against the certificate. A client that skips either
 * has an encrypted connection to whoever answered, which is not the same thing
 * as an encrypted connection to who it meant.
 *
 * There is no option to turn that off. A verify-off switch is a switch that
 * ends up on, and the failure it causes is silent.
 */

/*
 * Where the trusted roots live, inside ReconOS.
 *
 * A file in the ReconOS filesystem rather than a path on the host, so that
 * nothing at runtime depends on this being Linux. It is copied in on first use
 * from wherever the host keeps its bundle, which is the same shape as the
 * icons and the wallpapers: borrowed once at install, owned afterwards.
 */
#define RECON_TLS_CA_FILE "/System/Config/ca-certificates.crt"

/*
 * Wrap an already-connected socket as a client, and verify the far end.
 *
 * `hostname` is the name that was asked for, not the address it resolved to.
 * It is used twice and both matter: sent as SNI, because a server hosting many
 * names needs to know which certificate to present, and checked against the
 * certificate afterwards, because otherwise any valid certificate for any
 * name would pass.
 *
 * `fd` is handed over on success and stays the caller's on failure, the same
 * as recon_tls_accept. NULL with recon_tls_last_error() saying why -- and for
 * a verification failure it says which check failed, because "certificate
 * error" tells somebody nothing about whether their clock is wrong, their
 * bundle is missing, or they are being intercepted.
 */
struct recon_tls_conn *recon_tls_connect(int fd, const char *hostname);

/*
 * --- The same thing, driven from an event loop ---
 *
 * recon_tls_connect above runs the handshake to completion before it returns,
 * which is right for a blocking socket and wrong for everything the desktop
 * does. A handshake is several round trips; doing it inline would freeze the
 * screen for as long as the far end takes to answer, and the far end is on
 * somebody else's network.
 *
 * So the stream layer uses these instead: begin, then step whenever the socket
 * says it can make progress, and open the connection when a step says DONE.
 */
enum recon_tls_step {
    RECON_TLS_STEP_DONE,
    RECON_TLS_STEP_WANT_READ,
    RECON_TLS_STEP_WANT_WRITE,
    RECON_TLS_STEP_FAILED,
};

/*
 * Set up a client connection without starting the handshake.
 *
 * The socket may be non-blocking; this does not touch it. NULL means the
 * connection could not be set up at all, and `fd` remains the caller's.
 */
struct recon_tls_conn *recon_tls_client_begin(int fd, const char *hostname);

/*
 * Push the handshake as far as it will go right now.
 *
 * WANT_READ and WANT_WRITE say which way the socket has to become ready before
 * it is worth calling again -- watch for that and call again then, rather than
 * calling in a loop. FAILED leaves recon_tls_last_error() saying why, and the
 * connection must be closed with recon_tls_close.
 */
enum recon_tls_step recon_tls_handshake_step(struct recon_tls_conn *conn);

/*
 * Whether outgoing TLS can be used at all -- that is, whether there is a
 * bundle of trusted roots to check against.
 *
 * Asked before offering anything that would need it, so a machine with no
 * bundle says so up front instead of failing at the moment somebody tries to
 * fetch their mail.
 */
bool recon_tls_can_connect(char *why_out, size_t why_size);

/*
 * How many trusted roots are loaded, and how many in the bundle would not
 * parse. Zero for both until something has connected out.
 *
 * A number rather than a log line, because this file has no wlroots in it and
 * should keep none -- that is what lets its tests build without a compositor.
 * Worth showing somewhere: a bundle that lost a dozen roots explains a
 * verification failure that otherwise looks like an attack.
 */
void recon_tls_roots(int *loaded_out, int *rejected_out);

/* Say goodbye properly and close. A TLS connection that just vanishes is
 * indistinguishable from one that was cut, which is worth telling apart. */
void recon_tls_close(struct recon_tls_conn *conn);

/* The descriptor underneath, for the event loop to watch. */
int recon_tls_fd(struct recon_tls_conn *conn);

const char *recon_tls_last_error(void);

#endif /* RECON_TLS_H */
