/*
 * Error codes, and the screen that shows one.
 *
 * When something goes wrong, a person needs two things: a name for it they can
 * write down and look up, and a sentence saying what it means. "Something went
 * wrong" gives them neither, and a stack trace gives them the wrong one.
 *
 * --- The shape of a code ---
 *
 *   VT-A001
 *   ^^ ^ ^^^
 *   |  | +--- which fault, 001 upward within that area
 *   |  +----- which area of the system
 *   +-------- Void Tower
 *
 * **The letter is the area, not the severity.** The same area produces faults
 * of every kind -- the filesystem can fail to read a file (recoverable) and
 * fail to open at all (not) -- so severity in the letter would mean the same
 * subsystem scattered across the alphabet. Worse, a code would have to change
 * if a fault were ever reclassified, and a code that changes is a code nobody
 * can look up. Severity is a field on the entry instead.
 *
 * **I and O are not area letters.** This is a code somebody reads off a screen
 * and types into a search box, and in that setting I is 1 and O is 0. Losing
 * two letters costs nothing; a support answer about the wrong fault costs
 * everything.
 *
 * **A hundred per letter.** When an area fills, it continues at a second
 * letter reserved for it rather than renumbering, because the old codes are
 * already written down somewhere. Twenty-four letters at a hundred each is
 * two thousand four hundred, which is a long way past anywhere this is going.
 *
 * **Numbers are assigned in order within an area**, and grouped loosely by
 * kind where an area has enough entries to want it. Not enforced: a rule that
 * forces renumbering when a group fills is a rule that breaks the one
 * property a code has to have.
 *
 * --- Where the list lives ---
 *
 * In recon_errors.def, once. This file builds the table from it, the terminal
 * looks a code up in it, the error screen reads its description out of it, and
 * scripts/make-errors.sh turns it into docs/ERRORS.md for anybody holding a
 * code and not a machine. One list, so a code cannot mean one thing on screen
 * and another in the documentation.
 */

#ifndef RECON_ERROR_H
#define RECON_ERROR_H

#include <stdbool.h>
#include <stddef.h>

struct recon_server;

/*
 * How bad it is.
 *
 * The severity decides what happens, not only how it reads: STOP draws the
 * error screen and ends the session, FAULT is reported where it happened, NOTE
 * is written down and nothing else.
 */
enum recon_error_level {
    /* Recorded. Nothing broke; this is a fact worth having later. */
    RECON_ERROR_NOTE,
    /* Something failed and the system carried on without it. */
    RECON_ERROR_FAULT,
    /* The system cannot continue. The error screen, then out. */
    RECON_ERROR_STOP,
};

/* The codes themselves, as an enumeration, so a caller names one rather than
 * writing a string that nothing checks. */
#define RECON_ERROR(area, number, level, summary, detail) \
    RECON_ERR_##area##number,

enum recon_error_code {
#include "recon_errors.def"
    RECON_ERROR_COUNT,
};

#undef RECON_ERROR

struct recon_error_info {
    /* "VT-A001". */
    char code[12];
    enum recon_error_level level;
    /* A few words: what went wrong. */
    const char *summary;
    /* A sentence or two: what it means and what can be done. */
    const char *detail;
};

/* What a code says. NULL if there is no such code. */
const struct recon_error_info *recon_error_at(enum recon_error_code code);

/* The same, by the text somebody read off a screen. Case-insensitive, and the
 * "VT-" is optional, because that is how people type it. */
const struct recon_error_info *recon_error_find(const char *code);

int recon_error_count(void);

/* The name of an area letter, for the documentation and for `errors`. */
const char *recon_error_area_name(char letter);

/* --- Reporting --- */

/*
 * Record that something went wrong.
 *
 * `detail` is what this particular occurrence adds -- a path, a name, the
 * message from underneath -- and may be NULL. It is not the code's own
 * description, which is already known.
 *
 * A NOTE or a FAULT is written to the log and returns. A STOP does not
 * return: it draws the error screen and ends the session.
 */
void recon_error_raise(struct recon_server *server, enum recon_error_code code,
    const char *detail);

/* Formatted, for the common case where the detail is built from something. */
void recon_error_raisef(struct recon_server *server,
    enum recon_error_code code, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

/*
 * Who draws the screen that says the system has stopped.
 *
 * Registered rather than called directly, so this layer does not depend on
 * the thing that draws. That is not tidiness: recording a fault has to work
 * in places the compositor does not exist -- a test binary, a module built on
 * its own, and the moment before the display comes up, which is exactly when
 * the worst faults happen.
 *
 * With nothing registered, a stop writes its record, says so on stderr and
 * ends the process. That is the honest behaviour for "there is nothing to
 * draw with", which is a real state and not a failure of this design: "no
 * usable font" is a stop, and it is a stop that happens before there is any
 * way to say so on a screen.
 */
typedef void (*recon_error_screen_fn)(struct recon_server *server,
    const struct recon_error_info *info, const char *detail);

void recon_error_set_screen(struct recon_server *server,
    recon_error_screen_fn show);

/* --- What happened last time --- */

/*
 * Where a stop is written down before the screen is drawn.
 *
 * Written first, because the screen is the part most likely to fail when the
 * thing that failed is the drawing. A file on disk survives a system that
 * cannot draw its own error, and is what makes the next start able to say what
 * happened to the last one.
 */
#define RECON_ERROR_LOG "/System/Logs/errors.txt"
#define RECON_ERROR_LAST "/System/Logs/last-stop.txt"

/*
 * Did the last run end badly, and with what?
 *
 * True when there is a record from a previous start, with the code written
 * into `code_out`. Reading it clears it, so the same fault is reported once.
 */
bool recon_error_take_last(char *code_out, size_t size, char *detail_out,
    size_t detail_size);

/*
 * Install handlers for the signals that mean a crash.
 *
 * A crash cannot be drawn reliably -- the process is already in a state where
 * the drawing may be what is broken -- so the handler writes the record and
 * gets out of the way. The screen appears on the next start, which is a start
 * that works.
 */
void recon_error_catch_crashes(void);

#endif
