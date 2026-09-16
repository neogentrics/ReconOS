/*
 * The Recycle Bin, which nothing had ever run.
 *
 * `scripts/coverage.sh --zero` lists the functions no suite reaches, and
 * fourteen of them were the bin: `recon_fs_trash`, `_restore`, `_purge`,
 * `_empty`, `_origin`, `_count`, `_is_trash`, `_usage` and their per-volume
 * versions. `recon_fs.c` sat at **46.82% of 848 lines** with the whole of this
 * untouched.
 *
 * --- Why this one and not another forty percent ---
 *
 * Because of what the bin is *for*. Every other part of `recon_fs.c` either
 * works or reports that it did not; the bin's entire job is that **deleting is
 * recoverable**, and the way that breaks is silent. A restore that puts a file
 * somewhere else, a second file with the same name landing on the first, a
 * purge that misses -- none of those announce themselves. Somebody finds out
 * when they go looking for something that is not there.
 *
 * The board called this row "the trash, copying and listing". This is the
 * trash, and it is the half where being wrong costs somebody their files.
 *
 * --- What is held to the header ---
 *
 * `include/recon_fs.h` is specific, and every sentence in it is a claim:
 *
 *   - *"Refuses /System, and refuses the bin itself."*
 *   - *"Put an item back where it came from. Fails if something is there now."*
 *   - *"a file in the bin is restored or purged, not deleted again"*
 *   - *"recon_fs_trash puts it in the bin belonging to the space it came
 *     from, so nothing that deletes has to know about any of this"*
 *
 * And one that is only in the source, at the line that builds the name:
 * *"the name in the bin is made unique rather than assumed to be"* -- which is
 * the classic way a bin loses a file, and has its own test below.
 *
 * Run with: ./build/recon_trash_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_fs.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
        printf("        last error: %s\n", recon_fs_last_error());
    }
}

static void same_int(int got, int wanted, const char *what) {
    g_checks++;
    if (got != wanted) {
        g_failures++;
        printf("  FAIL: %s -- got %d, wanted %d\n", what, got, wanted);
    }
}

static void write_file(const char *path, const char *text) {
    recon_fs_write("/", path, text, strlen(text));
}

static bool file_is(const char *path, const char *text) {
    size_t size = 0;
    char *have = recon_fs_read("/", path, &size);
    if (have == NULL) {
        return false;
    }
    bool same = (size == strlen(text)) && memcmp(have, text, size) == 0;
    free(have);
    return same;
}

/* The bin, emptied, so each test starts from a known place. */
static void start_clean(void) {
    recon_fs_trash_empty();
}

/* The one name in the bin, for a test that put exactly one thing there. */
static bool only_item(char *out, size_t size) {
    struct recon_dirent entries[32];
    const char *bin = recon_fs_trash_dir();
    int found = recon_fs_list("/", bin, entries, 32);

    int files = 0;
    for (int i = 0; i < found && i < 32; i++) {
        /* The bin keeps a note beside each item saying where it came from;
         * the item itself is what this is looking for. */
        if (strstr(entries[i].name, ".origin") != NULL) {
            continue;
        }
        if (files == 0) {
            snprintf(out, size, "%s", entries[i].name);
        }
        files++;
    }
    return files == 1;
}

/* --- Tests --- */

static void test_a_file_goes_in_and_comes_back(void) {
    printf("a file put in the bin comes back where it came from\n");
    start_clean();

    recon_fs_mkdir("/", "/Users/Documents");
    write_file("/Users/Documents/notes.txt", "what it said");

    same_int(recon_fs_trash_count(), 0, "the bin starts empty");

    check(recon_fs_trash("/", "/Users/Documents/notes.txt"),
        "it goes in the bin");
    check(!recon_fs_exists("/", "/Users/Documents/notes.txt"),
        "and is no longer where it was");
    same_int(recon_fs_trash_count(), 1, "the bin holds one thing");

    char name[RECON_NAME_MAX];
    check(only_item(name, sizeof(name)), "which is the one item in there");

    /* Where it came from, which is what a listing shows. */
    char origin[RECON_PATH_MAX];
    check(recon_fs_trash_origin(name, origin, sizeof(origin)),
        "the bin remembers where it came from");
    check(strstr(origin, "notes.txt") != NULL,
        "and names the file it was");

    check(recon_fs_trash_restore(name), "it restores");
    check(recon_fs_exists("/", "/Users/Documents/notes.txt"),
        "back where it came from, not somewhere else");
    check(file_is("/Users/Documents/notes.txt", "what it said"),
        "with what was in it");
    same_int(recon_fs_trash_count(), 0, "and the bin is empty again");
}

/*
 * The classic way a bin loses a file.
 *
 * Two files called the same thing, from different folders, deleted one after
 * the other. If the second is stored under the same name as the first, the
 * first is gone -- and the count says one when two went in, which is the only
 * sign anybody gets.
 */
static void test_two_files_with_one_name(void) {
    printf("two files with the same name both survive the bin\n");
    start_clean();

    recon_fs_mkdir("/", "/Users/Documents");
    recon_fs_mkdir("/", "/Users/Documents/Work");
    write_file("/Users/Documents/report.txt", "the first one");
    write_file("/Users/Documents/Work/report.txt", "the second one");

    check(recon_fs_trash("/", "/Users/Documents/report.txt"),
        "the first goes in");
    check(recon_fs_trash("/", "/Users/Documents/Work/report.txt"),
        "and so does the second");

    same_int(recon_fs_trash_count(), 2,
        "both are in the bin -- the second did not land on the first");

    /* And they are still two different files. */
    struct recon_dirent entries[32];
    int found = recon_fs_list("/", recon_fs_trash_dir(), entries, 32);
    int first = 0, second = 0;

    for (int i = 0; i < found && i < 32; i++) {
        if (strstr(entries[i].name, ".origin") != NULL) {
            continue;
        }
        char path[RECON_PATH_MAX];
        if (!recon_fs_join(path, sizeof(path), recon_fs_trash_dir(),
                entries[i].name)) {
            continue;
        }
        if (file_is(path, "the first one")) {
            first++;
        }
        if (file_is(path, "the second one")) {
            second++;
        }
    }

    same_int(first, 1, "the first file's contents are in there once");
    same_int(second, 1, "and the second's, once");
}

/*
 * Restoring onto something that is there now.
 *
 * The header is explicit -- *"Fails if something is there now"* -- and the
 * reason it matters is that the alternative is silent: somebody deletes a
 * file, writes a new one with the same name, restores the old one from the
 * bin, and the new one is gone with nothing said.
 */
static void test_restore_does_not_overwrite(void) {
    printf("restoring onto something that exists now is refused\n");
    start_clean();

    recon_fs_mkdir("/", "/Users/Documents");
    write_file("/Users/Documents/letter.txt", "the old one");
    check(recon_fs_trash("/", "/Users/Documents/letter.txt"), "it goes in");

    /* Somebody made a new one with the same name. */
    write_file("/Users/Documents/letter.txt", "the new one");

    char name[RECON_NAME_MAX];
    check(only_item(name, sizeof(name)), "one thing in the bin");

    check(!recon_fs_trash_restore(name),
        "restoring is refused rather than replacing what is there");
    check(file_is("/Users/Documents/letter.txt", "the new one"),
        "and the file that is there now is untouched");
    same_int(recon_fs_trash_count(), 1,
        "the old one is still in the bin, not lost between the two");
}

/*
 * --- Two guards on /System, and this can only see one of them ---
 *
 * `recon_fs_trash` refuses a protected path itself, and the `recon_fs_rename`
 * it finishes with refuses a structural one anyway. Cutting the first out on
 * purpose leaves this test passing, because the second still holds.
 *
 * That is defence in depth and it is the right shape. It is written down so a
 * green run is not read as proof that the bin's own guard works -- the same
 * note `tests/test_package_install.c` carries about leaving a file alone.
 *
 * The guard below it, on the bin's own contents, has one guard and does bite:
 * cutting it out makes "cannot be deleted again" fail.
 */
static void test_what_it_refuses_to_delete(void) {
    printf("the bin refuses the system, and refuses itself\n");
    start_clean();

    check(!recon_fs_trash("/", RECON_DIR_SYSTEM),
        "/System cannot be put in the bin");
    check(recon_fs_exists("/", RECON_DIR_SYSTEM), "and is still there");

    check(!recon_fs_trash("/", "/"),
        "and neither can the root of everything");

    /* And the bin itself, which would otherwise be a bin inside a bin. */
    const char *bin = recon_fs_trash_dir();
    check(!recon_fs_trash("/", bin), "the bin cannot be put in the bin");
    check(recon_fs_exists("/", bin), "and still exists");

    /* Nor anything already inside it -- the header's rule: a file in the bin
     * is restored or purged, not deleted again. */
    recon_fs_mkdir("/", "/Users/Documents");
    write_file("/Users/Documents/twice.txt", "once");
    check(recon_fs_trash("/", "/Users/Documents/twice.txt"), "it goes in");

    char name[RECON_NAME_MAX];
    char inside[RECON_PATH_MAX];
    if (only_item(name, sizeof(name)) &&
            recon_fs_join(inside, sizeof(inside), bin, name)) {
        check(recon_fs_is_trash("/", inside),
            "something in the bin knows it is in the bin");
        check(!recon_fs_trash("/", inside),
            "and cannot be deleted again");
    }

    check(recon_fs_is_trash("/", bin), "the bin is the bin");
    check(!recon_fs_is_trash("/", "/Users/Documents"),
        "and an ordinary folder is not");
}

static void test_purging_one_and_emptying_all(void) {
    printf("one item goes permanently, or all of them do\n");
    start_clean();

    recon_fs_mkdir("/", "/Users/Documents");
    write_file("/Users/Documents/a.txt", "a");
    write_file("/Users/Documents/b.txt", "b");
    write_file("/Users/Documents/c.txt", "c");

    check(recon_fs_trash("/", "/Users/Documents/a.txt"), "a goes in");
    check(recon_fs_trash("/", "/Users/Documents/b.txt"), "b goes in");
    check(recon_fs_trash("/", "/Users/Documents/c.txt"), "c goes in");
    same_int(recon_fs_trash_count(), 3, "three things in the bin");

    /* One of them, permanently. */
    struct recon_dirent entries[32];
    int found = recon_fs_list("/", recon_fs_trash_dir(), entries, 32);
    char one[RECON_NAME_MAX];
    one[0] = '\0';
    for (int i = 0; i < found && i < 32; i++) {
        if (strstr(entries[i].name, ".origin") == NULL) {
            snprintf(one, sizeof(one), "%s", entries[i].name);
            break;
        }
    }

    if (one[0] != '\0') {
        check(recon_fs_trash_purge(one), "one of them is purged");
        same_int(recon_fs_trash_count(), 2, "and two are left");
    }

    check(recon_fs_trash_empty(), "the bin empties");
    same_int(recon_fs_trash_count(), 0, "and holds nothing");

    /* Emptying an empty bin is not a failure -- somebody clicking Empty twice
     * should not be told something went wrong. */
    check(recon_fs_trash_empty(), "emptying it again is still fine");
}

/*
 * A folder, which is the case that is not a file.
 *
 * Everything above moves one file. A folder with things in it has to go whole
 * and come back whole, and it is where a bin that moves entries one at a time
 * quietly drops the ones it did not reach.
 */
static void test_a_folder_goes_in_whole(void) {
    printf("a folder goes in whole and comes back whole\n");
    start_clean();

    recon_fs_mkdir("/", "/Users/Documents");
    recon_fs_mkdir("/", "/Users/Documents/Project");
    recon_fs_mkdir("/", "/Users/Documents/Project/Notes");
    write_file("/Users/Documents/Project/plan.txt", "the plan");
    write_file("/Users/Documents/Project/Notes/one.txt", "note one");

    check(recon_fs_trash("/", "/Users/Documents/Project"),
        "the folder goes in");
    check(!recon_fs_exists("/", "/Users/Documents/Project"),
        "and is gone from where it was");

    char name[RECON_NAME_MAX];
    check(only_item(name, sizeof(name)), "one thing in the bin");

    check(recon_fs_trash_restore(name), "it restores");
    check(recon_fs_exists("/", "/Users/Documents/Project"), "the folder is back");
    check(file_is("/Users/Documents/Project/plan.txt", "the plan"),
        "with the file that was in it");
    check(file_is("/Users/Documents/Project/Notes/one.txt", "note one"),
        "and the one that was two levels down");
}

/*
 * How much is in it.
 *
 * Shown on the Recycle Bin's own page, and the number people use to decide
 * whether emptying it is worth doing.
 */
static void test_what_it_says_it_is_holding(void) {
    printf("the bin says how much is in it\n");
    start_clean();

    unsigned long long bytes = 1;
    int files = 1;
    check(recon_fs_trash_usage_in(RECON_VOLUME_USER, &bytes, &files),
        "an empty bin reports its usage");
    same_int((int)bytes, 0, "nothing in it is no bytes");
    same_int(files, 0, "and no files");

    recon_fs_mkdir("/", "/Users/Documents");
    write_file("/Users/Documents/big.txt", "0123456789");
    check(recon_fs_trash("/", "/Users/Documents/big.txt"), "something goes in");

    bytes = 0;
    files = 0;
    check(recon_fs_trash_usage_in(RECON_VOLUME_USER, &bytes, &files),
        "and now it reports again");
    check(bytes >= 10, "counting at least the bytes that went in");
    check(files >= 1, "and at least the file");

    /* The per-volume bin and the plain one are the same bin for the user's
     * space, which is what every existing caller means. */
    check(strcmp(recon_fs_trash_dir(), recon_fs_trash_dir_in(RECON_VOLUME_USER))
            == 0,
        "the bin with no argument is the user's bin");
    same_int(recon_fs_trash_count(),
        recon_fs_trash_count_in(RECON_VOLUME_USER),
        "and they count the same things");
}

int main(void) {
    char root[] = "/tmp/reconos-trash-XXXXXX";
    if (mkdtemp(root) == NULL) {
        perror("mkdtemp");
        return 1;
    }

    printf("ReconOS Recycle Bin tests, root %s\n\n", root);

    if (!recon_fs_init(root)) {
        printf("could not set up the test filesystem: %s\n",
            recon_fs_last_error());
        return 1;
    }

    test_a_file_goes_in_and_comes_back();
    test_two_files_with_one_name();
    test_restore_does_not_overwrite();
    test_what_it_refuses_to_delete();
    test_purging_one_and_emptying_all();
    test_a_folder_goes_in_whole();
    test_what_it_says_it_is_holding();

    recon_fs_finish();

    char command[512];
    snprintf(command, sizeof(command), "rm -rf '%s'", root);
    if (system(command) != 0) {
        printf("\nnote: could not remove %s\n", root);
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
