/*
 * The fonts on this machine. See include/recon_fonts.h.
 *
 * Deliberately the same shape as recon_wallpaper.c, down to the origins file.
 * The two questions are the same question -- "what files of this kind does
 * the system have, and which of them did somebody put there" -- and answering
 * them two different ways would mean two sets of rules about what can be
 * removed.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ReconOS.h"
#include "recon_access.h"
#include "recon_fonts.h"
#include "recon_fs.h"
#include "recon_registry.h"

#define FONTS_MAX 64
#define NAME_MAX_LEN 96

static char g_names[FONTS_MAX][NAME_MAX_LEN];
static int g_count;
static char g_error[192];

const char *recon_fonts_last_error(void) {
    return g_error;
}

/* Whether the name ends in something this system can load. */
static bool looks_like_a_font(const char *name) {
    static const char *const KINDS[] = { ".ttf", ".otf", ".ttc", NULL };

    size_t length = strlen(name);
    for (int i = 0; KINDS[i] != NULL; i++) {
        size_t want = strlen(KINDS[i]);
        if (length > want &&
                strcasecmp(name + length - want, KINDS[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void scan(void) {
    g_count = 0;

    struct recon_dirent entries[FONTS_MAX];
    int found = recon_fs_list("/", RECON_DIR_FONTS, entries, FONTS_MAX);
    if (found < 0) {
        return;
    }
    if (found > FONTS_MAX) {
        found = FONTS_MAX;
    }

    for (int i = 0; i < found && g_count < FONTS_MAX; i++) {
        if (entries[i].kind == RECON_FILE_DIRECTORY) {
            continue;
        }
        if (!looks_like_a_font(entries[i].name)) {
            continue;
        }
        /*
         * A name too long to hold is left out rather than cut down. The name
         * is what the file is asked for by later, so a shortened one is not a
         * font with a shorter name -- it is a row in the list that cannot be
         * loaded, offered to somebody who will click it.
         */
        if (strlen(entries[i].name) >= NAME_MAX_LEN) {
            continue;
        }
        recon_text_copy(g_names[g_count], NAME_MAX_LEN, entries[i].name);
        g_count++;
    }
}

int recon_fonts_count(void) {
    scan();
    return g_count;
}

bool recon_fonts_at(int index, char *out, size_t size) {
    scan();
    if (index < 0 || index >= g_count || out == NULL) {
        return false;
    }
    snprintf(out, size, "%s", g_names[index]);
    return true;
}

bool recon_fonts_path(const char *name, char *out, size_t size) {
    if (name == NULL || *name == '\0' || out == NULL) {
        return false;
    }
    return recon_fs_join(out, size, RECON_DIR_FONTS, name);
}

/* --- Where each one came from --- */

/*
 * One line per installed font: `name|the path it was copied from`.
 *
 * A plain text file rather than the registry, because the registry's keys
 * cannot hold a space and a path can.
 */
static void remember_origin(const char *name, const char *from) {
    char line[RECON_PATH_MAX + NAME_MAX_LEN + 2];
    int length = snprintf(line, sizeof(line), "%s|%s\n", name, from);
    if (length <= 0 || (size_t)length >= sizeof(line)) {
        return;
    }

    if (!recon_fs_append("/", RECON_FONT_ORIGINS, line, (size_t)length)) {
        recon_fs_write("/", RECON_FONT_ORIGINS, line, (size_t)length);
    }
}

bool recon_fonts_origin(const char *name, char *out, size_t size) {
    if (name == NULL || out == NULL || size == 0) {
        return false;
    }
    out[0] = '\0';

    size_t length = 0;
    char *text = recon_fs_read("/", RECON_FONT_ORIGINS, &length);
    if (text == NULL) {
        return false;
    }

    bool found = false;
    size_t wanted = strlen(name);

    char *line = text;
    while (line != NULL && *line != '\0') {
        char *end = strchr(line, '\n');
        if (end != NULL) {
            *end = '\0';
        }

        const char *bar = strchr(line, '|');
        if (bar != NULL && (size_t)(bar - line) == wanted &&
                strncmp(line, name, wanted) == 0) {
            snprintf(out, size, "%s", bar + 1);
            found = true;
            /* The last line wins: a font removed and added again writes a
             * second line, and the second one is the true one. */
        }

        line = (end != NULL) ? end + 1 : NULL;
    }

    free(text);
    return found;
}

/* Drop one line from the origins file, matched on the name before the bar. */
static void forget_origin(const char *name) {
    size_t length = 0;
    char *text = recon_fs_read("/", RECON_FONT_ORIGINS, &length);
    if (text == NULL) {
        return;
    }

    char kept[4096];
    size_t used = 0;
    size_t wanted = strlen(name);

    char *line = text;
    while (line != NULL && *line != '\0') {
        char *end = strchr(line, '\n');
        if (end != NULL) {
            *end = '\0';
        }

        /* Compared against the part before the bar, and copied whole:
         * splitting a line to compare it and then rebuilding it is how a line
         * comes back with its second half twice. */
        const char *bar = strchr(line, '|');
        size_t label = (bar != NULL) ? (size_t)(bar - line) : strlen(line);
        bool mine = (label == wanted) && (strncmp(line, name, label) == 0);

        if (!mine && *line != '\0') {
            int n = snprintf(kept + used, sizeof(kept) - used, "%s\n", line);
            if (n > 0 && (size_t)n < sizeof(kept) - used) {
                used += (size_t)n;
            }
        }

        line = (end != NULL) ? end + 1 : NULL;
    }

    free(text);
    recon_fs_write("/", RECON_FONT_ORIGINS, kept, used);
}

/* --- Adding and removing --- */

bool recon_fonts_add(const char *cwd, const char *path, char *name_out,
        size_t name_size) {
    if (path == NULL || *path == '\0') {
        snprintf(g_error, sizeof(g_error), "no file named");
        return false;
    }

    char canonical[RECON_PATH_MAX];
    char host[RECON_PATH_MAX];
    if (!recon_fs_resolve(cwd, path, host, sizeof(host), canonical,
            sizeof(canonical))) {
        snprintf(g_error, sizeof(g_error), "there is no '%s'", path);
        return false;
    }

    const char *leaf = strrchr(canonical, '/');
    leaf = (leaf != NULL && leaf[1] != '\0') ? leaf + 1 : canonical;

    if (!looks_like_a_font(leaf)) {
        /* The leaf rather than the path: a message wide enough for a full
         * path is a message that wraps, and the name is the part that says
         * what was wrong with it. */
        snprintf(g_error, sizeof(g_error),
            "'%.64s' is not a font file -- .ttf, .otf or .ttc", leaf);
        return false;
    }

    recon_fs_mkdir("/", RECON_DIR_FONTS);

    /* Refused rather than shortened. A name cut to fit is a different name,
     * and the list is looked up by name. */
    if (strlen(leaf) >= RECON_NAME_MAX) {
        snprintf(g_error, sizeof(g_error), "that name is too long");
        return false;
    }

    char base[RECON_NAME_MAX];
    char extension[RECON_NAME_MAX] = "";
    memcpy(base, leaf, strlen(leaf) + 1);
    char *dot = strrchr(base, '.');
    if (dot != NULL && dot != base) {
        snprintf(extension, sizeof(extension), "%s", dot);
        *dot = '\0';
    }

    /* Two folders can each hold an Inter.ttf, and the second one to arrive
     * must not replace the first. */
    char name[RECON_NAME_MAX];
    if (!recon_fs_unique_name("/", RECON_DIR_FONTS, base, extension,
            name, sizeof(name))) {
        snprintf(g_error, sizeof(g_error), "cannot find a free name for it");
        return false;
    }

    char target[RECON_PATH_MAX];
    if (!recon_fs_join(target, sizeof(target), RECON_DIR_FONTS, name)) {
        snprintf(g_error, sizeof(g_error), "that name is too long");
        return false;
    }

    if (!recon_fs_copy("/", canonical, target)) {
        snprintf(g_error, sizeof(g_error), "%s", recon_fs_last_error());
        return false;
    }

    remember_origin(name, canonical);

    if (name_out != NULL && name_size > 0) {
        snprintf(name_out, name_size, "%s", name);
    }
    return true;
}

bool recon_fonts_remove(const char *name) {
    if (name == NULL || *name == '\0') {
        snprintf(g_error, sizeof(g_error), "no font named");
        return false;
    }

    char origin[RECON_PATH_MAX];
    if (!recon_fonts_origin(name, origin, sizeof(origin))) {
        snprintf(g_error, sizeof(g_error),
            "'%s' ships with ReconOS and cannot be removed", name);
        return false;
    }

    /*
     * Refused while it is the one being read from.
     *
     * The setting holds a path rather than a name, so this compares the path
     * this font would have -- the setting is what the desktop is actually
     * loading, and matching on anything else would be guessing.
     */
    char path[RECON_PATH_MAX];
    if (!recon_fonts_path(name, path, sizeof(path))) {
        snprintf(g_error, sizeof(g_error), "that name is too long");
        return false;
    }

    const char *chosen = recon_registry_get(RECON_REG_USER,
        RECON_ACCESS_FONT_KEY, "");
    if (strcmp(chosen, path) == 0) {
        snprintf(g_error, sizeof(g_error),
            "'%s' is the font in use. Choose another one first", name);
        return false;
    }

    if (!recon_fs_remove("/", path)) {
        snprintf(g_error, sizeof(g_error), "%s", recon_fs_last_error());
        return false;
    }

    /* And the line saying where it came from, which would otherwise be
     * waiting to attach itself to the next font to arrive under this name. */
    forget_origin(name);
    return true;
}
