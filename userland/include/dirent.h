/*
 * <dirent.h> -- reading a directory.
 *
 * --- Why the whole directory is read at once ---
 *
 * `SYS_LIST` answers with **every name in one call**, NUL-terminated and back
 * to back, and refuses rather than truncating: a caller compares the size it
 * is given against the size it offered, and a bigger number means nothing was
 * written and says how much to come back with.
 *
 * That is the opposite of the interface `readdir` presents, which is one name
 * at a time with a cursor. So `opendir` reads the lot into a buffer and
 * `readdir` walks it.
 *
 * The kernel's shape is the better one and the header there says why: a
 * directory read in pieces has no guarantee at all about what a caller sees
 * when the directory changes between two of them -- a name can appear twice or
 * not at all, and nothing in the interface can say so. Reading it in one call
 * is a directory as it was at one instant.
 *
 * What this costs is that `opendir` allocates, so it can fail with ENOMEM
 * where a cursor-based one could not, and a directory larger than the buffer
 * is a directory this cannot read. Both are honest failures rather than
 * silently short answers.
 *
 * --- `d_type` is always DT_UNKNOWN ---
 *
 * `SYS_LIST` returns names and nothing else. POSIX allows exactly this and
 * every careful program handles it by calling `stat`.
 *
 * It was checked before being written this way: **the desktop reads `d_name`
 * at every one of its sites and `d_type` at none of them.** Measured with
 * `grep` over `src/`, because the alternative -- assuming -- would have meant
 * discovering it from a file manager that shows every folder as a file.
 */

#ifndef RECON_DIRENT_H
#define RECON_DIRENT_H

/* Quoted, so it finds the one beside it rather than the host's.
 * See the note in userland/libc/posix.c. */
#include "sys/types.h"

#define RECON_NAME_MAX_IN_DIRENT 256

struct dirent {
	ino_t d_ino;
	unsigned char d_type;
	char d_name[RECON_NAME_MAX_IN_DIRENT];
};

#define DT_UNKNOWN	0
#define DT_DIR		4
#define DT_REG		8

typedef struct recon_dir DIR;

DIR *opendir(const char *path);
struct dirent *readdir(DIR *dir);
int closedir(DIR *dir);

#endif /* RECON_DIRENT_H */
