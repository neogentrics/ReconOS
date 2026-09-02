/*
 * ReconOS Calculator.
 *
 * Four-function arithmetic, by mouse or keyboard. Its purpose beyond being
 * useful is to prove the built-in window framework: it implements only its
 * own contents and keys, and gets a title bar, minimize, maximize, close,
 * dragging and taskbar presence without asking for any of them.
 */

#ifndef RECON_CALC_H
#define RECON_CALC_H

struct recon_server;
struct recon_font;
struct recon_appwin;

/* Returns the window, ready to show. */
struct recon_appwin *recon_calc_create(struct recon_server *server,
    struct recon_font *font);

#endif
