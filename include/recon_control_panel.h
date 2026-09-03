/*
 * The Control Panel: accounts, appearance, reading, and what the system is.
 *
 * Built into ReconOS rather than shipped as a module, unlike the calculator.
 * That is deliberate: this is where somebody goes to repair a system, and an
 * application you need in order to fix things should not itself be a thing
 * that can fail to load.
 */

#ifndef RECON_CONTROL_PANEL_H
#define RECON_CONTROL_PANEL_H

struct recon_server;
struct recon_font;
struct recon_appwin;

struct recon_appwin *recon_control_panel_create(struct recon_server *server,
    struct recon_font *font);

#endif
