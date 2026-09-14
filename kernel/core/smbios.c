/* Reading the SMBIOS structures. See smbios.h for what they are for.
 *
 * The format is two nested walks and both of them are places to run off the
 * end, so both are bounded by something the table itself declared:
 *
 *   THE STRUCTURE WALK. Each structure begins with a type, a length and a
 *   handle. The length covers only the *formatted* part -- the strings that
 *   follow it are not counted, and the next structure begins after a pair of
 *   consecutive zero bytes. A walk that trusted the length alone would land in
 *   the middle of the strings and read a length out of somebody's model name.
 *
 *   THE STRING WALK. Strings are referenced by *number*, one-based, counted
 *   from the end of the formatted part. Zero means "no string", which is not
 *   the same as the empty string and must not be reported as one.
 *
 * Everything here is read-only and every read is bounded. A machine's firmware
 * tables are written by the vendor, and this is the first code in the kernel
 * that parses a structure a stranger wrote where being wrong is silent.
 */
#include <recon/kernel/smbios.h>

#include <recon/kernel/boot.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/vm.h>

static struct smbios_facts facts;

/* Where the anchor was found, so the summary can say which answer it used.
 * Not cosmetic: the two sources disagree about nothing on a machine where both
 * work, so the only way to know the new one is being exercised is to print it.
 */
static const char *anchor_source = "nothing";

/* Make a physical range readable, if it is not already.
 *
 * The anchor and the table are firmware memory. Whether they fall inside the
 * direct map is a question about the firmware's memory map, and the answer has
 * already been no once: reading the legacy ROM region faulted on every boot
 * path that went through UEFI, because UEFI does not describe it. That was
 * fixed for the anchor and left standing for the table, which is read through
 * `phys_to_virt` a few lines below with nothing having mapped it.
 *
 * Read-only, because nothing here writes and a mapping that cannot be written
 * is one fewer thing a stray pointer can damage. */
static bool make_readable(paddr_t base, u64 len)
{
	paddr_t start = base & ~(paddr_t)(PAGE_SIZE - 1);
	u64 span = (base - start) + len;

	span = (span + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);

	if (vm_lookup((vaddr_t)(uintptr_t)phys_to_virt(start)))
		return true;

	return vm_map((vaddr_t)(uintptr_t)phys_to_virt(start), start,
		      (size_t)span, VM_READ | VM_GLOBAL);
}

/* One structure's header. Three bytes of formatted data follow before anything
 * type-specific, and the layout is fixed across every version. */
struct smbios_header {
	u8  type;
	u8  length;
	u16 handle;
} RK_PACKED;

#define SMBIOS_TYPE_BIOS	0
#define SMBIOS_TYPE_SYSTEM	1
#define SMBIOS_TYPE_END		127

/* The n-th string after a structure's formatted part, copied out.
 *
 * `n` is one-based and zero means the field was left unset -- so a caller
 * asking for string 0 gets nothing written, which is what "the vendor did not
 * say" should look like. */
static void copy_string(const u8 *strings, const u8 *limit, u8 n,
			char *out, size_t out_len)
{
	const u8 *p = strings;
	u8 index = 1;

	if (!n)
		return;

	while (p < limit && *p) {
		size_t len = 0;

		while (p + len < limit && p[len])
			len++;

		if (index == n) {
			if (len >= out_len)
				len = out_len - 1;

			kmemcpy(out, p, len);
			out[len] = '\0';
			return;
		}

		p += len + 1;
		index++;
	}
}

/* Walks past a structure: the formatted part, then the strings, then the pair
 * of zero bytes that ends them. Returns null at the end of the table or on
 * anything that does not make sense, which stops the walk rather than
 * continuing into whatever is next in memory. */
static const u8 *next_structure(const u8 *p, const u8 *limit)
{
	const struct smbios_header *h = (const struct smbios_header *)p;

	if (p + sizeof(*h) > limit)
		return NULL;

	if (h->length < sizeof(*h))
		return NULL;		/* a length that cannot be right */

	p += h->length;

	/* The strings, ending in two zero bytes. A structure with no strings at
	 * all still has the pair, so this is not a special case. */
	while (p + 1 < limit && !(p[0] == 0 && p[1] == 0))
		p++;

	if (p + 1 >= limit)
		return NULL;

	return p + 2;
}

void smbios_init(void)
{
	paddr_t anchor = 0;
	const u8 *e;
	const u8 *table, *limit, *p;
	u64 table_phys = 0;
	u32 table_len = 0;

	kmemset(&facts, 0, sizeof(facts));
	anchor_source = "nothing";

	/* The firmware's own answer first.
	 *
	 * `arch_smbios_anchor` sweeps the 64 KB below one megabyte sixteen
	 * bytes at a time, and its comment has said since it was written that
	 * the configuration table would be the better source -- *it needs no
	 * scan and it is correct on a machine whose legacy region is not
	 * populated* -- and that carrying the address through the handoff
	 * "would mean a protocol version bump".
	 *
	 * **It would not, and reconboot.h is the file that says so.** That
	 * header carries a `size` field and an append-only region under the
	 * words *appended rather than versioned, which is what `size` is for*,
	 * precisely because raising the version is expensive. Two fields
	 * already live there. This is the third. The reason the better source
	 * went unused was a cost recorded in one file and refuted in another.
	 *
	 * The scan stays, and is still the answer on every machine with no
	 * UEFI. It is no longer the first thing tried on a machine whose
	 * firmware is willing to say. */
	if (boot_info()->smbios &&
	    make_readable((paddr_t)boot_info()->smbios, 32)) {
		anchor = (paddr_t)boot_info()->smbios;
		anchor_source = "the firmware's configuration table";
	}

	if (!anchor) {
		anchor = arch_smbios_anchor();
		if (anchor)
			anchor_source = "a scan below one megabyte";
	}

	if (!anchor)
		return;

	e = phys_to_virt(anchor);

	/* Two entry-point formats, and which one this is decides where every
	 * field below lives. Checked by signature rather than by version,
	 * because the signature is what distinguishes them. */
	if (e[0] == '_' && e[1] == 'S' && e[2] == 'M' && e[3] == '3' &&
	    e[4] == '_') {
		facts.major = e[7];
		facts.minor = e[8];

		table_len = (u32)e[12] | ((u32)e[13] << 8) |
			    ((u32)e[14] << 16) | ((u32)e[15] << 24);

		for (unsigned i = 0; i < 8; i++)
			table_phys |= (u64)e[16 + i] << (i * 8);
	} else if (e[0] == '_' && e[1] == 'S' && e[2] == 'M' && e[3] == '_') {
		facts.major = e[6];
		facts.minor = e[7];

		table_len = (u32)e[0x16] | ((u32)e[0x17] << 8);

		table_phys = (u64)e[0x18] | ((u64)e[0x19] << 8) |
			     ((u64)e[0x1A] << 16) | ((u64)e[0x1B] << 24);
	} else {
		return;			/* not an anchor after all */
	}

	if (!table_phys || !table_len)
		return;

	/* A length nothing bounds is a walk with no end. Sixteen megabytes is
	 * far more than any real table and far less than a wrong number. */
	if (table_len > 16u * 1024 * 1024)
		return;

	/* Mapped before it is walked. It was not, and the only reason that has
	 * never faulted is that every table seen so far happened to sit inside
	 * the direct map. */
	if (!make_readable((paddr_t)table_phys, table_len))
		return;

	table = phys_to_virt((paddr_t)table_phys);
	limit = table + table_len;

	for (p = table; p && p < limit; ) {
		const struct smbios_header *h = (const struct smbios_header *)p;
		const u8 *strings;

		if (p + sizeof(*h) > limit)
			break;

		if (h->type == SMBIOS_TYPE_END)
			break;

		strings = p + h->length;
		facts.structures++;

		if (h->type == SMBIOS_TYPE_SYSTEM && h->length >= 8) {
			copy_string(strings, limit, p[4], facts.manufacturer,
				    sizeof(facts.manufacturer));
			copy_string(strings, limit, p[5], facts.product,
				    sizeof(facts.product));
			copy_string(strings, limit, p[6], facts.version,
				    sizeof(facts.version));
			copy_string(strings, limit, p[7], facts.serial,
				    sizeof(facts.serial));
		} else if (h->type == SMBIOS_TYPE_BIOS && h->length >= 6) {
			copy_string(strings, limit, p[4], facts.bios_vendor,
				    sizeof(facts.bios_vendor));
			copy_string(strings, limit, p[5], facts.bios_version,
				    sizeof(facts.bios_version));
		}

		p = next_structure(p, limit);
	}

	facts.present = facts.structures != 0;
}

bool smbios_read(struct smbios_facts *out)
{
	if (!facts.present)
		return false;

	*out = facts;
	return true;
}

void smbios_print_summary(void)
{
	if (!facts.present) {
		kputs("  machine      : the firmware published no SMBIOS "
		      "tables\n");
		return;
	}

	/* The model, then the firmware in it. The serial number is read and
	 * deliberately not printed: it identifies the physical machine, it ends
	 * up in every screenshot and bug report, and nothing here needs it on
	 * screen to be useful. */
	kprintf("  machine      : %s %s%s%s\n",
		facts.manufacturer[0] ? facts.manufacturer : "unnamed",
		facts.product[0] ? facts.product : "machine",
		facts.version[0] ? ", " : "",
		facts.version[0] ? facts.version : "");

	kprintf("  firmware by  : %s%s%s, SMBIOS %u.%u, %u structures\n",
		facts.bios_vendor[0] ? facts.bios_vendor : "somebody",
		facts.bios_version[0] ? " " : "",
		facts.bios_version[0] ? facts.bios_version : "",
		facts.major, facts.minor, facts.structures);

	/* Which of the two sources answered.
	 *
	 * Printed because the two agree on every machine where both work, so
	 * there is no other way to tell whether the new one is being used at
	 * all. Every emulated path here populates the legacy region, which
	 * means the scan has never once failed and a silent preference would be
	 * indistinguishable from a preference that is not taking effect. */
	kprintf("  found via    : %s\n", anchor_source);
}
