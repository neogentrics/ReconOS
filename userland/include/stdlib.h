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

/* --- What a program may ask about its own heap ---------------------------
 *
 * Not standard, and `recon_` for that reason. A program that reports memory
 * use should not have to keep its own running total beside the allocator's,
 * because the two disagreeing is a bug that can only be found by reading both.
 *
 * `recon_malloc_audit` walks the heap and returns the number of things it
 * found wrong -- a size that disagrees with its footer, a free block that no
 * list holds, a block that overlaps its neighbour. **Zero is the only good
 * answer**, and it is a number rather than a bool because which faults and how
 * many is the whole of what makes one actionable.
 *
 * `recon_malloc_refused` counts the times the source said no, which is what
 * tells a program out of memory apart from a program with a broken heap. On a
 * kernel with no anonymous memory it was the only number that moved.
 */
unsigned recon_malloc_audit(void);
size_t recon_malloc_held(void);		/* held from the kernel right now */
size_t recon_malloc_live(void);		/* what blocks in use hold */
size_t recon_malloc_refused(void);	/* times the kernel said no */

int atoi(const char *text);
long long atoll(const char *text);
double atof(const char *text);

long strtol(const char *text, char **end, int base);
long long strtoll(const char *text, char **end, int base);
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
