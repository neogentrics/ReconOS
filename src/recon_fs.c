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
        RECON_DIR_SYSTEM_MODULES,
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

    /*
     * An earlier layout put Desktop and Documents directly under /Users rather
     * than under each user. Those are gone from the layout above, but a system
     * created before the change still has them sitting next to the accounts,
     * where they look like users who do not exist.
     *
     * They are only removed when empty. rmdir fails on a directory with
     * anything in it, which is exactly the guarantee wanted here: if the user
     * has put something in one of them, it stays, and they can deal with it
     * themselves.
     */
    static const char *const RETIRED[] = { "Desktop", "Documents", NULL };
    for (int i = 0; RETIRED[i] != NULL; i++) {
        char host[RECON_PATH_MAX * 2];
        snprintf(host, sizeof(host), "%s%s/%s", g_host_root, RECON_DIR_USERS, RETIRED[i]);
        if (rmdir(host) == 0) {
            /* Not an error either way; nothing depends on the outcome. */
        }
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

    /* The recycle bin is part of an account, not something created the first
     * time a file is deleted: a bin that only exists once used cannot be
     * opened to find out it is empty. */
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

    /* Counting is a legitimate use: callers ask with max 0 and no buffer to
     * find out whether a directory has anything in it. */
    int sorted = count < max ? count : max;
    if (out != NULL && sorted > 0) {
        qsort(out, (size_t)sorted, sizeof(*out), compare_dirents);
    }
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

/*
 * Whether a canonical path is inside /System.
 *
 * Compared segment by segment rather than as a prefix: "/Systems" starts with
 * "/System" without being under it, and a protection that catches unrelated
 * names is a protection nobody trusts.
 */
static bool path_is_protected(const char *canonical) {
    size_t len = strlen(RECON_DIR_SYSTEM);
    if (strncmp(canonical, RECON_DIR_SYSTEM, len) != 0) {
        return false;
    }
    return canonical[len] == '\0' || canonical[len] == '/';
}

bool recon_fs_is_protected(const char *cwd, const char *path) {
    char canonical[RECON_PATH_MAX];
    char host[RECON_PATH_MAX];
    if (!recon_fs_resolve(cwd, path, host, sizeof(host), canonical, sizeof(canonical))) {
        return true; /* Cannot resolve it, so do not act on it. */
    }
    return path_is_protected(canonical);
}

bool recon_fs_remove(const char *cwd, const char *path) {
    char host[RECON_PATH_MAX];
    char canonical[RECON_PATH_MAX];
    if (!recon_fs_resolve(cwd, path, host, sizeof(host), canonical, sizeof(canonical))) {
        return false;
    }

    /* The system's own files are not something a stray command should be able
     * to delete. */
    if (path_is_protected(canonical)) {
        set_error("'%s' is part of the system and is protected", canonical);
        return false;
    }

    struct stat st;
    if (stat(host, &st) != 0) {
        set_error("'%s' not found", canonical);
        return false;
    }

    if (S_ISDIR(st.st_mode)) {
        if (rmdir(host) != 0) {
            /*
             * Say which problem it is. "Directory not empty" is a different
             * situation from a permission error, and the caller can offer to
             * delete the contents only if it knows that is what stopped it.
             */
            if (errno == ENOTEMPTY || errno == EEXIST) {
                set_error("'%s' is not empty", canonical);
            } else {
                set_error("cannot remove '%s': %s", canonical, strerror(errno));
            }
            return false;
        }
        return true;
    }

    if (unlink(host) != 0) {
        set_error("cannot remove '%s': %s", canonical, strerror(errno));
        return false;
    }
    return true;
}

/* --- Removing, renaming and copying --- */

/* Delete a host directory and its contents. Depth-first, so a directory is
 * only removed once it is empty. */
static bool remove_tree_host(const char *host_path) {
    struct stat st;
    if (lstat(host_path, &st) != 0) {
        set_error("cannot read '%s': %s", host_path, strerror(errno));
        return false;
    }

    if (!S_ISDIR(st.st_mode)) {
        if (unlink(host_path) != 0) {
            set_error("cannot remove '%s': %s", host_path, strerror(errno));
            return false;
        }
        return true;
    }

    DIR *dir = opendir(host_path);
    if (dir == NULL) {
        set_error("cannot open '%s': %s", host_path, strerror(errno));
        return false;
    }

    bool ok = true;
    struct dirent *entry;
    while (ok && (entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child[RECON_PATH_MAX * 2];
        snprintf(child, sizeof(child), "%s/%s", host_path, entry->d_name);
        ok = remove_tree_host(child);
    }
    closedir(dir);

    if (!ok) {
        return false;
    }
    if (rmdir(host_path) != 0) {
        set_error("cannot remove '%s': %s", host_path, strerror(errno));
        return false;
    }
    return true;
}

bool recon_fs_remove_tree(const char *cwd, const char *path) {
    char host[RECON_PATH_MAX];
    char canonical[RECON_PATH_MAX];
    if (!recon_fs_resolve(cwd, path, host, sizeof(host), canonical, sizeof(canonical))) {
        return false;
    }

    if (path_is_protected(canonical)) {
        set_error("'%s' is part of the system and is protected", canonical);
        return false;
    }
    if (strcmp(canonical, "/") == 0) {
        set_error("the root cannot be removed");
        return false;
    }

    struct stat st;
    if (stat(host, &st) != 0) {
        set_error("'%s' not found", canonical);
        return false;
    }
    return remove_tree_host(host);
}

/* True if `inner` is the same path as `outer`, or sits inside it. */
static bool path_within(const char *outer, const char *inner) {
    size_t len = strlen(outer);
    if (strcmp(outer, "/") == 0) {
        return true;
    }
    if (strncmp(inner, outer, len) != 0) {
        return false;
    }
    return inner[len] == '\0' || inner[len] == '/';
}

bool recon_fs_rename(const char *cwd, const char *from, const char *to) {
    char from_host[RECON_PATH_MAX], from_canonical[RECON_PATH_MAX];
    char to_host[RECON_PATH_MAX], to_canonical[RECON_PATH_MAX];

    if (!recon_fs_resolve(cwd, from, from_host, sizeof(from_host),
            from_canonical, sizeof(from_canonical))) {
        return false;
    }
    if (!recon_fs_resolve(cwd, to, to_host, sizeof(to_host),
            to_canonical, sizeof(to_canonical))) {
        return false;
    }

    if (path_is_protected(from_canonical) || path_is_protected(to_canonical)) {
        set_error("system files are protected");
        return false;
    }
    if (strcmp(from_canonical, to_canonical) == 0) {
        return true; /* Renaming something to its own name is not a failure. */
    }
    /* Moving a directory into itself would leave it unreachable, so it is
     * refused rather than attempted. */
    if (path_within(from_canonical, to_canonical)) {
        set_error("cannot move '%s' into itself", from_canonical);
        return false;
    }
    if (access(to_host, F_OK) == 0) {
        set_error("'%s' already exists", to_canonical);
        return false;
    }

    if (rename(from_host, to_host) != 0) {
        set_error("cannot rename '%s': %s", from_canonical, strerror(errno));
        return false;
    }
    return true;
}

/* Copy one regular file, preserving its permission bits. */
static bool copy_file_host(const char *src, const char *dst, mode_t mode) {
    FILE *in = fopen(src, "rb");
    if (in == NULL) {
        set_error("cannot read '%s': %s", src, strerror(errno));
        return false;
    }
    FILE *out = fopen(dst, "wb");
    if (out == NULL) {
        fclose(in);
        set_error("cannot write '%s': %s", dst, strerror(errno));
        return false;
    }

    char buffer[16384];
    size_t got;
    bool ok = true;
    while ((got = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, got, out) != got) {
            set_error("cannot write '%s': %s", dst, strerror(errno));
            ok = false;
            break;
        }
    }

    fclose(in);
    if (fclose(out) != 0 && ok) {
        set_error("cannot write '%s': %s", dst, strerror(errno));
        ok = false;
    }

    if (!ok) {
        /* A half-written file is worse than none: it looks like a copy that
         * worked. */
        unlink(dst);
        return false;
    }

    chmod(dst, mode & 0777);
    return true;
}

static bool copy_tree_host(const char *src, const char *dst) {
    struct stat st;
    if (lstat(src, &st) != 0) {
        set_error("cannot read '%s': %s", src, strerror(errno));
        return false;
    }

    if (!S_ISDIR(st.st_mode)) {
        if (!S_ISREG(st.st_mode)) {
            set_error("'%s' is not something that can be copied", src);
            return false;
        }
        return copy_file_host(src, dst, st.st_mode);
    }

    if (mkdir(dst, st.st_mode & 0777) != 0 && errno != EEXIST) {
        set_error("cannot create '%s': %s", dst, strerror(errno));
        return false;
    }

    DIR *dir = opendir(src);
    if (dir == NULL) {
        set_error("cannot open '%s': %s", src, strerror(errno));
        return false;
    }

    bool ok = true;
    struct dirent *entry;
    while (ok && (entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child_src[RECON_PATH_MAX * 2];
        char child_dst[RECON_PATH_MAX * 2];
        snprintf(child_src, sizeof(child_src), "%s/%s", src, entry->d_name);
        snprintf(child_dst, sizeof(child_dst), "%s/%s", dst, entry->d_name);
        ok = copy_tree_host(child_src, child_dst);
    }
    closedir(dir);
    return ok;
}

bool recon_fs_copy(const char *cwd, const char *from, const char *to) {
    char from_host[RECON_PATH_MAX], from_canonical[RECON_PATH_MAX];
    char to_host[RECON_PATH_MAX], to_canonical[RECON_PATH_MAX];

    if (!recon_fs_resolve(cwd, from, from_host, sizeof(from_host),
            from_canonical, sizeof(from_canonical))) {
        return false;
    }
    if (!recon_fs_resolve(cwd, to, to_host, sizeof(to_host),
            to_canonical, sizeof(to_canonical))) {
        return false;
    }

    if (path_is_protected(to_canonical)) {
        set_error("'%s' is part of the system and is protected", to_canonical);
        return false;
    }
    /* Copying a directory into itself recurses until the disk is full. */
    if (path_within(from_canonical, to_canonical)) {
        set_error("cannot copy '%s' into itself", from_canonical);
        return false;
    }
    if (access(to_host, F_OK) == 0) {
        set_error("'%s' already exists", to_canonical);
        return false;
    }

    return copy_tree_host(from_host, to_host);
}

bool recon_fs_unique_name(const char *cwd, const char *directory,
        const char *base, const char *extension, char *out, size_t size) {
    if (extension == NULL) {
        extension = "";
    }

    for (int attempt = 1; attempt < 1000; attempt++) {
        char candidate[RECON_NAME_MAX];
        if (attempt == 1) {
            snprintf(candidate, sizeof(candidate), "%s%s", base, extension);
        } else {
            snprintf(candidate, sizeof(candidate), "%s %d%s", base, attempt, extension);
        }

        char full[RECON_PATH_MAX];
        if (strcmp(directory, "/") == 0) {
            snprintf(full, sizeof(full), "/%s", candidate);
        } else {
            snprintf(full, sizeof(full), "%s/%s", directory, candidate);
        }

        if (!recon_fs_exists(cwd, full)) {
            snprintf(out, size, "%s", candidate);
            return true;
        }
    }

    set_error("no free name for '%s'", base);
    return false;
}

/* --- The recycle bin --- */

/*
 * Layout, inside the user's folder:
 *   .Trash/files/<name>       what was deleted, under a name unique in here
 *   .Trash/info/<name>.origin one line: where it came from
 *
 * Two directories rather than one so a note can never be mistaken for a
 * deleted file, and so listing the bin is just listing files/.
 */
#define TRASH_ROOT ".Trash"
#define TRASH_FILES ".Trash/files"
#define TRASH_INFO ".Trash/info"

static char g_trash_path[RECON_PATH_MAX];

static const char *trash_subdir(const char *which) {
    snprintf(g_trash_path, sizeof(g_trash_path), "%s/%s",
        recon_fs_user_dir(NULL), which);
    return g_trash_path;
}

const char *recon_fs_trash_dir(void) {
    return trash_subdir(TRASH_FILES);
}

/* Make sure the bin exists. Called before anything that writes to it, since
 * a user created before the bin existed has no .Trash. */
static bool ensure_trash(void) {
    char host[RECON_PATH_MAX * 2];

    const char *dirs[] = { TRASH_ROOT, TRASH_FILES, TRASH_INFO };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char reconos[RECON_PATH_MAX];
        snprintf(reconos, sizeof(reconos), "%s/%s", recon_fs_user_dir(NULL), dirs[i]);
        snprintf(host, sizeof(host), "%s%s", g_host_root, reconos);
        if (!make_tree(host)) {
            return false;
        }
    }
    return true;
}

bool recon_fs_is_trash(const char *cwd, const char *path) {
    char canonical[RECON_PATH_MAX];
    char host[RECON_PATH_MAX];
    if (!recon_fs_resolve(cwd, path, host, sizeof(host), canonical, sizeof(canonical))) {
        return false;
    }

    char root[RECON_PATH_MAX];
    snprintf(root, sizeof(root), "%s/%s", recon_fs_user_dir(NULL), TRASH_ROOT);

    size_t len = strlen(root);
    if (strncmp(canonical, root, len) != 0) {
        return false;
    }
    return canonical[len] == '\0' || canonical[len] == '/';
}

bool recon_fs_trash(const char *cwd, const char *path) {
    char canonical[RECON_PATH_MAX];
    char host[RECON_PATH_MAX];
    if (!recon_fs_resolve(cwd, path, host, sizeof(host), canonical, sizeof(canonical))) {
        return false;
    }

    if (path_is_protected(canonical)) {
        set_error("'%s' is part of the system and is protected", canonical);
        return false;
    }
    if (recon_fs_is_trash(cwd, path)) {
        set_error("'%s' is already in the recycle bin", canonical);
        return false;
    }
    if (strcmp(canonical, "/") == 0) {
        set_error("the root cannot be deleted");
        return false;
    }
    if (!recon_fs_exists(cwd, canonical)) {
        set_error("'%s' not found", canonical);
        return false;
    }
    if (!ensure_trash()) {
        return false;
    }

    const char *leaf = strrchr(canonical, '/');
    leaf = (leaf != NULL && leaf[1] != '\0') ? leaf + 1 : canonical;

    /* Two files with the same name can be deleted from different folders, so
     * the name in the bin is made unique rather than assumed to be. */
    char base[RECON_NAME_MAX];
    char extension[RECON_NAME_MAX] = "";
    snprintf(base, sizeof(base), "%s", leaf);
    char *dot = strrchr(base, '.');
    if (dot != NULL && dot != base) {
        snprintf(extension, sizeof(extension), "%s", dot);
        *dot = '\0';
    }

    char files_dir[RECON_PATH_MAX];
    snprintf(files_dir, sizeof(files_dir), "%s", trash_subdir(TRASH_FILES));

    char name[RECON_NAME_MAX];
    if (!recon_fs_unique_name("/", files_dir, base, extension, name, sizeof(name))) {
        return false;
    }

    char target[RECON_PATH_MAX];
    snprintf(target, sizeof(target), "%s/%s", files_dir, name);

    if (!recon_fs_rename("/", canonical, target)) {
        return false;
    }

    /*
     * Write the note after the move, not before: a note without a file is a
     * restore that fails confusingly, while a file without a note is one that
     * can still be purged and can be put somewhere sensible by hand.
     */
    char info[RECON_PATH_MAX];
    snprintf(info, sizeof(info), "%s/%s.origin", trash_subdir(TRASH_INFO), name);

    char body[RECON_PATH_MAX + 2];
    int length = snprintf(body, sizeof(body), "%s\n", canonical);
    recon_fs_write("/", info, body, (size_t)length);

    return true;
}

bool recon_fs_trash_origin(const char *name, char *out, size_t size) {
    if (name == NULL || out == NULL) {
        return false;
    }

    char info[RECON_PATH_MAX];
    snprintf(info, sizeof(info), "%s/%s.origin", trash_subdir(TRASH_INFO), name);

    size_t length = 0;
    char *data = recon_fs_read("/", info, &length);
    if (data == NULL) {
        return false;
    }

    char *newline = strchr(data, '\n');
    if (newline != NULL) {
        *newline = '\0';
    }
    snprintf(out, size, "%s", data);
    free(data);
    return out[0] != '\0';
}

bool recon_fs_trash_restore(const char *name) {
    if (name == NULL || *name == '\0' || strchr(name, '/') != NULL) {
        set_error("invalid name");
        return false;
    }

    char origin[RECON_PATH_MAX];
    if (!recon_fs_trash_origin(name, origin, sizeof(origin))) {
        set_error("nothing recorded about where '%s' came from", name);
        return false;
    }

    char source[RECON_PATH_MAX];
    snprintf(source, sizeof(source), "%s/%s", trash_subdir(TRASH_FILES), name);

    if (recon_fs_exists("/", origin)) {
        set_error("something is already at '%s'", origin);
        return false;
    }

    /*
     * The folder it came from may itself have been deleted since. Recreate the
     * path rather than refusing: the point of restoring is to get the file
     * back, and putting it in a folder that has to be remade is still that.
     */
    char parent[RECON_PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", origin);
    char *slash = strrchr(parent, '/');
    if (slash != NULL && slash != parent) {
        *slash = '\0';
        char host[RECON_PATH_MAX * 2];
        snprintf(host, sizeof(host), "%s%s", g_host_root, parent);
        if (!make_tree(host)) {
            return false;
        }
    }

    if (!recon_fs_rename("/", source, origin)) {
        return false;
    }

    char info[RECON_PATH_MAX];
    snprintf(info, sizeof(info), "%s/%s.origin", trash_subdir(TRASH_INFO), name);
    recon_fs_remove("/", info);
    return true;
}

bool recon_fs_trash_purge(const char *name) {
    if (name == NULL || *name == '\0' || strchr(name, '/') != NULL) {
        set_error("invalid name");
        return false;
    }

    char target[RECON_PATH_MAX];
    snprintf(target, sizeof(target), "%s/%s", trash_subdir(TRASH_FILES), name);

    struct recon_dirent info_entry;
    bool is_dir = recon_fs_stat("/", target, &info_entry) &&
        info_entry.kind == RECON_FILE_DIRECTORY;

    bool ok = is_dir ? recon_fs_remove_tree("/", target)
                     : recon_fs_remove("/", target);
    if (!ok) {
        return false;
    }

    char info[RECON_PATH_MAX];
    snprintf(info, sizeof(info), "%s/%s.origin", trash_subdir(TRASH_INFO), name);
    recon_fs_remove("/", info);
    return true;
}

int recon_fs_trash_count(void) {
    char files_dir[RECON_PATH_MAX];
    snprintf(files_dir, sizeof(files_dir), "%s", trash_subdir(TRASH_FILES));

    int count = recon_fs_list("/", files_dir, NULL, 0);
    return count > 0 ? count : 0;
}

bool recon_fs_trash_empty(void) {
    char files_dir[RECON_PATH_MAX];
    snprintf(files_dir, sizeof(files_dir), "%s", trash_subdir(TRASH_FILES));

    struct recon_dirent entries[512];
    int count = recon_fs_list("/", files_dir, entries, 512);
    if (count < 0) {
        return true; /* No bin yet is an empty bin. */
    }
    if (count > 512) {
        count = 512;
    }

    bool ok = true;
    for (int i = 0; i < count; i++) {
        if (!recon_fs_trash_purge(entries[i].name)) {
            ok = false;
        }
    }
    return ok;
}

/* --- The file clipboard --- */

static char g_clip_path[RECON_PATH_MAX];
static bool g_clip_cut;

void recon_fs_clip_set(const char *path, bool cut) {
    if (path == NULL || *path == '\0') {
        recon_fs_clip_clear();
        return;
    }
    snprintf(g_clip_path, sizeof(g_clip_path), "%s", path);
    g_clip_cut = cut;
}

void recon_fs_clip_clear(void) {
    g_clip_path[0] = '\0';
    g_clip_cut = false;
}

bool recon_fs_clip_empty(void) {
    return g_clip_path[0] == '\0';
}

bool recon_fs_clip_get(char *out, size_t size, bool *cut_out) {
    if (g_clip_path[0] == '\0') {
        return false;
    }
    if (out != NULL) {
        snprintf(out, size, "%s", g_clip_path);
    }
    if (cut_out != NULL) {
        *cut_out = g_clip_cut;
    }
    return true;
}
