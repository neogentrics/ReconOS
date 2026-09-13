/* See backtrace.h, particularly the part about running while something has
 * already gone wrong. */
#include <recon/kernel/backtrace.h>
#include <recon/kernel/console.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/compiler.h>

/* How far apart two frames may be and still be believable.
 *
 * A thread's stack is a few pages, so consecutive frames are within that. A
 * pointer that passes every other check and is a megabyte away is not the next
 * frame -- it is a plausible number that happens to be in the kernel half, which
 * is most of the numbers in a kernel. */
#define FRAME_MAX_STEP (64u * 1024u)

/* The kernel's half. A chain that leaves it has left the stack, whatever it
 * says. Taken as a mask rather than a range because there is one bit that
 * separates the two halves on both architectures, and a range would be two
 * constants to keep in step with the linker script. */
#define KERNEL_HALF_BIT (1ULL << 63)

static bool frame_is_believable(u64 frame, u64 previous)
{
	if (!frame)
		return false;

	/* In the kernel's half. */
	if (!(frame & KERNEL_HALF_BIT))
		return false;

	/* Aligned. A frame pointer is never not, on either architecture, and an
	 * unaligned read of one is a fault on aarch64 rather than a wrong
	 * answer. */
	if (frame & 0xF)
		return false;

	if (previous) {
		/* STRICTLY GREATER, which is what makes a loop impossible
		 * rather than unlikely. Stacks grow downward, so walking out of
		 * one is monotonically upward -- and a cycle in the chain is
		 * the classic way a backtrace hangs a machine that was only
		 * dying slowly. */
		if (frame <= previous)
			return false;

		if (frame - previous > FRAME_MAX_STEP)
			return false;
	}

	/* And it has to be memory this processor can actually read. Every check
	 * above is arithmetic on a number; this is the one that asks the page
	 * tables, and it is what stops the read below faulting. */
	if (!vm_lookup((vaddr_t)frame))
		return false;

	/* Both words of the frame, not just the first. A frame pointer one word
	 * below the end of a mapped page passes a check on its own address and
	 * faults on the return address beside it. */
	if (!vm_lookup((vaddr_t)(frame + sizeof(u64))))
		return false;

	return true;
}

void backtrace_print(u64 frame)
{
	u64 previous = 0;
	unsigned depth;

	if (!frame)
		frame = arch_frame_pointer();

	kputs_unlocked("  call chain   : ");

	if (!frame_is_believable(frame, 0)) {
		/* Said rather than left blank. A report with no chain and no
		 * reason reads as "there was nothing to say", and the
		 * interesting case is exactly the opposite: the frame pointer
		 * itself being rubbish is often *why* the machine faulted. */
		kputs_unlocked("no usable frame pointer, so none\n");
		return;
	}

	kputs_unlocked("\n");

	for (depth = 0; depth < BACKTRACE_MAX_FRAMES; depth++) {
		u64 next = 0, ret = 0;

		if (!arch_frame_step(frame, &next, &ret))
			break;

		if (!ret) {
			kputs_unlocked("    (end of the chain)\n");
			return;
		}

		kprintf_unlocked("    %p\n", (void *)(uintptr_t)ret);

		if (!frame_is_believable(next, frame)) {
			/* Which of the two it is matters. A chain that ends
			 * because it ran out is complete; one that ends because
			 * the next frame was not believable is *truncated*, and
			 * a reader who cannot tell them apart will stop looking
			 * one frame early. */
			kputs_unlocked("    (chain stops here: the next frame "
				       "is not believable)\n");
			return;
		}

		previous = frame;
		frame = next;
		(void)previous;
	}

	kputs_unlocked("    (stopped at the frame limit; there may be more)\n");
}

/* --- the self-test --------------------------------------------------------
 *
 * Three nested calls, and the chain has to have at least three frames in it.
 * That is the only part that can be asserted without symbols -- and it is
 * enough to catch the failure that matters, which is a walk that produces one
 * frame and stops because the chain was never followed at all.
 *
 * The second assertion is the one this file is really about: a deliberately
 * rubbish frame pointer must produce *no output and no fault*. A backtrace that
 * faults while reporting a fault is worse than having no backtrace, and it is
 * not a hypothetical -- a wild frame pointer is frequently the reason the
 * machine is in the report in the first place.
 */
static unsigned count_frames(u64 frame)
{
	unsigned n = 0;
	u64 previous = 0;

	while (n < BACKTRACE_MAX_FRAMES && frame_is_believable(frame, previous)) {
		u64 next = 0, ret = 0;

		if (!arch_frame_step(frame, &next, &ret) || !ret)
			break;

		n++;
		previous = frame;
		frame = next;
	}

	return n;
}

/* Real calls, not inlined ones.
 *
 * At -O2 the compiler folded all three of these into their caller, and the walk
 * found two frames where the test wanted three -- a test failing because it had
 * not actually built the thing it was measuring. The subject here is the call
 * chain, so the calls have to survive.
 *
 * Worth keeping as a note rather than a fix: an optimiser is allowed to remove
 * the very structure a test is asserting about, and this is the second time on
 * this project it has done so. The first was a recovery label deleted as
 * unreachable, which cost an hour of reasoning about the wrong thing. */
static RK_NOINLINE unsigned deep_three(void)
{
	return count_frames(arch_frame_pointer());
}

static RK_NOINLINE unsigned deep_two(void)
{
	return deep_three();
}

static RK_NOINLINE unsigned deep_one(void)
{
	return deep_two();
}

bool backtrace_self_test(void)
{
	unsigned frames;
	bool ok = true;

	frames = deep_one();

	/* Three calls deep plus whatever is above the test, so three is a floor
	 * rather than a target. One would mean the chain was read once and
	 * never followed. */
	if (frames < 3) {
		kprintf("  backtrace: walked %u frame(s) from three nested "
			"calls\n", frames);
		ok = false;
	}

	/* Rubbish, and it must come back with nothing rather than a fault. Each
	 * of these fails a different check, so a validation that lost one of
	 * them would still be caught: not in the kernel half, unaligned,
	 * plausible but unmapped, and zero. */
	{
		static const u64 nonsense[] = {
			0x0000000000401000ULL,	/* a user address */
			0xFFFFFFFF80100001ULL,	/* unaligned */
			0xFFFF9F0000000000ULL,	/* kernel half, nothing there */
			0ULL,
		};
		unsigned i;

		for (i = 0; i < sizeof(nonsense) / sizeof(nonsense[0]); i++)
			if (frame_is_believable(nonsense[i], 0)) {
				kprintf("  backtrace: %p was accepted as a "
					"frame pointer\n",
					(void *)(uintptr_t)nonsense[i]);
				ok = false;
			}
	}

	/* And a chain that points at itself terminates. Stacks grow downward so
	 * a frame may only be followed upward, and this is the check that turns
	 * a cycle from a hang into a stop. */
	{
		u64 here = arch_frame_pointer();

		if (frame_is_believable(here, here)) {
			kputs("  backtrace: a frame pointing at itself was "
			      "followed\n");
			ok = false;
		}
	}

	return ok;
}
