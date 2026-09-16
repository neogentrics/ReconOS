/*
 * <limits.h>.
 *
 * --- why this exists and why it looks like this ---------------------------
 *
 * Nothing in the desktop includes it directly. Two vendored headers do --
 * `third_party/stb_image.h` and `third_party/minimp3_ex.h` -- and with no
 * system headers underneath, the compiler's own `<limits.h>` chains to
 * `syslimits.h`, which looks for the system's and finds nothing:
 *
 *     limits.h:205: error: no include path in which to search for limits.h
 *
 * That is not the vendored code being awkward. **A C library provides
 * `<limits.h>`**, and this one did not.
 *
 * --- the values come from the compiler ------------------------------------
 *
 * Every limit below is one of the compiler's own predefined macros, not a
 * number typed in. `__INT_MAX__` is what this compiler will actually do with
 * an `int` on this target, so the header cannot disagree with the compiler it
 * is compiled by -- which is the failure mode a hand-written limits.h has, and
 * it is silent: code that checks `INT_MAX` before doing arithmetic gets the
 * wrong answer and carries on.
 *
 * The minimums are written as `(-MAX - 1)` rather than as a literal for the
 * same reason the tree has a comment about `(int)~0U >> 1`: a two's-complement
 * minimum has no positive counterpart, and writing `-2147483648` is an `int`
 * negated rather than a constant, which promotes to `long` and stops being the
 * value it looks like.
 *
 * --- and the ones that are ReconOS's own ----------------------------------
 *
 * `PATH_MAX` and `NAME_MAX` are not the compiler's business. They are this
 * system's, and they are the numbers this system already uses: `PATH_MAX`
 * matches `RECON_PATH_MAX` in `include/recon_fs.h`, and `NAME_MAX` matches the
 * `d_name` in `<dirent.h>`. Two places holding one number is how they come to
 * disagree, so the ones here say where the other copy is.
 */

#ifndef RECON_LIMITS_H
#define RECON_LIMITS_H

/* --- characters --- */

#define CHAR_BIT    __CHAR_BIT__
#define SCHAR_MAX   __SCHAR_MAX__
#define SCHAR_MIN   (-SCHAR_MAX - 1)
#define UCHAR_MAX   (SCHAR_MAX * 2 + 1)

/*
 * Whether a plain `char` is signed is the target's choice, and the compiler
 * knows which. Getting this wrong makes every `char` comparison above 127
 * wrong on one architecture and right on the other, which is the kind of
 * fault that survives every test run on one machine.
 */
#ifdef __CHAR_UNSIGNED__
#define CHAR_MIN    0
#define CHAR_MAX    UCHAR_MAX
#else
#define CHAR_MIN    SCHAR_MIN
#define CHAR_MAX    SCHAR_MAX
#endif

/* One byte. There is no multibyte state in this library. */
#define MB_LEN_MAX  1

/* --- integers --- */

#define SHRT_MAX    __SHRT_MAX__
#define SHRT_MIN    (-SHRT_MAX - 1)
#define USHRT_MAX   (SHRT_MAX * 2 + 1)

#define INT_MAX     __INT_MAX__
#define INT_MIN     (-INT_MAX - 1)
#define UINT_MAX    (INT_MAX * 2U + 1U)

#define LONG_MAX    __LONG_MAX__
#define LONG_MIN    (-LONG_MAX - 1L)
#define ULONG_MAX   (LONG_MAX * 2UL + 1UL)

#define LLONG_MAX   __LONG_LONG_MAX__
#define LLONG_MIN   (-LLONG_MAX - 1LL)
#define ULLONG_MAX  (LLONG_MAX * 2ULL + 1ULL)

/* --- this system's own --- */

/*
 * The longest path, including the terminator.
 *
 * **1024, not Linux's 4096**, and the difference has bitten this project
 * before: `src/recon_fs.c` has a comment about `realpath` requiring a buffer
 * of at least the *host's* PATH_MAX, and an optimised glibc aborting the
 * process when handed a smaller one. The number here is ReconOS's, and it is
 * the same number as `RECON_PATH_MAX` in `include/recon_fs.h` -- which is the
 * copy to change if it ever moves.
 */
#define PATH_MAX    1024

/*
 * The longest single name in a directory, without the terminator.
 *
 * 255 rather than 256, because `<dirent.h>`'s `d_name` holds 256 bytes and one
 * of them is the NUL. Off by one here is a name that fits being reported as
 * one that does not, or worse, a buffer sized from this being one short.
 */
#define NAME_MAX    255

#endif /* RECON_LIMITS_H */
