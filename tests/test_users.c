/*
 * Tests for accounts, passwords and what a limited account may do.
 *
 * The checks that matter most are the refusals. An account system that lets
 * people in is easy; one that keeps the right people out, and that cannot be
 * manoeuvred into a state with no administrator, is the part worth proving.
 *
 * Run with: ninja -C build && ./build/recon_users_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "recon_fs.h"
#include "recon_users.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
        printf("        accounts say:    %s\n", recon_users_last_error());
        printf("        filesystem says: %s\n", recon_fs_last_error());
    }
}

/* --- Tests --- */

static void test_creating(void) {
    printf("creating accounts\n");

    check(recon_users_count() == 0, "a new system has no accounts");

    check(recon_users_create("Joshua", "hunter2", RECON_ROLE_ADMINISTRATOR),
        "create an administrator");
    check(recon_users_count() == 1, "there is one account");

    struct recon_user user;
    check(recon_users_find("Joshua", &user), "it can be found");
    check(user.role == RECON_ROLE_ADMINISTRATOR, "and is an administrator");
    check(user.has_password, "and has a password");

    /* The folder comes with the account. One without is an account that half
     * exists. */
    check(recon_fs_exists("/", "/Users/Joshua/Documents"),
        "its folders were created");

    check(!recon_users_create("Joshua", "other", RECON_ROLE_LIMITED),
        "the same name twice is refused");
    check(!recon_users_create("Joshua ", "x", RECON_ROLE_LIMITED),
        "and so is the same name with a space on the end");

    check(recon_users_create("Guest", NULL, RECON_ROLE_LIMITED),
        "an account with no password");
    check(recon_users_find("Guest", &user) && !user.has_password,
        "which knows it has none");
}

static void test_names(void) {
    printf("what counts as a name\n");

    check(!recon_users_create("", "x", RECON_ROLE_LIMITED), "empty is refused");
    check(!recon_users_create("has/slash", "x", RECON_ROLE_LIMITED),
        "a slash is refused, since the name becomes a folder");
    check(!recon_users_create("has:colon", "x", RECON_ROLE_LIMITED),
        "a colon is refused, since it separates fields in the file");
    check(!recon_users_create(".hidden", "x", RECON_ROLE_LIMITED),
        "a leading dot is refused");
    check(!recon_users_create(" leading", "x", RECON_ROLE_LIMITED),
        "a leading space is refused");
}

static void test_passwords(void) {
    printf("passwords\n");

    check(recon_users_check("Joshua", "hunter2"), "the right password works");
    check(!recon_users_check("Joshua", "hunter3"), "a wrong one does not");
    check(!recon_users_check("Joshua", ""), "an empty one does not");
    check(!recon_users_check("Joshua", "HUNTER2"),
        "and neither does the right one in the wrong case");

    check(!recon_users_check("Nobody", "hunter2"),
        "an account that does not exist does not match");

    /* No password means anybody at the keyboard, which is a choice that
     * account made. */
    check(recon_users_check("Guest", ""), "an account with no password lets an "
        "empty attempt through");
    check(recon_users_check("Guest", NULL), "or none at all");

    check(recon_users_set_password("Joshua", "a better one"),
        "change a password");
    check(!recon_users_check("Joshua", "hunter2"), "the old one stops working");
    check(recon_users_check("Joshua", "a better one"), "the new one works");

    check(recon_users_set_password("Guest", "now protected"),
        "add a password to an account that had none");
    check(!recon_users_check("Guest", ""), "an empty attempt no longer works");
    check(recon_users_check("Guest", "now protected"), "the new one does");

    check(recon_users_set_password("Guest", NULL), "take it away again");
    check(recon_users_check("Guest", ""), "and it is open again");
}

static void test_signing_in(void) {
    printf("signing in\n");

    check(recon_users_current() == NULL, "nobody is signed in to begin with");

    check(!recon_users_login("Joshua", "wrong"), "a wrong password is refused");
    check(recon_users_current() == NULL, "and nobody is signed in after it");

    check(recon_users_login("Joshua", "a better one"), "the right one works");
    check(recon_users_current() != NULL &&
        strcmp(recon_users_current(), "Joshua") == 0, "and says who");
    check(recon_users_current_is_admin(), "who is an administrator");

    /* The filesystem follows the account, or "the user's folder" means the
     * wrong folder. */
    check(strcmp(recon_fs_current_user(), "Joshua") == 0,
        "the filesystem knows too");
    check(strcmp(recon_fs_user_dir(NULL), "/Users/Joshua") == 0,
        "and the user's folder is theirs");

    recon_users_logout();
    check(recon_users_current() == NULL, "signing out clears it");
}

static void test_what_limited_means(void) {
    printf("what a limited account may do\n");

    check(recon_users_create("Sam", "sam", RECON_ROLE_LIMITED),
        "create a limited account");

    /*
     * Created now, while nobody is signed in. A limited account cannot create
     * accounts -- it would have to write the account list, which lives in
     * /System -- so making this one later would be asking for a refusal and
     * calling it a failure.
     */
    check(recon_users_create("Sammy", "s", RECON_ROLE_LIMITED),
        "an account whose name starts with another's");
    check(recon_users_login("Sam", "sam"), "sign in as it");
    check(!recon_users_current_is_admin(), "it is not an administrator");
    check(!recon_fs_user_is_administrator(), "and the filesystem agrees");

    /* Its own folder is its own. */
    check(recon_fs_write("/", "/Users/Sam/Documents/mine.txt", "x", 1),
        "it can write in its own folder");

    /*
     * These are the refusals that make the role mean something. Without them
     * "limited" is a word in a list.
     */
    check(!recon_fs_write("/", "/System/Config/hack", "x", 1),
        "it cannot write into /System");
    check(!recon_fs_mkdir("/", "/System/Evil"),
        "it cannot create a folder in /System");
    check(!recon_fs_remove("/", "/System/Icons/folder.ico"),
        "it cannot delete a system file");
    check(!recon_fs_remove_tree("/", "/System/Icons"),
        "nor a system folder");

    check(!recon_fs_write("/", "/Users/Joshua/Documents/theirs.txt", "x", 1),
        "it cannot write into another account's folder");
    check(!recon_fs_mkdir("/", "/Users/Joshua/Intruder"),
        "nor create anything there");

    /* Segment by segment, so one name being a prefix of another does not
     * hand over the wrong folder. */
    check(!recon_fs_write("/", "/Users/Sammy/Documents/x.txt", "x", 1),
        "Sam cannot write into Sammy's folder");

    /* Nor create accounts, which is the check that the account list being in
     * /System actually protects it. */
    check(!recon_users_create("Sneaky", "x", RECON_ROLE_ADMINISTRATOR),
        "a limited account cannot create an administrator");

    /* And the reason to refuse can be asked for, so a menu entry can be
     * greyed out rather than failing when clicked. */
    check(recon_fs_refusal("/", "/System/Config/x") != NULL,
        "the refusal can be asked about beforehand");
    check(recon_fs_refusal("/", "/Users/Sam/Documents/x") == NULL,
        "and its own folder is not refused");

    recon_users_logout();
}

static void test_administrator_is_not_limited(void) {
    printf("what an administrator may do\n");

    check(recon_users_login("Joshua", "a better one"), "sign in as one");
    check(recon_fs_write("/", "/System/Config/allowed", "x", 1),
        "it can write into /System");
    check(recon_fs_write("/", "/Users/Sam/Documents/note.txt", "x", 1),
        "and into another account's folder");
    check(recon_fs_remove("/", "/System/Config/allowed"),
        "and remove what it wrote, which a limited account could not");

    /*
     * But not the layout. Not a permission: an administrator deleting
     * /System/Icons has not exercised authority, they have broken their
     * computer, and there is no state ReconOS knows how to be in without it.
     */
    check(!recon_fs_remove_tree("/", "/System/Icons"),
        "and not the directories the layout is made of");
    check(!recon_fs_remove_tree("/", "/Users"), "nor /Users");
    check(!recon_fs_remove_tree("/", "/"), "nor the root");
}

static void test_the_last_administrator(void) {
    printf("the last administrator\n");

    check(recon_users_admin_count() == 1, "there is one administrator");

    /*
     * A system nobody can administer cannot be repaired from inside it. These
     * two refusals are the ones that stop somebody locking themselves out with
     * a single click.
     */
    check(!recon_users_set_role("Joshua", RECON_ROLE_LIMITED),
        "the only administrator cannot demote itself");
    check(!recon_users_remove("Joshua"),
        "and cannot be removed");

    check(recon_users_set_role("Sam", RECON_ROLE_ADMINISTRATOR),
        "promote a second one");
    check(recon_users_admin_count() == 2, "now there are two");
    check(recon_users_set_role("Joshua", RECON_ROLE_LIMITED),
        "and now the first can be demoted");

    check(recon_users_set_role("Joshua", RECON_ROLE_ADMINISTRATOR),
        "put it back");
}

static void test_removing(void) {
    printf("removing accounts\n");

    check(recon_users_login("Joshua", "a better one"), "sign in");
    check(!recon_users_remove("Joshua"),
        "the account signed in cannot be removed");

    check(recon_users_remove("Sammy"), "another one can");
    check(!recon_users_find("Sammy", NULL), "and is gone");

    /* Removing a login is not a decision to destroy somebody's documents. */
    check(recon_fs_exists("/", "/Users/Sammy"),
        "its files are left alone");

    recon_users_logout();
}

/* Everything above proves the copy in memory works. This proves the accounts
 * are still there after a restart, which is the entire point. */
static void test_persistence(void) {
    printf("surviving a restart\n");

    int before = recon_users_count();

    recon_users_finish();
    check(recon_users_init(), "read the accounts again");
    check(recon_users_count() == before, "the same accounts are there");

    struct recon_user user;
    check(recon_users_find("Joshua", &user), "an account survived");
    check(user.role == RECON_ROLE_ADMINISTRATOR, "with its role");
    check(recon_users_check("Joshua", "a better one"),
        "and its password still matches");
    check(!recon_users_check("Joshua", "a better on"),
        "while a near miss still does not");

    check(recon_users_find("Guest", &user) && !user.has_password,
        "an account with no password survived as one");
}

static void test_damaged_file(void) {
    printf("an account file that is not quite right\n");

    const char *text =
        "# a comment\n"
        "\n"
        "Good:administrator:0::\n"
        "this line has no colons\n"
        ":limited:0::\n"
        "Halfway:limited:120000:notvalidhex:alsonot\n";

    check(recon_fs_write("/", RECON_USERS_FILE, text, strlen(text)),
        "write a damaged file");

    recon_users_finish();
    check(recon_users_init(), "read it");

    check(recon_users_count() == 2, "the two usable lines were kept");
    check(recon_users_find("Good", NULL), "the good account is there");

    /*
     * The important one. A password whose salt or hash will not parse is
     * treated as no password rather than as one that can never match --
     * otherwise a damaged file locks somebody out of their own machine
     * permanently, which is a worse outcome than an account being open.
     */
    struct recon_user user;
    check(recon_users_find("Halfway", &user), "the half-written one is there");
    check(!user.has_password,
        "and is treated as having no password rather than an unmatchable one");
}

int main(void) {
    char root[] = "/tmp/reconos-users-XXXXXX";
    if (mkdtemp(root) == NULL) {
        perror("mkdtemp");
        return 1;
    }

    printf("ReconOS account tests, root %s\n\n", root);

    if (!recon_fs_init(root)) {
        printf("could not set up the test filesystem: %s\n", recon_fs_last_error());
        return 1;
    }
    if (!recon_users_init()) {
        printf("could not read the accounts\n");
        return 1;
    }

    test_creating();
    test_names();
    test_passwords();
    test_signing_in();
    test_what_limited_means();
    test_administrator_is_not_limited();
    test_the_last_administrator();
    test_removing();
    test_persistence();
    test_damaged_file();

    recon_users_finish();
    recon_fs_finish();

    char command[512];
    snprintf(command, sizeof(command), "rm -rf '%s'", root);
    if (system(command) != 0) {
        printf("\nnote: could not remove %s\n", root);
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
