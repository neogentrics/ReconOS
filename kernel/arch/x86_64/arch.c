#include <recon/kernel/console.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/arch.h>
#include <recon/kernel/boot.h>
#include <recon/kernel/kstring.h>

#include "x86_64.h"

#include <recon/kernel/smbios.h>

/* Port I/O is in x86_64.h, which this already includes. */
/* --- 16550 UART -------------------------------------------------------- */

/* COM1. Fixed at this port since the IBM PC, and still where QEMU puts it,
 * which is why serial is the one output that works before anything else does. */
#define COM1 0x3F8

#define UART_DATA        0
#define UART_INT_ENABLE  1
#define UART_FIFO_CTRL   2
#define UART_LINE_CTRL   3
#define UART_MODEM_CTRL  4
#define UART_LINE_STATUS 5

#define UART_LSR_THR_EMPTY (1u << 5)

static void uart_init(void)
{
	outb(COM1 + UART_INT_ENABLE, 0x00); /* no interrupts: nothing services them yet */
	outb(COM1 + UART_LINE_CTRL, 0x80);  /* DLAB: the next two ports are the divisor */
	outb(COM1 + UART_DATA, 0x01);       /* divisor 1 -> 115200 baud */
	outb(COM1 + UART_INT_ENABLE, 0x00);
	outb(COM1 + UART_LINE_CTRL, 0x03);  /* 8 bits, no parity, one stop bit */
	outb(COM1 + UART_FIFO_CTRL, 0xC7);  /* enable and clear FIFOs, 14-byte trigger */
	outb(COM1 + UART_MODEM_CTRL, 0x0B); /* DTR, RTS, OUT2 */
}

/* --- Contract ---------------------------------------------------------- */

const char *arch_name(void)
{
	return "x86_64";
}

/* E820's type numbers, which both Multiboot2 and PVH pass through unchanged.
 * Anything not listed is reserved: an unknown type is memory whose owner we do
 * not know, and the safe reading of that is "not ours". */
void x86_add_e820_region(u64 base, u64 len, u32 e820_type)
{
	enum mem_kind kind;

	switch (e820_type) {
	case 1:  kind = MEM_USABLE;       break;
	case 2:  kind = MEM_RESERVED;     break;
	case 3:  kind = MEM_ACPI_RECLAIM; break;
	case 4:  kind = MEM_ACPI_NVS;     break;
	case 5:  kind = MEM_BAD;          break;
	default: kind = MEM_RESERVED;     break;
	}

	boot_add_region((paddr_t)base, len, kind);
}

void arch_early_init(void)
{
	bool ok = false;

	/* The console first, so that a failure to understand the boot protocol
	 * can be reported rather than merely happening. */
	uart_init();

	switch (boot_protocol) {
	case BOOT_PROTOCOL_MULTIBOOT2:
		/* The magic number is the loader's assertion that it really did
		 * follow the specification. Checked, because a wrong value means
		 * the pointer in the other register is not what we think. */
		if (boot_magic == MULTIBOOT2_BOOTLOADER_MAGIC)
			ok = mb2_parse(boot_info_phys);
		break;
	case BOOT_PROTOCOL_PVH:
		ok = pvh_parse(boot_info_phys);
		break;
	case BOOT_PROTOCOL_RECONBOOT:
		/* Our own loader. No magic number to check beyond the one in the
		 * structure itself, because the structure is the only thing that
		 * was passed -- and unlike the other two, we wrote the code on
		 * both sides of this handoff. */
		ok = reconboot_parse((paddr_t)reconboot_handoff);
		break;
	default:
		break;
	}

	if (!ok) {
		/* No memory map. Say so, and record nothing rather than invent
		 * something -- a kernel that guesses at what memory exists is a
		 * kernel that will corrupt whatever it guessed wrong about. */
		boot_info_reset("unrecognised", BOOT_FIRMWARE_UNKNOWN);
	}

	boot_finish_regions();
}

void arch_console_putc(char c)
{
	while (!(inb(COM1 + UART_LINE_STATUS) & UART_LSR_THR_EMPTY))
		;
	outb(COM1 + UART_DATA, (u8)c);
}


void arch_halt(void)
{
	for (;;)
		__asm__ volatile("cli; hlt");
}

void arch_wait_for_interrupt(void)
{
	__asm__ volatile("hlt");
}

/* --- Processors and interrupts -------------------------------------------- */

unsigned arch_cpu_id(void)
{
	/* It reads the local APIC now, which is what this comment promised at
	 * checkpoint 9 it would do once there were other processors. Every
	 * caller kept working, which is the reason it was a function from the
	 * start rather than a zero written at each call site.
	 *
	 * The APIC's identifier is not the kernel's processor number -- see
	 * smp.c, where the two are kept apart deliberately. */
	return x86_cpu_index();
}

u64 arch_irq_save(void)
{
	u64 flags;

	__asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
	return flags;
}

void arch_irq_restore(u64 flags)
{
	/* Restores the whole flags register, which puts the interrupt bit back
	 * exactly as it was -- masked stays masked. An unconditional `sti` here
	 * would silently enable interrupts inside an outer critical section that
	 * had deliberately masked them. */
	__asm__ volatile("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

bool arch_irqs_enabled(void)
{
	u64 flags;

	__asm__ volatile("pushfq; popq %0" : "=r"(flags));
	return (flags & (1ULL << 9)) != 0;	/* IF */
}

void arch_cpu_relax(void)
{
	__asm__ volatile("pause");
}

/* --- Finding the SMBIOS anchor -------------------------------------------
 *
 * On a PC it is somewhere in the sixty-four kilobytes below one megabyte, on a
 * sixteen-byte boundary, and the only way to find it is to look. That region is
 * firmware ROM and shadow RAM; it is marked reserved in every memory map this
 * kernel has ever been handed, so reading it is safe and allocating from it
 * never happens.
 *
 * The 3.0 anchor is checked for first. A machine that has both publishes the
 * older one for software that only knows the older one, and the newer carries a
 * 64-bit table address -- so preferring the older would work everywhere and
 * would silently truncate on a machine that put its tables high.
 *
 * On a UEFI machine the firmware also passes this as a configuration table, and
 * that would be the better source: it needs no scan and it is correct on a
 * machine whose legacy region is not populated. It would mean carrying the
 * address through the handoff, which is a protocol version bump -- see
 * reconboot.h, which is emphatic about what raising that number costs. The scan
 * works on every machine tested; when one turns up where it does not, that is
 * the fix and this comment is the reason it was not done first.
 */
#define SMBIOS_SCAN_START	0x000F0000u
#define SMBIOS_SCAN_END		0x00100000u

paddr_t arch_smbios_anchor(void)
{
	paddr_t at;
	vaddr_t window = (vaddr_t)(uintptr_t)phys_to_virt(SMBIOS_SCAN_START);

	/* Mapped before it is read, and this is not a formality.
	 *
	 * The direct map covers the regions the *firmware's memory map*
	 * described. On the paravirtual and BIOS paths low memory is in that
	 * map and this address happens to be reachable; under UEFI the legacy
	 * ROM region is not described at all, so the same address is not
	 * mapped and reading it is a page fault in the middle of boot.
	 *
	 * Which is exactly what it was: five boot paths failed and every one of
	 * them went through firmware, while every path loaded directly passed.
	 * A region that is readable on the machine you tested on and absent on
	 * the next one is what a boot-path matrix is for.
	 *
	 * Read-only, because nothing here writes, and a mapping that cannot be
	 * written is one fewer thing a stray pointer can damage. */
	if (!vm_lookup(window) &&
	    !vm_map(window, SMBIOS_SCAN_START,
		    SMBIOS_SCAN_END - SMBIOS_SCAN_START,
		    VM_READ | VM_GLOBAL))
		return 0;

	/* Sixteen-byte aligned, which the specification requires and which
	 * makes this four thousand comparisons rather than sixty-four
	 * thousand. */
	for (at = SMBIOS_SCAN_START; at < SMBIOS_SCAN_END; at += 16) {
		const u8 *p = phys_to_virt(at);

		if (p[0] == '_' && p[1] == 'S' && p[2] == 'M' &&
		    p[3] == '3' && p[4] == '_')
			return at;
	}

	for (at = SMBIOS_SCAN_START; at < SMBIOS_SCAN_END; at += 16) {
		const u8 *p = phys_to_virt(at);

		if (p[0] == '_' && p[1] == 'S' && p[2] == 'M' && p[3] == '_')
			return at;
	}

	return 0;
}

/* --- The vector unit ------------------------------------------------------
 *
 * SSE2, which every x86_64 processor has by definition -- it is part of the
 * architecture rather than an extension to it, which is why this needs no
 * capability check while AVX would.
 *
 * Four bits, and each one matters:
 *
 *   CR0.EM off. "Emulate": with it set, every vector instruction raises an
 *   exception so that software can emulate it. Leaving it on is the setting
 *   that makes a vector instruction look like an invalid opcode.
 *
 *   CR0.MP on. "Monitor coprocessor", which makes WAIT/FWAIT respect the
 *   task-switched flag rather than ignoring it.
 *
 *   CR4.OSFXSR on. This is the one that says *the operating system saves this
 *   state on a context switch*. The processor takes it at its word: setting it
 *   without saving is precisely the corruption this flag exists to promise
 *   against.
 *
 *   CR4.OSXMMEXCPT on, so a vector arithmetic fault arrives as #XF rather than
 *   as an invalid opcode, which is the difference between a fault report that
 *   names the problem and one that does not.
 *
 * AVX is deliberately **not** enabled. It needs XSAVE, whose save area is
 * sized by a CPUID query and differs between machines, so the fixed per-thread
 * area here could not hold it. Enabling it anyway would set OSXSAVE -- another
 * promise to save state -- and then not save it. */
/* EM_X86_64, fixed by the ABI. A number rather than a header constant because
 * there is no ELF header to include here and inventing one for a single value
 * is more code than the value. */
/* Nothing of its own. On this architecture turning the machine off *is* ACPI:
 * the register is in the FADT and the value is in the vendor's bytecode, and
 * there is no second mechanism to prefer. Saying so costs one function and
 * keeps the decision in one place. */
bool arch_power_off(void)
{
	return false;
}

unsigned arch_elf_machine(void)
{
	return 62;
}

void arch_vector_enable(void)
{
	u64 cr0, cr4;

	__asm__ volatile("movq %%cr0, %0" : "=r"(cr0));
	cr0 &= ~(1ull << 2);		/* EM off */
	cr0 |=  (1ull << 1);		/* MP on */

	/* WP: A READ-ONLY PAGE IS READ-ONLY TO THE KERNEL AS WELL.
	 *
	 * Without this bit, ring 0 may write through any mapping regardless of
	 * its read-only bit, and the processor does not object. Nothing in this
	 * kernel had ever set it: boot.S clears it for four instructions and
	 * puts back whatever was there, so its value was *whatever firmware
	 * left behind* -- set under OVMF, which is why boot.S had to clear it
	 * at all, and clear on the paths that boot with no firmware.
	 *
	 * So this was a difference in behaviour between boot paths that no
	 * test could see, and it became a correctness problem the moment
	 * copy-on-write existed: a shared page is kept shared by being mapped
	 * read-only, and a kernel that can write through read-only mappings
	 * writes into the page every other program is reading. No fault, no
	 * message, and every page table correct.
	 *
	 * Found by the address-space self-test, which reads the shared page
	 * back after the write and requires it still to be zero. (KF-145)
	 */
	cr0 |=  (1ull << 16);		/* WP on */
	__asm__ volatile("movq %0, %%cr0" : : "r"(cr0) : "memory");

	__asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
	cr4 |= (1ull << 9) | (1ull << 10);	/* OSFXSR, OSXMMEXCPT */
	__asm__ volatile("movq %0, %%cr4" : : "r"(cr4) : "memory");

	/* A defined starting state, so the first thread to look does not read
	 * whatever firmware left in the registers. */
	__asm__ volatile("fninit");
}

void arch_vector_save(void *area)
{
	__asm__ volatile("fxsave64 (%0)" : : "r"(area) : "memory");
}

void arch_vector_restore(const void *area)
{
	__asm__ volatile("fxrstor64 (%0)" : : "r"(area) : "memory");
}

/* ACPI's control registers are I/O ports on this architecture. Sixteen bits,
 * because that is the width the power management control register is defined
 * to be -- writing it as two bytes would deliver the low half first and act on
 * a half-formed value. */
bool arch_acpi_write_control(u64 address, u16 value)
{
	if (!address || address > 0xFFFF)
		return false;

	__asm__ volatile("outw %0, %1"
			 : : "a"(value), "Nd"((u16)address));
	return true;
}
