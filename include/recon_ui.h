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

/*
 * A colour scaled towards transparent, premultiplied.
 *
 * Wayland's ARGB8888 is premultiplied: the colour channels are already scaled
 * by the alpha, so halving a pixel's opacity means halving its red, green and
 * blue as well. A version that changed only the alpha byte would leave chrome
 * that is both see-through and too bright, which reads as a deliberate glow
 * rather than as a bug -- and would be found, if at all, by somebody
 * wondering why the glass looks lit from inside.
 *
 * Alpha 255 returns the colour unchanged, so a caller may apply this without
 * first asking whether there is anything to apply.
 */
recon_color recon_color_fade(recon_color color, uint8_t alpha);

/*
 * A colour part of the way between two others.
 *
 * `amount` out of 255: 0 is `from`, 255 is `to`. Alpha is taken from `from`,
 * because this is for mixing two opaque colours to get a third and not for
 * compositing -- a caller wanting transparency wants recon_color_fade.
 *
 * Useful for deriving a shade from roles a skin already answers rather than
 * asking it for another one. A gridline that is the readout's own text at a
 * quarter strength is behind the curve on every palette; a fixed grey is
 * behind it on most of them.
 */
recon_color recon_color_mix(recon_color from, recon_color to, uint8_t amount);

/*
 * A surface with light falling on it.
 *
 * A step, not a fraction of the distance to white -- because a fraction is a
 * gentle sheen on something light and a pale stripe on something dark, and a
 * highlight is supposed to look like the same lamp either way.
 */
recon_color recon_color_highlight(recon_color base);

/*
 * How light a colour is, 0 to 255.
 *
 * Weighted the way an eye weights it rather than as a plain average: green
 * carries most of the apparent brightness and blue almost none, so an average
 * calls pure blue and pure green equally bright and they are nothing alike.
 */
int recon_color_luminance(recon_color color);

/*
 * A colour to write in, on a surface of a given colour.
 *
 * `preferred` is what the skin asked for and is used whenever it can actually
 * be read there. When it cannot, this returns the skin's own light or dark
 * ink instead of a guess at black or white, so the answer still belongs to
 * the palette.
 *
 * This exists because one role can be right in two places and wrong in a
 * third. Beacon's `bar.text` is a near-black, chosen because that skin's task
 * buttons are pale -- correct on a button, and invisible on the deep blue bar
 * the clock is drawn straight onto. A role cannot know which of its uses is
 * being asked about. The surface can.
 */
recon_color recon_color_readable_on(recon_color surface, recon_color preferred,
    recon_color light_ink, recon_color dark_ink);

/*
 * Move a colour towards a hue while keeping how light it was.
 *
 * The point of keeping the lightness is that a palette's *structure* is in its
 * lightness, not its hue: which surfaces are above which, which text reads
 * against which. Recolouring a skin by rotating hues and leaving lightness
 * alone changes what it looks like; doing it the other way round changes
 * whether it works.
 *
 * So `tint` is used as a hue reference rather than as a colour to blend in.
 * A base at the tint's own lightness comes out as the tint; darker bases come
 * out as darker versions of it, lighter ones as lighter -- which is what stops
 * a light grey turning into a mid-tone just because the tint is one.
 *
 * `strength` is 0 to 255. Zero returns the base untouched, so a caller does
 * not have to ask whether a tint is set.
 */
recon_color recon_color_tint(recon_color base, recon_color tint, int strength);

/*
 * The same, at a lightness the caller chooses rather than the base's.
 *
 * A tint that keeps the base's lightness is a *mood*: the palette's structure
 * survives and the hue moves. That is right for glass and wrong for metal --
 * silver and ruby are not one colour at two hues, they are two lightnesses,
 * and pinning ruby to a pale grey's lightness gives pink. A skin whose tint is
 * the point of it wants the tint's own lightness, offset by where the role
 * sits relative to the rest of the skin.
 */
recon_color recon_color_tint_to(recon_color base, recon_color tint,
    int strength, int want);

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

/*
 * The shared font at a given size, loaded once and kept for the whole run.
 *
 * Application windows outlive the shell that built them -- that is what makes
 * restarting the shell a repair rather than a loss -- and every one of them
 * holds this pointer, several caching a copy of their own. A font owned by
 * the shell and freed with it left all of them drawing through freed memory,
 * which is a segmentation fault on the first frame after a restart rather
 * than an error anybody could act on.
 *
 * So the font outlives every shell. The pointer for a given size is the same
 * pointer every time it is asked for, which is also what makes changing the
 * typeface work: recon_font_reload replaces the contents in place, and every
 * window pointing here draws with the new one without being told.
 *
 * Returns NULL if no usable font was found; callers must cope.
 */
struct recon_font *recon_font_system(int pixel_height);

/*
 * The shared fixed-width font at a given size, cached the same way.
 *
 * For anything that prints in columns, which here means the terminal: the
 * interpreter already writes its tables with `%-20s`, and a proportional face
 * throws that work away -- the columns wander by a character or two on every
 * row, and a list of applications reads as a heap.
 *
 * Falls back to the system font, and logs, when the machine has no fixed-width
 * face. Never NULL when recon_font_system is not NULL.
 */
struct recon_font *recon_font_monospace(int pixel_height);

/*
 * The shared bold font at a given size, cached the same way.
 *
 * For a heading. A heading that is only *larger* than the text around it reads
 * as text that is larger; the weight is what makes it a heading.
 *
 * Falls back to the system font, and says so once, when the machine has no
 * bold face. Never NULL when recon_font_system is not NULL.
 */
struct recon_font *recon_font_bold(int pixel_height);

/* At shutdown, once nothing is left that could still draw. Frees both. */
void recon_font_system_finish(void);

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

/*
 * Read a rectangle of a panel's pixels, as ARGB8888.
 *
 * For saving part of what is on screen -- a graph, a picture somebody drew --
 * without going through screen capture, which would take the whole screen and
 * everything sitting on top of it.
 *
 * Copies into `out`, which must hold w * h pixels. False when the rectangle
 * runs outside the panel, rather than clamping: a caller asking for something
 * that is not there has a bug, and a smaller picture than it asked for is a
 * bug that looks like a feature.
 *
 * The panel's pixels are what was last drawn into it, so this is only
 * meaningful straight after a draw -- which is when anybody wants it.
 */
bool recon_panel_read(struct recon_panel *panel, int x, int y, int w, int h,
    uint32_t *out);


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
/*
 * Lay a colour over what is already there, honouring its alpha.
 *
 * `recon_fill_rect` writes the colour, which is right and is what nearly every
 * caller wants -- including the ones passing a translucent colour on purpose.
 * A Glass window frame is drawn with an alpha below 255 precisely so the
 * *compositor* blends the window over the wallpaper behind it; blending that
 * into the panel instead would make the frame opaque and the skin pointless.
 *
 * This is the other operation: blend into the panel, against what this window
 * has already drawn. The difference matters wherever a translucent colour is
 * meant to veil something in the same window rather than to make the window
 * see-through -- a dialog dimming the form behind it being the case that found
 * it. See BG-136.
 */
void recon_blend_rect(struct recon_panel *panel, int x, int y, int w, int h,
    recon_color color);

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

/*
 * A rounded rectangle drawn as a shape, filled or outlined, composited over
 * whatever is already there.
 *
 * Prefer these to recon_round_rect. That one fills a square rectangle and then
 * paints the corners back out with a colour the caller *believes* is behind
 * them, which is a guess -- and every place the guess is wrong shows as a
 * wedge of the wrong colour in the corner. It is wrong over a gradient, over
 * a wallpaper, and whenever a caller passes its panel colour rather than the
 * colour of the thing the control happens to be sitting on. These never
 * overwrite the corner, so there is nothing to guess.
 */
void recon_fill_round_rect(struct recon_panel *panel, int x, int y, int w,
    int h, int radius, recon_color color);
void recon_stroke_round_rect(struct recon_panel *panel, int x, int y, int w,
    int h, int radius, recon_color color);

/*
 * The two together, in one pass.
 *
 * Prefer this to a fill followed by a stroke. Those are two blends, and at a
 * corner the second lands on a pixel the first has already part-covered -- so
 * the outline comes out mixed with the face rather than with what is behind
 * the control, which on a light button over a dark title bar is a pale wedge
 * in each corner.
 */
void recon_fill_round_rect_edged(struct recon_panel *panel, int x, int y,
    int w, int h, int radius, recon_color face, recon_color edge);

/*
 * The same, with a one-pixel outline laid along the curve.
 *
 * Blended over what is already there rather than painted onto a fresh
 * surface, because a control is not always drawn before its contents -- the
 * taskbar puts a window icon and title into a button and asks for the edge
 * afterwards, and an outline that repainted the inside to get a clean surface
 * would erase both.
 */
void recon_round_rect_outline(struct recon_panel *panel, int x, int y, int w,
    int h, int radius, recon_color behind, recon_color edge);

/* A one-pixel outline just inside the given rectangle. */
void recon_stroke_rect(struct recon_panel *panel, int x, int y, int w, int h,
    recon_color color);

/*
 * Make a finished region see-through.
 *
 * --- Why this is a pass over the top rather than a colour ---
 *
 * The obvious way to build a translucent title bar is to give every fill an
 * alpha and blend as you go. It is also the way that means touching every
 * drawing primitive in the system, and getting one of them wrong produces a
 * window with an opaque patch where a bevel is.
 *
 * So the title bar is drawn exactly as it always was -- opaque, by code that
 * has not changed -- and then this runs once over the finished rectangle. Text,
 * icons, bevels and rounded corners all become see-through together, because
 * they are one image by the time this sees them, and nothing above had to learn
 * a new rule.
 *
 * --- Premultiplied ---
 *
 * A panel's pixels reach the compositor as ARGB8888, which Wayland defines as
 * *premultiplied*: the colour channels are already scaled by the alpha. So this
 * scales them, and a version that only set the alpha byte would produce chrome
 * that is both see-through and too bright -- which looks like a deliberate
 * glow rather than like a bug, and is the reason this is spelled out here.
 *
 * `alpha` of 255 is a no-op and costs one comparison, so a caller does not have
 * to ask whether the skin wanted glass before calling.
 */
void recon_panel_fade(struct recon_panel *panel, int x, int y, int w, int h,
    uint8_t alpha);

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
/*
 * --- For a control that must be drawn square and rounded afterwards ---
 *
 * recon_fill_button draws the shape, which is what nearly everything should
 * do. One thing cannot: the taskbar fills a button, draws a window's icon and
 * title into it, *washes the lot* when the window is put away, and only then
 * asks for the edge -- and the wash has to cover the icon and the title, so it
 * cannot happen before them and the fill cannot happen after.
 *
 * Such a caller keeps its corners before it starts and puts them back when it
 * has finished. Exact, unlike a `behind` colour: what goes back is what was
 * actually there, so it is right over a gradient, over a wallpaper and over
 * anything else, without being told which it is.
 *
 * The radius is worked out from the rectangle, so a caller that keeps and
 * restores the same rectangle cannot get the two out of step.
 */
#define RECON_CORNER_MAX 24

struct recon_corners {
    int radius;
    int w, h;
    bool held;
    uint32_t pixels[4][RECON_CORNER_MAX * RECON_CORNER_MAX];
};

void recon_corners_keep(struct recon_panel *panel, int x, int y, int w, int h,
    int radius, struct recon_corners *out);
void recon_corners_restore(struct recon_panel *panel, int x, int y,
    const struct recon_corners *keep);

/*
 * The edge alone -- outline and highlight, composited, filling nothing.
 *
 * For the caller above, after it has put its corners back.
 */
void recon_edge_button(struct recon_panel *panel, int x, int y, int w, int h,
    bool pressed);

/*
 * Fill a button and edge it, as one rounded shape.
 *
 * The one to use. Nothing is told what is behind the corners because nothing
 * paints over them: the shape is composited, so a button on a gradient, on a
 * photograph or on a flat panel all come out right without this being told
 * which it is.
 */
void recon_fill_button(struct recon_panel *panel, int x, int y, int w, int h,
    bool pressed, recon_color face);

/* The same button at a radius the caller supplies, for drawing one skin's
 * button while another skin is on screen. */
void recon_fill_button_radius(struct recon_panel *panel, int x, int y, int w,
    int h, bool pressed, recon_color face, int radius);

/*
 * The edge alone, for a caller that has already filled and drawn into the
 * button -- which the taskbar has to, because it washes a put-away window's
 * icon and title and the wash has to happen before the edge.
 *
 * Carves its corners back to `behind`, with the guess that implies. Prefer
 * recon_fill_button wherever the fill can be left to it.
 */
void recon_draw_button_edge(struct recon_panel *panel, int x, int y, int w,
    int h, bool pressed, recon_color behind);

/*
 * What corner radius a button of this size gets, given what the skin asked
 * for.
 *
 * Exposed for the few things that draw a rounded button themselves and have
 * to round something else to match it -- the ring around a dialog's default
 * answer, an icon inset into a button. They should be asking this rather than
 * reading the metric and subtracting a guess.
 */
int recon_button_radius(int w, int h);

/*
 * The same, for a skin that is not the one on screen.
 *
 * The Appearance page draws a sample button for every skin in the list, in
 * that skin's own numbers -- so the list cannot disagree with what the skin
 * will actually do. It needs the cap as much as the live one does, and having
 * its own copy of it is how the two come to differ.
 */
int recon_button_radius_of(int skin, int w, int h);

/*
 * Draw text with its left edge at x and its baseline at y.
 *
 * Clipped to max_width, with a trailing ellipsis when it doesn't fit, so long
 * window titles truncate instead of overrunning their button.
 */
void recon_draw_text(struct recon_panel *panel, struct recon_font *font,
    int x, int y, int max_width, const char *text, recon_color color);

/*
 * Draw text across as many lines as it needs, and say how tall it came out.
 *
 * The counterpart to recon_draw_text, which clips: a label that has to fit one
 * line wants that, and a sentence explaining something wants this. Getting the
 * two mixed up is why the Control Panel had an explanation reading "Its own
 * co..." -- an explanation cut off before it explains anything, which is worse
 * than the absence it was written to explain.
 *
 * `y` is the TOP of the first line rather than its baseline, unlike
 * recon_draw_text. A wrapped block is positioned by where it starts and
 * measured by how tall it is, and a caller that had to add an ascent to get in
 * and read a return value to get out would be doing arithmetic in two
 * different coordinate systems.
 *
 * Breaks at spaces. A single word longer than the width is drawn on its own
 * line and clipped by recon_draw_text rather than broken mid-word, because a
 * word split across two lines is unreadable in a way a clipped one is not.
 *
 * Returns the height drawn, so the caller can carry on below it.
 */
int recon_draw_paragraph(struct recon_panel *panel, struct recon_font *font,
    int x, int y, int max_width, const char *text, recon_color color);

/*
 * Draw RGBA pixels into a rectangle, scaling to fit and blending by alpha.
 *
 * Shrinking averages the source pixels that fall inside each destination
 * pixel; growing takes the nearest one. Averaging when shrinking is what
 * makes a small icon look small rather than damaged, and what makes a
 * photograph fitted to a window look like the photograph. Growing is left
 * alone, because icons are pixel art and blurring them upward is worse than
 * the steps.
 */
void recon_draw_image(struct recon_panel *panel, int x, int y, int w, int h,
    const unsigned char *rgba, int image_width, int image_height);

/*
 * The same, kept inside a band of rows.
 *
 * For a caller drawing into part of a panel rather than all of it. Text did
 * not need this: a line is twenty-four pixels tall, so a viewport that stops
 * at the last line whose top is visible is wrong by less than one line. An
 * image is three hundred, and one whose top is ten pixels above the last
 * visible row draws the other two hundred and ninety over whatever is below.
 */
void recon_draw_image_clipped(struct recon_panel *panel, int x, int y,
    int w, int h, const unsigned char *rgba,
    int image_width, int image_height, int top, int bottom);

/* --- Double clicks --- */

/*
 * Whether this click is the second of a pair.
 *
 * Kept here rather than added to the window's click callback, because that
 * callback is part of the module ABI: a new argument would mean every module
 * built before today reading past the end of its own arguments, which is
 * exactly the fault BG-050 was. A shared answer to "was that a double click"
 * costs nothing and breaks nothing.
 *
 * `id` is whatever the caller uses to tell one target from another -- a hit
 * region, a row number. Two clicks count as a pair when they land on the same
 * id inside the interval below. Call it once per click and act on the answer:
 * asking twice for one click reports the second ask as a single.
 */
#define RECON_DOUBLE_CLICK_MS 400

bool recon_click_is_double(uint32_t id);

/* Forget the last click, so the next one cannot pair with it. For a list that
 * has just been rebuilt under the pointer, where "the same id" no longer
 * means the same thing. */
void recon_click_forget(void);

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

    /*
     * Whether this field is the one being typed into.
     *
     * `recon_edit_begin` and `recon_edit_focus` set it, so a window with a
     * single field -- a rename box, a dialog -- gets it without asking. A
     * window with several sets it directly, and is responsible for clearing it
     * on the others: exactly one field at a time, or the caret is a lie about
     * where the next keystroke goes.
     */
    bool active;

    /* Draw dots instead of the characters. For a password, where the point is
     * that somebody behind you cannot read it. The text itself is untouched --
     * masking is about what is shown, not what is stored. */
    bool masked;

    /*
     * Enter puts a newline in rather than finishing.
     *
     * For a field that holds a letter rather than a name. Everywhere else in
     * this system Enter means "done", and it still does here for every other
     * field -- so a form with a body in it has one control that behaves
     * differently, which is why the flag is on the field rather than being a
     * mode the whole form is in.
     *
     * recon_edit_draw still draws one line. A caller with a multi-line field
     * has a box to lay out and is drawing it itself; what it needs from here
     * is the editing, not the picture.
     */
    bool multiline;
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

/*
 * Put the caret in a field that already has text in it, with the text selected.
 *
 * What recon_edit_begin does, minus the copy -- and the copy is the point.
 * `recon_edit_begin(edit, edit->text, false)` is how a form used to move focus
 * between fields, and it hands snprintf a source and a destination that are the
 * same buffer, which is undefined and on this library empties the field. A form
 * with defaults in it lost them to being tabbed past. See BG-112.
 */
void recon_edit_focus(struct recon_edit *edit);

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
/*
 * A line of text for the region added last.
 *
 * Written straight after the `recon_hit_add` it belongs to, which is why it
 * takes no id: a tooltip is part of registering a thing, not a second
 * bookkeeping exercise beside it. Somewhere for a control to say what it does
 * without spending screen on saying it all the time.
 *
 * Kept short. A tooltip is a label, not documentation -- the Help application
 * is where a paragraph goes.
 */
bool recon_hit_tip(struct recon_panel *panel, const char *text);

/*
 * The tooltip under a point, in the panel's own coordinates. Empty when the
 * region there has none, which is most of them.
 */
bool recon_hit_tip_at(struct recon_panel *panel, int x, int y,
    char *out, size_t size);

/*
 * The same question in screen coordinates, for callers holding a panel whose
 * position they would otherwise have to look up first.
 *
 * False means two different things -- outside the panel, or inside it with no
 * tip there -- so a caller searching front to back needs recon_panel_contains
 * to tell them apart. See BG-106.
 */
bool recon_panel_tip_at(struct recon_panel *panel, double lx, double ly,
    char *out, size_t size);

/*
 * Is this point inside the panel at all?
 *
 * Exists so that a panel in front can stop a search it has no answer for. A
 * blank patch of the thing on top is not a hole to read what is underneath
 * through -- the same rule the window loop follows, which the panels in front
 * of it could not, because a false from recon_panel_tip_at does not say which
 * kind of false it is. BG-106.
 */
bool recon_panel_contains(const struct recon_panel *panel, double lx, double ly);

bool recon_hit_region(const struct recon_panel *panel, size_t index,
    int *x, int *y, int *w, int *h, uint32_t *id);

/*
 * Mark the region added last as unable to answer a click.
 *
 * Written straight after its recon_hit_add, the same way recon_hit_tip is,
 * and for the same reason: being unavailable is part of registering a
 * control, not a second thing to remember about it somewhere else. It keeps
 * its tooltip, so pointing at it can still say why.
 *
 * Callers should reach this through recon_widget's `disabled`, which sets it.
 */
bool recon_hit_inert(struct recon_panel *panel);

/*
 * The id of the topmost region containing the point, or RECON_HIT_NONE.
 *
 * Two of them, because "what is under the pointer" and "what would answer a
 * click" stopped being the same question when controls learnt to be
 * disabled. recon_hit_test answers the first -- it is what a tooltip wants,
 * and what a cursor shape wants. recon_hit_test_active answers the second,
 * and is what every click path should be asking.
 */
uint32_t recon_hit_test(struct recon_panel *panel, int x, int y);
uint32_t recon_hit_test_active(struct recon_panel *panel, int x, int y);

/*
 * Which region the pointer is over, and which one it went down on.
 *
 * Storage only. What the two mean -- when something counts as hot, what a
 * held button looks like -- is recon_widget's, and callers should be asking
 * it rather than these. They live here because the panel is where a panel's
 * state lives, and because a second place to keep it would be a second place
 * for it to be stale.
 *
 * Deliberately untouched by recon_hit_clear. A panel redraws from scratch
 * many times a second while the pointer sits still, and hover that had to be
 * re-established after every repaint would flicker off and on again.
 */
void recon_panel_set_hot(struct recon_panel *panel, uint32_t id);
uint32_t recon_panel_hot(const struct recon_panel *panel);
void recon_panel_set_held(struct recon_panel *panel, uint32_t id);
uint32_t recon_panel_held(const struct recon_panel *panel);

#endif
