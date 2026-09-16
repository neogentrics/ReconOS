/*
 * Where the wayland stops.
 *
 * Four functions, and the only reason they are a file rather than four lines
 * in each caller is that this is the one place allowed to include
 * `recon_server.h` on their behalf. See that header and
 * `include/recon_server_facts.h` for why.
 */

#include "recon_server.h"
#include "recon_ui.h"
#include "recon_server_facts.h"

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

int recon_server_screen_width(const struct recon_server *server) {
    return server != NULL ? server->screen_width : 0;
}

int recon_server_screen_height(const struct recon_server *server) {
    return server != NULL ? server->screen_height : 0;
}
