/*
 * ReconOS networking.
 *
 * Read this before anything else here: **ReconOS does not implement a network
 * stack.** It has no kernel of its own, so it has no ARP, no IP, no TCP. What
 * this file provides is a ReconOS-shaped view of the network the host already
 * has, and a way for ReconOS applications to use it without knowing that.
 *
 * That is the same bargain `recon_fs.h` makes with the filesystem, for the
 * same reason and with the same payoff: this is the single file that knows
 * Linux exists. Nothing above it calls socket(), reads /proc, or includes a
 * system header. When ReconOS has a stack of its own, this is what changes
 * and nothing else does.
 *
 * Saying so plainly matters. A Control Panel page listing an IP address and a
 * gateway looks exactly like an operating system doing networking, and it is
 * not one. It is ReconOS reporting what it was told.
 *
 * What is here:
 *
 *   Looking      what interfaces exist, their addresses, the gateway, the
 *                nameservers, and whether there is a way out at all.
 *   Reaching     resolving a name, and finding out whether a host answers,
 *                without blocking the desktop while it happens.
 *
 *   Streams      opening a connection to somewhere and talking over it,
 *                and the rule about which applications are allowed to.
 *
 * What is deliberately not here yet: listening. Accepting connections means
 * deciding what may reach this machine from outside, which is a larger
 * question than deciding what this machine may reach, and it should not be
 * answered by accident while building the outgoing half.
 *
 * There is also no TLS *here*. Streams opened by this file carry what they
 * are given: nothing on this side encrypts anything, and https:// does not
 * work.
 *
 * Worth being exact about, because the incoming half is no longer like this.
 * The control port in recon_control.h speaks TLS as of v0.3.0, which makes it
 * easy to read "ReconOS has TLS" and assume it applies in both directions. It
 * does not. Outgoing is still a gap and is still written down as one, rather
 * than being papered over with a plaintext connection to a port that expects
 * otherwise.
 */

#ifndef RECON_NET_H
#define RECON_NET_H

#include <stdbool.h>
#include <stddef.h>

struct wl_event_loop;

/* Long enough for an IPv6 address in text, with room to spare. */
#define RECON_NET_ADDR_MAX 64
#define RECON_NET_NAME_MAX 64

/*
 * One interface, as ReconOS sees it.
 *
 * Addresses are text rather than numbers on purpose: everything above this
 * displays them, nothing above this does arithmetic on them, and text is the
 * one representation that does not care whether it is IPv4 or IPv6.
 */
struct recon_net_interface {
    char name[RECON_NET_NAME_MAX];
    char address[RECON_NET_ADDR_MAX];
    char netmask[RECON_NET_ADDR_MAX];

    bool up;          /* the link is up and the interface is configured */
    bool loopback;    /* talks only to this machine */
    bool wireless;    /* best effort; the host does not always say */

    /* Since the interface came up, as the host counts them. */
    unsigned long long rx_bytes;
    unsigned long long tx_bytes;
};

/*
 * Bring networking up.
 *
 * Takes an event loop rather than the server, because an event loop is all it
 * needs -- somewhere to hang a socket and a timer while a probe is running.
 * Nothing here has an opinion about compositors, which is also what lets it be
 * tested without one.
 */
void recon_net_init(struct wl_event_loop *loop);
void recon_net_finish(void);

/* --- Looking --- */

/*
 * Re-read everything. Interfaces come and go, addresses change, and a page
 * showing what was true when it opened is worse than no page.
 */
void recon_net_refresh(void);

int recon_net_interface_count(void);
bool recon_net_interface_at(int index, struct recon_net_interface *out);

/* The default gateway, or "" when there is no route off this machine. */
const char *recon_net_gateway(void);

int recon_net_nameserver_count(void);
const char *recon_net_nameserver_at(int index);

/*
 * Whether there is a plausible way out: an interface that is up, is not
 * loopback, has an address, and a gateway to send through.
 *
 * "Plausible" is the honest word. It says the machine is configured to reach
 * a network, not that anything on the other side is answering -- that is what
 * a probe is for.
 */
bool recon_net_online(void);

/* The name this machine calls itself. ReconOS's own, set during setup, not
 * the host's. */
const char *recon_net_machine_name(void);

/* --- Reaching --- */

enum recon_net_result {
    RECON_NET_OK,
    RECON_NET_NO_SUCH_HOST,   /* the name did not resolve */
    RECON_NET_UNREACHABLE,    /* it resolved and refused or did not answer */
    RECON_NET_TIMED_OUT,
    RECON_NET_NO_NETWORK,     /* nothing is configured to try with */
};

const char *recon_net_result_name(enum recon_net_result result);

/*
 * Turn a name into an address, without connecting to anything.
 *
 * This one blocks. Resolution goes through the host's resolver, which has no
 * asynchronous interface worth relying on, and a lookup against a configured
 * nameserver is short. It is still the one call here that can stall the
 * desktop, which is why it is named for what it does and used sparingly.
 */
enum recon_net_result recon_net_resolve(const char *host, char *out,
    size_t size);

/*
 * Ask whether a host answers on a port, and call back when the answer is
 * known.
 *
 * Does not block: the connection is started, handed to the event loop, and
 * the callback comes later -- possibly after this function has returned and
 * the desktop has drawn several frames. A network that is simply not there
 * takes as long as the timeout to say so, and freezing a whole desktop for
 * that would be a poor way to report it.
 *
 * `elapsed_ms` is how long the answer took, which is the useful half of a
 * reachability test: "yes, in 8ms" and "yes, in 1900ms" mean different things.
 *
 * Returns false if the probe could not even be started, in which case the
 * callback is never called.
 */
typedef void (*recon_net_probe_fn)(void *user, enum recon_net_result result,
    int elapsed_ms);

bool recon_net_probe(const char *host, int port, int timeout_ms,
    recon_net_probe_fn done, void *user);

/* How many probes are outstanding. For tests, and for refusing to start a
 * hundred of them. */
int recon_net_probe_count(void);

/*
 * What the most recent probe found.
 *
 * Kept because an answer arrives after the thing that asked has finished
 * running. A terminal command returns its output when it returns; a probe
 * started by one cannot print into it later. So the outcome is recorded, and
 * whatever wants it -- the next command, a Control Panel page redrawing --
 * reads it here.
 *
 * False when nothing has been asked yet. Any of the out parameters may be
 * NULL.
 */
bool recon_net_last_probe(char *host, size_t size,
    enum recon_net_result *result, int *elapsed_ms);

const char *recon_net_last_error(void);

/*
 * --- Who may use the network ---
 *
 * Every stream is opened in the name of an application, and an application
 * that has not been allowed cannot open one.
 *
 * This exists because "which programs may talk to the internet" is a decision
 * a person should be able to see and change, and a system that never asks it
 * has answered "all of them" without saying so. The rule is deliberately
 * simple -- allowed or not, per application, remembered in the system hive --
 * because a complicated rule nobody understands is worse protection than a
 * plain one they do.
 *
 * The default is *not* allowed. An application that appears and starts
 * talking is exactly what this is for, so a new one starts silent and
 * somebody has to say otherwise.
 *
 * What this is not: isolation. ReconOS is one process, so an application that
 * wanted to bypass this could call the host's socket() directly. It stops the
 * ones that go through ReconOS, which is all of ours, and it is enforced the
 * same way accounts are -- by ReconOS, inside ReconOS.
 */
/*
 * Where the decisions are kept, so a settings page can walk them.
 *
 * No trailing slash: the registry compares prefixes by whole segment, so
 * "network/allow/" matches nothing at all -- the character after the prefix
 * has to be a slash or the end, and there it is a letter.
 */
#define RECON_NET_PERMISSION_PREFIX "network/allow"

bool recon_net_may_use(const char *application);
bool recon_net_set_allowed(const char *application, bool allowed);

/*
 * Every application that has been asked about, so a settings page can list
 * them without inventing its own record of what exists.
 */
int recon_net_allowed_count(void);
bool recon_net_allowed_at(int index, char *name, size_t size, bool *allowed);

/* Note that an application exists, so it can be listed and decided about
 * before it has tried anything. Harmless to call repeatedly. */
void recon_net_note_application(const char *application);

/* --- Streams --- */

struct recon_net_stream;

/*
 * What a stream tells its owner.
 *
 * `opened` fires once, when there is something on the other end. `received`
 * fires as bytes arrive, in whatever sizes they arrive in -- a stream is not
 * a message boundary and anything wanting one has to find it itself.
 * `closed` fires exactly once and is the last thing: after it the stream is
 * gone, and touching the handle again is a mistake.
 *
 * Any of them may be NULL.
 */
struct recon_net_stream_handlers {
    void (*opened)(void *user, struct recon_net_stream *stream);
    void (*received)(void *user, struct recon_net_stream *stream,
        const char *bytes, size_t length);
    void (*closed)(void *user, struct recon_net_stream *stream,
        enum recon_net_result reason);
};

/*
 * Open a connection, in the name of an application.
 *
 * Returns NULL if the application may not use the network, if the name does
 * not resolve, or if there is no room for another stream -- in which case no
 * handler is ever called and recon_net_last_error says which.
 *
 * Otherwise it returns immediately with a stream that is not connected yet.
 * Nothing has happened when this returns; `opened` or `closed` will say what
 * did, later, from the event loop.
 */
struct recon_net_stream *recon_net_stream_open(const char *application,
    const char *host, int port,
    const struct recon_net_stream_handlers *handlers, void *user);

/*
 * Queue bytes to send.
 *
 * Copied, so the caller's buffer is theirs again immediately. Queued rather
 * than written, because a socket that is not ready would otherwise make this
 * block -- which is the thing the whole design is avoiding.
 *
 * False when there is no room, which is real backpressure and not an error:
 * the right answer is to send less or wait, not to retry harder.
 */
bool recon_net_stream_send(struct recon_net_stream *stream,
    const char *bytes, size_t length);

/* Send a string, for the many callers whose protocol is text. */
bool recon_net_stream_send_text(struct recon_net_stream *stream,
    const char *text);

/*
 * Close it. `closed` is *not* called: the owner asked, so the owner knows.
 * A handler that fires because you asked for it is a handler people write
 * loops with.
 */
void recon_net_stream_close(struct recon_net_stream *stream);

/* How many bytes this stream has carried each way, and how long it has been
 * open. For showing what is happening rather than guessing. */
bool recon_net_stream_stats(struct recon_net_stream *stream,
    size_t *sent, size_t *received, int *age_ms);

int recon_net_stream_count(void);

#endif
