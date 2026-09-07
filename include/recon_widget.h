/*
 * The widget layer: one place that owns what a control looks like and how it
 * behaves when a person points at it.
 *
 * --- Why this exists ---
 *
 * Every application in ReconOS drew its own buttons. Not "drew them
 * differently" -- drew them *itself*, five or six lines at a time:
 *
 *     recon_fill_rect(p, x, y, w, h, COLOR_BG);
 *     recon_draw_button_edge(p, x, y, w, h, false, COLOR_BAR);
 *     recon_draw_text(p, font, x + 8, baseline, w, label, COLOR_TEXT);
 *     recon_hit_add(p, x, y, w, h, HIT_SOMETHING);
 *     recon_hit_tip(p, "What it does");
 *
 * That block appears around two hundred times across this repository. It is
 * the reason the Photos toolbar clipped its labels at the wrong width, the
 * reason the file dialog's Choose button was cut off, and the reason nothing
 * anywhere in the system highlights under the pointer: a behaviour that has to
 * be written two hundred times is a behaviour that is written zero times.
 *
 * The fix is not to write it more carefully. It is to write it once. An
 * application should say *this is a button, here, called that, and pressing it
 * means this*, and be told nothing about bevels, corner radii, hover tints or
 * hit regions -- because those are the system's business and not the
 * application's, and because a system where they are the application's
 * business is one where the look drifts apart faster than anyone can pull it
 * back together.
 *
 * --- Why it carries a version ---
 *
 * Modules are built separately from the system that loads them, and the
 * calculator is one. If what a button looks like can change, then a module
 * compiled against one generation of that answer and loaded by another has to
 * be a thing the system can *notice*, rather than a thing that shows up as a
 * window whose buttons are subtly the wrong shape.
 *
 * So this layer has a number, and it is not the module ABI's number.
 * RECON_MODULE_ABI answers "can this be loaded at all" -- it changes when a
 * struct changes shape and getting it wrong is a crash. RECON_WIDGET_VERSION
 * answers "which generation of the look is this drawn against" -- it changes
 * when the appearance or behaviour of a control changes, and getting it wrong
 * is a window that looks foreign. Two different questions with two different
 * failure modes, so two different numbers. docs/WIDGETS.md records what each
 * version changed.
 *
 * --- The state model ---
 *
 * A control is in exactly one of four states, and the panel knows which
 * without the application being asked:
 *
 *   NORMAL    nothing is happening to it
 *   HOT       the pointer is over it
 *   ACTIVE    the pointer is over it and the button is down
 *   DISABLED  it is drawn, and explains itself, and cannot be pressed
 *
 * Two ids on the panel carry this: the *hot* id, set from pointer motion, and
 * the *held* id, set when a press lands and cleared when it is released. Both
 * survive a redraw -- they are input state, not drawing state, which is why
 * recon_hit_clear does not touch them.
 *
 * That is the whole mechanism. It is deliberately not a retained widget tree
 * with parents and invalidation: ReconOS redraws a panel from scratch whenever
 * anything about it changes, and a tree would be a second description of the
 * window that could disagree with the first.
 *
 * --- What this layer will not do ---
 *
 * It does not lay controls out. Where a button goes is a question about the
 * window it is in, and a layout engine here would have to be told about every
 * window in the system to be any good at it. Applications keep their own
 * arithmetic and pass rectangles.
 */

#ifndef RECON_WIDGET_H
#define RECON_WIDGET_H

#include <stdbool.h>
#include <stdint.h>

#include "recon_theme.h"
#include "recon_ui.h"

/*
 * Which generation of the look this is.
 *
 * Raised when the appearance or behaviour of a control changes in a way
 * somebody could see. Not raised for a bug fix that makes a control finally do
 * what this file already says it does.
 */
#define RECON_WIDGET_VERSION 2

/* What is happening to a control right now. */
enum recon_widget_state {
    RECON_WIDGET_NORMAL,
    RECON_WIDGET_HOT,
    RECON_WIDGET_ACTIVE,
    RECON_WIDGET_DISABLED,
};

/*
 * What a control is *for*, which is what decides how it reacts.
 *
 * Not a colour and not a style name. A caller says what the button means and
 * the skin decides what that looks like, because the alternative is every
 * application choosing its own red for delete and the system having four of
 * them.
 */
enum recon_widget_look {
    /* The ordinary button. */
    RECON_WIDGET_PLAIN,

    /*
     * The one pressing Return would press. Carries the accent colour at rest,
     * so it reads as the default without a second border around it.
     */
    RECON_WIDGET_ACCENT,

    /*
     * Close, delete, end task. Ordinary at rest and warning-coloured under the
     * pointer -- which is the convention every desktop settled on, and worth
     * following precisely because it is a convention: somebody who has learnt
     * that the red one is the one that loses their work has learnt it here
     * too.
     */
    RECON_WIDGET_DANGER,

    /*
     * One of a strip. Sits flush with its neighbours and shows which is
     * chosen through `checked` rather than through being pressed.
     */
    RECON_WIDGET_TAB,
};

/*
 * "Whatever the skin says."
 *
 * Zero is fully transparent black, which is not a colour anybody means for a
 * button, so it is free to mean this instead.
 */
#define RECON_WIDGET_SKIN ((recon_color)0)

struct recon_widget_button {
    int x, y, w, h;

    /*
     * The hit id, or RECON_HIT_NONE for a control that is drawn and not
     * pressable. A button with no id registers no hit region and can never be
     * hot, which is how a purely decorative one stays quiet under the pointer.
     */
    uint32_t id;

    /* Centred in the button, clipped at its edge. NULL draws no label, which
     * is what a button carrying only a glyph wants. */
    const char *label;
    struct recon_font *font;

    /* Shown on hover after a moment. NULL for a button whose label already
     * says what it does. */
    const char *tip;

    /*
     * What the rounded corner is filled back to.
     *
     * A button sits on something already painted, so its corner is not cleared
     * but returned to whatever was behind it. Getting this wrong is the one
     * mistake that shows: a corner filled with the wrong colour reads as a
     * chip out of the button.
     */
    recon_color behind;

    /* RECON_WIDGET_SKIN for the skin's own, which is almost always right. */
    recon_color fill;
    recon_color text;

    enum recon_widget_look look;

    /* Drawn, explained, and not pressable. Registers its hit region anyway, so
     * the tooltip can say why. */
    bool disabled;

    /* A tab or a toggle that is on. Ignored by RECON_WIDGET_PLAIN. */
    bool checked;
};

/*
 * Draw one button and register it.
 *
 * Does the fill, the bevel, the skin's corner radius, the hover and press
 * treatment, the label, the hit region and the tooltip -- all of it, in the
 * order that makes them compose, which is the order this file exists to stop
 * two hundred call sites getting individually wrong.
 *
 * Returns the state it drew in, for a caller that wants to draw its own glyph
 * on top and match.
 */
enum recon_widget_state recon_widget_button(struct recon_panel *panel,
    const struct recon_widget_button *button);

/*
 * The same, for the minimize/maximize/close buttons on a title bar.
 *
 * Separate because they are the one control whose surroundings are not a
 * surface but a title bar, whose colours come from a different set of roles,
 * and which have to be drawn before a font has loaded. `glyph` picks which of
 * the three shapes goes inside; the caller has already worked out where.
 */
enum recon_widget_caption {
    RECON_WIDGET_CAPTION_MINIMIZE,
    RECON_WIDGET_CAPTION_MAXIMIZE,
    RECON_WIDGET_CAPTION_RESTORE,
    RECON_WIDGET_CAPTION_CLOSE,
};

enum recon_widget_state recon_widget_caption_button(struct recon_panel *panel,
    int x, int y, int size, uint32_t id, enum recon_widget_caption glyph,
    recon_color behind, const char *tip);

/*
 * A selection or a hover highlight, rounded the way this skin's controls are.
 *
 * Every one of these was a plain rectangle: the selected file on the desktop,
 * the chosen tile in the Control Panel, the row under the pointer in a list,
 * the menu entry being pointed at. On a skin where nothing else in the window
 * has a square corner, a square highlight is the one shape that does not
 * belong -- and there were around thirty of them, each written out where it
 * was needed, which is the same arrangement that left nothing reacting to the
 * pointer.
 *
 * The radius is the button radius for a control of that size, so a highlight
 * and the thing it is highlighting round by the same amount. A skin that asks
 * for square gets square here too, without this being told.
 */
void recon_widget_highlight(struct recon_panel *panel, int x, int y, int w,
    int h, recon_color color);

/*
 * The same, taking a skin role rather than a colour, for the callers that
 * were using recon_fill_role.
 */
void recon_widget_highlight_role(struct recon_panel *panel, int x, int y,
    int w, int h, enum recon_theme_role role);

/* --- Interaction state --- */

/*
 * Say where the pointer is.
 *
 * Returns true when the answer changed, which is the caller's signal to
 * redraw. Returning it rather than redrawing here keeps this layer out of the
 * business of knowing what a window is.
 */
bool recon_widget_hover(struct recon_panel *panel, uint32_t id);

/* Say that the pointer went down on something, and that it came back up. */
bool recon_widget_press(struct recon_panel *panel, uint32_t id);
bool recon_widget_release(struct recon_panel *panel);

uint32_t recon_widget_hot_id(const struct recon_panel *panel);
uint32_t recon_widget_held_id(const struct recon_panel *panel);

/*
 * What state a control with this id is in.
 *
 * Exposed for the handful of things that draw themselves and still want to
 * react -- a list row, a menu entry -- so they do not each invent their own
 * answer to "is the pointer on me".
 */
enum recon_widget_state recon_widget_state_of(const struct recon_panel *panel,
    uint32_t id, bool disabled);

/*
 * The same rule with the panel taken out of it.
 *
 * The state a control is in is a question about three numbers, and answering
 * it needs no window, no skin and no screen. Separating it is what lets the
 * rules above -- held-but-wandered-off, nothing-else-lights-up -- be checked
 * by a test rather than by looking at a photograph and deciding it seems
 * right.
 */
enum recon_widget_state recon_widget_state_from(uint32_t hot, uint32_t held,
    uint32_t id, bool disabled);

/*
 * The skin's colour for a surface in this state.
 *
 * `base` is what it looks like at rest. Hover moves it toward the selection
 * colour and a press moves it further, so a skin that has said what selection
 * looks like has already said what hover looks like -- including a skin
 * somebody made this afternoon, which is the point of deriving it rather than
 * adding a role that every custom skin would leave unset.
 */
recon_color recon_widget_surface(recon_color base,
    enum recon_widget_state state);

#endif /* RECON_WIDGET_H */
