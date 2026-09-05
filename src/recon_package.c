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
#include "recon_package.h"
#include "recon_users.h"

#define RECEIPT_EXT ".txt"
#define PLACED_MAX 32

static char g_error[256];

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, args);
    va_end(args);
}

const char *recon_package_last_error(void) {
    return g_error[0] != '\0' ? g_error : "no error";
}

/* --- The manifest --- */

/*
 * What a manifest says, including the parts only the installer needs.
 *
 * Kept apart from recon_package_info, which is what anybody else is shown:
 * the name of the file inside the package holding the code is an installer's
 * business, and putting it in the public struct would invite somebody to act
 * on a path relative to a folder that may no longer be there.
 */
struct manifest {
    struct recon_package_info info;
    char module[RECON_NAME_MAX];
    char icon[RECON_NAME_MAX];
};

static void trim(char *text) {
    size_t end = strlen(text);
    while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
            text[end - 1] == '\r')) {
        text[--end] = '\0';
    }
}

static bool read_manifest(const char *package, struct manifest *out) {
    char path[RECON_PATH_MAX];
    if (!recon_fs_join(path, sizeof(path), package, RECON_PACKAGE_MANIFEST)) {
        set_error("that path is too long");
        return false;
    }

    size_t size = 0;
    char *text = recon_fs_read("/", path, &size);
    if (text == NULL) {
        set_error("'%s' has no " RECON_PACKAGE_MANIFEST, package);
        return false;
    }

    memset(out, 0, sizeof(*out));

    char *saveptr = NULL;
    for (char *line = strtok_r(text, "\n", &saveptr);
            line != NULL;
            line = strtok_r(NULL, "\n", &saveptr)) {

        while (*line == ' ' || *line == '\t') {
            line++;
        }
        if (*line == '\0' || *line == '#') {
            continue;
        }

        char *equals = strchr(line, '=');
        if (equals == NULL) {
            continue;
        }
        *equals = '\0';
        trim(line);

        char *value = equals + 1;
        while (*value == ' ' || *value == '\t') {
            value++;
        }
        trim(value);

        if (strcasecmp(line, "name") == 0) {
            snprintf(out->info.name, sizeof(out->info.name), "%s", value);
        } else if (strcasecmp(line, "version") == 0) {
            snprintf(out->info.version, sizeof(out->info.version), "%s", value);
        } else if (strcasecmp(line, "publisher") == 0) {
            snprintf(out->info.publisher, sizeof(out->info.publisher), "%s",
                value);
        } else if (strcasecmp(line, "description") == 0) {
            snprintf(out->info.description, sizeof(out->info.description),
                "%s", value);
        } else if (strcasecmp(line, "module") == 0) {
            snprintf(out->module, sizeof(out->module), "%s", value);
        } else if (strcasecmp(line, "icon") == 0) {
            snprintf(out->icon, sizeof(out->icon), "%s", value);
        }
        /* Anything else is skipped rather than refused, so a package built
         * for a later ReconOS still installs on this one. */
    }

    free(text);

    if (out->info.name[0] == '\0') {
        set_error("that package does not say what it is called");
        return false;
    }

    /*
     * A name is used to build paths, so it cannot contain a separator. A
     * package calling itself "../../System/Config" would otherwise write its
     * receipt somewhere it has no business.
     */
    if (strchr(out->info.name, '/') != NULL ||
            strcmp(out->info.name, ".") == 0 ||
            strcmp(out->info.name, "..") == 0) {
        set_error("'%s' is not a usable package name", out->info.name);
        return false;
    }

    if (out->module[0] == '\0') {
        set_error("'%s' does not say which file holds its code",
            out->info.name);
        return false;
    }
    if (strchr(out->module, '/') != NULL || strchr(out->icon, '/') != NULL) {
        set_error("a package names files beside its manifest, not paths");
        return false;
    }

    if (out->info.version[0] == '\0') {
        snprintf(out->info.version, sizeof(out->info.version), "unknown");
    }
    return true;
}

bool recon_package_read(const char *path, struct recon_package_info *out) {
    struct manifest manifest;
    if (!read_manifest(path, &manifest)) {
        return false;
    }
    if (out != NULL) {
        *out = manifest.info;
    }
    return true;
}

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
        const char placed[PLACED_MAX][RECON_PATH_MAX], int count) {
    char text[PLACED_MAX * RECON_PATH_MAX + 512];
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

    char path[RECON_PATH_MAX];
    receipt_path(manifest->info.name, path, sizeof(path));

    recon_fs_mkdir("/", RECON_DIR_RECEIPTS);
    return recon_fs_write("/", path, text, used);
}

/* Read a receipt's description, and optionally the files it names. */
static bool read_receipt(const char *name, struct recon_package_info *info,
        char placed[PLACED_MAX][RECON_PATH_MAX], int *count) {
    char path[RECON_PATH_MAX];
    receipt_path(name, path, sizeof(path));

    size_t size = 0;
    char *text = recon_fs_read("/", path, &size);
    if (text == NULL) {
        set_error("nothing called '%s' is installed", name);
        return false;
    }

    if (info != NULL) {
        memset(info, 0, sizeof(*info));
        snprintf(info->name, sizeof(info->name), "%s", name);
    }
    if (count != NULL) {
        *count = 0;
    }

    bool in_files = false;
    char *saveptr = NULL;
    for (char *line = strtok_r(text, "\n", &saveptr);
            line != NULL;
            line = strtok_r(NULL, "\n", &saveptr)) {

        trim(line);
        if (*line == '\0' || *line == '#') {
            continue;
        }

        if (strcmp(line, "files:") == 0) {
            in_files = true;
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
        trim(line);
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
        set_error("nothing to install");
        return false;
    }
    if (!recon_users_may_administer()) {
        set_error("only an administrator can install a program");
        return false;
    }

    struct recon_dirent entry;
    if (!recon_fs_stat("/", path, &entry) ||
            entry.kind != RECON_FILE_DIRECTORY) {
        set_error("'%s' is not a package folder", path);
        return false;
    }

    struct manifest manifest;
    if (!read_manifest(path, &manifest)) {
        return false;
    }

    if (recon_package_installed(manifest.info.name)) {
        set_error("'%s' is already installed", manifest.info.name);
        return false;
    }

    char placed[PLACED_MAX][RECON_PATH_MAX];
    int count = 0;

    /* The code. Named for the package rather than for the file it came from,
     * so /Apps reads as a list of programs. */
    char from[RECON_PATH_MAX];
    char to[RECON_PATH_MAX];

    if (!recon_fs_join(from, sizeof(from), path, manifest.module) ||
            !recon_fs_join(to, sizeof(to), RECON_DIR_APPS, manifest.module)) {
        set_error("that path is too long");
        return false;
    }

    if (!recon_fs_exists("/", from)) {
        set_error("'%s' says its code is in %s, which is not there",
            manifest.info.name, manifest.module);
        return false;
    }
    if (recon_fs_exists("/", to)) {
        set_error("something is already installed at %s", to);
        return false;
    }
    if (!recon_fs_copy("/", from, to)) {
        set_error("%s", recon_fs_last_error());
        return false;
    }
    snprintf(placed[count++], RECON_PATH_MAX, "%s", to);

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
     * The receipt before the module is loaded.
     *
     * Loading is the step that can fail in ways this cannot predict -- a
     * module built against a different ABI, a module that refuses. If it
     * fails, the files are already on disk and the receipt is what makes them
     * removable rather than litter.
     */
    if (!write_receipt(&manifest, placed, count)) {
        for (int i = 0; i < count; i++) {
            recon_fs_remove("/", placed[i]);
        }
        set_error("could not record what was installed, so nothing was");
        return false;
    }

    recon_icons_forget();

    if (!recon_modules_load(to)) {
        char reason[192];
        snprintf(reason, sizeof(reason), "%s", recon_modules_last_error());
        recon_package_uninstall(manifest.info.name);
        set_error("'%s' would not load: %s", manifest.info.name, reason);
        return false;
    }

    return true;
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
    if (!read_receipt(name, NULL, files, &count)) {
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
        set_error("nothing to remove");
        return false;
    }
    if (!recon_users_may_administer()) {
        set_error("only an administrator can remove a program");
        return false;
    }

    char placed[PLACED_MAX][RECON_PATH_MAX];
    int count = 0;
    if (!read_receipt(name, NULL, placed, &count)) {
        return false;
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
    return read_receipt(names[index], out, NULL, NULL);
}
