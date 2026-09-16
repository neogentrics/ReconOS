/*
 * What a panel is, and how it reaches a screen.
 *
 * --- why this file exists -------------------------------------------------
 *
 * `src/recon_ui.c` is three thousand lines of drawing and about thirty lines
 * of wlroots, and the thirty were enough to keep the three thousand off a
 * compiler with no Linux underneath it. **A panel is a pixel buffer** --
 * `uint32_t *pixels`, ARGB8888, width by height -- and everything the desktop
 * draws, every widget, every title bar, every themed fill, writes into that
 * array and nothing else.
 *
 * What wlroots was doing was the last inch: handing those pixels to a scene
 * graph, and moving, raising and hiding the node that holds them. That is
 * *presentation*, and on ReconOS it is a write to the framebuffer device
 * instead.
 *
 * So the last inch is behind a table of function pointers. `recon_ui.c` keeps
 * the drawing and knows nothing about what shows it; `recon_ui_wlr.c` is the
 * wlroots presentation and is the only file of the two that cannot leave
 * Linux.
 *
 * The same shape as `struct recon_memory_source` in the allocator and the
 * make-a-line function the wrapper takes: **put the part that is tied to the
 * host behind a seam, and the part that can be wrong on the other side of
 * it.**
 */

#ifndef RECON_UI_INTERNAL_H
#define RECON_UI_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "recon_ui.h"

/*
 * The hit list and the panel are defined here rather than in recon_ui.c so
 * that the presentation half can hold a `struct recon_panel *` -- it needs
 * the pixels and the size, and nothing else.
 *
 * The definitions are the ones that were in recon_ui.c, moved unchanged.
 */

/*
 * The last inch: how a panel's pixels reach a screen, and where it sits.
 *
 * Every one of these may be NULL. A panel with no presentation at all is a
 * perfectly good drawing surface -- which is what the tests use, and what a
 * program that renders into memory and writes a file wants. **Nothing in the
 * drawing half checks whether it is being shown**, because a widget that
 * behaves differently when nobody is looking is a widget that cannot be
 * tested.
 */
struct recon_panel_present {
    /* The panel's pixels have changed; put them on the screen. Straight
     * alpha, which is what everything above works in -- converting to
     * whatever the screen wants is the presentation's job, because that is
     * where the two models meet. */
    void (*commit)(struct recon_panel *panel, void *state);

    void (*set_position)(void *state, int x, int y);
    void (*position)(void *state, int *x, int *y);
    void (*raise_to_top)(void *state);
    void (*set_enabled)(void *state, bool enabled);

    /* Let go of whatever `state` is. The pixels are not yours. */
    void (*destroy)(void *state);
};

struct recon_hit_region {
    int x, y, w, h;
    uint32_t id;

    /*
     * Drawn, and explains itself, and cannot be pressed.
     *
     * A disabled control still has a region so that pointing at it can say
     * why it is unavailable. What it must not do is answer a click, and
     * before this the only way to arrange that was for every application to
     * remember its own guard next to its own switch statement -- which is
     * the arrangement that had "Half" shrinking a picture that was already
     * at its floor in one place and refusing in another.
     */
    bool inert;

    /* What this thing is, for the tooltip. Empty for most regions: a control
     * whose label already says it needs nothing said twice. */
    char tip[80];
};

#define MAX_HIT_REGIONS 64

struct recon_panel {
    int width, height;
    uint32_t *pixels; /* ARGB8888, straight alpha, width*height */

    const struct recon_panel_present *present;
    void *present_state;

    struct recon_hit_region hits[MAX_HIT_REGIONS];
    size_t hit_count;

    /*
     * Which region the pointer is over, and which it went down on.
     *
     * Not cleared with the hit list. The regions are rebuilt on every repaint
     * and these are not: they describe the pointer, which does not stop being
     * where it is because the window redrew.
     */
    uint32_t hot;
    uint32_t held;
};

/*
 * A panel of a given size, with a way of showing it.
 *
 * `present` and `state` may both be NULL, which makes a surface that draws
 * and is never shown. Returns NULL and raises D-002 if there is no memory for
 * the pixels.
 */
struct recon_panel *recon_panel_wrap(int width, int height,
    const struct recon_panel_present *present, void *state);

/*
 * Where recon_ui says something went wrong.
 *
 * It used to call `wlr_log` directly, from inside the font code -- five calls,
 * and the only reason the drawing half needed a wayland header at all besides
 * the presentation. A drawing library that writes to a log its caller did not
 * ask for is a drawing library with an opinion about stderr, so the message
 * goes to whoever set a hook and nowhere otherwise.
 */
void recon_ui_set_log(void (*log)(bool error, const char *message));
void recon_ui_say(bool error, const char *format, ...);

#endif /* RECON_UI_INTERNAL_H */
