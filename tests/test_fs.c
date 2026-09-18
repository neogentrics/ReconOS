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

#include "recon_error.h"
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

/*
 * One account's folder is not another's to read.
 *
 * Writing to somebody else's files was refused from the start; reading them
 * was not, so a limited account could open every document another account
 * had. Being unable to change someone's files is worth very little if you can
 * still read them, so this is checked rather than assumed.
 */
static void test_reading_across_accounts(void) {
    printf("what one account may read of another\n");

    check(recon_fs_create_user("Ada"), "an account exists");
    check(recon_fs_create_user("Grace"), "and another");
    check(recon_fs_write("/", "/Users/Ada/Documents/notes.txt", "secret", 6),
        "one of them has a document");

    struct recon_dirent entries[16];

    /* An administrator may look everywhere: that is what the role is for. */
    recon_fs_set_user("Grace", true);
    check(recon_fs_list("/", "/Users/Ada", entries, 16) >= 0,
        "an administrator may list another account's folder");
    check(file_says("/Users/Ada/Documents/notes.txt", "secret"),
        "and may read what is in it");

    /* A limited account may not. */
    recon_fs_set_user("Grace", false);
    check(recon_fs_list("/", "/Users/Ada", entries, 16) < 0,
        "a limited account may not list another's folder");
    check(recon_fs_read("/", "/Users/Ada/Documents/notes.txt", NULL) == NULL,
        "nor read a file inside it");

    /* Its own is still its own. */
    check(recon_fs_list("/", "/Users/Grace", entries, 16) >= 0,
        "but may still list its own");

    /*
     * The system's files stay readable. A limited account may look at how the
     * machine is set up; it just may not change it. Refusing the read as well
     * would leave an account unable to load its own settings.
     */
    check(recon_fs_list("/", "/System", entries, 16) >= 0,
        "and may still read the system's own folder");

    /* A name that merely starts with another's is a different account. */
    check(recon_fs_create_user("Adamant"), "an account whose name starts the same");
    recon_fs_set_user("Adamant", false);
    check(recon_fs_list("/", "/Users/Adamant", entries, 16) >= 0,
        "'Adamant' is not shut out of its own folder by 'Ada'");

    recon_fs_set_user(NULL, true);
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

/*
 * Every shape of escape anybody has ever written, tried against every call
 * that takes a path.
 *
 * The four cases above are the ones somebody thinks of. These are the ones
 * that get past a check written by somebody who thought of those four: a `..`
 * that arrives after enough real segments to look harmless, doubled slashes,
 * a `.` between the dots, a path that is nothing but separators, and the
 * absolute form of each.
 *
 * The property is one sentence and it is worth stating as one: **no path
 * reaches a file outside the ReconOS root.** Not "is refused" -- some of these
 * are perfectly legal paths that stay inside -- but that whatever they name is
 * under the root.
 */
static void test_every_shape_of_escape(void) {
    printf("escaping the root, exhaustively\n");

    static const char *OUT[] = {
        "..",
        "../",
        "/..",
        "/../",
        "../..",
        "/../../",
        "../../../../../../../../etc/passwd",
        "/../etc/passwd",
        "//../etc",
        "/.././../etc",
        "/./../etc",
        "/Temp/../../etc",
        "/Temp/../..",
        "/Temp/./../../etc/passwd",
        "/Users/../../etc",
        "Temp/../../outside",
        "./../outside",
        ".././outside",
        "/Temp/..%2f..%2fetc",   /* not decoded here, so it is a NAME */
        "/Temp/....//../etc",
    };

    int escaped = 0;
    for (size_t i = 0; i < sizeof(OUT) / sizeof(OUT[0]); i++) {
        char host[RECON_PATH_MAX];
        char canonical[RECON_PATH_MAX];

        /* From two different working directories, because a relative path is
         * resolved against one and an escape that needs depth to work would
         * only show from the deeper one. */
        static const char *FROM[] = { "/", "/Temp" };
        for (size_t f = 0; f < sizeof(FROM) / sizeof(FROM[0]); f++) {
            if (!recon_fs_resolve(FROM[f], OUT[i], host, sizeof(host),
                    canonical, sizeof(canonical))) {
                continue;   /* refused, which is one right answer */
            }
            /*
             * Allowed, so it must have landed inside. The host path has to
             * begin with the root and the next character has to be a
             * separator or the end -- "/tmp/recon-x" and "/tmp/recon-xyz" both
             * start with the same eleven characters, and only one of them is
             * inside.
             */
            const char *here = recon_fs_host_root();
            size_t len = strlen(here);
            bool inside = strncmp(host, here, len) == 0 &&
                (host[len] == '\0' || host[len] == '/');
            if (!inside) {
                escaped++;
                printf("        '%s' from '%s' resolved to %s\n",
                    OUT[i], FROM[f], host);
            }
        }
    }

    check(escaped == 0,
        "NO PATH REACHES A FILE OUTSIDE THE ROOT, however it is written");

    /*
     * And the calls that take a path, rather than the resolver alone: a
     * resolver that is right and a caller that does not use it is the same
     * hole one layer up.
     *
     * A write that *succeeds* is not the failure. Two of the paths above are
     * perfectly ordinary names -- `..%2f..%2fetc` is not decoded here, so it
     * is one filename with per-cent signs in it, and `....//../etc`
     * normalises to `/Temp/etc`. Both stay inside and both should work. What
     * counts is where the bytes ended up, which is why this asks the resolver
     * where that was rather than assuming a refusal.
     *
     * The first version of this counted every successful write and reported
     * two failures that were the test being wrong.
     */
    const char *root = recon_fs_host_root();
    size_t root_len = strlen(root);
    int leaked = 0;
    for (size_t i = 0; i < sizeof(OUT) / sizeof(OUT[0]); i++) {
        if (!recon_fs_write("/", OUT[i], "x", 1)) {
            continue;
        }
        char host[RECON_PATH_MAX];
        if (!recon_fs_resolve("/", OUT[i], host, sizeof(host), NULL, 0) ||
                strncmp(host, root, root_len) != 0 ||
                (host[root_len] != '\0' && host[root_len] != '/')) {
            leaked++;
            printf("        wrote outside through '%s'\n", OUT[i]);
        }
    }
    check(leaked == 0, "and nothing lands outside through one");

    /* A path that leaves and comes back is still allowed, because it stays
     * inside -- the point is not to refuse `..`, it is to refuse leaving. */
    check(recon_fs_mkdir("/Temp", "../Temp/there-and-back"),
        "and a path that leaves and returns still works");
}

static void test_a_link_out_of_the_tree(void) {
    printf("a symbolic link pointing outside\n");

    /*
     * The one escape the textual check cannot see.
     *
     * normalize() splits on '/', drops '.', pops on '..' and refuses to go
     * below the root. That is complete against anything *written* in a path.
     * It says nothing about what the resulting name turns out to be on the
     * host: a link inside the tree pointing at /etc is a path that normalises
     * perfectly and lands outside.
     *
     * ReconOS has no way to make one -- there is no command for it -- but the
     * host root is an ordinary directory somebody can copy files into, and
     * `cp -a` of a tree that has one brings it along. So this makes one the
     * way it would arrive and asks what happens.
     */
    char host[RECON_PATH_MAX];
    if (!recon_fs_resolve("/", "/Temp", host, sizeof(host), NULL, 0)) {
        check(false, "could not find somewhere to put a link");
        return;
    }

    char link[RECON_PATH_MAX + 16];
    snprintf(link, sizeof(link), "%s/way-out", host);
    unlink(link);

    /* Pointed at something that exists on every machine this runs on and that
     * ReconOS has no business reading. */
    if (symlink("/etc/hostname", link) != 0) {
        printf("        (this host will not make a link; skipped)\n");
        return;
    }

    size_t size = 0;
    char *through = recon_fs_read("/", "/Temp/way-out", &size);
    bool followed = (through != NULL && size > 0);
    if (through != NULL) {
        free(through);
    }

    if (followed) {
        printf("        it read %zu bytes of /etc/hostname\n", size);
    }
    check(!followed,
        "A LINK POINTING OUT OF THE TREE DOES NOT READ WHAT IT POINTS AT");

    /*
     * And it is written down, because this one is worth counting.
     *
     * A `..` too many is an ordinary mistake and is deliberately not recorded
     * -- this function runs on every file operation, and a line per attempt is
     * a log somebody can fill on purpose. A name that resolves outside is a
     * different thing: nobody types one by accident.
     */
    size_t log_size = 0;
    char *log = recon_fs_read("/", RECON_ERROR_LOG, &log_size);
    check(log != NULL && strstr(log, "B003") != NULL,
        "and VT-B003 is in the log");
    if (log != NULL) {
        free(log);
    }

    unlink(link);
}

static void test_a_common_mistake_is_not_logged(void) {
    printf("what is refused and not recorded\n");

    recon_fs_remove("/", RECON_ERROR_LOG);

    /* Fifty ordinary escapes, the kind a relative path produces by accident. */
    for (int i = 0; i < 50; i++) {
        recon_fs_exists("/", "../../outside");
    }

    size_t size = 0;
    char *log = recon_fs_read("/", RECON_ERROR_LOG, &size);
    if (log != NULL) {
        printf("        the log grew to %zu bytes\n", size);
        free(log);
    }
    check(log == NULL,
        "FIFTY REFUSED `..` PATHS WRITE NOTHING -- a log somebody can fill is "
        "a log that has lost the entry worth finding");
}

static void test_reporting_cannot_report(void) {
    printf("a fault raised while reporting one\n");

    /*
     * The guard, exercised rather than trusted.
     *
     * Recording a fault writes through the filesystem, and the filesystem is
     * a place faults come from, so a raise can reach a raise. Without a guard
     * that is unbounded and the last thing the machine does is lose the record
     * of what went wrong.
     *
     * There is no way to make the log write fail from here, so what this
     * proves is the cheaper half: raising while a raise is in progress does
     * not recurse and does not lose the outer one. A recursion would not
     * return.
     */
    recon_fs_remove("/", RECON_ERROR_LOG);
    recon_error_raise(NULL, RECON_ERR_B003, "the outer one");

    size_t size = 0;
    char *log = recon_fs_read("/", RECON_ERROR_LOG, &size);
    check(log != NULL && strstr(log, "the outer one") != NULL,
        "a fault raised on its own is recorded and returns");
    if (log != NULL) {
        free(log);
    }
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

/* --- Files that are private from the moment they exist --- */

/*
 * The claim recon_fs_write_private makes is not "ends up private" -- a chmod
 * after the write does that too. It is "was never anything else", and the
 * difference is a window during which a key is readable by everything on the
 * machine.
 *
 * A test cannot watch that window from inside one process. What it can do is
 * check the two things that produce it: the mode is right, and it is right
 * even when the file already existed with a looser one -- which is the case a
 * write-then-chmod gets wrong in the other direction, by truncating a file and
 * keeping whatever permissions it had.
 */
static void test_private_files(void) {
    printf("Files that start out private\n");

    const char *secret = "-----BEGIN PRIVATE KEY-----\nnot really\n";

    check(recon_fs_write_private("/", "/System/key.pem", secret,
        strlen(secret)), "a private file writes");

    char host[RECON_PATH_MAX];
    char canonical[RECON_PATH_MAX];
    check(recon_fs_resolve("/", "/System/key.pem", host, sizeof(host),
        canonical, sizeof(canonical)), "and can be found on the host");

    struct stat info;
    check(stat(host, &info) == 0, "and exists");
    check((info.st_mode & 0777) == 0600,
        "readable and writable by its owner, and by nobody else");

    /* The contents survived all that. */
    size_t size = 0;
    char *back = recon_fs_read("/", "/System/key.pem", &size);
    check(back != NULL && size == strlen(secret) &&
        memcmp(back, secret, size) == 0, "and holds what was written");
    free(back);

    /*
     * Written over a file that was already there and already loose.
     *
     * This is the case that separates the two approaches. Truncating an
     * existing file keeps its permissions, so a key written by an older
     * version at 0644 would stay at 0644 forever -- and the promise here is
     * that it does not.
     */
    check(recon_fs_write("/", "/System/loose.pem", "old", 3),
        "an ordinary file is written first");
    check(recon_fs_resolve("/", "/System/loose.pem", host, sizeof(host),
        canonical, sizeof(canonical)), "and found");
    check(chmod(host, 0644) == 0, "and deliberately made world-readable");

    check(recon_fs_write_private("/", "/System/loose.pem", secret,
        strlen(secret)), "then written privately over the top");
    check(stat(host, &info) == 0 && (info.st_mode & 0777) == 0600,
        "and it is private now, not still world-readable");

    /* An ordinary write is NOT private, which is the point of there being two
     * functions rather than one that quietly tightened everything. */
    check(recon_fs_write("/", "/System/plain.txt", "hello", 5),
        "an ordinary file still writes");
    check(recon_fs_resolve("/", "/System/plain.txt", host, sizeof(host),
        canonical, sizeof(canonical)) && stat(host, &info) == 0,
        "and is found");
    check((info.st_mode & 0077) != 0 || (info.st_mode & 0777) == 0600,
        "and is left to the umask rather than forced private");
}

/*
 * The first few bytes of a file, without reading the file.
 *
 * What a file *is* is decided by its first few bytes, and `recon_fs_read`
 * would pull an entire film into memory to answer that. The header is explicit
 * that it *"does not terminate the buffer and makes no claim about what is in
 * it -- a file's first bytes are bytes, and treating them as a string is how a
 * NUL in the middle of a PNG turns into a truncated read nobody notices"*, so
 * that is what this holds it to: a file with a NUL in the middle comes back
 * whole.
 *
 * It was the last function in recon_fs.c that no suite ran at all.
 */
static void test_reading_the_front_of_a_file(void) {
    printf("the first bytes of a file, without reading the file\n");

    /* Eight bytes with a NUL in the middle, which is what a real header looks
     * like and what a string-shaped reader stops at. */
    static const char HEADER[] = { 'R', 'E', 'C', 0, 'O', 'N', 'O', 'S' };

    /* Its own folder: nothing makes this one, and a test that assumed an
     * earlier one had left it behind would fail for a reason that has nothing
     * to do with reading the front of a file. */
    recon_fs_mkdir("/", "/Users/Documents");
    check(recon_fs_write("/", "/Users/Documents/thing.bin", HEADER,
            sizeof(HEADER)),
        "there is a file to read the front of");

    unsigned char head[4];
    memset(head, 0xAA, sizeof(head));

    size_t had = recon_fs_read_head("/", "/Users/Documents/thing.bin",
        head, sizeof(head));

    check(had == sizeof(head), "it reads as many as were asked for");
    check(memcmp(head, HEADER, sizeof(head)) == 0,
        "and they are the file's own bytes, NUL and all");

    /* Asking for more than there is gives what there is. */
    unsigned char all[64];
    had = recon_fs_read_head("/", "/Users/Documents/thing.bin", all,
        sizeof(all));
    check(had == sizeof(HEADER), "asking for more than there is gives the lot");

    /* And a file that is not there is nothing, not a failure to notice. */
    check(recon_fs_read_head("/", "/Users/Documents/nothing.bin", all,
            sizeof(all)) == 0,
        "a file that is not there reads as nothing");
}

/*
 * The host directory this suite was given, so the checks below can look for
 * it in a message. Set once by main, before anything runs.
 */
static const char *g_host_root_for_test = "";

static void test_an_error_names_a_reconos_path(void) {
    printf("an error message says where ReconOS thinks it is\n");

    /*
     * **The host must not show through.** `recon_fs.c` opens with the rule
     * this holds: *"ReconOS is the only thing on screen and Linux is
     * underneath it"* -- and the comment on `guest_path` records the day it
     * broke, when deleting something that was not there reported
     * `cannot read '/tmp/lookroot/Users/Joshua/Documents/x'`.
     *
     * That path exists on no ReconOS machine. It is also invisible to every
     * check that looks at a return value, because the call failed exactly as
     * it should have -- only the sentence was wrong.
     *
     * **This has to go through a call that handles a host path.** The first
     * version of this test asked `recon_fs_remove` about a file that was not
     * there, which reports with the ReconOS path it was handed and never
     * reaches `guest_path` at all: the check passed, and a mutation making
     * `guest_path` return the host path unchanged did not move it. A test
     * against a path you assumed proves nothing.
     *
     * A copy whose source cannot be opened does reach it, and is the one such
     * failure a test can arrange without privileges.
     */
    const char *body = "something";
    check(recon_fs_write("/", "/Users/unreadable.txt", body, 9),
        "a file is written");

    char host[RECON_PATH_MAX];
    check(recon_fs_resolve("/", "/Users/unreadable.txt", host, sizeof(host),
        NULL, 0), "and can be found on the host");

    if (chmod(host, 0) != 0) {
        /*
         * Running as root, most likely, where nothing is unreadable. Said out
         * loud rather than passed: a check that quietly does not run is worse
         * than one that is not there.
         */
        printf("        (skipped: this account can read anything)\n");
        recon_fs_remove("/", "/Users/unreadable.txt");
        return;
    }

    check(!recon_fs_copy("/", "/Users/unreadable.txt", "/Users/copy.txt"),
        "copying a file that cannot be opened fails");

    const char *said = recon_fs_last_error();
    check(said != NULL && said[0] != '\0', "and says something");

    if (said != NULL) {
        /*
         * The suite's root is a temporary directory under /tmp, so its name is
         * the host's fingerprint. Any part of it in the message means the host
         * has shown through.
         */
        check(strstr(said, "/tmp/") == NULL,
            "and the message names no host directory");
        check(g_host_root_for_test[0] == '\0' ||
                strstr(said, g_host_root_for_test) == NULL,
            "and none of the root this suite was given");
        check(strstr(said, "/Users/unreadable.txt") != NULL,
            "and it does name the ReconOS path");

        if (strstr(said, "/tmp/") != NULL) {
            printf("        it said: %s\n", said);
        }
    }

    chmod(host, 0600);
    recon_fs_remove("/", "/Users/unreadable.txt");
    recon_fs_remove("/", "/Users/copy.txt");
}

static void test_the_three_volumes(void) {
    printf("what each space is called, and where it starts\n");

    /*
     * Three, and the split is not cosmetic -- the header says why: *"deleting
     * a program and deleting a system file are not the same act, and one bin
     * holding both makes emptying it a decision nobody can make safely."*
     */
    check(RECON_VOLUME_COUNT == 3, "there are three spaces");

    const struct {
        enum recon_volume volume;
        const char *root;
    } EXPECTED[] = {
        { RECON_VOLUME_SYSTEM,   "/System" },
        { RECON_VOLUME_PROGRAMS, "/Apps" },
        { RECON_VOLUME_USER,     "/Users" },
    };

    for (size_t i = 0; i < sizeof(EXPECTED) / sizeof(EXPECTED[0]); i++) {
        const char *name = recon_volume_name(EXPECTED[i].volume);
        const char *root = recon_volume_root(EXPECTED[i].volume);
        const char *detail = recon_volume_detail(EXPECTED[i].volume);

        check(name != NULL && name[0] != '\0', "it has a name");
        check(detail != NULL && detail[0] != '\0',
            "and a line saying what lives there");
        check(root != NULL && strcmp(root, EXPECTED[i].root) == 0,
            "and its root is where the header says");
        if (root != NULL && strcmp(root, EXPECTED[i].root) != 0) {
            printf("        wanted %s, got %s\n", EXPECTED[i].root, root);
        }
    }

    /*
     * And a volume that is not one.
     *
     * Asked because these take an enum and C will hand them any int. A page
     * drawing a storage list off a loop that ran one too far would otherwise
     * read past the table -- which is a crash on a good day and somebody
     * else's string on a bad one.
     */
    check(recon_volume_name(RECON_VOLUME_COUNT) != NULL,
        "a volume that does not exist still answers with something");
    check(recon_volume_root(RECON_VOLUME_COUNT) != NULL,
        "and so does its root");
    check(recon_volume_detail(RECON_VOLUME_COUNT) != NULL,
        "and its detail");
    check(recon_volume_name((enum recon_volume)-1) != NULL,
        "and so does one below the start");

    /*
     * And usage *refuses* it rather than answering, which is the difference
     * that matters: the three above return a fixed string for a volume that
     * does not exist, and this one would index a table with it.
     *
     * Added because a mutation found it missing -- removing the upper bound
     * from `recon_volume_usage` failed nothing, because every check here
     * asked the other three.
     */
    unsigned long long nowhere_bytes = 1234;
    int nowhere_files = 5678;
    check(!recon_volume_usage(RECON_VOLUME_COUNT, &nowhere_bytes,
        &nowhere_files), "measuring a volume that does not exist is refused");
    check(!recon_volume_usage((enum recon_volume)-1, NULL, NULL),
        "and so is one below the start");
}

static void test_what_is_in_a_volume(void) {
    printf("how much is in a space, walked rather than remembered\n");

    /* Something of a known size, in a known place. */
    const char *body = "0123456789";
    check(recon_fs_write("/", "/Users/counted.txt", body, 10),
        "a file is written into the user space");

    unsigned long long bytes = 0;
    int files = 0;
    check(recon_volume_usage(RECON_VOLUME_USER, &bytes, &files),
        "the user space can be measured");
    check(files >= 1, "and it holds at least the file just written");
    check(bytes >= 10, "and at least its bytes");

    /*
     * Adding to it moves both numbers. **Walked rather than cached**, which
     * the header says and which is the property worth holding: a cached total
     * is right until something writes a file, and nothing here would notice.
     */
    unsigned long long before = bytes;
    int files_before = files;
    check(recon_fs_write("/", "/Users/counted2.txt", body, 10),
        "a second file is written");
    check(recon_volume_usage(RECON_VOLUME_USER, &bytes, &files),
        "and it is measured again");
    check(files == files_before + 1, "one more file");
    check(bytes == before + 10, "and ten more bytes");

    /* Either answer may be left out by a caller that wants only the other. */
    check(recon_volume_usage(RECON_VOLUME_USER, NULL, NULL),
        "a caller may ask for neither number");

    unsigned long long only_bytes = 0;
    check(recon_volume_usage(RECON_VOLUME_USER, &only_bytes, NULL),
        "or for just the bytes");
    check(only_bytes == bytes, "which is the same answer");

    recon_fs_remove("/", "/Users/counted.txt");
    recon_fs_remove("/", "/Users/counted2.txt");
}

/* Where the test filesystem lives on the host, for the one check
 * that has to reach round the back of it and change a permission. */
static const char *g_root;

/* How many lines the error log has, or 0 when there is none. */
static int logged_faults(void)
{
	size_t size = 0;
	char *text = recon_fs_read("/", RECON_ERROR_LOG, &size);

	if (text == NULL) {
		return 0;
	}

	int lines = 0;

	for (size_t i = 0; i < size; i++) {
		if (text[i] == '\n') {
			lines++;
		}
	}
	free(text);
	return lines;
}

static void test_a_missing_file_is_not_a_fault(void)
{
	printf("reading a file that is not there\n");

	/*
	 * --- What this is really about ---
	 *
	 * Looking for a file that may not be there is how a *search* works. The
	 * icon resolver tries `Metallic/x.ico`, `Metallic/x.png`, `x.ico` and
	 * `x.png`, and finding the fourth means three misses -- every time, for
	 * working correctly.
	 *
	 * VT-B001 used to be raised for all of them. A live desktop with four
	 * applications opened had **103 faults** in its error log, every one an
	 * icon that was looked for and not found. A log that fills with normal
	 * operation is a log nobody reads.
	 *
	 * The comment beside the raise claimed a guard: *"the file is there --
	 * resolve and readable both said so."* Neither said so.
	 * `recon_fs_resolve` works on paths that do not exist yet, because a
	 * write has to create one, and `readable` answers a question about
	 * permission -- returning true for an administrator without looking at
	 * the disk at all.
	 */
	int before = logged_faults();

	check(recon_fs_read("/", "/System/no-such-file-anywhere", NULL) == NULL,
	      "a file that is not there does not read");
	check(logged_faults() == before,
	      "AND NOTHING WAS LOGGED -- a missing file is the caller's business "
	      "and a NULL is how it is told");

	/* Several, because the fault this replaces was per-attempt. */
	recon_fs_read("/", "/System/Icons/nothing.ico", NULL);
	recon_fs_read("/", "/System/Icons/nothing.png", NULL);
	recon_fs_read("/", "/System/Icons/Metallic/nothing.png", NULL);
	check(logged_faults() == before,
	      "and a search that misses four times logs nothing four times");
}

static void test_a_real_failure_is_still_a_fault(void)
{
	printf("a file that is there and will not read\n");

	/*
	 * Narrowing a check to fire less often is exactly the change that can
	 * quietly turn it off altogether, so the other half is checked too.
	 *
	 * --- Arranging a failure that is not "missing" ---
	 *
	 * A directory was the first attempt and it does not reach this: `fseek`
	 * to the end of one gives a length nothing can allocate, so it comes out
	 * as "out of memory" down a different path.
	 *
	 * A file the host will not open is the real thing. `chmod 000` does it,
	 * and only for somebody who is not root -- root opens it anyway. Which
	 * is checked rather than assumed: a suite run as root would otherwise
	 * report that B-001 still fires when what happened is that the file
	 * opened.
	 */
	if (geteuid() == 0) {
		printf("    (skipped: running as root, which can open anything)\n");
		return;
	}

	int before = logged_faults();

	check(recon_fs_write("/", "/System/unreadable.txt", "x", 1),
	      "a file to make unreadable");

	char host[512];

	snprintf(host, sizeof(host), "%s/System/unreadable.txt", g_root);
	check(chmod(host, 0) == 0, "and the host refuses it to us");

	check(recon_fs_read("/", "/System/unreadable.txt", NULL) == NULL,
	      "it does not read");
	check(logged_faults() > before,
	      "AND IT WAS LOGGED -- B-001 still says what it is for, which is a "
	      "file that is there and will not open");

	chmod(host, 0600);
	recon_fs_remove("/", "/System/unreadable.txt");
}

int main(void) {
    char root[] = "/tmp/reconos-test-XXXXXX";

    if (mkdtemp(root) == NULL) {
        perror("mkdtemp");
        return 1;
    }

    printf("ReconOS filesystem tests, root %s\n\n", root);

    g_host_root_for_test = root;

    g_root = root;

    if (!recon_fs_init(root)) {
        printf("could not set up the test filesystem: %s\n", recon_fs_last_error());
        return 1;
    }

    test_an_error_names_a_reconos_path();
    test_the_three_volumes();
    test_what_is_in_a_volume();
    test_unique_names();
    test_rename();
    test_copy();
    test_delete();
    test_protection();
    test_reading_across_accounts();
    test_escapes();
    test_every_shape_of_escape();
    test_a_link_out_of_the_tree();
    test_a_common_mistake_is_not_logged();
    test_reporting_cannot_report();
    test_clipboard();
    test_private_files();
    test_reading_the_front_of_a_file();
    test_a_missing_file_is_not_a_fault();
    test_a_real_failure_is_still_a_fault();

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
