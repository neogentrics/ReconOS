/* What this aarch64 processor offers, read out of the ID registers.
 *
 * ARM does this more honestly than x86. There is no single CPUID instruction:
 * there is a bank of read-only system registers, each a set of four-bit fields,
 * each field saying which *version* of a feature is present rather than
 * whether it is. Zero almost always means absent, and larger means more
 * capable -- so the tests below are "at least", not "equal to", and a future
 * chip reporting a version this code has never heard of is read as supporting
 * the feature rather than as not supporting it.
 *
 * The exception is ID_AA64PFR0_EL1's floating point and SIMD fields, where
 * 0b1111 means absent and everything else means present, because those fields
 * are signed. Getting that backwards would report a CPU with no FPU.
 */
#include "aarch64.h"

#include <recon/kernel/cpu.h>
#include <recon/kernel/kstring.h>

#define READ_SYSREG(name) ({                            \
	u64 _v;                                         \
	__asm__ volatile("mrs %0, " #name : "=r"(_v));  \
	_v;                                             \
})

static inline unsigned field(u64 reg, unsigned shift)
{
	return (unsigned)((reg >> shift) & 0xf);
}

static void add_ext(char *dst, size_t cap, const char *name)
{
	size_t n = kstrlen(dst);

	if (n && n + 1 < cap)
		dst[n++] = ' ';
	while (*name && n + 1 < cap)
		dst[n++] = *name++;
	dst[n] = '\0';
}

static const char *implementer_name(unsigned id)
{
	switch (id) {
	case 0x41: return "ARM";
	case 0x42: return "Broadcom";
	case 0x43: return "Cavium";
	case 0x46: return "Fujitsu";
	case 0x48: return "HiSilicon";
	case 0x4e: return "NVIDIA";
	case 0x51: return "Qualcomm";
	case 0x61: return "Apple";
	case 0xc0: return "Ampere";
	default:   return "unknown";
	}
}

/* PARange, from ID_AA64MMFR0_EL1. The encoding is a table rather than an
 * arithmetic relation, which is why this is a switch. */
static unsigned parange_bits(unsigned encoded)
{
	switch (encoded) {
	case 0: return 32;
	case 1: return 36;
	case 2: return 40;
	case 3: return 42;
	case 4: return 44;
	case 5: return 48;
	case 6: return 52;
	default: return 48;	/* a value we do not know: assume the common one */
	}
}

void arch_cpu_caps(struct cpu_caps *c)
{
	u64 midr   = READ_SYSREG(midr_el1);
	u64 isar0  = READ_SYSREG(id_aa64isar0_el1);
	u64 pfr0   = READ_SYSREG(id_aa64pfr0_el1);
	u64 mmfr0  = READ_SYSREG(id_aa64mmfr0_el1);

	unsigned implementer = (unsigned)((midr >> 24) & 0xff);
	unsigned part        = (unsigned)((midr >> 4) & 0xfff);
	unsigned tgran4;

	kmemset(c, 0, sizeof(*c));

	kstrlcpy(c->vendor, implementer_name(implementer), sizeof(c->vendor));

	/* There is no brand string on this architecture -- no equivalent of
	 * x86's forty-eight bytes of marketing. The part number is what there
	 * is, and a table mapping part numbers to names would be a table that
	 * is wrong for every chip released after it was written. */
	{
		static const char hex[] = "0123456789abcdef";
		size_t n = kstrlcpy(c->brand, c->vendor, sizeof(c->brand));
		const char *tail = " part 0x";

		for (const char *p = tail; *p && n + 1 < sizeof(c->brand); p++)
			c->brand[n++] = *p;
		for (int s = 8; s >= 0 && n + 1 < sizeof(c->brand); s -= 4)
			c->brand[n++] = hex[(part >> s) & 0xf];
		c->brand[n] = '\0';
	}

	/* Signed fields: 0b1111 is absent, anything else is present. */
	c->fpu  = field(pfr0, 16) != 0xf;
	c->simd = field(pfr0, 20) != 0xf;
	c->simd_wide = field(pfr0, 32) != 0;		/* SVE */
	c->virtualization = field(pfr0, 8) != 0;	/* EL2 implemented */

	c->aes     = field(isar0, 4) != 0;
	c->sha     = field(isar0, 8) != 0 || field(isar0, 12) != 0;
	c->crc32   = field(isar0, 16) != 0;
	c->atomics = field(isar0, 20) != 0;		/* LSE */
	c->hw_random = field(isar0, 60) != 0;		/* RNDR / RNDRRS */

	if (c->simd)         add_ext(c->extensions, sizeof(c->extensions), "advsimd");
	if (c->simd_wide)    add_ext(c->extensions, sizeof(c->extensions), "sve");
	if (c->aes)          add_ext(c->extensions, sizeof(c->extensions), "aes");
	if (c->sha)          add_ext(c->extensions, sizeof(c->extensions), "sha2");
	if (c->crc32)        add_ext(c->extensions, sizeof(c->extensions), "crc32");
	if (c->atomics)      add_ext(c->extensions, sizeof(c->extensions), "lse");
	if (c->hw_random)    add_ext(c->extensions, sizeof(c->extensions), "rndr");
	if (c->virtualization) add_ext(c->extensions, sizeof(c->extensions), "el2");

	/* Execute-never is architectural here: every translation table entry
	 * has had the bit since the beginning. There is nothing to detect. */
	c->no_execute = true;

	/* Block mappings are how this architecture does large pages: with the
	 * 4KB granule, a level 2 block is 2MB and a level 1 block is 1GB. So
	 * the question is not "are large pages supported" but "is the 4KB
	 * granule supported", and TGran4 uses 0b1111 for absent. */
	tgran4 = field(mmfr0, 28);
	c->page_2m = (tgran4 != 0xf);
	c->page_1g = c->page_2m;
	if (c->page_1g)
		add_ext(c->extensions, sizeof(c->extensions), "1g-blocks");

	c->phys_addr_bits = parange_bits(field(mmfr0, 0));
	c->virt_addr_bits = 48;	/* what the kernel will configure; 52 needs LVA */

	/* The generic timer runs at a fixed frequency by architectural
	 * requirement -- it does not track the core clock the way x86's TSC
	 * historically did, so there is no invariant bit to look for. */
	c->invariant_timer = true;
	add_ext(c->extensions, sizeof(c->extensions), "generic-timer");

	/* Topology needs the device tree or ACPI: MPIDR describes *this* core's
	 * position, not how many there are. Left at zero deliberately, because
	 * zero means "did not say" and one would be a guess that the scheduler
	 * would believe. */
	c->cores = 0;
	c->threads = 0;
}
