/*
 * The ReconOS module interface.
 *
 * This is the header a module author includes, and nothing else. Everything
 * here is the contract between ReconOS and something loaded into it.
 *
 * A module is a file ReconOS loads at runtime to add to the system, so a
 * feature can arrive without the core being rebuilt. There are two kinds, and
 * the extension says which:
 *
 *   .rts    Recon Tower System -- a subsystem, a driver, a service. Loaded
 *           when the system starts. Adds capability rather than a window.
 *   .rex    Recon Executable -- an application. Loaded the same way, but it
 *           registers something the user can open, so it appears in the Apps
 *           menu.
 *
 * The two are the same format and the same loader. The distinction is about
 * when a thing loads and whether a person is meant to launch it, which is real
 * information rather than decoration -- the same split Windows draws between
 * .sys and .exe, and macOS between .kext and .app.
 *
 * The extension names *this contract*, not the machine code inside it. That is
 * deliberate: a native build and, later, a wrapped foreign binary can both
 * present this interface, and ReconOS should not have to care which it got.
 * `payload` says what the code actually is. Today only native is understood.
 *
 * A module is written like this:
 *
 *     #include "recon_module.h"
 *
 *     static bool load(void) {
 *         return recon_register_app(&(struct recon_app_registration){
 *             .name = "Calculator",
 *             .icon = "calculator",
 *             .create = my_create,
 *         });
 *     }
 *
 *     static void unload(void) {
 *         recon_unregister_app("Calculator");
 *     }
 *
 *     RECON_MODULE(
 *         .name = "Calculator",
 *         .version = "1.0",
 *         .description = "Arithmetic by mouse or keyboard",
 *         .load = load,
 *         .unload = unload,
 *     );
 */

#ifndef RECON_MODULE_H
#define RECON_MODULE_H

#include <stdbool.h>
#include <stdint.h>

struct recon_server;
struct recon_font;
struct recon_appwin;

/*
 * The interface version.
 *
 * Bumped whenever anything below changes shape -- or anything *reachable*
 * from below, which is the part that was missed. A module built against a
 * different number is refused rather than loaded, because the alternative is
 * calling through a function pointer that means something else now, which
 * does not fail, it corrupts.
 *
 * 2 (v0.2.16): `struct recon_appwin_impl` gained a `help` field in v0.2.15
 * and this number was not bumped. A module built before that has a smaller
 * struct, so reading the new field read past the end of it -- and ReconOS
 * segfaulted the moment somebody opened the Calculator. The gate existed
 * precisely for this and was standing open because nobody had turned the
 * number.
 *
 * 3 (v0.3.0): `struct recon_app_registration` gained `version`, so an applet
 * can be updated on its own. Bumped here rather than in a later panic: a
 * module built against 2 has a four-field registration struct, and reading a
 * fifth field out of it would read whatever follows it in memory and then
 * compare that as a version string.
 *
 * The rule this is here to enforce, stated so it is harder to miss: **any
 * change to a struct a module can hold or pass is an ABI change**, including
 * adding a field at the end, and including structs declared in other headers
 * that this interface exposes.
 */
#define RECON_MODULE_ABI 3

/* The symbol a module must export. Looked up by name after loading. */
#define RECON_MODULE_SYMBOL "recon_module_descriptor"

/*
 * The extensions. Deliberately unalike rather than a tidy pair differing by
 * one letter: extensions a letter apart get mistyped and misread, and these
 * two mean different things about when a file loads.
 */
#define RECON_MODULE_EXT ".rts"
#define RECON_APP_EXT ".rex"

/* What kind of machine code the file holds. */
enum recon_module_payload {
    /* An ELF shared object built for this machine. What is emitted today. */
    RECON_PAYLOAD_NATIVE = 0,
    /*
     * Reserved. A Windows DLL, through a translation layer. Named here so the
     * loader has somewhere to refuse it politely rather than mistaking it for
     * a native object and failing in a way nobody can read.
     */
    RECON_PAYLOAD_PE = 1,
};

/* --- Registering an application --- */

/*
 * An application a module contributes.
 *
 * `create` is called the first time the user opens it, not at load: a module
 * that builds its window at load time costs memory for something nobody has
 * asked for, which is exactly the idle weight ReconOS exists to avoid.
 */
struct recon_app_registration {
    const char *name;
    /* Icon name, looked up in /System/Icons. May be NULL. */
    const char *icon;

    struct recon_appwin *(*create)(struct recon_server *server,
        struct recon_font *font);

    /* False for something launched another way -- by a file association, or
     * by another module -- that should not clutter the menu. */
    bool in_menu;

    /*
     * Which release of *this applet* it is: "1.4.0", "0.3.1".
     *
     * This is how an applet is updated without updating ReconOS. Register an
     * application whose name is already taken and whose version is higher, and
     * it takes over the name; the one it displaced is remembered and comes
     * back if this module is removed. A version equal or lower is refused, and
     * so is one that will not parse.
     *
     * The applications built into ReconOS carry the system's version, so a
     * replacement has to claim to be newer than the system that shipped the
     * thing it is replacing. That has a consequence worth knowing about before
     * it surprises somebody: when ReconOS itself is updated past an installed
     * applet, the built-in wins again and the installed one steps aside. That
     * is the intended behaviour -- an applet update is superseded by the
     * release that catches up with it -- rather than an accident.
     *
     * NULL means "no opinion", which registers only if the name is free. An
     * applet with no version cannot displace anything, which is the right
     * default for something that has not thought about it.
     */
    const char *version;
};

/*
 * Add an application to the system. Called from a module's load().
 *
 * False if the name is already taken or there is no room. A module that
 * cannot register what it exists to register should return false from load(),
 * so it is unloaded rather than left half-present.
 */
bool recon_register_app(const struct recon_app_registration *app);

/*
 * Remove one. Called from unload(). Refused while the application has a window
 * open, because unloading the code underneath a live window is a crash with
 * extra steps -- check recon_app_in_use() first if that matters to you.
 */
bool recon_unregister_app(const char *name);

/* Whether an application currently has a window that exists. */
bool recon_app_in_use(const char *name);

/* --- Registering a command --- */

/*
 * A command a module adds to the ReconOS interpreter, reachable from the
 * terminal and the control socket.
 *
 * `run` writes its output with recon_command_print. Returning false marks the
 * command as having failed, which is separate from having printed an error.
 */
struct recon_command_registration {
    const char *name;
    const char *usage;
    const char *summary;
    bool (*run)(void *session, int argc, char **argv);
};

bool recon_register_command(const struct recon_command_registration *command);
bool recon_unregister_command(const char *name);

/* Print from inside a command's run(). */
void recon_command_print(void *session, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* --- The module itself --- */

/*
 * What a module exports. Everything except `abi` and `name` may be zero.
 *
 * load() returning false means the module declines to be loaded -- a missing
 * dependency, a machine it cannot work on. It is then unloaded cleanly and
 * reported, rather than sitting there half-alive.
 */
struct recon_module_descriptor {
    uint32_t abi;
    enum recon_module_payload payload;

    const char *name;
    const char *version;
    const char *description;

    bool (*load)(void);
    void (*unload)(void);
};

/*
 * Declare the module. Use once, at file scope.
 *
 * Fills in abi and payload so a module author cannot forget them and produce
 * something that loads as the wrong version.
 */
#define RECON_MODULE(...)                                          \
    __attribute__((visibility("default")))                         \
    const struct recon_module_descriptor recon_module_descriptor = { \
        .abi = RECON_MODULE_ABI,                                   \
        .payload = RECON_PAYLOAD_NATIVE,                           \
        __VA_ARGS__                                                \
    }

/* --- What ReconOS gives a module --- */

/*
 * The running system, for a module that needs it. Valid from load() until
 * unload() returns.
 */
struct recon_server *recon_module_server(void);
struct recon_font *recon_module_font(void);

#endif
