/*
 * Tests for the ReconOS filesystem operations.
 *
 * These exist because renaming, copying and deleting are the operations that
 * lose a user's work when they are wrong, and clicking through a desktop is a
 * poor way to find out that they are. Each test runs against a throwaway root,
 * so nothing here can touch a real installation.
 *
 * Run with: ninja -C build && ./build/recon_fs_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

static bool file_says(const char *path, const char *expected) {
    size_t size = 0;
    char *data = recon_fs_read("/", path, &size);
    if (data == NULL) {
        return false;
    }
    bool same = (size == strlen(expected)) &&
        memcmp(data, expected, size) == 0;
    free(data);
    return same;
}

/* --- Tests --- */

static void test_unique_names(void) {
    printf("unique names\n");

    char name[RECON_NAME_MAX];

    check(recon_fs_unique_name("/", "/Temp", "Thing", "", name, sizeof(name)),
        "first name is available");
    check(strcmp(name, "Thing") == 0, "first name is the base itself");

    check(recon_fs_mkdir("/", "/Temp/Thing"), "create Thing");
    check(recon_fs_unique_name("/", "/Temp", "Thing", "", name, sizeof(name)),
        "second name found");
    check(strcmp(name, "Thing 2") == 0, "second name is numbered");

    /* The number goes before the extension, so a copy of notes.txt is still a
     * .txt file. */
    check(recon_fs_write("/", "/Temp/notes.txt", "x", 1), "create notes.txt");
    check(recon_fs_unique_name("/", "/Temp", "notes", ".txt", name, sizeof(name)),
        "extension name found");
    check(strcmp(name, "notes 2.txt") == 0, "number goes before the extension");
}

static void test_rename(void) {
    printf("renaming\n");

    check(recon_fs_write("/", "/Temp/before.txt", "hello", 5), "create a file");
    check(recon_fs_rename("/", "/Temp/before.txt", "/Temp/after.txt"),
        "rename it");
    check(!recon_fs_exists("/", "/Temp/before.txt"), "old name is gone");
    check(file_says("/Temp/after.txt", "hello"), "contents survived");

    /* Renaming onto something that exists would destroy it silently. */
    check(recon_fs_write("/", "/Temp/occupied.txt", "keep", 4), "create another");
    check(!recon_fs_rename("/", "/Temp/after.txt", "/Temp/occupied.txt"),
        "refuses to overwrite an existing name");
    check(file_says("/Temp/occupied.txt", "keep"), "the existing file is intact");

    /* Renaming to its own name is a no-op, not a failure: it is what happens
     * when a user opens a rename box and presses Enter without typing. */
    check(recon_fs_rename("/", "/Temp/after.txt", "/Temp/after.txt"),
        "renaming to the same name succeeds");

    /*
     * These tests run with nobody signed in, which the filesystem treats as
     * the system itself acting -- setup and the login screen have to be able
     * to write. So /System is writable here, and what is refused is the
     * *structure*: the directories the layout is made of.
     *
     * Who may write into /System is a question about roles, and lives in
     * tests/test_users.c where there are accounts to sign in as.
     */
    check(recon_fs_rename("/", "/Temp/after.txt", "/System/after.txt"),
        "an administrator may move a file into /System");
    check(recon_fs_remove("/", "/System/after.txt"),
        "and take it out again");

    check(!recon_fs_rename("/", "/System/Icons", "/Temp/Icons"),
        "but /System/Icons is part of the layout and cannot be moved");
    check(!recon_fs_rename("/", "/Temp", "/Temp2"),
        "and neither can /Temp");
}

static void test_copy(void) {
    printf("copying\n");

    check(recon_fs_write("/", "/Temp/source.txt", "content", 7), "create source");
    check(recon_fs_copy("/", "/Temp/source.txt", "/Temp/copy.txt"), "copy a file");
    check(file_says("/Temp/copy.txt", "content"), "the copy has the contents");
    check(file_says("/Temp/source.txt", "content"), "the source is untouched");

    check(!recon_fs_copy("/", "/Temp/source.txt", "/Temp/copy.txt"),
        "refuses to copy over an existing name");

    /* A directory copy takes its contents with it. */
    check(recon_fs_mkdir("/", "/Temp/tree"), "create a tree");
    check(recon_fs_mkdir("/", "/Temp/tree/inner"), "create a nested folder");
    check(recon_fs_write("/", "/Temp/tree/inner/deep.txt", "deep", 4),
        "create a nested file");

    check(recon_fs_copy("/", "/Temp/tree", "/Temp/tree-copy"), "copy the tree");
    check(file_says("/Temp/tree-copy/inner/deep.txt", "deep"),
        "the nested file came with it");

    /*
     * Copying a directory into itself would recurse until the disk filled.
     * This is the test that matters most here.
     */
    check(!recon_fs_copy("/", "/Temp/tree", "/Temp/tree/inside"),
        "refuses to copy a directory into itself");
    check(!recon_fs_rename("/", "/Temp/tree", "/Temp/tree/inside"),
        "refuses to move a directory into itself");
}

static void test_delete(void) {
    printf("deleting\n");

    check(recon_fs_write("/", "/Temp/gone.txt", "x", 1), "create a file");
    check(recon_fs_remove("/", "/Temp/gone.txt"), "remove a file");
    check(!recon_fs_exists("/", "/Temp/gone.txt"), "it is gone");

    check(recon_fs_mkdir("/", "/Temp/empty"), "create an empty folder");
    check(recon_fs_remove("/", "/Temp/empty"), "remove an empty folder");

    /* A folder with contents needs the recursive form, and the plain one must
     * refuse rather than take the contents with it. */
    check(recon_fs_mkdir("/", "/Temp/full"), "create a folder");
    check(recon_fs_write("/", "/Temp/full/thing.txt", "x", 1), "put a file in it");
    check(!recon_fs_remove("/", "/Temp/full"), "plain remove refuses a full folder");
    check(recon_fs_exists("/", "/Temp/full/thing.txt"), "the contents survived");

    check(recon_fs_remove_tree("/", "/Temp/full"), "recursive remove works");
    check(!recon_fs_exists("/", "/Temp/full"), "the folder is gone");

    /*
     * The layout cannot be removed by anybody. This is not a permission: there
     * is no state ReconOS knows how to be in without /System/Icons, so an
     * administrator deleting it has not exercised authority, they have broken
     * their computer.
     */
    check(!recon_fs_remove("/", "/System/Icons"),
        "the icons folder is part of the layout");
    check(!recon_fs_remove_tree("/", "/System"), "and so is /System");
    check(!recon_fs_remove_tree("/", "/"), "and the root");
    check(!recon_fs_remove_tree("/", "/Users"), "and /Users");
    check(recon_fs_exists("/", "/System/Icons"), "all still there");

    /* A file inside one of them is not the layout, and may go. */
    check(recon_fs_write("/", "/System/Config/scratch", "x", 1),
        "write a file into /System");
    check(recon_fs_remove("/", "/System/Config/scratch"),
        "and remove it, which is what administering means");
}

static void test_protection(void) {
    printf("protection\n");

    check(recon_fs_is_protected("/", "/System"), "/System is the system's");
    check(recon_fs_is_protected("/", "/System/Icons"),
        "and so is everything in it");
    check(!recon_fs_is_protected("/", "/Users"), "/Users is not");

    /* A different question: what the layout is made of. */
    check(recon_fs_is_structural("/", "/System/Icons"),
        "the icons folder is part of the layout");
    check(!recon_fs_is_structural("/", "/System/Icons/folder.ico"),
        "a file inside it is not");
    check(recon_fs_is_structural("/", "/Users"), "/Users is");
    check(!recon_fs_is_structural("/", "/Users/Administrator"),
        "one account's folder is not");

    /*
     * A name that merely starts with "System" is not inside it. Compared as a
     * prefix rather than by segment, "/Systems" would be protected for no
     * reason a user could work out.
     */
    check(recon_fs_mkdir("/", "/Systems"), "create /Systems");
    check(!recon_fs_is_protected("/", "/Systems"), "/Systems is not /System");
    check(recon_fs_remove("/", "/Systems"), "and can be removed");
}

static void test_escapes(void) {
    printf("escaping the root\n");

    /* A path that climbs out is refused, not clamped: clamping would let a
     * path mean something other than what it says. */
    check(!recon_fs_exists("/", "/../etc/passwd"), "..  above the root fails");
    check(!recon_fs_write("/", "/../escaped.txt", "x", 1),
        "cannot write above the root");
    check(!recon_fs_mkdir("/Temp", "../../../outside"),
        "cannot climb out with a relative path");

    /* Climbing and coming back is fine, because it stays inside. */
    check(recon_fs_mkdir("/Temp", "../Temp/round-trip"),
        "a path that leaves and returns is allowed");
    check(recon_fs_exists("/", "/Temp/round-trip"), "and lands where it says");
}

static void test_clipboard(void) {
    printf("the clipboard\n");

    recon_fs_clip_clear();
    check(recon_fs_clip_empty(), "starts empty");

    recon_fs_clip_set("/Temp/source.txt", false);
    check(!recon_fs_clip_empty(), "holds what was copied");

    char path[RECON_PATH_MAX];
    bool cut = true;
    check(recon_fs_clip_get(path, sizeof(path), &cut), "reads back");
    check(strcmp(path, "/Temp/source.txt") == 0, "the path is what was set");
    check(cut == false, "a copy is not a cut");

    recon_fs_clip_set("/Temp/other.txt", true);
    check(recon_fs_clip_get(path, sizeof(path), &cut) && cut, "a cut is a cut");

    recon_fs_clip_clear();
    check(recon_fs_clip_empty(), "clears");
    check(!recon_fs_clip_get(path, sizeof(path), &cut), "an empty clipboard reads nothing");
}

/* --- Harness --- */

int main(void) {
    char root[] = "/tmp/reconos-test-XXXXXX";
    if (mkdtemp(root) == NULL) {
        perror("mkdtemp");
        return 1;
    }

    printf("ReconOS filesystem tests, root %s\n\n", root);

    if (!recon_fs_init(root)) {
        printf("could not set up the test filesystem: %s\n", recon_fs_last_error());
        return 1;
    }

    test_unique_names();
    test_rename();
    test_copy();
    test_delete();
    test_protection();
    test_escapes();
    test_clipboard();

    recon_fs_finish();

    /* Take the throwaway root with us; leaving these around would fill /tmp
     * one test run at a time. */
    char command[512];
    snprintf(command, sizeof(command), "rm -rf '%s'", root);
    if (system(command) != 0) {
        printf("\nnote: could not remove %s\n", root);
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
