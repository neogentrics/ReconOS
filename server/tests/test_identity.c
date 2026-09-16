/*
 * Naming a parallel, held against the cases that would put two machines on one
 * name.
 *
 * There is no host function to be differential against -- nothing in POSIX
 * numbers a machine's successor -- so this is a table of cases and their
 * required answers, and the table is where the value is. Each case below is
 * either a way the naming is asked to work or a way it is required to refuse,
 * and the refusals outnumber the successes because refusing is the harder half
 * to get right.
 *
 * **Every case here was watched failing before it was believed.** The suite was
 * run against a deliberately broken `server_name_split` -- the `atoi` shape the
 * module's header describes and rejects -- and the padding and overflow cases
 * failed as they should. A suite nobody has seen fail proves nothing about the
 * code; it proves the code and the suite agree.
 */

#include "../include/recon_server.h"

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

static void split_gives(const char *name, const char *stem,
                        unsigned number, unsigned digits)
{
	struct name_family f;
	int rc = server_name_split(name, &f);

	checks++;
	if (rc != SERVER_OK) {
		failures++;
		printf("  FAIL  split(\"%s\") refused with %d\n", name, rc);
		return;
	}
	if (strcmp(f.stem, stem) != 0 || f.number != number
	    || f.digits != digits) {
		failures++;
		printf("  FAIL  split(\"%s\") gave (\"%s\", %u, %u),"
		       " wanted (\"%s\", %u, %u)\n",
		       name, f.stem, f.number, f.digits, stem, number, digits);
	}
}

static void split_refuses(const char *name, int why)
{
	struct name_family f;
	int rc = server_name_split(name, &f);

	checks++;
	if (rc != why) {
		failures++;
		printf("  FAIL  split(\"%s\") gave %d, wanted %d\n",
		       name, rc, why);
	}
}

static void parallel_gives(const char *peer, const char *const *taken,
                           size_t count, const char *want)
{
	char got[RECON_NAME_MAX];
	int rc = server_name_next_parallel(peer, taken, count,
	                                   got, sizeof(got));

	checks++;
	if (rc != SERVER_OK) {
		failures++;
		printf("  FAIL  parallel(\"%s\") refused with %d\n", peer, rc);
		return;
	}
	if (strcmp(got, want) != 0) {
		failures++;
		printf("  FAIL  parallel(\"%s\") gave \"%s\", wanted \"%s\"\n",
		       peer, got, want);
	}
}

int main(void)
{
	printf("naming a parallel\n");

	/* --- the case this module exists for ------------------------------ */
	{
		const char *wire[] = { "M16" };

		split_gives("M16", "M", 16, 2);
		parallel_gives("M16", wire, 1, "M17");
	}

	/* --- a wire that already holds the successor ----------------------- */
	{
		const char *wire[] = { "M16", "M17" };
		const char *gappy[] = { "M16", "M17", "M19" };

		parallel_gives("M16", wire, 2, "M18");
		parallel_gives("M16", gappy, 3, "M18");
	}

	/* --- a host name is case-insensitive, and a collision does not care -
	 *
	 * The wire says `m17` in lower case. Handing out `M17` would put two
	 * machines on one name, which is the fault this module exists to
	 * prevent and the one a naive strcmp would walk straight into. */
	{
		const char *wire[] = { "M16", "m17" };

		parallel_gives("M16", wire, 2, "M18");
	}

	/* --- padding is joined, not redefined ------------------------------
	 *
	 * A machine cloning `srv007` joins a family that writes three wide.
	 * `srv8` would be a different family with a similar spelling. */
	{
		const char *wire[] = { "srv007" };

		split_gives("srv007", "srv", 7, 3);
		parallel_gives("srv007", wire, 1, "srv008");
	}

	/* --- and the width is a floor, not a ceiling ----------------------- */
	{
		const char *wire[] = { "M99" };
		const char *narrow[] = { "node9" };

		split_gives("M99", "M", 99, 2);
		parallel_gives("M99", wire, 1, "M100");
		parallel_gives("node9", narrow, 1, "node10");
	}

	/* --- zero is a number ---------------------------------------------- */
	{
		const char *wire[] = { "M0" };

		split_gives("M0", "M", 0, 1);
		parallel_gives("M0", wire, 1, "M1");
	}
	{
		const char *wire[] = { "M00" };

		split_gives("M00", "M", 0, 2);
		parallel_gives("M00", wire, 1, "M01");
	}

	/* --- an empty wire still numbers upward ---------------------------- */
	parallel_gives("M16", NULL, 0, "M17");

	/* --- names that name no family are refused, not invented ----------- */
	split_refuses("gateway", SERVER_EUNNUMBERED);
	split_refuses("fileserver", SERVER_EUNNUMBERED);
	split_refuses("M16a", SERVER_EUNNUMBERED);
	split_refuses("16", SERVER_EALLDIGITS);
	split_refuses("007", SERVER_EALLDIGITS);
	split_refuses("", SERVER_EEMPTY);

	/* A parallel of an unnumbered peer is refused for the same reason,
	 * rather than becoming `gateway2`. */
	{
		char got[RECON_NAME_MAX];

		ok(server_name_next_parallel("gateway", NULL, 0,
		                             got, sizeof(got))
		   == SERVER_EUNNUMBERED,
		   "a parallel of an unnumbered peer is refused");
	}

	/* --- characters a host name may not carry -------------------------- */
	split_refuses("srv_01", SERVER_ECHAR);	/* DNS will not publish it */
	split_refuses("srv.01", SERVER_ECHAR);	/* that is two labels */
	split_refuses("-srv01", SERVER_ECHAR);
	split_refuses("srv01-", SERVER_ECHAR);
	ok(1, "hyphens inside a name are legal");
	split_gives("web-srv01", "web-srv", 1, 2);

	/* --- overflow is refused, not wrapped ------------------------------
	 *
	 * The `atoi` shape the header rejects gives a number here instead of a
	 * refusal, and this is the case that catches it. */
	split_refuses("M99999999999999999999", SERVER_ERANGE);
	split_gives("M4294967295", "M", 4294967295u, 10);
	{
		char got[RECON_NAME_MAX];

		ok(server_name_next_parallel("M4294967295", NULL, 0,
		                             got, sizeof(got))
		   == SERVER_ERANGE,
		   "a parallel past the last number is refused");
	}

	/* --- a name too long is refused, never truncated -------------------
	 *
	 * A truncated name is a name that belongs to a different machine. */
	{
		char lengthy[RECON_NAME_MAX + 8];
		struct name_family f;
		char got[8];
		size_t i;

		for (i = 0; i < sizeof(lengthy) - 1; i++)
			lengthy[i] = 'a';
		lengthy[sizeof(lengthy) - 1] = '\0';
		ok(server_name_split(lengthy, &f) == SERVER_ETOOLONG,
		   "an over-long name is refused");

		/* And a buffer too small for the answer refuses rather than
		 * writing a shorter name into it. */
		ok(server_name_next_parallel("M16", NULL, 0, got, 3)
		   == SERVER_ETOOLONG,
		   "a buffer too small for the name is refused");
	}

	/* --- a family that is entirely taken ------------------------------- */
	{
		static char names[RECON_PARALLEL_SEARCH_MAX + 1][RECON_NAME_MAX];
		static const char *wire[RECON_PARALLEL_SEARCH_MAX + 1];
		struct name_family f;
		char got[RECON_NAME_MAX];
		size_t i;

		server_name_split("M1", &f);
		for (i = 0; i <= RECON_PARALLEL_SEARCH_MAX; i++) {
			server_name_format(&f, (unsigned)(1 + i),
			                   names[i], RECON_NAME_MAX);
			wire[i] = names[i];
		}

		ok(server_name_next_parallel("M1", wire,
		                             RECON_PARALLEL_SEARCH_MAX + 1,
		                             got, sizeof(got))
		   == SERVER_EEXHAUSTED,
		   "a family with no free number is refused");
	}

	printf("  %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
