/*
 * Feeding the system input as though it came from a mouse or a keyboard.
 *
 * --- Why this exists at all ---
 *
 * A desktop cannot be tested by reasoning about it. Every "the button does
 * nothing" report needs a way to press that button and watch what happens, and
 * asking a person to click while somebody else reads the code is not that.
 *
 * These go through **exactly the same entry points as real input** -- not a
 * shortcut past them -- so what they exercise is what somebody using the
 * machine exercises. `scripts/look.sh` drives a live headless desktop through
 * them and photographs the result, and most of the visual faults this project
 * has found were found that way rather than by reading.
 *
 * --- Why it is a header of its own ---
 *
 * These were in `include/recon_server.h`, which is the compositor: a wlroots
 * scene, a seat, an output layout, a cursor manager and a dozen
 * `wl_listener`s. The only caller is `src/recon_cmd.c`, which is 3,626 lines
 * of command interpreter with **one** mention of wayland in it, and that
 * include was most of what kept it off a compiler with no Linux underneath.
 *
 * Nothing here names a compositor type, and nothing here needs to. A pointer
 * position is two integers; a key is a symbol and a set of modifiers. Which is
 * the point: pretending to be a mouse is not a wayland idea, and on a machine
 * with no compositor at all the same four functions are still exactly what a
 * test harness wants.
 */

#ifndef RECON_INJECT_H
#define RECON_INJECT_H

#include <stdbool.h>
#include <stdint.h>

struct recon_server;

/* Put the pointer here. */
void recon_inject_pointer(struct recon_server *server, int x, int y);

/*
 * The buttons, named here rather than taken from Linux.
 *
 * `src/recon_cmd.c` included `<linux/input-event-codes.h>` for two constants,
 * and once the compositor's header was off it that include was the *only*
 * thing left keeping 3,630 lines of command interpreter off a compiler with no
 * Linux underneath. Two numbers holding a file about something else: the shape
 * `scripts/port-blockers.sh` was written to find.
 *
 * The values are Linux's, because they are also wayland's and also what the
 * ReconOS kernel reports -- `userland/include/sys/input.h` has the same three
 * as `RECON_BTN_*`, mirroring the kernel's header. Writing them down in three
 * places is a thing that can disagree, so `src/main.c` -- the one file that
 * sees both these and Linux's -- holds a `_Static_assert` on each. A
 * disagreement is a build failure rather than a right-click that does nothing.
 */
#define RECON_BUTTON_LEFT   0x110
#define RECON_BUTTON_RIGHT  0x111
#define RECON_BUTTON_MIDDLE 0x112

/* Press or release a mouse button, where the pointer is. */
void recon_inject_button(struct recon_server *server, uint32_t button,
    bool pressed);

/* Press a key, as a symbol with modifiers already applied. */
void recon_inject_key(struct recon_server *server, uint32_t sym,
    uint32_t modifiers);

/*
 * Turn the wheel where the pointer is.
 *
 * Positive is down, matching what the shell's own handler takes. Here for the
 * same reason the other three are: a path nothing can drive from outside is a
 * path that stops working without anybody noticing, which is exactly what had
 * happened to the Control Panel's lists.
 */
void recon_inject_scroll(struct recon_server *server, double delta);

/* Where the pointer is, so a test can check what it is about to click. */
void recon_pointer_position(struct recon_server *server, int *x, int *y);

#endif /* RECON_INJECT_H */
