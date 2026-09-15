/*
 * <stdlib.h>.
 *
 * **`malloc` is here now**, and what that sentence replaced is worth keeping:
 * this header used to say there was no `malloc` in it, so that a caller failed
 * to *link* rather than getting a stub that returned nothing. 430 call sites
 * in the desktop were behind that.
 *
 * `userland/libc/malloc.c` is a real allocator -- boundary tags, coalescing,
 * segregated free lists, and regions handed back when nothing in them is in
 * use. It takes its memory from two function pointers rather than from a
 * system call it names, which is what lets the whole of it be held against the
 * allocator it replaces on the host.
 *
 * **On ReconOS it needs a call that hands out anonymous memory, and there is
 * not one yet.** `mem_recon.c` asks `SYS_MAP` for a range with no file behind
 * it; today the kernel answers EBADF, so `malloc` answers NULL and says so
 * through `recon_malloc_stats().refusals`. The day that call exists this works
 * with nothing rebuilt. The entry is still first in `docs/KERNEL-WANTS.md`.
 */

#ifndef RECON_STDLIB_H
#define RECON_STDLIB_H

#include <stddef.h>

void *malloc(size_t bytes);
void *calloc(size_t count, size_t each);
void *realloc(void *p, size_t bytes);
void free(void *p);

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

/*
 * Always NULL on this kernel: a process is started with no environment. The
 * twenty callers are all asking the *host* something -- HOME, XDG_RUNTIME_DIR,
 * WAYLAND_DISPLAY -- and every one of them already handles the variable being
 * unset, because on Linux it can be.
 */
char *getenv(const char *name);

void exit(int code) __attribute__((noreturn));

#endif /* RECON_STDLIB_H */
