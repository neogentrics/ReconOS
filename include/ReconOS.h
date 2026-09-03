/*
 * What ReconOS calls itself.
 *
 * The version comes from the project declaration in CMakeLists.txt, so the
 * running system, the `ver` command and the packaged tarball cannot disagree
 * about what they are. It used to be written out by hand in five places, which
 * is why they had drifted.
 *
 * This file was the one being worked on when the drive failed, and was kept
 * empty afterwards as a marker. It has a job now.
 */

#ifndef RECONOS_H
#define RECONOS_H

#ifndef RECONOS_VERSION
/* Only reached in a build that has not defined it -- a test binary, or a
 * module compiled on its own. */
#define RECONOS_VERSION "unknown"
#endif

/* The short name, used in the shell and the terminal. */
#define RECONOS_NAME "ReconOS"

/*
 * The long name, for the places that announce the system: the boot splash and
 * the login screen. ReconOS is the shorthand.
 */
#define RECONOS_FULL_NAME "Recon Towers OS"

#endif
