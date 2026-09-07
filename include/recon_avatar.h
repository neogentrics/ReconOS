/*
 * Account pictures: the face beside a name.
 *
 * A login screen with a grey rectangle where a person's picture should be
 * looks unfinished, and it is the screen everybody sees most. This gives
 * every account something, whether or not anybody has chosen one.
 *
 * Three layers, in order of preference:
 *
 *   A picture the account chose, from the set ReconOS draws for itself at
 *   first run, or any file dropped into /System/Icons named avatar-something.
 *
 *   Failing that, a coloured disc with the account's initial on it. Every
 *   account has one of these without anybody doing anything, the colour is
 *   worked out from the name so it is stable and two accounts rarely match,
 *   and it needs no file at all.
 *
 * The choice is kept in the system registry rather than in the account file:
 * the account file is parsed field by field and adding one to it would break
 * every system that already has one. A picture is a preference, and
 * preferences live in the registry.
 */

#ifndef RECON_AVATAR_H
#define RECON_AVATAR_H

#include <stdbool.h>
#include <stddef.h>

#include "recon_ui.h"

/* Icons whose name begins with this are account pictures. Named by prefix so
 * the set can be listed by looking for it rather than from a second table
 * that would have to be kept in step. */
#define RECON_AVATAR_PREFIX "avatar-"

/* How many pictures there are to choose from. */
int recon_avatar_count(void);

/* The name of one, for showing or storing. False once index runs past the
 * end. */
bool recon_avatar_at(int index, char *out, size_t size);

/* The picture an account has chosen, or "" for the drawn initial. */
const char *recon_avatar_of(const char *account);

/* Choose one. Pass NULL or "" to go back to the drawn initial. */
bool recon_avatar_set(const char *account, const char *avatar);

/*
 * Delete the files for pictures this program has renamed.
 *
 * Called after the icon set is written, so the picker offers the new name and
 * not both. Accounts that had chosen the old one follow it -- see the table in
 * recon_avatar.c -- which is why this can be a deletion rather than a
 * migration somebody has to run.
 */
void recon_avatar_retire_old_files(void);

/*
 * Draw an account's picture, whatever it turns out to be.
 *
 * Always draws something: an account with no picture chosen, or one whose
 * chosen file has since been deleted, still gets its initial on a disc rather
 * than a hole where a face should be.
 */
void recon_avatar_draw(struct recon_panel *panel, struct recon_font *font,
    const char *account, int x, int y, int size);

#endif
