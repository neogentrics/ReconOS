/*
 * What a package says it is, shared between the two halves of this module.
 *
 * `recon_manifest.c` reads a manifest and signs what it names.
 * `recon_package.c` installs, upgrades, verifies and removes.
 *
 * They were one file, and the consequence was the same one recon_http.c had:
 * testing the half that is arithmetic on bytes meant linking the half that
 * loads a shared object into the process. The signature tests could only be
 * built with a stub file that aborted on the module loader, the icon cache
 * and the version comparison -- five functions stubbed to make one file
 * testable, which is the shape of a file that wants splitting.
 *
 * Private on purpose: this is in src/ rather than include/, because a
 * manifest's internals are this module's business. Everything anybody else
 * needs is in include/recon_package.h.
 */

#ifndef RECON_MANIFEST_H
#define RECON_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>

#include "recon_fs.h"
#include "recon_package.h"

/* How many files a package may place, and how many settings it may set. */
#define PLACES_MAX 16
#define SETTINGS_MAX 16

struct place {
    char file[RECON_NAME_MAX];
    char into[RECON_PATH_MAX];
};

struct setting {
    char key[128];
    char value[128];
};

/*
 * What a manifest says, including the parts only the installer needs.
 *
 * Kept apart from recon_package_info, which is what anybody else is shown:
 * the name of the file inside the package holding the code is an installer's
 * business, and putting it in the public struct would invite somebody to act
 * on a path relative to a folder that may no longer be there.
 */
struct manifest {
    struct recon_package_info info;
    char module[RECON_NAME_MAX];
    char icon[RECON_NAME_MAX];

    struct place places[PLACES_MAX];
    int place_count;

    struct setting settings[SETTINGS_MAX];
    int setting_count;
};

/*
 * The error string both halves report through.
 *
 * One buffer, because `recon_package_last_error` is one function and a caller
 * that got a message from the wrong half would be told about a failure that
 * did not happen.
 */
void recon_package_set_error(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

/* Trailing spaces, tabs and carriage returns off the end. Deliberately not
 * newlines: this is for a manifest value, where the line has already been
 * split off. */
void recon_package_trim(char *text);

bool recon_package_read_manifest(const char *package, struct manifest *out);

/*
 * May a package put a file here?
 *
 * An allow-list, and the reasoning is in recon_package.h: a list of forbidden
 * places is a list somebody has to keep complete, and the day it is missing an
 * entry is the day a package writes into /System/Config.
 */
bool recon_package_place_allowed(const char *into);

#endif /* RECON_MANIFEST_H */
