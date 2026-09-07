/*
 * Which icon belongs to a Wayland client.
 *
 * A client hands its compositor an `app_id` and never a picture: a string like
 * "org.gnome.Calculator", "foot", or "weston-terminal". Every client window in
 * ReconOS therefore wore the same generic application icon, which is honest
 * and is also a taskbar where six different programs look identical.
 *
 * --- Why this is not guessing ---
 *
 * The note this replaces said that guessing an icon from a reverse-DNS string
 * would be wrong more often than right, and that is true of *guessing*. What
 * is done here is not a guess, because every answer has to be confirmed
 * against something that exists:
 *
 *   1. A mapping written down in /System/Config/app-icons, which is a
 *      statement rather than an inference. A package can place one; a person
 *      can edit one.
 *   2. An application ReconOS has registered under that name, which already
 *      carries an icon it chose for itself.
 *   3. The last dot-separated part of the app_id, lowercased, IF an icon by
 *      that name actually exists. "org.gnome.Calculator" becomes "calculator"
 *      and is used only when /System/Icons/calculator.png is there to be used.
 *
 * The third rule is the one that looks like guessing and is not. It cannot
 * produce a wrong picture, only a right one or none: an app_id that reduces to
 * a name nobody has drawn an icon for falls through to the generic one, which
 * is exactly where it started. The failure mode is "no better than before",
 * which is the only failure mode worth having.
 *
 * What it CAN do is give the wrong icon to a client that happens to reduce to
 * a name ReconOS uses for something else -- a client called
 * "com.example.Notepad" would take Notepad's icon. That is why the written-down
 * mapping is first: it is the way to say "no, this one is different", and the
 * only rule that can override the automatic ones.
 */

#ifndef RECON_APPICON_H
#define RECON_APPICON_H

/* Where the written-down mapping lives, under the filesystem root. */
#define RECON_APPICON_FILE "/System/Config/app-icons"

/*
 * The icon name for a client, never NULL.
 *
 * Falls back to the generic application icon, so a caller does not have to ask
 * whether there was an answer -- there always is one, and the honest one when
 * nothing is known is the icon that says "a program".
 *
 * The returned string is owned by this file and stays valid until the next
 * call, which is the same arrangement recon_props_opener makes and for the
 * same reason: every caller uses it immediately, to draw.
 */
const char *recon_appicon_for(const char *app_id);

/*
 * Create the mapping file if it is not there, with the format written in it.
 *
 * Once, and never again. Unlike the help pages -- rewritten every start,
 * because help describing an older version is worse than none -- this is
 * somebody's list, and a line added by hand that the system helpfully replaced
 * on the next boot would be a file nobody edits twice.
 */
void recon_appicon_write_default(void);

/* Re-read the written-down mapping. Called when icons are reloaded, so a file
 * placed by a package takes effect without a restart. */
void recon_appicon_forget(void);

#endif /* RECON_APPICON_H */
