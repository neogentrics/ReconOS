/*
 * Where the wayland stops.
 *
 * Seven functions, and the only reason they are a file rather than six lines
 * in each caller is that this is the one place allowed to include
 * `recon_server.h` on their behalf. See that header and
 * `include/recon_server_facts.h` for why.
 */

#include "recon_server.h"
#include "recon_ui.h"
#include "recon_loop_wl.h"
#include "recon_server_facts.h"
#include "recon_clients.h"

/*
 * --- The windows the shell does not own ---
 *
 * A copy, front to back, rather than an iterator. `include/recon_clients.h`
 * gives the argument in full; the short version is that minimizing a window
 * sends it to the back of this list and focuses the next one, so a walk that
 * reads each window's successor after the body has run follows whatever the
 * body just did to the ordering. BG-210 is that fault, and this is the shape
 * that cannot have it.
 */
int recon_clients(struct recon_server *server, struct recon_toplevel **out,
        int max) {
    if (server == NULL || out == NULL || max <= 0) {
        return 0;
    }

    int count = 0;
    struct recon_toplevel *toplevel;

    wl_list_for_each(toplevel, &server->toplevels, link) {
        if (count >= max) {
            break;
        }
        out[count++] = toplevel;
    }
    return count;
}

int recon_client_count(struct recon_server *server) {
    if (server == NULL) {
        return 0;
    }
    return (int)wl_list_length(&server->toplevels);
}

struct recon_shell *recon_server_shell(struct recon_server *server) {
    return server != NULL ? server->shell : NULL;
}

struct recon_panel *recon_server_window_panel(struct recon_server *server,
        int width, int height) {
    if (server == NULL) {
        return NULL;
    }
    return recon_panel_create(server->layer_windows, width, height);
}

struct recon_panel *recon_server_system_panel(struct recon_server *server,
        int width, int height) {
    if (server == NULL) {
        return NULL;
    }
    return recon_panel_create(server->layer_system, width, height);
}

struct recon_panel *recon_server_chrome_panel(struct recon_server *server,
        int width, int height) {
    if (server == NULL) {
        return NULL;
    }
    return recon_panel_create(server->layer_chrome, width, height);
}

struct recon_panel *recon_server_background_panel(struct recon_server *server,
        int width, int height) {
    if (server == NULL) {
        return NULL;
    }
    return recon_panel_create(server->layer_background, width, height);
}

struct recon_loop *recon_server_loop(struct recon_server *server) {
    if (server == NULL || server->wl_display == NULL) {
        return NULL;
    }
    return recon_loop_from_wl(wl_display_get_event_loop(server->wl_display));
}

int recon_server_screen_width(const struct recon_server *server) {
    return server != NULL ? server->screen_width : 0;
}

int recon_server_screen_height(const struct recon_server *server) {
    return server != NULL ? server->screen_height : 0;
}
