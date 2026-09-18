/*
 * Putting the access log somewhere it survives a reboot.
 *
 * --- Why this is segments and not a file that grows ---
 *
 * **Because appending is not available, and the reason is not a missing
 * flag.** `docs/SERVER.md` recorded for three versions that the log was in
 * memory because the C library drops `O_APPEND`. It does drop it -- and that
 * was never the whole story, because appending needs the *ability to append*
 * and `SYS_SEEK` exists, so seek-to-end then write is the other route.
 *
 * Measured on the machine, against a file that exists, trying every open flag
 * the kernel has rather than the two the library publishes:
 *
 *     append probe: create=1 W=-12 W|CREATE=5 W|REPLACE=-12
 *
 * Plain write is refused. `OPEN_CREATE`, documented *it must not already
 * exist*, **succeeded** on a file that does. `OPEN_REPLACE`, documented *it
 * must exist*, was **refused** on one. Both are the opposite of their own
 * comments, so the only door that opens is the one contradicting its
 * documentation -- and a log built on that is a log built on a fault.
 *
 * What *is* documented and does work is `SYS_CREATE`: path and contents
 * together, written whole, and **refusing to overwrite**. ReconFS writes whole
 * files in one transaction, which is why that call has the shape it does.
 *
 * So this stops fighting the filesystem and takes its shape. A log that
 * appends is impossible here; a log that **rotates** is natural. Each segment
 * is created once, whole and atomically, and never touched again -- which is
 * how log-structured systems are built on purpose elsewhere, arrived at here
 * because nothing else was on offer.
 *
 * --- What that costs, said plainly ---
 *
 * An entry is durable when its segment is written, not when it is recorded.
 * Between flushes the newest entries live only in the ring, and a machine that
 * loses power loses them. `LOGFILE_FLUSH_EVERY` is how much is at risk.
 *
 * That is a real weakness and it is the one an append-only log would not have.
 * It is written here rather than discovered.
 */

#ifndef RECON_SERVER_LOGFILE_H
#define RECON_SERVER_LOGFILE_H

#include <stddef.h>

#include "log.h"

/* Where segments live. Under `/System` because it is the system's rather than
 * a user's, and **not** under the web root: nothing serves this directory, for
 * the same reason nothing serves `/System/Uploads`. */
#define LOGFILE_DIR "/System/Logs"

/*
 * Six digits, zero-padded, so a lexical sort is a chronological one.
 *
 * `10.log` sorting before `9.log` is how a log gets read out of order by
 * everything that lists a directory, and nothing about that failure announces
 * itself. Six is enough for a million segments at thirty-two entries each,
 * which is thirty-two million requests -- past that this refuses rather than
 * wrapping, because a wrapped number makes an old segment look new.
 */
#define LOGFILE_DIGITS 6
#define LOGFILE_MAX_NUMBER 999999UL
#define LOGFILE_NAME_MAX (LOGFILE_DIGITS + 5)	/* "NNNNNN.log" and a NUL */

/*
 * How many entries accumulate before a segment is written.
 *
 * Half the ring. Small enough that the flusher cannot fall behind and let the
 * ring drop something it has not written yet; large enough that a busy server
 * is not creating a file per request. If it is ever raised past
 * `LOG_ENTRIES_MAX`, entries will be lost between flushes -- which is why this
 * is expressed against the ring's size rather than as a number of its own.
 */
#define LOGFILE_FLUSH_EVERY (LOG_ENTRIES_MAX / 2)

#define LOGFILE_OK          0
#define LOGFILE_EROOM     (-1)	/* the caller's buffer is too small */
#define LOGFILE_EEXHAUSTED (-2)	/* past LOGFILE_MAX_NUMBER */
#define LOGFILE_ENOTHING  (-3)	/* nothing to write */

/*
 * The name a segment number gets: `000042.log`.
 *
 * Returns the length written, or a negative verdict. Refuses a number past
 * `LOGFILE_MAX_NUMBER` rather than producing a seven-digit name that sorts
 * before every six-digit one.
 */
long logfile_name(unsigned long number, char *out, size_t room);

/*
 * The next segment number, given a directory listing.
 *
 * `names` is what `SYS_LIST` produces: names NUL-terminated and back to back,
 * `len` bytes in total. **Anything that is not a segment name is ignored**
 * rather than refused -- a directory may hold whatever somebody put there, and
 * a log that will not start because of a stray file is worse than one that
 * steps over it.
 *
 * Returns the highest segment number found plus one, or 0 for a directory with
 * none. A number that would exceed `LOGFILE_MAX_NUMBER` comes back as
 * `LOGFILE_MAX_NUMBER + 1`, which `logfile_name` then refuses -- the check
 * lives in one place rather than two.
 */
unsigned long logfile_next(const char *names, size_t len);

/*
 * Is this name one of ours, and which number?
 *
 * Exported because two places need the answer and they must not each decide
 * it: `logfile_next` uses it to continue the numbering, and the endpoint that
 * lists segments uses it to report only the log's own files rather than
 * everything in the directory. Two copies of this rule would eventually list a
 * name that `logfile_next` ignores, which is a segment nobody can read.
 *
 * Exactly six digits and `.log`, and nothing else at all: `7.log` is not one
 * and neither is `0000001.log`. Both would sort wrongly against the names this
 * writes, and a name that sorts wrongly is a log read out of order.
 *
 * Returns 1 and sets `*number` when it is, 0 when it is not.
 */
int logfile_is_segment(const char *name, size_t len, unsigned long *number);

/*
 * Render the entries from `first` up to (not including) `book->written`.
 *
 * `first` is a count-since-boot, not a ring index: the ring holds the last
 * `LOG_ENTRIES_MAX` and `written` counts forever, so a caller that fell behind
 * asks for entries the ring no longer has. Those are **skipped and counted**
 * rather than faked -- `*missed` says how many, because a segment that silently
 * omits entries is the fault `log.h` exists to avoid, one layer up.
 *
 * Returns the bytes written, or a negative verdict.
 */
long logfile_render(const struct logbook *book, unsigned long first,
                    char *out, size_t room, unsigned long *missed);

#endif
