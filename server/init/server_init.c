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
#include "../http/multipart.h"
#include "../include/recon_server.h"
#include "../service.h"
#include "../log.h"

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
};

static struct server_facts FACTS;

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
 * In memory and not on the volume, because appending to a file needs
 * `O_APPEND` and the C library drops it -- the same fault that stopped
 * `O_CREAT` working. So this does not survive a reboot, which is a limitation
 * rather than a design and is written down in `server/log.h` where a reader
 * looking for yesterday's requests will find it.
 */
static struct logbook LOGBOOK;

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
	struct server_facts *f = (struct server_facts *)ctx;
	int n;

	(void)r; (void)body; (void)body_len;

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
	             "</table>\n"
	             "<form class=\"rename\" method=\"post\" "
	             "action=\"/api/name\">\n"
	             "<label>rename this machine "
	             "<input name=\"name\" value=\"%s\"></label>\n"
	             "<button>Set</button>\n"
	             "</form>\n"
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
	             "\"bytes_sent\":%lu}\n",
	             name, arch,
	             f->machine.processors_found, f->machine.processors_online,
	             (unsigned long long)f->machine.memory_bytes,
	             (unsigned long long)f->machine.memory_free_bytes,
	             f->machine.page_size,
	             f->served, f->bytes_out);

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
static int handle_set_name(const struct http_request *r, const char *body,
                           size_t body_len, struct http_response *out,
                           void *ctx)
{
	static char answer[256];
	struct server_facts *f = (struct server_facts *)ctx;
	struct http_form form;
	struct name_family family;
	char name_room[JSON_ROOM(RECON_NAME_MAX)];
	const char *wanted, *name;
	int rc, n;

	(void)r;

	rc = http_form_parse(body, body_len, &form);
	if (rc != HTTP_OK)
		return rc;

	wanted = http_form_get(&form, "name");
	if (!wanted) {
		/* Absent, or given twice. `http_form_get` deliberately answers
		 * the same for both -- see `form.h` -- and either way there is
		 * no single name to take. */
		http_response_simple(out, 400, "text/plain",
		                     "400 Bad Request: one name field\n", 32);
		return HTTP_OK;
	}

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
static int handle_log(const struct http_request *r, const char *body,
                      size_t body_len, struct http_response *out, void *ctx)
{
	static char text[LOG_ENTRIES_MAX * (LOG_LINE_MAX + 32) + 128];
	struct logbook *book = (struct logbook *)ctx;
	size_t held = log_held(book), i;
	int n, m;

	(void)r; (void)body; (void)body_len;

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

static const struct http_route ROUTES[] = {
	{ "GET",  "/",            1, handle_dashboard, 0, &FACTS },
	{ "GET",  "/api/status",  1, handle_status,    0, &FACTS },
	{ "GET",  "/health",      1, handle_health,    0, 0 },
	{ "GET",  "/api/services", 1, handle_services,  0, &SUPERVISOR },
	{ "GET",  "/api/log",     1, handle_log,       0, &LOGBOOK },
	{ "POST", "/api/name",    1, handle_set_name,  0, &FACTS },
	{ "POST", "/api/upload",  1, handle_upload,    0, 0 },

	/* Last, and streaming. A file no longer has to fit in a response, so
	 * the volume can serve something larger than this program's memory. */
	{ "GET", "",              0, 0, http_files_handler,
	  (void *)&SITE_FILES },
};

/* --- the web server, as a service ------------------------------------------ */

struct web_service {
	int listener;
	const struct http_site *site;
};

static struct web_service WEB;

static int web_start(void *ctx)
{
	struct web_service *w = (struct web_service *)ctx;

	w->listener = http_listen(80);
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
	if (rc > 0)
		FACTS.served++;
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
	"web", "serves the console and the volume on port 80",
	web_start, web_poll, web_stop, &WEB
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
	i64 fd, rc_closed = 0, rc_open = 0, wrote = 0, red = 0;
	char buf[64];

	/* 10.0.2.2 is the gateway QEMU's user networking provides; port 9 is
	 * discard and nothing here serves it. A connect that reports success
	 * to a closed port is a connect that cannot be used to find anything. */
	fd = recon_socket(1);
	if (fd >= 0) {
		rc_closed = recon_connect((int)fd, 0x0A000202u, 9);
		recon_close((int)fd);
	} else {
		rc_closed = -999;
	}

	/* This machine's own listener, which is certainly there. */
	fd = recon_socket(1);
	if (fd >= 0) {
		rc_open = recon_connect((int)fd, 0x0A00020Fu, 80);
		if (rc_open == 0) {
			wrote = recon_write((int)fd,
			                    "GET /health HTTP/1.0\r\n\r\n", 24);
			red = recon_read((int)fd, buf, sizeof(buf));
		}
		recon_close((int)fd);
	} else {
		rc_open = -999;
	}

	snprintf(line, sizeof(line),
	         "  the client side: connect(closed port)=%ld"
	         " connect(own :80)=%ld write=%ld read=%ld\n",
	         (long)rc_closed, (long)rc_open, (long)wrote, (long)red);
	say(line);
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
	snprintf(served_line, room,
	         "The web server is listening on port 80. %lu served, %lu bytes out.",
	         FACTS.served, FACTS.bytes_out);
	recon_screen_draw(canvas, facts);
}

int main(void)
{
	struct recon_screen screen;
	struct recon_canvas canvas;
	struct recon_first_boot facts;
	struct http_site site;
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

	site.routes = ROUTES;
	site.route_count = sizeof(ROUTES) / sizeof(ROUTES[0]);
	site.ctx = &FACTS;
	site.server_name = "ReconOS";
	site.bytes_sent = &FACTS.bytes_out;
	site.log = note_request;
	site.log_ctx = &LOGBOOK;

	/*
	 * What this system does while waiting for bytes that have not arrived.
	 *
	 * Without it, every request over about 2880 bytes stalled and was never
	 * answered -- not because of a size limit, but because a single process
	 * asking for bytes in a tight loop leaves nothing running that could
	 * deliver them. See `serve.h` and VF-013.
	 */
	site.idle = recon_yield;
	site.now_ms = clock_ms;

	/* --- the services ------------------------------------------------------ */

	WEB.listener = -1;
	WEB.site = &site;

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
		say("  the web server: could not listen on :80\n");
		snprintf(address_line, sizeof(address_line),
		         "The web server could not open port 80.");
	} else {
		snprintf(address_line, sizeof(address_line),
		         "Serving on port 80. / and /api/status, and files from "
		         WEB_ROOT ".");
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
	         "  the web server: listening on :80 -- %lu requests served,"
	         " %lu bytes out\n", FACTS.served, FACTS.bytes_out);
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
