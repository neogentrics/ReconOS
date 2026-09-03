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
#define RECON_ICON_SHUTDOWN "shutdown"
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
