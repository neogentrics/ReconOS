/*
 * Opening a connection to somebody else, with a deadline.
 *
 * The kernel session fixed `connect` (KF-244): it answers `SYS_OK` when the
 * handshake is done, `SYS_EAGAIN` while it is in flight, and `SYS_EIO` when the
 * peer refused. Calling it again is how a caller asks -- it is idempotent, and
 * a second call reports where the first attempt got to rather than sending
 * another SYN. Without that, every poll would open another connection and the
 * socket table would fill with attempts nobody is waiting on.
 *
 * So the loop is **connect with a deadline**, not connect once plus some other
 * call to check.
 *
 * --- Three answers, not two ---
 *
 * *Not yet* and *never* are different facts and this file never conflates them.
 * A caller that reads a refusal as "try again" spins for ever on a connection
 * the peer said no to; one that reads "in flight" as a refusal abandons every
 * connection that was about to work.
 *
 * --- The deadline is the caller's, because the kernel has none ---
 *
 * `connect` has no timeout of its own. A peer that never answers -- a silently
 * dropped packet rather than a refusal -- leaves the socket in `SYN_SENT` until
 * TCP gives up on its own schedule, which is measured in minutes. For a sweep
 * of a subnet that is not a wait, it is a hang.
 *
 * So the deadline lives here, and it is a number the caller supplies along with
 * a clock. That also makes the decision testable without waiting for any of it.
 *
 * --- One source, two systems, as `serve.c` is ---
 *
 * On ReconOS the library turns `SYS_EAGAIN` into `EAGAIN`. On a host, a
 * connection in progress reports `EINPROGRESS` or `EALREADY`, and one already
 * up reports `EISCONN`. All of those mean the same two things, so all of them
 * are handled -- which is what lets the decision below be exercised on a host
 * against real sockets rather than against a stand-in.
 */

#ifndef RECON_DIAL_H
#define RECON_DIAL_H

/* What a poll came to. */
#define DIAL_READY     0	/* established -- write to it */
#define DIAL_PENDING   1	/* in flight -- ask again */
#define DIAL_REFUSED (-1)	/* the peer said no; stop */
#define DIAL_TIMEDOUT (-2)	/* the deadline passed with no answer */
#define DIAL_BROKEN  (-3)	/* no socket, or the call was rejected outright */

struct dial {
	int fd;			/* -1 when there is none */
	unsigned int addr;	/* host order, as the kernel's calls take */
	unsigned short port;
	unsigned long deadline;	/* in the caller's clock */
	unsigned long attempts;	/* how many times it has been asked */
};

/*
 * The decision, separated from the call that produces it.
 *
 * `rc` is what `connect` returned and `err` is `errno` after it. `now` and
 * `deadline` are in whatever units the caller keeps time in; only their order
 * matters.
 *
 * Pure, so the suite drives every combination in microseconds rather than
 * waiting for handshakes. **This is the part that can be wrong**: the I/O
 * around it is four lines.
 */
int dial_verdict(int rc, int err, unsigned long now, unsigned long deadline);

/*
 * Open a socket and send the first SYN.
 *
 * Returns a verdict: `DIAL_READY` if it completed at once -- which happens on
 * a host connecting to loopback -- `DIAL_PENDING` if it is in flight, or a
 * negative reason. The socket is closed on any negative answer, so a caller
 * that gets one has nothing to clean up.
 */
int dial_begin(struct dial *d, unsigned int addr, unsigned short port,
               unsigned long now, unsigned long deadline);

/*
 * Ask again.
 *
 * Calls `connect` on the same socket, which is how the state is asked for.
 * Returns the same verdicts. As with `dial_begin`, the socket is closed on a
 * negative answer.
 */
int dial_poll(struct dial *d, unsigned long now);

/* Hand back the descriptor, leaving the dial empty. The caller owns it
 * afterwards and must close it. */
int dial_take(struct dial *d);

/* Close whatever is held. Safe on an empty dial, and safe twice. */
void dial_close(struct dial *d);

#endif
