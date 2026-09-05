/*
 * Loading modules, and the registry they add to.
 *
 * Internal to ReconOS -- a module author reads recon_module.h instead.
 *
 * Modules are found in two places and behave differently for it:
 *   /System/Modules/*.rts   loaded at startup, part of the system
 *   /Apps/*.rex             loaded at startup, offered to the user
 *
 * Both are loaded with dlopen. ReconOS is linked so its own symbols are
 * visible to what it loads, which is what lets a module call the same
 * functions the built-in code does rather than everything having to pass
 * through a hand-maintained table of function pointers. The cost is that a
 * module is tied to the build it was compiled against, and the ABI number in
 * its descriptor is what turns that from a crash into a refusal.
 */

#ifndef RECON_MODULES_H
#define RECON_MODULES_H

#include <stdbool.h>
#include <stddef.h>

#include "recon_module.h"
#include "recon_version.h"

struct recon_server;
struct recon_font;

/* Where modules are looked for, as ReconOS paths. */
#define RECON_DIR_MODULES "/System/Modules"

/* Long enough for a sentence saying why a module would not load. */
#define RECON_MODULES_PROBLEM_MAX 160

struct recon_module_state {
    char name[64];
    char version[32];
    char description[128];
    char path[256];
    /* True for an application (.rex), false for a system module (.rts). */
    bool is_app;
    bool loaded;
    /* Why it is not loaded, when it is not. */
    char problem[RECON_MODULES_PROBLEM_MAX];
};

void recon_modules_init(struct recon_server *server, struct recon_font *font);
void recon_modules_finish(void);

/*
 * Load everything in the module directories. Returns how many loaded.
 *
 * One module failing does not stop the others: a broken thing in /Apps should
 * cost the user that application, not their desktop.
 */
int recon_modules_load_all(void);

/*
 * Put the modules this build shipped with into the ReconOS filesystem, if they
 * are not there already.
 *
 * A freshly created filesystem has an empty /Apps, and a desktop whose
 * applications all live in modules would come up with none. This is the
 * equivalent of what a real installer would do once: it copies rather than
 * points, so a module in /Apps is a file the user owns and can remove, and
 * removing it keeps it removed.
 *
 * Returns how many were installed.
 */
int recon_modules_install_shipped(void);

/* Load or unload one by path or by name. */
bool recon_modules_load(const char *reconos_path);
bool recon_modules_unload(const char *name);

/*
 * --- Installing ---
 *
 * Loading and installing are different acts and it is worth keeping them
 * apart. Loading runs code that is already in the system; installing puts it
 * there, so that it is still there next time.
 *
 * `recon_modules_install` takes a `.rex` or `.rts` anywhere in the ReconOS
 * filesystem, copies it into `/Apps` or `/System/Modules` by its extension,
 * and loads it. The copy is the point: an application installed from
 * somebody's Downloads folder must not stop working when they tidy up.
 *
 * Refused for a limited account, because a module runs inside ReconOS with
 * everything ReconOS can do. Installing one is closer to installing a driver
 * than to saving a file, and the account rules should say so.
 *
 * `recon_modules_uninstall` unloads it and removes the file it came from, so
 * removing something removes it rather than hiding it until the next start.
 * It refuses when the module will not unload -- something it registered is
 * still in use -- because deleting the file underneath running code would
 * turn a tidy-up into a crash on the next start.
 */
bool recon_modules_install(const char *reconos_path);
bool recon_modules_uninstall(const char *name);

/*
 * Where a loaded module's file is, so something can be said about it and it
 * can be removed. Empty for one built into ReconOS.
 */
bool recon_modules_path_of(const char *name, char *out, size_t size);

/* Why the last load or unload failed. */
const char *recon_modules_last_error(void);

/* What is loaded, and what was tried and refused. */
int recon_modules_count(void);
bool recon_modules_at(int index, struct recon_module_state *out);

/* --- The application registry --- */

/*
 * Every application ReconOS knows about, whether compiled in or contributed by
 * a module. The built-in ones register through exactly this, so the interface
 * a module uses is the one the system uses -- an extension path that only
 * outsiders take is an extension path nobody keeps working.
 */
struct recon_installed_app {
    char name[64];
    char icon[64];
    bool in_menu;
    /* The module that contributed it, or empty for one built in. */
    char module[64];

    /*
     * Which release of this applet is the one answering to the name.
     *
     * For a built-in that is the system's version. For one a module
     * contributed it is whatever the module declared, which is how an applet
     * gets updated without the system being rebuilt.
     */
    char version[RECON_VERSION_MAX];

    /*
     * True when a module has taken over a name ReconOS ships an application
     * for, and the built-in is standing behind it.
     *
     * Worth showing rather than hiding: "Notepad 0.3.1, replacing the built-in
     * 0.3.0" is a different sentence from "Notepad 0.3.1", and somebody
     * wondering why their Notepad looks different deserves the first one.
     */
    bool replaces_builtin;
    char builtin_version[RECON_VERSION_MAX];

    /*
     * Turned off. Still registered, still listed where applications are
     * managed, and offered nowhere else: not in the menus, and not buildable.
     *
     * Distinct from removed on purpose. Removing a built-in is not possible
     * -- it is compiled in -- and "I do not want this and cannot delete it"
     * is a real thing to want, which this is the answer to.
     */
    bool disabled;
};

/*
 * The registered name a target refers to, or NULL.
 *
 * Exact first, then case-insensitively, then allowing the registered name to
 * be the tail of the target. That last step is for shortcuts written before
 * an application was renamed -- a desktop file saying "ReconOS Terminal"
 * should still open the Terminal rather than quietly doing nothing, which is
 * how a rename turns into a pile of dead icons.
 */
const char *recon_installed_app_resolve(const char *target);

/* The icon name for a registered application, stable for as long as it stays
 * registered. NULL if it has none. */
const char *recon_installed_app_icon(const char *name);

/*
 * Whether an application is turned off, and turning one off or back on.
 *
 * The list of disabled names lives in the system registry, so it survives a
 * restart -- an application that came back after a reboot would be a setting
 * that did not mean anything.
 */
bool recon_installed_app_is_disabled(const char *name);
bool recon_installed_app_set_disabled(const char *name, bool disabled);

int recon_installed_app_count(void);
bool recon_installed_app_at(int index, struct recon_installed_app *out);

/*
 * Build the window for an application, or return the one it already has.
 * NULL if there is no such application or it could not be created.
 */
struct recon_appwin *recon_installed_app_window(const char *name);

/* The window an application already has, without creating one. */
struct recon_appwin *recon_installed_app_existing(const char *name);

/*
 * --- Who owns an application's window ---
 *
 * This registry does. It built the window, it hands the same one back on
 * every open, and it destroys it.
 *
 * It used to be the shell: the shell destroyed the windows and called
 * recon_installed_apps_forget_windows to stop the registry handing out
 * pointers to freed memory. That was workable and it made the desktop shell
 * impossible to restart -- restarting it closed every open window, so
 * repairing a taskbar cost somebody the document they were writing.
 *
 * The shell *borrows* them now. It keeps a list of the windows that exist so
 * it can draw a taskbar and route clicks, and a new shell takes over
 * whatever this registry is holding.
 */

/*
 * Destroy every window and forget them.
 *
 * For a change of account: signing in as somebody else must not hand them
 * the previous person's File Explorer, still showing the folder they left it
 * in. That is the one case where the windows genuinely have to end.
 */
void recon_installed_apps_close_windows(void);

/*
 * Every window that currently exists, so a new shell can take over the ones
 * the previous one was drawing. Returns how many were written.
 */
int recon_installed_app_windows(struct recon_appwin **out, int max);

/*
 * Register something built into ReconOS. Same registry as a module's, with the
 * contributing module recorded as empty.
 */
bool recon_register_builtin_app(const struct recon_app_registration *app);

/* --- Commands modules added --- */

int recon_module_command_count(void);
bool recon_module_command_at(int index,
    struct recon_command_registration *out);

#endif
