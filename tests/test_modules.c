/*
 * The module loader, with a real module in front of it.
 *
 * --- What was not tested, and how that was found out ---
 *
 * `src/recon_modules.c` is 1,514 lines and nothing had ever linked it into a
 * suite. `tests/test_package_install.c` says so in its own header and stands
 * the loader in with three functions, which is right for what that file is
 * about -- but it left the one place ReconOS takes somebody else's machine
 * code and runs it inside itself with no test at all.
 *
 * What kept it out was not its size. Compiled on its own, the linker asked for
 * eighteen symbols, sixteen of which are ordinary ReconOS sources. The other
 * two were **`_wlr_log` and `recon_appwin_destroy`**: one log call and one
 * destructor, holding fifteen hundred lines out of every suite in the tree.
 * Both are stood in for below.
 *
 * --- The claim this is really about ---
 *
 * A package's signature is checked when it is **installed**. It covers a
 * digest for every file, so install proves where the files came from at that
 * moment -- and then stops proving anything. Nothing afterwards looked at the
 * bytes, and `recon_modules_load` called `dlopen` on whatever was at the path.
 *
 * The ABI check sat immediately after that `dlopen`, which reads like a gate
 * and is not one: **`dlopen` runs a module's initialisers**. By the time the
 * ABI refuses a module, code from that file has already run.
 *
 * That is why `tests/example_module.c` has a constructor that writes a marker,
 * and why this file builds it twice. The marker turns "was it opened?" into
 * something observable from outside the process, and the second build is what
 * a swapped module looks like when the swap is competent -- a valid, loadable
 * shared object that is simply not the one that was installed. Swapping in
 * rubbish would prove nothing, because `dlopen` refuses rubbish unaided.
 *
 * Run with: ./build/recon_modules_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "recon_fs.h"
#include "recon_module.h"
#include "recon_modules.h"
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

static void same_text(const char *got, const char *wanted, const char *what) {
    g_checks++;
    if (got == NULL || strcmp(got, wanted) != 0) {
        g_failures++;
        printf("  FAIL: %s -- got '%s', wanted '%s'\n", what,
            got != NULL ? got : "(none)", wanted);
    }
}

/* A message is allowed to change; what it has to keep doing is name the
 * thing that is wrong, so this asks for a phrase rather than the sentence. */
static void says(const char *got, const char *phrase, const char *what) {
    g_checks++;
    if (got == NULL || strstr(got, phrase) == NULL) {
        g_failures++;
        printf("  FAIL: %s -- '%s' does not mention '%s'\n", what,
            got != NULL ? got : "(none)", phrase);
    }
}


/* --- The compositor's side, stood in for -------------------------------
 *
 * The whole of it. Two functions: the loader logs what it loaded, and it
 * destroys an application's window when the module that contributed it goes
 * away. Neither is what this file is about, and between them they were the
 * reason none of it could be tested.
 */

void _wlr_log(int verbosity, const char *format, ...);

void _wlr_log(int verbosity, const char *format, ...) {
    (void)verbosity;
    (void)format;
}

void recon_appwin_destroy(struct recon_appwin *window);

void recon_appwin_destroy(struct recon_appwin *window) {
    (void)window;
}

/* And one more from the other side: installing a package drops the cache of
 * icons already read, so that a package which brought one is seen. Nothing
 * here reads an icon. */
void recon_icons_forget(void);

void recon_icons_forget(void) {
}


/* --- Somewhere to put a marker ------------------------------------------ */

#define MARKER_VAR "RECON_EXAMPLE_MODULE_MARKER"

static char g_marker[512];

static void forget_the_marker(void) {
    unlink(g_marker);
}

/* What the constructor wrote, or NULL if it did not run. Static buffer. */
static const char *what_ran(void) {
    static char text[64];

    FILE *f = fopen(g_marker, "r");

    if (f == NULL) {
        return NULL;
    }
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    return text;
}


/* --- Getting a built shared object into the ReconOS filesystem ---------- */

/*
 * The two builds of `tests/example_module.c`, as host paths. CMake fills
 * these in, because only CMake knows where it put them.
 */
#ifndef EXAMPLE_MODULE_ONE
#define EXAMPLE_MODULE_ONE ""
#endif
#ifndef EXAMPLE_MODULE_TWO
#define EXAMPLE_MODULE_TWO ""
#endif

static bool copy_in(const char *host_path, const char *reconos_path) {
    FILE *f = fopen(host_path, "rb");

    if (f == NULL) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return false;
    }

    char *bytes = malloc((size_t)size);

    if (bytes == NULL) {
        fclose(f);
        return false;
    }
    size_t read = fread(bytes, 1, (size_t)size, f);
    fclose(f);

    bool ok = read == (size_t)size &&
        recon_fs_write("/", reconos_path, bytes, read);
    free(bytes);
    return ok;
}


/* --- A package that brings the example module --------------------------- */

#define PKG "/Temp/Example.rpk"
#define PLACED RECON_DIR_APPS "/Example.rex"

static void write_file(const char *path, const char *text) {
    recon_fs_write("/", path, text, strlen(text));
}

static bool make_and_install_the_package(void) {
    recon_fs_remove_tree("/", PKG);
    recon_fs_mkdir("/", PKG);

    write_file(PKG "/package.txt",
        "name = Example\n"
        "version = 1.0\n"
        "publisher = Somebody\n"
        "description = A module that exists so the loader has something\n"
        "module = Example.rex\n");

    if (!copy_in(EXAMPLE_MODULE_ONE, PKG "/Example.rex")) {
        printf("  FAIL: could not read the built module at '%s'\n",
            EXAMPLE_MODULE_ONE);
        return false;
    }
    if (!recon_package_sign(PKG)) {
        printf("  FAIL: could not sign the package: %s\n",
            recon_package_last_error());
        return false;
    }
    if (!recon_package_install(PKG)) {
        printf("  FAIL: could not install the package: %s\n",
            recon_package_last_error());
        return false;
    }
    return true;
}

static void clean_up(void) {
    recon_package_uninstall("Example");
    recon_fs_remove_tree("/", PKG);
    forget_the_marker();
}


/* --- The tests ---------------------------------------------------------- */

static void test_a_real_module_loads(void) {
    printf("a module a package installed loads, and its code runs\n");

    forget_the_marker();

    /*
     * Installing loads it, which is the first time anything in this tree has
     * handed the loader a real shared object. If this check fails, none of
     * the ones below mean anything -- a gate that refuses everything would
     * pass every tamper test in the file.
     */
    check(make_and_install_the_package(), "the package installs");
    same_text(what_ran(), "one",
        "the module was opened and its constructor ran");

    char where[RECON_PATH_MAX];
    check(recon_modules_path_of("Example", where, sizeof(where)),
        "the loader knows where it came from");

    clean_up();
}

static void test_a_swapped_module_is_refused_before_it_is_opened(void) {
    printf("a module replaced by another valid module\n");

    check(make_and_install_the_package(), "the package installs");
    check(recon_modules_unload("Example"), "and it unloads again");

    /*
     * The swap. A different build of the same source: valid ELF, exports the
     * right symbol, declares the right ABI, and would load without complaint
     * on any day before today. The only thing wrong with it is that it is not
     * the file the receipt vouches for.
     */
    check(copy_in(EXAMPLE_MODULE_TWO, PLACED),
        "a different, equally valid module is put in its place");

    forget_the_marker();
    check(!recon_modules_load(PLACED), "the loader refuses it");

    /*
     * The check this file exists for. Not "it was refused" -- the ABI check
     * refused things before and refused them *after* their code had run. The
     * claim is that nothing from the file executed, and the marker is the
     * only way to ask.
     */
    check(what_ran() == NULL,
        "and nothing in it ran: the gate is in front of dlopen, not behind it");

    says(recon_modules_last_error(), "has changed",
        "the refusal says what is wrong");
    says(recon_modules_last_error(), "Example",
        "and which package to ask about it");

    clean_up();
}

static void test_a_module_nothing_installed_is_refused(void) {
    printf("a module dropped into /Apps by hand\n");

    /*
     * Not tampering: a file nothing knows about. Refused all the same, and
     * for the reason the refusal gives -- loading it would be running code on
     * the strength of it being in the right folder, which is the whole of
     * what the receipt replaced.
     */
    forget_the_marker();
    check(copy_in(EXAMPLE_MODULE_ONE, RECON_DIR_APPS "/Dropped.rex"),
        "a perfectly good module is copied into /Apps");

    check(!recon_modules_load(RECON_DIR_APPS "/Dropped.rex"),
        "the loader refuses it");
    check(what_ran() == NULL, "and nothing in it ran");
    says(recon_modules_last_error(), "nothing installed",
        "the refusal says no package claims it");

    recon_fs_remove("/", RECON_DIR_APPS "/Dropped.rex");
    forget_the_marker();
}

static void test_a_receipt_from_before_digests_refuses_and_says_so(void) {
    printf("a module installed before receipts recorded digests\n");

    check(make_and_install_the_package(), "the package installs");
    check(recon_modules_unload("Example"), "and it unloads again");

    /*
     * The receipt rewritten in the old shape: a bare path under `files:`,
     * which is every receipt on every machine that installed anything before
     * this landed. The file is untouched and is exactly what was installed --
     * and it still does not load, because what makes a module loadable is
     * something vouching for it and an old receipt cannot.
     *
     * Refusing here is a decision worth being able to see change. The message
     * is the whole of what makes it tolerable: it names the package and says
     * installing it again fixes this, rather than reporting a fault in a file
     * that does not have one.
     */
    write_file(RECON_DIR_RECEIPTS "/Example.txt",
        "name = Example\n"
        "version = 1.0\n"
        "publisher = Somebody\n"
        "description = A module that exists so the loader has something\n"
        "files:\n"
        PLACED "\n");

    forget_the_marker();
    check(!recon_modules_load(PLACED), "the loader refuses it");
    check(what_ran() == NULL, "without opening it");
    says(recon_modules_last_error(), "Install 'Example' again",
        "and says what to do about it rather than what is wrong with it");

    clean_up();
}


int main(void) {
    printf("ReconOS module loader tests\n\n");

    char root[] = "/tmp/reconos-modules-XXXXXX";

    if (mkdtemp(root) == NULL) {
        printf("  FAIL: no temporary directory to work in\n");
        return 1;
    }
    if (!recon_fs_init(root)) {
        printf("  FAIL: %s\n", recon_fs_last_error());
        return 1;
    }

    /*
     * The marker goes beside the filesystem rather than inside it: a module's
     * constructor writes it with plain `fopen`, and a host path is the only
     * kind it could use without going through the machinery under test.
     */
    snprintf(g_marker, sizeof(g_marker), "%s.marker", root);
    setenv(MARKER_VAR, g_marker, 1);

    if (!recon_sign_make_key("builder")) {
        printf("  FAIL: %s\n", recon_sign_last_error());
        return 1;
    }

    /* No server and no font: nothing here contributes a window. */
    recon_modules_init(NULL, NULL);

    test_a_real_module_loads();
    test_a_swapped_module_is_refused_before_it_is_opened();
    test_a_module_nothing_installed_is_refused();
    test_a_receipt_from_before_digests_refuses_and_says_so();

    recon_modules_finish();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
