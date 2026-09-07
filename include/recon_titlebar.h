/*
 * Where the things on a title bar go.
 *
 * There are two title bars in ReconOS -- the one `recon_appwin` draws around
 * its own windows, and the one `recon_decor` draws around a client's -- and
 * until now each worked out its own layout. They used different insets and
 * different gaps, so a client window's buttons sat two pixels from where a
 * built-in window's did. recon_decor's own header says why that is wrong: "a
 * client window that looked *nearly* like a ReconOS window would be worse than
 * one that plainly does not; near-misses read as a fault rather than as a
 * difference."
 *
 * So the arithmetic lives here and both ask. That is worth more than the two
 * pixels: it means a skin that moves the buttons moves them on every window,
 * rather than on the windows whoever made the change remembered to look at.
 *
 * --- What a skin may say ---
 *
 * Which side the buttons are on, and which buttons there are. Close is always
 * one of them and is always the outermost -- see RECON_METRIC_BUTTONS. Neither
 * of those is a preference this file reads and hopes about: the side changes
 * the arithmetic below, and the close bit is put back here regardless of what
 * the skin asked for, because a window nobody can close is not a look.
 */

#ifndef RECON_TITLEBAR_H
#define RECON_TITLEBAR_H

#include <stdbool.h>

/*
 * The three, in the order they sit from the outside in.
 *
 * Close first, because close is outermost on whichever side they are. A layout
 * that put maximize in the corner would be one that closes windows by accident
 * from the other direction.
 */
enum recon_titlebar_button {
    RECON_TITLEBAR_CLOSE,
    RECON_TITLEBAR_MAXIMIZE,
    RECON_TITLEBAR_MINIMIZE,
    RECON_TITLEBAR_BUTTON_COUNT,
};

struct recon_titlebar_rect {
    int x, y, w, h;
};

struct recon_titlebar_layout {
    /*
     * Each button's rectangle. `w` is zero for one the skin left out, which is
     * how a caller knows not to draw it without needing to ask the skin a
     * second question and risk a different answer.
     */
    struct recon_titlebar_rect button[RECON_TITLEBAR_BUTTON_COUNT];

    /* The window's icon, and where its title begins and how much room it has
     * before it would run into the buttons. */
    struct recon_titlebar_rect icon;
    int text_x;
    int text_width;

    /*
     * The part of the bar that drags the window.
     *
     * Everything the buttons are not, which is why it is worked out here: a
     * caller that assumed "from the left edge to where the buttons start" was
     * right only while the buttons were on the right.
     */
    struct recon_titlebar_rect drag;
};

/*
 * Work out one title bar's layout.
 *
 * `width` is the bar's, `inset` the margin at each end, and `gap` the space
 * between buttons. The height and the button size come from the skin.
 *
 * `with_icon` is false for a bar that draws no icon, which gives the title its
 * room back rather than leaving a hole where a picture would have been.
 */
void recon_titlebar_layout(int width, int inset, int gap, bool with_icon,
    struct recon_titlebar_layout *out);

#endif /* RECON_TITLEBAR_H */
