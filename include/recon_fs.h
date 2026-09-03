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
#define RECON_DIR_SYSTEM_MODULES "/System/Modules"
#define RECON_DIR_TEMP "/Temp"

/*
 * Every user gets the same folders, created with the account. Which ones
 * exist is the system's decision rather than each user's, so a program can
 * rely on Documents being there without checking.
 */
#define RECON_USER_ADMIN "Administrator"
#define RECON_USER_FOLDERS { "Desktop", "Documents", "Downloads",     "Music", "Pictures", "Videos", ".Trash", ".Trash/files", ".Trash/info" }

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

/* --- Users --- */

/*
 * The account in use. There is one for now, created on first run, and it is
 * the administrator: the system is single-user until there is a way to log in
 * as somebody else.
 */
const char *recon_fs_current_user(void);

/*
 * Say who is signed in, and whether they may administer the system.
 *
 * This is what makes a limited account limited: with `administrator` false,
 * the filesystem refuses writes to /System and refuses to touch another
 * account's folder at all. Pass NULL for nobody, which is the state during
 * first-run setup and while the login screen is up.
 *
 * Enforced inside ReconOS only. A native program on the host underneath is
 * not subject to it, and will not be until ReconOS owns the machine.
 */
void recon_fs_set_user(const char *name, bool administrator);

/* Whether the signed-in account may write outside its own folder. */
bool recon_fs_user_is_administrator(void);

/*
 * Why a path is refused, or NULL if it is not.
 *
 * Separate from doing the thing, so an application can grey out what will not
 * work instead of offering it and reporting a failure afterwards.
 */
const char *recon_fs_refusal(const char *cwd, const char *path);

/*
 * A folder belonging to the current user, as a ReconOS path -- pass NULL for
 * the user's own directory. The returned string is valid until the next call.
 */
const char *recon_fs_user_dir(const char *subdirectory);

/* Create an account and its folders. Existing accounts are left alone. */
bool recon_fs_create_user(const char *name);

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

/*
 * Join a folder and a name into `out`.
 *
 * False, with `out` emptied, when the result would not fit -- rather than
 * writing as much as fits. A truncated path is not a shortened name for the
 * same file, it is the name of a different one, and the operations that build
 * paths this way go on to move, copy or delete what they name.
 *
 * A trailing slash on the folder is not doubled, so joining onto the root
 * gives "/x" rather than "//x".
 */
bool recon_fs_join(char *out, size_t size, const char *dir, const char *name);

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

/*
 * Remove a directory and everything in it. Separate from recon_fs_remove
 * because losing a tree is a different act from losing a file, and should be
 * asked for explicitly rather than happening because a path turned out to be
 * a directory. Refuses anything under /System.
 */
bool recon_fs_remove_tree(const char *cwd, const char *path);

/* Rename or move within the ReconOS filesystem. Refuses /System. */
bool recon_fs_rename(const char *cwd, const char *from, const char *to);

/*
 * Copy a file, or a directory and its contents. Refuses to copy a directory
 * into itself, which would otherwise recurse until the disk filled.
 */
bool recon_fs_copy(const char *cwd, const char *from, const char *to);

/*
 * A name not already taken in a directory, derived from `base` by adding a
 * number. Used where something must be created without asking for a name.
 */
bool recon_fs_unique_name(const char *cwd, const char *directory,
    const char *base, const char *extension, char *out, size_t size);

/*
 * True if the path is the system's own -- inside /System.
 *
 * What that permits depends on who is signed in: an administrator may change
 * these, a limited account may not. Applications use it to grey out what will
 * not work rather than to decide policy.
 */
bool recon_fs_is_protected(const char *cwd, const char *path);

/*
 * True if the path is one of the directories the layout is made of, which
 * nobody may remove. Not a permission: /System/Icons not existing is not a
 * state ReconOS knows how to be in.
 */
bool recon_fs_is_structural(const char *cwd, const char *path);

/* --- The recycle bin --- */

/*
 * Deleting puts things here first.
 *
 * A system where delete means gone is a system people are afraid to use. The
 * bin belongs to the user rather than to the machine -- one account emptying
 * it should not reach into another's -- and lives hidden inside their folder
 * so it does not clutter a listing that is meant to hold their own work.
 *
 * Each item keeps a note of where it came from, so restoring puts it back
 * rather than dropping it somewhere convenient.
 */

/* The bin's own paths, for a listing. */
const char *recon_fs_trash_dir(void);

/* Move something to the bin. Refuses /System, and refuses the bin itself. */
bool recon_fs_trash(const char *cwd, const char *path);

/* Put an item back where it came from. Fails if something is there now. */
bool recon_fs_trash_restore(const char *name);

/* Where an item came from, for showing in a listing. */
bool recon_fs_trash_origin(const char *name, char *out, size_t size);

/* Remove one item permanently, or everything. */
bool recon_fs_trash_purge(const char *name);
bool recon_fs_trash_empty(void);

/* How many items are in the bin. */
int recon_fs_trash_count(void);

/* True if the path is the bin or inside it, which changes what can be done
 * with it: a file in the bin is restored or purged, not deleted again. */
bool recon_fs_is_trash(const char *cwd, const char *path);

/* --- The file clipboard --- */

/*
 * What was cut or copied, held for the whole system rather than per-window:
 * copying in one place and pasting in another is the point of a clipboard, and
 * a per-application one could not do it.
 *
 * Only the path is kept. Holding the contents would mean a large file was
 * copied at Ctrl+C rather than at paste, and a cut whose source moved before
 * the paste should fail rather than write out a stale copy.
 */
void recon_fs_clip_set(const char *path, bool cut);
void recon_fs_clip_clear(void);
bool recon_fs_clip_empty(void);

/* The held path, and whether it was a cut. False when the clipboard is empty. */
bool recon_fs_clip_get(char *out, size_t size, bool *cut_out);

/* The reason the last operation failed, for reporting. */
const char *recon_fs_last_error(void);

#endif
