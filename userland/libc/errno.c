/*
 * `errno`, `strerror`, and the one place the kernel's numbering meets C's.
 *
 * The desktop calls `strerror` at **43 sites**, measured by
 * `scripts/measure-libc.py` -- one symbol, and the third largest group of
 * anything still missing. It is also the one group that needs nothing from the
 * kernel: the kernel already answers with numbers, and turning a number into a
 * sentence is entirely this side's work.
 *
 * --- Two numberings, and why they are not the same one ---
 *
 * A ReconOS system call answers a negative number of its own: `SYS_ENOENT` is
 * -7. C says a failing library call sets `errno`, and every program ever
 * written believes `ENOENT` is 2. `userland/include/errno.h` says why the C
 * side keeps the familiar values; this file is the translation, and being the
 * only place the two meet is the point of it.
 *
 * `recon_errno_from_status` takes what a system call returned -- negative, or
 * a result -- and answers the `errno` for it, so that every wrapper in this
 * library has one line for the failure case rather than a table of its own.
 *
 * --- The messages are glibc's, word for word ---
 *
 * Not because the wording is specified -- it is not -- but because a message
 * this library invents is a message that differs from the one the same program
 * prints today on Linux, for the same fault, and somebody will eventually
 * compare two logs. Copying them also makes the whole thing testable against
 * the host by equality rather than by "is it a non-empty string", which is the
 * method the rest of this library is held to.
 */

#include "internal.h"

/* The public header, by relative path.
 *
 * The second file in this library to do it, and for the same reason `time.c`
 * does: the rule against including public headers exists so the differential
 * build cannot compare a function against itself, and that cannot happen here.
 * Nothing puts the host's <errno.h> on this library's include path, and
 * `prefix.h` arrives by `-include` so it is processed first and renames what
 * this declares on the way past.
 *
 * What it buys is one set of numbers rather than two that have to be kept
 * identical by hand -- which, for a file whose entire job is translating
 * between two numberings, would be the obvious thing to get wrong.
 */
#include "../include/errno.h"

int errno;

/* --- the kernel's numbering, into C's ------------------------------------- */

/*
 * One table rather than a switch, so that adding a kernel error means adding a
 * line -- and so that the count below can be checked against the kernel's own
 * list, which is what stops the two drifting.
 *
 * Indexed by the negation of the kernel's value: entry 7 is `SYS_ENOENT`.
 * Entry 0 is unused; a status of 0 is success and never reaches here.
 */
static const int from_kernel[] = {
	0,		/*   0  -- success, never translated */
	ENOSYS,		/*  -1  SYS_ENOSYS   */
	EFAULT,		/*  -2  SYS_EFAULT   */
	EINVAL,		/*  -3  SYS_EINVAL   */
	EAGAIN,		/*  -4  SYS_EAGAIN   */
	ENODEV,		/*  -5  SYS_ENODEV   */
	EEXIST,		/*  -6  SYS_EEXIST   */
	ENOENT,		/*  -7  SYS_ENOENT   */
	ENOSPC,		/*  -8  SYS_ENOSPC   */
	EIO,		/*  -9  SYS_EIO      */
	EMFILE,		/* -10  SYS_EMFILE   */
	EBADF,		/* -11  SYS_EBADF    */
	EPERM,		/* -12  SYS_EPERM    */
	EPIPE,		/* -13  SYS_EPIPE    */
	ENOMEM,		/* -14  SYS_ENOMEM   */

	/*
	 * The three the kernel grew for turning a machine off, and they have no
	 * C equivalent because C has never had to describe a computer that
	 * cannot switch itself off.
	 *
	 * `ENOSYS` for all three, which is the honest reading: the machine does
	 * not implement the thing that was asked. Flattening three into one
	 * loses a distinction, so the *wrapper* that meets them is expected to
	 * say which before it returns -- see `recon_power` in recon.h, whose
	 * caller has the number itself and does not have to go through errno.
	 */
	ENOSYS,		/* -15  SYS_ENOPOWER -- no way to cut the power   */
	ENOSYS,		/* -16  SYS_ENOSTATE -- no sleep state like that  */
	ENOSYS,		/* -17  SYS_ENOMECH  -- no mechanism for it       */
};

#define KERNEL_ERRORS (sizeof(from_kernel) / sizeof(from_kernel[0]))

int recon_errno_from_status(long status)
{
	long index;

	if (status >= 0)
		return 0;

	index = -status;

	/*
	 * A number this library has never heard of. `EIO` rather than a
	 * guess -- something went wrong at a layer that could not say what,
	 * which is exactly what EIO means, and is better than reporting a
	 * specific wrong reason.
	 *
	 * It is also the line that fires if the kernel grows an error and this
	 * file is not told, which is why `recon_errno_count()` exists below.
	 */
	if (index >= (long)KERNEL_ERRORS)
		return EIO;

	return from_kernel[index];
}

/* How many kernel errors this file knows how to translate.
 *
 * Asked by the suite and compared against the kernel's own list, because the
 * failure of a lookup table is that it goes quietly out of date -- and an
 * untranslated error becomes a plausible-looking EIO, which sends somebody to
 * look at a disk that is fine. */
unsigned recon_errno_count(void)
{
	return (unsigned)KERNEL_ERRORS - 1;	/* entry 0 is not an error */
}

/* --- numbers into sentences ----------------------------------------------- */

/*
 * A table indexed by `errno`, with holes.
 *
 * Sparse on purpose: the numbers are Linux's, and Linux's are not contiguous
 * -- there is no 15, and nothing between 40 and 75. The holes are cheap (a
 * pointer each) and the alternative is a switch that a reader has to scan to
 * find out which numbers are covered.
 */
static const char *const messages[] = {
	/*  0 */ "Success",
	/*  1 */ "Operation not permitted",
	/*  2 */ "No such file or directory",
	/*  3 */ "No such process",
	/*  4 */ "Interrupted system call",
	/*  5 */ "Input/output error",
	/*  6 */ "No such device or address",
	/*  7 */ "Argument list too long",
	/*  8 */ "Exec format error",
	/*  9 */ "Bad file descriptor",
	/* 10 */ "No child processes",
	/* 11 */ "Resource temporarily unavailable",
	/* 12 */ "Cannot allocate memory",
	/* 13 */ "Permission denied",
	/* 14 */ "Bad address",
	/* 15 */ 0,
	/* 16 */ "Device or resource busy",
	/* 17 */ "File exists",
	/* 18 */ "Invalid cross-device link",
	/* 19 */ "No such device",
	/* 20 */ "Not a directory",
	/* 21 */ "Is a directory",
	/* 22 */ "Invalid argument",
	/* 23 */ "Too many open files in system",
	/* 24 */ "Too many open files",
	/* 25 */ "Inappropriate ioctl for device",
	/* 26 */ 0,
	/* 27 */ "File too large",
	/* 28 */ "No space left on device",
	/* 29 */ "Illegal seek",
	/* 30 */ "Read-only file system",
	/* 31 */ "Too many links",
	/* 32 */ "Broken pipe",
	/* 33 */ "Numerical argument out of domain",
	/* 34 */ "Numerical result out of range",
	/* 35 */ 0,
	/* 36 */ "File name too long",
	/* 37 */ 0,
	/* 38 */ "Function not implemented",
	/* 39 */ "Directory not empty",
	/* 40 */ "Too many levels of symbolic links",
};

#define MESSAGES (sizeof(messages) / sizeof(messages[0]))

/* The ones far enough up the range that a table with holes would be mostly
 * holes. Sockets are at 88 and above, and there is nothing between 41 and 74
 * this system can produce. */
static const struct {
	int number;
	const char *text;
} scattered[] = {
	{ EOVERFLOW,	"Value too large for defined data type" },
	{ ENOTSOCK,	"Socket operation on non-socket" },
	{ EDESTADDRREQ,	"Destination address required" },
	{ EMSGSIZE,	"Message too long" },
	{ EOPNOTSUPP,	"Operation not supported" },
	{ EAFNOSUPPORT,	"Address family not supported by protocol" },
	{ EADDRINUSE,	"Address already in use" },
	{ ENETUNREACH,	"Network is unreachable" },
	{ ECONNRESET,	"Connection reset by peer" },
	{ EISCONN,	"Transport endpoint is already connected" },
	{ ENOTCONN,	"Transport endpoint is not connected" },
	{ ETIMEDOUT,	"Connection timed out" },
	{ ECONNREFUSED,	"Connection refused" },
	{ EHOSTUNREACH,	"No route to host" },
	{ EALREADY,	"Operation already in progress" },
	{ EINPROGRESS,	"Operation now in progress" },
};

#define SCATTERED (sizeof(scattered) / sizeof(scattered[0]))

/*
 * Room for the longest thing `unknown` can produce: the text, a sign, twenty
 * digits for a 64-bit number rendered through an int, and a terminator.
 */
static char unknown[48];

char *strerror(int number)
{
	size_t i;

	if (number >= 0 && (size_t)number < MESSAGES && messages[number])
		return (char *)messages[number];

	for (i = 0; i < SCATTERED; i++) {
		if (scattered[i].number == number)
			return (char *)scattered[i].text;
	}

	/*
	 * The same shape the host uses, so that a log written on ReconOS and a
	 * log written on Linux read the same for a number neither of them
	 * knows. Returning a fixed "Unknown error" instead would throw away the
	 * one piece of information the caller has.
	 *
	 * A static buffer, which is what `strerror` has always been and is why
	 * `strerror_r` exists. Said here because it is a real hazard the day
	 * this system has threads: two of them in this branch at once get one
	 * buffer between them.
	 */
	snprintf(unknown, sizeof(unknown), "Unknown error %d", number);
	return unknown;
}
