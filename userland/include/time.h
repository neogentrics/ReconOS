/*
 * <time.h>, cut to what the desktop calls.
 *
 * Measured rather than chosen: `clock_gettime` 10, `time` 8, `localtime_r` 4,
 * `strftime` 3, `gmtime_r` 1 -- and `time_t` itself 22 times, most of them in
 * `recon_cookie.h` and `recon_fs.h`, which include this header **for the type
 * alone**. Two desktop sources that could not be built without glibc turned
 * out to need nothing from it but a typedef.
 *
 * **The two clocks are two calls and cannot reach each other.**
 * `clock_gettime(CLOCK_MONOTONIC, ...)` never goes backwards and means nothing
 * outside this boot; `time()` is the date and can jump. A duration measured
 * across a clock correction comes out negative, and a negative duration is a
 * number that gets used -- which is why the kernel offers two system calls and
 * why this header does not offer a way to blur them.
 *
 * **`localtime_r` is `gmtime_r`.** On this kernel there is no host to ask and
 * no zone database to read, so UTC is the only answer that is true. The reason
 * is written out in `libc/time.c`, and the short version is that a zone
 * database is a file that goes stale in the direction that matters. It costs
 * the desktop nothing: `src/recon_clock.c` already applies ReconOS's own zone
 * to its own civil-date arithmetic, deliberately, and never comes through
 * here.
 *
 * **What is absent is absent on purpose.** No `mktime` or `timegm` -- the one
 * caller that needed a date turned into seconds wrote its own and recorded why
 * (`src/recon_cookie.c`). No `asctime`, `ctime`, `strptime`, or `nanosleep`.
 * No `struct timeval`: it appears twice in the desktop and both times it is a
 * socket timeout, which belongs to a header that does not exist here yet.
 */

#ifndef RECON_TIME_H
#define RECON_TIME_H

#include <stddef.h>

/*
 * Signed, because a date before 1970 is a negative number and the cookie jar
 * already relies on that. 64 bits on every architecture this will run on,
 * which is the whole of what ReconOS has to say about the year 2038.
 */
typedef long long time_t;

/*
 * The fields, in this order, are what every caller writes and what the C
 * standard names. The order also matters more than it looks: the differential
 * suite passes the *host's* `struct tm` to this library's `gmtime_r` and reads
 * the nine fields back, so a field out of order does not read as a style
 * difference -- it reads as a wrong date, loudly, on the first run.
 *
 * glibc's has two more fields after `tm_isdst` (`tm_gmtoff` and `tm_zone`,
 * both GNU extensions). Nothing here writes past `tm_isdst`, which is what
 * makes that test safe as well as meaningful.
 */
struct tm {
	int tm_sec;	/* 0-60, and 60 is a leap second */
	int tm_min;	/* 0-59 */
	int tm_hour;	/* 0-23 */
	int tm_mday;	/* 1-31 */
	int tm_mon;	/* 0-11, January is 0 */
	int tm_year;	/* years since 1900 */
	int tm_wday;	/* 0-6, Sunday is 0 */
	int tm_yday;	/* 0-365 */
	int tm_isdst;	/* always 0 here; there is no summer time without a
			 * zone, and a negative "unknown" would be a claim
			 * this cannot support either */
};

struct timespec {
	time_t tv_sec;
	long tv_nsec;
};

/*
 * Two clocks, two numbers. `CLOCK_REALTIME` is the same question `time()`
 * asks, at a finer grain.
 *
 * The values are not the host's and do not need to be: nothing crosses this
 * boundary but a call.
 */
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

int clock_gettime(int which, struct timespec *into);

time_t time(time_t *out);
double difftime(time_t later, time_t earlier);

struct tm *gmtime_r(const time_t *when, struct tm *into);
struct tm *localtime_r(const time_t *when, struct tm *into);

/*
 * Returns the length written, not counting the terminator, or **zero if it
 * did not fit** -- in which case the contents are unspecified, which is what
 * the standard says and is worth obeying rather than improving on. A caller
 * who ignores the 0 and prints the buffer anyway has a bug, and leaving a
 * plausible-looking truncation there is how that bug stays hidden.
 *
 * The conversions it has: Y y C m d e H I M S j p a A b h B F D T R z Z n t %.
 * Anything else is written back exactly as it arrived, which is what the
 * reference does and what `snprintf` here does with an unknown conversion.
 *
 * Month and day names are English and only English -- the same decision
 * `ctype.h` makes about letters. A name is a locale's, and a locale is a
 * database.
 */
size_t strftime(char *into, size_t room, const char *format,
		const struct tm *parts);

#endif /* RECON_TIME_H */
