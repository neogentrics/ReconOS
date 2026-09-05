#include <recon/kernel/kstring.h>

size_t kstrlen(const char *s)
{
	const char *p = s;
	while (*p)
		p++;
	return (size_t)(p - s);
}

void *kmemset(void *dst, int c, size_t n)
{
	unsigned char *d = dst;
	while (n--)
		*d++ = (unsigned char)c;
	return dst;
}

void *kmemcpy(void *dst, const void *src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;
	while (n--)
		*d++ = *s++;
	return dst;
}

int kmemcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *x = a, *y = b;
	while (n--) {
		if (*x != *y)
			return (int)*x - (int)*y;
		x++;
		y++;
	}
	return 0;
}

size_t kstrlcpy(char *dst, const char *src, size_t len)
{
	size_t i = 0;

	if (len == 0)
		return 0;
	while (i + 1 < len && src[i]) {
		dst[i] = src[i];
		i++;
	}
	dst[i] = '\0';
	return i;
}

/* GCC emits calls to these for struct assignment and array initialisation
 * even with -ffreestanding, so a freestanding kernel has to provide them
 * under their standard names whether it calls them itself or not. */
void *memset(void *dst, int c, size_t n) { return kmemset(dst, c, n); }
void *memcpy(void *dst, const void *src, size_t n) { return kmemcpy(dst, src, n); }
int   memcmp(const void *a, const void *b, size_t n) { return kmemcmp(a, b, n); }

void *memmove(void *dst, const void *src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;

	if (d == s || n == 0)
		return dst;
	if (d < s) {
		while (n--)
			*d++ = *s++;
	} else {
		d += n;
		s += n;
		while (n--)
			*--d = *--s;
	}
	return dst;
}
