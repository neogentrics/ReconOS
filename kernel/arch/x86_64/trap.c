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

#include <recon/kernel/trap.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/panic.h>

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
 * Null, then 64-bit code, then data -- the minimum a 64-bit kernel needs. */
static u64 gdt[3] RK_ALIGNED(16) = {
	0,
	0x00AF9A000000FFFFULL,	/* code: executable, readable, long mode */
	0x00CF92000000FFFFULL,	/* data: writable */
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
	/* A hardware interrupt rather than a fault. Handled and acknowledged;
	 * an interrupt the controller is not told about is the last one it
	 * ever sends. */
	if (f->vector >= 32 && f->vector < 48) {
		if (f->vector == 32)
			x86_timer_interrupt();
		else
			x86_pic_end_of_interrupt((unsigned)(f->vector - 32));
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

	gdt_ptr.limit = sizeof(gdt) - 1;
	gdt_ptr.base  = (u64)(uintptr_t)gdt;

	/* Selector 0x08: the second entry of our GDT, which is the code
	 * segment. Written as a constant rather than read from CS, because the
	 * point is to stop depending on whatever GDT we inherited. */
	for (unsigned v = 0; v < 32; v++)
		set_gate(v, stubs[v], 0x08);
	for (unsigned i = 0; i < 16; i++)
		set_gate(32 + i, irq_stubs[i], 0x08);

	idt_ptr.limit = sizeof(idt) - 1;
	idt_ptr.base  = (u64)(uintptr_t)idt;

	x86_load_tables(&gdt_ptr, &idt_ptr);
}
