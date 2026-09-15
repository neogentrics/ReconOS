/*
 * <math.h>, cut to what the desktop needs.
 *
 * Twenty functions, and they are not a calculator's luxury. `nm` on the
 * desktop's object files says where they are wanted:
 *
 *   `src/recon_expr.c`  seventeen of them, in a **table of function
 *                       pointers** so the calculator can look a name up.
 *                       Not one appears as `sqrt(` in the source.
 *   `src/recon_ui.c`    seven, through `third_party/stb_truetype.h`.
 *   `src/recon_ocr_match.c`  the same seven, the same way.
 *   `src/recon_stb.c`   `pow` and `ldexp`, through `stb_image.h`.
 *   `src/recon_session.c`, `src/recon_shell.c`, `src/recon_codec.c`
 *                       `sin`, `cos`, `atan2`, `lrintf`.
 *
 * The second and third are the ones that matter: that is the font rasteriser.
 * **ReconOS cannot draw a letter without `sqrt`, `floor`, `ceil`, `pow`,
 * `fmod`, `acos` and `cos`.**
 *
 * A call-site grep reported this whole area as four calls, because a function
 * held in a table is never written with a bracket after it and a function
 * reached through a vendored header is never written at all. See BG-185.
 *
 * --- What each function promises ---
 *
 * **Eight are exactly right** -- bit for bit what the host's return, asserted
 * that way in the suite: `fabs`, `floor`, `ceil`, `round`, `fmod`, `ldexp`,
 * `lrintf`, and `sqrt`. The last is exact because IEEE 754 requires square
 * root to be correctly rounded and every processor this runs on has the
 * instruction.
 *
 * **Twelve are approximations**, held to a bound in units in the last place
 * that the suite *measures and prints* rather than merely staying under. The
 * bound each one actually achieves is in `docs/CHANGELOG.md` and is a
 * measurement, not a claim.
 *
 * --- What is absent ---
 *
 * Every `float` variant except `lrintf`, which is the only one anything calls.
 * No `hypot`, `frexp`, `modf`, `nextafter`, `copysign`, `trunc`, `rint`,
 * `sinh`, `cosh`, `tanh`, `erf`, `tgamma` or `lgamma`: nothing in ReconOS
 * references any of them, and a function nobody calls is a function nobody
 * has ever checked.
 */

#ifndef RECON_MATH_H
#define RECON_MATH_H

/*
 * Only pi. The other constants a maths header usually carries -- M_E, M_LN2,
 * M_SQRT2 -- are not referenced anywhere in ReconOS, and `recon_expr.c`
 * defines its own `e` because it needs a name for it in an expression rather
 * than a number in C.
 *
 * Six uses, in `recon_session.c` and `recon_shell.c`, both turning a fraction
 * of a turn into radians.
 */
#define M_PI 3.14159265358979323846

/*
 * The values the functions below *return* on overflow, underflow and a
 * question with no answer. They are here because a caller needs a way to name
 * what came back -- `if (result == HUGE_VAL)` after a `pow` is the ordinary
 * way to notice an overflow, and without these it cannot be written.
 *
 * Built from the compiler's own constants rather than by overflowing an
 * expression, which a compiler is entitled to fold at translation time and
 * warn about while it does.
 */
#define INFINITY  (__builtin_inff())
#define NAN       (__builtin_nanf(""))
#define HUGE_VAL  (__builtin_inf())

/*
 * The classifiers are macros over real functions, which is how the standard
 * has them and is not a detail: `isnan` must work on a `float` and a `double`
 * without the caller saying which, so it cannot be an ordinary function -- and
 * it must not evaluate its argument twice, which is why there is a function
 * underneath rather than an expression.
 */
int recon_math_isnan(double x);
int recon_math_isinf(double x);
int recon_math_isfinite(double x);
int recon_math_signbit(double x);

#define isnan(x)    recon_math_isnan((double)(x))
#define isinf(x)    recon_math_isinf((double)(x))
#define isfinite(x) recon_math_isfinite((double)(x))
#define signbit(x)  recon_math_signbit((double)(x))

/* --- Exactly right --- */

double fabs(double x);
double sqrt(double x);

/* Written by the compiler rather than by anybody here -- see the note above
 * their definitions in `libc/math.c`. Declared so that a source file which
 * *does* name them gets the right prototype rather than an implicit int. */
float sqrtf(float x);
void sincos(double x, double *sine, double *cosine);
double floor(double x);
double ceil(double x);
double round(double x);
double fmod(double x, double y);
double ldexp(double x, int n);
long lrintf(float x);

/* --- Approximations, to a measured bound --- */

double exp(double x);
double log(double x);
double log10(double x);
double pow(double x, double y);

double sin(double x);
double cos(double x);
double tan(double x);

double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);

double cbrt(double x);

#endif /* RECON_MATH_H */
