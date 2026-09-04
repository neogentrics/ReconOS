/*
 * ReconOS UI layer.
 *
 * The drawing foundation the shell is built on: panels, text, and click
 * targets. Everything the desktop draws itself -- the taskbar, menus, window
 * frames -- is built from these, so all of it can be restyled in one place.
 *
 * A panel is a block of pixels ReconOS renders into directly and hands to the
 * scene graph as a buffer. Widgets are drawn into that buffer, not created as
 * scene nodes, so a whole panel costs the compositor a single texture no
 * matter how many things are on it.
 */

#ifndef RECON_UI_H
#define RECON_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <xkbcommon/xkbcommon.h>

struct wlr_scene_tree;
struct wlr_scene_buffer;

/* --- Colors --- */

/*
 * Packed 0xAARRGGBB. Named rather than numeric at the call site so a skin can
 * be swapped by changing a palette instead of hunting for literals.
 */
typedef uint32_t recon_color;

#define RECON_RGB(r, g, b)     ((recon_color)(0xFF000000u | ((r) << 16) | ((g) << 8) | (b)))
#define RECON_RGBA(r, g, b, a) ((recon_color)(((a) << 24) | ((r) << 16) | ((g) << 8) | (b)))

/* --- Text --- */

struct recon_font;

/*
 * Load a font for rendering at the given pixel height.
 *
 * Pass NULL for path to search the usual system font locations. Returns NULL
 * if no usable font was found; callers must cope, because a desktop that
 * refuses to start over a missing font is worse than one without labels.
 */
struct recon_font *recon_font_load(const char *path, int pixel_height);
void recon_font_destroy(struct recon_font *font);

/* --- Reading --- */

/*
 * How text is spaced out, for readers who need it spaced out.
 *
 * Extra space between letters is the best-supported thing a system can do for
 * a dyslexic reader -- more so than a special typeface, which is popular but
 * whose advantage has not held up in controlled study. Extra space between
 * lines and a font of the reader's choosing are here for the same reason: they
 * are the adjustments with evidence behind them.
 *
 * Applied everywhere text is drawn or measured, so nothing has to opt in and
 * nothing can forget. Measuring and drawing go through the same numbers, so a
 * spaced-out label still truncates in the right place.
 */
void recon_text_set_spacing(int letter, int line);
int recon_text_letter_spacing(void);
int recon_text_line_spacing(void);

/*
 * Swap a loaded font for another, in place.
 *
 * The pointer stays valid, which is what lets the font change while the system
 * is running: every window holds this pointer, and handing out a new one would
 * mean finding all of them. False leaves the existing font untouched, because
 * a desktop with no font is worse than one with the wrong font.
 */
bool recon_font_reload(struct recon_font *font, const char *path,
    int pixel_height);

/* Width in pixels the string would occupy. */
int recon_text_width(struct recon_font *font, const char *text);

/* Distance from the top of a line to the baseline. */
int recon_font_ascent(struct recon_font *font);

/* Line height, including the gap between lines. */
int recon_font_line_height(struct recon_font *font);

/* --- Panels --- */

/*
 * A rectangle of pixels ReconOS draws into and shows via the scene graph.
 *
 * Drawing calls only touch the pixel buffer; nothing reaches the screen until
 * recon_panel_commit(), so a redraw can be assembled without the display
 * showing it half-finished.
 */
struct recon_panel;

struct recon_panel *recon_panel_create(struct wlr_scene_tree *parent,
    int width, int height);
void recon_panel_destroy(struct recon_panel *panel);

/* Resize the panel's pixel buffer. Contents are discarded. */
bool recon_panel_resize(struct recon_panel *panel, int width, int height);

/* Publish whatever has been drawn. */
void recon_panel_commit(struct recon_panel *panel);

void recon_panel_set_position(struct recon_panel *panel, int x, int y);

/* Where the panel currently sits, so what is drawn can be located from
 * outside without keeping a second copy of the position. */
void recon_panel_position(const struct recon_panel *panel, int *x, int *y);
void recon_panel_raise_to_top(struct recon_panel *panel);
void recon_panel_set_enabled(struct recon_panel *panel, bool enabled);

int recon_panel_width(const struct recon_panel *panel);
int recon_panel_height(const struct recon_panel *panel);

/* The panel's node, for scene ordering and hit-test identification. */
struct wlr_scene_node *recon_panel_node(struct recon_panel *panel);

/* --- Drawing --- */

void recon_fill(struct recon_panel *panel, recon_color color);
void recon_fill_rect(struct recon_panel *panel, int x, int y, int w, int h,
    recon_color color);

/*
 * A vertical ramp from one colour at the top edge to another at the bottom.
 *
 * Vertical only, because every surface that wants one here is horizontal --
 * title bars, the taskbar, a menu header. A horizontal ramp would be a second
 * mechanism serving nothing.
 *
 * Alpha is not interpolated: both ends are drawn opaque. Everything this
 * paints is chrome sitting on its own panel, and a half-transparent title bar
 * is not a thing any skin should be able to ask for by accident.
 */
void recon_fill_gradient(struct recon_panel *panel, int x, int y, int w, int h,
    recon_color from, recon_color to);

/*
 * Round the top two corners of a panel, clearing what falls outside the curve
 * and redrawing the outline along it.
 *
 * Only the top two: the bottom corners of a window sit against whatever is
 * below them, and rounding those reads as a gap rather than as a shape.
 *
 * The cleared pixels are fully transparent rather than filled with a
 * background colour, because a window sits over the wallpaper and over other
 * windows -- there is no one colour a corner could be painted that would be
 * right anywhere but where it was chosen.
 *
 * Call it last. Anything drawn into the corner afterwards puts the square
 * back.
 */
void recon_round_top_corners(struct recon_panel *panel, int radius,
    recon_color edge);

/*
 * Round all four corners of a rectangle already drawn into the panel.
 *
 * Unlike a window's top corners, the corner is filled with `behind` rather
 * than cleared: a button sits on a panel that has already been painted, and
 * punching a hole in it would show the wallpaper through the middle of a
 * title bar.
 */
void recon_round_rect(struct recon_panel *panel, int x, int y, int w, int h,
    int radius, recon_color behind);

/* A one-pixel outline just inside the given rectangle. */
void recon_stroke_rect(struct recon_panel *panel, int x, int y, int w, int h,
    recon_color color);

/*
 * A raised or sunken bevel, the Windows 95 look: light on the top and left,
 * dark on the bottom and right, or the reverse when pressed.
 */
void recon_draw_bevel(struct recon_panel *panel, int x, int y, int w, int h,
    bool pressed);

/*
 * The edge every button shares: the bevel, then the skin's corner radius
 * rounded off it, filled back to `behind`.
 *
 * Use this rather than recon_draw_bevel wherever the rectangle is a button a
 * person can press. A bevel on its own is right for a sunken text field or a
 * panel's own outline, and those should keep calling the bevel directly --
 * they are not buttons and do not round.
 */
void recon_draw_button_edge(struct recon_panel *panel, int x, int y, int w,
    int h, bool pressed, recon_color behind);

/*
 * Draw text with its left edge at x and its baseline at y.
 *
 * Clipped to max_width, with a trailing ellipsis when it doesn't fit, so long
 * window titles truncate instead of overrunning their button.
 */
void recon_draw_text(struct recon_panel *panel, struct recon_font *font,
    int x, int y, int max_width, const char *text, recon_color color);

/*
 * Draw RGBA pixels into a rectangle, scaling to fit and blending by alpha.
 *
 * Nearest-neighbour, which suits icons: they are drawn at or near their own
 * size, and smoothing a 16-colour icon costs more than it gains.
 */
void recon_draw_image(struct recon_panel *panel, int x, int y, int w, int h,
    const unsigned char *rgba, int image_width, int image_height);

/* --- Text entry --- */

/*
 * A single line of editable text.
 *
 * Renaming a file, naming one to save, typing a path: all the same thing, so
 * they share one editor rather than each growing its own. The caller owns the
 * struct and decides where it appears; this only knows about the text and the
 * caret.
 */

#define RECON_EDIT_MAX 256

struct recon_edit {
    char text[RECON_EDIT_MAX];
    int length;
    int caret;
    /*
     * The other end of the selection, or -1 for none. Typing over a selection
     * replaces it, which is what makes renaming work the way it does
     * everywhere else: the name arrives selected, and the first key replaces
     * it instead of being appended to it.
     */
    int anchor;
    bool active;

    /* Draw dots instead of the characters. For a password, where the point is
     * that somebody behind you cannot read it. The text itself is untouched --
     * masking is about what is shown, not what is stored. */
    bool masked;
};

/*
 * Start editing `initial`, with it selected so typing replaces it.
 *
 * When `select_stem` is true the selection stops before the last dot, so
 * renaming "notes.txt" and typing replaces the name and keeps the extension --
 * retyping ".txt" every time is the kind of small tax that makes a feature
 * feel unfinished. For a name with no extension, or when false, the whole
 * thing is selected.
 */
void recon_edit_begin(struct recon_edit *edit, const char *initial,
    bool select_stem);
void recon_edit_end(struct recon_edit *edit);

enum recon_edit_result {
    RECON_EDIT_IGNORED,  /* Not a key the editor uses. */
    RECON_EDIT_CHANGED,  /* Text or caret moved; redraw. */
    RECON_EDIT_COMMIT,   /* Enter: the caller should apply edit->text. */
    RECON_EDIT_CANCEL,   /* Escape: the caller should discard it. */
};

enum recon_edit_result recon_edit_key(struct recon_edit *edit,
    xkb_keysym_t sym, uint32_t modifiers);

/*
 * Draw the field, sunken, with a caret. Long text scrolls so the caret stays
 * visible rather than running off the end of the box.
 */
void recon_edit_draw(struct recon_panel *panel, struct recon_font *font,
    int x, int y, int w, int h, const struct recon_edit *edit);

/* --- Click targets --- */

/*
 * Panels are opaque to the compositor, so it cannot tell which part of one was
 * clicked. Each panel keeps a list of regions with an id attached; a click is
 * resolved to an id and the owner decides what it means.
 */
#define RECON_HIT_NONE 0

void recon_hit_clear(struct recon_panel *panel);
bool recon_hit_add(struct recon_panel *panel, int x, int y, int w, int h,
    uint32_t id);

/*
 * Enumerate the regions a panel registered, so what is clickable can be found
 * from outside rather than by measuring a screenshot. Returns false once
 * `index` runs past the end.
 */
bool recon_hit_region(const struct recon_panel *panel, size_t index,
    int *x, int *y, int *w, int *h, uint32_t *id);

/* The id of the topmost region containing the point, or RECON_HIT_NONE. */
uint32_t recon_hit_test(struct recon_panel *panel, int x, int y);

#endif
