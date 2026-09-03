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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wlr/util/log.h>

#include "recon_cmd.h"
#include "ReconOS.h"
#include "recon_control.h"
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
};

struct recon_control {
    struct recon_server *server;
    struct wl_event_source *source;
    int fd;
    char path[256];

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

/* Run one complete line and answer it. */
static void handle_line(struct control_client *client, char *line) {
    const char *output = recon_cmd_run(client->session, line);
    if (output != NULL && *output != '\0') {
        send_all(client->fd, output, strlen(output));
    }

    /* A marker the caller can wait for, so it knows a reply is complete
     * rather than guessing from timing. */
    char prompt[RECON_PATH_MAX + 32];
    int n = snprintf(prompt, sizeof(prompt), "[%s]$ ", recon_cmd_cwd(client->session));
    send_all(client->fd, prompt, (size_t)n);
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
            handle_line(client, client->pending);
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

static int on_connection(int fd, uint32_t mask, void *data) {
    struct recon_control *control = data;

    int client_fd = accept(fd, NULL, NULL);
    if (client_fd < 0) {
        return 0;
    }

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
        return 0;
    }

    struct control_client *client = calloc(1, sizeof(*client));
    if (client == NULL) {
        close(client_fd);
        return 0;
    }

    client->control = control;
    client->fd = client_fd;
    client->session = recon_cmd_session_create(control->server);
    if (client->session == NULL) {
        close(client_fd);
        free(client);
        return 0;
    }

    struct wl_event_loop *loop = wl_display_get_event_loop(control->server->wl_display);
    client->source = wl_event_loop_add_fd(loop, client_fd, WL_EVENT_READABLE,
        on_client_readable, client);
    control->clients[slot] = client;

    const char *greeting =
        RECONOS_NAME " " RECONOS_VERSION
        " control connection. Type 'help' for commands.\n";
    send_all(client_fd, greeting, strlen(greeting));
    handle_line(client, "");

    wlr_log(WLR_INFO, "ReconOS: control connection opened");
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
    control->source = wl_event_loop_add_fd(loop, control->fd, WL_EVENT_READABLE,
        on_connection, control);

    wlr_log(WLR_INFO, "ReconOS: control socket at %s", control->path);
    return control;
}

void recon_control_destroy(struct recon_control *control) {
    if (control == NULL) {
        return;
    }

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

const char *recon_control_path(struct recon_control *control) {
    return control != NULL ? control->path : "";
}
