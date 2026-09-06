/* A flattened device tree, read far enough to find the memory.
 *
 * On ARM there is no E820 and usually no firmware to ask. What there is
 * instead is a blob the firmware left in memory describing the machine:
 * every device, where its registers are, which interrupts it raises. It is
 * the same file format that Linux boots with, and the one honest description
 * of a machine that has no enumerable bus.
 *
 * This is not a general device tree library and does not try to be. It answers
 * exactly two questions -- what memory exists, and what the command line was --
 * because those are what boot needs, and a parser is easier to trust when the
 * questions it can be asked are few. Device enumeration will need more, and
 * that is the point at which it grows.
 *
 * Everything in the format is big-endian regardless of the CPU, which is the
 * source of every conversion below.
 */
#include "aarch64.h"

#include <recon/kernel/boot.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/vm.h>

#define FDT_MAGIC 0xd00dfeedu

#define FDT_BEGIN_NODE 1
#define FDT_END_NODE   2
#define FDT_PROP       3
#define FDT_NOP        4
#define FDT_END        9

struct fdt_header {
	u32 magic;
	u32 totalsize;
	u32 off_dt_struct;
	u32 off_dt_strings;
	u32 off_mem_rsvmap;
	u32 version;
	u32 last_comp_version;
	u32 boot_cpuid_phys;
	u32 size_dt_strings;
	u32 size_dt_struct;
};

static u32 be32(const void *p)
{
	const u8 *b = p;
	return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
}

static u64 be64(const void *p)
{
	return ((u64)be32(p) << 32) | be32((const u8 *)p + 4);
}

/* Reads one address- or size-sized value: cells are 32 bits each, and a
 * property may use one or two of them depending on what the parent node
 * declared. Anything wider than two cells is beyond a 64-bit address space
 * and is treated as unreadable rather than truncated. */
static bool read_cells(const u8 **p, u32 cells, u64 *out)
{
	switch (cells) {
	case 1:
		*out = be32(*p);
		*p += 4;
		return true;
	case 2:
		*out = be64(*p);
		*p += 8;
		return true;
	default:
		return false;
	}
}

static bool name_is(const char *name, const char *want)
{
	while (*want) {
		if (*name != *want)
			return false;
		name++;
		want++;
	}
	/* A device tree node name is "memory" or "memory@40000000": the unit
	 * address after '@' is part of the name but not part of the identity. */
	return *name == '\0' || *name == '@';
}

static char cmdline_copy[256];

bool fdt_parse(u64 dtb_phys)
{
	const u8 *dtb = (const u8 *)(uintptr_t)dtb_phys;
	const struct fdt_header *h = (const struct fdt_header *)dtb;
	const u8 *strings, *p, *end;

	/* Root defaults, per the specification, until the root node says
	 * otherwise -- and QEMU's virt machine does say otherwise. */
	u32 addr_cells = 2, size_cells = 1;

	int depth = 0;
	const char *node_name = "";

	if (!dtb_phys || be32(&h->magic) != FDT_MAGIC)
		return false;

	strings = dtb + be32(&h->off_dt_strings);
	p       = dtb + be32(&h->off_dt_struct);
	end     = p + be32(&h->size_dt_struct);

	boot_info_reset("Device Tree", BOOT_FIRMWARE_DEVICETREE);
	boot_info()->loader = "firmware";
	boot_info()->dtb = (paddr_t)dtb_phys;

	/* The blob itself is memory somebody else owns until we have finished
	 * reading it. Recorded so the allocator does not hand it out. */
	boot_add_region((paddr_t)dtb_phys, be32(&h->totalsize), MEM_BOOTLOADER);

	/* The memory reservation block: ranges the firmware says must not be
	 * touched, listed before any node and terminated by a zero pair. */
	for (const u8 *r = dtb + be32(&h->off_mem_rsvmap); r + 16 <= dtb + be32(&h->totalsize);
	     r += 16) {
		u64 addr = be64(r);
		u64 size = be64(r + 8);

		if (addr == 0 && size == 0)
			break;
		boot_add_region((paddr_t)addr, size, MEM_RESERVED);
	}

	while (p + 4 <= end) {
		u32 token = be32(p);

		p += 4;

		switch (token) {
		case FDT_BEGIN_NODE:
			node_name = (const char *)p;
			depth++;
			/* Node names are NUL-terminated and padded to four bytes. */
			p += (kstrlen(node_name) + 1 + 3) & ~3u;
			break;

		case FDT_END_NODE:
			depth--;
			break;

		case FDT_NOP:
			break;

		case FDT_END:
			p = end;
			break;

		case FDT_PROP: {
			u32 len, nameoff;
			const char *prop;
			const u8 *value;

			if (p + 8 > end)
				return false;

			len     = be32(p);
			nameoff = be32(p + 4);
			value   = p + 8;
			prop    = (const char *)(strings + nameoff);

			p = value + ((len + 3) & ~3u);

			if (depth == 1) {
				/* Root properties decide how wide addresses and
				 * sizes are in every child's `reg`. */
				if (name_is(prop, "#address-cells") && len == 4)
					addr_cells = be32(value);
				else if (name_is(prop, "#size-cells") && len == 4)
					size_cells = be32(value);
			} else if (depth == 2 && name_is(node_name, "memory") &&
				   name_is(prop, "reg")) {
				const u8 *v = value;
				const u8 *v_end = value + len;

				while (v < v_end) {
					u64 base, size;

					if (!read_cells(&v, addr_cells, &base) ||
					    !read_cells(&v, size_cells, &size))
						break;
					boot_add_region((paddr_t)base, size, MEM_USABLE);
				}
			} else if (depth == 2 && name_is(node_name, "chosen") &&
				   name_is(prop, "bootargs") && len > 0) {
				kstrlcpy(cmdline_copy, (const char *)value,
					 sizeof(cmdline_copy));
				boot_info()->cmdline = cmdline_copy;
			}
			break;
		}

		default:
			/* An unrecognised token means the walk has lost its place;
			 * continuing would report nonsense as a memory map. */
			return false;
		}
	}

	return true;
}

/* --- A second pass, for devices ------------------------------------------
 *
 * The walk above runs before there is an allocator, so anything it wanted to
 * remember would need somewhere fixed to put it. The blob is still there
 * afterwards -- it was recorded as bootloader-owned memory precisely so that
 * nothing hands it out -- so the cheaper answer is to read it again when the
 * question is asked.
 *
 * `compatible` is a list of NUL-separated strings, most specific first, and a
 * match on any of them is a match. Comparing only the first would miss
 * "virtio,mmio" on a node that also claims something more specific, which is
 * exactly the shape the property exists to allow.
 */
static bool compatible_contains(const char *list, u32 len, const char *want)
{
	u32 i = 0;

	while (i < len) {
		const char *entry = list + i;
		u32 n = 0;

		while (i + n < len && entry[n] != '\0')
			n++;

		if (n && kstrlen(want) == n) {
			u32 k = 0;

			while (k < n && entry[k] == want[k])
				k++;
			if (k == n)
				return true;
		}

		i += n + 1;
	}

	return false;
}

/* A node's properties always precede its children, so a node is finished being
 * described the moment either a child begins or it ends. Reporting only at the
 * end is wrong for any node that has children: the child's FDT_BEGIN_NODE
 * clears the match that the parent had already earned, and the parent's
 * FDT_END_NODE then finds nothing to report.
 *
 * That is not a corner case. QEMU's GICv3 node has an ITS child, so a kernel
 * asking "is there an arm,gic-v3 here" was told no by the machine that had
 * one -- and fell back to GICv2, whose CPU interface does not exist on such a
 * machine, and panicked at boot. Every other node this walk is asked about is
 * childless, which is why it went unseen. (BG-125)
 */
static void report_node(bool matched, const u8 *reg_value, u32 reg_len,
			u32 addr_cells, u32 size_cells,
			void (*fn)(u64 base, u64 size))
{
	const u8 *v, *v_end;

	if (!matched || !reg_value)
		return;

	v     = reg_value;
	v_end = reg_value + reg_len;

	while (v < v_end) {
		u64 base, size;

		if (!read_cells(&v, addr_cells, &base) ||
		    !read_cells(&v, size_cells, &size))
			break;
		fn(base, size);
	}
}

void fdt_each_compatible(u64 dtb_phys, const char *compat,
			 void (*fn)(u64 base, u64 size))
{
	const u8 *dtb = (const u8 *)phys_to_virt((paddr_t)dtb_phys);
	const struct fdt_header *h = (const struct fdt_header *)dtb;
	const u8 *strings, *p, *end;

	u32 addr_cells = 2, size_cells = 1;
	int depth = 0;

	/* Held across the properties of one node, because `compatible` and
	 * `reg` arrive in whichever order the tree was written and the node is
	 * only interesting when both are present. */
	bool matched = false;
	const u8 *reg_value = 0;
	u32 reg_len = 0;

	if (!dtb_phys || be32(&h->magic) != FDT_MAGIC)
		return;

	strings = dtb + be32(&h->off_dt_strings);
	p       = dtb + be32(&h->off_dt_struct);
	end     = p + be32(&h->size_dt_struct);

	while (p + 4 <= end) {
		u32 token = be32(p);

		p += 4;

		switch (token) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;

			/* The enclosing node is fully described now: its own
			 * properties are behind us and this child is about to
			 * overwrite them. */
			report_node(matched, reg_value, reg_len,
				    addr_cells, size_cells, fn);

			depth++;
			p += (kstrlen(name) + 1 + 3) & ~3u;

			matched   = false;
			reg_value = 0;
			reg_len   = 0;
			break;
		}

		case FDT_END_NODE:
			report_node(matched, reg_value, reg_len,
				    addr_cells, size_cells, fn);

			matched   = false;
			reg_value = 0;
			reg_len   = 0;
			depth--;
			break;

		case FDT_NOP:
			break;

		case FDT_END:
			return;

		case FDT_PROP: {
			u32 len, nameoff;
			const char *prop;
			const u8 *value;

			if (p + 8 > end)
				return;

			len     = be32(p);
			nameoff = be32(p + 4);
			value   = p + 8;
			prop    = (const char *)(strings + nameoff);

			p = value + ((len + 3) & ~3u);

			if (depth == 1) {
				if (name_is(prop, "#address-cells") && len == 4)
					addr_cells = be32(value);
				else if (name_is(prop, "#size-cells") && len == 4)
					size_cells = be32(value);
			} else if (name_is(prop, "compatible")) {
				matched = compatible_contains((const char *)value,
							      len, compat);
			} else if (name_is(prop, "reg")) {
				reg_value = value;
				reg_len   = len;
			}
			break;
		}

		default:
			return;
		}
	}
}

/* The same walk as fdt_each_compatible, reporting a named property's bytes
 * rather than the node's `reg`.
 *
 * Two walkers rather than one general one with a switch: the caller of each
 * wants a different shape of answer, and a single function returning both would
 * have to hand back a pointer into the blob for one of them anyway.
 */
void fdt_each_property(u64 dtb_phys, const char *compat, const char *prop_name,
		       void (*fn)(const u8 *value, u32 len))
{
	const u8 *dtb = (const u8 *)phys_to_virt((paddr_t)dtb_phys);
	const struct fdt_header *h = (const struct fdt_header *)dtb;
	const u8 *strings, *p, *end;

	bool matched = false;
	const u8 *want_value = 0;
	u32 want_len = 0;

	if (!dtb_phys || be32(&h->magic) != FDT_MAGIC)
		return;

	strings = dtb + be32(&h->off_dt_strings);
	p       = dtb + be32(&h->off_dt_struct);
	end     = p + be32(&h->size_dt_struct);

	while (p + 4 <= end) {
		u32 token = be32(p);

		p += 4;

		switch (token) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;

			/* Before the child erases it -- see report_node. */
			if (matched && want_value)
				fn(want_value, want_len);

			p += (kstrlen(name) + 1 + 3) & ~3u;
			matched = false;
			want_value = 0;
			want_len = 0;
			break;
		}

		case FDT_END_NODE:
			if (matched && want_value)
				fn(want_value, want_len);
			matched = false;
			want_value = 0;
			want_len = 0;
			break;

		case FDT_NOP:
			break;

		case FDT_END:
			return;

		case FDT_PROP: {
			u32 len, nameoff;
			const char *prop;
			const u8 *value;

			if (p + 8 > end)
				return;

			len     = be32(p);
			nameoff = be32(p + 4);
			value   = p + 8;
			prop    = (const char *)(strings + nameoff);

			p = value + ((len + 3) & ~3u);

			if (name_is(prop, "compatible"))
				matched = compatible_contains((const char *)value,
							      len, compat);
			else if (name_is(prop, prop_name)) {
				want_value = value;
				want_len   = len;
			}
			break;
		}

		default:
			return;
		}
	}
}

/* The memory window a PCI host bridge forwards.
 *
 * Needed for the same reason x86_64 needs one: on a machine booted directly,
 * with no firmware, nothing has placed any device's registers and the kernel
 * has to. The difference is where the answer comes from. There is no
 * architectural hole in the address space here -- an ARM machine can put RAM
 * anywhere -- so guessing is not available, and the device tree says instead.
 *
 * The `ranges` property of the bridge node is a list of windows it forwards,
 * each one three cells of child address, two of parent address, two of size.
 * The top byte of the first child cell says which kind of space it is:
 *
 *   0x01  I/O ports, which this architecture does not have
 *   0x02  32-bit memory
 *   0x03  64-bit memory
 *
 * The 32-bit window is the one to use. Every device can decode an address in
 * it, including the many that implement only a 32-bit base address register,
 * and it is the window firmware would have used.
 */
static u64 pci_window_base, pci_window_size;

static void note_pci_ranges(const u8 *value, u32 len)
{
	/* Seven cells per entry: three child, two parent, two size. Fixed by
	 * the binding rather than read from the node, because a host bridge
	 * that declared anything else would not be this binding. */
	const u32 entry = 7 * 4;

	if (pci_window_base)
		return;		/* the first bridge is the only one walked */

	for (u32 off = 0; off + entry <= len; off += entry) {
		const u8 *e = value + off;
		u32 flags = be32(e);
		u64 parent, size;

		if ((flags >> 24) != 0x02)
			continue;	/* not the 32-bit memory window */

		parent = ((u64)be32(e + 12) << 32) | be32(e + 16);
		size   = ((u64)be32(e + 20) << 32) | be32(e + 24);

		if (!size)
			continue;

		pci_window_base = parent;
		pci_window_size = size;
		return;
	}
}

bool fdt_pci_window(u64 dtb_phys, u64 *base, u64 *size)
{
	pci_window_base = 0;
	pci_window_size = 0;

	fdt_each_property(dtb_phys, "pci-host-ecam-generic", "ranges",
			  note_pci_ranges);

	if (!pci_window_base)
		return false;

	*base = pci_window_base;
	*size = pci_window_size;
	return true;
}
