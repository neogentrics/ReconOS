/* aarch64 exception handling.
 *
 * ARM tells you far more about a fault than x86 does, and in one register.
 * ESR_EL1's top six bits are the *exception class* -- data abort, instruction
 * abort, illegal instruction, system call -- and the rest is a description
 * specific to that class. For an abort, that includes whether it was a read or
 * a write, which translation level failed, and whether it was a permission
 * fault or an absent translation.
 *
 * Decoding it properly is most of this file, and it is worth the space: the
 * difference between "data abort" and "level 3 translation fault on a write to
 * this address" is the difference between an afternoon and a minute.
 */
#include "aarch64.h"

#include <recon/kernel/trap.h>
#include <recon/kernel/console.h>
#include <recon/kernel/panic.h>

/* Must match the layout vectors.S builds, exactly. */
struct trap_frame {
	u64 x[31];	/* x0 holds the vector slot index, not the original x0 */
	u64 elr;	/* where it faulted; writable, to resume elsewhere */
	u64 spsr;
	u64 esr;
	u64 far;
};

extern char exception_vectors[];

static const char *const slot_name[16] = {
	"synchronous (current EL, SP0)", "IRQ (current EL, SP0)",
	"FIQ (current EL, SP0)", "SError (current EL, SP0)",
	"synchronous", "IRQ", "FIQ", "SError",
	"synchronous from a lower EL (64-bit)", "IRQ from a lower EL (64-bit)",
	"FIQ from a lower EL (64-bit)", "SError from a lower EL (64-bit)",
	"synchronous from a lower EL (32-bit)", "IRQ from a lower EL (32-bit)",
	"FIQ from a lower EL (32-bit)", "SError from a lower EL (32-bit)",
};

static const char *exception_class(unsigned ec)
{
	switch (ec) {
	case 0x00: return "unknown";
	case 0x0E: return "illegal execution state";
	case 0x15: return "system call (SVC)";
	case 0x18: return "trapped system register access";
	case 0x20: return "instruction abort from a lower EL";
	case 0x21: return "instruction abort";
	case 0x22: return "misaligned instruction fetch";
	case 0x24: return "data abort from a lower EL";
	case 0x25: return "data abort";
	case 0x26: return "stack pointer misaligned";
	case 0x2C: return "floating-point exception";
	case 0x30: return "breakpoint";
	case 0x3C: return "software breakpoint (BRK)";
	default:   return "unrecognised exception class";
	}
}

/* The low six bits of ESR for an abort: what went wrong and at which level of
 * the translation table walk. The level is genuinely useful -- it says how far
 * the hardware got before giving up, which points at which table was wrong. */
static const char *abort_reason(unsigned iss)
{
	switch (iss & 0x3F) {
	case 0x00: case 0x01: case 0x02: case 0x03:
		return "address size fault";
	case 0x04: return "translation fault, level 0";
	case 0x05: return "translation fault, level 1";
	case 0x06: return "translation fault, level 2";
	case 0x07: return "translation fault, level 3 (nothing is mapped there)";
	case 0x09: return "access flag fault, level 1";
	case 0x0A: return "access flag fault, level 2";
	case 0x0B: return "access flag fault, level 3 (the access flag was not set)";
	case 0x0D: return "permission fault, level 1";
	case 0x0E: return "permission fault, level 2";
	case 0x0F: return "permission fault, level 3 (mapped, but not like that)";
	case 0x10: return "external abort";
	case 0x21: return "alignment fault";
	default:   return "unrecognised fault";
	}
}

void trap_dispatch(struct trap_frame *f)
{
	unsigned ec = (unsigned)(f->esr >> 26);
	unsigned iss = (unsigned)(f->esr & 0x1FFFFFF);

	/* Slots 1, 5, 9 and 13 are IRQ. Five is the one kernel interrupts arrive
	 * on -- current exception level, on its own stack. */
	if ((f->x[0] & 3) == 1) {
		aarch64_irq();
		return;
	}

	if (trap_expecting) {
		trap_caught = true;
		trap_expecting = false;
		trap_note_catch();
		f->elr = (u64)(uintptr_t)trap_recovery;
		return;
	}

	kputs("\n=== ReconOS kernel fault ===\n");
	kprintf("  exception    : %s\n", slot_name[f->x[0] & 15]);
	kprintf("  class        : %s\n", exception_class(ec));

	if (ec == 0x24 || ec == 0x25) {
		/* Bit 6 of a data abort's syndrome is the direction, and it is
		 * valid on its own. The first version gated it on bit 24, which
		 * says whether the *other* syndrome fields are valid -- so every
		 * report said the direction was unknown when the register was
		 * sitting there holding it. Reading a specification's
		 * "valid" bit as covering more than it covers is its own small
		 * class of bug. */
		kprintf("  touched      : %p\n", (void *)(uintptr_t)f->far);
		kprintf("  what happened: %s, on a %s\n", abort_reason(iss),
			(iss & (1u << 6)) ? "write" : "read");
	} else if (ec == 0x20 || ec == 0x21) {
		/* An instruction abort: the address is the one it tried to
		 * fetch from, and there is no direction to report. */
		kprintf("  fetching from: %p\n", (void *)(uintptr_t)f->far);
		kprintf("  what happened: %s\n", abort_reason(iss));
	}

	kprintf("  at           : %p\n", (void *)(uintptr_t)f->elr);
	kprintf("  esr          : %p\n", (void *)(uintptr_t)f->esr);

	/* kprintf supports no field widths, so the register numbers are padded
	 * by hand. The alternative was to add widths to the formatter for the
	 * sake of one caller. */
	for (unsigned i = 1; i < 31; i += 2)
		kprintf("  x%s%u %p   x%s%u %p\n",
			i < 10 ? " " : "", i, (void *)(uintptr_t)f->x[i],
			(i + 1) < 10 ? " " : "", i + 1,
			(void *)(uintptr_t)f->x[i + 1]);

	panic("unhandled exception");
}

/* Faults on purpose, and resumes at the instruction after the one that faulted.
 *
 * The resume label is inside the asm block with the faulting store, so nothing
 * can come between them and no optimiser can move or delete it. See the note in
 * trap.h for what happened when this was written in portable C. */
bool trap_provoke_fault(void)
{
	unsigned long addr = trap_test_address;

	__asm__ volatile(
		"adr	x2, 1f\n\t"
		"str	x2, [%[rec]]\n\t"
		"mov	w2, #1\n\t"
		"strb	w2, [%[exp]]\n\t"
		"str	w2, [%[addr]]\n\t"	/* this is the one that faults */
		"1:\n\t"
		:
		: [rec] "r"(&trap_recovery), [exp] "r"(&trap_expecting),
		  [addr] "r"(addr)
		: "x2", "memory");

	trap_expecting = false;
	return true;
}

void trap_init(void)
{
	/* VBAR_EL1 is the base of the table. The instruction barrier matters:
	 * without it the CPU may still be using the old base when the next
	 * exception arrives, and the old base is whatever the firmware left. */
	__asm__ volatile(
		"msr vbar_el1, %0\n"
		"isb\n"
		: : "r"((u64)(uintptr_t)exception_vectors) : "memory");
}
