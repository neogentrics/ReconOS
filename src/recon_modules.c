/*
 * The ReconOS module loader. See include/recon_modules.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <wlr/util/log.h>

#include "ReconOS.h"
#include "recon_appwin.h"
#include "recon_fs.h"
#include "recon_modules.h"
#include "recon_users.h"

#define MODULES_MAX 32
#define APPS_MAX 32
#define COMMANDS_MAX 32

/* One loaded, or attempted, module. */
struct module_slot {
    struct recon_module_state state;
    void *handle;
    const struct recon_module_descriptor *descriptor;
    bool used;
};

/* One registered application, and the window it has if it has been opened. */
struct app_slot {
    struct recon_installed_app info;
    struct recon_appwin *(*create)(struct recon_server *server,
        struct recon_font *font);
    struct recon_appwin *window;
    bool used;
};

static struct recon_server *g_server;
static struct recon_font *g_font;

static struct module_slot g_modules[MODULES_MAX];
static struct app_slot g_apps[APPS_MAX];
static struct recon_command_registration g_commands[COMMANDS_MAX];
static int g_command_count;

static char g_error[256];

/*
 * The module being loaded right now, so registrations made from its load() can
 * be attributed to it without every call having to say who it is. Cleared as
 * soon as load() returns, because a registration arriving later has no honest
 * owner to record.
 */
static const char *g_loading;

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, args);
    va_end(args);
}

const char *recon_modules_last_error(void) {
    return g_error[0] != '\0' ? g_error : "no error";
}

void recon_modules_init(struct recon_server *server, struct recon_font *font) {
    g_server = server;
    g_font = font;
}

struct recon_server *recon_module_server(void) {
    return g_server;
}

struct recon_font *recon_module_font(void) {
    return g_font;
}

/* --- The application registry --- */

static struct app_slot *app_slot_for(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    for (int i = 0; i < APPS_MAX; i++) {
        if (g_apps[i].used && strcmp(g_apps[i].info.name, name) == 0) {
            return &g_apps[i];
        }
    }
    return NULL;
}

static bool register_app(const struct recon_app_registration *app,
        const char *module) {
    if (app == NULL || app->name == NULL || app->create == NULL) {
        set_error("an application needs a name and a way to create it");
        return false;
    }
    if (app_slot_for(app->name) != NULL) {
        set_error("an application called '%s' is already registered", app->name);
        return false;
    }

    for (int i = 0; i < APPS_MAX; i++) {
        if (g_apps[i].used) {
            continue;
        }

        struct app_slot *slot = &g_apps[i];
        memset(slot, 0, sizeof(*slot));
        slot->used = true;
        slot->create = app->create;
        snprintf(slot->info.name, sizeof(slot->info.name), "%s", app->name);
        snprintf(slot->info.icon, sizeof(slot->info.icon), "%s",
            app->icon != NULL ? app->icon : "");
        snprintf(slot->info.module, sizeof(slot->info.module), "%s",
            module != NULL ? module : "");
        slot->info.in_menu = app->in_menu;
        return true;
    }

    set_error("no room for another application");
    return false;
}

bool recon_register_app(const struct recon_app_registration *app) {
    return register_app(app, g_loading);
}

bool recon_register_builtin_app(const struct recon_app_registration *app) {
    return register_app(app, NULL);
}

bool recon_app_in_use(const char *name) {
    struct app_slot *slot = app_slot_for(name);
    return slot != NULL && slot->window != NULL;
}

bool recon_unregister_app(const char *name) {
    struct app_slot *slot = app_slot_for(name);
    if (slot == NULL) {
        set_error("no application called '%s'", name);
        return false;
    }

    /*
     * A window whose code is about to be unloaded is a window that will crash
     * the moment anything touches it -- a redraw, a click, the taskbar asking
     * for its title. Refuse rather than take the desktop down.
     */
    if (slot->window != NULL) {
        set_error("'%s' is open; close it first", name);
        return false;
    }

    memset(slot, 0, sizeof(*slot));
    return true;
}

int recon_installed_app_count(void) {
    int count = 0;
    for (int i = 0; i < APPS_MAX; i++) {
        if (g_apps[i].used) {
            count++;
        }
    }
    return count;
}

/* Registry order, skipping the gaps that unregistering leaves. */
static struct app_slot *app_at(int index) {
    int seen = 0;
    for (int i = 0; i < APPS_MAX; i++) {
        if (!g_apps[i].used) {
            continue;
        }
        if (seen == index) {
            return &g_apps[i];
        }
        seen++;
    }
    return NULL;
}

bool recon_installed_app_at(int index, struct recon_installed_app *out) {
    struct app_slot *slot = app_at(index);
    if (slot == NULL || out == NULL) {
        return false;
    }
    *out = slot->info;
    return true;
}

const char *recon_installed_app_icon(const char *name) {
    struct app_slot *slot = app_slot_for(name);
    if (slot == NULL || slot->info.icon[0] == '\0') {
        return NULL;
    }
    return slot->info.icon;
}

const char *recon_installed_app_resolve(const char *target) {
    if (target == NULL || *target == '\0') {
        return NULL;
    }

    for (int i = 0; i < APPS_MAX; i++) {
        if (g_apps[i].used && strcmp(g_apps[i].info.name, target) == 0) {
            return g_apps[i].info.name;
        }
    }
    for (int i = 0; i < APPS_MAX; i++) {
        if (g_apps[i].used && strcasecmp(g_apps[i].info.name, target) == 0) {
            return g_apps[i].info.name;
        }
    }

    /* "ReconOS Terminal" ends with "Terminal". Only whole words, so "Notepad"
     * cannot be matched by something merely ending in "pad". */
    size_t target_length = strlen(target);
    for (int i = 0; i < APPS_MAX; i++) {
        if (!g_apps[i].used) {
            continue;
        }
        size_t name_length = strlen(g_apps[i].info.name);
        if (name_length >= target_length) {
            continue;
        }
        const char *tail = target + target_length - name_length;
        if (tail[-1] == ' ' && strcasecmp(tail, g_apps[i].info.name) == 0) {
            return g_apps[i].info.name;
        }
    }

    return NULL;
}

void recon_installed_apps_forget_windows(void) {
    for (int i = 0; i < APPS_MAX; i++) {
        g_apps[i].window = NULL;
    }
}

struct recon_appwin *recon_installed_app_existing(const char *name) {
    struct app_slot *slot = app_slot_for(recon_installed_app_resolve(name));
    return slot != NULL ? slot->window : NULL;
}

struct recon_appwin *recon_installed_app_window(const char *name) {
    struct app_slot *slot = app_slot_for(recon_installed_app_resolve(name));
    if (slot == NULL) {
        set_error("no application called '%s'", name != NULL ? name : "");
        return NULL;
    }
    if (slot->window != NULL) {
        return slot->window;
    }

    /*
     * Built on first open rather than at registration. An application nobody
     * has opened should cost nothing but its entry in this list -- which is
     * the whole argument for modules over compiling everything in.
     */
    slot->window = slot->create(g_server, g_font);
    if (slot->window == NULL) {
        set_error("'%s' could not be started", name);
    }
    return slot->window;
}

/* --- Commands --- */

bool recon_register_command(const struct recon_command_registration *command) {
    if (command == NULL || command->name == NULL || command->run == NULL) {
        set_error("a command needs a name and something to run");
        return false;
    }
    if (g_command_count >= COMMANDS_MAX) {
        set_error("no room for another command");
        return false;
    }
    for (int i = 0; i < g_command_count; i++) {
        if (strcasecmp(g_commands[i].name, command->name) == 0) {
            set_error("a command called '%s' already exists", command->name);
            return false;
        }
    }

    g_commands[g_command_count++] = *command;
    return true;
}

bool recon_unregister_command(const char *name) {
    for (int i = 0; i < g_command_count; i++) {
        if (strcasecmp(g_commands[i].name, name) != 0) {
            continue;
        }
        /* Order is not meaningful, so the last entry fills the gap. */
        g_commands[i] = g_commands[g_command_count - 1];
        g_command_count--;
        return true;
    }
    set_error("no command called '%s'", name);
    return false;
}

int recon_module_command_count(void) {
    return g_command_count;
}

bool recon_module_command_at(int index,
        struct recon_command_registration *out) {
    if (index < 0 || index >= g_command_count || out == NULL) {
        return false;
    }
    *out = g_commands[index];
    return true;
}

/* --- Loading --- */

static struct module_slot *module_slot_for(const char *name) {
    for (int i = 0; i < MODULES_MAX; i++) {
        if (g_modules[i].used && strcmp(g_modules[i].state.name, name) == 0) {
            return &g_modules[i];
        }
    }
    return NULL;
}

static struct module_slot *free_module_slot(void) {
    for (int i = 0; i < MODULES_MAX; i++) {
        if (!g_modules[i].used) {
            return &g_modules[i];
        }
    }
    return NULL;
}

/* Record something that could not be loaded, so it can be reported rather than
 * silently missing. A module that fails invisibly is a module nobody fixes. */
static void remember_failure(const char *path, bool is_app, const char *problem) {
    struct module_slot *slot = free_module_slot();
    if (slot == NULL) {
        return;
    }

    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->state.loaded = false;
    slot->state.is_app = is_app;
    snprintf(slot->state.path, sizeof(slot->state.path), "%s", path);
    snprintf(slot->state.problem, sizeof(slot->state.problem), "%s", problem);

    /* Named by its file, since its descriptor could not be read. */
    const char *leaf = strrchr(path, '/');
    snprintf(slot->state.name, sizeof(slot->state.name), "%s",
        (leaf != NULL && leaf[1] != '\0') ? leaf + 1 : path);
}

static bool ends_with(const char *text, const char *suffix) {
    size_t text_length = strlen(text);
    size_t suffix_length = strlen(suffix);
    return text_length >= suffix_length &&
        strcasecmp(text + text_length - suffix_length, suffix) == 0;
}

bool recon_modules_load(const char *reconos_path) {
    if (reconos_path == NULL || *reconos_path == '\0') {
        set_error("no path given");
        return false;
    }

    bool is_app = ends_with(reconos_path, RECON_APP_EXT);
    if (!is_app && !ends_with(reconos_path, RECON_MODULE_EXT)) {
        set_error("'%s' is not a %s or %s", reconos_path,
            RECON_MODULE_EXT, RECON_APP_EXT);
        return false;
    }

    /* dlopen needs the real path on the machine, which only the filesystem
     * knows: everything above this line is in ReconOS's own namespace. */
    char host[RECON_PATH_MAX];
    char canonical[RECON_PATH_MAX];
    if (!recon_fs_resolve("/", reconos_path, host, sizeof(host),
            canonical, sizeof(canonical))) {
        set_error("%s", recon_fs_last_error());
        return false;
    }

    /*
     * RTLD_NOW so a module with a symbol ReconOS does not provide fails here,
     * at load, rather than the first time that symbol is reached -- which
     * could be halfway through something the user was doing.
     */
    void *handle = dlopen(host, RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        set_error("%s", dlerror());
        remember_failure(canonical, is_app, g_error);
        return false;
    }

    const struct recon_module_descriptor *descriptor =
        dlsym(handle, RECON_MODULE_SYMBOL);
    if (descriptor == NULL) {
        set_error("'%s' does not export " RECON_MODULE_SYMBOL, canonical);
        remember_failure(canonical, is_app, g_error);
        dlclose(handle);
        return false;
    }

    if (descriptor->abi != RECON_MODULE_ABI) {
        /*
         * Refused rather than attempted. Calling into a descriptor whose shape
         * has changed does not fail cleanly; it reads the wrong field as a
         * function pointer.
         */
        set_error("'%s' was built for module interface %u; this is %u",
            descriptor->name != NULL ? descriptor->name : canonical,
            descriptor->abi, (unsigned)RECON_MODULE_ABI);
        remember_failure(canonical, is_app, g_error);
        dlclose(handle);
        return false;
    }

    if (descriptor->payload != RECON_PAYLOAD_NATIVE) {
        set_error("'%s' holds a kind of code this version cannot run yet",
            descriptor->name != NULL ? descriptor->name : canonical);
        remember_failure(canonical, is_app, g_error);
        dlclose(handle);
        return false;
    }

    if (descriptor->name == NULL || descriptor->name[0] == '\0') {
        set_error("'%s' has no name", canonical);
        remember_failure(canonical, is_app, g_error);
        dlclose(handle);
        return false;
    }

    if (module_slot_for(descriptor->name) != NULL) {
        set_error("a module called '%s' is already loaded", descriptor->name);
        dlclose(handle);
        return false;
    }

    struct module_slot *slot = free_module_slot();
    if (slot == NULL) {
        set_error("no room for another module");
        dlclose(handle);
        return false;
    }

    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->handle = handle;
    slot->descriptor = descriptor;
    slot->state.is_app = is_app;
    snprintf(slot->state.name, sizeof(slot->state.name), "%s", descriptor->name);
    snprintf(slot->state.version, sizeof(slot->state.version), "%s",
        descriptor->version != NULL ? descriptor->version : "");
    snprintf(slot->state.description, sizeof(slot->state.description), "%s",
        descriptor->description != NULL ? descriptor->description : "");
    recon_text_copy(slot->state.path, sizeof(slot->state.path), canonical);

    if (descriptor->load != NULL) {
        g_loading = slot->state.name;
        bool ok = descriptor->load();
        g_loading = NULL;

        if (!ok) {
            /*
             * The module declined. Whatever it managed to register before
             * saying so is left behind unless it cleaned up itself, so unload
             * is called to give it the chance.
             */
            if (descriptor->unload != NULL) {
                descriptor->unload();
            }
            recon_text_copy(slot->state.problem, sizeof(slot->state.problem),
                g_error[0] != '\0' ? g_error : "the module declined to load");
            slot->state.loaded = false;
            slot->handle = NULL;
            slot->descriptor = NULL;
            dlclose(handle);
            set_error("'%s' declined to load", slot->state.name);
            return false;
        }
    }

    slot->state.loaded = true;
    wlr_log(WLR_INFO, "ReconOS: loaded %s '%s'%s%s",
        is_app ? "application" : "module", slot->state.name,
        slot->state.version[0] != '\0' ? " " : "", slot->state.version);
    return true;
}

bool recon_modules_unload(const char *name) {
    struct module_slot *slot = module_slot_for(name);
    if (slot == NULL) {
        set_error("no module called '%s' is loaded", name);
        return false;
    }
    if (!slot->state.loaded) {
        /* It never loaded; forgetting the record of the failure is all there
         * is to do. */
        memset(slot, 0, sizeof(*slot));
        return true;
    }

    if (slot->descriptor != NULL && slot->descriptor->unload != NULL) {
        slot->descriptor->unload();
    }

    /*
     * Anything the module registered and did not remove is still pointing at
     * code that is about to disappear. Refuse to close the library rather than
     * leave a dangling function pointer somewhere it will be called.
     */
    for (int i = 0; i < APPS_MAX; i++) {
        if (g_apps[i].used &&
                strcmp(g_apps[i].info.module, slot->state.name) == 0) {
            set_error("'%s' left '%s' registered; it cannot be unloaded",
                slot->state.name, g_apps[i].info.name);
            return false;
        }
    }

    void *handle = slot->handle;
    char unloaded[64];
    snprintf(unloaded, sizeof(unloaded), "%s", slot->state.name);

    memset(slot, 0, sizeof(*slot));
    if (handle != NULL) {
        dlclose(handle);
    }

    wlr_log(WLR_INFO, "ReconOS: unloaded '%s'", unloaded);
    return true;
}

/* Load every module in one directory. Returns how many loaded. */
static int load_directory(const char *directory, const char *extension) {
    struct recon_dirent entries[64];
    int count = recon_fs_list("/", directory, entries, 64);
    if (count < 0) {
        return 0;
    }
    if (count > 64) {
        count = 64;
    }

    int loaded = 0;
    for (int i = 0; i < count; i++) {
        if (entries[i].kind == RECON_FILE_DIRECTORY ||
                !ends_with(entries[i].name, extension)) {
            continue;
        }

        char path[RECON_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", directory, entries[i].name);

        /*
         * One failing does not stop the rest. A broken file in /Apps should
         * cost the user that application, not their desktop.
         */
        if (recon_modules_load(path)) {
            loaded++;
        } else {
            wlr_log(WLR_ERROR, "ReconOS: %s: %s", path, recon_modules_last_error());
        }
    }
    return loaded;
}

/* Copy a host file into the ReconOS filesystem. */
/*
 * Is the shipped copy different from the one already installed?
 *
 * Byte for byte, not by size or timestamp. A module rebuilt from unchanged
 * source is the same length and a different file; one rebuilt after an
 * interface change is often the same length too. Timestamps travel badly --
 * a tarball, a copy, a clock that went backwards -- and the question being
 * asked is not "which is newer" but "is this the one I shipped".
 *
 * True when they differ or when either cannot be read, because a file that
 * cannot be compared should be replaced rather than trusted.
 */
static bool files_differ(const char *host_path, const char *reconos_path) {
    char installed_host[RECON_PATH_MAX];
    if (!recon_fs_resolve(NULL, reconos_path, installed_host,
            sizeof(installed_host), NULL, 0)) {
        return true;
    }

    FILE *a = fopen(host_path, "rb");
    if (a == NULL) {
        return true;
    }
    FILE *b = fopen(installed_host, "rb");
    if (b == NULL) {
        fclose(a);
        return true;
    }

    bool differ = false;
    for (;;) {
        char left[4096];
        char right[4096];

        size_t got_left = fread(left, 1, sizeof(left), a);
        size_t got_right = fread(right, 1, sizeof(right), b);

        if (got_left != got_right || memcmp(left, right, got_left) != 0) {
            differ = true;
            break;
        }
        if (got_left == 0) {
            break;      /* Both ended together with everything matching. */
        }
    }

    fclose(a);
    fclose(b);
    return differ;
}

static bool install_file(const char *host_path, const char *reconos_path) {
    FILE *in = fopen(host_path, "rb");
    if (in == NULL) {
        return false;
    }

    char host_target[RECON_PATH_MAX];
    char canonical[RECON_PATH_MAX];
    if (!recon_fs_resolve("/", reconos_path, host_target, sizeof(host_target),
            canonical, sizeof(canonical))) {
        fclose(in);
        return false;
    }

    FILE *out = fopen(host_target, "wb");
    if (out == NULL) {
        fclose(in);
        return false;
    }

    char buffer[16384];
    size_t got;
    bool ok = true;
    while ((got = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, got, out) != got) {
            ok = false;
            break;
        }
    }

    fclose(in);
    if (fclose(out) != 0) {
        ok = false;
    }

    if (!ok) {
        /* A half-copied module would fail to load in a way nobody could read.
         * Better that it is simply absent. */
        recon_fs_remove("/", canonical);
        return false;
    }

    /* Executable, because it is code that gets loaded. */
    chmod(host_target, 0755);
    return true;
}

int recon_modules_install_shipped(void) {
#ifndef RECONOS_MODULE_DIR
    return 0;
#else
    const char *source = getenv("RECONOS_MODULE_DIR");
    if (source == NULL || *source == '\0') {
        source = RECONOS_MODULE_DIR;
    }

    DIR *dir = opendir(source);
    if (dir == NULL) {
        return 0;
    }

    int installed = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        bool is_app = ends_with(entry->d_name, RECON_APP_EXT);
        if (!is_app && !ends_with(entry->d_name, RECON_MODULE_EXT)) {
            continue;
        }

        char target[RECON_PATH_MAX];
        snprintf(target, sizeof(target), "%s/%s",
            is_app ? RECON_DIR_APPS : RECON_DIR_MODULES, entry->d_name);

        char host[RECON_PATH_MAX * 2];
        snprintf(host, sizeof(host), "%s/%s", source, entry->d_name);

        /*
         * A shipped module is replaced when the copy on disk is different.
         *
         * This used to skip anything already there, on the grounds that an
         * install which quietly replaces what is there undoes people's
         * choices. That reasoning is right about a program somebody
         * installed and wrong about the ones ReconOS ships beside itself --
         * and it caused exactly the fault it was meant to prevent, one step
         * removed: an old Calculator stayed on disk across an update that
         * changed the interface it was built against, and opening it
         * segfaulted the system.
         *
         * The system's own components are the system's to replace. A module
         * somebody wrote has a name of its own and is not touched by this
         * loop at all, because this loop only walks what ReconOS shipped.
         */
        if (recon_fs_exists("/", target) && !files_differ(host, target)) {
            continue;
        }

        if (install_file(host, target)) {
            wlr_log(WLR_INFO, "ReconOS: installed %s", target);
            installed++;
        } else {
            wlr_log(WLR_ERROR, "ReconOS: could not install %s", target);
        }
    }
    closedir(dir);
    return installed;
#endif
}

/* --- Installing --- */

bool recon_modules_path_of(const char *name, char *out, size_t size) {
    struct module_slot *slot = module_slot_for(name);
    if (slot == NULL || out == NULL || size == 0) {
        return false;
    }
    snprintf(out, size, "%s", slot->state.path);
    return true;
}

bool recon_modules_install(const char *reconos_path) {
    if (reconos_path == NULL || *reconos_path == '\0') {
        set_error("nothing to install");
        return false;
    }

    /*
     * An administrator's decision. A module runs inside ReconOS with
     * everything ReconOS can do -- it is closer to installing a driver than
     * to saving a file, and the account rules should say so.
     */
    if (!recon_users_may_administer()) {
        set_error("only an administrator can install a program");
        return false;
    }

    bool is_app = ends_with(reconos_path, RECON_APP_EXT);
    if (!is_app && !ends_with(reconos_path, RECON_MODULE_EXT)) {
        set_error("a program is a " RECON_APP_EXT " or a " RECON_MODULE_EXT);
        return false;
    }

    struct recon_dirent info;
    if (!recon_fs_stat("/", reconos_path, &info) ||
            info.kind == RECON_FILE_DIRECTORY) {
        set_error("there is no file at '%s'", reconos_path);
        return false;
    }

    /* The name it will have where it is going. */
    const char *leaf = strrchr(reconos_path, '/');
    leaf = (leaf != NULL) ? leaf + 1 : reconos_path;

    char target[RECON_PATH_MAX];
    snprintf(target, sizeof(target), "%s/%s",
        is_app ? RECON_DIR_APPS : RECON_DIR_MODULES, leaf);

    /*
     * Installing over itself is not an install, it is a load. Without this,
     * installing something already in /Apps would copy a file onto itself and
     * could truncate it before reading it.
     */
    if (strcmp(target, reconos_path) == 0) {
        return recon_modules_load(target);
    }

    if (recon_fs_exists("/", target)) {
        set_error("'%s' is already installed; remove it first", leaf);
        return false;
    }

    /*
     * Copied rather than pointed at. An application installed from somebody's
     * Downloads folder must not stop working when they tidy up.
     */
    size_t size = 0;
    char *bytes = recon_fs_read("/", reconos_path, &size);
    if (bytes == NULL) {
        set_error("%s", recon_fs_last_error());
        return false;
    }

    bool written = recon_fs_write("/", target, bytes, size);
    free(bytes);

    if (!written) {
        set_error("%s", recon_fs_last_error());
        return false;
    }

    /* Code has to be executable to be loaded. */
    char host[RECON_PATH_MAX];
    char canonical[RECON_PATH_MAX];
    if (recon_fs_resolve("/", target, host, sizeof(host), canonical,
            sizeof(canonical))) {
        chmod(host, 0755);
    }

    if (!recon_modules_load(target)) {
        /*
         * It copied and would not load, so the copy is removed again. Leaving
         * a file that fails on every start would turn one bad install into a
         * permanent error message.
         */
        recon_fs_remove("/", target);
        return false;
    }

    wlr_log(WLR_INFO, "ReconOS: installed %s", target);
    return true;
}

bool recon_modules_uninstall(const char *name) {
    if (!recon_users_may_administer()) {
        set_error("only an administrator can remove a program");
        return false;
    }

    char path[RECON_PATH_MAX];
    if (!recon_modules_path_of(name, path, sizeof(path)) || path[0] == '\0') {
        set_error("nothing installed is called '%s'",
            name != NULL ? name : "");
        return false;
    }

    /*
     * Unloaded first. Deleting the file underneath running code turns a
     * tidy-up into a crash on the next start, and unload already refuses when
     * something the module registered is still in use.
     */
    if (!recon_modules_unload(name)) {
        return false;
    }

    if (!recon_fs_remove("/", path)) {
        set_error("unloaded, but the file stayed: %s", recon_fs_last_error());
        return false;
    }

    wlr_log(WLR_INFO, "ReconOS: removed %s", path);
    return true;
}

int recon_modules_load_all(void) {
    int loaded = 0;
    loaded += load_directory(RECON_DIR_MODULES, RECON_MODULE_EXT);
    loaded += load_directory(RECON_DIR_APPS, RECON_APP_EXT);
    return loaded;
}

int recon_modules_count(void) {
    int count = 0;
    for (int i = 0; i < MODULES_MAX; i++) {
        if (g_modules[i].used) {
            count++;
        }
    }
    return count;
}

bool recon_modules_at(int index, struct recon_module_state *out) {
    int seen = 0;
    for (int i = 0; i < MODULES_MAX; i++) {
        if (!g_modules[i].used) {
            continue;
        }
        if (seen == index) {
            if (out != NULL) {
                *out = g_modules[i].state;
            }
            return true;
        }
        seen++;
    }
    return false;
}

void recon_modules_finish(void) {
    /*
     * Unloaded in reverse, so a module that registered something another one
     * depends on goes last. Nothing declares dependencies yet, but load order
     * is the only ordering there is, and reversing it is the cheapest thing
     * that will still be right when they do.
     */
    for (int i = MODULES_MAX - 1; i >= 0; i--) {
        if (!g_modules[i].used || !g_modules[i].state.loaded) {
            continue;
        }
        if (g_modules[i].descriptor != NULL &&
                g_modules[i].descriptor->unload != NULL) {
            g_modules[i].descriptor->unload();
        }
        if (g_modules[i].handle != NULL) {
            dlclose(g_modules[i].handle);
        }
        memset(&g_modules[i], 0, sizeof(g_modules[i]));
    }

    memset(g_apps, 0, sizeof(g_apps));
    g_command_count = 0;
    g_server = NULL;
    g_font = NULL;
}
