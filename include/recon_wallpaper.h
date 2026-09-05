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

/* Where the origins of added pictures are written down. */
#define RECON_WALLPAPER_ORIGINS "/System/Config/wallpaper-origins.txt"

/*
 * Take a picture from anywhere on the system and make it a wallpaper.
 *
 * Copied into /System/Wallpapers rather than pointed at. A wallpaper that
 * lived at the far end of a path would stop existing the moment somebody
 * tidied their Pictures folder, and a desktop that loses its background
 * because a file moved is a desktop nobody trusts. The copy also keeps every
 * other part of this working unchanged: wallpapers are chosen by name, and a
 * name only means something inside one folder.
 *
 * Where it came from is written down beside it, because "Daybreak.png" and
 * "Daybreak.png" are the same name and different pictures, and somebody
 * looking at a list of names deserves to know which of their folders each one
 * came out of.
 *
 * `name_out` receives the name it was filed under, which may differ from the
 * file's own if that name was taken. False if the file cannot be read or is
 * not a picture.
 */
bool recon_wallpaper_add(const char *cwd, const char *path, char *name_out,
    size_t name_size);

/*
 * Where an added wallpaper came from, or "" for one that ships.
 *
 * Shortened from the left when it is long: the end of a path says which
 * folder, and the beginning says things everybody already knows.
 */
bool recon_wallpaper_origin(const char *name, char *out, size_t size);

/*
 * Take one out of the list.
 *
 * Only one somebody added: the pictures that ship are presets, and a preset
 * that can be deleted is a preset that is gone for good with nowhere to get
 * it back from. The same rule the skins and the firewall's rules follow.
 *
 * Refuses the one currently showing, too. Deleting the background out from
 * under a desktop leaves it with nothing to draw.
 */
bool recon_wallpaper_remove(const char *name);

/* Why the last call refused. */
const char *recon_wallpaper_last_error(void);

#endif
