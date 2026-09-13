#ifndef RECON_KERNEL_COMPILER_H
#define RECON_KERNEL_COMPILER_H

#define RK_NORETURN   __attribute__((noreturn))
#define RK_PACKED     __attribute__((packed))
#define RK_ALIGNED(n) __attribute__((aligned(n)))
#define RK_UNUSED     __attribute__((unused))

/* Kept as a real call, so that a caller of it has a real stack frame.
 *
 * Needed by exactly one kind of code: something whose subject is the call
 * chain itself. The backtrace test calls three functions to make three frames,
 * and at -O2 the compiler inlines all three into one -- so the test measured
 * two frames and reported the walker broken when the walker was fine. */
#define RK_NOINLINE   __attribute__((noinline))
#define RK_PRINTF(f, a) __attribute__((format(printf, f, a)))

#define RK_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* The builtin rather than the cast-a-null-pointer trick, which is
 * undefined behaviour that happens to work and stops working under
 * enough optimisation. */
#define RK_OFFSETOF(t, m) __builtin_offsetof(t, m)

#endif /* RECON_KERNEL_COMPILER_H */
