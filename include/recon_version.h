/*
 * Version numbers, and comparing them.
 *
 * ReconOS has versions in three places -- the system itself, a module, and an
 * application inside a module -- and until now they were all free-form strings
 * that nothing ever compared. This is the type that makes them answerable.
 *
 * --- Why this is not strcmp ---
 *
 * Because strcmp says 1.10 is older than 1.9. It compares '1' against '9' at
 * the third character, finds '1' smaller, and stops. Every version scheme in
 * the world reaches a tenth release eventually, and a system that decided
 * update ordering with strcmp would, on that day, start refusing the newer
 * applet as older -- silently, and with an error message saying the opposite
 * of the truth.
 *
 * So a version is three numbers here, and comparing them is comparing numbers.
 *
 * --- Why a bad version is refused rather than treated as zero ---
 *
 * A typo in a version string is somebody's mistake, and the two ways to handle
 * it are not equally bad. Treating "1.O.0" as 0.0.0 makes that applet the
 * oldest thing in the system: every other build of it counts as newer, so it
 * gets replaced by anything at all and never replaces anything. That is a
 * mistake that hides. Refusing it says so at the moment it is loaded, while
 * somebody is still looking.
 */

#ifndef RECON_VERSION_H
#define RECON_VERSION_H

#include <stdbool.h>
#include <stddef.h>

/* "255.255.255" and its terminator, with room to spare. */
#define RECON_VERSION_MAX 32

struct recon_version {
    int major;
    int minor;
    int patch;
};

/*
 * Read "1", "1.2" or "1.2.3". Parts not given are zero, so "1.2" is 1.2.0 --
 * which is what somebody writing "1.2" means, and the only reading under which
 * "1.2" and "1.2.0" are the same release.
 *
 * A leading 'v' is accepted, because tags carry one and somebody will
 * eventually paste one in.
 *
 * False for anything else: empty, negative, more than three parts, or with
 * something that is not a digit in it. See the header comment for why this
 * refuses rather than shrugs.
 */
bool recon_version_parse(const char *text, struct recon_version *out);

/* Less than zero, zero, or greater than zero, the way strcmp reads -- but by
 * number. */
int recon_version_compare(const struct recon_version *a,
    const struct recon_version *b);

/*
 * Write it out as "1.2.3", always with all three parts.
 *
 * Always three, even when the input said "1.2", so a list of versions lines up
 * and two spellings of one release do not look like two releases.
 */
void recon_version_format(const struct recon_version *version, char *out,
    size_t size);

/*
 * Compare two version strings directly, for the common case.
 *
 * `unparseable` is set true when either side would not parse, and the return
 * is 0 -- which means "cannot say", not "equal". A caller that cares must look
 * at the flag; a caller deciding whether to replace something should treat
 * "cannot say" as "do not replace", because the safe answer to an unreadable
 * version is to leave the working thing where it is.
 */
int recon_version_compare_text(const char *a, const char *b,
    bool *unparseable);

#endif
