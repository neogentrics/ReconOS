/*
 * The twenty maths functions, against the host's.
 *
 * Two standards, because these functions divide into two kinds and pretending
 * otherwise would let the weaker half hide behind the stronger.
 *
 * **Eight have an exactly right answer** -- `fabs`, `floor`, `ceil`, `round`,
 * `fmod`, `ldexp`, `lrintf` and `sqrt` -- and are held to **bit-for-bit
 * equality**, including the sign of a zero, which is the part that is easy to
 * get wrong and impossible to see.
 *
 * **Twelve are approximations.** Their true values are not representable, so
 * there is no equality to hold them to. They are held to a bound in units in
 * the last place, and **the suite prints the worst error it found** for each
 * one rather than only whether it stayed under the line. A tolerance nobody
 * has looked inside is a tolerance that can grow by a factor of a thousand
 * and still pass.
 *
 * The bounds below were set *after* the runs, from what they measured.
 * Setting them first would have been setting them from hope. Each is the
 * figure achieved, rounded up to the next even number -- enough room that a
 * compiler or a host libm version cannot fail the suite by a single unit, and
 * not enough to hide a regression.
 *
 * **`pow` is the outlier at seventeen, and it is inherent.** Every other
 * function here computes its answer directly; pow computes exp(y * log x),
 * and exp turns an error in its argument into the same relative error in its
 * result -- so log's own last bit is multiplied by y before it ever reaches
 * the answer. Carrying log to a hundred bits took it from 576 ulp to 17, and
 * the rest would need exp carried the same way. Seventeen units is two parts
 * in 1e15: the last two digits of sixteen. A calculator shows fifteen.
 *
 * The four faults the first runs found are recorded as BG-186 to BG-189, and
 * every one of them was a wrong answer rather than an inaccurate one -- which
 * is what a suite that measures an error in units in the last place is for.
 * A tolerance of "close enough" would have passed three of them.
 *
 * Run with: ./build/recon_libc_math_tests
 */

#define _GNU_SOURCE

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ReconOS's, renamed by prefix.h when libc/ was compiled. */
double recon_fabs(double x);
double recon_sqrt(double x);
double recon_floor(double x);
double recon_ceil(double x);
double recon_round(double x);
double recon_fmod(double x, double y);
double recon_ldexp(double x, int n);
long recon_lrintf(float x);

double recon_exp(double x);
double recon_log(double x);
double recon_log10(double x);
double recon_pow(double x, double y);
double recon_sin(double x);
double recon_cos(double x);
double recon_tan(double x);
double recon_asin(double x);
double recon_acos(double x);
double recon_atan(double x);
double recon_atan2(double y, double x);
double recon_cbrt(double x);

int recon_math_isnan(double x);
int recon_math_isinf(double x);
int recon_math_isfinite(double x);
int recon_math_signbit(double x);

static int g_failures;
static long g_checks;

static void check(int condition, const char *what)
{
	g_checks++;
	if (!condition) {
		g_failures++;
		printf("  FAIL: %s\n", what);
	}
}

/* --- Comparing two doubles --- */

static uint64_t bits_of(double x)
{
	uint64_t w;

	memcpy(&w, &x, sizeof(w));
	return w;
}

/*
 * The distance between two doubles, counted in representable numbers.
 *
 * The trick is that the bit patterns of the positive doubles are in the same
 * order as the doubles themselves, so subtracting them counts how many
 * distinct values lie between -- which is what "units in the last place"
 * means and is the only scale-free way to talk about the error of a function
 * that has to work at 1e-300 and 1e300.
 *
 * The negatives are folded onto the same line first, so the count is
 * continuous across zero rather than jumping by 2^63 there.
 */
static long long ulp_gap(double a, double b)
{
	int64_t ia;
	int64_t ib;

	if (a == b) {
		return 0;
	}
	if (recon_math_isnan(a) || recon_math_isnan(b)) {
		return -1;		/* not comparable; caller handles */
	}

	ia = (int64_t)bits_of(a);
	ib = (int64_t)bits_of(b);
	if (ia < 0) {
		ia = (int64_t)0x8000000000000000LL - ia;
	}
	if (ib < 0) {
		ib = (int64_t)0x8000000000000000LL - ib;
	}

	/* Clamped rather than allowed to overflow. Two values far enough apart
	 * for this to matter have already failed by any bound worth having. */
	if ((ia > 0) != (ib > 0) && (ia > 0 ? ia : -ia) > 0x4000000000000000LL) {
		return 0x7FFFFFFFFFFFFFFFLL;
	}
	return ia > ib ? ia - ib : ib - ia;
}

/* --- The exact eight --- */

/*
 * Bit-for-bit, which catches the sign of a zero. `floor(-0.4)` is -0.0 and
 * not 0.0, and a caller who then divides by it gets -inf rather than +inf.
 * Nothing but a bit comparison can see that: the two are ==.
 */
static void same_bits(double mine, double theirs, const char *what,
		      double input)
{
	g_checks++;
	if (bits_of(mine) != bits_of(theirs)) {
		/* Two NaNs with different payloads are the same answer. */
		if (recon_math_isnan(mine) && recon_math_isnan(theirs)) {
			return;
		}
		g_failures++;
		printf("  FAIL: %s(%.17g)\n"
		       "    ReconOS: %.17g [%016llx]\n"
		       "    reference: %.17g [%016llx]\n",
		       what, input, mine,
		       (unsigned long long)bits_of(mine), theirs,
		       (unsigned long long)bits_of(theirs));
	}
}

/*
 * The awkward values, which is where every one of these functions is wrong if
 * it is wrong anywhere: the zeros with their signs, the infinities, a NaN,
 * the largest and smallest normals, a subnormal, and the boundaries where a
 * rounding function has to decide.
 */
static const double SPECIALS[] = {
	0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 1.5, -1.5, 2.5, -2.5,
	0.4999999999999999, -0.4999999999999999,
	0.5000000000000001, -0.5000000000000001,
	2.0, -2.0, 3.0, -3.0, 0.1, -0.1, 0.9, -0.9,
	4503599627370495.5,	/* the largest double with a half in it */
	4503599627370496.0,	/* 2^52 -- everything above is integral */
	9007199254740992.0,	/* 2^53 */
	1e15, -1e15, 1e16, -1e16, 1e300, -1e300,
	DBL_MAX, -DBL_MAX, DBL_MIN, -DBL_MIN,
	5e-324, -5e-324,	/* the smallest subnormal */
	1e-308, 123.456, -123.456, 1e-10, -1e-10,
	3.14159265358979, 2.718281828459045, 1e100, 1e-100,
};

#define SPECIAL_COUNT (sizeof(SPECIALS) / sizeof(SPECIALS[0]))

static void test_the_exact_ones(void)
{
	size_t i;
	size_t j;
	double x;

	printf("the eight that are exactly right, bit for bit\n");

	for (i = 0; i < SPECIAL_COUNT; i++) {
		x = SPECIALS[i];
		same_bits(recon_fabs(x), fabs(x), "fabs", x);
		same_bits(recon_floor(x), floor(x), "floor", x);
		same_bits(recon_ceil(x), ceil(x), "ceil", x);
		same_bits(recon_round(x), round(x), "round", x);
		same_bits(recon_sqrt(x), sqrt(x), "sqrt", x);
	}

	/* The infinities and a NaN, which are not in the table above because
	 * they cannot be written as literals without a warning. */
	{
		double weird[3];

		weird[0] = INFINITY;
		weird[1] = -INFINITY;
		weird[2] = NAN;
		for (i = 0; i < 3; i++) {
			x = weird[i];
			same_bits(recon_fabs(x), fabs(x), "fabs", x);
			same_bits(recon_floor(x), floor(x), "floor", x);
			same_bits(recon_ceil(x), ceil(x), "ceil", x);
			same_bits(recon_round(x), round(x), "round", x);
			same_bits(recon_sqrt(x), sqrt(x), "sqrt", x);
		}
	}

	/* A sweep, at a stride that is not a round number so it lands on
	 * fractions rather than repeatedly on the same few. */
	printf("  and swept across the line\n");
	for (i = 0; i < 200000; i++) {
		x = ((double)(long)i - 100000.0) * 0.0007312589;
		same_bits(recon_floor(x), floor(x), "floor", x);
		same_bits(recon_ceil(x), ceil(x), "ceil", x);
		same_bits(recon_round(x), round(x), "round", x);
		same_bits(recon_fabs(x), fabs(x), "fabs", x);
		if (x >= 0.0) {
			same_bits(recon_sqrt(x), sqrt(x), "sqrt", x);
		}
	}

	printf("  fmod, over pairs including the ones that are not numbers\n");
	for (i = 0; i < SPECIAL_COUNT; i++) {
		for (j = 0; j < SPECIAL_COUNT; j++) {
			double a = SPECIALS[i];
			double b = SPECIALS[j];

			same_bits(recon_fmod(a, b), fmod(a, b), "fmod", a);
		}
	}
	/* And a sweep, because the loop inside fmod is where it would be
	 * wrong and the table above barely enters it. */
	for (i = 0; i < 60000; i++) {
		double a = ((double)(long)i - 30000.0) * 0.017;
		double b = 0.3 + (double)(i % 97) * 0.11;

		same_bits(recon_fmod(a, b), fmod(a, b), "fmod", a);
		same_bits(recon_fmod(a, -b), fmod(a, -b), "fmod", a);
	}

	printf("  ldexp, at every exponent that means anything\n");
	for (i = 0; i < SPECIAL_COUNT; i++) {
		int n;

		for (n = -1200; n <= 1200; n += 7) {
			double a = SPECIALS[i];

			same_bits(recon_ldexp(a, n), ldexp(a, n), "ldexp", a);
		}
	}

	printf("  lrintf, including the halves it has to break\n");
	for (i = 0; i < 100000; i++) {
		float f = (float)(((double)(long)i - 50000.0) * 0.25);
		long mine = recon_lrintf(f);
		long theirs = lrintf(f);

		g_checks++;
		if (mine != theirs) {
			g_failures++;
			printf("  FAIL: lrintf(%.9g)\n    ReconOS: %ld  "
			       "reference: %ld\n", (double)f, mine, theirs);
		}
	}
}

/* --- The twelve approximations --- */

struct worst {
	long long ulp;
	double input;
	double second;
	double mine;
	double theirs;
};

static void consider(struct worst *w, double mine, double theirs,
		     double input, double second)
{
	long long gap;

	g_checks++;

	/* A NaN on one side and not the other is a fault however small the
	 * tolerance, and is not an error measured in ULP. */
	if (recon_math_isnan(mine) != recon_math_isnan(theirs)) {
		g_failures++;
		printf("  FAIL: one of them is not a number, at %.17g\n"
		       "    ReconOS: %.17g   reference: %.17g\n",
		       input, mine, theirs);
		return;
	}
	if (recon_math_isnan(mine)) {
		return;
	}
	if (recon_math_isinf(mine) != recon_math_isinf(theirs)) {
		g_failures++;
		printf("  FAIL: one of them is infinite, at %.17g\n"
		       "    ReconOS: %.17g   reference: %.17g\n",
		       input, mine, theirs);
		return;
	}

	gap = ulp_gap(mine, theirs);
	if (gap > w->ulp) {
		w->ulp = gap;
		w->input = input;
		w->second = second;
		w->mine = mine;
		w->theirs = theirs;
	}
}

/*
 * Print what the worst error actually was, and fail if it is over the bound.
 *
 * The printing is the point. A suite that says only "under 4 ulp" hides the
 * difference between 0 and 3, and the day a change makes it 3.9 nothing says
 * so until the day after, when it is 5.
 */
static void report(const char *name, struct worst *w, long long bound)
{
	g_checks++;
	printf("    %-8s worst %4lld ulp", name, w->ulp);
	if (w->ulp > 0) {
		printf("   at %.17g", w->input);
	}
	printf("\n");

	if (w->ulp > bound) {
		g_failures++;
		printf("  FAIL: %s is %lld ulp out, over its bound of %lld\n"
		       "    at %.17g%s%.17g\n"
		       "    ReconOS: %.17g\n    reference: %.17g\n",
		       name, w->ulp, bound, w->input,
		       w->second == w->second ? ", " : "", w->second,
		       w->mine, w->theirs);
	}
}

static void test_the_approximations(void)
{
	struct worst w;
	size_t i;
	int j;
	double x;

	printf("the twelve approximations, and the worst error in each\n");

	/* --- exp --- */
	memset(&w, 0, sizeof(w));
	for (i = 0; i < 200000; i++) {
		x = -745.0 + (double)(long)i * 0.00727;
		consider(&w, recon_exp(x), exp(x), x, 0.0 / 0.0);
	}
	for (i = 0; i < SPECIAL_COUNT; i++) {
		consider(&w, recon_exp(SPECIALS[i]), exp(SPECIALS[i]),
			 SPECIALS[i], 0.0 / 0.0);
	}
	report("exp", &w, 2);		/* measured 1 */

	/* --- log --- */
	memset(&w, 0, sizeof(w));
	for (i = 1; i < 200000; i++) {
		/* Log-uniform, so the sweep spends as much of itself on 1e-300
		 * as on 1e300 -- a linear sweep would test one exponent. */
		x = exp(-700.0 + (double)(long)i * 0.0070);
		consider(&w, recon_log(x), log(x), x, 0.0 / 0.0);
	}
	for (i = 0; i < SPECIAL_COUNT; i++) {
		consider(&w, recon_log(SPECIALS[i]), log(SPECIALS[i]),
			 SPECIALS[i], 0.0 / 0.0);
	}
	/* And right around 1, where log is small and the relative error of
	 * anything computed as a difference blows up. */
	for (i = 0; i < 20000; i++) {
		x = 1.0 + ((double)(long)i - 10000.0) * 1e-9;
		consider(&w, recon_log(x), log(x), x, 0.0 / 0.0);
	}
	report("log", &w, 4);		/* measured 3 */

	/* --- log10 --- */
	memset(&w, 0, sizeof(w));
	for (i = 1; i < 100000; i++) {
		x = exp(-700.0 + (double)(long)i * 0.014);
		consider(&w, recon_log10(x), log10(x), x, 0.0 / 0.0);
	}
	for (j = -300; j <= 300; j++) {
		/* The exact powers of ten, which is what anybody actually
		 * asks log10 about and where a wrong answer is most visible:
		 * log10(1000) reading 2.9999999999999996 is the classic. */
		x = pow(10.0, (double)j);
		consider(&w, recon_log10(x), log10(x), x, 0.0 / 0.0);
	}
	report("log10", &w, 2);		/* measured 2 */

	/* --- pow --- */
	memset(&w, 0, sizeof(w));
	for (i = 0; i < 400; i++) {
		double base = 1e-8 * pow(10.0, (double)(i % 40) * 0.4)
			+ (double)(i % 17);

		for (j = -60; j <= 60; j++) {
			double e = (double)j * 0.37;

			consider(&w, recon_pow(base, e), pow(base, e), base, e);
			consider(&w, recon_pow(base, (double)j),
				 pow(base, (double)j), base, (double)j);
		}
	}
	report("pow", &w, 20);		/* measured 17 */

	/* --- sin, cos, tan on an ordinary argument --- */
	{
		struct worst ws;
		struct worst wc;
		struct worst wt;

		memset(&ws, 0, sizeof(ws));
		memset(&wc, 0, sizeof(wc));
		memset(&wt, 0, sizeof(wt));
		for (i = 0; i < 300000; i++) {
			x = ((double)(long)i - 150000.0) * 0.0001379;
			consider(&ws, recon_sin(x), sin(x), x, 0.0 / 0.0);
			consider(&wc, recon_cos(x), cos(x), x, 0.0 / 0.0);
			consider(&wt, recon_tan(x), tan(x), x, 0.0 / 0.0);
		}
		report("sin", &ws, 2);	/* measured 2 */
		report("cos", &wc, 2);	/* measured 2 */
		/*
		 * tan gets a looser bound than its two parts, and the reason
		 * is arithmetic rather than laziness: it is sin/cos, so it
		 * inherits both errors and then divides by a number that goes
		 * to zero four times around the circle. Near those points the
		 * relative error of the quotient is the relative error of the
		 * divisor, magnified.
		 */
		report("tan", &wt, 4);	/* measured 4 */
	}

	/* --- asin, acos, atan --- */
	{
		struct worst wa;
		struct worst wb;
		struct worst wc;

		memset(&wa, 0, sizeof(wa));
		memset(&wb, 0, sizeof(wb));
		memset(&wc, 0, sizeof(wc));
		for (i = 0; i <= 200000; i++) {
			x = -1.0 + (double)(long)i * 1e-5;
			if (x > 1.0) {
				x = 1.0;
			}
			consider(&wa, recon_asin(x), asin(x), x, 0.0 / 0.0);
			consider(&wb, recon_acos(x), acos(x), x, 0.0 / 0.0);
		}
		for (i = 0; i < 200000; i++) {
			x = ((double)(long)i - 100000.0) * 0.00317;
			consider(&wc, recon_atan(x), atan(x), x, 0.0 / 0.0);
		}
		for (i = 0; i < SPECIAL_COUNT; i++) {
			consider(&wc, recon_atan(SPECIALS[i]),
				 atan(SPECIALS[i]), SPECIALS[i], 0.0 / 0.0);
		}
		report("asin", &wa, 4);	/* measured 3 */
		report("acos", &wb, 4);	/* measured 3 */
		report("atan", &wc, 2);	/* measured 2 */
	}

	/* --- atan2 --- */
	memset(&w, 0, sizeof(w));
	for (i = 0; i < SPECIAL_COUNT; i++) {
		size_t k;

		for (k = 0; k < SPECIAL_COUNT; k++) {
			double a = SPECIALS[i];
			double b = SPECIALS[k];

			consider(&w, recon_atan2(a, b), atan2(a, b), a, b);
		}
	}
	for (i = 0; i < 100000; i++) {
		double a = ((double)(long)i - 50000.0) * 0.013;
		double b = ((double)(long)(i * 7 % 99991) - 50000.0) * 0.011;

		consider(&w, recon_atan2(a, b), atan2(a, b), a, b);
	}
	report("atan2", &w, 4);		/* measured 3 */

	/* --- cbrt --- */
	memset(&w, 0, sizeof(w));
	for (i = 1; i < 200000; i++) {
		x = exp(-700.0 + (double)(long)i * 0.0070) *
			((i % 2) ? 1.0 : -1.0);
		consider(&w, recon_cbrt(x), cbrt(x), x, 0.0 / 0.0);
	}
	for (i = 0; i < SPECIAL_COUNT; i++) {
		consider(&w, recon_cbrt(SPECIALS[i]), cbrt(SPECIALS[i]),
			 SPECIALS[i], 0.0 / 0.0);
	}
	report("cbrt", &w, 4);		/* measured 4 */
}

/*
 * The special cases that have an exactly right answer even though the
 * function around them does not.
 *
 * `pow(0, 0)` is 1. `pow(-1, inf)` is 1. `atan2(-0.0, -1.0)` is -pi, and
 * `atan2(0.0, -1.0)` is +pi -- told apart by the sign of a zero, which is a
 * thing most people do not believe until they see it. None of these is a
 * tolerance question, so none of them is held to one.
 */
static void test_the_cases_that_are_exact_anyway(void)
{
	printf("the special cases, which are exact even where the function is"
	       " not\n");

	check(bits_of(recon_pow(0.0, 0.0)) == bits_of(1.0), "pow(0, 0) is 1");
	check(bits_of(recon_pow(NAN, 0.0)) == bits_of(1.0),
	      "pow(nan, 0) is 1, which is required and surprising");
	check(bits_of(recon_pow(1.0, NAN)) == bits_of(1.0), "pow(1, nan) is 1");
	check(bits_of(recon_pow(-1.0, INFINITY)) == bits_of(1.0),
	      "pow(-1, inf) is 1");
	check(bits_of(recon_pow(-1.0, -INFINITY)) == bits_of(1.0),
	      "pow(-1, -inf) is 1");
	check(bits_of(recon_pow(-0.0, 3.0)) == bits_of(-0.0),
	      "a negative zero cubed keeps its sign");
	check(bits_of(recon_pow(-0.0, 2.0)) == bits_of(0.0),
	      "and squared loses it");
	check(recon_pow(-0.0, -3.0) == -INFINITY,
	      "a negative zero to a negative odd power is a negative infinity");
	check(recon_math_isnan(recon_pow(-8.0, 0.5)),
	      "a negative base to a fractional power is not a real number");
	check(recon_pow(-8.0, 3.0) == -512.0,
	      "but an integer power of one is fine");

	/* Every one of these is also checked against the host above; they are
	 * repeated here because a reader of this file should be able to see
	 * what the answers are without running it. */
	check(bits_of(recon_atan2(0.0, -1.0)) == bits_of(atan2(0.0, -1.0)),
	      "atan2(+0, -1) is +pi");
	check(bits_of(recon_atan2(-0.0, -1.0)) == bits_of(atan2(-0.0, -1.0)),
	      "atan2(-0, -1) is -pi, told apart by the sign of a zero");
	check(bits_of(recon_atan2(0.0, 1.0)) == bits_of(atan2(0.0, 1.0)),
	      "atan2(+0, +1) is +0");
	check(bits_of(recon_atan2(-0.0, 1.0)) == bits_of(atan2(-0.0, 1.0)),
	      "atan2(-0, +1) is -0");

	check(bits_of(recon_sqrt(-0.0)) == bits_of(-0.0),
	      "the square root of a negative zero is a negative zero");
	check(recon_math_isnan(recon_sqrt(-1.0)),
	      "and of a negative number is not a number");

	check(recon_math_isnan(recon_sin(INFINITY)),
	      "an infinity is not at any point on the circle");
	check(recon_math_isnan(recon_asin(2.0)), "asin is only defined to 1");
	check(recon_math_isnan(recon_acos(-2.0)), "and so is acos");

	check(recon_log(0.0) == -INFINITY, "log(0) runs to negative infinity");
	check(recon_math_isnan(recon_log(-1.0)),
	      "and log of a negative number is not a real");
	check(recon_exp(1000.0) == INFINITY, "exp overflows to infinity");
	check(recon_exp(-1000.0) == 0.0, "and underflows to zero");
}

/*
 * How far out on the line sin and cos stay usable.
 *
 * Reducing an argument modulo pi/2 costs one bit of the remainder for every
 * bit of the quotient, so this gets worse the further out it goes and there
 * is a point where it stops being an answer. That point is **measured** here
 * rather than asserted, and it is printed, because it is a real limit of this
 * library and somebody should be able to see where it is without reading the
 * source.
 */
static void test_how_far_the_circle_reaches(void)
{
	static const double FAR[] = {
		1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e10, 1e12, 1e15, 1e18,
		1e30, 1e100, 1e300,
	};
	size_t i;

	printf("how far out sin and cos stay usable, measured\n");

	for (i = 0; i < sizeof(FAR) / sizeof(FAR[0]); i++) {
		struct worst w;
		int j;

		memset(&w, 0, sizeof(w));
		for (j = 0; j < 2000; j++) {
			double x = FAR[i] + (double)j * FAR[i] * 1e-13;

			consider(&w, recon_sin(x), sin(x), x, 0.0 / 0.0);
			consider(&w, recon_cos(x), cos(x), x, 0.0 / 0.0);
		}
		printf("    around %-8.0e worst %lld ulp\n", FAR[i], w.ulp);
	}
}

int main(void)
{
	printf("ReconOS C library: the maths functions\n\n");

	test_the_exact_ones();
	test_the_approximations();
	test_the_cases_that_are_exact_anyway();
	test_how_far_the_circle_reaches();

	printf("\n%ld checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
