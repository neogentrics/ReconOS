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
static void carve_out_owned(void)
{
	struct mem_region blockers[BOOT_MAX_REGIONS];
	unsigned n_blockers = 0;

	/* Snapshotted first: the loop below appends regions when a usable range
	 * is split in two, and iterating a list that grows underneath you is how
	 * a boot hangs. */
	for (unsigned i = 0; i < info.region_count; i++)
		if (info.regions[i].kind != MEM_USABLE)
			blockers[n_blockers++] = info.regions[i];

	for (unsigned b = 0; b < n_blockers; b++) {
		u64 bs = blockers[b].base;
		u64 be = bs + blockers[b].size;

		for (unsigned i = 0; i < info.region_count; i++) {
			struct mem_region *u = &info.regions[i];
			u64 us, ue;

			if (u->kind != MEM_USABLE || u->size == 0)
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
				u->size = bs - us;		/* owned in the middle */
				boot_add_region(be, ue - be, MEM_USABLE);
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
	boot_add_region((paddr_t)(uintptr_t)__kernel_start,
			(u64)(__kernel_end - __kernel_start), MEM_KERNEL);

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

void boot_print_summary(void)
{
	kprintf("\nBoot\n");
	kprintf("  firmware     : %s\n", boot_firmware_name(info.firmware));
	kprintf("  protocol     : %s\n", info.protocol ? info.protocol : "none");
	kprintf("  loader       : %s\n", info.loader);
	if (info.cmdline && info.cmdline[0])
		kprintf("  command line : %s\n", info.cmdline);
	if (info.acpi_rsdp)
		kprintf("  ACPI RSDP    : %p\n", (void *)(uintptr_t)info.acpi_rsdp);
	if (info.dtb)
		kprintf("  device tree  : %p\n", (void *)(uintptr_t)info.dtb);

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
	kprintf("\n  kernel : %p-%p  ", (void *)__kernel_start, (void *)__kernel_end);
	print_size((u64)(__kernel_end - __kernel_start));
	kprintf("\n");
}
