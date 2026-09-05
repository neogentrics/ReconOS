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

    RECON_METRIC_COUNT,
};

/*
 * What the current skin says, or the default when it says nothing.
 *
 * Clamped, because a skin is a text file somebody edits and a title bar of
 * height 4000 is not a look, it is a window with no contents.
 */
int recon_theme_metric(enum recon_theme_metric metric);

/* The name a skin file uses for a metric, for writing the defaults out. */
const char *recon_theme_metric_name(enum recon_theme_metric metric);

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

/* The name of a role, as it appears in a theme file. NULL if out of range. */
const char *recon_theme_role_name(enum recon_theme_role role);

const char *recon_theme_last_error(void);

#endif
