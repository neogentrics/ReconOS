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
#include <unistd.h> /* access */
#include <strings.h> /* strcasecmp */

#include "ReconOS.h"
#include "recon_appwin.h"
#include "recon_capture.h"
#include "recon_cmd.h"
#include "recon_control.h"
#include "recon_error.h"
#include "recon_firewall.h"
#include "recon_service.h"
#include "recon_fs.h"
#include "recon_modules.h"
#include "recon_package.h"
#include "recon_net.h"
#include "recon_procinfo.h"
#include "recon_registry.h"
#include "recon_session.h"
#include "recon_users.h"
#include "recon_wallpaper.h"
#include "recon_access.h"
#include "recon_theme.h"
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

/*
 * Printing from a module's command.
 *
 * A module is given the session as an opaque pointer and this to write with,
 * so it never has to know how output is buffered -- which is the sort of
 * detail that would otherwise become part of the interface and be impossible
 * to change afterwards.
 */
void recon_command_print(void *session, const char *fmt, ...) {
    struct recon_cmd_session *s = session;
    if (s == NULL || s->output_used >= sizeof(s->output) - 1) {
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
        int extra = recon_module_command_count();
        for (int i = 0; i < extra; i++) {
            struct recon_command_registration command;
            if (recon_module_command_at(i, &command) &&
                    strcasecmp(command.name, argv[1]) == 0) {
                out(s, "%s\n  %s\n  %s\n", command.name,
                    command.usage != NULL ? command.usage : command.name,
                    command.summary != NULL ? command.summary : "");
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

    /* Commands modules added, listed apart so it is clear which are part of
     * the system and which arrived with something installed. */
    int extra = recon_module_command_count();
    if (extra > 0) {
        out(s, "\nFrom modules:\n");
        for (int i = 0; i < extra; i++) {
            struct recon_command_registration command;
            if (recon_module_command_at(i, &command)) {
                out(s, "  %-10s %s\n", command.name,
                    command.summary != NULL ? command.summary : "");
            }
        }
    }
}

static void cmd_ver(struct recon_cmd_session *s, int argc, char **argv) {
    (void)argc; (void)argv;
    out(s, "%s %s\n", RECONOS_NAME, RECONOS_VERSION);
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

/*
 * The Recycle Bin.
 *
 * The bin existed and could only be reached from the File Explorer, which
 * meant nothing outside a window could look at it, put anything back, or
 * empty it -- and nothing could test that emptying it works. A thing the
 * system does that the system cannot be asked about is a thing that quietly
 * stops working.
 *
 * Note that `del` is not this: it removes a file, it does not bin it. That is
 * deliberate -- `del` has meant "gone" since DOS, and a word that has meant
 * one thing for forty years is a poor place to put a surprise -- but it does
 * mean the two ways of removing a file behave differently, which is worth
 * knowing before reaching for either.
 */
static void cmd_bin(struct recon_cmd_session *s, int argc, char **argv) {
    if (argc < 2 || strcasecmp(argv[1], "list") == 0) {
        int count = recon_fs_trash_count();
        if (count == 0) {
            out(s, "The bin is empty.\n");
            return;
        }

        struct recon_dirent entries[128];
        int got = recon_fs_list(NULL, recon_fs_trash_dir(), entries,
            (int)(sizeof(entries) / sizeof(entries[0])));
        if (got > (int)(sizeof(entries) / sizeof(entries[0]))) {
            got = (int)(sizeof(entries) / sizeof(entries[0]));
        }

        for (int i = 0; i < got; i++) {
            char origin[RECON_PATH_MAX];
            if (!recon_fs_trash_origin(entries[i].name, origin,
                    sizeof(origin))) {
                snprintf(origin, sizeof(origin), "(where it came from is not "
                    "recorded)");
            }
            out(s, "  %-28s %s\n", entries[i].name, origin);
        }

        out(s, "\n  %d item%s. 'bin restore <name>' puts one back, "
            "'bin empty' takes them all.\n", count, count == 1 ? "" : "s");
        return;
    }

    if (strcasecmp(argv[1], "empty") == 0) {
        int count = recon_fs_trash_count();
        if (count == 0) {
            out(s, "The bin is already empty.\n");
            return;
        }
        if (!recon_fs_trash_empty()) {
            out(s, "%s\n", recon_fs_last_error());
            return;
        }
        out(s, "Removed %d item%s permanently.\n", count,
            count == 1 ? "" : "s");
        return;
    }

    if (strcasecmp(argv[1], "restore") == 0) {
        if (argc < 3) {
            out(s, "Usage: bin restore <name>\n");
            return;
        }
        if (!recon_fs_trash_restore(argv[2])) {
            out(s, "%s\n", recon_fs_last_error());
            return;
        }
        out(s, "Put '%s' back.\n", argv[2]);
        return;
    }

    if (strcasecmp(argv[1], "purge") == 0) {
        if (argc < 3) {
            out(s, "Usage: bin purge <name>\n");
            return;
        }
        if (!recon_fs_trash_purge(argv[2])) {
            out(s, "%s\n", recon_fs_last_error());
            return;
        }
        out(s, "Removed '%s' permanently.\n", argv[2]);
        return;
    }

    /*
     * Anything else is read as a file to put in the bin, so the common case
     * is one word rather than two: `bin notes.txt`.
     */
    char path[RECON_PATH_MAX];
    size_t used = 0;
    for (int i = 1; i < argc && used < sizeof(path) - 1; i++) {
        int n = snprintf(path + used, sizeof(path) - used, "%s%s",
            used > 0 ? " " : "", argv[i]);
        if (n < 0 || (size_t)n >= sizeof(path) - used) {
            break;
        }
        used += (size_t)n;
    }

    if (!recon_fs_trash(s->cwd, path)) {
        out(s, "%s\n", recon_fs_last_error());
        return;
    }
    out(s, "'%s' is in the bin. 'bin restore' puts it back.\n", path);
}

/*
 * Start a program on the host, on ReconOS's Wayland socket.
 *
 * This is the one command that reaches the machine underneath, and the help
 * says plainly that nothing here does -- so it is only present when
 * RECONOS_ALLOW_SPAWN is set in the environment ReconOS was started from. It
 * exists for one reason: there is no Wayland client written for ReconOS, so
 * the only way to find out what ReconOS does with somebody else's window is to
 * start somebody else's window.
 *
 * `recon_spawn` had been in main.c since the compositor could host a client
 * and nothing had ever called it. A capability nothing exercises is a
 * capability nobody knows the state of.
 */
static void cmd_spawn(struct recon_cmd_session *s, int argc, char **argv) {
    struct recon_server *server = s->server;
    if (server == NULL) {
        out(s, "Nothing to launch onto.\n");
        return;
    }

    if (getenv("RECONOS_ALLOW_SPAWN") == NULL) {
        out(s, "'spawn' runs a program on the machine underneath, which is\n"
               "not something ReconOS does. It is here for testing what this\n"
               "compositor does with a window it did not draw, and is only\n"
               "available when RECONOS_ALLOW_SPAWN is set.\n");
        return;
    }

    /* Joined, so a command with a space in it works without quoting. */
    char command[RECON_PATH_MAX];
    size_t used = 0;
    for (int i = 1; i < argc && used < sizeof(command) - 1; i++) {
        int n = snprintf(command + used, sizeof(command) - used, "%s%s",
            i > 1 ? " " : "", argv[i]);
        if (n < 0 || (size_t)n >= sizeof(command) - used) {
            break;
        }
        used += (size_t)n;
    }

    recon_spawn(server, used > 0 ? command : NULL);
    out(s, "Started '%s'. It appears when it has drawn something.\n",
        used > 0 ? command : "the configured terminal");
}

/*
 * Look up an error code, or list them.
 *
 * The point of a code is that somebody can find out what it means from the
 * machine that showed it -- without a second machine, a search engine, or the
 * documentation being to hand. So the whole table is here, in the system,
 * rather than only in docs/ERRORS.md.
 */
static void cmd_errors(struct recon_cmd_session *s, int argc, char **argv) {
    if (argc >= 2 && strcasecmp(argv[1], "log") == 0) {
        size_t size = 0;
        char *text = recon_fs_read("/", RECON_ERROR_LOG, &size);
        if (text == NULL) {
            out(s, "Nothing has gone wrong on this machine yet.\n");
            return;
        }
        out(s, "%s", text);
        free(text);
        return;
    }

    if (argc >= 2) {
        const struct recon_error_info *info = recon_error_find(argv[1]);
        if (info == NULL) {
            out(s, "There is no error code '%s'. 'errors' lists them all.\n",
                argv[1]);
            return;
        }

        const char *area = recon_error_area_name(info->code[3]);
        const char *level =
            info->level == RECON_ERROR_STOP ? "stops the system"
            : info->level == RECON_ERROR_FAULT ? "something failed"
            : "recorded, nothing broke";

        out(s, "\n  %s  %s\n", info->code, info->summary);
        out(s, "  %-8s %s\n", "", area != NULL ? area : "");
        out(s, "  %-8s %s\n\n", "", level);
        out(s, "  %s\n\n", info->detail);
        return;
    }

    /*
     * The whole list, grouped by area, because a code is looked up far more
     * often than it is browsed -- and when it is browsed, it is because
     * somebody is asking what kind of thing can go wrong.
     */
    char area = '\0';
    int count = recon_error_count();

    for (int i = 0; i < count; i++) {
        const struct recon_error_info *info = recon_error_at(i);
        if (info == NULL) {
            continue;
        }

        if (info->code[3] != area) {
            area = info->code[3];
            const char *name = recon_error_area_name(area);
            out(s, "\n  %c -- %s\n", area, name != NULL ? name : "");
        }

        out(s, "    %-9s %-6s %s\n", info->code,
            info->level == RECON_ERROR_STOP ? "STOP"
                : info->level == RECON_ERROR_FAULT ? "fault" : "note",
            info->summary);
    }

    out(s, "\n  %d codes. 'errors <code>' says what one means; "
        "'errors log' is what has happened here.\n", count);
}

/*
 * Raise an error on purpose.
 *
 * Only with RECONOS_ALLOW_SPAWN set, the same gate as `spawn`, and for the
 * same reason: this is a development facility and not a thing a system should
 * offer. It exists because the stop screen is the one screen that has to work
 * on the day everything else has not, and a screen nobody has ever seen is a
 * screen nobody knows the state of.
 */
static void cmd_raise(struct recon_cmd_session *s, int argc, char **argv) {
    if (getenv("RECONOS_ALLOW_SPAWN") == NULL) {
        out(s, "'raise' makes the system report an error it did not have.\n"
               "It is here for testing the error screen, and is only\n"
               "available when RECONOS_ALLOW_SPAWN is set.\n");
        return;
    }

    if (argc < 2) {
        out(s, "Usage: raise <code>\n");
        return;
    }

    const struct recon_error_info *info = recon_error_find(argv[1]);
    if (info == NULL) {
        out(s, "There is no error code '%s'.\n", argv[1]);
        return;
    }

    /* Found by name, raised by index: the table is one array, so an entry's
     * position in it is its code. */
    int which = -1;
    for (int i = 0; i < recon_error_count(); i++) {
        if (recon_error_at(i) == info) {
            which = i;
            break;
        }
    }
    if (which < 0) {
        out(s, "That code is not in the table.\n");
        return;
    }

    out(s, "Raising %s.\n", info->code);
    recon_error_raise(s->server, (enum recon_error_code)which,
        "Raised on purpose from the terminal.");
}

/* One rule, as a line somebody can read. */
static void show_rule(struct recon_cmd_session *s, int index,
        const struct recon_fw_rule *rule) {
    char ports[48];
    if (rule->port_from == 0 && rule->port_to == 0) {
        recon_text_copy(ports, sizeof(ports), "any");
    } else if (rule->port_from == rule->port_to) {
        snprintf(ports, sizeof(ports), "%d", rule->port_from);
    } else {
        snprintf(ports, sizeof(ports), "%d-%d", rule->port_from,
            rule->port_to);
    }

    out(s, "  %2d  %-3s %-5s %-4s %-9s %-5s %s\n", index + 1,
        rule->enabled ? "on" : "off",
        recon_fw_action_name(rule->action),
        recon_fw_direction_name(rule->direction),
        ports,
        recon_fw_protocol_name(rule->protocol),
        rule->name);
}

/*
 * The firewall.
 *
 * Reads like the firewall command on any other system, because the shape is
 * the shape people already know: a switch, a default per direction, and a
 * numbered list where the first match decides.
 */
/*
 * The services, listed and driven.
 *
 * Watchtower has the same list behind a tab, and this is the same registry --
 * not a second copy of it. Two lists of what is running would eventually
 * disagree, and the one somebody happened to be looking at would be the one
 * they believed.
 */
/*
 * Blank the screen now, or say what the settings are.
 *
 * Here because the thing it drives is a timer measured in minutes, and a test
 * that has to wait a minute to find out whether blanking works is a test
 * nobody runs.
 */
static void cmd_blank(struct recon_cmd_session *s, int argc, char **argv) {
    struct recon_server *server = s->server;
    if (server == NULL || server->shell == NULL) {
        out(s, "  No desktop to blank.\n");
        return;
    }

    /*
     * Re-read the settings first. The shell holds them rather than asking the
     * registry sixty times a second, and the Control Panel tells it when it
     * changes one -- but `reg set` does not, so a setting written that way
     * would be reported here as whatever it was at startup.
     */
    recon_shell_blank_reload(server->shell);

    if (argc >= 2 && strcasecmp(argv[1], "now") == 0) {
        recon_shell_blank(server->shell);
        out(s, "  Blanked.\n");
        return;
    }

    if (argc >= 2 && strcasecmp(argv[1], "wake") == 0) {
        out(s, recon_shell_note_input(server->shell)
            ? "  Woken.\n" : "  It was not blanked.\n");
        return;
    }

    int minutes = recon_registry_get_int(RECON_REG_USER,
        RECON_BLANK_AFTER_KEY, 0);
    bool lock = recon_registry_get_bool(RECON_REG_USER,
        RECON_BLANK_LOCK_KEY, false);

    out(s, "\n  Blank the screen: %s\n", minutes > 0 ? "yes" : "never");
    if (minutes > 0) {
        out(s, "  After:            %d minute%s\n", minutes,
            minutes == 1 ? "" : "s");
        out(s, "  On waking:        %s\n",
            lock ? "ask for the password" : "straight back");
    }
    out(s, "  Now:              %s\n\n",
        recon_shell_is_blanked(server->shell) ? "blanked" : "awake");
    out(s, "  blank now | blank wake\n");
}

static void cmd_services(struct recon_cmd_session *s, int argc, char **argv) {
    if (argc < 2) {
        int count = recon_service_count();
        out(s, "\n  %-18s %-10s %6s  %s\n", "service", "state", "starts",
            "what it does");

        for (int i = 0; i < count; i++) {
            struct recon_service_info svc;
            if (!recon_service_at(i, &svc)) {
                continue;
            }

            char state[48];
            if (svc.state == RECON_SERVICE_FAILED && svc.problem[0] != '\0') {
                snprintf(state, sizeof(state), "failed %s", svc.problem);
            } else {
                snprintf(state, sizeof(state), "%s",
                    recon_service_state_name(svc.state));
            }

            out(s, "  %-18s %-10s %6d  %s%s\n", svc.name, state, svc.starts,
                svc.detail, svc.essential ? " [essential]" : "");
        }

        out(s, "\n  services start|stop|restart <name>\n"
               "  An essential service can be restarted but not stopped.\n");
        return;
    }

    const char *what = argv[1];
    if (argc < 3) {
        out(s, "  Which service? 'services' lists them.\n");
        return;
    }

    /*
     * The rest of the line, joined: service names have spaces in them
     * ("Desktop shell"), and quoting one on a command line is a thing nobody
     * should have to remember.
     */
    char name[RECON_SERVICE_NAME_MAX];
    name[0] = '\0';
    for (int i = 2; i < argc; i++) {
        if (i > 2) {
            strncat(name, " ", sizeof(name) - strlen(name) - 1);
        }
        strncat(name, argv[i], sizeof(name) - strlen(name) - 1);
    }

    struct recon_service_info svc;
    if (!recon_service_find(name, &svc)) {
        out(s, "  There is no service called '%s'.\n", name);
        return;
    }

    bool ok;
    if (strcasecmp(what, "stop") == 0) {
        ok = recon_service_stop(name);
    } else if (strcasecmp(what, "restart") == 0) {
        ok = recon_service_restart(name);
    } else if (strcasecmp(what, "start") == 0) {
        ok = recon_service_start(name);
    } else {
        out(s, "  services start|stop|restart <name>\n");
        return;
    }

    if (ok) {
        out(s, "  %s: %s.\n", svc.name, what);
    } else {
        const char *why = recon_service_last_error();
        out(s, "  Could not %s %s%s%s\n", what, svc.name,
            (why != NULL && why[0] != '\0') ? ": " : ".",
            (why != NULL && why[0] != '\0') ? why : "");
    }
}

static void cmd_firewall(struct recon_cmd_session *s, int argc, char **argv) {
    if (argc < 2) {
        out(s, "\nFirewall: %s\n", recon_firewall_is_on() ? "on" : "off");
        out(s, "  incoming, by default: %s\n",
            recon_fw_action_name(recon_firewall_default(RECON_FW_IN)));
        out(s, "  outgoing, by default: %s\n\n",
            recon_fw_action_name(recon_firewall_default(RECON_FW_OUT)));

        int count = recon_firewall_count();
        if (count == 0) {
            out(s, "  No rules. The defaults above decide everything.\n");
        } else {
            out(s, "   #   on  what  dir  ports     proto name\n");
            for (int i = 0; i < count; i++) {
                struct recon_fw_rule rule;
                if (recon_firewall_at(i, &rule)) {
                    show_rule(s, i, &rule);
                }
            }
        }

        out(s, "\n  The first rule that matches decides, so the order is part\n"
               "  of the rule. 'firewall help' lists what can be changed.\n");
        return;
    }

    const char *what = argv[1];

    if (strcasecmp(what, "help") == 0) {
        out(s,
            "  firewall                      what it is doing\n"
            "  firewall on|off               the switch\n"
            "  firewall default in|out allow|block\n"
            "  firewall allow|block <n>      change what rule n does\n"
            "  firewall enable|disable <n>   put rule n in or out of force\n"
            "  firewall up|down <n>          move rule n, which changes order\n"
            "  firewall remove <n>\n"
            "  firewall add in|out tcp|udp|any <port>[-<port>] allow|block "
            "<name>\n"
            "  firewall test in|out tcp|udp <port> [program]\n"
            "\n  Ports have names: 'firewall ports' lists the ones it "
            "knows.\n");
        return;
    }

    if (strcasecmp(what, "ports") == 0) {
        /* Walked rather than listed, because the table lives in the firewall
         * and a second copy here would be a second thing to update. */
        for (int port = 1; port <= 65535; port++) {
            const char *name = recon_fw_port_name(port);
            if (name != NULL) {
                out(s, "  %5d  %s\n", port, name);
            }
        }
        return;
    }

    if (strcasecmp(what, "on") == 0 || strcasecmp(what, "off") == 0) {
        bool on = (strcasecmp(what, "on") == 0);
        if (!recon_firewall_set_on(on)) {
            out(s, "%s\n", recon_firewall_last_error());
            return;
        }
        out(s, "The firewall is %s.\n", on ? "on" : "off");
        return;
    }

    if (strcasecmp(what, "default") == 0) {
        if (argc < 4) {
            out(s, "Usage: firewall default in|out allow|block\n");
            return;
        }
        enum recon_fw_direction direction =
            (strcasecmp(argv[2], "in") == 0) ? RECON_FW_IN : RECON_FW_OUT;
        enum recon_fw_action action =
            (strcasecmp(argv[3], "allow") == 0) ? RECON_FW_ALLOW
                                                : RECON_FW_BLOCK;

        if (!recon_firewall_set_default(direction, action)) {
            out(s, "%s\n", recon_firewall_last_error());
            return;
        }
        out(s, "%s traffic is %s by default now.\n",
            direction == RECON_FW_IN ? "Incoming" : "Outgoing",
            recon_fw_action_name(action));
        return;
    }

    if (strcasecmp(what, "test") == 0) {
        if (argc < 5) {
            out(s, "Usage: firewall test in|out tcp|udp <port> [program]\n");
            return;
        }
        enum recon_fw_direction direction =
            (strcasecmp(argv[2], "in") == 0) ? RECON_FW_IN : RECON_FW_OUT;
        enum recon_fw_protocol protocol =
            (strcasecmp(argv[3], "udp") == 0) ? RECON_FW_UDP : RECON_FW_TCP;
        int port = atoi(argv[4]);
        const char *program = (argc > 5) ? argv[5] : NULL;

        char why[96];
        bool allowed = recon_firewall_allows(direction, protocol, port,
            program, why, sizeof(why));

        const char *name = recon_fw_port_name(port);
        out(s, "  %s %s %d%s%s%s for %s: %s (%s)\n",
            recon_fw_direction_name(direction),
            recon_fw_protocol_name(protocol), port,
            name != NULL ? " (" : "", name != NULL ? name : "",
            name != NULL ? ")" : "",
            program != NULL ? program : "the system",
            allowed ? "allowed" : "blocked", why);
        return;
    }

    /* The rest take a rule number, counted from one as it is displayed. */
    if (strcasecmp(what, "allow") == 0 || strcasecmp(what, "block") == 0 ||
            strcasecmp(what, "enable") == 0 ||
            strcasecmp(what, "disable") == 0 ||
            strcasecmp(what, "remove") == 0 ||
            strcasecmp(what, "up") == 0 || strcasecmp(what, "down") == 0) {

        if (argc < 3) {
            out(s, "Usage: firewall %s <rule number>\n", what);
            return;
        }

        int index = atoi(argv[2]) - 1;
        struct recon_fw_rule rule;
        if (!recon_firewall_at(index, &rule)) {
            out(s, "There is no rule %s. 'firewall' lists them.\n", argv[2]);
            return;
        }

        bool done = false;
        if (strcasecmp(what, "enable") == 0) {
            done = recon_firewall_set_rule_on(index, true);
        } else if (strcasecmp(what, "disable") == 0) {
            done = recon_firewall_set_rule_on(index, false);
        } else if (strcasecmp(what, "remove") == 0) {
            done = recon_firewall_remove(index);
        } else if (strcasecmp(what, "up") == 0) {
            done = recon_firewall_move(index, -1);
        } else if (strcasecmp(what, "down") == 0) {
            done = recon_firewall_move(index, 1);
        } else {
            /* allow or block: the rule is rewritten with the other action,
             * which keeps its place in the order. */
            rule.action = (strcasecmp(what, "allow") == 0)
                ? RECON_FW_ALLOW : RECON_FW_BLOCK;
            done = recon_firewall_remove(index);
            if (done) {
                done = recon_firewall_add(&rule);
                if (done) {
                    /* Back where it was: order decides, so an edit must not
                     * quietly move a rule to the end of the list. */
                    recon_firewall_move(recon_firewall_count() - 1,
                        index - (recon_firewall_count() - 1));
                }
            }
        }

        if (!done) {
            out(s, "%s\n", recon_firewall_last_error());
            return;
        }
        out(s, "Done. '%s' is now:\n", rule.name);
        if (recon_firewall_at(index, &rule)) {
            show_rule(s, index, &rule);
        }
        return;
    }

    if (strcasecmp(what, "add") == 0) {
        if (argc < 7) {
            out(s, "Usage: firewall add in|out tcp|udp|any "
                   "<port>[-<port>] allow|block <name>\n");
            return;
        }

        struct recon_fw_rule rule;
        memset(&rule, 0, sizeof(rule));

        rule.direction = (strcasecmp(argv[2], "in") == 0)
            ? RECON_FW_IN : RECON_FW_OUT;

        if (strcasecmp(argv[3], "tcp") == 0) {
            rule.protocol = RECON_FW_TCP;
        } else if (strcasecmp(argv[3], "udp") == 0) {
            rule.protocol = RECON_FW_UDP;
        } else {
            rule.protocol = RECON_FW_ANY_PROTOCOL;
        }

        const char *ports = argv[4];
        const char *dash = strchr(ports, '-');
        rule.port_from = atoi(ports);
        rule.port_to = (dash != NULL) ? atoi(dash + 1) : rule.port_from;

        rule.action = (strcasecmp(argv[5], "allow") == 0)
            ? RECON_FW_ALLOW : RECON_FW_BLOCK;

        /* The name is the tail, joined: it has spaces in it and is the thing
         * the log says when the rule fires. */
        size_t used = 0;
        for (int i = 6; i < argc && used < sizeof(rule.name) - 1; i++) {
            int n = snprintf(rule.name + used, sizeof(rule.name) - used,
                "%s%s", i > 6 ? " " : "", argv[i]);
            if (n < 0 || (size_t)n >= sizeof(rule.name) - used) {
                break;
            }
            used += (size_t)n;
        }

        /* A rule somebody just wrote is in force. Adding one off would be a
         * rule that does nothing and looks like it does. */
        rule.enabled = true;

        if (!recon_firewall_add(&rule)) {
            out(s, "%s\n", recon_firewall_last_error());
            return;
        }

        out(s, "Added, at the end of the list:\n");
        int index = recon_firewall_count() - 1;
        struct recon_fw_rule added;
        if (recon_firewall_at(index, &added)) {
            show_rule(s, index, &added);
        }
        out(s, "\n  The first match decides, so 'firewall up %d' if it has to\n"
               "  come before something else.\n", index + 1);
        return;
    }

    out(s, "'firewall %s' is not something it does. 'firewall help' lists "
        "what is.\n", what);
}

/*
 * Remote access: which way in is open, and the key for the one that needs it.
 *
 * Both ways are always described, because only one of them is secure and a
 * command that mentioned the network port alone would be recommending the
 * worse option by omission.
 */
static void cmd_remote(struct recon_cmd_session *s, int argc, char **argv) {
    struct recon_control *control = recon_server_control(s->server);

    if (argc < 2) {
        out(s, "\nRemote access\n\n");

        out(s, "  Over SSH, always available and encrypted by SSH:\n");
        out(s, "    ssh -L /tmp/recon-there.sock:%s user@this-machine\n",
            recon_control_path(control));
        out(s, "    nc -U /tmp/recon-there.sock\n\n");

        bool listening = recon_control_network_listening(control);
        int port = listening ? recon_control_network_port(control)
            : recon_registry_get_int(RECON_REG_SYSTEM,
                RECON_REMOTE_PORT_KEY, RECON_FW_RECON_PORT);

        out(s, "  Over the network, on TCP %d: %s\n", port,
            listening ? "listening" : "off");
        out(s, "    A key: %s\n",
            recon_control_has_key() ? "set" : "not set yet");

        char why[96];
        bool allowed = recon_firewall_allows(RECON_FW_IN, RECON_FW_TCP, port,
            NULL, why, sizeof(why));
        out(s, "    The firewall: %s (%s)\n\n",
            allowed ? "allows it" : "blocks it", why);

        if (!listening) {
            out(s, "  'remote on' opens it. 'remote key' makes a key.\n");
        } else {
            out(s, "  'remote off' closes it.\n");
        }

        out(s, "\n  The key crosses the network in the clear -- there is no\n"
               "  TLS yet. On a trusted network that is a reasonable trade;\n"
               "  across anything else, use the SSH route above.\n");
        return;
    }

    if (strcasecmp(argv[1], "key") == 0) {
        char key[64];
        if (!recon_control_new_key(key, sizeof(key))) {
            out(s, "The key could not be made.\n");
            return;
        }

        out(s, "\n  %s\n\n", key);
        out(s, "  Written down now: this is the only time it is shown. Only\n"
               "  its hash is kept, so it cannot be read back -- 'remote "
               "key'\n  again makes a new one and forgets this.\n");
        return;
    }

    if (strcasecmp(argv[1], "port") == 0) {
        if (argc < 3) {
            out(s, "Usage: remote port <number>\n");
            return;
        }
        int port = atoi(argv[2]);
        if (port <= 0 || port > 65535) {
            out(s, "'%s' is not a port.\n", argv[2]);
            return;
        }
        recon_registry_set_int(RECON_REG_SYSTEM, RECON_REMOTE_PORT_KEY, port);
        out(s, "The port is %d. 'remote on' to open it there.\n", port);
        return;
    }

    if (strcasecmp(argv[1], "on") == 0) {
        int port = recon_registry_get_int(RECON_REG_SYSTEM,
            RECON_REMOTE_PORT_KEY, RECON_FW_RECON_PORT);

        char why[192];
        if (!recon_control_listen_network(control, port, why, sizeof(why))) {
            out(s, "Not opened: %s\n", why);
            return;
        }

        recon_registry_set_bool(RECON_REG_SYSTEM, RECON_REMOTE_ON_KEY, true);

        out(s, "Listening on %d. A connection is asked for the key before\n"
               "it can run anything.\n\n", port);
        out(s, "  The key crosses the network in the clear. There is no TLS\n"
               "  yet, so this is for a network you trust.\n");
        return;
    }

    if (strcasecmp(argv[1], "off") == 0) {
        recon_control_stop_network(control);
        recon_registry_set_bool(RECON_REG_SYSTEM, RECON_REMOTE_ON_KEY, false);
        out(s, "The port is closed. Connections already open are left "
               "alone.\n");
        return;
    }

    out(s, "'remote %s' is not something it does. 'remote' says what is.\n",
        argv[1]);
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
        out(s, "  ui drag <id> <dx> <dy>  press a region, move, release\n");
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

    if (strcasecmp(what, "scroll") == 0) {
        if (argc < 3) {
            out(s, "Usage: ui scroll <up|down> [times]\n");
            return;
        }

        bool down = strcasecmp(argv[2], "down") == 0;
        int times = (argc >= 4) ? atoi(argv[3]) : 1;
        if (times < 1) {
            times = 1;
        }

        for (int i = 0; i < times; i++) {
            recon_inject_scroll(server, down ? 1.0 : -1.0);
        }
        out(s, "scrolled %s %d\n", down ? "down" : "up", times);
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

    /*
     * Press on a region, move, release.
     *
     * 'ui hit' presses and releases in the same place, which cannot express a
     * drag at all -- so anything dragged (a column boundary, a window edge, a
     * selection) was untestable except by measuring a screenshot. This is the
     * gesture, done through the same entry points a real pointer uses.
     */
    if (strcasecmp(what, "drag") == 0) {
        if (argc < 5) {
            out(s, "Usage: ui drag <region id> <dx> <dy>\n");
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

        int dx = atoi(argv[3]);
        int dy = atoi(argv[4]);

        recon_inject_pointer(server, x, y);
        recon_inject_button(server, BTN_LEFT, true);
        /* A step in the middle, because a drag that only ever reports its
         * endpoint would not exercise the code that follows the pointer. */
        recon_inject_pointer(server, x + dx / 2, y + dy / 2);
        recon_inject_pointer(server, x + dx, y + dy);
        recon_inject_button(server, BTN_LEFT, false);

        out(s, "dragged region %u from %d,%d by %d,%d\n", id, x, y, dx, dy);
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

    if (strcasecmp(what, "account") == 0) {
        if (argc < 3) {
            out(s, "Usage: ui account <name>\n");
            return;
        }

        /* Joined, so an account name with a space in it works unquoted. */
        char name[64];
        size_t used = 0;
        for (int i = 2; i < argc && used < sizeof(name) - 1; i++) {
            int w = snprintf(name + used, sizeof(name) - used, "%s%s",
                i > 2 ? " " : "", argv[i]);
            if (w < 0) {
                break;
            }
            used += (size_t)w;
        }

        int x = 0, y = 0;
        if (!recon_shell_account_at(server->shell, name, &x, &y)) {
            out(s, "The login screen is not offering '%s'.\n", name);
            return;
        }

        recon_inject_pointer(server, x, y);
        recon_inject_button(server, BTN_LEFT, true);
        recon_inject_button(server, BTN_LEFT, false);
        out(s, "chose '%s' at %d,%d\n", name, x, y);
        return;
    }

    if (strcasecmp(what, "start") == 0) {
        if (argc < 3) {
            out(s, "Usage: ui start <label>\n");
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

        /*
         * Open the menu first if it is closed. Choosing something from the
         * Start menu is what this means, and a person would open it -- making
         * every test do that with a click at a guessed coordinate was one
         * more thing to get wrong on a screen of a different size.
         */
        recon_shell_open_menu(server->shell);

        int x = 0, y = 0;
        if (!recon_shell_menu_entry_at(server->shell, label, &x, &y)) {
            out(s, "The Start menu is not showing '%s'.\n", label);
            return;
        }

        recon_inject_pointer(server, x, y);
        recon_inject_button(server, BTN_LEFT, true);
        recon_inject_button(server, BTN_LEFT, false);
        out(s, "chose '%s' at %d,%d\n", label, x, y);
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

/* What a module added, and what was refused. */
static void cmd_modules(struct recon_cmd_session *s, int argc, char **argv) {
    if (argc > 1 && strcasecmp(argv[1], "load") == 0) {
        if (argc < 3) {
            out(s, "Usage: modules load <path>\n");
            return;
        }
        if (!recon_modules_load(argv[2])) {
            out(s, "%s\n", recon_modules_last_error());
            return;
        }
        out(s, "Loaded '%s'.\n", argv[2]);
        return;
    }

    if (argc > 1 && strcasecmp(argv[1], "unload") == 0) {
        if (argc < 3) {
            out(s, "Usage: modules unload <name>\n");
            return;
        }
        if (!recon_modules_unload(argv[2])) {
            out(s, "%s\n", recon_modules_last_error());
            return;
        }
        out(s, "Unloaded '%s'.\n", argv[2]);
        return;
    }

    int count = recon_modules_count();
    if (count == 0) {
        out(s, "No modules are loaded.\n");
    }

    for (int i = 0; i < count; i++) {
        struct recon_module_state state;
        if (!recon_modules_at(i, &state)) {
            continue;
        }

        out(s, "  %-18s %-8s %-6s %s\n",
            state.name,
            state.version[0] != '\0' ? state.version : "-",
            state.is_app ? "app" : "system",
            state.loaded ? state.description : "NOT LOADED");

        /* Say why, when it did not. A module that fails invisibly is a module
         * nobody fixes. */
        if (!state.loaded && state.problem[0] != '\0') {
            out(s, "  %-18s %s\n", "", state.problem);
        }
    }

    int apps = recon_installed_app_count();
    if (apps > 0) {
        out(s, "\nApplications:\n");
        for (int i = 0; i < apps; i++) {
            struct recon_installed_app app;
            if (!recon_installed_app_at(i, &app)) {
                continue;
            }
            out(s, "  %-18s %s\n", app.name,
                app.module[0] != '\0' ? app.module : "built in");
        }
    }
}

/* Read and write what the system remembers. */
static void cmd_reg(struct recon_cmd_session *s, int argc, char **argv) {
    /*
     * The hive is named rather than guessed. Which one a setting belongs in is
     * a real decision -- a theme is a person's, a module policy is the
     * machine's -- and defaulting would make it silently.
     */
    if (argc < 3) {
        out(s, "Usage: reg <system|user> list [prefix]\n");
        out(s, "       reg <system|user> get <key>\n");
        out(s, "       reg <system|user> set <key> <value>\n");
        out(s, "       reg <system|user> del <key>\n");
        return;
    }

    enum recon_registry_scope scope;
    if (strcasecmp(argv[1], "system") == 0) {
        scope = RECON_REG_SYSTEM;
    } else if (strcasecmp(argv[1], "user") == 0) {
        scope = RECON_REG_USER;
    } else {
        out(s, "'%s' is not a hive. Use 'system' or 'user'.\n", argv[1]);
        return;
    }

    const char *action = argv[2];

    if (strcasecmp(action, "list") == 0) {
        const char *prefix = argc > 3 ? argv[3] : "";
        int count = recon_registry_count(scope, prefix);

        if (count == 0) {
            out(s, "Nothing is stored%s%s.\n",
                *prefix != '\0' ? " under " : "", prefix);
            return;
        }

        for (int i = 0; i < count; i++) {
            const char *key = NULL;
            const char *value = NULL;
            if (recon_registry_at(scope, prefix, i, &key, &value)) {
                out(s, "  %-38s %s\n", key, value);
            }
        }
        out(s, "\n  %d setting%s\n", count, count == 1 ? "" : "s");
        return;
    }

    if (argc < 4) {
        out(s, "Usage: reg %s %s <key>%s\n", argv[1], action,
            strcasecmp(action, "set") == 0 ? " <value>" : "");
        return;
    }

    if (strcasecmp(action, "get") == 0) {
        const char *value = recon_registry_get(scope, argv[3], NULL);
        if (value == NULL) {
            out(s, "'%s' is not set.\n", argv[3]);
            return;
        }
        out(s, "%s\n", value);
        return;
    }

    if (strcasecmp(action, "set") == 0) {
        /* The rest of the line is the value, so a setting can contain
         * spaces without needing quoting rules the interpreter does not
         * have. */
        char value[RECON_REGISTRY_VALUE_MAX];
        size_t used = 0;
        for (int i = 4; i < argc && used < sizeof(value) - 1; i++) {
            int written = snprintf(value + used, sizeof(value) - used, "%s%s",
                i > 4 ? " " : "", argv[i]);
            if (written < 0) {
                break;
            }
            used += (size_t)written;
        }
        if (argc == 4) {
            value[0] = '\0';
        }

        if (!recon_registry_set(scope, argv[3], value)) {
            out(s, "%s\n", recon_registry_last_error());
            return;
        }
        out(s, "%s = %s\n", argv[3], value);
        return;
    }

    if (strcasecmp(action, "del") == 0) {
        if (!recon_registry_remove(scope, argv[3])) {
            out(s, "%s\n", recon_registry_last_error());
            return;
        }
        out(s, "Removed '%s'.\n", argv[3]);
        return;
    }

    out(s, "'%s' is not something reg does.\n", action);
}

/* List the skins, or put one on. */
static void cmd_theme(struct recon_cmd_session *s, int argc, char **argv) {
    if (argc < 2) {
        out(s, "Current skin: %s\n\n", recon_theme_current());

        int count = recon_theme_count();
        for (int i = 0; i < count; i++) {
            struct recon_theme_info info;
            if (!recon_theme_at(i, &info)) {
                continue;
            }
            out(s, "  %-12s %-8s %s\n", info.name,
                info.built_in ? "built in" : "file",
                info.description);
        }
        out(s, "\n'theme <name>' puts one on. 'theme roles' lists what a skin "
            "can colour.\n'theme copy <new name>' starts one from the one you "
            "are using.\n");
        return;
    }

    if (strcasecmp(argv[1], "roles") == 0) {
        /*
         * Every role and what it currently resolves to. This is what somebody
         * writing a skin needs: the list of questions, and the answers the
         * current one gives, so a new file can start from something real.
         */
        for (int i = 0; i < RECON_THEME_ROLE_COUNT; i++) {
            recon_color c = recon_theme_color((enum recon_theme_role)i);
            out(s, "  %-22s %08X\n", recon_theme_role_name(i), c);
        }
        out(s, "\n  %d roles. Skins live in %s as %s files.\n",
            RECON_THEME_ROLE_COUNT, RECON_DIR_THEMES, RECON_THEME_EXT);
        return;
    }

    if (strcasecmp(argv[1], "copy") == 0) {
        if (argc < 3) {
            out(s, "Usage: theme copy <new name>\n\n"
                "Copies the skin in use into %s as a file you can edit.\n",
                RECON_DIR_THEMES);
            return;
        }

        /* Joined, so a name with a space in it works without quoting. */
        char name[64];
        size_t used = 0;
        for (int i = 2; i < argc && used < sizeof(name) - 1; i++) {
            int n = snprintf(name + used, sizeof(name) - used, "%s%s",
                i > 2 ? " " : "", argv[i]);
            if (n < 0 || (size_t)n >= sizeof(name) - used) {
                break;
            }
            used += (size_t)n;
        }

        const char *from = recon_theme_current();
        if (!recon_theme_copy(NULL, name, NULL)) {
            out(s, "%s\n", recon_theme_last_error());
            return;
        }

        out(s, "Copied '%s' to '%s'. The file is %s/%s%s -- edit it, then "
            "'theme %s' to see it.\n", from, name, RECON_DIR_THEMES, name,
            RECON_THEME_EXT, name);
        return;
    }

    if (strcasecmp(argv[1], "install") == 0) {
        if (argc < 3) {
            out(s, "Usage: theme install <file>\n");
            return;
        }

        /* Joined, so a path with a space in it works without quoting -- the
         * same reason `apps File Explorer` joins its tail. */
        char path[RECON_PATH_MAX];
        size_t used = 0;
        for (int i = 2; i < argc && used < sizeof(path) - 1; i++) {
            int n = snprintf(path + used, sizeof(path) - used, "%s%s",
                i > 2 ? " " : "", argv[i]);
            if (n < 0) {
                break;
            }
            used += (size_t)n;
        }

        const char *resolved = path;
        char joined[RECON_PATH_MAX];
        if (path[0] != '/') {
            if (!recon_fs_join(joined, sizeof(joined), recon_cmd_cwd(s),
                    path)) {
                out(s, "That path is too long.\n");
                return;
            }
            resolved = joined;
        }

        if (!recon_theme_install(resolved)) {
            out(s, "%s\n", recon_theme_last_error());
            return;
        }
        out(s, "Installed. 'theme' lists it; 'theme <name>' puts it on.\n");
        return;
    }

    if (strcasecmp(argv[1], "remove") == 0) {
        if (argc < 3) {
            out(s, "Usage: theme remove <name>\n");
            return;
        }

        char name[48];
        size_t used = 0;
        for (int i = 2; i < argc && used < sizeof(name) - 1; i++) {
            int n = snprintf(name + used, sizeof(name) - used, "%s%s",
                i > 2 ? " " : "", argv[i]);
            if (n < 0) {
                break;
            }
            used += (size_t)n;
        }

        if (!recon_theme_uninstall(name)) {
            out(s, "%s\n", recon_theme_last_error());
            return;
        }

        /* The skin may have been the one on screen, in which case removing it
         * put the default on and everything drawn needs to hear about it. */
        recon_shell_restyle(s->server->shell);
        out(s, "Removed. The skin in use is '%s'.\n", recon_theme_current());
        return;
    }

    if (!recon_theme_set(argv[1])) {
        out(s, "%s\n", recon_theme_last_error());
        return;
    }

    /* The whole desktop, not just what is in front. */
    recon_shell_restyle(s->server->shell);
    out(s, "Skin is now '%s'.\n", recon_theme_current());
}

/* Reading settings: spacing, and the font. */
static void cmd_access(struct recon_cmd_session *s, int argc, char **argv) {
    struct recon_shell *shell = s->server->shell;

    if (argc < 2) {
        const char *font = recon_registry_get(RECON_REG_USER,
            RECON_ACCESS_FONT_KEY, "");

        out(s, "Letter spacing: %d\n", recon_text_letter_spacing());
        out(s, "Line spacing:   %d\n", recon_text_line_spacing());
        out(s, "Font:           %s\n", *font != '\0' ? font : "(the system's)");
        out(s, "Font size:      %d\n",
            recon_registry_get_int(RECON_REG_USER,
                RECON_ACCESS_FONT_SIZE_KEY,
                RECON_ACCESS_FONT_SIZE_DEFAULT));
        out(s, "\n");
        out(s, "  access spacing <n>   space between letters\n");
        out(s, "  access lines <n>     space between lines\n");
        out(s, "  access font <path>   a font file, or 'default'\n");
        out(s, "  access size <n>      font height in pixels (8-24)\n");
        out(s, "  access reading       spacing that suits a dyslexic reader\n");
        out(s, "  access reset         back to the defaults\n");
        return;
    }

    const char *action = argv[1];

    if (strcasecmp(action, "reading") == 0) {
        /*
         * Wider letters and lines. Extra letter spacing is the adjustment
         * with the best evidence behind it for dyslexic readers -- more than
         * a special typeface, whose advantage has not held up in controlled
         * study. The values are a starting point to adjust from, not a
         * prescription.
         */
        recon_registry_set_int(RECON_REG_USER, RECON_ACCESS_LETTER_KEY, 2);
        recon_registry_set_int(RECON_REG_USER, RECON_ACCESS_LINE_KEY, 6);
        recon_access_apply(recon_shell_font(shell));
        recon_shell_restyle(shell);
        out(s, "Letters and lines are further apart.\n");
        out(s, "The 'Reading' skin softens the contrast to go with it.\n");
        return;
    }

    if (strcasecmp(action, "reset") == 0) {
        recon_registry_remove(RECON_REG_USER, RECON_ACCESS_LETTER_KEY);
        recon_registry_remove(RECON_REG_USER, RECON_ACCESS_LINE_KEY);
        recon_registry_remove(RECON_REG_USER, RECON_ACCESS_FONT_KEY);
        recon_registry_remove(RECON_REG_USER, RECON_ACCESS_FONT_SIZE_KEY);
        recon_access_apply(recon_shell_font(shell));
        recon_shell_restyle(shell);
        out(s, "Back to the defaults.\n");
        return;
    }

    if (argc < 3) {
        out(s, "Usage: access %s <value>\n", action);
        return;
    }

    if (strcasecmp(action, "spacing") == 0 || strcasecmp(action, "lines") == 0) {
        bool letters = (strcasecmp(action, "spacing") == 0);
        recon_registry_set_int(RECON_REG_USER,
            letters ? RECON_ACCESS_LETTER_KEY : RECON_ACCESS_LINE_KEY,
            atoi(argv[2]));

        recon_access_apply(recon_shell_font(shell));
        recon_shell_restyle(shell);
        out(s, "%s spacing is now %d.\n", letters ? "Letter" : "Line",
            letters ? recon_text_letter_spacing() : recon_text_line_spacing());
        return;
    }

    if (strcasecmp(action, "font") == 0) {
        bool clearing = (strcasecmp(argv[2], "default") == 0);
        recon_registry_set(RECON_REG_USER, RECON_ACCESS_FONT_KEY,
            clearing ? "" : argv[2]);

        /* Applied immediately, and reported honestly if it did not take: a
         * font file that cannot be read leaves the old one in place. */
        recon_access_apply(recon_shell_font(shell));
        recon_shell_restyle(shell);

        if (!clearing && !recon_fs_exists("/", argv[2]) &&
                access(argv[2], R_OK) != 0) {
            out(s, "Set, but '%s' could not be read, so the font has not "
                "changed.\n", argv[2]);
            return;
        }
        out(s, "Font is now %s.\n", clearing ? "the system's" : argv[2]);
        return;
    }

    if (strcasecmp(action, "size") == 0) {
        recon_registry_set_int(RECON_REG_USER, RECON_ACCESS_FONT_SIZE_KEY,
            atoi(argv[2]));
        recon_access_apply(recon_shell_font(shell));
        recon_shell_restyle(shell);
        out(s, "Font size is now %d.\n",
            recon_registry_get_int(RECON_REG_USER,
                RECON_ACCESS_FONT_SIZE_KEY,
                RECON_ACCESS_FONT_SIZE_DEFAULT));
        out(s, "Note: the chrome is laid out in fixed pixels, so a large "
            "size can crowd it.\n");
        return;
    }

    out(s, "'%s' is not something access does.\n", action);
}

/* What the gate is showing, and who is signed in. */
static void cmd_session(struct recon_cmd_session *s, int argc, char **argv) {
    (void)argc; (void)argv;

    char buffer[512];
    buffer[0] = '\0';
    recon_shell_describe_session(s->server->shell, buffer, sizeof(buffer));
    out(s, "%s", buffer);
}

/* The accounts on the system. */
static void cmd_users(struct recon_cmd_session *s, int argc, char **argv) {
    if (argc < 2) {
        int count = recon_users_count();
        const char *current = recon_users_current();

        out(s, "Signed in: %s\n\n", current != NULL ? current : "(nobody)");
        for (int i = 0; i < count; i++) {
            struct recon_user user;
            if (!recon_users_at(i, &user)) {
                continue;
            }
            out(s, "  %-20s %-14s %s\n", user.name,
                user.role == RECON_ROLE_ADMINISTRATOR
                    ? "administrator" : "limited",
                user.has_password ? "password set" : "no password");
        }
        out(s, "\n  %d account%s\n", count, count == 1 ? "" : "s");
        return;
    }

    /*
     * Only an administrator may change accounts. The filesystem would refuse
     * the write anyway -- the account list is in /System -- but saying so here
     * is clearer than a failure that mentions a path.
     */
    if (!recon_users_may_administer()) {
        out(s, "Only an administrator can change accounts.\n");
        return;
    }

    if (strcasecmp(argv[1], "add") == 0 && argc >= 3) {
        const char *password = argc > 3 ? argv[3] : NULL;
        enum recon_user_role role = (argc > 4 &&
            strcasecmp(argv[4], "administrator") == 0)
            ? RECON_ROLE_ADMINISTRATOR : RECON_ROLE_LIMITED;

        if (!recon_users_create(argv[2], password, role)) {
            out(s, "%s\n", recon_users_last_error());
            return;
        }
        out(s, "Created '%s'.\n", argv[2]);
        return;
    }

    if (strcasecmp(argv[1], "remove") == 0 && argc >= 3) {
        /*
          * `users remove <name> files` takes the folder too. Spelled out
          * rather than a flag, because which of the two you meant is a
          * decision and a flag reads like a detail.
          */
         bool delete_files = (argc > 3 && strcasecmp(argv[3], "files") == 0);

        if (!recon_users_remove(argv[2], delete_files)) {
            out(s, "%s\n", recon_users_last_error());
            return;
        }
        out(s, delete_files
            ? "Removed '%s' and its files.\n"
            : "Removed '%s'. Its files are still there.\n", argv[2]);
        return;
    }

    if (strcasecmp(argv[1], "role") == 0 && argc >= 4) {
        enum recon_user_role role =
            (strcasecmp(argv[3], "administrator") == 0)
            ? RECON_ROLE_ADMINISTRATOR : RECON_ROLE_LIMITED;

        if (!recon_users_set_role(argv[2], role)) {
            out(s, "%s\n", recon_users_last_error());
            return;
        }
        out(s, "'%s' is now %s.\n", argv[2],
            role == RECON_ROLE_ADMINISTRATOR ? "an administrator" : "limited");
        return;
    }

    if (strcasecmp(argv[1], "password") == 0 && argc >= 3) {
        if (!recon_users_set_password(argv[2], argc > 3 ? argv[3] : NULL)) {
            out(s, "%s\n", recon_users_last_error());
            return;
        }
        out(s, "Password %s for '%s'.\n",
            argc > 3 ? "changed" : "removed", argv[2]);
        return;
    }

    if (strcasecmp(argv[1], "signout") == 0) {
        recon_shell_sign_out(s->server->shell);
        out(s, "Signed out.\n");
        return;
    }

    out(s, "Usage: users [add <name> [password] [administrator]]\n");
    out(s, "             [remove <name>] [role <name> <role>]\n");
    out(s, "             [password <name> [password]] [signout]\n");
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

    /*
     * What is installed, not what happens to have a window. Applications are
     * built the first time they are opened, so a list of windows would show
     * nothing on a system nobody has touched yet -- which is exactly when
     * somebody wants to know what is there.
     */
    int count = recon_installed_app_count();

    if (argc < 2) {
        out(s, "Applications:\n");
        for (int i = 0; i < count; i++) {
            struct recon_installed_app app;
            if (!recon_installed_app_at(i, &app)) {
                continue;
            }
            struct recon_appwin *win = recon_installed_app_existing(app.name);
            const char *state = (win == NULL) ? "not started"
                : (recon_appwin_is_open(win) ? "open" : "closed");

            out(s, "  %d  %-20s %-12s %s\n", i, app.name, state,
                app.module[0] != '\0' ? app.module : "built in");
        }
        out(s, "\n'apps <number>' or 'apps <name>' opens one.\n");
        return;
    }

    /* By name as well as by number: a number that moves when something is
     * installed is a number nobody can write down. */
    struct recon_installed_app app;
    bool found = false;

    /*
     * The whole tail is the name, not just the first word. Every application
     * with a space in its name -- File Explorer, Task Manager, Control Panel,
     * which is most of them -- was unopenable by name before this: 'apps File
     * Explorer' looked for an application called "File".
     */
    char label[64];
    size_t used = 0;
    for (int i = 1; i < argc && used < sizeof(label) - 1; i++) {
        int written = snprintf(label + used, sizeof(label) - used, "%s%s",
            i > 1 ? " " : "", argv[i]);
        if (written < 0) {
            break;
        }
        used += (size_t)written;
    }

    if (label[0] >= '0' && label[0] <= '9') {
        int index = atoi(label);
        found = (index >= 0 && index < count) &&
            recon_installed_app_at(index, &app);
    } else {
        for (int i = 0; i < count && !found; i++) {
            if (recon_installed_app_at(i, &app) &&
                    strcasecmp(app.name, label) == 0) {
                found = true;
            }
        }
    }

    if (!found) {
        out(s, "No application called '%s'.\n", label);
        return;
    }

    recon_shell_open_named(shell, app.name);
    out(s, "Opened %s.\n", app.name);
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

/*
 * `net` -- what the network looks like, and whether anything answers.
 *
 * The test does not block. A command's output is written when the command
 * returns, and an answer that arrives later cannot be written into it, so
 * `net reach` starts the test and `net` reports what came back. That is the
 * honest shape given how the interpreter works, and it is also the shape a
 * desktop wants: nothing freezes while a dead host is waited on.
 */
static void cmd_net(struct recon_cmd_session *s, int argc, char **argv) {
    if (argc > 1 && strcasecmp(argv[1], "refresh") == 0) {
        recon_net_refresh();
        out(s, "Read the network again.\n");
        return;
    }

    if (argc > 2 && strcasecmp(argv[1], "resolve") == 0) {
        char address[RECON_NET_ADDR_MAX];
        enum recon_net_result result =
            recon_net_resolve(argv[2], address, sizeof(address));

        if (result == RECON_NET_OK) {
            out(s, "%s is %s\n", argv[2], address);
        } else {
            /* The error already names the host; saying it twice reads as a
             * stutter. */
            out(s, "%s\n", recon_net_last_error());
        }
        return;
    }

    if (argc > 2 && strcasecmp(argv[1], "reach") == 0) {
        int port = (argc > 3) ? atoi(argv[3]) : 80;
        if (!recon_net_probe(argv[2], port, 3000, NULL, NULL)) {
            out(s, "Cannot test that: %s\n", recon_net_last_error());
            return;
        }
        out(s, "Asking %s on port %d. 'net' shows the answer.\n",
            argv[2], port);
        return;
    }

    if (argc > 1 && strcasecmp(argv[1], "apps") == 0) {
        int count = recon_net_allowed_count();
        if (count == 0) {
            out(s, "No application has asked to use the network yet.\n");
            return;
        }
        out(s, "Which applications may use the network:\n\n");
        for (int i = 0; i < count; i++) {
            char name[96];
            bool allowed = false;
            if (recon_net_allowed_at(i, name, sizeof(name), &allowed)) {
                out(s, "  %-24s %s\n", name,
                    allowed ? "allowed" : "not allowed");
            }
        }
        out(s, "\n'net allow <name>' and 'net deny <name>' change these.\n");
        return;
    }

    if (argc > 2 && (strcasecmp(argv[1], "allow") == 0 ||
            strcasecmp(argv[1], "deny") == 0)) {
        bool allow = strcasecmp(argv[1], "allow") == 0;

        /* Join the rest, since application names have spaces in them. */
        char name[96];
        size_t used = 0;
        for (int i = 2; i < argc && used < sizeof(name) - 1; i++) {
            int written = snprintf(name + used, sizeof(name) - used, "%s%s",
                i > 2 ? " " : "", argv[i]);
            if (written < 0) {
                break;
            }
            used += (size_t)written;
        }

        if (!recon_net_set_allowed(name, allow)) {
            out(s, "%s\n", recon_net_last_error());
            return;
        }
        out(s, "'%s' %s use the network.\n", name,
            allow ? "may now" : "may no longer");
        return;
    }

    if (argc > 1 && strcasecmp(argv[1], "interfaces") != 0) {
        out(s, "Usage: net [interfaces|refresh|apps|allow <app>|deny <app>|\n");
        out(s, "            resolve <host>|reach <host> [port]]\n");
        return;
    }

    recon_net_refresh();

    if (argc > 1) {
        int count = recon_net_interface_count();
        for (int i = 0; i < count; i++) {
            struct recon_net_interface interface;
            if (!recon_net_interface_at(i, &interface)) {
                continue;
            }
            out(s, "  %-10s %-24s %s%s%s\n",
                interface.name,
                interface.address[0] != '\0' ? interface.address : "(none)",
                interface.up ? "up" : "down",
                interface.loopback ? ", loopback" : "",
                interface.wireless ? ", wireless" : "");
            out(s, "  %-10s %llu received, %llu sent\n", "",
                interface.rx_bytes, interface.tx_bytes);
        }
        out(s, "\n  %d interface%s\n", count, count == 1 ? "" : "s");
        return;
    }

    /* The summary. */
    out(s, "Machine    %s\n", recon_net_machine_name());
    out(s, "Network    %s\n", recon_net_online() ? "up" : "down");

    int count = recon_net_interface_count();
    for (int i = 0; i < count; i++) {
        struct recon_net_interface interface;
        if (!recon_net_interface_at(i, &interface) || interface.loopback) {
            continue;
        }
        out(s, "%-10s %s%s\n", interface.name,
            interface.address[0] != '\0' ? interface.address : "(no address)",
            interface.up ? "" : "  (down)");
    }

    const char *gateway = recon_net_gateway();
    out(s, "Gateway    %s\n", gateway[0] != '\0' ? gateway : "(none)");

    int servers = recon_net_nameserver_count();
    for (int i = 0; i < servers; i++) {
        out(s, "%-10s %s\n", i == 0 ? "Resolver" : "",
            recon_net_nameserver_at(i));
    }
    if (servers == 0) {
        out(s, "Resolver   (none configured)\n");
    }

    char host[128];
    enum recon_net_result result;
    int elapsed = 0;
    if (recon_net_last_probe(host, sizeof(host), &result, &elapsed)) {
        out(s, "Last test  %s: %s", host, recon_net_result_name(result));
        if (result == RECON_NET_OK) {
            out(s, " in %d ms", elapsed);
        }
        out(s, "\n");
    }

    int running = recon_net_probe_count();
    if (running > 0) {
        out(s, "%-10s %d test%s still running\n", "", running,
            running == 1 ? "" : "s");
    }

    out(s, "\nReconOS has no network stack of its own. This is the host's,\n");
    out(s, "reported through ReconOS -- see include/recon_net.h.\n");
}

/*
 * `get` -- fetch a page, to prove a stream carries bytes both ways.
 *
 * A deliberately small HTTP client: send a request line and two headers,
 * collect what comes back, stop. It parses nothing. The point is not to be a
 * browser -- it is that opening a connection, writing to it, reading from it
 * and having it close is a path somebody has walked end to end, and this is
 * the shortest walk that touches all four.
 *
 * No TLS, so this is http:// only. Most of the web will answer with a
 * redirect to https, which is itself a useful answer: it proves the request
 * arrived and a real server replied.
 */
#define FETCH_MAX 8192

static struct {
    bool running;
    bool have;
    char host[128];
    char path[256];
    char body[FETCH_MAX];
    size_t used;
    enum recon_net_result result;
} g_fetch;

static void fetch_opened(void *user, struct recon_net_stream *stream) {
    (void)user;

    char request[512];
    snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: ReconOS/" RECONOS_VERSION "\r\n"
        "Connection: close\r\n"
        "\r\n",
        g_fetch.path, g_fetch.host);

    recon_net_stream_send_text(stream, request);
}

static void fetch_received(void *user, struct recon_net_stream *stream,
        const char *bytes, size_t length) {
    (void)user;
    (void)stream;

    size_t room = sizeof(g_fetch.body) - 1 - g_fetch.used;
    if (room == 0) {
        return;   /* Enough. This is a demonstration, not a download. */
    }
    if (length > room) {
        length = room;
    }
    memcpy(g_fetch.body + g_fetch.used, bytes, length);
    g_fetch.used += length;
    g_fetch.body[g_fetch.used] = '\0';
}

static void fetch_closed(void *user, struct recon_net_stream *stream,
        enum recon_net_result reason) {
    (void)user;
    (void)stream;

    g_fetch.running = false;
    g_fetch.have = true;
    g_fetch.result = reason;
}

static void cmd_get(struct recon_cmd_session *s, int argc, char **argv) {
    if (argc < 2) {
        /* No arguments means "show me what came back", because the answer to
         * a fetch arrives after the command that started it has returned. */
        if (g_fetch.running) {
            out(s, "Still fetching %s%s.\n", g_fetch.host, g_fetch.path);
            return;
        }
        if (!g_fetch.have) {
            out(s, "Usage: get <host>[:port] [path]\n");
            return;
        }

        out(s, "%s%s: %s, %zu bytes\n\n", g_fetch.host, g_fetch.path,
            recon_net_result_name(g_fetch.result), g_fetch.used);
        out(s, "%s", g_fetch.body);
        if (g_fetch.used > 0 && g_fetch.body[g_fetch.used - 1] != '\n') {
            out(s, "\n");
        }
        return;
    }

    if (g_fetch.running) {
        out(s, "One at a time. %s%s is still going.\n",
            g_fetch.host, g_fetch.path);
        return;
    }

    /* host[:port] */
    char host[128];
    snprintf(host, sizeof(host), "%s", argv[1]);

    int port = 80;
    char *colon = strrchr(host, ':');
    if (colon != NULL) {
        *colon = '\0';
        port = atoi(colon + 1);
        if (port <= 0) {
            port = 80;
        }
    }

    snprintf(g_fetch.host, sizeof(g_fetch.host), "%s", host);
    snprintf(g_fetch.path, sizeof(g_fetch.path), "%s",
        argc > 2 ? argv[2] : "/");
    g_fetch.used = 0;
    g_fetch.body[0] = '\0';
    g_fetch.have = false;

    static const struct recon_net_stream_handlers HANDLERS = {
        .opened = fetch_opened,
        .received = fetch_received,
        .closed = fetch_closed,
    };

    /* In the Terminal's name, so the permission is the Terminal's and shows
     * up under that name in the Control Panel. */
    if (recon_net_stream_open("Terminal", g_fetch.host, port, &HANDLERS,
            NULL) == NULL) {
        out(s, "%s\n", recon_net_last_error());
        if (!recon_net_may_use("Terminal")) {
            out(s, "Allow it with: net allow Terminal\n");
        }
        return;
    }

    g_fetch.running = true;
    out(s, "Fetching %s%s. 'get' with no arguments shows the answer.\n",
        g_fetch.host, g_fetch.path);
}

/*
 * `capture` -- a picture of the screen.
 *
 * Asynchronous like everything else that has to wait for something: a
 * compositor keeps no copy of what it drew, so this asks for one more frame
 * and reads that. `capture` with no arguments says where the last one went.
 */
static void cmd_capture(struct recon_cmd_session *s, int argc, char **argv) {
    if (argc < 2) {
        if (recon_capture_pending()) {
            out(s, "Waiting for a frame.\n");
            return;
        }

        char path[RECON_PATH_MAX];
        bool ok = false;
        if (!recon_capture_last(path, sizeof(path), &ok)) {
            out(s, "Usage: capture [path]\n");
            out(s, "Nothing has been captured yet.\n");
            return;
        }

        if (ok) {
            struct recon_dirent info;
            if (recon_fs_stat("/", path, &info)) {
                out(s, "%s  (%zu bytes)\n", path, info.size);
            } else {
                out(s, "%s\n", path);
            }
        } else {
            out(s, "The last capture failed: %s\n",
                recon_capture_last_error());
        }
        return;
    }

    const char *path = strcasecmp(argv[1], "here") == 0 ? NULL : argv[1];
    if (!recon_capture_request(s->server, path)) {
        out(s, "%s\n", recon_capture_last_error());
        return;
    }
    out(s, "Capturing. 'capture' with no arguments says where it went.\n");
}

/*
 * `install` and `uninstall` -- putting a program into the system and taking
 * it out again.
 *
 * Separate from `modules load`, which runs something already in place. This
 * copies it in first, so it is still there next time.
 */
static void cmd_install(struct recon_cmd_session *s, int argc, char **argv) {
    if (argc < 2) {
        out(s, "Usage: install <path to a %s or %s>\n",
            RECON_APP_EXT, RECON_MODULE_EXT);
        return;
    }

    /* Resolved against where the terminal is, so a relative path works the
     * way it does for every other command here. */
    char host[RECON_PATH_MAX];
    char canonical[RECON_PATH_MAX];
    if (!recon_fs_resolve(s->cwd, argv[1], host, sizeof(host), canonical,
            sizeof(canonical))) {
        out(s, "%s\n", recon_fs_last_error());
        return;
    }

    /*
     * A folder is a package; a file is a bare module.
     *
     * The same verb for both, because from where somebody is standing they
     * are the same act -- "put this program into the system" -- and making
     * them two commands would mean knowing which kind of thing you have
     * before you can install it.
     */
    struct recon_dirent entry;
    if (recon_fs_stat("/", canonical, &entry) &&
            entry.kind == RECON_FILE_DIRECTORY) {
        struct recon_package_info info;
        if (!recon_package_install(canonical)) {
            out(s, "%s\n", recon_package_last_error());
            return;
        }
        if (recon_package_read(canonical, &info)) {
            out(s, "Installed %s %s.\n", info.name, info.version);
        } else {
            out(s, "Installed.\n");
        }
        recon_shell_restyle(s->server->shell);
        return;
    }

    if (!recon_modules_install(canonical)) {
        out(s, "%s\n", recon_modules_last_error());
        return;
    }
    out(s, "Installed. It is loaded and will load again on every start.\n");
}

/* What is installed as a package, with what it says about itself. */
static void cmd_packages(struct recon_cmd_session *s, int argc, char **argv) {
    (void)argc; (void)argv;

    int count = recon_package_count();
    if (count == 0) {
        out(s, "Nothing is installed as a package.\n\n"
            "A package is a folder with a " RECON_PACKAGE_MANIFEST " in it.\n"
            "'install <folder>' puts one in; 'uninstall <name>' takes it out.\n");
        return;
    }

    for (int i = 0; i < count; i++) {
        struct recon_package_info info;
        if (!recon_package_at(i, &info)) {
            continue;
        }
        out(s, "  %-16s %-10s %s\n", info.name, info.version,
            info.description);
    }
    out(s, "\n  %d package%s\n", count, count == 1 ? "" : "s");
}

static void cmd_uninstall(struct recon_cmd_session *s, int argc, char **argv) {
    if (argc < 2) {
        out(s, "Usage: uninstall <module name>\n");
        out(s, "'modules' lists what is installed.\n");
        return;
    }

    /*
     * A package first, because a package's module is only part of what its
     * install placed. Removing the module alone would leave its icon and its
     * receipt behind -- and the receipt would then claim a program is
     * installed which is not.
     */
    if (recon_package_installed(argv[1])) {
        if (!recon_package_uninstall(argv[1])) {
            out(s, "%s\n", recon_package_last_error());
            return;
        }
        out(s, "Removed '%s' and everything it installed.\n", argv[1]);
        recon_shell_restyle(s->server->shell);
        return;
    }

    char name[64];
    size_t used = 0;
    for (int i = 1; i < argc && used < sizeof(name) - 1; i++) {
        int written = snprintf(name + used, sizeof(name) - used, "%s%s",
            i > 1 ? " " : "", argv[i]);
        if (written < 0) {
            break;
        }
        used += (size_t)written;
    }

    if (!recon_modules_uninstall(name)) {
        out(s, "%s\n", recon_modules_last_error());
        return;
    }
    out(s, "Removed '%s'.\n", name);
}

/* `wallpaper` -- what is behind everything, and what else there is. */
static void cmd_wallpaper(struct recon_cmd_session *s, int argc, char **argv) {
    if (argc < 2) {
        out(s, "Showing: %s\n\n", recon_wallpaper_current());

        int count = recon_wallpaper_count();
        for (int i = 0; i < count; i++) {
            char name[96];
            if (recon_wallpaper_at(i, name, sizeof(name))) {
                out(s, "  %s\n", name);
            }
        }
        out(s, "\n'wallpaper <name>' chooses one. 'wallpaper theme' follows "
            "the skin.\n");
        return;
    }

    /* Joined, because these have spaces in their names. */
    char name[96];
    size_t used = 0;
    for (int i = 1; i < argc && used < sizeof(name) - 1; i++) {
        int written = snprintf(name + used, sizeof(name) - used, "%s%s",
            i > 1 ? " " : "", argv[i]);
        if (written < 0) {
            break;
        }
        used += (size_t)written;
    }

    bool follow = strcasecmp(name, "theme") == 0;
    if (!recon_wallpaper_set(follow ? NULL : name)) {
        out(s, "There is no wallpaper called '%s'.\n", name);
        return;
    }

    recon_background_reload(s->server);
    out(s, follow ? "Following the skin: %s\n" : "Showing %s\n",
        recon_wallpaper_current());
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
    { "spawn",    "spawn [command]",       "Start a Wayland client (testing)",  cmd_spawn },
    { "errors",   "errors [<code>|log]",   "What an error code means",          cmd_errors },
    { "firewall", "firewall [...]",        "What may open and be opened",       cmd_firewall },
    { "services", "services [...]",        "The parts of ReconOS that run",     cmd_services },
    { "blank", "blank [now|wake]",         "Blanking the screen when idle",     cmd_blank },
    { "remote",   "remote [on|off|key|port <n>]",
                                           "Reaching ReconOS from elsewhere",  cmd_remote },
    { "raise",    "raise <code>",          "Report an error on purpose (testing)",
                                                                                cmd_raise },
    { "bin",      "bin [<name>|list|restore <name>|purge <name>|empty]",
                                               "The Recycle Bin",                   cmd_bin },
    { "rename",   "rename <name> <new>",   "Rename a file or folder",           cmd_rename },
    { "move",     "move <name> <dest>",    "Move a file or folder",             cmd_move },
    { "copy",     "copy <name> <dest>",    "Copy a file or folder",             cmd_copy },
    { "windows",  "windows",               "List open windows",                 cmd_windows },
    { "apps",     "apps [number]",         "List or open built-in applications", cmd_apps },
    { "modules",  "modules [load|unload]", "List, load or unload modules",      cmd_modules },
    { "install",  "install <path>",        "Put a program into the system",      cmd_install },
    { "uninstall","uninstall <name>",      "Take a program out again",           cmd_uninstall },
    { "packages", "packages",              "What is installed as a package",     cmd_packages },
    { "reg",      "reg <hive> <action>",   "Read or change stored settings",    cmd_reg },
    { "theme",    "theme [name|roles|install|remove]",
                                       "List skins, put one on, add or remove",
                                                                          cmd_theme },
    { "wallpaper","wallpaper [name]",      "What is behind everything",          cmd_wallpaper },
    { "session",  "session",               "What the login screen shows",       cmd_session },
    { "users",    "users [action] ...",    "List or change accounts",           cmd_users },
    { "access",   "access [setting] [n]",  "Reading settings: spacing, font",   cmd_access },
    { "mem",      "mem",                   "Show memory in use",                cmd_mem },
    { "net",      "net [action] ...",      "The network, and whether it answers", cmd_net },
    { "get",      "get <host> [path]",     "Fetch a page over HTTP",             cmd_get },
    { "capture",  "capture [path]",        "Save a picture of the screen",       cmd_capture },
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

    /*
     * Then whatever modules added. Built-in names win, so a module cannot
     * take over `del` or `shutdown` by registering one -- registration
     * already refuses a duplicate, and this makes the rule hold even if that
     * check is ever loosened.
     */
    int extra = recon_module_command_count();
    for (int i = 0; i < extra; i++) {
        struct recon_command_registration command;
        if (!recon_module_command_at(i, &command)) {
            continue;
        }
        if (strcasecmp(command.name, argv[0]) == 0) {
            command.run(session, argc, argv);
            return session->output;
        }
    }

    out(session, "'%s' is not a ReconOS command. Type 'help' for a list.\n", argv[0]);
    return session->output;
}
