/*
 * Numbering segments, and the sort order that makes a log readable.
 *
 * Pure: a directory listing in, a number out; a ring in, bytes out.
 *
 * **The case this exists for** is a name that sorts wrongly. The log is a
 * directory of files and the only thing putting them in order is their names,
 * so `10.log` before `9.log` is a log read out of sequence by everything that
 * lists a directory -- and nothing about that failure announces itself. The
 * entries are all present, all correct, and in the wrong order, which is the
 * kind of wrong that survives a careful look.
 *
 * **The second case** is a restart. The next number comes from the directory
 * rather than from memory, because `SYS_CREATE` refuses to overwrite: a server
 * that started again from zero would not clobber the previous run's segments,
 * it would fail to write anything at all, for ever, silently. A log that has
 * stopped looks exactly like a server with nothing to report.
 *
 * **Watched failing first**, against a version that formats names with no
 * padding and starts each boot from zero -- which is what this would have been
 * written as without the two paragraphs above. **10 of 34 checks failed**, and
 * both cases named there are among them.
 *
 * Neither fault is one the code would report. Unpadded names produce a
 * directory of files that are all present and all correct, in an order no
 * reader can fix; starting from zero produces a server whose log stops on its
 * second boot and says nothing about it.
 */

#include "../logfile.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

static void ok(int cond, const char *what)
{
	checks++;
	if (!cond) {
		failures++;
		printf("  FAIL  %s\n", what);
	}
}

/* A listing as `SYS_LIST` produces one: names NUL-terminated, back to back. */
static size_t listing(char *out, size_t room, const char **names, size_t count)
{
	size_t at = 0, i, j;

	for (i = 0; i < count; i++) {
		for (j = 0; names[i][j] && at + 1 < room; j++)
			out[at++] = names[i][j];
		if (at + 1 < room)
			out[at++] = '\0';
	}
	return at;
}

int main(void)
{
	char buf[512];
	char name[LOGFILE_NAME_MAX];
	size_t len;

	printf("numbering segments, and the sort order that makes a log readable\n");

	/* --- names ------------------------------------------------------------- */
	ok(logfile_name(0, name, sizeof(name)) == 10
	   && strcmp(name, "000000.log") == 0, "the first segment");
	ok(logfile_name(42, name, sizeof(name)) == 10
	   && strcmp(name, "000042.log") == 0, "forty-two, zero-padded");
	ok(logfile_name(999999, name, sizeof(name)) == 10
	   && strcmp(name, "999999.log") == 0, "the last one that fits");

	/*
	 * The case the file exists for, as an ordering rather than a format.
	 * Nine and ten must come back in that order from a lexical sort, which
	 * is the only sort a directory listing gives.
	 */
	{
		char nine[LOGFILE_NAME_MAX];
		char ten[LOGFILE_NAME_MAX];

		logfile_name(9, nine, sizeof(nine));
		logfile_name(10, ten, sizeof(ten));
		ok(strcmp(nine, ten) < 0,
		   "nine sorts before ten, which unpadded names do not");
		ok(strlen(nine) == strlen(ten),
		   "because every name is the same length");
	}

	ok(logfile_name(1000000, name, sizeof(name)) == LOGFILE_EEXHAUSTED,
	   "past a million is refused, not widened to seven digits");
	ok(logfile_name(0, name, 9) == LOGFILE_EROOM, "a buffer one short");
	ok(logfile_name(0, 0, 32) == LOGFILE_EROOM, "no buffer");

	/* --- the next number, from a listing ------------------------------------ */
	{
		const char *empty[] = { "" };

		ok(logfile_next(0, 0) == 0, "no listing at all starts at zero");
		len = listing(buf, sizeof(buf), empty, 0);
		ok(logfile_next(buf, len) == 0, "an empty directory starts at zero");
	}

	{
		const char *one[] = { "000000.log" };

		len = listing(buf, sizeof(buf), one, 1);
		ok(logfile_next(buf, len) == 1, "one segment gives the next");
	}

	{
		/* Deliberately not in order, because a directory listing is
		 * not required to be and this must not depend on it. */
		const char *many[] = { "000003.log", "000001.log",
		                       "000002.log", "000000.log" };

		len = listing(buf, sizeof(buf), many, 4);
		ok(logfile_next(buf, len) == 4,
		   "four segments in any order give the highest plus one");
	}

	{
		/* The restart. A second run must continue rather than begin
		 * again -- `SYS_CREATE` refuses to overwrite, so starting over
		 * means writing nothing at all from then on. */
		const char *previous[] = { "000000.log", "000001.log",
		                           "000002.log" };

		len = listing(buf, sizeof(buf), previous, 3);
		ok(logfile_next(buf, len) == 3,
		   "a restart carries on from where the last run stopped");
	}

	{
		/* Anything that is not a segment is stepped over. A stray file
		 * must not stop the log. */
		const char *mixed[] = { "notes.txt", "000005.log", "README",
		                        ".hidden", "000004.log" };

		len = listing(buf, sizeof(buf), mixed, 5);
		ok(logfile_next(buf, len) == 6,
		   "names that are not segments are ignored, not refused");
	}

	{
		/* Near misses, every one of which would sort wrongly against a
		 * padded name if it were accepted. */
		const char *near[] = { "9.log", "0000001.log", "00000a.log",
		                       "000001.txt", "000001.log.bak",
		                       "000001", ".log" };

		len = listing(buf, sizeof(buf), near, 7);
		ok(logfile_next(buf, len) == 0,
		   "not one near miss is read as a segment");
	}

	{
		const char *high[] = { "999999.log" };

		len = listing(buf, sizeof(buf), high, 1);
		ok(logfile_next(buf, len) == 1000000,
		   "a full directory answers past the bound");
		ok(logfile_name(logfile_next(buf, len), name, sizeof(name))
		   == LOGFILE_EEXHAUSTED,
		   "and the name is what refuses it -- one place knows the limit");
	}

	/* --- rendering ---------------------------------------------------------- */
	{
		struct logbook book;
		unsigned long missed = 99;
		long n;

		memset(&book, 0, sizeof(book));
		log_write(&book, 100, "GET / -> 200");
		log_write(&book, 200, "GET /health -> 200");

		n = logfile_render(&book, 0, buf, sizeof(buf), &missed);
		ok(n > 0, "two entries render");
		ok(n > 0 && strstr(buf, "100  GET / -> 200\n") != 0,
		   "the clock, two spaces, the line");
		ok(n > 0 && strstr(buf, "200  GET /health -> 200\n") != 0,
		   "and the second");
		ok(missed == 0, "with nothing missed");

		/* Only what is new since the last segment. */
		n = logfile_render(&book, 1, buf, sizeof(buf), &missed);
		ok(n > 0 && strstr(buf, "GET / -> 200") == 0,
		   "asking from the second leaves the first out");
		ok(n > 0 && strstr(buf, "GET /health") != 0, "and keeps the second");

		ok(logfile_render(&book, 2, buf, sizeof(buf), &missed)
		   == LOGFILE_ENOTHING, "nothing new is not an error");
		ok(logfile_render(&book, 99, buf, sizeof(buf), &missed)
		   == LOGFILE_ENOTHING, "nor is a caller ahead of the ring");
	}

	{
		/*
		 * A flusher that fell behind.
		 *
		 * The ring holds 64 and 100 have been written, so the first 36
		 * are gone. They are counted rather than quietly skipped --
		 * and the count goes **into the segment**, because whoever
		 * reads these files later is not the caller and has no other
		 * way to find out.
		 */
		struct logbook book;
		unsigned long missed = 0;
		char big[8192];
		long n;
		int i;

		memset(&book, 0, sizeof(book));
		for (i = 0; i < 100; i++) {
			char line[64];

			snprintf(line, sizeof(line), "request %d", i);
			log_write(&book, (unsigned long)i, line);
		}

		n = logfile_render(&book, 0, big, sizeof(big), &missed);
		ok(n > 0, "a segment renders after the ring has wrapped");
		ok(missed == 36, "and says how many it could not reach");
		ok(n > 0 && strstr(big, "36 entries were dropped") != 0,
		   "in the segment itself, for whoever reads it later");
		ok(n > 0 && strstr(big, "request 36") != 0,
		   "the oldest entry still held is there");
		ok(n > 0 && strstr(big, "request 99") != 0, "and the newest");
		ok(n > 0 && strstr(big, "request 35") == 0,
		   "and nothing is invented for the ones that are gone");
	}

	{
		struct logbook book;
		char tiny[16];

		memset(&book, 0, sizeof(book));
		log_write(&book, 1, "a line that will not fit in sixteen bytes");
		ok(logfile_render(&book, 0, tiny, sizeof(tiny), 0)
		   == LOGFILE_EROOM,
		   "a segment that does not fit is refused, not truncated");
		ok(logfile_render(0, 0, tiny, sizeof(tiny), 0) == LOGFILE_EROOM,
		   "no book");
		ok(logfile_render(&book, 0, 0, 16, 0) == LOGFILE_EROOM,
		   "no buffer");
	}

	printf("  %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
