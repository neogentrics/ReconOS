/* The entropy pool, and the generator that runs off it.
 *
 * One primitive does both jobs. ChaCha20 is a stream cipher; a stream cipher is
 * a function from a key to an indistinguishable-from-random byte stream, which
 * is exactly what a generator is. So the pool *is* a 32-byte key, entropy is
 * added by mixing it into that key, and output is taken by running the cipher.
 *
 * Using one primitive for both is not economy. A pool built on one construction
 * and a generator on another gives two things to get right and a seam between
 * them, and the seam is where the interesting failures live -- a mixing step
 * that loses entropy is invisible from either side.
 *
 * --- Forward secrecy, which is why the key is replaced rather than used ---
 *
 * Every extraction re-keys: the first 32 bytes of the cipher's output become
 * the new key and are never returned to anybody. So an attacker who reads the
 * pool's memory at some instant learns the *future* stream and not one byte of
 * what was generated before. Without that, compromising the pool once
 * retroactively compromises every key the machine has ever made.
 *
 * --- What is deliberately not here ---
 *
 * A blocking read. There is no way to wait in this kernel yet, and blocking
 * would turn "the key would be weak" into "the machine does not boot", which is
 * a failure people fix by disabling the check. It refuses instead.
 */
#include <recon/kernel/random.h>

#include <recon/kernel/arch.h>
#include <recon/kernel/console.h>
#include <recon/kernel/cpu.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/time.h>

/* --- ChaCha20 -------------------------------------------------------------
 *
 * Twenty rounds of add-rotate-xor over sixteen 32-bit words. Written out rather
 * than borrowed because it is forty lines and because the one thing that must
 * be right -- the rotation amounts and the diagonal pattern -- is easier to
 * check here than in a dependency.
 */

#define ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

#define QUARTERROUND(a, b, c, d)		\
	do {					\
		a += b; d ^= a; d = ROTL32(d, 16); \
		c += d; b ^= c; b = ROTL32(b, 12); \
		a += b; d ^= a; d = ROTL32(d, 8);  \
		c += d; b ^= c; b = ROTL32(b, 7);  \
	} while (0)

/* "expand 32-byte k", as four little-endian words. A constant of the
 * construction, not a choice. */
static const u32 chacha_sigma[4] = {
	0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u
};

static void chacha20_block(const u8 key[32], u64 counter, const u8 nonce[8],
			   u8 out[64])
{
	u32 state[16], working[16];
	unsigned i;

	for (i = 0; i < 4; i++)
		state[i] = chacha_sigma[i];

	for (i = 0; i < 8; i++)
		state[4 + i] = (u32)key[i * 4] |
			       ((u32)key[i * 4 + 1] << 8) |
			       ((u32)key[i * 4 + 2] << 16) |
			       ((u32)key[i * 4 + 3] << 24);

	state[12] = (u32)counter;
	state[13] = (u32)(counter >> 32);
	state[14] = (u32)nonce[0] | ((u32)nonce[1] << 8) |
		    ((u32)nonce[2] << 16) | ((u32)nonce[3] << 24);
	state[15] = (u32)nonce[4] | ((u32)nonce[5] << 8) |
		    ((u32)nonce[6] << 16) | ((u32)nonce[7] << 24);

	for (i = 0; i < 16; i++)
		working[i] = state[i];

	for (i = 0; i < 10; i++) {	/* ten double rounds = twenty rounds */
		QUARTERROUND(working[0], working[4], working[8],  working[12]);
		QUARTERROUND(working[1], working[5], working[9],  working[13]);
		QUARTERROUND(working[2], working[6], working[10], working[14]);
		QUARTERROUND(working[3], working[7], working[11], working[15]);

		QUARTERROUND(working[0], working[5], working[10], working[15]);
		QUARTERROUND(working[1], working[6], working[11], working[12]);
		QUARTERROUND(working[2], working[7], working[8],  working[13]);
		QUARTERROUND(working[3], working[4], working[9],  working[14]);
	}

	/* The feed-forward add. Without it the permutation is invertible and the
	 * key can be recovered from one block of output -- which is the whole
	 * difference between a cipher and a reversible shuffle. */
	for (i = 0; i < 16; i++) {
		u32 v = working[i] + state[i];

		out[i * 4]     = (u8)v;
		out[i * 4 + 1] = (u8)(v >> 8);
		out[i * 4 + 2] = (u8)(v >> 16);
		out[i * 4 + 3] = (u8)(v >> 24);
	}
}

/* --- the pool ------------------------------------------------------------ */

static u8 pool_key[32];
static u8 pool_nonce[8];
static u64 pool_counter;
static unsigned entropy_bits;
static bool seeded;

/* How the pool came to be seeded, for the summary. Two machines can both say
 * "ready" and mean very different things. */
static bool used_hardware;
static bool used_jitter;
static unsigned hw_words;
static u64 jitter_samples;

static struct spinlock pool_lock = SPINLOCK_INIT("random");

/* Enough that guessing the pool is not the cheapest attack on anything built
 * from it. Below this the answer is a refusal rather than a weaker byte. */
#define ENTROPY_THRESHOLD 256

void random_add(const void *data, size_t len, unsigned bits)
{
	const u8 *p = data;
	u8 block[64];
	size_t i;
	u64 flags;

	if (!len)
		return;

	/* Nothing may be credited more than the data could hold. A caller that
	 * believes its eight bytes carry a hundred bits of entropy is wrong,
	 * and the pool must not take its word for it -- an over-credited pool
	 * reports ready while holding nothing, which is the one failure this
	 * whole file exists to prevent. */
	if (bits > len * 8)
		bits = (unsigned)(len * 8);

	flags = spin_lock_irq(&pool_lock);

	/* Mixed in by XOR, then stirred by re-keying. XOR alone would let a
	 * caller who controls the data cancel what is already there; the stir
	 * is what makes the contribution one-way. */
	for (i = 0; i < len; i++)
		pool_key[i % sizeof(pool_key)] ^= p[i];

	chacha20_block(pool_key, ++pool_counter, pool_nonce, block);
	kmemcpy(pool_key, block, sizeof(pool_key));
	kmemset(block, 0, sizeof(block));

	entropy_bits += bits;
	if (entropy_bits > sizeof(pool_key) * 8)
		entropy_bits = sizeof(pool_key) * 8;

	if (entropy_bits >= ENTROPY_THRESHOLD)
		seeded = true;

	spin_unlock_irq(&pool_lock, flags);
}

unsigned random_entropy_bits(void)
{
	return entropy_bits;
}

bool random_ready(void)
{
	return seeded;
}

bool random_bytes(void *buf, size_t len)
{
	u8 *out = buf;
	u8 block[64];
	u64 flags;

	if (!seeded)
		return false;

	flags = spin_lock_irq(&pool_lock);

	while (len) {
		size_t take;

		chacha20_block(pool_key, ++pool_counter, pool_nonce, block);

		/* The first half of every block replaces the key and is never
		 * given out. That is what makes reading the pool's memory now
		 * tell an attacker nothing about what it produced before. */
		kmemcpy(pool_key, block, sizeof(pool_key));

		take = len < 32 ? len : 32;
		kmemcpy(out, block + 32, take);

		out += take;
		len -= take;
	}

	kmemset(block, 0, sizeof(block));
	spin_unlock_irq(&pool_lock, flags);

	return true;
}

/* --- gathering ------------------------------------------------------------
 *
 * Two sources, and the second exists because the first is not always there.
 */

/* The processor's own generator, if it has one and if it answers.
 *
 * Credited at **half** its width. Not because the hardware is suspected of
 * being bad, but because nothing here can check it: it is a sealed box whose
 * output is by construction indistinguishable from random whether it is working
 * or has failed closed. Halving is the standard hedge, and the reason to write
 * it down is that a full credit would let one instruction that returns a
 * constant declare the pool ready on its own. */
static void gather_hardware(void)
{
	unsigned got = 0;
	u64 previous = 0;
	unsigned i;

	if (!arch_random_hw_present())
		return;

	for (i = 0; i < 32 && got < 8; i++) {
		u64 value;

		if (!arch_random_hw(&value))
			continue;

		/* A generator returning the same value twice running is not
		 * generating. This is the cheap check that catches the one
		 * failure mode a sealed box has: RDRAND under load reports its
		 * refusal in a flag, and a driver that ignores the flag reads
		 * back whatever was in the register -- the same number, for
		 * ever, looking perfectly random the first time. */
		if (got && value == previous)
			continue;

		random_add(&value, sizeof(value), sizeof(value) * 8 / 2);
		previous = value;
		got++;
	}

	if (got) {
		used_hardware = true;
		hw_words = got;
	}
}

/* Timing jitter: what a machine with no hardware generator has.
 *
 * The counter is read either side of a small piece of work, and the *variation*
 * between successive measurements is the entropy -- not the measurement itself,
 * which is largely predictable. Two things make it real on any machine: the
 * counter runs at a different rate from the instruction pipeline, and caches,
 * branch predictors and interrupts perturb the work by amounts nothing can
 * predict from outside.
 *
 * It is credited at **one bit per sample**, which is far below what the low
 * bits of the differences actually carry, and deliberately so. This is the
 * source that cannot be checked from inside, on a machine that by definition
 * has nothing better to check it against.
 */
static void gather_jitter(void)
{
	u64 deltas[16];
	u64 last = time_monotonic_ns();
	unsigned i, credited = 0;

	for (i = 0; i < 16; i++) {
		volatile unsigned spin = 0;
		u64 now;
		unsigned j;

		/* Work whose duration the compiler cannot fold away and the
		 * processor cannot schedule identically twice. */
		for (j = 0; j < 64 + (i * 7); j++)
			spin += j;

		now = time_monotonic_ns();
		deltas[i] = now - last;
		last = now;
	}

	/* Only samples that differ from the one before are credited. A clock
	 * too coarse to see this work -- which is a real machine, not a
	 * hypothetical one -- produces a run of identical deltas, and crediting
	 * those would be crediting the constant zero. */
	for (i = 1; i < 16; i++)
		if (deltas[i] != deltas[i - 1])
			credited++;

	random_add(deltas, sizeof(deltas), credited);

	if (credited) {
		used_jitter = true;
		jitter_samples += credited;
	}
}

void random_init(void)
{
	struct cpu_caps caps;

	/* Something that differs between machines and between boots, mixed in
	 * for free. Credited nothing: an attacker can guess the time and the
	 * machine's own facts, so this is stirring rather than entropy, and
	 * calling it entropy is exactly the over-credit this file refuses. */
	{
		u64 facts[3];

		facts[0] = time_wall_ns();
		facts[1] = time_monotonic_ns();
		facts[2] = (u64)(uintptr_t)&caps;	/* where this stack is */

		random_add(facts, sizeof(facts), 0);
	}

	gather_hardware();

	/* Always, not only when there is no hardware generator. Two sources
	 * that fail independently are worth more than the better one alone, and
	 * mixing costs nothing. */
	gather_jitter();

	/* Still short? Keep sampling timing rather than lowering the bar. A
	 * bounded number of rounds, because a machine whose clock cannot see
	 * this work will never get there and hanging would be the wrong answer
	 * -- it reports not-ready instead, and callers refuse. */
	{
		unsigned round;

		for (round = 0; round < 64 && !seeded; round++)
			gather_jitter();
	}
}

void random_print_summary(void)
{
	kprintf("\nRandomness\n");

	kprintf("  state        : %s, %u bits\n",
		seeded ? "ready" : "NOT SEEDED -- keys will be refused",
		entropy_bits);

	/* Both facts, because they are different situations. A machine with no
	 * hardware generator is ordinary; a machine that has one and got
	 * nothing out of it is a machine with a fault, and one line that said
	 * only "no hardware" would report them identically. */
	if (used_hardware)
		kprintf("  hardware     : %u words accepted\n", hw_words);
	else if (arch_random_hw_present())
		kprintf("  hardware     : PRESENT AND SILENT -- it was asked "
			"and did not answer\n");
	else
		kprintf("  hardware     : none on this processor\n");

	kprintf("  timing       : %llu samples credited\n",
		(unsigned long long)jitter_samples);
}

/* --- the self-test --------------------------------------------------------
 *
 * What can and cannot be checked from in here is worth being exact about.
 *
 * **Cannot:** whether the output is cryptographically strong. No test in a
 * kernel can distinguish ChaCha20 from a true source, which is the point of
 * ChaCha20 -- and a test that appeared to would be testing itself.
 *
 * **Can:** every gross failure, and gross failure is what actually happens.
 * A generator wired to a stuck source, a mixing step that discards its input, a
 * re-key that does not re-key, an off-by-one that returns the same block twice.
 * Each of those produces output that fails immediately and obviously, and each
 * has shipped in real systems.
 */
bool random_self_test(void)
{
	u8 a[64], b[64];
	unsigned i, ones = 0;
	bool ok = true;

	/* The refusal, first, and on a machine where the pool *is* seeded that
	 * cannot be provoked -- so what is checked is the agreement between the
	 * two functions rather than the refusal itself. A random_ready() that
	 * says no while random_bytes() answers is the dangerous direction. */
	if (!random_ready()) {
		u8 scratch[8];

		kmemset(scratch, 0xA5, sizeof(scratch));

		if (random_bytes(scratch, sizeof(scratch))) {
			kputs("  random: it answered while reporting not "
			      "seeded\n");
			return false;
		}

		for (i = 0; i < sizeof(scratch); i++)
			if (scratch[i] != 0xA5) {
				kputs("  random: it refused and wrote to the "
				      "buffer anyway\n");
				return false;
			}

		/* Not a failure of this test: a machine with no usable entropy
		 * source is a machine this kernel should still boot on, and it
		 * says so rather than pretending. */
		kputs("  random: no entropy on this machine, so refusing is "
		      "the whole test\n");
		return true;
	}

	if (!random_bytes(a, sizeof(a)) || !random_bytes(b, sizeof(b))) {
		kputs("  random: it reported ready and then refused\n");
		return false;
	}

	/* Twice in a row must differ. This is the one that catches a generator
	 * that does not advance -- the most common real failure, and one that
	 * every statistical test would pass, because a constant block can be
	 * perfectly balanced. */
	if (kmemcmp(a, b, sizeof(a)) == 0) {
		kputs("  random: two successive reads were identical\n");
		ok = false;
	}

	/* All zeroes or all ones: the shape of a source that is not connected.
	 * Checked explicitly rather than left to the balance test below, which
	 * an all-zero buffer fails in a way that reads as a statistical
	 * complaint rather than as a wire that is not attached. */
	{
		bool all_same = true;

		for (i = 1; i < sizeof(a); i++)
			if (a[i] != a[0])
				all_same = false;

		if (all_same) {
			kprintf("  random: every byte of the output was %u\n",
				a[0]);
			ok = false;
		}
	}

	/* A balance check, and its bound is deliberately loose.
	 *
	 * 512 bits should hold about 256 ones. A tight bound here would fail
	 * occasionally on correct output, and a self-test that fails once a
	 * month for no reason is a self-test people learn to ignore -- which
	 * costs more than this check is worth. The bound below would be passed
	 * by output that is far from uniform, and that is the right trade:
	 * this is here to catch a wire that is not attached, not to grade
	 * a cipher. */
	for (i = 0; i < sizeof(a); i++) {
		u8 v = a[i];

		while (v) {
			ones += v & 1;
			v >>= 1;
		}
	}

	if (ones < sizeof(a) * 8 / 4 || ones > sizeof(a) * 8 * 3 / 4) {
		kprintf("  random: %u ones in %u bits, which is not plausible "
			"output\n", ones, (unsigned)sizeof(a) * 8);
		ok = false;
	}

	return ok;
}
