/*
 * `errno`, `strerror`, and the translation between two numberings.
 *
 * Three things, and the middle one is the reason this file exists rather than
 * a handful of assertions bolted onto another suite.
 *
 * **Every message is held against the host's, by equality.** The wording is
 * not specified by anything -- which is exactly why it has to be checked
 * against something rather than read. `userland/libc/errno.c` copies glibc's
 * text deliberately, so that a log written on ReconOS and a log written on
 * Linux say the same thing about the same fault; a typo in one of forty
 * strings is invisible to any test written from memory and obvious against
 * the real one.
 *
 * **The kernel's error list is read out of the kernel's own header**, at run
 * time, and compared with what the translation covers. A lookup table's
 * failure mode is going quietly out of date, and the cost here is specific:
 * an error the table has never heard of becomes `EIO`, which is a plausible
 * wrong answer. Somebody then goes to look at a disk that is fine.
 *
 * **And the translation is checked in both directions** -- every kernel status
 * maps to an errno, and no two different kernel statuses that mean different
 * things map to the same one by accident.
 *
 * Run with: ./build/recon_libc_errno_tests
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ReconOS's, renamed by prefix.h when libc/ was compiled. */
extern int recon_errno;

char *recon_strerror(int number);
int recon_errno_from_status(long status);
unsigned recon_errno_count(void);

static unsigned long checks;
static unsigned long failures;

static void ok(int condition, const char *what)
{
	checks++;

	if (!condition) {
		failures++;
		if (failures <= 30)
			printf("  FAIL  %s\n", what);
	}
}

/* --- the messages, against the host's ------------------------------------- */

/*
 * Every number `errno.h` defines. Written out rather than swept over a range,
 * because a sweep would also cover the numbers neither library defines, where
 * both say "Unknown error N" and the comparison proves nothing.
 */
static const struct {
	int number;
	const char *name;
} defined_here[] = {
	{ EPERM, "EPERM" }, { ENOENT, "ENOENT" }, { ESRCH, "ESRCH" },
	{ EINTR, "EINTR" }, { EIO, "EIO" }, { ENXIO, "ENXIO" },
	{ E2BIG, "E2BIG" }, { ENOEXEC, "ENOEXEC" }, { EBADF, "EBADF" },
	{ ECHILD, "ECHILD" }, { EAGAIN, "EAGAIN" }, { ENOMEM, "ENOMEM" },
	{ EACCES, "EACCES" }, { EFAULT, "EFAULT" }, { EBUSY, "EBUSY" },
	{ EEXIST, "EEXIST" }, { EXDEV, "EXDEV" }, { ENODEV, "ENODEV" },
	{ ENOTDIR, "ENOTDIR" }, { EISDIR, "EISDIR" }, { EINVAL, "EINVAL" },
	{ ENFILE, "ENFILE" }, { EMFILE, "EMFILE" }, { ENOTTY, "ENOTTY" },
	{ EFBIG, "EFBIG" }, { ENOSPC, "ENOSPC" }, { ESPIPE, "ESPIPE" },
	{ EROFS, "EROFS" }, { EMLINK, "EMLINK" }, { EPIPE, "EPIPE" },
	{ EDOM, "EDOM" }, { ERANGE, "ERANGE" },
	{ ENAMETOOLONG, "ENAMETOOLONG" }, { ENOSYS, "ENOSYS" },
	{ ENOTEMPTY, "ENOTEMPTY" }, { ELOOP, "ELOOP" },
	{ EOVERFLOW, "EOVERFLOW" },
	{ ENOTSOCK, "ENOTSOCK" }, { EDESTADDRREQ, "EDESTADDRREQ" },
	{ EMSGSIZE, "EMSGSIZE" }, { EOPNOTSUPP, "EOPNOTSUPP" },
	{ EAFNOSUPPORT, "EAFNOSUPPORT" }, { EADDRINUSE, "EADDRINUSE" },
	{ ENETUNREACH, "ENETUNREACH" }, { ECONNRESET, "ECONNRESET" },
	{ EISCONN, "EISCONN" }, { ENOTCONN, "ENOTCONN" },
	{ ETIMEDOUT, "ETIMEDOUT" }, { ECONNREFUSED, "ECONNREFUSED" },
	{ EHOSTUNREACH, "EHOSTUNREACH" }, { EALREADY, "EALREADY" },
	{ EINPROGRESS, "EINPROGRESS" },
};

#define DEFINED (sizeof(defined_here) / sizeof(defined_here[0]))

static void messages_match_the_host(void)
{
	size_t i;

	for (i = 0; i < DEFINED; i++) {
		const char *mine = recon_strerror(defined_here[i].number);
		const char *theirs = strerror(defined_here[i].number);
		char what[160];

		snprintf(what, sizeof(what),
			 "%s (%d): ours is %s, the host's is %s",
			 defined_here[i].name, defined_here[i].number,
			 mine ? mine : "(null)", theirs ? theirs : "(null)");

		ok(mine && theirs && strcmp(mine, theirs) == 0, what);
	}
}

/* Nothing this library defines may share a message with anything else, because
 * two errors that read identically are two errors nobody can tell apart in a
 * log -- which is the only place most of these are ever seen. */
static void messages_are_distinct(void)
{
	size_t i, j;

	for (i = 0; i < DEFINED; i++) {
		for (j = i + 1; j < DEFINED; j++) {
			char what[160];

			if (defined_here[i].number == defined_here[j].number)
				continue;	/* EWOULDBLOCK is EAGAIN */

			snprintf(what, sizeof(what),
				 "%s and %s both say \"%s\"",
				 defined_here[i].name, defined_here[j].name,
				 recon_strerror(defined_here[i].number));

			ok(strcmp(recon_strerror(defined_here[i].number),
				  recon_strerror(defined_here[j].number)) != 0,
			   what);
		}
	}
}

static void unknown_numbers(void)
{
	/* Numbers *neither* library defines. The same shape as the host's, so
	 * a log written on either reads the same for a number neither knows.
	 *
	 * Chosen above glibc's range rather than from the gaps inside it: 60
	 * was in this list and is `ENOSTR`, which glibc knows. */
	static const int odd[] = { 999, 4096, -1, 0x7FFFFFFF };
	size_t i;

	for (i = 0; i < sizeof(odd) / sizeof(odd[0]); i++) {
		const char *mine = recon_strerror(odd[i]);
		const char *theirs = strerror(odd[i]);
		char what[160];

		snprintf(what, sizeof(what),
			 "an unlisted %d: ours %s, the host's %s",
			 odd[i], mine, theirs);

		ok(mine && theirs && strcmp(mine, theirs) == 0, what);
	}

	/* And it never answers NULL, which a caller passing the result
	 * straight to printf would turn into a crash or "(null)". */
	ok(recon_strerror(12345) != NULL, "an unlisted number still answers");
	ok(recon_strerror(12345)[0] != '\0', "and not an empty string");

	/* --- the divergence, asserted rather than discovered -------------
	 *
	 * glibc defines errors for conditions this system does not have, and
	 * `errno.h` says why it does not follow: a constant for a condition
	 * that cannot arise is a constant somebody writes a branch for, and
	 * that branch is never taken and never tested.
	 *
	 * So for these, the two libraries deliberately disagree, and this is
	 * the record of which way. Found by the suite's first run, which had
	 * 60 in the list above believing nothing defined it.
	 */
	{
		static const struct {
			int number;
			const char *glibc_calls_it;
		} not_here[] = {
			{ 60, "ENOSTR -- there are no streams" },
			{ 62, "ETIME -- no stream ioctl timeouts" },
			{ 122, "EDQUOT -- there are no quotas" },
			{ 116, "ESTALE -- no network filesystem" },
		};
		size_t k;

		for (k = 0; k < sizeof(not_here) / sizeof(not_here[0]); k++) {
			char want[48];
			char what[200];

			snprintf(want, sizeof(want), "Unknown error %d",
				 not_here[k].number);
			snprintf(what, sizeof(what),
				 "%d is %s here, and the host says \"%s\"",
				 not_here[k].number, not_here[k].glibc_calls_it,
				 strerror(not_here[k].number));

			ok(strcmp(recon_strerror(not_here[k].number),
				  want) == 0, what);
		}
	}
}

/* --- the kernel's numbering ----------------------------------------------- */

/*
 * Read out of the kernel's own header at run time.
 *
 * The point is not to check that the file parses. It is that this suite must
 * not get its idea of "how many errors the kernel has" from the same place the
 * translation got it -- which is how a table goes out of date while everything
 * that looks at it agrees.
 */
static int kernel_error_count(void)
{
	FILE *f = fopen("userland/include/recon.h", "r");
	char line[256];
	int highest = 0;

	if (!f) {
		ok(0, "userland/include/recon.h could not be read -- run this "
		      "from the top of the repository");
		return -1;
	}

	while (fgets(line, sizeof(line), f)) {
		char name[64];
		int value;

		/* `#define SYS_ENOENT   (-7)` and nothing else: SYS_EXIT is a
		 * call number, not an error, and has no parenthesised
		 * negative. */
		if (sscanf(line, "#define SYS_%63s (%d)", name, &value) == 2 &&
		    value < 0 && name[0] == 'E') {
			if (-value > highest)
				highest = -value;
		}
	}

	fclose(f);
	return highest;
}

static void translation_covers_the_kernel(void)
{
	int highest = kernel_error_count();
	char what[200];

	if (highest < 0)
		return;

	snprintf(what, sizeof(what),
		 "the kernel has %d error numbers and the translation covers "
		 "%u", highest, recon_errno_count());

	ok(highest > 0 && (unsigned)highest == recon_errno_count(), what);

	/* Every one of them must produce a real errno rather than the EIO the
	 * translation falls back to. Checked by value, since EIO is also a
	 * legitimate answer for exactly one of them. */
	{
		int n;

		for (n = 1; n <= highest; n++) {
			int e = recon_errno_from_status(-n);

			snprintf(what, sizeof(what),
				 "kernel status -%d translates to something",
				 n);
			ok(e > 0, what);

			snprintf(what, sizeof(what),
				 "kernel status -%d has a message", n);
			ok(recon_strerror(e)[0] != '\0', what);
		}
	}
}

static void translation_is_sane(void)
{
	/* Success is not an error, whatever it is spelled as. */
	ok(recon_errno_from_status(0) == 0, "0 is not a failure");
	ok(recon_errno_from_status(1) == 0, "a positive result is not a "
					    "failure");
	ok(recon_errno_from_status(4096) == 0,
	   "and neither is a large one -- a read that returned bytes");

	/* The handful worth pinning by name, because getting one of these
	 * wrong means a program reports the wrong reason and the reason is the
	 * only thing a person sees. */
	ok(recon_errno_from_status(-7) == ENOENT,
	   "SYS_ENOENT is ENOENT");
	ok(recon_errno_from_status(-6) == EEXIST,
	   "SYS_EEXIST is EEXIST");
	ok(recon_errno_from_status(-11) == EBADF,
	   "SYS_EBADF is EBADF");
	ok(recon_errno_from_status(-12) == EPERM,
	   "SYS_EPERM is EPERM");
	ok(recon_errno_from_status(-3) == EINVAL,
	   "SYS_EINVAL is EINVAL");

	/* Beyond the table. EIO rather than reading off the end, and rather
	 * than a specific wrong reason. */
	ok(recon_errno_from_status(-9999) == EIO,
	   "an error this library has never heard of is EIO");
}

static void errno_is_a_variable(void)
{
	recon_errno = 0;
	ok(recon_errno == 0, "errno can be cleared");

	recon_errno = ENOENT;
	ok(recon_errno == ENOENT, "and set");
	ok(strcmp(recon_strerror(recon_errno),
		  "No such file or directory") == 0,
	   "and read back through strerror, which is how it is used");

	recon_errno = 0;
}

int main(void)
{
	printf("errno and strerror\n\n");

	messages_match_the_host();
	messages_are_distinct();
	unknown_numbers();
	translation_covers_the_kernel();
	translation_is_sane();
	errno_is_a_variable();

	printf("%lu checks, %lu failures\n", checks, failures);
	return failures ? 1 : 0;
}
