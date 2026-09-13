/* Signals: recorded when sent, acted on when returning to user mode.
 *
 * See `signal.h` for why those are two different moments and why the second is
 * the only one delivery can happen at.
 *
 * --- The register block ---
 *
 * `signal_on_return` is handed a pointer into the kernel stack, at the saved
 * registers the system call entry pushed. The layout is fixed by
 * `user_entry.S` and named here once:
 *
 *   [0] r9   [1] r8   [2] r10  [3] rdx  [4] rsi  [5] rdi
 *   [6] rflags        [7] user rip      [8] user rsp
 *
 * Changing [7] and [8] is how a handler is entered: the same pops and the same
 * SYSRET happen, and they arrive somewhere else. Nothing in the return path
 * needed a branch added to it.
 */
#include <recon/kernel/signal.h>
#include <recon/kernel/process.h>
#include <recon/kernel/sched.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/user.h>
#include <recon/kernel/addrspace.h>


/* What the kernel pushes onto the user's stack before a handler runs, and
 * reads back out of it when the handler returns.
 *
 * On the user's stack rather than in the kernel because a program may take a
 * second signal while handling the first, and a single saved slot in the
 * kernel would lose the outer one. A stack nests; a slot does not. */
struct sig_frame {
	u64 rip;
	u64 rsp;
	u64 rflags;
	u64 rdi, rsi, rdx;
	u64 magic;
};

/* Checked on the way back. A restorer that was never given a frame, or a
 * program that invoked SYS_SIGRETURN on its own, is refused rather than
 * obeyed -- otherwise it is a call that sets the instruction pointer to
 * anything the caller likes, which is the whole ring boundary undone. */
#define SIG_FRAME_MAGIC		0x5265636F6E536967ull	/* "ReconSig" */

static struct sig_action actions[PROCESS_MAX][SIG_MAX];
static u32 pending[PROCESS_MAX];

static u64 sent, delivered, defaulted, ignored, refused_frames;

/* Which signals terminate a process that has said nothing about them. Every
 * one that is not here is ignored by default. */
static bool default_is_fatal(unsigned sig)
{
	switch (sig) {
	case SIGHUP: case SIGINT: case SIGQUIT: case SIGILL:
	case SIGABRT: case SIGFPE: case SIGKILL: case SIGSEGV:
	case SIGPIPE: case SIGALRM: case SIGTERM:
		return true;
	default:
		return false;
	}
}

static unsigned slot_of(u32 process_id)
{
	unsigned i;

	for (i = 0; i < PROCESS_MAX; i++) {
		struct process *p = process_at(i);

		if (p && p->id == process_id)
			return i;
	}

	return PROCESS_MAX;
}

bool signal_send(u32 process_id, unsigned sig)
{
	unsigned slot;

	if (!sig || sig >= SIG_MAX)
		return false;

	slot = slot_of(process_id);

	if (slot >= PROCESS_MAX)
		return false;

	pending[slot] |= (1u << sig);
	sent++;

	return true;
}

bool signal_set_action(unsigned sig, unsigned what, u64 handler, u64 restorer)
{
	struct thread *t = sched_current();
	unsigned slot;

	if (!sig || sig >= SIG_MAX || !t)
		return false;

	/* SIGKILL is the one a program does not get an opinion about. A
	 * process able to catch it is a process nothing can stop, which is the
	 * single thing this number exists to prevent. */
	if (sig == SIGKILL)
		return false;

	slot = slot_of(t->process);

	if (slot >= PROCESS_MAX)
		return false;

	if (what == SIG_ACTION_HANDLER) {
		/* Both, or neither. A handler with nowhere to return to runs
		 * once and then executes whatever follows it in memory --
		 * refused here, where the caller can still fix it. */
		if (!handler || !restorer)
			return false;

		if (!user_range_ok(handler, 1) ||
		    !user_range_ok(restorer, 1))
			return false;
	}

	actions[slot][sig].what = what;
	actions[slot][sig].handler = handler;
	actions[slot][sig].restorer = restorer;

	return true;
}

u32 signal_set_mask(u32 mask)
{
	struct thread *t = sched_current();
	u32 was;

	if (!t)
		return 0;

	was = t->signal_mask;

	/* SIGKILL is never blocked. Silently, rather than as an error: a
	 * program masking everything is doing something reasonable, and
	 * failing the whole call because of one bit would make it handle a
	 * case it cannot do anything about. */
	t->signal_mask = mask & ~(1u << SIGKILL);

	return was;
}

u32 signal_get_mask(void)
{
	struct thread *t = sched_current();

	return t ? t->signal_mask : 0;
}

/* The lowest-numbered pending signal this thread would act on, or zero. */
static unsigned next_for(struct thread *t, unsigned slot)
{
	u32 live = pending[slot] & ~t->signal_mask;
	unsigned sig;

	for (sig = 1; sig < SIG_MAX; sig++)
		if (live & (1u << sig))
			return sig;

	return 0;
}

bool signal_pending(void)
{
	struct thread *t = sched_current();
	unsigned slot;

	if (!t)
		return false;

	slot = slot_of(t->process);

	return slot < PROCESS_MAX && next_for(t, slot) != 0;
}

bool signal_on_return(void *ctx)
{
	struct thread *t = sched_current();
	unsigned slot, sig;
	struct sig_action *a;
	u64 sp;
	struct sig_frame *frame;
	struct sig_regs r;

	if (!t || !ctx)
		return false;

	/* A restorer asked to go back. Done before anything else: a signal
	 * that arrived while a handler was running is still pending, and
	 * delivering it now -- on top of a frame that has not been unwound --
	 * would nest a second handler over the first with no way back out of
	 * either. It stays pending and arrives on the next return. */
	if (t->signal_restoring) {
		t->signal_restoring = false;
		signal_return(ctx);
		return true;
	}

	slot = slot_of(t->process);

	if (slot >= PROCESS_MAX)
		return false;

	sig = next_for(t, slot);

	if (!sig)
		return false;

	pending[slot] &= ~(1u << sig);
	a = &actions[slot][sig];

	/* SIGKILL takes the default whatever was registered, because
	 * signal_set_action refuses to record anything else for it. Stated
	 * here as well so the reading of this function does not depend on
	 * remembering that. */
	if (sig == SIGKILL || a->what == SIG_ACTION_DEFAULT) {
		if (!default_is_fatal(sig)) {
			ignored++;
			return false;
		}

		defaulted++;
		kprintf("  signal       : process %u ended by signal %u\n",
			t->process, sig);

		/* Ends the thread, which is what sys_exit does. Does not
		 * return, so nothing below this runs and the saved registers
		 * are never used. */
		thread_exit();
		return true;
	}

	if (a->what == SIG_ACTION_IGNORE) {
		ignored++;
		return false;
	}

	/* A handler. The interrupted context goes onto the user's own stack,
	 * below where it was, and the return path is pointed at the handler.
	 *
	 * Sixteen-byte alignment is not decoration: the ABI promises it at a
	 * function's entry, and code compiled with vector instructions will
	 * fault on a stack that does not have it. The frame is placed first
	 * and the alignment applied after, so the frame keeps its address. */
	arch_sig_get(ctx, &r);
	sp = r.sp;

	if (sp < sizeof(*frame) + 256)
		return false;

	sp -= sizeof(*frame);
	sp &= ~0xFull;

	if (!user_range_ok(sp, sizeof(*frame)))
		return false;

	frame = (struct sig_frame *)(uintptr_t)sp;

	frame->rip    = r.pc;
	frame->rsp    = r.sp;
	frame->rflags = r.flags;
	frame->rdi    = r.arg0;
	frame->rsi    = r.arg1;
	frame->rdx    = r.arg2;
	frame->magic  = SIG_FRAME_MAGIC;

	/* The restorer is where the handler returns to, and *where that is*
	 * differs by architecture -- a word on the stack here, a register
	 * there. Asked rather than assumed. */
	sp = arch_sig_place_return(ctx, sp, a->restorer);

	r.sp   = sp;
	r.pc   = a->handler;
	r.arg0 = (u64)sig;		/* the handler's one argument */
	arch_sig_set(ctx, &r);

	delivered++;

	return true;
}

i64 signal_return(void *ctx)
{
	struct sig_frame *frame;
	struct sig_regs r;
	u64 sp;

	if (!ctx)
		return SYS_EFAULT;

	/* The restorer was entered by the handler returning, so the frame sits
	 * exactly where the handler's stack pointer now points. */
	arch_sig_get(ctx, &r);
	sp = r.sp;

	if (!user_range_ok(sp, sizeof(*frame)))
		return SYS_EFAULT;

	frame = (struct sig_frame *)(uintptr_t)sp;

	if (frame->magic != SIG_FRAME_MAGIC) {
		/* Not a frame this kernel wrote. Refused, and counted,
		 * because a program reaching here is either broken or trying
		 * to set its own instruction pointer through a system call. */
		refused_frames++;
		return SYS_EPERM;
	}

	r.pc = frame->rip;
	r.sp = frame->rsp;

	/* Flags are restored except for the bits a user program must not set
	 * for itself. IF stays on and IOPL stays at zero, whatever the frame
	 * says -- a frame is on memory the program can write, so every field
	 * taken from it is a field the program chose. */
	r.flags = arch_sig_safe_flags(frame->rflags);

	r.arg0 = frame->rdi;
	r.arg1 = frame->rsi;
	r.arg2 = frame->rdx;

	arch_sig_set(ctx, &r);

	return SYS_OK;
}

void signal_print_summary(void)
{
	kprintf("  signals      : %llu sent, %llu delivered to a handler, "
		"%llu took the default, %llu ignored\n",
		(unsigned long long)sent, (unsigned long long)delivered,
		(unsigned long long)defaulted, (unsigned long long)ignored);

	if (refused_frames)
		kprintf("  signals      : %llu restorer calls refused -- a "
			"frame this kernel did not write\n",
			(unsigned long long)refused_frames);
}
