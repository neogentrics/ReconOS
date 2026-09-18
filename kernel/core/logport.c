/*
 * The boot log, offered over TCP. See logport.h for what this is not.
 */
#include <recon/kernel/logport.h>
#include <recon/kernel/net.h>
#include <recon/kernel/klog.h>
#include <recon/kernel/sched.h>
#include <recon/kernel/boot.h>
#include <recon/kernel/console.h>
#include <recon/kernel/timer.h>
#include <recon/kernel/heap.h>

/* Why it is not listening, when it is not.
 *
 * Three different reasons, and a summary that said only "not listening" would
 * make the ordinary case (nobody asked) look like the two faults. */
enum logport_state {
	LOGPORT_UNASKED,	/* no `logport` on the command line */
	LOGPORT_NO_SOCKET,	/* asked for, and the socket layer refused */
	LOGPORT_NO_ADDRESS,	/* asked for, and this machine has no address */
	LOGPORT_LISTENING
};

/* The first device that has an address.
 *
 * There is no `netdev_primary` -- I reached for one and it does not exist.
 * `netdev_route` answers for a destination and this question has none, so it
 * is the table, walked. First-with-an-address rather than first: a machine
 * with two cards where only one has come up should report the one somebody
 * can reach. */
static struct net_device *addressed_device(void)
{
	unsigned i;

	for (i = 0; i < netdev_count(); i++) {
		struct net_device *d = netdev_at(i);

		if (d && d->up && d->ip != IPV4_ANY)
			return d;
	}

	return NULL;
}

static enum logport_state state = LOGPORT_UNASKED;
static struct socket *listener;
static u64 served;
static u64 bytes_sent;

/* The most of the log to carry to one reader.
 *
 * Read **in one call**, and the first version of this got that wrong in a way
 * worth keeping: it read the ring in 1 KB pieces, which cannot work, because
 * `klog_read` answers with *the most recent* `max` bytes rather than from a
 * cursor. Asking for a kilobyte repeatedly returns the same last kilobyte
 * every time, so the reader would have received the tail of the log over and
 * over and called it the whole thing -- a loop that runs, terminates, sends
 * plausible output and is entirely wrong.
 *
 * klog.h says so directly and I skimmed it. The allocation is bounded here
 * instead. */
#define LOGPORT_MAX (64 * 1024)

/* Sent to a reader before anything else when the log is longer than that.
 * Truncation that does not announce itself is the same fault as a wrapped ring
 * that does not. */
static const char capped[] =
	"--- longer than this port will carry: the oldest lines are omitted ---\n";

/* Sending, with the one property that matters on a socket nobody is obliged
 * to read: **a client that stops reading must not stop the kernel.**
 *
 * `socket_send` returns what it took. A full window takes nothing, and the
 * honest answer to that is to give up on this client rather than to spin --
 * a thread blocked on a laptop somebody closed the lid of is a thread that
 * never comes back.
 */
static bool send_all(struct socket *s, const char *p, u32 len)
{
	u32 sent = 0;
	unsigned stalls = 0;

	while (sent < len) {
		i64 n = socket_send(s, p + sent, len - sent);

		if (n < 0)
			return false;

		if (n == 0) {
			/* Bounded rather than infinite, and counted rather
			 * than slept on blindly: sixty-four turns of the
			 * scheduler is long enough for a window to open and
			 * short enough that a dead client is noticed. */
			if (++stalls > 64)
				return false;

			sched_yield();
			continue;
		}

		stalls = 0;
		sent += (u32)n;
		bytes_sent += (u64)n;
	}

	return true;
}

/* One client: everything the kernel has said, oldest first, then goodbye.
 *
 * **It does not stay and stream.** A connection that lives for the life of the
 * machine is a second place the log exists, needing its own buffering and its
 * own answer to a slow reader. Reconnecting costs the caller a line of shell
 * and costs this kernel nothing.
 */
static void serve(struct socket *c)
{
	char *buf;
	u32 held = klog_held();
	u32 got;
	bool truncated = false;

	if (!held) {
		socket_close(c);
		served++;
		return;
	}

	if (held > LOGPORT_MAX) {
		held = LOGPORT_MAX;
		truncated = true;
	}

	buf = kmalloc(held);

	if (!buf) {
		/* Said rather than dropped. A reader who connects and is given
		 * silence cannot tell that from a machine with nothing to say,
		 * and those are different facts. */
		static const char nomem[] =
			"--- no memory to gather the log ---\n";

		send_all(c, nomem, (u32)(sizeof(nomem) - 1));
		socket_close(c);
		return;
	}

	/* Both warnings first, because a reader who does not know the log is
	 * incomplete will read what survives as though it were the whole story
	 * -- which is klog.h's own warning about this exact function. */
	if (klog_wrapped()) {
		static const char lost[] =
			"--- the log ring wrapped: the oldest lines are gone ---\n";

		if (!send_all(c, lost, (u32)(sizeof(lost) - 1)))
			goto done;
	}

	if (truncated && !send_all(c, capped, (u32)(sizeof(capped) - 1)))
		goto done;

	/* One call, and the whole of what is being sent. See LOGPORT_MAX. */
	got = klog_read(buf, held);

	if (got)
		send_all(c, buf, got);

done:
	kfree(buf);
	socket_close(c);
	served++;
}

static void logport_thread(void *arg)
{
	(void)arg;

	for (;;) {
		struct socket *c = socket_accept(listener);

		if (c) {
			serve(c);
			continue;
		}

		/* **Yielding rather than sleeping, and that is a workaround for
		 * KF-258 rather than a preference.**
		 *
		 * The obvious shape here is `timer_sleep_ns(100 ms)` -- a
		 * tenth of a second is far longer than a connection can wait
		 * for, and it costs the machine nothing. It does not work:
		 * the first sleep in this thread returns and the second never
		 * does, because by then the machine has gone idle and a
		 * sleeping thread's deadline does not bring it back. That is a
		 * fault in the idle path, measured and recorded, not in this
		 * file.
		 *
		 * So this yields instead, which keeps a runnable thread in the
		 * round and the processor out of deep idle. **The cost is
		 * real**: a machine with this port open does not idle properly,
		 * which is KF-249's fault arriving by a second road. It is
		 * accepted here only because this is a diagnostic that is off
		 * unless somebody asks for it, and a debugging aid that keeps
		 * the machine awake while it is running is a trade a person can
		 * make knowingly.
		 *
		 * **When KF-258 is fixed this becomes a sleep again**, and the
		 * line above is how the next reader knows that is possible. */
		sched_yield();
	}
}

bool logport_start(void)
{
	struct net_device *dev;

	if (!boot_cmdline_has("logport")) {
		state = LOGPORT_UNASKED;
		return false;
	}

	/* An address is required, and its absence is reported rather than
	 * worked around. A listener on 0.0.0.0 would "succeed" on a machine
	 * with no network and print a port nobody can reach, which is the
	 * confident wrong answer this project keeps refusing to give. */
	dev = addressed_device();

	if (!dev) {
		state = LOGPORT_NO_ADDRESS;
		return false;
	}

	listener = socket_create(SOCK_STREAM);

	if (!listener ||
	    !socket_bind(listener, IPV4_ANY, LOGPORT_PORT) ||
	    !socket_listen(listener, 4)) {
		state = LOGPORT_NO_SOCKET;
		return false;
	}

	if (!thread_create("logport", logport_thread, NULL)) {
		state = LOGPORT_NO_SOCKET;
		return false;
	}

	state = LOGPORT_LISTENING;
	return true;
}

void logport_print_summary(void)
{
	struct net_device *dev;

	/* Nobody asked, so an ordinary boot says nothing at all. A line
	 * reading "log port: off" on every machine is a line people stop
	 * reading, and this is one that must be read when it appears. */
	if (state == LOGPORT_UNASKED)
		return;

	switch (state) {
	case LOGPORT_NO_ADDRESS:
		kputs("  log port     : asked for, and this machine has no "
		      "address, so it is not listening\n");
		return;

	case LOGPORT_NO_SOCKET:
		kputs("  log port     : asked for, and the socket could not be "
		      "opened, so it is not listening\n");
		return;

	default:
		break;
	}

	dev = addressed_device();

	if (!dev) {
		kputs("  log port     : listening, and the address it was given "
		      "has gone\n");
		return;
	}

	kprintf("  log port     : **listening** on %u.%u.%u.%u:%u -- the boot "
		"log is readable by anybody on this network\n",
		(dev->ip >> 24) & 0xFF, (dev->ip >> 16) & 0xFF,
		(dev->ip >> 8) & 0xFF, dev->ip & 0xFF,
		(unsigned)LOGPORT_PORT);

	kprintf("                 %llu reader(s) so far, %llu byte(s) sent; "
		"it reads nothing from them\n",
		(unsigned long long)served,
		(unsigned long long)bytes_sent);
}
