/* Running a method, which is the other half of reading a machine's description.
 *
 * `aml.c` builds the namespace: the tree of scopes, devices and named values
 * the bytecode *declares*. Its header states the line it does not cross --
 * *what the machine is, which is declared, against what the machine does, which
 * is executed* -- and that line held for everything this kernel needed until
 * it needed a trackpad.
 *
 * An I2C HID device says where its descriptor lives in `_DSM`, and states its
 * address and interrupt in `_CRS`. On most firmware both are Methods. There is
 * no reading them without running them.
 *
 * --- what it will refuse, which is most of AML -----------------------------
 *
 * This evaluates **pure computation**: constants, locals and arguments,
 * arithmetic, logic, comparisons, buffers, packages, control flow, and calls to
 * other methods. That is enough for a `_CRS` that builds a resource template
 * and for a `_DSM` that compares its UUID argument and returns a package.
 *
 * It refuses, by name and with the offset:
 *
 *   - **any read or write of an operation region.** Those are memory, I/O
 *     ports, PCI configuration space and embedded-controller traffic, and
 *     touching them is a different kind of act from computing a value. A method
 *     that needs one gets a refusal naming the region rather than a zero.
 *   - mutexes, events, notifications, and anything else that makes the machine
 *     do something rather than say something.
 *   - every opcode it does not know.
 *
 * **A refusal is the whole design.** The parser's rule -- *stop rather than
 * guess, because a wrong answer is worse than an absent one* -- costs more
 * here, not less: a `_CRS` evaluated to a plausible wrong buffer is an
 * interrupt number a driver will program into a controller.
 *
 * --- and it is bounded in four directions ----------------------------------
 *
 * AML is a real language and this runs bytecode a vendor wrote. Every one of
 * these is a way a method can fail to return, and each is bounded separately
 * because they fail differently: instructions executed, call depth, loop
 * iterations, and objects allocated. Reaching any bound is a refusal with that
 * bound named.
 */
#ifndef RECON_KERNEL_AML_EVAL_H
#define RECON_KERNEL_AML_EVAL_H

#include <recon/kernel/types.h>

/* How an evaluation ended.
 *
 * Every failure is its own value rather than one "did not work", for the reason
 * this project keeps rediscovering: a caller that cannot tell *this machine
 * does not declare it* from *this kernel cannot run it* has to treat both the
 * same, and they want different responses. The first is a fact about the
 * machine; the second is work for whoever reads it. */
enum aml_result {
	AML_OK = 0,
	AML_NO_METHOD,		/* nothing of that name is declared */
	AML_BAD_ARGS,		/* the method takes a different number */
	AML_UNSUPPORTED_OP,	/* an opcode this evaluator does not implement */
	AML_NEEDS_HARDWARE,	/* it reads or writes an operation region */
	AML_OUT_OF_OBJECTS,	/* the arena filled */
	AML_TOO_LONG,		/* the instruction budget ran out */
	AML_TOO_DEEP,		/* calls nested past the limit */
	AML_TOO_MANY_LOOPS,	/* a While ran past its iteration limit */
	AML_TRUNCATED,		/* the body ended in the middle of something */
	AML_TYPE,		/* an operand was the wrong kind of thing */
};

const char *aml_why(enum aml_result r);

/* What a method can hand back.
 *
 * Buffers and packages point into the evaluation's own arena, so **everything
 * here stops being valid when the call that produced it returns**. A caller
 * that wants to keep a buffer copies it. That is stated rather than managed:
 * reference counting an object graph is the piece of ACPI that is hardest to
 * get right, and nothing this kernel does yet needs an object to outlive the
 * call.
 */
enum aml_type {
	AML_TYPE_NONE = 0,
	AML_TYPE_INT,
	AML_TYPE_BUFFER,
	AML_TYPE_STRING,
	AML_TYPE_PACKAGE,
};

struct aml_value;

struct aml_value {
	enum aml_type type;
	u64 integer;
	const u8 *bytes;		/* buffer or string */
	u32 length;
	struct aml_value **items;	/* package */
	u32 count;
};

/* Run a method and hand back what it returned.
 *
 * `device` may be null to match a method at the top level or in any scope; when
 * given, it names the enclosing device, because a machine has many methods
 * called `_CRS` and what they are inside is the only thing that tells them
 * apart.
 *
 * `args` are the method's arguments, `argc` how many -- and a count that does
 * not match what the method declares is AML_BAD_ARGS rather than a run with
 * whatever happened to be in the unset ones.
 *
 * `out` is filled only on AML_OK, and points into memory that is gone when this
 * returns. See above.
 */
enum aml_result aml_eval(const char *device, const char *method,
			 const struct aml_value *args, unsigned argc,
			 struct aml_value *out);

/* The same, for a method taking no arguments and returning an integer -- which
 * is most of the ones worth asking about, and saves every caller the same six
 * lines of unwrapping. */
enum aml_result aml_eval_integer(const char *device, const char *method,
				 u64 *out);

/* Counts, printed by the summary: how many evaluations were asked for, how many
 * returned, and how many were refused for each of the reasons that mean work
 * rather than a fact about the machine. */
void aml_eval_print_summary(void);

/* Run a body of AML directly, without looking a name up. For the self-test,
 * which runs bytecode written by hand rather than whatever the machine shipped
 * -- the DSDT reaches almost none of this evaluator, so a green run against it
 * would say nothing. */
enum aml_result aml_eval_body(const u8 *body, u32 len, struct aml_value *out);

/* Evaluate every zero-argument method this machine declares and count the
 * outcomes, so the summary can say how much of *this* table is within reach.
 *
 * Safe to call on any machine: everything that could affect it is refused
 * before it happens. Methods taking arguments are skipped, because inventing
 * arguments to somebody else's method is a different kind of act. */
void aml_eval_survey(void);

bool aml_eval_self_test(void);

#endif /* RECON_KERNEL_AML_EVAL_H */
