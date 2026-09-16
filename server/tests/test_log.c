/*
 * A ring of recent entries, and the count that stops it lying.
 *
 * Pure: a buffer and a clock the test supplies.
 *
 * **The case this exists for is the overrun.** A ring drops the oldest entry to
 * make room, and a log that drops silently looks complete, reads plausibly, and
 * has lost exactly the entries somebody is hunting for -- because trouble
 * arrives in bursts, and a burst is what overruns a ring. So the checks below
 * are mostly about `log_dropped` being right at every boundary, not about
 * entries being stored.
 *
 * **Watched failing first**, against a version keeping its own `dropped`
 * counter incremented on overwrite: it was right in the steady state and wrong
 * at the boundary, reporting one drop when the ring had just filled exactly and
 * lost nothing. Two counters for one fact.
 */

#include "../log.h"

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

static struct logbook BOOK;

static void fill(unsigned long n)
{
	unsigned long i;

	for (i = 0; i < n; i++) {
		char line[32];

		snprintf(line, sizeof(line), "entry-%lu", i);
		log_write(&BOOK, i, line);
	}
}

int main(void)
{
	printf("a ring of recent entries, and the count that stops it lying\n");

	/* --- empty --------------------------------------------------------- */
	memset(&BOOK, 0, sizeof(BOOK));
	ok(log_held(&BOOK) == 0, "an empty book holds nothing");
	ok(log_dropped(&BOOK) == 0, "and has dropped nothing");
	ok(log_at(&BOOK, 0) == 0, "and has no first entry");

	/* --- below capacity ------------------------------------------------- */
	memset(&BOOK, 0, sizeof(BOOK));
	fill(3);
	ok(log_held(&BOOK) == 3, "three written, three held");
	ok(log_dropped(&BOOK) == 0, "nothing dropped");
	ok(log_at(&BOOK, 0) && strcmp(log_at(&BOOK, 0)->line, "entry-0") == 0,
	   "oldest first");
	ok(log_at(&BOOK, 2) && strcmp(log_at(&BOOK, 2)->line, "entry-2") == 0,
	   "and newest last");
	ok(log_at(&BOOK, 3) == 0, "nothing past the end");
	ok(log_at(&BOOK, 1)->at == 1, "the clock the caller gave is kept");

	/* --- exactly full, which is the boundary a second counter gets wrong -- */
	memset(&BOOK, 0, sizeof(BOOK));
	fill(LOG_ENTRIES_MAX);
	ok(log_held(&BOOK) == LOG_ENTRIES_MAX, "a full book holds its capacity");
	ok(log_dropped(&BOOK) == 0,
	   "and has dropped nothing -- full is not the same as overrun");
	ok(strcmp(log_at(&BOOK, 0)->line, "entry-0") == 0,
	   "the first entry is still there");

	/* --- one past full --------------------------------------------------- */
	memset(&BOOK, 0, sizeof(BOOK));
	fill(LOG_ENTRIES_MAX + 1);
	ok(log_held(&BOOK) == LOG_ENTRIES_MAX, "still holds its capacity");
	ok(log_dropped(&BOOK) == 1, "and reports the one it lost");
	ok(strcmp(log_at(&BOOK, 0)->line, "entry-1") == 0,
	   "the oldest held is the second ever written");

	/* --- well past full, which is the ordinary state of a busy machine ---- */
	memset(&BOOK, 0, sizeof(BOOK));
	fill(LOG_ENTRIES_MAX * 3 + 7);
	ok(log_held(&BOOK) == LOG_ENTRIES_MAX, "capacity again");
	ok(log_dropped(&BOOK) == (unsigned long)(LOG_ENTRIES_MAX * 2 + 7),
	   "and the dropped count is the whole difference, not a recent one");
	{
		char want[32];

		snprintf(want, sizeof(want), "entry-%lu",
		         (unsigned long)(LOG_ENTRIES_MAX * 2 + 7));
		ok(strcmp(log_at(&BOOK, 0)->line, want) == 0,
		   "the oldest held is the right one after many wraps");

		snprintf(want, sizeof(want), "entry-%lu",
		         (unsigned long)(LOG_ENTRIES_MAX * 3 + 6));
		ok(strcmp(log_at(&BOOK, LOG_ENTRIES_MAX - 1)->line, want) == 0,
		   "and the newest held is the last written");
	}

	/* Held plus dropped is everything ever written, at every point. That is
	 * the invariant a separate counter can break and arithmetic on one
	 * counter cannot. */
	{
		unsigned long n;
		int holds = 1;

		for (n = 0; n <= LOG_ENTRIES_MAX * 2; n++) {
			memset(&BOOK, 0, sizeof(BOOK));
			fill(n);
			if ((unsigned long)log_held(&BOOK) + log_dropped(&BOOK)
			    != n)
				holds = 0;
		}
		ok(holds,
		   "held plus dropped equals everything written, at every size");
	}

	/* --- a line longer than a line --------------------------------------- */
	{
		char big[LOG_LINE_MAX + 64];
		size_t i;

		for (i = 0; i < sizeof(big) - 1; i++)
			big[i] = 'x';
		big[sizeof(big) - 1] = '\0';

		memset(&BOOK, 0, sizeof(BOOK));
		log_write(&BOOK, 0, big);

		ok(log_held(&BOOK) == 1, "an over-long line is still recorded");
		ok(strlen(log_at(&BOOK, 0)->line) == LOG_LINE_MAX - 1,
		   "cut to fit rather than refused -- the busiest moments must"
		   " still record something");
	}

	/* --- nothing at all --------------------------------------------------- */
	log_write(0, 0, "x");		/* must not crash */
	log_write(&BOOK, 0, 0);
	ok(log_held(&BOOK) == 1, "a null line is not recorded");
	ok(log_held(0) == 0 && log_dropped(0) == 0 && log_at(0, 0) == 0,
	   "a null book answers empty rather than crashing");

	printf("  %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
