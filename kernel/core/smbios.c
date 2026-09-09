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

#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/vm.h>

static struct smbios_facts facts;

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
	paddr_t anchor = arch_smbios_anchor();
	const u8 *e;
	const u8 *table, *limit, *p;
	u64 table_phys = 0;
	u32 table_len = 0;

	kmemset(&facts, 0, sizeof(facts));

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
}
