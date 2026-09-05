/*
 * ReconOS networking. See include/recon_net.h.
 *
 * Everything Linux-specific in the system's networking is in this file. It
 * reads the host's interface list, routing table and resolver configuration,
 * and it opens sockets. Nothing above it does any of that.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <wayland-server-core.h>

#include "recon_error.h"
#include "recon_firewall.h"
#include "recon_net.h"
#include "recon_tls.h"
#include "recon_registry.h"

#define INTERFACES_MAX 16
#define NAMESERVERS_MAX 4
#define PROBES_MAX 8

/*
 * How many connections may be open at once, and how much may be queued on
 * each. Both small on purpose: this is a desktop, not a server, and a limit
 * that is reached is a bug worth noticing rather than memory worth growing.
 */
#define STREAMS_MAX 16
#define STREAM_BUFFER 8192
#define STREAM_CONNECT_MS 8000

/* Where the machine's name is kept. The same key the setup flow writes. */
#define MACHINE_NAME_KEY "system/machine-name"
#define MACHINE_NAME_DEFAULT "recon-tower"

static struct wl_event_loop *g_loop;

static struct recon_net_interface g_interfaces[INTERFACES_MAX];
static int g_interface_count;

static char g_gateway[RECON_NET_ADDR_MAX];
static char g_nameservers[NAMESERVERS_MAX][RECON_NET_ADDR_MAX];
static int g_nameserver_count;

static char g_error[256];

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, args);
    va_end(args);
}

const char *recon_net_last_error(void) {
    return g_error[0] != '\0' ? g_error : "no error";
}

const char *recon_net_result_name(enum recon_net_result result) {
    switch (result) {
    case RECON_NET_OK:           return "answered";
    case RECON_NET_NO_SUCH_HOST:  return "no such host";
    case RECON_NET_UNREACHABLE:   return "unreachable";
    case RECON_NET_TIMED_OUT:     return "timed out";
    case RECON_NET_NO_NETWORK:    return "no network";
    case RECON_NET_UNTRUSTED:     return "could not prove who it is";
    }
    return "unknown";
}

/* --- Looking --- */

/* Read one short line out of a file, which is how most of /sys reports. */
static bool read_line(const char *path, char *out, size_t size) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return false;
    }
    bool ok = fgets(out, (int)size, f) != NULL;
    fclose(f);

    if (!ok) {
        return false;
    }
    char *newline = strchr(out, '\n');
    if (newline != NULL) {
        *newline = '\0';
    }
    return true;
}

static unsigned long long read_counter(const char *interface, const char *what) {
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/%s",
        interface, what);

    char text[64];
    if (!read_line(path, text, sizeof(text))) {
        return 0;
    }
    return strtoull(text, NULL, 10);
}

/*
 * Whether an interface is wireless.
 *
 * By the presence of a directory the kernel creates for wireless devices.
 * There is no portable answer and this one is only a hint, which is why
 * nothing depends on it -- it decides an icon, not a behaviour.
 */
static bool is_wireless(const char *interface) {
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/net/%s/wireless", interface);

    struct stat st;
    return stat(path, &st) == 0;
}

static struct recon_net_interface *interface_named(const char *name) {
    for (int i = 0; i < g_interface_count; i++) {
        if (strcmp(g_interfaces[i].name, name) == 0) {
            return &g_interfaces[i];
        }
    }
    return NULL;
}

/*
 * The default gateway, from the kernel's routing table.
 *
 * /proc/net/route is a table of hex, little-endian, one row per route. The
 * default route is the one whose destination is all zeroes: it matches
 * everything, which is what "send anything you do not otherwise know about
 * here" means.
 */
static void read_gateway(void) {
    g_gateway[0] = '\0';

    FILE *f = fopen("/proc/net/route", "r");
    if (f == NULL) {
        return;
    }

    char line[256];
    /* The first line is the column headings. */
    if (fgets(line, sizeof(line), f) == NULL) {
        fclose(f);
        return;
    }

    while (fgets(line, sizeof(line), f) != NULL) {
        char name[RECON_NET_NAME_MAX];
        unsigned long destination = 0;
        unsigned long gateway = 0;

        if (sscanf(line, "%63s %lx %lx", name, &destination, &gateway) != 3) {
            continue;
        }
        if (destination != 0 || gateway == 0) {
            continue;
        }

        struct in_addr address;
        address.s_addr = (in_addr_t)gateway;
        snprintf(g_gateway, sizeof(g_gateway), "%s", inet_ntoa(address));
        break;
    }

    fclose(f);
}

/* The resolvers the host is configured to ask. */
static void read_nameservers(void) {
    g_nameserver_count = 0;

    FILE *f = fopen("/etc/resolv.conf", "r");
    if (f == NULL) {
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f) != NULL &&
            g_nameserver_count < NAMESERVERS_MAX) {
        char address[RECON_NET_ADDR_MAX];
        if (sscanf(line, " nameserver %63s", address) == 1) {
            snprintf(g_nameservers[g_nameserver_count], RECON_NET_ADDR_MAX,
                "%s", address);
            g_nameserver_count++;
        }
    }

    fclose(f);
}

void recon_net_refresh(void) {
    g_interface_count = 0;

    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) != 0) {
        set_error("cannot read the interface list: %s", strerror(errno));
        return;
    }

    for (struct ifaddrs *it = list; it != NULL; it = it->ifa_next) {
        if (it->ifa_addr == NULL) {
            continue;
        }

        int family = it->ifa_addr->sa_family;
        if (family != AF_INET && family != AF_INET6) {
            /* Packet-level entries carry counters, not addresses. Skipped:
             * an interface without an address is one nothing can be said
             * about that anybody wanted to know. */
            continue;
        }

        /*
         * One entry per interface, not per address. An interface with both an
         * IPv4 and an IPv6 address is one interface, and listing it twice
         * would make a two-address machine look like a four-interface one.
         * The first address wins, and IPv4 is preferred because it is what
         * everything else here displays.
         */
        struct recon_net_interface *entry = interface_named(it->ifa_name);
        bool fresh = (entry == NULL);

        if (fresh) {
            if (g_interface_count >= INTERFACES_MAX) {
                continue;
            }
            entry = &g_interfaces[g_interface_count++];
            memset(entry, 0, sizeof(*entry));
            snprintf(entry->name, sizeof(entry->name), "%s", it->ifa_name);

            entry->up = (it->ifa_flags & IFF_UP) != 0 &&
                (it->ifa_flags & IFF_RUNNING) != 0;
            entry->loopback = (it->ifa_flags & IFF_LOOPBACK) != 0;
            entry->wireless = is_wireless(it->ifa_name);
            entry->rx_bytes = read_counter(it->ifa_name, "rx_bytes");
            entry->tx_bytes = read_counter(it->ifa_name, "tx_bytes");
        } else if (family == AF_INET6 && entry->address[0] != '\0') {
            continue;   /* Already has an address, and this is the lesser one. */
        }

        size_t length = (family == AF_INET)
            ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);

        char text[RECON_NET_ADDR_MAX];
        if (getnameinfo(it->ifa_addr, (socklen_t)length, text, sizeof(text),
                NULL, 0, NI_NUMERICHOST) == 0) {
            /* An IPv6 link-local address carries a %scope suffix that means
             * nothing to a reader. */
            char *scope = strchr(text, '%');
            if (scope != NULL) {
                *scope = '\0';
            }
            snprintf(entry->address, sizeof(entry->address), "%s", text);
        }

        if (it->ifa_netmask != NULL &&
                getnameinfo(it->ifa_netmask, (socklen_t)length, text,
                    sizeof(text), NULL, 0, NI_NUMERICHOST) == 0) {
            snprintf(entry->netmask, sizeof(entry->netmask), "%s", text);
        }
    }

    freeifaddrs(list);

    read_gateway();
    read_nameservers();
}

int recon_net_interface_count(void) {
    return g_interface_count;
}

bool recon_net_interface_at(int index, struct recon_net_interface *out) {
    if (index < 0 || index >= g_interface_count) {
        return false;
    }
    if (out != NULL) {
        *out = g_interfaces[index];
    }
    return true;
}

const char *recon_net_gateway(void) {
    return g_gateway;
}

int recon_net_nameserver_count(void) {
    return g_nameserver_count;
}

const char *recon_net_nameserver_at(int index) {
    if (index < 0 || index >= g_nameserver_count) {
        return "";
    }
    return g_nameservers[index];
}

bool recon_net_online(void) {
    if (g_gateway[0] == '\0') {
        return false;
    }
    for (int i = 0; i < g_interface_count; i++) {
        if (g_interfaces[i].up && !g_interfaces[i].loopback &&
                g_interfaces[i].address[0] != '\0') {
            return true;
        }
    }
    return false;
}

const char *recon_net_machine_name(void) {
    return recon_registry_get(RECON_REG_SYSTEM, MACHINE_NAME_KEY,
        MACHINE_NAME_DEFAULT);
}

/* --- Reaching --- */

struct wl_event_loop *recon_net_event_loop(void) {
    return g_loop;
}

enum recon_net_result recon_net_resolve(const char *host, char *out,
        size_t size) {
    if (host == NULL || *host == '\0' || out == NULL || size == 0) {
        set_error("nothing to resolve");
        return RECON_NET_NO_SUCH_HOST;
    }
    out[0] = '\0';

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *found = NULL;
    int status = getaddrinfo(host, NULL, &hints, &found);
    if (status != 0 || found == NULL) {
        set_error("%s: %s", host, gai_strerror(status));
        return RECON_NET_NO_SUCH_HOST;
    }

    char text[RECON_NET_ADDR_MAX];
    enum recon_net_result result = RECON_NET_NO_SUCH_HOST;

    if (getnameinfo(found->ai_addr, found->ai_addrlen, text, sizeof(text),
            NULL, 0, NI_NUMERICHOST) == 0) {
        snprintf(out, size, "%s", text);
        result = RECON_NET_OK;
    }

    freeaddrinfo(found);
    return result;
}

/*
 * One outstanding reachability test.
 *
 * A connection that has been started and not yet succeeded or failed. The
 * event loop owns it: one source watching the socket for writability, which
 * is how a non-blocking connect reports that it finished, and one timer for
 * the case where it never does.
 */
struct probe {
    bool used;
    int fd;
    char host[128];
    struct wl_event_source *ready;
    struct wl_event_source *deadline;
    recon_net_probe_fn done;
    void *user;
    struct timespec started;
};

static struct probe g_probes[PROBES_MAX];

/* The last answer, kept because it arrives after whatever asked has gone. */
static bool g_have_last;
static char g_last_host[128];
static enum recon_net_result g_last_result;
static int g_last_elapsed;

bool recon_net_last_probe(char *host, size_t size,
        enum recon_net_result *result, int *elapsed_ms) {
    if (!g_have_last) {
        return false;
    }
    if (host != NULL && size > 0) {
        snprintf(host, size, "%s", g_last_host);
    }
    if (result != NULL) {
        *result = g_last_result;
    }
    if (elapsed_ms != NULL) {
        *elapsed_ms = g_last_elapsed;
    }
    return true;
}

static int elapsed_since(const struct timespec *then) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    long long ms = (long long)(now.tv_sec - then->tv_sec) * 1000 +
        (now.tv_nsec - then->tv_nsec) / 1000000;
    return ms < 0 ? 0 : (int)ms;
}

static void probe_finish(struct probe *probe, enum recon_net_result result) {
    if (!probe->used) {
        return;
    }

    /*
     * Everything is taken down before the callback runs, so a callback that
     * starts another probe cannot find this one half-alive.
     */
    recon_net_probe_fn done = probe->done;
    void *user = probe->user;
    int elapsed = elapsed_since(&probe->started);

    /* Recorded before the callback, so a callback that reads it sees its own
     * result rather than the one before. */
    g_have_last = true;
    snprintf(g_last_host, sizeof(g_last_host), "%s", probe->host);
    g_last_result = result;
    g_last_elapsed = elapsed;

    if (probe->ready != NULL) {
        wl_event_source_remove(probe->ready);
    }
    if (probe->deadline != NULL) {
        wl_event_source_remove(probe->deadline);
    }
    if (probe->fd >= 0) {
        close(probe->fd);
    }
    memset(probe, 0, sizeof(*probe));

    if (done != NULL) {
        done(user, result, elapsed);
    }
}

static int probe_ready(int fd, uint32_t mask, void *data) {
    struct probe *probe = data;
    (void)mask;

    /*
     * A non-blocking connect reports its outcome as a socket error, not as
     * the result of anything. Zero means it connected.
     */
    int error = 0;
    socklen_t length = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0) {
        error = errno;
    }

    if (error == 0) {
        probe_finish(probe, RECON_NET_OK);
    } else {
        set_error("%s", strerror(error));
        probe_finish(probe, RECON_NET_UNREACHABLE);
    }
    return 0;
}

static int probe_expired(void *data) {
    probe_finish(data, RECON_NET_TIMED_OUT);
    return 0;
}

int recon_net_probe_count(void) {
    int count = 0;
    for (int i = 0; i < PROBES_MAX; i++) {
        if (g_probes[i].used) {
            count++;
        }
    }
    return count;
}

bool recon_net_probe(const char *host, int port, int timeout_ms,
        recon_net_probe_fn done, void *user) {
    if (g_loop == NULL) {
        set_error("networking is not up");
        return false;
    }
    if (host == NULL || *host == '\0' || port <= 0 || port > 65535) {
        set_error("a host and a port are needed");
        return false;
    }

    struct probe *probe = NULL;
    for (int i = 0; i < PROBES_MAX; i++) {
        if (!g_probes[i].used) {
            probe = &g_probes[i];
            break;
        }
    }
    if (probe == NULL) {
        set_error("too many tests already running");
        return false;
    }

    char service[16];
    snprintf(service, sizeof(service), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *found = NULL;
    int status = getaddrinfo(host, service, &hints, &found);
    if (status != 0 || found == NULL) {
        set_error("%s: %s", host, gai_strerror(status));
        return false;
    }

    int fd = socket(found->ai_family, found->ai_socktype | SOCK_NONBLOCK,
        found->ai_protocol);
    if (fd < 0) {
        set_error("cannot open a socket: %s", strerror(errno));
        freeaddrinfo(found);
        return false;
    }

    int connected = connect(fd, found->ai_addr, found->ai_addrlen);
    int reason = errno;
    freeaddrinfo(found);

    if (connected != 0 && reason != EINPROGRESS) {
        set_error("%s", strerror(reason));
        close(fd);
        return false;
    }

    memset(probe, 0, sizeof(*probe));
    probe->used = true;
    probe->fd = fd;
    probe->done = done;
    probe->user = user;
    snprintf(probe->host, sizeof(probe->host), "%s:%d", host, port);
    clock_gettime(CLOCK_MONOTONIC, &probe->started);

    struct wl_event_loop *loop = g_loop;

    /*
     * Watched for writability even when the connect already succeeded. A
     * socket that is connected is writable, so the callback arrives on the
     * next turn of the loop -- which keeps one path through this function
     * instead of two, and means the callback never runs before the caller
     * has finished setting itself up.
     */
    probe->ready = wl_event_loop_add_fd(loop, fd, WL_EVENT_WRITABLE,
        probe_ready, probe);
    probe->deadline = wl_event_loop_add_timer(loop, probe_expired, probe);

    if (probe->ready == NULL || probe->deadline == NULL) {
        set_error("cannot watch the connection");
        probe_finish(probe, RECON_NET_UNREACHABLE);
        return false;
    }

    wl_event_source_timer_update(probe->deadline,
        timeout_ms > 0 ? timeout_ms : 3000);
    return true;
}

/* --- Lifecycle --- */

void recon_net_init(struct wl_event_loop *loop) {
    g_loop = loop;
    memset(g_probes, 0, sizeof(g_probes));
    recon_net_refresh();

    /* Not logged from here. Logging is wlroots', and the point of taking an
     * event loop rather than the server was that this file needs nothing from
     * the compositor -- including its logger. Whoever calls this says what
     * was found. */
}

/* --- Who may use the network --- */

/*
 * Where one application's permission lives. Under a prefix of its own, so a
 * settings page can list them by walking it rather than by keeping a second
 * copy of what applications exist.
 */
static void permission_key(const char *application, char *out, size_t size) {
    /*
     * Spaces become dashes, the way window geometry keys already do it: a key
     * is a path, not a sentence, and the registry refuses a segment with a
     * space in it. Without this, every application whose name has a space --
     * which is most of them -- silently failed to record a permission at all.
     *
     * Reversed for display, which means an application with a real dash in
     * its name would show a space. None has one, and the alternative is
     * storing the name twice.
     */
    char safe[96];
    size_t used = 0;
    for (const char *c = application != NULL ? application : "";
            *c != '\0' && used < sizeof(safe) - 1; c++) {
        safe[used++] = (*c == ' ' || *c == '/') ? '-' : *c;
    }
    safe[used] = '\0';

    snprintf(out, size, "%s/%s", RECON_NET_PERMISSION_PREFIX, safe);
}

bool recon_net_may_use(const char *application) {
    if (application == NULL || *application == '\0') {
        return false;
    }
    char key[RECON_REGISTRY_KEY_MAX];
    permission_key(application, key, sizeof(key));

    /* Not allowed unless somebody said so. An application that appears and
     * starts talking is what this exists to catch. */
    return recon_registry_get_bool(RECON_REG_SYSTEM, key, false);
}

bool recon_net_set_allowed(const char *application, bool allowed) {
    if (application == NULL || *application == '\0') {
        set_error("no application named");
        return false;
    }
    char key[RECON_REGISTRY_KEY_MAX];
    permission_key(application, key, sizeof(key));

    if (!recon_registry_set_bool(RECON_REG_SYSTEM, key, allowed)) {
        set_error("%s", recon_registry_last_error());
        return false;
    }
    return true;
}

void recon_net_note_application(const char *application) {
    if (application == NULL || *application == '\0') {
        return;
    }
    char key[RECON_REGISTRY_KEY_MAX];
    permission_key(application, key, sizeof(key));

    /* Only if it is not already recorded: writing every time would turn a
     * deliberate "allowed" back into the default on the next start. */
    if (recon_registry_get(RECON_REG_SYSTEM, key, NULL) == NULL) {
        recon_registry_set_bool(RECON_REG_SYSTEM, key, false);
    }
}

int recon_net_allowed_count(void) {
    return recon_registry_count(RECON_REG_SYSTEM,
        RECON_NET_PERMISSION_PREFIX);
}

bool recon_net_allowed_at(int index, char *name, size_t size, bool *allowed) {
    const char *key = NULL;
    const char *value = NULL;
    if (!recon_registry_at(RECON_REG_SYSTEM, RECON_NET_PERMISSION_PREFIX,
            index, &key, &value)) {
        return false;
    }

    if (name != NULL && size > 0) {
        /* What follows the prefix and its slash, with dashes read back as the
         * spaces they stood in for. */
        size_t prefix = strlen(RECON_NET_PERMISSION_PREFIX) + 1;
        snprintf(name, size, "%s", strlen(key) > prefix ? key + prefix : key);

        for (char *c = name; *c != '\0'; c++) {
            if (*c == '-') {
                *c = ' ';
            }
        }
    }
    if (allowed != NULL) {
        *allowed = (value != NULL) &&
            (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 ||
             strcmp(value, "yes") == 0);
    }
    return true;
}

/* --- Streams --- */

/*
 * One connection.
 *
 * The send buffer is fixed and small. A stream that lets a caller queue
 * without limit turns a slow network into memory that grows until something
 * dies; refusing is honest backpressure, and the caller is told so it can
 * send less rather than retry harder.
 */
struct recon_net_stream {
    bool used;
    int fd;
    bool connected;

    /*
     * The encryption, when there is any.
     *
     * `tls` is NULL for a plain stream and everything below behaves as it
     * always did. When it is set, `connected` means the TCP connection is up
     * and the handshake is still running -- `opened` does not fire until
     * `secure` is true, so nothing above ever writes a password into a socket
     * that has not finished proving who is on the other end.
     */
    struct recon_tls_conn *tls;
    bool secure;
    char hostname[256];

    char application[64];
    char peer[192];

    struct wl_event_source *source;
    struct wl_event_source *deadline;

    struct recon_net_stream_handlers handlers;
    void *user;

    char outgoing[STREAM_BUFFER];
    size_t outgoing_used;

    size_t sent;
    size_t received;
    struct timespec started;

    /* Set while a handler is running, so a handler that closes its own
     * stream does not free the thing it is standing on. */
    bool in_handler;
    bool close_wanted;
};

static struct recon_net_stream g_streams[STREAMS_MAX];

int recon_net_stream_count(void) {
    int count = 0;
    for (int i = 0; i < STREAMS_MAX; i++) {
        if (g_streams[i].used) {
            count++;
        }
    }
    return count;
}

/* Take a stream down. `reason` is reported only when somebody else caused
 * it -- a close the owner asked for is not news to the owner. */
static void stream_end(struct recon_net_stream *stream,
        enum recon_net_result reason, bool tell) {
    if (!stream->used) {
        return;
    }

    if (stream->in_handler) {
        /* Deferred: a handler is on the stack and the stream is what it is
         * standing on. Torn down when that handler returns. */
        stream->close_wanted = true;
        return;
    }

    void (*closed)(void *, struct recon_net_stream *, enum recon_net_result) =
        tell ? stream->handlers.closed : NULL;
    void *user = stream->user;

    if (stream->source != NULL) {
        wl_event_source_remove(stream->source);
    }
    if (stream->deadline != NULL) {
        wl_event_source_remove(stream->deadline);
    }

    /*
     * The TLS connection owns the descriptor once it exists -- closing it says
     * goodbye properly and closes the socket -- so closing here as well would
     * be a double close, which in a process with an event loop means closing
     * whatever descriptor got that number next.
     */
    if (stream->tls != NULL) {
        recon_tls_close(stream->tls);
    } else if (stream->fd >= 0) {
        close(stream->fd);
    }

    /*
     * Cleared before the callback rather than after. A handler that opens
     * another stream would otherwise be handed this slot while it still
     * looks occupied, and one that looks at the handle it was given would
     * see a half-dismantled stream.
     */
    struct recon_net_stream copy = *stream;
    memset(stream, 0, sizeof(*stream));

    if (closed != NULL) {
        closed(user, &copy, reason);
    }
}

void recon_net_stream_close(struct recon_net_stream *stream) {
    if (stream != NULL) {
        stream_end(stream, RECON_NET_OK, false);
    }
}

bool recon_net_stream_stats(struct recon_net_stream *stream, size_t *sent,
        size_t *received, int *age_ms) {
    if (stream == NULL || !stream->used) {
        return false;
    }
    if (sent != NULL) {
        *sent = stream->sent;
    }
    if (received != NULL) {
        *received = stream->received;
    }
    if (age_ms != NULL) {
        *age_ms = elapsed_since(&stream->started);
    }
    return true;
}

bool recon_net_stream_send(struct recon_net_stream *stream, const char *bytes,
        size_t length) {
    if (stream == NULL || !stream->used || bytes == NULL) {
        set_error("nothing to send it on");
        return false;
    }
    if (length == 0) {
        return true;
    }
    if (stream->outgoing_used + length > sizeof(stream->outgoing)) {
        set_error("the send buffer is full");
        return false;
    }

    memcpy(stream->outgoing + stream->outgoing_used, bytes, length);
    stream->outgoing_used += length;

    /* Wanting to write means wanting to hear about writability again. */
    if (stream->source != NULL) {
        wl_event_source_fd_update(stream->source,
            WL_EVENT_READABLE | WL_EVENT_WRITABLE);
    }
    return true;
}

bool recon_net_stream_send_text(struct recon_net_stream *stream,
        const char *text) {
    if (text == NULL) {
        return true;
    }
    return recon_net_stream_send(stream, text, strlen(text));
}

/* Push out whatever is queued that the socket will take. */
static bool stream_flush(struct recon_net_stream *stream) {
    while (stream->outgoing_used > 0) {
        ssize_t written;

        if (stream->tls != NULL) {
            int rc = recon_tls_write(stream->tls, stream->outgoing,
                stream->outgoing_used);
            if (rc == RECON_TLS_AGAIN) {
                return true;   /* Full for now, same as EAGAIN below. */
            }
            if (rc < 0) {
                set_error("%s", recon_tls_last_error());
                return false;
            }
            written = rc;
        } else {
            written = send(stream->fd, stream->outgoing,
                stream->outgoing_used, MSG_NOSIGNAL);
        }

        if (written > 0) {
            stream->sent += (size_t)written;
            stream->outgoing_used -= (size_t)written;
            memmove(stream->outgoing, stream->outgoing + written,
                stream->outgoing_used);
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;   /* Full for now; the loop will say when it is not. */
        }
        set_error("%s", strerror(errno));
        return false;
    }

    /* Nothing left to write, so stop asking to be told about writability --
     * a socket that is writable and has nothing to write would wake the loop
     * on every turn forever. */
    if (stream->source != NULL) {
        wl_event_source_fd_update(stream->source, WL_EVENT_READABLE);
    }
    return true;
}

static int stream_event(int fd, uint32_t mask, void *data) {
    struct recon_net_stream *stream = data;
    (void)fd;

    if ((mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0) {
        stream_end(stream, RECON_NET_UNREACHABLE, true);
        return 0;
    }

    /* The first writability is the connect finishing, not room to send. */
    if (!stream->connected && (mask & WL_EVENT_WRITABLE) != 0) {
        int error = 0;
        socklen_t length = sizeof(error);
        if (getsockopt(stream->fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0) {
            error = errno;
        }
        if (error != 0) {
            set_error("%s", strerror(error));
            stream_end(stream, RECON_NET_UNREACHABLE, true);
            return 0;
        }

        stream->connected = true;
        if (stream->deadline != NULL) {
            /* Connected, so the connect timeout has nothing left to guard. */
            wl_event_source_remove(stream->deadline);
            stream->deadline = NULL;
        }

        /*
         * An encrypted stream is not open yet. The TCP connection is up and
         * the handshake has not started, and `opened` firing here would let
         * the owner write a password into a socket that has proved nothing.
         */
        if (stream->hostname[0] != '\0') {
            stream->tls = recon_tls_client_begin(stream->fd, stream->hostname);
            if (stream->tls == NULL) {
                set_error("%s", recon_tls_last_error());
                stream_end(stream, RECON_NET_UNREACHABLE, true);
                return 0;
            }
        } else if (stream->handlers.opened != NULL) {
            stream->in_handler = true;
            stream->handlers.opened(stream->user, stream);
            stream->in_handler = false;
            if (stream->close_wanted) {
                stream->close_wanted = false;
                stream_end(stream, RECON_NET_OK, false);
                return 0;
            }
        }
    }

    /*
     * The handshake, one step per turn of the event loop.
     *
     * Stepped rather than looped: on a non-blocking socket WANT_READ means
     * "nothing has arrived", and calling again immediately would spin at full
     * speed with the desktop inside the loop. Each step says which way the
     * socket has to become ready, and that is what the loop is asked for.
     */
    if (stream->tls != NULL && !stream->secure) {
        switch (recon_tls_handshake_step(stream->tls)) {
        case RECON_TLS_STEP_WANT_READ:
            wl_event_source_fd_update(stream->source, WL_EVENT_READABLE);
            return 0;
        case RECON_TLS_STEP_WANT_WRITE:
            wl_event_source_fd_update(stream->source,
                WL_EVENT_READABLE | WL_EVENT_WRITABLE);
            return 0;
        case RECON_TLS_STEP_FAILED:
            set_error("%s", recon_tls_last_error());
            stream_end(stream, RECON_NET_UNTRUSTED, true);
            return 0;
        case RECON_TLS_STEP_DONE:
            break;
        }

        stream->secure = true;
        wl_event_source_fd_update(stream->source, WL_EVENT_READABLE);

        if (stream->handlers.opened != NULL) {
            stream->in_handler = true;
            stream->handlers.opened(stream->user, stream);
            stream->in_handler = false;
            if (stream->close_wanted) {
                stream->close_wanted = false;
                stream_end(stream, RECON_NET_OK, false);
                return 0;
            }
        }

        /* Whatever the owner queued from `opened` wants writing, and this
         * turn's writability has already been consumed by the handshake. */
        if (stream->outgoing_used > 0) {
            wl_event_source_fd_update(stream->source,
                WL_EVENT_READABLE | WL_EVENT_WRITABLE);
        }
        return 0;
    }

    if ((mask & WL_EVENT_WRITABLE) != 0 && !stream_flush(stream)) {
        stream_end(stream, RECON_NET_UNREACHABLE, true);
        return 0;
    }

    if ((mask & WL_EVENT_READABLE) != 0) {
        char buffer[4096];
        for (;;) {
            ssize_t got;

            if (stream->tls != NULL) {
                int rc = recon_tls_read(stream->tls, buffer, sizeof(buffer));
                if (rc == RECON_TLS_AGAIN) {
                    break;
                }
                if (rc < 0) {
                    set_error("%s", recon_tls_last_error());
                    stream_end(stream, RECON_NET_UNREACHABLE, true);
                    return 0;
                }
                got = rc;
            } else {
                got = recv(stream->fd, buffer, sizeof(buffer), 0);
            }

            if (got == 0) {
                /* The other end finished. Not a failure: a server that has
                 * said everything it has to say closes. */
                stream_end(stream, RECON_NET_OK, true);
                return 0;
            }
            if (got < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                set_error("%s", strerror(errno));
                stream_end(stream, RECON_NET_UNREACHABLE, true);
                return 0;
            }

            stream->received += (size_t)got;
            if (stream->handlers.received != NULL) {
                stream->in_handler = true;
                stream->handlers.received(stream->user, stream, buffer,
                    (size_t)got);
                stream->in_handler = false;
                if (stream->close_wanted) {
                    stream->close_wanted = false;
                    stream_end(stream, RECON_NET_OK, false);
                    return 0;
                }
            }
        }
    }

    return 0;
}

static int stream_expired(void *data) {
    stream_end(data, RECON_NET_TIMED_OUT, true);
    return 0;
}

/*
 * Both open calls come through here; `encrypted` is the only difference.
 *
 * One function rather than two that drift, because the firewall check, the
 * permission check, the resolution and the timeout are the same questions
 * whether or not the bytes are encrypted afterwards.
 */
static struct recon_net_stream *stream_open(const char *application,
        const char *host, int port, bool encrypted,
        const struct recon_net_stream_handlers *handlers, void *user);

struct recon_net_stream *recon_net_stream_open(const char *application,
        const char *host, int port,
        const struct recon_net_stream_handlers *handlers, void *user) {
    return stream_open(application, host, port, false, handlers, user);
}

struct recon_net_stream *recon_net_stream_open_tls(const char *application,
        const char *host, int port,
        const struct recon_net_stream_handlers *handlers, void *user) {
    char why[256];
    if (!recon_tls_can_connect(why, sizeof(why))) {
        set_error("%s", why);
        return NULL;
    }
    return stream_open(application, host, port, true, handlers, user);
}

static struct recon_net_stream *stream_open(const char *application,
        const char *host, int port, bool encrypted,
        const struct recon_net_stream_handlers *handlers, void *user) {
    if (g_loop == NULL) {
        set_error("networking is not up");
        return NULL;
    }
    if (host == NULL || *host == '\0' || port <= 0 || port > 65535) {
        set_error("a host and a port are needed");
        return NULL;
    }

    /*
     * The permission check, before anything is resolved or opened. Refusing
     * after a connection exists would be a race with itself.
     */
    recon_net_note_application(application);
    if (!recon_net_may_use(application)) {
        set_error("'%s' is not allowed to use the network",
            application != NULL ? application : "an unnamed application");
        return NULL;
    }

    /*
     * Then the firewall, which is a different question.
     *
     * The check above asks whether this program may use the network at all;
     * this one asks whether *this* connection is one the machine allows. A
     * program can be permitted and the port still shut.
     */
    char why[96];
    if (!recon_firewall_allows(RECON_FW_OUT, RECON_FW_TCP, port, application,
            why, sizeof(why))) {
        recon_error_raisef(NULL, RECON_ERR_G001, "out, tcp %d, %s -- %s",
            port, application != NULL ? application : "the system", why);
        set_error("the firewall blocked that (%s)", why);
        return NULL;
    }

    struct recon_net_stream *stream = NULL;
    for (int i = 0; i < STREAMS_MAX; i++) {
        if (!g_streams[i].used) {
            stream = &g_streams[i];
            break;
        }
    }
    if (stream == NULL) {
        set_error("too many connections already open");
        return NULL;
    }

    char service[16];
    snprintf(service, sizeof(service), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *found = NULL;
    int status = getaddrinfo(host, service, &hints, &found);
    if (status != 0 || found == NULL) {
        set_error("%s: %s", host, gai_strerror(status));
        return NULL;
    }

    int fd = socket(found->ai_family, found->ai_socktype | SOCK_NONBLOCK,
        found->ai_protocol);
    if (fd < 0) {
        set_error("cannot open a socket: %s", strerror(errno));
        freeaddrinfo(found);
        return NULL;
    }

    int connected = connect(fd, found->ai_addr, found->ai_addrlen);
    int reason = errno;
    freeaddrinfo(found);

    if (connected != 0 && reason != EINPROGRESS) {
        set_error("%s", strerror(reason));
        close(fd);
        return NULL;
    }

    memset(stream, 0, sizeof(*stream));
    stream->used = true;
    stream->fd = fd;
    stream->user = user;
    if (handlers != NULL) {
        stream->handlers = *handlers;
    }
    snprintf(stream->application, sizeof(stream->application), "%s",
        application != NULL ? application : "");
    snprintf(stream->peer, sizeof(stream->peer), "%s:%d", host, port);

    /*
     * The name as asked for, not the address it resolved to.
     *
     * This is what the certificate gets checked against, so it has to be the
     * name a person typed. Checking against the address would pass for
     * anything holding a certificate for that IP, which is the check not
     * happening.
     *
     * Empty for a plain stream, which is also how stream_event knows there is
     * a handshake to run.
     */
    if (encrypted) {
        snprintf(stream->hostname, sizeof(stream->hostname), "%s", host);
    }

    clock_gettime(CLOCK_MONOTONIC, &stream->started);

    stream->source = wl_event_loop_add_fd(g_loop, fd,
        WL_EVENT_READABLE | WL_EVENT_WRITABLE, stream_event, stream);
    stream->deadline = wl_event_loop_add_timer(g_loop, stream_expired, stream);

    if (stream->source == NULL || stream->deadline == NULL) {
        set_error("cannot watch the connection");
        stream_end(stream, RECON_NET_UNREACHABLE, false);
        return NULL;
    }

    /* Guards the connect only; removed once there is something on the other
     * end. A stream that is open and idle is not a stream that has failed. */
    wl_event_source_timer_update(stream->deadline, STREAM_CONNECT_MS);
    return stream;
}

void recon_net_finish(void) {
    for (int i = 0; i < STREAMS_MAX; i++) {
        if (g_streams[i].used) {
            /* Without telling anybody: whoever owned it is going away too. */
            g_streams[i].in_handler = false;
            stream_end(&g_streams[i], RECON_NET_NO_NETWORK, false);
        }
    }
    for (int i = 0; i < PROBES_MAX; i++) {
        if (g_probes[i].used) {
            /* Taken down without calling back: whoever asked is going away
             * too, and an answer delivered into a torn-down system is worse
             * than no answer. */
            g_probes[i].done = NULL;
            probe_finish(&g_probes[i], RECON_NET_NO_NETWORK);
        }
    }
    g_loop = NULL;
    g_interface_count = 0;
    g_nameserver_count = 0;
    g_gateway[0] = '\0';
}
