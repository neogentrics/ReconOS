/*
 * Rename every function the ReconOS libc defines, so it can be compiled into
 * the same program as the host's C library and the two compared directly.
 *
 * This is the whole trick behind `test_libc.c`, and it is worth more than any
 * test written from memory of what these functions should do. A `strcmp` that
 * returns the wrong *sign* still sorts -- backwards -- and would pass a test
 * that checked "is it nonzero". A `snprintf` that rounds `%.2f` the wrong way
 * at a half is wrong in a way nobody writing the test would think to check.
 * Against a reference, both fail immediately.
 *
 * Applied with `-include`, so the sources under `libc/` say `strlen` and know
 * nothing about this.
 */

#ifndef RECON_LIBC_PREFIX_H
#define RECON_LIBC_PREFIX_H

#define memcpy      recon_memcpy
#define memmove     recon_memmove
#define memset      recon_memset
#define memcmp      recon_memcmp
#define memchr      recon_memchr

#define strlen      recon_strlen
#define strnlen     recon_strnlen
#define strcmp      recon_strcmp
#define strncmp     recon_strncmp
#define strcasecmp  recon_strcasecmp
#define strncasecmp recon_strncasecmp
#define strcpy      recon_strcpy
#define strncpy     recon_strncpy
#define strcat      recon_strcat
#define strchr      recon_strchr
#define strrchr     recon_strrchr
#define strstr      recon_strstr
#define strspn      recon_strspn
#define strcspn     recon_strcspn

#define snprintf    recon_snprintf
#define vsnprintf   recon_vsnprintf

#endif /* RECON_LIBC_PREFIX_H */
