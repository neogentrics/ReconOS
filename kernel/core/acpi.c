/* Walking the ACPI tables: a directory, a checksum, and nothing clever.
 *
 * The structure is a two-level lookup. A root pointer, published by the
 * firmware, names a root *table*; that table is an array of pointers to every
 * other table; each of those starts with a four-character signature saying what
 * it is. So finding the one you want is a linear scan of an array, and the only
 * subtlety is which array.
 *
 * --- Two root tables, and picking the wrong one is silent ---
 *
 * ACPI 1.0 has an RSDT whose entries are 32-bit addresses. ACPI 2.0 added an
 * XSDT whose entries are 64. A machine with both must be read through the
 * XSDT -- the specification says so, and the practical reason is that a table
 * above four gigabytes simply cannot appear in the RSDT, so the older list is
 * quietly incomplete on exactly the machines where it matters.
 *
 * --- The checksums are not decoration ---
 *
 * Every table's bytes sum to zero in eight bits. That is a weak check and it is
 * still worth doing, because the failure it catches is the one that matters
 * here: a pointer that leads somewhere that is not a table at all. Following it
 * would read a length out of whatever is there and then walk that far, which is
 * how a bad root pointer becomes a fault a hundred kilobytes away.
 */
#include <recon/kernel/acpi.h>

#include <recon/kernel/console.h>
#include <recon/kernel/vm.h>

struct acpi_rsdp {
	char signature[8];	/* "RSD PTR " -- with the trailing space */
	u8   checksum;		/* covers the first twenty bytes only */
	char oem_id[6];
	u8   revision;		/* 0 for ACPI 1.0, 2 for anything later */
	u32  rsdt_address;

	/* Present only when revision is 2 or more. */
	u32  length;
	u64  xsdt_address;
	u8   extended_checksum;	/* covers the whole structure */
	u8   reserved[3];
} RK_PACKED;

static const struct acpi_sdt_header *root;
static bool root_is_xsdt;
static unsigned table_count;

static bool checksum_ok(const void *p, u32 len)
{
	const u8 *b = p;
	u8 sum = 0;

	for (u32 i = 0; i < len; i++)
		sum = (u8)(sum + b[i]);

	return sum == 0;
}

static bool signature_is(const char *actual, const char *want)
{
	for (unsigned i = 0; i < 4; i++)
		if (actual[i] != want[i])
			return false;
	return true;
}

void acpi_init(paddr_t rsdp_phys)
{
	const struct acpi_rsdp *rsdp;
	const struct acpi_sdt_header *candidate;
	paddr_t root_phys;

	root = 0;
	table_count = 0;

	if (!rsdp_phys)
		return;

	rsdp = phys_to_virt(rsdp_phys);

	/* Eight characters, and the last of them is a space. Checking only
	 * "RSD PTR" would match a structure that is nearly right, and nearly
	 * right is the dangerous case. */
	{
		static const char want[8] = { 'R', 'S', 'D', ' ', 'P', 'T', 'R', ' ' };

		for (unsigned i = 0; i < 8; i++)
			if (rsdp->signature[i] != want[i]) {
				kputs("acpi: the published root pointer is not "
				      "one; ignoring it\n");
				return;
			}
	}

	/* The first checksum covers twenty bytes, which is all ACPI 1.0 had.
	 * The extended one covers the rest and only exists from revision 2. */
	if (!checksum_ok(rsdp, 20)) {
		kputs("acpi: the root pointer's checksum is wrong\n");
		return;
	}

	if (rsdp->revision >= 2 && rsdp->xsdt_address) {
		if (!checksum_ok(rsdp, rsdp->length)) {
			kputs("acpi: the root pointer's extended checksum is "
			      "wrong\n");
			return;
		}
		root_phys = (paddr_t)rsdp->xsdt_address;
		root_is_xsdt = true;
	} else {
		root_phys = (paddr_t)rsdp->rsdt_address;
		root_is_xsdt = false;
	}

	if (!root_phys)
		return;

	candidate = phys_to_virt(root_phys);

	if (!checksum_ok(candidate, candidate->length)) {
		kputs("acpi: the root table's checksum is wrong, so nothing it "
		      "points at can be trusted\n");
		return;
	}

	root = candidate;
	table_count = (candidate->length - sizeof(*candidate))
		    / (root_is_xsdt ? 8 : 4);
}

bool acpi_available(void)
{
	return root != 0;
}

/* One entry of the root table, widened.
 *
 * Read byte by byte rather than as a u64. The XSDT's entries are eight bytes
 * long and the table header is thirty-six, so every entry after the first is
 * *misaligned* -- and a 64-bit load from an odd multiple of four faults on
 * aarch64 while working on x86. A table walker that works on one architecture
 * and not the other, for a reason invisible in the source, is exactly the kind
 * of thing this kernel's arch split exists to avoid. */
static u64 root_entry(unsigned i)
{
	const u8 *entries = (const u8 *)root + sizeof(*root);
	unsigned width = root_is_xsdt ? 8 : 4;
	const u8 *p = entries + (size_t)i * width;
	u64 v = 0;

	for (unsigned b = 0; b < width; b++)
		v |= (u64)p[b] << (b * 8);

	return v;
}

const struct acpi_sdt_header *acpi_find(const char signature[4])
{
	if (!root)
		return 0;

	for (unsigned i = 0; i < table_count; i++) {
		paddr_t phys = (paddr_t)root_entry(i);
		const struct acpi_sdt_header *t;

		if (!phys)
			continue;

		t = phys_to_virt(phys);

		if (!signature_is(t->signature, signature))
			continue;

		/* Checked only on the table that was asked for, not on every
		 * table during the scan. A machine may carry a table this
		 * kernel will never read and whose checksum is wrong, and
		 * refusing to boot over one would be refusing to boot over
		 * something that does not matter. */
		if (!checksum_ok(t, t->length)) {
			kprintf("acpi: the %c%c%c%c table's checksum is wrong; "
				"ignoring it\n", signature[0], signature[1],
				signature[2], signature[3]);
			return 0;
		}

		return t;
	}

	return 0;
}

/* MCFG: where PCI configuration space is memory-mapped.
 *
 * The table is a header, eight reserved bytes, and then one entry per segment
 * group. Almost every machine has exactly one, and this reads the first --
 * multiple segments happen on large servers, and a kernel that has never seen
 * a second processor socket has no business pretending to handle them. */
struct acpi_mcfg_entry {
	u64 base_address;
	u16 segment;
	u8  start_bus;
	u8  end_bus;
	u32 reserved;
} RK_PACKED;

bool acpi_pci_ecam(u64 *base, u8 *start_bus, u8 *end_bus)
{
	static const char sig[4] = { 'M', 'C', 'F', 'G' };
	const struct acpi_sdt_header *t = acpi_find(sig);
	const u8 *p;
	struct acpi_mcfg_entry e;
	const u8 *src;
	u8 *dst;

	if (!t)
		return false;

	if (t->length < sizeof(*t) + 8 + sizeof(e))
		return false;

	p = (const u8 *)t + sizeof(*t) + 8;

	/* Copied out byte by byte for the same reason the root entries are read
	 * that way: the entry begins forty-four bytes into the table, which is
	 * not a multiple of eight. */
	src = p;
	dst = (u8 *)&e;
	for (unsigned i = 0; i < sizeof(e); i++)
		dst[i] = src[i];

	if (!e.base_address)
		return false;

	*base      = e.base_address;
	*start_bus = e.start_bus;
	*end_bus   = e.end_bus;
	return true;
}

void acpi_print_summary(void)
{
	if (!root) {
		kputs("  acpi         : no tables\n");
		return;
	}

	kprintf("  acpi         : %s, %u table%s\n",
		root_is_xsdt ? "XSDT" : "RSDT", table_count,
		table_count == 1 ? "" : "s");

	kputs("    ");
	for (unsigned i = 0; i < table_count; i++) {
		paddr_t phys = (paddr_t)root_entry(i);
		const struct acpi_sdt_header *t;

		if (!phys)
			continue;

		t = phys_to_virt(phys);
		kprintf("%c%c%c%c ", t->signature[0], t->signature[1],
			t->signature[2], t->signature[3]);
	}
	kputs("\n");
}
