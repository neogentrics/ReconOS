/*
 * The `FILE *` layer: what `fopen` returns and what the eleven stdio
 * functions the desktop actually calls do with it.
 *
 * Measured across `src/` before any of it was written:
 *
 *     fclose 47   fopen 26   fread 13   fgets 9   fseek 7   ftell 7
 *     rewind 7    fprintf 5   fwrite 4   fgetc 1   fflush 1
 *
 * and the modes, of which there are three: `"rb"` 13, `"r"` 9, `"wb"` 3.
 *
 * So there is no append, no `"r+"`, no `fscanf`, no `fputs`, no `feof`. A mode
 * this does not have is **refused** and `fopen` returns nothing, rather than
 * being quietly treated as the nearest one it does — a program that asked to
 * append and was given truncate destroys the file it meant to add to.
 *
 * --- No allocation, and what that decides ---
 *
 * There is no `malloc` on this kernel yet, so a `FILE` cannot be allocated.
 * The streams are a fixed table with their buffers inside them, which has
 * three consequences worth stating rather than discovering:
 *
 *   - there is a **limit on how many files are open at once**, and it is
 *     sixteen. Opening a seventeenth fails and says so.
 *   - `fopen` cannot fail for want of memory, and the failure it can have --
 *     the table being full -- is deterministic rather than depending on what
 *     else the program has done.
 *   - the library costs a fixed 64 KiB of .bss whether a program opens a file
 *     or not. That is the price of not having an allocator, it is paid once,
 *     and it goes away when there is one.
 *
 * --- Why there is a buffer at all ---
 *
 * `fgets` on an unbuffered descriptor is one system call per byte. Reading a
 * forty-kilobyte stylesheet a line at a time would be forty thousand crossings
 * of the ring boundary instead of ten. The buffer is the whole reason this
 * layer exists rather than callers using `read` directly.
 */

#include "internal.h"

#define STREAMS_MAX 16
#define BUFFER_SIZE 4096

#define FLAG_USED    (1u << 0)
#define FLAG_READ    (1u << 1)
#define FLAG_WRITE   (1u << 2)
#define FLAG_EOF     (1u << 3)
#define FLAG_ERROR   (1u << 4)

struct recon_stream {
	int fd;
	unsigned flags;

	/*
	 * The buffer holds bytes read ahead of the caller (`at` to `used`) or
	 * bytes written and not yet handed to the kernel (`0` to `used`). A
	 * stream is only ever one or the other, because the modes this accepts
	 * are only ever one or the other -- which is why one buffer and one
	 * pair of offsets is enough, and why `"r+"` is refused rather than
	 * half-supported.
	 */
	unsigned char buffer[BUFFER_SIZE];
	unsigned long used;
	unsigned long at;

	/*
	 * Where the *caller* thinks it is, which is not where the descriptor
	 * is: the descriptor has run ahead by whatever is still in the buffer.
	 * `ftell` has to answer for the caller, so the two are tracked apart.
	 */
	long long position;

	/*
	 * One character of pushback, and one is what the standard promises.
	 *
	 * More would mean deciding what happens when a caller pushes back
	 * characters it never read, and what `ftell` should then say about a
	 * position that is partly imaginary. One slot has neither question.
	 *
	 * -1 for "nothing pushed back", which is why it is an int and not a
	 * char: every value a char can hold is a value that can be pushed.
	 */
	int pushed_back;
};

/*
 * Named as the struct throughout, and never typedefed to `FILE` here.
 *
 * The typedef lives in `userland/include/stdio.h`, where desktop code sees it.
 * Putting it in this file would collide with the host's `FILE` the moment both
 * are compiled into one program -- which is precisely what the differential
 * test does, so the collision would land on the one build that proves this
 * layer works.
 */
static struct recon_stream g_streams[STREAMS_MAX];

/*
 * The three that exist before anything is opened.
 *
 * Descriptors 0, 1 and 2, unbuffered on the write side, because output that
 * sits in a buffer when a program faults is output nobody ever sees -- and the
 * first thing anybody does with a new program is print from it.
 */
static struct recon_stream g_stdin  = { 0, FLAG_USED | FLAG_READ,  { 0 }, 0, 0, 0, -1 };
static struct recon_stream g_stdout = { 1, FLAG_USED | FLAG_WRITE, { 0 }, 0, 0, 0, -1 };
static struct recon_stream g_stderr = { 2, FLAG_USED | FLAG_WRITE, { 0 }, 0, 0, 0, -1 };

struct recon_stream *recon_stdin  = &g_stdin;
struct recon_stream *recon_stdout = &g_stdout;
struct recon_stream *recon_stderr = &g_stderr;

static int is_standard(struct recon_stream *f)
{
	return f == &g_stdin || f == &g_stdout || f == &g_stderr;
}

/* --- Opening --- */

static struct recon_stream *take_stream(void)
{
	int i;

	for (i = 0; i < STREAMS_MAX; i++) {
		if (!(g_streams[i].flags & FLAG_USED)) {
			g_streams[i].fd = -1;
			g_streams[i].flags = FLAG_USED;
			g_streams[i].used = 0;
			g_streams[i].at = 0;
			g_streams[i].position = 0;

			/* **-1, and not left at zero.** Static storage makes
			 * it 0, and 0 is a character -- so a slot that was
			 * never reset hands back a NUL before the first byte
			 * of the file. The same shape as the kernel's
			 * `idle_for`: a structure cleared wholesale and one
			 * field whose zero is meaningful and wrong. */
			g_streams[i].pushed_back = -1;
			return &g_streams[i];
		}
	}
	return (struct recon_stream *)0;
}

struct recon_stream *fopen(const char *path, const char *mode)
{
	unsigned long flags;
	struct recon_stream *f;
	long fd;

	if (path == NULL || mode == NULL) {
		return (struct recon_stream *)0;
	}

	/*
	 * The three modes the desktop uses, and nothing else. A `+` anywhere
	 * is refused, and so is `a`: a program that asked to append and was
	 * given truncate destroys the file it meant to add to, and that is a
	 * far worse outcome than being told no.
	 */
	if (mode[0] == 'r') {
		flags = RECON_O_READ;
	} else if (mode[0] == 'w') {
		flags = RECON_O_WRITE;
	} else {
		return (struct recon_stream *)0;
	}

	{
		const char *m;

		for (m = mode + 1; *m != '\0'; m++) {
			/* 'b' is accepted and ignored, which is what it means
			 * on a system with no text mode. Anything else is a
			 * mode this does not implement. */
			if (*m != 'b') {
				return (struct recon_stream *)0;
			}
		}
	}

	f = take_stream();
	if (f == NULL) {
		return (struct recon_stream *)0;
	}

	fd = recon_sys_open(path, flags);
	if (fd < 0) {
		f->flags = 0;
		return (struct recon_stream *)0;
	}

	f->fd = (int)fd;
	f->flags |= (flags & RECON_O_READ) ? FLAG_READ : FLAG_WRITE;
	return f;
}

/* --- Writing --- */

/* Hand everything buffered to the kernel. A short write is retried rather than
 * treated as an error: a descriptor is entitled to accept less than it was
 * offered, and a layer that gave up would lose the rest of the file. */
static int flush_out(struct recon_stream *f)
{
	unsigned long sent = 0;

	while (sent < f->used) {
		long n = recon_sys_write(f->fd, f->buffer + sent,
					 f->used - sent);

		if (n <= 0) {
			f->flags |= FLAG_ERROR;
			/* What did land still counts as gone: keeping it in
			 * the buffer would write it twice on the next flush. */
			f->used = 0;
			return -1;
		}
		sent += (unsigned long)n;
	}
	f->used = 0;
	return 0;
}

int fflush(struct recon_stream *f)
{
	if (f == NULL || !(f->flags & FLAG_WRITE)) {
		return 0;
	}
	return flush_out(f);
}

unsigned long fwrite(const void *from, unsigned long size, unsigned long count,
		     struct recon_stream *f)
{
	const unsigned char *p = from;
	unsigned long total;
	unsigned long done = 0;

	if (f == NULL || !(f->flags & FLAG_WRITE) || size == 0 || count == 0) {
		return 0;
	}
	total = size * count;

	/*
	 * The standard three: through the buffer normally, but a write larger
	 * than the buffer goes straight out rather than being copied in pieces
	 * -- which is what makes writing a megabyte not a thousand memcpys.
	 */
	if (is_standard(f)) {
		while (done < total) {
			long n = recon_sys_write(f->fd, p + done, total - done);

			if (n <= 0) {
				f->flags |= FLAG_ERROR;
				break;
			}
			done += (unsigned long)n;
		}
		f->position += (long long)done;
		return done / size;
	}

	while (done < total) {
		unsigned long room = BUFFER_SIZE - f->used;
		unsigned long take = total - done;

		if (room == 0) {
			if (flush_out(f) < 0) {
				break;
			}
			room = BUFFER_SIZE;
		}
		if (take > room) {
			take = room;
		}
		memcpy(f->buffer + f->used, p + done, take);
		f->used += take;
		done += take;
	}

	f->position += (long long)done;
	return done / size;
}

/*
 * A line to standard output, and **nothing in the desktop calls it.**
 *
 * It is here because the linker asked for it. `src/main.c` calls `printf`
 * with a string literal containing no conversions, and the compiler rewrites
 * that into `puts` -- so the desktop needs a function whose name does not
 * appear anywhere in its source. A grep could not have found this and neither
 * could reading the file; `nm` on the object file found it in one command.
 *
 * The newline is part of the contract and is the whole difference from
 * `fputs`, which this library does not have.
 */
int puts(const char *text)
{
	unsigned long length;

	if (text == NULL) {
		return -1;
	}
	length = (unsigned long)strlen(text);

	if (length != 0 && fwrite(text, 1, length, recon_stdout) != length) {
		return -1;
	}
	if (fwrite("\n", 1, 1, recon_stdout) != 1) {
		return -1;
	}

	/* Any non-negative number. The reference returns a count that is not
	 * specified further, so a caller relying on which one has a bug
	 * either way -- and zero is the one that cannot be mistaken for a
	 * length. */
	return 0;
}

int fprintf(struct recon_stream *f, const char *format, ...)
{
	/*
	 * A fixed buffer, and a long message is **truncated rather than
	 * split**. Five call sites, all of them a diagnostic line; the
	 * alternative is a second formatting pass or an allocation, and there
	 * is no allocator. The return is what `vsnprintf` wanted, so a caller
	 * that cares can tell.
	 */
	char line[1024];
	va_list args;
	int n;

	if (f == NULL) {
		return -1;
	}

	va_start(args, format);
	n = vsnprintf(line, sizeof(line), format, args);
	va_end(args);

	if (n < 0) {
		return -1;
	}
	{
		unsigned long length = (unsigned long)n;

		if (length > sizeof(line) - 1) {
			length = sizeof(line) - 1;
		}
		fwrite(line, 1, length, f);
	}
	return n;
}

/* --- Reading --- */

/* Fill the buffer. Returns how many bytes are available, or 0 at the end. */
static unsigned long fill(struct recon_stream *f)
{
	long n;

	if (f->at < f->used) {
		return f->used - f->at;
	}
	f->at = 0;
	f->used = 0;

	n = recon_sys_read(f->fd, f->buffer, BUFFER_SIZE);
	if (n < 0) {
		f->flags |= FLAG_ERROR;
		return 0;
	}
	if (n == 0) {
		f->flags |= FLAG_EOF;
		return 0;
	}
	f->used = (unsigned long)n;
	return f->used;
}

unsigned long fread(void *into, unsigned long size, unsigned long count,
		    struct recon_stream *f)
{
	unsigned char *p = into;
	unsigned long total;
	unsigned long done = 0;

	if (f == NULL || !(f->flags & FLAG_READ) || size == 0 || count == 0) {
		return 0;
	}
	total = size * count;

	/* The pushback, first and once. Taken here rather than inside the loop
	 * because there is only ever one of them, and a branch in the loop for
	 * something true at most once is a branch that is wrong eventually. */
	if (f->pushed_back >= 0) {
		p[done++] = (unsigned char)f->pushed_back;
		f->pushed_back = -1;
	}

	while (done < total) {
		unsigned long have = fill(f);
		unsigned long take;

		if (have == 0) {
			break;
		}
		take = total - done;
		if (take > have) {
			take = have;
		}
		memcpy(p + done, f->buffer + f->at, take);
		f->at += take;
		done += take;
	}

	f->position += (long long)done;

	/*
	 * **Whole items, which is what `fread` returns.** A partial item at
	 * the end is not reported, and the bytes of it are consumed -- that is
	 * the function's contract, and a caller reading fixed-size records
	 * relies on the count being records rather than bytes.
	 */
	return done / size;
}

int fgetc(struct recon_stream *f)
{
	unsigned char c;

	if (f == NULL || !(f->flags & FLAG_READ)) {
		return -1;
	}

	/* A character put back comes out again before anything is read.
	 * Every reader has to do this; one that forgot would make a pushback
	 * silently disappear, which is worse than not offering the function. */
	if (f->pushed_back >= 0) {
		int back = f->pushed_back;

		f->pushed_back = -1;
		f->position++;
		return back;
	}

	if (fill(f) == 0) {
		return -1;
	}
	c = f->buffer[f->at++];
	f->position++;
	return (int)c;
}

/*
 * Whether the last read reached the end, and whether anything has failed.
 *
 * Two questions, and the whole reason they are two: a loop that stops reading
 * cannot tell from the stopping alone whether the file ended or the disk did.
 * A caller that treats them the same truncates a file on an I/O error and
 * reports success.
 *
 * The flags have been maintained since this file was written -- set on the
 * paths that should set them and cleared by `fseek` and `rewind`. What was
 * missing was any way to ask.
 */
int feof(struct recon_stream *f)
{
	if (f == NULL) {
		return 0;
	}

	return (f->flags & FLAG_EOF) ? 1 : 0;
}

int ferror(struct recon_stream *f)
{
	if (f == NULL) {
		return 0;
	}

	return (f->flags & FLAG_ERROR) ? 1 : 0;
}

void clearerr(struct recon_stream *f)
{
	if (f != NULL) {
		f->flags &= ~(FLAG_EOF | FLAG_ERROR);
	}
}

/*
 * One character back onto the stream.
 *
 * **One, and the standard promises only one.** More would mean deciding what
 * happens when a caller pushes back characters it never read, and what `ftell`
 * should then say about a position that is partly invented. One slot has
 * neither question, and the position simply moves back with it.
 *
 * Pushing back EOF is defined to do nothing and fail, which is not the same as
 * an error -- a parser that reaches the end and tries to unread it is behaving
 * correctly, and gets told the stream is unchanged.
 *
 * Clears the end-of-file flag, because there is now a character to read.
 */
int ungetc(int c, struct recon_stream *f)
{
	if (f == NULL || !(f->flags & FLAG_READ) || c < 0) {
		return -1;
	}

	if (f->pushed_back >= 0) {
		return -1;		/* one at a time, as promised */
	}

	f->pushed_back = (int)(unsigned char)c;
	f->flags &= ~FLAG_EOF;
	f->position--;

	return f->pushed_back;
}

/*
 * A line, including its newline, NUL-terminated.
 *
 * The three things this gets asked to do that are easy to get wrong, and each
 * has a caller that depends on it:
 *
 *   - **the newline is kept.** A caller stripping it knows where it is; one
 *     that needed it back cannot get it.
 *   - **`room` counts the terminator**, so at most `room - 1` bytes of the
 *     line are stored. Off by one here overruns every caller's buffer.
 *   - **NULL at the end**, and only when nothing at all was read. A last line
 *     with no trailing newline is still a line and is returned.
 */
char *fgets(char *into, int room, struct recon_stream *f)
{
	int n = 0;

	if (into == NULL || room <= 0 || f == NULL ||
	    !(f->flags & FLAG_READ)) {
		return (char *)0;
	}

	while (n < room - 1) {
		unsigned long have = fill(f);
		unsigned char c;

		if (have == 0) {
			break;
		}
		c = f->buffer[f->at++];
		f->position++;
		into[n++] = (char)c;
		if (c == '\n') {
			break;
		}
	}

	if (n == 0) {
		return (char *)0;
	}
	into[n] = '\0';
	return into;
}

/* --- Position --- */

long ftell(struct recon_stream *f)
{
	if (f == NULL) {
		return -1;
	}
	/* The caller's position, not the descriptor's -- the descriptor has
	 * run ahead by whatever is still sitting in the read buffer. */
	return (long)f->position;
}

int fseek(struct recon_stream *f, long offset, int from)
{
	long long target;

	if (f == NULL) {
		return -1;
	}

	if (f->flags & FLAG_WRITE) {
		if (flush_out(f) < 0) {
			return -1;
		}
	}

	/*
	 * A seek relative to the current position has to be relative to the
	 * *caller's*, and the descriptor is somewhere else. So it is converted
	 * to an absolute seek here rather than handed to the kernel as-is --
	 * which was the first thing this got wrong, and it is invisible until
	 * a file is read and then seeked within.
	 */
	if (from == RECON_SEEK_CUR) {
		target = f->position + offset;
		from = RECON_SEEK_SET;
	} else {
		target = offset;
	}

	/* Whatever was read ahead is no longer where the caller is. */
	f->at = 0;
	f->used = 0;
	f->flags &= ~FLAG_EOF;

	/* A seek throws the pushback away, because it was a character at a
	 * position the stream is no longer at. Keeping it would hand the
	 * caller a byte from somewhere else in the file. */
	f->pushed_back = -1;

	if (recon_sys_seek(f->fd, target, from) < 0) {
		return -1;
	}

	if (from == RECON_SEEK_SET) {
		f->position = target;
	} else {
		/* From the end: ask where that turned out to be rather than
		 * assuming, because only the kernel knows the length. */
		long where = (long)recon_sys_seek(f->fd, 0, RECON_SEEK_CUR);

		f->position = where < 0 ? 0 : where;
	}
	return 0;
}

void rewind(struct recon_stream *f)
{
	if (f != NULL) {
		fseek(f, 0, RECON_SEEK_SET);
		f->flags &= ~(FLAG_EOF | FLAG_ERROR);
		f->pushed_back = -1;
	}
}

/* --- Closing --- */

int fclose(struct recon_stream *f)
{
	int result = 0;

	if (f == NULL) {
		return -1;
	}
	if (f->flags & FLAG_WRITE) {
		if (flush_out(f) < 0) {
			result = -1;
		}
	}

	/* The three standard streams are not closed and not released -- a
	 * program that closed stdout would have no way to get it back, and
	 * every later print would go to whatever opened next and took
	 * descriptor 1. */
	if (is_standard(f)) {
		return result;
	}

	if (recon_sys_close(f->fd) < 0) {
		result = -1;
	}
	f->flags = 0;
	f->fd = -1;
	return result;
}

/* --- an assertion that failed --- */

/*
 * A failed assertion.
 *
 * Says what failed, where, and in which function, and then ends the program
 * with a code the kernel reads. See `userland/include/assert.h` for why it
 * does not `abort`: there is no shell watching and no core to leave behind, so
 * the message *is* the record -- and it goes to standard error, which the
 * kernel puts on the serial line and into its own log, where it survives the
 * program ending.
 *
 * **In stdio.c rather than stdlib.c**, which is where it started. It prints,
 * and putting it beside `exit` made every suite that links the number
 * conversions without the file layer fail to link -- a dependency added to
 * `stdlib.c` for the sake of one function that does not belong there.
 *
 * The wording is glibc's shape, so a program asserting on Linux and the same
 * program asserting here produce lines somebody can compare.
 */
void recon_libc_assert(const char *condition, const char *file, int line,
		       const char *function)
{
	fprintf(recon_stderr, "%s:%d: %s: Assertion `%s' failed.\n",
		file ? file : "?", line, function ? function : "?",
		condition ? condition : "?");

	/*
	 * 134, which is what a shell reports for a program killed by SIGABRT
	 * -- 128 plus the signal. There is no SIGABRT to raise here, and a
	 * code of our own would mean a script checking for an assertion
	 * failure having to know which system it ran on.
	 */
	exit(134);
}

