/* The AML parser. See aml.h for what it does and what it refuses to do.
 *
 * AML's encoding has three things worth understanding before reading any of
 * this, because each one is a place a parser goes quietly wrong:
 *
 *   PACKAGE LENGTHS are variable-width and *include their own bytes*. The top
 *   two bits of the first byte say how many more follow; with none, the length
 *   is six bits, and otherwise it is four bits from the first byte and eight
 *   from each of the rest. Getting the "includes itself" part wrong produces
 *   an offset that is correct for short objects and wrong for long ones.
 *
 *   NAMES are four characters, always, padded with underscores -- which is why
 *   the sleep state is "_S5_" and not "_S5". A parser that compares three
 *   characters finds it on some machines and not others.
 *
 *   MOST OPCODES HAVE NO LENGTH. Skipping one means knowing how many operands
 *   it takes and what shape each is. That is why this walks only the parts of
 *   the tree where every term is a declaration, and stops at anything else.
 */
#include <recon/kernel/aml.h>

#include <recon/kernel/acpi.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/vm.h>

static struct aml_state state;

/* --- opcodes, and only the ones a declaration can begin with -------------- */
#define OP_ZERO		0x00
#define OP_ONE		0x01
#define OP_ALIAS	0x06
#define OP_NAME		0x08
#define OP_BYTE		0x0A
#define OP_WORD		0x0B
#define OP_DWORD	0x0C
#define OP_STRING	0x0D
#define OP_QWORD	0x0E
#define OP_SCOPE	0x10
#define OP_BUFFER	0x11
#define OP_PACKAGE	0x12
#define OP_VAR_PACKAGE	0x13
#define OP_METHOD	0x14
#define OP_EXTERNAL	0x15
#define OP_EXT_PREFIX	0x5B
#define OP_ONES		0xFF

/* Second byte of a two-byte opcode. */
#define EXT_MUTEX	0x01
#define EXT_EVENT	0x02
#define EXT_OPREGION	0x80
#define EXT_FIELD	0x81
#define EXT_DEVICE	0x82
#define EXT_PROCESSOR	0x83
#define EXT_POWER_RES	0x84
#define EXT_THERMAL	0x85
#define EXT_INDEX_FIELD	0x86
#define EXT_BANK_FIELD	0x87

/* Two NUL-terminated names, compared. Local rather than added to kstring.h,
 * because two call sites is not a reason to grow the shared surface. */
static bool same(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}

	return *a == *b;
}

struct cursor {
	const u8 *p;
	const u8 *end;
	bool stopped;
};

static bool have(const struct cursor *c, size_t n)
{
	return (size_t)(c->end - c->p) >= n;
}

/* A package length: variable width, and it counts its own bytes. */
static bool pkg_length(struct cursor *c, u32 *out)
{
	u8 lead;
	unsigned follow, i;
	u32 v;

	if (!have(c, 1))
		return false;

	lead = *c->p;
	follow = (unsigned)(lead >> 6);

	if (!have(c, 1 + follow))
		return false;

	if (follow == 0) {
		v = lead & 0x3F;
	} else {
		v = lead & 0x0F;
		for (i = 0; i < follow; i++)
			v |= (u32)c->p[1 + i] << (4 + i * 8);
	}

	if (v < 1 + follow)
		return false;		/* shorter than its own header */

	c->p += 1 + follow;
	*out = v - (1 + follow);	/* what remains after the length itself */
	return true;
}

/* A name string. Only the final segment is kept: this builds a flat list of
 * devices rather than a path-addressable tree, and the segment is what
 * identifies a device to everything that asks. */
static bool name_string(struct cursor *c, char out[AML_NAME_LEN])
{
	unsigned segments = 1;

	if (out)
		out[0] = '\0';

	if (!have(c, 1))
		return false;

	while (have(c, 1) && (*c->p == '\\' || *c->p == '^'))
		c->p++;

	if (!have(c, 1))
		return false;

	if (*c->p == 0x00) {		/* NullName */
		c->p++;
		return true;
	}

	if (*c->p == 0x2E) {		/* two segments */
		c->p++;
		segments = 2;
	} else if (*c->p == 0x2F) {	/* a counted run of them */
		c->p++;
		if (!have(c, 1))
			return false;
		segments = *c->p++;
		if (segments == 0)
			return false;
	}

	if (!have(c, segments * 4u))
		return false;

	/* The last one is the object's own name; the ones before it are the
	 * path to it. */
	c->p += (segments - 1) * 4;

	if (out) {
		unsigned i;

		for (i = 0; i < 4; i++)
			out[i] = (char)c->p[i];
		out[4] = '\0';

		/* Trailing underscores are padding, not part of the name. */
		for (i = 4; i > 0 && out[i - 1] == '_'; i--)
			out[i - 1] = '\0';
	}

	c->p += 4;
	return true;
}

/* A constant data object, stepped over. Returns its integer value where it has
 * one, which is what _HID and the sleep package are made of. */
static bool data_object(struct cursor *c, u64 *value, bool *is_integer)
{
	u8 op;

	if (is_integer)
		*is_integer = false;

	if (!have(c, 1))
		return false;

	op = *c->p++;

	switch (op) {
	case OP_ZERO:
		if (value) *value = 0;
		if (is_integer) *is_integer = true;
		return true;
	case OP_ONE:
		if (value) *value = 1;
		if (is_integer) *is_integer = true;
		return true;
	case OP_ONES:
		if (value) *value = ~0ull;
		if (is_integer) *is_integer = true;
		return true;
	case OP_BYTE:
		if (!have(c, 1)) return false;
		if (value) *value = *c->p;
		if (is_integer) *is_integer = true;
		c->p += 1;
		return true;
	case OP_WORD:
		if (!have(c, 2)) return false;
		if (value) *value = (u64)c->p[0] | ((u64)c->p[1] << 8);
		if (is_integer) *is_integer = true;
		c->p += 2;
		return true;
	case OP_DWORD:
		if (!have(c, 4)) return false;
		if (value) *value = (u64)c->p[0] | ((u64)c->p[1] << 8) |
				    ((u64)c->p[2] << 16) | ((u64)c->p[3] << 24);
		if (is_integer) *is_integer = true;
		c->p += 4;
		return true;
	case OP_QWORD: {
		unsigned i;
		u64 v = 0;

		if (!have(c, 8)) return false;
		for (i = 0; i < 8; i++)
			v |= (u64)c->p[i] << (i * 8);
		if (value) *value = v;
		if (is_integer) *is_integer = true;
		c->p += 8;
		return true;
	}
	case OP_STRING:
		while (have(c, 1) && *c->p)
			c->p++;
		if (!have(c, 1))
			return false;
		c->p++;			/* the terminator */
		return true;
	case OP_BUFFER:
	case OP_PACKAGE:
	case OP_VAR_PACKAGE: {
		u32 len;
		const u8 *before = c->p;

		if (!pkg_length(c, &len))
			return false;
		if ((size_t)(c->end - c->p) < len)
			return false;

		(void)before;
		c->p += len;
		return true;
	}
	default:
		/* A name reference, or an expression. Either way this is not a
		 * constant and cannot be stepped over safely. */
		c->p--;
		return false;
	}
}

/* The two values inside \_S5_'s package, which is a package of small integers
 * and is parsed here rather than by the generic stepper because they are
 * wanted rather than skipped. */
static void read_s5_package(struct cursor *c)
{
	u32 len;
	struct cursor inner;
	unsigned count;
	u64 v;
	bool is_int;

	if (!have(c, 1) || *c->p != OP_PACKAGE)
		return;

	c->p++;

	if (!pkg_length(c, &len))
		return;
	if ((size_t)(c->end - c->p) < len)
		return;

	inner.p = c->p;
	inner.end = c->p + len;
	inner.stopped = false;

	c->p += len;			/* the outer walk continues past it */

	if (!have(&inner, 1))
		return;

	count = *inner.p++;

	if (count >= 1 && data_object(&inner, &v, &is_int) && is_int) {
		state.s5_typ_a = (u8)v;

		if (count >= 2 && data_object(&inner, &v, &is_int) && is_int)
			state.s5_typ_b = (u8)v;

		state.have_s5 = true;
	}
}

/* An EISA identifier, which is how _HID is usually written: three
 * five-bit letters and four hex digits, packed into a big-endian word inside a
 * little-endian integer. "PNP0303" is 0x0303D041. Decoded rather than compared
 * as a number so that the summary and the lookup both read as text. */
static void decode_eisa(u64 v, char out[AML_HID_LEN])
{
	static const char hex[] = "0123456789ABCDEF";
	u32 id = (u32)v;
	u16 vendor = (u16)(((id & 0xFF) << 8) | ((id >> 8) & 0xFF));

	out[0] = (char)('@' + ((vendor >> 10) & 0x1F));
	out[1] = (char)('@' + ((vendor >> 5) & 0x1F));
	out[2] = (char)('@' + (vendor & 0x1F));
	out[3] = hex[(id >> 20) & 0xF];
	out[4] = hex[(id >> 16) & 0xF];
	out[5] = hex[(id >> 28) & 0xF];
	out[6] = hex[(id >> 24) & 0xF];
	out[7] = '\0';
}

static void walk(struct cursor *c, int depth, char *device_name);

/* One declaration. Returns false to stop the whole walk, which is what happens
 * at an opcode this does not know: see the header for why guessing is worse
 * than stopping. */
static bool term(struct cursor *c, int depth, char *device_name)
{
	u8 op;
	char name[AML_NAME_LEN];
	u32 len;

	if (!have(c, 1))
		return false;

	op = *c->p++;

	switch (op) {
	case OP_NAME: {
		if (!name_string(c, name))
			return false;

		state.names++;

		/* The two names worth reading rather than stepping over. */
		if (same(name, "_S5")) {
			read_s5_package(c);
			return true;
		}

		if (same(name, "_HID") && device_name &&
		    state.devices < AML_MAX_DEVICES) {
			u64 v;
			bool is_int;
			const u8 *save = c->p;

			if (data_object(c, &v, &is_int)) {
				struct aml_device *d =
					&state.device[state.devices];

				kstrlcpy(d->name, device_name, sizeof(d->name));

				if (is_int)
					decode_eisa(v, d->hid);
				else
					d->hid[0] = '\0';

				state.devices++;
				return true;
			}

			c->p = save;
		}

		return data_object(c, NULL, NULL);
	}

	case OP_SCOPE: {
		struct cursor inner;

		if (!pkg_length(c, &len))
			return false;
		if ((size_t)(c->end - c->p) < len)
			return false;

		inner.p = c->p;
		inner.end = c->p + len;
		inner.stopped = false;

		if (!name_string(&inner, name))
			return false;

		walk(&inner, depth + 1, device_name);
		if (inner.stopped)
			c->stopped = true;

		c->p += len;
		return true;
	}

	case OP_METHOD: {
		if (!pkg_length(c, &len))
			return false;
		if ((size_t)(c->end - c->p) < len)
			return false;

		/* Located, counted, and stepped over entire. Its body is
		 * executable AML and this parser does not execute. */
		state.methods++;
		c->p += len;
		return true;
	}

	case OP_ALIAS:
		return name_string(c, NULL) && name_string(c, NULL);

	case OP_EXTERNAL:
		if (!name_string(c, NULL))
			return false;
		if (!have(c, 2))
			return false;
		c->p += 2;
		return true;

	case OP_EXT_PREFIX: {
		u8 ext;

		if (!have(c, 1))
			return false;

		ext = *c->p++;

		switch (ext) {
		case EXT_DEVICE: {
			struct cursor inner;
			char child[AML_NAME_LEN];

			if (!pkg_length(c, &len))
				return false;
			if ((size_t)(c->end - c->p) < len)
				return false;

			inner.p = c->p;
			inner.end = c->p + len;
			inner.stopped = false;

			if (!name_string(&inner, child))
				return false;

			walk(&inner, depth + 1, child);
			if (inner.stopped)
				c->stopped = true;

			c->p += len;
			return true;
		}

		/* All of these are package-length prefixed, so they can be
		 * stepped over exactly without knowing their contents. */
		case EXT_FIELD:
		case EXT_INDEX_FIELD:
		case EXT_BANK_FIELD:
		case EXT_PROCESSOR:
		case EXT_POWER_RES:
		case EXT_THERMAL:
			if (!pkg_length(c, &len))
				return false;
			if ((size_t)(c->end - c->p) < len)
				return false;
			c->p += len;
			return true;

		case EXT_OPREGION:
			/* A name, a space byte, then two data objects. */
			if (!name_string(c, NULL))
				return false;
			if (!have(c, 1))
				return false;
			c->p++;
			return data_object(c, NULL, NULL) &&
			       data_object(c, NULL, NULL);

		case EXT_MUTEX:
			if (!name_string(c, NULL))
				return false;
			if (!have(c, 1))
				return false;
			c->p++;
			return true;

		case EXT_EVENT:
			return name_string(c, NULL);

		default:
			state.stopped_on_opcode = ext;
			return false;
		}
	}

	default:
		state.stopped_on_opcode = op;
		return false;
	}
}

static void walk(struct cursor *c, int depth, char *device_name)
{
	/* Bounded, because a malformed table that made no progress would spin
	 * here for ever and a deeply nested one would exhaust the stack. Real
	 * namespaces are a handful deep. */
	if (depth > 16) {
		c->stopped = true;
		return;
	}

	while (c->p < c->end) {
		const u8 *before = c->p;

		if (!term(c, depth, device_name)) {
			c->stopped = true;
			return;
		}

		/* A term that consumed nothing is a term this did not
		 * understand, whatever it returned. */
		if (c->p == before) {
			c->stopped = true;
			return;
		}
	}
}

void aml_init(void)
{
	struct acpi_fadt_facts fadt;
	const struct acpi_sdt_header *dsdt;
	struct cursor c;

	kmemset(&state, 0, sizeof(state));

	if (!acpi_fadt(&fadt) || !fadt.dsdt)
		return;

	dsdt = phys_to_virt(fadt.dsdt);

	/* The DSDT is an ordinary table with an ordinary header, and the
	 * bytecode is everything after it. A length that does not even cover
	 * the header is a table to leave alone. */
	if (dsdt->length <= sizeof(*dsdt))
		return;

	c.p = (const u8 *)dsdt + sizeof(*dsdt);
	c.end = (const u8 *)dsdt + dsdt->length;
	c.stopped = false;

	walk(&c, 0, NULL);

	state.parsed = true;

	if (c.stopped)
		state.stopped_at = (u32)(c.p - (const u8 *)dsdt);
}

const struct aml_state *aml(void)
{
	return &state;
}

bool aml_has_device(const char *hid)
{
	unsigned i;

	for (i = 0; i < state.devices; i++)
		if (same(state.device[i].hid, hid))
			return true;

	return false;
}

void aml_print_summary(void)
{
	if (!state.parsed) {
		kputs("  aml          : no DSDT on this machine\n");
		return;
	}

	kprintf("  aml          : %u names, %u devices, %u methods stepped "
		"over\n", state.names, state.devices, state.methods);

	/* Said out loud, because a partial namespace that reports itself as a
	 * namespace is worse than no namespace: everything that asks it a
	 * question gets "no" for the part that was not read. */
	if (state.stopped_at)
		kprintf("  aml          : stopped at byte %u on opcode %02x -- "
			"the namespace is partial\n",
			state.stopped_at, state.stopped_on_opcode);

	if (state.have_s5)
		kprintf("  power off    : sleep state 5, values %u and %u\n",
			state.s5_typ_a, state.s5_typ_b);
	else
		kputs("  power off    : no _S5 in this table; the machine can "
		      "only be halted\n");
}
