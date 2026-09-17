/*
 * A guard, and every way of getting past one that is not the token.
 *
 * Pure: bytes in, a yes or a no out. So every header a client can send is
 * driven here directly, including the ones no client would send on purpose.
 *
 * **The case this exists for** is the guard that is not armed. A server whose
 * randomness failed has no secret; if the check then falls open, every
 * document and every log says the writes are guarded and they are not. That
 * is worse than having built nothing, because nothing does not lie.
 *
 * **Watched failing first**, against a guard written the obvious way --
 * `strcmp` on the token, no scheme check, and an unarmed guard treated as "no
 * guard configured" and allowed through. **9 of 33 checks failed**, and the
 * three about an unarmed guard are among them.
 *
 * One near-miss is worth recording. The naive version, handed too few random
 * bytes, padded them with zeroes and armed anyway -- and the check that a
 * look-alike header is refused still *passed*, because a padded token is a
 * different token and this particular header did not match it. The fault was
 * caught only by checking the arming itself. **A weak secret refuses the wrong
 * password exactly as convincingly as a strong one does.**
 *
 * What is deliberately *not* here is a timing measurement. `auth.c` compares
 * in constant time and says why, but a suite that tried to prove it by the
 * clock would be measuring this host's scheduler, and would pass or fail for
 * reasons that have nothing to do with the code. The constant-time property is
 * argued in the source and checked by reading it, which is the honest status
 * and is said out loud rather than dressed up as a check.
 */

#include "../auth.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

static void ok(int cond, const char *what)
{
	checks++;
	if (!cond) {
		failures++;
		printf("  FAIL  %s\n", what);
	}
}

/* Sixteen bytes that are not all the same, so a bug that reads one byte and
 * repeats it cannot pass. */
static const unsigned char BYTES[AUTH_TOKEN_BYTES] = {
	0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
	0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF
};

int main(void)
{
	struct auth a;
	char header[128];

	printf("a guard, and every way of getting past one that is not the token\n");

	/* --- arming ------------------------------------------------------------ */
	ok(auth_arm(&a, BYTES, sizeof(BYTES)) == 1, "sixteen bytes arms it");
	ok(a.armed == 1, "and it says so");
	ok(strcmp(a.token, "00112233445566778899aabbccddeeff") == 0,
	   "rendered as hex, low case, every byte in order");
	ok(strlen(a.token) == AUTH_TOKEN_CHARS, "thirty-two characters");

	/* --- the token, accepted ------------------------------------------------ */
	snprintf(header, sizeof(header), "Bearer %s", a.token);
	ok(auth_ok(&a, header) == 1, "the right token, sent properly");

	snprintf(header, sizeof(header), "bearer %s", a.token);
	ok(auth_ok(&a, header) == 1, "the scheme is case-insensitive -- RFC 7235");

	snprintf(header, sizeof(header), "BEARER %s", a.token);
	ok(auth_ok(&a, header) == 1, "however it is spelled");

	snprintf(header, sizeof(header), "  Bearer   %s", a.token);
	ok(auth_ok(&a, header) == 1, "spaces around the scheme are allowed");

	snprintf(header, sizeof(header), "Bearer\t%s", a.token);
	ok(auth_ok(&a, header) == 1, "a tab separates as well as a space");

	/* --- the token, refused -------------------------------------------------
	 *
	 * The case the file exists for goes first. */
	{
		struct auth unarmed;

		memset(&unarmed, 0, sizeof(unarmed));
		snprintf(header, sizeof(header), "Bearer %s", a.token);

		ok(auth_ok(&unarmed, header) == 0,
		   "an unarmed guard refuses even a token that looks right");
		ok(auth_ok(&unarmed, "") == 0, "and refuses an empty header");
		ok(auth_ok(&unarmed, 0) == 0, "and refuses no header");

		/* Too few bytes is what a failed SYS_RANDOM looks like. It
		 * must not produce a short token; it must produce no guard. */
		ok(auth_arm(&unarmed, BYTES, AUTH_TOKEN_BYTES - 1) == 0,
		   "too few random bytes does not arm it");
		ok(unarmed.armed == 0, "and leaves it closed, not half-open");
		ok(auth_ok(&unarmed, header) == 0, "so nothing gets through");

		ok(auth_arm(&unarmed, 0, AUTH_TOKEN_BYTES) == 0,
		   "no bytes at all does not arm it either");
		ok(auth_arm(0, BYTES, sizeof(BYTES)) == 0, "nor does no guard");
	}

	ok(auth_ok(&a, 0) == 0, "no Authorization header");
	ok(auth_ok(&a, "") == 0, "an empty one");
	ok(auth_ok(&a, "Bearer") == 0, "the scheme with no token");
	ok(auth_ok(&a, "Bearer ") == 0, "the scheme with an empty token");

	/* No separator. Accepting this means this server splits the value
	 * where another reader would not. */
	snprintf(header, sizeof(header), "Bearer%s", a.token);
	ok(auth_ok(&a, header) == 0, "no space between scheme and token");

	/* A different scheme carrying the right bytes. */
	snprintf(header, sizeof(header), "Basic %s", a.token);
	ok(auth_ok(&a, header) == 0, "the right token under the wrong scheme");
	snprintf(header, sizeof(header), "Token %s", a.token);
	ok(auth_ok(&a, header) == 0, "or under no scheme anyone agreed on");

	/* The token alone, which is what somebody will try first. */
	ok(auth_ok(&a, a.token) == 0, "the bare token with no scheme");

	/* --- near misses ---------------------------------------------------------
	 *
	 * Each is a token that a comparison stopping at the first difference,
	 * or one checking only a prefix, would let through. */
	ok(auth_ok(&a, "Bearer 00112233445566778899aabbccddeef0") == 0,
	   "the last character wrong");
	ok(auth_ok(&a, "Bearer 10112233445566778899aabbccddeeff") == 0,
	   "the first character wrong");
	ok(auth_ok(&a, "Bearer 00112233445566778899aabbccddeef") == 0,
	   "one character short -- a prefix of the real one");
	ok(auth_ok(&a, "Bearer 00112233445566778899aabbccddeeffa") == 0,
	   "one character long -- the real one with something after it");
	ok(auth_ok(&a, "Bearer 00112233445566778899AABBCCDDEEFF") == 0,
	   "the right bytes in upper case -- the scheme folds, the token does not");
	ok(auth_ok(&a, "Bearer 0") == 0, "a single character");
	ok(auth_ok(&a, "Bearer 000000000000000000000000000000000000") == 0,
	   "far too long");

	/* A token with a NUL in the middle. The bytes after it are a different
	 * value that this must not silently ignore. */
	{
		char sneaky[64];
		size_t n;

		n = (size_t)snprintf(sneaky, sizeof(sneaky), "Bearer %s", a.token);
		sneaky[n - 4] = '\0';
		ok(auth_ok(&a, sneaky) == 0,
		   "a token cut short by a NUL is not the token");
	}

	/* --- the same decision, from a form field ------------------------------
	 *
	 * A browser form cannot send a header, and a console a browser cannot
	 * use is not a console. The dashboard's rename form answered 401 to
	 * every submission for three versions because of exactly that, and
	 * nobody noticed: the endpoint was tested with `curl -H` and the page
	 * it sits on was never submitted.
	 *
	 * A query string stays refused -- it is logged, sent onward in
	 * `Referer`, and kept in history. A POST body is none of those. */
	{
		char form[128];
		int n;

		n = snprintf(form, sizeof(form), "token=%s", a.token);
		ok(n > 0 && auth_ok_form(&a, form, (size_t)n) == 1,
		   "the right token in a form field");

		n = snprintf(form, sizeof(form), "name=M17&token=%s", a.token);
		ok(n > 0 && auth_ok_form(&a, form, (size_t)n) == 1,
		   "and beside the field the form is actually for");

		n = snprintf(form, sizeof(form), "token=%s&name=M17", a.token);
		ok(n > 0 && auth_ok_form(&a, form, (size_t)n) == 1,
		   "in either order");

		ok(auth_ok_form(&a, "token=wrong", 11) == 0, "a wrong token");
		ok(auth_ok_form(&a, "name=M17", 8) == 0, "no token field");
		ok(auth_ok_form(&a, "", 0) == 0, "an empty body");
		ok(auth_ok_form(&a, 0, 10) == 0, "no body");
		ok(auth_ok_form(&a, "token=", 6) == 0, "an empty token");

		/* Given twice, `http_form_get` answers NULL -- see `form.h`.
		 * A body carrying two tokens is one two readers could
		 * disagree about. */
		n = snprintf(form, sizeof(form), "token=%s&token=%s",
		             a.token, a.token);
		ok(n > 0 && auth_ok_form(&a, form, (size_t)n) == 0,
		   "the right token given twice is still refused");

		/* Longer than any token can be, so the copy cannot run off. */
		{
			char big[256];
			size_t i;

			for (i = 0; i < 6; i++)
				big[i] = "token="[i];
			for (i = 6; i < sizeof(big) - 1; i++)
				big[i] = 'a';
			big[sizeof(big) - 1] = '\0';
			ok(auth_ok_form(&a, big, sizeof(big) - 1) == 0,
			   "a token far too long");
		}

		/* And the rule that matters most, once more from this door. */
		{
			struct auth unarmed;

			memset(&unarmed, 0, sizeof(unarmed));
			n = snprintf(form, sizeof(form), "token=%s", a.token);
			ok(n > 0 && auth_ok_form(&unarmed, form, (size_t)n) == 0,
			   "an unarmed guard refuses a form token too");
		}
	}

	printf("  %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
