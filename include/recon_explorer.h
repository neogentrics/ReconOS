/*
 * ReconOS File Explorer.
 *
 * A native application: it ships with the system rather than being installed
 * into it. It browses the ReconOS filesystem only -- there is no path it can
 * be given that reaches the host, because from inside ReconOS no such path
 * exists.
 */

#ifndef RECON_EXPLORER_H
#define RECON_EXPLORER_H

struct recon_server;
struct recon_font;
struct recon_appwin;

struct recon_appwin *recon_explorer_create(struct recon_server *server,
    struct recon_font *font);

/*
 * Show a particular folder.
 *
 * Opening a folder from the desktop used to launch the explorer and leave it
 * wherever it happened to be, which looks exactly like the folder not
 * opening. Safe to call on any window: it checks it is really the explorer.
 */
void recon_explorer_open_at(struct recon_appwin *win, const char *path);

#endif
