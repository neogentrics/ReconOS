/*
 * Skins.
 *
 * Every colour ReconOS draws is asked for by what it means rather than by what
 * it looks like: TITLE_ACTIVE, not "dark blue". A skin is a set of answers to
 * those questions, and swapping one for another restyles the whole system
 * without a single drawing call changing.
 *
 * That indirection is the entire point. Colours used to be `#define COLOR_*`
 * in nine files, several of them the same value under different names, and
 * "make the system look different" meant editing all nine and hoping none had
 * been missed.
 *
 * A role is a question about meaning, so the answer stays sensible under a
 * skin nobody has written yet. SELECTION is "what marks the thing the user
 * picked", and a light skin, a dark skin and a high-contrast skin can all
 * answer it without the code that draws a selection knowing which is in use.
 *
 * Skins live in /System/Themes as text, and four are shipped: the native Recon
 * look, and Windows-, Mac- and Linux-flavoured ones. Which is in use is a
 * registry setting, so it is remembered and can differ per account.
 *
 * Icons are deliberately not themed here. They are files in /System/Icons, and
 * a skin that wanted its own would ship its own files rather than have its
 * artwork described as a list of colours.
 */

#ifndef RECON_THEME_H
#define RECON_THEME_H

#include <stdbool.h>
#include <stddef.h>

#include "recon_ui.h"

/*
 * What a colour is for.
 *
 * Grouped by the part of the system that asks. Adding one means adding it to
 * the name table and to every built-in skin, which is deliberate: a role no
 * skin answers is a role that draws as magenta and gets noticed.
 */
enum recon_theme_role {
    /* --- Window frames --- */
    RECON_THEME_WINDOW_FRAME,
    RECON_THEME_WINDOW_EDGE,
    RECON_THEME_TITLE_ACTIVE,
    RECON_THEME_TITLE_INACTIVE,
    RECON_THEME_TITLE_TEXT,
    RECON_THEME_TITLE_TEXT_INACTIVE,
    RECON_THEME_WINDOW_BUTTON,
    RECON_THEME_WINDOW_BUTTON_GLYPH,

    /* --- The shell: taskbar and its buttons --- */
    RECON_THEME_BAR,
    RECON_THEME_BAR_TEXT,
    /*
     * The name of a window that is not the current one, on its own button.
     *
     * Dimmer than BAR_TEXT, and still readable: this is the label somebody
     * reads to find the window they want, so being quieter is what marks it
     * as not-current, not being hard to see. Four skins had it below what the
     * accessibility test calls readable, which nothing noticed while the test
     * was measuring it against the wrong surface.
     */
    RECON_THEME_BAR_TEXT_DIM,
    RECON_THEME_BUTTON,
    RECON_THEME_BUTTON_ACTIVE,
    /*
     * A button's own label -- "Apps", "End Task", "Cancel".
     *
     * Separate from BAR_TEXT, which is the label of the window a taskbar
     * button stands for, and which is drawn on BUTTON_ACTIVE. One role could
     * not serve both: the high-contrast skin fills an ordinary button white
     * and a pressed one black, so a single colour was invisible on one of
     * them whichever it was.
     */
    RECON_THEME_BUTTON_TEXT,

    /* --- Menus --- */
    RECON_THEME_MENU,
    RECON_THEME_MENU_BORDER,
    RECON_THEME_MENU_TEXT,
    RECON_THEME_MENU_TEXT_DISABLED,
    RECON_THEME_MENU_HILITE,
    RECON_THEME_MENU_HILITE_TEXT,
    RECON_THEME_MENU_SEPARATOR,

    /* --- Dialogs --- */
    RECON_THEME_DIALOG,
    RECON_THEME_DIALOG_TITLE,
    RECON_THEME_DIALOG_TITLE_TEXT,
    /* What is laid over the screen behind something modal. Needs an alpha
     * that is not 255, or the thing behind it disappears rather than dims. */
    RECON_THEME_DIM,

    /* --- Content: lists, editors, anything a document lives in --- */
    RECON_THEME_SURFACE,
    RECON_THEME_SURFACE_ALT,
    RECON_THEME_SURFACE_TEXT,
    RECON_THEME_SURFACE_TEXT_DIM,
    RECON_THEME_SURFACE_HEADER,
    RECON_THEME_SELECTION,
    RECON_THEME_SELECTION_TEXT,

    /* --- Text fields --- */
    RECON_THEME_FIELD,
    RECON_THEME_FIELD_BORDER,
    RECON_THEME_FIELD_TEXT,
    RECON_THEME_FIELD_SELECTION,
    RECON_THEME_CARET,

    /* --- A dark readout: a terminal, a calculator's display --- */
    RECON_THEME_READOUT,
    RECON_THEME_READOUT_TEXT,
    RECON_THEME_READOUT_ACCENT,
    RECON_THEME_READOUT_INPUT,

    /* --- The desktop --- */
    RECON_THEME_DESKTOP_LABEL,
    RECON_THEME_DESKTOP_LABEL_SHADOW,
    RECON_THEME_DESKTOP_SELECTION,

    /* --- Meaning rather than place --- */
    RECON_THEME_ACCENT,
    RECON_THEME_ACCENT_TEXT,
    RECON_THEME_WARNING,
    /* Folder names in a listing, which read differently from file names. */
    RECON_THEME_DIRECTORY,

    RECON_THEME_ROLE_COUNT,
};

/*
 * What shape a window frame is.
 *
 * Colours were the whole of a skin: a skin could recolour a title bar and not
 * change its height, its corner radius or how big its buttons were -- so
 * every skin, from the 95 one to the early-2000s one, drew a frame of exactly
 * the same proportions in different colours.
 *
 * These are numbers rather than roles, and they are optional: a skin that
 * says nothing gets the default, which is the shape ReconOS has always had.
 * That is deliberately unlike the colours, where every skin answers every
 * role -- a colour nobody chose should be obvious on screen, and a measurement
 * nobody chose should simply be the usual one.
 */
enum recon_theme_metric {
    /* The title bar's height, and so where a window's contents begin. */
    RECON_METRIC_TITLE_HEIGHT,
    /* The frame around the contents. */
    RECON_METRIC_BORDER,
    /*
     * How far the top corners are rounded. Zero is square.
     *
     * Only the top two: the bottom corners of a window sit against whatever
     * is below them, and rounding those reads as a gap rather than as a
     * shape.
     */
    RECON_METRIC_CORNER,
    /* The minimize, maximize and close buttons. */
    RECON_METRIC_BUTTON_SIZE,
    /*
     * How far a button's corners are rounded. Zero is square.
     *
     * All four, unlike a window's -- a button sits inside something rather
     * than against the edge of the screen, so there is no side of it that
     * rounding would turn into a gap.
     */
    RECON_METRIC_BUTTON_CORNER,

    /*
     * How solid the window chrome is, from 255 for opaque down to 140.
     *
     * The window's own buffer carries alpha and the compositor blends it, so
     * this is a real see-through rather than a colour mixed with a guess at
     * what is behind. Applied to the finished title bar and border rather than
     * to each thing drawn into them, which is what keeps every other drawing
     * primitive on the opaque path it has always taken.
     *
     * The floor is 140 and it is not shyness. Below about that the title text
     * stops being reliably readable over a busy wallpaper, and a skin that can
     * make a window unusable is a skin somebody installs once. Glass has to be
     * something a person can still read a filename off.
     */
    RECON_METRIC_CHROME_OPACITY,

    /*
     * Whether to use the glossy icon set: 1 for yes, 0 for the flat one.
     *
     * A choice between two sets of files rather than an amount, because that
     * is what it really is -- icons are written to disk once and replaceable
     * by dropping a different file over them, so there is nothing here that
     * could vary continuously. A 0-to-100 knob would be a promise the shape of
     * the thing cannot keep.
     */
    RECON_METRIC_ICON_GLOSS,

    /*
     * Whether this skin may be given a tint: 1 for yes.
     *
     * Off for every skin but Glass, and the important case is *why* it is off
     * for three of them. Contrast, Deuteran, Protan and Tritan have palettes
     * chosen so that particular pairs of colours stay distinguishable to
     * particular eyes. Rotating their hues would undo the one thing they are
     * for, and it would undo it invisibly to whoever chose the tint.
     *
     * So a skin opts in rather than a tint being applied to whatever is
     * current. A colour-vision skin cannot be tinted at all, which is a
     * property of the design rather than a warning somebody has to read.
     */
    RECON_METRIC_TINTABLE,

    /*
     * Which side of the title bar the buttons are on: 0 for the right, 1 for
     * the left.
     *
     * Close stays outermost either way -- nearest the right edge on the right,
     * nearest the left edge on the left. That is not a detail: the button
     * somebody reaches for without looking is the one in the corner, and a
     * layout that put maximize there would be a layout that closes windows by
     * accident in the other direction.
     */
    RECON_METRIC_BUTTONS_LEFT,

    /*
     * Which buttons a window has, as a sum: 1 close, 2 maximize, 4 minimize.
     * Seven is all three and is the default.
     *
     * **Close cannot be taken away.** A skin that removed it would make every
     * window in the system unclosable by mouse, and a skin is a file somebody
     * downloads -- so the 1 is put back whatever the skin asked for, in the
     * one place that reads this. It is the same rule as everywhere else here:
     * a safety property is not something a setting gets to switch off.
     *
     * Maximize and minimize are genuinely optional. A layout with only a close
     * button is a real design and not a broken one.
     */
    RECON_METRIC_BUTTONS,

    RECON_METRIC_COUNT,
};

/* The bits in RECON_METRIC_BUTTONS. */
#define RECON_BUTTON_CLOSE    1
#define RECON_BUTTON_MAXIMIZE 2
#define RECON_BUTTON_MINIMIZE 4
#define RECON_BUTTONS_ALL     (RECON_BUTTON_CLOSE | RECON_BUTTON_MAXIMIZE | \
                               RECON_BUTTON_MINIMIZE)

/*
 * What the current skin says, or the default when it says nothing.
 *
 * Clamped, because a skin is a text file somebody edits and a title bar of
 * height 4000 is not a look, it is a window with no contents.
 */
int recon_theme_metric(enum recon_theme_metric metric);

/* The name a skin file uses for a metric, for writing the defaults out. */
const char *recon_theme_metric_name(enum recon_theme_metric metric);

/*
 * Whether the current skin says anything about this, or is taking the default.
 *
 * The difference matters to anything offering to change one. A skin that says
 * nothing about a measurement gets the shape ReconOS has always had, and that
 * is not the same as a skin that asks for exactly that number: the first
 * follows the default if the default ever moves, and the second does not.
 * Showing them as the same thing would make "use the default" look like a
 * button that does nothing.
 */
bool recon_theme_metric_is_set(enum recon_theme_metric metric);

/*
 * What a skin may say: the range, and the value used when it says nothing.
 *
 * Any of the three may be NULL. For telling somebody what will be accepted
 * *before* they type it -- `recon_theme_set_metric` clamps silently, which is
 * right for a file being read and wrong as the only answer a person gets.
 */
void recon_theme_metric_range(enum recon_theme_metric metric,
    int *least, int *most, int *fallback);

/*
 * A measurement written the way somebody would say it, rather than as the
 * number the file holds.
 *
 * Three of these are not lengths and one is not a number at all. `buttons` is
 * a bit set: 7 is the default and means all three, and a list reading
 * "7" tells nobody which buttons that is. `icon-gloss`, `tintable` and
 * `buttons-left` are yes-or-no. Only the six remaining are pixels.
 *
 * So: "close maximize minimize", "yes", "24". Here rather than in the Control
 * Panel because how a measurement is written is this file's business -- it is
 * the same vocabulary the skin file uses -- and because here it can be tested
 * without a window.
 */
void recon_theme_metric_text(enum recon_theme_metric metric, int value,
    char *out, size_t size);

/*
 * The inverse. False when the text is not something this measurement can be.
 *
 * Accepts what somebody would type: a number for a length, `yes`/`no` (or
 * `on`/`off`, `true`/`false`, `1`/`0`) for the three that are really
 * questions, and a list of button names in any order -- or the number, for
 * anybody who already knows the bits.
 *
 * **Does not clamp.** A number outside the range is refused here rather than
 * quietly moved, because the caller is a person who can be told; `set_metric`
 * still clamps, because its caller is a file that cannot.
 */
bool recon_theme_metric_parse(enum recon_theme_metric metric,
    const char *text, int *value);

/*
 * --- Tints ---
 *
 * A tint is a hue the chrome is moved towards, keeping the lightness it had.
 * It is *not* a skin: eleven skins times six colours would be sixty-six
 * entries in a list somebody has to read, and every one of them would differ
 * from its neighbours in one respect. So it sits beside the skin, remembered
 * per account like the skin is.
 *
 * Only the chrome moves -- frames, bars, buttons, menus. Text does not, because
 * a tint must not decide whether a label is readable; and neither do the
 * accent, the selection or the warning colour, because those carry meaning and
 * a meaning that changes hue with the decor is a meaning nobody can learn.
 */
#define RECON_THEME_TINT_KEY "theme/tint"
#define RECON_TINT_NAME_MAX 24

int recon_tint_count(void);
bool recon_tint_at(int index, char *name, size_t size);

/* The tint in use, or "" when none is. Never NULL. */
const char *recon_tint_current(void);

/*
 * The hue a tint moves the chrome towards.
 *
 * For showing one rather than naming it. Six words in a row say six things
 * somebody has to imagine; six colours say them.
 */
bool recon_tint_colour(int index, recon_color *out);

/*
 * Put one on. The empty string, or "None", takes it off again.
 *
 * False when there is no such tint, or when the current skin does not accept
 * one -- which is not a failure to report so much as a question that does not
 * apply. recon_theme_last_error says which.
 */
bool recon_tint_set(const char *name);

/* Whether the current skin accepts a tint at all, for a page that should not
 * offer a choice that cannot be made. */
bool recon_tint_available(void);

/* Where skins live, and how they are named. */
#define RECON_DIR_THEMES "/System/Themes"
#define RECON_THEME_EXT ".theme"
#define RECON_THEME_DEFAULT "Recon"

/* Which skin is in use. Reading settings are a separate concern and live in
 * recon_access.h, because spacing is not a colour. */
#define RECON_THEME_KEY "theme"

struct recon_theme_info {
    char name[48];
    char description[96];
    /* The wallpaper this skin goes with, by file name in /System/Wallpapers.
     * A suggestion: choosing the skin puts it on, and a wallpaper chosen
     * afterwards stays. Empty when the skin has no opinion. */
    char wallpaper[96];
    /* True for one compiled in, false for one read from /System/Themes. */
    bool built_in;
};

/*
 * Load the skins that ship with ReconOS, then anything in /System/Themes, then
 * select whichever the registry asks for.
 *
 * Requires the filesystem and registry to be up. Never fails in a way worth
 * stopping for: with no files and no setting, the built-in default is used.
 */
void recon_theme_init(void);
void recon_theme_finish(void);

/* --- Asking --- */

/*
 * The colour for a role under the current skin.
 *
 * A role out of range returns something deliberately hideous rather than
 * black, because a colour nobody chose should be obvious on screen instead of
 * blending in as a shadow.
 */
recon_color recon_theme_color(enum recon_theme_role role);

/* Shorthand at drawing sites, where the noise would otherwise be the point. */
#define THEME(role) recon_theme_color(RECON_THEME_##role)

/*
 * The second colour of a role's gradient, if the skin gave it one.
 *
 * False when it did not, which is the normal case: a gradient is something a
 * skin opts into for a handful of surfaces, so this is not a role of its own
 * and does not have to be answered by every skin. A skin file says so with a
 * `.to` alongside the colour:
 *
 *     title.active    = #2A5BC8
 *     title.active.to = #4A8BE8
 *
 * Callers should use recon_fill_role rather than this, unless they need the
 * two colours for something other than filling a rectangle.
 */
bool recon_theme_gradient(enum recon_theme_role role, recon_color *from,
    recon_color *to);

/*
 * Fill a rectangle the way the current skin says that role should look:
 * a vertical gradient where it asked for one, a flat fill where it did not.
 *
 * This is what drawing sites call. It exists so that adding a gradient to a
 * skin is a change to the skin and not a change to the code that draws --
 * which is the same bargain the roles themselves make.
 */
/*
 * Mix a region back towards the way a role looks, without repainting it.
 *
 * The inverse of recon_panel_fade, and needed for the opposite reason. Fading
 * moves a region towards transparent, which is right for chrome sitting over a
 * wallpaper and wrong anywhere the thing behind is not going to be redrawn --
 * on the taskbar it would show the desktop through the middle of the bar.
 *
 * This moves a region towards the colour it is sitting on instead, which is
 * what "receded" actually means: less different from its surroundings. That
 * makes it the one way to say "this is not prominent" that cannot be defeated
 * by a skin. Every other way -- a dimmer ink, a paler fill, an accent mark --
 * needs two colours to stay far enough apart, and a skin may put them
 * anywhere. Contrast paints the bar, the accent and the active button all
 * pure black; Midnight makes the active button lighter rather than darker.
 * Moving towards the surface underneath is correct on both, because the
 * direction is defined by the surface rather than by a second colour.
 *
 * `amount` is out of 255: 0 changes nothing and 255 erases the region back to
 * the plain role. Alpha is taken from what is already there, so an opaque
 * panel stays opaque.
 *
 * Pass the same rectangle the fill used. Where a role carries a gradient the
 * ramp is positioned against the rectangle given here, exactly as the fill
 * positions its own -- so washing a sub-rectangle stretches the ramp across
 * the wrong height and lands on colours the fill never used. To wash part of a
 * control, pass the whole control and clip.
 *
 * How wrong that goes has not been measured. It was blamed for a real
 * discrepancy once and turned out to be innocent, so it is stated here as a
 * property of the arithmetic rather than as a fault anybody has seen.
 */
void recon_wash_role(struct recon_panel *panel, int x, int y, int w, int h,
    enum recon_theme_role role, uint8_t amount);

void recon_fill_role(struct recon_panel *panel, int x, int y, int w, int h,
    enum recon_theme_role role);

const char *recon_theme_current(void);

/* The wallpaper the current skin suggests, or "". */
const char *recon_theme_wallpaper(void);

/* --- Choosing --- */

/*
 * Switch skin, remember the choice, and tell the shell to redraw. False if
 * there is no such skin, in which case nothing changes.
 */
bool recon_theme_set(const char *name);

int recon_theme_count(void);
bool recon_theme_at(int index, struct recon_theme_info *out);

/*
 * A colour from a skin that is not the one in use.
 *
 * For showing a skin rather than describing it: a list of skins drawn
 * entirely in the current skin's colours tells you nothing about any of the
 * others, and several of ours share a selection blue, so the list looked as
 * though half the skins were the same. Out of range returns the same
 * deliberately hideous colour recon_theme_color does.
 */
recon_color recon_theme_color_of(int index, enum recon_theme_role role);

/*
 * A metric from a skin that is not the one in use.
 *
 * The colour twin above exists because a list of skins painted in the current
 * skin's colours says nothing about any of the others. Shape is now half of
 * what a skin is -- how round its buttons are, how thick its border, how tall
 * its title bar -- so a list that shows only colour has the same problem in
 * the other half. Out of range gives the default, which is what an unlisted
 * skin would draw with anyway.
 */
int recon_theme_metric_of(int index, enum recon_theme_metric metric);

/* The same for a gradient, so a skin shown in a list shows its ramps too. A
 * preview drawn flat makes every skin with a gradient look like a skin
 * without one. */
bool recon_theme_gradient_of(int index, enum recon_theme_role role,
    recon_color *from, recon_color *to);

/*
 * Bumped whenever the colours change. Anything holding a cached colour can
 * compare this to know it is stale -- nothing does yet, and this exists so
 * that the first thing to want it does not have to invent a way.
 */
unsigned recon_theme_generation(void);

/* --- Writing them out --- */

/*
 * Write the built-in skins into /System/Themes if they are not there, so a
 * person has something to copy when writing their own. Existing files are left
 * alone. Returns how many were written.
 */
int recon_theme_write_defaults(void);

/*
 * Copy a skin under a new name, and register it.
 *
 * The way to start writing one from inside ReconOS. A skin could be installed
 * and removed since v0.2.4, and there was still no way to make one here:
 * authoring meant writing a text file somewhere else and bringing it in.
 * Copying an existing skin gives a complete file -- every role answered, the
 * ramps and the frame shape included -- which is a far better starting point
 * than a blank one, because the questions a skin has to answer are the part
 * nobody knows in advance.
 *
 * `source` is the skin to copy, or NULL for the one in use. `description` is
 * optional; without one it says which skin it was started from.
 *
 * The copy is written to /System/Themes and read back, so what the system has
 * is what the file says. False with recon_theme_last_error() explaining why.
 */
bool recon_theme_copy(const char *source, const char *name,
    const char *description);

/* --- Editing one --- */

/*
 * Change one answer in a skin that came from a file, and write the file back.
 *
 * Built-in skins are refused rather than written over: a file cannot shadow a
 * built-in, so the edit would be saved, ignored, and lost on the next start.
 * That is worse than a refusal, because it looks like it worked. Copy first,
 * then edit the copy.
 *
 * The change is live: the palette everything draws from is the loaded skin,
 * so a colour set here is on screen before the file is closed. The file is
 * still what is real -- if the write fails the change is put back.
 */
bool recon_theme_set_role(const char *name, enum recon_theme_role role,
    recon_color color);

/* The far end of a ramp, or `on` false to make the role flat again. */
bool recon_theme_set_gradient(const char *name, enum recon_theme_role role,
    bool on, recon_color to);

/*
 * --- Writing a colour down, and reading one back ---
 *
 * Six hex digits, or eight when the alpha matters: the same notation a skin
 * file uses, so what somebody types and what is in the file are the same
 * thing and reading one teaches the other.
 */
void recon_theme_colour_text(recon_color colour, char *out, size_t size);

/*
 * One colour, and nothing after it.
 *
 * Strict about the whole string, unlike the version this replaced -- that one
 * stopped at the first space and returned what it had, so "AABBCC junk" was
 * accepted as AABBCC and the junk was silently dropped.
 */
bool recon_theme_colour_parse(const char *text, recon_color *out);

/*
 * A colour and its ramp, written the way a list shows them:
 * "E8E8EC to D4DAE2", or "1C1C20" when there is no ramp.
 *
 * The same text in both directions. A field showing the whole value is a
 * field somebody can retype the whole value into, which is what makes
 * `recon_theme_ramp_parse` below able to mean "flat" without a second control
 * to say it.
 */
void recon_theme_ramp_text(recon_color from, bool ramped, recon_color to,
    char *out, size_t size);

/*
 * The inverse.
 *
 * **One colour means flat.** The text is the whole value, not an edit to part
 * of it, so what somebody types is what they get -- a field reading
 * "E8E8EC to D4DAE2" that is replaced with "AABBCC" has been told to be
 * AABBCC, and quietly keeping the old far end would produce a colour nobody
 * asked for and nobody typed.
 */
bool recon_theme_ramp_parse(const char *text, recon_color *from, bool *ramped,
    recon_color *to);

/*
 * A frame measurement, or `on` false to hand it back to the default.
 *
 * Clamped to what the system will accept before it is written, so a file
 * cannot hold a number the system would then refuse to use.
 */
bool recon_theme_set_metric(const char *name, enum recon_theme_metric metric,
    bool on, int value);

/* What the skin says about itself in the list. */
bool recon_theme_describe(const char *name, const char *description);

/* --- Installing --- */

/*
 * Take a skin file at `path` and put it where skins live.
 *
 * The file format has existed since the skin system did, and until now there
 * was no way to get one into /System/Themes short of writing it there by
 * hand -- so a skin somebody wrote was a file the system could read and
 * nobody could install.
 *
 * It is read and parsed *before* it is copied, so a file that is not a skin
 * is refused rather than left in the folder for the next start to trip over.
 * A name already taken is refused too: a file cannot shadow a built-in, so
 * copying one in under an existing name would put a file in place that the
 * system would then silently ignore.
 *
 * Administrator only, which the filesystem enforces on /System anyway; this
 * says so with a sentence rather than with a write error.
 *
 * False with recon_theme_last_error() explaining why. Nothing is left behind
 * on failure.
 */
bool recon_theme_install(const char *path);

/*
 * Remove an installed skin.
 *
 * Built-in skins are refused: they are compiled in, so deleting the file
 * would remove a copy of something that would come back on the next start,
 * and the skin would still be there. If the skin being removed is the one in
 * use, the default is put on first -- taking away the colours somebody is
 * looking at without giving them others is not something to do quietly.
 */
bool recon_theme_uninstall(const char *name);

/*
 * Give an installed skin a different name.
 *
 * Three things have to move together and this is the only place that knows
 * they are one operation: the file is named after the skin, the name is also
 * written inside it, and the account remembers which skin it is using by name.
 * Doing any two of those leaves a skin that half exists.
 *
 * Built-in skins are refused, for the reason removing one is: they are
 * compiled in, so the renamed copy would sit beside the original that came
 * back on the next start.
 *
 * **Changing only the capitalisation is allowed**, and is the case worth
 * naming: a skin is found by name without regard to case, so "testing" and
 * "Testing" are the same skin -- but they are different *files*, and the old
 * one still has to go.
 *
 * False with recon_theme_last_error() explaining why. On failure the skin
 * keeps the name it had and no file is left behind under the other one.
 */
bool recon_theme_rename(const char *name, const char *to);

/* The name of a role, as it appears in a theme file. NULL if out of range. */
const char *recon_theme_role_name(enum recon_theme_role role);

const char *recon_theme_last_error(void);

#endif
