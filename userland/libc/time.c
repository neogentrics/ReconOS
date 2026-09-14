/*
 * Time: the two clocks, the calendar, and the one thing this system cannot
 * honestly answer.
 *
 * Measured before it was written, the same as everything else here. What the
 * desktop actually calls, across `src/`, `include/` and `modules/`:
 *
 *     clock_gettime(CLOCK_MONOTONIC)  10   durations
 *     time(NULL)                       8   stamping things
 *     localtime_r                      4
 *     strftime                         3   three format strings, eight
 *                                          conversions between them
 *     gmtime_r                         1
 *
 * And `time_t` itself 22 times, most of them in `recon_cookie.h` and
 * `recon_fs.h`, which include <time.h> **for the type alone**. Two desktop
 * sources that could not be compiled without glibc could be compiled with
 * nothing but that typedef available, which is the smallest reason this file
 * exists and the one that paid first.
 *
 * --- The two clocks are two calls, and that is not an accident ---
 *
 * `SYS_TIME` is monotonic: it never goes backwards and means nothing outside
 * this boot. `SYS_WALLTIME` is the date and can jump. The kernel offers both
 * rather than one because a duration measured across a clock correction comes
 * out negative, and a negative duration is a number that gets used.
 *
 * So `clock_gettime(CLOCK_MONOTONIC, ...)` reaches the first and
 * `time(NULL)` reaches the second, and neither can reach the other.
 *
 * --- What this cannot answer: local time ---
 *
 * `localtime_r` here **is** `gmtime_r`. That is not a stub and not a
 * placeholder: on this kernel there is no host to ask and no zone database to
 * read, so UTC is the only answer that is true. Returning the machine's local
 * time would require knowing what a zone name means in a given year, and that
 * is a *file* -- one that goes stale in the direction that matters, the same
 * argument `recon_cookie.h` makes about the Public Suffix List.
 *
 * **This costs the desktop nothing, which is worth checking rather than
 * assuming.** `src/recon_clock.c` already does its own civil-date arithmetic
 * and applies ReconOS's own zone setting, deliberately, under a comment
 * saying why: *"localtime reads the host's idea of a zone from the
 * environment and a file, and ReconOS's zone is its own."* The one place the
 * desktop calls `localtime_r` for a zone rather than for a date is
 * `recon_clock_host_zone`, whose entire job is to ask the host what zone it is
 * in -- a question with no meaning on a machine that is not hosted. That
 * function already returns false when the host cannot say, and its own comment
 * already states the consequence: *"the account simply starts at UTC and
 * somebody sets it."*
 *
 * --- What is absent, and why ---
 *
 * No `mktime` and no `timegm`. Zero call sites: the one place in the desktop
 * that needed a date turned into a count of seconds -- cookie expiry -- wrote
 * its own `days_from_civil` rather than call either, and recorded the reason
 * in `src/recon_cookie.c`: `timegm` is in no standard and `mktime` reads the
 * machine's time zone, which for a cookie expiry is wrong.
 *
 * No `asctime`, `ctime` or `strptime`. No `nanosleep`: there is nothing to
 * wake a sleeping program on this kernel yet, and a busy loop pretending to be
 * a sleep would be a worse answer than no answer.
 *
 * No `struct timeval`. It appears twice in the desktop, both times as a socket
 * timeout handed to `setsockopt`, which is a sockets question and belongs to a
 * header that does not exist here yet.
 */

#include "internal.h"

/*
 * The one place in this library that includes a public header, and the reason
 * is at the foot of `internal.h`: it means there is one definition of
 * `struct tm` and one of `time_t` rather than two kept identical by hand.
 * `prefix.h` arrives by `-include`, so it has already renamed everything this
 * declares by the time the compiler reaches it.
 */
#include "../include/time.h"

#define NS_PER_SECOND 1000000000LL

/*
 * Floored division: the second a nanosecond count belongs to is the one it is
 * *inside*, and C's `/` truncates toward zero instead. For a negative count
 * that is off by one for every value that is not an exact second -- a fault
 * invisible for fifty-odd years and then visible on one file's timestamp.
 */
static long long seconds_from_ns(long long ns)
{
	long long seconds = ns / NS_PER_SECOND;

	if (ns < 0 && ns % NS_PER_SECOND != 0) {
		seconds -= 1;
	}
	return seconds;
}

int clock_gettime(int which, struct timespec *into)
{
	long long ns;
	long long seconds;

	if (into == NULL) {
		return -1;
	}

	/*
	 * The two are separate system calls and this is the only place they
	 * meet. `CLOCK_MONOTONIC` cannot be answered from the wall clock and
	 * `CLOCK_REALTIME` cannot be answered from the monotonic one, so an
	 * unknown value is refused rather than resolved to whichever is
	 * nearer -- the wrong clock is exactly the failure the kernel offers
	 * two calls to prevent.
	 */
	if (which == CLOCK_MONOTONIC) {
		ns = recon_sys_time();
	} else if (which == CLOCK_REALTIME) {
		ns = recon_sys_walltime();
	} else {
		return -1;
	}

	seconds = seconds_from_ns(ns);
	into->tv_sec = (time_t)seconds;
	into->tv_nsec = (long)(ns - seconds * NS_PER_SECOND);
	return 0;
}

time_t time(time_t *out)
{
	time_t seconds = (time_t)seconds_from_ns(recon_sys_walltime());

	if (out != NULL) {
		*out = seconds;
	}
	return seconds;
}

double difftime(time_t later, time_t earlier)
{
	return (double)later - (double)earlier;
}

/* --- The calendar --- */

/*
 * Howard Hinnant's civil-from-days, which is a published algorithm with a
 * proof.
 *
 * That matters more than it sounds. Calendar arithmetic people derive
 * themselves is wrong in 2100 -- which is divisible by four and is *not* a
 * leap year -- and nobody finds out for decades. `src/recon_cookie.c` and
 * `src/recon_clock.c` both use the same algorithm, in the other direction, for
 * the same reason.
 *
 * The shift by 719468 days moves the era so that March is month 0, which puts
 * the leap day at the end of a year and makes everything after it branchless.
 */
static void civil_from_days(long long days, int *year, int *month, int *day)
{
	long long z = days + 719468;
	long long era = (z >= 0 ? z : z - 146096) / 146097;
	long long doe = z - era * 146097;
	long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	long long y = yoe + era * 400;
	long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	long long mp = (5 * doy + 2) / 153;
	long long d = doy - (153 * mp + 2) / 5 + 1;
	long long m = mp + (mp < 10 ? 3 : -9);

	*year = (int)(y + (m <= 2 ? 1 : 0));
	*month = (int)m;
	*day = (int)d;
}

static int is_leap(int year)
{
	return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

struct tm *gmtime_r(const time_t *when, struct tm *into)
{
	static const int BEFORE[12] = {
		0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334,
	};
	time_t seconds;
	long long days;
	long long rest;
	int year;
	int month;
	int day;

	if (when == NULL || into == NULL) {
		return (struct tm *)0;
	}
	seconds = *when;

	/*
	 * Floored division again, and for the same reason as in `time`: the
	 * day a negative second belongs to is the one it is inside.
	 */
	days = (long long)(seconds / 86400);
	rest = (long long)(seconds % 86400);
	if (rest < 0) {
		rest += 86400;
		days -= 1;
	}

	into->tm_hour = (int)(rest / 3600);
	into->tm_min = (int)((rest % 3600) / 60);
	into->tm_sec = (int)(rest % 60);

	/* 1970-01-01 was a Thursday, day 4 counting Sunday as 0. The +11
	 * carries a negative remainder up into range before the second
	 * modulus, which is cheaper than branching on it. */
	into->tm_wday = (int)(((days % 7) + 11) % 7);

	civil_from_days(days, &year, &month, &day);

	into->tm_year = year - 1900;
	into->tm_mon = month - 1;
	into->tm_mday = day;
	into->tm_yday = BEFORE[month - 1] + day - 1 +
		((month > 2 && is_leap(year)) ? 1 : 0);

	/* Always zero. There is no summer time without a zone, and "unknown"
	 * -- which is what a negative value means -- would be a claim this
	 * cannot support either. */
	into->tm_isdst = 0;

	return into;
}

/*
 * The same function, deliberately.
 *
 * See the head of this file. This is the honest answer on a machine with no
 * host and no zone database, and it is *not* the answer the desktop relies on
 * for its clock -- `recon_clock.c` applies ReconOS's own zone to its own
 * arithmetic and never comes through here.
 */
struct tm *localtime_r(const time_t *when, struct tm *into)
{
	return gmtime_r(when, into);
}

/* --- strftime --- */

static const char *const DAY_SHORT[7] = {
	"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat",
};
static const char *const DAY_LONG[7] = {
	"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
	"Saturday",
};
static const char *const MONTH_SHORT[12] = {
	"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	"Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};
static const char *const MONTH_LONG[12] = {
	"January", "February", "March", "April", "May", "June",
	"July", "August", "September", "October", "November", "December",
};

/*
 * English, and only English -- the same decision `ctype.c` makes about
 * letters, for a related reason. A month name is not a translation of a
 * number; it is a locale's, and a locale is a database. What ReconOS would
 * need to do this properly is the thing it has decided not to carry.
 *
 * These are also exactly what the C locale gives, which is what makes the
 * differential test against the host's `strftime` mean something.
 */

struct sink {
	char *into;
	size_t room;	/* including the terminator */
	size_t used;	/* not counting the terminator */
	int overflowed;
};

static void put(struct sink *s, char c)
{
	if (s->used + 1 >= s->room) {
		s->overflowed = 1;
		return;
	}
	s->into[s->used++] = c;
}

static void put_text(struct sink *s, const char *text)
{
	while (*text != '\0') {
		put(s, *text++);
	}
}

/* Right-aligned in `width`, padded with `pad`, which is a space for %e and a
 * zero for everything else. Values here are all small and non-negative except
 * a year before 1900, which is handled by its own case. */
static void put_number(struct sink *s, long long value, int width, char pad)
{
	char digits[24];
	int n = 0;
	int negative = 0;

	if (value < 0) {
		negative = 1;
		value = -value;
	}
	do {
		digits[n++] = (char)('0' + (int)(value % 10));
		value /= 10;
	} while (value != 0 && n < (int)sizeof(digits));

	if (negative) {
		put(s, '-');
		if (width > 0) {
			width--;
		}
	}
	while (n < width) {
		put(s, pad);
		width--;
	}
	while (n > 0) {
		put(s, digits[--n]);
	}
}

static int in_range(int value, int low, int high)
{
	return value >= low && value <= high;
}

size_t strftime(char *into, size_t room, const char *format,
		const struct tm *parts)
{
	struct sink s;
	const char *at = format;

	if (into == NULL || format == NULL || parts == NULL || room == 0) {
		return 0;
	}

	s.into = into;
	s.room = room;
	s.used = 0;
	s.overflowed = 0;

	while (*at != '\0') {
		char c;

		if (*at != '%') {
			put(&s, *at++);
			continue;
		}

		c = at[1];
		if (c == '\0') {
			/* A trailing '%' is a literal one. Consuming the
			 * terminator instead would walk off the string. */
			put(&s, '%');
			break;
		}
		at += 2;

		switch (c) {
		case 'Y':
			put_number(&s, (long long)parts->tm_year + 1900, 0,
				   '0');
			break;
		case 'y':
			put_number(&s, ((parts->tm_year + 1900) % 100 + 100)
				   % 100, 2, '0');
			break;
		case 'C': {
			/*
			 * Unpadded, and floored rather than truncated: the
			 * reference gives -1 for year -1, where `-1 / 100` in
			 * C is 0. Measured, not read off the standard, which
			 * describes it as two digits.
			 */
			int year = parts->tm_year + 1900;
			int century = year / 100;

			if (year < 0 && year % 100 != 0) {
				century -= 1;
			}
			put_number(&s, century, 0, '0');
			break;
		}
		case 'm':
			put_number(&s, parts->tm_mon + 1, 2, '0');
			break;
		case 'd':
			put_number(&s, parts->tm_mday, 2, '0');
			break;
		case 'e':
			/* Space-padded rather than zero-padded, which is the
			 * whole of what makes it a different conversion. */
			put_number(&s, parts->tm_mday, 2, ' ');
			break;
		case 'H':
			put_number(&s, parts->tm_hour, 2, '0');
			break;
		case 'I': {
			int hour = parts->tm_hour % 12;

			put_number(&s, hour == 0 ? 12 : hour, 2, '0');
			break;
		}
		case 'M':
			put_number(&s, parts->tm_min, 2, '0');
			break;
		case 'S':
			put_number(&s, parts->tm_sec, 2, '0');
			break;
		case 'j':
			put_number(&s, parts->tm_yday + 1, 3, '0');
			break;
		case 'p':
			put_text(&s, parts->tm_hour < 12 ? "AM" : "PM");
			break;
		case 'a':
			put_text(&s, in_range(parts->tm_wday, 0, 6)
				 ? DAY_SHORT[parts->tm_wday] : "?");
			break;
		case 'A':
			put_text(&s, in_range(parts->tm_wday, 0, 6)
				 ? DAY_LONG[parts->tm_wday] : "?");
			break;
		case 'b':
		case 'h':
			put_text(&s, in_range(parts->tm_mon, 0, 11)
				 ? MONTH_SHORT[parts->tm_mon] : "?");
			break;
		case 'B':
			put_text(&s, in_range(parts->tm_mon, 0, 11)
				 ? MONTH_LONG[parts->tm_mon] : "?");
			break;
		case 'F':
			/* The same unpadded year %Y gives, because that is
			 * what the reference puts here -- year 1 is
			 * "1-01-01", not "0001-01-01". */
			put_number(&s, (long long)parts->tm_year + 1900, 0,
				   '0');
			put(&s, '-');
			put_number(&s, parts->tm_mon + 1, 2, '0');
			put(&s, '-');
			put_number(&s, parts->tm_mday, 2, '0');
			break;
		case 'D':
			put_number(&s, parts->tm_mon + 1, 2, '0');
			put(&s, '/');
			put_number(&s, parts->tm_mday, 2, '0');
			put(&s, '/');
			put_number(&s, ((parts->tm_year + 1900) % 100 + 100)
				   % 100, 2, '0');
			break;
		case 'T':
			put_number(&s, parts->tm_hour, 2, '0');
			put(&s, ':');
			put_number(&s, parts->tm_min, 2, '0');
			put(&s, ':');
			put_number(&s, parts->tm_sec, 2, '0');
			break;
		case 'R':
			put_number(&s, parts->tm_hour, 2, '0');
			put(&s, ':');
			put_number(&s, parts->tm_min, 2, '0');
			break;
		case 'z':
			/* Always UTC here, so always this. */
			put_text(&s, "+0000");
			break;
		case 'Z':
			/*
			 * "UTC", where the host's C locale says "GMT" for a
			 * time produced by `gmtime`. Asserted as a deliberate
			 * difference in the suite rather than matched: GMT is
			 * a zone with a history and this is not in it. What
			 * this system has is a count of seconds since 1970,
			 * and the name for that is UTC.
			 */
			put_text(&s, "UTC");
			break;
		case 'n':
			put(&s, '\n');
			break;
		case 't':
			put(&s, '\t');
			break;
		case '%':
			put(&s, '%');
			break;
		default:
			/*
			 * Written back exactly as it arrived, which is what
			 * the reference does and what `printf.c` does with an
			 * unknown conversion. A caller who used a conversion
			 * this does not have sees it in the output rather than
			 * losing the text around it.
			 */
			put(&s, '%');
			put(&s, c);
			break;
		}
	}

	/*
	 * Zero on overflow, and **the buffer's contents are unspecified** --
	 * which is what the standard says and is worth obeying rather than
	 * improving on. A caller who gets 0 and then prints the buffer anyway
	 * is a caller with a bug, and leaving a plausible-looking truncation
	 * there is how that bug stays hidden. The terminator is still written
	 * so nothing runs off the end.
	 */
	s.into[s.used < s.room ? s.used : s.room - 1] = '\0';
	return s.overflowed ? 0 : s.used;
}
