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
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "recon_loop.h"
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

/* --- A server to talk to, and a hand to turn the loop -------------------
 *
 * Eighteen functions of `src/recon_net.c` had never run, 268 lines between
 * them, the largest being `stream_event` at 106 -- the state machine for a
 * socket becoming readable or writable. Everything below exists to reach it.
 *
 * --- Why there is a real socket in here ---
 *
 * The alternative was a fake: hand recon_net a descriptor of our own and
 * pretend. But what is being tested is precisely what it does with the
 * *states a real socket goes through* -- a connect that is still in flight, a
 * peer that closes mid-read, a send that cannot take everything at once. A
 * fake that produced those would be a second implementation of the thing
 * under test, and the first one to disagree would be the one nobody checked.
 *
 * A listener on 127.0.0.1 costs a few lines and is the real thing.
 *
 * --- And why the test turns the loop by hand ---
 *
 * `include/recon_loop.h` says there is no `poll` in the interface on purpose:
 * ReconOS has no such call, and whoever owns the loop knows how it finds out.
 * **This test is that owner.** It asks the loop what it is waiting on, polls
 * exactly that, and hands the answers back -- which is the same three steps
 * `userland/` will take on the real machine, so the path being exercised is
 * the path that will run.
 */

struct server {
    int listener;
    int port;
    int accepted;          /* the connection this test's end holds, or -1 */
};

static bool server_start(struct server *sv) {
    sv->accepted = -1;
    sv->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (sv->listener < 0) {
        return false;
    }

    int on = 1;
    setsockopt(sv->listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;                       /* the system picks one */

    if (bind(sv->listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
            listen(sv->listener, 4) != 0) {
        close(sv->listener);
        sv->listener = -1;
        return false;
    }

    socklen_t length = sizeof(addr);
    if (getsockname(sv->listener, (struct sockaddr *)&addr, &length) != 0) {
        close(sv->listener);
        sv->listener = -1;
        return false;
    }
    sv->port = ntohs(addr.sin_port);
    return true;
}

static void server_stop(struct server *sv) {
    if (sv->accepted >= 0) {
        close(sv->accepted);
        sv->accepted = -1;
    }
    if (sv->listener >= 0) {
        close(sv->listener);
        sv->listener = -1;
    }
}

/* Take the connection, if one is waiting. Not an error when none is: the
 * connect may not have reached the listener yet, and the pump below calls
 * this repeatedly rather than once at a moment it guessed. */
static void server_accept(struct server *sv) {
    if (sv->listener < 0 || sv->accepted >= 0) {
        return;
    }

    struct pollfd p = { .fd = sv->listener, .events = POLLIN };

    if (poll(&p, 1, 0) == 1 && (p.revents & POLLIN) != 0) {
        sv->accepted = accept(sv->listener, NULL, NULL);
    }
}

static uint64_t now_ms(void) {
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000u + (uint64_t)(tv.tv_usec / 1000);
}

/*
 * Turn the loop for up to `ms`, or until `done` says to stop.
 *
 * The three steps an owner takes, and no more: ask the loop what it waits on,
 * wait on exactly that, hand back what happened. Timers are ticked with the
 * real clock because recon_net arms real ones.
 */
static void pump(struct recon_loop *loop, struct server *sv, int ms,
        bool (*done)(void *), void *user) {
    uint64_t until = now_ms() + (uint64_t)ms;

    while (now_ms() < until) {
        server_accept(sv);

        if (done != NULL && done(user)) {
            return;
        }

        struct pollfd set[8];
        int fds[8];
        int count = recon_loop_watch_count(loop);

        if (count > 8) {
            count = 8;
        }

        int n = 0;
        for (int i = 0; i < count; i++) {
            unsigned want = 0;

            if (!recon_loop_watch_at(loop, i, &fds[n], &want)) {
                continue;
            }
            set[n].fd = fds[n];
            set[n].events = 0;
            if ((want & RECON_WATCH_READABLE) != 0) {
                set[n].events |= POLLIN;
            }
            if ((want & RECON_WATCH_WRITABLE) != 0) {
                set[n].events |= POLLOUT;
            }
            set[n].revents = 0;
            n++;
        }

        /* A short wait rather than none: a spin would make this test the
         * busiest thing on the machine, and a long one would make a timer
         * that was due fire late enough to change what is being measured. */
        if (n > 0) {
            poll(set, (nfds_t)n, 5);
        } else {
            struct timespec rest = { 0, 5 * 1000 * 1000 };

            nanosleep(&rest, NULL);
        }

        for (int i = 0; i < n; i++) {
            unsigned events = 0;

            if ((set[i].revents & POLLIN) != 0) {
                events |= RECON_WATCH_READABLE;
            }
            if ((set[i].revents & POLLOUT) != 0) {
                events |= RECON_WATCH_WRITABLE;
            }
            if ((set[i].revents & POLLHUP) != 0) {
                events |= RECON_WATCH_HANGUP;
            }
            if ((set[i].revents & POLLERR) != 0) {
                events |= RECON_WATCH_ERROR;
            }
            if (events != 0) {
                recon_loop_ready(loop, set[i].fd, events);
            }
        }

        recon_loop_tick(loop, now_ms());
    }
}


/* --- What the handlers saw --------------------------------------------- */

struct seen {
    int opened;
    int secured;
    int closed;
    enum recon_net_result reason;
    char text[512];
    size_t length;
};

static void saw_opened(void *user, struct recon_net_stream *stream) {
    (void)stream;
    ((struct seen *)user)->opened++;
}

static void saw_secured(void *user, struct recon_net_stream *stream) {
    (void)stream;
    ((struct seen *)user)->secured++;
}

static void saw_received(void *user, struct recon_net_stream *stream,
        const char *bytes, size_t length) {
    struct seen *s = user;

    (void)stream;
    if (s->length + length < sizeof(s->text)) {
        memcpy(s->text + s->length, bytes, length);
        s->length += length;
        s->text[s->length] = '\0';
    }
}

static void saw_closed(void *user, struct recon_net_stream *stream,
        enum recon_net_result reason) {
    struct seen *s = user;

    (void)stream;
    s->closed++;
    s->reason = reason;
}

static const struct recon_net_stream_handlers HANDLERS = {
    .opened = saw_opened,
    .secured = saw_secured,
    .received = saw_received,
    .closed = saw_closed,
};

static bool is_open(void *user) {
    return ((struct seen *)user)->opened > 0;
}

static bool has_text(void *user) {
    return ((struct seen *)user)->length > 0;
}

static bool is_closed(void *user) {
    return ((struct seen *)user)->closed > 0;
}


/* --- The tests ---------------------------------------------------------- */

static void test_a_connection_opens_and_carries_bytes(void) {
    printf("a stream connects, sends and receives\n");

    struct recon_loop *loop = recon_loop_create();
    struct server sv;
    struct seen seen;

    memset(&seen, 0, sizeof(seen));
    check(loop != NULL && server_start(&sv), "a listener is up");
    if (loop == NULL) {
        return;
    }

    recon_net_init(loop);

    /*
     * Allowed first, because it is not by default and that is the firewall
     * working: `recon_net_stream_open` asks before it opens anything, and an
     * application nobody has said yes to does not get a socket. The first run
     * of these tests failed on exactly that, which is the check earning its
     * place rather than getting in the way.
     */
    recon_net_set_allowed("tests", true);

    struct recon_net_stream *stream = recon_net_stream_open("tests",
        "127.0.0.1", sv.port, &HANDLERS, &seen);

    check(stream != NULL, "the stream opens");
    if (stream == NULL) {
        printf("    (%s)\n", recon_net_last_error());
        server_stop(&sv);
        recon_net_finish();
        recon_loop_destroy(loop);
        return;
    }

    /*
     * Nothing has happened yet, and the header says so: "it returns
     * immediately with a stream that is not connected yet". Checked, because
     * a version that connected synchronously would pass every check below and
     * block the desktop for as long as a far end took to answer.
     */
    check(seen.opened == 0, "and nothing has happened yet");

    pump(loop, &sv, 2000, is_open, &seen);
    check(seen.opened == 1, "the connect finishes and `opened` is called");
    check(sv.accepted >= 0, "and the far end has the connection");

    check(recon_net_stream_send_text(stream, "hello from the desktop\n"),
        "it takes something to send");
    pump(loop, &sv, 500, NULL, NULL);

    char got[64] = "";
    ssize_t n = sv.accepted >= 0 ? recv(sv.accepted, got, sizeof(got) - 1,
        MSG_DONTWAIT) : -1;

    check(n > 0 && strncmp(got, "hello from the desktop", 22) == 0,
        "and the far end receives it");

    /* And the other direction, which is `stream_event`'s readable half. */
    if (sv.accepted >= 0) {
        send(sv.accepted, "and back again\n", 15, 0);
    }
    pump(loop, &sv, 1000, has_text, &seen);
    check(seen.length == 15 && strncmp(seen.text, "and back again", 14) == 0,
        "what the far end sends arrives at `received`");

    size_t sent = 0;
    size_t received = 0;
    int age = -1;

    check(recon_net_stream_stats(stream, &sent, &received, &age),
        "the stream can say how much it has carried");
    check(sent >= 23 && received == 15, "and the numbers are what went by");
    check(age >= 0, "and how long it has been open");

    recon_net_stream_close(stream);
    server_stop(&sv);
    recon_net_finish();
    recon_loop_destroy(loop);
}

static void test_a_far_end_that_goes_away(void) {
    printf("the far end closes while the stream is open\n");

    struct recon_loop *loop = recon_loop_create();
    struct server sv;
    struct seen seen;

    memset(&seen, 0, sizeof(seen));
    check(loop != NULL && server_start(&sv), "a listener is up");
    if (loop == NULL) {
        return;
    }
    recon_net_init(loop);

    /*
     * Allowed first, because it is not by default and that is the firewall
     * working: `recon_net_stream_open` asks before it opens anything, and an
     * application nobody has said yes to does not get a socket. The first run
     * of these tests failed on exactly that, which is the check earning its
     * place rather than getting in the way.
     */
    recon_net_set_allowed("tests", true);

    struct recon_net_stream *stream = recon_net_stream_open("tests",
        "127.0.0.1", sv.port, &HANDLERS, &seen);

    check(stream != NULL, "the stream opens");
    if (stream == NULL) {
        server_stop(&sv);
        recon_net_finish();
        recon_loop_destroy(loop);
        return;
    }

    pump(loop, &sv, 2000, is_open, &seen);
    check(seen.opened == 1, "and connects");

    /*
     * The far end goes without saying anything. This is the path that used to
     * be unreachable: a hangup arrives on the descriptor, and until v0.4.68
     * the seam reported every event as "readable" -- so the only way to learn
     * this had happened was to read and get nothing, which is also what a
     * quiet connection looks like.
     */
    if (sv.accepted >= 0) {
        close(sv.accepted);
        sv.accepted = -1;
    }

    pump(loop, &sv, 2000, is_closed, &seen);
    check(seen.closed == 1, "`closed` is called once");
    check(seen.opened == 1, "and `opened` is not called again");

    server_stop(&sv);
    recon_net_finish();
    recon_loop_destroy(loop);
}

static void test_nothing_listening(void) {
    printf("a port with nothing behind it\n");

    struct recon_loop *loop = recon_loop_create();
    struct server sv;
    struct seen seen;

    memset(&seen, 0, sizeof(seen));
    check(loop != NULL && server_start(&sv), "a listener is up");
    if (loop == NULL) {
        return;
    }

    /*
     * The port is real and then the listener is shut, so the number is one
     * nothing is on rather than one picked out of the air -- a port chosen by
     * hand is a port something else on the machine may be using, and the
     * failure then is a test that talks to somebody else's program.
     */
    int port = sv.port;

    server_stop(&sv);
    recon_net_init(loop);

    /*
     * Allowed first, because it is not by default and that is the firewall
     * working: `recon_net_stream_open` asks before it opens anything, and an
     * application nobody has said yes to does not get a socket. The first run
     * of these tests failed on exactly that, which is the check earning its
     * place rather than getting in the way.
     */
    recon_net_set_allowed("tests", true);

    struct recon_net_stream *stream = recon_net_stream_open("tests",
        "127.0.0.1", port, &HANDLERS, &seen);

    if (stream != NULL) {
        pump(loop, &sv, 2000, is_closed, &seen);
        check(seen.closed == 1, "the refusal arrives as `closed`");
        check(seen.opened == 0, "and `opened` never was");
        check(seen.reason == RECON_NET_UNREACHABLE,
            "with a reason saying it was refused rather than slow");
    } else {
        /* Refused before it ever reached the loop, which is also correct and
         * is what a system that checks reachability first would do. */
        check(true, "it was refused outright");
    }

    recon_net_finish();
    recon_loop_destroy(loop);
}

/* What a probe was told. */
struct probed {
    int calls;
    enum recon_net_result result;
    int elapsed_ms;
};

static void saw_probe(void *user, enum recon_net_result result,
        int elapsed_ms) {
    struct probed *p = user;

    p->calls++;
    p->result = result;
    p->elapsed_ms = elapsed_ms;
}

static bool probe_answered(void *user) {
    return ((struct probed *)user)->calls > 0;
}

static void test_a_probe_reaches_something_and_says_how_long(void) {
    printf("asking whether something is reachable\n");

    struct recon_loop *loop = recon_loop_create();
    struct server sv;
    struct probed p;

    memset(&p, 0, sizeof(p));
    check(loop != NULL && server_start(&sv), "a listener is up");
    if (loop == NULL) {
        return;
    }
    recon_net_init(loop);

    check(recon_net_probe("127.0.0.1", sv.port, 2000, saw_probe, &p),
        "a probe starts");
    check(recon_net_probe_count() == 1, "and is outstanding");

    pump(loop, &sv, 3000, probe_answered, &p);

    check(p.calls == 1, "it answers once");
    check(p.result == RECON_NET_OK, "and the listener was reachable");

    /*
     * The elapsed time, and why it is checked at all: the header says it is
     * *the useful half* of a reachability test, because "yes, in 8ms" and
     * "yes, in 1900ms" mean different things. A probe that answered OK with
     * nothing in that field would look right and be half a result.
     */
    check(p.elapsed_ms >= 0 && p.elapsed_ms < 3000,
        "with how long it took");
    check(recon_net_probe_count() == 0, "and nothing is outstanding after");

    char host[64] = "";
    enum recon_net_result result = RECON_NET_NO_NETWORK;
    int elapsed = -1;

    check(recon_net_last_probe(host, sizeof(host), &result, &elapsed),
        "the last probe can be read back");
    /*
     * `host:port`, not the host alone. The first version of this check
     * expected the host and was wrong: two probes to one machine on different
     * ports are different probes, and an answer that named only the machine
     * could not tell them apart. The header did not say which, and says so
     * now.
     */
    char expected[64];

    snprintf(expected, sizeof(expected), "127.0.0.1:%d", sv.port);
    check(strcmp(host, expected) == 0, "naming what was probed, with the port");
    check(result == RECON_NET_OK, "with the answer it got");

    server_stop(&sv);
    recon_net_finish();
    recon_loop_destroy(loop);
}

static void test_a_peer_that_accepts_and_says_nothing(void) {
    printf("a far end that takes the connection and never answers\n");

    struct recon_loop *loop = recon_loop_create();
    struct server sv;
    struct seen seen;

    memset(&seen, 0, sizeof(seen));
    check(loop != NULL && server_start(&sv), "a listener is up");
    if (loop == NULL) {
        return;
    }
    recon_net_init(loop);
    recon_net_set_allowed("tests", true);

    struct recon_net_stream *stream = recon_net_stream_open("tests",
        "127.0.0.1", sv.port, &HANDLERS, &seen);

    check(stream != NULL, "the stream opens");
    if (stream == NULL) {
        server_stop(&sv);
        recon_net_finish();
        recon_loop_destroy(loop);
        return;
    }

    pump(loop, &sv, 2000, is_open, &seen);
    check(seen.opened == 1, "and connects");

    /*
     * --- Why this is not a test of the eight-second timeout ---
     *
     * `STREAM_CONNECT_MS` is eight seconds, and a suite that waited for it
     * would add eight seconds to every run of `ctest` to watch a clock. What
     * is checked instead is the half that is cheap and is what a caller
     * actually depends on: **a connection nobody is speaking on stays open
     * and is not closed by mistake.**
     *
     * That is the failure worth catching. A stream torn down because the far
     * end was thinking is a request that failed for no reason, and it looks
     * exactly like a network fault from the outside. A timeout that fires
     * late is a slow answer; one that fires early is a wrong one.
     */
    pump(loop, &sv, 700, NULL, NULL);
    check(seen.closed == 0, "a quiet connection is left alone");
    check(recon_net_stream_count() == 1, "and is still counted as open");

    size_t sent = 0;
    size_t received = 0;
    int age = -1;

    check(recon_net_stream_stats(stream, &sent, &received, &age),
        "its figures can still be read");
    check(received == 0, "nothing arrived");
    check(age >= 500, "and it has been open a while");

    recon_net_stream_close(stream);
    check(recon_net_stream_count() == 0, "closing it takes it off the list");

    server_stop(&sv);
    recon_net_finish();
    recon_loop_destroy(loop);
}

static void test_the_things_that_answer_without_a_network(void) {
    printf("what can be asked with nothing connected\n");

    struct recon_loop *loop = recon_loop_create();

    check(loop != NULL, "a loop");
    if (loop == NULL) {
        return;
    }
    recon_net_init(loop);

    check(recon_net_loop() == loop,
        "the loop it was started with is the loop it hands back");
    check(recon_net_machine_name() != NULL &&
        recon_net_machine_name()[0] != '\0',
        "the machine has a name");
    check(recon_net_nameserver_count() >= 0,
        "and says how many nameservers it knows of");
    check(recon_net_last_error() != NULL, "there is always an error to read");
    check(recon_net_stream_count() == 0, "and no streams are open");

    /*
     * `recon_net_finish` twice. The second is the interesting one: a shutdown
     * that only works once is a shutdown that faults when something goes wrong
     * on the way to it and the caller tries again.
     */
    recon_net_finish();
    recon_net_finish();
    check(recon_net_stream_count() == 0, "and finishing twice is safe");
    check(recon_net_loop() == NULL, "with the loop let go");

    recon_loop_destroy(loop);
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

    test_a_connection_opens_and_carries_bytes();
    test_a_far_end_that_goes_away();
    test_nothing_listening();
    test_a_probe_reaches_something_and_says_how_long();
    test_a_peer_that_accepts_and_says_nothing();
    test_the_things_that_answer_without_a_network();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
