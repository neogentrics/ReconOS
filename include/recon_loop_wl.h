/*
 * The one function that crosses the loop seam in the wayland direction.
 *
 * `include/recon_loop.h` is the seam, and nothing above it may know that a
 * `wl_event_loop` exists -- that is the whole point of it, and the reason six
 * files and eight thousand lines now build with no Linux underneath. So this
 * declaration cannot live there.
 *
 * It lives here rather than being written out by hand in each file that needs
 * it. Two of them do, and two hand-written declarations of the same function
 * is where a signature quietly stops matching its definition: the compiler
 * checks each against the call, and neither against the other.
 *
 * Only `src/recon_loop_wl.c` and the files that hold a compositor's loop and
 * have to hand it out as ours should include this. Including it is a statement
 * that the file is below the seam.
 */

#ifndef RECON_LOOP_WL_H
#define RECON_LOOP_WL_H

#include <wayland-server-core.h>

#include "recon_loop.h"

/*
 * A compositor's loop, as one of ours.
 *
 * The same pointer, cast -- see the header of `src/recon_loop_wl.c` for why
 * there is no wrapper. Which means this is not a conversion so much as a
 * single place for the cast to be written down, and the reason a caller
 * should never write the cast itself.
 *
 * --- Why inline, which is not a performance argument ---
 *
 * `src/recon_net.c` holds a compositor's loop and hands it out as ours, and
 * two test binaries compile it **against the plain half of the seam** on
 * purpose: checking how an SNTP reply is read should not need a compositor
 * linked. Out of line, this function would be a symbol those two binaries
 * have no definition for, and the only ways to give them one are to link the
 * wayland half -- which defines `recon_timer_create` a second time -- or to
 * let each caller write the cast itself, which is what this file exists to
 * stop.
 *
 * Inline, there is no symbol to resolve and the cast is still written once.
 */
static inline struct recon_loop *recon_loop_from_wl(struct wl_event_loop *loop) {
    return (struct recon_loop *)loop;
}

#endif /* RECON_LOOP_WL_H */
