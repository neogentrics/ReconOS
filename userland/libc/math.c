/*
 * The twenty floating-point functions the desktop needs, and the line between
 * the ones that can be exactly right and the ones that cannot.
 *
 * --- Who actually wants these, which is not who a grep said ---
 *
 * A call-site grep reported this whole area as four calls. `nm` on the object
 * files says twenty functions across seven files, and the difference is not an
 * accident of counting -- it is two entire mechanisms a grep cannot see:
 *
 *   `src/recon_expr.c` holds them in a **table of function pointers**, so the
 *   calculator can look a name up: `{ "sqrt", sqrt }, { "cbrt", cbrt }`.
 *   Seventeen of the twenty are there, and not one appears as `sqrt(`.
 *
 *   `src/recon_ui.c` and `src/recon_ocr_match.c` reach seven of them through
 *   **`third_party/stb_truetype.h`**, which is the font rasteriser. Nothing in
 *   either file names a maths function at all.
 *
 * That second one is worth saying plainly: **ReconOS cannot draw a letter
 * without `sqrt`, `floor`, `ceil`, `pow`, `fmod`, `acos` and `cos`.** This is
 * not a calculator's luxury. It is between the system and its first glyph.
 *
 * --- Eight are exactly right, and twelve are not ---
 *
 * The split is not a matter of effort. Eight of these have an answer that is a
 * representable double and can be computed exactly: `fabs`, `floor`, `ceil`,
 * `round` and `ldexp` are bit manipulation, `fmod` is exact by definition,
 * `lrintf` is a rounding, and **`sqrt` is exact because IEEE 754 requires it
 * to be** -- every processor this will run on has an instruction that returns
 * the correctly rounded root, so asking for anything less would be slower and
 * worse. Those eight are held to **bit-for-bit equality** with the host's.
 *
 * The other twelve are transcendental: their true values are not
 * representable, so "correct" means "the nearest double", and computing that
 * for every input is a research problem. They are held to a **measured
 * bound in units in the last place**, and the suite prints the worst error it
 * found rather than only whether it stayed under the line -- a tolerance
 * nobody has looked inside is a tolerance that can quietly grow.
 *
 * This is the same rule `printf.c` and `strtod` already follow, for the same
 * reason, and it is the whole of what this file promises.
 */

#include "internal.h"

/*
 * The one place in this library that includes a public header besides
 * `time.c`, and for the same reason: one definition of the classifiers rather
 * than two kept identical by hand. `prefix.h` arrives by `-include` and has
 * already renamed everything it declares.
 */
#include "../include/math.h"

/* --- Looking at a double as bits --- */

/*
 * A union rather than a cast through a pointer.
 *
 * `*(unsigned long long *)&x` is a strict-aliasing violation, and the
 * compilers this is built with are entitled to assume it never happens -- at
 * -O2 they act on that assumption and the read comes back stale. A union is
 * the spelling the standard blesses and every compiler here implements.
 */
union bits {
	double value;
	unsigned long long word;
};

static unsigned long long to_bits(double x)
{
	union bits u;

	u.value = x;
	return u.word;
}

static double from_bits(unsigned long long word)
{
	union bits u;

	u.word = word;
	return u.value;
}

#define SIGN_BIT   0x8000000000000000ULL
#define EXP_MASK   0x7FF0000000000000ULL
#define FRAC_MASK  0x000FFFFFFFFFFFFFULL
#define EXP_SHIFT  52
#define EXP_BIAS   1023

static int exponent_of(double x)
{
	return (int)((to_bits(x) >> EXP_SHIFT) & 0x7FF);
}

/*
 * Built with the compiler's own constants rather than by overflowing an
 * expression, which a compiler is entitled to fold and warn about. These are
 * explicit builtins, so `-fno-builtin` does not take them away.
 */
static double infinity(void)
{
	return __builtin_inf();
}

static double not_a_number(void)
{
	return __builtin_nan("");
}

int recon_math_isnan(double x)
{
	/* x != x is true only for a NaN, and it is the definition rather than
	 * a trick -- a NaN compares unordered with everything, itself
	 * included. */
	return x != x;
}

int recon_math_isinf(double x)
{
	unsigned long long w = to_bits(x) & ~SIGN_BIT;

	return w == EXP_MASK;
}

int recon_math_isfinite(double x)
{
	return (to_bits(x) & EXP_MASK) != EXP_MASK;
}

int recon_math_signbit(double x)
{
	return (to_bits(x) & SIGN_BIT) != 0;
}

/* --- The eight that are exactly right --- */

double fabs(double x)
{
	return from_bits(to_bits(x) & ~SIGN_BIT);
}

double sqrt(double x)
{
	/*
	 * The hardware instruction, through the compiler's own builtin --
	 * `sqrtsd` on x86_64 and `fsqrt` on aarch64. IEEE 754 **requires**
	 * square root to be correctly rounded, so this is not an
	 * approximation with a good constant: it is the nearest double to the
	 * true root, on every machine, and it is what glibc does too.
	 *
	 * Writing a Newton iteration here would be slower and strictly worse.
	 * The only reason to do so would be a processor with no square root,
	 * and neither architecture ReconOS targets is one.
	 */
	return __builtin_sqrt(x);
}

/*
 * Scale by a power of two, which is exact until it runs out of range.
 *
 * Done in at most three steps rather than one so that a huge `n` cannot
 * overflow the exponent arithmetic itself: each step moves by at most 1023,
 * and the intermediate is a real double.
 */
static double scale_by_2(double x, int n)
{
	int e;

	if (x == 0.0 || !recon_math_isfinite(x)) {
		return x;
	}

	if (n > 1023) {
		x *= from_bits((unsigned long long)(1023 + EXP_BIAS)
			       << EXP_SHIFT);
		n -= 1023;
		if (n > 1023) {
			x *= from_bits((unsigned long long)(1023 + EXP_BIAS)
				       << EXP_SHIFT);
			n -= 1023;
			if (n > 1023) {
				n = 1023;
			}
		}
	} else if (n < -1022) {
		/*
		 * Downward in steps of 969 rather than 1022, and this is the
		 * part that is easy to get wrong: stepping by the full
		 * exponent range takes the intermediate into the subnormals,
		 * where it loses bits that the second step cannot give back.
		 * 969 keeps every intermediate normal.
		 */
		x *= from_bits((unsigned long long)(-969 + EXP_BIAS)
			       << EXP_SHIFT);
		n += 969;
		if (n < -1022) {
			x *= from_bits((unsigned long long)(-969 + EXP_BIAS)
				       << EXP_SHIFT);
			n += 969;
			if (n < -1022) {
				n = -1022;
			}
		}
	}

	e = n + EXP_BIAS;
	if (e < 1) {
		e = 1;
	}
	return x * from_bits((unsigned long long)e << EXP_SHIFT);
}

double ldexp(double x, int n)
{
	return scale_by_2(x, n);
}

/*
 * Toward negative infinity, by clearing the fractional bits.
 *
 * The arithmetic version -- `(double)(long long)x`, adjusted -- is wrong for
 * anything past what a `long long` holds and is undefined for a NaN. Working
 * on the bits has neither problem and no branch on magnitude.
 */
double floor(double x)
{
	int e = exponent_of(x) - EXP_BIAS;
	unsigned long long w = to_bits(x);

	if (e >= 52) {
		/* Already integral, or infinite, or a NaN. All three are
		 * returned unchanged, which is what the reference does. */
		return x;
	}
	if (e < 0) {
		/* |x| < 1: the answer is 0 or -1, and the sign of the zero
		 * follows x -- floor(-0.0) is -0.0, not 0.0, and a caller
		 * dividing by it can tell. */
		if (w & SIGN_BIT) {
			return (w & ~SIGN_BIT) == 0 ? x : -1.0;
		}
		return from_bits(w & SIGN_BIT);
	}

	{
		unsigned long long fraction = FRAC_MASK >> e;

		if ((w & fraction) == 0) {
			return x;	/* already integral */
		}
		w &= ~fraction;
		if (to_bits(x) & SIGN_BIT) {
			/* Negative and not integral: step one place away from
			 * zero, which is where the truncation landed short. */
			w += 0x0010000000000000ULL >> e;
		}
		return from_bits(w);
	}
}

double ceil(double x)
{
	/* Toward positive infinity, which is floor mirrored. Written this way
	 * rather than duplicated so the two cannot come apart, and the sign of
	 * a zero survives it: -0.0 negated is 0.0 and back again. */
	return -floor(-x);
}

/*
 * Halfway cases away from zero -- 0.5 to 1, -0.5 to -1.
 *
 * **Not** round-half-to-even, which is what `rint` does and what the hardware
 * does. C's `round` is the one that rounds a half away from zero, and the
 * difference shows on exactly the inputs a person tests with.
 */
double round(double x)
{
	int e = exponent_of(x) - EXP_BIAS;
	unsigned long long w = to_bits(x);
	unsigned long long sign = w & SIGN_BIT;

	if (e >= 52) {
		return x;
	}
	if (e < 0) {
		if (e == -1) {
			/* 0.5 <= |x| < 1 */
			return from_bits(sign | to_bits(1.0));
		}
		return from_bits(sign);		/* |x| < 0.5, signed zero */
	}

	{
		unsigned long long fraction = FRAC_MASK >> e;
		unsigned long long half = 0x0008000000000000ULL >> e;

		if ((w & fraction) == 0) {
			return x;
		}
		/* Add a half at the place being rounded to, then truncate.
		 * The carry does the rounding, including out of the mantissa
		 * into the exponent, which is why this needs no special case
		 * for 0.9999999999999999. */
		w += half;
		w &= ~fraction;
		return from_bits(w);
	}
}

/*
 * The remainder of x/y with the sign of x, computed exactly.
 *
 * Exact is not an aspiration here: `fmod` has a representable answer for every
 * pair of finite inputs, because it is x minus an integer multiple of y and
 * both are doubles. Long division on the mantissas gets it; anything going
 * through a quotient in floating point does not.
 */
double fmod(double x, double y)
{
	unsigned long long ux = to_bits(x);
	unsigned long long uy = to_bits(y);
	unsigned long long sign = ux & SIGN_BIT;
	int ex;
	int ey;

	if (recon_math_isnan(x) || recon_math_isnan(y) ||
	    recon_math_isinf(x) || y == 0.0) {
		return not_a_number();
	}
	if (recon_math_isinf(y) || x == 0.0) {
		return x;
	}

	ux &= ~SIGN_BIT;
	uy &= ~SIGN_BIT;
	if (ux < uy) {
		return x;	/* |x| < |y|: the whole of x is the remainder */
	}
	if (ux == uy) {
		return from_bits(sign);		/* signed zero */
	}

	/*
	 * Normalise both to a 53-bit integer mantissa and an exponent, so the
	 * loop below is integer arithmetic with nothing to round.
	 */
	ex = (int)(ux >> EXP_SHIFT);
	ey = (int)(uy >> EXP_SHIFT);
	if (ex == 0) {
		/* Subnormal: shift up until the implicit bit appears. */
		unsigned long long m = ux;

		ex = 1;
		while ((m & 0x0010000000000000ULL) == 0) {
			m <<= 1;
			ex--;
		}
		ux = m;
	} else {
		ux = (ux & FRAC_MASK) | 0x0010000000000000ULL;
	}
	if (ey == 0) {
		unsigned long long m = uy;

		ey = 1;
		while ((m & 0x0010000000000000ULL) == 0) {
			m <<= 1;
			ey--;
		}
		uy = m;
	} else {
		uy = (uy & FRAC_MASK) | 0x0010000000000000ULL;
	}

	/* Shift-and-subtract, one bit of quotient per turn. The quotient
	 * itself is discarded -- only the remainder is wanted. */
	while (ex > ey) {
		if (ux >= uy) {
			ux -= uy;
		}
		if (ux == 0) {
			return from_bits(sign);
		}
		ux <<= 1;
		ex--;
	}
	if (ux >= uy) {
		ux -= uy;
	}
	if (ux == 0) {
		return from_bits(sign);
	}

	/* Renormalise: shift the remainder up until the implicit bit is back
	 * in place, taking the exponent down with it. */
	while ((ux & 0x0010000000000000ULL) == 0) {
		ux <<= 1;
		ex--;
	}

	if (ex > 0) {
		ux = (ux - 0x0010000000000000ULL) |
			((unsigned long long)ex << EXP_SHIFT);
	} else {
		/* The answer is subnormal. Shift it back down into place;
		 * this is exact, because a remainder that lands here has
		 * fewer significant bits than the room it is going into. */
		ux >>= (1 - ex);
	}
	return from_bits(ux | sign);
}

/*
 * A float to the nearest long, halves to even.
 *
 * Halves to *even*, unlike `round` above -- `lrintf` follows the current
 * rounding direction, which is nearest-even unless something changed it, and
 * nothing in ReconOS changes it. `src/recon_codec.c` is the one caller and it
 * is converting audio samples, where the difference between the two rules is
 * a bias: rounding halves away from zero pushes every sample outward by a
 * fraction of a bit, which over a track is a tiny gain in level.
 */
long lrintf(float x)
{
	double d = (double)x;
	double r;

	if (recon_math_isnan(d) || recon_math_isinf(d)) {
		/* Unspecified by the standard. The reference returns the most
		 * negative long; matching it costs nothing and means a caller
		 * that hits this sees the same wrong number on both. */
		return (long)(-0x7FFFFFFFFFFFFFFFLL - 1);
	}

	r = floor(d);
	{
		double rest = d - r;

		if (rest > 0.5) {
			r += 1.0;
		} else if (rest == 0.5) {
			/* The half: take whichever neighbour is even. */
			if (fmod(r, 2.0) != 0.0) {
				r += 1.0;
			}
		}
	}
	return (long)r;
}

/* --- The twelve that are approximations --- */

/*
 * Every constant below is the decimal expansion of a number with a name, cut
 * where a double stops. They are written out rather than computed because
 * computing them at run time would compute them in double, which is the
 * precision they are here to exceed.
 */
#define LN2_HI   6.93147180369123816490e-01	/* ln2, top 33 bits */
#define LN2_LO   1.90821492927058770002e-10	/* and the rest */
#define INV_LN2  1.44269504088896338700e+00	/* 1/ln2 */
#define INV_LN10 4.34294481903251816668e-01	/* 1/ln10 */
#define SQRT_HALF 7.07106781186547524401e-01

/*
 * exp, by reduction to a small remainder and a power of two.
 *
 * x = k*ln2 + r with |r| <= ln2/2, so exp(x) = 2^k * exp(r). The reduction
 * subtracts ln2 in **two pieces**: k*LN2_HI is exact because LN2_HI has its
 * low bits clear and k is small, so the only rounding is in k*LN2_LO, which is
 * a tenth of a billionth the size. Subtracting a single double ln2 would lose
 * about as many bits as k has, which for x near 700 is ten of them.
 *
 * exp(r) on |r| <= 0.347 is then a Taylor series, which converges fast enough
 * there that the fourteenth term is below the rounding of the first.
 */
double exp(double x)
{
	int k;
	double r;
	double term;
	double sum;
	int i;

	if (recon_math_isnan(x)) {
		return x;
	}
	if (recon_math_isinf(x)) {
		return x > 0 ? x : 0.0;
	}
	if (x > 709.782712893383973096) {
		return infinity();		/* overflows a double */
	}
	if (x < -745.133219101941108420) {
		return 0.0;			/* underflows to nothing */
	}
	if (x == 0.0) {
		return 1.0;
	}

	k = (int)round(x * INV_LN2);
	r = (x - (double)k * LN2_HI) - (double)k * LN2_LO;

	/*
	 * Horner from the top, which sums the smallest terms first.
	 *
	 * The obvious loop -- start at 1 and add r, then r^2/2, and so on --
	 * adds each term to a running total that is already about 1, so a
	 * term of 1e-18 is rounded away before it can contribute. Nesting the
	 * series instead means the tiny terms meet each other first, and the
	 * whole nest is added to 1 exactly once at the end. Measured: it took
	 * exp from 4 ulp to 1.
	 *
	 *   r + r^2/2 + r^3/6 + ... = r(1 + (r/2)(1 + (r/3)(1 + ...)))
	 */
	sum = 0.0;
	for (i = 18; i >= 1; i--) {
		sum = (1.0 + sum) * r / (double)i;
	}
	(void)term;

	return scale_by_2(1.0 + sum, k);
}

/*
 * log, by pulling out the exponent and expanding what is left.
 *
 * x = m * 2^k with m in [sqrt(1/2), sqrt(2)), so log(x) = k*ln2 + log(m). The
 * range for m is centred on 1 rather than being [1, 2), which halves the
 * largest |s| the series below has to handle and is worth a whole term.
 *
 * log(m) = 2*atanh(s) where s = (m-1)/(m+1), and atanh's series has **only odd
 * powers** -- so for the same number of terms it reaches twice as far as the
 * series for log(1+u) would. |s| here is at most 0.1716.
 */
double log(double x)
{
	int k;
	double m;
	double s;
	double s2;
	double sum;
	double power;
	int i;

	if (recon_math_isnan(x)) {
		return x;
	}
	if (x < 0.0) {
		return not_a_number();
	}
	if (x == 0.0) {
		return -infinity();
	}
	if (recon_math_isinf(x)) {
		return x;
	}

	/* Split off the exponent. A subnormal is scaled up first so the bit
	 * pattern below means what it says. */
	k = 0;
	if (exponent_of(x) == 0) {
		x = scale_by_2(x, 64);
		k = -64;
	}
	k += exponent_of(x) - EXP_BIAS;
	m = from_bits((to_bits(x) & FRAC_MASK) |
		      ((unsigned long long)EXP_BIAS << EXP_SHIFT));

	/* Centre m on 1 rather than leaving it in [1, 2). */
	if (m < SQRT_HALF * 2.0) {
		/* m is in [1, sqrt(2)): leave it. */
	} else {
		m *= 0.5;
		k += 1;
	}

	s = (m - 1.0) / (m + 1.0);
	s2 = s * s;

	/* Horner from the top again, smallest terms first:
	 *   s + s^3/3 + s^5/5 + ... = s(1/1 + s^2(1/3 + s^2(1/5 + ...)))
	 */
	sum = 0.0;
	for (i = 31; i >= 1; i -= 2) {
		sum = sum * s2 + 1.0 / (double)i;
	}
	sum *= s;
	(void)power;

	/* k*ln2 in two pieces again, for the same reason it was split in exp:
	 * k can be a thousand, and a thousand times a rounded ln2 is wrong in
	 * the tenth digit. */
	return (double)k * LN2_HI + ((double)k * LN2_LO + 2.0 * sum);
}

double log10(double x)
{
	return log(x) * INV_LN10;
}

/*
 * --- Arithmetic that keeps what it would otherwise round away ---
 *
 * `two_sum` and `two_product` each return their answer *and the error they
 * made*, exactly. That sounds impossible and is not: the error of a rounded
 * sum or product is itself a representable double, so it can be recovered
 * rather than lost. These are Knuth's and Dekker's, and they are the whole
 * mechanism behind carrying a hundred bits in two doubles.
 *
 * Only `pow` needs them. Everything else here is content with the fifty-three
 * bits it has.
 */
static void two_sum(double a, double b, double *sum, double *error)
{
	double s = a + b;
	double bb = s - a;

	/* What `a + b` had to throw away, recovered by asking what each side
	 * would have had to be for the answer to be exact. */
	*error = (a - (s - bb)) + (b - bb);
	*sum = s;
}

/*
 * Dekker's splitting: 2^27 + 1 times a double, minus itself, leaves the top
 * half of the mantissa. Two halves of 26 bits multiply exactly, and the four
 * partial products reconstruct the error of the rounded whole.
 *
 * Safe for |a| and |b| below about 2^996, which everything here is: the
 * arguments are an exponent and a logarithm, and neither is astronomical.
 */
static void two_product(double a, double b, double *product, double *error)
{
	double p = a * b;
	double c = 134217729.0 * a;		/* 2^27 + 1 */
	double a_hi = c - (c - a);
	double a_lo = a - a_hi;
	double d = 134217729.0 * b;
	double b_hi = d - (d - b);
	double b_lo = b - b_hi;

	*error = ((a_hi * b_hi - p) + a_hi * b_lo + a_lo * b_hi) + a_lo * b_lo;
	*product = p;
}

/*
 * log(x) to about a hundred bits, in two doubles, for `pow` alone.
 *
 * `pow` is exp(y * log x), and exp turns an error in its argument into the
 * same *relative* error in its answer. So log's own half a unit in the last
 * place -- 3.5e-15 where log(x) is 17.5 -- becomes forty times that at y = 40,
 * which is 576 ulp and is exactly what the suite measured before this existed.
 * There is no arranging around it. pow needs more bits of log than a double
 * has.
 *
 * Three things are done differently from the ordinary `log` above:
 *
 *   `s` is refined. `s = f/(2+f)` rounds once, and that rounding is the
 *   largest error left; one Newton step on the division recovers it exactly
 *   into `s_lo`.
 *
 *   `2*s` is free. Doubling a double is exact, so the dominant term of the
 *   series needs no care at all once `s` has it.
 *
 *   The rest of the series is tiny. |s| is at most 0.1716, so everything past
 *   the first term is under 0.0034, and one double covers it to 4e-19 --
 *   below what the answer can see.
 */
static void log_extended(double x, double *hi, double *lo)
{
	int k = 0;
	double m;
	double f;
	double denominator;
	double s;
	double s_lo;
	double rest;
	double s2;
	double p;
	double pe;
	double e;
	int i;

	if (exponent_of(x) == 0) {
		x = scale_by_2(x, 64);
		k = -64;
	}
	k += exponent_of(x) - EXP_BIAS;
	m = from_bits((to_bits(x) & FRAC_MASK) |
		      ((unsigned long long)EXP_BIAS << EXP_SHIFT));
	if (m >= 1.4142135623730951) {
		m *= 0.5;
		k += 1;
	}

	/* f = m - 1 is exact: m is within a factor of sqrt(2) of 1, so the
	 * subtraction cancels nothing it cannot afford. */
	f = m - 1.0;
	denominator = 2.0 + f;
	s = f / denominator;

	/* One Newton step on the division, which recovers the rounding of it
	 * exactly. two_product gives the error of s*denominator; dividing
	 * that back by the denominator is the correction. */
	two_product(s, denominator, &p, &pe);
	s_lo = (f - p - pe) / denominator;

	/* The rest of the series, which is small enough for one double. */
	s2 = s * s;
	rest = 0.0;
	for (i = 31; i >= 3; i -= 2) {
		rest = rest * s2 + 1.0 / (double)i;
	}
	rest = 2.0 * (s * s2 * rest);

	/* k * ln2, exactly, in two pieces. */
	two_product((double)k, LN2_HI, &p, &pe);
	pe += (double)k * LN2_LO;

	/* Assemble, largest first, keeping every rounding. */
	two_sum(p, 2.0 * s, hi, &e);
	*lo = e + pe + 2.0 * s_lo + rest;

	/* One renormalisation, so the caller can rely on |lo| being under half
	 * a unit in the last place of hi. */
	two_sum(*hi, *lo, hi, lo);
}

/*
 * pow, which is the hard one and the one with the most special cases.
 *
 * The arithmetic is exp(y * log(x)), and the trouble is that log(x) is only
 * good to a double's worth of bits -- so y * log(x) loses however many bits y
 * has, and exp magnifies what is left. A y of 1000 costs ten bits of the
 * answer.
 *
 * The fix is to carry log(x) in two doubles. `log_hi` has its low bits
 * cleared, so `y * log_hi` is exact for a reasonable y; the correction rides
 * along in `log_lo` and is applied at the end, where exp(a+b) = exp(a)*(1+b)
 * for a small b costs one multiply and recovers the bits.
 */
double pow(double x, double y)
{
	int y_is_odd_integer = 0;
	int y_is_integer = 0;
	double log_hi;
	double log_lo;
	double result;

	/*
	 * The special cases, in the order the standard gives them, because
	 * several overlap and the order is what decides them. `pow(0, 0)` is
	 * 1 and `pow(nan, 0)` is 1 -- both are surprising, both are required,
	 * and both are before the NaN check for exactly that reason.
	 */
	if (y == 0.0) {
		return 1.0;
	}
	if (x == 1.0) {
		/*
		 * **Before** the NaN check, and that is the whole of what
		 * decides pow(1, nan). One to any power is one, including to
		 * a power that is not a number -- required, surprising, and
		 * invisible unless something asks.
		 */
		return 1.0;
	}
	if (recon_math_isnan(x) || recon_math_isnan(y)) {
		return not_a_number();
	}

	if (recon_math_isfinite(y) && fabs(y) < 9.0e15) {
		double whole = round(y);

		if (whole == y) {
			y_is_integer = 1;
			y_is_odd_integer = (fmod(fabs(whole), 2.0) == 1.0);
		}
	}

	if (x == 0.0) {
		if (y < 0.0) {
			/* A signed zero to a negative power is a signed
			 * infinity, and which one depends on whether the
			 * power was odd. */
			return (recon_math_signbit(x) && y_is_odd_integer)
				? -infinity() : infinity();
		}
		return (recon_math_signbit(x) && y_is_odd_integer) ? -0.0 : 0.0;
	}

	if (recon_math_isinf(x)) {
		if (x < 0.0) {
			double flipped = pow(-x, y);

			return y_is_odd_integer ? -flipped : flipped;
		}
		return y > 0.0 ? infinity() : 0.0;
	}

	if (recon_math_isinf(y)) {
		double size = fabs(x);

		if (size == 1.0) {
			return 1.0;	/* pow(-1, inf) is 1, which is also
					 * required and also surprising */
		}
		if ((size > 1.0) == (y > 0.0)) {
			return infinity();
		}
		return 0.0;
	}

	if (x < 0.0) {
		/*
		 * A negative base is only defined for an integer power --
		 * anything else is a root of a negative number, which is not
		 * a real. Refused as NaN, which is what the reference does.
		 */
		if (!y_is_integer) {
			return not_a_number();
		}
		{
			double flipped = pow(-x, y);

			return y_is_odd_integer ? -flipped : flipped;
		}
	}

	/*
	 * A small integer power is repeated squaring, not a logarithm.
	 *
	 * exp(3 * log(8)) has no reason to land on 512, and it did not: it
	 * came out 511.99999999999994, which is the kind of wrong a person
	 * notices immediately on a calculator. Squaring gets it exactly right
	 * whenever the answer is exactly representable, and is more accurate
	 * than the logarithm even when it is not -- six multiplications carry
	 * six roundings, where exp(y * log x) carries y times log's.
	 *
	 * Bounded at 8, which was measured rather than chosen. Squaring costs
	 * about one rounding per doubling, so its error grows with the
	 * logarithm of the power while the logarithm path's stays flat: at a
	 * power of 60 squaring was 33 ulp where the logarithm was 17, and at 8
	 * it is three. Eight is also comfortably past any power somebody types
	 * into a calculator, which is what this path is for.
	 */
	if (y_is_integer && fabs(y) <= 8.0) {
		double base = x;
		long n = (long)fabs(y);
		double accumulated = 1.0;
		int usable = 1;

		while (n > 0) {
			if (n & 1) {
				accumulated *= base;
			}
			n >>= 1;
			if (n != 0) {
				base *= base;
			}
			/*
			 * The guard is on the **intermediate**, not on the
			 * answer. pow(1.58e7, -43) is 2.5e-310, which is a
			 * perfectly good subnormal -- but 1.58e7 to the
			 * *positive* 43 is 1e310, which is not a double at
			 * all. Squaring cannot reach that answer and the
			 * logarithm can, so running out of range here means
			 * handing the question on rather than returning what
			 * the overflow left behind.
			 */
			if (!recon_math_isfinite(accumulated) ||
			    (n != 0 && !recon_math_isfinite(base))) {
				usable = 0;
				break;
			}
		}
		if (usable) {
			return y < 0.0 ? 1.0 / accumulated : accumulated;
		}
	}

	/* log(x) to a hundred bits, because fifty-three is not enough here. */
	log_extended(x, &log_hi, &log_lo);

	{
		double whole;
		double whole_error;
		double small;

		/* y * log_hi, and the bits that multiply threw away. */
		two_product(y, log_hi, &whole, &whole_error);
		small = whole_error + y * log_lo;

		/*
		 * The exponent is `whole + small`, and `small` is under a unit
		 * in the last place of `whole` -- so exp(whole + small) is
		 * exp(whole) * exp(small), and exp of something that small is
		 * 1 + small + small^2/2 with the third term already below the
		 * rounding.
		 *
		 * Overflow and underflow are decided on `whole` alone, before
		 * the correction: a result that is infinite stays infinite
		 * however it is nudged, and multiplying an infinity by
		 * (1 + small) would make it a NaN.
		 */
		if (whole > 709.782712893383973096) {
			return infinity();
		}
		if (whole < -745.133219101941108420) {
			return 0.0;
		}

		result = exp(whole);
		result = result * (1.0 + small * (1.0 + 0.5 * small));
	}
	return result;
}

/* --- The circle --- */

/*
 * --- Reducing an angle, which is the whole of what makes sin usable ---
 *
 * sin repeats every two pi, so sin(x) for a large x is sin of a small
 * remainder. Finding that remainder is the hard part, and the obvious way is
 * wrong in a way that is easy to miss.
 *
 * Subtracting a rounded pi/2 costs **one bit of the remainder for every bit of
 * the quotient**. At x = 1e6 the quotient has twenty bits, so twenty of the
 * remainder's fifty-three are gone; at 1e15 there is nothing left. The first
 * version of this file did that, and the suite measured 860,948,872,375 units
 * in the last place at three pi -- not an inaccurate answer, a wrong one.
 *
 * The cure is to hold 2/pi to as many bits as the argument can demand and do
 * the multiply in integers, where nothing rounds. A double reaches 2^1024, so
 * the table is two thousand bits long; `scripts/gen-two-over-pi.py` computes
 * it and checks it against the published expansion before writing it.
 *
 * This is Payne and Hanek's reduction, in the form that only keeps the window
 * of the product that matters.
 */

#include "two_over_pi.inc"

#define PIO2_1  1.57079632673412561417e+00	/* pi/2, top 33 bits */
#define PIO2_2  6.07710050630396597660e-11	/* the next 33 */
#define PIO2_3  2.02226624871116645580e-21	/* and the next */
#define PIO2_3T 8.47842766036889956997e-32	/* and the rest */

/*
 * The remainder of x modulo pi/2, and which quadrant it came out of.
 *
 * Returns the quadrant 0-3; `*out` gets the remainder, |r| <= pi/4. sin and
 * cos then differ only in which quadrant asks for which kernel, which is what
 * stops the two drifting apart.
 */
static int reduce_quadrant(double x, double *out)
{
	unsigned long long w = to_bits(x);
	int negative = (w & SIGN_BIT) != 0;
	int biased = (int)((w >> EXP_SHIFT) & 0x7FF);
	unsigned long long mantissa;
	int p;
	unsigned __int128 acc = 0;
	int i;
	int quadrant;
	double fraction_hi;
	double fraction_lo;
	double r;

	/*
	 * Small enough to need no reduction at all. |x| < pi/4 is already the
	 * remainder, and going through the machinery below would only round
	 * something that is exactly right.
	 */
	if (fabs(x) < 0.78539816339744827900) {
		*out = x;
		return 0;
	}

	/* |x| = mantissa * 2^p, with mantissa a 53-bit integer. */
	if (biased == 0) {
		/* A subnormal is nowhere near pi/4, so this cannot be
		 * reached -- but a reduction that silently mishandled one
		 * would be a fault nothing looked for. */
		*out = x;
		return 0;
	}
	mantissa = (w & FRAC_MASK) | 0x0010000000000000ULL;
	p = biased - EXP_BIAS - 52;

	/*
	 * The accumulator holds x * (2/pi) with the binary point 126 places
	 * up: two integer bits, which is all the quadrant needs, and 126
	 * fractional ones, which is more than twice what the remainder does.
	 *
	 * 2/pi is a fraction, so word i of the table stands for
	 * 2^(-32*(i+1)). Multiplying by the mantissa and shifting by p puts
	 * each partial product where it belongs; the window of words that can
	 * reach the accumulator at all is about five wide, and the loop finds
	 * it rather than computing an index and being wrong at a boundary.
	 */
	for (i = 0; i < (int)(sizeof(TWO_OVER_PI) / sizeof(TWO_OVER_PI[0]));
	     i++) {
		int shift = p - 32 * (i + 1) + 126;
		unsigned __int128 part;

		if (shift >= 128) {
			/*
			 * This partial product lands entirely above the two
			 * integer bits, so it is a multiple of four and
			 * changes neither the quadrant nor the remainder.
			 */
			continue;
		}
		if (shift <= -128) {
			/* And this one is entirely below the last fractional
			 * bit. Everything after it is smaller still. */
			break;
		}

		part = (unsigned __int128)mantissa *
			(unsigned __int128)TWO_OVER_PI[i];
		if (shift >= 0) {
			acc += part << shift;
		} else {
			acc += part >> (-shift);
		}
	}

	quadrant = (int)((acc >> 126) & 3);
	acc &= ((unsigned __int128)1 << 126) - 1;

	/*
	 * A fraction over a half belongs to the next quadrant, with a negative
	 * remainder -- that is what keeps |r| <= pi/4 rather than pi/2, and
	 * halves the largest value the kernels below ever see.
	 */
	{
		int over_half = (acc >> 125) != 0;
		signed __int128 signed_fraction = (signed __int128)acc;
		unsigned __int128 size;
		int flipped = 0;
		double p0;
		double p1;
		double p2;
		double big;
		double small;

		/*
		 * A fraction over a half belongs to the next quadrant with a
		 * negative remainder. That is what keeps |r| below pi/4 rather
		 * than pi/2, and it halves the largest value the kernels below
		 * ever see -- which is worth a term of every series in them.
		 */
		if (over_half) {
			signed_fraction -= ((signed __int128)1 << 126);
			quadrant = (quadrant + 1) & 3;
		}
		if (signed_fraction < 0) {
			flipped = 1;
			size = (unsigned __int128)(-signed_fraction);
		} else {
			size = (unsigned __int128)signed_fraction;
		}

		/*
		 * Three pieces of 42 bits, **not two of 63**.
		 *
		 * Two of 63 is the obvious split and it is wrong: converting a
		 * 63-bit integer to a double rounds, so bits 53 to 62 are in
		 * neither piece. The measurement said so -- 4,598 ulp at three
		 * pi, which is where the fraction is small and those are the
		 * only bits there are. 42 fits in a double exactly with room
		 * over, and three of them cover the accumulator.
		 */
		p0 = (double)(unsigned long long)(size >> 84);
		p1 = (double)(unsigned long long)
			((size >> 42) & 0x3FFFFFFFFFFULL);
		p2 = (double)(unsigned long long)(size & 0x3FFFFFFFFFFULL);

		/* Each scaling is a power of two, so each is exact. */
		p0 *= 2.2737367544323206e-13;	/* 2^-42 */
		p1 *= 5.1698788284564229e-26;	/* 2^-84 */
		p2 *= 1.1754943508222875e-38;	/* 2^-126 */

		/*
		 * Multiplied by pi/2 in its own four pieces, with the big term
		 * kept apart from the corrections until the end -- so that a
		 * fraction of a hundred bits is never multiplied by a pi/2 of
		 * fifty-three, and the corrections meet each other before they
		 * meet something a million times their size.
		 */
		big = p0 * PIO2_1;
		small = (p1 * PIO2_1 + p2 * PIO2_1)
			+ (p0 * PIO2_2 + p1 * PIO2_2)
			+ p0 * PIO2_3;
		r = big + small;

		fraction_hi = 0.0;
		fraction_lo = 0.0;
		(void)fraction_hi;
		(void)fraction_lo;

		if (flipped) {
			r = -r;
		}
	}

	/*
	 * The table was walked for |x|. A negative x reflects: the remainder
	 * changes sign and the quadrant counts the other way round the circle.
	 */
	if (negative) {
		r = -r;
		quadrant = (4 - quadrant) & 3;
	}

	*out = r;
	return quadrant;
}

/* sin(r) for |r| <= pi/4, by its Taylor series -- odd powers only, so seven
 * terms reach further than fourteen of a general series would. */
static double sin_kernel(double r)
{
	double r2 = r * r;
	double sum = 0.0;
	int i;

	/*
	 * Horner from the top, smallest first:
	 *   r - r^3/3! + r^5/5! - ... = r(1 - (r^2/(2*3))(1 - (r^2/(4*5))(...)))
	 *
	 * And the whole thing is r * (1 + nest) rather than r + r*nest, so
	 * that sin(r) for a tiny r comes back as exactly r -- which it must,
	 * because the true value differs from r by less than half an ulp
	 * there and the reference returns r.
	 */
	for (i = 21; i >= 3; i -= 2) {
		sum = -(1.0 + sum) * r2 / ((double)i * (double)(i - 1));
	}
	return r * (1.0 + sum);
}

/* cos(r) for |r| <= pi/4, even powers only. */
static double cos_kernel(double r)
{
	double r2 = r * r;
	double sum = 0.0;
	int i;

	for (i = 22; i >= 2; i -= 2) {
		sum = -(1.0 + sum) * r2 / ((double)i * (double)(i - 1));
	}
	return 1.0 + sum;
}

double sin(double x)
{
	double r;
	int quadrant;

	if (recon_math_isnan(x)) {
		return x;
	}
	if (recon_math_isinf(x)) {
		/* No answer exists: an infinity is not at any point on the
		 * circle. NaN, which is what the reference returns. */
		return not_a_number();
	}

	quadrant = reduce_quadrant(x, &r);
	switch (quadrant) {
	case 0:  return sin_kernel(r);
	case 1:  return cos_kernel(r);
	case 2:  return -sin_kernel(r);
	default: return -cos_kernel(r);
	}
}

double cos(double x)
{
	double r;
	int quadrant;

	if (recon_math_isnan(x)) {
		return x;
	}
	if (recon_math_isinf(x)) {
		return not_a_number();
	}

	quadrant = reduce_quadrant(x, &r);
	switch (quadrant) {
	case 0:  return cos_kernel(r);
	case 1:  return -sin_kernel(r);
	case 2:  return -cos_kernel(r);
	default: return sin_kernel(r);
	}
}

double tan(double x)
{
	double r;
	int quadrant;
	double s;
	double c;

	if (recon_math_isnan(x)) {
		return x;
	}
	if (recon_math_isinf(x)) {
		return not_a_number();
	}

	quadrant = reduce_quadrant(x, &r);
	s = sin_kernel(r);
	c = cos_kernel(r);

	/*
	 * tan repeats every half turn rather than every whole one, so only
	 * the low bit of the quadrant matters. An odd quadrant is -cos/sin,
	 * which is where tan runs to infinity -- and it is left to do so
	 * rather than being clamped, because a caller who asked for tan(pi/2)
	 * wants to find out.
	 */
	if (quadrant & 1) {
		return -c / s;
	}
	return s / c;
}

/*
 * atan, by folding the whole real line into [0, tan(pi/12)] and expanding
 * there.
 *
 * Two identities do the folding. atan(x) = pi/2 - atan(1/x) brings anything
 * above 1 down below it, and atan(x) = pi/6 + atan((x*sqrt3 - 1)/(x +
 * sqrt3)) brings [tan(pi/12), 1] down to [0, tan(pi/12)] -- which is 0.2679,
 * where the series converges four times as fast as it would at 1.
 *
 * Without the second fold the series at x = 1 converges so slowly that it
 * would need hundreds of terms, and it is the classic way this function comes
 * out slow and wrong at the same time.
 */
#define PI       3.14159265358979311600e+00
#define PI_2     1.57079632679489655800e+00
#define PI_4     7.85398163397448278999e-01
#define PI_6     5.23598775598298873077e-01
#define SQRT3    1.73205080756887729353e+00
#define TAN_PI12 2.67949192431122706472e-01

static double atan_series(double x)
{
	double x2 = x * x;
	double sum = 0.0;
	int i;

	/* x - x^3/3 + x^5/5 - ..., by Horner from the top. |x| is at most
	 * tan(pi/12) = 0.268 by the time it gets here, so twenty-one terms
	 * reach far below a double's last bit. */
	for (i = 41; i >= 1; i -= 2) {
		sum = -sum * x2 + 1.0 / (double)i;
	}
	return x * sum;
}

double atan(double x)
{
	int negative = 0;
	int inverted = 0;
	int folded = 0;
	double result;

	if (recon_math_isnan(x)) {
		return x;
	}
	if (recon_math_isinf(x)) {
		return x > 0 ? PI_2 : -PI_2;
	}

	if (x < 0.0) {
		negative = 1;
		x = -x;
	}
	if (x > 1.0) {
		inverted = 1;
		x = 1.0 / x;
	}
	if (x > TAN_PI12) {
		folded = 1;
		x = (x * SQRT3 - 1.0) / (x + SQRT3);
	}

	result = atan_series(x);
	if (folded) {
		result += PI_6;
	}
	if (inverted) {
		result = PI_2 - result;
	}
	return negative ? -result : result;
}

/*
 * asin and acos, through atan and a square root.
 *
 * asin(x) = atan(x / sqrt(1 - x^2)), which is exact as an identity and loses
 * accuracy only where 1 - x^2 does -- near |x| = 1, where the subtraction
 * cancels. That is handled by rewriting 1 - x^2 as (1-x)(1+x), which is two
 * exact operations where the first is the only one that cancels, and it
 * cancels against a number the caller gave rather than against a square.
 */
double asin(double x)
{
	double size = fabs(x);
	double root;

	if (recon_math_isnan(x)) {
		return x;
	}
	if (size > 1.0) {
		return not_a_number();	/* outside the domain */
	}
	if (size == 1.0) {
		return x > 0 ? PI_2 : -PI_2;
	}

	root = sqrt((1.0 - size) * (1.0 + size));
	return x < 0 ? -atan(size / root) : atan(size / root);
}

double acos(double x)
{
	if (recon_math_isnan(x)) {
		return x;
	}
	if (fabs(x) > 1.0) {
		return not_a_number();
	}
	if (x == 1.0) {
		return 0.0;
	}
	if (x == -1.0) {
		return PI;
	}

	if (x == 0.0) {
		/* Exactly pi/2, and worth its own line: the general form below
		 * divides by x. The first version of this function tried to
		 * dodge that with a ternary, divided by zero anyway, and
		 * returned pi. */
		return PI_2;
	}

	/*
	 * acos(x) = atan(sqrt(1-x^2) / x) for a positive x, and pi minus the
	 * same thing for a negative one.
	 *
	 * **Not** pi/2 - asin(x), which is the identity everybody reaches for
	 * and which throws the answer away near x = 1: there asin is close to
	 * pi/2, so the subtraction cancels the leading digits and what
	 * survives is the rounding. This form has the two halves of the domain
	 * computed differently for exactly that reason -- each one the way that
	 * does not cancel.
	 *
	 * 1 - x^2 is written (1-x)(1+x) for the same kind of reason: as x
	 * approaches 1 the first factor cancels against a number the caller
	 * gave, which is exact, rather than against a square, which is not.
	 */
	{
		double size = fabs(x);
		double root = sqrt((1.0 - size) * (1.0 + size));

		if (x > 0.0) {
			return atan(root / x);
		}
		return PI - atan(root / size);
	}
}

/*
 * atan2, which is atan told which quadrant it is in.
 *
 * The whole reason it exists is that atan(y/x) throws the signs away before
 * it is called, so it cannot tell the second quadrant from the fourth. Every
 * branch below is recovering that.
 */
double atan2(double y, double x)
{
	if (recon_math_isnan(x) || recon_math_isnan(y)) {
		return not_a_number();
	}

	if (recon_math_isinf(x) || recon_math_isinf(y)) {
		/*
		 * Both infinite: the answer is the diagonal of whichever
		 * quadrant, because "infinitely far along both axes" is a
		 * direction even though the ratio is not a number.
		 */
		if (recon_math_isinf(x) && recon_math_isinf(y)) {
			double base = x > 0 ? PI_4 : (PI - PI_4);

			return y > 0 ? base : -base;
		}
		if (recon_math_isinf(y)) {
			return y > 0 ? PI_2 : -PI_2;
		}
		if (x > 0) {
			return recon_math_signbit(y) ? -0.0 : 0.0;
		}
		return recon_math_signbit(y) ? -PI : PI;
	}

	if (x == 0.0) {
		if (y == 0.0) {
			/*
			 * Both zero, and the answer still depends on the
			 * *sign* of the zeros -- which is the case that
			 * surprises people and is required.
			 */
			if (!recon_math_signbit(x)) {
				return recon_math_signbit(y) ? -0.0 : 0.0;
			}
			return recon_math_signbit(y) ? -PI : PI;
		}
		return y > 0 ? PI_2 : -PI_2;
	}

	if (y == 0.0) {
		if (x > 0.0) {
			return recon_math_signbit(y) ? -0.0 : 0.0;
		}
		return recon_math_signbit(y) ? -PI : PI;
	}

	{
		double angle = atan(fabs(y / x));

		if (x > 0.0) {
			return y > 0.0 ? angle : -angle;
		}
		return y > 0.0 ? PI - angle : angle - PI;
	}
}

/*
 * The cube root, computed where nothing can overflow.
 *
 * The first version worked on x directly and burst at both ends of the range:
 * its last step was `guess * (cube + 2x) / (2cube + x)`, which multiplies
 * before it divides -- so at x = 1e300 the numerator is 1e100 times 3e300 and
 * there is no such double. The ratio is always about one; only the spelling
 * was astronomical. At the other end the same line underflowed to zero, and at
 * DBL_MAX `guess^3` overflowed before the step began.
 *
 * Found by printing the intermediates. The formula was right the whole time,
 * which is exactly why reading it again would not have helped.
 *
 * So x is reduced first: |x| = y * 2^(3q) with y in [0.5, 4), the root is
 * taken there -- where every intermediate is between 0.79 and 1.6 -- and the
 * answer is scaled back by 2^q. That removes the possibility rather than the
 * instance, which is the difference between a fix and a patch.
 *
 * Unlike `sqrt` this cannot borrow an instruction; no processor has one.
 */
double cbrt(double x)
{
	/* cbrt(1) and cbrt(2) and cbrt(4), for putting back a reduction of one
	 * or two powers of two. Written out rather than computed, because
	 * computing them would compute them with this function. */
	static const double CBRT_OF_POWER[3] = {
		1.0,
		1.2599210498948731648,
		1.5874010519681994748,
	};
	int negative = 0;
	int borrowed;
	int e;
	int q;
	int rest;
	double y;
	double guess;
	int i;

	if (recon_math_isnan(x) || recon_math_isinf(x) || x == 0.0) {
		return x;
	}
	if (x < 0.0) {
		negative = 1;
		x = -x;
	}

	/*
	 * A subnormal has no exponent to read, so it is scaled into the
	 * normals first -- and **the borrowed shift has to be given back
	 * everywhere the value is used afterwards**, not only where the
	 * exponent is computed. Missing it in the second place turned
	 * cbrt(1e-308) into a number thirty-five orders of magnitude out.
	 */
	borrowed = 0;
	if (exponent_of(x) == 0) {
		x = scale_by_2(x, 120);
		borrowed = 120;
	}
	e = exponent_of(x) - EXP_BIAS - borrowed;

	/*
	 * Floored division, not C's. `-808 / 3` is -269 in C and the floor is
	 * -270, and taking the wrong one leaves the reduced value outside the
	 * range the iteration below is sized for.
	 */
	q = e / 3;
	rest = e - q * 3;
	if (rest < 0) {
		q -= 1;
		rest += 3;
	}

	/*
	 * Reduced to the mantissa alone -- [1, 2) -- rather than to [1, 8).
	 *
	 * The wider range is what the exponent naturally leaves behind, and a
	 * linear first guess is 67% out at the top of it: four Newton steps
	 * from there do not reach the last bit. Taking the mantissa on its own
	 * and putting the leftover factor back afterwards keeps the guess
	 * within 6%, where the iteration has room to spare.
	 */
	y = from_bits((to_bits(x) & FRAC_MASK) |
		      ((unsigned long long)EXP_BIAS << EXP_SHIFT));

	/* The tangent to the cube root at 1, which is the whole of the guess
	 * and is never more than 6% out over [1, 2). */
	guess = 1.0 + (y - 1.0) * 0.33333333333333333;

	for (i = 0; i < 4; i++) {
		guess = (2.0 * guess + y / (guess * guess)) / 3.0;
	}

	{
		double cube = guess * guess * guess;

		/*
		 * The division first, then the multiply. Both operands are
		 * near one here so it makes no difference to this arrangement
		 * -- it is written this way because the other order is what
		 * broke, and a reader of the earlier version should be able to
		 * see that the order was the point.
		 */
		guess = guess * ((cube + 2.0 * y) / (2.0 * cube + y));
	}

	/* Put back the power of two the mantissa was taken out of, then the
	 * one the exponent was divided by. */
	guess *= CBRT_OF_POWER[rest];
	guess = scale_by_2(guess, q);

	return negative ? -guess : guess;
}

/* --- Two the compiler asks for and the source never writes ---------------
 *
 * Neither `sqrtf` nor `sincos` appears anywhere in ReconOS. GCC puts them
 * there: a `sqrt` whose argument and result are both floats is narrowed to the
 * single-precision instruction, and a `sin(x)` and a `cos(x)` of the same
 * argument are fused into one call that computes both. Both substitutions
 * happen at -O2 and above and at no optimisation level below it, which is why
 * a release build of the desktop needed symbols a debug build did not.
 */

float sqrtf(float x)
{
	/*
	 * The hardware instruction again -- `sqrtss` and `fsqrt` -- for the
	 * same reason `sqrt` uses it: IEEE 754 requires a correctly rounded
	 * result and the processor gives one.
	 *
	 * Written as the single-precision builtin rather than as
	 * `(float)sqrt((double)x)`. That form happens to be exactly right for
	 * square root -- a double carries more than twice a float's
	 * significand, so the double rounding cannot land on the wrong side --
	 * but it is right by an argument rather than by construction, and it
	 * does the work in the wrong precision on the way.
	 */
	return __builtin_sqrtf(x);
}

/*
 * Both at once.
 *
 * Not faster here: it is two calls with a shared argument, and the reduction
 * of that argument -- the expensive half of either function, and the reason
 * `two_over_pi.inc` exists -- is done twice. glibc shares it. This does not,
 * yet, and saying so is better than implying an optimisation that is not here:
 * what this provides is the **symbol**, so that a program the compiler rewrote
 * links and computes the right two numbers.
 *
 * The sharing is a later change to this one function and to nothing else,
 * which is the reason it is worth writing it this way now.
 */
void sincos(double x, double *sine, double *cosine)
{
	*sine = sin(x);
	*cosine = cos(x);
}
