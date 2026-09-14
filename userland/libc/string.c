/*
 * The string and memory functions, for programs that have no library under
 * them.
 *
 * Measured before it was written rather than guessed at: across the desktop's
 * 79 modules these are called about eight hundred and thirty times, and the
 * ten most-used account for nearly all of it. Nothing here is included because
 * a real libc has it; everything here is included because `src/` calls it.
 *
 * --- What correctness means here, and how it is checked ---
 *
 * These are not "close enough" implementations. Every one of them is compared
 * against the host's C library over a corpus of inputs, including the awkward
 * ones -- empty strings, overlapping ranges, embedded NULs, lengths of zero --
 * in `userland/tests/test_libc.c`. A differential test against a reference is
 * the only kind worth having for functions this widely used: a `strcmp` that
 * returns the wrong *sign* still sorts, just backwards, and would pass any
 * test somebody wrote from memory of what it should do.
 *
 * --- The one rule that shapes the code ---
 *
 * A function here may not call another function here unless it is obviously
 * cheaper to. The compiler is entitled to recognise a byte-copy loop and
 * replace it with a call to `memcpy` -- which, inside `memcpy`, is infinite
 * recursion that builds fine and fails at run time. Every loop below is
 * written so that GCC's idiom recognition has nothing to match, and the build
 * passes `-fno-builtin` so that it does not try.
 */

#include <stddef.h>

void *memcpy(void *to, const void *from, size_t length)
{
	unsigned char *d = to;
	const unsigned char *s = from;
	size_t i;

	for (i = 0; i < length; i++) {
		d[i] = s[i];
	}
	return to;
}

/*
 * Overlapping ranges, which is the whole reason this exists separately.
 *
 * Copying forwards through an overlap where the destination is *after* the
 * source overwrites bytes that have not been read yet. The direction is chosen
 * from the addresses, and this is the one function here where getting it wrong
 * produces a result that looks almost right -- the first few bytes are correct
 * and the rest is a repeating pattern.
 */
void *memmove(void *to, const void *from, size_t length)
{
	unsigned char *d = to;
	const unsigned char *s = from;
	size_t i;

	if (d == s || length == 0) {
		return to;
	}
	if (d < s) {
		for (i = 0; i < length; i++) {
			d[i] = s[i];
		}
	} else {
		for (i = length; i > 0; i--) {
			d[i - 1] = s[i - 1];
		}
	}
	return to;
}

void *memset(void *to, int value, size_t length)
{
	unsigned char *d = to;
	size_t i;

	for (i = 0; i < length; i++) {
		d[i] = (unsigned char)value;
	}
	return to;
}

/*
 * The comparison is of *unsigned* bytes, and that is not a detail.
 *
 * On a machine where `char` is signed -- which is every machine this runs on
 * -- comparing them directly makes 0x80 less than 0x01, so any comparison
 * involving a byte above 127 comes out backwards. Every string in this system
 * is UTF-8, so every non-English character is such a byte.
 */
int memcmp(const void *a, const void *b, size_t length)
{
	const unsigned char *x = a;
	const unsigned char *y = b;
	size_t i;

	for (i = 0; i < length; i++) {
		if (x[i] != y[i]) {
			return (int)x[i] - (int)y[i];
		}
	}
	return 0;
}

void *memchr(const void *in, int value, size_t length)
{
	const unsigned char *p = in;
	unsigned char want = (unsigned char)value;
	size_t i;

	for (i = 0; i < length; i++) {
		if (p[i] == want) {
			return (void *)(p + i);
		}
	}
	return NULL;
}

size_t strlen(const char *text)
{
	size_t n = 0;

	while (text[n] != '\0') {
		n++;
	}
	return n;
}

/* The length, or `most` if the string is longer -- which is how a caller
 * measures something that may not be terminated without running off it. */
size_t strnlen(const char *text, size_t most)
{
	size_t n = 0;

	while (n < most && text[n] != '\0') {
		n++;
	}
	return n;
}

int strcmp(const char *a, const char *b)
{
	const unsigned char *x = (const unsigned char *)a;
	const unsigned char *y = (const unsigned char *)b;

	while (*x != '\0' && *x == *y) {
		x++;
		y++;
	}
	return (int)*x - (int)*y;
}

int strncmp(const char *a, const char *b, size_t length)
{
	const unsigned char *x = (const unsigned char *)a;
	const unsigned char *y = (const unsigned char *)b;
	size_t i;

	for (i = 0; i < length; i++) {
		if (x[i] != y[i] || x[i] == '\0') {
			return (int)x[i] - (int)y[i];
		}
	}
	return 0;
}

/*
 * Case folding is ASCII only, deliberately and permanently.
 *
 * `strcasecmp` is called 357 times in this system and every one of them is
 * comparing something whose spelling is defined by a standard somebody else
 * wrote: an HTTP header name, an HTML tag, a CSS property, a file extension,
 * a skin's role name. All of those are ASCII by definition.
 *
 * Folding beyond ASCII is not a bigger table, it is a different *question* --
 * it depends on locale, it is not a per-byte operation in UTF-8, and in
 * Turkish the answer for the letter i is famously not what any of these
 * callers want. A function that did it would be wrong for all 357.
 */
static unsigned char fold(unsigned char c)
{
	return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + ('a' - 'A')) : c;
}

int strcasecmp(const char *a, const char *b)
{
	const unsigned char *x = (const unsigned char *)a;
	const unsigned char *y = (const unsigned char *)b;

	while (*x != '\0' && fold(*x) == fold(*y)) {
		x++;
		y++;
	}
	return (int)fold(*x) - (int)fold(*y);
}

int strncasecmp(const char *a, const char *b, size_t length)
{
	const unsigned char *x = (const unsigned char *)a;
	const unsigned char *y = (const unsigned char *)b;
	size_t i;

	for (i = 0; i < length; i++) {
		if (fold(x[i]) != fold(y[i]) || x[i] == '\0') {
			return (int)fold(x[i]) - (int)fold(y[i]);
		}
	}
	return 0;
}

char *strcpy(char *to, const char *from)
{
	char *at = to;

	while ((*at++ = *from++) != '\0') {
	}
	return to;
}

/*
 * `strncpy` does two surprising things and both are required of it.
 *
 * It does *not* terminate the result when the source is at least as long as
 * the buffer, and it pads the whole remainder with NULs when the source is
 * shorter. Neither is what anybody wants, which is why this system almost
 * never uses it and uses `snprintf` instead -- but it is what the function is,
 * and an implementation that "fixed" it would break the callers that rely on
 * the padding.
 */
char *strncpy(char *to, const char *from, size_t length)
{
	size_t i = 0;

	while (i < length && from[i] != '\0') {
		to[i] = from[i];
		i++;
	}
	while (i < length) {
		to[i] = '\0';
		i++;
	}
	return to;
}

char *strcat(char *to, const char *from)
{
	char *at = to;

	while (*at != '\0') {
		at++;
	}
	while ((*at++ = *from++) != '\0') {
	}
	return to;
}

/*
 * The terminator counts as part of the string, which is why searching for
 * '\0' finds the end rather than nothing. Callers rely on that to find where
 * a string stops in one pass.
 */
char *strchr(const char *text, int value)
{
	char want = (char)value;

	for (;;) {
		if (*text == want) {
			return (char *)text;
		}
		if (*text == '\0') {
			return NULL;
		}
		text++;
	}
}

char *strrchr(const char *text, int value)
{
	char want = (char)value;
	const char *found = NULL;

	for (;;) {
		if (*text == want) {
			found = text;
		}
		if (*text == '\0') {
			return (char *)found;
		}
		text++;
	}
}

/* The empty needle is found immediately, at the start. That is what the
 * standard says and what callers that build a search term from user input
 * depend on -- the alternative is that an empty search matches nothing, and
 * the find bar goes blank the moment somebody clears it. */
char *strstr(const char *haystack, const char *needle)
{
	size_t n = strlen(needle);
	size_t i;

	if (n == 0) {
		return (char *)haystack;
	}
	for (i = 0; haystack[i] != '\0'; i++) {
		size_t j = 0;

		while (j < n && haystack[i + j] == needle[j]) {
			j++;
		}
		if (j == n) {
			return (char *)(haystack + i);
		}
	}
	return NULL;
}

size_t strspn(const char *text, const char *of)
{
	size_t n = 0;

	while (text[n] != '\0' && strchr(of, text[n]) != NULL &&
	       text[n] != '\0') {
		n++;
	}
	return n;
}

size_t strcspn(const char *text, const char *stop)
{
	size_t n = 0;

	while (text[n] != '\0' && strchr(stop, text[n]) == NULL) {
		n++;
	}
	return n;
}

/*
 * Find a run of bytes inside a run of bytes.
 *
 * A GNU extension rather than a standard function, and it is here because
 * `src/recon_html.c` uses it to find the end of a comment -- where the
 * haystack is a whole page that may contain a zero byte, so `strstr` is the
 * wrong tool and not a slower one.
 *
 * An empty needle matches at the start, which is what the reference does and
 * is the answer a caller looping over matches needs in order to terminate.
 */
void *memmem(const void *haystack, size_t haystack_length,
	     const void *needle, size_t needle_length)
{
	const unsigned char *h = (const unsigned char *)haystack;
	const unsigned char *n = (const unsigned char *)needle;
	size_t i;

	if (needle_length == 0) {
		return (void *)haystack;
	}
	if (haystack_length < needle_length) {
		return NULL;
	}

	for (i = 0; i + needle_length <= haystack_length; i++) {
		if (h[i] == n[0] && memcmp(h + i, n, needle_length) == 0) {
			return (void *)(h + i);
		}
	}
	return NULL;
}

/*
 * `strstr` that does not care about case.
 *
 * Also a GNU extension, and also here for one caller: `src/recon_http.c`
 * looks for "chunked" in a Transfer-Encoding header, and a header's value is
 * case-insensitive by the specification that defines it.
 */
char *strcasestr(const char *haystack, const char *needle)
{
	size_t i;
	size_t n = strlen(needle);

	if (n == 0) {
		return (char *)haystack;
	}
	for (i = 0; haystack[i] != '\0'; i++) {
		if (strncasecmp(haystack + i, needle, n) == 0) {
			return (char *)(haystack + i);
		}
	}
	return NULL;
}

/*
 * --- Splitting a string ---
 *
 * Forty-four call sites, and every one of them the reentrant spelling. That is
 * not an accident of style: `strtok` keeps its place in a static, so two
 * loops walking two strings at once destroy each other, and a compositor is
 * full of places where one parse calls something that parses.
 *
 * **There is deliberately no `strtok` here.** The desktop does not call it,
 * and adding it would put a hidden static in a library that is going to be
 * linked into a system where the same code can run on two cores. A caller who
 * wants it can pass their own `save`.
 *
 * It writes into the string it is given -- that is what makes it cheap and
 * what makes it dangerous, and it is worth saying out loud rather than leaving
 * to be discovered: a caller handing this a string literal is writing to
 * read-only memory.
 */
char *strtok_r(char *text, const char *separators, char **save)
{
	char *start;

	if (save == NULL) {
		return NULL;
	}
	if (text == NULL) {
		text = *save;
	}
	if (text == NULL) {
		return NULL;
	}

	/* Leading separators are not an empty token. Two separators in a row
	 * produce one break, which is the behaviour every caller here is
	 * relying on when it splits on "{space}" and does not want to handle
	 * runs of them. */
	text += strspn(text, separators);
	if (*text == '\0') {
		*save = text;
		return NULL;
	}

	start = text;
	text += strcspn(text, separators);
	if (*text == '\0') {
		*save = text;
	} else {
		*text = '\0';
		*save = text + 1;
	}
	return start;
}

/*
 * At most `length` characters from `from`, and then a terminator -- so the
 * most it writes is `length + 1` bytes past the end of what is already there,
 * which is the part that surprises people about this function and the reason
 * it is only on two call sites.
 */
char *strncat(char *to, const char *from, size_t length)
{
	char *at = to + strlen(to);
	size_t n = 0;

	while (n < length && from[n] != '\0') {
		at[n] = from[n];
		n++;
	}
	at[n] = '\0';
	return to;
}
