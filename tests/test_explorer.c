/*
 * The file manager, driven headlessly.
 *
 * --- What this is here to hold ---
 *
 * `src/recon_explorer.c` is 2,955 lines and had no suite. It is also the one
 * application in this system that **deletes things**, and the rule that keeps
 * that safe is a rule about *order*: it asks first, and the file is still
 * there while it is asking.
 *
 * That order is exactly the kind of thing no photograph can check. A picture
 * of a dialog says a dialog appeared; it cannot say the file survived to the
 * moment the picture was taken. Here the question and the directory can be
 * read in the same breath.
 *
 * --- How it is driven ---
 *
 * `recon_explorer_open_at` puts it somewhere, `recon_appwin_handle_key` sends
 * a keystroke, and `recon_appwin_describe` reads back what `explorer_describe`
 * writes -- cwd, how many entries, what is selected, and which question is
 * standing. The real `recon_fs.c` is underneath, so every directory below is
 * a real directory and every delete is a real delete.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ReconOS.h"
#include "recon_appwin.h"
#include "recon_explorer.h"
#include "recon_fs.h"
#include "recon_ui.h"

static int g_checks;
static int g_failures;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
    }
}

static void report(struct recon_appwin *win, char *out, size_t size) {
    recon_appwin_describe(win, out, size);
}

static bool reports(struct recon_appwin *win, const char *want) {
    char out[2048];
    report(win, out, sizeof(out));
    return strstr(out, want) != NULL;
}

static void check_reports(struct recon_appwin *win, const char *want,
        const char *what) {
    g_checks++;
    if (!reports(win, want)) {
        g_failures++;
        char out[2048];
        report(win, out, sizeof(out));
        printf("  FAIL: %s\n    wanted to find: \"%s\"\n%s", what, want, out);
    }
}

static struct recon_server *fake_server(void) {
    static int nothing;
    return (struct recon_server *)&nothing;
}

static void write_file(const char *path, const char *text) {
    if (!recon_fs_write("/", path, text, strlen(text))) {
        printf("  (could not write %s)\n", path);
    }
}

/* --- the tests ---------------------------------------------------------- */

static void test_it_lists_what_is_there(void) {
    printf("a file manager opens somewhere and lists what is in it\n");

    recon_fs_mkdir("/", "/Users/Tester/Papers");
    write_file("/Users/Tester/Papers/one.txt", "1");
    write_file("/Users/Tester/Papers/two.txt", "2");

    struct recon_appwin *win = recon_explorer_create(fake_server(), NULL);
    check(win != NULL, "A FILE MANAGER CAN BE MADE WITHOUT A COMPOSITOR");
    if (win == NULL) {
        return;
    }
    recon_appwin_show(win);
    /*
     * Focused, or no key arrives. `recon_appwin_handle_key` refuses a window
     * that is not focused, which is right -- a keystroke belongs to whatever
     * is in front -- and it is the sort of thing a suite finds by a test doing
     * nothing and saying so.
     */
    recon_appwin_set_focused(win, true);

    recon_explorer_open_at(win, "/Users/Tester/Papers");
    check_reports(win, "cwd: /Users/Tester/Papers", "it is where it was sent");
    check_reports(win, "entries: 2", "and it found both files");

    recon_appwin_destroy(win);
}

static void test_a_delete_asks_before_it_acts(void) {
    printf("and a delete asks first -- with the file still there\n");

    recon_fs_mkdir("/", "/Users/Tester/Doomed");
    write_file("/Users/Tester/Doomed/keepme.txt", "still here");

    struct recon_appwin *win = recon_explorer_create(fake_server(), NULL);
    if (win == NULL) {
        check(false, "a file manager to work with");
        return;
    }
    recon_appwin_show(win);
    /*
     * Focused, or no key arrives. `recon_appwin_handle_key` refuses a window
     * that is not focused, which is right -- a keystroke belongs to whatever
     * is in front -- and it is the sort of thing a suite finds by a test doing
     * nothing and saying so.
     */
    recon_appwin_set_focused(win, true);
    recon_explorer_open_at(win, "/Users/Tester/Doomed");

    check_reports(win, "entries: 1", "one file to begin with");
    check_reports(win, "question: idle", "and nothing is being asked");

    /*
     * Select it, then press Delete.
     *
     * Down first because nothing is selected when a folder opens -- and a
     * Delete with no selection is a different path saying "select something
     * first", which would make this test pass while measuring that instead.
     */
    recon_appwin_handle_key(win, RECON_KEY_Down, 0);
    check_reports(win, "selected: [0] keepme.txt", "and it is selected");

    recon_appwin_handle_key(win, RECON_KEY_Delete, 0);

    /*
     * **Both halves in the same breath**, which is the whole reason this is a
     * suite and not a photograph. A picture of a dialog proves a dialog
     * appeared; it cannot prove the file was still there when it did.
     */
    check_reports(win, "asking: move to bin target 'keepme.txt'",
        "IT ASKS, AND NAMES WHAT IT IS ASKING ABOUT");

    size_t length = 0;
    char *still = recon_fs_read("/", "/Users/Tester/Doomed/keepme.txt",
        &length);
    check(still != NULL, "AND THE FILE IS STILL THERE WHILE IT ASKS");
    free(still);

    recon_appwin_destroy(win);
}

static void test_the_system_folder_is_refused_not_offered(void) {
    printf("the system folder is refused, not offered\n");

    /*
     * A protected entry gets **no question at all**, which is the right shape:
     * a dialog offering to delete something that cannot be deleted is a dialog
     * whose only honest button is Cancel.
     *
     * `path_is_protected` in `recon_fs.c` guards `RECON_DIR_SYSTEM` and
     * everything under it, and nothing else — see the note at the end of this
     * function for what that leaves unguarded.
     */
    struct recon_appwin *win = recon_explorer_create(fake_server(), NULL);
    if (win == NULL) {
        check(false, "a file manager to work with");
        return;
    }
    recon_appwin_show(win);
    recon_appwin_set_focused(win, true);
    recon_explorer_open_at(win, "/");

    /*
     * Walk to it rather than assuming where it is. The root's contents are
     * whatever `recon_fs_init` laid down, and a test that pressed Down a fixed
     * number of times would quietly select something else the day that list
     * changes — and then pass, because the thing it selected is deletable.
     */
    bool found = false;
    for (int i = 0; i < 32 && !found; i++) {
        recon_appwin_handle_key(win, RECON_KEY_Down, 0);
        found = reports(win, "] System");
    }

    check(found, "THE SYSTEM FOLDER IS THERE TO BE SELECTED");
    if (!found) {
        recon_appwin_destroy(win);
        return;
    }

    recon_appwin_handle_key(win, RECON_KEY_Delete, 0);

    check(!reports(win, "asking: move to bin"),
        "IT RAISES NO QUESTION ABOUT THE SYSTEM FOLDER");
    check_reports(win, "protected",
        "and says why, rather than saying nothing");

    /*
     * --- What this found, which is a question rather than a fault ---
     *
     * Only `/System` is protected. `/Users` is not, so a file manager opened
     * at the root will offer to move every account's files to the Recycle Bin
     * on one keystroke, and the bin is on the same volume.
     *
     * Deliberately not "fixed" here: whether the top-level folders should be
     * undeletable is a ruling about what a file manager is for, and the answer
     * that suits an administrator may not be the one that suits a workstation.
     * Recorded on the board instead. What is checked below is only that the
     * refusal that *does* exist is real.
     */
    recon_appwin_destroy(win);
}

static void test_a_folder_that_is_not_there(void) {
    printf("and being sent somewhere that does not exist\n");

    struct recon_appwin *win = recon_explorer_create(fake_server(), NULL);
    if (win == NULL) {
        check(false, "a file manager to work with");
        return;
    }
    recon_appwin_show(win);
    /*
     * Focused, or no key arrives. `recon_appwin_handle_key` refuses a window
     * that is not focused, which is right -- a keystroke belongs to whatever
     * is in front -- and it is the sort of thing a suite finds by a test doing
     * nothing and saying so.
     */
    recon_appwin_set_focused(win, true);

    recon_explorer_open_at(win, "/Users/Tester/Papers");
    recon_explorer_open_at(win, "/Users/Tester/NoSuchPlace");

    /*
     * Whatever it does, it must not be sitting in a folder it cannot read
     * while claiming to be there. Either it refused and stayed put, or it
     * went and found nothing -- both are answers; being somewhere that does
     * not exist is not.
     */
    check(reports(win, "cwd: /Users/Tester/Papers") ||
          reports(win, "entries: 0"),
        "it either stays where it was or arrives empty");

    recon_appwin_destroy(win);
}

int main(void) {
    printf("\n--- the file manager, with no compositor under it ---\n\n");

    char root[] = "/tmp/reconos-explorer-XXXXXX";
    if (mkdtemp(root) == NULL) {
        printf("could not make a test filesystem\n");
        return 1;
    }
    if (!recon_fs_init(root)) {
        printf("could not start the test filesystem\n");
        return 1;
    }
    recon_fs_mkdir("/", "/Users");
    recon_fs_mkdir("/", "/Users/Tester");

    test_it_lists_what_is_there();
    test_a_delete_asks_before_it_acts();
    test_the_system_folder_is_refused_not_offered();
    test_a_folder_that_is_not_there();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
