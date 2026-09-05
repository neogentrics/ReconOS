#ifndef RECON_KERNEL_COMPILER_H
#define RECON_KERNEL_COMPILER_H

#define RK_NORETURN   __attribute__((noreturn))
#define RK_PACKED     __attribute__((packed))
#define RK_ALIGNED(n) __attribute__((aligned(n)))
#define RK_UNUSED     __attribute__((unused))
#define RK_PRINTF(f, a) __attribute__((format(printf, f, a)))

#define RK_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#endif /* RECON_KERNEL_COMPILER_H */
