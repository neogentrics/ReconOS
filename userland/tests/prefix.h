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
#define strtok_r    recon_strtok_r
#define strncat     recon_strncat

#define snprintf    recon_snprintf
#define vsnprintf   recon_vsnprintf

#define isalnum     recon_isalnum
#define isalpha     recon_isalpha
#define isdigit     recon_isdigit
#define isspace     recon_isspace
#define isupper     recon_isupper
#define islower     recon_islower
#define isxdigit    recon_isxdigit
#define isprint     recon_isprint
#define toupper     recon_toupper
#define tolower     recon_tolower

#define atoi        recon_atoi
#define atoll       recon_atoll
#define atof        recon_atof
#define strtol      recon_strtol
#define strtoul     recon_strtoul
#define strtoull    recon_strtoull
#define strtod      recon_strtod
#define abs         recon_abs
#define qsort       recon_qsort
#define getenv      recon_getenv
#define exit        recon_libc_exit

#define fopen       recon_fopen
#define fclose      recon_fclose
#define fflush      recon_fflush
#define fread       recon_fread
#define fwrite      recon_fwrite
#define fgets       recon_fgets
#define fgetc       recon_fgetc
#define fseek       recon_fseek
#define ftell       recon_ftell
#define rewind      recon_rewind
#define fprintf     recon_fprintf

#endif /* RECON_LIBC_PREFIX_H */
