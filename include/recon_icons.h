/*
 * The ReconOS icon set.
 *
 * Icons are files in /System/Icons, looked up by name and loaded on first use.
 * Because the name is the only thing the code knows, replacing a file changes
 * what the system shows without any code changing -- which is what makes the
 * icons replaceable rather than compiled in.
 *
 * .ico and .png are both accepted. Anything drawing an icon must cope with it
 * being absent: a missing file is normal, and the caller draws its own shape
 * instead, so the system looks complete before any icons exist at all.
 */

#ifndef RECON_ICONS_H
#define RECON_ICONS_H

#include <stdbool.h>

#include "recon_ui.h"

/* Well-known names, so the same icon is asked for by the same word. */
#define RECON_ICON_FOLDER "folder"
#define RECON_ICON_FILE "file"

/*
 * One per kind of file, all drawn on the same sheet so they are a family.
 *
 * A single icon for every file says nothing, and saying what a thing is before
 * its name is read is most of what an icon is for. recon_props_icon decides
 * which name a given file gets.
 */
#define RECON_ICON_FILE_SOUND "file-sound"
#define RECON_ICON_FILE_VIDEO "file-video"
#define RECON_ICON_FILE_IMAGE "file-image"
#define RECON_ICON_FILE_WEB "file-web"
#define RECON_ICON_FILE_DATA "file-data"
#define RECON_ICON_FILE_FONT "file-font"
#define RECON_ICON_FILE_ARCHIVE "file-archive"
#define RECON_ICON_APP "application"
#define RECON_ICON_TERMINAL "terminal"

/*
 * The three glyphs on a window's title bar.
 *
 * Drawn as rectangles in C -- a cross, an outlined box, a bar -- because they
 * have to be there before a font has loaded and before any file has been read.
 * A skin that wants different ones can put a picture at any of these names and
 * it is used instead; there is no picture for them by default, and none is
 * written, so the rectangles are what almost every system draws.
 *
 * The same rule as every other lookup here: the file has to exist before it is
 * used, so a skin that supplies one of the three and not the others gets the
 * one it supplied and the drawn version of the rest. It cannot produce a
 * missing button, only a differently drawn one.
 *
 * "restore" is the maximize button when the window is already maximized. It is
 * a separate name because it is a separate meaning, and a skin that drew both
 * the same would be a skin that cannot say which state the window is in.
 */
#define RECON_ICON_WINDOW_CLOSE "window-close"
#define RECON_ICON_WINDOW_MAXIMIZE "window-maximize"
#define RECON_ICON_WINDOW_RESTORE "window-restore"
#define RECON_ICON_WINDOW_MINIMIZE "window-minimize"
#define RECON_ICON_NOTEPAD "notepad"
#define RECON_ICON_CALCULATOR "calculator"
#define RECON_ICON_EXPLORER "explorer"
#define RECON_ICON_TASKMGR "taskmanager"
#define RECON_ICON_SYSTEM "system"
/*
 * Drawn artwork rather than generated shapes, copied out of the asset
 * directory on first run the way the Recon Towers mark is.
 *
 * These three were all the generic system square, which is what the Control
 * Panel, the Apps button and half the settings pages were showing -- the same
 * red window meaning four different things.
 */
/*
 * Two pages that used to borrow.
 *
 * Accounts showed the generic application icon and Troubleshoot showed the
 * terminal's -- the nearest things to hand rather than pictures of what those
 * pages are. Borrowing works until an icon set turns up with the right
 * picture in it and nothing can ask for it, because the name says
 * "application" and means "the people who may sign in".
 *
 * The generated set draws the same shapes it always did under these names, so
 * a skin without an icon set looks exactly as it did.
 */
#define RECON_ICON_ACCOUNTS "accounts"

/*
 * Display Settings, which had been showing the notepad's pencil -- and so had
 * Registry, so two pages of the Control Panel wore the same picture. Type size
 * is most of what that page is for, and there is a picture of exactly that.
 */
#define RECON_ICON_DISPLAY "display"
#define RECON_ICON_TROUBLESHOOT "troubleshoot"

#define RECON_ICON_CONTROL_PANEL "control-panel"
#define RECON_ICON_APPS "apps"
#define RECON_ICON_HELP "help"

/* Control Panel items that had been sharing the generic system square. */
#define RECON_ICON_APPEARANCE "appearance"
#define RECON_ICON_PROGRAMS "programs"
#define RECON_ICON_FIREWALL "firewall"
#define RECON_ICON_NETWORK "network"
#define RECON_ICON_MODULES "modules"
#define RECON_ICON_RECOVERY "recovery"
#define RECON_ICON_UPDATE "update"
#define RECON_ICON_CLOCK "clock"
#define RECON_ICON_PHOTOS "photos"
#define RECON_ICON_CALENDAR "calendar"
#define RECON_ICON_MAIL "mail"
#define RECON_ICON_WEB "web"
#define RECON_ICON_PLAYER "player"
#define RECON_ICON_SHUTDOWN "shutdown"
/* The page that lists kept passwords. */
#define RECON_ICON_KEYRING "keyring"
/* The Recon Towers mark. Copied in from the assets rather than drawn: it is
 * artwork, not a generated glyph. */
#define RECON_ICON_LOGO "recon-towers"
/* Two bins, so a full one looks different from an empty one at a glance --
 * which is the whole point of having it on the desktop. */
#define RECON_ICON_TRASH "trash"
#define RECON_ICON_TRASH_FULL "trash-full"

/* The pixels for an icon, or NULL if there is no file for it. */
const unsigned char *recon_icon_get(const char *name, int *width, int *height);

/*
 * Draw an icon in a colour of the caller's choosing.
 *
 * The colour is used only for a *silhouette* -- an icon whose every visible
 * pixel is white and whose picture is entirely in its alpha channel. Whole
 * icon sets are drawn that way, the colour left to whoever shows them, and it
 * is what lets one file be a dark glyph on a light toolbar and a light one on
 * a dark title bar without being two files.
 *
 * A picture is drawn as it was made and ignores the colour, so a caller does
 * not have to know which kind it is asking for.
 */
bool recon_icon_draw_in(struct recon_panel *panel, const char *name,
    int x, int y, int size, recon_color ink);

/* Whether this icon is one of those silhouettes. */
bool recon_icon_is_mask(const char *name);

/* Draw an icon, returning false if there is none, so the caller can draw its
 * own. A silhouette takes the surface's text colour; recon_icon_draw_in is
 * the way to say otherwise. */
bool recon_icon_draw(struct recon_panel *panel, const char *name,
    int x, int y, int size);

/* Drop the cache, so replaced files are picked up. */
void recon_icons_forget(void);

#endif
