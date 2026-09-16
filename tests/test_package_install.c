/*
 * What installing, upgrading and removing a package actually does.
 *
 * `tests/test_package_sign.c` covers the signature -- that it reaches every
 * file a package brings rather than only the manifest naming them. Its own
 * opening paragraph says why it was written: *"There was no suite for packages
 * at all, which meant the install path -- the one that takes somebody else's
 * shared object and loads it into this process -- was the least tested thing
 * in the system."*
 *
 * That was half answered. This is the other half: **21 checks on reading a
 * manifest and 0 on doing anything with it.**
 *
 * --- What is worth holding this code to ---
 *
 * `include/recon_package.h` is unusually specific about behaviour, and every
 * claim in it is a decision somebody could reverse by accident:
 *
 *   - install refuses an unsigned package, and there is no way to say
 *     "anyway" -- *"a check that can be turned off is a check that is off on
 *     the day it matters"*
 *   - a file already on disk is left alone **and not recorded**, so removing
 *     this package cannot take away something that belonged to another
 *   - a setting that already has a value is somebody's choice and is not
 *     overwritten, and only what was written goes in the receipt
 *   - an upgrade takes strictly newer and refuses same, older and
 *     unreadable -- *"'probably newer' is not a property to move somebody's
 *     files on"*
 *   - an upgrade that fails **puts the old files back**, because
 *     uninstall-then-install leaves somebody who wanted a newer version with
 *     no version
 *
 * Each of those is silent when it breaks. Nothing crashes if an install
 * quietly overwrites a wallpaper somebody chose, or if a failed upgrade takes
 * a working program away -- the failure is that somebody's machine is wrong
 * afterwards, which no other test in this tree would notice.
 *
 * --- Why this can run without a display or a kernel ---
 *
 * `recon_package_install` copies files, places them, writes settings and
 * writes a receipt. The only step that needs anything else is
 * `recon_modules_load` at the very end, and that is reached only by a package
 * that declares a module. A package of *content* -- a wallpaper pack, a set of
 * skins -- is a real thing the manifest supports, and it exercises everything
 * up to that line.
 *
 * The module path is still tested, from the other side: a package whose module
 * is not loadable must leave **nothing** behind, and that is the rollback this
 * file cares most about.
 *
 * Run with: ./build/recon_package_install_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_fs.h"
#include "recon_icons.h"
#include "recon_modules.h"
#include "recon_package.h"
#include "recon_registry.h"
#include "recon_sign.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
    }
}

/* Says what it found as well as that it was wrong, because "the count is not
 * 2" sends somebody back to the debugger and "the count is 3, wanted 2" does
 * not. */
static void same_int(int got, int wanted, const char *what) {
    g_checks++;
    if (got != wanted) {
        g_failures++;
        printf("  FAIL: %s -- got %d, wanted %d\n", what, got, wanted);
    }
}

/* --- Packages to work on --- */

#define PKG      "/Temp/Notes.rpk"
#define WALLS    "/System/Wallpapers"
#define RECEIPTS "/System/Installed"

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

/*
 * A package folder, signed, ready to install.
 *
 * Signed here rather than in each test because every one of them needs it:
 * install refuses an unsigned package before it reads anything, so a test
 * that forgot would be testing the refusal by accident and passing.
 */
static void make_package(const char *manifest, bool with_module) {
    recon_fs_remove_tree("/", PKG);
    recon_fs_mkdir("/", PKG);
    write_file(PKG "/package.txt", manifest);
    write_file(PKG "/notes.png", "an icon");
    write_file(PKG "/paper.png", "a wallpaper");
    if (with_module) {
        write_file(PKG "/Notes.rex", "not a shared object");
    }
}

/*
 * Returns whether it worked rather than asserting it did.
 *
 * Signing reads the manifest, so a manifest the reader refuses is refused
 * here too -- and one test below is about exactly that. A helper that treated
 * every refusal as a broken fixture would have reported that finding as its
 * own failure.
 */
static bool sign_it(void) {
    if (recon_package_sign(PKG)) {
        return true;
    }
    return false;
}

/* For the tests that need a signed package to get anywhere. */
static void sign_it_or_say_so(void) {
    if (!sign_it()) {
        printf("  FAIL: could not sign the package: %s\n",
            recon_package_last_error());
    }
}

/* Content only: no `module`, so install finishes at the receipt. */
static const char *CONTENT =
    "name = Notes\n"
    "version = 1.0\n"
    "publisher = Somebody\n"
    "description = A place to put things\n"
    "icon = notes.png\n"
    "place = paper.png " WALLS "\n";

static void clean_up(void) {
    recon_package_uninstall("Notes");
    recon_fs_remove("/", WALLS "/paper.png");
    recon_fs_remove("/", "/System/Icons/notes.png");
    recon_fs_remove_tree("/", PKG);
}


/* --- The compositor's side of the install, stood in for ------------------
 *
 * `recon_package_install` finishes by loading the module it just copied, and
 * `recon_modules.c` is the compositor's: it pulls in wlroots and the window
 * layer. A suite about copying files and writing receipts should not need a
 * display library to link, so these three are defined here and that file is
 * not linked.
 *
 * **The loader is therefore not under test in this file**, and saying so
 * matters: what *is* under test is what install does when the loader refuses,
 * which is to undo everything it had done. A stand-in that refuses states that
 * condition exactly, and it states it for a module built against a different
 * ABI or one that simply will not open -- the two cases the header names and
 * neither of which can be produced on demand from a real loader.
 *
 * The same seam `userland/tests/hostsys.c` sits on the other side of.
 */

/*
 * Whether the stand-in loader accepts anything.
 *
 * False by default, which is the interesting case -- a module that will not
 * load is what the rollback exists for. One test needs a module package
 * actually installed before it can upgrade one, so it turns this on for that
 * step and off again for the step that has to fail.
 *
 * A switch rather than two stand-ins because the difference between the two
 * runs is then *one line in the test*, which is what makes it about the
 * rollback and not about the arrangement.
 */
static bool g_module_loads;

bool recon_modules_load(const char *reconos_path) {
    (void)reconos_path;
    return g_module_loads;
}

const char *recon_modules_last_error(void) {
    return "this stand-in never loads anything";
}

/* Uninstall asks for the module to be let go before its file is removed.
 * Nothing was ever loaded here, so there is nothing to let go. */
bool recon_modules_unload(const char *name) {
    (void)name;
    return true;
}

/* Drops a cache of icons already read. Nothing here reads one. */
void recon_icons_forget(void) {
}

/* --- Tests --- */

static void test_it_refuses_an_unsigned_package(void) {
    printf("an unsigned package is refused, and there is no way round it\n");

    make_package(CONTENT, false);
    /* Deliberately not signed. */

    check(!recon_package_install(PKG), "an unsigned package will not install");
    check(!recon_package_installed("Notes"),
        "and nothing was installed by the attempt");
    check(!recon_fs_exists("/", WALLS "/paper.png"),
        "and it placed no files on the way to refusing");

    clean_up();
}

static void test_a_content_package_installs(void) {
    printf("a package of content places its files and writes a receipt\n");

    make_package(CONTENT, false);
    sign_it_or_say_so();

    check(recon_package_install(PKG), "it installs");
    check(recon_package_installed("Notes"), "and says it is installed");
    check(file_is(WALLS "/paper.png", "a wallpaper"),
        "the wallpaper it brought is where the manifest said");
    check(file_is("/System/Icons/notes.png", "an icon"),
        "and so is its icon");
    check(recon_fs_exists("/", RECEIPTS "/Notes.txt"),
        "and there is a receipt naming what it did");

    struct recon_package_info info;
    memset(&info, 0, sizeof(info));
    check(recon_package_count() >= 1, "it is counted");
    check(recon_package_at(0, &info), "and can be read back");
    check(strcmp(info.name, "Notes") == 0, "under its own name");
    check(strcmp(info.version, "1.0") == 0, "with its version");

    clean_up();
}

static void test_installing_it_twice_is_refused(void) {
    printf("installing something already installed is refused, not repeated\n");

    make_package(CONTENT, false);
    sign_it_or_say_so();
    check(recon_package_install(PKG), "the first install works");

    check(!recon_package_install(PKG), "the second is refused");
    check(strstr(recon_package_last_error(), "already installed") != NULL,
        "and says so, rather than silently swapping the program");

    /* The first install is untouched by the refusal. */
    check(recon_package_installed("Notes"), "the first one is still installed");
    check(file_is(WALLS "/paper.png", "a wallpaper"),
        "and its files are still there");

    clean_up();
}

/*
 * The sharpest of the placement rules, and the one that costs somebody
 * something when it breaks.
 *
 * --- Two guards, and this can only see one of them ---
 *
 * *Leaving the file alone* is guarded twice: `recon_package_install` skips a
 * destination that exists, and `recon_fs_copy` refuses one anyway --
 * *"'%s' already exists"*. Deleting either one on purpose leaves this test
 * passing, because the other still holds. That is defence in depth and it is
 * the right shape; it is recorded here so a green run is not read as proof
 * that install's own check works.
 *
 * *Not recording it* has one guard, install's, and the second half of this
 * test is what watches it: a receipt naming a file the package never placed
 * makes removing the package take away somebody else's file. Mutating install
 * to record it does break this, which is how that half is known to bite.
 */
static void test_a_file_that_was_there_already_is_left_alone(void) {
    printf("a file that was already there is left alone, and not recorded\n");

    /* Somebody's own wallpaper, under the name the package also uses. */
    write_file(WALLS "/paper.png", "the one they chose");

    make_package(CONTENT, false);
    sign_it_or_say_so();
    check(recon_package_install(PKG), "it installs over the top of nothing");

    check(file_is(WALLS "/paper.png", "the one they chose"),
        "their file is untouched -- the package did not overwrite it");

    /*
     * And the half that only shows up later: it must not be in the receipt
     * either, or removing this package takes away a file it never placed.
     */
    recon_package_uninstall("Notes");
    check(recon_fs_exists("/", WALLS "/paper.png"),
        "and removing the package leaves their file behind");
    check(file_is(WALLS "/paper.png", "the one they chose"),
        "still theirs, and still what it was");

    recon_fs_remove("/", WALLS "/paper.png");
    clean_up();
}

static void test_a_setting_that_has_a_value_is_kept(void) {
    printf("a setting somebody has answered is not answered again\n");

    /* It places a file as well as writing settings, because a package that
     * brings neither code nor a file is refused -- see the test below. */
    static const char *WITH_SETTING =
        "name = Notes\n"
        "version = 1.0\n"
        "icon = notes.png\n"
        "place = paper.png " WALLS "\n"
        "setting = notes.theme paper\n"
        "setting = notes.width 80\n";

    recon_registry_remove(RECON_REG_SYSTEM, "notes.theme");
    recon_registry_remove(RECON_REG_SYSTEM, "notes.width");

    /* One of the two is already answered, and the answer is not the default. */
    recon_registry_set(RECON_REG_SYSTEM, "notes.theme", "midnight");

    make_package(WITH_SETTING, false);
    sign_it_or_say_so();
    check(recon_package_install(PKG), "it installs");

    check(strcmp(recon_registry_get(RECON_REG_SYSTEM, "notes.theme", ""),
            "midnight") == 0,
        "the setting they had chosen is still theirs");
    check(strcmp(recon_registry_get(RECON_REG_SYSTEM, "notes.width", ""),
            "80") == 0,
        "and the one they had not is filled in");

    /* Removing it takes back what it wrote and nothing else. */
    recon_package_uninstall("Notes");
    check(strcmp(recon_registry_get(RECON_REG_SYSTEM, "notes.theme", ""),
            "midnight") == 0,
        "removing it leaves the choice they made");
    check(recon_registry_get(RECON_REG_SYSTEM, "notes.width", "")[0] == '\0',
        "and takes back only what it put there");

    recon_registry_remove(RECON_REG_SYSTEM, "notes.theme");
    clean_up();
}

/*
 * The allow-list, and what it does when it trips.
 *
 * `/System/Config` is where the account list and the settings live, so a
 * package that could write there could rewrite who may use the machine. The
 * list is of permitted places rather than forbidden ones because, as the
 * header puts it, *"a list of forbidden places is a list somebody has to keep
 * complete, and the day it is missing an entry is the day a package writes
 * into /System/Config."*
 *
 * The refusal happens partway through placing, so the second half of this test
 * is the one that matters: the icon was already copied by then, and it has to
 * be taken back.
 */
/*
 * Found by this suite failing, which is the right way round: the settings test
 * first described a package with an icon and two settings and nothing else,
 * and it was refused.
 *
 * `recon_manifest.c` has the reasoning, and it is a rule that was loosened
 * once already -- a module used to be required, which meant a wallpaper pack
 * could not be a package at all. What replaced it asks whether the package
 * brings *anything*: a manifest with neither code nor a file to place
 * *"describes something that would do nothing on installing and nothing on
 * removal."* An icon does not count, because an icon belongs to something.
 */
static void test_a_package_must_bring_something(void) {
    printf("a package that brings neither code nor files is refused\n");

    static const char *EMPTY_HANDED =
        "name = Notes\n"
        "version = 1.0\n"
        "icon = notes.png\n"
        "setting = notes.width = 80\n";

    recon_registry_remove(RECON_REG_SYSTEM, "notes.width");

    make_package(EMPTY_HANDED, false);

    /* **It cannot even be signed.** The refusal is in reading the manifest and
     * both paths read it, so a package that would do nothing is turned down a
     * step earlier than expected -- which is the better place for it. */
    check(!sign_it(), "it cannot be signed either");

    check(!recon_package_install(PKG),
        "an icon and a setting are not something to install");
    check(!recon_package_installed("Notes"), "and nothing was installed");
    check(recon_registry_get(RECON_REG_SYSTEM, "notes.width", "")[0] == '\0',
        "and its settings were not written on the way to refusing");

    clean_up();
}

static void test_a_place_outside_the_list_is_refused(void) {
    printf("a package may not write where the list does not allow\n");

    static const char *NOSY =
        "name = Notes\n"
        "version = 1.0\n"
        "icon = notes.png\n"
        "place = paper.png /System/Config\n";

    make_package(NOSY, false);
    sign_it_or_say_so();

    check(!recon_package_install(PKG),
        "a package that wants to write to /System/Config is refused");
    check(strstr(recon_package_last_error(), "/System/Config") != NULL,
        "and says where it was trying to write");

    check(!recon_fs_exists("/", "/System/Config/paper.png"),
        "nothing was written there");
    check(!recon_fs_exists("/", "/System/Icons/notes.png"),
        "and the icon it had already copied was taken back, so a refusal "
        "halfway leaves no half-install");
    check(!recon_package_installed("Notes"), "nothing is installed");

    clean_up();
}

static void test_a_module_that_will_not_load_leaves_nothing(void) {
    printf("a package whose code will not load leaves nothing behind\n");

    static const char *WITH_MODULE =
        "name = Notes\n"
        "version = 1.0\n"
        "module = Notes.rex\n"
        "icon = notes.png\n"
        "place = paper.png " WALLS "\n";

    make_package(WITH_MODULE, true);
    sign_it_or_say_so();

    /*
     * `Notes.rex` is a text file, so `recon_modules_load` refuses it -- which
     * is the case this test is for. The install has already copied the module,
     * the icon and the wallpaper and written a receipt by the time it finds
     * out, so everything it did has to be undone.
     */
    check(!recon_package_install(PKG), "the install fails at the load");

    check(!recon_package_installed("Notes"), "nothing is left installed");
    check(!recon_fs_exists("/", "/Apps/Notes.rex"),
        "the module it copied is gone");
    check(!recon_fs_exists("/", "/System/Icons/notes.png"),
        "the icon it copied is gone");
    check(!recon_fs_exists("/", WALLS "/paper.png"),
        "the wallpaper it placed is gone");
    check(!recon_fs_exists("/", RECEIPTS "/Notes.txt"),
        "and the receipt is gone with them");

    clean_up();
}

static void test_uninstall_removes_what_the_receipt_names(void) {
    printf("removing a package takes back what it placed\n");

    make_package(CONTENT, false);
    sign_it_or_say_so();
    check(recon_package_install(PKG), "it installs");

    check(recon_package_uninstall("Notes"), "and it uninstalls");
    check(!recon_package_installed("Notes"), "it is no longer installed");
    check(!recon_fs_exists("/", WALLS "/paper.png"),
        "its wallpaper is gone");
    check(!recon_fs_exists("/", "/System/Icons/notes.png"),
        "its icon is gone");
    check(!recon_fs_exists("/", RECEIPTS "/Notes.txt"),
        "and so is the receipt");

    clean_up();
}

static void test_a_file_deleted_by_hand_is_not_an_error(void) {
    printf("a file somebody deleted first does not block removing it\n");

    make_package(CONTENT, false);
    sign_it_or_say_so();
    check(recon_package_install(PKG), "it installs");

    /* Somebody tidied up by hand. The receipt still names it. */
    recon_fs_remove("/", WALLS "/paper.png");

    check(recon_package_uninstall("Notes"),
        "it still uninstalls -- a file already gone is not a failure");
    check(!recon_package_installed("Notes"), "and it is properly gone");

    clean_up();
}

static void test_verify_counts_and_names_what_is_missing(void) {
    printf("verify says how much is missing, and names one\n");

    make_package(CONTENT, false);
    sign_it_or_say_so();
    check(recon_package_install(PKG), "it installs");

    int placed = 0, missing = -1;
    char first[RECON_PATH_MAX];
    first[0] = '\0';

    check(recon_package_verify("Notes", &placed, &missing, first, sizeof(first)),
        "a fresh install verifies");
    same_int(missing, 0, "with nothing missing");
    check(placed > 0, "and something placed");

    /* Take one away behind its back. */
    recon_fs_remove("/", WALLS "/paper.png");

    placed = 0;
    missing = 0;
    first[0] = '\0';
    recon_package_verify("Notes", &placed, &missing, first, sizeof(first));
    same_int(missing, 1, "one file gone is one missing");
    check(strstr(first, "paper.png") != NULL,
        "and it names the one that is gone rather than only counting");

    /* It checks; it does not repair. */
    check(!recon_fs_exists("/", WALLS "/paper.png"),
        "verify put nothing back, which is what its header promises");

    clean_up();
}

static void test_upgrade_takes_only_something_newer(void) {
    printf("an upgrade is strictly newer, or it is refused\n");

    make_package(CONTENT, false);
    sign_it_or_say_so();
    check(recon_package_install(PKG), "1.0 installs");

    /* The same version is not an upgrade. */
    make_package(CONTENT, false);
    sign_it_or_say_so();
    check(!recon_package_upgrade(PKG), "the same version is refused");

    static const char *OLDER =
        "name = Notes\n"
        "version = 0.9\n"
        "icon = notes.png\n"
        "place = paper.png " WALLS "\n";
    make_package(OLDER, false);
    sign_it_or_say_so();
    check(!recon_package_upgrade(PKG), "an older one is refused");

    static const char *NONSENSE =
        "name = Notes\n"
        "version = the good one\n"
        "icon = notes.png\n"
        "place = paper.png " WALLS "\n";
    make_package(NONSENSE, false);
    sign_it_or_say_so();
    check(!recon_package_upgrade(PKG),
        "and a version that cannot be compared is refused rather than guessed");

    /* Through all of that, the installed one is untouched. */
    struct recon_package_info info;
    memset(&info, 0, sizeof(info));
    check(recon_package_installed("Notes"), "1.0 is still installed");
    if (recon_package_at(0, &info)) {
        check(strcmp(info.version, "1.0") == 0, "and still says 1.0");
    }

    static const char *NEWER =
        "name = Notes\n"
        "version = 1.1\n"
        "icon = notes.png\n"
        "place = paper.png " WALLS "\n";
    make_package(NEWER, false);
    write_file(PKG "/paper.png", "a newer wallpaper");
    sign_it_or_say_so();
    check(recon_package_upgrade(PKG), "a newer one is taken");
    check(file_is(WALLS "/paper.png", "a newer wallpaper"),
        "and its files replaced the old ones");

    clean_up();
}

/*
 * The reason `upgrade` is not `uninstall` followed by `install`.
 *
 * The header is emphatic about it: an upgrade that fails halfway has removed a
 * program that worked and put nothing in its place, and somebody who wanted a
 * newer version now has no version -- *"worse than what they started with and
 * worse than being refused."*
 *
 * So the old files are parked with a suffix, the new package goes in over the
 * gap, and the old ones are only removed once everything has worked including
 * the load. This forces the load to fail at exactly that point and checks that
 * what was there before is back.
 */
static void test_a_failed_upgrade_puts_the_old_one_back(void) {
    printf("an upgrade that fails leaves the working version in place\n");

    static const char *V1 =
        "name = Notes\n"
        "version = 1.0\n"
        "module = Notes.rex\n"
        "icon = notes.png\n"
        "place = paper.png " WALLS "\n";

    static const char *V2 =
        "name = Notes\n"
        "version = 2.0\n"
        "module = Notes.rex\n"
        "icon = notes.png\n"
        "place = paper.png " WALLS "\n";

    /* The version that works. */
    g_module_loads = true;
    make_package(V1, true);
    write_file(PKG "/Notes.rex", "the version that works");
    write_file(PKG "/paper.png", "the wallpaper it came with");
    sign_it_or_say_so();
    check(recon_package_install(PKG), "1.0 installs");
    check(file_is("/Apps/Notes.rex", "the version that works"),
        "and its code is in place");

    /* And a newer one whose code will not load. */
    g_module_loads = false;
    make_package(V2, true);
    write_file(PKG "/Notes.rex", "the version that does not");
    write_file(PKG "/paper.png", "a wallpaper nobody should see");
    sign_it_or_say_so();

    check(!recon_package_upgrade(PKG), "the upgrade fails at the load");

    check(recon_package_installed("Notes"),
        "and the program that worked is still installed");
    check(file_is("/Apps/Notes.rex", "the version that works"),
        "with its own code, not the one that would not load");
    check(file_is(WALLS "/paper.png", "the wallpaper it came with"),
        "and its own files, not the new ones");

    struct recon_package_info info;
    memset(&info, 0, sizeof(info));
    if (recon_package_at(0, &info)) {
        check(strcmp(info.version, "1.0") == 0,
            "and it still says the version that is actually there");
    }

    /* Nothing parked and forgotten. */
    struct recon_dirent entries[64];
    int found = recon_fs_list("/", "/Apps", entries, 64);
    bool parked = false;
    for (int i = 0; i < found && i < 64; i++) {
        if (strstr(entries[i].name, "replaced-by-upgrade") != NULL) {
            parked = true;
        }
    }
    check(!parked, "and nothing was left parked under an upgrade suffix");

    g_module_loads = false;
    clean_up();
    recon_fs_remove("/", "/Apps/Notes.rex");
}

int main(void) {
    printf("ReconOS package install tests\n\n");

    char root[] = "/tmp/reconos-pkginstall-XXXXXX";
    if (mkdtemp(root) == NULL) {
        printf("  FAIL: no temporary directory to work in\n");
        return 1;
    }
    if (!recon_fs_init(root)) {
        printf("  FAIL: %s\n", recon_fs_last_error());
        return 1;
    }

    /* recon_fs_init lays down the system directories but not this one, and a
     * package that places a wallpaper into a directory that is not there is a
     * different test from the one being run. */
    recon_fs_mkdir("/", WALLS);

    if (!recon_sign_make_key("builder")) {
        printf("  FAIL: %s\n", recon_sign_last_error());
        return 1;
    }

    test_it_refuses_an_unsigned_package();
    test_a_content_package_installs();
    test_installing_it_twice_is_refused();
    test_a_file_that_was_there_already_is_left_alone();
    test_a_setting_that_has_a_value_is_kept();
    test_a_package_must_bring_something();
    test_a_place_outside_the_list_is_refused();
    test_a_module_that_will_not_load_leaves_nothing();
    test_uninstall_removes_what_the_receipt_names();
    test_a_file_deleted_by_hand_is_not_an_error();
    test_verify_counts_and_names_what_is_missing();
    test_upgrade_takes_only_something_newer();
    test_a_failed_upgrade_puts_the_old_one_back();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
