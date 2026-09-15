/*
 * <assert.h>.
 *
 * --- What an assertion does on a machine with no debugger ---
 *
 * On Linux a failed assertion calls `abort`, which raises SIGABRT, which a
 * shell reports and a core dump preserves. ReconOS has none of that yet: there
 * is no shell watching, no core, and on a machine drawing its first screen
 * there may be nobody reading a serial line either.
 *
 * So a failed assertion **says what failed, where, and in which function, and
 * then exits with a code the kernel reads**. The message goes to standard
 * error, which the kernel puts on the serial line and into its own log, so it
 * survives the program ending. That is the most a program can do here, and
 * more than a silent abort would.
 *
 * --- Why this header is guarded oddly ---
 *
 * The declaration is guarded. **The macro is not**, and that is the standard's
 * rule rather than an oversight: `assert` is redefined every time this header
 * is included, according to `NDEBUG` as it stands *at that moment*. A file
 * that includes it, defines `NDEBUG`, and includes it again is entitled to
 * assertions and then none.
 *
 * An ordinary include guard around the whole file would silently break that,
 * which is the sort of thing a header gets wrong once and nobody notices for
 * years -- the second include simply does nothing.
 */

#ifndef RECON_ASSERT_H
#define RECON_ASSERT_H

/*
 * Does not return.
 *
 * Named `recon_libc_assert` rather than `__assert_fail`, because this library
 * does not put its functions in the implementation's reserved namespace -- and
 * because `scripts/measure-libc.py` reads the name back as `assert`, which is
 * what the desktop actually calls.
 */
void recon_libc_assert(const char *condition, const char *file, int line,
		       const char *function);

#endif /* RECON_ASSERT_H */

/* --- outside the guard, on purpose. See above. --- */

#undef assert

#ifdef NDEBUG

#define assert(condition) ((void)0)

#else

#define assert(condition)						\
	((condition) ? (void)0						\
		     : recon_libc_assert(#condition, __FILE__, __LINE__,	\
					 __func__))

#endif /* NDEBUG */
