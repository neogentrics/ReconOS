/* The architecture contract.
 *
 * This header is the whole boundary between the portable kernel and the
 * machine it runs on. Every architecture under kernel/arch/ implements
 * exactly these functions and nothing in core/ may reach past them.
 *
 * The rule that keeps it honest: no file under core/ may contain the string
 * "x86", "aarch64", inline assembly, or a hardware address. If a portable
 * file needs something machine-specific, the answer is a new function here,
 * not an #ifdef there. `make check-portable` enforces this.
 *
 * It is deliberately tiny right now. It grows one function at a time, as the
 * kernel actually needs the capability -- an interface invented before
 * anything wants it gets fixed in place before it is understood.
 */
#ifndef RECON_KERNEL_ARCH_H
#define RECON_KERNEL_ARCH_H

#include <recon/kernel/types.h>
#include <recon/kernel/compiler.h>

/* --- Identity ---------------------------------------------------------- */

/* Compile-time name of the architecture: "x86_64", "aarch64", ... */
const char *arch_name(void);

/* What this particular CPU is and what it can do, read at run time.
 *
 * Note the distinction, because it matters for the install story: *which*
 * architecture is not something the kernel discovers -- the firmware already
 * decided that when it chose which binary to load. What is discovered at run
 * time is which CPU *within* that architecture, and which features it has --
 * and that is the difference between using a machine and using the least
 * capable machine that could have run this code. */
struct cpu_caps;
void arch_cpu_caps(struct cpu_caps *out);

/* --- Bring-up ---------------------------------------------------------- */

/* Called first thing from kmain(), before anything else. Puts the CPU in a
 * known state and makes arch_console_putc() work. Must not allocate, must not
 * assume memory beyond the kernel image is usable. */
void arch_early_init(void);

/* --- Console ----------------------------------------------------------- */

/* Write one byte to the architecture's earliest possible output: the serial
 * port on x86_64, the PL011 UART on aarch64. This exists so that a kernel
 * which has nothing else working can still say what went wrong. */
void arch_console_putc(char c);

/* --- Time -----------------------------------------------------------------
 *
 * Three functions rather than one, because a machine has two clocks and they
 * answer different questions -- see time.h. The tick is set up here too, since
 * arranging a periodic interrupt means naming a timer and an interrupt
 * controller, and both are machine-specific in every detail. */

/* Starts the counter, calibrates it, and arranges a periodic interrupt at
 * TIME_TICK_HZ. Interrupts are enabled by the time this returns. */
void arch_time_init(void);

/* Nanoseconds since arch_time_init(). Never decreases. */
u64 arch_monotonic_ns(void);

/* Nanoseconds since 1970-01-01 UTC, or 0 where the machine has no clock the
 * kernel can read. Zero is a real answer, and better than a plausible wrong
 * one. */
u64 arch_wall_ns(void);

/* --- Processors and interrupts --------------------------------------------
 *
 * Added for locking, which needs three things no portable code can express:
 * which processor is asking, whether interrupts are on, and how to wait
 * politely. */

/* This processor's number. Zero on a machine with one, and zero until the
 * others are woken. */
unsigned arch_cpu_id(void);

/* Masks interrupts and returns how they were, for arch_irq_restore(). Saving
 * and restoring rather than disabling and enabling, because a lock taken inside
 * another lock must not re-enable interrupts the outer one masked. */
u64  arch_irq_save(void);
void arch_irq_restore(u64 flags);
bool arch_irqs_enabled(void);

/* A hint that this is a spin loop: `pause` on x86_64, `yield` on aarch64.
 * Without it a spinning processor draws as much power as a working one. */
void arch_cpu_relax(void);

/* Puts device interrupts wherever this machine can best deliver them, once
 * every processor is running. On x86_64 that means moving the ISA lines off
 * the 8259 -- which has one output, wired to one processor -- and onto the I/O
 * APIC, which can name any of them. On aarch64 the interrupt controller
 * already distributes and there is nothing to move.
 *
 * Called after smp_init, because the destination is a processor and there is
 * no point choosing one before they exist. */
void arch_irq_route_init(void);
void arch_irq_print_summary(void);

/* The part of that summary which is about devices, and therefore cannot be
 * printed until the buses have been walked. Separate from the above because
 * the two are true at different moments, not because they are about different
 * things. */
void arch_irq_print_device_summary(void);

/* That the machine's way of signalling an interrupt without a wire actually
 * signals one. On x86_64 that is an MSI written to the local APIC's window.
 */
bool arch_irq_self_test(void);

struct pci_device;

/* Asks this machine to deliver one PCI device's interrupt to a handler.
 *
 * `entry` is which of the device's messages -- a device with several queues has
 * several, numbered by the device rather than by this kernel.
 *
 * False means the machine or the device has no way to do it, which is not a
 * failure: it means that device signals the old way, and a driver that gets
 * false should carry on polling rather than wait for something that will not
 * come. There is deliberately no call to give one back, because nothing in
 * this kernel detaches a driver yet -- when something does, this is where the
 * other half goes. */
bool arch_pci_request_interrupt(const struct pci_device *d, unsigned entry,
				void (*fn)(void *), void *arg,
				const char *name);

/* --- Control ----------------------------------------------------------- */

/* Stop this CPU forever, with interrupts masked. Used by panic(). */
RK_NORETURN void arch_halt(void);

/* Wait for the next interrupt without burning the CPU. The idle loop's whole
 * body -- this is where the "nothing polls" principle is paid for in silicon
 * rather than promised in a document. */
void arch_wait_for_interrupt(void);

/* --- The vector unit ------------------------------------------------------
 *
 * Enabled per processor, and saved per thread. Both halves are required
 * together: a unit that is on and not saved is two threads sharing arithmetic
 * registers, which corrupts results rather than crashing and does it only when
 * two threads happen to use them at once.
 *
 * `arch_vector_enable` is called by every processor on itself, because the
 * control registers that turn it on are per-processor. The save and restore
 * take the 512-byte aligned area on each thread. */
void arch_vector_enable(void);

/* The `e_machine` value an ELF must carry to run on this processor.
 *
 * Asked rather than written into the loader, because core/ does not name
 * machines -- and because getting it wrong is not a build error: a kernel that
 * accepted the other architecture's number would map somebody else's
 * instructions and jump into them. 62 is x86-64 and 183 is AArch64, and both
 * are fixed by the ABI rather than by anything here. */
unsigned arch_elf_machine(void);

/* Turns the machine off by whatever means this architecture has of its own,
 * and does not return if it works. False means "not mine" -- there is no
 * architecture-native way here and the caller should try the portable one.
 *
 * It exists because power_off() is written against ACPI, which is how an x86
 * machine is turned off and is simply absent on an ARM machine booted from a
 * device tree. That kernel could pass every test and then sit there with the
 * fans running, which is what a person calls broken. */
bool arch_power_off(void);
void arch_vector_save(void *area);
void arch_vector_restore(const void *area);

/* Writes one of ACPI's platform control registers.
 *
 * On a PC these are I/O ports, which only one architecture has. Returns false
 * where there is no such thing, which is how a machine that cannot be powered
 * off this way says so rather than appearing to succeed.
 *
 * The address is the one the FADT gave, and only the I/O-port form is handled:
 * the specification also allows these registers to be memory-mapped, and a
 * machine that does that is a machine this returns false for. */
bool arch_acpi_write_control(u64 address, u16 value);

#endif /* RECON_KERNEL_ARCH_H */
