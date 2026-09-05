/*
 * The default icon set, drawn by ReconOS rather than shipped with it.
 *
 * Generating them means the system has a complete set the moment it first
 * runs, with nothing to install and nothing borrowed. They are written as
 * ordinary files in /System/Icons, so any one of them can be replaced by
 * dropping a different file over it.
 */

#ifndef RECON_ICON_GEN_H
#define RECON_ICON_GEN_H

#include <stdbool.h>

/*
 * The subdirectory holding the same icons with a curved-glass treatment.
 *
 * A skin asks for them by setting metric.icon-gloss, and recon_icons looks here
 * before it looks at the flat set. Named here rather than written out in both
 * places, because the generator and the loader agreeing about it is the whole
 * of how this works.
 */
#define RECON_ICONS_GLOSSY "Glossy"

/*
 * Write the default icons, returning how many were written. Existing files are
 * left alone unless overwrite is set: a replaced icon should stay replaced.
 *
 * Every icon is written twice -- flat, and again into RECON_ICONS_GLOSSY -- so
 * the count is about double the number of icons.
 */
int recon_icons_write_defaults(bool overwrite);

#endif
