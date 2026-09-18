/*
 * Naming segments, finding where the last one left off, and rendering one.
 *
 * See `logfile.h` for why the log rotates instead of appending. Everything
 * here is pure: a listing in, a number out; a ring in, bytes out. The two
 * syscalls this needs -- `SYS_LIST` and `SYS_CREATE` -- are in
 * `server_init.c`, which is the only place that can make them.
 *
 * --- What was tried and rejected ---
 *
 * **Keeping the next number in memory and starting from zero each boot.** It
 * is simpler and it overwrites the first segments of the previous run on the
 * second boot -- except `SYS_CREATE` refuses to overwrite, so instead every
 * flush after a restart fails silently and the log stops. Worse than losing
 * data: a log that has stopped looks exactly like a server with nothing to
 * report. The number comes from the directory.
 *
 * **Refusing a directory that holds something unexpected.** A stray file in
 * `/System/Logs` would then stop logging entirely. Names that are not segments
 * are stepped over.
 *
 * **Reporting the next number as an error when the directory is full.** It is
 * returned as `LOGFILE_MAX_NUMBER + 1` and refused by `logfile_name`, so the
 * bound is checked in one place. Two places that each know the limit are two
 * places that can disagree about it.
 */

#include "logfile.h"

/* --- names ----------------------------------------------------------------- */

static int is_digit(char c)
{
	return c >= '0' && c <= '9';
}

long logfile_name(unsigned long number, char *out, size_t room)
{
	static const char SUFFIX[] = ".log";
	size_t i;

	if (!out)
		return LOGFILE_EROOM;
	if (number > LOGFILE_MAX_NUMBER)
		return LOGFILE_EEXHAUSTED;
	if (room < LOGFILE_NAME_MAX)
		return LOGFILE_EROOM;

	/* Written backwards from the last digit, which is what makes the
	 * zero-padding fall out rather than needing a second pass. */
	for (i = LOGFILE_DIGITS; i > 0; i--) {
		out[i - 1] = (char)('0' + (number % 10));
		number /= 10;
	}
	for (i = 0; i < sizeof(SUFFIX) - 1; i++)
		out[LOGFILE_DIGITS + i] = SUFFIX[i];
	out[LOGFILE_DIGITS + sizeof(SUFFIX) - 1] = '\0';

	return (long)(LOGFILE_DIGITS + sizeof(SUFFIX) - 1);
}

/*
 * Is this name a segment, and which one?
 *
 * Exactly six digits and then `.log`, and nothing else at all. `7.log` is not
 * a segment and neither is `0000001.log`: both would sort wrongly against the
 * ones this writes, and a name that sorts wrongly is a log read out of order.
 */
static int segment_number(const char *name, size_t len, unsigned long *out)
{
	static const char SUFFIX[] = ".log";
	unsigned long n = 0;
	size_t i;

	if (len != LOGFILE_DIGITS + sizeof(SUFFIX) - 1)
		return 0;

	for (i = 0; i < LOGFILE_DIGITS; i++) {
		if (!is_digit(name[i]))
			return 0;
		n = n * 10 + (unsigned long)(name[i] - '0');
	}
	for (i = 0; i < sizeof(SUFFIX) - 1; i++)
		if (name[LOGFILE_DIGITS + i] != SUFFIX[i])
			return 0;

	*out = n;
	return 1;
}

unsigned long logfile_next(const char *names, size_t len)
{
	size_t at = 0;
	unsigned long highest = 0;
	int any = 0;

	if (!names)
		return 0;

	while (at < len) {
		size_t start = at;
		unsigned long n;

		while (at < len && names[at] != '\0')
			at++;

		if (segment_number(names + start, at - start, &n)) {
			if (!any || n > highest)
				highest = n;
			any = 1;
		}

		/* Step over the terminator. A listing that ends without one
		 * leaves `at == len` and the loop ends, which is the right
		 * answer for a truncated buffer -- the last name is not
		 * trusted because it may be a prefix of a longer one. */
		at++;
	}

	if (!any)
		return 0;

	/* Deliberately allowed to become MAX + 1, which `logfile_name`
	 * refuses. See the header: one place knows the bound. */
	return highest + 1;
}

/* --- rendering -------------------------------------------------------------- */

static size_t put(char *out, size_t room, size_t at, const char *text)
{
	size_t i;

	for (i = 0; text[i]; i++) {
		if (at + 1 >= room)
			return room;	/* caller sees at >= room and refuses */
		out[at++] = text[i];
	}
	return at;
}

static size_t put_ulong(char *out, size_t room, size_t at, unsigned long v)
{
	char digits[24];
	size_t n = 0;

	if (v == 0)
		digits[n++] = '0';
	while (v > 0) {
		digits[n++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (n > 0) {
		if (at + 1 >= room)
			return room;
		out[at++] = digits[--n];
	}
	return at;
}

long logfile_render(const struct logbook *book, unsigned long first,
                    char *out, size_t room, unsigned long *missed)
{
	unsigned long i;
	size_t at = 0;
	unsigned long skipped = 0;

	if (missed)
		*missed = 0;
	if (!book || !out || room == 0)
		return LOGFILE_EROOM;
	if (first >= book->written)
		return LOGFILE_ENOTHING;

	/*
	 * The oldest entry the ring still holds.
	 *
	 * `written` counts forever and the ring keeps the last
	 * `LOG_ENTRIES_MAX`, so anything below this is gone. A caller that
	 * fell behind asks for it anyway, and saying how many were lost is the
	 * whole point -- `log.h` reports its dropped count for the same
	 * reason, one layer down.
	 */
	{
		unsigned long oldest = 0;

		if (book->written > LOG_ENTRIES_MAX)
			oldest = book->written - LOG_ENTRIES_MAX;
		if (first < oldest) {
			skipped = oldest - first;
			first = oldest;
		}
	}

	for (i = first; i < book->written; i++) {
		const struct log_entry *e =
			&book->entries[i % LOG_ENTRIES_MAX];

		at = put_ulong(out, room, at, e->at);
		at = put(out, room, at, "  ");
		at = put(out, room, at, e->line);
		at = put(out, room, at, "\n");
		if (at >= room)
			return LOGFILE_EROOM;
	}

	if (skipped) {
		/* Recorded in the segment itself rather than only returned,
		 * because whoever reads these files later is not the caller
		 * and has no other way to learn of it. */
		at = put(out, room, at, "(");
		at = put_ulong(out, room, at, skipped);
		at = put(out, room, at,
		         " entries were dropped before this segment)\n");
		if (at >= room)
			return LOGFILE_EROOM;
	}

	out[at] = '\0';
	if (missed)
		*missed = skipped;
	return (long)at;
}
