/*
 * The ReconOS filesystem.
 *
 * ReconOS has one root and cannot see outside it. Every path it handles is
 * relative to that root, resolved against a single directory on the host, and
 * a path that would climb out of it is refused. That containment is the point:
 * ReconOS is a system with its own world, not a program browsing someone
 * else's disk.
 *
 * The host directory is visible to Linux, which is convenient while ReconOS is
 * being built. Nothing above this file knows or cares -- when ReconOS talks to
 * its own kernel instead, this is the file that changes.
 */

#ifndef RECON_FS_H
#define RECON_FS_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define RECON_PATH_MAX 1024
#define RECON_NAME_MAX 256

/*
 * The layout, which is deliberately Windows-shaped: the system's own files
 * kept apart from installed applications, kept apart from user documents.
 *
 *   /System     ReconOS itself -- its apps, settings, themes and icons
 *   /Apps       installed applications
 *   /Users      user documents, including the desktop
 *   /Temp       scratch space, not preserved
 */
#define RECON_DIR_SYSTEM "/System"
#define RECON_DIR_SYSTEM_APPS "/System/Apps"
#define RECON_DIR_SYSTEM_CONFIG "/System/Config"
#define RECON_DIR_SYSTEM_THEMES "/System/Themes"
/*
 * Icons the system and its native applications draw from. Installed
 * applications carry their own alongside themselves under /Apps, so removing
 * one takes its icons with it.
 */
#define RECON_DIR_SYSTEM_ICONS "/System/Icons"
#define RECON_DIR_APPS "/Apps"
#define RECON_DIR_USERS "/Users"
#define RECON_DIR_TEMP "/Temp"

enum recon_file_kind {
    RECON_FILE_REGULAR,
    RECON_FILE_DIRECTORY,
    RECON_FILE_OTHER,
};

struct recon_dirent {
    char name[RECON_NAME_MAX];
    enum recon_file_kind kind;
    size_t size;
};

/*
 * Open the filesystem rooted at a host directory, creating the standard layout
 * if it is not there. Pass NULL for the default, which RECONOS_ROOT overrides.
 */
bool recon_fs_init(const char *host_root);
void recon_fs_finish(void);

/* The host directory backing the root, for diagnostics. */
const char *recon_fs_host_root(void);

/* --- Paths --- */

/*
 * Resolve a ReconOS path to a host path.
 *
 * Relative paths resolve against `cwd`. The result is guaranteed to stay
 * inside the root: a path climbing past it is refused rather than clamped, so
 * an attempt to escape fails loudly instead of landing somewhere unintended.
 *
 * Returns false if the path is malformed or would escape.
 */
bool recon_fs_resolve(const char *cwd, const char *path,
    char *host_out, size_t host_size, char *canonical_out, size_t canonical_size);

/* --- Queries --- */

bool recon_fs_exists(const char *cwd, const char *path);
bool recon_fs_stat(const char *cwd, const char *path, struct recon_dirent *out);

/*
 * List a directory. Entries are written to `out` up to `max`; the total count
 * is returned, which may exceed `max`.
 */
int recon_fs_list(const char *cwd, const char *path,
    struct recon_dirent *out, int max);

/* --- Contents --- */

/* Read a whole file. Caller frees. Returns NULL if it cannot be read. */
char *recon_fs_read(const char *cwd, const char *path, size_t *size_out);

bool recon_fs_write(const char *cwd, const char *path, const char *data, size_t size);
bool recon_fs_append(const char *cwd, const char *path, const char *data, size_t size);

bool recon_fs_mkdir(const char *cwd, const char *path);

/* Remove a file, or an empty directory. Refuses anything under /System. */
bool recon_fs_remove(const char *cwd, const char *path);

/* The reason the last operation failed, for reporting. */
const char *recon_fs_last_error(void);

#endif
