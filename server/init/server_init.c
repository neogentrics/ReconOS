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
#include "../include/recon_server.h"

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

static void say(const char *text)
{
	recon_write(1, text, recon_strlen(text));
}

/* --- what this server answers with --------------------------------------- */

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

static const char PAGE_HEAD[] =
	"<!doctype html>\n<meta charset=\"utf-8\">\n"
	"<title>ReconOS Server</title>\n"
	"<style>"
	"body{background:#141821;color:#d8dce6;font:15px/1.6 system-ui,sans-serif;"
	"margin:0;padding:40px}"
	"main{max-width:640px;margin:0 auto}"
	"h1{color:#4fb3a5;font-size:26px;margin:0 0 4px}"
	"p.sub{color:#737c90;margin:0 0 28px}"
	"table{border-collapse:collapse;width:100%}"
	"td{padding:8px 0;border-bottom:1px solid #2a3145}"
	"td:first-child{color:#737c90;width:40%}"
	"code{color:#4fb3a5}"
	"</style>\n<main>\n";

/* The dashboard. One page, real numbers, nothing invented.
 *
 * This is the console the docx calls the Server Manager dashboard, at the size
 * it can honestly be today: what this machine is, and what this server has
 * done. Every row is read from the kernel or counted here. */
static int handle_dashboard(const struct http_request *r, const char *body,
                            size_t body_len, struct http_response *out,
                            void *ctx)
{
	static char page[HTTP_RESPONSE_MAX];
	struct server_facts *f = (struct server_facts *)ctx;
	int n;

	(void)r; (void)body; (void)body_len;

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
	             "<p class=\"sub\" style=\"margin-top:28px\">"
	             "Served by <code>server/http/</code> over the kernel's five "
	             "socket calls. This page is the first thing to move bytes "
	             "over an accepted connection on this system.</p>\n"
	             "</main>\n",
	             PAGE_HEAD,
	             f->name,
	             f->machine.architecture[0] ? f->machine.architecture
	                                        : "unnamed",
	             f->machine.cpu_model[0] ? f->machine.cpu_model
	                                     : f->machine.cpu_vendor,
	             f->machine.processors_found, f->machine.processors_online,
	             (unsigned long long)(f->machine.memory_bytes >> 20),
	             (unsigned long long)(f->machine.memory_free_bytes >> 20),
	             f->machine.page_size / 1024u,
	             f->served, f->bytes_out);

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
	int n;

	(void)r; (void)body; (void)body_len;

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
	             f->name,
	             f->machine.architecture[0] ? f->machine.architecture
	                                        : "unnamed",
	             f->machine.processors_found, f->machine.processors_online,
	             (unsigned long long)f->machine.memory_bytes,
	             (unsigned long long)f->machine.memory_free_bytes,
	             f->machine.page_size,
	             f->served, f->bytes_out);

	if (n < 0 || (size_t)n >= sizeof(json))
		return HTTP_EBODY_LONG;

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
static const struct http_route ROUTES[] = {
	{ "GET", "/",             1, handle_dashboard,   &FACTS },
	{ "GET", "/api/status",   1, handle_status,      &FACTS },
	{ "GET", "/health",       1, handle_health,      0 },
	{ "GET", "",              0, http_files_handler,
	  (void *)&SITE_FILES },
};

/* --- the volume this role writes to --------------------------------------- */

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

	int fd;

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

	errno = 0;
	fd = open(WEB_ROOT "/index.html", O_RDONLY);
	SITE_OPEN_READ = fd;
	E_OPEN_READ = errno;
	if (fd >= 0) {
		close(fd);
		return 1;		/* somebody's page is already there */
	}

	/*
	 * Created with `SYS_CREATE` rather than `open(O_CREAT)`, and that is a
	 * workaround for a fault in the C library rather than a preference.
	 *
	 * **`recon_flags_from_posix` in `userland/libc/posix.c` translates only
	 * the access mode.** `O_CREAT`, `O_TRUNC`, `O_EXCL` and `O_APPEND` are
	 * dropped on the floor, so `open(path, O_WRONLY | O_CREAT, 0644)` never
	 * creates anything -- it opens an existing file for writing, and for a
	 * file that is not there it answers `ENOENT`. Which is exactly the
	 * condition the caller passed `O_CREAT` to fix.
	 *
	 * Measured on the machine: `write=-1/e2` for a path whose directory had
	 * just answered `EEXIST`. Reported in `docs/SERVER.md`; the number is
	 * the desktop session's to assign, since `posix.c` is theirs.
	 *
	 * The kernel has always had the call -- `SYS_CREATE` takes the path and
	 * the contents together, which suits a file written once at boot better
	 * than open-then-write would anyway.
	 */
	SITE_OPEN_WRITE = (long)recon_call6(SYS_CREATE,
	                                    (u64)(unsigned long)(WEB_ROOT "/index.html"),
	                                    sizeof(WEB_ROOT "/index.html") - 1,
	                                    0644,
	                                    (u64)(unsigned long)DEFAULT_PAGE,
	                                    sizeof(DEFAULT_PAGE) - 1, 0);
	E_OPEN_WRITE = 0;
	SITE_WROTE = SITE_OPEN_WRITE;

	/* Anything negative is a refusal, and a refusal here means the role has
	 * nothing to serve. Said rather than drawn over. */
	return SITE_OPEN_WRITE < 0 ? 0 : 2;
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

	listener = http_listen(80);
	if (listener < 0) {
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

	site.routes = ROUTES;
	site.route_count = sizeof(ROUTES) / sizeof(ROUTES[0]);
	site.ctx = &FACTS;
	site.server_name = "ReconOS";
	site.bytes_sent = &FACTS.bytes_out;

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
	for (;;) {
		int rc = http_serve_once(listener, &site);

		if (rc < 0) {
			say("  the web server: the listener failed\n");
			return NO_LISTENER;
		}
		if (rc == 0) {
			recon_yield();
			continue;
		}

		FACTS.served++;

		/* Reported on every request, because this line is the
		 * measurement the whole exercise exists for. */
		snprintf(line, sizeof(line),
		         "  the web server: %lu served\n", FACTS.served);
		say(line);

		draw(&canvas, &facts, served_line, sizeof(served_line));
	}

	return WENT_WELL;
}
