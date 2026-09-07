/*
 * Error codes. See include/recon_error.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "ReconOS.h"
#include "recon_error.h"
#include "recon_fs.h"

/* --- The table --- */

#define RECON_ERROR(area, number, level, summary, detail) \
    { "VT-" #area #number, RECON_ERROR_##level, summary, detail },

static const struct recon_error_info ERRORS[] = {
#include "recon_errors.def"
};

#undef RECON_ERROR

#define ERROR_COUNT ((int)(sizeof(ERRORS) / sizeof(ERRORS[0])))

/*
 * The enumeration and the table are built from the same file, so they cannot
 * drift -- but a mistake in the macro could still put them out of step, and
 * that would mean every code reading as the one next to it.
 */
_Static_assert(ERROR_COUNT == RECON_ERROR_COUNT,
    "the error table and the error enumeration disagree");

static const struct {
    char letter;
    const char *name;
} AREAS[] = {
    { 'A', "Startup and shutdown" },
    { 'B', "Storage and the filesystem" },
    { 'C', "Accounts and signing in" },
    { 'D', "Display, windows and drawing" },
    { 'E', "Programs, modules and packages" },
    { 'F', "Network" },
    { 'G', "Firewall and remote access" },
    { 'H', "Settings and the registry" },
    { 'J', "Applications" },
    { 'K', "Input" },
    { 'L', "Skins, icons and wallpapers" },
    { 'M', "Help and documentation" },
};

int recon_error_count(void) {
    return ERROR_COUNT;
}

const struct recon_error_info *recon_error_at(enum recon_error_code code) {
    if (code < 0 || code >= ERROR_COUNT) {
        return NULL;
    }
    return &ERRORS[code];
}

const struct recon_error_info *recon_error_find(const char *code) {
    if (code == NULL) {
        return NULL;
    }

    /* The "VT-" is optional and the case is not checked, because this is a
     * code somebody is typing back from a screen or a note. */
    while (*code == ' ') {
        code++;
    }
    if (strncasecmp(code, "VT-", 3) == 0) {
        code += 3;
    } else if (strncasecmp(code, "VT", 2) == 0) {
        code += 2;
    }

    for (int i = 0; i < ERROR_COUNT; i++) {
        if (strcasecmp(ERRORS[i].code + 3, code) == 0) {
            return &ERRORS[i];
        }
    }
    return NULL;
}

const char *recon_error_area_name(char letter) {
    if (letter >= 'a' && letter <= 'z') {
        letter = (char)(letter - 'a' + 'A');
    }
    for (size_t i = 0; i < sizeof(AREAS) / sizeof(AREAS[0]); i++) {
        if (AREAS[i].letter == letter) {
            return AREAS[i].name;
        }
    }
    return NULL;
}

/* --- Writing it down --- */

static const char *level_name(enum recon_error_level level) {
    switch (level) {
    case RECON_ERROR_NOTE:  return "note";
    case RECON_ERROR_FAULT: return "fault";
    case RECON_ERROR_STOP:  return "STOP";
    default:                return "?";
    }
}

/*
 * A timestamp, for the log.
 *
 * The host's clock, because ReconOS has no clock of its own -- and said in a
 * fixed order so a log can be read by eye and sorted by machine without
 * knowing where it came from.
 */
static void stamp(char *out, size_t size) {
    time_t now = time(NULL);
    struct tm parts;

    if (localtime_r(&now, &parts) == NULL) {
        recon_text_copy(out, size, "(no clock)");
        return;
    }
    strftime(out, size, "%Y-%m-%d %H:%M:%S", &parts);
}

static void append_log(const struct recon_error_info *info,
        const char *detail) {
    char when[32];
    stamp(when, sizeof(when));

    char line[512];
    int length = snprintf(line, sizeof(line), "%s  %-8s %s  %s%s%s\n",
        when, level_name(info->level), info->code, info->summary,
        (detail != NULL && detail[0] != '\0') ? " -- " : "",
        (detail != NULL) ? detail : "");

    if (length <= 0) {
        return;
    }
    if ((size_t)length >= sizeof(line)) {
        length = (int)sizeof(line) - 1;
    }

    recon_fs_mkdir("/", RECON_DIR_LOGS);
    recon_fs_append("/", RECON_ERROR_LOG, line, (size_t)length);
}

/*
 * A stop is written to its own file as well as to the log.
 *
 * Written *before* the screen is drawn, because the screen is the part most
 * likely to fail when what failed is the drawing. A file on disk survives a
 * system that cannot draw its own error, and is what lets the next start say
 * what happened to the last one.
 */
static void write_last_stop(const struct recon_error_info *info,
        const char *detail) {
    char when[32];
    stamp(when, sizeof(when));

    char text[1024];
    int length = snprintf(text, sizeof(text), "%s\n%s\n%s\n",
        info->code, when, (detail != NULL) ? detail : "");
    if (length <= 0) {
        return;
    }
    if ((size_t)length >= sizeof(text)) {
        length = (int)sizeof(text) - 1;
    }

    recon_fs_mkdir("/", RECON_DIR_LOGS);
    recon_fs_write("/", RECON_ERROR_LAST, text, (size_t)length);
}

/*
 * Who draws a stop, and for whom. Both set together, because a screen
 * without the server it draws on is half an answer.
 */
static recon_error_screen_fn g_screen;
static struct recon_server *g_screen_server;

void recon_error_set_screen(struct recon_server *server,
        recon_error_screen_fn show) {
    g_screen_server = server;
    g_screen = show;
}

/*
 * True while a fault is being written down.
 *
 * Recording one goes through the filesystem, and the filesystem is a place
 * faults come from -- so a raise can reach a raise. Without this that is
 * unbounded: a path fault reported by writing to a path that also faults
 * would recurse until the stack ran out, and the last thing the machine did
 * would be to lose the record of what went wrong.
 *
 * The inner one is dropped rather than queued. It is a fault *about reporting
 * a fault*, so the outer one is the news; and a queue here would be state that
 * only exists during a failure, which is the worst kind to have to reason
 * about.
 *
 * Not thread-safe, and does not need to be: ReconOS raises faults from the
 * event loop, which is one thread. That is worth saying rather than assuming,
 * because the day it is not true this is where it breaks.
 */
static bool g_reporting;

void recon_error_raise(struct recon_server *server,
        enum recon_error_code code, const char *detail) {
    const struct recon_error_info *info = recon_error_at(code);
    if (info == NULL) {
        return;
    }

    if (g_reporting) {
        /* Straight to stderr, which needs nothing of ours. Something has gone
         * wrong twice and losing the second one silently would be worse than
         * a line nobody was watching for. */
        fprintf(stderr, "ReconOS: %s while reporting another fault: %s\n",
            info->code, (detail != NULL) ? detail : "");
        return;
    }
    g_reporting = true;

    append_log(info, detail);

    if (info->level != RECON_ERROR_STOP) {
        g_reporting = false;
        return;
    }

    /*
     * A stop does not clear the flag. Nothing after this returns to ordinary
     * running -- the screen is drawn and the session ends -- and a fault
     * raised from inside the error screen would be one more thing going wrong
     * on top of the thing that already stopped the system.
     */
    write_last_stop(info, detail);

    if (g_screen != NULL) {
        /*
         * The caller's server if it gave one, otherwise whoever registered.
         * Most callers are deep enough in the system that they do not hold a
         * server pointer, and requiring one would mean threading it through
         * a dozen files for the sake of the one case that needs it.
         */
        g_screen((server != NULL) ? server : g_screen_server, info, detail);
        return;
    }

    fprintf(stderr, "\nReconOS has stopped.\n\n  %s  %s\n\n%s\n%s%s\n\n",
        info->code, info->summary, info->detail,
        (detail != NULL && detail[0] != '\0') ? "\n" : "",
        (detail != NULL) ? detail : "");
    exit(1);
}

void recon_error_raisef(struct recon_server *server,
        enum recon_error_code code, const char *format, ...) {
    char detail[512];

    va_list args;
    va_start(args, format);
    vsnprintf(detail, sizeof(detail), format, args);
    va_end(args);

    recon_error_raise(server, code, detail);
}

/* --- What happened last time --- */

bool recon_error_begin_run(void) {
    /*
     * Read first, then written, and in that order for the obvious reason: the
     * write is what makes the read say yes next time.
     */
    size_t length = 0;
    char *previous = recon_fs_read("/", RECON_ERROR_RUNNING, &length);
    bool ended_badly = (previous != NULL);

    char was[256] = "";
    if (previous != NULL) {
        snprintf(was, sizeof(was), "%s", previous);
        /* One line, and it may not have one. */
        char *nl = strchr(was, '\n');
        if (nl != NULL) {
            *nl = '\0';
        }
        free(previous);
    }

    char when[32];
    stamp(when, sizeof(when));

    char text[256];
    int written = snprintf(text, sizeof(text),
        "started %s, ReconOS " RECONOS_VERSION "\n", when);
    if (written > 0) {
        recon_fs_mkdir("/", RECON_DIR_LOGS);
        recon_fs_write("/", RECON_ERROR_RUNNING, text, (size_t)written);
    }

    if (!ended_badly) {
        return false;
    }

    /*
     * Only when nothing else already explained it.
     *
     * A caught crash writes its own record naming the actual fault, and that
     * is a better answer than "something happened" -- reporting both would put
     * two screens in front of somebody for one event, and the vaguer one
     * second. So this defers: it says what it knows only when nothing else
     * knows more.
     *
     * Read without taking, because the screen that shows it is the one that
     * takes it, several seconds from now.
     */
    size_t stop_length = 0;
    char *stop = recon_fs_read("/", RECON_ERROR_LAST, &stop_length);
    if (stop != NULL) {
        free(stop);
        return false;
    }

    /* 448, because `was` is 256 of it and the sentence around it is 96. The
     * compiler worked that out before anything ran; 320 was a guess. */
    char detail[448];
    snprintf(detail, sizeof(detail), "the run that %s did not shut down; "
        "nothing was caught, so it was a power cut, a kill, or a lock-up",
        was[0] != '\0' ? was : "was running");

    recon_error_raise(NULL, RECON_ERR_A005, detail);

    /*
     * And into the last-stop file as well, which raising does not do for a
     * fault -- only a STOP writes there, because only a STOP has ended the
     * system.
     *
     * This one is written by hand because it is the one fault that is *about*
     * the last run rather than about this one, and the screen that reports the
     * last run reads that file. Making A-005 a STOP instead would be wrong in
     * the other direction: nothing is stopping, the machine is starting up
     * perfectly well, and it has one thing to say about yesterday.
     */
    write_last_stop(recon_error_at(RECON_ERR_A005), detail);
    return true;
}

void recon_error_end_run(void) {
    recon_fs_remove("/", RECON_ERROR_RUNNING);
}

bool recon_error_take_last(char *code_out, size_t size, char *detail_out,
        size_t detail_size) {
    if (code_out != NULL && size > 0) {
        code_out[0] = '\0';
    }
    if (detail_out != NULL && detail_size > 0) {
        detail_out[0] = '\0';
    }

    size_t length = 0;
    char *text = recon_fs_read("/", RECON_ERROR_LAST, &length);
    if (text == NULL) {
        return false;
    }

    /* Three lines: the code, when, and whatever the detail was. */
    char *save = NULL;
    char *code = strtok_r(text, "\n", &save);
    char *when = (code != NULL) ? strtok_r(NULL, "\n", &save) : NULL;
    char *detail = (when != NULL) ? strtok_r(NULL, "\n", &save) : NULL;

    if (code != NULL && code_out != NULL) {
        recon_text_copy(code_out, size, code);
    }
    if (detail != NULL && detail_out != NULL) {
        recon_text_copy(detail_out, detail_size, detail);
    }

    free(text);

    /*
     * Taken rather than read: the same stop is reported once. Leaving it
     * would mean a machine that stopped in March saying so every morning
     * until somebody deleted a file they do not know about.
     */
    recon_fs_remove("/", RECON_ERROR_LAST);
    return code != NULL;
}

/* --- Crashes --- */

/*
 * What a signal handler may do is a short list, and none of it involves the
 * compositor. So this writes with write(2) -- which is on the list -- and
 * nothing else, then lets the default handler finish the job.
 *
 * The path is worked out ahead of time, because resolving one is not on the
 * list either.
 */
static char g_crash_path[RECON_PATH_MAX];

static void write_crash(int signal_number) {
    if (g_crash_path[0] == '\0') {
        _exit(128 + signal_number);
    }

    const char *code;
    switch (signal_number) {
    case SIGSEGV: code = "VT-A005\n(no time)\nThe system stopped at a bad "
                         "memory access (SIGSEGV).\n"; break;
    case SIGBUS:  code = "VT-A005\n(no time)\nThe system stopped at a bus "
                         "error (SIGBUS).\n"; break;
    case SIGFPE:  code = "VT-A005\n(no time)\nThe system stopped at an "
                         "arithmetic fault (SIGFPE).\n"; break;
    case SIGILL:  code = "VT-A005\n(no time)\nThe system stopped at an "
                         "illegal instruction (SIGILL).\n"; break;
    default:      code = "VT-A005\n(no time)\nThe system stopped "
                         "unexpectedly.\n"; break;
    }

    int fd = open(g_crash_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        ssize_t ignored = write(fd, code, strlen(code));
        (void)ignored;
        close(fd);
    }

    /* Back to the default, and let it happen properly: a core file is worth
     * more to whoever debugs this than a tidy exit. */
    signal(signal_number, SIG_DFL);
    raise(signal_number);
}

void recon_error_catch_crashes(void) {
    if (!recon_fs_resolve(NULL, RECON_ERROR_LAST, g_crash_path,
            sizeof(g_crash_path), NULL, 0)) {
        g_crash_path[0] = '\0';
    }

    static const int SIGNALS[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL };
    for (size_t i = 0; i < sizeof(SIGNALS) / sizeof(SIGNALS[0]); i++) {
        signal(SIGNALS[i], write_crash);
    }
}
