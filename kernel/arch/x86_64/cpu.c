/* What this x86_64 processor offers, read out of CPUID.
 *
 * CPUID is a self-describing interface: leaf 0 says how many leaves there are,
 * and asking beyond that returns whatever the highest supported leaf returns
 * rather than an error. Every read below is therefore guarded by the maximum
 * the CPU admitted to, because the alternative is not a fault -- it is
 * plausible nonsense, which is worse.
 */
#include "x86_64.h"

#include <recon/kernel/cpu.h>
#include <recon/kernel/kstring.h>

static void cpuid_count(u32 leaf, u32 sub, u32 *a, u32 *b, u32 *c, u32 *d)
{
	__asm__ volatile("cpuid"
			 : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
			 : "a"(leaf), "c"(sub));
}

/* Appends a name to the human-readable list, space separated, never
 * overrunning. Silently stops when full: the list is for a person to read and
 * a truncated one is better than a corrupted one. */
static void add_ext(char *dst, size_t cap, const char *name)
{
	size_t n = kstrlen(dst);

	if (n && n + 1 < cap)
		dst[n++] = ' ';
	while (*name && n + 1 < cap)
		dst[n++] = *name++;
	dst[n] = '\0';
}

void arch_cpu_caps(struct cpu_caps *c)
{
	u32 max_basic, max_ext, a, b, cx, d;
	u32 words[13];

	kmemset(c, 0, sizeof(*c));

	/* Leaf 0: the vendor string, and how far we may ask. The three
	 * registers are not in register order -- EBX, EDX, ECX -- which is a
	 * genuine quirk of the instruction and not a transcription error. */
	cpuid_count(0, 0, &max_basic, &b, &cx, &d);
	words[0] = b;
	words[1] = d;
	words[2] = cx;
	kmemcpy(c->vendor, words, 12);
	c->vendor[12] = '\0';

	cpuid_count(0x80000000u, 0, &max_ext, &b, &cx, &d);

	if (max_ext >= 0x80000004u) {
		const char *brand;
		size_t n = 0;

		for (unsigned i = 0; i < 3; i++)
			cpuid_count(0x80000002u + i, 0, &words[i * 4 + 0],
				    &words[i * 4 + 1], &words[i * 4 + 2],
				    &words[i * 4 + 3]);
		words[12] = 0;

		/* Intel pads the brand with leading spaces. */
		brand = (const char *)words;
		while (*brand == ' ')
			brand++;
		n = kstrlcpy(c->brand, brand, sizeof(c->brand));
		(void)n;
	} else {
		kstrlcpy(c->brand, c->vendor, sizeof(c->brand));
	}

	if (max_basic >= 1) {
		cpuid_count(1, 0, &a, &b, &cx, &d);

		c->fpu  = (d >> 0) & 1;
		c->simd = (d >> 26) & 1;		/* SSE2 -- the x86_64 baseline */
		c->aes  = (cx >> 25) & 1;
		c->crc32 = (cx >> 20) & 1;		/* SSE4.2 */
		c->simd_wide = (cx >> 28) & 1;		/* AVX */
		c->hw_random = (cx >> 30) & 1;		/* RDRAND */
		c->virtualization = (cx >> 5) & 1;	/* VMX */

		if (c->simd)      add_ext(c->extensions, sizeof(c->extensions), "sse2");
		if (c->crc32)     add_ext(c->extensions, sizeof(c->extensions), "sse4.2");
		if (c->simd_wide) add_ext(c->extensions, sizeof(c->extensions), "avx");
		if (c->aes)       add_ext(c->extensions, sizeof(c->extensions), "aes-ni");
		if (c->hw_random) add_ext(c->extensions, sizeof(c->extensions), "rdrand");

		/* EBX[23:16] is the number of logical processors this package
		 * reports. It is famously unreliable on its own; leaf 0x1F and
		 * 0x0B replace it below where they exist. */
		c->threads = (b >> 16) & 0xff;
	}

	if (max_basic >= 7) {
		cpuid_count(7, 0, &a, &b, &cx, &d);

		if ((b >> 5) & 1)  add_ext(c->extensions, sizeof(c->extensions), "avx2");
		if ((b >> 16) & 1) add_ext(c->extensions, sizeof(c->extensions), "avx512f");
		if ((b >> 18) & 1) {
			/* RDSEED: a true entropy source rather than RDRAND's
			 * conditioned stream. The randomness service prefers it. */
			c->hw_random = true;
			add_ext(c->extensions, sizeof(c->extensions), "rdseed");
		}
		if ((b >> 29) & 1) {
			c->sha = true;
			add_ext(c->extensions, sizeof(c->extensions), "sha-ni");
		}
		if ((b >> 7) & 1)  add_ext(c->extensions, sizeof(c->extensions), "smep");
		if ((b >> 20) & 1) add_ext(c->extensions, sizeof(c->extensions), "smap");
	}

	/* Long mode always has 2MB pages: PAE paging is a requirement of it and
	 * PAE is where 2MB pages come from. 1GB pages are optional and worth
	 * asking about -- they are the difference between mapping a gigabyte
	 * with one entry and with five hundred and twelve. */
	c->page_2m = true;
	c->atomics = true;	/* lock cmpxchg has been there since the 486 */

	if (max_ext >= 0x80000001u) {
		cpuid_count(0x80000001u, 0, &a, &b, &cx, &d);

		c->no_execute = (d >> 20) & 1;
		c->page_1g    = (d >> 26) & 1;

		if (c->no_execute) add_ext(c->extensions, sizeof(c->extensions), "nx");
		if (c->page_1g)    add_ext(c->extensions, sizeof(c->extensions), "1g-pages");
	}

	if (max_ext >= 0x80000007u) {
		cpuid_count(0x80000007u, 0, &a, &b, &cx, &d);
		c->invariant_timer = (d >> 8) & 1;
		if (c->invariant_timer)
			add_ext(c->extensions, sizeof(c->extensions), "invariant-tsc");
	}

	if (max_ext >= 0x80000008u) {
		cpuid_count(0x80000008u, 0, &a, &b, &cx, &d);
		c->phys_addr_bits = a & 0xff;
		c->virt_addr_bits = (a >> 8) & 0xff;
	} else {
		/* The architectural minimum for long mode. Reported rather than
		 * left at zero, because zero would read as "no memory". */
		c->phys_addr_bits = 36;
		c->virt_addr_bits = 48;
	}

	/* Topology. Leaf 0x1F supersedes 0x0B and both describe the same thing:
	 * levels, each saying how many of the level below it contains. Level
	 * type 1 is a thread within a core, type 2 a core within a package. */
	if (max_basic >= 0x0B) {
		unsigned per_core = 0, per_package = 0;

		for (u32 level = 0; level < 8; level++) {
			unsigned type;

			cpuid_count(0x0B, level, &a, &b, &cx, &d);
			type = (cx >> 8) & 0xff;

			if (type == 0)
				break;		/* no more levels */
			if (type == 1)
				per_core = b & 0xffff;
			else if (type == 2)
				per_package = b & 0xffff;
		}

		if (per_package) {
			c->threads = per_package;
			c->cores = per_core ? per_package / per_core : per_package;
		}
	}

	if (c->threads && !c->cores)
		c->cores = c->threads;
}
