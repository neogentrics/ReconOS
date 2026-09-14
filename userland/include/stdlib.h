/*
 * <stdlib.h>, and the shape of it is a statement about what this kernel has.
 *
 * **There is no `malloc` here.** Not because it is hard, but because there is
 * nothing underneath it: a program on this kernel has no way to ask for
 * memory. That is the first entry in `docs/KERNEL-WANTS.md`, with the
 * measurements -- 430 call sites, a largest single request of just under four
 * megabytes, and eighty-one megabytes live for a browser window.
 *
 * A program that calls `malloc` therefore fails to **link**, with a message
 * naming the symbol. That is the right failure: the alternative is a stub
 * returning NULL, which every caller would treat as "out of memory" and report
 * as such -- so a desktop built against it would come up insisting the machine
 * was full.
 */

#ifndef RECON_STDLIB_H
#define RECON_STDLIB_H

#include <stddef.h>

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
