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
    RECON_THEME_BAR_TEXT_DIM,
    RECON_THEME_BUTTON,
    RECON_THEME_BUTTON_ACTIVE,

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

/* The name of a role, as it appears in a theme file. NULL if out of range. */
const char *recon_theme_role_name(enum recon_theme_role role);

const char *recon_theme_last_error(void);

#endif
