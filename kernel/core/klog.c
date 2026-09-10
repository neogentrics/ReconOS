/* See klog.h, particularly the part about why this takes no lock. */
#include <recon/kernel/klog.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>

static char ring[KLOG_BYTES];

/* Where the next character goes, and how many have ever been written.
 *
 * `written` rather than a length, because it answers both questions at once: the
 * ring holds min(written, KLOG_BYTES) bytes, and it has wrapped if written is
 * larger. Two numbers that can disagree with each other would be a third thing
 * to keep right on a path that deliberately has no lock.
 *
 * Volatile because they are read by a reader that is not the writer, and written
 * by whichever processor happens to be printing. */
static volatile u32 next;
static volatile u64 written;

void klog_putc(char c)
{
	/* Read once, advanced once. Two processors printing together can land
	 * on the same index and one character is lost -- which is the same
	 * outcome those two processors already have on the console itself, and
	 * is the price of not taking a lock on the path a dying machine uses.
	 *
	 * What must not happen is an index that leaves the array, and that is
	 * why the modulo is applied to the value being stored rather than
	 * trusted to stay in range across the read-modify-write. */
	u32 at = next % KLOG_BYTES;

	ring[at] = c;
	next = (at + 1) % KLOG_BYTES;
	written++;
}

u32 klog_held(void)
{
	u64 w = written;

	return (w > KLOG_BYTES) ? KLOG_BYTES : (u32)w;
}

bool klog_wrapped(void)
{
	return written > KLOG_BYTES;
}

u32 klog_read(void *out, u32 max)
{
	u32 held = klog_held();
	u32 start;
	u32 i;
	char *dst = out;

	if (!out || !max)
		return 0;

	if (held > max)
		held = max;

	/* Oldest first. When the ring has wrapped the oldest byte is the one
	 * about to be overwritten, which is wherever `next` points; when it has
	 * not, the oldest is index zero.
	 *
	 * The two cases are genuinely different and collapsing them is the
	 * classic ring-buffer bug: a log that has not yet wrapped, read as
	 * though it had, comes back as a page of zeroes followed by the real
	 * output. */
	if (klog_wrapped())
		start = (next + (KLOG_BYTES - held)) % KLOG_BYTES;
	else
		start = klog_held() - held;

	for (i = 0; i < held; i++)
		dst[i] = ring[(start + i) % KLOG_BYTES];

	return held;
}

void klog_print_summary(void)
{
	kprintf("\nKernel log\n");
	kprintf("  held         : %u of %u bytes%s\n", klog_held(),
		(unsigned)KLOG_BYTES,
		klog_wrapped() ? ", and it has wrapped -- the beginning is gone"
			       : "");
}

/* --- the self-test --------------------------------------------------------
 *
 * The ring is being written by the very act of testing it, which is unusual and
 * is the whole difficulty: every kputs in here adds to the thing under test. So
 * nothing asserts an exact length, and what is checked instead is the shape.
 *
 * The case that matters is the unwrapped one. A ring read as though it had
 * always wrapped returns zeroes followed by the real output -- plausible-looking
 * and wrong -- and on a machine that boots quietly enough never to wrap, that is
 * every read anybody ever does.
 */
bool klog_self_test(void)
{
	static const char marker[] = "klog-marker-9f3a";
	char back[512];
	u32 got;
	u32 i;
	bool found = false;
	bool ok = true;

	u32 before = klog_held();

	kputs(marker);

	if (klog_held() < before) {
		kputs("  klog: writing to the log made it shorter\n");
		ok = false;
	}

	got = klog_read(back, sizeof(back));

	if (got == 0) {
		kputs("  klog: nothing has been recorded at all\n");
		return false;
	}

	/* The marker has to be in the tail. Searched rather than positioned,
	 * because anything else printing between the write and the read would
	 * move it -- and on a machine with four processors, something will. */
	for (i = 0; i + sizeof(marker) - 1 <= got; i++)
		if (kmemcmp(back + i, marker, sizeof(marker) - 1) == 0) {
			found = true;
			break;
		}

	if (!found) {
		kputs("  klog: what was just printed is not in the log\n");
		ok = false;
	}

	/* Oldest first, which is the direction that makes it readable. The very
	 * first thing this kernel prints is its own name, so on an unwrapped
	 * log the front of what comes back is the front of the boot -- and a
	 * ring read from the wrong end would put the newest there. */
	if (!klog_wrapped() && got > 8) {
		if (back[0] == '\0') {
			kputs("  klog: an unwrapped log came back starting "
			      "with zeroes, so it was read as though it had "
			      "wrapped\n");
			ok = false;
		}
	}

	/* Asking for less than is held gives back exactly what was asked for,
	 * and not silently more. A caller with a small buffer is the one most
	 * likely to be a program. */
	{
		char small[16];
		u32 n = klog_read(small, sizeof(small));

		if (n > sizeof(small)) {
			kprintf("  klog: asked for %u bytes and got %u\n",
				(unsigned)sizeof(small), n);
			ok = false;
		}
	}

	/* And a caller that asks for nothing gets nothing rather than a fault.
	 */
	if (klog_read(back, 0) != 0) {
		kputs("  klog: a zero-length read returned something\n");
		ok = false;
	}

	return ok;
}
