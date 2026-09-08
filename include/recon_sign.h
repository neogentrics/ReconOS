/*
 * Signatures: who made this, and has it changed since.
 *
 * Installing a package was trusting whoever handed it over. A package brings a
 * module, a module is a shared object loaded into this process, and a module
 * loaded into this process can do everything ReconOS can do -- which is
 * everything on the machine. The allow-list in recon_package bounds where a
 * package may *place a file*; it does nothing at all about what the code does
 * once it is running, and it was never meant to.
 *
 * So this is the other half: a package says who built it, and the machine
 * checks that claim against a key it already had.
 *
 * --- What this is, exactly ---
 *
 * ECDSA over NIST P-256, hashing with SHA-256, from mbedTLS. Not a signature
 * scheme written here: the rule in docs/ROADMAP.md is that vetted primitives
 * guard anything that actually protects user data, and a signature is the
 * thing standing between a person and somebody else's code running as them.
 *
 * Ed25519 would be the better choice -- smaller, faster, and with no
 * parameters to get wrong. It is not here because mbedTLS 2.x does not
 * implement EdDSA. It has Curve25519 for key agreement only, which is a
 * different operation and cannot sign. P-256 is what this library can do
 * correctly, and a correct P-256 signature is worth more than an Ed25519 one
 * this would have had to write itself.
 *
 * --- The nonce, which is the whole of ECDSA's danger ---
 *
 * ECDSA needs a per-signature secret, `k`. Reuse it under one key for two
 * messages and the private key falls out with school algebra -- this is not a
 * weakening, it is total recovery, and it is how the PlayStation 3's signing
 * key was published. A biased `k` is nearly as bad and much harder to notice.
 *
 * **So there is no random `k` here at all.** mbedTLS is built with
 * MBEDTLS_ECDSA_DETERMINISTIC, which makes `k` a function of the private key
 * and the message hash by way of HMAC-DRBG (RFC 6979). Two signatures over the
 * same message under the same key are identical, and there is no source of
 * randomness that can fail quietly and take the key with it.
 *
 * That is a property of how the library was built, so this file checks for it
 * at compile time and refuses to build without it, rather than falling back to
 * the randomised form and being weaker in a way nobody would see. It is the
 * same rule as everywhere else here: a safety check has no off switch.
 *
 * --- Who is trusted ---
 *
 * A directory of public keys, one file each, in RECON_SIGN_TRUST_DIR. A
 * signature is checked against every key in it and the first that verifies
 * names the signer. Adding one is an administrator's decision and is the whole
 * of the trust model -- there is no root, no chain and no revocation list,
 * because a machine that is not talking to a certificate authority does not
 * need any of it and every one of them is a thing to get wrong.
 *
 * Removing a key stops anything it signed from installing. It does not
 * uninstall what is already there: a package that was trusted when it went on
 * is running code that is already on the disk, and pretending otherwise would
 * be theatre.
 *
 * --- What this does not do ---
 *
 * It says a named key signed these exact bytes. It says nothing about whether
 * that key's owner is honest, and nothing about what the code does. A trusted
 * signature on malicious code installs perfectly, and that is not a flaw in
 * the signature -- it is what a signature is for. The question it answers is
 * "did this come from who it says, unchanged", and that is the question that
 * was previously unanswerable.
 */

#ifndef RECON_SIGN_H
#define RECON_SIGN_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Where the machine's trusted public keys live, and where its own key pair
 * goes.
 *
 * The private key is under /System/Config, which is not on the package
 * allow-list and never will be -- a package that could write there could
 * install its own signing key and then sign itself.
 */
#define RECON_SIGN_TRUST_DIR "/System/Keys"
#define RECON_SIGN_OWN_KEY "/System/Config/signing.key"

/* A signature as this writes it: hex, on one line. Bounded by the largest a
 * P-256 ECDSA signature encodes to in DER, which is 72 bytes. */
#define RECON_SIGN_MAX 160

/* A key's name is its file name without the extension: "recon-towers". */
#define RECON_SIGN_NAME_MAX 64

/*
 * Make a key pair and trust its public half.
 *
 * The private key is written private, the public key goes into the trust
 * directory under `name`. False if a key of that name is already there --
 * replacing one silently would invalidate everything it had signed, and
 * would do it quietly.
 */
bool recon_sign_make_key(const char *name);

/* Whether the machine has a signing key of its own. */
bool recon_sign_have_own_key(void);

/*
 * Sign `size` bytes with the machine's own key.
 *
 * `out` receives the signature as hex. False when there is no key, which is
 * not an error worth hiding -- it means this machine has not been set up to
 * sign anything.
 */
bool recon_sign_data(const void *bytes, size_t size, char *out, size_t out_size);

/*
 * Check a signature against every trusted key.
 *
 * True when one of them verifies, with `signer` set to its name. False when
 * none does, which is the same answer for "signed by somebody unknown", "the
 * bytes have changed" and "the signature is nonsense" -- because they are
 * indistinguishable from here and reporting otherwise would be a guess.
 */
bool recon_sign_verify(const void *bytes, size_t size, const char *signature,
    char *signer, size_t signer_size);

/*
 * Trust a public key from a file, under a name.
 *
 * The file must parse as a public key and must not be a private one -- a
 * private key in the trust directory would be a private key readable by
 * anything that lists it.
 */
bool recon_sign_trust(const char *path, const char *name);

/* Stop trusting one. True if it was there. */
bool recon_sign_distrust(const char *name);

/* What is trusted, for a page that lists it. */
int recon_sign_trusted_count(void);
bool recon_sign_trusted_at(int index, char *out, size_t size);

const char *recon_sign_last_error(void);

#endif /* RECON_SIGN_H */
