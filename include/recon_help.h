/*
 * Help, and the change log.
 *
 * One window with a list of topics down the left and the chosen one on the
 * right. Two documents rather than two applications: "how do I do this" and
 * "what changed" are the same question asked at different times, and somebody
 * who has found one has found the other.
 *
 * The text is written out of assets/help into /System/Help on first run and
 * on every update, the way icons and skins are. That folder is the same shape
 * as the sources it came from -- one file per topic, plus an index naming
 * them in order -- so a topic can be corrected by editing a text file on the
 * running system, and the next update puts the shipped version back.
 *
 * The source of truth is docs/HELP.md and docs/CHANGELOG.md, turned into
 * these files by scripts/make-help.sh. Prose about a feature lives beside the
 * prose about every other feature rather than in the C file that implements
 * it, because that is the arrangement under which it stays written.
 */

#ifndef RECON_HELP_H
#define RECON_HELP_H

#include <stdbool.h>
#include <stddef.h>

struct recon_server;
struct recon_font;
struct recon_appwin;

#define RECON_DIR_HELP "/System/Help"
#define RECON_HELP_INDEX "index.txt"

/*
 * Where the account last saw the change log.
 *
 * Per account rather than per machine: an update is news to each person the
 * first time they sign in after it, and somebody who has not been at the
 * machine for a month should still be told what changed.
 */
#define RECON_HELP_SEEN_KEY "changelog/seen-version"

/*
 * Copy the shipped help into /System/Help, replacing what is there.
 *
 * Unlike icons and skins, this overwrites: help that describes a version the
 * system is no longer running is worse than no help, and there is nothing in
 * a help page somebody would have customised and want kept.
 *
 * Returns how many topics were written.
 */
int recon_help_write_defaults(void);

/*
 * Read a whole asset into memory, terminated. Caller frees.
 *
 * Lives with the rest of the startup copying in main.c, and is declared here
 * because the help text is the first asset that is not an image.
 */
char *recon_asset_read(const char *name, size_t *size_out);

struct recon_appwin *recon_help_create(struct recon_server *server,
    struct recon_font *font);

/* Open the window at a particular topic, by the title it has in the index.
 * Nothing happens if there is no such topic. */
void recon_help_show_topic(struct recon_appwin *win, const char *title);

/*
 * Whether a topic with this title exists.
 *
 * For the startup check that every application's declared help topic is a real
 * one. That declaration lives beside the application and the topics live in
 * docs/HELP.md, and nothing made the two agree: the Web viewer and Mail both
 * asked for a page called "Networking" that has never existed, so F1 from
 * either of them silently left whatever page was already showing. Photos, the
 * player and the Calendar each asked for "Writing", which exists and is about
 * Notepad -- which is worse, because it looks like an answer.
 *
 * Checked when a window declaring one is made, and reported as VT-M002.
 *
 * This said "at startup", which it never could and never did: the topic is
 * declared in an application's impl, and an impl is only reachable once a
 * window is being made from it. What actually happened was that the check ran
 * when somebody pressed F1, for the window in front -- so a program naming a
 * page that does not exist stayed invisible until the one person who needed
 * help happened to be in it.
 *
 * Window creation is the earliest moment the declaration exists, so that is
 * where it is checked now.
 */
bool recon_help_topic_exists(const char *title);

/*
 * The longest a topic title is, for a caller holding results.
 *
 * Titles come from the headings in docs/HELP.md, so this is a property of that
 * document rather than of anything here. Longer ones are skipped rather than
 * cut, because a truncated title handed back would be asked for again by
 * recon_help_show_topic and would not be found.
 */
#define RECON_HELP_TITLE_MAX 64

/*
 * Which topics mention this, by title, without opening Help.
 *
 * The same search the Help window's own box does -- titles and bodies, neither
 * preferred, because the change log's topics are titled with version numbers
 * so a title-only search finds nothing in two thirds of the document.
 *
 * Here rather than in the window because the Start menu asks it too. Somebody
 * typing "firewall" into a box that searches programs and stops there is being
 * told the system has nothing to say about firewalls, which is not true and is
 * the kind of wrong answer that stops people asking again.
 *
 * Reads from disk every time and keeps nothing: this runs on a keystroke in a
 * menu that is open for a couple of seconds, and a cache that outlives the
 * menu is a cache that has to be invalidated when the help is regenerated.
 * A hundred kilobytes read at typing speed is nothing.
 *
 * Fills `titles` and returns how many. Zero for an empty needle, so a menu
 * with nothing typed into it does not list every page in the help.
 */
int recon_help_search(const char *needle,
    char titles[][RECON_HELP_TITLE_MAX], int max);

/*
 * What the change log says about the version now running, or "" if it says
 * nothing about it.
 *
 * For the notice shown after an update, which is the current version's entry
 * rather than the whole document -- somebody who has just updated wants to
 * know what they got, not the history of everything.
 */
const char *recon_help_current_changes(void);

/* --- The notice shown after an update --- */

/*
 * Whether the signed-in account should be shown what changed.
 *
 * True when the change log says something about the running version and this
 * account has not acknowledged that version yet. Read after the account's hive
 * has been loaded, since the answer is that account's.
 */
bool recon_help_notice_due(void);

/*
 * The window that says what changed, with an OK button that records having
 * read it.
 *
 * Registered as an application so the shell can build it, focus it and put it
 * on the taskbar the way it does every other window -- but kept out of the
 * menus, because it is not something anybody goes looking for. The change log
 * is reached deliberately through Help.
 */
struct recon_appwin *recon_help_notice_create(struct recon_server *server,
    struct recon_font *font);

#endif
