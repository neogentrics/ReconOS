/*
 * What the C library's own sources share, and nothing else.
 *
 * Deliberately **not** the public headers in `userland/include/`. Those exist
 * so that desktop source can say `#include <string.h>` and compile unchanged;
 * this one exists so that the library's own files can call each other.
 *
 * Keeping them apart is what lets the whole library be compiled *for the host*
 * with every symbol renamed, and compared against the system's — if
 * `libc/string.c` included `<string.h>` it would get whichever of the two the
 * include path happened to put first, and the comparison would be a function
 * against itself.
 */

#ifndef RECON_LIBC_INTERNAL_H
#define RECON_LIBC_INTERNAL_H

#include <stdarg.h>
#include <stddef.h>

/* string.c */
void *memcpy(void *to, const void *from, size_t length);
void *memmove(void *to, const void *from, size_t length);
void *memset(void *to, int value, size_t length);
int memcmp(const void *a, const void *b, size_t length);
void *memchr(const void *in, int value, size_t length);
size_t strlen(const char *text);
size_t strnlen(const char *text, size_t most);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t length);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t length);
char *strcpy(char *to, const char *from);
char *strncpy(char *to, const char *from, size_t length);
char *strcat(char *to, const char *from);
char *strchr(const char *text, int value);
char *strrchr(const char *text, int value);
char *strstr(const char *haystack, const char *needle);
size_t strspn(const char *text, const char *of);
size_t strcspn(const char *text, const char *stop);

/* printf.c */
int snprintf(char *to, size_t room, const char *format, ...);
int vsnprintf(char *to, size_t room, const char *format, va_list args);

/* ctype.c */
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

/* stdlib.c */
int atoi(const char *text);
long long atoll(const char *text);
double atof(const char *text);
long strtol(const char *text, char **end, int base);
unsigned long strtoul(const char *text, char **end, int base);
double strtod(const char *text, char **end);
int abs(int value);
void qsort(void *base, size_t count, size_t size,
	   int (*compare)(const void *, const void *));
char *getenv(const char *name);

/*
 * The primitives the FILE layer is built on.
 *
 * Declared here rather than included from `recon.h`, because the host build of
 * this library — the one compared against the system's — needs them to reach
 * POSIX instead. `userland/tests/hostsys.c` provides that version; on ReconOS
 * they are the inline system calls.
 */
long recon_sys_open(const char *path, unsigned long flags);
long recon_sys_read(int fd, void *into, unsigned long length);
long recon_sys_write(int fd, const void *from, unsigned long length);
long recon_sys_seek(int fd, long long offset, int from);
long recon_sys_close(int fd);

#define RECON_SEEK_SET 0
#define RECON_SEEK_CUR 1
#define RECON_SEEK_END 2

#define RECON_O_READ  (1u << 0)
#define RECON_O_WRITE (1u << 1)

#endif /* RECON_LIBC_INTERNAL_H */
