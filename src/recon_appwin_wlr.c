/*
 * The one thing a window needed a compositor for.
 *
 * `src/recon_appwin.c` is one thousand six hundred lines about what a window
 * *is* -- its frame, its title, its buttons, where its edges are, what a drag
 * on one of them means -- and not one of them is about wayland. It was held
 * off a compiler with no Linux under it by the function below, which forwards
 * one layer's answer to another, and by a second include used by nothing.
 *
 * That is the same shape as `recon_ui.c`'s twenty-eight lines holding three
 * thousand, and `recon_ui_wlr.c` is where those went. This is the same split
 * one layer up, and the file is deliberately this short: **if it grew, the
 * seam would be in the wrong place.**
 */

#include <wlr/types/wlr_scene.h>

#include "recon_appwin.h"
#include "recon_ui.h"

struct wlr_scene_node *recon_appwin_node(struct recon_appwin *win) {
    struct recon_panel *panel = recon_appwin_panel(win);

    return panel != NULL ? recon_panel_node(panel) : NULL;
}
