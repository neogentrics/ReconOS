/*
 * The shape a ReconOS volume has, checked without one.
 *
 * `layout.c` is handed the thing that makes a directory, so here it is handed
 * a fake that records what it was asked for and can be told to fail. Every
 * decision the real thing makes -- what gets made, in what order, what counts
 * as already done, what a refusal does to the rest -- is checked on the host
 * with no volume, no kernel and no disk.
 *
 * **And the drift check is the one that matters most.** The layout is named in
 * two places: `include/recon_fs.h`, which the compositor uses and has carried
 * since v0.1.0, and `userland/init/layout.c`, which is what actually gets made
 * on a machine. They cannot be one file -- `recon_fs.h` reaches for <time.h>
 * and a filesystem interface that do not exist for a program on this kernel --
 * so the duplication is real, and the only honest thing to do with a real
 * duplication is test it. This suite reads the header and requires the two to
 * agree in both directions.
 *
 * Run with: ./build/recon_init_layout_tests
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../init/layout.h"

/* The value the kernel answers when a directory is already there. Written out
 * rather than included: this file is the host's and SYS_EEXIST lives in a
 * header meant for the machine. The two agreeing is checked by the machine
 * refusing, not by this number. */
#define ALREADY_THERE (-6L)

static int g_failures;
static long g_checks;

static void check(int condition, const char *what)
{
	g_checks++;
	if (!condition) {
		g_failures++;
		printf("  FAIL: %s\n", what);
	}
}

/* --- A directory maker that does not make directories --- */

#define ASKED_MAX 64

static const char *g_asked[ASKED_MAX];
static unsigned long g_modes[ASKED_MAX];
static int g_asked_count;

/* Paths the fake should pretend already exist, or refuse. */
static const char *g_pretend_exists[ASKED_MAX];
static int g_pretend_exists_count;
static const char *g_pretend_refused[ASKED_MAX];
static long g_refusal_reason = -12L;	/* SYS_EPERM */
static int g_pretend_refused_count;

static void fake_reset(void)
{
	g_asked_count = 0;
	g_pretend_exists_count = 0;
	g_pretend_refused_count = 0;
}

static long fake_make(const char *path, unsigned long mode)
{
	int i;

	if (g_asked_count < ASKED_MAX) {
		g_asked[g_asked_count] = path;
		g_modes[g_asked_count] = mode;
		g_asked_count++;
	}

	for (i = 0; i < g_pretend_exists_count; i++) {
		if (strcmp(g_pretend_exists[i], path) == 0) {
			return ALREADY_THERE;
		}
	}
	for (i = 0; i < g_pretend_refused_count; i++) {
		if (strcmp(g_pretend_refused[i], path) == 0) {
			return g_refusal_reason;
		}
	}
	return 0;
}

/* --- The list itself --- */

static void test_the_list(void)
{
	int n = recon_layout_count();
	int i;

	printf("the list, and the order it has to be in\n");

	check(n > 0, "there is a layout at all");
	check(n == 10, "and it is the ten directories the desktop names");

	/*
	 * **Parents before children**, which is load-bearing rather than tidy:
	 * SYS_MKDIR makes one directory and refuses if the parent is not
	 * there. A list with `/System/Apps` before `/System` would make a
	 * machine that boots to nine refusals, and nothing else here would
	 * notice.
	 */
	for (i = 0; i < n; i++) {
		const char *path = RECON_LAYOUT[i];
		const char *slash;
		char parent[256];
		int j;
		int found = 0;

		check(path[0] == '/', "every path is absolute");
		check(strstr(path, "//") == NULL, "and has no empty component");
		check(path[strlen(path) - 1] != '/',
		      "and does not end in a separator");

		slash = strrchr(path, '/');
		if (slash == path) {
			continue;	/* a top-level directory has the root
					 * as its parent, which always exists */
		}

		snprintf(parent, sizeof(parent), "%.*s",
			 (int)(slash - path), path);
		for (j = 0; j < i; j++) {
			if (strcmp(RECON_LAYOUT[j], parent) == 0) {
				found = 1;
				break;
			}
		}
		if (!found) {
			g_checks++;
			g_failures++;
			printf("  FAIL: %s comes before its parent %s, so it"
			       " would be refused\n", path, parent);
		} else {
			check(1, "its parent comes first");
		}
	}

	/* No duplicates. Two entries for one path is one wasted refusal and a
	 * count that does not mean what it says. */
	for (i = 0; i < n; i++) {
		int j;

		for (j = i + 1; j < n; j++) {
			if (strcmp(RECON_LAYOUT[i], RECON_LAYOUT[j]) == 0) {
				g_checks++;
				g_failures++;
				printf("  FAIL: %s is in the list twice\n",
				       RECON_LAYOUT[i]);
			}
		}
	}
}

/* --- Building it --- */

static void test_a_fresh_volume(void)
{
	struct recon_layout_report report;
	int present;
	int i;

	printf("a volume with nothing on it\n");

	fake_reset();
	present = recon_layout_build(fake_make, ALREADY_THERE, &report);

	check(present == recon_layout_count(),
	      "every directory is there afterwards");
	check(report.made == recon_layout_count(), "and every one was made");
	check(report.already == 0, "none was already there");
	check(report.refused == 0, "and none was refused");
	check(g_asked_count == recon_layout_count(),
	      "each was asked for exactly once");

	/* In the order the list gives, which is what the parent rule needs. */
	for (i = 0; i < g_asked_count; i++) {
		check(strcmp(g_asked[i], RECON_LAYOUT[i]) == 0,
		      "and in the order the list gives");
		check(g_modes[i] == 0755u, "with the mode the layout states");
	}
}

static void test_a_second_boot(void)
{
	struct recon_layout_report report;
	int present;
	int i;

	printf("the same volume, booted again\n");

	fake_reset();
	for (i = 0; RECON_LAYOUT[i] != NULL; i++) {
		g_pretend_exists[g_pretend_exists_count++] = RECON_LAYOUT[i];
	}

	present = recon_layout_build(fake_make, ALREADY_THERE, &report);

	/*
	 * A second boot has to be a no-op, and that is a *requirement* rather
	 * than a nicety: this runs on every start, and a layout step that
	 * failed the second time would make every machine report a fault from
	 * its second boot onward.
	 */
	check(present == recon_layout_count(), "everything is still there");
	check(report.made == 0, "nothing was made");
	check(report.already == recon_layout_count(),
	      "because everything already was");
	check(report.refused == 0, "and nothing was refused");
}

static void test_a_boot_that_was_interrupted(void)
{
	struct recon_layout_report report;
	int present;

	printf("a machine that lost power half way through its first boot\n");

	fake_reset();
	/* The first four exist; the rest do not. */
	g_pretend_exists[g_pretend_exists_count++] = RECON_LAYOUT[0];
	g_pretend_exists[g_pretend_exists_count++] = RECON_LAYOUT[1];
	g_pretend_exists[g_pretend_exists_count++] = RECON_LAYOUT[2];
	g_pretend_exists[g_pretend_exists_count++] = RECON_LAYOUT[3];

	present = recon_layout_build(fake_make, ALREADY_THERE, &report);

	check(present == recon_layout_count(), "it finishes the job");
	check(report.already == 4, "four were already there");
	check(report.made == recon_layout_count() - 4,
	      "and the rest were made");
	check(report.refused == 0, "with nothing refused");
}

static void test_a_refusal(void)
{
	struct recon_layout_report report;
	int present;

	printf("a directory that cannot be made\n");

	fake_reset();
	g_pretend_refused[g_pretend_refused_count++] = "/Users";
	present = recon_layout_build(fake_make, ALREADY_THERE, &report);

	/*
	 * **It carries on.** The obvious thing is to stop at the first
	 * refusal, and it is wrong: these are independent of each other except
	 * for the parent rule, so one that cannot be made says nothing about
	 * the next. Stopping would turn a read-only `/Users` into a machine
	 * with no `/Temp` either, and would report the first fault while
	 * hiding the rest.
	 */
	check(present == recon_layout_count() - 1,
	      "the others are still made");
	check(report.refused == 1, "one was refused");
	check(report.first_refused != NULL &&
	      strcmp(report.first_refused, "/Users") == 0,
	      "and the report says which");
	check(report.first_reason == g_refusal_reason, "and why");
	check(g_asked_count == recon_layout_count(),
	      "every one was still attempted");
}

static void test_nothing_to_call(void)
{
	struct recon_layout_report report;

	printf("no way to make a directory at all\n");

	memset(&report, 0xFF, sizeof(report));
	check(recon_layout_build(NULL, ALREADY_THERE, &report) == 0,
	      "a null maker builds nothing");
	check(report.made == 0 && report.already == 0 && report.refused == 0,
	      "and the report is cleared rather than left as it was found");

	fake_reset();
	check(recon_layout_build(fake_make, ALREADY_THERE, NULL) ==
	      recon_layout_count(),
	      "and a caller that wants no report still gets the work done");
}

/* --- The drift check --- */

/*
 * Read `include/recon_fs.h` and require it to agree with this list.
 *
 * The layout is named in two places and cannot be one: the header is the
 * compositor's and reaches for things a program on this kernel does not have.
 * A real duplication is a thing to test, not a thing to feel bad about -- and
 * this is the cheapest possible test of it, because the header is right there
 * and its constants are all spelled the same way.
 *
 * Both directions, which is the half people leave out: a path added to the
 * header and not here is a directory the desktop expects and no machine has,
 * and a path here and not in the header is a directory nothing will ever look
 * in.
 */
static void test_it_agrees_with_the_desktop(void)
{
	FILE *f;
	char line[512];
	const char *names[64];
	char storage[64][256];
	int count = 0;
	int i;
	int j;

	printf("this list against include/recon_fs.h, in both directions\n");

	f = fopen("include/recon_fs.h", "r");
	if (f == NULL) {
		f = fopen("../include/recon_fs.h", "r");
	}
	if (f == NULL) {
		check(0, "could not read include/recon_fs.h to compare with");
		return;
	}

	while (fgets(line, sizeof(line), f) != NULL && count < 64) {
		char *at = strstr(line, "#define RECON_DIR_");
		char *open_quote;
		char *close_quote;

		if (at == NULL) {
			continue;
		}
		open_quote = strchr(at, '"');
		if (open_quote == NULL) {
			continue;
		}
		close_quote = strchr(open_quote + 1, '"');
		if (close_quote == NULL) {
			continue;
		}
		*close_quote = '\0';
		snprintf(storage[count], sizeof(storage[count]), "%s",
			 open_quote + 1);
		names[count] = storage[count];
		count++;
	}
	fclose(f);

	check(count > 0, "the header names some directories");

	/* Every constant in the header is in the layout. */
	for (i = 0; i < count; i++) {
		int found = 0;

		for (j = 0; RECON_LAYOUT[j] != NULL; j++) {
			if (strcmp(RECON_LAYOUT[j], names[i]) == 0) {
				found = 1;
				break;
			}
		}
		g_checks++;
		if (!found) {
			g_failures++;
			printf("  FAIL: include/recon_fs.h names %s and no"
			       " machine would ever have it\n", names[i]);
		}
	}

	/* And every entry in the layout is in the header. */
	for (j = 0; RECON_LAYOUT[j] != NULL; j++) {
		int found = 0;

		for (i = 0; i < count; i++) {
			if (strcmp(RECON_LAYOUT[j], names[i]) == 0) {
				found = 1;
				break;
			}
		}
		g_checks++;
		if (!found) {
			g_failures++;
			printf("  FAIL: %s is made on every machine and"
			       " nothing in the desktop names it\n",
			       RECON_LAYOUT[j]);
		}
	}
}

int main(void)
{
	printf("ReconOS: the shape a volume has\n\n");

	test_the_list();
	test_a_fresh_volume();
	test_a_second_boot();
	test_a_boot_that_was_interrupted();
	test_a_refusal();
	test_nothing_to_call();
	test_it_agrees_with_the_desktop();

	printf("\n%ld checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
