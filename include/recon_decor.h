/*
 * A ReconOS title bar for a window ReconOS did not draw.
 *
 * A Wayland client draws its own decorations by default, and until now that is
 * what every client on ReconOS did: weston-terminal arrives with a grey
 * Wayland-ish frame, its own close button, and nothing about it in the skin
 * anybody chose. Two window managers in one screen, disagreeing about what a
 * window looks like.
 *
 * The xdg-decoration protocol is how a compositor says "I will draw that".
 * Saying it is one line; meaning it is this file. A compositor that claims the
 * decorations and then draws none leaves the client with no title bar at all,
 * which is worse than the mismatch it was meant to fix -- so the claim and the
 * drawing arrive together.
 *
 * This is a title bar, not a frame. The sides and the bottom are left to the
 * client's own edge, and resizing a client window is still the client's
 * business, because a border that can be dragged needs hit regions outside the
 * surface and that is a separate piece of work. What is here is what a person
 * reaches for: the window's name, somewhere to drag it by, and the three
 * buttons.
 */

#ifndef RECON_DECOR_H
#define RECON_DECOR_H

#include <stdbool.h>

struct recon_server;
struct recon_font;
struct recon_toplevel;
struct recon_decor;

/*
 * Build the title bar for a client window. NULL if it cannot be made, which
 * leaves the window undecorated rather than absent -- a window with no title
 * bar can still be reached from the taskbar and from Alt+Tab.
 */
struct recon_decor *recon_decor_create(struct recon_server *server,
    struct recon_font *font, struct recon_toplevel *toplevel);

void recon_decor_destroy(struct recon_decor *decor);

/*
 * Follow the window: position, width, focus, name.
 *
 * Called on every commit, because a client resizes itself and nothing else
 * tells us when. Cheap when nothing has changed -- it compares first and
 * redraws only when there is something different to draw.
 */
void recon_decor_update(struct recon_decor *decor);

/* Show or hide with the window, for minimizing and for desktops. */
void recon_decor_set_visible(struct recon_decor *decor, bool visible);

/*
 * A click in screen coordinates. True when the title bar took it, which is
 * also what stops the click reaching the client underneath.
 */
bool recon_decor_click(struct recon_decor *decor, double lx, double ly,
    bool pressed);

/* Pointer movement, for dragging and for highlighting a button. */
void recon_decor_motion(struct recon_decor *decor, double lx, double ly);

/* True while this decoration is being dragged, so the shell knows the pointer
 * belongs to it. */
bool recon_decor_dragging(struct recon_decor *decor);

/*
 * How much room the title bar needs above a window, in pixels. Zero when
 * there is no decoration -- so a caller can subtract it without first asking
 * whether there is one.
 *
 * Whatever places or resizes a client window has to leave this, or the bar
 * ends up off the top of the screen. A maximized window is the case that
 * showed it: filling the screen puts the window at y=0 and its title bar at
 * y=-30, which is to say nowhere.
 */
int recon_decor_reserved_top(struct recon_decor *decor);

#endif
