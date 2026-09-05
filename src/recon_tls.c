/*
 * Encryption for the network port. See include/recon_tls.h.
 *
 * mbedTLS is kept behind this file on purpose. Nothing else in ReconOS
 * includes an mbedtls header, so the day this is replaced -- by a different
 * library, or eventually by something written here -- the change is one file
 * rather than a search through the whole tree for a type name.
 *
 * Written against mbedTLS 2.28, which is the long-term-support branch. 3.x
 * moved several of these calls; if this stops compiling after an upgrade that
 * is the first thing to check.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>

#include "ReconOS.h"
#include "recon_crypt.h"
#include "recon_fs.h"
#include "recon_tls.h"

/*
 * 2048 bits, and RSA rather than an elliptic curve.
 *
 * Not because it is better -- it is not, and it is slower to generate. It is
 * because this certificate is pinned by fingerprint by whatever connects, and
 * the widest possible set of clients being able to complete a handshake at
 * all matters more here than the key being fashionable. Generating one takes
 * a second or two on this machine, once, ever.
 */
#define TLS_KEY_BITS 2048

/* Ten years. A self-signed certificate that is pinned by fingerprint gains
 * nothing from expiring: the client is checking that it is the same machine,
 * not that somebody's vouching is still current. A short life would only
 * break working setups on a date nobody wrote down. */
#define TLS_VALID_FROM "20250101000000"
#define TLS_VALID_TO   "20350101000000"

/* What the certificate calls itself. Nothing verifies this -- the fingerprint
 * is the identity -- but a client that prints the subject should print
 * something that says where it is. */
#define TLS_SUBJECT "CN=ReconOS,O=Recon Towers OS"

/* Mixed into the random number generator alongside the entropy source, so two
 * machines that start from an identical image do not produce identical keys.
 * Belt and braces: the entropy source should already prevent that. */
#define TLS_SEED "ReconOS remote access"

struct recon_tls_conn {
    mbedtls_ssl_context ssl;
    int fd;
    /* Kept for the message a failed verification prints. mbedTLS holds the
     * name too, but not anywhere worth reaching into. Empty when listening. */
    char hostname[256];
};

static struct {
    bool ready;

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt cert;
    mbedtls_pk_context key;
    mbedtls_ssl_config config;
} g_tls;

static char g_error[256];

const char *recon_tls_last_error(void) {
    return g_error;
}

/* mbedTLS returns negative codes with a lookup table of its own. Turning one
 * into a sentence is the difference between a message somebody can act on and
 * "remote access failed (-0x2700)". */
static void fail(int code, const char *doing) {
    char detail[128];
    mbedtls_strerror(code, detail, sizeof(detail));
    snprintf(g_error, sizeof(g_error), "%s: %s", doing, detail);
}

bool recon_tls_ready(void) {
    return g_tls.ready;
}

/* --- Making a certificate --- */

/*
 * Write a fresh key and a self-signed certificate over it.
 *
 * Both land in /System/Config as PEM. PEM rather than DER because these are
 * files somebody may want to look at, copy to a client, or replace with one
 * of their own, and a text format is the one that lets them.
 */
static bool generate(void) {
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    mbedtls_mpi serial;

    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);
    mbedtls_mpi_init(&serial);

    bool ok = false;
    int rc;

    rc = mbedtls_pk_setup(&key,
        mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    if (rc != 0) {
        fail(rc, "setting up a key");
        goto done;
    }

    rc = mbedtls_rsa_gen_key(mbedtls_pk_rsa(key), mbedtls_ctr_drbg_random,
        &g_tls.drbg, TLS_KEY_BITS, 65537);
    if (rc != 0) {
        fail(rc, "generating a key");
        goto done;
    }

    /*
     * Serial 1. A serial number distinguishes certificates issued by the same
     * authority; there is one certificate and it is its own authority, so
     * there is nothing to distinguish it from.
     */
    rc = mbedtls_mpi_lset(&serial, 1);
    if (rc != 0) {
        fail(rc, "setting the serial number");
        goto done;
    }

    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);

    rc = mbedtls_x509write_crt_set_subject_name(&crt, TLS_SUBJECT);
    if (rc != 0) {
        fail(rc, "naming the certificate");
        goto done;
    }
    /* Issuer and subject are the same name, which is what self-signed means
     * and is exactly what a certificate authority would refuse to do. */
    rc = mbedtls_x509write_crt_set_issuer_name(&crt, TLS_SUBJECT);
    if (rc != 0) {
        fail(rc, "naming the issuer");
        goto done;
    }

    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_serial(&crt, &serial);

    rc = mbedtls_x509write_crt_set_validity(&crt, TLS_VALID_FROM,
        TLS_VALID_TO);
    if (rc != 0) {
        fail(rc, "setting the validity dates");
        goto done;
    }

    /* Says it is its own authority, which a reader should be told plainly
     * rather than left to infer from the issuer matching the subject. */
    rc = mbedtls_x509write_crt_set_basic_constraints(&crt, 1, 0);
    if (rc != 0) {
        fail(rc, "setting the constraints");
        goto done;
    }

    unsigned char pem[8192];

    rc = mbedtls_x509write_crt_pem(&crt, pem, sizeof(pem),
        mbedtls_ctr_drbg_random, &g_tls.drbg);
    if (rc != 0) {
        fail(rc, "writing the certificate");
        goto done;
    }
    if (!recon_fs_write("/", RECON_TLS_CERT_FILE, (const char *)pem,
            strlen((const char *)pem))) {
        snprintf(g_error, sizeof(g_error), "cannot write %s",
            RECON_TLS_CERT_FILE);
        goto done;
    }

    rc = mbedtls_pk_write_key_pem(&key, pem, sizeof(pem));
    if (rc != 0) {
        fail(rc, "writing the key");
        goto done;
    }
    if (!recon_fs_write("/", RECON_TLS_KEY_FILE, (const char *)pem,
            strlen((const char *)pem))) {
        snprintf(g_error, sizeof(g_error), "cannot write %s",
            RECON_TLS_KEY_FILE);
        goto done;
    }

    /*
     * The private key is readable only by the account that owns ReconOS.
     * Writing it and then tightening it leaves a window, which is worth
     * naming: the alternative is a filesystem layer that can create a file
     * with a mode, and that does not exist yet.
     */
    char host[RECON_PATH_MAX];
    char canonical[RECON_PATH_MAX];
    if (recon_fs_resolve("/", RECON_TLS_KEY_FILE, host, sizeof(host),
            canonical, sizeof(canonical))) {
        chmod(host, 0600);
    }

    /* The buffer held a private key a moment ago. */
    memset(pem, 0, sizeof(pem));
    ok = true;

done:
    mbedtls_mpi_free(&serial);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    return ok;
}

/* --- Loading --- */

/*
 * Read one of the two PEM files into a buffer mbedTLS will accept.
 *
 * mbedTLS wants the terminating NUL counted in the length for PEM, which is
 * the kind of detail that produces "invalid format" on a file that is
 * perfectly valid. The +1 below is that, not an off-by-one.
 */
static char *read_pem(const char *path, size_t *length_out) {
    size_t size = 0;
    char *text = recon_fs_read("/", path, &size);
    if (text == NULL) {
        return NULL;
    }

    char *buffer = calloc(1, size + 1);
    if (buffer == NULL) {
        free(text);
        return NULL;
    }
    memcpy(buffer, text, size);
    free(text);

    *length_out = size + 1;
    return buffer;
}

static bool load(void) {
    size_t size = 0;

    char *cert = read_pem(RECON_TLS_CERT_FILE, &size);
    if (cert == NULL) {
        snprintf(g_error, sizeof(g_error), "cannot read %s",
            RECON_TLS_CERT_FILE);
        return false;
    }

    int rc = mbedtls_x509_crt_parse(&g_tls.cert, (const unsigned char *)cert,
        size);
    free(cert);
    if (rc != 0) {
        fail(rc, "reading the certificate");
        return false;
    }

    char *key = read_pem(RECON_TLS_KEY_FILE, &size);
    if (key == NULL) {
        snprintf(g_error, sizeof(g_error), "cannot read %s",
            RECON_TLS_KEY_FILE);
        return false;
    }

    rc = mbedtls_pk_parse_key(&g_tls.key, (const unsigned char *)key, size,
        NULL, 0);
    /* It was a private key. */
    memset(key, 0, size);
    free(key);
    if (rc != 0) {
        fail(rc, "reading the key");
        return false;
    }

    return true;
}

bool recon_tls_init(char *why_out, size_t why_size) {
    if (why_out != NULL && why_size > 0) {
        why_out[0] = '\0';
    }
    if (g_tls.ready) {
        return true;
    }

    g_error[0] = '\0';

    mbedtls_entropy_init(&g_tls.entropy);
    mbedtls_ctr_drbg_init(&g_tls.drbg);
    mbedtls_x509_crt_init(&g_tls.cert);
    mbedtls_pk_init(&g_tls.key);
    mbedtls_ssl_config_init(&g_tls.config);

    int rc = mbedtls_ctr_drbg_seed(&g_tls.drbg, mbedtls_entropy_func,
        &g_tls.entropy, (const unsigned char *)TLS_SEED, strlen(TLS_SEED));
    if (rc != 0) {
        fail(rc, "seeding the random number generator");
        goto failed;
    }

    /* Made on first use rather than at install time, so a machine that never
     * turns remote access on never has a private key to lose. */
    if (!recon_fs_exists("/", RECON_TLS_CERT_FILE) ||
            !recon_fs_exists("/", RECON_TLS_KEY_FILE)) {
        if (!generate()) {
            goto failed;
        }
    }

    if (!load()) {
        goto failed;
    }

    rc = mbedtls_ssl_config_defaults(&g_tls.config, MBEDTLS_SSL_IS_SERVER,
        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        fail(rc, "setting up the server");
        goto failed;
    }

    /*
     * TLS 1.2 at the lowest. The versions below it are broken in ways with
     * names, and nothing that will ever connect to this needs them.
     */
    mbedtls_ssl_conf_min_version(&g_tls.config, MBEDTLS_SSL_MAJOR_VERSION_3,
        MBEDTLS_SSL_MINOR_VERSION_3);

    /*
     * The client is not asked for a certificate.
     *
     * It proves itself with the shared key, over the encrypted channel, which
     * is the mechanism that already exists and that a person can actually
     * use. Asking for a client certificate would mean issuing one per client,
     * which is a certificate authority, which is the thing this deliberately
     * does not have.
     */
    mbedtls_ssl_conf_authmode(&g_tls.config, MBEDTLS_SSL_VERIFY_NONE);

    mbedtls_ssl_conf_rng(&g_tls.config, mbedtls_ctr_drbg_random, &g_tls.drbg);

    rc = mbedtls_ssl_conf_own_cert(&g_tls.config, &g_tls.cert, &g_tls.key);
    if (rc != 0) {
        fail(rc, "installing the certificate");
        goto failed;
    }

    g_tls.ready = true;
    return true;

failed:
    if (why_out != NULL && why_size > 0) {
        snprintf(why_out, why_size, "%s", g_error);
    }
    recon_tls_finish();
    return false;
}

void recon_tls_finish(void) {
    mbedtls_ssl_config_free(&g_tls.config);
    mbedtls_pk_free(&g_tls.key);
    mbedtls_x509_crt_free(&g_tls.cert);
    mbedtls_ctr_drbg_free(&g_tls.drbg);
    mbedtls_entropy_free(&g_tls.entropy);
    g_tls.ready = false;
}

/* --- The fingerprint --- */

bool recon_tls_fingerprint(char *out, size_t size) {
    if (out == NULL || size == 0) {
        return false;
    }
    out[0] = '\0';

    if (!g_tls.ready || g_tls.cert.raw.p == NULL) {
        return false;
    }

    /*
     * Over the DER, using ReconOS's own SHA-256 rather than mbedTLS's.
     *
     * Not to avoid the dependency -- it is already linked -- but because this
     * is the number a person reads out over the phone to somebody at the
     * other machine, and it is worth it being computed by the code this
     * project tests against published vectors.
     */
    uint8_t digest[RECON_SHA256_SIZE];
    recon_sha256(g_tls.cert.raw.p, g_tls.cert.raw.len, digest);

    /* Colon-separated uppercase hex, which is what every other tool prints
     * and therefore what somebody comparing two of them expects. */
    size_t used = 0;
    for (int i = 0; i < RECON_SHA256_SIZE; i++) {
        int n = snprintf(out + used, size - used, "%s%02X",
            i > 0 ? ":" : "", digest[i]);
        if (n < 0 || (size_t)n >= size - used) {
            out[0] = '\0';
            return false;
        }
        used += (size_t)n;
    }
    return true;
}

/* --- Connections --- */

struct recon_tls_conn *recon_tls_accept(int fd) {
    if (!g_tls.ready || fd < 0) {
        snprintf(g_error, sizeof(g_error), "no certificate is loaded");
        return NULL;
    }

    struct recon_tls_conn *conn = calloc(1, sizeof(*conn));
    if (conn == NULL) {
        snprintf(g_error, sizeof(g_error), "out of memory");
        return NULL;
    }

    conn->fd = fd;
    mbedtls_ssl_init(&conn->ssl);

    int rc = mbedtls_ssl_setup(&conn->ssl, &g_tls.config);
    if (rc != 0) {
        fail(rc, "starting the connection");
        mbedtls_ssl_free(&conn->ssl);
        free(conn);
        return NULL;
    }

    /*
     * mbedTLS's own socket callbacks, given the descriptor directly.
     *
     * mbedtls_net_context is a struct holding an int and nothing else, so
     * passing the address of the field is exactly what the library wants.
     * This is the one place the library's I/O layer is used at all; the
     * listening and accepting are ReconOS's, because the firewall has to be
     * asked first and mbedTLS knows nothing about that.
     */
    mbedtls_ssl_set_bio(&conn->ssl, &conn->fd, mbedtls_net_send,
        mbedtls_net_recv, NULL);

    while ((rc = mbedtls_ssl_handshake(&conn->ssl)) != 0) {
        if (rc == MBEDTLS_ERR_SSL_WANT_READ ||
                rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        fail(rc, "the handshake");
        mbedtls_ssl_free(&conn->ssl);
        free(conn);
        return NULL;
    }

    return conn;
}

/* --- Going out --- */

/*
 * The client side, set up separately from the server side and on first use.
 *
 * Separate because almost nothing is shared: a different role, a different
 * verification mode, a bundle of somebody else's roots instead of this
 * machine's own certificate. Folding the two into one config would mean a
 * field meaning one thing when connecting and another when listening, which is
 * how a verify mode ends up NONE on the wrong side of a connection.
 *
 * On first use because most machines never connect out, and reading and
 * parsing a hundred and fifty root certificates is real work to do during
 * startup for something nobody has asked for.
 */
static struct {
    bool ready;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt roots;
    mbedtls_ssl_config config;

    /* How many roots are trusted, and how many in the bundle would not
     * parse. Kept because this file has no logging in it; see below. */
    int loaded;
    int rejected;
} g_out;

/*
 * Where a host might keep its bundle of trusted roots.
 *
 * Read once, at the moment ReconOS first needs to connect out, and copied
 * inside. After that the copy is the one used and this list is not consulted
 * again -- so the day ReconOS boots on its own kernel, the bundle is already a
 * file it owns rather than a path into a system that is no longer underneath.
 *
 * The same shape as the icons: borrowed once at install, owned afterwards.
 */
static const char *const HOST_CA_BUNDLES[] = {
    "/etc/ssl/certs/ca-certificates.crt",     /* Debian, Ubuntu, Alpine */
    "/etc/pki/tls/certs/ca-bundle.crt",       /* Fedora, RHEL */
    "/etc/ssl/ca-bundle.pem",                 /* openSUSE */
    "/etc/ssl/cert.pem",                      /* BSD, macOS */
    NULL,
};

/* Put a bundle inside ReconOS if there is not one there already. */
static bool install_roots(void) {
    if (recon_fs_exists("/", RECON_TLS_CA_FILE)) {
        return true;
    }

    for (int i = 0; HOST_CA_BUNDLES[i] != NULL; i++) {
        FILE *f = fopen(HOST_CA_BUNDLES[i], "rb");
        if (f == NULL) {
            continue;
        }

        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        rewind(f);

        /* A bundle is a few hundred kilobytes. Something enormous under this
         * name is not a bundle and should not be read into memory to find
         * out. */
        if (size <= 0 || size > 4 * 1024 * 1024) {
            fclose(f);
            continue;
        }

        char *bytes = malloc((size_t)size);
        if (bytes == NULL) {
            fclose(f);
            snprintf(g_error, sizeof(g_error), "out of memory");
            return false;
        }

        size_t got = fread(bytes, 1, (size_t)size, f);
        fclose(f);

        if (got == (size_t)size &&
                recon_fs_write("/", RECON_TLS_CA_FILE, bytes, got)) {
            free(bytes);
            return true;
        }
        free(bytes);
    }

    snprintf(g_error, sizeof(g_error),
        "there is no bundle of trusted certificates on this machine, so "
        "there is no way to check who a server claims to be");
    return false;
}

static bool out_ready(void) {
    if (g_out.ready) {
        return true;
    }
    if (!install_roots()) {
        return false;
    }

    size_t size = 0;
    char *pem = recon_fs_read("/", RECON_TLS_CA_FILE, &size);
    if (pem == NULL || size == 0) {
        free(pem);
        snprintf(g_error, sizeof(g_error), "could not read %s",
            RECON_TLS_CA_FILE);
        return false;
    }

    mbedtls_entropy_init(&g_out.entropy);
    mbedtls_ctr_drbg_init(&g_out.drbg);
    mbedtls_x509_crt_init(&g_out.roots);
    mbedtls_ssl_config_init(&g_out.config);

    int rc = mbedtls_ctr_drbg_seed(&g_out.drbg, mbedtls_entropy_func,
        &g_out.entropy, (const unsigned char *)TLS_SEED, strlen(TLS_SEED));
    if (rc != 0) {
        fail(rc, "seeding the random number generator");
        goto failed;
    }

    /*
     * The length includes the terminator: mbedTLS decides PEM versus DER by
     * looking for one, and a bundle passed without it is read as DER and
     * rejected as garbage.
     */
    rc = mbedtls_x509_crt_parse(&g_out.roots, (const unsigned char *)pem,
        size + 1);
    if (rc < 0) {
        fail(rc, "reading the trusted certificates");
        goto failed;
    }
    /*
     * A positive return is the number that failed to parse, with the rest
     * loaded. Not an error: a bundle of a hundred and fifty roots picked up
     * from any real machine usually has one or two the library will not take,
     * and refusing the whole bundle over them would leave nothing trusted.
     *
     * Counted rather than logged, and readable through recon_tls_roots().
     * This file has no wlroots in it and should not gain any -- that is what
     * lets the TLS tests build and run without a compositor -- so what would
     * have been a log line is a number the Network page can show instead.
     * "Some of your roots did not load" explains a verification failure that
     * otherwise makes no sense.
     */
    g_out.rejected = rc > 0 ? rc : 0;
    for (const mbedtls_x509_crt *at = &g_out.roots; at != NULL; at = at->next) {
        g_out.loaded++;
    }

    free(pem);
    pem = NULL;

    rc = mbedtls_ssl_config_defaults(&g_out.config, MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        fail(rc, "setting up the client");
        goto failed;
    }

    mbedtls_ssl_conf_min_version(&g_out.config, MBEDTLS_SSL_MAJOR_VERSION_3,
        MBEDTLS_SSL_MINOR_VERSION_3);

    /*
     * REQUIRED, and there is no way to ask for less.
     *
     * OPTIONAL would let the handshake finish and leave the result in a flag
     * for the caller to check -- which is the arrangement where somebody
     * forgets to check it, and the connection is encrypted to whoever
     * answered rather than to who was asked for. REQUIRED fails the handshake,
     * so forgetting is not available.
     */
    mbedtls_ssl_conf_authmode(&g_out.config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&g_out.config, &g_out.roots, NULL);
    mbedtls_ssl_conf_rng(&g_out.config, mbedtls_ctr_drbg_random, &g_out.drbg);

    g_out.ready = true;
    return true;

failed:
    free(pem);
    mbedtls_ssl_config_free(&g_out.config);
    mbedtls_x509_crt_free(&g_out.roots);
    mbedtls_ctr_drbg_free(&g_out.drbg);
    mbedtls_entropy_free(&g_out.entropy);
    memset(&g_out, 0, sizeof(g_out));
    return false;
}

void recon_tls_roots(int *loaded_out, int *rejected_out) {
    if (loaded_out != NULL) {
        *loaded_out = g_out.loaded;
    }
    if (rejected_out != NULL) {
        *rejected_out = g_out.rejected;
    }
}

bool recon_tls_can_connect(char *why_out, size_t why_size) {
    if (out_ready()) {
        return true;
    }
    if (why_out != NULL && why_size > 0) {
        snprintf(why_out, why_size, "%s", g_error);
    }
    return false;
}

/*
 * Turn a failed handshake into a sentence, and say which check failed.
 *
 * "Certificate error" leaves somebody with three very different possibilities
 * and no way to tell them apart: their clock is wrong, their bundle is short a
 * root, or somebody is sitting in the middle of the connection. The last of
 * those is the reason this code exists and it deserves its own sentence.
 */
static void handshake_failed(struct recon_tls_conn *conn, int rc) {
    if (rc != MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
        fail(rc, "the handshake");
        return;
    }

    uint32_t flags = mbedtls_ssl_get_verify_result(&conn->ssl);
    const char *why = "the certificate did not check out";

    if ((flags & MBEDTLS_X509_BADCERT_CN_MISMATCH) != 0) {
        why = "the certificate is for a different name -- either the wrong "
              "address, or something is answering in its place";
    } else if ((flags & MBEDTLS_X509_BADCERT_EXPIRED) != 0) {
        why = "the certificate has expired, or this machine's clock is wrong";
    } else if ((flags & MBEDTLS_X509_BADCERT_FUTURE) != 0) {
        why = "the certificate is not valid yet, which usually means this "
              "machine's clock is behind";
    } else if ((flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) != 0) {
        why = "nothing this machine trusts has vouched for that certificate";
    } else if ((flags & MBEDTLS_X509_BADCERT_REVOKED) != 0) {
        why = "that certificate has been revoked";
    }

    snprintf(g_error, sizeof(g_error), "%.96s: %s", conn->hostname, why);
}

struct recon_tls_conn *recon_tls_client_begin(int fd, const char *hostname) {
    if (fd < 0 || hostname == NULL || *hostname == '\0') {
        snprintf(g_error, sizeof(g_error),
            "a connection needs a socket and the name to check it against");
        return NULL;
    }
    if (!out_ready()) {
        return NULL;
    }

    struct recon_tls_conn *conn = calloc(1, sizeof(*conn));
    if (conn == NULL) {
        snprintf(g_error, sizeof(g_error), "out of memory");
        return NULL;
    }

    conn->fd = fd;
    snprintf(conn->hostname, sizeof(conn->hostname), "%s", hostname);
    mbedtls_ssl_init(&conn->ssl);

    int rc = mbedtls_ssl_setup(&conn->ssl, &g_out.config);
    if (rc != 0) {
        fail(rc, "starting the connection");
        goto failed;
    }

    /*
     * The name, set once and used twice.
     *
     * mbedtls_ssl_set_hostname both sends it as SNI -- so a server holding
     * many names knows which certificate to offer -- and records it as the
     * name to check the certificate against. Skipping this call does not fail;
     * it turns the name check off, which is the quiet half of the mistake this
     * whole file exists to avoid.
     */
    rc = mbedtls_ssl_set_hostname(&conn->ssl, hostname);
    if (rc != 0) {
        fail(rc, "setting the server name");
        goto failed;
    }

    mbedtls_ssl_set_bio(&conn->ssl, &conn->fd, mbedtls_net_send,
        mbedtls_net_recv, NULL);
    return conn;

failed:
    mbedtls_ssl_free(&conn->ssl);
    free(conn);
    return NULL;
}

enum recon_tls_step recon_tls_handshake_step(struct recon_tls_conn *conn) {
    if (conn == NULL) {
        return RECON_TLS_STEP_FAILED;
    }

    int rc = mbedtls_ssl_handshake(&conn->ssl);
    if (rc == 0) {
        return RECON_TLS_STEP_DONE;
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
        return RECON_TLS_STEP_WANT_READ;
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return RECON_TLS_STEP_WANT_WRITE;
    }

    handshake_failed(conn, rc);
    return RECON_TLS_STEP_FAILED;
}

struct recon_tls_conn *recon_tls_connect(int fd, const char *hostname) {
    struct recon_tls_conn *conn = recon_tls_client_begin(fd, hostname);
    if (conn == NULL) {
        return NULL;
    }

    /*
     * Spun rather than waited on, which is correct only because this entry
     * point is for a *blocking* socket: mbedTLS returns WANT_READ when the
     * underlying read would block, and on a blocking socket it does not, so
     * these arrive only for a renegotiation and clear immediately.
     *
     * Anything on the event loop uses recon_tls_client_begin and steps the
     * handshake itself, because spinning here on a non-blocking socket would
     * be a busy loop with the desktop inside it.
     */
    for (;;) {
        switch (recon_tls_handshake_step(conn)) {
        case RECON_TLS_STEP_DONE:
            return conn;
        case RECON_TLS_STEP_WANT_READ:
        case RECON_TLS_STEP_WANT_WRITE:
            continue;
        case RECON_TLS_STEP_FAILED:
            mbedtls_ssl_free(&conn->ssl);
            free(conn);
            return NULL;
        }
    }
}

int recon_tls_read(struct recon_tls_conn *conn, void *out, size_t size) {
    if (conn == NULL) {
        return -1;
    }

    int rc = mbedtls_ssl_read(&conn->ssl, out, size);

    /*
     * WANT_READ and WANT_WRITE are reported, not spun on.
     *
     * These used to loop until the call gave a real answer, which is right on
     * a blocking socket -- there they only arrive during a renegotiation and
     * clear at once. On a *non*-blocking socket WANT_READ is the ordinary way
     * of saying "nothing has arrived", and looping on it is a busy wait with
     * the whole desktop inside it. So the answer goes back to the caller, who
     * either knows to wait for the event loop or is on a blocking socket and
     * will never see it.
     */
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return RECON_TLS_AGAIN;
    }

    /* A peer that said goodbye is a closed connection, not a fault. The
     * caller treats 0 as "gone", which is what it is. */
    if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        return 0;
    }
    return rc < 0 ? -1 : rc;
}

int recon_tls_write(struct recon_tls_conn *conn, const void *data,
        size_t size) {
    if (conn == NULL) {
        return -1;
    }

    int rc = mbedtls_ssl_write(&conn->ssl, data, size);
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return RECON_TLS_AGAIN;
    }
    return rc < 0 ? -1 : rc;
}

void recon_tls_close(struct recon_tls_conn *conn) {
    if (conn == NULL) {
        return;
    }

    /* Best effort. A connection already gone will refuse this, and there is
     * nothing to do about that and nothing worth saying. */
    mbedtls_ssl_close_notify(&conn->ssl);

    mbedtls_ssl_free(&conn->ssl);
    close(conn->fd);
    free(conn);
}

int recon_tls_fd(struct recon_tls_conn *conn) {
    return conn != NULL ? conn->fd : -1;
}
