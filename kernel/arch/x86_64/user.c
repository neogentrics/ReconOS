/* Dropping to ring 3 on x86_64, and coming back.
 *
 * This is the architecture that makes it hardest. ARM has a number in a
 * register; x86 has descriptors, a task-state segment, four model-specific
 * registers, and an instruction pair whose two halves disagree about how they
 * find things. All of it is here, and every piece of it exists because
 * something in the processor insists.
 *
 * --- Going out ---
 *
 * The way *in* to a lower privilege level is the way *back* from an interrupt:
 * build the frame an interrupt would have pushed -- SS, RSP, RFLAGS, CS, RIP --
 * and execute IRETQ against it. The processor sees a return to a code segment
 * whose privilege is lower than the current one, and does the whole descent in
 * one instruction. SYSRET could do it too and is faster, but IRETQ is the
 * general case: it can set RFLAGS to anything, which matters exactly once, on
 * the first entry, when there are no saved flags to restore.
 *
 * --- Coming back ---
 *
 * SYSCALL, whose entry point is in user_entry.S. See the comment there for what
 * it does not do -- which is most of what a system call needs.
 *
 * --- The task-state segment ---
 *
 * Long mode threw out hardware task switching and kept the structure, because
 * one field in it could not be replaced: RSP0, the stack the processor switches
 * to when an *interrupt* arrives while user code is running. SYSCALL does not
 * consult it; interrupts do, and a timer tick that arrived while a user program
 * was running would otherwise push the kernel's interrupt frame onto the user
 * program's stack, at whatever address the user program had chosen.
 */
#include "x86_64.h"
#include <recon/kernel/console.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/pmm.h>

#include <recon/kernel/user.h>
#include <recon/kernel/sched.h>
#include <recon/kernel/smp.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/compiler.h>

/* What GS points at while the kernel runs, and the reason SWAPGS exists. The
 * two offsets are known to user_entry.S; they are the first two words on
 * purpose, so that the assembly can name them as GS:0 and GS:8. */
struct x86_cpu {
	u64 kernel_rsp;		/* GS:0 -- the stack a system call lands on */
	u64 user_rsp;		/* GS:8 -- where the user program was standing */
};

/* 64-bit task-state segment. Its layout is fixed by the processor, and the
 * packing matters: the first field is four bytes and every RSP after it is
 * eight, so the structure is deliberately misaligned and must stay that way. */
struct tss {
	u32 reserved0;
	u64 rsp[3];		/* the stack for each privilege level */
	u64 reserved1;
	u64 ist[7];		/* interrupt stack table; unused so far */
	u64 reserved2;
	u16 reserved3;
	u16 iomap_base;
} RK_PACKED;

static struct x86_cpu percpu[MAX_CPUS];
static struct tss tss[MAX_CPUS] RK_ALIGNED(16);

/* In user_entry.S. */
void x86_syscall_entry(void);

/* A 64-bit system descriptor: sixteen bytes, because the base address no longer
 * fits in the four the old format allowed. Written as two words rather than a
 * struct because the fields are scattered through it in an order that no
 * structure declaration makes clearer. */
static void write_tss_descriptor(unsigned slot, u64 base, u32 limit)
{
	x86_gdt[slot] = (u64)(limit & 0xFFFFu)
		      | ((base & 0xFFFFFFu) << 16)
		      | (0x89ULL << 40)		/* present, available 64-bit TSS */
		      | ((u64)((limit >> 16) & 0xFu) << 48)
		      | (((base >> 24) & 0xFFULL) << 56);
	x86_gdt[slot + 1] = (base >> 32) & 0xFFFFFFFFULL;
}

void arch_user_init(void)
{
	unsigned cpu = (unsigned)arch_cpu_id();
	u64 star;

	if (cpu >= MAX_CPUS)
		return;

	kmemset(&tss[cpu], 0, sizeof(tss[cpu]));

	/* An I/O permission bitmap base at or past the segment limit means
	 * "there is no bitmap", which is what we want: a user program that
	 * executes IN or OUT should fault, not reach the hardware. Leaving this
	 * field zero would instead point the processor at the start of the TSS
	 * and let it read the structure itself as permissions. */
	tss[cpu].iomap_base = sizeof(struct tss);

	/* This processor's block, parked where the system-call stub's SWAPGS
	 * will find it. Here rather than in arch_enter_user, which is where it
	 * used to be: that runs only on a processor that is about to enter user
	 * mode, so a processor which never did had KERNEL_GS_BASE at zero -- and
	 * a thread that entered a system call elsewhere and was rescheduled
	 * onto this one would SWAPGS to a base of zero and take a double fault
	 * on the first instruction of the entry stub.
	 *
	 * It is per-processor and constant, so once is the right number of
	 * times to write it. */
	x86_wrmsr(MSR_KERNEL_GS_BASE, (u64)(uintptr_t)&percpu[cpu]);
	x86_wrmsr(MSR_GS_BASE, 0);

	/* Two stacks this processor can take a fault on when the stack it was
	 * using is the problem. See x86_arm_fault_stacks in trap.c.
	 *
	 * Allocated per processor, because a double fault on two processors at
	 * once landing on one stack is the same class of failure the shared
	 * task-state segment had -- and it would arrive in the middle of
	 * reporting something else that had already gone wrong.
	 *
	 * A processor with no memory for them keeps the old behaviour, which is
	 * a triple fault on a bad stack. Saying so beats a silent reset. */
	{
		paddr_t df = pmm_alloc_pages(2);
		paddr_t nmi = pmm_alloc_pages(2);

		if (df && nmi) {
			/* The *top*, because the stack grows down. Writing the
			 * base here gives the processor a stack pointer aimed
			 * at the rest of memory. */
			tss[cpu].ist[0] = (u64)(uintptr_t)phys_to_virt(df) +
					  2 * PAGE_SIZE;
			tss[cpu].ist[1] = (u64)(uintptr_t)phys_to_virt(nmi) +
					  2 * PAGE_SIZE;

			x86_arm_fault_stacks();
		} else {
			if (df)
				pmm_free_pages(df, 2);
			if (nmi)
				pmm_free_pages(nmi, 2);

			kprintf("  cpu %u: no memory for fault stacks; a bad "
				"stack will reset the machine\n", cpu);
		}
	}

	/* This processor's own slot, not the shared one. See trap.c. */
	write_tss_descriptor(SEL_TSS_FOR(cpu) / 8, (u64)(uintptr_t)&tss[cpu],
			     sizeof(struct tss) - 1);
	__asm__ volatile("ltr %w0" : : "r"(SEL_TSS_FOR(cpu)));

	/* SYSCALL is off until EFER says otherwise. Read, modify, write: NXE was
	 * turned on in vm.c and clearing it here would make every no-execute
	 * mapping in the kernel a reserved-bit fault instead. */
	x86_wrmsr(MSR_EFER, x86_rdmsr(MSR_EFER) | 1u);

	/* STAR holds both directions in one register, and neither half is a
	 * selector you can read off the GDT and be done with.
	 *
	 * Bits 47:32 are the kernel base: SYSCALL loads CS from it and SS from
	 * it plus 8, which is why kernel data must follow kernel code.
	 *
	 * Bits 63:48 are the user base, and it is NOT the user code selector.
	 * SYSRET loads SS from base + 8 and CS from base + 16, so the value to
	 * store is the one *before* user data -- here, kernel data's selector.
	 * Storing the user code selector instead is the classic mistake, and it
	 * returns to user mode with a stack segment one entry off. */
	star = ((u64)SEL_KDATA << 48) | ((u64)SEL_KCODE << 32);
	x86_wrmsr(MSR_STAR, star);
	x86_wrmsr(MSR_LSTAR, (u64)(uintptr_t)x86_syscall_entry);

	/* Flags to clear on entry. IF, so the kernel is not interrupted before
	 * it has swapped onto its own stack -- the window between SYSCALL and
	 * the stack switch is three instructions long and an interrupt in it
	 * would push onto the user program's stack. DF, because the C compiler
	 * assumes it is clear and a user program is free to set it. TF, so that
	 * a program cannot single-step the kernel. */
	x86_wrmsr(MSR_FMASK, 0x700u);
}

void arch_thread_switched_in(struct thread *t)
{
	unsigned cpu = (unsigned)arch_cpu_id();

	/* A thread that has never been to user mode has nowhere for a trap
	 * from user mode to land, and cannot take one. Leaving the previous
	 * occupant's values in place is correct: the next thread that *can*
	 * take one brings its own. */
	if (cpu >= MAX_CPUS || !t || !t->entry_stack)
		return;

	percpu[cpu].kernel_rsp = (u64)(uintptr_t)t->entry_stack;
	tss[cpu].rsp[0] = (u64)(uintptr_t)t->entry_stack;
}

RK_NORETURN void arch_enter_user(u64 entry, u64 stack_top)
{
	unsigned cpu = (unsigned)arch_cpu_id();
	u64 rsp;

	/* The kernel stack to come back to, taken from where we are standing --
	 * which is this thread's own kernel stack, since arch_enter_user runs on
	 * it. Rounded down because both SYSCALL and IRETQ hand it to C code that
	 * expects sixteen-byte alignment.
	 *
	 * One thread's stack, recorded once. That is correct for exactly as long
	 * as one thread at a time runs in user mode; when processes arrive, this
	 * has to move into the context switch, where the switch already knows
	 * which stack is which. Recorded here as the known limit rather than
	 * discovered later as a mystery. */
	__asm__ volatile("movq %%rsp, %0" : "=r"(rsp));
	rsp &= ~0xFULL;

	/* Recorded on the *thread*, and then told to this processor. The
	 * comment above used to say this would have to move into the context
	 * switch when processes arrived, and it was right: see
	 * arch_thread_switched_in. */
	if (sched_current())
		sched_current()->entry_stack = (void *)(uintptr_t)rsp;

	percpu[cpu].kernel_rsp = rsp;
	tss[cpu].rsp[0] = rsp;

	/* The SWAPGS invariant -- GS_BASE zero, this processor's block in
	 * KERNEL_GS_BASE -- is established once per processor in
	 * arch_user_init and holds everywhere, so there is nothing to set
	 * here. It used to be set here, which is why a processor that had
	 * never entered user mode did not have it. */

	__asm__ volatile(
		"cli\n\t"

		/* DS and ES are ignored for addressing in 64-bit mode, but they
		 * are still loaded, and leaving kernel selectors in them across
		 * a privilege change is the kind of tidiness that costs two
		 * instructions and saves an afternoon. FS and GS are left
		 * alone: writing GS here would silently zero the base MSR that
		 * was just set. */
		"movw %w[uds], %%ds\n\t"
		"movw %w[uds], %%es\n\t"

		/* The frame IRETQ expects, in the order it pops it. */
		"pushq %[ss]\n\t"
		"pushq %[sp]\n\t"
		"pushq %[fl]\n\t"
		"pushq %[cs]\n\t"
		"pushq %[ip]\n\t"
		
		/* Every register the program has no business seeing, zeroed --
		 * after the frame is on the stack, so the operands above have
		 * already been consumed by the pushes.
		 *
		 * Without this, a program's first instruction can read whatever
		 * the kernel last left in a register: kernel stack addresses,
		 * pointers into kernel structures, whatever a system call was
		 * holding. None of it is secret the way a key is, and all of it
		 * is the map somebody needs to aim at the kernel -- knowing
		 * where the stack is defeats much of the point of not being
		 * able to read it.
		 *
		 * RSP is the exception and is untouched: IRETQ takes the
		 * program's stack pointer off the frame it was just given.
		 *
		 * Thirty-two-bit writes, because writing the low half of a
		 * register on this architecture clears the top half -- so this
		 * is shorter than the 64-bit form and does the same thing.
		 *
		 * The vector registers are not cleared here. They are restored
		 * from the thread's own saved state at every context switch, so
		 * they hold this thread's values rather than the kernel's -- and
		 * the kernel is built with no floating point at all. */
		"xorl %%eax, %%eax\n\t"
		"xorl %%ebx, %%ebx\n\t"
		"xorl %%ecx, %%ecx\n\t"
		"xorl %%edx, %%edx\n\t"
		"xorl %%esi, %%esi\n\t"
		"xorl %%edi, %%edi\n\t"
		"xorl %%ebp, %%ebp\n\t"
		"xorl %%r8d, %%r8d\n\t"
		"xorl %%r9d, %%r9d\n\t"
		"xorl %%r10d, %%r10d\n\t"
		"xorl %%r11d, %%r11d\n\t"
		"xorl %%r12d, %%r12d\n\t"
		"xorl %%r13d, %%r13d\n\t"
		"xorl %%r14d, %%r14d\n\t"
		"xorl %%r15d, %%r15d\n\t"
		
		"iretq\n\t"
		:
		: [uds] "r"((u16)(SEL_UDATA | 3)),
		  [ss] "r"((u64)(SEL_UDATA | 3)),
		  [sp] "r"(stack_top),
		  /* Bit 1 is reserved and must be set; bit 9 is IF. A user
		   * program entered with interrupts masked cannot be preempted,
		   * and one loop in it would stop the machine. */
		  [fl] "r"((u64)0x202),
		  [cs] "r"((u64)(SEL_UCODE | 3)),
		  [ip] "r"(entry)
		: "memory");

	__builtin_unreachable();
}
