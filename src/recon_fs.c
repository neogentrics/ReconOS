/*
 * The ReconOS filesystem. See include/recon_fs.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */
#include <sys/stat.h>
#include <unistd.h>

#include "recon_fs.h"

#define DEFAULT_HOST_ROOT "/recon"

static char g_host_root[RECON_PATH_MAX];
static char g_error[256];
static char g_user[RECON_NAME_MAX] = RECON_USER_ADMIN;
static char g_user_path[RECON_PATH_MAX];

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, args);
    va_end(args);
}

const char *recon_fs_last_error(void) {
    return g_error[0] != '\0' ? g_error : "no error";
}

const char *recon_fs_host_root(void) {
    return g_host_root;
}

/* --- Path handling --- */

/*
 * Normalize a ReconOS path into a canonical absolute one.
 *
 * Segments are resolved as they are read, so ".." is applied to what came
 * before it rather than left in the string. A ".." that would climb past the
 * root fails: refusing is safer than silently clamping, which would let a path
 * mean something other than it says.
 */
static bool normalize(const char *cwd, const char *path, char *out, size_t size) {
    char work[RECON_PATH_MAX * 2];

    if (path == NULL || *path == '\0') {
        path = ".";
    }

    if (path[0] == '/') {
        snprintf(work, sizeof(work), "%s", path);
    } else {
        const char *base = (cwd != NULL && *cwd != '\0') ? cwd : "/";
        if (strcmp(base, "/") == 0) {
            snprintf(work, sizeof(work), "/%s", path);
        } else {
            snprintf(work, sizeof(work), "%s/%s", base, path);
        }
    }

    /* Walk the segments, building the result as a stack. */
    char *segments[64];
    int depth = 0;

    char *saveptr = NULL;
    for (char *token = strtok_r(work, "/", &saveptr);
            token != NULL;
            token = strtok_r(NULL, "/", &saveptr)) {

        if (strcmp(token, ".") == 0) {
            continue;
        }
        if (strcmp(token, "..") == 0) {
            if (depth == 0) {
                set_error("path leaves the ReconOS root");
                return false;
            }
            depth--;
            continue;
        }
        if (depth >= (int)(sizeof(segments) / sizeof(segments[0]))) {
            set_error("path is nested too deeply");
            return false;
        }
        segments[depth++] = token;
    }

    if (depth == 0) {
        snprintf(out, size, "/");
        return true;
    }

    size_t used = 0;
    out[0] = '\0';
    for (int i = 0; i < depth; i++) {
        int written = snprintf(out + used, size - used, "/%s", segments[i]);
        if (written < 0 || (size_t)written >= size - used) {
            set_error("path is too long");
            return false;
        }
        used += (size_t)written;
    }
    return true;
}

bool recon_fs_resolve(const char *cwd, const char *path,
        char *host_out, size_t host_size, char *canonical_out, size_t canonical_size) {
    char canonical[RECON_PATH_MAX];
    if (!normalize(cwd, path, canonical, sizeof(canonical))) {
        return false;
    }

    int written;
    if (strcmp(canonical, "/") == 0) {
        written = snprintf(host_out, host_size, "%s", g_host_root);
    } else {
        written = snprintf(host_out, host_size, "%s%s", g_host_root, canonical);
    }
    if (written < 0 || (size_t)written >= host_size) {
        set_error("path is too long");
        return false;
    }

    if (canonical_out != NULL) {
        snprintf(canonical_out, canonical_size, "%s", canonical);
    }
    return true;
}

/* --- Setup --- */

/* Defined below; recon_fs_init creates the first account. */
bool recon_fs_create_user(const char *name);

/* Create a directory and any parents, ignoring those that already exist. */
static bool make_tree(const char *host_path) {
    char work[RECON_PATH_MAX];
    snprintf(work, sizeof(work), "%s", host_path);

    for (char *p = work + 1; *p != '\0'; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(work, 0755) != 0 && errno != EEXIST) {
            set_error("cannot create '%s': %s", work, strerror(errno));
            return false;
        }
        *p = '/';
    }

    if (mkdir(work, 0755) != 0 && errno != EEXIST) {
        set_error("cannot create '%s': %s", work, strerror(errno));
        return false;
    }
    return true;
}

/* Trim a trailing slash, so joining paths does not double it. */
static void set_root(const char *path) {
    snprintf(g_host_root, sizeof(g_host_root), "%s", path);
    size_t len = strlen(g_host_root);
    while (len > 1 && g_host_root[len - 1] == '/') {
        g_host_root[--len] = '\0';
    }
}

bool recon_fs_init(const char *host_root) {
    const char *requested = host_root;
    if (requested == NULL || *requested == '\0') {
        requested = getenv("RECONOS_ROOT");
    }

    if (requested != NULL && *requested != '\0') {
        /* An explicit location is not second-guessed: if it cannot be used,
         * something is wrong that a fallback would only hide. */
        set_root(requested);
        if (!make_tree(g_host_root)) {
            return false;
        }
    } else {
        /*
         * Otherwise prefer /recon, which is where ReconOS belongs on a machine
         * it owns, and fall back to somewhere writable when it cannot be used.
         * Running as an ordinary user is normal during development, and
         * refusing to start over a directory permission would be a poor way to
         * report a problem with an obvious answer.
         *
         * Existing is not the test: /recon may be there from a run as root and
         * not be writable now. Writability is what matters.
         */
        set_root(DEFAULT_HOST_ROOT);
        if (!make_tree(g_host_root) || access(g_host_root, W_OK) != 0) {
            const char *home = getenv("HOME");
            if (home == NULL || *home == '\0') {
                home = "/tmp";
            }
            char fallback[RECON_PATH_MAX];
            snprintf(fallback, sizeof(fallback), "%s/.reconos", home);
            set_root(fallback);

            if (!make_tree(g_host_root)) {
                return false;
            }
        }
    }

    /* The standard layout, created on first run so the system always has the
     * shape it expects rather than discovering pieces missing later. */
    static const char *const LAYOUT[] = {
        RECON_DIR_SYSTEM,
        RECON_DIR_SYSTEM_APPS,
        RECON_DIR_SYSTEM_CONFIG,
        RECON_DIR_SYSTEM_THEMES,
        RECON_DIR_SYSTEM_ICONS,
        RECON_DIR_APPS,
        RECON_DIR_USERS,
        RECON_DIR_TEMP,
        NULL,
    };

    for (int i = 0; LAYOUT[i] != NULL; i++) {
        char host[RECON_PATH_MAX];
        snprintf(host, sizeof(host), "%s%s", g_host_root, LAYOUT[i]);
        if (!make_tree(host)) {
            return false;
        }
    }

    /* The administrator exists from the first run: something has to own the
     * desktop, and there is no way to log in as anybody else yet. */
    if (!recon_fs_create_user(RECON_USER_ADMIN)) {
        return false;
    }

    return true;
}

const char *recon_fs_current_user(void) {
    return g_user;
}

const char *recon_fs_user_dir(const char *subdirectory) {
    if (subdirectory == NULL || *subdirectory == '\0') {
        snprintf(g_user_path, sizeof(g_user_path), "%s/%s", RECON_DIR_USERS, g_user);
    } else {
        snprintf(g_user_path, sizeof(g_user_path), "%s/%s/%s",
            RECON_DIR_USERS, g_user, subdirectory);
    }
    return g_user_path;
}

bool recon_fs_create_user(const char *name) {
    if (name == NULL || *name == '\0' || strchr(name, '/') != NULL) {
        set_error("invalid user name");
        return false;
    }

    /* Room for the root, the users directory and the longest folder name. */
    char host[RECON_PATH_MAX * 2];
    snprintf(host, sizeof(host), "%s%s/%s", g_host_root, RECON_DIR_USERS, name);
    if (!make_tree(host)) {
        return false;
    }

    static const char *const FOLDERS[] = RECON_USER_FOLDERS;
    for (size_t i = 0; i < sizeof(FOLDERS) / sizeof(FOLDERS[0]); i++) {
        char child[RECON_PATH_MAX * 3];
        snprintf(child, sizeof(child), "%s/%s", host, FOLDERS[i]);
        if (!make_tree(child)) {
            return false;
        }
    }
    return true;
}

void recon_fs_finish(void) {
    g_host_root[0] = '\0';
}

/* --- Queries --- */

static enum recon_file_kind kind_of(const struct stat *st) {
    if (S_ISDIR(st->st_mode)) {
        return RECON_FILE_DIRECTORY;
    }
    if (S_ISREG(st->st_mode)) {
        return RECON_FILE_REGULAR;
    }
    return RECON_FILE_OTHER;
}

bool recon_fs_stat(const char *cwd, const char *path, struct recon_dirent *out) {
    char host[RECON_PATH_MAX];
    char canonical[RECON_PATH_MAX];
    if (!recon_fs_resolve(cwd, path, host, sizeof(host), canonical, sizeof(canonical))) {
        return false;
    }

    struct stat st;
    if (stat(host, &st) != 0) {
        set_error("'%s' not found", canonical);
        return false;
    }

    if (out != NULL) {
        const char *name = strrchr(canonical, '/');
        snprintf(out->name, sizeof(out->name), "%s",
            (name != NULL && name[1] != '\0') ? name + 1 : "/");
        out->kind = kind_of(&st);
        out->size = (size_t)st.st_size;
    }
    return true;
}

bool recon_fs_exists(const char *cwd, const char *path) {
    return recon_fs_stat(cwd, path, NULL);
}

static int compare_dirents(const void *a, const void *b) {
    const struct recon_dirent *da = a, *db = b;
    /* Directories first, then by name: a listing is easier to read when the
     * places you can go are grouped together. */
    if (da->kind != db->kind) {
        if (da->kind == RECON_FILE_DIRECTORY) return -1;
        if (db->kind == RECON_FILE_DIRECTORY) return 1;
    }
    return strcasecmp(da->name, db->name);
}

int recon_fs_list(const char *cwd, const char *path,
        struct recon_dirent *out, int max) {
    char host[RECON_PATH_MAX];
    char canonical[RECON_PATH_MAX];
    if (!recon_fs_resolve(cwd, path, host, sizeof(host), canonical, sizeof(canonical))) {
        return -1;
    }

    DIR *dir = opendir(host);
    if (dir == NULL) {
        set_error("cannot open '%s': %s", canonical, strerror(errno));
        return -1;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (count < max) {
            char child[RECON_PATH_MAX * 3];
            snprintf(child, sizeof(child), "%s/%s", host, entry->d_name);

            struct stat st;
            snprintf(out[count].name, sizeof(out[count].name), "%s", entry->d_name);
            if (stat(child, &st) == 0) {
                out[count].kind = kind_of(&st);
                out[count].size = (size_t)st.st_size;
            } else {
                out[count].kind = RECON_FILE_OTHER;
                out[count].size = 0;
            }
        }
        count++;
    }
    closedir(dir);

    int sorted = count < max ? count : max;
    qsort(out, (size_t)sorted, sizeof(*out), compare_dirents);
    return count;
}

/* --- Contents --- */

char *recon_fs_read(const char *cwd, const char *path, size_t *size_out) {
    char host[RECON_PATH_MAX];
    char canonical[RECON_PATH_MAX];
    if (!recon_fs_resolve(cwd, path, host, sizeof(host), canonical, sizeof(canonical))) {
        return NULL;
    }

    FILE *f = fopen(host, "rb");
    if (f == NULL) {
        set_error("cannot read '%s': %s", canonical, strerror(errno));
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    if (size < 0) {
        fclose(f);
        set_error("cannot read '%s'", canonical);
        return NULL;
    }

    char *data = malloc((size_t)size + 1);
    if (data == NULL) {
        fclose(f);
        set_error("out of memory");
        return NULL;
    }
    size_t read = fread(data, 1, (size_t)size, f);
    fclose(f);

    data[read] = '\0';
    if (size_out != NULL) {
        *size_out = read;
    }
    return data;
}

static bool write_file(const char *cwd, const char *path, const char *data,
        size_t size, const char *mode) {
    char host[RECON_PATH_MAX];
    char canonical[RECON_PATH_MAX];
    if (!recon_fs_resolve(cwd, path, host, sizeof(host), canonical, sizeof(canonical))) {
        return false;
    }

    FILE *f = fopen(host, mode);
    if (f == NULL) {
        set_error("cannot write '%s': %s", canonical, strerror(errno));
        return false;
    }

    size_t written = size > 0 ? fwrite(data, 1, size, f) : 0;
    fclose(f);

    if (written != size) {
        set_error("only wrote %zu of %zu bytes to '%s'", written, size, canonical);
        return false;
    }
    return true;
}

bool recon_fs_write(const char *cwd, const char *path, const char *data, size_t size) {
    return write_file(cwd, path, data, size, "wb");
}

bool recon_fs_append(const char *cwd, const char *path, const char *data, size_t size) {
    return write_file(cwd, path, data, size, "ab");
}

bool recon_fs_mkdir(const char *cwd, const char *path) {
    char host[RECON_PATH_MAX];
    char canonical[RECON_PATH_MAX];
    if (!recon_fs_resolve(cwd, path, host, sizeof(host), canonical, sizeof(canonical))) {
        return false;
    }
    if (mkdir(host, 0755) != 0) {
        set_error("cannot create '%s': %s", canonical, strerror(errno));
        return false;
    }
    return true;
}

bool recon_fs_remove(const char *cwd, const char *path) {
    char host[RECON_PATH_MAX];
    char canonical[RECON_PATH_MAX];
    if (!recon_fs_resolve(cwd, path, host, sizeof(host), canonical, sizeof(canonical))) {
        return false;
    }

    /* The system's own files are not something a stray command should be able
     * to delete. */
    if (strncmp(canonical, RECON_DIR_SYSTEM, strlen(RECON_DIR_SYSTEM)) == 0) {
        set_error("'%s' is part of the system and is protected", canonical);
        return false;
    }

    struct stat st;
    if (stat(host, &st) != 0) {
        set_error("'%s' not found", canonical);
        return false;
    }

    int result = S_ISDIR(st.st_mode) ? rmdir(host) : unlink(host);
    if (result != 0) {
        set_error("cannot remove '%s': %s", canonical, strerror(errno));
        return false;
    }
    return true;
}
