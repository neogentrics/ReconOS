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
#define RECON_ICON_APP "application"
#define RECON_ICON_TERMINAL "terminal"
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
#define RECON_ICON_SHUTDOWN "shutdown"
/* The Recon Towers mark. Copied in from the assets rather than drawn: it is
 * artwork, not a generated glyph. */
#define RECON_ICON_LOGO "recon-towers"
/* Two bins, so a full one looks different from an empty one at a glance --
 * which is the whole point of having it on the desktop. */
#define RECON_ICON_TRASH "trash"
#define RECON_ICON_TRASH_FULL "trash-full"

/* The pixels for an icon, or NULL if there is no file for it. */
const unsigned char *recon_icon_get(const char *name, int *width, int *height);

/* Draw an icon, returning false if there is none, so the caller can draw its
 * own. */
bool recon_icon_draw(struct recon_panel *panel, const char *name,
    int x, int y, int size);

/* Drop the cache, so replaced files are picked up. */
void recon_icons_forget(void);

#endif
