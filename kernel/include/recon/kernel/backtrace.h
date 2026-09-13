/* Where the code was, and how it got there.
 *
 * A fault report names one address. That is enough to find the instruction and
 * almost never enough to find the *reason*, which is usually three frames up in
 * whoever passed the bad pointer down. The audit's diagnostics row names this
 * as the second of its two gaps, and both of them are here now.
 *
 * --- Why it is possible at all ---
 *
 * The build already passes `-fno-omit-frame-pointer`. Every function therefore
 * begins by saving the caller's frame pointer and its own return address in a
 * known place, and the chain can be walked. Without that flag this would need
 * unwind tables and a parser for them, which is a different and much larger
 * piece of work -- so the flag being there is the reason this is small, and
 * removing it would silently turn every backtrace into one frame.
 *
 * --- The danger, which is the whole design ---
 *
 * **This runs while something has already gone wrong.** The frame pointer it is
 * handed may be rubbish -- that may be *why* the machine faulted -- and
 * dereferencing rubbish inside a fault report produces a fault while reporting a
 * fault, which on x86_64 is a double fault and on a bad day a silent reset. That
 * is the one outcome worse than the original fault, and this kernel has
 * interrupt-stack machinery that exists specifically because of it.
 *
 * So every frame is validated before it is read, and the rules are deliberately
 * strict rather than clever:
 *
 *   - it must be in the kernel's half, because a chain that wanders into user
 *     addresses is a chain that has already left the stack;
 *   - it must be aligned, because a frame pointer never is not;
 *   - it must be *strictly greater* than the previous one, which is what makes
 *     a loop impossible rather than merely unlikely -- stacks grow downward, so
 *     walking up is monotonic, and a cycle in the chain is the classic way a
 *     backtrace hangs a dying machine;
 *   - it must be within one stack's distance of the previous one, because a
 *     plausible-looking pointer a gigabyte away is not the next frame;
 *   - and the walk stops after a fixed number of frames whatever happens.
 *
 * A frame that fails any of those ends the walk and says so. **A short backtrace
 * that stops with a reason is worth more than a long one that might be
 * invented**, and far more than a machine that stopped while printing it.
 *
 * --- What it cannot do ---
 *
 * There are no symbols. The kernel has no symbol table in memory and putting one
 * there is a decision about image size that nothing has asked for yet -- so this
 * prints addresses, and `addr2line` turns them into names afterwards. The report
 * says so rather than leaving somebody to wonder.
 */
#ifndef RECON_KERNEL_BACKTRACE_H
#define RECON_KERNEL_BACKTRACE_H

#include <recon/kernel/types.h>

/* Never more than this many frames, whatever the chain says. A cap that is a
 * visible number rather than a trust in the validation above it: the checks are
 * what should stop a bad walk, and this is what stops it if they do not. */
#define BACKTRACE_MAX_FRAMES 16

/* Prints the call chain leading to `frame`, one address per line.
 *
 * `frame` is the frame pointer to start from -- rbp on x86_64, x29 on aarch64 --
 * and zero means "wherever this was called from", which is what a panic wants.
 *
 * Prints why it stopped, always. A backtrace that ends without saying whether it
 * ran out of frames, hit the cap, or found something it would not follow is a
 * backtrace whose length means nothing. */
void backtrace_print(u64 frame);

/* The caller's own frame pointer, for `backtrace_print(0)` to start from. */
u64 arch_frame_pointer(void);

/* Reads one frame: the next frame pointer and the return address stored with
 * it. False if `frame` is not somewhere this may read.
 *
 * Separate from the walk because the *validation* is architecture-independent
 * and the *layout* is not -- x86_64 and aarch64 both put the saved frame pointer
 * first and the return address second, and that agreement is a fact about two
 * ABIs rather than a rule, so each says so for itself. */
bool arch_frame_step(u64 frame, u64 *next, u64 *return_address);

bool backtrace_self_test(void);

#endif /* RECON_KERNEL_BACKTRACE_H */
