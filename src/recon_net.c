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

#include "recon_net.h"
#include "recon_registry.h"

#define INTERFACES_MAX 16
#define NAMESERVERS_MAX 4
#define PROBES_MAX 8

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

void recon_net_finish(void) {
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
