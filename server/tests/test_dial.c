/*
 * Three answers, and the two ways of confusing them.
 *
 * `dial_verdict` is the whole of the decision and is pure, so every
 * combination of return value, `errno`, and where the clock stands is driven
 * here in microseconds -- rather than by waiting for handshakes that take
 * minutes to fail.
 *
 * **The two failures this is really about**, both named in the kernel's own
 * note announcing KF-244:
 *
 *   *never* read as *not yet*  -- a caller spins for ever on a connection the
 *                                 peer refused.
 *   *not yet* read as *never*  -- a caller abandons every connection that was
 *                                 about to work.
 *
 * Neither produces an error anywhere. The first is a sweep that never finishes;
 * the second is a sweep that finds nothing and reports that cheerfully.
 *
 * The end of the file connects to real sockets on the host, which proves the
 * ready and refused paths against something that is not a stand-in. The
 * *pending* path cannot be shown on a host -- a blocking `connect` to loopback
 * finishes before it returns -- so it is shown above, where the decision lives.
 */

#include "../dial.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

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

static void verdict(int rc, int err, unsigned long now, unsigned long deadline,
                    int want, const char *what)
{
	int got = dial_verdict(rc, err, now, deadline);

	checks++;
	if (got != want) {
		failures++;
		printf("  FAIL  %s: got %d, wanted %d\n", what, got, want);
	}
}

int main(void)
{
	printf("three answers, and the two ways of confusing them\n");

	/* --- established ------------------------------------------------------- */
	verdict(0, 0, 0, 100, DIAL_READY, "success is ready");
	verdict(0, ECONNREFUSED, 0, 100, DIAL_READY,
	        "success is ready whatever errno happens to hold");
	verdict(-1, EISCONN, 0, 100, DIAL_READY,
	        "already connected is ready -- a host answers a second ask this way");

	/* Ready even past the deadline. A handshake that completed on the very
	 * attempt that crossed it has done the work; giving up then would be a
	 * needless failure at the moment of success. */
	verdict(0, 0, 500, 100, DIAL_READY,
	        "a connection that completed is ready even past the deadline");
	verdict(-1, EISCONN, 500, 100, DIAL_READY,
	        "and so is one already established");

	/* --- in flight ---------------------------------------------------------- */
	verdict(-1, EAGAIN, 0, 100, DIAL_PENDING,
	        "EAGAIN is not yet -- what ReconOS says");
	verdict(-1, EINPROGRESS, 0, 100, DIAL_PENDING,
	        "EINPROGRESS is not yet -- what a host says first");
	verdict(-1, EALREADY, 0, 100, DIAL_PENDING,
	        "EALREADY is not yet -- what a host says after that");
	verdict(-1, EINTR, 0, 100, DIAL_PENDING,
	        "interrupted is not yet either");

	/* --- the deadline -------------------------------------------------------- */
	verdict(-1, EAGAIN, 99, 100, DIAL_PENDING, "just inside the deadline");
	verdict(-1, EAGAIN, 100, 100, DIAL_TIMEDOUT, "exactly on it");
	verdict(-1, EAGAIN, 101, 100, DIAL_TIMEDOUT, "past it");
	verdict(-1, EINTR, 200, 100, DIAL_TIMEDOUT,
	        "an interruption past the deadline gives up too");

	/*
	 * The deadline applies only to *not yet*. A refusal past the deadline is
	 * still a refusal -- reporting it as a timeout would tell a caller to
	 * try again later against a peer that said no.
	 */
	verdict(-1, ECONNREFUSED, 500, 100, DIAL_REFUSED,
	        "a refusal past the deadline is a refusal, not a timeout");

	/* --- never ---------------------------------------------------------------
	 *
	 * All one answer on purpose. A caller sweeping a subnet does the same
	 * thing with every one of these, and reporting them apart would invite
	 * treating one as retryable -- which is the spin the kernel's note
	 * warns about. */
	verdict(-1, ECONNREFUSED, 0, 100, DIAL_REFUSED, "refused");
	verdict(-1, ENETUNREACH, 0, 100, DIAL_REFUSED, "no route");
	verdict(-1, EHOSTUNREACH, 0, 100, DIAL_REFUSED, "host unreachable");
	verdict(-1, EACCES, 0, 100, DIAL_REFUSED, "not permitted");
	verdict(-1, EPERM, 0, 100, DIAL_REFUSED,
	        "EPERM is never -- the number the announcement gave for EAGAIN");
	verdict(-1, EBADF, 0, 100, DIAL_REFUSED, "not a socket");

	/*
	 * The case the file exists for, stated as a check.
	 *
	 * Had this been written against the numbers in the announcement --
	 * `SYS_EAGAIN (-12)`, `SYS_EIO (-4)` -- the two would have been exactly
	 * swapped. These two lines are the difference.
	 */
	ok(dial_verdict(-1, EAGAIN, 0, 100) == DIAL_PENDING
	   && dial_verdict(-1, ECONNREFUSED, 0, 100) == DIAL_REFUSED,
	   "not-yet and never are not interchangeable");

	/* --- against real sockets -------------------------------------------------
	 *
	 * A listener that exists, and a port that does not. Both on loopback, so
	 * both finish before `connect` returns -- which is why the pending path
	 * is proved above and not here. */
	{
		struct dial d;
		struct sockaddr_in addr;
		int listener = socket(AF_INET, SOCK_STREAM, 0);
		unsigned port = 18500;
		int bound = 0;

		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(0x7F000001);

		while (port < 18600 && !bound) {
			addr.sin_port = htons((unsigned short)port);
			if (bind(listener, (struct sockaddr *)&addr,
			         sizeof(addr)) == 0
			    && listen(listener, 4) == 0)
				bound = 1;
			else
				port++;
		}
		ok(bound, "a listener for the real-socket cases");

		if (bound) {
			int rc = dial_begin(&d, 0x7F000001u,
			                    (unsigned short)port, 0, 1000);

			ok(rc == DIAL_READY,
			   "a listener that exists answers ready");
			ok(d.fd >= 0, "and the socket is held for the caller");

			{
				int taken = dial_take(&d);

				ok(taken >= 0, "which the caller can take");
				ok(d.fd < 0, "leaving the dial empty");
				if (taken >= 0)
					close(taken);
			}
			dial_close(&d);	/* safe on an empty dial */

			/* A port nothing is listening on. Under the old kernel
			 * this answered SYS_OK, which is the whole reason
			 * KF-244 existed. */
			rc = dial_begin(&d, 0x7F000001u, (unsigned short)(port + 1),
			                0, 1000);
			ok(rc == DIAL_REFUSED,
			   "a port nothing is listening on answers refused");
			ok(d.fd < 0,
			   "and the socket is closed, so a caller has nothing to tidy");

			close(listener);
		}
	}

	/* --- nothing at all ------------------------------------------------------- */
	{
		struct dial d;

		memset(&d, 0, sizeof(d));
		d.fd = -1;
		ok(dial_poll(&d, 0) == DIAL_BROKEN, "polling an empty dial");
		ok(dial_begin(0, 0, 80, 0, 100) == DIAL_BROKEN, "no dial at all");
		ok(dial_take(0) == -1, "taking from no dial");
		dial_close(0);		/* must not crash */
		dial_close(&d);
		ok(1, "closing nothing is safe, twice");
	}

	/* --- the verdict as a word -------------------------------------------
	 *
	 * Read off a serial console by a person, so it has to say something.
	 * Checked here because a printed verdict that is wrong is wrong in the
	 * one place nobody goes back to re-read. */
	ok(strcmp(dial_says(DIAL_READY), "ready") == 0, "ready reads as ready");
	ok(strcmp(dial_says(DIAL_PENDING), "in flight") == 0,
	   "and pending says in flight, which is what it means");
	ok(strcmp(dial_says(DIAL_REFUSED), "refused") == 0, "refused");
	ok(strcmp(dial_says(DIAL_TIMEDOUT), "timed out") == 0, "timed out");
	ok(strcmp(dial_says(DIAL_BROKEN), "broken") == 0, "broken");
	ok(dial_says(42) != 0,
	   "and something that is not a verdict still answers, rather than"
	   " handing a caller a null to print");

	printf("  %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
