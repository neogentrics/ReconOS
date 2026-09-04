/*
 * What a file or folder is, said in a few lines.
 *
 * Properties has been in the context menu since the menus existed, greyed
 * out, because there was nothing to show. Hiding the entry instead would have
 * suggested there never would be.
 *
 * It lives here rather than in the explorer because the desktop offers the
 * same menu entry, and two copies of "how big is this, and when did it
 * change" is two places to answer the question differently.
 *
 * It is deliberately not in recon_fs: counting a folder's contents is a
 * filesystem question, but "2.4 KB" and "3 September 2026" are decisions
 * about how to say a number to a person, and the filesystem should not be
 * making those.
 */

#ifndef RECON_PROPS_H
#define RECON_PROPS_H

#include <stdbool.h>
#include <stddef.h>

#include "recon_fs.h"

/*
 * Describe `path` into `out`, as lines separated by newlines: the name, what
 * it is, how big, where it lives, and when it last changed.
 *
 * False when there is nothing there, in which case `out` holds the reason.
 */
bool recon_props_describe(const char *cwd, const char *path,
    char *out, size_t size);

/*
 * A size said the way a person would say it: bytes below a kilobyte, then
 * KB, MB, GB, with one decimal where that carries information.
 *
 * Exposed because listings want the same phrasing as the properties box; a
 * file that says "2.4 KB" in one place and "2441 bytes" in the other looks
 * like two different files.
 */
void recon_props_size(size_t bytes, char *out, size_t size);

/*
 * What kind of thing this is, by name: "Folder", "Text file", "Skin".
 *
 * By extension for files, because that is what the rest of the system goes
 * by -- the explorer's icons, what Notepad will open. Exposed for the same
 * reason as the size: a listing that calls something a File while its
 * properties call it a Text file is describing two things.
 */
const char *recon_props_kind(const struct recon_dirent *entry,
    const char *name);

/*
 * Which application opens a file of this name, or NULL if none does.
 *
 * Nothing in ReconOS opened a file by clicking it: the explorer put its size
 * in the status bar and the desktop opened the folder the file was in.
 * Notepad could open one, but only through its own File menu -- so a document
 * on the desktop was something you could see and not something you could
 * read.
 *
 * By extension, beside the type name, because the two answer the same
 * question and keeping them together is what stops a file being called a Text
 * file by one and handed to nothing by the other.
 */
const char *recon_props_opener(const char *name);

#endif
