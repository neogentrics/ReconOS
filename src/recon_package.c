/*
 * Packages. See include/recon_package.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ReconOS.h"
#include "recon_fs.h"
#include "recon_icons.h"
#include "recon_modules.h"
#include "recon_manifest.h"
#include "recon_package.h"
#include "recon_sign.h"
#include "recon_error.h"
#include "recon_registry.h"
#include "recon_version.h"
#include "recon_users.h"

#define RECEIPT_EXT ".txt"
#define PLACED_MAX 32

/* --- Receipts --- */

static void receipt_path(const char *name, char *out, size_t size) {
    snprintf(out, size, "%s/%s%s", RECON_DIR_RECEIPTS, name, RECEIPT_EXT);
}

bool recon_package_installed(const char *name) {
    if (name == NULL || *name == '\0') {
        return false;
    }
    char path[RECON_PATH_MAX];
    receipt_path(name, path, sizeof(path));
    return recon_fs_exists("/", path);
}

/*
 * Write the receipt.
 *
 * The description first, then the placed files one per line under a marker.
 * One file rather than two, because a receipt split across two files is a
 * receipt that can be half deleted.
 */
static bool write_receipt(const struct manifest *manifest,
        const char placed[PLACED_MAX][RECON_PATH_MAX], int count,
        const char wrote[SETTINGS_MAX][128], int wrote_count) {
    char text[PLACED_MAX * RECON_PATH_MAX + SETTINGS_MAX * 128 + 512];
    size_t used = 0;

    int n = snprintf(text, sizeof(text),
        "# What this package is, and what installing it put where.\n"
        "# Removing the package removes exactly the files listed below.\n"
        "name = %s\n"
        "version = %s\n"
        "publisher = %s\n"
        "description = %s\n"
        "files:\n",
        manifest->info.name, manifest->info.version,
        manifest->info.publisher, manifest->info.description);
    if (n < 0) {
        return false;
    }
    used = (size_t)n;

    for (int i = 0; i < count && used < sizeof(text); i++) {
        n = snprintf(text + used, sizeof(text) - used, "%s\n", placed[i]);
        if (n < 0) {
            break;
        }
        used += (size_t)n;
    }

    /*
     * The settings this install actually wrote, under their own marker.
     *
     * Only the ones it wrote. A package may name a setting that already had a
     * value, and that one is left alone at install and must be left alone at
     * removal -- writing it down here would mean uninstalling took away
     * something somebody had chosen.
     */
    if (wrote_count > 0 && used < sizeof(text)) {
        n = snprintf(text + used, sizeof(text) - used, "settings:\n");
        if (n > 0) {
            used += (size_t)n;
        }
        for (int i = 0; i < wrote_count && used < sizeof(text); i++) {
            n = snprintf(text + used, sizeof(text) - used, "%s\n", wrote[i]);
            if (n < 0) {
                break;
            }
            used += (size_t)n;
        }
    }

    char path[RECON_PATH_MAX];
    receipt_path(manifest->info.name, path, sizeof(path));

    recon_fs_mkdir("/", RECON_DIR_RECEIPTS);
    return recon_fs_write("/", path, text, used);
}

/* Read a receipt's description, and optionally the files it names. */
static bool read_receipt(const char *name, struct recon_package_info *info,
        char placed[PLACED_MAX][RECON_PATH_MAX], int *count,
        char wrote[SETTINGS_MAX][128], int *wrote_count) {
    char path[RECON_PATH_MAX];
    receipt_path(name, path, sizeof(path));

    size_t size = 0;
    char *text = recon_fs_read("/", path, &size);
    if (text == NULL) {
        recon_package_set_error("nothing called '%s' is installed", name);
        return false;
    }

    if (info != NULL) {
        memset(info, 0, sizeof(*info));
        snprintf(info->name, sizeof(info->name), "%s", name);
    }
    if (count != NULL) {
        *count = 0;
    }
    if (wrote_count != NULL) {
        *wrote_count = 0;
    }

    bool in_files = false;
    bool in_settings = false;
    char *saveptr = NULL;
    for (char *line = strtok_r(text, "\n", &saveptr);
            line != NULL;
            line = strtok_r(NULL, "\n", &saveptr)) {

        recon_package_trim(line);
        if (*line == '\0' || *line == '#') {
            continue;
        }

        if (strcmp(line, "files:") == 0) {
            in_files = true;
            in_settings = false;
            continue;
        }
        if (strcmp(line, "settings:") == 0) {
            in_settings = true;
            in_files = false;
            continue;
        }

        if (in_settings) {
            if (wrote != NULL && wrote_count != NULL &&
                    *wrote_count < SETTINGS_MAX) {
                snprintf(wrote[*wrote_count], 128, "%s", line);
                (*wrote_count)++;
            }
            continue;
        }

        if (in_files) {
            if (placed != NULL && count != NULL && *count < PLACED_MAX) {
                snprintf(placed[*count], RECON_PATH_MAX, "%s", line);
                (*count)++;
            }
            continue;
        }

        char *equals = strchr(line, '=');
        if (equals == NULL || info == NULL) {
            continue;
        }
        *equals = '\0';
        recon_package_trim(line);
        char *value = equals + 1;
        while (*value == ' ') {
            value++;
        }

        if (strcasecmp(line, "version") == 0) {
            snprintf(info->version, sizeof(info->version), "%s", value);
        } else if (strcasecmp(line, "publisher") == 0) {
            snprintf(info->publisher, sizeof(info->publisher), "%s", value);
        } else if (strcasecmp(line, "description") == 0) {
            snprintf(info->description, sizeof(info->description), "%s", value);
        }
    }

    free(text);
    return true;
}

/* --- Installing --- */


bool recon_package_install(const char *path) {
    if (path == NULL || *path == '\0') {
        recon_package_set_error("nothing to install");
        return false;
    }
    if (!recon_users_may_administer()) {
        recon_package_set_error("only an administrator can install a program");
        return false;
    }

    struct recon_dirent entry;
    if (!recon_fs_stat("/", path, &entry) ||
            entry.kind != RECON_FILE_DIRECTORY) {
        recon_package_set_error("'%s' is not a package folder", path);
        return false;
    }

    /*
     * --- Who made this ---
     *
     * Checked before anything is read out of the manifest and acted on, and
     * with no way to say "install it anyway".
     *
     * A package brings a module; a module is loaded into this process; a
     * module in this process can do everything ReconOS can do. The allow-list
     * below bounds where a package may *place a file* and says nothing at all
     * about what its code does once running -- so the signature is the only
     * thing standing between a person and somebody else's code running as
     * them.
     *
     * There is no --force. A check that can be turned off is a check that is
     * off on the day it matters, and the person who would use the flag is
     * exactly the person being attacked. Somebody who means to install
     * something unsigned signs it: `packages sign` does that with the
     * machine's own key, which is one command, and doing it deliberately is
     * the point.
     */
    char signer[RECON_SIGN_NAME_MAX];
    if (!recon_package_signed_by(path, signer, sizeof(signer))) {
        recon_error_raisef(NULL, RECON_ERR_E003, "%s",
            recon_package_last_error());
        return false;
    }

    struct manifest manifest;
    if (!recon_package_read_manifest(path, &manifest)) {
        /*
         * Raised here rather than in read_manifest, which several callers use
         * only to have a look at a folder.
         *
         * It was in the reader, and `install` on a folder that is not a
         * package produced two identical lines in the log for one action --
         * once from the install and once from the read that builds the
         * message about it. Installing is the thing that failed; looking is
         * not.
         */
        recon_error_raisef(NULL, RECON_ERR_E003, "%s",
            recon_package_last_error());
        return false;
    }

    if (recon_package_installed(manifest.info.name)) {
        recon_package_set_error("'%s' is already installed", manifest.info.name);
        return false;
    }

    char placed[PLACED_MAX][RECON_PATH_MAX];
    int count = 0;

    /*
     * The code, if it brought any.
     *
     * A package with no module is a package of content -- a wallpaper pack, a
     * set of skins -- and is a real thing. What it must not be is a package
     * that says it has code and does not: a manifest naming a file that is not
     * in the folder is a package that was built wrong, and installing most of
     * it would leave somebody with a program that is not there.
     */
    char from[RECON_PATH_MAX];
    char to[RECON_PATH_MAX];
    bool has_module = manifest.module[0] != '\0';

    if (has_module) {
        if (!recon_fs_join(from, sizeof(from), path, manifest.module) ||
                !recon_fs_join(to, sizeof(to), RECON_DIR_APPS,
                    manifest.module)) {
            recon_package_set_error("that path is too long");
            return false;
        }

        if (!recon_fs_exists("/", from)) {
            recon_package_set_error("'%s' says its code is in %s, which is not there",
                manifest.info.name, manifest.module);
            return false;
        }
        if (recon_fs_exists("/", to)) {
            recon_package_set_error("something is already installed at %s", to);
            return false;
        }
        if (!recon_fs_copy("/", from, to)) {
            recon_package_set_error("%s", recon_fs_last_error());
            return false;
        }
        snprintf(placed[count++], RECON_PATH_MAX, "%s", to);
    }

    /* Its icon, if it brought one. */
    if (manifest.icon[0] != '\0') {
        char icon_to[RECON_PATH_MAX];
        if (recon_fs_join(from, sizeof(from), path, manifest.icon) &&
                recon_fs_join(icon_to, sizeof(icon_to),
                    RECON_DIR_SYSTEM_ICONS, manifest.icon) &&
                recon_fs_exists("/", from)) {

            /*
             * An icon that is already there is left alone and not recorded,
             * so uninstalling this package cannot take away an icon that
             * belonged to something else.
             */
            if (!recon_fs_exists("/", icon_to) &&
                    recon_fs_copy("/", from, icon_to)) {
                snprintf(placed[count++], RECON_PATH_MAX, "%s", icon_to);
            }
        }
    }

    /*
     * The files the package brought, each into a directory that is allowed to
     * hold that kind of thing.
     */
    for (int i = 0; i < manifest.place_count; i++) {
        const struct place *p = &manifest.places[i];

        if (!recon_package_place_allowed(p->into)) {
            for (int k = 0; k < count; k++) {
                recon_fs_remove("/", placed[k]);
            }
            recon_package_set_error("'%s' wants to put a file in %s, which packages may not "
                "write to", manifest.info.name, p->into);
            return false;
        }

        char place_to[RECON_PATH_MAX];
        if (!recon_fs_join(from, sizeof(from), path, p->file) ||
                !recon_fs_join(place_to, sizeof(place_to), p->into, p->file)) {
            continue;
        }
        if (!recon_fs_exists("/", from)) {
            /* Named and not shipped. Skipped rather than refused: a manifest
             * that mentions a file the package forgot is a package with one
             * thing missing, not a package that should not install. */
            continue;
        }

        /*
         * Something already there is left alone and not recorded -- the same
         * rule the icon follows, and for the same reason: uninstalling this
         * package must not take away a file that belonged to something else.
         */
        if (recon_fs_exists("/", place_to)) {
            continue;
        }
        if (count >= PLACED_MAX) {
            break;
        }
        if (recon_fs_copy("/", from, place_to)) {
            snprintf(placed[count++], RECON_PATH_MAX, "%s", place_to);
        }
    }

    /*
     * The defaults, and only the ones that were not already answered.
     *
     * A key that already has a value is somebody's choice, and an install that
     * overwrote it would be an install rearranging their desk. Only what is
     * written here goes in the receipt, so removing the package takes back
     * exactly what it added.
     */
    char wrote[SETTINGS_MAX][128];
    int wrote_count = 0;

    for (int i = 0; i < manifest.setting_count &&
            wrote_count < SETTINGS_MAX; i++) {
        const struct setting *g = &manifest.settings[i];
        if (recon_registry_has(RECON_REG_SYSTEM, g->key)) {
            continue;
        }
        if (recon_registry_set(RECON_REG_SYSTEM, g->key, g->value)) {
            snprintf(wrote[wrote_count++], 128, "%s", g->key);
        }
    }

    /*
     * The receipt before the module is loaded.
     *
     * Loading is the step that can fail in ways this cannot predict -- a
     * module built against a different ABI, a module that refuses. If it
     * fails, the files are already on disk and the receipt is what makes them
     * removable rather than litter.
     */
    if (!write_receipt(&manifest, placed, count, wrote, wrote_count)) {
        for (int i = 0; i < count; i++) {
            recon_fs_remove("/", placed[i]);
        }
        for (int i = 0; i < wrote_count; i++) {
            recon_registry_remove(RECON_REG_SYSTEM, wrote[i]);
        }
        recon_package_set_error("could not record what was installed, so nothing was");
        return false;
    }

    recon_icons_forget();

    /*
     * A package of content is finished here: the files are placed and the
     * receipt is written, and there is nothing to load.
     *
     * The themes and wallpapers it may have brought are read from disk when
     * they are asked for, so they are already available -- there is no
     * equivalent of recon_icons_forget for them to need.
     */
    if (!has_module) {
        return true;
    }

    if (!recon_modules_load(to)) {
        char reason[192];
        snprintf(reason, sizeof(reason), "%s", recon_modules_last_error());
        recon_package_uninstall(manifest.info.name);
        recon_package_set_error("'%s' would not load: %s", manifest.info.name, reason);
        return false;
    }

    return true;
}

/* The suffix an upgrade parks the old files under. Long and unlikely on
 * purpose: it briefly shares a directory with whatever somebody has installed,
 * and a collision here would delete the wrong file. */
#define ASIDE_SUFFIX ".replaced-by-upgrade"

/*
 * Move a file out of the way, or back again.
 *
 * Nothing is created or destroyed either way, so a failure leaves the file
 * where it was, which is what makes the caller's rollback able to be a loop
 * with no bookkeeping in it.
 */
static bool move_aside(const char *file, bool back) {
    char aside[RECON_PATH_MAX];
    int written = snprintf(aside, sizeof(aside), "%s" ASIDE_SUFFIX, file);
    if (written < 0 || (size_t)written >= sizeof(aside)) {
        return false;
    }
    return back ? recon_fs_rename("/", aside, file)
                : recon_fs_rename("/", file, aside);
}

bool recon_package_upgrade(const char *path) {
    if (!recon_users_may_administer()) {
        recon_package_set_error("only an administrator can upgrade a program");
        return false;
    }

    /* What is being offered. Read before anything is touched, so a package
     * that is not one is refused without the installed version noticing. */
    struct recon_package_info incoming;
    if (!recon_package_read(path, &incoming)) {
        return false;
    }

    struct recon_package_info current;
    bool found = false;
    int installed = recon_package_count();
    for (int i = 0; i < installed && !found; i++) {
        struct recon_package_info info;
        if (recon_package_at(i, &info) &&
                strcmp(info.name, incoming.name) == 0) {
            current = info;
            found = true;
        }
    }

    if (!found) {
        recon_package_set_error("'%s' is not installed, so there is nothing to upgrade",
            incoming.name);
        return false;
    }

    /*
     * Strictly newer.
     *
     * `bad` comes back true when either side is not a version at all, and
     * that is a refusal rather than a guess: an upgrade removes a working
     * program, and doing that on the strength of an unreadable number is
     * exactly the kind of decision this system is written not to make.
     */
    bool bad = false;
    int order = recon_version_compare_text(incoming.version, current.version,
        &bad);
    if (bad) {
        recon_package_set_error("'%s' or '%s' is not a version this can compare",
            incoming.version, current.version);
        return false;
    }
    if (order == 0) {
        recon_package_set_error("'%s' %s is already installed; to reinstall it, remove it "
            "first", incoming.name, current.version);
        return false;
    }
    if (order < 0) {
        recon_package_set_error("'%s' %s is older than the installed %s; removing it and "
            "installing this is the way to go back on purpose",
            incoming.name, incoming.version, current.version);
        return false;
    }

    /* What the installed one put where. Read now, because the receipt is
     * about to be replaced. */
    char old_files[PLACED_MAX][RECON_PATH_MAX];
    int old_count = 0;
    if (!read_receipt(incoming.name, NULL, old_files, &old_count, NULL, NULL)) {
        return false;
    }

    /*
     * Unloaded before its file moves, for the reason the uninstall gives: a
     * module whose file has gone while it is still loaded is a program whose
     * code is in memory and whose home is not.
     */
    for (int i = 0; i < old_count; i++) {
        if (strstr(old_files[i], RECON_DIR_APPS) != old_files[i]) {
            continue;
        }
        const char *leaf = strrchr(old_files[i], '/');
        leaf = (leaf != NULL) ? leaf + 1 : old_files[i];

        char module_name[RECON_NAME_MAX];
        recon_text_copy(module_name, sizeof(module_name), leaf);
        char *dot = strrchr(module_name, '.');
        if (dot != NULL) {
            *dot = '\0';
        }
        recon_modules_unload(module_name);
    }

    /* Aside, one at a time, remembering how far it got. */
    int moved = 0;
    bool ok = true;
    for (int i = 0; i < old_count && ok; i++) {
        if (!recon_fs_exists("/", old_files[i])) {
            /* Already gone by somebody's hand. Counted as moved so the
             * rollback below does not try to bring back what was not there,
             * and not an error for the same reason uninstall forgives it. */
            moved++;
            continue;
        }
        if (move_aside(old_files[i], false)) {
            moved++;
        } else {
            recon_package_set_error("'%s' could not be moved out of the way: %s",
                old_files[i], recon_fs_last_error());
            ok = false;
        }
    }

    /* The receipt goes too, so the install below does not see the old one and
     * refuse. Kept in memory, which is what the rollback puts back. */
    char receipt[RECON_PATH_MAX];
    receipt_path(incoming.name, receipt, sizeof(receipt));
    if (ok && !move_aside(receipt, false)) {
        recon_package_set_error("the receipt for '%s' could not be moved out of the way",
            incoming.name);
        ok = false;
    }

    if (ok) {
        ok = recon_package_install(path);
    }

    if (ok) {
        /*
         * It worked, so the old files go for good.
         *
         * Last, and only now. Up to this line every failure could put things
         * back; after it, the upgrade has happened.
         */
        for (int i = 0; i < old_count; i++) {
            char aside[RECON_PATH_MAX];
            if (snprintf(aside, sizeof(aside), "%s" ASIDE_SUFFIX,
                    old_files[i]) > 0 && recon_fs_exists("/", aside)) {
                recon_fs_remove("/", aside);
            }
        }
        char old_receipt[RECON_PATH_MAX];
        if (snprintf(old_receipt, sizeof(old_receipt), "%s" ASIDE_SUFFIX,
                receipt) > 0 && recon_fs_exists("/", old_receipt)) {
            recon_fs_remove("/", old_receipt);
        }
        recon_icons_forget();
        return true;
    }

    /*
     * It did not, so the old one comes back.
     *
     * The reason this is worth the trouble: the obvious way to write an
     * upgrade is uninstall then install, and an upgrade that fails halfway
     * that way has removed a program that worked and put nothing in its
     * place. Somebody trying to get a newer version ends up with no version.
     */
    char why[256];
    snprintf(why, sizeof(why), "%s", recon_package_last_error());

    /* Whatever the failed install managed to place, out of the way first --
     * otherwise the old files have nowhere to come back to. */
    recon_package_uninstall(incoming.name);

    move_aside(receipt, true);
    for (int i = 0; i < moved && i < old_count; i++) {
        move_aside(old_files[i], true);
    }

    /* Loaded again, so the program that was working before this started is
     * working after it. */
    for (int i = 0; i < old_count; i++) {
        if (strstr(old_files[i], RECON_DIR_APPS) == old_files[i]) {
            recon_modules_load(old_files[i]);
        }
    }
    recon_icons_forget();

    recon_error_raisef(NULL, RECON_ERR_E004,
        "upgrading '%s' from %s to %s failed and the installed version has "
        "been put back: %s", incoming.name, current.version, incoming.version,
        why);
    recon_package_set_error("'%s' was not upgraded and %s is still installed: %s",
        incoming.name, current.version, why);
    return false;
}

bool recon_package_verify(const char *name, int *placed, int *missing,
        char *first_missing, size_t size) {
    if (placed != NULL) {
        *placed = 0;
    }
    if (missing != NULL) {
        *missing = 0;
    }
    if (first_missing != NULL && size > 0) {
        first_missing[0] = '\0';
    }

    char files[PLACED_MAX][RECON_PATH_MAX];
    int count = 0;
    if (!read_receipt(name, NULL, files, &count, NULL, NULL)) {
        return false;
    }

    int gone = 0;
    for (int i = 0; i < count; i++) {
        if (recon_fs_exists("/", files[i])) {
            continue;
        }
        if (gone == 0 && first_missing != NULL && size > 0) {
            snprintf(first_missing, size, "%s", files[i]);
        }
        gone++;
    }

    if (placed != NULL) {
        *placed = count;
    }
    if (missing != NULL) {
        *missing = gone;
    }
    return true;
}

bool recon_package_uninstall(const char *name) {
    if (name == NULL || *name == '\0') {
        recon_package_set_error("nothing to remove");
        return false;
    }
    if (!recon_users_may_administer()) {
        recon_package_set_error("only an administrator can remove a program");
        return false;
    }

    char placed[PLACED_MAX][RECON_PATH_MAX];
    int count = 0;
    char wrote[SETTINGS_MAX][128];
    int wrote_count = 0;
    if (!read_receipt(name, NULL, placed, &count, wrote, &wrote_count)) {
        /*
         * Not VT-E005, though it was for one draft.
         *
         * read_receipt fails only when the file is not there, and that means
         * "nothing called this is installed" -- which is a different thing
         * from "a program could not be removed", and putting that code here
         * would send somebody looking up a removal that never started. A code
         * on the wrong condition is worse than a code nothing raises, which is
         * the whole argument the roadmap makes about unreachable ones.
         */
        return false;
    }

    /*
     * The settings this install wrote, and only those.
     *
     * Removed rather than set back to something, because there was nothing
     * there before: the install only wrote keys that had no value, which is
     * what makes taking them away the exact opposite of putting them there.
     */
    for (int i = 0; i < wrote_count; i++) {
        recon_registry_remove(RECON_REG_SYSTEM, wrote[i]);
    }

    /*
     * Unload before deleting. A module whose file has gone while it is still
     * loaded is a program whose code is in memory and whose home is not,
     * which is a state nothing else in the system knows how to describe.
     */
    /*
     * By name, because that is what the module system is keyed on. The file
     * placed in /Apps is named for the module, so its leaf without the
     * extension is the name it registered under.
     */
    for (int i = 0; i < count; i++) {
        if (strstr(placed[i], RECON_DIR_APPS) != placed[i]) {
            continue;
        }
        const char *leaf = strrchr(placed[i], '/');
        leaf = (leaf != NULL) ? leaf + 1 : placed[i];

        char module_name[RECON_NAME_MAX];
        recon_text_copy(module_name, sizeof(module_name), leaf);
        char *dot = strrchr(module_name, '.');
        if (dot != NULL) {
            *dot = '\0';
        }
        recon_modules_unload(module_name);
    }

    for (int i = 0; i < count; i++) {
        /* A file somebody already deleted by hand is not an error. Refusing
         * would make a package impossible to uninstall because of a tidy-up
         * that happened months ago. */
        if (recon_fs_exists("/", placed[i])) {
            recon_fs_remove("/", placed[i]);
        }
    }

    char receipt[RECON_PATH_MAX];
    receipt_path(name, receipt, sizeof(receipt));
    recon_fs_remove("/", receipt);

    recon_icons_forget();
    return true;
}

/* --- What is installed --- */

static int list_receipts(char names[][64], int max) {
    struct recon_dirent entries[64];
    int found = recon_fs_list("/", RECON_DIR_RECEIPTS, entries, 64);
    if (found < 0) {
        return 0;
    }
    if (found > 64) {
        found = 64;
    }

    int count = 0;
    for (int i = 0; i < found && count < max; i++) {
        size_t length = strlen(entries[i].name);
        size_t suffix = strlen(RECEIPT_EXT);
        if (entries[i].kind == RECON_FILE_DIRECTORY || length <= suffix) {
            continue;
        }
        if (strcasecmp(entries[i].name + length - suffix, RECEIPT_EXT) != 0) {
            continue;
        }
        snprintf(names[count], 64, "%.*s", (int)(length - suffix),
            entries[i].name);
        count++;
    }
    return count;
}

int recon_package_count(void) {
    char names[64][64];
    return list_receipts(names, 64);
}

bool recon_package_at(int index, struct recon_package_info *out) {
    char names[64][64];
    int count = list_receipts(names, 64);
    if (index < 0 || index >= count) {
        return false;
    }
    return read_receipt(names[index], out, NULL, NULL, NULL, NULL);
}
