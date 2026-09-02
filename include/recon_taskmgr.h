/*
 * ReconOS Task Manager.
 *
 * A built-in window: the compositor draws it, so it does not depend on a
 * client toolkit or a healthy desktop to appear. The tool you reach for when
 * something has gone wrong should not need the thing that went wrong.
 *
 * Its frame comes from the built-in window framework, so its minimize,
 * maximize and close behave exactly as every other window's do.
 */

#ifndef RECON_TASKMGR_H
#define RECON_TASKMGR_H

struct recon_server;
struct recon_font;
struct recon_appwin;

struct recon_appwin *recon_taskmgr_create(struct recon_server *server,
    struct recon_font *font);

#endif
