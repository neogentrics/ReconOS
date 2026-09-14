/*
 * <ctype.h>, ASCII and no locale. `libc/ctype.c` argues for that at length.
 *
 * Functions rather than macros, deliberately. The classic implementation is a
 * table lookup behind a macro, which is faster and is undefined for a negative
 * argument -- and a caller passing a `char` on a machine where it is signed
 * passes a negative for every byte above 127, which is every non-English
 * character in a UTF-8 system. These are functions that check both ends of the
 * range, and the thirty-seven call sites are not in a loop that would notice.
 */

#ifndef RECON_CTYPE_H
#define RECON_CTYPE_H

int isalnum(int c);
int isalpha(int c);
int isdigit(int c);
int isspace(int c);
int isupper(int c);
int islower(int c);
int isxdigit(int c);
int isprint(int c);
int toupper(int c);
int tolower(int c);

#endif /* RECON_CTYPE_H */
