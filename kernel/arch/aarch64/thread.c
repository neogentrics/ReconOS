#include "aarch64.h"

#include <recon/kernel/sched.h>
#include <recon/kernel/kstring.h>

extern void thread_trampoline(void);

/* Builds a stack that arch_context_switch() can switch *to*.
 *
 * The layout is the twelve saved registers in the order the restore half loads
 * them, low address to high:
 *
 *     [x19][x20][x21][x22][x23][x24][x25][x26][x27][x28][x29][x30]
 *      ^ the stack pointer we hand back
 *
 * x19 carries the function and x20 the argument, and x30 -- the link register,
 * which is where this architecture keeps the return address -- holds the
 * trampoline. So the RET at the end of the switch goes to the trampoline with
 * the function and argument already in registers, at no cost, because the
 * restore sequence put them there.
 */
void *arch_thread_stack_init(void *stack_top, void (*entry)(void *), void *arg)
{
	u64 *sp = (u64 *)((uintptr_t)stack_top & ~0xFULL);

	/* Sixteen-byte alignment is not a convention on this architecture, it is
	 * enforced: the CPU faults on a stack-pointer-relative access with the
	 * stack misaligned. Twelve slots is ninety-six bytes, which keeps it. */
	sp -= 12;

	kmemset(sp, 0, 12 * sizeof(u64));

	sp[0]  = (u64)(uintptr_t)entry;			/* x19 */
	sp[1]  = (u64)(uintptr_t)arg;			/* x20 */
	sp[11] = (u64)(uintptr_t)thread_trampoline;	/* x30, the return address */

	return sp;
}
