/*
 * What happened, kept where something can read it back.
 *
 * The architecture document asks for an audit daemon and an Event Viewer. This
 * is the part of that which can exist today: a fixed ring of recent entries,
 * written by the server as it answers requests and readable over the API.
 *
 * --- Why it is in memory, and why that is a limitation rather than a design ---
 *
 * A log belongs in a file. This one cannot go in one: appending needs
 * `O_APPEND`, and `recon_flags_from_posix` in the C library drops it along with
 * `O_CREAT`, `O_TRUNC` and `O_EXCL` -- the fault this role reported and is
 * still waiting on. `SYS_CREATE` writes a whole file at once, which would mean
 * rewriting the entire log on every request.
 *
 * So it is a ring in memory, and **it does not survive a reboot**. That is
 * written here rather than discovered by somebody looking for yesterday's
 * requests. When the C library honours `O_APPEND`, this grows a second home
 * rather than being replaced.
 *
 * --- The one property that makes a bounded log honest ---
 *
 * A ring drops the oldest entry to make room. A log that drops silently is a
 * log that **lies about what happened**: it looks complete, it reads
 * plausibly, and the entries somebody is hunting for are exactly the ones that
 * went -- because trouble produces bursts, and a burst is what overruns a ring.
 *
 * So the count of dropped entries is kept and reported alongside them. A reader
 * who sees `dropped: 0` has the whole story; one who sees `dropped: 412` knows
 * to stop drawing conclusions from what is left. The number is the difference
 * between a short log and a misleading one.
 */

#ifndef RECON_LOG_H
#define RECON_LOG_H

#include <stddef.h>

/* How many entries are kept. Small on purpose: this is a window on what just
 * happened, not an archive, and an archive is what the file the C library
 * cannot append to would be for. */
#define LOG_ENTRIES_MAX  64
#define LOG_LINE_MAX    160

struct log_entry {
	char          line[LOG_LINE_MAX];
	unsigned long at;		/* the clock the caller supplied */
};

struct logbook {
	struct log_entry entries[LOG_ENTRIES_MAX];

	/* Where the next entry goes, counting forever rather than wrapping.
	 * `written` is also the total ever recorded, which is what makes the
	 * dropped count a subtraction rather than a separate counter that
	 * could disagree with it. */
	unsigned long written;
};

/* Record one line. Longer than `LOG_LINE_MAX` is truncated -- the one place in
 * this role where truncation is right, because a log line is prose for a
 * person and a short line still says most of what it meant. Everything else
 * that truncates here would be changing a value. */
void log_write(struct logbook *book, unsigned long at, const char *line);

/* How many entries are held, which is `written` until the ring fills. */
size_t log_held(const struct logbook *book);

/* How many were dropped to make room: zero until the ring fills, and the
 * difference thereafter. See the header above for why this is not optional. */
unsigned long log_dropped(const struct logbook *book);

/*
 * The `n`th held entry, oldest first, or NULL.
 *
 * Oldest first because a log is read forwards -- what happened, then what
 * happened next. A reader wanting the most recent can walk backwards from
 * `log_held`.
 */
const struct log_entry *log_at(const struct logbook *book, size_t n);

#endif
