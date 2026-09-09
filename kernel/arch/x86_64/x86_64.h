/* Internal to arch/x86_64. Nothing above arch/ includes this. */
#ifndef RECON_ARCH_X86_64_H
#define RECON_ARCH_X86_64_H

#include <recon/kernel/types.h>
#include <recon/kernel/smp.h>

/* Where the kernel image is linked, and where boot.S puts the processor before
 * any C runs. The linker script holds the same number; they are two statements
 * of one fact and must not drift. See linker.ld for why this address and not
 * another -- -mcmodel=kernel does not offer a choice. */
#define KERNEL_VMA 0xFFFFFFFF80000000ULL

/* Set by boot.S before anything else runs. */
extern u32 boot_protocol;	/* 1 = Multiboot2, 2 = PVH */
extern u32 boot_info_phys;
extern u32 boot_magic;

#define BOOT_PROTOCOL_MULTIBOOT2 1
#define BOOT_PROTOCOL_PVH        2
#define BOOT_PROTOCOL_RECONBOOT  3

/* Set by reconboot_entry from RDI. Zero on every other path. */
extern u64 reconboot_handoff;

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289u
#define PVH_START_INFO_MAGIC        0x336ec578u

/* In time.c: the tick, telling the interrupt controller an interrupt was
 * handled, and what the console reports about the clock. */
void x86_timer_interrupt(void);
void x86_apic_timer_interrupt(void);	/* the same tick, on a secondary */
void x86_pic_end_of_interrupt(unsigned irq);
void x86_time_print_source(void);

/* Each translates one boot protocol's account of the machine into the kernel's
 * own. Returns false if the structure did not look like what it claimed. */
bool mb2_parse(u32 info_phys);
bool pvh_parse(u32 info_phys);

/* The descriptor table, and the selectors that index it. Shared with user.c,
 * which fills the task-state segment entry and needs the user selectors.
 *
 * See trap.c for why the order is what it is: SYSRET derives both user
 * selectors by adding 8 and 16 to a single base. */
extern u64 x86_gdt[5 + 2 * MAX_CPUS];

#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UDATA 0x18
#define SEL_UCODE 0x20
#define SEL_TSS   0x28

/* Each processor has a task-state segment of its own, sixteen bytes further
 * along than the one before it. Sharing one would share the stack a system call
 * lands on, which two processors cannot do. */
#define SEL_TSS_FOR(cpu) (SEL_TSS + (u16)((cpu) * 16))

/* Model-specific registers this architecture's code reaches for by name. */
#define MSR_EFER            0xC0000080u
#define MSR_STAR            0xC0000081u
#define MSR_LSTAR           0xC0000082u
#define MSR_FMASK           0xC0000084u
#define MSR_GS_BASE         0xC0000101u
#define MSR_KERNEL_GS_BASE  0xC0000102u

/* The page tables this processor is using. Read rather than remembered,
 * because a secondary must be started on the tables that are live now -- not
 * on whatever address was recorded when they were built. */
static inline u64 x86_read_cr3(void)
{
	u64 value;

	__asm__ volatile("movq %%cr3, %0" : "=r"(value));
	return value;
}

static inline u64 x86_rdmsr(u32 msr)
{
	u32 lo, hi;

	__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((u64)hi << 32) | lo;
}

static inline void x86_wrmsr(u32 msr, u64 value)
{
	__asm__ volatile("wrmsr"
			 :
			 : "a"((u32)value), "d"((u32)(value >> 32)), "c"(msr));
}

/* --- the local APIC, in apic.c -------------------------------------------
 *
 * Arrives with checkpoint 9b, because nothing needed it until there was another
 * processor to talk to. The two chips the PC shipped with are singular: one
 * 8259 and one 8254, delivering to one processor. */
bool x86_apic_init(void);		/* per processor: each APIC is its own */
bool x86_apic_present(void);
u32  x86_apic_id(void);
void x86_apic_eoi(void);
void x86_apic_send_init(u32 target);
void x86_apic_send_startup(u32 target, u8 page);
void x86_apic_calibrate_timer(void);	/* once, against a clock already trusted */
void x86_apic_start_timer(void);	/* per processor */
bool x86_apic_timer_ready(void);

/* The vectors the APIC raises. Above the sixteen the 8259 occupies, so the two
 * can coexist while there is no I/O APIC to replace it. */
#define VECTOR_APIC_TIMER	0x40
#define VECTOR_SPURIOUS		0xFF

/* In smp.c: this processor's kernel index, which is not its APIC identifier. */
unsigned x86_cpu_index(void);

/* In trap.c: the machine's two descriptor tables, on a processor that has just
 * started and has neither. */
void x86_load_tables_this_cpu(void);

/* Points the double-fault and NMI gates at this processor's dedicated stacks.
 * Called once the TSS holding those stacks is live, and not before. */
void x86_arm_fault_stacks(void);

/* E820's numbering, which both protocols above borrow. */
void x86_add_e820_region(u64 base, u64 len, u32 e820_type);

#endif /* RECON_ARCH_X86_64_H */
