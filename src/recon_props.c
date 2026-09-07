/*
 * Properties. See include/recon_props.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <time.h>

#include "recon_codec.h"
#include "recon_icons.h"
#include "recon_modules.h"
#include "recon_registry.h"
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
    if (strcasecmp(dot, ".md") == 0)    { return "Text file"; }
    if (strcasecmp(dot, ".log") == 0)   { return "Log"; }

    /*
     * Pictures, all of them, rather than only PNG. The Type column said "File"
     * for a JPEG that Photos would happily open -- which is the column
     * disagreeing with the rest of the system about what it is looking at.
     */
    if (strcasecmp(dot, ".png") == 0 || strcasecmp(dot, ".jpg") == 0 ||
            strcasecmp(dot, ".jpeg") == 0 || strcasecmp(dot, ".bmp") == 0 ||
            strcasecmp(dot, ".gif") == 0 || strcasecmp(dot, ".tga") == 0) {
        return "Picture";
    }

    if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0) {
        return "Web page";
    }
    if (strcasecmp(dot, ".xml") == 0)   { return "XML document"; }
    if (strcasecmp(dot, ".json") == 0)  { return "JSON data"; }
    if (strcasecmp(dot, ".csv") == 0)   { return "Table"; }
    if (strcasecmp(dot, ".ini") == 0 || strcasecmp(dot, ".conf") == 0) {
        return "Settings";
    }

    if (strcasecmp(dot, ".ttf") == 0 || strcasecmp(dot, ".otf") == 0 ||
            strcasecmp(dot, ".ttc") == 0) {
        return "Font";
    }

    /*
     * Sound and video, including the formats nothing here can decode.
     *
     * A file is what it is regardless of what can open it, and "Video" beside
     * a message saying it cannot be played is more use than "File" beside the
     * same message. The Type column said "File" for a .mp4 whose icon already
     * said video, which is two parts of one window disagreeing.
     */
    if (recon_codec_handles_extension(name) ||
            strcasecmp(dot, ".flac") == 0 || strcasecmp(dot, ".ogg") == 0 ||
            strcasecmp(dot, ".opus") == 0 || strcasecmp(dot, ".m4a") == 0 ||
            strcasecmp(dot, ".aac") == 0) {
        return "Sound";
    }
    if (strcasecmp(dot, ".mp4") == 0 || strcasecmp(dot, ".mkv") == 0 ||
            strcasecmp(dot, ".webm") == 0 || strcasecmp(dot, ".avi") == 0 ||
            strcasecmp(dot, ".mov") == 0 || strcasecmp(dot, ".m4v") == 0) {
        return "Video";
    }

    if (strcasecmp(dot, ".ico") == 0)   { return "Icon"; }
    if (strcasecmp(dot, ".theme") == 0) { return "Skin"; }
    if (strcasecmp(dot, ".reg") == 0)   { return "Settings"; }
    if (strcasecmp(dot, ".rex") == 0)   { return "Application"; }
    if (strcasecmp(dot, ".rts") == 0)   { return "System module"; }
    if (strcasecmp(dot, ".rpk") == 0)   { return "Package"; }
    if (strcasecmp(dot, ".lnk") == 0)   { return "Shortcut"; }
    return "File";
}

/* --- Choosing what opens a kind of file --- */

/*
 * Where one extension's chosen application is written down.
 *
 * Keyed by the extension including its dot and lowercased, so ".PNG" and
 * ".png" are one setting -- choosing a program for a photograph and finding it
 * did not apply to the next photograph because the camera shouts its file
 * names would be a setting that looks broken.
 */
static bool opener_key(const char *name, char *out, size_t size) {
    const char *dot = strrchr(name != NULL ? name : "", '.');
    if (dot == NULL || dot == name || dot[1] == '\0') {
        return false;
    }

    int at = snprintf(out, size, "open-with/");
    if (at < 0 || (size_t)at >= size) {
        return false;
    }

    size_t i = (size_t)at;
    for (const char *p = dot; *p != '\0' && i + 1 < size; p++, i++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        out[i] = c;
    }
    out[i] = '\0';
    return i > (size_t)at;
}

int recon_props_openers(const char **names, int max) {
    int count = 0;
    int total = recon_installed_app_count();

    for (int i = 0; i < total && count < max; i++) {
        struct recon_installed_app app;
        if (!recon_installed_app_at(i, &app) || app.disabled) {
            continue;
        }
        if (app.opens[0] == '\0') {
            continue;
        }
        names[count++] = recon_installed_app_resolve(app.name);
    }
    return count;
}

const char *recon_props_chosen_opener(const char *name) {
    char key[128];
    if (!opener_key(name, key, sizeof(key))) {
        return NULL;
    }

    const char *chosen = recon_registry_get(RECON_REG_USER, key, "");
    if (chosen[0] == '\0') {
        return NULL;
    }

    /*
     * Only if it still exists.
     *
     * An application chosen and then uninstalled would otherwise leave a file
     * type pointing at nothing, and double-clicking it would do nothing at all
     * with no way to find out why. Falling back to the declared answer is what
     * somebody would want and is also what happens if this is never cleaned
     * up -- so it is not cleaned up here, because a read that quietly writes
     * is a surprise nobody needs.
     */
    return recon_installed_app_resolve(chosen);
}

bool recon_props_set_opener(const char *name, const char *application) {
    char key[128];
    if (!opener_key(name, key, sizeof(key)) || application == NULL) {
        return false;
    }
    return recon_registry_set(RECON_REG_USER, key, application);
}

bool recon_props_clear_opener(const char *name) {
    char key[128];
    if (!opener_key(name, key, sizeof(key))) {
        return false;
    }
    recon_registry_remove(RECON_REG_USER, key);
    return true;
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
     * What somebody chose, before anything anybody declared.
     *
     * Everything below is inheritance -- an application says what it opens and
     * the system agrees -- and a choice is not inheritance. It is the only
     * rule here that came from a person, so it is the only one allowed to
     * contradict the rest.
     */
    const char *chosen = recon_props_chosen_opener(name);
    if (chosen != NULL) {
        return chosen;
    }

    /*
     * What an application says it opens.
     *
     * This used to be a list of extensions here, which meant the answer to
     * "what opens a .png?" lived a long way from the application that opens
     * one -- and a module bringing an application that reads a format had no
     * way to say so at all: the file sat on the desktop, correctly named and
     * correctly drawn, and double-clicking it did nothing.
     *
     * The list now lives on each registration, so a replacement applet
     * inherits the association or overrides it by the same rule that decides
     * everything else about a replacement, and turning an application off
     * hands its file types back -- because an "off" that leaves the
     * associations behind is not off.
     */
    const char *registered = recon_installed_app_for_file(name);
    if (registered != NULL) {
        return registered;
    }

    /*
     * Sound and video, asked of the codec registry rather than listed
     * anywhere.
     *
     * The one association that cannot be a string on a registration: what the
     * player opens is whatever can be decoded, and that changes when a module
     * brings a decoder. Written down once, it would be a list that is wrong
     * from the moment somebody installs anything.
     */
    if (recon_codec_handles_extension(name)) {
        return "Media Player";
    }

    /*
     * A module is not a document, and neither is a font or a package. They
     * return NULL rather than being handed to Notepad, which would show a
     * screen of binary and look like the file was damaged.
     */
    return NULL;
}

const char *recon_props_icon(const char *name) {
    const char *dot = (name != NULL) ? strrchr(name, '.') : NULL;
    if (dot == NULL || dot == name || dot[1] == '\0') {
        return RECON_ICON_FILE;
    }

    /*
     * Sound and video are asked of the codec registry where they can be, so a
     * decoder a module brings makes its files look right without this list
     * knowing about it. The video extensions are listed here because nothing
     * decodes video yet -- the file still has a kind even when nothing can
     * open it, and an icon that says "video" beside a message that says "this
     * cannot be played" is more informative than a blank sheet.
     */
    if (recon_codec_handles_extension(name)) {
        return RECON_ICON_FILE_SOUND;
    }
    if (strcasecmp(dot, ".mp4") == 0 || strcasecmp(dot, ".mkv") == 0 ||
            strcasecmp(dot, ".webm") == 0 || strcasecmp(dot, ".avi") == 0 ||
            strcasecmp(dot, ".mov") == 0 || strcasecmp(dot, ".m4v") == 0) {
        return RECON_ICON_FILE_VIDEO;
    }
    /* Sound formats nothing here decodes yet. Same reasoning as video: the
     * file is what it is regardless of what can open it. */
    if (strcasecmp(dot, ".flac") == 0 || strcasecmp(dot, ".ogg") == 0 ||
            strcasecmp(dot, ".opus") == 0 || strcasecmp(dot, ".m4a") == 0 ||
            strcasecmp(dot, ".aac") == 0) {
        return RECON_ICON_FILE_SOUND;
    }

    if (strcasecmp(dot, ".png") == 0 || strcasecmp(dot, ".jpg") == 0 ||
            strcasecmp(dot, ".jpeg") == 0 || strcasecmp(dot, ".bmp") == 0 ||
            strcasecmp(dot, ".gif") == 0 || strcasecmp(dot, ".tga") == 0 ||
            strcasecmp(dot, ".ico") == 0) {
        return RECON_ICON_FILE_IMAGE;
    }

    if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0 ||
            strcasecmp(dot, ".xml") == 0) {
        return RECON_ICON_FILE_WEB;
    }

    if (strcasecmp(dot, ".json") == 0 || strcasecmp(dot, ".csv") == 0 ||
            strcasecmp(dot, ".reg") == 0 || strcasecmp(dot, ".ini") == 0 ||
            strcasecmp(dot, ".conf") == 0 || strcasecmp(dot, ".theme") == 0) {
        return RECON_ICON_FILE_DATA;
    }

    if (strcasecmp(dot, ".ttf") == 0 || strcasecmp(dot, ".otf") == 0 ||
            strcasecmp(dot, ".ttc") == 0) {
        return RECON_ICON_FILE_FONT;
    }

    if (strcasecmp(dot, ".rpk") == 0 || strcasecmp(dot, ".zip") == 0 ||
            strcasecmp(dot, ".tar") == 0 || strcasecmp(dot, ".gz") == 0) {
        return RECON_ICON_FILE_ARCHIVE;
    }

    /* Applications and modules already have icons of their own. */
    if (strcasecmp(dot, ".rex") == 0) {
        return RECON_ICON_APP;
    }
    if (strcasecmp(dot, ".rts") == 0) {
        return RECON_ICON_MODULES;
    }

    return RECON_ICON_FILE;
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
