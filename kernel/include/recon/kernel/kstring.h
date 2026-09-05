#ifndef RECON_KERNEL_KSTRING_H
#define RECON_KERNEL_KSTRING_H

#include <recon/kernel/types.h>

size_t kstrlen(const char *s);
void  *kmemset(void *dst, int c, size_t n);
void  *kmemcpy(void *dst, const void *src, size_t n);
int    kmemcmp(const void *a, const void *b, size_t n);

/* Copies at most len-1 bytes and always terminates. Returns bytes written,
 * not the length of the source, because the caller of a kernel string
 * function should never be handed a number larger than the buffer it owns. */
size_t kstrlcpy(char *dst, const char *src, size_t len);

#endif /* RECON_KERNEL_KSTRING_H */
