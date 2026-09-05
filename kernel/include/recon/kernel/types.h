/* Fixed-width types for the kernel.
 *
 * The kernel is freestanding: there is no libc, so <stdint.h> from the host
 * toolchain is the only header borrowed, because GCC ships a freestanding
 * copy of it that describes the *target*, not the host. Everything else is
 * written here.
 */
#ifndef RECON_KERNEL_TYPES_H
#define RECON_KERNEL_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t  i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

/* An address in the physical address space, as the CPU sees RAM. */
typedef uint64_t paddr_t;
/* An address in the kernel's virtual address space. */
typedef uintptr_t vaddr_t;

#endif /* RECON_KERNEL_TYPES_H */
