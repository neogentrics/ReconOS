/*
 * The half of recon_tls that decides whether to believe a server.
 *
 * `tests/test_tls.c` covers the identity this machine *presents* -- that it is
 * made once, survives a restart, and that the private key is private. This
 * file covers the other direction: the roots it checks other people against,
 * and what it says when a check fails.
 *
 * --- why this is worth a suite of its own -------------------------------
 *
 * Verification is delegated to mbedTLS, which is right -- writing certificate
 * path validation again is how a project acquires a vulnerability it cannot
 * find. What is ours is the two things either side of it:
 *
 *   **Which roots get loaded**, including the rule that a bundle with a few
 *   unparseable entries loads anyway. Refusing the whole bundle over one bad
 *   root leaves nothing trusted, which fails *closed* and therefore looks
 *   safe -- and a machine that trusts nothing cannot fetch mail, so somebody
 *   turns something off to make it work.
 *
 *   **What a failure is called.** The header's claim is that "certificate
 *   error" tells somebody nothing about whether their clock is wrong, their
 *   bundle is short a root, or they are being intercepted -- and that the last
 *   of those is the reason this code exists, so it gets its own sentence.
 *   A sentence is a claim like any other and nothing was checking these.
 *
 * --- why it mints its own certificates ----------------------------------
 *
 * Every one of those messages is chosen from a flag mbedTLS sets, and *which*
 * flag it sets for a given kind of bad certificate is a fact about mbedTLS
 * rather than something to reason out. So each scenario is a real handshake
 * against a real certificate with the fault built into it, over a socket pair:
 * an expired one, one for the wrong name, one not valid yet, one nothing
 * vouches for. If a version of mbedTLS ever reported an expired certificate
 * differently, this suite says so rather than the browser quietly telling
 * somebody the wrong thing about their own machine.
 *
 * The certificates are elliptic-curve rather than RSA purely so the suite runs
 * in a moment: six keys at 2048 bits is several seconds of nothing.
 *
 * Run with: cmake --build build && ./build/recon_tls_verify_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "recon_fs.h"
#include "recon_tls.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
    }
}

/* Says what it got as well as that it was wrong, because a message is the
 * thing under test here and "it was not the right one" is not a report. */
static void check_says(const char *got, const char *wanted, const char *what) {
    g_checks++;
    if (got == NULL || strstr(got, wanted) == NULL) {
        g_failures++;
        printf("  FAIL: %s\n", what);
        printf("        wanted to find: %s\n", wanted);
        printf("        it said:        %s\n", got == NULL ? "(nothing)" : got);
    }
}

/* ---------------------------------------------------------------------- */
/* Minting certificates                                                    */
/* ---------------------------------------------------------------------- */

static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_drbg;

struct identity {
    char name[64];          /* the common name, kept for signing children */
    char pem[2048];         /* the certificate */
    mbedtls_pk_context key; /* kept, so this identity can sign another */
};

/*
 * A validity bound, as a certificate spells one: YYYYMMDDHHMMSS.
 *
 * Relative to now rather than written down, because a fixture with a fixed
 * expiry date is a test that starts failing on a day nobody chose.
 */
static void when(char *out, size_t size, long days_from_now) {
    /* Held to a size the compiler can see is enough: `tm_year + 1900` is an
     * int, so `%04d` can want eleven digits as far as it knows, and a release
     * build of this tree is required to report no truncation at all. */
    time_t t = time(NULL) + (time_t)days_from_now * 24 * 60 * 60;
    struct tm parts;
    gmtime_r(&t, &parts);
    snprintf(out, size, "%04d%02d%02d%02d%02d%02d",
        parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
        parts.tm_hour, parts.tm_min, parts.tm_sec);
}

/*
 * One certificate, optionally signed by another.
 *
 * `issuer` NULL means self-signed -- the certificate says it vouches for
 * itself, which is what a certificate authority would refuse to do and is
 * exactly the shape that has to come back as "nothing this machine trusts has
 * vouched for that".
 */
static bool mint(struct identity *out, const char *cn,
        const struct identity *issuer, bool is_ca,
        long valid_from_days, long valid_to_days) {
    mbedtls_x509write_cert crt;
    mbedtls_mpi serial;
    static int next_serial = 1;

    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", cn);

    mbedtls_pk_init(&out->key);
    mbedtls_x509write_crt_init(&crt);
    mbedtls_mpi_init(&serial);

    bool ok = false;
    char subject[128];
    char issuer_name[128];
    char from[32];
    char to[32];

    if (mbedtls_pk_setup(&out->key,
            mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) {
        goto done;
    }
    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(out->key),
            mbedtls_ctr_drbg_random, &g_drbg) != 0) {
        goto done;
    }

    snprintf(subject, sizeof(subject), "CN=%s", cn);
    snprintf(issuer_name, sizeof(issuer_name), "CN=%s",
        issuer != NULL ? issuer->name : cn);

    when(from, sizeof(from), valid_from_days);
    when(to, sizeof(to), valid_to_days);

    mbedtls_x509write_crt_set_subject_key(&crt, &out->key);
    mbedtls_x509write_crt_set_issuer_key(&crt,
        issuer != NULL ? (mbedtls_pk_context *)&issuer->key : &out->key);

    if (mbedtls_x509write_crt_set_subject_name(&crt, subject) != 0 ||
            mbedtls_x509write_crt_set_issuer_name(&crt, issuer_name) != 0) {
        goto done;
    }

    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);

    if (mbedtls_mpi_lset(&serial, next_serial++) != 0 ||
            mbedtls_x509write_crt_set_serial(&crt, &serial) != 0 ||
            mbedtls_x509write_crt_set_validity(&crt, from, to) != 0 ||
            mbedtls_x509write_crt_set_basic_constraints(&crt,
                is_ca ? 1 : 0, 0) != 0) {
        goto done;
    }

    /*
     * The terminator is part of the PEM as far as everything downstream is
     * concerned: mbedTLS decides PEM versus DER by looking for one, and a
     * bundle handed over without it is read as DER and rejected as garbage.
     * The same trap the loading code has a comment about.
     */
    memset(out->pem, 0, sizeof(out->pem));
    if (mbedtls_x509write_crt_pem(&crt, (unsigned char *)out->pem,
            sizeof(out->pem), mbedtls_ctr_drbg_random, &g_drbg) != 0) {
        goto done;
    }

    ok = true;

done:
    mbedtls_mpi_free(&serial);
    mbedtls_x509write_crt_free(&crt);
    if (!ok) {
        mbedtls_pk_free(&out->key);
    }
    return ok;
}

/* ---------------------------------------------------------------------- */
/* A server to fail against                                                */
/* ---------------------------------------------------------------------- */

struct server {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt chain;
    int fd;
    bool live;
};

static bool server_start(struct server *s, int fd,
        struct identity *leaf, struct identity *ca) {
    memset(s, 0, sizeof(*s));
    s->fd = fd;

    mbedtls_ssl_init(&s->ssl);
    mbedtls_ssl_config_init(&s->config);
    mbedtls_x509_crt_init(&s->chain);
    s->live = true;

    /* The leaf first, then whatever vouches for it -- the order a server
     * sends a chain in. A self-signed leaf is offered alone, which is the
     * whole of what makes it untrusted. */
    if (mbedtls_x509_crt_parse(&s->chain, (const unsigned char *)leaf->pem,
            strlen(leaf->pem) + 1) != 0) {
        return false;
    }
    if (ca != NULL && mbedtls_x509_crt_parse(&s->chain,
            (const unsigned char *)ca->pem, strlen(ca->pem) + 1) != 0) {
        return false;
    }

    if (mbedtls_ssl_config_defaults(&s->config, MBEDTLS_SSL_IS_SERVER,
            MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        return false;
    }

    /* The server does not ask the client for a certificate. Nothing in
     * ReconOS presents one, and this suite is about the other direction. */
    mbedtls_ssl_conf_authmode(&s->config, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&s->config, mbedtls_ctr_drbg_random, &g_drbg);

    if (mbedtls_ssl_conf_own_cert(&s->config, &s->chain, &leaf->key) != 0) {
        return false;
    }
    if (mbedtls_ssl_setup(&s->ssl, &s->config) != 0) {
        return false;
    }

    mbedtls_ssl_set_bio(&s->ssl, &s->fd, mbedtls_net_send, mbedtls_net_recv,
        NULL);
    return true;
}

static void server_stop(struct server *s) {
    if (!s->live) {
        return;
    }
    mbedtls_ssl_free(&s->ssl);
    mbedtls_ssl_config_free(&s->config);
    mbedtls_x509_crt_free(&s->chain);
    s->live = false;
}

/*
 * What a scenario produces: whether the client opened the connection, and what
 * it said if it did not.
 */
struct attempt {
    bool connected;
    char said[256];
};

/*
 * Run one handshake and report.
 *
 * Both sides are driven non-blocking from a single loop rather than from two
 * threads. It is the same path the browser takes -- `recon_tls_client_begin`
 * plus `recon_tls_handshake_step`, which the header says is what everything on
 * the event loop uses -- and it keeps the suite deterministic and free of a
 * threading dependency it would otherwise acquire for four tests.
 *
 * `recon_tls_connect` cannot be used here: its own comment says it spins,
 * correctly, because it is for a *blocking* socket, and these are not.
 */
static struct attempt attempt_connect(const char *asking_for,
        struct identity *leaf, struct identity *ca) {
    struct attempt result;
    memset(&result, 0, sizeof(result));

    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        snprintf(result.said, sizeof(result.said), "no socket pair");
        return result;
    }
    fcntl(pair[0], F_SETFL, O_NONBLOCK);
    fcntl(pair[1], F_SETFL, O_NONBLOCK);

    struct server s;
    if (!server_start(&s, pair[1], leaf, ca)) {
        snprintf(result.said, sizeof(result.said), "the test server failed");
        server_stop(&s);
        close(pair[0]);
        close(pair[1]);
        return result;
    }

    struct recon_tls_conn *conn = recon_tls_client_begin(pair[0], asking_for);
    if (conn == NULL) {
        snprintf(result.said, sizeof(result.said), "%s",
            recon_tls_last_error());
        server_stop(&s);
        close(pair[0]);
        close(pair[1]);
        return result;
    }

    /*
     * Bounded rather than open. A handshake over a socket pair is a handful of
     * exchanges; anything that has not finished in two hundred turns is stuck,
     * and a test that hangs is worse than a test that fails, because a hang in
     * a suite somebody runs on every commit stops being run.
     */
    for (int turn = 0; turn < 200; turn++) {
        mbedtls_ssl_handshake(&s.ssl);

        enum recon_tls_step step = recon_tls_handshake_step(conn);
        if (step == RECON_TLS_STEP_DONE) {
            result.connected = true;
            break;
        }
        if (step == RECON_TLS_STEP_FAILED) {
            snprintf(result.said, sizeof(result.said), "%s",
                recon_tls_last_error());
            break;
        }
    }

    if (!result.connected && result.said[0] == '\0') {
        snprintf(result.said, sizeof(result.said),
            "the handshake neither finished nor failed");
    }

    if (result.connected) {
        /* Hands the descriptor back and says goodbye properly. */
        recon_tls_close(conn);
    } else {
        recon_tls_close(conn);
        close(pair[0]);
    }
    server_stop(&s);
    close(pair[1]);
    return result;
}

/* ---------------------------------------------------------------------- */
/* The fixtures, minted once                                               */
/* ---------------------------------------------------------------------- */

static struct identity g_ca;          /* what the machine is told to trust */
static struct identity g_other_ca;    /* a second root, so the count is > 1 */
static struct identity g_good;        /* the right name, signed, in date */
static struct identity g_wrong_name;  /* signed, in date, for someone else */
static struct identity g_expired;
static struct identity g_not_yet;
static struct identity g_self_signed; /* the right name, vouched for by
                                       * nobody but itself */

#define THE_NAME "reconos.test"

/*
 * A PEM block that is shaped like a certificate and is not one.
 *
 * Real bundles picked up off real machines contain these -- an algorithm the
 * library was not built with, a root in a format it does not take -- and the
 * loading code's comment says so. The point of putting one here is that the
 * bundle must load anyway.
 */
static const char BAD_BLOCK[] =
    "-----BEGIN CERTIFICATE-----\n"
    "VGhpcyBpcyBub3QgYSBjZXJ0aWZpY2F0ZSwgaXQgaXMgc29tZSB0ZXh0IHRoYXQg\n"
    "aGFwcGVucyB0byBiZSB2YWxpZCBiYXNlNjQgYW5kIG5vdGhpbmcgZWxzZS4=\n"
    "-----END CERTIFICATE-----\n";

static bool mint_everything(void) {
    return mint(&g_ca, "ReconOS Test Root", NULL, true, -1, 365) &&
        mint(&g_other_ca, "ReconOS Second Root", NULL, true, -1, 365) &&
        mint(&g_good, THE_NAME, &g_ca, false, -1, 365) &&
        mint(&g_wrong_name, "somewhere-else.test", &g_ca, false, -1, 365) &&
        mint(&g_expired, THE_NAME, &g_ca, false, -800, -400) &&
        mint(&g_not_yet, THE_NAME, &g_ca, false, 100, 400) &&
        mint(&g_self_signed, THE_NAME, NULL, false, -1, 365);
}

static void free_everything(void) {
    mbedtls_pk_free(&g_ca.key);
    mbedtls_pk_free(&g_other_ca.key);
    mbedtls_pk_free(&g_good.key);
    mbedtls_pk_free(&g_wrong_name.key);
    mbedtls_pk_free(&g_expired.key);
    mbedtls_pk_free(&g_not_yet.key);
    mbedtls_pk_free(&g_self_signed.key);
}

/* ---------------------------------------------------------------------- */
/* The tests                                                               */
/* ---------------------------------------------------------------------- */

static void test_the_bundle_loads(void) {
    printf("the roots load, and a bad entry does not take the rest with it\n");

    /*
     * Written before anything connects, so `install_roots` finds a bundle
     * already inside ReconOS and takes the branch that does not go looking at
     * the host. Which is also the arrangement that makes this suite say the
     * same thing on a machine with a different set of roots.
     */
    char bundle[8192];
    snprintf(bundle, sizeof(bundle), "%s%s%s", g_ca.pem, BAD_BLOCK,
        g_other_ca.pem);

    check(recon_fs_write("/", RECON_TLS_CA_FILE, bundle, strlen(bundle)),
        "a bundle can be put where ReconOS keeps its roots");

    char why[256];
    check(recon_tls_can_connect(why, sizeof(why)),
        "and outgoing encryption is available once it is there");

    int loaded = 0;
    int rejected = 0;
    recon_tls_roots(&loaded, &rejected);

    check(loaded == 2, "both real roots are trusted");

    /*
     * **The bundle is not refused over the entry it could not read.** A
     * hundred and fifty roots off a real machine usually include one or two
     * the library will not take, and refusing all of them fails closed --
     * which looks safe and is not, because a machine that trusts nothing
     * cannot fetch mail and somebody goes looking for the switch that makes
     * it work.
     */
    check(rejected == 1, "and the one that would not parse is counted");

    if (loaded != 2 || rejected != 1) {
        printf("        loaded %d, rejected %d\n", loaded, rejected);
    }
}

static void test_a_good_certificate_connects(void) {
    printf("a certificate for the right name, signed by a trusted root\n");

    struct attempt a = attempt_connect(THE_NAME, &g_good, &g_ca);
    check(a.connected, "the connection opens");
    if (!a.connected) {
        printf("        it said: %s\n", a.said);
    }
}

static void test_the_wrong_name(void) {
    printf("a valid certificate, for somebody else\n");

    /*
     * The sharpest one. This certificate is real, in date, and signed by a
     * root this machine trusts -- everything except *whose* it is. Without the
     * name check any valid certificate for any name would pass, which is the
     * failure the whole file exists to prevent, and it leaves an encrypted
     * connection to whoever answered rather than to who was asked for.
     */
    struct attempt a = attempt_connect(THE_NAME, &g_wrong_name, &g_ca);
    check(!a.connected, "it is refused");
    check_says(a.said, "for a different name",
        "and says the certificate is for a different name");
    check_says(a.said, "answering in its place",
        "and names the possibility that matters");
}

static void test_expired(void) {
    printf("a certificate that ran out\n");

    struct attempt a = attempt_connect(THE_NAME, &g_expired, &g_ca);
    check(!a.connected, "it is refused");

    /* Both readings in one sentence, because from here they are genuinely
     * indistinguishable: the certificate may have expired, or this machine's
     * clock may be wrong, and telling somebody only the first sends them to
     * complain to a website that is fine. */
    check_says(a.said, "expired", "and says it expired");
    check_says(a.said, "clock is wrong", "or that the clock is wrong");
}

static void test_not_valid_yet(void) {
    printf("a certificate that has not started\n");

    struct attempt a = attempt_connect(THE_NAME, &g_not_yet, &g_ca);
    check(!a.connected, "it is refused");
    check_says(a.said, "not valid yet", "and says it is not valid yet");
    check_says(a.said, "clock is behind",
        "and points at the likely cause, which is this machine");
}

static void test_nothing_vouches_for_it(void) {
    printf("a certificate signed by nobody\n");

    struct attempt a = attempt_connect(THE_NAME, &g_self_signed, NULL);
    check(!a.connected, "it is refused");
    check_says(a.said, "nothing this machine trusts",
        "and says nothing trusted vouched for it");
}

static void test_the_message_names_the_host(void) {
    printf("every refusal says which host it was about\n");

    /*
     * Without it, a page that fetches from three places reports a failure
     * about none of them in particular.
     */
    struct attempt a = attempt_connect(THE_NAME, &g_self_signed, NULL);
    check_says(a.said, THE_NAME, "the name asked for is in the message");

    /*
     * And a name too long to print is cut in the right half.
     *
     * The format is `%.96s: %s` -- the hostname is bounded and the sentence is
     * not, so a name long enough to fill the buffer eats itself. **Reversed,
     * somebody would read "the certificate is for a differ" and be told
     * nothing at all**, which is the failure this whole set of messages exists
     * to avoid, arriving by a different route.
     *
     * Cutting is the right answer here and is not the fault BG-205 was about.
     * That rule -- *"a truncated path is not a shortened name for the same
     * file, it is the name of a different one"* -- is about a name something
     * is then looked up by. This one is shown to a person and looked up by
     * nothing.
     */
    char very_long[200];
    memset(very_long, 'a', sizeof(very_long) - 1);
    very_long[sizeof(very_long) - 1] = '\0';
    memcpy(very_long + 180, ".test", 6);

    struct attempt b = attempt_connect(very_long, &g_self_signed, NULL);
    check(!b.connected, "a very long name is still refused");

    const char *colon = strstr(b.said, ": ");
    check(colon != NULL, "the message still has a name and a reason");
    if (colon != NULL) {
        check((size_t)(colon - b.said) == 96,
            "the name is cut at 96 characters");
    }

    /*
     * The whole sentence, not a prefix of it -- and it is the *longest* of the
     * five, which is the one worth holding here. Asking for a name the
     * certificate does not carry is a mismatch before it is anything else, so
     * this scenario reaches the name check rather than the trust check even
     * though the certificate is also signed by nobody.
     */
    check_says(b.said, "either the wrong address, or something is answering "
        "in its place", "and the reason survives the cut entirely");
}

static void test_there_is_no_way_to_turn_it_off(void) {
    printf("verification is not optional\n");

    /*
     * The header states it: *"There is no option to turn that off. A
     * verify-off switch is a switch that ends up on, and the failure it causes
     * is silent."* What makes that true in the code is REQUIRED rather than
     * OPTIONAL -- with OPTIONAL the handshake finishes and leaves the result
     * in a flag for a caller to remember to check.
     *
     * So the property to hold is not "the flag is set" but **that a connection
     * to something untrusted does not exist at all**: no object, nothing to
     * read or write through, nothing for a forgetful caller to use.
     */
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        check(false, "a socket pair could be made");
        return;
    }
    fcntl(pair[0], F_SETFL, O_NONBLOCK);
    fcntl(pair[1], F_SETFL, O_NONBLOCK);

    struct server s;
    check(server_start(&s, pair[1], &g_self_signed, NULL),
        "a server with an untrusted certificate is listening");

    struct recon_tls_conn *conn = recon_tls_client_begin(pair[0], THE_NAME);
    check(conn != NULL, "a client is set up");

    bool ever_done = false;
    for (int turn = 0; turn < 200 && conn != NULL; turn++) {
        mbedtls_ssl_handshake(&s.ssl);
        enum recon_tls_step step = recon_tls_handshake_step(conn);
        if (step == RECON_TLS_STEP_DONE) {
            ever_done = true;
            break;
        }
        if (step == RECON_TLS_STEP_FAILED) {
            break;
        }
    }

    check(!ever_done,
        "the handshake never reports success against an untrusted server");

    recon_tls_close(conn);
    close(pair[0]);
    server_stop(&s);
    close(pair[1]);
}

static void test_bytes_go_through(void) {
    printf("an open connection carries data, both ways\n");

    /*
     * The handshake is the part that can be *wrong*; this is the part that is
     * used. Every byte the browser and the mail client send goes through
     * `recon_tls_write` and comes back through `recon_tls_read`, and nothing
     * ran either of them.
     *
     * Done here rather than through `attempt_connect` because that closes both
     * sides as soon as it has its answer, and the thing under test is what
     * happens after.
     */
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        check(false, "a socket pair could be made");
        return;
    }
    fcntl(pair[0], F_SETFL, O_NONBLOCK);
    fcntl(pair[1], F_SETFL, O_NONBLOCK);

    struct server s;
    check(server_start(&s, pair[1], &g_good, &g_ca), "a server is listening");

    struct recon_tls_conn *conn = recon_tls_client_begin(pair[0], THE_NAME);
    check(conn != NULL, "a client is set up");
    if (conn == NULL) {
        server_stop(&s);
        close(pair[0]);
        close(pair[1]);
        return;
    }

    check(recon_tls_fd(conn) == pair[0],
        "and the event loop can find the descriptor to watch");

    bool open = false;
    for (int turn = 0; turn < 200; turn++) {
        mbedtls_ssl_handshake(&s.ssl);
        if (recon_tls_handshake_step(conn) == RECON_TLS_STEP_DONE) {
            open = true;
            break;
        }
    }
    check(open, "the connection opens");

    if (open) {
        /*
         * **Nothing has arrived, and that is not a fault.**
         *
         * This is the case the read path was rewritten for: on a non-blocking
         * socket WANT_READ is the ordinary way of saying the far end has not
         * spoken yet, and the old code looped on it. AGAIN is distinct from
         * -1 precisely so a caller does not close a healthy connection.
         */
        char nothing[64];
        check(recon_tls_read(conn, nothing, sizeof(nothing)) ==
            RECON_TLS_AGAIN,
            "a read with nothing waiting says so rather than failing");

        static const char OUT[] = "from the client";
        check(recon_tls_write(conn, OUT, sizeof(OUT) - 1) ==
            (int)sizeof(OUT) - 1, "the client writes");

        char got[64];
        int n = -1;
        for (int turn = 0; turn < 200; turn++) {
            n = mbedtls_ssl_read(&s.ssl, (unsigned char *)got, sizeof(got));
            if (n != MBEDTLS_ERR_SSL_WANT_READ &&
                    n != MBEDTLS_ERR_SSL_WANT_WRITE) {
                break;
            }
        }
        check(n == (int)sizeof(OUT) - 1 && memcmp(got, OUT, (size_t)n) == 0,
            "and the server reads exactly what was sent");

        static const char BACK[] = "and from the server";
        mbedtls_ssl_write(&s.ssl, (const unsigned char *)BACK,
            sizeof(BACK) - 1);

        int m = RECON_TLS_AGAIN;
        for (int turn = 0; turn < 200; turn++) {
            m = recon_tls_read(conn, got, sizeof(got));
            if (m != RECON_TLS_AGAIN) {
                break;
            }
        }
        check(m == (int)sizeof(BACK) - 1 &&
            memcmp(got, BACK, (size_t)(m > 0 ? m : 0)) == 0,
            "and the client reads what came back");

        /*
         * A peer that says goodbye is a closed connection, not a fault -- zero
         * rather than -1, so a caller can tell "they hung up" from "something
         * broke". A TLS connection that just vanishes is the second of those
         * and is worth telling apart.
         */
        mbedtls_ssl_close_notify(&s.ssl);
        int after = RECON_TLS_AGAIN;
        for (int turn = 0; turn < 200; turn++) {
            after = recon_tls_read(conn, got, sizeof(got));
            if (after != RECON_TLS_AGAIN) {
                break;
            }
        }
        check(after == 0, "a proper goodbye reads as gone, not as a fault");
    }

    recon_tls_close(conn);
    server_stop(&s);
    close(pair[1]);
}

/*
 * Not covered here, and said out loud rather than left to be noticed:
 *
 *   **Revocation.** `handshake_failed` has a sentence for a revoked
 *   certificate and there is no test for it, because reaching it needs a CRL
 *   and nothing in ReconOS loads one -- `mbedtls_ssl_conf_ca_chain` is called
 *   with NULL for the revocation list. The message is written for the day that
 *   changes; today it is unreachable, which is a different thing from untested
 *   and is worth not confusing with one.
 *
 *   **`recon_tls_accept` and `recon_tls_connect`**, which both run the
 *   handshake to completion before returning. That is correct for the blocking
 *   sockets they are for, and it means neither can be driven from the same
 *   loop as its peer: one of the two has to be somewhere else while the other
 *   blocks. Covering them means a thread or a second process, which is a
 *   dependency this suite would acquire for two functions -- and everything
 *   they do besides the loop is the begin-and-step path, which is tested
 *   above. Worth doing, not worth doing here.
 *
 *   **`fail`**, the one that turns an mbedTLS error number into a sentence.
 *   Reaching it needs a library call to fail for a reason that is not a
 *   verification failure, which is memory exhaustion or a configuration that
 *   cannot occur.
 */

int main(void) {
    char root[] = "/tmp/reconos-tlsv-XXXXXX";
    if (mkdtemp(root) == NULL) {
        perror("mkdtemp");
        return 1;
    }

    printf("ReconOS TLS verification tests, root %s\n\n", root);

    if (!recon_fs_init(root)) {
        printf("could not set up the test filesystem: %s\n",
            recon_fs_last_error());
        return 1;
    }

    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);
    if (mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
            (const unsigned char *)"reconos tls tests", 17) != 0) {
        printf("could not seed the random number generator\n");
        return 1;
    }

    if (!mint_everything()) {
        printf("could not mint the test certificates\n");
        return 1;
    }

    test_the_bundle_loads();
    test_a_good_certificate_connects();
    test_the_wrong_name();
    test_expired();
    test_not_valid_yet();
    test_nothing_vouches_for_it();
    test_the_message_names_the_host();
    test_bytes_go_through();
    test_there_is_no_way_to_turn_it_off();

    free_everything();
    mbedtls_ctr_drbg_free(&g_drbg);
    mbedtls_entropy_free(&g_entropy);

    recon_tls_finish();
    recon_fs_finish();

    char command[512];
    snprintf(command, sizeof(command), "rm -rf '%s'", root);
    if (system(command) != 0) {
        printf("\nnote: could not remove %s\n", root);
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
