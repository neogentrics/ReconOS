/* See input.h, particularly the part about why an event is a key and not a
 * character.
 *
 * --- The anomalies this accounts for -------------------------------------
 *
 *  1. A release for a key nobody saw pressed. A no-op, never a decrement --
 *     the whole reason held-state is a bit per key rather than a counter.
 *  2. A press for a key already held. Reported as a repeat, because that is
 *     what the keyboard's typematic circuit is doing.
 *  3. A keycode past the end of the table. Delivered as an event, but not
 *     recorded as held: an out-of-range write into the bitmap is a fault, and
 *     a keycode is a number that came off a wire.
 *  4. The queue filling. The oldest goes and the loss is counted. Refusing
 *     new events would lose the most recent thing the person did.
 *  5. A reader asking for less than one event. Refused rather than served
 *     half of one, which the reader could not identify as half.
 *  6. A reader asking for several. Served as many whole events as fit.
 *  7. A read with nothing to read. Returns zero rather than waiting, and
 *     says why in the header -- waiting needs the reader to be a thread that
 *     can block, which is a change to the file and not to this queue.
 *  8. Two processors posting at once. Under the lock, which is taken with
 *     interrupts off because the worker that posts can be preempted by the
 *     handler that feeds it.
 *  9. A driver posting from interrupt context. Not forbidden here -- it
 *     cannot be, this file cannot tell -- but the lock is interrupt-safe so
 *     that a driver which does it anyway does not deadlock.
 * 10. An event with a reserved field somebody set. Zeroed on the way in, so
 *     that a later version giving it a meaning cannot read somebody's stack
 *     rubbish as a value.
 */
#include <recon/kernel/input.h>
#include <recon/kernel/vfs.h>
#include <recon/kernel/user.h>	/* the SYS_ error numbers */
#include <recon/kernel/lock.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/time.h>

static struct input_event queue[INPUT_QUEUE];
static unsigned head;			/* where the next event goes */
static unsigned count;			/* how many are held */
static struct spinlock input_lock = SPINLOCK_INIT("input");

/* One bit per keycode. A bitmap and not a counter, for the reason in the
 * header: a release with no press must be a no-op, and a counter would go
 * negative and leave the machine believing shift is held for ever. */
static u8 held[(KEY_MAX + 1 + 7) / 8];

static u64 posted, dropped, delivered, out_of_range;

static bool held_test(u16 code)
{
	if (code > KEY_MAX)
		return false;

	return (held[code / 8] >> (code % 8)) & 1u;
}

static void held_set(u16 code, bool down)
{
	if (code > KEY_MAX) {
		out_of_range++;		/* anomaly 3 */
		return;
	}

	if (down)
		held[code / 8] |= (u8)(1u << (code % 8));
	else
		held[code / 8] &= (u8)~(1u << (code % 8));
}

void input_post(u16 code, enum input_kind kind)
{
	u64 flags = spin_lock_irq(&input_lock);
	struct input_event *e;

	/* A press for a key already down is the keyboard repeating, whatever
	 * the driver called it. Decided here rather than in each driver, so
	 * that two drivers cannot disagree about what a repeat is. Anomaly 2.
	 */
	if (kind == INPUT_PRESS && held_test(code))
		kind = INPUT_REPEAT;

	if (kind == INPUT_PRESS)
		held_set(code, true);
	else if (kind == INPUT_RELEASE)
		held_set(code, false);
	/* A repeat changes nothing: the key was already down. */

	if (count == INPUT_QUEUE) {
		/* Anomaly 4. The oldest goes. */
		unsigned oldest = (head + INPUT_QUEUE - count) % INPUT_QUEUE;

		(void)oldest;
		count--;
		dropped++;
	}

	e = &queue[head];
	e->when = time_monotonic_ns();
	e->code = code;
	e->kind = (u8)kind;
	e->reserved = 0;		/* anomaly 10 */

	head = (head + 1) % INPUT_QUEUE;
	count++;
	posted++;

	spin_unlock_irq(&input_lock, flags);
}

bool input_take(struct input_event *out)
{
	u64 flags;
	bool got = false;

	if (!out)
		return false;

	flags = spin_lock_irq(&input_lock);

	if (count) {
		unsigned oldest = (head + INPUT_QUEUE - count) % INPUT_QUEUE;

		*out = queue[oldest];
		count--;
		delivered++;
		got = true;
	}

	spin_unlock_irq(&input_lock, flags);
	return got;
}

bool input_key_held(u16 code)
{
	u64 flags = spin_lock_irq(&input_lock);
	bool down = held_test(code);

	spin_unlock_irq(&input_lock, flags);
	return down;
}

/* --- the file ------------------------------------------------------------ */

static i64 input_read(struct file *f, void *out, u64 len)
{
	u8 *dst = out;
	u64 done = 0;

	if (!out)
		return SYS_EFAULT;

	/* Anomaly 5. Half an event is not a short read, it is a reader that
	 * cannot tell which half it got. */
	if (len < sizeof(struct input_event))
		return SYS_EINVAL;

	while (len - done >= sizeof(struct input_event)) {
		struct input_event e;

		if (!input_take(&e))
			break;

		kmemcpy(dst + done, &e, sizeof(e));
		done += sizeof(e);
	}

	/* Anomaly 7: nothing to read is zero, not a wait and not an error. */
	return (i64)done;
}

const struct file_ops input_file_ops = {
	.read  = input_read,
	.write = 0,		/* nothing to say to a keyboard yet. The lights
				 * are a command to the controller, which is the
				 * driver's business and not a byte stream. */
	.seek  = 0,		/* no position: the queue is what it is */
	.close = 0,
	.name  = "input",
};

struct file *input_open(unsigned flags, i64 *error)
{
	struct file *f = file_new_external(&input_file_ops, flags, NULL);

	if (!f && error)
		*error = SYS_ENOMEM;

	return f;
}

void input_init(void)
{
	head = count = 0;
	kmemset(held, 0, sizeof(held));
	arch_input_probe();
}

void input_print_summary(void)
{
	kprintf("\nInput\n");

	arch_input_print();

	kprintf("  events       : %llu posted, %llu read, %llu waiting\n",
		(unsigned long long)posted, (unsigned long long)delivered,
		(unsigned long long)count);

	/* Printed only when it happened, and never hidden. "Input feels like it
	 * drops keys" is otherwise a complaint nobody can check. */
	if (dropped)
		kprintf("  lost         : %llu, because nothing was reading\n",
			(unsigned long long)dropped);

	if (out_of_range)
		kprintf("  out of range : %llu keycode(s) past %u\n",
			(unsigned long long)out_of_range, (unsigned)KEY_MAX);
}

/* --- the self-test --------------------------------------------------------
 *
 * No hardware is touched. Every machine this boots on has a different answer to
 * "is there a keyboard", and a test that needed one would report a failure on a
 * machine that simply has none -- so this drives the queue directly, which is
 * where the decisions worth checking live.
 *
 * The two that matter are the ones with a plausible wrong answer:
 *
 *   - **a release for a key nobody pressed must not leave it held.** With a
 *     counter instead of a bitmap this underflows, and the machine believes
 *     shift is down for the rest of the boot;
 *   - **the queue must drop the oldest, not refuse the newest.** Both keep the
 *     kernel running and both look fine in a summary; only one of them still
 *     lets you type.
 */
bool input_self_test(void)
{
	struct input_event e;
	bool ok = true;
	unsigned i;
	u64 dropped_before;

	/* Start from a known queue rather than assuming an empty one: the
	 * keyboard may have delivered something between init and here, and a
	 * test that assumed otherwise would fail on a machine where somebody
	 * pressed a key during boot. */
	while (input_take(&e))
		;

	/* --- a press is a press, and it is held ------------------------- */
	input_post(KEY_A, INPUT_PRESS);

	if (!input_key_held(KEY_A)) {
		kputs("  input: a key that was pressed is not held\n");
		ok = false;
	}

	if (!input_take(&e)) {
		kputs("  input: a posted event did not come back\n");
		return false;
	}

	if (e.code != KEY_A || e.kind != INPUT_PRESS) {
		kprintf("  input: got code %u kind %u, not %u press\n",
			e.code, e.kind, (unsigned)KEY_A);
		ok = false;
	}

	if (e.reserved) {
		kputs("  input: the reserved field came back non-zero\n");
		ok = false;
	}

	/* --- a second press while held is a repeat ---------------------- */
	input_post(KEY_A, INPUT_PRESS);

	if (input_take(&e) && e.kind != INPUT_REPEAT) {
		kputs("  input: pressing a key that is already down was "
		      "reported as a fresh press, so a game cannot tell a "
		      "repeat from a jump\n");
		ok = false;
	}

	/* --- release, and it is no longer held -------------------------- */
	input_post(KEY_A, INPUT_RELEASE);
	(void)input_take(&e);

	if (input_key_held(KEY_A)) {
		kputs("  input: a released key is still held\n");
		ok = false;
	}

	/* --- the one that a counter gets wrong -------------------------- */
	input_post(KEY_LEFTSHIFT, INPUT_RELEASE);
	(void)input_take(&e);

	if (input_key_held(KEY_LEFTSHIFT)) {
		kputs("  input: releasing a key that was never pressed left it "
		      "held, so every modifier is stuck from the first dropped "
		      "event onward\n");
		ok = false;
	}

	/* --- the queue drops the oldest --------------------------------- */
	dropped_before = dropped;

	for (i = 0; i < INPUT_QUEUE + 10; i++)
		input_post((u16)(KEY_1 + (i % 9)), INPUT_PRESS);

	if (dropped == dropped_before) {
		kputs("  input: overfilling the queue dropped nothing, so the "
		      "queue is not the size it says\n");
		ok = false;
	}

	if (!input_take(&e)) {
		kputs("  input: the queue is empty after being overfilled\n");
		ok = false;
	} else if (e.code == KEY_1) {
		/* The very first event posted was KEY_1. If it is still at the
		 * front after ten more than the queue holds, the ring refused
		 * the new ones instead of dropping the old. */
		kputs("  input: after overflowing, the oldest event is still "
		      "at the front -- the queue is refusing new events rather "
		      "than dropping old ones, which loses whatever was just "
		      "typed\n");
		ok = false;
	}

	while (input_take(&e))
		;

	/* --- a keycode off the end is delivered, not written into ------- */
	{
		u64 before = out_of_range;

		input_post(KEY_MAX + 100, INPUT_PRESS);

		if (out_of_range == before) {
			kputs("  input: a keycode past the end was recorded as "
			      "held, which is a write past the bitmap\n");
			ok = false;
		}

		if (!input_take(&e) || e.code != KEY_MAX + 100) {
			kputs("  input: a keycode past the end was swallowed "
			      "rather than delivered\n");
			ok = false;
		}
	}

	/* --- the file refuses half an event ----------------------------- */
	{
		i64 err = SYS_OK;
		struct file *f = input_open(0, &err);
		u8 small[4];

		if (!f) {
			kputs("  input: could not open the device\n");
			return false;
		}

		if (input_read(f, small, sizeof(small)) != SYS_EINVAL) {
			kputs("  input: a read too small for one event was "
			      "not refused\n");
			ok = false;
		}

		/* And an empty queue reads zero rather than blocking or
		 * failing. */
		{
			struct input_event one;

			if (input_read(f, &one, sizeof(one)) != 0) {
				kputs("  input: reading an empty queue did not "
				      "return zero\n");
				ok = false;
			}
		}

		file_release(f);
	}

	return ok;
}
