/* Running an AML method. See aml_eval.h for what this refuses and why.
 *
 * Three things about this file are decisions rather than consequences, and each
 * is the reason a piece of it looks smaller than an ACPI interpreter usually
 * does.
 *
 * NOTHING IS ALLOCATED FROM THE HEAP. Every object an evaluation makes comes
 * out of an arena that lives for exactly that call, and the header says so:
 * what a method returns stops being valid when the call returns. Reference
 * counting an ACPI object graph is the part of this that is hardest to get
 * right, and nothing in this kernel yet needs an object to outlive the call
 * that made it. When something does, that is the change to make, deliberately.
 *
 * A STORE MAY ONLY TARGET A LOCAL OR AN ARGUMENT. Storing to a named object is
 * refused, and that refusal is what makes `aml.c`'s table of named objects
 * *correct* rather than probably correct: a name holds what the table declared,
 * for the life of the machine, because nothing here can change one.
 *
 * AN OPERATION REGION IS NEVER TOUCHED. Reading one is memory, an I/O port, PCI
 * configuration space or embedded-controller traffic. A method that needs one
 * is refused by name -- AML_NEEDS_HARDWARE -- rather than handed a zero, for
 * the reason the parser stops at an opcode it does not know: a `_CRS` evaluated
 * to a plausible wrong buffer is an interrupt number a driver will program into
 * a controller.
 */
#include <recon/kernel/aml_eval.h>

#include <recon/kernel/aml.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>

/* --- the bounds -------------------------------------------------------------
 *
 * Four of them, separately, because they fail differently and a caller that is
 * told only "it did not finish" cannot tell a loop that will never end from a
 * table larger than this kernel will hold.
 *
 * The numbers are chosen to be far past anything a `_CRS` or a `_DSM` does and
 * far short of a machine that stops responding. A method that reaches one is
 * refused with that bound named. */
#define BUDGET_TERMS	20000	/* terms evaluated, in total */
#define MAX_DEPTH	8	/* method calls nested */
#define MAX_LOOPS	4096	/* iterations of one While */
#define MAX_OBJECTS	256	/* values one evaluation may make */
#define ARENA_BYTES	4096	/* bytes of constructed buffer */
#define MAX_PKG_ITEMS	64

/* --- opcodes ---------------------------------------------------------------- */
#define OP_ZERO		0x00
#define OP_ONE		0x01
#define OP_BYTE		0x0A
#define OP_WORD		0x0B
#define OP_DWORD	0x0C
#define OP_STRING	0x0D
#define OP_QWORD	0x0E
#define OP_BUFFER	0x11
#define OP_PACKAGE	0x12
#define OP_VAR_PACKAGE	0x13
#define OP_EXT_PREFIX	0x5B
#define OP_LOCAL0	0x60
#define OP_ARG0		0x68
#define OP_STORE	0x70
#define OP_CONCAT	0x73
#define OP_ADD		0x72
#define OP_SUBTRACT	0x74
#define OP_INCREMENT	0x75
#define OP_DECREMENT	0x76
#define OP_MULTIPLY	0x77
#define OP_DIVIDE	0x78
#define OP_SHIFT_LEFT	0x79
#define OP_SHIFT_RIGHT	0x7A
#define OP_AND		0x7B
#define OP_NAND		0x7C
#define OP_OR		0x7D
#define OP_NOR		0x7E
#define OP_XOR		0x7F
#define OP_NOT		0x80
#define OP_DEREF_OF	0x83
#define OP_NOTIFY	0x86
#define OP_SIZE_OF	0x87
#define OP_INDEX	0x88
#define OP_LAND		0x90
#define OP_LOR		0x91
#define OP_LNOT		0x92
#define OP_LEQUAL	0x93
#define OP_LGREATER	0x94
#define OP_LLESS	0x95
#define OP_TO_INTEGER	0x99
#define OP_CONTINUE	0x9F
#define OP_IF		0xA0
#define OP_ELSE		0xA1
#define OP_WHILE	0xA2
#define OP_NOOP		0xA3
#define OP_RETURN	0xA4
#define OP_BREAK	0xA5
#define OP_ONES		0xFF

/* --- what an evaluation is doing --------------------------------------------
 *
 * `flow` is how a term list ended, and it is separate from `result` on purpose:
 * a `Return` is not an error and neither is a `Break`, but both stop the list
 * they are in, and a function that folded them into the failure value would
 * make every caller check whether the failure was really a failure. */
enum flow {
	FLOW_NORMAL = 0,
	FLOW_RETURN,
	FLOW_BREAK,
	FLOW_CONTINUE,
};

struct ctx {
	struct aml_value *local[8];
	struct aml_value *arg[7];

	struct aml_value objects[MAX_OBJECTS];
	unsigned used;

	u8 arena[ARENA_BYTES];
	u32 arena_used;

	struct aml_value *items[MAX_PKG_ITEMS * 8];
	unsigned items_used;

	u32 budget;
	int depth;

	enum aml_result result;
	enum flow flow;
	struct aml_value *returned;
};

struct span {
	const u8 *p;
	const u8 *end;
};

/* One field per thing said, rather than one `last_op` every refusal writes.
 *
 * The first version shared it, and the survey's first run printed
 * `15 met an opcode this does not run (last: 5b 23)` -- where `5b 23` is
 * Acquire, a mutex, refused as hardware by a different branch entirely. A right
 * number with a wrong label sends the reader to implement something that is
 * already handled on purpose. */
static struct {
	unsigned asked;
	unsigned returned;
	unsigned refused_op;
	unsigned refused_hardware;
	unsigned refused_bound;
	unsigned refused_missing;

	u8 last_unknown;		/* set only with AML_UNSUPPORTED_OP */
	bool last_unknown_ext;
	bool last_unknown_was_name;
	bool last_unknown_was_store;

	u8 last_hw;			/* set only with AML_NEEDS_HARDWARE */
	bool last_hw_ext;

	enum aml_result last_bound;
} counters;

const char *aml_why(enum aml_result r)
{
	switch (r) {
	case AML_OK:              return "it returned";
	case AML_NO_METHOD:       return "this machine declares no such method";
	case AML_BAD_ARGS:        return "it takes a different number of arguments";
	case AML_UNSUPPORTED_OP:  return "an opcode this evaluator does not run";
	case AML_NEEDS_HARDWARE:  return "it reads or writes an operation region";
	case AML_OUT_OF_OBJECTS:  return "it made more objects than the arena holds";
	case AML_TOO_LONG:        return "it ran past the instruction budget";
	case AML_TOO_DEEP:        return "it nested calls past the limit";
	case AML_TOO_MANY_LOOPS:  return "a loop ran past its iteration limit";
	case AML_TRUNCATED:       return "its body ended in the middle of a term";
	case AML_TYPE:            return "an operand was the wrong kind of thing";
	default:                  return "for a reason with no name";
	}
}

/* --- the arena -------------------------------------------------------------- */

static struct aml_value *new_value(struct ctx *x)
{
	struct aml_value *v;

	if (x->used >= MAX_OBJECTS) {
		x->result = AML_OUT_OF_OBJECTS;
		return NULL;
	}

	v = &x->objects[x->used++];
	kmemset(v, 0, sizeof(*v));
	return v;
}

static struct aml_value *new_int(struct ctx *x, u64 n)
{
	struct aml_value *v = new_value(x);

	if (v) {
		v->type = AML_TYPE_INT;
		v->integer = n;
	}
	return v;
}

/* Bytes that are not in the table: a buffer longer than its byte list, or one
 * a method built. Zeroed, because AML says the tail of a short buffer is. */
static u8 *arena_bytes(struct ctx *x, u32 n)
{
	u8 *p;

	if (n > ARENA_BYTES - x->arena_used) {
		x->result = AML_OUT_OF_OBJECTS;
		return NULL;
	}

	p = x->arena + x->arena_used;
	x->arena_used += n;
	kmemset(p, 0, n);
	return p;
}

/* --- reading the encoding ---------------------------------------------------- */

static bool span_have(const struct span *s, size_t n)
{
	return (size_t)(s->end - s->p) >= n;
}

static bool pkg_length(struct span *s, u32 *out)
{
	u8 lead;
	unsigned follow, i;
	u32 v;

	if (!span_have(s, 1))
		return false;

	lead = *s->p;
	follow = (unsigned)(lead >> 6);

	if (!span_have(s, 1 + follow))
		return false;

	if (follow == 0) {
		v = lead & 0x3F;
	} else {
		v = lead & 0x0F;
		for (i = 0; i < follow; i++)
			v |= (u32)s->p[1 + i] << (4 + i * 8);
	}

	if (v < 1 + follow)
		return false;

	s->p += 1 + follow;
	*out = v - (1 + follow);
	return true;
}

/* The final segment of a name, which is what `aml.c` keys its tables on. */
static bool name_string(struct span *s, char out[AML_NAME_LEN])
{
	unsigned segments = 1, i;

	out[0] = '\0';

	while (span_have(s, 1) && (*s->p == '\\' || *s->p == '^'))
		s->p++;

	if (!span_have(s, 1))
		return false;

	if (*s->p == 0x00) {
		s->p++;
		return true;
	}

	if (*s->p == 0x2E) {
		s->p++;
		segments = 2;
	} else if (*s->p == 0x2F) {
		s->p++;
		if (!span_have(s, 1))
			return false;
		segments = *s->p++;
		if (segments == 0)
			return false;
	}

	if (!span_have(s, segments * 4u))
		return false;

	s->p += (segments - 1) * 4;

	for (i = 0; i < 4; i++)
		out[i] = (char)s->p[i];
	out[4] = '\0';

	for (i = 4; i > 0 && out[i - 1] == '_'; i--)
		out[i - 1] = '\0';

	s->p += 4;
	return true;
}

static bool names_equal(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

/* --- finding things in the namespace ------------------------------------------
 *
 * Preferring the enclosing device and falling back to any scope, in that order,
 * because a machine has many methods called `_CRS` and what they are inside is
 * the only thing that tells them apart -- but a method at the top level has no
 * device and must still be findable. */
static const struct aml_method *find_method(const char *device, const char *name)
{
	const struct aml_state *st = aml();
	unsigned i;

	if (device && device[0])
		for (i = 0; i < st->methods_kept; i++)
			if (names_equal(st->method[i].name, name) &&
			    names_equal(st->method[i].device, device))
				return &st->method[i];

	for (i = 0; i < st->methods_kept; i++)
		if (names_equal(st->method[i].name, name))
			return &st->method[i];

	return NULL;
}

static const struct aml_name *find_name(const char *device, const char *name)
{
	const struct aml_state *st = aml();
	unsigned i;

	if (device && device[0])
		for (i = 0; i < st->names_kept; i++)
			if (names_equal(st->name_obj[i].name, name) &&
			    names_equal(st->name_obj[i].device, device))
				return &st->name_obj[i];

	for (i = 0; i < st->names_kept; i++)
		if (names_equal(st->name_obj[i].name, name))
			return &st->name_obj[i];

	return NULL;
}

/* --- evaluation ------------------------------------------------------------- */

static struct aml_value *eval_term(struct ctx *x, struct span *s,
				   const char *device);

static bool as_integer(struct ctx *x, struct aml_value *v, u64 *out)
{
	if (!v)
		return false;

	if (v->type != AML_TYPE_INT) {
		x->result = AML_TYPE;
		return false;
	}

	*out = v->integer;
	return true;
}

/* Two operands and an optional target, which is the shape of almost every
 * arithmetic opcode in AML. The target is evaluated as a store destination and
 * may be the null name, which means "discard". */
static bool two_operands(struct ctx *x, struct span *s, const char *device,
			 u64 *a, u64 *b)
{
	struct aml_value *va, *vb;

	va = eval_term(x, s, device);
	if (!va || !as_integer(x, va, a))
		return false;

	vb = eval_term(x, s, device);
	if (!vb || !as_integer(x, vb, b))
		return false;

	return true;
}

/* Where a Store may land.
 *
 * Locals and arguments only. A named object is refused, and that refusal is
 * load-bearing: `aml.c` records what each name was *declared* as, and reading
 * one is correct only because nothing can write it. See the file comment. */
static bool store_to(struct ctx *x, struct span *s, struct aml_value *v)
{
	u8 op;

	if (!span_have(s, 1)) {
		x->result = AML_TRUNCATED;
		return false;
	}

	op = *s->p;

	if (op == 0x00) {		/* the null name: the result is dropped */
		s->p++;
		return true;
	}

	if (op >= OP_LOCAL0 && op < OP_LOCAL0 + 8) {
		s->p++;
		x->local[op - OP_LOCAL0] = v;
		return true;
	}

	if (op >= OP_ARG0 && op < OP_ARG0 + 7) {
		s->p++;
		x->arg[op - OP_ARG0] = v;
		return true;
	}

	/* Everything else -- a name, an Index, a field -- is refused rather
	 * than approximated. Storing to a name would make aml.c's table a
	 * guess from that moment on. */
	counters.last_unknown = op;
	counters.last_unknown_ext = false;
	counters.last_unknown_was_name = false;
	counters.last_unknown_was_store = true;
	x->result = AML_UNSUPPORTED_OP;
	return false;
}

static struct aml_value *eval_buffer(struct ctx *x, struct span *s,
				     const char *device)
{
	u32 len;
	struct span inner;
	struct aml_value *size, *v;
	u64 want;
	u32 have_bytes;

	if (!pkg_length(s, &len) || !span_have(s, len)) {
		x->result = AML_TRUNCATED;
		return NULL;
	}

	inner.p = s->p;
	inner.end = s->p + len;
	s->p += len;

	size = eval_term(x, &inner, device);
	if (!size || !as_integer(x, size, &want))
		return NULL;

	have_bytes = (u32)(inner.end - inner.p);

	v = new_value(x);
	if (!v)
		return NULL;

	v->type = AML_TYPE_BUFFER;
	v->length = (u32)want;

	if (want <= have_bytes) {
		/* Points straight into the table. Nothing is copied, which is
		 * why what this returns dies with the call. */
		v->bytes = inner.p;
	} else {
		/* Longer than its own byte list, so the tail is zeroes and
		 * there is nowhere to point but the arena. */
		u8 *b = arena_bytes(x, (u32)want);

		if (!b)
			return NULL;

		kmemcpy(b, inner.p, have_bytes);
		v->bytes = b;
	}

	return v;
}

static struct aml_value *eval_package(struct ctx *x, struct span *s,
				      const char *device, bool var)
{
	u32 len;
	struct span inner;
	struct aml_value *v;
	u64 declared = 0;
	unsigned n = 0;
	struct aml_value **slot;

	if (!pkg_length(s, &len) || !span_have(s, len)) {
		x->result = AML_TRUNCATED;
		return NULL;
	}

	inner.p = s->p;
	inner.end = s->p + len;
	s->p += len;

	if (var) {
		struct aml_value *c = eval_term(x, &inner, device);

		if (!c || !as_integer(x, c, &declared))
			return NULL;
	} else {
		if (!span_have(&inner, 1)) {
			x->result = AML_TRUNCATED;
			return NULL;
		}
		declared = *inner.p++;
	}

	if (declared > MAX_PKG_ITEMS) {
		x->result = AML_OUT_OF_OBJECTS;
		return NULL;
	}

	if (x->items_used + declared > MAX_PKG_ITEMS * 8) {
		x->result = AML_OUT_OF_OBJECTS;
		return NULL;
	}

	slot = &x->items[x->items_used];
	x->items_used += (unsigned)declared;

	/* **The declared count wins over what is there.** A package may list
	 * fewer elements than it declares and the rest are uninitialised, which
	 * is a thing firmware does; reading past the element list would be
	 * reading the next term as an element. */
	while (n < declared && inner.p < inner.end) {
		struct aml_value *e = eval_term(x, &inner, device);

		if (!e)
			return NULL;

		slot[n++] = e;
	}

	while (n < declared) {
		struct aml_value *e = new_value(x);

		if (!e)
			return NULL;
		slot[n++] = e;		/* AML_TYPE_NONE: declared, never set */
	}

	v = new_value(x);
	if (!v)
		return NULL;

	v->type = AML_TYPE_PACKAGE;
	v->items = slot;
	v->count = (u32)declared;
	return v;
}

static bool eval_list(struct ctx *x, struct span *s, const char *device);

/* One term. Returns the value it produced, or null with `x->result` set.
 *
 * A term that is a statement rather than an expression -- If, While, Return --
 * returns a value nobody reads, because the two are not distinguishable by
 * their opcode alone in every position AML allows. `x->flow` is how the
 * statement ones say what happened. */
static struct aml_value *eval_term(struct ctx *x, struct span *s,
				   const char *device)
{
	u8 op;
	u64 a, b;

	if (x->result != AML_OK)
		return NULL;

	if (x->budget == 0) {
		x->result = AML_TOO_LONG;
		return NULL;
	}
	x->budget--;

	if (!span_have(s, 1)) {
		x->result = AML_TRUNCATED;
		return NULL;
	}

	op = *s->p++;

	switch (op) {
	case OP_ZERO:  return new_int(x, 0);
	case OP_ONE:   return new_int(x, 1);
	case OP_ONES:  return new_int(x, ~0ull);

	case OP_BYTE:
		if (!span_have(s, 1)) { x->result = AML_TRUNCATED; return NULL; }
		a = *s->p;
		s->p += 1;
		return new_int(x, a);

	case OP_WORD:
		if (!span_have(s, 2)) { x->result = AML_TRUNCATED; return NULL; }
		a = (u64)s->p[0] | ((u64)s->p[1] << 8);
		s->p += 2;
		return new_int(x, a);

	case OP_DWORD:
		if (!span_have(s, 4)) { x->result = AML_TRUNCATED; return NULL; }
		a = (u64)s->p[0] | ((u64)s->p[1] << 8) |
		    ((u64)s->p[2] << 16) | ((u64)s->p[3] << 24);
		s->p += 4;
		return new_int(x, a);

	case OP_QWORD: {
		unsigned i;

		if (!span_have(s, 8)) { x->result = AML_TRUNCATED; return NULL; }
		a = 0;
		for (i = 0; i < 8; i++)
			a |= (u64)s->p[i] << (i * 8);
		s->p += 8;
		return new_int(x, a);
	}

	case OP_STRING: {
		struct aml_value *v = new_value(x);
		const u8 *from = s->p;

		while (span_have(s, 1) && *s->p)
			s->p++;
		if (!span_have(s, 1)) { x->result = AML_TRUNCATED; return NULL; }

		if (!v)
			return NULL;

		v->type = AML_TYPE_STRING;
		v->bytes = from;
		v->length = (u32)(s->p - from);
		s->p++;			/* the terminator */
		return v;
	}

	case OP_BUFFER:
		return eval_buffer(x, s, device);

	case OP_PACKAGE:
		return eval_package(x, s, device, false);

	case OP_VAR_PACKAGE:
		return eval_package(x, s, device, true);

	case OP_STORE: {
		struct aml_value *v = eval_term(x, s, device);

		if (!v)
			return NULL;
		if (!store_to(x, s, v))
			return NULL;
		return v;
	}

	case OP_ADD: case OP_SUBTRACT: case OP_MULTIPLY:
	case OP_AND: case OP_NAND: case OP_OR: case OP_NOR: case OP_XOR:
	case OP_SHIFT_LEFT: case OP_SHIFT_RIGHT: {
		u64 r = 0;
		struct aml_value *v;

		if (!two_operands(x, s, device, &a, &b))
			return NULL;

		switch (op) {
		case OP_ADD:         r = a + b; break;
		case OP_SUBTRACT:    r = a - b; break;
		case OP_MULTIPLY:    r = a * b; break;
		case OP_AND:         r = a & b; break;
		case OP_NAND:        r = ~(a & b); break;
		case OP_OR:          r = a | b; break;
		case OP_NOR:         r = ~(a | b); break;
		case OP_XOR:         r = a ^ b; break;
		/* A shift of 64 or more is undefined in C and zero in AML, so
		 * it is written rather than left to the compiler. */
		case OP_SHIFT_LEFT:  r = b >= 64 ? 0 : a << b; break;
		case OP_SHIFT_RIGHT: r = b >= 64 ? 0 : a >> b; break;
		}

		v = new_int(x, r);
		if (!v || !store_to(x, s, v))
			return NULL;
		return v;
	}

	case OP_DIVIDE: {
		struct aml_value *q;

		if (!two_operands(x, s, device, &a, &b))
			return NULL;

		/* Divide by zero is a fault in AML, and this has nowhere to
		 * raise one -- so it is a refusal with its own shape rather
		 * than a quotient nobody asked for. */
		if (b == 0) {
			x->result = AML_TYPE;
			return NULL;
		}

		q = new_int(x, a % b);		/* remainder first, then quotient */
		if (!q || !store_to(x, s, q))
			return NULL;

		q = new_int(x, a / b);
		if (!q || !store_to(x, s, q))
			return NULL;
		return q;
	}

	case OP_NOT: {
		struct aml_value *v = eval_term(x, s, device);

		if (!v || !as_integer(x, v, &a))
			return NULL;
		v = new_int(x, ~a);
		if (!v || !store_to(x, s, v))
			return NULL;
		return v;
	}

	case OP_INCREMENT:
	case OP_DECREMENT: {
		const u8 *before = s->p;
		struct aml_value *v = eval_term(x, s, device);
		struct aml_value *r;
		struct span target;

		if (!v || !as_integer(x, v, &a))
			return NULL;

		r = new_int(x, op == OP_INCREMENT ? a + 1 : a - 1);
		if (!r)
			return NULL;

		/* The operand is also the destination, so it is read once and
		 * then stored to from where it started. */
		target.p = before;
		target.end = s->p;
		if (!store_to(x, &target, r))
			return NULL;
		return r;
	}

	case OP_LAND: case OP_LOR: {
		struct aml_value *v;

		if (!two_operands(x, s, device, &a, &b))
			return NULL;

		v = new_int(x, op == OP_LAND ? (a && b) : (a || b));
		return v;
	}

	case OP_LEQUAL: case OP_LGREATER: case OP_LLESS: {
		if (!two_operands(x, s, device, &a, &b))
			return NULL;

		switch (op) {
		case OP_LEQUAL:   return new_int(x, a == b ? ~0ull : 0);
		case OP_LGREATER: return new_int(x, a > b ? ~0ull : 0);
		default:          return new_int(x, a < b ? ~0ull : 0);
		}
	}

	case OP_LNOT: {
		/* `LNot` is also the first byte of the three comparisons AML
		 * has no opcode of its own for: LNotEqual, LLessEqual and
		 * LGreaterEqual are LNot followed by the opposite test. */
		if (span_have(s, 1) &&
		    (*s->p == OP_LEQUAL || *s->p == OP_LGREATER ||
		     *s->p == OP_LLESS)) {
			u8 inner = *s->p++;

			if (!two_operands(x, s, device, &a, &b))
				return NULL;

			switch (inner) {
			case OP_LEQUAL:   return new_int(x, a != b ? ~0ull : 0);
			case OP_LGREATER: return new_int(x, a <= b ? ~0ull : 0);
			default:          return new_int(x, a >= b ? ~0ull : 0);
			}
		}

		{
			struct aml_value *v = eval_term(x, s, device);

			if (!v || !as_integer(x, v, &a))
				return NULL;
			return new_int(x, a ? 0 : ~0ull);
		}
	}

	case OP_SIZE_OF: {
		struct aml_value *v = eval_term(x, s, device);

		if (!v)
			return NULL;

		switch (v->type) {
		case AML_TYPE_BUFFER:
		case AML_TYPE_STRING:  return new_int(x, v->length);
		case AML_TYPE_PACKAGE: return new_int(x, v->count);
		default:
			x->result = AML_TYPE;
			return NULL;
		}
	}

	case OP_TO_INTEGER: {
		struct aml_value *v = eval_term(x, s, device);
		struct aml_value *r;

		if (!v)
			return NULL;

		if (v->type == AML_TYPE_INT) {
			r = new_int(x, v->integer);
		} else if (v->type == AML_TYPE_BUFFER) {
			u64 n = 0;
			u32 i, take = v->length < 8 ? v->length : 8;

			for (i = 0; i < take; i++)
				n |= (u64)v->bytes[i] << (i * 8);
			r = new_int(x, n);
		} else {
			/* A string would need a parser with a base and a sign,
			 * and nothing has asked. Refused rather than guessed. */
			x->result = AML_TYPE;
			return NULL;
		}

		if (!r || !store_to(x, s, r))
			return NULL;
		return r;
	}

	case OP_INDEX: {
		struct aml_value *src = eval_term(x, s, device);
		struct aml_value *idx, *r;
		u64 n;

		if (!src)
			return NULL;

		idx = eval_term(x, s, device);
		if (!idx || !as_integer(x, idx, &n))
			return NULL;

		/* **Out of range is a refusal, not a clamp.** A clamp hands
		 * back element zero for an index nobody meant, which is the
		 * wrong answer wearing the right shape. */
		if (src->type == AML_TYPE_PACKAGE) {
			if (n >= src->count) {
				x->result = AML_TYPE;
				return NULL;
			}
			r = src->items[n];
		} else if (src->type == AML_TYPE_BUFFER ||
			   src->type == AML_TYPE_STRING) {
			if (n >= src->length) {
				x->result = AML_TYPE;
				return NULL;
			}
			r = new_int(x, src->bytes[n]);
		} else {
			x->result = AML_TYPE;
			return NULL;
		}

		if (!r || !store_to(x, s, r))
			return NULL;
		return r;
	}

	case OP_DEREF_OF:
		/* Index already hands back the element rather than a reference
		 * to it, so a DerefOf of one is the element again. That is a
		 * simplification and it is only sound because nothing here can
		 * make a reference to anything else. */
		return eval_term(x, s, device);

	case OP_NOOP:
		return new_int(x, 0);

	case OP_BREAK:
		x->flow = FLOW_BREAK;
		return new_int(x, 0);

	case OP_CONTINUE:
		x->flow = FLOW_CONTINUE;
		return new_int(x, 0);

	case OP_RETURN: {
		struct aml_value *v = eval_term(x, s, device);

		if (!v)
			return NULL;
		x->returned = v;
		x->flow = FLOW_RETURN;
		return v;
	}

	case OP_IF: {
		u32 len;
		struct span body;
		struct aml_value *cond;
		u64 taken;

		if (!pkg_length(s, &len) || !span_have(s, len)) {
			x->result = AML_TRUNCATED;
			return NULL;
		}

		body.p = s->p;
		body.end = s->p + len;
		s->p += len;

		cond = eval_term(x, &body, device);
		if (!cond || !as_integer(x, cond, &taken))
			return NULL;

		if (taken) {
			if (!eval_list(x, &body, device))
				return NULL;

			/* An Else after a taken If is skipped whole. */
			if (span_have(s, 1) && *s->p == OP_ELSE) {
				struct span e = *s;
				u32 elen;

				e.p++;
				if (!pkg_length(&e, &elen) ||
				    !span_have(&e, elen)) {
					x->result = AML_TRUNCATED;
					return NULL;
				}
				s->p = e.p + elen;
			}
		} else if (span_have(s, 1) && *s->p == OP_ELSE) {
			struct span e;
			u32 elen;

			s->p++;
			if (!pkg_length(s, &elen) || !span_have(s, elen)) {
				x->result = AML_TRUNCATED;
				return NULL;
			}

			e.p = s->p;
			e.end = s->p + elen;
			s->p += elen;

			if (!eval_list(x, &e, device))
				return NULL;
		}

		return new_int(x, 0);
	}

	case OP_ELSE:
		/* Only reachable as a stray one: a taken or untaken If consumes
		 * its own Else above. */
		counters.last_unknown = op;
		counters.last_unknown_ext = false;
		counters.last_unknown_was_name = false;
		counters.last_unknown_was_store = false;
		x->result = AML_UNSUPPORTED_OP;
		return NULL;

	case OP_WHILE: {
		u32 len;
		struct span body;
		unsigned rounds = 0;

		if (!pkg_length(s, &len) || !span_have(s, len)) {
			x->result = AML_TRUNCATED;
			return NULL;
		}

		body.p = s->p;
		body.end = s->p + len;
		s->p += len;

		for (;;) {
			struct span round = body;
			struct aml_value *cond;
			u64 go;

			if (++rounds > MAX_LOOPS) {
				x->result = AML_TOO_MANY_LOOPS;
				return NULL;
			}

			cond = eval_term(x, &round, device);
			if (!cond || !as_integer(x, cond, &go))
				return NULL;

			if (!go)
				break;

			if (!eval_list(x, &round, device))
				return NULL;

			if (x->flow == FLOW_BREAK) {
				x->flow = FLOW_NORMAL;
				break;
			}
			if (x->flow == FLOW_CONTINUE)
				x->flow = FLOW_NORMAL;
			if (x->flow == FLOW_RETURN)
				break;
		}

		return new_int(x, 0);
	}

	case OP_NOTIFY:
		/* Makes the machine do something rather than say something. */
		counters.last_hw = op;
		counters.last_hw_ext = false;
		x->result = AML_NEEDS_HARDWARE;
		return NULL;

	case OP_EXT_PREFIX: {
		u8 ext;

		if (!span_have(s, 1)) {
			x->result = AML_TRUNCATED;
			return NULL;
		}

		ext = *s->p++;
		counters.last_hw = ext;
		counters.last_hw_ext = true;

		/* Every two-byte opcode this could meet inside a method is
		 * either an operation region, a mutex, an event or a debug
		 * object -- hardware or synchronisation in every case. Named
		 * as one refusal rather than a list of them, because the answer
		 * is the same and the list would be a list of things nobody has
		 * needed. */
		x->result = AML_NEEDS_HARDWARE;
		return NULL;
	}

	default:
		if (op >= OP_LOCAL0 && op < OP_LOCAL0 + 8) {
			struct aml_value *v = x->local[op - OP_LOCAL0];

			/* An unset local reads as zero, which AML requires --
			 * and is the one place here a missing thing is not a
			 * refusal, because the specification says what it is. */
			return v ? v : new_int(x, 0);
		}

		if (op >= OP_ARG0 && op < OP_ARG0 + 7) {
			struct aml_value *v = x->arg[op - OP_ARG0];

			return v ? v : new_int(x, 0);
		}

		/* A name: either a method to call or a named object to read. */
		if (op == '\\' || op == '^' || op == 0x2E || op == 0x2F ||
		    op == '_' || (op >= 'A' && op <= 'Z')) {
			char nm[AML_NAME_LEN];
			const struct aml_method *m;
			const struct aml_name *n;

			s->p--;
			if (!name_string(s, nm)) {
				x->result = AML_TRUNCATED;
				return NULL;
			}

			m = find_method(device, nm);
			if (m) {
				struct aml_value *argv[7];
				unsigned i;

				if (x->depth >= MAX_DEPTH) {
					x->result = AML_TOO_DEEP;
					return NULL;
				}

				for (i = 0; i < m->args; i++) {
					argv[i] = eval_term(x, s, device);
					if (!argv[i])
						return NULL;
				}

				{
					struct span body;
					struct aml_value *saved_local[8];
					struct aml_value *saved_arg[7];
					struct aml_value *ret;
					enum flow saved_flow = x->flow;

					kmemcpy(saved_local, x->local,
						sizeof(saved_local));
					kmemcpy(saved_arg, x->arg,
						sizeof(saved_arg));
					kmemset(x->local, 0, sizeof(x->local));
					kmemset(x->arg, 0, sizeof(x->arg));

					for (i = 0; i < m->args; i++)
						x->arg[i] = argv[i];

					body.p = m->body;
					body.end = m->body + m->body_len;

					x->depth++;
					x->flow = FLOW_NORMAL;
					x->returned = NULL;

					if (!eval_list(x, &body, m->device[0] ?
						       m->device : device)) {
						x->depth--;
						return NULL;
					}

					ret = x->returned;
					x->depth--;
					x->flow = saved_flow;
					kmemcpy(x->local, saved_local,
						sizeof(saved_local));
					kmemcpy(x->arg, saved_arg,
						sizeof(saved_arg));

					/* A method that falls off its end
					 * returns zero, which AML says. */
					return ret ? ret : new_int(x, 0);
				}
			}

			n = find_name(device, nm);
			if (n) {
				struct span obj;

				obj.p = n->object;
				obj.end = n->object + n->object_len;
				return eval_term(x, &obj, device);
			}

			/* A name that is neither a method nor a named object.
			 * Almost always one declared inside a conditional the
			 * parser stepped over -- see aml.c's count of those. */
			counters.last_unknown = 0;
			counters.last_unknown_ext = false;
			counters.last_unknown_was_name = true;
			counters.last_unknown_was_store = false;
			x->result = AML_UNSUPPORTED_OP;
			return NULL;
		}

		counters.last_unknown = op;
		counters.last_unknown_ext = false;
		counters.last_unknown_was_name = false;
		counters.last_unknown_was_store = false;
		x->result = AML_UNSUPPORTED_OP;
		return NULL;
	}
}

/* A list of terms, to the end of the span or until one of them says stop. */
static bool eval_list(struct ctx *x, struct span *s, const char *device)
{
	while (s->p < s->end) {
		if (!eval_term(x, s, device))
			return false;

		if (x->flow != FLOW_NORMAL)
			return true;
	}

	return true;
}

/* --- the way in -------------------------------------------------------------- */

static void count_refusal(enum aml_result r)
{
	switch (r) {
	case AML_UNSUPPORTED_OP:  counters.refused_op++; break;
	case AML_NEEDS_HARDWARE:  counters.refused_hardware++; break;
	case AML_NO_METHOD:
	case AML_BAD_ARGS:        counters.refused_missing++; break;
	default:
		counters.refused_bound++;
		counters.last_bound = r;
		break;
	}
}

enum aml_result aml_eval(const char *device, const char *method,
			 const struct aml_value *args, unsigned argc,
			 struct aml_value *out)
{
	/* One at a time, and the arena is the reason: it is a quarter of a
	 * kilobyte of values and four kilobytes of bytes, and two evaluations
	 * sharing it would each see the other's objects. Static rather than on
	 * the stack because a kernel stack is 64KB and this is most of one. */
	static struct ctx x;
	static bool busy;

	const struct aml_method *m;
	struct span body;
	unsigned i;

	counters.asked++;

	if (busy) {
		/* A method calling back into this from a driver would be the
		 * only way here, and it is refused rather than made to work:
		 * re-entering would hand the outer evaluation's objects to the
		 * inner one. */
		count_refusal(AML_TOO_DEEP);
		return AML_TOO_DEEP;
	}

	m = find_method(device, method);
	if (!m) {
		count_refusal(AML_NO_METHOD);
		return AML_NO_METHOD;
	}

	if (argc != m->args) {
		count_refusal(AML_BAD_ARGS);
		return AML_BAD_ARGS;
	}

	busy = true;
	kmemset(&x, 0, sizeof(x));
	x.budget = BUDGET_TERMS;
	x.result = AML_OK;

	for (i = 0; i < argc; i++) {
		struct aml_value *v = new_value(&x);

		if (!v)
			break;
		*v = args[i];
		x.arg[i] = v;
	}

	if (x.result == AML_OK) {
		body.p = m->body;
		body.end = m->body + m->body_len;

		(void)eval_list(&x, &body, m->device[0] ? m->device : device);
	}

	busy = false;

	if (x.result != AML_OK) {
		count_refusal(x.result);
		return x.result;
	}

	if (out) {
		if (x.returned)
			*out = *x.returned;
		else
			kmemset(out, 0, sizeof(*out));
	}

	counters.returned++;
	return AML_OK;
}

/* Run a body directly, for the self-test.
 *
 * The public way in takes a method name and looks it up, which is right for a
 * caller and useless for a test: the whole point is to run bytecode written
 * here rather than whatever the machine shipped. Same evaluator, same bounds,
 * same arena -- only the lookup is skipped.
 */
enum aml_result aml_eval_body(const u8 *body, u32 len, struct aml_value *out)
{
	static struct ctx x;
	struct span s;

	kmemset(&x, 0, sizeof(x));
	x.budget = BUDGET_TERMS;
	x.result = AML_OK;

	s.p = body;
	s.end = body + len;

	(void)eval_list(&x, &s, NULL);

	if (x.result != AML_OK)
		return x.result;

	if (out) {
		if (x.returned)
			*out = *x.returned;
		else
			kmemset(out, 0, sizeof(*out));
	}

	return AML_OK;
}

enum aml_result aml_eval_integer(const char *device, const char *method,
				 u64 *out)
{
	struct aml_value v;
	enum aml_result r = aml_eval(device, method, NULL, 0, &v);

	if (r != AML_OK)
		return r;

	if (v.type != AML_TYPE_INT)
		return AML_TYPE;

	if (out)
		*out = v.integer;
	return AML_OK;
}

/* Run every zero-argument method, and count what each did.
 *
 * The self-test above proves this evaluator does what bytecode says. It says
 * nothing about whether *this machine's* bytecode is within reach, which is the
 * question worth asking: the trackpad on row 3.3 is behind a `_DSM` a vendor
 * wrote, and the only way to know how close that is is to try the ones that are
 * here.
 *
 * **Safe by construction rather than by care.** An operation region is refused
 * before it is read, a store to a named object is refused, and there is nothing
 * else in this evaluator that can reach the machine. A method that would *do*
 * something gets refused rather than run, so a survey that tries all of them
 * cannot change anything.
 *
 * Methods taking arguments are left alone. Making up arguments to a method
 * somebody else wrote is the one thing here that would deserve the word
 * reckless.
 */
void aml_eval_survey(void)
{
	const struct aml_state *st = aml();
	unsigned i;

	if (!st->parsed)
		return;

	for (i = 0; i < st->methods_kept; i++) {
		const struct aml_method *m = &st->method[i];

		if (m->args != 0)
			continue;

		(void)aml_eval(m->device[0] ? m->device : NULL, m->name,
			       NULL, 0, NULL);
	}
}

void aml_eval_print_summary(void)
{
	if (!counters.asked)
		return;

	kprintf("  aml eval     : %u asked, %u returned\n",
		counters.asked, counters.returned);

	/* Each refusal separately, and only when it happened. They mean
	 * different things: a machine that declares no such method is a fact
	 * about the machine, and an opcode this cannot run is work. */
	if (counters.refused_missing)
		kprintf("  aml eval     : %u not declared, or asked with the "
			"wrong number of arguments\n", counters.refused_missing);
	if (counters.refused_hardware)
		kprintf("  aml eval     : %u needed hardware or a mutex, which "
			"this evaluator does not touch (last: %s%02x)\n",
			counters.refused_hardware,
			counters.last_hw_ext ? "5b " : "", counters.last_hw);
	if (counters.refused_op) {
		if (counters.last_unknown_was_store)
			kprintf("  aml eval     : %u stored somewhere this "
				"refuses to write -- a named object, not a "
				"local\n", counters.refused_op);
		else if (counters.last_unknown_was_name)
			kprintf("  aml eval     : %u met something this does "
				"not run (last: a name that is declared "
				"nowhere this parser reached)\n",
				counters.refused_op);
		else
			kprintf("  aml eval     : %u met an opcode this does "
				"not run (last: %s%02x)\n", counters.refused_op,
				counters.last_unknown_ext ? "5b " : "",
				counters.last_unknown);
	}
	if (counters.refused_bound)
		kprintf("  aml eval     : %u ran past a bound (last: %s)\n",
			counters.refused_bound, aml_why(counters.last_bound));
}

/* --- does the evaluator do what the bytecode says? -------------------------
 *
 * Against AML written here rather than against the machine's own table. The
 * DSDT is whatever the vendor shipped and on QEMU it reaches almost none of
 * this, so a green run against it would mean "none of this was exercised" and
 * would be indistinguishable from "all of it works".
 *
 * Each case is a few bytes with its source in the comment beside it. The
 * expected answer is one a person can work out by reading, which is the point:
 * a test whose expectation is computed the same way as the code cannot catch
 * the code being wrong.
 */

struct eval_case {
	const char *what;
	const u8 *body;
	u32 len;
	enum aml_result want;
	u64 value;			/* when want is AML_OK */
};

/* Store(Add(2, 3), Local0); Return(Local0)  ->  5 */
static const u8 t_add[] = {
	0x70, 0x72, 0x0A, 0x02, 0x0A, 0x03, 0x00, 0x60,
	0xA4, 0x60,
};

/* Return(LEqual(7, 7))  ->  Ones, which is ~0 */
static const u8 t_equal[] = {
	0xA4, 0x93, 0x0A, 0x07, 0x0A, 0x07,
};

/* Return(LNotEqual(7, 8))  ->  Ones.
 *
 * The three comparisons AML has no opcode for: LNot followed by the opposite
 * test, which an evaluator reading LNot as a unary operator gets wrong by
 * consuming only the first operand. */
static const u8 t_notequal[] = {
	0xA4, 0x92, 0x93, 0x0A, 0x07, 0x0A, 0x08,
};

/* If (1) { Return (0x11) } Else { Return (0x22) }  ->  0x11 */
static const u8 t_if_taken[] = {
	/* If: one byte of predicate and three of Return is four, plus the
	 * length byte itself is five. */
	0xA0, 0x05, 0x01, 0xA4, 0x0A, 0x11,
	0xA1, 0x04, 0xA4, 0x0A, 0x22,
};

/* If (0) { Return (0x11) } Else { Return (0x22) }  ->  0x22 */
static const u8 t_if_else[] = {
	0xA0, 0x05, 0x00, 0xA4, 0x0A, 0x11,
	0xA1, 0x04, 0xA4, 0x0A, 0x22,
};

/* Store(0, Local0); Store(0, Local1);
 * While (LLess(Local0, 4)) { Increment(Local0); Add(Local1, Local0, Local1) }
 * Return (Local1)                                  ->  1+2+3+4 = 10
 *
 * The loop that proves the condition is re-evaluated each round rather than
 * once: a While that read its predicate a single time would return 1. */
static const u8 t_while[] = {
	0x70, 0x00, 0x60,
	0x70, 0x00, 0x61,
	/* 4 + 2 + 4 of body, and one for the length byte, is eleven. */
	0xA2, 0x0B,
		0x95, 0x60, 0x0A, 0x04,
		0x75, 0x60,
		0x72, 0x61, 0x60, 0x61,
	0xA4, 0x61,
};

/* Return (SizeOf (Buffer (5) { 1, 2, 3 }))  ->  5, not 3.
 *
 * A buffer longer than its byte list: the tail is zeroes and the size is what
 * was declared. An evaluator that returned the byte count would say 3. */
static const u8 t_buffer[] = {
	/* Two bytes of size term and three of byte list is five, plus the
	 * length byte is six. This said five, so the evaluator saw
	 * `Buffer (5) { 1, 2 }` -- **and SizeOf returned 5 anyway**, because
	 * the declared size is what SizeOf reports. A wrong buffer, the right
	 * answer, and a green test. See t_buffer_byte, which is why it is no
	 * longer green. */
	0xA4, 0x87, 0x11, 0x06, 0x0A, 0x05, 0x01, 0x02, 0x03,
};

/* Return (Index (Buffer (5) { 1, 2, 3 }, 2))  ->  3.
 *
 * The case SizeOf could not fail. It reads the third byte back out, which is 3
 * only if all three arrived and 0 if the byte list was cut short. */
static const u8 t_buffer_byte[] = {
	0xA4, 0x88, 0x11, 0x06, 0x0A, 0x05, 0x01, 0x02, 0x03,
		0x0A, 0x02,
		0x00,
};

/* Return (Index (Package (3) { 0x0A, 0x0B, 0x0C }, 1))  ->  0x0B */
static const u8 t_package[] = {
	/* One byte of element count and six of elements is seven, plus the
	 * length byte is eight. */
	0xA4, 0x88,
		0x12, 0x08, 0x03, 0x0A, 0x0A, 0x0A, 0x0B, 0x0A, 0x0C,
		0x01,
		0x00,
};

/* Return (Divide (17, 5))  ->  3, and the remainder stored nowhere.
 *
 * Divide takes two targets, remainder first. An evaluator that had them the
 * other way round returns 2. */
static const u8 t_divide[] = {
	0xA4, 0x78, 0x0A, 0x11, 0x0A, 0x05, 0x00, 0x00,
};

/* Return (ShiftLeft (1, 64))  ->  0.
 *
 * Undefined in C and zero in AML, so it is written rather than left to
 * whatever the compiler emits for a shift the width of the type. */
static const u8 t_shift[] = {
	/* ShiftLeft takes a target like every other arithmetic opcode, and
	 * the null name is how AML says to discard it. Leaving it off made
	 * the evaluator read past the end looking for one. */
	0xA4, 0x79, 0x01, 0x0A, 0x40, 0x00,
};

/* --- and the two that must be refused ------------------------------------- */

/* Return (\_SB.PCI0.OPRG)  -- an extended opcode: an operation region.
 *
 * This is the case the whole design turns on. An evaluator that returned zero
 * here would pass every case above it and hand a driver an interrupt number
 * that came from nowhere. */
static const u8 t_region[] = {
	0xA4, 0x5B, 0x80,
};

/* Store (1, BUF0)  -- a store to a named object.
 *
 * Refused, and the refusal is load-bearing: aml.c records what each name was
 * *declared* as, and reading one is correct only because nothing can write it.
 */
static const u8 t_store_name[] = {
	0x70, 0x01, 'B', 'U', 'F', '0',
};

bool aml_eval_self_test(void)
{
	static const struct eval_case cases[] = {
		{ "add",        t_add,        sizeof(t_add),        AML_OK, 5 },
		{ "equal",      t_equal,      sizeof(t_equal),      AML_OK, ~0ull },
		{ "not equal",  t_notequal,   sizeof(t_notequal),   AML_OK, ~0ull },
		{ "if taken",   t_if_taken,   sizeof(t_if_taken),   AML_OK, 0x11 },
		{ "else",       t_if_else,    sizeof(t_if_else),    AML_OK, 0x22 },
		{ "while",      t_while,      sizeof(t_while),      AML_OK, 10 },
		{ "buffer",     t_buffer,     sizeof(t_buffer),     AML_OK, 5 },
		{ "a byte of it", t_buffer_byte, sizeof(t_buffer_byte),
		  AML_OK, 3 },
		{ "package",    t_package,    sizeof(t_package),    AML_OK, 0x0B },
		{ "divide",     t_divide,     sizeof(t_divide),     AML_OK, 3 },
		{ "shift",      t_shift,      sizeof(t_shift),      AML_OK, 0 },
		{ "an operation region", t_region, sizeof(t_region),
		  AML_NEEDS_HARDWARE, 0 },
		{ "a store to a name",   t_store_name, sizeof(t_store_name),
		  AML_UNSUPPORTED_OP, 0 },
	};

	unsigned i;
	bool ok = true;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const struct eval_case *k = &cases[i];
		struct aml_value out;
		enum aml_result r = aml_eval_body(k->body, k->len, &out);

		if (r != k->want) {
			kprintf("  aml eval: %s -- %s, wanted %s\n",
				k->what, aml_why(r), aml_why(k->want));
			ok = false;
			continue;
		}

		if (k->want != AML_OK)
			continue;

		if (out.type != AML_TYPE_INT) {
			kprintf("  aml eval: %s returned something that is not "
				"an integer\n", k->what);
			ok = false;
		} else if (out.integer != k->value) {
			kprintf("  aml eval: %s returned %lu, wanted %lu\n",
				k->what, (unsigned long)out.integer,
				(unsigned long)k->value);
			ok = false;
		}
	}

	return ok;
}
