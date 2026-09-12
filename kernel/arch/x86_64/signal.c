/* Reading and writing an interrupted user context on x86_64.
 *
 * `ctx` is the register block the system call entry pushed, at the offsets
 * `user_entry.S` fixes:
 *
 *   [0] r9  [1] r8  [2] r10  [3] rdx  [4] rsi  [5] rdi
 *   [6] rflags      [7] user rip      [8] user rsp
 *
 * Named once, here, because this is the only file that has any business
 * knowing them.
 */
#include <recon/kernel/signal.h>

#define REG_R9		0
#define REG_R8		1
#define REG_R10		2
#define REG_RDX		3
#define REG_RSI		4
#define REG_RDI		5
#define REG_RFLAGS	6
#define REG_RIP		7
#define REG_RSP		8

void arch_sig_get(void *ctx, struct sig_regs *out)
{
	const u64 *regs = ctx;

	out->pc    = regs[REG_RIP];
	out->sp    = regs[REG_RSP];
	out->flags = regs[REG_RFLAGS];
	out->arg0  = regs[REG_RDI];
	out->arg1  = regs[REG_RSI];
	out->arg2  = regs[REG_RDX];
}

void arch_sig_set(void *ctx, const struct sig_regs *in)
{
	u64 *regs = ctx;

	regs[REG_RIP]    = in->pc;
	regs[REG_RSP]    = in->sp;
	regs[REG_RFLAGS] = in->flags;
	regs[REG_RDI]    = in->arg0;
	regs[REG_RSI]    = in->arg1;
	regs[REG_RDX]    = in->arg2;
}

u64 arch_sig_safe_flags(u64 from_frame)
{
	/* The arithmetic and direction flags a program may choose, plus the
	 * two bits it does not get an opinion about: bit 1 is reserved and
	 * must be set, and IF must stay on. IOPL and everything above is
	 * masked off -- a program that could restore IOPL through a frame it
	 * wrote would have given itself port access. */
	return (from_frame & 0x0CD5ull) | 0x202ull;
}

u64 arch_sig_place_return(void *ctx, u64 sp, u64 restorer)
{
	/* `ret` takes its address off the stack, so the restorer goes exactly
	 * where a CALL would have left it: immediately below the stack pointer
	 * the handler is entered with. */
	sp -= 8;
	*(u64 *)(uintptr_t)sp = restorer;

	return sp;
}
