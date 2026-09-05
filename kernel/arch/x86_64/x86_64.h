/* Internal to arch/x86_64. Nothing above arch/ includes this. */
#ifndef RECON_ARCH_X86_64_H
#define RECON_ARCH_X86_64_H

#include <recon/kernel/types.h>

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
extern u64 x86_gdt[7];

#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UDATA 0x18
#define SEL_UCODE 0x20
#define SEL_TSS   0x28

/* Model-specific registers this architecture's code reaches for by name. */
#define MSR_EFER            0xC0000080u
#define MSR_STAR            0xC0000081u
#define MSR_LSTAR           0xC0000082u
#define MSR_FMASK           0xC0000084u
#define MSR_GS_BASE         0xC0000101u
#define MSR_KERNEL_GS_BASE  0xC0000102u

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

/* E820's numbering, which both protocols above borrow. */
void x86_add_e820_region(u64 base, u64 len, u32 e820_type);

#endif /* RECON_ARCH_X86_64_H */
