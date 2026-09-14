/*
 * Character classification, ASCII only and permanently so.
 *
 * Thirty-seven call sites across the desktop, and every one of them is asking
 * about a byte in something whose grammar is ASCII by definition: a number
 * being parsed, whitespace in a header, a hexadecimal colour, a tag name.
 *
 * --- Why there is no locale, and never will be here ---
 *
 * A real `ctype` answers "is this a letter" differently depending on where the
 * machine thinks it is. That is the wrong question for every caller in this
 * system. ReconOS's text is UTF-8, where a non-ASCII character is *two or more
 * bytes* — so "is this byte a letter" has no useful answer above 127 no matter
 * what table is consulted, and a function that said yes would encourage
 * exactly the per-byte reasoning that breaks on the first accented name.
 *
 * So: bytes above 127 are not letters, not digits, and not spaces. Anything
 * wanting to know about a *character* rather than a byte has to decode first,
 * and nothing in the desktop currently needs to.
 *
 * --- The argument is an int, and that matters ---
 *
 * These take `int` because `EOF` is -1 and has to be passable. A caller that
 * hands over a `char` on a machine where it is signed passes a negative number
 * for every byte above 127, so the range check below tests *both* ends rather
 * than only the top. Getting that wrong is an out-of-bounds table read in a
 * real implementation and a wrong answer in this one.
 */

#include "internal.h"

int isdigit(int c)
{
	return c >= '0' && c <= '9';
}

int isupper(int c)
{
	return c >= 'A' && c <= 'Z';
}

int islower(int c)
{
	return c >= 'a' && c <= 'z';
}

int isalpha(int c)
{
	return isupper(c) || islower(c);
}

int isalnum(int c)
{
	return isalpha(c) || isdigit(c);
}

/*
 * Space, tab, newline, carriage return, vertical tab and form feed — the same
 * six the standard names. The last two are here for completeness rather than
 * because anything sends them; leaving them out would make this subtly
 * different from the reference for no gain.
 */
int isspace(int c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
	       c == '\v' || c == '\f';
}

int isxdigit(int c)
{
	return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int isprint(int c)
{
	return c >= 0x20 && c < 0x7F;
}

int toupper(int c)
{
	return islower(c) ? c - ('a' - 'A') : c;
}

int tolower(int c)
{
	return isupper(c) ? c + ('a' - 'A') : c;
}
