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
 * Read and write, with the same shape as read(2) and write(2): the number of
 * bytes moved, 0 for a closed connection, negative for an error.
 *
 * Retries are handled inside. mbedTLS returns WANT_READ and WANT_WRITE for a
 * renegotiation that has nothing to do with the caller, and a caller that
 * treated those as failure would drop connections for reasons it could not
 * have explained.
 */
int recon_tls_read(struct recon_tls_conn *conn, void *out, size_t size);
int recon_tls_write(struct recon_tls_conn *conn, const void *data, size_t size);

/* Say goodbye properly and close. A TLS connection that just vanishes is
 * indistinguishable from one that was cut, which is worth telling apart. */
void recon_tls_close(struct recon_tls_conn *conn);

/* The descriptor underneath, for the event loop to watch. */
int recon_tls_fd(struct recon_tls_conn *conn);

const char *recon_tls_last_error(void);

#endif /* RECON_TLS_H */
