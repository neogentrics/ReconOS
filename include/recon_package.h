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
 *     place       = paper.png /System/Wallpapers
 *     setting     = notes/wrap on
 *
 * `module` and `icon` name files inside the package. `place` and `setting` may
 * each appear as often as needed. Everything else is description.
 *
 * --- Placing a file ---
 *
 *     place = <file in the package> <where it goes>
 *
 * A program is more than code: a wallpaper, a skin, a sound, a document it
 * ships with. Before this there was nowhere to declare any of it, so a package
 * could bring exactly one module and exactly one icon.
 *
 * **Where it may go is an allow-list, not a check on what is forbidden.** A
 * package may write into the directories that exist to hold content -- icons,
 * wallpapers, themes, fonts -- and nowhere else. Not /System/Config, which
 * holds the accounts file. Not /System/Modules, which is loaded at startup.
 * Not a user's folder, because a package is installed once by an administrator
 * and there may be six users.
 *
 * Written as an allow-list because the other shape does not work: a list of
 * forbidden places is a list somebody has to keep complete, and the day it is
 * missing an entry is the day a package writes there. A list of permitted
 * places is wrong in the safe direction -- a package that wanted somewhere new
 * fails to install and somebody reads this comment.
 *
 * **A file already there is left alone**, and not recorded. That is the same
 * rule the icon has always followed and for the same reason: uninstalling a
 * package must not take away something that belonged to somebody else.
 *
 * --- Setting a default ---
 *
 *     setting = <key> <value>
 *
 * In the system's settings, because a package is installed once for everybody.
 *
 * **A default, not an override.** If the key already has a value it is left
 * alone: an install that changed settings somebody had chosen would be an
 * install that rearranged their desk. Only the ones it actually wrote go in
 * the receipt, so removing the package removes exactly those and leaves
 * anything it found already there.
 */

#ifndef RECON_PACKAGE_H
#define RECON_PACKAGE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Where a package may place a file.
 *
 * Directories that exist to hold content, and nothing else. See the note above
 * about why this is an allow-list.
 */
#define RECON_PACKAGE_PLACES_MAX 6

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
 * Replace an installed package with a newer build of itself.
 *
 * Its own verb rather than making `install` quietly replace things. Somebody
 * running an install twice by accident should get "that is already installed",
 * not a program silently swapped underneath them -- and an upgrade is a
 * decision worth having to make on purpose, because it is the only operation
 * here that removes something in order to succeed.
 *
 * --- The version rule ---
 *
 * Strictly newer, and refused otherwise. The same version is not an upgrade
 * and reinstalling to fix a broken install is what `uninstall` then `install`
 * is for; an older one is a downgrade, which is a different decision with
 * different consequences and should not arrive by way of a button labelled
 * "upgrade". A package whose version cannot be read is refused too: without
 * two numbers there is nothing to compare and "probably newer" is not a
 * property to move somebody's files on.
 *
 * --- What happens to the old one ---
 *
 * **It is moved aside, not deleted.** Every file the old receipt names is
 * renamed with a suffix, the new package is installed over the gap, and only
 * once that has worked -- including the module loading -- are the old files
 * removed. Anything that fails puts them back.
 *
 * The obvious implementation is uninstall-then-install, and it is wrong in a
 * specific way: an upgrade that fails halfway has removed a program that
 * worked and put nothing in its place. Somebody who was trying to *get a
 * newer version* now has no version, which is worse than what they started
 * with and worse than being refused.
 *
 * Settings the old install wrote are left alone. It wrote them only where
 * there was no value, so they are the ones somebody may since have changed,
 * and an upgrade is not an occasion to reset a preference.
 */
bool recon_package_upgrade(const char *path);

/*
 * Remove an installed package and everything its receipt names.
 *
 * A file the receipt names but which is not there any more is not an error:
 * somebody deleting a file by hand should not make the package impossible to
 * uninstall afterwards.
 */
bool recon_package_uninstall(const char *name);

/*
 * Check that everything an install placed is still there.
 *
 * `placed` comes back as the number of files the receipt names and `missing`
 * as how many of them are gone; `first_missing` names one of them, for a
 * message that can say something rather than a count.
 *
 * This checks; it does not put anything back. Putting a file back needs the
 * package it came from, and nothing keeps one -- a package is a folder
 * somebody had, on their disk, which they are free to delete the moment the
 * install finishes. Saying so is more use than a Repair button that quietly
 * does nothing.
 */
bool recon_package_verify(const char *name, int *placed, int *missing,
    char *first_missing, size_t size);

/* What is installed, in the order the receipts were found. */
int recon_package_count(void);
bool recon_package_at(int index, struct recon_package_info *out);
bool recon_package_installed(const char *name);

const char *recon_package_last_error(void);

#endif
