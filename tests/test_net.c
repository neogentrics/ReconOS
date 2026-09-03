/*
 * Tests for what ReconOS can see of the network.
 *
 * These check the parts that are true of any machine rather than of this one.
 * A test that asserts an address is 192.168.1.5 tests the tester's router; a
 * test that asserts loopback exists, is up, and is marked as loopback tests
 * the code that decided those things.
 *
 * Reaching out is deliberately not tested here. A test suite that fails when
 * the building's internet is down is a test suite people learn to ignore, and
 * the reachability path is exercised from the control socket instead, where a
 * failure can be read as "the network is off today" rather than "the build is
 * broken".
 *
 * Run with: ninja -C build && ./build/recon_net_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_fs.h"
#include "recon_net.h"
#include "recon_registry.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
        printf("        last error: %s\n", recon_net_last_error());
    }
}

/* --- Tests --- */

static void test_interfaces(void) {
    printf("what interfaces there are\n");

    recon_net_refresh();

    int count = recon_net_interface_count();
    check(count > 0, "at least one interface exists");

    /*
     * Every machine has loopback. If this fails, the enumeration is wrong --
     * not the machine.
     */
    bool found_loopback = false;
    for (int i = 0; i < count; i++) {
        struct recon_net_interface interface;
        if (!recon_net_interface_at(i, &interface)) {
            continue;
        }
        if (interface.loopback) {
            found_loopback = true;
            check(interface.up, "loopback is up");
            check(interface.address[0] != '\0', "loopback has an address");
            check(!interface.wireless, "loopback is not wireless");
        }
    }
    check(found_loopback, "loopback is among them");
}

static void test_one_entry_per_interface(void) {
    printf("one entry per interface, not per address\n");

    recon_net_refresh();
    int count = recon_net_interface_count();

    /*
     * An interface with both an IPv4 and an IPv6 address must appear once.
     * Listing it twice would make a two-address machine look like it had
     * twice the hardware, which is the kind of wrong that looks plausible.
     */
    for (int i = 0; i < count; i++) {
        struct recon_net_interface a;
        if (!recon_net_interface_at(i, &a)) {
            continue;
        }
        for (int j = i + 1; j < count; j++) {
            struct recon_net_interface b;
            if (recon_net_interface_at(j, &b)) {
                check(strcmp(a.name, b.name) != 0,
                    "no interface is listed twice");
            }
        }
    }
}

static void test_bounds(void) {
    printf("asking for what is not there\n");

    struct recon_net_interface interface;
    check(!recon_net_interface_at(-1, &interface), "index -1 is refused");
    check(!recon_net_interface_at(recon_net_interface_count(), &interface),
        "one past the end is refused");

    /* Empty rather than NULL: every caller of this prints it, and a NULL
     * would turn a missing nameserver into a crash. */
    check(recon_net_nameserver_at(-1) != NULL, "a bad nameserver index is safe");
    check(recon_net_nameserver_at(-1)[0] == '\0', "and reads as empty");
    check(recon_net_nameserver_at(9999)[0] == '\0', "at either end");
}

static void test_online_agrees_with_itself(void) {
    printf("whether it thinks it is online\n");

    recon_net_refresh();

    /*
     * Online means: a gateway, and an interface that is up, not loopback, and
     * has an address. Checked against the parts rather than assumed, so the
     * answer cannot drift from what it claims to mean.
     */
    bool has_gateway = recon_net_gateway()[0] != '\0';
    bool has_real_interface = false;

    int count = recon_net_interface_count();
    for (int i = 0; i < count; i++) {
        struct recon_net_interface interface;
        if (recon_net_interface_at(i, &interface) && interface.up &&
                !interface.loopback && interface.address[0] != '\0') {
            has_real_interface = true;
        }
    }

    check(recon_net_online() == (has_gateway && has_real_interface),
        "online matches the parts it is made of");

    if (!recon_net_online()) {
        printf("  (this machine is not online; that is not a failure)\n");
    }
}

static void test_names(void) {
    printf("naming results\n");

    /* Every result has a word for it. A result that prints as "unknown" is a
     * result somebody added and did not name. */
    check(strcmp(recon_net_result_name(RECON_NET_OK), "unknown") != 0,
        "OK has a name");
    check(strcmp(recon_net_result_name(RECON_NET_NO_SUCH_HOST), "unknown") != 0,
        "no such host has a name");
    check(strcmp(recon_net_result_name(RECON_NET_UNREACHABLE), "unknown") != 0,
        "unreachable has a name");
    check(strcmp(recon_net_result_name(RECON_NET_TIMED_OUT), "unknown") != 0,
        "timed out has a name");
    check(strcmp(recon_net_result_name(RECON_NET_NO_NETWORK), "unknown") != 0,
        "no network has a name");
}

static void test_probes_need_a_server(void) {
    printf("probing before the system is up\n");

    /*
     * recon_net_init has not been called, so there is no event loop to hang a
     * probe on. It must refuse rather than reach for a null server.
     */
    check(!recon_net_probe("example.com", 80, 100, NULL, NULL),
        "a probe with no event loop is refused");
    check(recon_net_probe_count() == 0, "and nothing is left running");

    /* Nonsense arguments are refused whatever the state. */
    check(!recon_net_probe(NULL, 80, 100, NULL, NULL), "no host is refused");
    check(!recon_net_probe("example.com", 0, 100, NULL, NULL),
        "port 0 is refused");
    check(!recon_net_probe("example.com", 70000, 100, NULL, NULL),
        "a port past 65535 is refused");
}

static void test_resolve_refuses_nothing(void) {
    printf("resolving nothing\n");

    char address[RECON_NET_ADDR_MAX];
    check(recon_net_resolve(NULL, address, sizeof(address)) != RECON_NET_OK,
        "resolving NULL fails");
    check(recon_net_resolve("", address, sizeof(address)) != RECON_NET_OK,
        "resolving an empty name fails");
    check(recon_net_resolve("example.com", NULL, 0) != RECON_NET_OK,
        "resolving into nowhere fails");
}

/*
 * The permission rule.
 *
 * Worth testing rather than eyeballing, because both of its failures are
 * silent: a rule that says no to everything looks like a broken network, and
 * a rule that says yes to everything looks like it is working.
 */
static void test_permission(void) {
    printf("who may use the network\n");

    check(!recon_net_may_use("Nobody Has Asked"),
        "an application nobody decided about may not");
    check(!recon_net_may_use(NULL), "and neither may a nameless one");
    check(!recon_net_may_use(""), "nor an empty one");

    check(recon_net_set_allowed("Terminal", true), "one can be allowed");
    check(recon_net_may_use("Terminal"), "and then it may");

    check(recon_net_set_allowed("Terminal", false), "and denied again");
    check(!recon_net_may_use("Terminal"), "and then it may not");

    /*
     * A name with a space in it. The registry refuses a key segment
     * containing one, so this silently recorded nothing at all until the
     * name was made key-safe -- and silently recording nothing reads exactly
     * like a permission that was set and ignored.
     */
    check(recon_net_set_allowed("File Explorer", true),
        "a name with a space can be allowed");
    check(recon_net_may_use("File Explorer"),
        "and the permission is found again under the same name");

    /* And it comes back out of the list spelled the way it went in. */
    bool found = false;
    int count = recon_net_allowed_count();
    for (int i = 0; i < count; i++) {
        char name[96];
        bool allowed = false;
        if (recon_net_allowed_at(i, name, sizeof(name), &allowed) &&
                strcmp(name, "File Explorer") == 0) {
            found = true;
            check(allowed, "and is listed as allowed");
        }
    }
    check(found, "a spaced name survives the round trip");

    check(!recon_net_set_allowed(NULL, true), "a nameless one cannot be set");
}

static void test_noting(void) {
    printf("noticing an application exists\n");

    int before = recon_net_allowed_count();
    recon_net_note_application("Something New");
    check(recon_net_allowed_count() == before + 1,
        "a new application appears in the list");
    check(!recon_net_may_use("Something New"),
        "not allowed, which is the default");

    /* Noting again must not undo a decision. It used to be tempting to write
     * the default every time, which would turn "allowed" back to "no" on
     * every start. */
    check(recon_net_set_allowed("Something New", true), "allow it");
    recon_net_note_application("Something New");
    check(recon_net_may_use("Something New"),
        "noting it again leaves the decision alone");
}

static void test_streams_need_permission(void) {
    printf("opening a stream\n");

    /* No event loop, so nothing can be opened whatever the permission -- but
     * the permission is checked first, and its message is the useful one. */
    check(recon_net_stream_open("Not Allowed", "example.com", 80, NULL, NULL)
        == NULL, "a stream for an unallowed application is refused");
    check(recon_net_stream_count() == 0, "and nothing is left open");

    check(recon_net_stream_open("Terminal", NULL, 80, NULL, NULL) == NULL,
        "a stream to nowhere is refused");
    check(recon_net_stream_open("Terminal", "example.com", 0, NULL, NULL)
        == NULL, "port 0 is refused");

    /* Sending on nothing is a mistake, not a crash. */
    check(!recon_net_stream_send(NULL, "x", 1), "sending on no stream fails");
    check(!recon_net_stream_send_text(NULL, "x"), "and so does sending text");
    check(!recon_net_stream_stats(NULL, NULL, NULL, NULL),
        "and asking it for figures");

    /* Closing nothing is allowed, because a caller that has already lost its
     * stream should not have to know that. */
    recon_net_stream_close(NULL);
    check(true, "closing nothing is harmless");
}

int main(void) {
    /*
     * A throwaway root, because the permission rule lives in the registry and
     * the registry writes into the filesystem. Without one these tests would
     * fail for want of somewhere to write -- or, worse, write into a real
     * installation.
     */
    char root[] = "/tmp/reconos-net-XXXXXX";
    if (mkdtemp(root) == NULL) {
        perror("mkdtemp");
        return 1;
    }

    printf("ReconOS network tests, root %s\n\n", root);

    if (!recon_fs_init(root)) {
        printf("could not set up the test filesystem: %s\n",
            recon_fs_last_error());
        return 1;
    }
    recon_registry_init();

    test_interfaces();
    test_one_entry_per_interface();
    test_bounds();
    test_online_agrees_with_itself();
    test_names();
    test_probes_need_a_server();
    test_resolve_refuses_nothing();
    test_permission();
    test_noting();
    test_streams_need_permission();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
