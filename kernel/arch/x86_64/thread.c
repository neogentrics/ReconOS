#include "x86_64.h"

#include <recon/kernel/sched.h>
#include <recon/kernel/kstring.h>

extern void thread_trampoline(void);

/* Builds a stack that arch_context_switch() can switch *to*.
 *
 * The layout has to be exactly what the restore half of the switch expects,
 * read in the order it pops: r15, r14, r13, r12, rbp, rbx, then the return
 * address that RET consumes. Written low address to high, that is:
 *
 *     [r15][r14][r13][r12][rbp][rbx][return address]
 *      ^ the stack pointer we hand back
 *
 * R12 carries the function and R13 the argument, because they are callee-saved
 * -- the restore sequence loads them for free, so the trampoline finds them
 * already in place without any code to put them there.
 */
void *arch_thread_stack_init(void *stack_top, void (*entry)(void *), void *arg)
{
	u64 *sp = (u64 *)stack_top;

	/* Sixteen-byte alignment is required at a call boundary. The frame
	 * pushed below is seven words, so starting aligned and pushing seven
	 * leaves the stack misaligned by eight at the RET -- which is exactly
	 * what a real CALL would have left, since a CALL pushes a return
	 * address. Aligning here and letting the frame be odd is what makes the
	 * trampoline see the alignment the ABI promises it. */
	sp = (u64 *)((uintptr_t)sp & ~0xFULL);

	*--sp = (u64)(uintptr_t)thread_trampoline;	/* what RET returns to */
	*--sp = 0;					/* rbx */
	*--sp = 0;					/* rbp */
	*--sp = (u64)(uintptr_t)entry;			/* r12 */
	*--sp = (u64)(uintptr_t)arg;			/* r13 */
	*--sp = 0;					/* r14 */
	*--sp = 0;					/* r15 */

	return sp;
}
