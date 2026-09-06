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

/*
 * Open one Control Panel item directly, by the name on its tile.
 *
 * For anything that already knows which setting it is about. Clicking the
 * taskbar clock opened the Control Panel at its front page -- the right
 * application at the wrong page, and four more clicks for somebody who was
 * pointing at the thing they wanted to change.
 *
 * The page windows are the same ones the front page opens, so asking for a
 * page that is already up raises and focuses it rather than making a second
 * copy. False when there is no item by that name, so a caller can fall back to
 * the front page rather than doing nothing.
 */
bool recon_control_panel_open_named(struct recon_server *server,
    struct recon_font *font, const char *item);

#endif
