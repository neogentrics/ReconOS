/* Signals -- telling a program something happened, whether or not it asked.
 *
 * Every other way into a program is something it called. A signal is the one
 * thing that arrives, and that is what makes it awkward: the program is in the
 * middle of whatever it was doing, and the kernel has to interrupt it in a way
 * it can survive.
 *
 * --- Where delivery happens, and why it is the only place ---
 *
 * On the way back to user mode, and nowhere else. Not when the signal is sent
 * -- the target may be running on another processor, or asleep, or not
 * scheduled for a second. Sending records; **returning delivers**.
 *
 * That is a place this kernel has never done anything conditional. The return
 * from a system call was a fixed sequence of pops and a SYSRET, and it is now a
 * fixed sequence of pops and a SYSRET with one question asked first. The
 * question is asked with interrupts off, on the kernel stack, with the user's
 * registers still saved -- which is the only moment where the kernel both knows
 * where the program was and can still change where it goes.
 *
 * --- What a signal does when nothing has been said about it ---
 *
 * Terminates the process, except for the ones whose default is to be ignored.
 * That is not a policy this kernel invented; it is what programs expect, and a
 * default of "ignore" would mean a program that never installed a handler could
 * not be stopped.
 *
 * --- Handlers, and the restorer ---
 *
 * A handler runs in user mode, on the user's own stack, with the interrupted
 * context saved below it. When it returns it has to get back -- and there is no
 * address in the program it can return *to*, because the kernel put it there.
 *
 * So the caller supplies a **restorer**: a few instructions that invoke
 * SYS_SIGRETURN. Linux calls this SA_RESTORER and hides it inside the C
 * library. Requiring it here is deliberate rather than lazy: the alternative is
 * a page of kernel-provided code mapped into every address space, which is a
 * real thing to build and a decision about the shape of every program's memory.
 * A caller that passes no restorer gets its handler refused at registration,
 * which is the moment it can still do something about it.
 */
#ifndef RECON_KERNEL_SIGNAL_H
#define RECON_KERNEL_SIGNAL_H

#include <recon/kernel/types.h>

/* Sixteen, which is what fits in the mask a thread carries and is more than
 * anything here raises. The numbers are ours: a program built for another
 * system arrives through a personality that translates them. */
#define SIG_MAX		16

#define SIGHUP		1	/* the thing on the other end went away */
#define SIGINT		2	/* somebody asked it to stop */
#define SIGQUIT		3
#define SIGILL		4	/* it executed something that is not an instruction */
#define SIGABRT		6	/* it gave up on itself */
#define SIGFPE		8
#define SIGKILL		9	/* cannot be caught, blocked or ignored */
#define SIGSEGV		11	/* it touched memory that is not there */
#define SIGPIPE		13	/* it wrote to a pipe nobody is reading */
#define SIGALRM		14
#define SIGTERM		15	/* please stop */

/* What to do when one arrives. */
#define SIG_ACTION_DEFAULT	0
#define SIG_ACTION_IGNORE	1
#define SIG_ACTION_HANDLER	2

struct sig_action {
	u64 handler;		/* user address, when the action is HANDLER */
	u64 restorer;		/* where the handler returns to */
	unsigned what;		/* one of SIG_ACTION_* */
};

/* Sent to a process. Recorded; delivered when one of its threads next returns
 * to user mode. */
bool signal_send(u32 process_id, unsigned sig);

/* What a process should do about a signal, and what it was doing before. */
bool signal_set_action(unsigned sig, unsigned what, u64 handler, u64 restorer);

/* Which signals this thread is not accepting at the moment. SIGKILL cannot be
 * blocked, and asking to block it is not an error -- the bit is simply not
 * taken, because a program that could block it could not be stopped. */
u32 signal_set_mask(u32 mask);
u32 signal_get_mask(void);

/* Is anything pending that this thread would act on? Cheap enough to ask on
 * every return to user mode, which is what it is for. */
bool signal_pending(void);

/* The part of an interrupted user context that delivering a signal needs.
 *
 * Six values, and no more. A handler is entered with a fresh register set and
 * returns through the restorer, so the only things that must survive are where
 * the program was, what stack it was on, its flags, and the few argument
 * registers a system call's return would otherwise have arrived in.
 *
 * The two architectures keep these in very different places -- x86_64 has them
 * in a block the system call entry pushed, aarch64 in a trap frame plus a
 * register only a special instruction can read -- so the shape is named here
 * and each architecture reads and writes its own. */
struct sig_regs {
	u64 pc;		/* where the program was */
	u64 sp;		/* the stack it was on */
	u64 flags;
	u64 arg0;	/* and the registers a return value would use */
	u64 arg1;
	u64 arg2;
};

/* Implemented per architecture. `ctx` is whatever that architecture handed to
 * signal_on_return -- a pointer it understands and this file does not. */
void arch_sig_get(void *ctx, struct sig_regs *out);
void arch_sig_set(void *ctx, const struct sig_regs *in);

/* Flags a user program is allowed to have restored to it.
 *
 * The frame is on memory the program can write, so every field taken from it
 * is a field the program chose. Which bits are safe is entirely architectural
 * -- on x86_64 it is about IOPL and the interrupt flag; on aarch64 it is about
 * which exception level SPSR names. */
u64 arch_sig_safe_flags(u64 from_frame);

/* Arranges for a handler to return to `restorer`, and answers what the stack
 * pointer should be when the handler is entered.
 *
 * The two architectures disagree about where a return address *is*, and that
 * disagreement is not a detail: on x86_64 `ret` takes a word off the stack, so
 * the restorer is pushed and the stack pointer moves. On aarch64 `ret` jumps to
 * the link register, so the restorer goes in x30 and the stack does not move at
 * all.
 *
 * Getting this wrong is quiet. The handler runs correctly, does its work, and
 * then returns to whatever was already in the place this kernel forgot to
 * write -- which on aarch64 was x30, still holding wherever the program last
 * made a call from. */
u64 arch_sig_place_return(void *ctx, u64 sp, u64 restorer);

/* Called on the way back to user mode. The layout `regs` points at is the
 * architecture's business; see the arch file. Returns true when the thread
 * should not go back where it came from. */
bool signal_on_return(void *ctx);

/* Restores what was interrupted. The system call a restorer makes. */
i64 signal_return(void *ctx);

void signal_print_summary(void);
bool signal_self_test(void);

#endif
