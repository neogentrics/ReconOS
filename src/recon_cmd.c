/*
 * The ReconOS command interpreter. See include/recon_cmd.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */

#include "recon_appwin.h"
#include "recon_cmd.h"
#include "recon_fs.h"
#include "recon_procinfo.h"
#include "recon_server.h"
#include "recon_shell.h"

#include <linux/input-event-codes.h> /* BTN_LEFT, BTN_RIGHT */
#include <xkbcommon/xkbcommon.h>

#define OUTPUT_MAX 16384
#define MAX_ARGS 16
#define LIST_MAX 512

struct recon_cmd_session {
    struct recon_server *server;
    char cwd[RECON_PATH_MAX];
    bool should_exit;

    char output[OUTPUT_MAX];
    size_t output_used;
};

/* --- Output --- */

static void out(struct recon_cmd_session *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void out(struct recon_cmd_session *s, const char *fmt, ...) {
    if (s->output_used >= sizeof(s->output) - 1) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(s->output + s->output_used,
        sizeof(s->output) - s->output_used, fmt, args);
    va_end(args);

    if (written > 0) {
        s->output_used += (size_t)written;
        if (s->output_used >= sizeof(s->output)) {
            s->output_used = sizeof(s->output) - 1;
        }
    }
}

/* --- Commands --- */

typedef void (*command_fn)(struct recon_cmd_session *s, int argc, char **argv);

struct command {
    const char *name;
    const char *usage;
    const char *summary;
    command_fn run;
};

static const struct command COMMANDS[];
static int command_count(void);

static void cmd_help(struct recon_cmd_session *s, int argc, char **argv) {
    if (argc > 1) {
        for (int i = 0; i < command_count(); i++) {
            if (strcasecmp(COMMANDS[i].name, argv[1]) == 0) {
                out(s, "%s\n  %s\n  %s\n",
                    COMMANDS[i].name, COMMANDS[i].usage, COMMANDS[i].summary);
                return;
            }
        }
        out(s, "No command named '%s'.\n", argv[1]);
        return;
    }

    out(s, "ReconOS commands. 'help <command>' for detail.\n\n");
    for (int i = 0; i < command_count(); i++) {
        out(s, "  %-10s %s\n", COMMANDS[i].name, COMMANDS[i].summary);
    }
}

static void cmd_ver(struct recon_cmd_session *s, int argc, char **argv) {
    (void)argc; (void)argv;
    out(s, "ReconOS 0.0.7\n");
    out(s, "Root: %s\n", recon_fs_host_root());
}

static void cmd_dir(struct recon_cmd_session *s, int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : ".";

    struct recon_dirent entries[LIST_MAX];
    int count = recon_fs_list(s->cwd, path, entries, LIST_MAX);
    if (count < 0) {
        out(s, "%s\n", recon_fs_last_error());
        return;
    }

    int shown = count < LIST_MAX ? count : LIST_MAX;
    int dirs = 0;
    size_t bytes = 0;

    for (int i = 0; i < shown; i++) {
        if (entries[i].kind == RECON_FILE_DIRECTORY) {
            out(s, "  <DIR>  %s\n", entries[i].name);
            dirs++;
        } else {
            out(s, "  %6zu  %s\n", entries[i].size, entries[i].name);
            bytes += entries[i].size;
        }
    }

    if (count == 0) {
        out(s, "  (empty)\n");
    } else {
        out(s, "\n  %d director%s, %d file%s, %zu bytes\n",
            dirs, dirs == 1 ? "y" : "ies",
            shown - dirs, (shown - dirs) == 1 ? "" : "s", bytes);
    }
    if (count > shown) {
        out(s, "  (%d more not shown)\n", count - shown);
    }
}

static void cmd_cd(struct recon_cmd_session *s, int argc, char **argv) {
    if (argc < 2) {
        out(s, "%s\n", s->cwd);
        return;
    }

    char host[RECON_PATH_MAX];
    char canonical[RECON_PATH_MAX];
    if (!recon_fs_resolve(s->cwd, argv[1], host, sizeof(host),
            canonical, sizeof(canonical))) {
        out(s, "%s\n", recon_fs_last_error());
        return;
    }

    struct recon_dirent info;
    if (!recon_fs_stat(s->cwd, argv[1], &info)) {
        out(s, "%s\n", recon_fs_last_error());
        return;
    }
    if (info.kind != RECON_FILE_DIRECTORY) {
        out(s, "'%s' is not a directory.\n", canonical);
        return;
    }

    snprintf(s->cwd, sizeof(s->cwd), "%s", canonical);
}

static void cmd_type(struct recon_cmd_session *s, int argc, char **argv) {
    if (argc < 2) {
        out(s, "Usage: type <file>\n");
        return;
    }

    size_t size = 0;
    char *data = recon_fs_read(s->cwd, argv[1], &size);
    if (data == NULL) {
        out(s, "%s\n", recon_fs_last_error());
        return;
    }
    out(s, "%s", data);
    if (size > 0 && data[size - 1] != '\n') {
        out(s, "\n");
    }
    free(data);
}

static void cmd_write(struct recon_cmd_session *s, int argc, char **argv) {
    if (argc < 2) {
        out(s, "Usage: write <file> [text...]\n");
        return;
    }

    char text[OUTPUT_MAX];
    size_t used = 0;
    for (int i = 2; i < argc; i++) {
        int written = snprintf(text + used, sizeof(text) - used, "%s%s",
            i > 2 ? " " : "", argv[i]);
        if (written < 0) {
            break;
        }
        used += (size_t)written;
    }
    if (used < sizeof(text) - 1) {
        text[used++] = '\n';
    }

    if (!recon_fs_write(s->cwd, argv[1], text, used)) {
        out(s, "%s\n", recon_fs_last_error());
        return;
    }
    out(s, "Wrote %zu bytes.\n", used);
}

static void cmd_mkdir(struct recon_cmd_session *s, int argc, char **argv) {
    if (argc < 2) {
        out(s, "Usage: mkdir <name>\n");
        return;
    }
    if (!recon_fs_mkdir(s->cwd, argv[1])) {
        out(s, "%s\n", recon_fs_last_error());
    }
}

static void cmd_del(struct recon_cmd_session *s, int argc, char **argv) {
    if (argc < 2) {
        out(s, "Usage: del <name>\n");
        return;
    }
    if (recon_fs_remove(s->cwd, argv[1])) {
        return;
    }

    /*
     * A folder with things in it is not a failure, it is a bigger question.
     * Say which one it is rather than reporting an error the user cannot act
     * on.
     */
    struct recon_dirent info;
    if (recon_fs_stat(s->cwd, argv[1], &info) &&
            info.kind == RECON_FILE_DIRECTORY &&
            !recon_fs_is_protected(s->cwd, argv[1])) {
        out(s, "'%s' is not empty. Use 'deltree %s' to remove it and its "
            "contents.\n", argv[1], argv[1]);
        return;
    }

    out(s, "%s\n", recon_fs_last_error());
}

static void cmd_deltree(struct recon_cmd_session *s, int argc, char **argv) {
    if (argc < 2) {
        out(s, "Usage: deltree <name>\n");
        return;
    }

    /*
     * Named separately from del rather than being a flag on it. Losing a whole
     * tree should take a different word, not a character somebody might not
     * have meant to type.
     */
    if (!recon_fs_remove_tree(s->cwd, argv[1])) {
        out(s, "%s\n", recon_fs_last_error());
        return;
    }
    out(s, "Removed '%s' and everything in it.\n", argv[1]);
}

static void cmd_rename(struct recon_cmd_session *s, int argc, char **argv) {
    if (argc < 3) {
        out(s, "Usage: rename <name> <new name>\n");
        return;
    }
    if (strchr(argv[2], '/') != NULL) {
        out(s, "A new name cannot contain '/'. Use 'move' to put it "
            "somewhere else.\n");
        return;
    }
    if (!recon_fs_rename(s->cwd, argv[1], argv[2])) {
        out(s, "%s\n", recon_fs_last_error());
        return;
    }
    out(s, "Renamed '%s' to '%s'.\n", argv[1], argv[2]);
}

/* Work out where something lands when the destination is a folder. */
static bool destination_for(struct recon_cmd_session *s, const char *from,
        const char *to, char *out_path, size_t size) {
    struct recon_dirent info;
    if (!recon_fs_stat(s->cwd, to, &info) || info.kind != RECON_FILE_DIRECTORY) {
        snprintf(out_path, size, "%s", to);
        return true;
    }

    /* "move notes.txt /Temp" means into /Temp, keeping the name -- which is
     * what anyone typing it expects. */
    const char *leaf = strrchr(from, '/');
    leaf = (leaf != NULL && leaf[1] != '\0') ? leaf + 1 : from;

    char host[RECON_PATH_MAX];
    char canonical[RECON_PATH_MAX];
    if (!recon_fs_resolve(s->cwd, to, host, sizeof(host),
            canonical, sizeof(canonical))) {
        return false;
    }

    if (strcmp(canonical, "/") == 0) {
        snprintf(out_path, size, "/%s", leaf);
    } else {
        snprintf(out_path, size, "%s/%s", canonical, leaf);
    }
    return true;
}

static void cmd_move(struct recon_cmd_session *s, int argc, char **argv) {
    if (argc < 3) {
        out(s, "Usage: move <name> <destination>\n");
        return;
    }

    char target[RECON_PATH_MAX];
    if (!destination_for(s, argv[1], argv[2], target, sizeof(target))) {
        out(s, "%s\n", recon_fs_last_error());
        return;
    }

    if (!recon_fs_rename(s->cwd, argv[1], target)) {
        out(s, "%s\n", recon_fs_last_error());
        return;
    }
    out(s, "Moved '%s' to '%s'.\n", argv[1], target);
}

static void cmd_copy(struct recon_cmd_session *s, int argc, char **argv) {
    if (argc < 3) {
        out(s, "Usage: copy <name> <destination>\n");
        return;
    }

    char target[RECON_PATH_MAX];
    if (!destination_for(s, argv[1], argv[2], target, sizeof(target))) {
        out(s, "%s\n", recon_fs_last_error());
        return;
    }

    if (!recon_fs_copy(s->cwd, argv[1], target)) {
        out(s, "%s\n", recon_fs_last_error());
        return;
    }
    out(s, "Copied '%s' to '%s'.\n", argv[1], target);
}

/*
 * Drive the desktop as though a person were using it.
 *
 * A desktop cannot be tested by reading it. "The delete button does nothing"
 * is a report that needs the button pressed and the result watched, and this
 * is what presses it. Input goes through the same path real input takes, so a
 * pass here means the thing a user touches works, not that a shortcut around
 * it works.
 */
static void cmd_ui(struct recon_cmd_session *s, int argc, char **argv) {
    if (argc < 2) {
        out(s, "Usage: ui move|click|rclick|press|release|key|type ...\n");
        out(s, "  ui move <x> <y>       put the pointer somewhere\n");
        out(s, "  ui click <x> <y>      move there, then press and release\n");
        out(s, "  ui rclick <x> <y>     the same with the right button\n");
        out(s, "  ui press|release      the left button, without moving\n");
        out(s, "  ui key <name>         one key, e.g. Return, Escape, F2, ctrl+c\n");
        out(s, "  ui type <text>        a character at a time\n");
        out(s, "  ui where              where the pointer is\n");
        return;
    }

    struct recon_server *server = s->server;
    const char *what = argv[1];

    if (strcasecmp(what, "where") == 0) {
        int x = 0, y = 0;
        recon_pointer_position(server, &x, &y);
        out(s, "%d %d\n", x, y);
        return;
    }

    if (strcasecmp(what, "move") == 0 || strcasecmp(what, "click") == 0 ||
            strcasecmp(what, "rclick") == 0) {
        if (argc < 4) {
            out(s, "Usage: ui %s <x> <y>\n", what);
            return;
        }
        int x = atoi(argv[2]);
        int y = atoi(argv[3]);
        recon_inject_pointer(server, x, y);

        if (strcasecmp(what, "move") == 0) {
            out(s, "pointer at %d %d\n", x, y);
            return;
        }

        uint32_t button = (strcasecmp(what, "rclick") == 0) ? BTN_RIGHT : BTN_LEFT;
        recon_inject_button(server, button, true);
        recon_inject_button(server, button, false);
        out(s, "%s at %d %d\n", what, x, y);
        return;
    }

    if (strcasecmp(what, "press") == 0 || strcasecmp(what, "release") == 0) {
        bool pressed = (strcasecmp(what, "press") == 0);
        uint32_t button = (argc > 2 && strcasecmp(argv[2], "right") == 0)
            ? BTN_RIGHT : BTN_LEFT;
        recon_inject_button(server, button, pressed);
        out(s, "%s\n", what);
        return;
    }

    if (strcasecmp(what, "key") == 0) {
        if (argc < 3) {
            out(s, "Usage: ui key <name>\n");
            return;
        }

        /*
         * "ctrl+c" and "shift+Tab" rather than a separate modifier argument:
         * a key press is one thing, and splitting it across two arguments
         * invites sending half of it.
         */
        char spec[128];
        snprintf(spec, sizeof(spec), "%s", argv[2]);

        uint32_t modifiers = 0;
        char *name = spec;
        for (char *plus = strchr(name, '+'); plus != NULL; plus = strchr(name, '+')) {
            *plus = '\0';
            if (strcasecmp(name, "ctrl") == 0) {
                modifiers |= RECON_MOD_CTRL;
            } else if (strcasecmp(name, "shift") == 0) {
                modifiers |= RECON_MOD_SHIFT;
            } else if (strcasecmp(name, "alt") == 0) {
                modifiers |= RECON_MOD_ALT;
            } else {
                out(s, "No modifier called '%s'.\n", name);
                return;
            }
            name = plus + 1;
        }

        xkb_keysym_t sym = xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE);
        if (sym == XKB_KEY_NoSymbol) {
            out(s, "No key called '%s'.\n", name);
            return;
        }

        recon_inject_key(server, sym, modifiers);
        out(s, "key %s\n", argv[2]);
        return;
    }

    if (strcasecmp(what, "app") == 0) {
        struct recon_appwin *win = recon_shell_focused_app(server->shell);
        if (win == NULL) {
            out(s, "No built-in window has focus.\n");
            return;
        }
        char buffer[2048];
        buffer[0] = '\0';
        recon_appwin_describe(win, buffer, sizeof(buffer));
        out(s, "%s:\n%s", recon_appwin_title(win),
            buffer[0] != '\0' ? buffer : "  (says nothing about itself)\n");
        return;
    }

    if (strcasecmp(what, "hits") == 0) {
        struct recon_appwin *win = recon_shell_focused_app(server->shell);
        if (win == NULL) {
            out(s, "No built-in window has focus.\n");
            return;
        }
        char buffer[3072];
        buffer[0] = '\0';
        recon_appwin_describe_hits(win, buffer, sizeof(buffer));
        out(s, "%s clickable regions:\n%s", recon_appwin_title(win), buffer);
        return;
    }

    if (strcasecmp(what, "hit") == 0) {
        if (argc < 3) {
            out(s, "Usage: ui hit <region id>\n");
            return;
        }
        struct recon_appwin *win = recon_shell_focused_app(server->shell);
        if (win == NULL) {
            out(s, "No built-in window has focus.\n");
            return;
        }

        uint32_t id = (uint32_t)strtoul(argv[2], NULL, 10);
        int x = 0, y = 0;
        if (!recon_appwin_hit_centre(win, id, &x, &y)) {
            out(s, "%s has no region with id %u.\n", recon_appwin_title(win), id);
            return;
        }

        recon_inject_pointer(server, x, y);
        recon_inject_button(server, BTN_LEFT, true);
        recon_inject_button(server, BTN_LEFT, false);
        out(s, "clicked region %u at %d,%d\n", id, x, y);
        return;
    }

    if (strcasecmp(what, "answer") == 0) {
        if (argc < 3) {
            out(s, "Usage: ui answer <button label>\n");
            return;
        }

        char label[64];
        size_t used = 0;
        for (int i = 2; i < argc && used < sizeof(label) - 1; i++) {
            int w = snprintf(label + used, sizeof(label) - used, "%s%s",
                i > 2 ? " " : "", argv[i]);
            if (w < 0) {
                break;
            }
            used += (size_t)w;
        }

        int x = 0, y = 0;
        if (!recon_shell_dialog_button_at(server->shell, label, &x, &y)) {
            out(s, "No dialog button called '%s' is showing.\n", label);
            return;
        }

        recon_inject_pointer(server, x, y);
        recon_inject_button(server, BTN_LEFT, true);
        recon_inject_button(server, BTN_LEFT, false);
        out(s, "answered '%s' at %d,%d\n", label, x, y);
        return;
    }

    if (strcasecmp(what, "menu") == 0) {
        if (argc < 3) {
            out(s, "Usage: ui menu <label>\n");
            return;
        }

        /* Join the rest, so "ui menu New Folder" works without quoting. */
        char label[64];
        size_t used = 0;
        for (int i = 2; i < argc && used < sizeof(label) - 1; i++) {
            int w = snprintf(label + used, sizeof(label) - used, "%s%s",
                i > 2 ? " " : "", argv[i]);
            if (w < 0) {
                break;
            }
            used += (size_t)w;
        }

        int x = 0, y = 0;
        if (!recon_shell_context_entry_at(server->shell, label, &x, &y)) {
            out(s, "No menu entry called '%s' is showing.\n", label);
            return;
        }

        recon_inject_pointer(server, x, y);
        recon_inject_button(server, BTN_LEFT, true);
        recon_inject_button(server, BTN_LEFT, false);
        out(s, "chose '%s' at %d,%d\n", label, x, y);
        return;
    }

    if (strcasecmp(what, "type") == 0) {
        if (argc < 3) {
            out(s, "Usage: ui type <text>\n");
            return;
        }

        int typed = 0;
        for (int i = 2; i < argc; i++) {
            if (i > 2) {
                recon_inject_key(server, XKB_KEY_space, 0);
                typed++;
            }
            for (const char *c = argv[i]; *c != '\0'; c++) {
                /* Only printable ASCII: anything else is not something a
                 * keyboard would have produced here. */
                if ((unsigned char)*c < 0x20 || (unsigned char)*c > 0x7E) {
                    continue;
                }
                recon_inject_key(server, (uint32_t)*c, 0);
                typed++;
            }
        }
        out(s, "typed %d character%s\n", typed, typed == 1 ? "" : "s");
        return;
    }

    out(s, "No 'ui' action called '%s'.\n", what);
}

/* What the shell currently has open, so a failure can be looked at rather
 * than guessed at. */
static void cmd_state(struct recon_cmd_session *s, int argc, char **argv) {
    (void)argc; (void)argv;
    char buffer[4096];
    buffer[0] = '\0';
    recon_shell_describe(s->server->shell, buffer, sizeof(buffer));
    out(s, "%s", buffer);
}

static void cmd_windows(struct recon_cmd_session *s, int argc, char **argv) {
    (void)argc; (void)argv;

    struct recon_shell *shell = s->server->shell;
    int count = 0;

    int builtin = recon_shell_app_count(shell);
    for (int i = 0; i < builtin; i++) {
        struct recon_appwin *win = recon_shell_app_at(shell, i);
        if (!recon_appwin_is_open(win)) {
            continue;
        }
        out(s, "  %-28s %-10s ReconOS\n", recon_appwin_title(win),
            recon_appwin_is_minimized(win) ? "minimized" : "running");
        count++;
    }

    struct recon_toplevel *toplevel;
    wl_list_for_each(toplevel, &s->server->toplevels, link) {
        out(s, "  %-28s %-10s client (pid %d)\n",
            recon_toplevel_title(toplevel),
            recon_toplevel_is_minimized(toplevel) ? "minimized" : "running",
            recon_toplevel_pid(toplevel));
        count++;
    }

    if (count == 0) {
        out(s, "  (no windows open)\n");
    }
}

static void cmd_apps(struct recon_cmd_session *s, int argc, char **argv) {
    struct recon_shell *shell = s->server->shell;
    int count = recon_shell_app_count(shell);

    if (argc < 2) {
        out(s, "Built-in applications:\n");
        for (int i = 0; i < count; i++) {
            struct recon_appwin *win = recon_shell_app_at(shell, i);
            out(s, "  %d  %-24s %s\n", i, recon_appwin_title(win),
                recon_appwin_is_open(win) ? "open" : "closed");
        }
        out(s, "\n'apps <number>' opens one.\n");
        return;
    }

    int index = atoi(argv[1]);
    if (index < 0 || index >= count) {
        out(s, "No application numbered %d.\n", index);
        return;
    }
    recon_shell_open_app(shell, index);
    out(s, "Opened %s.\n", recon_appwin_title(recon_shell_app_at(shell, index)));
}

static void cmd_mem(struct recon_cmd_session *s, int argc, char **argv) {
    (void)argc; (void)argv;

    struct recon_proc_snapshot *snapshot = recon_proc_snapshot_create();
    if (snapshot == NULL || !recon_proc_snapshot_refresh(snapshot)) {
        out(s, "Cannot read memory information.\n");
        recon_proc_snapshot_destroy(snapshot);
        return;
    }

    size_t total = recon_proc_total_memory_kb(snapshot);
    size_t used = recon_proc_used_memory_kb(snapshot);
    out(s, "Memory: %zu MB used of %zu MB (%zu MB free)\n",
        used / 1024, total / 1024, (total - used) / 1024);
    out(s, "Processes: %zu\n", recon_proc_count(snapshot));

    recon_proc_snapshot_destroy(snapshot);
}

static void cmd_echo(struct recon_cmd_session *s, int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        out(s, "%s%s", i > 1 ? " " : "", argv[i]);
    }
    out(s, "\n");
}

static void cmd_exit(struct recon_cmd_session *s, int argc, char **argv) {
    (void)argc; (void)argv;
    s->should_exit = true;
}

static void cmd_shutdown(struct recon_cmd_session *s, int argc, char **argv) {
    (void)argc; (void)argv;
    out(s, "Shutting down ReconOS.\n");
    recon_quit(s->server);
}

static const struct command COMMANDS[] = {
    { "help",     "help [command]",        "List commands, or explain one",     cmd_help },
    { "ver",      "ver",                   "Show the ReconOS version",          cmd_ver },
    { "dir",      "dir [path]",            "List a directory",                  cmd_dir },
    { "cd",       "cd [path]",             "Change or show the directory",      cmd_cd },
    { "type",     "type <file>",           "Show a file's contents",            cmd_type },
    { "write",    "write <file> [text]",   "Write text to a file",              cmd_write },
    { "mkdir",    "mkdir <name>",          "Create a directory",                cmd_mkdir },
    { "del",      "del <name>",            "Delete a file or empty directory",  cmd_del },
    { "deltree",  "deltree <name>",        "Delete a folder and its contents",  cmd_deltree },
    { "rename",   "rename <name> <new>",   "Rename a file or folder",           cmd_rename },
    { "move",     "move <name> <dest>",    "Move a file or folder",             cmd_move },
    { "copy",     "copy <name> <dest>",    "Copy a file or folder",             cmd_copy },
    { "windows",  "windows",               "List open windows",                 cmd_windows },
    { "apps",     "apps [number]",         "List or open built-in applications", cmd_apps },
    { "mem",      "mem",                   "Show memory in use",                cmd_mem },
    { "ui",       "ui <action> ...",       "Drive the desktop, for testing",    cmd_ui },
    { "state",    "state",                 "What the shell has open",           cmd_state },
    { "echo",     "echo <text>",           "Print text",                        cmd_echo },
    { "exit",     "exit",                  "End this session",                  cmd_exit },
    { "shutdown", "shutdown",              "Shut ReconOS down",                 cmd_shutdown },
};

static int command_count(void) {
    return (int)(sizeof(COMMANDS) / sizeof(COMMANDS[0]));
}

int recon_cmd_names(const char *const **names_out) {
    static const char *names[sizeof(COMMANDS) / sizeof(COMMANDS[0])];
    for (int i = 0; i < command_count(); i++) {
        names[i] = COMMANDS[i].name;
    }
    *names_out = names;
    return command_count();
}

/* --- Session --- */

struct recon_cmd_session *recon_cmd_session_create(struct recon_server *server) {
    struct recon_cmd_session *session = calloc(1, sizeof(*session));
    if (session == NULL) {
        return NULL;
    }
    session->server = server;
    snprintf(session->cwd, sizeof(session->cwd), "/");
    return session;
}

void recon_cmd_session_destroy(struct recon_cmd_session *session) {
    free(session);
}

const char *recon_cmd_cwd(struct recon_cmd_session *session) {
    return session != NULL ? session->cwd : "/";
}

bool recon_cmd_should_exit(struct recon_cmd_session *session) {
    return session != NULL && session->should_exit;
}

/*
 * Split a line into arguments.
 *
 * Double quotes group words, so a path with a space in it can be given as one
 * argument.
 */
static int tokenize(char *line, char **argv, int max) {
    int argc = 0;
    char *p = line;

    while (*p != '\0' && argc < max) {
        while (*p != '\0' && isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        if (*p == '"') {
            p++;
            argv[argc++] = p;
            while (*p != '\0' && *p != '"') {
                p++;
            }
        } else {
            argv[argc++] = p;
            while (*p != '\0' && !isspace((unsigned char)*p)) {
                p++;
            }
        }

        if (*p != '\0') {
            *p++ = '\0';
        }
    }

    return argc;
}

const char *recon_cmd_run(struct recon_cmd_session *session, const char *line) {
    if (session == NULL) {
        return "";
    }

    session->output[0] = '\0';
    session->output_used = 0;

    if (line == NULL) {
        return session->output;
    }

    char work[1024];
    snprintf(work, sizeof(work), "%s", line);

    char *argv[MAX_ARGS];
    int argc = tokenize(work, argv, MAX_ARGS);
    if (argc == 0) {
        return session->output;
    }

    for (int i = 0; i < command_count(); i++) {
        if (strcasecmp(COMMANDS[i].name, argv[0]) == 0) {
            COMMANDS[i].run(session, argc, argv);
            return session->output;
        }
    }

    out(session, "'%s' is not a ReconOS command. Type 'help' for a list.\n", argv[0]);
    return session->output;
}
