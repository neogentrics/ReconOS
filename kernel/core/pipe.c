/* A pipe: bytes from one program to another, and the first thing in this kernel
 * that two programs can hold between them.
 *
 * The architecture audit's IPC row has said "waits on: descriptors" since it was
 * written. This is what that was waiting for, and it is deliberately the second
 * thing built on `struct file` rather than the first: a pipe is the case that
 * proves the interface, because it has no path, no size, no position, and two
 * ends that are not the same kind of thing.
 *
 * --- What a pipe is, and what makes it hard ---
 *
 * A ring of bytes, a lock, and two queues of threads. The mechanism is small.
 * What is not small is the four questions at its edges, each of which has a
 * wrong answer that looks like it works:
 *
 *   - A reader with nothing to read **waits**, and does not return zero. Zero
 *     means the input has ended, and a reader told that while a writer is still
 *     running stops early and reports success.
 *   - A reader waiting when the last writer closes gets zero, because now the
 *     input really has ended. That is why the writers are counted rather than
 *     inferred.
 *   - A writer with nowhere to put bytes waits, rather than discarding them or
 *     failing. A pipe that drops bytes under pressure is a pipe that works in
 *     every test and loses data on a busy machine.
 *   - A writer whose readers have all gone gets an error and not a wait, because
 *     nothing will ever drain it. Waiting there is a program that stops forever
 *     for a reason nothing reports.
 *
 * Each of those is one comparison, and each is asserted below against a case
 * built to fail without it.
 */
#include <recon/kernel/vfs.h>
#include <recon/kernel/wait.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/sched.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/time.h>
#include <recon/kernel/user.h>

/* One page. Big enough that ordinary use never blocks, small enough that the
 * blocking paths are reachable by a test that wants them -- which matters more:
 * a buffer nothing can fill is a buffer whose full case is never exercised. */
#define PIPE_CAPACITY 4096

struct pipe {
	struct spinlock lock;
	struct wait_queue readers;	/* waiting for bytes */
	struct wait_queue writers;	/* waiting for room */

	u8 buf[PIPE_CAPACITY];
	u32 head;			/* next byte to read */
	u32 tail;			/* next place to write */
	u32 count;

	/* How many open files of each kind point here. Counted, not inferred
	 * from the reference count on either file: the whole question a reader
	 * asks at the end of input is "is there still a writer", and the
	 * reference count answers "is this end still open", which is a
	 * different question with the same shape. */
	unsigned read_ends;
	unsigned write_ends;

	u64 passed;			/* bytes, for the summary */
};

static u64 pipes_made;

/* The caller holds the lock. */
static void ring_put(struct pipe *p, u8 byte)
{
	p->buf[p->tail] = byte;
	p->tail = (p->tail + 1) % PIPE_CAPACITY;
	p->count++;
}

static u8 ring_take(struct pipe *p)
{
	u8 byte = p->buf[p->head];

	p->head = (p->head + 1) % PIPE_CAPACITY;
	p->count--;
	return byte;
}

/* Frees the pipe once neither end is open. Called with the lock held; returns
 * true if it freed, in which case the caller must not touch it again. */
static bool pipe_maybe_free(struct pipe *p, u64 flags)
{
	if (p->read_ends || p->write_ends) {
		spin_unlock_irq(&p->lock, flags);
		return false;
	}

	spin_unlock_irq(&p->lock, flags);
	kfree(p);
	return true;
}

static i64 pipe_read(struct file *f, void *out, u64 len)
{
	struct pipe *p = f->private;
	u8 *dst = out;
	u64 got = 0;
	u64 flags;

	if (len == 0)
		return 0;

	flags = spin_lock_irq(&p->lock);

	/* Waits for the first byte, and only the first.
	 *
	 * A read that waited for `len` bytes would be a read that blocks until
	 * a writer happens to send exactly as much as this reader happened to
	 * ask for -- which is not what a pipe promises and is a deadlock
	 * between two correct programs. Returning what is there is the
	 * contract, and the caller loops if it wants more. */
	while (p->count == 0) {
		if (p->write_ends == 0) {
			/* The end of input, and now it really is one. */
			spin_unlock_irq(&p->lock, flags);
			return 0;
		}

		if (!wait_sleep(&p->readers, &p->lock, flags)) {
			/* Cannot block here -- the idle thread, or no thread
			 * at all. Reporting that is better than spinning a
			 * processor on an empty pipe. */
			spin_unlock_irq(&p->lock, flags);
			return SYS_EAGAIN;
		}
	}

	while (got < len && p->count)
		dst[got++] = ring_take(p);

	p->passed += got;

	/* Room has appeared, so anybody waiting for some can look again. Under
	 * the lock they will take, which is what makes the wake impossible to
	 * lose. */
	wait_wake_all(&p->writers);

	spin_unlock_irq(&p->lock, flags);
	return (i64)got;
}

static i64 pipe_write(struct file *f, const void *in, u64 len)
{
	struct pipe *p = f->private;
	const u8 *src = in;
	u64 done = 0;
	u64 flags;

	flags = spin_lock_irq(&p->lock);

	while (done < len) {
		while (p->count == PIPE_CAPACITY) {
			/* Nobody left to drain it. An error, not a wait: a
			 * program waiting for a reader that has gone waits
			 * forever, and nothing reports why. */
			if (p->read_ends == 0) {
				spin_unlock_irq(&p->lock, flags);
				return done ? (i64)done : SYS_EPIPE;
			}

			if (!wait_sleep(&p->writers, &p->lock, flags)) {
				spin_unlock_irq(&p->lock, flags);
				return done ? (i64)done : SYS_EAGAIN;
			}
		}

		/* Checked again after every wait, and not only when full: the
		 * reader may have gone while this thread was asleep. */
		if (p->read_ends == 0) {
			spin_unlock_irq(&p->lock, flags);
			return done ? (i64)done : SYS_EPIPE;
		}

		while (done < len && p->count < PIPE_CAPACITY)
			ring_put(p, src[done++]);

		wait_wake_all(&p->readers);
	}

	spin_unlock_irq(&p->lock, flags);
	return (i64)done;
}

static i64 pipe_close_read(struct file *f)
{
	struct pipe *p = f->private;
	u64 flags = spin_lock_irq(&p->lock);

	if (p->read_ends)
		p->read_ends--;

	/* Woken before the lock goes, because a writer asleep waiting for room
	 * will never be woken by room appearing -- there is nobody left to make
	 * any. It wakes to find no readers and returns an error, which is the
	 * answer. */
	wait_wake_all(&p->writers);

	pipe_maybe_free(p, flags);
	return SYS_OK;
}

static i64 pipe_close_write(struct file *f)
{
	struct pipe *p = f->private;
	u64 flags = spin_lock_irq(&p->lock);

	if (p->write_ends)
		p->write_ends--;

	/* And the mirror: a reader asleep waiting for bytes must be told the
	 * input has ended, or it waits for a writer that no longer exists. */
	wait_wake_all(&p->readers);

	pipe_maybe_free(p, flags);
	return SYS_OK;
}

/* Two tables, not one with a flag.
 *
 * A read end has no write function *at all*, so "you cannot write to the read
 * end of a pipe" is answered by the same line in sys_write that answers "you
 * cannot read the console" -- once, above, rather than by a check inside every
 * implementation that might get it wrong differently. */
static const struct file_ops pipe_read_ops = {
	.read  = pipe_read,
	.close = pipe_close_read,
	.name  = "pipe-read",
};

static const struct file_ops pipe_write_ops = {
	.write = pipe_write,
	.close = pipe_close_write,
	.name  = "pipe-write",
};

/* Makes both ends. Either both come back or neither does. */
bool pipe_create(struct file **read_end, struct file **write_end)
{
	struct pipe *p = kzalloc(sizeof(*p));

	if (!p)
		return false;

	spin_init(&p->lock, "pipe");
	p->read_ends = 1;
	p->write_ends = 1;

	*read_end = file_new_external(&pipe_read_ops, OPEN_READ, p);
	*write_end = file_new_external(&pipe_write_ops, OPEN_WRITE, p);

	if (!*read_end || !*write_end) {
		/* One end without the other is a pipe nothing can use and
		 * memory nothing will free. */
		if (*read_end)
			kfree(*read_end);
		if (*write_end)
			kfree(*write_end);

		kfree(p);
		*read_end = NULL;
		*write_end = NULL;
		return false;
	}

	pipes_made++;
	return true;
}

void pipe_print_summary(void)
{
	kprintf("  pipes        : %lu made, %u bytes of room each\n",
		pipes_made, (unsigned)PIPE_CAPACITY);
}

/* --- the self-test --------------------------------------------------------
 *
 * The mechanism -- bytes in, bytes out -- is the easy part and would pass with
 * every one of the four edge cases wrong. So most of this is about the edges,
 * and each case is built so that the wrong answer is visible rather than merely
 * possible.
 *
 * The important one is that a reader with an empty pipe **waits**. A reader that
 * returned zero instead would still pass a test that wrote first and read
 * afterwards, because there would be bytes waiting either way. So the reader is
 * started first and made to wait, and the assertion is that it got the whole
 * message rather than the end of input.
 */
#define PIPE_TEST_BYTES 9000	/* more than the pipe holds, so the writer waits */

static struct file *test_read_end;
static struct file *test_write_end;
static volatile bool reader_done;
static volatile bool reader_saw_eof;
static volatile u32 reader_got;
static volatile u32 reader_wrong;
static volatile i64 writer_result;

static void pipe_reader(void *arg)
{
	u8 buf[64];
	u32 next = 0;

	for (;;) {
		i64 n = test_read_end->ops->read(test_read_end, buf,
						 sizeof(buf));

		if (n == 0) {
			reader_saw_eof = true;
			break;
		}

		if (n < 0)
			break;

		/* Every byte checked against what it should be, in order.
		 * "The right number of bytes arrived" would pass on a ring
		 * that wrapped wrongly and handed back the same page twice. */
		for (i64 i = 0; i < n; i++) {
			if (buf[i] != (u8)(next & 0xFF))
				reader_wrong++;
			next++;
		}

		reader_got = next;
	}

	reader_done = true;
}

bool pipe_self_test(void)
{
	u8 *out;
	u32 i;
	u64 deadline;
	bool ok = true;

	/* --- the ordinary case, with both waits exercised --- */

	if (!pipe_create(&test_read_end, &test_write_end)) {
		kputs("  pipe: could not make one\n");
		return false;
	}

	/* Neither end can do the other's job, and that is expressed by the
	 * function being absent rather than by a check inside it. */
	if (test_read_end->ops->write || test_write_end->ops->read) {
		kputs("  pipe: an end of a pipe can do both jobs\n");
		ok = false;
	}

	out = kmalloc(PIPE_TEST_BYTES);
	if (!out) {
		file_release(test_read_end);
		file_release(test_write_end);
		kputs("  pipe: no memory for the test\n");
		return false;
	}

	for (i = 0; i < PIPE_TEST_BYTES; i++)
		out[i] = (u8)(i & 0xFF);

	reader_done = false;
	reader_saw_eof = false;
	reader_got = 0;
	reader_wrong = 0;

	/* The reader starts first and finds nothing. If a read on an empty pipe
	 * returned zero rather than waiting, it would decide the input had
	 * ended before a single byte was written -- and every assertion below
	 * would report the wrong thing rather than nothing. */
	if (!thread_create("pipe-reader", pipe_reader, 0)) {
		kfree(out);
		file_release(test_read_end);
		file_release(test_write_end);
		kputs("  pipe: could not make a reader\n");
		return false;
	}

	/* Long enough that the reader has certainly run and found the pipe
	 * empty. Without this the reader might not start until after the write,
	 * and the waiting path would go untested while the test still passed. */
	{
		u64 until = time_monotonic_ns() + 50000000ULL;	/* 50 ms */

		while (time_monotonic_ns() < until)
			sched_yield();
	}

	if (reader_done) {
		kputs("  pipe: the reader finished before anything was "
		      "written, so an empty pipe reported the end of input\n");
		ok = false;
	}

	/* More than the pipe holds, so the writer has to wait for the reader at
	 * least twice. A pipe that dropped what would not fit would return the
	 * full count here and lose the middle. */
	writer_result = test_write_end->ops->write(test_write_end, out,
						   PIPE_TEST_BYTES);

	if (writer_result != PIPE_TEST_BYTES) {
		kprintf("  pipe: wrote %ld of %u bytes\n", (long)writer_result,
			(unsigned)PIPE_TEST_BYTES);
		ok = false;
	}

	/* Closing the write end is what ends the reader. Until it happens the
	 * reader is waiting, which is the correct behaviour and is why this is
	 * the only way to finish the test. */
	file_release(test_write_end);
	test_write_end = NULL;

	deadline = time_monotonic_ns() + 5000000000ULL;
	while (!reader_done && time_monotonic_ns() < deadline)
		sched_yield();

	if (!reader_done) {
		kputs("  pipe: the reader never finished after the write end "
		      "closed\n");
		kfree(out);
		return false;
	}

	if (!reader_saw_eof) {
		kputs("  pipe: the reader stopped without seeing the end of "
		      "input\n");
		ok = false;
	}

	if (reader_got != PIPE_TEST_BYTES) {
		kprintf("  pipe: the reader got %u bytes of %u\n",
			reader_got, (unsigned)PIPE_TEST_BYTES);
		ok = false;
	}

	if (reader_wrong) {
		kprintf("  pipe: %u bytes arrived with the wrong value\n",
			reader_wrong);
		ok = false;
	}

	file_release(test_read_end);
	test_read_end = NULL;

	/* --- a writer with no readers left --- */

	if (!pipe_create(&test_read_end, &test_write_end)) {
		kfree(out);
		kputs("  pipe: could not make a second one\n");
		return false;
	}

	file_release(test_read_end);
	test_read_end = NULL;

	/* Nothing will ever drain this. An error, not a wait: a program that
	 * waits for a reader that has gone waits for ever, and nothing reports
	 * why. */
	if (test_write_end->ops->write(test_write_end, out, 8) != SYS_EPIPE) {
		kputs("  pipe: writing with no reader left did not report a "
		      "broken pipe\n");
		ok = false;
	}

	file_release(test_write_end);
	test_write_end = NULL;

	/* --- a reader with no writers left --- */

	if (!pipe_create(&test_read_end, &test_write_end)) {
		kfree(out);
		kputs("  pipe: could not make a third one\n");
		return false;
	}

	if (test_write_end->ops->write(test_write_end, out, 4) != 4) {
		kputs("  pipe: a short write into an empty pipe failed\n");
		ok = false;
	}

	file_release(test_write_end);
	test_write_end = NULL;

	/* What was already written is still there. A pipe that discarded its
	 * contents when the writer closed would lose the last thing said, which
	 * is usually the thing that mattered. */
	{
		u8 back[8];
		i64 n = test_read_end->ops->read(test_read_end, back,
						 sizeof(back));

		if (n != 4) {
			kprintf("  pipe: after the writer closed, %ld of 4 "
				"bytes were still there\n", (long)n);
			ok = false;
		}

		/* And *now* it is the end of input. */
		if (test_read_end->ops->read(test_read_end, back,
					     sizeof(back)) != 0) {
			kputs("  pipe: an empty pipe with no writers did not "
			      "report the end of input\n");
			ok = false;
		}
	}

	file_release(test_read_end);
	test_read_end = NULL;

	kfree(out);
	return ok;
}
