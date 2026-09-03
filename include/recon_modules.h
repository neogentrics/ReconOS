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

struct recon_server;
struct recon_font;

/* Where modules are looked for, as ReconOS paths. */
#define RECON_DIR_MODULES "/System/Modules"

struct recon_module_state {
    char name[64];
    char version[32];
    char description[128];
    char path[256];
    /* True for an application (.rex), false for a system module (.rts). */
    bool is_app;
    bool loaded;
    /* Why it is not loaded, when it is not. */
    char problem[160];
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
 * Forget every cached window without destroying any of them.
 *
 * For a change of account. The windows themselves belong to the shell, which
 * destroys them; this only stops the registry handing the next person the
 * previous one's File Explorer, still showing the folder they left it in.
 * Calling it without destroying them leaks; destroying them without calling
 * it leaves dangling pointers behind.
 */
void recon_installed_apps_forget_windows(void);

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
