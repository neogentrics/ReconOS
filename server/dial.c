/*
 * Connecting, and knowing which of the three answers came back.
 *
 * See `dial.h` for why the loop is `connect` called again rather than `connect`
 * plus something else, and for why the deadline belongs to the caller.
 *
 * --- Why this file names error constants and not their numbers ---
 *
 * The note announcing KF-244 gave the numbers as `SYS_EAGAIN (-12)` and
 * `SYS_EIO (-4)`. The header says `SYS_EAGAIN` is **-4** and `SYS_EIO` is
 * **-9**; -12 is `SYS_EPERM`.
 *
 * Had this file been written against those numbers it would have had the two
 * cases **exactly backwards**: a real "in flight" (-4) read as "the peer
 * refused", so every connection that was about to work would have been
 * abandoned -- and `SYS_EPERM` (-12) read as "ask again", so a permission
 * refusal would have spun for ever. That is precisely the failure the note
 * warned about, arrived at by trusting a transcribed constant.
 *
 * So nothing here compares against a number. The library maps the kernel's
 * status to `errno` and this reads `errno` by name, which is correct whatever
 * the numbers turn out to be.
 *
 * --- What was tried and rejected ---
 *
 * **A non-blocking socket and a readiness check.** That is how this is done
 * everywhere else and it needs `fcntl`, which ReconOS does not have, and a
 * readiness call, which it also does not have. Polling `connect` is not a
 * workaround here; it is the interface.
 *
 * **Closing and reopening on each attempt.** It looks like a way to retry and
 * it is how the socket table fills with half-open attempts nobody is waiting
 * on. The kernel made `connect` idempotent so that this would not be
 * necessary; one socket, asked repeatedly.
 */

#include "dial.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

int dial_verdict(int rc, int err, unsigned long now, unsigned long deadline)
{
	if (rc == 0)
		return DIAL_READY;

	/*
	 * Already connected. A host answers a second `connect` on a finished
	 * socket this way rather than with success, so treating it as anything
	 * but ready would mean a connection that worked was reported as broken.
	 */
	if (err == EISCONN)
		return DIAL_READY;

	/*
	 * In flight. Three spellings of one fact: ReconOS's, and the two a host
	 * uses for a first and a subsequent attempt.
	 *
	 * The deadline is checked *here* rather than before the call, so a
	 * handshake that completed on the very attempt that crossed the
	 * deadline is reported as ready. Giving up on a connection that had
	 * just succeeded would be a needless failure at exactly the moment the
	 * work was done.
	 */
	if (err == EAGAIN || err == EINPROGRESS || err == EALREADY) {
		if (now >= deadline)
			return DIAL_TIMEDOUT;
		return DIAL_PENDING;
	}

	/*
	 * Interrupted before anything happened. Not a refusal and not progress
	 * -- ask again, subject to the same deadline.
	 */
	if (err == EINTR) {
		if (now >= deadline)
			return DIAL_TIMEDOUT;
		return DIAL_PENDING;
	}

	/*
	 * Everything else is *never*, and is deliberately not split further.
	 *
	 * A refusal, an unreachable network, a permission denied: a caller
	 * sweeping a subnet does the same thing with all of them, which is move
	 * on. Reporting them separately would invite a caller to treat one of
	 * them as retryable, which is how the spin the kernel's note warns
	 * about gets written.
	 */
	return DIAL_REFUSED;
}

int dial_begin(struct dial *d, unsigned int addr, unsigned short port,
               unsigned long now, unsigned long deadline)
{
	struct sockaddr_in to;
	int rc, verdict;

	if (!d)
		return DIAL_BROKEN;

	d->fd = -1;
	d->addr = addr;
	d->port = port;
	d->deadline = deadline;
	d->attempts = 0;

	d->fd = socket(AF_INET, SOCK_STREAM, 0);
	if (d->fd < 0)
		return DIAL_BROKEN;

	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = (unsigned short)((port >> 8) | (port << 8));
	to.sin_addr.s_addr = (unsigned int)((addr >> 24)
	                                    | ((addr >> 8) & 0x0000FF00u)
	                                    | ((addr << 8) & 0x00FF0000u)
	                                    | (addr << 24));

	errno = 0;
	rc = connect(d->fd, (struct sockaddr *)&to, sizeof(to));
	d->attempts++;

	verdict = dial_verdict(rc, errno, now, deadline);
	if (verdict < 0)
		dial_close(d);
	return verdict;
}

int dial_poll(struct dial *d, unsigned long now)
{
	struct sockaddr_in to;
	int rc, verdict;

	if (!d || d->fd < 0)
		return DIAL_BROKEN;

	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = (unsigned short)((d->port >> 8) | (d->port << 8));
	to.sin_addr.s_addr = (unsigned int)((d->addr >> 24)
	                                    | ((d->addr >> 8) & 0x0000FF00u)
	                                    | ((d->addr << 8) & 0x00FF0000u)
	                                    | (d->addr << 24));

	/* The same socket, asked again. See `dial.h`: this is the interface,
	 * and it does not send a second SYN. */
	errno = 0;
	rc = connect(d->fd, (struct sockaddr *)&to, sizeof(to));
	d->attempts++;

	verdict = dial_verdict(rc, errno, now, d->deadline);
	if (verdict < 0)
		dial_close(d);
	return verdict;
}

int dial_take(struct dial *d)
{
	int fd;

	if (!d)
		return -1;
	fd = d->fd;
	d->fd = -1;
	return fd;
}

void dial_close(struct dial *d)
{
	if (!d || d->fd < 0)
		return;
	close(d->fd);
	d->fd = -1;
}

const char *dial_says(int verdict)
{
	switch (verdict) {
	case DIAL_READY:    return "ready";
	case DIAL_PENDING:  return "in flight";
	case DIAL_REFUSED:  return "refused";
	case DIAL_TIMEDOUT: return "timed out";
	case DIAL_BROKEN:   return "broken";
	default:            return "not a verdict";
	}
}
