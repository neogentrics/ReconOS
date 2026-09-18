/*
 * The first program on a machine whose role is **server**.
 *
 * `docs/ROLES.md`: the installer puts down the whole operating system every
 * time and the role is chosen on the first boot afterwards. So this is not a
 * different system from the workstation's `userland/init/recon_init.c` -- it
 * is the same system told what it is for. It draws with the same `screen.c`,
 * on purpose and at Joshua's instruction, so that a person who has seen one
 * ReconOS machine recognises the next one.
 *
 * What it does that the workstation's does not: **it serves.**
 *
 * --- What this program is actually here to prove ---
 *
 * The kernel session recorded, in its own words, that a socket descriptor
 * working with `read` and `write` is *argued, not measured*. Their self-test
 * cannot hold a descriptor -- a kernel thread has no process, so `fd_install`
 * fails there for sockets exactly as it does for pipes. `recon_init` answered
 * half of it: a descriptor can be held, closed once, refused twice, and a
 * listener says `EAGAIN` with nobody waiting.
 *
 * The half still open is whether **bytes move over an accepted connection**.
 * Nothing had ever done that. This program does it, and the line it prints on
 * the serial console is the measurement:
 *
 *     the web server: listening on :80 -- <n> requests served, <m> bytes out
 *
 * A number there that goes up is the first evidence that this kernel's sockets
 * carry traffic. A number that stays at zero, with a client that connected, is
 * a fault worth a KF entry.
 */

#include <recon.h>
#include <recon_machine.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#include "layout.h"
#include "screen.h"

#include "../http/serve.h"
#include "../http/files.h"
#include "../http/form.h"
#include "../http/escape.h"
#include "../http/json.h"
#include "../http/jsonread.h"
#include "../http/accept.h"
#include "../http/multipart.h"
#include "../auth.h"
#include "../dns.h"
#include "../dial.h"
#include "../ntp.h"
#include "../include/recon_server.h"
#include "../service.h"
#include "../log.h"
#include "../logfile.h"
#include "../config.h"

/* Return codes, in the workstation's numbering so that a reader of one file
 * recognises them in the other. The server's own start at 70. */
#define WENT_WELL             0
#define NO_SCREEN_DESCRIBED  60
#define SCREEN_STRUCT_WRONG  61
#define FRAMEBUFFER_CLOSED   62
#define MAPPING_REFUSED      63
#define SCREEN_MAKES_NO_SENSE 64
#define MACHINE_UNREADABLE   65
#define NO_LISTENER          70

/*
 * Milliseconds since boot, for the server's receive deadline.
 *
 * `SYS_TIME` is nanoseconds and monotonic, which is exactly what a deadline
 * wants: `serve.c` only ever subtracts two of these, so where the count starts
 * does not matter and a clock that could go backwards would.
 */
static unsigned long clock_ms(void)
{
	return (unsigned long)(recon_time() / 1000000ULL);
}

static void say(const char *text)
{
	recon_write(1, text, recon_strlen(text));
}

/* --- what this server answers with --------------------------------------- */

/*
 * A value on its way into a JSON string.
 *
 * **Everything goes through here, including the strings that cannot hold a
 * quote today.** The machine's name is restricted by `server_name_split` to
 * letters, digits and the hyphen; a service's name and description are
 * literals a few hundred lines below. Neither can close a string, and neither
 * being able to is exactly the argument that was true of the dashboard until
 * it was not -- the coincidence `escape.c` was written to stop depending on,
 * found still load-bearing in the JSON path one file away.
 *
 * A rule with an exception is a rule a reader cannot check. The exception here
 * would be "unless the value is known safe", which is a judgement the next
 * person to add a field has to make correctly and silently.
 *
 * Returns NULL when the text will not fit, or holds a byte the escaper refuses
 * -- a caller answers 500 rather than sending a document with a piece missing.
 */
static const char *as_json(const char *text, char *room, size_t size)
{
	if (!text)
		text = "";
	return json_escape(text, room, size) < 0 ? 0 : room;
}

/* Room for the worst case: six bytes out for one in, plus a terminator. See
 * `json.h`. Written as arithmetic on the source bound rather than as a number,
 * so a longer name cannot outgrow the buffer quietly. */
#define JSON_ROOM(n) ((n) * 6 + 1)

/*
 * The machine's own facts, gathered once and shown on every page.
 *
 * Kept in one place because the dashboard and the JSON endpoint must not be
 * able to disagree: two readers of `SYS_MACHINE` would eventually be two
 * answers, and a monitoring system that sees a different figure from the page
 * beside it has to be told which one to believe.
 */
struct server_facts {
	struct recon_machine machine;
	char                 name[RECON_NAME_MAX];
	unsigned long        served;
	unsigned long        bytes_out;

	/* What the clock service last measured. Kept here rather than read
	 * from the NTP client directly, for the reason this structure exists
	 * at all: the page and the JSON endpoint must not be able to disagree,
	 * and two readers of one fact eventually become two facts. */
	long long clock_offset_ms;
	int       clock_stratum;
	int       clock_known;
};

static struct server_facts FACTS;

/*
 * The secret that the two writing endpoints require.
 *
 * Made once at boot from `SYS_RANDOM` and printed on the serial console. See
 * `server/auth.h` for what it is, what it is not, and why source-address
 * restriction was not available instead.
 */
static struct auth GUARD;

/*
 * Where this machine asks about names.
 *
 * 10.0.2.3 is the resolver QEMU's user networking provides, and it is a
 * stand-in: a real machine learns its resolver from DHCP, which cannot be
 * written because a broadcast needs an unconnected datagram socket. That half
 * of the DNS entry in `docs/SERVER.md` is still blocked and the other half
 * turned out not to be -- see `server/dns.h`.
 *
 * So the address is a constant with a reason rather than a guess, and the day
 * DHCP exists it becomes a field somebody fills in.
 */
static struct dns_client RESOLVER;

/*
 * The site's policy, asked by `serve.c` before any route marked `guarded`.
 *
 * Only the `Authorization` header is read. A token in a query string would be
 * written into this server's own access log and sent onward by a browser in
 * `Referer`, which is the one place a secret must not go -- `auth.c` says so at
 * more length.
 */
static int may_write(const struct http_request *r, const char *body,
                     size_t body_len, void *ctx)
{
	const struct auth *guard = (const struct auth *)ctx;

	/* The header first: it is what an API client sends and it works for
	 * every method, including the ones with no body. */
	if (auth_ok(guard, http_header_get(r, "authorization")))
		return 1;

	/*
	 * Then a form field, so the console can be driven from a browser.
	 *
	 * **A browser form cannot send a header**, which is why the dashboard's
	 * rename form answered 401 to every submission from the moment the
	 * guard landed until this was written. See `auth.h` for why a POST body
	 * is admitted where a query string is still refused.
	 */
	return auth_ok_form(guard, body, body_len);
}

/*
 * What the supervisor holds on this machine.
 *
 * One service today. The shape is the point: when discovery exists it
 * registers here, and when the datagram call arrives DNS and DHCP register
 * beside it, and the loop at the bottom of `main` does not change.
 */
static struct supervisor SUPERVISOR;

/*
 * What this machine has answered lately.
 *
 * In memory and not on the volume, and **the reason written here for three
 * versions was wrong.** It said appending needs `O_APPEND`, which the C library
 * drops. The library does drop it -- but appending needs the ability to
 * append, and `O_APPEND` is only one way to get it: `SYS_SEEK` exists, and
 * seek-to-end then write is the other.
 *
 * Measured on the machine against a file that exists, trying every open flag
 * the kernel has rather than the two the C library publishes:
 *
 *     append probe: create=1 W=-12 W|CREATE=5 W|REPLACE=-12
 *
 * Plain write is refused. `OPEN_CREATE`, documented *it must not already
 * exist*, **succeeded** on a file that does. `OPEN_REPLACE`, documented *it
 * must exist*, was refused on one. Both behave opposite to their own comments,
 * so the only route that opens is the one that contradicts its documentation,
 * and building a log on that would be building on a fault.
 *
 * So this still does not survive a reboot, for a better-understood reason that
 * is now in `docs/SIGNALS.md` for the kernel session rather than resting on a
 * C-library gap that was never the whole story.
 */
static struct logbook LOGBOOK;

/*
 * Write out whatever the ring holds that the volume does not.
 *
 * Defined further down with the rest of the log-on-volume code, where its
 * reasoning belongs. Declared here because the web service's poll is the only
 * thing that knows a request has been answered, and it runs long before that
 * part of the file.
 *
 * `force` writes a short segment rather than waiting for a full one.
 */
/*
 * Where the log stands on the volume.
 *
 * Declared up here with the ring it mirrors rather than beside the functions
 * that maintain it, because the endpoints that *report* it are defined before
 * those. See `log_volume_open` and `log_volume_flush` further down.
 */
static struct {
	unsigned long next;		/* the segment number to write */
	unsigned long flushed;		/* entries already written out */
	int ready;			/* the directory was read */
	unsigned long segments;		/* written this boot */
	unsigned long refused;		/* flushes that could not be written */
} LOGVOL;

static void log_volume_flush(int force);

/*
 * Record one answered request.
 *
 * Called by the server rather than by each handler, so every response is
 * logged -- including the ones a handler never saw, where a request was refused
 * before routing. `request` is NULL for those, and that is the entry somebody
 * will come looking for.
 */
static void note_request(void *ctx, const struct http_request *r, int status,
                         unsigned long bytes)
{
	struct logbook *book = (struct logbook *)ctx;
	char line[LOG_LINE_MAX];

	if (r)
		snprintf(line, sizeof(line), "%s %s%s%s -> %d, %lu bytes",
		         r->method, r->target,
		         r->query[0] ? "?" : "", r->query,
		         status, bytes);
	else
		snprintf(line, sizeof(line),
		         "(unparsable request) -> %d, %lu bytes",
		         status, bytes);

	log_write(book, (unsigned long)recon_time(), line);

	/*
	 * And the counter, here rather than in the serving loop.
	 *
	 * This runs exactly once per response -- including the ones refused
	 * before any handler saw them, which are requests the machine answered
	 * and should be counted as such. It is the same event the log records,
	 * so the page's figure and the log's length cannot drift apart.
	 */
	FACTS.served++;
}


/*
 * The console's styles, served as a file rather than written into the page.
 *
 * **This is what makes a real Content-Security-Policy possible**, and that is
 * the only reason it moved. While the rules lived in a `<style>` block and in
 * `style=` attributes, any policy this server could send needed
 * `'unsafe-inline'` -- which permits exactly what CSP exists to stop, while the
 * header's presence suggests otherwise. `serve.c` carried a comment saying so
 * and refusing to ship one.
 *
 * With the rules in a file, `style-src 'self'` is true, and the policy in
 * `serve.c` says what it means.
 *
 * It is written to the volume at boot beside `index.html`, by the same
 * function and with the same rule: **an existing file is not overwritten.** An
 * administrator who has restyled their console keeps their work.
 */
static const char CONSOLE_CSS[] =
	"body{background:#141821;color:#d8dce6;"
	"font:15px/1.6 system-ui,sans-serif;margin:0;padding:40px}\n"
	"main{max-width:640px;margin:0 auto}\n"
	"h1{color:#4fb3a5;font-size:26px;margin:0 0 4px}\n"
	"p.sub{color:#737c90;margin:0 0 28px}\n"
	"table{border-collapse:collapse;width:100%}\n"
	"td{padding:8px 0;border-bottom:1px solid #2a3145}\n"
	"td:first-child{color:#737c90;width:40%}\n"
	"code{color:#4fb3a5}\n"
	"form.rename{margin-top:28px}\n"
	"form.rename label{color:#737c90}\n"
	"form.rename input{background:#1c2130;color:#d8dce6;"
	"border:1px solid #2a3145;padding:6px 8px;font:inherit}\n"
	"form.rename button{background:#4fb3a5;color:#141821;border:0;"
	"padding:7px 14px;font:inherit;cursor:pointer}\n"
	"p.footnote{color:#737c90;margin-top:28px}\n";

static const char PAGE_HEAD[] =
	"<!doctype html>\n<meta charset=\"utf-8\">\n"
	"<title>ReconOS Server</title>\n"
	"<link rel=\"stylesheet\" href=\"/console.css\">\n"
	"<main>\n";

/*
 * The dashboard. One page, real numbers, nothing invented.
 *
 * This is the console the docx calls the Server Manager dashboard, at the size
 * it can honestly be today: what this machine is, and what this server has
 * done. Every row is read from the kernel or counted here.
 *
 * --- The machine name is escaped, and it did not used to be ---
 *
 * It appears twice below: as the heading, and as the value of an input. Both
 * go through `http_escape` now.
 *
 * They did not, and it was *safe* -- because `handle_set_name` runs every
 * candidate through `server_name_split`, which admits letters, digits and the
 * hyphen and refuses everything else, so a `<` or a `"` could not reach here.
 * That safety lived in a function three files away and was recorded only in a
 * comment at this spot, which said in as many words that **the day the name
 * rules loosened -- a dot, for a fully qualified name -- this page would become
 * a cross-site scripting hole with nothing to announce it.**
 *
 * A dependency that is true by coincidence and documented in prose is a
 * dependency waiting to be broken by somebody improving something else. The
 * escaping is here now and the coincidence no longer matters.
 *
 * The `value="..."` on the input is the one that mattered most: a name
 * containing a quote would have closed the attribute and everything after it
 * become attributes of the tag. `server/tests/test_http_escape.c` carries that
 * exact case, reconstructed from this field.
 */
static int handle_dashboard(const struct http_request *r, const char *body,
                            size_t body_len, struct http_response *out,
                            void *ctx)
{
	static char page[HTTP_RESPONSE_MAX];
	static char safe_name[RECON_NAME_MAX * 6 + 1];
	static char clock_said[96];
	struct server_facts *f = (struct server_facts *)ctx;
	unsigned running = 0, failed = 0, refused = 0;
	int n;

	(void)r; (void)body; (void)body_len;

	supervisor_tally(&SUPERVISOR, &running, &failed, &refused);

	/*
	 * The clock, in words, with its real uncertainty.
	 *
	 * **Never to the millisecond it cannot support.** `SYS_WALLTIME`
	 * counts whole seconds -- VF-021 -- so this machine's contribution to
	 * the offset is coarse, and a page that printed "1174 ms" would be
	 * claiming a precision the hardware underneath it does not have.
	 */
	if (!f->clock_known) {
		snprintf(clock_said, sizeof(clock_said),
		         "not measured yet");
	} else {
		long long off = f->clock_offset_ms;

		snprintf(clock_said, sizeof(clock_said),
		         "about %lld s %s (+/- 1 s), from a stratum %d server",
		         (off >= 0 ? off : -off) / 1000,
		         off >= 0 ? "behind" : "ahead", f->clock_stratum);
	}

	/* Six bytes out for one in, worst case -- a name of nothing but quotes.
	 * A buffer sized at the name's length would refuse on the first one,
	 * which is safe and useless. */
	if (http_escape(f->name, safe_name, sizeof(safe_name)) < 0)
		return HTTP_EBODY_LONG;

	n = snprintf(page, sizeof(page),
	             "%s"
	             "<h1>%s</h1>\n"
	             "<p class=\"sub\">ReconOS, running its own kernel. "
	             "No Linux is underneath it.</p>\n"
	             "<table>\n"
	             "<tr><td>role</td><td>server</td></tr>\n"
	             "<tr><td>architecture</td><td>%s</td></tr>\n"
	             "<tr><td>processor</td><td>%s</td></tr>\n"
	             "<tr><td>processors</td><td>%u found, %u in use</td></tr>\n"
	             "<tr><td>memory</td><td>%llu MiB total, %llu MiB free</td></tr>\n"
	             "<tr><td>page size</td><td>%u KiB</td></tr>\n"
	             "<tr><td>requests served</td><td><code>%lu</code></td></tr>\n"
	             "<tr><td>bytes sent</td><td><code>%lu</code></td></tr>\n"
	             "<tr><td>services</td><td>%u running, %u failed, "
	             "%u refused</td></tr>\n"
	             "<tr><td>clock</td><td>%s</td></tr>\n"
	             "<tr><td>writes</td><td>%s</td></tr>\n"
	             "</table>\n"
	             "<form class=\"rename\" method=\"post\" "
	             "action=\"/api/name\">\n"
	             "<label>rename this machine "
	             "<input name=\"name\" value=\"%s\"></label>\n"
	             "<label>console token "
	             "<input name=\"token\" type=\"password\" "
	             "autocomplete=\"off\"></label>\n"
	             "<button>Set</button>\n"
	             "</form>\n"
	             "<p class=\"footnote\">The token is printed on this "
	             "machine's serial console at boot and is new on every "
	             "boot. A browser form cannot send an "
	             "<code>Authorization</code> header, which is why this "
	             "asks for it here; an API client should send the header "
	             "instead.</p>\n"
	             "<p class=\"footnote\">"
	             "Served by <code>server/http/</code> over the kernel's five "
	             "socket calls. This page is the first thing to move bytes "
	             "over an accepted connection on this system.</p>\n"
	             "</main>\n",
	             PAGE_HEAD,
	             safe_name,
	             f->machine.architecture[0] ? f->machine.architecture
	                                        : "unnamed",
	             f->machine.cpu_model[0] ? f->machine.cpu_model
	                                     : f->machine.cpu_vendor,
	             f->machine.processors_found, f->machine.processors_online,
	             (unsigned long long)(f->machine.memory_bytes >> 20),
	             (unsigned long long)(f->machine.memory_free_bytes >> 20),
	             f->machine.page_size / 1024u,
	             f->served, f->bytes_out,
	             running, failed, refused,
	             clock_said,
	             GUARD.armed ? "need the console token"
	                         : "refused -- this machine has no secret",
	             safe_name);

	if (n < 0 || (size_t)n >= sizeof(page))
		return HTTP_EBODY_LONG;

	http_response_simple(out, 200, "text/html; charset=utf-8", page,
	                     (size_t)n);
	return HTTP_OK;
}

/* The same facts as JSON, which is what turns this from a page into an API.
 *
 * Deliberately the same numbers from the same structure -- see `struct
 * server_facts`. A dashboard and an endpoint that read separately are a
 * dashboard and an endpoint that eventually disagree. */
static int handle_status(const struct http_request *r, const char *body,
                         size_t body_len, struct http_response *out, void *ctx)
{
	static char json[1024];
	struct server_facts *f = (struct server_facts *)ctx;
	char name_room[JSON_ROOM(RECON_NAME_MAX)];
	char arch_room[JSON_ROOM(sizeof(f->machine.architecture))];
	const char *name, *arch;
	int n;

	(void)r; (void)body; (void)body_len;

	name = as_json(f->name, name_room, sizeof(name_room));
	arch = as_json(f->machine.architecture[0] ? f->machine.architecture
	                                          : "unnamed",
	               arch_room, sizeof(arch_room));
	if (!name || !arch)
		return HTTP_EINTERNAL;

	n = snprintf(json, sizeof(json),
	             "{\"role\":\"server\","
	             "\"name\":\"%s\","
	             "\"architecture\":\"%s\","
	             "\"processors_found\":%u,"
	             "\"processors_online\":%u,"
	             "\"memory_bytes\":%llu,"
	             "\"memory_free_bytes\":%llu,"
	             "\"page_size\":%u,"
	             "\"requests_served\":%lu,"
	             "\"bytes_sent\":%lu,"
	             /*
	              * The clock, reported here because the dashboard reports
	              * it and `struct server_facts` exists so the two cannot
	              * disagree. It was added to that structure and read only
	              * by the page for one version, which is the drift the
	              * structure was written to prevent -- caught by rereading
	              * the comment that justified it.
	              *
	              * `clock_measured` is separate from the offset rather
	              * than a sentinel value, because zero is a real offset:
	              * a machine whose clock is right and one that has never
	              * asked must not report the same thing.
	              *
	              * `clock_uncertainty_ms` is 1000 and is not a guess. See
	              * VF-021: `SYS_WALLTIME` counts whole seconds, so this
	              * machine's own two timestamps put about a second around
	              * any offset it computes. A consumer that rounds to the
	              * nearest second is reading this correctly.
	              */
	             "\"clock_measured\":%s,"
	             "\"clock_offset_ms\":%lld,"
	             "\"clock_uncertainty_ms\":1000,"
	             "\"clock_stratum\":%d}\n",
	             name, arch,
	             f->machine.processors_found, f->machine.processors_online,
	             (unsigned long long)f->machine.memory_bytes,
	             (unsigned long long)f->machine.memory_free_bytes,
	             f->machine.page_size,
	             f->served, f->bytes_out,
	             f->clock_known ? "true" : "false",
	             f->clock_offset_ms, f->clock_stratum);

	/* 500, not 413. The request was fine; this server could not fit its own
	 * answer into its own buffer. See `HTTP_EINTERNAL` in `http.h`. */
	if (n < 0 || (size_t)n >= sizeof(json))
		return HTTP_EINTERNAL;

	http_response_simple(out, 200, "application/json", json, (size_t)n);
	return HTTP_OK;
}

/* Enough of a health endpoint to be polled by something that is not a person.
 * Plain text and a fixed body, so that a checker can compare bytes rather than
 * parse. */
static int handle_health(const struct http_request *r, const char *body,
                         size_t body_len, struct http_response *out, void *ctx)
{
	(void)r; (void)body; (void)body_len; (void)ctx;
	http_response_simple(out, 200, "text/plain", "ok\n", 3);
	return HTTP_OK;
}

/* Where this role keeps what it serves.
 *
 * Under `/System` because it is part of the system rather than a user's, and
 * named for what it is. It is laid down on first boot the same way the
 * workstation lays out the rest of the volume -- an init's job is to make the
 * shape the system expects, not to find it already there. */
#define WEB_ROOT "/System/Web"

static const struct http_files SITE_FILES = { WEB_ROOT, "index.html" };

/*
 * Where an upload lands, and **why it is deliberately not under `WEB_ROOT`.**
 *
 * An upload directory that is also a document root is the oldest way to turn a
 * file upload into code execution: whatever serves the root will happily hand
 * back the `.html` somebody just posted, script and all, from this server's own
 * origin -- which is the origin the console's session lives on.
 *
 * `http_files_handler` is rooted at `WEB_ROOT` and refuses anything that climbs
 * out of it, so nothing written here is reachable over HTTP by any path. That
 * is the property, and it is structural rather than a rule somebody follows:
 * there is no route to this directory to forget to secure.
 *
 * The cost is that an upload cannot be fetched back, which is the right trade
 * at this size. A server that needs to serve what it was given needs a decision
 * about content types and an origin to serve them from, and neither exists yet.
 */
#define UPLOAD_ROOT "/System/Uploads"

/*
 * Write a file, unless it is already there.
 *
 * Defined further down with the boot-time layout code, which is where it was
 * written and where its story belongs -- the `EEXIST`-read-as-failure fault is
 * recorded beside it. Declared here because the upload endpoint needs it and
 * runs long before that part of the file.
 *
 * Returns 1 if the path already existed, 2 if it was written, -1 if it could
 * not be. **Never overwrites**, which is the property both callers want for
 * different reasons.
 */
static int put_if_absent(const char *path, unsigned long path_len,
                         const char *data, unsigned long len);

/*
 * The routes, and the order is load-bearing.
 *
 * The first match wins, so the three exact routes are checked before the file
 * handler, and the file handler is last because it is the one that can answer
 * anything. Reversed, `/api/status` would be looked for on the volume and
 * answered 404 while the endpoint sat unreachable behind it.
 *
 * Its prefix is the empty string, which matches every rooted path. Writing
 * `"/"` there would match only the root itself -- the boundary rule that stops
 * `/api` claiming `/apifoo` applies to `/` as well, and a site written that way
 * serves its index and nothing else.
 */
/*
 * Rename this machine, from a form.
 *
 * The first thing on this server that *changes* something, which is why the
 * validation is the interesting part rather than the storing.
 *
 * **The name is checked by `server_name_split`**, the same function that works
 * out what a parallel of `M16` should be called -- so a name this accepts is a
 * name the parallel numbering can work with, and there is one idea of a legal
 * machine name rather than two that drift apart.
 *
 * Two of its refusals are accepted here, and that is deliberate. `M16` splits
 * into a family and a number; `gateway` does not, and answers
 * `SERVER_EUNNUMBERED`. But a server may perfectly well be called `gateway` --
 * it simply cannot have a parallel numbered from it, which is a fact about
 * *later* and not a reason to refuse the name now. Everything else it refuses
 * -- an illegal character, an over-long name, a name that is only digits --
 * is refused here too.
 */
/*
 * Is this the media type `want`, ignoring parameters?
 *
 * `application/json; charset=utf-8` is `application/json`. Compared
 * case-insensitively because a media type is a token, and cut at the first
 * `;` or space rather than parsed: this needs to know which of two readers to
 * use, not what the parameters say.
 */
static int media_is(const char *value, const char *want)
{
	size_t i;

	if (!value)
		return 0;
	for (i = 0; want[i]; i++) {
		char c = value[i];

		if (c >= 'A' && c <= 'Z')
			c = (char)(c + 32);
		if (c != want[i])
			return 0;
	}
	return value[i] == '\0' || value[i] == ';' || value[i] == ' '
	       || value[i] == '\t';
}

/*
 * The name a request is asking for, from a form **or** from a JSON document.
 *
 * --- Why this endpoint takes two shapes ---
 *
 * A browser form can only send `application/x-www-form-urlencoded`. A
 * management client -- the thing `docs/SERVER.md` calls a structured REST API
 * -- sends a document. This server has had only the first since 0.3.0, which
 * meant the API could be driven by a person and not by a program.
 *
 * The reader is chosen by `Content-Type` and by nothing else. **Not by
 * sniffing the body**, which is how one reader ends up parsing what another
 * reader framed: a body that is valid in both shapes would then mean whichever
 * one this tried first.
 *
 * A type this server does not implement is **415**, not 400: the client's
 * request is well formed and this cannot read it, and those are different
 * facts.
 *
 * Returns HTTP_OK and points `*wanted` at the name, or fills `out` with the
 * refusal and returns HTTP_OK, or returns a verdict.
 */
static int name_wanted(const struct http_request *r, const char *body,
                       size_t body_len, struct http_response *out,
                       const char **wanted, int *answered)
{
	static struct json doc;		/* static: over four kilobytes, and
					 * this program's stack is not the
					 * place for it */
	struct http_form form;
	const char *type = http_header_get(r, "content-type");
	const struct json_node *root, *member;
	const char *text;
	size_t len = 0;
	int rc;

	*answered = 0;

	if (type && media_is(type, "application/json")) {
		char line[128];
		int n;

		rc = json_parse(body, body_len, &doc);
		if (rc != JSON_OK) {
			n = snprintf(line, sizeof(line),
			             "400 Bad Request: %s, at byte %lu\n",
			             json_reason(rc), (unsigned long)doc.where);
			if (n < 0 || (size_t)n >= sizeof(line))
				return HTTP_EINTERNAL;
			http_response_simple(out, 400, "text/plain", line,
			                     (size_t)n);
			*answered = 1;
			return HTTP_OK;
		}

		root = json_root(&doc);
		if (!root || root->kind != JSON_OBJECT) {
			http_response_simple(out, 400, "text/plain",
			                     "400 Bad Request: an object was"
			                     " expected\n", 41);
			*answered = 1;
			return HTTP_OK;
		}

		member = json_member(&doc, root, "name");
		text = json_string(&doc, member, &len);
		if (!text) {
			http_response_simple(out, 400, "text/plain",
			                     "400 Bad Request: one name, as a"
			                     " string\n", 39);
			*answered = 1;
			return HTTP_OK;
		}

		/*
		 * A NUL inside the string. Legal JSON -- `\u0000` -- and this
		 * value is about to be used as a C string by everything
		 * downstream, which would see a shorter name than was sent.
		 * Refused rather than truncated: a truncated name is a
		 * different name.
		 */
		{
			size_t i;

			for (i = 0; i < len; i++) {
				if (text[i] == '\0') {
					http_response_simple(out, 400,
					                     "text/plain",
					                     "400 Bad Request: a"
					                     " NUL in the name\n",
					                     35);
					*answered = 1;
					return HTTP_OK;
				}
			}
		}

		*wanted = text;
		return HTTP_OK;
	}

	if (type && !media_is(type, "application/x-www-form-urlencoded")) {
		http_response_simple(out, 415, "text/plain",
		                     "415 Unsupported Media Type: send a form"
		                     " or application/json\n", 61);
		*answered = 1;
		return HTTP_OK;
	}

	rc = http_form_parse(body, body_len, &form);
	if (rc != HTTP_OK)
		return rc;

	*wanted = http_form_get(&form, "name");
	if (!*wanted) {
		/* Absent, or given twice. `http_form_get` deliberately answers
		 * the same for both -- see `form.h` -- and either way there is
		 * no single name to take. */
		http_response_simple(out, 400, "text/plain",
		                     "400 Bad Request: one name field\n", 32);
		*answered = 1;
	}
	return HTTP_OK;
}

static int handle_set_name(const struct http_request *r, const char *body,
                           size_t body_len, struct http_response *out,
                           void *ctx)
{
	static char answer[256];
	struct server_facts *f = (struct server_facts *)ctx;
	struct name_family family;
	char name_room[JSON_ROOM(RECON_NAME_MAX)];
	const char *wanted = 0, *name;
	int rc, n, answered = 0;

	rc = name_wanted(r, body, body_len, out, &wanted, &answered);
	if (rc != HTTP_OK)
		return rc;
	if (answered)
		return HTTP_OK;

	rc = server_name_split(wanted, &family);
	if (rc != SERVER_OK && rc != SERVER_EUNNUMBERED) {
		http_response_simple(out, 400, "text/plain",
		                     "400 Bad Request: not a machine name\n",
		                     36);
		return HTTP_OK;
	}

	snprintf(f->name, sizeof(f->name), "%s", wanted);

	/* What it is now, read back from where it was stored rather than from
	 * what was sent. A reply that echoes the request proves the request,
	 * not the change.
	 *
	 * Escaped even though `server_name_split` has just refused everything
	 * that would need escaping. This is the endpoint where a client's own
	 * text came closest to reaching a JSON document, and the validator
	 * standing between them is two calls away in another file. */
	name = as_json(f->name, name_room, sizeof(name_room));
	if (!name)
		return HTTP_EINTERNAL;

	n = snprintf(answer, sizeof(answer),
	             "{\"role\":\"server\",\"name\":\"%s\",\"numbered\":%s}\n",
	             name, rc == SERVER_OK ? "true" : "false");
	if (n < 0 || (size_t)n >= sizeof(answer))
		return HTTP_EINTERNAL;

	http_response_simple(out, 200, "application/json", answer, (size_t)n);
	return HTTP_OK;
}

/*
 * What the supervisor holds, as JSON.
 *
 * This is the Services and Daemon Inspector the architecture document asks
 * for, at the size it can honestly be: every registered service, its state,
 * how often it has been asked, how often it has faulted, and how often it has
 * been restarted.
 *
 * **`faults` and `restarts` are both here on purpose.** A service showing
 * `running` with three restarts behind it is not the same machine as one
 * showing `running` with none, and a console that reported only the state
 * would say they were.
 */
static int handle_services(const struct http_request *r, const char *body,
                           size_t body_len, struct http_response *out,
                           void *ctx)
{
	static char json[2048];
	struct supervisor *sup = (struct supervisor *)ctx;
	unsigned running = 0, failed = 0, refused = 0;
	unsigned i;
	int n = 0, m;

	(void)r; (void)body; (void)body_len;

	supervisor_tally(sup, &running, &failed, &refused);

	m = snprintf(json, sizeof(json),
	             "{\"running\":%u,\"failed\":%u,\"refused\":%u,"
	             "\"services\":[",
	             running, failed, refused);
	if (m < 0 || (size_t)m >= sizeof(json))
		return HTTP_EINTERNAL;
	n = m;

	for (i = 0; i < sup->count; i++) {
		const struct service *s = sup->services[i];
		const struct service_status *st = &sup->status[i];
		/*
		 * `name`, `what` and the state are all `const char *` with no
		 * bound of their own -- a service is registered with whatever
		 * the registrant passed. These are the rooms they have to fit
		 * in escaped; a description longer than this is refused rather
		 * than cut, because a cut one ends mid-escape.
		 */
		char name_room[128];
		char what_room[512];
		char state_room[64];
		const char *name, *what, *state;

		name = as_json(s->name, name_room, sizeof(name_room));
		what = as_json(s->what, what_room, sizeof(what_room));
		state = as_json(service_state_name(st->state), state_room,
		                sizeof(state_room));
		if (!name || !what || !state)
			return HTTP_EINTERNAL;

		m = snprintf(json + n, sizeof(json) - (size_t)n,
		             "%s{\"name\":\"%s\",\"what\":\"%s\","
		             "\"state\":\"%s\",\"reason\":%d,\"polls\":%lu,"
		             "\"faults\":%lu,\"restarts\":%lu}",
		             i ? "," : "", name, what, state, st->last_reason,
		             st->polls, st->faults, st->restarts);
		if (m < 0 || (size_t)(n + m) >= sizeof(json))
			return HTTP_EINTERNAL;
		n += m;
	}

	m = snprintf(json + n, sizeof(json) - (size_t)n, "]}\n");
	if (m < 0 || (size_t)(n + m) >= sizeof(json))
		return HTTP_EINTERNAL;
	n += m;

	http_response_simple(out, 200, "application/json", json, (size_t)n);
	return HTTP_OK;
}

/*
 * What this machine has answered lately, as text.
 *
 * **Plain text rather than JSON, and that is not laziness.** A log line holds
 * the request's target, which is a string a client chose: the parser refuses
 * control bytes and NUL, but `%22` decodes to a quote, so a target may
 * perfectly legally contain one. Written into JSON without escaping, that
 * quote ends the string and everything after it becomes structure -- a client
 * writing entries of its own into the log a reader is looking at.
 *
 * The right answer is a JSON escaper, which does not exist yet. Until it does,
 * this is the same call as the one `serve.c` makes about CSP: ship the honest
 * thing rather than the one that reads better and is wrong. `docs/WEB.md`
 * carries the row.
 *
 * The dropped count leads, because a reader who does not know entries were lost
 * will draw conclusions from a log that is missing exactly the burst they are
 * looking for.
 */
/*
 * The access log, as text or as JSON.
 *
 * **One endpoint, two renderings, and that is the decision `docs/WEB.md` has
 * been waiting on since 0.12.0.** The row there said a JSON log was unblocked
 * and not built because it *would make one endpoint serve two formats, and two
 * representations of one thing drift exactly like two lists do*. The drift is
 * real and the answer is not a second endpoint -- that is two handlers reading
 * one ring, and the second one to be edited is the one that goes wrong. It is
 * one handler, one walk over the entries, and a branch on which bytes to emit.
 *
 * The route is marked `negotiated`, so the server sends `Vary: Accept` without
 * this function remembering to.
 */
static int handle_log(const struct http_request *r, const char *body,
                      size_t body_len, struct http_response *out, void *ctx)
{
	static const char *const OFFERS[] = {
		"text/plain",		/* first, so a browser and curl get
					 * something a person can read */
		"application/json"
	};
	static char text[LOG_ENTRIES_MAX * (LOG_LINE_MAX + 32) + 128];
	struct logbook *book = (struct logbook *)ctx;
	size_t held = log_held(book), i;
	int n, m, pick;

	(void)body; (void)body_len;

	pick = http_accept_pick(http_header_get(r, "accept"), OFFERS, 2);
	if (pick == ACCEPT_EMALFORMED) {
		http_response_simple(out, 400, "text/plain",
		                     "400 Bad Request: an Accept header this"
		                     " cannot read\n", 51);
		return HTTP_OK;
	}
	if (pick == ACCEPT_NONE) {
		/*
		 * 406, and it carries a body saying what is on offer.
		 *
		 * A bare 406 tells a client it asked for the wrong thing and
		 * not what the right thing would have been, which leaves it
		 * with nothing to do but guess.
		 */
		http_response_simple(out, 406, "text/plain",
		                     "406 Not Acceptable: this endpoint can"
		                     " send text/plain or application/json\n",
		                     74);
		return HTTP_OK;
	}

	if (pick == 1) {
		n = snprintf(text, sizeof(text),
		             "{\"held\":%lu,\"capacity\":%d,\"dropped\":%lu,"
		             "\"entries\":[",
		             (unsigned long)held, LOG_ENTRIES_MAX,
		             log_dropped(book));
		if (n < 0 || (size_t)n >= sizeof(text))
			return HTTP_EINTERNAL;

		for (i = 0; i < held; i++) {
			const struct log_entry *e = log_at(book, i);
			char line[JSON_ROOM(LOG_LINE_MAX)];

			/*
			 * Escaped, not written straight in.
			 *
			 * A log line is the one text on this machine composed
			 * entirely by somebody else -- it carries the request
			 * target, which a client chose. VF-010 is this
			 * project's entry about a value that reached a JSON
			 * document unescaped and was safe only by coincidence.
			 */
			if (json_escape(e->line, line, sizeof(line)) < 0)
				return HTTP_EINTERNAL;

			m = snprintf(text + n, sizeof(text) - (size_t)n,
			             "%s{\"at\":%lu,\"line\":\"%s\"}",
			             i ? "," : "", e->at, line);
			if (m < 0 || (size_t)(n + m) >= sizeof(text))
				return HTTP_EBODY_LONG;
			n += m;
		}

		m = snprintf(text + n, sizeof(text) - (size_t)n, "]}\n");
		if (m < 0 || (size_t)(n + m) >= sizeof(text))
			return HTTP_EBODY_LONG;
		n += m;

		http_response_simple(out, 200, "application/json", text,
		                     (size_t)n);
		return HTTP_OK;
	}

	n = snprintf(text, sizeof(text),
	             "held %lu of %d, dropped %lu\n\n",
	             (unsigned long)held, LOG_ENTRIES_MAX,
	             log_dropped(book));
	if (n < 0 || (size_t)n >= sizeof(text))
		return HTTP_EBODY_LONG;

	for (i = 0; i < held; i++) {
		const struct log_entry *e = log_at(book, i);

		m = snprintf(text + n, sizeof(text) - (size_t)n, "%lu  %s\n",
		             e->at, e->line);
		if (m < 0 || (size_t)(n + m) >= sizeof(text))
			return HTTP_EBODY_LONG;
		n += m;
	}

	http_response_simple(out, 200, "text/plain; charset=utf-8", text,
	                     (size_t)n);
	return HTTP_OK;
}

/*
 * Take a file.
 *
 * The first endpoint on this machine that accepts something a client composed
 * and keeps it. Everything before it either reported facts or set a name the
 * validator had already restricted to letters, digits and a hyphen.
 *
 * --- What is relied on, and what is not ---
 *
 * `multipart.c` has already refused a filename holding a separator, a `..` or
 * a NUL by the time this runs. **This does not rely on that** for the property
 * that matters: an accepted name is joined to `UPLOAD_ROOT`, which nothing
 * serves, so even a name that got through could not be fetched back. Two
 * independent reasons, because the interesting failures in this project have
 * all been a single reason that quietly stopped holding.
 *
 * --- Why it refuses to overwrite ---
 *
 * `put_if_absent`, the same call the site layout uses, and for a sharper
 * version of the same reason. A second upload of a name that already exists is
 * either a mistake or somebody replacing a file they should not be able to
 * reach, and both deserve 409 rather than a silent replacement. There is no
 * authentication on this server; "whoever asks last wins" is not a rule to
 * build a file store on.
 *
 * --- The size limit is the request buffer, and it is honest ---
 *
 * `HTTP_BODY_MAX` is 64 KiB and a larger body is refused with 413 by `serve.c`
 * before this runs. This is not an upload endpoint for real files yet, because
 * the request side does not stream -- `docs/WEB.md` carries the entry. What it
 * is: the parsing, the naming and the writing, working and measured, so that
 * the day streaming arrives is not also the day this format is learnt.
 */
static int handle_upload(const struct http_request *r, const char *body,
                         size_t body_len, struct http_response *out, void *ctx)
{
	static char answer[512];
	static char path[256];
	struct http_multipart form;
	char boundary[HTTP_BOUNDARY_MAX + 1];
	char name_room[JSON_ROOM(HTTP_PART_FILENAME_MAX)];
	const struct http_part *file;
	const char *ct, *escaped;
	int rc, n, wrote;
	size_t at, i;

	(void)ctx;

	ct = http_header_get(r, "content-type");
	if (!ct) {
		http_response_simple(out, 415, "text/plain",
		                     "415 Unsupported Media Type:"
		                     " multipart/form-data\n", 52);
		return HTTP_OK;
	}

	rc = http_multipart_boundary(ct, boundary, sizeof(boundary));
	if (rc != HTTP_OK) {
		/* 415 rather than 400: the request is well formed, it is the
		 * media type this endpoint cannot take. A client told 400
		 * looks for a syntax error it will not find. */
		http_response_simple(out, 415, "text/plain",
		                     "415 Unsupported Media Type:"
		                     " multipart/form-data\n", 52);
		return HTTP_OK;
	}

	rc = http_multipart_parse(body, body_len, boundary, &form);
	if (rc != HTTP_OK)
		return rc;

	/* One part, called `file`, carrying a filename. `http_multipart_get`
	 * answers NULL for absent and for sent-twice alike -- see `form.h` --
	 * and neither is a request this can act on. */
	file = http_multipart_get(&form, "file");
	if (!file || !file->has_filename || file->filename[0] == '\0') {
		http_response_simple(out, 400, "text/plain",
		                     "400 Bad Request: one file part\n", 31);
		return HTTP_OK;
	}

	/*
	 * Join the name to the root by hand rather than with `snprintf`, so
	 * the bound is checked before anything is written rather than after a
	 * truncation has already happened. A truncated path is a path to a
	 * different file, and this is the moment that would matter.
	 */
	at = sizeof(UPLOAD_ROOT) - 1;
	for (i = 0; i < at; i++)
		path[i] = UPLOAD_ROOT[i];
	path[at++] = '/';
	for (i = 0; file->filename[i]; i++) {
		if (at + 1 >= sizeof(path))
			return HTTP_EINTERNAL;
		path[at++] = file->filename[i];
	}
	path[at] = '\0';

	wrote = put_if_absent(path, at, file->data, file->data_len);
	if (wrote == 1) {
		http_response_simple(out, 409, "text/plain",
		                     "409 Conflict: that name is taken\n", 33);
		return HTTP_OK;
	}
	if (wrote < 0)
		return HTTP_EINTERNAL;

	escaped = as_json(file->filename, name_room, sizeof(name_room));
	if (!escaped)
		return HTTP_EINTERNAL;

	n = snprintf(answer, sizeof(answer),
	             "{\"stored\":\"%s\",\"bytes\":%lu,\"served\":false}\n",
	             escaped, (unsigned long)file->data_len);
	if (n < 0 || (size_t)n >= sizeof(answer))
		return HTTP_EINTERNAL;

	/* `served: false` is in the reply on purpose. A client that has just
	 * uploaded a file will look for its URL, and the honest answer is that
	 * there is not one. See `UPLOAD_ROOT`. */
	http_response_simple(out, 201, "application/json", answer, (size_t)n);
	return HTTP_OK;
}

/*
 * Resolving a name, on behalf of whoever asked.
 *
 * --- Why this is a guarded route ---
 *
 * An open resolver endpoint is an open resolver. Anyone who can reach it makes
 * this machine send a query of their choosing to a name server, and the reply
 * comes back at this machine's expense -- which is the shape of a DNS
 * amplifier and, less dramatically, a way to use somebody else's server to ask
 * questions they would rather not ask themselves.
 *
 * Reads elsewhere on this server are open because they report facts about this
 * machine. This one makes the machine *act*, on a target the caller chose, and
 * that is the line the guard is drawn on rather than the read/write one.
 */
static int handle_resolve(const struct http_request *r, const char *body,
                          size_t body_len, struct http_response *out, void *ctx)
{
	static char answer[512];
	struct dns_client *c = (struct dns_client *)ctx;
	struct dns_result res;
	struct http_form q;
	char name_room[JSON_ROOM(DNS_NAME_MAX)];
	const char *name, *escaped;
	unsigned short id;
	unsigned char seed[2];
	long got;
	int rc, n, i;

	(void)body; (void)body_len;

	/*
	 * The name comes from the query string, which `request.c` keeps raw and
	 * undecoded -- so it is decoded here with the form decoder, which is the
	 * one that knows `+` is a space. `form.h` explains why that is a
	 * separate function from the path decoder.
	 */
	rc = http_form_parse(r->query, recon_strlen(r->query), &q);
	if (rc != HTTP_OK)
		return rc;

	name = http_form_get(&q, "name");
	if (!name) {
		/* Absent, or given twice -- `http_form_get` answers the same
		 * for both, deliberately. Neither is a name to look up. */
		http_response_simple(out, 400, "text/plain",
		                     "400 Bad Request: one name\n", 26);
		return HTTP_OK;
	}

	/*
	 * The identifier, from `SYS_RANDOM`.
	 *
	 * **It is one of the few things an off-path attacker has to guess.** A
	 * counter would make every query's identifier predictable from the last
	 * one, which turns forging a reply from a guess into arithmetic. If
	 * randomness fails this refuses rather than falling back to something
	 * orderly, for the same reason the guard refuses without a secret.
	 */
	got = (long)recon_call6(SYS_RANDOM, (u64)(unsigned long)seed,
	                        sizeof(seed), 0, 0, 0, 0);
	if (got < (long)sizeof(seed))
		return HTTP_EINTERNAL;
	id = (unsigned short)((seed[0] << 8) | seed[1]);

	rc = dns_resolve(c, name, id, &res);

	escaped = as_json(name, name_room, sizeof(name_room));
	if (!escaped)
		return HTTP_EINTERNAL;

	if (rc != DNS_OK) {
		/*
		 * The verdict by name, not by number, and the server's own
		 * RCODE beside it. "No such name" and "the server failed" are
		 * different facts and a caller that treats them alike will
		 * cache a temporary failure for ever.
		 */
		const char *why =
			rc == DNS_ETRUNCATED ? "truncated" :
			rc == DNS_EMISMATCH  ? "no matching answer" :
			rc == DNS_ENAME      ? "not a name" :
			rc == DNS_ELOOP      ? "bad compression pointer" :
			rc == DNS_ESERVER    ? "server said no" :
			                       "malformed";

		n = snprintf(answer, sizeof(answer),
		             "{\"name\":\"%s\",\"resolved\":false,"
		             "\"why\":\"%s\",\"rcode\":%d}\n",
		             escaped, why, res.rcode);
		if (n < 0 || (size_t)n >= sizeof(answer))
			return HTTP_EINTERNAL;

		/* 502: this server asked somebody else and did not get a usable
		 * answer. Not 404 -- the resource here is the lookup, and the
		 * lookup happened. */
		http_response_simple(out, 502, "application/json", answer,
		                     (size_t)n);
		return HTTP_OK;
	}

	n = snprintf(answer, sizeof(answer),
	             "{\"name\":\"%s\",\"resolved\":true,\"ttl\":%lu,"
	             "\"addresses\":[", escaped, res.ttl);
	if (n < 0 || (size_t)n >= sizeof(answer))
		return HTTP_EINTERNAL;

	for (i = 0; (size_t)i < res.count; i++) {
		unsigned int a = res.addrs[i];
		int m = snprintf(answer + n, sizeof(answer) - (size_t)n,
		                 "%s\"%u.%u.%u.%u\"", i ? "," : "",
		                 (a >> 24) & 0xFF, (a >> 16) & 0xFF,
		                 (a >> 8) & 0xFF, a & 0xFF);

		if (m < 0 || (size_t)(n + m) >= sizeof(answer))
			return HTTP_EINTERNAL;
		n += m;
	}

	{
		int m = snprintf(answer + n, sizeof(answer) - (size_t)n, "]}\n");

		if (m < 0 || (size_t)(n + m) >= sizeof(answer))
			return HTTP_EINTERNAL;
		n += m;
	}

	http_response_simple(out, 200, "application/json", answer, (size_t)n);
	return HTTP_OK;
}

/*
 * Reading the log back.
 *
 * --- Why these are guarded when `GET /api/log` is not ---
 *
 * The ring is a **snapshot**: the last sixty-four requests, bounded, current,
 * and the thing a monitor needs to see that a machine is alive. `auth.h`
 * leaves reads open for exactly that.
 *
 * The segments are **history**. Every path ever asked for on this machine,
 * from every boot it has had, for as long as the volume has existed. That is a
 * different thing to hand a stranger, and the difference is not one of degree:
 * a snapshot tells you what is happening, an archive tells you what the people
 * who use this machine do.
 *
 * So the line is drawn between *current state* and *accumulated record* rather
 * than between reading and writing. It is written here because it is the first
 * place in this server where those two do not coincide.
 *
 * --- Why the caller gives a number and never a name ---
 *
 * `GET /api/log/segment?n=42` and not `?name=000042.log`.
 *
 * A name from a caller is a path to be validated, and every validation of a
 * path is a chance to get it wrong -- `..`, a separator, a NUL, an encoding
 * that decodes to one of those later. `multipart.h` sets out at length why
 * this server refuses such names rather than repairing them.
 *
 * A **number** has no such shape. It is parsed as digits, bounded, and handed
 * to `logfile_name`, which is the same function that wrote the file. The path
 * is *constructed* here and never assembled from anything a caller typed, so
 * there is no traversal to refuse. That is not a check that passes; it is a
 * check that cannot be reached.
 */

/* Read one whole file off the volume. Returns bytes, or negative.
 *
 * Whole, because a segment is written whole and a partial one is a log with a
 * hole in it that nothing announces. */
static long read_whole(const char *path, char *out, size_t room)
{
	i64 fd = recon_open_path(path, OPEN_READ);
	long total = 0;

	if (fd < 0)
		return -1;

	for (;;) {
		i64 n = recon_read((int)fd, out + total,
		                   (u64)(room - 1 - (size_t)total));

		if (n <= 0)
			break;
		total += (long)n;
		if ((size_t)total >= room - 1)
			break;		/* does not fit; see the caller */
	}

	recon_close((int)fd);
	out[total] = '\0';
	return total;
}

/*
 * Which segments the volume holds.
 *
 * Reported as numbers rather than names, because a number is what the other
 * endpoint takes -- handing back a name a caller then sends home would invite
 * exactly the path this design avoids.
 */
static int handle_log_segments(const struct http_request *r, const char *body,
                               size_t body_len, struct http_response *out,
                               void *ctx)
{
	static char names[4096];
	static char json[2048];
	long need;
	size_t at = 0;
	int n, first = 1;

	(void)r; (void)body; (void)body_len; (void)ctx;

	need = (long)recon_call6(SYS_LIST, (u64)(unsigned long)LOGFILE_DIR,
	                         sizeof(LOGFILE_DIR) - 1,
	                         (u64)(unsigned long)names, sizeof(names),
	                         0, 0);
	if (need < 0) {
		http_response_simple(out, 503, "text/plain",
		                     "503 Service Unavailable: no volume\n", 35);
		return HTTP_OK;
	}
	if ((size_t)need > sizeof(names))
		return HTTP_EINTERNAL;	/* more listing than room: see below */

	n = snprintf(json, sizeof(json), "{\"segments\":[");
	if (n < 0 || (size_t)n >= sizeof(json))
		return HTTP_EINTERNAL;

	/*
	 * Walked with the same rule `logfile_next` uses -- six digits and
	 * `.log`, nothing else -- rather than reported raw. A directory may
	 * hold whatever somebody put there, and a listing that hands back
	 * every name is a listing of the volume rather than of the log.
	 */
	while (at < (size_t)need) {
		size_t start = at;
		unsigned long number;
		int m;

		while (at < (size_t)need && names[at] != '\0')
			at++;

		if (logfile_is_segment(names + start, at - start, &number)) {
			m = snprintf(json + n, sizeof(json) - (size_t)n,
			             "%s%lu", first ? "" : ",", number);
			if (m < 0 || (size_t)(n + m) >= sizeof(json))
				return HTTP_EINTERNAL;
			n += m;
			first = 0;
		}
		at++;
	}

	{
		int m = snprintf(json + n, sizeof(json) - (size_t)n,
		                 "],\"next\":%lu,\"written_this_boot\":%lu}\n",
		                 LOGVOL.next, LOGVOL.segments);

		if (m < 0 || (size_t)(n + m) >= sizeof(json))
			return HTTP_EINTERNAL;
		n += m;
	}

	http_response_simple(out, 200, "application/json", json, (size_t)n);
	return HTTP_OK;
}

/*
 * One segment, as text.
 *
 * The number comes from the query string and the **path is built from it**;
 * see the top of this block for why that removes a class of fault rather than
 * checking for it.
 */
static int handle_log_segment(const struct http_request *r, const char *body,
                              size_t body_len, struct http_response *out,
                              void *ctx)
{
	static char text[LOG_ENTRIES_MAX * (LOG_LINE_MAX + 48) + 256];
	char name[LOGFILE_NAME_MAX];
	char path[64];
	struct http_form q;
	const char *want;
	unsigned long number = 0;
	size_t at, i;
	long got;
	int rc;

	(void)body; (void)body_len; (void)ctx;

	rc = http_form_parse(r->query, recon_strlen(r->query), &q);
	if (rc != HTTP_OK)
		return rc;

	want = http_form_get(&q, "n");
	if (!want) {
		http_response_simple(out, 400, "text/plain",
		                     "400 Bad Request: one n\n", 23);
		return HTTP_OK;
	}

	/* Digits and nothing else. A number with a sign, a space or a letter
	 * in it is not a segment this wrote. */
	for (i = 0; want[i]; i++) {
		if (want[i] < '0' || want[i] > '9') {
			http_response_simple(out, 400, "text/plain",
			                     "400 Bad Request: n is digits\n",
			                     29);
			return HTTP_OK;
		}
		if (number > LOGFILE_MAX_NUMBER) {
			http_response_simple(out, 404, "text/plain",
			                     "404 Not Found\n", 14);
			return HTTP_OK;
		}
		number = number * 10 + (unsigned long)(want[i] - '0');
	}
	if (i == 0) {
		http_response_simple(out, 400, "text/plain",
		                     "400 Bad Request: n is digits\n", 29);
		return HTTP_OK;
	}

	if (logfile_name(number, name, sizeof(name)) < 0) {
		http_response_simple(out, 404, "text/plain",
		                     "404 Not Found\n", 14);
		return HTTP_OK;
	}

	at = sizeof(LOGFILE_DIR) - 1;
	for (i = 0; i < at; i++)
		path[i] = LOGFILE_DIR[i];
	path[at++] = '/';
	for (i = 0; name[i]; i++)
		path[at++] = name[i];
	path[at] = '\0';

	got = read_whole(path, text, sizeof(text));
	if (got < 0) {
		http_response_simple(out, 404, "text/plain",
		                     "404 Not Found\n", 14);
		return HTTP_OK;
	}

	http_response_simple(out, 200, "text/plain; charset=utf-8", text,
	                     (size_t)got);
	return HTTP_OK;
}

static const struct http_route ROUTES[] = {
	{ .method = "GET", .prefix = "/",
	  .exact = 1, .handler = handle_dashboard, .ctx = &FACTS },
	{ .method = "GET", .prefix = "/api/status",
	  .exact = 1, .handler = handle_status, .ctx = &FACTS },
	{ .method = "GET", .prefix = "/health",
	  .exact = 1, .handler = handle_health },
	{ .method = "GET", .prefix = "/api/services",
	  .exact = 1, .handler = handle_services, .ctx = &SUPERVISOR },
	{ .method = "GET", .prefix = "/api/log",
	  .exact = 1, .handler = handle_log, .ctx = &LOGBOOK,
	  .negotiated = 1 },
	{ .method = "POST", .prefix = "/api/name",
	  .exact = 1, .handler = handle_set_name, .ctx = &FACTS, .guarded = 1 },
	{ .method = "POST", .prefix = "/api/upload",
	  .exact = 1, .handler = handle_upload, .guarded = 1 },
	{ .method = "GET", .prefix = "/api/resolve",
	  .exact = 1, .handler = handle_resolve, .ctx = &RESOLVER, .guarded = 1 },
	/* The archive, guarded. The ring at `/api/log` is a snapshot and stays
	 * open; these are every path this machine has ever been asked for. See
	 * the block above `handle_log_segments`. */
	{ .method = "GET", .prefix = "/api/log/segments",
	  .exact = 1, .handler = handle_log_segments, .guarded = 1 },
	{ .method = "GET", .prefix = "/api/log/segment",
	  .exact = 1, .handler = handle_log_segment, .guarded = 1 },

	/* Last, and streaming. A file no longer has to fit in a response, so
	 * the volume can serve something larger than this program's memory. */
	{ .method = "GET", .prefix = "",
	  .stream = http_files_handler, .ctx = (void *)&SITE_FILES },
};

/*
 * The site this machine serves, whole, in one place.
 *
 * **Named, at file scope, and not assembled field by field in `main`.** It was
 * the other way until 0.24.0: a `struct http_site` on the stack with ten
 * assignments after it. That shape is wrong for exactly one reason, and the
 * reason arrived the moment `serve.h` gained a field -- the assignments set the
 * fields somebody remembered, and everything else kept whatever the stack
 * happened to hold. `host` and `next` landed in that gap: the server dispatched
 * on a name read out of uninitialised memory and answered correctly on the boot
 * it was measured on, which is the worst possible outcome of the two.
 *
 * A designated initializer zero-fills everything it does not mention, by the
 * language rather than by anybody being careful, and it does it again for every
 * field added after this line is written.
 *
 * `host` is deliberately absent, which means NULL, which means **this site
 * claims every name it is asked about**. That is what this server did before
 * virtual hosts existed and it is what a machine with one site should do -- a
 * console that starts refusing the name somebody reaches it by, after an
 * upgrade nobody asked for, is a worse outcome than the fault. `next` is absent
 * for the same reason: there is nothing after it.
 */
static const struct http_site SITE = {
	.routes = ROUTES,
	.route_count = sizeof(ROUTES) / sizeof(ROUTES[0]),
	.ctx = &FACTS,
	.server_name = "ReconOS",
	.log = note_request,
	.log_ctx = &LOGBOOK,
	.bytes_sent = &FACTS.bytes_out,

	/*
	 * What this system does while waiting for bytes that have not arrived.
	 *
	 * Without it, every request over about 2880 bytes stalled and was never
	 * answered -- not because of a size limit, but because a single process
	 * asking for bytes in a tight loop leaves nothing running that could
	 * deliver them. See `serve.h` and VF-013.
	 */
	.idle = recon_yield,
	.now_ms = clock_ms,
	.allow = may_write,
	.allow_ctx = &GUARD
};


/* --- what the configuration file adds --------------------------------------
 *
 * See `config.h`. A configured site serves files under a name; the console is
 * the site with no name and stays **last**, so it answers to anything nothing
 * else claimed -- which is what this server did before any of this existed.
 *
 * Three parallel arrays rather than one array of a bigger struct, because each
 * of the three is a different library's shape: `http_files` is what `files.c`
 * takes, `http_route` is what the dispatcher walks, and `http_site` is what the
 * server is handed. Bundling them would mean a structure whose only reason to
 * exist is that three things have the same count.
 */
static struct http_files CONFIGURED_FILES[CONFIG_SITES_MAX];
static struct http_route CONFIGURED_ROUTES[CONFIG_SITES_MAX];
static struct http_site  CONFIGURED_SITES[CONFIG_SITES_MAX];

/* The names, kept here because `struct config` is a local in `main` and the
 * sites outlive it -- a site pointing at a name on a dead stack is VF-029
 * wearing a different hat. */
static char CONFIGURED_HOSTS[CONFIG_SITES_MAX][CONFIG_VALUE_MAX];
static char CONFIGURED_ROOTS[CONFIG_SITES_MAX][CONFIG_VALUE_MAX];
static char CONFIGURED_INDEX[CONFIG_SITES_MAX][CONFIG_VALUE_MAX];

/*
 * The head of the chain the server is given.
 *
 * `&SITE` until a configuration adds sites in front of it. Everything that
 * hands a site to `http_serve_once` takes this rather than `&SITE`, so a
 * machine with no configuration file is byte-for-byte what it was.
 */
static const struct http_site *CHAIN = &SITE;

static void copy_into(char *out, size_t room, const char *from)
{
	size_t i;

	for (i = 0; i + 1 < room && from[i]; i++)
		out[i] = from[i];
	out[i] = '\0';
}

/*
 * Build the chain from a parsed configuration.
 *
 * Walked backwards so that the order in the file is the order in the chain,
 * and the chain is what decides which site answers first. A file that lists
 * `shop.example` before `docs.example` means exactly that.
 *
 * Each site **starts as a copy of the console site** rather than being filled
 * in from nothing. Everything the server needs and nobody thinks about -- the
 * byte counter, the access log, `idle`, the clock, the guard -- comes along,
 * and a field added to `struct http_site` next year arrives here for free. A
 * site assembled field by field is the fault this file already paid for once.
 */
static void chain_sites(const struct config *c)
{
	const struct http_site *next = &SITE;
	size_t i = c->site_count;

	while (i > 0) {
		struct http_site *site = &CONFIGURED_SITES[i - 1];

		i--;
		copy_into(CONFIGURED_HOSTS[i], CONFIG_VALUE_MAX, c->sites[i].host);
		copy_into(CONFIGURED_ROOTS[i], CONFIG_VALUE_MAX, c->sites[i].root);
		copy_into(CONFIGURED_INDEX[i], CONFIG_VALUE_MAX, c->sites[i].index);

		CONFIGURED_FILES[i].root = CONFIGURED_ROOTS[i];
		CONFIGURED_FILES[i].index = CONFIGURED_INDEX[i];

		/*
		 * One route, and it is a catch-all: the empty prefix, which
		 * `serve.h` says matches every path. `"/"` would match the root
		 * and nothing else, which is a site that serves its index and
		 * 404s everything beside it -- the wrong one of the two is
		 * silent, so this comment is here rather than in a commit.
		 */
		CONFIGURED_ROUTES[i].method = "GET";
		CONFIGURED_ROUTES[i].prefix = "";
		CONFIGURED_ROUTES[i].exact = 0;
		CONFIGURED_ROUTES[i].handler = 0;
		CONFIGURED_ROUTES[i].stream = http_files_handler;
		CONFIGURED_ROUTES[i].ctx = &CONFIGURED_FILES[i];
		CONFIGURED_ROUTES[i].guarded = 0;

		*site = SITE;
		site->host = CONFIGURED_HOSTS[i];
		site->next = next;
		site->routes = &CONFIGURED_ROUTES[i];
		site->route_count = 1;
		site->ctx = 0;

		/*
		 * No guard on a configured site. Nothing here is guarded --
		 * there is one route and it reads files -- and a policy left
		 * attached to a site that cannot use it is a policy somebody
		 * will one day believe is doing something.
		 */
		site->allow = 0;
		site->allow_ctx = 0;

		next = site;
	}
	CHAIN = next;
}


/* --- the configuration file -------------------------------------------------
 *
 * Read once, at boot, by the only part of this system that can read anything.
 * `config.c` decides what the bytes mean and this decides what to do about it.
 *
 * **It always says what it did.** Three outcomes -- no file, a refused file, a
 * file applied -- and a console that reports only the interesting one leaves
 * the ordinary case indistinguishable from a step that did not run. VF-008 is
 * this project's entry about two builds that reported success and produced
 * nothing.
 */

/*
 * What the web server listens on. 80 unless the configuration says otherwise.
 *
 * A value rather than a literal in `web_start`, because the supervisor may
 * restart that service and a port read from a file at boot must survive the
 * restart -- re-reading the file there would mean a machine whose listening
 * port changes without a reboot, which is a different thing from what the
 * file says.
 */
static unsigned WEB_PORT = 80;

/*
 * Which time server the clock service asks.
 *
 * The default, and only the default: `clock` in the configuration file
 * replaces it. A buffer rather than a macro since 0.25.0, which is why the
 * clock service's console lines carry `%s` where they used to concatenate a
 * literal. Why this particular name, and why a name rather than an address, is
 * in the comment above `clock_start`.
 */
#define CLOCK_SERVER_DEFAULT "time.cloudflare.com"

static char CLOCK_SERVER_NAME[CONFIG_VALUE_MAX] = CLOCK_SERVER_DEFAULT;

/* A dotted quad this has already been told is one. Returns the address. */
static unsigned int address_of(const char *text)
{
	unsigned int out = 0;
	unsigned int part = 0;
	size_t i;

	for (i = 0; text[i]; i++) {
		if (text[i] == '.') {
			out = (out << 8) | (part & 0xFF);
			part = 0;
			continue;
		}
		part = part * 10 + (unsigned int)(text[i] - '0');
	}
	return (out << 8) | (part & 0xFF);
}

static void configure(void)
{
	/*
	 * Static, not automatic. `struct config` is over four kilobytes with
	 * eight sites in it and the text buffer is eight more; this program's
	 * stack is not the place for either, and a stack overflow at boot
	 * looks like a machine that does not start rather than like a
	 * configuration that is too big.
	 */
	static char text[CONFIG_FILE_MAX + 2];
	static struct config CONF;
	char line[CONFIG_VALUE_MAX + 128];
	long got;
	int rc;
	size_t i;

	got = read_whole(CONFIG_PATH, text, sizeof(text));
	if (got < 0) {
		/*
		 * No file. Leave one.
		 *
		 * **A machine that reads a configuration file and has no way to
		 * be given one is a feature with no door into it.** Nothing in
		 * user mode here can put a file on this volume from outside,
		 * and nothing can replace or remove one either -- `SYS_CREATE`
		 * writes whole and refuses to overwrite, and there is no call
		 * that deletes. So the server writes the template itself, once,
		 * and says where it is.
		 *
		 * Every line of it is a comment, which is why this is safe to
		 * do unasked: the machine that writes it is configured exactly
		 * as it was, and the suite checks that property rather than
		 * trusting it.
		 *
		 * What this does **not** solve is editing it afterwards. That
		 * one is in `docs/KERNEL-WANTS.md` and in `docs/SIGNALS.md`.
		 */
		const char *tmpl;
		size_t tmpl_len = 0;
		int wrote;

		tmpl = config_template(&tmpl_len);
		wrote = put_if_absent(CONFIG_PATH, sizeof(CONFIG_PATH) - 1,
		                      tmpl, tmpl_len);
		if (wrote == 2)
			say("  the configuration: none found, wrote a template"
			    " to " CONFIG_PATH "\n");
		else if (wrote < 0)
			say("  the configuration: none, and no volume to keep"
			    " one on\n");
		else
			say("  the configuration: " CONFIG_PATH
			    " is there but could not be read\n");
		say("  the configuration: serving the built-in console only\n");
		return;
	}

	/*
	 * `read_whole` stops when the buffer is full, so a file larger than
	 * this would arrive **truncated and parse perfectly** -- the worst
	 * shape a bound can have. The buffer is one byte longer than the limit
	 * so that "too long" is something this can see rather than infer.
	 */
	if ((size_t)got > CONFIG_FILE_MAX) {
		snprintf(line, sizeof(line),
		         "  the configuration: REFUSED -- %s\n",
		         config_reason(CONFIG_EFILE_LONG));
		say(line);
		say("  the configuration: nothing applied,"
		    " serving the built-in console only\n");
		return;
	}

	rc = config_parse(text, (size_t)got, &CONF);
	if (rc != CONFIG_OK) {
		snprintf(line, sizeof(line),
		         "  the configuration: REFUSED at line %u -- %s (%s)\n",
		         CONF.line, config_reason(rc), CONF.found);
		say(line);
		say("  the configuration: nothing applied,"
		    " serving the built-in console only\n");
		return;
	}

	/*
	 * The machine's name is checked here rather than in `config.c`,
	 * against `server_name_split` -- **the same validator `POST /api/name`
	 * uses**. `config.c` knows what a host name looks like and nothing
	 * about this system's naming scheme, and a file that could set a name
	 * the API would refuse is two rules for one field.
	 *
	 * And it is checked before anything is applied, so this stays a
	 * whole-file decision.
	 */
	if (CONF.has_name) {
		struct name_family family;
		int split = server_name_split(CONF.name, &family);

		if (split != SERVER_OK && split != SERVER_EUNNUMBERED) {
			snprintf(line, sizeof(line),
			         "  the configuration: REFUSED at line %u --"
			         " `%s` is not a machine name\n",
			         CONF.line, CONF.name);
			say(line);
			say("  the configuration: nothing applied,"
			    " serving the built-in console only\n");
			return;
		}
	}

	/* --- from here everything is applied, because all of it parsed ------ */

	if (CONF.has_name) {
		snprintf(FACTS.name, sizeof(FACTS.name), "%s", CONF.name);
		snprintf(line, sizeof(line),
		         "  the configuration: name %s\n", FACTS.name);
		say(line);
	}
	if (CONF.has_port) {
		WEB_PORT = CONF.port;
		snprintf(line, sizeof(line),
		         "  the configuration: listening on :%u\n", WEB_PORT);
		say(line);
	}
	if (CONF.has_resolver) {
		RESOLVER.server = address_of(CONF.resolver);
		snprintf(line, sizeof(line),
		         "  the configuration: resolver %s\n", CONF.resolver);
		say(line);
	}
	if (CONF.has_clock) {
		snprintf(CLOCK_SERVER_NAME, sizeof(CLOCK_SERVER_NAME), "%s",
		         CONF.clock);
		snprintf(line, sizeof(line),
		         "  the configuration: clock %s\n", CLOCK_SERVER_NAME);
		say(line);
	}

	if (CONF.site_count) {
		chain_sites(&CONF);
		for (i = 0; i < CONF.site_count; i++) {
			snprintf(line, sizeof(line),
			         "  the configuration: site %s from %s\n",
			         CONFIGURED_HOSTS[i], CONFIGURED_ROOTS[i]);
			say(line);
		}
		say("  the configuration: the console answers to every other"
		    " name, as it always has\n");
	}

	if (!CONF.has_name && !CONF.has_port && !CONF.has_resolver &&
	    !CONF.has_clock && !CONF.site_count)
		say("  the configuration: " CONFIG_PATH
		    " read, and it says nothing\n");
}

/* --- the web server, as a service ------------------------------------------ */

struct web_service {
	int listener;
	const struct http_site *site;
};

static struct web_service WEB;

static int web_start(void *ctx)
{
	struct web_service *w = (struct web_service *)ctx;

	w->listener = http_listen(WEB_PORT);
	if (w->listener < 0)
		return -1;
	return SERVICE_OK;
}

/*
 * One round of serving.
 *
 * Answers `SERVICE_OK` for both "served one" and "nobody was waiting", because
 * neither is a fault -- `accept` on this kernel says `EAGAIN` with nobody
 * there, and a supervisor told that was a fault would restart the listener
 * every time the machine was quiet.
 *
 * The count that the loop and the dashboard read is `FACTS.served`, incremented
 * here, so there is one number rather than the supervisor's and the page's.
 */
static int web_poll(void *ctx)
{
	struct web_service *w = (struct web_service *)ctx;
	int rc = http_serve_once(w->listener, w->site);

	if (rc < 0)
		return -2;		/* the listener itself failed */

	/*
	 * **Nothing is counted here.** `http_serve_once` answers "something
	 * happened", and since the connection pool landed that means one step
	 * of one connection -- accepting it, reading part of a request,
	 * answering it. Counting those as requests made `requests_served`
	 * read about three times the truth on the dashboard and in
	 * `/api/status` for five versions: eleven requests moved it by
	 * thirty-three.
	 *
	 * The count belongs where a response is finished, which is `note`.
	 */

	/* Cheap when there is nothing to do: one comparison. See
	 * `log_volume_flush`. */
	log_volume_flush(0);
	return SERVICE_OK;
}

static void web_stop(void *ctx)
{
	struct web_service *w = (struct web_service *)ctx;

	if (w->listener >= 0)
		close(w->listener);
	w->listener = -1;
}

static const struct service WEB_SERVICE = {
	/* The description does not name the port. It is a constant in a
	 * `struct service` and the port is not one any more -- and a
	 * description that says 80 on a machine listening on 8080 is worse
	 * than one that does not mention it. `GET /api/services` shows this
	 * text; the port is on the line beside it. */
	"web", "serves the console and the volume over HTTP",
	web_start, web_poll, web_stop, &WEB
};

/* --- the log, on the volume ------------------------------------------------
 *
 * The ring in `log.c` is what `GET /api/log` reads and it does not survive a
 * restart. This writes it out in segments, which is the shape ReconFS leaves
 * available -- see `server/logfile.h` for why appending is not.
 */


/*
 * Find where the last run stopped.
 *
 * **The number comes from the directory, not from memory.** `SYS_CREATE`
 * refuses to overwrite, so a server that began again at zero would not clobber
 * the previous run's segments -- it would fail to write anything at all, from
 * the second boot onwards, silently. A log that has stopped looks exactly like
 * a server with nothing to report.
 */
static void log_volume_open(void)
{
	static char names[4096];
	long need;

	LOGVOL.next = 0;
	LOGVOL.flushed = 0;
	LOGVOL.ready = 0;

	mkdir(LOGFILE_DIR, 0755);	/* already there is the ordinary case */

	/*
	 * `SYS_LIST` answers with the size of the whole listing whether or not
	 * it fitted, so a number larger than what was offered means nothing
	 * was written. A directory too big for this buffer is therefore not
	 * read at all rather than read in part -- and a partial listing would
	 * give a highest number that is not the highest, which is the one
	 * mistake that makes `SYS_CREATE` refuse every flush afterwards.
	 */
	need = (long)recon_call6(SYS_LIST, (u64)(unsigned long)LOGFILE_DIR,
	                         sizeof(LOGFILE_DIR) - 1,
	                         (u64)(unsigned long)names, sizeof(names),
	                         0, 0);

	if (need < 0) {
		say("  the log: no volume to keep segments on\n");
		return;
	}
	if ((size_t)need > sizeof(names)) {
		char line[160];

		snprintf(line, sizeof(line),
		         "  the log: %ld bytes of listing and room for %lu --"
		         " not read, so nothing is written\n",
		         need, (unsigned long)sizeof(names));
		say(line);
		return;
	}

	LOGVOL.next = logfile_next(names, (size_t)need);
	LOGVOL.ready = 1;

	/*
	 * Said out loud, because it is the one number that proves the log is
	 * continuing rather than starting again.
	 *
	 * A machine that has been restarted five times and reports segment 0
	 * every time has a log that is not being kept -- and until this line
	 * existed, that state and a working one looked identical from the
	 * console, because everything here is silent on success.
	 */
	{
		char line[120];

		snprintf(line, sizeof(line),
		         "  the log: %s, continuing at segment %06lu\n",
		         LOGFILE_DIR, LOGVOL.next);
		say(line);
	}
}

/*
 * Write everything recorded since the last segment.
 *
 * Called from the web service's poll, which is the only thing that knows a
 * request has been answered. Cheap when there is nothing to do: one comparison.
 */
static void log_volume_flush(int force)
{
	static char body[LOG_ENTRIES_MAX * (LOG_LINE_MAX + 32)];
	char name[LOGFILE_NAME_MAX];
	char path[64];
	char line[200];
	unsigned long missed = 0;
	unsigned long have;
	long n;
	size_t at, i;

	if (!LOGVOL.ready)
		return;

	have = LOGBOOK.written - LOGVOL.flushed;
	if (have == 0)
		return;
	if (!force && have < LOGFILE_FLUSH_EVERY)
		return;

	n = logfile_name(LOGVOL.next, name, sizeof(name));
	if (n < 0) {
		/* A million segments. Refused rather than wrapped: a wrapped
		 * number makes an old segment look new, and the reader with
		 * the problem is a person months from now. */
		say("  the log: the segment numbers are used up\n");
		LOGVOL.ready = 0;
		return;
	}

	at = sizeof(LOGFILE_DIR) - 1;
	for (i = 0; i < at; i++)
		path[i] = LOGFILE_DIR[i];
	path[at++] = '/';
	for (i = 0; name[i]; i++)
		path[at++] = name[i];
	path[at] = '\0';

	n = logfile_render(&LOGBOOK, LOGVOL.flushed, body, sizeof(body),
	                   &missed);
	if (n < 0) {
		LOGVOL.refused++;
		return;
	}

	if (put_if_absent(path, at, body, (unsigned long)n) < 0) {
		/*
		 * Reported rather than retried. `SYS_CREATE` refusing here
		 * means the number was wrong -- the directory held a segment
		 * this did not see -- and writing the next one would leave a
		 * hole nobody could explain.
		 */
		snprintf(line, sizeof(line),
		         "  the log: %s could not be written\n", path);
		say(line);
		LOGVOL.refused++;
		LOGVOL.ready = 0;
		return;
	}

	LOGVOL.flushed = LOGBOOK.written;
	LOGVOL.next++;
	LOGVOL.segments++;

	if (missed) {
		snprintf(line, sizeof(line),
		         "  the log: %s written, and %lu entries were already"
		         " gone from the ring\n", name, missed);
		say(line);
	}
}

/* --- knowing how wrong this machine's clock is ----------------------------
 *
 * The supervisor's second service, and the first thing to prove its shape was
 * worth having: the loop at the bottom of `main` did not change to add it.
 *
 * **It measures and does not correct**, because nothing can set this clock.
 * See `server/ntp.h`. The number is the useful half regardless -- a machine
 * eleven seconds fast that knows it is a different machine from one that does
 * not, since every log line, every TTL and every expiry this system will
 * eventually check is read against that clock.
 */

/* This machine's clock as a 64-bit NTP timestamp.
 *
 * `recon_walltime` is nanoseconds since 1970 and NTP counts from 1900, in
 * 32.32 fixed point. The fraction is scaled *before* it is shifted; the other
 * order throws it away and yields offsets that are always a whole number of
 * seconds and look entirely plausible. */
static unsigned long long clock_ntp(void)
{
	unsigned long long ns = (unsigned long long)recon_walltime();
	unsigned long long secs = ns / 1000000000ULL;
	unsigned long long frac = ns % 1000000000ULL;

	return ((secs + NTP_EPOCH_OFFSET) << 32)
	     | ((frac << 32) / 1000000000ULL);
}

static struct ntp_client CLOCK_CHECK;

/*
 * How often to ask.
 *
 * Deliberately infrequent. A server that queries a public time source on every
 * poll is a server that gets a kiss-of-death, which is the packet that exists
 * precisely for clients like that -- and `ntp.c` refuses to retry through one.
 * Once every five minutes is far more often than a clock drifts and far less
 * often than anybody would mind.
 */
#define CLOCK_CHECK_EVERY_MS (5 * 60 * 1000)

/*
 * The time server, by name.
 *
 * **Resolved rather than written as an address**, which is what a real machine
 * does -- and it is the first thing on this system to use one capability to
 * reach another: the resolver built in 0.18.0 is what makes this reachable.
 *
 * It had to be. QEMU's user networking answers DNS on 10.0.2.3 and answers NTP
 * nowhere, so the first version of this pointed at the gateway and got
 * silence, which is the correct reply to a question nobody is listening for.
 */
/* The name lives beside `WEB_PORT`, with the other things a file can change.
 * See the block above `configure`. */

struct clock_service {
	unsigned long last_ms;
	int asked_once;

	/* Resolved once and kept. A name already looked up does not need
	 * looking up every five minutes, and a resolver asked on a schedule
	 * for an answer nobody is waiting on is a resolver used as a
	 * heartbeat. Cleared when a query fails, so a machine that has moved
	 * network finds the server again. */
	unsigned int address;
};

static struct clock_service CLOCKWORK;

static int clock_start(void *ctx)
{
	struct clock_service *s = (struct clock_service *)ctx;

	s->last_ms = 0;
	s->asked_once = 0;
	s->address = 0;
	return SERVICE_OK;
}

static int clock_poll(void *ctx)
{
	struct clock_service *s = (struct clock_service *)ctx;
	struct ntp_sample sample;
	unsigned long now = clock_ms();
	char line[200];
	int rc;

	if (s->asked_once && now - s->last_ms < CLOCK_CHECK_EVERY_MS)
		return SERVICE_OK;

	s->last_ms = now;
	s->asked_once = 1;

	/* Find the server, if it is not known yet. */
	if (s->address == 0) {
		struct dns_result found;
		unsigned char seed[2];
		unsigned short id;
		long got = (long)recon_call6(SYS_RANDOM,
		                             (u64)(unsigned long)seed,
		                             sizeof(seed), 0, 0, 0, 0);

		if (got < (long)sizeof(seed)) {
			say("  the clock: no randomness for a query id\n");
			return SERVICE_OK;
		}
		id = (unsigned short)((seed[0] << 8) | seed[1]);

		rc = dns_resolve(&RESOLVER, CLOCK_SERVER_NAME, id, &found);
		if (rc != DNS_OK || found.count == 0) {
			snprintf(line, sizeof(line),
			         "  the clock: could not find %s (%d)\n",
			         CLOCK_SERVER_NAME, rc);
			say(line);
			return SERVICE_OK;
		}

		s->address = found.addrs[0];
		CLOCK_CHECK.server = s->address;
		snprintf(line, sizeof(line),
		         "  the clock: asking %u.%u.%u.%u (%s)\n",
		         (s->address >> 24) & 0xFF, (s->address >> 16) & 0xFF,
		         (s->address >> 8) & 0xFF, s->address & 0xFF,
		         CLOCK_SERVER_NAME);
		say(line);
	}

	rc = ntp_query(&CLOCK_CHECK, &sample);
	if (rc != NTP_OK) {
		/*
		 * **Not a service failure.** A time server that does not answer,
		 * or answers something this refuses, is an ordinary condition
		 * and not a reason to restart anything. `service.h` counts a
		 * fault against a restart budget, and a network that is simply
		 * absent would exhaust it in fifteen minutes.
		 */
		snprintf(line, sizeof(line),
		         "  the clock: no usable answer (%d), still %s\n", rc,
		         CLOCK_CHECK.have_last ? "on the last sample"
		                               : "unchecked");
		say(line);
		/* Look the name up again next round: a server that stopped
		 * answering may have moved, and a cached address is the one
		 * thing this would otherwise never reconsider. */
		s->address = 0;
		return SERVICE_OK;
	}

	/*
	 * **The round trip is not reported, and that is a finding rather than
	 * an omission.**
	 *
	 * `SYS_WALLTIME` is declared in nanoseconds and counts whole seconds.
	 * Measured: five reads in a row gave 1789646397000000000, low nine
	 * digits zero every time. So both of this machine's two timestamps in
	 * the exchange land on the same second, the computed round trip is
	 * zero, and printing "round trip 0 ms" to a server on the far side of
	 * the internet would be reporting quantisation as a measurement.
	 *
	 * The offset survives because two of its four terms are the *server's*
	 * timestamps, which are fine-grained -- which is why it reads 1616 ms
	 * rather than a whole number of seconds. What it does not survive is
	 * precision: this machine's two coarse readings put roughly a second
	 * of uncertainty around it, and the line says so rather than implying
	 * millisecond accuracy it cannot have.
	 *
	 * Filed in `docs/SIGNALS.md`. A finer wall clock makes this useful;
	 * nothing else has to change.
	 */
	FACTS.clock_offset_ms = sample.offset_ms;
	FACTS.clock_stratum = sample.stratum;
	FACTS.clock_known = 1;

	snprintf(line, sizeof(line),
	         "  the clock: %s by about %lld ms (+/- a second, this"
	         " machine's clock counts whole ones), stratum %d\n",
	         sample.offset_ms >= 0 ? "behind" : "ahead",
	         sample.offset_ms >= 0 ? sample.offset_ms : -sample.offset_ms,
	         sample.stratum);
	say(line);
	return SERVICE_OK;
}

static const struct service CLOCK_SERVICE = {
	"clock", "measures how far this machine's time is from a time server",
	clock_start, clock_poll, 0, &CLOCKWORK
};

/* --- measuring the client side, which nothing has ever exercised ----------- */

/*
 * What `connect` actually does on this kernel.
 *
 * Everything this role has proved so far is the *server* side: bind, listen,
 * accept, and bytes over a connection somebody else opened. Discovery needs
 * the other half -- opening a connection to somebody else -- and reading
 * `socket_connect` suggests it does not mean what a caller would assume: it
 * calls `tcp_open` and returns, without waiting for the handshake.
 *
 * Reading is not measuring, so this measures. Three cases, and the numbers go
 * to the serial console where they can be compared against what the code
 * appeared to say:
 *
 *   a port nothing is listening on   -- does `connect` report success anyway?
 *   this machine's own web server    -- does a connection that should work?
 *   a write straight after connect   -- is the connection usable yet?
 *
 * It runs once at boot and costs two sockets. It is here rather than in a suite
 * because a suite on the host measures the *host's* sockets, which answer these
 * questions differently and correctly -- which is exactly why reading the code
 * and running a host suite both failed to catch this.
 *
 * --- What it found, 16 September 2026 ---
 *
 *     the client side: connect(closed port)=0 connect(own :80)=0
 *                      write=-1 read=0
 *
 * `connect` answers `SYS_OK` for a port nothing is listening on, and the
 * connection that should have worked was not usable when it returned.
 * `socket_connect` calls `tcp_open`, which starts a handshake and does not
 * finish one. Filed at the top of `docs/KERNEL-WANTS.md`.
 *
 * --- Why it stays in, now that the answer is known ---
 *
 * **It is a standing check on a gap somebody else will close.** When
 * `SYS_CONNECT` learns to say `EAGAIN` while a handshake is in flight, the
 * first number on this line changes, and whoever boots this next finds out
 * without going looking. A limitation recorded only in a document is a
 * limitation that stays recorded after it stops being true -- which this role
 * has now done twice, as VF-004 and VF-009.
 */
static void measure_the_client_side(void)
{
	char line[220];
	char buf[64];

	/*
	 * --- What changed, and why this is now a poll rather than one call ---
	 *
	 * Under kernel 0.2.41 this called `connect` once and printed what came
	 * back, because one call was all the answer there was: `connect`
	 * reported `SYS_OK` for a port nothing was listening on, and the line
	 * read
	 *
	 *     connect(closed port)=0 connect(own :80)=0 write=-1 read=0
	 *
	 * which is VF-009 -- a call that says yes to everything cannot find
	 * anything.
	 *
	 * KF-244 fixed it in 0.2.48 and the same single call then answered -4,
	 * `SYS_EAGAIN`, for **both** ports. That is correct and is not an
	 * answer: in flight is not a verdict, it is a question asked again.
	 * Telling a refusal from a connection now requires polling until one or
	 * the other, which is exactly what `server/dial.c` was written for
	 * three versions ago and has never until now been able to exercise.
	 *
	 * So the measurement is the thing that was built for it. `dial.c`
	 * compares no numbers at all -- it reads `errno` by name -- which is
	 * why the announcement's three wrong constants cost nothing.
	 */
	{
		struct dial closed, open;
		int v_closed, v_open;
		unsigned long now = clock_ms();
		long wrote = 0, red = 0;

		/* 10.0.2.2 is the gateway QEMU's user networking provides;
		 * port 9 is discard and nothing here serves it. */
		v_closed = dial_begin(&closed, 0x0A000202u, 9, now, now + 2000);
		while (v_closed == DIAL_PENDING) {
			recon_yield();
			v_closed = dial_poll(&closed, clock_ms());
		}
		dial_close(&closed);

		/* This machine's own listener, which is certainly there. */
		now = clock_ms();
		v_open = dial_begin(&open, 0x0A00020Fu, 80, now, now + 2000);
		while (v_open == DIAL_PENDING) {
			recon_yield();
			v_open = dial_poll(&open, clock_ms());
		}

		if (v_open == DIAL_READY) {
			int fd = dial_take(&open);

			if (fd >= 0) {
				wrote = recon_write(fd,
				                    "GET /health HTTP/1.0\r\n\r\n",
				                    24);
				/* Nothing blocks here: ask again until the
				 * answer arrives or the tries run out. */
				{
					int t;

					for (t = 0; t < 200000 && red <= 0; t++) {
						red = recon_read(fd, buf,
						                 sizeof(buf));
						if (red <= 0)
							recon_yield();
					}
				}
				recon_close(fd);
			}
		}
		dial_close(&open);

		snprintf(line, sizeof(line),
		         "  the client side: closed port=%s own :80=%s"
		         " write=%ld read=%ld\n",
		         dial_says(v_closed), dial_says(v_open),
		         (long)wrote, (long)red);
		say(line);
	}
}

/* --- the volume this role writes to --------------------------------------- */

/*
 * Write one file, unless it is already there.
 *
 * Returns 1 when something was already at that name, 2 when this wrote it, and
 * a negative number when it could not.
 *
 * **An existing file is never overwritten.** A server that rewrote its own
 * pages on every boot would silently discard whatever an administrator had put
 * there, which is the kind of helpfulness that loses somebody's work.
 *
 * Created with `SYS_CREATE` rather than `open(O_CREAT)`, and that is a
 * workaround for a fault in the C library rather than a preference.
 * **`recon_flags_from_posix` in `userland/libc/posix.c` translates only the
 * access mode**: `O_CREAT`, `O_TRUNC`, `O_EXCL` and `O_APPEND` are dropped, so
 * `open(path, O_WRONLY | O_CREAT, 0644)` never creates anything -- it opens an
 * existing file for writing, and for a file that is not there it answers
 * `ENOENT`, which is exactly the condition `O_CREAT` was passed to fix.
 * Measured on the machine as `write=-1/e2` for a path whose directory had just
 * answered `EEXIST`. Reported in `docs/SERVER.md`; the number is the desktop
 * session's, since `posix.c` is theirs.
 *
 * The kernel has always had the call, and `SYS_CREATE` taking the path and the
 * contents together suits a file written once at boot better than
 * open-then-write would anyway.
 */
static int put_if_absent(const char *path, unsigned long path_len,
                         const char *data, unsigned long len)
{
	long rc;
	int fd = open(path, O_RDONLY);

	if (fd >= 0) {
		close(fd);
		return 1;
	}

	rc = (long)recon_call6(SYS_CREATE, (u64)(unsigned long)path,
	                       (u64)path_len, 0644,
	                       (u64)(unsigned long)data, (u64)len, 0);
	return rc < 0 ? -1 : 2;
}

/*
 * Make the web root, and put something in it if it is empty.
 *
 * Runs every boot and is deliberately safe to: a directory that is already
 * there is not a failure, and a file that is already there is **not
 * overwritten**. A server that rewrote its own index on every boot would
 * silently discard whatever an administrator had put there, which is the kind
 * of helpfulness that loses somebody's work.
 *
 * Returns what it found, for the line on the screen -- 0 if the root could not
 * be made at all, 1 if it exists, 2 if a default page was written into it.
 */
/* Filled in by `lay_out_the_site` so the serial line can report the numbers
 * the decision was made from, rather than the conclusion drawn from them. Two
 * calls disagreed about whether a file existed and the conclusion alone could
 * not say which. */
static long SITE_OPEN_READ, SITE_OPEN_WRITE;
static long SITE_WROTE = -1;
/* A -1 says a call failed and nothing more. Which failure it was decides
 * whether this is a volume that cannot be written or a directory that was
 * already there, and those want opposite responses. */
static long E_OPEN_READ, E_OPEN_WRITE;

static int lay_out_the_site(void)
{
	static const char DEFAULT_PAGE[] =
		"<!doctype html>\n<meta charset=\"utf-8\">\n"
		"<title>ReconOS</title>\n"
		"<style>body{background:#141821;color:#d8dce6;"
		"font:15px/1.6 system-ui,sans-serif;margin:0;padding:40px}"
		"main{max-width:560px;margin:0 auto}"
		"h1{color:#4fb3a5;font-size:24px;margin:0 0 6px}"
		"p{color:#737c90}a{color:#4fb3a5}</style>\n"
		"<main>\n<h1>It works.</h1>\n"
		"<p>This file is on the volume, at <code>" WEB_ROOT
		"/index.html</code>, and was read off the disk to answer your "
		"request. Replace it with your own.</p>\n"
		"<p><a href=\"/\">The server dashboard</a> is served by a "
		"handler rather than from a file.</p>\n</main>\n";

	/*
	 * Make the directories, and **do not read anything into whether they
	 * were made or already there.**
	 *
	 * This function got that wrong and the fault is worth keeping written
	 * down, because it is the one this project keeps naming. `mkdir`
	 * answers -1 with `EEXIST` for a directory that is already present,
	 * which is the ordinary case on every boot after the first. The code
	 * here read that -1 as "the web root could not be made", reported
	 * exactly that on the serial console, and returned before creating the
	 * page -- so a server with a perfectly good web root spent three boots
	 * saying it had none, and every file request answered 404.
	 *
	 * The numbers said so as soon as they were printed: `errno=17/17`
	 * against a raw `SYS_EEXIST` of -6 for both paths, while the file
	 * itself answered `errno=2`. A directory that exists and a file that
	 * does not is not a failure to report; it is the state this function
	 * exists to fix.
	 *
	 * So the directories are made and their answers discarded. **The only
	 * question that decides anything is whether the file can be opened**,
	 * which is the thing actually needed, asked directly.
	 */
	mkdir("/System", 0755);
	mkdir(WEB_ROOT, 0755);

	/* Same rule, and the same discarded answer. An upload has nowhere to
	 * land until this exists, and it existing already is the ordinary
	 * case. */
	mkdir(UPLOAD_ROOT, 0755);

	/*
	 * Where the access log's segments go, and where the last run stopped.
	 *
	 * Read here rather than at the first flush so that a machine with no
	 * volume says so once at boot, rather than silently keeping nothing
	 * for as long as it runs.
	 */
	log_volume_open();

	/*
	 * Each file decided on its own, which this function did not used to do.
	 *
	 * It returned as soon as `index.html` was found, on the reasoning that
	 * the site was already laid out. That was fine while there was one
	 * file. The moment a second arrived, **every machine that already had a
	 * page would never get the stylesheet** -- and the page would render
	 * unstyled with nothing to say why. The early return was correct and
	 * became a bug by addition, which is the kind worth naming.
	 *
	 * `put_if_absent` therefore answers per file, and the summary below is
	 * built from all of them rather than from the first.
	 */
	{
		int page = put_if_absent(WEB_ROOT "/index.html",
		                         sizeof(WEB_ROOT "/index.html") - 1,
		                         DEFAULT_PAGE,
		                         sizeof(DEFAULT_PAGE) - 1);
		int css = put_if_absent(WEB_ROOT "/console.css",
		                        sizeof(WEB_ROOT "/console.css") - 1,
		                        CONSOLE_CSS,
		                        sizeof(CONSOLE_CSS) - 1);

		if (page < 0 || css < 0)
			return 0;	/* nothing to serve; say so */
		return (page == 2 || css == 2) ? 2 : 1;
	}
}

/* --- the screen ----------------------------------------------------------- */

/*
 * Redraw with the current counts.
 *
 * Called after each connection so the screen in front of the machine is the
 * same truth as the page being served from it. Cheap enough at this scale, and
 * the alternative -- a screen drawn once at boot -- is a screen that is wrong
 * from the first request onward.
 */
static void draw(const struct recon_canvas *canvas,
                 struct recon_first_boot *facts, char *served_line,
                 size_t room)
{
	/* The port comes from `WEB_PORT` rather than being written here. A
	 * screen that says 80 on a machine listening on 8080 is the same class
	 * of fault as a document that says 736 checks -- a second copy of a
	 * fact, drifting quietly. */
	snprintf(served_line, room,
	         "The web server is listening on port %u. %lu served,"
	         " %lu bytes out.",
	         WEB_PORT, FACTS.served, FACTS.bytes_out);
	recon_screen_draw(canvas, facts);
}

int main(void)
{
	struct recon_screen screen;
	struct recon_canvas canvas;
	struct recon_first_boot facts;
	char kernel_line[96];
	char processors_line[64];
	char memory_line[96];
	char display_line[96];
	char served_line[140];
	char address_line[120];
	char line[200];
	i64 answer, fd, mapped;
	int listener;
	unsigned started;

	/* --- the screen, exactly as the workstation does it ------------------ */

	answer = recon_screen(&screen, sizeof(screen));
	if (answer < 0)
		return NO_SCREEN_DESCRIBED;
	if ((u64)answer != sizeof(screen))
		return SCREEN_STRUCT_WRONG;
	if (screen.width == 0 || screen.height == 0 ||
	    screen.pitch < screen.width * 4u)
		return SCREEN_MAKES_NO_SENSE;

	fd = recon_open("/dev/fb0", 8, OPEN_WRITE | OPEN_READ, 0);
	if (fd < 0)
		return FRAMEBUFFER_CLOSED;

	mapped = recon_map((int)fd, screen.bytes);
	if (mapped < 0)
		return MAPPING_REFUSED;

	/* --- the machine ------------------------------------------------------ */

	memset(&FACTS, 0, sizeof(FACTS));
	FACTS.machine.size = (u32)sizeof(FACTS.machine);
	answer = recon_machine_facts(&FACTS.machine, sizeof(FACTS.machine));
	if (answer < 0)
		return MACHINE_UNREADABLE;

	/* This machine's name.
	 *
	 * Hard-coded for now, and deliberately a name that *belongs to a
	 * family* so that the parallel naming in `server/identity.c` has
	 * something real to work from the moment discovery exists. When a peer
	 * can be found on the wire, this is the line that changes: the name
	 * comes from `server_name_next_parallel` instead of from here. */
	snprintf(FACTS.name, sizeof(FACTS.name), "M16");

	snprintf(kernel_line, sizeof(kernel_line), "kernel on %s, %u KiB pages",
	         FACTS.machine.architecture[0] ? FACTS.machine.architecture
	                                       : "an unnamed architecture",
	         FACTS.machine.page_size / 1024u);
	snprintf(processors_line, sizeof(processors_line), "%u found, %u in use",
	         FACTS.machine.processors_found, FACTS.machine.processors_online);
	snprintf(memory_line, sizeof(memory_line), "%llu MiB, %llu MiB free",
	         (unsigned long long)(FACTS.machine.memory_bytes >> 20),
	         (unsigned long long)(FACTS.machine.memory_free_bytes >> 20));
	snprintf(display_line, sizeof(display_line), "%u x %u, %u bytes a row",
	         screen.width, screen.height, screen.pitch);

	/* --- the listener, before anything is drawn about it ------------------ */

	{
		int site = lay_out_the_site();

		/* The numbers, before the conclusion drawn from them.
		 *
		 * The conclusion alone said "already there" on a boot where
		 * the file could not then be read, and one of those two claims
		 * had to be wrong. A line that reports what each call actually
		 * answered says which. */
		snprintf(line, sizeof(line),
		         "  the web root: read=%ld/e%ld write=%ld/e%ld"
		         " wrote=%ld\n",
		         SITE_OPEN_READ, E_OPEN_READ,
		         SITE_OPEN_WRITE, E_OPEN_WRITE, SITE_WROTE);
		say(line);

		snprintf(line, sizeof(line),
		         "  the web root: %s\n",
		         site == 0 ? WEB_ROOT " could not be made -- files will 404"
		                   : site == 2 ? WEB_ROOT ", and a default page written"
		                               : WEB_ROOT ", already there");
		say(line);
	}


	RESOLVER.server = 0x0A000203u;	/* 10.0.2.3 -- see the declaration */
	RESOLVER.port = 53;
	RESOLVER.idle = recon_yield;
	RESOLVER.now_ms = clock_ms;
	RESOLVER.timeout_ms = 3000;

	/*
	 * --- what the file says ----------------------------------------------
	 *
	 * After the defaults above, because it replaces them, and before the
	 * services below, because they read what it left. Everything it can
	 * change has already been set to something this machine can run on, so
	 * a refused file leaves a working server rather than an unconfigured
	 * one. See `config.h` on why that is the one place here that does not
	 * refuse outright.
	 */
	configure();

	/*
	 * --- the secret, made once ------------------------------------------
	 *
	 * Printed on the serial console, because that is the whole of what it
	 * asserts: whoever can read this machine's console may write to it.
	 *
	 * **A failure to get randomness leaves the guard closed, not open.** If
	 * `SYS_RANDOM` gives fewer bytes than asked for, `auth_arm` refuses and
	 * every guarded route answers 401 until the machine is restarted. That
	 * is the right way round: a server that cannot make a secret cannot
	 * keep one, and the alternative is a machine that reports itself as
	 * guarded while accepting anything.
	 */
	{
		unsigned char seed[AUTH_TOKEN_BYTES];
		long got = (long)recon_call6(SYS_RANDOM,
		                             (u64)(unsigned long)seed,
		                             sizeof(seed), 0, 0, 0, 0);

		if (got < 0 || !auth_arm(&GUARD, seed, (size_t)got)) {
			snprintf(line, sizeof(line),
			         "  the guard: NO SECRET (SYS_RANDOM gave %ld)"
			         " -- every write is refused until reboot\n",
			         got);
			say(line);
		} else {
			snprintf(line, sizeof(line),
			         "  the guard: writes need"
			         " `Authorization: Bearer %s`\n", GUARD.token);
			say(line);
			say("  the guard: reads are open;"
			    " this token is new on every boot\n");
		}
	}

	/* --- the services ------------------------------------------------------ */

	WEB.listener = -1;
	WEB.site = CHAIN;

	/* The second service. The supervisor was built for more than one and
	 * has never held more than one; adding this changed nothing in the
	 * loop at the bottom of `main`, which is what that shape was for. */
	CLOCK_CHECK.server = 0;		/* resolved by name on the first poll */
	CLOCK_CHECK.port = 123;
	CLOCK_CHECK.idle = recon_yield;
	CLOCK_CHECK.now_ms = clock_ms;
	CLOCK_CHECK.now_ntp = clock_ntp;
	CLOCK_CHECK.timeout_ms = 3000;

	if (supervisor_add(&SUPERVISOR, &CLOCK_SERVICE) != SERVICE_OK)
		say("  the clock: could not be registered\n");

	if (supervisor_add(&SUPERVISOR, &WEB_SERVICE) != SERVICE_OK) {
		/* The registry refused the one service this role has. Nothing
		 * below can work, and saying which is more use than a blank
		 * screen. */
		say("  the supervisor: would not register the web server\n");
		return NO_LISTENER;
	}

	started = supervisor_start(&SUPERVISOR, (unsigned long long)recon_time());

	/* After the listener exists, so the second case has something to reach. */
	measure_the_client_side();
	listener = WEB.listener;

	if (started == 0) {
		/* Said plainly rather than drawn over. A server role whose
		 * server did not start is the one fact worth stopping for. */
		snprintf(line, sizeof(line),
		         "  the web server: could not listen on :%u\n",
		         WEB_PORT);
		say(line);
		snprintf(address_line, sizeof(address_line),
		         "The web server could not open port %u.", WEB_PORT);
	} else {
		snprintf(address_line, sizeof(address_line),
		         "Serving on port %u. / and /api/status, and files from "
		         WEB_ROOT ".", WEB_PORT);
	}

	/* --- the first screen -------------------------------------------------- */

	memset(&facts, 0, sizeof(facts));
	facts.version = "ReconOS -- server";
	facts.kernel = kernel_line;
	facts.processors = processors_line;
	facts.memory = memory_line;
	facts.cpu = FACTS.machine.cpu_model[0] ? FACTS.machine.cpu_model
	                                       : FACTS.machine.cpu_vendor;
	facts.display = display_line;
	facts.storage = "not yet read on this role";

	facts.notes[0] = address_line;
	facts.notes[1] = served_line;
	facts.notes[2] = "This machine is running its own kernel.";
	facts.notes[3] = "No Linux is underneath it.";
	facts.notes[4] = "";
	facts.notes[5] = "DNS and DHCP wait on a datagram system call.";

	canvas.pixels = (unsigned char *)(unsigned long)mapped;
	canvas.width = screen.width;
	canvas.height = screen.height;
	canvas.pitch = screen.pitch;

	draw(&canvas, &facts, served_line, sizeof(served_line));

	snprintf(line, sizeof(line),
	         "  the web server: listening on :%u -- %lu requests served,"
	         " %lu bytes out\n", WEB_PORT, FACTS.served, FACTS.bytes_out);
	say(line);

	if (listener < 0)
		return NO_LISTENER;

	/* --- serve ------------------------------------------------------------- */

	/*
	 * The loop, and why it yields.
	 *
	 * `accept` answers `EAGAIN` with nobody waiting -- there is no wait
	 * queue on a listener -- so this polls. `recon_yield` rather than a
	 * bare spin, for the reason the workstation's idle loop gives: a
	 * processor held at a hundred percent to wait for a connection is a
	 * fan running and an hour less battery.
	 *
	 * When the kernel can report readiness on more than a listener, this
	 * is the loop that changes, and `docs/WEB.md` carries the row.
	 */
	/*
	 * The supervisor's round, rather than a loop that serves.
	 *
	 * The web server is one service among what will be several, and this
	 * loop knows nothing about it beyond that: `supervisor_poll` asks each
	 * registered service to do a little work and records what it said. When
	 * discovery exists it registers itself here and nothing in this loop
	 * changes; when DNS and DHCP become possible, the same.
	 *
	 * **It yields when nothing happened, and not otherwise.** A round that
	 * served a request is followed immediately by another, because a client
	 * with a second request is waiting now. A round that served nothing
	 * gives the processor back, for the reason the workstation's idle loop
	 * gives: a processor held at a hundred per cent to wait for a
	 * connection is a fan running and an hour less battery.
	 */
	for (;;) {
		unsigned long before = FACTS.served;
		unsigned running = 0;

		supervisor_poll(&SUPERVISOR, (unsigned long long)recon_time());

		supervisor_tally(&SUPERVISOR, &running, 0, 0);
		if (running == 0) {
			/* Everything registered has failed or refused. There
			 * is nothing left for this round to do, and saying so
			 * once is more use than spinning silently. */
			say("  the supervisor: nothing is running\n");
			draw(&canvas, &facts, served_line, sizeof(served_line));
			return NO_LISTENER;
		}

		if (FACTS.served == before) {
			recon_yield();
			continue;
		}

		/* Reported on every request, because this line is the
		 * measurement the whole exercise exists for. */
		snprintf(line, sizeof(line),
		         "  the web server: %lu served\n", FACTS.served);
		say(line);

		draw(&canvas, &facts, served_line, sizeof(served_line));
	}

	return WENT_WELL;
}
