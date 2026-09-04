/*
 * The ReconOS control socket. See include/recon_control.h.
 *
 * A Unix socket carrying the same commands the terminal window sends, so a
 * running ReconOS can be examined without a person at the screen. Each
 * connection gets its own session, so a remote look does not disturb the
 * working directory of whoever is sitting in front of it.
 *
 * It is registered with the compositor's event loop rather than given a thread
 * of its own. An idle socket then costs exactly nothing, and commands run on
 * the same thread as everything else, so there is no shared state to protect.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wlr/util/log.h>

#include "recon_cmd.h"
#include "ReconOS.h"
#include "recon_control.h"
#include "recon_crypt.h"
#include "recon_error.h"
#include "recon_firewall.h"
#include "recon_fs.h"
#include "recon_server.h"

#define MAX_CLIENTS 8
#define READ_BUFFER 1024

struct control_client {
    struct recon_control *control;
    struct recon_cmd_session *session;
    struct wl_event_source *source;
    int fd;

    char pending[READ_BUFFER];
    size_t pending_used;

    /*
     * True until this connection has proved itself.
     *
     * Only ever set for one that arrived over the network. A connection on
     * the Unix socket has already proved itself by being able to open a file
     * that only its owner can open, and asking it for a key as well would be
     * a second lock on the same door.
     */
    bool needs_key;
    /* Where it came from, for the log when it fails. */
    char from[64];
};

struct recon_control {
    struct recon_server *server;
    struct wl_event_source *source;
    int fd;
    char path[256];

    /* The network listener, when it is open. -1 when it is not. */
    int net_fd;
    struct wl_event_source *net_source;
    int net_port;

    struct control_client *clients[MAX_CLIENTS];
};

static void client_close(struct control_client *client) {
    struct recon_control *control = client->control;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (control->clients[i] == client) {
            control->clients[i] = NULL;
            break;
        }
    }

    if (client->source != NULL) {
        wl_event_source_remove(client->source);
    }
    close(client->fd);
    recon_cmd_session_destroy(client->session);
    free(client);
}

static void send_all(int fd, const char *data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        ssize_t written = write(fd, data + sent, size - sent);
        if (written <= 0) {
            /* The reader has gone; nothing useful to do about it here. */
            return;
        }
        sent += (size_t)written;
    }
}

/* --- The key --- */

/*
 * Only its hash is kept.
 *
 * A file holding the key is a secret at rest, and a key that can be
 * regenerated in one command costs nothing to lose. So the key is shown once,
 * when it is made, and what stays on disk cannot be turned back into it.
 *
 * Salted and stretched with PBKDF2 like a password, because that is what it
 * is: a short secret somebody typed, which is exactly the thing a plain hash
 * of is worth guessing at.
 */
#define KEY_SALT_SIZE 16
#define KEY_HASH_SIZE 32
#define KEY_ITERATIONS 60000

/* Sixteen bytes as thirty-two hex characters. Long enough that guessing is
 * hopeless and short enough to read out loud once. */
#define KEY_BYTES 16

static bool read_key_file(uint8_t salt[KEY_SALT_SIZE],
        uint8_t hash[KEY_HASH_SIZE], uint32_t *iterations) {
    size_t size = 0;
    char *text = recon_fs_read("/", RECON_REMOTE_KEY_FILE, &size);
    if (text == NULL) {
        return false;
    }

    /* iterations, salt, hash -- one per line, hex for the two that are
     * bytes, so the file can be looked at without a tool. */
    char *save = NULL;
    char *first = strtok_r(text, "\n", &save);
    char *second = (first != NULL) ? strtok_r(NULL, "\n", &save) : NULL;
    char *third = (second != NULL) ? strtok_r(NULL, "\n", &save) : NULL;

    bool ok = false;
    if (third != NULL) {
        *iterations = (uint32_t)strtoul(first, NULL, 10);
        ok = (*iterations > 0) &&
             recon_from_hex(second, salt, KEY_SALT_SIZE) &&
             recon_from_hex(third, hash, KEY_HASH_SIZE);
    }

    free(text);
    return ok;
}

bool recon_control_has_key(void) {
    uint8_t salt[KEY_SALT_SIZE];
    uint8_t hash[KEY_HASH_SIZE];
    uint32_t iterations = 0;
    return read_key_file(salt, hash, &iterations);
}

bool recon_control_new_key(char *out, size_t size) {
    if (out == NULL || size < KEY_BYTES * 2 + 1) {
        return false;
    }

    uint8_t key[KEY_BYTES];
    uint8_t salt[KEY_SALT_SIZE];

    /*
     * Both from the operating system, and a failure here is a failure.
     * Filling in zeroes would produce a key that looks like a key and is the
     * same one on every machine.
     */
    if (!recon_random_bytes(key, sizeof(key)) ||
            !recon_random_bytes(salt, sizeof(salt))) {
        return false;
    }

    recon_to_hex(key, sizeof(key), out);

    uint8_t hash[KEY_HASH_SIZE];
    recon_pbkdf2_sha256(out, salt, sizeof(salt), KEY_ITERATIONS, hash,
        sizeof(hash));

    char salt_hex[KEY_SALT_SIZE * 2 + 1];
    char hash_hex[KEY_HASH_SIZE * 2 + 1];
    recon_to_hex(salt, sizeof(salt), salt_hex);
    recon_to_hex(hash, sizeof(hash), hash_hex);

    char text[256];
    int n = snprintf(text, sizeof(text), "%u\n%s\n%s\n", KEY_ITERATIONS,
        salt_hex, hash_hex);
    if (n <= 0) {
        return false;
    }

    return recon_fs_write("/", RECON_REMOTE_KEY_FILE, text, (size_t)n);
}

bool recon_control_key_matches(const char *offered) {
    uint8_t salt[KEY_SALT_SIZE];
    uint8_t stored[KEY_HASH_SIZE];
    uint32_t iterations = 0;

    if (offered == NULL || !read_key_file(salt, stored, &iterations)) {
        return false;
    }

    uint8_t hash[KEY_HASH_SIZE];
    recon_pbkdf2_sha256(offered, salt, sizeof(salt), iterations, hash,
        sizeof(hash));

    /* Constant time: how long the comparison took must not say how much of
     * the guess was right. */
    return recon_equal_constant_time(hash, stored, sizeof(hash));
}

/*
 * Run one complete line and answer it.
 *
 * Returns false when the connection is finished with -- a key that did not
 * match. The caller closes it, and the caller is the only place that may:
 * closing from in here freed the client that the read loop was standing on,
 * and the loop went on to touch it. That corrupted the heap quietly and the
 * process aborted on the *next* connection, which is exactly the shape of
 * fault that takes a day to find from the symptom.
 */
static bool handle_line(struct control_client *client, char *line) {
    /*
     * A connection that has not proved itself gets exactly one thing read
     * from it: the key. Not a command, not an error message naming a
     * command -- nothing that would let somebody find out what this is
     * before they are allowed to talk to it.
     */
    if (client->needs_key) {
        /* An empty line is the prompt being answered before anything was
         * typed. Wait rather than counting it as a wrong key. */
        while (*line == ' ') {
            line++;
        }
        if (*line == '\0') {
            return true;
        }

        if (!recon_control_key_matches(line)) {
            recon_error_raisef(NULL, RECON_ERR_G005, "from %s", client->from);

            const char *no = "That key is not right.\n";
            send_all(client->fd, no, strlen(no));
            return false;
        }

        client->needs_key = false;
        wlr_log(WLR_INFO, "ReconOS: remote connection from %s accepted",
            client->from);

        const char *greeting =
            RECONOS_NAME " " RECONOS_VERSION
            " remote connection. Type 'help' for commands.\n";
        send_all(client->fd, greeting, strlen(greeting));

        /*
         * And the line stops being a line here.
         *
         * Falling through ran the key as a command, which failed and said so
         * -- quoting the key back into the output, where it would sit in
         * whatever scrollback or log the other end keeps. A secret that has
         * been checked has done its job; it should not then be repeated.
         */
        line = (char *)"";
    }

    const char *output = recon_cmd_run(client->session, line);
    if (output != NULL && *output != '\0') {
        send_all(client->fd, output, strlen(output));
    }

    /* A marker the caller can wait for, so it knows a reply is complete
     * rather than guessing from timing. */
    char prompt[RECON_PATH_MAX + 32];
    int n = snprintf(prompt, sizeof(prompt), "[%s]$ ", recon_cmd_cwd(client->session));
    send_all(client->fd, prompt, (size_t)n);
    return true;
}

static int on_client_readable(int fd, uint32_t mask, void *data) {
    struct control_client *client = data;

    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        client_close(client);
        return 0;
    }

    char buffer[READ_BUFFER];
    ssize_t got = read(fd, buffer, sizeof(buffer));
    if (got <= 0) {
        client_close(client);
        return 0;
    }

    for (ssize_t i = 0; i < got; i++) {
        char c = buffer[i];

        if (c == '\n' || client->pending_used >= sizeof(client->pending) - 1) {
            client->pending[client->pending_used] = '\0';

            if (!handle_line(client, client->pending)) {
                client_close(client);
                return 0;
            }
            client->pending_used = 0;

            if (recon_cmd_should_exit(client->session)) {
                client_close(client);
                return 0;
            }
            continue;
        }
        if (c != '\r') {
            client->pending[client->pending_used++] = c;
        }
    }

    return 0;
}

/*
 * Take a connection, whichever way it arrived.
 *
 * One path for both, because the difference between them is two things and
 * neither is about what a connection can do once it is up: where it came
 * from, and whether it has to prove itself first.
 */
static void take_connection(struct recon_control *control, int client_fd,
        bool from_network, const char *from) {
    int slot = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (control->clients[i] == NULL) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        const char *busy = "ReconOS: too many control connections\n";
        send_all(client_fd, busy, strlen(busy));
        close(client_fd);
        return;
    }

    struct control_client *client = calloc(1, sizeof(*client));
    if (client == NULL) {
        close(client_fd);
        return;
    }

    client->control = control;
    client->fd = client_fd;
    client->needs_key = from_network;
    recon_text_copy(client->from, sizeof(client->from),
        from != NULL ? from : "the local machine");

    client->session = recon_cmd_session_create(control->server);
    if (client->session == NULL) {
        close(client_fd);
        free(client);
        return;
    }

    struct wl_event_loop *loop =
        wl_display_get_event_loop(control->server->wl_display);
    client->source = wl_event_loop_add_fd(loop, client_fd, WL_EVENT_READABLE,
        on_client_readable, client);
    control->clients[slot] = client;

    if (from_network) {
        /*
         * Nothing about what this is, and no prompt it could be mistaken
         * for. Somebody who has not proved themselves should not learn the
         * version number from the greeting.
         */
        const char *ask = "Key: ";
        send_all(client_fd, ask, strlen(ask));
        wlr_log(WLR_INFO, "ReconOS: remote connection from %s, asking for a "
            "key", client->from);
        return;
    }

    const char *greeting =
        RECONOS_NAME " " RECONOS_VERSION
        " control connection. Type 'help' for commands.\n";
    send_all(client_fd, greeting, strlen(greeting));
    handle_line(client, "");

    wlr_log(WLR_INFO, "ReconOS: control connection opened");
}

static int on_connection(int fd, uint32_t mask, void *data) {
    struct recon_control *control = data;
    (void)mask;

    int client_fd = accept(fd, NULL, NULL);
    if (client_fd < 0) {
        return 0;
    }

    take_connection(control, client_fd, false, NULL);
    return 0;
}

/*
 * A connection on the network port.
 *
 * The firewall is asked again here, not only when the port was opened. A rule
 * can be turned off while the listener is up, and the answer somebody expects
 * is that the rule takes effect -- not that it takes effect after a restart.
 */
static int on_network_connection(int fd, uint32_t mask, void *data) {
    struct recon_control *control = data;
    (void)mask;

    struct sockaddr_in address;
    socklen_t length = sizeof(address);

    int client_fd = accept(fd, (struct sockaddr *)&address, &length);
    if (client_fd < 0) {
        return 0;
    }

    char from[64];
    unsigned char *quad = (unsigned char *)&address.sin_addr.s_addr;
    snprintf(from, sizeof(from), "%u.%u.%u.%u", quad[0], quad[1], quad[2],
        quad[3]);

    char why[96];
    if (!recon_firewall_allows(RECON_FW_IN, RECON_FW_TCP, control->net_port,
            NULL, why, sizeof(why))) {
        recon_error_raisef(NULL, RECON_ERR_G001, "in, tcp %d, from %s -- %s",
            control->net_port, from, why);
        close(client_fd);
        return 0;
    }

    take_connection(control, client_fd, true, from);
    return 0;
}

struct recon_control *recon_control_create(struct recon_server *server,
        const char *socket_path) {
    const char *path = socket_path;
    if (path == NULL || *path == '\0') {
        path = getenv("RECONOS_CONTROL_SOCKET");
    }
    if (path == NULL || *path == '\0') {
        path = RECON_CONTROL_DEFAULT_PATH;
    }

    /*
     * A Unix socket address is 108 bytes at most, and the kernel truncates
     * anything longer without complaining. That would bind one path while
     * every other operation here -- the unlink, the chmod that restricts it,
     * the unlink on shutdown -- used the untruncated one, so the socket would
     * come up unprotected under a name nothing could clean up. Refuse instead.
     */
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        wlr_log(WLR_ERROR, "ReconOS: control socket path is too long "
            "(%zu bytes, limit %zu): %s",
            strlen(path), sizeof(addr.sun_path) - 1, path);
        return NULL;
    }

    struct recon_control *control = calloc(1, sizeof(*control));
    if (control == NULL) {
        return NULL;
    }
    control->server = server;
    snprintf(control->path, sizeof(control->path), "%s", path);

    control->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (control->fd < 0) {
        wlr_log(WLR_ERROR, "ReconOS: cannot create control socket: %s", strerror(errno));
        free(control);
        return NULL;
    }

    /* A socket left behind by a previous run would block binding. */
    unlink(control->path);

    memcpy(addr.sun_path, control->path, strlen(control->path));

    if (bind(control->fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        wlr_log(WLR_ERROR, "ReconOS: cannot bind '%s': %s",
            control->path, strerror(errno));
        close(control->fd);
        free(control);
        return NULL;
    }

    /*
     * Only the owner may connect.
     *
     * The socket carries no authentication -- anything that can connect can
     * run any ReconOS command, including deleting a tree and shutting the
     * system down -- so who may connect is decided entirely by this. Without
     * it the mode came from the umask, which on a normal machine leaves a
     * socket in /tmp that every other account on the box can drive.
     *
     * Set after bind, because the file does not exist until then, and before
     * listen, so there is no window in which it is both connectable and
     * open to everybody.
     */
    if (chmod(control->path, S_IRUSR | S_IWUSR) != 0) {
        wlr_log(WLR_ERROR, "ReconOS: cannot restrict '%s': %s",
            control->path, strerror(errno));
        close(control->fd);
        unlink(control->path);
        free(control);
        return NULL;
    }

    if (listen(control->fd, 4) != 0) {
        wlr_log(WLR_ERROR, "ReconOS: cannot listen on '%s': %s",
            control->path, strerror(errno));
        close(control->fd);
        unlink(control->path);
        free(control);
        return NULL;
    }

    struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
    control->net_fd = -1;

    control->source = wl_event_loop_add_fd(loop, control->fd, WL_EVENT_READABLE,
        on_connection, control);

    wlr_log(WLR_INFO, "ReconOS: control socket at %s", control->path);
    return control;
}

void recon_control_destroy(struct recon_control *control) {
    if (control == NULL) {
        return;
    }

    recon_control_stop_network(control);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (control->clients[i] != NULL) {
            client_close(control->clients[i]);
        }
    }

    if (control->source != NULL) {
        wl_event_source_remove(control->source);
    }
    close(control->fd);
    unlink(control->path);
    free(control);
}

/* --- The network port --- */

bool recon_control_network_listening(struct recon_control *control) {
    return control != NULL && control->net_fd >= 0;
}

int recon_control_network_port(struct recon_control *control) {
    return (control != NULL) ? control->net_port : 0;
}

void recon_control_stop_network(struct recon_control *control) {
    if (control == NULL || control->net_fd < 0) {
        return;
    }

    if (control->net_source != NULL) {
        wl_event_source_remove(control->net_source);
        control->net_source = NULL;
    }
    close(control->net_fd);
    control->net_fd = -1;

    /*
     * Connections already up are left alone.
     *
     * Closing the door is not the same as throwing out whoever is already
     * inside, and somebody turning remote access off in the middle of using
     * it remotely would otherwise cut themselves off mid-command with no way
     * back. The port is shut; nothing new arrives.
     */
    wlr_log(WLR_INFO, "ReconOS: remote access closed");
}

bool recon_control_listen_network(struct recon_control *control, int port,
        char *why_out, size_t why_size) {
    if (control == NULL) {
        return false;
    }
    if (port <= 0 || port > 65535) {
        recon_text_copy(why_out, why_size, "that is not a port");
        return false;
    }

    /*
     * A key first. A listener that accepts anything is not remote access, it
     * is an invitation -- and the failure would be silent, because it would
     * work.
     */
    if (!recon_control_has_key()) {
        recon_text_copy(why_out, why_size,
            "there is no key yet; 'remote key' makes one");
        return false;
    }

    /*
     * Then the firewall. Turning remote access on is not enough: the rule has
     * to allow it. That is the point of having a firewall rather than a
     * setting -- one place says what may be reached, and everything that
     * opens a port goes through it.
     */
    char why[96];
    if (!recon_firewall_allows(RECON_FW_IN, RECON_FW_TCP, port, NULL, why,
            sizeof(why))) {
        char note[192];
        snprintf(note, sizeof(note),
            "the firewall blocks incoming tcp %d (%s)", port, why);
        recon_text_copy(why_out, why_size, note);
        return false;
    }

    recon_control_stop_network(control);

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        recon_text_copy(why_out, why_size, "no socket could be made");
        return false;
    }

    /*
     * So a restart does not have to wait out the kernel's hold on the port.
     * Without this, stopping and starting remote access inside a couple of
     * minutes fails with "address already in use" -- which reads as a fault
     * in ReconOS rather than as the way TCP works.
     */
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        char note[128];
        snprintf(note, sizeof(note), "cannot listen on %d: %s", port,
            strerror(errno));
        recon_text_copy(why_out, why_size, note);
        close(fd);
        return false;
    }

    if (listen(fd, 4) != 0) {
        recon_text_copy(why_out, why_size, "the port would not open");
        close(fd);
        return false;
    }

    struct wl_event_loop *loop =
        wl_display_get_event_loop(control->server->wl_display);
    control->net_source = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE,
        on_network_connection, control);
    if (control->net_source == NULL) {
        recon_text_copy(why_out, why_size, "nothing to watch the port with");
        close(fd);
        return false;
    }

    control->net_fd = fd;
    control->net_port = port;

    recon_text_copy(why_out, why_size, "listening");
    wlr_log(WLR_INFO, "ReconOS: remote access listening on %d", port);
    return true;
}

const char *recon_control_path(struct recon_control *control) {
    return control != NULL ? control->path : "";
}
