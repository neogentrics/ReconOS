/*
 * Tests for skins.
 *
 * The failure worth catching here is silent: a role that no skin answers, or a
 * name table that has drifted out of step with the enum, does not crash. It
 * just makes one part of the desktop the wrong colour forever, and nobody
 * connects that to the commit that caused it.
 *
 * Run with: ninja -C build && ./build/recon_theme_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "recon_fs.h"
#include "recon_registry.h"
#include "recon_theme.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
    }
}

/* --- Tests --- */

static void test_roles_are_all_answered(void) {
    printf("every role has a name and a colour\n");

    for (int i = 0; i < RECON_THEME_ROLE_COUNT; i++) {
        const char *name = recon_theme_role_name((enum recon_theme_role)i);
        if (name == NULL || *name == '\0') {
            g_checks++;
            g_failures++;
            printf("  FAIL: role %d has no name\n", i);
            continue;
        }
        g_checks++;
    }

    /* Names must be unique, or a theme file setting one of them would be
     * setting whichever the lookup happened to find first. */
    for (int i = 0; i < RECON_THEME_ROLE_COUNT; i++) {
        for (int j = i + 1; j < RECON_THEME_ROLE_COUNT; j++) {
            if (strcmp(recon_theme_role_name(i), recon_theme_role_name(j)) == 0) {
                g_checks++;
                g_failures++;
                printf("  FAIL: roles %d and %d share the name '%s'\n",
                    i, j, recon_theme_role_name(i));
            }
        }
    }
    g_checks++;

    check(recon_theme_role_name(RECON_THEME_ROLE_COUNT) == NULL,
        "a role past the end has no name");
}

static void test_no_role_is_unset(void) {
    printf("no skin leaves a role blank\n");

    int count = recon_theme_count();
    check(count >= 4, "the four shipped skins are all there");

    for (int t = 0; t < count; t++) {
        struct recon_theme_info info;
        if (!recon_theme_at(t, &info)) {
            continue;
        }
        check(recon_theme_set(info.name), info.name);

        /*
         * Fully transparent means nothing was written there. A role left at
         * zero draws as nothing at all, which is the failure this whole test
         * exists to catch -- it looks like a missing feature, not a missing
         * colour.
         */
        for (int i = 0; i < RECON_THEME_ROLE_COUNT; i++) {
            recon_color c = recon_theme_color((enum recon_theme_role)i);
            if ((c >> 24) == 0) {
                g_checks++;
                g_failures++;
                printf("  FAIL: '%s' leaves '%s' fully transparent\n",
                    info.name, recon_theme_role_name(i));
            }
        }
        g_checks++;
    }
}

static void test_out_of_range_is_loud(void) {
    printf("an unknown role is obvious rather than invisible\n");

    recon_color bad = recon_theme_color((enum recon_theme_role)-1);
    check(bad == RECON_RGB(0xFF, 0x00, 0xFF),
        "a role below the range gives magenta, not black");

    bad = recon_theme_color(RECON_THEME_ROLE_COUNT);
    check(bad == RECON_RGB(0xFF, 0x00, 0xFF),
        "a role past the end gives magenta, not black");
}

static void test_switching(void) {
    printf("switching\n");

    check(recon_theme_set("Recon"), "put on the default");
    recon_color recon_bar = recon_theme_color(RECON_THEME_BAR);

    check(recon_theme_set("Midnight"), "put on a dark one");
    recon_color midnight_bar = recon_theme_color(RECON_THEME_BAR);

    /* The point of the whole system: asking the same question gives a
     * different answer. */
    check(recon_bar != midnight_bar, "the same role gives a different colour");
    check(strcmp(recon_theme_current(), "Midnight") == 0,
        "the current skin is the one that was chosen");

    check(!recon_theme_set("NoSuchSkin"), "an unknown skin is refused");
    check(strcmp(recon_theme_current(), "Midnight") == 0,
        "and nothing changed when it was");

    unsigned before = recon_theme_generation();
    check(recon_theme_set("Recon"), "switch back");
    check(recon_theme_generation() != before,
        "the generation moves, so a cache can tell it is stale");
}

static void test_remembered(void) {
    printf("the choice is remembered\n");

    check(recon_theme_set("Aqua"), "choose one");
    check(strcmp(recon_registry_get(RECON_REG_USER, RECON_THEME_KEY, ""),
        "Aqua") == 0, "it was written to the registry");

    /* Exactly what a restart does. */
    recon_theme_finish();
    recon_theme_init();

    check(strcmp(recon_theme_current(), "Aqua") == 0,
        "and it comes back after a restart");
}

static void test_files(void) {
    printf("skins from files\n");

    check(recon_theme_write_defaults() == 0,
        "the shipped skins are already written, so nothing is rewritten");

    /*
     * A partial file: it says one thing and inherits the rest. This is what
     * makes "theme only the taskbar" possible without listing every role.
     */
    const char *partial =
        "name = JustTheBar\n"
        "description = Only the taskbar differs\n"
        "bar = 123456\n";

    check(recon_fs_write("/", RECON_DIR_THEMES "/JustTheBar" RECON_THEME_EXT,
        partial, strlen(partial)), "write a skin that sets one role");

    recon_theme_finish();
    recon_theme_init();

    check(recon_theme_set("JustTheBar"), "it loaded and can be chosen");
    check(recon_theme_color(RECON_THEME_BAR) == RECON_RGB(0x12, 0x34, 0x56),
        "the role it set is its own");
    check(recon_theme_color(RECON_THEME_ACCENT) ==
        RECON_RGB(0x8B, 0x1A, 0x1A),
        "and everything it did not mention is inherited, not blank");

    /* Six digits mean opaque; a person writing a colour by hand means opaque
     * unless they say otherwise. */
    check((recon_theme_color(RECON_THEME_BAR) >> 24) == 0xFF,
        "a six-digit colour is fully opaque");

    /* A file cannot quietly replace something that ships with the system. */
    const char *impostor = "name = Recon\nbar = 000000\n";
    check(recon_fs_write("/", RECON_DIR_THEMES "/Impostor" RECON_THEME_EXT,
        impostor, strlen(impostor)), "write a file claiming a built-in name");

    recon_theme_finish();
    recon_theme_init();
    check(recon_theme_set("Recon"), "the built-in is still there");
    check(recon_theme_color(RECON_THEME_BAR) != RECON_RGB(0, 0, 0),
        "and was not replaced by the file");
}

static void test_damaged_file(void) {
    printf("a skin file that is not quite right\n");

    const char *damaged =
        "name = Patchy\n"
        "this line has no equals\n"
        "not-a-role = FFFFFF\n"
        "accent = not a colour\n"
        "warning = 00FF00\n";

    check(recon_fs_write("/", RECON_DIR_THEMES "/Patchy" RECON_THEME_EXT,
        damaged, strlen(damaged)), "write it");

    recon_theme_finish();
    recon_theme_init();

    check(recon_theme_set("Patchy"), "it still loaded");
    check(recon_theme_color(RECON_THEME_WARNING) == RECON_RGB(0x00, 0xFF, 0x00),
        "the good line after the bad ones was read");
    check(recon_theme_color(RECON_THEME_ACCENT) == RECON_RGB(0x8B, 0x1A, 0x1A),
        "the unparseable colour was left at the default, not zeroed");
}

int main(void) {
    char root[] = "/tmp/reconos-theme-XXXXXX";
    if (mkdtemp(root) == NULL) {
        perror("mkdtemp");
        return 1;
    }

    printf("ReconOS skin tests, root %s\n\n", root);

    if (!recon_fs_init(root)) {
        printf("could not set up the test filesystem: %s\n", recon_fs_last_error());
        return 1;
    }
    recon_registry_init();
    recon_theme_write_defaults();
    recon_theme_init();

    test_roles_are_all_answered();
    test_no_role_is_unset();
    test_out_of_range_is_loud();
    test_switching();
    test_remembered();
    test_files();
    test_damaged_file();

    recon_theme_finish();
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
