/*
 * Packages: how a program arrives, and how it leaves.
 *
 * Installing was `install <file.rex>`: copy one shared object into /Apps and
 * load it. That works for a program which is only code, and every program is
 * more than that -- an icon, a default setting, a file it ships alongside
 * itself. There was nowhere to put any of it, so the Calculator's icon is
 * drawn by ReconOS rather than supplied by the Calculator.
 *
 * There was also no record of what an install had put anywhere. Uninstalling
 * deleted the one file it knew about, which is only correct while that is the
 * only file there was.
 *
 * A package is a folder with a manifest in it:
 *
 *     Notes.rpk/
 *         package.txt          what it is, and what to place where
 *         Notes.rex            the code
 *         notes.png            an icon
 *
 * A folder rather than an archive, because ReconOS cannot read one. Writing a
 * container format is real work that buys nothing yet: nothing here is sent
 * over a network, and a folder can be copied, inspected and repaired with the
 * tools the system already has. A single-file form can wrap this later
 * without the manifest changing.
 *
 * The manifest is the same shape as a skin file -- `key = value`, one per
 * line, anything unknown skipped -- because a person who has edited one has
 * already learnt how to read the other.
 *
 *     name        = Notes
 *     version     = 1.0
 *     publisher   = Somebody
 *     description = A place to put things
 *     module      = Notes.rex
 *     icon        = notes.png
 *
 * `module` and `icon` name files inside the package. Everything else is
 * description.
 */

#ifndef RECON_PACKAGE_H
#define RECON_PACKAGE_H

#include <stdbool.h>
#include <stddef.h>

/* What a package folder is called, and what its manifest is called. */
#define RECON_PACKAGE_EXT ".rpk"
#define RECON_PACKAGE_MANIFEST "package.txt"

/*
 * Where the receipts live: one file per installed package, listing every
 * path the install placed.
 *
 * A receipt rather than working it out again at removal time. What an install
 * placed is a fact about that install, and rederiving it from a manifest that
 * may have been edited, or from a package folder that may be gone, is
 * guessing about somebody else's disk.
 */
#define RECON_DIR_RECEIPTS "/System/Installed"

struct recon_package_info {
    char name[64];
    char version[32];
    char publisher[64];
    char description[128];
};

/* Read a package's manifest without installing it, so a caller can say what
 * it is about to do. False with recon_package_last_error() set. */
bool recon_package_read(const char *path, struct recon_package_info *out);

/*
 * Install the package at `path`.
 *
 * Everything it places is written down first and put in place second, so a
 * failure halfway leaves a receipt that names what did land -- which is what
 * makes the mess removable rather than permanent.
 *
 * Administrator only: a module runs inside ReconOS with everything ReconOS
 * can do, which is closer to installing a driver than to saving a file.
 */
bool recon_package_install(const char *path);

/*
 * Remove an installed package and everything its receipt names.
 *
 * A file the receipt names but which is not there any more is not an error:
 * somebody deleting a file by hand should not make the package impossible to
 * uninstall afterwards.
 */
bool recon_package_uninstall(const char *name);

/* What is installed, in the order the receipts were found. */
int recon_package_count(void);
bool recon_package_at(int index, struct recon_package_info *out);
bool recon_package_installed(const char *name);

const char *recon_package_last_error(void);

#endif
