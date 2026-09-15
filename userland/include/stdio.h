/*
 * <stdio.h>, cut to what the desktop calls.
 *
 * Eleven functions, measured rather than chosen: fclose 47, fopen 26, fread
 * 13, fgets 9, fseek 7, ftell 7, rewind 7, fprintf 5, fwrite 4, fgetc 1,
 * fflush 1 -- plus snprintf and vsnprintf, which are the most-called functions
 * in the whole system at 952 between them.
 *
 * **What is absent is absent on purpose**, and a program using one of these
 * fails to compile rather than linking against something that half works:
 * there is no `printf` to a stream this cannot flush usefully, no `fscanf`,
 * no `fputs`, no `feof`, no `ferror`, no `tmpfile`, and no `"a"` mode. Each of
 * those is a decision recorded in `libc/stdio.c`.
 */

#ifndef RECON_STDIO_H
#define RECON_STDIO_H

#include <stdarg.h>
#include <stddef.h>

/*
 * A stream is an opaque struct. Its shape is `libc/stdio.c`'s business, and a
 * program that reached into it would be a program that breaks when the buffer
 * size changes.
 */
struct recon_stream;
typedef struct recon_stream FILE;

extern struct recon_stream *recon_stdin;
extern struct recon_stream *recon_stdout;
extern struct recon_stream *recon_stderr;

#define stdin  recon_stdin
#define stdout recon_stdout
#define stderr recon_stderr

/*
 * -1, and it is an `int` for the reason `fgetc` returns one: every byte value
 * from 0 to 255 is a legitimate result, so the end has to be a value that is
 * not a byte.
 */
#define EOF (-1)

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *f);
int fflush(FILE *f);

unsigned long fread(void *into, unsigned long size, unsigned long count,
		    FILE *f);
unsigned long fwrite(const void *from, unsigned long size,
		     unsigned long count, FILE *f);
char *fgets(char *into, int room, FILE *f);
int fgetc(FILE *f);

/*
 * A line to standard output. Nothing in the desktop calls it by name: the
 * compiler rewrites a `printf` with no conversions in it into this, which is
 * why it is here and why no grep of the source could have said so.
 */
int puts(const char *text);

int fseek(FILE *f, long offset, int from);
long ftell(FILE *f);
void rewind(FILE *f);

int fprintf(FILE *f, const char *format, ...)
	__attribute__((format(printf, 2, 3)));

int snprintf(char *to, size_t room, const char *format, ...)
	__attribute__((format(printf, 3, 4)));
int vsnprintf(char *to, size_t room, const char *format, va_list args)
	__attribute__((format(printf, 3, 0)));

/* Reading values back out of text.
 *
 * The attribute is `scanf` rather than `printf`, which is not a detail: it is
 * what lets the compiler check that `%lu` was given an `unsigned long *` and
 * not an `unsigned long`. A scanner handed a value where it wanted an address
 * writes through whatever that value happens to be.
 *
 * Scansets -- `%[a-z]` -- and `%p` are not implemented. A format holding one
 * **stops the scan** rather than skipping it, so a caller gets a short count
 * instead of a field in the wrong variable. See `userland/libc/scanf.c`.
 */
int sscanf(const char *text, const char *format, ...)
	__attribute__((format(scanf, 2, 3)));
int vsscanf(const char *text, const char *format, va_list args)
	__attribute__((format(scanf, 2, 0)));

#endif /* RECON_STDIO_H */
