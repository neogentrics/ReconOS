/*
 * ReconOS Task Manager.
 *
 * A window ReconOS draws itself rather than an application it launches. The
 * tool you reach for when the system is misbehaving should not depend on the
 * system behaving: if a program has wedged the desktop, a task manager that
 * needs that desktop to start is no use.
 *
 * Because the compositor draws it, its title bar and buttons are the first
 * window frame ReconOS renders on its own -- the groundwork for drawing every
 * window's frame later.
 */

#ifndef RECON_TASKMGR_H
#define RECON_TASKMGR_H

#include <stdbool.h>

struct recon_server;
struct recon_font;
struct recon_taskmgr;

struct recon_taskmgr *recon_taskmgr_create(struct recon_server *server,
    struct recon_font *font);
void recon_taskmgr_destroy(struct recon_taskmgr *taskmgr);

/* Show, hide, or toggle. Sampling only runs while visible. */
void recon_taskmgr_show(struct recon_taskmgr *taskmgr);
void recon_taskmgr_hide(struct recon_taskmgr *taskmgr);
void recon_taskmgr_toggle(struct recon_taskmgr *taskmgr);
bool recon_taskmgr_visible(struct recon_taskmgr *taskmgr);

/* Centre on a screen of this size. */
void recon_taskmgr_center(struct recon_taskmgr *taskmgr, int screen_w, int screen_h);

void recon_taskmgr_raise(struct recon_taskmgr *taskmgr);

/* True if the point falls on the window. */
bool recon_taskmgr_contains_point(struct recon_taskmgr *taskmgr, double lx, double ly);

/*
 * Offer a click. Returns true if consumed.
 *
 * Dragging the title bar is handled here too, which is why motion is reported
 * separately below.
 */
bool recon_taskmgr_handle_click(struct recon_taskmgr *taskmgr, double lx, double ly,
    bool pressed);

/* Report pointer motion so an in-progress title bar drag can follow it. */
void recon_taskmgr_handle_motion(struct recon_taskmgr *taskmgr, double lx, double ly);

/* Scroll the process list. */
void recon_taskmgr_handle_scroll(struct recon_taskmgr *taskmgr, double delta);

#endif
