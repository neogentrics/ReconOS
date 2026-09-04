/*
 * Properties. See include/recon_props.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <time.h>

#include "recon_fs.h"
#include "recon_props.h"

void recon_props_size(size_t bytes, char *out, size_t size) {
    if (out == NULL || size == 0) {
        return;
    }

    /*
     * Bytes exactly, below a kilobyte. Rounding 900 bytes to "0.9 KB" hides
     * the one thing somebody looking at a small file wants to know, which is
     * whether it has anything in it.
     */
    if (bytes < 1024) {
        snprintf(out, size, "%zu byte%s", bytes, bytes == 1 ? "" : "s");
        return;
    }

    static const char *const UNITS[] = { "KB", "MB", "GB", "TB" };
    double value = (double)bytes / 1024.0;
    int unit = 0;

    while (value >= 1024.0 && unit < (int)(sizeof(UNITS) / sizeof(UNITS[0])) - 1) {
        value /= 1024.0;
        unit++;
    }

    /* One decimal below ten, none above: "1.4 MB" says something "1 MB" does
     * not, and "247.3 MB" says nothing "247 MB" does not. */
    if (value < 10.0) {
        snprintf(out, size, "%.1f %s", value, UNITS[unit]);
    } else {
        snprintf(out, size, "%.0f %s", value, UNITS[unit]);
    }
}

/* The folder a path is in, and the name at the end of it. */
static void split(const char *canonical, char *folder, size_t folder_size,
        const char **name) {
    const char *slash = strrchr(canonical, '/');
    if (slash == NULL) {
        snprintf(folder, folder_size, "/");
        *name = canonical;
        return;
    }

    if (slash == canonical) {
        /* Something directly in the root: the folder is "/", not "". */
        snprintf(folder, folder_size, "/");
    } else {
        snprintf(folder, folder_size, "%.*s", (int)(slash - canonical), canonical);
    }
    *name = (slash[1] != '\0') ? slash + 1 : canonical;
}

const char *recon_props_kind(const struct recon_dirent *entry,
        const char *name) {
    if (entry == NULL || name == NULL) {
        return "File";
    }
    if (entry->kind == RECON_FILE_DIRECTORY) {
        return "Folder";
    }

    /*
     * By extension, because that is what the rest of the system goes by --
     * the explorer's icons, what Notepad will open. Saying "File" for
     * everything would be true and useless.
     */
    const char *dot = strrchr(name, '.');
    if (dot == NULL || dot == name || dot[1] == '\0') {
        return "File";
    }
    if (strcasecmp(dot, ".txt") == 0)   { return "Text file"; }
    if (strcasecmp(dot, ".png") == 0)   { return "Picture"; }
    if (strcasecmp(dot, ".ico") == 0)   { return "Icon"; }
    if (strcasecmp(dot, ".theme") == 0) { return "Skin"; }
    if (strcasecmp(dot, ".reg") == 0)   { return "Settings"; }
    if (strcasecmp(dot, ".rex") == 0)   { return "Application"; }
    if (strcasecmp(dot, ".rts") == 0)   { return "System module"; }
    if (strcasecmp(dot, ".lnk") == 0)   { return "Shortcut"; }
    return "File";
}

const char *recon_props_opener(const char *name) {
    if (name == NULL) {
        return NULL;
    }

    const char *dot = strrchr(name, '.');
    if (dot == NULL || dot == name || dot[1] == '\0') {
        return NULL;
    }

    /*
     * Text and things that are text underneath. A skin and a settings file
     * are both editable by hand on purpose -- that is most of why they are
     * text -- so opening one should put it in front of somebody rather than
     * refusing because the extension is unfamiliar.
     */
    if (strcasecmp(dot, ".txt") == 0 ||
            strcasecmp(dot, ".theme") == 0 ||
            strcasecmp(dot, ".reg") == 0 ||
            strcasecmp(dot, ".md") == 0 ||
            strcasecmp(dot, ".log") == 0) {
        return "Notepad";
    }

    /*
     * A picture has nowhere to go yet, and a module is not a document. Both
     * return NULL rather than being handed to Notepad, which would show a
     * screen of binary and look like the file was damaged.
     */
    return NULL;
}

bool recon_props_describe(const char *cwd, const char *path,
        char *out, size_t size) {
    if (out == NULL || size == 0) {
        return false;
    }
    out[0] = '\0';

    struct recon_dirent entry;
    if (!recon_fs_stat(cwd, path, &entry)) {
        snprintf(out, size, "%s", recon_fs_last_error());
        return false;
    }

    /*
     * The path as the filesystem understands it, so the folder line says
     * where the thing actually is rather than repeating what was typed.
     */
    char host[RECON_PATH_MAX];
    char canonical[RECON_PATH_MAX];
    if (!recon_fs_resolve(cwd, path, host, sizeof(host),
            canonical, sizeof(canonical))) {
        snprintf(canonical, sizeof(canonical), "%s", path);
    }

    char folder[RECON_PATH_MAX];
    const char *name = NULL;
    split(canonical, folder, sizeof(folder), &name);

    char measure[64];
    if (entry.kind == RECON_FILE_DIRECTORY) {
        /*
         * How many things are in it, not how many bytes the directory itself
         * occupies -- which is a number about the filesystem rather than
         * about anything the person put there.
         */
        int items = recon_fs_list(cwd, path, NULL, 0);
        if (items < 0) {
            snprintf(measure, sizeof(measure), "contents unreadable");
        } else {
            snprintf(measure, sizeof(measure), "%d item%s", items,
                items == 1 ? "" : "s");
        }
    } else {
        recon_props_size(entry.size, measure, sizeof(measure));
    }

    char when[64];
    if (entry.modified == 0) {
        snprintf(when, sizeof(when), "not known");
    } else {
        struct tm parts;
        localtime_r(&entry.modified, &parts);
        strftime(when, sizeof(when), "%e %B %Y at %H:%M", &parts);

        /* %e pads a single-digit day with a space. */
        char *text = when;
        while (*text == ' ') {
            text++;
        }
        memmove(when, text, strlen(text) + 1);
    }

    snprintf(out, size,
        "%s\n"
        "%s, %s\n"
        "In %s\n"
        "Changed %s",
        name, recon_props_kind(&entry, name), measure, folder, when);
    return true;
}
