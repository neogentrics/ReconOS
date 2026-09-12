/* Reading and writing an interrupted user context on aarch64.
 *
 * `ctx` is the trap frame `vectors.S` builds: x0..x30 at the front, then ELR
 * and SPSR. The user stack is **not** in it -- aarch64 gives EL0 its own stack
 * pointer in `SP_EL0`, which only a system-register move can reach, so it is
 * read and written here rather than indexed.
 *
 * That difference is the whole reason the portable half asks an architecture
 * for six values instead of indexing a block itself.
 */
#include <recon/kernel/signal.h>

#define F_ELR		31
#define F_SPSR		32
#define F_X30		30

static u64 read_sp_el0(void)
{
	u64 v;

	__asm__ __volatile__("mrs %0, sp_el0" : "=r"(v));
	return v;
}

static void write_sp_el0(u64 v)
{
	__asm__ __volatile__("msr sp_el0, %0" :: "r"(v));
}

void arch_sig_get(void *ctx, struct sig_regs *out)
{
	const u64 *f = ctx;

	out->pc    = f[F_ELR];
	out->sp    = read_sp_el0();
	out->flags = f[F_SPSR];
	out->arg0  = f[0];
	out->arg1  = f[1];
	out->arg2  = f[2];
}

void arch_sig_set(void *ctx, const struct sig_regs *in)
{
	u64 *f = ctx;

	f[F_ELR]  = in->pc;
	f[F_SPSR] = in->flags;
	f[0]      = in->arg0;
	f[1]      = in->arg1;
	f[2]      = in->arg2;

	write_sp_el0(in->sp);
}

u64 arch_sig_safe_flags(u64 from_frame)
{
	/* Only the condition flags -- N, Z, C, V -- are the program's to
	 * choose. Everything else in SPSR says what exception level to return
	 * to and whether interrupts are masked there, and a program that could
	 * set those through a frame it wrote would return to EL1.
	 *
	 * The rest is pinned: mode 0 is EL0 with SP_EL0, and the four mask bits
	 * stay clear so the program comes back interruptible. */
	return (from_frame & 0xF0000000ull);
}

u64 arch_sig_place_return(void *ctx, u64 sp, u64 restorer)
{
	u64 *f = ctx;

	/* `ret` jumps to x30. Nothing goes on the stack and the stack pointer
	 * does not move -- which is why this returns what it was given.
	 *
	 * The handler's own x30 is not saved anywhere, and does not need to
	 * be: the interrupted context is in the frame on the user's stack, and
	 * x30 at the moment of interruption is part of what the *program*
	 * saved when it made whatever call it was inside. A handler that wants
	 * to make calls of its own saves x30 like any other function. */
	f[F_X30] = restorer;

	return sp;
}
