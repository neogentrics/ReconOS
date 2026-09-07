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
#include "recon_registry.h"
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
static void trim(char *text) {
    size_t end = strlen(text);
    while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
            text[end - 1] == '\r')) {
        text[--end] = '\0';
    }
}

/* How many files a package may place, and how many settings it may set. */
#define PLACES_MAX 16
#define SETTINGS_MAX 16

struct place {
    char file[RECON_NAME_MAX];
    char into[RECON_PATH_MAX];
};

struct setting {
    char key[128];
    char value[128];
};

struct manifest {
    struct recon_package_info info;
    char module[RECON_NAME_MAX];
    char icon[RECON_NAME_MAX];

    struct place places[PLACES_MAX];
    int place_count;

    struct setting settings[SETTINGS_MAX];
    int setting_count;
};

/*
 * Where a package may put a file.
 *
 * An allow-list, and the reasoning is in recon_package.h: a list of forbidden
 * places is a list somebody has to keep complete, and the day it is missing an
 * entry is the day a package writes into /System/Config. A list of permitted
 * places is wrong in the safe direction.
 *
 * /Apps is not here. A module already goes there by being named `module`, and
 * a package that could drop a second thing into the directory scanned at
 * startup could bring code it did not declare.
 */
static const char *const PLACES_ALLOWED[] = {
    RECON_DIR_SYSTEM_ICONS,
    "/System/Wallpapers",
    "/System/Themes",
    "/System/Fonts",
    "/System/Sounds",
};

/*
 * Split "a.png /System/Wallpapers" into its two halves.
 *
 * The last space separates them, not the first, because a file may have a
 * space in its name and a directory in ReconOS may not begin with one. False
 * when there is only one word, which is a line somebody meant something by and
 * that this cannot act on.
 */
static bool split_two(const char *text, char *first, size_t first_size,
        char *second, size_t second_size) {
    const char *space = strrchr(text, ' ');
    if (space == NULL || space == text || space[1] == '\0') {
        return false;
    }

    size_t length = (size_t)(space - text);
    if (length >= first_size) {
        return false;
    }
    memcpy(first, text, length);
    first[length] = '\0';
    trim(first);

    snprintf(second, second_size, "%s", space + 1);
    return first[0] != '\0' && second[0] != '\0';
}

static bool place_is_allowed(const char *into) {
    for (size_t i = 0; i < sizeof(PLACES_ALLOWED) / sizeof(PLACES_ALLOWED[0]);
            i++) {
        if (strcmp(PLACES_ALLOWED[i], into) == 0) {
            return true;
        }
    }
    return false;
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
        } else if (strcasecmp(line, "place") == 0 &&
                out->place_count < PLACES_MAX) {
            struct place *p = &out->places[out->place_count];
            if (split_two(value, p->file, sizeof(p->file),
                    p->into, sizeof(p->into))) {
                out->place_count++;
            }
            /* A line that will not split is skipped rather than refused, the
             * same as any other line this does not understand -- but a line
             * naming a directory that is not allowed is a different thing and
             * is refused at install, where it can be said out loud. */
        } else if (strcasecmp(line, "setting") == 0 &&
                out->setting_count < SETTINGS_MAX) {
            struct setting *g = &out->settings[out->setting_count];
            if (split_two(value, g->key, sizeof(g->key),
                    g->value, sizeof(g->value))) {
                out->setting_count++;
            }
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

    /*
     * A package must bring something, and code is not the only something.
     *
     * This required a module, which made a package the wrapper for a program
     * and nothing else -- so a wallpaper pack, a skin pack or a set of fonts
     * had no way to be one, even though placing files is exactly what they are
     * for. The question now is whether it brings anything at all, which is the
     * honest version: a manifest with neither code nor a file to place
     * describes something that would do nothing on installing and nothing on
     * removal.
     */
    if (out->module[0] == '\0' && out->place_count == 0) {
        set_error("'%s' brings no program and no files, so there is nothing "
            "to install", out->info.name);
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
    if (wrote_count != NULL) {
        *wrote_count = 0;
    }

    bool in_files = false;
    bool in_settings = false;
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

        if (!place_is_allowed(p->into)) {
            for (int k = 0; k < count; k++) {
                recon_fs_remove("/", placed[k]);
            }
            set_error("'%s' wants to put a file in %s, which packages may not "
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
        set_error("could not record what was installed, so nothing was");
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
        set_error("nothing to remove");
        return false;
    }
    if (!recon_users_may_administer()) {
        set_error("only an administrator can remove a program");
        return false;
    }

    char placed[PLACED_MAX][RECON_PATH_MAX];
    int count = 0;
    char wrote[SETTINGS_MAX][128];
    int wrote_count = 0;
    if (!read_receipt(name, NULL, placed, &count, wrote, &wrote_count)) {
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
