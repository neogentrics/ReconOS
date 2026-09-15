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
char *strtok_r(char *text, const char *separators, char **save);
void *memmem(const void *haystack, size_t haystack_length,
	     const void *needle, size_t needle_length);
char *strcasestr(const char *haystack, const char *needle);
char *strncat(char *to, const char *from, size_t length);

/* stdio.c */
int puts(const char *text);

/* printf.c */
int snprintf(char *to, size_t room, const char *format, ...);
int vsnprintf(char *to, size_t room, const char *format, va_list args);

/* scanf.c */
int sscanf(const char *text, const char *format, ...);
int vsscanf(const char *text, const char *format, va_list args);

/* scanf.c */
int sscanf(const char *text, const char *format, ...);
int vsscanf(const char *text, const char *format, va_list args);

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
unsigned long long strtoull(const char *text, char **end, int base);
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
/*
 * The two clocks, in nanoseconds, and they answer different questions.
 * `recon_sys_time` never goes backwards and means nothing outside this boot;
 * `recon_sys_walltime` is the date and can jump.
 */
long long recon_sys_time(void);
long long recon_sys_walltime(void);

long recon_sys_open(const char *path, unsigned long flags);
long recon_sys_read(int fd, void *into, unsigned long length);
long recon_sys_write(int fd, const void *from, unsigned long length);
long recon_sys_seek(int fd, long long offset, int from);
long recon_sys_close(int fd);

/* Every name in a directory, in one call, NUL-terminated and back to back.
 * Answers the size of the whole listing whether or not it fitted, so asking
 * with no room is how a caller finds out how much to bring. Whole or nothing:
 * a caller handed the first half of a directory alongside a success has no way
 * to know. */
long recon_sys_list(const char *path, char *names, unsigned long names_len);

long recon_sys_mkdir(const char *path, unsigned long mode);

/* How big a page is, which is the machine's to say rather than a constant in a
 * header. -1 if it cannot be asked. */
long recon_sys_page_size(void);

#define RECON_SEEK_SET 0
#define RECON_SEEK_CUR 1
#define RECON_SEEK_END 2

#define RECON_O_READ  (1u << 0)
#define RECON_O_WRITE (1u << 1)

/* --- malloc.c -------------------------------------------------------------
 *
 * Where memory comes from, as two function pointers rather than a system call
 * named in the allocator. See malloc.c for why, and `mem_recon.c` for the
 * source ReconOS installs.
 *
 * `take` answers a range of at least `bytes`, or NULL. `give_back` answers 0
 * if the range is gone and anything else if it cannot be given back -- which
 * is not an error: it is the configuration ReconOS is in today, and the
 * allocator keeps and reuses the range instead.
 */
struct recon_memory_source {
	void *(*take)(size_t bytes);
	int (*give_back)(void *at, size_t bytes);
};

void recon_memory_from(const struct recon_memory_source *from);

/* Weak in malloc.c and strong in mem_recon.c: linking the latter is what gives
 * a program memory from the kernel. */
void recon_memory_default(void);

void *malloc(size_t bytes);
void *calloc(size_t count, size_t each);
void *realloc(void *p, size_t bytes);
void free(void *p);
char *strdup(const char *text);

/* What the allocator will say about itself, for the suite and for anything
 * that wants to report memory use. Counted as it happens rather than derived
 * by walking the heap, so that `recon_malloc_audit` comparing the two means
 * something. */
struct recon_malloc_stats {
	size_t taken_bytes;	/* held from the source right now */
	size_t live_bytes;	/* what blocks in use hold */
	size_t live_blocks;
	size_t regions;
	size_t takes;		/* ranges asked for, since the start */
	size_t gives;		/* ranges actually handed back */
	size_t refusals;	/* times the source said no */
};

void recon_malloc_stats(struct recon_malloc_stats *into);
unsigned recon_malloc_audit(void);
void recon_malloc_reset(void);

/* --- errno.c --------------------------------------------------------------
 *
 * `errno` itself is declared in `userland/include/errno.h`, which this file's
 * one other exception -- see the note at the bottom -- lets errno.c include
 * directly.
 */
char *strerror(int number);

/* --- posix.c --------------------------------------------------------------
 *
 * The types and structs these use are in the public headers, which posix.c
 * includes by relative path -- the same exception time.c and errno.c take.
 * Declared here only so the rest of the library can call them.
 */
char *realpath(const char *path, char *into);

/* What a system call's answer means in C's numbering. Negative in, `errno`
 * out; 0 for anything that did not fail. One place, so that every wrapper in
 * this library has a line for failure rather than a table of its own. */
int recon_errno_from_status(long status);

/* How many kernel error numbers that translation knows about. Asked by the
 * suite and compared against the kernel's own list, because a lookup table's
 * failure is going quietly out of date -- and an untranslated error becomes a
 * plausible-looking EIO, which sends somebody to look at a disk that is fine. */
unsigned recon_errno_count(void);

/*
 * --- Time is not declared here ---
 *
 * `libc/time.c` includes `userland/include/time.h` directly, which is the one
 * place in this library that reads a public header, and it is deliberate.
 *
 * The rule at the top of this file exists so the differential build cannot
 * compare a function against itself -- `libc/string.c` including <string.h>
 * would get whichever of the two copies the include path put first. That
 * cannot happen with <time.h>: nothing puts the host's on the library's
 * include path, and `prefix.h` arrives by `-include`, so it is processed
 * before that header and renames what it declares on the way past.
 *
 * What it buys is one definition of `struct tm` and one of `time_t` rather
 * than two that have to be kept byte-identical by hand.
 */

#endif /* RECON_LIBC_INTERNAL_H */
