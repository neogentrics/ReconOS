/*
 * The text clipboard.
 *
 * There was none. The file clipboard in recon_fs holds a *path*, which is
 * what the explorer and the desktop copy between them, and text was simply
 * not something the system could carry from one place to another -- a line
 * could be read out of the Terminal and typed back into Notepad by hand, and
 * that was all.
 *
 * Separate from the file clipboard rather than an extra field on it. They are
 * the same idea and not the same thing: copying a file and then copying a
 * sentence should not make the file un-pasteable, which is what one clipboard
 * holding either would mean.
 *
 * One buffer for the whole system, because that is what makes it a clipboard.
 * A per-application one would be a scratch space with a misleading name.
 */

#ifndef RECON_CLIP_H
#define RECON_CLIP_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Hold a copy of `text`. Passing NULL or an empty string empties it.
 *
 * The text is copied rather than referenced: whatever was selected may be
 * deleted, or its window closed, long before anybody pastes.
 */
bool recon_clip_set_text(const char *text, size_t length);

/* What is held, or "" when nothing is. Never NULL, so callers can paste it
 * without checking first. */
const char *recon_clip_text(void);

/* How much is held, so a paste of text with a NUL in it is not truncated by
 * accident. */
size_t recon_clip_length(void);

bool recon_clip_empty(void);
void recon_clip_finish(void);

#endif
