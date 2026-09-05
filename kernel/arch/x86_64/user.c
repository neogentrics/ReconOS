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

	write_tss_descriptor(SEL_TSS / 8, (u64)(uintptr_t)&tss[cpu],
			     sizeof(struct tss) - 1);
	__asm__ volatile("ltr %w0" : : "r"((u16)SEL_TSS));

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

	percpu[cpu].kernel_rsp = rsp;
	tss[cpu].rsp[0] = rsp;

	/* Establish the SWAPGS invariant for user mode: GS_BASE zero, this
	 * processor's block parked in KERNEL_GS_BASE for the entry stub to swap
	 * in. Set in this order so that no kernel code between here and IRETQ
	 * can be tempted to read GS and find the wrong thing. */
	x86_wrmsr(MSR_KERNEL_GS_BASE, (u64)(uintptr_t)&percpu[cpu]);
	x86_wrmsr(MSR_GS_BASE, 0);

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
