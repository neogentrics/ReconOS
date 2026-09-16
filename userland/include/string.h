/*
 * <string.h> for programs built against the ReconOS C library.
 *
 * The point of this header existing at all is that desktop source says
 * `#include <string.h>` in seventy-four places and should keep saying it. A
 * port that required ninety thousand lines to be edited would not be a port.
 *
 * The declarations match the standard exactly, including the `restrict`
 * qualifiers being absent -- adding them would be a promise about aliasing
 * that this library's implementations do not need and that callers have not
 * been checked against.
 */

#ifndef RECON_STRING_H
#define RECON_STRING_H

#include <stddef.h>

void *memcpy(void *to, const void *from, size_t length);
void *memmove(void *to, const void *from, size_t length);
void *memset(void *to, int value, size_t length);
int memcmp(const void *a, const void *b, size_t length);
void *memchr(const void *in, int value, size_t length);

size_t strlen(const char *text);

/* Declared here and implemented in malloc.c, because it is an allocation that
 * happens to copy a string rather than a string routine that happens to
 * allocate: a program with no allocator cannot have it at all. */
char *strdup(const char *text);
size_t strnlen(const char *text, size_t most);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t length);
char *strcpy(char *to, const char *from);
char *strncpy(char *to, const char *from, size_t length);
char *strcat(char *to, const char *from);
char *strchr(const char *text, int value);
char *strrchr(const char *text, int value);
char *strstr(const char *haystack, const char *needle);
size_t strspn(const char *text, const char *of);
size_t strcspn(const char *text, const char *stop);

/*
 * The reentrant spelling only. Forty-four call sites in the desktop and every
 * one of them uses it; plain `strtok` keeps its place in a static, which is
 * the wrong thing to put in a library this system will link into code that can
 * run on two cores at once.
 */
char *strtok_r(char *text, const char *separators, char **save);

/*
 * Two GNU extensions rather than standard functions, each here for one
 * caller: `recon_html.c` finds the end of a comment in a page that may hold a
 * zero byte, and `recon_http.c` looks for "chunked" in a header whose value is
 * case-insensitive by specification.
 */
void *memmem(const void *haystack, size_t haystack_length,
	     const void *needle, size_t needle_length);
char *strcasestr(const char *haystack, const char *needle);
char *strncat(char *to, const char *from, size_t length);

/*
 * What an errno value means, in words.
 *
 * It lives in `libc/errno.c` beside the numbering it describes, and it has
 * been written and tested since that file was; it was declared in no header at
 * all, so every one of the desktop's forty-three call sites was reaching it by
 * implicit declaration -- which C11 does not allow, and which the freestanding
 * build reported the moment `recon_fs.c` was offered to it.
 *
 * Returns a pointer into static storage. That is what `strerror` has always
 * been and is why `strerror_r` exists; the note in `libc/errno.c` says when it
 * starts to matter.
 */
char *strerror(int number);

#endif /* RECON_STRING_H */
