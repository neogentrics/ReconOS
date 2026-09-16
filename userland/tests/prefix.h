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

/* --- the descriptor layer --------------------------------------------------
 *
 * `recon_libc_` rather than `recon_`, because `recon_open`, `recon_read`,
 * `recon_write` and `recon_close` are already taken: they are the system-call
 * inlines in `userland/include/recon.h`, one layer below these.
 * `scripts/measure-libc.py` strips both prefixes, so the coverage figure still
 * reads these as the POSIX names they are.
 */
/* --- sockets ---
 *
 * `recon_libc_`, not `recon_`, and this is the sharpest case of that rule in
 * the file: `recon_socket`, `recon_bind`, `recon_listen`, `recon_accept` and
 * `recon_connect` are the *system calls*, sitting in `recon.h`. Renaming the
 * library's `socket` to `recon_socket` would not collide -- it would
 * **replace**, silently, and a program asking for a stream would be passing 2
 * as a domain to a call that takes a type.
 */
#define socket      recon_libc_socket
#define bind        recon_libc_bind
#define listen      recon_libc_listen
#define accept      recon_libc_accept
#define connect     recon_libc_connect
#define send        recon_libc_send
#define recv        recon_libc_recv
#define setsockopt  recon_libc_setsockopt
#define getsockopt  recon_libc_getsockopt

#define open        recon_libc_open
#define close       recon_libc_close
#define read        recon_libc_read
#define write       recon_libc_write
#define lseek       recon_libc_lseek
#define mkdir       recon_libc_mkdir
#define opendir     recon_libc_opendir
#define readdir     recon_libc_readdir
#define closedir    recon_libc_closedir
#define realpath    recon_libc_realpath
#define sysconf     recon_libc_sysconf

#define htons        recon_htons
#define htonl        recon_htonl
#define ntohs        recon_ntohs
#define ntohl        recon_ntohl
#define inet_pton    recon_inet_pton
#define inet_ntoa    recon_inet_ntoa
#define gai_strerror recon_gai_strerror

#define feof        recon_feof
#define ferror      recon_ferror
#define clearerr    recon_clearerr
#define ungetc      recon_ungetc

#define sscanf      recon_sscanf
#define vsscanf     recon_vsscanf

#define errno       recon_errno
#define strerror    recon_strerror

#define malloc      recon_malloc
#define calloc      recon_calloc
#define realloc     recon_realloc
#define free        recon_free
#define strdup      recon_strdup

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
#define memmem      recon_memmem
#define strcasestr  recon_strcasestr
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
#define strtoll     recon_strtoll
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
#define puts        recon_puts

/*
 * time() is renamed to recon_libc_time rather than recon_time, because
 * recon_time already exists: it is the SYS_TIME inline in recon.h, and
 * syscalls.c calls it. Same reason exit() became recon_libc_exit.
 */
#define time          recon_libc_time
#define clock_gettime recon_clock_gettime
#define difftime      recon_difftime
#define gmtime_r      recon_gmtime_r
#define localtime_r   recon_localtime_r
#define strftime      recon_strftime

/*
 * The maths functions. `recon_math_isnan` and its three companions are not
 * renamed and do not need to be: they are already ours, and the classifiers
 * the caller writes -- isnan, isinf, isfinite, signbit -- are macros in both
 * libraries rather than symbols, so there is nothing for a linker to confuse.
 */
#define fabs        recon_fabs
#define sqrt        recon_sqrt
#define sqrtf       recon_sqrtf
#define sincos      recon_sincos
#define floor       recon_floor
#define ceil        recon_ceil
#define round       recon_round
#define fmod        recon_fmod
#define ldexp       recon_ldexp
#define lrintf      recon_lrintf
#define exp         recon_exp
#define log         recon_log
#define log10       recon_log10
#define pow         recon_pow
#define sin         recon_sin
#define cos         recon_cos
#define tan         recon_tan
#define asin        recon_asin
#define acos        recon_acos
#define atan        recon_atan
#define atan2       recon_atan2
#define cbrt        recon_cbrt

#endif /* RECON_LIBC_PREFIX_H */
