/* Internal to arch/x86_64. Nothing above arch/ includes this. */
#ifndef RECON_ARCH_X86_64_H
#define RECON_ARCH_X86_64_H

#include <recon/kernel/acpi.h>
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
void x86_pic_mask_all(void);
void x86_pic_restore_default(void);

/* Acknowledges a device interrupt to whichever controller delivered it. Every
 * handler calls this rather than naming a chip. */
void x86_irq_ack(unsigned irq);

/* The MADT, shared because two files walk it: smp.c for the processors and
 * ioapic.c for the interrupt routing. One declaration, because two would be a
 * layout agreed by hand between two files. */
struct madt {
	struct acpi_sdt_header header;
	u32 local_apic_address;
	u32 flags;
	/* entries follow */
} RK_PACKED;

/* --- the I/O APIC ------------------------------------------------------- */

/* Finds and quiets every I/O APIC the firmware describes. False if there are
 * none, or none that could be mapped, in which case the 8259 keeps the lines
 * and the machine works exactly as it did. */
bool x86_ioapic_init(void);
bool x86_ioapic_present(void);

/* Whether device interrupts are actually coming through it. Not the same
 * question as `present`: a chip that was found and could not be armed is
 * present and not in use, and the acknowledgement path depends on which. */
bool x86_ioapic_in_use(void);

bool x86_ioapic_route_isa(unsigned irq, u8 vector, u32 destination);
bool x86_ioapic_take_over(void);
void x86_ioapic_print_summary(void);

/* --- message-signalled interrupts --------------------------------------- */

struct pci_device;

/* The address and data a device writes to raise `vector` on `destination`. */
void x86_msi_compose(u8 vector, u32 destination, u64 *address, u32 *data);

/* Programs that message into a device's MSI capability and turns it on.
 * False if the device has no such capability, which is not an error. */
bool x86_msi_enable(const struct pci_device *d, u8 vector, u32 destination);

unsigned x86_msi_capable_devices(bool *any_msix);
void x86_msi_print_summary(void);
bool x86_msi_self_test(void);

extern volatile unsigned x86_msi_test_arrivals;
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

/* Whether this processor is using 32-bit x2APIC identifiers rather than the
 * 8-bit ones of the memory-mapped block. */
bool x86_apic_is_x2(void);

/* The x2APIC addressing arithmetic, exposed for the self-test: which MSR a
 * register offset becomes, and how a destination and a command combine into
 * the 64-bit interrupt command. Neither reads or writes hardware. */
u32 x86_apic_msr_for(unsigned reg);
u64 x86_apic_command_word(u32 target, u32 command);
void x86_apic_eoi(void);
void x86_apic_send_init(u32 target);
void x86_apic_send_startup(u32 target, u8 page);
void x86_apic_send_ipi_all_but_self(u8 vector);
void x86_apic_calibrate_timer(void);	/* once, against a clock already trusted */
void x86_apic_start_timer(void);	/* per processor */
bool x86_apic_timer_ready(void);

/* The vectors the APIC raises. Above the sixteen the 8259 occupies, so the two
 * can coexist while there is no I/O APIC to replace it. */
#define VECTOR_APIC_TIMER	0x40
#define VECTOR_TLB_SHOOTDOWN	0x41

/* Where a message-signalled interrupt lands. One vector, because there is one
 * caller: the self-test, which writes an MSI to the local APIC to prove the
 * encoding. A real device would be given a vector out of an allocator, which
 * arrives with the first driver that asks for one. */
#define VECTOR_MSI		0x42
#define VECTOR_SPURIOUS		0xFF

/* In smp.c: this processor's kernel index, which is not its APIC identifier. */
unsigned x86_cpu_index(void);

/* In trap.c: the machine's two descriptor tables, on a processor that has just
 * started and has neither. */
void x86_load_tables_this_cpu(void);

/* Points the double-fault and NMI gates at this processor's dedicated stacks.
 * Called once the TSS holding those stacks is live, and not before. */
void x86_arm_fault_stacks(void);

/* In vm.c: honours another processor's request to drop a translation, and how
 * many such requests have gone unanswered. The count is reported rather than
 * kept, because a shootdown that timed out means a processor somewhere may
 * still be using a mapping that no longer exists. */
/* In vm.c: programs this processor's page attribute table. Must run before any
 * mapping that asks for write-combining, and on every processor. */
void x86_pat_init(void);

void x86_tlb_shootdown_service(void);
unsigned x86_tlb_shootdown_timeouts(void);
unsigned x86_tlb_shootdowns_sent(void);

/* E820's numbering, which both protocols above borrow. */
void x86_add_e820_region(u64 base, u64 len, u32 e820_type);

#endif /* RECON_ARCH_X86_64_H */
