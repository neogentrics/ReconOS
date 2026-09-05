/*
 * Tests for the font list.
 *
 * The failure worth catching here is the preset rule, which is a rule about
 * what cannot happen: a font that ships must not be removable, and neither
 * must the one currently being drawn with. Both are enforced by a couple of
 * `if`s, both are easy to lose in a refactor, and neither failure is visible
 * until somebody's desktop has no typeface left.
 *
 * The other thing worth pinning down is that two fonts with the same name
 * from different folders both survive. That one is silent in the other
 * direction: the second install appears to work and quietly replaces the
 * first.
 *
 * Run with: ninja -C build && ./build/recon_fonts_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "recon_access.h"
#include "recon_fonts.h"
#include "recon_fs.h"
#include "recon_registry.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
    }
}

/* A file that is not a font but is named like one. Nothing here loads a
 * typeface -- these tests are about the list, not about drawing. */
static bool make_file(const char *path, const char *contents) {
    return recon_fs_write("/", path, contents, strlen(contents));
}

/* --- Tests --- */

static void test_only_fonts_are_taken(void) {
    printf("only things named like a font are taken\n");

    recon_fs_mkdir("/", "/Users");
    recon_fs_mkdir("/", "/Users/Test");
    check(make_file("/Users/Test/notes.txt", "not a font"),
        "the test file could be written");

    char name[96];
    check(!recon_fonts_add("/", "/Users/Test/notes.txt", name, sizeof(name)),
        "a .txt is refused");
    check(recon_fonts_last_error()[0] != '\0',
        "and the refusal says why");
}

static void test_added_then_listed(void) {
    printf("a font added is a font listed\n");

    check(make_file("/Users/Test/Inter.ttf", "pretend typeface"),
        "the test font could be written");

    int before = recon_fonts_count();

    char name[96];
    check(recon_fonts_add("/", "/Users/Test/Inter.ttf", name, sizeof(name)),
        "it is taken");
    check(strcmp(name, "Inter.ttf") == 0, "under its own name");
    check(recon_fonts_count() == before + 1, "and the count goes up by one");

    char origin[RECON_PATH_MAX];
    check(recon_fonts_origin("Inter.ttf", origin, sizeof(origin)),
        "where it came from is remembered");
    check(strcmp(origin, "/Users/Test/Inter.ttf") == 0,
        "and it is the path it came from");

    char path[RECON_PATH_MAX];
    check(recon_fonts_path("Inter.ttf", path, sizeof(path)),
        "it has a path inside the system");
    check(recon_fs_exists("/", path), "and something is actually there");
}

static void test_same_name_twice(void) {
    printf("two folders can each hold an Inter.ttf\n");

    recon_fs_mkdir("/", "/Users/Test/Second");
    check(make_file("/Users/Test/Second/Inter.ttf", "a different typeface"),
        "the second one could be written");

    char name[96];
    check(recon_fonts_add("/", "/Users/Test/Second/Inter.ttf", name,
        sizeof(name)), "the second is taken too");
    check(strcmp(name, "Inter.ttf") != 0,
        "under a different name, so the first survives");

    char origin[RECON_PATH_MAX];
    check(recon_fonts_origin("Inter.ttf", origin, sizeof(origin)) &&
        strcmp(origin, "/Users/Test/Inter.ttf") == 0,
        "and the first still says where it came from");
}

static void test_a_preset_cannot_be_removed(void) {
    printf("a font that ships cannot be removed\n");

    /*
     * "Ships" means "has no origin recorded", which is what a font placed by
     * the build rather than by a person looks like. Written straight into the
     * folder to make one.
     */
    char path[RECON_PATH_MAX];
    check(recon_fonts_path("Shipped.ttf", path, sizeof(path)),
        "a name inside the folder can be built");
    check(make_file(path, "shipped typeface"), "and written to");

    char origin[RECON_PATH_MAX];
    check(!recon_fonts_origin("Shipped.ttf", origin, sizeof(origin)),
        "nothing says where it came from, which is what shipping looks like");

    check(!recon_fonts_remove("Shipped.ttf"), "removing it is refused");
    check(recon_fs_exists("/", path), "and it is still there");

    const char *why = recon_fonts_last_error();
    check(strstr(why, "ships") != NULL,
        "and the refusal says it is because it ships");
}

static void test_the_one_in_use_cannot_be_removed(void) {
    printf("the font being drawn with cannot be removed\n");

    char path[RECON_PATH_MAX];
    check(recon_fonts_path("Inter.ttf", path, sizeof(path)), "its path");

    recon_registry_set(RECON_REG_USER, RECON_ACCESS_FONT_KEY, path);

    check(!recon_fonts_remove("Inter.ttf"), "removing it is refused");
    check(recon_fs_exists("/", path), "and it is still there");

    const char *why = recon_fonts_last_error();
    check(strstr(why, "in use") != NULL,
        "and the refusal says it is the one in use");

    /* Chosen something else, and now it goes. */
    recon_registry_remove(RECON_REG_USER, RECON_ACCESS_FONT_KEY);

    check(recon_fonts_remove("Inter.ttf"), "once nothing is using it, it goes");
    check(!recon_fs_exists("/", path), "and the file is gone");
    check(!recon_fonts_origin("Inter.ttf", path, sizeof(path)),
        "and so is the line saying where it came from");
}

static void test_removing_leaves_the_others_alone(void) {
    printf("removing one leaves the others' origins intact\n");

    /*
     * The origins file is rewritten line by line on a removal, which is
     * exactly the kind of code that comes back with a line duplicated or a
     * line's second half written twice.
     */
    int count = recon_fonts_count();
    check(count >= 1, "there is at least one left to check");

    for (int i = 0; i < count; i++) {
        char name[96];
        if (!recon_fonts_at(i, name, sizeof(name))) {
            continue;
        }

        char origin[RECON_PATH_MAX];
        if (!recon_fonts_origin(name, origin, sizeof(origin))) {
            continue;   /* One that ships. */
        }

        check(strchr(origin, '|') == NULL, "no origin has a bar left in it");
        check(origin[0] == '/', "and every one is still a path");
    }
}

int main(void) {
    char root[] = "/tmp/reconos-fonts-XXXXXX";
    if (mkdtemp(root) == NULL) {
        perror("mkdtemp");
        return 1;
    }

    printf("ReconOS font tests, root %s\n\n", root);

    if (!recon_fs_init(root)) {
        printf("could not set up the test filesystem: %s\n",
            recon_fs_last_error());
        return 1;
    }
    recon_registry_init();

    test_only_fonts_are_taken();
    test_added_then_listed();
    test_same_name_twice();
    test_a_preset_cannot_be_removed();
    test_the_one_in_use_cannot_be_removed();
    test_removing_leaves_the_others_alone();

    recon_registry_finish();
    recon_fs_finish();

    char command[512];
    snprintf(command, sizeof(command), "rm -rf '%s'", root);
    if (system(command) != 0) {
        printf("\nnote: could not remove %s\n", root);
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
