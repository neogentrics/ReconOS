/* x86_64 exception handling.
 *
 * Two tables get installed here, and the second is the interesting one.
 *
 * THE INTERRUPT DESCRIPTOR TABLE says where to go for each of 256 vectors.
 *
 * THE GLOBAL DESCRIPTOR TABLE has to be ours, and this is not obvious. On the
 * two boot paths that come through our own trampoline the GDT is already in the
 * kernel image and fine. On the UEFI path it is the *firmware's* GDT -- which
 * sits in memory the firmware marked as boot-services data, which we correctly
 * treat as free, and which the page allocator will therefore hand out and
 * something will write over. Nothing goes wrong immediately. It goes wrong the
 * first time the CPU reloads a segment descriptor, which is at the next
 * interrupt, and by then the cause is a long way from the effect.
 */
#include "x86_64.h"

#include <recon/kernel/smp.h>

#include <recon/kernel/trap.h>
#include <recon/kernel/addrspace.h>
#include <recon/kernel/user.h>
#include <recon/kernel/console.h>
#include <recon/kernel/backtrace.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/panic.h>
#include <recon/kernel/user.h>
#include <recon/kernel/sched.h>

/* Pushed by the stubs in isr.S, in the order that file pushes them. The two
 * declarations have to agree exactly; a mismatch is a dump of the wrong
 * registers, which is worse than no dump because it is believable. */
struct trap_frame {
	u64 r15, r14, r13, r12, r11, r10, r9, r8;
	u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
	u64 vector;
	u64 error;
	/* Pushed by the CPU itself. */
	u64 rip, cs, rflags, rsp, ss;
};

struct idt_entry {
	u16 offset_low;
	u16 selector;
	u8  ist;
	u8  flags;
	u16 offset_mid;
	u32 offset_high;
	u32 reserved;
} RK_PACKED;

struct table_descriptor {
	u16 limit;
	u64 base;
} RK_PACKED;

static struct idt_entry idt[256];

/* Our own, so that nothing depends on firmware memory staying untouched.
 *
 * The order is not free. SYSRET reconstructs both user selectors by *adding to*
 * a single base held in one MSR: the stack selector is base + 8 and the code
 * selector is base + 16. So user data must sit immediately after kernel data,
 * and user code immediately after that, or returning to user mode lands on
 * whatever descriptor happens to be there. A GDT laid out in the order a person
 * would find natural -- code, code, data, data -- does not work, and fails by
 * loading a plausible wrong segment rather than by refusing.
 *
 * After them come the task-state segments: sixteen bytes each, because a base
 * address no longer fits in one entry, and **one per processor**.
 *
 * One per processor rather than one shared, which is what this was until
 * checkpoint 9b. A TSS holds the stack a system call lands on, and that stack
 * cannot be shared: two processors entering the kernel at once on one stack
 * overwrite each other's frames. The single slot worked only because there was
 * only ever one processor to write it -- arch_user_init filled it with the
 * caller's own TSS, so with secondaries running every processor would have used
 * whichever one was written last. Nothing would have failed until two of them
 * took a system call at the same moment.
 *
 * They are filled in at run time by user.c, which is the only code that needs
 * them. */
u64 x86_gdt[5 + 2 * MAX_CPUS] RK_ALIGNED(16) = {
	0,
	0x00AF9A000000FFFFULL,	/* 0x08 kernel code: executable, long mode */
	0x00CF92000000FFFFULL,	/* 0x10 kernel data: writable */
	0x00CFF2000000FFFFULL,	/* 0x18 user data: writable, DPL 3 */
	0x00AFFA000000FFFFULL,	/* 0x20 user code: executable, long mode, DPL 3 */
	/* 0x28 onwards: one task-state segment per processor, two entries each,
	 * left zero until the processor that owns it fills it in. */
};

static struct table_descriptor gdt_ptr, idt_ptr;

/* In isr.S. Takes the two descriptors and reloads every segment register,
 * which cannot be done from C because CS can only be written by a far jump. */
void x86_load_tables(struct table_descriptor *gdt_d, struct table_descriptor *idt_d);

/* The stubs, one per vector. */
extern void isr_0(void);   extern void isr_1(void);   extern void isr_2(void);
extern void isr_3(void);   extern void isr_4(void);   extern void isr_5(void);
extern void isr_6(void);   extern void isr_7(void);   extern void isr_8(void);
extern void isr_9(void);   extern void isr_10(void);  extern void isr_11(void);
extern void isr_12(void);  extern void isr_13(void);  extern void isr_14(void);
extern void isr_15(void);  extern void isr_16(void);  extern void isr_17(void);
extern void isr_18(void);  extern void isr_19(void);  extern void isr_20(void);
extern void isr_21(void);  extern void isr_22(void);  extern void isr_23(void);
extern void isr_24(void);  extern void isr_25(void);  extern void isr_26(void);
extern void isr_27(void);  extern void isr_28(void);  extern void isr_29(void);
extern void isr_30(void);  extern void isr_31(void);

extern void irq_0(void);   extern void irq_1(void);   extern void irq_2(void);
extern void irq_3(void);   extern void irq_4(void);   extern void irq_5(void);
extern void irq_6(void);   extern void irq_7(void);   extern void irq_8(void);
extern void irq_9(void);   extern void irq_10(void);  extern void irq_11(void);
extern void irq_12(void);  extern void irq_13(void);  extern void irq_14(void);
extern void irq_15(void);

/* The local APIC's two, which have vectors rather than IRQ numbers. */
extern void vector_64(void);
extern void vector_65(void);
extern void vector_66(void);
extern void vector_255(void);

static void (*const stubs[32])(void) = {
	isr_0,  isr_1,  isr_2,  isr_3,  isr_4,  isr_5,  isr_6,  isr_7,
	isr_8,  isr_9,  isr_10, isr_11, isr_12, isr_13, isr_14, isr_15,
	isr_16, isr_17, isr_18, isr_19, isr_20, isr_21, isr_22, isr_23,
	isr_24, isr_25, isr_26, isr_27, isr_28, isr_29, isr_30, isr_31,
};

static void (*const irq_stubs[16])(void) = {
	irq_0,  irq_1,  irq_2,  irq_3,  irq_4,  irq_5,  irq_6,  irq_7,
	irq_8,  irq_9,  irq_10, irq_11, irq_12, irq_13, irq_14, irq_15,
};

static const char *const exception_name[32] = {
	"divide error", "debug", "non-maskable interrupt", "breakpoint",
	"overflow", "bound range exceeded", "invalid opcode",
	"device not available", "double fault", "coprocessor segment overrun",
	"invalid TSS", "segment not present", "stack-segment fault",
	"general protection fault", "page fault", "reserved",
	"x87 floating-point exception", "alignment check", "machine check",
	"SIMD floating-point exception", "virtualisation exception",
	"control protection exception", "reserved", "reserved", "reserved",
	"reserved", "reserved", "reserved", "hypervisor injection",
	"VMM communication exception", "security exception", "reserved",
};

static void set_gate(unsigned vector, void (*handler)(void), u16 selector)
{
	u64 addr = (u64)(uintptr_t)handler;

	idt[vector].offset_low  = (u16)(addr & 0xFFFF);
	idt[vector].selector    = selector;
	idt[vector].ist         = 0;
	/* 0x8E: present, privilege level 0, 64-bit interrupt gate. An interrupt
	 * gate rather than a trap gate, so interrupts are masked on entry --
	 * a handler that can be interrupted by the thing it is handling is a
	 * handler that runs out of stack. */
	idt[vector].flags       = 0x8E;
	idt[vector].offset_mid  = (u16)((addr >> 16) & 0xFFFF);
	idt[vector].offset_high = (u32)(addr >> 32);
	idt[vector].reserved    = 0;
}

/* --- Stacks for the faults that cannot use the one they arrived on --------
 *
 * A double fault means the processor failed while *delivering* a fault, and by
 * far the most common cause is that the stack it was told to use is unusable --
 * unmapped, misaligned, or overrun. Pushing a fault frame onto that same stack
 * to report it fails in exactly the same way, and a fault taken while
 * delivering a double fault is a triple fault, which is a silent reset.
 *
 * So the interrupt stack table exists: an IST index in a gate tells the
 * processor to load a stack pointer out of the TSS instead of using the current
 * one. It is the difference between a machine that says what went wrong and a
 * machine that reboots.
 *
 * The TSS field is per-processor and the IDT is shared, which sets the order:
 * these gates are armed by whichever processor has just filled in its own
 * stacks, and never before -- a gate naming an IST slot that is still zero
 * turns the one fault this exists to survive into the one it exists to prevent.
 */
#define IST_DOUBLE_FAULT	1	/* one-based, as the gate encodes it */
#define IST_NMI			2

void x86_arm_fault_stacks(void)
{
	set_gate(2, stubs[2], 0x08);
	idt[2].ist = IST_NMI;

	set_gate(8, stubs[8], 0x08);
	idt[8].ist = IST_DOUBLE_FAULT;
}

/* A page fault's error code says what was attempted, and the four bits that
 * matter are worth spelling out rather than printing as a number. */
static void describe_page_fault(u64 error)
{
	u64 cr2;

	__asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

	/* Unlocked like the rest of the report: this is only ever reached from
	 * the fault path, where waiting for the console lock could mean waiting
	 * for the code that just faulted while holding it. */
	kprintf_unlocked("  touched      : %p\n", (void *)(uintptr_t)cr2);
	kprintf_unlocked("  what happened: %s, %s, in %s mode%s\n",
		(error & 1) ? "protection violation" : "page not present",
		(error & 2) ? "on a write" : "on a read",
		(error & 4) ? "user" : "kernel",
		(error & 8) ? ", reserved bit set in a page table entry" : "");

	/* That last clause exists because of checkpoint 5: setting the
	 * no-execute bit without enabling it in EFER produces exactly this, and
	 * it took reading QEMU's own log to see it. Now the kernel says so. */
}

void trap_dispatch(struct trap_frame *f)
{
	/* This processor's own timer, from its local APIC rather than from the
	 * one 8254 in the machine. Acknowledged to the APIC, never to the 8259:
	 * telling the 8259 an interrupt it did not send has been handled
	 * unbalances it, and it stops delivering the ones it did send. */
	if (f->vector == VECTOR_APIC_TIMER) {
		x86_apic_timer_interrupt();
		return;
	}

	/* Another processor has changed a mapping this one may have cached.
	 *
	 * Handled before anything else that could take time, because the
	 * processor that sent it is *waiting* -- and acknowledged to the APIC
	 * before the acknowledgement to the sender, so that the sender is never
	 * released while this processor still has an interrupt in service. */
	if (f->vector == VECTOR_TLB_SHOOTDOWN) {
		x86_apic_eoi();
		x86_tlb_shootdown_service();
		return;
	}

	/* Raised when an interrupt is withdrawn between being raised and being
	 * taken. Rare, harmless, and it must land somewhere that returns --
	 * with no acknowledgement, which is the one thing the specification is
	 * explicit about. */
	if (f->vector == VECTOR_SPURIOUS)
		return;

	/* A device -- or, so far, only the self-test -- raising an interrupt by
	 * writing to the local APIC's window. Acknowledged like any other
	 * interrupt the APIC delivered; there is no controller behind it to
	 * tell, which is the point of it. */
	if (f->vector == VECTOR_MSI) {
		x86_msi_test_arrivals++;
		x86_apic_eoi();
		return;
	}

	/* A hardware interrupt rather than a fault. Handled and acknowledged;
	 * an interrupt the controller is not told about is the last one it
	 * ever sends. */
	if (f->vector >= 32 && f->vector < 48) {
		if (f->vector == 32)
			x86_timer_interrupt();
		else
			x86_irq_ack((unsigned)(f->vector - 32));
		return;
	}

	/* A fault in a user program rather than in the kernel.
	 *
	 * The bottom two bits of the saved CS are the privilege the code was
	 * running at, and three means ring 3. It is the saved CS and not the
	 * current one that matters: by the time this runs the processor is in
	 * ring 0 either way, and asking where it *is* would answer every time.
	 *
	 * A user program that faults is ended, and nothing else happens. That
	 * sentence is the whole return on this checkpoint -- before it, any
	 * wrong pointer anywhere stopped the machine. */
	if ((f->cs & 3) == 3) {
		/* Before it is a fault, ask whether it is a promise.
		 *
		 * A page fault on an address the program was told it could
		 * use is how demand paging works at all: nothing was mapped,
		 * the program touched it, and the handler makes it exist and
		 * lets the instruction run again. Bit 1 of the error code says
		 * the access was a write, which is what decides between
		 * sharing the machine's one page of zeroes and handing over a
		 * page of its own.
		 *
		 * Only for exception 14. A protection fault at an address is
		 * not a missing page, and must not be answered by inventing
		 * one. */
		if (f->vector == 14) {
			u64 want;

			__asm__ volatile("movq %%cr2, %0" : "=r"(want));

			if (vm_fault_user((vaddr_t)want, (f->error & 2) != 0))
				return;
		}

		kprintf("\nuser program fault: %s at %p\n",
			f->vector < 32 ? exception_name[f->vector] : "unknown",
			(void *)(uintptr_t)f->rip);
		if (f->vector == 14) {
			u64 cr2;

			__asm__ volatile("movq %%cr2, %0" : "=r"(cr2));
			kprintf("  it touched %p, which is not its to touch\n",
				(void *)(uintptr_t)cr2);
		}
		user_note_fault();
		thread_exit();
	}

	/* The kernel, touching a program's memory on its behalf.
	 *
	 * A system call that writes into a buffer the program gave it is the
	 * kernel dereferencing a user address, and the moment that memory is
	 * demand paged the write can fault -- in kernel mode, at a user
	 * address, with nothing wrong. Refusing here would mean every buffer a
	 * program passes has to be touched by the program first, which is a
	 * rule nobody could keep and one that fails silently when they do not.
	 *
	 * This is deliberately narrow. It only applies while a program's
	 * address space is the active one, and `vm_fault_user` still refuses
	 * any address that program was not promised -- so a kernel pointer that
	 * has gone wild into the lower half is still a fault, and still stops
	 * the machine with a report. What it must not become is a rule that any
	 * low address the kernel touches is made to exist on request.
	 */
	if (f->vector == 14) {
		u64 want;

		__asm__ volatile("movq %%cr2, %0" : "=r"(want));

		if (want < USER_LIMIT && addrspace_active() &&
			vm_fault_user((vaddr_t)want, (f->error & 2) != 0))
			return;
	}

	/* Somebody was expecting this. Record it and resume where they said,
	 * rather than reporting a fault that is not a fault. */
	if (trap_expecting) {
		trap_caught = true;
		trap_expecting = false;
		trap_note_catch();
		f->rip = (u64)(uintptr_t)trap_recovery;
		return;
	}

	kputs_unlocked("\n=== ReconOS kernel fault ===\n");
	kprintf_unlocked("  exception    : %lu, %s\n", f->vector,
		f->vector < 32 ? exception_name[f->vector] : "unknown");
	kprintf_unlocked("  error code   : %lu\n", f->error);
	kprintf_unlocked("  at           : %p\n", (void *)(uintptr_t)f->rip);

	if (f->vector == 14)
		describe_page_fault(f->error);

	kprintf_unlocked("  rax %p  rbx %p\n", (void *)(uintptr_t)f->rax, (void *)(uintptr_t)f->rbx);
	kprintf_unlocked("  rcx %p  rdx %p\n", (void *)(uintptr_t)f->rcx, (void *)(uintptr_t)f->rdx);
	kprintf_unlocked("  rsi %p  rdi %p\n", (void *)(uintptr_t)f->rsi, (void *)(uintptr_t)f->rdi);
	kprintf_unlocked("  rbp %p  rsp %p\n", (void *)(uintptr_t)f->rbp, (void *)(uintptr_t)f->rsp);
	kprintf_unlocked("  flags %p  cs %lu\n", (void *)(uintptr_t)f->rflags, f->cs);

	panic("unhandled exception");
}

/* Faults on purpose, and resumes at the instruction after the one that faulted.
 *
 * The resume label lives *inside* the same asm block as the faulting store, and
 * that is the entire point. A label in C can be moved or deleted by the
 * optimiser -- which is what happened when this was written in C, leaving the
 * recovery address pointing at the function prologue and the kernel retaking
 * the same fault forever. Inside an asm block, `1:` is exactly where it is
 * written, and nothing may come between it and the instruction above it. */
bool trap_provoke_fault(void)
{
	unsigned long addr = trap_test_address;

	__asm__ volatile(
		"leaq 1f(%%rip), %%rax\n\t"
		"movq %%rax, %[rec]\n\t"
		"movb $1, %[exp]\n\t"
		"movl $1, (%[addr])\n\t"	/* this is the one that faults */
		"1:\n\t"
		: [rec] "=m"(trap_recovery), [exp] "=m"(trap_expecting)
		: [addr] "r"(addr)
		: "rax", "memory");

	/* Cleared by the handler on the way through, and again here for the case
	 * where no fault happened at all -- which the caller reports as its own
	 * kind of failure. */
	trap_expecting = false;
	return true;
}

void trap_init(void)
{
	kmemset(idt, 0, sizeof(idt));

	gdt_ptr.limit = sizeof(x86_gdt) - 1;
	gdt_ptr.base  = (u64)(uintptr_t)x86_gdt;

	/* Selector 0x08: the second entry of our GDT, which is the code
	 * segment. Written as a constant rather than read from CS, because the
	 * point is to stop depending on whatever GDT we inherited. */
	for (unsigned v = 0; v < 32; v++)
		set_gate(v, stubs[v], 0x08);
	for (unsigned i = 0; i < 16; i++)
		set_gate(32 + i, irq_stubs[i], 0x08);

	set_gate(VECTOR_APIC_TIMER, vector_64, 0x08);
	set_gate(VECTOR_TLB_SHOOTDOWN, vector_65, 0x08);
	set_gate(VECTOR_MSI, vector_66, 0x08);
	set_gate(VECTOR_SPURIOUS, vector_255, 0x08);

	idt_ptr.limit = sizeof(idt) - 1;
	idt_ptr.base  = (u64)(uintptr_t)idt;

	x86_load_tables(&gdt_ptr, &idt_ptr);
}

/* The same two tables, on a processor that has just started.
 *
 * Not a second trap_init: the descriptors are the machine's, built once, and
 * rebuilding them per processor would mean eight chances to build them
 * differently. What is per-processor is only that each one must be *told* --
 * a secondary begins on whatever GDT the trampoline left it with and with no
 * IDT at all, so the first fault it takes without this would triple-fault. */
void x86_load_tables_this_cpu(void)
{
	x86_load_tables(&gdt_ptr, &idt_ptr);
}

/* --- walking the stack ----------------------------------------------------
 *
 * The System V ABI's frame layout, which `-fno-omit-frame-pointer` guarantees:
 * a function pushes the caller's RBP and sets RBP to point at it, so [RBP] is
 * the caller's frame pointer and [RBP+8] is the return address into the caller.
 *
 * Nothing here validates anything. That is deliberate and it is the division of
 * labour the header describes: the *rules* about which frames may be followed
 * are the same on both architectures and live in one place, and this only knows
 * the layout. A second copy of the validation here would be a second copy to
 * get wrong.
 */
u64 arch_frame_pointer(void)
{
	u64 rbp;

	__asm__ volatile("movq %%rbp, %0" : "=r"(rbp));
	return rbp;
}

bool arch_frame_step(u64 frame, u64 *next, u64 *return_address)
{
	const u64 *f = (const u64 *)(uintptr_t)frame;

	if (!frame || !next || !return_address)
		return false;

	*next = f[0];
	*return_address = f[1];
	return true;
}
