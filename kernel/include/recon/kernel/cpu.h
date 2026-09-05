/* What this particular processor can do.
 *
 * Not which architecture -- the firmware settled that before we existed -- but
 * what this specific chip offers within it. A 2008 Core 2 and a 2024 Zen 5 are
 * both x86_64 and differ in almost everything that matters for going fast.
 *
 * This is read once at boot and then consulted, because the whole point is to
 * use what the machine has rather than what the slowest machine has. Every
 * field below is here because something is going to branch on it:
 *
 *   large pages      decide how the kernel maps memory, and whether a
 *                    gigabyte costs one page table entry or two hundred
 *                    thousand
 *   hardware random  decides whether the entropy pool has a real source
 *   no-execute       decides whether pages can be made non-executable at all
 *   physical bits    decide how much RAM can be addressed, and bound the
 *                    page tables
 *   core count       bounds the scheduler
 *   crypto           decides whether encryption costs a instruction or a loop
 *
 * Anything that cannot branch on it does not belong here, however interesting
 * it is.
 */
#ifndef RECON_KERNEL_CPU_H
#define RECON_KERNEL_CPU_H

#include <recon/kernel/types.h>

#define CPU_VENDOR_MAX     16
#define CPU_BRAND_MAX      64
#define CPU_EXTENSIONS_MAX 224

struct cpu_caps {
	char vendor[CPU_VENDOR_MAX];	/* who made it */
	char brand[CPU_BRAND_MAX];	/* what it calls itself */

	/* Arithmetic */
	bool fpu;		/* floating point at all */
	bool simd;		/* SSE2 / AdvSIMD -- the baseline vector unit */
	bool simd_wide;		/* AVX / SVE -- worth a different code path */

	/* Cryptography, in silicon rather than in a loop */
	bool aes;
	bool sha;
	bool crc32;

	/* Concurrency */
	bool atomics;		/* the good atomic instructions, not just a CAS loop */

	/* The one the randomness service needs */
	bool hw_random;

	/* Memory */
	bool no_execute;	/* pages can be marked non-executable */
	bool page_2m;		/* large pages */
	bool page_1g;		/* huge pages */
	unsigned phys_addr_bits;
	unsigned virt_addr_bits;

	/* Time */
	bool invariant_timer;	/* the counter does not change rate with clock speed */

	bool virtualization;

	/* Topology. Zero means the CPU declined to say, which is different from
	 * one and must not be rounded to it. */
	unsigned cores;
	unsigned threads;

	/* Everything above, in this architecture's own vocabulary, for showing
	 * to a person. Nothing branches on this string. */
	char extensions[CPU_EXTENSIONS_MAX];
};

void cpu_print_caps(const struct cpu_caps *c);

#endif /* RECON_KERNEL_CPU_H */
