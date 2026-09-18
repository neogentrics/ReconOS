/*
 * The server's configuration, read from a file instead of compiled in.
 *
 * --- Why this exists ---
 *
 * `server/README.md` opens by saying that a role is meant to be
 * **configuration, not a build**, and then says plainly that today it is a
 * build. This is the first piece of that sentence to stop being true.
 *
 * The immediate reason is narrower. 0.24.0 gave this server name-based virtual
 * hosts: a chain of sites, the first whose name matches a request's `Host`
 * answers. Nothing could add a second site to that chain without editing
 * `server_init.c` and rebuilding the kernel, and **a virtual host you cannot
 * configure is not a virtual host**, it is a code path with a suite.
 *
 * --- What it does not do, on purpose ---
 *
 * It does not configure handlers. A route runs a C function, and a file cannot
 * name one that was not linked in -- there is no dynamic loading here and
 * pretending otherwise would mean a configuration key whose values are a fixed
 * list the file cannot see. So a configured site serves **files**, which is
 * what a virtual host means for everything that is not this machine's own
 * console.
 *
 * The console stays a built-in site and stays last in the chain, with no name,
 * which means it answers to anything nothing else claimed. That is what this
 * server did before configuration existed, and a machine that stops answering
 * on its own address because somebody added a site is a machine somebody has
 * locked themselves out of.
 *
 * --- The rule this file follows ---
 *
 * **Refuse the whole file, name the line, and keep running.**
 *
 * *Refuse the whole file*, because a configuration half-applied is a machine in
 * a state nobody described. Every refusal below stops at the first fault and
 * reports it; nothing is applied unless all of it parsed.
 *
 * *Name the line*, because the fault is somebody's typing and the only useful
 * answer names where. A parser that says `bad configuration` has moved the work
 * back to the person.
 *
 * *And keep running* -- with the built-in console site, saying loudly that the
 * file was refused and why. This is the one place here that does not refuse
 * outright, and the reason is worth writing down rather than leaving as an
 * inconsistency: the alternative is a machine that will not serve, whose only
 * repair route is the web console it is not serving. A typo in a site's root
 * should not cost physical access. The console says what it is running and why,
 * on the screen and in the log, so nothing is running silently.
 *
 * Every other ambiguity is a refusal, in the same shape as `http.h`: an unknown
 * key, a key given twice, two sites with one name, a value that does not fit.
 * **A configuration file that is read leniently is read differently by the next
 * version**, which is the same fault as two HTTP parsers disagreeing about one
 * request, arriving a year later instead of a millisecond later.
 */

#ifndef RECON_SERVER_CONFIG_H
#define RECON_SERVER_CONFIG_H

#include <stddef.h>

/* Where the file lives when nobody says otherwise. Under `/System`, beside the
 * log directory, because it is the system's rather than a user's -- and
 * **outside the web root**, because a configuration file that can be fetched
 * over HTTP is a configuration file that has been fetched over HTTP. */
#define CONFIG_PATH "/System/server.conf"

/* Bounds. Each is a refusal rather than a truncation, for the reason `http.h`
 * gives about targets: a truncated path is a different path, and a truncated
 * host name is a different machine. */
#define CONFIG_LINE_MAX     256
#define CONFIG_VALUE_MAX    192
#define CONFIG_SITES_MAX      8
#define CONFIG_FILE_MAX    8192

/*
 * A verdict. Zero is a file with exactly one meaning.
 *
 * Every one of these carries the line it happened on, in `struct config`, so
 * the console can say *line 12: two sites are called shop.example* rather than
 * *bad configuration*.
 */
#define CONFIG_OK              0
#define CONFIG_ELINE_LONG    (-1)  /* a line over CONFIG_LINE_MAX */
#define CONFIG_EUNKNOWN      (-2)  /* a key this version does not have */
#define CONFIG_EREPEATED     (-3)  /* the same key twice in one place */
#define CONFIG_EMISPLACED    (-4)  /* a key that is real, in the wrong place */
#define CONFIG_EVALUE        (-5)  /* a value that is empty, long, or malformed */
#define CONFIG_ESECTION      (-6)  /* a `[...]` header that does not parse */
#define CONFIG_ETOOMANY      (-7)  /* more sites than CONFIG_SITES_MAX */
#define CONFIG_EDUPLICATE    (-8)  /* two sites with the same name */
#define CONFIG_EINCOMPLETE   (-9)  /* a site missing something it must have */
#define CONFIG_EFILE_LONG   (-10)  /* the file is over CONFIG_FILE_MAX */

/*
 * One configured site.
 *
 * A name and a document root, which is the whole of what a file-serving
 * virtual host is. `index` is the file a directory answers with -- never a
 * listing, the same rule `files.c` already holds.
 */
struct config_site {
	char host[CONFIG_VALUE_MAX];
	char root[CONFIG_VALUE_MAX];
	char index[CONFIG_VALUE_MAX];
	unsigned line;			/* where the section started, for errors */
};

/*
 * Everything a file can say.
 *
 * `has_*` accompanies the settings that have a meaningful default, because
 * *absent* and *set to the same value the default happens to be* are different
 * facts: one of them stops being true the day the default changes, and the
 * console should be able to say which of the two it is looking at.
 */
struct config {
	char name[CONFIG_VALUE_MAX];	/* the machine's name */
	int  has_name;

	unsigned port;			/* what the web server listens on */
	int  has_port;

	char resolver[CONFIG_VALUE_MAX];/* the DNS server, as dotted quad */
	int  has_resolver;

	char clock[CONFIG_VALUE_MAX];	/* the NTP server, as a name */
	int  has_clock;

	struct config_site sites[CONFIG_SITES_MAX];
	size_t site_count;

	/*
	 * Where a refusal happened, one-based, and zero when the verdict is
	 * `CONFIG_OK`. Filled in for **every** refusal -- a parser that
	 * reports a line for some faults and not others is one whose caller
	 * has to know which.
	 */
	unsigned line;

	/*
	 * The offending text, as far as it fits, NUL-terminated.
	 *
	 * The key or the value that was refused, not the whole line: a line
	 * echoed back in full is a line that can carry anything into a log,
	 * and this one is about to be printed on a console and written into an
	 * access log by the same machine that could not read it. Bounded and
	 * stripped of control bytes by the parser.
	 */
	char found[CONFIG_VALUE_MAX];
};

/*
 * Parse `text` into `into`.
 *
 * `len` is how many bytes are valid; the text need not be NUL-terminated, and
 * a NUL inside it is a refusal rather than an early end -- a file whose
 * meaning depends on whether the reader stops at a zero byte is a file two
 * readers read differently.
 *
 * Returns `CONFIG_OK`, or one of the refusals above with `into->line` and
 * `into->found` filled in. On a refusal `into` holds nothing usable: the
 * caller keeps its built-in defaults rather than applying part of a file.
 *
 * Pure. No allocation, no filesystem, no clock -- which is what lets the suite
 * hand it every hostile file as a string literal and run in milliseconds,
 * the same arrangement `request.c` has.
 */
int config_parse(const char *text, size_t len, struct config *into);

/* What a refusal means, in a few words, for a console line. Never NULL: an
 * unrecognised verdict gives "unknown", which is easier to find than a crash
 * and is what the suite looks for to prove the table covers every code. */
const char *config_reason(int verdict);

/*
 * Is this a plausible absolute path for a document root?
 *
 * Here rather than inside the parser because it is a rule about what a path
 * means, and a rule with a suite is a rule somebody can check.
 *
 * Absolute, no `..` component anywhere, no backslash, no control byte, and not
 * a bare `/`. The `..` rule is the load-bearing one: a document root is the
 * boundary every served path is resolved against, so a root that climbs is a
 * boundary that was never there. `files.c` refuses a climbing *request*, and
 * a root nobody checked would have moved the whole tree instead.
 *
 * Returns 1 when it is usable, 0 otherwise.
 */
int config_root_ok(const char *path);

/*
 * Is this a plausible host name for a site?
 *
 * Letters, digits, `-` and `.`, at least one character, no leading or trailing
 * dot, no empty label, and short enough to fit. **No port**, because a site is
 * a machine and `http_host_matches` already ignores the port a client sends --
 * a configured `shop.example:8080` would be a name that can never match and no
 * message would ever say so.
 *
 * Returns 1 when it is usable, 0 otherwise.
 */
int config_host_ok(const char *host);

/*
 * The file to write when a volume has none.
 *
 * **A machine that needs a configuration file cannot be given one without a
 * way to put a file on it**, and today nothing in user mode can: `SYS_CREATE`
 * writes a file whole and refuses to overwrite, and there is no call that
 * removes or replaces one. `docs/KERNEL-WANTS.md` carries the entry. So the
 * server writes a commented template on a volume that does not have one, which
 * is the difference between *no configuration yet* and *nowhere to put one*.
 *
 * Every line in it is a comment, so a machine that writes it and reads it back
 * is configured exactly as it was. The suite checks that this parses, because a
 * template the parser refuses is the worst first impression a file format can
 * make -- and it is the sort of thing that goes wrong the first time somebody
 * adds a setting and updates the example to match.
 *
 * Returns the text; `*len` is its length. Never NULL.
 */
const char *config_template(size_t *len);

/*
 * --- Configuring a machine that cannot replace a file ---------------------
 *
 * `docs/KERNEL-WANTS.md` carries the entry: nothing in user mode can replace or
 * remove a file. `SYS_CREATE` writes one whole and refuses a name that exists,
 * there is no unlink, no rename and no truncate, and the only door onto an
 * existing file is the flag combination VF-027 measured as contradicting its
 * own documentation. **So a configuration file could be written once and never
 * corrected**, which is not a configuration.
 *
 * That is the same wall `logfile.h` hit, and it found the shape on the other
 * side of it: *a log that appends is impossible here; a log that **rotates** is
 * natural.* The same answer works here. A configuration is not edited, it is
 * **superseded**: each one is written once, whole, under the next number, and
 * the machine reads the highest it finds.
 *
 * What that buys, beyond being possible at all:
 *
 *   * **every configuration this machine has ever run is still on the volume**,
 *     which is an audit trail nobody had to build;
 *   * a generation is either there or it is not, because ReconFS writes a whole
 *     file in one transaction -- there is no half-written configuration for the
 *     next boot to read;
 *   * and getting it wrong is recoverable, because writing another one is the
 *     same operation as writing the first.
 *
 * **A new generation takes effect at the next boot, and nothing about that is
 * an accident.** Applying it live would mean a machine whose listening port or
 * site list changed under the connection that was asking for the change, and a
 * mistake would take the console with it before anybody could write the
 * correction. Written, then read on the next boot, means a person who mistypes
 * a port can simply write another generation.
 */

/* Where generations live. A directory of its own, so that listing it is
 * unambiguous -- the log's own numbering had to ignore names that were not
 * segments, and a directory that holds one kind of thing does not. */
#define CONFIG_DIR         "/System/Config"
#define CONFIG_DIGITS      6
#define CONFIG_NAME_MAX    (CONFIG_DIGITS + 5 + 1)  /* digits + ".conf" + NUL */
#define CONFIG_MAX_NUMBER  999999UL

/*
 * The file name for a generation, zero-padded.
 *
 * Six digits for the same reason `logfile.h` gives: `10.conf` sorts before
 * `9.conf` and every reader that lists a directory then gets them out of order,
 * and nothing about that failure announces itself.
 *
 * Returns the length written, or negative when `number` is past
 * `CONFIG_MAX_NUMBER` or the room is too small.
 */
long config_generation_name(unsigned long number, char *out, size_t room);

/*
 * Is this a generation's name, and which one?
 *
 * Public, and for the reason `logfile_is_segment` is: the function that decides
 * the next number and the function that lists what is there must agree, and two
 * readers of one naming rule drift. A name this refuses is a name
 * `config_generation_next` steps over, which would be a configuration nobody
 * can reach.
 */
int config_is_generation(const char *name, size_t len, unsigned long *number);

/*
 * The highest generation in a directory listing, and the next number to use.
 *
 * `names` is what `SYS_LIST` produces: names NUL-terminated and back to back,
 * `len` bytes in total. Anything that is not a generation is ignored -- a stray
 * file in the directory must not stop a machine configuring itself.
 *
 * `*highest` receives the newest generation present, or 0 when there is none.
 * The return is the number to write next, which is `*highest + 1`.
 *
 * **Taken from the directory rather than from memory**, which is the mistake
 * `logfile.h` records at length: a server that started again at one each boot
 * would not clobber anything -- `SYS_CREATE` refuses -- it would fail every
 * write from the second boot onwards, silently.
 */
unsigned long config_generation_next(const char *names, size_t len,
                                     unsigned long *highest);

#endif
