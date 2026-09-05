/*
 * The fonts on this machine.
 *
 * The same shape as the wallpaper list, and for the same reason: a font is a
 * file somebody chose, it comes from somewhere else on the disk, and the
 * system has to be able to list what it has without the chooser knowing where
 * any of it is kept.
 *
 * What it does not do is decide anything about text. Setting the font is a
 * key in the registry and a call to `recon_access_apply`, which is where it
 * was before this file existed; this only answers "which fonts are there" and
 * "put a copy of that one here".
 */
#ifndef RECON_FONTS_H
#define RECON_FONTS_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Where fonts installed for the whole system live.
 *
 * Under /System because a font is not one account's: two people signed in to
 * the same machine looking at different typefaces is a setting, and the file
 * behind it is not.
 */
#define RECON_DIR_FONTS "/System/Fonts"

/* Where each installed font came from, so one somebody added can be told
 * apart from one that ships -- and only what was added can be removed. */
#define RECON_FONT_ORIGINS "/System/Config/font-origins.txt"

/* How many are installed. */
int recon_fonts_count(void);

/* The name of the one at `index`, in the order they are listed. */
bool recon_fonts_at(int index, char *out, size_t size);

/* The full path to an installed font, for handing to `recon_font_load`. */
bool recon_fonts_path(const char *name, char *out, size_t size);

/*
 * Copy a font file into the system's own folder.
 *
 * `cwd` is where a relative path is relative to. The name it lands under
 * comes back in `name_out`, which is not always the name it had: two folders
 * can each hold an Inter.ttf and the second must not replace the first.
 */
bool recon_fonts_add(const char *cwd, const char *path, char *name_out,
    size_t name_size);

/* Where an installed font came from, or false for one that ships. */
bool recon_fonts_origin(const char *name, char *out, size_t size);

/*
 * Take one out.
 *
 * Only one somebody added: what ships is a preset, and a preset that can be
 * deleted is gone for good with nowhere to get it back from. Refuses the one
 * currently in use as well -- taking the typeface out from under a running
 * desktop leaves it with nothing to draw text with.
 */
bool recon_fonts_remove(const char *name);

/* Why the last call refused. */
const char *recon_fonts_last_error(void);

#endif /* RECON_FONTS_H */
