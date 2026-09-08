/*
 * What a package says it is, and who signed it. See include/recon_package.h
 * for the format and src/recon_manifest.h for what this shares with the
 * installer.
 *
 * Split out of recon_package.c, which installs. The two were one file, and
 * testing the half that is arithmetic on bytes meant linking the half that
 * loads a shared object into this process -- so the signature tests needed a
 * stub file that aborted on the module loader, the icon cache and the version
 * comparison. Five functions stubbed to make one file testable is the shape of
 * a file that wants splitting, and recon_http.c had already been split for the
 * same reason.
 *
 * Nothing in here loads anything, writes to /Apps, or touches the registry.
 * It reads a folder and takes digests of what it finds.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "recon_crypt.h"
#include "recon_fs.h"
#include "recon_manifest.h"
#include "recon_package.h"
#include "recon_sign.h"

static char g_error[256];

void recon_package_set_error(const char *fmt, ...) {
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
void recon_package_trim(char *text) {
    size_t end = strlen(text);
    while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
            text[end - 1] == '\r')) {
        text[--end] = '\0';
    }
}

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
    recon_package_trim(first);

    snprintf(second, second_size, "%s", space + 1);
    return first[0] != '\0' && second[0] != '\0';
}

bool recon_package_place_allowed(const char *into) {
    for (size_t i = 0; i < sizeof(PLACES_ALLOWED) / sizeof(PLACES_ALLOWED[0]);
            i++) {
        if (strcmp(PLACES_ALLOWED[i], into) == 0) {
            return true;
        }
    }
    return false;
}

bool recon_package_read_manifest(const char *package, struct manifest *out) {
    char path[RECON_PATH_MAX];
    if (!recon_fs_join(path, sizeof(path), package, RECON_PACKAGE_MANIFEST)) {
        recon_package_set_error("that path is too long");
        return false;
    }

    size_t size = 0;
    char *text = recon_fs_read("/", path, &size);
    if (text == NULL) {
        recon_package_set_error("'%s' has no " RECON_PACKAGE_MANIFEST, package);
        return false;
    }

    memset(out, 0, sizeof(*out));

    /* Set if the manifest asked for more than can be held, so the refusal
     * below can name which limit it was rather than saying something is
     * wrong. */
    bool too_many_places = false;
    bool too_many_settings = false;

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
        recon_package_trim(line);

        char *value = equals + 1;
        while (*value == ' ' || *value == '\t') {
            value++;
        }
        recon_package_trim(value);

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
        } else if (strcasecmp(line, "place") == 0) {
            /*
             * Refused rather than trimmed.
             *
             * Silently dropping the seventeenth `place` line would install
             * most of a package and report success, and the missing file
             * would turn up as something not working weeks later. The rule
             * here is the same as everywhere else in this system: a thing
             * that will not fit is said about, not shortened.
             */
            if (out->place_count >= PLACES_MAX) {
                too_many_places = true;
                continue;
            }
            struct place *p = &out->places[out->place_count];
            if (split_two(value, p->file, sizeof(p->file),
                    p->into, sizeof(p->into))) {
                out->place_count++;
            }
            /* A line that will not split is skipped rather than refused, the
             * same as any other line this does not understand -- but a line
             * naming a directory that is not allowed is a different thing and
             * is refused at install, where it can be said out loud. */
        } else if (strcasecmp(line, "setting") == 0) {
            if (out->setting_count >= SETTINGS_MAX) {
                too_many_settings = true;
                continue;
            }
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

    /*
     * More than can be held is a refusal, not a trim.
     *
     * Silently dropping the seventeenth `place` line would install most of a
     * package and report success, and the missing file would turn up as
     * something not working weeks later.
     */
    if (too_many_places) {
        recon_package_set_error("'%s' wants to place more than %d files",
            out->info.name, PLACES_MAX);
        return false;
    }
    if (too_many_settings) {
        recon_package_set_error("'%s' wants to set more than %d settings",
            out->info.name, SETTINGS_MAX);
        return false;
    }

    if (out->info.name[0] == '\0') {
        recon_package_set_error("that package does not say what it is called");
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
        recon_package_set_error("'%s' is not a usable package name", out->info.name);
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
        recon_package_set_error("'%s' brings no program and no files, so there is nothing "
            "to install", out->info.name);
        return false;
    }
    if (strchr(out->module, '/') != NULL || strchr(out->icon, '/') != NULL) {
        recon_package_set_error("a package names files beside its manifest, not paths");
        return false;
    }

    if (out->info.version[0] == '\0') {
        snprintf(out->info.version, sizeof(out->info.version), "unknown");
    }
    return true;
}

bool recon_package_read(const char *path, struct recon_package_info *out) {
    struct manifest manifest;
    if (!recon_package_read_manifest(path, &manifest)) {
        return false;
    }
    if (out != NULL) {
        *out = manifest.info;
    }
    return true;
}


/* --- Who made this --- */

/*
 * What a signature covers.
 *
 * Not the manifest. Signing the manifest alone would bind the *names* of the
 * files and none of their contents, so somebody could keep the signed manifest
 * and replace the module beside it -- which is the only file that matters and
 * the whole reason to sign anything here.
 *
 * So the signed object is a list of digests: one line per file the package
 * brings, sorted by name, with the manifest itself first. Changing any byte of
 * any of them changes a digest and the signature stops verifying.
 *
 *     reconos-package-v1
 *     <sha256 hex>  package.txt
 *     <sha256 hex>  Notes.rex
 *     <sha256 hex>  notes.png
 *
 * Sorted, because the order a directory hands back its entries is not a
 * property of the package and two machines must build the same bytes from the
 * same folder. The version line is there so a later format cannot be verified
 * as though it were this one.
 */
#define DIGEST_LINE_MAX (RECON_SHA256_SIZE * 2 + RECON_NAME_MAX + 8)
#define DIGESTS_MAX (PLACES_MAX + 4)

/* The name of the file holding the signature, inside the package. */
#define PACKAGE_SIGNATURE "package.sig"

static bool digest_of(const char *package, const char *name, char *out) {
    char path[RECON_PATH_MAX];
    if (!recon_fs_join(path, sizeof(path), package, name)) {
        return false;
    }

    size_t size = 0;
    char *bytes = recon_fs_read("/", path, &size);
    if (bytes == NULL) {
        recon_package_set_error("'%s' is named in the manifest and is not in the package",
            name);
        return false;
    }

    uint8_t digest[RECON_SHA256_SIZE];
    recon_sha256(bytes, size, digest);
    free(bytes);

    recon_to_hex(digest, sizeof(digest), out);
    return true;
}

/*
 * Every file the manifest names, once each, in sorted order.
 *
 * Once each because a manifest may name the same file twice -- an icon that is
 * also placed, say -- and a digest list with a repeat in it is a list that
 * depends on how the manifest was written rather than on what the package
 * contains.
 */
static int collect_names(const struct manifest *m,
        char names[DIGESTS_MAX][RECON_NAME_MAX]) {
    int count = 0;

    const char *candidates[DIGESTS_MAX];
    int candidate_count = 0;

    if (m->module[0] != '\0') {
        candidates[candidate_count++] = m->module;
    }
    if (m->icon[0] != '\0') {
        candidates[candidate_count++] = m->icon;
    }
    for (int i = 0; i < m->place_count && candidate_count < DIGESTS_MAX; i++) {
        candidates[candidate_count++] = m->places[i].file;
    }

    for (int i = 0; i < candidate_count; i++) {
        bool already = false;
        for (int j = 0; j < count; j++) {
            if (strcmp(names[j], candidates[i]) == 0) {
                already = true;
                break;
            }
        }
        if (!already && count < DIGESTS_MAX) {
            snprintf(names[count], RECON_NAME_MAX, "%s", candidates[i]);
            count++;
        }
    }

    /*
     * Sorted, so the bytes do not depend on the order a manifest happened to
     * list things in. Insertion sort: there are at most nine of these.
     *
     * memcpy of whole rows rather than snprintf between them. The rows are
     * distinct so there is no real overlap, but snprintf with a source and
     * destination the compiler cannot prove disjoint is undefined behaviour
     * on its face, and -Wrestrict says so. Moving fixed-size rows is what
     * this is actually doing.
     */
    for (int i = 1; i < count; i++) {
        char held[RECON_NAME_MAX];
        memcpy(held, names[i], RECON_NAME_MAX);
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], held) > 0) {
            memcpy(names[j + 1], names[j], RECON_NAME_MAX);
            j--;
        }
        memcpy(names[j + 1], held, RECON_NAME_MAX);
    }
    return count;
}

/*
 * Build the bytes a signature is over.
 *
 * Returns false with the error set when a named file is missing, which is
 * itself worth refusing on: a package that names a file it does not contain
 * cannot be signed for, because there is nothing to take a digest of.
 */
static bool digest_list(const char *package, const struct manifest *m,
        char *out, size_t size) {
    size_t used = 0;
    int n = snprintf(out, size, "reconos-package-v1\n");
    if (n < 0 || (size_t)n >= size) {
        return false;
    }
    used = (size_t)n;

    char hex[RECON_SHA256_SIZE * 2 + 1];
    if (!digest_of(package, RECON_PACKAGE_MANIFEST, hex)) {
        return false;
    }
    n = snprintf(out + used, size - used, "%s  %s\n", hex,
        RECON_PACKAGE_MANIFEST);
    if (n < 0 || (size_t)n >= size - used) {
        return false;
    }
    used += (size_t)n;

    char names[DIGESTS_MAX][RECON_NAME_MAX];
    int count = collect_names(m, names);

    for (int i = 0; i < count; i++) {
        if (!digest_of(package, names[i], hex)) {
            return false;
        }
        n = snprintf(out + used, size - used, "%s  %s\n", hex, names[i]);
        if (n < 0 || (size_t)n >= size - used) {
            recon_package_set_error("that package names more files than this can sign for");
            return false;
        }
        used += (size_t)n;
    }
    return true;
}

bool recon_package_sign(const char *path) {
    struct manifest m;
    if (!recon_package_read_manifest(path, &m)) {
        return false;
    }

    static char listing[DIGESTS_MAX * DIGEST_LINE_MAX + 64];
    if (!digest_list(path, &m, listing, sizeof(listing))) {
        return false;
    }

    char signature[RECON_SIGN_MAX];
    if (!recon_sign_data(listing, strlen(listing), signature,
            sizeof(signature))) {
        recon_package_set_error("%s", recon_sign_last_error());
        return false;
    }

    char sig_path[RECON_PATH_MAX];
    if (!recon_fs_join(sig_path, sizeof(sig_path), path, PACKAGE_SIGNATURE)) {
        recon_package_set_error("there is nowhere to put the signature");
        return false;
    }

    char file[RECON_SIGN_MAX + 8];
    int n = snprintf(file, sizeof(file), "%s\n", signature);
    if (n < 0 || !recon_fs_write("/", sig_path, file, (size_t)n)) {
        recon_package_set_error("%s", recon_fs_last_error());
        return false;
    }
    return true;
}

/*
 * Is this package signed by a key this machine trusts?
 *
 * `signer` is filled in when it is. The failure messages are deliberately
 * different for "no signature" and "a signature that does not verify": the
 * first is a package that was never signed, which is a thing somebody can fix
 * by signing it, and the second is a package that has been changed since it
 * was, which is not.
 */
bool recon_package_signed_by(const char *path, char *signer,
        size_t signer_size) {
    if (signer != NULL && signer_size > 0) {
        signer[0] = '\0';
    }

    struct manifest m;
    if (!recon_package_read_manifest(path, &m)) {
        return false;
    }

    char sig_path[RECON_PATH_MAX];
    if (!recon_fs_join(sig_path, sizeof(sig_path), path, PACKAGE_SIGNATURE)) {
        recon_package_set_error("that package has no signature");
        return false;
    }

    size_t sig_size = 0;
    char *sig = recon_fs_read("/", sig_path, &sig_size);
    if (sig == NULL) {
        /*
         * The message says what to do, because the person reading it has a
         * package they built and no reason to know signing exists. A refusal
         * that names the next command is a refusal somebody can act on; one
         * that does not is a wall.
         */
        recon_package_set_error("that package is not signed -- 'sign %s' signs it with this "
            "machine's key", path);
        return false;
    }

    /*
     * The line, and only the line.
     *
     * Not `trim`, which strips spaces, tabs and carriage returns and
     * deliberately leaves newlines alone -- it is for manifest values, where a
     * line has already been split off. Using it here left the trailing newline
     * on the signature, which made its length odd, which made it fail the hex
     * check: every package signed correctly and no package verified. Worth the
     * four lines to cut at the first line ending rather than to reason about
     * which whitespace somebody else's helper happens to remove.
     */
    for (char *c = sig; *c != '\0'; c++) {
        if (*c == '\n' || *c == '\r') {
            *c = '\0';
            break;
        }
    }
    recon_package_trim(sig);

    static char listing[DIGESTS_MAX * DIGEST_LINE_MAX + 64];
    if (!digest_list(path, &m, listing, sizeof(listing))) {
        free(sig);
        return false;
    }

    bool ok = recon_sign_verify(listing, strlen(listing), sig,
        signer, signer_size);
    if (!ok) {
        recon_package_set_error("%s", recon_sign_last_error());
    }
    free(sig);
    return ok;
}

