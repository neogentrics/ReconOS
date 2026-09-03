/*
 * The ReconOS registry: what the system remembers between runs.
 *
 * Settings, and the small facts a desktop has to keep about how it is being
 * used -- which folder a window was last showing, where a window was put,
 * whether something has been set up already. Without somewhere to put these,
 * every restart is a first run, which is what ReconOS did until now.
 *
 * Two hives, for the same reason Windows has two:
 *
 *   SYSTEM   /System/Config/system.reg   the machine's, shared by everyone
 *   USER     /Users/<name>/user.reg      one account's own
 *
 * The split is not decoration. A theme belongs to a person; which modules are
 * allowed to load belongs to the machine. Putting both in one place means one
 * account's preference silently becomes everyone's.
 *
 * Keys are paths, like a filesystem, so related settings group naturally and
 * can be listed together:
 *
 *   desktop/icon-size
 *   apps/explorer/last-folder
 *   windows/Notepad/x
 *
 * Values are text on disk. The typed accessors parse on the way out, and a
 * value that will not parse yields the fallback rather than a zero -- a
 * corrupted number should look like a missing setting, not like a real
 * setting of 0.
 *
 * Written through on every change. These files are small and changes are
 * rare, and the alternative -- holding them in memory until shutdown -- loses
 * everything when the desktop crashes, which is exactly when it is worth
 * knowing what the settings were.
 */

#ifndef RECON_REGISTRY_H
#define RECON_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>

/* Where the files live. */
#define RECON_REGISTRY_SYSTEM_FILE "/System/Config/system.reg"
#define RECON_REGISTRY_USER_FILE "user.reg"

#define RECON_REGISTRY_KEY_MAX 128
#define RECON_REGISTRY_VALUE_MAX 512

enum recon_registry_scope {
    /* The machine's. Shared by every account. */
    RECON_REG_SYSTEM,
    /* The account that is logged in. */
    RECON_REG_USER,
};

/*
 * Load both hives. Missing files are not an error -- a system that has never
 * saved anything has nothing to load, which is a normal first run rather than
 * a problem to report.
 */
bool recon_registry_init(void);

/* Write anything outstanding and let go. */
void recon_registry_finish(void);

/* --- Reading --- */

/*
 * The value, or `fallback` if there is none. The pointer is valid until that
 * key is next written or the registry is closed.
 */
const char *recon_registry_get(enum recon_registry_scope scope,
    const char *key, const char *fallback);

int recon_registry_get_int(enum recon_registry_scope scope,
    const char *key, int fallback);

bool recon_registry_get_bool(enum recon_registry_scope scope,
    const char *key, bool fallback);

bool recon_registry_has(enum recon_registry_scope scope, const char *key);

/* --- Writing --- */

/*
 * Set a value, creating the key if it is new. Writing the value a key already
 * has does nothing and touches no disk, so saving a window's position every
 * time it stops moving costs nothing when it has not moved.
 */
bool recon_registry_set(enum recon_registry_scope scope,
    const char *key, const char *value);

bool recon_registry_set_int(enum recon_registry_scope scope,
    const char *key, int value);

bool recon_registry_set_bool(enum recon_registry_scope scope,
    const char *key, bool value);

/* Forget a key. False if it was not there. */
bool recon_registry_remove(enum recon_registry_scope scope, const char *key);

/* Forget everything under a prefix, e.g. "windows/Notepad". Returns how many
 * went. */
int recon_registry_remove_all(enum recon_registry_scope scope,
    const char *prefix);

/* --- Listing --- */

/*
 * Walk the keys under a prefix, in order. Pass an empty prefix for all of
 * them. `index` counts only matching keys.
 *
 * Both out parameters may be NULL.
 */
bool recon_registry_at(enum recon_registry_scope scope, const char *prefix,
    int index, const char **key_out, const char **value_out);

int recon_registry_count(enum recon_registry_scope scope, const char *prefix);

/* --- Errors --- */

const char *recon_registry_last_error(void);

#endif
