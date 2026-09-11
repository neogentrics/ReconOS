#include <recon/kernel/boot.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>

static struct boot_info info;

struct boot_info *boot_info(void)
{
	return &info;
}

void boot_info_reset(const char *protocol, enum boot_firmware firmware)
{
	kmemset(&info, 0, sizeof(info));
	info.protocol = protocol;
	info.firmware = firmware;
	info.loader = "unknown";
	info.cmdline = "";
}

void boot_add_region(paddr_t base, u64 size, enum mem_kind kind)
{
	if (size == 0)
		return;

	if (info.region_count >= BOOT_MAX_REGIONS) {
		/* Counted rather than ignored. A machine that reports more
		 * regions than we can hold is a machine whose memory map we do
		 * not actually know, and that has to be visible. */
		info.regions_dropped++;
		return;
	}

	info.regions[info.region_count].base = base;
	info.regions[info.region_count].size = size;
	info.regions[info.region_count].kind = kind;
	info.region_count++;
}

/* Insertion sort. The list is at most BOOT_MAX_REGIONS long and this runs
 * once, so the simple algorithm is the right one -- and unlike a recursive
 * sort it cannot surprise a boot stack that is only 64KB deep. */
static void sort_regions(void)
{
	for (unsigned i = 1; i < info.region_count; i++) {
		struct mem_region key = info.regions[i];
		unsigned j = i;

		while (j > 0 && info.regions[j - 1].base > key.base) {
			info.regions[j] = info.regions[j - 1];
			j--;
		}
		info.regions[j] = key;
	}
}

/* Joins regions that touch and agree. Firmware routinely reports usable RAM in
 * several pieces that are in fact one; leaving them split makes every later
 * "is this range free" question harder than it needs to be. */
static void merge_regions(void)
{
	unsigned out = 0;

	for (unsigned i = 0; i < info.region_count; i++) {
		if (out > 0) {
			struct mem_region *prev = &info.regions[out - 1];

			if (prev->kind == info.regions[i].kind &&
			    prev->base + prev->size == info.regions[i].base) {
				prev->size += info.regions[i].size;
				continue;
			}
		}
		info.regions[out++] = info.regions[i];
	}

	info.region_count = out;
}

/* Firmware describes memory in layers: a broad "this is RAM" range, and then
 * smaller ranges inside it that something already owns. On aarch64 the device
 * tree says the whole 512MB is memory and separately says the blob sits at
 * 0x48000000 -- both true, and overlapping.
 *
 * Left alone, the totals would count owned memory as free, and the page
 * allocator would eventually hand out the device tree or the kernel's own
 * image. So anything not usable is subtracted from anything that is. Claims of
 * ownership win over claims of availability, always, because the cost of being
 * wrong is asymmetric: unused free memory is a waste, but reused owned memory
 * is corruption.
 */
/* Which claim wins where two regions describe the same address.
 *
 * It is not enough to say "anything beats usable". Our own loader reports the
 * kernel image inside the range it also reports as loader memory -- both true,
 * and the kernel's claim is the more specific one. Without an ordering the map
 * printed the same range twice under two names, and a later reader would have
 * had to guess which was operative.
 *
 * Higher wins. The shape of the ordering is: things that must never be touched,
 * then things owned until somebody finishes with them, then free.
 */
static unsigned kind_priority(enum mem_kind k)
{
	switch (k) {
	case MEM_KERNEL:       return 6;	/* never reusable, and ours */
	case MEM_BAD:          return 5;	/* never usable at all */
	case MEM_ACPI_NVS:     return 4;
	case MEM_ACPI_RECLAIM: return 3;
	case MEM_RESERVED:     return 2;
	case MEM_BOOTLOADER:   return 1;	/* ours, once we have finished reading it */
	case MEM_USABLE:
	default:               return 0;
	}
}

static void carve_out_owned(void)
{
	struct mem_region blockers[BOOT_MAX_REGIONS];
	unsigned n_blockers = 0;

	/* Snapshotted first: the loop below appends regions when a range is
	 * split in two, and iterating a list that grows underneath you is how a
	 * boot hangs. */
	for (unsigned i = 0; i < info.region_count; i++)
		if (kind_priority(info.regions[i].kind) > 0)
			blockers[n_blockers++] = info.regions[i];

	for (unsigned b = 0; b < n_blockers; b++) {
		u64 bs = blockers[b].base;
		u64 be = bs + blockers[b].size;
		unsigned bp = kind_priority(blockers[b].kind);

		for (unsigned i = 0; i < info.region_count; i++) {
			struct mem_region *u = &info.regions[i];
			u64 us, ue;

			if (u->size == 0 || kind_priority(u->kind) >= bp)
				continue;

			us = u->base;
			ue = us + u->size;

			if (be <= us || ue <= bs)
				continue;			/* disjoint */

			if (bs <= us && be >= ue) {
				u->size = 0;			/* wholly owned */
			} else if (bs <= us) {
				u->base = be;			/* owned at the bottom */
				u->size = ue - be;
			} else if (be >= ue) {
				u->size = bs - us;		/* owned at the top */
			} else {
				enum mem_kind kind = u->kind;

				u->size = bs - us;		/* owned in the middle */
				boot_add_region(be, ue - be, kind);
			}
		}
	}
}

static void drop_empty_regions(void)
{
	unsigned out = 0;

	for (unsigned i = 0; i < info.region_count; i++)
		if (info.regions[i].size != 0)
			info.regions[out++] = info.regions[i];

	info.region_count = out;
}

void boot_finish_regions(void)
{
	/* The kernel's own image is the one region every architecture knows
	 * about without being told, and the one nothing may ever reuse.
	 * Physical and virtual are still the same thing at this point; when the
	 * kernel moves to the higher half this has to become a translation. */
	boot_add_region((paddr_t)(uintptr_t)__kernel_phys_start,
			(u64)(__kernel_phys_end - __kernel_phys_start),
			MEM_KERNEL);

	carve_out_owned();
	drop_empty_regions();
	sort_regions();
	merge_regions();

	info.usable_bytes = 0;
	info.total_bytes = 0;

	for (unsigned i = 0; i < info.region_count; i++) {
		info.total_bytes += info.regions[i].size;
		if (info.regions[i].kind == MEM_USABLE)
			info.usable_bytes += info.regions[i].size;
	}
}

const char *boot_firmware_name(enum boot_firmware f)
{
	switch (f) {
	case BOOT_FIRMWARE_BIOS:       return "BIOS";
	case BOOT_FIRMWARE_UEFI:       return "UEFI";
	case BOOT_FIRMWARE_DEVICETREE: return "device tree";
	case BOOT_FIRMWARE_PARAVIRT:   return "paravirtual (no firmware)";
	case BOOT_FIRMWARE_UNKNOWN:
	default:                       return "unknown";
	}
}

const char *mem_kind_name(enum mem_kind k)
{
	switch (k) {
	case MEM_USABLE:       return "usable";
	case MEM_RESERVED:     return "reserved";
	case MEM_ACPI_RECLAIM: return "ACPI reclaimable";
	case MEM_ACPI_NVS:     return "ACPI NVS";
	case MEM_BAD:          return "bad";
	case MEM_BOOTLOADER:   return "bootloader";
	case MEM_KERNEL:       return "kernel image";
	default:               return "?";
	}
}

/* Sizes are printed in whole units with one decimal, which is enough to read a
 * memory map by and avoids needing division of 64-bit values by anything the
 * compiler would want a helper routine for. */
static void print_size(u64 bytes)
{
	static const char *const units[] = { "B", "KB", "MB", "GB", "TB" };
	unsigned unit = 0;
	u64 whole = bytes;
	u64 frac = 0;

	while (whole >= 1024 && unit < 4) {
		frac = ((whole % 1024) * 10) / 1024;
		whole /= 1024;
		unit++;
	}

	if (unit == 0)
		kprintf("%lu B", whole);
	else
		kprintf("%lu.%lu %s", whole, frac, units[unit]);
}

/* --- the registers the loader handed over ---------------------------------*/

#if defined(__x86_64__)

/* The copy in the higher half, not the capture itself: the capture sits beside
 * the page tables at a low address, and the identity mapping that reaches it is
 * gone by the time anything in C asks. See boot.S. */
extern u64 handoff_saved[];

#define boot_handoff_regs (handoff_saved)
#define boot_handoff_mark (handoff_saved[16])

/* In the order boot.S stores them. RDI is absent because it carries the
 * handoff; RAX is present, and is the one that cannot be zero. */
static const char *const handoff_names[] = {
	"rax", "rbx", "rcx", "rdx", "rsi", "rbp",
	"r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
};

#define HANDOFF_MARK   0x5245434F4E524547ull	/* "RECONREG" */
#define HANDOFF_SKIP   1			/* rax holds the entry point */
#define HANDOFF_UNCHECKED "rdi carried the handoff, rax the entry point"

#elif defined(__aarch64__)

extern u64 handoff_saved[];

#define boot_handoff_regs (handoff_saved)
#define boot_handoff_mark (handoff_saved[28])

/* x0, x1 and x16 are absent, for three different reasons -- see boot.S. */
static const char *const handoff_names[] = {
	"x2",  "x3",  "x4",  "x5",  "x6",  "x7",  "x8",
	"x9",  "x10", "x11", "x12", "x13", "x14", "x15",
	"x17", "x18", "x19", "x20", "x21", "x22", "x23",
	"x24", "x25", "x26", "x27", "x28", "x29", "x30",
};

#define HANDOFF_MARK   0x5245434F4E524547ull	/* "RECONREG" */
#define HANDOFF_SKIP   0			/* none of these carry one */
#define HANDOFF_UNCHECKED "x0 carried the handoff, x16 the entry point, and x1 was spent naming where to record the rest"

#else

static u64 *const boot_handoff_regs;
static const u64 boot_handoff_mark;
static const char *const handoff_names[] = { 0 };

#define HANDOFF_MARK   0
#define HANDOFF_SKIP   0
#define HANDOFF_UNCHECKED ""

#endif

bool boot_handoff_registers_clear(const char **dirty, unsigned *checked)
{
	unsigned i;
	unsigned n = sizeof(handoff_names) / sizeof(handoff_names[0]);

	if (dirty)
		*dirty = 0;
	if (checked)
		*checked = 0;

	/* Nothing recorded. Either this is not the ReconBoot path, or it is an
	 * architecture with no capture -- and in both cases the honest answer
	 * is that there is nothing to report rather than that everything was
	 * fine. */
	if (!HANDOFF_MARK || boot_handoff_mark != HANDOFF_MARK)
		return true;

	if (checked)
		*checked = n - HANDOFF_SKIP;

	for (i = HANDOFF_SKIP; i < n; i++) {
		if (boot_handoff_regs[i]) {
			if (dirty)
				*dirty = handoff_names[i];
			return false;
		}
	}

	return true;
}

void boot_print_summary(void)
{
	kprintf("\nBoot\n");
	kprintf("  firmware     : %s\n", boot_firmware_name(info.firmware));
	kprintf("  protocol     : %s\n", info.protocol ? info.protocol : "none");
	kprintf("  loader       : %s\n", info.loader);

	{
		const char *dirty = 0;
		unsigned checked = 0;

		if (!boot_handoff_registers_clear(&dirty, &checked))
			kprintf("  handoff      : %s arrived holding "
				"something\n", dirty ? dirty : "a register");
		else if (checked)
			kprintf("  handoff      : %u register(s) arrived "
				"clear (%s)\n", checked,
				HANDOFF_UNCHECKED);
	}
	if (info.cmdline && info.cmdline[0])
		kprintf("  command line : %s\n", info.cmdline);
	if (info.acpi_rsdp)
		kprintf("  ACPI RSDP    : %p\n", (void *)(uintptr_t)info.acpi_rsdp);
	if (info.dtb)
		kprintf("  device tree  : %p\n", (void *)(uintptr_t)info.dtb);

	if (info.fb.width)
		kprintf("  framebuffer  : %ux%u at %p, pitch %u, %s\n",
			info.fb.width, info.fb.height,
			(void *)(uintptr_t)info.fb.base, info.fb.pitch,
			info.fb.format == FB_FORMAT_BGRA ? "BGRA" : "RGBA");
	else
		kputs("  framebuffer  : none\n");

	kprintf("\nMemory map (%u regions)\n", info.region_count);

	for (unsigned i = 0; i < info.region_count; i++) {
		const struct mem_region *r = &info.regions[i];

		kprintf("  %p-%p  ", (void *)(uintptr_t)r->base,
			(void *)(uintptr_t)(r->base + r->size - 1));
		print_size(r->size);
		kprintf("  %s\n", mem_kind_name(r->kind));
	}

	if (info.regions_dropped)
		kprintf("  WARNING: %u regions did not fit and were dropped\n",
			info.regions_dropped);

	kprintf("\n  usable : ");
	print_size(info.usable_bytes);
	kprintf("\n  mapped : ");
	print_size(info.total_bytes);
	kprintf("\n  kernel : %p-%p  ", (void *)__kernel_phys_start,
		(void *)__kernel_phys_end);
	print_size((u64)(__kernel_phys_end - __kernel_phys_start));
	kprintf("\n");
}
