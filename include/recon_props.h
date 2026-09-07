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

/*
 * --- Choosing what opens a kind of file ---
 *
 * Everything recon_props_opener does on its own is inheritance: an application
 * says what it opens and the system agrees. That covers the common case and
 * has no room in it for somebody who simply wants a different program -- two
 * applications claiming the same extension was first-come, and there was no
 * way to change one without uninstalling something.
 *
 * A choice made here is a *user's*, so it lives in their settings and beats
 * everything an application declares. Clearing it goes back to the declared
 * answer rather than to nothing, which is the same rule the presets follow
 * everywhere else in this system: what shipped cannot be deleted, and what you
 * added can.
 *
 * `name` is a file's name; only its extension is used.
 */
/*
 * Every application that could open a file, for offering a choice among them.
 *
 * "Could" is a wider question than "does": this is every application that
 * declares it opens files at all, because somebody choosing a different
 * program for a .log is choosing among the programs that read files, not among
 * the ones that claim that extension -- if it were the latter the list would
 * have one entry in it and be no use.
 *
 * Fills `names` with registered names, which stay valid as long as the
 * applications do, and returns how many. Turned-off applications are left out,
 * because an application that is off should not be offered as somewhere to
 * send a file.
 */
#define RECON_OPEN_WITH_MAX 8
int recon_props_openers(const char **names, int max);

/*
 * Where a chosen opener is written down: the section of the registry.
 *
 * **No trailing slash.** The registry's own listing takes a prefix and checks
 * that what follows it is a separator or the end, so "open-with/" matches
 * nothing at all -- the character after it is the dot of the extension. The
 * slash belongs to the key being built, not to the name of the section.
 *
 * That cost a round: the page listed nothing while the keys were plainly in
 * the file, because one constant was standing for two slightly different
 * things.
 */
#define RECON_OPEN_WITH_PREFIX "open-with"

/*
 * Does this application say it opens files of this kind?
 *
 * The question recon_props_openers deliberately does not ask. That one offers
 * every program that reads files at all, because narrowing it to the ones
 * claiming the extension would leave a list with one entry in it and nothing
 * to choose between -- which is the whole reason a person opened the menu.
 *
 * So the list stays wide and this is how a caller warns instead. A picture
 * pointed at Notepad opens as a screen of binary, and that is a thing somebody
 * is allowed to do; it is not a thing they should be able to do without being
 * told first.
 *
 * False when the application does not exist, declares no file types, or
 * declares some that do not include this one. True is a claim, not a promise:
 * an application saying it opens .png can still fail on a particular file.
 */
bool recon_props_claims(const char *application, const char *name);

/*
 * A sentence about what a file actually is, from its first bytes, or an empty
 * string when there is nothing worth saying.
 *
 * `recon_props_claims` decides by extension, which is right -- only a program
 * knows what it will open, and nothing here can ask it. This does not change
 * that decision; it improves what the warning *says*, and adds the one thing
 * an extension cannot tell anybody: that the name and the contents disagree.
 *
 * Empty far more often than not, and deliberately. Plain text, an unrecognised
 * format, and a file whose name matches its contents all produce nothing,
 * because a warning that appears every time is one people stop reading.
 */
void recon_props_open_with_warning(const char *cwd, const char *name,
    const char *application, char *out, size_t size);

bool recon_props_set_opener(const char *name, const char *application);
bool recon_props_clear_opener(const char *name);

/* The chosen application for this kind of file, or NULL when nobody has
 * chosen one. Distinct from recon_props_opener, which always has an answer:
 * this says whether the answer was a decision. */
const char *recon_props_chosen_opener(const char *name);

/*
 * The icon a file should be drawn with, by its name.
 *
 * Never NULL: a name nothing recognises gets the plain sheet, which is what
 * a file that is only a file should look like.
 *
 * Here rather than in the explorer because it is the third question of the
 * same shape as the two above -- what kind of thing this is, what opens it,
 * and what it looks like -- and the answers have to agree. Two files deciding
 * separately what a .mp3 is would eventually disagree.
 */
const char *recon_props_icon(const char *name);

#endif
