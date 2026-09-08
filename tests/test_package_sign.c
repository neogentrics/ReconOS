/*
 * What a package signature actually covers.
 *
 * There was no suite for packages at all, which meant the install path -- the
 * one that takes somebody else's shared object and loads it into this process
 * -- was the least tested thing in the system.
 *
 * The claim being tested is narrow and is the whole point: **the signature
 * covers every file the package brings, not just its manifest.** Signing the
 * manifest alone binds the names of the files and none of their contents,
 * which leaves the module free to be swapped -- and the module is the only
 * file that can do anything.
 *
 * Run with: ./build/recon_package_sign_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_fs.h"
#include "recon_package.h"
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

/* --- A package to work on --- */

#define PKG "/Temp/Notes.rpk"

static void write_file(const char *path, const char *text) {
    recon_fs_write("/", path, text, strlen(text));
}

/*
 * A package folder with a manifest, a module and an icon.
 *
 * The module is not a real shared object -- nothing here loads it. What
 * matters is that it is a file the manifest names, so it is a file the
 * signature has to cover.
 */
static void make_package(const char *manifest) {
    recon_fs_remove_tree("/", PKG);
    recon_fs_mkdir("/", PKG);
    write_file(PKG "/package.txt", manifest);
    write_file(PKG "/Notes.rex", "this stands in for the code");
    write_file(PKG "/notes.png", "this stands in for the icon");
    write_file(PKG "/paper.png", "this stands in for a wallpaper");
}

static const char *ORDINARY =
    "name = Notes\n"
    "version = 1.0\n"
    "publisher = Somebody\n"
    "description = A place to put things\n"
    "module = Notes.rex\n"
    "icon = notes.png\n"
    "place = paper.png /System/Wallpapers\n";

/* --- Tests --- */

static void test_a_signed_package_verifies(void) {
    printf("a package signs, and says who signed it\n");

    make_package(ORDINARY);

    char signer[RECON_SIGN_NAME_MAX];
    check(!recon_package_signed_by(PKG, signer, sizeof(signer)),
        "an unsigned package does not verify");

    check(recon_package_sign(PKG), "it can be signed");
    check(recon_package_signed_by(PKG, signer, sizeof(signer)),
        "and then it verifies");
    check(strcmp(signer, "builder") == 0, "naming the key that signed it");
}

static void test_the_signature_covers_the_module(void) {
    printf("changing the module breaks the signature\n");

    /*
     * The test this file exists for.
     *
     * A signature over the manifest alone would still verify here: the
     * manifest has not changed, only the file it names. And the file it names
     * is the shared object that gets loaded into this process -- so a
     * manifest-only signature would be a signature over the one part of a
     * package that cannot hurt anybody.
     */
    make_package(ORDINARY);
    check(recon_package_sign(PKG), "it signs");

    write_file(PKG "/Notes.rex", "this is somebody else's code");

    char signer[RECON_SIGN_NAME_MAX];
    check(!recon_package_signed_by(PKG, signer, sizeof(signer)),
        "a swapped module does not verify");
}

static void test_the_signature_covers_everything_else(void) {
    printf("changing any other file breaks it too\n");

    make_package(ORDINARY);
    check(recon_package_sign(PKG), "it signs");

    char signer[RECON_SIGN_NAME_MAX];

    /* The icon. */
    write_file(PKG "/notes.png", "a different icon");
    check(!recon_package_signed_by(PKG, signer, sizeof(signer)),
        "a changed icon does not verify");

    /* The placed file, which the manifest names through `place`. */
    make_package(ORDINARY);
    recon_package_sign(PKG);
    write_file(PKG "/paper.png", "a different wallpaper");
    check(!recon_package_signed_by(PKG, signer, sizeof(signer)),
        "a changed placed file does not verify");

    /* And the manifest itself. Changing where a file goes changes nothing
     * about the files, and must still break the signature. */
    make_package(ORDINARY);
    recon_package_sign(PKG);
    write_file(PKG "/package.txt",
        "name = Notes\n"
        "version = 1.0\n"
        "publisher = Somebody\n"
        "description = A place to put things\n"
        "module = Notes.rex\n"
        "icon = notes.png\n"
        "place = paper.png /System/Themes\n");
    check(!recon_package_signed_by(PKG, signer, sizeof(signer)),
        "a changed destination does not verify");
}

static void test_the_order_in_the_manifest_does_not_matter(void) {
    printf("the same package written in a different order signs the same\n");

    /*
     * The digest list is sorted, so the bytes signed are a property of what
     * the package *contains* rather than of how somebody happened to type the
     * manifest. Without that, a package signed on one machine and verified on
     * another could disagree for no reason anybody could see.
     */
    make_package(ORDINARY);
    check(recon_package_sign(PKG), "the first order signs");

    size_t size = 0;
    char *first = recon_fs_read("/", PKG "/package.sig", &size);
    check(first != NULL, "and there is a signature");

    /* The same files, the same destinations, a different order of lines. */
    make_package(
        "place = paper.png /System/Wallpapers\n"
        "icon = notes.png\n"
        "module = Notes.rex\n"
        "description = A place to put things\n"
        "publisher = Somebody\n"
        "version = 1.0\n"
        "name = Notes\n");
    check(recon_package_sign(PKG), "the second order signs");

    char *second = recon_fs_read("/", PKG "/package.sig", &size);
    check(second != NULL, "and so does it");

    /*
     * The manifests differ byte for byte, so the digests of *them* differ and
     * the signatures must too. What is being checked is that both verify --
     * the sort is about the file list, not about the manifest.
     */
    char signer[RECON_SIGN_NAME_MAX];
    check(recon_package_signed_by(PKG, signer, sizeof(signer)),
        "and the reordered one verifies");

    free(first);
    free(second);
}

static void test_a_missing_file_cannot_be_signed_for(void) {
    printf("a package that names a file it does not have will not sign\n");

    /*
     * There is nothing to take a digest of, so there is nothing to sign. It
     * matters that this refuses rather than signing what is there: a
     * signature that quietly covered fewer files than the manifest names is a
     * signature that says less than it appears to.
     */
    make_package(ORDINARY);
    recon_fs_remove("/", PKG "/Notes.rex");

    check(!recon_package_sign(PKG), "signing is refused");

    char signer[RECON_SIGN_NAME_MAX];
    check(!recon_package_signed_by(PKG, signer, sizeof(signer)),
        "and it does not verify either");
}

static void test_a_signature_from_an_untrusted_key(void) {
    printf("a signature this machine cannot trace to a key it trusts\n");

    make_package(ORDINARY);
    check(recon_package_sign(PKG), "it signs");

    char signer[RECON_SIGN_NAME_MAX];
    check(recon_package_signed_by(PKG, signer, sizeof(signer)),
        "and verifies while the key is trusted");

    /*
     * Distrusting the key is what an administrator does when a publisher
     * turns out not to be one. The package is unchanged and its signature is
     * still a real signature; it is simply no longer one this machine will
     * act on.
     */
    check(recon_sign_distrust("builder"), "the key is distrusted");
    check(!recon_package_signed_by(PKG, signer, sizeof(signer)),
        "and the package no longer verifies");
}

int main(void) {
    printf("ReconOS package signature tests\n\n");

    char root[] = "/tmp/reconos-pkgsign-XXXXXX";
    if (mkdtemp(root) == NULL) {
        printf("  FAIL: no temporary directory to work in\n");
        return 1;
    }
    if (!recon_fs_init(root)) {
        printf("  FAIL: %s\n", recon_fs_last_error());
        return 1;
    }
    recon_fs_mkdir("/", "/Temp");

    if (!recon_sign_make_key("builder")) {
        printf("  FAIL: %s\n", recon_sign_last_error());
        return 1;
    }

    test_a_signed_package_verifies();
    test_the_signature_covers_the_module();
    test_the_signature_covers_everything_else();
    test_the_order_in_the_manifest_does_not_matter();
    test_a_missing_file_cannot_be_signed_for();
    test_a_signature_from_an_untrusted_key();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
