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
 * Write the default icons, returning how many were written. Existing files are
 * left alone unless overwrite is set: a replaced icon should stay replaced.
 */
int recon_icons_write_defaults(bool overwrite);

#endif
