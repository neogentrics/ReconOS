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

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/*
 * Copy text into a fixed buffer, cutting it short if it does not fit.
 *
 * For the places where a shorter answer is the right answer: a window title
 * with room for a name and not a path, a status line quoting something
 * somebody typed, a note about why a module would not load. Cutting is what
 * those want, and `snprintf(out, size, "%s", text)` was how they said it.
 *
 * That reads as a formatting call, so the compiler has to warn that it might
 * truncate -- it cannot tell an intended cut from an accidental one. Fourteen
 * such warnings stood in this build, and a build with fourteen warnings in it
 * is a build where the fifteenth is invisible. This says the same thing in a
 * way that names the intent, so what is left on the list is what nobody meant.
 *
 * Also for a copy whose input is already known to fit -- a path component into
 * a buffer the size of a path component -- where the cut cannot happen and is
 * there so the bound is one the compiler can see too.
 *
 * Where a truncated answer would be *wrong* and could actually happen -- a
 * path that has to resolve, a name something will be looked up by -- do not
 * use this. Use recon_fs_join, or refuse the input, and say so.
 *
 * Inline rather than in a source file because half a dozen test binaries link
 * a handful of these files each, and a new object for five lines would mean
 * adding it to every one of those lists and to every one added later.
 */
static inline void recon_text_copy(char *out, size_t size, const char *text) {
    if (out == NULL || size == 0) {
        return;
    }
    if (text == NULL) {
        out[0] = '\0';
        return;
    }

    size_t length = strlen(text);
    if (length >= size) {
        length = size - 1;
    }
    memcpy(out, text, length);
    out[length] = '\0';
}

/*
 * The same, for a message that is built rather than copied: a status line
 * quoting a path, a dialog naming what went wrong.
 *
 * Only for text a person reads. Everything said above about where not to use
 * recon_text_copy applies here twice over, because this one hides the warning
 * for every argument at once.
 */
__attribute__((format(printf, 3, 4)))
static inline void recon_text_printf(char *out, size_t size,
        const char *format, ...) {
    if (out == NULL || size == 0) {
        return;
    }

    va_list args;
    va_start(args, format);
    vsnprintf(out, size, format, args);
    va_end(args);
}

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
