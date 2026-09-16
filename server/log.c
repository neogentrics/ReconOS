/*
 * A ring of recent entries.
 *
 * See `log.h` for why it is in memory and why the dropped count is not
 * optional.
 *
 * --- What was tried and rejected ---
 *
 * **A separate `dropped` counter, incremented when an entry is overwritten.**
 * It is the obvious shape and it can disagree with the ring: two counters for
 * one fact, and a bug in either produces a log that describes a history that
 * did not happen. `written` is the only counter, and everything else is
 * arithmetic on it -- held, dropped and the index of any entry all come from
 * one number, so they cannot contradict each other.
 *
 * **Wrapping `written` at the ring size.** It makes the index cheaper and
 * throws away the total, which is the thing the dropped count is computed
 * from. The counter runs forever; at a million requests a second it would take
 * half a million years to overflow an `unsigned long`, which is not a hazard
 * worth a branch.
 */

#include "log.h"

void log_write(struct logbook *book, unsigned long at, const char *line)
{
	struct log_entry *e;
	size_t i = 0;

	if (!book || !line)
		return;

	e = &book->entries[book->written % LOG_ENTRIES_MAX];

	/* Truncated rather than refused, which is the opposite of what the rest
	 * of this role does -- and is right here. A log line is prose for a
	 * person, and a short line still says most of what it meant. Refusing
	 * would mean the busiest moments record nothing, which is when a log
	 * matters most. */
	while (line[i] && i < LOG_LINE_MAX - 1) {
		e->line[i] = line[i];
		i++;
	}
	e->line[i] = '\0';
	e->at = at;

	book->written++;
}

size_t log_held(const struct logbook *book)
{
	if (!book)
		return 0;
	if (book->written < LOG_ENTRIES_MAX)
		return (size_t)book->written;
	return LOG_ENTRIES_MAX;
}

unsigned long log_dropped(const struct logbook *book)
{
	if (!book || book->written <= LOG_ENTRIES_MAX)
		return 0;
	return book->written - LOG_ENTRIES_MAX;
}

const struct log_entry *log_at(const struct logbook *book, size_t n)
{
	size_t held;

	if (!book)
		return 0;

	held = log_held(book);
	if (n >= held)
		return 0;

	/* The oldest held entry sits at `written - held`; the nth is that many
	 * further on. Both are absolute positions in a counter that never
	 * wraps, so the modulo happens once, here, and nowhere else. */
	return &book->entries[(book->written - held + n) % LOG_ENTRIES_MAX];
}
