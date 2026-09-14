/*
 * <strings.h>: the case-insensitive comparisons.
 *
 * A separate header because that is where they live on every system this
 * source has been compiled on, and thirty-four files include it by that name.
 *
 * The folding is ASCII only and permanently so; `libc/string.c` says why, and
 * the short version is that all 357 callers are comparing something whose
 * spelling is defined by a standard somebody else wrote.
 */

#ifndef RECON_STRINGS_H
#define RECON_STRINGS_H

#include <stddef.h>

int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t length);

#endif /* RECON_STRINGS_H */
