/*
 * The picture behind everything.
 *
 * Wallpapers are files in /System/Wallpapers, chosen by name, the same way
 * icons and skins already work. Which one is showing is a per-account
 * setting, because a desktop is a person's and two people sharing a machine
 * should not share a background.
 *
 * A skin can name one it goes with. That is a default rather than a rule:
 * choosing a skin puts its wallpaper on, and choosing a wallpaper afterwards
 * keeps it until the skin changes again. The alternative -- a skin that owns
 * the background -- means a person who liked one picture cannot keep it.
 *
 * ReconOS draws several of these itself at first run, for the same reason it
 * draws its own icons: a system with nothing installed should still look like
 * something. The photograph that ships with it is one option among them.
 */

#ifndef RECON_WALLPAPER_H
#define RECON_WALLPAPER_H

#include <stdbool.h>
#include <stddef.h>

/* Where they live, and where the choice is kept. */
#define RECON_DIR_WALLPAPERS "/System/Wallpapers"
#define RECON_WALLPAPER_KEY "desktop/wallpaper"

/*
 * Write the wallpapers ReconOS draws for itself, if they are not there.
 * Returns how many were written. An existing file is left alone, so one
 * somebody replaced stays replaced.
 */
int recon_wallpapers_write_defaults(void);

int recon_wallpaper_count(void);
bool recon_wallpaper_at(int index, char *name, size_t size);

/*
 * The wallpaper that should be showing: the account's choice, or failing
 * that the current skin's, or failing that whatever is there. Empty only when
 * there are none at all.
 */
const char *recon_wallpaper_current(void);

/* Choose one, by file name. Pass NULL or "" to go back to following the
 * skin. False if there is no such file. */
bool recon_wallpaper_set(const char *name);

#endif
