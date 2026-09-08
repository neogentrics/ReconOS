/* Randomness the kernel owns.
 *
 * Asked for by the desktop, in docs/KERNEL-WANTS.md, and the sentence that made
 * it worth building is this one: *"A machine generating its own long-lived
 * private key on first boot is exactly the situation where a weak entropy
 * source produces keys that are quietly guessable, and ReconOS has no way to
 * know how good the one underneath it is."*
 *
 * Both halves of that matter. A source, and **a way to ask how good it is** --
 * because the failure mode here is not a crash or a wrong answer. It is a key
 * that looks exactly like a good key, is accepted by everything, and can be
 * guessed by somebody who knows how it was made. Nothing downstream can detect
 * it. The only defence is at the point of generation, which is here.
 *
 * --- The rule this follows ---
 *
 * **It refuses rather than guessing.** random_bytes() returns false until the
 * pool has been seeded with enough entropy to be worth anything, and there is
 * no flag to override that -- which is this project's standing rule about
 * safety checks, and it has never mattered more than it does here. A generator
 * that returns low-quality bytes rather than an error is a generator whose
 * caller cannot tell, and every caller will assume it worked.
 *
 * The alternative that real systems have shipped is to block until seeded, and
 * that is worse in a kernel with no way to wait: it turns a weak key into a
 * machine that does not boot, which is a failure people work around by
 * disabling the check.
 */
#ifndef RECON_KERNEL_RANDOM_H
#define RECON_KERNEL_RANDOM_H

#include <recon/kernel/types.h>

/* How much entropy the pool is believed to hold, in bits, capped at the pool's
 * width. An *estimate*, and deliberately a conservative one -- see random.c for
 * what each source is credited and why each figure is lower than the source's
 * own claim about itself. */
unsigned random_entropy_bits(void);

/* Whether random_bytes will answer at all. */
bool random_ready(void);

/* Fills `buf`. Returns false, having written nothing, if the pool has not been
 * seeded -- and a caller that ignores that has generated a key from an
 * uninitialised buffer, so the return value is not advisory. */
bool random_bytes(void *buf, size_t len);

/* Adds entropy from wherever the caller got it. `bits` is what the caller
 * believes it is worth, and is clamped: nothing can credit more than the data
 * it supplied could possibly contain. Callers that do not know say zero, which
 * still mixes the data in -- mixing is free and cannot make the pool worse. */
void random_add(const void *data, size_t len, unsigned bits);

/* Gathers what this machine can offer and seeds the pool. Called once during
 * boot, after the clock exists, because one of the sources is timing. */
void random_init(void);

void random_print_summary(void);
bool random_self_test(void);

/* --- what the architecture provides --------------------------------------
 *
 * A hardware generator, if this processor has one. Returns false when there is
 * none, and *also* when there is one and it declined to answer -- which is a
 * real case rather than a theoretical one: x86's RDRAND reports failure in the
 * carry flag under load, and a driver that ignores it returns whatever was in
 * the register, forever, without ever failing. */
bool arch_random_hw(u64 *out);

/* Whether the processor claims to have one at all, asked separately so that
 * "there is no hardware source" and "there is one and it is not answering" can
 * be told apart in the summary. They call for different responses. */
bool arch_random_hw_present(void);

#endif /* RECON_KERNEL_RANDOM_H */
