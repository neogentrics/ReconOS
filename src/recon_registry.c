/*
 * The ReconOS registry. See include/recon_registry.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_fs.h"
#include "recon_registry.h"

#define ENTRIES_MAX 512

struct entry {
    char key[RECON_REGISTRY_KEY_MAX];
    char value[RECON_REGISTRY_VALUE_MAX];
    bool used;
};

/* One hive: the entries, and where they are written. */
struct hive {
    struct entry entries[ENTRIES_MAX];
    int count;
    bool loaded;
};

static struct hive g_system;
static struct hive g_user;
static char g_error[256];

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, args);
    va_end(args);
}

const char *recon_registry_last_error(void) {
    return g_error[0] != '\0' ? g_error : "no error";
}

static struct hive *hive_for(enum recon_registry_scope scope) {
    return scope == RECON_REG_SYSTEM ? &g_system : &g_user;
}

/*
 * Where a hive is written.
 *
 * The user's is asked for each time rather than remembered, because the
 * account can change while the system runs and a remembered path would keep
 * writing one person's settings into another's file.
 */
static void hive_path(enum recon_registry_scope scope, char *out, size_t size) {
    if (scope == RECON_REG_SYSTEM) {
        snprintf(out, size, "%s", RECON_REGISTRY_SYSTEM_FILE);
    } else {
        snprintf(out, size, "%s/%s", recon_fs_user_dir(NULL),
            RECON_REGISTRY_USER_FILE);
    }
}

/* --- Keys --- */

/*
 * A key is a path: letters, digits, and the few punctuation marks that read
 * as part of a name. No leading or trailing slash, and no empty segments, so
 * "a//b" and "a/b/" cannot be two spellings of the same thing.
 */
static bool key_is_valid(const char *key) {
    if (key == NULL || *key == '\0') {
        return false;
    }
    if (strlen(key) >= RECON_REGISTRY_KEY_MAX) {
        return false;
    }
    if (key[0] == '/' || key[strlen(key) - 1] == '/') {
        return false;
    }

    bool previous_was_slash = false;
    for (const char *c = key; *c != '\0'; c++) {
        if (*c == '/') {
            if (previous_was_slash) {
                return false;
            }
            previous_was_slash = true;
            continue;
        }
        previous_was_slash = false;

        if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_' && *c != '.') {
            return false;
        }
    }
    return true;
}

/*
 * Whether `key` is at or under `prefix`.
 *
 * Compared by segment, so "windows/Notepad" does not match
 * "windows/NotepadPlus" -- a prefix that catches unrelated keys is a prefix
 * that deletes the wrong things.
 */
static bool key_under(const char *key, const char *prefix) {
    if (prefix == NULL || *prefix == '\0') {
        return true;
    }
    size_t length = strlen(prefix);
    if (strncmp(key, prefix, length) != 0) {
        return false;
    }
    return key[length] == '\0' || key[length] == '/';
}

static struct entry *find(struct hive *hive, const char *key) {
    for (int i = 0; i < ENTRIES_MAX; i++) {
        if (hive->entries[i].used && strcmp(hive->entries[i].key, key) == 0) {
            return &hive->entries[i];
        }
    }
    return NULL;
}

/* --- On disk --- */

/*
 * Values are escaped so a newline in one cannot be read back as another line.
 * Only three characters need it, and leaving the rest alone keeps the file
 * readable, which matters for something a person may want to repair by hand.
 */
static void escape(const char *value, char *out, size_t size) {
    size_t used = 0;
    for (const char *c = value; *c != '\0' && used + 2 < size; c++) {
        if (*c == '\\') {
            out[used++] = '\\';
            out[used++] = '\\';
        } else if (*c == '\n') {
            out[used++] = '\\';
            out[used++] = 'n';
        } else if (*c == '\r') {
            out[used++] = '\\';
            out[used++] = 'r';
        } else {
            out[used++] = *c;
        }
    }
    out[used] = '\0';
}

static void unescape(const char *text, char *out, size_t size) {
    size_t used = 0;
    for (const char *c = text; *c != '\0' && used + 1 < size; c++) {
        if (*c != '\\') {
            out[used++] = *c;
            continue;
        }

        c++;
        if (*c == '\0') {
            break; /* A trailing backslash escapes nothing. */
        }
        if (*c == 'n') {
            out[used++] = '\n';
        } else if (*c == 'r') {
            out[used++] = '\r';
        } else {
            out[used++] = *c;
        }
    }
    out[used] = '\0';
}

static int compare_entries(const void *a, const void *b) {
    const struct entry *ea = a, *eb = b;
    if (ea->used != eb->used) {
        return ea->used ? -1 : 1;  /* used ones first */
    }
    if (!ea->used) {
        return 0;
    }
    return strcmp(ea->key, eb->key);
}

/* Sorted, so the file reads in a sensible order and a diff of it is useful. */
static void sort_hive(struct hive *hive) {
    qsort(hive->entries, ENTRIES_MAX, sizeof(hive->entries[0]), compare_entries);
}

static bool save(enum recon_registry_scope scope) {
    struct hive *hive = hive_for(scope);
    sort_hive(hive);

    /* Big enough for every entry at full length, plus the file's header. */
    size_t capacity = (size_t)ENTRIES_MAX *
        (RECON_REGISTRY_KEY_MAX + RECON_REGISTRY_VALUE_MAX * 2 + 8) + 256;
    char *text = malloc(capacity);
    if (text == NULL) {
        set_error("out of memory");
        return false;
    }

    size_t used = (size_t)snprintf(text, capacity,
        "# ReconOS registry -- %s settings.\n"
        "# key = value, one per line. Edited by hand at your own risk;\n"
        "# a line that will not parse is skipped, not repaired.\n\n",
        scope == RECON_REG_SYSTEM ? "system" : "user");

    for (int i = 0; i < ENTRIES_MAX && used < capacity; i++) {
        if (!hive->entries[i].used) {
            continue;
        }

        char escaped[RECON_REGISTRY_VALUE_MAX * 2 + 4];
        escape(hive->entries[i].value, escaped, sizeof(escaped));

        int written = snprintf(text + used, capacity - used, "%s = %s\n",
            hive->entries[i].key, escaped);
        if (written < 0) {
            break;
        }
        used += (size_t)written;
    }

    char path[RECON_PATH_MAX];
    hive_path(scope, path, sizeof(path));

    bool ok = recon_fs_write("/", path, text, used);
    if (!ok) {
        set_error("%s", recon_fs_last_error());
    }
    free(text);
    return ok;
}

static void load(enum recon_registry_scope scope) {
    struct hive *hive = hive_for(scope);
    memset(hive, 0, sizeof(*hive));
    hive->loaded = true;

    char path[RECON_PATH_MAX];
    hive_path(scope, path, sizeof(path));

    size_t size = 0;
    char *text = recon_fs_read("/", path, &size);

    /*
     * The user's hive used to be called "user.reg", in plain sight in the
     * middle of their home folder. It is hidden now. An installation that
     * predates the change still has the old file, and losing somebody's skin
     * and spacing to a rename would be a poor trade for tidiness, so it is
     * read once and written back under the new name.
     */
    bool migrated = false;
    if (text == NULL && scope == RECON_REG_USER) {
        char legacy[RECON_PATH_MAX];
        snprintf(legacy, sizeof(legacy), "%s/%s", recon_fs_user_dir(NULL),
            RECON_REGISTRY_USER_FILE_LEGACY);
        text = recon_fs_read("/", legacy, &size);
        migrated = (text != NULL);
    }

    if (text == NULL) {
        /* Never saved anything. A normal first run, not a problem. */
        return;
    }

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
            continue; /* Not a setting. Skipped rather than guessed at. */
        }
        *equals = '\0';

        /* Trim around the separator, so "a = b" and "a=b" are the same. */
        char *key_end = equals;
        while (key_end > line && (key_end[-1] == ' ' || key_end[-1] == '\t')) {
            *--key_end = '\0';
        }
        char *value = equals + 1;
        while (*value == ' ' || *value == '\t') {
            value++;
        }

        if (!key_is_valid(line) || hive->count >= ENTRIES_MAX) {
            continue;
        }

        struct entry *slot = NULL;
        for (int i = 0; i < ENTRIES_MAX; i++) {
            if (!hive->entries[i].used) {
                slot = &hive->entries[i];
                break;
            }
        }
        if (slot == NULL) {
            break;
        }

        slot->used = true;
        snprintf(slot->key, sizeof(slot->key), "%s", line);
        unescape(value, slot->value, sizeof(slot->value));
        hive->count++;
    }

    free(text);

    if (migrated) {
        /* Written under the new name, then the old one taken away. Leaving it
         * would mean two files claiming to be the settings, and the stale one
         * winning the next time somebody looked. */
        char legacy[RECON_PATH_MAX];
        snprintf(legacy, sizeof(legacy), "%s/%s", recon_fs_user_dir(NULL),
            RECON_REGISTRY_USER_FILE_LEGACY);
        if (save(scope)) {
            recon_fs_remove("/", legacy);
        }
    }
}

bool recon_registry_init(void) {
    load(RECON_REG_SYSTEM);
    load(RECON_REG_USER);
    return true;
}

void recon_registry_reload_user(void) {
    /* load() clears the hive first, so this drops the previous account's
     * settings rather than merging one person's over another's. */
    load(RECON_REG_USER);
}

void recon_registry_finish(void) {
    memset(&g_system, 0, sizeof(g_system));
    memset(&g_user, 0, sizeof(g_user));
}

/* --- Reading --- */

const char *recon_registry_get(enum recon_registry_scope scope,
        const char *key, const char *fallback) {
    if (!key_is_valid(key)) {
        return fallback;
    }
    struct entry *found = find(hive_for(scope), key);
    return found != NULL ? found->value : fallback;
}

int recon_registry_get_int(enum recon_registry_scope scope,
        const char *key, int fallback) {
    const char *text = recon_registry_get(scope, key, NULL);
    if (text == NULL || *text == '\0') {
        return fallback;
    }

    /*
     * A value that will not parse gives the fallback, not zero. A corrupted
     * number should look like a missing setting rather than like a real one
     * that happens to be 0 -- otherwise a damaged file silently becomes a
     * window at the top-left corner with no icons.
     */
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (end == text || (end != NULL && *end != '\0')) {
        return fallback;
    }
    return (int)value;
}

bool recon_registry_get_bool(enum recon_registry_scope scope,
        const char *key, bool fallback) {
    const char *text = recon_registry_get(scope, key, NULL);
    if (text == NULL) {
        return fallback;
    }

    if (strcmp(text, "true") == 0 || strcmp(text, "yes") == 0 ||
            strcmp(text, "1") == 0) {
        return true;
    }
    if (strcmp(text, "false") == 0 || strcmp(text, "no") == 0 ||
            strcmp(text, "0") == 0) {
        return false;
    }
    return fallback;
}

bool recon_registry_has(enum recon_registry_scope scope, const char *key) {
    return key_is_valid(key) && find(hive_for(scope), key) != NULL;
}

/* --- Writing --- */

bool recon_registry_set(enum recon_registry_scope scope,
        const char *key, const char *value) {
    if (!key_is_valid(key)) {
        set_error("'%s' is not a usable key", key != NULL ? key : "");
        return false;
    }
    if (value == NULL) {
        value = "";
    }
    if (strlen(value) >= RECON_REGISTRY_VALUE_MAX) {
        set_error("the value for '%s' is too long", key);
        return false;
    }

    struct hive *hive = hive_for(scope);
    struct entry *found = find(hive, key);

    /* Writing what is already there costs nothing and touches no disk, so
     * saving a window's position whenever it stops moving is free when it has
     * not moved. */
    if (found != NULL && strcmp(found->value, value) == 0) {
        return true;
    }

    if (found == NULL) {
        for (int i = 0; i < ENTRIES_MAX && found == NULL; i++) {
            if (!hive->entries[i].used) {
                found = &hive->entries[i];
            }
        }
        if (found == NULL) {
            set_error("the registry is full");
            return false;
        }
        found->used = true;
        snprintf(found->key, sizeof(found->key), "%s", key);
        hive->count++;
    }

    snprintf(found->value, sizeof(found->value), "%s", value);
    return save(scope);
}

bool recon_registry_set_int(enum recon_registry_scope scope,
        const char *key, int value) {
    char text[32];
    snprintf(text, sizeof(text), "%d", value);
    return recon_registry_set(scope, key, text);
}

bool recon_registry_set_bool(enum recon_registry_scope scope,
        const char *key, bool value) {
    return recon_registry_set(scope, key, value ? "true" : "false");
}

bool recon_registry_remove(enum recon_registry_scope scope, const char *key) {
    if (!key_is_valid(key)) {
        return false;
    }

    struct hive *hive = hive_for(scope);
    struct entry *found = find(hive, key);
    if (found == NULL) {
        set_error("no setting called '%s'", key);
        return false;
    }

    memset(found, 0, sizeof(*found));
    hive->count--;
    return save(scope);
}

int recon_registry_remove_all(enum recon_registry_scope scope,
        const char *prefix) {
    struct hive *hive = hive_for(scope);
    int removed = 0;

    for (int i = 0; i < ENTRIES_MAX; i++) {
        if (hive->entries[i].used && key_under(hive->entries[i].key, prefix)) {
            memset(&hive->entries[i], 0, sizeof(hive->entries[i]));
            hive->count--;
            removed++;
        }
    }

    if (removed > 0) {
        save(scope);
    }
    return removed;
}

/* --- Listing --- */

bool recon_registry_at(enum recon_registry_scope scope, const char *prefix,
        int index, const char **key_out, const char **value_out) {
    struct hive *hive = hive_for(scope);
    sort_hive(hive);

    int seen = 0;
    for (int i = 0; i < ENTRIES_MAX; i++) {
        if (!hive->entries[i].used ||
                !key_under(hive->entries[i].key, prefix)) {
            continue;
        }
        if (seen == index) {
            if (key_out != NULL) {
                *key_out = hive->entries[i].key;
            }
            if (value_out != NULL) {
                *value_out = hive->entries[i].value;
            }
            return true;
        }
        seen++;
    }
    return false;
}

int recon_registry_count(enum recon_registry_scope scope, const char *prefix) {
    struct hive *hive = hive_for(scope);
    int count = 0;

    for (int i = 0; i < ENTRIES_MAX; i++) {
        if (hive->entries[i].used && key_under(hive->entries[i].key, prefix)) {
            count++;
        }
    }
    return count;
}
