/*
 * The error codes, and the catalogue they come from.
 *
 * `scripts/coverage.sh` put recon_error.c near the bottom of the list: it is
 * linked into eight test targets and exercised by none of them, so what it did
 * for those eight was compile.
 *
 * That mattered more than usual this week, because eleven codes were wired
 * into the system and every one of them was checked by reading. This is the
 * part that can be checked by asking.
 *
 * --- The catalogue's own rules ---
 *
 * `include/recon_errors.def` states three rules at the top and nothing
 * enforced any of them:
 *
 *   * a number is never reused, even after the fault it named stops existing,
 *     because somebody has that code written down
 *   * a code's letter and number never change
 *   * every area letter is one of the listed areas, and never I or O -- which
 *     are the two that read as 1 and 0 in the font somebody will be squinting
 *     at on a screen that has stopped
 *
 * Two of those three the compiler already enforces, which was found by
 * breaking them on purpose rather than by assuming either way. The macro
 * builds an enumerator name out of the area and the number, so a repeated pair
 * is a redeclaration and the build stops -- the duplicate check below is a
 * statement of the rule and a guard against the macro scheme changing, not the
 * thing standing between the project and a duplicate. Written down because a
 * test that claims to be load-bearing and is not is worse than no test.
 *
 * What the compiler does *not* check, and what breaking them proved this
 * does: an area letter that is not one of the listed areas -- adding VT-I001
 * compiles perfectly and is a code that reads as VT-1001 on a screen somebody
 * is squinting at -- and a code with an empty summary or detail, which is a
 * code somebody looks up and finds blank.
 *
 * --- And what happens when one is raised ---
 *
 * That it reaches the log, that a STOP also reaches the file the next start
 * reads, and that reading that file clears it -- so a fault is reported once
 * rather than at every start until somebody deletes something.
 *
 * Run with: cmake --build build && ./build/recon_error_tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_error.h"
#include "recon_fs.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL  %s\n", what);
    } else {
        printf("  ok    %s\n", what);
    }
}

static void test_every_code_is_different(void) {
    printf("The catalogue\n");

    int count = recon_error_count();
    check(count > 0, "there are codes");

    int duplicates = 0;
    char first_duplicate[16] = "";

    for (int i = 0; i < count; i++) {
        const struct recon_error_info *a = recon_error_at(i);
        if (a == NULL) {
            check(false, "every code in the range has an entry");
            return;
        }
        for (int j = i + 1; j < count; j++) {
            const struct recon_error_info *b = recon_error_at(j);
            if (b != NULL && strcmp(a->code, b->code) == 0) {
                if (duplicates == 0) {
                    snprintf(first_duplicate, sizeof(first_duplicate), "%s",
                        a->code);
                }
                duplicates++;
            }
        }
    }

    if (duplicates > 0) {
        printf("        %s appears twice\n", first_duplicate);
    }
    /* The compiler gets here first -- see the note at the top. This states the
     * rule and would catch it arriving by some other route. */
    check(duplicates == 0,
        "no number is used twice, which the build also refuses");
}

static void test_every_code_is_well_formed(void) {
    printf("What a code looks like\n");

    /* The areas listed at the top of include/recon_errors.def, and no others.
     * I and O are missing on purpose: they read as 1 and 0 on a screen
     * somebody is squinting at because the system has stopped. */
    static const char AREAS[] = "ABCDEFGHJKLM";

    int wrong_shape = 0;
    int unknown_area = 0;
    int no_words = 0;

    for (int i = 0; i < recon_error_count(); i++) {
        const struct recon_error_info *info = recon_error_at(i);
        if (info == NULL) {
            continue;
        }

        /* "VT-" then a letter then three digits. */
        if (strlen(info->code) != 7 || strncmp(info->code, "VT-", 3) != 0 ||
                info->code[4] < '0' || info->code[4] > '9' ||
                info->code[5] < '0' || info->code[5] > '9' ||
                info->code[6] < '0' || info->code[6] > '9') {
            wrong_shape++;
            continue;
        }
        if (strchr(AREAS, info->code[3]) == NULL) {
            unknown_area++;
            printf("        %s is in area '%c', which is not one of %s\n",
                info->code, info->code[3], AREAS);
        }
        if (info->summary == NULL || info->summary[0] == '\0' ||
                info->detail == NULL || info->detail[0] == '\0') {
            no_words++;
            printf("        %s has no %s\n", info->code,
                (info->summary == NULL || info->summary[0] == '\0')
                    ? "summary" : "detail");
        }
    }

    check(wrong_shape == 0, "every code is VT- a letter and three digits");
    check(unknown_area == 0, "every area letter is one of the listed areas");
    check(no_words == 0,
        "AND EVERY CODE SAYS SOMETHING -- a code somebody looks up and finds "
        "blank is worse than no code");
}

static void test_looking_one_up(void) {
    printf("Looking a code up the way somebody types it\n");

    const struct recon_error_info *info = recon_error_at(RECON_ERR_A001);
    check(info != NULL, "a code has an entry");
    if (info == NULL) {
        return;
    }

    check(recon_error_find(info->code) == info, "found by its full text");
    check(recon_error_find("a001") == info,
        "and without the VT-, in lower case, because that is how people type "
        "it");
    check(recon_error_find("VT-A001") == info, "and exactly as printed");
    check(recon_error_find("A999") == NULL,
        "a code that does not exist says so rather than returning the "
        "nearest");
    check(recon_error_find(NULL) == NULL, "and nothing at all is not a code");

    check(recon_error_at(-1) == NULL && recon_error_at(recon_error_count())
        == NULL, "an index outside the catalogue has no entry");
}

static void test_the_areas_have_names(void) {
    printf("The areas\n");

    int unnamed = 0;
    for (int i = 0; i < recon_error_count(); i++) {
        const struct recon_error_info *info = recon_error_at(i);
        if (info == NULL || strlen(info->code) < 4) {
            continue;
        }
        const char *name = recon_error_area_name(info->code[3]);
        if (name == NULL || name[0] == '\0') {
            unnamed++;
            printf("        area '%c' has no name\n", info->code[3]);
        }
    }
    check(unnamed == 0,
        "every area a code is in has a name, so `errors` can group them");

    check(recon_error_area_name('Z') == NULL ||
        recon_error_area_name('Z')[0] == '\0',
        "and an area nothing uses is not invented");
}

static void test_raising_reaches_the_log(void) {
    printf("Raising one\n");

    recon_fs_remove("/", RECON_ERROR_LOG);

    recon_error_raise(NULL, RECON_ERR_C003, "as 'Somebody'");

    size_t size = 0;
    char *log = recon_fs_read("/", RECON_ERROR_LOG, &size);
    check(log != NULL, "the log is written");
    if (log == NULL) {
        return;
    }

    check(strstr(log, "C003") != NULL, "with the code");
    check(strstr(log, "Somebody") != NULL,
        "and the detail the caller added, which is the part that says which "
        "occurrence this was");

    const struct recon_error_info *info = recon_error_at(RECON_ERR_C003);
    check(info != NULL && strstr(log, info->summary) != NULL,
        "and what the code means, so the log reads without the catalogue "
        "beside it");
    free(log);

    /* A second one appends rather than replacing. A log that keeps the last
     * line is a log that answers "what went wrong just now" and nothing else,
     * and "forty of these overnight" is the question it exists for. */
    recon_error_raise(NULL, RECON_ERR_H003, "something was ignored");
    log = recon_fs_read("/", RECON_ERROR_LOG, &size);
    check(log != NULL && strstr(log, "C003") != NULL &&
        strstr(log, "H003") != NULL,
        "AND A SECOND ONE IS ADDED RATHER THAN REPLACING THE FIRST");
    free(log);
}

static void test_a_fault_is_not_a_stop(void) {
    printf("What only a stop writes\n");

    recon_fs_remove("/", RECON_ERROR_LAST);

    /* A fault goes to the log and no further. The file below is what the next
     * start reads to say what happened to the last one, and a fault did not
     * end the last one. */
    recon_error_raise(NULL, RECON_ERR_H003, "a fault");

    char code[16] = "";
    char detail[256] = "";
    check(!recon_error_take_last(code, sizeof(code), detail, sizeof(detail)),
        "a fault leaves nothing for the next start to report");
}

static void test_the_last_run(void) {
    printf("Whether the last run ended properly\n");

    recon_fs_remove("/", RECON_ERROR_LAST);
    recon_fs_remove("/", RECON_ERROR_RUNNING);

    check(!recon_error_begin_run(),
        "with no marker, the last run ended properly");

    /* The marker is there while it runs. */
    size_t size = 0;
    char *marker = recon_fs_read("/", RECON_ERROR_RUNNING, &size);
    check(marker != NULL, "and a marker is left saying this one is running");
    free(marker);

    recon_error_end_run();
    check(recon_fs_read("/", RECON_ERROR_RUNNING, &size) == NULL,
        "ending properly takes it away");

    /* Now the case this exists for: a run that never reached its own end. */
    check(!recon_error_begin_run(), "a run starts");
    check(recon_error_begin_run(),
        "AND A SECOND ONE WITHOUT AN END BETWEEN THEM IS REPORTED -- which is "
        "a power cut, a kill, or a lock-up, none of which a handler can "
        "catch");

    char code[16] = "";
    char detail[256] = "";
    check(recon_error_take_last(code, sizeof(code), detail, sizeof(detail)),
        "and the next start has something to say");
    check(strstr(code, "A005") != NULL, "which is VT-A005");
    check(detail[0] != '\0', "with the detail naming the run that died");

    check(!recon_error_take_last(code, sizeof(code), detail, sizeof(detail)),
        "TAKEN RATHER THAN READ, so it is reported once rather than at every "
        "start forever");

    recon_error_end_run();
}

static void test_a_crash_is_the_better_answer(void) {
    printf("A crash and a power cut at once\n");

    recon_fs_remove("/", RECON_ERROR_RUNNING);
    recon_fs_remove("/", RECON_ERROR_LAST);

    check(!recon_error_begin_run(), "a run starts");

    /*
     * Something caught the fault and wrote what it was. The marker is still
     * there, because writing that record is not reaching the end of the run.
     *
     * Both are true, and only one is worth showing: the caught one names the
     * actual fault, and A005 says "something happened". Reporting both would
     * put two screens in front of somebody for one event, with the vaguer one
     * second.
     */
    const char *pretend = "VT-D001\n2026-01-01 00:00:00\nsomething specific\n";
    recon_fs_write("/", RECON_ERROR_LAST, pretend, strlen(pretend));

    check(!recon_error_begin_run(),
        "A005 STANDS ASIDE when something else already explained it");

    char code[16] = "";
    char detail[256] = "";
    check(recon_error_take_last(code, sizeof(code), detail, sizeof(detail)) &&
        strstr(code, "D001") != NULL,
        "and the specific answer is the one that survives");

    recon_error_end_run();
}

int main(void) {
    printf("Error codes\n\n");

    char root[] = "/tmp/recon-error-test-XXXXXX";
    if (mkdtemp(root) == NULL) {
        printf("could not make a temporary root\n");
        return 1;
    }
    if (!recon_fs_init(root)) {
        printf("could not start the filesystem: %s\n", recon_fs_last_error());
        return 1;
    }

    test_every_code_is_different();
    test_every_code_is_well_formed();
    test_looking_one_up();
    test_the_areas_have_names();
    test_raising_reaches_the_log();
    test_a_fault_is_not_a_stop();
    test_the_last_run();
    test_a_crash_is_the_better_answer();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
